// Checks the installer's game-file steps (installer/, #138) without game files: AES-128 and
// SHA-256 against the standards' test vectors, and the disc reader, the XEX decoder, the import
// glue generator, the title icon and the image check on images and executables built here by hand.
// The same code is checked byte for byte against the Python tools on a real disc (see the PR).
// Build:
//   clang++ -std=c++20 -Wall -Wextra -O2 -I third_party/XenonRecomp/XenonUtils tests/installer_test.cpp \
//       installer/crypto.cpp installer/disc.cpp installer/xex.cpp installer/game_check.cpp -o build/installer_test
#include "../installer/crypto.h"
#include "../installer/disc.h"
#include "../installer/build.h"
#include "../installer/game_check.h"
#include "../installer/menu_entry.h"
#include "../installer/xex.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

using namespace installer;

static int failures = 0, checks = 0;

static void expect(bool ok, const std::string& what)
{
    checks++;
    if (!ok)
    {
        failures++;
        printf("FAIL %s\n", what.c_str());
    }
}

static std::vector<uint8_t> unhex(const std::string& s)
{
    std::vector<uint8_t> v(s.size() / 2);
    fromHex(s, v.data(), v.size());
    return v;
}

static std::string sha256(const std::string& s)
{
    Sha256 h;
    h.update(s.data(), s.size());
    const auto d = h.finish();
    return toHex(d.data(), d.size());
}

// Big-endian builder (XEX, XDBF) and little-endian pokes (XDVDFS)
struct Bytes
{
    std::vector<uint8_t> b;
    void u8(uint32_t v) { b.push_back(uint8_t(v)); }
    void u16(uint32_t v) { u8(v >> 8); u8(v); }
    void u32(uint32_t v) { u16(v >> 16); u16(v); }
    void u64(uint64_t v) { u32(uint32_t(v >> 32)); u32(uint32_t(v)); }
    void str(const std::string& s) { for (char c : s) u8(uint8_t(c)); }
    void pad(size_t to) { b.resize(to, 0); }
};

static void le16(std::vector<uint8_t>& d, size_t at, uint32_t v) { d[at] = uint8_t(v); d[at + 1] = uint8_t(v >> 8); }
static void le32(std::vector<uint8_t>& d, size_t at, uint32_t v) { le16(d, at, v); le16(d, at + 2, v >> 16); }

// An XDVDFS directory entry at byte offset `at` of a table (offsets in the tree are in 4-byte units)
static void entry(std::vector<uint8_t>& t, size_t at, uint16_t left, uint16_t right, uint32_t sector, uint32_t size,
                  uint8_t attributes, const std::string& name)
{
    le16(t, at, left);
    le16(t, at + 2, right);
    le32(t, at + 4, sector);
    le32(t, at + 8, size);
    t[at + 12] = attributes;
    t[at + 13] = uint8_t(name.size());
    std::memcpy(t.data() + at + 14, name.data(), name.size());
}

// A minimal retail-style XEX2: basic compression, no encryption, an execution info header
static std::vector<uint8_t> makeXex(uint32_t titleId, uint16_t encryption = 0)
{
    Bytes x;
    x.str("XEX2");
    x.u32(0);      // module flags
    x.u32(0x200);  // PE data offset
    x.u32(0);
    x.u32(0x100);  // security info offset
    x.u32(3);      // optional headers
    x.u32(0x3FF); x.u32(0x80);      // file format info
    x.u32(0x40006); x.u32(0xA0);    // execution info
    x.u32(0x10201); x.u32(0x82000000); // image base (an inline value)
    x.pad(0x80);
    x.u32(8 + 2 * 8); x.u16(encryption); x.u16(1); // info size, encryption, basic compression
    x.u32(4); x.u32(4);  // 4 bytes of data, then 4 zeros
    x.u32(2); x.u32(0);  // 2 bytes of data
    x.pad(0xA0);
    x.u32(0x11223344); x.u32(2); x.u32(1); x.u32(titleId); x.u8(0); x.u8(0); x.u8(1); x.u8(1); x.u32(0);
    x.pad(0x200);
    x.str("MZabcd");
    x.pad(0x210);
    return x.b;
}

static std::string tempDir()
{
    char tmpl[] = "/tmp/installer_test.XXXXXX";
    const char* d = mkdtemp(tmpl);
    return d ? d : "";
}

static void writeFile(const std::string& path, const std::vector<uint8_t>& data)
{
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

static std::vector<uint8_t> readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// A plain XDVDFS image (partition at offset 0) with this tree:
//   root: b.bin (entry 0) - left a.txt, right sub/ ; sub/: default.xex or c.dat
// tools/xdvdfs.py's order is the node, then the right subtree (a directory is entered at once),
// then the left: /b.bin, /sub/..., /a.txt
static std::vector<uint8_t> makeImage(const std::vector<uint8_t>& xex, bool xexAtRoot)
{
    constexpr size_t kSec = 2048;
    std::vector<uint8_t> img(48 * kSec, 0);
    std::memcpy(img.data() + 32 * kSec, "MICROSOFT*XBOX*MEDIA", 20);
    le32(img, 32 * kSec + 20, 33); // root table sector
    le32(img, 32 * kSec + 24, kSec);
    std::vector<uint8_t> root(kSec, 0), sub(kSec, 0);
    entry(root, 0, 6, 12, 35, 5, 0x00, "b.bin");    // left at 6*4 = 24, right at 12*4 = 48
    entry(root, 24, 0, 0, 36, 3, 0x00, "a.txt");
    entry(root, 48, 0, xexAtRoot ? 18 : 0, 34, uint32_t(kSec), 0x10, "sub");
    if (xexAtRoot) entry(root, 72, 0, 0, 38, uint32_t(xex.size()), 0x00, "default.xex");
    entry(sub, 0, 0, 0, 37, 2058, 0x00, "c.dat");
    std::memcpy(img.data() + 33 * kSec, root.data(), kSec);
    std::memcpy(img.data() + 34 * kSec, sub.data(), kSec);
    std::memcpy(img.data() + 35 * kSec, "hello", 5);
    std::memcpy(img.data() + 36 * kSec, "abc", 3);
    for (size_t i = 0; i < 2058; i++) img[37 * kSec + i] = uint8_t(i * 7);
    if (xexAtRoot) {
        img.resize(std::max(img.size(), 38 * kSec + xex.size()));
        std::memcpy(img.data() + 38 * kSec, xex.data(), xex.size());
    }
    return img;
}

int main()
{
    // --- AES-128 (FIPS-197 appendix C.1; NIST SP 800-38A F.2.2 CBC-AES128.Decrypt) -------------
    {
        const auto key = unhex("000102030405060708090a0b0c0d0e0f");
        const auto ct = unhex("69c4e0d86a7b0430d8cdb78070b4c55a");
        uint8_t pt[16];
        Aes128Decryptor(key.data()).decryptBlock(ct.data(), pt);
        expect(toHex(pt, 16) == "00112233445566778899aabbccddeeff", "AES-128 FIPS-197 C.1 decrypt");

        const auto cbcKey = unhex("2b7e151628aed2a6abf7158809cf4f3c");
        auto iv = unhex("000102030405060708090a0b0c0d0e0f");
        auto data = unhex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
                          "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7");
        const Aes128Decryptor cbc(cbcKey.data());
        cbc.decryptCbc(data.data(), 32, iv.data()); // in two pieces: the IV carries over
        cbc.decryptCbc(data.data() + 32, 32, iv.data());
        expect(toHex(data.data(), data.size()) ==
                   "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                   "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710",
               "AES-128 CBC SP 800-38A F.2.2, decrypted in two pieces");
    }

    // --- SHA-256 (FIPS 180-2 examples) -----------------------------------------------------------
    expect(sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of nothing");
    expect(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 abc");
    expect(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
           "SHA-256 448-bit message");
    {
        Sha256 h; // a million 'a', fed in uneven pieces through the buffer
        const std::string chunk(997, 'a');
        size_t left = 1000000;
        while (left) {
            const size_t n = left < chunk.size() ? left : chunk.size();
            h.update(chunk.data(), n);
            left -= n;
        }
        const auto d = h.finish();
        expect(toHex(d.data(), d.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
               "SHA-256 of a million 'a' in 997-byte pieces");
    }

    const std::string dir = tempDir();
    expect(!dir.empty(), "temporary directory");

    // --- the disc reader -------------------------------------------------------------------------
    {
        const std::string path = dir + "/plain.iso";
        writeFile(path, makeImage({}, false));
        DiscImage image;
        std::string error;
        expect(image.open(path, error) == DiscImage::OpenResult::Ok && image.partitionOffset() == 0, "open a plain XDVDFS image");
        std::vector<DiscFile> files;
        expect(image.list(files, error), "list: " + error);
        std::string order;
        for (const DiscFile& f : files) order += f.path + " ";
        expect(order == "/b.bin /sub/c.dat /a.txt ", "tools/xdvdfs.py's walk order, got: " + order);
        std::vector<uint8_t> data;
        expect(files.size() == 3 && image.read(files[0], data, error) && std::string(data.begin(), data.end()) == "hello", "read /b.bin");
        expect(files.size() == 3 && image.extract(files[1], dir + "/out/sub/c.dat", error), "extract /sub/c.dat: " + error);
        const auto c = readFile(dir + "/out/sub/c.dat");
        bool same = c.size() == 2058;
        for (size_t i = 0; same && i < c.size(); i++) same = c[i] == uint8_t(i * 7);
        expect(same, "extracted /sub/c.dat crosses a sector boundary intact");
        expect(!std::filesystem::exists(dir + "/out/sub/c.dat.part"), "no .part file left behind");

        // Cut short in the middle of c.dat: listing still works, the file is past the end
        std::filesystem::resize_file(path, 37 * 2048 + 100);
        DiscImage cut;
        expect(cut.open(path, error) == DiscImage::OpenResult::Ok && cut.list(files, error), "open and list a cut-short image");
        expect(files.size() == 3 && cut.end(files[1]) > cut.imageSize(), "a cut-short file ends past the image");
        expect(files.size() == 3 && !cut.extract(files[1], dir + "/out/cut.dat", error) &&
                   error.find("runs past the end of the image") != std::string::npos,
               "extracting a cut-short file says so: " + error);
        expect(!std::filesystem::exists(dir + "/out/cut.dat"), "and writes nothing");

        // Cut short inside the folder tables
        std::filesystem::resize_file(path, 33 * 2048 + 100);
        DiscImage cut2;
        expect(cut2.open(path, error) == DiscImage::OpenResult::Ok && !cut2.list(files, error) &&
                   error.find("damaged or cut short") != std::string::npos,
               "a folder past the end is reported: " + error);
    }
    {
        std::string error;
        writeFile(dir + "/archive.zip", {'P', 'K', 3, 4, 0, 0, 0, 0});
        DiscImage zip;
        expect(zip.open(dir + "/archive.zip", error) == DiscImage::OpenResult::NotXbox360 &&
                   error.find("ZIP archive") != std::string::npos,
               "a ZIP archive is recognised as one: " + error);
        DiscImage missing;
        expect(missing.open(dir + "/missing.iso", error) == DiscImage::OpenResult::CannotOpen, "a missing file can't be opened");
        expect(error.find(dir) == std::string::npos, "the message doesn't contain the path");
    }

    // --- the executable --------------------------------------------------------------------------
    {
        std::string error;
        Xex x;
        expect(x.parse(makeXex(kBackbreakerTitleId), error), "parse a synthetic XEX2: " + error);
        ExecutionInfo info;
        expect(x.executionInfo(info) && info.titleId == kBackbreakerTitleId && info.mediaId == 0x11223344 &&
                   info.discNumber == 1 && info.discCount == 1,
               "execution info: title ID, media ID, disc 1/1");
        expect(x.imageBase() == 0x82000000, "image base from the inline header");
        std::vector<uint8_t> pe;
        expect(x.decryptToPe(pe, error), "basic compression, no encryption: " + error);
        expect(pe == std::vector<uint8_t>{'M', 'Z', 'a', 'b', 0, 0, 0, 0, 'c', 'd'}, "data and zero runs in order");

        Xex bad;
        expect(bad.parse(makeXex(kBackbreakerTitleId, 2), error) && !bad.decryptToPe(pe, error) &&
                   error.find("doesn't handle") != std::string::npos,
               "an unknown encryption is refused: " + error);
        std::vector<uint8_t> notXex(64, 0);
        expect(!bad.parse(notXex, error), "no XEX2 magic is refused");
        auto truncated = makeXex(kBackbreakerTitleId);
        truncated.resize(0x203);
        expect(bad.parse(truncated, error) && !bad.decryptToPe(pe, error) && error.find("cut short") != std::string::npos,
               "a cut-short payload is reported: " + error);
    }

    // --- the import glue (tools/gen_imports.py's format) ------------------------------------------
    {
        Bytes x;
        x.b = makeXex(kBackbreakerTitleId);
        x.b[23] = 4; // a fourth optional header, in the padding after the third: the import libraries at 0x300
        std::memcpy(x.b.data() + 48, "\x00\x01\x03\xFF\x00\x00\x03\x00", 8);
        x.pad(0x300);
        Bytes lib;
        lib.u32(0x28 + 4 * 4); // record size
        lib.pad(0x26);
        lib.u16(4);            // import count
        lib.u32(0x82000100); lib.u32(0x82000104); lib.u32(0x82000108); lib.u32(0x8200010C);
        x.u32(0); x.u32(8); x.u32(1); // size (unused), string table size, library count
        x.str("lib.xex"); x.u8(0);
        x.b.insert(x.b.end(), lib.b.begin(), lib.b.end());
        Bytes pe;
        pe.pad(0x100);
        pe.u32(0x01000001); // function ordinal 1: the thunk
        pe.u32(0x00000002); // variable ordinal 2
        pe.u32(0x00000001); // function ordinal 1 as a pointer slot
        pe.u32(0x01000007); // an ordinal the table doesn't know
        std::map<std::string, ExportTable> exports;
        exports["lib.xex"][1] = {"FuncA", false};
        exports["lib.xex"][2] = {"VarB", true};
        std::string error, cpp;
        std::vector<std::string> stats;
        Xex xx;
        expect(xx.parse(x.b, error) && xx.generateImports(pe.b, exports, cpp, stats, error), "generate imports: " + error);
        const std::string want =
            "// Generated by tools/gen_imports.py - do not edit\n"
            "#include \"kernel/import_support.h\"\n\n"
            "PPC_WEAK_IMPORT(FuncA)\n"
            "PPC_WEAK_IMPORT(lib_007)\n"
            "\nconst ImportFunction g_importFunctions[] = {\n"
            "    { 0x82000100, \"FuncA\" },\n"
            "    { 0x8200010C, \"lib_007\" },\n"
            "    { 0, nullptr },\n};\n\n"
            "const ImportVariable g_importVariables[] = {\n"
            "    { 0x82000104, \"VarB\" },\n"
            "    { 0, nullptr },\n};\n\n"
            "const ImportFunctionPointer g_importFunctionPointers[] = {\n"
            "    { 0x82000108, 0x82000100, \"FuncA\" },\n"
            "    { 0, 0, nullptr },\n};\n";
        expect(cpp == want, "the generated text matches tools/gen_imports.py's format:\n" + cpp);
        expect(stats.size() == 3 && stats[2] == "unknown ordinals: lib_007", "unknown ordinals are listed");
        const auto& kernel = kernelExports();
        expect(kernel.count("xboxkrnl.exe") && kernel.at("xboxkrnl.exe").count(3) &&
                   kernel.at("xboxkrnl.exe").at(3).name == "DbgPrint",
               "the kernel's export table is built in (ordinal 3 = DbgPrint)");
    }

    // --- the title icon ------------------------------------------------------------------------
    {
        Bytes pe;
        pe.str("junk XDB junk");
        pe.str("XDBF"); pe.u32(0x10000); pe.u32(2); pe.u32(2); pe.u32(0); pe.u32(0);
        pe.u16(1); pe.u64(0x8000); pe.u32(0); pe.u32(4);  // section 1: not an image
        pe.u16(2); pe.u64(0x8000); pe.u32(4); pe.u32(12); // section 2, the title image
        pe.str("abcd");
        pe.u8(0x89); pe.str("PNG\r\n"); pe.u8(0x1A); pe.str("\nDATA");
        const auto png = titleIcon(pe.b);
        expect(png.size() == 12 && png[0] == 0x89 && png[11] == 'A', "the title image is section 2, id 0x8000");
        expect(titleIcon(std::vector<uint8_t>(100, 0)).empty(), "no XDBF: no icon");
    }

    // --- the image check ---------------------------------------------------------------------------
    {
        auto check = [&](const std::vector<uint8_t>& img) {
            const std::string path = dir + "/check.iso";
            writeFile(path, img);
            DiscImage image;
            return checkImage(path, image);
        };
        CheckResult r = check(makeImage({}, false));
        expect(r.status == CheckStatus::NotXbox360 && r.message.find("no default.xex") != std::string::npos,
               "a disc without default.xex: " + r.message);
        r = check(makeImage(makeXex(0x54540830), true));
        expect(r.status == CheckStatus::OtherGame && r.message.find("54540830") != std::string::npos,
               "another game's title ID: " + r.message);
        r = check(makeImage(makeXex(kBackbreakerTitleId), true));
        expect(r.status == CheckStatus::Damaged && r.message.find("isn't the known release") != std::string::npos,
               "Backbreaker's title ID with an unknown executable: " + r.message);
        auto cut = makeImage(makeXex(kBackbreakerTitleId), true);
        cut.resize(38 * 2048 + 100);
        r = check(cut);
        expect(r.status == CheckStatus::Damaged && r.message.find("cut short") != std::string::npos,
               "a cut-short image: " + r.message);
        expect(kKnownReleaseCount >= 1 && std::strlen(kKnownReleases[0].xexSha256) == 64, "a known release is listed");
    }

    // --- The applications-menu entry (Desktop Entry Specification 1.5, "The Exec key") ----------
    {
        expect(desktopExecArg("/home/deck/.local/share/backbreaker-recomp/install/bin/backbreaker") ==
                   "/home/deck/.local/share/backbreaker-recomp/install/bin/backbreaker",
               "a plain path is written as it is");
        expect(desktopExecArg("/home/a b/bin/backbreaker") == "\"/home/a b/bin/backbreaker\"", "a space: quoted");
        expect(desktopExecArg("/home/it's/(1)/bin/backbreaker") == "\"/home/it's/(1)/bin/backbreaker\"",
               "' and (): quoted");
        expect(desktopExecArg("/games/100%/bin/backbreaker") == "/games/100%%/bin/backbreaker", "% doubled");
        expect(desktopExecArg("/home/a\"b/backbreaker").empty() && desktopExecArg("/home/$x/backbreaker").empty() &&
                   desktopExecArg("/home/a\\b/backbreaker").empty() && desktopExecArg("/home/`x`/backbreaker").empty() &&
                   desktopExecArg("/home/a\nb/backbreaker").empty() && desktopExecArg("").empty(),
               "\", $, \\, `, a newline or nothing: not written");
        const std::string e = menuEntry("/home/a b/bin/backbreaker", "/home/a b/game/icon.png");
        expect(e.rfind("[Desktop Entry]\n", 0) == 0, "the entry starts with its group");
        expect(e.find("\nExec=\"/home/a b/bin/backbreaker\"\n") != std::string::npos, "Exec quoted");
        expect(e.find("\nTryExec=/home/a b/bin/backbreaker\n") != std::string::npos, "TryExec as it is (a string)");
        expect(e.find("\nIcon=/home/a b/game/icon.png\n") != std::string::npos, "the icon's path");
        expect(e.find("\nStartupWMClass=backbreaker-recomp\n") != std::string::npos &&
                   std::string(kAppId) == "backbreaker-recomp",
               "the window's app id (runtime/video/presenter.cpp)");
        expect(menuEntry("/home/$x/bin/backbreaker", "/home/$x/game/icon.png").empty(), "no entry for a path it can't write");
        setenv("XDG_DATA_HOME", "/xdg/data", 1);
        expect(menuEntryPath() == "/xdg/data/applications/backbreaker-recomp.desktop", "the entry's path ($XDG_DATA_HOME)");
        setenv("XDG_DATA_HOME", "relative", 1);
        setenv("HOME", "/home/deck", 1);
        expect(menuEntryPath() == "/home/deck/.local/share/applications/backbreaker-recomp.desktop" &&
                   defaultPrefix() == "/home/deck/.local/share/backbreaker-recomp/install",
               "a relative $XDG_DATA_HOME is ignored, as the game ignores it");
    }

    std::filesystem::remove_all(dir);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
