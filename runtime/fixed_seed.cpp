// --fixed-seed: the same random numbers in every test run (issue #71).
//
// Most of the game's randomness - the coin toss, the AI's call, the wind, play variations, crowd
// and animation choices - comes from one global multiply-with-carry generator (state at 0x82CC5430:
// x, carry, multiplier 0x249EB0B2). Five functions draw from it (a u32, a u16, a byte, and two
// floats), each stepping it once through the state getter sub_826431D0. The game seeds it from the
// wall clock (milliseconds * 0x2CD2D + seconds << 8, sub_82643068), on first use and again when a
// match loads. A fixed seed is not enough, though: it is drawn about 10,000 times a second, much of
// it once per frame and from up to five threads, so the value the coin toss gets depended on how
// many frames had run before it and in which order the threads drew.
//
// So under the flag every call site gets its own stream: each draw swaps in the state of the place
// it was called from (its return address), steps it with the game's own code, and keeps the
// result. A one-off decision such as the toss then gets the first number of its own stream however
// long the menus ran, and draws elsewhere can't disturb it. When the game re-seeds its generator
// (match load), every stream starts over. Without the flag the draws are the game's own.
//
// What still varies between runs: anything that depends on frame timing or on when a scripted
// press lands, such as physics and the play after the kickoff.
#include <ppc_config.h>
#include <ppc_context.h>
#include "kernel/kernel.h"

#include <cstdio>
#include <mutex>
#include <unordered_map>

extern "C" PPC_FUNC(__imp__sub_82643068); // seed the generator from the clock
extern "C" PPC_FUNC(__imp__sub_826431D0); // the generator's state (seeded on first use)
extern "C" PPC_FUNC(__imp__sub_82643220); // draw: u32
extern "C" PPC_FUNC(__imp__sub_82643268); // draw: low 16 bits
extern "C" PPC_FUNC(__imp__sub_826432B0); // draw: a byte
extern "C" PPC_FUNC(__imp__sub_826432F8); // draw: float from the low 16 bits
extern "C" PPC_FUNC(__imp__sub_82643358); // draw: float from the low 16 bits, offset

namespace
{
    constexpr uint32_t kState = 0x82CC5430;  // u32 x, u32 carry, u64 multiplier
    constexpr uint32_t kCarry = 0x0002CD2D;   // the carry the game seeds with

    struct Stream
    {
        uint32_t x, carry;
    };

    std::recursive_mutex g_mutex; // recursive: a first draw seeds the global through sub_82643068
    std::unordered_map<uint32_t, Stream> g_streams;

    uint32_t load(uint8_t* base, uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); }
    void store(uint8_t* base, uint32_t a, uint32_t v) { *reinterpret_cast<uint32_t*>(base + a) = __builtin_bswap32(v); }

    // A call site's first state: its address, mixed (any fixed nonzero x will do).
    Stream initial(uint32_t site)
    {
        uint32_t h = site * 0x9E3779B1u;
        h ^= h >> 15;
        h *= 0x85EBCA77u;
        h ^= h >> 13;
        return { h | 1u, kCarry };
    }

    template <void (*Draw)(PPCContext&, uint8_t*)>
    void draw(PPCContext& ctx, uint8_t* base)
    {
        if (!kernel::fixedSeed())
        {
            Draw(ctx, base);
            return;
        }
        const uint32_t site = uint32_t(ctx.lr);
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        PPCContext getter = ctx;
        __imp__sub_826431D0(getter, base); // seeds the global first if needed, so the draw can't
        auto it = g_streams.find(site);
        const Stream s = it != g_streams.end() ? it->second : initial(site);
        store(base, kState, s.x);
        store(base, kState + 4, s.carry);
        Draw(ctx, base);
        g_streams[site] = { load(base, kState), load(base, kState + 4) };
    }
}

PPC_FUNC(sub_82643220) { draw<__imp__sub_82643220>(ctx, base); }
PPC_FUNC(sub_82643268) { draw<__imp__sub_82643268>(ctx, base); }
PPC_FUNC(sub_826432B0) { draw<__imp__sub_826432B0>(ctx, base); }
PPC_FUNC(sub_826432F8) { draw<__imp__sub_826432F8>(ctx, base); }
PPC_FUNC(sub_82643358) { draw<__imp__sub_82643358>(ctx, base); }

PPC_FUNC(sub_82643068)
{
    __imp__sub_82643068(ctx, base);
    if (!kernel::fixedSeed())
        return;
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!g_streams.empty())
        std::fprintf(stderr, "[rng] the game re-seeded its generator: %zu call-site streams start over\n", g_streams.size());
    g_streams.clear();
}
