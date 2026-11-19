// Windows stand-ins for the Linux-only debugging aids (#174): the per-thread timing reports
// (thread_times.cpp reads /proc/self/task/<tid>/schedstat) and the guest thread sampler
// (sampler.cpp interrupts threads with signals). Here BB_PERF reports nothing and sampling is
// skipped; thread names are still set, so a debugger tells the threads apart. Built on Windows only.
#include "sampler.h"
#include "thread_times.h"
#include "../log.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <windows.h>

namespace debug
{
    void scheduleThreadSampling(double, int)
    {
        warn("[sampler] guest thread sampling isn't available on Windows");
    }
}

namespace debug::threadtimes
{
    namespace
    {
        void setName(const char* name)
        {
            wchar_t wide[32];
            if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, 32) > 0)
                SetThreadDescription(GetCurrentThread(), wide);
        }
    }

    bool enabled() { return false; }

    uint64_t nowNs()
    {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    void registerGuest(uint32_t guestId)
    {
        char name[16];
        std::snprintf(name, sizeof name, "bb-g%u", guestId);
        setName(name);
    }

    void registerRole(const char*, const char* threadName)
    {
        if (threadName) setName(threadName);
    }

    void nameGuest(uint32_t, const char*) {}

    Timer::Timer(Bucket bucket, uint32_t) : m_start(0), m_bucket(bucket) {}
    Timer::~Timer() { (void)m_start, (void)m_bucket; } // nothing is timed here

    void addFault(uint64_t) {}
    void frameEnded(double, double) {}
}
