#include "video_view.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <thread>
#include <vector>

#ifdef MUSESYNC_HAS_WEBVIEW2
#include <wrl.h>
#include <WebView2.h>
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
constexpr UINT_PTR kRescanTimerId = 1;
constexpr int kToolbarHeight = 38;

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
    bool queuedPlaying = false;
    double queuedPlaybackRate = 1.0;
    bool queuedDiscontinuity = false;
    bool syncQueued = false;
    bool startupDone = false;
    bool startupSucceeded = false;
    bool runExited = false;
    DWORD threadId = 0;
    double pendingSeconds = 0.0;
    bool pendingPlaying = false;
    double pendingPlaybackRate = 1.0;
    bool pendingDiscontinuity = false;
    bool hasPendingPlayback = false;
    bool hasRateSample = false;
    bool lastSyncPlaying = false;
    double lastSyncSeconds = 0.0;
    std::chrono::steady_clock::time_point lastSyncAt {};
    int rescanPasses = 0;

    void resetRateEstimate ()
    {
        std::lock_guard<std::mutex> lock (syncMutex);
        hasRateSample = false;
        lastSyncPlaying = false;
        lastSyncAt = {};
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
        wchar_t script[5200] {};
        std::swprintf (script, sizeof script / sizeof script[0],
            L"(()=>{window.__museSyncPlayback={t:%.6f,at:performance.now(),playing:%s,rate:%.4f,local:%s,seek:%s};"
            L"const findVideo=()=>{const vs=Array.from(document.querySelectorAll('video'));if(!vs.length)return null;"
            L"const visible=vs.map(e=>({e,r:e.getBoundingClientRect(),s:getComputedStyle(e)})).filter(x=>"
            L"x.s.visibility!=='hidden'&&x.s.display!=='none'&&(x.r.width>80&&x.r.height>45||x.e.videoWidth>0&&x.e.videoHeight>0));"
            L"return (visible.length?visible:vs.map(e=>({e,r:e.getBoundingClientRect()})))"
            L".sort((a,b)=>b.r.width*b.r.height-a.r.width*a.r.height)[0]?.e||null};"
            L"const frame=()=>{const fs=Array.from(document.querySelectorAll('iframe')).map(e=>({e,r:e.getBoundingClientRect(),s:getComputedStyle(e)}))"
            L".filter(x=>x.s.visibility!=='hidden'&&x.s.display!=='none'&&x.r.width>160&&x.r.height>90)"
            L".sort((a,b)=>b.r.width*b.r.height-a.r.width*a.r.height);return fs[0]?.e||null};"
            L"const applyLayout=(v)=>{if(!v)return;const state=window.__museSyncPlayback||{};"
            L"if(state.local){document.documentElement.style.height='100%%';document.documentElement.style.background='#111';"
            L"document.body.style.height='100%%';document.body.style.margin='0';document.body.style.background='#111';"
            L"document.body.style.display='flex';document.body.style.alignItems='center';document.body.style.justifyContent='center';"
            L"v.style.display='block';v.style.maxWidth='100%%';v.style.maxHeight='100%%';v.style.width='100%%';v.style.height='100%%';v.style.objectFit='contain';v.style.background='#111';}"
            L"};"
            L"const apply=()=>{const state=window.__museSyncPlayback||{};const v=findVideo();"
            L"if(!v){frame();return false}applyLayout(v);"
            L"if(Number.isFinite(state.rate)&&state.rate>0.05&&state.rate<8&&Math.abs(v.playbackRate-state.rate)>0.01){try{v.playbackRate=state.rate}catch(e){}}"
            L"const target=state.t+(state.playing?Math.max(0,performance.now()-state.at)/1000*state.rate:0);"
            L"const tolerance=state.local?0.16:0.32;"
            L"if(Number.isFinite(target)&&(state.seek||!Number.isFinite(v.currentTime)||Math.abs(v.currentTime-target)>tolerance)){try{v.currentTime=target}catch(e){}}"
            L"if(state.playing){if(v.paused){const p=v.play();if(p&&p.catch)p.catch(()=>{})}}else if(!v.paused)v.pause();return true};"
            L"return !!apply()})()",
            std::max (0.0, pendingSeconds), pendingPlaying ? L"true" : L"false",
            pendingPlaybackRate, localVideo ? L"true" : L"false",
            pendingDiscontinuity ? L"true" : L"false");
        return script;
    }

    void applyPlaybackState ()
    {
        if (!webview || !hasPendingPlayback) return;
        if (localVideo) updateStatus (true);
        const auto script = makeSyncScript ();
        auto self = std::enable_shared_from_this<Impl>::shared_from_this ();
        const auto generation = navigationGeneration;
        webview->ExecuteScript (script.c_str (), Callback<ICoreWebView2ExecuteScriptCompletedHandler> (
            [self, generation] (HRESULT error, LPCWSTR result) -> HRESULT {
                if (generation != self->navigationGeneration || self->localVideo) return S_OK;
                if (SUCCEEDED (error) && result && std::wcscmp (result, L"true") == 0)
                {
                    self->foundRemoteVideo = true;
                    self->updateStatus (true);
                }
                return S_OK;
            }).Get ());
        for (auto it = frames.begin (); it != frames.end ();)
        {
            BOOL destroyed = FALSE;
            if (FAILED ((*it)->IsDestroyed (&destroyed)) || destroyed)
            {
                it = frames.erase (it);
                continue;
            }
            auto frame = *it;
            frame->ExecuteScript (script.c_str (),
                Callback<ICoreWebView2ExecuteScriptCompletedHandler> (
                    [self, generation] (HRESULT error, LPCWSTR result) -> HRESULT {
                        if (generation != self->navigationGeneration || self->localVideo) return S_OK;
                        if (SUCCEEDED (error) && result && std::wcscmp (result, L"true") == 0)
                        {
                            self->foundRemoteVideo = true;
                            self->updateStatus (true);
                        }
                        return S_OK;
                    }).Get ());
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
                    ++self->navigationGeneration;
                    if (self->webview) self->navigate ();
                    else self->initialize ();
                }
                ::ShowWindow (hwnd, SW_RESTORE);
                ::SetForegroundWindow (hwnd);
                return 0;
            }
            case kSyncPlayback:
            {
                {
                    std::lock_guard<std::mutex> lock (self->syncMutex);
                    self->pendingSeconds = self->queuedSeconds;
                    self->pendingPlaying = self->queuedPlaying;
                    self->pendingPlaybackRate = self->queuedPlaybackRate;
                    self->pendingDiscontinuity = self->queuedDiscontinuity;
                    self->queuedDiscontinuity = false;
                    self->hasPendingPlayback = true;
                    self->syncQueued = false;
                }
                self->applyPlaybackState ();
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
        const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions (nullptr, data.c_str (), nullptr,
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
                                self->initializing = false;
                                self->setStartup (true);
                                self->navigate ();
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
        if (FAILED (webview->Navigate (target.c_str ()))) fail ();
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
        const HWND created = ::CreateWindowExW (WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kWindowClass,
            L"MuseSync 视频播放器", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
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
        ::ShowWindow (created, SW_SHOW);
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

    const auto now = std::chrono::steady_clock::now ();
    bool post = false;
    {
        std::lock_guard<std::mutex> lock (m_impl->syncMutex);
        const double target = std::max (0.0, seconds);
        double rate = 1.0;
        if (!discontinuity && playing && m_impl->hasRateSample && m_impl->lastSyncPlaying)
        {
            const double elapsed = std::chrono::duration<double> (now - m_impl->lastSyncAt).count ();
            const double delta = target - m_impl->lastSyncSeconds;
            // Large deltas are seeks, not a speed change. A bounded rate keeps
            // normal tempo changes smooth without making a seek play too fast.
            if (elapsed >= 0.08 && elapsed <= 1.5 && delta > 0.0 && delta < 2.5)
                rate = std::clamp (delta / elapsed, 0.25, 4.0);
        }
        m_impl->lastSyncSeconds = target;
        m_impl->lastSyncAt = now;
        m_impl->hasRateSample = !discontinuity;
        m_impl->lastSyncPlaying = playing;
        m_impl->queuedSeconds = target;
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

void VideoView::close ()
{
    if (!m_impl || !m_impl->thread.joinable ()) return;
    m_impl->stopping = true;
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
    m_impl->thread.join ();
}

#endif

} // namespace ap
