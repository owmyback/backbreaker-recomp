#include "build.h"

#include "crypto.h"
#include "xex.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace fs = std::filesystem;

namespace installer
{
    namespace
    {
        // What the installer changes from the kit's compile arguments for ppc/ (#138): no debug info.
        // Crash backtraces use the exported symbol names (-rdynamic), not DWARF, and without it the
        // biggest file compiles in 11 s instead of 13 s and peaks at 720 MB instead of 890 MB.
        const std::vector<std::string> kPpcExtraFlags = {"-g0"};
        constexpr uint64_t kBytesPerJob = 1ull << 30;

        std::string readText(const std::string& path)
        {
            std::ifstream f(path, std::ios::binary);
            std::stringstream s;
            s << f.rdbuf();
            return s.str();
        }

        bool writeText(const std::string& path, const std::string& text)
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            f << text;
            return bool(f.flush());
        }

        std::string fileHash(const std::string& path)
        {
            std::ifstream f(path, std::ios::binary);
            if (!f) return "missing";
            Sha256 h;
            std::vector<char> buffer(1 << 20);
            while (f) {
                f.read(buffer.data(), std::streamsize(buffer.size()));
                h.update(buffer.data(), size_t(f.gcount()));
            }
            const auto d = h.finish();
            return toHex(d.data(), d.size());
        }

        std::string textHash(const std::string& text)
        {
            Sha256 h;
            h.update(text.data(), text.size());
            const auto d = h.finish();
            return toHex(d.data(), d.size());
        }

        // An .args file of the kit: one argument per line, @KIT@ and @PPC@ filled in, @GAME@ replaced
        // by `game` (CMakeLists.txt, "The runtime kit")
        std::vector<std::string> readArgs(const std::string& file, const std::string& kit, const std::string& ppc,
                                          const std::vector<std::string>& game = {})
        {
            std::vector<std::string> args;
            std::istringstream in(readText(file));
            std::string line;
            auto replace = [](std::string s, const std::string& from, const std::string& to) {
                for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
                    s.replace(at, from.size(), to);
                return s;
            };
            while (std::getline(in, line)) {
                if (line.empty()) continue;
                if (line == "@GAME@") {
                    args.insert(args.end(), game.begin(), game.end());
                    continue;
                }
                args.push_back(replace(replace(line, "@KIT@", kit), "@PPC@", ppc));
            }
            return args;
        }

        // The last lines of a log, for an error message
        std::string tail(const std::string& path, size_t lines)
        {
            const std::string text = readText(path);
            size_t at = text.size(), n = 0;
            while (at > 0 && n <= lines) {
                at = text.rfind('\n', at - 1);
                if (at == std::string::npos) {
                    at = 0;
                    break;
                }
                n++;
            }
            return text.substr(at);
        }

        // Starts argv with the toolchain's libraries first on the library path, its stdout and stderr
        // going to `log`. The environment is otherwise the caller's.
        pid_t spawn(const std::vector<std::string>& argv, const std::string& libraryPath, const std::string& log,
                    std::string& error)
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
                return -1;
            }
            return pid;
        }

        bool succeeded(int status) { return WIFEXITED(status) && WEXITSTATUS(status) == 0; }

        bool run(const std::vector<std::string>& argv, const std::string& libraryPath, const std::string& log,
                 std::string& error)
        {
            const pid_t pid = spawn(argv, libraryPath, log, error);
            if (pid < 0) return false;
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            if (!succeeded(status)) {
                error = fs::path(argv[0]).filename().string() + " failed (log: " + log + "):" + tail(log, 15);
                return false;
            }
            return true;
        }

        struct Stamp
        {
            std::string ppc, link;
        };

        Stamp readStamp(const std::string& path)
        {
            Stamp s;
            std::istringstream in(readText(path));
            std::string key, value;
            while (in >> key >> value) {
                if (key == "ppc") s.ppc = value;
                else if (key == "link") s.link = value;
            }
            return s;
        }

        void writeStamp(const std::string& path, const Stamp& s)
        {
            writeText(path + ".tmp", "ppc " + s.ppc + "\nlink " + s.link + "\n");
            std::error_code ec;
            fs::rename(path + ".tmp", path, ec);
        }

        // ppc/ depends on: XenonRecomp and its header, the config, the executable, and how it is
        // compiled (the kit's arguments and force-included header, and the installer's own flags)
        std::string ppcInputs(const Bundle& b, const std::string& prefix)
        {
            std::string all;
            for (const std::string& f : {b.xenonRecomp, b.ppcContext, b.config + "/backbreaker.toml",
                                         b.config + "/backbreaker_switch_tables.toml",
                                         b.kit + "/share/backbreaker-runtime/ppc-compile.args",
                                         b.kit + "/include/ppc_hooks.h", prefix + "/game/extracted/default.xex"})
                all += fileHash(f) + "\n";
            for (const std::string& f : kPpcExtraFlags) all += f + "\n";
            return textHash(all);
        }

        // The link depends on every file of the kit (by name, size and time: a new bundle has new
        // ones) and on ppc/. Not on where the bundle is: an AppImage is mounted somewhere new each run.
        std::string linkInputs(const Bundle& b, const std::string& ppc)
        {
            std::vector<std::string> lines;
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(b.kit, ec); !ec && it != fs::recursive_directory_iterator();
                 it.increment(ec)) {
                if (!it->is_regular_file()) continue;
                struct stat st {};
                stat(it->path().c_str(), &st);
                lines.push_back(fs::relative(it->path(), b.kit).string() + " " + std::to_string(st.st_size) + " " +
                                std::to_string(st.st_mtim.tv_sec) + "." + std::to_string(st.st_mtim.tv_nsec));
            }
            std::sort(lines.begin(), lines.end());
            std::string all = ppc + "\n";
            for (const std::string& l : lines) all += l + "\n";
            return textHash(all);
        }

        std::vector<std::string> toolchainCompile(const Bundle& b)
        {
            return {b.cxx, "--target=x86_64-linux-gnu", "--sysroot=" + b.sysroot};
        }

        // XenonRecomp into <prefix>/ppc, with the config copied beside the game files: its paths
        // are relative to the config's folder (file_path = "../game/extracted/default.xex")
        bool recompile(const Bundle& b, const std::string& prefix, std::string& error)
        {
            std::error_code ec;
            fs::create_directories(prefix + "/config", ec);
            fs::create_directories(prefix + "/ppc", ec);
            fs::create_directories(prefix + "/obj", ec);
            for (const char* f : {"backbreaker.toml", "backbreaker_switch_tables.toml"})
                fs::copy_file(b.config + "/" + f, prefix + "/config/" + f, fs::copy_options::overwrite_existing, ec);
            for (auto& e : fs::directory_iterator(prefix + "/ppc", ec)) fs::remove(e.path(), ec);
            if (!run({b.xenonRecomp, prefix + "/config/backbreaker.toml", b.ppcContext}, b.toolchainLib,
                     prefix + "/obj/xenonrecomp.log", error))
                return false;
            // The layout check of CMakeLists.txt and link_game.sh
            auto layout = [](const std::string& path) {
                std::vector<std::string> v;
                std::istringstream in(readText(path));
                std::string line;
                while (std::getline(in, line))
                    if (line.rfind("#define PPC_IMAGE_", 0) == 0 || line.rfind("#define PPC_CODE_", 0) == 0) {
                        std::string squeezed; // runs of blanks as one space
                        bool blank = false;
                        for (char c : line) {
                            if (c == ' ' || c == '\t') {
                                blank = true;
                                continue;
                            }
                            if (blank && !squeezed.empty()) squeezed += ' ';
                            blank = false;
                            squeezed += c;
                        }
                        v.push_back(squeezed);
                    }
                std::sort(v.begin(), v.end());
                return v;
            };
            if (layout(prefix + "/ppc/ppc_config.h") != layout(b.kit + "/share/backbreaker-runtime/ppc_config.h")) {
                error = "the executable's layout (ppc/ppc_config.h) isn't the one this runtime is built for";
                return false;
            }
            return true;
        }

        // Compiles every ppc/*.cpp into obj/, `jobs` at a time, progress by source size
        bool compilePpc(const Bundle& b, const std::string& prefix, unsigned jobs, const Progress& progress, double from,
                        double to, std::string& error)
        {
            struct Unit { std::string source, object; uint64_t size; };
            std::vector<Unit> units;
            std::error_code ec;
            for (auto& e : fs::directory_iterator(prefix + "/ppc", ec))
                if (e.path().extension() == ".cpp")
                    units.push_back({e.path().string(), prefix + "/obj/" + e.path().stem().string() + ".o", uint64_t(e.file_size())});
            // Biggest first, so the longest compiles don't start last
            std::sort(units.begin(), units.end(), [](const Unit& a, const Unit& c) { return a.size > c.size; });
            uint64_t total = 0, done = 0;
            for (const Unit& u : units) total += u.size;
            for (auto& e : fs::directory_iterator(prefix + "/obj", ec))
                if (e.path().filename().string().rfind("ppc_", 0) == 0) fs::remove(e.path(), ec);

            std::vector<std::string> base = toolchainCompile(b);
            for (const std::string& a : readArgs(b.kit + "/share/backbreaker-runtime/ppc-compile.args", b.kit, prefix + "/ppc"))
                base.push_back(a);
            base.insert(base.end(), kPpcExtraFlags.begin(), kPpcExtraFlags.end());

            std::map<pid_t, size_t> running;
            size_t next = 0, finished = 0;
            bool failed = false;
            while (finished < units.size()) {
                while (!failed && running.size() < jobs && next < units.size()) {
                    const Unit& u = units[next];
                    std::vector<std::string> argv = base;
                    argv.insert(argv.end(), {"-c", u.source, "-o", u.object});
                    const pid_t pid = spawn(argv, b.toolchainLib, u.object + ".log", error);
                    if (pid < 0) {
                        failed = true;
                        break;
                    }
                    running[pid] = next++;
                }
                if (running.empty()) break;
                int status = 0;
                const pid_t pid = waitpid(-1, &status, 0);
                if (pid < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                auto it = running.find(pid);
                if (it == running.end()) continue;
                const Unit& u = units[it->second];
                running.erase(it);
                finished++;
                if (!succeeded(status)) {
                    if (!failed) error = "compiling " + fs::path(u.source).filename().string() + " failed (log: " + u.object + ".log):" + tail(u.object + ".log", 15);
                    failed = true;
                    for (auto& [p, i] : running) kill(p, SIGTERM);
                    continue;
                }
                fs::remove(u.object + ".log", ec);
                done += u.size;
                progress(from + (to - from) * double(done) / double(total ? total : 1),
                         "compiling the game (" + std::to_string(finished) + " of " + std::to_string(units.size()) + " files)");
            }
            return !failed && finished == units.size();
        }

        bool link(const Bundle& b, const std::string& prefix, std::string& error)
        {
            const std::string share = b.kit + "/share/backbreaker-runtime";
            std::error_code ec;
            fs::create_directories(prefix + "/bin", ec);
            // The import glue, from the player's executable (tools/gen_imports.py's output)
            std::vector<uint8_t> xexData, pe;
            {
                std::ifstream x(prefix + "/game/extracted/default.xex", std::ios::binary), p(prefix + "/game/extracted/default.pe", std::ios::binary);
                xexData.assign(std::istreambuf_iterator<char>(x), std::istreambuf_iterator<char>());
                pe.assign(std::istreambuf_iterator<char>(p), std::istreambuf_iterator<char>());
            }
            Xex xex;
            std::string cpp;
            std::vector<std::string> stats;
            if (!xex.parse(std::move(xexData), error) || !xex.generateImports(pe, kernelExports(), cpp, stats, error)) return false;
            if (!writeText(prefix + "/obj/imports.cpp", cpp)) {
                error = "can't write " + prefix + "/obj/imports.cpp";
                return false;
            }
            std::vector<std::string> argv = toolchainCompile(b);
            for (const std::string& a : readArgs(share + "/imports-compile.args", b.kit, prefix + "/ppc")) argv.push_back(a);
            argv.insert(argv.end(), {"-c", prefix + "/obj/imports.cpp", "-o", prefix + "/obj/imports.o"});
            if (!run(argv, b.toolchainLib, prefix + "/obj/imports.log", error)) return false;

            std::vector<std::string> game{prefix + "/obj/imports.o"};
            for (auto& e : fs::directory_iterator(prefix + "/obj", ec))
                if (e.path().extension() == ".o" && e.path().filename().string().rfind("ppc_", 0) == 0) game.push_back(e.path().string());
            std::sort(game.begin() + 1, game.end());
            argv = toolchainCompile(b);
            argv.push_back("-fuse-ld=lld");
            for (const std::string& a : readArgs(share + "/game-link.args", b.kit, prefix + "/ppc", game)) argv.push_back(a);
            argv.insert(argv.end(), {"-o", prefix + "/bin/backbreaker.tmp"});
            if (!run(argv, b.toolchainLib, prefix + "/obj/link.log", error)) return false;
            fs::rename(prefix + "/bin/backbreaker.tmp", prefix + "/bin/backbreaker", ec);
            if (ec) {
                error = "can't write " + prefix + "/bin/backbreaker (" + ec.message() + ")";
                return false;
            }
            return true;
        }
    }

    bool findBundle(const std::string& dir, Bundle& b, std::string& error)
    {
        std::string root = dir;
        if (root.empty()) {
            char self[PATH_MAX];
            const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
            if (n <= 0) {
                error = "can't tell where this program is (/proc/self/exe)";
                return false;
            }
            self[n] = 0;
            root = (fs::path(self).parent_path() / ".." / "lib" / "backbreaker-installer").lexically_normal().string();
        }
        std::error_code ec;
        root = fs::absolute(root, ec).lexically_normal().string();
        b.root = root;
        b.kit = root + "/kit";
        b.xenonRecomp = root + "/xenonrecomp/XenonRecomp";
        b.ppcContext = root + "/xenonrecomp/ppc_context.h";
        b.config = root + "/config";
        b.cxx = root + "/toolchain/bin/clang++";
        b.sysroot = root + "/sysroot";
        b.toolchainLib = root + "/toolchain/lib";
        for (const std::string& f : {b.kit + "/share/backbreaker-runtime/game-link.args", b.xenonRecomp, b.ppcContext,
                                     b.config + "/backbreaker.toml", b.cxx, b.sysroot + "/usr/include/stdio.h"}) {
            if (!fs::exists(f, ec)) {
                error = "the installer's bundle is incomplete or missing (" + f + "); build it with packaging/installer/build_installer.sh";
                return false;
            }
        }
        return true;
    }

    std::string dataHome()
    {
        // Relative values are ignored, as the game does (runtime/paths.cpp), so both agree
        if (const char* data = std::getenv("XDG_DATA_HOME"); data && data[0] == '/') return data;
        const char* home = std::getenv("HOME");
        return std::string(home && *home ? home : ".") + "/.local/share";
    }

    std::string defaultPrefix()
    {
        return dataHome() + "/backbreaker-recomp/install";
    }

    unsigned chooseJobs(std::string& why)
    {
        const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
        uint64_t available = 0;
        std::ifstream meminfo("/proc/meminfo");
        std::string key;
        uint64_t value = 0;
        std::string unit;
        while (meminfo >> key >> value >> unit)
            if (key == "MemAvailable:") available = value * 1024;
        const unsigned byMemory = available ? unsigned(std::max<uint64_t>(1, available / kBytesPerJob)) : cores;
        const unsigned jobs = std::min(cores, byMemory);
        why = std::to_string(jobs) + " at a time (" + std::to_string(cores) + " CPU threads, " +
              std::to_string(available >> 20) + " MB free memory at 1 GB a job)";
        return jobs;
    }

    BuildResult buildGame(const Bundle& b, const BuildOptions& o, const Progress& progress, double from, double to,
                          std::string& error)
    {
        const std::string& prefix = o.prefix;
        const std::string stampPath = prefix + "/obj/build.stamp";
        std::error_code ec;
        fs::create_directories(prefix + "/obj", ec);
        Stamp stamp = readStamp(stampPath);
        const std::string ppc = ppcInputs(b, prefix);
        bool objects = fs::exists(prefix + "/obj/ppc_func_mapping.o", ec);
        BuildResult result = BuildResult::UpToDate;
        if (o.recompile || stamp.ppc != ppc || !objects) {
            std::string why;
            const unsigned jobs = o.jobs ? o.jobs : chooseJobs(why);
            progress(from, "translating the game's code (XenonRecomp)");
            if (!recompile(b, prefix, error)) return BuildResult::Failed;
            progress(from + (to - from) * 0.02, "compiling the game, " + (why.empty() ? std::to_string(jobs) + " at a time" : why));
            stamp.ppc.clear(); // a failed compile leaves no stamp that says ppc/ is current
            writeStamp(stampPath, stamp);
            if (!compilePpc(b, prefix, jobs, progress, from + (to - from) * 0.02, from + (to - from) * 0.96, error))
                return BuildResult::Failed;
            // The sources are no longer needed (XenonRecomp makes them again in seconds); the
            // headers stay, for compiling the import glue
            for (auto& e : fs::directory_iterator(prefix + "/ppc", ec))
                if (e.path().extension() == ".cpp") fs::remove(e.path(), ec);
            stamp.ppc = ppc;
            stamp.link.clear();
            writeStamp(stampPath, stamp);
            result = BuildResult::Rebuilt;
        }
        const std::string linkHash = linkInputs(b, ppc);
        if (o.relink || result == BuildResult::Rebuilt || stamp.link != linkHash || !fs::exists(prefix + "/bin/backbreaker", ec)) {
            progress(from + (to - from) * 0.97, "linking the game");
            if (!link(b, prefix, error)) return BuildResult::Failed;
            stamp.link = linkHash;
            writeStamp(stampPath, stamp);
            if (result == BuildResult::UpToDate) result = BuildResult::Relinked;
        }
        progress(to, result == BuildResult::UpToDate ? "the game is up to date" : "the game is built");
        return result;
    }

    bool InstallLock::acquire(const std::string& prefix, std::string& error)
    {
        std::error_code ec;
        fs::create_directories(prefix, ec);
        fd_ = open((prefix + "/.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd_ < 0 || flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            error = fd_ < 0 ? "can't write to " + prefix + " (" + std::strerror(errno) + ")"
                            : "another installer is working in " + prefix + " right now";
            return false;
        }
        return true;
    }

    InstallLock::~InstallLock()
    {
        if (fd_ >= 0) close(fd_);
    }
}
