// Checks runtime/gpu/clear_value.h (issue #43): resolve clear registers -> host clear values, per
// render target format. No game files, no generated headers - see README "Tests" for the build
// command.
//
// Expected values are worked out by hand from the formats' bit layouts (Xenia's xenos.h and
// vulkan_render_target_cache.cc PerformTransfersAndResolveClears; the transfer shader's
// writeUnpacked in renderer.cpp decodes the same way), e.g. 7e3 0x1C0 = exponent 3 (bias 3),
// mantissa 0x40/128 = 1.5, and 20e4 0xFFFFFF = 2^0 * (1 + 0xFFFFF/2^20) = float bits 3FFFFFF8.
#include "../runtime/gpu/clear_value.h"

#include <bit>
#include <cstdio>
#include <initializer_list>
#include <limits>

static int failures = 0, checks = 0;

static void same(const char* what, float got, float want)
{
    checks++;
    if (std::bit_cast<uint32_t>(got) != std::bit_cast<uint32_t>(want))
    {
        failures++;
        printf("FAIL %s: %.9g (%08X), expected %.9g (%08X)\n", what, got, std::bit_cast<uint32_t>(got), want, std::bit_cast<uint32_t>(want));
    }
}

static void color(const char* what, uint32_t format, uint64_t bits, float r, float g, float b, float a)
{
    float out[4];
    checks++;
    if (!gpu::clear::color(format, bits, out))
    {
        failures++;
        printf("FAIL %s: format %u not converted\n", what, format);
        return;
    }
    char name[96];
    const float want[4] = { r, g, b, a };
    for (int i = 0; i < 4; i++)
    {
        snprintf(name, sizeof name, "%s .%c", what, "rgba"[i]);
        same(name, out[i], want[i]);
    }
}

static float bits(uint32_t u) { return std::bit_cast<float>(u); }

int main()
{
    using namespace gpu::clear;

    // RB_COLOR_CLEAR is the low word of the 64-bit value, RB_COLOR_CLEAR_LO the high one
    checks++;
    if (colorBits(0x11223344, 0x55667788) != 0x5566778811223344ull) failures++, printf("FAIL colorBits\n");

    // 8_8_8_8 and 8_8_8_8_GAMMA: red in the low byte
    color("8_8_8_8 80FF4000", 0, 0x80FF4000, 0.0f, 64.0f / 255.0f, 1.0f, 128.0f / 255.0f);
    color("8_8_8_8_GAMMA FF0000FF", 1, 0xFF0000FF, 1.0f, 0.0f, 0.0f, 1.0f);
    color("8_8_8_8 ignores the high word", 0, 0xFFFFFFFF00000000ull, 0.0f, 0.0f, 0.0f, 0.0f);

    // 2_10_10_10: 10 bits per color from bit 0, 2 alpha bits on top
    color("2_10_10_10 C00003FF", 2, 0xC00003FF, 1.0f, 0.0f, 0.0f, 1.0f);
    color("2_10_10_10 40000200", 2, 0x40000200, 512.0f / 1023.0f, 0.0f, 0.0f, 1.0f / 3.0f);
    color("2_10_10_10_AS_10_10_10_10 3FF00000", 10, 0x3FF00000, 0.0f, 0.0f, 1.0f, 0.0f);

    // 2_10_10_10_FLOAT: 7e3 colors (1.0 = 0x180, 1.5 = 0x1C0, 31.875 = 0x3FF), alpha 2/3
    uint32_t f = 0x180 | (0x1C0u << 10) | (0x3FFu << 20) | (2u << 30);
    color("2_10_10_10_FLOAT 1, 1.5, 31.875, 2/3", 3, f, 1.0f, 1.5f, 31.875f, 2.0f / 3.0f);
    color("2_10_10_10_FLOAT_AS_16_16_16_16 denormal 0x001", 12, 0x001, 0.001953125f, 0.0f, 0.0f, 0.0f); // 1/128 * 2^-2
    same("7e3 0x040 (denormal 1/2 * 2^-2)", float7e3To32(0x040), 0.125f);
    same("7e3 0 -> 0", float7e3To32(0), 0.0f);

    // 16_16 / 16_16_16_16: signed 16-bit, /32767, -32768 clamps to -1 (the host targets are SNORM)
    color("16_16 80007FFF", 4, 0x80007FFF, 1.0f, -1.0f, 0.0f, 0.0f);
    color("16_16 0000C000", 4, 0x0000C000, -16384.0f / 32767.0f, 0.0f, 0.0f, 0.0f);
    color("16_16 ignores the high word", 4, 0x7FFF7FFF00000000ull, 0.0f, 0.0f, 0.0f, 0.0f);
    color("16_16_16_16 blue/alpha from the high word", 5, colorBits(0x00017FFF, 0xFFFF0000), 1.0f, 1.0f / 32767.0f, 0.0f, -1.0f / 32767.0f);

    // 16_16_FLOAT / 16_16_16_16_FLOAT: halves, low half first
    color("16_16_FLOAT C0003C00", 6, 0xC0003C00, 1.0f, -2.0f, 0.0f, 0.0f);
    color("16_16_FLOAT inf, smallest denormal", 6, 0x00017C00, std::numeric_limits<float>::infinity(), 0x1p-24f, 0.0f, 0.0f);
    color("16_16_16_16_FLOAT", 7, colorBits(0x80003C00, 0x35553800), 1.0f, -0.0f, 0.5f, 0.333251953125f);

    // 32_FLOAT / 32_32_FLOAT: the bits as they are, NaN payloads included
    color("32_FLOAT 1.0", 14, 0x3F800000, 1.0f, 0.0f, 0.0f, 0.0f);
    color("32_FLOAT NaN payload", 14, 0x7FC00001, bits(0x7FC00001), 0.0f, 0.0f, 0.0f);
    color("32_32_FLOAT pi, -1", 15, colorBits(0x40490FDB, 0xBF800000), bits(0x40490FDB), -1.0f, 0.0f, 0.0f);

    // Formats without a render target here
    float out[4];
    for (uint32_t unsupported : { 8u, 9u, 11u, 13u })
    {
        checks++;
        if (color(unsupported, 0, out)) failures++, printf("FAIL format %u should be unsupported\n", unsupported);
    }

    // Depth: RB_DEPTH_CLEAR = depth << 8 | stencil
    same("D24S8 FFFFFF00 -> 1.0", depth(0, 0xFFFFFF00), 1.0f);
    same("D24S8 00000000 -> 0.0", depth(0, 0x00000000), 0.0f);
    same("D24S8 800000xx -> (2^23 + 1) / 2^24", depth(0, 0x80000080), 8388609.0f / 16777216.0f);
    same("D24S8 C00000xx -> (0xC00000 + 1) / 2^24", depth(0, 0xC0000000), 12582913.0f / 16777216.0f);
    same("D24S8 00000100 -> 2^-24", depth(0, 0x00000100), 0x1p-24f);
    same("D24FS8 FFFFFF00 -> largest 20e4", depth(1, 0xFFFFFF00), bits(0x3FFFFFF8));
    same("D24FS8 F00000xx -> 1.0", depth(1, 0xF0000000), 1.0f);
    same("D24FS8 E00000xx -> 0.5", depth(1, 0xE0000000), 0.5f);
    same("20e4 0x080000 (denormal 1/2 * 2^-14)", float20e4To32(0x080000), 0x1p-15f);
    checks++;
    if (stencil(0xFFFFFF80) != 0x80) failures++, printf("FAIL stencil\n");

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
