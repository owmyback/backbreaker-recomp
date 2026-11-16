// The frame rate (#89): 30 frames a second as on the console, or 60. Two numbers in the game make it
// 30, and 60 changes both:
//
// - How long a frame stays up. Before each swap, the command stream has the CPU call D3D's
//   sub_8241A410 (through an interrupt) with an argument whose bits 8-11 are the present interval:
//   how many vblanks the frame is shown for before the next one may replace it. The game passes
//   0x264, interval 2: 30 fps on the 60 Hz vblank. At 60 it gets interval 1.
// - The gameplay tick. The game object, which sub_82274628 constructs, runs the simulation in fixed
//   steps of its +172 (1/30 s): each frame adds the game clock's time to an accumulator (+168) and
//   runs one step for each 1/30 s in it, so at 60 frames a second each step would be drawn twice. At
//   60 the step is 1/60 s. PhysX takes 1/60 s substeps either way (the game's NxScene::setTiming),
//   now one per step instead of two.
//
// The game clock is what makes either work: it only runs at the right speed since the guest's time
// base runs at the console's rate (kernel/system.cpp). Before that, every frame measured 1/30 s or
// more, and 60 frames a second played at twice the speed.
#include "frame_rate.h"
#include "log.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <atomic>
#include <bit>
#include <cmath>

extern "C" PPC_FUNC(__imp__sub_8241A410);
extern "C" PPC_FUNC(__imp__sub_82274628);

namespace
{
    std::atomic<uint32_t> g_fps{ 30 };

    float loadFloat(const uint8_t* base, uint32_t a) { return std::bit_cast<float>(__builtin_bswap32(*(const uint32_t*)(base + a))); }
    void storeFloat(uint8_t* base, uint32_t a, float v) { *(uint32_t*)(base + a) = __builtin_bswap32(std::bit_cast<uint32_t>(v)); }
}

namespace frame_rate
{
    void set(uint32_t fps) { g_fps = fps; }
    uint32_t get() { return g_fps; }
}

// D3D's swap queue: r3 packs the present interval in bits 8-11 (with a late threshold and a release
// value, left as they are)
PPC_FUNC(sub_8241A410)
{
    const uint32_t interval = (ctx.r3.u32 >> 8) & 0xF;
    if (g_fps == 60 && interval == 2)
        ctx.r3.u32 = (ctx.r3.u32 & ~0xF00u) | 0x100u;
    static bool logged = false;
    if (!logged)
    {
        logged = true;
        info("[gpu] present interval %u (the game asks for %u)", (ctx.r3.u32 >> 8) & 0xF, interval);
    }
    __imp__sub_8241A410(ctx, base);
}

// The game object's constructor: it sets the gameplay tick (+172) to 1/30 s
PPC_FUNC(sub_82274628)
{
    const uint32_t self = ctx.r3.u32;
    __imp__sub_82274628(ctx, base);
    if (g_fps != 60)
        return;
    const float step = loadFloat(base, self + 172);
    if (std::fabs(step - 1.0f / 30) > 1e-6f)
    {
        // not what this expects: leave it (the game keeps its speed, but shows each step twice)
        warn("[game] the gameplay tick at %08X is %f s, not 1/30: left as it is", self + 172, step);
        return;
    }
    storeFloat(base, self + 172, 1.0f / 60);
    info("[game] gameplay tick 1/60 s, for 60 fps (game object %08X)", self);
}
