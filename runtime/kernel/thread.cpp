// Guest threads: stack, processor control region (r13), KTHREAD, and host thread lifecycle.
#include "objects.h"
#include "import_support.h"
#include "../log.h"

#include <atomic>
#include <csignal>
#include <cstring>
#include <pthread.h>
#include <vector>

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

    constexpr uint32_t kThreadType = 0x000; // DISPATCHER_HEADER.Type = 6 (ThreadObject)
    constexpr uint32_t kThreadStackBase = 0x05C;
    constexpr uint32_t kThreadStackLimit = 0x060;
    constexpr uint32_t kThreadId = 0x14C;
    constexpr uint32_t kThreadSize = 0xAB0;

    constexpr size_t kHostStackSize = 16 << 20; // recompiled code recurses on the host stack

    struct ThreadExit { uint32_t code; };

    struct StartInfo
    {
        kernel::Thread* thread;
        uint32_t xapiStartup, start, param;
    };

    void* threadEntry(void* arg)
    {
        auto* si = (StartInfo*)arg;
        kernel::Thread* th = si->thread;

        // Per-thread alternate stack so the crash handler works here too.
        static thread_local std::vector<uint8_t> altStack(1 << 16);
        stack_t ss{};
        ss.ss_sp = altStack.data();
        ss.ss_size = altStack.size();
        sigaltstack(&ss, nullptr);

        t_current = th->guestThread;
        kernel::adoptExistingThread(th);

        {
            std::unique_lock lock(kernel::g_dispatchLock);
            kernel::g_dispatchCv.wait(lock, [&] { return th->suspendCount == 0; });
        }

        PPCContext& ctx = *th->ctx;
        ctx.fpscr.loadFromHost();
        uint32_t fn = si->xapiStartup ? si->xapiStartup : si->start;
        ctx.r3.u64 = si->xapiStartup ? si->start : si->param;
        ctx.r4.u64 = si->xapiStartup ? si->param : 0;
        ctx.lr = 0;
        delete si;

        uint32_t code = 0;
        try
        {
            (PPC_LOOKUP_FUNC(base, fn))(ctx, base);
            code = ctx.r3.u32;
        }
        catch (const ThreadExit& e)
        {
            code = e.code;
        }

        info("thread %u exited (code %08X)", th->guestThread->id, code);
        std::lock_guard lock(kernel::g_dispatchLock);
        th->exited = true;
        th->exitCode = code;
        kernel::signalObjectsChanged();
        return nullptr;
    }
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

        write8(t->kthread + kThreadType, 6);
        write32(t->kthread + kThreadStackBase, t->stackBase);
        write32(t->kthread + kThreadStackLimit, t->stackLimit);
        write32(t->kthread + kThreadId, t->id);

        ctx.r1.u64 = t->stackBase - 0x100; // leave room for the caller's back chain area
        ctx.r13.u64 = t->pcr;
        info("thread %u: stack %08X-%08X pcr %08X kthread %08X", t->id, t->stackLimit, t->stackBase, t->pcr, t->kthread);
        return t;
    }

    void setCurrentThread(GuestThread* t) { t_current = t; }
    GuestThread* currentThread() { return t_current; }
}

using namespace kernel;

// NTSTATUS ExCreateThread(HANDLE* Handle, u32 StackSize, u32* ThreadId, void* XapiStartup,
//                         void* StartAddress, void* Parameter, u32 CreationFlags)
GUEST_IMPORT(ExCreateThread)
{
    uint32_t pHandle = ctx.r3.u32, stackSize = ctx.r4.u32, pId = ctx.r5.u32;
    uint32_t xapi = ctx.r6.u32, start = ctx.r7.u32, param = ctx.r8.u32, flags = ctx.r9.u32;
    stackSize = std::max<uint32_t>((stackSize + 0xFFFF) & ~0xFFFFu, 0x40000);

    auto* th = new Thread();
    th->ctx = new PPCContext{};
    th->guestThread = createGuestThread(*th->ctx, stackSize);
    th->guest = th->guestThread->kthread;
    th->suspendCount = (flags & 1) ? 1 : 0; // CREATE_SUSPENDED

    uint32_t handle = registerObject(th);
    if (pHandle) write32(pHandle, handle);
    if (pId) write32(pId, th->guestThread->id);
    info("ExCreateThread: thread %u start %08X (xapi %08X) param %08X%s", th->guestThread->id, start, xapi, param,
         th->suspendCount ? " suspended" : "");

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kHostStackSize);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t tid;
    int err = pthread_create(&tid, &attr, threadEntry, new StartInfo{ th, xapi, start, param });
    pthread_attr_destroy(&attr);
    if (err)
        fatal("pthread_create failed: %s", strerror(err));
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(ExTerminateThread) { throw ThreadExit{ ctx.r3.u32 }; }

// NTSTATUS NtSuspendThread(HANDLE Thread, u32* PreviousCount)
GUEST_IMPORT(NtSuspendThread)
{
    auto* th = static_cast<Thread*>(objectFromHandle(ctx.r3.u32));
    if (!th || th->type != Object::Type::Thread) { ctx.r3.u64 = 0xC0000008; return; }
    std::lock_guard lock(g_dispatchLock);
    if (ctx.r4.u32) write32(ctx.r4.u32, th->suspendCount);
    if (th != currentThreadObject())
        warn("NtSuspendThread on running thread %u: only takes effect before it starts", th->guestThread->id);
    th->suspendCount++;
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtResumeThread(HANDLE Thread, u32* PreviousCount)
GUEST_IMPORT(NtResumeThread)
{
    auto* th = static_cast<Thread*>(objectFromHandle(ctx.r3.u32));
    if (!th || th->type != Object::Type::Thread) { ctx.r3.u64 = 0xC0000008; return; }
    std::lock_guard lock(g_dispatchLock);
    if (ctx.r4.u32) write32(ctx.r4.u32, th->suspendCount);
    if (th->suspendCount > 0) th->suspendCount--;
    signalObjectsChanged();
    ctx.r3.u64 = STATUS_SUCCESS;
}

// Scheduling hints: accepted, no effect on the host scheduler yet.
GUEST_IMPORT(KeSetAffinityThread) { if (ctx.r5.u32) write32(ctx.r5.u32, 0x3F); ctx.r3.u64 = 0x3F; }
GUEST_IMPORT(KeSetBasePriorityThread) { ctx.r3.u64 = 0; }
GUEST_IMPORT(KeQueryBasePriorityThread) { ctx.r3.u64 = 0; }
GUEST_IMPORT(KeSetDisableBoostThread) { ctx.r3.u64 = 0; }

// void RtlRaiseException(EXCEPTION_RECORD* Record)
GUEST_IMPORT(RtlRaiseException)
{
    uint32_t rec = ctx.r3.u32;
    uint32_t code = read32(rec);
    if (code == 0x406D1388) // MSVC "set thread name"
    {
        // ExceptionInformation: [0]=0x1000, [1]=name, [2]=thread id
        uint32_t name = read32(rec + 0x18), id = read32(rec + 0x1C);
        info("thread %d named \"%s\"", int32_t(id), name ? (const char*)host(name) : "?");
        return;
    }
    warn("RtlRaiseException(code %08X) ignored", code);
}
