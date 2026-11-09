#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace ap {

// Browser PCM enters here; only the VST output is connected to the speakers.
class VideoAudioStream
{
public:
    void synchronize (bool playing, bool discontinuity)
    {
        std::lock_guard<std::mutex> lock (m_mutex);
        if (playing != m_playing || discontinuity)
        {
            m_read = m_count = 0;
            m_phase = 0;
            m_generation.fetch_add (1, std::memory_order_release);
        }
        m_playing = playing;
    }

    uint32_t generation () const { return m_generation.load (std::memory_order_acquire); }

    void push (const float* stereo, size_t frames, uint32_t rate, uint32_t generation)
    {
        if (!stereo || !frames || frames > kCapacity || rate < 8000 || rate > 192000) return;
        std::array<float, kCapacity * 2> sanitized;
        for (size_t i = 0; i < frames * 2; ++i)
            sanitized[i] = std::isfinite (stereo[i]) ? std::clamp (stereo[i], -1.f, 1.f) : 0.f;
        std::lock_guard<std::mutex> lock (m_mutex);
        if (!m_playing || generation != m_generation.load (std::memory_order_relaxed)) return;
        if (m_rate != rate)
        {
            m_read = m_count = 0;
            m_phase = 0;
            m_rate = rate;
        }
        // Bound latency when the host suspends rendering (mute, device change).
        const size_t limit = std::min (kCapacity, static_cast<size_t> (rate * .12));
        if (m_count + frames > limit)
        {
            const size_t discard = std::min (m_count, m_count + frames - limit);
            m_read = (m_read + discard) % kCapacity;
            m_count -= discard;
            m_phase = 0;
        }
        const size_t start = (m_read + m_count) % kCapacity;
        const size_t first = std::min (frames, kCapacity - start);
        std::memcpy (m_samples.data () + start * 2, sanitized.data (), first * 2 * sizeof (float));
        std::memcpy (m_samples.data (), sanitized.data () + first * 2, (frames - first) * 2 * sizeof (float));
        m_count += frames;
    }

    void render (float* left, float* right, size_t frames, double hostRate,
                 float volume, bool hostPlaying)
    {
        std::fill_n (left, frames, 0.f);
        if (right != left) std::fill_n (right, frames, 0.f);
        std::unique_lock<std::mutex> lock (m_mutex, std::try_to_lock);
        if (!lock.owns_lock ()) return;
        if (!hostPlaying || !m_playing)
        {
            m_read = m_count = 0;
            m_phase = 0;
            return;
        }
        if (!(hostRate > 0)) return;
        const double step = m_rate / hostRate;
        for (size_t i = 0; i < frames && m_count > 1; ++i)
        {
            const size_t next = (m_read + 1) % kCapacity;
            const auto sample = [&] (size_t ch) {
                const float a = m_samples[m_read * 2 + ch];
                return (a + (m_samples[next * 2 + ch] - a) * static_cast<float> (m_phase)) * volume;
            };
            const float l = sample (0), r = sample (1);
            left[i] = right == left ? (l + r) * .5f : l;
            if (right != left) right[i] = r;
            m_phase += step;
            const size_t consumed = std::min (m_count, static_cast<size_t> (m_phase));
            m_read = (m_read + consumed) % kCapacity;
            m_count -= consumed;
            m_phase -= consumed;
            if (!m_count) m_phase = 0;
        }
    }

private:
    static constexpr size_t kCapacity = 16384;
    std::mutex m_mutex;
    std::array<float, kCapacity * 2> m_samples {};
    size_t m_read = 0, m_count = 0;
    double m_phase = 0;
    uint32_t m_rate = 48000;
    std::atomic<uint32_t> m_generation {1};
    bool m_playing = false;
};

} // namespace ap
