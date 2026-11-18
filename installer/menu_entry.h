// The player's applications-menu entry for the installed game (#138): a .desktop file
// (freedesktop.org Desktop Entry Specification 1.5) in ~/.local/share/applications. Its name is
// the window's app id (runtime/video/presenter.cpp), which is how a Wayland desktop puts the
// entry's icon on the game's window in its taskbar, and how Steam's "Add a Non-Steam Game" list
// gets a name and an icon for it.
#pragma once
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
    std::string menuEntry(const std::string& game, const std::string& icon);
}
