/*
* Copyright (c) 2022 NVIDIA CORPORATION. All rights reserved
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

#include <unordered_set>

// sl_hooks.h keys off of this to know that Vulkan is in use.  Normally this is defined in vulkan_core.h when an
// app compiles it in.  In the case here, an early inclusion of sl_hooks.h (before vulkan.h) leads to it not being
// defined inside of SL itself.  This works around that issue
#define VK_VERSION_1_0 1

#include "include/sl.h"
#include "source/core/sl.api/internal.h"
#include "source/core/sl.log/log.h"
#include "source/core/sl.param/parameters.h"
#include "source/core/sl.plugin-manager/pluginManager.h"
#include "source/core/sl.interposer/vulkan/layer.h"
#include "source/core/sl.interposer/hook.h"
#include "include/sl_hooks.h"
#include "include/sl_helpers_vk.h"

HMODULE s_module = {};

using namespace sl::interposer;

VkTable s_vk{};
VkLayerInstanceDispatchTable s_idt{};
VkLayerDispatchTable s_ddt{};

HMODULE loadVulkanLibrary()
{
    if (!s_module)
    {
#ifdef SL_WINDOWS
        s_module = ::LoadLibraryA("vulkan-1.dll");
#else
        s_module = ::LoadLibraryA("vulkan-1.so");
#endif
    }
    return s_module;
}

//! Only used when manually hooking Vulkan API
//! 
//! Host is in charge and providing information we need
//! 
sl::Result processVulkanInterface(const sl::VulkanInfo* extension)
{
    if (!loadVulkanLibrary())
    {
        SL_LOG_ERROR( "Failed to load Vulkan library");
        return sl::Result::eErrorVulkanAPI;
    }

    s_vk.instance = extension->instance;
    s_vk.device = extension->device;
    s_vk.getDeviceProcAddr = (PFN_vkGetDeviceProcAddr)GetProcAddress(s_module, "vkGetDeviceProcAddr");
    s_vk.getInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(s_module, "vkGetInstanceProcAddr");
    s_vk.graphicsQueueFamily = extension->graphicsQueueFamily;
    s_vk.graphicsQueueIndex = extension->graphicsQueueIndex;
    s_vk.computeQueueFamily = extension->computeQueueFamily;
    s_vk.computeQueueIndex = extension->computeQueueIndex;    

    // Allow all plugins to access this information
    sl::param::getInterface()->set(sl::param::global::kVulkanTable, &s_vk);

    return sl::Result::eOk;
}

extern "C"
{

    PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName);
    PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName);

    VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance)
    {
        if (!loadVulkanLibrary())
        {
            SL_LOG_ERROR( "Failed to load Vulkan library");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        s_vk.getDeviceProcAddr = (PFN_vkGetDeviceProcAddr)GetProcAddress(s_module, "vkGetDeviceProcAddr");
        s_vk.getInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(s_module, "vkGetInstanceProcAddr");
        auto createInstance = (PFN_vkCreateInstance)GetProcAddress(s_module, "vkCreateInstance");

        if (!createInstance)
        {
            SL_LOG_ERROR( "Failed to map vkCreateInstance");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkApplicationInfo appInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        if (pCreateInfo->pApplicationInfo)
        {
            appInfo = *pCreateInfo->pApplicationInfo;
        }

        if (appInfo.apiVersion < VK_API_VERSION_1_3)
        {
            appInfo.apiVersion = VK_API_VERSION_1_3;
        }

        VkInstanceCreateInfo createInfo = *pCreateInfo;
        createInfo.pApplicationInfo = &appInfo;

        // Build up a list of extensions to enable
        std::unordered_set<std::string> extensionSet =
        {
#ifndef SL_PRODUCTION
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
#endif
        };

        auto pluginManager = sl::plugin_manager::getInterface();
        std::vector<json> configs;
        pluginManager->getLoadedFeatureConfigs(configs);
        for (auto& cfg : configs)
        {
            if (cfg.contains("/external/vk/instance/extensions"_json_pointer))
            {
                std::vector<std::string> pluginExtensions;
                cfg["external"]["vk"]["instance"]["extensions"].get_to(pluginExtensions);
                for (auto ext : pluginExtensions)
                {
                    auto res = extensionSet.insert(ext);
                    if (res.second)
                    {
                        SL_LOG_INFO("Adding instance extension '%s'", ext.c_str());
                    }
                }
            }
        }

        for (uint32_t i = 0; i < createInfo.enabledExtensionCount; i++)
        {
            extensionSet.insert(createInfo.ppEnabledExtensionNames[i]);
        }

        std::vector<const char*> extensions;
        extensions.reserve(extensionSet.size());
        for (auto& e : extensionSet)
        {
            extensions.push_back(e.c_str());
        }

        createInfo.enabledExtensionCount = (uint32_t)extensions.size();
        createInfo.ppEnabledExtensionNames = extensions.data();

#ifndef SL_PRODUCTION
        // Extra layers
        std::vector<const char*> layers;
        for (uint32_t i = 0; i < createInfo.enabledLayerCount; i++)
        {
            layers.push_back(createInfo.ppEnabledLayerNames[i]);
        }

        if (sl::interposer::getInterface()->getConfig().vkValidation)
        {
            if (std::find(layers.begin(), layers.end(), "VK_LAYER_KHRONOS_validation") == layers.end())
            {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                sl::param::getInterface()->set(sl::param::interposer::kVKValidationActive, true);
            }
        }

        createInfo.enabledLayerCount = (uint32_t)layers.size();
        createInfo.ppEnabledLayerNames = layers.data();
#endif
        auto res = createInstance(&createInfo, pAllocator, pInstance);
        if (res != VK_SUCCESS)
        {
            SL_LOG_ERROR( "vkCreateInstance failed");
            return res;
        }

        s_vk.instance = *pInstance;

        s_vk.mapVulkanInstanceAPI(s_vk.instance);

        s_idt = s_vk.dispatchInstanceMap[s_vk.instance];

        return VK_SUCCESS;
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties)
    {
        auto lib = loadVulkanLibrary();
        if (!lib)
        {
            SL_LOG_ERROR( "Failed to load Vulkan library");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        auto trampoline = (PFN_vkEnumerateInstanceExtensionProperties)GetProcAddress(lib, "vkEnumerateInstanceExtensionProperties");
        return trampoline(pLayerName, pPropertyCount, pProperties);
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties* pProperties)
    {
        auto lib = loadVulkanLibrary();
        if (!lib)
        {
            SL_LOG_ERROR( "Failed to load Vulkan library");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        auto trampoline = (PFN_vkEnumerateInstanceLayerProperties)GetProcAddress(lib, "vkEnumerateInstanceLayerProperties");
        return trampoline(pPropertyCount, pProperties);
    }

    VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice)
    {
        auto lib = loadVulkanLibrary();
        if (!lib)
        {
            SL_LOG_ERROR( "Failed to load Vulkan library");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto createInfo = *pCreateInfo;

        // Enable extra extensions SL requires
        std::unordered_set<std::string> extensionSet =
        {
            VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME
        };

        // Figure out what extra features we need 
        uint32_t extraGraphicsQueues = 0;
        uint32_t extraComputeQueues = 0;
        auto pluginManager = sl::plugin_manager::getInterface();
        std::vector<json> configs;
        pluginManager->getLoadedFeatureConfigs(configs);
        for (auto& cfg : configs)
        {
            // Device extensions
            if (cfg.contains("/external/vk/device/extensions"_json_pointer))
            {
                std::vector<std::string> pluginExtensions;
                cfg["external"]["vk"]["device"]["extensions"].get_to(pluginExtensions);
                for (auto ext : pluginExtensions)
                {
                    auto res = extensionSet.insert(ext);
                    if (res.second)
                    {
                        SL_LOG_INFO("Adding device extension '%s' requested by a plugin(s)", ext.c_str());
                    }
                }
            }
            // Additional queues?
            if (cfg.contains("/external/vk/device/queues/graphics/count"_json_pointer))
            {
                extraGraphicsQueues += cfg["external"]["vk"]["device"]["queues"]["graphics"]["count"];
                SL_LOG_INFO("Adding extra %u graphics queue(s) requested by a plugin(s)", extraGraphicsQueues);
            }
            if (cfg.contains("/external/vk/device/queues/compute/count"_json_pointer))
            {
                extraComputeQueues += cfg["external"]["vk"]["device"]["queues"]["compute"]["count"];
                SL_LOG_INFO("Adding extra %u compute queue(s) requested by a plugin(s)", extraComputeQueues);
            }
        }

        for (uint32_t i = 0; i < createInfo.enabledExtensionCount; i++)
        {
            extensionSet.insert(createInfo.ppEnabledExtensionNames[i]);
        }

        std::vector<const char*> extensions;
        extensions.reserve(extensionSet.size());
        for (auto& e : extensionSet)
        {
            // VK validation complains about 'VK_EXT_buffer_device_address' and 'bufferDeviceAddress' VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES set at the same time
            if (e != std::string("VK_EXT_buffer_device_address") && e != std::string("VK_KHR_buffer_device_address"))
            {
                extensions.push_back(e.c_str());
            }
        }

        createInfo.enabledExtensionCount = (uint32_t)extensions.size();
        createInfo.ppEnabledExtensionNames = extensions.data();

        // Check if host is already specifying 1.2 features
        auto features12 = (VkPhysicalDeviceVulkan12Features*)createInfo.pNext;
        while (features12)
        {
            if (features12->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
            {
                break;
            }
            features12 = (VkPhysicalDeviceVulkan12Features*)features12->pNext;
        }

        VkPhysicalDeviceVulkan12Features enable12Features{};

        // Update either existing features or add ours to the chain
        if (features12)
        {
            features12->timelineSemaphore = true;
            features12->descriptorIndexing = true;
            features12->bufferDeviceAddress = true;
        }
        else
        {
            enable12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            enable12Features.timelineSemaphore = true;
            enable12Features.descriptorIndexing = true;
            enable12Features.bufferDeviceAddress = true;
            enable12Features.pNext = (void*)createInfo.pNext;
            createInfo.pNext = &enable12Features;
        }

        auto& dt = s_vk.dispatchInstanceMap[s_vk.instance];

        // Queue family properties, used for setting up requested queues upon device creation
        uint32_t queueFamilyCount;
        s_idt.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilyProperties;
        queueFamilyProperties.resize(queueFamilyCount);
        s_idt.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilyProperties.data());

        s_vk.graphicsQueueFamily = 0;
        s_vk.computeQueueFamily = 0;
        for (uint32_t i = 0; i < static_cast<uint32_t>(queueFamilyProperties.size()); i++)
        {
            if ((queueFamilyProperties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            {
                SL_LOG_VERBOSE("Found Vulkan graphics queue family at index %u - max queues allowed %u", i, queueFamilyProperties[i].queueCount);
                s_vk.graphicsQueueFamily = i;
            }
            else if ((queueFamilyProperties[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
            {
                SL_LOG_VERBOSE("Found Vulkan compute queue family at index %u - max queues allowed %u", i, queueFamilyProperties[i].queueCount);
                s_vk.computeQueueFamily = i;
            }
        }

        // Check and add extra graphics and compute queues for SL workloads
        s_vk.computeQueueIndex = 0;
        s_vk.graphicsQueueIndex = 0;
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        for (uint32_t i = 0; i < createInfo.queueCreateInfoCount; i++)
        {
            queueCreateInfos.push_back(createInfo.pQueueCreateInfos[i]);
            if (createInfo.pQueueCreateInfos[i].queueFamilyIndex == s_vk.computeQueueFamily)
            {
                if (queueFamilyProperties[s_vk.computeQueueFamily].queueCount < queueCreateInfos.back().queueCount + extraComputeQueues)
                {
                    SL_LOG_WARN("SL feature(s) requiring more compute queues than available on this device");
                    continue;
                }
                s_vk.computeQueueIndex++;
                queueCreateInfos.back().queueCount += extraComputeQueues; // defaults to 0 unless requested otherwise by plugin(s)
            }
            if (createInfo.pQueueCreateInfos[i].queueFamilyIndex == s_vk.graphicsQueueFamily)
            {
                if (queueFamilyProperties[s_vk.graphicsQueueFamily].queueCount < queueCreateInfos.back().queueCount + extraGraphicsQueues)
                {
                    SL_LOG_WARN("SL feature(s) requiring more graphics queues than available on this device");
                    continue;
                }
                s_vk.graphicsQueueIndex++;
                queueCreateInfos.back().queueCount += extraGraphicsQueues; // defaults to 0 unless requested otherwise by plugin(s)
            }
        }

        const float defaultQueuePriority = 0.0f;
        VkDeviceQueueCreateInfo queueInfo{};

        if (extraComputeQueues > 0 && s_vk.computeQueueIndex == 0 && queueFamilyProperties[s_vk.computeQueueFamily].queueCount >= extraComputeQueues)
        {
            // We have to add compute queue(s) explicitly since host has none
            queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueInfo.queueFamilyIndex = s_vk.computeQueueFamily;
            queueInfo.queueCount = extraComputeQueues;
            queueInfo.pQueuePriorities = &defaultQueuePriority;
            queueCreateInfos.push_back(queueInfo);
        }

        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.queueCreateInfoCount = (uint32_t)queueCreateInfos.size();

        auto trampoline = (PFN_vkCreateDevice)GetProcAddress(lib, "vkCreateDevice");
        auto res = trampoline(physicalDevice, &createInfo, pAllocator, pDevice);

        if (res != VK_SUCCESS)
        {
            SL_LOG_ERROR( "vkCreateDevice failed");
            return res;
        }
        s_vk.instance = s_vk.instanceDeviceMap[physicalDevice];
        s_vk.mapVulkanInstanceAPI(s_vk.instance);
        s_idt = s_vk.dispatchInstanceMap[s_vk.instance];

        s_vk.device = *pDevice;
        s_vk.mapVulkanDeviceAPI(*pDevice);

        sl::param::getInterface()->set(sl::param::global::kVulkanTable, &s_vk);

        s_ddt = s_vk.dispatchDeviceMap[s_vk.device];

        pluginManager->setVulkanDevice(physicalDevice, *pDevice, s_vk.instance);
        pluginManager->initializePlugins();

        return res;
    }

    void VKAPI_CALL vkDestroyInstance(VkInstance Instance, const VkAllocationCallbacks* Allocator)
    {
        s_idt.DestroyInstance(Instance, Allocator);
        auto it = s_vk.instanceDeviceMap.begin();
        while (it != s_vk.instanceDeviceMap.end())
        {
            if ((*it).second == Instance)
            {
                it = s_vk.instanceDeviceMap.erase(it);
            }
            else
            {
                it++;
            }
        }
    }

    VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance Instance, uint32_t* PhysicalDeviceCount, VkPhysicalDevice* PhysicalDevices)
    {
        VkResult Result = s_idt.EnumeratePhysicalDevices(Instance, PhysicalDeviceCount, PhysicalDevices);
        if (PhysicalDevices)
        {
            auto i = *PhysicalDeviceCount;
            while (i--)
            {
                s_vk.instanceDeviceMap[PhysicalDevices[i]] = Instance;
            }
        }
        return Result;
    }

    void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceFeatures* Features)
    {
        s_idt.GetPhysicalDeviceFeatures(PhysicalDevice, Features);
    }

    void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice PhysicalDevice, VkFormat Format, VkFormatProperties* FormatProperties)
    {
        s_idt.GetPhysicalDeviceFormatProperties(PhysicalDevice, Format, FormatProperties);
    }

    VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice PhysicalDevice, VkFormat Format, VkImageType Type, VkImageTiling Tiling, VkImageUsageFlags Usage, VkImageCreateFlags Flags, VkImageFormatProperties* pImageFormatProperties)
    {
        return s_idt.GetPhysicalDeviceImageFormatProperties(PhysicalDevice, Format, Type, Tiling, Usage, Flags, pImageFormatProperties);
    }

    void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceProperties* Properties)
    {
        s_idt.GetPhysicalDeviceProperties(PhysicalDevice, Properties);
    }

    void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceMemoryProperties2* pMemoryProperties)
    {
        s_idt.GetPhysicalDeviceMemoryProperties2(PhysicalDevice, pMemoryProperties);
    }

    void VKAPI_CALL vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceProperties2KHR* Properties)
    {
        s_idt.GetPhysicalDeviceProperties2KHR(PhysicalDevice, Properties);
    }

    void VKAPI_CALL vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceFeatures2KHR* Features)
    {
        s_idt.GetPhysicalDeviceFeatures2KHR(PhysicalDevice, Features);
    }

    void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice PhysicalDevice, uint32_t* QueueFamilyPropertyCount, VkQueueFamilyProperties* QueueFamilyProperties)
    {
        s_idt.GetPhysicalDeviceQueueFamilyProperties(PhysicalDevice, QueueFamilyPropertyCount, QueueFamilyProperties);
    }

    void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice PhysicalDevice, VkPhysicalDeviceMemoryProperties* MemoryProperties)
    {
        s_idt.GetPhysicalDeviceMemoryProperties(PhysicalDevice, MemoryProperties);
    }


    void VKAPI_CALL vkDestroyDevice(VkDevice Device, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyDevice(Device, Allocator);
    }

    VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice PhysicalDevice, const char* LayerName, uint32_t* PropertyCount, VkExtensionProperties* Properties)
    {
        return s_idt.EnumerateDeviceExtensionProperties(PhysicalDevice, LayerName, PropertyCount, Properties);
    }

    VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice PhysicalDevice, uint32_t* PropertyCount, VkLayerProperties* Properties)
    {
        return s_idt.EnumerateDeviceLayerProperties(PhysicalDevice, PropertyCount, Properties);
    }

    void VKAPI_CALL vkGetDeviceQueue(VkDevice Device, uint32_t QueueFamilyIndex, uint32_t QueueIndex, VkQueue* Queue)
    {
        s_ddt.GetDeviceQueue(Device, QueueFamilyIndex, QueueIndex, Queue);
    }

    VkResult VKAPI_CALL vkQueueSubmit(VkQueue Queue, uint32_t SubmitCount, const VkSubmitInfo* Submits, VkFence Fence)
    {
        return s_ddt.QueueSubmit(Queue, SubmitCount, Submits, Fence);
    }
    VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue Queue)
    {
        return s_ddt.QueueWaitIdle(Queue);
    }

    VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice Device)
    {
        return s_ddt.DeviceWaitIdle(Device);
    }

    VkResult VKAPI_CALL vkAllocateMemory(VkDevice Device, const VkMemoryAllocateInfo* AllocateInfo, const VkAllocationCallbacks* Allocator, VkDeviceMemory* Memory)
    {
        return s_ddt.AllocateMemory(Device, AllocateInfo, Allocator, Memory);
    }

    void VKAPI_CALL vkFreeMemory(VkDevice Device, VkDeviceMemory Memory, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.FreeMemory(Device, Memory, Allocator);
    }

    VkResult VKAPI_CALL vkMapMemory(VkDevice Device, VkDeviceMemory Memory, VkDeviceSize Offset, VkDeviceSize Size, VkMemoryMapFlags Flags, void** Data)
    {
        return s_ddt.MapMemory(Device, Memory, Offset, Size, Flags, Data);
    }

    void VKAPI_CALL vkUnmapMemory(VkDevice Device, VkDeviceMemory Memory)
    {
        s_ddt.UnmapMemory(Device, Memory);
    }

    VkResult VKAPI_CALL vkFlushMappedMemoryRanges(VkDevice Device, uint32_t MemoryRangeCount, const VkMappedMemoryRange* MemoryRanges)
    {
        return s_ddt.FlushMappedMemoryRanges(Device, MemoryRangeCount, MemoryRanges);
    }

    VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges(VkDevice Device, uint32_t MemoryRangeCount, const VkMappedMemoryRange* MemoryRanges)
    {
        return s_ddt.InvalidateMappedMemoryRanges(Device, MemoryRangeCount, MemoryRanges);
    }

    void VKAPI_CALL vkGetDeviceMemoryCommitment(VkDevice Device, VkDeviceMemory Memory, VkDeviceSize* pCommittedMemoryInBytes)
    {
        s_ddt.GetDeviceMemoryCommitment(Device, Memory, pCommittedMemoryInBytes);
    }

    VkResult VKAPI_CALL vkBindBufferMemory(VkDevice Device, VkBuffer Buffer, VkDeviceMemory Memory, VkDeviceSize MemoryOffset)
    {
        return s_ddt.BindBufferMemory(Device, Buffer, Memory, MemoryOffset);
    }

    VkResult VKAPI_CALL vkBindImageMemory(VkDevice Device, VkImage Image, VkDeviceMemory Memory, VkDeviceSize MemoryOffset)
    {
        return s_ddt.BindImageMemory(Device, Image, Memory, MemoryOffset);
    }

    void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice Device, VkBuffer Buffer, VkMemoryRequirements* MemoryRequirements)
    {
        s_ddt.GetBufferMemoryRequirements(Device, Buffer, MemoryRequirements);
    }

    void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice Device, VkImage Image, VkMemoryRequirements* MemoryRequirements)
    {
        s_ddt.GetImageMemoryRequirements(Device, Image, MemoryRequirements);
    }

    void VKAPI_CALL vkGetImageSparseMemoryRequirements(VkDevice Device, VkImage Image, uint32_t* pSparseMemoryRequirementCount, VkSparseImageMemoryRequirements* pSparseMemoryRequirements)
    {
        s_ddt.GetImageSparseMemoryRequirements(Device, Image, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
    }

    void VKAPI_CALL vkGetPhysicalDeviceSparseImageFormatProperties(VkPhysicalDevice PhysicalDevice, VkFormat Format, VkImageType Type, VkSampleCountFlagBits Samples, VkImageUsageFlags Usage, VkImageTiling Tiling, uint32_t* pPropertyCount, VkSparseImageFormatProperties* pProperties)
    {
        s_idt.GetPhysicalDeviceSparseImageFormatProperties(PhysicalDevice, Format, Type, Samples, Usage, Tiling, pPropertyCount, pProperties);
    }

    VkResult VKAPI_CALL vkQueueBindSparse(VkQueue Queue, uint32_t BindInfoCount, const VkBindSparseInfo* pBindInfo, VkFence Fence)
    {
        return s_ddt.QueueBindSparse(Queue, BindInfoCount, pBindInfo, Fence);
    }

    VkResult VKAPI_CALL vkCreateFence(VkDevice Device, const VkFenceCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkFence* Fence)
    {
        return s_ddt.CreateFence(Device, CreateInfo, Allocator, Fence);
    }

    void VKAPI_CALL vkDestroyFence(VkDevice Device, VkFence Fence, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyFence(Device, Fence, Allocator);
    }

    VkResult VKAPI_CALL vkResetFences(VkDevice Device, uint32_t FenceCount, const VkFence* Fences)
    {
        return s_ddt.ResetFences(Device, FenceCount, Fences);
    }

    VkResult VKAPI_CALL vkGetFenceStatus(VkDevice Device, VkFence Fence)
    {
        return s_ddt.GetFenceStatus(Device, Fence);
    }

    VkResult VKAPI_CALL vkWaitForFences(VkDevice Device, uint32_t FenceCount, const VkFence* Fences, VkBool32 bWaitAll, uint64_t Timeout)
    {
        return s_ddt.WaitForFences(Device, FenceCount, Fences, bWaitAll, Timeout);
    }

    VkResult VKAPI_CALL vkCreateSemaphore(VkDevice Device, const VkSemaphoreCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkSemaphore* Semaphore)
    {
        return s_ddt.CreateSemaphore(Device, CreateInfo, Allocator, Semaphore);
    }


    void VKAPI_CALL vkDestroySemaphore(VkDevice Device, VkSemaphore Semaphore, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroySemaphore(Device, Semaphore, Allocator);
    }

    VkResult VKAPI_CALL vkCreateEvent(VkDevice Device, const VkEventCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkEvent* Event)
    {
        return s_ddt.CreateEvent(Device, CreateInfo, Allocator, Event);
    }

    void VKAPI_CALL vkDestroyEvent(VkDevice Device, VkEvent Event, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyEvent(Device, Event, Allocator);
    }

    VkResult VKAPI_CALL vkGetEventStatus(VkDevice Device, VkEvent Event)
    {
        return s_ddt.GetEventStatus(Device, Event);
    }

    VkResult VKAPI_CALL vkSetEvent(VkDevice Device, VkEvent Event)
    {
        return s_ddt.SetEvent(Device, Event);
    }

    VkResult VKAPI_CALL vkResetEvent(VkDevice Device, VkEvent Event)
    {
        return s_ddt.ResetEvent(Device, Event);
    }

    VkResult VKAPI_CALL vkCreateQueryPool(VkDevice Device, const VkQueryPoolCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkQueryPool* QueryPool)
    {
        return s_ddt.CreateQueryPool(Device, CreateInfo, Allocator, QueryPool);
    }

    void VKAPI_CALL vkDestroyQueryPool(VkDevice Device, VkQueryPool QueryPool, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyQueryPool(Device, QueryPool, Allocator);
    }

    VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice Device, VkQueryPool QueryPool,
        uint32_t FirstQuery, uint32_t QueryCount, size_t DataSize, void* Data, VkDeviceSize Stride, VkQueryResultFlags Flags)
    {
        return s_ddt.GetQueryPoolResults(Device, QueryPool, FirstQuery, QueryCount, DataSize, Data, Stride, Flags);
    }

    VkResult VKAPI_CALL vkCreateBuffer(VkDevice Device, const VkBufferCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkBuffer* Buffer)
    {
        return s_ddt.CreateBuffer(Device, CreateInfo, Allocator, Buffer);
    }

    void VKAPI_CALL vkDestroyBuffer(VkDevice Device, VkBuffer Buffer, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyBuffer(Device, Buffer, Allocator);
    }

    VkResult VKAPI_CALL vkCreateBufferView(VkDevice Device, const VkBufferViewCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkBufferView* View)
    {
        return s_ddt.CreateBufferView(Device, CreateInfo, Allocator, View);
    }

    void VKAPI_CALL vkDestroyBufferView(VkDevice Device, VkBufferView BufferView, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyBufferView(Device, BufferView, Allocator);
    }

    VkResult VKAPI_CALL vkCreateImage(VkDevice Device, const VkImageCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkImage* Image)
    {
        return s_ddt.CreateImage(Device, CreateInfo, Allocator, Image);
    }

    void VKAPI_CALL vkDestroyImage(VkDevice Device, VkImage Image, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyImage(Device, Image, Allocator);
    }

    void VKAPI_CALL vkGetImageSubresourceLayout(VkDevice Device, VkImage Image, const VkImageSubresource* Subresource, VkSubresourceLayout* Layout)
    {
        s_ddt.GetImageSubresourceLayout(Device, Image, Subresource, Layout);
    }

    VkResult VKAPI_CALL vkCreateImageView(VkDevice Device, const VkImageViewCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkImageView* View)
    {
        return s_ddt.CreateImageView(Device, CreateInfo, Allocator, View);
    }

    void VKAPI_CALL vkDestroyImageView(VkDevice Device, VkImageView ImageView, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyImageView(Device, ImageView, Allocator);
    }

    VkResult VKAPI_CALL vkCreateShaderModule(VkDevice Device, const VkShaderModuleCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkShaderModule* ShaderModule)
    {
        return s_ddt.CreateShaderModule(Device, CreateInfo, Allocator, ShaderModule);
    }

    void VKAPI_CALL vkDestroyShaderModule(VkDevice Device, VkShaderModule ShaderModule, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyShaderModule(Device, ShaderModule, Allocator);
    }

    VkResult VKAPI_CALL vkCreatePipelineCache(VkDevice Device, const VkPipelineCacheCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkPipelineCache* PipelineCache)
    {
        return s_ddt.CreatePipelineCache(Device, CreateInfo, Allocator, PipelineCache);
    }

    void VKAPI_CALL vkDestroyPipelineCache(VkDevice Device, VkPipelineCache PipelineCache, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyPipelineCache(Device, PipelineCache, Allocator);
    }

    VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice Device, VkPipelineCache PipelineCache, size_t* DataSize, void* Data)
    {
        return s_ddt.GetPipelineCacheData(Device, PipelineCache, DataSize, Data);
    }

    VkResult VKAPI_CALL vkMergePipelineCaches(VkDevice Device, VkPipelineCache DestCache, uint32_t SourceCacheCount, const VkPipelineCache* SrcCaches)
    {
        return s_ddt.MergePipelineCaches(Device, DestCache, SourceCacheCount, SrcCaches);
    }

    VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice Device, VkPipelineCache PipelineCache, uint32_t CreateInfoCount, const VkGraphicsPipelineCreateInfo* CreateInfos, const VkAllocationCallbacks* Allocator, VkPipeline* Pipelines)
    {
        return s_ddt.CreateGraphicsPipelines(Device, PipelineCache, CreateInfoCount, CreateInfos, Allocator, Pipelines);
    }

    VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice Device, VkPipelineCache PipelineCache, uint32_t CreateInfoCount, const VkComputePipelineCreateInfo* CreateInfos, const VkAllocationCallbacks* Allocator, VkPipeline* Pipelines)
    {
        return s_ddt.CreateComputePipelines(Device, PipelineCache, CreateInfoCount, CreateInfos, Allocator, Pipelines);
    }

    void VKAPI_CALL vkDestroyPipeline(VkDevice Device, VkPipeline Pipeline, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyPipeline(Device, Pipeline, Allocator);
    }

    VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice Device, const VkPipelineLayoutCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkPipelineLayout* PipelineLayout)
    {
        return s_ddt.CreatePipelineLayout(Device, CreateInfo, Allocator, PipelineLayout);
    }

    void VKAPI_CALL vkDestroyPipelineLayout(VkDevice Device, VkPipelineLayout PipelineLayout, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyPipelineLayout(Device, PipelineLayout, Allocator);
    }

    VkResult VKAPI_CALL vkCreateSampler(VkDevice Device, const VkSamplerCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkSampler* Sampler)
    {
        return s_ddt.CreateSampler(Device, CreateInfo, Allocator, Sampler);
    }

    void VKAPI_CALL vkDestroySampler(VkDevice Device, VkSampler Sampler, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroySampler(Device, Sampler, Allocator);
    }

    VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice Device, const VkDescriptorSetLayoutCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkDescriptorSetLayout* SetLayout)
    {
        return s_ddt.CreateDescriptorSetLayout(Device, CreateInfo, Allocator, SetLayout);
    }

    void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice Device, VkDescriptorSetLayout DescriptorSetLayout, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyDescriptorSetLayout(Device, DescriptorSetLayout, Allocator);
    }

    VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice Device, const VkDescriptorPoolCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkDescriptorPool* DescriptorPool)
    {
        return s_ddt.CreateDescriptorPool(Device, CreateInfo, Allocator, DescriptorPool);
    }

    void VKAPI_CALL vkDestroyDescriptorPool(VkDevice Device, VkDescriptorPool DescriptorPool, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyDescriptorPool(Device, DescriptorPool, Allocator);
    }

    VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice Device, VkDescriptorPool DescriptorPool, VkDescriptorPoolResetFlags Flags)
    {
        return s_ddt.ResetDescriptorPool(Device, DescriptorPool, Flags);
    }

    VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice Device, const VkDescriptorSetAllocateInfo* AllocateInfo, VkDescriptorSet* DescriptorSets)
    {
        return s_ddt.AllocateDescriptorSets(Device, AllocateInfo, DescriptorSets);
    }

    VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice Device, VkDescriptorPool DescriptorPool, uint32_t DescriptorSetCount, const VkDescriptorSet* DescriptorSets)
    {
        return s_ddt.FreeDescriptorSets(Device, DescriptorPool, DescriptorSetCount, DescriptorSets);
    }

    void VKAPI_CALL vkUpdateDescriptorSets(VkDevice Device, uint32_t DescriptorWriteCount, const VkWriteDescriptorSet* DescriptorWrites, uint32_t DescriptorCopyCount, const VkCopyDescriptorSet* DescriptorCopies)
    {
        s_ddt.UpdateDescriptorSets(Device, DescriptorWriteCount, DescriptorWrites, DescriptorCopyCount, DescriptorCopies);
    }

    VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice Device, const VkFramebufferCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkFramebuffer* Framebuffer)
    {
        return s_ddt.CreateFramebuffer(Device, CreateInfo, Allocator, Framebuffer);
    }

    void VKAPI_CALL vkDestroyFramebuffer(VkDevice Device, VkFramebuffer Framebuffer, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyFramebuffer(Device, Framebuffer, Allocator);
    }

    VkResult VKAPI_CALL vkCreateRenderPass(VkDevice Device, const VkRenderPassCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkRenderPass* RenderPass)
    {
        return s_ddt.CreateRenderPass(Device, CreateInfo, Allocator, RenderPass);
    }

    void VKAPI_CALL vkDestroyRenderPass(VkDevice Device, VkRenderPass RenderPass, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyRenderPass(Device, RenderPass, Allocator);
    }

    void VKAPI_CALL vkGetRenderAreaGranularity(VkDevice Device, VkRenderPass RenderPass, VkExtent2D* pGranularity)
    {
        s_ddt.GetRenderAreaGranularity(Device, RenderPass, pGranularity);
    }

    VkResult VKAPI_CALL vkCreateCommandPool(VkDevice Device, const VkCommandPoolCreateInfo* CreateInfo, const VkAllocationCallbacks* Allocator, VkCommandPool* CommandPool)
    {
        return s_ddt.CreateCommandPool(Device, CreateInfo, Allocator, CommandPool);
    }

    void VKAPI_CALL vkDestroyCommandPool(VkDevice Device, VkCommandPool CommandPool, const VkAllocationCallbacks* Allocator)
    {
        s_ddt.DestroyCommandPool(Device, CommandPool, Allocator);
    }

    VkResult VKAPI_CALL vkResetCommandPool(VkDevice Device, VkCommandPool CommandPool, VkCommandPoolResetFlags Flags)
    {
        return s_ddt.ResetCommandPool(Device, CommandPool, Flags);
    }

    VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice Device, const VkCommandBufferAllocateInfo* AllocateInfo, VkCommandBuffer* CommandBuffers)
    {
        return s_ddt.AllocateCommandBuffers(Device, AllocateInfo, CommandBuffers);
    }

    void VKAPI_CALL vkFreeCommandBuffers(VkDevice Device, VkCommandPool CommandPool, uint32_t CommandBufferCount, const VkCommandBuffer* CommandBuffers)
    {
        s_ddt.FreeCommandBuffers(Device, CommandPool, CommandBufferCount, CommandBuffers);
    }

    VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer CommandBuffer, const VkCommandBufferBeginInfo* BeginInfo)
    {
        auto res = s_ddt.BeginCommandBuffer(CommandBuffer, BeginInfo);

        return res;
    }

    VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer CommandBuffer)
    {
        return s_ddt.EndCommandBuffer(CommandBuffer);
    }

    VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer CommandBuffer, VkCommandBufferResetFlags Flags)
    {
        return s_ddt.ResetCommandBuffer(CommandBuffer, Flags);
    }

    void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer CommandBuffer, VkPipelineBindPoint PipelineBindPoint, VkPipeline Pipeline)
    {
        s_ddt.CmdBindPipeline(CommandBuffer, PipelineBindPoint, Pipeline);
    }

    void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer CommandBuffer, uint32_t FirstViewport, uint32_t ViewportCount, const VkViewport* Viewports)
    {
        s_ddt.CmdSetViewport(CommandBuffer, FirstViewport, ViewportCount, Viewports);
    }

    void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer CommandBuffer, uint32_t FirstScissor, uint32_t ScissorCount, const VkRect2D* Scissors)
    {
        s_ddt.CmdSetScissor(CommandBuffer, FirstScissor, ScissorCount, Scissors);
    }

    void VKAPI_CALL vkCmdSetLineWidth(VkCommandBuffer CommandBuffer, float LineWidth)
    {
        s_ddt.CmdSetLineWidth(CommandBuffer, LineWidth);
    }

    void VKAPI_CALL vkCmdSetDepthBias(VkCommandBuffer CommandBuffer, float DepthBiasConstantFactor, float DepthBiasClamp, float DepthBiasSlopeFactor)
    {
        s_ddt.CmdSetDepthBias(CommandBuffer, DepthBiasConstantFactor, DepthBiasClamp, DepthBiasSlopeFactor);
    }

    void VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer CommandBuffer, const float BlendConstants[4])
    {
        s_ddt.CmdSetBlendConstants(CommandBuffer, BlendConstants);
    }

    void VKAPI_CALL vkCmdSetDepthBounds(VkCommandBuffer CommandBuffer, float MinDepthBounds, float MaxDepthBounds)
    {
        s_ddt.CmdSetDepthBounds(CommandBuffer, MinDepthBounds, MaxDepthBounds);
    }

    void VKAPI_CALL vkCmdSetStencilCompareMask(VkCommandBuffer CommandBuffer, VkStencilFaceFlags FaceMask, uint32_t CompareMask)
    {
        s_ddt.CmdSetStencilCompareMask(CommandBuffer, FaceMask, CompareMask);
    }

    void VKAPI_CALL vkCmdSetStencilWriteMask(VkCommandBuffer CommandBuffer, VkStencilFaceFlags FaceMask, uint32_t WriteMask)
    {
        s_ddt.CmdSetStencilWriteMask(CommandBuffer, FaceMask, WriteMask);
    }

    void VKAPI_CALL vkCmdSetStencilReference(VkCommandBuffer CommandBuffer, VkStencilFaceFlags FaceMask, uint32_t Reference)
    {
        s_ddt.CmdSetStencilReference(CommandBuffer, FaceMask, Reference);
    }

    void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer CommandBuffer, VkPipelineBindPoint PipelineBindPoint, VkPipelineLayout Layout, uint32_t FirstSet, uint32_t DescriptorSetCount, const VkDescriptorSet* DescriptorSets, uint32_t DynamicOffsetCount, const uint32_t* DynamicOffsets)
    {
        s_ddt.CmdBindDescriptorSets(CommandBuffer, PipelineBindPoint, Layout, FirstSet, DescriptorSetCount, DescriptorSets, DynamicOffsetCount, DynamicOffsets);
    }

    void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer CommandBuffer, VkBuffer Buffer, VkDeviceSize Offset, VkIndexType IndexType)
    {
        s_ddt.CmdBindIndexBuffer(CommandBuffer, Buffer, Offset, IndexType);
    }

    void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer CommandBuffer, uint32_t FirstBinding, uint32_t BindingCount, const VkBuffer* Buffers, const VkDeviceSize* Offsets)
    {
        s_ddt.CmdBindVertexBuffers(CommandBuffer, FirstBinding, BindingCount, Buffers, Offsets);
    }

    void VKAPI_CALL vkCmdDraw(VkCommandBuffer CommandBuffer, uint32_t VertexCount, uint32_t InstanceCount, uint32_t FirstVertex, uint32_t FirstInstance)
    {
        s_ddt.CmdDraw(CommandBuffer, VertexCount, InstanceCount, FirstVertex, FirstInstance);
    }

    void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer CommandBuffer, uint32_t IndexCount, uint32_t InstanceCount, uint32_t FirstIndex, int32_t VertexOffset, uint32_t FirstInstance)
    {
        s_ddt.CmdDrawIndexed(CommandBuffer, IndexCount, InstanceCount, FirstIndex, VertexOffset, FirstInstance);
    }

    void VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer CommandBuffer, VkBuffer Buffer, VkDeviceSize Offset, uint32_t DrawCount, uint32_t Stride)
    {
        s_ddt.CmdDrawIndirect(CommandBuffer, Buffer, Offset, DrawCount, Stride);
    }

    void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer CommandBuffer, VkBuffer Buffer, VkDeviceSize Offset, uint32_t DrawCount, uint32_t Stride)
    {
        s_ddt.CmdDrawIndexedIndirect(CommandBuffer, Buffer, Offset, DrawCount, Stride);
    }

    void VKAPI_CALL vkCmdDispatch(VkCommandBuffer CommandBuffer, uint32_t X, uint32_t Y, uint32_t Z)
    {
        s_ddt.CmdDispatch(CommandBuffer, X, Y, Z);
    }

    void VKAPI_CALL vkCmdDispatchIndirect(VkCommandBuffer CommandBuffer, VkBuffer Buffer, VkDeviceSize Offset)
    {
        s_ddt.CmdDispatchIndirect(CommandBuffer, Buffer, Offset);
    }

    void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer CommandBuffer, VkBuffer SrcBuffer, VkBuffer DstBuffer, uint32_t RegionCount, const VkBufferCopy* Regions)
    {
        s_ddt.CmdCopyBuffer(CommandBuffer, SrcBuffer, DstBuffer, RegionCount, Regions);
    }

    void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer CommandBuffer, VkImage SrcImage, VkImageLayout SrcImageLayout, VkImage DstImage, VkImageLayout DstImageLayout, uint32_t RegionCount, const VkImageCopy* Regions)
    {
        s_ddt.CmdCopyImage(CommandBuffer, SrcImage, SrcImageLayout, DstImage, DstImageLayout, RegionCount, Regions);
    }

    void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer CommandBuffer, VkImage SrcImage, VkImageLayout SrcImageLayout, VkImage DstImage, VkImageLayout DstImageLayout, uint32_t RegionCount, const VkImageBlit* Regions, VkFilter Filter)
    {
        s_ddt.CmdBlitImage(CommandBuffer, SrcImage, SrcImageLayout, DstImage, DstImageLayout, RegionCount, Regions, Filter);
    }

    void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer CommandBuffer, VkBuffer SrcBuffer, VkImage DstImage, VkImageLayout DstImageLayout, uint32_t RegionCount, const VkBufferImageCopy* Regions)
    {
        s_ddt.CmdCopyBufferToImage(CommandBuffer, SrcBuffer, DstImage, DstImageLayout, RegionCount, Regions);
    }

    void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer CommandBuffer, VkImage SrcImage, VkImageLayout SrcImageLayout, VkBuffer DstBuffer, uint32_t RegionCount, const VkBufferImageCopy* Regions)
    {
        s_ddt.CmdCopyImageToBuffer(CommandBuffer, SrcImage, SrcImageLayout, DstBuffer, RegionCount, Regions);
    }

    void VKAPI_CALL vkCmdUpdateBuffer(VkCommandBuffer CommandBuffer, VkBuffer DstBuffer, VkDeviceSize DstOffset, VkDeviceSize DataSize, const void* pData)
    {
        s_ddt.CmdUpdateBuffer(CommandBuffer, DstBuffer, DstOffset, DataSize, pData);
    }

    void VKAPI_CALL vkCmdFillBuffer(VkCommandBuffer CommandBuffer, VkBuffer DstBuffer, VkDeviceSize DstOffset, VkDeviceSize Size, uint32_t Data)
    {
        s_ddt.CmdFillBuffer(CommandBuffer, DstBuffer, DstOffset, Size, Data);
    }

    void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer CommandBuffer, VkImage Image, VkImageLayout ImageLayout, const VkClearColorValue* Color, uint32_t RangeCount, const VkImageSubresourceRange* Ranges)
    {
        s_ddt.CmdClearColorImage(CommandBuffer, Image, ImageLayout, Color, RangeCount, Ranges);
    }

    void VKAPI_CALL vkCmdClearDepthStencilImage(VkCommandBuffer CommandBuffer, VkImage Image, VkImageLayout ImageLayout, const VkClearDepthStencilValue* DepthStencil, uint32_t RangeCount, const VkImageSubresourceRange* Ranges)
    {
        s_ddt.CmdClearDepthStencilImage(CommandBuffer, Image, ImageLayout, DepthStencil, RangeCount, Ranges);
    }

    void VKAPI_CALL vkCmdClearAttachments(VkCommandBuffer CommandBuffer, uint32_t AttachmentCount, const VkClearAttachment* Attachments, uint32_t RectCount, const VkClearRect* Rects)
    {
        s_ddt.CmdClearAttachments(CommandBuffer, AttachmentCount, Attachments, RectCount, Rects);
    }

    void VKAPI_CALL vkCmdResolveImage(
        VkCommandBuffer CommandBuffer,
        VkImage SrcImage, VkImageLayout SrcImageLayout,
        VkImage DstImage, VkImageLayout DstImageLayout,
        uint32_t RegionCount, const VkImageResolve* Regions)
    {
        s_ddt.CmdResolveImage(CommandBuffer, SrcImage, SrcImageLayout, DstImage, DstImageLayout, RegionCount, Regions);
    }

    void VKAPI_CALL vkCmdSetEvent(VkCommandBuffer CommandBuffer, VkEvent Event, VkPipelineStageFlags StageMask)
    {
        s_ddt.CmdSetEvent(CommandBuffer, Event, StageMask);
    }

    void VKAPI_CALL vkCmdResetEvent(VkCommandBuffer CommandBuffer, VkEvent Event, VkPipelineStageFlags StageMask)
    {
        s_ddt.CmdResetEvent(CommandBuffer, Event, StageMask);
    }

    void VKAPI_CALL vkCmdWaitEvents(VkCommandBuffer CommandBuffer, uint32_t EventCount, const VkEvent* Events,
        VkPipelineStageFlags SrcStageMask, VkPipelineStageFlags DstStageMask,
        uint32_t MemoryBarrierCount, const VkMemoryBarrier* pMemoryBarriers,
        uint32_t BufferMemoryBarrierCount, const VkBufferMemoryBarrier* pBufferMemoryBarriers,
        uint32_t ImageMemoryBarrierCount, const VkImageMemoryBarrier* pImageMemoryBarriers)
    {
        s_ddt.CmdWaitEvents(CommandBuffer, EventCount, Events, SrcStageMask, DstStageMask, MemoryBarrierCount, pMemoryBarriers,
            BufferMemoryBarrierCount, pBufferMemoryBarriers, ImageMemoryBarrierCount, pImageMemoryBarriers);
    }

    void VKAPI_CALL vkCmdPipelineBarrier(
        VkCommandBuffer CommandBuffer, VkPipelineStageFlags SrcStageMask, VkPipelineStageFlags DstStageMask, VkDependencyFlags DependencyFlags,
        uint32_t MemoryBarrierCount, const VkMemoryBarrier* MemoryBarriers,
        uint32_t BufferMemoryBarrierCount, const VkBufferMemoryBarrier* BufferMemoryBarriers,
        uint32_t ImageMemoryBarrierCount, const VkImageMemoryBarrier* ImageMemoryBarriers)
    {
        s_ddt.CmdPipelineBarrier(CommandBuffer, SrcStageMask, DstStageMask, DependencyFlags, MemoryBarrierCount, MemoryBarriers, BufferMemoryBarrierCount, BufferMemoryBarriers, ImageMemoryBarrierCount, ImageMemoryBarriers);
    }

    void VKAPI_CALL vkCmdBeginQuery(VkCommandBuffer CommandBuffer, VkQueryPool QueryPool, uint32_t Query, VkQueryControlFlags Flags)
    {
        s_ddt.CmdBeginQuery(CommandBuffer, QueryPool, Query, Flags);
    }

    void VKAPI_CALL vkCmdEndQuery(VkCommandBuffer CommandBuffer, VkQueryPool QueryPool, uint32_t Query)
    {
        s_ddt.CmdEndQuery(CommandBuffer, QueryPool, Query);
    }

    void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer CommandBuffer, VkQueryPool QueryPool, uint32_t FirstQuery, uint32_t QueryCount)
    {
        s_ddt.CmdResetQueryPool(CommandBuffer, QueryPool, FirstQuery, QueryCount);
    }

    void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer CommandBuffer, VkPipelineStageFlagBits PipelineStage, VkQueryPool QueryPool, uint32_t Query)
    {
        s_ddt.CmdWriteTimestamp(CommandBuffer, PipelineStage, QueryPool, Query);
    }

    void VKAPI_CALL vkCmdCopyQueryPoolResults(VkCommandBuffer CommandBuffer, VkQueryPool QueryPool, uint32_t FirstQuery, uint32_t QueryCount,
        VkBuffer DstBuffer, VkDeviceSize DstOffset, VkDeviceSize Stride, VkQueryResultFlags Flags)
    {
        s_ddt.CmdCopyQueryPoolResults(CommandBuffer, QueryPool, FirstQuery, QueryCount, DstBuffer, DstOffset, Stride, Flags);
    }

    void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer CommandBuffer, VkPipelineLayout Layout, VkShaderStageFlags StageFlags, uint32_t Offset, uint32_t Size, const void* pValues)
    {
        s_ddt.CmdPushConstants(CommandBuffer, Layout, StageFlags, Offset, Size, pValues);
    }

    void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer CommandBuffer, const VkRenderPassBeginInfo* RenderPassBegin, VkSubpassContents Contents)
    {
        s_ddt.CmdBeginRenderPass(CommandBuffer, RenderPassBegin, Contents);
    }

    void VKAPI_CALL vkCmdNextSubpass(VkCommandBuffer CommandBuffer, VkSubpassContents Contents)
    {
        s_ddt.CmdNextSubpass(CommandBuffer, Contents);
    }

    void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer CommandBuffer)
    {
        s_ddt.CmdEndRenderPass(CommandBuffer);
    }

    void VKAPI_CALL vkCmdExecuteCommands(VkCommandBuffer CommandBuffer, uint32_t CommandBufferCount, const VkCommandBuffer* pCommandBuffers)
    {
        s_ddt.CmdExecuteCommands(CommandBuffer, CommandBufferCount, pCommandBuffers);
    }

    VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice Device, const VkSwapchainCreateInfoKHR* CreateInfo, const VkAllocationCallbacks* Allocator, VkSwapchainKHR* Swapchain)
    {
        const auto& hooks = sl::plugin_manager::getInterface()->getBeforeHooks(sl::FunctionHookID::eVulkan_CreateSwapchainKHR);
        bool skip = false;
        for (auto [hook, feature] : hooks) ((sl::PFunVkCreateSwapchainKHRBefore*)hook)(Device, CreateInfo, Allocator, Swapchain, skip);

        VkResult result = VK_SUCCESS;
        if (!skip)
        {
            result = s_ddt.CreateSwapchainKHR(Device, CreateInfo, Allocator, Swapchain);
        }
        return result;
    }

    void VKAPI_CALL vkDestroySwapchainKHR(VkDevice Device, VkSwapchainKHR Swapchain, const VkAllocationCallbacks* Allocator)
    {
        const auto& hooks = sl::plugin_manager::getInterface()->getBeforeHooks(sl::FunctionHookID::eVulkan_DestroySwapchainKHR);
        bool skip = false;
        for (auto [hook, feature] : hooks) ((sl::PFunVkDestroySwapchainKHRBefore*)hook)(Device, Swapchain, Allocator, skip);
        if (!skip)
        {
            s_ddt.DestroySwapchainKHR(Device, Swapchain, Allocator);
        }
    }

    VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice Device, VkSwapchainKHR Swapchain, uint32_t* SwapchainImageCount, VkImage* SwapchainImages)
    {
        const auto& hooks = sl::plugin_manager::getInterface()->getBeforeHooks(sl::FunctionHookID::eVulkan_GetSwapchainImagesKHR);
        bool skip = false;
        for (auto [hook, feature] : hooks) ((sl::PFunVkGetSwapchainImagesKHRBefore*)hook)(Device, Swapchain, SwapchainImageCount, SwapchainImages, skip);

        VkResult result = VK_SUCCESS;
        if (!skip)
        {
            result = s_ddt.GetSwapchainImagesKHR(Device, Swapchain, SwapchainImageCount, SwapchainImages);
        }
        return result;
    }

    VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice Device, VkSwapchainKHR Swapchain, uint64_t Timeout, VkSemaphore Semaphore, VkFence Fence, uint32_t* ImageIndex)
    {
        const auto& hooks = sl::plugin_manager::getInterface()->getBeforeHooks(sl::FunctionHookID::eVulkan_AcquireNextImageKHR);
        bool skip = false;
        VkResult result = VK_SUCCESS;
        for (auto [hook, feature] : hooks)
        {
            result = ((sl::PFunVkAcquireNextImageKHRBefore*)hook)(Device, Swapchain, Timeout, Semaphore, Fence, ImageIndex, skip);
            // report error on first fail
            if (result != VK_SUCCESS)
            {
                return result;
            }
        }

        if (!skip)
        {
            result = s_ddt.AcquireNextImageKHR(Device, Swapchain, Timeout, Semaphore, Fence, ImageIndex);
        }
        return result;
    }

    VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue Queue, const VkPresentInfoKHR* PresentInfo)
    {
        const auto& hooks = sl::plugin_manager::getInterface()->getBeforeHooks(sl::FunctionHookID::eVulkan_Present);
        bool skip = false;
        VkResult result = VK_SUCCESS;
        for (auto [hook, feature] : hooks)
        {
            result = ((sl::PFunVkQueuePresentKHRBefore*)hook)(Queue, PresentInfo, skip);
            // report error on first fail
            if (result != VK_SUCCESS)
            {
                return result;
            }
        }

        if (!skip)
        {
            result = s_ddt.QueuePresentKHR(Queue, PresentInfo);
        }
        return result;
    }

    VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice PhysicalDevice, VkSurfaceKHR Surface, VkSurfaceCapabilitiesKHR* SurfaceCapabilities)
    {
        return s_idt.GetPhysicalDeviceSurfaceCapabilitiesKHR(PhysicalDevice, Surface, SurfaceCapabilities);
    }

    VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice PhysicalDevice, VkSurfaceKHR Surface, uint32_t* SurfaceFormatCountPtr, VkSurfaceFormatKHR* SurfaceFormats)
    {
        return s_idt.GetPhysicalDeviceSurfaceFormatsKHR(PhysicalDevice, Surface, SurfaceFormatCountPtr, SurfaceFormats);
    }

    VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice PhysicalDevice, uint32_t QueueFamilyIndex, VkSurfaceKHR Surface, VkBool32* SupportedPtr)
    {
        return s_idt.GetPhysicalDeviceSurfaceSupportKHR(PhysicalDevice, QueueFamilyIndex, Surface, SupportedPtr);
    }

    VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice PhysicalDevice, VkSurfaceKHR Surface, uint32_t* PresentModeCountPtr, VkPresentModeKHR* PresentModesPtr)
    {
        return s_idt.GetPhysicalDeviceSurfacePresentModesKHR(PhysicalDevice, Surface, PresentModeCountPtr, PresentModesPtr);
    }

    VkResult VKAPI_CALL vkCreateWin32SurfaceKHR(VkInstance Instance, const VkWin32SurfaceCreateInfoKHR* CreateInfo, const VkAllocationCallbacks* Allocator, VkSurfaceKHR* Surface)
    {
        return s_idt.CreateWin32SurfaceKHR(Instance, CreateInfo, Allocator, Surface);
    }

    void VKAPI_CALL vkDestroySurfaceKHR(VkInstance Instance, VkSurfaceKHR Surface, const VkAllocationCallbacks* pAllocator)
    {
        s_idt.DestroySurfaceKHR(Instance, Surface, pAllocator);
    }

    void VKAPI_CALL vkGetImageMemoryRequirements2KHR(VkDevice Device, const VkImageMemoryRequirementsInfo2KHR* Info, VkMemoryRequirements2KHR* MemoryRequirements)
    {
        s_ddt.GetImageMemoryRequirements2KHR(Device, Info, MemoryRequirements);
    }

    VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddress(VkDevice Device, const VkBufferDeviceAddressInfo* Info)
    {
        return s_ddt.GetBufferDeviceAddress(Device, Info);
    }

#define SL_INTERCEPT(F)          \
if (strcmp(pName, #F) == 0)          \
{                      \
  return (PFN_vkVoidFunction)F;      \
}

    PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName)
    {
        if (!loadVulkanLibrary())
        {
            SL_LOG_ERROR( "Failed to load Vulkan library");
            return nullptr;
        }

        if (!s_ddt.GetDeviceProcAddr)
        {
            s_ddt.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)GetProcAddress(s_module, "vkGetDeviceProcAddr");
        }

        // Redirect only the hooks we need
        SL_INTERCEPT(vkGetInstanceProcAddr);
        SL_INTERCEPT(vkGetDeviceProcAddr);
        SL_INTERCEPT(vkQueuePresentKHR);
        SL_INTERCEPT(vkCreateImage);
        SL_INTERCEPT(vkCmdPipelineBarrier);
        SL_INTERCEPT(vkCmdBindPipeline);
        SL_INTERCEPT(vkCmdBindDescriptorSets);
        SL_INTERCEPT(vkCreateSwapchainKHR);
        SL_INTERCEPT(vkGetSwapchainImagesKHR);
        SL_INTERCEPT(vkDestroySwapchainKHR);
        SL_INTERCEPT(vkAcquireNextImageKHR);
        SL_INTERCEPT(vkBeginCommandBuffer);

        return s_ddt.GetDeviceProcAddr(device, pName);
    }

    PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName)
    {
        if (!loadVulkanLibrary())
        {
            SL_LOG_ERROR( "Failed to load Vulkan library");
            return nullptr;
        }

        // this can be called before vkCreateInstance, so we may not have the pointer table set up yet
        if (!s_idt.GetInstanceProcAddr)
        {
            s_idt.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(s_module, "vkGetInstanceProcAddr");
        }
    	
        // Redirect only the hooks we need
        SL_INTERCEPT(vkGetInstanceProcAddr);
        SL_INTERCEPT(vkGetDeviceProcAddr);
        SL_INTERCEPT(vkCreateInstance);
        SL_INTERCEPT(vkDestroyInstance);
        SL_INTERCEPT(vkCreateDevice);
        SL_INTERCEPT(vkDestroyDevice);
        SL_INTERCEPT(vkEnumeratePhysicalDevices);

        SL_INTERCEPT(vkQueuePresentKHR);
        SL_INTERCEPT(vkCreateImage);
        SL_INTERCEPT(vkCmdPipelineBarrier);
        SL_INTERCEPT(vkCmdBindPipeline);
        SL_INTERCEPT(vkCmdBindDescriptorSets);
        SL_INTERCEPT(vkCreateSwapchainKHR);
        SL_INTERCEPT(vkDestroySwapchainKHR);
        SL_INTERCEPT(vkGetSwapchainImagesKHR);
        SL_INTERCEPT(vkAcquireNextImageKHR);
        SL_INTERCEPT(vkBeginCommandBuffer);

        return s_idt.GetInstanceProcAddr(instance, pName);
    }

} // extern "C"