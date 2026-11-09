#include "tempo.h"
#include "third_party/minibpm/MiniBpm.h"

#include <cmath>
#include <stdexcept>

namespace ap {
namespace {
constexpr uint32_t analysisRate = 22050;
constexpr double windowSeconds = 8;

// Box-filter before decimation; choose a channel per block to retain antiphase stereo.
void appendMono (const float* input, size_t frames, uint32_t channels, double rate,
                 std::vector<float>& output, double& phase, double& sum, double& weight)
{
    size_t channel = 0;
    double best = -1;
    for (uint32_t c = 0; c < channels; ++c)
    {
        double energy = 0;
        for (size_t i = 0; i < frames; i += 32)
        {
            const double x = input[i * channels + c];
            if (std::isfinite (x)) energy += x * x;
        }
        if (energy > best) { best = energy; channel = c; }
    }
    const double step = analysisRate / rate;
    for (size_t i = 0; i < frames; ++i)
    {
        const float x = input[i * channels + channel];
        double remaining = step;
        while (remaining > 1e-10)
        {
            const double portion = std::min (remaining, 1 - phase);
            sum += (std::isfinite (x) ? x : 0) * portion;
            weight += portion;
            phase += portion;
            remaining -= portion;
            if (phase >= 1 - 1e-10)
            {
                output.push_back (static_cast<float> (sum / weight));
                phase = sum = weight = 0;
            }
        }
    }
}
}

double TempoResult::bpmAt (double seconds) const
{
    const auto it = std::upper_bound (segments.begin (), segments.end (), seconds,
        [] (double t, const TempoSegment& s) { return t < s.start; });
    if (it == segments.begin ()) return 0;
    const auto& s = *std::prev (it);
    return seconds < s.end ? s.bpm : 0;
}

double estimateSegmentTempo (const float* mono, size_t frames, uint32_t rate)
{
    if (!mono || rate < 8000 || frames < rate * 4ull) return 0;
    double energy = 0, minEnergy = 1, maxEnergy = 0;
    std::vector<double> envelope;
    // Reject silence and unmodulated tones before asking the periodicity estimator.
    for (size_t start = 0; start < frames; start += rate / 50)
    {
        double e = 0;
        const size_t end = std::min (frames, start + rate / 50);
        for (size_t i = start; i < end; ++i)
            if (std::isfinite (mono[i])) e += mono[i] * mono[i];
        energy += e;
        e /= end - start;
        envelope.push_back (e);
        minEnergy = std::min (minEnergy, e);
        maxEnergy = std::max (maxEnergy, e);
    }
    if (energy / frames < 1e-8 || maxEnergy - minEnergy < maxEnergy * .08) return 0;
    breakfastquay::MiniBPM engine (static_cast<float> (rate));
    engine.setBPMRange (45, 240);
    double bpm = engine.estimateTempoOfSamples (mono, static_cast<int> (frames));
    // Only resolve octave ambiguity when isolated, evenly spaced attacks provide
    // independent evidence; dense or irregular music retains MiniBPM's estimate.
    std::vector<double> attacks;
    double previous = 0;
    const double frameSeconds = double (rate / 50) / rate;
    for (size_t i = 0; i < envelope.size (); ++i)
    {
        const double time = i * frameSeconds;
        if (envelope[i] - previous > maxEnergy * .3 &&
            (attacks.empty () || time - attacks.back () > .12)) attacks.push_back (time);
        previous = envelope[i];
    }
    const size_t quiet = std::count_if (envelope.begin (), envelope.end (),
        [maxEnergy] (double e) { return e < maxEnergy * .05; });
    if (attacks.size () >= 6 && quiet > envelope.size () * .65)
    {
        std::vector<double> intervals;
        for (size_t i = 1; i < attacks.size (); ++i) intervals.push_back (attacks[i] - attacks[i - 1]);
        auto middle = intervals.begin () + intervals.size () / 2;
        std::nth_element (intervals.begin (), middle, intervals.end ());
        const double interval = *middle;
        const size_t regular = std::count_if (intervals.begin (), intervals.end (),
            [interval] (double t) { return std::abs (t - interval) < interval * .1; });
        const double pulse = 60 * (attacks.size () - 1) / (attacks.back () - attacks.front ());
        const double ratio = pulse / bpm;
        if (regular >= intervals.size () * .9 && pulse >= 45 && pulse <= 240 &&
            (std::abs (ratio - 2) < .08 || std::abs (ratio - .5) < .02)) bpm = pulse;
    }
    return std::isfinite (bpm) && bpm >= 45 && bpm <= 240 ? bpm : 0;
}

TempoAnalyzer::TempoAnalyzer () : m_result (std::make_shared<TempoResult> ())
{
    m_thread = std::thread ([this] { loop (); });
}

TempoAnalyzer::~TempoAnalyzer ()
{
    {
        std::lock_guard<std::mutex> lock (m_mutex);
        m_stop = true;
        ++m_generation;
    }
    m_cv.notify_one ();
    m_thread.join ();
}

void TempoAnalyzer::replace (std::shared_ptr<const AudioData> audio, Decoder decoder, bool live)
{
    std::lock_guard<std::mutex> lock (m_mutex);
    ++m_generation;
    m_audio = std::move (audio);
    m_decoder = std::move (decoder);
    m_blocks.clear ();
    m_live = live;
    m_pending = true;
    auto result = std::make_shared<TempoResult> ();
    result->state = live ? TempoResult::State::Live :
        m_audio || m_decoder ? TempoResult::State::Analyzing : TempoResult::State::Empty;
    std::atomic_store (&m_result, std::shared_ptr<const TempoResult> (result));
    m_cv.notify_one ();
}

void TempoAnalyzer::clear () { replace ({}, {}, false); }
void TempoAnalyzer::analyze (std::shared_ptr<const AudioData> audio) { replace (std::move (audio), {}, false); }
void TempoAnalyzer::decode (Decoder decoder) { replace ({}, std::move (decoder), false); }
void TempoAnalyzer::startLive () { replace ({}, {}, true); }
std::shared_ptr<const TempoResult> TempoAnalyzer::snapshot () const { return std::atomic_load (&m_result); }

void TempoAnalyzer::publish (uint64_t generation, const TempoResult& result)
{
    auto copy = std::make_shared<const TempoResult> (result);
    std::lock_guard<std::mutex> lock (m_mutex);
    if (generation == m_generation) std::atomic_store (&m_result, std::move (copy));
}

void TempoAnalyzer::push (const float* stereo, size_t frames, uint32_t rate,
                          uint32_t generation, double endSeconds, double playbackRate)
{
    if (!stereo || !frames || frames > 16384 || rate < 8000 || rate > 192000 ||
        !std::isfinite (endSeconds) || !std::isfinite (playbackRate) || playbackRate < .25 || playbackRate > 4) return;
    std::lock_guard<std::mutex> lock (m_mutex);
    if (!m_live) return;
    // This queue is independent of the playback ring and never touched by render().
    if (m_blocks.size () >= 32) m_blocks.pop_front ();
    m_blocks.push_back ({std::vector<float> (stereo, stereo + frames * 2), rate,
                         generation, endSeconds, playbackRate});
    m_cv.notify_one ();
}

void TempoAnalyzer::loop ()
{
    uint64_t current = 0;
    uint32_t streamGeneration = 0;
    std::vector<float> rolling;
    double phase = 0, sum = 0, weight = 0, lastEnd = -1, nextEstimate = 0;
    TempoResult liveResult;
    for (;;)
    {
        std::shared_ptr<const AudioData> audio;
        Decoder decoder;
        Block block;
        bool live = false;
        {
            std::unique_lock<std::mutex> lock (m_mutex);
            m_cv.wait (lock, [&] { return m_stop || m_pending || !m_blocks.empty (); });
            if (m_stop) return;
            if (m_pending)
            {
                current = m_generation;
                audio = std::move (m_audio);
                decoder = std::move (m_decoder);
                m_pending = false;
                rolling.clear ();
                phase = sum = weight = 0;
                lastEnd = -1;
                liveResult = {};
                liveResult.state = TempoResult::State::Live;
            }
            live = m_live;
            if (live && !m_blocks.empty ()) { block = std::move (m_blocks.front ()); m_blocks.pop_front (); }
        }
        const auto cancelled = [&] { return current != m_generation.load (); };
        try
        {
            if (decoder)
            {
                auto decoded = std::make_shared<AudioData> ();
                std::string error;
                if (!decoder (*decoded, error, cancelled))
                {
                    TempoResult failed;
                    failed.state = TempoResult::State::Error;
                    failed.error = error;
                    publish (current, failed);
                    continue;
                }
                audio = std::move (decoded);
            }
            if (cancelled ()) continue;
            if (audio)
            {
                if (!audio->sampleRate || !audio->numChannels || !audio->numFrames ())
                    throw std::runtime_error ("No audio samples");
                std::vector<float> mono;
                mono.reserve (static_cast<size_t> (audio->durationSec () * analysisRate));
                double p = 0, s = 0, w = 0;
                for (size_t i = 0; i < audio->numFrames () && !cancelled (); i += 8192)
                    appendMono (audio->samples.data () + i * audio->numChannels,
                        std::min (size_t (8192), audio->numFrames () - i), audio->numChannels,
                        audio->sampleRate, mono, p, s, w);
                TempoResult result;
                result.state = TempoResult::State::Analyzing;
                const double duration = mono.size () / double (analysisRate);
                const size_t window = std::min (mono.size (), size_t (windowSeconds * analysisRate));
                for (double t = 0; t < duration && !cancelled (); t += 1)
                {
                    const size_t begin = static_cast<size_t> (std::clamp (t + .5 - windowSeconds / 2,
                        0.0, std::max (0.0, duration - windowSeconds)) * analysisRate);
                    const double bpm = estimateSegmentTempo (mono.data () + begin, window, analysisRate);
                    result.segments.push_back ({t, std::min (duration, t + 1), bpm});
                    result.progress = std::min (1.0, (t + 1) / duration);
                    if (result.segments.size () % 8 == 0) publish (current, result);
                }
                result.state = TempoResult::State::Ready;
                result.progress = 1;
                publish (current, result);
            }
            else if (live && !block.samples.empty ())
            {
                const double begin = block.end - block.samples.size () / 2.0 / block.rate * block.speed;
                if (streamGeneration != block.generation || lastEnd < 0 || std::abs (begin - lastEnd) > .3)
                {
                    rolling.clear ();
                    phase = sum = weight = 0;
                    nextEstimate = begin + 6;
                    streamGeneration = block.generation;
                }
                appendMono (block.samples.data (), block.samples.size () / 2, 2,
                            block.rate / block.speed, rolling, phase, sum, weight);
                lastEnd = block.end;
                const size_t maximum = static_cast<size_t> (windowSeconds * analysisRate);
                if (rolling.size () > maximum) rolling.erase (rolling.begin (), rolling.end () - maximum);
                if (block.end >= nextEstimate && rolling.size () >= analysisRate * 6ull)
                {
                    const double bpm = estimateSegmentTempo (rolling.data (), rolling.size (), analysisRate);
                    const double start = std::floor (std::max (0.0, block.end));
                    auto& segments = liveResult.segments;
                    const auto it = std::lower_bound (segments.begin (), segments.end (), start,
                        [] (const TempoSegment& a, double t) { return a.start < t; });
                    if (it != segments.end () && it->start == start) *it = {start, start + 2, bpm};
                    else segments.insert (it, {start, start + 2, bpm});
                    if (segments.size () > 7200) segments.erase (segments.begin ());
                    nextEstimate = block.end + 1;
                    publish (current, liveResult);
                }
            }
        }
        catch (const std::exception& e)
        {
            TempoResult failed;
            failed.state = TempoResult::State::Error;
            failed.error = e.what ();
            publish (current, failed);
        }
    }
}
} // namespace ap
