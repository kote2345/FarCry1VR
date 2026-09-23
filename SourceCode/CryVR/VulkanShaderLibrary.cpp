#include "VulkanShaderLibrary.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace CryVR
{
VulkanShaderLibrary::~VulkanShaderLibrary()
{
    Shutdown();
}

void VulkanShaderLibrary::SetError(const char* message)
{
    std::snprintf(m_lastError, sizeof(m_lastError), "%s", message ? message : "unknown error");
}

bool VulkanShaderLibrary::Initialize(VulkanContext& context)
{
    Shutdown();
    if (!context.IsInitialized())
    {
        SetError("Vulkan context is not initialized");
        return false;
    }
    m_context = &context;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = context.GetInstanceProcAddr();
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        getInstanceProcAddr(context.GetInstance(), "vkGetDeviceProcAddr"));
    if (!getDeviceProcAddr)
    {
        SetError("vkGetDeviceProcAddr is unavailable");
        Shutdown();
        return false;
    }
    m_createShaderModule = reinterpret_cast<PFN_vkCreateShaderModule>(
        getDeviceProcAddr(context.GetDevice(), "vkCreateShaderModule"));
    m_destroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(
        getDeviceProcAddr(context.GetDevice(), "vkDestroyShaderModule"));
    if (!m_createShaderModule || !m_destroyShaderModule)
    {
        SetError("Vulkan shader module functions are unavailable");
        Shutdown();
        return false;
    }
    return true;
}

bool VulkanShaderLibrary::LoadSpirV(const void* data, size_t size, VkShaderStageFlagBits stage,
                                    VulkanShader& shader, const char* entryPoint)
{
    DestroyShader(shader);
    if (!m_context || !data || size < sizeof(uint32_t) || (size % sizeof(uint32_t)) != 0)
    {
        SetError("invalid SPIR-V buffer");
        return false;
    }
    uint32_t magic = 0;
    std::memcpy(&magic, data, sizeof(magic));
    if (magic != 0x07230203u)
    {
        SetError("SPIR-V magic number is invalid");
        return false;
    }
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = size;
    createInfo.pCode = static_cast<const uint32_t*>(data);
    if (m_createShaderModule(m_context->GetDevice(), &createInfo, nullptr, &shader.module) != VK_SUCCESS)
    {
        SetError("vkCreateShaderModule failed");
        return false;
    }
    shader.stage = stage;
    shader.entryPoint = entryPoint && entryPoint[0] ? entryPoint : "main";
    return true;
}

bool VulkanShaderLibrary::LoadSpirVFile(const char* path, VkShaderStageFlagBits stage,
                                        VulkanShader& shader, const char* entryPoint)
{
    if (!path || !path[0])
    {
        SetError("SPIR-V path is empty");
        return false;
    }
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        SetError("SPIR-V file could not be opened");
        return false;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0)
    {
        SetError("SPIR-V file is empty");
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size))
    {
        SetError("SPIR-V file read failed");
        return false;
    }
    return LoadSpirV(bytes.data(), bytes.size(), stage, shader, entryPoint);
}

void VulkanShaderLibrary::DestroyShader(VulkanShader& shader)
{
    if (m_context && shader.module && m_destroyShaderModule)
        m_destroyShaderModule(m_context->GetDevice(), shader.module, nullptr);
    shader = VulkanShader{};
}

void VulkanShaderLibrary::Shutdown()
{
    m_context = nullptr;
    m_createShaderModule = nullptr;
    m_destroyShaderModule = nullptr;
}
}
