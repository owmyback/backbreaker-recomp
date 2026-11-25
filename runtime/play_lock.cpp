// A race in the game between its update and render threads over a play's player slots (Training
// Camp's passing drills crashed on a retry, reading guest address 0xFB00).
//
// A play (offense or defense formation) builds its list of 11 player slots on first use and keeps a
// "built" flag at +100. sub_82323D28 does that under the play lock (sub_8237E7E8 / sub_8237E7F8, a
// recursive critical section) and checks the flag again inside it; the render thread goes through
// it when it looks a play up to draw the on-field route display. sub_8236E8D0, which sets up a
// drill repetition on the update thread, has the same check-build-set sequence inlined - without
// the lock. On a retry the drill resets its plays (flag 0, no slots) and both threads can build the
// same play at once: 22 slots instead of 11. sub_823DBCD0 then walks them giving each slot one of
// the team's 11 players, and the twelfth reads past that table: a null player, a load from
// 0x0000FB00, and a crash. On the console the two threads presumably never meet there; here they
// did on most passing-drill retries.
//
// The fix gives that one unlocked build the lock the other path takes: the slot builder
// sub_8232FA50, when called from sub_8236E8D0 (return address 0x8236EA8C), runs inside the play
// lock and only if the flag is still clear, and sets the flag before the lock is released, so the
// render thread's locked check sees it. Every other caller is unchanged.
#include <ppc_config.h>
#include <ppc_context.h>

extern "C" PPC_FUNC(__imp__sub_8232FA50); // build a play's player slots (r3 play)
extern "C" PPC_FUNC(__imp__sub_8237E7E8); // play lock: enter (recursive)
extern "C" PPC_FUNC(__imp__sub_8237E7F8); // play lock: leave

namespace
{
    constexpr uint32_t kUnlockedBuildReturn = BB_EXE(0x8236EA8C, 0x82374C6C); // in sub_8236E8D0, after its call of sub_8232FA50
    constexpr uint32_t kPlayBuilt = 100;                   // u8: the play's slots exist
}

PPC_FUNC(sub_8232FA50)
{
    if (uint32_t(ctx.lr) != kUnlockedBuildReturn)
    {
        __imp__sub_8232FA50(ctx, base);
        return;
    }
    const uint32_t play = ctx.r3.u32;
    PPCContext lock = ctx; // the lock functions only need r13 (the thread) and the stack
    __imp__sub_8237E7E8(lock, base);
    if (base[play + kPlayBuilt] == 0)
    {
        __imp__sub_8232FA50(ctx, base);
        base[play + kPlayBuilt] = 1; // what the caller does right after, now before the lock is let go
    }
    PPCContext unlock = ctx;
    __imp__sub_8237E7F8(unlock, base);
}
