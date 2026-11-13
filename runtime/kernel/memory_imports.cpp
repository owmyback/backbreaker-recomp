// Memory management imports.
#include "import_support.h"
#include "../log.h"

using namespace mem;

namespace
{
    constexpr uint32_t MEM_COMMIT = 0x1000;
    constexpr uint32_t MEM_LARGE_PAGES = 0x20000000; // 64 KB pages
    constexpr uint32_t MEM_16MB_PAGES = 0x80000000;

    uint32_t pageSizeFor(uint32_t type)
    {
        if (type & MEM_16MB_PAGES) return 0x1000000;
        if (type & MEM_LARGE_PAGES) return 0x10000;
        return 0x1000;
    }

    uint32_t roundUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
}

// NTSTATUS NtAllocateVirtualMemory(void** BaseAddress, size_t* RegionSize, u32 AllocationType, u32 Protect, u32 DebugMemory)
GUEST_IMPORT(NtAllocateVirtualMemory)
{
    uint32_t pBase = ctx.r3.u32, pSize = ctx.r4.u32, type = ctx.r5.u32, protect = ctx.r6.u32;
    uint32_t want = read32(pBase);
    uint32_t page = pageSizeFor(type);
    uint32_t size = roundUp(read32(pSize), page);

    if (want != 0)
    {
        // Committing (part of) an existing reservation: memory is always backed, just succeed.
        Allocation a;
        if (!findAllocation(want, a))
            warn("NtAllocateVirtualMemory at unreserved %08X (size %X, type %X)", want, size, type);
        write32(pBase, want & ~(page - 1));
        write32(pSize, size);
        ctx.r3.u64 = STATUS_SUCCESS;
        return;
    }

    // A full address space is an ordinary failure: the game's heap reserves segments of doubling
    // size and, when one doesn't fit, halves the request and retries (sub_82405308)
    uint32_t addr = tryVirtualAlloc(size, page < 0x10000 ? 0x10000 : page);
    if (!addr)
    {
        info("NtAllocateVirtualMemory: no room for 0x%X bytes (type %X): STATUS_NO_MEMORY", size, type);
        ctx.r3.u64 = STATUS_NO_MEMORY;
        return;
    }
    recordAllocation(addr, size, protect ? protect : kProtectReadWrite, "NtAllocateVirtualMemory", uint32_t(ctx.lr));
    write32(pBase, addr);
    write32(pSize, size);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtFreeVirtualMemory(void** BaseAddress, size_t* RegionSize, u32 FreeType, u32 DebugMemory)
GUEST_IMPORT(NtFreeVirtualMemory)
{
    constexpr uint32_t MEM_RELEASE = 0x8000;
    uint32_t pBase = ctx.r3.u32, type = ctx.r5.u32;
    uint32_t addr = read32(pBase);
    if (type & MEM_RELEASE)
    {
        // Releasing the start of a reservation frees all of it; decommits keep the memory
        if (!virtualFree(addr))
            warn("NtFreeVirtualMemory(%08X): not an allocation start", addr);
    }
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtQueryVirtualMemory(void* BaseAddress, MEMORY_BASIC_INFORMATION* Info)
GUEST_IMPORT(NtQueryVirtualMemory)
{
    uint32_t addr = ctx.r3.u32, info = ctx.r4.u32;
    Allocation a;
    if (!findAllocation(addr, a))
    {
        a = { addr & ~0xFFFu, 0x1000, 0 };
        write32(info + 0x10, 0x10000); // State = MEM_FREE
    }
    else
    {
        write32(info + 0x10, MEM_COMMIT);
    }
    write32(info + 0x00, addr & ~0xFFFu); // BaseAddress
    write32(info + 0x04, a.base);         // AllocationBase
    write32(info + 0x08, a.protect);      // AllocationProtect
    write32(info + 0x0C, a.base + a.size - (addr & ~0xFFFu)); // RegionSize
    write32(info + 0x14, a.protect);      // Protect
    write32(info + 0x18, 0x20000);        // Type = MEM_PRIVATE
    ctx.r3.u64 = STATUS_SUCCESS;
}

// void* MmAllocatePhysicalMemoryEx(u32 Flags, size_t Size, u32 Protect, u32 MinAddress, u32 MaxAddress, u32 Alignment)
GUEST_IMPORT(MmAllocatePhysicalMemoryEx)
{
    uint32_t size = ctx.r4.u32, protect = ctx.r5.u32, align = ctx.r8.u32;
    uint32_t page = (protect & MEM_16MB_PAGES) ? 0x1000000 : (protect & MEM_LARGE_PAGES) ? 0x10000 : 0x1000;
    if (align < page) align = page;
    uint32_t addr = physicalAlloc(roundUp(size, page), align);
    recordAllocation(addr, roundUp(size, page), protect & 0xFF, "MmAllocatePhysicalMemoryEx", uint32_t(ctx.lr));
    ctx.r3.u64 = addr;
}

// void MmFreePhysicalMemory(u32 Type, void* BaseAddress)
GUEST_IMPORT(MmFreePhysicalMemory)
{
    if (!physicalFree(ctx.r4.u32))
        warn("MmFreePhysicalMemory(%08X): not an allocation start", ctx.r4.u32);
}

// Physical address as the GPU sees it
GUEST_IMPORT(MmGetPhysicalAddress) { ctx.r3.u64 = physicalAddress(ctx.r3.u32); }

GUEST_IMPORT(MmQueryAllocationSize)
{
    Allocation a;
    ctx.r3.u64 = findAllocation(ctx.r3.u32, a) ? a.size : 0;
}

GUEST_IMPORT(MmQueryAddressProtect)
{
    Allocation a;
    ctx.r3.u64 = findAllocation(ctx.r3.u32, a) ? a.protect : kProtectReadWrite;
}

// void* ExAllocatePoolWithTag(size_t Size, u32 Tag)
GUEST_IMPORT(ExAllocatePoolWithTag)
{
    uint32_t size = roundUp(ctx.r3.u32 ? ctx.r3.u32 : 1, 16);
    uint32_t addr = virtualAlloc(size, 16);
    recordAllocation(addr, size, kProtectReadWrite, "ExAllocatePoolWithTag", uint32_t(ctx.lr));
    ctx.r3.u64 = addr;
}

GUEST_IMPORT(ExFreePool)
{
    if (ctx.r3.u32 && !virtualFree(ctx.r3.u32))
        warn("ExFreePool(%08X): not an allocation start", ctx.r3.u32);
}

// u32 XamAlloc(u32 Flags, u32 Size, void** Result)
GUEST_IMPORT(XamAlloc)
{
    uint32_t size = roundUp(ctx.r4.u32 ? ctx.r4.u32 : 1, 16);
    uint32_t addr = virtualAlloc(size, 16);
    recordAllocation(addr, size, kProtectReadWrite, "XamAlloc", uint32_t(ctx.lr));
    write32(ctx.r5.u32, addr);
    ctx.r3.u64 = 0;
}

GUEST_IMPORT(XamFree)
{
    if (ctx.r3.u32 && !virtualFree(ctx.r3.u32))
        warn("XamFree(%08X): not an allocation start", ctx.r3.u32);
    ctx.r3.u64 = 0;
}
