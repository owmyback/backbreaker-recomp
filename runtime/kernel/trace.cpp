// Import call tracing: a ring of recent calls for crash reports, plus per-import counts.
#include "import_support.h"
#include "kernel.h"
#include "../log.h"

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace
{
    constexpr size_t kRing = 32;
    struct Entry { const char* name; uint32_t lr, r3, r4, r5; bool implemented; };
    Entry g_ring[kRing];
    std::atomic<size_t> g_ringPos{0};

    std::mutex g_countLock;
    std::map<std::string, std::pair<uint64_t, bool>> g_counts; // name -> (calls, implemented)

    int g_logLimit = 3; // per unimplemented import, first N calls are printed

    void record(const char* name, PPCContext& ctx, bool implemented)
    {
        size_t i = g_ringPos.fetch_add(1) % kRing;
        g_ring[i] = { name, uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, implemented };
        std::lock_guard guard(g_countLock);
        auto& c = g_counts[name];
        c.first++;
        c.second = implemented;
        if (!implemented && c.first <= uint64_t(g_logLimit))
            info("unimplemented %s(r3=%08X r4=%08X r5=%08X) from %08X%s", name, ctx.r3.u32, ctx.r4.u32,
                 ctx.r5.u32, uint32_t(ctx.lr), c.first == uint64_t(g_logLimit) ? "  (further calls not logged)" : "");
    }
}

void UnimplementedImport(const char* name, PPCContext& ctx)
{
    record(name, ctx, false);
    ctx.r3.u64 = 0;
}

void TraceImport(const char* name, PPCContext& ctx)
{
    record(name, ctx, true);
}

namespace kernel
{
    void printTrace()
    {
        size_t end = g_ringPos.load();
        size_t n = end < kRing ? end : kRing;
        std::fprintf(stderr, "---- last %zu import calls (oldest first) ----\n", n);
        for (size_t k = end - n; k < end; k++)
        {
            const Entry& e = g_ring[k % kRing];
            std::fprintf(stderr, "  %c %-40s r3=%08X r4=%08X r5=%08X lr=%08X\n", e.implemented ? ' ' : '!',
                         e.name, e.r3, e.r4, e.r5, e.lr);
        }
    }

    void printSummary()
    {
        std::lock_guard guard(g_countLock);
        size_t impl = 0, unimpl = 0;
        std::fprintf(stderr, "---- import usage (! = unimplemented) ----\n");
        for (auto& [name, c] : g_counts)
        {
            std::fprintf(stderr, "  %c %-40s %llu\n", c.second ? ' ' : '!', name.c_str(), (unsigned long long)c.first);
            (c.second ? impl : unimpl)++;
        }
        std::fprintf(stderr, "  distinct imports called: %zu implemented, %zu unimplemented\n", impl, unimpl);
    }
}
