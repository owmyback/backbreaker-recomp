#pragma once
// The title's achievement database: an XDBF ("SPA") blob that the XEX carries as a resource named
// after the title id (XEX_HEADER_RESOURCE_INFO). Header-only so tests/spa_test.cpp can check it on
// a hand-built blob without game files (issue #97).
//
// Layouts follow Xenia (xenia-canary src/xenia/kernel/xam/xdbf/xdbf_io.h, spa_info.h/.cc;
// BSD 3-clause, docs/licenses/xenia-LICENSE.txt). Everything is big-endian:
//   XDBF header: magic 'XDBF', version, entry_count, entry_used, free_count, free_used (6 x u32)
//   entry_count x { u16 section; u64 id; u32 offset; u32 size }    (18 bytes, packed)
//   free_count  x { u32 offset; u32 size }
//   data: offsets are relative to the end of the free table
// Sections: 1 metadata (ids 'XACH' achievements, 'XSTC' default language, 'XTHD' title header),
// 2 images (id = image id, PNG), 3 string tables (id = language, 1 = English).
// 'XACH' / 'XSTR' start with { u32 magic; u32 version; u32 size; u16 count } (14 bytes).
// An XACH record (0x24 bytes): u16 id, label string, description string, unachieved string;
// u32 image id; u16 gamerscore; u16 ?; u32 flags; 4 x u32 ?.
// An XSTR record: u16 string id, u16 length, then that many UTF-8 bytes (no terminator).
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace kernel::spa
{
    // Achievement flags (Xenia xdbf/gpd_info.h AchievementFlags)
    constexpr uint32_t kTypeMask = 0x7;
    constexpr uint32_t kShowUnachieved = 0x8; // the description shows while locked (else: secret)
    constexpr uint32_t kAchievedOnline = 0x10000;
    constexpr uint32_t kAchieved = 0x20000;

    struct Achievement
    {
        uint16_t id = 0;
        std::string label, description, unachieved; // UTF-8
        uint32_t imageId = 0;
        uint16_t gamerscore = 0;
        uint32_t flags = 0;
    };

    struct Info
    {
        uint32_t defaultLanguage = 1;
        uint32_t language = 0; // the string table the texts came from
        std::vector<Achievement> achievements;
        std::map<uint32_t, std::pair<uint32_t, uint32_t>> images; // image id -> (offset, size) in the blob
    };

    namespace detail
    {
        inline uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
        inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
        inline uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
        constexpr uint32_t fourcc(const char (&s)[5]) { return uint32_t(s[0]) << 24 | uint32_t(s[1]) << 16 | uint32_t(s[2]) << 8 | uint32_t(s[3]); }
    }

    // Parses the blob. Returns false (with a reason) if it is not a usable XDBF; a blob without an
    // achievement table is valid and just has none.
    inline bool parse(const uint8_t* data, size_t size, Info& out, std::string& error, uint32_t wantLanguage = 1)
    {
        using namespace detail;
        out = Info{};
        if (size < 24 || be32(data) != fourcc("XDBF"))
        {
            error = "no XDBF header";
            return false;
        }
        const uint32_t entryCount = be32(data + 8), entryUsed = be32(data + 12), freeCount = be32(data + 16);
        const uint64_t dataStart = 24 + uint64_t(entryCount) * 18 + uint64_t(freeCount) * 8;
        if (entryUsed > entryCount || dataStart > size)
        {
            error = "XDBF tables run past the end";
            return false;
        }
        struct Block { const uint8_t* p; uint32_t size; };
        std::map<std::pair<uint16_t, uint64_t>, Block> entries;
        for (uint32_t i = 0; i < entryUsed; i++)
        {
            const uint8_t* e = data + 24 + i * 18;
            const uint16_t section = be16(e);
            const uint64_t id = be64(e + 2);
            const uint32_t offset = be32(e + 10), length = be32(e + 14);
            if (dataStart + offset + length > size)
            {
                error = "an XDBF entry runs past the end";
                return false;
            }
            entries[{ section, id }] = { data + dataStart + offset, length };
            if (section == 2)
                out.images[uint32_t(id)] = { uint32_t(dataStart + offset), length };
        }
        auto find = [&](uint16_t section, uint64_t id) -> const Block* {
            auto it = entries.find({ section, id });
            return it == entries.end() ? nullptr : &it->second;
        };

        if (const Block* xstc = find(1, fourcc("XSTC")); xstc && xstc->size >= 16 && be32(xstc->p) == fourcc("XSTC"))
            out.defaultLanguage = be32(xstc->p + 12);

        // Strings: the language asked for, else the title's default, else the first table there is.
        std::map<uint16_t, std::string> strings;
        const Block* table = find(3, wantLanguage);
        out.language = wantLanguage;
        if (!table) { table = find(3, out.defaultLanguage); out.language = out.defaultLanguage; }
        if (!table)
            for (auto& [key, block] : entries)
                if (key.first == 3) { table = &block; out.language = uint32_t(key.second); break; }
        if (table)
        {
            if (table->size < 14 || be32(table->p) != fourcc("XSTR"))
            {
                error = "bad string table";
                return false;
            }
            const uint16_t count = be16(table->p + 12);
            uint32_t pos = 14;
            for (uint16_t i = 0; i < count; i++)
            {
                if (pos + 4 > table->size) { error = "string table runs past its end"; return false; }
                const uint16_t sid = be16(table->p + pos), length = be16(table->p + pos + 2);
                if (pos + 4 + length > table->size) { error = "a string runs past its table"; return false; }
                strings[sid].assign(reinterpret_cast<const char*>(table->p + pos + 4), length);
                pos += 4 + length;
            }
        }
        else
            out.language = 0;
        auto text = [&](uint16_t sid) {
            auto it = strings.find(sid);
            return it == strings.end() ? std::string() : it->second;
        };

        if (const Block* xach = find(1, fourcc("XACH")))
        {
            if (xach->size < 14 || be32(xach->p) != fourcc("XACH"))
            {
                error = "bad achievement table";
                return false;
            }
            const uint16_t count = be16(xach->p + 12);
            if (14 + uint64_t(count) * 0x24 > xach->size)
            {
                error = "achievement table runs past its end";
                return false;
            }
            for (uint16_t i = 0; i < count; i++)
            {
                const uint8_t* r = xach->p + 14 + i * 0x24;
                Achievement a;
                a.id = be16(r);
                a.label = text(be16(r + 2));
                a.description = text(be16(r + 4));
                a.unachieved = text(be16(r + 6));
                a.imageId = be32(r + 8);
                a.gamerscore = be16(r + 12);
                a.flags = be32(r + 16);
                out.achievements.push_back(std::move(a));
            }
        }
        return true;
    }

    // UTF-8 -> UTF-16 code units (the XAM structures hold UTF-16BE strings).
    inline std::u16string toUtf16(const std::string& s)
    {
        std::u16string out;
        for (size_t i = 0; i < s.size();)
        {
            uint32_t c = uint8_t(s[i]), n = 0;
            if (c < 0x80) n = 0;
            else if ((c >> 5) == 6) { c &= 0x1F; n = 1; }
            else if ((c >> 4) == 14) { c &= 0x0F; n = 2; }
            else if ((c >> 3) == 30) { c &= 0x07; n = 3; }
            else { out.push_back(u'?'); i++; continue; }
            if (n && i + n >= s.size()) { out.push_back(u'?'); break; } // truncated sequence
            for (uint32_t k = 1; k <= n; k++) c = (c << 6) | (uint8_t(s[i + k]) & 0x3F);
            i += n + 1;
            if (c >= 0x10000)
            {
                c -= 0x10000;
                out.push_back(char16_t(0xD800 + (c >> 10)));
                out.push_back(char16_t(0xDC00 + (c & 0x3FF)));
            }
            else
                out.push_back(char16_t(c));
        }
        return out;
    }
}
