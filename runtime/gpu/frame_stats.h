#pragma once
// Frame timing from the guest's swaps (#146): frames, the longest frame and the game's speed over
// each second, for the [gpu] status line and the overlay's fps readout.
//
// Why speed and not only fps: the game clock counts a frame as at most 1/30 s (docs/runtime.md,
// "Frame rate"), so every frame that takes longer loses game time and the game plays in slow motion,
// while the frame count over a second can still read close to 30. Speed is game time over real time:
// a frame of dt seconds advances the game by min(dt, 1/30), so it loses max(0, dt - 1/30). It is an
// estimate from the swaps' host times; the game measures its frames with its own timer (mftb), which
// runs at the same pace. A loading screen that stops drawing reads as slow too.
//
// Header-only so tests/frame_stats_test.cpp checks it without game files. Not thread-safe on its
// own: the command processor (onSwap) and the vblank thread (closeSecond) share it under a lock.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gpu
{
    // One second of swaps
    struct FrameSecond
    {
        uint32_t frames = 0;          // swaps in the second
        uint32_t longestMs = 0;       // the longest gap between swaps seen in it, rounded
        uint32_t speedPercent = 100;  // game time over real time, rounded (100 before the first swap)
    };

    class FrameStats
    {
    public:
        static constexpr double kClamp = 1.0 / 30.0; // the game clock's longest frame

        // A swap at host time t (seconds, steady clock)
        void onSwap(double t)
        {
            if (m_started)
            {
                const double dt = t - m_last;
                m_lostClosed += std::max(0.0, dt - kClamp);
                m_longest = std::max(m_longest, dt);
            }
            else
            {
                m_started = true;
                m_windowStart = t; // the first second starts at the first swap
            }
            m_last = t;
            m_frames++;
        }

        // Ends the second at host time now and starts the next one there. A gap still open at now
        // (no swap yet) counts toward this second's longest frame and lost time, so a freeze shows in
        // the seconds it lasts, not only in the one where it ends.
        FrameSecond closeSecond(double now)
        {
            FrameSecond s;
            s.frames = m_frames;
            if (!m_started)
                return s;
            const double open = std::max(0.0, now - m_last);
            const double lostNow = m_lostClosed + std::max(0.0, open - kClamp);
            const double window = now - m_windowStart;
            if (window > 0)
            {
                const double speed = 1.0 - (lostNow - m_lostAtWindowStart) / window;
                s.speedPercent = uint32_t(std::lround(std::clamp(speed, 0.0, 1.0) * 100.0));
            }
            s.longestMs = uint32_t(std::lround(std::max(m_longest, open) * 1000.0));
            m_windowStart = now;
            m_lostAtWindowStart = lostNow;
            m_frames = 0;
            m_longest = 0;
            return s;
        }

    private:
        bool m_started = false;
        double m_last = 0;              // the last swap
        double m_windowStart = 0;       // where this second started
        double m_lostClosed = 0;        // game time lost by the gaps between swaps so far, in total
        double m_lostAtWindowStart = 0; // the total lost at m_windowStart, gaps still open included
        double m_longest = 0;           // the longest gap that ended in this second
        uint32_t m_frames = 0;
    };
}
