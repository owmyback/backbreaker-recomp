// Checks runtime/gpu/mip_layout.h (issue #46): where each mip level of a texture is in guest
// memory. No game files, no generated headers - see README "Tests" for the build command.
//
// Expected values are hand-computed from Xenia's GetGuestTextureLayout / GetPackedMipOffset
// (texture_util.cc). Two cases are the examples Xenia's own comments describe: an 80x260 tiled
// texture whose packed mip tail (level 3) holds a level ending at Y = 36, and a 2x2 linear
// texture whose base is at (16, 0) in its tail and its 1x1 mip at (8, 0).
#include "../runtime/gpu/mip_layout.h"

#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

using gpu::BlockFormat;
using gpu::GuestRegion;
using gpu::mipRegions;

constexpr BlockFormat kDxt1{ 4, 4, 8 }, kRgba8{ 1, 1, 4 };

struct Expect
{
    bool mip;
    uint32_t offset, rowBlocks, sliceStride, xBlocks, yBlocks;
    std::vector<GuestRegion::Level> levels;
};

static void check(const char* name, const std::vector<GuestRegion>& got, const std::vector<Expect>& want)
{
    CHECK(got.size() == want.size(), "%s: %zu regions, expected %zu", name, got.size(), want.size());
    for (size_t i = 0; i < got.size() && i < want.size(); i++)
    {
        const GuestRegion& g = got[i];
        const Expect& w = want[i];
        CHECK(g.mip == w.mip && g.offset == w.offset, "%s region %zu: %s+%u, expected %s+%u", name, i, g.mip ? "mip" : "base", g.offset,
              w.mip ? "mip" : "base", w.offset);
        CHECK(g.rowBlocks == w.rowBlocks, "%s region %zu: pitch %u blocks, expected %u", name, i, g.rowBlocks, w.rowBlocks);
        CHECK(g.sliceStride == w.sliceStride, "%s region %zu: face stride %u, expected %u", name, i, g.sliceStride, w.sliceStride);
        CHECK(g.xBlocks == w.xBlocks && g.yBlocks == w.yBlocks, "%s region %zu: extent %ux%u blocks, expected %ux%u", name, i, g.xBlocks,
              g.yBlocks, w.xBlocks, w.yBlocks);
        CHECK(g.srcBytes % 4 == 0, "%s region %zu: %u bytes read, not a multiple of 4", name, i, g.srcBytes);
        CHECK(g.levels.size() == w.levels.size(), "%s region %zu: %zu levels, expected %zu", name, i, g.levels.size(), w.levels.size());
        for (size_t l = 0; l < g.levels.size() && l < w.levels.size(); l++)
            CHECK(g.levels[l].level == w.levels[l].level && g.levels[l].xBlock == w.levels[l].xBlock && g.levels[l].yBlock == w.levels[l].yBlock,
                  "%s region %zu: level %u at (%u, %u), expected level %u at (%u, %u)", name, i, g.levels[l].level, g.levels[l].xBlock,
                  g.levels[l].yBlock, w.levels[l].level, w.levels[l].xBlock, w.levels[l].yBlock);
    }
}

int main()
{
    // 256x256 DXT1, tiled, packed, 9 levels. Levels 1-3 are 32-block-aligned tiles of 8 KB
    // (128, 64, 32 texels: 32, 16, 8 blocks, all padded to 32x32 blocks). The tail starts at
    // level 4 (16x16): 16x16 at x = 16 texels, 8x8 at 8, 4x4 at 4, then 2x2 at y = 8 and 1x1 at
    // y = 4 (Xenia's diagram).
    check("256x256 DXT1", mipRegions(256, 256, 256, true, kDxt1, 9, true),
          { { false, 0, 64, 32768, 64, 64, { { 0, 0, 0 } } },
            { true, 0, 32, 8192, 32, 32, { { 1, 0, 0 } } },
            { true, 8192, 32, 8192, 16, 16, { { 2, 0, 0 } } },
            { true, 16384, 32, 8192, 8, 8, { { 3, 0, 0 } } },
            { true, 24576, 32, 8192, 8, 4, { { 4, 4, 0 }, { 5, 2, 0 }, { 6, 1, 0 }, { 7, 0, 2 }, { 8, 0, 1 } } } });

    // 512x1024 DXT1 (the size of the field texture), taller than wide: the tail (level 5,
    // 16x32) lays its first three levels out along x, the rest along y.
    check("512x1024 DXT1", mipRegions(512, 1024, 512, true, kDxt1, 11, true),
          { { false, 0, 128, 262144, 128, 256, { { 0, 0, 0 } } },
            { true, 0, 64, 65536, 64, 128, { { 1, 0, 0 } } },
            { true, 65536, 32, 16384, 32, 64, { { 2, 0, 0 } } },
            { true, 81920, 32, 8192, 16, 32, { { 3, 0, 0 } } },
            { true, 90112, 32, 8192, 8, 16, { { 4, 0, 0 } } },
            { true, 98304, 32, 8192, 8, 8, { { 5, 4, 0 }, { 6, 2, 0 }, { 7, 1, 0 }, { 8, 0, 4 }, { 9, 0, 2 }, { 10, 0, 1 } } } });

    // 80x260 8_8_8_8, tiled, packed (Xenia's example): mips are laid out from the power-of-two
    // size 128x512, so level 1 is 64 x 256 rows (64 KB) and level 2 32 x 128 (16 KB). The tail is
    // level 3, 32 x 64 rows; level 6 (1x4) sits at y = 32 and ends at y = 36.
    check("80x260 8_8_8_8", mipRegions(80, 260, 96, true, kRgba8, 9, true),
          { { false, 0, 96, 110592, 80, 260, { { 0, 0, 0 } } },
            { true, 0, 64, 65536, 40, 130, { { 1, 0, 0 } } },
            { true, 65536, 32, 16384, 20, 65, { { 2, 0, 0 } } },
            { true, 81920, 32, 8192, 26, 36, { { 3, 16, 0 }, { 4, 8, 0 }, { 5, 4, 0 }, { 6, 0, 32 }, { 7, 0, 16 }, { 8, 0, 8 } } } });

    // 16x16 DXT1 with mips: the tail is level 0, so the base sits in its own tail at x = 16 texels
    // (4 blocks), and levels 1-4 share the tail at the mip address, laid out at level 0's size.
    check("16x16 DXT1", mipRegions(16, 16, 32, true, kDxt1, 5, true),
          { { false, 0, 32, 8192, 8, 4, { { 0, 4, 0 } } },
            { true, 0, 32, 8192, 4, 3, { { 1, 2, 0 }, { 2, 1, 0 }, { 3, 0, 2 }, { 4, 0, 1 } } } });

    // 2x2 8_8_8_8 linear (Xenia's example): base at (16, 0), the 1x1 mip at (8, 0). Rows are
    // 256 bytes; only the rows the levels use are read (2 and 1), not 32.
    {
        std::vector<GuestRegion> r = mipRegions(2, 2, 64, false, kRgba8, 2, true);
        check("2x2 8_8_8_8 linear", r,
              { { false, 0, 64, 8192, 18, 2, { { 0, 16, 0 } } },
                { true, 0, 64, 8192, 9, 1, { { 1, 8, 0 } } } });
        if (r.size() == 2)
        {
            CHECK(r[0].srcBytes == 256 + 18 * 4, "2x2 linear base reads %u bytes, expected %u", r[0].srcBytes, 256 + 18 * 4);
            CHECK(r[1].srcBytes == 9 * 4, "2x2 linear mip tail reads %u bytes, expected %u", r[1].srcBytes, 9 * 4);
        }
    }

    // 128x128 DXT1 cube with mips: a level holds all six faces before the next level, so the mip
    // offsets step by 6 x the face stride.
    check("128x128 DXT1 cube", mipRegions(128, 128, 128, true, kDxt1, 8, true, 6),
          { { false, 0, 32, 8192, 32, 32, { { 0, 0, 0 } } },
            { true, 0, 32, 8192, 16, 16, { { 1, 0, 0 } } },
            { true, 49152, 32, 8192, 8, 8, { { 2, 0, 0 } } },
            { true, 98304, 32, 8192, 8, 4, { { 3, 4, 0 }, { 4, 2, 0 }, { 5, 1, 0 }, { 6, 0, 2 }, { 7, 0, 1 } } } });

    // Without packed mips every level is its own 32x32-block region down to 1x1.
    check("64x64 8_8_8_8 unpacked", mipRegions(64, 64, 64, true, kRgba8, 7, false),
          { { false, 0, 64, 16384, 64, 64, { { 0, 0, 0 } } },
            { true, 0, 32, 4096, 32, 32, { { 1, 0, 0 } } },
            { true, 4096, 32, 4096, 16, 16, { { 2, 0, 0 } } },
            { true, 8192, 32, 4096, 8, 8, { { 3, 0, 0 } } },
            { true, 12288, 32, 4096, 4, 4, { { 4, 0, 0 } } },
            { true, 16384, 32, 4096, 2, 2, { { 5, 0, 0 } } },
            { true, 20480, 32, 4096, 1, 1, { { 6, 0, 0 } } } });

    if (failures) printf("%d failure(s)\n", failures);
    else printf("mip layout: all checks passed\n");
    return failures ? 1 : 0;
}
