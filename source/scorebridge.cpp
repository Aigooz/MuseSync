#include "scorebridge.h"

#include <cmath>
#include <atomic>
#include <charconv>
#include <locale>
#include <sstream>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace ap {
namespace {

bool parseUnsigned (const std::string& text, uint64_t& value)
{
    if (text.empty ()) return false;
    const auto result = std::from_chars (text.data (), text.data () + text.size (), value);
    return result.ec == std::errc {} && result.ptr == text.data () + text.size ();
}

bool parseInt (const std::string& text, int& value)
{
    if (text.empty ()) return false;
    const auto result = std::from_chars (text.data (), text.data () + text.size (), value);
    return result.ec == std::errc {} && result.ptr == text.data () + text.size ();
}

bool parseDecimal (const std::string& text, double& value)
{
    if (text.empty () || text.find_first_not_of ("0123456789.") != std::string::npos) return false;
    std::istringstream input (text);
    input.imbue (std::locale::classic ());
    input >> std::noskipws >> value;
    return !input.fail () && input.eof () && std::isfinite (value);
}

bool validSession (const std::string& session)
{
    const auto split = session.find ('-');
    uint64_t first = 0, second = 0;
    return session.size () <= 64 && split != std::string::npos &&
        parseUnsigned (session.substr (0, split), first) &&
        parseUnsigned (session.substr (split + 1), second);
}

} // namespace

bool parseScoreSyncSnapshot (const std::string& text, uint64_t nowMs,
                             ScoreSyncSnapshot& snapshot)
{
    if (text.size () > 1024 * 1024 || text.find_first_of ("\r\n\0", 0, 3) != std::string::npos)
        return false;
    std::istringstream input (text);
    std::string version, timestamp, bpm, beats, denominator, changes, measures, session, tempos;
    if (!std::getline (input, version, '|') || (version != "MT1" && version != "MT2" && version != "MT3") ||
        !std::getline (input, timestamp, '|') ||
        !std::getline (input, bpm, '|') ||
        !std::getline (input, beats, '|') ||
        !std::getline (input, denominator, '|'))
        return false;
    if (input.eof ()) return false;
    if (version == "MT2" || version == "MT3")
    {
        if (!std::getline (input, changes, '|') || !std::getline (input, measures, '|') ||
            !std::getline (input, session, '|') || !validSession (session)) return false;
        if (version == "MT3")
        {
            if (input.eof ()) return false;
            std::getline (input, tempos);
        }
        else if (!input.eof ()) return false;
    }
    else std::getline (input, changes);
    if (changes.find ('|') != std::string::npos) return false;

    ScoreSyncSnapshot parsed;
    int beatCount = 0, beatDenominator = 0;
    if (!parseUnsigned (timestamp, parsed.timestampMs) ||
        parsed.timestampMs > nowMs || nowMs - parsed.timestampMs > 15000 ||
        !parseInt (beats, beatCount) || !parseInt (denominator, beatDenominator))
        return false;

    if (bpm.empty () || bpm.find_first_not_of ("0123456789.") != std::string::npos) return false;
    std::istringstream bpmInput (bpm);
    bpmInput.imbue (std::locale::classic ());
    double parsedBpm = 0;
    bpmInput >> std::noskipws >> parsedBpm;
    if (bpmInput.fail () || !bpmInput.eof () || !std::isfinite (parsedBpm) || parsedBpm < 1 || parsedBpm > 1000 ||
        beatCount < 1 || beatCount > 32 || normalizeBeatDenom (beatDenominator, 0) == 0)
        return false;
    parsed.bpm = static_cast<float> (parsedBpm);
    parsed.beats = beatCount;
    parsed.denominator = beatDenominator;
    parsed.session = session;

    std::istringstream list (changes);
    if (!changes.empty () && changes.back () == ';') return false;
    std::string item;
    int previousBar = 1;
    while (std::getline (list, item, ';'))
    {
        if (item.empty ()) return false;
        std::istringstream fields (item);
        if (item.back () == ',') return false;
        std::string barText, beatsText, denominatorText, trailing;
        if (!std::getline (fields, barText, ',') || !std::getline (fields, beatsText, ',') ||
            !std::getline (fields, denominatorText, ',') || std::getline (fields, trailing, ','))
            return false;
        TimeSigChange change;
        if (!parseInt (barText, change.bar) || !parseInt (beatsText, change.beats) ||
            !parseInt (denominatorText, change.denom) || change.bar <= previousBar ||
            change.bar > 1000000 || change.beats < 1 || change.beats > 32 ||
            normalizeBeatDenom (change.denom, 0) == 0)
            return false;
        parsed.timeSigChanges.push_back (change);
        previousBar = change.bar;
        if (parsed.timeSigChanges.size () > 10000) return false;
    }

    std::istringstream measureList (measures);
    previousBar = 0;
    if (!measures.empty () && measures.back () == ';') return false;
    while (std::getline (measureList, item, ';'))
    {
        const auto a = item.find (','), b = item.find (',', a == std::string::npos ? 0 : a + 1);
        MeasureOverride measure;
        if (a == std::string::npos || b == std::string::npos ||
            !parseInt (item.substr (0, a), measure.bar) ||
            !parseDecimal (item.substr (a + 1, b - a - 1), measure.quarters) ||
            !parseInt (item.substr (b + 1), measure.number) ||
            measure.bar <= previousBar || measure.bar > 100000 ||
            measure.quarters <= 0 || measure.quarters > 256 ||
            measure.number < -1000000 || measure.number > 1000000) return false;
        previousBar = measure.bar;
        parsed.measures.push_back (measure);
    }

    std::istringstream tempoList (tempos);
    double previousQuarter = -1;
    if (!tempos.empty () && tempos.back () == ';') return false;
    while (std::getline (tempoList, item, ';'))
    {
        const auto comma = item.find (',');
        TempoChange tempo;
        if (comma == std::string::npos || !parseDecimal (item.substr (0, comma), tempo.quarter) ||
            !parseDecimal (item.substr (comma + 1), tempo.bpm) || tempo.quarter <= previousQuarter ||
            tempo.quarter > 10000000 || tempo.bpm < 1 || tempo.bpm > 1000 || parsed.tempos.size () >= 100000)
            return false;
        previousQuarter = tempo.quarter;
        parsed.tempos.push_back (tempo);
    }
    snapshot = std::move (parsed);
    return true;
}

#if defined(_WIN32)
namespace {
std::wstring bridgePath (const wchar_t* name)
{
    // The companion lives beside MuseScore's bundled QML plugins. No score file
    // or application preferences are opened, and this runs on the editor timer.
    wchar_t executable[32768] {};
    const DWORD length = GetModuleFileNameW (nullptr, executable, 32768);
    if (!length || length >= 32768) return {};
    std::wstring path (executable, length);
    const auto bin = path.find_last_of (L"\\/");
    if (bin == std::wstring::npos) return {};
    path.resize (bin);
    const auto root = path.find_last_of (L"\\/");
    if (root == std::wstring::npos) return {};
    path.resize (root);
    path += L"\\plugins\\MuseToneSync\\";
    return path + name;
}
} // namespace

uint64_t scoreBridgeNowMs ()
{
    FILETIME time {};
    GetSystemTimeAsFileTime (&time);
    ULARGE_INTEGER stamp {};
    stamp.LowPart = time.dwLowDateTime;
    stamp.HighPart = time.dwHighDateTime;
    return (stamp.QuadPart - 116444736000000000ULL) / 10000;
}

bool readScoreSyncSnapshot (uint64_t nowMs, ScoreSyncSnapshot& snapshot,
                            std::string& rawSnapshot)
{
    const auto path = bridgePath (L"MuseToneSync.ini");
    if (path.empty ()) return false;

    std::vector<wchar_t> buffer (2 * 1024 * 1024 + 1);
    const DWORD count = GetPrivateProfileStringW (L"Snapshot", L"hex", L"", buffer.data (),
                                                 static_cast<DWORD> (buffer.size ()), path.c_str ());
    if (!count || count >= buffer.size () - 1 || count % 2) return false;
    std::string decoded;
    decoded.reserve (count / 2);
    auto digit = [] (wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    for (DWORD i = 0; i < count; i += 2)
    {
        const int high = digit (buffer[i]), low = digit (buffer[i + 1]);
        if (high < 0 || low < 0) return false;
        decoded.push_back (static_cast<char> ((high << 4) | low));
    }
    if (!parseScoreSyncSnapshot (decoded, nowMs, snapshot)) return false;
    rawSnapshot = std::move (decoded);
    return true;
}

bool requestScoreSeek (double scoreSeconds, uint64_t nowMs)
{
    if (!std::isfinite (scoreSeconds) || scoreSeconds < 0 || scoreSeconds > 86400) return false;
    ScoreSyncSnapshot snapshot;
    std::string raw;
    if (!readScoreSyncSnapshot (nowMs, snapshot, raw) || snapshot.session.empty ()) return false;
    static std::atomic<uint64_t> sequence {0};
    std::ostringstream command;
    command.imbue (std::locale::classic ());
    command.precision (15);
    command << "MC1|" << GetCurrentProcessId () << '-' << ++sequence << '|' << nowMs << '|'
            << snapshot.session << '|' << scoreSeconds;
    const auto text = command.str ();
    const std::wstring wide (text.begin (), text.end ());
    const auto path = bridgePath (L"MuseToneCommands.ini");
    return !path.empty () && WritePrivateProfileStringW (L"Command", L"seek", wide.c_str (), path.c_str ());
}
#endif

} // namespace ap
