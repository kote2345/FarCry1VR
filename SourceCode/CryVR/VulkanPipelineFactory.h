#ifndef CRY_VR_VULKAN_PIPELINE_FACTORY_H
#define CRY_VR_VULKAN_PIPELINE_FACTORY_H

#include "VulkanContext.h"
#include "VulkanPipelineState.h"
#include "VulkanVertexFormat.h"

namespace CryVR
{
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
    bool hasNormalMap = false;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    bool useVertexInput = true;
    bool dynamicViewport = true;
    VkExtent2D viewportExtent{};
    uint32_t renderState = 0;
    int cullMode = 2; // R_CULL_BACK
    bool mirror = false;
    bool supportsAlphaTest = false;
    bool supportsStage1Combine = false;
    bool supportsStage0Combine = false;
    bool hasSecondaryColor = false;
    uint32_t stage0ColorMode = 1;
    uint32_t stage0AlphaMode = 1;
    uint32_t stage0ColorArg = 0x0a1u;
    uint32_t stage0AlphaArg = 0x0a1u;
    uint32_t stage0Constant = 0xffffffffu;
    uint32_t stage1ColorMode = 0;
    uint32_t stage1AlphaMode = 0;
    uint32_t stage1ColorArg = 0x0a1u;
    uint32_t stage1AlphaArg = 0x0a1u;
    uint32_t stage1Constant = 0xffffffffu;
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

private:
    void SetError(const char* message);
    VulkanContext* m_context = nullptr;
    PFN_vkCreateGraphicsPipelines m_createGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline m_destroyPipeline = nullptr;
    char m_lastError[256]{};
};
}

#endif
