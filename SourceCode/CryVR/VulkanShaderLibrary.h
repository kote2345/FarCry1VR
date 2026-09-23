#ifndef CRY_VR_VULKAN_SHADER_LIBRARY_H
#define CRY_VR_VULKAN_SHADER_LIBRARY_H

#include "VulkanContext.h"

#include <vulkan/vulkan.h>
#include <cstddef>
#include <string>
#include <vector>

namespace CryVR
{
struct VulkanShader
{
    VkShaderModule module = VK_NULL_HANDLE;
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_VERTEX_BIT;
    std::string entryPoint = "main";
};

// SPIR-V is intentionally loaded from memory so the final renderer can use
// CryPak on both Android and PC without platform-specific shader IO here.
class VulkanShaderLibrary
{
public:
    VulkanShaderLibrary() = default;
    ~VulkanShaderLibrary();

    bool Initialize(VulkanContext& context);
    void Shutdown();
    bool LoadSpirV(const void* data, size_t size, VkShaderStageFlagBits stage,
                  VulkanShader& shader, const char* entryPoint = "main");
    bool LoadSpirVFile(const char* path, VkShaderStageFlagBits stage,
                       VulkanShader& shader, const char* entryPoint = "main");
    void DestroyShader(VulkanShader& shader);

    bool IsInitialized() const { return m_context != nullptr; }
    const char* GetLastError() const { return m_lastError; }

private:
    void SetError(const char* message);

    VulkanContext* m_context = nullptr;
    PFN_vkCreateShaderModule m_createShaderModule = nullptr;
    PFN_vkDestroyShaderModule m_destroyShaderModule = nullptr;
    char m_lastError[256]{};
};
}

#endif
