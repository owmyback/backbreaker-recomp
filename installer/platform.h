// What the installer needs from the system that Linux and Windows do differently (#138, #174):
// the program's own path, free space and memory, the compile jobs as child processes, starting the
// game, the install lock and, on Windows, the Start-menu shortcut. installer/platform.cpp has both
// versions (POSIX and Win32), so every other installer file builds unchanged on both.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
// The disc reader's POSIX file calls (installer/disc.cpp), as runtime/platform.h has them for the
// game: O_CLOEXEC also opens in binary mode, never the CRT's text mode, which would turn the disc's
// \n bytes into \r\n as they're copied; pread reads at an offset.
#include <fcntl.h>
#include <io.h>
#include <sys/types.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC (_O_NOINHERIT | _O_BINARY)
#endif
namespace installer::platform
{
    ssize_t pread(int fd, void* buf, size_t n, int64_t offset);
}
using installer::platform::pread;
#endif

namespace installer::platform
{
#ifdef _WIN32
    constexpr const char* kExe = ".exe"; // what a program's file name ends in
#else
    constexpr const char* kExe = "";
#endif

    // This program's own path; empty when the system won't say
    std::string selfPath();

    // Bytes this user may still write where `dir` is (which has to exist); false when unknown
    bool freeSpace(const std::string& dir, uint64_t& bytes);

    // Memory free for new work (Linux's MemAvailable), in bytes; 0 when unknown
    uint64_t availableMemory();

    // The most compile jobs that can run at once (Windows waits for at most 64 processes)
    unsigned maxJobs();

    // A child process: a pid on Linux, a process handle on Windows
    using Child = intptr_t;

    // Starts argv with its stdout and stderr going to `log`, the toolchain's libraries first on the
    // library path (Linux; on Windows they're beside the programs). The environment is the caller's.
    bool spawn(const std::vector<std::string>& argv, const std::string& libraryPath, const std::string& log,
               Child& child, std::string& error);
    // Waits for one of `running` to end: which one, and whether it exited with 0. False when there's
    // nothing to wait for.
    bool waitAny(const std::vector<Child>& running, size_t& index, bool& succeeded);
    void terminate(Child child);

    // The game, started as the launcher's start step: on Linux it takes this process's place (execv,
    // so a launcher's stop reaches it); on Windows a child in a job that ends with this process, whose
    // exit code this returns. Returns only when it couldn't start (Linux) or once it ended (Windows);
    // `started` says which.
    int runGame(const std::vector<std::string>& argv, bool& started, std::string& error);

    // <prefix>/.lock held for the life of the object, so two installers don't build into one
    // folder at once
    class FileLock
    {
    public:
        // `busy` says another installer holds it (otherwise the folder can't be written)
        bool acquire(const std::string& path, bool& busy, std::string& error);
        ~FileLock();

    private:
        intptr_t handle_ = -1;
    };

#ifdef _WIN32
    // %LOCALAPPDATA%, else %USERPROFILE%\AppData\Local
    std::string localAppData();

    // The Start menu's own Programs folder for this user, and Backbreaker's shortcut in it: a .lnk
    // starting `target` in its folder with `icon` (an .ico). Replaced when it's there already.
    std::string startMenuShortcutPath();
    bool writeStartMenuShortcut(const std::string& target, const std::string& icon, std::string& error);

    // Steam's folder from the registry (HKCU\Software\Valve\Steam SteamPath, else the machine's
    // InstallPath); empty when Steam isn't installed
    std::string steamFolder();

    // A process named steam.exe (any user's; Steam runs one per machine)
    bool processRunning(const char* exeName);

    // Whether this console was made for this process alone (double-clicked from Explorer): it then
    // closes when the program ends, so a message would vanish unread
    bool ownsConsole();
#endif
}
