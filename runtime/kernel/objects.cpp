// Handle table, dispatcher objects and waits.
#include "objects.h"
#include "import_support.h"
#include "../log.h"

#include <algorithm>
#include <thread>
#include <unordered_map>

using namespace mem;

namespace
{
    constexpr uint32_t STATUS_TIMEOUT = 0x00000102;
    constexpr uint32_t STATUS_INVALID_HANDLE = 0xC0000008;

    // DISPATCHER_HEADER types
    constexpr uint8_t kNotificationEvent = 0, kSynchronizationEvent = 1, kSemaphore = 5;

    std::mutex g_tableLock;
    std::unordered_map<uint32_t, kernel::Object*> g_handles; // handle -> object
    std::unordered_map<uint32_t, kernel::Object*> g_byGuest; // guest address -> object
    uint32_t g_nextHandle = 0xF8000004;

    thread_local kernel::Thread* t_thread = nullptr;

    bool isSignaled(kernel::Object* o)
    {
        using T = kernel::Object::Type;
        switch (o->type)
        {
        case T::Event: case T::Timer: return static_cast<kernel::Event*>(o)->signaled;
        case T::Semaphore: return static_cast<kernel::Semaphore*>(o)->count > 0;
        case T::Thread: return static_cast<kernel::Thread*>(o)->exited;
        // A socket never becomes signaled: offline, no data arrives and no connection completes.
        case T::Socket: return false;
        default: return true;
        }
    }

    void syncGuestState(kernel::Object* o)
    {
        if (!o->guest || o->type == kernel::Object::Type::Thread || o->type == kernel::Object::Type::Socket)
            return; // sockets have no DISPATCHER_HEADER signal state to write back
        uint32_t state = o->type == kernel::Object::Type::Semaphore ? uint32_t(static_cast<kernel::Semaphore*>(o)->count)
                                                                    : uint32_t(static_cast<kernel::Event*>(o)->signaled);
        write32(o->guest + 4, state); // SignalState
    }

    void acquire(kernel::Object* o)
    {
        using T = kernel::Object::Type;
        if (o->type == T::Event || o->type == T::Timer)
        {
            auto* e = static_cast<kernel::Event*>(o);
            if (!e->manualReset) e->signaled = false;
        }
        else if (o->type == T::Semaphore)
        {
            static_cast<kernel::Semaphore*>(o)->count--;
        }
        syncGuestState(o);
    }
}

namespace kernel
{
    std::mutex g_dispatchLock;
    std::condition_variable g_dispatchCv;

    void signalObjectsChanged() { g_dispatchCv.notify_all(); }

    uint32_t registerObject(Object* o)
    {
        std::lock_guard guard(g_tableLock);
        if (!o->guest)
        {
            // Give handle-created objects a guest header so ObReferenceObjectByHandle works.
            o->guest = kernelAlloc(0x20);
            uint8_t type = o->type == Object::Type::Semaphore ? kSemaphore
                         : (o->type == Object::Type::Event && !static_cast<Event*>(o)->manualReset) ? kSynchronizationEvent
                         : kNotificationEvent;
            write8(o->guest, type);
        }
        g_byGuest[o->guest] = o;
        uint32_t h = g_nextHandle;
        g_nextHandle += 4;
        g_handles[h] = o;
        syncGuestState(o);
        return h;
    }

    uint32_t duplicateHandle(uint32_t handle)
    {
        Object* o = objectFromHandle(handle);
        if (!o) return 0;
        std::lock_guard guard(g_tableLock);
        uint32_t h = g_nextHandle;
        g_nextHandle += 4;
        g_handles[h] = o;
        return h;
    }

    void closeHandle(uint32_t handle)
    {
        // Objects are kept alive: guest code may still hold pointers to their headers.
        std::lock_guard guard(g_tableLock);
        g_handles.erase(handle);
    }

    Object* objectFromHandle(uint32_t handle)
    {
        if (handle == kCurrentThreadHandle)
            return t_thread;
        std::lock_guard guard(g_tableLock);
        auto it = g_handles.find(handle);
        return it == g_handles.end() ? nullptr : it->second;
    }

    Object* objectFromGuest(uint32_t guest)
    {
        std::lock_guard guard(g_tableLock);
        auto it = g_byGuest.find(guest);
        if (it != g_byGuest.end())
            return it->second;

        // First time we see a guest-initialized object (KeInitializeEvent etc. are inlined by the game).
        uint8_t type = host(guest)[0];
        int32_t state = int32_t(read32(guest + 4));
        Object* o;
        if (type == kNotificationEvent || type == kSynchronizationEvent)
            o = new Event(type == kNotificationEvent, state != 0);
        else if (type == kSemaphore)
            o = new Semaphore(state, int32_t(read32(guest + 0x10)));
        else
        {
            warn("unknown dispatcher object type %u at %08X - treating as event", type, guest);
            o = new Event(true, state != 0);
        }
        o->guest = guest;
        g_byGuest[guest] = o;
        return o;
    }

    Thread* currentThreadObject() { return t_thread; }

    Thread* adoptCurrentThread(GuestThread* gt, PPCContext* ctx)
    {
        auto* t = new Thread();
        t->guestThread = gt;
        t->ctx = ctx;
        t->guest = gt->kthread;
        {
            std::lock_guard guard(g_tableLock);
            g_byGuest[t->guest] = t;
        }
        t_thread = t;
        return t;
    }

    void adoptExistingThread(Thread* t) { t_thread = t; }

    Deadline deadlineFromGuest(uint32_t pTimeout)
    {
        if (!pTimeout)
            return std::nullopt;
        int64_t t = int64_t(read64(pTimeout));
        auto now = std::chrono::steady_clock::now();
        if (t <= 0)
            return now + std::chrono::nanoseconds(-t * 100);
        // Absolute FILETIME: convert via the system clock (same base as KeQuerySystemTime)
        using namespace std::chrono;
        int64_t nowFt = int64_t(kernel::systemTime());
        return now + nanoseconds(std::max<int64_t>(0, t - nowFt) * 100);
    }

    uint32_t waitForObjects(const std::vector<Object*>& objects, bool waitAll, Deadline deadline)
    {
        std::unique_lock lock(g_dispatchLock);
        for (;;)
        {
            if (waitAll)
            {
                bool all = true;
                for (Object* o : objects) all = all && isSignaled(o);
                if (all)
                {
                    for (Object* o : objects) acquire(o);
                    return STATUS_SUCCESS;
                }
            }
            else
            {
                for (size_t i = 0; i < objects.size(); i++)
                {
                    if (isSignaled(objects[i]))
                    {
                        acquire(objects[i]);
                        return uint32_t(i);
                    }
                }
            }
            if (deadline)
            {
                if (g_dispatchCv.wait_until(lock, *deadline) == std::cv_status::timeout && std::chrono::steady_clock::now() >= *deadline)
                {
                    // one last check after the timeout
                    if (!waitAll)
                        for (size_t i = 0; i < objects.size(); i++)
                            if (isSignaled(objects[i])) { acquire(objects[i]); return uint32_t(i); }
                    return STATUS_TIMEOUT;
                }
            }
            else
            {
                g_dispatchCv.wait(lock);
            }
        }
    }

    void setEvent(Event* e)
    {
        std::lock_guard lock(g_dispatchLock);
        e->signaled = true;
        syncGuestState(e);
        signalObjectsChanged();
    }
}

using namespace kernel;

namespace
{
    Object* resolve(uint32_t handle, const char* who)
    {
        Object* o = objectFromHandle(handle);
        if (!o) warn("%s: invalid handle %08X", who, handle);
        return o;
    }
}

// ---- Events ----
// NTSTATUS NtCreateEvent(HANDLE* Handle, OBJECT_ATTRIBUTES* Attr, u32 EventType, BOOL InitialState)
GUEST_IMPORT(NtCreateEvent)
{
    auto* e = new Event(ctx.r5.u32 == 0, ctx.r6.u32 != 0); // 0 = NotificationEvent (manual reset)
    write32(ctx.r3.u32, registerObject(e));
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(NtSetEvent)
{
    auto* o = resolve(ctx.r3.u32, "NtSetEvent");
    if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    auto* e = static_cast<Event*>(o);
    uint32_t prev;
    {
        std::lock_guard lock(g_dispatchLock);
        prev = e->signaled;
        e->signaled = true;
        syncGuestState(e);
        signalObjectsChanged();
    }
    if (ctx.r4.u32) write32(ctx.r4.u32, prev);
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(NtClearEvent)
{
    auto* o = resolve(ctx.r3.u32, "NtClearEvent");
    if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    std::lock_guard lock(g_dispatchLock);
    static_cast<Event*>(o)->signaled = false;
    syncGuestState(o);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// Pulse: release current waiters, then reset. Approximated as set + immediate reset after a yield.
GUEST_IMPORT(NtPulseEvent)
{
    auto* o = resolve(ctx.r3.u32, "NtPulseEvent");
    if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    auto* e = static_cast<Event*>(o);
    setEvent(e);
    std::this_thread::yield();
    std::lock_guard lock(g_dispatchLock);
    e->signaled = false;
    syncGuestState(e);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// LONG KeSetEvent(KEVENT* Event, KPRIORITY Increment, BOOLEAN Wait)
GUEST_IMPORT(KeSetEvent)
{
    auto* e = static_cast<Event*>(objectFromGuest(ctx.r3.u32));
    std::lock_guard lock(g_dispatchLock);
    uint32_t prev = e->signaled;
    e->signaled = true;
    syncGuestState(e);
    signalObjectsChanged();
    ctx.r3.u64 = prev;
}

GUEST_IMPORT(KeResetEvent)
{
    auto* e = static_cast<Event*>(objectFromGuest(ctx.r3.u32));
    std::lock_guard lock(g_dispatchLock);
    uint32_t prev = e->signaled;
    e->signaled = false;
    syncGuestState(e);
    ctx.r3.u64 = prev;
}

// ---- Semaphores ----
// NTSTATUS NtCreateSemaphore(HANDLE* Handle, OBJECT_ATTRIBUTES* Attr, LONG InitialCount, LONG MaximumCount)
GUEST_IMPORT(NtCreateSemaphore)
{
    auto* s = new Semaphore(int32_t(ctx.r5.u32), int32_t(ctx.r6.u32));
    write32(ctx.r3.u32, registerObject(s));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtReleaseSemaphore(HANDLE Handle, LONG ReleaseCount, LONG* PreviousCount)
GUEST_IMPORT(NtReleaseSemaphore)
{
    auto* o = resolve(ctx.r3.u32, "NtReleaseSemaphore");
    if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    auto* s = static_cast<Semaphore*>(o);
    int32_t prev;
    {
        std::lock_guard lock(g_dispatchLock);
        prev = s->count;
        s->count = std::min(s->count + int32_t(ctx.r4.u32), s->maximum);
        syncGuestState(s);
        signalObjectsChanged();
    }
    if (ctx.r5.u32) write32(ctx.r5.u32, uint32_t(prev));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ---- Waits ----
// NTSTATUS NtWaitForSingleObjectEx(HANDLE Handle, u32 WaitMode, BOOL Alertable, i64* Timeout)
GUEST_IMPORT(NtWaitForSingleObjectEx)
{
    auto* o = resolve(ctx.r3.u32, "NtWaitForSingleObjectEx");
    if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    ctx.r3.u64 = waitForObjects({ o }, false, deadlineFromGuest(ctx.r6.u32));
}

// NTSTATUS NtWaitForMultipleObjectsEx(u32 Count, HANDLE* Handles, u32 WaitType, u32 WaitMode, BOOL Alertable, i64* Timeout)
GUEST_IMPORT(NtWaitForMultipleObjectsEx)
{
    std::vector<Object*> objs;
    for (uint32_t i = 0; i < ctx.r3.u32; i++)
    {
        auto* o = resolve(read32(ctx.r4.u32 + i * 4), "NtWaitForMultipleObjectsEx");
        if (!o) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
        objs.push_back(o);
    }
    ctx.r3.u64 = waitForObjects(objs, ctx.r5.u32 == 0, deadlineFromGuest(ctx.r8.u32));
}

// NTSTATUS KeWaitForSingleObject(void* Object, u32 Reason, u32 Mode, BOOL Alertable, i64* Timeout)
GUEST_IMPORT(KeWaitForSingleObject)
{
    ctx.r3.u64 = waitForObjects({ objectFromGuest(ctx.r3.u32) }, false, deadlineFromGuest(ctx.r7.u32));
}

// NTSTATUS KeWaitForMultipleObjects(u32 Count, void** Objects, u32 WaitType, u32 Reason, u32 Mode, BOOL Alertable, i64* Timeout, void* WaitBlocks)
GUEST_IMPORT(KeWaitForMultipleObjects)
{
    std::vector<Object*> objs;
    for (uint32_t i = 0; i < ctx.r3.u32; i++)
        objs.push_back(objectFromGuest(read32(ctx.r4.u32 + i * 4)));
    ctx.r3.u64 = waitForObjects(objs, ctx.r5.u32 == 0, deadlineFromGuest(ctx.r9.u32));
}

// ---- Object manager ----
GUEST_IMPORT(NtClose) { closeHandle(ctx.r3.u32); ctx.r3.u64 = STATUS_SUCCESS; }

// NTSTATUS NtDuplicateObject(HANDLE Source, HANDLE* Target, u32 Options)
GUEST_IMPORT(NtDuplicateObject)
{
    uint32_t h = duplicateHandle(ctx.r3.u32);
    if (ctx.r4.u32) write32(ctx.r4.u32, h);
    ctx.r3.u64 = h ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

// NTSTATUS ObReferenceObjectByHandle(HANDLE Handle, void* ObjectType, void** Object)
GUEST_IMPORT(ObReferenceObjectByHandle)
{
    Object* o = objectFromHandle(ctx.r3.u32);
    if (!o)
    {
        warn("ObReferenceObjectByHandle: invalid handle %08X", ctx.r3.u32);
        if (ctx.r5.u32) write32(ctx.r5.u32, 0);
        ctx.r3.u64 = STATUS_INVALID_HANDLE;
        return;
    }
    if (ctx.r5.u32) write32(ctx.r5.u32, o->guest);
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(ObReferenceObject) {}
GUEST_IMPORT(ObDereferenceObject) {}

// ---- Timers ----
// A timer is an event that a host thread signals at its due time (and again every period).
// Re-arming or cancelling bumps a generation counter so stale sleeper threads do nothing.
namespace
{
    struct Timer : Event
    {
        std::atomic<uint64_t> generation{0};
        explicit Timer(bool manual) : Event(manual, false) { type = Type::Timer; }
    };
}

// NTSTATUS NtCreateTimer(HANDLE* Handle, OBJECT_ATTRIBUTES* Attr, u32 TimerType)  (0 = notification, 1 = sync)
GUEST_IMPORT(NtCreateTimer)
{
    auto* t = new Timer(ctx.r5.u32 == 0);
    write32(ctx.r3.u32, registerObject(t));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtSetTimerEx(HANDLE Timer, i64* DueTime, void* ApcRoutine, u32 ApcMode, void* ApcContext,
//                       BOOL Resume, LONG Period /* ms */, BOOL* PreviousState)
GUEST_IMPORT(NtSetTimerEx)
{
    auto* t = dynamic_cast<Timer*>(objectFromHandle(ctx.r3.u32));
    if (!t) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    uint32_t apc = ctx.r5.u32;
    int32_t period = int32_t(ctx.r9.u32);
    uint32_t pPrev = ctx.r10.u32;
    if (apc)
        warn("NtSetTimerEx: APC routine %08X not supported, timer will only be signaled", apc);
    Deadline due = deadlineFromGuest(ctx.r4.u32);
    {
        std::lock_guard lock(g_dispatchLock);
        if (pPrev) write32(pPrev, t->signaled);
        t->signaled = false;
        syncGuestState(t);
    }
    uint64_t gen = ++t->generation;
    std::thread([t, gen, due, period] {
        auto when = due ? *due : std::chrono::steady_clock::now();
        for (;;)
        {
            std::this_thread::sleep_until(when);
            if (t->generation != gen) return;
            {
                std::lock_guard lock(g_dispatchLock);
                t->signaled = true;
                syncGuestState(t);
                signalObjectsChanged();
            }
            if (period <= 0) return;
            when += std::chrono::milliseconds(period);
        }
    }).detach();
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtCancelTimer(HANDLE Timer, BOOL* CurrentState)
GUEST_IMPORT(NtCancelTimer)
{
    auto* t = dynamic_cast<Timer*>(objectFromHandle(ctx.r3.u32));
    if (!t) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    t->generation++;
    if (ctx.r4.u32) write32(ctx.r4.u32, t->signaled);
    ctx.r3.u64 = STATUS_SUCCESS;
}
