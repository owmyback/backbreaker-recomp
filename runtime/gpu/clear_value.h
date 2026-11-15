#pragma once
// Resolve clears (issue #43): the clear registers hold the raw bits a Xenos clear writes into
// EDRAM, in the render target's own format. These turn them into the clear value of the host
// render target the renderer uses for that format (colorTargetFormat in renderer.cpp; depth is
// D32_SFLOAT_S8_UINT), decoding the bits exactly as the EDRAM transfer shader's writeUnpacked does,
// so a cleared sample holds what a transfer of the same EDRAM bits would give.
//
// From Xenia (BSD 3-clause, docs/licenses/xenia-LICENSE.txt): the 64-bit clear is
// RB_COLOR_CLEAR | RB_COLOR_CLEAR_LO << 32 (vulkan_render_target_cache.cc, Resolve), and the
// per-format decoding of PerformTransfersAndResolveClears; UNorm24To32 (xenos.h) and
// Float20e4To32 (xenos.cc). Float7e3To32 places the 7-bit mantissa at bit 16 of the float, as the
// transfer shader does (Xenia's C++ version shifts it by 3, which only works for 20e4's 20 bits).
// No Vulkan or other runtime headers, so tests/clear_value_test.cpp builds without game files.
#include <bit>
#include <cmath>
#include <cstdint>

namespace gpu::clear
{
    inline float unorm24To32(uint32_t n24)
    {
        n24 &= 0xFFFFFF;
        return float(n24 + (n24 >> 23)) * (1.0f / float(1 << 24)); // exact, and 0xFFFFFF -> 1.0
    }

    inline float float20e4To32(uint32_t f24) // D24FS8 depth, [0, 2)
    {
        f24 &= 0xFFFFFF;
        if (!f24) return 0.0f;
        uint32_t mantissa = f24 & 0xFFFFF, exponent = f24 >> 20;
        if (!exponent)
        {
            int lz = std::countl_zero(mantissa) - (32 - 21); // normalize the denormal
            exponent = uint32_t(1 - lz);
            mantissa = (mantissa << lz) & 0xFFFFF;
        }
        return std::bit_cast<float>(((exponent + 112) << 23) | (mantissa << 3));
    }

    inline float float7e3To32(uint32_t f10) // 2_10_10_10_FLOAT color, [0, 31.875]
    {
        f10 &= 0x3FF;
        if (!f10) return 0.0f;
        uint32_t mantissa = f10 & 0x7F, exponent = f10 >> 7;
        if (!exponent)
        {
            int lz = std::countl_zero(mantissa) - (32 - 8);
            exponent = uint32_t(1 - lz);
            mantissa = (mantissa << lz) & 0x7F;
        }
        return std::bit_cast<float>(((exponent + 124) << 23) | (mantissa << 16));
    }

    inline float halfToFloat(uint16_t h)
    {
        uint32_t sign = uint32_t(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023;
        if (e == 31) return std::bit_cast<float>(sign | 0x7F800000u | (m << 13)); // inf / NaN
        if (e == 0)
        {
            if (!m) return std::bit_cast<float>(sign);
            float v = std::ldexp(float(m), -24); // denormal: exact
            return sign ? -v : v;
        }
        return std::bit_cast<float>(sign | ((e + 112) << 23) | (m << 13));
    }

    inline float snorm16To32(uint32_t bits) { return std::fmax(float(int16_t(uint16_t(bits))) / 32767.0f, -1.0f); }

    // The 64-bit clear value: RB_COLOR_CLEAR in the low word (32bpp formats use only that one)
    inline uint64_t colorBits(uint32_t rbColorClear, uint32_t rbColorClearLo) { return rbColorClear | uint64_t(rbColorClearLo) << 32; }

    // format: xenos::ColorRenderTargetFormat. Writes the host clear color (the float32 view of a
    // VkClearColorValue; 32_FLOAT and 32_32_FLOAT keep their exact bits) and returns false for a
    // format the renderer has no render target for.
    inline bool color(uint32_t format, uint64_t v, float out[4])
    {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        uint32_t lo = uint32_t(v), hi = uint32_t(v >> 32);
        switch (format)
        {
        case 0: case 1: // 8_8_8_8 (+ GAMMA): red in the low byte
            for (int i = 0; i < 4; i++) out[i] = float((lo >> (i * 8)) & 0xFF) / 255.0f;
            return true;
        case 2: case 10: // 2_10_10_10 (+ AS_10_10_10_10)
            for (int i = 0; i < 3; i++) out[i] = float((lo >> (i * 10)) & 0x3FF) / 1023.0f;
            out[3] = float(lo >> 30) / 3.0f;
            return true;
        case 3: case 12: // 2_10_10_10_FLOAT (+ AS_16_16_16_16)
            for (int i = 0; i < 3; i++) out[i] = float7e3To32(lo >> (i * 10));
            out[3] = float(lo >> 30) / 3.0f;
            return true;
        case 4: case 5: // 16_16, 16_16_16_16 (64bpp: blue and alpha in the second word)
            out[0] = snorm16To32(lo);
            out[1] = snorm16To32(lo >> 16);
            if (format == 5) out[2] = snorm16To32(hi), out[3] = snorm16To32(hi >> 16);
            return true;
        case 6: case 7: // 16_16_FLOAT, 16_16_16_16_FLOAT
            out[0] = halfToFloat(uint16_t(lo));
            out[1] = halfToFloat(uint16_t(lo >> 16));
            if (format == 7) out[2] = halfToFloat(uint16_t(hi)), out[3] = halfToFloat(uint16_t(hi >> 16));
            return true;
        case 14: // 32_FLOAT
            out[0] = std::bit_cast<float>(lo);
            return true;
        case 15: // 32_32_FLOAT
            out[0] = std::bit_cast<float>(lo);
            out[1] = std::bit_cast<float>(hi);
            return true;
        default:
            return false;
        }
    }

    // depthFormat: xenos::DepthRenderTargetFormat (0 = D24S8, 1 = D24FS8). RB_DEPTH_CLEAR is
    // depth << 8 | stencil, like a depth sample in EDRAM.
    inline float depth(uint32_t depthFormat, uint32_t rbDepthClear)
    {
        return depthFormat == 1 ? float20e4To32(rbDepthClear >> 8) : unorm24To32(rbDepthClear >> 8);
    }
    inline uint32_t stencil(uint32_t rbDepthClear) { return rbDepthClear & 0xFF; }
}
