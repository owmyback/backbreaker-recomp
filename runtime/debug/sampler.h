#pragma once
// Guest thread sampler (debugging aid for hangs): at a given time, every running guest thread
// is interrupted repeatedly and its host stack recorded; the most frequent guest call stacks
// (recompiled sub_XXXXXXXX functions, innermost first) are printed per thread. A thread stuck
// in a wait or polling loop shows up as one stack in (nearly) every sample.
namespace debug
{
    // Sample `samples` times, 20 ms apart, starting `atSeconds` after this call.
    void scheduleThreadSampling(double atSeconds, int samples = 50);
}
