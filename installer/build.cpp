#include "build.h"

#include "crypto.h"
#include "platform.h"
#include "xex.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include <sys/stat.h>

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
                if (!line.empty() && line.back() == '\r') line.pop_back(); // a kit written with CRLF
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

        // argv with its output in `log` (installer/platform.h, spawn), waited for
        bool run(const std::vector<std::string>& argv, const std::string& libraryPath, const std::string& log,
                 std::string& error)
        {
            platform::Child child = 0;
            if (!platform::spawn(argv, libraryPath, log, child, error)) return false;
            size_t index = 0;
            bool ok = false;
            if (!platform::waitAny({child}, index, ok) || !ok) {
                error = fs::path(argv[0]).filename().string() + " failed (log: " + log + "):" + tail(log, 15);
                return false;
            }
            return true;
        }

        // What differs between the two executables the game can be built from (docs/title-update.md).
        // The update's build has folders of its own, so both stay compiled and switching only links;
        // the program, bin/backbreaker, is whichever was linked last.
        struct Variant
        {
            bool update;
            const char* config;   // in <bundle>/config and, copied, <prefix>/config
            const char* switches;
            const char* xex;      // under <prefix>, what the import glue is made from
            const char* pe;
            const char* ppc;      // folders under <prefix>
            const char* obj;
            const char* share;    // under the kit's share/backbreaker-runtime: its layout and link arguments
        };
        const Variant kDisc{false, "backbreaker.toml", "backbreaker_switch_tables.toml", "/game/extracted/default.xex",
                            "/game/extracted/default.pe", "/ppc", "/obj", ""};
        const Variant kUpdate{true, "backbreaker_tu2.toml", "backbreaker_tu2_switch_tables.toml", "/game/extracted/tu2/default.xex",
                              "/game/extracted/tu2/default.pe", "/ppc-tu2", "/obj-tu2", "/tu2"};
        const char kUpdatePatch[] = "/game/extracted/tu2/default.xexp";

        std::string kitShare(const Bundle& b, const Variant& v = kDisc) { return b.kit + "/share/backbreaker-runtime" + v.share; }

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

        // What the bundle gives a variant's ppc/: bundlePpcInputs for the disc's, which the release
        // workflow compares between releases, and the same with the update's config for the update's
        std::string bundleInputs(const Bundle& b, const Variant& v)
        {
            std::error_code ec;
            std::string all;
            for (const std::string& f : {fs::exists(b.xenonRecompSource, ec) ? b.xenonRecompSource : b.xenonRecomp,
                                         b.ppcContext, b.config + "/" + v.config, b.config + "/" + v.switches,
                                         kitShare(b) + "/ppc-compile.args", b.kit + "/include/ppc_hooks.h"})
                all += fileHash(f) + "\n";
            for (const std::string& f : kPpcExtraFlags) all += f + "\n";
            return textHash(all);
        }

        // ppc/ depends on the bundle's part and the player's executable; the update's on its patch too
        std::string ppcInputs(const Bundle& b, const std::string& prefix, const Variant& v)
        {
            std::string all = bundleInputs(b, v) + "\n" + fileHash(prefix + "/game/extracted/default.xex") + "\n";
            if (v.update) all += fileHash(prefix + kUpdatePatch) + "\n";
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
#ifdef _WIN32
                std::error_code e;
                // In the file clock's own units (libc++ counts them in 128 bits)
                const long long when = (long long)fs::last_write_time(it->path(), e).time_since_epoch().count();
                lines.push_back(fs::relative(it->path(), b.kit).string() + " " + std::to_string(it->file_size(e)) + " " +
                                std::to_string(when));
#else
                struct stat st {};
                stat(it->path().c_str(), &st);
                lines.push_back(fs::relative(it->path(), b.kit).string() + " " + std::to_string(st.st_size) + " " +
                                std::to_string(st.st_mtim.tv_sec) + "." + std::to_string(st.st_mtim.tv_nsec));
#endif
            }
            std::sort(lines.begin(), lines.end());
            std::string all = ppc + "\n";
            for (const std::string& l : lines) all += l + "\n";
            return textHash(all);
        }

        std::vector<std::string> toolchainCompile(const Bundle& b)
        {
#ifdef _WIN32
            // llvm-mingw: clang finds its MinGW-w64 headers and libraries (UCRT) beside itself
            return {b.cxx, "--target=x86_64-w64-mingw32"};
#else
            return {b.cxx, "--target=x86_64-linux-gnu", "--sysroot=" + b.sysroot};
#endif
        }

        // XenonRecomp into <prefix>/ppc, with the config copied beside the game files: its paths
        // are relative to the config's folder (file_path = "../game/extracted/default.xex")
        // For the update, XenonRecomp first applies the update's patch to the disc's executable and
        // writes the patched one (the config's patch_file_path and patched_file_path), from which
        // the image the game loads is then taken.
        bool recompile(const Bundle& b, const std::string& prefix, const Variant& v, std::string& error)
        {
            const std::string ppcDir = prefix + v.ppc, objDir = prefix + v.obj;
            std::error_code ec;
            fs::create_directories(prefix + "/config", ec);
            fs::create_directories(ppcDir, ec);
            fs::create_directories(objDir, ec);
            for (const char* f : {v.config, v.switches})
                fs::copy_file(b.config + "/" + f, prefix + "/config/" + f, fs::copy_options::overwrite_existing, ec);
            for (auto& e : fs::directory_iterator(ppcDir, ec)) fs::remove(e.path(), ec);
            if (v.update) {
                // Made again from the disc's executable and the patch: one left by an earlier run
                // would be used as it is
                fs::remove(prefix + v.xex, ec);
                fs::remove(prefix + v.pe, ec);
            }
            if (!run({b.xenonRecomp, prefix + "/config/" + v.config, b.ppcContext}, b.toolchainLib,
                     objDir + "/xenonrecomp.log", error))
                return false;
            if (v.update) {
                std::ifstream in(prefix + v.xex, std::ios::binary);
                std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), pe;
                Xex patched;
                if (data.empty()) {
                    error = "the title update's patch didn't apply to your disc's executable (log: " + objDir +
                            "/xenonrecomp.log):" + tail(objDir + "/xenonrecomp.log", 5);
                    return false;
                }
                if (!patched.parse(std::move(data), error) || !patched.decryptToPe(pe, error)) return false;
                std::ofstream out(prefix + v.pe, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(pe.data()), std::streamsize(pe.size()));
                if (!out.flush()) {
                    error = "can't write " + prefix + v.pe;
                    return false;
                }
            }
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
            if (layout(ppcDir + "/ppc_config.h") != layout(kitShare(b, v) + "/ppc_config.h")) {
                error = "the executable's layout (ppc_config.h) isn't the one this runtime is built for";
                return false;
            }
            return true;
        }

        // Compiles every ppc/*.cpp into obj/, `jobs` at a time, progress by source size
        bool compilePpc(const Bundle& b, const std::string& prefix, const Variant& v, unsigned jobs, const Progress& progress,
                        double from, double to, std::string& error)
        {
            const std::string ppcDir = prefix + v.ppc, objDir = prefix + v.obj;
            struct Unit { std::string source, object; uint64_t size; };
            std::vector<Unit> units;
            std::error_code ec;
            for (auto& e : fs::directory_iterator(ppcDir, ec))
                if (e.path().extension() == ".cpp")
                    units.push_back({e.path().string(), objDir + "/" + e.path().stem().string() + ".o", uint64_t(e.file_size())});
            // Biggest first, so the longest compiles don't start last
            std::sort(units.begin(), units.end(), [](const Unit& a, const Unit& c) { return a.size > c.size; });
            uint64_t total = 0, done = 0;
            for (const Unit& u : units) total += u.size;
            for (auto& e : fs::directory_iterator(objDir, ec))
                if (e.path().filename().string().rfind("ppc_", 0) == 0) fs::remove(e.path(), ec);

            std::vector<std::string> base = toolchainCompile(b);
            for (const std::string& a : readArgs(kitShare(b) + "/ppc-compile.args", b.kit, ppcDir))
                base.push_back(a);
            base.insert(base.end(), kPpcExtraFlags.begin(), kPpcExtraFlags.end());

            jobs = std::min(jobs, platform::maxJobs());
            std::vector<platform::Child> running; // and which unit each one compiles
            std::vector<size_t> runningUnit;
            size_t next = 0, finished = 0;
            bool failed = false;
            while (finished < units.size()) {
                while (!failed && running.size() < jobs && next < units.size()) {
                    const Unit& u = units[next];
                    std::vector<std::string> argv = base;
                    argv.insert(argv.end(), {"-c", u.source, "-o", u.object});
                    platform::Child child = 0;
                    if (!platform::spawn(argv, b.toolchainLib, u.object + ".log", child, error)) {
                        failed = true;
                        break;
                    }
                    running.push_back(child);
                    runningUnit.push_back(next++);
                }
                size_t index = 0;
                bool ok = false;
                if (!platform::waitAny(running, index, ok)) break;
                const Unit& u = units[runningUnit[index]];
                running.erase(running.begin() + long(index));
                runningUnit.erase(runningUnit.begin() + long(index));
                finished++;
                if (!ok) {
                    if (!failed) error = "compiling " + fs::path(u.source).filename().string() + " failed (log: " + u.object + ".log):" + tail(u.object + ".log", 15);
                    failed = true;
                    for (platform::Child c : running) platform::terminate(c);
                    continue;
                }
                fs::remove(u.object + ".log", ec);
                done += u.size;
                progress(from + (to - from) * double(done) / double(total ? total : 1),
                         "compiling the game (" + std::to_string(finished) + " of " + std::to_string(units.size()) + " files)");
            }
            return !failed && finished == units.size();
        }

        bool link(const Bundle& b, const std::string& prefix, const Variant& v, std::string& error)
        {
            const std::string ppcDir = prefix + v.ppc, objDir = prefix + v.obj;
            std::error_code ec;
            fs::create_directories(prefix + "/bin", ec);
            // The import glue, from the player's executable (tools/gen_imports.py's output)
            std::vector<uint8_t> xexData, pe;
            {
                std::ifstream x(prefix + v.xex, std::ios::binary), p(prefix + v.pe, std::ios::binary);
                xexData.assign(std::istreambuf_iterator<char>(x), std::istreambuf_iterator<char>());
                pe.assign(std::istreambuf_iterator<char>(p), std::istreambuf_iterator<char>());
            }
            Xex xex;
            std::string cpp;
            std::vector<std::string> stats;
            if (!xex.parse(std::move(xexData), error) || !xex.generateImports(pe, kernelExports(), cpp, stats, error)) return false;
            if (!writeText(objDir + "/imports.cpp", cpp)) {
                error = "can't write " + objDir + "/imports.cpp";
                return false;
            }
            std::vector<std::string> argv = toolchainCompile(b);
            for (const std::string& a : readArgs(kitShare(b) + "/imports-compile.args", b.kit, ppcDir)) argv.push_back(a);
            argv.insert(argv.end(), {"-c", objDir + "/imports.cpp", "-o", objDir + "/imports.o"});
            if (!run(argv, b.toolchainLib, objDir + "/imports.log", error)) return false;

            std::vector<std::string> game{objDir + "/imports.o"};
            for (auto& e : fs::directory_iterator(objDir, ec))
                if (e.path().extension() == ".o" && e.path().filename().string().rfind("ppc_", 0) == 0) game.push_back(e.path().string());
            std::sort(game.begin() + 1, game.end());
            argv = toolchainCompile(b);
            argv.push_back("-fuse-ld=lld");
            for (const std::string& a : readArgs(kitShare(b, v) + "/game-link.args", b.kit, ppcDir, game)) argv.push_back(a);
            const std::string program = prefix + "/" + gameProgram();
            argv.insert(argv.end(), {"-o", program + ".tmp"});
            if (!run(argv, b.toolchainLib, objDir + "/link.log", error)) return false;
            fs::rename(program + ".tmp", program, ec);
            if (ec) {
                error = "can't write " + program + " (" + ec.message() + ")";
                return false;
            }
            return true;
        }
    }

    bool findBundle(const std::string& dir, Bundle& b, std::string& error)
    {
        std::string root = dir;
        if (root.empty()) {
            const std::string self = platform::selfPath();
            if (self.empty()) {
                error = "can't tell where this program is";
                return false;
            }
#ifdef _WIN32
            // The release zip: the program, and its bundle in the folder beside it
            root = (fs::path(self).parent_path() / "bundle").lexically_normal().string();
#else
            root = (fs::path(self).parent_path() / ".." / "lib" / "backbreaker-installer").lexically_normal().string();
#endif
        }
        std::error_code ec;
        root = fs::absolute(root, ec).lexically_normal().string();
        const std::string exe = platform::kExe;
        b.root = root;
        b.kit = root + "/kit";
        b.xenonRecomp = root + "/xenonrecomp/XenonRecomp" + exe;
        b.ppcContext = root + "/xenonrecomp/ppc_context.h";
        b.xenonRecompSource = root + "/xenonrecomp/SOURCE";
        b.config = root + "/config";
        b.cxx = root + "/toolchain/bin/clang++" + exe;
#ifdef _WIN32
        // llvm-mingw is its own sysroot, which clang finds by itself: include/ for every target,
        // x86_64-w64-mingw32/lib for this one
        b.sysroot = root + "/toolchain";
        const std::string sysrootHeader = b.sysroot + "/include/stdio.h";
#else
        b.sysroot = root + "/sysroot";
        const std::string sysrootHeader = b.sysroot + "/usr/include/stdio.h";
#endif
        b.toolchainLib = root + "/toolchain/lib";
        for (const std::string& f : {b.kit + "/share/backbreaker-runtime/game-link.args", b.xenonRecomp, b.ppcContext,
                                     b.config + "/backbreaker.toml", b.cxx, sysrootHeader}) {
            if (!fs::exists(f, ec)) {
                error = "the installer's bundle is incomplete or missing (" + f + "); build it with packaging/installer/build_installer.sh";
                return false;
            }
        }
        return true;
    }

    std::string bundlePpcInputs(const Bundle& b) { return bundleInputs(b, kDisc); }

    bool bundleHasTitleUpdate(const Bundle& b)
    {
        std::error_code ec;
        for (const std::string& f : {b.config + "/" + kUpdate.config, b.config + "/" + kUpdate.switches,
                                     kitShare(b, kUpdate) + "/game-link.args", kitShare(b, kUpdate) + "/ppc_config.h"})
            if (!fs::exists(f, ec)) return false;
        return true;
    }

    std::string dataHome()
    {
#ifdef _WIN32
        // Where the game keeps the player's files there too (runtime/paths.cpp)
        return platform::localAppData();
#else
        // Relative values are ignored, as the game does (runtime/paths.cpp), so both agree
        if (const char* data = std::getenv("XDG_DATA_HOME"); data && data[0] == '/') return data;
        const char* home = std::getenv("HOME");
        return std::string(home && *home ? home : ".") + "/.local/share";
#endif
    }

    std::string gameProgram() { return std::string("bin/backbreaker") + platform::kExe; }

    std::string defaultPrefix()
    {
        return dataHome() + "/backbreaker-recomp/install";
    }

    unsigned chooseJobs(std::string& why)
    {
        const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
        const uint64_t available = platform::availableMemory();
        const unsigned byMemory = available ? unsigned(std::max<uint64_t>(1, available / kBytesPerJob)) : cores;
        const unsigned jobs = std::min({cores, byMemory, platform::maxJobs()});
        why = std::to_string(jobs) + " at a time (" + std::to_string(cores) + " CPU threads, " +
              std::to_string(available >> 20) + " MB free memory at 1 GB a job)";
        return jobs;
    }

    BuildResult buildGame(const Bundle& b, const BuildOptions& o, const Progress& progress, double from, double to,
                          std::string& error)
    {
        const std::string& prefix = o.prefix;
        const Variant& v = o.titleUpdate ? kUpdate : kDisc;
        const Variant& other = o.titleUpdate ? kDisc : kUpdate;
        if (v.update && !bundleHasTitleUpdate(b)) {
            error = "this installer's bundle has no support for the title update";
            return BuildResult::Failed;
        }
        const std::string stampPath = prefix + v.obj + "/build.stamp";
        std::error_code ec;
        fs::create_directories(prefix + v.obj, ec);
        Stamp stamp = readStamp(stampPath);
        const std::string ppc = ppcInputs(b, prefix, v);
        bool objects = fs::exists(prefix + v.obj + "/ppc_func_mapping.o", ec);
        BuildResult result = BuildResult::UpToDate;
        if (o.recompile || stamp.ppc != ppc || !objects) {
            std::string why;
            const unsigned jobs = o.jobs ? o.jobs : chooseJobs(why);
            progress(from, v.update ? "patching the game's executable with the title update and translating its code (XenonRecomp)"
                                    : "translating the game's code (XenonRecomp)");
            if (!recompile(b, prefix, v, error)) return BuildResult::Failed;
            progress(from + (to - from) * 0.02, "compiling the game, " + (why.empty() ? std::to_string(jobs) + " at a time" : why));
            stamp.ppc.clear(); // a failed compile leaves no stamp that says ppc/ is current
            writeStamp(stampPath, stamp);
            if (!compilePpc(b, prefix, v, jobs, progress, from + (to - from) * 0.02, from + (to - from) * 0.96, error))
                return BuildResult::Failed;
            // The sources are no longer needed (XenonRecomp makes them again in seconds); the
            // headers stay, for compiling the import glue
            for (auto& e : fs::directory_iterator(prefix + v.ppc, ec))
                if (e.path().extension() == ".cpp") fs::remove(e.path(), ec);
            stamp.ppc = ppc;
            stamp.link.clear();
            writeStamp(stampPath, stamp);
            result = BuildResult::Rebuilt;
        }
        const std::string linkHash = linkInputs(b, ppc);
        if (o.relink || result == BuildResult::Rebuilt || stamp.link != linkHash || !fs::exists(prefix + "/" + gameProgram(), ec)) {
            progress(from + (to - from) * 0.97, "linking the game");
            // The program is this variant's from here on: the other's stamp must not say it is current
            const std::string otherStampPath = prefix + other.obj + "/build.stamp";
            if (fs::exists(otherStampPath, ec)) {
                Stamp otherStamp = readStamp(otherStampPath);
                otherStamp.link.clear();
                writeStamp(otherStampPath, otherStamp);
            }
            if (!link(b, prefix, v, error)) return BuildResult::Failed;
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
        bool busy = false;
        std::string why;
        if (!lock_.acquire(prefix + "/.lock", busy, why)) {
            error = busy ? "another installer is working in " + prefix + " right now"
                         : "can't write to " + prefix + " (" + why + ")";
            return false;
        }
        return true;
    }
}
