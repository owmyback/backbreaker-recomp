// The run's log as a file (#146): see log_file.h.
#include "log_file.h"
#include "log.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace log_file
{
    namespace
    {
        constexpr off_t kMaxBytes = off_t(256) << 20;
        constexpr int kRuns = 3; // this run's file and the two before it

        std::atomic<bool> g_started{false};
        std::atomic<int> g_console{-1};       // what stderr was; -1: nothing to copy to
        int g_read = -1;                      // the log file again, read-only, for the copies
        std::atomic<off_t> g_copied{0};       // how much of the file the console has had
        std::atomic<int> g_copying{0};        // copies under way (drain() lets them finish first)
        std::atomic<bool> g_capped{false};
        static_assert(std::atomic<off_t>::is_always_lock_free && std::atomic<int>::is_always_lock_free,
                      "drain() runs in a signal handler");

        // write() until done; false if the descriptor can't take it (EPIPE from a closed pipe)
        bool writeAll(int fd, const char* p, size_t n)
        {
            while (n)
            {
                ssize_t w = write(fd, p, n);
                if (w < 0)
                {
                    if (errno == EINTR) continue;
                    return false;
                }
                p += w;
                n -= size_t(w);
            }
            return true;
        }

        // Copies the file's bytes the console hasn't had. A caller claims [from, end) before it copies,
        // so the thread and drain() never send the same bytes twice. Async-signal-safe.
        void copyNew()
        {
            const int console = g_console.load();
            if (console < 0 || g_read < 0) return;
            g_copying.fetch_add(1); // before the claim, so drain() never sees a claimed range unwritten and goes ahead
            struct stat st;
            if (fstat(g_read, &st) != 0)
            {
                g_copying.fetch_sub(1);
                return;
            }
            const off_t end = st.st_size;
            off_t from = g_copied.load();
            do
            {
                if (from >= end)
                {
                    g_copying.fetch_sub(1);
                    return;
                }
            } while (!g_copied.compare_exchange_weak(from, end));
            char buf[16384];
            while (from < end)
            {
                ssize_t n = pread(g_read, buf, size_t(std::min<off_t>(off_t(sizeof buf), end - from)), from);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                if (!writeAll(console, buf, size_t(n)))
                {
                    g_console.store(-1); // the console went away: keep the file, stop copying
                    break;
                }
                from += n;
            }
            g_copying.fetch_sub(1);
        }

        // At kMaxBytes: a last line in the file, then stderr goes to the console (or nowhere) instead
        void capIfFull()
        {
            struct stat st;
            if (g_capped.load() || fstat(g_read, &st) != 0 || st.st_size < kMaxBytes) return;
            g_capped.store(true);
            static const char note[] = "[rt] WARNING: [log] the log file reached 256 MB: the rest of this run goes only to the console\n";
            writeAll(STDERR_FILENO, note, sizeof note - 1);
            copyNew();
            int to = g_console.load();
            if (to < 0) to = open("/dev/null", O_WRONLY | O_CLOEXEC);
            if (to >= 0) dup2(to, STDERR_FILENO); // the file stops here; copyNew() still sends what raced in
        }

        void mirror()
        {
            const timespec pause{ 0, 20 * 1000 * 1000 };
            for (;;)
            {
                copyNew();
                capIfFull();
                nanosleep(&pause, nullptr);
            }
        }

        // backbreaker.1.log -> .2.log, backbreaker.log -> .1.log
        void rotate(const std::filesystem::path& dir)
        {
            std::error_code ec;
            for (int i = kRuns - 1; i > 0; i--)
            {
                std::filesystem::path from = dir / (i == 1 ? std::string("backbreaker.log") : "backbreaker." + std::to_string(i - 1) + ".log");
                std::filesystem::rename(from, dir / ("backbreaker." + std::to_string(i) + ".log"), ec);
            }
        }
    }

    std::string start(const std::string& dir)
    {
        if (g_started.load()) return {};
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        rotate(dir);
        const std::string path = (std::filesystem::path(dir) / "backbreaker.log").string();
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
        if (fd < 0)
        {
            warn("[log] cannot write %s: %s - this run's log goes to the console only", path.c_str(), std::strerror(errno));
            return {};
        }
        g_read = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (g_read < 0)
        {
            warn("[log] cannot read back %s: %s - this run's log goes to the console only", path.c_str(), std::strerror(errno));
            close(fd);
            return {};
        }
        std::signal(SIGPIPE, SIG_IGN);
        std::fflush(stderr);
        g_console.store(fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3)); // -1 if the process had no stderr
        dup2(fd, STDERR_FILENO);
        close(fd);
        g_started.store(true);
        std::thread(mirror).detach();
        std::atexit(drain);
        return path;
    }

    void drain()
    {
        if (!g_started.load()) return;
        // A copy the thread has under way goes out first, so the console keeps the file's order.
        // Bounded: the copy can't finish if this is the thread's own crash.
        const timespec pause{ 0, 1000 * 1000 };
        for (int i = 0; i < 50 && g_copying.load() > 0; i++)
            nanosleep(&pause, nullptr);
        copyNew();
    }
}
