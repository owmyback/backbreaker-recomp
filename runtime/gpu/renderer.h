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

    void onShaderLoad(uint32_t type, const uint32_t* bigEndianWords, uint32_t dwords);
    // indexBase / indexSizeWord: DRAW_INDX DMA index buffer (physical address, VGT_DMA_SIZE)
    void onDraw(uint32_t initiator, uint32_t indexBase, uint32_t indexSizeWord);
    void onSwap(uint32_t frontBuffer, uint32_t width, uint32_t height, uint64_t frameNumber);
}
