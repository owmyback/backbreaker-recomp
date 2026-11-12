#include "memory.h"
#include "log.h"

#include <cstring>
#include <map>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

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

        Bump g_kernel{ "kernel", kKernelBase, kKernelEnd };
        Bump g_virtual{ "virtual", kVirtualBase, kVirtualEnd };
        Bump g_physical{ "physical", kPhysicalBase, kPhysicalEnd };
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

    void recordAllocation(uint32_t b, uint32_t size, uint32_t protect)
    {
        std::lock_guard guard(g_allocLock);
        g_allocs[b] = { b, size, protect };
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
}
