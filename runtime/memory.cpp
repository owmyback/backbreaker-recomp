#include "memory.h"
#include "log.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <tuple>
#include <unistd.h>
#include <vector>

namespace mem
{
    uint8_t* base = nullptr;

    namespace
    {
        struct Bump
        {
            const char* name;
            uint32_t next, end;
            std::mutex lock;

            uint32_t alloc(size_t size, size_t align)
            {
                std::lock_guard guard(lock);
                uint64_t a = (uint64_t(next) + align - 1) & ~uint64_t(align - 1);
                if (a + size > end)
                    fatal("out of %s memory (want 0x%zX bytes)", name, size);
                next = uint32_t(a + size);
                memset(base + a, 0, size);
                return uint32_t(a);
            }
        };

        // First-fit heap with coalescing free ranges (guest memory is always backed, so
        // allocating is pure bookkeeping plus zero-fill).
        struct Heap
        {
            const char* name;
            uint32_t begin, end;
            std::mutex lock;
            std::map<uint32_t, uint32_t> freeRanges; // start -> size
            std::map<uint32_t, uint32_t> used;       // start -> size
            uint64_t inUse = 0, peak = 0;

            Heap(const char* n, uint32_t b, uint32_t e) : name(n), begin(b), end(e) { freeRanges[b] = e - b; }

            uint32_t alloc(size_t size, size_t align)
            {
                std::lock_guard guard(lock);
                size = (size + 15) & ~size_t(15);
                if (align < 16) align = 16;
                for (auto it = freeRanges.begin(); it != freeRanges.end(); ++it)
                {
                    uint64_t s = it->first, len = it->second;
                    uint64_t a = (s + align - 1) & ~uint64_t(align - 1);
                    if (a + size > s + len)
                        continue;
                    freeRanges.erase(it);
                    if (a > s) freeRanges[uint32_t(s)] = uint32_t(a - s);
                    if (a + size < s + len) freeRanges[uint32_t(a + size)] = uint32_t(s + len - a - size);
                    used[uint32_t(a)] = uint32_t(size);
                    inUse += size;
                    if (inUse > peak) peak = inUse;
                    memset(base + a, 0, size);
                    return uint32_t(a);
                }
                fatal("out of %s memory (want 0x%zX bytes, %llu MB in use)", name, size, (unsigned long long)(inUse >> 20));
            }

            bool free(uint32_t a)
            {
                std::lock_guard guard(lock);
                auto u = used.find(a);
                if (u == used.end())
                    return false;
                uint32_t start = a, size = u->second;
                used.erase(u);
                inUse -= size;
                auto next = freeRanges.lower_bound(start);
                if (next != freeRanges.end() && start + size == next->first)
                {
                    size += next->second;
                    next = freeRanges.erase(next);
                }
                if (next != freeRanges.begin())
                {
                    auto prev = std::prev(next);
                    if (prev->first + prev->second == start)
                    {
                        start = prev->first;
                        size += prev->second;
                        freeRanges.erase(prev);
                    }
                }
                freeRanges[start] = size;
                return true;
            }
        };

        Bump g_kernel{ "kernel", kKernelBase, kKernelEnd };
        Heap g_virtual{ "virtual", kVirtualBase, kVirtualEnd };
        // Physical address 0 is left unused: guest code treats a zero physical address as "unset"
        Heap g_physical{ "physical", kPhysicalBase + 0x10000, kPhysicalEnd };
    }

    void init()
    {
        // Lazily committed; untouched pages cost nothing.
        void* p = mmap(nullptr, 0x100000000ull, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED)
            fatal("could not reserve 4 GB guest address space");
        base = (uint8_t*)p;
        mprotect(base, kNullGuardEnd, PROT_NONE);

        // Physical RAM is one 512 MB file mapped at three guest views, like the console:
        // 0xA0000000 (64 KB pages), 0xC0000000 (16 MB pages) and 0xE0000000 (4 KB pages, offset 0x1000).
        int fd = memfd_create("xbox-physical", 0);
        if (fd < 0 || ftruncate(fd, kPhysicalSize) != 0)
            fatal("could not create physical memory backing");
        auto view = [&](uint32_t guest, off_t offset, size_t size) {
            if (mmap(base + guest, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, offset) == MAP_FAILED)
                fatal("could not map physical view at %08X", guest);
        };
        view(0xA0000000, 0, kPhysicalSize);
        view(0xC0000000, 0, kPhysicalSize);
        view(0xE0000000, 0x1000, kPhysicalSize - 0x1000);
        close(fd);
        info("guest memory at %p", base);
    }

    namespace
    {
        std::mutex g_allocLock;
        std::map<uint32_t, Allocation> g_allocs; // keyed by base
    }

    void recordAllocation(uint32_t b, uint32_t size, uint32_t protect, const char* caller, uint32_t lr)
    {
        std::lock_guard guard(g_allocLock);
        g_allocs[b] = { b, size, protect, caller, lr };
    }

    bool findAllocation(uint32_t addr, Allocation& out)
    {
        std::lock_guard guard(g_allocLock);
        auto it = g_allocs.upper_bound(addr);
        if (it == g_allocs.begin())
            return false;
        --it;
        if (addr >= it->second.base + uint64_t(it->second.size))
            return false;
        out = it->second;
        return true;
    }

    uint32_t kernelAlloc(size_t size, size_t align) { return g_kernel.alloc(size, align); }
    uint32_t virtualAlloc(size_t size, size_t align) { return g_virtual.alloc(size, align); }
    uint32_t physicalAlloc(size_t size, size_t align) { return g_physical.alloc(size, align); }

    void forgetAllocation(uint32_t b)
    {
        std::lock_guard guard(g_allocLock);
        g_allocs.erase(b);
    }

    bool virtualFree(uint32_t a)
    {
        forgetAllocation(a);
        return g_virtual.free(a);
    }

    bool physicalFree(uint32_t a)
    {
        // Any of the three physical views may be passed in
        uint32_t v = kPhysicalBase + physicalAddress(a);
        forgetAllocation(a);
        forgetAllocation(v);
        return g_physical.free(v);
    }

    void usage(uint64_t& virtualMb, uint64_t& physicalMb)
    {
        virtualMb = g_virtual.inUse >> 20;
        physicalMb = g_physical.inUse >> 20;
    }

    void printAllocationStats()
    {
        struct Row { const char* heap; uint64_t bytes; std::string caller; uint32_t lr; uint32_t blocks; };
        std::lock_guard guard(g_allocLock);
        std::map<std::pair<std::string, uint32_t>, std::pair<uint64_t, uint32_t>> byCaller; // -> (bytes, blocks)
        std::map<std::pair<std::string, uint32_t>, const char*> heap;                        // -> which heap holds them
        for (auto& [b, a] : g_allocs)
        {
            auto& r = byCaller[{a.caller, a.lr}];
            r.first += a.size;
            r.second++;
            heap[{a.caller, a.lr}] = a.base >= kPhysicalBase ? "phys" : "virt";
        }
        std::vector<Row> rows;
        for (auto& [k, v] : byCaller)
            rows.push_back({heap[k], v.first, k.first, k.second, v.second});
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.bytes > b.bytes; });

        std::fprintf(stderr, "---- allocations still held ----\n");
        for (auto& r : rows)
            std::fprintf(stderr, "  %c %8llu KB  %5u blocks  %-30s lr=%08X\n", r.heap[0],
                         (unsigned long long)(r.bytes >> 10), r.blocks, r.caller.c_str(), r.lr);
    }
}
