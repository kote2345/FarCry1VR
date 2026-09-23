#include "VulkanPipelineState.h"

namespace CryVR
{
namespace
{
// Keep these synchronized with the public legacy macros in CryCommon/IRenderer.h.
const uint32_t BlendMask = 0x000000ffu;
const uint32_t DepthWrite = 0x00000100u;
const uint32_t NoColorMask = 0x00000400u;
const uint32_t PolyLine = 0x00001000u;
const uint32_t AlphaOnly = 0x00010000u;
const uint32_t NoDepthTest = 0x00020000u;
const uint32_t RGBOnly = 0x00040000u;
const uint32_t DepthEqual = 0x00100000u;
const uint32_t DepthGreater = 0x00200000u;
const uint32_t Stencil = 0x00400000u;
const uint32_t AlphaTestMask = 0xf0000000u;

bool DecodeSourceBlendFactor(uint32_t value, VkBlendFactor& factor)
{
    switch (value)
    {
    // GL_VertBuffer's state switch defaults a missing source factor to ONE.
    case 0: factor = VK_BLEND_FACTOR_ONE; return true;
    case 1: factor = VK_BLEND_FACTOR_ZERO; return true;                 // GS_BLSRC_ZERO
    case 2: factor = VK_BLEND_FACTOR_ONE; return true;                  // GS_BLSRC_ONE
    case 3: factor = VK_BLEND_FACTOR_DST_COLOR; return true;
    case 4: factor = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; return true;
    case 5: factor = VK_BLEND_FACTOR_SRC_ALPHA; return true;
    case 6: factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; return true;
    case 7: factor = VK_BLEND_FACTOR_DST_ALPHA; return true;
    case 8: factor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA; return true;
    case 9: factor = VK_BLEND_FACTOR_SRC_ALPHA_SATURATE; return true;
    }
    return false;
}

bool DecodeDestinationBlendFactor(uint32_t value, VkBlendFactor& factor)
{
    switch (value)
    {
    case 0: case 0x10: factor = VK_BLEND_FACTOR_ZERO; return true;
    case 0x20: factor = VK_BLEND_FACTOR_ONE; return true;
    case 0x30: factor = VK_BLEND_FACTOR_SRC_COLOR; return true;
    case 0x40: factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR; return true;
    case 0x50: factor = VK_BLEND_FACTOR_SRC_ALPHA; return true;
    case 0x60: factor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; return true;
    case 0x70: factor = VK_BLEND_FACTOR_DST_ALPHA; return true;
    case 0x80: factor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA; return true;
    }
    return false;
}

bool DecodeStencilCompare(uint32_t value, VkCompareOp& op)
{
    switch (value & 7u)
    {
    case 0: op = VK_COMPARE_OP_ALWAYS; return true;
    case 1: op = VK_COMPARE_OP_NEVER; return true;
    case 2: op = VK_COMPARE_OP_LESS; return true;
    case 3: op = VK_COMPARE_OP_LESS_OR_EQUAL; return true;
    case 4: op = VK_COMPARE_OP_GREATER; return true;
    case 5: op = VK_COMPARE_OP_GREATER_OR_EQUAL; return true;
    case 6: op = VK_COMPARE_OP_EQUAL; return true;
    case 7: op = VK_COMPARE_OP_NOT_EQUAL; return true;
    }
    return false;
}

bool DecodeStencilOperation(uint32_t value, VkStencilOp& op)
{
    switch (value & 7u)
    {
    case 0: op = VK_STENCIL_OP_KEEP; return true;
    case 1: op = VK_STENCIL_OP_REPLACE; return true;
    case 2: op = VK_STENCIL_OP_INCREMENT_AND_CLAMP; return true;
    case 3: op = VK_STENCIL_OP_DECREMENT_AND_CLAMP; return true;
    case 4: op = VK_STENCIL_OP_ZERO; return true;
    case 5: op = VK_STENCIL_OP_INCREMENT_AND_WRAP; return true;
    case 6: op = VK_STENCIL_OP_DECREMENT_AND_WRAP; return true;
    default: return false;
    }
}

bool DecodeStencilFace(uint32_t state, uint32_t shift, uint32_t function,
                       VkStencilOpState& face)
{
    face.failOp = VK_STENCIL_OP_KEEP;
    face.passOp = VK_STENCIL_OP_KEEP;
    face.depthFailOp = VK_STENCIL_OP_KEEP;
    face.compareOp = VK_COMPARE_OP_ALWAYS;
    face.compareMask = 0xffffffffu;
    face.writeMask = 0xffffffffu;
    face.reference = 0;
    return DecodeStencilCompare(function, face.compareOp) &&
        DecodeStencilOperation((state >> (4 + shift)) & 7u, face.failOp) &&
        DecodeStencilOperation((state >> (8 + shift)) & 7u, face.depthFailOp) &&
        DecodeStencilOperation((state >> (12 + shift)) & 7u, face.passOp);
}
}

bool DecodeLegacyPipelineState(uint32_t state, int cull, bool mirror,
                               uint32_t stencilState, VulkanPipelineState& out)
{
    // The GL renderer resolves these flags from per-flush shader state before
    // applying them. A Vulkan pipeline key must be built from that resolved state.
    if ((state & BlendMask) == BlendMask)
        return false;

    out.depthTestEnable = (state & NoDepthTest) == 0;
    out.depthWriteEnable = (state & DepthWrite) != 0;
    out.depthCompareOp = (state & DepthEqual) ? VK_COMPARE_OP_EQUAL :
                         (state & DepthGreater) ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_LESS_OR_EQUAL;
    out.cullMode = cull == 0 ? VK_CULL_MODE_NONE :
                   cull == 1 ? VK_CULL_MODE_FRONT_BIT :
                   cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FLAG_BITS_MAX_ENUM;
    if (out.cullMode == VK_CULL_MODE_FLAG_BITS_MAX_ENUM)
        return false;
    if (mirror && out.cullMode != VK_CULL_MODE_NONE)
        out.cullMode = out.cullMode == VK_CULL_MODE_FRONT_BIT ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_BIT;
    // Vulkan projection uses a Y flip to retain the OpenXR framebuffer convention.
    out.frontFace = VK_FRONT_FACE_CLOCKWISE;
    out.stencilTestEnable = (state & Stencil) != 0;
    const bool twoSidedStencil = (stencilState & 8u) != 0;
    if (out.stencilTestEnable &&
        (!DecodeStencilFace(stencilState, 0, stencilState, out.stencilFront) ||
         !DecodeStencilFace(stencilState, twoSidedStencil ? 16u : 0u,
                            twoSidedStencil ? (stencilState >> 16) : stencilState,
                            out.stencilBack)))
        return false;
    out.blendEnable = (state & BlendMask) != 0;
    out.colorBlendOp = VK_BLEND_OP_ADD;
    out.alphaBlendOp = VK_BLEND_OP_ADD;
    out.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    out.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    out.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    out.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    if (out.blendEnable &&
        (!DecodeSourceBlendFactor(state & 0x0fu, out.srcColorBlendFactor) ||
         !DecodeDestinationBlendFactor(state & 0xf0u, out.dstColorBlendFactor)))
        return false;
    // GL's glBlendFunc applies the same factors to alpha and color.
    out.srcAlphaBlendFactor = out.srcColorBlendFactor;
    out.dstAlphaBlendFactor = out.dstColorBlendFactor;
    out.colorWriteMask = (state & NoColorMask) ? 0 :
        (state & AlphaOnly) ? VK_COLOR_COMPONENT_A_BIT :
        (state & RGBOnly) ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT :
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    out.polygonLine = (state & PolyLine) != 0;
    out.alphaTest = LegacyAlphaTestNone;
    switch (state & AlphaTestMask)
    {
    case 0: break;
    case 0x10000000u: out.alphaTest = LegacyAlphaTestGreaterZero; break;
    case 0x20000000u: out.alphaTest = LegacyAlphaTestLess128; break;
    case 0x40000000u: out.alphaTest = LegacyAlphaTestAtLeast128; break;
    case 0x80000000u: out.alphaTest = LegacyAlphaTestAtLeast64; break;
    default: return false;
    }
    return true;
}
}
