// Renderer debug aids (issue #40): see debug_aids.h. They were written to find the black grass
// (issue #45) and are kept for the next rendering bug of that kind.
#include "debug_aids.h"
#include "../debug/png.h"
#include "../log.h"
#include "../memory.h"
#include "../paths.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <vector>

namespace gpu::debugaids
{
    namespace
    {
        constexpr uint32_t kWatchBytes = 256u << 10;
        constexpr uint32_t kPhysicalSize = 0x20000000u;

        struct Config
        {
            std::vector<std::string> paint; // BB_DEBUG_PS: hash prefixes (lowercase hex), in order
            std::string expr;               // BB_DEBUG_EXPR
            uint32_t watch = 0;             // BB_DEBUG_WATCH (physical address)
            uint32_t watchFrames = 30;      // how far back the resolves listed at a change go
            bool watching = false;
            uint32_t dump = 0;              // BB_DEBUG_DUMP (physical address), 0 = off
        };

        // A guest address in hex, virtual (A0000000/C0000000/E0000000 views) or physical, and an
        // optional ",<number>" after it (BB_DEBUG_WATCH's frame count)
        bool parseAddress(const char* name, uint32_t& address, uint32_t* number = nullptr)
        {
            const char* v = getenv(name);
            if (!v || !*v) return false;
            char* end = nullptr;
            unsigned long a = strtoul(v, &end, 16);
            if (number && end != v && *end == ',')
            {
                char* end2 = nullptr;
                unsigned long n = strtoul(end + 1, &end2, 10);
                if (end2 != end + 1 && n) *number = uint32_t(std::min(n, 100000ul));
                end = end2;
            }
            if (end == v || *end)
            {
                warn("[debug] %s=%s is not a hex address%s - ignored", name, v, number ? " (with an optional ,<frames>)" : "");
                return false;
            }
            address = uint32_t(a) & 0x1FFFFFFFu;
            return true;
        }

        const Config& config()
        {
            static const Config c = [] {
                Config c;
                if (const char* list = getenv("BB_DEBUG_PS"))
                    for (const char* p = list; *p;)
                    {
                        const char* end = std::strchr(p, ',');
                        if (!end) end = p + std::strlen(p);
                        std::string item(p, end);
                        for (char& ch : item) ch = char(std::tolower((unsigned char)ch));
                        if (item.empty() || item.size() > 16 || item.find_first_not_of("0123456789abcdef") != std::string::npos)
                            warn("[debug] BB_DEBUG_PS: \"%s\" is not a shader hash (1-16 hex digits) - skipped", item.c_str());
                        else
                            c.paint.push_back(item);
                        p = *end ? end + 1 : end;
                    }
                if (const char* e = getenv("BB_DEBUG_EXPR")) c.expr = e;
                if (!c.paint.empty())
                    info("[debug] BB_DEBUG_PS: %zu pixel shader(s) painted %s", c.paint.size(),
                         c.expr.empty() ? "in flat colors" : ("with " + c.expr).c_str());
                if (parseAddress("BB_DEBUG_WATCH", c.watch, &c.watchFrames))
                {
                    if (c.watch > kPhysicalSize - kWatchBytes) c.watch = kPhysicalSize - kWatchBytes;
                    c.watching = true;
                    info("[debug] BB_DEBUG_WATCH: watching %08X..%08X; a change lists the resolves of the last %u frames", c.watch,
                         c.watch + kWatchBytes, c.watchFrames);
                }
                if (parseAddress("BB_DEBUG_DUMP", c.dump))
                    info("[debug] BB_DEBUG_DUMP: the resolve image at %08X is saved on screenshot frames", c.dump);
                return c;
            }();
            return c;
        }

        const char* const kPaintColors[8] = { "1.0, 0.0, 1.0", "0.0, 1.0, 1.0", "1.0, 1.0, 0.0", "0.0, 1.0, 0.0",
                                              "1.0, 0.0, 0.0", "0.0, 0.0, 1.0", "1.0, 1.0, 1.0", "1.0, 0.5, 0.0" };

        // BB_DEBUG_WATCH state
        struct ResolveRecord
        {
            uint64_t frame;
            uint32_t source, width, height, destBase, destPitch, destHeight, bytes, command;
            int x, y;
        };
        std::deque<ResolveRecord> g_recent; // resolves of the last watchFrames frames, for the log at a change
        uint64_t g_lastSwap = 0;            // resolves belong to frame g_lastSwap + 1 (it ends with that swap)
        uint64_t g_lastHash = 0;
        bool g_hashed = false;

        void logResolve(const char* prefix, const ResolveRecord& r)
        {
            info("[debug] %s frame %llu: %s source %u, rect %d,%d %ux%u -> %08X pitch %u height %u (%u KB), command %u", prefix,
                 (unsigned long long)r.frame, r.source < 4 ? "color" : "depth", r.source, r.x, r.y, r.width, r.height, r.destBase,
                 r.destPitch, r.destHeight, r.bytes >> 10, r.command);
        }
    }

    void patchPixelShader(uint64_t hash, const std::string& name, std::string& glsl)
    {
        const Config& c = config();
        char hex[17];
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)hash);
        for (size_t i = 0; i < c.paint.size(); i++)
        {
            if (std::strncmp(hex, c.paint[i].c_str(), c.paint[i].size()) != 0) continue; // a prefix is enough
            static const char kLine[] = "  frag_col0 = o_col[0];";
            size_t at = glsl.find(kLine);
            std::string value = !c.expr.empty() ? c.expr : std::string("vec4(") + kPaintColors[i % 8] + ", 1.0)";
            if (at != std::string::npos) glsl.replace(at, sizeof kLine - 1, "  frag_col0 = " + value + ";");
            info("[debug] BB_DEBUG_PS: %s paints %s%s", name.c_str(), c.expr.empty() ? ("(" + std::string(kPaintColors[i % 8]) + ")").c_str() : "the expression",
                 at == std::string::npos ? " - it has no color 0, nothing changed" : "");
            return;
        }
    }

    void onResolve(uint32_t source, int x, int y, uint32_t width, uint32_t height, uint32_t destBase, uint32_t destPitch,
                   uint32_t destHeight, uint32_t bytesPerPixel, uint32_t command)
    {
        const Config& c = config();
        if (!c.watching) return;
        ResolveRecord r{ g_lastSwap + 1, source, width, height, destBase, destPitch, destHeight, destPitch * destHeight * bytesPerPixel,
                         command, x, y };
        if (destBase < c.watch + kWatchBytes && destBase + r.bytes > c.watch) logResolve("BB_DEBUG_WATCH: resolve into the watched range,", r);
        g_recent.push_back(r);
        if (g_recent.size() > 65536) g_recent.pop_front(); // bound the memory, whatever the frame count
    }

    void onSwap(uint64_t frameNumber)
    {
        const Config& c = config();
        g_lastSwap = frameNumber;
        if (!c.watching) return;
        const uint8_t* p = (const uint8_t*)mem::physical(c.watch);
        uint64_t hash = 1469598103934665603ull;
        uint32_t nonzero = 0;
        for (uint32_t i = 0; i < kWatchBytes; i++) { hash = (hash ^ p[i]) * 1099511628211ull; nonzero += p[i] != 0; }
        while (!g_recent.empty() && g_recent.front().frame + c.watchFrames <= frameNumber) g_recent.pop_front();
        if (g_hashed && hash == g_lastHash) return;
        info("[debug] BB_DEBUG_WATCH: %08X %s at frame %llu: %u nonzero bytes, first %02X%02X%02X%02X %02X%02X%02X%02X", c.watch,
             g_hashed ? "changed" : "first seen", (unsigned long long)frameNumber, nonzero, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        g_lastHash = hash;
        g_hashed = true;
        if (g_recent.empty()) return;
        // The resolves of the last frames, one line per destination (its latest resolve), largest
        // first: the one the CPU turned into what's now in the watched range is usually near the top
        std::map<uint32_t, std::pair<ResolveRecord, uint32_t>> byDest; // destination -> latest, count
        for (const ResolveRecord& r : g_recent)
        {
            auto& e = byDest[r.destBase];
            e.first = r;
            e.second++;
        }
        std::vector<std::pair<ResolveRecord, uint32_t>> list;
        for (auto& [dest, e] : byDest) list.push_back(e);
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.first.bytes != b.first.bytes ? a.first.bytes > b.first.bytes : a.first.frame > b.first.frame; });
        info("[debug]   %zu resolves into %zu destinations in the last %u frames, largest first:", g_recent.size(), list.size(), c.watchFrames);
        constexpr size_t kShown = 16;
        for (size_t i = 0; i < list.size() && i < kShown; i++)
        {
            char prefix[48];
            snprintf(prefix, sizeof prefix, "  %ux, latest in", list[i].second);
            logResolve(prefix, list[i].first);
        }
        if (list.size() > kShown) info("[debug]   ... and %zu smaller destinations", list.size() - kShown);
    }

    uint32_t dumpAddress() { return config().dump; }

    void writeDump(uint64_t frameNumber, const uint8_t* rgba, uint32_t width, uint32_t height, bool swapRB)
    {
        double sum[4] = {};
        for (size_t i = 0; i < size_t(width) * height * 4; i++) sum[i & 3] += rgba[i];
        double n = double(width) * height * 255.0;
        char file[64];
        snprintf(file, sizeof file, "/resolve_%08X_%llu.png", config().dump, (unsigned long long)frameNumber);
        const std::string name = paths::screenshots() + file;
        std::filesystem::create_directories(paths::screenshots());
        if (!debug::writePng(name, rgba, width, height))
        {
            warn("[debug] BB_DEBUG_DUMP: could not write %s", name.c_str());
            return;
        }
        info("[debug] BB_DEBUG_DUMP: %s (%ux%u), channel means R %.3f G %.3f B %.3f A %.3f%s", name.c_str(), width, height, sum[0] / n,
             sum[1] / n, sum[2] / n, sum[3] / n, swapRB ? " (image order: the guest's texture view swaps R and B)" : "");
    }
}
