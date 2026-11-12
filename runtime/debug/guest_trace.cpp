// Debug-only wrappers for specific guest functions: log arguments and return value.
// Recompiled functions are weak aliases of __imp__sub_XXXXXXXX, so a strong definition here
// replaces the symbol while still being able to call the original body.
#include <ppc_config.h>
#include <ppc_context.h>
#include <atomic>
#include <cstdio>

#define GUEST_TRACE(addr)                                                                            \
    extern "C" PPC_FUNC(__imp__sub_##addr);                                                          \
    PPC_FUNC(sub_##addr)                                                                             \
    {                                                                                                \
        uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32, a6 = ctx.r6.u32, lr = uint32_t(ctx.lr); \
        __imp__sub_##addr(ctx, base);                                                                \
        std::fprintf(stderr, "[trace] sub_" #addr "(%08X, %08X, %08X, %08X) from %08X -> %08X\n",   \
                     a3, a4, a5, a6, lr, ctx.r3.u32);                                                \
    }

// Sampled variant: log every Nth call
#define GUEST_TRACE_EVERY(addr, N)                                                                   \
    extern "C" PPC_FUNC(__imp__sub_##addr);                                                          \
    PPC_FUNC(sub_##addr)                                                                             \
    {                                                                                                \
        static std::atomic<uint64_t> calls{0};                                                      \
        uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32, a6 = ctx.r6.u32, lr = uint32_t(ctx.lr); \
        __imp__sub_##addr(ctx, base);                                                                \
        uint64_t n = calls++;                                                                        \
        if (n % (N) == 0)                                                                            \
            std::fprintf(stderr, "[trace] #%llu sub_" #addr "(%08X, %08X, %08X, %08X) from %08X -> %08X\n", \
                         (unsigned long long)n, a3, a4, a5, a6, lr, ctx.r3.u32);                    \
    }

// Add GUEST_TRACE(XXXXXXXX) / GUEST_TRACE_EVERY(XXXXXXXX, N) lines here while debugging, e.g.
//   GUEST_TRACE(82936368)   // XHV2 helper that failed on XamUserReadProfileSettings
