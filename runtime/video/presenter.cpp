// SDL3 window + Vulkan swapchain that shows the guest front buffer.
//
// When the renderer (runtime/gpu/renderer.cpp) produced the swapped frame, its resolved front
// buffer image is blitted, letterboxed to 16:9, into the swapchain image. Otherwise (renderer
// off, or the frame was not rendered) the raw guest front buffer is copied from physical memory
// into a staging buffer, uploaded to a source image and blitted instead. One frame in flight.
// Both threads use one queue: the renderer submits a frame before publishing it, so queue
// submission order plus the barrier below order its writes before our read.
#include "presenter.h"
#include "overlay.h"
#include "vk_context.h"
#include "../build_id.h"
#include "../debug/png.h"
#include "../gpu/capture.h"
#include "../hid/hid.h"
#include "../log.h"
#include "../debug/thread_times.h"
#include "../memory.h"
#include "../settings.h"
#include "../paths.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace video
{
    namespace
    {
        // ---- Published by the guest (command processor thread) ----
        std::atomic<uint64_t> g_frameNumber{0};
        std::atomic<uint32_t> g_front{0}, g_width{0}, g_height{0};
        // ---- Published by the renderer (same thread as above, before publishFrame: its frame
        // number is never behind g_frameNumber while it produces the frames, and for a moment at
        // each swap one ahead) ----
        std::atomic<uint64_t> g_renderedFrame{0}, g_renderedImage{0};
        std::atomic<uint32_t> g_renderedWidth{0}, g_renderedHeight{0};
        vkctx::Context* g_ctx = nullptr;

        // BB_PERF (#174): read by the renderer's [perf] report (takePresentStats). Microseconds.
        std::atomic<uint64_t> g_statPresents{0}, g_statFenceUs{0}, g_statAcquireUs{0}, g_statQueueUs{0}, g_statQueueMaxUs{0};
        std::atomic<uint64_t> g_statSkipped{0}; // game frames that were never presented
        uint64_t g_presentedFrame = 0;          // the game frame the last present showed (presenter thread)
        uint64_t usSince(std::chrono::steady_clock::time_point t0)
        {
            return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
        }

        // BB_PRESENT_MODE (#174). The window is presented with vsync (FIFO) on the queue the
        // renderer submits to. On Windows a present holds that queue until the display's next
        // refresh: with one for each refresh of a 240 Hz display, the renderer's round trips to the
        // GPU took 3-4 ms each and the frame rate fell to 49-53 around every play. With one present
        // for each game frame they take about 1 ms, and the match holds 60. On Linux a present
        // doesn't hold the queue (the wait is in vkAcquireNextImageKHR), so it stays as it was.
        //   fifo:       one present for each refresh of the display (the default on Linux)
        //   paced:      FIFO, but one present for each new game frame, not one for each refresh
        //               (the default on Windows)
        //   mailbox:    no present waits for a refresh; the display shows the newest picture at
        //               each refresh, without tearing. One present for each new game frame.
        //   immediate:  no present waits for a refresh; can tear. One for each new game frame.
        // A mode the surface doesn't have falls back: mailbox -> immediate -> paced.
        enum class PresentPlan { Fifo, Paced, Mailbox, Immediate };
        const PresentPlan g_presentPlan = [] {
            const char* v = getenv("BB_PRESENT_MODE");
#ifdef _WIN32
            if (!v) return PresentPlan::Paced;
#else
            if (!v) return PresentPlan::Fifo;
#endif
            if (!strcmp(v, "paced")) return PresentPlan::Paced;
            if (!strcmp(v, "mailbox")) return PresentPlan::Mailbox;
            if (!strcmp(v, "immediate")) return PresentPlan::Immediate;
            return PresentPlan::Fifo;
        }();
        // Set with the swapchain: a present only for a new game frame. The window's loop then
        // waits on g_frameSignal, which publishFrame signals, so a frame is shown as it arrives.
        std::atomic<bool> g_presentPaced{false};
        std::atomic<SDL_Semaphore*> g_frameSignal{nullptr};

        struct Vk
        {
            SDL_Window* window = nullptr;
            VkInstance instance = VK_NULL_HANDLE;
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            VkPhysicalDevice gpu = VK_NULL_HANDLE;
            uint32_t queueFamily = 0;
            VkDevice device = VK_NULL_HANDLE;
            VkQueue queue = VK_NULL_HANDLE;

            VkSwapchainKHR swapchain = VK_NULL_HANDLE;
            VkFormat swapFormat = VK_FORMAT_UNDEFINED;
            VkExtent2D swapExtent{};
            std::vector<VkImage> swapImages;

            // Guest frame upload
            VkBuffer staging = VK_NULL_HANDLE;
            VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
            void* stagingMap = nullptr;
            VkImage source = VK_NULL_HANDLE;
            VkDeviceMemory sourceMemory = VK_NULL_HANDLE;
            uint32_t sourceWidth = 0, sourceHeight = 0;
            bool sourceValid = false; // has been written at least once

            VkCommandPool pool = VK_NULL_HANDLE;
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            VkSemaphore acquired = VK_NULL_HANDLE;
            std::vector<VkSemaphore> rendered; // one per swapchain image
            VkFence inFlight = VK_NULL_HANDLE;

            // F12 screenshots. shotWant is set by the key handler, consumed by the next
            // presentOnce, which copies the swapchain image it just drew into `shot`; the file
            // writing happens on a helper thread so the presenter keeps presenting.
            // Presenter-thread only, no atomics needed.
            VkBuffer shot = VK_NULL_HANDLE;
            VkDeviceMemory shotMemory = VK_NULL_HANDLE;
            VkDeviceSize shotSize = 0;
            bool shotWant = false, shotPending = false;
            // Can this swapchain image be read back at all? F12 only works when it was created as a
            // transfer source, which the surface decides - see createSwapchain.
            bool shotSupported = false;
            uint32_t shotWidth = 0, shotHeight = 0;
            std::vector<std::thread> shotWriters;
        } vk;

        // ---- Window state remembered between runs (issue #9) ----
        // game/settings.ini (runtime/settings.h). The window writes the file back when it closes,
        // with every key as it was read, so the resolution scale (#115) stays as the player set it.
        settings::Settings g_settings;
        bool g_fullscreen = false;

        // Only the windowed geometry is worth remembering: in borderless fullscreen the window is
        // the whole display, so what is stored is the size it was before going fullscreen.
        void rememberWindowGeometry()
        {
            if (g_fullscreen)
                return;
            int w = 0, h = 0, x = 0, y = 0;
            SDL_GetWindowSize(vk.window, &w, &h);
            SDL_GetWindowPosition(vk.window, &x, &y);
            if (w > 0 && h > 0) { g_settings.width = w; g_settings.height = h; }
            if (x >= 0 && y >= 0) { g_settings.x = x; g_settings.y = y; g_settings.hasPosition = true; }
        }

        void saveSettings()
        {
            if (!g_fullscreen)
                rememberWindowGeometry();
            g_settings.fullscreen = g_fullscreen;
            settings::save(g_settings, paths::settingsFile());
        }

        // The title's own icon, from the player's disc: <game files>/icon.png, which the installer
        // and scripts/setup.sh write (#138); nothing is shipped. X11 and KDE Plasma's Wayland
        // (xdg_toplevel_icon_v1) show it in the taskbar; GNOME takes the menu entry's icon, found by
        // the app id set before SDL_Init. No file: the desktop's default icon, as before.
        void setWindowIcon()
        {
#if SDL_VERSION_ATLEAST(3, 4, 0)
            const std::string path = paths::data() + "/icon.png";
            std::error_code ec;
            if (!std::filesystem::exists(path, ec))
                return;
            SDL_Surface* icon = SDL_LoadPNG(path.c_str());
            if (!icon)
            {
                warn("[video] window icon: can't read %s (%s)", path.c_str(), SDL_GetError());
                return;
            }
            if (SDL_SetWindowIcon(vk.window, icon))
                info("[video] window icon %s (%dx%d)", path.c_str(), icon->w, icon->h);
            else
                info("[video] no window icon here: %s", SDL_GetError());
            SDL_DestroySurface(icon);
#endif
        }

        void setFullscreen(bool on)
        {
            if (on == g_fullscreen)
                return;
            if (on)
                rememberWindowGeometry(); // the size we are about to leave
            if (!SDL_SetWindowFullscreen(vk.window, on))
            {
                warn("[video] fullscreen %s: %s", on ? "on" : "off", SDL_GetError());
                return;
            }
            g_fullscreen = on;
            if (!on && g_settings.hasPosition)
            {
                // SDL restores the pre-fullscreen size on its own; putting the remembered size back
                // is what makes the value in the file effective on the next run.
                SDL_SetWindowSize(vk.window, g_settings.width, g_settings.height);
                SDL_SetWindowPosition(vk.window, g_settings.x, g_settings.y);
            }
            info("[video] %s (window %dx%d)", on ? "borderless fullscreen" : "windowed", g_settings.width, g_settings.height);
            saveSettings();
        }

        uint32_t findMemory(uint32_t typeBits, VkMemoryPropertyFlags flags)
        {
            uint32_t i = g_ctx->findMemory(typeBits, flags);
            if (i == UINT32_MAX) fatal("[video] no suitable memory type");
            return i;
        }

        void createInstanceAndDevice()
        {
            g_ctx = &vkctx::createForWindow(vk.window, vk.surface);
            vk.instance = g_ctx->instance;
            vk.gpu = g_ctx->gpu;
            vk.queueFamily = g_ctx->queueFamily;
            vk.device = g_ctx->device;
            vk.queue = g_ctx->queue;

            VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pci.queueFamilyIndex = vk.queueFamily;
            VK_CHECK(vkCreateCommandPool(vk.device, &pci, nullptr, &vk.pool));
            VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            cai.commandPool = vk.pool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            VK_CHECK(vkAllocateCommandBuffers(vk.device, &cai, &vk.cmd));
            VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            VK_CHECK(vkCreateSemaphore(vk.device, &sci, nullptr, &vk.acquired));
            VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VK_CHECK(vkCreateFence(vk.device, &fci, nullptr, &vk.inFlight));
        }

        void destroySwapchain()
        {
            for (VkSemaphore s : vk.rendered) vkDestroySemaphore(vk.device, s, nullptr);
            vk.rendered.clear();
            if (vk.swapchain) vkDestroySwapchainKHR(vk.device, vk.swapchain, nullptr);
            vk.swapchain = VK_NULL_HANDLE;
            vk.swapImages.clear();
        }

        bool createSwapchain()
        {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(vk.window, &w, &h);
            if (w <= 0 || h <= 0) return false; // minimized

            VkSurfaceCapabilitiesKHR caps;
            VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.gpu, vk.surface, &caps));
            uint32_t fn = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, vk.surface, &fn, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(fn);
            vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, vk.surface, &fn, formats.data());
            VkSurfaceFormatKHR chosen = formats[0];
            for (auto& f : formats)
                if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                    f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                    { chosen = f; break; }

            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(vk.gpu, chosen.format, &fp);
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
                fatal("[video] swapchain format %d cannot be a blit destination", int(chosen.format));

            // F12 reads the picture back out of the swapchain image, so that image has to be usable
            // as a copy source as well as a blit destination. The loader says which usage bits the
            // surface takes at all (VUID-VkSwapchainCreateInfoKHR-imageUsage-01275) - every desktop
            // driver allows reading one back, but the bit is asked for at vkCreateSwapchainKHR and
            // a driver that refuses it would fail the swapchain, and presentation does not need it.
            // So: ask only when allowed, and turn the screenshot key off instead of the window.
            vk.shotSupported = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
            static bool shotWarned = false;
            if (!vk.shotSupported && !shotWarned)
            {
                shotWarned = true;
                warn("[video] the surface will not make a swapchain image a copy source: F12 screenshots are off");
            }

            VkExtent2D extent = caps.currentExtent;
            if (extent.width == UINT32_MAX)
                extent = { uint32_t(w), uint32_t(h) };
            uint32_t images = caps.minImageCount + 1;
            if (caps.maxImageCount && images > caps.maxImageCount) images = caps.maxImageCount;

            VkSwapchainCreateInfoKHR sci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
            sci.surface = vk.surface;
            sci.minImageCount = images;
            sci.imageFormat = chosen.format;
            sci.imageColorSpace = chosen.colorSpace;
            sci.imageExtent = extent;
            sci.imageArrayLayers = 1;
            sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (vk.shotSupported ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : VkImageUsageFlags(0));
            sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            sci.preTransform = caps.currentTransform;
            sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            sci.presentMode = VK_PRESENT_MODE_FIFO_KHR; // vsync; only this thread waits on it
            if (g_presentPlan != PresentPlan::Fifo)
            {
                uint32_t modeCount = 0;
                vkGetPhysicalDeviceSurfacePresentModesKHR(vk.gpu, vk.surface, &modeCount, nullptr);
                std::vector<VkPresentModeKHR> modes(modeCount);
                vkGetPhysicalDeviceSurfacePresentModesKHR(vk.gpu, vk.surface, &modeCount, modes.data());
                auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
                const char* name = "FIFO";
                if (g_presentPlan == PresentPlan::Mailbox && has(VK_PRESENT_MODE_MAILBOX_KHR))
                    sci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR, name = "MAILBOX";
                else if (g_presentPlan != PresentPlan::Paced && has(VK_PRESENT_MODE_IMMEDIATE_KHR))
                    sci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR, name = "IMMEDIATE";
                g_presentPaced = true;
                std::string list;
                for (VkPresentModeKHR m : modes)
                    list += std::string(list.empty() ? "" : ", ") +
                            (m == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : m == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" :
                             m == VK_PRESENT_MODE_FIFO_KHR ? "FIFO" : m == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "FIFO_RELAXED" : "other");
                static bool said = false;
                if (!said)
                {
                    said = true;
                    const char* asked = getenv("BB_PRESENT_MODE");
                    info("[video] presenting with %s, one present for each new game frame (%s%s; the surface has %s)", name,
                         asked ? "BB_PRESENT_MODE=" : "the default here", asked ? asked : "", list.c_str());
                }
            }
            sci.clipped = VK_TRUE;
            VK_CHECK(vkCreateSwapchainKHR(vk.device, &sci, nullptr, &vk.swapchain));
            vk.swapFormat = chosen.format;
            vk.swapExtent = extent;
            uint32_t n = 0;
            vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &n, nullptr);
            vk.swapImages.resize(n);
            vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &n, vk.swapImages.data());
            VkSemaphoreCreateInfo semi{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            vk.rendered.resize(n);
            for (auto& s : vk.rendered) VK_CHECK(vkCreateSemaphore(vk.device, &semi, nullptr, &s));
            return true;
        }

        void destroySource()
        {
            if (vk.stagingMap) vkUnmapMemory(vk.device, vk.stagingMemory);
            if (vk.staging) vkDestroyBuffer(vk.device, vk.staging, nullptr);
            if (vk.stagingMemory) vkFreeMemory(vk.device, vk.stagingMemory, nullptr);
            if (vk.source) vkDestroyImage(vk.device, vk.source, nullptr);
            if (vk.sourceMemory) vkFreeMemory(vk.device, vk.sourceMemory, nullptr);
            vk.stagingMap = nullptr;
            vk.staging = VK_NULL_HANDLE;
            vk.stagingMemory = vk.sourceMemory = VK_NULL_HANDLE;
            vk.source = VK_NULL_HANDLE;
            vk.sourceValid = false;
        }

        void createSource(uint32_t width, uint32_t height)
        {
            destroySource();
            VkDeviceSize size = VkDeviceSize(width) * height * 4;
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = size;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            VK_CHECK(vkCreateBuffer(vk.device, &bci, nullptr, &vk.staging));
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(vk.device, vk.staging, &req);
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VK_CHECK(vkAllocateMemory(vk.device, &mai, nullptr, &vk.stagingMemory));
            VK_CHECK(vkBindBufferMemory(vk.device, vk.staging, vk.stagingMemory, 0));
            VK_CHECK(vkMapMemory(vk.device, vk.stagingMemory, 0, size, 0, &vk.stagingMap));

            // Guest 8_8_8_8 with red/blue swapped lands as B8G8R8A8 once untiled/converted; the raw
            // copy for now is interpreted the same way.
            VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            ici.imageType = VK_IMAGE_TYPE_2D;
            ici.format = VK_FORMAT_B8G8R8A8_UNORM;
            ici.extent = { width, height, 1 };
            ici.mipLevels = 1;
            ici.arrayLayers = 1;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VK_CHECK(vkCreateImage(vk.device, &ici, nullptr, &vk.source));
            vkGetImageMemoryRequirements(vk.device, vk.source, &req);
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VK_CHECK(vkAllocateMemory(vk.device, &mai, nullptr, &vk.sourceMemory));
            VK_CHECK(vkBindImageMemory(vk.device, vk.source, vk.sourceMemory, 0));
            vk.sourceWidth = width;
            vk.sourceHeight = height;
            info("[video] front buffer source %ux%u", width, height);
        }

        void barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                     VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
        {
            VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.oldLayout = from;
            b.newLayout = to;
            b.srcAccessMask = srcAccess;
            b.dstAccessMask = dstAccess;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(vk.cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
        }

        void destroyShotBuffer()
        {
            if (vk.shot) vkDestroyBuffer(vk.device, vk.shot, nullptr);
            if (vk.shotMemory) vkFreeMemory(vk.device, vk.shotMemory, nullptr);
            vk.shot = VK_NULL_HANDLE;
            vk.shotMemory = VK_NULL_HANDLE;
            vk.shotSize = 0;
        }

        void createShotBuffer(VkDeviceSize size)
        {
            destroyShotBuffer();
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = size;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VK_CHECK(vkCreateBuffer(vk.device, &bci, nullptr, &vk.shot));
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(vk.device, vk.shot, &req);
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = findMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VK_CHECK(vkAllocateMemory(vk.device, &mai, nullptr, &vk.shotMemory));
            VK_CHECK(vkBindBufferMemory(vk.device, vk.shot, vk.shotMemory, 0));
            vk.shotSize = size;
        }

        const char* swapFormatName(VkFormat f)
        {
            switch (f)
            {
            case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
            case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
            case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
            case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
            default: return "unknown";
            }
        }

        bool swapToRedBlue(VkFormat f) { return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB; }

        // screenshots/<date-time>.png, "_2", "_3" ... on repeats within the same second. The writer
        // thread creates the file later, so checking the disk alone would give two presses in one
        // second the same name (the second PNG replacing the first): the names handed out are
        // remembered here too. Presenter thread only, so no lock.
        std::string shotPath()
        {
            static std::string lastStamp;
            static int lastN = 0;
            std::time_t now = std::time(nullptr);
            std::tm tm{};
            localtime_r(&now, &tm);
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm);
            for (int n = stamp == lastStamp ? lastN + 1 : 1;; n++)
            {
                std::string path = paths::screenshots() + "/" + stamp + (n > 1 ? "_" + std::to_string(n) : std::string()) + ".png";
                if (std::filesystem::exists(path)) continue; // from an earlier run
                lastStamp = stamp;
                lastN = n;
                return path;
            }
        }

        // The copy is already in the command buffer of the frame we just submitted; wait for that
        // frame (inFlight, the same fence the next present waits on), hand the pixels to a helper
        // thread and get back to presenting. The guest threads are never involved.
        void writeScreenshot()
        {
            VK_CHECK(vkWaitForFences(vk.device, 1, &vk.inFlight, VK_TRUE, UINT64_MAX));
            void* map = nullptr;
            VK_CHECK(vkMapMemory(vk.device, vk.shotMemory, 0, vk.shotSize, 0, &map));
            std::vector<uint8_t> rgba((uint8_t*)map, (uint8_t*)map + vk.shotSize);
            vkUnmapMemory(vk.device, vk.shotMemory);
            if (swapToRedBlue(vk.swapFormat))
                for (size_t i = 0; i + 3 < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);

            std::string path = shotPath();
            uint32_t w = vk.shotWidth, h = vk.shotHeight;
            auto start = std::chrono::steady_clock::now();
            // A PNG write is tens of milliseconds, so keeping every writer thread around would pile up
            // handles on a mash of F12 presses: retire the oldest once there are a few.
            if (vk.shotWriters.size() >= 4) { vk.shotWriters.front().join(); vk.shotWriters.erase(vk.shotWriters.begin()); }
            vk.shotWriters.emplace_back([path, rgba = std::move(rgba), w, h, start]() mutable {
                std::filesystem::create_directories(paths::screenshots());
                if (debug::writePng(path, rgba.data(), w, h))
                    info("[video] screenshot %s (%ux%u, %.1f MB, %.0f ms)", path.c_str(), w, h, double(rgba.size()) / (1024 * 1024),
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                else
                    warn("[video] could not write %s", path.c_str());
            });
        }

        // Returns false if the swapchain must be recreated.
        bool presentOnce(uint64_t& uploadedFrame)
        {
            auto t0 = std::chrono::steady_clock::now();
            VK_CHECK(vkWaitForFences(vk.device, 1, &vk.inFlight, VK_TRUE, UINT64_MAX));
            g_statFenceUs += usSince(t0);
            uint32_t index = 0;
            t0 = std::chrono::steady_clock::now();
            VkResult r = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, vk.acquired, VK_NULL_HANDLE, &index);
            g_statAcquireUs += usSince(t0);
            if (r == VK_ERROR_OUT_OF_DATE_KHR) return false;
            if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) fatal("[video] vkAcquireNextImageKHR: %d", int(r));
            VK_CHECK(vkResetFences(vk.device, 1, &vk.inFlight));

            // New guest frame? Copy it into the staging buffer (the previous upload is complete:
            // we waited on the fence above).
            uint64_t frame = g_frameNumber.load(std::memory_order_acquire);
            if (g_presentedFrame && frame > g_presentedFrame + 1) g_statSkipped += frame - g_presentedFrame - 1;
            g_presentedFrame = frame;
            uint32_t front = g_front.load(), width = g_width.load(), height = g_height.load();
            // Did the renderer produce this frame? Then show its image instead of the raw copy.
            // The renderer publishes its frame before the guest's number goes up (onSwap, then
            // publishFrame, on one thread), so between the two it is one ahead of `frame`: still
            // its picture, and the newest there is. Asking for the same number here took the raw
            // copy whenever a present fell into that gap, and during a match the front buffer's
            // memory is black with a few rows of stale pixels across the middle: one refresh of
            // black and "static". The gap is a microsecond, tens of them on the frames the
            // renderer prunes its geometry cache, and a 240 Hz display looks four times a game
            // frame: there it showed every minute or two (#167's flashing lines look like it too).
            VkImage rendered = VK_NULL_HANDLE;
            uint32_t renderedWidth = 0, renderedHeight = 0;
            if (frame && g_renderedFrame.load(std::memory_order_acquire) >= frame)
            {
                rendered = VkImage(g_renderedImage.load());
                renderedWidth = g_renderedWidth.load();
                renderedHeight = g_renderedHeight.load();
            }
            bool upload = false;
            if (!rendered && frame && frame != uploadedFrame && width && height && width <= 8192 && height <= 8192)
            {
                if (width != vk.sourceWidth || height != vk.sourceHeight)
                    createSource(width, height);
                memcpy(vk.stagingMap, mem::physical(front), size_t(width) * height * 4);
                uploadedFrame = frame;
                upload = true;
            }

            VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VK_CHECK(vkResetCommandBuffer(vk.cmd, 0));
            VK_CHECK(vkBeginCommandBuffer(vk.cmd, &bi));

            VkImage dst = vk.swapImages[index];
            barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkClearColorValue black{};
            VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(vk.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

            auto letterbox = [&](VkImage image, uint32_t srcWidth, uint32_t srcHeight) {
                // Keep the guest aspect ratio inside the window
                float sx = float(vk.swapExtent.width) / srcWidth, sy = float(vk.swapExtent.height) / srcHeight;
                float s = sx < sy ? sx : sy;
                int32_t w = int32_t(srcWidth * s), h = int32_t(srcHeight * s);
                int32_t x = (int32_t(vk.swapExtent.width) - w) / 2, y = (int32_t(vk.swapExtent.height) - h) / 2;
                VkImageBlit blit{};
                blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                blit.srcOffsets[1] = { int32_t(srcWidth), int32_t(srcHeight), 1 };
                blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                blit.dstOffsets[0] = { x, y, 0 };
                blit.dstOffsets[1] = { x + w, y + h, 1 };
                vkCmdBlitImage(vk.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                               VK_FILTER_LINEAR);
            };
            if (rendered)
            {
                // The renderer keeps its front buffer images in TRANSFER_SRC_OPTIMAL between submissions
                VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
                mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(vk.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0,
                                     nullptr);
                letterbox(rendered, renderedWidth, renderedHeight);
            }
            else if (vk.source)
            {
                if (upload)
                {
                    barrier(vk.source, vk.sourceValid ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                    VkBufferImageCopy copy{};
                    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                    copy.imageExtent = { vk.sourceWidth, vk.sourceHeight, 1 };
                    vkCmdCopyBufferToImage(vk.cmd, vk.staging, vk.source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                    barrier(vk.source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT);
                    vk.sourceValid = true;
                }
                if (vk.sourceValid)
                    letterbox(vk.source, vk.sourceWidth, vk.sourceHeight);
            }

            // The overlay (achievements list, unlock pop-up; #101) on top of the game. Records
            // nothing while nothing is shown. Before the F12 read-back, so screenshots include it.
            overlay::record(*g_ctx, vk.cmd, dst, vk.swapExtent);

            // F12: read back exactly what is on screen - the letterboxed, format-converted picture,
            // whether it came from the renderer's front buffer or the raw front-buffer upload. We are
            // inside the same command buffer, so this is ordered after the blit and before the present.
            if (vk.shotWant && !vk.shotPending) // shotPending: the previous read-back is still being written; defer one frame
            {
                vk.shotWant = false;
                VkDeviceSize size = VkDeviceSize(vk.swapExtent.width) * vk.swapExtent.height * 4;
                if (!vk.shot || vk.shotSize != size) createShotBuffer(size);
                barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy bic{};
                bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                bic.imageExtent = { vk.swapExtent.width, vk.swapExtent.height, 1 };
                vkCmdCopyImageToBuffer(vk.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.shot, 1, &bic);
                barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                vk.shotWidth = vk.swapExtent.width;
                vk.shotHeight = vk.swapExtent.height;
                vk.shotPending = true;
                info("[video] screenshot requested (%ux%u, %s)", vk.swapExtent.width, vk.swapExtent.height, swapFormatName(vk.swapFormat));
            }

            barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            VK_CHECK(vkEndCommandBuffer(vk.cmd));

            VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = &vk.acquired;
            si.pWaitDstStageMask = &waitStage;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &vk.cmd;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &vk.rendered[index];
            std::lock_guard queueGuard(g_ctx->queueLock);
            auto q0 = std::chrono::steady_clock::now();
            VK_CHECK(vkQueueSubmit(vk.queue, 1, &si, vk.inFlight));

            VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &vk.rendered[index];
            pi.swapchainCount = 1;
            pi.pSwapchains = &vk.swapchain;
            pi.pImageIndices = &index;
            r = vkQueuePresentKHR(vk.queue, &pi);
            const uint64_t queueUs = usSince(q0);
            g_statQueueUs += queueUs;
            if (queueUs > g_statQueueMaxUs.load(std::memory_order_relaxed)) g_statQueueMaxUs = queueUs;
            g_statPresents++;
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) return false;
            if (r != VK_SUCCESS) fatal("[video] vkQueuePresentKHR: %d", int(r));
            if (vk.shotPending) { writeScreenshot(); vk.shotPending = false; }
            return true;
        }
    }

    PresentStats takePresentStats()
    {
        PresentStats s;
        s.presents = g_statPresents.exchange(0);
        s.fenceMs = g_statFenceUs.exchange(0) / 1000.0;
        s.acquireMs = g_statAcquireUs.exchange(0) / 1000.0;
        s.queueMs = g_statQueueUs.exchange(0) / 1000.0;
        s.queueMaxMs = g_statQueueMaxUs.exchange(0) / 1000.0;
        s.skippedFrames = g_statSkipped.exchange(0);
        return s;
    }

    void publishRenderedFrame(VkImage image, uint32_t width, uint32_t height, uint64_t frameNumber)
    {
        g_renderedImage.store(uint64_t(image), std::memory_order_relaxed);
        g_renderedWidth.store(width, std::memory_order_relaxed);
        g_renderedHeight.store(height, std::memory_order_relaxed);
        g_renderedFrame.store(frameNumber, std::memory_order_release);
    }

    void publishFrame(uint32_t front, uint32_t width, uint32_t height, uint64_t frameNumber)
    {
        g_front.store(front & 0x1FFFFFFF, std::memory_order_relaxed);
        g_width.store(width, std::memory_order_relaxed);
        g_height.store(height, std::memory_order_relaxed);
        g_frameNumber.store(frameNumber, std::memory_order_release);
        if (g_presentPaced.load(std::memory_order_relaxed))
            if (SDL_Semaphore* signal = g_frameSignal.load(std::memory_order_acquire)) SDL_SignalSemaphore(signal);
    }

    void run(bool startFullscreen)
    {
        debug::threadtimes::registerRole("window", nullptr); // the process's main thread: its name stays
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1"); // SIGINT/SIGTERM end the run (main.cpp)
        // The window's app id (Wayland) and class (X11): the name of the menu entry, the installer's
        // (installer/menu_entry.h) and the portable AppImage's, so a desktop puts them together
        SDL_SetAppMetadata("Backbreaker", build_id::revision(), "backbreaker-recomp");
        if (!SDL_Init(SDL_INIT_VIDEO))
            fatal("[video] SDL_Init: %s (use --headless to run without a window)", SDL_GetError());
        g_settings = settings::load(paths::settingsFile());
        // The revision is in the title so a screenshot of a window identifies the checkout (#75).
        char openTitle[128];
        snprintf(openTitle, sizeof(openTitle), "Backbreaker (recomp) %s", build_id::revision());
        vk.window = SDL_CreateWindow(openTitle, g_settings.width, g_settings.height,
                                     SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!vk.window)
            fatal("[video] SDL_CreateWindow: %s (use --headless to run without a window)", SDL_GetError());
        setWindowIcon();
        if (g_settings.hasPosition)
            SDL_SetWindowPosition(vk.window, g_settings.x, g_settings.y);
        if (startFullscreen || g_settings.fullscreen)
            setFullscreen(true);
        hid::initGamepads(); // controllers are polled in this loop (SDL events live on this thread)
        createInstanceAndDevice();
        bool haveSwapchain = createSwapchain();
        info("[video] window open (%ux%u)", vk.swapExtent.width, vk.swapExtent.height);

        using clock = std::chrono::steady_clock;
        auto lastTitle = clock::now(), lastLog = lastTitle, lastPresent = lastTitle;
        uint64_t lastGuestFrame = 0, presents = 0, uploadedFrame = 0;
        g_frameSignal.store(SDL_CreateSemaphore(0), std::memory_order_release);
        bool running = true;
        while (running)
        {
            SDL_Event e;
            while (SDL_PollEvent(&e))
            {
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                    running = false;
                else if (overlay::handleEvent(e))
                    continue; // F1 and the open list's keys
                else if (e.type == SDL_EVENT_KEY_DOWN && (e.key.scancode == SDL_SCANCODE_F12 || e.key.key == SDLK_F12) && vk.shotSupported)
                {
                    if (!e.key.repeat) vk.shotWant = true; // one per press, captured on the next present so the picture is complete
                }
                else if (e.type == SDL_EVENT_KEY_DOWN && (e.key.scancode == SDL_SCANCODE_F10 || e.key.key == SDLK_F10) && !e.key.repeat)
                {
                    // F10: GPU frame capture (draws, state, textures) of a frame right after the
                    // press, into captures/frame_N/ - for "it looks wrong right now" moments
                    uint64_t f = g_frameNumber.load(std::memory_order_acquire) + 2;
                    gpu::capture::request(f);
                    info("[video] F10: capturing frame %llu into %s/frame_%llu/", (unsigned long long)f, paths::captures().c_str(), (unsigned long long)f);
                }
                else if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                    haveSwapchain = false;
                else if (e.type == SDL_EVENT_KEY_DOWN && e.key.down && !e.key.repeat &&
                         (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))))
                    setFullscreen(!g_fullscreen); // the pixel size event recreates the swapchain
            }
            if (!running) break;
            hid::pollGamepads();
            overlay::handlePad(hid::takeMenuPresses());

            if (!haveSwapchain)
            {
                { std::lock_guard g(g_ctx->queueLock); vkDeviceWaitIdle(vk.device); }
                destroySwapchain();
                haveSwapchain = createSwapchain();
                if (!haveSwapchain) { SDL_Delay(50); continue; } // minimized
            }
            if (g_presentPaced.load(std::memory_order_relaxed))
            {
                // One present for each new game frame, as soon as it is published. Without one, a
                // present every 20 ms keeps the achievements list moving in a loading screen, and
                // the loop still turns every 4 ms for the window's events and the controllers.
                if (g_frameNumber.load(std::memory_order_acquire) == g_presentedFrame &&
                    clock::now() - lastPresent < std::chrono::milliseconds(20))
                {
                    SDL_WaitSemaphoreTimeout(g_frameSignal.load(std::memory_order_relaxed), 4);
                    continue;
                }
            }
            if (!presentOnce(uploadedFrame))
                haveSwapchain = false;
            presents++;
            lastPresent = clock::now();

            auto now = clock::now();
            double dt = std::chrono::duration<double>(now - lastTitle).count();
            if (dt >= 0.5)
            {
                uint64_t f = g_frameNumber.load();
                char title[160];
                snprintf(title, sizeof(title), "Backbreaker (recomp) %s - game %.0f fps - display %.0f fps", build_id::revision(),
                         (f - lastGuestFrame) / dt, presents / dt);
                SDL_SetWindowTitle(vk.window, title);
                if (now - lastLog >= std::chrono::seconds(5))
                {
                    // Spelled out instead of slicing the title text: the title now starts with the
                    // revision, so a fixed offset into it meant nothing.
                    info("[video] game %.0f fps, display %.0f fps, window %ux%u", (f - lastGuestFrame) / dt,
                         presents / dt, vk.swapExtent.width, vk.swapExtent.height);
                    lastLog = now;
                }
                lastGuestFrame = f;
                presents = 0;
                lastTitle = now;
            }
        }

        if (vk.shotPending) { writeScreenshot(); vk.shotPending = false; } // closing right after a press still writes it
        for (auto& t : vk.shotWriters) t.join(); // let any in-flight PNG finish writing
        destroyShotBuffer();
        { std::lock_guard g(g_ctx->queueLock); vkDeviceWaitIdle(vk.device); }
        overlay::destroy(vk.device);
        info("[video] window closed");
        saveSettings(); // what the window was when it closed, for the next run
    }
}
