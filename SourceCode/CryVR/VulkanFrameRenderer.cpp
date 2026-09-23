#include "VulkanFrameRenderer.h"
#include "VulkanPanelShaders.h"
#include "VulkanSceneShaders.h"
#include "VulkanLitSceneShaders.h"
#include "VulkanFrameCapture.h"

#include <vector>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace CryVR
{
namespace
{
template<class T>
T LoadDevice(PFN_vkGetDeviceProcAddr getProc, VkDevice device, const char* name)
{
    return reinterpret_cast<T>(getProc(device, name));
}

bool IsPreferredFormat(int64_t format)
{
    return format == VK_FORMAT_R8G8B8A8_SRGB ||
           format == VK_FORMAT_R8G8B8A8_UNORM ||
           format == VK_FORMAT_B8G8R8A8_SRGB ||
           format == VK_FORMAT_B8G8R8A8_UNORM;
}

bool HasVulkanVertexColor(int format)
{
    switch (format)
    {
    case 2: case 4: case 5: case 6: case 8: case 10: case 11: case 12: case 13: case 16:
        return true;
    default: return false;
    }
}

bool HasVulkanSecondaryColor(int format)
{
    return format == 6 || format == 11 || format == 12 || format == 13;
}

bool HasVulkanVertexNormal(int format)
{
    switch (format)
    {
    case 7: case 8: case 9: case 10: case 11: case 13: return true;
    default: return false;
    }
}

bool HasVulkanTextureCoordinate(int format)
{
    switch (format)
    {
    case 3: case 4: case 5: case 9: case 10: case 12: case 13: case 16: return true;
    default: return false;
    }
}

bool MapTextureCombineOperation(int legacyOperation, uint32_t& mode)
{
    // Numeric values are EColorOp from CryCommon/IShader.h.
    switch (legacyOperation)
    {
    case -1: case 255: case 0: case 5: mode = 1; return true; // unresolved/default and MODULATE
    case 1: mode = 0; return true;        // DISABLE behaves as REPLACE in legacy GL
    case 2: case 3: mode = 0; return true; // REPLACE / DECAL
    case 6: mode = 2; return true;        // MODULATE2X
    case 7: mode = 3; return true;        // MODULATE4X
    case 4: mode = 8; return true; // ARG2 / SELECTARG2
    case 10: case 13: mode = 1; return true; // legacy backends default these to MODULATE
    case 14: mode = 9; return true;       // MULTIPLYADD
    case 17: mode = 11; return true;      // MODULATEALPHA_ADDCOLOR
    case 18: mode = 12; return true;      // MODULATECOLOR_ADDALPHA
    case 19: mode = 13; return true;      // MODULATEINVALPHA_ADDCOLOR
    case 20: mode = 14; return true;      // MODULATEINVCOLOR_ADDALPHA
    case 21: mode = 15; return true;      // DOTPRODUCT3
    case 22: mode = 16; return true;      // LERP
    case 23: mode = 10; return true;      // SUBTRACT
    case 16: mode = 1; return true;       // legacy backends default BLEND to MODULATE
    case 11: mode = 4; return true;       // ADD
    case 12: mode = 5; return true;       // ADDSIGNED
    case 8: mode = 6; return true;        // BLENDDIFFUSEALPHA
    case 9: mode = 7; return true;        // BLENDTEXTUREALPHA
    default: return false;
    }
}

bool MapTextureAlphaOperation(int legacyOperation, uint32_t& mode)
{
    if (legacyOperation == 8) { mode = 6; return true; } // BLENDDIFFUSEALPHA
    if (legacyOperation == 9) { mode = 7; return true; } // BLENDTEXTUREALPHA
    if (legacyOperation == 4) { mode = 8; return true; } // SELECTARG2
    // These operators are defined for color in the stock D3D9 path but have
    // no separate alpha-stage mapping there; preserve its MODULATE default.
    if ((legacyOperation >= 17 && legacyOperation <= 21))
    {
        mode = 1;
        return true;
    }
    return MapTextureCombineOperation(legacyOperation, mode);
}

}

VulkanFrameRenderer::~VulkanFrameRenderer()
{
    Shutdown();
}

void VulkanFrameRenderer::SetError(const char* message)
{
    std::snprintf(m_lastError, sizeof(m_lastError), "%s", message ? message : "unknown error");
}

bool VulkanFrameRenderer::LoadFunctions()
{
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = m_context->GetInstanceProcAddr();
    m_getPhysicalDeviceFormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
        getInstanceProcAddr(m_context->GetInstance(), "vkGetPhysicalDeviceFormatProperties"));
    m_getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        getInstanceProcAddr(m_context->GetInstance(), "vkGetDeviceProcAddr"));
    if (!m_getDeviceProcAddr || !m_getPhysicalDeviceFormatProperties)
    {
        SetError("vkGetDeviceProcAddr is unavailable");
        return false;
    }

#define LOAD_VK(field, name) m_##field = LoadDevice<PFN_vk##name>(m_getDeviceProcAddr, m_context->GetDevice(), "vk" #name)
    LOAD_VK(destroyImageView, DestroyImageView);
    LOAD_VK(createImageView, CreateImageView);
    LOAD_VK(destroyRenderPass, DestroyRenderPass);
    LOAD_VK(createRenderPass, CreateRenderPass);
    LOAD_VK(destroyPipelineLayout, DestroyPipelineLayout);
    LOAD_VK(createPipelineLayout, CreatePipelineLayout);
    LOAD_VK(destroyPipeline, DestroyPipeline);
    LOAD_VK(createGraphicsPipelines, CreateGraphicsPipelines);
    LOAD_VK(createShaderModule, CreateShaderModule);
    LOAD_VK(destroyShaderModule, DestroyShaderModule);
    LOAD_VK(destroyFramebuffer, DestroyFramebuffer);
    LOAD_VK(createFramebuffer, CreateFramebuffer);
    LOAD_VK(destroyCommandPool, DestroyCommandPool);
    LOAD_VK(createCommandPool, CreateCommandPool);
    LOAD_VK(allocateCommandBuffers, AllocateCommandBuffers);
    LOAD_VK(freeCommandBuffers, FreeCommandBuffers);
    LOAD_VK(beginCommandBuffer, BeginCommandBuffer);
    LOAD_VK(endCommandBuffer, EndCommandBuffer);
    LOAD_VK(cmdPipelineBarrier, CmdPipelineBarrier);
    LOAD_VK(cmdBeginRenderPass, CmdBeginRenderPass);
    LOAD_VK(cmdEndRenderPass, CmdEndRenderPass);
    LOAD_VK(cmdClearAttachments, CmdClearAttachments);
    LOAD_VK(cmdBindPipeline, CmdBindPipeline);
    LOAD_VK(cmdDraw, CmdDraw);
    LOAD_VK(cmdDrawIndexed, CmdDrawIndexed);
    LOAD_VK(cmdSetStencilCompareMask, CmdSetStencilCompareMask);
    LOAD_VK(cmdSetStencilWriteMask, CmdSetStencilWriteMask);
    LOAD_VK(cmdSetStencilReference, CmdSetStencilReference);
    LOAD_VK(cmdBindVertexBuffers, CmdBindVertexBuffers);
    LOAD_VK(cmdBindIndexBuffer, CmdBindIndexBuffer);
    LOAD_VK(cmdSetViewport, CmdSetViewport);
    LOAD_VK(cmdSetScissor, CmdSetScissor);
    LOAD_VK(cmdPushConstants, CmdPushConstants);
    LOAD_VK(cmdBindDescriptorSets, CmdBindDescriptorSets);
    LOAD_VK(createDescriptorSetLayout, CreateDescriptorSetLayout);
    LOAD_VK(destroyDescriptorSetLayout, DestroyDescriptorSetLayout);
    LOAD_VK(createDescriptorPool, CreateDescriptorPool);
    LOAD_VK(destroyDescriptorPool, DestroyDescriptorPool);
    LOAD_VK(allocateDescriptorSets, AllocateDescriptorSets);
    LOAD_VK(freeDescriptorSets, FreeDescriptorSets);
    LOAD_VK(updateDescriptorSets, UpdateDescriptorSets);
    LOAD_VK(createSampler, CreateSampler);
    LOAD_VK(destroySampler, DestroySampler);
    LOAD_VK(queueSubmit, QueueSubmit);
    LOAD_VK(createFence, CreateFence);
    LOAD_VK(destroyFence, DestroyFence);
    LOAD_VK(waitForFences, WaitForFences);
    LOAD_VK(resetFences, ResetFences);
#undef LOAD_VK

    return m_destroyImageView && m_createImageView && m_destroyRenderPass && m_createRenderPass &&
           m_destroyPipelineLayout && m_createPipelineLayout && m_destroyPipeline &&
           m_createGraphicsPipelines && m_createShaderModule && m_destroyShaderModule &&
           m_destroyFramebuffer && m_createFramebuffer && m_destroyCommandPool && m_createCommandPool &&
           m_allocateCommandBuffers && m_freeCommandBuffers && m_beginCommandBuffer && m_endCommandBuffer &&
           m_cmdPipelineBarrier && m_cmdBeginRenderPass && m_cmdEndRenderPass && m_cmdBindPipeline &&
           m_cmdClearAttachments &&
           m_cmdDraw && m_cmdDrawIndexed && m_cmdBindVertexBuffers && m_cmdBindIndexBuffer &&
           m_cmdSetViewport && m_cmdSetScissor && m_cmdPushConstants && m_cmdBindDescriptorSets &&
           m_createDescriptorSetLayout && m_destroyDescriptorSetLayout &&
           m_createDescriptorPool && m_destroyDescriptorPool && m_allocateDescriptorSets &&
           m_freeDescriptorSets &&
           m_updateDescriptorSets && m_createSampler && m_destroySampler &&
           m_queueSubmit && m_createFence && m_destroyFence && m_waitForFences && m_resetFences &&
           m_cmdSetStencilCompareMask &&
           m_cmdSetStencilWriteMask && m_cmdSetStencilReference;
}

bool VulkanFrameRenderer::Initialize(Runtime& runtime, VulkanContext& context)
{
    Shutdown();
    if (!context.IsInitialized() || !runtime.IsInitialized())
    {
        SetError("OpenXR/Vulkan context is not initialized");
        return false;
    }
    m_runtime = &runtime;
    m_context = &context;
    m_viewCount = runtime.GetViewCount();
    if (m_viewCount == 0 || m_viewCount > 2)
        m_viewCount = 2;
    if (!LoadFunctions())
    {
        Shutdown();
        return false;
    }
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (m_createFence(context.GetDevice(), &fenceInfo, nullptr, &m_frameFence) != VK_SUCCESS)
    {
        SetError("vkCreateFence failed for XR frame submission");
        Shutdown();
        return false;
    }

    std::vector<int64_t> formats;
    if (!runtime.EnumerateVulkanSwapchainFormats(formats))
    {
        SetError("OpenXR returned no Vulkan swapchain formats");
        Shutdown();
        return false;
    }
    int64_t selectedFormat = formats.front();
    for (int64_t format : formats)
    {
        if (IsPreferredFormat(format))
        {
            selectedFormat = format;
            break;
        }
    }
    m_format = static_cast<VkFormat>(selectedFormat);

    const uint32_t width = runtime.GetRecommendedViewWidth();
    const uint32_t height = runtime.GetRecommendedViewHeight();
    if (width == 0 || height == 0 || !runtime.CreateVulkanSwapchain(selectedFormat, width, height, m_viewCount, m_swapchain))
    {
        SetError("OpenXR Vulkan swapchain creation failed");
        Shutdown();
        return false;
    }

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = context.GetGraphicsQueueFamily();
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (m_createCommandPool(context.GetDevice(), &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS)
    {
        SetError("vkCreateCommandPool failed");
        Shutdown();
        return false;
    }

    m_depthFormat = VK_FORMAT_UNDEFINED;
    const VkFormat depthCandidates[] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                         VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM };
    for (size_t i = 0; i < sizeof(depthCandidates) / sizeof(depthCandidates[0]); ++i)
    {
        VkFormatProperties properties{};
        m_getPhysicalDeviceFormatProperties(context.GetPhysicalDevice(), depthCandidates[i], &properties);
        if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        {
            m_depthFormat = depthCandidates[i];
            break;
        }
    }
    if (m_depthFormat == VK_FORMAT_UNDEFINED)
    {
        SetError("Vulkan device exposes no supported depth attachment format");
        Shutdown();
        return false;
    }
    VkAttachmentDescription attachments[2]{};
    VkAttachmentDescription& attachment = attachments[0];
    attachment.format = m_format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription& depthAttachment = attachments[1];
    depthAttachment.format = m_depthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    const bool hasStencil = m_depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
                            m_depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT;
    depthAttachment.stencilLoadOp = hasStencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorReference{};
    colorReference.attachment = 0;
    colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;
    VkAttachmentReference depthReference{};
    depthReference.attachment = 1;
    depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    subpass.pDepthStencilAttachment = &depthReference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 2;
    renderPassInfo.pAttachments = attachments;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;
    if (m_createRenderPass(context.GetDevice(), &renderPassInfo, nullptr, &m_renderPass) != VK_SUCCESS)
    {
        SetError("vkCreateRenderPass failed");
        Shutdown();
        return false;
    }

    if (!CreateRenderTargets())
    {
        Shutdown();
        return false;
    }
    if (!CreatePanelPipeline())
    {
        Shutdown();
        return false;
    }
    if (!CreateScenePipelines())
    {
        Shutdown();
        return false;
    }
    if (m_resources)
    {
        const VkMemoryPropertyFlags hostMemory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (!m_resources->CreateBuffer(8ull * 1024ull * 1024ull, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                       hostMemory, m_dynamicVertexBuffer) ||
            !m_resources->CreateBuffer(4ull * 1024ull * 1024ull, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                       hostMemory, m_dynamicIndexBuffer))
        {
            m_resources->DestroyBuffer(m_dynamicVertexBuffer);
            m_resources->DestroyBuffer(m_dynamicIndexBuffer);
        }
    }
    m_referenceHeadPoseValid = false;
    m_initialized = true;
    return true;
}

bool VulkanFrameRenderer::CreateScenePipelines()
{
    VkShaderModuleCreateInfo shaderInfo{};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = sizeof(kVrSceneVertexSpirv);
    shaderInfo.pCode = kVrSceneVertexSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr, &m_sceneVertexShader) != VK_SUCCESS)
    {
        SetError("failed to create scene vertex shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrSceneFragmentSpirv);
    shaderInfo.pCode = kVrSceneFragmentSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr, &m_sceneFragmentShader) != VK_SUCCESS)
    {
        SetError("failed to create scene fragment shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrSceneColorVertexSpirv);
    shaderInfo.pCode = kVrSceneColorVertexSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr, &m_sceneColorVertexShader) != VK_SUCCESS)
    {
        SetError("failed to create color scene vertex shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrSceneColorFragmentSpirv);
    shaderInfo.pCode = kVrSceneColorFragmentSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr, &m_sceneColorFragmentShader) != VK_SUCCESS)
    {
        SetError("failed to create color scene fragment shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrLitVertexSpirv);
    shaderInfo.pCode = kVrLitVertexSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr, &m_sceneLitVertexShader) != VK_SUCCESS)
    {
        SetError("failed to create lit scene vertex shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrLitColorVertexSpirv);
    shaderInfo.pCode = kVrLitColorVertexSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr, &m_sceneLitColorVertexShader) != VK_SUCCESS)
    {
        SetError("failed to create lit color scene vertex shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrLitFragmentSpirv);
    shaderInfo.pCode = kVrLitFragmentSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr, &m_sceneLitFragmentShader) != VK_SUCCESS)
    {
        SetError("failed to create lit scene fragment shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrSceneTextureVertexSpirv);
    shaderInfo.pCode = kVrSceneTextureVertexSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr, &m_sceneTextureVertexShader) != VK_SUCCESS)
    {
        SetError("failed to create texture scene vertex shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrSceneTextureFragmentSpirv);
    shaderInfo.pCode = kVrSceneTextureFragmentSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr, &m_sceneTextureFragmentShader) != VK_SUCCESS)
    {
        SetError("failed to create texture scene fragment shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrSceneTextureColorVertexSpirv);
    shaderInfo.pCode = kVrSceneTextureColorVertexSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr, &m_sceneTextureColorVertexShader) != VK_SUCCESS)
    {
        SetError("failed to create textured color vertex shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrSceneTextureColorFragmentSpirv);
    shaderInfo.pCode = kVrSceneTextureColorFragmentSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr, &m_sceneTextureColorFragmentShader) != VK_SUCCESS)
    {
        SetError("failed to create textured color fragment shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrSceneMultiTextureColorVertexSpirv);
    shaderInfo.pCode = kVrSceneMultiTextureColorVertexSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr,
                             &m_sceneMultiTextureColorVertexShader) != VK_SUCCESS)
    {
        SetError("failed to create multitexture vertex shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrSceneMultiTextureColorFragmentSpirv);
    shaderInfo.pCode = kVrSceneMultiTextureColorFragmentSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr,
                             &m_sceneMultiTextureColorFragmentShader) != VK_SUCCESS)
    {
        SetError("failed to create multitexture fragment shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrSceneTextureLitVertexSpirv);
    shaderInfo.pCode = kVrSceneTextureLitVertexSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr,
                             &m_sceneTextureLitVertexShader) != VK_SUCCESS)
    {
        SetError("failed to create lit texture vertex shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrSceneTextureLitColorVertexSpirv);
    shaderInfo.pCode = kVrSceneTextureLitColorVertexSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr,
                             &m_sceneTextureLitColorVertexShader) != VK_SUCCESS)
    {
        SetError("failed to create lit colored texture vertex shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrSceneTextureBumpVertexSpirv);
    shaderInfo.pCode = kVrSceneTextureBumpVertexSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr,
                             &m_sceneTextureBumpVertexShader) != VK_SUCCESS)
    {
        SetError("failed to create bump-mapped texture vertex shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrSceneTextureBumpColorVertexSpirv);
    shaderInfo.pCode = kVrSceneTextureBumpColorVertexSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr,
                             &m_sceneTextureBumpColorVertexShader) != VK_SUCCESS)
    {
        SetError("failed to create bump-mapped colored texture vertex shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrSceneTextureBumpFragmentSpirv);
    shaderInfo.pCode = kVrSceneTextureBumpFragmentSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr,
                             &m_sceneTextureBumpFragmentShader) != VK_SUCCESS)
    {
        SetError("failed to create per-fragment bump lighting shader module");
        return false;
    }
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    // RecordAndSubmit pushes two complete mat4 values for scene draws
    // (MVP + eye/model-view, 128 bytes). A 96-byte range made every such
    // vkCmdPushConstants call exceed the pipeline-layout range.
    pushRange.size = sizeof(float) * 32;
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkDescriptorSetLayout sceneSetLayouts[2] = { m_descriptorSetLayout, m_descriptorSetLayout };
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = sceneSetLayouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (m_createPipelineLayout(m_context->GetDevice(), &layoutInfo, nullptr,
                               &m_scenePipelineLayout) != VK_SUCCESS)
    {
        SetError("failed to create scene pipeline layout");
        return false;
    }
    return true;
}

bool VulkanFrameRenderer::QueueStockIndexedDraw(const VulkanBuffer* vertexBuffer,
                                                 const VulkanBuffer* indexBuffer,
                                                 int vertexFormat, uint32_t indexCount,
                                                 uint32_t firstIndex, int primitiveMode,
                                                 uint32_t renderState, int cullMode, int textureId, int textureId1,
                                                 int textureStage0ColorOp, int textureStage0AlphaOp,
                                                 int textureStage1ColorOp, int textureStage1AlphaOp,
                                                 uint32_t textureStage0ColorArg, uint32_t textureStage0AlphaArg,
                                                 uint32_t textureStage0Constant, uint32_t textureStage1ColorArg,
                                                 uint32_t textureStage1AlphaArg, uint32_t textureStage1Constant,
                                                 uint32_t stencilState, uint32_t stencilRef, uint32_t stencilMask,
                                                 const float modelView[16], const float textureMatrix0[16],
                                                 const float textureMatrix1[16], int32_t vertexOffset,
                                                 const float* materialLighting,
                                                 float globalOpacity, float alphaTestRef,
                                                 const VulkanBuffer* tangentBuffer,
                                                 int normalMapTextureId)
{
    if (!m_frameActive || !m_frame.shouldRender || !m_frame.viewsValid ||
        !vertexBuffer || !indexBuffer || !vertexBuffer->buffer || !indexBuffer->buffer ||
        !modelView || !textureMatrix0 || !textureMatrix1 || indexCount == 0 || vertexFormat < 1 || vertexFormat > 16 ||
        // Stock font vertices carry pre-projected screen coordinates plus RHW.
        // They remain visible in the captured 2D panel; sending them through
        // the world-space OpenXR projection produces misplaced HUD geometry.
        vertexFormat == 5 || vertexFormat == 14 || vertexFormat == 15 ||
        (primitiveMode < 0 || primitiveMode > 2))
        return false;
    if (vertexFormat == 14 || vertexFormat == 15 || cullMode < 0 || cullMode > 2)
        return false;
    const uint32_t topologyIndex = static_cast<uint32_t>(primitiveMode);
    const std::map<int, LegacyTexture>::const_iterator texture = m_legacyTextures.find(textureId);
    const bool textureExpected = HasVulkanTextureCoordinate(vertexFormat) && textureId > 0;
    if (textureExpected && texture == m_legacyTextures.end())
        return false;
    const bool useTexture = textureExpected;
    const std::map<int, LegacyTexture>::const_iterator texture1 = m_legacyTextures.find(textureId1);
    if (vertexFormat == 16 && textureId1 > 0 && textureStage1ColorOp != 1 &&
        texture1 == m_legacyTextures.end())
        return false;
    uint32_t stage0ColorMode = 1;
    uint32_t stage0AlphaMode = 1;
    const bool stage0OpsSupported = MapTextureCombineOperation(textureStage0ColorOp, stage0ColorMode) &&
                                    MapTextureAlphaOperation(textureStage0AlphaOp, stage0AlphaMode);
    if (useTexture && !stage0OpsSupported)
        return false;
    const auto validInterpolateArgs = [](uint32_t packedArgs)
    {
        for (uint32_t source = 0; source < 2; ++source)
        {
            const uint32_t selector = (packedArgs >> (source * 3)) & 7u;
            if (selector > 4u) return false;
        }
        return true;
    };
    if ((stage0ColorMode == 6 || stage0ColorMode == 7) &&
        (!HasVulkanVertexColor(vertexFormat) || !validInterpolateArgs(textureStage0ColorArg)))
        return false;
    uint32_t stage1ColorMode = 1;
    uint32_t stage1AlphaMode = 1;
    const bool stage1OpsSupported = MapTextureCombineOperation(textureStage1ColorOp, stage1ColorMode) &&
                                    MapTextureAlphaOperation(textureStage1AlphaOp, stage1AlphaMode);
    const bool secondTextureAvailable = vertexFormat == 16 && useTexture && textureId1 > 0 &&
                                        texture1 != m_legacyTextures.end();
    if (secondTextureAvailable && !stage1OpsSupported)
        return false;
    const bool useSecondTexture = secondTextureAvailable && textureStage1ColorOp != 1;
    if (useSecondTexture && (stage1ColorMode == 6 || stage1ColorMode == 7) &&
        (!validInterpolateArgs(textureStage1ColorArg) || !HasVulkanVertexColor(vertexFormat)))
        return false;
    const bool hasTangentBasis = tangentBuffer && tangentBuffer->buffer &&
                                 (vertexFormat == 9 || vertexFormat == 10 || vertexFormat == 13);
    const std::map<int, LegacyTexture>::const_iterator normalMap =
        m_legacyTextures.find(normalMapTextureId);
    const bool useNormalMap = hasTangentBasis && useTexture && normalMapTextureId > 0 &&
                              normalMap != m_legacyTextures.end() && !useSecondTexture;
    if (normalMapTextureId > 0 && !useNormalMap)
        return false;
    // Drop stock flags that affect fixed-function texture/shader stages only;
    // otherwise harmless material bits would create duplicate Vulkan pipelines.
    const uint32_t pipelineStateMask = 0x000000ffu | 0x00000100u | 0x00000400u |
        0x00001000u | 0x00010000u | 0x00020000u | 0x00040000u |
        0x00100000u | 0x00200000u | 0x00400000u | 0xf0000000u;
    const uint32_t pipelineState = renderState & pipelineStateMask;
    const bool stencilEnabled = (pipelineState & 0x00400000u) != 0;
    const bool depthHasStencil = m_depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
                                 m_depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT;
    if (stencilEnabled && !depthHasStencil)
        return false;
    VulkanPipelineState decodedState{};
    if (!DecodeLegacyPipelineState(pipelineState, cullMode, false, stencilState, decodedState))
        return false;
    const uint64_t compactPipelineKey = (static_cast<uint64_t>(useTexture ? 1u : 0u) << 63) |
                                 (static_cast<uint64_t>(useSecondTexture ? 1u : 0u) << 62) |
                                 (static_cast<uint64_t>(useNormalMap ? 1u : 0u) << 61) |
                                 (static_cast<uint64_t>(hasTangentBasis ? 1u : 0u) << 60) |
                                 (static_cast<uint64_t>(pipelineState) << 24) |
                                 (static_cast<uint64_t>(vertexFormat) << 16) |
                                 (static_cast<uint64_t>(topologyIndex) << 8) |
                                 static_cast<uint64_t>(cullMode);
    std::array<uint32_t, 13> pipelineKey = {{
        static_cast<uint32_t>(compactPipelineKey), static_cast<uint32_t>(compactPipelineKey >> 32),
        (renderState & 0x00400000u) ? stencilState : 0u, stage0ColorMode, stage0AlphaMode,
        textureStage0ColorArg, textureStage0AlphaArg, textureStage0Constant,
        textureStage1ColorArg, textureStage1AlphaArg, textureStage1Constant,
        stage1ColorMode, stage1AlphaMode
    }};
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::map<std::array<uint32_t, 13>, VkPipeline>::iterator cached = m_scenePipelineCache.find(pipelineKey);
    if (cached != m_scenePipelineCache.end())
        pipeline = cached->second;
    else
    {
        const bool hasNormal = HasVulkanVertexNormal(vertexFormat);
        const bool hasColor = HasVulkanVertexColor(vertexFormat);
        VulkanGraphicsPipelineDesc desc{};
        desc.renderPass = m_renderPass;
        desc.layout = m_scenePipelineLayout;
        desc.vertexShader = useNormalMap ? (hasColor ? m_sceneTextureBumpColorVertexShader :
                                                       m_sceneTextureBumpVertexShader) :
                            useSecondTexture ? m_sceneMultiTextureColorVertexShader :
                            useTexture && hasNormal ? (hasColor ? m_sceneTextureLitColorVertexShader :
                                                          m_sceneTextureLitVertexShader) :
                            useTexture ? (hasColor ? m_sceneTextureColorVertexShader :
                                                     m_sceneTextureVertexShader) :
                            hasNormal ? (hasColor ? m_sceneLitColorVertexShader : m_sceneLitVertexShader) :
                            (hasColor ? m_sceneColorVertexShader : m_sceneVertexShader);
        desc.fragmentShader = useNormalMap ? m_sceneTextureBumpFragmentShader :
                              useSecondTexture ? m_sceneMultiTextureColorFragmentShader :
                              useTexture ? m_sceneTextureColorFragmentShader :
                              hasNormal ? m_sceneLitFragmentShader :
                              (hasColor ? m_sceneColorFragmentShader : m_sceneFragmentShader);
        desc.cryVertexFormat = static_cast<uint32_t>(vertexFormat);
        desc.hasTangents = tangentBuffer && tangentBuffer->buffer &&
                           (vertexFormat == 9 || vertexFormat == 10 || vertexFormat == 13);
        desc.hasNormalMap = useNormalMap;
        desc.topology = topologyIndex == 0 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST :
                        topologyIndex == 1 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP :
                                             VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        desc.renderState = pipelineState;
        desc.cullMode = cullMode;
        desc.supportsAlphaTest = true;
        desc.supportsWireframe = m_context->SupportsWireframe();
        desc.supportsStage0Combine = useTexture;
        desc.hasSecondaryColor = HasVulkanSecondaryColor(vertexFormat);
        desc.stage0ColorMode = stage0ColorMode;
        desc.stage0AlphaMode = stage0AlphaMode;
        desc.stage0ColorArg = textureStage0ColorArg;
        desc.stage0AlphaArg = textureStage0AlphaArg;
        desc.stage0Constant = textureStage0Constant;
        desc.supportsStage1Combine = useSecondTexture;
        desc.stencilTestState = stencilState;
        desc.dynamicStencil = stencilEnabled;
        desc.stencilFront = decodedState.stencilFront;
        desc.stencilBack = decodedState.stencilBack;
        desc.stage1ColorMode = stage1ColorMode;
        desc.stage1AlphaMode = stage1AlphaMode;
        desc.stage1ColorArg = textureStage1ColorArg;
        desc.stage1AlphaArg = textureStage1AlphaArg;
        desc.stage1Constant = textureStage1Constant;
        if (!m_pipelineFactory.CreateGraphicsPipeline(desc, pipeline))
            return false;
        m_scenePipelineCache[pipelineKey] = pipeline;
    }
    StockDraw draw;
    draw.vertexBuffer = vertexBuffer->buffer;
    draw.tangentBuffer = (vertexFormat == 9 || vertexFormat == 10 || vertexFormat == 13) &&
                         tangentBuffer ? tangentBuffer->buffer : VK_NULL_HANDLE;
    draw.indexBuffer = indexBuffer->buffer;
    draw.vertexFormat = vertexFormat;
    draw.indexCount = indexCount;
    draw.firstIndex = firstIndex;
    draw.vertexOffset = vertexOffset;
    draw.topology = primitiveMode == 0 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST :
                    primitiveMode == 1 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP :
                                         VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    draw.pipeline = pipeline;
    draw.textureId = useTexture ? textureId : 0;
    draw.textureId1 = useSecondTexture ? textureId1 : 0;
    draw.useSecondTexture = useSecondTexture;
    draw.useNormalMap = useNormalMap;
    draw.normalMapTextureId = useNormalMap ? normalMapTextureId : 0;
    draw.globalOpacity = globalOpacity;
    draw.alphaTestRef = alphaTestRef;
    draw.additiveMaterial = (renderState & 0xffu) == (0x2u | 0x20u);
    draw.stage1ColorMode = stage1ColorMode;
    draw.stage1AlphaMode = stage1AlphaMode;
    draw.stage0ColorMode = stage0ColorMode;
    draw.stage0AlphaMode = stage0AlphaMode;
    draw.stage0ColorArg = textureStage0ColorArg;
    draw.stage0AlphaArg = textureStage0AlphaArg;
    draw.stage0Constant = textureStage0Constant;
    draw.stage1ColorArg = textureStage1ColorArg;
    draw.stage1AlphaArg = textureStage1AlphaArg;
    draw.stage1Constant = textureStage1Constant;
    draw.stencilState = stencilEnabled ? stencilState : 0u;
    draw.stencilRef = stencilRef;
    draw.stencilMask = stencilMask;
    draw.scissorEnabled = m_stockScissorEnabled;
    draw.scissor = m_stockScissor;
    draw.fogEnabled = m_stockFogEnabled;
    std::memcpy(draw.fogColor, m_stockFogColor, sizeof(draw.fogColor));
    draw.fogDensity = m_stockFogDensity;
    draw.fogStart = m_stockFogStart;
    draw.fogEnd = m_stockFogEnd;
    draw.fogMode = m_stockFogMode;
    for (int i = 0; i < 16; ++i) draw.modelView[i] = modelView[i];
    for (int i = 0; i < 16; ++i)
    {
        draw.textureMatrix0[i] = textureMatrix0[i];
        draw.textureMatrix1[i] = textureMatrix1[i];
    }
    if (materialLighting)
        std::memcpy(draw.materialLighting, materialLighting, sizeof(draw.materialLighting));
    m_stockDraws.push_back(draw);
    return true;
}

bool VulkanFrameRenderer::QueueStockClientIndexedDraw(const void* vertices, uint32_t vertexCount,
                                                        const uint16_t* indices, uint32_t indexCount,
                                                        int vertexFormat, int primitiveMode,
                                                        uint32_t renderState, int cullMode,
                                                        int textureId, int textureId1,
                                                        int textureStage0ColorOp, int textureStage0AlphaOp,
                                                        int textureStage1ColorOp, int textureStage1AlphaOp,
                                                        uint32_t textureStage0ColorArg, uint32_t textureStage0AlphaArg,
                                                        uint32_t textureStage0Constant, uint32_t textureStage1ColorArg,
                                                        uint32_t textureStage1AlphaArg, uint32_t textureStage1Constant,
                                                        uint32_t stencilState, uint32_t stencilRef, uint32_t stencilMask,
                                                        const float modelView[16],
                                                        const float textureMatrix0[16],
                                                        const float textureMatrix1[16],
                                                        const float* materialLighting,
                                                        bool reusePreviousClientGeometry,
                                                        float globalOpacity, float alphaTestRef,
                                                        const VulkanBuffer* tangentBuffer,
                                                        int normalMapTextureId)
{
    if (!m_resources || !vertices || vertexCount == 0 ||
        !m_dynamicVertexBuffer.buffer || !m_dynamicIndexBuffer.buffer)
        return false;
    const uint16_t* sourceIndices = indices;
    if (reusePreviousClientGeometry && sourceIndices && m_reusableClientGeometry.valid &&
        vertices == m_reusableClientGeometry.sourceVertices &&
        sourceIndices == m_reusableClientGeometry.sourceIndices &&
        vertexCount == m_reusableClientGeometry.vertexCount &&
        indexCount == m_reusableClientGeometry.indexCount &&
        vertexFormat == m_reusableClientGeometry.vertexFormat &&
        primitiveMode == m_reusableClientGeometry.primitiveMode)
    {
        if (QueueStockIndexedDraw(&m_reusableClientGeometry.vertexBuffer,
                                  &m_reusableClientGeometry.indexBuffer,
                                  vertexFormat, indexCount,
                                  m_reusableClientGeometry.firstIndex, primitiveMode,
                                  renderState, cullMode, textureId, textureId1,
                                  textureStage0ColorOp, textureStage0AlphaOp,
                                  textureStage1ColorOp, textureStage1AlphaOp,
                                  textureStage0ColorArg, textureStage0AlphaArg, textureStage0Constant,
                                  textureStage1ColorArg, textureStage1AlphaArg, textureStage1Constant,
                                  stencilState, stencilRef, stencilMask,
                                  modelView, textureMatrix0, textureMatrix1,
                                  m_reusableClientGeometry.vertexOffset, materialLighting,
                                  globalOpacity, alphaTestRef, tangentBuffer, normalMapTextureId))
            return true;
        m_reusableClientGeometry.valid = false;
    }
    std::vector<uint16_t> generatedIndices;
    if (!indices)
    {
        if (indexCount != 0 || vertexCount > 65535)
            return false;
        generatedIndices.resize(vertexCount);
        for (uint32_t i = 0; i < vertexCount; ++i)
            generatedIndices[i] = static_cast<uint16_t>(i);
        indices = generatedIndices.data();
        indexCount = vertexCount;
    }
    if (indexCount == 0)
        return false;
    VulkanVertexFormat format{};
    if (!GetVulkanVertexFormat(static_cast<uint32_t>(vertexFormat), format))
        return false;
    const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(vertexCount) * format.stride;
    const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(indexCount) * sizeof(uint16_t);
    if (vertexBytes > std::numeric_limits<VkDeviceSize>::max() - m_dynamicVertexUsed ||
        indexBytes > std::numeric_limits<VkDeviceSize>::max() - m_dynamicIndexUsed ||
        !EnsureDynamicBufferCapacity(m_dynamicVertexBuffer, m_previousDynamicVertexBuffers,
                                     m_dynamicVertexUsed + vertexBytes,
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !EnsureDynamicBufferCapacity(m_dynamicIndexBuffer, m_previousDynamicIndexBuffers,
                                     m_dynamicIndexUsed + indexBytes,
                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
        return false;
    if (!m_resources->UploadBuffer(m_dynamicVertexBuffer, vertices, vertexBytes, m_dynamicVertexUsed) ||
        !m_resources->UploadBuffer(m_dynamicIndexBuffer, indices, indexBytes, m_dynamicIndexUsed))
        return false;
    const uint32_t firstIndex = static_cast<uint32_t>(m_dynamicIndexUsed / sizeof(uint16_t));
    const int32_t firstVertex = static_cast<int32_t>(m_dynamicVertexUsed / format.stride);
    if (!QueueStockIndexedDraw(&m_dynamicVertexBuffer, &m_dynamicIndexBuffer, vertexFormat,
                               indexCount, firstIndex, primitiveMode, renderState, cullMode,
                               textureId, textureId1, textureStage0ColorOp, textureStage0AlphaOp,
                               textureStage1ColorOp, textureStage1AlphaOp,
                               textureStage0ColorArg, textureStage0AlphaArg, textureStage0Constant,
                               textureStage1ColorArg, textureStage1AlphaArg, textureStage1Constant,
                               stencilState, stencilRef, stencilMask,
                               modelView, textureMatrix0, textureMatrix1, firstVertex,
                               materialLighting, globalOpacity, alphaTestRef,
                               tangentBuffer, normalMapTextureId))
        return false;
    m_dynamicVertexUsed += vertexBytes;
    m_dynamicIndexUsed += indexBytes;
    if (sourceIndices)
    {
        m_reusableClientGeometry.valid = true;
        m_reusableClientGeometry.sourceVertices = vertices;
        m_reusableClientGeometry.sourceIndices = sourceIndices;
        m_reusableClientGeometry.vertexCount = vertexCount;
        m_reusableClientGeometry.indexCount = indexCount;
        m_reusableClientGeometry.vertexFormat = vertexFormat;
        m_reusableClientGeometry.primitiveMode = primitiveMode;
        m_reusableClientGeometry.vertexBuffer = m_dynamicVertexBuffer;
        m_reusableClientGeometry.indexBuffer = m_dynamicIndexBuffer;
        m_reusableClientGeometry.firstIndex = firstIndex;
        m_reusableClientGeometry.vertexOffset = firstVertex;
    }
    return true;
}

bool VulkanFrameRenderer::EnsureDynamicBufferCapacity(VulkanBuffer& buffer,
                                                        std::vector<VulkanBuffer>& previousBuffers,
                                                        VkDeviceSize requiredSize,
                                                        VkBufferUsageFlags usage)
{
    if (requiredSize <= buffer.size)
        return buffer.buffer != VK_NULL_HANDLE;
    if (!m_resources || !buffer.buffer)
        return false;

    VkDeviceSize capacity = buffer.size;
    while (capacity < requiredSize)
    {
        if (capacity > std::numeric_limits<VkDeviceSize>::max() / 2)
        {
            capacity = requiredSize;
            break;
        }
        capacity *= 2;
    }

    const VkMemoryPropertyFlags hostMemory = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VulkanBuffer grownBuffer;
    if (!m_resources->CreateBuffer(capacity, usage, hostMemory, grownBuffer))
        return false;

    // Queued StockDraw entries hold the old VkBuffer handle. Keep its backing
    // allocation until this frame's fence has completed; BeginFrame reclaims
    // these retired chunks after waiting for that fence.
    previousBuffers.push_back(buffer);
    buffer = grownBuffer;
    return true;
}

bool VulkanFrameRenderer::QueuePanelImage(int textureId, float x, float y, float width, float height,
                                          float s0, float t0, float s1, float t1, float angleDegrees,
                                          float red, float green, float blue, float alpha,
                                          float logicalWidth, float logicalHeight)
{
    if (!m_frameActive || !m_frame.shouldRender || textureId <= 0 ||
        !(width > 0.0f) || !(height > 0.0f) ||
        !(logicalWidth > 0.0f) || !(logicalHeight > 0.0f))
        return false;
    PanelImage image;
    image.textureId = textureId;
    image.x = x; image.y = y; image.width = width; image.height = height;
    image.s0 = s0; image.t0 = t0; image.s1 = s1; image.t1 = t1;
    image.angleDegrees = angleDegrees;
    image.red = red; image.green = green; image.blue = blue; image.alpha = alpha;
    image.logicalWidth = logicalWidth; image.logicalHeight = logicalHeight;
    m_panelImages.push_back(image);
    return true;
}

void VulkanFrameRenderer::SetStockScissor(bool enabled, int x, int y, int width, int height,
                                           float logicalWidth, float logicalHeight)
{
    m_stockScissorEnabled = enabled;
    m_stockScissor = VkRect2D{};
    if (!enabled || !m_swapchain.width || !m_swapchain.height ||
        !(logicalWidth > 0.0f) || !(logicalHeight > 0.0f) ||
        !std::isfinite(logicalWidth) || !std::isfinite(logicalHeight) || width < 0 || height < 0)
        return;

    const double scaleX = static_cast<double>(m_swapchain.width) / logicalWidth;
    const double scaleY = static_cast<double>(m_swapchain.height) / logicalHeight;
    const double maxX = static_cast<double>(m_swapchain.width);
    const double maxY = static_cast<double>(m_swapchain.height);
    const double left = std::max(0.0, std::min(maxX, static_cast<double>(x) * scaleX));
    const double top = std::max(0.0, std::min(maxY, static_cast<double>(y) * scaleY));
    const double right = std::max(left, std::max(0.0, std::min(maxX,
        (static_cast<double>(x) + width) * scaleX)));
    const double bottom = std::max(top, std::max(0.0, std::min(maxY,
        (static_cast<double>(y) + height) * scaleY)));
    const uint32_t x0 = static_cast<uint32_t>(std::floor(left));
    const uint32_t y0 = static_cast<uint32_t>(std::floor(top));
    const uint32_t x1 = static_cast<uint32_t>(std::max(left, std::ceil(right)));
    const uint32_t y1 = static_cast<uint32_t>(std::max(top, std::ceil(bottom)));
    m_stockScissor.offset.x = static_cast<int32_t>(x0);
    m_stockScissor.offset.y = static_cast<int32_t>(y0);
    m_stockScissor.extent.width = x1 - x0;
    m_stockScissor.extent.height = y1 - y0;
}

void VulkanFrameRenderer::SetStockFog(bool enabled, float density, float start, float end,
                                      const float color[3], int mode)
{
    m_stockFogEnabled = enabled && color && std::isfinite(density) &&
        std::isfinite(start) && std::isfinite(end) && end > start;
    if (!m_stockFogEnabled)
        return;
    for (int i = 0; i < 3; ++i)
        m_stockFogColor[i] = std::isfinite(color[i]) ? color[i] : 0.0f;
    m_stockFogDensity = std::max(0.0f, density);
    m_stockFogStart = start;
    m_stockFogEnd = end;
    m_stockFogMode = mode;
}

bool VulkanFrameRenderer::RegisterLegacyRgbaTexture(int textureId, uint32_t width, uint32_t height,
                                                     const uint8_t* rgbaPixels,
                                                     bool clampU, bool clampV,
                                                     bool dynamicTexture, bool noMipmaps,
                                                     int filterMode)
{
    if (!m_initialized || !m_resources || textureId <= 0 || !width || !height || !rgbaPixels ||
        width > 4096 || height > 4096)
        return false;
    VkDeviceSize bytes = 0;
    for (uint32_t mipWidth = width, mipHeight = height;;
         mipWidth = mipWidth > 1 ? mipWidth / 2 : 1,
         mipHeight = mipHeight > 1 ? mipHeight / 2 : 1)
    {
        bytes += static_cast<VkDeviceSize>(mipWidth) * mipHeight * 4;
        if (noMipmaps || (mipWidth == 1 && mipHeight == 1))
            break;
    }
    const VkDeviceSize budget = 192ull * 1024ull * 1024ull;
    if (filterMode < VulkanFilterNearest || filterMode > VulkanFilterTrilinear)
        filterMode = VulkanFilterTrilinear;
    const auto createTextureSampler = [&](VkSampler& sampler, float maxLod) -> bool
    {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        const bool nearest = filterMode == VulkanFilterNearest;
        samplerInfo.magFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        samplerInfo.minFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = filterMode == VulkanFilterTrilinear ?
            VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = clampU ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = clampV ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.maxLod = (noMipmaps || filterMode == VulkanFilterLinear) ? 0.0f : maxLod;
        return m_createSampler(m_context->GetDevice(), &samplerInfo, nullptr, &sampler) == VK_SUCCESS;
    };
    std::map<int, LegacyTexture>::iterator existing = m_legacyTextures.find(textureId);
    if (existing != m_legacyTextures.end())
    {
        if (existing->second.texture.width != width || existing->second.texture.height != height ||
            existing->second.noMipmaps != noMipmaps)
        {
            ReleaseLegacyTexture(textureId);
            existing = m_legacyTextures.end();
        }
        else
        {
            existing->second.cpuBacked = dynamicTexture;
            if (dynamicTexture)
            {
                existing->second.rgbaPixels.assign(rgbaPixels,
                    rgbaPixels + static_cast<size_t>(width) * height * 4);
                existing->second.uploadPending = true;
            }
            else
            {
                if (!m_resources->UpdateTextureRGBA8(rgbaPixels, width, height, existing->second.texture))
                    return false;
                existing->second.rgbaPixels.clear();
                existing->second.uploadPending = false;
            }
            if (existing->second.clampU == clampU && existing->second.clampV == clampV &&
                existing->second.filterMode == filterMode)
                return true;
            VkSampler newSampler = VK_NULL_HANDLE;
            if (!createTextureSampler(newSampler,
                    static_cast<float>(existing->second.texture.mipLevels - 1))) return false;
            const VkSampler oldSampler = existing->second.sampler;
            existing->second.sampler = newSampler;
            existing->second.clampU = clampU;
            existing->second.clampV = clampV;
            existing->second.filterMode = filterMode;
            VkDescriptorImageInfo imageInfo{};
            imageInfo.sampler = newSampler;
            imageInfo.imageView = existing->second.texture.view;
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = existing->second.descriptorSet;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &imageInfo;
            m_updateDescriptorSets(m_context->GetDevice(), 1, &write, 0, nullptr);
            if (oldSampler) m_destroySampler(m_context->GetDevice(), oldSampler, nullptr);
            return true;
        }
    }
    if (bytes > budget - (m_legacyTextureBytes > budget ? budget : m_legacyTextureBytes))
        return false;
    LegacyTexture mirror;
    mirror.clampU = clampU;
    mirror.clampV = clampV;
    mirror.noMipmaps = noMipmaps;
    mirror.filterMode = filterMode;
    mirror.cpuBacked = dynamicTexture;
    if (dynamicTexture)
        mirror.rgbaPixels.assign(rgbaPixels, rgbaPixels + static_cast<size_t>(width) * height * 4);
    if (!m_resources->CreateTextureRGBA8(rgbaPixels, width, height, mirror.texture,
                                         VK_FORMAT_R8G8B8A8_UNORM, false, !noMipmaps))
        return false;
    mirror.sampler = VK_NULL_HANDLE;
    if (!createTextureSampler(mirror.sampler, static_cast<float>(mirror.texture.mipLevels - 1)))
    {
        m_resources->DestroyTexture(mirror.texture);
        return false;
    }
    VkDescriptorSetAllocateInfo descriptorInfo{};
    descriptorInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorInfo.descriptorPool = m_descriptorPool;
    descriptorInfo.descriptorSetCount = 1;
    descriptorInfo.pSetLayouts = &m_descriptorSetLayout;
    if (m_allocateDescriptorSets(m_context->GetDevice(), &descriptorInfo, &mirror.descriptorSet) != VK_SUCCESS)
    {
        m_destroySampler(m_context->GetDevice(), mirror.sampler, nullptr);
        m_resources->DestroyTexture(mirror.texture);
        return false;
    }
    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = mirror.sampler;
    imageInfo.imageView = mirror.texture.view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = mirror.descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    m_updateDescriptorSets(m_context->GetDevice(), 1, &write, 0, nullptr);
    mirror.bytes = bytes;
    m_legacyTextures[textureId] = mirror;
    m_legacyTextureBytes += bytes;
    return true;
}

bool VulkanFrameRenderer::RegisterLegacyRgbaTextureRegion(int textureId, uint32_t x, uint32_t y,
                                                            uint32_t width, uint32_t height,
                                                            const uint8_t* rgbaPixels)
{
    if (!rgbaPixels || width == 0 || height == 0)
        return false;
    std::map<int, LegacyTexture>::iterator found = m_legacyTextures.find(textureId);
    if (found == m_legacyTextures.end())
        return false;
    LegacyTexture& texture = found->second;
    if (x > texture.texture.width || y > texture.texture.height ||
        width > texture.texture.width - x || height > texture.texture.height - y ||
        !texture.cpuBacked || texture.rgbaPixels.size() != static_cast<size_t>(texture.texture.width) *
                                     texture.texture.height * 4)
        return false;
    for (uint32_t row = 0; row < height; ++row)
    {
        uint8_t* destination = texture.rgbaPixels.data() +
            (static_cast<size_t>(y + row) * texture.texture.width + x) * 4;
        const uint8_t* source = rgbaPixels + static_cast<size_t>(row) * width * 4;
        std::memcpy(destination, source, static_cast<size_t>(width) * 4);
    }
    texture.uploadPending = true;
    return true;
}

void VulkanFrameRenderer::ReleaseLegacyTexture(int textureId)
{
    std::map<int, LegacyTexture>::iterator found = m_legacyTextures.find(textureId);
    if (found == m_legacyTextures.end()) return;
    if (found->second.descriptorSet && m_freeDescriptorSets && m_descriptorPool)
        m_freeDescriptorSets(m_context->GetDevice(), m_descriptorPool, 1, &found->second.descriptorSet);
    if (found->second.sampler && m_destroySampler)
        m_destroySampler(m_context->GetDevice(), found->second.sampler, nullptr);
    if (m_resources)
        m_resources->DestroyTexture(found->second.texture);
    m_legacyTextureBytes -= found->second.bytes;
    m_legacyTextures.erase(found);
}

bool VulkanFrameRenderer::CreatePanelPipeline()
{
    VkShaderModule vertexShader = VK_NULL_HANDLE;
    VkShaderModule fragmentShader = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo shaderInfo{};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = sizeof(kVrPanelVertexSpirv);
    shaderInfo.pCode = kVrPanelVertexSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr, &vertexShader) != VK_SUCCESS)
    {
        SetError("failed to create VR panel vertex shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrPanelFragmentSpirv);
    shaderInfo.pCode = kVrPanelFragmentSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr, &fragmentShader) != VK_SUCCESS)
    {
        m_destroyShaderModule(m_context->GetDevice(), vertexShader, nullptr);
        SetError("failed to create VR panel fragment shader module");
        return false;
    }

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkDescriptorSetLayoutBinding textureBinding{};
    textureBinding.binding = 0;
    textureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    textureBinding.descriptorCount = 1;
    textureBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo{};
    descriptorLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorLayoutInfo.bindingCount = 1;
    descriptorLayoutInfo.pBindings = &textureBinding;
    if (m_createDescriptorSetLayout(m_context->GetDevice(), &descriptorLayoutInfo, nullptr,
                                    &m_descriptorSetLayout) != VK_SUCCESS)
    {
        m_destroyShaderModule(m_context->GetDevice(), fragmentShader, nullptr);
        m_destroyShaderModule(m_context->GetDevice(), vertexShader, nullptr);
        SetError("vkCreateDescriptorSetLayout failed");
        return false;
    }
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_descriptorSetLayout;
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(float) * 32;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    const VkResult layoutResult = m_createPipelineLayout(m_context->GetDevice(), &layoutInfo, nullptr, &m_pipelineLayout);
    if (layoutResult != VK_SUCCESS)
    {
        m_destroyShaderModule(m_context->GetDevice(), fragmentShader, nullptr);
        m_destroyShaderModule(m_context->GetDevice(), vertexShader, nullptr);
        m_destroyDescriptorSetLayout(m_context->GetDevice(), m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
        SetError("vkCreatePipelineLayout failed");
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 512;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 512;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (m_createDescriptorPool(m_context->GetDevice(), &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
    {
        m_destroyShaderModule(m_context->GetDevice(), fragmentShader, nullptr);
        m_destroyShaderModule(m_context->GetDevice(), vertexShader, nullptr);
        SetError("vkCreateDescriptorPool failed");
        return false;
    }
    VkDescriptorSetAllocateInfo descriptorAlloc{};
    descriptorAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorAlloc.descriptorPool = m_descriptorPool;
    descriptorAlloc.descriptorSetCount = 1;
    descriptorAlloc.pSetLayouts = &m_descriptorSetLayout;
    if (m_allocateDescriptorSets(m_context->GetDevice(), &descriptorAlloc, &m_descriptorSet) != VK_SUCCESS)
    {
        m_destroyShaderModule(m_context->GetDevice(), fragmentShader, nullptr);
        m_destroyShaderModule(m_context->GetDevice(), vertexShader, nullptr);
        SetError("vkAllocateDescriptorSets failed");
        return false;
    }
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    if (m_createSampler(m_context->GetDevice(), &samplerInfo, nullptr, &m_gameSampler) != VK_SUCCESS)
    {
        m_destroyShaderModule(m_context->GetDevice(), fragmentShader, nullptr);
        m_destroyShaderModule(m_context->GetDevice(), vertexShader, nullptr);
        SetError("vkCreateSampler failed");
        return false;
    }
    if (!m_pipelineFactory.Initialize(*m_context))
    {
        m_destroyShaderModule(m_context->GetDevice(), fragmentShader, nullptr);
        m_destroyShaderModule(m_context->GetDevice(), vertexShader, nullptr);
        SetError(m_pipelineFactory.GetLastError());
        return false;
    }
    VulkanGraphicsPipelineDesc panelPipeline{};
    panelPipeline.renderPass = m_renderPass;
    panelPipeline.layout = m_pipelineLayout;
    panelPipeline.vertexShader = vertexShader;
    panelPipeline.fragmentShader = fragmentShader;
    panelPipeline.useVertexInput = false;
    panelPipeline.dynamicViewport = false;
    panelPipeline.viewportExtent.width = m_swapchain.width;
    panelPipeline.viewportExtent.height = m_swapchain.height;
    panelPipeline.renderState = 0x00020000u; // GS_NODEPTHTEST
    panelPipeline.cullMode = 0; // R_CULL_NONE
    const bool opaquePipelineCreated = m_pipelineFactory.CreateGraphicsPipeline(panelPipeline, m_panelPipeline);
    panelPipeline.renderState = 0x00020000u | 0x65u; // no depth test, SRCALPHA/INVSRCALPHA
    const bool blendPipelineCreated = opaquePipelineCreated &&
        m_pipelineFactory.CreateGraphicsPipeline(panelPipeline, m_panelBlendPipeline);
    m_destroyShaderModule(m_context->GetDevice(), fragmentShader, nullptr);
    m_destroyShaderModule(m_context->GetDevice(), vertexShader, nullptr);
    if (!opaquePipelineCreated || !blendPipelineCreated)
    {
        SetError(m_pipelineFactory.GetLastError());
        return false;
    }
    return true;
}

bool VulkanFrameRenderer::SetGameFrameRGBA(const uint8_t* pixels, uint32_t width, uint32_t height)
{
    if (!m_initialized || !m_resources || !pixels || width == 0 || height == 0)
        return false;
    bool uploaded = false;
    bool descriptorChanged = false;
    if (!m_gameTexture.image)
    {
        uploaded = m_resources->CreateTextureRGBA8(pixels, width, height, m_gameTexture,
                                                    VK_FORMAT_R8G8B8A8_UNORM, true);
        descriptorChanged = uploaded;
    }
    else if (m_gameTexture.width != width || m_gameTexture.height != height)
    {
        uploaded = m_resources->CreateTextureRGBA8(pixels, width, height, m_gameTexture,
                                                    VK_FORMAT_R8G8B8A8_UNORM, true);
        descriptorChanged = uploaded;
    }
    else
    {
        uploaded = m_resources->PrepareTextureRGBA8Update(pixels, width, height, m_gameTexture);
        m_gameTextureUploadPending = uploaded;
    }
    if (!uploaded)
    {
        SetError(m_resources->GetLastError());
        return false;
    }
    if (descriptorChanged)
        m_gameTextureUploadPending = false;
    else
        return true;
    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = m_gameSampler;
    imageInfo.imageView = m_gameTexture.view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = m_descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    m_updateDescriptorSets(m_context->GetDevice(), 1, &write, 0, nullptr);
    return true;
}

bool VulkanFrameRenderer::CreateRenderTargets()
{
    m_targets.resize(m_swapchain.images.size());
    for (size_t imageIndex = 0; imageIndex < m_targets.size(); ++imageIndex)
    {
        Target& target = m_targets[imageIndex];
        target.image = reinterpret_cast<VkImage>(m_swapchain.images[imageIndex].image);
        for (uint32_t viewIndex = 0; viewIndex < m_viewCount; ++viewIndex)
        {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = target.image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = m_format;
            viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = viewIndex;
            if (m_createImageView(m_context->GetDevice(), &viewInfo, nullptr, &target.views[viewIndex]) != VK_SUCCESS)
            {
                SetError("vkCreateImageView failed");
                return false;
            }

            if (!m_resources || !m_resources->CreateDepthImage(m_swapchain.width, m_swapchain.height,
                                                                 m_depthFormat, target.depth[viewIndex]))
            {
                SetError(m_resources ? m_resources->GetLastError() : "Vulkan resource manager is unavailable for depth targets");
                return false;
            }
            VkImageView framebufferAttachments[2] = { target.views[viewIndex], target.depth[viewIndex].view };

            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = m_renderPass;
            framebufferInfo.attachmentCount = 2;
            framebufferInfo.pAttachments = framebufferAttachments;
            framebufferInfo.width = m_swapchain.width;
            framebufferInfo.height = m_swapchain.height;
            framebufferInfo.layers = 1;
            if (m_createFramebuffer(m_context->GetDevice(), &framebufferInfo, nullptr,
                                    &target.framebuffers[viewIndex]) != VK_SUCCESS)
            {
                SetError("vkCreateFramebuffer failed");
                return false;
            }
        }
    }

    m_commandBuffers.resize(m_targets.size() * m_viewCount);
    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = m_commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = static_cast<uint32_t>(m_commandBuffers.size());
    if (m_allocateCommandBuffers(m_context->GetDevice(), &allocateInfo, m_commandBuffers.data()) != VK_SUCCESS)
    {
        SetError("vkAllocateCommandBuffers failed");
        return false;
    }
    return true;
}

void VulkanFrameRenderer::SetStockClearColor(float red, float green, float blue)
{
    m_stockClearColor[0] = red;
    m_stockClearColor[1] = green;
    m_stockClearColor[2] = blue;
}

bool VulkanFrameRenderer::QueueStockClearDepth()
{
    if (!m_frameActive || !m_frame.shouldRender || !m_frame.viewsValid)
        return false;
    StockDraw clear;
    clear.clearDepth = true;
    clear.scissorEnabled = m_stockScissorEnabled;
    clear.scissor = m_stockScissor;
    m_stockDraws.push_back(clear);
    return true;
}

bool VulkanFrameRenderer::QueueStockClearStencil()
{
    if (!m_frameActive || !m_frame.shouldRender || !m_frame.viewsValid)
        return false;
    const bool hasStencil = m_depthFormat == VK_FORMAT_D24_UNORM_S8_UINT ||
                            m_depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT;
    if (!hasStencil)
        return true;
    StockDraw clear;
    clear.clearStencil = true;
    clear.scissorEnabled = m_stockScissorEnabled;
    clear.scissor = m_stockScissor;
    m_stockDraws.push_back(clear);
    return true;
}

bool VulkanFrameRenderer::BeginFrame()
{
    if (!m_initialized || m_frameActive)
        return false;
    if (m_frameSubmissionPending)
    {
        if (m_waitForFences(m_context->GetDevice(), 1, &m_frameFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        {
            SetError("waiting for the previous Vulkan frame before recycling dynamic buffers failed");
            return false;
        }
        m_frameSubmissionPending = false;
    }
    for (size_t i = 0; i < m_previousDynamicVertexBuffers.size(); ++i)
        m_resources->DestroyBuffer(m_previousDynamicVertexBuffers[i]);
    m_previousDynamicVertexBuffers.clear();
    for (size_t i = 0; i < m_previousDynamicIndexBuffers.size(); ++i)
        m_resources->DestroyBuffer(m_previousDynamicIndexBuffers[i]);
    m_previousDynamicIndexBuffers.clear();
    if (!m_runtime->BeginFrame(m_frame))
        return false;
    m_frameActive = true;
    m_stockDraws.clear();
    m_panelImages.clear();
    m_stockScissorEnabled = false;
    m_stockScissor = VkRect2D{};
    m_reusableClientGeometry.valid = false;
    m_untranslatedDrawCount = 0;
    m_dynamicVertexUsed = 0;
    m_dynamicIndexUsed = 0;
    if (m_frame.viewsValid && m_frame.viewCount >= 2 && !m_referenceHeadPoseValid)
    {
        m_referenceHeadPose = m_frame.views[0].pose;
        m_referenceHeadPose.position.x = 0.5f * (m_frame.views[0].pose.position.x + m_frame.views[1].pose.position.x);
        m_referenceHeadPose.position.y = 0.5f * (m_frame.views[0].pose.position.y + m_frame.views[1].pose.position.y);
        m_referenceHeadPose.position.z = 0.5f * (m_frame.views[0].pose.position.z + m_frame.views[1].pose.position.z);
        m_referenceHeadPoseValid = true;
    }
    if (!m_frame.shouldRender || m_frame.viewCount == 0)
        return true;
    if (!m_runtime->AcquireSwapchainImage(m_swapchain, m_imageIndex))
    {
        m_runtime->EndFrame(nullptr, 0);
        m_frameActive = false;
        return false;
    }
    m_imageAcquired = true;
    if (!m_runtime->WaitSwapchainImage(m_swapchain))
    {
        m_runtime->ReleaseSwapchainImage(m_swapchain);
        m_runtime->EndFrame(nullptr, 0);
        m_imageAcquired = false;
        m_frameActive = false;
        return false;
    }
    return true;
}

bool VulkanFrameRenderer::ShouldCaptureGameFrame() const
{
    // CVulkanRenderer derives from CNULLRenderer; its ReadFrameBuffer is a
    // no-op. Never capture it as if it were a valid legacy game backbuffer.
    return false;
}

bool VulkanFrameRenderer::RecordAndSubmit(uint32_t viewIndex)
{
    VkCommandBuffer commandBuffer = m_commandBuffers[m_imageIndex * m_viewCount + viewIndex];
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (m_beginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
        return false;

    if (viewIndex == 0 && m_gameTextureUploadPending &&
        !m_resources->RecordTextureRGBA8Update(commandBuffer, m_gameTexture))
    {
        SetError("failed to record deferred VR panel texture upload");
        return false;
    }
    if (viewIndex == 0)
    {
        for (std::map<int, LegacyTexture>::const_iterator it = m_legacyTextures.begin();
             it != m_legacyTextures.end(); ++it)
        {
            if (it->second.uploadPending &&
                !m_resources->RecordTextureRGBA8Update(commandBuffer, it->second.texture))
            {
                SetError(m_resources->GetLastError());
                return false;
            }
        }
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = m_targets[m_imageIndex].image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = viewIndex;
    barrier.subresourceRange.layerCount = 1;
    m_cmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkClearValue clear{};
    clear.color.float32[0] = m_stockClearColor[0];
    clear.color.float32[1] = m_stockClearColor[1];
    clear.color.float32[2] = m_stockClearColor[2];
    clear.color.float32[3] = 1.0f;
    VkClearValue clearValues[2]{};
    clearValues[0] = clear;
    clearValues[1].depthStencil.depth = 1.0f;
    clearValues[1].depthStencil.stencil = 0;
    VkRenderPassBeginInfo renderPassBegin{};
    renderPassBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassBegin.renderPass = m_renderPass;
    renderPassBegin.framebuffer = m_targets[m_imageIndex].framebuffers[viewIndex];
    renderPassBegin.renderArea.extent.width = m_swapchain.width;
    renderPassBegin.renderArea.extent.height = m_swapchain.height;
    renderPassBegin.clearValueCount = 2;
    renderPassBegin.pClearValues = clearValues;
    m_cmdBeginRenderPass(commandBuffer, &renderPassBegin, VK_SUBPASS_CONTENTS_INLINE);
    if (m_frame.viewsValid && m_gameTexture.image && m_descriptorSet)
    {
            float panelConstants[24] = {};
            float* mvp = panelConstants;
            panelConstants[16] = panelConstants[17] = 1.0f;
            panelConstants[20] = panelConstants[21] = panelConstants[22] = panelConstants[23] = 1.0f;
        const float aspect = static_cast<float>(m_gameTexture.width) /
                             static_cast<float>(m_gameTexture.height);
        if (BuildFlatPanelMvp(m_frame.views[0], m_frame.views[1], viewIndex,
                              aspect, 2.5f, mvp))
        {
            m_cmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_panelPipeline);
            m_cmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout,
                                    0, 1, &m_descriptorSet, 0, nullptr);
            m_cmdPushConstants(commandBuffer, m_pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(panelConstants), panelConstants);
            m_cmdDraw(commandBuffer, 6, 1, 0, 0);
        }
    }
    if (m_referenceHeadPoseValid && !m_stockDraws.empty())
    {
        VkViewport viewport{};
        viewport.width = static_cast<float>(m_swapchain.width);
        viewport.height = static_cast<float>(m_swapchain.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.extent.width = m_swapchain.width;
        scissor.extent.height = m_swapchain.height;
        m_cmdSetViewport(commandBuffer, 0, 1, &viewport);
        m_cmdSetScissor(commandBuffer, 0, 1, &scissor);
        for (size_t i = 0; i < m_stockDraws.size(); ++i)
        {
            const StockDraw& draw = m_stockDraws[i];
            if (draw.clearDepth || draw.clearStencil)
            {
                if (draw.scissorEnabled &&
                    (draw.scissor.extent.width == 0 || draw.scissor.extent.height == 0))
                    continue;
                VkClearAttachment attachment{};
                attachment.aspectMask = draw.clearDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
                attachment.clearValue.depthStencil.depth = 1.0f;
                attachment.clearValue.depthStencil.stencil = 0;
                VkClearRect rect{};
                rect.rect = draw.scissorEnabled ? draw.scissor : scissor;
                rect.baseArrayLayer = 0;
                rect.layerCount = 1;
                m_cmdClearAttachments(commandBuffer, 1, &attachment, 1, &rect);
                continue;
            }
            if (draw.scissorEnabled &&
                (draw.scissor.extent.width == 0 || draw.scissor.extent.height == 0))
                continue;
            const VkRect2D drawScissor = draw.scissorEnabled ? draw.scissor : scissor;
            m_cmdSetScissor(commandBuffer, 0, 1, &drawScissor);
            VkPipeline pipeline = draw.pipeline;
            VkDescriptorSet textureSets[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
            if (draw.textureId)
            {
                std::map<int, LegacyTexture>::const_iterator texture = m_legacyTextures.find(draw.textureId);
                if (texture == m_legacyTextures.end()) continue;
                textureSets[0] = texture->second.descriptorSet;
                textureSets[1] = textureSets[0];
            }
            if (draw.useSecondTexture || draw.useNormalMap)
            {
                const int secondaryTextureId = draw.useNormalMap ? draw.normalMapTextureId : draw.textureId1;
                std::map<int, LegacyTexture>::const_iterator texture = m_legacyTextures.find(secondaryTextureId);
                if (texture == m_legacyTextures.end()) continue;
                textureSets[1] = texture->second.descriptorSet;
            }
            float pushConstants[32];
            float* mvp = pushConstants;
            float* eyeModelView = pushConstants + 16;
            if (!pipeline || !BuildOpenXrEyeMvp(m_frame.views[viewIndex], m_referenceHeadPose,
                                                 draw.modelView, 0.05f, 1000.0f, mvp, eyeModelView))
                continue;
            if (draw.textureId)
            {
                if (HasVulkanVertexNormal(draw.vertexFormat))
                {
                    const float* matrix = draw.textureMatrix0;
                    eyeModelView[0] = matrix[0]; eyeModelView[1] = matrix[4];
                    eyeModelView[2] = matrix[12]; eyeModelView[3] = 0.0f;
                    eyeModelView[4] = matrix[1]; eyeModelView[5] = matrix[5];
                    eyeModelView[6] = matrix[13]; eyeModelView[7] = 0.0f;
                    for (int i = 0; i < 8; ++i)
                        eyeModelView[8 + i] = draw.materialLighting[i];
                }
                else
                {
                    const float* matrices[2] = { draw.textureMatrix0, draw.textureMatrix1 };
                    for (int matrixIndex = 0; matrixIndex < 2; ++matrixIndex)
                    {
                        const float* matrix = matrices[matrixIndex];
                        float* rows = eyeModelView + matrixIndex * 8;
                        rows[0] = matrix[0]; rows[1] = matrix[4]; rows[2] = matrix[12]; rows[3] = 0.0f;
                        rows[4] = matrix[1]; rows[5] = matrix[5]; rows[6] = matrix[13]; rows[7] = 0.0f;
                    }
                }
            }
            else if (HasVulkanVertexNormal(draw.vertexFormat))
            {
                // The untextured lighting shaders use only mat3(modelView) for
                // normal transformation. Pack material values into the unused
                // affine row/translation slots of this same 4x4 push constant.
                eyeModelView[3] = draw.materialLighting[0];
                eyeModelView[7] = draw.materialLighting[1];
                eyeModelView[11] = draw.materialLighting[2];
                eyeModelView[12] = draw.materialLighting[4];
                eyeModelView[13] = draw.materialLighting[5];
                eyeModelView[14] = draw.materialLighting[6];
                eyeModelView[15] = draw.materialLighting[3];
            }
            m_cmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            if (draw.stencilState)
            {
                m_cmdSetStencilCompareMask(commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, draw.stencilMask);
                m_cmdSetStencilWriteMask(commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, 0xffffffffu);
                m_cmdSetStencilReference(commandBuffer, VK_STENCIL_FACE_FRONT_AND_BACK, draw.stencilRef);
            }
            if (textureSets[0])
                m_cmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_scenePipelineLayout, 0,
                                        (draw.useSecondTexture || draw.useNormalMap) ? 2u : 1u,
                                        textureSets, 0, nullptr);
            m_cmdPushConstants(commandBuffer, m_scenePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(pushConstants), pushConstants);
            const float fogPushConstants[16] = {
                draw.fogColor[0], draw.fogColor[1], draw.fogColor[2], 1.0f,
                draw.fogEnabled ? 1.0f : 0.0f, static_cast<float>(draw.fogMode),
                draw.fogDensity, draw.fogStart,
                draw.fogEnd, 0.05f, 1000.0f, 0.0f,
                draw.globalOpacity, draw.alphaTestRef, draw.additiveMaterial ? 1.0f : 0.0f, 0.0f
            };
            m_cmdPushConstants(commandBuffer, m_scenePipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(fogPushConstants), fogPushConstants);
            const VkDeviceSize offsets[2] = { 0, 0 };
            if (draw.tangentBuffer)
            {
                const VkBuffer vertexBuffers[2] = { draw.vertexBuffer, draw.tangentBuffer };
                m_cmdBindVertexBuffers(commandBuffer, 0, 2, vertexBuffers, offsets);
            }
            else
                m_cmdBindVertexBuffers(commandBuffer, 0, 1, &draw.vertexBuffer, offsets);
            m_cmdBindIndexBuffer(commandBuffer, draw.indexBuffer, 0, VK_INDEX_TYPE_UINT16);
            m_cmdDrawIndexed(commandBuffer, draw.indexCount, 1, draw.firstIndex, draw.vertexOffset, 0);
        }
    }
    if (m_frame.viewsValid && m_referenceHeadPoseValid && !m_panelImages.empty())
    {
        VkViewport uiViewport{};
        uiViewport.width = static_cast<float>(m_swapchain.width);
        uiViewport.height = static_cast<float>(m_swapchain.height);
        uiViewport.minDepth = 0.0f;
        uiViewport.maxDepth = 1.0f;
        VkRect2D uiScissor{};
        uiScissor.extent.width = m_swapchain.width;
        uiScissor.extent.height = m_swapchain.height;
        m_cmdSetViewport(commandBuffer, 0, 1, &uiViewport);
        m_cmdSetScissor(commandBuffer, 0, 1, &uiScissor);
        const float panelAspect = static_cast<float>(m_swapchain.width) /
                                  static_cast<float>(m_swapchain.height);
        float baseMvp[16];
        if (BuildFlatPanelMvp(m_frame.views[0], m_frame.views[1], viewIndex,
                              panelAspect, 2.5f, baseMvp))
        {
            for (size_t i = 0; i < m_panelImages.size(); ++i)
            {
                const PanelImage& image = m_panelImages[i];
                std::map<int, LegacyTexture>::const_iterator texture = m_legacyTextures.find(image.textureId);
                if (texture == m_legacyTextures.end() || !texture->second.descriptorSet)
                    continue;
                const float radians = image.angleDegrees * 0.01745329251994329577f;
                const float c = std::cos(radians), s = std::sin(radians);
                const float sx = image.width / image.logicalWidth;
                const float sy = image.height / image.logicalHeight;
                const float cx = 2.0f * (image.x + image.width * 0.5f) / image.logicalWidth - 1.0f;
                const float cy = 1.0f - 2.0f * (image.y + image.height * 0.5f) / image.logicalHeight;
                float local[16] = {};
                local[0] = c * sx; local[1] = s * sx;
                local[4] = -s * sy; local[5] = c * sy;
                local[10] = 1.0f; local[12] = cx; local[13] = cy; local[15] = 1.0f;
                float constants[24] = {};
                for (int column = 0; column < 4; ++column)
                    for (int row = 0; row < 4; ++row)
                        for (int k = 0; k < 4; ++k)
                            constants[column * 4 + row] += baseMvp[k * 4 + row] * local[column * 4 + k];
                constants[16] = image.s1 - image.s0;
                constants[17] = image.t1 - image.t0;
                constants[18] = image.s0;
                constants[19] = 1.0f - image.t1;
                constants[20] = image.red; constants[21] = image.green;
                constants[22] = image.blue; constants[23] = image.alpha;
                m_cmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_panelBlendPipeline);
                m_cmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_pipelineLayout, 0, 1, &texture->second.descriptorSet, 0, nullptr);
                m_cmdPushConstants(commandBuffer, m_pipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(constants), constants);
                m_cmdDraw(commandBuffer, 6, 1, 0, 0);
            }
        }
    }
    m_cmdEndRenderPass(commandBuffer);
    return m_endCommandBuffer(commandBuffer) == VK_SUCCESS;
}

bool VulkanFrameRenderer::EndFrame()
{
    if (!m_frameActive)
        return false;
    bool result = true;
    if (m_frame.shouldRender && m_imageAcquired)
    {
        for (std::map<int, LegacyTexture>::iterator it = m_legacyTextures.begin();
             it != m_legacyTextures.end(); ++it)
        {
            if (!it->second.uploadPending)
                continue;
            if (!m_resources->PrepareTextureRGBA8Update(it->second.rgbaPixels.data(),
                    it->second.texture.width, it->second.texture.height, it->second.texture))
                result = false;
        }
        const uint32_t renderViewCount = m_frame.viewCount < m_viewCount ? m_frame.viewCount : m_viewCount;
        VkCommandBuffer frameCommands[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
        uint32_t commandCount = 0;
        bool firstViewRecorded = false;
        for (uint32_t viewIndex = 0; viewIndex < renderViewCount; ++viewIndex)
        {
            if (!RecordAndSubmit(viewIndex))
                result = false;
            else
            {
                frameCommands[commandCount++] = m_commandBuffers[m_imageIndex * m_viewCount + viewIndex];
                if (viewIndex == 0)
                    firstViewRecorded = true;
            }
        }
        if (commandCount)
        {
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = commandCount;
            submit.pCommandBuffers = frameCommands;
            bool queueCompleted = false;
            const bool fenceReset = m_resetFences(m_context->GetDevice(), 1, &m_frameFence) == VK_SUCCESS;
            if (!fenceReset || m_queueSubmit(m_context->GetGraphicsQueue(), 1, &submit, m_frameFence) != VK_SUCCESS)
                result = false;
            else
            {
                m_frameSubmissionPending = true;
                m_gameTextureUploadPending = false;
                // OpenXR requires submitted work using its swapchain image to
                // finish before release. Wait only for this frame's submission,
                // not every operation sharing the runtime graphics queue.
                if (m_waitForFences(m_context->GetDevice(), 1, &m_frameFence, VK_TRUE,
                                    UINT64_MAX) == VK_SUCCESS)
                {
                    queueCompleted = true;
                    m_frameSubmissionPending = false;
                }
                else
                    result = false;
            }
            if (queueCompleted && firstViewRecorded)
                for (std::map<int, LegacyTexture>::iterator it = m_legacyTextures.begin();
                     it != m_legacyTextures.end(); ++it)
                    it->second.uploadPending = false;
            if (!queueCompleted)
                result = false;
        }
        if (commandCount != renderViewCount)
            result = false;
        if (!m_runtime->ReleaseSwapchainImage(m_swapchain))
            result = false;

        if (!m_frame.viewsValid || m_frame.viewCount < 2)
        {
            result = m_runtime->EndFrame(nullptr, 0) && result;
            m_imageAcquired = false;
            m_frameActive = false;
            return result;
        }

        if (!result)
        {
            result = m_runtime->EndFrame(nullptr, 0) && result;
            m_imageAcquired = false;
            m_frameActive = false;
            return result;
        }

        XrCompositionLayerProjectionView projectionViews[2]{};
        const uint32_t viewCount = m_frame.viewCount < 2 ? m_frame.viewCount : 2;
        for (uint32_t viewIndex = 0; viewIndex < viewCount; ++viewIndex)
        {
            projectionViews[viewIndex].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            projectionViews[viewIndex].pose = m_frame.views[viewIndex].pose;
            projectionViews[viewIndex].fov = m_frame.views[viewIndex].fov;
            projectionViews[viewIndex].subImage.swapchain = m_swapchain.handle;
            projectionViews[viewIndex].subImage.imageRect.extent.width = static_cast<int32_t>(m_swapchain.width);
            projectionViews[viewIndex].subImage.imageRect.extent.height = static_cast<int32_t>(m_swapchain.height);
            projectionViews[viewIndex].subImage.imageArrayIndex = viewIndex;
        }
        XrCompositionLayerProjection projection{};
        projection.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        projection.space = m_runtime->GetStageSpace();
        projection.viewCount = viewCount;
        projection.views = projectionViews;
        const XrCompositionLayerBaseHeader* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        result = m_runtime->EndFrame(&layer, 1) && result;
    }
    else
    {
        result = m_runtime->EndFrame(nullptr, 0);
    }
    m_imageAcquired = false;
    m_frameActive = false;
    return result;
}

void VulkanFrameRenderer::Shutdown()
{
    if (m_context && m_context->IsInitialized())
    {
        if (m_frameSubmissionPending && m_frameFence && m_waitForFences)
            m_waitForFences(m_context->GetDevice(), 1, &m_frameFence, VK_TRUE, UINT64_MAX);
        if (m_commandBuffers.size() && m_freeCommandBuffers && m_commandPool)
            m_freeCommandBuffers(m_context->GetDevice(), m_commandPool,
                                 static_cast<uint32_t>(m_commandBuffers.size()), m_commandBuffers.data());
        if (m_frameFence && m_destroyFence)
            m_destroyFence(m_context->GetDevice(), m_frameFence, nullptr);
        for (Target& target : m_targets)
        {
            for (uint32_t viewIndex = 0; viewIndex < m_viewCount; ++viewIndex)
            {
                if (target.framebuffers[viewIndex] && m_destroyFramebuffer)
                    m_destroyFramebuffer(m_context->GetDevice(), target.framebuffers[viewIndex], nullptr);
                if (target.views[viewIndex] && m_destroyImageView)
                    m_destroyImageView(m_context->GetDevice(), target.views[viewIndex], nullptr);
                if (m_resources && target.depth[viewIndex].image)
                    m_resources->DestroyTexture(target.depth[viewIndex]);
            }
        }
        if (m_panelPipeline && m_destroyPipeline)
            m_destroyPipeline(m_context->GetDevice(), m_panelPipeline, nullptr);
        if (m_panelBlendPipeline && m_destroyPipeline)
            m_destroyPipeline(m_context->GetDevice(), m_panelBlendPipeline, nullptr);
        for (std::map<std::array<uint32_t, 13>, VkPipeline>::iterator it = m_scenePipelineCache.begin();
             it != m_scenePipelineCache.end(); ++it)
            if (it->second && m_destroyPipeline)
                m_destroyPipeline(m_context->GetDevice(), it->second, nullptr);
        if (m_resources)
            for (std::map<int, LegacyTexture>::iterator it = m_legacyTextures.begin();
                 it != m_legacyTextures.end(); ++it)
            {
                if (it->second.sampler && m_destroySampler)
                    m_destroySampler(m_context->GetDevice(), it->second.sampler, nullptr);
                if (it->second.texture.image)
                    m_resources->DestroyTexture(it->second.texture);
            }
        if (m_resources)
        {
            m_resources->DestroyBuffer(m_dynamicVertexBuffer);
            m_resources->DestroyBuffer(m_dynamicIndexBuffer);
            for (size_t i = 0; i < m_previousDynamicVertexBuffers.size(); ++i)
                m_resources->DestroyBuffer(m_previousDynamicVertexBuffers[i]);
            for (size_t i = 0; i < m_previousDynamicIndexBuffers.size(); ++i)
                m_resources->DestroyBuffer(m_previousDynamicIndexBuffers[i]);
        }
        if (m_scenePipelineLayout && m_destroyPipelineLayout)
            m_destroyPipelineLayout(m_context->GetDevice(), m_scenePipelineLayout, nullptr);
        if (m_sceneVertexShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneVertexShader, nullptr);
        if (m_sceneFragmentShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneFragmentShader, nullptr);
        if (m_sceneColorVertexShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneColorVertexShader, nullptr);
        if (m_sceneColorFragmentShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneColorFragmentShader, nullptr);
        if (m_sceneLitVertexShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneLitVertexShader, nullptr);
        if (m_sceneLitColorVertexShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneLitColorVertexShader, nullptr);
        if (m_sceneLitFragmentShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneLitFragmentShader, nullptr);
        if (m_sceneTextureVertexShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneTextureVertexShader, nullptr);
        if (m_sceneTextureFragmentShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneTextureFragmentShader, nullptr);
        if (m_sceneTextureColorVertexShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneTextureColorVertexShader, nullptr);
        if (m_sceneTextureColorFragmentShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneTextureColorFragmentShader, nullptr);
        if (m_sceneMultiTextureColorVertexShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneMultiTextureColorVertexShader, nullptr);
        if (m_sceneMultiTextureColorFragmentShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneMultiTextureColorFragmentShader, nullptr);
        if (m_sceneTextureLitVertexShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneTextureLitVertexShader, nullptr);
        if (m_sceneTextureLitColorVertexShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneTextureLitColorVertexShader, nullptr);
        if (m_sceneTextureBumpVertexShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneTextureBumpVertexShader, nullptr);
        if (m_sceneTextureBumpColorVertexShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneTextureBumpColorVertexShader, nullptr);
        if (m_sceneTextureBumpFragmentShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneTextureBumpFragmentShader, nullptr);
        if (m_pipelineLayout && m_destroyPipelineLayout)
            m_destroyPipelineLayout(m_context->GetDevice(), m_pipelineLayout, nullptr);
        if (m_gameSampler && m_destroySampler)
            m_destroySampler(m_context->GetDevice(), m_gameSampler, nullptr);
        if (m_descriptorPool && m_destroyDescriptorPool)
            m_destroyDescriptorPool(m_context->GetDevice(), m_descriptorPool, nullptr);
        if (m_descriptorSetLayout && m_destroyDescriptorSetLayout)
            m_destroyDescriptorSetLayout(m_context->GetDevice(), m_descriptorSetLayout, nullptr);
        if (m_renderPass && m_destroyRenderPass)
            m_destroyRenderPass(m_context->GetDevice(), m_renderPass, nullptr);
        if (m_commandPool && m_destroyCommandPool)
            m_destroyCommandPool(m_context->GetDevice(), m_commandPool, nullptr);
    }
    if (m_resources && m_gameTexture.image)
        m_resources->DestroyTexture(m_gameTexture);
    if (m_runtime && m_swapchain.handle)
        m_runtime->DestroyVulkanSwapchain(m_swapchain);
    m_commandBuffers.clear();
    m_targets.clear();
    m_renderPass = VK_NULL_HANDLE;
    m_panelPipeline = VK_NULL_HANDLE;
    m_panelBlendPipeline = VK_NULL_HANDLE;
    m_scenePipelineLayout = VK_NULL_HANDLE;
    m_sceneVertexShader = VK_NULL_HANDLE;
    m_sceneFragmentShader = VK_NULL_HANDLE;
    m_sceneColorVertexShader = VK_NULL_HANDLE;
    m_sceneColorFragmentShader = VK_NULL_HANDLE;
    m_sceneLitVertexShader = VK_NULL_HANDLE;
    m_sceneLitColorVertexShader = VK_NULL_HANDLE;
    m_sceneLitFragmentShader = VK_NULL_HANDLE;
    m_sceneTextureVertexShader = VK_NULL_HANDLE;
    m_sceneTextureFragmentShader = VK_NULL_HANDLE;
    m_sceneTextureColorVertexShader = VK_NULL_HANDLE;
    m_sceneTextureColorFragmentShader = VK_NULL_HANDLE;
    m_sceneMultiTextureColorVertexShader = VK_NULL_HANDLE;
    m_sceneMultiTextureColorFragmentShader = VK_NULL_HANDLE;
    m_sceneTextureLitVertexShader = VK_NULL_HANDLE;
    m_sceneTextureLitColorVertexShader = VK_NULL_HANDLE;
    m_sceneTextureBumpVertexShader = VK_NULL_HANDLE;
    m_sceneTextureBumpColorVertexShader = VK_NULL_HANDLE;
    m_sceneTextureBumpFragmentShader = VK_NULL_HANDLE;
    m_scenePipelineCache.clear();
    m_legacyTextures.clear();
    m_legacyTextureBytes = 0;
    m_pipelineLayout = VK_NULL_HANDLE;
    m_descriptorSetLayout = VK_NULL_HANDLE;
    m_descriptorPool = VK_NULL_HANDLE;
    m_descriptorSet = VK_NULL_HANDLE;
    m_gameSampler = VK_NULL_HANDLE;
    m_gameTextureUploadPending = false;
    m_dynamicVertexUsed = 0;
    m_dynamicIndexUsed = 0;
    m_previousDynamicVertexBuffers.clear();
    m_previousDynamicIndexBuffers.clear();
    m_frameSubmissionPending = false;
    m_commandPool = VK_NULL_HANDLE;
    m_frameFence = VK_NULL_HANDLE;
    m_depthFormat = VK_FORMAT_UNDEFINED;
    m_runtime = nullptr;
    m_context = nullptr;
    m_resources = nullptr;
    m_pipelineFactory.Shutdown();
    m_initialized = false;
    m_frameActive = false;
    m_imageAcquired = false;
    m_referenceHeadPoseValid = false;
    m_stockDraws.clear();
    m_panelImages.clear();
}
}
