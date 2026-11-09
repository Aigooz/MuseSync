#pragma once

#include <cmath>
#include <cwchar>
#include <cwctype>

namespace ap::ui {

inline bool numericDraft (const wchar_t* text)
{
    if (!text) return false;
    bool decimal = false;
    const wchar_t* p = text;
    if (*p == L'+' || *p == L'-') ++p;
    for (; *p; ++p)
    {
        if (*p >= L'0' && *p <= L'9') continue;
        else if (*p == L'.' && !decimal) decimal = true;
        else return false;
    }
    return true;
}

inline bool parseNumericInput (const wchar_t* text, double minimum, double maximum, double& value)
{
    if (!text || !*text || !numericDraft (text)) return false;
    wchar_t* end = nullptr;
    const double parsed = std::wcstod (text, &end);
    if (end == text || !std::isfinite (parsed)) return false;
    if (*end || parsed < minimum || parsed > maximum) return false;
    value = parsed;
    return true;
}

} // namespace ap::ui
