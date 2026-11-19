#include "platform.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace installer::platform
{
#ifndef _WIN32
    // --- Linux (and other POSIX systems) ---------------------------------------------------------

    std::string selfPath()
    {
        char self[PATH_MAX];
        const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n <= 0) return {};
        self[n] = 0;
        return self;
    }

    bool freeSpace(const std::string& dir, uint64_t& bytes)
    {
        struct statvfs fs {};
        if (statvfs(dir.c_str(), &fs) != 0) return false;
        bytes = uint64_t(fs.f_bavail) * fs.f_frsize;
        return true;
    }

    uint64_t availableMemory()
    {
        std::ifstream meminfo("/proc/meminfo");
        std::string key, unit;
        uint64_t value = 0;
        while (meminfo >> key >> value >> unit)
            if (key == "MemAvailable:") return value * 1024;
        return 0;
    }

    unsigned maxJobs() { return UINT_MAX; }

    bool spawn(const std::vector<std::string>& argv, const std::string& libraryPath, const std::string& log,
               Child& child, std::string& error)
    {
        std::vector<std::string> env;
        for (char** e = environ; *e; e++)
            if (std::strncmp(*e, "LD_LIBRARY_PATH=", 16) != 0) env.emplace_back(*e);
        if (!libraryPath.empty()) env.push_back("LD_LIBRARY_PATH=" + libraryPath);
        std::vector<char*> args, envp;
        for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        for (const std::string& e : env) envp.push_back(const_cast<char*>(e.c_str()));
        envp.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
        pid_t pid = -1;
        const int rc = posix_spawn(&pid, argv[0].c_str(), &actions, nullptr, args.data(), envp.data());
        posix_spawn_file_actions_destroy(&actions);
        if (rc != 0) {
            error = "can't start " + argv[0] + " (" + std::strerror(rc) + ")";
            return false;
        }
        child = pid;
        return true;
    }

    bool waitAny(const std::vector<Child>& running, size_t& index, bool& succeeded)
    {
        if (running.empty()) return false;
        for (;;) {
            int status = 0;
            const pid_t pid = waitpid(-1, &status, 0);
            if (pid < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            const auto it = std::find(running.begin(), running.end(), Child(pid));
            if (it == running.end()) continue; // not one of these
            index = size_t(it - running.begin());
            succeeded = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            return true;
        }
    }

    void terminate(Child child) { kill(pid_t(child), SIGTERM); }

    int runGame(const std::vector<std::string>& argv, bool& started, std::string& error)
    {
        std::vector<std::string> args = argv;
        std::vector<char*> raw;
        for (std::string& s : args) raw.push_back(s.data());
        raw.push_back(nullptr);
        execv(args[0].c_str(), raw.data());
        started = false;
        error = std::strerror(errno);
        return -1;
    }

    bool FileLock::acquire(const std::string& path, bool& busy, std::string& error)
    {
        busy = false;
        const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) {
            error = std::strerror(errno);
            return false;
        }
        handle_ = fd;
        if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            busy = true;
            return false;
        }
        return true;
    }

    FileLock::~FileLock()
    {
        if (handle_ >= 0) close(int(handle_));
    }

#else
    // --- Windows (#174) ----------------------------------------------------------------------------
    // Strings are UTF-8 everywhere else (the manifest makes it the process's code page, as the
    // game's does); the Win32 calls here take them wide.

    namespace
    {
        std::wstring widen(const std::string& s)
        {
            if (s.empty()) return {};
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
            std::wstring w(size_t(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
            return w;
        }

        std::string narrow(const wchar_t* w, size_t length)
        {
            if (!length) return {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, w, int(length), nullptr, 0, nullptr, nullptr);
            std::string s(size_t(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w, int(length), s.data(), n, nullptr, nullptr);
            return s;
        }

        // "Windows error N: what it means", for messages
        std::string describe(DWORD code)
        {
            wchar_t* text = nullptr;
            const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                               FORMAT_MESSAGE_IGNORE_INSERTS,
                                           nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
            std::string what = n ? narrow(text, n) : std::string();
            if (text) LocalFree(text);
            while (!what.empty() && (what.back() == '\n' || what.back() == '\r' || what.back() == ' ' || what.back() == '.'))
                what.pop_back();
            return "Windows error " + std::to_string(code) + (what.empty() ? "" : ": " + what);
        }

        // One argument as the C runtime's command-line parser reads it back (CommandLineToArgvW's
        // rules): quoted when it has a blank or a quote, backslashes doubled only before a quote
        void appendArg(std::wstring& line, const std::wstring& arg)
        {
            if (!line.empty()) line += L' ';
            if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
                line += arg;
                return;
            }
            line += L'"';
            size_t slashes = 0;
            for (wchar_t c : arg) {
                if (c == L'\\') {
                    slashes++;
                    continue;
                }
                line.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
                slashes = 0;
                line += c;
            }
            line.append(slashes * 2, L'\\');
            line += L'"';
        }

        std::wstring commandLine(const std::vector<std::string>& argv)
        {
            std::wstring line;
            for (const std::string& a : argv) appendArg(line, widen(a));
            return line;
        }

        // A job whose processes end when its last handle closes, which is when this process ends:
        // a launcher's stop, or a closed console, takes the compiles or the game with it
        HANDLE killOnCloseJob()
        {
            HANDLE job = CreateJobObjectW(nullptr, nullptr);
            if (!job) return nullptr;
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
            return job;
        }

        HANDLE compileJob()
        {
            static HANDLE job = killOnCloseJob();
            return job;
        }
    }

    ssize_t pread(int fd, void* buf, size_t n, int64_t offset)
    {
        HANDLE h = HANDLE(_get_osfhandle(fd));
        if (h == INVALID_HANDLE_VALUE) {
            errno = EBADF;
            return -1;
        }
        OVERLAPPED at{};
        at.Offset = DWORD(uint64_t(offset));
        at.OffsetHigh = DWORD(uint64_t(offset) >> 32);
        DWORD got = 0;
        if (!ReadFile(h, buf, n > 0x7FFFF000 ? 0x7FFFF000 : DWORD(n), &got, &at)) {
            if (GetLastError() == ERROR_HANDLE_EOF) return 0; // at or past the end: nothing, as POSIX
            errno = EIO;
            return -1;
        }
        return ssize_t(got);
    }

    std::string selfPath()
    {
        std::wstring buf(MAX_PATH, L'\0');
        for (;;) {
            const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
            if (n == 0) return {};
            if (n < buf.size()) return narrow(buf.data(), n);
            buf.resize(buf.size() * 2);
        }
    }

    bool freeSpace(const std::string& dir, uint64_t& bytes)
    {
        ULARGE_INTEGER available{};
        if (!GetDiskFreeSpaceExW(widen(dir).c_str(), &available, nullptr, nullptr)) return false;
        bytes = available.QuadPart;
        return true;
    }

    uint64_t availableMemory()
    {
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof status;
        return GlobalMemoryStatusEx(&status) ? status.ullAvailPhys : 0;
    }

    unsigned maxJobs() { return MAXIMUM_WAIT_OBJECTS; }

    bool spawn(const std::vector<std::string>& argv, const std::string&, const std::string& log, Child& child,
               std::string& error)
    {
        SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
        HANDLE out = CreateFileW(widen(log).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) {
            error = "can't write " + log + " (" + describe(GetLastError()) + ")";
            return false;
        }
        // Only the log is inherited: the other jobs' logs stay closed to this one
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<uint8_t> attributes(size);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        InitializeProcThreadAttributeList(list, 1, 0, &size);
        UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &out, sizeof out, nullptr, nullptr);
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof si;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdOutput = out;
        si.StartupInfo.hStdError = out;
        si.lpAttributeList = list;
        PROCESS_INFORMATION pi{};
        std::wstring line = commandLine(argv);
        // No console of its own (a launcher may start this without one), suspended until it's in the job
        const BOOL ok = CreateProcessW(widen(argv[0]).c_str(), line.data(), nullptr, nullptr, TRUE,
                                       EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                       nullptr, &si.StartupInfo, &pi);
        const DWORD code = GetLastError();
        DeleteProcThreadAttributeList(list);
        CloseHandle(out);
        if (!ok) {
            error = "can't start " + argv[0] + " (" + describe(code) + ")";
            return false;
        }
        if (HANDLE job = compileJob()) AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        child = Child(pi.hProcess);
        return true;
    }

    bool waitAny(const std::vector<Child>& running, size_t& index, bool& succeeded)
    {
        if (running.empty() || running.size() > MAXIMUM_WAIT_OBJECTS) return false;
        std::vector<HANDLE> handles;
        for (Child c : running) handles.push_back(HANDLE(c));
        const DWORD r = WaitForMultipleObjects(DWORD(handles.size()), handles.data(), FALSE, INFINITE);
        if (r >= WAIT_OBJECT_0 + handles.size()) return false;
        index = r - WAIT_OBJECT_0;
        DWORD code = 1;
        GetExitCodeProcess(handles[index], &code);
        CloseHandle(handles[index]);
        succeeded = code == 0;
        return true;
    }

    void terminate(Child child) { TerminateProcess(HANDLE(child), 1); }

    int runGame(const std::vector<std::string>& argv, bool& started, std::string& error)
    {
        started = false;
        std::wstring line = commandLine(argv);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        // It shares this console, as the game's log in a terminal; started without one (a launcher),
        // it gets none either rather than a window of its own
        const DWORD flags = CREATE_SUSPENDED | (GetConsoleWindow() ? 0 : CREATE_NO_WINDOW);
        if (!CreateProcessW(widen(argv[0]).c_str(), line.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si, &pi)) {
            error = describe(GetLastError());
            return -1;
        }
        started = true;
        HANDLE job = killOnCloseJob();
        if (job) AssignProcessToJobObject(job, pi.hProcess);
        // Ctrl+C is the game's (it ends the run with its report); this waits for its exit code
        SetConsoleCtrlHandler(nullptr, TRUE);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        if (job) CloseHandle(job);
        return int(code);
    }

    bool FileLock::acquire(const std::string& path, bool& busy, std::string& error)
    {
        busy = false;
        // No sharing: a second installer's open fails until this one's handle closes, even if it crashed
        HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            busy = code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
            error = describe(code);
            return false;
        }
        handle_ = intptr_t(h);
        return true;
    }

    FileLock::~FileLock()
    {
        if (handle_ != -1) CloseHandle(HANDLE(handle_));
    }

    std::string localAppData()
    {
        if (const wchar_t* v = _wgetenv(L"LOCALAPPDATA"); v && *v) return narrow(v, wcslen(v));
        if (const wchar_t* h = _wgetenv(L"USERPROFILE"); h && *h) return narrow(h, wcslen(h)) + "\\AppData\\Local";
        return ".";
    }

    std::string startMenuShortcutPath()
    {
        PWSTR programs = nullptr;
        std::string path;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs)))
            path = narrow(programs, wcslen(programs)) + "\\Backbreaker.lnk";
        CoTaskMemFree(programs);
        return path;
    }

    bool writeStartMenuShortcut(const std::string& target, const std::string& icon, std::string& error)
    {
        const std::string path = startMenuShortcutPath();
        if (path.empty()) {
            error = "Windows didn't say where the Start menu's folder is";
            return false;
        }
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IShellLinkW* link = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                      reinterpret_cast<void**>(&link));
        if (SUCCEEDED(hr)) {
            const std::wstring exe = widen(target);
            const size_t slash = exe.find_last_of(L"\\/");
            link->SetPath(exe.c_str());
            link->SetWorkingDirectory(slash == std::wstring::npos ? L"." : exe.substr(0, slash).c_str());
            link->SetDescription(L"Backbreaker, built from your own disc (static recompilation)");
            if (!icon.empty()) link->SetIconLocation(widen(icon).c_str(), 0);
            IPersistFile* file = nullptr;
            hr = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file));
            if (SUCCEEDED(hr)) {
                hr = file->Save(widen(path).c_str(), TRUE);
                file->Release();
            }
            link->Release();
        }
        if (SUCCEEDED(init)) CoUninitialize();
        if (FAILED(hr)) {
            error = "can't write " + path + " (" + describe(DWORD(hr)) + ")";
            return false;
        }
        return true;
    }

    std::string steamFolder()
    {
        const struct { HKEY root; const wchar_t* key; const wchar_t* value; } places[] = {
            {HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"},
            {HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"},
            {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath"},
        };
        for (const auto& p : places) {
            wchar_t buf[MAX_PATH * 2];
            DWORD size = sizeof buf;
            if (RegGetValueW(p.root, p.key, p.value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) continue;
            std::string folder = narrow(buf, wcslen(buf));
            std::replace(folder.begin(), folder.end(), '/', '\\'); // SteamPath has forward slashes
            if (!folder.empty()) return folder;
        }
        return {};
    }

    bool processRunning(const char* exeName)
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        const std::wstring want = widen(exeName);
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof entry;
        bool found = false;
        for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
            found = _wcsicmp(entry.szExeFile, want.c_str()) == 0;
        CloseHandle(snapshot);
        return found;
    }

    bool ownsConsole()
    {
        DWORD processes[2];
        return GetConsoleProcessList(processes, 2) == 1;
    }
#endif
}
