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
    constexpr uint32_t kKernelEnd        = 0x80000000;
    constexpr uint32_t kPhysicalBase     = 0xA0000000; // MmAllocatePhysicalMemoryEx
    constexpr uint32_t kPhysicalEnd      = 0xC0000000;

    extern uint8_t* base;

    void init();

    // Zeroed allocations; return a guest address. Abort on exhaustion.
    uint32_t kernelAlloc(size_t size, size_t align = 16);
    uint32_t virtualAlloc(size_t size, size_t align = 0x10000);
    uint32_t physicalAlloc(size_t size, size_t align = 0x1000);

    // Allocation bookkeeping for NtQueryVirtualMemory / MmQueryAllocationSize
    struct Allocation { uint32_t base, size, protect; };
    bool findAllocation(uint32_t addr, Allocation& out);
    void recordAllocation(uint32_t base, uint32_t size, uint32_t protect);

    inline uint8_t* host(uint32_t guest) { return base + guest; }
    inline bool isGuest(const void* p) { return (const uint8_t*)p >= base && (const uint8_t*)p < base + 0x100000000ull; }

    // Big-endian accessors
    inline uint32_t read32(uint32_t a) { return __builtin_bswap32(*(uint32_t*)(base + a)); }
    inline uint16_t read16(uint32_t a) { return __builtin_bswap16(*(uint16_t*)(base + a)); }
    inline uint64_t read64(uint32_t a) { return __builtin_bswap64(*(uint64_t*)(base + a)); }
    inline void write8(uint32_t a, uint8_t v) { base[a] = v; }
    inline void write16(uint32_t a, uint16_t v) { *(uint16_t*)(base + a) = __builtin_bswap16(v); }
    inline void write32(uint32_t a, uint32_t v) { *(uint32_t*)(base + a) = __builtin_bswap32(v); }
    inline void write64(uint32_t a, uint64_t v) { *(uint64_t*)(base + a) = __builtin_bswap64(v); }
}
