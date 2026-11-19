#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

// One log line, formatted here and written to stderr in one call. fprintf straight onto stderr,
// which is unbuffered, writes a line in many small pieces on Windows' C runtime: 250 us a line,
// against 4 us this way, and the renderer logs hundreds of lines in a second when a scene starts
// new memory export streams, which made frames of 150-300 ms there (#174). On Linux it's the same
// one write as before.
[[gnu::format(printf, 1, 2)]] inline void bbLogLine(const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (size_t(n) < sizeof buf)
    {
        std::fwrite(buf, 1, size_t(n), stderr);
        return;
    }
    va_start(ap, fmt); // longer than the buffer: straight onto stderr, as before
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}

#define info(fmt, ...) bbLogLine("[rt] " fmt "\n", ##__VA_ARGS__)
#define warn(fmt, ...) bbLogLine("[rt] WARNING: " fmt "\n", ##__VA_ARGS__)
#define err(fmt, ...) bbLogLine("[rt] ERROR: " fmt "\n", ##__VA_ARGS__)
#define fatal(fmt, ...) do { bbLogLine("[rt] FATAL: " fmt "\n", ##__VA_ARGS__); std::abort(); } while (0)
