#include "disc.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <set>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace installer
{
    namespace
    {
        constexpr uint64_t kSector = 2048;
        constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA"; // 20 bytes at sector 32 of the partition
        // Where the game partition starts in the image formats in use: a plain XDVDFS image, XGD2,
        // XGD1 and XGD3 (the same list tools/xdvdfs.py and scripts/setup.sh probe)
        constexpr uint64_t kPartitions[] = {0x0, 0xFD90000, 0x2080000, 0x18300000};

        uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
        uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

        // Names are bytes; tools/xdvdfs.py decodes them as Latin-1, which this matches in UTF-8
        std::string latin1ToUtf8(const uint8_t* p, size_t n)
        {
            std::string s;
            for (size_t i = 0; i < n; i++) {
                if (p[i] < 0x80) {
                    s += char(p[i]);
                } else {
                    s += char(0xC0 | p[i] >> 6);
                    s += char(0x80 | (p[i] & 0x3F));
                }
            }
            return s;
        }

        // What a file that isn't an Xbox 360 disc image probably is, for the message
        std::string whatItIs(const uint8_t* head, size_t n, uint64_t size)
        {
            auto starts = [&](const char* magic, size_t len) { return n >= len && std::memcmp(head, magic, len) == 0; };
            if (starts("PK\x03\x04", 4)) return "it is a ZIP archive: extract the disc image from it first";
            if (starts("7z\xBC\xAF\x27\x1C", 6)) return "it is a 7-Zip archive: extract the disc image from it first";
            if (starts("Rar!", 4)) return "it is a RAR archive: extract the disc image from it first";
            if (starts("CON ", 4) || starts("LIVE", 4) || starts("PIRS", 4))
                return "it is an Xbox 360 content package (Games on Demand or a download), not a disc image";
            if (size < 64 * kSector) return "it is too small to be a disc image";
            return "there is no Xbox 360 file system at any of the usual places in it";
        }
    }

    DiscImage::~DiscImage()
    {
        if (fd_ >= 0) close(fd_);
    }

    bool DiscImage::readAt(uint64_t offset, void* data, size_t size) const
    {
        if (offset > size_ || size > size_ - offset) return false;
        uint8_t* p = static_cast<uint8_t*>(data);
        while (size) {
            const ssize_t n = pread(fd_, p, size, off_t(offset));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            p += n;
            offset += uint64_t(n);
            size -= size_t(n);
        }
        return true;
    }

    DiscImage::OpenResult DiscImage::open(const std::string& path, std::string& error)
    {
        fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        struct stat st {};
        if (fd_ < 0 || fstat(fd_, &st) != 0) {
            error = std::string("the image can't be opened (") + std::strerror(errno) + ")";
            return OpenResult::CannotOpen;
        }
        if (S_ISDIR(st.st_mode)) {
            error = "that is a folder, not a disc image file";
            return OpenResult::CannotOpen;
        }
        size_ = uint64_t(st.st_size);
        for (uint64_t at : kPartitions) {
            char magic[20];
            if (readAt(at + 32 * kSector, magic, sizeof(magic)) && std::memcmp(magic, kMagic, 20) == 0) {
                partition_ = at;
                return OpenResult::Ok;
            }
        }
        uint8_t head[8] = {};
        const size_t n = size_ < sizeof(head) ? size_t(size_) : sizeof(head);
        readAt(0, head, n);
        error = "not an Xbox 360 disc image: " + whatItIs(head, n, size_);
        return OpenResult::NotXbox360;
    }

    bool DiscImage::walk(uint32_t sector, uint32_t size, const std::string& path, std::vector<DiscFile>& files,
                         int depth, std::string& error) const
    {
        if (depth > 64) {
            error = "the disc's folders nest too deep: the image is damaged";
            return false;
        }
        std::vector<uint8_t> d(size);
        if (!readAt(partition_ + uint64_t(sector) * kSector, d.data(), d.size())) {
            error = "a folder of the disc lies outside the image: the image is damaged or cut short";
            return false;
        }
        // A directory is a binary tree of entries at 4-byte offsets; tools/xdvdfs.py walks it with a
        // stack (right subtree first) and recurses into a subdirectory as soon as it meets it
        std::vector<uint32_t> stack{0};
        std::set<size_t> seen;
        while (!stack.empty()) {
            const size_t o = size_t(stack.back()) * 4;
            stack.pop_back();
            if (seen.count(o) || o + 14 > d.size()) continue;
            seen.insert(o);
            const uint8_t* e = d.data() + o;
            const uint16_t left = le16(e), right = le16(e + 2);
            const uint32_t start = le32(e + 4), length = le32(e + 8);
            const uint8_t attributes = e[12], nameLength = e[13];
            if (left == 0xFFFF) continue;
            const size_t n = std::min<size_t>(nameLength, d.size() - (o + 14));
            const std::string p = path + "/" + latin1ToUtf8(e + 14, n);
            if (attributes & 0x10) {
                if (!walk(start, length, p, files, depth + 1, error)) return false;
            } else {
                files.push_back({p, start, length});
            }
            if (left) stack.push_back(left);
            if (right) stack.push_back(right);
        }
        return true;
    }

    bool DiscImage::list(std::vector<DiscFile>& files, std::string& error) const
    {
        uint8_t root[8];
        if (!readAt(partition_ + 32 * kSector + 20, root, sizeof(root))) {
            error = "the disc's header is cut short: the image is damaged";
            return false;
        }
        files.clear();
        return walk(le32(root), le32(root + 4), "", files, 0, error);
    }

    uint64_t DiscImage::end(const DiscFile& file) const
    {
        return partition_ + uint64_t(file.sector) * kSector + file.size;
    }

    bool DiscImage::read(const DiscFile& file, std::vector<uint8_t>& data, std::string& error) const
    {
        data.resize(file.size);
        if (!readAt(partition_ + uint64_t(file.sector) * kSector, data.data(), data.size())) {
            error = file.path + " runs past the end of the image: the image is cut short or damaged";
            return false;
        }
        return true;
    }

    bool DiscImage::extract(const DiscFile& file, const std::string& dest, std::string& error,
                            const std::function<void(uint64_t)>& written) const
    {
        if (end(file) > size_) {
            error = file.path + " runs past the end of the image: the image is cut short or damaged";
            return false;
        }
        std::error_code ec;
        const std::filesystem::path out(dest);
        if (out.has_parent_path()) std::filesystem::create_directories(out.parent_path(), ec);
        const std::string tmp = dest + ".part";
        const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            error = "can't write " + dest + " (" + std::strerror(errno) + ")";
            return false;
        }
        std::vector<uint8_t> buffer(16 << 20);
        uint64_t at = partition_ + uint64_t(file.sector) * kSector, left = file.size;
        bool ok = true;
        while (left && ok) {
            const size_t n = left < buffer.size() ? size_t(left) : buffer.size();
            ok = readAt(at, buffer.data(), n);
            if (!ok) {
                error = "reading " + file.path + " from the image failed";
                break;
            }
            for (size_t w = 0; w < n && ok;) {
                const ssize_t k = ::write(fd, buffer.data() + w, n - w);
                if (k < 0 && errno == EINTR) continue;
                if (k <= 0) {
                    ok = false;
                    error = "can't write " + dest + " (" + std::strerror(errno) + ")";
                } else {
                    w += size_t(k);
                }
            }
            at += n;
            left -= n;
            if (ok && written) written(n);
        }
        if (::close(fd) != 0 && ok) {
            ok = false;
            error = "can't write " + dest + " (" + std::strerror(errno) + ")";
        }
        if (ok) std::filesystem::rename(tmp, dest, ec);
        if (!ok || ec) {
            if (ok) error = "can't write " + dest + " (" + ec.message() + ")";
            std::filesystem::remove(tmp, ec);
            return false;
        }
        return true;
    }
}
