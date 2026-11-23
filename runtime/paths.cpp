#include "paths.h"
#include "log.h"

#include <cstdlib>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace
{
    bool g_installed = false;
    std::string g_data = "game";
    std::string g_user = "game";
    std::string g_settings = "game/settings.ini";
    std::string g_screenshots = "screenshots";
    std::string g_captures = "captures";
    std::string g_logs; // none in the checkout layout: a terminal or a pipe has the log there

#ifdef _WIN32
    // Windows (#174): the player's files and settings both go in %LOCALAPPDATA%\backbreaker-recomp
    std::string xdg(const char*, const char*)
    {
        if (const char* v = std::getenv("LOCALAPPDATA"); v && *v) return v;
        if (const char* h = std::getenv("USERPROFILE"); h && *h) return std::string(h) + "/AppData/Local";
        return ".";
    }

    fs::path selfPath(std::error_code& ec)
    {
        std::wstring buf(MAX_PATH, L'\0');
        for (;;)
        {
            DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
            if (n == 0) { ec = std::error_code(int(GetLastError()), std::system_category()); return {}; }
            if (n < buf.size()) { buf.resize(n); return fs::path(buf); }
            buf.resize(buf.size() * 2);
        }
    }
#else
    std::string home()
    {
        if (const char* h = std::getenv("HOME"); h && *h) return h;
        if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir) return pw->pw_dir;
        return ".";
    }

    // $XDG_<name>_HOME if set to an absolute path (the spec ignores relative ones), else ~/<fallback>
    std::string xdg(const char* name, const char* fallback)
    {
        if (const char* v = std::getenv(name); v && v[0] == '/') return v;
        return home() + "/" + fallback;
    }

    fs::path selfPath(std::error_code& ec) { return fs::read_symlink("/proc/self/exe", ec); }
#endif

    // The installer's layout (#138): this program is <prefix>/bin/backbreaker and the game's files
    // are <prefix>/game, which the installer marks finished with game/.complete. Found from the
    // program's own path, so a shortcut to it (Steam's non-Steam game) needs no --data-dir and no
    // particular working directory. A checkout's build/backbreaker never matches.
    std::string installerGameDir()
    {
        std::error_code ec;
        const fs::path self = selfPath(ec);
        if (ec || self.parent_path().filename() != "bin") return {};
        const fs::path game = self.parent_path().parent_path() / "game";
        return fs::exists(game / ".complete", ec) ? game.string() : std::string();
    }
}

namespace paths
{
    void init(const Options& o)
    {
        const char* appImage = std::getenv("APPIMAGE");
        const char* appDir = std::getenv("APPDIR");
        if (!o.dataDir.empty())
        {
            g_data = o.dataDir;
            g_installed = true;
        }
        else if (appImage && appDir && fs::is_directory(fs::path(appDir) / "game"))
        {
            g_data = (fs::path(appDir) / "game").string();
            g_installed = true;
        }
        else if (std::string game = installerGameDir(); !game.empty())
        {
            g_data = std::move(game);
            g_installed = true;
        }

        if (!o.userDir.empty())
        {
            g_user = o.userDir;
            g_settings = g_user + "/settings.ini";
        }
        else if (g_installed)
        {
            g_user = xdg("XDG_DATA_HOME", ".local/share") + "/backbreaker-recomp";
            g_settings = xdg("XDG_CONFIG_HOME", ".config") + "/backbreaker-recomp/settings.ini";
        }
        if (g_installed || !o.userDir.empty())
        {
            g_screenshots = g_user + "/screenshots";
            g_captures = g_user + "/captures";
            g_logs = g_user + "/logs";
        }

        std::error_code ec;
        fs::create_directories(g_user, ec);
        fs::create_directories(fs::path(g_settings).parent_path(), ec);
    }

    void printLayout()
    {
        if (!g_logs.empty())
            info("[paths] game files %s; player files %s; settings %s", g_data.c_str(), g_user.c_str(), g_settings.c_str());
    }

    const std::string& logs() { return g_logs; }

    bool installed() { return g_installed; }
    const std::string& data() { return g_data; }
    const std::string& user() { return g_user; }
    const std::string& settingsFile() { return g_settings; }
    const std::string& screenshots() { return g_screenshots; }
    const std::string& captures() { return g_captures; }
    std::string mods() { return g_user + "/mods"; }
}
