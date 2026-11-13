#pragma once
// GPU frame capture: records everything the command processor sees during one frame
// (draws, shader loads, register state, textures, render targets) to captures/frame_NNNNN/.
// Used to learn what the renderer (roadmap phase 4) must support.
#include <cstdint>
#include <string>

namespace gpu::capture
{
    // Capture the frame that ends with swap number `frame` (1-based), into `dir`.
    // Can be called several times to capture several frames in one run.
    void request(uint64_t frame, const std::string& dir = "captures");

    // Write every unique shader the game loads during the run to `dir` (vs_/ps_<hash>.bin).
    void dumpAllShaders(const std::string& dir);

    // Hooks called by the command processor (cheap when no capture is active)
    void onPacket(uint32_t opcode);
    void onShaderLoad(uint32_t type, uint32_t start, uint32_t dwords, const uint32_t* bigEndianWords);
    void onDraw(uint32_t opcode, uint32_t initiator, uint32_t indexBase, uint32_t indexSizeWord);
    void onSwap(uint64_t frameNumber, uint32_t frontBuffer, uint32_t width, uint32_t height);
}
