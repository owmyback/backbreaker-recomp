#pragma once
// The run's log as a file (#146). A run started from Steam (Gaming Mode, a launcher) has no terminal,
// so its log went nowhere. In the installed layout or with --user-dir (paths::logs()), main() calls
// start() and every byte written to stderr from then on - the runtime's log, crash reports, SDL's and
// the Vulkan driver's messages - goes into <user>/logs/backbreaker.log. The two runs before stay as
// backbreaker.1.log and backbreaker.2.log.
//
// The file comes first: stderr is the file itself (dup2), so a crash report is complete in it even
// when the process ends straight after. Whatever stderr was before (a terminal, a pipe, Steam's
// console) still gets everything: a thread copies the file's new bytes there every 20 ms, and drain()
// copies the rest just before the process ends. SIGPIPE is ignored from start() on, so a console that
// goes away (a closed pipe) stops the copies instead of ending the game. At 256 MB the file stops
// growing and the rest of the run goes to the console only.
#include <string>

namespace log_file
{
    // Starts the file in dir (created if missing) and returns its path; empty, with a warning and
    // stderr left as it was, if it can't be opened. Once, before other threads write to stderr.
    std::string start(const std::string& dir);

    // Copies to the console what it hasn't had yet. Async-signal-safe: the crash handler reaches it
    // (main.cpp, printExitSummary) before _exit. Does nothing without start().
    void drain();
}
