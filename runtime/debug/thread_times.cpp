#include "thread_times.h"
#include "../log.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <cxxabi.h>
#ifdef _WIN32
// Windows (#174): thread CPU from QueryThreadCycleTime (there's no time-waiting-for-a-core there),
// the other threads from a Toolhelp snapshot, the main thread's stacks by suspending it from a
// sampler thread and unwinding with its .pdata, and names from the program's COFF symbol table
#include <windows.h>
#include <tlhelp32.h>
#include <fstream>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <unordered_map>
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
        // A frame this long gets a report: 80 ms, or BB_HITCH_MS (at least 20) to look at shorter ones
        const double kHitchSeconds = getenv("BB_HITCH_MS") ? std::max(20.0, atof(getenv("BB_HITCH_MS"))) / 1000 : 0.08;
        constexpr size_t kMaxLines = 8, kSecondLines = 6;
        // The guest thread whose stacks the hitch reports sample: the game's main thread, or
        // BB_STACK_GUEST=<n> to look at a worker it waits for
        const uint32_t g_stackGuest = getenv("BB_STACK_GUEST") ? uint32_t(std::max(1, atoi(getenv("BB_STACK_GUEST")))) : 1;
        // BB_STACK_CP=1: the command processor's stacks instead (the renderer: when the game waits
        // on the GPU fence, it's what the frame waits for)
        const bool g_stackCp = getenv("BB_STACK_CP") != nullptr;
        std::string stackLabel() { return g_stackCp ? std::string("the command processor") : "guest " + std::to_string(g_stackGuest); }

        // One host thread's counters. Never freed: the sampler may still read a thread's last values
        // after it exits.
#ifdef _WIN32
        using pid_t = unsigned long; // a thread id
#endif
        struct Counters
        {
            pid_t tid = 0;
            int fd = -1;               // its schedstat; -1 none, -2 the thread is gone (Windows: 0 once `thread` is open)
#ifdef _WIN32
            HANDLE thread = nullptr;   // for its cycle time
#endif
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

#ifdef _WIN32
        pid_t tidNow() { return GetCurrentThreadId(); }

        // QueryThreadCycleTime counts the TSC's cycles: its rate, measured once against the steady clock
        double nsPerCycle()
        {
            static const double v = [] {
                const auto t0 = std::chrono::steady_clock::now();
                const uint64_t c0 = __rdtsc();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                const uint64_t c1 = __rdtsc();
                const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
                return c1 > c0 ? ns / double(c1 - c0) : 0.25;
            }();
            return v;
        }

        bool readThreadTimes(HANDLE thread, uint64_t& run, uint64_t& wait)
        {
            ULONG64 cycles = 0;
            if (!thread || !QueryThreadCycleTime(thread, &cycles)) return false;
            run = uint64_t(double(cycles) * nsPerCycle());
            wait = 0; // Windows doesn't count a thread's time waiting for a core
            return true;
        }
#else
        pid_t tidNow() { return pid_t(syscall(SYS_gettid)); }
#endif
        double seconds(uint64_t ns) { return double(ns) / 1e9; }

        void samplerLoop();

        // ---- the game's main thread's stacks ----
        //
        // A CPU-time timer on guest thread 1 signals it every 2 ms of its own CPU time (so a thread
        // that waits isn't interrupted), and the handler keeps the host return addresses in a ring.
        // A hitch report symbolizes the samples of its window: which recompiled functions (sub_...) or
        // imports the main thread was in. The other threads' reports say what they waited for.
        constexpr int kStackFrames = 28;
        constexpr size_t kStackRing = 8192; // about 20 s of the main thread at 350 samples a second
        constexpr long kStackPeriodNs = 2000000;
        struct StackSample
        {
            std::atomic<uint64_t> seq{0}; // the sample's number + 1 once complete, 0 while written
            uint64_t t = 0;
            int n = 0;
            void* pc[kStackFrames];
        };
        StackSample* g_stacks = nullptr;
        std::atomic<uint64_t> g_stackCount{0};

        void keepSample(void* const* pc, int n)
        {
            const uint64_t i = g_stackCount.load(std::memory_order_relaxed);
            StackSample& s = g_stacks[i % kStackRing];
            s.seq.store(0, std::memory_order_relaxed);
            std::atomic_signal_fence(std::memory_order_seq_cst);
            s.t = nowNs();
            s.n = n;
            std::copy(pc, pc + n, s.pc);
            s.seq.store(i + 1, std::memory_order_release);
            g_stackCount.store(i + 1, std::memory_order_release);
        }

#ifdef _WIN32
        // Windows: no CPU-time timer can signal a thread, so a sampler thread suspends the main
        // thread every 2 ms of wall time (a waiting thread is sampled too: its wait shows), reads its
        // registers and unwinds its stack with the program's .pdata, then lets it go. Nothing is
        // allocated or logged while it's suspended.
        constexpr int kSkipFrames = 0; // the samples start at the thread's own instruction
        HANDLE g_sampled = nullptr;

        int unwind(CONTEXT c, void** pc, int max)
        {
            int n = 0;
            while (n < max && c.Rip)
            {
                pc[n++] = (void*)c.Rip;
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &imageBase, nullptr);
                if (!f) // a leaf function: its return address is on top of the stack
                {
                    if (!c.Rsp) break;
                    c.Rip = *(const DWORD64*)c.Rsp;
                    c.Rsp += 8;
                    continue;
                }
                PVOID handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, c.Rip, f, &c, &handlerData, &establisher, nullptr);
            }
            return n;
        }

        void stackSamplerLoop()
        {
            SetThreadDescription(GetCurrentThread(), L"bb-stacks");
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            auto next = std::chrono::steady_clock::now();
            for (;;)
            {
                next += std::chrono::nanoseconds(kStackPeriodNs);
                std::this_thread::sleep_until(next);
                if (SuspendThread(g_sampled) == DWORD(-1)) return; // the thread is gone
                CONTEXT c{};
                c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                void* pc[kStackFrames];
                const int n = GetThreadContext(g_sampled, &c) ? unwind(c, pc, kStackFrames) : 0;
                ResumeThread(g_sampled);
                if (n) keepSample(pc, n);
            }
        }

        // On the thread to sample (the game's main thread), once
        void startStackSampling()
        {
            if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_sampled,
                                 THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
            {
                warn("[threads] can't sample the main thread's stacks (error %lu): the hitch reports won't name its functions", GetLastError());
                return;
            }
            g_stacks = new StackSample[kStackRing];
            std::thread(stackSamplerLoop).detach();
            info("[threads] sampling the game's main thread every %ld ms for the hitch reports", kStackPeriodNs / 1000000);
        }
#else
        constexpr int kSkipFrames = 2; // the signal handler and the signal trampoline

        void onStackSample(int, siginfo_t*, void*)
        {
            const int saved = errno;
            const uint64_t i = g_stackCount.load(std::memory_order_relaxed);
            StackSample& s = g_stacks[i % kStackRing];
            s.seq.store(0, std::memory_order_relaxed);
            std::atomic_signal_fence(std::memory_order_seq_cst);
            s.t = nowNs();
            s.n = backtrace(s.pc, kStackFrames);
            s.seq.store(i + 1, std::memory_order_release);
            g_stackCount.store(i + 1, std::memory_order_release);
            errno = saved;
        }

        // On the thread to sample (the game's main thread), once
        void startStackSampling()
        {
            g_stacks = new StackSample[kStackRing];
            void* warm[4];
            backtrace(warm, 4); // the unwinder's first use may allocate: not inside a signal handler
            const int sig = SIGRTMIN + 5; // the hang sampler (debug/sampler.cpp) uses SIGRTMIN + 4
            struct sigaction sa{};
            sa.sa_sigaction = onStackSample;
            sa.sa_flags = SA_SIGINFO | SA_RESTART;
            sigemptyset(&sa.sa_mask);
            clockid_t cpuClock;
            sigevent sev{};
            sev.sigev_notify = SIGEV_THREAD_ID;
            sev.sigev_signo = sig;
            sev._sigev_un._tid = tidNow();
            timer_t timer;
            itimerspec period{};
            period.it_interval.tv_nsec = period.it_value.tv_nsec = kStackPeriodNs;
            if (sigaction(sig, &sa, nullptr) != 0 || pthread_getcpuclockid(pthread_self(), &cpuClock) != 0 ||
                timer_create(cpuClock, &sev, &timer) != 0 || timer_settime(timer, 0, &period, nullptr) != 0)
            {
                warn("[threads] can't sample the main thread's stacks (%s): the hitch reports won't name its functions", strerror(errno));
                return;
            }
            info("[threads] sampling the game's main thread every %ld ms of its CPU time for the hitch reports", kStackPeriodNs / 1000000);
        }
#endif

#ifdef _WIN32
        // The program's COFF symbol table, read from its file once (the game keeps one: the
        // installer doesn't strip it): every function's address, as an RVA, and its name
        struct Symbols
        {
            uintptr_t base = 0, size = 0; // the program in memory
            std::vector<std::pair<uint32_t, std::string>> byRva; // sorted
        };

        const Symbols& symbols()
        {
            static const Symbols s = [] {
                Symbols out;
                out.base = uintptr_t(GetModuleHandleW(nullptr));
                const auto* nt = (const IMAGE_NT_HEADERS*)(out.base + ((const IMAGE_DOS_HEADER*)out.base)->e_lfanew);
                out.size = nt->OptionalHeader.SizeOfImage;
                wchar_t path[1024];
                const DWORD len = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
                if (!len || len >= std::size(path)) return out;
                std::ifstream f(path, std::ios::binary);
                auto readAt = [&](uint64_t at, void* to, size_t n) {
                    f.seekg(std::streamoff(at));
                    return bool(f.read((char*)to, std::streamsize(n)));
                };
                IMAGE_DOS_HEADER dos{};
                IMAGE_FILE_HEADER fh{};
                if (!readAt(0, &dos, sizeof dos) || !readAt(uint64_t(dos.e_lfanew) + 4, &fh, sizeof fh) || !fh.PointerToSymbolTable)
                    return out;
                std::vector<IMAGE_SECTION_HEADER> sections(fh.NumberOfSections);
                const uint64_t sectionsAt = uint64_t(dos.e_lfanew) + 4 + sizeof fh + fh.SizeOfOptionalHeader;
                if (!readAt(sectionsAt, sections.data(), sections.size() * sizeof(IMAGE_SECTION_HEADER))) return out;
                std::vector<uint8_t> table(size_t(fh.NumberOfSymbols) * IMAGE_SIZEOF_SYMBOL);
                uint32_t stringsSize = 0;
                const uint64_t stringsAt = fh.PointerToSymbolTable + table.size();
                if (!readAt(fh.PointerToSymbolTable, table.data(), table.size()) || !readAt(stringsAt, &stringsSize, 4)) return out;
                std::vector<char> strings(stringsSize > 4 ? stringsSize : 4);
                if (stringsSize > 4 && !readAt(stringsAt, strings.data(), strings.size())) return out;
                for (uint32_t i = 0; i < fh.NumberOfSymbols; i++)
                {
                    IMAGE_SYMBOL sym;
                    std::memcpy(&sym, table.data() + size_t(i) * IMAGE_SIZEOF_SYMBOL, IMAGE_SIZEOF_SYMBOL);
                    const uint32_t aux = sym.NumberOfAuxSymbols;
                    if (sym.SectionNumber > 0 && sym.SectionNumber <= int(sections.size()) &&
                        (sections[sym.SectionNumber - 1].Characteristics & IMAGE_SCN_CNT_CODE) &&
                        (sym.StorageClass == IMAGE_SYM_CLASS_EXTERNAL || sym.StorageClass == IMAGE_SYM_CLASS_STATIC))
                    {
                        std::string name = sym.N.Name.Short ? std::string((const char*)sym.N.ShortName, strnlen((const char*)sym.N.ShortName, 8))
                                                            : (sym.N.Name.Long < strings.size() ? std::string(strings.data() + sym.N.Name.Long) : std::string());
                        // Not section names (.text) nor weak aliases (.weak.<name>.default...): the function's own name
                        if (!name.empty() && name[0] != '.')
                            out.byRva.push_back({ sections[sym.SectionNumber - 1].VirtualAddress + sym.Value, std::move(name) });
                    }
                    i += aux;
                }
                std::sort(out.byRva.begin(), out.byRva.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                return out;
            }();
            return s;
        }

        // The mangled name of the function `pc` is in, or "" when the symbol table doesn't have it;
        // outside the program, the name of the DLL it's in (ntdll.dll, the GPU driver's...)
        std::string rawName(void* pc)
        {
            const Symbols& s = symbols();
            if (uintptr_t(pc) < s.base || uintptr_t(pc) >= s.base + s.size)
            {
                HMODULE m = nullptr;
                wchar_t path[512];
                if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)pc, &m) ||
                    !GetModuleFileNameW(m, path, DWORD(std::size(path))))
                    return {};
                const wchar_t* file = wcsrchr(path, L'\\');
                file = file ? file + 1 : path;
                char narrow[128];
                return WideCharToMultiByte(CP_UTF8, 0, file, -1, narrow, sizeof narrow, nullptr, nullptr) > 0 ? std::string(narrow) : std::string();
            }
            if (s.byRva.empty()) return {};
            const uint32_t rva = uint32_t(uintptr_t(pc) - s.base);
            auto it = std::upper_bound(s.byRva.begin(), s.byRva.end(), rva, [](uint32_t v, const auto& e) { return v < e.first; });
            return it == s.byRva.begin() ? std::string() : std::prev(it)->second;
        }
#else
        std::string rawName(void* pc)
        {
            Dl_info di{};
            return dladdr(pc, &di) && di.dli_sname ? std::string(di.dli_sname) : std::string();
        }
#endif

        // "sub_XXXXXXXX" for recompiled code, "[Name]" for an import or a runtime function, "" if unknown.
        // On the sampler thread only (cached: the same addresses come back).
        const std::string& frameName(void* pc)
        {
            static std::unordered_map<void*, std::string> cache;
            auto it = cache.find(pc);
            if (it != cache.end()) return it->second;
            std::string name;
            const std::string raw = rawName(pc);
            if (!raw.empty())
            {
                // Imports are C++ functions named __imp__<Name>(PPCContext&, unsigned char*): demangle
                // first, then keep the name up to its arguments
                int status = 0;
                char* d = abi::__cxa_demangle(raw.c_str(), nullptr, nullptr, &status);
                std::string full = status == 0 && d ? d : raw;
                free(d);
                // "(anonymous namespace)::" would end the name at its own '(' (the runtime's functions)
                for (size_t at; (at = full.find("(anonymous namespace)::")) != std::string::npos;) full.erase(at, 23);
                full = full.substr(0, full.find('('));
                if (size_t sub = full.find("sub_"); sub != std::string::npos && full.size() >= sub + 12) name = full.substr(sub, 12);
                else if (size_t imp = full.find("__imp__"); imp != std::string::npos) name = "[" + full.substr(imp + 7, 40) + "]";
                else name = "[" + (full.size() > 48 ? full.substr(full.size() - 48) : full) + "]"; // the end names the function
            }
            return cache.emplace(pc, std::move(name)).first->second;
        }

        // The main thread's samples from t0 to t1 (steady clock ns), symbolized innermost first
        std::vector<std::vector<std::string>> stackSamples(uint64_t t0, uint64_t t1)
        {
            std::vector<std::vector<std::string>> out;
            if (!g_stacks) return out;
            const uint64_t end = g_stackCount.load(std::memory_order_acquire);
            for (uint64_t i = end > kStackRing ? end - kStackRing : 0; i < end; i++)
            {
                StackSample& s = g_stacks[i % kStackRing];
                if (s.seq.load(std::memory_order_acquire) != i + 1) continue;
                const uint64_t t = s.t;
                const int n = std::min(s.n, kStackFrames);
                void* pc[kStackFrames];
                std::copy(s.pc, s.pc + n, pc);
                if (s.seq.load(std::memory_order_acquire) != i + 1 || t < t0 || t > t1) continue; // overwritten meanwhile, or outside
                std::vector<std::string> frames;
                for (int f = kSkipFrames; f < n; f++) // past the handler and the signal trampoline (Linux)
                {
                    const std::string& name = frameName(pc[f]);
                    if (!name.empty()) frames.push_back(name);
                }
                out.push_back(std::move(frames));
            }
            return out;
        }

        Counters* counters()
        {
            if (t_counters) return t_counters;
            auto* c = new Counters;
            c->tid = tidNow();
#ifdef _WIN32
            c->thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, c->tid);
            c->fd = c->thread ? 0 : -1;
#else
            char path[64];
            snprintf(path, sizeof path, "/proc/self/task/%d/schedstat", int(c->tid));
            c->fd = open(path, O_RDONLY | O_CLOEXEC);
#endif
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
#ifdef _WIN32
            wchar_t wide[32];
            if (name && MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 32) > 0) SetThreadDescription(GetCurrentThread(), wide);
#else
            if (name && tidNow() != getpid()) pthread_setname_np(pthread_self(), name); // not the process's own name
#endif
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
#ifdef _WIN32
            HANDLE thread = nullptr;
#endif
            uint64_t run = 0, wait = 0;
            bool seen = false;
        };

#ifdef _WIN32
        bool readStats(const Counters* c, uint64_t& run, uint64_t& wait) { return readThreadTimes(c->thread, run, wait); }
        bool readStats(const Other& o, uint64_t& run, uint64_t& wait) { return readThreadTimes(o.thread, run, wait); }
        void closeStats(Counters* c) { CloseHandle(c->thread); c->thread = nullptr; }

        // Threads this process has that never registered (the GPU driver's, SDL's): watched as one group
        void rescanOthers(std::map<pid_t, Other>& others, const std::set<pid_t>& registered)
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE) return;
            const DWORD self = GetCurrentProcessId();
            std::set<pid_t> alive;
            THREADENTRY32 e{};
            e.dwSize = sizeof e;
            for (BOOL more = Thread32First(snap, &e); more; more = Thread32Next(snap, &e))
            {
                if (e.th32OwnerProcessID != self) continue;
                const pid_t tid = e.th32ThreadID;
                alive.insert(tid);
                if (registered.count(tid) || others.count(tid)) continue;
                if (HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid)) others[tid].thread = h;
            }
            CloseHandle(snap);
            for (auto it = others.begin(); it != others.end();)
                if (!alive.count(it->first) || registered.count(it->first))
                {
                    CloseHandle(it->second.thread);
                    it = others.erase(it);
                }
                else ++it;
        }
#else
        bool readSchedstat(int fd, uint64_t& run, uint64_t& wait)
        {
            char buf[128];
            ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
            if (n <= 0) return false;
            buf[n] = 0;
            return parseSchedstat(buf, run, wait);
        }
        bool readStats(const Counters* c, uint64_t& run, uint64_t& wait) { return readSchedstat(c->fd, run, wait); }
        bool readStats(const Other& o, uint64_t& run, uint64_t& wait) { return readSchedstat(o.fd, run, wait); }
        void closeStats(Counters* c) { close(c->fd); }

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
#endif

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
                // What the main thread was running
                StackSummary sum = summarizeStacks(stackSamples(uint64_t(A.t * 1e9), uint64_t(B.t * 1e9)), "sub_8240F1B8", 4, 12, 4, g_stackCp);
                if (sum.samples)
                {
                    const double pct = 100.0 / double(sum.samples), rest = sum.samples > sum.fence ? 100.0 / double(sum.samples - sum.fence) : 0;
                    std::string line;
                    char buf[160];
                    for (const auto& [count, stack] : sum.stacks)
                    {
                        snprintf(buf, sizeof buf, " | %.0f%% ", count * pct);
                        line += buf + stack;
                    }
#ifdef _WIN32
                    const char* per = "ms, waiting or not"; // sampled by the clock (startStackSampling)
#else
                    const char* per = "ms of its CPU";
#endif
                    info("[hitch]   %s's stacks (%zu samples, one per %ld %s; innermost first): GPU fence spin %.0f%%%s", stackLabel().c_str(),
                         sum.samples, kStackPeriodNs / 1000000, per, sum.fence * pct, line.c_str());
                    line.clear();
                    for (const auto& [count, fn] : sum.functions)
                    {
                        snprintf(buf, sizeof buf, "%s%s %.0f%%", line.empty() ? "" : ", ", fn.c_str(), count * rest);
                        line += buf;
                    }
                    if (!line.empty())
                        info("[hitch]   %s's functions, by share of its samples outside the fence spin: %s", stackLabel().c_str(), line.c_str());
                }
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
#ifdef _WIN32
            if (t_counters->fd < 0) warn("[threads] can't open this thread for its CPU time: no CPU times in the reports");
            else info("[threads] Windows doesn't count a thread's time waiting for a core: the reports' core waits are 0");
#else
            if (t_counters->fd < 0)
                warn("[threads] /proc/self/task/<tid>/schedstat can't be read: no CPU or core-wait times in the reports");
#endif
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
                    if (c->fd >= 0 && !readStats(c, x.cpuNs, x.queueNs))
                    {
                        closeStats(c);
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
                    if (!readStats(o, run, wait)) continue;
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
#ifdef _WIN32
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#else
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#endif
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
        {
            std::lock_guard g(g_lock);
            c->guestId = guestId;
        }
        if (!g_stackCp && guestId == g_stackGuest && !g_stacks && !(getenv("BB_THREAD_STACKS") && getenv("BB_THREAD_STACKS")[0] == '0'))
            startStackSampling();
    }

    void registerRole(const char* label, const char* threadName)
    {
        setName(threadName);
        if (!g_on) return;
        Counters* c = counters();
        {
            std::lock_guard g(g_lock);
            c->role = label;
        }
        if (g_stackCp && !g_stacks && label && !strcmp(label, "gpu command processor")) startStackSampling();
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
