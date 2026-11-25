#pragma once
// Per-entry overrides for the game's archive (#169). Most of the game is in one archive,
// packedfilesx360/PackedOutputX360: a .pkh index and a .pkd data file. A file in the mods folder
// named like one of its entries (mods/media/PlayDatabase/Offense/Dagger.pd for the entry
// media\PlayDatabase\Offense\Dagger.pd) replaces that entry, without repacking.
//
// The game's own reader is left alone. It reads the whole index once at boot into a table of
// { offset, size, name } (NmgPackedFileArchive's constructor, sub_82650A80) and then reads entries
// from the .pkd by those offsets. So the file layer shows it a rewritten index, where a replaced
// entry points past the end of the original data, and a .pkd that is the original with the
// replacement files appended there, each at a 2048-byte boundary as the shipped entries are.
// Nothing is copied: reads past the original data go to the replacement files themselves.
//
// Host-only (no guest memory), so tests/pack_overlay_test.cpp runs it on a made-up archive.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace kernel::pack
{
    // The index, as tools/pkd.py documents it: u32 count, u32 total name bytes, then per entry
    // u32 name length, the name (NUL-padded to that length), u32 offset, u32 size. Both byte
    // orders are tried, and one is accepted only when its entries end exactly at the end of the
    // index and every entry lies inside the data file (the shipped archive is little-endian).
    struct Entry
    {
        std::string name;       // as the game names it: media\PlayDatabase\Offense\Dagger.pd
        uint32_t offset = 0, size = 0;
        size_t field = 0;       // where the entry's offset is in the index; its size follows
    };
    struct Index
    {
        std::vector<Entry> entries;
        bool bigEndian = false;
    };
    // False, with the reason in `why`, when the index doesn't parse in either byte order.
    bool parseIndex(const std::vector<uint8_t>& pkh, uint64_t pkdSize, Index& out, std::string& why);

    constexpr uint64_t kAlign = 2048;

    class Overlay
    {
    public:
        // The archive at pkh/pkd with the files in `mods` that are named like its entries (matched
        // without regard to case, like the disc). Null when none are, or when the archive can't be
        // read; `log` gets a line for each replacement and for anything that was left out.
        // Each replacement starts at a 2048-byte boundary and takes at least one block, as entries do
        // in the shipped archive.
        static std::shared_ptr<Overlay> build(const std::filesystem::path& pkh, const std::filesystem::path& pkd,
                                              const std::filesystem::path& mods, std::vector<std::string>& log);
        ~Overlay();
        Overlay(const Overlay&) = delete;
        Overlay& operator=(const Overlay&) = delete;

        uint64_t size(bool index) const { return index ? m_index.size() : m_dataSize; }
        // Like pread on the rewritten index or on the data with the replacements appended: the bytes
        // at `offset`, fewer at the end, 0 past it. Gaps between files read as zeros.
        long read(bool index, uint8_t* dst, uint32_t len, uint64_t offset) const;
        // The entries whose replacement [offset, offset + len) of the data covers for the first
        // time, so the log can say when the game read each one.
        std::vector<std::string> firstReads(uint64_t offset, uint32_t len) const;
        size_t replacements() const { return m_files.size(); }

    private:
        struct Replacement
        {
            std::string entry;
            std::filesystem::path file;
            int fd = -1;
            uint64_t offset = 0, size = 0;  // where it sits in the data
        };
        Overlay() = default;
        std::vector<uint8_t> m_index;
        int m_dataFd = -1;
        uint64_t m_originalSize = 0, m_dataSize = 0;
        std::vector<Replacement> m_files;  // in offset order
        std::unique_ptr<std::atomic<bool>[]> m_read;
    };

    // One entry's bytes: from the archive at pkh/pkd, or through `overlay` when the archive has one,
    // so that a replacement from the mods folder is what comes back, as the game gets it. Names are
    // matched without regard to case. False when the archive can't be read or has no such entry.
    bool readEntry(const std::filesystem::path& pkh, const std::filesystem::path& pkd, const Overlay* overlay,
                   const std::string& name, std::string& out);
}
