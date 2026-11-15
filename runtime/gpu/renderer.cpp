// Vulkan renderer: the first-picture path of roadmap phase 4. Simplifications (see the ROADMAP):
// - EDRAM: one Vulkan image per (base tile, pitch, samples, format). Tiles written through one
//   image and then used through another are copied over by a transfer shader, through the raw EDRAM
//   bits when the two formats differ (depth <-> color, 32 <-> 64 bits per sample).
// - Resolves: the destination is a Vulkan image keyed by its guest address, which textures at
//   that address sample directly; the result is also written to guest memory for the CPU (see
//   "resolve writeback"). The copy rectangle is the window scissor.
// - Guest data is snapshotted at draw time into a per-frame upload buffer: vertex data (the
//   vertex fetch constants are rewritten to point into it), constants, indices, textures.
// - Textures: 2D linear and tiled (runtime/gpu/tiling.h) 8, 8_8, 1_5_5_5, 5_6_5, 4_4_4_4,
//   8_8_8_8, 2_10_10_10, DXT, 24_8 (depth), 16_16, 16_16_16_16, the 16-bit float and EXPAND
//   formats, 32_FLOAT and 32_32_FLOAT (signedness is not applied yet), with their mip levels
//   (runtime/gpu/mip_layout.h). Each frame a texture is used, its guest bytes are hashed, and it is
//   re-uploaded only if they changed. Other textures get a magenta
//   placeholder. A texture at a resolve destination samples the resolved image when the formats
//   are compatible (render to texture), including depth resolves (R32_SFLOAT).
// Register meanings follow Xenia (BSD; docs/licenses/xenia-LICENSE.txt).
#include "renderer.h"
#include "clear_value.h"
#include "debug_aids.h"
#include "gpu.h"
#include "shader_translator.h"
#include "state.h"
#include "mip_layout.h"
#include "tiling.h"
#include "../debug/png.h"
#include "../hid/hid.h"
#include "../log.h"
#include "../memory.h"
#include "../video/presenter.h"
#include "../video/vk_context.h"

#include <shaderc/shaderc.h>

#include <algorithm>
#include <bit>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <map>
#include <unistd.h>
#include <unordered_set>
#include <set>
#include <string>
#include <unordered_map>
#include <tuple>
#include <vector>

namespace gpu::render
{
    namespace
    {
        constexpr uint32_t kAluConstants = 0x4000, kFetchConstants = 0x4800, kBoolConstants = 0x4900, kLoopConstants = 0x4908;
        constexpr uint32_t RB_COPY_CONTROL = 0x2318, RB_COPY_DEST_BASE = 0x2319, RB_COPY_DEST_PITCH = 0x231A;
        constexpr uint32_t RB_DEPTH_CLEAR = 0x231D, RB_COLOR_CLEAR = 0x231E, RB_COLOR_CLEAR_LO = 0x231F, RB_BLEND_RED = 0x2105;
        constexpr uint32_t kFramesInFlight = 2;
        constexpr VkDeviceSize kUploadSize = 96ull << 20; // per frame in flight
        constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT; // D24S8 is not available on AMD
        constexpr uint32_t kConstantsSize = 4096 * 2 + 928;           // vs, ps, then bool/loop/fetch

        bool g_disabled = false, g_failed = false;
        // Internal resolution scale: render targets and everything drawn into them are this many
        // times larger than the guest's (the game still sees 1280x720)
        uint32_t g_scale = 1;
        uint32_t g_forcedAniso = 0; // --anisotropy
        VkRect2D scaled(VkRect2D r)
        {
            return { { r.offset.x * int32_t(g_scale), r.offset.y * int32_t(g_scale) }, { r.extent.width * g_scale, r.extent.height * g_scale } };
        }
        vkctx::Context* C = nullptr;
        VkDevice D = VK_NULL_HANDLE;
        std::set<uint64_t> g_screenshots;
        std::multimap<double, std::string> g_timedScreenshots; // seconds since launch -> label
        // Guest physical memory imported as a writable storage buffer (memory export)
        VkBuffer g_guestMem = VK_NULL_HANDLE;
        VkDeviceMemory g_guestMemMemory = VK_NULL_HANDLE;
        bool g_memExportReady = false;
        // Fallback when the driver can't import the guest mapping: a host-visible buffer at the
        // same addresses; export stream ranges are copied in before and out after each draw
        uint8_t* g_exportMap = nullptr;

        void warnOnce(const std::string& what)
        {
            static std::set<std::string> seen;
            if (seen.insert(what).second) warn("[render] %s", what.c_str());
        }

        uint32_t gpuSwap(uint32_t v, uint32_t endian)
        {
            switch (endian & 3)
            {
            case 1: return ((v >> 8) & 0x00FF00FF) | ((v << 8) & 0xFF00FF00);
            case 2: return __builtin_bswap32(v);
            case 3: return (v >> 16) | (v << 16);
            default: return v;
            }
        }

        float regFloat(uint32_t index)
        {
            uint32_t v = readRegister(index);
            float f;
            std::memcpy(&f, &v, 4);
            return f;
        }

        // ---------------------------------------------------------------- shaders

        struct Shader
        {
            ShaderType type;
            std::vector<uint32_t> words; // host order
            bool built = false;
            std::string name;                        // ps_<hash>, as in the shader dump; variants add _m<mask>
            TranslatedShader t;
            VkShaderModule module = VK_NULL_HANDLE; // null if translation or compilation failed
            VkShaderModule colorVariant[16]{};     // [mask] same shader, writing only the color slots in that mask
        };
        std::unordered_map<uint64_t, Shader> g_shaders;
        uint64_t g_current[2] = { 0, 0 }; // vertex, pixel
        // First-use cost (issue #73): time spent translating shaders, compiling them with shaderc and
        // creating pipelines, logged once per second when there was any
        struct CompileStats { uint32_t translated = 0, compiled = 0, cached = 0, pipelines = 0; double translateMs = 0, compileMs = 0, pipelineMs = 0; };
        CompileStats g_compileStats;
        // BB_PERF=1: one line per second of where the renderer thread's time went (issue #73)
        struct PerfStats { uint64_t draws = 0, hashBytes = 0, uploads = 0, uploadBytes = 0, resolves = 0, writebacks = 0, flushes = 0;
                           uint64_t flushExport = 0, flushSync = 0, exportDraws = 0, pendingAdded = 0, gpuVertexCopies = 0, syncRetries = 0, chunks = 0,
                                    idleSubmits = 0, asyncFences = 0, asyncWaits = 0, copyInsAroundPending = 0;
                           double hashMs = 0, waitMs = 0, cpWaitMs = 0;
                           std::map<std::string, uint64_t> syncsBy;          // syncs that had pending writes, by trigger
                           struct Up { uint32_t format = 0, width = 0, height = 0; uint64_t count = 0, bytes = 0, fresh = 0; };
                           std::map<uint32_t, Up> uploadsBy; };              // texture uploads by guest base address
        PerfStats g_perf;
        double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

        shaderc_compiler_t g_compiler = nullptr;
        shaderc_compile_options_t g_options = nullptr;
        VkShaderModule g_rectGs = VK_NULL_HANDLE;

        // Same hash as the frame capture, so names match the shader dump (vs_/ps_<hash>.bin)

        // ---------------------------------------------------------------- shader cache (issue #73)
        //
        // Compiling GLSL to SPIR-V with shaderc is nearly all of a new shader's cost: the side select
        // compiles 61 shaders in one second (1.8 s, a freeze), the match setup 45 (0.3 s), every run.
        // Translating the microcode takes about 2 ms for all of them, and RADV caches its own
        // pipeline compiles. So compiled SPIR-V is kept on disk, keyed by a hash of everything
        // that goes into it (the GLSL text, the stage, the color mask, the target and a format
        // tag): $XDG_CACHE_HOME/backbreaker-recomp/spirv/<key>.spv (or ~/.cache/...). A changed
        // translation is a different key, so the cache never needs clearing; --no-shader-cache
        // turns it off.
        bool g_shaderCacheOff = false;
        std::string g_shaderCacheDir; // empty: not set up yet, or unusable
        uint32_t g_targetEnv = 0;

        const std::string& shaderCacheDir()
        {
            static bool tried = false;
            if (tried || g_shaderCacheOff) return g_shaderCacheDir;
            tried = true;
            std::string base;
            if (const char* x = getenv("XDG_CACHE_HOME"); x && *x) base = x;
            else if (const char* h = getenv("HOME"); h && *h) base = std::string(h) + "/.cache";
            if (base.empty()) return g_shaderCacheDir;
            std::error_code ec;
            std::string dir = base + "/backbreaker-recomp/spirv";
            std::filesystem::create_directories(dir, ec);
            if (ec) warn("[render] shader cache off: cannot create %s (%s)", dir.c_str(), ec.message().c_str());
            else
            {
                g_shaderCacheDir = dir;
                info("[render] shader cache: %s", dir.c_str());
            }
            return g_shaderCacheDir;
        }

        uint64_t shaderCacheKey(const std::string& glsl, shaderc_shader_kind kind, uint32_t colorMask)
        {
            uint64_t h = 1469598103934665603ull;
            auto mix = [&](const void* p, size_t n) {
                for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 1099511628211ull;
            };
            const char tag[] = "bb-spirv-1 opt=performance"; // bump when the compile options change
            uint32_t k = uint32_t(kind);
            mix(tag, sizeof tag);
            mix(&g_targetEnv, sizeof g_targetEnv);
            mix(&k, sizeof k);
            mix(&colorMask, sizeof colorMask);
            mix(glsl.data(), glsl.size());
            return h;
        }

        std::vector<uint32_t> loadCachedSpirv(uint64_t key)
        {
            const std::string& dir = shaderCacheDir();
            if (dir.empty()) return {};
            char name[32];
            snprintf(name, sizeof name, "/%016llx.spv", (unsigned long long)key);
            FILE* f = fopen((dir + name).c_str(), "rb");
            if (!f) return {};
            std::vector<uint32_t> words;
            if (fseek(f, 0, SEEK_END) == 0)
            {
                long size = ftell(f);
                if (size >= 20 && size % 4 == 0 && fseek(f, 0, SEEK_SET) == 0)
                {
                    words.resize(size_t(size) / 4);
                    if (fread(words.data(), 4, words.size(), f) != words.size() || words[0] != 0x07230203u) words.clear();
                }
            }
            fclose(f);
            return words;
        }

        void storeCachedSpirv(uint64_t key, const void* data, size_t size)
        {
            const std::string& dir = shaderCacheDir();
            if (dir.empty()) return;
            char name[32];
            snprintf(name, sizeof name, "/%016llx.spv", (unsigned long long)key);
            std::string path = dir + name, tmp = path + ".tmp" + std::to_string(getpid());
            FILE* f = fopen(tmp.c_str(), "wb");
            if (!f) return;
            bool ok = fwrite(data, 1, size, f) == size;
            ok = (fclose(f) == 0) && ok;
            if (ok) ok = rename(tmp.c_str(), path.c_str()) == 0; // atomic: a reader sees all or nothing
            if (!ok) remove(tmp.c_str());
        }

        VkShaderModule compile(const std::string& glsl, shaderc_shader_kind kind, const char* name, std::string& error, uint32_t colorMask = 15)
        {
            uint64_t key = g_shaderCacheOff ? 0 : shaderCacheKey(glsl, kind, colorMask);
            if (!g_shaderCacheOff)
            {
                std::vector<uint32_t> spirv = loadCachedSpirv(key);
                if (!spirv.empty())
                {
                    VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
                    ci.codeSize = spirv.size() * 4;
                    ci.pCode = spirv.data();
                    VkShaderModule m = VK_NULL_HANDLE;
                    VK_CHECK(vkCreateShaderModule(D, &ci, nullptr, &m));
                    g_compileStats.cached++;
                    return m;
                }
            }
            shaderc_compile_options_t options = shaderc_compile_options_clone(g_options);
            if (colorMask != 15) // see BB_COLOR_MASK in the translated shader
            {
                char value[8];
                snprintf(value, sizeof(value), "0x%X", colorMask);
                shaderc_compile_options_add_macro_definition(options, "BB_COLOR_MASK", 13, value, strlen(value));
            }
            double t0 = nowMs();
            shaderc_compilation_result_t r = shaderc_compile_into_spv(g_compiler, glsl.data(), glsl.size(), kind, name, "main", options);
            shaderc_compile_options_release(options);
            g_compileStats.compiled++;
            g_compileStats.compileMs += nowMs() - t0;
            VkShaderModule m = VK_NULL_HANDLE;
            if (shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success)
            {
                VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
                ci.codeSize = shaderc_result_get_length(r);
                ci.pCode = (const uint32_t*)shaderc_result_get_bytes(r);
                VK_CHECK(vkCreateShaderModule(D, &ci, nullptr, &m));
                if (!g_shaderCacheOff) storeCachedSpirv(key, ci.pCode, ci.codeSize);
            }
            else
                error = shaderc_result_get_error_message(r);
            shaderc_result_release(r);
            return m;
        }

        Shader* currentShader(uint32_t type)
        {
            auto it = g_shaders.find(g_current[type]);
            if (it == g_shaders.end()) return nullptr;
            Shader& s = it->second;
            if (!s.built)
            {
                s.built = true;
                char name[48];
                snprintf(name, sizeof(name), "%s_%016llx", type ? "ps" : "vs", (unsigned long long)it->first);
                s.name = name;
                double t0 = nowMs();
                s.t = translateToGlsl(s.words, s.type);
                g_compileStats.translated++;
                g_compileStats.translateMs += nowMs() - t0;
                if (s.t.ok && type) debugaids::patchPixelShader(it->first, name, s.t.glsl); // BB_DEBUG_PS
                if (!s.t.ok)
                    warn("[render] shader %s not translated: %s", name, s.t.error.c_str());
                else
                {
                    std::string error;
                    s.module = compile(s.t.glsl, type ? shaderc_fragment_shader : shaderc_vertex_shader, name, error);
                    if (!s.module) warn("[render] shader %s failed to compile:\n%s", name, error.c_str());
                }
            }
            return s.module ? &s : nullptr;
        }

        // Rectangle lists: 3 vertices per rectangle; the 4th is the third corner mirrored across
        // the diagonal (the longest edge), for all interpolators too.
        const char* kRectangleGs = R"(#version 450
layout(triangles) in;
layout(triangle_strip, max_vertices = 4) out;
layout(location = 0) in XeInterpolators { vec4 v[16]; } xe_in[];
layout(location = 0) out XeInterpolators { vec4 v[16]; } xe_out;
void emitIndex(int i) { gl_Position = gl_in[i].gl_Position; xe_out.v = xe_in[i].v; EmitVertex(); }
void main() {
  vec3 p0 = gl_in[0].gl_Position.xyz / gl_in[0].gl_Position.w;
  vec3 p1 = gl_in[1].gl_Position.xyz / gl_in[1].gl_Position.w;
  vec3 p2 = gl_in[2].gl_Position.xyz / gl_in[2].gl_Position.w;
  vec3 e = vec3(dot(p1 - p0, p1 - p0), dot(p2 - p0, p2 - p0), dot(p2 - p1, p2 - p1));
  ivec3 order; vec3 w;  // strip order of the 3 given vertices; v3 = w.x * v0 + w.y * v1 + w.z * v2
  if (e.z > e.x && e.z > e.y) { order = ivec3(0, 1, 2); w = vec3(-1.0, 1.0, 1.0); }      // 1-2 is the diagonal
  else if (e.y > e.x && e.y > e.z) { order = ivec3(1, 0, 2); w = vec3(1.0, -1.0, 1.0); } // 0-2
  else { order = ivec3(2, 0, 1); w = vec3(1.0, 1.0, -1.0); }                             // 0-1
  for (int k = 0; k < 3; k++) emitIndex(order[k]);
  gl_Position = w.x * gl_in[0].gl_Position + w.y * gl_in[1].gl_Position + w.z * gl_in[2].gl_Position;
  for (int i = 0; i < 16; i++) xe_out.v[i] = w.x * xe_in[0].v[i] + w.y * xe_in[1].v[i] + w.z * xe_in[2].v[i];
  EmitVertex();
  EndPrimitive();
}
)";

        // ---------------------------------------------------------------- images

        struct Image
        {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE; // identity view of the whole image
            VkImageView depthView = VK_NULL_HANDLE, stencilView = VK_NULL_HANDLE; // depth formats: per-aspect views for sampling
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            uint32_t width = 0, height = 0, samples = 1, levels = 1;
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
            // EDRAM render targets only: where the image lives in EDRAM
            bool edram = false, edramWide = false; // wide: 64 bits per sample
            bool swapRB = false;                    // resolve destinations: red/blue swapped in memory (RB_COPY_DEST_INFO)
            uint32_t resolvePitch = 0;              // resolve destinations: guest pitch (RB_COPY_DEST_PITCH)
            // Resolve destinations: where pixel (0, 0) lives in guest memory, and the pitch and height
            // RB_COPY_DEST_PITCH gave it (the bounds of the writeback to guest memory)
            uint32_t guestBase = 0, guestPitch = 0, guestHeight = 0;
            // Resolve destinations: a fingerprint of their guest memory as of the last resolve (and
            // writeback), to notice when the CPU has since put something else there (issue #63)
            uint64_t memSig = 0, memSigFrame = 0;
            bool memSigPending = false; // a writeback into it is still to land; fingerprint after the sync
            uint32_t edramBase = 0, edramPitch = 0;
            bool edramDepth = false;
            uint32_t edramFormat = 0; // xenos::ColorRenderTargetFormat, or xenos::DepthRenderTargetFormat for depth
        };

        Image createImage(VkImageType type, VkImageViewType viewType, VkFormat format, uint32_t w, uint32_t h, uint32_t depth,
                          uint32_t layers, uint32_t samples, VkImageUsageFlags usage, VkImageCreateFlags flags = 0, uint32_t levels = 1)
        {
            Image img;
            img.format = format;
            img.width = w;
            img.height = h;
            img.samples = samples;
            img.levels = levels;
            img.aspect = format == kDepthFormat ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            ici.flags = flags;
            ici.imageType = type;
            ici.format = format;
            ici.extent = { w, h, depth };
            ici.mipLevels = levels;
            ici.arrayLayers = layers;
            ici.samples = VkSampleCountFlagBits(samples);
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = usage;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VK_CHECK(vkCreateImage(D, &ici, nullptr, &img.image));
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(D, img.image, &req);
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = C->findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VK_CHECK(vkAllocateMemory(D, &mai, nullptr, &img.memory));
            VK_CHECK(vkBindImageMemory(D, img.image, img.memory, 0));
            VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vci.image = img.image;
            vci.viewType = viewType;
            vci.format = format;
            vci.subresourceRange = { img.aspect, 0, levels, 0, layers };
            VK_CHECK(vkCreateImageView(D, &vci, nullptr, &img.view));
            if (format == kDepthFormat)
            {
                vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                VK_CHECK(vkCreateImageView(D, &vci, nullptr, &img.depthView));
                vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
                VK_CHECK(vkCreateImageView(D, &vci, nullptr, &img.stencilView));
            }
            return img;
        }

        // Every hazard in this renderer comes with a layout change, so one full barrier per
        // transition is enough for now (correct, not fast).
        void transition(VkCommandBuffer cmd, Image& img, VkImageLayout to)
        {
            if (img.layout == to) return;
            VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.oldLayout = img.layout;
            b.newLayout = to;
            b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img.image;
            b.subresourceRange = { img.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            img.layout = to;
        }

        // ---------------------------------------------------------------- frames in flight

        // Guest physical ranges [start, end) written by GPU work (resolve writebacks, memory export)
        // that are not in guest memory yet. `serial` is the submit that carries the write (0: still
        // being recorded); retire() copies a range out once the GPU has finished that submit.
        struct PendingWrite { uint32_t start, end; uint64_t serial; bool exported; };
        std::vector<PendingWrite> g_pendingWrites;
        // A draw whose textures or indices come from a pending memory export cannot read them out of
        // guest memory yet; it sets g_syncRetry, and draw() syncs and records it again (a sync in the
        // middle of a draw would reset the upload buffer under it).
        bool g_syncRetry = false;
        void needsExportedData(uint64_t start, uint64_t end)
        {
            for (const PendingWrite& w : g_pendingWrites)
                if (w.exported && start < w.end && w.start < end) { g_syncRetry = true; return; }
        }

        // Every renderer submit signals g_timeline with the next serial (#73), so "the GPU has run
        // everything up to submit N" is one counter: a semaphore signal covers all earlier submits.
        VkSemaphore g_timeline = VK_NULL_HANDLE;
        uint64_t g_submitSerial = 0;

        void queueSubmit(VkCommandBuffer cb, VkFence fence)
        {
            VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cb;
            VkTimelineSemaphoreSubmitInfo ts{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
            uint64_t value = 0;
            if (g_timeline)
            {
                value = ++g_submitSerial;
                ts.signalSemaphoreValueCount = 1;
                ts.pSignalSemaphoreValues = &value;
                si.pNext = &ts;
                si.signalSemaphoreCount = 1;
                si.pSignalSemaphores = &g_timeline;
                for (PendingWrite& w : g_pendingWrites)
                    if (!w.serial) w.serial = value; // recorded into this command buffer or an earlier one
            }
            std::lock_guard guard(C->queueLock);
            VK_CHECK(vkQueueSubmit(C->queue, 1, &si, fence));
        }

        struct Frame
        {
            VkCommandPool pool = VK_NULL_HANDLE;
            VkCommandBuffer cmd = VK_NULL_HANDLE;     // the one being recorded (or recorded next)
            VkFence fence = VK_NULL_HANDLE;           // the frame's last submit, at the swap (or a flushAndWait)
            // Early submits (#73): the frame's command buffers so far, all from `pool`, and the fences of
            // the chunks submitted before the swap without waiting for them
            std::vector<VkCommandBuffer> cmdBuffers;
            uint32_t cmdBuffersUsed = 1;
            std::vector<VkFence> chunkFences;
            uint32_t chunksInFlight = 0;
            bool recording = false, submitted = false;
            VkBuffer upload = VK_NULL_HANDLE;
            VkDeviceMemory uploadMemory = VK_NULL_HANDLE;
            uint8_t* uploadMap = nullptr;
            VkDeviceSize used = 0;
            VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
            VkDescriptorSet buffers = VK_NULL_HANDLE; // set 0 over `upload`; constants by dynamic offset
        };
        Frame g_frames[kFramesInFlight];
        uint32_t g_frameIndex = 0;
        uint64_t g_frameSerial = 1; // once-per-frame texture uploads
        Frame& frame() { return g_frames[g_frameIndex]; }

        VkCommandBuffer cmd()
        {
            Frame& f = frame();
            if (!f.recording)
            {
                VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                VK_CHECK(vkBeginCommandBuffer(f.cmd, &bi));
                f.recording = true;
            }
            return f.cmd;
        }

        void endPass();
        uint32_t g_drawsSinceSubmit = 0;
        constexpr uint32_t kChunkDraws = 128; // draws recorded before an early submit is worth its cost

        // Hand what is recorded to the GPU now, without waiting for it (#73): the GPU runs while the CPU
        // records the rest of the frame, and a sync later waits only for what came after. Queue
        // submission order keeps every barrier recorded so far in force for the next command buffer.
        void submitChunk()
        {
            endPass();
            Frame& f = frame();
            if (!f.recording) return;
            VK_CHECK(vkEndCommandBuffer(f.cmd));
            f.recording = false;
            if (f.chunksInFlight == f.chunkFences.size())
            {
                VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
                VkFence fence;
                VK_CHECK(vkCreateFence(D, &fci, nullptr, &fence));
                f.chunkFences.push_back(fence);
            }
            queueSubmit(f.cmd, f.chunkFences[f.chunksInFlight]);
            f.chunksInFlight++;
            if (f.cmdBuffersUsed == f.cmdBuffers.size())
            {
                VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                cai.commandPool = f.pool;
                cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                cai.commandBufferCount = 1;
                VkCommandBuffer b;
                VK_CHECK(vkAllocateCommandBuffers(D, &cai, &b));
                f.cmdBuffers.push_back(b);
            }
            f.cmd = f.cmdBuffers[f.cmdBuffersUsed++];
            g_drawsSinceSubmit = 0;
            g_perf.chunks++;
        }

        void maybeSubmitChunk()
        {
            if (g_drawsSinceSubmit >= kChunkDraws) submitChunk();
        }

        // Wait for a frame's early submits and make their fences reusable
        void waitChunks(Frame& f)
        {
            if (!f.chunksInFlight) return;
            double w0 = nowMs();
            VK_CHECK(vkWaitForFences(D, f.chunksInFlight, f.chunkFences.data(), VK_TRUE, UINT64_MAX));
            g_perf.waitMs += nowMs() - w0;
            VK_CHECK(vkResetFences(D, f.chunksInFlight, f.chunkFences.data()));
            f.chunksInFlight = 0;
        }

        // Everything the frame recorded has executed: start it over at its first command buffer
        void restartFrame(Frame& f)
        {
            VK_CHECK(vkResetCommandPool(D, f.pool, 0));
            f.cmdBuffersUsed = 1;
            f.cmd = f.cmdBuffers[0];
            f.used = 0;
        }

        // Offset in this frame's upload buffer, or UINT64_MAX (and g_outOfSpace) when it is full
        bool g_outOfSpace = false;
        uint64_t g_drawsInFrame = 0;
        VkDeviceSize uploadAlloc(VkDeviceSize size, VkDeviceSize align = 256)
        {
            Frame& f = frame();
            VkDeviceSize off = (f.used + align - 1) & ~(align - 1);
            if (off + size > kUploadSize)
            {
                g_outOfSpace = true;
                return UINT64_MAX;
            }
            f.used = off + size;
            return off;
        }

        // ---------------------------------------------------------------- render targets

        struct Pass
        {
            Image* color[4]{};
            Image* depth = nullptr;
            bool active = false;
            uint32_t colorCount = 0; // color slots this pass actually carries (see colorSlots)
            VkExtent2D extent{};
        } g_pass;

        void endPass()
        {
            if (!g_pass.active) return;
            vkCmdEndRendering(frame().cmd);
            g_pass.active = false;
        }

        // Xenon's four render target slots are used loosely: a draw that writes depth only leaves
        // them all empty, and one may leave a slot in the middle empty as well (its write mask is
        // off, or the shader does not write that output). A pass then carries the slots up to the
        // last one with an image, and the pipeline is built with the same number, because a draw's
        // pipeline and its pass have to agree (VUID-vkCmdDraw-colorAttachmentCount-06179). What is
        // left inside that range without an image stays a slot with no format and no image.
        uint32_t colorSlots(const Image* const color[4])
        {
            uint32_t n = 0;
            for (uint32_t i = 0; i < 4; i++)
                if (color[i]) n = i + 1;
            return n;
        }

        // The pipeline side of the same rule, and it counts the same way: an empty slot has
        // VK_FORMAT_UNDEFINED in pColorAttachmentFormats, which is allowed. The two are set in
        // lockstep for a draw (one loop over s.color[] fills both), so the counts cannot disagree.
        uint32_t colorSlots(const VkFormat* colors)
        {
            uint32_t n = 0;
            for (uint32_t i = 0; i < 4; i++)
                if (colors[i] != VK_FORMAT_UNDEFINED) n = i + 1;
            return n;
        }

        // Which slots have something in them - what a pixel shader used in this pass may write
        // (see psModule).
        uint32_t colorMask(const VkFormat* colors)
        {
            uint32_t m = 0;
            for (uint32_t i = 0; i < 4; i++)
                if (colors[i] != VK_FORMAT_UNDEFINED) m |= 1u << i;
            return m;
        }

        void beginPass(Image* const color[4], Image* depth)
        {
            if (g_pass.active && std::equal(color, color + 4, g_pass.color) && depth == g_pass.depth) return;
            endPass();
            VkCommandBuffer c = cmd();
            VkRenderingAttachmentInfo ca[4]{};
            uint32_t w = UINT32_MAX, h = UINT32_MAX;
            for (int i = 0; i < 4; i++)
            {
                ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                g_pass.color[i] = color[i];
                // A slot the pass carries but has no image for (an empty target in front of a used
                // one) stays zero-initialized: with imageView VK_NULL_HANDLE the other members are
                // ignored and writes to it are discarded (VkRenderingAttachmentInfo in the spec).
                // VK_ATTACHMENT_LOAD_OP_NONE would need Vulkan 1.4 or VK_KHR_load_store_op_none.
                if (!color[i]) continue;
                transition(c, *color[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                ca[i].imageView = color[i]->view;
                ca[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                w = std::min(w, color[i]->width);
                h = std::min(h, color[i]->height);
            }
            VkRenderingAttachmentInfo da{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            if (depth)
            {
                transition(c, *depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
                da.imageView = depth->view;
                da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                w = std::min(w, depth->width);
                h = std::min(h, depth->height);
            }
            if (w == UINT32_MAX) w = h = 1; // no attachments: a memory export pass, rasterization discarded
            VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            ri.renderArea = { { 0, 0 }, { w, h } };
            ri.layerCount = 1;
            ri.colorAttachmentCount = g_pass.colorCount = colorSlots(color);
            ri.pColorAttachments = ca;
            ri.pDepthAttachment = depth ? &da : nullptr;
            ri.pStencilAttachment = depth ? &da : nullptr;
            vkCmdBeginRendering(c, &ri);
            g_pass.depth = depth;
            g_pass.extent = { w, h };
            g_pass.active = true;
        }

        // Submit what is recorded and continue in the next frame context (at swaps, and mid-frame
        // when the upload buffer is full)
        void submitAndAdvance()
        {
            endPass();
            Frame& f = frame();
            if (f.recording)
            {
                VK_CHECK(vkEndCommandBuffer(f.cmd));
                f.recording = false;
                queueSubmit(f.cmd, f.fence);
                f.submitted = true;
            }
            g_drawsSinceSubmit = 0;
            g_frameIndex = (g_frameIndex + 1) % kFramesInFlight;
            Frame& next = frame();
            if (next.submitted)
            {
                double w0 = nowMs();
                VK_CHECK(vkWaitForFences(D, 1, &next.fence, VK_TRUE, UINT64_MAX));
                g_perf.waitMs += nowMs() - w0;
                next.submitted = false;
            }
            waitChunks(next);
            VK_CHECK(vkResetFences(D, 1, &next.fence));
            restartFrame(next);
        }

        struct TargetKey
        {
            uint32_t base, pitch, samples, format;
            bool depth;
            auto operator<=>(const TargetKey&) const = default;
        };
        std::map<TargetKey, Image> g_targets;

        VkFormat colorTargetFormat(uint32_t f) // xenos::ColorRenderTargetFormat
        {
            switch (f)
            {
            case 0: case 1: return VK_FORMAT_R8G8B8A8_UNORM;            // 8_8_8_8 (gamma approximated as linear)
            case 2: case 10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; // 2_10_10_10 (+ AS_10_10_10_10)
            case 3: case 12: return VK_FORMAT_R16G16B16A16_SFLOAT;      // 2_10_10_10_FLOAT (+ AS_16_16_16_16)
            case 4: return VK_FORMAT_R16G16_SNORM;
            case 5: return VK_FORMAT_R16G16B16A16_SNORM;
            case 6: return VK_FORMAT_R16G16_SFLOAT;
            case 7: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case 14: return VK_FORMAT_R32_SFLOAT;
            case 15: return VK_FORMAT_R32G32_SFLOAT;
            default: return VK_FORMAT_UNDEFINED;
            }
        }

        Image* findTarget(const TargetKey& k)
        {
            auto it = g_targets.find(k);
            return it == g_targets.end() ? nullptr : &it->second;
        }

        Image* target(const TargetKey& k)
        {
            if (Image* t = findTarget(k)) return t;
            VkFormat format = k.depth ? kDepthFormat : colorTargetFormat(k.format);
            if (format == VK_FORMAT_UNDEFINED)
            {
                warnOnce("render target format " + std::to_string(k.format) + " not supported yet");
                return nullptr;
            }
            if (!k.pitch || k.pitch > 8192) return nullptr;
            // Height: what fits in EDRAM (2048 tiles of 80x16 samples at 32 bpp) from the base tile
            bool wide = !k.depth && (k.format == 5 || k.format == 7 || k.format == 15);
            uint32_t sx = k.samples >= 4 ? 2 : 1, sy = k.samples >= 2 ? 2 : 1;
            uint32_t tileW = 80 / sx / (wide ? 2 : 1), tileH = 16 / sy;
            uint32_t tilesPerRow = std::max(1u, (k.pitch + tileW - 1) / tileW);
            uint32_t rows = k.base < 2048 ? (2048 - k.base) / tilesPerRow : 1;
            uint32_t height = std::clamp(rows * tileH, 16u, 4096u);
            VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                      (k.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
            Image img = createImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, format, k.pitch * g_scale, height * g_scale, 1, 1, k.samples, usage);
            img.edram = true;
            img.edramWide = wide;
            img.edramBase = k.base;
            img.edramPitch = k.pitch;
            img.edramFormat = k.format;
            img.edramDepth = k.depth;
            // EDRAM starts with undefined contents; clear for reproducible output
            endPass();
            VkCommandBuffer c = cmd();
            transition(c, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageSubresourceRange range{ img.aspect, 0, 1, 0, 1 };
            if (k.depth)
            {
                VkClearDepthStencilValue v{ 1.0f, 0 };
                vkCmdClearDepthStencilImage(c, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
            }
            else
            {
                VkClearColorValue v{};
                vkCmdClearColorImage(c, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
            }
            info("[render] EDRAM %s target: tile %u, pitch %u, %ux msaa, format %u -> %ux%u image", k.depth ? "depth" : "color", k.base,
                 k.pitch, k.samples, k.format, img.width, img.height);
            return &(g_targets[k] = img);
        }

        // ---------------------------------------------------------------- EDRAM aliasing
        //
        // EDRAM is 2048 tiles of 80x16 samples (40x16 at 64 bits per sample). The game may draw
        // into the same tiles with different pitches, MSAA modes or formats (it clears the
        // 1280x720 screen as a 640-pitch 4x MSAA target, for example). Each tile remembers the
        // image that wrote it last; before an image is used, tiles last written through another
        // image are copied over by a shader that maps every sample to its EDRAM address in both
        // layouts; between different formats (depth and color share tiles every frame in the match),
        // each sample goes through the bits EDRAM would hold.

        Image* g_owner[2048] = {};

        struct EdramLayout { uint32_t sx, sy, tileSamplesX, tilesPerRow; };
        EdramLayout edramLayout(const Image& t)
        {
            EdramLayout l;
            l.sx = t.samples >= 4 ? 2 : 1;
            l.sy = t.samples >= 2 ? 2 : 1;
            l.tileSamplesX = t.edramWide ? 40 : 80;
            l.tilesPerRow = std::max(1u, (t.edramPitch * l.sx + l.tileSamplesX - 1) / l.tileSamplesX);
            return l;
        }

        // Calls fn(tile) for each EDRAM tile under guest pixel rectangle r of target t
        template <typename F> void forEachTile(const Image& t, VkRect2D r, F fn)
        {
            if (!r.extent.width || !r.extent.height) return;
            EdramLayout l = edramLayout(t);
            uint32_t c0 = r.offset.x * l.sx / l.tileSamplesX, c1 = ((r.offset.x + r.extent.width) * l.sx - 1) / l.tileSamplesX;
            uint32_t r0 = r.offset.y * l.sy / 16, r1 = ((r.offset.y + r.extent.height) * l.sy - 1) / 16;
            c1 = std::min(c1, l.tilesPerRow - 1);
            for (uint32_t row = r0; row <= r1; row++)
                for (uint32_t col = c0; col <= c1; col++)
                {
                    uint32_t tile = t.edramBase + row * l.tilesPerRow + col;
                    if (tile >= 2048) return;
                    fn(tile);
                }
        }

        void markWritten(Image* t, VkRect2D r)
        {
            if (t && t->edram) forEachTile(*t, r, [&](uint32_t tile) { g_owner[tile] = t; });
        }

        const char* kTransferVs = R"(#version 450
void main() { vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }
)";
        // Prefixed with #defines SRC_DEPTH, DEST_DEPTH, SRC_MSAA, DEST_MSAA (0/1); CONVERT (0: same
        // format, the value is copied; 1: through the raw EDRAM bits); SRC_FMT, DEST_FMT (the guest
        // format, see transferKind) and SRC_WORDS, DEST_WORDS (32-bit words per sample: 2 for 64bpp).
        // Packing follows Xenia's ownership transfer shaders (vulkan_render_target_cache.cc,
        // GetTransferShader): depth is depth24 << 8 | stencil,
        // 8_8_8_8 has red in the low byte, 16-bit formats are their 16-bit integers, 32-bit floats
        // their bits; the 7e3 and 20e4 helpers are Xenia's (spirv_shader_translator_rb.cc, xenos.cc).
        const char* kTransferFs = R"(
#if DEST_DEPTH
#extension GL_ARB_shader_stencil_export : require
#endif
layout(push_constant) uniform P { int dBase, dTpr, dSx, dSy, dTileX, sBase, sTpr, sSx, sSy, sTileX, scale; } p;
#if SRC_MSAA
layout(set = 0, binding = 0) uniform sampler2DMS src;
#if SRC_DEPTH
layout(set = 0, binding = 1) uniform usampler2DMS srcStencil;
#endif
#define FETCH(t, c, s) texelFetch(t, c, s)
#else
layout(set = 0, binding = 0) uniform sampler2D src;
#if SRC_DEPTH
layout(set = 0, binding = 1) uniform usampler2D srcStencil;
#endif
#define FETCH(t, c, s) texelFetch(t, c, 0)
#endif
layout(set = 0, binding = 2, std140) uniform Mask { uvec4 bits[16]; } m; // tiles to copy
#if !DEST_DEPTH
layout(location = 0) out vec4 o;
#endif
// Sample index <-> position in the pixel's sample grid (Vulkan standard sample positions:
// 4x = 2x2 grid in reading order, 2x = sample 1 on top of sample 0)
ivec2 subOf(int s, int sx, int sy) { return sx == 2 ? ivec2(s & 1, s >> 1) : sy == 2 ? ivec2(0, s == 0 ? 1 : 0) : ivec2(0); }
int indexOf(ivec2 sub, int sx, int sy) { return sx == 2 ? sub.y * 2 + sub.x : sy == 2 ? (sub.y == 0 ? 1 : 0) : 0; }
// Host pixel and sample index of source EDRAM sample (x, y): x in samples along the source's rows
// of tiles, y in sample rows
void locate(int x, int y, ivec2 f, out ivec2 sp, out int ss) {
  sp = ivec2(x / p.sSx, y / p.sSy) * p.scale + f;
  ss = indexOf(ivec2(x % p.sSx, y % p.sSy), p.sSx, p.sSy);
}
#if CONVERT
// 7e3 (2_10_10_10_FLOAT color, [0, 31.875]) and 20e4 (D24FS8 depth, [0, 2)), rounding to nearest even
uint f32To7e3(float f) {
  uint u = floatBitsToUint(f > 0.0 ? min(f, 31.875) : 0.0);
  uint b = u < 0x3E800000u ? ((u & 0x7FFFFFu) | 0x800000u) >> min(125u - (u >> 23), 24u) : u - (124u << 23);
  return ((b + 0x7FFFu + ((b >> 16) & 1u)) >> 16) & 0x3FFu;
}
float f7e3To32(uint v) {
  v &= 0x3FFu;
  if (v == 0u) return 0.0;
  uint mant = v & 0x7Fu;
  int e = int(v >> 7);
  if (e == 0) { int lz = 7 - findMSB(mant); e = 1 - lz; mant = (mant << lz) & 0x7Fu; }
  return uintBitsToFloat((uint(e + 124) << 23) | (mant << 16));
}
uint f32To20e4(float f) {
  if (!(f > 0.0)) return 0u;
  uint u = floatBitsToUint(f);
  if (u >= 0x3FFFFFF8u) return 0xFFFFFFu;
  u = u < 0x38800000u ? (0x800000u | (u & 0x7FFFFFu)) >> min(113u - (u >> 23), 24u) : u + 0xC8000000u;
  u += 3u + ((u >> 3) & 1u);
  return (u >> 3) & 0xFFFFFFu;
}
float f20e4To32(uint v) {
  v &= 0xFFFFFFu;
  if (v == 0u) return 0.0;
  uint mant = v & 0xFFFFFu;
  int e = int(v >> 20);
  if (e == 0) { int lz = 20 - findMSB(mant); e = 1 - lz; mant = (mant << lz) & 0xFFFFFu; }
  return uintBitsToFloat((uint(e + 112) << 23) | (mant << 3));
}
// The source sample as the bits EDRAM holds (the second word only for 64bpp formats)
uvec2 readPacked(ivec2 sp, int ss) {
#if SRC_DEPTH
  float d = FETCH(src, sp, ss).r;
#if SRC_FMT == 1
  uint d24 = f32To20e4(d);
#else
  // round(d * 2^24), minus one above 2^23: the exact inverse of the UNorm24To32 below, so depth
  // survives a trip through another format bit for bit (Xenia's round(d * 0xFFFFFF) in float32
  // loses the low bit of 1 in 8 values), and as close to exact rounding for other depths
  uint n = uint(roundEven(clamp(d, 0.0, 1.0) * 16777216.0));
  uint d24 = n - uint(n > 0x800000u);
#endif
  return uvec2((d24 << 8) | (FETCH(srcStencil, sp, ss).r & 0xFFu), 0u);
#else
  vec4 c = FETCH(src, sp, ss);
#if SRC_FMT == 0
  uvec4 u = uvec4(clamp(c, 0.0, 1.0) * 255.0 + 0.5);
  return uvec2(u.r | (u.g << 8) | (u.b << 16) | (u.a << 24), 0u);
#elif SRC_FMT == 2
  uvec4 u = uvec4(clamp(c, 0.0, 1.0) * vec4(1023.0, 1023.0, 1023.0, 3.0) + 0.5);
  return uvec2(u.r | (u.g << 10) | (u.b << 20) | (u.a << 30), 0u);
#elif SRC_FMT == 3
  uint a = uint(clamp(c.a, 0.0, 1.0) * 3.0 + 0.5);
  return uvec2(f32To7e3(c.r) | (f32To7e3(c.g) << 10) | (f32To7e3(c.b) << 20) | (a << 30), 0u);
#elif SRC_FMT == 4 || SRC_FMT == 5
  uvec4 u = uvec4(ivec4(round(clamp(c, -1.0, 1.0) * 32767.0))) & 0xFFFFu;
  return uvec2(u.r | (u.g << 16), u.b | (u.a << 16));
#elif SRC_FMT == 6 || SRC_FMT == 7
  return uvec2(packHalf2x16(c.rg), packHalf2x16(c.ba));
#else
  return floatBitsToUint(c.rg);
#endif
#endif
}
// Bits as EDRAM holds them -> the destination's format
void writeUnpacked(uvec2 v) {
#if DEST_DEPTH
  uint d24 = v.x >> 8;
#if DEST_FMT == 1
  gl_FragDepth = f20e4To32(d24);
#else
  gl_FragDepth = float(d24 + (d24 >> 23)) * (1.0 / 16777216.0); // Xenia's UNorm24To32
#endif
  gl_FragStencilRefARB = int(v.x & 0xFFu);
#elif DEST_FMT == 0
  o = vec4((uvec4(v.x) >> uvec4(0, 8, 16, 24)) & 0xFFu) / 255.0;
#elif DEST_FMT == 2
  o = vec4((uvec4(v.x) >> uvec4(0, 10, 20, 30)) & uvec4(0x3FFu, 0x3FFu, 0x3FFu, 3u)) / vec4(1023.0, 1023.0, 1023.0, 3.0);
#elif DEST_FMT == 3
  o = vec4(f7e3To32(v.x), f7e3To32(v.x >> 10), f7e3To32(v.x >> 20), float(v.x >> 30) / 3.0);
#elif DEST_FMT == 4 || DEST_FMT == 5
  o = max(vec4(ivec4(uvec4(v.x, v.x, v.y, v.y) << uvec4(16, 0, 16, 0)) >> 16) / 32767.0, -1.0);
#elif DEST_FMT == 6 || DEST_FMT == 7
  o = vec4(unpackHalf2x16(v.x), unpackHalf2x16(v.y));
#else
  o = vec4(uintBitsToFloat(v), 0.0, 0.0);
#endif
}
#endif
void main() {
  ivec2 h = ivec2(gl_FragCoord.xy), g = h / p.scale, f = h % p.scale;
#if DEST_MSAA
  ivec2 sub = subOf(gl_SampleID, p.dSx, p.dSy);
#else
  ivec2 sub = ivec2(0);
#endif
  int X = g.x * p.dSx + sub.x, Y = g.y * p.dSy + sub.y; // destination sample in EDRAM sample rows
  int tile = p.dBase + (Y >> 4) * p.dTpr + X / p.dTileX;
  if (tile >= 2048 || ((m.bits[tile >> 7][(tile >> 5) & 3] >> uint(tile & 31)) & 1u) == 0u) discard;
  int rel = tile - p.sBase;
  if (rel < 0) discard;
  int x0 = (rel % p.sTpr) * p.sTileX, ys = (rel / p.sTpr) * 16 + (Y & 15), xt = X % p.dTileX;
  ivec2 sp;
  int ss;
#if !CONVERT
  locate(x0 + xt, ys, f, sp, ss);
#if DEST_DEPTH
  gl_FragDepth = FETCH(src, sp, ss).r;
  gl_FragStencilRefARB = int(FETCH(srcStencil, sp, ss).r);
#else
  o = FETCH(src, sp, ss);
#endif
#else
  // Word by word: a tile row is 80 32-bit words whatever the format; a 64bpp sample is two of
  // them, its first components (R, G) in the first
  uvec2 v = uvec2(0u);
  for (int k = 0; k < DEST_WORDS; k++) {
    int w = xt * DEST_WORDS + k;
    locate(x0 + w / SRC_WORDS, ys, f, sp, ss);
    uvec2 s = readPacked(sp, ss);
    v[k] = (w % SRC_WORDS) == 0 ? s.x : s.y;
  }
  writeUnpacked(v);
#endif
}
)";

        VkDescriptorSetLayout g_transferSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout g_transferLayout = VK_NULL_HANDLE;
        VkShaderModule g_transferVs = VK_NULL_HANDLE;
        VkSampler g_nearest = VK_NULL_HANDLE;
        // Source kind, destination kind, destination samples, source MSAA
        std::map<std::tuple<uint32_t, uint32_t, uint32_t, bool>, VkPipeline> g_transferPipelines;

        // What decides how a target's samples map to EDRAM bits: depth (0x100 | xenos::DepthRenderTargetFormat)
        // or the color format, with the formats that share a bit layout merged (8_8_8_8_GAMMA is
        // 8_8_8_8, the AS_ variants are their base formats)
        uint32_t transferKind(const Image& i)
        {
            if (i.edramDepth) return 0x100 | i.edramFormat;
            return i.edramFormat == 1 ? 0 : i.edramFormat == 10 ? 2 : i.edramFormat == 12 ? 3 : i.edramFormat;
        }

        const char* kindName(uint32_t kind)
        {
            switch (kind)
            {
            case 0x100: return "D24S8"; case 0x101: return "D24FS8";
            case 0: return "8_8_8_8"; case 2: return "2_10_10_10"; case 3: return "2_10_10_10_FLOAT"; case 4: return "16_16";
            case 5: return "16_16_16_16"; case 6: return "16_16_FLOAT"; case 7: return "16_16_16_16_FLOAT";
            case 14: return "32_FLOAT"; case 15: return "32_32_FLOAT"; default: return "?";
            }
        }

        VkPipeline transferPipeline(const Image& srcImage, const Image& dstImage)
        {
            uint32_t srcKind = transferKind(srcImage), dstKind = transferKind(dstImage);
            bool srcMsaa = srcImage.samples > 1;
            uint32_t destSamples = dstImage.samples;
            VkFormat format = dstImage.format;
            auto key = std::make_tuple(srcKind, dstKind, destSamples, srcMsaa);
            auto it = g_transferPipelines.find(key);
            if (it != g_transferPipelines.end()) return it->second;
            bool depth = dstImage.edramDepth;
            if (!g_transferLayout)
            {
                VkDescriptorSetLayoutBinding b[3] = {
                    { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                    { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                    { 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr } };
                VkDescriptorSetLayoutCreateInfo dl{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
                dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
                dl.bindingCount = 3;
                dl.pBindings = b;
                VK_CHECK(vkCreateDescriptorSetLayout(D, &dl, nullptr, &g_transferSetLayout));
                VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 44 };
                VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
                pli.setLayoutCount = 1;
                pli.pSetLayouts = &g_transferSetLayout;
                pli.pushConstantRangeCount = 1;
                pli.pPushConstantRanges = &pcr;
                VK_CHECK(vkCreatePipelineLayout(D, &pli, nullptr, &g_transferLayout));
                std::string error;
                g_transferVs = compile(kTransferVs, shaderc_vertex_shader, "edram_transfer.vert", error);
                if (!g_transferVs) fatal("[render] EDRAM transfer vertex shader: %s", error.c_str());
                VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
                VK_CHECK(vkCreateSampler(D, &si, nullptr, &g_nearest));
            }
            bool convert = srcKind != dstKind;
            auto def = [](const char* name, uint32_t v) { return std::string("#define ") + name + " " + std::to_string(v) + "\n"; };
            std::string src = "#version 450\n" + def("SRC_DEPTH", srcImage.edramDepth) + def("DEST_DEPTH", depth) + def("SRC_MSAA", srcMsaa) +
                              def("DEST_MSAA", destSamples > 1) + def("CONVERT", convert) + def("SRC_FMT", srcKind & 0xFF) +
                              def("DEST_FMT", dstKind & 0xFF) + def("SRC_WORDS", srcImage.edramWide ? 2 : 1) +
                              def("DEST_WORDS", dstImage.edramWide ? 2 : 1) + kTransferFs;
            std::string error;
            VkShaderModule fs = compile(src, shaderc_fragment_shader, "edram_transfer.frag", error);
            if (!fs) fatal("[render] EDRAM transfer fragment shader: %s", error.c_str());

            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, g_transferVs, "main" };
            stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" };
            VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            vp.viewportCount = vp.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            rs.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            ms.rasterizationSamples = VkSampleCountFlagBits(destSamples);
            ms.sampleShadingEnable = destSamples > 1;
            ms.minSampleShading = 1.0f;
            VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
            ds.depthTestEnable = ds.depthWriteEnable = depth;
            ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
            ds.stencilTestEnable = depth && C->stencilExport;
            ds.front = ds.back = { VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS, 0xFF, 0xFF, 0 };
            VkPipelineColorBlendAttachmentState att{};
            att.colorWriteMask = 0xF;
            VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            cb.attachmentCount = depth ? 0 : 1;
            cb.pAttachments = &att;
            // The same dynamic states as the 3D pipelines (see pipeline() below): those draws set
            // stencil masks, blend constants and depth bias with their own commands, and drawing a
            // pipeline that states any of them statically after that is an error
            // (VUID-vkCmdDraw-None-08608). transfer(), below, sets them.
            static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                                  VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                                  VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BIAS };
            VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dy.dynamicStateCount = uint32_t(std::size(dyn));
            dy.pDynamicStates = dyn;
            VkPipelineRenderingCreateInfo rci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
            rci.colorAttachmentCount = depth ? 0 : 1;
            rci.pColorAttachmentFormats = &format;
            rci.depthAttachmentFormat = rci.stencilAttachmentFormat = depth ? format : VK_FORMAT_UNDEFINED;
            VkGraphicsPipelineCreateInfo gci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            gci.pNext = &rci;
            gci.stageCount = 2;
            gci.pStages = stages;
            gci.pVertexInputState = &vi;
            gci.pInputAssemblyState = &ia;
            gci.pViewportState = &vp;
            gci.pRasterizationState = &rs;
            gci.pMultisampleState = &ms;
            gci.pDepthStencilState = &ds;
            gci.pColorBlendState = &cb;
            gci.pDynamicState = &dy;
            gci.layout = g_transferLayout;
            VkPipeline pl;
            VK_CHECK(vkCreateGraphicsPipelines(D, VK_NULL_HANDLE, 1, &gci, nullptr, &pl));
            vkDestroyShaderModule(D, fs, nullptr);
            info("[render] EDRAM transfer pipeline: %s %ux -> %s %ux%s", kindName(srcKind), srcImage.samples, kindName(dstKind), destSamples,
                 convert ? " (through the EDRAM bits)" : "");
            return g_transferPipelines[key] = pl;
        }

        // Copy the given EDRAM tiles from src into dst with the layout mapping, converting through
        // the raw EDRAM bits when the formats differ
        void transfer(Image& src, Image& dst, const uint32_t mask[64], VkRect2D guestRect)
        {
            VkDeviceSize maskOff = uploadAlloc(256);
            if (maskOff == UINT64_MAX) { warnOnce("upload buffer full: EDRAM transfer skipped"); return; }
            std::memcpy(frame().uploadMap + maskOff, mask, 256);
            bool depth = dst.edramDepth, srcDepth = src.edramDepth;
            VkPipeline pl = transferPipeline(src, dst);
            endPass();
            VkCommandBuffer c = cmd();
            VkImageLayout srcLayout = srcDepth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkImageLayout dstLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            transition(c, src, srcLayout);
            transition(c, dst, dstLayout);
            VkRenderingAttachmentInfo a{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
            a.imageView = dst.view;
            a.imageLayout = dstLayout;
            a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            ri.renderArea = { { 0, 0 }, { dst.width, dst.height } };
            ri.layerCount = 1;
            ri.colorAttachmentCount = depth ? 0 : 1;
            ri.pColorAttachments = &a;
            ri.pDepthAttachment = depth ? &a : nullptr;
            ri.pStencilAttachment = depth && C->stencilExport ? &a : nullptr;
            vkCmdBeginRendering(c, &ri);
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
            // A state that is dynamic in this pipeline still has to be set before it is used, and
            // only then: the blend factors are One and Zero (no constant color), depth bias is off,
            // and there is no stencil state unless the transfer keeps the guest's stencil values.
            if (depth && C->stencilExport)
            {
                vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_BIT, 0);
                vkCmdSetStencilReference(c, VK_STENCIL_FACE_BACK_BIT, 0);
                vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_BIT, 0xFF);
                vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_BACK_BIT, 0xFF);
                vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_BIT, 0xFF);
                vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_BACK_BIT, 0xFF);
            }
            VkDescriptorImageInfo images[2] = { { g_nearest, srcDepth ? src.depthView : src.view, srcLayout },
                                                { g_nearest, srcDepth ? src.stencilView : src.view, srcLayout } };
            VkDescriptorBufferInfo maskInfo{ frame().upload, maskOff, 256 };
            VkWriteDescriptorSet w[3]{};
            for (uint32_t i = 0; i < 3; i++)
            {
                w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                w[i].dstBinding = i;
                w[i].descriptorCount = 1;
                w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                if (i < 2) w[i].pImageInfo = &images[i];
                else w[i].pBufferInfo = &maskInfo;
            }
            if (srcDepth) C->cmdPushDescriptorSet(c, VK_PIPELINE_BIND_POINT_GRAPHICS, g_transferLayout, 0, 3, w);
            else { w[1] = w[2]; C->cmdPushDescriptorSet(c, VK_PIPELINE_BIND_POINT_GRAPHICS, g_transferLayout, 0, 2, w); }
            EdramLayout dl = edramLayout(dst), sl = edramLayout(src);
            int32_t pc[11] = { int32_t(dst.edramBase), int32_t(dl.tilesPerRow), int32_t(dl.sx),          int32_t(dl.sy),
                               int32_t(dl.tileSamplesX), int32_t(src.edramBase), int32_t(sl.tilesPerRow), int32_t(sl.sx),
                               int32_t(sl.sy),           int32_t(sl.tileSamplesX), int32_t(g_scale) };
            vkCmdPushConstants(c, g_transferLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
            VkViewport viewport{ 0, 0, float(dst.width), float(dst.height), 0.0f, 1.0f };
            vkCmdSetViewport(c, 0, 1, &viewport);
            VkRect2D scissor = scaled(guestRect);
            scissor.extent.width = std::min(scissor.extent.width, dst.width - std::min<uint32_t>(dst.width, scissor.offset.x));
            scissor.extent.height = std::min(scissor.extent.height, dst.height - std::min<uint32_t>(dst.height, scissor.offset.y));
            vkCmdSetScissor(c, 0, 1, &scissor);
            vkCmdDraw(c, 3, 1, 0, 0);
            vkCmdEndRendering(c);
        }

        // Rectangle r of target t grown to whole EDRAM tiles: ownership moves a tile at a time, so
        // the part of a tile outside r has to come along
        VkRect2D tileAligned(const Image& t, VkRect2D r)
        {
            EdramLayout l = edramLayout(t);
            int32_t tw = int32_t(l.tileSamplesX / l.sx), th = int32_t(16 / l.sy); // tile size in pixels
            int32_t x0 = r.offset.x / tw * tw, y0 = r.offset.y / th * th;
            int32_t x1 = (r.offset.x + int32_t(r.extent.width) + tw - 1) / tw * tw, y1 = (r.offset.y + int32_t(r.extent.height) + th - 1) / th * th;
            return { { x0, y0 }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } };
        }

        // The pixels of target t covering the tiles in mask (a bounding box): what a transfer of
        // those tiles has to shade
        VkRect2D tilesRect(const Image& t, const uint32_t mask[64])
        {
            EdramLayout l = edramLayout(t);
            int32_t tw = int32_t(l.tileSamplesX / l.sx), th = int32_t(16 / l.sy);
            int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = 0, y1 = 0;
            for (uint32_t tile = t.edramBase; tile < 2048; tile++)
                if ((mask[tile >> 5] >> (tile & 31)) & 1)
                {
                    int32_t rel = int32_t(tile - t.edramBase), col = rel % int32_t(l.tilesPerRow), row = rel / int32_t(l.tilesPerRow);
                    x0 = std::min(x0, col * tw), y0 = std::min(y0, row * th);
                    x1 = std::max(x1, (col + 1) * tw), y1 = std::max(y1, (row + 1) * th);
                }
            if (x0 >= x1 || y0 >= y1) return { { 0, 0 }, { 0, 0 } };
            return { { x0, y0 }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } };
        }

        // Before target t is used inside guest rectangle r: bring in tiles another image wrote last
        void syncTarget(Image* t, VkRect2D r)
        {
            if (!t || !t->edram) return;
            std::map<Image*, std::array<uint32_t, 64>> masks;
            forEachTile(*t, r, [&](uint32_t tile) {
                Image* o = g_owner[tile];
                if (o && o != t) masks[o][tile >> 5] |= 1u << (tile & 31);
            });
            for (auto& [owner, mask] : masks)
            {
                transfer(*owner, *t, mask.data(), tilesRect(*t, mask.data()));
                for (uint32_t tile = 0; tile < 2048; tile++)
                    if ((mask[tile >> 5] >> (tile & 31)) & 1) g_owner[tile] = t;
            }
        }

        // Before a clear of rectangle r, which takes over every tile it touches: tiles it covers
        // only in part keep their other contents, so bring those in first
        void syncPartialTiles(Image* t, VkRect2D r)
        {
            VkRect2D whole = tileAligned(*t, r);
            if (whole.offset.x != r.offset.x || whole.offset.y != r.offset.y || whole.extent.width != r.extent.width ||
                whole.extent.height != r.extent.height)
                syncTarget(t, r);
        }

        // Resolve destinations, by guest physical address (the latest image written there).
        // Replaced images are kept alive: the presenter may still be showing them.
        std::map<uint32_t, Image> g_resolved;
        std::vector<Image> g_retired;

        // ---------------------------------------------------------------- textures

        // Xenos texture swizzles: 3 bits per component (0-3 = R, G, B, A of the texture, 4 = 0, 5 = 1)
        constexpr uint32_t makeSwizzle(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | (g << 3) | (b << 6) | (a << 9); }
        constexpr uint32_t kSwizzleRGBA = makeSwizzle(0, 1, 2, 3), kSwizzleRRRR = 0, kSwizzleRGGG = makeSwizzle(0, 1, 1, 1),
                           kSwizzleRGBB = makeSwizzle(0, 1, 2, 2);

        // The guest fetch swizzle applied through the host format's own swizzle (Xenia's
        // TextureCache::GuestToHostSwizzle): components a format lacks read as Xenos returns them,
        // e.g. RRRR for one-channel formats. 6 and 7 become 0 and 1.
        uint32_t hostSwizzle(uint32_t guest, uint32_t formatSwizzle)
        {
            uint32_t out = 0;
            for (uint32_t i = 0; i < 4; i++)
            {
                uint32_t g = (guest >> (3 * i)) & 7;
                out |= (g >= 4 ? (g & 5) : (formatSwizzle >> (3 * g)) & 7) << (3 * i);
            }
            return out;
        }

        // `convert`: 16-bit formats expanded to RGBA8 on upload (component X = lowest bits -> R);
        // 24_8 depth (24-bit unorm in the high bits) converted to float as Xenia's XeUNorm24To32.
        // Vulkan formats and swizzles follow Xenia's vulkan_texture_cache.cc host format table.
        enum class Convert { None, From4444, From565, From1555, FromDepth24 };
        struct TextureFormat
        {
            VkFormat vk;
            uint32_t blockW, blockH, bytes;
            Convert convert = Convert::None;
            uint32_t swizzle = kSwizzleRGBA;
        };
        TextureFormat textureFormat(uint32_t f) // xenos::TextureFormat
        {
            switch (f)
            {
            case 2: return { VK_FORMAT_R8_UNORM, 1, 1, 1, Convert::None, kSwizzleRRRR };               // 8
            case 3: return { VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 2, Convert::From1555 };                  // 1_5_5_5
            case 4: return { VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 2, Convert::From565, kSwizzleRGBB };     // 5_6_5
            case 15: return { VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 2, Convert::From4444 };                 // 4_4_4_4
            case 6: return { VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 4 };                                     // 8_8_8_8
            case 7: return { VK_FORMAT_A2B10G10R10_UNORM_PACK32, 1, 1, 4 };                           // 2_10_10_10
            case 10: return { VK_FORMAT_R8G8_UNORM, 1, 1, 2, Convert::None, kSwizzleRGGG };           // 8_8
            case 18: return { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 4, 8 };                              // DXT1
            case 19: return { VK_FORMAT_BC2_UNORM_BLOCK, 4, 4, 16 };                                  // DXT2_3
            case 20: return { VK_FORMAT_BC3_UNORM_BLOCK, 4, 4, 16 };                                  // DXT4_5
            case 22: return { VK_FORMAT_R32_SFLOAT, 1, 1, 4, Convert::FromDepth24, kSwizzleRRRR };    // 24_8
            case 25: return { VK_FORMAT_R16G16_UNORM, 1, 1, 4, Convert::None, kSwizzleRGGG };         // 16_16
            case 26: return { VK_FORMAT_R16G16B16A16_UNORM, 1, 1, 8 };                                // 16_16_16_16
            case 27: return { VK_FORMAT_R16_SFLOAT, 1, 1, 2, Convert::None, kSwizzleRRRR };           // 16_EXPAND
            case 28: return { VK_FORMAT_R16G16_SFLOAT, 1, 1, 4, Convert::None, kSwizzleRGGG };        // 16_16_EXPAND
            case 29: return { VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1, 8 };                               // 16_16_16_16_EXPAND
            case 30: return { VK_FORMAT_R16_SFLOAT, 1, 1, 2, Convert::None, kSwizzleRRRR };           // 16_FLOAT
            case 31: return { VK_FORMAT_R16G16_SFLOAT, 1, 1, 4, Convert::None, kSwizzleRGGG };        // 16_16_FLOAT
            case 32: return { VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1, 8 };                               // 16_16_16_16_FLOAT
            case 36: return { VK_FORMAT_R32_SFLOAT, 1, 1, 4, Convert::None, kSwizzleRRRR };           // 32_FLOAT
            case 37: return { VK_FORMAT_R32G32_SFLOAT, 1, 1, 8, Convert::None, kSwizzleRGGG };        // 32_32_FLOAT
            default: return { VK_FORMAT_UNDEFINED, 1, 1, 0 };
            }
        }

        // Can a texture of Xenos format `f` sample a resolve destination of format `r` directly?
        // Resolves are not written back to guest memory, and the copy's own format conversion
        // (RB_COPY_DEST_INFO) is skipped, so this matches by channel layout.
        bool resolveCompatible(uint32_t f, VkFormat r)
        {
            switch (f)
            {
            case 6: case 3: case 4: case 15: return r == VK_FORMAT_R8G8B8A8_UNORM; // 8_8_8_8 and the 16-bit RGBA formats
            case 7: return r == VK_FORMAT_A2B10G10R10_UNORM_PACK32;                // 2_10_10_10
            case 25: return r == VK_FORMAT_R16G16_SNORM;                           // 16_16 (EDRAM 16_16 targets are SNORM)
            case 26: return r == VK_FORMAT_R16G16B16A16_SNORM;                     // 16_16_16_16
            case 28: case 31: return r == VK_FORMAT_R16G16_SFLOAT;                 // 16_16_EXPAND, 16_16_FLOAT
            case 29: case 32: return r == VK_FORMAT_R16G16B16A16_SFLOAT;           // 16_16_16_16_EXPAND, _FLOAT
            case 37: return r == VK_FORMAT_R32G32_SFLOAT;                          // 32_32_FLOAT
            case 22: case 23: case 36: return r == VK_FORMAT_R32_SFLOAT;          // 24_8, 24_8_FLOAT (depth resolves), 32_FLOAT
            default: return false;
            }
        }

        struct TextureKey
        {
            uint32_t base, format, endian, width, height, pitch;
            uint32_t mips = 0; // mip address | packed << 1 | level count - 1 (bits 0 and 5-11 of a 4 KB-aligned address are free)
            auto operator<=>(const TextureKey&) const = default;
        };
        struct Texture
        {
            Image img;
            uint64_t uploadedFrame = 0;
            uint64_t hash = 0; // of the guest bytes last uploaded (0: never uploaded)
            std::map<uint32_t, VkImageView> views; // by fetch constant swizzle
            bool everUploaded = false;             // BB_PERF: first upload vs re-upload
        };
        std::map<TextureKey, Texture> g_textures;
        uint64_t g_texturesUploaded = 0, g_texturesKept = 0; // for the stats line
        Image g_placeholder[4]; // per dimension: 1D, 2D, 3D, cube (magenta)
        std::map<uint64_t, VkSampler> g_samplers;

        Image& placeholder(uint32_t dim)
        {
            Image& p = g_placeholder[dim];
            if (p.image) return p;
            static const VkImageType types[4] = { VK_IMAGE_TYPE_1D, VK_IMAGE_TYPE_2D, VK_IMAGE_TYPE_3D, VK_IMAGE_TYPE_2D };
            static const VkImageViewType views[4] = { VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_CUBE };
            endPass();
            p = createImage(types[dim], views[dim], VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, dim == 3 ? 6 : 1, 1,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, dim == 3 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0);
            VkCommandBuffer c = cmd();
            transition(c, p, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue magenta{ { 1.0f, 0.0f, 1.0f, 1.0f } };
            VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, dim == 3 ? 6u : 1u };
            vkCmdClearColorImage(c, p.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &magenta, 1, &range);
            transition(c, p, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            return p;
        }

        // Sampler for a texture fetch constant: clamp modes, filters (mag/min/mip: 0 point, 1 linear,
        // 2 base map = no mips), anisotropy (0 off, n = 2^(n-1):1), the mip range, the LOD bias
        // (signed, 1/32 steps) and the border colour. Shader fetch instructions could override the
        // filters; none of the match's do (all leave them to the fetch constant).
        VkSampler sampler(const uint32_t d[6])
        {
            uint32_t clamp = (d[0] >> 10) & 0x1FF, filters = (d[3] >> 19) & 0x3F, aniso = (d[3] >> 25) & 7;
            uint32_t mipMin = (d[4] >> 2) & 0xF, mipMax = (d[4] >> 6) & 0xF, lodBias = (d[4] >> 12) & 0x3FF, border = d[5] & 3;
            uint64_t key = clamp | (uint64_t(filters) << 9) | (uint64_t(aniso) << 15) | (uint64_t(mipMin) << 18) | (uint64_t(mipMax) << 22) |
                           (uint64_t(lodBias) << 26) | (uint64_t(border) << 36);
            auto it = g_samplers.find(key);
            if (it != g_samplers.end()) return it->second;
            static const VkSamplerAddressMode modes[8] = {
                VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE };
            VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            si.magFilter = (filters & 3) == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            si.minFilter = ((filters >> 2) & 3) == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            uint32_t mip = (filters >> 4) & 3;
            si.mipmapMode = mip == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            si.addressModeU = modes[clamp & 7];
            si.addressModeV = modes[(clamp >> 3) & 7];
            si.addressModeW = modes[(clamp >> 6) & 7];
            // Base map: the base level only. Otherwise the fetch constant's level range (Vulkan
            // clamps it to the levels the image has).
            si.minLod = mip == 2 ? 0.0f : float(mipMin);
            si.maxLod = mip == 2 ? 0.0f : float(std::max(mipMin, mipMax));
            si.mipLodBias = float(int32_t(lodBias << 22) >> 22) / 32.0f;
            float wanted = aniso >= 2 && aniso <= 5 ? float(1u << (aniso - 1)) : 1.0f;
            if (wanted > 1.0f) si.minFilter = si.magFilter = VK_FILTER_LINEAR; // the game's anisotropic filtering is linear
            // --anisotropy: at least that much where the texture is minified linearly through mips
            // (not base-map textures: the UI)
            if (g_forcedAniso > 1 && si.minFilter == VK_FILTER_LINEAR && mip != 2) wanted = std::max(wanted, float(g_forcedAniso));
            if (g_forcedAniso > 1 && !C->samplerAnisotropy) warnOnce("--anisotropy ignored: the GPU has no samplerAnisotropy");
            if (wanted > 1.0f && C->samplerAnisotropy)
            {
                si.anisotropyEnable = VK_TRUE;
                si.maxAnisotropy = std::min(wanted, C->props.limits.maxSamplerAnisotropy);
            }
            // Border colour (used by clamp-to-border): 0 black (all zero), 1 opaque white, 2/3 YCbCr
            // black (core Vulkan has no such colour; not seen in the match, so all-zero). The shadow
            // maps are white-bordered (issue #64): outside a map the depth reads 1.0 (lit), and with a
            // black border each map's whole projected square came out shadowed.
            si.borderColor = border == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            VkSampler sm;
            VK_CHECK(vkCreateSampler(D, &si, nullptr, &sm));
            return g_samplers[key] = sm;
        }

        VkComponentSwizzle swizzleComponent(uint32_t s, bool swapRB)
        {
            static const VkComponentSwizzle comps[8] = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
                                                         VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE,
                                                         VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE };
            if (swapRB && (s == 0 || s == 2)) s ^= 2; // memory holds B, G, R, A
            return comps[s & 7];
        }

        VkImageView createView(VkImage image, VkImageViewType type, VkFormat format, uint32_t layers, uint32_t swizzle, bool swapRB)
        {
            VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vci.image = image;
            vci.viewType = type;
            vci.format = format;
            vci.components = { swizzleComponent(swizzle & 7, swapRB), swizzleComponent((swizzle >> 3) & 7, swapRB),
                               swizzleComponent((swizzle >> 6) & 7, swapRB), swizzleComponent((swizzle >> 9) & 7, swapRB) };
            vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, layers };
            VkImageView view;
            VK_CHECK(vkCreateImageView(D, &vci, nullptr, &view));
            return view;
        }

        // Fast 64-bit hash of guest bytes, for "did this texture change since its last upload"
        // (four independent lanes; not cryptographic). Reading the bytes costs far less than
        // swapping, untiling and uploading them.
        uint64_t hashBytes(const uint8_t* p, size_t n, uint64_t seed)
        {
            constexpr uint64_t k = 0x9E3779B97F4A7C15ull;
            uint64_t lane[4] = { seed ^ 0x243F6A8885A308D3ull, seed ^ 0x13198A2E03707344ull, seed ^ 0xA4093822299F31D0ull,
                                 seed ^ 0x082EFA98EC4E6C89ull };
            size_t i = 0;
            for (; i + 32 <= n; i += 32)
                for (int l = 0; l < 4; l++)
                {
                    uint64_t w;
                    std::memcpy(&w, p + i + l * 8, 8);
                    lane[l] = (lane[l] ^ w) * k;
                    lane[l] ^= lane[l] >> 32;
                }
            uint64_t h = n;
            for (; i < n; i++) h = (h ^ p[i]) * k; // the last few bytes
            for (int l = 0; l < 4; l++) h = ((h ^ lane[l]) * k) ^ (h >> 29);
            return h ? h : 1; // 0 means "never uploaded"
        }

        // Row-major guest blocks (already byte-swapped) -> the upload format (see Convert)
        void convertBlocks(const uint8_t* linear, uint8_t* dst, size_t blocks, const TextureFormat& tf)
        {
            if (tf.convert == Convert::None)
            {
                std::memcpy(dst, linear, blocks * tf.bytes);
                return;
            }
            for (size_t i = 0; i < blocks; i++)
            {
                if (tf.convert == Convert::FromDepth24)
                {
                    // XeUNorm24To32: (n + (n >> 23)) / 2^24, exact, unlike / (2^24 - 1)
                    uint32_t n = ((const uint32_t*)linear)[i] >> 8;
                    float f = float(n + (n >> 23)) * (1.0f / 16777216.0f);
                    std::memcpy(dst + i * 4, &f, 4);
                    continue;
                }
                uint16_t v = ((const uint16_t*)linear)[i];
                uint8_t* o = dst + i * 4;
                if (tf.convert == Convert::From4444)
                    for (int c = 0; c < 4; c++) o[c] = uint8_t(((v >> (4 * c)) & 0xF) * 17);
                else if (tf.convert == Convert::From1555)
                {
                    for (int c = 0; c < 3; c++) o[c] = uint8_t(((v >> (5 * c)) & 0x1F) * 255 / 31);
                    o[3] = (v >> 15) ? 255 : 0;
                }
                else
                {
                    o[0] = uint8_t((v & 0x1F) * 255 / 31);
                    o[1] = uint8_t(((v >> 5) & 0x3F) * 255 / 63);
                    o[2] = uint8_t((v >> 11) * 255 / 31);
                    o[3] = 255;
                }
            }
        }

        // ---------------------------------------------------------------- depth resolves read as color

        // Depth resolve destination -> its stencil plane (R8_UINT, same size), and a counter of
        // resolves into it, so the packed view below is rebuilt only after a new resolve.
        std::map<VkImage, Image> g_stencilOf;
        std::map<VkImage, uint64_t> g_depthGeneration;

        Image& stencilCompanion(const Image& dest)
        {
            Image& st = g_stencilOf[dest.image];
            if (!st.image)
                st = createImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8_UINT, dest.width, dest.height, 1, 1, 1,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            return st;
        }

        // A texture can read a depth resolve as 8_8_8_8 (issue #60): the guest's resolve wrote the
        // raw D24S8 words, depth in the upper 24 bits and stencil in the low byte, and the player
        // highlight looks for the selected player's stencil value there (its first pass samples the
        // scene's depth resolve that way and edge-detects the stencil). A compute pass packs the
        // R32_SFLOAT depth and R8_UINT stencil planes back into those bytes, as R8G8B8A8_UNORM with
        // component X = the low byte (stencil), as the texture upload would decode the words.
        constexpr char kPackDepthCs[] = R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D depthPlane;
layout(set = 0, binding = 1) uniform usampler2D stencilPlane;
layout(set = 0, binding = 2, rgba8) uniform writeonly image2D packed;
void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(p, imageSize(packed)))) return;
    uint d = uint(clamp(texelFetch(depthPlane, p, 0).r, 0.0, 1.0) * 16777215.0 + 0.5);
    uint st = texelFetch(stencilPlane, p, 0).r;
    imageStore(packed, p, vec4(float(st & 255u), float(d & 255u), float((d >> 8) & 255u), float(d >> 16)) / 255.0);
}
)";
        struct PackedDepth { Image img; uint64_t generation = 0; };
        std::map<VkImage, PackedDepth> g_packedDepth;
        VkDescriptorSetLayout g_packSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout g_packLayout = VK_NULL_HANDLE;
        VkPipeline g_packPipeline = VK_NULL_HANDLE;
        VkSampler g_packSampler = VK_NULL_HANDLE;

        // The packed RGBA8 image for depth resolve `dest`, current as of its latest resolve;
        // nullptr if the depth resolve has no stencil plane (never resolved through resolveDepth)
        Image* packedDepth(Image& dest)
        {
            auto st = g_stencilOf.find(dest.image);
            if (st == g_stencilOf.end()) return nullptr;
            if (!g_packPipeline)
            {
                VkDescriptorSetLayoutBinding b[3] = {
                    { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                    { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                    { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
                VkDescriptorSetLayoutCreateInfo dl{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
                dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
                dl.bindingCount = 3;
                dl.pBindings = b;
                VK_CHECK(vkCreateDescriptorSetLayout(D, &dl, nullptr, &g_packSetLayout));
                VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
                pli.setLayoutCount = 1;
                pli.pSetLayouts = &g_packSetLayout;
                VK_CHECK(vkCreatePipelineLayout(D, &pli, nullptr, &g_packLayout));
                std::string error;
                VkShaderModule cs = compile(kPackDepthCs, shaderc_compute_shader, "pack_depth.comp", error);
                if (!cs) fatal("[render] depth packing shader: %s", error.c_str());
                VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
                ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main", nullptr };
                ci.layout = g_packLayout;
                VK_CHECK(vkCreateComputePipelines(D, VK_NULL_HANDLE, 1, &ci, nullptr, &g_packPipeline));
                vkDestroyShaderModule(D, cs, nullptr);
                VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
                VK_CHECK(vkCreateSampler(D, &si, nullptr, &g_packSampler));
            }
            PackedDepth& pd = g_packedDepth[dest.image];
            if (!pd.img.image)
                pd.img = createImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, dest.width, dest.height, 1, 1, 1,
                                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            uint64_t gen = g_depthGeneration[dest.image];
            if (pd.generation != gen)
            {
                endPass();
                VkCommandBuffer c = cmd();
                transition(c, dest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                transition(c, st->second, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                transition(c, pd.img, VK_IMAGE_LAYOUT_GENERAL);
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, g_packPipeline);
                VkDescriptorImageInfo ii[3] = { { g_packSampler, dest.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                                { g_packSampler, st->second.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                                { VK_NULL_HANDLE, pd.img.view, VK_IMAGE_LAYOUT_GENERAL } };
                VkWriteDescriptorSet w[3]{};
                for (int i = 0; i < 3; i++)
                {
                    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    w[i].dstBinding = i;
                    w[i].descriptorCount = 1;
                    w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    w[i].pImageInfo = &ii[i];
                }
                C->cmdPushDescriptorSet(c, VK_PIPELINE_BIND_POINT_COMPUTE, g_packLayout, 0, 3, w);
                vkCmdDispatch(c, (dest.width + 7) / 8, (dest.height + 7) / 8, 1);
                transition(c, pd.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                pd.generation = gen;
            }
            return &pd.img;
        }

        // ---------------------------------------------------------------- stale resolve images

        // A texture at a resolve destination samples the resolve image (render to texture). But the
        // guest can free that memory and load something else there: in a match, the team-colored
        // jersey font sheet is rendered to an address that the in-game Controls screen later loads
        // its controller picture into, and the picture showed the font sheet (issue #63). So each
        // resolve image keeps a fingerprint of its guest memory as of its last resolve (after its
        // writeback lands, if one is pending), and a texture lookup that finds the memory changed
        // since drops the image and reads the guest's data instead. The fingerprint is 256 bytes at
        // the start of each of the first 16 pages, leaving out a last page another allocation may
        // share: cheap, and any texture load over it changes it.
        uint32_t formatBytes(VkFormat f);

        bool memRange(const Image& img, uint32_t& start, uint32_t& end)
        {
            if (!img.guestPitch) return false;
            uint32_t bpp = formatBytes(img.format);
            uint32_t w = std::min(img.width / g_scale, img.guestPitch);
            uint32_t h = img.guestHeight ? std::min(img.height / g_scale, img.guestHeight) : img.height / g_scale;
            if (!bpp || !w || !h) return false;
            start = img.guestBase;
            uint64_t e = uint64_t(start) + tiledUpperBound2D(w, h, tiledBlocksAligned(img.guestPitch), bpp);
            if (e > mem::kPhysicalSize) return false;
            end = uint32_t(e);
            return true;
        }

        uint64_t memSignature(uint32_t start, uint32_t end)
        {
            uint64_t h = 0x5EED;
            uint32_t lastPage = (end - 1) & ~0xFFFu;
            int pages = 0;
            for (uint32_t p = start; p < end && pages < 16; p = (p & ~0xFFFu) + 0x1000, pages++)
            {
                if ((p & ~0xFFFu) == lastPage && pages > 0) break;
                h = hashBytes(mem::physical(p), std::min<uint32_t>(256, end - p), h);
            }
            return h;
        }

        void retireResolved(std::map<uint32_t, Image>::iterator it)
        {
            g_retired.push_back(it->second);
            if (auto st = g_stencilOf.find(it->second.image); st != g_stencilOf.end())
            {
                g_retired.push_back(st->second);
                g_stencilOf.erase(st);
            }
            if (auto pd = g_packedDepth.find(it->second.image); pd != g_packedDepth.end())
            {
                g_retired.push_back(pd->second.img);
                g_packedDepth.erase(pd);
            }
            g_depthGeneration.erase(it->second.image);
            g_resolved.erase(it);
        }

        // The resolve image at `base`, or end() if there is none or the guest has rewritten its
        // memory since (checked at most once per frame)
        std::map<uint32_t, Image>::iterator findResolved(uint32_t base)
        {
            auto it = g_resolved.find(base);
            if (it == g_resolved.end()) return it;
            Image& img = it->second;
            if (img.memSigPending || img.memSigFrame == g_frameSerial) return it;
            img.memSigFrame = g_frameSerial;
            uint32_t start, end;
            if (!memRange(img, start, end) || memSignature(start, end) == img.memSig) return it;
            static std::set<uint32_t> logged;
            if (logged.insert(base).second)
                info("[render] resolve %08X was overwritten by the guest since: sampling its memory instead", base);
            retireResolved(it);
            return g_resolved.end();
        }

        std::map<std::pair<VkImage, uint32_t>, VkImageView> g_resolvedViews; // resolve images used as textures

        // Descriptor for texture fetch constant `slot` as a shader declaring dimension `dim`.
        // May record upload commands (ends the current pass).
        VkDescriptorImageInfo texture(uint32_t slot, uint32_t dim)
        {
            uint32_t d[6];
            for (int i = 0; i < 6; i++) d[i] = readRegister(kFetchConstants + slot * 6 + i);
            VkDescriptorImageInfo out{ sampler(d), placeholder(dim).view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            uint32_t format = d[1] & 0x3F, endian = (d[1] >> 6) & 3, base = d[1] & 0xFFFFF000, fetchDim = (d[5] >> 9) & 3;
            bool tiled = d[0] >> 31, cube = fetchDim == 3;
            uint32_t pitch = ((d[0] >> 22) & 0x1FF) * 32;
            uint32_t width = (d[2] & 0x1FFF) + 1, height = ((d[2] >> 13) & 0x1FFF) + 1;
            TextureFormat tf = textureFormat(format);
            uint32_t swizzle = hostSwizzle((d[3] >> 1) & 0xFFF, tf.swizzle);
            if ((d[0] & 3) != 2 || (fetchDim != 1 && fetchDim != 3) || dim != fetchDim)
            {
                warnOnce("texture dimension " + std::to_string(fetchDim) + " (shader " + std::to_string(dim) + ") not supported yet");
                return out;
            }

            // Render to texture: a 2D texture at a resolve destination samples the resolved image
            // directly when the formats are compatible (resolves are not written to guest memory yet)
            auto resolved = findResolved(base & 0x1FFFFFFF);
            if (!cube && format == 6 && resolved != g_resolved.end() && resolved->second.format == VK_FORMAT_R32_SFLOAT &&
                width * g_scale <= resolved->second.width && height * g_scale <= resolved->second.height)
                if (Image* packed = packedDepth(resolved->second))
                {
                    VkImageView& view = g_resolvedViews[{ packed->image, swizzle }];
                    if (!view)
                    {
                        view = createView(packed->image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 1, swizzle, false);
                        info("[render] depth resolve %08X read as 8_8_8_8: raw D24S8 words (stencil in the low byte)", base & 0x1FFFFFFF);
                    }
                    out.imageView = view;
                    return out;
                }
            if (!cube && resolved != g_resolved.end() && resolveCompatible(format, resolved->second.format) &&
                width * g_scale <= resolved->second.width && height * g_scale <= resolved->second.height)
            {
                Image& r = resolved->second;
                VkImageView& view = g_resolvedViews[{ r.image, swizzle | (r.swapRB ? 0x1000u : 0u) }];
                if (!view)
                {
                    view = createView(r.image, VK_IMAGE_VIEW_TYPE_2D, r.format, 1, swizzle, r.swapRB);
                    info("[render] render to texture: resolve %08X sampled as a %ux%u texture (format %u)%s", base & 0x1FFFFFFF, width,
                         height, format, r.swapRB ? " (red/blue swapped)" : "");
                }
                endPass();
                transition(cmd(), r, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                out.imageView = view;
                return out;
            }

            if (tf.vk == VK_FORMAT_UNDEFINED || !pitch)
            {
                warnOnce("texture format " + std::to_string(format) + (tiled ? " (tiled)" : " (linear)") +
                         " not supported yet: magenta placeholder");
                return out;
            }
            uint32_t layers = cube ? 6 : 1;
            // Render to cube map: all six faces resolved to their slots (face stride = 4 KB-aligned
            // 32-block-aligned face size) -> blit the resolved images into one cube image
            if (cube && format == 6 && tiled)
            {
                uint32_t faceStride = (tiledBlocksAligned(std::max(pitch, width)) * tiledBlocksAligned(height) * 4 + 4095) & ~4095u;
                Image* faces[6]{};
                for (uint32_t f = 0; f < 6; f++)
                {
                    auto it = findResolved((base + f * faceStride) & 0x1FFFFFFF);
                    if (it == g_resolved.end() || it->second.format != VK_FORMAT_R8G8B8A8_UNORM || it->second.width < width * g_scale ||
                        it->second.height < height * g_scale)
                        break;
                    faces[f] = &it->second;
                }
                if (faces[5])
                {
                    TextureKey ckey{ base, format | 0x2000u, endian, width, height, pitch };
                    Texture& t = g_textures[ckey];
                    if (!t.img.image)
                    {
                        t.img = createImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, VK_FORMAT_R8G8B8A8_UNORM, width * g_scale, height * g_scale,
                                            1, 6, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                            VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
                        info("[render] render to cube map: 6 resolves from %08X (face stride %X) sampled as a %ux%u cube", base & 0x1FFFFFFF,
                             faceStride, width, height);
                    }
                    if (t.uploadedFrame != g_frameSerial)
                    {
                        endPass();
                        VkCommandBuffer c = cmd();
                        transition(c, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        for (uint32_t f = 0; f < 6; f++)
                        {
                            transition(c, *faces[f], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                            VkImageBlit b{};
                            b.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                            b.srcOffsets[1] = { int32_t(width * g_scale), int32_t(height * g_scale), 1 };
                            b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, f, 1 };
                            b.dstOffsets[1] = { int32_t(width * g_scale), int32_t(height * g_scale), 1 };
                            vkCmdBlitImage(c, faces[f]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           1, &b, VK_FILTER_NEAREST);
                        }
                        transition(c, t.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        t.uploadedFrame = g_frameSerial;
                    }
                    VkImageView& view = t.views[swizzle | (faces[0]->swapRB ? 0x1000u : 0u)];
                    if (!view) view = createView(t.img.image, VK_IMAGE_VIEW_TYPE_CUBE, VK_FORMAT_R8G8B8A8_UNORM, 6, swizzle, faces[0]->swapRB);
                    out.imageView = view;
                    return out;
                }
            }
            // Mip levels (fetch constant: mip address in dword 5, packed mips bit, level range in dword 4)
            uint32_t mipAddress = d[5] & 0xFFFFF000, packed = (d[5] >> 11) & 1;
            uint32_t levels = 1;
            if (mipAddress)
            {
                uint32_t sizeLevels = 31 - __builtin_clz(std::max(width, height)); // log2_floor of the longer side
                levels = std::min((d[4] >> 6) & 0xF, sizeLevels) + 1;
            }
            TextureKey key{ base, format | (tiled ? 0x100u : 0u) | (fetchDim << 9), endian, width, height, pitch,
                            levels > 1 ? (mipAddress | (packed << 1) | ((levels - 1) << 5)) : 0u };
            Texture& t = g_textures[key];
            if (!t.img.image)
                t.img = createImage(VK_IMAGE_TYPE_2D, cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D, tf.vk, width, height, 1, layers, 1,
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                                    levels);
            std::vector<GuestRegion> regions;
            uint64_t hash = 0;
            if (t.uploadedFrame != g_frameSerial)
            {
                regions = mipRegions(width, height, pitch, tiled, { tf.blockW, tf.blockH, tf.bytes }, levels, packed, layers);
                double hashT0 = nowMs();
                // Unchanged since the last upload (same bytes at every level and face): keep the image
                for (const GuestRegion& r : regions)
                    for (uint32_t layer = 0; layer < layers; layer++)
                    {
                        uint64_t at = uint64_t(r.mip ? mipAddress : base) + r.offset + uint64_t(layer) * r.sliceStride;
                        if (!g_pendingWrites.empty()) needsExportedData(at, at + r.srcBytes);
                        if (at + r.srcBytes <= mem::kPhysicalSize) hash = hashBytes(mem::physical(uint32_t(at)), r.srcBytes, hash);
                        g_perf.hashBytes += r.srcBytes;
                    }
                g_perf.hashMs += nowMs() - hashT0;
                if (hash && hash == t.hash)
                {
                    t.uploadedFrame = g_frameSerial;
                    g_texturesKept++;
                }
            }
            if (t.uploadedFrame != g_frameSerial)
            {
                // Guest regions -> row-major blocks (byte-swapped, converted), one after the other in
                // the upload buffer; each level is a rectangle of its region's rows
                VkDeviceSize total = 0;
                uint32_t outBytes = tf.convert == Convert::None ? tf.bytes : 4;
                for (const GuestRegion& r : regions)
                {
                    uint64_t end = uint64_t(r.mip ? mipAddress : base) + r.offset + uint64_t(r.sliceStride) * (layers - 1) + r.srcBytes;
                    if (end > mem::kPhysicalSize)
                    {
                        warnOnce("texture extends past physical memory: placeholder");
                        return out;
                    }
                    total = (total + 15) & ~VkDeviceSize(15);
                    total += VkDeviceSize(r.rowBlocks) * r.yBlocks * outBytes * layers;
                }
                VkDeviceSize off = uploadAlloc(total);
                if (off == UINT64_MAX) return out;
                static std::vector<uint8_t> swapped, rows;
                std::vector<VkBufferImageCopy> copies;
                VkDeviceSize at = off;
                for (const GuestRegion& r : regions)
                {
                    at = (at + 15) & ~VkDeviceSize(15);
                    VkDeviceSize layerSize = VkDeviceSize(r.rowBlocks) * r.yBlocks * outBytes;
                    for (uint32_t layer = 0; layer < layers; layer++)
                    {
                        swapped.resize(r.srcBytes);
                        const uint32_t* src = (const uint32_t*)mem::physical((r.mip ? mipAddress : base) + r.offset + layer * r.sliceStride);
                        for (uint32_t i = 0; i < r.srcBytes / 4; i++) ((uint32_t*)swapped.data())[i] = gpuSwap(src[i], endian);
                        const uint8_t* linear = swapped.data();
                        if (tiled)
                        {
                            rows.assign(size_t(r.rowBlocks) * r.yBlocks * tf.bytes, 0);
                            untile2D(swapped.data(), rows.data(), r.xBlocks, r.yBlocks, r.rowBlocks, tf.bytes);
                            linear = rows.data();
                        }
                        else if (size_t(r.rowBlocks) * r.yBlocks * tf.bytes > r.srcBytes)
                        {
                            // Linear: the last row may end before the pitch does
                            rows.assign(size_t(r.rowBlocks) * r.yBlocks * tf.bytes, 0);
                            std::memcpy(rows.data(), swapped.data(), r.srcBytes);
                            linear = rows.data();
                        }
                        convertBlocks(linear, frame().uploadMap + at + layer * layerSize, size_t(r.rowBlocks) * r.yBlocks, tf);
                        for (const GuestRegion::Level& l : r.levels)
                        {
                            VkBufferImageCopy c{};
                            c.bufferOffset = at + layer * layerSize + (VkDeviceSize(l.yBlock) * r.rowBlocks + l.xBlock) * outBytes;
                            c.bufferRowLength = r.rowBlocks * tf.blockW;
                            c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l.level, layer, 1 };
                            c.imageExtent = { std::max(width >> l.level, 1u), std::max(height >> l.level, 1u), 1 };
                            copies.push_back(c);
                        }
                    }
                    at += layerSize * layers;
                }
                endPass();
                VkCommandBuffer c = cmd();
                transition(c, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                vkCmdCopyBufferToImage(c, frame().upload, t.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()), copies.data());
                transition(c, t.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                t.uploadedFrame = g_frameSerial;
                t.hash = hash;
                g_texturesUploaded++;
                g_perf.uploads++;
                g_perf.uploadBytes += at - off; // `at` ran from the allocation's offset to its end
                {
                    PerfStats::Up& u = g_perf.uploadsBy[base];
                    u.format = format; u.width = width; u.height = height;
                    u.count++; u.bytes += at - off;
                    if (!t.everUploaded) u.fresh++; // first upload of this key, not a re-upload of changed data
                }
                t.everUploaded = true;
            }
            VkImageView& view = t.views[swizzle];
            if (!view) view = createView(t.img.image, cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D, tf.vk, layers, swizzle, false);
            out.imageView = view;
            return out;
        }

        // ---------------------------------------------------------------- pipelines

        VkDescriptorSetLayout g_setLayouts[2];
        VkPipelineLayout g_layout = VK_NULL_HANDLE;
        // Pipelines by everything that goes into one, as a fixed array of fields: formatting a text key
        // with snprintf on every draw was 7.5% of the command processor thread (#73)
        struct PipelineKey
        {
            std::array<uint64_t, 64> v{};
            bool operator==(const PipelineKey&) const = default;
        };
        struct PipelineKeyHash
        {
            size_t operator()(const PipelineKey& k) const
            {
                uint64_t h = 0x9E3779B97F4A7C15ull;
                for (uint64_t x : k.v) { h = (h ^ x) * 0x100000001B3ull; h ^= h >> 29; }
                return size_t(h);
            }
        };
        std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> g_pipelines;

        VkBlendFactor blendFactor(uint32_t f)
        {
            switch (f)
            {
            case 0: return VK_BLEND_FACTOR_ZERO;
            case 4: return VK_BLEND_FACTOR_SRC_COLOR;
            case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
            case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case 8: return VK_BLEND_FACTOR_DST_COLOR;
            case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case 10: return VK_BLEND_FACTOR_DST_ALPHA;
            case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
            case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
            case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
            case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
            case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
            default: return VK_BLEND_FACTOR_ONE;
            }
        }

        VkBlendOp blendOp(uint32_t op)
        {
            static const VkBlendOp ops[5] = { VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX, VK_BLEND_OP_REVERSE_SUBTRACT };
            return op < 5 ? ops[op] : VK_BLEND_OP_ADD;
        }

        // xenos StencilOp and CompareFunction use Vulkan's order
        VkStencilOpState stencilState(const StencilFace& f)
        {
            return { VkStencilOp(f.fail), VkStencilOp(f.zpass), VkStencilOp(f.zfail), VkCompareOp(f.func), f.mask, f.writeMask, f.ref };
        }

        // A pixel shader writes the render targets Xenos gave it; a draw into a pass with no image
        // in one of those slots needs the same shader without the outputs nobody reads. Which slots
        // have an image, not how many the pass carries, is what decides: a hole in the middle drops
        // that one output and keeps the ones after it.
        VkShaderModule psModule(Shader* ps, uint32_t mask)
        {
            mask &= 15;
            if (!(ps->t.colorOutputs & ~mask)) return ps->module;
            VkShaderModule& m = ps->colorVariant[mask];
            if (!m)
            {
                char vname[64];
                snprintf(vname, sizeof(vname), "%s_m%x", ps->name.c_str(), mask);
                std::string error;
                m = compile(ps->t.glsl, shaderc_fragment_shader, vname, error, mask);
                if (!m) warnOnce(std::string(vname) + " failed to compile: " + error);
            }
            return m ? m : ps->module;
        }

        struct PipelineDesc
        {
            const DrawState* s;
            Shader *vs, *ps;
            VkPrimitiveTopology topology;
            bool rectangles;
            VkFormat colors[4];
            bool depth;
            uint32_t samples;
            bool discard = false; // rasterizer discard: a memory export draw with no render targets
        };

        VkPipeline pipeline(const PipelineDesc& p)
        {
            const DrawState& s = *p.s;
            bool strip = p.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || p.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                         p.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
            bool restart = strip && s.primitiveRestart;
            bool cullable = !p.rectangles && p.topology >= VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            PipelineKey key;
            {
                uint64_t* o = key.v.data();
                for (uint64_t x : { uint64_t(p.discard), uint64_t(uintptr_t(p.vs->module)), uint64_t(uintptr_t(p.ps->module)), uint64_t(p.topology),
                                    uint64_t(p.rectangles), uint64_t(restart), uint64_t(p.colors[0]), uint64_t(p.colors[1]), uint64_t(p.colors[2]),
                                    uint64_t(p.colors[3]), uint64_t(p.depth), uint64_t(p.samples) })
                    *o++ = x;
                for (int i = 0; i < 4; i++)
                {
                    const ColorTarget& t = s.color[i];
                    for (uint64_t x : { uint64_t(p.colors[i] ? t.writeMask : 0), uint64_t(t.blend.colorSrc), uint64_t(t.blend.colorDst),
                                        uint64_t(t.blend.colorOp), uint64_t(t.blend.alphaSrc), uint64_t(t.blend.alphaDst), uint64_t(t.blend.alphaOp) })
                        *o++ = x;
                }
                for (uint64_t x : { uint64_t(p.depth && s.depthTest), uint64_t(p.depth && s.depthWrite), uint64_t(s.depthFunc),
                                    uint64_t(p.depth && s.stencil), uint64_t(s.stencilBackface), uint64_t(s.front.func), uint64_t(s.front.fail),
                                    uint64_t(s.front.zpass), uint64_t(s.front.zfail), uint64_t(s.back.func), uint64_t(s.back.fail),
                                    uint64_t(s.back.zpass), uint64_t(s.back.zfail), uint64_t(cullable && s.cullFront), uint64_t(cullable && s.cullBack),
                                    uint64_t(s.frontFaceCW), uint64_t(s.polyMode == 1 && s.polyFront == 1),
                                    uint64_t(s.polyOffsetFront || s.polyOffsetBack), uint64_t(s.clipDisable), uint64_t(s.alphaToMask) })
                    *o++ = x;
            }
            auto it = g_pipelines.find(key);
            if (it != g_pipelines.end()) return it->second;

            VkPipelineShaderStageCreateInfo stages[3]{};
            uint32_t stageCount = 0;
            auto stage = [&](VkShaderStageFlagBits bit, VkShaderModule m) {
                stages[stageCount] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
                stages[stageCount].stage = bit;
                stages[stageCount].module = m;
                stages[stageCount].pName = "main";
                stageCount++;
            };
            stage(VK_SHADER_STAGE_VERTEX_BIT, p.vs->module);
            if (p.rectangles) stage(VK_SHADER_STAGE_GEOMETRY_BIT, g_rectGs);
            // Rasterizer discard (a memory export draw with no render targets) means no fragment
            // shader state is required, and the spec forbids a fragment stage then anyway
            // (VUID-VkGraphicsPipelineCreateInfo-pStages-06894). The export is done by the vertex
            // shader, which still runs, so nothing here would have been read.
            if (!p.discard)
                stage(VK_SHADER_STAGE_FRAGMENT_BIT, psModule(p.ps, colorMask(p.colors)));

            VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO }; // vertices fetched in the shader
            VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            ia.topology = p.topology;
            ia.primitiveRestartEnable = restart;
            VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            vp.viewportCount = 1;
            vp.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            rs.depthClampEnable = s.clipDisable;
            rs.rasterizerDiscardEnable = p.discard;
            rs.polygonMode = s.polyMode == 1 && s.polyFront == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
            rs.cullMode = (cullable && s.cullFront ? VK_CULL_MODE_FRONT_BIT : 0) | (cullable && s.cullBack ? VK_CULL_MODE_BACK_BIT : 0);
            rs.frontFace = s.frontFaceCW ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.depthBiasEnable = s.polyOffsetFront || s.polyOffsetBack;
            rs.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            ms.rasterizationSamples = VkSampleCountFlagBits(p.samples);
            ms.alphaToCoverageEnable = s.alphaToMask;
            VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
            ds.depthTestEnable = p.depth && s.depthTest;
            ds.depthWriteEnable = p.depth && s.depthWrite;
            ds.depthCompareOp = VkCompareOp(s.depthFunc);
            ds.stencilTestEnable = p.depth && s.stencil;
            ds.front = stencilState(s.front);
            ds.back = stencilState(s.stencilBackface ? s.back : s.front);
            VkPipelineColorBlendAttachmentState att[4]{};
            for (int i = 0; i < 4; i++)
            {
                const ColorTarget& t = s.color[i];
                att[i].colorWriteMask = p.colors[i] ? t.writeMask : 0;
                att[i].blendEnable = p.colors[i] && t.blend.enabled();
                att[i].srcColorBlendFactor = blendFactor(t.blend.colorSrc);
                att[i].dstColorBlendFactor = blendFactor(t.blend.colorDst);
                att[i].colorBlendOp = blendOp(t.blend.colorOp);
                att[i].srcAlphaBlendFactor = blendFactor(t.blend.alphaSrc);
                att[i].dstAlphaBlendFactor = blendFactor(t.blend.alphaDst);
                att[i].alphaBlendOp = blendOp(t.blend.alphaOp);
            }
            VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            uint32_t colorCount = colorSlots(p.colors); // must match the pass these draws go in
            cb.attachmentCount = colorCount;
            cb.pAttachments = colorCount ? att : nullptr;
            static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                                  VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                                  VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BIAS };
            VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dy.dynamicStateCount = uint32_t(std::size(dyn));
            dy.pDynamicStates = dyn;
            VkPipelineRenderingCreateInfo rci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
            rci.colorAttachmentCount = colorCount;
            rci.pColorAttachmentFormats = colorCount ? p.colors : nullptr;
            rci.depthAttachmentFormat = p.depth ? kDepthFormat : VK_FORMAT_UNDEFINED;
            rci.stencilAttachmentFormat = rci.depthAttachmentFormat;

            VkGraphicsPipelineCreateInfo gci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            gci.pNext = &rci;
            gci.stageCount = stageCount;
            gci.pStages = stages;
            gci.pVertexInputState = &vi;
            gci.pInputAssemblyState = &ia;
            gci.pViewportState = &vp;
            gci.pRasterizationState = &rs;
            gci.pMultisampleState = &ms;
            gci.pDepthStencilState = &ds;
            gci.pColorBlendState = &cb;
            gci.pDynamicState = &dy;
            gci.layout = g_layout;
            VkPipeline pl = VK_NULL_HANDLE;
            double t0 = nowMs();
            VK_CHECK(vkCreateGraphicsPipelines(D, VK_NULL_HANDLE, 1, &gci, nullptr, &pl));
            g_compileStats.pipelines++;
            g_compileStats.pipelineMs += nowMs() - t0;
            info("[render] pipeline %zu %p: %s", g_pipelines.size() + 1, (void*)pl, pipelineKey(s).c_str());
            return g_pipelines[key] = pl;
        }

        // ---------------------------------------------------------------- setup

        // Import guest physical memory (a host mapping) as a Vulkan buffer, so memory export can
        // write it directly: after the GPU finishes, the data is where the guest expects it.
        void createExportBuffer()
        {
            VkDeviceSize size = mem::kPhysicalSize;
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = size;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT; // vertex fetches of pending writes
            VK_CHECK(vkCreateBuffer(D, &bci, nullptr, &g_guestMem));
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(D, g_guestMem, &req);
            uint32_t type = C->findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                                  VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            if (type == UINT32_MAX) type = C->findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = type;
            if (type == UINT32_MAX || vkAllocateMemory(D, &mai, nullptr, &g_guestMemMemory) != VK_SUCCESS)
            {
                warn("[render] no memory for the memory export buffer: memory export draws are skipped");
                vkDestroyBuffer(D, g_guestMem, nullptr);
                g_guestMem = VK_NULL_HANDLE;
                return;
            }
            VK_CHECK(vkBindBufferMemory(D, g_guestMem, g_guestMemMemory, 0));
            VK_CHECK(vkMapMemory(D, g_guestMemMemory, 0, size, 0, (void**)&g_exportMap));
            g_memExportReady = true;
            info("[render] memory export: %llu MB staging buffer (stream ranges copied in and out per draw)", (unsigned long long)(size >> 20));
        }

        void importGuestMemory()
        {
            void* ptr = mem::physical(0);
            VkDeviceSize size = mem::kPhysicalSize;
            if (!C->hostMemoryImport || !C->getMemoryHostPointerProperties || (uintptr_t(ptr) % std::max(1u, C->hostPointerAlignment)))
            {
                if (!C->hostMemoryImport) { warn("[render] no vertex shader stores: memory export draws are skipped"); return; }
                createExportBuffer();
                return;
            }
            VkMemoryHostPointerPropertiesEXT hp{ VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
            if (C->getMemoryHostPointerProperties(D, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr, &hp) != VK_SUCCESS)
            {
                createExportBuffer();
                return;
            }
            VkExternalMemoryBufferCreateInfo ext{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
            ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.pNext = &ext;
            bci.size = size;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT; // vertex fetches of pending writes
            VK_CHECK(vkCreateBuffer(D, &bci, nullptr, &g_guestMem));
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(D, g_guestMem, &req);
            uint32_t bits = req.memoryTypeBits & hp.memoryTypeBits, type = bits ? uint32_t(__builtin_ctz(bits)) : UINT32_MAX;
            VkImportMemoryHostPointerInfoEXT imp{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT };
            imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            imp.pHostPointer = ptr;
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.pNext = &imp;
            mai.allocationSize = size;
            mai.memoryTypeIndex = type;
            VkResult allocResult = VK_ERROR_UNKNOWN, bindResult = VK_ERROR_UNKNOWN;
            if (type == UINT32_MAX || (allocResult = vkAllocateMemory(D, &mai, nullptr, &g_guestMemMemory)) != VK_SUCCESS ||
                (bindResult = vkBindBufferMemory(D, g_guestMem, g_guestMemMemory, 0)) != VK_SUCCESS)
            {
                info("[render] guest memory not importable here (allocate %d, bind %d): using a staging buffer for memory export",
                     int(allocResult), int(bindResult));
                vkDestroyBuffer(D, g_guestMem, nullptr);
                g_guestMem = VK_NULL_HANDLE;
                createExportBuffer();
                return;
            }
            g_memExportReady = true;
            info("[render] guest physical memory imported (%llu MB) for memory export", (unsigned long long)(size >> 20));
        }

        // Submit what is recorded and wait for it, then keep recording in the same frame context
        // (memory export results must be in guest memory before later draws copy vertex data
        // from it or the CPU reads it)
        void flushAndWait()
        {
            endPass();
            Frame& f = frame();
            if (!f.recording && !f.chunksInFlight) return;
            if (f.recording)
            {
                VK_CHECK(vkEndCommandBuffer(f.cmd));
                f.recording = false;
                queueSubmit(f.cmd, f.fence);
                double w0 = nowMs();
                VK_CHECK(vkWaitForFences(D, 1, &f.fence, VK_TRUE, UINT64_MAX));
                g_perf.waitMs += nowMs() - w0;
                VK_CHECK(vkResetFences(D, 1, &f.fence));
            }
            waitChunks(f); // the early submits before it
            g_perf.flushes++;
            f.submitted = false;
            g_drawsSinceSubmit = 0;
            restartFrame(f); // everything recorded so far has executed
        }

        bool ready()
        {
            if (g_disabled || g_failed) return false;
            if (D) return true;
            C = vkctx::get();
            if (!C) { g_failed = true; return false; }
            D = C->device;
            importGuestMemory();
            if (C->timelineSemaphore)
            {
                VkSemaphoreTypeCreateInfo tci{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
                tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
                VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
                sci.pNext = &tci;
                VK_CHECK(vkCreateSemaphore(D, &sci, nullptr, &g_timeline));
            }
            else
                info("[render] no timeline semaphores: the game's GPU fences make the command processor wait");

            g_compiler = shaderc_compiler_initialize();
            g_options = shaderc_compile_options_initialize();
            g_targetEnv = C->demoteHelper ? shaderc_env_version_vulkan_1_3 : shaderc_env_version_vulkan_1_2;
            shaderc_compile_options_set_target_env(g_options, shaderc_target_env_vulkan, g_targetEnv);
            if (!C->demoteHelper)
                info("[render] GPU lacks shaderDemoteToHelperInvocation; shaders target Vulkan 1.2");
            shaderc_compile_options_set_optimization_level(g_options, shaderc_optimization_level_performance);
            std::string error;
            g_rectGs = compile(kRectangleGs, shaderc_geometry_shader, "rectangle_list.geom", error);
            if (!g_rectGs) fatal("[render] rectangle list geometry shader: %s", error.c_str());

            // set 0: guest data + constants (one set per frame in flight, constants by dynamic
            // offset); set 1: 32 textures, pushed per draw (a layout may have one push set only)
            // binding 4: guest physical memory, written by memory export
            VkDescriptorSetLayoutBinding b0[5]{};
            for (uint32_t i = 0; i < 5; i++)
                b0[i] = { i, (i == 0 || i == 4) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
                          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
            VkDescriptorSetLayoutBinding b1[32]{};
            for (uint32_t i = 0; i < 32; i++)
                b1[i] = { i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
            VkDescriptorSetLayoutCreateInfo dl{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dl.bindingCount = 5;
            dl.pBindings = b0;
            VK_CHECK(vkCreateDescriptorSetLayout(D, &dl, nullptr, &g_setLayouts[0]));
            dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
            dl.bindingCount = 32;
            dl.pBindings = b1;
            VK_CHECK(vkCreateDescriptorSetLayout(D, &dl, nullptr, &g_setLayouts[1]));
            VkPushConstantRange pcr{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                     sizeof(SystemConstants) };
            VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pli.setLayoutCount = 2;
            pli.pSetLayouts = g_setLayouts;
            pli.pushConstantRangeCount = 1;
            pli.pPushConstantRanges = &pcr;
            VK_CHECK(vkCreatePipelineLayout(D, &pli, nullptr, &g_layout));

            for (Frame& f : g_frames)
            {
                VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
                pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
                pci.queueFamilyIndex = C->queueFamily;
                VK_CHECK(vkCreateCommandPool(D, &pci, nullptr, &f.pool));
                VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                cai.commandPool = f.pool;
                cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                cai.commandBufferCount = 1;
                VK_CHECK(vkAllocateCommandBuffers(D, &cai, &f.cmd));
                f.cmdBuffers = { f.cmd };
                VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
                VK_CHECK(vkCreateFence(D, &fci, nullptr, &f.fence));
                VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                bci.size = kUploadSize;
                bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                VK_CHECK(vkCreateBuffer(D, &bci, nullptr, &f.upload));
                VkMemoryRequirements req;
                vkGetBufferMemoryRequirements(D, f.upload, &req);
                VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                mai.allocationSize = req.size;
                mai.memoryTypeIndex = C->findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                VK_CHECK(vkAllocateMemory(D, &mai, nullptr, &f.uploadMemory));
                VK_CHECK(vkBindBufferMemory(D, f.upload, f.uploadMemory, 0));
                VK_CHECK(vkMapMemory(D, f.uploadMemory, 0, kUploadSize, 0, (void**)&f.uploadMap));

                VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 } };
                VkDescriptorPoolCreateInfo dpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
                dpi.maxSets = 1;
                dpi.poolSizeCount = 2;
                dpi.pPoolSizes = sizes;
                VK_CHECK(vkCreateDescriptorPool(D, &dpi, nullptr, &f.descriptorPool));
                VkDescriptorSetAllocateInfo dai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                dai.descriptorPool = f.descriptorPool;
                dai.descriptorSetCount = 1;
                dai.pSetLayouts = &g_setLayouts[0];
                VK_CHECK(vkAllocateDescriptorSets(D, &dai, &f.buffers));
                VkDescriptorBufferInfo bufs[5] = { { f.upload, 0, kUploadSize }, { f.upload, 0, 4096 }, { f.upload, 0, 4096 }, { f.upload, 0, 928 },
                                                   g_memExportReady ? VkDescriptorBufferInfo{ g_guestMem, 0, mem::kPhysicalSize }
                                                                    : VkDescriptorBufferInfo{ f.upload, 0, 256 } };
                VkWriteDescriptorSet w[5]{};
                for (uint32_t i = 0; i < 5; i++)
                {
                    w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    w[i].dstSet = f.buffers;
                    w[i].dstBinding = i;
                    w[i].descriptorCount = 1;
                    w[i].descriptorType = (i == 0 || i == 4) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                    w[i].pBufferInfo = &bufs[i];
                }
                vkUpdateDescriptorSets(D, 5, w, 0, nullptr);
            }
            info("[render] Vulkan renderer ready");
            return true;
        }

        // ---------------------------------------------------------------- draws

        // a and b overlapping: their intersection; false if they do not overlap
        bool intersect(VkRect2D a, VkRect2D b, VkRect2D& out)
        {
            int32_t x0 = std::max(a.offset.x, b.offset.x), y0 = std::max(a.offset.y, b.offset.y);
            int32_t x1 = std::min(a.offset.x + int32_t(a.extent.width), b.offset.x + int32_t(b.extent.width));
            int32_t y1 = std::min(a.offset.y + int32_t(a.extent.height), b.offset.y + int32_t(b.extent.height));
            if (x1 <= x0 || y1 <= y0) return false;
            out = { { x0, y0 }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } };
            return true;
        }

        // The screen rectangle a draw's vertices span, in guest pixels, when that can be read on the
        // CPU: no clipping, no scissor (right and bottom at 8192 or beyond, Direct3D 9's "none"),
        // auto-indexed triangles, rectangles or quads, and a vertex shader that only moves a fetched
        // position into oPos (TranslatedShader::position). That is how the engine clears: a
        // rectangle of the target's size, but with nothing else bounding it the draw would own every
        // EDRAM tile from its base, and transfer them all in first. As Xenia's DrawExtentEstimator
        // (execute_unclipped_draw_vs_on_cpu), with the position math of the translated shaders.
        bool vertexBounds(const DrawState& s, const PositionFetch& p, VkRect2D& out)
        {
            bool area = s.primitive == 4 || s.primitive == 5 || s.primitive == 6 || s.primitive == 8 || s.primitive == 13 || s.primitive == 14;
            if (!p.valid || !s.clipDisable || !area || s.indexSource != 2 || !s.indexCount || s.indexCount > 64) return false;
            int32_t right = int32_t(s.scissorBR[0]), bottom = int32_t(s.scissorBR[1]);
            if (!s.windowOffsetDisabled) { right += s.windowOffsetX; bottom += s.windowOffsetY; }
            if (std::min(right, int32_t(s.screenScissorBR[0])) < 8192 || std::min(bottom, int32_t(s.screenScissorBR[1])) < 8192) return false;
            uint32_t d0 = readRegister(kFetchConstants + p.slot * 2), d1 = readRegister(kFetchConstants + p.slot * 2 + 1);
            uint64_t baseDw = d0 >> 2, sizeDw = (d1 >> 2) & 0xFFFFFF;
            uint32_t endian = d1 & 3;
            float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
            for (uint32_t i = 0; i < s.indexCount; i++)
            {
                int64_t at = (int64_t(i) + s.indexOffset) * p.stride + p.offset;
                if (at < 0 || (sizeDw && uint64_t(at) + 4 > sizeDw)) return false;
                const uint32_t* v = (const uint32_t*)mem::physical(uint32_t((baseDw + uint64_t(at)) * 4));
                auto comp = [&](uint32_t c) {
                    uint8_t k = p.comp[c];
                    return k < 4 ? std::bit_cast<float>(gpuSwap(v[k], endian)) : k == 5 ? 1.0f : 0.0f;
                };
                float x = comp(0), y = comp(1);
                if (!s.xyDividedByW) // as the translated shader: XY divided by W (W given as 1/W unless vtx_w0_fmt)
                {
                    if (p.comp[3] > 5) return false;
                    float w = s.wNotReciprocal ? comp(3) : 1.0f / comp(3);
                    if (!(w > 0.0f)) return false;
                    x /= w, y /= w;
                }
                x = x * s.vpScale[0] + s.vpOffset[0], y = y * s.vpScale[1] + s.vpOffset[1];
                if (!std::isfinite(x) || !std::isfinite(y)) return false;
                x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
            }
            // Pixel space as the host draws it (window offset, Direct3D's pixel centers at .0), one
            // pixel of margin on every side
            float ox = s.vertexWindowOffset ? float(s.windowOffsetX) : 0.0f, oy = s.vertexWindowOffset ? float(s.windowOffsetY) : 0.0f;
            if (!s.pixelCenterHalf) ox += 0.5f, oy += 0.5f;
            int32_t ix0 = int32_t(std::max(std::floor(x0 + ox) - 1.0f, 0.0f)), iy0 = int32_t(std::max(std::floor(y0 + oy) - 1.0f, 0.0f));
            int32_t ix1 = int32_t(std::min(std::ceil(x1 + ox) + 1.0f, 8192.0f)), iy1 = int32_t(std::min(std::ceil(y1 + oy) + 1.0f, 8192.0f));
            if (ix1 <= ix0 || iy1 <= iy0) return false;
            out = { { ix0, iy0 }, { uint32_t(ix1 - ix0), uint32_t(iy1 - iy0) } };
            return true;
        }

        // Window scissor (+ window offset) intersected with the screen scissor and `limit`
        bool scissorRect(const DrawState& s, VkExtent2D limit, VkRect2D& out)
        {
            int32_t x0 = s.scissorTL[0], y0 = s.scissorTL[1], x1 = s.scissorBR[0], y1 = s.scissorBR[1];
            if (!s.windowOffsetDisabled) { x0 += s.windowOffsetX; x1 += s.windowOffsetX; y0 += s.windowOffsetY; y1 += s.windowOffsetY; }
            x0 = std::max({ x0, int32_t(s.screenScissorTL[0]), 0 });
            y0 = std::max({ y0, int32_t(s.screenScissorTL[1]), 0 });
            x1 = std::min({ x1, int32_t(s.screenScissorBR[0]), int32_t(limit.width) });
            y1 = std::min({ y1, int32_t(s.screenScissorBR[1]), int32_t(limit.height) });
            if (x1 <= x0 || y1 <= y0) return false;
            out = { { x0, y0 }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } };
            return true;
        }

        // Destination image for a resolve to `destBase`: replaced when the size or format changes
        // (the old one is retired, the presenter may still show it)
        Image& resolveDestination(uint32_t destBase, uint32_t destPitch, uint32_t destHeight, uint32_t dw, uint32_t dh, VkFormat format)
        {
            auto it = g_resolved.find(destBase);
            if (it != g_resolved.end() && (it->second.width != dw * g_scale || it->second.height != dh * g_scale || it->second.format != format))
            {
                retireResolved(it);
                it = g_resolved.end();
            }
            if (it == g_resolved.end())
            {
                it = g_resolved.emplace(destBase, createImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, format, dw * g_scale, dh * g_scale, 1, 1, 1,
                                                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                                  VK_IMAGE_USAGE_SAMPLED_BIT)).first;
                info("[render] resolve destination %08X: %ux%u%s", destBase, dw, dh, format == VK_FORMAT_R32_SFLOAT ? " (depth)" : "");
            }
            it->second.resolvePitch = std::max(it->second.resolvePitch, dw);
            it->second.guestBase = destBase;
            it->second.guestPitch = destPitch;
            it->second.guestHeight = destHeight;
            return it->second;
        }

        uint32_t formatBytes(VkFormat f)
        {
            switch (f)
            {
            case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_R16G16_SNORM:
            case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R32_SFLOAT:
                return 4;
            case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT:
                return 8;
            default:
                return 0;
            }
        }

        // Predicated tiling: the game renders a large target in tiles (the match's 1152x640 2x MSAA
        // scene is two 576x640 tiles) and resolves each tile with RB_COPY_DEST_BASE moved to the
        // tile's place inside the same tiled texture. If `destBase` lies inside an existing resolve
        // destination of the same pitch and format, return that image and the tile's position in it:
        // the 32-aligned (x, y) whose tiled offset (Xenia's Tiled2D) is the address difference and
        // which keeps the `extent` inside the image.
        Image* resolveInto(uint32_t destBase, uint32_t destPitch, VkFormat format, VkExtent2D extent, VkOffset2D& offset)
        {
            uint32_t bytes = formatBytes(format);
            if (!bytes || !destPitch) return nullptr;
            auto it = g_resolved.lower_bound(destBase);
            for (int n = 0; n < 4 && it != g_resolved.begin(); n++)
            {
                --it;
                Image& img = it->second;
                if (img.format != format || img.resolvePitch != destPitch) continue;
                uint32_t delta = destBase - it->first, w = img.width / g_scale, h = img.height / g_scale;
                for (uint32_t y = 0; y + extent.height <= h; y += 32)
                    for (uint32_t x = 0; x + extent.width <= w; x += 32)
                        if (tiledOffset2D(x, y, tiledBlocksAligned(destPitch), bytes) == delta)
                        {
                            offset = { int32_t(x), int32_t(y) };
                            return &img;
                        }
            }
            return nullptr;
        }

        // Depth resolves: the depth of `src` within `hostRect` goes to an R32_SFLOAT image at the
        // destination address, which 24_8 / 24_8_FLOAT / 32_FLOAT textures then sample (render to
        // texture; shadow maps, scene depth). Vulkan has no depth -> color image copy, so the depth
        // aspect goes through a buffer (D32_SFLOAT_S8_UINT's depth copies out as 32-bit floats). A
        // multisampled source is first resolved to sample 0 by an empty dynamic-rendering pass.
        VkBuffer g_depthScratch = VK_NULL_HANDLE;
        VkDeviceSize g_depthScratchSize = 0;
        std::vector<std::pair<VkBuffer, VkDeviceMemory>> g_retiredBuffers; // may still be in use by frames in flight
        std::map<std::pair<uint32_t, uint32_t>, Image> g_depthSampleZero;   // single-sample copies, by size

        void resolveDepth(Image& src, VkRect2D hostRect, Image& dest, VkOffset2D destAt)
        {
            VkDeviceSize need = VkDeviceSize(hostRect.extent.width) * hostRect.extent.height * 4;
            VkDeviceSize needAll = ((need + 255) & ~VkDeviceSize(255)) + VkDeviceSize(hostRect.extent.width) * hostRect.extent.height;
            if (needAll > g_depthScratchSize)
            {
                static VkDeviceMemory memory = VK_NULL_HANDLE;
                if (g_depthScratch) g_retiredBuffers.push_back({ g_depthScratch, memory });
                VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                bci.size = std::max<VkDeviceSize>(needAll, 4u << 20);
                bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                VK_CHECK(vkCreateBuffer(D, &bci, nullptr, &g_depthScratch));
                VkMemoryRequirements req;
                vkGetBufferMemoryRequirements(D, g_depthScratch, &req);
                VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                mai.allocationSize = req.size;
                mai.memoryTypeIndex = C->findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                VK_CHECK(vkAllocateMemory(D, &mai, nullptr, &memory));
                VK_CHECK(vkBindBufferMemory(D, g_depthScratch, memory, 0));
                g_depthScratchSize = bci.size;
            }
            endPass();
            VkCommandBuffer c = cmd();
            Image* from = &src;
            if (src.samples > 1)
            {
                Image& one = g_depthSampleZero[{ src.width, src.height }];
                if (!one.image)
                    one = createImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, kDepthFormat, src.width, src.height, 1, 1, 1,
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
                transition(c, src, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
                transition(c, one, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
                VkRenderingAttachmentInfo da{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
                da.imageView = src.view;
                da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                da.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT; // always supported for depth
                da.resolveImageView = one.view;
                da.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
                ri.renderArea = hostRect;
                ri.layerCount = 1;
                ri.pDepthAttachment = &da;
                ri.pStencilAttachment = &da; // stencil too (sample zero is always supported), see depthStencilOf
                vkCmdBeginRendering(c, &ri);
                vkCmdEndRendering(c);
                from = &one;
            }
            VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; // earlier uses of the scratch buffer
            mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
            transition(c, *from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
            copy.imageOffset = { hostRect.offset.x, hostRect.offset.y, 0 };
            copy.imageExtent = { hostRect.extent.width, hostRect.extent.height, 1 };
            vkCmdCopyImageToBuffer(c, from->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_depthScratch, 1, &copy);
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
            transition(c, dest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageOffset = { destAt.x, destAt.y, 0 };
            vkCmdCopyBufferToImage(c, g_depthScratch, dest.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            transition(c, dest, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); // resting layout, as color resolves

            // The stencil plane goes the same way into the destination's R8_UINT companion (issue
            // #60): the player highlight reads the scene's depth resolve as 8_8_8_8, i.e. the raw
            // D24S8 words, and finds the selected player by its stencil value in the low byte.
            Image& st = stencilCompanion(dest);
            VkDeviceSize stencilAt = (need + 255) & ~VkDeviceSize(255);
            mb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
            VkBufferImageCopy sc = copy;
            sc.bufferOffset = stencilAt;
            sc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
            sc.imageOffset = { hostRect.offset.x, hostRect.offset.y, 0 };
            vkCmdCopyImageToBuffer(c, from->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_depthScratch, 1, &sc);
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
            transition(c, st, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            sc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            sc.imageOffset = { destAt.x, destAt.y, 0 };
            vkCmdCopyBufferToImage(c, g_depthScratch, st.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &sc);
            g_depthGeneration[dest.image]++;
        }

        // ---------------------------------------------------------------- resolve writeback

        // Resolves are also written to guest memory (issue #45), because the CPU reads some of them:
        // the game's "Stadium texture generator" thread renders the field lightmap, resolves it
        // and compresses the result to DXT1 on the CPU, so a resolve that stays on the GPU gives a
        // black field. A compute shader writes the resolved pixels into the imported guest memory
        // buffer (the one memory export uses), tiled as runtime/gpu/tiling.h and packed as
        // RB_COPY_DEST_INFO says: format, endian, red/blue swap, exponent bias. That is the inverse
        // of the texture upload, so a texture read from memory sees what the resolve image holds.
        // The GPU writes land when the frame's commands run; syncGuestMemory() (called by the command
        // processor before the CPU can see its progress) waits for them.
        // Where guest memory can't be imported (RADV fails the import with
        // VK_ERROR_INVALID_EXTERNAL_HANDLE, probably because the physical views are a shared memfd
        // mapping), the buffer is memory export's 512 MB staging copy: the destination range
        // is copied into it when the resolve is recorded and back out at the sync. A writeback that
        // overlaps one still pending syncs first, so the copy-in can't undo the earlier one.
        // Formats: 8_8_8_8 and 2_10_10_10 color, D24S8 depth (stencil not kept: written as 0).
        // Others are left on the GPU, as before, with a warning.
        // Cost: every writeback means a sync (a GPU drain) at the next fence. A match frame resolves
        // about 34 times, and writing all of them back took live play from 26.6 to 22.2 fps. Those
        // are the scene, shadow maps, bloom and front buffer, resolved every frame and sampled by
        // the GPU. So a destination resolved in each of the last kGpuOnlyAfterFrames frames is
        // treated as GPU-only: its writebacks stop until it misses a frame (logged once per address).
        // The CPU-read resolves seen so far come from load-time texture generators, a few frames each.
        // A CPU read of a resolve made every frame would need lazy writeback (write-protect the
        // destination, write it back on the first CPU access) instead.
        constexpr char kWritebackCs[] = R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, std430) buffer Guest { uint words[]; } guest;
layout(push_constant) uniform Params
{
    uint base;    // guest physical address of pixel (0, 0)
    uint pitch;   // tiled pitch in pixels, 32-aligned
    ivec2 origin; // first pixel written (guest pixels)
    uvec2 size;
    uint scale;   // host pixels per guest pixel (--resolution-scale)
    uint flags;   // bits 0-1 endian, bit 2 red/blue swap, bits 4-5 kind: 0 8_8_8_8, 1 2_10_10_10, 2 D24S8 depth
    float bias;   // 2^copy_dest_exp_bias
} pc;

// tiledOffset2D (runtime/gpu/tiling.h) for 4 bytes per block
uint tiledOffset(uint x, uint y, uint pitch)
{
    uint outerBlocks = ((y >> 5) * (pitch >> 5) + (x >> 5)) << 6;
    uint innerBlocks = (((y >> 1) & 7u) << 3) | (x & 7u);
    uint oi = (outerBlocks | innerBlocks) << 2;
    uint bank = (y >> 4) & 1u;
    uint pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
    return ((y & 1u) << 4) | (oi & 15u) | (((oi >> 4) & 1u) << 5) | (((oi >> 5) & 7u) << 8) | (bank << 11) | (pipe << 6) |
           ((oi >> 8) << 12);
}

uint swapEndian(uint v, uint mode)
{
    if (mode == 1u) return ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
    if (mode == 2u) return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    if (mode == 3u) return (v >> 16) | (v << 16);
    return v;
}

void main()
{
    uvec2 g = gl_GlobalInvocationID.xy;
    if (g.x >= pc.size.x || g.y >= pc.size.y) return;
    uvec2 p = uvec2(pc.origin) + g;
    vec4 c = texelFetch(src, ivec2(p * pc.scale), 0);
    uint kind = (pc.flags >> 4) & 3u, word;
    if (kind == 2u)
        word = uint(clamp(c.r, 0.0, 1.0) * 16777215.0 + 0.5) << 8;
    else
    {
        c = clamp(c * pc.bias, 0.0, 1.0);
        if ((pc.flags & 4u) != 0u) c = c.bgra;
        if (kind == 1u)
        {
            uvec4 u = uvec4(round(c * vec4(1023.0, 1023.0, 1023.0, 3.0)));
            word = u.r | (u.g << 10) | (u.b << 20) | (u.a << 30);
        }
        else
            word = packUnorm4x8(c);
    }
    guest.words[(pc.base + tiledOffset(p.x, p.y, pc.pitch)) >> 2] = swapEndian(word, pc.flags & 3u);
}
)";
        constexpr uint32_t RB_COPY_DEST_INFO = 0x231B;
        enum WritebackKind : uint32_t { kWrite8888 = 0, kWrite2101010 = 1, kWriteDepth24 = 2 };
        VkDescriptorSetLayout g_writebackSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout g_writebackLayout = VK_NULL_HANDLE;
        VkPipeline g_writebackPipeline = VK_NULL_HANDLE;
        VkSampler g_writebackSampler = VK_NULL_HANDLE;
        bool g_writebackFailed = false;
        uint64_t g_writebacks = 0, g_writebackSyncs = 0, g_writebacksSkipped = 0; // for the stats line
        constexpr uint32_t kGpuOnlyAfterFrames = 60;
        struct Recurrence { uint64_t lastFrame = 0; uint32_t run = 0; }; // frames in a row with a resolve there
        std::unordered_map<uint32_t, Recurrence> g_recurrence;            // by destination base

        // Count this frame's resolve to `base`; true once it has been resolved every frame for a while
        bool gpuOnly(uint32_t base)
        {
            Recurrence& r = g_recurrence[base];
            if (r.lastFrame != g_frameSerial)
            {
                r.run = r.lastFrame + 1 == g_frameSerial ? r.run + 1 : 1;
                r.lastFrame = g_frameSerial;
            }
            if (r.run <= kGpuOnlyAfterFrames) return false;
            static std::set<uint32_t> logged;
            if (logged.insert(base).second)
                info("[render] resolve writeback: %08X is resolved every frame - kept on the GPU while that lasts", base);
            return true;
        }

        // Copy pending writes to guest memory: those of every submit up to `serial`, which the GPU has
        // finished (all of them with `all`, after a full wait).
        void retire(uint64_t serial, bool all = false)
        {
            if (g_pendingWrites.empty()) return;
            // Ranges can overlap (a resolve over a pending one): copying the older out may already
            // carry some of the newer, as the GPU writing memory as it runs would; the newer one
            // retires later and lands whole.
            size_t kept = 0;
            for (size_t i = 0; i < g_pendingWrites.size(); i++)
            {
                const PendingWrite& w = g_pendingWrites[i];
                if (all || (w.serial && w.serial <= serial))
                {
                    if (g_exportMap) // staging copy of guest memory: copy the written range out
                        std::memcpy(mem::physical(w.start), g_exportMap + w.start, w.end - w.start);
                }
                else
                    g_pendingWrites[kept++] = w;
            }
            g_pendingWrites.resize(kept);
            if (kept) return;
            for (auto& [base, img] : g_resolved) // their writebacks have landed: fingerprint them now
                if (img.memSigPending)
                {
                    uint32_t start, end;
                    img.memSigPending = false;
                    if (memRange(img, start, end))
                    {
                        img.memSig = memSignature(start, end);
                        img.memSigFrame = g_frameSerial;
                    }
                }
        }

        uint64_t gpuProgress()
        {
            uint64_t v = 0;
            if (g_timeline) VK_CHECK(vkGetSemaphoreCounterValue(D, g_timeline, &v));
            return v;
        }

        // Before deciding a write must wait for pending ones: retire what the GPU has already done
        void retireCompleted()
        {
            if (g_timeline && !g_pendingWrites.empty()) retire(gpuProgress());
        }

        // Copy guest memory [start, end) into the staging buffer, except the parts pending GPU writes
        // cover (there the staging buffer is ahead of guest memory)
        void copyInAroundPending(uint32_t start, uint32_t end)
        {
            std::vector<std::pair<uint32_t, uint32_t>> skip;
            for (const PendingWrite& w : g_pendingWrites)
                if (w.start < end && start < w.end) skip.push_back({ std::max(w.start, start), std::min(w.end, end) });
            std::sort(skip.begin(), skip.end());
            uint32_t at = start;
            for (auto [s0, s1] : skip)
            {
                if (s0 > at) std::memcpy(g_exportMap + at, mem::physical(at), s0 - at);
                at = std::max(at, s1);
            }
            if (end > at) std::memcpy(g_exportMap + at, mem::physical(at), end - at);
            if (!skip.empty()) g_perf.copyInsAroundPending++;
        }

        bool pendingOverlap(uint32_t start, uint32_t end)
        {
            for (const PendingWrite& w : g_pendingWrites)
                if (start < w.end && w.start < end) return true;
            return false;
        }

        bool writebackPipeline()
        {
            if (g_writebackPipeline) return true;
            if (g_writebackFailed) return false;
            VkDescriptorSetLayoutBinding b[2] = {
                { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
            VkDescriptorSetLayoutCreateInfo dl{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
            dl.bindingCount = 2;
            dl.pBindings = b;
            VK_CHECK(vkCreateDescriptorSetLayout(D, &dl, nullptr, &g_writebackSetLayout));
            VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 36 };
            VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pli.setLayoutCount = 1;
            pli.pSetLayouts = &g_writebackSetLayout;
            pli.pushConstantRangeCount = 1;
            pli.pPushConstantRanges = &pcr;
            VK_CHECK(vkCreatePipelineLayout(D, &pli, nullptr, &g_writebackLayout));
            std::string error;
            VkShaderModule cs = compile(kWritebackCs, shaderc_compute_shader, "resolve_writeback.comp", error);
            if (!cs)
            {
                warn("[render] resolve writeback shader: %s", error.c_str());
                g_writebackFailed = true;
                return false;
            }
            VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main", nullptr };
            ci.layout = g_writebackLayout;
            VK_CHECK(vkCreateComputePipelines(D, VK_NULL_HANDLE, 1, &ci, nullptr, &g_writebackPipeline));
            vkDestroyShaderModule(D, cs, nullptr);
            VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            VK_CHECK(vkCreateSampler(D, &si, nullptr, &g_writebackSampler));
            info("[render] resolves are written back to guest memory");
            return true;
        }

        // Write `r` (guest pixels, in the resolve image's own coordinates) of resolve image `img` to
        // guest memory at img.guestBase. `kind` is a WritebackKind, or anything else to skip.
        void writeBack(Image& img, VkRect2D r, uint32_t kind)
        {
            g_perf.writebacks++;
            if (!g_memExportReady)
            {
                warnOnce("resolves stay on the GPU: no guest memory buffer");
                return;
            }
            uint32_t destInfo = readRegister(RB_COPY_DEST_INFO), endian = destInfo & 7;
            if (kind > kWriteDepth24 || endian > 3)
            {
                warnOnce("resolve to format " + std::to_string((destInfo >> 7) & 0x3F) + ", endian " + std::to_string(endian) +
                         " is not written to guest memory yet");
                return;
            }
            // Only inside the destination the game described (the image can be larger)
            uint32_t x1 = std::min(uint32_t(r.offset.x) + r.extent.width, img.guestPitch);
            uint32_t y1 = std::min(uint32_t(r.offset.y) + r.extent.height, img.guestHeight);
            if (x1 <= uint32_t(r.offset.x) || y1 <= uint32_t(r.offset.y) || !writebackPipeline()) return;
            if (gpuOnly(img.guestBase))
            {
                g_writebacksSkipped++;
                return;
            }
            uint32_t pitch = tiledBlocksAligned(img.guestPitch);
            uint64_t end = uint64_t(img.guestBase) + tiledUpperBound2D(x1, y1, pitch, 4);
            if (end > mem::kPhysicalSize)
            {
                warnOnce("resolve destination extends past physical memory: not written back");
                return;
            }
            if (g_exportMap)
            {
                // The staging copy has to hold what the guest memory holds around the rectangle the
                // shader writes. Where an earlier GPU write is still pending, the staging copy is
                // already the newer one (and queue order puts this write after it): copy in only the
                // rest, instead of waiting for the GPU (#73: 75-150 waits a second in some scenes).
                if (!g_pendingWrites.empty()) retireCompleted();
                copyInAroundPending(img.guestBase, uint32_t(end));
            }
            endPass();
            VkCommandBuffer c = cmd();
            VkImageLayout was = img.layout;
            transition(c, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, g_writebackPipeline);
            VkDescriptorImageInfo ii{ g_writebackSampler, img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorBufferInfo bi{ g_guestMem, 0, mem::kPhysicalSize };
            VkWriteDescriptorSet w[2]{};
            for (int i = 0; i < 2; i++)
            {
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[i].dstBinding = i;
                w[i].descriptorCount = 1;
            }
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[0].pImageInfo = &ii;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[1].pBufferInfo = &bi;
            C->cmdPushDescriptorSet(c, VK_PIPELINE_BIND_POINT_COMPUTE, g_writebackLayout, 0, 2, w);
            struct
            {
                uint32_t base, pitch;
                int32_t ox, oy;
                uint32_t w, h, scale, flags;
                float bias;
            } pc{ img.guestBase, pitch, r.offset.x, r.offset.y, x1 - r.offset.x, y1 - r.offset.y, g_scale,
                  endian | (((destInfo >> 24) & 1) << 2) | (kind << 4),
                  std::ldexp(1.0f, int32_t((destInfo >> 16) & 0x3F) << 26 >> 26) };
            vkCmdPushConstants(c, g_writebackLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vkCmdDispatch(c, (pc.w + 7) / 8, (pc.h + 7) / 8, 1);
            VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                                 &mb, 0, nullptr, 0, nullptr);
            transition(c, img, was == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : was);
            g_pendingWrites.push_back({ img.guestBase, uint32_t(end), 0, false });
            g_perf.pendingAdded++;
            g_writebacks++;
        }

        // The writeback kind for a color resolve: by the destination format (RB_COPY_DEST_INFO)
        uint32_t colorWritebackKind()
        {
            switch ((readRegister(RB_COPY_DEST_INFO) >> 7) & 0x3F)
            {
            case 6: return kWrite8888;    // k_8_8_8_8
            case 7: return kWrite2101010; // k_2_10_10_10
            default: return UINT32_MAX;
            }
        }

        // After a resolve into `img`: fingerprint its guest memory now, or after the sync if a
        // writeback into it is still to land (see "stale resolve images")
        void markResolved(Image& img)
        {
            uint32_t start, end;
            if (!memRange(img, start, end)) return;
            img.memSigPending = pendingOverlap(start, end);
            if (!img.memSigPending)
            {
                img.memSig = memSignature(start, end);
                img.memSigFrame = g_frameSerial;
            }
        }

        void resolve(const DrawState& s)
        {
            g_perf.resolves++;
            uint32_t control = readRegister(RB_COPY_CONTROL);
            uint32_t source = control & 7, command = (control >> 20) & 3;
            bool clearColor = (control >> 8) & 1, clearDepth = (control >> 9) & 1;
            uint32_t destBase = readRegister(RB_COPY_DEST_BASE) & 0x1FFFFFFF, destPitchReg = readRegister(RB_COPY_DEST_PITCH);
            uint32_t destPitch = destPitchReg & 0x3FFF, destHeight = (destPitchReg >> 16) & 0x3FFF;

            Image* color = source < 4 ? findTarget({ s.color[source].edramBase, s.surfacePitch, s.msaaSamples, s.color[source].format, false })
                                      : nullptr;
            Image* depth = findTarget({ s.depthBase, s.surfacePitch, s.msaaSamples, 0, true });
            if (depth) depth->edramFormat = s.depthFormat; // D24S8 or D24FS8: how its samples map to EDRAM bits
            Image* src = source < 4 ? color : depth;
            VkRect2D rect;
            VkExtent2D limit = src ? VkExtent2D{ src->width / g_scale, src->height / g_scale } : VkExtent2D{ 8192, 8192 };
            if (!scissorRect(s, limit, rect)) return; // guest pixels
            VkRect2D hostRect = scaled(rect);

            uint32_t dw = std::max(destPitch, rect.offset.x + rect.extent.width), dh = std::max(destHeight, rect.offset.y + rect.extent.height);
            debugaids::onResolve(source, rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height, destBase, destPitch, destHeight,
                                 source < 4 && color ? std::max(formatBytes(color->format), 1u) : 4u, command); // BB_DEBUG_WATCH
            if (source == 4)
            {
                if (depth && command != 3 && destPitch && destHeight)
                {
                    syncTarget(depth, rect); // EDRAM aliasing: latest contents of these tiles
                    VkOffset2D tile{};
                    Image* into = resolveInto(destBase, destPitch, VK_FORMAT_R32_SFLOAT, rect.extent, tile);
                    Image& dest = into ? *into : resolveDestination(destBase, destPitch, destHeight, dw, dh, VK_FORMAT_R32_SFLOAT);
                    resolveDepth(*depth, hostRect, dest,
                                 { hostRect.offset.x + tile.x * int32_t(g_scale), hostRect.offset.y + tile.y * int32_t(g_scale) });
                    writeBack(dest, { { rect.offset.x + tile.x, rect.offset.y + tile.y }, rect.extent },
                              depth->edramFormat == 0 ? uint32_t(kWriteDepth24) : UINT32_MAX); // D24FS8: not yet
                    markResolved(dest);
                }
            }
            else if (color && command != 3 && destPitch && destHeight)
            {
                syncTarget(color, rect); // EDRAM aliasing: latest contents of these tiles
                // Destination image for this guest address (same format as the source), or the image a
                // tile of it belongs to (predicated tiling)
                VkOffset2D tile{};
                Image* into = resolveInto(destBase, destPitch, color->format, rect.extent, tile);
                Image& dest = into ? *into : resolveDestination(destBase, destPitch, destHeight, dw, dh, color->format);
                dest.swapRB = (readRegister(0x231B) >> 24) & 1; // RB_COPY_DEST_INFO copy_dest_swap
                endPass();
                VkCommandBuffer c = cmd();
                transition(c, *color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                transition(c, dest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkImageSubresourceLayers layers{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                VkOffset3D at{ hostRect.offset.x, hostRect.offset.y, 0 };
                VkOffset3D destAt{ at.x + tile.x * int32_t(g_scale), at.y + tile.y * int32_t(g_scale), 0 };
                VkExtent3D extent{ hostRect.extent.width, hostRect.extent.height, 1 };
                if (color->samples > 1)
                {
                    VkImageResolve r{ layers, at, layers, destAt, extent };
                    vkCmdResolveImage(c, color->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
                }
                else
                {
                    VkImageCopy r{ layers, at, layers, destAt, extent };
                    vkCmdCopyImage(c, color->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
                }
                transition(c, dest, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); // resting layout (presenter, screenshots)
                writeBack(dest, { { rect.offset.x + tile.x, rect.offset.y + tile.y }, rect.extent }, colorWritebackKind());
                markResolved(dest);
            }

            // Clears after the copy, of the copied rectangle. The registers hold the bits the clear
            // writes into EDRAM, in the target's own format (clear_value.h).
            if (clearColor && color)
            {
                syncPartialTiles(color, rect);
                Image* targets[4] = { color, nullptr, nullptr, nullptr };
                beginPass(targets, nullptr);
                VkClearAttachment a{ VK_IMAGE_ASPECT_COLOR_BIT, 0, {} };
                clear::color(s.color[source].format, clear::colorBits(readRegister(RB_COLOR_CLEAR), readRegister(RB_COLOR_CLEAR_LO)),
                             a.clearValue.color.float32);
                VkClearRect r{ hostRect, 0, 1 };
                vkCmdClearAttachments(frame().cmd, 1, &a, 1, &r);
                markWritten(color, rect);
            }
            if (clearDepth && (depth || (depth = target({ s.depthBase, s.surfacePitch, s.msaaSamples, 0, true }))))
            {
                depth->edramFormat = s.depthFormat;
                syncPartialTiles(depth, rect);
                Image* none[4] = {};
                beginPass(none, depth);
                uint32_t v = readRegister(RB_DEPTH_CLEAR);
                VkClearAttachment a{ VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, {} };
                a.clearValue.depthStencil = { clear::depth(s.depthFormat, v), clear::stencil(v) };
                VkRect2D dr{ hostRect.offset, { std::min(hostRect.extent.width, depth->width - uint32_t(hostRect.offset.x)),
                                                std::min(hostRect.extent.height, depth->height - uint32_t(hostRect.offset.y)) } };
                VkClearRect r{ dr, 0, 1 };
                if (hostRect.offset.x < int32_t(depth->width) && hostRect.offset.y < int32_t(depth->height))
                {
                    vkCmdClearAttachments(frame().cmd, 1, &a, 1, &r);
                    markWritten(depth, rect);
                }
            }
        }

        // Guest indices for this draw, byte-swapped into the upload buffer (32-bit when expanded)
        struct IndexBuffer
        {
            VkDeviceSize offset = 0;
            VkIndexType type = VK_INDEX_TYPE_UINT16;
            uint32_t count = 0;
            bool ok = false;
            uint32_t minIndex = 0, maxIndex = 0; // range of guest indices used (before VGT_INDX_OFFSET)
        };

        IndexBuffer indices(const DrawState& s, uint32_t indexBase, uint32_t indexSizeWord, bool quads)
        {
            IndexBuffer ib;
            uint32_t count = s.indexCount;
            bool dma = s.indexSource == 0;
            if (dma && !g_pendingWrites.empty())
                needsExportedData(indexBase & 0x1FFFFFFF, uint64_t(indexBase & 0x1FFFFFFF) + uint64_t(count) * (s.indexSize ? 4 : 2) + 4);
            uint32_t endian = indexSizeWord >> 30;
            // The guest's indices, decoded in one tight loop per byte order instead of a call, an
            // endian switch and a size check per index (10.8% of the CP thread, #73)
            static std::vector<uint32_t> guest;
            if (dma)
            {
                guest.resize(count);
                const uint32_t* words = (const uint32_t*)mem::physical(s.indexSize ? indexBase : (indexBase & ~3u));
                uint32_t odd = (indexBase >> 1) & 1;
                uint32_t* g = guest.data();
                bool wide = s.indexSize;
                auto decode = [&](auto swap) {
                    if (wide)
                        for (uint32_t i = 0; i < count; i++) g[i] = swap(words[i]);
                    else
                        for (uint32_t i = 0; i < count; i++)
                        {
                            uint32_t k = i + odd, w = swap(words[k >> 1]);
                            g[i] = (k & 1) ? w >> 16 : w & 0xFFFF;
                        }
                };
                switch (endian & 3)
                {
                case 1: decode([](uint32_t v) { return ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u); }); break;
                case 2: decode([](uint32_t v) { return __builtin_bswap32(v); }); break;
                case 3: decode([](uint32_t v) { return (v >> 16) | (v << 16); }); break;
                default: decode([](uint32_t v) { return v; }); break;
                }
            }
            auto guestIndex = [&](uint32_t i) -> uint32_t { return guest[i]; };
            if (quads)
            {
                // Quad (a, b, c, d) -> triangles (a, b, c), (a, c, d)
                uint32_t n = count / 4 * 6;
                VkDeviceSize off = uploadAlloc(VkDeviceSize(n) * 4, 4);
                if (off == UINT64_MAX) return ib;
                uint32_t* out = (uint32_t*)(frame().uploadMap + off);
                uint32_t lo = UINT32_MAX, hi = 0;
                for (uint32_t q = 0; q < count / 4; q++)
                {
                    uint32_t v[4];
                    for (int k = 0; k < 4; k++)
                    {
                        v[k] = dma ? guestIndex(q * 4 + k) : q * 4 + k;
                        lo = std::min(lo, v[k]);
                        hi = std::max(hi, v[k]);
                    }
                    const int order[6] = { 0, 1, 2, 0, 2, 3 };
                    for (int k = 0; k < 6; k++) out[q * 6 + k] = v[order[k]];
                }
                return { off, VK_INDEX_TYPE_UINT32, n, true, lo == UINT32_MAX ? 0 : lo, hi };
            }
            VkDeviceSize bytes = VkDeviceSize(count) * (s.indexSize ? 4 : 2);
            VkDeviceSize off = uploadAlloc(bytes + 4, 4);
            if (off == UINT64_MAX) return ib;
            uint8_t* out = frame().uploadMap + off;
            uint32_t restart = s.indexSize ? 0xFFFFFFFFu : 0xFFFFu;
            uint32_t lo = UINT32_MAX, hi = 0;
            const uint32_t* g = guest.data();
            if (!s.primitiveRestart)
            {
                for (uint32_t i = 0; i < count; i++) { lo = std::min(lo, g[i]); hi = std::max(hi, g[i]); }
                if (s.indexSize) std::memcpy(out, g, size_t(count) * 4);
                else for (uint32_t i = 0; i < count; i++) ((uint16_t*)out)[i] = uint16_t(g[i]);
            }
            else
                for (uint32_t i = 0; i < count; i++)
                {
                    uint32_t v = g[i];
                    if (v == (s.restartIndex & restart)) v = restart;
                    else { lo = std::min(lo, v); hi = std::max(hi, v); }
                    if (s.indexSize) ((uint32_t*)out)[i] = v;
                    else ((uint16_t*)out)[i] = uint16_t(v);
                }
            return { off, s.indexSize ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16, count, true, lo == UINT32_MAX ? 0 : lo, hi };
        }

        // false: the upload buffer ran out before anything of this draw was recorded
        // ---------------------------------------------------------------- redraws of depth-written geometry
        //
        // Multipass effects draw the same geometry twice: once writing depth, then again with
        // LessEqual (or Equal) and no depth write, relying on the second pass producing the very same
        // depths, e.g. the decal pass that lays the zones, routes and kick meter onto the field (issue
        // #54) re-draws the field's own quad. The console's two shaders compute bit-identical
        // positions; ours can differ by an ulp, and where the geometry is clipped at the near plane
        // (the field quad reaches behind the camera) an ulp becomes a tenth of a pixel, i.e. hundreds
        // of depth units on a surface seen at a grazing angle, and the whole second pass fails the
        // depth test on some frames: the flicker. Such redraws get a depth bias toward the camera of
        // half a pixel's depth slope (plus a few units), which only matters where the two depths tie.
        constexpr float kRedrawBiasSlopePixels = 0.5f, kRedrawBiasUnits = 16.0f;
        std::unordered_set<uint64_t> g_depthWrittenGeometry; // this frame's depth-writing draws, by geometryKey

        // The geometry a draw rasterizes: primitive, indices, and the vertex fetch constants its
        // vertex shader reads (same data, same layout)
        uint64_t geometryKey(const DrawState& s, const Shader& vs, uint32_t indexBase, uint32_t indexSizeWord)
        {
            uint64_t h = 1469598103934665603ull;
            auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
            mix(s.primitive); mix(s.indexSource); mix(s.indexCount); mix(indexBase); mix(indexSizeWord);
            for (uint32_t slot = 0; slot < 96; slot++)
                if ((vs.t.vertexFetchSlots[slot / 32] >> (slot % 32)) & 1)
                {
                    mix(slot);
                    mix(readRegister(kFetchConstants + slot * 2));
                    mix(readRegister(kFetchConstants + slot * 2 + 1));
                }
            return h;
        }

        bool drawOnce(const DrawState& s, uint32_t indexBase, uint32_t indexSizeWord)
        {
            g_perf.draws++;
            // Redraws of geometry this frame already wrote depth for: see kRedrawBiasSlopePixels
            constexpr uint32_t kZEqual = 2, kZLessEqual = 3;
            if (s.depthTest && !s.polyOffsetFront && !s.polyOffsetBack)
                if (Shader* geomVs = currentShader(0))
                {
                    uint64_t key = geometryKey(s, *geomVs, indexBase, indexSizeWord);
                    if (s.depthWrite) g_depthWrittenGeometry.insert(key);
                    else if ((s.depthFunc == kZLessEqual || s.depthFunc == kZEqual) && g_depthWrittenGeometry.count(key))
                    {
                        static bool logged = false;
                        if (!logged)
                        {
                            logged = true;
                            info("[render] depth-tested redraw of depth-written geometry (decal pass): depth bias toward the camera");
                        }
                        DrawState t = s;
                        t.polyOffsetFront = t.polyOffsetBack = true;
                        t.polyOffset = -kRedrawBiasUnits / 16777216.0f;   // depth-range units (see vkCmdSetDepthBias below)
                        t.polyOffsetScale = -kRedrawBiasSlopePixels * 16.0f; // 1/16-pixel units
                        return drawOnce(t, indexBase, indexSizeWord);
                    }
                }
            g_outOfSpace = false;
            g_syncRetry = false;

            // Topology
            VkPrimitiveTopology topology;
            bool rectangles = false, quads = false;
            switch (s.primitive)
            {
            case 1: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
            case 2: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
            case 3: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
            case 4: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
            case 5: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
            case 6: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
            case 8: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; rectangles = true; break;
            case 13: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; quads = true; break;
            default:
                warnOnce("primitive type " + std::to_string(s.primitive) + " not supported yet");
                return true;
            }
            if (s.indexSource == 1)
            {
                warnOnce("draws with immediate indices not supported yet");
                return true;
            }

            Shader* vs = currentShader(0);
            Shader* ps = currentShader(1);
            if (!vs || !ps) return true;

            // Render targets this draw writes
            Image* color[4]{};
            VkFormat formats[4] = { VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
            if (s.edramMode == 4)
                for (int i = 0; i < 4; i++)
                    if (s.color[i].writeMask && ((ps->t.colorOutputs >> i) & 1) &&
                        (color[i] = target({ s.color[i].edramBase, s.surfacePitch, s.msaaSamples, s.color[i].format, false })))
                        formats[i] = color[i]->format;
            Image* depth = (s.depthTest || s.stencil) ? target({ s.depthBase, s.surfacePitch, s.msaaSamples, 0, true }) : nullptr;
            if (depth) depth->edramFormat = s.depthFormat; // D24S8 or D24FS8: how its samples map to EDRAM bits
            // Memory export: the shader writes guest memory (GPU skinning, particles...). Without
            // render targets it is a "compute" draw with rasterization discarded.
            bool memExport = vs->t.memExport || ps->t.memExport;
            if (memExport && !g_memExportReady) { warnOnce("memory export draw skipped (guest memory not imported)"); return true; }
            bool noTargets = !depth && !color[0] && !color[1] && !color[2] && !color[3];
            if (memExport && g_exportMap && !(vs->t.memExportStreams[0] | vs->t.memExportStreams[1] | vs->t.memExportStreams[2] |
                                              vs->t.memExportStreams[3] | vs->t.memExportStreams[4] | vs->t.memExportStreams[5] |
                                              vs->t.memExportStreams[6] | vs->t.memExportStreams[7]))
            {
                warnOnce("memory export without a recognizable stream constant (mad eA, r#, const0100, c#): draw skipped");
                return true;
            }
            if (noTargets && !memExport) return true; // nothing to write (e.g. masked point draws)
            std::vector<std::pair<uint32_t, uint32_t>> exportRanges; // (physical address, bytes)
            if (memExport)
                for (uint32_t c = 0; c < 256; c++)
                {
                    if (!((vs->t.memExportStreams[c / 32] >> (c % 32)) & 1)) continue;
                    uint32_t r = kAluConstants + std::min(s.vsConstBase * 4 + c * 4, 2044u);
                    uint32_t x = readRegister(r), z = readRegister(r + 2), w = readRegister(r + 3);
                    static const uint8_t sizes[64] = { 0, 0, 1, 0, 0, 0, 4, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 4,
                                                       8, 0, 0, 0, 2, 4, 8, 4, 8, 16, 4, 8, 16 }; // as xeElementSize() in the shader
                    uint64_t start = uint64_t(x & 0x3FFFFFFF) << 2, bytes = uint64_t(w & 0x7FFFFF) * sizes[(z >> 8) & 0x3F];
                    if (bytes && start < mem::kPhysicalSize)
                        exportRanges.push_back({ uint32_t(start), uint32_t(std::min<uint64_t>(bytes, mem::kPhysicalSize - start)) });
                    static std::set<std::pair<uint32_t, uint32_t>> seen;
                    if (seen.insert({ x, z }).second)
                        info("[render] memory export stream c%u: base %08X, %u elements, format %u (number format %u, endian %u%s)", c,
                             (x & 0x3FFFFFFF) << 2, w & 0x7FFFFF, (z >> 8) & 0x3F, (z >> 16) & 7, z & 7, (z >> 19) & 1 ? ", red/blue swap" : "");
                }
            // The staging copy-in below would undo a resolve writeback still pending in these ranges
            // (run, but not copied out yet): sync it first, before this draw records anything
            if (g_exportMap)
                for (auto [start, bytes] : exportRanges)
                    if (pendingOverlap(start, start + bytes)) { retireCompleted(); if (pendingOverlap(start, start + bytes)) syncGuestMemory("export_over_pending"); }

            // Snapshot guest data into the upload buffer: constants, vertex data, indices
            Frame& f = frame();
            VkDeviceSize constants = uploadAlloc(kConstantsSize);
            if (constants == UINT64_MAX) return false;
            uint32_t* cv = (uint32_t*)(f.uploadMap + constants);
            // Straight from the register file, swapped in tight loops (a call per register was 12% of
            // the renderer thread's time, #73). Past the end of the 2048 ALU constants, the last one.
            const uint32_t* regs = registerFile();
            auto copySwapped = [&](uint32_t* dst, uint32_t first, uint32_t count) {
                for (uint32_t i = 0; i < count; i++) dst[i] = __builtin_bswap32(regs[first + i]);
            };
            for (uint32_t half = 0; half < 2; half++)
            {
                uint32_t base = std::min((half ? s.psConstBase : s.vsConstBase) * 4, 2047u);
                uint32_t inRange = std::min(1024u, 2048u - base);
                copySwapped(cv + half * 1024, kAluConstants + base, inRange);
                uint32_t last = __builtin_bswap32(regs[kAluConstants + 2047]);
                for (uint32_t i = inRange; i < 1024; i++) cv[half * 1024 + i] = last;
            }
            uint32_t* other = cv + 2048;
            copySwapped(other, kBoolConstants, 8);
            copySwapped(other + 8, kLoopConstants, 32);
            uint32_t* fetch = other + 40;
            copySwapped(fetch, kFetchConstants, 192);
            IndexBuffer ib;
            int64_t minIndex = 0, maxIndex = int64_t(s.indexCount) - 1;
            if (s.indexSource == 0 || quads)
            {
                ib = indices(s, indexBase, indexSizeWord, quads);
                if (!ib.ok) return false;
                if (s.indexSource == 0) { minIndex = ib.minIndex; maxIndex = ib.maxIndex; }
            }
            minIndex += s.indexOffset;
            maxIndex += s.indexOffset;
            std::vector<VkBufferCopy> gpuVertexCopies;
            for (uint32_t slot = 0; slot < 96; slot++)
            {
                if (!((vs->t.vertexFetchSlots[slot / 32] >> (slot % 32)) & 1)) continue;
                // Copy the vertices this draw uses (the whole buffer when the shader computes its
                // own fetch index), at most 16 MB
                uint32_t& d0 = fetch[slot * 2];
                uint64_t baseDw = d0 >> 2, sizeDw = (fetch[slot * 2 + 1] >> 2) & 0xFFFFFF, stride = vs->t.vertexStride[slot];
                uint64_t first = 0, end = sizeDw;
                if (vs->t.vertexIndexDirect && stride)
                {
                    first = uint64_t(std::max<int64_t>(minIndex, 0)) * stride;
                    end = uint64_t(std::max<int64_t>(maxIndex, 0)) * stride + std::max<uint64_t>(vs->t.vertexExtent[slot], stride);
                    if (sizeDw) end = std::min(end, sizeDw);
                }
                end = std::min(end, first + (4u << 20));
                uint64_t dwords = end > first ? end - first : 0;
                VkDeviceSize off = uploadAlloc(dwords * 4 + 16, 16);
                if (off == UINT64_MAX) return false;
                uint32_t src = uint32_t((baseDw + first) * 4);
                std::memcpy(f.uploadMap + off, mem::physical(src), dwords * 4);
                // Parts written by a memory export or resolve writeback that is recorded but not yet
                // synced are only right in the GPU's copy of guest memory: copy those over the CPU's
                // Only the pending ranges inside this slot's bytes (almost always none), merged where
                // they overlap so no byte is copied twice
                static std::vector<std::pair<uint64_t, uint64_t>> hits;
                hits.clear();
                for (const PendingWrite& w : g_pendingWrites)
                {
                    uint64_t a = std::max<uint64_t>(w.start, src), b = std::min<uint64_t>(w.end, uint64_t(src) + dwords * 4);
                    if (a < b) hits.push_back({ a, b });
                }
                if (!hits.empty() && g_guestMem)
                {
                    std::sort(hits.begin(), hits.end());
                    uint64_t a = hits[0].first, b = hits[0].second;
                    for (size_t i = 1; i <= hits.size(); i++)
                    {
                        if (i < hits.size() && hits[i].first <= b) { b = std::max(b, hits[i].second); continue; }
                        gpuVertexCopies.push_back({ a, off + (a - src), b - a });
                        if (i < hits.size()) { a = hits[i].first; b = hits[i].second; }
                    }
                }
                d0 = uint32_t(off / 4) - uint32_t(first); // shader: mem[d0 + index * stride] (mod 2^32)
            }
            if (!gpuVertexCopies.empty())
            {
                endPass();
                VkCommandBuffer cb = cmd();
                vkCmdCopyBuffer(cb, g_guestMem, f.upload, uint32_t(gpuVertexCopies.size()), gpuVertexCopies.data());
                VkMemoryBarrier vb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                vb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 1, &vb, 0, nullptr, 0, nullptr);
                g_perf.gpuVertexCopies += gpuVertexCopies.size();
            }

            // Textures (uploads end the current pass)
            VkDescriptorImageInfo images[32]{};
            uint32_t textureSlots = vs->t.textureSlots | ps->t.textureSlots;
            for (uint32_t slot = 0; slot < 32; slot++)
                if ((textureSlots >> slot) & 1)
                    images[slot] = texture(slot, ((ps->t.textureSlots >> slot) & 1) ? ps->t.textureDims[slot] : vs->t.textureDims[slot]);
            if (g_outOfSpace || g_syncRetry) return false;

            // Scissor over all attachments, in guest pixels
            VkExtent2D guestExtent{ UINT32_MAX, UINT32_MAX };
            for (Image* t : { color[0], color[1], color[2], color[3], depth })
                if (t) guestExtent = { std::min(guestExtent.width, t->width / g_scale), std::min(guestExtent.height, t->height / g_scale) };
            VkRect2D guestScissor{ { 0, 0 }, { 1, 1 } };
            VkRect2D edramRect = guestScissor; // what the draw can touch: the scissor, or less (vertexBounds)
            if (!noTargets)
            {
                if (!scissorRect(s, guestExtent, guestScissor)) return true;
                edramRect = guestScissor;
                VkRect2D vertices;
                if (vertexBounds(s, vs->t.position, vertices) && !intersect(edramRect, vertices, edramRect))
                    edramRect = { { 0, 0 }, { 0, 0 } }; // drawn outside the scissor: touches nothing
                for (Image* t : { color[0], color[1], color[2], color[3], depth }) syncTarget(t, edramRect); // EDRAM aliasing
            }

            beginPass(color, depth);
            VkRect2D scissor = noTargets ? VkRect2D{ { 0, 0 }, { 1, 1 } } : scaled(guestScissor);
            VkCommandBuffer c = f.cmd;
            VkPipeline pl = pipeline({ &s, vs, ps, topology, rectangles, { formats[0], formats[1], formats[2], formats[3] }, depth != nullptr,
                                       s.msaaSamples, noTargets });
            vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);

            uint32_t dynamicOffsets[3] = { uint32_t(constants), uint32_t(constants + 4096), uint32_t(constants + 8192) };
            vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 1, &f.buffers, 3, dynamicOffsets);
            VkWriteDescriptorSet writes[32]{};
            uint32_t textureWrites = 0;
            for (uint32_t slot = 0; slot < 32; slot++)
            {
                if (!((textureSlots >> slot) & 1)) continue;
                VkWriteDescriptorSet& w = writes[textureWrites++];
                w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                w.dstBinding = slot;
                w.descriptorCount = 1;
                w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w.pImageInfo = &images[slot];
            }
            if (textureWrites) C->cmdPushDescriptorSet(c, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 1, textureWrites, writes);

            // Guest viewport -> host NDC over the render area (see shader_translator.h). The NDC
            // math is in guest pixels; the host viewport covers the scaled render area.
            float w = float(guestExtent.width), h = float(guestExtent.height);
            float ox = s.vpOffset[0], oy = s.vpOffset[1];
            if (s.vertexWindowOffset) { ox += s.windowOffsetX; oy += s.windowOffsetY; }
            if (!s.pixelCenterHalf) { ox += 0.5f; oy += 0.5f; } // Direct3D pixel centers are at .0, Vulkan's at .5
            SystemConstants sc{};
            sc.ndcScale[0] = s.vpScale[0] * 2.0f / w;
            sc.ndcScale[1] = s.vpScale[1] * 2.0f / h;
            sc.ndcScale[2] = s.vpScale[2];
            sc.ndcOffset[0] = ox * 2.0f / w - 1.0f;
            sc.ndcOffset[1] = oy * 2.0f / h - 1.0f;
            sc.ndcOffset[2] = s.vpOffset[2];
            sc.vertexIndexOffset = s.indexOffset;
            sc.flags = (s.xyDividedByW ? kSysFlagXYDividedByW : 0) | (s.zDividedByW ? kSysFlagZDividedByW : 0) |
                       (s.wNotReciprocal ? kSysFlagWNotReciprocal : 0);
            sc.alphaFunc = s.alphaTest ? s.alphaFunc : 7;
            sc.alphaRef = s.alphaRef;
            vkCmdPushConstants(c, g_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(sc), &sc);

            VkViewport viewport{ 0, 0, float(g_pass.extent.width), float(g_pass.extent.height), 0.0f, 1.0f };
            vkCmdSetViewport(c, 0, 1, &viewport);
            vkCmdSetScissor(c, 0, 1, &scissor);
            // The states above are dynamic in every pipeline this draws with, but setting one a
            // pipeline does not use is counted as drawing with a state the pipeline states
            // statically (VUID-vkCmdDraw-None-08608) - stencil only when the pipeline's stencil test
            // is on (a depth buffer without it has no stencil state at all), depth bias only when
            // the rasterizer enables it. The constant blend color is always worth setting: any
            // attachment of a draw may blend with it.
            if (depth && s.stencil)
            {
                vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_BIT, s.front.ref);
                vkCmdSetStencilReference(c, VK_STENCIL_FACE_BACK_BIT, s.stencilBackface ? s.back.ref : s.front.ref);
                vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_BIT, s.front.mask);
                vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_BACK_BIT, s.stencilBackface ? s.back.mask : s.front.mask);
                vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_BIT, s.front.writeMask);
                vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_BACK_BIT, s.stencilBackface ? s.back.writeMask : s.front.writeMask);
            }
            float blend[4] = { regFloat(RB_BLEND_RED), regFloat(RB_BLEND_RED + 1), regFloat(RB_BLEND_RED + 2), regFloat(RB_BLEND_RED + 3) };
            vkCmdSetBlendConstants(c, blend);
            if (s.polyOffsetFront || s.polyOffsetBack)
            {
                // Polygon offset as Direct3D 9 sets it (issue #54). The constant is in depth-range
                // units (the match's shadow maps use 0.00025), and the slope scale is in 1/16-pixel
                // units (they use 16, i.e. 1; Xenia's kPolygonOffsetScaleSubpixelUnit). Vulkan's
                // constant factor counts the format's minimum resolvable difference instead:
                // 2^(e - 23) for our D32_SFLOAT depth, 2^-24 for depths in [0.5, 1), where most of a
                // perspective scene is, and D24's unit everywhere. Depth slopes are per host pixel:
                // they get smaller at a higher resolution scale, so the slope factor grows with it.
                vkCmdSetDepthBias(c, s.polyOffset * 16777216.0f, 0.0f, s.polyOffsetScale / 16.0f * float(g_scale));
            }

            if (ib.ok)
            {
                vkCmdBindIndexBuffer(c, f.upload, ib.offset, ib.type);
                vkCmdDrawIndexed(c, ib.count, 1, 0, 0, 0);
            }
            else
                vkCmdDraw(c, s.indexCount, 1, 0, 0);
            if (memExport && g_exportMap)
                for (auto [start, bytes] : exportRanges) std::memcpy(g_exportMap + start, mem::physical(start), bytes);
            if (memExport)
            {
                // Make the shader's writes to guest memory visible to the host and to later commands.
                // No submit and wait here (#73: that was one GPU round trip per export draw, dozens per
                // frame): the ranges are pending like resolve writebacks, so syncGuestMemory() copies
                // them out when the guest can look (fences, interrupts, memory writes), later draws
                // fetch vertices from them GPU to GPU, and textures or indices from them sync first.
                endPass();
                VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT;
                vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
                g_perf.exportDraws++;
                static const bool waitPerExport = getenv("BB_EXPORT_WAIT") != nullptr; // the old way, for A/B runs
                if (waitPerExport)
                {
                    g_perf.flushExport++;
                    flushAndWait();
                    if (g_exportMap)
                        for (auto [start, bytes] : exportRanges) std::memcpy(mem::physical(start), g_exportMap + start, bytes);
                }
                else
                    for (auto [start, bytes] : exportRanges)
                    {
                        g_pendingWrites.push_back({ start, start + bytes, 0, true });
                    }
                g_drawsSinceSubmit++;
                maybeSubmitChunk(); // the pass is ended here anyway
            }
            for (int i = 0; i < 4; i++) markWritten(color[i], edramRect);
            if (depth && (s.depthWrite || s.stencil)) markWritten(depth, edramRect);
            g_drawsInFrame++;
            if (!memExport) g_drawsSinceSubmit++;
            return true;
        }

        void draw(uint32_t initiator, uint32_t indexBase, uint32_t indexSizeWord)
        {
            DrawState s = captureDrawState(initiator);
            if (s.edramMode == 6) { resolve(s); maybeSubmitChunk(); return; }
            if (s.edramMode != 4 && s.edramMode != 5) return;
            if (!s.indexCount) return;
            if (drawOnce(s, indexBase, indexSizeWord)) return;
            if (g_syncRetry)
            {
                // Textures or indices from a memory export still pending on the GPU: sync, record again
                g_perf.syncRetries++;
                syncGuestMemory("retry");
                if (drawOnce(s, indexBase, indexSizeWord)) return;
                if (g_syncRetry) { warnOnce("a draw still reads pending memory export output after a sync: skipped"); return; }
            }
            // Upload buffer full: submit what we have and retry once in a fresh one
            static bool logged = false;
            if (!logged) { info("[render] upload buffer full after %llu draws without a swap: submitting early", (unsigned long long)g_drawsInFrame); logged = true; }
            submitAndAdvance();
            if (!drawOnce(s, indexBase, indexSizeWord)) warnOnce("a draw needs more than the whole upload buffer: skipped");
        }

        void writeScreenshot(VkBuffer buffer, VkDeviceMemory memory, VkFormat format, uint32_t w, uint32_t h, const std::string& path,
                             const std::string& note)
        {
            void* p = nullptr;
            VK_CHECK(vkMapMemory(D, memory, 0, VkDeviceSize(w) * h * 4, 0, &p));
            std::vector<uint8_t> rgba((uint8_t*)p, (uint8_t*)p + size_t(w) * h * 4);
            vkUnmapMemory(D, memory);
            if (format == VK_FORMAT_B8G8R8A8_UNORM)
                for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
            std::filesystem::create_directories("screenshots");
            if (debug::writePng(path, rgba.data(), w, h)) info("[render] screenshot %s (%ux%u%s)", path.c_str(), w, h, note.c_str());
            else warn("[render] could not write %s", path.c_str());
            vkDestroyBuffer(D, buffer, nullptr);
            vkFreeMemory(D, memory, nullptr);
        }

        // BB_DEBUG_DUMP (debug_aids.h), on screenshot frames: read the 8_8_8_8 resolve image at that
        // address back now (this waits for the GPU) and save it
        void dumpResolveImage(uint64_t frameNumber)
        {
            uint32_t address = debugaids::dumpAddress();
            if (!address) return;
            auto it = g_resolved.find(address);
            if (it == g_resolved.end() || it->second.format != VK_FORMAT_R8G8B8A8_UNORM)
            {
                info("[debug] BB_DEBUG_DUMP: no 8_8_8_8 resolve image at %08X at frame %llu%s", address, (unsigned long long)frameNumber,
                     it != g_resolved.end() ? " (the one there has another format)" : "");
                return;
            }
            Image& r = it->second;
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = VkDeviceSize(r.width) * r.height * 4;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VkBuffer buffer;
            VK_CHECK(vkCreateBuffer(D, &bci, nullptr, &buffer));
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(D, buffer, &req);
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = C->findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkDeviceMemory memory;
            VK_CHECK(vkAllocateMemory(D, &mai, nullptr, &memory));
            VK_CHECK(vkBindBufferMemory(D, buffer, memory, 0));
            endPass();
            VkCommandBuffer c = cmd();
            transition(c, r, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { r.width, r.height, 1 };
            vkCmdCopyImageToBuffer(c, r.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
            VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
            flushAndWait();
            void* p = nullptr;
            VK_CHECK(vkMapMemory(D, memory, 0, bci.size, 0, &p));
            debugaids::writeDump(frameNumber, (const uint8_t*)p, r.width, r.height, r.swapRB);
            vkUnmapMemory(D, memory);
            vkDestroyBuffer(D, buffer, nullptr);
            vkFreeMemory(D, memory, nullptr);
        }
    }

    void disable() { g_disabled = true; }
    void setResolutionScale(uint32_t scale) { g_scale = std::clamp(scale, 1u, 4u); }
    void setAnisotropy(uint32_t n) { g_forcedAniso = std::clamp(n, 1u, 16u); }
    void setShaderCache(bool on) { g_shaderCacheOff = !on; }
    void requestScreenshot(uint64_t frame) { g_screenshots.insert(frame); }
    void requestScreenshotAt(double seconds, const std::string& label) { g_timedScreenshots.emplace(seconds, label); }

    void addCommandWaitMs(double ms) { g_perf.cpWaitMs += ms; }

    void submitBeforeWait()
    {
        if (g_disabled || g_failed || !D || !frame().recording || !g_drawsSinceSubmit) return;
        g_perf.idleSubmits++;
        submitChunk();
    }

    void syncGuestMemory(const char* why)
    {
        if (g_pendingWrites.empty()) return;
        g_perf.syncsBy[why]++;
        g_perf.flushSync++;
        flushAndWait(); // the frame being recorded
        double w0 = nowMs();
        for (Frame& f : g_frames) // and frames submitted at swaps, still running (with their early submits)
        {
            if (f.submitted) VK_CHECK(vkWaitForFences(D, 1, &f.fence, VK_TRUE, UINT64_MAX));
            waitChunks(f);
        }
        g_perf.waitMs += nowMs() - w0;
        retire(0, true);
        g_writebackSyncs++;
    }

    bool asyncFences() { return !g_disabled && !g_failed && D && g_timeline; }

    uint64_t fenceSerial()
    {
        if (!asyncFences()) return 0;
        retireCompleted();
        if (g_pendingWrites.empty()) return 0;
        for (const PendingWrite& w : g_pendingWrites)
            if (!w.serial) { submitChunk(); break; } // still being recorded: hand it to the GPU now
        uint64_t serial = 0;
        for (const PendingWrite& w : g_pendingWrites)
        {
            if (!w.serial) { syncGuestMemory("untagged"); return 0; } // cannot happen: a submit tags every range
            serial = std::max(serial, w.serial);
        }
        g_perf.asyncFences++;
        return serial;
    }

    uint64_t gpuDone()
    {
        if (!asyncFences()) return UINT64_MAX;
        uint64_t done = gpuProgress();
        retire(done);
        return done;
    }

    void waitGpu(uint64_t serial)
    {
        if (!asyncFences() || gpuProgress() >= serial) return;
        double w0 = nowMs();
        VkSemaphoreWaitInfo wi{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        wi.semaphoreCount = 1;
        wi.pSemaphores = &g_timeline;
        wi.pValues = &serial;
        VK_CHECK(vkWaitSemaphores(D, &wi, UINT64_MAX));
        g_perf.waitMs += nowMs() - w0;
        g_perf.asyncWaits++;
    }

    void onShaderLoad(uint32_t type, const uint32_t* words, uint32_t dwords, uint64_t h)
    {
        if (g_disabled || type > 1) return;
        auto [it, inserted] = g_shaders.try_emplace(h);
        if (inserted)
        {
            it->second.type = type ? ShaderType::Pixel : ShaderType::Vertex;
            it->second.words.resize(dwords);
            for (uint32_t i = 0; i < dwords; i++) it->second.words[i] = __builtin_bswap32(words[i]);
        }
        g_current[type] = h;
    }

    void onDraw(uint32_t initiator, uint32_t indexBase, uint32_t indexSizeWord)
    {
        if (!ready()) return;
        draw(initiator, indexBase, indexSizeWord);
    }

    void onSwap(uint32_t frontBuffer, uint32_t width, uint32_t height, uint64_t frameNumber)
    {
        if (!ready()) return;
        {
            static double lastLog = 0;
            double t = nowMs();
            CompileStats& cs = g_compileStats;
            if ((cs.translated || cs.compiled || cs.cached || cs.pipelines) && t - lastLog >= 1000.0)
            {
                info("[render] first use: %u shaders translated (%.0f ms), %u compiled by shaderc (%.0f ms), %u from the shader cache, "
                     "%u pipelines (%.0f ms)", cs.translated, cs.translateMs, cs.compiled, cs.compileMs, cs.cached, cs.pipelines, cs.pipelineMs);
                cs = {};
                lastLog = t;
            }
        }
        {
            static const bool perfOn = getenv("BB_PERF") != nullptr;
            static double perfLast = 0, cpuLast = 0;
            static uint64_t swaps = 0;
            swaps++;
            if (perfOn)
            {
                double t = nowMs();
                timespec ts;
                clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
                double cpu = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
                if (perfLast == 0) { perfLast = t; cpuLast = cpu; }
                else if (t - perfLast >= 1000.0)
                {
                    PerfStats& p = g_perf;
                    info("[perf] %.1fs: %llu swaps, %llu draws, renderer thread CPU %.0f ms (of it polling WAIT_REG_MEM %.0f ms), GPU waits %.0f ms (%llu flushes), "
                         "textures hashed %.1f MB in %.0f ms, %llu uploads (%.1f MB), %llu resolves, %llu writeBack calls (%llu queued); "
                         "%llu memexport draws (%llu waited for), %llu syncs with pending writes, %llu GPU vertex copies, %llu sync retries, "
                         "%llu early submits (%llu while the CP waited), %llu fences written when the GPU got there (%llu waited for), "
                         "%llu writebacks over pending ones",
                         (t - perfLast) / 1000.0, (unsigned long long)swaps, (unsigned long long)p.draws, cpu - cpuLast, p.cpWaitMs, p.waitMs,
                         (unsigned long long)p.flushes, p.hashBytes / 1048576.0, p.hashMs, (unsigned long long)p.uploads,
                         p.uploadBytes / 1048576.0, (unsigned long long)p.resolves, (unsigned long long)p.writebacks,
                         (unsigned long long)p.pendingAdded, (unsigned long long)p.exportDraws, (unsigned long long)p.flushExport, (unsigned long long)p.flushSync,
                         (unsigned long long)p.gpuVertexCopies, (unsigned long long)p.syncRetries, (unsigned long long)p.chunks,
                         (unsigned long long)p.idleSubmits, (unsigned long long)p.asyncFences, (unsigned long long)p.asyncWaits,
                         (unsigned long long)p.copyInsAroundPending);
                    if (!p.syncsBy.empty() || !p.uploadsBy.empty())
                    {
                        std::string why, ups;
                        for (auto& [k, n] : p.syncsBy) why += (why.empty() ? "" : ", ") + k + " " + std::to_string(n);
                        std::vector<std::pair<uint32_t, PerfStats::Up>> top(p.uploadsBy.begin(), p.uploadsBy.end());
                        std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second.bytes > b.second.bytes; });
                        for (size_t i = 0; i < top.size() && i < 6; i++)
                        {
                            char b[160];
                            snprintf(b, sizeof(b), "%s%08X fmt %u %ux%u x%llu (%.1f MB%s)", i ? ", " : "", top[i].first, top[i].second.format,
                                     top[i].second.width, top[i].second.height, (unsigned long long)top[i].second.count,
                                     top[i].second.bytes / 1048576.0, top[i].second.fresh ? ", first" : "");
                            ups += b;
                        }
                        info("[perf]   syncs by trigger: %s | uploads (%zu textures): %s", why.empty() ? "-" : why.c_str(), p.uploadsBy.size(),
                             ups.empty() ? "-" : ups.c_str());
                    }
                    p = {};
                    swaps = 0;
                    perfLast = t;
                    cpuLast = cpu;
                }
            }
        }
        g_depthWrittenGeometry.clear(); // geometry keys are per frame
        endPass();
        Frame& f = frame();
        auto it = g_resolved.find(frontBuffer & 0x1FFFFFFF);
        Image* front = it != g_resolved.end() ? &it->second : nullptr;
        uint32_t w = front ? std::min(width * g_scale, front->width) : 0, h = front ? std::min(height * g_scale, front->height) : 0;
        if (!front) warnOnce("swap of a front buffer the renderer did not produce: showing guest memory");
        if (front) transition(cmd(), *front, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); // presenter reads it in this layout
        debugaids::onSwap(frameNumber); // BB_DEBUG_WATCH

        // Screenshot: copy the front buffer image into a host-visible buffer in this submission
        VkBuffer shot = VK_NULL_HANDLE;
        VkDeviceMemory shotMemory = VK_NULL_HANDLE;
        std::string shotPath, shotNote;
        if (g_screenshots.erase(frameNumber))
            shotPath = "screenshots/frame_" + std::to_string(frameNumber) + ".png";
        else if (!g_timedScreenshots.empty())
        {
            // One timed screenshot per swap: if two times fall within one frame, the next swap takes the second
            double t = hid::secondsSinceLaunch();
            if (t >= g_timedScreenshots.begin()->first)
            {
                shotPath = "screenshots/t_" + g_timedScreenshots.begin()->second + ".png";
                char note[64];
                snprintf(note, sizeof note, ", frame %llu at %.2f s", (unsigned long long)frameNumber, t);
                shotNote = note;
                g_timedScreenshots.erase(g_timedScreenshots.begin());
            }
        }
        if (!shotPath.empty()) dumpResolveImage(frameNumber); // BB_DEBUG_DUMP
        if (!shotPath.empty() && front && w && h)
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = VkDeviceSize(w) * h * 4;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VK_CHECK(vkCreateBuffer(D, &bci, nullptr, &shot));
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(D, shot, &req);
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = C->findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VK_CHECK(vkAllocateMemory(D, &mai, nullptr, &shotMemory));
            VK_CHECK(vkBindBufferMemory(D, shot, shotMemory, 0));
            VkCommandBuffer c = cmd();
            transition(c, *front, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { w, h, 1 };
            vkCmdCopyImageToBuffer(c, front->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot, 1, &copy);
            VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        }

        Frame& submitted = f;
        submitAndAdvance(); // also waits for the frame in flight before it
        if (front) video::publishRenderedFrame(front->image, w, h, frameNumber);
        if (shot)
        {
            if (submitted.submitted) VK_CHECK(vkWaitForFences(D, 1, &submitted.fence, VK_TRUE, UINT64_MAX));
            writeScreenshot(shot, shotMemory, front->format, w, h, shotPath, shotNote);
        }
        g_frameSerial++;
        g_drawsInFrame = 0;
        if (frameNumber % 300 == 0)
            info("[render] textures: %llu uploads, %llu unchanged and kept so far", (unsigned long long)g_texturesUploaded,
                 (unsigned long long)g_texturesKept);
        if (frameNumber % 300 == 0 && g_writebacks)
            info("[render] resolve writeback: %llu resolves written to guest memory, %llu kept on the GPU, %llu syncs so far",
                 (unsigned long long)g_writebacks, (unsigned long long)g_writebacksSkipped, (unsigned long long)g_writebackSyncs);
    }
}
