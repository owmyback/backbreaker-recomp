// SDL3 window + Vulkan swapchain that shows the guest front buffer.
//
// When the renderer (runtime/gpu/renderer.cpp) produced the swapped frame, its resolved front
// buffer image is blitted, letterboxed to 16:9, into the swapchain image. Otherwise (renderer
// off, or the frame was not rendered) the raw guest front buffer is copied from physical memory
// into a staging buffer, uploaded to a source image and blitted instead. One frame in flight.
// Both threads use one queue: the renderer submits a frame before publishing it, so queue
// submission order plus the barrier below order its writes before our read.
#include "presenter.h"
#include "vk_context.h"
#include "../hid/hid.h"
#include "../log.h"
#include "../memory.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <vector>

namespace video
{
    namespace
    {
        // ---- Published by the guest (command processor thread) ----
        std::atomic<uint64_t> g_frameNumber{0};
        std::atomic<uint32_t> g_front{0}, g_width{0}, g_height{0};
        // ---- Published by the renderer (same thread as above, before publishFrame) ----
        std::atomic<uint64_t> g_renderedFrame{0}, g_renderedImage{0};
        std::atomic<uint32_t> g_renderedWidth{0}, g_renderedHeight{0};
        vkctx::Context* g_ctx = nullptr;

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
        } vk;

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
            sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            sci.preTransform = caps.currentTransform;
            sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            sci.presentMode = VK_PRESENT_MODE_FIFO_KHR; // vsync; only this thread waits on it
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

        // Returns false if the swapchain must be recreated.
        bool presentOnce(uint64_t& uploadedFrame)
        {
            VK_CHECK(vkWaitForFences(vk.device, 1, &vk.inFlight, VK_TRUE, UINT64_MAX));
            uint32_t index = 0;
            VkResult r = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX, vk.acquired, VK_NULL_HANDLE, &index);
            if (r == VK_ERROR_OUT_OF_DATE_KHR) return false;
            if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) fatal("[video] vkAcquireNextImageKHR: %d", int(r));
            VK_CHECK(vkResetFences(vk.device, 1, &vk.inFlight));

            // New guest frame? Copy it into the staging buffer (the previous upload is complete:
            // we waited on the fence above).
            uint64_t frame = g_frameNumber.load(std::memory_order_acquire);
            uint32_t front = g_front.load(), width = g_width.load(), height = g_height.load();
            // Did the renderer produce this frame? Then show its image instead of the raw copy.
            VkImage rendered = VK_NULL_HANDLE;
            uint32_t renderedWidth = 0, renderedHeight = 0;
            if (frame && g_renderedFrame.load(std::memory_order_acquire) == frame)
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
            VK_CHECK(vkQueueSubmit(vk.queue, 1, &si, vk.inFlight));

            VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &vk.rendered[index];
            pi.swapchainCount = 1;
            pi.pSwapchains = &vk.swapchain;
            pi.pImageIndices = &index;
            r = vkQueuePresentKHR(vk.queue, &pi);
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) return false;
            if (r != VK_SUCCESS) fatal("[video] vkQueuePresentKHR: %d", int(r));
            return true;
        }
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
    }

    void run()
    {
        if (!SDL_Init(SDL_INIT_VIDEO))
            fatal("[video] SDL_Init: %s (use --headless to run without a window)", SDL_GetError());
        vk.window = SDL_CreateWindow("Backbreaker (recomp)", 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (!vk.window)
            fatal("[video] SDL_CreateWindow: %s (use --headless to run without a window)", SDL_GetError());
        hid::initGamepads(); // controllers are polled in this loop (SDL events live on this thread)
        createInstanceAndDevice();
        bool haveSwapchain = createSwapchain();
        info("[video] window open (%ux%u)", vk.swapExtent.width, vk.swapExtent.height);

        using clock = std::chrono::steady_clock;
        auto lastTitle = clock::now(), lastLog = lastTitle;
        uint64_t lastGuestFrame = 0, presents = 0, uploadedFrame = 0;
        bool running = true;
        while (running)
        {
            SDL_Event e;
            while (SDL_PollEvent(&e))
            {
                if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                    running = false;
                else if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                    haveSwapchain = false;
            }
            if (!running) break;
            hid::pollGamepads();

            if (!haveSwapchain)
            {
                { std::lock_guard g(g_ctx->queueLock); vkDeviceWaitIdle(vk.device); }
                destroySwapchain();
                haveSwapchain = createSwapchain();
                if (!haveSwapchain) { SDL_Delay(50); continue; } // minimized
            }
            if (!presentOnce(uploadedFrame))
                haveSwapchain = false;
            presents++;

            auto now = clock::now();
            double dt = std::chrono::duration<double>(now - lastTitle).count();
            if (dt >= 0.5)
            {
                uint64_t f = g_frameNumber.load();
                char title[160];
                snprintf(title, sizeof(title), "Backbreaker (recomp) - game %.0f fps - display %.0f fps", (f - lastGuestFrame) / dt,
                         presents / dt);
                SDL_SetWindowTitle(vk.window, title);
                if (now - lastLog >= std::chrono::seconds(5))
                {
                    info("[video] %s, window %ux%u", title + 23, vk.swapExtent.width, vk.swapExtent.height);
                    lastLog = now;
                }
                lastGuestFrame = f;
                presents = 0;
                lastTitle = now;
            }
        }

        { std::lock_guard g(g_ctx->queueLock); vkDeviceWaitIdle(vk.device); }
        info("[video] window closed");
    }
}
