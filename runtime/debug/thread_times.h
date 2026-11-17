#pragma once
// Per-thread timing and hitch reports (#146), with BB_PERF=1 (BB_THREAD_TIMES=0 turns them off).
//
// Every thread that runs guest code or a runtime role registers itself. The calls that block or spin
// time themselves into the calling thread's counters (Timer), and a sampler thread reads each
// thread's time on a CPU and time waiting for one from /proc/self/task/<tid>/schedstat every 20 ms.
// A frame of 80 ms or more gets a [hitch] report: what every busy thread did during it. Once a
// second, a [perf] threads line gives the same for the second.
//
// Thread names (bb-gpu-cp, bb-g<N>, ...) are set whatever BB_PERF says, so `top -H` tells the
// threads apart.
#include "thread_times_core.h"

#include <cstdint>

namespace debug::threadtimes
{
    bool enabled();

    // The calling thread runs guest thread `guestId` (kernel::setCurrentThread)
    void registerGuest(uint32_t guestId);
    // The calling thread has a runtime role. A role replaces a guest label: the command processor
    // and the vblank thread run guest thread 2's interrupt callbacks. `threadName` (at most 15
    // characters) is set with pthread_setname_np; nullptr keeps the name (the main thread's is the
    // process's).
    void registerRole(const char* label, const char* threadName);
    // The game named guest thread `guestId` (the MSVC "set thread name" exception)
    void nameGuest(uint32_t guestId, const char* name);

    uint64_t nowNs(); // CLOCK_MONOTONIC, async-signal-safe

    // Times the enclosing scope into the calling thread's `bucket`; `lr` is the guest caller. Only
    // the outermost timer of a thread counts (no double counting when one timed call makes another).
    class Timer
    {
    public:
        Timer(Bucket bucket, uint32_t lr);
        ~Timer();
        Timer(const Timer&) = delete;
        Timer& operator=(const Timer&) = delete;

    private:
        uint64_t m_start = 0;
        Bucket m_bucket;
    };

    // A write-watch fault on the calling thread took `ns` (from the SIGSEGV handler: async-signal-safe)
    void addFault(uint64_t ns);

    // A frame from startS to endS (steady clock seconds) ended: a long one gets a report
    void frameEnded(double startS, double endS);
}
