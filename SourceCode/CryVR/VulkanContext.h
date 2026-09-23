#ifndef CRY_VR_VULKAN_CONTEXT_H
#define CRY_VR_VULKAN_CONTEXT_H

#include "CryVR.h"

#include <vulkan/vulkan.h>

namespace CryVR
{
// Platform-neutral Vulkan device bootstrap shared by Quest and PCVR.
// The renderer owns all graphics resources; this class owns only the loader,
// instance, physical device, logical device and graphics queue.
class VulkanContext
{
public:
    VulkanContext() = default;
    ~VulkanContext();

    bool Initialize(const Runtime& runtime);
    void Shutdown();

    bool IsInitialized() const { return m_instance != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE; }
    VkInstance GetInstance() const { return m_instance; }
    VkPhysicalDevice GetPhysicalDevice() const { return m_physicalDevice; }
    VkDevice GetDevice() const { return m_device; }
    VkQueue GetGraphicsQueue() const { return m_graphicsQueue; }
    uint32_t GetGraphicsQueueFamily() const { return m_graphicsQueueFamily; }
    bool SupportsWireframe() const { return m_supportsWireframe; }
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr() const { return m_getInstanceProcAddr; }

    VulkanBinding GetOpenXRBinding() const;
    const char* GetLastError() const { return m_lastError; }

private:
    bool LoadVulkanLibrary();
    bool LoadInstanceFunctions();
    bool LoadDeviceFunctions();
    void SetError(const char* message);

    bool m_vulkanLibraryLoaded = false;
    PFN_vkGetInstanceProcAddr m_getInstanceProcAddr = nullptr;
    PFN_vkEnumerateInstanceVersion m_enumerateInstanceVersion = nullptr;
    PFN_vkCreateInstance m_createInstance = nullptr;
    PFN_vkDestroyInstance m_destroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices m_enumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties m_getPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures m_getPhysicalDeviceFeatures = nullptr;
    PFN_vkCreateDevice m_createDevice = nullptr;
    PFN_vkDestroyDevice m_destroyDevice = nullptr;
    PFN_vkGetDeviceQueue m_getDeviceQueue = nullptr;
    PFN_vkDeviceWaitIdle m_deviceWaitIdle = nullptr;

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    uint32_t m_graphicsQueueFamily = 0;
    bool m_supportsWireframe = false;
    char m_lastError[256]{};
};
}

#endif
