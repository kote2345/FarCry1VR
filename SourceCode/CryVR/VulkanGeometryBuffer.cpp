#include "VulkanGeometryBuffer.h"

namespace CryVR
{
VulkanGeometryBuffer::~VulkanGeometryBuffer()
{
    Destroy();
}

bool VulkanGeometryBuffer::CreateStatic(VulkanResourceManager& resources, uint32_t cryFormat,
                                        const void* vertices, uint32_t vertexCount,
                                        const uint16_t* indices, uint32_t indexCount)
{
    Destroy();
    if (!vertices || vertexCount == 0 || !indices || indexCount == 0 ||
        !GetVulkanVertexFormat(cryFormat, m_format))
        return false;

    const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(m_format.stride) * vertexCount;
    const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(sizeof(uint16_t)) * indexCount;
    if (!resources.CreateBufferWithData(vertices, vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, m_vertices))
        return false;
    if (!resources.CreateBufferWithData(indices, indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, m_indices))
    {
        resources.DestroyBuffer(m_vertices);
        return false;
    }

    m_resources = &resources;
    m_vertexCapacity = vertexCount;
    m_indexCapacity = indexCount;
    return true;
}

bool VulkanGeometryBuffer::CreateDynamic(VulkanResourceManager& resources, uint32_t cryFormat,
                                         uint32_t vertexCapacity, uint32_t indexCapacity)
{
    Destroy();
    if (vertexCapacity == 0 || indexCapacity == 0 ||
        !GetVulkanVertexFormat(cryFormat, m_format))
        return false;

    const VkMemoryPropertyFlags hostMemory =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(m_format.stride) * vertexCapacity;
    const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(sizeof(uint16_t)) * indexCapacity;
    if (!resources.CreateBuffer(vertexBytes,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            hostMemory, m_vertices))
        return false;
    if (!resources.CreateBuffer(indexBytes,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            hostMemory, m_indices))
    {
        resources.DestroyBuffer(m_vertices);
        return false;
    }

    m_resources = &resources;
    m_vertexCapacity = vertexCapacity;
    m_indexCapacity = indexCapacity;
    m_dynamic = true;
    return true;
}

bool VulkanGeometryBuffer::UpdateVertices(const void* data, VkDeviceSize size, VkDeviceSize offset)
{
    return m_resources && m_dynamic && size != 0 &&
        m_resources->UploadBuffer(m_vertices, data, size, offset);
}

bool VulkanGeometryBuffer::UpdateIndices(const uint16_t* data, uint32_t count, uint32_t firstIndex)
{
    if (!m_resources || !m_dynamic || !data || count == 0 || firstIndex > m_indexCapacity ||
        count > m_indexCapacity - firstIndex)
        return false;
    const VkDeviceSize offset = static_cast<VkDeviceSize>(firstIndex) * sizeof(uint16_t);
    const VkDeviceSize size = static_cast<VkDeviceSize>(count) * sizeof(uint16_t);
    return m_resources->UploadBuffer(m_indices, data, size, offset);
}

bool VulkanGeometryBuffer::RecordDrawIndexed(VkCommandBuffer commandBuffer, uint32_t count,
                                              uint32_t firstIndex, int32_t vertexOffset,
                                              PFN_vkCmdBindVertexBuffers bindVertexBuffers,
                                              PFN_vkCmdBindIndexBuffer bindIndexBuffer,
                                              PFN_vkCmdDrawIndexed drawIndexed) const
{
    if (!commandBuffer || !m_vertices.buffer || !m_indices.buffer || count == 0 ||
        firstIndex > m_indexCapacity || count > m_indexCapacity - firstIndex ||
        !bindVertexBuffers || !bindIndexBuffer || !drawIndexed)
        return false;

    const VkDeviceSize vertexOffsetBytes = 0;
    bindVertexBuffers(commandBuffer, 0, 1, &m_vertices.buffer, &vertexOffsetBytes);
    bindIndexBuffer(commandBuffer, m_indices.buffer, 0, VK_INDEX_TYPE_UINT16);
    drawIndexed(commandBuffer, count, 1, firstIndex, vertexOffset, 0);
    return true;
}

void VulkanGeometryBuffer::Destroy()
{
    if (m_resources)
    {
        m_resources->DestroyBuffer(m_vertices);
        m_resources->DestroyBuffer(m_indices);
    }
    m_resources = nullptr;
    m_format = VulkanVertexFormat{};
    m_vertexCapacity = 0;
    m_indexCapacity = 0;
    m_dynamic = false;
}
}
