#pragma once
#include <cstdint>

namespace apu
{
    // XMA decoder: context array, kick/lock/clear registers, silent decoding thread
    void initXma();
    void mmioStore(uint32_t reg, uint32_t value); // value in register (little-endian) order
    uint64_t xmaFramesDecoded();
}
