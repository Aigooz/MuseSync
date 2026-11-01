#pragma once

#include <string>

namespace ap {

struct ScoreInfo
{
    float bpm = 0.0f;
    int beats = 0;
    int denominator = 4;
};

// Read the opening tempo and meter, not an estimate from the audio file.
bool readScoreInfo (const std::string& path, ScoreInfo& info, std::string& error);

} // namespace ap
