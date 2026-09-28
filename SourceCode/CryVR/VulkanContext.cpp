#include "VulkanContext.h"

#if defined(_WIN32)
#include <Windows.h>
#elif defined(__ANDROID__)
#include <SDL3/SDL_vulkan.h>
#else
#include <dlfcn.h>
#endif

#include <cstdio>
#include <cstring>
#include <vector>

namespace CryVR
{
namespace
{
template<class T>
T LoadGlobal(PFN_vkGetInstanceProcAddr getProc, const char* name)
{
    return reinterpret_cast<T>(getProc(VK_NULL_HANDLE, name));
}

template<class T>
T LoadInstance(PFN_vkGetInstanceProcAddr getProc, VkInstance instance, const char* name)
{
    return reinterpret_cast<T>(getProc(instance, name));
}

template<class T>
T LoadDevice(PFN_vkGetDeviceProcAddr getProc, VkDevice device, const char* name)
{
    return reinterpret_cast<T>(getProc(device, name));
}
}

VulkanContext::~VulkanContext()
{
    Shutdown();
}

void VulkanContext::SetError(const char* message)
{
    std::snprintf(m_lastError, sizeof(m_lastError), "%s", message ? message : "unknown error");
}

bool VulkanContext::LoadVulkanLibrary()
{
#if defined(_WIN32)
    m_vulkanLibrary = LoadLibraryA("vulkan-1.dll");
    if (!m_vulkanLibrary)
    {
        SetError("Could not load vulkan-1.dll");
        return false;
    }
    m_getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(static_cast<HMODULE>(m_vulkanLibrary), "vkGetInstanceProcAddr"));
    if (!m_getInstanceProcAddr)
    {
        FreeLibrary(static_cast<HMODULE>(m_vulkanLibrary));
        m_vulkanLibrary = nullptr;
        SetError("vulkan-1.dll does not export vkGetInstanceProcAddr");
        return false;
    }
    m_vulkanLibraryLoaded = true;
#elif defined(__ANDROID__)
    if (!SDL_Vulkan_LoadLibrary(nullptr))
    {
        SetError(SDL_GetError());
        return false;
    }
    m_vulkanLibraryLoaded = true;
    m_getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!m_getInstanceProcAddr)
    {
        SetError("SDL_Vulkan_GetVkGetInstanceProcAddr returned null");
        return false;
    }
#else
    const char* libraryName = "libvulkan.so.1";
    m_vulkanLibrary = dlopen(libraryName, RTLD_NOW | RTLD_LOCAL);
    if (!m_vulkanLibrary)
    {
        m_vulkanLibrary = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!m_vulkanLibrary)
        {
            SetError(dlerror());
            return false;
        }
    }
    m_getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(m_vulkanLibrary, "vkGetInstanceProcAddr"));
    if (!m_getInstanceProcAddr)
    {
        dlclose(m_vulkanLibrary);
        m_vulkanLibrary = nullptr;
        SetError("Vulkan loader does not export vkGetInstanceProcAddr");
        return false;
    }
    m_vulkanLibraryLoaded = true;
#endif
    return true;
}

bool VulkanContext::LoadGlobalFunctions()
{
    m_enumerateInstanceVersion = LoadGlobal<PFN_vkEnumerateInstanceVersion>(m_getInstanceProcAddr, "vkEnumerateInstanceVersion");
    m_createInstance = LoadGlobal<PFN_vkCreateInstance>(m_getInstanceProcAddr, "vkCreateInstance");
    if (!m_createInstance)
    {
        SetError("vkCreateInstance is unavailable");
        return false;
    }
    return true;
}

bool VulkanContext::LoadInstanceFunctions()
{
    m_destroyInstance = LoadInstance<PFN_vkDestroyInstance>(m_getInstanceProcAddr, m_instance, "vkDestroyInstance");
    m_enumeratePhysicalDevices = LoadInstance<PFN_vkEnumeratePhysicalDevices>(m_getInstanceProcAddr, m_instance, "vkEnumeratePhysicalDevices");
    m_getPhysicalDeviceQueueFamilyProperties = LoadInstance<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        m_getInstanceProcAddr, m_instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    m_getPhysicalDeviceFeatures = LoadInstance<PFN_vkGetPhysicalDeviceFeatures>(
        m_getInstanceProcAddr, m_instance, "vkGetPhysicalDeviceFeatures");
    m_getPhysicalDeviceProperties = LoadInstance<PFN_vkGetPhysicalDeviceProperties>(
        m_getInstanceProcAddr, m_instance, "vkGetPhysicalDeviceProperties");
    m_createDevice = LoadInstance<PFN_vkCreateDevice>(m_getInstanceProcAddr, m_instance, "vkCreateDevice");
    m_getDeviceProcAddr = LoadInstance<PFN_vkGetDeviceProcAddr>(m_getInstanceProcAddr, m_instance, "vkGetDeviceProcAddr");
    return m_destroyInstance && m_enumeratePhysicalDevices &&
           m_getPhysicalDeviceQueueFamilyProperties && m_getPhysicalDeviceFeatures &&
           m_getPhysicalDeviceProperties &&
           m_createDevice && m_getDeviceProcAddr;
}

bool VulkanContext::LoadDeviceFunctions()
{
    if (!m_getDeviceProcAddr)
        return false;
    m_destroyDevice = LoadDevice<PFN_vkDestroyDevice>(m_getDeviceProcAddr, m_device, "vkDestroyDevice");
    m_getDeviceQueue = LoadDevice<PFN_vkGetDeviceQueue>(m_getDeviceProcAddr, m_device, "vkGetDeviceQueue");
    m_deviceWaitIdle = LoadDevice<PFN_vkDeviceWaitIdle>(m_getDeviceProcAddr, m_device, "vkDeviceWaitIdle");
    return m_destroyDevice && m_getDeviceQueue && m_deviceWaitIdle;
}

bool VulkanContext::Initialize(const Runtime& runtime)
{
    Shutdown();
    if (!LoadVulkanLibrary() || !LoadGlobalFunctions())
        return false;

    VkApplicationInfo applicationInfo{};
    applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    applicationInfo.pApplicationName = "Far Cry";
    applicationInfo.applicationVersion = 1;
    applicationInfo.pEngineName = "CryEngine 1 VR";
    applicationInfo.engineVersion = 1;
    XrVersion minApiVersion = 0;
    XrVersion maxApiVersion = 0;
    if (!runtime.GetVulkanRequirements(&minApiVersion, &maxApiVersion))
    {
        SetError("OpenXR Vulkan requirements are unavailable");
        Shutdown();
        return false;
    }

    uint32_t availableApiVersion = VK_API_VERSION_1_0;
    if (m_enumerateInstanceVersion)
        m_enumerateInstanceVersion(&availableApiVersion);

    const uint32_t minMajor = static_cast<uint32_t>(minApiVersion >> 48);
    const uint32_t minMinor = static_cast<uint32_t>((minApiVersion >> 32) & 0xffffu);
    const uint32_t maxMajor = static_cast<uint32_t>(maxApiVersion >> 48);
    const uint32_t maxMinor = static_cast<uint32_t>((maxApiVersion >> 32) & 0xffffu);
    const uint32_t minVkVersion = VK_MAKE_VERSION(minMajor, minMinor, 0);
    const uint32_t maxVkVersion = VK_MAKE_VERSION(maxMajor, maxMinor, 0);
    if (availableApiVersion < minVkVersion)
    {
        SetError("installed Vulkan API is below the OpenXR minimum");
        Shutdown();
        return false;
    }
    applicationInfo.apiVersion = availableApiVersion < maxVkVersion ? availableApiVersion : maxVkVersion;

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &applicationInfo;

    int32_t vulkanResult = VK_ERROR_UNKNOWN;
    if (!runtime.CreateVulkanInstance(reinterpret_cast<void*>(m_getInstanceProcAddr),
                                       &instanceInfo, reinterpret_cast<void**>(&m_instance), &vulkanResult))
    {
        SetError("OpenXR failed to create the Vulkan instance");
        Shutdown();
        return false;
    }

    if (!LoadInstanceFunctions())
    {
        SetError("required Vulkan instance functions are unavailable");
        Shutdown();
        return false;
    }

    if (!runtime.GetVulkanGraphicsDevice(reinterpret_cast<void*>(m_instance),
                                         reinterpret_cast<void**>(&m_physicalDevice)))
    {
        SetError("OpenXR could not select a Vulkan physical device");
        Shutdown();
        return false;
    }

    uint32_t queueFamilyCount = 0;
    m_getPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, nullptr);
    if (queueFamilyCount == 0)
    {
        SetError("Vulkan device has no queue families");
        Shutdown();
        return false;
    }

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    m_getPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, queueFamilies.data());
    for (uint32_t i = 0; i < queueFamilyCount; ++i)
    {
        if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && queueFamilies[i].queueCount > 0)
        {
            m_graphicsQueueFamily = i;
            break;
        }
    }
    if ((queueFamilies[m_graphicsQueueFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
    {
        SetError("Vulkan device has no graphics queue");
        Shutdown();
        return false;
    }

    const float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = m_graphicsQueueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    VkPhysicalDeviceFeatures availableFeatures{};
    m_getPhysicalDeviceFeatures(m_physicalDevice, &availableFeatures);
    VkPhysicalDeviceProperties physicalProperties{};
    m_getPhysicalDeviceProperties(m_physicalDevice, &physicalProperties);
    VkPhysicalDeviceFeatures enabledFeatures{};
    enabledFeatures.fillModeNonSolid = availableFeatures.fillModeNonSolid;
    enabledFeatures.samplerAnisotropy = availableFeatures.samplerAnisotropy;
    m_supportsWireframe = enabledFeatures.fillModeNonSolid == VK_TRUE;
    m_supportsAnisotropicFiltering = enabledFeatures.samplerAnisotropy == VK_TRUE;
    // GL_EYE_RADIAL_NV is the extension path used by XRenderOGL. Its Vulkan
    // counterpart is selected only on NVIDIA devices; other vendors retain
    // OpenGL's default eye-space-Z fog distance.
    m_supportsRadialFog = physicalProperties.vendorID == 0x10de;
    m_maxSamplerAnisotropy = m_supportsAnisotropicFiltering ?
        physicalProperties.limits.maxSamplerAnisotropy : 1.0f;
    m_minUniformBufferOffsetAlignment = physicalProperties.limits.minUniformBufferOffsetAlignment;
    if (m_minUniformBufferOffsetAlignment == 0)
        m_minUniformBufferOffsetAlignment = 16;

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.pEnabledFeatures = &enabledFeatures;

    vulkanResult = VK_ERROR_UNKNOWN;
    if (!runtime.CreateVulkanDevice(reinterpret_cast<void*>(m_getInstanceProcAddr),
                                    reinterpret_cast<void*>(m_physicalDevice),
                                    &deviceInfo, reinterpret_cast<void**>(&m_device), &vulkanResult))
    {
        SetError("OpenXR failed to create the Vulkan device");
        Shutdown();
        return false;
    }

    if (!LoadDeviceFunctions())
    {
        SetError("required Vulkan device functions are unavailable");
        Shutdown();
        return false;
    }
    m_getDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
    if (!m_graphicsQueue)
    {
        SetError("vkGetDeviceQueue returned null");
        Shutdown();
        return false;
    }
    return true;
}

void VulkanContext::Shutdown()
{
    if (m_device && m_deviceWaitIdle)
        m_deviceWaitIdle(m_device);
    if (m_device && m_destroyDevice)
        m_destroyDevice(m_device, nullptr);
    if (m_instance && m_destroyInstance)
        m_destroyInstance(m_instance, nullptr);
    if (m_vulkanLibraryLoaded)
#if defined(_WIN32)
    {
        FreeLibrary(static_cast<HMODULE>(m_vulkanLibrary));
        m_vulkanLibrary = nullptr;
    }
#elif defined(__ANDROID__)
    {
        SDL_Vulkan_UnloadLibrary();
    }
#else
    {
        dlclose(m_vulkanLibrary);
        m_vulkanLibrary = nullptr;
    }
#endif

    m_device = VK_NULL_HANDLE;
    m_instance = VK_NULL_HANDLE;
    m_physicalDevice = VK_NULL_HANDLE;
    m_graphicsQueue = VK_NULL_HANDLE;
    m_graphicsQueueFamily = 0;
    m_supportsWireframe = false;
    m_supportsAnisotropicFiltering = false;
    m_supportsRadialFog = false;
    m_maxSamplerAnisotropy = 1.0f;
    m_vulkanLibraryLoaded = false;
    m_getInstanceProcAddr = nullptr;
    m_getDeviceProcAddr = nullptr;
    m_createInstance = nullptr;
    m_destroyInstance = nullptr;
    m_enumeratePhysicalDevices = nullptr;
    m_getPhysicalDeviceQueueFamilyProperties = nullptr;
    m_getPhysicalDeviceProperties = nullptr;
    m_createDevice = nullptr;
    m_destroyDevice = nullptr;
    m_getDeviceQueue = nullptr;
    m_deviceWaitIdle = nullptr;
}

VulkanBinding VulkanContext::GetOpenXRBinding() const
{
    VulkanBinding binding;
    binding.instance = reinterpret_cast<void*>(m_instance);
    binding.physicalDevice = reinterpret_cast<void*>(m_physicalDevice);
    binding.device = reinterpret_cast<void*>(m_device);
    binding.queue = reinterpret_cast<void*>(m_graphicsQueue);
    binding.queueFamilyIndex = m_graphicsQueueFamily;
    binding.queueIndex = 0;
    return binding;
}
}
