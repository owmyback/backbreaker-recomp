#pragma once
// Vulkan renderer for the Xenos command stream (roadmap phase 4): executes the game's draws on
// the host GPU with translated shaders, keeps EDRAM render targets as Vulkan images, performs
// resolves, and hands the resolved front buffer to the presenter at each swap.
// All hooks are called on the command processor thread.
#include <cstdint>

namespace gpu::render
{
    void disable();                         // --no-render: command stream side effects only
    void requestScreenshot(uint64_t frame); // write screenshots/frame_<N>.png when swap N is rendered
    // Internal resolution multiplier, 1-4 (2 = 2560x1440, 3 = 3840x2160 for the game's 1280x720)
    void setResolutionScale(uint32_t scale);
    // --anisotropy N (2-16): at least N:1 anisotropic filtering on textures the game minifies with
    // linear filtering and mips (the ground at a shallow angle stays sharp). Default: what the
    // texture fetch constant asks for (mostly none).
    void setAnisotropy(uint32_t n);

    void onShaderLoad(uint32_t type, const uint32_t* bigEndianWords, uint32_t dwords);
    // indexBase / indexSizeWord: DRAW_INDX DMA index buffer (physical address, VGT_DMA_SIZE)
    void onDraw(uint32_t initiator, uint32_t indexBase, uint32_t indexSizeWord);
    void onSwap(uint32_t frontBuffer, uint32_t width, uint32_t height, uint64_t frameNumber);
    // Resolves are written to guest memory by the GPU, when the recorded commands run. Before the
    // command processor lets the CPU see its progress (a fence or other memory write, an
    // interrupt, the read pointer), this waits until the writebacks recorded so far have landed.
    void syncGuestMemory();
}
