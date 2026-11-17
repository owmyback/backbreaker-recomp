// An Xbox 360 disc image's file system (XDVDFS), read-only: the C++ form of tools/xdvdfs.py for the
// installer (#138). Same partition offsets, same walk order (so `backbreaker-installer list` prints
// what docs/disc-listing.txt shows), and every read is bounds-checked, so a cut-short or damaged
// image gives a message instead of a short file. Error messages never contain the image's path.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace installer
{
    struct DiscFile
    {
        std::string path; // "/media/sounds/...", '/'-separated, from the root
        uint32_t sector = 0;
        uint32_t size = 0;
    };

    class DiscImage
    {
    public:
        DiscImage() = default;
        DiscImage(const DiscImage&) = delete;
        DiscImage& operator=(const DiscImage&) = delete;
        ~DiscImage();

        enum class OpenResult { Ok, CannotOpen, NotXbox360 };
        // On NotXbox360, error says what the file looks like instead, when that is recognisable
        OpenResult open(const std::string& path, std::string& error);

        uint64_t partitionOffset() const { return partition_; }
        uint64_t imageSize() const { return size_; }

        // Every file, in tools/xdvdfs.py's order. Fails on a directory that lies outside the image.
        bool list(std::vector<DiscFile>& files, std::string& error) const;
        // Where a file's data ends in the image; past imageSize() means a cut-short image
        uint64_t end(const DiscFile& file) const;

        bool read(const DiscFile& file, std::vector<uint8_t>& data, std::string& error) const;
        // Writes the file to dest (directories created), reporting bytes as they are written
        bool extract(const DiscFile& file, const std::string& dest, std::string& error,
                     const std::function<void(uint64_t)>& written = {}) const;

    private:
        bool readAt(uint64_t offset, void* data, size_t size) const;
        bool walk(uint32_t sector, uint32_t size, const std::string& path, std::vector<DiscFile>& files,
                  int depth, std::string& error) const;

        int fd_ = -1;
        uint64_t partition_ = 0;
        uint64_t size_ = 0;
    };
}
