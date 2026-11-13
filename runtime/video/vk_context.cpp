#include "vk_context.h"
#include "../log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <condition_variable>
#include <cstring>
#include <vector>

namespace vkctx
{
    namespace
    {
        std::mutex g_lock;
        std::condition_variable g_ready;
        Context* g_context = nullptr;
        bool g_expectWindow = false, g_headlessFailed = false;

        bool hasExtension(VkPhysicalDevice gpu, const char* name)
        {
            uint32_t n = 0;
            vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, nullptr);
            std::vector<VkExtensionProperties> exts(n);
            vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, exts.data());
            for (auto& e : exts)
                if (!strcmp(e.extensionName, name)) return true;
            return false;
        }

        VkInstance createInstance(const char* const* exts, uint32_t extCount)
        {
            VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
            app.pApplicationName = "backbreaker-recomp";
            app.apiVersion = VK_API_VERSION_1_3;
            VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
            ici.pApplicationInfo = &app;
            ici.enabledExtensionCount = extCount;
            ici.ppEnabledExtensionNames = exts;
            const char* validation = "VK_LAYER_KHRONOS_validation";
            if (getenv("BB_VK_VALIDATION"))
            {
                ici.enabledLayerCount = 1;
                ici.ppEnabledLayerNames = &validation;
            }
            VkInstance instance = VK_NULL_HANDLE;
            if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return VK_NULL_HANDLE;
            return instance;
        }

        // Pick a GPU (discrete preferred) with a graphics queue (that can present, with a surface)
        // and create the device. Returns false if there is no usable GPU.
        bool createDevice(Context& c, VkSurfaceKHR surface)
        {
            uint32_t n = 0;
            vkEnumeratePhysicalDevices(c.instance, &n, nullptr);
            std::vector<VkPhysicalDevice> gpus(n);
            vkEnumeratePhysicalDevices(c.instance, &n, gpus.data());
            int bestScore = -1;
            for (VkPhysicalDevice g : gpus)
            {
                VkPhysicalDeviceProperties p;
                vkGetPhysicalDeviceProperties(g, &p);
                if (p.apiVersion < VK_API_VERSION_1_3 || !hasExtension(g, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) continue;
                if (surface && !hasExtension(g, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
                uint32_t qn = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
                std::vector<VkQueueFamilyProperties> qs(qn);
                vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qs.data());
                for (uint32_t q = 0; q < qn; q++)
                {
                    VkBool32 present = VK_TRUE;
                    if (surface) vkGetPhysicalDeviceSurfaceSupportKHR(g, q, surface, &present);
                    if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
                    int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
                    if (score > bestScore) { bestScore = score; c.gpu = g; c.queueFamily = q; }
                    break;
                }
            }
            if (!c.gpu) return false;
            vkGetPhysicalDeviceProperties(c.gpu, &c.props);
            info("[vulkan] device: %s (Vulkan %u.%u)", c.props.deviceName, VK_API_VERSION_MAJOR(c.props.apiVersion),
                 VK_API_VERSION_MINOR(c.props.apiVersion));

            // Features the renderer uses (all supported by any current desktop GPU)
            VkPhysicalDeviceFeatures supported;
            vkGetPhysicalDeviceFeatures(c.gpu, &supported);
            VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
            f13.dynamicRendering = VK_TRUE;
            VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
            f12.pNext = &f13;
            f12.samplerMirrorClampToEdge = VK_TRUE;
            VkPhysicalDeviceFeatures2 features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            features.pNext = &f12;
            features.features.geometryShader = supported.geometryShader;
            features.features.depthClamp = supported.depthClamp;
            features.features.independentBlend = supported.independentBlend;
            features.features.fillModeNonSolid = supported.fillModeNonSolid;
            features.features.samplerAnisotropy = supported.samplerAnisotropy;
            features.features.sampleRateShading = supported.sampleRateShading;
            features.features.vertexPipelineStoresAndAtomics = supported.vertexPipelineStoresAndAtomics;
            features.features.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics;
            if (!supported.geometryShader || !supported.depthClamp || !supported.independentBlend)
                warn("[vulkan] GPU lacks geometry shaders, depth clamp or independent blend; some draws will be wrong");

            float priority = 1.0f;
            VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
            qci.queueFamilyIndex = c.queueFamily;
            qci.queueCount = 1;
            qci.pQueuePriorities = &priority;
            std::vector<const char*> exts = { VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME };
            if (surface) exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            c.stencilExport = hasExtension(c.gpu, VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
            if (c.stencilExport) exts.push_back(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
            c.hostMemoryImport = hasExtension(c.gpu, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) && supported.vertexPipelineStoresAndAtomics;
            if (c.hostMemoryImport)
            {
                exts.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
                VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProps{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
                VkPhysicalDeviceProperties2 p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
                p2.pNext = &hostProps;
                vkGetPhysicalDeviceProperties2(c.gpu, &p2);
                c.hostPointerAlignment = uint32_t(hostProps.minImportedHostPointerAlignment);
            }
            VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
            dci.pNext = &features;
            dci.queueCreateInfoCount = 1;
            dci.pQueueCreateInfos = &qci;
            dci.enabledExtensionCount = uint32_t(exts.size());
            dci.ppEnabledExtensionNames = exts.data();
            if (vkCreateDevice(c.gpu, &dci, nullptr, &c.device) != VK_SUCCESS) return false;
            vkGetDeviceQueue(c.device, c.queueFamily, 0, &c.queue);
            c.cmdPushDescriptorSet = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c.device, "vkCmdPushDescriptorSetKHR");
            if (c.hostMemoryImport)
                c.getMemoryHostPointerProperties =
                    (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(c.device, "vkGetMemoryHostPointerPropertiesEXT");
            return c.cmdPushDescriptorSet != nullptr;
        }
    }

    uint32_t Context::findMemory(uint32_t typeBits, VkMemoryPropertyFlags flags) const
    {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
        return UINT32_MAX;
    }

    void expectWindow()
    {
        std::lock_guard guard(g_lock);
        g_expectWindow = true;
    }

    Context& createForWindow(SDL_Window* window, VkSurfaceKHR& surface)
    {
        auto* c = new Context;
        Uint32 extCount = 0;
        const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&extCount);
        if (!exts) fatal("[video] SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError());
        c->instance = createInstance(exts, extCount);
        if (!c->instance) fatal("[video] vkCreateInstance failed");
        if (!SDL_Vulkan_CreateSurface(window, c->instance, nullptr, &surface))
            fatal("[video] SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        if (!createDevice(*c, surface))
            fatal("[video] no Vulkan 1.3 GPU with push descriptors can present to this window");
        std::lock_guard guard(g_lock);
        g_context = c;
        g_ready.notify_all();
        return *c;
    }

    Context* get()
    {
        std::unique_lock guard(g_lock);
        if (g_expectWindow)
        {
            g_ready.wait(guard, [] { return g_context != nullptr; });
            return g_context;
        }
        if (g_context || g_headlessFailed) return g_context;
        auto* c = new Context;
        c->instance = createInstance(nullptr, 0);
        if (!c->instance || !createDevice(*c, VK_NULL_HANDLE))
        {
            warn("[vulkan] no usable Vulkan 1.3 GPU; rendering is off for this headless run");
            g_headlessFailed = true;
            delete c;
            return nullptr;
        }
        g_context = c;
        return g_context;
    }
}
