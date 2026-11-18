// Backbreaker in Steam as a non-Steam game (#160). Steam keeps those in each account's
// userdata/<account>/config/shortcuts.vdf, a binary KeyValues file it reads when it starts and
// writes back from memory when it closes, so it has to be closed while the file changes.
//
// The format, as Steam writes it (client 1792618285, October 2026): a map is a 0x00 byte, its
// key and its entries, closed by 0x08; a string is 0x01, the key, the bytes and a 0; an int32 is
// 0x02, the key and four little-endian bytes. The file is the one map "shortcuts", whose entries
// "0", "1"... are the games, and it ends 08 08. Steam itself writes some keys in lower case
// ("appname", "exe"), others mixed ("StartDir", "LaunchOptions"), and reads either.
//
// The shortcut's `appid` is what Steam knows the game by: its playtime, its controller settings
// and the library artwork's file names (config/grid/<appid>p.png and so on). Steam stores it in
// the entry and uses it as stored: one it adds itself gets an id of its own (two of two on the
// maintainer's PC aren't the checksum below). A new entry here gets crc32(Exe + AppName) with the
// top bit set, as Steam ROM Manager (generate-app-id.ts) and RecompLauncher (shortcuts.rs) give it.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace installer
{
    // CRC-32 as zlib computes it (reflected 0xEDB88320); crc32("123456789") = 0xCBF43926
    uint32_t crc32(const void* data, size_t size);

    // One KeyValues entry. Scalars keep their stored bytes, so a file reads and writes back byte
    // for byte; a type this doesn't know (or a file cut short) is refused rather than guessed at.
    struct VdfNode
    {
        uint8_t type = 0; // 0x00 map, 0x01 string, 0x02 int32, 0x03 float, 0x06 colour, 0x07 uint64, 0x0A int64
        std::string key;
        std::string value; // a string's bytes, or a number's little-endian bytes
        std::vector<VdfNode> children; // a map's entries
    };
    bool parseVdf(const std::vector<uint8_t>& data, std::vector<VdfNode>& top);
    std::vector<uint8_t> writeVdf(const std::vector<VdfNode>& top);

    // A path as shortcuts.vdf's Exe and StartDir hold it: in double quotes. Empty when it has a
    // character Steam's command line or the shell it uses (for %command%) would read differently
    // (", `, $, \ or a control character), so the shortcut is left out rather than written wrong.
    std::string steamQuoted(const std::string& path);

    struct SteamShortcut
    {
        std::string name; // AppName
        std::string exe;  // the program, unquoted
        std::string startDir;
        std::string icon; // a picture's path, or empty
    };

    enum class ShortcutChange { Added, Updated, Unchanged, Failed };
    // Adds `s` to a shortcuts.vdf's contents (`old` empty: no file yet), or updates the entry that
    // already starts the same program, or failing that has the same name (case aside), so the game
    // is there once. An update keeps the entry's appid, its other keys and, when it already started
    // this program, its launch options. `appId` is the entry's id, for the artwork's file names.
    ShortcutChange addShortcut(const std::vector<uint8_t>& old, const SteamShortcut& s, std::vector<uint8_t>& out,
                               uint32_t& appId, std::string& error);

    // Steam installs on this machine for $HOME: the native one (~/.steam/steam, ~/.local/share/Steam
    // and the other places its launcher script uses) and Flathub's
    // (~/.var/app/com.valvesoftware.Steam). Only folders with a userdata/, each once (symlinks
    // followed), native ones first.
    struct SteamInstall
    {
        std::string root;
        bool flatpak = false;
    };
    std::vector<SteamInstall> findSteamInstalls();

    // userdata/<account>/config for every account that has signed in there (numbered folders; 0 is
    // no account)
    std::vector<std::string> steamAccountConfigs(const std::string& root);

    // A process of this user's named steam: Steam's client (ubuntu12_32/ or steamrt64/), its
    // launcher script or Flathub's, any of which may still write shortcuts.vdf on its way out.
    // `proc` is /proc but for the tests.
    bool steamRunning(const std::string& proc = "/proc");

    constexpr const char* kFlatpakSteam = "com.valvesoftware.Steam";
}
