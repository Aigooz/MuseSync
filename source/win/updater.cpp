#include "updater.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <chrono>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace ap::updates {
namespace {
constexpr uint64_t maxInstaller = 128 * 1024 * 1024;
const std::wstring releaseRoot = L"https://github.com/Aigooz/MuseSync/releases/";
struct Internet
{
    HINTERNET value;
    explicit Internet (HINTERNET h) : value (h) { if (!h) throw std::runtime_error ("network"); }
    ~Internet () { const DWORD error = GetLastError (); WinHttpCloseHandle (value); SetLastError (error); }
};
// Async completion keeps cancellation safe while WinHTTP owns an in-flight request.
struct Request
{
    HINTERNET value;
    HANDLE completed = nullptr, closed = nullptr;
    DWORD error = 0, received = 0;
    bool registered = false;
    static void CALLBACK callback (HINTERNET, DWORD_PTR context, DWORD status, void* info, DWORD length)
    {
        auto* self = reinterpret_cast<Request*> (context);
        if (!self) return;
        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) { SetEvent (self->closed); return; }
        if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
            self->error = static_cast<WINHTTP_ASYNC_RESULT*> (info)->dwError;
        else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) self->received = length;
        else if (status != WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE && status != WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE) return;
        SetEvent (self->completed);
    }
    explicit Request (HINTERNET handle) : value (handle)
    {
        try
        {
            if (!value) throw std::runtime_error ("request");
            completed = CreateEventW (nullptr, FALSE, FALSE, nullptr);
            closed = CreateEventW (nullptr, TRUE, FALSE, nullptr);
            if (!completed || !closed) throw std::runtime_error ("network event");
            DWORD_PTR context = reinterpret_cast<DWORD_PTR> (this);
            if (!WinHttpSetOption (value, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof context) ||
                WinHttpSetStatusCallback (value, callback, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS |
                    WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
                throw std::runtime_error ("network callback");
            registered = true;
        }
        catch (...) { close (); throw; }
    }
    void wait (BOOL started, const std::atomic<bool>& cancel)
    {
        if (!started && GetLastError () != ERROR_IO_PENDING) throw std::runtime_error ("network start");
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (30);
        while (!cancel && std::chrono::steady_clock::now () < deadline)
        {
            const auto result = WaitForSingleObject (completed, 50);
            if (result == WAIT_OBJECT_0)
            {
                if (error) { SetLastError (error); throw std::runtime_error ("network completion"); }
                return;
            }
            if (result != WAIT_TIMEOUT) throw std::runtime_error ("network wait");
        }
        SetLastError (cancel ? ERROR_CANCELLED : ERROR_TIMEOUT);
        throw std::runtime_error ("network cancelled or timed out");
    }
    void close ()
    {
        if (value)
        {
            WinHttpCloseHandle (value);
            if (registered) WaitForSingleObject (closed, INFINITE);
            value = nullptr;
        }
        if (completed) CloseHandle (completed);
        if (closed) CloseHandle (closed);
        completed = closed = nullptr;
    }
    ~Request ()
    {
        const DWORD error = GetLastError ();
        close ();
        SetLastError (error);
    }
};
std::array<unsigned long, 3> versionParts (const std::string& text)
{
    static const std::regex pattern (R"((0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5}))");
    std::smatch match;
    if (!std::regex_match (text, match, pattern)) throw std::runtime_error ("version");
    return {std::stoul (match[1]), std::stoul (match[2]), std::stoul (match[3])};
}
std::wstring wide (const std::string& ascii) { return {ascii.begin (), ascii.end ()}; }
std::filesystem::path downloadDirectory ()
{
    PWSTR folder = nullptr;
    if (FAILED (SHGetKnownFolderPath (FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &folder)))
        throw std::runtime_error ("download folder");
    std::filesystem::path root (folder);
    CoTaskMemFree (folder);
    root /= L"MuseSync/Updates";
    std::filesystem::create_directories (root);
    return root;
}
}

bool newerVersion (const std::string& candidate, const std::string& current)
{
    return versionParts (candidate) > versionParts (current);
}

Release parseChecksums (const std::string& text)
{
    if (text.size () > 65536) throw std::runtime_error ("manifest size");
    static const std::regex linePattern (R"(^([a-fA-F0-9]{64}) [ *](MuseSync-Setup-([0-9]+\.[0-9]+\.[0-9]+)-Windows-x64\.exe)\r?$)");
    Release result;
    std::istringstream stream (text);
    std::string line;
    while (std::getline (stream, line))
    {
        std::smatch match;
        if (!std::regex_match (line, match, linePattern)) continue;
        if (!result.filename.empty ()) throw std::runtime_error ("ambiguous manifest");
        result = {match[3], match[2], match[1]};
        versionParts (result.version);
        std::transform (result.sha256.begin (), result.sha256.end (), result.sha256.begin (),
                        [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    }
    if (result.filename.empty ()) throw std::runtime_error ("manifest");
    return result;
}

bool allowedUrl (const std::wstring& url)
{
    if (url.size () > 16384 || url.find_first_of (L"\r\n") != std::wstring::npos) return false;
    URL_COMPONENTS parts {sizeof parts};
    parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD> (-1);
    if (!WinHttpCrackUrl (url.c_str (), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS ||
        parts.nPort != INTERNET_DEFAULT_HTTPS_PORT || parts.dwUserNameLength || parts.dwPasswordLength) return false;
    std::wstring host (parts.lpszHostName, parts.dwHostNameLength);
    std::transform (host.begin (), host.end (), host.begin (), ::towlower);
    return host == L"github.com" || host == L"release-assets.githubusercontent.com" || host == L"objects.githubusercontent.com";
}

void httpsGet (const std::wstring& initial, const Sink& sink, const std::atomic<bool>& cancel)
{
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG proxy {};
    WinHttpGetIEProxyConfigForCurrentUser (&proxy);
    struct ProxyCleanup {
        WINHTTP_CURRENT_USER_IE_PROXY_CONFIG& config;
        ~ProxyCleanup () { GlobalFree (config.lpszProxy); GlobalFree (config.lpszProxyBypass); GlobalFree (config.lpszAutoConfigUrl); }
    } proxyCleanup {proxy};
    const DWORD access = proxy.lpszProxy ? WINHTTP_ACCESS_TYPE_NAMED_PROXY :
        proxy.fAutoDetect || proxy.lpszAutoConfigUrl ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY;
    Internet session (WinHttpOpen (L"MuseSync-Updater/1", access,
                                   proxy.lpszProxy, proxy.lpszProxyBypass, WINHTTP_FLAG_ASYNC));
    WinHttpSetTimeouts (session.value, 5000, 10000, 10000, 10000);
    std::wstring url = initial;
    for (int redirect = 0; redirect <= 5; ++redirect)
    {
        if (cancel || !allowedUrl (url)) throw std::runtime_error ("cancel or URL");
        URL_COMPONENTS parts {sizeof parts};
        parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD> (-1);
        if (!WinHttpCrackUrl (url.c_str (), 0, 0, &parts)) throw std::runtime_error ("URL");
        const std::wstring host (parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path (parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength) path.append (parts.lpszExtraInfo, parts.dwExtraInfoLength);
        Internet connection (WinHttpConnect (session.value, host.c_str (), parts.nPort, 0));
        std::array<char, 65536> buffer;
        Request request (WinHttpOpenRequest (connection.value, L"GET", path.c_str (), nullptr,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!WinHttpSetOption (request.value, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy)) throw std::runtime_error ("redirect policy");
        request.wait (WinHttpSendRequest (request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0,
            reinterpret_cast<DWORD_PTR> (&request)), cancel);
        request.wait (WinHttpReceiveResponse (request.value, nullptr), cancel);
        DWORD code = 0, bytes = sizeof code;
        if (!WinHttpQueryHeaders (request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  nullptr, &code, &bytes, nullptr)) throw std::runtime_error ("HTTP");
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308)
        {
            wchar_t location[16385] {};
            bytes = sizeof location;
            if (!WinHttpQueryHeaders (request.value, WINHTTP_QUERY_LOCATION, nullptr, location, &bytes, nullptr))
                throw std::runtime_error ("redirect");
            url = location;
            if (!url.empty () && url[0] == L'/') url = L"https://" + host + url;
            continue;
        }
        if (code != 200) throw std::runtime_error ("HTTP status");
        DWORD length = 0;
        bytes = sizeof length;
        WinHttpQueryHeaders (request.value, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                             nullptr, &length, &bytes, nullptr);
        if (!sink (nullptr, 0, length)) throw std::runtime_error ("response rejected");
        while (!cancel)
        {
            request.wait (WinHttpReadData (request.value, buffer.data (), static_cast<DWORD> (buffer.size ()), nullptr), cancel);
            const DWORD read = request.received;
            if (!read) return;
            if (!sink (buffer.data (), read, length)) throw std::runtime_error ("response rejected");
        }
        throw std::runtime_error ("cancel");
    }
    throw std::runtime_error ("redirect limit");
}

std::string sha256File (const std::filesystem::path& file)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    struct Cleanup {
        BCRYPT_ALG_HANDLE& algorithm; BCRYPT_HASH_HANDLE& hash;
        ~Cleanup () { if (hash) BCryptDestroyHash (hash); if (algorithm) BCryptCloseAlgorithmProvider (algorithm, 0); }
    } cleanup {algorithm, hash};
    if (BCryptOpenAlgorithmProvider (&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash (algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) throw std::runtime_error ("hash init");
    std::ifstream stream (file, std::ios::binary);
    if (!stream) throw std::runtime_error ("file");
    std::array<char, 65536> buffer;
    while (stream)
    {
        stream.read (buffer.data (), buffer.size ());
        if (BCryptHashData (hash, reinterpret_cast<PUCHAR> (buffer.data ()), static_cast<ULONG> (stream.gcount ()), 0) < 0)
            throw std::runtime_error ("hash");
    }
    if (!stream.eof ()) throw std::runtime_error ("file read");
    std::array<unsigned char, 32> digest {};
    if (BCryptFinishHash (hash, digest.data (), static_cast<ULONG> (digest.size ()), 0) < 0) throw std::runtime_error ("hash finish");
    const char* digits = "0123456789abcdef";
    std::string result;
    for (auto byte : digest) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}

Client::Client (std::string current, Transport transport) : m_current (std::move (current)), m_transport (std::move (transport)) {}
Client::~Client () { stop (); }
Status Client::status () const { std::lock_guard<std::mutex> lock (m_mutex); return m_status; }
void Client::publish (Status next) { std::lock_guard<std::mutex> lock (m_mutex); next.revision = m_status.revision + 1; m_status = std::move (next); }
void Client::cancel () { m_cancel = true; }
void Client::stop () { cancel (); if (m_worker.joinable ()) m_worker.join (); }
void Client::check () { launch (false); }
void Client::download () { launch (true); }
void Client::launch (bool download)
{
    auto s = status ();
    if (s.phase == Phase::checking || s.phase == Phase::downloading) return;
    if (download && (s.release.filename.empty () || !newerVersion (s.release.version, m_current))) return;
    if (m_worker.joinable ()) m_worker.join ();
    m_cancel = false;
    s.phase = download ? Phase::downloading : Phase::checking;
    s.message = download ? L"准备下载…" : L"正在检查更新…";
    s.received = s.total = 0;
    s.installer.clear ();
    publish (s);
    try { m_worker = std::thread ([this, download] { run (download); }); }
    catch (...) { s.phase = Phase::failed; s.message = L"无法启动更新任务，请重试"; publish (s); }
}
void Client::run (bool download)
{
    auto s = status ();
    std::filesystem::path partial;
    try
    {
        if (!download)
        {
            std::string manifest;
            m_transport (releaseRoot + L"latest/download/SHA256SUMS.txt",
                [&] (const char* data, size_t size, uint64_t total) {
                    if (m_cancel || total > 65536 || manifest.size () + size > 65536) return false;
                    if (size) manifest.append (data, size);
                    return true;
                }, m_cancel);
            if (m_cancel) throw std::runtime_error ("cancel");
            s.release = parseChecksums (manifest);
            s.phase = newerVersion (s.release.version, m_current) ? Phase::available : Phase::current;
            s.message = s.phase == Phase::available ? L"发现新版本 " + wide (s.release.version) : L"当前已是最新版本";
            s.installer.clear ();
        }
        else
        {
            GUID id {};
            if (FAILED (CoCreateGuid (&id))) throw std::runtime_error ("download id");
            wchar_t guid[40] {};
            StringFromGUID2 (id, guid, 40);
            const auto folder = downloadDirectory () / guid;
            std::filesystem::create_directory (folder);
            s.installer = folder / wide (s.release.filename);
            partial = s.installer;
            partial += L".part";
            {
                std::ofstream file (partial, std::ios::binary | std::ios::trunc);
                if (!file) throw std::runtime_error ("write");
                m_transport (releaseRoot + L"download/v" + wide (s.release.version) + L"/" + wide (s.release.filename),
                    [&] (const char* data, size_t size, uint64_t total) {
                        if (m_cancel || total > maxInstaller || s.received + size > maxInstaller) return false;
                        if (size) file.write (data, size);
                        if (!file) return false;
                        s.received += size;
                        s.total = total;
                        publish (s);
                        return true;
                    }, m_cancel);
                file.flush ();
                if (!file) throw std::runtime_error ("flush");
            }
            if (m_cancel || !s.received || (s.total && s.received != s.total) || sha256File (partial) != s.release.sha256)
                throw std::runtime_error ("checksum");
            std::filesystem::rename (partial, s.installer);
            s.phase = Phase::ready;
            s.message = L"下载完成，SHA-256 校验通过";
        }
    }
    catch (...)
    {
        std::error_code error;
        if (!partial.empty ()) std::filesystem::remove (partial, error);
        s.installer.clear ();
        s.phase = m_cancel && !s.release.filename.empty () ? Phase::available : Phase::failed;
        s.message = m_cancel ? L"已取消，可重新检查或下载" : download ? L"下载或校验失败，请重试" : L"检查失败，请检查网络后重试";
    }
    publish (s);
}
}
