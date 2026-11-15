// Tests mem::watch (runtime/memory.cpp, #73): the renderer skips reading a texture when none of its
// pages was written since it last read them, so a write the watch misses is a stale texture.
// Maps guest memory exactly as the runtime does (one 4 GB reservation, physical RAM as a memfd at
// 0xA0000000, 0xC0000000 and 0xE0000000) and installs the same SIGSEGV hook. No game files.
//
//   clang++ -std=c++20 -O2 -pthread -I runtime tests/memory_watch_test.cpp runtime/memory.cpp -o build/memory_watch_test

#include "memory.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

static int failures = 0, checks = 0;
static void ok(bool good, const std::string& what)
{
    checks++;
    if (!good) failures++, std::printf("FAIL %s\n", what.c_str());
}

static void onSegv(int, siginfo_t* si, void*)
{
    if (mem::watch::onFault(si->si_addr)) return;
    std::signal(SIGSEGV, SIG_DFL); // a real crash: let it happen
}

// Physical address -> host pointer in one of the three views
static uint8_t* view(int which, uint32_t phys)
{
    static const uint32_t bases[3] = { 0xA0000000u, 0xC0000000u, 0xE0000000u };
    return mem::host(bases[which] + phys - (which == 2 ? 0x1000 : 0));
}

int main()
{
    mem::init();
    struct sigaction sa{};
    sa.sa_sigaction = onSegv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);

    const char* names[3] = { "0xA0000000", "0xC0000000", "0xE0000000" };

    // 1. A write through each view is seen, and the bytes land (the faulting store is retried)
    for (int v = 0; v < 3; v++)
    {
        uint32_t start = 0x100000 + v * 0x10000, end = start + 0x3000; // three pages
        uint64_t stamp = mem::watch::protect(start, end);
        ok(mem::watch::unchangedSince(start, end, stamp), std::string("freshly protected range is unchanged (") + names[v] + ")");
        ok(mem::watch::protectedPages() >= 3, "pages counted as protected");
        view(v, start + 0x1800)[0] = 0x5A; // middle page
        ok(!mem::watch::unchangedSince(start, end, stamp), std::string("write through ") + names[v] + " view is seen");
        ok(*mem::physical(start + 0x1800) == 0x5A, std::string("write through ") + names[v] + " view landed");
        ok(mem::watch::unchangedSince(start, start + 0x1000, stamp), "untouched first page still unchanged");
        ok(mem::watch::unchangedSince(start + 0x2000, end, stamp), "untouched last page still unchanged");
    }

    // 2. Physical page 0 is at 0xE0000000 - 0x1000, outside that view: protecting it must not break the others
    {
        uint64_t stamp = mem::watch::protect(0, 0x2000);
        *mem::host(0xA0000000u) = 1;
        ok(!mem::watch::unchangedSince(0, 0x1000, stamp), "page 0 write seen");
        *view(2, 0x1000) = 2; // page 1 through the E view
        ok(!mem::watch::unchangedSince(0x1000, 0x2000, stamp), "page 1 write through 0xE0000000 seen");
    }

    // 3. Two textures sharing a page: re-protecting it for one must not hide the write from the other
    {
        uint32_t a0 = 0x200000, a1 = 0x202800, b0 = 0x202000, b1 = 0x204000; // share page 0x202000
        uint64_t sa_ = mem::watch::protect(a0, a1);
        uint64_t sb = mem::watch::protect(b0, b1);
        *mem::physical(0x202100) = 7; // the shared page
        uint64_t sa2 = mem::watch::protect(a0, a1); // texture A re-reads and re-protects
        ok(!mem::watch::unchangedSince(b0, b1, sb), "texture B still sees the write after A re-protected the shared page");
        ok(mem::watch::unchangedSince(a0, a1, sa2), "texture A is unchanged since its own re-read");
        ok(!mem::watch::unchangedSince(a0, a1, sa_), "...but changed since its first read");
    }

    // 4. release(): the runtime's own writes (GPU copy-outs) need no fault and are seen
    {
        uint32_t s0 = 0x300000, s1 = 0x308000;
        uint64_t stamp = mem::watch::protect(s0, s1);
        mem::watch::release(s0 + 0x2000, s0 + 0x3000);
        uint64_t before = mem::watch::faults();
        std::memset(mem::physical(s0 + 0x2000), 3, 0x1000);
        ok(mem::watch::faults() == before, "a released page takes no fault");
        ok(!mem::watch::unchangedSince(s0, s1, stamp), "released range counts as written");
    }

    // 5. readFile() into protected physical pages: the data arrives (pread would fail with EFAULT)
    {
        std::string pathStr = std::string(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp") + "/memory_watch_test_XXXXXX";
        std::vector<char> path(pathStr.begin(), pathStr.end());
        path.push_back(0);
        int fd = mkstemp(path.data());
        std::vector<uint8_t> data(0x5000);
        for (size_t i = 0; i < data.size(); i++) data[i] = uint8_t(i * 7 + 1);
        ok(write(fd, data.data(), data.size()) == ssize_t(data.size()), "temp file written");
        uint32_t s0 = 0x400000;
        uint64_t stamp = mem::watch::protect(s0, s0 + 0x5000);
        long n = mem::readFile(fd, 0xA0000000u + s0 + 0x10, 0x4F00, 0);
        ok(n == 0x4F00, "readFile read everything into protected pages");
        ok(std::memcmp(mem::physical(s0 + 0x10), data.data(), 0x4F00) == 0, "readFile data is right");
        ok(!mem::watch::unchangedSince(s0, s0 + 0x5000, stamp), "readFile counts as a write");
        n = mem::readFile(fd, 0x40000000u, 0x100, 0); // virtual memory: plain pread
        ok(n == 0x100 && std::memcmp(mem::host(0x40000000u), data.data(), 0x100) == 0, "readFile into virtual memory");
        close(fd);
        unlink(path.data());
    }

    // 6. The race protect() has to get right: writers fault while the renderer thread keeps
    //    re-protecting. After the writers stop, every page they wrote must read as changed since
    //    the last stamp taken before their last write; a page left writable but marked protected
    //    would read as unchanged here.
    {
        constexpr uint32_t start = 0x1000000, pages = 64, end = start + pages * 0x1000;
        std::atomic<bool> stop{false};
        std::atomic<uint32_t> written{0};
        std::vector<std::thread> writers;
        for (int t = 0; t < 4; t++)
            writers.emplace_back([&, t] {
                uint32_t x = 12345 + t;
                while (!stop.load())
                {
                    x = x * 1103515245 + 12345;
                    *(volatile uint32_t*)view(int(x % 3), start + ((x >> 8) % pages) * 0x1000 + 8 * t) = x;
                    written.fetch_add(1);
                }
            });
        for (int i = 0; i < 20000; i++) mem::watch::protect(start, end);
        stop = true;
        for (auto& w : writers) w.join();
        // Now quiet: protect, then one write per page through a rotating view, each must be seen
        uint64_t stamp = mem::watch::protect(start, end);
        bool allSeen = true;
        for (uint32_t p = 0; p < pages; p++)
        {
            uint32_t a = start + p * 0x1000;
            *(volatile uint8_t*)view(int(p % 3), a + 0x40) = uint8_t(p);
            if (mem::watch::unchangedSince(a, a + 0x1000, stamp)) allSeen = false;
        }
        ok(allSeen, "after " + std::to_string(written.load()) + " racing writes, every page is protected again and its next write is seen");
    }

    std::printf("%d checks, %d failed (faults handled: %llu)\n", checks, failures, (unsigned long long)mem::watch::faults());
    return failures ? 1 : 0;
}
