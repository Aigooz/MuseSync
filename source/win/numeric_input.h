#pragma once

#include <cmath>
#include <cwchar>
#include <cwctype>

namespace ap::ui {

inline bool parseNumericInput (const wchar_t* text, double minimum, double maximum, double& value)
{
    if (!text || !*text) return false;
    wchar_t* end = nullptr;
    const double parsed = std::wcstod (text, &end);
    if (end == text || !std::isfinite (parsed)) return false;
    while (std::iswspace (*end)) ++end;
    if (*end || parsed < minimum || parsed > maximum) return false;
    value = parsed;
    return true;
}

} // namespace ap::ui
