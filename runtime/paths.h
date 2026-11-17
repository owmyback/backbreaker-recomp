// Where the runtime finds the game's files and keeps the player's (#134).
//
// Checkout layout (the default, as before): everything relative to the working directory - the
// game's files and the player's settings, saves, cache partition and achievements in game/,
// screenshots/ and captures/ beside it. Run from the checkout's root.
//
// Installed layout: inside an AppImage (the AppImage runtime sets APPIMAGE and APPDIR) or when a
// launcher passes --data-dir. The game's files are read from the AppImage's game/ or that
// directory and may be read-only; the player's go to the per-user folders:
//   ~/.config/backbreaker-recomp/settings.ini        ($XDG_CONFIG_HOME)
//   ~/.local/share/backbreaker-recomp/               ($XDG_DATA_HOME): saves/, cache/, achievements/,
//                                                     sounds/, mods/, screenshots/, captures/, logs/
// --user-dir puts all of the player's files in one directory instead, in either layout.
#pragma once
#include <string>

namespace paths
{
    struct Options
    {
        std::string dataDir; // --data-dir
        std::string userDir; // --user-dir
    };

    // Decides the layout and creates the player's directories. Once, after the command line is
    // read and before anything below is used (until then they give the checkout layout). Prints
    // nothing: printLayout() does, once the log file (if any) has started.
    void init(const Options& options);
    void printLayout(); // the [paths] line, in the installed layout or with --user-dir

    bool installed();                  // the installed layout (see above)
    const std::string& data();         // the game's files: disc/, extracted/
    const std::string& user();         // the player's: saves/, cache/, achievements/, sounds/, mods/
    const std::string& settingsFile(); // game/settings.ini, or the per-user one
    const std::string& screenshots();  // screenshots/, or <user>/screenshots
    const std::string& captures();     // captures/, or <user>/captures
    std::string mods();                // <user>/mods: files here replace the disc's (see files.cpp)
    const std::string& logs();         // <user>/logs for log_file (#146); empty in the checkout layout
}
