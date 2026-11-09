#pragma once

#include <algorithm>
#include <cstdint>

namespace ap::ui {

// Theme indices are stored in VST3 project state; light/dark retain their indices.
struct Theme
{
    const wchar_t* name;
    uint32_t paper, surface, ink, muted, rule, softRule;
    uint32_t accent, accentSoft, audioHead, scoreHead;
};

inline constexpr Theme themes[] = {
    {L"亮色",   0xf3f4f5, 0xffffff, 0x242628, 0x62676c, 0xbac0c5, 0xdce0e4, 0x126eae, 0xdcecf8, 0xb83b50, 0x126eae},
    {L"暗色",   0x282a2d, 0x202225, 0xf1f2f3, 0xb3b7bd, 0x60656c, 0x3b3f44, 0x81bfff, 0x30445b, 0xff9aac, 0x81bfff},
    {L"黑色",   0x000000, 0x121212, 0xf5f5f5, 0xb8b8b8, 0x5c5c5c, 0x303030, 0x9bcaff, 0x213448, 0xff9aac, 0x9bcaff},
    {L"MuseScore", 0xeeeeee, 0xffffff, 0x242424, 0x606060, 0xb3b3b3, 0xd5d5d5, 0x0068b5, 0xd8eafa, 0xb8324a, 0x0068b5},
};

inline constexpr int themeCount = static_cast<int> (sizeof themes / sizeof themes[0]);
inline constexpr int defaultTheme = 1;
inline int clampTheme (int value) { return std::clamp (value, 0, themeCount - 1); }

} // namespace ap::ui
