#ifndef CRY_VR_VULKAN_RESOURCE_MANAGER_H
#define CRY_VR_VULKAN_RESOURCE_MANAGER_H

#include "VulkanContext.h"

#include <vulkan/vulkan.h>
#include <cstddef>
#include <functional>

namespace CryVR
{
struct VulkanBuffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkMemoryPropertyFlags memoryProperties = 0;
};

struct VulkanTexture
{
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VulkanBuffer uploadBuffer;
};

// Common GPU resource primitives. Higher render layers decide lifetime and
// descriptors; this class only handles Vulkan allocation and upload rules.
class VulkanResourceManager
{
public:
    VulkanResourceManager() = default;
    ~VulkanResourceManager();

    bool Initialize(VulkanContext& context);
    void Shutdown();

    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags memoryProperties, VulkanBuffer& buffer);
    bool CreateBufferWithData(const void* data, VkDeviceSize size,
                              VkBufferUsageFlags usage, VulkanBuffer& buffer);
    bool UploadBuffer(VulkanBuffer& buffer, const void* data, VkDeviceSize size,
                      VkDeviceSize offset = 0);
    void DestroyBuffer(VulkanBuffer& buffer);

    bool CreateTextureRGBA8(const void* rgbaData, uint32_t width, uint32_t height,
                            VulkanTexture& texture,
                            VkFormat format = VK_FORMAT_R8G8B8A8_UNORM,
                            bool retainUploadBuffer = false,
                            bool generateMipmaps = true);
    bool CreateDepthImage(uint32_t width, uint32_t height, VkFormat format,
                          VulkanTexture& texture);
    bool UpdateTextureRGBA8(const void* rgbaData, uint32_t width, uint32_t height,
                            VulkanTexture& texture);
    bool PrepareTextureRGBA8Update(const void* rgbaData, uint32_t width, uint32_t height,
                                   VulkanTexture& texture);
    bool RecordTextureRGBA8Update(VkCommandBuffer commandBuffer, const VulkanTexture& texture);
    void DestroyTexture(VulkanTexture& texture);

    bool IsInitialized() const { return m_context != nullptr; }
    const char* GetLastError() const { return m_lastError; }

private:
    bool LoadFunctions();
    bool FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties, uint32_t& typeIndex) const;
    bool AllocateMemory(const VkMemoryRequirements& requirements,
                        VkMemoryPropertyFlags properties, VkDeviceMemory& memory) const;
    bool SubmitImmediate(const std::function<void(VkCommandBuffer)>& record);
    void SetError(const char* message);

    VulkanContext* m_context = nullptr;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties m_memoryProperties{};
    char m_lastError[256]{};

    PFN_vkGetDeviceProcAddr m_getDeviceProcAddr = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties m_getPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkCreateCommandPool m_createCommandPool = nullptr;
    PFN_vkDestroyCommandPool m_destroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers m_allocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers m_freeCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer m_beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer m_endCommandBuffer = nullptr;
    PFN_vkQueueSubmit m_queueSubmit = nullptr;
    PFN_vkQueueWaitIdle m_queueWaitIdle = nullptr;
    PFN_vkCreateBuffer m_createBuffer = nullptr;
    PFN_vkDestroyBuffer m_destroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements m_getBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory m_allocateMemory = nullptr;
    PFN_vkFreeMemory m_freeMemory = nullptr;
    PFN_vkBindBufferMemory m_bindBufferMemory = nullptr;
    PFN_vkMapMemory m_mapMemory = nullptr;
    PFN_vkUnmapMemory m_unmapMemory = nullptr;
    PFN_vkFlushMappedMemoryRanges m_flushMappedMemoryRanges = nullptr;
    PFN_vkCmdCopyBuffer m_cmdCopyBuffer = nullptr;
    PFN_vkCreateImage m_createImage = nullptr;
    PFN_vkDestroyImage m_destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements m_getImageMemoryRequirements = nullptr;
    PFN_vkBindImageMemory m_bindImageMemory = nullptr;
    PFN_vkCreateImageView m_createImageView = nullptr;
    PFN_vkDestroyImageView m_destroyImageView = nullptr;
    PFN_vkCmdPipelineBarrier m_cmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyBufferToImage m_cmdCopyBufferToImage = nullptr;
};
}

#endif
