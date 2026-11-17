#include "thread_times.h"
#include "../log.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace debug::threadtimes
{
    namespace
    {
        const bool g_on = getenv("BB_PERF") && !(getenv("BB_THREAD_TIMES") && getenv("BB_THREAD_TIMES")[0] == '0');
        const uint64_t g_startNs = nowNs(); // reports give times since the process started
        constexpr auto kSamplePeriod = std::chrono::milliseconds(20);
        constexpr double kRingSeconds = 3.0;
        constexpr double kHitchSeconds = 0.08;
        constexpr size_t kMaxLines = 8, kSecondLines = 6;

        // One host thread's counters. Never freed: the sampler may still read a thread's last values
        // after it exits.
        struct Counters
        {
            pid_t tid = 0;
            int fd = -1;               // its schedstat; -1 none, -2 the thread is gone
            uint32_t guestId = 0;      // the guest thread it runs, 0 if none
            const char* role = nullptr;
            std::atomic<uint64_t> total[kBuckets]{};
            std::atomic<uint64_t> faults{0};
            std::atomic<uint64_t> curStart{0}; // the timed call in progress (0: none)
            std::atomic<uint32_t> curBucket{0}, curLr{0};
            std::atomic<uint64_t> longestNs[kBuckets]{}; // since the sampler last took them
            std::atomic<uint32_t> longestLr[kBuckets]{};
        };

        thread_local Counters* t_counters = nullptr;
        std::mutex g_lock; // the slots, the names and the pending hitches
        std::vector<Counters*> g_slots;
        std::map<uint32_t, std::string> g_names;
        std::vector<std::pair<double, double>> g_hitches;
        std::once_flag g_started;

        pid_t tidNow() { return pid_t(syscall(SYS_gettid)); }
        double seconds(uint64_t ns) { return double(ns) / 1e9; }

        void samplerLoop();

        Counters* counters()
        {
            if (t_counters) return t_counters;
            auto* c = new Counters;
            c->tid = tidNow();
            char path[64];
            snprintf(path, sizeof path, "/proc/self/task/%d/schedstat", int(c->tid));
            c->fd = open(path, O_RDONLY | O_CLOEXEC);
            {
                std::lock_guard g(g_lock);
                g_slots.push_back(c);
            }
            t_counters = c;
            std::call_once(g_started, [] { std::thread(samplerLoop).detach(); });
            return c;
        }

        void setName(const char* name)
        {
            if (name && tidNow() != getpid()) pthread_setname_np(pthread_self(), name); // not the process's own name
        }

        std::string label(const Counters* c)
        {
            if (c->role) return c->role;
            if (!c->guestId) return "host thread " + std::to_string(c->tid);
            std::string s = "guest " + std::to_string(c->guestId);
            auto it = g_names.find(c->guestId); // under g_lock
            if (it != g_names.end()) s += " \"" + it->second + "\"";
            return s;
        }

        // ---- the sampler ----

        struct Other
        {
            int fd = -1;
            uint64_t run = 0, wait = 0;
            bool seen = false;
        };

        bool readSchedstat(int fd, uint64_t& run, uint64_t& wait)
        {
            char buf[128];
            ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
            if (n <= 0) return false;
            buf[n] = 0;
            return parseSchedstat(buf, run, wait);
        }

        // Threads this process has that never registered (Mesa, PipeWire, SDL): watched as one group
        void rescanOthers(std::map<pid_t, Other>& others, const std::set<pid_t>& registered)
        {
            DIR* d = opendir("/proc/self/task");
            if (!d) return;
            std::set<pid_t> alive;
            while (dirent* e = readdir(d))
            {
                pid_t tid = pid_t(atoi(e->d_name));
                if (tid <= 0) continue;
                alive.insert(tid);
                if (registered.count(tid) || others.count(tid)) continue;
                char path[64];
                snprintf(path, sizeof path, "/proc/self/task/%d/schedstat", int(tid));
                int fd = open(path, O_RDONLY | O_CLOEXEC);
                if (fd >= 0) others[tid].fd = fd;
            }
            closedir(d);
            for (auto it = others.begin(); it != others.end();)
                if (!alive.count(it->first) || registered.count(it->first))
                {
                    close(it->second.fd);
                    it = others.erase(it);
                }
                else ++it;
        }

        void report(const std::deque<Snapshot>& ring, size_t a, size_t b, const std::vector<Counters*>& slots, double frameS, double endS)
        {
            const Snapshot &A = ring[a], &B = ring[b];
            const double window = B.t - A.t;
            std::vector<ThreadDelta> ds = deltas(ring, a, b);
            uint64_t cpu = B.otherCpuNs - A.otherCpuNs, queue = B.otherQueueNs - A.otherQueueNs, faults = 0;
            for (const ThreadDelta& d : ds)
            {
                cpu += d.cpuNs;
                queue += d.queueNs;
                faults += d.faults;
            }
            std::vector<bool> always(slots.size());
            std::vector<std::string> labels(slots.size());
            {
                std::lock_guard g(g_lock);
                for (size_t s = 0; s < slots.size(); s++)
                {
                    labels[s] = label(slots[s]);
                    always[s] = (slots[s]->guestId == 1 && !slots[s]->role) ||
                                (slots[s]->role && !strcmp(slots[s]->role, "gpu command processor"));
                }
            }
            const uint64_t otherCpu = B.otherCpuNs - A.otherCpuNs, otherQueue = B.otherQueueNs - A.otherQueueNs;
            if (frameS > 0)
            {
                char f[64] = "";
                if (faults) snprintf(f, sizeof f, ", %llu write faults", (unsigned long long)faults);
                info("[hitch] %.0f ms frame ending %.2f s into the run (threads measured over %.0f ms): %.1f cores busy, %.0f ms waiting for a "
                     "core in total%s",
                     frameS * 1000, endS - seconds(g_startNs), window * 1000, seconds(cpu) / window, ms(queue), f);
                for (const ThreadDelta* d : pick(ds, always, kMaxLines))
                    info("[hitch]   %s: %s", labels[d->slot].c_str(), describe(*d).c_str());
                if (otherCpu >= 5000000 || otherQueue >= 5000000)
                    info("[hitch]   other threads (%u): cpu %.0f ms, waiting for a core %.0f ms", B.otherCount, ms(otherCpu), ms(otherQueue));
                return;
            }
            // The second's line: the busiest threads, briefly
            std::string line;
            char buf[256];
            snprintf(buf, sizeof buf, "%.2f cores busy, %.0f ms waiting for a core", seconds(cpu) / window, ms(queue));
            line = buf;
            std::vector<const ThreadDelta*> top = pick(ds, always, kSecondLines);
            for (const ThreadDelta* d : top)
            {
                snprintf(buf, sizeof buf, " | %s: cpu %.0f", labels[d->slot].c_str(), ms(d->cpuNs));
                line += buf;
                if (d->queueNs >= 5000000)
                {
                    snprintf(buf, sizeof buf, ", core wait %.0f", ms(d->queueNs));
                    line += buf;
                }
                for (uint32_t k = 0; k < kBuckets; k++)
                    if (k != kGuestWait && k != kSleep && d->bucketNs[k] >= 10000000)
                    {
                        snprintf(buf, sizeof buf, ", in %s %.0f", kBucketNames[k], ms(d->bucketNs[k]));
                        line += buf;
                    }
            }
            snprintf(buf, sizeof buf, " | other threads (%u): cpu %.0f, core wait %.0f", B.otherCount, ms(otherCpu), ms(otherQueue));
            line += buf;
            info("[perf]   threads: %s", line.c_str());
        }

        void samplerLoop()
        {
            registerRole("thread timing", "bb-threadtimes");
            if (t_counters->fd < 0)
                warn("[threads] /proc/self/task/<tid>/schedstat can't be read: no CPU or core-wait times in the reports");
            info("[threads] thread timing on (BB_PERF=1; BB_THREAD_TIMES=0 turns it off): a [hitch] report for every frame of %.0f ms or "
                 "more, and a [perf] threads line each second",
                 kHitchSeconds * 1000);
            std::deque<Snapshot> ring;
            std::map<pid_t, Other> others;
            uint64_t otherCpu = 0, otherQueue = 0;
            double lastSecond = 0, lastScan = -1;
            auto next = std::chrono::steady_clock::now();
            for (;;)
            {
                next += kSamplePeriod;
                auto now = std::chrono::steady_clock::now();
                if (next < now - std::chrono::milliseconds(100)) next = now; // fell behind (a stop): don't race to catch up
                std::this_thread::sleep_until(next);

                std::vector<Counters*> slots;
                {
                    std::lock_guard g(g_lock);
                    slots = g_slots;
                }
                Snapshot s;
                const uint64_t nowNsValue = nowNs();
                s.t = seconds(nowNsValue);
                s.threads.resize(slots.size());
                std::set<pid_t> registered;
                for (size_t i = 0; i < slots.size(); i++)
                {
                    Counters* c = slots[i];
                    ThreadSample& x = s.threads[i];
                    if (c->fd == -2) continue; // gone
                    registered.insert(c->tid);
                    if (c->fd >= 0 && !readSchedstat(c->fd, x.cpuNs, x.queueNs))
                    {
                        close(c->fd);
                        c->fd = -2;
                        continue;
                    }
                    x.present = true;
                    // The totals first, then the call in progress: a call that ends in between is
                    // missed this time and counted next time, never counted twice
                    for (uint32_t k = 0; k < kBuckets; k++) x.bucketNs[k] = c->total[k].load();
                    x.faults = c->faults.load();
                    for (uint32_t k = 0; k < kBuckets; k++)
                    {
                        uint64_t l = c->longestNs[k].exchange(0);
                        if (l) x.longest[k] = { l, c->longestLr[k].load() };
                    }
                    if (uint64_t st = c->curStart.load(); st && st < nowNsValue)
                    {
                        uint32_t k = c->curBucket.load() % kBuckets;
                        uint64_t open = nowNsValue - st;
                        x.bucketNs[k] += open;
                        if (open > x.longest[k].ns) x.longest[k] = { open, c->curLr.load() };
                    }
                }
                if (s.t - lastScan >= 1.0)
                {
                    rescanOthers(others, registered);
                    lastScan = s.t;
                }
                for (auto& [tid, o] : others)
                {
                    uint64_t run, wait;
                    if (!readSchedstat(o.fd, run, wait)) continue;
                    if (o.seen)
                    {
                        otherCpu += run > o.run ? run - o.run : 0;
                        otherQueue += wait > o.wait ? wait - o.wait : 0;
                    }
                    o.run = run;
                    o.wait = wait;
                    o.seen = true;
                }
                s.otherCpuNs = otherCpu;
                s.otherQueueNs = otherQueue;
                s.otherCount = uint32_t(others.size());
                ring.push_back(std::move(s));
                while (ring.size() > 2 && ring.front().t < ring.back().t - kRingSeconds) ring.pop_front();

                // Long frames that have ended by now
                std::vector<std::pair<double, double>> due;
                {
                    std::lock_guard g(g_lock);
                    for (auto it = g_hitches.begin(); it != g_hitches.end();)
                        if (it->second <= ring.back().t)
                        {
                            due.push_back(*it);
                            it = g_hitches.erase(it);
                        }
                        else ++it;
                }
                for (auto [start, end] : due)
                {
                    size_t a, b;
                    if (pickWindow(ring, start, end, a, b)) report(ring, a, b, slots, end - start, end);
                    else
                        info("[hitch] %.0f ms frame ending %.2f s into the run: longer than the %.0f s the thread timing keeps", (end - start) * 1000,
                             end - seconds(g_startNs), kRingSeconds);
                }

                // The second's line
                const double t = ring.back().t;
                if (t - lastSecond >= 1.0)
                {
                    size_t a = 0;
                    while (a + 1 < ring.size() && ring[a + 1].t <= t - 1.0) a++;
                    if (lastSecond > 0 && a + 1 < ring.size()) report(ring, a, ring.size() - 1, slots, 0, t);
                    lastSecond = t;
                }
            }
        }
    }

    bool enabled() { return g_on; }

    uint64_t nowNs()
    {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
    }

    void registerGuest(uint32_t guestId)
    {
        char name[16];
        snprintf(name, sizeof name, "bb-g%u", guestId);
        if (!g_on)
        {
            setName(name);
            return;
        }
        Counters* c = counters();
        if (c->role) return; // a role's thread binding a guest thread for its callbacks keeps its role
        setName(name);
        std::lock_guard g(g_lock);
        c->guestId = guestId;
    }

    void registerRole(const char* label, const char* threadName)
    {
        setName(threadName);
        if (!g_on) return;
        Counters* c = counters();
        std::lock_guard g(g_lock);
        c->role = label;
    }

    void nameGuest(uint32_t guestId, const char* name)
    {
        if (!g_on || !name) return;
        std::lock_guard g(g_lock);
        g_names[guestId] = std::string(name).substr(0, 40);
    }

    Timer::Timer(Bucket bucket, uint32_t lr) : m_bucket(bucket)
    {
        if (!g_on) return;
        Counters* c = t_counters;
        if (!c || c->curStart.load(std::memory_order_relaxed)) return; // unregistered, or inside another timed call
        m_start = nowNs();
        c->curBucket.store(bucket);
        c->curLr.store(lr);
        c->curStart.store(m_start);
    }

    Timer::~Timer()
    {
        if (!m_start) return;
        Counters* c = t_counters;
        const uint64_t ns = nowNs() - m_start;
        const uint32_t lr = c->curLr.load(std::memory_order_relaxed);
        c->curStart.store(0); // before the total: the sampler may miss this call once, never count it twice
        c->total[m_bucket].fetch_add(ns);
        uint64_t cur = c->longestNs[m_bucket].load(std::memory_order_relaxed);
        while (ns > cur && !c->longestNs[m_bucket].compare_exchange_weak(cur, ns)) {}
        if (ns > cur) c->longestLr[m_bucket].store(lr);
    }

    void addFault(uint64_t ns)
    {
        if (!g_on) return;
        Counters* c = t_counters;
        if (!c) return;
        c->total[kFault].fetch_add(ns);
        c->faults.fetch_add(1);
    }

    void frameEnded(double startS, double endS)
    {
        if (!g_on || startS <= 0 || endS - startS < kHitchSeconds) return;
        std::lock_guard g(g_lock);
        if (g_hitches.size() < 64) g_hitches.push_back({ startS, endS });
    }
}

// D3D's wait for the GPU to pass a fence (#146): it spins, reading the fence the command processor
// writes when the GPU gets there (sub_82419CB0's delay hints, db16cyc, are recompiled to nothing).
// Timed so the reports can tell the game's main thread waiting on the GPU from it working.
extern "C" PPC_FUNC(__imp__sub_8240F1B8);
PPC_FUNC(sub_8240F1B8)
{
    debug::threadtimes::Timer t(debug::threadtimes::kFenceSpin, uint32_t(ctx.lr));
    __imp__sub_8240F1B8(ctx, base);
}
