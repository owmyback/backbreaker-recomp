// The game's profiler markers were compiled to an empty function, sub_824972A8(category, name),
// but every call still passes a readable scope name ("NmgCommsVoice::Initialise", ...).
// Overriding the weak symbol lets crash reports show which part of the game was running.
// In the title update's executable the linker folded every empty function into one (at 0x82656E18
// there), so the same hook also sees the game's compiled-out debug prints (category, format): their
// format strings show up among the scope names.
#include "kernel.h"
#include "../memory.h"

#include <cstdio>

PPC_FUNC(sub_824972A8)
{
    if (auto* t = kernel::currentThread())
        t->scopes[t->scopePos++ % 16] = ctx.r4.u32;
}

namespace kernel
{
    void printScopes(const GuestThread* t)
    {
        if (!t) return;
        std::fprintf(stderr, "---- recent profiler scopes on thread %u (oldest first) ----\n", t->id);
        for (uint32_t k = 0; k < 16; k++)
        {
            uint32_t a = t->scopes[(t->scopePos + k) % 16];
            if (a < 0x82000000 || a >= 0x83000000)
                continue;
            // Some markers pass non-string data; only print readable names
            const char* s = (const char*)mem::host(a);
            size_t n = 0;
            while (n < 120 && s[n] >= 0x20 && s[n] < 0x7F) n++;
            if (n >= 3 && s[n] == 0)
                std::fprintf(stderr, "  %s\n", s);
        }
    }
}
