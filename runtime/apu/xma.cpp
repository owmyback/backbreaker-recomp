// XMA hardware decoder emulation.
//
// The XDK's XMA helpers (linked into the game, used by FMOD) drive the decoder directly:
//   * register 0x600 holds the physical address of a 320-entry array of 64-byte contexts;
//     a context's ID is its index in that array
//   * kick (enable) / lock (disable) / clear are bitmask registers at 0x650 / 0x690 / 0x6A0,
//     32 contexts per register, written with stwbrx (little-endian)
//   * register 0x606 reports the context the hardware is busy with (XOR 0x200)
// Context layout follows Xenia's XMA_CONTEXT_DATA (16 big-endian dwords of bitfields).
//
// Each kick runs one decoder pass over each kicked context on the kicking thread, as Xenia does
// (XmaDecoder::WriteRegister: every Work() clears the enable flag, and the kicking thread waits
// for it): frames are decoded until the output ring is full or the input runs out, and the game
// kicks again once it has drained the ring. The pass itself is xma_context.h (a port of Xenia's
// XmaContextNew); frames are decoded by xma_frame_decoder.cpp, or played as silence when the
// build has no XMA decoder (see scripts/build_xma_decoder.sh).
//
// Lock waits for a pass in progress on that context (Xenia: Disable + Block), so the game can
// rewrite a locked context safely; a pass's result is merged into the context as it is in guest
// memory at the end, so fields the game wrote meanwhile survive (Xenia StoreContextMerged).
#include "apu.h"
#include "xma_context.h"
#include "xma_frame_decoder.h"
#include "../kernel/import_support.h"
#include "../log.h"

#include <atomic>
#include <bitset>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>

using namespace mem;

namespace
{
    constexpr uint32_t kApuBase = 0x7FEA0000;
    constexpr uint32_t kContextCount = 320;
    constexpr uint32_t kContextSize = 64;

    enum : uint32_t
    {
        REG_CONTEXT_ARRAY = 0x600,
        REG_CURRENT_CONTEXT = 0x606,
        REG_KICK0 = 0x650,
        REG_LOCK0 = 0x690,
        REG_CLEAR0 = 0x6A0,
    };

    struct HwContext
    {
        std::mutex lock; // held for a decoder pass, and by lock/clear/release
        std::unique_ptr<apu::xma::Context> state;
        apu::xma::FrameDecoder* decoder = nullptr;
        bool decoderTried = false;
    };

    uint32_t g_arrayPhys = 0, g_arrayVirt = 0;
    std::mutex g_allocLock;
    std::bitset<kContextCount> g_allocated;
    HwContext g_contexts[kContextCount];

    // Counters (all contexts), read by the [apu] status line
    std::mutex g_statsLock;
    apu::xma::Stats g_stats;
    std::atomic<uint64_t> g_kicks{0}, g_passNs{0}, g_decodeNs{0};
    std::atomic<uint32_t> g_errorsLogged{0};
    bool g_haveDecoder = false; // set once in initXma; without one, frames are silent, not errors

    // APU registers are little-endian (accessed with lwbrx/stwbrx)
    void setReg(uint32_t r, uint32_t v) { *(uint32_t*)host(kApuBase + r * 4) = v; }

    uint32_t contextAddr(uint32_t id) { return g_arrayVirt + id * kContextSize; }

    struct DecodeTarget { HwContext* hw; uint32_t id; };

    uint8_t* hostPhysical(void*, uint32_t address) { return physical(address); }

    bool hostDecode(void* user, int rate, int channels, bool reset, const uint8_t* packet, int size, uint8_t* out)
    {
        auto* t = static_cast<DecodeTarget*>(user);
        HwContext& hw = *t->hw;
        if (!hw.decoderTried)
        {
            hw.decoderTried = true;
            hw.decoder = apu::xma::createFrameDecoder();
        }
        if (!hw.decoder)
            return false;
        auto start = std::chrono::steady_clock::now();
        bool ok = apu::xma::decodeFrame(hw.decoder, rate, channels, reset, packet, size, out);
        g_decodeNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        if (ok)
            return true;
        if (g_errorsLogged.fetch_add(1) < 20)
            warn("[xma] context %u: frame not decoded (%s), played as silence", t->id, apu::xma::lastError(hw.decoder));
        return false;
    }

    // One decoder pass over a context (a kick).
    void kick(uint32_t id)
    {
        g_kicks++;
        HwContext& hw = g_contexts[id];
        std::lock_guard guard(hw.lock);
        auto start = std::chrono::steady_clock::now();
        if (!hw.state)
            hw.state = std::make_unique<apu::xma::Context>();
        uint8_t* guest = host(contextAddr(id));
        const auto initial = apu::xma::ContextData::load(guest);
        auto data = initial;
        DecodeTarget target{ &hw, id };
        apu::xma::Host hostIo{ &target, hostPhysical, g_haveDecoder ? hostDecode : nullptr };
        apu::xma::Stats stats;
        if (hw.state->work(data, hostIo, stats))
            apu::xma::mergeDecoderFields(apu::xma::ContextData::load(guest), data, initial).store(guest);
        g_passNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        std::lock_guard statsGuard(g_statsLock);
        g_stats.framesDecoded += stats.framesDecoded;
        g_stats.framesSilent += stats.framesSilent;
        g_stats.decodeErrors += stats.decodeErrors;
        g_stats.parseErrors += stats.parseErrors;
    }

    void clear(uint32_t id)
    {
        HwContext& hw = g_contexts[id];
        std::lock_guard guard(hw.lock);
        if (!hw.state)
            hw.state = std::make_unique<apu::xma::Context>();
        uint8_t* guest = host(contextAddr(id));
        auto data = apu::xma::ContextData::load(guest);
        hw.state->clear(data);
        data.store(guest);
    }
}

namespace apu
{
    void initXma()
    {
        g_arrayVirt = physicalAlloc(kContextCount * kContextSize, 0x1000);
        g_arrayPhys = physicalAddress(g_arrayVirt);
        setReg(REG_CONTEXT_ARRAY, g_arrayPhys);
        setReg(REG_CURRENT_CONTEXT, 0); // XOR 0x200 never matches a valid ID: hardware never looks busy
        g_haveDecoder = xma::decoderAvailable();
        info("[xma] %u contexts at %08X (physical %08X), decoder: %s", kContextCount, g_arrayVirt, g_arrayPhys,
             g_haveDecoder ? "FFmpeg xmaframes" : "none in this build (XMA plays as silence)");
    }

    // Value is in register (little-endian) order.
    void mmioStore(uint32_t reg, uint32_t value)
    {
        setReg(reg, value);
        auto each = [&](uint32_t first, auto fn) {
            if (reg < first || reg >= first + 10) return false;
            uint32_t baseId = (reg - first) * 32;
            for (uint32_t bits = value; bits; bits &= bits - 1)
                if (uint32_t id = baseId + __builtin_ctz(bits); id < kContextCount)
                    fn(id);
            return true;
        };
        if (each(REG_KICK0, kick)) {}
        else if (each(REG_LOCK0, [](uint32_t id) { std::lock_guard wait(g_contexts[id].lock); })) {}
        else if (each(REG_CLEAR0, clear)) {}
    }

    uint64_t xmaFramesDecoded()
    {
        std::lock_guard guard(g_statsLock);
        return g_stats.framesDecoded + g_stats.framesSilent;
    }

    XmaCounters xmaCounters()
    {
        std::lock_guard guard(g_statsLock);
        return { g_kicks.load(), g_stats.framesDecoded, g_stats.framesSilent, g_stats.decodeErrors, g_stats.parseErrors,
                 g_passNs.load(), g_decodeNs.load() };
    }
}

// u32 XMACreateContext(void** Context)
GUEST_IMPORT(XMACreateContext)
{
    std::lock_guard guard(g_allocLock);
    for (uint32_t id = 0; id < kContextCount; id++)
    {
        if (g_allocated.test(id)) continue;
        g_allocated.set(id);
        {
            std::lock_guard ctxGuard(g_contexts[id].lock);
            memset(host(contextAddr(id)), 0, kContextSize);
            if (g_contexts[id].state)
            {
                auto data = apu::xma::ContextData::load(host(contextAddr(id)));
                g_contexts[id].state->clear(data); // a fresh stream: drop the old one's decoder state
            }
        }
        write32(ctx.r3.u32, contextAddr(id));
        ctx.r3.u64 = 0;
        return;
    }
    warn("[xma] out of contexts");
    ctx.r3.u64 = 0x8007000E; // E_OUTOFMEMORY
}

// u32 XMAReleaseContext(void* Context)
GUEST_IMPORT(XMAReleaseContext)
{
    uint32_t id = (ctx.r3.u32 - g_arrayVirt) / kContextSize;
    if (id < kContextCount)
    {
        std::lock_guard ctxGuard(g_contexts[id].lock); // waits for a pass in progress
        memset(host(contextAddr(id)), 0, kContextSize);
        std::lock_guard guard(g_allocLock);
        g_allocated.reset(id);
    }
    ctx.r3.u64 = 0;
}
