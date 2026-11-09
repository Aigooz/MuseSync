#include "video_view.h"
#include "../video_audio.h"
#include "../tempo.h"
#include "video_script.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifdef MUSESYNC_HAS_WEBVIEW2
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <shlwapi.h>
#endif

namespace ap {

#ifndef MUSESYNC_HAS_WEBVIEW2

bool isVideoFile (const std::wstring& path)
{
    const size_t slash = path.find_last_of (L"/\\");
    const size_t dot = path.find_last_of (L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
        return false;
    std::wstring ext = path.substr (dot);
    std::transform (ext.begin (), ext.end (), ext.begin (), towlower);
    return ext == L".mp4" || ext == L".webm" || ext == L".m4v";
}

struct VideoView::Impl {};

VideoView::VideoView () : m_impl (std::make_shared<Impl> ()) {}
VideoView::~VideoView () = default;
bool VideoView::openLocal (const std::wstring&) { return false; }
bool VideoView::openUrl (const std::wstring&) { return false; }
void VideoView::sync (double, bool, bool) {}
double VideoView::durationSec () const { return 0.0; }
std::shared_ptr<VideoAudioStream> VideoView::audioStream () const { return {}; }
void VideoView::setTempoAnalyzer (std::shared_ptr<TempoAnalyzer>) {}
void VideoView::present (void*, int, int, int, int, bool, bool, int) {}
void VideoView::close () {}

#else

namespace {
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"MuseSyncVideoView";
constexpr UINT kOpenTarget = WM_APP + 0x361;
constexpr UINT kCloseView = WM_APP + 0x362;
constexpr UINT kSyncPlayback = WM_APP + 0x363;
constexpr UINT kRescanPlayer = WM_APP + 0x364;
constexpr UINT kPresentView = WM_APP + 0x365;
constexpr UINT_PTR kRescanTimerId = 1;
constexpr int kToolbarHeight = 0;

std::wstring lowerExtension (const std::wstring& path)
{
    const size_t slash = path.find_last_of (L"/\\");
    const size_t dot = path.find_last_of (L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    std::wstring ext = path.substr (dot);
    std::transform (ext.begin (), ext.end (), ext.begin (), towlower);
    return ext;
}

std::wstring userDataPath ()
{
    PWSTR local = nullptr;
    if (FAILED (::SHGetKnownFolderPath (FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local)))
        return {};
    std::wstring result (local);
    ::CoTaskMemFree (local);
    result += L"\\MuseSync\\WebView2";
    const int error = ::SHCreateDirectoryExW (nullptr, result.c_str (), nullptr);
    if (error != ERROR_SUCCESS && error != ERROR_ALREADY_EXISTS)
        return {};
    return result;
}

bool validTarget (const std::wstring& url)
{
    return (url.size () > 8 && _wcsnicmp (url.c_str (), L"https://", 8) == 0) ||
           (url.size () > 7 && _wcsnicmp (url.c_str (), L"http://", 7) == 0) ||
           (url.size () > 7 && _wcsnicmp (url.c_str (), L"file://", 7) == 0);
}
} // namespace

bool isVideoFile (const std::wstring& path)
{
    const auto ext = lowerExtension (path);
    return ext == L".mp4" || ext == L".webm" || ext == L".m4v";
}

struct VideoView::Impl : std::enable_shared_from_this<VideoView::Impl>
{
    HWND window = nullptr;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    ComPtr<ICoreWebView2_4> webview4;
    std::vector<ComPtr<ICoreWebView2Frame2>> frames;
    EventRegistrationToken navigationStartingToken {};
    EventRegistrationToken navigationCompletedToken {};
    EventRegistrationToken frameCreatedToken {};
    bool navigationStartingRegistered = false;
    bool navigationCompletedRegistered = false;
    bool frameCreatedRegistered = false;
    EventRegistrationToken audioMessageToken {};
    bool audioMessageRegistered = false;
    std::shared_ptr<VideoAudioStream> audio = std::make_shared<VideoAudioStream> ();
    std::shared_ptr<TempoAnalyzer> tempo;
    HWND statusLabel = nullptr;
    std::wstring target;
    bool localVideo = false;
    bool foundRemoteVideo = false;
    unsigned long long navigationGeneration = 0;
    bool initializing = false;
    std::atomic<bool> stopping {false};
    std::thread thread;
    std::mutex stateMutex;
    std::condition_variable stateChanged;
    // The GUI timer can publish a new timeline position while WebView2 is
    // still handling the previous script. Keep one pending state only.
    std::mutex syncMutex;
    double queuedSeconds = 0.0;
    std::chrono::steady_clock::time_point queuedAt {}, pendingAt {};
    bool queuedPlaying = false;
    double queuedPlaybackRate = 1.0;
    bool queuedDiscontinuity = false;
    bool syncQueued = false;
    bool startupDone = false;
    bool startupSucceeded = false;
    bool runExited = false;
    DWORD threadId = 0;
    double pendingSeconds = 0.0;
    std::atomic<double> duration {0.0};
    HWND presentationParent = nullptr;
    RECT presentationBounds {};
    bool presentationVisible = false;
    bool presentationMouseThrough = false;
    int presentationOpacity = 100;
    bool presentationQueued = false;
    bool appliedPresentation = false;
    bool appliedVisible = false;
    bool appliedMouseThrough = false;
    int appliedOpacity = 100;
    bool pendingPlaying = false;
    double pendingPlaybackRate = 1.0;
    bool pendingDiscontinuity = false;
    bool hasPendingPlayback = false;
    unsigned scriptsInFlight = 0;
    bool playbackDirty = false;
    bool hasRateSample = false;
    bool lastSyncPlaying = false;
    double lastSyncSeconds = 0.0;
    double estimatedRate = 1.0;
    std::chrono::steady_clock::time_point lastSyncAt {};
    int rescanPasses = 0;

    void resetRateEstimate ()
    {
        std::lock_guard<std::mutex> lock (syncMutex);
        hasRateSample = false;
        lastSyncPlaying = false;
        lastSyncAt = {};
        estimatedRate = 1.0;
    }

    void schedulePlayerRescan ()
    {
        const HWND hwnd = currentWindow ();
        if (!hwnd) return;
        rescanPasses = 0;
        ::SetTimer (hwnd, kRescanTimerId, 350, nullptr);
    }

    void stopPlayerRescan ()
    {
        if (const HWND hwnd = currentWindow ())
            ::KillTimer (hwnd, kRescanTimerId);
        rescanPasses = 0;
    }

    void updateStatus (bool foundVideo)
    {
        if (!statusLabel) return;
        std::wstring status = localVideo ? L"本地视频 · 谱面 " :
            (foundVideo ? L"已检测到网页视频 · 谱面 " : L"正在查找网页视频 · 谱面 ");
        wchar_t timeText[64] {};
        const auto total = static_cast<unsigned long long> (std::max (0.0, pendingSeconds));
        std::swprintf (timeText, sizeof timeText / sizeof timeText[0], L"%02llu:%02llu",
            total / 60, total % 60);
        status += timeText;
        status += pendingPlaying ? L" · 播放中" : L" · 已暂停";
        ::SetWindowTextW (statusLabel, status.c_str ());
    }

    std::wstring makeSyncScript () const
    {
        wchar_t script[512] {};
        std::swprintf (script, sizeof script / sizeof script[0],
            L"window.__museSyncPlayback={t:%.6f,at:performance.now(),playing:%s,rate:%.4f,local:%s,seek:%s,generation:%u};",
            std::max (0.0, pendingSeconds + (pendingPlaying ? pendingPlaybackRate *
                std::chrono::duration<double> (std::chrono::steady_clock::now () - pendingAt).count () : 0.0)),
            pendingPlaying ? L"true" : L"false",
            pendingPlaybackRate, localVideo ? L"true" : L"false",
            pendingDiscontinuity ? L"true" : L"false", audio->generation ());
        return std::wstring (script) + L"window.__museSyncUpdate ? window.__museSyncUpdate() : -1";
    }

    void receiveAudio (ICoreWebView2WebMessageReceivedEventArgs* args)
    {
        LPWSTR text = nullptr;
        if (!args || FAILED (args->TryGetWebMessageAsString (&text))) return;
        constexpr wchar_t prefix[] = L"MuseSyncPCM:";
        constexpr size_t prefixLength = sizeof prefix / sizeof prefix[0] - 1;
        const size_t length = std::wcslen (text);
        if (length > prefixLength && length <= 50000 && std::wcsncmp (text, prefix, prefixLength) == 0)
        {
            DWORD size = 0;
            if (::CryptStringToBinaryW (text + prefixLength, static_cast<DWORD> (length - prefixLength),
                                        CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr) &&
                size >= 32 && size <= 32792 && (size - 24) % 8 == 0)
            {
                std::vector<float> samples ((size - 24) / 4);
                std::vector<BYTE> decoded (size);
                if (::CryptStringToBinaryW (text + prefixLength, static_cast<DWORD> (length - prefixLength),
                                           CRYPT_STRING_BASE64, decoded.data (), &size, nullptr, nullptr))
                {
                    uint32_t generation = 0, rate = 0;
                    std::memcpy (&generation, decoded.data (), 4);
                    std::memcpy (&rate, decoded.data () + 4, 4);
                    double endSeconds = 0, speed = 1;
                    std::memcpy (&endSeconds, decoded.data () + 8, 8);
                    std::memcpy (&speed, decoded.data () + 16, 8);
                    std::memcpy (samples.data (), decoded.data () + 24, size - 24);
                    if (generation != audio->generation ()) { ::CoTaskMemFree (text); return; }
                    audio->push (samples.data (), samples.size () / 2, rate, generation);
                    if (auto analyzer = std::atomic_load (&tempo))
                        analyzer->push (samples.data (), samples.size () / 2, rate,
                                        generation, endSeconds, speed);
                }
            }
        }
        ::CoTaskMemFree (text);
    }

    void applyPlaybackState ()
    {
        if (!webview || !hasPendingPlayback) return;
        if (scriptsInFlight) { playbackDirty = true; return; }
        playbackDirty = false;
        if (localVideo) updateStatus (true);
        const auto script = makeSyncScript ();
        pendingDiscontinuity = false;
        auto self = std::enable_shared_from_this<Impl>::shared_from_this ();
        const auto generation = navigationGeneration;
        auto completed = Callback<ICoreWebView2ExecuteScriptCompletedHandler> (
            [self, generation] (HRESULT error, LPCWSTR result) -> HRESULT {
                if (generation != self->navigationGeneration) return S_OK;
                if (SUCCEEDED (error) && result)
                {
                    wchar_t* end = nullptr;
                    const double duration = std::wcstod (result, &end);
                    if (end != result && !*end && std::isfinite (duration) && duration >= 0)
                    {
                        self->duration.store (duration);
                        self->foundRemoteVideo = true;
                    }
                }
                if (self->scriptsInFlight && --self->scriptsInFlight == 0 && self->playbackDirty)
                    self->applyPlaybackState ();
                return S_OK;
            });
        // Account for the whole batch before issuing asynchronous work.
        scriptsInFlight = 1 + static_cast<unsigned> (frames.size ());
        if (FAILED (webview->ExecuteScript (script.c_str (), completed.Get ())))
            completed->Invoke (E_FAIL, nullptr);
        for (auto it = frames.begin (); it != frames.end ();)
        {
            BOOL destroyed = FALSE;
            if (FAILED ((*it)->IsDestroyed (&destroyed)) || destroyed)
            {
                it = frames.erase (it);
                completed->Invoke (E_FAIL, nullptr);
                continue;
            }
            auto frame = *it;
            if (FAILED (frame->ExecuteScript (script.c_str (), completed.Get ())))
                completed->Invoke (E_FAIL, nullptr);
            ++it;
        }
    }

    void setStartup (bool success)
    {
        {
            std::lock_guard<std::mutex> lock (stateMutex);
            startupDone = true;
            startupSucceeded = success;
        }
        stateChanged.notify_all ();
    }

    HWND currentWindow ()
    {
        std::lock_guard<std::mutex> lock (stateMutex);
        return window;
    }

    static LRESULT CALLBACK windowProc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<Impl*> (::GetWindowLongPtrW (hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE)
        {
            auto* create = reinterpret_cast<CREATESTRUCTW*> (lp);
            self = static_cast<Impl*> (create->lpCreateParams);
            {
                std::lock_guard<std::mutex> lock (self->stateMutex);
                self->window = hwnd;
            }
            ::SetWindowLongPtrW (hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR> (self));
        }
        if (!self) return ::DefWindowProcW (hwnd, msg, wp, lp);

        switch (msg)
        {
            case kOpenTarget:
            {
                std::unique_ptr<std::wstring> value (reinterpret_cast<std::wstring*> (lp));
                if (value)
                {
                    self->target = std::move (*value);
                    self->localVideo = self->target.size () > 7 &&
                        _wcsnicmp (self->target.c_str (), L"file://", 7) == 0;
                    self->frames.clear ();
                    self->resetRateEstimate ();
                    self->stopPlayerRescan ();
                    self->foundRemoteVideo = false;
                    self->duration.store (0.0);
                    ++self->navigationGeneration;
                    self->scriptsInFlight = 0;
                    self->playbackDirty = false;
                    if (self->webview) self->navigate ();
                    else self->initialize ();
                }
                return 0;
            }
            case kSyncPlayback:
            {
                {
                    std::lock_guard<std::mutex> lock (self->syncMutex);
                    self->pendingSeconds = self->queuedSeconds;
                    self->pendingAt = self->queuedAt;
                    self->pendingPlaying = self->queuedPlaying;
                    self->pendingPlaybackRate = self->queuedPlaybackRate;
                    self->pendingDiscontinuity = self->pendingDiscontinuity || self->queuedDiscontinuity;
                    self->queuedDiscontinuity = false;
                    self->hasPendingPlayback = true;
                    self->syncQueued = false;
                }
                self->applyPlaybackState ();
                return 0;
            }
            case kPresentView:
            {
                HWND parent;
                RECT bounds;
                bool visible, mouseThrough;
                int opacity;
                {
                    std::lock_guard<std::mutex> lock (self->stateMutex);
                    parent = self->presentationParent;
                    bounds = self->presentationBounds;
                    visible = self->presentationVisible;
                    mouseThrough = self->presentationMouseThrough;
                    opacity = self->presentationOpacity;
                    self->presentationQueued = false;
                }
                if (parent && ::IsWindow (parent))
                {
                    const bool wasPopup = (::GetWindowLongPtrW (hwnd, GWL_STYLE) & WS_POPUP) != 0;
                    const bool changedParent = ::GetParent (hwnd) != parent || wasPopup != mouseThrough;
                    const bool changedMode = !self->appliedPresentation ||
                        self->appliedMouseThrough != mouseThrough || changedParent;
                    if (changedParent)
                    {
                        ::ShowWindow (hwnd, SW_HIDE);
                        ::SetParent (hwnd, nullptr);
                        ::SetWindowLongPtrW (hwnd, GWL_STYLE,
                            (mouseThrough ? WS_POPUP : WS_CHILD) | WS_CLIPCHILDREN);
                        if (mouseThrough)
                            ::SetWindowLongPtrW (hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR> (parent));
                        else ::SetParent (hwnd, parent);
                    }
                    // Layered transparent top-level windows pass input through even
                    // to other processes; HTTRANSPARENT alone only covers one thread.
                    if (changedMode)
                        ::SetWindowLongPtrW (hwnd, GWL_EXSTYLE, WS_EX_TOOLWINDOW |
                            (mouseThrough ? WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE : 0));
                    // WebView2 owns native child windows in its browser process.
                    // Disable the input subtree as well as the popup hit surface.
                    if (changedMode) ::EnableWindow (hwnd, mouseThrough ? FALSE : TRUE);
                    if (mouseThrough)
                    {
                        if (changedMode || self->appliedOpacity != opacity)
                            ::SetLayeredWindowAttributes (hwnd, 0,
                                static_cast<BYTE> (std::clamp (opacity, 20, 100) * 255 / 100), LWA_ALPHA);
                        ::MapWindowPoints (parent, nullptr, reinterpret_cast<POINT*> (&bounds), 2);
                    }
                    ::SetWindowPos (hwnd, mouseThrough ? HWND_TOPMOST : HWND_TOP, bounds.left, bounds.top,
                        bounds.right - bounds.left, bounds.bottom - bounds.top,
                        SWP_NOACTIVATE | (changedMode ? SWP_FRAMECHANGED : SWP_NOZORDER) |
                        ((!self->appliedPresentation || self->appliedVisible != visible || changedParent) ?
                            (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW) : 0));
                    if (self->controller)
                    {
                        if (!self->appliedPresentation || self->appliedVisible != visible || changedParent)
                            self->controller->put_IsVisible (visible ? TRUE : FALSE);
                        self->controller->NotifyParentWindowPositionChanged ();
                    }
                    self->appliedPresentation = true;
                    self->appliedVisible = visible;
                    self->appliedMouseThrough = mouseThrough;
                    self->appliedOpacity = opacity;
                }
                return 0;
            }
            case kRescanPlayer:
                self->schedulePlayerRescan ();
                return 0;
            case kCloseView:
            case WM_CLOSE:
                ::DestroyWindow (hwnd);
                return 0;
            case WM_TIMER:
                if (wp == kRescanTimerId)
                {
                    if (++self->rescanPasses > 45)
                    {
                        self->stopPlayerRescan ();
                        if (!self->localVideo && !self->foundRemoteVideo)
                            self->updateStatus (false);
                    }
                    else
                        self->applyPlaybackState ();
                    return 0;
                }
                break;
            case WM_SIZE:
                if (self->controller)
                {
                    RECT rc {};
                    ::GetClientRect (hwnd, &rc);
                    rc.top = kToolbarHeight;
                    self->controller->put_Bounds (rc);
                    if (self->statusLabel)
                        ::SetWindowPos (self->statusLabel, nullptr, 12, 0,
                            (std::max) (1, static_cast<int> (rc.right) - 24), kToolbarHeight,
                            SWP_NOZORDER | SWP_NOACTIVATE);
                }
                return 0;
            case WM_GETMINMAXINFO:
            {
                auto* info = reinterpret_cast<MINMAXINFO*> (lp);
                info->ptMinTrackSize.x = 560;
                info->ptMinTrackSize.y = 380;
                return 0;
            }
            case WM_NCDESTROY:
                self->stopPlayerRescan ();
                if (self->webview && self->navigationStartingRegistered)
                {
                    self->webview->remove_NavigationStarting (self->navigationStartingToken);
                    self->navigationStartingRegistered = false;
                }
                if (self->webview && self->navigationCompletedRegistered)
                {
                    self->webview->remove_NavigationCompleted (self->navigationCompletedToken);
                    self->navigationCompletedRegistered = false;
                }
                if (self->webview4 && self->frameCreatedRegistered)
                {
                    self->webview4->remove_FrameCreated (self->frameCreatedToken);
                    self->frameCreatedRegistered = false;
                }
                self->frames.clear ();
                if (self->webview && self->audioMessageRegistered)
                    self->webview->remove_WebMessageReceived (self->audioMessageToken);
                self->statusLabel = nullptr;
                if (self->controller) self->controller->Close ();
                self->webview4.Reset ();
                self->webview.Reset ();
                self->controller.Reset ();
                self->environment.Reset ();
                {
                    std::lock_guard<std::mutex> lock (self->stateMutex);
                    self->window = nullptr;
                }
                ::PostQuitMessage (0);
                break;
        }
        return ::DefWindowProcW (hwnd, msg, wp, lp);
    }

    void initialize ()
    {
        if (initializing || stopping) return;
        initializing = true;
        const auto data = userDataPath ();
        if (data.empty ())
        {
            fail ();
            return;
        }

        auto self = std::enable_shared_from_this<Impl>::shared_from_this ();
        auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions> ();
        options->put_AdditionalBrowserArguments (L"--autoplay-policy=no-user-gesture-required");
        const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions (nullptr, data.c_str (), options.Get (),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> (
                [self] (HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                    if (self->stopping || !self->currentWindow ()) return S_OK;
                    if (FAILED (result) || !env)
                    {
                        self->fail ();
                        return S_OK;
                    }
                    self->environment = env;
                    return self->environment->CreateCoreWebView2Controller (self->currentWindow (),
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> (
                            [self] (HRESULT controllerResult, ICoreWebView2Controller* value) -> HRESULT {
                                if (self->stopping || !self->currentWindow ()) return S_OK;
                                if (FAILED (controllerResult) || !value)
                                {
                                    self->fail ();
                                    return S_OK;
                                }
                                self->controller = value;
                                if (FAILED (self->controller->get_CoreWebView2 (&self->webview)))
                                {
                                    self->fail ();
                                    return S_OK;
                                }
                                self->audioMessageRegistered = SUCCEEDED (self->webview->add_WebMessageReceived (
                                    Callback<ICoreWebView2WebMessageReceivedEventHandler> (
                                        [self] (ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                            self->receiveAudio (args);
                                            return S_OK;
                                        }).Get (), &self->audioMessageToken));
                                ComPtr<ICoreWebView2_8> audioControl;
                                if (FAILED (self->webview.As (&audioControl)) ||
                                    FAILED (audioControl->put_IsMuted (TRUE)) || !self->audioMessageRegistered)
                                {
                                    self->fail ();
                                    return S_OK;
                                }
                                ComPtr<ICoreWebView2Settings> settings;
                                if (SUCCEEDED (self->webview->get_Settings (&settings)))
                                {
                                    settings->put_IsStatusBarEnabled (FALSE);
                                    settings->put_AreDefaultContextMenusEnabled (TRUE);
                                }
                                RECT rc {};
                                ::GetClientRect (self->currentWindow (), &rc);
                                rc.top = kToolbarHeight;
                                self->controller->put_Bounds (rc);
                                self->statusLabel = ::CreateWindowW (L"STATIC",
                                    L"正在连接视频播放器…", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                                    12, 0, (std::max) (1, static_cast<int> (rc.right) - 24), kToolbarHeight,
                                    self->currentWindow (), nullptr, ::GetModuleHandleW (nullptr), nullptr);
                                ::SendMessageW (self->statusLabel, WM_SETFONT,
                                    reinterpret_cast<WPARAM> (::GetStockObject (DEFAULT_GUI_FONT)), TRUE);
                                if (SUCCEEDED (self->webview.As (&self->webview4)))
                                {
                                    const HRESULT frameResult = self->webview4->add_FrameCreated (
                                        Callback<ICoreWebView2FrameCreatedEventHandler> (
                                            [self] (ICoreWebView2*, ICoreWebView2FrameCreatedEventArgs* args) -> HRESULT {
                                                ComPtr<ICoreWebView2Frame> frame;
                                                if (args && SUCCEEDED (args->get_Frame (&frame)))
                                                {
                                                    ComPtr<ICoreWebView2Frame2> frame2;
                                                    if (SUCCEEDED (frame.As (&frame2)))
                                                    {
                                                        self->frames.push_back (frame2);
                                                        EventRegistrationToken token {};
                                                        std::weak_ptr<Impl> weak = self;
                                                        frame2->add_WebMessageReceived (
                                                            Callback<ICoreWebView2FrameWebMessageReceivedEventHandler> (
                                                                [weak] (ICoreWebView2Frame*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                                                    if (auto impl = weak.lock ()) impl->receiveAudio (args);
                                                                    return S_OK;
                                                                }).Get (), &token);
                                                        // Web players often create their video element after the frame.
                                                        // Rescan on this window's STA thread until it appears.
                                                        if (const HWND hwnd = self->currentWindow ())
                                                            ::PostMessageW (hwnd, kRescanPlayer, 0, 0);
                                                    }
                                                }
                                                return S_OK;
                                            }).Get (), &self->frameCreatedToken);
                                    self->frameCreatedRegistered = SUCCEEDED (frameResult);
                                }
                                const HRESULT navigationStartingResult = self->webview->add_NavigationStarting (
                                    Callback<ICoreWebView2NavigationStartingEventHandler> (
                                        [self] (ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*) -> HRESULT {
                                            self->stopPlayerRescan ();
                                            self->frames.clear ();
                                            self->foundRemoteVideo = false;
                                            ++self->navigationGeneration;
                                            self->scriptsInFlight = 0;
                                            self->playbackDirty = false;
                                            return S_OK;
                                        }).Get (), &self->navigationStartingToken);
                                self->navigationStartingRegistered = SUCCEEDED (navigationStartingResult);
                                const HRESULT navigationCompletedResult = self->webview->add_NavigationCompleted (
                                    Callback<ICoreWebView2NavigationCompletedEventHandler> (
                                        [self] (ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                            self->foundRemoteVideo = false;
                                            self->updateStatus (false);
                                            self->applyPlaybackState ();
                                            self->schedulePlayerRescan ();
                                            return S_OK;
                                        }).Get (), &self->navigationCompletedToken);
                                self->navigationCompletedRegistered = SUCCEEDED (navigationCompletedResult);
                                const HRESULT injected = self->webview->AddScriptToExecuteOnDocumentCreated (
                                    kVideoPlayerScript, Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler> (
                                        [self] (HRESULT error, LPCWSTR) -> HRESULT {
                                            if (self->stopping) return S_OK;
                                            if (FAILED (error)) return self->fail ();
                                            self->initializing = false;
                                            self->setStartup (true);
                                            self->navigate ();
                                            return S_OK;
                                        }).Get ());
                                if (FAILED (injected)) return self->fail ();
                                return S_OK;
                            }).Get ());
                }).Get ());
        if (FAILED (hr)) fail ();
    }

    HRESULT fail ()
    {
        initializing = false;
        setStartup (false);
        if (!stopping)
            if (const HWND hwnd = currentWindow ()) ::PostMessageW (hwnd, WM_CLOSE, 0, 0);
        return E_FAIL;
    }

    void navigate ()
    {
        if (!webview || target.empty ()) return;
        std::wstring destination = target;
        if (localVideo)
        {
            // file: media has an opaque origin and Web Audio returns silence.
            // A private HTTPS folder mapping gives local media a same-origin URL.
            std::vector<wchar_t> path (32768);
            DWORD length = static_cast<DWORD> (path.size ());
            ComPtr<ICoreWebView2_3> mapped;
            if (FAILED (::PathCreateFromUrlW (target.c_str (), path.data (), &length, 0)) ||
                FAILED (webview.As (&mapped))) { fail (); return; }
            std::wstring folder (path.data ());
            folder.resize (folder.find_last_of (L"/\\"));
            if (FAILED (mapped->SetVirtualHostNameToFolderMapping (L"musesync-video.invalid",
                folder.c_str (), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS))) { fail (); return; }
            destination = L"https://musesync-video.invalid/" + target.substr (target.find_last_of (L'/') + 1);
        }
        if (FAILED (webview->Navigate (destination.c_str ()))) fail ();
    }

    void run (const std::wstring& firstTarget)
    {
        MSG queuedMessage {};
        ::PeekMessageW (&queuedMessage, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        {
            std::lock_guard<std::mutex> lock (stateMutex);
            threadId = ::GetCurrentThreadId ();
        }
        if (stopping)
        {
            setStartup (false);
            {
                std::lock_guard<std::mutex> lock (stateMutex);
                runExited = true;
            }
            stateChanged.notify_all ();
            return;
        }
        const HRESULT com = ::CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED (com))
        {
            setStartup (false);
            {
                std::lock_guard<std::mutex> lock (stateMutex);
                runExited = true;
            }
            stateChanged.notify_all ();
            return;
        }

        WNDCLASSEXW wc {};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = windowProc;
        wc.hInstance = ::GetModuleHandleW (nullptr);
        wc.hCursor = ::LoadCursorW (nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH> (COLOR_WINDOW + 1);
        wc.lpszClassName = kWindowClass;
        ::RegisterClassExW (&wc);
        target = firstTarget;
        localVideo = target.size () > 7 && _wcsnicmp (target.c_str (), L"file://", 7) == 0;
        RECT workArea {};
        ::SystemParametersInfoW (SPI_GETWORKAREA, 0, &workArea, 0);
        const int workWidth = static_cast<int> (workArea.right - workArea.left);
        const int workHeight = static_cast<int> (workArea.bottom - workArea.top);
        const int windowWidth = (std::min) ((std::max) (720, workWidth / 2), 860);
        const int windowHeight = (std::min) ((std::max) (460, workHeight / 2), 560);
        const int x = (std::max) (static_cast<int> (workArea.left), static_cast<int> (workArea.right) - windowWidth - 24);
        const int y = (std::max) (static_cast<int> (workArea.top), static_cast<int> (workArea.top) + 48);
        // Start hidden and unattached so startup never waits on the host UI thread.
        const HWND created = ::CreateWindowExW (WS_EX_TOOLWINDOW, kWindowClass,
            L"MuseSync 视频播放器", WS_POPUP | WS_CLIPCHILDREN,
            x, y, windowWidth, windowHeight, nullptr, nullptr, wc.hInstance, this);
        if (!created)
        {
            setStartup (false);
            ::CoUninitialize ();
            std::lock_guard<std::mutex> lock (stateMutex);
            runExited = true;
            stateChanged.notify_all ();
            return;
        }
        if (stopping)
        {
            ::DestroyWindow (created);
            ::CoUninitialize ();
            {
                std::lock_guard<std::mutex> lock (stateMutex);
                runExited = true;
                window = nullptr;
            }
            stateChanged.notify_all ();
            return;
        }
        initialize ();

        MSG msg {};
        while (::GetMessageW (&msg, nullptr, 0, 0) > 0)
        {
            ::TranslateMessage (&msg);
            ::DispatchMessageW (&msg);
        }
        if (::IsWindow (created))
            ::DestroyWindow (created);
        ::CoUninitialize ();
        {
            std::lock_guard<std::mutex> lock (stateMutex);
            runExited = true;
            window = nullptr;
        }
        stateChanged.notify_all ();
    }
};

VideoView::VideoView () : m_impl (std::make_shared<Impl> ())
{}

VideoView::~VideoView () { close (); }

bool VideoView::openLocal (const std::wstring& path)
{
    if (!isVideoFile (path) || ::GetFileAttributesW (path.c_str ()) == INVALID_FILE_ATTRIBUTES)
        return false;
    const DWORD size = ::GetFullPathNameW (path.c_str (), 0, nullptr, nullptr);
    if (!size) return false;
    std::vector<wchar_t> full (static_cast<size_t> (size) + 1);
    if (!::GetFullPathNameW (path.c_str (), static_cast<DWORD> (full.size ()), full.data (), nullptr))
        return false;
    DWORD capacity = static_cast<DWORD> (full.size () * 3 + 16);
    std::vector<wchar_t> url (capacity);
    HRESULT hr = ::UrlCreateFromPathW (full.data (), url.data (), &capacity, 0);
    if (hr == E_POINTER && capacity > url.size ())
    {
        url.resize (capacity);
        hr = ::UrlCreateFromPathW (full.data (), url.data (), &capacity, 0);
    }
    if (FAILED (hr)) return false;
    return openUrl (url.data ());
}

bool VideoView::openUrl (const std::wstring& url)
{
    if (!validTarget (url) || !m_impl || m_impl->stopping) return false;

    if (m_impl->thread.joinable ())
    {
        HWND window = m_impl->currentWindow ();
        bool exited = false;
        {
            std::lock_guard<std::mutex> lock (m_impl->stateMutex);
            exited = m_impl->runExited;
        }
        if (window)
        {
            auto* target = new std::wstring (url);
            if (::PostMessageW (window, kOpenTarget, 0, reinterpret_cast<LPARAM> (target)))
                return true;
            delete target;
        }
        if (!exited) return false;
        m_impl->thread.join ();
    }

    {
        std::lock_guard<std::mutex> lock (m_impl->stateMutex);
        m_impl->startupDone = false;
        m_impl->startupSucceeded = false;
        m_impl->runExited = false;
    }
    auto impl = m_impl;
    impl->thread = std::thread ([impl, url] { impl->run (url); });
    std::unique_lock<std::mutex> lock (impl->stateMutex);
    const bool reported = impl->stateChanged.wait_for (lock, std::chrono::seconds (20),
        [&impl] { return impl->startupDone; });
    return reported && impl->startupSucceeded;
}

void VideoView::sync (double seconds, bool playing, bool discontinuity)
{
    if (!m_impl || m_impl->stopping || !std::isfinite (seconds)) return;
    const HWND window = m_impl->currentWindow ();
    if (!window) return;
    m_impl->audio->synchronize (playing, discontinuity);

    const auto now = std::chrono::steady_clock::now ();
    bool post = false;
    {
        std::lock_guard<std::mutex> lock (m_impl->syncMutex);
        const double target = std::max (0.0, seconds);
        double rate = m_impl->estimatedRate;
        if (!discontinuity && playing && m_impl->hasRateSample && m_impl->lastSyncPlaying)
        {
            const double elapsed = std::chrono::duration<double> (now - m_impl->lastSyncAt).count ();
            const double delta = target - m_impl->lastSyncSeconds;
            // Large deltas are seeks, not a speed change. A bounded rate keeps
            // normal tempo changes smooth without making a seek play too fast.
            if (elapsed >= 0.5 && elapsed <= 2.0 && delta > 0.0 && delta < 8.0)
            {
                const double measured = std::clamp (delta / elapsed, 0.25, 4.0);
                rate += (measured - rate) * .35;
                if (std::abs (rate - 1.0) < .025) rate = 1.0;
                m_impl->lastSyncSeconds = target;
                m_impl->lastSyncAt = now;
            }
        }
        if (discontinuity || !playing || !m_impl->hasRateSample || !m_impl->lastSyncPlaying ||
            now - m_impl->lastSyncAt > std::chrono::seconds (2))
        {
            m_impl->lastSyncSeconds = target;
            m_impl->lastSyncAt = now;
            if (!playing) rate = 1.0;
        }
        m_impl->estimatedRate = rate;
        m_impl->hasRateSample = !discontinuity;
        m_impl->lastSyncPlaying = playing;
        m_impl->queuedSeconds = target;
        m_impl->queuedAt = now;
        m_impl->queuedPlaying = playing;
        m_impl->queuedPlaybackRate = rate;
        m_impl->queuedDiscontinuity = m_impl->queuedDiscontinuity || discontinuity;
        if (!m_impl->syncQueued)
        {
            m_impl->syncQueued = true;
            post = true;
        }
    }
    if (!post) return;
    if (!::PostMessageW (window, kSyncPlayback, 0, 0))
    {
        std::lock_guard<std::mutex> lock (m_impl->syncMutex);
        m_impl->syncQueued = false;
    }
}

double VideoView::durationSec () const
{
    return m_impl ? m_impl->duration.load () : 0.0;
}

std::shared_ptr<VideoAudioStream> VideoView::audioStream () const
{
    return m_impl ? m_impl->audio : nullptr;
}

void VideoView::setTempoAnalyzer (std::shared_ptr<TempoAnalyzer> analyzer)
{
    std::atomic_store (&m_impl->tempo, std::move (analyzer));
}

void VideoView::present (void* parent, int x, int y, int width, int height, bool visible,
                         bool mouseThrough, int opacity)
{
    if (!m_impl || m_impl->stopping) return;
    const HWND window = m_impl->currentWindow ();
    if (!window) return;
    bool post = false;
    {
        std::lock_guard<std::mutex> lock (m_impl->stateMutex);
        m_impl->presentationParent = static_cast<HWND> (parent);
        m_impl->presentationBounds = {x, y, x + width, y + height};
        m_impl->presentationVisible = visible;
        m_impl->presentationMouseThrough = mouseThrough;
        m_impl->presentationOpacity = opacity;
        if (!m_impl->presentationQueued)
        {
            m_impl->presentationQueued = true;
            post = true;
        }
    }
    if (post && !::PostMessageW (window, kPresentView, 0, 0))
    {
        std::lock_guard<std::mutex> lock (m_impl->stateMutex);
        m_impl->presentationQueued = false;
    }
}

void VideoView::close ()
{
    if (!m_impl || !m_impl->thread.joinable ()) return;
    m_impl->stopping = true;
    m_impl->audio->synchronize (false, true);
    HWND window = nullptr;
    DWORD threadId = 0;
    {
        std::lock_guard<std::mutex> lock (m_impl->stateMutex);
        window = m_impl->window;
        threadId = m_impl->threadId;
    }
    if (window)
    {
        if (!::PostMessageW (window, kCloseView, 0, 0))
            ::PostThreadMessageW (threadId, WM_QUIT, 0, 0);
    }
    else if (threadId)
        ::PostThreadMessageW (threadId, WM_QUIT, 0, 0);
    // A child on the video STA can send synchronous messages to its UI parent
    // while closing. Dispatch those messages until that thread has exited.
    const HANDLE handle = m_impl->thread.native_handle ();
    while (::MsgWaitForMultipleObjectsEx (1, &handle, INFINITE, QS_ALLINPUT,
                                         MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0 + 1)
    {
        MSG message {};
        while (::PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                ::PostQuitMessage (static_cast<int> (message.wParam));
                break;
            }
            ::TranslateMessage (&message);
            ::DispatchMessageW (&message);
        }
    }
    m_impl->thread.join ();
}

#endif

} // namespace ap
