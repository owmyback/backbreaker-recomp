#pragma once
// In-window overlay drawn by the runtime on top of the game (issue #101, roadmap phase 11 parts
// 3-5): the achievements list (F1 or the controller's Guide/PS button) and an "Achievement
// unlocked" pop-up. The game has no achievements screen of its own; on the console it was the
// Guide's. Also the fps readout (show_fps, #146): the game's own frames a second and speed.
//
// Everything here runs on the presenter thread (window events, controller presses, the present
// path). Drawing happens on the CPU (SDL's software renderer and its built-in 8x8 font, icons
// decoded with SDL_LoadPNG_IO), only when what is shown changes; the present path then blits the
// visible panels onto the swapchain image. Hidden, it costs one atomic load per present.
#include <vulkan/vulkan.h>

#include <cstdint>

union SDL_Event;
namespace vkctx { struct Context; }

namespace overlay
{
    // A window event. Returns true if the overlay used it (F1, and while the list is open its
    // navigation keys, Enter, and the mouse's clicks and wheel).
    bool handleEvent(const SDL_Event& e);

    // Controller presses from hid::takeMenuPresses() (Guide toggles the list; while it is open:
    // d-pad / left stick to move, LB/RB a page, A to read the selected achievement in full and
    // back, B or Back to go back to the list, then to close it).
    void handlePad(uint32_t presses);

    // Present path: dst holds the game's picture in TRANSFER_DST_OPTIMAL and stays in that layout.
    // Records nothing while nothing is shown. The previous submission must have completed (the
    // presenter waits on its fence first), since the staging buffer is rewritten here.
    void record(vkctx::Context& ctx, VkCommandBuffer cmd, VkImage dst, VkExtent2D extent);

    // The fps readout in the top-left corner (settings.ini show_fps / --show-fps, #146): the game's
    // frames, speed and longest frame over the last second (gpu::lastSecond()). Hidden while the
    // achievements list is open. Before the window opens.
    void setShowFps(bool on);

    // Frees the GPU resources (device idle).
    void destroy(VkDevice device);

    // --overlay-preview: draws the list and a pop-up over a grey backdrop into a PNG, with no
    // window or GPU. Needs the achievements loaded (kernel::initAchievements). `detail`
    // (--overlay-preview-detail): with the achievement with the longest text open in full.
    bool writePreview(const char* path, uint32_t width, uint32_t height, bool detail = false);
}
