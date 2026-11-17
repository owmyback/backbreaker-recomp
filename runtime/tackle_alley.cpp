// Tackle Alley at 60 fps: after a tackle the next attempt never started; the player stood still,
// the clock frozen (#139). At 30 fps it never happened.
//
// After a tackle the game switches to match stage 43 (Tackle Alley's between-attempts stage), whose
// start (Gamemanager::StartMatchStage, sub_822748A0) begins a 0.25 s fade to black (sub_8226AE80:
// flag 0x82B5B4E4, type 1). Tackle Alley sets up the next attempt (sub_823C7858, from the game
// manager's update sub_82277110) once that fade has finished - while the stage is still 43. In the
// meantime the play-flow state machine (sub_8215B710, state at +22128) steps on, mostly one state
// per game tick: 2, 3, 4, 5, 7, 8 (until both teams are ready), 66, 67, 64, and in state 64 it
// requests stage 4, the next attempt's start, which begins a fade back in. At 30 ticks a second that
// chain takes about 0.47 s and the fade has long finished; at 60 (#89) it takes about 0.23 s, stage
// 4 starts before the fade is done, its fade in cuts the fade to black off, and the stage-43 check
// that sets up the next attempt never sees it finish. Tackle Alley then waits for good.
//
// The fix keeps the order the console's timing gave: while the play flow is in state 64, the
// current stage is 43 and the fade to black is still running, the play flow's update waits that
// tick. The fade finishes, the next attempt is set up, and state 64 then requests stage 4 as at 30
// fps. At 30 fps the fade is always done first, so this never applies. A fade that somehow never
// ended would let it go after a second.

#include <ppc_config.h>
#include <ppc_context.h>

#include <chrono>

extern "C" PPC_FUNC(__imp__sub_8215B710); // the play-flow state machine's update (this = r3)

namespace
{
    constexpr uint32_t kGameObject = 0x82B5B7D8;  // pointer to the game manager
    constexpr uint32_t kFadeActive = 0x82B5B4E4;  // u8: a fade is running (sub_8226AE10 reads it)
    constexpr uint32_t kFadeType = 0x82B5B4E0;    // u32: 1 = to black, 2 = back in
    constexpr uint32_t kFlowState = 22128;        // play flow: current state
    constexpr uint32_t kStateRequestNext = 64;    // the state that requests stage 4
    constexpr uint32_t kStageBetweenAttempts = 43;

    uint32_t load32(uint8_t* base, uint32_t a) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + a)); }

    // The game manager's current match stage: the slot at +212 for the index at +188
    // (sub_82278F08 starts the stage requested in +196 when the two differ)
    uint32_t currentStage(uint8_t* base)
    {
        uint32_t gm = load32(base, kGameObject);
        if (!gm)
            return 0;
        return load32(base, gm + 212 + 4 * load32(base, gm + 188));
    }
}

PPC_FUNC(sub_8215B710)
{
    using clock = std::chrono::steady_clock;
    static clock::time_point heldSince{};
    static bool holding = false;

    const uint32_t self = ctx.r3.u32;
    if (load32(base, self + kFlowState) == kStateRequestNext && currentStage(base) == kStageBetweenAttempts &&
        base[kFadeActive] != 0 && load32(base, kFadeType) == 1)
    {
        const auto now = clock::now();
        if (!holding)
        {
            holding = true;
            heldSince = now;
        }
        if (now - heldSince < std::chrono::seconds(1))
            return; // this tick: wait for the fade to black, as at 30 fps
    }
    holding = false;
    __imp__sub_8215B710(ctx, base);
}
