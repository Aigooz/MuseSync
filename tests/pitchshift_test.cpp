//==============================================================================
// pitchshift_test.cpp - focused fixed-duration pitch-shift regression tests
//==============================================================================
#include "pitchshift.h"

#include <cmath>
#include <complex>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <string>

namespace {

constexpr double kPi = 3.1415926535897932384626433832795;

int gPassed = 0;
int gFailed = 0;

void check (const std::string& name, bool condition)
{
    if (condition)
    {
        std::printf ("[PASS] %s\n", name.c_str ());
        ++gPassed;
    }
    else
    {
        std::printf ("[FAIL] %s\n", name.c_str ());
        ++gFailed;
    }
}

ap::AudioData makeStereoSine (uint32_t sampleRate, std::size_t frames,
                              double frequency)
{
    ap::AudioData audio;
    audio.sampleRate = sampleRate;
    audio.numChannels = 2;
    audio.samples.resize (frames * audio.numChannels);
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const float value = static_cast<float> (0.7 * std::sin (
            2.0 * kPi * frequency * static_cast<double> (frame) /
            static_cast<double> (sampleRate)));
        audio.samples[frame * 2] = value;
        audio.samples[frame * 2 + 1] = value;
    }
    return audio;
}

double frequencyMagnitude (const ap::AudioData& audio, double frequency)
{
    const std::size_t first = audio.numFrames () / 4;
    const std::size_t count = audio.numFrames () / 2;
    std::complex<double> sum (0.0, 0.0);
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::size_t frame = first + i;
        const double window = 0.5 - 0.5 * std::cos (
            2.0 * kPi * static_cast<double> (i) /
            static_cast<double> (count));
        const double phase = 2.0 * kPi * frequency *
            static_cast<double> (frame) / static_cast<double> (audio.sampleRate);
        sum += static_cast<double> (audio.samples[frame * audio.numChannels]) *
               window * std::complex<double> (std::cos (phase), -std::sin (phase));
    }
    return std::abs (sum);
}

double dominantFrequencyNear (const ap::AudioData& audio, double expected)
{
    const double halfWidth = std::max (30.0, expected * 0.12);
    double bestFrequency = expected;
    double bestMagnitude = 0.0;
    for (double frequency = std::max (1.0, expected - halfWidth);
         frequency <= expected + halfWidth; frequency += 0.5)
    {
        const double magnitude = frequencyMagnitude (audio, frequency);
        if (magnitude > bestMagnitude)
        {
            bestMagnitude = magnitude;
            bestFrequency = frequency;
        }
    }
    return bestFrequency;
}

void runPitchCase (int semitones, int cents)
{
    constexpr uint32_t sampleRate = 44100;
    constexpr std::size_t frames = sampleRate * 2;
    constexpr double sourceFrequency = 440.0;

    const ap::AudioData input = makeStereoSine (sampleRate, frames, sourceFrequency);
    ap::AudioData output;
    const bool rendered = ap::pitchShiftFixedDuration (input, semitones, cents, output);
    const double expected = sourceFrequency * ap::pitchRatio (semitones, cents);
    const double measured = rendered ? dominantFrequencyNear (output, expected) : 0.0;
    const double tolerance = std::max (3.0, expected * 0.025);

    const std::string label = "pitch " + std::to_string (semitones) +
                              " semitones, " + std::to_string (cents) + " cents";
    check (label + " renders", rendered);
    check (label + " keeps sample rate", rendered && output.sampleRate == input.sampleRate);
    check (label + " keeps channels", rendered && output.numChannels == input.numChannels);
    check (label + " keeps frames", rendered && output.numFrames () == input.numFrames ());
    check (label + " reaches expected frequency", rendered &&
           std::abs (measured - expected) <= tolerance);
}

void runRenderControlTests ()
{
    const auto input = makeStereoSine (48000, 48000, 440);
    ap::AudioData output;
    check ("zero pitch renders original samples", ap::pitchShiftFixedDuration (input, 0, 0, output) &&
           output.samples == input.samples);
    check ("opposing semitone and cents preserve samples", ap::pitchShiftFixedDuration (input, 1, -100, output) &&
           output.samples == input.samples);
    check ("cancel before rendering", !ap::pitchShiftFixedDuration (input, 2, 0, output,
           [] (float) { return false; }));
    float lastProgress = 0;
    bool monotonic = true;
    const bool cancelled = !ap::pitchShiftFixedDuration (input, 2, 0, output,
        [&] (float value) {
            monotonic = monotonic && value >= lastProgress && value <= 1;
            lastProgress = value;
            return value < .25f;
        });
    check ("cancel during block processing", cancelled && lastProgress >= .25f && lastProgress < .3f);
    check ("render progress is bounded and monotonic", monotonic);
    lastProgress = 0;
    const bool complete = ap::pitchShiftFixedDuration (input, 2, 0, output,
        [&] (float value) { lastProgress = value; return true; });
    check ("completed render reports full progress", complete && lastProgress == 1);
    ap::AudioData invalid;
    check ("reject empty audio", !ap::pitchShiftFixedDuration (invalid, 2, 0, output));
    auto malformed = input;
    malformed.samples.pop_back ();
    check ("reject incomplete interleaved frame", !ap::pitchShiftFixedDuration (malformed, 2, 0, output));
    ap::AudioData shortMono;
    shortMono.sampleRate = 48000;
    shortMono.numChannels = 1;
    shortMono.samples.assign (137, .5f);
    check ("short mono audio keeps frame count", ap::pitchShiftFixedDuration (shortMono, 2, 0, output) &&
           output.numFrames () == 137 && output.numChannels == 1);
}

} // namespace

int main (int argc, char** argv)
{
    if (argc > 1 && std::strcmp (argv[1], "--benchmark") == 0)
    {
        const auto input = makeStereoSine (48000, 48000 * 60, 440);
        ap::AudioData output;
        const auto start = std::chrono::steady_clock::now ();
        const bool ok = ap::pitchShiftFixedDuration (input, 2, 0, output);
        std::printf ("60s stereo / 48kHz / +2 semitones: %.3fs, %s\n",
            std::chrono::duration<double> (std::chrono::steady_clock::now () - start).count (),
            ok ? "OK" : "FAILED");
        return ok ? 0 : 1;
    }
    check ("ratio +12 semitones", std::abs (ap::pitchRatio (12, 0) - 2.0) < 1.0e-12);
    check ("ratio -12 semitones", std::abs (ap::pitchRatio (-12, 0) - 0.5) < 1.0e-12);
    check ("ratio +100 cents", std::abs (ap::pitchRatio (0, 100) - std::pow (2.0, 1.0 / 12.0)) < 1.0e-12);
    check ("semitone lower clamp", ap::clampPitchSemitones (-999) == -24);
    check ("semitone upper clamp", ap::clampPitchSemitones (999) == 24);
    check ("cent lower clamp", ap::clampPitchCents (-999) == -100);
    check ("cent upper clamp", ap::clampPitchCents (999) == 100);

    runPitchCase (-24, 0);
    runPitchCase (24, 0);
    runPitchCase (2, 0);
    runPitchCase (0, -100);
    runPitchCase (0, 100);
    runRenderControlTests ();

    std::printf ("Result: %d passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
