// Checks runtime/gpu/resolve_tiles.h (#198): which resolve destination a resolve is a tile of. No game
// files, no Vulkan:
//   clang++ -std=c++20 -Wall -Wextra -O2 tests/resolve_tiles_test.cpp -o build/resolve_tiles_test
//
// The first checks are the issue's report: the match's field black but for its left quarter, under
// the interface and the controlled player's outline. The addresses are the ones in that run's log.
#include "../runtime/gpu/resolve_tiles.h"

#include <algorithm>
#include <cstdio>
#include <map>

using namespace gpu::tiles;

static int failures = 0, checks = 0;

static void check(bool ok, const char* what)
{
    checks++;
    if (!ok)
    {
        failures++;
        printf("FAIL %s\n", what);
    }
}

// A resolve destination as the renderer's lookup sees it
struct Destination
{
    uint32_t format, pitch, bytes, width, height;
    bool recent; // resolved this frame or the last
};
using Map = std::map<uint32_t, Destination>;

constexpr uint32_t kColor8 = 37, kHalf4 = 97; // any two formats

static uint32_t largestSpan(const Map& m)
{
    uint32_t s = 0;
    for (const auto& [address, d] : m) s = std::max(s, span(d.width, d.height, d.pitch, d.bytes));
    return s;
}

// The destination a `w` x `h` resolve of `format` to `address` goes into (its address; 0 = none, it
// gets an image of its own) and where
static uint32_t into(Map& m, uint32_t address, uint32_t format, uint32_t pitch, uint32_t bytes, uint32_t w, uint32_t h, uint32_t& x, uint32_t& y)
{
    auto usable = [&](const Destination& d, uint32_t& dw, uint32_t& dh) {
        if (d.format != format || d.pitch != pitch || d.bytes != bytes || !d.recent) return false;
        dw = d.width;
        dh = d.height;
        return true;
    };
    auto it = find(m, address, pitch, bytes, w, h, largestSpan(m), usable, x, y);
    return it == m.end() ? 0 : it->first;
}

int main()
{
    uint32_t x = 0, y = 0;

    // The report. The scene: 1280x720, 16_16_16_16_FLOAT (8 bytes a pixel), resolved in four
    // 320x720 strips, 80 KB apart. Around it, what Tackle Alley's loading left: cube map faces
    // and their mips, a 256x256 map and a depth resolve.
    {
        Map m;
        const uint32_t scene = 0x0E5E1000, strip = 0x14000;
        for (uint32_t a = 0x0E5DC000; a <= 0x0E5E0000; a += 0x1000) m[a] = { kColor8, 32, 4, 32, 16, false };
        for (uint32_t a = 0x0E5E2000; a <= 0x0E5E7000; a += 0x1000) m[a] = { kColor8, 32, 4, 32, 8, false };
        for (uint32_t a = 0x0E5E8000; a <= 0x0E5FC000; a += 0x4000) m[a] = { kColor8, 64, 4, 64, 64, false };
        m[0x0E600000] = { kColor8, 256, 4, 256, 256, false };
        m[0x0E602000] = { 100, 1280, 4, 1280, 720, true }; // this match's depth
        m[scene] = { kHalf4, 1280, 8, 1280, 720, true };   // the first strip made it

        // Ten destinations lie between the scene's address and the second strip's, four of them
        // between the second strip's and the third's: more than the four the lookup used to try
        int toSecond = 0, toThird = 0;
        for (const auto& [a, d] : m)
        {
            toSecond += a > scene && a < scene + strip;
            toThird += a > scene + strip && a < scene + 2 * strip;
        }
        check(toSecond == 10, "ten other destinations between the first strip and the second");
        check(toThird == 4, "four between the second strip and the third");

        check(into(m, scene + strip, kHalf4, 1280, 8, 320, 720, x, y) == scene && x == 320 && y == 0, "the second strip is the scene's, at x 320");
        check(into(m, scene + 2 * strip, kHalf4, 1280, 8, 320, 720, x, y) == scene && x == 640 && y == 0, "the third strip is the scene's, at x 640");
        check(into(m, scene + 3 * strip, kHalf4, 1280, 8, 320, 720, x, y) == scene && x == 960 && y == 0, "the fourth strip is the scene's, at x 960");
        check(into(m, scene, kHalf4, 1280, 8, 320, 720, x, y) == 0, "the first strip, at the scene's own address, is nobody's tile");
        check(into(m, scene + 4 * strip, kHalf4, 1280, 8, 320, 720, x, y) == 0, "a fifth strip would not fit");
        check(into(m, scene + strip, kHalf4, 1280, 8, 640, 720, x, y) == scene && x == 320, "a wider tile fits there too");
        check(into(m, scene + 3 * strip, kHalf4, 1280, 8, 640, 720, x, y) == 0, "but not at the last strip's place");
        check(into(m, scene + strip + 0x1000, kHalf4, 1280, 8, 320, 720, x, y) == 0, "an address that is no tile's place");
        check(into(m, scene + strip, kColor8, 1280, 8, 320, 720, x, y) == 0, "not for another format");
        check(into(m, scene + strip, kHalf4, 1024, 8, 320, 720, x, y) == 0, "not for another pitch");
        check(into(m, scene + strip, kHalf4, 1280, 4, 320, 720, x, y) == 0, "not for another pixel size");

        // What the old lookup made: strips two and three with images of their own. Once they exist
        // the scene is still found first for them, and nothing is resolved into them again.
        m[scene + strip] = { kHalf4, 1280, 8, 1280, 720, false };
        m[scene + 2 * strip] = { kHalf4, 1280, 8, 1280, 720, false };
        check(into(m, scene + strip, kHalf4, 1280, 8, 320, 720, x, y) == scene && x == 320, "a stale image at the strip's own address doesn't take it");
        check(into(m, scene + 3 * strip, kHalf4, 1280, 8, 320, 720, x, y) == scene && x == 960, "nor does a stale image just below it");

        // An older image that merely covers the address is memory the game has handed out again (#140)
        m[scene].recent = false;
        check(into(m, scene + strip, kHalf4, 1280, 8, 320, 720, x, y) == 0, "not into an image last resolved long ago");
    }

    // The nearest one that fits wins: two scenes of the same kind in a row, the second's strips its own
    {
        Map m;
        const uint32_t first = 0x04000000, second = first + span(1280, 720, 1280, 8);
        m[first] = { kHalf4, 1280, 8, 1280, 720, true };
        m[second] = { kHalf4, 1280, 8, 1280, 720, true };
        check(into(m, second + 0x14000, kHalf4, 1280, 8, 320, 720, x, y) == second && x == 320, "the nearest image that has the place");
        check(into(m, second, kHalf4, 1280, 8, 320, 720, x, y) == 0, "an address just past an image is not inside it");
        check(into(m, first + 0x14000, kHalf4, 1280, 8, 320, 720, x, y) == first && x == 320, "the first one's strips are its own");
    }

    // The crowd's impostor atlas (#55): 1024 wide, 2 bytes a pixel, a row of fans (128 pixel rows) at
    // a time, each 256 KB on
    {
        Map m;
        const uint32_t atlas = 0x08714000;
        m[atlas] = { kColor8, 1024, 2, 1024, 1024, true };
        for (uint32_t row = 1; row < 8; row++)
        {
            bool ok = into(m, atlas + row * 0x40000, kColor8, 1024, 2, 1024, 128, x, y) == atlas && x == 0 && y == row * 128;
            check(ok, "an atlas row lands 128 rows below the one before");
        }
        check(into(m, atlas + 8 * 0x40000, kColor8, 1024, 2, 1024, 128, x, y) == 0, "there is no ninth row");
    }

    // Many destinations far below don't make it look further than one can reach
    {
        Map m;
        for (uint32_t a = 0x01000000; a < 0x01100000; a += 0x1000) m[a] = { kColor8, 32, 4, 32, 32, true };
        int asked = 0;
        auto counting = [&](const Destination&, uint32_t&, uint32_t&) { asked++; return false; };
        find(m, 0x02000000, 32, 4, 32, 32, largestSpan(m), counting, x, y);
        check(asked == 0, "destinations too far below are not asked");
        find(m, 0x01100000, 32, 4, 32, 32, largestSpan(m), counting, x, y);
        check(asked == 0, "nor is one that ends where the address begins");
        find(m, 0x010FF800, 32, 4, 32, 32, largestSpan(m), counting, x, y);
        check(asked == 1, "only the one a 4 KB span reaches from");
    }

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
