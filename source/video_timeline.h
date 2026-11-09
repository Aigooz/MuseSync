#pragma once

#include <cmath>

namespace ap {

// Score selection is only a fallback while stopped. An unchanged selection
// must never pull playback back to the note selected before pressing Play.
class VideoTimeline
{
public:
    double update (double hostSeconds, bool hostValid, bool playing,
                   bool selectionValid, double selectionSeconds)
    {
        const bool hostJump = m_initialized && hostValid && m_hostValid &&
            std::abs (hostSeconds - m_hostSeconds) > 1e-4;
        const bool selectionChanged = selectionValid &&
            (!m_selectionValid || std::abs (selectionSeconds - m_selectionSeconds) > 1e-4);
        if (playing || hostJump || !selectionValid) m_override = false;
        if (!playing && selectionChanged) m_override = true;
        m_hostSeconds = hostSeconds;
        m_hostValid = hostValid;
        m_selectionValid = selectionValid;
        if (selectionValid) m_selectionSeconds = selectionSeconds;
        m_initialized = true;
        return m_override ? m_selectionSeconds : hostValid ? hostSeconds : 0.0;
    }

private:
    bool m_initialized = false, m_hostValid = false;
    bool m_selectionValid = false, m_override = false;
    double m_hostSeconds = 0.0, m_selectionSeconds = 0.0;
};

} // namespace ap
