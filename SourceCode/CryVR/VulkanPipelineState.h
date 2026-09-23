#ifndef CRY_VR_VULKAN_PIPELINE_STATE_H
#define CRY_VR_VULKAN_PIPELINE_STATE_H

#include <vulkan/vulkan.h>
#include <stdint.h>

namespace CryVR
{
enum LegacyAlphaTest
{
    LegacyAlphaTestNone,
    LegacyAlphaTestGreaterZero,
    LegacyAlphaTestLess128,
    LegacyAlphaTestAtLeast128,
    LegacyAlphaTestAtLeast64
};

// Decoded immutable state for Vulkan pipeline creation. Values follow the
// GS_* and R_CULL_* contract in CryCommon/IRenderer.h and the behavior in
// XRenderOGL/GLRendPipeline.cpp::EF_SetState.
struct VulkanPipelineState
{
    bool depthTestEnable;
    bool depthWriteEnable;
    VkCompareOp depthCompareOp;
    VkCullModeFlags cullMode;
    VkFrontFace frontFace;
    bool stencilTestEnable;
    VkStencilOpState stencilFront;
    VkStencilOpState stencilBack;
    bool blendEnable;
    VkBlendFactor srcColorBlendFactor;
    VkBlendFactor dstColorBlendFactor;
    VkBlendOp colorBlendOp;
    VkBlendFactor srcAlphaBlendFactor;
    VkBlendFactor dstAlphaBlendFactor;
    VkBlendOp alphaBlendOp;
    VkColorComponentFlags colorWriteMask;
    bool polygonLine;
    LegacyAlphaTest alphaTest;
};

// Returns false for state requiring engine flush overrides or unsupported
// legacy blend modes. Alpha-test mode is returned for shader specialization.
bool DecodeLegacyPipelineState(uint32_t renderState, int cullMode, bool mirror,
                               uint32_t stencilState, VulkanPipelineState& result);
}

#endif
