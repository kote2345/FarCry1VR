#ifndef CRY_VR_VULKAN_VERTEX_FORMAT_H
#define CRY_VR_VULKAN_VERTEX_FORMAT_H

#include <vulkan/vulkan.h>
#include <stdint.h>

namespace CryVR
{
// Vertex-format IDs and byte layouts are the legacy CryEngine eVertexFormat
// contract in CryCommon/VertexFormats.h. Locations are a stable Vulkan-side
// semantic convention: position, normal, primary color, UV0, secondary color,
// UV1, tangent, binormal.
struct VulkanVertexFormat
{
    uint32_t stride;
    uint32_t attributeCount;
    VkVertexInputAttributeDescription attributes[12];
};

bool GetVulkanVertexFormat(uint32_t cryVertexFormat, VulkanVertexFormat& result);
}

#endif
