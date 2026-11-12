// Thread-local storage, critical sections and spinlocks.
// Lock state lives in guest memory (big-endian), so updates use atomics on the raw words.
#include "import_support.h"
#include "kernel.h"
#include "../log.h"

#include <atomic>
#include <mutex>
#include <sched.h>

using namespace mem;

namespace
{
    std::mutex g_tlsLock;
    uint64_t g_tlsUsed = 0; // 64 slots

    inline uint32_t* raw32(uint32_t guest) { return (uint32_t*)host(guest); }
    inline uint32_t be(uint32_t v) { return __builtin_bswap32(v); }

    // Atomically replace guest word `expected` with `desired` (values in host order).
    bool cas32(uint32_t guest, uint32_t expected, uint32_t desired)
    {
        uint32_t e = be(expected);
        return __atomic_compare_exchange_n(raw32(guest), &e, be(desired), false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }

    uint32_t self() { return kernel::currentThread()->kthread; }

    // RTL_CRITICAL_SECTION: header(0x10) LockCount(0x10) RecursionCount(0x14) OwningThread(0x18)
    constexpr uint32_t kCsLockCount = 0x10, kCsRecursion = 0x14, kCsOwner = 0x18;

    void csInit(uint32_t cs)
    {
        write8(cs + 0, 1); // Type = SynchronizationEvent
        write32(cs + kCsLockCount, 0xFFFFFFFF);
        write32(cs + kCsRecursion, 0);
        write32(cs + kCsOwner, 0);
    }
}

// ---- TLS (64 dynamic slots per thread, stored in guest memory at PCR+0) ----
GUEST_IMPORT(KeTlsAlloc)
{
    std::lock_guard guard(g_tlsLock);
    for (uint32_t i = 0; i < 64; i++)
    {
        if (!(g_tlsUsed & (1ull << i)))
        {
            g_tlsUsed |= 1ull << i;
            ctx.r3.u64 = i;
            return;
        }
    }
    ctx.r3.u64 = 0xFFFFFFFF;
}

GUEST_IMPORT(KeTlsFree)
{
    std::lock_guard guard(g_tlsLock);
    if (ctx.r3.u32 < 64)
        g_tlsUsed &= ~(1ull << ctx.r3.u32);
    ctx.r3.u64 = 1;
}

GUEST_IMPORT(KeTlsGetValue)
{
    uint32_t i = ctx.r3.u32;
    ctx.r3.u64 = i < 64 ? read32(kernel::currentThread()->tlsSlots + i * 4) : 0;
}

GUEST_IMPORT(KeTlsSetValue)
{
    uint32_t i = ctx.r3.u32;
    if (i < 64)
        write32(kernel::currentThread()->tlsSlots + i * 4, ctx.r4.u32);
    ctx.r3.u64 = i < 64;
}

// ---- Critical sections ----
GUEST_IMPORT(RtlInitializeCriticalSection) { csInit(ctx.r3.u32); }
GUEST_IMPORT(RtlInitializeCriticalSectionAndSpinCount) { csInit(ctx.r3.u32); ctx.r3.u64 = STATUS_SUCCESS; }

GUEST_IMPORT(RtlEnterCriticalSection)
{
    uint32_t cs = ctx.r3.u32, me = self();
    if (read32(cs + kCsOwner) == me)
    {
        write32(cs + kCsRecursion, read32(cs + kCsRecursion) + 1);
        return;
    }
    while (!cas32(cs + kCsOwner, 0, me))
        sched_yield();
    write32(cs + kCsRecursion, 1);
    write32(cs + kCsLockCount, 0);
}

GUEST_IMPORT(RtlTryEnterCriticalSection)
{
    uint32_t cs = ctx.r3.u32, me = self();
    if (read32(cs + kCsOwner) == me)
    {
        write32(cs + kCsRecursion, read32(cs + kCsRecursion) + 1);
        ctx.r3.u64 = 1;
        return;
    }
    if (cas32(cs + kCsOwner, 0, me))
    {
        write32(cs + kCsRecursion, 1);
        write32(cs + kCsLockCount, 0);
        ctx.r3.u64 = 1;
        return;
    }
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(RtlLeaveCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    uint32_t rec = read32(cs + kCsRecursion);
    if (rec > 1)
    {
        write32(cs + kCsRecursion, rec - 1);
        return;
    }
    write32(cs + kCsRecursion, 0);
    write32(cs + kCsLockCount, 0xFFFFFFFF);
    __atomic_store_n(raw32(cs + kCsOwner), 0u, __ATOMIC_RELEASE);
}

// ---- Spinlocks (u32 in guest memory; 0 = free) ----
GUEST_IMPORT(KfAcquireSpinLock)
{
    while (!cas32(ctx.r3.u32, 0, self()))
        sched_yield();
    ctx.r3.u64 = 0; // previous IRQL
}

GUEST_IMPORT(KfReleaseSpinLock) { __atomic_store_n(raw32(ctx.r3.u32), 0u, __ATOMIC_RELEASE); }

GUEST_IMPORT(KeAcquireSpinLockAtRaisedIrql)
{
    while (!cas32(ctx.r3.u32, 0, self()))
        sched_yield();
}

GUEST_IMPORT(KeReleaseSpinLockFromRaisedIrql) { __atomic_store_n(raw32(ctx.r3.u32), 0u, __ATOMIC_RELEASE); }

// Critical regions and L2 locking have no meaning on the host.
GUEST_IMPORT(KeEnterCriticalRegion) {}
GUEST_IMPORT(KeLeaveCriticalRegion) {}
GUEST_IMPORT(KeLockL2) { ctx.r3.u64 = 0; }
GUEST_IMPORT(KeUnlockL2) {}
