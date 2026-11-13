#pragma once
#include <string>

namespace hid
{
    // Scripted button presses for testing without a controller:
    // "start@10,a@14,down@16.5" presses each button at that many seconds after launch
    // (held 150 ms), reported both as pad state and as keystroke events.
    // Returns false (and logs) on a malformed script.
    bool setScript(const std::string& script);
}
