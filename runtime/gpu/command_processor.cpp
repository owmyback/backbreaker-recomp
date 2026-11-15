// PM4 command processor (headless). Executes the ring buffer and indirect buffers for their
// side effects: register writes, memory writes, fences, waits, interrupts and swaps.
// Packet formats follow Xenia's command_processor.cc.
#include "gpu.h"
#include "capture.h"
#include "renderer.h"
#include "shader_hash.h"
#include "../video/presenter.h"
#include "../kernel/objects.h"
#include "../apu/apu.h"
#include "../log.h"
#include "../memory.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>
#include <map>
#include <cstdio>

using namespace mem;

namespace gpu
{
    namespace
    {
        // PM4 type-3 opcodes
        enum : uint32_t
        {
            PM4_NOP = 0x10, PM4_REG_RMW = 0x21, PM4_DRAW_INDX = 0x22, PM4_VIZ_QUERY = 0x23,
            PM4_WAIT_FOR_IDLE = 0x26, PM4_IM_LOAD = 0x27, PM4_IM_LOAD_IMMEDIATE = 0x2B,
            PM4_SET_CONSTANT = 0x2D, PM4_LOAD_ALU_CONSTANT = 0x2F, PM4_DRAW_INDX_2 = 0x36,
            PM4_INDIRECT_BUFFER_PFD = 0x37, PM4_INVALIDATE_STATE = 0x3B, PM4_WAIT_REG_MEM = 0x3C,
            PM4_MEM_WRITE = 0x3D, PM4_REG_TO_MEM = 0x3E, PM4_INDIRECT_BUFFER = 0x3F,
            PM4_COND_WRITE = 0x45, PM4_EVENT_WRITE = 0x46, PM4_ME_INIT = 0x48,
            PM4_INTERRUPT = 0x54, PM4_SET_CONSTANT2 = 0x55, PM4_EVENT_WRITE_SHD = 0x58,
            PM4_EVENT_WRITE_EXT = 0x5A, PM4_EVENT_WRITE_ZPD = 0x5B, PM4_SET_BIN_MASK = 0x50,
            PM4_SET_BIN_SELECT = 0x51, PM4_SET_BIN_MASK_LO = 0x60, PM4_SET_BIN_MASK_HI = 0x61,
            PM4_SET_BIN_SELECT_LO = 0x62, PM4_SET_BIN_SELECT_HI = 0x63,
        };

        // Predicated tiling: D3D replays a command buffer once per screen tile ("bin"); packets with
        // the predicate bit (header bit 0) only run when the selected bins overlap the bin mask.
        // Both start as 0xFFFFFFFF, as in Xenia's command_processor.h. Only the command processor
        // thread uses them.
        uint64_t g_binMask = 0xFFFFFFFFull, g_binSelect = 0xFFFFFFFFull;

        // Register file lives in guest memory (big-endian) so CPU MMIO reads see it directly.
        inline uint32_t* regPtr(uint32_t i) { return (uint32_t*)host(kGpuMmioBase + i * 4); }

        std::mutex g_wakeLock;
        std::condition_variable g_wake;
        std::atomic<bool> g_cpStarted{false}, g_vblankStarted{false};

        uint32_t g_ringBase = 0, g_ringDwords = 0;
        uint32_t g_rptr = 0;
        uint32_t g_rptrWriteback = 0;

        uint32_t g_callback = 0, g_callbackData = 0;
        std::mutex g_interruptLock;
        kernel::GuestThread* g_irqThread = nullptr;
        kernel::Thread* g_irqThreadObj = nullptr;
        PPCContext* g_irqCtx = nullptr;

        // Stats
        std::atomic<uint64_t> g_frames{0}, g_draws{0}, g_packets{0};
        uint32_t g_frontBuffer = 0, g_frontWidth = 0, g_frontHeight = 0;
        uint64_t g_counter = 0; // EVENT_WRITE_SHD counter

        uint32_t gpuSwap(uint32_t v, uint32_t endian)
        {
            switch (endian & 3)
            {
            case 1: return ((v >> 8) & 0x00FF00FF) | ((v << 8) & 0xFF00FF00); // 8in16
            case 2: return __builtin_bswap32(v);                                // 8in32
            case 3: return (v >> 16) | (v << 16);                               // 16in32
            default: return v;
            }
        }

        // Memory accesses with GPU endian modes: the address carries the mode in its low 2 bits.
        void storeGpu(uint32_t addr, uint32_t v) { *(uint32_t*)physical(addr & ~3u) = gpuSwap(v, addr & 3); }
        uint32_t loadGpu(uint32_t addr) { return gpuSwap(*(uint32_t*)physical(addr & ~3u), addr & 3); }

        struct Reader
        {
            uint32_t base, dwords, pos;
            bool ring;
            uint32_t next()
            {
                uint32_t v = __builtin_bswap32(*(uint32_t*)physical(base + pos * 4));
                pos = ring ? (pos + 1) % dwords : pos + 1;
                return v;
            }
            void skip(uint32_t n) { pos = ring ? (pos + n) % dwords : pos + n; }
        };

        void bindInterruptThread()
        {
            kernel::setCurrentThread(g_irqThread);
            kernel::adoptExistingThread(g_irqThreadObj);
        }

        void dispatchInterrupt(uint32_t source, uint32_t cpu)
        {
            if (!g_callback)
                return;
            std::lock_guard guard(g_interruptLock);
            PPCContext& ctx = *g_irqCtx;
            ctx.r1.u64 = g_irqThread->stackBase - 0x100;
            ctx.r3.u64 = source;
            ctx.r4.u64 = g_callbackData;
            ctx.lr = 0;
            write8(g_irqThread->pcr + 0x10C, uint8_t(cpu));
            (PPC_LOOKUP_FUNC(base, g_callback))(ctx, base);
        }

        bool compare(uint32_t func, uint32_t value, uint32_t ref)
        {
            switch (func & 7)
            {
            case 0: return false;
            case 1: return value < ref;
            case 2: return value <= ref;
            case 3: return value == ref;
            case 4: return value != ref;
            case 5: return value >= ref;
            case 6: return value > ref;
            default: return true;
            }
        }

        void execute(Reader& r, uint32_t endPos);
        void syncAll(const char* why);
        void completeDeferred(bool wait);
        enum class Target : uint8_t { Memory, Register, Rptr }; // where a deferred write goes (#73)
        void writeWhenGpuDone(Target target, uint32_t addr, uint32_t value, const char* why);

        void executeIndirect(uint32_t addr, uint32_t dwords)
        {
            Reader ib{ addr & 0x1FFFFFFC, dwords, 0, false };
            execute(ib, dwords);
        }

        void type3(Reader& r, uint32_t header)
        {
            uint32_t op = (header >> 8) & 0x7F;
            uint32_t count = ((header >> 16) & 0x3FFF) + 1;
            g_packets++;
            capture::onPacket(op);
            if (header & 1)
            {
                static bool logged = false;
                if (!logged)
                {
                    logged = true;
                    info("[gpu] predicated packets in use (bin mask %016llX, select %016llX)", (unsigned long long)g_binMask,
                         (unsigned long long)g_binSelect);
                }
                // A predicated swap is never valid (Xenia skips it too)
                if (!(g_binSelect & g_binMask) || op == PM4_XE_SWAP)
                {
                    r.skip(count);
                    return;
                }
            }
            switch (op)
            {
            case PM4_INDIRECT_BUFFER:
            case PM4_INDIRECT_BUFFER_PFD:
            {
                uint32_t addr = r.next(), size = r.next() & 0xFFFFF;
                r.skip(count - 2);
                executeIndirect(addr, size);
                return;
            }
            case PM4_WAIT_REG_MEM:
            {
                uint32_t info = r.next(), addr = r.next(), ref = r.next(), mask = r.next();
                r.next(); // poll interval
                r.skip(count - 5);
                auto start = std::chrono::steady_clock::now();
                bool warned = false, first = true;
                for (;;)
                {
                    uint32_t value = (info & 0x10) ? loadGpu(addr) : readRegister(addr & 0x7FFF);
                    if (compare(info, value & mask, ref))
                    {
                        render::addCommandWaitMs(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                        break;
                    }
                    if (first)
                    {
                        first = false;
                        render::submitBeforeWait(); // a real wait: the GPU can work on what is recorded meanwhile
                    }
                    completeDeferred(false); // what it waits for may be a fence the GPU has just reached
                    std::this_thread::yield();
                    if (!warned && std::chrono::steady_clock::now() - start > std::chrono::seconds(1))
                    {
                        warn("[gpu] WAIT_REG_MEM stuck >1s: %s %08X & %08X func %u ref %08X (value %08X)",
                             (info & 0x10) ? "mem" : "reg", addr, mask, info & 7, ref, value);
                        warned = true;
                    }
                }
                return;
            }
            case PM4_COND_WRITE:
            {
                uint32_t info = r.next(), pollAddr = r.next(), ref = r.next(), mask = r.next();
                uint32_t writeAddr = r.next(), data = r.next();
                r.skip(count - 6);
                syncAll("cond_write");
                uint32_t value = (info & 0x10) ? loadGpu(pollAddr) : readRegister(pollAddr & 0x7FFF);
                if (compare(info, value & mask, ref))
                {
                    if (info & 0x100) storeGpu(writeAddr, data);
                    else writeRegister(writeAddr & 0x7FFF, data);
                }
                return;
            }
            case PM4_REG_RMW:
            {
                uint32_t info = r.next(), andMask = r.next(), orMask = r.next();
                r.skip(count - 3);
                uint32_t value = readRegister(info & 0x1FFF);
                value &= (info >> 31) ? readRegister(andMask & 0x1FFF) : andMask;
                value |= ((info >> 30) & 1) ? readRegister(orMask & 0x1FFF) : orMask;
                writeRegister(info & 0x1FFF, value);
                return;
            }
            case PM4_REG_TO_MEM:
            {
                uint32_t regIndex = r.next(), addr = r.next();
                r.skip(count - 2);
                syncAll("reg_to_mem");
                storeGpu(addr, readRegister(regIndex & 0x7FFF));
                return;
            }
            case PM4_MEM_WRITE:
            {
                uint32_t addr = r.next();
                syncAll("mem_write");
                for (uint32_t i = 0; i < count - 1; i++)
                    storeGpu((addr & ~3u) + i * 4 | (addr & 3), r.next());
                return;
            }
            case PM4_EVENT_WRITE_SHD:
            {
                uint32_t initiator = r.next(), addr = r.next(), value = r.next();
                r.skip(count - 3);
                writeRegister(reg::VGT_EVENT_INITIATOR, initiator & 0x3F);
                writeWhenGpuDone(Target::Memory, addr, (initiator >> 31) ? uint32_t(g_counter) : value, "event_write_shd");
                return;
            }
            case PM4_EVENT_WRITE_EXT:
            {
                uint32_t initiator = r.next(), addr = r.next();
                r.skip(count - 2);
                syncAll("event_write_ext");
                writeRegister(reg::VGT_EVENT_INITIATOR, initiator & 0x3F);
                // Screen extents (min/max x, y, z) as u16s: report "everything"
                uint16_t ext[6] = { 0, 0x2000 >> 3, 0, 0x2000 >> 3, 0, 1 };
                for (int i = 0; i < 6; i++)
                    *(uint16_t*)physical((addr & ~3u) + i * 2) = __builtin_bswap16(ext[i]);
                return;
            }
            case PM4_EVENT_WRITE:
            {
                uint32_t initiator = r.next();
                r.skip(count - 1);
                writeRegister(reg::VGT_EVENT_INITIATOR, initiator & 0x3F);
                return;
            }
            case PM4_INTERRUPT:
            {
                uint32_t cpuMask = r.next();
                r.skip(count - 1);
                syncAll("interrupt");
                for (uint32_t cpu = 0; cpu < 6; cpu++)
                    if (cpuMask & (1u << cpu))
                        dispatchInterrupt(1, cpu);
                return;
            }
            case PM4_SET_CONSTANT:
            {
                uint32_t offsetType = r.next();
                uint32_t index = offsetType & 0x7FF;
                static const uint32_t bases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
                uint32_t type = (offsetType >> 16) & 0xFF;
                uint32_t baseIndex = type < 5 ? bases[type] : 0x2000;
                for (uint32_t i = 0; i < count - 1; i++)
                    writeRegister(baseIndex + index + i, r.next());
                return;
            }
            case PM4_SET_CONSTANT2:
            {
                uint32_t index = r.next() & 0xFFFF;
                for (uint32_t i = 0; i < count - 1; i++)
                    writeRegister(index + i, r.next());
                return;
            }
            case PM4_LOAD_ALU_CONSTANT:
            {
                uint32_t addr = r.next() & 0x3FFFFFFF, offsetType = r.next(), size = r.next();
                r.skip(count - 3);
                static const uint32_t bases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
                uint32_t type = (offsetType >> 16) & 0xFF;
                uint32_t baseIndex = (type < 5 ? bases[type] : 0x2000) + (offsetType & 0x7FF);
                for (uint32_t i = 0; i < size; i++)
                    writeRegister(baseIndex + i, __builtin_bswap32(*(uint32_t*)physical(addr + i * 4)));
                return;
            }
            case PM4_DRAW_INDX:
            {
                // [0] viz query, [1] VGT_DRAW_INITIATOR, then for DMA indices: [2] base, [3] size
                r.next();
                uint32_t initiator = r.next(), indexBase = 0, indexSize = 0;
                uint32_t used = 2;
                if (((initiator >> 6) & 3) == 0 && count >= 4)
                {
                    indexBase = r.next();
                    indexSize = r.next();
                    used = 4;
                }
                r.skip(count - used);
                g_draws++;
                capture::onDraw(op, initiator, indexBase, indexSize);
                render::onDraw(initiator, indexBase, indexSize);
                return;
            }
            case PM4_DRAW_INDX_2:
            {
                // [0] VGT_DRAW_INITIATOR, then immediate indices
                uint32_t initiator = r.next();
                r.skip(count - 1);
                g_draws++;
                capture::onDraw(op, initiator, 0, 0);
                render::onDraw(initiator, 0, 0);
                return;
            }
            case PM4_IM_LOAD:
            {
                // [0] shader address | type, [1] start << 16 | size in dwords
                uint32_t addrType = r.next(), startSize = r.next();
                r.skip(count - 2);
                uint32_t size = startSize & 0xFFFF;
                const uint32_t* words = (const uint32_t*)physical(addrType & ~3u);
                uint64_t hash = shaderHash(words, size); // once, for both
                capture::onShaderLoad(addrType & 3, startSize >> 16, size, words, hash);
                render::onShaderLoad(addrType & 3, words, size, hash);
                return;
            }
            case PM4_IM_LOAD_IMMEDIATE:
            {
                // [0] type, [1] start << 16 | size in dwords, then the microcode
                uint32_t type = r.next(), startSize = r.next();
                uint32_t size = std::min<uint32_t>(startSize & 0xFFFF, count - 2);
                std::vector<uint32_t> words(size);
                for (uint32_t i = 0; i < size; i++) words[i] = __builtin_bswap32(r.next()); // keep big-endian
                r.skip(count - 2 - size);
                uint64_t hash = shaderHash(words.data(), size); // once, for both
                capture::onShaderLoad(type & 3, startSize >> 16, size, words.data(), hash);
                render::onShaderLoad(type & 3, words.data(), size, hash);
                return;
            }
            case PM4_XE_SWAP:
            {
                g_frontBuffer = r.next();
                g_frontWidth = r.next();
                g_frontHeight = r.next();
                r.skip(count - 3);
                g_frames++;
                g_counter++;
                capture::onSwap(g_frames, g_frontBuffer, g_frontWidth, g_frontHeight);
                render::onSwap(g_frontBuffer, g_frontWidth, g_frontHeight, g_frames); // before publishFrame
                video::publishFrame(g_frontBuffer, g_frontWidth, g_frontHeight, g_frames);
                return;
            }
            case PM4_SET_BIN_MASK:
            case PM4_SET_BIN_SELECT:
            {
                uint64_t hi = r.next(), lo = r.next();
                r.skip(count - 2);
                (op == PM4_SET_BIN_MASK ? g_binMask : g_binSelect) = (hi << 32) | lo;
                return;
            }
            case PM4_SET_BIN_MASK_LO:
            case PM4_SET_BIN_MASK_HI:
            case PM4_SET_BIN_SELECT_LO:
            case PM4_SET_BIN_SELECT_HI:
            {
                uint64_t v = r.next();
                r.skip(count - 1);
                uint64_t& target = (op == PM4_SET_BIN_MASK_LO || op == PM4_SET_BIN_MASK_HI) ? g_binMask : g_binSelect;
                if (op == PM4_SET_BIN_MASK_LO || op == PM4_SET_BIN_SELECT_LO)
                    target = (target & 0xFFFFFFFF00000000ull) | v;
                else
                    target = (target & 0xFFFFFFFFull) | (v << 32);
                return;
            }
            default:
                // ME_INIT, NOP, IM_LOAD*, INVALIDATE_STATE, VIZ_QUERY, WAIT_FOR_IDLE, ...
                r.skip(count);
                return;
            }
        }

        // Scratch registers are mirrored to memory: D3D's fences
        // Guest-visible writes that must wait until the GPU has run everything before them (#73): the
        // game's fences (EVENT_WRITE_SHD to memory, D3D's scratch registers) and the read pointer. The
        // console's CP writes them when the GPU gets there; so do we, instead of stopping until it
        // has. Applied strictly in order.
        struct DeferredWrite { uint64_t serial; Target target; uint32_t addr, value; };
        std::deque<DeferredWrite> g_deferred;
        uint32_t g_pollTick = 0;

        void publishRptr(uint32_t rptr)
        {
            *regPtr(reg::CP_RB_RPTR) = __builtin_bswap32(rptr);
            if (g_rptrWriteback)
                *(uint32_t*)physical(g_rptrWriteback) = __builtin_bswap32(rptr);
        }

        void applyWrite(const DeferredWrite& d)
        {
            switch (d.target)
            {
            case Target::Memory: storeGpu(d.addr, d.value); break;
            case Target::Register: writeRegister(d.addr, d.value); break;
            case Target::Rptr: publishRptr(d.value); break;
            }
        }

        // Apply the deferred writes whose GPU work has finished (all of them with `wait`)
        void completeDeferred(bool wait)
        {
            if (g_deferred.empty()) return;
            if (wait) render::waitGpu(g_deferred.back().serial);
            uint64_t done = render::gpuDone(); // copies the finished GPU writes to guest memory first
            while (!g_deferred.empty() && g_deferred.front().serial <= done)
            {
                applyWrite(g_deferred.front());
                g_deferred.pop_front();
            }
        }

        // A write the guest may take as "the GPU got here": now if nothing is outstanding, otherwise
        // queued behind the GPU work recorded before it
        void writeWhenGpuDone(Target target, uint32_t addr, uint32_t value, const char* why)
        {
            if (!render::asyncFences())
            {
                render::syncGuestMemory(why);
                applyWrite({ 0, target, addr, value });
                return;
            }
            uint64_t serial = render::fenceSerial();
            if (!g_deferred.empty()) serial = std::max(serial, g_deferred.back().serial); // keep the order
            if (!serial) { applyWrite({ 0, target, addr, value }); return; }
            g_deferred.push_back({ serial, target, addr, value });
        }

        // Every other point where the guest can look: GPU writes and deferred writes all land first
        void syncAll(const char* why)
        {
            render::syncGuestMemory(why);
            completeDeferred(true);
        }

        bool touchesScratch(uint32_t index, uint32_t count)
        {
            return index < reg::SCRATCH_REG0 + 8 && index + count > reg::SCRATCH_REG0;
        }

        void execute(Reader& r, uint32_t endPos)
        {
            while (r.pos != endPos)
            {
                if (!g_deferred.empty() && (++g_pollTick & 63) == 0) completeDeferred(false);
                uint32_t header = r.next();
                switch (header >> 30)
                {
                case 0: // write `count` consecutive registers (or one register repeatedly)
                {
                    uint32_t count = ((header >> 16) & 0x3FFF) + 1;
                    uint32_t index = header & 0x7FFF;
                    bool oneReg = header & 0x8000;
                    bool scratch = touchesScratch(index, oneReg ? 1 : count); // D3D fences: written when the GPU gets there
                    for (uint32_t i = 0; i < count; i++)
                    {
                        uint32_t reg = oneReg ? index : index + i, value = r.next();
                        if (scratch && touchesScratch(reg, 1)) writeWhenGpuDone(Target::Register, reg, value, "scratch_reg");
                        else writeRegister(reg, value);
                    }
                    break;
                }
                case 1: // two registers
                {
                    uint32_t r0 = header & 0x7FF, r1 = (header >> 11) & 0x7FF;
                    for (uint32_t reg : { r0, r1 })
                    {
                        uint32_t value = r.next();
                        if (touchesScratch(reg, 1)) writeWhenGpuDone(Target::Register, reg, value, "scratch_reg");
                        else writeRegister(reg, value);
                    }
                    break;
                }
                case 2: // nop
                    break;
                case 3:
                    type3(r, header);
                    break;
                }
            }
        }

        void commandProcessorThread()
        {
            bindInterruptThread();
            info("[gpu] command processor running (ring %08X, %u dwords)", g_ringBase, g_ringDwords);
            for (;;)
            {
                uint32_t wptr;
                if ((__builtin_bswap32(*regPtr(reg::CP_RB_WPTR)) % g_ringDwords) == g_rptr)
                    render::submitBeforeWait(); // no new commands yet: the GPU can work on what is recorded meanwhile
                completeDeferred(false);
                {
                    std::unique_lock lock(g_wakeLock);
                    g_wake.wait_for(lock, g_deferred.empty() ? std::chrono::microseconds(1000) : std::chrono::microseconds(100), [&] {
                        return (__builtin_bswap32(*regPtr(reg::CP_RB_WPTR)) % g_ringDwords) != g_rptr;
                    });
                    wptr = __builtin_bswap32(*regPtr(reg::CP_RB_WPTR)) % g_ringDwords;
                }
                if (wptr == g_rptr)
                    continue;
                Reader ring{ g_ringBase, g_ringDwords, g_rptr, true };
                execute(ring, wptr);
                g_rptr = wptr;
                // A CPU waiting for the GPU to go idle polls the read pointer: it moves when the GPU has
                // run the commands, as the fences do
                writeWhenGpuDone(Target::Rptr, 0, wptr, "rptr");
            }
        }

        void vblankThread()
        {
            bindInterruptThread();
            using clock = std::chrono::steady_clock;
            auto next = clock::now();
            auto lastReport = next;
            uint64_t lastFrames = 0, lastDraws = 0;
            for (;;)
            {
                next += std::chrono::microseconds(16667);
                std::this_thread::sleep_until(next);
                dispatchInterrupt(0, 2);
                if (clock::now() - lastReport >= std::chrono::seconds(1))
                {
                    uint64_t f = g_frames, d = g_draws;
                    uint64_t vmb, pmb;
                    mem::usage(vmb, pmb);
                    info("[gpu] %llu fps, %llu draws/s, %llu frames total, front buffer %08X %ux%u, %llu xma frames, mem %llu+%llu MB",
                         (unsigned long long)(f - lastFrames), (unsigned long long)(d - lastDraws),
                         (unsigned long long)f, g_frontBuffer, g_frontWidth, g_frontHeight,
                         (unsigned long long)apu::xmaFramesDecoded(), (unsigned long long)vmb, (unsigned long long)pmb);
                    lastFrames = f;
                    lastDraws = d;
                    lastReport = clock::now();
                }
            }
        }

        void startCommandProcessor()
        {
            if (!g_cpStarted.exchange(true))
                std::thread(commandProcessorThread).detach();
        }

        void startVblank()
        {
            if (!g_vblankStarted.exchange(true))
                std::thread(vblankThread).detach();
        }
    }

    void init()
    {
        // Values the CPU reads from display/EDRAM registers (from Xenia's GraphicsSystem::ReadRegister)
        *regPtr(reg::RB_EDRAM_TIMING) = __builtin_bswap32(0x08100748);
        *regPtr(reg::RB_BC_CONTROL) = __builtin_bswap32(0x0000200E);
        *regPtr(reg::D1MODE_V_COUNTER) = __builtin_bswap32(0x000002D0);
        *regPtr(reg::D1MODE_INTERRUPT_STATUS) = __builtin_bswap32(1);
        *regPtr(reg::D1MODE_VIEWPORT_SIZE) = __builtin_bswap32((kDisplayWidth << 16) | kDisplayHeight);

        // Guest context for interrupt callbacks (shared by the CP and vblank threads, serialized)
        g_irqCtx = new PPCContext{};
        g_irqCtx->fpscr.loadFromHost();
        g_irqThread = kernel::createGuestThread(*g_irqCtx, 0x40000);
        g_irqThreadObj = new kernel::Thread();
        g_irqThreadObj->guestThread = g_irqThread;
        g_irqThreadObj->ctx = g_irqCtx;
        g_irqThreadObj->guest = g_irqThread->kthread;
        info("[gpu] register file at %08X, interrupt thread %u", kGpuMmioBase, g_irqThread->id);
    }

    uint32_t readRegister(uint32_t index) { return __builtin_bswap32(*regPtr(index % reg::kCount)); }
    const uint32_t* registerFile() { return regPtr(0); }

    void writeRegister(uint32_t index, uint32_t value)
    {
        index %= reg::kCount;
        *regPtr(index) = __builtin_bswap32(value);
        if (index >= reg::SCRATCH_REG0 && index < reg::SCRATCH_REG0 + 8)
        {
            // Scratch registers are mirrored to memory when enabled in SCRATCH_UMSK (D3D fences)
            uint32_t n = index - reg::SCRATCH_REG0;
            if (readRegister(reg::SCRATCH_UMSK) & (1u << n))
                *(uint32_t*)physical(readRegister(reg::SCRATCH_ADDR) + n * 4) = __builtin_bswap32(value);
        }
        else if (index == reg::COHER_STATUS_HOST && (value & 0x80000000))
        {
            // Cache flush request: complete immediately
            *regPtr(index) = __builtin_bswap32(value & ~0x80000000u);
        }
    }

    void setInterruptCallback(uint32_t callback, uint32_t userData)
    {
        g_callback = callback;
        g_callbackData = userData;
        info("[gpu] interrupt callback %08X (data %08X)", callback, userData);
        if (callback) startVblank();
    }

    void initializeRingBuffer(uint32_t physAddr, uint32_t sizeLog2)
    {
        g_ringBase = physAddr & 0x1FFFFFFF;
        g_ringDwords = (1u << (sizeLog2 + 3)) / 4;
        g_rptr = 0;
        *regPtr(reg::CP_RB_RPTR) = 0;
        *regPtr(reg::CP_RB_WPTR) = 0;
        info("[gpu] ring buffer at %08X, %u bytes", g_ringBase, g_ringDwords * 4);
        startCommandProcessor();
    }

    void enableReadPointerWriteBack(uint32_t physAddr, uint32_t blockLog2)
    {
        g_rptrWriteback = physAddr & 0x1FFFFFFF;
        info("[gpu] read pointer writeback at %08X (block 2^%u)", g_rptrWriteback, blockLog2);
    }

    void wakeCommandProcessor() { g_wake.notify_one(); }
}

// MMIO stores from recompiled code (PPC_MM_STORE_U32).
// XenonRecomp emits PPC_MM_STORE_U32 for *any* store followed by `eieio`, but D3D also puts
// `eieio` after ordinary writes: while an Exhibition match loads, its command-list code links
// nodes in physical memory that way (the 0xA0../0xC0../0xE0.. views), and every one of those
// stores used to be reported as an unhandled device register (issue #25). Only
// 0x7F000000-0x7FFFFFFF (mem::kDeviceBase..kDeviceEnd) holds device registers - the GPU file at
// 0x7FC80000 and the XMA block at 0x7FEA0000 below are inside it - so anything outside is
// stored as normal memory, without a word about it.
void GuestMmioStore32(uint8_t* base, uint32_t address, uint32_t value)
{
    if (address >= mem::kGpuMmioBase && address < mem::kGpuMmioBase + 0x10000)
    {
        uint32_t index = (address - mem::kGpuMmioBase) / 4;
        gpu::writeRegister(index, value);
        if (index == gpu::reg::CP_RB_WPTR)
            gpu::wakeCommandProcessor();
        return;
    }
    if (address >= 0x7FEA0000 && address < 0x7FEB0000)
    {
        // Audio chip (XMA): registers are little-endian, i.e. byte-swapped from the store value
        apu::mmioStore((address - 0x7FEA0000) / 4, __builtin_bswap32(value));
        return;
    }
    if (address >= mem::kDeviceBase && address < mem::kDeviceEnd)
    {
        // A device-register store that no device claims: worth reporting, once per address.
        static std::mutex lk;
        static std::map<uint32_t, int> seen;
        std::lock_guard g(lk);
        if (seen[address]++ == 0)
            warn("unhandled MMIO store %08X = %08X", address, value);
    }
    *(uint32_t*)(base + address) = __builtin_bswap32(value);
}
