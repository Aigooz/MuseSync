#pragma once

#include <memory>
#include <string>

namespace ap {

class VideoView
{
public:
    VideoView ();
    ~VideoView ();
    VideoView (const VideoView&) = delete;
    VideoView& operator= (const VideoView&) = delete;

    bool openLocal (const std::wstring& path);
    bool openUrl (const std::wstring& url);
    void sync (double seconds, bool playing, bool discontinuity = false);
    void close ();

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

bool isVideoFile (const std::wstring& path);

} // namespace ap
