#include "aplaysdk.h"
#include "hostcompat.h"
#include "scoreinfo.h"
#include "scorebridge.h"
#include "win/ui_layout.h"
#include "ui_theme.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "miniz.h"

#include <windows.h>
#include <commctrl.h>
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

void savePreview (HWND window, const wchar_t* name)
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
    SendMessageW (window, WM_PRINTCLIENT, reinterpret_cast<WPARAM> (dc), PRF_CLIENT);
    for (HWND child = GetWindow (window, GW_CHILD); child; child = GetWindow (child, GW_HWNDNEXT))
    {
        if (!(GetWindowLongPtrW (child, GWL_STYLE) & WS_VISIBLE)) continue;
        RECT childRect {};
        GetWindowRect (child, &childRect);
        MapWindowPoints (nullptr, window, reinterpret_cast<POINT*> (&childRect), 2);
        const int saved = SaveDC (dc);
        IntersectClipRect (dc, childRect.left, childRect.top, childRect.right, childRect.bottom);
        SetViewportOrgEx (dc, childRect.left, childRect.top, nullptr);
        SendMessageW (child, WM_PRINTCLIENT, reinterpret_cast<WPARAM> (dc), PRF_CLIENT);
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

void writeWave (const std::filesystem::path& path)
{
    std::ofstream file (path, std::ios::binary);
    auto u16 = [&] (uint16_t v) { file.write (reinterpret_cast<char*> (&v), 2); };
    auto u32 = [&] (uint32_t v) { file.write (reinterpret_cast<char*> (&v), 4); };
    const int count = 48000 * 4;
    file.write ("RIFF", 4); u32 (36 + count * 2); file.write ("WAVEfmt ", 8);
    u32 (16); u16 (1); u16 (1); u32 (48000); u32 (96000); u16 (2); u16 (16);
    file.write ("data", 4); u32 (count * 2);
    for (int i = 0; i < count; ++i) u16 (static_cast<uint16_t> (static_cast<int16_t> (9000 * std::sin (i * 0.04))));
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
    savePreview (panel, L"playback-light.bmp");
    backend.setTheme (1);
    SendMessageW (panel, WM_TIMER, 1200, 0);
    savePreview (panel, L"playback-dark.bmp");
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
    check (GetPixel (memory, r.right - 3, r.bottom / 2) == RGB (23, 24, 26),
           "dark meter arrow area has no white native button");
    backend.setTheme (2);
    SendMessageW (panel, WM_TIMER, 1200, 0);
    SendMessageW (meter, WM_PRINTCLIENT, reinterpret_cast<WPARAM> (memory), PRF_CLIENT);
    check (GetPixel (memory, r.right - 3, r.bottom / 2) == RGB (250, 252, 250),
           "sage meter face follows its surface color");

    const auto compact = ap::ui::arrange (ap::ui::minWidth, ap::ui::minHeight);
    SelectObject (memory, reinterpret_cast<HFONT> (SendMessageW (bpm, WM_GETFONT, 0, 0)));
    SIZE textSize {};
    const wchar_t* longestOffset = L"-3600000";
    GetTextExtentPoint32W (memory, longestOffset, 8, &textSize);
    const double scale = static_cast<double> (r.bottom) / compact.timeSig.h;
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
    SendMessageW (panel, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM (100, 100));
    check (!(GetWindowLongPtrW (waveform, GWL_STYLE) & WS_VISIBLE), "guide body click does not dismiss it");
    SendMessageW (panel, WM_KEYDOWN, VK_ESCAPE, 0);
    check ((GetWindowLongPtrW (waveform, GWL_STYLE) & WS_VISIBLE) != 0, "Escape closes guide");
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
    HWND editor = CreateWindowExW (0, L"STATIC", L"MuseTone", WS_OVERLAPPED,
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

int main ()
{
    const auto directory = std::filesystem::temp_directory_path () /
        (L"MuseTone-test-" + std::to_wstring (GetCurrentProcessId ()));
    try
    {
        std::filesystem::create_directory (directory);
        const auto wave = directory / "test.wav";
        writeWave (wave);
        hostIdentityTests ();
        scoreTests (directory);
        transportTests (wave);
        pitchOutputTests (wave);
        liveScoreTests ();
        hiddenControlTests (wave);
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
