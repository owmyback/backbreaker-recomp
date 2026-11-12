// Guest thread state: stack, processor control region (r13) and KTHREAD.
#include "kernel.h"
#include "../memory.h"
#include "../log.h"

#include <atomic>

using namespace mem;

namespace
{
    thread_local kernel::GuestThread* t_current = nullptr;
    std::atomic<uint32_t> g_nextThreadId{1};

    // Offsets follow Xenia's X_KPCR / X_KTHREAD research.
    constexpr uint32_t kPcrTlsPtr = 0x000;
    constexpr uint32_t kPcrSelf = 0x030;
    constexpr uint32_t kPcrStackBase = 0x070;
    constexpr uint32_t kPcrStackLimit = 0x074;
    constexpr uint32_t kPcrCurrentThread = 0x100;
    constexpr uint32_t kPcrCurrentCpu = 0x10C;
    constexpr uint32_t kPcrSize = 0x2D8;

    constexpr uint32_t kThreadStackBase = 0x05C;
    constexpr uint32_t kThreadStackLimit = 0x060;
    constexpr uint32_t kThreadId = 0x14C;
    constexpr uint32_t kThreadSize = 0xAB0;
}

namespace kernel
{
    GuestThread* createGuestThread(PPCContext& ctx, uint32_t stackSize)
    {
        auto* t = new GuestThread{};
        t->id = g_nextThreadId++;
        t->stackLimit = virtualAlloc(stackSize, 0x10000);
        t->stackBase = t->stackLimit + stackSize;
        t->tlsSlots = kernelAlloc(64 * 4);
        t->pcr = kernelAlloc(kPcrSize, 0x1000);
        t->kthread = kernelAlloc(kThreadSize, 0x10);

        write32(t->pcr + kPcrTlsPtr, t->tlsSlots);
        write32(t->pcr + kPcrSelf, t->pcr);
        write32(t->pcr + kPcrStackBase, t->stackBase);
        write32(t->pcr + kPcrStackLimit, t->stackLimit);
        write32(t->pcr + kPcrCurrentThread, t->kthread);
        write8(t->pcr + kPcrCurrentCpu, 0);

        write32(t->kthread + kThreadStackBase, t->stackBase);
        write32(t->kthread + kThreadStackLimit, t->stackLimit);
        write32(t->kthread + kThreadId, t->id);

        ctx.r1.u64 = t->stackBase - 0x100; // leave room for the caller's back chain area
        ctx.r13.u64 = t->pcr;
        t_current = t;
        info("thread %u: stack %08X-%08X pcr %08X kthread %08X", t->id, t->stackLimit, t->stackBase, t->pcr, t->kthread);
        return t;
    }

    GuestThread* currentThread() { return t_current; }
}
