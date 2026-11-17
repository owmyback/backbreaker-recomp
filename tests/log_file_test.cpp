// Checks runtime/log_file.cpp (issue #146): stderr into a file, copied on to what stderr was. No game
// files, no generated headers:
//   clang++ -std=c++20 -Wall -Wextra -O2 -pthread tests/log_file_test.cpp runtime/log_file.cpp -o build/log_file_test
//
// Each case runs in a child process (start() takes over the process's stderr), with stderr on a pipe
// the parent reads, as a terminal or Steam would. The children end with drain() and _exit, the way
// every exit path of the game does (main.cpp, printExitSummary), so the last lines must still arrive.
#include "../runtime/log_file.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

static int failures = 0, checks = 0;

static void check(bool ok, const char* what)
{
    checks++;
    if (!ok)
    {
        failures++;
        printf("FAIL %s\n", what);
    }
}

static std::string readFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// Child: start the log in dir, write `lines` lines through stdio and write(2), drain, _exit(0).
// With closeConsole, the parent closes its end of the pipe first (a console that went away).
struct Run
{
    std::string console; // what came through the pipe
    int status = -1;
};

static Run run(const std::string& dir, int tag, int lines, bool closeConsole)
{
    int fds[2];
    if (pipe(fds) != 0) { perror("pipe"); exit(2); }
    if (closeConsole) close(fds[0]);
    pid_t pid = fork();
    if (pid == 0)
    {
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        if (!closeConsole) close(fds[0]);
        if (log_file::start(dir).empty()) _exit(3);
        for (int i = 0; i < lines; i++)
        {
            if (i % 2) std::fprintf(stderr, "run %d line %d (stdio)\n", tag, i);
            else
            {
                char b[64];
                int n = std::snprintf(b, sizeof(b), "run %d line %d (write)\n", tag, i);
                if (write(STDERR_FILENO, b, size_t(n)) != n) _exit(4);
            }
        }
        std::fprintf(stderr, "run %d last line\n", tag);
        log_file::drain();
        _exit(0); // no atexit handlers, no stdio flush: drain() alone must have sent everything
    }
    close(fds[1]);
    Run r;
    if (!closeConsole)
    {
        char buf[65536];
        ssize_t n;
        while ((n = read(fds[0], buf, sizeof(buf))) > 0) r.console.append(buf, size_t(n));
        close(fds[0]);
    }
    waitpid(pid, &r.status, 0);
    return r;
}

int main()
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("log_file_test." + std::to_string(getpid()));
    fs::remove_all(dir);

    // Three runs: each console gets exactly the file, and the files rotate
    Run r1 = run(dir.string(), 1, 3000, false);
    check(WIFEXITED(r1.status) && WEXITSTATUS(r1.status) == 0, "run 1 exits 0");
    check(r1.console == readFile(dir / "backbreaker.log"), "run 1: console == file");
    check(r1.console.find("run 1 last line\n") != std::string::npos, "run 1: last line reached the console");
    Run r2 = run(dir.string(), 2, 10, false);
    Run r3 = run(dir.string(), 3, 10, false);
    check(r3.console == readFile(dir / "backbreaker.log"), "run 3 in backbreaker.log");
    check(r2.console == readFile(dir / "backbreaker.1.log"), "run 2 in backbreaker.1.log");
    check(r1.console == readFile(dir / "backbreaker.2.log"), "run 1 in backbreaker.2.log");
    Run r4 = run(dir.string(), 4, 10, false);
    check(r4.console == readFile(dir / "backbreaker.log") && r2.console == readFile(dir / "backbreaker.2.log") &&
              !fs::exists(dir / "backbreaker.3.log"),
          "a fourth run drops the oldest");

    // A console that went away: the game goes on (no SIGPIPE) and the file has everything
    Run r5 = run(dir.string(), 5, 3000, true);
    check(WIFEXITED(r5.status) && WEXITSTATUS(r5.status) == 0, "closed console: the run still exits 0");
    const std::string f5 = readFile(dir / "backbreaker.log");
    check(f5.find("run 5 line 2999 (stdio)\n") != std::string::npos && f5.find("run 5 last line\n") != std::string::npos,
          "closed console: the file is complete");

    // A directory that can't be made: start() says so and leaves stderr alone
    {
        int fds[2];
        if (pipe(fds) != 0) { perror("pipe"); return 2; }
        pid_t pid = fork();
        if (pid == 0)
        {
            dup2(fds[1], STDERR_FILENO);
            close(fds[0]);
            close(fds[1]);
            std::string path = log_file::start("/proc/self/no-such-dir/logs");
            std::fprintf(stderr, "still here\n");
            _exit(path.empty() ? 0 : 1);
        }
        close(fds[1]);
        std::string out;
        char buf[4096];
        ssize_t n;
        while ((n = read(fds[0], buf, sizeof(buf))) > 0) out.append(buf, size_t(n));
        close(fds[0]);
        int status = -1;
        waitpid(pid, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "unwritable dir: start() returns empty");
        check(out.find("cannot write") != std::string::npos && out.find("still here\n") != std::string::npos,
              "unwritable dir: a warning, and stderr keeps working");
    }

    fs::remove_all(dir);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
