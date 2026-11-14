#pragma once
// Xenos texture mip layout (roadmap phase 4, issue #46): where each level of a 2D or cube texture
// is in guest memory. Follows Xenia's `texture_util.cc` (`GetGuestTextureLayout`,
// `GetPackedMipOffset`, `GetPackedMipLevel`; BSD 3-clause - see docs/licenses/xenia-LICENSE.txt):
// - Level 0 is at the base address, rows `pitch` texels apart (the fetch constant's pitch).
// - Levels 1 and up follow each other from the mip address. Each level is laid out as if the base
//   size were rounded up to a power of two (then >> level): rows and height aligned to 32 blocks,
//   linear rows to 256 bytes. Each face of a level is 4 KB aligned, and a level holds all its
//   faces before the next level starts.
// - With packed mips (fetch constant dword 5 bit 11), the levels from the first one whose shorter
//   side is 16 texels or less share one region, the mip tail, each at its own block offset in
//   it. When that first level is 0, the base sits in a tail of its own at the base address, and
//   levels 1 and up share the tail at the mip address, laid out at level 0's size.
// The renderer reads and untiles each region once per face; each of its levels is a rectangle of
// the result. tests/mip_layout_test.cpp checks it against values from Xenia's functions.
// Header-only (inline) like tiling.h, so the test builds without the renderer.
#include "tiling.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace gpu
{
    // Texture format block: 1x1 texels for most formats, 4x4 for DXT
    struct BlockFormat
    {
        uint32_t blockW, blockH, bytes;
    };

    struct GuestRegion
    {
        bool mip = false;                  // at the mip address (else at the base address)
        uint32_t offset = 0;               // bytes from that address (face 0)
        uint32_t rowBlocks = 0;            // pitch, in blocks
        uint32_t xBlocks = 0, yBlocks = 0; // extent read, in blocks
        uint32_t sliceStride = 0;          // bytes from one face to the next
        uint32_t srcBytes = 0;             // bytes read per face (a multiple of 4)
        struct Level
        {
            uint32_t level, xBlock, yBlock; // a level stored here, at this block offset
        };
        std::vector<Level> levels;
    };

    inline uint32_t log2Ceil(uint32_t v) { return v <= 1 ? 0 : 32 - __builtin_clz(v - 1); }

    // First level of the packed mip tail (Xenia's GetPackedMipLevel)
    inline uint32_t packedMipLevel(uint32_t w, uint32_t h)
    {
        uint32_t l = log2Ceil(std::min(w, h));
        return l > 4 ? l - 4 : 0;
    }

    // Block offset of `level` (>= packedMipLevel) inside the packed mip tail of a w x h (texels)
    // texture (Xenia's GetPackedMipOffset, 2D)
    inline void packedMipOffset(uint32_t w, uint32_t h, uint32_t level, BlockFormat f, uint32_t& xb, uint32_t& yb)
    {
        uint32_t lw = log2Ceil(w), lh = log2Ceil(h), first = packedMipLevel(w, h), n = level - first;
        if (n < 3) // 16, 8 and 4 texels in: next to each other across the shorter side
        {
            xb = lw > lh ? 0 : 16u >> n;
            yb = lw > lh ? 16u >> n : 0;
        }
        else // the smaller ones: along the longer side
        {
            xb = lw > lh ? (1u << (lw - first)) >> (n - 2) : 0;
            yb = lw > lh ? 0 : (1u << (lh - first)) >> (n - 2);
        }
        xb /= f.blockW;
        yb /= f.blockH;
    }

    // The regions of a texture with `levels` levels (1 = base only) and `faces` faces (6 for a
    // cube). The base region comes first.
    inline std::vector<GuestRegion> mipRegions(uint32_t width, uint32_t height, uint32_t pitch, bool tiled, BlockFormat f,
                                               uint32_t levels, bool packed, uint32_t faces = 1)
    {
        uint32_t tail = packed ? packedMipLevel(width, height) : UINT32_MAX;
        auto blocksW = [&](uint32_t texels) { return (texels + f.blockW - 1) / f.blockW; };
        auto blocksH = [&](uint32_t texels) { return (texels + f.blockH - 1) / f.blockH; };
        auto extent = [&](GuestRegion& r) {
            for (const GuestRegion::Level& l : r.levels)
            {
                r.xBlocks = std::max(r.xBlocks, l.xBlock + blocksW(std::max(width >> l.level, 1u)));
                r.yBlocks = std::max(r.yBlocks, l.yBlock + blocksH(std::max(height >> l.level, 1u)));
            }
        };
        auto dataBytes = [&](const GuestRegion& r) {
            return tiled ? tiledUpperBound2D(r.xBlocks, r.yBlocks, r.rowBlocks, f.bytes)
                         : r.rowBlocks * f.bytes * (r.yBlocks - 1) + r.xBlocks * f.bytes;
        };
        std::vector<GuestRegion> out;

        // Level 0. Tiled pitch: aligned to 32 blocks (never below the width). Linear: 256-byte
        // rows (Direct3D already writes such a pitch).
        GuestRegion base;
        uint32_t xb = 0, yb = 0;
        if (tail == 0) packedMipOffset(width, height, 0, f, xb, yb);
        base.levels.push_back({ 0, xb, yb });
        extent(base);
        if (tiled)
            base.rowBlocks = tiledBlocksAligned(std::max(blocksW(pitch), base.xBlocks));
        else
            base.rowBlocks = std::max(((blocksW(pitch) * f.bytes + 255) & ~255u) / f.bytes, base.xBlocks);
        base.sliceStride = (base.rowBlocks * f.bytes * tiledBlocksAligned(blocksH(height)) + 4095) & ~4095u;
        base.srcBytes = dataBytes(base);
        out.push_back(base);

        // Levels 1 and up, from the mip address
        uint32_t offset = 0;
        for (uint32_t level = 1; level < levels; level++)
        {
            GuestRegion r;
            r.mip = true;
            r.offset = offset;
            if (level >= tail)
                for (uint32_t l = level; l < levels; l++)
                {
                    packedMipOffset(width, height, l, f, xb, yb);
                    r.levels.push_back({ l, xb, yb });
                }
            else
                r.levels.push_back({ level, 0, 0 });
            extent(r);
            uint32_t layoutLevel = tail == 0 ? 0 : level; // a tail at level 0 is laid out at level 0's size
            uint32_t pitchTexels = std::max((1u << log2Ceil(width)) >> layoutLevel, 1u);
            uint32_t rows = std::max((1u << log2Ceil(height)) >> layoutLevel, 1u);
            r.rowBlocks = tiledBlocksAligned(blocksW(pitchTexels));
            if (!tiled) r.rowBlocks = ((r.rowBlocks * f.bytes + 255) & ~255u) / f.bytes;
            r.sliceStride = (r.rowBlocks * f.bytes * tiledBlocksAligned(blocksH(rows)) + 4095) & ~4095u;
            r.srcBytes = dataBytes(r);
            offset += r.sliceStride * faces;
            out.push_back(r);
            if (level >= tail) break; // the tail holds the rest
        }
        for (GuestRegion& r : out) r.srcBytes = (r.srcBytes + 3) & ~3u;
        return out;
    }
}
