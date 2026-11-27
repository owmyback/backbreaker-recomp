#pragma once
// Which resolve destination a resolve is a tile of (#198).
//
// The game renders a large target in tiles (predicated tiling: the match's 1280x720 2x MSAA scene is
// four 320x720 strips, 1152x640 two of 576x640) and resolves each with RB_COPY_DEST_BASE moved to the
// tile's place inside the same tiled texture. Texture atlases filled a strip at a time do the same
// (the crowd's impostors, #55). The renderer keeps one image per destination address, so such a
// resolve has to find the image it lands in: a destination at a lower address, of the same pitch and
// pixel size, in whose tiled layout (Xenia's Tiled2D, tiling.h) the address difference is a place the
// tile fits.
//
// It used to look at the four destinations just below the address and no further. Destinations stay
// in the map after the game has freed their memory (renderer.cpp, resolveInto), and the strips of
// the 1280x720 scene are 80 KB apart. An Exhibition game started after a Tackle Alley game in the
// same session has the scene's texture where Tackle Alley's loading resolved cube map mips: ten old
// destinations, 4 KB apart, between the first strip and the second, and four more below the third.
// Each of those strips found no image and got one of its own, the texture the game samples had only
// its left quarter, and the rest of the field was black under the interface and the controlled
// player's outline.
//
// Here every destination is looked at whose address is close enough to contain the tile at all
// (`maxSpan`: the most bytes any destination spans), nearest first.
//
// Header-only, with no Vulkan in it, so tests/resolve_tiles_test.cpp can check it on its own.
#include "tiling.h"

#include <cstdint>

namespace gpu::tiles
{
    // Bytes from a tiled destination's address to the end of its last pixel: a tile of it starts
    // less than this past it. `bytes` per pixel is 1, 2, 4, 8 or 16.
    constexpr uint32_t span(uint32_t width, uint32_t height, uint32_t pitch, uint32_t bytes)
    {
        return tiledUpperBound2D(width, height, tiledBlocksAligned(pitch), bytes);
    }

    // The place of a tile that starts `delta` bytes past the address of a `width` x `height`
    // destination: the 32-aligned (x, y) with that tiled offset at which a `tileWidth` x `tileHeight`
    // tile stays inside it. False when there is none.
    constexpr bool place(uint32_t delta, uint32_t pitch, uint32_t bytes, uint32_t width, uint32_t height, uint32_t tileWidth,
                         uint32_t tileHeight, uint32_t& x, uint32_t& y)
    {
        for (uint32_t ty = 0; ty + tileHeight <= height; ty += kMacroTileBlocks)
            for (uint32_t tx = 0; tx + tileWidth <= width; tx += kMacroTileBlocks)
                if (tiledOffset2D(tx, ty, tiledBlocksAligned(pitch), bytes) == delta)
                {
                    x = tx;
                    y = ty;
                    return true;
                }
        return false;
    }

    // The destination below `address` that a `tileWidth` x `tileHeight` resolve to `address` is a
    // tile of, and the tile's place (x, y) in it; `destinations.end()` when it is none's.
    // `destinations` is a std::map by guest address. `usable(destination, width, height)` says
    // whether one can take the tile at all (same format, pitch and pixel size, resolved recently)
    // and gives its size in guest pixels. `maxSpan` is the largest span() of any destination.
    template <class Map, class Usable>
    typename Map::iterator find(Map& destinations, uint32_t address, uint32_t pitch, uint32_t bytes, uint32_t tileWidth, uint32_t tileHeight,
                                uint32_t maxSpan, Usable&& usable, uint32_t& x, uint32_t& y)
    {
        auto it = destinations.lower_bound(address);
        while (it != destinations.begin())
        {
            --it;
            uint32_t delta = address - it->first;
            if (delta >= maxSpan) break; // too far below to reach the address, and so is every one before it
            uint32_t width = 0, height = 0;
            if (usable(it->second, width, height) && place(delta, pitch, bytes, width, height, tileWidth, tileHeight, x, y)) return it;
        }
        return destinations.end();
    }
}
