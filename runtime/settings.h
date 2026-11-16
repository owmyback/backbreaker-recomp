#pragma once
// game/settings.ini: the player's own settings, plain key=value lines (issues #9, #115, #89). game/ is
// gitignored, so this is per-machine state, not project data. Every key the runtime knows is in
// Settings, so writing the file back (the window does when it closes) keeps all of them.
#include <cstdint>
#include <string>

namespace settings
{
    constexpr const char* kPath = "game/settings.ini";

    struct Settings
    {
        // The window (#9). In fullscreen, width and height are the size it had before.
        int width = 1280, height = 720;
        int x = 0, y = 0;
        bool hasPosition = false;
        bool fullscreen = false;
        // Render scale of the 3D scenes, 1-4 (#115; what it does: docs/runtime.md). Windowed runs
        // only; --resolution-scale overrides it for one run and leaves the file alone.
        uint32_t resolutionScale = 1;
        // Frames a second, 30 (the console's) or 60 (#89; what it changes: docs/runtime.md, "Frame
        // rate"). Windowed runs only; --fps overrides it for one run and leaves the file alone.
        uint32_t fps = 60;
        // Anisotropic filtering for the textures the game minifies with mips, 1 (off: what the game
        // asks for) to 16. The game asks for it on only a few textures, so without it the field
        // blurs toward the horizon, as on the console; 16 costs nothing measurable (#82). Windowed
        // runs only; --anisotropy overrides it for one run and leaves the file alone.
        uint32_t anisotropy = 16;
    };

    // No file or no key: the defaults above. A value out of range gets a warning and the nearest
    // valid one (for fps, the default). Keys may have spaces around them (the file is edited by hand).
    Settings load(const std::string& path = kPath);

    // false (and a warning) when the file cannot be written
    bool save(const Settings& s, const std::string& path = kPath);
}
