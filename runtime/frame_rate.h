#pragma once
// The frame rate (#89): 30 frames a second, the console's, or 60. What it changes: frame_rate.cpp and
// docs/runtime.md, "Frame rate".
#include <cstdint>

namespace frame_rate
{
    // Before the game starts; 30 or 60 (main.cpp refuses anything else)
    void set(uint32_t fps);
    uint32_t get();
}
