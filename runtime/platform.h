#pragma once
// The few POSIX calls the runtime uses that Windows (mingw-w64 with the UCRT) lacks or names
// differently (#174). On Linux this adds nothing. CMake builds Windows with
// _POSIX_THREAD_SAFE_FUNCTIONS (mingw's localtime_r and gmtime_r) and _FILE_OFFSET_BITS=64
// (64-bit off_t, stat and lseek), and the program's manifest makes UTF-8 its code page, so the
// narrow-string file calls take the same UTF-8 paths as on Linux.
#ifdef _WIN32
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <io.h>
#include <sys/types.h>

// Not inherited by child processes, as on Linux; and binary, never the CRT's text mode, which
// would turn \r\n into \n on every read
#ifndef O_CLOEXEC
#define O_CLOEXEC (_O_NOINHERIT | _O_BINARY)
#endif

namespace platform
{
    // pread/pwrite on a CRT descriptor: ReadFile/WriteFile at an offset. Unlike POSIX they move the
    // file pointer, which nothing here relies on (every read and write of these files is positioned).
    ssize_t pread(int fd, void* buf, size_t n, int64_t offset);
    ssize_t pwrite(int fd, const void* buf, size_t n, int64_t offset);
    // fnmatch(pattern, name, 0): *, ? and [...] (with ! or ^, and ranges); 0 when it matches
    int fnmatch(const char* pattern, const char* name, int flags);
}
using platform::fnmatch;
using platform::pread;
using platform::pwrite;

inline time_t timegm(struct tm* t) { return _mkgmtime(t); }
#endif
