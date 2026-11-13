#pragma once
// Window + Vulkan presentation of the guest front buffer (roadmap phase 4, step 1).
// The presenter runs on its own thread (the process main thread, for SDL); the guest only
// publishes "a new frame is in the front buffer" and never waits on the window.
#include <vulkan/vulkan.h>

#include <cstdint>

namespace video
{
    // Called by the renderer for a swap it rendered, before publishFrame for the same frame.
    // `image` (from the shared vkctx device) stays alive and in TRANSFER_SRC_OPTIMAL between
    // the renderer's queue submissions.
    void publishRenderedFrame(VkImage image, uint32_t width, uint32_t height, uint64_t frameNumber);

    // Called by the command processor at every guest swap. Lock-free, never blocks.
    void publishFrame(uint32_t frontBufferPhys, uint32_t width, uint32_t height, uint64_t frameNumber);

    // Opens the window and presents until the window is closed. Returns normally on close;
    // calls fatal() if SDL/Vulkan cannot be initialized.
    void run();
}
