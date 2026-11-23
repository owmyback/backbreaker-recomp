#include "steam_shortcut.h"
#include "build.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include "platform.h"
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace installer
{
    uint32_t crc32(const void* data, size_t size)
    {
        static const auto table = [] {
            std::array<uint32_t, 256> t{};
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }
            return t;
        }();
        uint32_t c = 0xFFFFFFFFu;
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    namespace
    {
        constexpr uint8_t kMap = 0x00, kString = 0x01, kInt32 = 0x02, kEnd = 0x08;
        constexpr int kMaxDepth = 32; // shortcuts.vdf goes three deep (shortcuts, an entry, its tags)

        // The bytes a scalar type holds, or 0 for one this doesn't read
        size_t scalarSize(uint8_t type)
        {
            switch (type) {
            case 0x02: case 0x03: case 0x04: case 0x06: return 4; // int32, float, pointer, colour
            case 0x07: case 0x0A: return 8;                       // uint64, int64
            default: return 0;
            }
        }

        struct Reader
        {
            const std::vector<uint8_t>& d;
            size_t i = 0;

            bool cstr(std::string& s)
            {
                const auto end = std::find(d.begin() + long(i), d.end(), uint8_t(0));
                if (end == d.end()) return false;
                s.assign(d.begin() + long(i), end);
                i = size_t(end - d.begin()) + 1;
                return true;
            }

            // Entries up to and including the map's 0x08
            bool map(std::vector<VdfNode>& out, int depth)
            {
                if (depth > kMaxDepth) return false;
                while (i < d.size()) {
                    const uint8_t type = d[i++];
                    if (type == kEnd) return true;
                    VdfNode n;
                    n.type = type;
                    if (!cstr(n.key)) return false;
                    if (type == kMap) {
                        if (!map(n.children, depth + 1)) return false;
                    } else if (type == kString) {
                        if (!cstr(n.value)) return false;
                    } else if (const size_t size = scalarSize(type); size && i + size <= d.size()) {
                        n.value.assign(d.begin() + long(i), d.begin() + long(i + size));
                        i += size;
                    } else {
                        return false;
                    }
                    out.push_back(std::move(n));
                }
                return false; // cut short
            }
        };

        void writeMap(std::vector<uint8_t>& out, const std::vector<VdfNode>& m)
        {
            for (const VdfNode& n : m) {
                out.push_back(n.type);
                out.insert(out.end(), n.key.begin(), n.key.end());
                out.push_back(0);
                if (n.type == kMap) {
                    writeMap(out, n.children);
                } else {
                    out.insert(out.end(), n.value.begin(), n.value.end());
                    if (n.type == kString) out.push_back(0);
                }
            }
            out.push_back(kEnd);
        }

        bool sameKey(const std::string& a, const char* b)
        {
            return a.size() == std::strlen(b) &&
                   std::equal(a.begin(), a.end(), b, [](char x, char y) { return std::tolower(uint8_t(x)) == std::tolower(uint8_t(y)); });
        }

        VdfNode* find(std::vector<VdfNode>& m, const char* key)
        {
            for (VdfNode& n : m)
                if (sameKey(n.key, key)) return &n;
            return nullptr;
        }

        std::string stringField(std::vector<VdfNode>& e, const char* key)
        {
            const VdfNode* n = find(e, key);
            return n && n->type == kString ? n->value : std::string();
        }

        // Sets a string, keeping the key's spelling when it's there
        void setString(std::vector<VdfNode>& e, const char* key, const std::string& value)
        {
            VdfNode* n = find(e, key);
            if (!n) {
                e.push_back({kString, key, {}, {}});
                n = &e.back();
            }
            n->type = kString;
            n->value = value;
            n->children.clear();
        }

        std::string int32Bytes(uint32_t v)
        {
            return {char(v), char(v >> 8), char(v >> 16), char(v >> 24)};
        }

        uint32_t int32Value(const std::string& b)
        {
            uint32_t v = 0;
            for (size_t k = 0; k < 4 && k < b.size(); k++) v |= uint32_t(uint8_t(b[k])) << (8 * k);
            return v;
        }

        // The id Steam gives an entry with no appid of its own (and the one a new entry gets)
        uint32_t shortcutId(const std::string& quotedExe, const std::string& name)
        {
            const std::string key = quotedExe + name;
            return crc32(key.data(), key.size()) | 0x80000000u;
        }

        std::string unquoted(const std::string& s)
        {
            return s.size() >= 2 && s.front() == '"' && s.back() == '"' ? s.substr(1, s.size() - 2) : s;
        }

        bool sameName(const std::string& a, const std::string& b)
        {
            return sameKey(a, b.c_str());
        }
    }

    bool parseVdf(const std::vector<uint8_t>& data, std::vector<VdfNode>& top)
    {
        top.clear();
        Reader r{data};
        return r.map(top, 0) && r.i == data.size();
    }

    std::vector<uint8_t> writeVdf(const std::vector<VdfNode>& top)
    {
        std::vector<uint8_t> out;
        writeMap(out, top);
        return out;
    }

    std::string steamQuoted(const std::string& path)
    {
        if (path.empty()) return {};
#ifdef _WIN32
        // Steam on Windows starts it with CreateProcess, no shell: only a quote would end the path early
        // (and every Windows path has backslashes)
        const char* unsafe = "\"";
#else
        const char* unsafe = "\"`$\\";
#endif
        for (char c : path) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u < 0x20 || u == 0x7F || std::strchr(unsafe, c)) return {};
        }
        return "\"" + path + "\"";
    }

    ShortcutChange addShortcut(const std::vector<uint8_t>& old, const SteamShortcut& s, std::vector<uint8_t>& out,
                               uint32_t& appId, std::string& error)
    {
        const std::string exe = steamQuoted(s.exe), startDir = steamQuoted(s.startDir);
        if (exe.empty() || startDir.empty()) {
            error = "the folder's path has a \", `, $ or \\ in it";
            return ShortcutChange::Failed;
        }
        std::vector<VdfNode> top;
        if (!old.empty() && !parseVdf(old, top)) {
            // Rather than write a file with only this game in it
            error = "it has something in it this installer can't read";
            return ShortcutChange::Failed;
        }
        VdfNode* list = find(top, "shortcuts");
        if (list && list->type != kMap) {
            error = "its list of games isn't one";
            return ShortcutChange::Failed;
        }
        if (!list) {
            top.push_back({kMap, "shortcuts", {}, {}});
            list = &top.back();
        }

        // The entry that already starts this program, else one with the same name
        VdfNode* entry = nullptr;
        for (VdfNode& e : list->children)
            if (!entry && e.type == kMap && unquoted(stringField(e.children, "exe")) == s.exe) entry = &e;
        const bool sameProgram = entry != nullptr;
        for (VdfNode& e : list->children)
            if (!entry && e.type == kMap && sameName(stringField(e.children, "appname"), s.name)) entry = &e;

        if (entry) {
            std::vector<VdfNode>& e = entry->children;
            const std::vector<uint8_t> before = writeVdf(e);
            VdfNode* id = find(e, "appid");
            if (!id || id->type != kInt32) {
                // The id Steam has been using for it, from before this changes its Exe
                const uint32_t legacy = shortcutId(stringField(e, "exe"), stringField(e, "appname"));
                if (id) *id = {kInt32, id->key, int32Bytes(legacy), {}};
                else e.insert(e.begin(), {kInt32, "appid", int32Bytes(legacy), {}});
                id = find(e, "appid");
            }
            appId = int32Value(id->value);
            setString(e, "appname", s.name);
            setString(e, "exe", exe);
            setString(e, "StartDir", startDir);
            setString(e, "icon", s.icon);
            // Options for another program (the AppImage's `play`, say) would be this one's arguments
            if (!sameProgram) setString(e, "LaunchOptions", "");
            if (writeVdf(e) == before) return ShortcutChange::Unchanged;
            out = writeVdf(top);
            return ShortcutChange::Updated;
        }

        // A new entry: the keys, their order and their defaults as Steam's own "Add a Non-Steam Game" writes them
        appId = shortcutId(exe, s.name);
        unsigned next = 0;
        for (const VdfNode& e : list->children)
            if (!e.key.empty() && std::all_of(e.key.begin(), e.key.end(), [](char c) { return std::isdigit(uint8_t(c)); }))
                next = std::max(next, unsigned(std::strtoul(e.key.c_str(), nullptr, 10)) + 1);
        auto str = [](const char* k, const std::string& v) { return VdfNode{kString, k, v, {}}; };
        auto num = [](const char* k, uint32_t v) { return VdfNode{kInt32, k, int32Bytes(v), {}}; };
        list->children.push_back({kMap, std::to_string(next), {},
                                  {num("appid", appId), str("appname", s.name), str("exe", exe), str("StartDir", startDir),
                                   str("icon", s.icon), str("ShortcutPath", ""), str("LaunchOptions", ""), num("IsHidden", 0),
                                   num("AllowDesktopConfig", 1), num("AllowOverlay", 1), num("OpenVR", 0), num("Devkit", 0),
                                   str("DevkitGameID", ""), num("DevkitOverrideAppID", 0), num("LastPlayTime", 0),
                                   str("FlatpakAppID", ""), str("sortas", ""), VdfNode{kMap, "tags", {}, {}}}});
        out = writeVdf(top);
        return ShortcutChange::Added;
    }

    std::vector<SteamInstall> findSteamInstalls()
    {
#ifdef _WIN32
        // One Steam per machine, where the registry says (installer/platform.h, steamFolder)
        std::vector<SteamInstall> found;
        std::error_code ec;
        if (const std::string folder = platform::steamFolder(); !folder.empty() && fs::is_directory(folder + "/userdata", ec))
            found.push_back({folder, false});
        return found;
#else
        const char* h = std::getenv("HOME");
        const std::string home = h && *h ? h : ".";
        const std::string flatpak = home + "/.var/app/" + kFlatpakSteam;
        // Native: the links Steam's launcher script keeps, then where it puts Steam ($XDG_DATA_HOME,
        // ~/.local/share, Debian's package, the old ~/Steam). Flathub's: its home inside the sandbox;
        // its ~/.steam links point at paths only the sandbox sees.
        const std::pair<std::string, bool> candidates[] = {
            {home + "/.steam/steam", false},        {home + "/.steam/root", false},
            {dataHome() + "/Steam", false},         {home + "/.local/share/Steam", false},
            {home + "/.steam/debian-installation", false}, {home + "/Steam", false},
            {flatpak + "/.local/share/Steam", true}, {flatpak + "/data/Steam", true},
        };
        std::vector<SteamInstall> found;
        std::vector<std::string> seen;
        for (const auto& [path, isFlatpak] : candidates) {
            std::error_code ec;
            if (!fs::is_directory(path + "/userdata", ec)) continue;
            const std::string real = fs::canonical(path, ec).string();
            if (ec || std::find(seen.begin(), seen.end(), real) != seen.end()) continue;
            seen.push_back(real);
            found.push_back({path, isFlatpak});
        }
        return found;
#endif
    }

    std::vector<std::string> steamAccountConfigs(const std::string& root)
    {
        std::vector<std::string> configs;
        std::error_code ec;
        for (const fs::directory_entry& d : fs::directory_iterator(root + "/userdata", ec)) {
            const std::string name = d.path().filename().string();
            if (name.empty() || name == "0" || !std::all_of(name.begin(), name.end(), [](char c) { return std::isdigit(uint8_t(c)); }))
                continue;
            if (d.is_directory(ec)) configs.push_back(d.path().string() + "/config");
        }
        std::sort(configs.begin(), configs.end());
        return configs;
    }

    bool steamRunning(const std::string& proc)
    {
#ifdef _WIN32
        (void)proc;
        return platform::processRunning("steam.exe");
#else
        std::error_code ec;
        for (const fs::directory_entry& d : fs::directory_iterator(proc, ec)) {
            const std::string pid = d.path().filename().string();
            if (pid.empty() || !std::all_of(pid.begin(), pid.end(), [](char c) { return std::isdigit(uint8_t(c)); })) continue;
            struct stat st {};
            if (stat(d.path().c_str(), &st) != 0 || st.st_uid != getuid()) continue;
            std::ifstream f(d.path() / "comm");
            std::string comm;
            if (std::getline(f, comm) && comm == "steam") return true;
        }
        return false;
#endif
    }
}
