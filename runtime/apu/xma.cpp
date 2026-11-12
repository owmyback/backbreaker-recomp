// XMA hardware decoder emulation.
//
// The XDK's XMA helpers (linked into the game) drive the decoder directly:
//   * register 0x600 holds the physical address of a 320-entry array of 64-byte contexts;
//     a context's ID is its index in that array
//   * kick (enable) / lock (disable) / clear are bitmask registers at 0x650 / 0x690 / 0x6A0,
//     32 contexts per register, written with stwbrx (little-endian)
//   * register 0x606 reports the context the hardware is busy with (XOR 0x200)
// Context layout follows Xenia's XMA_CONTEXT_DATA (16 big-endian dwords of bitfields).
//
// Decoding is SILENT for now: packet headers are parsed to count frames (512 samples each)
// and the output ring is filled with zeros of the right length, so code waiting on decoded
// data makes progress. Real decoding (XMA2 = WMA Pro) needs an XMA-capable decoder.
#include "../kernel/import_support.h"
#include "../log.h"

#include <atomic>
#include <bitset>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

using namespace mem;

namespace
{
    constexpr uint32_t kApuBase = 0x7FEA0000;
    constexpr uint32_t kContextCount = 320;
    constexpr uint32_t kContextSize = 64;
    constexpr uint32_t kPacketSize = 2048;
    constexpr uint32_t kSamplesPerFrame = 512;
    constexpr uint32_t kBlockSize = 256; // output buffer granularity

    enum : uint32_t
    {
        REG_CONTEXT_ARRAY = 0x600,
        REG_CURRENT_CONTEXT = 0x606,
        REG_KICK0 = 0x650,
        REG_LOCK0 = 0x690,
        REG_CLEAR0 = 0x6A0,
    };

    uint32_t g_arrayPhys = 0, g_arrayVirt = 0;
    std::mutex g_lock;
    std::bitset<kContextCount> g_allocated, g_enabled;
    std::condition_variable g_wake;
    std::atomic<bool> g_started{false};
    std::atomic<uint64_t> g_framesDecoded{0};
    // Frames of the current input packet already emitted, per context (hardware-internal state)
    uint32_t g_framesDone[kContextCount] = {};

    // APU registers are little-endian (accessed with lwbrx/stwbrx)
    void setReg(uint32_t r, uint32_t v) { *(uint32_t*)host(kApuBase + r * 4) = v; }

    // ---- XMA_CONTEXT_DATA bitfields (dword index, low bit, width) ----
    struct Field { uint32_t dw, lo, bits; };
    constexpr Field IB0_PACKETS{0, 0, 12}, IB0_VALID{0, 20, 1}, IB1_VALID{0, 21, 1};
    constexpr Field OUT_BLOCKS{0, 22, 5}, OUT_WRITE{0, 27, 5};
    constexpr Field IB1_PACKETS{1, 0, 12}, IS_STEREO{1, 29, 1}, OUT_VALID{1, 31, 1};
    constexpr Field IB_READ_OFFSET{2, 0, 26};
    constexpr Field CURRENT_BUFFER{4, 31, 1};
    constexpr Field OUT_READ{9, 0, 5}, STOP_WHEN_DONE{9, 30, 1};

    struct Context
    {
        uint32_t d[16];
        uint32_t get(Field f) const { return (d[f.dw] >> f.lo) & ((1u << f.bits) - 1); }
        void set(Field f, uint32_t v)
        {
            uint32_t m = ((1u << f.bits) - 1) << f.lo;
            d[f.dw] = (d[f.dw] & ~m) | ((v << f.lo) & m);
        }
    };

    uint32_t contextAddr(uint32_t id) { return g_arrayVirt + id * kContextSize; }

    void load(uint32_t id, Context& c)
    {
        for (int i = 0; i < 16; i++) c.d[i] = read32(contextAddr(id) + i * 4);
    }

    void store(uint32_t id, const Context& c)
    {
        for (int i = 0; i < 16; i++) write32(contextAddr(id) + i * 4, c.d[i]);
    }

    // Frames that begin in an XMA2 packet: top 6 bits of the big-endian packet header.
    uint32_t framesInPacket(uint32_t packetPhys) { return __builtin_bswap32(*(uint32_t*)physical(packetPhys)) >> 26; }

    // Decode (silently) as much as fits. Returns true if the context changed.
    bool work(uint32_t id)
    {
        Context c;
        load(id, c);
        if (!c.get(OUT_VALID))
            return false;

        uint32_t blocks = c.get(OUT_BLOCKS);
        if (blocks == 0) blocks = 32;
        uint32_t outPhys = c.d[7] & 0x1FFFFFFF;
        uint32_t channels = c.get(IS_STEREO) ? 2 : 1;
        uint32_t blocksPerFrame = kSamplesPerFrame * channels * 2 / kBlockSize;
        bool changed = false;

        for (;;)
        {
            uint32_t cur = c.get(CURRENT_BUFFER);
            Field valid = cur ? IB1_VALID : IB0_VALID;
            if (!c.get(valid))
            {
                // Current buffer empty: switch to the other one if it has data
                Field otherValid = cur ? IB0_VALID : IB1_VALID;
                if (!c.get(otherValid)) break;
                c.set(CURRENT_BUFFER, cur ^ 1);
                c.set(IB_READ_OFFSET, 32);
                changed = true;
                continue;
            }
            uint32_t packets = c.get(cur ? IB1_PACKETS : IB0_PACKETS);
            uint32_t inPhys = (cur ? c.d[6] : c.d[5]) & 0x1FFFFFFF;
            uint32_t readBits = c.get(IB_READ_OFFSET);
            if (readBits < 32) readBits = 32;
            uint32_t packet = readBits / (kPacketSize * 8);
            if (packet >= packets)
            {
                // Buffer consumed
                c.set(valid, 0);
                c.set(IB_READ_OFFSET, 32);
                c.set(CURRENT_BUFFER, cur ^ 1);
                g_framesDone[id] = 0;
                changed = true;
                continue;
            }

            uint32_t frames = framesInPacket(inPhys + packet * kPacketSize);
            if (g_framesDone[id] >= frames)
            {
                // Packet finished: move on to the next one
                g_framesDone[id] = 0;
                c.set(IB_READ_OFFSET, (packet + 1) * kPacketSize * 8 + 32);
                changed = true;
                continue;
            }

            // Emit one frame of silence if the output ring has room (one block is kept free so
            // that write == read always means "empty")
            uint32_t write = c.get(OUT_WRITE), readOff = c.get(OUT_READ);
            uint32_t freeBlocks = (readOff + blocks - write - 1) % blocks;
            if (blocksPerFrame > freeBlocks)
                break; // wait for the game to drain the output ring
            for (uint32_t b = 0; b < blocksPerFrame; b++)
            {
                memset(physical(outPhys + write * kBlockSize), 0, kBlockSize);
                write = (write + 1) % blocks;
            }
            c.set(OUT_WRITE, write);
            g_framesDone[id]++;
            // Approximate position inside the packet (real hardware points at the next frame)
            c.set(IB_READ_OFFSET, packet * kPacketSize * 8 + 32 + g_framesDone[id] * (kPacketSize * 8 - 32) / (frames + 1));
            g_framesDecoded++;
            changed = true;
        }

        if (!c.get(IB0_VALID) && !c.get(IB1_VALID) && c.get(STOP_WHEN_DONE))
        {
            std::lock_guard guard(g_lock);
            g_enabled.reset(id);
        }
        if (changed)
            store(id, c);
        return changed;
    }

    void decoderThread()
    {
        for (;;)
        {
            std::bitset<kContextCount> todo;
            {
                std::unique_lock lock(g_lock);
                g_wake.wait_for(lock, std::chrono::milliseconds(2));
                todo = g_enabled;
            }
            for (uint32_t id = 0; id < kContextCount; id++)
                if (todo.test(id))
                    work(id);
        }
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
        info("[xma] %u contexts at %08X (physical %08X)", kContextCount, g_arrayVirt, g_arrayPhys);
        if (!g_started.exchange(true))
            std::thread(decoderThread).detach();
    }

    // Value is in register (little-endian) order.
    void mmioStore(uint32_t reg, uint32_t value)
    {
        setReg(reg, value);
        auto apply = [&](uint32_t first, auto fn) {
            if (reg < first || reg >= first + 10) return false;
            uint32_t baseId = (reg - first) * 32;
            std::lock_guard guard(g_lock);
            for (uint32_t b = 0; b < 32; b++)
                if (value & (1u << b) && baseId + b < kContextCount)
                    fn(baseId + b);
            return true;
        };
        if (apply(REG_KICK0, [](uint32_t id) { g_enabled.set(id); }))
            g_wake.notify_one();
        else if (apply(REG_LOCK0, [](uint32_t id) { g_enabled.reset(id); })) {}
        else if (apply(REG_CLEAR0, [](uint32_t id) {
            g_enabled.reset(id);
            g_framesDone[id] = 0;
            memset(host(contextAddr(id)), 0, kContextSize);
        })) {}
    }

    uint64_t xmaFramesDecoded() { return g_framesDecoded; }
}

// u32 XMACreateContext(void** Context)
GUEST_IMPORT(XMACreateContext)
{
    std::lock_guard guard(g_lock);
    for (uint32_t id = 0; id < kContextCount; id++)
    {
        if (g_allocated.test(id)) continue;
        g_allocated.set(id);
        memset(host(contextAddr(id)), 0, kContextSize);
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
    std::lock_guard guard(g_lock);
    if (id < kContextCount)
    {
        g_allocated.reset(id);
        g_enabled.reset(id);
    }
    ctx.r3.u64 = 0;
}
