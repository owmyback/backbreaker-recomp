// game/settings.ini (issues #9, #115, #89): see settings.h.
#include "settings.h"
#include "log.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace settings
{
    namespace
    {
        std::string trim(const std::string& s)
        {
            size_t a = s.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) return {};
            return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
        }
    }

    Settings load(const std::string& path)
    {
        Settings s;
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f)
            return s; // first run: defaults
        char line[128];
        while (std::fgets(line, sizeof(line), f))
        {
            char* eq = std::strchr(line, '=');
            if (!eq)
                continue;
            std::string key = trim(std::string(line, eq));
            int value = std::atoi(eq + 1);
            if (key == "width") s.width = value;
            else if (key == "height") s.height = value;
            else if (key == "x") { s.x = value; s.hasPosition = true; }
            else if (key == "y") { s.y = value; s.hasPosition = true; }
            else if (key == "fullscreen") s.fullscreen = value != 0;
            else if (key == "resolution_scale")
            {
                std::string text = trim(eq + 1);
                char* end = nullptr;
                long n = std::strtol(text.c_str(), &end, 10);
                bool number = !text.empty() && *end == 0;
                long scale = number ? std::clamp(n, 1L, 4L) : 1;
                if (!number || scale != n)
                    warn("%s: resolution_scale=%s is not 1-4 - using %ld", path.c_str(), text.c_str(), scale);
                s.resolutionScale = uint32_t(scale);
            }
            else if (key == "anisotropy")
            {
                std::string text = trim(eq + 1);
                char* end = nullptr;
                long n = std::strtol(text.c_str(), &end, 10);
                bool number = !text.empty() && *end == 0;
                long level = number ? std::clamp(n, 1L, 16L) : long(Settings{}.anisotropy);
                if (!number || level != n)
                    warn("%s: anisotropy=%s is not 1-16 - using %ld", path.c_str(), text.c_str(), level);
                s.anisotropy = uint32_t(level);
            }
            else if (key == "skip_intro")
            {
                std::string text = trim(eq + 1);
                if (text == "0" || text == "1") s.skipIntro = text == "1";
                else warn("%s: skip_intro=%s is not 0 or 1 - using %d", path.c_str(), text.c_str(), int(s.skipIntro));
            }
            else if (key == "native_720p")
            {
                std::string text = trim(eq + 1);
                if (text == "0" || text == "1") s.native720p = text == "1";
                else warn("%s: native_720p=%s is not 0 or 1 - using %d", path.c_str(), text.c_str(), int(s.native720p));
            }
            else if (key == "fps")
            {
                std::string text = trim(eq + 1);
                s.fps = text == "30" ? 30 : text == "60" ? 60 : Settings{}.fps;
                if (text != "30" && text != "60")
                    warn("%s: fps=%s is not 30 or 60 - using %u", path.c_str(), text.c_str(), s.fps);
            }
        }
        std::fclose(f);
        if (s.width <= 0 || s.height <= 0) { s.width = 1280; s.height = 720; }
        return s;
    }

    bool save(const Settings& s, const std::string& path)
    {
        FILE* f = std::fopen(path.c_str(), "w");
        if (!f)
        {
            warn("cannot write %s: %s", path.c_str(), std::strerror(errno));
            return false;
        }
        std::fprintf(f, "width=%d\nheight=%d\nx=%d\ny=%d\nfullscreen=%d\nresolution_scale=%u\nfps=%u\nanisotropy=%u\nnative_720p=%d\nskip_intro=%d\n",
                     s.width, s.height, s.x, s.y, int(s.fullscreen), s.resolutionScale, s.fps, s.anisotropy, int(s.native720p),
                     int(s.skipIntro));
        std::fclose(f);
        return true;
    }
}
