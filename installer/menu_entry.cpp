#include "menu_entry.h"
#include "build.h"

#include <cstring>

namespace installer
{
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

    std::string menuEntry(const std::string& game, const std::string& icon)
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
               "StartupWMClass=" + kAppId + "\n";
    }
}
