#pragma once

#include "audiofile.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <thread>

namespace ap {

struct TempoSegment
{
    double start = 0, end = 0, bpm = 0;
};

struct TempoResult
{
    enum class State { Empty, Analyzing, Ready, Live, Error };
    State state = State::Empty;
    double progress = 0;
    std::string error;
    std::vector<TempoSegment> segments;
    double bpmAt (double seconds) const;
};

// A short, locally constant estimate, not a whole-song average or a beat grid.
double estimateSegmentTempo (const float* mono, size_t frames, uint32_t rate);

class TempoAnalyzer
{
public:
    using Decoder = std::function<bool (AudioData&, std::string&, const std::function<bool ()>&)>;
    TempoAnalyzer ();
    ~TempoAnalyzer ();
    void clear ();
    void analyze (std::shared_ptr<const AudioData> audio);
    void decode (Decoder decoder);
    void startLive ();
    void push (const float* stereo, size_t frames, uint32_t rate,
               uint32_t generation, double endSeconds, double playbackRate);
    std::shared_ptr<const TempoResult> snapshot () const;

private:
    struct Block
    {
        std::vector<float> samples;
        uint32_t rate = 0, generation = 0;
        double end = 0, speed = 1;
    };
    void loop ();
    void publish (uint64_t generation, const TempoResult& result);
    void replace (std::shared_ptr<const AudioData> audio, Decoder decoder, bool live);
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_thread;
    bool m_stop = false, m_pending = false, m_live = false;
    std::atomic<uint64_t> m_generation {0};
    std::shared_ptr<const AudioData> m_audio;
    Decoder m_decoder;
    std::deque<Block> m_blocks;
    std::shared_ptr<const TempoResult> m_result;
};

} // namespace ap
