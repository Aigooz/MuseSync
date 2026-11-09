#include "aplaysdk.h"
#include "hostcompat.h"
#include "scoreinfo.h"
#include "scorebridge.h"
#include "win/ui_layout.h"
#include "ui_theme.h"
#include "video_audio.h"
#include "win/video_view.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "miniz.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {
void check (bool condition, const char* message)
{
    if (!condition) throw std::runtime_error (message);
}

void savePreview (HWND window, const wchar_t* name, bool captureScreen = false)
{
    wchar_t directory[MAX_PATH] {};
    if (!GetEnvironmentVariableW (L"MUSETONE_UI_PREVIEWS", directory, MAX_PATH)) return;
    RECT r {};
    GetClientRect (window, &r);
    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
    info.bmiHeader.biWidth = r.right;
    info.bmiHeader.biHeight = -r.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HDC dc = CreateCompatibleDC (nullptr);
    HBITMAP bitmap = CreateDIBSection (dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    check (bitmap && pixels, "preview bitmap allocation");
    HGDIOBJ previous = SelectObject (dc, bitmap);
    if (captureScreen)
    {
        POINT origin {};
        ClientToScreen (window, &origin);
        HDC screen = GetDC (nullptr);
        BitBlt (dc, 0, 0, r.right, r.bottom, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT);
        ReleaseDC (nullptr, screen);
    }
    else SendMessageW (window, WM_PRINTCLIENT, reinterpret_cast<WPARAM> (dc), PRF_CLIENT);
    for (HWND child = captureScreen ? nullptr : GetWindow (window, GW_CHILD); child; child = GetWindow (child, GW_HWNDNEXT))
    {
        if (!(GetWindowLongPtrW (child, GWL_STYLE) & WS_VISIBLE)) continue;
        RECT childRect {};
        GetWindowRect (child, &childRect);
        MapWindowPoints (nullptr, window, reinterpret_cast<POINT*> (&childRect), 2);
        const int saved = SaveDC (dc);
        IntersectClipRect (dc, childRect.left, childRect.top, childRect.right, childRect.bottom);
        SetViewportOrgEx (dc, childRect.left, childRect.top, nullptr);
        wchar_t className[32] {};
        GetClassNameW (child, className, 32);
        if (std::wcscmp (className, L"Edit") == 0)
        {
            // Native edits skip text painting under a hidden parent; capture their unfocused face.
            RECT face {};
            GetClientRect (child, &face);
            const auto brush = reinterpret_cast<HBRUSH> (SendMessageW (window, WM_CTLCOLOREDIT,
                reinterpret_cast<WPARAM> (dc), reinterpret_cast<LPARAM> (child)));
            if (brush) FillRect (dc, &face, brush);
            SelectObject (dc, reinterpret_cast<HFONT> (SendMessageW (child, WM_GETFONT, 0, 0)));
            wchar_t value[256] {};
            GetWindowTextW (child, value, 256);
            SetBkMode (dc, TRANSPARENT);
            DrawTextW (dc, value, -1, &face, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        else SendMessageW (child, WM_PRINTCLIENT, reinterpret_cast<WPARAM> (dc), PRF_CLIENT);
        RestoreDC (dc, saved);
    }
    BITMAPFILEHEADER header {};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof header + sizeof info.bmiHeader;
    const DWORD bytes = r.right * r.bottom * 4;
    header.bfSize = header.bfOffBits + bytes;
    std::filesystem::create_directories (directory);
    std::ofstream file (std::filesystem::path (directory) / name, std::ios::binary);
    file.write (reinterpret_cast<const char*> (&header), sizeof header);
    file.write (reinterpret_cast<const char*> (&info.bmiHeader), sizeof info.bmiHeader);
    file.write (static_cast<const char*> (pixels), bytes);
    check (file.good (), "preview bitmap write");
    SelectObject (dc, previous);
    DeleteObject (bitmap);
    DeleteDC (dc);
}

void checkWavePaint (HWND waveform, int theme)
{
    RECT r {};
    GetClientRect (waveform, &r);
    HDC dc = CreateCompatibleDC (nullptr);
    BITMAPINFO info {};
    info.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
    info.bmiHeader.biWidth = r.right;
    info.bmiHeader.biHeight = -r.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection (dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    check (dc && bitmap && pixels, "wave paint target allocation");
    const auto previous = SelectObject (dc, bitmap);
    SendMessageW (waveform, WM_PRINTCLIENT, reinterpret_cast<WPARAM> (dc), PRF_CLIENT);
    GdiFlush ();
    const auto* colors = static_cast<const uint32_t*> (pixels);
    const auto& palette = ap::ui::themes[theme];
    int wavePixels = 0, audioPixels = 0, scorePixels = 0;
    for (int i = 0; i < r.right * r.bottom; ++i)
    {
        const auto color = colors[i] & 0xffffff;
        wavePixels += color == palette.muted;
        audioPixels += color == palette.audioHead;
        scorePixels += color == palette.scoreHead;
    }
    check (wavePixels > 100, "printed waveform contains rendered audio peaks");
    check (audioPixels > 10, "audio playhead renders in printed waveform");
    check (scorePixels > 5, "score marker remains visible when playheads coincide");
    SelectObject (dc, previous);
    DeleteObject (bitmap);
    DeleteDC (dc);
}

struct TestHost : HostApplication
{
    bool museScore;
    explicit TestHost (bool muse) : museScore (muse) {}
    tresult PLUGIN_API getName (String128 name) override
    {
        const auto* source = museScore ? STR16 ("MuseScore Studio") : STR16 ("Test Host");
        std::fill_n (name, 128, 0);
        std::copy_n (source, museScore ? 16 : 9, name);
        return kResultOk;
    }
};

struct TestSeekBridge
{
    std::filesystem::path directory;
    TestSeekBridge ()
    {
        wchar_t executable[32768] {};
        check (GetModuleFileNameW (nullptr, executable, 32768) != 0, "test executable path");
        directory = std::filesystem::path (executable).parent_path ().parent_path () / "plugins" / "MuseToneSync";
        check (!std::filesystem::exists (directory), "seek bridge fixture must not replace existing files");
        std::filesystem::create_directories (directory);
        const std::string snapshot = "MT2|" + std::to_string (ap::scoreBridgeNowMs ()) + "|120|4|4|||1000-1";
        static const wchar_t digits[] = L"0123456789abcdef";
        std::wstring hex;
        for (const unsigned char c : snapshot) { hex += digits[c >> 4]; hex += digits[c & 15]; }
        check (WritePrivateProfileStringW (L"Snapshot", L"hex", hex.c_str (),
               (directory / "MuseToneSync.ini").c_str ()) != 0, "write isolated test snapshot");
    }
    std::wstring command () const
    {
        wchar_t buffer[512] {};
        GetPrivateProfileStringW (L"Command", L"seek", L"", buffer, 512,
                                 (directory / "MuseToneCommands.ini").c_str ());
        return buffer;
    }
    double target () const
    {
        const auto text = command ();
        check (text.find (L"MC1|") == 0, "progress sends companion seek command");
        return std::stod (text.substr (text.find_last_of (L'|') + 1));
    }
    void selection (double seconds) const
    {
        const auto value = L"MP1|" + std::to_wstring (ap::scoreBridgeNowMs ()) +
            L"|1000-1|" + std::to_wstring (seconds);
        check (WritePrivateProfileStringW (L"Position", L"value", value.c_str (),
            (directory / "MuseToneSync.ini").c_str ()) != 0, "publish test score selection");
    }
    ~TestSeekBridge ()
    {
        std::error_code error;
        std::filesystem::remove (directory / "MuseToneSync.ini", error);
        std::filesystem::remove (directory / "MuseToneCommands.ini", error);
        std::filesystem::remove (directory, error);
    }
};

struct Fixture
{
    TestHost host;
    aplay::APlayProcessor processor;
    ProcessContext context {};
    ProcessData data {};
    AudioBusBuffers bus {};
    float left[1024] {}, right[1024] {};
    float* channels[2] {left, right};
    explicit Fixture (bool museScore) : host (museScore)
    {
        check (processor.initialize (&host) == kResultOk, "initialize");
        ProcessSetup setup {};
        setup.processMode = kRealtime;
        setup.symbolicSampleSize = kSample32;
        setup.maxSamplesPerBlock = 1024;
        setup.sampleRate = 48000;
        processor.setupProcessing (setup);
        processor.setActive (true);
        processor.setProcessing (true);
        context.sampleRate = 48000;
        data.symbolicSampleSize = kSample32;
        data.numOutputs = 1;
        data.outputs = &bus;
        data.processContext = &context;
        bus.numChannels = 2;
        bus.channelBuffers32 = channels;
    }
    ~Fixture ()
    {
        processor.setProcessing (false);
        processor.setActive (false);
        processor.terminate ();
    }
    double block (int64 sample, uint32 state, int count = 256)
    {
        context.projectTimeSamples = sample;
        context.state = state;
        data.numSamples = count;
        bus.silenceFlags = 3;
        std::fill_n (left, 1024, -9.f);
        processor.process (data);
        check (bus.silenceFlags == 0, "output silence flags not cleared");
        double energy = 0;
        for (int i = 0; i < count; ++i) energy += std::abs (left[i]);
        return energy;
    }
};

void writeWave (const std::filesystem::path& path, bool dynamics = false, bool tempo = false)
{
    std::ofstream file (path, std::ios::binary);
    auto u16 = [&] (uint16_t v) { file.write (reinterpret_cast<char*> (&v), 2); };
    auto u32 = [&] (uint32_t v) { file.write (reinterpret_cast<char*> (&v), 4); };
    const int count = 48000 * (tempo ? 24 : 4);
    file.write ("RIFF", 4); u32 (36 + count * 2); file.write ("WAVEfmt ", 8);
    u32 (16); u16 (1); u16 (1); u32 (48000); u32 (96000); u16 (2); u16 (16);
    file.write ("data", 4); u32 (count * 2);
    for (int i = 0; i < count; ++i)
    {
        const double seconds = i / 48000.0;
        const double pulseTime = std::fmod (seconds, seconds < 12 ? 60.0 / 90 : 60.0 / 150);
        const double envelope = tempo ? std::exp (-pulseTime * 220) :
            dynamics ? .2 + .8 * std::exp (-std::fmod (seconds, .5) * 7) : .5;
        u16 (static_cast<uint16_t> (static_cast<int16_t> (18000 * envelope * std::sin (i * .04))));
    }
}

void hostIdentityTests ()
{
    check (ap::isMuseScoreHost ("MuseScore Studio", L""), "named MuseScore host");
    check (ap::isMuseScoreHost ("My VST3 HostApplication", L"C:\\Program Files\\MuseScore 4\\bin\\MuseScore4.exe"),
           "MuseScore executable with SDK default host name");
    check (ap::isMuseScoreHost ("", L"C:/Apps/MUSESCORE.EXE"), "case insensitive MuseScore executable");
    check (!ap::isMuseScoreHost ("My VST3 HostApplication", L"C:\\Apps\\validator.exe"),
           "SDK default name must not enable fallback in other hosts");
    check (!ap::isMuseScoreHost ("Test Host", L"C:\\MuseScore4.exe\\other.exe"),
           "MuseScore directory must not count as host executable");
    check (!ap::isMuseScoreHost ("Test Host", L"C:\\Apps\\not-MuseScore4.exe"),
           "reject executable substring match");
}

void scoreTests (const std::filesystem::path& directory)
{
    const char* xml = "<?xml version=\"1.0\"?><museScore version=\"4.0\"><Score>"
        "<Part><Staff><TimeSig><sigN>2</sigN><sigD>2</sigD></TimeSig></Staff></Part>"
        "<Staff id=\"1\"><Measure><voice><TimeSig><sigN>7</sigN><sigD>8</sigD></TimeSig>"
        "<Tempo><tempo>1.5</tempo></Tempo></voice></Measure>"
        "<Measure><voice><Tempo><tempo>2.5</tempo></Tempo></voice></Measure></Staff>"
        "<Staff id=\"2\"><Measure><voice><Tempo><tempo>3</tempo></Tempo></voice></Measure></Staff>"
        "</Score></museScore>";
    const auto score = directory / L"\u4e50\u8c31.mscx";
    { std::ofstream file (score, std::ios::binary); file << xml; }
    ap::ScoreInfo info;
    std::string error;
    check (ap::readScoreInfo (score.u8string (), info, error), "unicode MSCX parse");
    check (info.bpm == 90 && info.beats == 7 && info.denominator == 8, "opening tempo / meter");
    mz_zip_archive zip {};
    check (mz_zip_writer_init_heap (&zip, 0, 0) != 0, "zip init");
    check (mz_zip_writer_add_mem (&zip, "Excerpts/part.mscx", "<wrong/>", 8, MZ_BEST_COMPRESSION) != 0, "zip excerpt");
    check (mz_zip_writer_add_mem (&zip, "score.mscx", xml, std::strlen (xml), MZ_BEST_COMPRESSION) != 0, "zip score");
    void* buffer = nullptr;
    size_t size = 0;
    check (mz_zip_writer_finalize_heap_archive (&zip, &buffer, &size) != 0, "zip finalize");
    const auto compressed = directory / "score.mscz";
    { std::ofstream file (compressed, std::ios::binary); file.write (static_cast<const char*> (buffer), size); }
    mz_free (buffer);
    mz_zip_writer_end (&zip);
    check (ap::readScoreInfo (compressed.u8string (), info, error), "MSCZ parse");
    check (info.bpm == 90 && info.beats == 7 && info.denominator == 8, "MSCZ ignores excerpt");
    const auto defaults = directory / "defaults.mscx";
    { std::ofstream file (defaults); file << "<museScore><Score><Staff><Measure/><Measure><Tempo><tempo>3</tempo></Tempo></Measure></Staff></Score></museScore>"; }
    check (ap::readScoreInfo (defaults.u8string (), info, error) && info.bpm == 120 && info.beats == 4,
           "later tempo is not the opening tempo");
    const auto malformed = directory / "broken.mscx";
    { std::ofstream file (malformed); file << "<museScore><Score>"; }
    check (!ap::readScoreInfo (malformed.u8string (), info, error), "reject malformed XML");
    check (!ap::readScoreInfo ((directory / "missing.mscz").u8string (), info, error), "reject missing score");
    Fixture fixture (false);
    check (fixture.processor.theme () == 1, "new instances default to dark theme");
    check (fixture.processor.loadScore (score.u8string ()), "backend load score");
    auto timeline = fixture.processor.hostTimeline ();
    check (timeline.scoreTempoValid && timeline.scoreBpm == 90 && timeline.scoreMeterValid &&
           timeline.scoreBeats == 7 && timeline.scoreDenominator == 8, "linked score metadata");
    fixture.processor.setTheme (1);
    fixture.processor.setFollowScore (false);
    fixture.processor.setVolume (.37f);
    fixture.processor.setOffsetSec (1.25f);
    MemoryStream stream;
    fixture.processor.getState (&stream);
    fixture.processor.setTheme (2);
    fixture.processor.setFollowScore (true);
    fixture.processor.setVolume (1);
    fixture.processor.setOffsetSec (0);
    stream.seek (0, IBStream::kIBSeekSet, nullptr);
    fixture.processor.setState (&stream);
    check (fixture.processor.theme () == 1 && !fixture.processor.followScore (), "theme / follow state roundtrip");
    check (std::abs (fixture.processor.volume () - .37f) < 1e-5 && fixture.processor.offsetSec () == 1.25f,
           "current volume / offset state roundtrip");
    check (fixture.processor.scorePath () == score.u8string (), "score path state roundtrip");
    fixture.processor.setTheme (99);
    check (fixture.processor.theme () == ap::ui::themeCount - 1, "theme upper bound");
    fixture.processor.setTheme (-1);
    check (fixture.processor.theme () == 0, "theme lower bound");
    for (int i = 0; i < ap::ui::themeCount; ++i)
    {
        fixture.processor.setTheme (i);
        MemoryStream themeState;
        fixture.processor.getState (&themeState);
        fixture.processor.setTheme (0);
        themeState.seek (0, IBStream::kIBSeekSet, nullptr);
        fixture.processor.setState (&themeState);
        check (fixture.processor.theme () == i, "every palette survives state roundtrip");
    }
}

void transportTests (const std::filesystem::path& wave)
{
    Fixture fixture (false);
    auto& p = fixture.processor;
    check (p.loadFile (wave.u8string ()), "load WAV");
    check (fixture.block (0, 0) == 0, "paused host must be silent");
    check (fixture.block (0, ProcessContext::kPlaying) > 1, "render despite output silenceFlags");
    check (p.playing (), "play follows host");
    const double fullGain = fixture.block (48000, ProcessContext::kPlaying);
    p.setVolume (.25f);
    fixture.block (0, 0);
    const double quarterGain = fixture.block (48000, ProcessContext::kPlaying);
    check (std::abs (quarterGain / fullGain - .25) < .0001, "volume changes actual output gain");
    p.setVolume (0);
    check (fixture.block (48256, ProcessContext::kPlaying) == 0, "volume zero mutes output");
    p.setVolume (1);
    check (fixture.block (256, 0) == 0 && !p.playing (), "pause follows host");
    fixture.block (48000, ProcessContext::kPlaying);
    check (std::abs (p.positionSec () - (1 + 256. / 48000)) < 1e-5, "resume seeks to host");
    fixture.block (96000, ProcessContext::kPlaying);
    check (std::abs (p.positionSec () - (2 + 256. / 48000)) < 1e-5, "playing seek follows host");
    fixture.context.tempo = 90;
    fixture.context.projectTimeMusic = 100;
    fixture.context.timeSigNumerator = 7;
    fixture.context.timeSigDenominator = 8;
    fixture.block (96256, ProcessContext::kPlaying | ProcessContext::kTempoValid |
                   ProcessContext::kTimeSigValid | ProcessContext::kProjectTimeMusicValid, 128);
    const auto tl = p.hostTimeline ();
    check (tl.tempoValid && tl.bpm == 90 && tl.timeSigValid && tl.beatsPerBar == 7 &&
           tl.beatDenominator == 8, "host tempo / meter");
    check (std::abs (tl.playheadSec - 96256. / 48000) < 1e-8, "sample time authoritative over musical time");
    fixture.block (96384, ProcessContext::kPlaying, 512);
    check (std::abs (p.positionSec () - 96896. / 48000) < 1e-5, "variable blocks stay aligned");
    fixture.data.processContext = nullptr;
    fixture.processor.process (fixture.data);
    check (!p.playing (), "missing context stops playback");
    fixture.data.processContext = &fixture.context;

    fixture.block (48000, ProcessContext::kPlaying);
    p.setOffsetSec (.25f);
    p.setVolume (.37f);
    check (p.loadFile (wave.u8string ()), "reload audio during playback");
    fixture.block (48256, ProcessContext::kPlaying);
    check (std::abs (p.positionSec () - (48512. / 48000 + .25)) < 1e-5,
           "reloaded audio realigns with playing score");
    check (std::abs (p.volume () - .37f) < 1e-5 && p.offsetSec () == .25f,
           "audio replacement preserves volume and offset");

    Fixture generic (false);
    generic.block (0, 0); generic.block (256, 0); generic.block (512, 0);
    check (!generic.processor.hostTimeline ().playing, "generic paused host must not infer playback");

    Fixture muse (true);
    check (muse.processor.loadFile (wave.u8string ()), "MuseScore WAV load");
    muse.block (0, 0, 128); muse.block (128, 0, 512);
    check (!muse.processor.hostTimeline ().playing, "inference waits for advancing blocks");
    check (muse.block (640, 0, 256) > 1 && muse.processor.hostTimeline ().playing,
           "MuseScore without kPlaying renders after advancing timeline");
    check (std::abs (muse.processor.positionSec () - 896. / 48000) < 1e-5, "inferred playback aligned");
    muse.block (896, 0);
    check (muse.block (896, 0) == 0 && !muse.processor.playing (), "frozen score position pauses");
    muse.block (48000, 0); muse.block (48256, 0); muse.block (48512, 0);
    check (muse.processor.playing (), "inferred playback resumes");
    muse.block (96000, 0);
    check (std::abs (muse.processor.positionSec () - (2 + 256. / 48000)) < 1e-5, "inferred playing seek");
    muse.block (96256, ProcessContext::kPlaying);
    muse.block (96512, 0);
    check (!muse.processor.playing () && !muse.processor.hostTimeline ().inferredTransport,
           "explicit transport supersedes inference");
    muse.processor.setProcessing (false);
    check (!muse.processor.playing (), "lifecycle stop");
}

void liveScoreTests ()
{
    Fixture fixture (false);
    auto* backend = aplay::stableBackend ();
    auto grid = ap::resolveGridMetadata (*backend);
    check (grid.source == ap::GridMetadata::Source::Default && grid.bpm == 120 &&
           grid.beats == 4 && grid.denominator == 4, "missing host info retains default grid");
    std::vector<ap::BarMark> bars;
    ap::buildBars (0, 8, 0, 60.0 / grid.bpm, grid.beats, grid.denominator, grid.changes, bars);
    check (!bars.empty (), "missing host info retains bar numbers");

    backend->setTimeSigChange (6, 5, 4);
    backend->setLiveScoreMetadata (90, 7, 8, {{3, 3, 4}}, {{1, 1, 0}}, {{4, 60}});
    const auto tl = backend->hostTimeline ();
    check (tl.scoreSourceLive && tl.scoreBpm == 90 && tl.scoreBeats == 7 &&
           tl.scoreDenominator == 8 && tl.scoreTimeSigChanges.size () == 1 &&
           tl.scoreMeasures.size () == 1 && tl.scoreTempos.size () == 1,
           "proxy forwards live metadata / pickup duration / tempo changes");
    fixture.context.tempo = 150;
    fixture.context.timeSigNumerator = 4;
    fixture.context.timeSigDenominator = 4;
    fixture.block (0, ProcessContext::kTempoValid | ProcessContext::kTimeSigValid);
    grid = ap::resolveGridMetadata (*backend);
    check (grid.source == ap::GridMetadata::Source::LiveScore && grid.bpm == 90 &&
           grid.beats == 7 && grid.changes.size () == 1 && grid.measures.front ().quarters == 1 && grid.tempos.size () == 1,
           "opening score metadata authoritative for grid");
    backend->setFollowScore (false);
    backend->setGridBPM (110);
    grid = ap::resolveGridMetadata (*backend);
    check (grid.source == ap::GridMetadata::Source::Manual && grid.bpm == 110 &&
           grid.changes.size () == 1 && grid.changes.front ().bar == 6, "manual meter map remains independent");
    backend->setFollowScore (true);
    backend->setLiveScoreMetadata (132, 6, 8, {});
    grid = ap::resolveGridMetadata (*backend);
    check (grid.bpm == 132 && grid.beats == 6 && grid.changes.empty () && grid.measures.empty () && grid.tempos.empty (),
           "project switch replaces entire live map");
    backend->clearLiveScoreMetadata ();
    grid = ap::resolveGridMetadata (*backend);
    check (!backend->hostTimeline ().scoreSourceLive && grid.source == ap::GridMetadata::Source::Host &&
           grid.bpm == 150, "disconnected companion falls back to host");
    fixture.block (0, 0);
    grid = ap::resolveGridMetadata (*backend);
    check (grid.source == ap::GridMetadata::Source::Default && grid.bpm == 120,
           "disconnected companion and missing host retain grid");
}

void hiddenControlTests (const std::filesystem::path& wave)
{
    Fixture fixture (false);
    auto& backend = fixture.processor;
    check (backend.loadFile (wave.u8string ()), "control test audio");
    backend.setTheme (0);
    // The test host is never shown and never interacts with the user's desktop.
    HWND host = CreateWindowExW (0, L"STATIC", L"MuseTone hidden test", WS_OVERLAPPED,
                                 0, 0, 1000, 700, nullptr, nullptr, GetModuleHandleW (nullptr), nullptr);
    check (host != nullptr, "hidden host creation");
    auto* view = new ap::PlugView (&backend);
    check (view->attached (host, kPlatformTypeHWND) == kResultOk, "attach hidden editor");
    HWND panel = FindWindowExW (host, nullptr, L"MuseToneMainView", nullptr);
    check (panel != nullptr, "editor panel exists");
    HWND slider = GetDlgItem (panel, 1012);
    check (slider != nullptr, "volume slider is a real window");
    RECT r {};
    SendMessageW (slider, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM> (&r));
    check (r.right > r.left && r.bottom > r.top, "volume channel has nonzero geometry");
    SendMessageW (slider, TBM_SETPOS, TRUE, 28);
    SendMessageW (panel, WM_HSCROLL, TB_THUMBPOSITION, reinterpret_cast<LPARAM> (slider));
    check (std::abs (backend.volume () - .28f) < .0001, "slider changes backend volume");

    HWND progress = GetDlgItem (panel, 1032);
    check (progress && IsWindowEnabled (progress), "loaded audio enables playback progress");
    backend.setOffsetSec (.25f);
    fixture.block (48000, ProcessContext::kPlaying);
    SendMessageW (panel, WM_TIMER, 1200, 0);
    check (std::abs (SendMessageW (progress, TBM_GETPOS, 0, 0) -
           std::lround (backend.positionSec () / backend.durationSec () * 10000)) <= 1,
           "progress follows actual playback position");
    {
        TestSeekBridge bridge;
        GetClientRect (progress, &r);
        SendMessageW (progress, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM (r.right / 2, r.bottom / 2));
        check (bridge.command ().empty (), "scrubbing previews without sending commands");
        SendMessageW (panel, WM_TIMER, 1200, 0);
        check (std::abs (SendMessageW (progress, TBM_GETPOS, 0, 0) - 5000) < 40,
               "timer does not overwrite scrub preview");
        savePreview (progress, L"progress-light.bmp");
        SendMessageW (progress, WM_LBUTTONUP, 0, MAKELPARAM (r.right / 2, r.bottom / 2));
        check (std::abs (bridge.target () - 1.75) < .02, "progress midpoint seek subtracts audio offset");
        const auto previous = bridge.command ();
        SendMessageW (progress, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM (r.right, r.bottom / 2));
        SendMessageW (progress, WM_KEYDOWN, VK_ESCAPE, 0);
        check (bridge.command () == previous && GetCapture () != progress,
               "Escape cancels scrub without a jump");
        SendMessageW (progress, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM (0, r.bottom / 2));
        SendMessageW (progress, WM_CAPTURECHANGED, 0, 0);
        check (bridge.command () == previous, "lost capture cancels scrub");
        SendMessageW (progress, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM (r.right / 2, r.bottom / 2));
        SendMessageW (progress, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM (r.right + 20, r.bottom / 2));
        SendMessageW (progress, WM_LBUTTONUP, 0, MAKELPARAM (r.right + 20, r.bottom / 2));
        check (std::abs (bridge.target () - 3.75) < 1e-6, "progress drag clamps to duration");
        SendMessageW (progress, WM_KEYDOWN, VK_HOME, 0);
        check (bridge.target () == 0, "progress Home clamps negative score time to zero");
        SendMessageW (progress, WM_KEYDOWN, VK_END, 0);
        check (std::abs (bridge.target () - 3.75) < 1e-6, "progress End seeks to audio end");
        backend.setOffsetSec (-.5f);
        SendMessageW (progress, WM_KEYDOWN, VK_HOME, 0);
        check (std::abs (bridge.target () - .5) < 1e-6, "progress seek handles negative offset");
    }
    backend.setOffsetSec (0);
    backend.setLiveScoreMetadata (120, 4, 4, {{2, 3, 4}}, {}, {{2, 60}, {5, 180}});
    fixture.block (192000, 0);
    SendMessageW (panel, WM_TIMER, 1200, 0);
    wchar_t currentBpm[32] {};
    GetWindowTextW (GetDlgItem (panel, 1008), currentBpm, 32);
    check (std::wcstod (currentBpm, nullptr) == 180, "BPM field follows tempo at host playhead");
    savePreview (panel, L"tempo-meter-change.bmp");
    fixture.block (0, 0);
    SendMessageW (panel, WM_TIMER, 1200, 0);
    GetWindowTextW (GetDlgItem (panel, 1008), currentBpm, 32);
    check (std::wcstod (currentBpm, nullptr) == 120, "seeking backward restores opening tempo");
    backend.setLiveScoreMetadata (96, 6, 8, {});
    SendMessageW (panel, WM_TIMER, 1200, 0);
    GetWindowTextW (GetDlgItem (panel, 1008), currentBpm, 32);
    check (std::wcstod (currentBpm, nullptr) == 96, "editing live metadata refreshes tempo without reopening editor");
    backend.setLiveScoreMetadata (120, 4, 4, {});
    fixture.block (6960000, 0);
    SendMessageW (panel, WM_TIMER, 1200, 0);
    savePreview (panel, L"measure-73-3.bmp");
    backend.clearLiveScoreMetadata ();
    fixture.block (48000, 0);
    SendMessageW (panel, WM_TIMER, 1200, 0);
    HWND previewWave = FindWindowExW (panel, nullptr, L"MuseToneWaveView", nullptr);
    checkWavePaint (previewWave, 0);
    savePreview (panel, L"playback-light.bmp");
    backend.setTheme (1);
    SendMessageW (panel, WM_TIMER, 1200, 0);
    checkWavePaint (previewWave, 1);
    savePreview (panel, L"playback-dark.bmp");
    struct PromptCheck
    {
        HWND panel;
        unsigned paper;
        bool found = false, matches = false, controls = false, emptyRejected = false;
    };
    static PromptCheck* promptCheck = nullptr;
    for (int theme = 0; theme < ap::ui::themeCount; ++theme)
    {
        backend.setTheme (theme);
        SendMessageW (panel, WM_TIMER, 1200, 0);
        PromptCheck state {panel, ap::ui::themes[theme].paper};
        promptCheck = &state;
        const UINT_PTR timer = SetTimer (nullptr, 0, 10, [] (HWND, UINT, UINT_PTR timerId, DWORD) {
            HWND prompt = FindWindowExW (nullptr, nullptr, L"MuseSyncVideoUrlPrompt", nullptr);
            if (!prompt) return;
            KillTimer (nullptr, timerId);
            auto& state = *promptCheck;
            state.found = true;
            HDC dc = CreateCompatibleDC (nullptr);
            HDC windowDc = GetDC (prompt);
            HBITMAP bitmap = CreateCompatibleBitmap (windowDc, 16, 16);
            ReleaseDC (prompt, windowDc);
            const auto old = SelectObject (dc, bitmap);
            SendMessageW (prompt, WM_PRINTCLIENT, reinterpret_cast<WPARAM> (dc), PRF_CLIENT);
            const COLORREF expected = RGB ((state.paper >> 16) & 255, (state.paper >> 8) & 255, state.paper & 255);
            state.matches = GetPixel (dc, 8, 8) == expected;
            SelectObject (dc, old);
            DeleteObject (bitmap);
            DeleteDC (dc);
            HWND edit = GetDlgItem (prompt, 1), open = GetDlgItem (prompt, 2);
            state.controls = edit && open && !IsWindowEnabled (state.panel) &&
                SendMessageW (edit, WM_GETFONT, 0, 0) == SendMessageW (GetDlgItem (state.panel, 1001), WM_GETFONT, 0, 0) &&
                (GetWindowLongPtrW (open, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW &&
                !(GetWindowLongPtrW (prompt, GWL_STYLE) & WS_CAPTION) && !IsWindowEnabled (open);
            SetWindowTextW (edit, L"   ");
            SendMessageW (prompt, WM_COMMAND, 2, 0);
            state.emptyRejected = IsWindow (prompt) && !IsWindowEnabled (open);
            SetWindowTextW (edit, L"https://www.bilibili.com/video/test");
            state.controls = state.controls && IsWindowEnabled (open);
            savePreview (prompt, (L"video-url-theme-" + std::to_wstring (state.paper) + L".bmp").c_str ());
            PostMessageW (edit, WM_KEYDOWN, VK_ESCAPE, 0);
        });
        SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1402, BN_CLICKED), 0);
        KillTimer (nullptr, timer);
        promptCheck = nullptr;
        check (state.found && state.matches, "video URL prompt follows each plugin theme");
        check (state.controls && state.emptyRejected && IsWindowEnabled (panel),
            "video URL prompt uses shared fonts, themed buttons, validation and Escape cancellation");
        check (backend.hasAudio (), "cancelling video URL preserves loaded audio");
    }
    backend.setTheme (0);
    SendMessageW (panel, WM_TIMER, 1200, 0);

    auto findEdit = [&] (int id) { return GetDlgItem (panel, id); };
    // IDs are appended to the existing public Win32 command sequence.
    HWND offset = findEdit (1025);
    check (offset && !findEdit (1026), "millisecond input exists; volume is not an edit");
    SetWindowTextW (offset, L"-1250");
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1025, EN_KILLFOCUS), reinterpret_cast<LPARAM> (offset));
    check (backend.offsetSec () == -1.25f, "signed millisecond input updates seconds backend");
    SetWindowTextW (offset, L"bad");
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1025, EN_KILLFOCUS), reinterpret_cast<LPARAM> (offset));
    check (backend.offsetSec () == -1.25f, "invalid offset does not silently reset to zero");
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1028, BN_CLICKED), 0);
    check (std::abs (backend.offsetSec () + 1.24f) < 1e-5, "offset plus button adds 10 ms");
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1027, BN_CLICKED), 0);
    check (std::abs (backend.offsetSec () + 1.25f) < 1e-5, "offset minus button subtracts 10 ms");
    backend.setOffsetSec (3600);
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1028, BN_CLICKED), 0);
    check (backend.offsetSec () == 3600, "offset plus button clamps at upper bound");
    backend.setOffsetSec (-3600);
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1027, BN_CLICKED), 0);
    check (backend.offsetSec () == -3600, "offset minus button clamps at lower bound");
    backend.setOffsetSec (-1.25f);
    auto wheel = [&] (HWND control, int delta, int modifiers = 0) {
        SendMessageW (control, WM_MOUSEWHEEL, MAKEWPARAM (modifiers, static_cast<WORD> (delta)), 0);
    };
    SetWindowTextW (offset, L"-1250");
    wheel (offset, 120);
    check (std::abs (backend.offsetSec () + 1.24f) < 1e-5, "offset wheel adds 10 ms");
    wheel (offset, -120, MK_SHIFT);
    check (std::abs (backend.offsetSec () + 1.241f) < 1e-5, "Shift wheel subtracts 1 ms");
    wheel (offset, 120, MK_CONTROL);
    check (std::abs (backend.offsetSec () + 1.141f) < 1e-5, "Ctrl wheel adds 100 ms");
    SetFocus (offset);
    SendMessageW (offset, WM_CHAR, VK_SPACE, 0);
    wchar_t clean[64] {};
    GetWindowTextW (offset, clean, 64);
    check (!std::wcschr (clean, L' '), "numeric edits swallow translated space characters");
    SetWindowTextW (offset, L"-1250");
    SendMessageW (offset, WM_KEYDOWN, VK_RETURN, 0);
    SendMessageW (offset, WM_CHAR, VK_RETURN, 0);
    check (backend.offsetSec () == -1.25f,
        "Enter commits offset and consumes the translated character");
    HWND semitones = findEdit (1014), cents = findEdit (1017);
    SetWindowTextW (semitones, L"2");
    check (backend.semitones () == 2, "typing +2 commits without leaving field");
    wheel (semitones, 60);
    check (backend.semitones () == 2, "fractional wheel delta retained");
    wheel (semitones, 60);
    check (backend.semitones () == 3, "semitone wheel accumulates partial notches");
    SetWindowTextW (cents, L"99");
    wheel (cents, 240);
    check (backend.cents () == 100, "cents wheel clamps at upper bound");
    wheel (cents, -120);
    check (backend.cents () == 99, "cents wheel down");
    SetWindowTextW (semitones, L"invalid");
    check (backend.semitones () == 3, "invalid pitch input does not reset pitch");
    SetWindowTextW (semitones, L"3");
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1023, BN_CLICKED), 0);
    HWND bpm = findEdit (1008);
    SetWindowTextW (bpm, L"120");
    wheel (bpm, 120);
    check (backend.gridBPM () == 121, "manual BPM supports wheel");
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1023, BN_CLICKED), 0);
    wheel (bpm, 120);
    check (backend.gridBPM () == 121, "score-follow BPM cannot be overwritten by wheel");

    HWND meter = findEdit (1011);
    check (meter != nullptr && !IsWindowEnabled (meter), "followed meter remains read-only");
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1023, BN_CLICKED), 0);
    check (IsWindowEnabled (meter), "manual meter retains native combo input");
    SendMessageW (meter, CB_SETCURSEL, 4, 0);
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1011, CBN_SELCHANGE), reinterpret_cast<LPARAM> (meter));
    check (backend.gridBeatsPerBar () == 6 && backend.gridBeatDenominator () == 8,
           "restyled meter still applies manual selection");
    SendMessageW (meter, WM_KEYDOWN, VK_DOWN, 0);
    SendMessageW (meter, WM_KEYUP, VK_DOWN, 0);
    check (backend.gridBeatsPerBar () == 7 && backend.gridBeatDenominator () == 8,
           "restyled meter retains arrow-key selection");
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1023, BN_CLICKED), 0);
    SendMessageW (panel, WM_TIMER, 1200, 0);

    GetClientRect (meter, &r);
    HDC screen = GetDC (nullptr);
    HDC memory = CreateCompatibleDC (screen);
    HBITMAP bitmap = CreateCompatibleBitmap (screen, r.right, r.bottom);
    ReleaseDC (nullptr, screen);
    check (memory && bitmap, "in-memory meter paint target");
    HGDIOBJ previousBitmap = SelectObject (memory, bitmap);
    SendMessageW (meter, WM_PRINTCLIENT, reinterpret_cast<WPARAM> (memory), PRF_CLIENT);
    check (GetPixel (memory, r.right - 3, r.bottom / 2) == RGB (255, 255, 255),
           "light meter face follows the light surface color");
    backend.setTheme (1);
    SendMessageW (panel, WM_TIMER, 1200, 0);
    SendMessageW (meter, WM_PRINTCLIENT, reinterpret_cast<WPARAM> (memory), PRF_CLIENT);
    check (GetPixel (memory, r.right - 3, r.bottom / 2) == RGB (32, 34, 37),
           "dark meter arrow area has no white native button");
    backend.setTheme (2);
    SendMessageW (panel, WM_TIMER, 1200, 0);
    SendMessageW (meter, WM_PRINTCLIENT, reinterpret_cast<WPARAM> (memory), PRF_CLIENT);
    check (GetPixel (memory, r.right - 3, r.bottom / 2) == RGB (18, 18, 18),
           "black meter face follows its surface color");

    const auto compact = ap::ui::arrange (ap::ui::minWidth, ap::ui::minHeight);
    SelectObject (memory, reinterpret_cast<HFONT> (SendMessageW (bpm, WM_GETFONT, 0, 0)));
    SIZE textSize {};
    const wchar_t* longestOffset = L"-3600000";
    GetTextExtentPoint32W (memory, longestOffset, 8, &textSize);
    const double scale = static_cast<double> (r.bottom) / compact.timeSig.h;
    for (int id : {1001, 1035})
    {
        HWND button = GetDlgItem (panel, id);
        RECT bounds {};
        GetClientRect (button, &bounds);
        SelectObject (memory, reinterpret_cast<HFONT> (SendMessageW (button, WM_GETFONT, 0, 0)));
        wchar_t label[32] {};
        GetWindowTextW (button, label, 32);
        GetTextExtentPoint32W (memory, label, static_cast<int> (std::wcslen (label)), &textSize);
        check (textSize.cx + 12 * scale <= bounds.right, "full import label fits button with padding");
    }
    SelectObject (memory, reinterpret_cast<HFONT> (SendMessageW (bpm, WM_GETFONT, 0, 0)));
    GetTextExtentPoint32W (memory, longestOffset, 8, &textSize);
    check (textSize.cx <= (compact.offset.w - 52) * scale,
           "signed maximum offset fits the compact input");
    const wchar_t* tempo = L"400.0";
    GetTextExtentPoint32W (memory, tempo, 5, &textSize);
    check (textSize.cx <= (compact.bpm.w - 62) * scale,
           "automatic BPM decimal fits at minimum width");
    SelectObject (memory, previousBitmap);
    DeleteObject (bitmap);
    DeleteDC (memory);
    backend.setTheme (0);
    SendMessageW (panel, WM_TIMER, 1200, 0);

    GetClientRect (slider, &r);
    SendMessageW (slider, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM (r.right / 2, r.bottom / 2));
    SendMessageW (slider, WM_LBUTTONUP, 0, MAKELPARAM (r.right / 2, r.bottom / 2));
    check (std::abs (backend.volume () - .5f) < .02, "clicking volume bar sets volume directly");
    SendMessageW (slider, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM (0, r.bottom / 2));
    SendMessageW (slider, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM (r.right + 20, r.bottom / 2));
    SendMessageW (slider, WM_LBUTTONUP, 0, MAKELPARAM (r.right + 20, r.bottom / 2));
    check (backend.volume () == 1, "volume drag clamps at 100 percent");
    wheel (slider, -120);
    check (std::abs (backend.volume () - .99f) < 1e-5, "volume wheel changes gain");
    SendMessageW (slider, WM_KEYDOWN, VK_LEFT, 0);
    SendMessageW (slider, WM_KEYUP, VK_LEFT, 0);
    check (std::abs (backend.volume () - .98f) < 1e-5, "volume keyboard left changes gain");
    HWND waveform = FindWindowExW (panel, nullptr, L"MuseToneWaveView", nullptr);
    const float beforeDrag = backend.offsetSec ();
    SendMessageW (waveform, WM_LBUTTONDOWN, MK_CONTROL, MAKELPARAM (100, 60));
    SendMessageW (waveform, WM_MOUSEMOVE, MK_CONTROL | MK_LBUTTON, MAKELPARAM (160, 60));
    SendMessageW (waveform, WM_LBUTTONUP, 0, MAKELPARAM (160, 60));
    wchar_t text[64] {};
    GetWindowTextW (offset, text, 64);
    check (backend.offsetSec () > beforeDrag, "Ctrl drag changes offset rather than panning the view");
    check (std::abs (std::wcstod (text, nullptr) - backend.offsetSec () * 1000.0) <= .51,
           "Ctrl drag immediately updates millisecond input");

    for (int i = 0; i < ap::ui::themeCount; ++i)
    {
        backend.setTheme (i);
        SendMessageW (panel, WM_TIMER, 1200, 0);
        GetClientRect (meter, &r);
        screen = GetDC (nullptr);
        memory = CreateCompatibleDC (screen);
        bitmap = CreateCompatibleBitmap (screen, r.right, r.bottom);
        ReleaseDC (nullptr, screen);
        previousBitmap = SelectObject (memory, bitmap);
        SendMessageW (meter, WM_PRINTCLIENT, reinterpret_cast<WPARAM> (memory), PRF_CLIENT);
        const auto rgb = ap::ui::themes[i].surface;
        check (GetPixel (memory, r.right - 3, r.bottom / 2) ==
               RGB ((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255), "all theme surfaces paint correctly");
        SendMessageW (progress, TBM_SETPOS, FALSE, 5000);
        SendMessageW (progress, WM_PRINTCLIENT, reinterpret_cast<WPARAM> (memory), PRF_CLIENT);
        RECT progressRect {};
        GetClientRect (progress, &progressRect);
        const auto accent = ap::ui::themes[i].accent;
        check (GetPixel (memory, 15, progressRect.bottom / 2) ==
               RGB ((accent >> 16) & 255, (accent >> 8) & 255, accent & 255),
               "progress fill paints with each theme accent");
        SelectObject (memory, previousBitmap);
        DeleteObject (bitmap);
        DeleteDC (memory);
    }
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1024, BN_CLICKED), 0);
    savePreview (panel, L"guide-midnight.bmp");
    check (!(GetWindowLongPtrW (waveform, GWL_STYLE) & WS_VISIBLE), "guide hides waveform");
    check ((GetWindowLongPtrW (GetDlgItem (panel, 1033), GWL_STYLE) & WS_VISIBLE) != 0,
           "guide shows check update button");
    check (!IsWindowEnabled (GetDlgItem (panel, 1034)), "download disabled until update is available");
    SendMessageW (panel, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM (100, 100));
    check (!(GetWindowLongPtrW (waveform, GWL_STYLE) & WS_VISIBLE), "guide body click does not dismiss it");
    SendMessageW (panel, WM_KEYDOWN, VK_ESCAPE, 0);
    check ((GetWindowLongPtrW (waveform, GWL_STYLE) & WS_VISIBLE) != 0, "Escape closes guide");
    check (!(GetWindowLongPtrW (GetDlgItem (panel, 1033), GWL_STYLE) & WS_VISIBLE), "closing guide hides updater controls");
    backend.setTheme (0);
    SendMessageW (panel, WM_TIMER, 1200, 0);
    ViewRect compactSize {0, 0, 760, 500};
    view->onSize (&compactSize);
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1024, BN_CLICKED), 0);
    savePreview (panel, L"guide-light-compact.bmp");
    RECT panelRect {};
    GetClientRect (panel, &panelRect);
    const auto helpLayout = ap::ui::arrangeGuide (panelRect.right, panelRect.bottom);
    SendMessageW (panel, WM_LBUTTONDOWN, MK_LBUTTON,
                  MAKELPARAM (helpLayout.close.x + 16, helpLayout.close.y + 16));
    check ((GetWindowLongPtrW (waveform, GWL_STYLE) & WS_VISIBLE) != 0, "guide close icon restores controls");
    savePreview (panel, L"playback-light-compact.bmp");
    const float savedOffset = backend.offsetSec ();
    auto checkFloatingPosition = [&] (HWND strip) {
        RECT collapse {}, expand {};
        GetWindowRect (GetDlgItem (panel, 1029), &collapse);
        GetWindowRect (GetDlgItem (strip, 1030), &expand);
        const int unit = collapse.right - collapse.left;
        auto px = [unit] (int value) { return static_cast<int> (std::lround (value * unit / 32.0)); };
        MONITORINFO monitor {sizeof monitor};
        GetMonitorInfoW (MonitorFromRect (&collapse, MONITOR_DEFAULTTONEAREST), &monitor);
        const int expectedX = std::clamp ((collapse.left + collapse.right) / 2 - px (220) - px (32) / 2,
            monitor.rcWork.left, std::max (monitor.rcWork.left, monitor.rcWork.right - px (260)));
        const int expectedY = std::clamp ((collapse.top + collapse.bottom) / 2 - px (11) - px (36) / 2,
            monitor.rcWork.top, std::max (monitor.rcWork.top, monitor.rcWork.bottom - px (58)));
        check (std::abs (expand.left - expectedX - px (220)) <= 2 &&
               std::abs (expand.top - expectedY - px (11)) <= 2,
               "floating expand aligns with collapse button and stays on monitor");
    };
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1029, BN_CLICKED), 0);
    HWND floating = FindWindowExW (nullptr, nullptr, L"MuseToneFloatingView", nullptr);
    check (floating && GetDlgItem (floating, 1030), "collapse creates floating strip with expand control");
    wchar_t expandLabel[64] {};
    GetWindowTextW (GetDlgItem (floating, 1030), expandLabel, 64);
    check (std::wcscmp (expandLabel, L"展开 MuseSync") == 0, "floating expand control uses MuseSync brand");
    checkFloatingPosition (floating);
    check (!(GetWindowLongPtrW (panel, GWL_STYLE) & WS_VISIBLE), "collapsed editor panel is hidden");
    check (!IsWindowVisible (floating), "hidden regression host never shows floating UI");
    check (SendMessageW (floating, WM_NCHITTEST, 0, 0) == HTCAPTION, "floating strip is draggable");
    RECT expandRect {};
    GetWindowRect (GetDlgItem (floating, 1030), &expandRect);
    check (SendMessageW (floating, WM_NCHITTEST, 0,
                        MAKELPARAM (expandRect.left + 4, expandRect.top + 4)) == HTCLIENT,
           "expand button is excluded from drag hit test");
    SendMessageW (panel, WM_TIMER, 1200, 0);
    backend.setOffsetSec (0);
    backend.setLiveScoreMetadata (120, 4, 4, {});
    fixture.block (6960000, 0);
    SendMessageW (panel, WM_TIMER, 1200, 0);
    savePreview (floating, L"floating-light.bmp");
    backend.clearLiveScoreMetadata ();
    backend.setOffsetSec (savedOffset);
    check (backend.offsetSec () == savedOffset && backend.hasAudio (), "collapse preserves audio and offset");
    backend.setOffsetSec (0);
    fixture.block (0, 0);
    check (fixture.block (48000, ProcessContext::kPlaying) > 0,
           "collapsed editor continues actual audio output");
    backend.setOffsetSec (savedOffset);
    SendMessageW (floating, WM_COMMAND, MAKEWPARAM (1030, BN_CLICKED), 0);
    check ((GetWindowLongPtrW (panel, GWL_STYLE) & WS_VISIBLE) != 0, "expand restores panel");
    SetWindowPos (host, nullptr, -500, -300, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1029, BN_CLICKED), 0);
    checkFloatingPosition (floating);
    view->removed ();
    check (!IsWindow (floating), "removing collapsed editor destroys floating window");
    view->release ();
    DestroyWindow (host);

    // An owned hidden dialog models MuseScore's editor without showing a window.
    HWND application = CreateWindowExW (0, L"STATIC", L"Hidden MuseScore application", WS_OVERLAPPED,
                                        0, 0, 1000, 700, nullptr, nullptr, GetModuleHandleW (nullptr), nullptr);
    HWND editor = CreateWindowExW (0, L"STATIC", L"MuseSync", WS_OVERLAPPED,
                                   0, 0, 1000, 700, application, nullptr, GetModuleHandleW (nullptr), nullptr);
    auto* ownedView = new ap::PlugView (&backend);
    check (ownedView->attached (editor, kPlatformTypeHWND) == kResultOk, "attach owned editor shell");
    HWND ownedPanel = FindWindowExW (editor, nullptr, L"MuseToneMainView", nullptr);
    savePreview (GetDlgItem (ownedPanel, 1032), L"progress-restored.bmp");
    SendMessageW (ownedPanel, WM_COMMAND, MAKEWPARAM (1029, BN_CLICKED), 0);
    floating = FindWindowExW (nullptr, nullptr, L"MuseToneFloatingView", nullptr);
    check (floating && GetWindow (floating, GW_OWNER) == application,
           "floating strip is owned by application, not hidden editor shell");
    SendMessageW (floating, WM_CLOSE, 0, 0);
    check ((GetWindowLongPtrW (ownedPanel, GWL_STYLE) & WS_VISIBLE) != 0,
           "closing floating strip restores editor");
    check (!IsWindowVisible (application) && !IsWindowVisible (editor), "hidden owner stays hidden");
    backend.unloadFile ();
    SendMessageW (ownedPanel, WM_TIMER, 1200, 0);
    check (!IsWindowEnabled (GetDlgItem (ownedPanel, 1032)) &&
           SendMessageW (GetDlgItem (ownedPanel, 1032), TBM_GETPOS, 0, 0) == 0,
           "unloaded audio disables and clears progress");
    ownedView->removed ();
    ownedView->release ();
    DestroyWindow (editor);
    DestroyWindow (application);

    for (const wchar_t* name : {L"MuseTone", L"MuseSync"})
    {
        HWND shell = CreateWindowExW (0, L"STATIC", name, WS_OVERLAPPED,
            0, 0, 1000, 700, nullptr, nullptr, GetModuleHandleW (nullptr), nullptr);
        auto* shellView = new ap::PlugView (&backend);
        check (shellView->attached (shell, kPlatformTypeHWND) == kResultOk, "attach branded editor shell");
        HWND shellPanel = FindWindowExW (shell, nullptr, L"MuseToneMainView", nullptr);
        SendMessageW (shellPanel, WM_COMMAND, MAKEWPARAM (1029, BN_CLICKED), 0);
        HWND strip = FindWindowExW (nullptr, nullptr, L"MuseToneFloatingView", nullptr);
        check (strip && !GetWindow (strip, GW_OWNER), "old and new standalone editor titles are recognized");
        SendMessageW (strip, WM_COMMAND, MAKEWPARAM (1030, BN_CLICKED), 0);
        check ((GetWindowLongPtrW (shellPanel, GWL_STYLE) & WS_VISIBLE) != 0, "branded shell expands");
        shellView->removed ();
        shellView->release ();
        DestroyWindow (shell);
    }
}

void videoAudioTests ()
{
    Fixture fixture (false);
    auto stream = std::make_shared<ap::VideoAudioStream> ();
    fixture.processor.setVideoAudio (stream);
    stream->synchronize (true, true);
    std::vector<float> pcm (4096, .25f);
    auto push = [&] { stream->push (pcm.data (), 2048, 48000, stream->generation ()); };
    push ();
    check (fixture.block (0, ProcessContext::kPlaying) > 60, "video PCM enters VST output");
    fixture.processor.setVolume (0);
    push ();
    check (fixture.block (256, ProcessContext::kPlaying) == 0, "plugin volume mutes video PCM");
    fixture.processor.setVolume (1);
    check (fixture.block (512, 0) == 0, "host pause silences and drains video PCM");
    const auto oldGeneration = stream->generation ();
    stream->synchronize (true, true);
    stream->push (pcm.data (), 2048, 48000, oldGeneration);
    check (fixture.block (48000, ProcessContext::kPlaying) == 0, "seek rejects stale browser packets");
    stream->push (pcm.data (), 2048, 44100, stream->generation ());
    check (fixture.block (48256, ProcessContext::kPlaying) > 60, "video PCM resamples to host rate");
    double left[256] {}, right[256] {};
    double* channels[] {left, right};
    fixture.data.symbolicSampleSize = kSample64;
    fixture.bus.channelBuffers64 = channels;
    fixture.context.projectTimeSamples += 256;
    fixture.processor.process (fixture.data);
    check (left[100] > .24 && right[100] > .24, "video PCM supports double precision hosts");
    stream->synchronize (true, true);
    std::vector<float> largePcm (8192, .5f);
    stream->push (largePcm.data (), 4096, 48000, stream->generation ());
    std::vector<double> largeLeft (2048), largeRight (2048);
    double* largeChannels[] {largeLeft.data (), largeRight.data ()};
    fixture.bus.channelBuffers64 = largeChannels;
    fixture.data.numSamples = 2048;
    fixture.context.projectTimeSamples += 256;
    fixture.processor.process (fixture.data);
    check (largeLeft[100] > .49 && largeLeft[1500] > .49 && largeRight[1500] > .49,
        "large double precision blocks retain audio across chunks");
    fixture.processor.setVideoAudio (nullptr);

    ap::VideoAudioStream ring;
    ring.synchronize (true, true);
    std::vector<float> ringInput (8000, .3f), ringLeft (4000), ringRight (4000);
    for (int i = 0; i < 8; ++i)
    {
        ring.push (ringInput.data (), 4000, 48000, ring.generation ());
        ring.render (ringLeft.data (), ringRight.data (), 4000, 48000, 1, true);
        check (ringLeft[3000] > .29f && ringRight[3000] > .29f,
            "video PCM remains continuous across ring buffer wrap");
    }
}

void tempoControlTests (const std::filesystem::path& wave)
{
    writeWave (wave, false, true);
    Fixture fixture (false);
    auto& backend = fixture.processor;
    check (backend.loadFile (wave.u8string ()), "tempo fixture imports through audio decoder");
    const auto deadline = GetTickCount64 () + 15000;
    while (backend.tempoAnalyzer ()->snapshot ()->state == ap::TempoResult::State::Analyzing &&
           GetTickCount64 () < deadline) Sleep (10);
    const auto result = backend.tempoAnalyzer ()->snapshot ();
    check (result->state == ap::TempoResult::State::Ready &&
           std::abs (result->bpmAt (5) - 90) < 4 && std::abs (result->bpmAt (19) - 150) < 4,
           "imported WAV publishes variable material BPM");
    HWND host = CreateWindowExW (0, L"STATIC", L"Hidden BPM test", WS_OVERLAPPED,
        0, 0, 1000, 700, nullptr, nullptr, GetModuleHandleW (nullptr), nullptr);
    auto* view = new ap::PlugView (&backend);
    check (view->attached (host, kPlatformTypeHWND) == kResultOk, "attach tempo editor");
    HWND panel = FindWindowExW (host, nullptr, L"MuseToneMainView", nullptr);
    for (const auto size : {ap::ui::Rect {0, 0, 1000, 700}, ap::ui::Rect {0, 0, 760, 500}})
    {
        ViewRect bounds {0, 0, size.w, size.h};
        view->onSize (&bounds);
        fixture.block (19 * 48000, 0);
        SendMessageW (panel, WM_TIMER, 1200, 0);
        savePreview (panel, (L"material-bpm-" + std::to_wstring (size.w) + L".bmp").c_str ());
    }
    view->removed ();
    view->release ();
    DestroyWindow (host);
}

void browserAudioTests ()
{
    wchar_t path[32768] {};
    if (!GetEnvironmentVariableW (L"MUSESYNC_TEST_VIDEO", path, 32768)) return;
    Fixture fixture (false);
    ap::VideoView video;
    check (video.openLocal (path), "open browser PCM test video");
    HWND host = CreateWindowExW (0, L"STATIC", L"MuseSync browser audio test", WS_OVERLAPPEDWINDOW,
        100, 100, 640, 400, nullptr, nullptr, GetModuleHandleW (nullptr), nullptr);
    video.present (host, 0, 0, 620, 360, true);
    fixture.processor.setVideoAudio (video.audioStream ());
    const auto start = GetTickCount64 ();
    double energy = 0;
    while (GetTickCount64 () - start < 10000 && energy < 1)
    {
        MSG message {};
        while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage (&message);
            DispatchMessageW (&message);
        }
        const double seconds = (GetTickCount64 () - start) / 1000.0;
        video.sync (std::fmod (seconds, 3.0), true);
        energy += fixture.block (static_cast<int64> (seconds * 48000), ProcessContext::kPlaying, 960);
        MsgWaitForMultipleObjects (0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
    std::printf ("Browser video VST energy: %.3f\n", energy);
    check (energy > 1, "actual browser video audio reaches host mixer bus");
    video.close ();
    DestroyWindow (host);
    fixture.processor.setVideoAudio (nullptr);
}

void videoControlTests (const std::filesystem::path& wave)
{
    wchar_t videoPath[32768] {};
    if (!GetEnvironmentVariableW (L"MUSESYNC_TEST_VIDEO", videoPath, 32768)) return;
    Fixture fixture (false);
    auto& backend = fixture.processor;
    check (backend.loadFile (wave.u8string ()), "video test starts with audio");
    HWND host = CreateWindowExW (0, L"STATIC", L"MuseSync video test", WS_OVERLAPPED,
        0, 0, 1000, 700, nullptr, nullptr, GetModuleHandleW (nullptr), nullptr);
    auto* view = new ap::PlugView (&backend);
    check (view->attached (host, kPlatformTypeHWND) == kResultOk, "attach video test editor");
    HWND panel = FindWindowExW (host, nullptr, L"MuseToneMainView", nullptr);
    const bool visible = GetEnvironmentVariableW (L"MUSESYNC_TEST_VISIBLE", nullptr, 0) != 0;
    if (visible)
    {
        ViewRect size {0, 0, 760, 500};
        view->onSize (&size);
        SetWindowPos (host, HWND_TOPMOST, 0, 0, 760, 500, SWP_NOACTIVATE);
        ShowWindow (host, SW_SHOWNOACTIVATE);
    }
    auto waitUntil = [&] (auto predicate) {
        const auto deadline = GetTickCount64 () + 15000;
        do
        {
            MSG message {};
            while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage (&message);
                DispatchMessageW (&message);
            }
            SendMessageW (panel, WM_TIMER, 1200, 0);
            if (predicate ()) return true;
            MsgWaitForMultipleObjects (0, nullptr, FALSE, 20, QS_ALLINPUT);
        } while (GetTickCount64 () < deadline);
        return false;
    };
    auto dropFile = [&] (const std::wstring& path) {
        const size_t bytes = sizeof (DROPFILES) + (path.size () + 2) * sizeof (wchar_t);
        HGLOBAL memory = GlobalAlloc (GHND, bytes);
        auto* drop = static_cast<DROPFILES*> (GlobalLock (memory));
        drop->pFiles = sizeof (DROPFILES);
        drop->fWide = TRUE;
        std::memcpy (reinterpret_cast<char*> (drop) + sizeof (DROPFILES),
            path.c_str (), (path.size () + 1) * sizeof (wchar_t));
        GlobalUnlock (memory);
        SendMessageW (panel, WM_DROPFILES, reinterpret_cast<WPARAM> (memory), 0);
    };
    dropFile (videoPath);
    HWND progress = GetDlgItem (panel, 1032);
    check (!backend.hasAudio () && backend.currentPath ().empty (), "video import unloads audio");
    check (waitUntil ([&] { return IsWindowEnabled (progress); }),
        "video duration enables progress before host provides a playhead");
    check (waitUntil ([&] {
        return backend.tempoAnalyzer ()->snapshot ()->state == ap::TempoResult::State::Ready;
    }), "local video soundtrack is decoded and analyzed without playback");
    fixture.block (48000, 0);
    SendMessageW (panel, WM_TIMER, 1200, 0);
    HWND video = FindWindowExW (panel, nullptr, L"MuseSyncVideoView", nullptr);
    check (video != nullptr, "video embedded in main editor");
    RECT videoRect {}, waveRect {};
    GetWindowRect (video, &videoRect);
    HWND waveform = FindWindowExW (panel, nullptr, L"MuseToneWaveView", nullptr);
    GetWindowRect (waveform, &waveRect);
    check (EqualRect (&videoRect, &waveRect), "video occupies exact waveform bounds");
    for (const auto size : {ap::ui::Rect {0, 0, 1280, 800}, ap::ui::Rect {0, 0, 760, 500}})
    {
        SetWindowPos (host, nullptr, 0, 0, size.w, size.h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        ViewRect bounds {0, 0, size.w, size.h};
        view->onSize (&bounds);
        check (waitUntil ([&] {
            GetWindowRect (video, &videoRect);
            GetWindowRect (waveform, &waveRect);
            return EqualRect (&videoRect, &waveRect);
        }), "resizing main editor keeps video in waveform bounds");
    }
    if (visible)
    {
        const bool rendered = waitUntil ([&] {
            HDC screen = GetDC (nullptr);
            int colored = 0;
            for (int y = videoRect.top + 50; y < videoRect.bottom - 30; y += 20)
                for (int x = videoRect.left + 30; x < videoRect.right - 30; x += 20)
                {
                    const COLORREF c = GetPixel (screen, x, y);
                    if (c != CLR_INVALID && std::max ({GetRValue (c), GetGValue (c), GetBValue (c)}) -
                        std::min ({GetRValue (c), GetGValue (c), GetBValue (c)}) > 70) ++colored;
                }
            ReleaseDC (nullptr, screen);
            return colored > 30;
        });
        savePreview (panel, L"video-expanded-screen.bmp", true);
        check (rendered, "embedded video renders colored frames on screen");
    }
    check (!(GetWindowLongPtrW (waveform, GWL_STYLE) & WS_VISIBLE), "video hides waveform");
    check (!IsWindowEnabled (GetDlgItem (panel, 1014)), "video disables audio-only pitch controls");
    for (const int id : {1002, 1003, 1004, 1006, 1007})
        check (!(GetWindowLongPtrW (GetDlgItem (panel, id), GWL_STYLE) & WS_VISIBLE),
            "video hides waveform-only tools");
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1024, BN_CLICKED), 0);
    check (waitUntil ([&] { return !(GetWindowLongPtrW (video, GWL_STYLE) & WS_VISIBLE); }),
        "help hides embedded video");
    SendMessageW (panel, WM_KEYDOWN, VK_ESCAPE, 0);
    check (waitUntil ([&] { return (GetWindowLongPtrW (video, GWL_STYLE) & WS_VISIBLE) != 0; }),
        "closing help restores video");
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1029, BN_CLICKED), 0);
    HWND floating = FindWindowExW (nullptr, nullptr, L"MuseToneFloatingView", nullptr);
    check (floating && GetParent (progress) == floating, "video floating window shares progress control");
    HWND floatingVolume = GetDlgItem (floating, 1012), floatingOffset = GetDlgItem (floating, 1025);
    check (floatingVolume && floatingOffset, "floating video shares volume and delay controls");
    SendMessageW (floatingVolume, TBM_SETPOS, TRUE, 37);
    SendMessageW (floating, WM_HSCROLL, TB_THUMBPOSITION, reinterpret_cast<LPARAM> (floatingVolume));
    check (std::abs (backend.volume () - .37f) < 1e-5, "floating volume updates audio backend");
    SetWindowTextW (floatingOffset, L"150");
    SendMessageW (floating, WM_COMMAND, MAKEWPARAM (1025, EN_KILLFOCUS), reinterpret_cast<LPARAM> (floatingOffset));
    check (std::abs (backend.offsetSec () - .15f) < 1e-5, "floating delay commits to shared backend");
    SendMessageW (floatingOffset, WM_MOUSEWHEEL, MAKEWPARAM (MK_CONTROL, 120), 0);
    check (std::abs (backend.offsetSec () - .25f) < 1e-5, "floating Ctrl wheel adjusts delay by 100 ms");
    backend.setOffsetSec (0);
    backend.setVolume (1);
    check (waitUntil ([&] { return GetParent (video) == floating; }), "video moves into floating window");
    check ((GetWindowLongPtrW (video, GWL_STYLE) & WS_POPUP) &&
           !(GetWindowLongPtrW (video, GWL_STYLE) & WS_CHILD), "floating video is an owned popup");
    const LONG_PTR throughStyles = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    check ((GetWindowLongPtrW (video, GWL_EXSTYLE) & throughStyles) == throughStyles,
        "video surface passes mouse input through without activation");
    check (!(GetWindowLongPtrW (panel, GWL_STYLE) & WS_VISIBLE), "video collapse hides main editor");
    const auto previousProgress = SendMessageW (progress, TBM_GETPOS, 0, 0);
    fixture.block (96000, 0);
    SendMessageW (panel, WM_TIMER, 1200, 0);
    check (SendMessageW (progress, TBM_GETPOS, 0, 0) > previousProgress,
        "floating video progress follows score position");
    RECT expandRect {}, collapseRect {}, floatingRect {};
    GetWindowRect (GetDlgItem (floating, 1030), &expandRect);
    GetWindowRect (GetDlgItem (panel, 1029), &collapseRect);
    GetWindowRect (floating, &floatingRect);
    MONITORINFO monitor {sizeof monitor};
    GetMonitorInfoW (MonitorFromRect (&collapseRect, MONITOR_DEFAULTTONEAREST), &monitor);
    const int expectedX = std::clamp<LONG> (
        floatingRect.left + (collapseRect.left + collapseRect.right - expandRect.left - expandRect.right) / 2,
        monitor.rcWork.left, std::max (monitor.rcWork.left,
            monitor.rcWork.right - (floatingRect.right - floatingRect.left)));
    check (std::abs (floatingRect.left - expectedX) <= 1,
        "video floating expand stays aligned with collapse within monitor bounds");
    HWND opacity = GetDlgItem (floating, 1036);
    check (opacity != nullptr, "floating video has opacity slider");
    SendMessageW (opacity, TBM_SETPOS, TRUE, 55);
    SendMessageW (floating, WM_HSCROLL, TB_THUMBPOSITION, reinterpret_cast<LPARAM> (opacity));
    BYTE alpha = 0;
    DWORD flags = 0;
    check (GetLayeredWindowAttributes (floating, nullptr, &alpha, &flags) &&
        alpha == 55 * 255 / 100 && flags == LWA_ALPHA, "opacity adjusts floating window alpha");
    check (waitUntil ([&] {
        return GetLayeredWindowAttributes (video, nullptr, &alpha, &flags) && alpha == 55 * 255 / 100;
    }), "opacity also updates independent video layer");
    SendMessageW (opacity, WM_KEYDOWN, VK_RIGHT, 0);
    check (SendMessageW (opacity, TBM_GETPOS, 0, 0) == 56 &&
        GetLayeredWindowAttributes (floating, nullptr, &alpha, &flags) && alpha == 56 * 255 / 100,
        "opacity keyboard updates window alpha");
    RECT opacityRect {};
    GetClientRect (opacity, &opacityRect);
    SendMessageW (opacity, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM (opacityRect.right / 2, opacityRect.bottom / 2));
    SendMessageW (opacity, WM_LBUTTONUP, 0, MAKELPARAM (opacityRect.right / 2, opacityRect.bottom / 2));
    check (std::abs (SendMessageW (opacity, TBM_GETPOS, 0, 0) - 60) <= 1,
        "opacity click uses displayed slider range");
    {
        TestSeekBridge bridge;
        SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1021, BN_CLICKED), 0);
        fixture.block (0, 0);
        bridge.selection (.5);
        SendMessageW (panel, WM_TIMER, 1200, 0);
        const auto firstSelection = SendMessageW (progress, TBM_GETPOS, 0, 0);
        bridge.selection (1.5);
        SendMessageW (panel, WM_TIMER, 1200, 0);
        check (SendMessageW (progress, TBM_GETPOS, 0, 0) > firstSelection,
            "paused video progress follows score selection without host playback");
        fixture.block (96000, ProcessContext::kPlaying);
        SendMessageW (panel, WM_TIMER, 1200, 0);
        fixture.block (96000, 0);
        SendMessageW (panel, WM_TIMER, 1200, 0);
        const auto stoppedProgress = SendMessageW (progress, TBM_GETPOS, 0, 0);
        SendMessageW (panel, WM_TIMER, 1200, 0);
        check (SendMessageW (progress, TBM_GETPOS, 0, 0) == stoppedProgress,
            "pause retains host position instead of stale selection");
        SendMessageW (progress, WM_KEYDOWN, VK_HOME, 0);
        check (bridge.target () == 0, "floating video Home seeks score start");
        fixture.block (0, 0);
        SendMessageW (panel, WM_TIMER, 1200, 0);
        SendMessageW (progress, WM_KEYDOWN, VK_RIGHT, 0);
        check (std::abs (bridge.target () - 1.0) < .02, "floating video progress seeks score");
    }
    SetWindowPos (floating, nullptr, 0, 0, 760, 500, SWP_NOZORDER | SWP_NOACTIVATE);
    check (waitUntil ([&] {
        RECT client {}, bounds {};
        GetClientRect (floating, &client);
        GetWindowRect (video, &bounds);
        return bounds.right - bounds.left == client.right - 12;
    }), "floating resize updates video bounds");
    HRGN toolbarRegion = CreateRectRgn (0, 0, 0, 0);
    check (GetWindowRgn (floating, toolbarRegion) != ERROR &&
           PtInRegion (toolbarRegion, 20, 20) && !PtInRegion (toolbarRegion, 20, 200) &&
           PtInRegion (toolbarRegion, 2, 200) && PtInRegion (toolbarRegion, 758, 200) &&
           PtInRegion (toolbarRegion, 380, 498),
        "toolbar and resize frame remain interactive around video cutout");
    DeleteObject (toolbarRegion);
    SetWindowPos (floating, nullptr, 120, 140, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    check (waitUntil ([&] {
        RECT bounds {}, toolbar {};
        GetWindowRect (floating, &toolbar);
        GetWindowRect (video, &bounds);
        return bounds.left == toolbar.left + 6 && bounds.top == toolbar.top + 88;
    }), "dragging toolbar keeps transparent video aligned");
    fixture.block (48000, ProcessContext::kPlaying);
    SendMessageW (panel, WM_TIMER, 1200, 0);
    for (int i = 0; i < 150; ++i)
        SetWindowPos (floating, nullptr, 120 + i, 140 + i / 2, 0, 0,
                      SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    check (waitUntil ([&] {
        RECT bounds {}, toolbar {};
        GetWindowRect (floating, &toolbar);
        GetWindowRect (video, &bounds);
        return bounds.left == toolbar.left + 6 && bounds.top == toolbar.top + 88;
    }), "rapid moves leave video aligned at the final position");
    check (GetLayeredWindowAttributes (video, nullptr, &alpha, &flags) &&
           alpha == SendMessageW (opacity, TBM_GETPOS, 0, 0) * 255 / 100,
        "rapid moves preserve video opacity");
    const auto progressBeforeMovePlayback = SendMessageW (progress, TBM_GETPOS, 0, 0);
    fixture.block (144000, ProcessContext::kPlaying);
    SendMessageW (panel, WM_TIMER, 1200, 0);
    check (SendMessageW (progress, TBM_GETPOS, 0, 0) > progressBeforeMovePlayback,
        "playback progress advances after rapid moves");
    fixture.block (144000, 0);
    const auto hitAt = [&] (int x, int y) {
        RECT bounds {};
        GetWindowRect (floating, &bounds);
        return SendMessageW (floating, WM_NCHITTEST, 0,
            MAKELPARAM (bounds.left + x, bounds.top + y));
    };
    check (hitAt (2, 250) == HTLEFT && hitAt (758, 250) == HTRIGHT &&
           hitAt (380, 498) == HTBOTTOM && hitAt (380, 2) == HTTOP &&
           hitAt (2, 2) == HTTOPLEFT && hitAt (758, 2) == HTTOPRIGHT &&
           hitAt (2, 498) == HTBOTTOMLEFT && hitAt (758, 498) == HTBOTTOMRIGHT,
        "all four edges and corners resize floating video");
    check (hitAt (380, 250) == HTTRANSPARENT && hitAt (24, 22) == HTCAPTION,
        "video center passes input through while toolbar remains draggable");
    // Desktop hit testing can be obstructed by another application's topmost
    // window. Never send mouse input to an unrelated window during this test.
    HWND underlay = CreateWindowExW (WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"STATIC", L"Input target",
        WS_POPUP | WS_VISIBLE, 120, 140, 760, 500, nullptr, nullptr, GetModuleHandleW (nullptr), nullptr);
    check (underlay != nullptr, "create underlying input target");
    ShowWindow (floating, SW_SHOWNOACTIVATE);
    SetWindowPos (floating, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SendMessageW (floating, WM_MOVE, 0, 0);
    check (waitUntil ([&] { return IsWindowVisible (video) != FALSE; }), "video overlay visible");
    RECT overlayBounds {};
    GetWindowRect (video, &overlayBounds);
    POINT target {(overlayBounds.left + overlayBounds.right) / 2,
                  (overlayBounds.top + overlayBounds.bottom) / 2};
    SetWindowPos (underlay, video, overlayBounds.left, overlayBounds.top,
        overlayBounds.right - overlayBounds.left, overlayBounds.bottom - overlayBounds.top, SWP_NOACTIVATE);
    HWND hitBefore = WindowFromPoint (target);
    const bool obstructed = hitBefore != underlay && GetAncestor (hitBefore, GA_ROOT) != video;
    bool mousePassed = false;
    if (obstructed)
    {
        std::puts ("SKIP: desktop mouse-through check obstructed by another application's topmost window");
    }
    else
    {
        mousePassed = waitUntil ([&] { return WindowFromPoint (target) == underlay; });
    }
    ShowWindow (floating, SW_HIDE);
    DestroyWindow (underlay);
    check (obstructed || mousePassed, "mouse over video reaches underlying window across threads");
    RECT progressBounds {};
    GetWindowRect (progress, &progressBounds);
    const auto toolbarHit = SendMessageW (floating, WM_NCHITTEST, 0, MAKELPARAM (
        (progressBounds.left + progressBounds.right) / 2, (progressBounds.top + progressBounds.bottom) / 2));
    check (toolbarHit == HTCLIENT, "toolbar progress remains clickable");
    if (visible) ShowWindow (floating, SW_SHOWNOACTIVATE);
    savePreview (floating, L"video-floating.bmp");
    if (visible)
    {
        savePreview (floating, L"video-floating-screen.bmp", true);
        SendMessageW (opacity, TBM_SETPOS, TRUE, 100);
        SendMessageW (floating, WM_HSCROLL, TB_THUMBPOSITION, reinterpret_cast<LPARAM> (opacity));
        const auto ready = GetTickCount64 () + 400;
        waitUntil ([&] { return GetTickCount64 () >= ready; });
        savePreview (floating, L"video-floating-opaque-screen.bmp", true);
    }
    const double floatingScale = (expandRect.bottom - expandRect.top) / 36.0;
    MINMAXINFO minimum {};
    SendMessageW (floating, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM> (&minimum));
    check (minimum.ptMinTrackSize.x <= std::lround (280 * floatingScale) + 1 &&
           minimum.ptMinTrackSize.y <= std::lround (180 * floatingScale) + 1,
           "video floating minimum permits a small window");
    SetWindowPos (floating, nullptr, 0, 0, minimum.ptMinTrackSize.x, minimum.ptMinTrackSize.y,
                  SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RECT smallWindow {}, smallClient {};
    GetWindowRect (floating, &smallWindow);
    GetClientRect (floating, &smallClient);
    check (smallWindow.right - smallWindow.left == smallClient.right &&
           smallWindow.bottom - smallWindow.top == smallClient.bottom,
           "floating window has no native non-client white strip");
    check (waitUntil ([&] {
        RECT bounds {};
        GetWindowRect (video, &bounds);
        return bounds.right - bounds.left == smallClient.right - 12;
    }), "small floating video finishes asynchronous resize");
    for (HWND control : {progress, opacity, GetDlgItem (floating, 1030),
                        GetDlgItem (floating, 1012), GetDlgItem (floating, 1025), video})
    {
        RECT bounds {};
        GetWindowRect (control, &bounds);
        MapWindowPoints (nullptr, floating, reinterpret_cast<POINT*> (&bounds), 2);
        check (bounds.left >= 0 && bounds.top >= 0 && bounds.right <= smallClient.right &&
               bounds.bottom <= smallClient.bottom, "small video floating controls stay in bounds");
    }
    savePreview (floating, L"video-floating-small.bmp");
    if (visible) savePreview (floating, L"video-floating-small-screen.bmp", true);
    SendMessageW (floating, WM_COMMAND, MAKEWPARAM (1030, BN_CLICKED), 0);
    check (waitUntil ([&] { return GetParent (video) == panel; }), "expand embeds video again");
    check (waitUntil ([&] { return (GetWindowLongPtrW (video, GWL_STYLE) & WS_CHILD) &&
           !(GetWindowLongPtrW (video, GWL_EXSTYLE) & throughStyles); }),
        "expanded video restores ordinary embedded window styles");
    check (GetParent (progress) == panel, "expand restores main progress control");
    check (GetParent (floatingVolume) == panel && GetParent (floatingOffset) == panel,
        "expand restores shared volume and delay controls");
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1029, BN_CLICKED), 0);
    check (waitUntil ([&] { return GetParent (video) == floating; }), "video can collapse repeatedly");
    check (GetLayeredWindowAttributes (floating, nullptr, &alpha, &flags) &&
        alpha == SendMessageW (opacity, TBM_GETPOS, 0, 0) * 255 / 100,
        "video opacity persists after expand and collapse");
    SendMessageW (floating, WM_COMMAND, MAKEWPARAM (1030, BN_CLICKED), 0);
    check (waitUntil ([&] { return GetParent (video) == panel; }), "repeated expand restores video parent");
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1024, BN_CLICKED), 0);
    dropFile (wave.wstring ());
    check (backend.hasAudio (), "audio import replaces video");
    check (!IsWindow (video) && !(GetWindowLongPtrW (waveform, GWL_STYLE) & WS_VISIBLE),
        "audio replacement keeps waveform hidden behind help");
    check (!(GetWindowLongPtrW (GetDlgItem (panel, 1014), GWL_STYLE) & WS_VISIBLE),
        "audio replacement keeps parameter controls hidden behind help");
    SendMessageW (panel, WM_KEYDOWN, VK_ESCAPE, 0);
    check (!IsWindow (video) && (GetWindowLongPtrW (waveform, GWL_STYLE) & WS_VISIBLE),
        "audio import closes video and restores waveform");
    check (IsWindowEnabled (GetDlgItem (panel, 1014)), "audio restores pitch controls");
    for (const int id : {1002, 1003, 1004, 1006, 1007})
        check ((GetWindowLongPtrW (GetDlgItem (panel, id), GWL_STYLE) & WS_VISIBLE) != 0,
            "audio restores waveform-only tools");
    check (IsWindowEnabled (GetDlgItem (panel, 1037)), "loaded audio enables close material");
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1037, BN_CLICKED), 0);
    check (!backend.hasAudio () && backend.currentPath ().empty () && !IsWindowEnabled (progress) &&
           !IsWindowEnabled (GetDlgItem (panel, 1037)), "close material clears audio and progress");
    dropFile (videoPath);
    check (waitUntil ([&] {
        video = FindWindowExW (panel, nullptr, L"MuseSyncVideoView", nullptr);
        return video && IsWindowEnabled (GetDlgItem (panel, 1037));
    }), "video enables close material");
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1037, BN_CLICKED), 0);
    check (!IsWindow (video) && !IsWindowEnabled (progress) &&
           !IsWindowEnabled (GetDlgItem (panel, 1037)), "close material destroys video and clears progress");
    check (backend.tempoAnalyzer ()->snapshot ()->state == ap::TempoResult::State::Empty,
        "closing video clears soundtrack BPM results");
    dropFile (videoPath);
    check (waitUntil ([&] { return IsWindowEnabled (progress); }), "video can reopen after audio replacement");
    video = FindWindowExW (panel, nullptr, L"MuseSyncVideoView", nullptr);
    check (video != nullptr, "reopened video embedded before collapse");
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1029, BN_CLICKED), 0);
    check (waitUntil ([&] { return GetWindow (video, GW_OWNER) == floating; }),
        "second video collapses");
    SendMessageW (floating, WM_COMMAND, MAKEWPARAM (1030, BN_CLICKED), 0);
    check (waitUntil ([&] { return GetParent (video) == panel; }), "expand before opening online video");
    check (waitUntil ([&] { return backend.tempoAnalyzer ()->snapshot ()->state == ap::TempoResult::State::Ready; }),
        "local video analysis completes before replacement");
    const UINT_PTR urlTimer = SetTimer (nullptr, 0, 10, [] (HWND, UINT, UINT_PTR timerId, DWORD) {
        HWND prompt = FindWindowExW (nullptr, nullptr, L"MuseSyncVideoUrlPrompt", nullptr);
        if (!prompt) return;
        KillTimer (nullptr, timerId);
        SetWindowTextW (GetDlgItem (prompt, 1), L"https://example.com/video");
        PostMessageW (prompt, WM_COMMAND, 2, 0);
    });
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1402, BN_CLICKED), 0);
    KillTimer (nullptr, urlTimer);
    check (backend.tempoAnalyzer ()->snapshot ()->state == ap::TempoResult::State::Empty,
        "online video clears previous BPM and does not start analysis");
    SendMessageW (panel, WM_COMMAND, MAKEWPARAM (1029, BN_CLICKED), 0);
    view->removed ();
    check (!IsWindow (floating), "closing collapsed video destroys floating window");
    view->release ();
    DestroyWindow (host);
}

void pitchOutputTests (const std::filesystem::path& wave)
{
    Fixture fixture (false);
    auto& p = fixture.processor;
    check (p.loadFile (wave.u8string ()), "pitch audio loaded");
    p.setSemitones (2);
    auto waitForPitch = [&] {
        const auto deadline = GetTickCount64 () + 10000;
        while (GetTickCount64 () < deadline)
        {
            if (!p.pitchProcessing () && p.pitchStatus () == "变调已应用") return;
            Sleep (10);
        }
        check (false, "pitch render completes and result is published");
    };
    auto frequency = [&] (double expected) {
        std::vector<float> samples;
        samples.reserve (48000);
        fixture.block (0, 0);
        for (int i = 0; i < 48; ++i)
        {
            fixture.block (48000 + i * 1000, ProcessContext::kPlaying, 1000);
            samples.insert (samples.end (), fixture.left, fixture.left + 1000);
        }
        constexpr double pi = 3.141592653589793;
        for (size_t i = 0; i < samples.size (); ++i)
            samples[i] *= static_cast<float> (.5 - .5 * std::cos (2 * pi * i / samples.size ()));
        double bestFrequency = 0, bestMagnitude = 0;
        for (double candidate = expected - 10; candidate <= expected + 10; candidate += .25)
        {
            double real = 0, imaginary = 0;
            for (size_t i = 0; i < samples.size (); ++i)
            {
                const double phase = 2 * pi * candidate * i / 48000;
                real += samples[i] * std::cos (phase);
                imaginary += samples[i] * std::sin (phase);
            }
            const double magnitude = real * real + imaginary * imaginary;
            if (magnitude > bestMagnitude)
            {
                bestMagnitude = magnitude;
                bestFrequency = candidate;
            }
        }
        return bestFrequency;
    };
    const double sourceFrequency = .04 * 48000 / (2 * 3.141592653589793);
    waitForPitch ();
    const double expected = sourceFrequency * ap::pitchRatio (2, 0);
    const double shiftedFrequency = frequency (expected);
    std::printf ("VST output +2 semitones: %.2f Hz (expected %.2f Hz)\n", shiftedFrequency, expected);
    check (std::abs (shiftedFrequency - expected) < 2,
           "+2 changes actual VST audio frequency");
    p.setSemitones (7);
    p.setCents (75);
    p.setSemitones (-2);
    p.setCents (-50);
    waitForPitch ();
    const double lastExpected = sourceFrequency * ap::pitchRatio (-2, -50);
    check (std::abs (frequency (lastExpected) - lastExpected) < 2,
           "rapid changes publish only the final semitone and cent values");
    p.setCents (0);
    p.setSemitones (0);
    check (!p.pitchProcessing () && p.pitchStatus () == "原始音高", "pitch reset is immediate");
    check (std::abs (frequency (sourceFrequency) - sourceFrequency) < 1,
           "reset restores actual original frequency");
}
} // namespace

int main (int argc, char** argv)
{
    const auto directory = std::filesystem::temp_directory_path () /
        (L"MuseTone-test-" + std::to_wstring (GetCurrentProcessId ()));
    try
    {
        if (argc == 3 && std::strcmp (argv[1], "--video-probe") == 0)
        {
            Fixture fixture (false);
            ap::VideoView video;
            const auto target = std::filesystem::u8path (argv[2]).wstring ();
            const bool local = std::filesystem::is_regular_file (target);
            check (local ? video.openLocal (target) : video.openUrl (target), "video probe opens");
            HWND host = CreateWindowExW (0, L"STATIC", L"MuseSync video verification", WS_OVERLAPPEDWINDOW,
                100, 100, 960, 640, nullptr, nullptr, GetModuleHandleW (nullptr), nullptr);
            ShowWindow (host, SW_SHOWNOACTIVATE);
            video.present (host, 0, 0, 940, 600, true);
            fixture.processor.setVideoAudio (video.audioStream ());
            const auto start = GetTickCount64 ();
            double energy = 0;
            while (GetTickCount64 () - start < 45000)
            {
                MSG message {};
                while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage (&message);
                    DispatchMessageW (&message);
                }
                const double seconds = (GetTickCount64 () - start) / 1000.0;
                const double duration = video.durationSec ();
                video.sync (duration > 0 ? std::fmod (seconds, duration) : seconds, true);
                energy += fixture.block (static_cast<int64> (seconds * 48000), ProcessContext::kPlaying, 960);
                MsgWaitForMultipleObjects (0, nullptr, FALSE, 20, QS_ALLINPUT);
            }
            video.close ();
            DestroyWindow (host);
            std::printf ("Probe VST energy: %.3f\n", energy);
            check (energy > 1, "probe sends audio to VST");
            return 0;
        }
        std::filesystem::create_directory (directory);
        const auto wave = directory / "test.wav";
        writeWave (wave);
        hostIdentityTests ();
        scoreTests (directory);
        transportTests (wave);
        pitchOutputTests (wave);
        videoAudioTests ();
        browserAudioTests ();
        liveScoreTests ();
        writeWave (wave, true);
        hiddenControlTests (wave);
        videoControlTests (wave);
        tempoControlTests (wave);
        // Only the explicitly created test directory is removed.
        std::filesystem::remove_all (directory);
        std::puts ("PASS: MSCX / MSCZ parsing, Unicode paths, state, host transport, MuseScore fallback and audio output");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf (stderr, "FAIL: %s (fixtures retained at %s)\n", error.what (), directory.u8string ().c_str ());
        return 1;
    }
}
