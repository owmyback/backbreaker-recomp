// The two boot logo movies (505.wmv, nmlogo.wmv, about 21 s together) are played by the boot code
// sub_822813A0 through NmgMovie::Play (sub_82804660), one call each, while the game loads. They
// play in full since #132; before that the movie player stopped each one after a second or two.
// With skip_intro the two calls return at once, as a button press during them would end them.
// Headless runs skip them unless told otherwise, so scripted routes keep the timing they had
// (the title screen about 4 s after launch instead of about 24 s). Other movies are unaffected.
#include "intro_movies.h"
#include "log.h"

#include <ppc_config.h>
#include <ppc_context.h>

#include <atomic>

extern "C" PPC_FUNC(__imp__sub_82804660); // NmgMovie::Play(path, flags)

namespace
{
    std::atomic<bool> g_skip{ false };
    constexpr uint32_t k505Return = BB_EXE(0x82281440, 0x822834D4);    // sub_822813A0 after playing media\Movies\Xbox360\505.wmv
    constexpr uint32_t kNmLogoReturn = BB_EXE(0x82281450, 0x822834E4); // ... and after nmlogo.wmv
}

namespace intro_movies
{
    void setSkip(bool skip) { g_skip = skip; }
}

PPC_FUNC(sub_82804660)
{
    const uint32_t ret = uint32_t(ctx.lr);
    if (g_skip && (ret == k505Return || ret == kNmLogoReturn))
    {
        info("[movie] boot logo %s skipped (skip_intro)", ret == k505Return ? "505.wmv" : "nmlogo.wmv");
        return;
    }
    __imp__sub_82804660(ctx, base);
}
