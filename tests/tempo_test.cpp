#include "tempo.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
void check (bool condition, const char* message)
{
    if (!condition) throw std::runtime_error (message);
}

std::shared_ptr<ap::AudioData> clicks (uint32_t rate, int channels, double seconds,
                                      double first, double second = 0, double change = 0)
{
    auto audio = std::make_shared<ap::AudioData> ();
    audio->sampleRate = rate;
    audio->numChannels = channels;
    audio->samples.resize (static_cast<size_t> (rate * seconds) * channels);
    for (double t = .1; t < seconds;)
    {
        const size_t begin = static_cast<size_t> (t * rate);
        for (size_t i = 0; i < rate * .025 && begin + i < audio->numFrames (); ++i)
        {
            const float value = static_cast<float> (.8 * std::exp (-i / (rate * .004)) *
                std::cos (6.28318530718 * 170 * i / rate));
            for (int c = 0; c < channels; ++c)
                audio->samples[(begin + i) * channels + c] = c % 2 ? -value : value;
        }
        t += 60 / (second && t >= change ? second : first);
    }
    return audio;
}

std::shared_ptr<const ap::TempoResult> waitReady (ap::TempoAnalyzer& analyzer)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (30);
    while (std::chrono::steady_clock::now () < deadline)
    {
        const auto result = analyzer.snapshot ();
        check (result->state != ap::TempoResult::State::Error, "analysis failed");
        if (result->state == ap::TempoResult::State::Ready) return result;
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    throw std::runtime_error ("analysis timeout");
}
}

int main ()
{
    try
    {
        ap::TempoAnalyzer analyzer;
        for (uint32_t rate : {22050, 44100, 48000})
        {
            analyzer.analyze (clicks (rate, 2, 14, 120));
            const auto result = waitReady (analyzer);
            std::printf ("%u Hz antiphase stereo: %.2f BPM\n", rate, result->bpmAt (7));
            check (std::abs (result->bpmAt (7) - 120) < 3, "fixed tempo / sample rate / stereo");
        }
        analyzer.analyze (clicks (44100, 1, 40, 90, 150, 20));
        const auto variable = waitReady (analyzer);
        std::printf ("Variable: %.2f -> %.2f BPM\n", variable->bpmAt (9), variable->bpmAt (32));
        check (std::abs (variable->bpmAt (9) - 90) < 4, "slow segment");
        check (std::abs (variable->bpmAt (32) - 150) < 4, "fast segment");
        check (!variable->bpmAt (-1) && !variable->bpmAt (45), "out of range lookup");
        auto silence = clicks (22050, 1, 12, 120);
        std::fill (silence->samples.begin (), silence->samples.end (), 0.f);
        analyzer.analyze (silence);
        check (!waitReady (analyzer)->bpmAt (6), "silence must not invent BPM");
        for (size_t i = 0; i < silence->samples.size (); ++i)
            silence->samples[i] = static_cast<float> (.2 * std::sin (6.28318530718 * 440 * i / 22050));
        analyzer.analyze (silence);
        check (!waitReady (analyzer)->bpmAt (6), "steady tone must not invent BPM");
        analyzer.analyze (clicks (44100, 2, 120, 100));
        analyzer.analyze (clicks (22050, 1, 12, 140));
        check (std::abs (waitReady (analyzer)->bpmAt (6) - 140) < 4, "replacement cancels old analysis");
        analyzer.clear ();
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
        check (analyzer.snapshot ()->state == ap::TempoResult::State::Empty, "clear discards stale result");

        analyzer.startLive ();
        const auto live = clicks (48000, 2, 16, 120);
        // Run at double speed but retain the original media BPM and timestamp.
        const size_t block = 2048;
        for (size_t i = 0; i + block <= live->numFrames (); i += block)
        {
            analyzer.push (live->samples.data () + i * 2, block, 96000, 1,
                           double (i + block) / 48000, 2);
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (200));
        const auto online = analyzer.snapshot ();
        std::printf ("Online: %zu segments\n", online->segments.size ());
        check (online->segments.size () >= 4, "live segment updates");
        check (std::abs (online->segments.back ().bpm - 120) < 3, "live tempo follows media time");
        analyzer.push (live->samples.data (), block, 48000, 2, 60, 1);
        std::this_thread::sleep_for (std::chrono::milliseconds (30));
        check (!analyzer.snapshot ()->bpmAt (60), "seek does not copy BPM into unobserved region");
        analyzer.clear ();
        std::puts ("PASS: tempo windows, variable BPM, silence, stereo, cancellation and online PCM");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf (stderr, "FAIL: %s\n", error.what ());
        return 1;
    }
}
