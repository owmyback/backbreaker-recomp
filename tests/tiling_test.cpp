// Checks runtime/gpu/tiling.h (issue #12, roadmap phase 4): the tiled byte offset of a block in a
// 2D surface, and the tile/untile helpers. No game files, no generated headers - see README
// "Tests" for the build command, which compiles this file together with tests/tiling_test_odr.cpp
// (a second translation unit including the header, so a non-inline definition fails to link).
//
// Spot values below are hand-computed from the address decomposition in tiling.h. With
// oib = (outerBlocks | innerBlocks) << log2(bytesPerBlock), the byte offset is:
//   bits [3:0]   oib[3:0]      = inner blocks low bits = {y[1], x[2:0]}
//   bit  [4]     y & 1
//   bit  [5]     oib[4]        = y[2] (when bytesPerBlock == 1)
//   bits [7:6]   pipe          = x[4:3] ^ (y[3] << 1)
//   bits [10:8]  oib[7:5]      = y[3] (when bytesPerBlock == 1) plus the 2 low bits of the tile index
//   bit  [11]    bank          = y[4]
//   bits [12+]   oib[8+]       = tile index >> 2
// For a single macro tile (pitch 32 blocks) the tile index is 0, so only the micro tile fields
// contribute - and note that they are not row-major within the tile.
#include "../runtime/gpu/tiling.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

using gpu::tiledOffset2D;
using gpu::tiledUpperBound2D;
using gpu::tiledBlocksAligned;

// tests/tiling_test_odr.cpp: same header, second translation unit. Building both together is what
// catches a definition in the header that is not inline.
extern int odrCheck();

// Tile then untile must be the identity over the blocks inside the given dimensions.
static void checkRoundTrip(uint32_t widthBlocks, uint32_t heightBlocks, uint32_t pitchBlocks, uint32_t bytesPerBlock)
{
    // The linear side is row-major with pitch pitchBlocks, so it has padding columns when
    // widthBlocks is smaller than the pitch. Only blocks inside the dimensions are compared.
    std::vector<uint8_t> linear(pitchBlocks * heightBlocks * bytesPerBlock);
    std::vector<uint8_t> tiled(tiledUpperBound2D(widthBlocks, heightBlocks, pitchBlocks, bytesPerBlock), 0);
    std::vector<uint8_t> back(pitchBlocks * heightBlocks * bytesPerBlock, 0);
    for (size_t i = 0; i < linear.size(); i++) linear[i] = uint8_t(i * 31 + 7);
    gpu::tile2D(linear.data(), tiled.data(), widthBlocks, heightBlocks, pitchBlocks, bytesPerBlock);
    gpu::untile2D(tiled.data(), back.data(), widthBlocks, heightBlocks, pitchBlocks, bytesPerBlock);
    for (uint32_t y = 0; y < heightBlocks; y++)
        for (uint32_t x = 0; x < widthBlocks; x++)
        {
            size_t i = (y * pitchBlocks + x) * bytesPerBlock;
            for (uint32_t b = 0; b < bytesPerBlock; b++)
                CHECK(linear[i + b] == back[i + b], "round trip block (%u,%u) byte %u differs", x, y, b);
        }
}

// Offsets must be distinct, block aligned, and inside the address span of the surface.
static void checkPermutation(uint32_t widthBlocks, uint32_t heightBlocks, uint32_t pitchBlocks, uint32_t bytesPerBlock)
{
    uint32_t span = tiledUpperBound2D(widthBlocks, heightBlocks, pitchBlocks, bytesPerBlock);
    std::vector<uint8_t> seen(span, 0);
    uint32_t maxOffset = 0;
    for (uint32_t y = 0; y < heightBlocks; y++)
        for (uint32_t x = 0; x < widthBlocks; x++)
        {
            uint32_t off = tiledOffset2D(x, y, pitchBlocks, bytesPerBlock);
            CHECK(off % bytesPerBlock == 0, "offset of block (%u,%u) not block aligned: %u", x, y, off);
            CHECK(off < span, "offset of block (%u,%u) past the address span: %u >= %u", x, y, off, span);
            if (off >= span) continue;
            CHECK(seen[off] == 0, "block (%u,%u) collides with another block at offset %u", x, y, off);
            seen[off] = 1;
            if (off > maxOffset) maxOffset = off;
        }
    uint32_t used = 0;
    for (uint8_t v : seen) if (v) used++;
    CHECK(used == widthBlocks * heightBlocks, "permutation: %u distinct offsets for %u blocks", used, widthBlocks * heightBlocks);

    // 4 bytes per block and more: a macro tile is 4 KB or larger, so each tile sits in its own
    // region and the offsets cover the whole extent exactly (Xenia's note about 4bpb+ tiling).
    if (bytesPerBlock >= 4)
        CHECK(maxOffset + bytesPerBlock == span && used == span / bytesPerBlock,
              "bpb %u: offsets should cover the extent exactly (used %u, max %u, span %u)", bytesPerBlock, used, maxOffset, span);
}

int main()
{
    // Single macro tile (pitch 32 blocks), 1 byte per block.
    CHECK(tiledOffset2D(0, 0, 32, 1) == 0, "tile origin (0,0)");
    CHECK(tiledOffset2D(1, 0, 32, 1) == 1, "block (1,0): inner low bits x[2:0]=1");
    CHECK(tiledOffset2D(8, 0, 32, 1) == 64, "block (8,0): inner=0, pipe=x[4:3]=01 -> 64, not 32 (micro tiles are not row-major)");
    CHECK(tiledOffset2D(0, 1, 32, 1) == 16, "block (0,1): y&1 -> bit 4");
    CHECK(tiledOffset2D(1, 1, 32, 1) == 17, "block (1,1): bit 4 + x[2:0]");
    CHECK(tiledOffset2D(4, 2, 32, 1) == 12, "block (4,2): y[1] -> bit 3 (8) + x[2:0]=4");
    // (31,31): bank y[4]=1 (2048) + y[3] -> bit 8 (256) + pipe = 011 ^ 010 = 001 (64) +
    //          y[2] -> bit 5 (32) + y&1 -> bit 4 (16) + y[1] -> bit 3 (8) + x[2:0]=7.
    CHECK(tiledOffset2D(31, 31, 32, 1) == 2431, "block (31,31) inside the 0xA00 span of the tile");

    // Single macro tile, other block sizes (oib scaled by bytesPerBlock).
    CHECK(tiledOffset2D(1, 0, 32, 2) == 2, "bpb 2, block (1,0): inner=1 -> 2");
    CHECK(tiledOffset2D(4, 2, 32, 4) == 288, "bpb 4, block (4,2): inner=12 -> oib 48 -> bit 5 (32) + bit 8 (256)");
    CHECK(tiledOffset2D(1, 1, 32, 8) == 24, "bpb 8, block (1,1): oib 8 -> bit 3 (8) + bit 4 (16)");
    CHECK(tiledOffset2D(1, 0, 32, 16) == 32, "bpb 16, block (1,0): oib 16 -> bit 5");
    CHECK(tiledOffset2D(31, 31, 32, 16) == 16240, "bpb 16, block (31,31): oib 1008 -> 12288 + 768 + 32 + pipe 64 + bank 2048 + y&1 16");

    // Macro tile origins. For 1 byte per block, origins are 0x200-aligned and 4 tiles share each
    // 4 KB page: origin(t) = (t >> 2) * 0x1000 + (t & 3) * 0x200.
    CHECK(tiledOffset2D(32, 0, 32, 1) == 512, "tile 1 origin = 0x200");
    CHECK(tiledOffset2D(0, 128, 32, 1) == 4096, "tile 4 origin = 0x1000 (four 1bpb tiles per page)");
    // For 4 bytes per block and more the tile is 4 KB or larger: origin(t) = t * 1024 * bpb.
    CHECK(tiledOffset2D(3 * 32, 1 * 32, 160, 8) == 8 * 1024 * 8, "DXT1-like pitch 160: tile 8 origin");

    // The front buffer as VdSwap reports it: pitch 1280 blocks, 1280x720, 8_8_8_8 - a 1x1 texel
    // block, so 4 bytes per block. Block (1000,500): tile index t = (500>>5)*(1280>>5) + (1000>>5)
    // = 15*40 + 31 = 631, inner blocks = ((500>>1)&7)<<3 | (1000&7) = 2<<3 = 16, so
    // oib = (631<<6 | 16) << 2 = 161600:
    //   (161600>>8) << 12 = 631 * 4096 = 2584576;  ((161600>>5)&7) << 8 = 2 * 256 = 512;
    //   ((161600>>4)&1) << 5 = 0;  low nibble 0;  bank = (500>>4)&1 = 1 -> 2048;
    //   pipe = ((1000>>3)&3) ^ (((500>>3)&1)<<1) = 1 -> 64;  y&1 = 0.
    // Total 2587200 = tile origin 2584576 + 2624 inside the tile.
    CHECK(tiledOffset2D(1000, 500, 1280, 4) == 2587200, "front buffer block (1000,500) at 4 bytes per block");
    // Storage for it: 1280 x 736 blocks (720 rows rounded up to 32) = 3768320 bytes, more than the
    // 1280 * 720 * 4 = 3686400 a caller would expect from the visible rows.
    CHECK(tiledUpperBound2D(1280, 720, 1280, 4) == 3768320, "front buffer span at 4 bytes per block");
    // The same dimensions at 1 byte per block are bpb-1 spot checks, not the 8_8_8_8 front buffer.
    // There the span is the 0xA00 tile span: 942080, past the 1280 * 720 = 921600 linear size.
    CHECK(tiledOffset2D(1000, 500, 1280, 1) == 646752, "bpb 1 spot check on the front buffer dims, block (1000,500)");
    CHECK(tiledUpperBound2D(1280, 720, 1280, 1) == 942080, "bpb 1 span on the front buffer dims");

    // Round trip identity, sizes that are and are not multiples of 32 blocks.
    for (uint32_t bpb : {1u, 2u, 4u, 8u, 16u})
        for (auto s : std::vector<std::array<uint32_t, 3>>{{ {32, 32, 32}, {64, 48, 64}, {33, 33, 64}, {8, 8, 32}, {160, 128, 160}, {1280, 720, 1280}, {1280, 48, 1408} }})
            checkRoundTrip(s[0], s[1], s[2], bpb);

    // Permutation over pitch-aligned surfaces (and a pitch that only differs from the width).
    for (uint32_t bpb : {1u, 2u, 4u, 8u, 16u})
        for (auto s : std::vector<std::array<uint32_t, 3>>{{ {32, 32, 32}, {64, 64, 64}, {96, 224, 96}, {160, 128, 160}, {320, 256, 320}, {1280, 256, 1280}, {1280, 32, 1408} }})
            checkPermutation(s[0], s[1], s[2], bpb);

    // Alignment helpers.
    CHECK(gpu::tiledPitchAligned(1280) && gpu::tiledPitchAligned(320) && !gpu::tiledPitchAligned(1279), "pitch alignment");
    CHECK(tiledBlocksAligned(720) == 736 && tiledBlocksAligned(32) == 32 && tiledBlocksAligned(1) == 32, "extent alignment");

    // Second translation unit (link check for inline): 17 + 2560 + 1 + 64.
    CHECK(odrCheck() == 2642, "second translation unit odrCheck() = %d", odrCheck());

    printf("%s (%d failures)\n", failures ? "FAILED" : "all tiling checks passed", failures);
    return failures != 0;
}
