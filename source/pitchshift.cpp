#include "pitchshift.h"
#include "third_party/signalsmith-stretch/signalsmith-stretch.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ap {
namespace {

// Read interleaved source data directly, padding only the algorithm's tail.
struct InputChannels
{
    const AudioData& audio;
    size_t start;
    struct Channel
    {
        const AudioData& audio;
        size_t start, channel;
        float operator[] (int index) const
        {
            const size_t frame = start + static_cast<size_t> (index);
            return frame < audio.numFrames ()
                ? audio.samples[frame * audio.numChannels + channel] : 0.0f;
        }
    };
    Channel operator[] (int channel) const { return {audio, start, static_cast<size_t> (channel)}; }
};

} // namespace

int clampPitchSemitones (int value)
{
    return std::clamp (value, kMinPitchSemitones, kMaxPitchSemitones);
}

int clampPitchCents (int value)
{
    return std::clamp (value, kMinPitchCents, kMaxPitchCents);
}

double pitchRatio (int semitones, int cents)
{
    return std::pow (2.0, (clampPitchSemitones (semitones) * 100.0 +
                           clampPitchCents (cents)) / 1200.0);
}

bool pitchShiftFixedDuration (const AudioData& input, int semitones, int cents,
                              AudioData& output, const PitchRenderProgress& progress)
{
    if (input.samples.empty () || input.sampleRate == 0 || input.numChannels == 0 ||
        input.numChannels > 32 || input.samples.size () % input.numChannels != 0)
        return false;
    if (progress && !progress (0)) return false;

    output.sampleRate = input.sampleRate;
    output.numChannels = input.numChannels;
    output.samples.resize (input.samples.size ());
    if (clampPitchSemitones (semitones) * 100 + clampPitchCents (cents) == 0)
    {
        output.samples = input.samples;
        return !progress || progress (1);
    }

    signalsmith::stretch::SignalsmithStretch<float> stretch (0);
    stretch.presetDefault (input.numChannels, static_cast<float> (input.sampleRate));
    stretch.setTransposeFactor (static_cast<float> (pitchRatio (semitones, cents)));

    // Discard the combined algorithm latency, not source samples. Feeding the
    // same number of input/output frames preserves score timing at either pitch.
    const size_t latency = stretch.inputLatency () + stretch.outputLatency ();
    const size_t frames = input.numFrames ();
    constexpr int blockSize = 1024;
    std::vector<std::vector<float>> block (input.numChannels, std::vector<float> (blockSize));
    for (size_t start = 0; start < frames + latency; start += blockSize)
    {
        if (progress && !progress (static_cast<float> (start) / (frames + latency)))
            return false;
        const int count = static_cast<int> (std::min<size_t> (blockSize, frames + latency - start));
        stretch.process (InputChannels {input, start}, count, block, count);
        for (int i = 0; i < count; ++i)
        {
            const size_t absolute = start + i;
            if (absolute < latency) continue;
            const size_t frame = absolute - latency;
            for (uint32_t c = 0; c < input.numChannels; ++c)
                output.samples[frame * input.numChannels + c] = block[c][i];
        }
    }
    return !progress || progress (1);
}

} // namespace ap
