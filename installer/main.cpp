// backbreaker-installer: the installer's game-file steps (#138), without Python or OpenSSL.
// The full build (recompile, compile, link against the runtime kit) comes with the bundled
// toolchain; these are the steps before it. The image's path is never printed: messages say
// "the image" (as scripts/setup.sh does).
#include "crypto.h"
#include "disc.h"
#include "game_check.h"
#include "xex.h"

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace installer;

namespace
{
    // Exit codes, stable so a launcher can map them to messages (RecompLauncher's exit_codes)
    enum Exit { kOk = 0, kFailed = 1, kUsage = 2, kCannotOpen = 3, kNotXbox360 = 4, kOtherGame = 5, kDamaged = 6 };

    const char kUsageText[] =
        "usage: backbreaker-installer <command> ...\n"
        "  check <image>                     is it Backbreaker, the release this project supports?\n"
        "  list <image>                      the disc's files (tools/xdvdfs.py's listing)\n"
        "  extract <image> <dir> --all       every file, under <dir> (tools/xdvdfs.py --all)\n"
        "  extract <image> <dir> <file>...   those files, into <dir> (tools/xdvdfs.py)\n"
        "  decrypt <default.xex> <default.pe>         the program image (tools/xex_decrypt.py)\n"
        "  imports <default.xex> <default.pe> <out.cpp>  the import glue (tools/gen_imports.py)\n"
        "  icon <default.pe> <out.png>       the title's icon (tools/title_icon.py)\n"
        "  game-files <image> <game dir> [--progress]\n"
        "                                    check, then <game dir>/disc/ and extracted/default.{xex,pe}\n"
        "                                    (--progress: 'progress <0-1> <step>' lines for a launcher)\n"
        "exit codes: 0 done, 1 failed, 2 usage, 3 the image can't be opened, 4 not an Xbox 360 game disc,\n"
        "            5 another game, 6 damaged image or unknown release\n";

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

    // The game files a build needs, laid out as the checkout's game/ (docs/runtime.md, "Where files
    // live"): disc/ with every file of the disc, extracted/default.xex and extracted/default.pe
    int cmdGameFiles(const std::string& path, const std::string& gameDir, bool progressLines)
    {
        auto progress = [&](double f, const std::string& step) {
            if (progressLines) std::printf("progress %.3f %s\n", f, step.c_str());
            else std::printf("[installer] %s\n", step.c_str());
            std::fflush(stdout);
        };
        progress(0.0, "checking your disc image");
        DiscImage image;
        const CheckResult r = checkImage(path, image);
        if (r.status != CheckStatus::Ok) return fail(exitFor(r.status), r.message);
        progress(0.01, r.message);

        uint64_t total = 0, done = 0;
        for (const DiscFile& f : r.files) total += f.size;
        std::string error;
        int lastPercent = -1;
        for (const DiscFile& f : r.files) {
            if (!image.extract(f, gameDir + "/disc" + f.path, error, [&](uint64_t n) {
                    done += n;
                    const int percent = int(done * 100 / (total ? total : 1));
                    if (percent != lastPercent) {
                        lastPercent = percent;
                        progress(0.01 + 0.94 * double(done) / double(total ? total : 1), "copying the disc's files");
                    }
                }))
                return fail(kFailed, error);
        }

        progress(0.96, "decrypting the game's executable");
        std::vector<uint8_t> data, pe;
        for (const DiscFile& f : r.files)
            if (f.path == "/default.xex" && !image.read(f, data, error)) return fail(kDamaged, error);
        Xex x;
        if (!x.parse(data, error) || !x.decryptToPe(pe, error)) return fail(kDamaged, error);
        if (!writeFile(gameDir + "/extracted/default.xex", data.data(), data.size(), error) ||
            !writeFile(gameDir + "/extracted/default.pe", pe.data(), pe.size(), error))
            return fail(kFailed, error);
        progress(1.0, "game files ready");
        return kOk;
    }
}

int main(int argc, char** argv)
{
    std::vector<std::string> a(argv + 1, argv + argc);
    if (a.empty() || a[0] == "--help" || a[0] == "-h") {
        std::fputs(kUsageText, a.empty() ? stderr : stdout);
        return a.empty() ? kUsage : kOk;
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
    std::fputs(kUsageText, stderr);
    return kUsage;
}
