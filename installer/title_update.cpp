#include "title_update.h"
#include "build.h"
#include "crypto.h"
#include "game_check.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace installer
{
    const char kTitleUpdate2PatchSha256[] = "8511013dd9bedb114b7fbfeadfb780c631e0e26e2ec1946f11d680e4ea91784a";
    const char* const kTitleUpdateFiles[3] = {"default.xexp", "PatchPackedOutputX360.pkh", "PatchPackedOutputX360.pkd"};

    namespace
    {
        constexpr size_t kBlock = 0x1000;
        constexpr uint32_t kBlocksPerTable = 0xAA;  // data blocks one hash table covers
        constexpr uint32_t kBlocksPerLevel1 = 0x70E4; // ... and one level-1 table (170 * 170)
        constexpr size_t kMaxPackage = 256u << 20;  // the update is 4 MB; nothing near this is one

        bool be32(const std::vector<uint8_t>& d, size_t at, uint32_t& v)
        {
            if (at + 4 > d.size()) return false;
            v = uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3];
            return true;
        }
        uint32_t le24(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16; }

        std::string lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return s;
        }

        bool readWhole(const std::string& path, std::vector<uint8_t>& out)
        {
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            if (!f) return false;
            const std::streamoff size = f.tellg();
            if (size < 0 || size_t(size) > kMaxPackage) return false;
            out.resize(size_t(size));
            f.seekg(0);
            f.read(reinterpret_cast<char*>(out.data()), size);
            return bool(f);
        }

        bool writeWhole(const std::string& path, const void* data, size_t size)
        {
            std::ofstream f(path + ".tmp", std::ios::binary | std::ios::trunc);
            f.write(static_cast<const char*>(data), std::streamsize(size));
            f.close();
            if (!f) return false;
            std::error_code ec;
            fs::rename(fs::path(path + ".tmp"), fs::path(path), ec);
            return !ec;
        }

        std::string sha256Hex(const std::vector<uint8_t>& data)
        {
            Sha256 h;
            h.update(data.data(), data.size());
            const auto digest = h.finish();
            return toHex(digest.data(), digest.size());
        }
    }

    bool StfsPackage::parse(std::vector<uint8_t> data, std::string& error)
    {
        data_ = std::move(data);
        entries_.clear();
        if (data_.size() < 0x971A || (std::memcmp(data_.data(), "LIVE", 4) && std::memcmp(data_.data(), "PIRS", 4) &&
                                      std::memcmp(data_.data(), "CON ", 4))) {
            error = "not an Xbox 360 package (a title update's file starts with LIVE)";
            return false;
        }
        uint32_t headerSize = 0;
        be32(data_, 0x340, headerSize);
        be32(data_, 0x360, titleId_);
        displayName_.clear();
        for (size_t i = 0x411; i + 1 < 0x511; i += 2) { // UTF-16BE; the names here are ASCII
            const unsigned c = unsigned(data_[i]) << 8 | data_[i + 1];
            if (!c) break;
            displayName_ += c >= 0x20 && c < 0x7F ? char(c) : '?';
        }
        const size_t vd = 0x379; // the volume descriptor
        shift_ = (~data_[vd + 2]) & 1;
        first_ = (size_t(headerSize) + 0xFFF) & ~size_t(0xFFF);
        const uint32_t tableBlocks = uint32_t(data_[vd + 3]) | uint32_t(data_[vd + 4]) << 8;
        const uint32_t tableStart = le24(&data_[vd + 5]);
        std::vector<uint8_t> table;
        if (!readBlocks(tableStart, tableBlocks, size_t(tableBlocks) * kBlock, table)) {
            error = "a damaged package (its file table is cut short)";
            return false;
        }
        struct Raw { std::string name; int parent; };
        std::vector<Raw> raw;
        for (size_t i = 0; i + 0x40 <= table.size(); i += 0x40) {
            const uint8_t* e = &table[i];
            if (!e[0]) break;
            const uint8_t flags = e[0x28];
            StfsEntry entry;
            entry.folder = flags & 0x80;
            entry.blocks = le24(e + 0x29);
            entry.firstBlock = le24(e + 0x2F);
            entry.size = uint32_t(e[0x34]) << 24 | uint32_t(e[0x35]) << 16 | uint32_t(e[0x36]) << 8 | e[0x37];
            raw.push_back({std::string(reinterpret_cast<const char*>(e), flags & 0x3F), int16_t(uint16_t(e[0x32]) << 8 | e[0x33])});
            entries_.push_back(entry);
        }
        for (size_t i = 0; i < entries_.size(); i++) {
            std::string path = raw[i].name;
            int parent = raw[i].parent;
            for (int depth = 0; parent >= 0 && depth < 32; depth++) { // a loop of parents ends at 32
                if (size_t(parent) >= raw.size()) {
                    error = "a damaged package (its file table names a folder that isn't there)";
                    return false;
                }
                path = raw[size_t(parent)].name + "/" + path;
                parent = raw[size_t(parent)].parent;
            }
            entries_[i].path = path;
        }
        return true;
    }

    bool StfsPackage::blockOffset(uint32_t block, size_t& offset) const
    {
        uint64_t n = (uint64_t((block + kBlocksPerTable) / kBlocksPerTable) << shift_) + block;
        if (block >= kBlocksPerTable) n += uint64_t((block + kBlocksPerLevel1) / kBlocksPerLevel1) << shift_;
        if (block >= kBlocksPerLevel1) n += uint64_t(1) << shift_;
        const uint64_t at = n * kBlock + first_;
        if (at + kBlock > data_.size()) return false;
        offset = size_t(at);
        return true;
    }

    bool StfsPackage::nextBlock(uint32_t block, uint32_t& next) const
    {
        uint64_t n = 0;
        if (block >= kBlocksPerTable) {
            n = uint64_t(block / kBlocksPerTable) * (shift_ ? 0xAC : 0xAB) + (uint64_t(block / kBlocksPerLevel1 + 1) << shift_);
            if (block / kBlocksPerLevel1) n += uint64_t(1) << shift_;
        }
        const uint64_t table = n * kBlock + first_;
        int best = -1;
        for (uint32_t t = 0; t <= shift_; t++) { // the table in use, of a pair
            const uint64_t o = table + t * kBlock + uint64_t(block % kBlocksPerTable) * 0x18;
            if (o + 0x18 > data_.size()) continue;
            const int state = data_[size_t(o) + 0x14] >> 6;
            if (state > best) {
                best = state;
                next = uint32_t(data_[size_t(o) + 0x15]) << 16 | uint32_t(data_[size_t(o) + 0x16]) << 8 | data_[size_t(o) + 0x17];
            }
        }
        return best >= 0;
    }

    bool StfsPackage::readBlocks(uint32_t first, uint32_t blocks, size_t size, std::vector<uint8_t>& out) const
    {
        if (size > size_t(blocks) * kBlock || size_t(blocks) * kBlock > data_.size()) return false;
        out.clear();
        out.reserve(size);
        uint32_t block = first;
        for (uint32_t i = 0; i < blocks; i++) {
            size_t at = 0;
            if (!blockOffset(block, at)) return false;
            const size_t take = std::min(kBlock, size - out.size());
            out.insert(out.end(), data_.begin() + long(at), data_.begin() + long(at + take));
            if (i + 1 < blocks && !nextBlock(block, block)) return false;
        }
        return out.size() == size;
    }

    const StfsEntry* StfsPackage::find(const std::string& path) const
    {
        const std::string want = lower(path);
        for (const StfsEntry& e : entries_)
            if (!e.folder && lower(e.path) == want) return &e;
        return nullptr;
    }

    bool StfsPackage::read(const StfsEntry& entry, std::vector<uint8_t>& out, std::string& error) const
    {
        if (!readBlocks(entry.firstBlock, entry.blocks, entry.size, out)) {
            error = "a damaged package (" + entry.path + " in it is cut short)";
            return false;
        }
        return true;
    }

    bool looksLikeStfs(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary);
        char magic[4] = {};
        f.read(magic, 4);
        return f && (!std::memcmp(magic, "LIVE", 4) || !std::memcmp(magic, "PIRS", 4) || !std::memcmp(magic, "CON ", 4));
    }

    std::string updateFolder() { return dataHome() + "/backbreaker-recomp/update"; }

    bool readTitleUpdate(const std::string& path, std::vector<uint8_t>& patch, std::vector<uint8_t>& index,
                         std::vector<uint8_t>& data, std::string& error)
    {
        std::vector<uint8_t> bytes;
        if (!readWhole(path, bytes)) {
            error = "can't be read";
            return false;
        }
        // Every error here completes "the file ..." or "<name> in the update folder ..."
        StfsPackage package;
        if (!package.parse(std::move(bytes), error)) {
            error = "is " + error;
            return false;
        }
        if (package.titleId() != kBackbreakerTitleId) {
            char id[16];
            std::snprintf(id, sizeof id, "%08X", package.titleId());
            error = std::string("is a package for another game (title ID ") + id + ", Backbreaker's is 464F07DA)";
            return false;
        }
        std::vector<uint8_t>* into[3] = {&patch, &index, &data};
        for (int i = 0; i < 3; i++) {
            const StfsEntry* e = package.find(kTitleUpdateFiles[i]);
            if (!e) {
                error = std::string("is a Backbreaker package without ") + kTitleUpdateFiles[i] + ": not Title Update 2";
                return false;
            }
            if (!package.read(*e, *into[i], error)) {
                error = "is " + error;
                return false;
            }
        }
        if (sha256Hex(patch) != kTitleUpdate2PatchSha256) {
            error = "holds a patch this installer doesn't know (not Title Update 2, or a damaged copy)";
            return false;
        }
        return true;
    }

    UpdateResult syncTitleUpdate(const std::string& folder, const std::string& gameDir)
    {
        UpdateResult result;
        std::error_code ec;
        std::vector<std::string> candidates;
        for (auto it = fs::directory_iterator(fs::path(folder), ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec) && looksLikeStfs(it->path().string())) candidates.push_back(it->path().string());
        if (candidates.empty()) return result;
        std::sort(candidates.begin(), candidates.end());

        const std::string dir = gameDir + "/extracted/tu2";
        const std::string stampPath = dir + "/.complete";
        const std::string want = std::string(kTitleUpdate2PatchSha256) + "\n";
        std::vector<uint8_t> patch, index, data;
        std::string firstError, firstName;
        for (const std::string& path : candidates) {
            std::string error;
            if (!readTitleUpdate(path, patch, index, data, error)) {
                if (firstError.empty()) {
                    firstError = error;
                    firstName = fs::path(path).filename().string();
                }
                continue;
            }
            // Already extracted from this patch, and all three files still there?
            std::vector<uint8_t> have;
            bool current = readWhole(stampPath, have) && std::string(have.begin(), have.end()) == want;
            for (const char* name : kTitleUpdateFiles) current = current && fs::exists(fs::path(dir + "/" + name), ec);
            if (!current) {
                fs::create_directories(fs::path(dir), ec);
                fs::remove(fs::path(stampPath), ec);
                const std::vector<uint8_t>* from[3] = {&patch, &index, &data};
                for (int i = 0; i < 3; i++)
                    if (!writeWhole(dir + "/" + kTitleUpdateFiles[i], from[i]->data(), from[i]->size())) {
                        result.status = UpdateStatus::Unusable;
                        result.message = std::string("can't write the title update's files (") + kTitleUpdateFiles[i] + ") where the game is installed";
                        return result;
                    }
                // The patched executable and its image are made from these by the build
                fs::remove(fs::path(dir + "/default.xex"), ec);
                fs::remove(fs::path(dir + "/default.pe"), ec);
                if (!writeWhole(stampPath, want.data(), want.size())) {
                    result.status = UpdateStatus::Unusable;
                    result.message = "can't write the title update's files where the game is installed";
                    return result;
                }
                result.extracted = true;
            }
            result.status = UpdateStatus::Ready;
            result.message = "Title Update 2 (" + fs::path(path).filename().string() + ")";
            return result;
        }
        result.status = UpdateStatus::Unusable;
        result.message = firstName + " in the update folder " + firstError;
        return result;
    }
}
