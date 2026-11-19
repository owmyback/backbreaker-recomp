// Per-entry overrides for the game's archive (#169): see pack_overlay.h.
#include "pack_overlay.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include "../platform.h"

#include <fcntl.h>
#include <fstream>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace kernel::pack
{
    namespace
    {
        uint32_t get32(const std::vector<uint8_t>& b, size_t p, bool big)
        {
            const uint8_t* q = b.data() + p;
            return big ? uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | q[3]
                       : uint32_t(q[3]) << 24 | uint32_t(q[2]) << 16 | uint32_t(q[1]) << 8 | q[0];
        }

        void put32(std::vector<uint8_t>& b, size_t p, uint32_t v, bool big)
        {
            for (int i = 0; i < 4; i++)
                b[p + i] = uint8_t(v >> (big ? 24 - 8 * i : 8 * i));
        }

        std::string lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        }

        uint64_t alignUp(uint64_t v) { return (v + kAlign - 1) / kAlign * kAlign; }

        // One byte order's reading; false with the reason when it isn't self-consistent
        bool parseAs(const std::vector<uint8_t>& pkh, uint64_t pkdSize, bool big, Index& out, std::string& why)
        {
            uint32_t count = get32(pkh, 0, big), nameTotal = get32(pkh, 4, big);
            if (count == 0 || uint64_t(count) * 12 > pkh.size())
            {
                why = std::to_string(count) + " entries can't fit in " + std::to_string(pkh.size()) + " index bytes";
                return false;
            }
            out.entries.clear();
            out.entries.reserve(count);
            out.bigEndian = big;
            size_t p = 8;
            uint64_t names = 0;
            for (uint32_t i = 0; i < count; i++)
            {
                if (p + 4 > pkh.size()) { why = "the entry list runs past the end of the index"; return false; }
                uint32_t len = get32(pkh, p, big);
                p += 4;
                if (len == 0 || len > pkh.size() - p)
                {
                    why = "a name length of " + std::to_string(len) + " doesn't fit in the index";
                    return false;
                }
                Entry e;
                const char* s = (const char*)pkh.data() + p;
                e.name.assign(s, strnlen(s, len));
                names += len;
                p += len;
                if (p + 8 > pkh.size()) { why = "an entry's offset and size run past the end of the index"; return false; }
                e.field = p;
                e.offset = get32(pkh, p, big);
                e.size = get32(pkh, p + 4, big);
                p += 8;
                if (uint64_t(e.offset) + e.size > pkdSize)
                {
                    why = e.name + " claims " + std::to_string(e.size) + " bytes at " + std::to_string(e.offset) +
                          ", past the end of the " + std::to_string(pkdSize) + "-byte data file";
                    return false;
                }
                out.entries.push_back(std::move(e));
            }
            if (p != pkh.size())
            {
                why = std::to_string(count) + " entries cover " + std::to_string(p) + " of " + std::to_string(pkh.size()) + " index bytes";
                return false;
            }
            if (names != nameTotal)
            {
                why = "the header says " + std::to_string(nameTotal) + " bytes of names, the entries hold " + std::to_string(names);
                return false;
            }
            return true;
        }

        // pread until `len` bytes or the end of the file
        uint64_t readFully(int fd, uint8_t* dst, uint64_t len, uint64_t at)
        {
            uint64_t got = 0;
            while (got < len)
            {
                ssize_t n = pread(fd, dst + got, len - got, off_t(at + got));
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                got += uint64_t(n);
            }
            return got;
        }
    }

    bool parseIndex(const std::vector<uint8_t>& pkh, uint64_t pkdSize, Index& out, std::string& why)
    {
        if (pkh.size() < 8) { why = "the index is too short to hold a header"; return false; }
        std::string little, big;
        if (parseAs(pkh, pkdSize, false, out, little)) return true;
        if (parseAs(pkh, pkdSize, true, out, big)) return true;
        out.entries.clear();
        why = "it parses in neither byte order (little-endian: " + little + "; big-endian: " + big + ")";
        return false;
    }

    std::shared_ptr<Overlay> Overlay::build(const fs::path& pkh, const fs::path& pkd, const fs::path& mods, std::vector<std::string>& log)
    {
        std::error_code ec;
        if (mods.empty() || !fs::is_directory(mods, ec))
            return nullptr;

        // Every file in the mods folder, by its path there in lowercase with the game's backslashes.
        // packedfilesx360/ holds whole-archive replacements, which the file layer already swaps in.
        std::map<std::string, fs::path> byName;
        {
            std::vector<fs::path> files;
            for (auto it = fs::recursive_directory_iterator(mods, fs::directory_options::follow_directory_symlink |
                                                                      fs::directory_options::skip_permission_denied, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file(ec))
                    files.push_back(it->path());
            std::sort(files.begin(), files.end());
            for (auto& f : files)
            {
                std::string rel = f.lexically_relative(mods).generic_string();
                std::replace(rel.begin(), rel.end(), '/', '\\');
                rel = lower(rel);
                if (rel.rfind("packedfilesx360\\", 0) == 0)
                    continue;
                byName.emplace(rel, f); // two files differing only in case: the first, as sorted
            }
        }
        if (byName.empty())
            return nullptr;

        std::string archive = pkh.stem().string();
        std::vector<uint8_t> index;
        {
            std::ifstream in(pkh, std::ios::binary);
            index.assign(std::istreambuf_iterator<char>(in), {});
        }
        struct stat st{};
        if (index.empty() || stat(pkd.string().c_str(), &st) != 0)
        {
            log.push_back("[mods] can't read " + archive + "'s index or data, so its entries can't be replaced");
            return nullptr;
        }
        Index parsed;
        std::string why;
        if (!parseIndex(index, uint64_t(st.st_size), parsed, why))
        {
            log.push_back("[mods] " + archive + "'s index can't be read, so its entries can't be replaced: " + why);
            return nullptr;
        }

        std::shared_ptr<Overlay> o(new Overlay());
        o->m_originalSize = uint64_t(st.st_size);
        uint64_t end = alignUp(o->m_originalSize);
        for (auto& e : parsed.entries)
        {
            auto found = byName.find(lower(e.name));
            if (found == byName.end())
                continue;
            int fd = ::open(found->second.string().c_str(), O_RDONLY | O_CLOEXEC);
            struct stat fst{};
            if (fd < 0 || fstat(fd, &fst) != 0)
            {
                log.push_back("[mods] can't read " + found->second.string() + ": " + strerror(errno));
                if (fd >= 0) close(fd);
                continue;
            }
            uint64_t size = uint64_t(fst.st_size);
            // The index holds 32-bit offsets and sizes
            if (end + size > 0xFFFFFFFFull)
            {
                log.push_back("[mods] " + found->second.string() + " doesn't fit: with it, " + archive +
                              " would be larger than 4 GB, which its index can't address");
                close(fd);
                continue;
            }
            put32(index, e.field, uint32_t(end), parsed.bigEndian);
            put32(index, e.field + 4, uint32_t(size), parsed.bigEndian);
            o->m_files.push_back({ e.name, found->second, fd, end, size });
            log.push_back("[mods] " + found->second.string() + " replaces " + e.name + " in " + archive);
            end = alignUp(end + std::max<uint64_t>(size, 1)); // a block each, an empty one too, as the shipped archive has
        }
        if (o->m_files.empty())
            return nullptr;
        o->m_dataFd = ::open(pkd.string().c_str(), O_RDONLY | O_CLOEXEC);
        if (o->m_dataFd < 0)
        {
            log.push_back("[mods] can't open " + pkd.string() + ": " + strerror(errno));
            return nullptr;
        }
        o->m_index = std::move(index);
        o->m_dataSize = end;
        o->m_read = std::make_unique<std::atomic<bool>[]>(o->m_files.size());
        return o;
    }

    Overlay::~Overlay()
    {
        for (auto& f : m_files)
            if (f.fd >= 0) close(f.fd);
        if (m_dataFd >= 0) close(m_dataFd);
    }

    long Overlay::read(bool index, uint8_t* dst, uint32_t len, uint64_t offset) const
    {
        uint64_t total = size(index);
        if (offset >= total)
            return 0;
        uint64_t n = std::min<uint64_t>(len, total - offset);
        if (index)
        {
            std::memcpy(dst, m_index.data() + offset, n);
            return long(n);
        }
        uint64_t done = 0;
        while (done < n)
        {
            uint64_t pos = offset + done, chunk;
            if (pos < m_originalSize)
            {
                // The original data: a failed read ends the read short, as pread would
                chunk = std::min(n - done, m_originalSize - pos);
                uint64_t got = readFully(m_dataFd, dst + done, chunk, pos);
                if (got < chunk)
                    return long(done + got);
            }
            else
            {
                // The first replacement that ends after pos: pos is in it, or in the gap before it
                auto it = std::upper_bound(m_files.begin(), m_files.end(), pos,
                                           [](uint64_t p, const Replacement& r) { return p < r.offset + r.size; });
                if (it != m_files.end() && pos >= it->offset)
                {
                    chunk = std::min(n - done, it->offset + it->size - pos);
                    // A file that got shorter since the game started reads as zeros past its end
                    uint64_t got = readFully(it->fd, dst + done, chunk, pos - it->offset);
                    std::memset(dst + done + got, 0, chunk - got);
                }
                else
                {
                    chunk = std::min(n - done, (it != m_files.end() ? it->offset : total) - pos);
                    std::memset(dst + done, 0, chunk);
                }
            }
            done += chunk;
        }
        return long(n);
    }

    std::vector<std::string> Overlay::firstReads(uint64_t offset, uint32_t len) const
    {
        std::vector<std::string> out;
        for (size_t i = 0; i < m_files.size(); i++)
        {
            const auto& f = m_files[i];
            if (offset < f.offset + std::max<uint64_t>(f.size, 1) && f.offset < offset + len && !m_read[i].exchange(true))
                out.push_back(f.entry);
        }
        return out;
    }
}
