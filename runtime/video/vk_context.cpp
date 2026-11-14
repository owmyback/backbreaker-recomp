#include "vk_context.h"
#include "../log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
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

        // ---- validation layer messages, BB_VK_VALIDATION=1 (docs/runtime.md) ----
        // One messenger for the process, chained into VkInstanceCreateInfo::pNext so instance
        // creation is validated too. Installing ours turns off the layer's stdout messenger,
        // so every message goes through our logger with the [rt] prefix.
        struct ValMessage
        {
            int kind = 0;      // 0 info, 1 warning, 2 error
            uint64_t count = 0;
            std::string text;
        };
        std::mutex g_valLock;
        std::unordered_map<uint32_t, ValMessage> g_valMessages;
        // Plain counters, no lock: the summary is also printed from the crash and timeout handlers.
        std::atomic<uint64_t> g_valTotal{0}, g_valUnique{0}, g_valErrors{0}, g_valWarnings{0};
        bool useValidation = false; // the layer is loaded for this process
        VkDebugUtilsMessengerEXT g_messenger = VK_NULL_HANDLE; // never destroyed: the instance is not either
        constexpr uint64_t kLogRepeats = 3; // full copies of one message in the run log

        uint32_t messageId(const VkDebugUtilsMessengerCallbackDataEXT* d)
        {
            if (d->messageIdNumber) return uint32_t(d->messageIdNumber);
            uint32_t h = 2166136261u; // checks without an id: hash the text
            for (const char* p = d->pMessage; p && *p; p++) h = (h ^ uint8_t(*p)) * 16777619u;
            return h;
        }

        VKAPI_ATTR VkBool32 VKAPI_CALL onValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                           VkDebugUtilsMessageTypeFlagsEXT,
                                                           const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
        {
            if (!data->pMessage) return VK_FALSE;
            bool isError = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
            bool isWarn = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0;
            uint64_t n;
            {
                std::lock_guard guard(g_valLock);
                ValMessage& m = g_valMessages[messageId(data)];
                if (!m.count)
                {
                    m.kind = isError ? 2 : isWarn ? 1 : 0;
                    m.text = data->pMessage;
                    if (data->pMessageIdName) m.text = std::string(data->pMessageIdName) + ": " + m.text;
                    g_valUnique++;
                }
                m.count++;
                g_valTotal++;
                if (isError) g_valErrors++;
                else if (isWarn) g_valWarnings++;
                n = m.count;
            }
            if (n > kLogRepeats) return VK_FALSE; // counted, not repeated in the log
            for (size_t at = 0; at <= std::strlen(data->pMessage);)
            {
                const char* eol = std::strchr(data->pMessage + at, '\n');
                size_t len = eol ? size_t(eol - (data->pMessage + at)) : std::strlen(data->pMessage + at);
                std::string line(data->pMessage + at, len);
                if (isError) err("[vk-val] %s", line.c_str());
                else if (isWarn) warn("[vk-val] %s", line.c_str());
                else info("[vk-val] %s", line.c_str());
                at += len + 1;
            }
            if (n == kLogRepeats) info("[vk-val] (this message repeats; later copies are counted only)");
            return VK_FALSE; // a validation message never aborts the call that caused it
        }

        bool layerAvailable(const char* name)
        {
            uint32_t n = 0;
            vkEnumerateInstanceLayerProperties(&n, nullptr);
            std::vector<VkLayerProperties> layers(n);
            vkEnumerateInstanceLayerProperties(&n, layers.data());
            for (auto& l : layers)
                if (!strcmp(l.layerName, name)) return true;
            return false;
        }

        VkInstance createInstance(const char* const* exts, uint32_t extCount)
        {
            int level = validationLevel();
            std::vector<const char*> names(exts, exts + extCount);
            useValidation = level && layerAvailable("VK_LAYER_KHRONOS_validation");
            if (level && !useValidation)
                warn("[vk-val] VK_LAYER_KHRONOS_validation is not installed, running unvalidated "
                     "(Fedora: sudo dnf install vulkan-validation-layers)");
            VkDebugUtilsMessengerCreateInfoEXT dbg{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            if (useValidation)
            {
                names.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
                if (level >= 2)
                    dbg.messageSeverity |= VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
                dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
                dbg.pfnUserCallback = onValidationMessage;
            }
            VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
            app.pApplicationName = "backbreaker-recomp";
            app.apiVersion = VK_API_VERSION_1_3;
            VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
            ici.pApplicationInfo = &app;
            ici.enabledExtensionCount = uint32_t(names.size());
            ici.ppEnabledExtensionNames = names.data();
            if (useValidation)
            {
                static const char* validation = "VK_LAYER_KHRONOS_validation";
                ici.enabledLayerCount = 1;
                ici.ppEnabledLayerNames = &validation;
                ici.pNext = &dbg;
            }
            VkInstance instance = VK_NULL_HANDLE;
            if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return VK_NULL_HANDLE;
            if (useValidation)
            {
                // The messenger chained above only hears about the instance itself; device-level
                // messages need a messenger created by vkCreateDebugUtilsMessengerEXT, and having one
                // also turns off the layer's stdout output.
                auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
                if (create)
                    create(instance, &dbg, nullptr, &g_messenger);
                else
                    warn("[vk-val] vkCreateDebugUtilsMessengerEXT not available; validation output stays on stdout");
                info("[vk-val] validation layer on (BB_VK_VALIDATION=%d)", level);
            }
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
            VkPhysicalDeviceVulkan13Features have13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
            VkPhysicalDeviceVulkan12Features have12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
            have12.pNext = &have13;
            VkPhysicalDeviceFeatures2 have{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            have.pNext = &have12;
            vkGetPhysicalDeviceFeatures2(c.gpu, &have);
            VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
            f13.dynamicRendering = VK_TRUE;
            // shaderc declares DemoteToHelperInvocation in shaders that read textures with an
            // implicit LOD when targeting Vulkan 1.3; the module is invalid (VUID-
            // VkShaderModuleCreateInfo-pCode-08740) unless the feature is enabled. On a GPU without
            // it the renderer compiles for Vulkan 1.2 instead, where the capability is not used.
            f13.shaderDemoteToHelperInvocation = c.demoteHelper = have13.shaderDemoteToHelperInvocation;
            VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
            f12.pNext = &f13;
            f12.samplerMirrorClampToEdge = have12.samplerMirrorClampToEdge;
            VkPhysicalDeviceFeatures2 features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            features.pNext = &f12;
            features.features.geometryShader = have.features.geometryShader;
            features.features.depthClamp = have.features.depthClamp;
            features.features.independentBlend = have.features.independentBlend;
            features.features.fillModeNonSolid = have.features.fillModeNonSolid;
            features.features.samplerAnisotropy = c.samplerAnisotropy = have.features.samplerAnisotropy;
            features.features.sampleRateShading = have.features.sampleRateShading;
            // Xenos memory export writes guest memory from a vertex stage (see renderer.cpp).
            features.features.vertexPipelineStoresAndAtomics = have.features.vertexPipelineStoresAndAtomics;
            features.features.fragmentStoresAndAtomics = have.features.fragmentStoresAndAtomics;
            if (!have.features.geometryShader || !have.features.depthClamp || !have.features.independentBlend)
                warn("[vulkan] GPU lacks geometry shaders, depth clamp or independent blend; some draws will be wrong");
            if (!have12.samplerMirrorClampToEdge)
                warn("[vulkan] GPU lacks VK_KHR_sampler_mirror_clamp_to_edge; edge clamping of wrapped textures will be wrong");

            float priority = 1.0f;
            VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
            qci.queueFamilyIndex = c.queueFamily;
            qci.queueCount = 1;
            qci.pQueuePriorities = &priority;
            std::vector<const char*> exts = { VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME };
            if (surface) exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            c.stencilExport = hasExtension(c.gpu, VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
            if (c.stencilExport) exts.push_back(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
            c.hostMemoryImport = hasExtension(c.gpu, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) &&
                                 have.features.vertexPipelineStoresAndAtomics;
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

    int validationLevel()
    {
        static int level = [] {
            const char* v = getenv("BB_VK_VALIDATION");
            if (!v || !*v) return 0;
            int n = atoi(v);
            return n > 0 ? n : 1;
        }();
        return level;
    }

    void logValidationSummary()
    {
        if (!useValidation) return;
        uint64_t errors = g_valErrors, warnings = g_valWarnings;
        info("[vk-val] %llu validation message(s), %llu distinct: %llu error(s), %llu warning(s)%s",
             (unsigned long long)g_valTotal, (unsigned long long)g_valUnique.load(), (unsigned long long)errors,
             (unsigned long long)warnings, errors || warnings ? "" : " - clean run");
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
