// The match's render size (see scene_size.h).
#include "scene_size.h"
#include "log.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <atomic>

extern "C" PPC_FUNC(__imp__sub_827A09B0);

namespace
{
    std::atomic<bool> g_native720p{ false };
}

namespace scene_size
{
    void setNative720p(bool on) { g_native720p = on; }
}

// The game's render views: sub_82381928 creates three through sub_827A09B0 (r3 view, r5 viewport
// count, r6 width, r7 height): the match (1152x640 when the display is at most 1280 wide, 1920x1080
// otherwise), its two-player split (the same width, half the height) and the display. Everything
// after takes its size from the view: the predicated tiling (four 320-wide tiles at 1280x720
// instead of two 576-wide ones), the post-processing buffers and the final stretch to the display.
PPC_FUNC(sub_827A09B0)
{
    uint32_t w = ctx.r6.u32, h = ctx.r7.u32;
    if (g_native720p && w == 1152 && (h == 640 || h == 320))
    {
        ctx.r6.u32 = 1280;
        ctx.r7.u32 = h == 640 ? 720 : 360;
        info("[gpu] render view %ux%u instead of the game's %ux%u (native_720p)", ctx.r6.u32, ctx.r7.u32, w, h);
    }
    __imp__sub_827A09B0(ctx, base);
}
