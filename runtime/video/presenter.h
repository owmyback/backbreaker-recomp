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
    // Size, position and fullscreen state are read from game/settings.ini (issue #9,
    // runtime/settings.h) and written back when the window closes, with the file's other keys
    // (resolution_scale, #115) as they were; startFullscreen forces fullscreen whatever the file says.
    void run(bool startFullscreen = false);

    // BB_PERF (#174): what presenting cost since the last call. Both threads share one queue, so
    // a present that holds the queue (or its lock) holds the renderer's submits back.
    struct PresentStats
    {
        uint64_t presents = 0;
        double fenceMs = 0;   // waiting for the last present's own commands
        double acquireMs = 0; // in vkAcquireNextImageKHR
        double queueMs = 0, queueMaxMs = 0; // holding the queue lock: the submit and vkQueuePresentKHR
        uint64_t skippedFrames = 0;         // game frames that were never presented
    };
    PresentStats takePresentStats();
}
