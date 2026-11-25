// Checks the installer's game-file steps (installer/, #138) without game files: AES-128 and
// SHA-256 against the standards' test vectors, and the disc reader, the XEX decoder, the import
// glue generator, the title icon and the image check on images and executables built here by hand.
// The same code is checked byte for byte against the Python tools on a real disc (see the PR).
// Also the applications-menu entry, and the Steam shortcut's shortcuts.vdf against files written
// here byte by byte, with a temporary HOME and a made-up /proc (never the machine's own Steam).
// And the title update's package reader (installer/title_update.h) on packages made here.
// Build:
//   clang++ -std=c++20 -Wall -Wextra -O2 -I third_party/XenonRecomp/XenonUtils tests/installer_test.cpp \
//       installer/crypto.cpp installer/disc.cpp installer/xex.cpp installer/game_check.cpp \
//       installer/menu_entry.cpp installer/steam_shortcut.cpp installer/build.cpp installer/platform.cpp \
//       installer/title_update.cpp -o build/installer_test
#include "../installer/crypto.h"
#include "../installer/disc.h"
#include "../installer/build.h"
#include "../installer/game_check.h"
#include "../installer/menu_entry.h"
#include "../installer/steam_shortcut.h"
#include "../installer/title_update.h"
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

// Binary KeyValues written by hand, as Steam writes shortcuts.vdf (installer/steam_shortcut.h)
struct Kv
{
    std::vector<uint8_t> b;
    Kv& key(uint8_t type, const std::string& k) { b.push_back(type); b.insert(b.end(), k.begin(), k.end()); b.push_back(0); return *this; }
    Kv& map(const std::string& k) { return key(0x00, k); }
    Kv& str(const std::string& k, const std::string& v) { key(0x01, k); b.insert(b.end(), v.begin(), v.end()); b.push_back(0); return *this; }
    Kv& i32(const std::string& k, uint32_t v) { key(0x02, k); for (int i = 0; i < 4; i++) b.push_back(uint8_t(v >> (8 * i))); return *this; }
    Kv& u64(const std::string& k, uint64_t v) { key(0x07, k); for (int i = 0; i < 8; i++) b.push_back(uint8_t(v >> (8 * i))); return *this; }
    Kv& end() { b.push_back(0x08); return *this; }
    Kv& add(const Kv& o) { b.insert(b.end(), o.b.begin(), o.b.end()); return *this; }
};

// One game's entry with the keys, order and spelling Steam's own "Add a Non-Steam Game" writes
static Kv steamEntry(const std::string& index, uint32_t appid, const std::string& name, const std::string& exe,
                     const std::string& dir, const std::string& icon, const std::string& options)
{
    Kv e;
    e.map(index).i32("appid", appid).str("appname", name).str("exe", exe).str("StartDir", dir).str("icon", icon)
        .str("ShortcutPath", "").str("LaunchOptions", options).i32("IsHidden", 0).i32("AllowDesktopConfig", 1)
        .i32("AllowOverlay", 1).i32("OpenVR", 0).i32("Devkit", 0).str("DevkitGameID", "").i32("DevkitOverrideAppID", 0)
        .i32("LastPlayTime", 0).str("FlatpakAppID", "").str("sortas", "").map("tags").end().end();
    return e;
}

// The entry `index` of a shortcuts.vdf, parsed
static const VdfNode* shortcutEntry(const std::vector<VdfNode>& top, const std::string& index)
{
    for (const VdfNode& list : top)
        if (list.key == "shortcuts")
            for (const VdfNode& e : list.children)
                if (e.key == index) return &e;
    return nullptr;
}

static std::string field(const VdfNode* e, const std::string& key)
{
    if (e)
        for (const VdfNode& n : e->children)
            if (n.key == key) return n.value;
    return "<missing>";
}

static std::string le32s(uint32_t v) { return {char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; }

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

// An STFS package (installer/title_update.h) with one hash table per group, as a title update has:
// the file table in block 0, then each file's blocks as given, chained through the hash table.
struct StfsFileSpec
{
    std::string name;
    int parent;                   // an index into the files, -1 at the top
    bool folder;
    std::vector<uint32_t> blocks; // where its data goes, in order
    std::vector<uint8_t> data;
};

static std::vector<uint8_t> makeStfs(uint32_t titleId, const std::vector<StfsFileSpec>& files, const char* magic = "LIVE")
{
    const size_t first = 0xB000; // header size 0xAD0E, rounded up to a block
    uint32_t last = 0;
    for (const auto& f : files)
        for (uint32_t b : f.blocks) last = std::max(last, b);
    std::vector<uint8_t> d(first + 0x1000 * (last + 2), 0);
    auto be32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; i++) d[at + i] = uint8_t(v >> (24 - 8 * i)); };
    auto link = [&](uint32_t block, uint32_t next) {
        const size_t o = first + block * 0x18;
        d[o + 0x14] = 0x80;
        d[o + 0x15] = uint8_t(next >> 16); d[o + 0x16] = uint8_t(next >> 8); d[o + 0x17] = uint8_t(next);
    };
    std::memcpy(d.data(), magic, 4);
    be32(0x340, 0xAD0E);
    be32(0x360, titleId);
    const char name[] = "Test Update";
    for (size_t i = 0; name[i]; i++) d[0x411 + 2 * i + 1] = uint8_t(name[i]);
    d[0x379] = 0x24; d[0x37B] = 1; // the volume descriptor: its size, one hash table per group
    d[0x37C] = 1;                  // the file table: one block, at block 0
    link(0, 0xFFFFFF);
    for (size_t i = 0; i < files.size(); i++) {
        const StfsFileSpec& f = files[i];
        uint8_t* e = &d[first + 0x1000 + i * 0x40];
        std::memcpy(e, f.name.data(), f.name.size());
        e[0x28] = uint8_t(f.name.size() | (f.folder ? 0x80 : 0));
        e[0x29] = e[0x2C] = uint8_t(f.blocks.size());
        if (!f.blocks.empty()) { e[0x2F] = uint8_t(f.blocks[0]); e[0x30] = uint8_t(f.blocks[0] >> 8); }
        e[0x32] = uint8_t(uint16_t(f.parent) >> 8); e[0x33] = uint8_t(f.parent);
        for (int k = 0; k < 4; k++) e[0x34 + k] = uint8_t(f.data.size() >> (24 - 8 * k));
        for (size_t k = 0; k < f.blocks.size(); k++) {
            const size_t at = k * 0x1000, n = std::min<size_t>(0x1000, f.data.size() - std::min(at, f.data.size()));
            std::memcpy(&d[first + 0x1000 * (f.blocks[k] + 1)], f.data.data() + std::min(at, f.data.size()), n);
            link(f.blocks[k], k + 1 < f.blocks.size() ? f.blocks[k + 1] : 0xFFFFFF);
        }
    }
    return d;
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

        // A launcher's hidden entry, for the window's name and icon only
        const std::string g = "/lib/bin/backbreaker";
        const std::string h = menuEntry(g, "/lib/game/icon.png", true);
        expect(h.find("\nNoDisplay=true\n") != std::string::npos && e.find("NoDisplay") == std::string::npos,
               "hidden: NoDisplay=true; the menu's entry has none");
        expect(entryValue(h, "TryExec") == g && entryValue(h, "NoDisplay") == "true" &&
                   entryValue(h, "StartupWMClass") == "backbreaker-recomp" && entryValue(h, "Hidden").empty() &&
                   entryValue(h, "Exec") == g,
               "entryValue reads the entry's keys");
        expect(entryValue("[Other]\nTryExec=/x\n[Desktop Entry]\nName=B\n", "TryExec").empty() &&
                   entryValue("[Desktop Entry]\r\nTryExec = /y\r\n", "TryExec") == "/y" &&
                   entryValue("[Desktop Entry]\nTryExecX=/z\n", "TryExec").empty(),
               "entryValue: only the [Desktop Entry] group, spaces around '=', CRLF, whole keys");
        const auto there = [](const std::string& p) { return p == "/other/bin/backbreaker"; };
        expect(launcherEntryReplaces("", g, there), "no entry yet: written");
        expect(!launcherEntryReplaces(menuEntry("/other/bin/backbreaker", "/other/game/icon.png"), g, there) &&
                   !launcherEntryReplaces(menuEntry("/other/bin/backbreaker", "/other/game/icon.png", true), g, there),
               "another install that's still there, in the menu or hidden: left alone");
        expect(launcherEntryReplaces(menuEntry("/gone/bin/backbreaker", "/gone/game/icon.png"), g, there) &&
                   launcherEntryReplaces(menuEntry("/gone/bin/backbreaker", "/gone/game/icon.png", true), g, there),
               "another install that's gone: replaced");
        expect(launcherEntryReplaces(menuEntry(g, "/old/icon.png", true), g, there) &&
                   !launcherEntryReplaces(menuEntry(g, "/lib/game/icon.png"), g, there),
               "this game's own: rewritten while hidden, left alone in the menu");
        expect(!launcherEntryReplaces("[Desktop Entry]\nType=Application\nName=Mine\nExec=mine\n", g, there),
               "an entry with no TryExec isn't one of ours: left alone");

        // Windows' shortcut icon (#174): the title's PNG in an .ico, as its one image
        auto pngOf = [](uint32_t w, uint32_t h) {
            std::vector<uint8_t> p = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
            for (uint32_t v : {w, h})
                for (int i = 3; i >= 0; i--) p.push_back(uint8_t(v >> (8 * i)));
            p.insert(p.end(), {8, 6, 0, 0, 0, 0x12, 0x34, 0x56, 0x78, 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82});
            return p;
        };
        const std::vector<uint8_t> png = pngOf(64, 64), ico = icoFromPng(png);
        auto le32 = [&](size_t at) { return uint32_t(ico[at]) | uint32_t(ico[at + 1]) << 8 | uint32_t(ico[at + 2]) << 16 | uint32_t(ico[at + 3]) << 24; };
        expect(ico.size() == 22 + png.size() && ico[0] == 0 && ico[1] == 0 && ico[2] == 1 && ico[4] == 1,
               "an .ico: ICONDIR for one icon image, then the PNG");
        expect(ico[6] == 64 && ico[7] == 64 && ico[10] == 1 && ico[12] == 32 && le32(14) == png.size() && le32(18) == 22 &&
                   std::equal(png.begin(), png.end(), ico.begin() + 22),
               "its entry: 64x64, one plane, 32 bits, the PNG's size, at offset 22, the PNG's bytes as they are");
        expect(icoFromPng(pngOf(256, 256))[6] == 0 && icoFromPng(pngOf(256, 256))[7] == 0, "256x256 is written as 0");
        expect(icoFromPng(pngOf(257, 64)).empty() && icoFromPng(pngOf(0, 64)).empty() &&
                   icoFromPng(std::vector<uint8_t>(40, 0)).empty() && icoFromPng({}).empty(),
               "not a PNG, or too big for an .ico: none");
        setenv("XDG_DATA_HOME", "/xdg/data", 1);
        expect(menuEntryPath() == "/xdg/data/applications/backbreaker-recomp.desktop", "the entry's path ($XDG_DATA_HOME)");
        setenv("XDG_DATA_HOME", "relative", 1);
        setenv("HOME", "/home/deck", 1);
        expect(menuEntryPath() == "/home/deck/.local/share/applications/backbreaker-recomp.desktop" &&
                   defaultPrefix() == "/home/deck/.local/share/backbreaker-recomp/install",
               "a relative $XDG_DATA_HOME is ignored, as the game ignores it");
    }

    // --- Steam's non-Steam game (installer/steam_shortcut.h, #160) -----------------------------
    {
        expect(crc32("123456789", 9) == 0xCBF43926u && crc32("", 0) == 0, "CRC-32's check value (zlib's)");
        const std::string exe = "/home/deck/.local/share/backbreaker-recomp/install/bin/backbreaker";
        const std::string quotedExe = "\"" + exe + "\"";
        const std::string bin = "/home/deck/.local/share/backbreaker-recomp/install/bin";
        const std::string icon = "/home/deck/.local/share/backbreaker-recomp/install/game/icon.png";
        const SteamShortcut s{"Backbreaker", exe, bin, icon};
        const std::string idKey = quotedExe + "Backbreaker";
        const uint32_t newId = crc32(idKey.data(), idKey.size()) | 0x80000000u;

        // Another game, as Steam wrote it, with a tag and a uint64 this doesn't use
        Kv other = steamEntry("0", 0xC0FFEE01u, "Other", "\"/usr/bin/other\"", "\"/usr/bin/\"", "", "-x");
        other.b.resize(other.b.size() - 2); // reopen its tags
        other.str("0", "favorite").end().u64("Big", 1ull << 40).end();
        Kv file;
        file.map("shortcuts").add(other).end().end();
        std::vector<VdfNode> top;
        expect(parseVdf(file.b, top) && writeVdf(top) == file.b, "a Steam-written shortcuts.vdf reads and writes back byte for byte");

        std::vector<uint8_t> out;
        uint32_t id = 0;
        std::string error;
        ShortcutChange change = addShortcut(file.b, s, out, id, error);
        Kv want;
        want.map("shortcuts").add(other)
            .add(steamEntry("1", newId, "Backbreaker", quotedExe, "\"" + bin + "\"", icon, "")).end().end();
        expect(change == ShortcutChange::Added && out == want.b && id == newId,
               "added after the other game, as entry 1, with Steam's keys and crc32(Exe + AppName) | 0x80000000 as its id");
        const std::vector<uint8_t> added = out;
        change = addShortcut(added, s, out, id, error);
        expect(change == ShortcutChange::Unchanged && id == newId, "added again: nothing changes");

        Kv none;
        none.map("shortcuts").add(steamEntry("0", newId, "Backbreaker", quotedExe, "\"" + bin + "\"", icon, "")).end().end();
        expect(addShortcut({}, s, out, id, error) == ShortcutChange::Added && out == none.b, "no file yet: one with only the game");

        // Added before through Steam's own dialog (browsed to: the file's name, an id of Steam's
        // own, no icon), with the player's launch options
        Kv mine;
        mine.map("shortcuts").add(steamEntry("0", 0x9234ABCDu, "backbreaker", quotedExe, "\"" + bin + "/\"", "", "BB_PERF=1 %command%"))
            .end().end();
        change = addShortcut(mine.b, s, out, id, error);
        expect(change == ShortcutChange::Updated && id == 0x9234ABCDu, "the same program: updated, its id kept");
        expect(parseVdf(out, top) && field(shortcutEntry(top, "0"), "appname") == "Backbreaker" &&
                   field(shortcutEntry(top, "0"), "icon") == icon &&
                   field(shortcutEntry(top, "0"), "StartDir") == "\"" + bin + "\"" &&
                   field(shortcutEntry(top, "0"), "LaunchOptions") == "BB_PERF=1 %command%" && !shortcutEntry(top, "1"),
               "its name and icon set, the player's launch options kept, no second entry");

        // The portable AppImage added as Backbreaker, with options for it: the same name
        Kv appImage;
        appImage.map("shortcuts").add(steamEntry("0", 0x8111AAAAu, "BACKBREAKER", "\"/home/deck/Backbreaker.AppImage\"",
                                                 "\"/home/deck/\"", "", "--fullscreen"))
            .end().end();
        change = addShortcut(appImage.b, s, out, id, error);
        expect(change == ShortcutChange::Updated && id == 0x8111AAAAu && parseVdf(out, top) &&
                   field(shortcutEntry(top, "0"), "exe") == quotedExe && field(shortcutEntry(top, "0"), "LaunchOptions").empty() &&
                   !shortcutEntry(top, "1"),
               "the same name, another program: that entry starts the game now, without the other program's options");

        // From a Steam that stored no appid (it worked it out from Exe and AppName): that id is kept
        Kv legacy;
        legacy.map("shortcuts").map("0").str("AppName", "Backbreaker").str("Exe", "\"/old/bin/backbreaker\"").end().end().end();
        const std::string oldKey = "\"/old/bin/backbreaker\"Backbreaker";
        const uint32_t oldId = crc32(oldKey.data(), oldKey.size()) | 0x80000000u;
        change = addShortcut(legacy.b, s, out, id, error);
        expect(change == ShortcutChange::Updated && id == oldId && parseVdf(out, top) &&
                   field(shortcutEntry(top, "0"), "appid") == le32s(oldId) && field(shortcutEntry(top, "0"), "Exe") == quotedExe,
               "no stored id: the one Steam derived from the old Exe is stored, the keys keep their spelling");

        Kv wide;
        wide.map("shortcuts").map("0").key(0x05, "name").b.insert(wide.b.end(), {'x', 0, 0, 0});
        wide.end().end().end();
        expect(addShortcut(wide.b, s, out, id, error) == ShortcutChange::Failed && error.find("can't read") != std::string::npos,
               "a type it doesn't know: the file is left alone (" + error + ")");
        std::vector<uint8_t> cut = added;
        if (!cut.empty()) cut.pop_back();
        expect(!parseVdf(cut, top) && addShortcut(cut, s, out, id, error) == ShortcutChange::Failed, "a file cut short: left alone");
        std::vector<uint8_t> trailing = added;
        trailing.push_back(0);
        expect(!parseVdf(trailing, top), "bytes after the end: refused");
        expect(addShortcut({}, {"Backbreaker", "/home/$x/bin/backbreaker", "/home/$x/bin", ""}, out, id, error) ==
                   ShortcutChange::Failed,
               "a path it can't quote: no shortcut");
        expect(steamQuoted("/home/a b/it's") == "\"/home/a b/it's\"" && steamQuoted("/a\"b").empty() &&
                   steamQuoted("/a`b").empty() && steamQuoted("/a\\b").empty() && steamQuoted("/a\nb").empty(),
               "Exe in quotes; \", `, \\ or a newline: not written");

        // Where Steam is, under a temporary HOME
        const std::string home = dir + "/home";
        namespace fs = std::filesystem;
        fs::create_directories(home + "/.local/share/Steam/userdata/123/config");
        fs::create_directories(home + "/.local/share/Steam/userdata/0/config");
        fs::create_directories(home + "/.local/share/Steam/userdata/anonymous");
        fs::create_directories(home + "/.steam");
        fs::create_directory_symlink(home + "/.local/share/Steam", home + "/.steam/steam");
        fs::create_directory_symlink(home + "/.local/share/Steam", home + "/.steam/root");
        fs::create_directories(home + "/Steam"); // no userdata/
        fs::create_directories(home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam/userdata/456");
        setenv("HOME", home.c_str(), 1);
        unsetenv("XDG_DATA_HOME");
        const std::vector<SteamInstall> installs = findSteamInstalls();
        expect(installs.size() == 2 && installs[0].root == home + "/.steam/steam" && !installs[0].flatpak &&
                   installs[1].root == home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam" && installs[1].flatpak,
               "native Steam once (through its links) and Flathub's");
        const auto accounts = installs.size() == 2 ? steamAccountConfigs(installs[0].root) : std::vector<std::string>{};
        expect(accounts.size() == 1 && accounts[0] == home + "/.steam/steam/userdata/123/config",
               "accounts: numbered folders, not 0");

        // A made-up /proc: Steam's helpers aren't Steam, Steam is
        const std::string proc = dir + "/proc";
        auto process = [&](const std::string& pid, const std::string& comm) {
            fs::create_directories(proc + "/" + pid);
            std::ofstream(proc + "/" + pid + "/comm") << comm << "\n";
        };
        process("10", "bash");
        process("11", "steamwebhelper");
        process("self", "steam");
        expect(!steamRunning(proc), "no process named steam: not running");
        process("12", "steam");
        expect(steamRunning(proc), "a process named steam: running");
    }

    // What a bundle gives ppc/ (#160): XenonRecomp's source, not its bytes, so a release built in a
    // newer container doesn't make every player recompile; the config and kit still count
    {
        const std::string root = dir + "/bundle";
        Bundle b;
        b.root = root;
        b.kit = root + "/kit";
        b.xenonRecomp = root + "/xenonrecomp/XenonRecomp";
        b.xenonRecompSource = root + "/xenonrecomp/SOURCE";
        b.ppcContext = root + "/xenonrecomp/ppc_context.h";
        b.config = root + "/config";
        auto put = [](const std::string& path, const std::string& text) {
            std::filesystem::create_directories(std::filesystem::path(path).parent_path());
            std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
        };
        put(b.xenonRecomp, "a program");
        put(b.ppcContext, "context");
        put(b.config + "/backbreaker.toml", "config");
        put(b.config + "/backbreaker_switch_tables.toml", "tables");
        put(b.kit + "/share/backbreaker-runtime/ppc-compile.args", "-O2\n");
        put(b.kit + "/include/ppc_hooks.h", "hooks");
        const std::string byProgram = bundlePpcInputs(b);
        put(b.xenonRecomp, "the same source, another compiler");
        expect(bundlePpcInputs(b) != byProgram, "no SOURCE: the program's bytes count");
        put(b.xenonRecompSource, "XenonRecomp ddd128b\npatch 1\n");
        const std::string bySource = bundlePpcInputs(b);
        put(b.xenonRecomp, "a third build");
        expect(bundlePpcInputs(b) == bySource, "with SOURCE: a rebuilt program changes nothing");
        put(b.xenonRecompSource, "XenonRecomp ddd128b\npatch 2\n");
        expect(bundlePpcInputs(b) != bySource, "a new patch changes it");
        put(b.xenonRecompSource, "XenonRecomp ddd128b\npatch 1\n");
        put(b.config + "/backbreaker.toml", "config, changed");
        expect(bundlePpcInputs(b) != bySource, "a new config changes it");
        put(b.config + "/backbreaker.toml", "config");
        expect(bundlePpcInputs(b) == bySource, "and back again");
        // The title update's parts are not the disc build's inputs: adding them rebuilds nothing
        expect(!bundleHasTitleUpdate(b), "a bundle without the title update's parts");
        put(b.config + "/backbreaker_tu2.toml", "update config");
        put(b.config + "/backbreaker_tu2_switch_tables.toml", "update tables");
        put(b.kit + "/share/backbreaker-runtime/tu2/game-link.args", "link");
        expect(!bundleHasTitleUpdate(b), "... or with only some of them");
        put(b.kit + "/share/backbreaker-runtime/tu2/ppc_config.h", "layout");
        expect(bundleHasTitleUpdate(b), "a bundle with all of them");
        expect(bundlePpcInputs(b) == bySource, "which leave the disc build's inputs as they were");
    }

    // --- The title update's package (installer/title_update.h) -----------------------------------
    {
        std::vector<uint8_t> big(0x1000 + 9);
        for (size_t i = 0; i < big.size(); i++) big[i] = uint8_t(i * 7);
        const std::vector<uint8_t> small{'s', 'e', 'c', 'o', 'n', 'd'};
        // a.bin's two blocks are not next to each other: 1, then 3
        const std::vector<StfsFileSpec> files{{"sub", -1, true, {}, {}}, {"a.bin", 0, false, {1, 3}, big}, {"B.txt", -1, false, {2}, small}};
        StfsPackage p;
        std::string error;
        expect(p.parse(makeStfs(0x464F07DA, files), error), "an STFS package parses");
        expect(p.titleId() == 0x464F07DA && p.displayName() == "Test Update", "its title ID and name");
        expect(p.entries().size() == 3 && p.entries()[0].folder && p.entries()[1].path == "sub/a.bin" &&
                   p.entries()[1].size == big.size() && p.entries()[2].path == "B.txt",
               "its files, with their folder");
        std::vector<uint8_t> out;
        expect(p.find("SUB/A.BIN") && p.read(*p.find("SUB/A.BIN"), out, error) && out == big,
               "a file whose blocks are apart is read through the hash table's chain");
        expect(p.find("b.txt") && p.read(*p.find("b.txt"), out, error) && out == small, "a small file, found without regard to case");
        expect(!p.find("sub") && !p.find("missing"), "a folder or a missing name is not a file");

        StfsPackage bad;
        std::vector<uint8_t> xexBytes = makeXex(0x464F07DA);
        xexBytes.resize(0xA000);
        expect(!bad.parse(xexBytes, error) && error.find("not an Xbox 360 package") != std::string::npos, "an executable is not a package");
        std::vector<uint8_t> cut = makeStfs(0x464F07DA, files);
        cut.resize(0xB000 + 0x1000 + 0x1800); // ends inside block 1
        StfsPackage cutPackage;
        expect(cutPackage.parse(cut, error) && !cutPackage.read(*cutPackage.find("sub/a.bin"), out, error) &&
                   error.find("cut short") != std::string::npos,
               "a file past the end of a cut-short package is an error, not a crash");
        std::vector<uint8_t> loop = makeStfs(0x464F07DA, {{"x", 0, false, {1}, small}}); // its own parent
        expect(StfsPackage().parse(loop, error) || !error.empty(), "an entry that is its own parent doesn't hang");

        // Is it Title Update 2? Another game's package, one without the patch, one with another patch
        const std::string updates = dir + "/update";
        std::filesystem::create_directories(updates);
        std::vector<uint8_t> patch, index, data;
        writeFile(updates + "/other", makeStfs(0x12345678, files));
        expect(looksLikeStfs(updates + "/other") && !readTitleUpdate(updates + "/other", patch, index, data, error) &&
                   error.find("another game") != std::string::npos,
               "another game's package is refused");
        writeFile(updates + "/nopatch", makeStfs(0x464F07DA, files));
        expect(!readTitleUpdate(updates + "/nopatch", patch, index, data, error) && error.find("without default.xexp") != std::string::npos,
               "a Backbreaker package without the patch is refused");
        const std::vector<StfsFileSpec> lookalike{{"default.xexp", -1, false, {1}, small},
                                                  {"PatchPackedOutputX360.pkh", -1, false, {2}, small},
                                                  {"PatchPackedOutputX360.pkd", -1, false, {3}, small}};
        writeFile(updates + "/lookalike", makeStfs(0x464F07DA, lookalike));
        expect(!readTitleUpdate(updates + "/lookalike", patch, index, data, error) && error.find("doesn't know") != std::string::npos,
               "a package whose patch is not the known one is refused");
        writeFile(updates + "/notes.txt", {'h', 'i'});
        expect(!looksLikeStfs(updates + "/notes.txt") && !looksLikeStfs(updates + "/missing"), "other files are not packages");

        // The update folder: nothing there, only other files, only packages that can't be used
        const std::string game = dir + "/tu-game";
        expect(syncTitleUpdate(dir + "/no-such-folder", game).status == UpdateStatus::None, "no update folder: the disc's version");
        const std::string onlyNotes = dir + "/update-notes";
        std::filesystem::create_directories(onlyNotes);
        writeFile(onlyNotes + "/notes.txt", {'h', 'i'});
        expect(syncTitleUpdate(onlyNotes, game).status == UpdateStatus::None, "a folder without a package: the disc's version");
        const UpdateResult unusable = syncTitleUpdate(updates, game);
        expect(unusable.status == UpdateStatus::Unusable && unusable.message.find("lookalike in the update folder") == 0 &&
                   !std::filesystem::exists(game + "/extracted/tu2"),
               "packages that can't be used: said so, by name, and nothing is written");
    }

    // --- The patched executable XenonRecomp writes: not encrypted, not compressed ----------------
    {
        Bytes x;
        x.str("XEX2");
        x.u32(0); x.u32(0x200); x.u32(0); x.u32(0x100); x.u32(1);
        x.u32(0x3FF); x.u32(0x80);
        x.pad(0x80);
        x.u32(8); x.u16(0); x.u16(0); // no encryption, no compression
        x.pad(0x200);
        x.str("MZ, the image as it is");
        Xex plain;
        std::string error;
        std::vector<uint8_t> pe;
        expect(plain.parse(x.b, error) && plain.decryptToPe(pe, error) && std::string(pe.begin(), pe.end()) == "MZ, the image as it is",
               "a plain XEX gives the bytes after its headers");
        x.b[0x200] = 'X';
        expect(plain.parse(x.b, error) && !plain.decryptToPe(pe, error), "... and is refused when no program is there");
    }

    std::filesystem::remove_all(dir);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
