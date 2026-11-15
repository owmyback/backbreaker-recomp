#pragma once
// Renderer debug aids (issue #40). Each is off unless its environment variable is set; the
// variables are read once, at first use. docs/runtime.md ("Renderer debug aids") has examples.
// All hooks are called on the command processor thread, like the renderer's.
#include <cstdint>
#include <string>

namespace gpu::debugaids
{
    // BB_DEBUG_PS=<hash>[,<hash>...]: those pixel shaders' color 0 becomes a flat color, by
    // position in the list: magenta, cyan, yellow, green, red, blue, white, orange. A hash may be
    // shortened to its first hex digits (4834dce2 for 4834dce2...).
    // BB_DEBUG_EXPR=<GLSL vec4> replaces the flat color in all of them, e.g. vec4(r[4].xyz, 1.0)
    // or vec4(psc.c[38].xyz, 1.0). Called with each translated pixel shader before it's compiled.
    void patchPixelShader(uint64_t hash, const std::string& name, std::string& glsl);

    // BB_DEBUG_WATCH=<guest address>[,<frames>]: logs each swap where the 256 KB at that address
    // changed, every resolve whose destination overlaps those 256 KB, and with each change the
    // resolves of the last <frames> frames (default 30) wherever they went, one line per
    // destination, largest first: the CPU may have copied or compressed one into the watched range,
    // as in issue #45.
    void onResolve(uint32_t source, int x, int y, uint32_t width, uint32_t height, uint32_t destBase, uint32_t destPitch,
                   uint32_t destHeight, uint32_t bytesPerPixel, uint32_t command);
    void onSwap(uint64_t frameNumber);

    // BB_DEBUG_DUMP=<guest address>: the resolve image the renderer saves on screenshot frames
    // (0 = off), and the writer: screenshots/resolve_<address>_<frame>.png plus channel means.
    uint32_t dumpAddress();
    void writeDump(uint64_t frameNumber, const uint8_t* rgba, uint32_t width, uint32_t height, bool swapRB);
}
