// game/settings.ini (issues #9, #115): see settings.h.
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
        std::fprintf(f, "width=%d\nheight=%d\nx=%d\ny=%d\nfullscreen=%d\nresolution_scale=%u\n",
                     s.width, s.height, s.x, s.y, int(s.fullscreen), s.resolutionScale);
        std::fclose(f);
        return true;
    }
}
