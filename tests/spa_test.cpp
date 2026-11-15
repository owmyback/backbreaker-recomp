// Checks runtime/kernel/spa.h (issue #97): the achievement database parser, on XDBF blobs built
// here by hand (no game files). Build:
//   clang++ -std=c++20 -Wall -Wextra -O2 tests/spa_test.cpp -o build/spa_test
//
// The blob follows the layout in spa.h (Xenia xdbf_io.h / spa_info.h): a 24-byte header, 18-byte
// packed entries, 8-byte free entries, then data with offsets from the end of the free table.
#include "../runtime/kernel/spa.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace kernel::spa;

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

struct Bytes
{
    std::vector<uint8_t> b;
    void u8(uint32_t v) { b.push_back(uint8_t(v)); }
    void u16(uint32_t v) { u8(v >> 8); u8(v); }
    void u32(uint32_t v) { u16(v >> 16); u16(v); }
    void u64(uint64_t v) { u32(uint32_t(v >> 32)); u32(uint32_t(v)); }
    void str(const std::string& s) { for (char c : s) u8(uint8_t(c)); }
    void add(const Bytes& o) { b.insert(b.end(), o.b.begin(), o.b.end()); }
};

struct Section { uint16_t section; uint64_t id; Bytes data; };

static uint32_t fourcc(const char* s) { return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint8_t(s[3]); }

static Bytes xdbf(const std::vector<Section>& sections, uint32_t spareEntries = 2)
{
    Bytes out, data;
    const uint32_t entryCount = uint32_t(sections.size()) + spareEntries, freeCount = 1;
    out.u32(fourcc("XDBF"));
    out.u32(0x10000);
    out.u32(entryCount);
    out.u32(uint32_t(sections.size()));
    out.u32(freeCount);
    out.u32(1);
    for (auto& s : sections)
    {
        out.u16(s.section);
        out.u64(s.id);
        out.u32(uint32_t(data.b.size()));
        out.u32(uint32_t(s.data.b.size()));
        data.add(s.data);
    }
    for (uint32_t i = 0; i < spareEntries; i++) { out.u16(0); out.u64(0); out.u32(0); out.u32(0); }
    out.u32(uint32_t(data.b.size())); // free entry: end of data
    out.u32(0);
    out.add(data);
    return out;
}

struct Ach { uint16_t id, label, desc, unach; uint32_t image; uint16_t gs; uint32_t flags; };

static Bytes xach(const std::vector<Ach>& list)
{
    Bytes b;
    b.u32(fourcc("XACH"));
    b.u32(1);
    b.u32(uint32_t(2 + list.size() * 0x24));
    b.u16(uint32_t(list.size()));
    for (auto& a : list)
    {
        b.u16(a.id); b.u16(a.label); b.u16(a.desc); b.u16(a.unach);
        b.u32(a.image); b.u16(a.gs); b.u16(0); b.u32(a.flags);
        b.u32(0); b.u32(0); b.u32(0); b.u32(0);
    }
    return b;
}

static Bytes xstr(const std::vector<std::pair<uint16_t, std::string>>& strings)
{
    Bytes body;
    for (auto& [id, s] : strings) { body.u16(id); body.u16(uint32_t(s.size())); body.str(s); }
    Bytes b;
    b.u32(fourcc("XSTR"));
    b.u32(1);
    b.u32(uint32_t(2 + body.b.size()));
    b.u16(uint32_t(strings.size()));
    b.add(body);
    return b;
}

static Bytes xstc(uint32_t language)
{
    Bytes b;
    b.u32(fourcc("XSTC")); b.u32(1); b.u32(4); b.u32(language);
    return b;
}

int main()
{
    const std::vector<Ach> list = {
        { 1, 10, 11, 12, 7, 40, 0x9 },   // shown while locked (0x8), type 1
        { 2, 20, 21, 22, 6, 10, 0x4 },   // secret (no 0x8)
        { 29, 30, 31, 32, 6, 950, 0x0 }, // a label with no string: empty text
    };
    Bytes png; png.str("\x89PNG fake image bytes");
    const std::vector<std::pair<uint16_t, std::string>> english = {
        { 10, "Boys to Men" }, { 11, "Finish the drills." }, { 12, "Finish the drills (locked)." },
        { 20, "Caf\xC3\xA9 \xE2\x84\xA2" }, { 21, "Secret one" }, { 22, "Secret, locked" },
        { 31, "Third" },
    };
    const std::vector<std::pair<uint16_t, std::string>> french = { { 10, "Gar\xC3\xA7ons" } };

    // 1. A full blob: the achievements, their texts, flags, gamerscore, the image table.
    {
        Bytes blob = xdbf({ { 1, fourcc("XSTC"), xstc(1) }, { 1, fourcc("XACH"), xach(list) }, { 3, 1, xstr(english) },
                            { 3, 3, xstr(french) }, { 2, 7, png } });
        Info info;
        std::string error;
        expect(parse(blob.b.data(), blob.b.size(), info, error), "full blob parses: " + error);
        expect(info.achievements.size() == 3, "3 achievements");
        if (info.achievements.size() == 3)
        {
            const auto& a = info.achievements[0];
            expect(a.id == 1 && a.gamerscore == 40 && a.imageId == 7 && a.flags == 0x9, "achievement 1 numbers");
            expect(a.label == "Boys to Men" && a.description == "Finish the drills." && a.unachieved == "Finish the drills (locked).",
                   "achievement 1 texts: '" + a.label + "'");
            expect(info.achievements[1].label == "Caf\xC3\xA9 \xE2\x84\xA2", "UTF-8 text kept as is");
            expect(!(info.achievements[1].flags & kShowUnachieved), "achievement 2 is secret");
            expect(info.achievements[2].id == 29 && info.achievements[2].gamerscore == 950 && info.achievements[2].label.empty(),
                   "achievement 29: missing label string is empty");
        }
        expect(info.defaultLanguage == 1 && info.language == 1, "English (1) is default and used");
        expect(info.images.count(7) && info.images[7].second == png.b.size(), "image 7 located");
        if (info.images.count(7))
            expect(std::string((const char*)blob.b.data() + info.images[7].first, png.b.size()) == std::string(png.b.begin(), png.b.end()),
                   "image 7 offset points at its bytes");

        Info fr;
        expect(parse(blob.b.data(), blob.b.size(), fr, error, 3) && fr.language == 3 && fr.achievements[0].label == "Gar\xC3\xA7ons",
               "French (3) table when asked for");
        Info de;
        expect(parse(blob.b.data(), blob.b.size(), de, error, 2) && de.language == 1 && de.achievements[0].label == "Boys to Men",
               "a missing language falls back to the default");
    }

    // 2. No XACH: valid, no achievements. No string tables: texts empty.
    {
        Bytes blob = xdbf({ { 1, fourcc("XSTC"), xstc(1) } });
        Info info;
        std::string error;
        expect(parse(blob.b.data(), blob.b.size(), info, error) && info.achievements.empty(), "no XACH: no achievements");
        Bytes noStrings = xdbf({ { 1, fourcc("XACH"), xach(list) } });
        expect(parse(noStrings.b.data(), noStrings.b.size(), info, error) && info.achievements.size() == 3 &&
                   info.achievements[0].label.empty() && info.language == 0,
               "no string tables: empty texts");
    }

    // 3. Broken blobs are refused, never read past their end.
    {
        Info info;
        std::string error;
        Bytes blob = xdbf({ { 1, fourcc("XACH"), xach(list) }, { 3, 1, xstr(english) } });
        std::vector<uint8_t> bad = blob.b;
        bad[0] = 'Y';
        expect(!parse(bad.data(), bad.size(), info, error), "bad magic refused");
        for (size_t cut : { size_t(10), size_t(24), size_t(60), blob.b.size() - 1 })
            expect(!parse(blob.b.data(), cut, info, error), "truncated to " + std::to_string(cut) + " bytes refused");
        Ach many = list[0];
        Bytes shortTable = xach({ many });
        shortTable.b[13] = 50; // claims 50 records, has 1
        Bytes blob2 = xdbf({ { 1, fourcc("XACH"), shortTable } });
        expect(!parse(blob2.b.data(), blob2.b.size(), info, error), "XACH count beyond its size refused");
        Bytes longString = xstr({ { 10, "abc" } });
        longString.b[17] = 200; // string length 200 in a 3-byte string
        Bytes blob3 = xdbf({ { 3, 1, longString } });
        expect(!parse(blob3.b.data(), blob3.b.size(), info, error), "string past its table refused");
    }

    // 4. UTF-8 -> UTF-16 for the XAM structures.
    {
        expect(toUtf16("Abc") == u"Abc", "ASCII");
        expect(toUtf16("Caf\xC3\xA9") == u"Café", "2-byte sequence");
        expect(toUtf16("\xE2\x84\xA2") == u"™", "3-byte sequence");
        expect(toUtf16("\xF0\x9F\x8F\x88") == u"\U0001F3C8", "4-byte sequence -> surrogate pair");
        expect(toUtf16("ab\xE2\x84") == u"ab?", "truncated sequence -> ?");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
