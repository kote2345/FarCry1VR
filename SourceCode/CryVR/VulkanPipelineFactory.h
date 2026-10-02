#ifndef CRY_VR_VULKAN_PIPELINE_FACTORY_H
#define CRY_VR_VULKAN_PIPELINE_FACTORY_H

#include "VulkanContext.h"
#include "VulkanPipelineState.h"
#include "VulkanVertexFormat.h"

#include <array>

namespace CryVR
{
struct VulkanPipelineTextureStage
{
    bool enabled = false;
    uint32_t colorMode = 1;
    uint32_t alphaMode = 1;
    uint32_t colorArg = 0x0a1u;
    uint32_t alphaArg = 0x0a1u;
    uint32_t constant = 0xffffffffu;
    bool useTexCoord1 = true;
    float lodBias = 0.0f;
};

struct VulkanGraphicsPipelineDesc
{
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    const char* vertexEntry = "main";
    const char* fragmentEntry = "main";
    uint32_t cryVertexFormat = 0;
    bool hasTangents = false;
    bool hasLightmapTexCoords = false;
    bool hasNormalMap = false;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    bool useVertexInput = true;
    bool dynamicViewport = true;
    VkExtent2D viewportExtent{};
    uint32_t renderState = 0;
    bool hasColorWriteMaskOverride = false;
    VkColorComponentFlags colorWriteMaskOverride = VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    int cullMode = 2; // R_CULL_BACK
    bool mirror = false;
    bool depthBias = false;
    float depthBiasSlopeFactor = 0.0f;
    float depthBiasConstantFactor = 0.0f;
    bool supportsAlphaTest = false;
    bool supportsDiscardSpecialization = false;
    bool fragmentDiscardEnabled = true;
    bool simpleDecalMode = false;
    bool supportsStereoTransform = false;
    bool multiview = false;
    bool supportsStage1Combine = false;
    bool supportsStage0Combine = false;
    bool hasSecondaryColor = false;
    uint32_t stage0ColorMode = 1;
    uint32_t stage0AlphaMode = 1;
    uint32_t stage0ColorArg = 0x0a1u;
    uint32_t stage0AlphaArg = 0x0a1u;
    uint32_t stage0Constant = 0xffffffffu;
    bool stage0UsesTexCoord1 = false;
    uint32_t stage1ColorMode = 0;
    uint32_t stage1AlphaMode = 0;
    uint32_t stage1ColorArg = 0x0a1u;
    uint32_t stage1AlphaArg = 0x0a1u;
    uint32_t stage1Constant = 0xffffffffu;
    bool stage1UsesTexCoord1 = true;
    bool supportsStage2Combine = false;
    uint32_t stage2ColorMode = 1;
    uint32_t stage2AlphaMode = 1;
    uint32_t stage2ColorArg = 0x0a1u;
    uint32_t stage2AlphaArg = 0x0a1u;
    uint32_t stage2Constant = 0xffffffffu;
    bool stage2UsesTexCoord1 = true;
    bool supportsStage3Combine = false;
    uint32_t stage3ColorMode = 1;
    uint32_t stage3AlphaMode = 1;
    uint32_t stage3ColorArg = 0x0a1u;
    uint32_t stage3AlphaArg = 0x0a1u;
    uint32_t stage3Constant = 0xffffffffu;
    bool stage3UsesTexCoord1 = true;
    bool directionalLightmap = false;
    std::array<VulkanPipelineTextureStage, 4> stages4To7{};
    bool supportsWireframe = false;
    bool dynamicStencil = false;
    uint32_t stencilTestState = 0;
    VkStencilOpState stencilFront{};
    VkStencilOpState stencilBack{};
};

// Creates immutable Vulkan pipelines from the legacy engine's compact state
// and vertex-format IDs. Pipeline ownership remains with the caller.
class VulkanPipelineFactory
{
public:
    bool Initialize(VulkanContext& context);
    void Shutdown();
    bool CreateGraphicsPipeline(const VulkanGraphicsPipelineDesc& desc, VkPipeline& pipeline);
    void DestroyPipeline(VkPipeline& pipeline);
    const char* GetLastError() const { return m_lastError; }
    VkResult GetLastResult() const { return m_lastResult; }

private:
    void SetError(const char* message);
    VulkanContext* m_context = nullptr;
    PFN_vkCreateGraphicsPipelines m_createGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline m_destroyPipeline = nullptr;
    PFN_vkCreatePipelineCache m_createPipelineCache = nullptr;
    PFN_vkDestroyPipelineCache m_destroyPipelineCache = nullptr;
    PFN_vkGetPipelineCacheData m_getPipelineCacheData = nullptr;
    VkPipelineCache m_pipelineCache = VK_NULL_HANDLE;
    char m_lastError[256]{};
    VkResult m_lastResult = VK_SUCCESS;
};
}

#endif
