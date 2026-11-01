#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace ap::updates {

struct Release { std::string version, filename, sha256; };
bool newerVersion (const std::string& candidate, const std::string& current);
Release parseChecksums (const std::string& text);
bool allowedUrl (const std::wstring& url);
std::string sha256File (const std::filesystem::path& file);

enum class Phase { idle, checking, current, available, downloading, ready, failed };
struct Status
{
    Phase phase = Phase::idle;
    Release release;
    std::wstring message;
    std::filesystem::path installer;
    uint64_t received = 0, total = 0, revision = 0;
};

// The sink bounds and consumes each response chunk; returning false cancels the request.
using Sink = std::function<bool (const char*, size_t, uint64_t)>;
using Transport = std::function<void (const std::wstring&, const Sink&, const std::atomic<bool>&)>;
void httpsGet (const std::wstring& url, const Sink& sink, const std::atomic<bool>& cancel);

class Client
{
public:
    explicit Client (std::string current, Transport transport = httpsGet);
    ~Client ();
    Status status () const;
    void check ();
    void download ();
    void cancel ();
    void stop ();
private:
    void launch (bool download);
    void run (bool download);
    void publish (Status status);
    std::string m_current;
    Transport m_transport;
    mutable std::mutex m_mutex;
    Status m_status;
    std::atomic<bool> m_cancel {false};
    std::thread m_worker;
};
}
