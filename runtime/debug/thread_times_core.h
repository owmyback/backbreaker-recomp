#pragma once
// Per-thread timing for hitch reports (#146): the part that needs no game and no running threads,
// so tests/thread_times_test.cpp checks it. runtime/debug/thread_times.cpp collects the samples.
//
// A sample is one thread's totals at one moment:
//  - from the kernel (/proc/self/task/<tid>/schedstat): time on a CPU, and time runnable but
//    waiting for a CPU (the runqueue), which is what a machine with too few cores shows as;
//  - from the runtime: time inside each kind of call that blocks or spins (guest waits, sleeps,
//    file I/O, lock and GPU-fence spinning, the command processor polling for the game) and in
//    write-watch faults, plus the longest single call of each kind.
// A hitch report is the difference between the snapshots around one long frame.
#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace debug::threadtimes
{
    // Kinds of call a thread is timed inside: wall-clock time from entering the call to leaving it,
    // whether the thread spun on a CPU, slept or was descheduled meanwhile. The kernel's numbers
    // (time on a CPU, time waiting for one) are the other view.
    enum Bucket : uint32_t { kGuestWait, kSleep, kFile, kLockSpin, kFenceSpin, kFault, kCpPoll, kBuckets };
    constexpr const char* kBucketNames[kBuckets] = { "guest waits",  "sleeping",     "file I/O",          "spinning on locks",
                                                     "GPU fence spin", "write faults", "polling for the game" };

    struct Longest
    {
        uint64_t ns = 0;
        uint32_t lr = 0; // the guest caller, 0 if none
    };

    struct ThreadSample
    {
        bool present = false;
        uint64_t cpuNs = 0, queueNs = 0;
        uint64_t bucketNs[kBuckets]{};
        uint64_t faults = 0;
        Longest longest[kBuckets]; // since the snapshot before this one
    };

    struct Snapshot
    {
        double t = 0;                      // steady clock, seconds
        std::vector<ThreadSample> threads; // by registry slot
        uint64_t otherCpuNs = 0, otherQueueNs = 0; // unregistered threads (Mesa, PipeWire, SDL), running totals
        uint32_t otherCount = 0;
    };

    // "<on cpu ns> <runqueue ns> <timeslices>"
    inline bool parseSchedstat(const char* s, uint64_t& runNs, uint64_t& waitNs)
    {
        char* end = nullptr;
        runNs = std::strtoull(s, &end, 10);
        if (end == s) return false;
        const char* p = end;
        waitNs = std::strtoull(p, &end, 10);
        return end != p;
    }

    // The latest snapshot at or before `start` and the first at or after `end`. False until the ring
    // has one past `end`, or when `start` is older than the ring.
    inline bool pickWindow(const std::deque<Snapshot>& ring, double start, double end, size_t& a, size_t& b)
    {
        if (ring.empty() || ring.back().t < end || ring.front().t > start) return false;
        a = 0;
        while (a + 1 < ring.size() && ring[a + 1].t <= start) a++;
        b = a;
        while (b < ring.size() && ring[b].t < end) b++;
        return b < ring.size() && b > a;
    }

    struct ThreadDelta
    {
        size_t slot = 0;
        uint64_t cpuNs = 0, queueNs = 0, bucketNs[kBuckets]{}, faults = 0;
        Longest longest[kBuckets];
        uint64_t windowNs = 0;
        uint64_t outsideNs = 0; // the window, less the timed calls: running the game's code, or blocked somewhere not timed
    };

    // Each thread present at ring[b]: what it did from ring[a] to ring[b] (a thread new since a counts
    // from zero). The longest calls are the longest of the snapshots in between, cut to the window (a
    // wait that began long before it counts for the window only).
    inline std::vector<ThreadDelta> deltas(const std::deque<Snapshot>& ring, size_t a, size_t b)
    {
        std::vector<ThreadDelta> out;
        const Snapshot &A = ring[a], &B = ring[b];
        const uint64_t window = uint64_t((B.t - A.t) * 1e9);
        auto sub = [](uint64_t x, uint64_t y) { return x > y ? x - y : 0; };
        for (size_t s = 0; s < B.threads.size(); s++)
        {
            const ThreadSample& y = B.threads[s];
            if (!y.present) continue;
            ThreadSample x;
            if (s < A.threads.size() && A.threads[s].present) x = A.threads[s];
            ThreadDelta d;
            d.slot = s;
            d.cpuNs = sub(y.cpuNs, x.cpuNs);
            d.queueNs = sub(y.queueNs, x.queueNs);
            d.faults = sub(y.faults, x.faults);
            d.windowNs = window;
            uint64_t timed = 0;
            for (uint32_t k = 0; k < kBuckets; k++)
            {
                d.bucketNs[k] = sub(y.bucketNs[k], x.bucketNs[k]);
                timed += d.bucketNs[k];
                for (size_t i = a + 1; i <= b; i++)
                    if (s < ring[i].threads.size() && ring[i].threads[s].longest[k].ns > d.longest[k].ns)
                        d.longest[k] = ring[i].threads[s].longest[k];
                d.longest[k].ns = std::min(d.longest[k].ns, window);
            }
            d.outsideNs = sub(window, timed);
            out.push_back(d);
        }
        return out;
    }

    inline double ms(uint64_t ns) { return double(ns) / 1e6; }

    // One thread's line: "cpu 151 ms, waiting for a core 2 ms; in GPU fence spin 120, guest waits 7
    // (longest 6 ms, from 82412345); outside those 33". Calls under 1 ms in total are left out.
    inline std::string describe(const ThreadDelta& d)
    {
        char b[160];
        snprintf(b, sizeof b, "cpu %.0f ms, waiting for a core %.0f ms", ms(d.cpuNs), ms(d.queueNs));
        std::string s = b, in;
        for (uint32_t k = 0; k < kBuckets; k++)
        {
            if (d.bucketNs[k] < 1000000) continue;
            snprintf(b, sizeof b, "%s%s %.0f", in.empty() ? "" : ", ", kBucketNames[k], ms(d.bucketNs[k]));
            in += b;
            if (k == kFault)
            {
                snprintf(b, sizeof b, " (%" PRIu64 " faults)", d.faults);
                in += b;
            }
            else if (d.longest[k].ns >= 1000000 && d.longest[k].ns + 1000000 < d.bucketNs[k])
            {
                // more than one call: the longest, and who made it
                if (d.longest[k].lr) snprintf(b, sizeof b, " (longest %.0f, from %08X)", ms(d.longest[k].ns), d.longest[k].lr);
                else snprintf(b, sizeof b, " (longest %.0f)", ms(d.longest[k].ns));
                in += b;
            }
            else if (d.longest[k].lr)
            {
                snprintf(b, sizeof b, " (from %08X)", d.longest[k].lr);
                in += b;
            }
        }
        if (d.faults && d.bucketNs[kFault] < 1000000)
        {
            snprintf(b, sizeof b, "%s%" PRIu64 " write faults", in.empty() ? "" : ", ", d.faults);
            in += b;
        }
        if (!in.empty()) s += "; in " + in;
        snprintf(b, sizeof b, "; outside those %.0f", ms(d.outsideNs));
        s += b;
        return s;
    }

    // Worth a line in a report: busy for a tenth of the window, short of a core, reading files,
    // spinning on a lock, or faulting
    inline bool notable(const ThreadDelta& d)
    {
        const uint64_t busy = std::max<uint64_t>(5000000, d.windowNs / 10), some = 5000000;
        return d.cpuNs >= busy || d.queueNs >= some || d.bucketNs[kFile] >= some || d.bucketNs[kLockSpin] >= some || d.faults >= 50;
    }

    // The threads to print, busiest first: the `always` ones (the game's main thread, the command
    // processor) and the notable ones, at most maxLines
    inline std::vector<const ThreadDelta*> pick(const std::vector<ThreadDelta>& ds, const std::vector<bool>& always, size_t maxLines)
    {
        std::vector<const ThreadDelta*> out;
        for (const ThreadDelta& d : ds)
            if ((d.slot < always.size() && always[d.slot]) || notable(d)) out.push_back(&d);
        auto score = [](const ThreadDelta* d) { return d->cpuNs + d->queueNs + d->bucketNs[kFile]; };
        std::stable_sort(out.begin(), out.end(), [&](const ThreadDelta* x, const ThreadDelta* y) { return score(x) > score(y); });
        if (out.size() > maxLines) out.resize(maxLines);
        return out;
    }

    // Stack samples of one thread over a window, each symbolized innermost first: "sub_XXXXXXXX" for
    // recompiled guest code, "[name]" for a host function (an import or the runtime). Summarized as:
    //  - how many samples were in the GPU fence spin (any frame equal to `fence`): the thread waiting;
    //  - the commonest stacks among the others: the innermost host function if the sample stopped in
    //    one, then up to `guestFrames` guest frames;
    //  - the guest functions on the stack in the most of those samples (where the time went, whole
    //    calls included).
    struct StackSummary
    {
        size_t samples = 0, fence = 0;
        std::vector<std::pair<size_t, std::string>> stacks, functions; // (samples, name), most first
    };

    // `hostFrames`: a thread that runs no guest code (the command processor): the runtime's own
    // functions count as guest frames do, and a system DLL ("[ntdll.dll]") only as the innermost frame
    inline StackSummary summarizeStacks(const std::vector<std::vector<std::string>>& samples, const std::string& fence,
                                        size_t topStacks = 4, size_t topFunctions = 8, size_t guestFrames = 4, bool hostFrames = false)
    {
        StackSummary out;
        out.samples = samples.size();
        std::map<std::string, size_t> stacks, functions;
        for (const std::vector<std::string>& frames : samples)
        {
            if (std::find(frames.begin(), frames.end(), fence) != frames.end())
            {
                out.fence++;
                continue;
            }
            std::string key;
            size_t guest = 0;
            std::set<std::string> seen;
            for (const std::string& f : frames)
            {
                auto isDll = [&] {
                    if (f.size() < 6) return false;
                    std::string tail = f.substr(f.size() - 5);
                    for (char& c : tail) c = char(std::tolower(static_cast<unsigned char>(c)));
                    return tail == ".dll]";
                };
                const bool isGuest = f.rfind("sub_", 0) == 0 || (hostFrames && !f.empty() && !isDll());
                if (isGuest)
                {
                    if (seen.insert(f).second) functions[f]++;
                    if (guest < guestFrames && key.find(f) == std::string::npos)
                    {
                        key += (key.empty() ? "" : " < ") + f;
                        guest++;
                    }
                }
                else if (key.empty() && !f.empty())
                    key = f; // the innermost host function, before any guest frame
            }
            stacks[key.empty() ? "(no guest frames)" : key]++;
        }
        auto top = [](const std::map<std::string, size_t>& m, size_t n) {
            std::vector<std::pair<size_t, std::string>> v;
            for (const auto& [name, count] : m) v.push_back({ count, name });
            std::stable_sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
            if (v.size() > n) v.resize(n);
            return v;
        };
        out.stacks = top(stacks, topStacks);
        out.functions = top(functions, topFunctions);
        return out;
    }
}
