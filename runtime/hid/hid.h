#pragma once
#include <cstdint>
#include <string>

namespace hid
{
    // Scripted button presses for testing without a controller:
    // "start@10,a@14,down@16.5" presses each button at that many seconds after launch
    // (held 150 ms), reported both as pad state and as keystroke events.
    // Returns false (and logs) on a malformed script.
    bool setScript(const std::string& script);
    // Seconds since launch: the clock --press uses (also --screenshot-at and --capture-at).
    double secondsSinceLaunch();

    // Host controllers through SDL3 (issue #6). SDL wants its events handled on one thread:
    // with a window that is the presenter's loop, which calls initGamepads() once after SDL_Init
    // and pollGamepads() every iteration. Headless runs call startInputThread() instead.
    void initGamepads();
    void pollGamepads();
    void startInputThread();
    // User 0 stays connected without a real controller (scripted / headless runs).
    void setVirtualPad(bool enabled);

    // For the in-window overlay (#101). While captured, the game reads the controllers as neutral,
    // and a button held when the capture ends stays released for the game until it is let go.
    // takeMenuPresses() returns the buttons newly pressed on any real controller since the last
    // call: XINPUT_GAMEPAD_* masks (the left stick counts as the d-pad, and a direction kept held
    // presses again), plus 0x0400 for the Guide (PS) button. Scripted presses (--press) that begin
    // while captured go to the overlay as well, and not to the game.
    void setInputCaptured(bool captured);
    uint32_t takeMenuPresses();
}
