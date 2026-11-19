// The player's applications-menu entry for the installed game (#138): a .desktop file
// (freedesktop.org Desktop Entry Specification 1.5) in ~/.local/share/applications. Its name is
// the window's app id (runtime/video/presenter.cpp), which is how a Wayland desktop puts the
// entry's icon on the game's window in its taskbar, and how Steam's "Add a Non-Steam Game" list
// gets a name and an icon for it. A launcher's install writes the same file hidden (NoDisplay):
// not in the menu, since the launcher has its own entry, but still the window's name and icon.
#pragma once
#include <functional>
#include <string>

namespace installer
{
    constexpr const char* kAppId = "backbreaker-recomp";

    // $XDG_DATA_HOME/applications/backbreaker-recomp.desktop
    std::string menuEntryPath();

    // One Exec argument as the specification wants it: quoted when it has a reserved character,
    // % doubled. Empty when it has a character this doesn't write (", `, $, \ or a control
    // character): the entry is then left out rather than written wrong.
    std::string desktopExecArg(const std::string& arg);

    // The entry for the game at `game` (<prefix>/bin/backbreaker) with the icon at `icon`, or
    // empty when a path can't be written (above). TryExec hides it once the game is gone.
    // `hidden`: NoDisplay=true, kept out of the menu (a launcher's install).
    std::string menuEntry(const std::string& game, const std::string& icon, bool hidden = false);

    // A key's value in an entry's [Desktop Entry] group; empty when it has none
    std::string entryValue(const std::string& entry, const std::string& key);

    // Whether a launcher's hidden entry for `game` goes in place of `existing`, the entry already
    // there (empty: none). Never over an entry for a game that `exists` still finds, a terminal
    // install's in the menu or another launcher install's, nor over this game's own entry when
    // it's in the menu, nor over an entry this program didn't write (no TryExec). Over one whose
    // game is gone, it does.
    bool launcherEntryReplaces(const std::string& existing, const std::string& game,
                               const std::function<bool(const std::string&)>& exists);
}
