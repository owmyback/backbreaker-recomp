// Title Update 2 for the installer (docs/title-update.md): a patch Backbreaker got on Xbox
// Live. The player gives the installer the update's package, the file Xbox Live delivered
// (TU_134U1UQ_0000008000000.00000000000G2, an STFS package), and the game is then built from the
// disc's executable with the update's patch applied, and reads the update's own archive.
//
// The package lives in the player's update folder, <data home>/backbreaker-recomp/update (next to
// mods/ and saves/): the `title-update` command copies it there, or the player does. While a
// usable package is in that folder the game is the updated one; take it out and the next `play`
// goes back to the disc's version. Both builds' compiled code is kept, so switching only links.
// Only this program looks in the folder: the menu entry and the Steam shortcut start the game's
// program itself, the version linked last.
//
// StfsPackage is tools/stfs.py in C++: layout after Free60's STFS notes and Velocity's
// StfsPackage. Every offset read from the file is bounds-checked; the hashes are not verified,
// since what matters is checked afterwards: the patch inside must be the one this project's
// config and runtime are made for (its SHA-256, like the disc's executable in game_check.h).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace installer
{
    struct StfsEntry
    {
        std::string path; // folders joined with '/'
        uint32_t size = 0;
        bool folder = false;
        uint32_t firstBlock = 0, blocks = 0;
    };

    class StfsPackage
    {
    public:
        // False with `error` (what the bytes are instead: "not an Xbox 360 package ...", "a damaged
        // package ...") when they are not an STFS package or its file table is damaged
        bool parse(std::vector<uint8_t> data, std::string& error);
        uint32_t titleId() const { return titleId_; }
        const std::string& displayName() const { return displayName_; } // ASCII of the first name
        const std::vector<StfsEntry>& entries() const { return entries_; }
        const StfsEntry* find(const std::string& path) const; // without regard to case
        bool read(const StfsEntry& entry, std::vector<uint8_t>& out, std::string& error) const;

    private:
        bool blockOffset(uint32_t block, size_t& offset) const;
        bool nextBlock(uint32_t block, uint32_t& next) const;
        bool readBlocks(uint32_t first, uint32_t blocks, size_t size, std::vector<uint8_t>& out) const;

        std::vector<uint8_t> data_;
        std::vector<StfsEntry> entries_;
        std::string displayName_;
        uint32_t titleId_ = 0;
        uint32_t shift_ = 0; // 0: one hash table before each group of 170 blocks; 1: two
        size_t first_ = 0;   // where the first hash table is
    };

    // Starts with an STFS package's magic (LIVE, PIRS or CON): what tells a title update from a
    // disc image when the installer is given just a file
    bool looksLikeStfs(const std::string& path);

    // The update's patch for default.xex. A hash, not game content.
    extern const char kTitleUpdate2PatchSha256[];
    // The three files the build and the game need from the package, as <game dir>/extracted/tu2/<name>
    extern const char* const kTitleUpdateFiles[3];

    std::string updateFolder(); // <data home>/backbreaker-recomp/update

    enum class UpdateStatus
    {
        None,     // no package in the folder: the disc's version
        Ready,    // <game dir>/extracted/tu2 holds the update's files
        Unusable, // a package is there but can't be used; `message` says why
    };
    struct UpdateResult
    {
        UpdateStatus status = UpdateStatus::None;
        std::string message; // for the player; names the file, not its folder
        bool extracted = false; // the files were written now (not already there)
    };

    // Is this file Title Update 2 for Backbreaker? On success `patch`, `index` and `data` hold the
    // package's default.xexp and PatchPackedOutputX360.pkh/.pkd.
    bool readTitleUpdate(const std::string& path, std::vector<uint8_t>& patch, std::vector<uint8_t>& index,
                         std::vector<uint8_t>& data, std::string& error);

    // Looks for the update's package in `folder` and makes <gameDir>/extracted/tu2 hold its files
    // (written once: a stamp there names the patch they came from).
    UpdateResult syncTitleUpdate(const std::string& folder, const std::string& gameDir);
}
