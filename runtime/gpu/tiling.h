#pragma once
// Xenos 2D tiling: the byte offset of a block in a tiled surface, and helpers to tile/untile a
// whole 2D surface between buffers (roadmap phase 4, issue #12). Users: the renderer's texture
// untiling and the presenter's front-buffer copy - VdSwap reports the front buffer as 8_8_8_8,
// tiled, pitch 1280, 1280x720.
//
// The address decomposition follows Xenia's `texture_address::Tiled2D` in
// `src/xenia/gpu/texture_address.h` (BSD 3-clause - see docs/licenses/xenia-LICENSE.txt).
// A hardware tiled address is not "tile index * tile size": inside a macro tile the micro tiles
// are placed by the bank/pipe bits the hardware uses to avoid bank and channel conflicts, so the
// blocks of a tile are not contiguous in memory.
#include <cstdint>
#include <cstring>

namespace gpu
{
    // Everything below is inline/constexpr: the renderer and the presenter both include this
    // header, so a plain definition here would fail to link.
    // Macro tile: 32 x 32 blocks. Micro tile: 8 x 4 blocks, so 32 micro tiles per macro tile.
    // A "block" is the texture format's block: 1x1 texels for most formats, 4x4 texels for DXT.
    constexpr uint32_t kMacroTileBlocks = 32;                      // width and height, in blocks
    constexpr uint32_t kMicroTileBlocksX = 8, kMicroTileBlocksY = 4;

    // The hardware requires the pitch of a tiled surface to be aligned to 32 blocks (Xenia asserts
    // this in Tiled2D). Storage extents are also aligned to 32 blocks per axis.
    constexpr bool tiledPitchAligned(uint32_t pitchBlocks) { return (pitchBlocks & (kMacroTileBlocks - 1)) == 0; }
    constexpr uint32_t tiledBlocksAligned(uint32_t blocks) { return (blocks + kMacroTileBlocks - 1) & ~(kMacroTileBlocks - 1); }

    // bytesPerBlock must be 1, 2, 4, 8 or 16 - the bit decomposition needs a power of two
    // (Direct3D 9 uses log2 of the block size for tiled addressing).
    constexpr unsigned bytesPerBlockLog2(uint32_t bytesPerBlock)
    {
        return bytesPerBlock == 1 ? 0 : bytesPerBlock == 2 ? 1 : bytesPerBlock == 4 ? 2 : bytesPerBlock == 8 ? 3 : 4;
    }

    // Byte offset of block (x, y) - block coordinates, not texels - relative to the start of the
    // surface. pitchBlocks must be 32-block aligned.
    constexpr uint32_t tiledOffset2D(uint32_t x, uint32_t y, uint32_t pitchBlocks, uint32_t bytesPerBlock)
    {
        unsigned l2 = bytesPerBlockLog2(bytesPerBlock);

        // "outer": origin of the 32x32 macro tile, treated as macro-tile-linear (Y, then X).
        // "inner": position of the block inside its micro tile, linear with Y above X.
        uint32_t outerBlocks = ((y >> 5) * (pitchBlocks >> 5) + (x >> 5)) << 6;
        uint32_t innerBlocks = (((y >> 1) & 0b111) << 3) | (x & 0b111);
        uint32_t outerInnerBytes = (outerBlocks | innerBlocks) << l2;

        // The micro tile position within the macro tile is carried by the bank/pipe bits.
        uint32_t bank = (y >> 4) & 0b1;
        uint32_t pipe = ((x >> 3) & 0b11) ^ (((y >> 3) & 0b1) << 1);

        return ((y & 1) << 4) | (outerInnerBytes & 0b1111) | (((outerInnerBytes >> 4) & 0b1) << 5) |
               (((outerInnerBytes >> 5) & 0b111) << 8) | (bank << 11) | (pipe << 6) | ((outerInnerBytes >> 8) << 12);
    }

    // Bytes a tiled surface needs: the address span, not width * height * bytesPerBlock. For
    // 1 and 2 bytes per block the addresses of the blocks of a tile reach past the tile's own
    // size (spans 0xA00 and 0xC00 from the tile origin - Xenia's GetTiledAddressUpperBound2D), so
    // a buffer sized to the aligned block extent alone is not enough. 4 bytes per block and more
    // need exactly the tile-major size.
    constexpr uint32_t tiledUpperBound2D(uint32_t widthBlocks, uint32_t heightBlocks, uint32_t pitchBlocks, uint32_t bytesPerBlock)
    {
        if (!widthBlocks || !heightBlocks) return 0;
        unsigned l2 = bytesPerBlockLog2(bytesPerBlock);
        uint32_t origin = tiledOffset2D((widthBlocks - 1) & ~(kMacroTileBlocks - 1),
                                       (heightBlocks - 1) & ~(kMacroTileBlocks - 1), pitchBlocks, bytesPerBlock);
        return origin + (l2 == 0 ? 0xA00 : l2 == 1 ? 0xC00 : (0x400u << l2));
    }

    // Tile and untile a whole surface. `widthBlocks` x `heightBlocks` is the surface in blocks,
    // which need not be aligned; `tiled` must hold tiledUpperBound2D() bytes. The linear side is
    // row-major with pitch pitchBlocks (aligned, and >= widthBlocks - the pitch is the row stride).
    // Blocks outside the given dimensions are left untouched - for a non-aligned surface that
    // padding is not always allocated by the guest.
    inline void untile2D(const uint8_t* tiled, uint8_t* linear, uint32_t widthBlocks, uint32_t heightBlocks,
                  uint32_t pitchBlocks, uint32_t bytesPerBlock)
    {
        for (uint32_t y = 0; y < heightBlocks; y++)
            for (uint32_t x = 0; x < widthBlocks; x++)
                memcpy(linear + (y * pitchBlocks + x) * bytesPerBlock,
                       tiled + tiledOffset2D(x, y, pitchBlocks, bytesPerBlock), bytesPerBlock);
    }

    // The inverse of untile2D, over the same block set.
    inline void tile2D(const uint8_t* linear, uint8_t* tiled, uint32_t widthBlocks, uint32_t heightBlocks,
                uint32_t pitchBlocks, uint32_t bytesPerBlock)
    {
        for (uint32_t y = 0; y < heightBlocks; y++)
            for (uint32_t x = 0; x < widthBlocks; x++)
                memcpy(tiled + tiledOffset2D(x, y, pitchBlocks, bytesPerBlock),
                       linear + (y * pitchBlocks + x) * bytesPerBlock, bytesPerBlock);
    }
}
