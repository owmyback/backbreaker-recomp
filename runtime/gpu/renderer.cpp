// Vulkan renderer: the first-picture path of roadmap phase 4. Simplifications (see the ROADMAP):
// - EDRAM: one Vulkan image per (base tile, pitch, samples, format); no aliasing between them.
// - Resolves stay on the GPU: the destination is a Vulkan image keyed by its guest address
//   (nothing is written back to guest memory yet). The copy rectangle is the window scissor.
// - Guest data is snapshotted at draw time into a per-frame upload buffer: vertex data (the
//   vertex fetch constants are rewritten to point into it), constants, indices, textures.
// - Textures: 2D linear and tiled (runtime/gpu/tiling.h) 8, 8_8, 8_8_8_8, 4_4_4_4, 5_6_5 and DXT
//   formats, base level only, re-uploaded once per frame they are used. Other textures get a
//   magenta placeholder.
// Register meanings follow Xenia (BSD; docs/licenses/xenia-LICENSE.txt).
#include "renderer.h"
#include "gpu.h"
#include "shader_translator.h"
#include "state.h"
#include "tiling.h"
#include "../debug/png.h"
#include "../log.h"
#include "../memory.h"
#include "../video/presenter.h"
#include "../video/vk_context.h"

#include <shaderc/shaderc.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace gpu::render
{
    namespace
    {
        constexpr uint32_t kAluConstants = 0x4000, kFetchConstants = 0x4800, kBoolConstants = 0x4900, kLoopConstants = 0x4908;
        constexpr uint32_t RB_COPY_CONTROL = 0x2318, RB_COPY_DEST_BASE = 0x2319, RB_COPY_DEST_PITCH = 0x231A;
        constexpr uint32_t RB_DEPTH_CLEAR = 0x231D, RB_COLOR_CLEAR = 0x231E, RB_BLEND_RED = 0x2105;
        constexpr uint32_t kFramesInFlight = 2;
        constexpr VkDeviceSize kUploadSize = 96ull << 20; // per frame in flight
        constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT; // D24S8 is not available on AMD
        constexpr uint32_t kConstantsSize = 4096 * 2 + 928;           // vs, ps, then bool/loop/fetch

        bool g_disabled = false, g_failed = false;
        // Internal resolution scale: render targets and everything drawn into them are this many
        // times larger than the guest's (the game still sees 1280x720)
        uint32_t g_scale = 1;
        VkRect2D scaled(VkRect2D r)
        {
            return { { r.offset.x * int32_t(g_scale), r.offset.y * int32_t(g_scale) }, { r.extent.width * g_scale, r.extent.height * g_scale } };
        }
        vkctx::Context* C = nullptr;
        VkDevice D = VK_NULL_HANDLE;
        std::set<uint64_t> g_screenshots;

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
            TranslatedShader t;
            VkShaderModule module = VK_NULL_HANDLE; // null if translation or compilation failed
        };
        std::unordered_map<uint64_t, Shader> g_shaders;
        uint64_t g_current[2] = { 0, 0 }; // vertex, pixel
        shaderc_compiler_t g_compiler = nullptr;
        shaderc_compile_options_t g_options = nullptr;
        VkShaderModule g_rectGs = VK_NULL_HANDLE;

        // Same hash as the frame capture, so names match the shader dump (vs_/ps_<hash>.bin)
        uint64_t fnv1a(const uint32_t* w, uint32_t n)
        {
            uint64_t h = 1469598103934665603ull;
            for (uint32_t i = 0; i < n; i++)
                for (int b = 0; b < 4; b++) h = (h ^ ((w[i] >> (b * 8)) & 0xFF)) * 1099511628211ull;
            return h;
        }

        VkShaderModule compile(const std::string& glsl, shaderc_shader_kind kind, const char* name, std::string& error)
        {
            shaderc_compilation_result_t r = shaderc_compile_into_spv(g_compiler, glsl.data(), glsl.size(), kind, name, "main", g_options);
            VkShaderModule m = VK_NULL_HANDLE;
            if (shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success)
            {
                VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
                ci.codeSize = shaderc_result_get_length(r);
                ci.pCode = (const uint32_t*)shaderc_result_get_bytes(r);
                VK_CHECK(vkCreateShaderModule(D, &ci, nullptr, &m));
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
                s.t = translateToGlsl(s.words, s.type);
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
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            uint32_t width = 0, height = 0, samples = 1;
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        };

        Image createImage(VkImageType type, VkImageViewType viewType, VkFormat format, uint32_t w, uint32_t h, uint32_t depth,
                          uint32_t layers, uint32_t samples, VkImageUsageFlags usage, VkImageCreateFlags flags = 0)
        {
            Image img;
            img.format = format;
            img.width = w;
            img.height = h;
            img.samples = samples;
            img.aspect = format == kDepthFormat ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            ici.flags = flags;
            ici.imageType = type;
            ici.format = format;
            ici.extent = { w, h, depth };
            ici.mipLevels = 1;
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
            vci.subresourceRange = { img.aspect, 0, 1, 0, layers };
            VK_CHECK(vkCreateImageView(D, &vci, nullptr, &img.view));
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

        struct Frame
        {
            VkCommandPool pool = VK_NULL_HANDLE;
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
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
            VkExtent2D extent{};
        } g_pass;

        void endPass()
        {
            if (!g_pass.active) return;
            vkCmdEndRendering(frame().cmd);
            g_pass.active = false;
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
            VkRenderingInfo ri{ VK_STRUCTURE_TYPE_RENDERING_INFO };
            ri.renderArea = { { 0, 0 }, { w, h } };
            ri.layerCount = 1;
            ri.colorAttachmentCount = 4;
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
                VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
                si.commandBufferCount = 1;
                si.pCommandBuffers = &f.cmd;
                std::lock_guard guard(C->queueLock);
                VK_CHECK(vkQueueSubmit(C->queue, 1, &si, f.fence));
                f.submitted = true;
            }
            g_frameIndex = (g_frameIndex + 1) % kFramesInFlight;
            Frame& next = frame();
            if (next.submitted)
            {
                VK_CHECK(vkWaitForFences(D, 1, &next.fence, VK_TRUE, UINT64_MAX));
                next.submitted = false;
            }
            VK_CHECK(vkResetFences(D, 1, &next.fence));
            VK_CHECK(vkResetCommandPool(D, next.pool, 0));
            next.used = 0;
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
            VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                      (k.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
            Image img = createImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, format, k.pitch * g_scale, height * g_scale, 1, 1, k.samples, usage);
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

        // Resolve destinations, by guest physical address (the latest image written there).
        // Replaced images are kept alive: the presenter may still be showing them.
        std::map<uint32_t, Image> g_resolved;
        std::vector<Image> g_retired;

        // ---------------------------------------------------------------- textures

        // `convert`: 16-bit formats expanded to RGBA8 on upload (component X = lowest bits -> R)
        enum class Convert { None, From4444, From565 };
        struct TextureFormat { VkFormat vk; uint32_t blockW, blockH, bytes; Convert convert = Convert::None; };
        TextureFormat textureFormat(uint32_t f) // xenos::TextureFormat
        {
            switch (f)
            {
            case 2: return { VK_FORMAT_R8_UNORM, 1, 1, 1 };            // 8
            case 4: return { VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 2, Convert::From565 };  // 5_6_5
            case 15: return { VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 2, Convert::From4444 }; // 4_4_4_4
            case 6: return { VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 4 };      // 8_8_8_8
            case 10: return { VK_FORMAT_R8G8_UNORM, 1, 1, 2 };         // 8_8
            case 18: return { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 4, 8 }; // DXT1
            case 19: return { VK_FORMAT_BC2_UNORM_BLOCK, 4, 4, 16 };   // DXT2_3
            case 20: return { VK_FORMAT_BC3_UNORM_BLOCK, 4, 4, 16 };   // DXT4_5
            default: return { VK_FORMAT_UNDEFINED, 1, 1, 0 };
            }
        }

        struct TextureKey
        {
            uint32_t base, format, endian, width, height, pitch;
            auto operator<=>(const TextureKey&) const = default;
        };
        struct Texture
        {
            Image img;
            uint64_t uploadedFrame = 0;
            std::map<uint32_t, VkImageView> views; // by fetch constant swizzle
        };
        std::map<TextureKey, Texture> g_textures;
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

        VkSampler sampler(const uint32_t d[6])
        {
            uint32_t clamp = (d[0] >> 10) & 0x1FF, filters = (d[3] >> 19) & 0x3F;
            uint64_t key = clamp | (uint64_t(filters) << 9);
            auto it = g_samplers.find(key);
            if (it != g_samplers.end()) return it->second;
            static const VkSamplerAddressMode modes[8] = {
                VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE };
            VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            si.magFilter = (filters & 3) == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            si.minFilter = ((filters >> 2) & 3) == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            si.mipmapMode = ((filters >> 4) & 3) == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            si.addressModeU = modes[clamp & 7];
            si.addressModeV = modes[(clamp >> 3) & 7];
            si.addressModeW = modes[(clamp >> 6) & 7];
            si.maxLod = 0.0f; // base level only for now
            si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            VkSampler s;
            VK_CHECK(vkCreateSampler(D, &si, nullptr, &s));
            return g_samplers[key] = s;
        }

        // Descriptor for texture fetch constant `slot` as a shader declaring dimension `dim`.
        // May record upload commands (ends the current pass).
        VkDescriptorImageInfo texture(uint32_t slot, uint32_t dim)
        {
            uint32_t d[6];
            for (int i = 0; i < 6; i++) d[i] = readRegister(kFetchConstants + slot * 6 + i);
            VkDescriptorImageInfo out{ sampler(d), placeholder(dim).view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            uint32_t format = d[1] & 0x3F, endian = (d[1] >> 6) & 3, base = d[1] & 0xFFFFF000, fetchDim = (d[5] >> 9) & 3;
            bool tiled = d[0] >> 31;
            uint32_t pitch = ((d[0] >> 22) & 0x1FF) * 32;
            uint32_t width = (d[2] & 0x1FFF) + 1, height = ((d[2] >> 13) & 0x1FFF) + 1;
            TextureFormat tf = textureFormat(format);
            if ((d[0] & 3) != 2 || fetchDim != 1 || dim != 1)
            {
                warnOnce("texture dimension " + std::to_string(fetchDim) + " (shader " + std::to_string(dim) + ") not supported yet");
                return out;
            }
            if (tf.vk == VK_FORMAT_UNDEFINED || !pitch)
            {
                warnOnce("texture format " + std::to_string(format) + (tiled ? " (tiled)" : " (linear)") +
                         " not supported yet: magenta placeholder");
                return out;
            }
            TextureKey key{ base, format | (tiled ? 0x100u : 0u), endian, width, height, pitch };
            Texture& t = g_textures[key];
            if (!t.img.image)
                t.img = createImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, tf.vk, width, height, 1, 1, 1,
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            if (t.uploadedFrame != g_frameSerial)
            {
                // Guest layout -> row-major blocks (byte-swapped), `rowBlocks` blocks per row.
                // Linear: rows of `pitch` texels (blocks for compressed formats), 256-byte aligned.
                // Tiled: 32x32-block tiles (runtime/gpu/tiling.h), pitch aligned to 32 blocks.
                uint32_t blocksW = (width + tf.blockW - 1) / tf.blockW, blocksH = (height + tf.blockH - 1) / tf.blockH;
                uint32_t rowBlocks, srcBytes;
                if (tiled)
                {
                    rowBlocks = tiledBlocksAligned(std::max(pitch / tf.blockW, blocksW));
                    srcBytes = tiledUpperBound2D(blocksW, blocksH, rowBlocks, tf.bytes);
                }
                else
                {
                    uint32_t rowBytes = ((pitch / tf.blockW) * tf.bytes + 255) & ~255u;
                    rowBlocks = rowBytes / tf.bytes;
                    srcBytes = rowBytes * blocksH;
                }
                srcBytes = (srcBytes + 3) & ~3u;
                if (base + uint64_t(srcBytes) > mem::kPhysicalSize)
                {
                    warnOnce("texture extends past physical memory: placeholder");
                    return out;
                }
                static std::vector<uint8_t> swapped, rows;
                swapped.resize(srcBytes);
                const uint32_t* src = (const uint32_t*)mem::physical(base);
                for (uint32_t i = 0; i < srcBytes / 4; i++) ((uint32_t*)swapped.data())[i] = gpuSwap(src[i], endian);
                const uint8_t* linear = swapped.data();
                if (tiled)
                {
                    rows.assign(size_t(rowBlocks) * blocksH * tf.bytes, 0);
                    untile2D(swapped.data(), rows.data(), blocksW, blocksH, rowBlocks, tf.bytes);
                    linear = rows.data();
                }
                uint32_t outBytes = tf.convert == Convert::None ? tf.bytes : 4;
                VkDeviceSize size = VkDeviceSize(rowBlocks) * blocksH * outBytes;
                VkDeviceSize off = uploadAlloc(size);
                if (off == UINT64_MAX) return out;
                uint8_t* dst = frame().uploadMap + off;
                if (tf.convert == Convert::None)
                    std::memcpy(dst, linear, size);
                else
                    for (size_t i = 0; i < size_t(rowBlocks) * blocksH; i++)
                    {
                        uint16_t v = ((const uint16_t*)linear)[i];
                        uint8_t* o = dst + i * 4;
                        if (tf.convert == Convert::From4444)
                            for (int c = 0; c < 4; c++) o[c] = uint8_t(((v >> (4 * c)) & 0xF) * 17);
                        else
                        {
                            o[0] = uint8_t((v & 0x1F) * 255 / 31);
                            o[1] = uint8_t(((v >> 5) & 0x3F) * 255 / 63);
                            o[2] = uint8_t((v >> 11) * 255 / 31);
                            o[3] = 255;
                        }
                    }
                endPass();
                VkCommandBuffer c = cmd();
                transition(c, t.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy copy{};
                copy.bufferOffset = off;
                copy.bufferRowLength = rowBlocks * tf.blockW;
                copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                copy.imageExtent = { width, height, 1 };
                vkCmdCopyBufferToImage(c, frame().upload, t.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                transition(c, t.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                t.uploadedFrame = g_frameSerial;
            }
            uint32_t swizzle = (d[3] >> 1) & 0xFFF;
            VkImageView& view = t.views[swizzle];
            if (!view)
            {
                static const VkComponentSwizzle comps[8] = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
                                                             VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE,
                                                             VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO };
                VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                vci.image = t.img.image;
                vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vci.format = tf.vk;
                vci.components = { comps[swizzle & 7], comps[(swizzle >> 3) & 7], comps[(swizzle >> 6) & 7], comps[(swizzle >> 9) & 7] };
                vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                VK_CHECK(vkCreateImageView(D, &vci, nullptr, &view));
            }
            out.imageView = view;
            return out;
        }

        // ---------------------------------------------------------------- pipelines

        VkDescriptorSetLayout g_setLayouts[2];
        VkPipelineLayout g_layout = VK_NULL_HANDLE;
        std::unordered_map<std::string, VkPipeline> g_pipelines;

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

        struct PipelineDesc
        {
            const DrawState* s;
            Shader *vs, *ps;
            VkPrimitiveTopology topology;
            bool rectangles;
            VkFormat colors[4];
            bool depth;
            uint32_t samples;
        };

        VkPipeline pipeline(const PipelineDesc& p)
        {
            const DrawState& s = *p.s;
            bool strip = p.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || p.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                         p.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
            bool restart = strip && s.primitiveRestart;
            bool cullable = !p.rectangles && p.topology >= VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            char key[512];
            int n = snprintf(key, sizeof(key), "%p %p %d %d %d|%d %d %d %d %d|%u|", (void*)p.vs->module, (void*)p.ps->module, int(p.topology),
                             p.rectangles, restart, p.colors[0], p.colors[1], p.colors[2], p.colors[3], p.depth, p.samples);
            for (int i = 0; i < 4; i++)
            {
                const ColorTarget& t = s.color[i];
                n += snprintf(key + n, sizeof(key) - n, "%x %u %u %u %u %u %u|", p.colors[i] ? t.writeMask : 0, t.blend.colorSrc, t.blend.colorDst,
                              t.blend.colorOp, t.blend.alphaSrc, t.blend.alphaDst, t.blend.alphaOp);
            }
            snprintf(key + n, sizeof(key) - n, "%d %d %u %d %d %u %u %u %u %u %u %u %u|%d %d %d %d %d %d %d %d", p.depth && s.depthTest,
                     p.depth && s.depthWrite, s.depthFunc, p.depth && s.stencil, s.stencilBackface, s.front.func, s.front.fail, s.front.zpass,
                     s.front.zfail, s.back.func, s.back.fail, s.back.zpass, s.back.zfail, cullable && s.cullFront, cullable && s.cullBack,
                     s.frontFaceCW, s.polyMode == 1 && s.polyFront == 1, s.polyOffsetFront || s.polyOffsetBack, s.clipDisable, s.alphaToMask,
                     p.samples);
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
            stage(VK_SHADER_STAGE_FRAGMENT_BIT, p.ps->module);

            VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO }; // vertices fetched in the shader
            VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            ia.topology = p.topology;
            ia.primitiveRestartEnable = restart;
            VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            vp.viewportCount = 1;
            vp.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            rs.depthClampEnable = s.clipDisable;
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
            cb.attachmentCount = 4;
            cb.pAttachments = att;
            static const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                                  VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                                  VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BIAS };
            VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dy.dynamicStateCount = uint32_t(std::size(dyn));
            dy.pDynamicStates = dyn;
            VkPipelineRenderingCreateInfo rci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
            rci.colorAttachmentCount = 4;
            rci.pColorAttachmentFormats = p.colors;
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
            VK_CHECK(vkCreateGraphicsPipelines(D, VK_NULL_HANDLE, 1, &gci, nullptr, &pl));
            info("[render] pipeline %zu: %s", g_pipelines.size() + 1, pipelineKey(s).c_str());
            return g_pipelines[key] = pl;
        }

        // ---------------------------------------------------------------- setup

        bool ready()
        {
            if (g_disabled || g_failed) return false;
            if (D) return true;
            C = vkctx::get();
            if (!C) { g_failed = true; return false; }
            D = C->device;

            g_compiler = shaderc_compiler_initialize();
            g_options = shaderc_compile_options_initialize();
            shaderc_compile_options_set_target_env(g_options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
            shaderc_compile_options_set_optimization_level(g_options, shaderc_optimization_level_performance);
            std::string error;
            g_rectGs = compile(kRectangleGs, shaderc_geometry_shader, "rectangle_list.geom", error);
            if (!g_rectGs) fatal("[render] rectangle list geometry shader: %s", error.c_str());

            // set 0: guest data + constants (one set per frame in flight, constants by dynamic
            // offset); set 1: 32 textures, pushed per draw (a layout may have one push set only)
            VkDescriptorSetLayoutBinding b0[4]{};
            for (uint32_t i = 0; i < 4; i++)
                b0[i] = { i, i ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
            VkDescriptorSetLayoutBinding b1[32]{};
            for (uint32_t i = 0; i < 32; i++)
                b1[i] = { i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
            VkDescriptorSetLayoutCreateInfo dl{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dl.bindingCount = 4;
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
                VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
                VK_CHECK(vkCreateFence(D, &fci, nullptr, &f.fence));
                VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                bci.size = kUploadSize;
                bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
                VK_CHECK(vkCreateBuffer(D, &bci, nullptr, &f.upload));
                VkMemoryRequirements req;
                vkGetBufferMemoryRequirements(D, f.upload, &req);
                VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                mai.allocationSize = req.size;
                mai.memoryTypeIndex = C->findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                VK_CHECK(vkAllocateMemory(D, &mai, nullptr, &f.uploadMemory));
                VK_CHECK(vkBindBufferMemory(D, f.upload, f.uploadMemory, 0));
                VK_CHECK(vkMapMemory(D, f.uploadMemory, 0, kUploadSize, 0, (void**)&f.uploadMap));

                VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 } };
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
                VkDescriptorBufferInfo bufs[4] = { { f.upload, 0, kUploadSize }, { f.upload, 0, 4096 }, { f.upload, 0, 4096 }, { f.upload, 0, 928 } };
                VkWriteDescriptorSet w[4]{};
                for (uint32_t i = 0; i < 4; i++)
                {
                    w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    w[i].dstSet = f.buffers;
                    w[i].dstBinding = i;
                    w[i].descriptorCount = 1;
                    w[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    w[i].pBufferInfo = &bufs[i];
                }
                vkUpdateDescriptorSets(D, 4, w, 0, nullptr);
            }
            info("[render] Vulkan renderer ready");
            return true;
        }

        // ---------------------------------------------------------------- draws

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

        void resolve(const DrawState& s)
        {
            uint32_t control = readRegister(RB_COPY_CONTROL);
            uint32_t source = control & 7, command = (control >> 20) & 3;
            bool clearColor = (control >> 8) & 1, clearDepth = (control >> 9) & 1;
            uint32_t destBase = readRegister(RB_COPY_DEST_BASE) & 0x1FFFFFFF, destPitchReg = readRegister(RB_COPY_DEST_PITCH);
            uint32_t destPitch = destPitchReg & 0x3FFF, destHeight = (destPitchReg >> 16) & 0x3FFF;

            Image* color = source < 4 ? findTarget({ s.color[source].edramBase, s.surfacePitch, s.msaaSamples, s.color[source].format, false })
                                      : nullptr;
            Image* depth = findTarget({ s.depthBase, s.surfacePitch, s.msaaSamples, 0, true });
            Image* src = source < 4 ? color : depth;
            VkRect2D rect;
            VkExtent2D limit = src ? VkExtent2D{ src->width / g_scale, src->height / g_scale } : VkExtent2D{ 8192, 8192 };
            if (!scissorRect(s, limit, rect)) return; // guest pixels
            VkRect2D hostRect = scaled(rect);

            if (source == 4)
                warnOnce("depth resolves not supported yet");
            else if (color && command != 3 && destPitch && destHeight)
            {
                // Destination image for this guest address (same format as the source)
                uint32_t dw = std::max(destPitch, rect.offset.x + rect.extent.width), dh = std::max(destHeight, rect.offset.y + rect.extent.height);
                auto it = g_resolved.find(destBase);
                if (it != g_resolved.end() &&
                    (it->second.width != dw * g_scale || it->second.height != dh * g_scale || it->second.format != color->format))
                {
                    g_retired.push_back(it->second);
                    g_resolved.erase(it);
                    it = g_resolved.end();
                }
                if (it == g_resolved.end())
                {
                    it = g_resolved.emplace(destBase, createImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, color->format, dw * g_scale, dh * g_scale, 1, 1, 1,
                                                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                                      VK_IMAGE_USAGE_SAMPLED_BIT)).first;
                    info("[render] resolve destination %08X: %ux%u", destBase, dw, dh);
                }
                Image& dest = it->second;
                endPass();
                VkCommandBuffer c = cmd();
                transition(c, *color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                transition(c, dest, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkImageSubresourceLayers layers{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                VkOffset3D at{ hostRect.offset.x, hostRect.offset.y, 0 };
                VkExtent3D extent{ hostRect.extent.width, hostRect.extent.height, 1 };
                if (color->samples > 1)
                {
                    VkImageResolve r{ layers, at, layers, at, extent };
                    vkCmdResolveImage(c, color->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
                }
                else
                {
                    VkImageCopy r{ layers, at, layers, at, extent };
                    vkCmdCopyImage(c, color->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
                }
                transition(c, dest, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); // resting layout (presenter, screenshots)
            }

            // Clears after the copy, of the copied rectangle
            if (clearColor && color)
            {
                Image* targets[4] = { color, nullptr, nullptr, nullptr };
                beginPass(targets, nullptr);
                uint32_t v = readRegister(RB_COLOR_CLEAR);
                VkClearAttachment a{ VK_IMAGE_ASPECT_COLOR_BIT, 0, {} };
                a.clearValue.color = { { (v & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, ((v >> 16) & 0xFF) / 255.0f, (v >> 24) / 255.0f } };
                VkClearRect r{ hostRect, 0, 1 };
                vkCmdClearAttachments(frame().cmd, 1, &a, 1, &r);
            }
            if (clearDepth && (depth || (depth = target({ s.depthBase, s.surfacePitch, s.msaaSamples, 0, true }))))
            {
                Image* none[4] = {};
                beginPass(none, depth);
                uint32_t v = readRegister(RB_DEPTH_CLEAR);
                VkClearAttachment a{ VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, {} };
                a.clearValue.depthStencil = { (v >> 8) / float(0xFFFFFF), v & 0xFF };
                VkRect2D dr{ hostRect.offset, { std::min(hostRect.extent.width, depth->width - uint32_t(hostRect.offset.x)),
                                                std::min(hostRect.extent.height, depth->height - uint32_t(hostRect.offset.y)) } };
                VkClearRect r{ dr, 0, 1 };
                if (hostRect.offset.x < int32_t(depth->width) && hostRect.offset.y < int32_t(depth->height))
                    vkCmdClearAttachments(frame().cmd, 1, &a, 1, &r);
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
            std::vector<uint32_t> values; // only needed when expanding quads
            uint32_t endian = indexSizeWord >> 30;
            auto guestIndex = [&](uint32_t i) -> uint32_t {
                if (s.indexSize)
                    return gpuSwap(((const uint32_t*)mem::physical(indexBase))[i], endian);
                uint32_t word = gpuSwap(((const uint32_t*)mem::physical(indexBase & ~3u))[(i + ((indexBase >> 1) & 1)) / 2], endian);
                return ((i + ((indexBase >> 1) & 1)) & 1) ? word >> 16 : word & 0xFFFF;
            };
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
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t v = guestIndex(i);
                if (s.primitiveRestart && v == (s.restartIndex & restart)) v = restart;
                else { lo = std::min(lo, v); hi = std::max(hi, v); }
                if (s.indexSize) ((uint32_t*)out)[i] = v;
                else ((uint16_t*)out)[i] = uint16_t(v);
            }
            return { off, s.indexSize ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16, count, true, lo == UINT32_MAX ? 0 : lo, hi };
        }

        // false: the upload buffer ran out before anything of this draw was recorded
        bool drawOnce(const DrawState& s, uint32_t indexBase, uint32_t indexSizeWord)
        {
            g_outOfSpace = false;

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
            if (!depth && !color[0] && !color[1] && !color[2] && !color[3]) return true; // nothing to write (e.g. masked point draws)

            // Snapshot guest data into the upload buffer: constants, vertex data, indices
            Frame& f = frame();
            VkDeviceSize constants = uploadAlloc(kConstantsSize);
            if (constants == UINT64_MAX) return false;
            uint32_t* cv = (uint32_t*)(f.uploadMap + constants);
            for (uint32_t i = 0; i < 1024; i++)
            {
                cv[i] = readRegister(kAluConstants + std::min(s.vsConstBase * 4 + i, 2047u));
                cv[1024 + i] = readRegister(kAluConstants + std::min(s.psConstBase * 4 + i, 2047u));
            }
            uint32_t* other = cv + 2048;
            for (uint32_t i = 0; i < 8; i++) other[i] = readRegister(kBoolConstants + i);
            for (uint32_t i = 0; i < 32; i++) other[8 + i] = readRegister(kLoopConstants + i);
            uint32_t* fetch = other + 40;
            for (uint32_t i = 0; i < 192; i++) fetch[i] = readRegister(kFetchConstants + i);
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
                std::memcpy(f.uploadMap + off, mem::physical(uint32_t((baseDw + first) * 4)), dwords * 4);
                d0 = uint32_t(off / 4) - uint32_t(first); // shader: mem[d0 + index * stride] (mod 2^32)
            }

            // Textures (uploads end the current pass)
            VkDescriptorImageInfo images[32]{};
            uint32_t textureSlots = vs->t.textureSlots | ps->t.textureSlots;
            for (uint32_t slot = 0; slot < 32; slot++)
                if ((textureSlots >> slot) & 1)
                    images[slot] = texture(slot, ((ps->t.textureSlots >> slot) & 1) ? ps->t.textureDims[slot] : vs->t.textureDims[slot]);
            if (g_outOfSpace) return false;

            beginPass(color, depth);
            VkRect2D scissor;
            VkExtent2D guestExtent{ g_pass.extent.width / g_scale, g_pass.extent.height / g_scale };
            if (!scissorRect(s, guestExtent, scissor)) return true;
            scissor = scaled(scissor);
            VkCommandBuffer c = f.cmd;
            VkPipeline pl = pipeline({ &s, vs, ps, topology, rectangles, { formats[0], formats[1], formats[2], formats[3] }, depth != nullptr,
                                       s.msaaSamples });
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
            vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_BIT, s.front.ref);
            vkCmdSetStencilReference(c, VK_STENCIL_FACE_BACK_BIT, s.stencilBackface ? s.back.ref : s.front.ref);
            vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_BIT, s.front.mask);
            vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_BACK_BIT, s.stencilBackface ? s.back.mask : s.front.mask);
            vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_BIT, s.front.writeMask);
            vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_BACK_BIT, s.stencilBackface ? s.back.writeMask : s.front.writeMask);
            float blend[4] = { regFloat(RB_BLEND_RED), regFloat(RB_BLEND_RED + 1), regFloat(RB_BLEND_RED + 2), regFloat(RB_BLEND_RED + 3) };
            vkCmdSetBlendConstants(c, blend);
            vkCmdSetDepthBias(c, s.polyOffset, 0.0f, s.polyOffsetScale);

            if (ib.ok)
            {
                vkCmdBindIndexBuffer(c, f.upload, ib.offset, ib.type);
                vkCmdDrawIndexed(c, ib.count, 1, 0, 0, 0);
            }
            else
                vkCmdDraw(c, s.indexCount, 1, 0, 0);
            g_drawsInFrame++;
            return true;
        }

        void draw(uint32_t initiator, uint32_t indexBase, uint32_t indexSizeWord)
        {
            DrawState s = captureDrawState(initiator);
            if (s.edramMode == 6) { resolve(s); return; }
            if (s.edramMode != 4 && s.edramMode != 5) return;
            if (!s.indexCount) return;
            if (drawOnce(s, indexBase, indexSizeWord)) return;
            // Upload buffer full: submit what we have and retry once in a fresh one
            static bool logged = false;
            if (!logged) { info("[render] upload buffer full after %llu draws without a swap: submitting early", (unsigned long long)g_drawsInFrame); logged = true; }
            submitAndAdvance();
            if (!drawOnce(s, indexBase, indexSizeWord)) warnOnce("a draw needs more than the whole upload buffer: skipped");
        }

        void writeScreenshot(VkBuffer buffer, VkDeviceMemory memory, VkFormat format, uint32_t w, uint32_t h, uint64_t frameNumber)
        {
            void* p = nullptr;
            VK_CHECK(vkMapMemory(D, memory, 0, VkDeviceSize(w) * h * 4, 0, &p));
            std::vector<uint8_t> rgba((uint8_t*)p, (uint8_t*)p + size_t(w) * h * 4);
            vkUnmapMemory(D, memory);
            if (format == VK_FORMAT_B8G8R8A8_UNORM)
                for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
            std::filesystem::create_directories("screenshots");
            std::string path = "screenshots/frame_" + std::to_string(frameNumber) + ".png";
            if (debug::writePng(path, rgba.data(), w, h)) info("[render] screenshot %s (%ux%u)", path.c_str(), w, h);
            else warn("[render] could not write %s", path.c_str());
            vkDestroyBuffer(D, buffer, nullptr);
            vkFreeMemory(D, memory, nullptr);
        }
    }

    void disable() { g_disabled = true; }
    void setResolutionScale(uint32_t scale) { g_scale = std::clamp(scale, 1u, 4u); }
    void requestScreenshot(uint64_t frame) { g_screenshots.insert(frame); }

    void onShaderLoad(uint32_t type, const uint32_t* words, uint32_t dwords)
    {
        if (g_disabled || type > 1) return;
        uint64_t h = fnv1a(words, dwords);
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
        endPass();
        Frame& f = frame();
        auto it = g_resolved.find(frontBuffer & 0x1FFFFFFF);
        Image* front = it != g_resolved.end() ? &it->second : nullptr;
        uint32_t w = front ? std::min(width * g_scale, front->width) : 0, h = front ? std::min(height * g_scale, front->height) : 0;
        if (!front) warnOnce("swap of a front buffer the renderer did not produce: showing guest memory");

        // Screenshot: copy the front buffer image into a host-visible buffer in this submission
        VkBuffer shot = VK_NULL_HANDLE;
        VkDeviceMemory shotMemory = VK_NULL_HANDLE;
        if (g_screenshots.erase(frameNumber) && front && w && h)
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
            writeScreenshot(shot, shotMemory, front->format, w, h, frameNumber);
        }
        g_frameSerial++;
        g_drawsInFrame = 0;
    }
}
