#pragma once

#include <string>
#include <string_view>

namespace ap {
inline bool isMuseScoreHost (std::string_view hostName, std::wstring_view processPath)
{
    if (hostName.find ("MuseScore") != std::string_view::npos) return true;

    // MuseScore 4.6 uses the SDK's default HostApplication name. Check the
    // executable as well, without enabling transport inference for other hosts.
    const auto separator = processPath.find_last_of (L"/\\");
    std::wstring executable (separator == std::wstring_view::npos
                                 ? processPath : processPath.substr (separator + 1));
    for (auto& c : executable)
        if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    return executable == L"musescore4.exe" || executable == L"musescore3.exe" ||
           executable == L"musescore.exe";
}
} // namespace ap
