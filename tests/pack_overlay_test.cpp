// Tests runtime/kernel/pack_overlay.cpp (#169): files in the mods folder that replace single entries
// of the game's archive, through a rewritten index and replacement files appended to the data.
//
// What is checked, on archives made up here in a temporary directory:
//   - nothing to replace (no mods folder, an empty one, files named like no entry, files under
//     packedfilesx360/): no overlay, so the game reads its own files untouched
//   - one entry replaced: the index is the same size and still parses, the entry points past the
//     original data at a 2048-byte boundary with the replacement's size, the other entries are as
//     they were, and the data reads back as the original followed by the replacement
//   - the mods file's name matched without regard to case, in folders with spaces in their names
//   - two replacements, one larger than the entry and one empty: no overlap, zeros between files
//   - reads that cross from the original data into the padding and on into a replacement, reads
//     that end past the end (short), and reads past the end (0)
//   - firstReads names each replacement once, when a read first covers it
//   - a big-endian index is rewritten big-endian
//   - an index that doesn't parse: no overlay, and a log line that says why
//   - readEntry: an entry's bytes straight from the archive, by a name in another case; through an
//     overlay the replacement instead; an empty entry; no such entry, no such archive
//
// No game files. Build and run: .github/workflows/ci.yml
//   clang++ -std=c++20 -Wall -Wextra -O2 tests/pack_overlay_test.cpp runtime/kernel/pack_overlay.cpp -o build/pack_overlay_test

#include "../runtime/kernel/pack_overlay.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace kernel::pack;

static int failures = 0, checks = 0;

static void ok(bool good, const char* what)
{
    checks++;
    if (!good) failures++, printf("FAIL %s\n", what);
}

static fs::path g_dir;

using Bytes = std::vector<uint8_t>;

static Bytes text(const std::string& s) { return Bytes(s.begin(), s.end()); }

static void put32(Bytes& b, uint32_t v, bool big)
{
    for (int i = 0; i < 4; i++)
        b.push_back(uint8_t(v >> (big ? 24 - 8 * i : 8 * i)));
}

static void writeFile(const fs::path& p, const Bytes& b)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write((const char*)b.data(), std::streamsize(b.size()));
}

// An archive laid out as the shipped one is: entries at 2048-byte boundaries, each taking at least
// one 2048-byte block, names NUL-terminated.
static fs::path makePack(const std::string& name, const std::vector<std::pair<std::string, Bytes>>& entries, bool big = false)
{
    Bytes pkh, pkd;
    uint32_t names = 0;
    for (auto& [n, _] : entries) names += uint32_t(n.size() + 1);
    put32(pkh, uint32_t(entries.size()), big);
    put32(pkh, names, big);
    for (auto& [n, data] : entries)
    {
        put32(pkh, uint32_t(n.size() + 1), big);
        pkh.insert(pkh.end(), n.begin(), n.end());
        pkh.push_back(0);
        put32(pkh, uint32_t(pkd.size()), big);
        put32(pkh, uint32_t(data.size()), big);
        pkd.insert(pkd.end(), data.begin(), data.end());
        pkd.resize(data.empty() ? pkd.size() + 2048 : (pkd.size() + 2047) / 2048 * 2048); // an empty entry takes a block
    }
    fs::path prefix = g_dir / name / "packedfilesx360" / "Pack";
    writeFile(prefix.string() + ".pkh", pkh);
    writeFile(prefix.string() + ".pkd", pkd);
    return prefix;
}

static Bytes readAll(const Overlay& o, bool index, uint64_t at, uint32_t len)
{
    Bytes b(len, 0xAA);
    long n = o.read(index, b.data(), len, at);
    b.resize(n < 0 ? 0 : size_t(n));
    return b;
}

static Bytes fileBytes(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), {});
}

static const Entry* find(const Index& ix, const std::string& name)
{
    for (auto& e : ix.entries)
        if (e.name == name) return &e;
    return nullptr;
}

static std::shared_ptr<Overlay> build(const fs::path& prefix, const fs::path& mods, std::vector<std::string>& log)
{
    return Overlay::build(prefix.string() + ".pkh", prefix.string() + ".pkd", mods, log);
}

int main()
{
    std::string tmpl = (fs::temp_directory_path() / "pack_overlay_test_XXXXXX").string();
    if (!mkdtemp(tmpl.data())) { perror("mkdtemp"); return 1; }
    g_dir = tmpl;

    const Bytes play = text("<play name=\"Dagger\">\r\n  <route/>\r\n</play>\r\n");
    const Bytes menu = text("<scene>title</scene>\n");
    Bytes big(5000);
    for (size_t i = 0; i < big.size(); i++) big[i] = uint8_t(i * 7 + 1);
    std::vector<std::pair<std::string, Bytes>> entries = {
        { "media\\PlayDatabase\\Offense\\Dagger.pd", play },
        { "media\\ui\\Final_XUI\\Single Player\\Title.xui", menu },
        { "shaders\\post_process.fx360", big },
    };
    fs::path pack = makePack("a", entries);
    Bytes pkh = fileBytes(pack.string() + ".pkh"), pkd = fileBytes(pack.string() + ".pkd");

    // Nothing to replace
    {
        std::vector<std::string> log;
        ok(!build(pack, g_dir / "no-mods", log), "no mods folder: no overlay");
        fs::create_directories(g_dir / "empty");
        ok(!build(pack, g_dir / "empty", log), "empty mods folder: no overlay");
        writeFile(g_dir / "loose" / "media" / "Movies" / "505.wmv", text("movie"));
        writeFile(g_dir / "loose" / "packedfilesx360" / "media" / "PlayDatabase" / "Offense" / "Dagger.pd", text("x"));
        ok(!build(pack, g_dir / "loose", log), "files named like no entry, or under packedfilesx360/: no overlay");
        ok(log.empty(), "nothing to replace: nothing logged");
    }

    // One entry, its name in another case
    {
        fs::path mods = g_dir / "one";
        const Bytes mine = text("<play name=\"Dagger\">\r\n  <route deep=\"1\"/>\r\n</play>\r\n");
        writeFile(mods / "MEDIA" / "playdatabase" / "Offense" / "dagger.PD", mine);
        std::vector<std::string> log;
        auto o = build(pack, mods, log);
        ok(o != nullptr, "one replacement: an overlay");
        if (o)
        {
            ok(o->replacements() == 1, "one replacement counted");
            ok(log.size() == 1 && log[0].find("replaces media\\PlayDatabase\\Offense\\Dagger.pd in Pack") != std::string::npos,
               "the log names the file and the entry it replaces");
            ok(o->size(true) == pkh.size(), "the index keeps its size");
            Bytes ix = readAll(*o, true, 0, uint32_t(pkh.size()));
            Index parsed;
            std::string why;
            ok(parseIndex(ix, o->size(false), parsed, why), "the rewritten index parses against the new data size");
            ok(!parsed.bigEndian && parsed.entries.size() == 3, "little-endian, three entries");
            const Entry* e = find(parsed, "media\\PlayDatabase\\Offense\\Dagger.pd");
            ok(e && e->offset == pkd.size() && e->offset % 2048 == 0, "the entry points past the original data, at a boundary");
            ok(e && e->size == mine.size(), "the entry has the replacement's size");
            ok(o->size(false) == pkd.size() + 2048, "the data grows by the replacement, padded to 2048");
            Index orig;
            parseIndex(pkh, pkd.size(), orig, why);
            ok(find(parsed, "shaders\\post_process.fx360")->offset == find(orig, "shaders\\post_process.fx360")->offset,
               "other entries keep their offsets");
            ok(readAll(*o, false, 0, uint32_t(pkd.size())) == pkd, "the original data reads as it was");
            if (e)
                ok(readAll(*o, false, e->offset, e->size) == mine, "the entry reads as the replacement");
            Bytes tail = readAll(*o, false, pkd.size() + mine.size(), 2048);
            ok(tail.size() == 2048 - mine.size(), "a read past the end is short");
            bool zeros = true;
            for (auto b : tail) zeros = zeros && b == 0;
            ok(zeros, "the padding after the replacement reads as zeros");
            ok(readAll(*o, false, o->size(false), 16).empty(), "a read at the end reads nothing");
            ok(readAll(*o, true, o->size(true) - 4, 16).size() == 4, "an index read past its end is short");

            ok(o->firstReads(0, 2048).empty(), "reading the original data reads no replacement");
            auto first = o->firstReads(pkd.size() + 10, 4);
            ok(first.size() == 1 && first[0] == "media\\PlayDatabase\\Offense\\Dagger.pd", "the first read of a replacement is named");
            ok(o->firstReads(pkd.size(), 64).empty(), "and only the first");
        }
    }

    // Two replacements: one larger than its entry, one empty; a folder with a space in its name
    {
        fs::path mods = g_dir / "two";
        Bytes large(3000);
        for (size_t i = 0; i < large.size(); i++) large[i] = uint8_t(255 - i % 251);
        writeFile(mods / "shaders" / "post_process.fx360", large);
        writeFile(mods / "media" / "ui" / "Final_XUI" / "single player" / "title.xui", {});
        std::vector<std::string> log;
        auto o = build(pack, mods, log);
        ok(o && o->replacements() == 2 && log.size() == 2, "two replacements");
        if (o)
        {
            Index parsed;
            std::string why;
            ok(parseIndex(readAll(*o, true, 0, uint32_t(pkh.size())), o->size(false), parsed, why), "the index parses");
            const Entry* t = find(parsed, "media\\ui\\Final_XUI\\Single Player\\Title.xui");
            const Entry* s = find(parsed, "shaders\\post_process.fx360");
            ok(t && s && t->size == 0 && s->size == large.size(), "sizes: the empty file and the larger one");
            ok(t && s && t->offset % 2048 == 0 && s->offset % 2048 == 0 && t->offset >= pkd.size() && s->offset >= pkd.size(),
               "both past the original data, at boundaries");
            ok(t && s && (t->offset + t->size <= s->offset || s->offset + s->size <= t->offset), "no overlap");
            ok(o->size(false) % 2048 == 0, "the data's size is a multiple of 2048");
            if (s)
            {
                ok(readAll(*o, false, s->offset, s->size) == large, "the larger replacement reads back whole");
                // From the end of the original data, through any padding, into the replacement
                uint64_t from = pkd.size() - 100;
                Bytes span = readAll(*o, false, from, uint32_t(s->offset + 200 - from));
                Bytes want(pkd.end() - 100, pkd.end());
                want.resize(s->offset - from, 0);
                want.insert(want.end(), large.begin(), large.begin() + 200);
                ok(span == want, "a read across the original data, padding and a replacement");
            }
        }
    }

    // Big-endian
    {
        fs::path bigPack = makePack("b", entries, true);
        fs::path mods = g_dir / "big";
        writeFile(mods / "media" / "PlayDatabase" / "Offense" / "Dagger.pd", text("mine"));
        std::vector<std::string> log;
        auto o = build(bigPack, mods, log);
        ok(o != nullptr, "big-endian archive: an overlay");
        if (o)
        {
            Index parsed;
            std::string why;
            Bytes ix = readAll(*o, true, 0, uint32_t(o->size(true)));
            ok(parseIndex(ix, o->size(false), parsed, why) && parsed.bigEndian, "rewritten big-endian");
            const Entry* e = find(parsed, "media\\PlayDatabase\\Offense\\Dagger.pd");
            ok(e && e->size == 4 && readAll(*o, false, e->offset, 4) == text("mine"), "and it reads the replacement");
        }
    }

    // An index that doesn't parse
    {
        fs::path bad = g_dir / "bad" / "Pack";
        Bytes junk = pkh;
        junk.resize(junk.size() - 3);
        writeFile(bad.string() + ".pkh", junk);
        writeFile(bad.string() + ".pkd", pkd);
        std::vector<std::string> log;
        ok(!build(bad, g_dir / "one", log), "a cut-short index: no overlay");
        ok(log.size() == 1 && log[0].find("can't be read") != std::string::npos, "and the log says why");
        Index parsed;
        std::string why;
        ok(!parseIndex(junk, pkd.size(), parsed, why) && !why.empty(), "parseIndex gives the reason");
        Bytes past = pkh;
        ok(!parseIndex(past, 100, parsed, why), "entries past the end of the data: refused");
    }

    // readEntry: what the game would read for one entry
    {
        const std::string h = pack.string() + ".pkh", d = pack.string() + ".pkd";
        std::string got = "stale";
        ok(readEntry(h, d, nullptr, "MEDIA\\playdatabase\\offense\\DAGGER.pd", got) && Bytes(got.begin(), got.end()) == play,
           "readEntry: the archive's own bytes, the name in another case");
        ok(readEntry(h, d, nullptr, "shaders\\post_process.fx360", got) && Bytes(got.begin(), got.end()) == big,
           "readEntry: an entry longer than a block");
        ok(!readEntry(h, d, nullptr, "media\\PlayDatabase\\Offense\\Nothing.pd", got) && got.empty(), "readEntry: no such entry");
        ok(!readEntry(g_dir / "nowhere.pkh", g_dir / "nowhere.pkd", nullptr, "shaders\\post_process.fx360", got), "readEntry: no such archive");
        std::vector<std::string> log;
        auto o = build(pack, g_dir / "one", log);
        ok(o && readEntry(h, d, o.get(), "media\\PlayDatabase\\Offense\\Dagger.pd", got) &&
               got == "<play name=\"Dagger\">\r\n  <route deep=\"1\"/>\r\n</play>\r\n",
           "readEntry through an overlay: the mods folder's replacement");
        ok(o && readEntry(h, d, o.get(), "media\\ui\\Final_XUI\\Single Player\\Title.xui", got) && Bytes(got.begin(), got.end()) == menu,
           "readEntry through an overlay: an entry that isn't replaced");
        fs::path withEmpty = makePack("e", { { "a.txt", {} }, { "b.txt", text("b") } });
        ok(readEntry(withEmpty.string() + ".pkh", withEmpty.string() + ".pkd", nullptr, "a.txt", got) && got.empty(), "readEntry: an empty entry");
    }

    fs::remove_all(g_dir);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
