// Checks runtime/debug/thread_times_core.h (issue #146): the arithmetic behind the [hitch] reports,
// and that /proc/self/task/<tid>/schedstat tells a busy thread from a sleeping one. No game files:
//   clang++ -std=c++20 -Wall -Wextra -O2 -pthread tests/thread_times_test.cpp -o build/thread_times_test
#include "../runtime/debug/thread_times_core.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/syscall.h>

using namespace debug::threadtimes;

static int failures = 0, checks = 0;

static void check(bool ok, const char* what)
{
    checks++;
    if (!ok)
    {
        failures++;
        printf("FAIL %s\n", what);
    }
}

static Snapshot snap(double t, std::vector<ThreadSample> threads)
{
    Snapshot s;
    s.t = t;
    s.threads = std::move(threads);
    return s;
}

static ThreadSample sample(uint64_t cpuMs, uint64_t queueMs, uint64_t waitMs = 0, uint64_t fenceMs = 0, uint64_t fileMs = 0)
{
    ThreadSample s;
    s.present = true;
    s.cpuNs = cpuMs * 1000000;
    s.queueNs = queueMs * 1000000;
    s.bucketNs[kGuestWait] = waitMs * 1000000;
    s.bucketNs[kFenceSpin] = fenceMs * 1000000;
    s.bucketNs[kFile] = fileMs * 1000000;
    return s;
}

// The thread's own schedstat file, read now: on-CPU and runqueue nanoseconds
static bool readSchedstat(pid_t tid, uint64_t& run, uint64_t& wait)
{
    char path[64], buf[128] = {};
    snprintf(path, sizeof path, "/proc/self/task/%d/schedstat", int(tid));
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
    close(fd);
    return n > 0 && parseSchedstat(buf, run, wait);
}

int main()
{
    {
        uint64_t r = 0, w = 0;
        check(parseSchedstat("138623786567 170177279 404354\n", r, w) && r == 138623786567ull && w == 170177279ull, "schedstat parsed");
        check(!parseSchedstat("", r, w), "an empty schedstat is refused");
    }
    {
        // Snapshots every 20 ms; a frame from 0.05 to 0.19 s
        std::deque<Snapshot> ring;
        for (int i = 0; i <= 12; i++) ring.push_back(snap(i * 0.02, { sample(i * 10, i, 0, i * 8), sample(0, 0, i * 20) }));
        size_t a = 0, b = 0;
        check(pickWindow(ring, 0.05, 0.19, a, b) && a == 2 && b == 10, "the snapshots just outside the frame");
        check(!pickWindow(ring, 0.05, 0.25, a, b), "not until there is a snapshot past the frame's end");
        std::deque<Snapshot> late(ring.begin() + 5, ring.end());
        check(!pickWindow(late, 0.05, 0.19, a, b), "a frame older than the ring isn't reported");

        pickWindow(ring, 0.05, 0.19, a, b);
        std::vector<ThreadDelta> d = deltas(ring, a, b);
        check(d.size() == 2, "both threads");
        // Thread 0 over 160 ms: cpu 80, queue 8, 64 in the fence spin, 96 outside timed calls
        check(d[0].cpuNs == 80000000 && d[0].queueNs == 8000000 && d[0].bucketNs[kFenceSpin] == 64000000, "busy thread's deltas");
        check(d[0].windowNs == 160000000 && d[0].outsideNs == 96000000, "the time outside timed calls");
        // Thread 1: waited 160 of 160 ms
        check(d[1].bucketNs[kGuestWait] == 160000000 && d[1].outsideNs == 0, "a thread that waited the whole frame");
    }
    {
        // Never negative, even when the calls overrun the window (sampling isn't exact)
        std::deque<Snapshot> ring = { snap(0, { sample(0, 0) }), snap(0.1, { sample(90, 30, 150) }) };
        std::vector<ThreadDelta> d = deltas(ring, 0, 1);
        check(d[0].outsideNs == 0, "outside is clamped at zero");
    }
    {
        // A thread that appeared during the window counts from zero; one gone by its end is left out
        ThreadSample gone = sample(10, 0);
        std::deque<Snapshot> ring = { snap(0, { sample(5, 0), gone }), snap(0.1, { sample(50, 0), ThreadSample{}, sample(20, 1) }) };
        std::vector<ThreadDelta> d = deltas(ring, 0, 1);
        check(d.size() == 2 && d[0].slot == 0 && d[1].slot == 2 && d[1].cpuNs == 20000000, "new and gone threads");
    }
    {
        // Longest call: the longest of the snapshots in the window
        std::deque<Snapshot> ring;
        for (int i = 0; i < 4; i++) ring.push_back(snap(i * 0.02, { sample(0, 0, i * 20) }));
        ring[2].threads[0].longest[kGuestWait] = { 15000000, 0x82412345 };
        ring[3].threads[0].longest[kGuestWait] = { 9000000, 0x82400000 };
        std::vector<ThreadDelta> d = deltas(ring, 0, 3);
        check(d[0].longest[kGuestWait].ns == 15000000 && d[0].longest[kGuestWait].lr == 0x82412345, "longest wait and its caller");
        std::string line = describe(d[0]);
        check(line.find("in guest waits 60 (longest 15, from 82412345)") != std::string::npos, "the line names the wait's caller");
        // A wait that began long before the window counts for the window only
        ring[3].threads[0].longest[kGuestWait] = { 9000000000ull, 0x82400000 };
        d = deltas(ring, 0, 3);
        check(d[0].longest[kGuestWait].ns == 60000000, "the longest call is cut to the window");
    }
    {
        // Which lines: always-shown threads, then notable ones, busiest first, capped
        std::vector<ThreadDelta> ds(5);
        for (size_t i = 0; i < ds.size(); i++) ds[i].slot = i;
        ds[0].cpuNs = 1000000;   // quiet, but always shown
        ds[1].cpuNs = 50000000;  // busy
        ds[2].queueNs = 9000000; // short of a core
        ds[3].bucketNs[kGuestWait] = 100000000; // only waited: left out
        for (ThreadDelta& d : ds) d.windowNs = 120000000;
        ds[4].bucketNs[kFile] = 30000000;       // reading files
        auto p = pick(ds, { true }, 8);
        check(p.size() == 4 && p[0]->slot == 1 && p[1]->slot == 4 && p[2]->slot == 2 && p[3]->slot == 0, "line choice and order");
        check(pick(ds, { true }, 2).size() == 2, "at most maxLines");
    }
    {
        // Stack samples: the fence spin counted apart, then stacks and functions by how often
        std::vector<std::vector<std::string>> samples = {
            { "[backtrace]", "sub_8240F1B8", "sub_8240F288", "sub_82277110" }, // waiting on the GPU
            { "sub_82AAAAAA", "sub_82BBBBBB", "sub_82277110" },
            { "sub_82AAAAAA", "sub_82BBBBBB", "sub_82277110" },
            { "[NtAllocateVirtualMemory]", "sub_82CCCCCC", "sub_82BBBBBB", "sub_82277110" },
            { "sub_82AAAAAA", "sub_82AAAAAA", "sub_82BBBBBB", "sub_82277110" }, // recursion: one frame
        };
        StackSummary sum = summarizeStacks(samples, "sub_8240F1B8");
        check(sum.samples == 5 && sum.fence == 1, "the fence spin is counted apart");
        check(!sum.stacks.empty() && sum.stacks[0].first == 3 && sum.stacks[0].second == "sub_82AAAAAA < sub_82BBBBBB < sub_82277110",
              "the commonest stack, innermost first");
        check(sum.stacks.size() == 2 && sum.stacks[1].second == "[NtAllocateVirtualMemory] < sub_82CCCCCC < sub_82BBBBBB < sub_82277110",
              "a stack that stopped in an import names it");
        check(sum.functions.size() >= 2 && sum.functions[0].first == 4 && sum.functions[1].first == 4 &&
                  (sum.functions[0].second == "sub_82277110" || sum.functions[0].second == "sub_82BBBBBB"),
              "functions on the stack in the most samples, once per sample");
    }
    {
        // Live: a spinning thread and a sleeping one, 200 ms, through their own schedstat files
        std::atomic<bool> stop{false};
        std::atomic<pid_t> busyTid{0}, idleTid{0};
        std::thread busy([&] {
            busyTid = pid_t(syscall(SYS_gettid));
            volatile uint64_t x = 0;
            while (!stop) x = x + 1;
        });
        std::thread idle([&] {
            idleTid = pid_t(syscall(SYS_gettid));
            while (!stop) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        });
        while (!busyTid || !idleTid) std::this_thread::yield();
        uint64_t b0, bq0, i0, iq0, b1, bq1, i1, iq1;
        bool ok = readSchedstat(busyTid, b0, bq0) && readSchedstat(idleTid, i0, iq0);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ok = ok && readSchedstat(busyTid, b1, bq1) && readSchedstat(idleTid, i1, iq1);
        stop = true;
        busy.join();
        idle.join();
        check(ok, "schedstat readable for this process's threads");
        if (ok)
        {
            printf("live: busy thread %.0f ms on a CPU (+%.0f waiting), sleeping thread %.1f ms, over 200 ms\n", (b1 - b0) / 1e6,
                   (bq1 - bq0) / 1e6, (i1 - i0) / 1e6);
            check(b1 - b0 > 100000000, "the spinning thread ran most of the 200 ms");
            check(i1 - i0 < 20000000, "the sleeping thread hardly ran");
        }
    }
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
