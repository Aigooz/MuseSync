#include "video_view.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
void VideoView::close () {}

#else

namespace {
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClass[] = L"MuseSyncVideoView";
constexpr UINT kOpenTarget = WM_APP + 0x361;
constexpr UINT kCloseView = WM_APP + 0x362;

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
    std::wstring target;
    bool initializing = false;
    std::atomic<bool> stopping {false};
    std::thread thread;
    std::mutex stateMutex;
    std::condition_variable stateChanged;
    bool startupDone = false;
    bool startupSucceeded = false;
    bool runExited = false;
    DWORD threadId = 0;

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
                    if (self->webview) self->navigate ();
                    else self->initialize ();
                }
                ::ShowWindow (hwnd, SW_RESTORE);
                ::SetForegroundWindow (hwnd);
                return 0;
            }
            case kCloseView:
            case WM_CLOSE:
                ::DestroyWindow (hwnd);
                return 0;
            case WM_SIZE:
                if (self->controller)
                {
                    RECT rc {};
                    ::GetClientRect (hwnd, &rc);
                    self->controller->put_Bounds (rc);
                }
                return 0;
            case WM_NCDESTROY:
                if (self->controller) self->controller->Close ();
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
        const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions (
            nullptr, data.c_str (), nullptr,
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
                                self->controller->put_Bounds (rc);
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
        const HWND created = ::CreateWindowExW (WS_EX_APPWINDOW, kWindowClass, L"MuseSync 视频播放器",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 960, 600,
            nullptr, nullptr, wc.hInstance, this);
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
