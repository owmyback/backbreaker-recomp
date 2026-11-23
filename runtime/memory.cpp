#include "memory.h"
#include "log.h"
#include "platform.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <atomic>
#include <string>
#include <tuple>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

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

            // Returns 0 when nothing fits and !mustFit; aborts otherwise
            uint32_t alloc(size_t size, size_t align, bool mustFit = true)
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
                if (!mustFit)
                    return 0;
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

#ifdef _WIN32
    namespace
    {
        // mprotect's meaning on Windows, for the write watch below
        constexpr int PROT_NONE = 0, PROT_READ = 1, PROT_WRITE = 2;
        int mprotect(void* p, size_t n, int prot)
        {
            DWORD old = 0;
            const DWORD w = (prot & PROT_WRITE) ? PAGE_READWRITE : (prot & PROT_READ) ? PAGE_READONLY : PAGE_NOACCESS;
            if (VirtualProtect(p, n, w, &old)) return 0;
            errno = ENOMEM;
            return -1;
        }
    }

    void init()
    {
        // Windows (#174): one 4 GB placeholder reservation, split where the physical views go, and
        // the views are of one 512 MB section, as the memfd is on Linux. Windows maps a view only at
        // 64 KB boundaries of both its address and the section, so the console's 0xE0000000 view
        // (physical 0x1000 at its start) can't be made: that range stays reserved, and touching it
        // is a crash that names the address. The game was checked not to use it (#174).
        // VirtualAlloc2 and MapViewOfFile3 are Windows 10 1803 and newer (and Wine).
        using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
        using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        auto virtualAlloc2 = kb ? (VirtualAlloc2Fn)(void*)GetProcAddress(kb, "VirtualAlloc2") : nullptr;
        auto mapViewOfFile3 = kb ? (MapViewOfFile3Fn)(void*)GetProcAddress(kb, "MapViewOfFile3") : nullptr;
        if (!virtualAlloc2 || !mapViewOfFile3)
            fatal("this Windows is too old: Backbreaker needs Windows 10 version 1803 or newer");

        const HANDLE self = GetCurrentProcess();
        HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, kPhysicalSize, nullptr);
        if (!section)
            fatal("could not create physical memory backing (error %lu)", GetLastError());
        auto* p = (uint8_t*)virtualAlloc2(self, nullptr, 0x100000000ull, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
        if (!p)
            fatal("could not reserve 4 GB guest address space (error %lu)", GetLastError());
        base = p;
        // [0, A0000000) [A0000000, C0000000) [C0000000, E0000000) [E0000000, 4 GB)
        for (uint32_t at : { 0xA0000000u, 0xC0000000u })
            if (!VirtualFree(base + at, kPhysicalSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
                fatal("could not split the guest address space at %08X (error %lu)", at, GetLastError());
        // Ordinary guest memory: committed, which Windows counts against RAM and the page file;
        // a page takes memory only once it's touched
        if (!virtualAlloc2(self, base, 0xA0000000u, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
            fatal("could not commit guest memory (error %lu): is the page file too small?", GetLastError());
        for (uint32_t at : { 0xA0000000u, 0xC0000000u })
            if (!mapViewOfFile3(section, self, base + at, 0, kPhysicalSize, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0))
                fatal("could not map physical view at %08X (error %lu)", at, GetLastError());
        CloseHandle(section); // the views keep it
        mprotect(base, kNullGuardEnd, PROT_NONE);
        info("guest memory at %p", base);
    }
#else
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
#endif

    namespace watch
    {
        namespace
        {
            constexpr uint32_t kPages = kPhysicalSize >> 12;
            std::atomic<uint64_t> g_counter{1};      // write stamps; 0 = never written
            std::atomic<uint64_t> g_faults{0};
            std::atomic<uint32_t> g_protectedCount{0};
            std::atomic<uint64_t>* g_stamp = new std::atomic<uint64_t>[kPages]();
            std::atomic<uint8_t>* g_protected = new std::atomic<uint8_t>[kPages]();

            // The page's three host addresses: 0xA0000000 and 0xC0000000 views map physical 0 at
            // their start, the 0xE0000000 view starts at physical 0x1000. False if any mprotect failed
            // (ENOMEM: each protected run splits a mapping, and vm.max_map_count can run out).
            bool setProtection(uint32_t firstPage, uint32_t count, int prot)
            {
                size_t bytes = size_t(count) << 12;
                uint32_t phys = firstPage << 12;
                bool ok = mprotect(base + 0xA0000000u + phys, bytes, prot) == 0;
                ok &= mprotect(base + 0xC0000000u + phys, bytes, prot) == 0;
#ifndef _WIN32 // no 0xE0000000 view there (init)
                if (phys >= 0x1000) ok &= mprotect(base + 0xE0000000u + phys - 0x1000, bytes, prot) == 0;
                else if (count > 1) ok &= mprotect(base + 0xE0000000u, bytes - 0x1000, prot) == 0;
#endif
                return ok;
            }
            std::atomic<bool> g_protectFailed{false}; // since then, nothing new is protected

            // Order matters against protect() on the renderer thread: stamp, make writable, and only
            // then clear the flag. protect() re-protects only pages whose flag is clear, so its
            // read-only mprotect always lands after this writable one, never before it (which would
            // leave a writable page marked protected, and later writes unseen).
            void unprotectPage(uint32_t page)
            {
                g_stamp[page].store(g_counter.fetch_add(1) + 1);
                setProtection(page, 1, PROT_READ | PROT_WRITE);
                if (g_protected[page].exchange(0)) g_protectedCount.fetch_sub(1);
            }
        }

        uint64_t protect(uint32_t start, uint32_t end)
        {
            uint64_t stamp = g_counter.load(); // before protecting: a write after it stamps later
            if (g_protectFailed.load(std::memory_order_relaxed)) return stamp; // pages stay unflagged: never "unchanged"
            uint32_t p = start >> 12, last = std::min<uint32_t>((end + 0xFFF) >> 12, kPages);
            while (p < last)
            {
                if (g_protected[p].load()) { p++; continue; }
                uint32_t run = p;
                while (run < last && !g_protected[run].load())
                {
                    g_protected[run].store(1);
                    run++;
                }
                g_protectedCount.fetch_add(run - p);
                if (!setProtection(p, run - p, PROT_READ))
                {
                    // Not (all) protected: a write could go unseen. Make the run writable again and
                    // unflag it with a new stamp, so it never reads as unchanged, and stop protecting:
                    // the renderer then reads such memory every time, as it would without the watch.
                    for (uint32_t q = p; q < run; q++) g_stamp[q].store(g_counter.fetch_add(1) + 1);
                    setProtection(p, run - p, PROT_READ | PROT_WRITE);
                    for (uint32_t q = p; q < run; q++)
                        if (g_protected[q].exchange(0)) g_protectedCount.fetch_sub(1);
                    if (!g_protectFailed.exchange(true))
                        warn("[mem] write watch: mprotect failed (%s); memory is no longer watched, so textures and geometry "
                             "are read every time (raise vm.max_map_count?)", strerror(errno));
                    return stamp;
                }
                p = run;
            }
            return stamp;
        }

        bool unchangedSince(uint32_t start, uint32_t end, uint64_t stamp)
        {
            uint32_t last = std::min<uint32_t>((end + 0xFFF) >> 12, kPages);
            for (uint32_t p = start >> 12; p < last; p++)
                if (!g_protected[p].load(std::memory_order_relaxed) || g_stamp[p].load(std::memory_order_relaxed) > stamp) return false;
            return true;
        }

        // A run of protected pages at a time: one mprotect per view for the run, not one per page
        // (a 1024x1024 resolve written back over a watched image is 1,023 pages). Same order as
        // unprotectPage: stamp, make writable, then clear the flags.
        void release(uint32_t start, uint32_t end)
        {
            uint32_t p = start >> 12, last = std::min<uint32_t>((end + 0xFFF) >> 12, kPages);
            while (p < last)
            {
                if (!g_protected[p].load()) { p++; continue; }
                uint32_t run = p;
                while (run < last && g_protected[run].load())
                {
                    g_stamp[run].store(g_counter.fetch_add(1) + 1);
                    run++;
                }
                setProtection(p, run - p, PROT_READ | PROT_WRITE);
                for (uint32_t q = p; q < run; q++)
                    if (g_protected[q].exchange(0)) g_protectedCount.fetch_sub(1);
                p = run;
            }
        }

        bool onFault(const void* addr)
        {
            const uint8_t* a = (const uint8_t*)addr;
            if (a < base + 0xA0000000u || a >= base + 0x100000000ull) return false;
#ifdef _WIN32
            if (a >= base + 0xE0000000u) return false; // no view there (init): a crash, not a watched page
#endif
            uint32_t page = physicalAddress(uint32_t(a - base)) >> 12;
            if (page >= kPages) return false;
            // Nothing else ever makes the physical views non-writable, so a fault here is a watched
            // page (or one another thread unprotected a moment ago): stamp it, make it writable, retry.
            unprotectPage(page);
            g_faults.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        uint64_t faults() { return g_faults.load(); }
        uint32_t protectedPages() { return g_protectedCount.load(); }
    }

    long readFile(int fd, uint32_t guest, uint32_t len, int64_t offset)
    {
        if (guest < kPhysicalBase || watch::protectedPages() == 0)
            return pread(fd, host(guest), len, offset);
        static thread_local std::vector<uint8_t> buffer;
        buffer.resize(len);
        long n = pread(fd, buffer.data(), len, offset);
        if (n > 0) std::memcpy(host(guest), buffer.data(), size_t(n)); // faults on watched pages, as a game write would
        return n;
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
    uint32_t tryVirtualAlloc(size_t size, size_t align) { return g_virtual.alloc(size, align, false); }
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
