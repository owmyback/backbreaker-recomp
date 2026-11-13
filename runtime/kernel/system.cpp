// Time, system information and process-level imports.
#include "import_support.h"
#include "kernel.h"
#include "../log.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <thread>
#include <x86intrin.h>

using namespace mem;

namespace
{
    // Recompiled `mftb` reads the host TSC, so report the host TSC rate as the timebase.
    uint64_t measureTscFrequency()
    {
        using clock = std::chrono::steady_clock;
        auto t0 = clock::now();
        uint64_t c0 = __rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        uint64_t c1 = __rdtsc();
        double secs = std::chrono::duration<double>(clock::now() - t0).count();
        return uint64_t((c1 - c0) / secs);
    }

    uint64_t tscFrequency()
    {
        static uint64_t f = [] {
            uint64_t v = measureTscFrequency();
            info("host TSC frequency: %.3f MHz", v / 1e6);
            return v;
        }();
        return f;
    }
}

namespace kernel
{
    // Windows FILETIME: 100 ns intervals since 1601-01-01. One definition, shared by
    // KeQuerySystemTime, the time stamp bundle below and absolute-timeout waits.
    uint64_t systemTime()
    {
        using namespace std::chrono;
        constexpr uint64_t kEpochDelta = 116444736000000000ull;
        return kEpochDelta + duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100;
    }
}

GUEST_IMPORT(KeQueryPerformanceFrequency) { ctx.r3.u64 = tscFrequency(); }
GUEST_IMPORT(KeQuerySystemTime) { if (ctx.r3.u32) write64(ctx.r3.u32, kernel::systemTime()); }

// NTSTATUS KeDelayExecutionThread(u32 WaitMode, BOOL Alertable, i64* Interval)  (negative = relative, 100 ns)
GUEST_IMPORT(KeDelayExecutionThread)
{
    int64_t interval = int64_t(read64(ctx.r5.u32));
    if (interval < 0)
        std::this_thread::sleep_for(std::chrono::nanoseconds(-interval * 100));
    else if (interval == 0)
        std::this_thread::yield();
    ctx.r3.u64 = STATUS_SUCCESS;
}

namespace
{
    // The real kernel refreshes KeTimeStampBundle from a DPC about every millisecond. Games read
    // it for coarse timing without an import call, so keep it current from a host thread.
    void timeStampLoop()
    {
        using namespace std::chrono;
        using clock = std::chrono::steady_clock;
        const uint32_t bundle = kernel::timeStampBundleAddress();
        // "Since boot": the thread starts just before the entry point, which is as close to the
        // guest's idea of process start as we get.
        const auto boot = clock::now();
        for (;;)
        {
            uint64_t since100ns = duration_cast<nanoseconds>(clock::now() - boot).count() / 100;
            write64(bundle + 0x00, since100ns);                        // u64 InterruptTime
            write64(bundle + 0x08, kernel::systemTime());              // u64 SystemTime (FILETIME)
            // TickCount last: a guest that reads the bundle without stopping the world never sees
            // a TickCount running ahead of the two fields written above it.
            write32(bundle + 0x10, uint32_t(since100ns / 10000));      // u32 TickCount (ms)
            std::this_thread::sleep_for(milliseconds(1));
        }
    }
}

namespace kernel
{
    void startTimeStampThread()
    {
        if (!timeStampBundleAddress())
        {
            info("KeTimeStampBundle not imported - no time stamp thread");
            return;
        }
        // Left running: the guest mapping is never unmapped, so writing from this thread is safe
        // right up to process exit (every exit path in the runtime ends in _exit/std::exit).
        std::thread(timeStampLoop).detach();
        info("time stamp bundle at %08X - updating it every millisecond", timeStampBundleAddress());
    }
}

// TIME_FIELDS: u16 Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday
GUEST_IMPORT(RtlTimeToTimeFields)
{
    uint64_t t = read64(ctx.r3.u32);
    uint32_t out = ctx.r4.u32;
    int64_t unix100ns = int64_t(t) - 116444736000000000ll;
    time_t secs = unix100ns / 10000000;
    struct tm tm{};
    gmtime_r(&secs, &tm);
    write16(out + 0, tm.tm_year + 1900);
    write16(out + 2, tm.tm_mon + 1);
    write16(out + 4, tm.tm_mday);
    write16(out + 6, tm.tm_hour);
    write16(out + 8, tm.tm_min);
    write16(out + 10, tm.tm_sec);
    write16(out + 12, (unix100ns / 10000) % 1000);
    write16(out + 14, tm.tm_wday);
}

GUEST_IMPORT(RtlTimeFieldsToTime)
{
    uint32_t in = ctx.r3.u32;
    struct tm tm{};
    tm.tm_year = read16(in) - 1900;
    tm.tm_mon = read16(in + 2) - 1;
    tm.tm_mday = read16(in + 4);
    tm.tm_hour = read16(in + 6);
    tm.tm_min = read16(in + 8);
    tm.tm_sec = read16(in + 10);
    int64_t secs = timegm(&tm);
    write64(ctx.r4.u32, uint64_t(secs * 10000000ll + read16(in + 12) * 10000ll + 116444736000000000ll));
    ctx.r3.u64 = 1;
}

// ---- Console configuration ----
GUEST_IMPORT(XGetLanguage) { ctx.r3.u64 = 1; }        // English
GUEST_IMPORT(XGetGameRegion) { ctx.r3.u64 = 0x00FF; } // North America
GUEST_IMPORT(XGetAVPack) { ctx.r3.u64 = 8; }          // HDMI
GUEST_IMPORT(KeGetCurrentProcessType) { ctx.r3.u64 = 1; } // title process

namespace kernel
{
    // XVIDEO_MODE as reported by XGetVideoMode / VdQueryVideoMode
    void fillVideoMode(uint32_t m)
    {
    memset(host(m), 0, 0x30);
    write32(m + 0x00, 1280);  // DisplayWidth
    write32(m + 0x04, 720);   // DisplayHeight
    write32(m + 0x08, 0);     // IsInterlaced
    write32(m + 0x0C, 1);     // IsWidescreen
    write32(m + 0x10, 1);     // IsHiDef
    uint32_t hz = 0x42700000; // 60.0f
    write32(m + 0x14, hz);    // RefreshRate
    write32(m + 0x18, 8);     // VideoStandard (NTSC-M)
    }
}

// void XGetVideoMode(XVIDEO_MODE* Mode)
GUEST_IMPORT(XGetVideoMode) { kernel::fillVideoMode(ctx.r3.u32); }

// void XamGetSystemVersion() -> packed 2.0.17559.0
GUEST_IMPORT(XamGetSystemVersion) { ctx.r3.u64 = (2u << 28) | (0u << 24) | (17559u << 8); }

// NTSTATUS ExGetXConfigSetting(u16 Category, u16 Setting, void* Buffer, u16 Size, u16* RequiredSize)
// Values follow Xenia: US English console, 720p widescreen.
GUEST_IMPORT(ExGetXConfigSetting)
{
    uint32_t category = ctx.r3.u16, setting = ctx.r4.u16, buf = ctx.r5.u32, size = ctx.r6.u16, pRequired = ctx.r7.u32;
    uint32_t value = 0, valueSize = 4;
    bool known = true;
    if (category == 2 && setting == 2)                   // XCONFIG_SECURED_AV_REGION
        value = 0x00001000;                              // NTSC-M (USA/Canada)
    else if (category == 3 && setting >= 1 && setting <= 7)
        value = 0;                                       // time zone fields
    else if (category == 3 && setting == 9)
        value = 1;                                       // XCONFIG_USER_LANGUAGE: English
    else if (category == 3 && setting == 10)
        value = 0x00040000;                              // XCONFIG_USER_VIDEO_FLAGS: widescreen
    else if (category == 3 && setting == 11)
        value = 0;                                       // XCONFIG_USER_AUDIO_FLAGS
    else if (category == 3 && setting == 12)
        value = 0;                                       // XCONFIG_USER_RETAIL_FLAGS
    else if (category == 3 && setting == 14)
        { value = 103; valueSize = 1; }                  // XCONFIG_USER_COUNTRY: US
    else if (category == 3 && setting == 15)
        { value = 0; valueSize = 1; }                    // XCONFIG_USER_PC_FLAGS
    else
        known = false;

    if (!known)
    {
        warn("ExGetXConfigSetting(category %u, setting %u) - unknown", category, setting);
        ctx.r3.u64 = STATUS_UNSUCCESSFUL;
        return;
    }
    if (pRequired) write16(pRequired, valueSize);
    if (buf)
    {
        if (size < valueSize) { ctx.r3.u64 = 0xC0000023; return; } // STATUS_BUFFER_TOO_SMALL
        if (valueSize == 1) write8(buf, value); else write32(buf, value);
    }
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(ExRegisterTitleTerminateNotification) {}
GUEST_IMPORT(FscSetCacheElementCount) { ctx.r3.u64 = STATUS_SUCCESS; }

GUEST_IMPORT(RtlNtStatusToDosError)
{
    uint32_t s = ctx.r3.u32;
    ctx.r3.u64 = s == 0 ? 0 : (s & 0xFFFF0000) == 0x80070000 ? (s & 0xFFFF) : 317; // ERROR_MR_MID_NOT_FOUND
}

// Fatal paths: report and stop.
GUEST_IMPORT(KeBugCheck) { fatal("guest KeBugCheck(%08X)", ctx.r3.u32); }
GUEST_IMPORT(KeBugCheckEx) { fatal("guest KeBugCheckEx(%08X, %08X, %08X, %08X)", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
GUEST_IMPORT(HalReturnToFirmware) { info("guest requested HalReturnToFirmware(%u) - exiting", ctx.r3.u32); kernel::printSummary(); std::exit(0); }
GUEST_IMPORT(XamLoaderTerminateTitle) { info("guest requested XamLoaderTerminateTitle - exiting"); kernel::printSummary(); std::exit(0); }
