#pragma once
// Minimal PNG writer (8-bit RGB, uncompressed deflate blocks): no dependencies, used for
// renderer screenshots. Files are large (~2.7 MB at 1280x720) but valid everywhere.
#include <cstdint>
#include <string>

namespace debug
{
    // rgba: width * height pixels, 4 bytes each (R, G, B, A); alpha is dropped. False on I/O error.
    bool writePng(const std::string& path, const uint8_t* rgba, uint32_t width, uint32_t height);
}
