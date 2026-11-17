// GPU frame capture. Field layouts and enum names follow Xenia's xenos.h / registers.h
// (BSD 3-clause, see docs/licenses/xenia-LICENSE.txt).
#include "capture.h"
#include "gpu.h"
#include "state.h"
#include "../build_id.h"
#include "../hid/hid.h"
#include "../log.h"
#include "../memory.h"
#include "../paths.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace gpu
{
    struct RegisterName { uint32_t index; const char* name; };
    extern const RegisterName kRegisterNames[];
    extern const uint32_t kRegisterNameCount;
}

namespace gpu::capture
{
    namespace
    {
        namespace fs = std::filesystem;

        // Register ranges
        constexpr uint32_t kAluConstants = 0x4000, kAluConstantsEnd = 0x4800;
        constexpr uint32_t kFetchConstants = 0x4800, kFetchConstantsEnd = 0x48C0; // 32 x 6 dwords
        constexpr uint32_t RB_SURFACE_INFO = 0x2000, RB_COLOR_INFO = 0x2001, RB_DEPTH_INFO = 0x2002;
        constexpr uint32_t RB_COLOR1_INFO = 0x2003, RB_COLOR2_INFO = 0x2004, RB_COLOR3_INFO = 0x2005;
        constexpr uint32_t RB_MODECONTROL = 0x2208;

        const char* kPrimNames[0x17] = {
            "none", "point_list", "line_list", "line_strip", "triangle_list", "triangle_fan", "triangle_strip",
            "triangle_with_w_flags", "rectangle_list", nullptr, nullptr, nullptr, "line_loop", "quad_list",
            "quad_strip", "polygon", "2d_copy_rect_list_v0", "2d_copy_rect_list_v1", "2d_copy_rect_list_v2",
            "2d_copy_rect_list_v3", "2d_fill_rect_list", "2d_line_strip", "2d_tri_strip" };
        const char* kSourceNames[4] = { "dma", "immediate", "auto_index", "?" };
        const char* kEdramModes[8] = { "no_op", "?1", "?2", "?3", "color_depth", "depth_only", "copy", "?7" };
        const char* kColorFormats[16] = { "8_8_8_8", "8_8_8_8_GAMMA", "2_10_10_10", "2_10_10_10_FLOAT", "16_16",
            "16_16_16_16", "16_16_FLOAT", "16_16_16_16_FLOAT", "?8", "?9", "2_10_10_10_AS_10_10_10_10", "?11",
            "2_10_10_10_FLOAT_AS_16_16_16_16", "?13", "32_FLOAT", "32_32_FLOAT" };
        const char* kTextureFormats[64] = { "1_REVERSE", "1", "8", "1_5_5_5", "5_6_5", "6_5_5", "8_8_8_8",
            "2_10_10_10", "8_A", "8_B", "8_8", "Cr_Y1_Cb_Y0_REP", "Y1_Cr_Y0_Cb_REP", "16_16_EDRAM", "8_8_8_8_A",
            "4_4_4_4", "10_11_11", "11_11_10", "DXT1", "DXT2_3", "DXT4_5", "16_16_16_16_EDRAM", "24_8",
            "24_8_FLOAT", "16", "16_16", "16_16_16_16", "16_EXPAND", "16_16_EXPAND", "16_16_16_16_EXPAND",
            "16_FLOAT", "16_16_FLOAT", "16_16_16_16_FLOAT", "32", "32_32", "32_32_32_32", "32_FLOAT",
            "32_32_FLOAT", "32_32_32_32_FLOAT", "32_AS_8", "32_AS_8_8", "16_MPEG", "16_16_MPEG", "8_INTERLACED",
            "32_AS_8_INTERLACED", "32_AS_8_8_INTERLACED", "16_INTERLACED", "16_MPEG_INTERLACED",
            "16_16_MPEG_INTERLACED", "DXN", "8_8_8_8_AS_16_16_16_16", "DXT1_AS_16_16_16_16",
            "DXT2_3_AS_16_16_16_16", "DXT4_5_AS_16_16_16_16", "2_10_10_10_AS_16_16_16_16",
            "10_11_11_AS_16_16_16_16", "11_11_10_AS_16_16_16_16", "32_32_32_FLOAT", "DXT3A", "DXT5A", "CTX1",
            "DXT3A_AS_1_1_1_1", "8_8_8_8_GAMMA_EDRAM", "2_10_10_10_FLOAT_EDRAM" };
        const char* kDimensions[4] = { "1D", "2D", "3D", "cube" };

        struct Shader { uint32_t type; std::vector<uint32_t> words; };

        std::mutex g_lock;
        std::map<uint64_t, Shader> g_shaders; // hash -> microcode (all shaders seen, big-endian words)
        uint64_t g_currentShader[2] = { 0, 0 }; // vertex, pixel
        std::set<uint64_t> g_targets_pending; // frames still to capture
        std::multiset<double> g_timesPending;  // --capture-at seconds still to come
        std::string g_dumpDir;                // --dump-shaders target, empty = off
        std::string g_dir; // set by request()/requestAt(); paths::captures() unless given
        bool g_active = false;

        // Per-capture state
        uint64_t g_frame = 0;
        std::vector<uint32_t> g_shadow;          // register values at the previous draw
        std::map<uint32_t, uint64_t> g_opcodes;  // type-3 opcode histogram
        std::map<std::string, uint64_t> g_prims;
        std::set<uint64_t> g_usedShaders[2];
        std::set<std::string> g_targets, g_textures;
        std::map<std::string, uint64_t> g_pipelines; // pipeline key -> draws
        uint64_t g_draws = 0, g_copies = 0;
        FILE* g_drawLog = nullptr;

        // Registers that change on every packet batch and carry no rendering state
        bool isNoise(uint32_t i) { return i == 0x01C4 || i == 0x01C5; } // CP_RB_RPTR, CP_RB_WPTR

        const char* regName(uint32_t i)
        {
            auto* end = kRegisterNames + kRegisterNameCount;
            auto* it = std::lower_bound(kRegisterNames, end, i, [](const RegisterName& r, uint32_t v) { return r.index < v; });
            return (it != end && it->index == i) ? it->name : nullptr;
        }


        std::string describeTarget()
        {
            uint32_t surface = readRegister(RB_SURFACE_INFO), depth = readRegister(RB_DEPTH_INFO);
            uint32_t mode = readRegister(RB_MODECONTROL) & 7;
            char buf[256];
            int n = snprintf(buf, sizeof(buf), "mode=%s pitch=%u msaa=%ux", kEdramModes[mode], surface & 0x3FFF,
                             1u << ((surface >> 16) & 3));
            static const uint32_t colorRegs[4] = { RB_COLOR_INFO, RB_COLOR1_INFO, RB_COLOR2_INFO, RB_COLOR3_INFO };
            for (int i = 0; i < 4; i++)
            {
                uint32_t c = readRegister(colorRegs[i]);
                if (i > 0 && c == 0) continue;
                n += snprintf(buf + n, sizeof(buf) - n, " rt%d=%s@tile%u", i, kColorFormats[(c >> 16) & 0xF], c & 0xFFF);
            }
            snprintf(buf + n, sizeof(buf) - n, " depth=%s@tile%u", (depth >> 16) & 1 ? "24_8_FLOAT" : "24_8", depth & 0xFFF);
            return buf;
        }

        // Texture fetch constants (type 2) at the time of a draw, per Xenia's xe_gpu_texture_fetch_t
        void describeTextures(FILE* f)
        {
            for (uint32_t slot = 0; slot < 32; slot++)
            {
                uint32_t d[6];
                for (int i = 0; i < 6; i++) d[i] = readRegister(kFetchConstants + slot * 6 + i);
                if ((d[0] & 3) != 2) continue; // not a texture fetch constant
                uint32_t format = d[1] & 0x3F, dim = (d[5] >> 9) & 3;
                uint32_t width, height = 1, depth = 1;
                if (dim == 0) width = (d[2] & 0xFFFFFF) + 1;
                else if (dim == 2) { width = (d[2] & 0x7FF) + 1; height = ((d[2] >> 11) & 0x7FF) + 1; depth = ((d[2] >> 22) & 0x3FF) + 1; }
                else
                {
                    // 2D/stacked and cube use the 2D size layout (cube: 6 faces)
                    width = (d[2] & 0x1FFF) + 1;
                    height = ((d[2] >> 13) & 0x1FFF) + 1;
                    depth = dim == 3 ? 6 : ((d[2] >> 26) & 0x3F) + 1;
                }
                bool tiled = d[0] >> 31;
                uint32_t pitch = ((d[0] >> 22) & 0x1FF) * 32, address = d[1] & 0xFFFFF000, mips = d[5] & 0xFFFFF000;
                char key[160];
                snprintf(key, sizeof(key), "%s %s %ux%u%s%s", kTextureFormats[format], kDimensions[dim], width, height,
                         depth > 1 ? (" x" + std::to_string(depth)).c_str() : "", tiled ? " tiled" : " linear");
                g_textures.insert(key);
                if (f)
                {
                    // What guest memory holds there: textures the game builds on the GPU and reads
                    // back (or never fills) show as all zeros
                    uint32_t scan = std::min<uint32_t>(256u << 10, 0x20000000u - std::min(address, 0x20000000u)), nonzero = 0;
                    uint64_t hash = 1469598103934665603ull;
                    const uint8_t* p = (const uint8_t*)mem::physical(address);
                    for (uint32_t i = 0; i < scan; i++) { nonzero += p[i] != 0; hash = (hash ^ p[i]) * 1099511628211ull; }
                    char head[100] = "";
                    for (int i = 0; i < 32; i++) snprintf(head + 2 * i + i / 4, sizeof(head) - (2 * i + i / 4), i % 4 == 3 ? "%02X " : "%02X", p[i]);
                    fprintf(f, "    tex[%2u] %s pitch=%u base=%08X mips=%08X  raw %08X %08X %08X %08X %08X %08X  data: %u/%u nonzero, fnv %016llX, first bytes %s\n",
                            slot, key, pitch, address, mips, d[0], d[1], d[2], d[3], d[4], d[5], nonzero, scan, (unsigned long long)hash, head);
                }
            }
        }

        void begin(uint64_t frame)
        {
            g_frame = frame;
            fs::path dir = fs::path(g_dir) / ("frame_" + std::to_string(frame));
            fs::create_directories(dir / "shaders");
            g_drawLog = fopen((dir / "draws.txt").c_str(), "w");
            g_shadow.assign(reg::kCount, 0);
            FILE* f = fopen((dir / "registers_start.txt").c_str(), "w");
            uint32_t aluNonZero = 0;
            for (uint32_t i = 0; i < reg::kCount; i++)
            {
                g_shadow[i] = readRegister(i);
                if (i >= kAluConstants && i < kAluConstantsEnd) { aluNonZero += g_shadow[i] != 0; continue; }
                if (g_shadow[i] && f)
                    fprintf(f, "%04X %-40s %08X\n", i, regName(i) ? regName(i) : "?", g_shadow[i]);
            }
            if (f)
            {
                fprintf(f, "(ALU constants 0x4000-0x47FF: %u non-zero, not listed)\n", aluNonZero);
                fclose(f);
            }
            g_opcodes.clear(); g_prims.clear(); g_targets.clear(); g_textures.clear(); g_pipelines.clear();
            g_usedShaders[0].clear(); g_usedShaders[1].clear();
            g_draws = g_copies = 0;
            g_active = true;
            info("[capture] recording frame %llu into %s", (unsigned long long)frame, dir.c_str());
        }

        void finish(uint32_t front, uint32_t width, uint32_t height)
        {
            g_active = false;
            if (g_drawLog) { fclose(g_drawLog); g_drawLog = nullptr; }
            fs::path dir = fs::path(g_dir) / ("frame_" + std::to_string(g_frame));
            for (int t = 0; t < 2; t++)
            {
                for (uint64_t h : g_usedShaders[t])
                {
                    auto it = g_shaders.find(h);
                    if (it == g_shaders.end()) continue;
                    char name[64];
                    snprintf(name, sizeof(name), "%s_%016llx.bin", t ? "ps" : "vs", (unsigned long long)h);
                    if (FILE* s = fopen((dir / "shaders" / name).c_str(), "wb"))
                    {
                        fwrite(it->second.words.data(), 4, it->second.words.size(), s); // big-endian dwords
                        fclose(s);
                    }
                }
            }
            FILE* f = fopen((dir / "summary.txt").c_str(), "w");
            if (!f) return;
            fprintf(f, "Backbreaker GPU frame capture - frame %llu\n", (unsigned long long)g_frame);
            // Which binary made this capture: a capture folder gets emailed around on its own (#75).
            fprintf(f, "built from %s\n", build_id::line());
            fprintf(f, "front buffer %08X %ux%u\n\n", front, width, height);
            fprintf(f, "draws: %llu (of which EDRAM copies/resolves: %llu)\n", (unsigned long long)g_draws, (unsigned long long)g_copies);
            fprintf(f, "\nprimitive types:\n");
            for (auto& [k, v] : g_prims) fprintf(f, "  %-24s %llu\n", k.c_str(), (unsigned long long)v);
            fprintf(f, "\nshaders used: %zu vertex, %zu pixel (microcode in shaders/, big-endian dwords)\n",
                    g_usedShaders[0].size(), g_usedShaders[1].size());
            fprintf(f, "\nrender target setups seen at draws:\n");
            for (auto& t : g_targets) fprintf(f, "  %s\n", t.c_str());
            fprintf(f, "\ntextures seen at draws (format, dimension, size, tiling):\n");
            for (auto& t : g_textures) fprintf(f, "  %s\n", t.c_str());
            fprintf(f, "\npipeline states (fixed-function state per draw, see runtime/gpu/state.cpp): %zu unique\n", g_pipelines.size());
            for (auto& [k, v] : g_pipelines) fprintf(f, "  %4llu  %s\n", (unsigned long long)v, k.c_str());
            fprintf(f, "\nPM4 type-3 packets:\n");
            for (auto& [op, n] : g_opcodes) fprintf(f, "  0x%02X  %llu\n", op, (unsigned long long)n);
            fclose(f);
            info("[capture] frame %llu: %llu draws, %zu+%zu shaders, %zu textures -> %s", (unsigned long long)g_frame,
                 (unsigned long long)g_draws, g_usedShaders[0].size(), g_usedShaders[1].size(), g_textures.size(), dir.c_str());
        }
    }

    void dumpAllShaders(const std::string& dir)
    {
        std::lock_guard guard(g_lock);
        fs::create_directories(dir);
        g_dumpDir = dir;
        info("[capture] dumping every shader to %s", dir.c_str());
    }

    void request(uint64_t frame, const std::string& dir)
    {
        std::lock_guard guard(g_lock);
        g_targets_pending.insert(frame);
        g_dir = dir.empty() ? paths::captures() : dir;
    }

    void requestAt(double seconds, const std::string& dir)
    {
        std::lock_guard guard(g_lock);
        g_timesPending.insert(seconds);
        g_dir = dir.empty() ? paths::captures() : dir;
    }

    void onPacket(uint32_t opcode)
    {
        if (!g_active) return;
        g_opcodes[opcode]++;
    }

    void onShaderLoad(uint32_t type, uint32_t start, uint32_t dwords, const uint32_t* words, uint64_t h)
    {
        std::lock_guard guard(g_lock);
        if (!g_shaders.count(h))
        {
            g_shaders[h] = { type, std::vector<uint32_t>(words, words + dwords) };
            if (!g_dumpDir.empty())
            {
                char name[64];
                snprintf(name, sizeof(name), "%s_%016llx.bin", type ? "ps" : "vs", (unsigned long long)h);
                if (FILE* s = fopen((fs::path(g_dumpDir) / name).c_str(), "wb"))
                {
                    fwrite(words, 4, dwords, s); // big-endian dwords, as in guest memory
                    fclose(s);
                }
            }
        }
        if (type < 2) g_currentShader[type] = h;
        if (g_active && g_drawLog)
            fprintf(g_drawLog, "load %s shader %016llx (%u dwords at %u)\n", type ? "pixel" : "vertex",
                    (unsigned long long)h, dwords, start);
    }

    void onDraw(uint32_t opcode, uint32_t initiator, uint32_t indexBase, uint32_t indexSizeWord)
    {
        if (!g_active) return;
        std::lock_guard guard(g_lock);
        uint32_t prim = initiator & 0x3F, source = (initiator >> 6) & 3, indexSize = (initiator >> 11) & 1;
        uint32_t count = initiator >> 16;
        uint32_t mode = readRegister(RB_MODECONTROL) & 7;
        const char* primName = prim < 0x17 && kPrimNames[prim] ? kPrimNames[prim] : "?";
        g_draws++;
        if (mode == 6) g_copies++;
        g_prims[primName]++;
        g_usedShaders[0].insert(g_currentShader[0]);
        g_usedShaders[1].insert(g_currentShader[1]);
        std::string target = describeTarget();
        g_targets.insert(target);
        DrawState state = captureDrawState(initiator);
        g_pipelines[pipelineKey(state)]++;
        FILE* f = g_drawLog;
        if (!f) return;
        fprintf(f, "\ndraw %llu  %s  prim=%s count=%u source=%s", (unsigned long long)g_draws,
                opcode == 0x22 ? "DRAW_INDX" : "DRAW_INDX_2", primName, count, kSourceNames[source]);
        if (source == 0)
            fprintf(f, " indices=%s base=%08X size_word=%08X", indexSize ? "u32" : "u16", indexBase, indexSizeWord);
        fprintf(f, "\n  vs=%016llx ps=%016llx\n%s  raw register changes since the previous draw:\n", (unsigned long long)g_currentShader[0],
                (unsigned long long)g_currentShader[1], describe(state).c_str());
        // Registers changed since the previous draw (ALU constants only counted)
        uint32_t aluChanged = 0;
        std::set<uint32_t> changedConsts; // float constant vec4s with a changed dword (0-255 vertex, 256-511 pixel)
        for (uint32_t i = 0; i < reg::kCount; i++)
        {
            uint32_t v = readRegister(i);
            if (v == g_shadow[i] || isNoise(i)) continue;
            if (i >= kAluConstants && i < kAluConstantsEnd) aluChanged++, changedConsts.insert((i - kAluConstants) / 4);
            else if (!(i >= kFetchConstants && i < kFetchConstantsEnd))
                fprintf(f, "  %04X %-36s %08X (was %08X)\n", i, regName(i) ? regName(i) : "?", v, g_shadow[i]);
            g_shadow[i] = v;
        }
        if (aluChanged)
        {
            fprintf(f, "  (%u ALU constant dwords changed)\n", aluChanged);
            for (uint32_t c : changedConsts)
            {
                float v[4];
                for (int k = 0; k < 4; k++) { uint32_t u = readRegister(kAluConstants + c * 4 + k); std::memcpy(&v[k], &u, 4); }
                fprintf(f, "    %s c%u = (%g, %g, %g, %g)\n", c < 256 ? "vs" : "ps", c < 256 ? c : c - 256, v[0], v[1], v[2], v[3]);
            }
        }
        describeTextures(f);
    }

    void onSwap(uint64_t frameNumber, uint32_t front, uint32_t width, uint32_t height)
    {
        std::lock_guard guard(g_lock);
        if (g_active)
            finish(front, width, height);
        // frameNumber swaps have happened; the next frame ends with swap frameNumber + 1
        bool timed = false;
        double t = 0;
        if (!g_timesPending.empty() && !g_targets_pending.count(frameNumber + 1) && (t = hid::secondsSinceLaunch()) >= *g_timesPending.begin())
        {
            info("[capture] --capture-at %g: frame %llu (%.2f s)", *g_timesPending.begin(), (unsigned long long)frameNumber + 1, t);
            g_timesPending.erase(g_timesPending.begin());
            timed = true;
        }
        if (g_targets_pending.erase(frameNumber + 1) || timed)
            begin(frameNumber + 1);
    }
}
