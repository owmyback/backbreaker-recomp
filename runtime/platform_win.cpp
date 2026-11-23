// Windows versions of the POSIX calls in platform.h (#174). Built on Windows only (CMakeLists.txt).
#include "platform.h"

#include <cerrno>
#include <windows.h>

namespace platform
{
    namespace
    {
        // The overlapped offset of a synchronous handle: the call reads or writes there and waits
        OVERLAPPED at(int64_t offset)
        {
            OVERLAPPED o{};
            o.Offset = DWORD(uint64_t(offset));
            o.OffsetHigh = DWORD(uint64_t(offset) >> 32);
            return o;
        }

        int errnoFor(DWORD error)
        {
            switch (error) {
            case ERROR_ACCESS_DENIED: return EACCES;
            case ERROR_INVALID_HANDLE: return EBADF;
            case ERROR_NOACCESS: return EFAULT; // the buffer: a watched guest page (memory.h, readFile)
            case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return ENOSPC;
            default: return EIO;
            }
        }
    }

    ssize_t pread(int fd, void* buf, size_t n, int64_t offset)
    {
        HANDLE h = HANDLE(_get_osfhandle(fd));
        if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
        OVERLAPPED o = at(offset);
        DWORD got = 0;
        const DWORD want = n > 0x7FFFF000 ? 0x7FFFF000 : DWORD(n);
        if (!ReadFile(h, buf, want, &got, &o)) {
            const DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF) return 0; // at or past the end: nothing, as POSIX
            errno = errnoFor(e);
            return -1;
        }
        return ssize_t(got);
    }

    ssize_t pwrite(int fd, const void* buf, size_t n, int64_t offset)
    {
        HANDLE h = HANDLE(_get_osfhandle(fd));
        if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
        OVERLAPPED o = at(offset);
        DWORD put = 0;
        const DWORD want = n > 0x7FFFF000 ? 0x7FFFF000 : DWORD(n);
        if (!WriteFile(h, buf, want, &put, &o)) {
            errno = errnoFor(GetLastError());
            return -1;
        }
        return ssize_t(put);
    }

    int fnmatch(const char* p, const char* s, int)
    {
        // Backtracking over the last '*' only: enough for a glob, and linear in practice
        const char* star = nullptr;
        const char* resume = nullptr;
        while (*s) {
            if (*p == '*') { star = p++; resume = s; continue; }
            bool ok = false;
            const char* next = p + 1;
            if (*p == '?') ok = true;
            else if (*p == '[') {
                const char* q = p + 1;
                const bool negate = *q == '!' || *q == '^';
                if (negate) q++;
                bool in = false;
                for (bool first = true; *q && (first || *q != ']'); first = false) {
                    char lo = *q++, hi = lo;
                    if (*q == '-' && q[1] && q[1] != ']') { hi = q[1]; q += 2; }
                    if (*s >= lo && *s <= hi) in = true;
                }
                if (*q == ']') { ok = in != negate; next = q + 1; }
                else ok = *s == '['; // no closing ']': a plain '['
            }
            else ok = *p && *p == *s;
            if (ok) { p = next; s++; continue; }
            if (!star) return 1;
            p = star + 1;
            s = ++resume;
        }
        while (*p == '*') p++;
        return *p ? 1 : 0;
    }

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

    void sleepUntilPrecise(std::chrono::steady_clock::time_point when)
    {
        using namespace std::chrono;
        // One timer for each thread that sleeps this way, kept for the thread's life. Windows 10
        // 1803 and newer have the high-resolution kind; without it, whole milliseconds of Sleep.
        static thread_local HANDLE timer =
            CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        constexpr auto spin = microseconds(500);
        const auto left = when - steady_clock::now();
        if (left > spin)
        {
            if (timer)
            {
                LARGE_INTEGER due;
                due.QuadPart = -(duration_cast<nanoseconds>(left - spin).count() / 100); // relative, in 100 ns
                if (due.QuadPart < 0 && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
            }
            else
            {
                const auto ms = duration_cast<milliseconds>(left - spin).count() - 1; // a Sleep can run a tick over
                if (ms > 0) Sleep(DWORD(ms));
            }
        }
        while (steady_clock::now() < when) YieldProcessor();
    }
}
