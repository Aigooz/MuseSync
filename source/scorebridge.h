#pragma once

#include "gui.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ap {

struct ScoreSyncSnapshot
{
    uint64_t timestampMs = 0;
    float bpm = 120.0f;
    int beats = 4;
    int denominator = 4;
    std::vector<TimeSigChange> timeSigChanges;
    std::vector<MeasureOverride> measures;
    std::vector<TempoChange> tempos;
    std::string session;
};

bool parseScoreSyncSnapshot (const std::string& text, uint64_t nowMs,
                             ScoreSyncSnapshot& snapshot);
bool parseScoreSelection (const std::string& text, const std::string& session,
                         uint64_t nowMs, double& seconds);

#if defined(_WIN32)
uint64_t scoreBridgeNowMs ();
bool readScoreSyncSnapshot (uint64_t nowMs, ScoreSyncSnapshot& snapshot,
                            std::string& rawSnapshot);
bool requestScoreSeek (double scoreSeconds, uint64_t nowMs);
bool readScoreSelection (const std::string& session, uint64_t nowMs, double& seconds);
#endif

} // namespace ap
