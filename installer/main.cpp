// backbreaker-installer (#138): from the player's own disc image to a built game, without Python,
// OpenSSL or a compiler on the host. `install` checks the image, copies the game's files, then
// translates, compiles and links the game with the bundled toolchain (installer/build.h); `play`
// relinks it when the runtime changed and starts it. The single steps are commands too. The
// image's path is never printed: messages say "the image" (as scripts/setup.sh does).
#include "build.h"
#include "crypto.h"
#include "disc.h"
#include "game_check.h"
#include "menu_entry.h"
#include "xex.h"

#include <cctype>
#include <cerrno>
#include <functional>
#include <memory>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <sys/statvfs.h>
#include <unistd.h>

using namespace installer;

namespace
{
    // Exit codes, stable so a launcher can map them to messages (RecompLauncher's exit_codes)
    enum Exit { kOk = 0, kFailed = 1, kUsage = 2, kCannotOpen = 3, kNotXbox360 = 4, kOtherGame = 5, kDamaged = 6,
                kNotInstalled = 7, kBuildFailed = 8 };

    // How the player runs this program again, for the messages that tell them: the AppImage's own
    // path (its runtime sets APPIMAGE), else this program's. Nothing named backbreaker-installer is
    // on their PATH. Quoted for a shell when the path needs it.
    std::string selfCommand()
    {
        std::string path;
        if (const char* a = std::getenv("APPIMAGE"); a && a[0] == '/') path = a;
        else {
            std::error_code ec;
            path = std::filesystem::read_symlink("/proc/self/exe", ec).string();
            if (path.empty()) return "backbreaker-installer";
        }
        bool plain = true;
        for (char c : path)
            if (!std::isalnum(static_cast<unsigned char>(c)) && !std::strchr("/._+-", c)) plain = false;
        if (plain) return path;
        std::string quoted = "'";
        for (char c : path) quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
        return quoted + "'";
    }

    // After the usage line, which names selfCommand()
    const char kUsageText[] =
        "  (no command)                      play, once the game is installed in the default folder\n"
        "  check <image>                     is it Backbreaker, the release this project supports?\n"
        "  list <image>                      the disc's files (tools/xdvdfs.py's listing)\n"
        "  extract <image> <dir> --all       every file, under <dir> (tools/xdvdfs.py --all)\n"
        "  extract <image> <dir> <file>...   those files, into <dir> (tools/xdvdfs.py)\n"
        "  decrypt <default.xex> <default.pe>         the program image (tools/xex_decrypt.py)\n"
        "  imports <default.xex> <default.pe> <out.cpp>  the import glue (tools/gen_imports.py)\n"
        "  icon <default.pe> <out.png>       the title's icon (tools/title_icon.py)\n"
        "  game-files <image> <game dir> [--progress]\n"
        "                                    check, then <game dir>/disc/ and extracted/default.{xex,pe}\n"
        "  install <image> [options]         check, copy the game's files, build the game\n"
        "  play [options] [-- <game options>]  build what changed (a new runtime relinks in seconds), start the game\n"
        "  rebuild [options]                 translate, compile and link again\n"
        "options: --prefix <dir>   where the game is installed (default: ~/.local/share/backbreaker-recomp/install)\n"
        "         --jobs <n>       parallel compiles (default: by CPU threads and free memory)\n"
        "         --progress       'progress <0-1> <step>' lines for a launcher\n"
        "         --bundle <dir>   the toolchain, runtime kit and XenonRecomp (default: beside this program)\n"
        "         --no-menu-entry  install: no Backbreaker in the applications menu (--progress never adds one)\n"
        "exit codes: 0 done, 1 failed, 2 usage, 3 the image can't be opened, 4 not an Xbox 360 game disc,\n"
        "            5 another game, 6 damaged image or unknown release, 7 not installed, 8 the build failed\n";

    void printUsage(FILE* to)
    {
        std::fprintf(to, "usage: %s <command> ...\n%s", selfCommand().c_str(), kUsageText);
    }

    // `play`, `rebuild`: what to run first
    std::string notInstalled(const std::string& prefix)
    {
        return "the game isn't installed in " + prefix + " (install it from your disc image first: " +
               selfCommand() + " install <your disc image>" + (prefix == defaultPrefix() ? "" : " --prefix " + prefix) + ")";
    }

    int fail(int code, const std::string& message)
    {
        std::fprintf(stderr, "backbreaker-installer: %s\n", message.c_str());
        return code;
    }

    bool readFile(const std::string& path, std::vector<uint8_t>& data, std::string& error)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            error = "can't read " + path;
            return false;
        }
        data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return true;
    }

    bool writeFile(const std::string& path, const void* data, size_t size, std::string& error)
    {
        std::error_code ec;
        const std::filesystem::path p(path);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(static_cast<const char*>(data), std::streamsize(size));
        if (!f.flush()) {
            error = "can't write " + path;
            return false;
        }
        return true;
    }

    // writeFile to <path>.tmp, then renamed: a desktop or the game never reads half a file
    bool replaceFile(const std::string& path, const void* data, size_t size, std::string& error)
    {
        const std::string tmp = path + ".tmp";
        std::error_code ec;
        if (!writeFile(tmp, data, size, error)) return false;
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            error = "can't write " + path;
            return false;
        }
        return true;
    }

    // <game dir>/icon.png, the title's own icon from the player's executable, for the game's window
    // (runtime/video/presenter.cpp) and the menu entry. Made once; nothing when the title has none.
    void ensureIcon(const std::string& gameDir)
    {
        const std::string icon = gameDir + "/icon.png";
        std::error_code ec;
        if (std::filesystem::exists(icon, ec)) return;
        std::vector<uint8_t> pe;
        std::string error;
        if (!readFile(gameDir + "/extracted/default.pe", pe, error)) return;
        const std::vector<uint8_t> png = titleIcon(pe);
        if (!png.empty()) replaceFile(icon, png.data(), png.size(), error);
    }

    // install from a terminal: Backbreaker in the applications menu, with the title's icon
    // (installer/menu_entry.h). Not with --progress: a launcher has its own entry for the game.
    void addMenuEntry(const std::string& prefix)
    {
        const std::string text = menuEntry(prefix + "/bin/backbreaker", prefix + "/game/icon.png");
        const std::string path = menuEntryPath();
        std::string error;
        if (text.empty())
            std::printf("[installer] no applications-menu entry: the folder's path has a \", `, $ or \\ in it\n");
        else if (!replaceFile(path, text.data(), text.size(), error))
            std::printf("[installer] no applications-menu entry: %s\n", error.c_str());
        else
            std::printf("[installer] added Backbreaker to your applications menu (%s)\n", path.c_str());
    }

    int exitFor(CheckStatus s)
    {
        switch (s) {
        case CheckStatus::Ok: return kOk;
        case CheckStatus::CannotOpen: return kCannotOpen;
        case CheckStatus::NotXbox360: return kNotXbox360;
        case CheckStatus::OtherGame: return kOtherGame;
        case CheckStatus::Damaged: return kDamaged;
        }
        return kFailed;
    }

    int openImage(const std::string& path, DiscImage& image)
    {
        std::string error;
        switch (image.open(path, error)) {
        case DiscImage::OpenResult::Ok: return kOk;
        case DiscImage::OpenResult::CannotOpen: return fail(kCannotOpen, error);
        case DiscImage::OpenResult::NotXbox360: return fail(kNotXbox360, error);
        }
        return kFailed;
    }

    void printListing(const DiscImage& image, const std::vector<DiscFile>& files)
    {
        std::printf("partition offset 0x%" PRIx64 "\n", image.partitionOffset());
        for (const DiscFile& f : files) std::printf("%12u %s\n", f.size, f.path.c_str());
    }

    int cmdCheck(const std::string& path)
    {
        DiscImage image;
        const CheckResult r = checkImage(path, image);
        if (r.status != CheckStatus::Ok) return fail(exitFor(r.status), r.message);
        std::printf("%s: title ID %08X, disc %u/%u, version %08X, default.xex SHA-256 %s, %zu files\n",
                    r.message.c_str(), r.info.titleId, r.info.discNumber, r.info.discCount, r.info.version,
                    r.xexSha256.c_str(), r.files.size());
        return kOk;
    }

    int cmdList(const std::string& path)
    {
        DiscImage image;
        if (int rc = openImage(path, image)) return rc;
        std::vector<DiscFile> files;
        std::string error;
        if (!image.list(files, error)) return fail(kDamaged, error);
        printListing(image, files);
        return kOk;
    }

    int cmdExtract(const std::string& path, const std::string& dir, const std::vector<std::string>& wanted)
    {
        DiscImage image;
        if (int rc = openImage(path, image)) return rc;
        std::vector<DiscFile> files;
        std::string error;
        if (!image.list(files, error)) return fail(kDamaged, error);
        printListing(image, files);
        const bool all = wanted.size() == 1 && wanted[0] == "--all";
        auto lower = [](std::string s) {
            for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        std::vector<std::string> want;
        for (const std::string& w : wanted) want.push_back(lower(w));
        for (const DiscFile& f : files) {
            std::string dest;
            if (all) {
                dest = dir + f.path; // f.path starts with '/'
            } else {
                const std::string rel = lower(f.path.substr(1));
                bool match = false;
                for (const std::string& w : want) match = match || rel == w;
                if (!match) continue;
                dest = dir + "/" + f.path.substr(f.path.rfind('/') + 1);
            }
            if (!image.extract(f, dest, error)) return fail(image.end(f) > image.imageSize() ? kDamaged : kFailed, error);
        }
        return kOk;
    }

    int cmdDecrypt(const std::string& xexPath, const std::string& pePath)
    {
        std::vector<uint8_t> data, pe;
        std::string error;
        Xex x;
        if (!readFile(xexPath, data, error)) return fail(kFailed, error);
        if (!x.parse(std::move(data), error) || !x.decryptToPe(pe, error)) return fail(kDamaged, error);
        if (!writeFile(pePath, pe.data(), pe.size(), error)) return fail(kFailed, error);
        std::printf("wrote %s (%#zx bytes)\n", pePath.c_str(), pe.size());
        return kOk;
    }

    int cmdImports(const std::string& xexPath, const std::string& pePath, const std::string& outPath)
    {
        std::vector<uint8_t> data, pe;
        std::string error, cpp;
        std::vector<std::string> stats;
        Xex x;
        if (!readFile(xexPath, data, error) || !readFile(pePath, pe, error)) return fail(kFailed, error);
        if (!x.parse(std::move(data), error) || !x.generateImports(pe, kernelExports(), cpp, stats, error))
            return fail(kDamaged, error);
        if (!writeFile(outPath, cpp.data(), cpp.size(), error)) return fail(kFailed, error);
        for (const std::string& s : stats) std::printf("%s\n", s.c_str());
        return kOk;
    }

    int cmdIcon(const std::string& pePath, const std::string& outPath)
    {
        std::vector<uint8_t> pe;
        std::string error;
        if (!readFile(pePath, pe, error)) return fail(kFailed, error);
        const std::vector<uint8_t> png = titleIcon(pe);
        if (png.empty()) return fail(kFailed, "no title image in the executable's XDBF resource");
        if (!writeFile(outPath, png.data(), png.size(), error)) return fail(kFailed, error);
        std::printf("title icon: %zu bytes\n", png.size());
        return kOk;
    }

    using ProgressFn = std::function<void(double, const std::string&)>;

    // --progress: every update as a "progress <0-1> <step>" line for a launcher; otherwise each
    // step once, for a terminal
    ProgressFn makeProgress(bool lines)
    {
        auto last = std::make_shared<std::string>();
        return [lines, last](double f, const std::string& step) {
            if (lines) std::printf("progress %.3f %s\n", f, step.c_str());
            else if (step != *last) std::printf("[installer] %s\n", step.c_str());
            else return;
            *last = step;
            std::fflush(stdout);
        };
    }

    // The game files a build needs, laid out as the checkout's game/ (docs/runtime.md, "Where files
    // live"): disc/ with every file of the disc, extracted/default.xex and extracted/default.pe.
    // Skipped when a complete copy of the same executable is already there (game/.complete).
    int copyGameFiles(const DiscImage& image, const CheckResult& r, const std::string& gameDir, const ProgressFn& progress,
                      double from, double to)
    {
        std::string error;
        const std::string complete = gameDir + "/.complete";
        std::vector<uint8_t> have;
        if (readFile(complete, have, error) && std::string(have.begin(), have.end()) == r.xexSha256 + "\n") {
            ensureIcon(gameDir); // an install from before the icon
            progress(to, "the game's files are already there");
            return kOk;
        }
        std::error_code ec;
        std::filesystem::remove(complete, ec);
        uint64_t total = 0, done = 0;
        for (const DiscFile& f : r.files) total += f.size;
        int lastPercent = -1;
        for (const DiscFile& f : r.files) {
            if (!image.extract(f, gameDir + "/disc" + f.path, error, [&](uint64_t n) {
                    done += n;
                    const int percent = int(done * 100 / (total ? total : 1));
                    if (percent != lastPercent) {
                        lastPercent = percent;
                        progress(from + (to - from) * 0.95 * double(done) / double(total ? total : 1), "copying the disc's files");
                    }
                }))
                return fail(kFailed, error);
        }
        progress(from + (to - from) * 0.96, "decrypting the game's executable");
        std::vector<uint8_t> data, pe;
        for (const DiscFile& f : r.files)
            if (f.path == "/default.xex" && !image.read(f, data, error)) return fail(kDamaged, error);
        Xex x;
        if (!x.parse(data, error) || !x.decryptToPe(pe, error)) return fail(kDamaged, error);
        if (!writeFile(gameDir + "/extracted/default.xex", data.data(), data.size(), error) ||
            !writeFile(gameDir + "/extracted/default.pe", pe.data(), pe.size(), error))
            return fail(kFailed, error);
        const std::string stamp = r.xexSha256 + "\n";
        if (!writeFile(complete, stamp.data(), stamp.size(), error)) return fail(kFailed, error);
        ensureIcon(gameDir);
        progress(to, "the game's files are ready");
        return kOk;
    }

    int cmdGameFiles(const std::string& path, const std::string& gameDir, bool progressLines)
    {
        const ProgressFn progress = makeProgress(progressLines);
        progress(0.0, "checking your disc image");
        DiscImage image;
        const CheckResult r = checkImage(path, image);
        if (r.status != CheckStatus::Ok) return fail(exitFor(r.status), r.message);
        progress(0.01, r.message);
        return copyGameFiles(image, r, gameDir, progress, 0.01, 1.0);
    }

    struct Options
    {
        std::string image, prefix, bundle;
        unsigned jobs = 0;
        bool progress = false;
        bool menuEntry = true; // install: --no-menu-entry
        std::vector<std::string> gameArgs; // after --
    };

    bool parseOptions(const std::vector<std::string>& a, bool wantImage, Options& o)
    {
        for (size_t i = 1; i < a.size(); i++) {
            const std::string& s = a[i];
            if (s == "--") {
                o.gameArgs.assign(a.begin() + long(i) + 1, a.end());
                break;
            }
            if (s == "--progress") o.progress = true;
            else if (s == "--no-menu-entry" && wantImage) o.menuEntry = false;
            else if ((s == "--prefix" || s == "--bundle" || s == "--jobs") && i + 1 < a.size()) {
                const std::string& v = a[++i];
                if (s == "--prefix") o.prefix = v;
                else if (s == "--bundle") o.bundle = v;
                else o.jobs = unsigned(std::strtoul(v.c_str(), nullptr, 10));
            } else if (wantImage && o.image.empty() && s.rfind("--", 0) != 0) o.image = s;
            else return false;
        }
        if (o.prefix.empty()) o.prefix = defaultPrefix();
        std::error_code ec;
        o.prefix = std::filesystem::absolute(o.prefix, ec).lexically_normal().string();
        return !wantImage || !o.image.empty();
    }

    int build(const Options& o, BuildOptions bo, const ProgressFn& progress, double from)
    {
        Bundle bundle;
        std::string error;
        if (!findBundle(o.bundle, bundle, error)) return fail(kFailed, error);
        bo.prefix = o.prefix;
        bo.jobs = o.jobs;
        if (buildGame(bundle, bo, progress, from, 1.0, error) == BuildResult::Failed) return fail(kBuildFailed, error);
        return kOk;
    }

    int cmdInstall(const Options& o)
    {
        const ProgressFn progress = makeProgress(o.progress);
        InstallLock lock;
        std::string error;
        if (!lock.acquire(o.prefix, error)) return fail(kFailed, error);
        // The disc's files, ppc/ while it compiles, the objects and the game: about 1.6 GB at most
        struct statvfs fsInfo {};
        if (statvfs(o.prefix.c_str(), &fsInfo) == 0) {
            const uint64_t free = uint64_t(fsInfo.f_bavail) * fsInfo.f_frsize;
            if (free < (2ull << 30))
                return fail(kFailed, "not enough free space where the game goes (" + std::to_string(free >> 20) +
                                         " MB free, 2048 MB needed)");
        }
        progress(0.0, "checking your disc image");
        DiscImage image;
        const CheckResult r = checkImage(o.image, image);
        if (r.status != CheckStatus::Ok) return fail(exitFor(r.status), r.message);
        progress(0.01, r.message);
        if (int rc = copyGameFiles(image, r, o.prefix + "/game", progress, 0.01, 0.12)) return rc;
        if (int rc = build(o, {}, progress, 0.12)) return rc;
        if (!o.progress) {
            std::printf("[installer] installed in %s\n", o.prefix.c_str());
            if (o.menuEntry) addMenuEntry(o.prefix);
            std::printf("[installer] start it with: %s play%s\n", selfCommand().c_str(),
                        o.prefix == defaultPrefix() ? "" : (" --prefix " + o.prefix).c_str());
            // The game finds its files from its own path (runtime/paths.cpp), so it needs no options
            std::printf("[installer] or start %s/bin/backbreaker itself, for example as a non-Steam game in Steam\n",
                        o.prefix.c_str());
        }
        return kOk;
    }

    int cmdRebuild(const Options& o)
    {
        if (!std::filesystem::exists(o.prefix + "/game/.complete"))
            return fail(kNotInstalled, notInstalled(o.prefix));
        InstallLock lock;
        std::string error;
        if (!lock.acquire(o.prefix, error)) return fail(kFailed, error);
        BuildOptions bo;
        bo.recompile = true;
        bo.relink = true;
        return build(o, bo, makeProgress(o.progress), 0.0);
    }

    // The launcher's start step: builds whatever a new bundle changed, then becomes the game
    int cmdPlay(const Options& o)
    {
        if (!std::filesystem::exists(o.prefix + "/game/.complete"))
            return fail(kNotInstalled, notInstalled(o.prefix));
        {
            InstallLock lock;
            std::string error;
            if (!lock.acquire(o.prefix, error)) return fail(kFailed, error);
            // Quiet when there is nothing to build; a relink or rebuild says so
            const ProgressFn report = makeProgress(o.progress);
            const ProgressFn quiet = [&](double f, const std::string& step) {
                if (step.rfind("the game is", 0) != 0) report(f, step);
            };
            if (int rc = build(o, {}, quiet, 0.0)) return rc;
        }
        ensureIcon(o.prefix + "/game"); // an install from before the icon
        const std::string game = o.prefix + "/bin/backbreaker";
        std::vector<std::string> args{game, "--data-dir", o.prefix + "/game"};
        // RecompLauncher's Big Picture mode is played from a couch or a Steam Deck: full screen
        // whatever settings.ini says (it sets RECOMPLAUNCHER_MODE for every game it starts)
        const char* mode = std::getenv("RECOMPLAUNCHER_MODE");
        if (mode && std::string(mode) == "bigpicture") args.push_back("--fullscreen");
        args.insert(args.end(), o.gameArgs.begin(), o.gameArgs.end());
        std::vector<char*> argv;
        for (std::string& s : args) argv.push_back(s.data());
        argv.push_back(nullptr);
        std::fflush(stdout);
        execv(game.c_str(), argv.data());
        return fail(kFailed, "can't start " + game + " (" + std::strerror(errno) + ")");
    }
}

int main(int argc, char** argv)
{
    std::vector<std::string> a(argv + 1, argv + argc);
    if (a.empty()) {
        // Double-clicked, or a shortcut with no launch options (Steam's non-Steam game): the game,
        // once it's installed where install puts it by default
        Options o;
        parseOptions({"play"}, false, o);
        if (std::filesystem::exists(o.prefix + "/game/.complete")) return cmdPlay(o);
        printUsage(stderr);
        return kUsage;
    }
    if (a[0] == "--help" || a[0] == "-h") {
        printUsage(stdout);
        return kOk;
    }
    const std::string& c = a[0];
    if (c == "check" && a.size() == 2) return cmdCheck(a[1]);
    if (c == "list" && a.size() == 2) return cmdList(a[1]);
    if (c == "extract" && a.size() >= 4) return cmdExtract(a[1], a[2], std::vector<std::string>(a.begin() + 3, a.end()));
    if (c == "decrypt" && a.size() == 3) return cmdDecrypt(a[1], a[2]);
    if (c == "imports" && a.size() == 4) return cmdImports(a[1], a[2], a[3]);
    if (c == "icon" && a.size() == 3) return cmdIcon(a[1], a[2]);
    if (c == "game-files" && (a.size() == 3 || (a.size() == 4 && a[3] == "--progress")))
        return cmdGameFiles(a[1], a[2], a.size() == 4);
    Options o;
    if (c == "install" && parseOptions(a, true, o)) return cmdInstall(o);
    if (c == "play" && parseOptions(a, false, o)) return cmdPlay(o);
    if (c == "rebuild" && parseOptions(a, false, o)) return cmdRebuild(o);
    printUsage(stderr);
    return kUsage;
}
