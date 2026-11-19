#include "menu_entry.h"
#include "build.h"

#include <algorithm>
#include <cstring>

namespace installer
{
    std::vector<uint8_t> icoFromPng(const std::vector<uint8_t>& png)
    {
        static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        // The signature, then IHDR first: its length (4 bytes), type, width and height (big-endian)
        if (png.size() < 33 || !std::equal(signature, signature + 8, png.begin()) ||
            std::memcmp(png.data() + 12, "IHDR", 4) != 0)
            return {};
        auto be32 = [&](size_t at) {
            return uint32_t(png[at]) << 24 | uint32_t(png[at + 1]) << 16 | uint32_t(png[at + 2]) << 8 | png[at + 3];
        };
        const uint32_t width = be32(16), height = be32(20);
        if (!width || !height || width > 256 || height > 256) return {};
        // ICONDIR (reserved, type 1 = icon, one image), then its ICONDIRENTRY: width and height (0
        // for 256), no palette, reserved, one plane, 32 bits a pixel, the image's size and offset (22)
        std::vector<uint8_t> ico = {0, 0, 1, 0, 1, 0, uint8_t(width & 0xFF), uint8_t(height & 0xFF), 0, 0, 1, 0, 32, 0};
        for (uint32_t v : {uint32_t(png.size()), 22u})
            for (int i = 0; i < 4; i++) ico.push_back(uint8_t(v >> (8 * i)));
        ico.insert(ico.end(), png.begin(), png.end());
        return ico;
    }

    std::string menuEntryPath()
    {
        return dataHome() + "/applications/" + kAppId + ".desktop";
    }

    std::string desktopExecArg(const std::string& arg)
    {
        if (arg.empty()) return {};
        bool quote = false;
        std::string out;
        for (char c : arg) {
            const unsigned char u = static_cast<unsigned char>(c);
            // Inside quotes these need a backslash, which the string escapes double again: not worth it
            if (u < 0x20 || u == 0x7F || std::strchr("\"`$\\", c)) return {};
            if (std::strchr(" '><~|&;*?#()", c)) quote = true;
            if (c == '%') out += '%'; // a field code otherwise
            out += c;
        }
        return quote ? "\"" + out + "\"" : out;
    }

    std::string menuEntry(const std::string& game, const std::string& icon, bool hidden)
    {
        const std::string exec = desktopExecArg(game);
        // TryExec and Icon are plain strings: only a backslash or a control character would need care
        if (exec.empty() || desktopExecArg(icon).empty()) return {};
        return std::string("[Desktop Entry]\n"
                           "Type=Application\n"
                           "Name=Backbreaker\n"
                           "Comment=Built from your own disc (static recompilation)\n") +
               "Exec=" + exec + "\n" +
               "TryExec=" + game + "\n" +
               "Icon=" + icon + "\n" +
               "Terminal=false\n"
               "Categories=Game;SportsGame;\n"
               "StartupWMClass=" + kAppId + "\n" +
               (hidden ? "NoDisplay=true\n" : "");
    }

    std::string entryValue(const std::string& entry, const std::string& key)
    {
        bool inGroup = false;
        size_t at = 0;
        while (at < entry.size()) {
            size_t end = entry.find('\n', at);
            if (end == std::string::npos) end = entry.size();
            std::string line = entry.substr(at, end - at);
            at = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && line[0] == '[') {
                inGroup = line == "[Desktop Entry]";
                continue;
            }
            const size_t eq = line.find('=');
            if (!inGroup || eq == std::string::npos) continue;
            // The specification allows spaces around the '='
            size_t k = eq;
            while (k > 0 && line[k - 1] == ' ') k--;
            if (line.compare(0, k, key) != 0 || k != key.size()) continue;
            size_t v = eq + 1;
            while (v < line.size() && line[v] == ' ') v++;
            return line.substr(v);
        }
        return {};
    }

    bool launcherEntryReplaces(const std::string& existing, const std::string& game,
                               const std::function<bool(const std::string&)>& exists)
    {
        if (existing.empty()) return true;
        const std::string target = entryValue(existing, "TryExec");
        if (target.empty()) return false;
        if (target == game) return entryValue(existing, "NoDisplay") == "true";
        return !exists(target);
    }
}
