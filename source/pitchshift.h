//==============================================================================
// pitchshift.h - fixed-duration offline pitch shifting
//
// The processor keeps the original decoded audio as a source snapshot.  A
// pitch change is rendered away from the audio callback and published as a
// second immutable snapshot, so changing pitch never changes the MuseScore
// timeline or the number of output frames.
//==============================================================================
#pragma once

#include "audiofile.h"
#include <functional>

namespace ap {

constexpr int kMinPitchSemitones = -24;
constexpr int kMaxPitchSemitones = 24;
constexpr int kMinPitchCents = -100;
constexpr int kMaxPitchCents = 100;

int clampPitchSemitones (int value);
int clampPitchCents (int value);
double pitchRatio (int semitones, int cents);

// Render a pitch-shifted copy with exactly the same number of frames, sample
// rate, and channel count as input. Returns false for invalid audio or on
// cancellation. The optional callback returns false to abort an obsolete render.
using PitchRenderProgress = std::function<bool(float)>;
bool pitchShiftFixedDuration (const AudioData& input,
                              int semitones,
                              int cents,
                              AudioData& output,
                              const PitchRenderProgress& progress = {});

} // namespace ap
