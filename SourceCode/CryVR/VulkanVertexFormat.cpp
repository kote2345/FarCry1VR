#include "VulkanVertexFormat.h"

#include <stddef.h>
#include <string.h>

namespace CryVR
{
namespace
{
enum AttributeLocation
{
    Position = 0, Normal = 1, Color = 2, UV0 = 3,
    SecondaryColor = 4, UV1 = 5, Tangent = 6, Binormal = 7
};

void Add(VulkanVertexFormat& f, uint32_t location, VkFormat format, uint32_t offset)
{
    VkVertexInputAttributeDescription& a = f.attributes[f.attributeCount++];
    a.location = location;
    a.binding = 0;
    a.format = format;
    a.offset = offset;
}

void Position3(VulkanVertexFormat& f) { Add(f, Position, VK_FORMAT_R32G32B32_SFLOAT, 0); }
void Normal3(VulkanVertexFormat& f, uint32_t offset) { Add(f, Normal, VK_FORMAT_R32G32B32_SFLOAT, offset); }
void Color4(VulkanVertexFormat& f, uint32_t location, uint32_t offset)
{ Add(f, location, VK_FORMAT_R8G8B8A8_UNORM, offset); }
void UV(VulkanVertexFormat& f, uint32_t location, uint32_t offset)
{ Add(f, location, VK_FORMAT_R32G32_SFLOAT, offset); }
}

bool GetVulkanVertexFormat(uint32_t format, VulkanVertexFormat& out)
{
    memset(&out, 0, sizeof(out));
    switch (format)
    {
    case 1: out.stride = 12; Position3(out); break;                                      // P3F
    case 2: out.stride = 16; Position3(out); Color4(out, Color, 12); break;              // P3F_COL4UB
    case 3: out.stride = 20; Position3(out); UV(out, UV0, 12); break;                    // P3F_TEX2F
    case 4: out.stride = 24; Position3(out); Color4(out, Color, 12); UV(out, UV0, 16); break;
    // Legacy GL_VertBuffer.cpp deliberately reads only xyz from this 4-float
    // font record (rhw remains in the stride but is not a vertex attribute).
    case 5: out.stride = 28; Position3(out);
            Color4(out, Color, 16); UV(out, UV0, 20); break;                              // TRP3F...
    case 6: out.stride = 20; Position3(out); Color4(out, Color, 12); Color4(out, SecondaryColor, 16); break;
    case 7: out.stride = 24; Position3(out); Normal3(out, 12); break;
    case 8: out.stride = 28; Position3(out); Normal3(out, 12); Color4(out, Color, 24); break;
    case 9: out.stride = 32; Position3(out); Normal3(out, 12); UV(out, UV0, 24); break;
    case 10: out.stride = 36; Position3(out); Normal3(out, 12); Color4(out, Color, 24); UV(out, UV0, 28); break;
    case 11: out.stride = 32; Position3(out); Normal3(out, 12); Color4(out, Color, 24); Color4(out, SecondaryColor, 28); break;
    case 12: out.stride = 28; Position3(out); Color4(out, Color, 12); Color4(out, SecondaryColor, 16); UV(out, UV0, 20); break;
    case 13: out.stride = 40; Position3(out); Normal3(out, 12); Color4(out, Color, 24); Color4(out, SecondaryColor, 28); UV(out, UV0, 32); break;
    case 14: out.stride = 36; Add(out, Tangent, VK_FORMAT_R32G32B32_SFLOAT, 0);
             Add(out, Binormal, VK_FORMAT_R32G32B32_SFLOAT, 12); Normal3(out, 24); break;
    case 15: out.stride = 8; UV(out, UV0, 0); break;                                     // TEX2F (no position)
    case 16: out.stride = 32; Position3(out); Color4(out, Color, 12); UV(out, UV0, 16); UV(out, UV1, 24); break;
    default: return false;
    }
    return true;
}
}
