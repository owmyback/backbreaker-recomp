#pragma once
// Guest address space: one 4 GB host reservation, guest address == offset from mem::base.
#include <cstddef>
#include <cstdint>

namespace mem
{
    // Guest memory map (Xbox 360 layout, simplified)
    constexpr uint32_t kNullGuardEnd     = 0x00010000; // first 64 KB is inaccessible
    constexpr uint32_t kVirtualBase      = 0x40000000; // NtAllocateVirtualMemory, stacks
    constexpr uint32_t kVirtualEnd       = 0x7F000000;
    constexpr uint32_t kKernelBase       = 0x7F000000; // kernel objects owned by the runtime
    constexpr uint32_t kKernelEnd        = 0x7FC00000;
    constexpr uint32_t kDeviceBase       = 0x7F000000; // device register windows begin here (GPU 0x7FC80000, XMA 0x7FEA0000)
    constexpr uint32_t kDeviceEnd        = 0x80000000; // ... and end here; a store outside it is plain RAM, not MMIO
    // (the runtime's own kernel objects, kKernelBase-kKernelEnd, sit in the same range)
    constexpr uint32_t kGpuMmioBase      = 0x7FC80000; // GPU register file (CPU-visible MMIO window)
    constexpr uint32_t kPhysicalBase     = 0xA0000000; // MmAllocatePhysicalMemoryEx
    constexpr uint32_t kPhysicalEnd      = 0xC0000000;
    constexpr uint32_t kPhysicalSize     = 0x20000000; // 512 MB, also visible at 0xC0000000 and 0xE0000000

    // Guest virtual address in a physical view -> physical address
    inline uint32_t physicalAddress(uint32_t v) { return (v & 0x1FFFFFFF) + (v >= 0xE0000000 ? 0x1000 : 0); }

    extern uint8_t* base;

    void init();

    // Zeroed allocations; return a guest address. Abort on exhaustion.
    uint32_t kernelAlloc(size_t size, size_t align = 16);
    uint32_t virtualAlloc(size_t size, size_t align = 0x10000);
    // As virtualAlloc, but returns 0 instead of aborting when the address space is full
    uint32_t tryVirtualAlloc(size_t size, size_t align = 0x10000);
    uint32_t physicalAlloc(size_t size, size_t align = 0x1000);
    // Return an allocation to its heap (exact start address). False if it isn't one.
    bool virtualFree(uint32_t guest);
    bool physicalFree(uint32_t guest);
    void usage(uint64_t& virtualMb, uint64_t& physicalMb);

    constexpr uint32_t kProtectReadWrite = 0x04;

    // Allocation bookkeeping for NtQueryVirtualMemory / MmQueryAllocationSize. The caller and
    // its lr are kept so the crash/timeout report can show which import still holds memory.
    struct Allocation { uint32_t base, size, protect; const char* caller; uint32_t lr; };
    bool findAllocation(uint32_t addr, Allocation& out);
    void recordAllocation(uint32_t base, uint32_t size, uint32_t protect, const char* caller, uint32_t lr = 0);
    void forgetAllocation(uint32_t base);
    // Tracked allocations still held, grouped by (caller, lr), largest first
    void printAllocationStats();

    inline uint8_t* host(uint32_t guest) { return base + guest; }
    // Physical address (as the GPU sees it) -> host pointer
    inline uint8_t* physical(uint32_t p) { return base + kPhysicalBase + (p & 0x1FFFFFFF); }
    inline bool isGuest(const void* p) { return (const uint8_t*)p >= base && (const uint8_t*)p < base + 0x100000000ull; }

    // Big-endian accessors
    inline uint8_t read8(uint32_t a) { return base[a]; }
    inline uint32_t read32(uint32_t a) { return __builtin_bswap32(*(uint32_t*)(base + a)); }
    inline uint16_t read16(uint32_t a) { return __builtin_bswap16(*(uint16_t*)(base + a)); }
    inline uint64_t read64(uint32_t a) { return __builtin_bswap64(*(uint64_t*)(base + a)); }
    inline void write8(uint32_t a, uint8_t v) { base[a] = v; }
    inline void write16(uint32_t a, uint16_t v) { *(uint16_t*)(base + a) = __builtin_bswap16(v); }
    inline void write32(uint32_t a, uint32_t v) { *(uint32_t*)(base + a) = __builtin_bswap32(v); }
    inline void write64(uint32_t a, uint64_t v) { *(uint64_t*)(base + a) = __builtin_bswap64(v); }
}
