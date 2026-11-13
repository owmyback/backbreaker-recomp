#include "sampler.h"
#include "../kernel/kernel.h"
#include "../log.h"

#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace debug
{
    namespace
    {
        // One sample at a time: the sampler thread sets the target, signals it and waits.
        void* g_frames[64];
        std::atomic<int> g_count{0};
        std::atomic<bool> g_done{false};

        void onSample(int)
        {
            int saved = errno;
            g_count = backtrace(g_frames, 64);
            g_done = true;
            errno = saved;
        }

        // "sub_XXXXXXXX" of a host frame in recompiled code, or the plain symbol, or ""
        std::string frameName(void* pc, bool& guest)
        {
            Dl_info di{};
            guest = false;
            if (!dladdr(pc, &di) || !di.dli_sname) return "";
            const char* s = strstr(di.dli_sname, "sub_");
            if (s && strlen(s) >= 12)
            {
                guest = true;
                return std::string(s, 12);
            }
            return di.dli_sname;
        }

        // Short readable form of a host symbol (import handlers are __imp__Name)
        std::string shortHost(const std::string& sym)
        {
            const char* imp = strstr(sym.c_str(), "__imp__");
            if (imp) return std::string(imp + 7).substr(0, 40);
            return sym.substr(0, 40);
        }

        void run(double atSeconds, int samples)
        {
            std::this_thread::sleep_for(std::chrono::duration<double>(atSeconds));
            void* warm[4];
            backtrace(warm, 4); // first call may allocate; do it outside a signal handler

            struct sigaction sa{};
            sa.sa_handler = onSample;
            sa.sa_flags = SA_RESTART;
            sigemptyset(&sa.sa_mask);
            int sig = SIGRTMIN + 4;
            sigaction(sig, &sa, nullptr);

            // per guest thread id: stack key -> count, plus the innermost host function seen
            std::map<uint32_t, std::map<std::string, int>> stacks;
            std::map<uint32_t, int> taken;
            for (int i = 0; i < samples; i++)
            {
                for (kernel::GuestThread* t : kernel::allThreads())
                {
                    if (!t->hostRunning) continue;
                    g_done = false;
                    if (pthread_kill(pthread_t(t->hostThread), sig) != 0) continue;
                    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
                    while (!g_done && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
                    if (!g_done) continue;
                    // Innermost host function (an import or runtime routine) + up to 6 guest frames
                    std::string host, key;
                    int guestFrames = 0;
                    for (int f = 2; f < g_count && guestFrames < 6; f++) // skip onSample + signal trampoline
                    {
                        bool guest;
                        std::string name = frameName(g_frames[f], guest);
                        if (guest)
                        {
                            if (key.find(name) == std::string::npos) { key += (key.empty() ? "" : " < ") + name; guestFrames++; }
                        }
                        else if (key.empty() && host.empty() && !name.empty())
                            host = shortHost(name);
                    }
                    if (key.empty()) key = "(no guest frames)";
                    if (!host.empty()) key = "[" + host + "] " + key;
                    stacks[t->id][key]++;
                    taken[t->id]++;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            std::fprintf(stderr, "\n==== guest thread samples (%d rounds at %.1f s; innermost first) ====\n", samples, atSeconds);
            for (auto& [id, m] : stacks)
            {
                std::vector<std::pair<int, std::string>> sorted;
                for (auto& [k, n] : m) sorted.push_back({ n, k });
                std::sort(sorted.rbegin(), sorted.rend());
                std::fprintf(stderr, "thread %u (%d samples):\n", id, taken[id]);
                for (size_t k = 0; k < sorted.size() && k < 3; k++)
                    std::fprintf(stderr, "  %3d%%  %s\n", sorted[k].first * 100 / taken[id], sorted[k].second.c_str());
            }
            std::fprintf(stderr, "==== end of samples ====\n\n");
        }
    }

    void scheduleThreadSampling(double atSeconds, int samples)
    {
        std::thread(run, atSeconds, samples).detach();
    }
}
