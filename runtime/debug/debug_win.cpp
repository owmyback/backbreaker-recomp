// Windows stand-in for the Linux-only guest thread sampler (#174): sampler.cpp interrupts threads
// with signals. Here sampling is skipped. (The per-thread timing and hitch reports, thread_times.cpp,
// run on Windows too.) Built on Windows only.
#include "sampler.h"
#include "../log.h"

namespace debug
{
    void scheduleThreadSampling(double, int)
    {
        warn("[sampler] guest thread sampling isn't available on Windows");
    }
}
