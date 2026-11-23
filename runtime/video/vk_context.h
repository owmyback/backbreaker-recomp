#pragma once
// One Vulkan instance + device shared by the presenter (window, main thread) and the renderer
// (GPU emulation, command processor thread). In window mode the presenter creates it together
// with the window surface; headless runs create it without a surface on first use.
#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>

struct SDL_Window;

namespace vkctx
{
    struct Context
    {
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice gpu = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties props{};
        VkDevice device = VK_NULL_HANDLE;
        uint32_t queueFamily = 0;
        VkQueue queue = VK_NULL_HANDLE;
        PFN_vkCmdPushDescriptorSetKHR cmdPushDescriptorSet = nullptr;
        bool stencilExport = false; // VK_EXT_shader_stencil_export (EDRAM depth transfers keep stencil)
        bool demoteHelper = false;  // VkPhysicalDeviceVulkan13Features::shaderDemoteToHelperInvocation
        bool samplerAnisotropy = false; // enabled when the GPU has it (texture fetch constants ask for up to 16:1)
        bool sampleRateShading = false; // enabled when the GPU has it: interpolateAtCentroid (#166) needs it
        bool timelineSemaphore = false; // Vulkan 1.2 timeline semaphores: asynchronous guest fences (renderer.cpp)
        // VK_EXT_external_memory_host + vertexPipelineStoresAndAtomics: shaders can write guest
        // memory directly (Xenos memory export)
        bool hostMemoryImport = false;
        uint32_t hostPointerAlignment = 0;
        PFN_vkGetMemoryHostPointerPropertiesEXT getMemoryHostPointerProperties = nullptr;
        // VK_EXT_device_fault, when the driver has it: what went wrong, asked for when a call fails
        // (failed() below), since a driver's error code alone often says only "unknown" (#174)
        PFN_vkGetDeviceFaultInfoEXT getDeviceFaultInfo = nullptr;
        std::mutex queueLock; // the queue is used by both threads (vkQueueSubmit, vkQueuePresentKHR)

        // Memory type with all of `flags` among `typeBits`; UINT32_MAX if there is none.
        uint32_t findMemory(uint32_t typeBits, VkMemoryPropertyFlags flags) const;
    };

    // The process will open a window: get() waits for createForWindow instead of going headless.
    void expectWindow();

    // Presenter: create the instance, the window's surface and a device that can present to it.
    Context& createForWindow(SDL_Window* window, VkSurfaceKHR& surface);

    // Renderer: the shared context (waits for the window's), or a headless one.
    // nullptr if Vulkan is unavailable in a headless run.
    Context* get();

    // BB_VK_VALIDATION: 0 off, 1 errors+warnings, 2 also info/performance messages.
    int validationLevel();

    // One line per distinct validation message seen, with how many times it repeated.
    // Called at shutdown so a run's log says whether it was clean.
    void logValidationSummary();

    // A Vulkan call failed: the driver's fault report when it has one (VK_EXT_device_fault: its
    // description, the faulting addresses, the vendor's own codes), then the run ends (FATAL)
    [[noreturn]] void failed(const char* call, VkResult result);
}

#define VK_CHECK(x)                                                                     \
    do {                                                                                \
        VkResult r_ = (x);                                                              \
        if (r_ != VK_SUCCESS) vkctx::failed(#x, r_);                                    \
    } while (0)
