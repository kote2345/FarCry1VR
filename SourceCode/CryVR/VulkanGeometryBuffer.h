#ifndef CRY_VR_VULKAN_GEOMETRY_BUFFER_H
#define CRY_VR_VULKAN_GEOMETRY_BUFFER_H

#include "VulkanResourceManager.h"
#include "VulkanVertexFormat.h"

namespace CryVR
{
// GPU storage for one CryEngine vertex stream and its 16-bit index stream.
// Static data is staged to device-local memory; dynamic data stays mapped-
// capable so legacy UpdateBuffer-style partial writes can be represented.
class VulkanGeometryBuffer
{
public:
    VulkanGeometryBuffer() = default;
    ~VulkanGeometryBuffer();

    bool CreateStatic(VulkanResourceManager& resources, uint32_t cryFormat,
                      const void* vertices, uint32_t vertexCount,
                      const uint16_t* indices, uint32_t indexCount);
    bool CreateDynamic(VulkanResourceManager& resources, uint32_t cryFormat,
                       uint32_t vertexCapacity, uint32_t indexCapacity);
    bool UpdateVertices(const void* data, VkDeviceSize size, VkDeviceSize offset = 0);
    bool UpdateIndices(const uint16_t* data, uint32_t indexCount, uint32_t firstIndex = 0);
    bool RecordDrawIndexed(VkCommandBuffer commandBuffer, uint32_t indexCount,
                           uint32_t firstIndex, int32_t vertexOffset,
                           PFN_vkCmdBindVertexBuffers bindVertexBuffers,
                           PFN_vkCmdBindIndexBuffer bindIndexBuffer,
                           PFN_vkCmdDrawIndexed drawIndexed) const;
    void Destroy();

    const VulkanBuffer& GetVertexBuffer() const { return m_vertices; }
    const VulkanBuffer& GetIndexBuffer() const { return m_indices; }
    const VulkanVertexFormat& GetFormat() const { return m_format; }
    uint32_t GetVertexCapacity() const { return m_vertexCapacity; }
    uint32_t GetIndexCapacity() const { return m_indexCapacity; }
    bool IsDynamic() const { return m_dynamic; }

private:
    VulkanResourceManager* m_resources = nullptr;
    VulkanBuffer m_vertices;
    VulkanBuffer m_indices;
    VulkanVertexFormat m_format{};
    uint32_t m_vertexCapacity = 0;
    uint32_t m_indexCapacity = 0;
    bool m_dynamic = false;
};
}

#endif
