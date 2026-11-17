// Checks runtime/gpu/frame_stats.h (issue #146): frames, longest frame and game speed per second
// from swap times. No game files, no generated headers:
//   clang++ -std=c++20 -Wall -Wextra -O2 tests/frame_stats_test.cpp -o build/frame_stats_test
//
// Expected speeds are worked out by hand: a frame of dt seconds loses max(0, dt - 1/30) of game time,
// and speed = 1 - lost / the second's length. One 50 ms frame in a second loses 16.7 ms: 98%.
#include "../runtime/gpu/frame_stats.h"

#include <cstdio>

static int failures = 0, checks = 0;

static void expect(const char* what, const gpu::FrameSecond& s, uint32_t frames, uint32_t longestMs, uint32_t speed)
{
    checks++;
    if (s.frames != frames || s.longestMs != longestMs || s.speedPercent != speed)
    {
        failures++;
        printf("FAIL %s: %u frames, longest %u ms, speed %u%%; expected %u, %u ms, %u%%\n", what, s.frames, s.longestMs,
               s.speedPercent, frames, longestMs, speed);
    }
}

// Swaps every `step` seconds from t to t + seconds (exclusive of the end); returns the time after the last
static double swaps(gpu::FrameStats& f, double t, double step, int count)
{
    for (int i = 0; i < count; i++, t += step)
        f.onSwap(t);
    return t;
}

int main()
{
    {
        gpu::FrameStats f;
        expect("no swap yet", f.closeSecond(1.0), 0, 0, 100);
    }
    {
        // Steady 30: 31 swaps from t=0 make 30 frames of 1/30 s, closed at t=1
        gpu::FrameStats f;
        swaps(f, 0.0, 1.0 / 30, 31);
        expect("steady 30 fps", f.closeSecond(1.0), 31, 33, 100);
        swaps(f, 1.0 + 1.0 / 30, 1.0 / 30, 30);
        expect("steady 30 fps, second 2", f.closeSecond(2.0), 30, 33, 100);
    }
    {
        gpu::FrameStats f;
        swaps(f, 0.0, 1.0 / 60, 61);
        expect("steady 60 fps", f.closeSecond(1.0), 61, 17, 100);
    }
    {
        // One 50 ms frame among 1/30 s ones: loses 50 - 33.3 = 16.7 ms of the second
        gpu::FrameStats f;
        double t = swaps(f, 0.0, 1.0 / 30, 10);           // swaps at 0 .. 0.3
        t = t - 1.0 / 30 + 0.050;                          // the late one, at 0.35
        swaps(f, t, 1.0 / 30, 20);                         // 0.35 .. 0.983, all inside the second
        expect("one 50 ms frame", f.closeSecond(1.0), 30, 50, 98);
    }
    {
        // 20 fps evenly: every frame 50 ms, a third of it lost
        gpu::FrameStats f;
        swaps(f, 0.0, 0.050, 21);
        expect("steady 20 fps", f.closeSecond(1.0), 21, 50, 67);
    }
    {
        // A 2 s freeze from t=0.5: the second it starts in, the one it covers, and the one it ends in
        gpu::FrameStats f;
        swaps(f, 0.0, 1.0 / 30, 16);                       // up to t = 0.5
        // second 1 (0 .. 1): 0.5 s open at the close, 0.5 - 1/30 lost
        expect("freeze, its first second", f.closeSecond(1.0), 16, 500, 53);
        // second 2 (1 .. 2): no swap at all, every bit lost
        expect("freeze, a second with no swap", f.closeSecond(2.0), 0, 1500, 0);
        // second 3: the swap at 2.5 ends the gap; the 0.5 s of it in this second are lost, the rest runs
        swaps(f, 2.5, 1.0 / 30, 15);
        gpu::FrameSecond s = f.closeSecond(3.0);
        checks++;
        if (s.frames != 15 || s.longestMs != 2000 || s.speedPercent != 50)
            failures++, printf("FAIL freeze, the second it ends in: %u frames, longest %u ms, speed %u%%; expected 15, 2000 ms, 50%%\n",
                               s.frames, s.longestMs, s.speedPercent);
        // and the next second is clean again
        swaps(f, 3.0, 1.0 / 30, 30);
        expect("after the freeze", f.closeSecond(4.0), 30, 33, 100);
    }
    {
        // A frame that crosses the boundary between two seconds counts in the one it ends in
        gpu::FrameStats f;
        swaps(f, 0.0, 1.0 / 30, 30);                       // last swap at 29/30 = 0.967
        expect("before the boundary", f.closeSecond(1.0), 30, 33, 100);
        f.onSwap(1.0 + 1.0 / 60);                          // a 50 ms gap: 0.033 open at the close, 0.017 after
        swaps(f, 1.0 + 1.0 / 60 + 1.0 / 30, 1.0 / 30, 29);
        gpu::FrameSecond s = f.closeSecond(2.0);
        checks++;
        if (s.longestMs != 50 || s.speedPercent != 98)
            failures++, printf("FAIL across the boundary: longest %u ms, speed %u%%; expected 50 ms, 98%%\n", s.longestMs, s.speedPercent);
    }

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
