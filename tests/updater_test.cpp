#include "win/updater.h"
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

using namespace ap::updates;
namespace {
const std::string abcHash = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
const std::string manifest = abcHash + "  MuseSync-Setup-2.10.0-Windows-x64.exe\r\n";
void check (bool value, const char* message) { if (!value) throw std::runtime_error (message); }
template<class F> void rejects (F action, const char* message)
{
    try { action (); } catch (...) { return; }
    throw std::runtime_error (message);
}
Status wait (Client& client)
{
    const auto end = std::chrono::steady_clock::now () + std::chrono::seconds (90);
    while (std::chrono::steady_clock::now () < end)
    {
        const auto s = client.status ();
        if (s.phase != Phase::checking && s.phase != Phase::downloading) return s;
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    throw std::runtime_error ("update timed out");
}
void cleanDownload (const Status& s)
{
    if (s.phase == Phase::ready)
    {
        // Only this test's unique, verified download and its empty directory.
        std::filesystem::remove (s.installer);
        std::filesystem::remove (s.installer.parent_path ());
    }
}
}
int main (int argc, char**)
{
    try
    {
        if (argc > 1)
        {
            Client cancelled ("0.0.0");
            cancelled.check ();
            std::this_thread::sleep_for (std::chrono::milliseconds (100));
            const auto start = std::chrono::steady_clock::now ();
            cancelled.stop ();
            check (std::chrono::steady_clock::now () - start < std::chrono::seconds (3), "live cancellation blocked");
            std::puts ("PASS: live network request cancellation");
            Client live ("0.0.0", [] (const std::wstring& url, const Sink& sink, const std::atomic<bool>& cancel) {
                try { httpsGet (url, sink, cancel); }
                catch (const std::exception& error) { std::fprintf (stderr, "HTTPS: %s (Win32 %lu)\n", error.what (), GetLastError ()); throw; }
            });
            live.check ();
            auto s = wait (live);
            check (s.phase == Phase::available, "live release check failed");
            live.download ();
            s = wait (live);
            check (s.phase == Phase::ready, "live download or checksum failed");
            std::printf ("PASS: live GitHub release %s, %llu bytes, SHA-256 verified\n", s.release.version.c_str (), s.received);
            cleanDownload (s);
            return 0;
        }
        check (newerVersion ("2.10.0", "2.9.9"), "numeric comparison");
        check (!newerVersion ("2.2.0", "2.2.0"), "equal version");
        check (!newerVersion ("2.1.9", "2.2.0"), "older version");
        rejects ([] { newerVersion ("v2.3.0", "2.2.0"); }, "invalid version accepted");
        rejects ([] { newerVersion ("2.3.0-beta", "2.2.0"); }, "prerelease accepted");
        check (parseChecksums (manifest).version == "2.10.0", "manifest parse");
        rejects ([] { parseChecksums (manifest + manifest); }, "ambiguous manifest accepted");
        rejects ([] { parseChecksums (abcHash + "  ../bad.exe"); }, "path traversal accepted");
        rejects ([] { parseChecksums (std::string (65537, 'x')); }, "oversized manifest accepted");
        for (const auto* url : {L"https://github.com/Aigooz/MuseSync/releases/latest", L"https://release-assets.githubusercontent.com/a?sig=123", L"https://objects.githubusercontent.com/a"})
            check (allowedUrl (url), "trusted URL rejected");
        for (const auto* url : {L"http://github.com/a", L"https://github.com.evil.com/a", L"https://evil.com/a", L"https://user@github.com/a", L"https://github.com:444/a", L"file:///a.exe"})
            check (!allowedUrl (url), "unsafe URL accepted");
        bool corrupt = false, truncated = false, cancel = false, failure = false;
        Transport transport = [&] (const std::wstring& url, const Sink& sink, const std::atomic<bool>& cancelled) {
            if (failure) throw std::runtime_error ("offline");
            if (url.find (L"SHA256SUMS.txt") != std::wstring::npos)
            {
                if (!sink (manifest.data (), manifest.size (), manifest.size ())) throw std::runtime_error ("rejected");
            }
            else
            {
                check (url == L"https://github.com/Aigooz/MuseSync/releases/download/v2.10.0/MuseSync-Setup-2.10.0-Windows-x64.exe", "unpinned download URL");
                if (cancel) { while (!cancelled) std::this_thread::sleep_for (std::chrono::milliseconds (1)); return; }
                if (!sink (corrupt ? "bad" : "abc", 3, truncated ? 5 : 3)) throw std::runtime_error ("rejected");
            }
        };
        Client client ("2.2.0", transport);
        failure = true;
        client.check ();
        check (wait (client).phase == Phase::failed, "offline not reported");
        failure = false;
        client.check ();
        check (wait (client).phase == Phase::available, "retry failed");
        client.download ();
        auto s = wait (client);
        check (s.phase == Phase::ready && s.received == 3, "verified download not ready");
        check (sha256File (s.installer) == abcHash, "download hash");
        cleanDownload (s);
        corrupt = true;
        client.download ();
        check (wait (client).phase == Phase::failed && client.status ().installer.empty (), "corrupt download accepted");
        corrupt = false;
        truncated = true;
        client.download ();
        check (wait (client).phase == Phase::failed, "truncated download accepted");
        truncated = false;
        cancel = true;
        client.download ();
        client.cancel ();
        check (wait (client).phase == Phase::available, "cancellation not retryable");
        cancel = false;
        client.download ();
        s = wait (client);
        check (s.phase == Phase::ready, "download retry failed");
        cleanDownload (s);
        Client current ("2.10.0", transport);
        current.check ();
        check (wait (current).phase == Phase::current, "latest version misreported");
        current.download ();
        check (current.status ().phase == Phase::current, "current version downloaded unnecessarily");
        std::puts ("PASS: versions, manifest bounds, trusted HTTPS URLs, hash, retry, cancellation and corruption rejection");
        return 0;
    }
    catch (const std::exception& error) { std::fprintf (stderr, "FAIL: %s\n", error.what ()); return 1; }
}
