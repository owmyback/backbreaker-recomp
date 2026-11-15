// In-window overlay: the achievements list and the unlock pop-up (issue #101). See overlay.h.
//
// GPU side, kept off the FPS-critical present path: nothing is created until something is first
// shown; then one RGBA8 image and one host-visible staging buffer the size of the swapchain
// image. A present that shows something records, at most, one buffer-to-image copy (only when
// the content changed) and one blit per visible panel (opaque, nearest filter, same size: a copy
// with format conversion). No extra submits, waits or allocations per present.
#include "overlay.h"
#include "vk_context.h"
#include "../hid/hid.h"
#include "../kernel/kernel.h"
#include "../log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace overlay
{
    namespace
    {
        using clock = std::chrono::steady_clock;
        constexpr double kToastSeconds = 6.0;
        constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kBack = 0x0020, kLb = 0x0100, kRb = 0x0200, kB = 0x2000, kGuide = 0x0400;

        // ---- what is shown ----
        bool g_listOpen = false;
        int g_selected = 0, g_top = 0, g_rowsVisible = 1;
        std::vector<kernel::AchievementView> g_list;
        uint64_t g_seenUnlocks = 0;
        bool g_toast = false;
        clock::time_point g_toastUntil;
        kernel::AchievementView g_toastItem;
        bool g_dirty = true;

        // ---- CPU drawing ----
        SDL_Surface* g_surface = nullptr;
        SDL_Renderer* g_sw = nullptr;
        std::map<const uint8_t*, SDL_Texture*> g_icons; // by PNG address in the SPA
        SDL_FRect g_listRect{}, g_toastRect{};

        // ---- GPU ----
        struct Gpu
        {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory imageMemory = VK_NULL_HANDLE;
            VkBuffer staging = VK_NULL_HANDLE;
            VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
            void* map = nullptr;
            uint32_t width = 0, height = 0;
            bool written = false; // the image has been filled once (layout TRANSFER_SRC_OPTIMAL)
        } g_gpu;

        struct Color { uint8_t r, g, b; };
        constexpr Color kPanel{ 14, 20, 34 }, kHeader{ 24, 38, 66 }, kBorder{ 72, 112, 176 }, kRowSel{ 40, 62, 104 };
        constexpr Color kText{ 236, 240, 248 }, kDim{ 128, 136, 152 }, kGold{ 242, 198, 76 }, kGreen{ 120, 206, 140 };

        void refreshList()
        {
            g_list = kernel::achievementList();
            g_selected = std::clamp(g_selected, 0, std::max(0, int(g_list.size()) - 1));
        }

        void setOpen(bool open)
        {
            if (open == g_listOpen) return;
            g_listOpen = open;
            if (open) refreshList();
            hid::setInputCaptured(open);
            g_dirty = true;
            info("[overlay] achievements list %s", open ? "opened" : "closed");
        }

        void move(int delta)
        {
            if (g_list.empty()) return;
            int s = std::clamp(g_selected + delta, 0, int(g_list.size()) - 1);
            if (s != g_selected) { g_selected = s; g_dirty = true; }
        }

        // ---- drawing helpers ----
        int g_scale = 2; // the 8x8 font's scale for body text

        void fill(const SDL_FRect& r, Color c)
        {
            SDL_SetRenderDrawColor(g_sw, c.r, c.g, c.b, 255);
            SDL_RenderFillRect(g_sw, &r);
        }

        void outline(const SDL_FRect& r, Color c, float thickness)
        {
            fill({ r.x, r.y, r.w, thickness }, c);
            fill({ r.x, r.y + r.h - thickness, r.w, thickness }, c);
            fill({ r.x, r.y, thickness, r.h }, c);
            fill({ r.x + r.w - thickness, r.y, thickness, r.h }, c);
        }

        // The built-in font has printable ASCII only.
        std::string ascii(const std::string& s)
        {
            std::string out;
            for (size_t i = 0; i < s.size(); i++)
            {
                unsigned char c = s[i];
                if (c >= 32 && c < 127) out += char(c);
                else if (c >= 0xC0) out += '?';  // first byte of a UTF-8 sequence
                // continuation bytes are dropped
            }
            return out;
        }

        // Text at (x, y) in pixels, glyphs 8 * scale; cut with ".." to fit maxWidth (0 = no limit).
        // Returns the width drawn.
        float text(float x, float y, int scale, Color c, const std::string& s, float maxWidth = 0)
        {
            std::string t = ascii(s);
            float glyph = 8.0f * scale;
            if (maxWidth > 0 && t.size() * glyph > maxWidth)
            {
                size_t fit = size_t(maxWidth / glyph);
                t = fit > 2 ? t.substr(0, fit - 2) + ".." : t.substr(0, fit);
            }
            SDL_SetRenderScale(g_sw, float(scale), float(scale));
            SDL_SetRenderDrawColor(g_sw, c.r, c.g, c.b, 255);
            SDL_RenderDebugText(g_sw, x / scale, y / scale, t.c_str());
            SDL_SetRenderScale(g_sw, 1, 1);
            return t.size() * glyph;
        }

        float textWidth(const std::string& s, int scale) { return ascii(s).size() * 8.0f * scale; }

        SDL_Texture* icon(const kernel::AchievementView& a)
        {
            if (!a.image) return nullptr;
            auto it = g_icons.find(a.image);
            if (it != g_icons.end()) return it->second;
            SDL_Texture* tex = nullptr;
            if (SDL_Surface* png = SDL_LoadPNG_IO(SDL_IOFromConstMem(a.image, a.imageSize), true))
            {
                tex = SDL_CreateTextureFromSurface(g_sw, png);
                if (tex) SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
                SDL_DestroySurface(png);
            }
            else
                warn("[overlay] achievement %u: icon not decoded: %s", a.id, SDL_GetError());
            g_icons[a.image] = tex;
            return tex;
        }

        void drawIcon(const kernel::AchievementView& a, const SDL_FRect& r, bool lit)
        {
            fill(r, { 30, 40, 60 });
            SDL_Texture* tex = (a.secret && !a.unlocked) ? nullptr : icon(a);
            if (tex)
            {
                SDL_SetTextureColorMod(tex, lit ? 255 : 70, lit ? 255 : 70, lit ? 255 : 80);
                SDL_RenderTexture(g_sw, tex, nullptr, &r);
            }
            else
            {
                int s = std::max(1, int(r.h / 16));
                text(r.x + (r.w - 8.0f * s) / 2, r.y + (r.h - 8.0f * s) / 2, s, kDim, "?");
            }
        }

        std::string date(uint64_t unixSeconds)
        {
            std::time_t t = std::time_t(unixSeconds);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::localtime(&t));
            return buf;
        }

        void drawList(float W, float H)
        {
            const int s = g_scale, small = std::max(1, s - 1), big = s + 1;
            const float glyph = 8.0f * s, pad = glyph;
            const float panelW = std::min(W * 0.86f, H * 1.55f), panelH = H * 0.86f;
            g_listRect = { std::floor((W - panelW) / 2), std::floor((H - panelH) / 2), std::floor(panelW), std::floor(panelH) };
            const SDL_FRect& P = g_listRect;
            fill(P, kPanel);

            // Header: title, totals
            const float headerH = glyph * 4.2f;
            fill({ P.x, P.y, P.w, headerH }, kHeader);
            text(P.x + pad * 1.5f, P.y + glyph * 0.8f, big, kText, "ACHIEVEMENTS");
            uint32_t got = 0, total = 0, gs = 0, gsTotal = 0;
            for (auto& a : g_list)
            {
                total++;
                gsTotal += a.gamerscore;
                if (a.unlocked) { got++; gs += a.gamerscore; }
            }
            char summary[96];
            std::snprintf(summary, sizeof(summary), "%u of %u unlocked", got, total);
            text(P.x + pad * 1.5f, P.y + glyph * 2.6f, small, kDim, summary);
            char score[64];
            std::snprintf(score, sizeof(score), "%u / %u G", gs, gsTotal);
            text(P.x + P.w - pad * 1.5f - textWidth(score, big), P.y + glyph * 1.4f, big, kGold, score);

            // Rows
            const float footerH = glyph * 2.2f, rowH = glyph * 3.4f;
            const float listY = P.y + headerH + pad * 0.5f, listH = P.h - headerH - footerH - pad;
            g_rowsVisible = std::max(1, int(listH / rowH));
            if (g_selected < g_top) g_top = g_selected;
            if (g_selected >= g_top + g_rowsVisible) g_top = g_selected - g_rowsVisible + 1;
            g_top = std::clamp(g_top, 0, std::max(0, int(g_list.size()) - g_rowsVisible));
            const float rowX = P.x + pad, rowW = P.w - pad * 3; // room for the scroll bar
            for (int i = 0; i < g_rowsVisible && g_top + i < int(g_list.size()); i++)
            {
                const auto& a = g_list[size_t(g_top + i)];
                const float y = listY + i * rowH;
                if (g_top + i == g_selected) fill({ rowX, y, rowW, rowH - 2 }, kRowSel);
                const float iconSize = rowH - glyph * 0.8f;
                drawIcon(a, { rowX + glyph * 0.4f, y + glyph * 0.4f - 1, iconSize, iconSize }, a.unlocked);
                const float tx = rowX + iconSize + glyph * 1.2f;
                const bool hidden = a.secret && !a.unlocked;
                char gsText[16];
                std::snprintf(gsText, sizeof(gsText), "%u G", a.gamerscore);
                const float gsW = textWidth(gsText, s);
                text(rowX + rowW - gsW - glyph * 0.6f, y + glyph * 0.5f, s, a.unlocked ? kGold : kDim, gsText);
                text(tx, y + glyph * 0.5f, s, a.unlocked ? kText : kDim, hidden ? "Secret achievement" : a.label,
                     rowX + rowW - gsW - glyph * 1.6f - tx);
                std::string line2 = hidden ? "Keep playing to unlock this secret achievement."
                                           : (a.unlocked ? a.description : a.unachieved);
                float dateW = 0;
                if (a.unlocked)
                {
                    std::string d = "Unlocked " + date(a.unlockTime);
                    dateW = textWidth(d, small);
                    text(rowX + rowW - dateW - glyph * 0.6f, y + glyph * 1.9f, small, kGreen, d);
                }
                text(tx, y + glyph * 1.9f, small, kDim, line2, rowX + rowW - tx - dateW - glyph * 1.2f);
            }

            // Scroll bar
            if (int(g_list.size()) > g_rowsVisible)
            {
                const float barX = P.x + P.w - pad * 1.4f, barW = glyph * 0.5f;
                fill({ barX, listY, barW, listH }, { 30, 40, 60 });
                const float knobH = std::max(glyph, listH * g_rowsVisible / g_list.size());
                const float knobY = listY + (listH - knobH) * g_top / std::max(1, int(g_list.size()) - g_rowsVisible);
                fill({ barX, knobY, barW, knobH }, kBorder);
            }

            // Footer
            fill({ P.x, P.y + P.h - footerH, P.w, footerH }, kHeader);
            text(P.x + pad * 1.5f, P.y + P.h - footerH + (footerH - 8.0f * small) / 2, small, kDim,
                 "Up/Down: scroll   LB/RB, PgUp/PgDn: page   B, Esc, F1, Guide: close");
            outline(P, kBorder, std::max(2.0f, float(s)));
        }

        void drawToast(float W, float H)
        {
            const int s = g_scale, small = std::max(1, s - 1);
            const float glyph = 8.0f * s;
            const float h = glyph * 4.2f, iconSize = h - glyph * 1.2f;
            const std::string title = "ACHIEVEMENT UNLOCKED";
            char line[160];
            std::snprintf(line, sizeof(line), "%s  -  %u G", ascii(g_toastItem.label).c_str(), g_toastItem.gamerscore);
            const float contentW = std::max(textWidth(title, small), textWidth(line, s));
            const float w = std::min(W * 0.9f, iconSize + contentW + glyph * 3.0f);
            // Top centre; over the open list, at its bottom instead (above the footer), so the
            // list's header and totals stay visible
            float y = H * 0.06f;
            if (g_listOpen && g_listRect.h > 0)
                y = g_listRect.y + g_listRect.h - glyph * 2.2f - h - glyph * 0.8f;
            g_toastRect = { std::floor((W - w) / 2), std::floor(y), std::floor(w), std::floor(h) };
            const SDL_FRect& R = g_toastRect;
            fill(R, kPanel);
            drawIcon(g_toastItem, { R.x + glyph * 0.6f, R.y + glyph * 0.6f, iconSize, iconSize }, true);
            const float tx = R.x + iconSize + glyph * 1.6f;
            text(tx, R.y + glyph * 0.9f, small, kGold, title);
            text(tx, R.y + glyph * 2.3f, s, kText, line, R.x + R.w - tx - glyph * 0.6f);
            outline(R, kGold, std::max(2.0f, float(s) * 0.75f));
        }

        // (Re)creates the CPU surface at this size; drops cached icons with the old renderer.
        bool ensureSurface(uint32_t w, uint32_t h)
        {
            if (g_surface && uint32_t(g_surface->w) == w && uint32_t(g_surface->h) == h) return true;
            for (auto& [_, t] : g_icons) if (t) SDL_DestroyTexture(t);
            g_icons.clear();
            if (g_sw) SDL_DestroyRenderer(g_sw);
            if (g_surface) SDL_DestroySurface(g_surface);
            g_sw = nullptr;
            g_surface = SDL_CreateSurface(int(w), int(h), SDL_PIXELFORMAT_RGBA32);
            if (g_surface) g_sw = SDL_CreateSoftwareRenderer(g_surface);
            if (!g_sw)
            {
                warn("[overlay] no software renderer: %s", SDL_GetError());
                return false;
            }
            g_scale = std::max(2, int(h / 360)); // 720p: 2, 1080p: 3, 1440p: 4, 2160p: 6
            return true;
        }

        void draw(uint32_t w, uint32_t h, bool backdrop)
        {
            if (backdrop)
            {
                SDL_SetRenderDrawColor(g_sw, 70, 90, 70, 255); // stands in for the game in a preview
                SDL_RenderClear(g_sw);
            }
            g_listRect = g_toastRect = {};
            if (g_listOpen) drawList(float(w), float(h));
            if (g_toast) drawToast(float(w), float(h));
            SDL_FlushRenderer(g_sw);
        }

        void updateToast()
        {
            uint64_t n = kernel::achievementUnlockCount();
            if (n != g_seenUnlocks)
            {
                g_seenUnlocks = n;
                uint16_t id = kernel::lastUnlockedAchievement();
                refreshList();
                for (auto& a : g_list)
                    if (a.id == id) g_toastItem = a;
                g_toast = true;
                g_toastUntil = clock::now() + std::chrono::milliseconds(int(kToastSeconds * 1000));
                g_dirty = true;
            }
            else if (g_toast && clock::now() >= g_toastUntil)
            {
                g_toast = false;
                g_dirty = true; // the list under it (if open) needs drawing again
            }
        }

        // ---- GPU resources ----
        void destroyGpu(VkDevice device)
        {
            if (g_gpu.map) vkUnmapMemory(device, g_gpu.stagingMemory);
            if (g_gpu.staging) vkDestroyBuffer(device, g_gpu.staging, nullptr);
            if (g_gpu.stagingMemory) vkFreeMemory(device, g_gpu.stagingMemory, nullptr);
            if (g_gpu.image) vkDestroyImage(device, g_gpu.image, nullptr);
            if (g_gpu.imageMemory) vkFreeMemory(device, g_gpu.imageMemory, nullptr);
            g_gpu = Gpu{};
        }

        bool createGpu(vkctx::Context& ctx, uint32_t w, uint32_t h)
        {
            destroyGpu(ctx.device);
            VkDeviceSize size = VkDeviceSize(w) * h * 4;
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = size;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            VK_CHECK(vkCreateBuffer(ctx.device, &bci, nullptr, &g_gpu.staging));
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(ctx.device, g_gpu.staging, &req);
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = ctx.findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (mai.memoryTypeIndex == UINT32_MAX) { warn("[overlay] no host-visible memory"); destroyGpu(ctx.device); return false; }
            VK_CHECK(vkAllocateMemory(ctx.device, &mai, nullptr, &g_gpu.stagingMemory));
            VK_CHECK(vkBindBufferMemory(ctx.device, g_gpu.staging, g_gpu.stagingMemory, 0));
            VK_CHECK(vkMapMemory(ctx.device, g_gpu.stagingMemory, 0, size, 0, &g_gpu.map));

            VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = VK_FORMAT_R8G8B8A8_UNORM; // SDL_PIXELFORMAT_RGBA32: bytes R, G, B, A
            ici.extent = { w, h, 1 };
            ici.mipLevels = 1;
            ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VK_CHECK(vkCreateImage(ctx.device, &ici, nullptr, &g_gpu.image));
            vkGetImageMemoryRequirements(ctx.device, g_gpu.image, &req);
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = ctx.findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VK_CHECK(vkAllocateMemory(ctx.device, &mai, nullptr, &g_gpu.imageMemory));
            VK_CHECK(vkBindImageMemory(ctx.device, g_gpu.image, g_gpu.imageMemory, 0));
            g_gpu.width = w;
            g_gpu.height = h;
            info("[overlay] %ux%u overlay image (%.1f MB, made the first time something is shown)", w, h, double(size) / (1024 * 1024));
            return true;
        }

        void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                          VkAccessFlags dstAccess)
        {
            VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.oldLayout = from;
            b.newLayout = to;
            b.srcAccessMask = srcAccess;
            b.dstAccessMask = dstAccess;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        }

        void blitRect(VkCommandBuffer cmd, VkImage dst, const SDL_FRect& r)
        {
            if (r.w <= 0 || r.h <= 0) return;
            int32_t x0 = int32_t(r.x), y0 = int32_t(r.y);
            int32_t x1 = std::min(int32_t(r.x + r.w), int32_t(g_gpu.width)), y1 = std::min(int32_t(r.y + r.h), int32_t(g_gpu.height));
            if (x1 <= x0 || y1 <= y0) return;
            VkImageBlit blit{};
            blit.srcSubresource = blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.srcOffsets[0] = blit.dstOffsets[0] = { x0, y0, 0 };
            blit.srcOffsets[1] = blit.dstOffsets[1] = { x1, y1, 1 };
            vkCmdBlitImage(cmd, g_gpu.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_NEAREST);
        }
    }

    bool handleEvent(const SDL_Event& e)
    {
        if (e.type != SDL_EVENT_KEY_DOWN) return false;
        SDL_Keycode k = e.key.key;
        if (k == SDLK_F1)
        {
            if (!e.key.repeat) setOpen(!g_listOpen);
            return true;
        }
        if (!g_listOpen) return false;
        switch (k)
        {
        case SDLK_ESCAPE: setOpen(false); return true;
        case SDLK_UP: move(-1); return true;
        case SDLK_DOWN: move(1); return true;
        case SDLK_PAGEUP: move(-g_rowsVisible); return true;
        case SDLK_PAGEDOWN: move(g_rowsVisible); return true;
        case SDLK_HOME: move(-int(g_list.size())); return true;
        case SDLK_END: move(int(g_list.size())); return true;
        default: return false;
        }
    }

    void handlePad(uint32_t presses)
    {
        if (!presses) return;
        if (presses & kGuide) { setOpen(!g_listOpen); return; }
        if (!g_listOpen) return;
        if (presses & (kB | kBack)) { setOpen(false); return; }
        if (presses & kUp) move(-1);
        if (presses & kDown) move(1);
        if (presses & kLb) move(-g_rowsVisible);
        if (presses & kRb) move(g_rowsVisible);
    }

    void record(vkctx::Context& ctx, VkCommandBuffer cmd, VkImage dst, VkExtent2D extent)
    {
        updateToast();
        if (!g_listOpen && !g_toast) return; // hidden: nothing recorded
        if (!extent.width || !extent.height) return;
        if (g_gpu.width != extent.width || g_gpu.height != extent.height)
        {
            if (!createGpu(ctx, extent.width, extent.height)) return;
            g_dirty = true;
        }
        if (!ensureSurface(extent.width, extent.height)) return;

        if (g_dirty)
        {
            draw(extent.width, extent.height, false);
            // Rows of the surface into the tightly packed staging buffer
            const uint8_t* src = static_cast<const uint8_t*>(g_surface->pixels);
            uint8_t* out = static_cast<uint8_t*>(g_gpu.map);
            const size_t row = size_t(extent.width) * 4;
            for (uint32_t y = 0; y < extent.height; y++)
                std::memcpy(out + y * row, src + size_t(y) * g_surface->pitch, row);
            imageBarrier(cmd, g_gpu.image, g_gpu.written ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { extent.width, extent.height, 1 };
            vkCmdCopyBufferToImage(cmd, g_gpu.staging, g_gpu.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            imageBarrier(cmd, g_gpu.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            g_gpu.written = true;
            g_dirty = false;
        }
        // The game's blit into dst comes first in this command buffer: order our writes after it.
        VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        if (g_listOpen) blitRect(cmd, dst, g_listRect);
        if (g_toast) blitRect(cmd, dst, g_toastRect);
    }

    void destroy(VkDevice device)
    {
        destroyGpu(device);
        for (auto& [_, t] : g_icons) if (t) SDL_DestroyTexture(t);
        g_icons.clear();
        if (g_sw) SDL_DestroyRenderer(g_sw);
        if (g_surface) SDL_DestroySurface(g_surface);
        g_sw = nullptr;
        g_surface = nullptr;
    }

    bool writePreview(const char* path, uint32_t width, uint32_t height)
    {
        if (!ensureSurface(width, height)) return false;
        refreshList();
        g_listOpen = true;
        g_selected = 0;
        for (size_t i = 0; i < g_list.size(); i++)
            if (g_list[i].unlocked) { g_selected = int(i); break; }
        g_toast = !g_list.empty();
        if (g_toast) g_toastItem = g_list[size_t(g_selected)];
        draw(width, height, true);
        bool ok = SDL_SavePNG(g_surface, path);
        if (!ok) warn("[overlay] could not write %s: %s", path, SDL_GetError());
        return ok;
    }
}
