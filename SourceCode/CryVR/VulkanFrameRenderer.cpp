#include "VulkanFrameRenderer.h"
#include "VulkanPanelShaders.h"
#include "VulkanSceneShaders.h"
#include "VulkanLitSceneShaders.h"
#include "VulkanMultiTextureLitShaders.h"
#include "VulkanFrameCapture.h"
#include "ISystem.h"

#include <vector>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>
#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace CryVR
{
namespace
{
#if defined(__ANDROID__)
uint64_t GetAuditThreadId()
{
    return static_cast<uint64_t>(syscall(SYS_gettid));
}
#else
uint64_t GetAuditThreadId()
{
    return 0;
}
#endif

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
    // Match CGLRenderer::EF_SetColorOp rather than the broader D3D9 combiner:
    // OpenGL implements only the cases below. Other operations either fall
    // through to GL's default MODULATE or are explicit no-ops; explicit no-ops
    // preserve the previous stage mode and are resolved by the queue state.
    switch (legacyOperation)
    {
    case -1: case 255:
        mode = 0xffffffffu; return true; // 255 means OpenGL leaves the stage unchanged
    case 14: case 15:
        mode = 0xffffffffu; return true; // MULTIPLYADD / BUMPENVMAP are GL no-ops
    case 0: case 4: case 5: case 10: case 13: case 16: case 17: case 18:
    case 19: case 20: case 21: case 22: case 23:
        mode = 1; return true; // default / unsupported GL op / MODULATE
    case 1: mode = 0; return true;        // DISABLE behaves as REPLACE in legacy GL
    case 2: case 3: mode = 0; return true; // REPLACE / DECAL
    case 6: mode = 2; return true;        // MODULATE2X
    case 7: mode = 3; return true;        // MODULATE4X
    case 11: mode = 4; return true;       // ADD
    case 12: mode = 5; return true;       // ADDSIGNED
    case 8: mode = 6; return true;        // BLENDDIFFUSEALPHA (GL interpolate)
    case 9: mode = 7; return true;        // BLENDTEXTUREALPHA (GL interpolate)
    default: mode = 1; return true; // OpenGL's switch default is MODULATE
    }
}

XrVector3f RotateVector(const XrQuaternionf& q, const XrVector3f& v)
{
    const float dot = q.x*v.x + q.y*v.y + q.z*v.z;
    const float norm = q.x*q.x + q.y*q.y + q.z*q.z;
    const float crossX = q.y*v.z - q.z*v.y;
    const float crossY = q.z*v.x - q.x*v.z;
    const float crossZ = q.x*v.y - q.y*v.x;
    return XrVector3f{
        2.0f*dot*q.x + (q.w*q.w - norm)*v.x + 2.0f*q.w*crossX,
        2.0f*dot*q.y + (q.w*q.w - norm)*v.y + 2.0f*q.w*crossY,
        2.0f*dot*q.z + (q.w*q.w - norm)*v.z + 2.0f*q.w*crossZ};
}

bool GetHeadForward(const XrQuaternionf& orientation, XrVector3f& forward)
{
    const float length = std::sqrt(orientation.x*orientation.x + orientation.y*orientation.y +
        orientation.z*orientation.z + orientation.w*orientation.w);
    if (!(length > 0.0f) || !std::isfinite(length))
        return false;
    const XrQuaternionf normalized{orientation.x / length, orientation.y / length,
                                   orientation.z / length, orientation.w / length};
    forward = RotateVector(normalized, XrVector3f{0.0f, 0.0f, -1.0f});
    return std::isfinite(forward.x) && std::isfinite(forward.y) && std::isfinite(forward.z);
}

bool GetHeadGazeDeltas(const XrQuaternionf& reference, const XrQuaternionf& current,
                       float& yaw, float& pitch)
{
    XrQuaternionf relative{};
    if (!GetOpenXrRelativeOrientation(reference, current, relative))
        return false;
    XrVector3f forward{};
    if (!GetHeadForward(relative, forward))
        return false;
    const float horizontal = std::sqrt(forward.x*forward.x + forward.z*forward.z);
    if (horizontal < 1.0e-5f)
        return false;
    // Extract the same head-local gaze direction used by rendering from the
    // shared origin^-1 * current orientation.
    yaw = std::atan2(-forward.x, -forward.z);
    pitch = std::atan2(forward.y, horizontal);
    return std::isfinite(yaw) && std::isfinite(pitch);
}

bool MapTextureAlphaOperation(int legacyOperation, uint32_t& mode)
{
    // EF_SetColorOp leaves GL_COMBINE_ALPHA unchanged for blend,
    // MULTIPLYADD, and BUMPENVMAP. Their mode is retained for this TMU.
    switch (legacyOperation)
    {
    case -1: case 255: case 8: case 9: case 14: case 15:
        mode = 0xffffffffu; return true; // unchanged alpha combine in OpenGL
    case 1: case 2: case 3: mode = 0; return true;
    case 11: mode = 4; return true;
    case 12: mode = 5; return true;
    case 0: case 4: case 5: case 6: case 7:
    case 10: case 13: case 16: case 17: case 18: case 19:
    case 20: case 21: case 22: case 23:
        mode = 1; return true; // OpenGL's default MODULATE / unchanged stock state
    default: mode = 1; return true; // OpenGL's switch default is MODULATE
    }
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
    LOAD_VK(createQueryPool, CreateQueryPool);
    LOAD_VK(destroyQueryPool, DestroyQueryPool);
    LOAD_VK(getQueryPoolResults, GetQueryPoolResults);
    LOAD_VK(cmdResetQueryPool, CmdResetQueryPool);
    LOAD_VK(cmdBeginQuery, CmdBeginQuery);
    LOAD_VK(cmdEndQuery, CmdEndQuery);
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
    VulkanResourceManager* resources = m_resources;
    Shutdown();
    m_resources = resources;
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
    m_visibilityQueriesEnabled = context.SupportsOcclusionQueries() &&
        m_createQueryPool && m_destroyQueryPool && m_getQueryPoolResults &&
        m_cmdResetQueryPool && m_cmdBeginQuery && m_cmdEndQuery;
    if (m_visibilityQueriesEnabled)
    {
        VkQueryPoolCreateInfo queryPoolInfo{};
        queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queryPoolInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
        queryPoolInfo.queryCount = 256;
        if (m_createQueryPool(context.GetDevice(), &queryPoolInfo, nullptr,
                              &m_visibilityQueryPool) != VK_SUCCESS)
        {
            m_visibilityQueriesEnabled = false;
            m_visibilityQueryPool = VK_NULL_HANDLE;
        }
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
    // RecordAndSubmit transitions the acquired OpenXR image to this layout
    // before the render pass. Keep the render pass initial layout consistent
    // with that explicit barrier; declaring UNDEFINED here made the Vulkan
    // attachment layout disagree with the actual swapchain image layout.
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
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

    VkAttachmentDescription outputAttachment{};
    outputAttachment.format = m_format;
    outputAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    outputAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    outputAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    outputAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    outputAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    outputAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    outputAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference outputColorReference{};
    outputColorReference.attachment = 0;
    outputColorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription outputSubpass{};
    outputSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    outputSubpass.colorAttachmentCount = 1;
    outputSubpass.pColorAttachments = &outputColorReference;
    VkSubpassDependency outputDependency{};
    outputDependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    outputDependency.dstSubpass = 0;
    outputDependency.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    outputDependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    outputDependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo outputRenderPassInfo{};
    outputRenderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    outputRenderPassInfo.attachmentCount = 1;
    outputRenderPassInfo.pAttachments = &outputAttachment;
    outputRenderPassInfo.subpassCount = 1;
    outputRenderPassInfo.pSubpasses = &outputSubpass;
    outputRenderPassInfo.dependencyCount = 1;
    outputRenderPassInfo.pDependencies = &outputDependency;
    if (m_createRenderPass(context.GetDevice(), &outputRenderPassInfo, nullptr,
                           &m_outputRenderPass) != VK_SUCCESS)
    {
        SetError("vkCreateRenderPass(output) failed");
        Shutdown();
        return false;
    }

    if (!CreatePanelPipeline())
    {
        Shutdown();
        return false;
    }
    if (!CreateRenderTargets())
    {
        Shutdown();
        return false;
    }
    if (!CreateOutputPipeline())
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
        const bool vertexBufferCreated = m_resources->CreateBuffer(
            8ull * 1024ull * 1024ull, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            hostMemory, m_dynamicVertexBuffer);
        const bool indexBufferCreated = m_resources->CreateBuffer(
            4ull * 1024ull * 1024ull, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            hostMemory, m_dynamicIndexBuffer);
        CryLogAlways("OpenXR/Vulkan audit: dynamic buffers init resources=%p vertex=%u handle=%p size=%llu index=%u handle=%p size=%llu",
                     static_cast<void*>(m_resources), vertexBufferCreated ? 1u : 0u,
                     reinterpret_cast<void*>(m_dynamicVertexBuffer.buffer),
                     static_cast<unsigned long long>(m_dynamicVertexBuffer.size),
                     indexBufferCreated ? 1u : 0u,
                     reinterpret_cast<void*>(m_dynamicIndexBuffer.buffer),
                     static_cast<unsigned long long>(m_dynamicIndexBuffer.size));
        if (!vertexBufferCreated || !indexBufferCreated)
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
    shaderInfo.codeSize = sizeof(kVrMultiTextureLitColorVertexSpirv);
    shaderInfo.pCode = kVrMultiTextureLitColorVertexSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr,
                             &m_sceneMultiTextureLitColorVertexShader) != VK_SUCCESS)
    {
        SetError("failed to create lit multitexture vertex shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrMultiTextureLitNoColorVertexSpirv);
    shaderInfo.pCode = kVrMultiTextureLitNoColorVertexSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr,
                             &m_sceneMultiTextureLitNoColorVertexShader) != VK_SUCCESS)
    {
        SetError("failed to create lit no-color multitexture vertex shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrMultiTextureNoColorVertexSpirv);
    shaderInfo.pCode = kVrMultiTextureNoColorVertexSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr,
                             &m_sceneMultiTextureNoColorVertexShader) != VK_SUCCESS)
    {
        SetError("failed to create no-color multitexture vertex shader module");
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
    shaderInfo.codeSize = sizeof(kVrSceneThreeTextureColorFragmentSpirv);
    shaderInfo.pCode = kVrSceneThreeTextureColorFragmentSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr,
                             &m_sceneThreeTextureColorFragmentShader) != VK_SUCCESS)
    {
        SetError("failed to create three-stage fixed-function texture shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrSceneFourTextureColorFragmentSpirv);
    shaderInfo.pCode = kVrSceneFourTextureColorFragmentSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr,
                             &m_sceneFourTextureColorFragmentShader) != VK_SUCCESS)
    {
        SetError("failed to create four-stage fixed-function texture shader module");
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
    // Fragment parameters live in the per-draw uniform block. A single
    // overlapping vertex/fragment push-constant range cannot be updated
    // independently for the two stages.
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.size = sizeof(float) * 32;
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VkDescriptorSetLayout sceneSetLayouts[8] = {
        m_descriptorSetLayout, m_descriptorSetLayout, m_descriptorSetLayout,
        m_descriptorSetLayout, m_descriptorSetLayout, m_descriptorSetLayout,
        m_descriptorSetLayout, m_descriptorSetLayout
    };
    layoutInfo.setLayoutCount = 8;
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
                                                 int normalMapTextureId,
                                                 const float* primaryColor,
                                                 const float* primaryColorMask,
                                                 uint32_t colorWriteMaskOverride,
                                                 float textureStage0LodBias,
                                                 float textureStage1LodBias,
                                                 const VulkanStockTextureStage* textureStage2,
                                                 const VulkanStockTextureStage* textureStage3,
                                                 bool polygonOffset,
                                                 float polygonOffsetFactor,
                                                 float polygonOffsetUnits,
                                                 const float* clipPlane,
                                                 VkDeviceSize vertexBufferOffset,
                                                 int textureWrapMode0,
                                                 int textureWrapMode1,
                                                 int textureWrapMode2,
                                                 int textureWrapMode3,
                                                 const VulkanStockTextureStage* textureStages4To7,
                                                 const VulkanBuffer* lightmapTexCoordBuffer,
                                                 VkDeviceSize lightmapTexCoordOffset,
                                                 bool textureStage1UsesTexCoord1,
                                                 bool textureStage0UsesTexCoord1,
                                                 bool invertVertexRgb,
                                                 bool nearestObject)
{
    if (!m_frameActive || !m_frame.shouldRender || !m_frame.viewsValid ||
        !vertexBuffer || !indexBuffer || !vertexBuffer->buffer || !indexBuffer->buffer ||
        !modelView || !textureMatrix0 || !textureMatrix1 || indexCount == 0 || vertexFormat < 1 || vertexFormat > 16)
    {
        static uint32_t invalidFrameAuditCount = 0;
        if (invalidFrameAuditCount < 32)
        {
            CryLogAlways("OpenXR/Vulkan audit: indexed queue gate instance=%p tid=%llu active=%u render=%u viewsValid=%u viewCount=%u beginCalls=%u beginSuccesses=%u endCalls=%u acquired=%u vbo=%p ibo=%p model=%p tex0=%p tex1=%p indexCount=%u vertexFormat=%d rejectedInputBefore=%u",
                static_cast<void*>(this), static_cast<unsigned long long>(GetAuditThreadId()),
                m_frameActive ? 1u : 0u, m_frame.shouldRender ? 1u : 0u,
                m_frame.viewsValid ? 1u : 0u, m_frame.viewCount,
                m_frameBeginAttempts, m_frameBeginSuccesses, m_frameEndCalls,
                m_imageAcquired ? 1u : 0u,
                vertexBuffer ? reinterpret_cast<void*>(vertexBuffer->buffer) : nullptr,
                indexBuffer ? reinterpret_cast<void*>(indexBuffer->buffer) : nullptr,
                modelView, textureMatrix0, textureMatrix1, indexCount, vertexFormat,
                m_sceneDiagnostics.rejectedInput);
            ++invalidFrameAuditCount;
        }
        ++m_sceneDiagnostics.rejectedInput;
        return false;
    }
    const uint64_t visibilityKey = m_pendingVisibilityKey;
    const bool visibilityTotalCoverage = m_pendingVisibilityTotalCoverage;
    m_pendingVisibilityKey = 0;
    m_pendingVisibilityTotalCoverage = false;
    // These streams are screen-font or coordinate/tangent-only data; they do
    // not contain the world-space position expected by the scene pipeline.
    if (vertexFormat == 5 || vertexFormat == 14 || vertexFormat == 15)
    {
        ++m_sceneDiagnostics.rejectedVertexFeature;
        return false;
    }
    if ((primitiveMode < 0 || primitiveMode > 2) || cullMode < 0 || cullMode > 2)
    {
        ++m_sceneDiagnostics.rejectedInput;
        return false;
    }
    const uint32_t topologyIndex = static_cast<uint32_t>(primitiveMode);
    const std::map<int, LegacyTexture>::const_iterator texture = m_legacyTextures.find(textureId);
    const bool textureExpected = HasVulkanTextureCoordinate(vertexFormat) && textureId > 0;
    // An unsupported legacy image format must not discard the entire world
    // draw. Render the geometry with its vertex/material color until the
    // texture mirror is available; the fallback counter makes this visible in
    // the frame diagnostics rather than turning the missing material black.
    const bool useTexture = textureExpected && texture != m_legacyTextures.end();
    if (textureExpected && !useTexture)
        ++m_sceneDiagnostics.textureFallbacks;
    const std::map<int, LegacyTexture>::const_iterator texture1 = m_legacyTextures.find(textureId1);
    const bool textureId1IsNormalMap = normalMapTextureId > 0 && textureId1 == normalMapTextureId;
    const bool secondTextureExpected = vertexFormat == 16 && useTexture && textureId1 > 0 &&
                                       !textureId1IsNormalMap;
    if (secondTextureExpected && texture1 == m_legacyTextures.end())
        ++m_sceneDiagnostics.textureFallbacks;
    uint32_t stage0ColorMode = 1;
    uint32_t stage0AlphaMode = 1;
    const bool stage0OpsSupported = MapTextureCombineOperation(textureStage0ColorOp, stage0ColorMode) &&
                                    MapTextureAlphaOperation(textureStage0AlphaOp, stage0AlphaMode);
    const auto resolveTextureCombinerState = [&](uint32_t stage, uint32_t& colorMode,
                                                  uint32_t& alphaMode)
    {
        if (colorMode == 0xffffffffu)
            colorMode = m_stockTextureColorModes[stage];
        else
        {
            if (colorMode != m_stockTextureColorModes[stage] &&
                (colorMode == 6u || colorMode == 7u))
            {
                // EF_SetColorOp installs SOURCE2_RGB when entering either
                // diffuse-alpha or texture-alpha interpolation. An eCA_Specular
                // selector in the following argument block leaves that source
                // installed, exactly as GL's no-op switch branch does.
                const uint32_t source2 = colorMode == 6u ? 2u : 1u;
                m_stockTextureEffectiveColorArgs[stage] =
                    (m_stockTextureEffectiveColorArgs[stage] & ~(7u << 6)) |
                    (source2 << 6);
            }
            m_stockTextureColorModes[stage] = colorMode;
        }
        if (alphaMode == 0xffffffffu)
            alphaMode = m_stockTextureAlphaModes[stage];
        else
            m_stockTextureAlphaModes[stage] = alphaMode;
    };
    const auto resolveOneTextureCombinerArgs = [](uint32_t requested, uint32_t& cachedRequest,
                                                   uint32_t& effective)
    {
        if ((requested & 0xffu) == 0xffu)
            return effective;
        if (requested != cachedRequest)
        {
            for (uint32_t shift = 0; shift <= 6; shift += 3)
            {
                const uint32_t selector = (requested >> shift) & 7u;
                // OpenGL's eCA_Specular branch does not call glTexEnvi;
                // retain the source currently installed on that operand.
                if (selector != 0u)
                    effective = (effective & ~(7u << shift)) | (selector << shift);
            }
            cachedRequest = requested;
        }
        return effective;
    };
    const auto resolveTextureCombinerArgs = [&](uint32_t stage, uint32_t& colorArgs,
                                                 uint32_t& alphaArgs)
    {
        colorArgs = resolveOneTextureCombinerArgs(colorArgs, m_stockTextureColorArgs[stage],
                                                  m_stockTextureEffectiveColorArgs[stage]);
        alphaArgs = resolveOneTextureCombinerArgs(alphaArgs, m_stockTextureAlphaArgs[stage],
                                                  m_stockTextureEffectiveAlphaArgs[stage]);
    };
    resolveTextureCombinerState(0, stage0ColorMode, stage0AlphaMode);
    resolveTextureCombinerArgs(0, textureStage0ColorArg, textureStage0AlphaArg);
    if (useTexture && !stage0OpsSupported)
    {
        ++m_sceneDiagnostics.rejectedCombine;
        return false;
    }
    const auto validInterpolateArgs = [](uint32_t packedArgs)
    {
        for (uint32_t source = 0; source < 3; ++source)
        {
            const uint32_t selector = (packedArgs >> (source * 3)) & 7u;
            if (selector > 4u) return false;
        }
        return true;
    };
    if ((stage0ColorMode == 6 || stage0ColorMode == 7) &&
        (!HasVulkanVertexColor(vertexFormat) || !validInterpolateArgs(textureStage0ColorArg)))
    {
        ++m_sceneDiagnostics.rejectedCombine;
        return false;
    }
    uint32_t stage1ColorMode = 1;
    uint32_t stage1AlphaMode = 1;
    const bool stage1OpsSupported = MapTextureCombineOperation(textureStage1ColorOp, stage1ColorMode) &&
                                    MapTextureAlphaOperation(textureStage1AlphaOp, stage1AlphaMode);
    const bool secondTextureAvailable = vertexFormat == 16 && useTexture && textureId1 > 0 &&
                                        !textureId1IsNormalMap &&
                                        texture1 != m_legacyTextures.end();
    if (secondTextureAvailable)
    {
        resolveTextureCombinerState(1, stage1ColorMode, stage1AlphaMode);
        resolveTextureCombinerArgs(1, textureStage1ColorArg, textureStage1AlphaArg);
    }
    if (secondTextureAvailable && !stage1OpsSupported)
    {
        ++m_sceneDiagnostics.rejectedCombine;
        return false;
    }
    const bool useSecondTexture = secondTextureAvailable;
    if (useSecondTexture && (stage1ColorMode == 6 || stage1ColorMode == 7) &&
        (!validInterpolateArgs(textureStage1ColorArg) || !HasVulkanVertexColor(vertexFormat)))
    {
        ++m_sceneDiagnostics.rejectedCombine;
        return false;
    }
    uint32_t stage2ColorMode = 1;
    uint32_t stage2AlphaMode = 1;
    uint32_t stage2ColorArg = textureStage2 ? textureStage2->colorArg : 0x0a1u;
    uint32_t stage2AlphaArg = textureStage2 ? textureStage2->alphaArg : 0x0a1u;
    bool useThirdTexture = false;
    if (textureStage2 && textureStage2->textureId > 0)
    {
        // Stock vertex format 16 carries the second UV set used by texture
        // stages after the first. Until the vertex path grows more UV
        // channels, stage 2 follows that same transformed set.
        if (vertexFormat != 16)
        {
            ++m_sceneDiagnostics.rejectedVertexFeature;
            return false;
        }
        if (!useTexture)
        {
            ++m_sceneDiagnostics.textureFallbacks;
        }
        else
        {
            const std::map<int, LegacyTexture>::const_iterator texture2 =
                m_legacyTextures.find(textureStage2->textureId);
            if (texture2 == m_legacyTextures.end())
            {
                ++m_sceneDiagnostics.textureFallbacks;
            }
            else
            {
                if (!MapTextureCombineOperation(textureStage2->colorOp, stage2ColorMode) ||
                    !MapTextureAlphaOperation(textureStage2->alphaOp, stage2AlphaMode))
                {
                    ++m_sceneDiagnostics.rejectedCombine;
                    return false;
                }
                resolveTextureCombinerState(2, stage2ColorMode, stage2AlphaMode);
                resolveTextureCombinerArgs(2, stage2ColorArg, stage2AlphaArg);
                // OpenGL's eCO_DISABLE installs GL_REPLACE; it is still an
                // active texture stage and samples source 0 (normally TEXTURE).
                useThirdTexture = true;
                if (useThirdTexture && (stage2ColorMode == 6 || stage2ColorMode == 7) &&
                    (!validInterpolateArgs(stage2ColorArg) || !HasVulkanVertexColor(vertexFormat)))
                {
                    ++m_sceneDiagnostics.rejectedCombine;
                    return false;
                }
            }
        }
    }
    uint32_t stage3ColorMode = 1;
    uint32_t stage3AlphaMode = 1;
    uint32_t stage3ColorArg = textureStage3 ? textureStage3->colorArg : 0x0a1u;
    uint32_t stage3AlphaArg = textureStage3 ? textureStage3->alphaArg : 0x0a1u;
    bool useFourthTexture = false;
    if (textureStage3 && textureStage3->textureId > 0)
    {
        if (vertexFormat != 16)
        {
            ++m_sceneDiagnostics.rejectedVertexFeature;
            return false;
        }
        const std::map<int, LegacyTexture>::const_iterator texture3 =
            m_legacyTextures.find(textureStage3->textureId);
        if (texture3 == m_legacyTextures.end() || !useTexture)
            ++m_sceneDiagnostics.textureFallbacks;
        else
        {
            if (!MapTextureCombineOperation(textureStage3->colorOp, stage3ColorMode) ||
                !MapTextureAlphaOperation(textureStage3->alphaOp, stage3AlphaMode))
            {
                ++m_sceneDiagnostics.rejectedCombine;
                return false;
            }
            resolveTextureCombinerState(3, stage3ColorMode, stage3AlphaMode);
            resolveTextureCombinerArgs(3, stage3ColorArg, stage3AlphaArg);
            useFourthTexture = true;
            if (useFourthTexture && (stage3ColorMode == 6 || stage3ColorMode == 7) &&
                (!validInterpolateArgs(stage3ColorArg) || !HasVulkanVertexColor(vertexFormat)))
            {
                ++m_sceneDiagnostics.rejectedCombine;
                return false;
            }
        }
    }
    std::array<bool, 4> useTextureStages4To7{};
    std::array<uint32_t, 4> stage4To7ColorModes{};
    std::array<uint32_t, 4> stage4To7AlphaModes{};
    std::array<uint32_t, 4> stage4To7ColorArgs{};
    std::array<uint32_t, 4> stage4To7AlphaArgs{};
    if (textureStages4To7)
    {
        for (uint32_t stageIndex = 0; stageIndex < 4; ++stageIndex)
        {
            const VulkanStockTextureStage& stage = textureStages4To7[stageIndex];
            stage4To7ColorModes[stageIndex] = 1;
            stage4To7AlphaModes[stageIndex] = 1;
            stage4To7ColorArgs[stageIndex] = stage.colorArg;
            stage4To7AlphaArgs[stageIndex] = stage.alphaArg;
            if (stage.textureId <= 0)
                continue;
            if (vertexFormat != 16)
            {
                ++m_sceneDiagnostics.rejectedVertexFeature;
                return false;
            }
            if (!useTexture || m_legacyTextures.find(stage.textureId) == m_legacyTextures.end())
            {
                ++m_sceneDiagnostics.textureFallbacks;
                continue;
            }
            if (!MapTextureCombineOperation(stage.colorOp, stage4To7ColorModes[stageIndex]) ||
                !MapTextureAlphaOperation(stage.alphaOp, stage4To7AlphaModes[stageIndex]))
            {
                ++m_sceneDiagnostics.rejectedCombine;
                return false;
            }
            resolveTextureCombinerState(stageIndex + 4, stage4To7ColorModes[stageIndex],
                                        stage4To7AlphaModes[stageIndex]);
            resolveTextureCombinerArgs(stageIndex + 4, stage4To7ColorArgs[stageIndex],
                                       stage4To7AlphaArgs[stageIndex]);
            useTextureStages4To7[stageIndex] = true;
            if (useTextureStages4To7[stageIndex] &&
                (stage4To7ColorModes[stageIndex] == 6 || stage4To7ColorModes[stageIndex] == 7) &&
                (!validInterpolateArgs(stage4To7ColorArgs[stageIndex]) ||
                 !HasVulkanVertexColor(vertexFormat)))
            {
                ++m_sceneDiagnostics.rejectedCombine;
                return false;
            }
        }
    }
    const bool useFifthToEighthTexture =
        useTextureStages4To7[0] || useTextureStages4To7[1] ||
        useTextureStages4To7[2] || useTextureStages4To7[3];
    const bool hasTangentBasis = tangentBuffer && tangentBuffer->buffer &&
                                 (vertexFormat == 9 || vertexFormat == 10 || vertexFormat == 13);
    const std::map<int, LegacyTexture>::const_iterator normalMap =
        m_legacyTextures.find(normalMapTextureId);
    const bool useNormalMap = hasTangentBasis && useTexture && normalMapTextureId > 0 &&
                              normalMap != m_legacyTextures.end() && !useSecondTexture &&
                              !useThirdTexture && !useFourthTexture;
    if (hasTangentBasis && normalMapTextureId > 0 && useFifthToEighthTexture)
    {
        // The eight-stage stock fragment variant does not yet share the
        // tangent-space bump lighting code. Reject instead of silently
        // substituting a different material result.
        ++m_sceneDiagnostics.rejectedVertexFeature;
        return false;
    }
    if (normalMapTextureId > 0 && normalMap == m_legacyTextures.end())
        ++m_sceneDiagnostics.textureFallbacks;
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
    {
        ++m_sceneDiagnostics.rejectedPipelineState;
        return false;
    }
    VulkanPipelineState decodedState{};
    if (!DecodeLegacyPipelineState(pipelineState, cullMode, false, stencilState, decodedState))
    {
        ++m_sceneDiagnostics.rejectedPipelineState;
        return false;
    }
    const uint64_t compactPipelineKey = (static_cast<uint64_t>(useTexture ? 1u : 0u) << 63) |
                                 (static_cast<uint64_t>(useSecondTexture ? 1u : 0u) << 62) |
                                 (static_cast<uint64_t>(useNormalMap ? 1u : 0u) << 61) |
                                 (static_cast<uint64_t>(lightmapTexCoordBuffer &&
                                     lightmapTexCoordBuffer->buffer ? 1u : 0u) << 59) |
                                 (static_cast<uint64_t>(textureStage1UsesTexCoord1 ? 1u : 0u) << 58) |
                                 (static_cast<uint64_t>(textureStage0UsesTexCoord1 ? 1u : 0u) << 57) |
                                 (static_cast<uint64_t>(hasTangentBasis ? 1u : 0u) << 60) |
                                 (static_cast<uint64_t>(pipelineState) << 24) |
                                 (static_cast<uint64_t>(vertexFormat) << 16) |
                                 (static_cast<uint64_t>(topologyIndex) << 8) |
                                 static_cast<uint64_t>(cullMode);
    uint32_t polygonOffsetFactorBits = 0;
    uint32_t polygonOffsetUnitsBits = 0;
    if (polygonOffset)
    {
        std::memcpy(&polygonOffsetFactorBits, &polygonOffsetFactor, sizeof(polygonOffsetFactorBits));
        std::memcpy(&polygonOffsetUnitsBits, &polygonOffsetUnits, sizeof(polygonOffsetUnitsBits));
    }
    std::array<uint32_t, 61> pipelineKey = {{
        static_cast<uint32_t>(compactPipelineKey), static_cast<uint32_t>(compactPipelineKey >> 32),
        (renderState & 0x00400000u) ? stencilState : 0u, stage0ColorMode, stage0AlphaMode,
        textureStage0ColorArg, textureStage0AlphaArg, textureStage0Constant,
        textureStage1ColorArg, textureStage1AlphaArg, textureStage1Constant,
        stage1ColorMode, stage1AlphaMode, colorWriteMaskOverride,
        useThirdTexture ? stage2ColorMode : 0u,
        useThirdTexture ? stage2AlphaMode : 0u,
        useThirdTexture ? stage2ColorArg : 0u,
        useThirdTexture ? stage2AlphaArg : 0u,
        useThirdTexture ? textureStage2->constant : 0u,
        useThirdTexture && textureStage2->useTexCoord1 ? 1u : 0u,
        useFourthTexture ? stage3ColorMode : 0u,
        useFourthTexture ? stage3AlphaMode : 0u,
        useFourthTexture ? stage3ColorArg : 0u,
        useFourthTexture ? stage3AlphaArg : 0u,
        useFourthTexture ? textureStage3->constant : 0u,
        useFourthTexture && textureStage3->useTexCoord1 ? 1u : 0u,
        polygonOffset ? 1u : 0u, polygonOffsetFactorBits, polygonOffsetUnitsBits
    }};
    for (uint32_t stageIndex = 0; stageIndex < 4; ++stageIndex)
    {
        const uint32_t keyIndex = 29 + stageIndex * 7;
        pipelineKey[keyIndex + 0] = useTextureStages4To7[stageIndex] ? stage4To7ColorModes[stageIndex] : 0u;
        pipelineKey[keyIndex + 1] = useTextureStages4To7[stageIndex] ? stage4To7AlphaModes[stageIndex] : 0u;
        pipelineKey[keyIndex + 2] = useTextureStages4To7[stageIndex] ? stage4To7ColorArgs[stageIndex] : 0u;
        pipelineKey[keyIndex + 3] = useTextureStages4To7[stageIndex] ? stage4To7AlphaArgs[stageIndex] : 0u;
        pipelineKey[keyIndex + 4] = useTextureStages4To7[stageIndex] ?
            textureStages4To7[stageIndex].constant : 0u;
        pipelineKey[keyIndex + 5] = useTextureStages4To7[stageIndex] &&
            textureStages4To7[stageIndex].useTexCoord1 ? 1u : 0u;
        pipelineKey[keyIndex + 6] = useTextureStages4To7[stageIndex] ? 1u : 0u;
        const float stageLodBias = useTextureStages4To7[stageIndex] && textureStages4To7 ?
            textureStages4To7[stageIndex].lodBias : 0.0f;
        std::memcpy(&pipelineKey[57 + stageIndex], &stageLodBias, sizeof(uint32_t));
    }
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::map<std::array<uint32_t, 61>, VkPipeline>::iterator cached = m_scenePipelineCache.find(pipelineKey);
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
                            (useSecondTexture || useThirdTexture || useFourthTexture || useFifthToEighthTexture) ?
                                (hasNormal ? (hasColor ? m_sceneMultiTextureLitColorVertexShader :
                                                         m_sceneMultiTextureLitNoColorVertexShader) :
                                             (hasColor ? m_sceneMultiTextureColorVertexShader :
                                                         m_sceneMultiTextureNoColorVertexShader)) :
                            useTexture && hasNormal ? (hasColor ? m_sceneTextureLitColorVertexShader :
                                                          m_sceneTextureLitVertexShader) :
                            useTexture ? (hasColor ? m_sceneTextureColorVertexShader :
                                                     m_sceneTextureVertexShader) :
                            hasNormal ? (hasColor ? m_sceneLitColorVertexShader : m_sceneLitVertexShader) :
                            (hasColor ? m_sceneColorVertexShader : m_sceneVertexShader);
        desc.fragmentShader = useNormalMap ? m_sceneTextureBumpFragmentShader :
                              (useFourthTexture || useFifthToEighthTexture) ? m_sceneFourTextureColorFragmentShader :
                              useThirdTexture ? m_sceneThreeTextureColorFragmentShader :
                              useSecondTexture ? m_sceneMultiTextureColorFragmentShader :
                              useTexture ? m_sceneTextureColorFragmentShader :
                              hasNormal ? m_sceneLitFragmentShader :
                              (hasColor ? m_sceneColorFragmentShader : m_sceneFragmentShader);
        desc.cryVertexFormat = static_cast<uint32_t>(vertexFormat);
        desc.hasTangents = tangentBuffer && tangentBuffer->buffer &&
                           (vertexFormat == 9 || vertexFormat == 10 || vertexFormat == 13);
        desc.hasLightmapTexCoords = lightmapTexCoordBuffer && lightmapTexCoordBuffer->buffer;
        desc.hasNormalMap = useNormalMap;
        desc.topology = topologyIndex == 0 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST :
                        topologyIndex == 1 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP :
                                             VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        desc.renderState = pipelineState;
        desc.hasColorWriteMaskOverride = colorWriteMaskOverride != 0xffffffffu;
        desc.colorWriteMaskOverride = static_cast<VkColorComponentFlags>(colorWriteMaskOverride);
        desc.cullMode = cullMode;
        desc.depthBias = polygonOffset;
        desc.depthBiasSlopeFactor = polygonOffset ? polygonOffsetFactor : 0.0f;
        desc.depthBiasConstantFactor = polygonOffset ? polygonOffsetUnits : 0.0f;
        desc.supportsAlphaTest = true;
        desc.supportsWireframe = m_context->SupportsWireframe();
        desc.supportsStage0Combine = useTexture;
        desc.hasSecondaryColor = HasVulkanSecondaryColor(vertexFormat);
        desc.stage0ColorMode = stage0ColorMode;
        desc.stage0AlphaMode = stage0AlphaMode;
        desc.stage0ColorArg = textureStage0ColorArg;
        desc.stage0AlphaArg = textureStage0AlphaArg;
        desc.stage0Constant = textureStage0Constant;
        desc.stage0UsesTexCoord1 = textureStage0UsesTexCoord1;
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
        desc.stage1UsesTexCoord1 = textureStage1UsesTexCoord1;
        desc.supportsStage2Combine = useThirdTexture;
        desc.stage2ColorMode = stage2ColorMode;
        desc.stage2AlphaMode = stage2AlphaMode;
        desc.stage2UsesTexCoord1 = textureStage2 ? textureStage2->useTexCoord1 : true;
        if (useThirdTexture)
        {
            desc.stage2ColorArg = stage2ColorArg;
            desc.stage2AlphaArg = stage2AlphaArg;
            desc.stage2Constant = textureStage2->constant;
        }
        desc.supportsStage3Combine = useFourthTexture;
        desc.stage3ColorMode = stage3ColorMode;
        desc.stage3AlphaMode = stage3AlphaMode;
        desc.stage3UsesTexCoord1 = textureStage3 ? textureStage3->useTexCoord1 : true;
        if (useFourthTexture)
        {
            desc.stage3ColorArg = stage3ColorArg;
            desc.stage3AlphaArg = stage3AlphaArg;
            desc.stage3Constant = textureStage3->constant;
        }
        for (uint32_t stageIndex = 0; stageIndex < 4; ++stageIndex)
        {
            VulkanPipelineTextureStage& pipelineStage = desc.stages4To7[stageIndex];
            pipelineStage.enabled = useTextureStages4To7[stageIndex];
            pipelineStage.colorMode = stage4To7ColorModes[stageIndex];
            pipelineStage.alphaMode = stage4To7AlphaModes[stageIndex];
            pipelineStage.colorArg = stage4To7ColorArgs[stageIndex];
            pipelineStage.alphaArg = stage4To7AlphaArgs[stageIndex];
            pipelineStage.useTexCoord1 = textureStages4To7 &&
                textureStages4To7[stageIndex].useTexCoord1;
            if (textureStages4To7)
            {
                pipelineStage.constant = textureStages4To7[stageIndex].constant;
                pipelineStage.lodBias = textureStages4To7[stageIndex].lodBias;
            }
        }
        if (!m_pipelineFactory.CreateGraphicsPipeline(desc, pipeline))
        {
            ++m_sceneDiagnostics.pipelineCreationFailed;
            if (!m_scenePipelineErrorLogged)
            {
#if defined(__ANDROID__)
                __android_log_print(ANDROID_LOG_ERROR, "CryVulkan",
                    "scene pipeline failed: %s; vertexFormat=%u normalMap=%u textures=%u vertexColor=%u normal=%u",
                    m_pipelineFactory.GetLastError(), static_cast<uint32_t>(vertexFormat),
                    useNormalMap ? 1u : 0u,
                    (useTexture ? 1u : 0u) + (useSecondTexture ? 1u : 0u) +
                    (useThirdTexture ? 1u : 0u) + (useFourthTexture ? 1u : 0u) +
                    (useFifthToEighthTexture ? 1u : 0u),
                    hasColor ? 1u : 0u, hasNormal ? 1u : 0u);
#endif
                CryLogAlways("OpenXR/Vulkan: scene pipeline creation failed: %s; vertexFormat=%u normalMap=%u textures=%u vertexColor=%u normal=%u",
                    m_pipelineFactory.GetLastError(), static_cast<uint32_t>(vertexFormat),
                    useNormalMap ? 1u : 0u,
                    (useTexture ? 1u : 0u) + (useSecondTexture ? 1u : 0u) +
                    (useThirdTexture ? 1u : 0u) + (useFourthTexture ? 1u : 0u) +
                    (useFifthToEighthTexture ? 1u : 0u),
                    hasColor ? 1u : 0u, hasNormal ? 1u : 0u);
                m_scenePipelineErrorLogged = true;
            }
            return false;
        }
        m_scenePipelineCache[pipelineKey] = pipeline;
    }
    StockDraw draw;
    draw.vertexBuffer = vertexBuffer->buffer;
    draw.tangentBuffer = (vertexFormat == 9 || vertexFormat == 10 || vertexFormat == 13) &&
                         tangentBuffer ? tangentBuffer->buffer : VK_NULL_HANDLE;
    draw.lightmapTexCoordBuffer = lightmapTexCoordBuffer && lightmapTexCoordBuffer->buffer ?
        lightmapTexCoordBuffer->buffer : VK_NULL_HANDLE;
    draw.indexBuffer = indexBuffer->buffer;
    draw.vertexFormat = vertexFormat;
    draw.nearPlane = nearestObject ? 0.01f : m_stockNearPlane;
    draw.farPlane = nearestObject ? 40.0f : m_stockFarPlane;
    draw.indexCount = indexCount;
    draw.firstIndex = firstIndex;
    draw.vertexOffset = vertexOffset;
    draw.vertexBufferOffset = vertexBufferOffset;
    draw.lightmapTexCoordOffset = lightmapTexCoordOffset;
    draw.topology = primitiveMode == 0 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST :
                    primitiveMode == 1 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP :
                                         VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    draw.pipeline = pipeline;
    if (m_stockViewportSet)
    draw.viewport = m_stockViewport;
    else
    {
        draw.viewport.x = 0.0f;
        draw.viewport.y = 0.0f;
        draw.viewport.width = static_cast<float>(m_swapchain.width);
        draw.viewport.height = static_cast<float>(m_swapchain.height);
        draw.viewport.minDepth = m_stockMinDepth;
        draw.viewport.maxDepth = m_stockMaxDepth;
    }
    draw.nearestObject = nearestObject;
    if (nearestObject)
    {
        draw.viewport.minDepth = 0.0f;
        draw.viewport.maxDepth = 0.1f; // OpenGL EF_ObjectChange uses glDepthRange(0, 0.1).
    }
    draw.textureId = useTexture ? textureId : 0;
    draw.projectorCookieTextureId = m_stockProjectorTextureId;
    draw.projectorCookieEnabled = m_stockProjectorTextureId > 0 &&
        m_legacyTextures.find(m_stockProjectorTextureId) != m_legacyTextures.end();
    std::memcpy(draw.projectorBasis, m_stockProjectorBasis, sizeof(draw.projectorBasis));
    draw.projectorFrustumScale = m_stockProjectorFrustumScale;
    draw.textureId1 = useSecondTexture ? textureId1 : 0;
    draw.textureWrapMode[0] = textureWrapMode0;
    draw.textureWrapMode[1] = textureWrapMode1;
    draw.textureWrapMode[2] = textureWrapMode2;
    draw.textureWrapMode[3] = textureWrapMode3;
    if (textureStages4To7)
    {
        for (uint32_t stage = 0; stage < 4; ++stage)
            draw.textureStages4To7[stage] = textureStages4To7[stage];
    }
    draw.textureStage2 = textureStage2 ? *textureStage2 : VulkanStockTextureStage{};
    draw.textureStage3 = textureStage3 ? *textureStage3 : VulkanStockTextureStage{};
    draw.useThirdTexture = useThirdTexture;
    draw.useFourthTexture = useFourthTexture;
    for (uint32_t stageIndex = 0; stageIndex < 4; ++stageIndex)
    {
        draw.useTextureStages4To7[stageIndex] = useTextureStages4To7[stageIndex];
        if (textureStages4To7)
            draw.textureStages4To7[stageIndex] = textureStages4To7[stageIndex];
    }
    draw.useSecondTexture = useSecondTexture;
    draw.useNormalMap = useNormalMap;
    draw.polygonOffset = polygonOffset;
    draw.polygonOffsetFactor = polygonOffsetFactor;
    draw.polygonOffsetUnits = polygonOffsetUnits;
    if (clipPlane)
        std::memcpy(draw.clipPlane, clipPlane, sizeof(draw.clipPlane));
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
    draw.fogStart = m_stockFogStart * m_stockFogRangeScale;
    draw.fogEnd = m_stockFogEnd * m_stockFogRangeScale;
    draw.fogMode = m_stockFogMode;
    for (int i = 0; i < 16; ++i) draw.modelView[i] = modelView[i];
    for (int i = 0; i < 16; ++i)
    {
        draw.textureMatrix0[i] = textureMatrix0[i];
        draw.textureMatrix1[i] = textureMatrix1[i];
    }
    for (uint32_t stage = 0; stage < 8; ++stage)
    {
        draw.textureTransformRows[0][stage][0] = 1.0f;
        draw.textureTransformRows[1][stage][1] = 1.0f;
        draw.textureTransformRows[2][stage][2] = 1.0f;
    }
    // Textured-lit vertex shaders use the dynamic block for the secondary UV
    // transform because their 128-byte push-constant layout is already full.
    draw.textureTransformRows[0][1][0] = textureMatrix1[0];
    draw.textureTransformRows[0][1][1] = textureMatrix1[4];
    draw.textureTransformRows[0][1][2] = textureMatrix1[12];
    draw.textureTransformRows[1][1][0] = textureMatrix1[1];
    draw.textureTransformRows[1][1][1] = textureMatrix1[5];
    draw.textureTransformRows[1][1][2] = textureMatrix1[13];
    const auto setStageUvTransform = [&](uint32_t stage, const VulkanStockTextureStage& textureStage)
    {
        draw.textureTransformRows[0][stage][0] = textureStage.uvTransform[0];
        draw.textureTransformRows[0][stage][1] = textureStage.uvTransform[1];
        draw.textureTransformRows[0][stage][2] = textureStage.uvTransform[2];
        draw.textureTransformRows[1][stage][0] = textureStage.uvTransform[3];
        draw.textureTransformRows[1][stage][1] = textureStage.uvTransform[4];
        draw.textureTransformRows[1][stage][2] = textureStage.uvTransform[5];
        draw.textureTransformRows[2][stage][0] = textureStage.uvTransform[6];
        draw.textureTransformRows[2][stage][1] = textureStage.uvTransform[7];
        draw.textureTransformRows[2][stage][2] = textureStage.uvTransform[8];
    };
    if (useThirdTexture && textureStage2) setStageUvTransform(2, *textureStage2);
    if (useFourthTexture && textureStage3) setStageUvTransform(3, *textureStage3);
    for (uint32_t stage = 0; stage < 4; ++stage)
        if (useTextureStages4To7[stage] && textureStages4To7)
            setStageUvTransform(stage + 4, textureStages4To7[stage]);
    if (materialLighting)
        std::memcpy(draw.materialLighting, materialLighting, sizeof(draw.materialLighting));
    std::memcpy(draw.lightingConstants, draw.materialLighting, sizeof(draw.materialLighting));
    // The final component in each transform row is unused by UV evaluation.
    // Carry OpenGL's RGB ambient material/object factor there for lit shaders.
    draw.textureTransformRows[0][7][3] = draw.materialLighting[8];
    draw.textureTransformRows[1][7][3] = draw.materialLighting[9];
    draw.textureTransformRows[2][6][3] = draw.materialLighting[10];
    draw.textureTransformRows[2][7][3] = draw.materialLighting[7];
    if (draw.projectorCookieEnabled)
    {
        // The otherwise unused W components carry the object-space projector
        // basis and the scale used by OpenGL's LightCMProject matrix.
        for (uint32_t row = 0; row < 3; ++row)
        {
            draw.textureTransformRows[row][2][3] = draw.projectorBasis[row * 3];
            draw.textureTransformRows[row][3][3] = draw.projectorBasis[row * 3 + 1];
            draw.textureTransformRows[row][4][3] = draw.projectorBasis[row * 3 + 2];
        }
        draw.textureTransformRows[0][5][3] = draw.projectorFrustumScale;
        draw.textureTransformRows[1][5][3] = 1.0f;
    }
    if (primaryColor)
        std::memcpy(draw.primaryColor, primaryColor, sizeof(draw.primaryColor));
    if (primaryColorMask)
        std::memcpy(draw.primaryColorMask, primaryColorMask, sizeof(draw.primaryColorMask));
    draw.invertVertexRgb = invertVertexRgb;
    draw.textureLodBias[0] = textureStage0LodBias;
    draw.textureLodBias[1] = textureStage1LodBias;
    draw.textureLodBias[2] = useThirdTexture && textureStage2 ? textureStage2->lodBias : 0.0f;
    draw.textureLodBias[3] = useFourthTexture && textureStage3 ? textureStage3->lodBias : 0.0f;
    const float fogConstants[32] = {
        draw.fogColor[0], draw.fogColor[1], draw.fogColor[2], 1.0f,
        draw.fogEnabled ? 1.0f : 0.0f, static_cast<float>(draw.fogMode),
        draw.fogDensity, draw.fogStart,
        draw.fogEnd, 0.05f, 1000.0f, 0.0f,
        draw.globalOpacity, draw.alphaTestRef, draw.additiveMaterial ? 1.0f : 0.0f,
        draw.invertVertexRgb ? 1.0f : 0.0f,
        draw.primaryColor[0], draw.primaryColor[1], draw.primaryColor[2], draw.primaryColor[3],
        draw.primaryColorMask[0], draw.primaryColorMask[1],
        draw.primaryColorMask[2], draw.primaryColorMask[3],
        draw.textureLodBias[0], draw.textureLodBias[1],
        draw.textureLodBias[2], draw.textureLodBias[3],
        draw.clipPlane[0], draw.clipPlane[1], draw.clipPlane[2], draw.clipPlane[3]
    };
    std::memcpy(draw.fogConstants, fogConstants, sizeof(fogConstants));
    struct SceneUniformBlock {
        float rows[3][8][4];
        float fog[32];
        float lighting[12];
    };
    const VkDeviceSize transformOffset = static_cast<VkDeviceSize>(m_stockDraws.size()) *
                                         m_textureTransformStride * 2;
    constexpr VkDeviceSize transformBlockSize = sizeof(SceneUniformBlock);
    const VkBuffer previousTransformBuffer = m_textureTransformBuffer.buffer;
    if (transformOffset > std::numeric_limits<uint32_t>::max() - m_textureTransformStride ||
        transformOffset > std::numeric_limits<VkDeviceSize>::max() -
            m_textureTransformStride - transformBlockSize ||
        !EnsureDynamicBufferCapacity(m_textureTransformBuffer, m_previousTextureTransformBuffers,
                                     transformOffset + m_textureTransformStride + transformBlockSize,
                                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
        return false;
    if (previousTransformBuffer != m_textureTransformBuffer.buffer &&
        !UpdateTextureTransformDescriptors())
        return false;
    if (previousTransformBuffer != m_textureTransformBuffer.buffer)
    {
        for (size_t index = 0; index < m_stockDraws.size(); ++index)
        {
            const StockDraw& previousDraw = m_stockDraws[index];
            for (uint32_t eye = 0; eye < 2; ++eye)
            {
                SceneUniformBlock eyeBlock{};
                std::memcpy(eyeBlock.rows, previousDraw.textureTransformRows, sizeof(eyeBlock.rows));
                std::memcpy(eyeBlock.fog, previousDraw.fogConstants, sizeof(eyeBlock.fog));
                std::memcpy(eyeBlock.lighting, previousDraw.lightingConstants, sizeof(eyeBlock.lighting));
                auto& eyeRows = eyeBlock.rows;
                // The homogeneous .w channels are unused by UV evaluation here.
                // Carry this eye's asymmetric projection rays and render extent
                // to the fragment shader in unused UV-row components.
                if (m_context->SupportsRadialFog() && m_frame.viewsValid && eye < m_frame.viewCount)
                {
                    const XrFovf& fov = m_frame.views[eye].fov;
                    eyeRows[0][0][3] = std::tan(fov.angleLeft);
                    eyeRows[1][0][3] = std::tan(fov.angleRight);
                    eyeRows[0][1][3] = std::tan(fov.angleUp);
                    eyeRows[1][1][3] = std::tan(fov.angleDown);
                }
                eyeRows[2][0][3] = static_cast<float>(m_swapchain.width);
                eyeRows[2][1][3] = static_cast<float>(m_swapchain.height);
                const VkDeviceSize eyeOffset = (static_cast<VkDeviceSize>(index) * 2 + eye) *
                                                m_textureTransformStride;
                if (!m_resources->UploadBuffer(m_textureTransformBuffer, &eyeBlock,
                                               transformBlockSize, eyeOffset))
                    return false;
            }
        }
    }
    draw.textureTransformOffset = static_cast<uint32_t>(transformOffset);
    for (uint32_t eye = 0; eye < 2; ++eye)
    {
        SceneUniformBlock eyeBlock{};
        std::memcpy(eyeBlock.rows, draw.textureTransformRows, sizeof(eyeBlock.rows));
        std::memcpy(eyeBlock.fog, draw.fogConstants, sizeof(eyeBlock.fog));
        std::memcpy(eyeBlock.lighting, draw.lightingConstants, sizeof(eyeBlock.lighting));
        auto& eyeRows = eyeBlock.rows;
        if (m_context->SupportsRadialFog() && m_frame.viewsValid && eye < m_frame.viewCount)
        {
            const XrFovf& fov = m_frame.views[eye].fov;
            eyeRows[0][0][3] = std::tan(fov.angleLeft);
            eyeRows[1][0][3] = std::tan(fov.angleRight);
            eyeRows[0][1][3] = std::tan(fov.angleUp);
            eyeRows[1][1][3] = std::tan(fov.angleDown);
        }
        eyeRows[2][0][3] = static_cast<float>(m_swapchain.width);
        eyeRows[2][1][3] = static_cast<float>(m_swapchain.height);
        if (!m_resources->UploadBuffer(m_textureTransformBuffer, &eyeBlock,
                transformBlockSize, transformOffset + static_cast<VkDeviceSize>(eye) *
                                    m_textureTransformStride))
            return false;
    }
    m_stockDraws.push_back(draw);
    if (visibilityKey && m_visibilityQueriesEnabled && m_currentVisibilityQueries.size() < 256)
    {
        std::pair<std::map<uint64_t, std::pair<uint32_t, uint32_t> >::iterator, bool> inserted =
            m_currentVisibilityQueryByKey.insert(std::make_pair(visibilityKey,
                std::make_pair(UINT32_MAX, UINT32_MAX)));
        std::pair<uint32_t, uint32_t>& queryIndices = inserted.first->second;
        uint32_t& queryIndex = visibilityTotalCoverage ? queryIndices.second : queryIndices.first;
        if (queryIndex == UINT32_MAX)
        {
            queryIndex = static_cast<uint32_t>(m_currentVisibilityQueries.size());
            m_currentVisibilityQueries.push_back(
                VisibilityQuery{ visibilityKey, queryIndex, visibilityTotalCoverage });
            if (visibilityTotalCoverage)
                m_stockDraws.back().visibilityCoverageQueryIndex = queryIndex;
            else
                m_stockDraws.back().visibilityQueryIndex = queryIndex;
        }
    }
    ++m_sceneDiagnostics.queuedDraws;
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
                                                        int normalMapTextureId,
                                                        const float* primaryColor,
                                                        const float* primaryColorMask,
                                                        uint32_t colorWriteMaskOverride,
                                                        float textureStage0LodBias,
                                                        float textureStage1LodBias,
                                                        const VulkanStockTextureStage* textureStage2,
                                                        const VulkanStockTextureStage* textureStage3,
                                                        bool polygonOffset,
                                                        float polygonOffsetFactor,
                                                        float polygonOffsetUnits,
                                                        const float* clipPlane,
                                                        int textureWrapMode0,
                                                        int textureWrapMode1,
                                                        int textureWrapMode2,
                                                        int textureWrapMode3,
                                                        const VulkanStockTextureStage* textureStages4To7,
                                                        const void* lightmapTexCoords,
                                                        bool textureStage1UsesTexCoord1,
                                                        bool textureStage0UsesTexCoord1,
                                                        bool invertVertexRgb,
                                                        bool nearestObject)
{
    static uint32_t clientDrawAuditCount = 0;
    const auto auditFailure = [&](const char* reason) -> bool
    {
        if (clientDrawAuditCount < 384)
        {
            CryLogAlways("OpenXR/Vulkan audit: client draw rejected reason=%s call=%u vertices=%p count=%u indices=%p count=%u format=%d primitive=%d resources=%p vbo=%p/%llu ibo=%p/%llu used=%llu/%llu frameActive=%u render=%u views=%u",
                         reason, clientDrawAuditCount, vertices, vertexCount, indices, indexCount,
                         vertexFormat, primitiveMode, static_cast<void*>(m_resources),
                         reinterpret_cast<void*>(m_dynamicVertexBuffer.buffer),
                         static_cast<unsigned long long>(m_dynamicVertexBuffer.size),
                         reinterpret_cast<void*>(m_dynamicIndexBuffer.buffer),
                         static_cast<unsigned long long>(m_dynamicIndexBuffer.size),
                         static_cast<unsigned long long>(m_dynamicVertexUsed),
                         static_cast<unsigned long long>(m_dynamicIndexUsed),
                         m_frameActive ? 1u : 0u, m_frame.shouldRender ? 1u : 0u,
                         m_frame.viewsValid ? 1u : 0u);
        }
        ++clientDrawAuditCount;
        return false;
    };
    if (!m_resources || !vertices || vertexCount == 0 ||
        !m_dynamicVertexBuffer.buffer || !m_dynamicIndexBuffer.buffer)
        return auditFailure("missing resources, vertices, count, or dynamic buffer");
    const uint16_t* sourceIndices = indices;
    if (!lightmapTexCoords && reusePreviousClientGeometry && sourceIndices && m_reusableClientGeometry.valid &&
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
                                  globalOpacity, alphaTestRef, tangentBuffer, normalMapTextureId,
                                  primaryColor, primaryColorMask, colorWriteMaskOverride,
                                  textureStage0LodBias, textureStage1LodBias, textureStage2, textureStage3,
                                  polygonOffset, polygonOffsetFactor, polygonOffsetUnits, clipPlane,
                                  m_reusableClientGeometry.vertexBufferOffset,
                                  textureWrapMode0, textureWrapMode1, textureWrapMode2, textureWrapMode3,
                                  textureStages4To7, nullptr, 0,
                                  textureStage1UsesTexCoord1,
                                  textureStage0UsesTexCoord1,
                                  invertVertexRgb, nearestObject))
            return true;
        m_reusableClientGeometry.valid = false;
    }
    std::vector<uint16_t> generatedIndices;
    if (!indices)
    {
        if (indexCount != 0 || vertexCount > 65535)
            return auditFailure("cannot generate 16-bit indices");
        generatedIndices.resize(vertexCount);
        for (uint32_t i = 0; i < vertexCount; ++i)
            generatedIndices[i] = static_cast<uint16_t>(i);
        indices = generatedIndices.data();
        indexCount = vertexCount;
    }
    if (indexCount == 0)
        return auditFailure("zero index count");
    // Invalid 16-bit indices make the GPU fetch vertices outside this draw's
    // upload. Some drivers then rasterize enormous, apparently stretched
    // triangles instead of reporting an error, so reject the draw before it
    // reaches vkCmdDrawIndexed and include enough data to identify its source.
    static uint32_t outOfRangeIndexAuditCount = 0;
    for (uint32_t index = 0; index < indexCount; ++index)
    {
        if (indices[index] < vertexCount)
            continue;
        if (outOfRangeIndexAuditCount < 32)
            CryLogAlways("OpenXR/Vulkan audit: client index out of range draw=%u indexOffset=%u value=%u vertexCount=%u format=%d primitive=%d",
                outOfRangeIndexAuditCount, index, indices[index], vertexCount,
                vertexFormat, primitiveMode);
        ++outOfRangeIndexAuditCount;
        return auditFailure("index exceeds uploaded vertex count");
    }
    if (lightmapTexCoords)
        m_reusableClientGeometry.valid = false;
    VulkanVertexFormat format{};
    if (!GetVulkanVertexFormat(static_cast<uint32_t>(vertexFormat), format))
        return auditFailure("unsupported vertex format");
    const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(vertexCount) * format.stride;
    const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(indexCount) * sizeof(uint16_t);
    const VkDeviceSize lightmapBytes = lightmapTexCoords ?
        static_cast<VkDeviceSize>(vertexCount) * sizeof(float) * 2 : 0;
    if (vertexBytes > std::numeric_limits<VkDeviceSize>::max() - m_dynamicVertexUsed ||
        indexBytes > std::numeric_limits<VkDeviceSize>::max() - m_dynamicIndexUsed)
        return auditFailure("dynamic buffer size overflow");
    const VkDeviceSize lightmapOffset = lightmapTexCoords ?
        (m_dynamicVertexUsed + vertexBytes + 7u) & ~static_cast<VkDeviceSize>(7u) : 0;
    const VkDeviceSize dynamicVertexEnd = lightmapTexCoords ?
        lightmapOffset + lightmapBytes : m_dynamicVertexUsed + vertexBytes;
    if (lightmapTexCoords && lightmapBytes > std::numeric_limits<VkDeviceSize>::max() - lightmapOffset)
        return auditFailure("lightmap buffer size overflow");
    if (!EnsureDynamicBufferCapacity(m_dynamicVertexBuffer, m_previousDynamicVertexBuffers,
                                    dynamicVertexEnd, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
        return auditFailure("vertex buffer capacity growth failed");
    if (!EnsureDynamicBufferCapacity(m_dynamicIndexBuffer, m_previousDynamicIndexBuffers,
                                    m_dynamicIndexUsed + indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
        return auditFailure("index buffer capacity growth failed");
    const VkDeviceSize vertexBufferOffset = m_dynamicVertexUsed;
    if (!m_resources->UploadBuffer(m_dynamicVertexBuffer, vertices, vertexBytes, vertexBufferOffset))
        return auditFailure("vertex upload failed");
    if (lightmapTexCoords && !m_resources->UploadBuffer(m_dynamicVertexBuffer, lightmapTexCoords,
                                                         lightmapBytes, lightmapOffset))
        return auditFailure("lightmap coordinate upload failed");
    if (!m_resources->UploadBuffer(m_dynamicIndexBuffer, indices, indexBytes, m_dynamicIndexUsed))
        return auditFailure("index upload failed");
    const uint32_t firstIndex = static_cast<uint32_t>(m_dynamicIndexUsed / sizeof(uint16_t));
    if (!QueueStockIndexedDraw(&m_dynamicVertexBuffer, &m_dynamicIndexBuffer, vertexFormat,
                               indexCount, firstIndex, primitiveMode, renderState, cullMode,
                               textureId, textureId1, textureStage0ColorOp, textureStage0AlphaOp,
                               textureStage1ColorOp, textureStage1AlphaOp,
                               textureStage0ColorArg, textureStage0AlphaArg, textureStage0Constant,
                               textureStage1ColorArg, textureStage1AlphaArg, textureStage1Constant,
                               stencilState, stencilRef, stencilMask,
                               modelView, textureMatrix0, textureMatrix1, 0,
                               materialLighting, globalOpacity, alphaTestRef,
                               tangentBuffer, normalMapTextureId, primaryColor, primaryColorMask,
                               colorWriteMaskOverride, textureStage0LodBias, textureStage1LodBias,
                               textureStage2, textureStage3, polygonOffset, polygonOffsetFactor, polygonOffsetUnits,
                               clipPlane, vertexBufferOffset,
                               textureWrapMode0, textureWrapMode1, textureWrapMode2, textureWrapMode3,
                               textureStages4To7,
                               lightmapTexCoords ? &m_dynamicVertexBuffer : nullptr,
                               lightmapOffset,
                               textureStage1UsesTexCoord1,
                               textureStage0UsesTexCoord1,
                               invertVertexRgb, nearestObject))
        return auditFailure("indexed draw queue rejected");
    if (clientDrawAuditCount < 384)
    {
        CryLogAlways("OpenXR/Vulkan audit: client draw queued call=%u verts=%u indices=%u format=%d bytes=%llu/%llu offset=%llu firstIndex=%u",
                     clientDrawAuditCount, vertexCount, indexCount, vertexFormat,
                     static_cast<unsigned long long>(vertexBytes),
                     static_cast<unsigned long long>(indexBytes),
                     static_cast<unsigned long long>(vertexBufferOffset), firstIndex);
    }
    ++clientDrawAuditCount;
    m_dynamicVertexUsed = dynamicVertexEnd;
    m_dynamicIndexUsed += indexBytes;
    if (sourceIndices && !lightmapTexCoords)
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
        m_reusableClientGeometry.vertexOffset = 0;
        m_reusableClientGeometry.vertexBufferOffset = vertexBufferOffset;
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

bool VulkanFrameRenderer::UpdateTextureTransformDescriptors()
{
    if (!m_context || !m_descriptorSet || !m_textureTransformBuffer.buffer)
        return false;
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = m_textureTransformBuffer.buffer;
    bufferInfo.offset = 0;
    bufferInfo.range = sizeof(float) * (8 * 3 * 4 + 32 + 12);
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(1 + m_legacyTextures.size() * 3 + m_projectorDescriptorSets.size());
    const auto appendWrite = [&](VkDescriptorSet set)
    {
        if (!set) return;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 1;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        write.pBufferInfo = &bufferInfo;
        writes.push_back(write);
    };
    appendWrite(m_descriptorSet);
    for (std::map<int, LegacyTexture>::const_iterator it = m_legacyTextures.begin();
         it != m_legacyTextures.end(); ++it)
    {
        appendWrite(it->second.descriptorSet);
        for (int wrap = 0; wrap < 2; ++wrap)
            appendWrite(it->second.wrapDescriptorSets[wrap]);
    }
    for (const auto& entry : m_projectorDescriptorSets)
        appendWrite(entry.second);
    if (!writes.empty())
        m_updateDescriptorSets(m_context->GetDevice(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    return true;
}

bool VulkanFrameRenderer::QueuePanelImage(int textureId, float x, float y, float width, float height,
                                          float s0, float t0, float s1, float t1, float angleDegrees,
                                          float red, float green, float blue, float alpha,
                                          float logicalWidth, float logicalHeight,
                                          uint32_t blendState)
{
    if (!m_frameActive || !m_frame.shouldRender || textureId <= 0 ||
        !(width > 0.0f) || !(height > 0.0f) ||
        !(logicalWidth > 0.0f) || !(logicalHeight > 0.0f))
        return false;
    PanelImage image;
    image.textureId = textureId;
    image.blendState = blendState & 0xffu;
    image.x = x; image.y = y; image.width = width; image.height = height;
    image.s0 = s0; image.t0 = t0; image.s1 = s1; image.t1 = t1;
    image.angleDegrees = angleDegrees;
    image.red = red; image.green = green; image.blue = blue; image.alpha = alpha;
    image.logicalWidth = logicalWidth; image.logicalHeight = logicalHeight;
    image.scissorEnabled = m_stockScissorEnabled;
    image.scissor = m_stockScissor;
    m_panelImages.push_back(image);
    return true;
}

bool VulkanFrameRenderer::QueuePanelIndexedGeometry(int textureId, const void* vertices,
                                                    uint32_t vertexCount,
                                                    const uint16_t* indices,
                                                    uint32_t indexCount,
                                                    float logicalWidth, float logicalHeight,
                                                    const float* screenTransform,
                                                    uint32_t blendState)
{
    // DrawDynVB supplies P3F_COL4UB_TEX2F (format 4: 24-byte stride).
    constexpr VkDeviceSize vertexStride = 24;
    if (!m_frameActive || !m_frame.shouldRender || !m_resources || textureId <= 0 ||
        !vertices || !indices || !vertexCount || !indexCount || indexCount % 3 != 0 ||
        vertexCount > 65535 || !(logicalWidth > 0.0f) || !(logicalHeight > 0.0f) ||
        vertexCount > std::numeric_limits<VkDeviceSize>::max() / vertexStride)
        return false;

    for (uint32_t i = 0; i < indexCount; ++i)
        if (indices[i] >= vertexCount)
            return false;

    const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(vertexCount) * vertexStride;
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

    std::vector<uint8_t> panelVertices(static_cast<size_t>(vertexBytes));
    const uint8_t* source = static_cast<const uint8_t*>(vertices);
    for (uint32_t i = 0; i < vertexCount; ++i)
    {
        uint8_t* destination = panelVertices.data() + static_cast<size_t>(i) * vertexStride;
        memcpy(destination, source + static_cast<size_t>(i) * vertexStride,
               static_cast<size_t>(vertexStride));
        float* position = reinterpret_cast<float*>(destination);
        if (screenTransform)
        {
            const float x = position[0], y = position[1], z = position[2];
            const float w = screenTransform[3] * x + screenTransform[7] * y +
                            screenTransform[11] * z + screenTransform[15];
            position[0] = screenTransform[0] * x + screenTransform[4] * y +
                          screenTransform[8] * z + screenTransform[12];
            position[1] = screenTransform[1] * x + screenTransform[5] * y +
                          screenTransform[9] * z + screenTransform[13];
            position[2] = screenTransform[2] * x + screenTransform[6] * y +
                          screenTransform[10] * z + screenTransform[14];
            if (std::isfinite(w) && std::fabs(w) > 1.0e-6f && w != 1.0f)
            {
                position[0] /= w;
                position[1] /= w;
                position[2] /= w;
            }
        }
        position[0] = 2.0f * position[0] / logicalWidth - 1.0f;
        position[1] = 2.0f * position[1] / logicalHeight - 1.0f;
        position[2] = 0.0f;
    }

    const VkDeviceSize vertexOffset = m_dynamicVertexUsed;
    const uint32_t firstIndex = static_cast<uint32_t>(m_dynamicIndexUsed / sizeof(uint16_t));
    if (!m_resources->UploadBuffer(m_dynamicVertexBuffer, panelVertices.data(), vertexBytes, vertexOffset) ||
        !m_resources->UploadBuffer(m_dynamicIndexBuffer, indices, indexBytes, m_dynamicIndexUsed))
        return false;

    PanelImage draw;
    draw.indexedGeometry = true;
    draw.textureId = textureId;
    draw.blendState = blendState & 0xffu;
    draw.logicalWidth = logicalWidth;
    draw.logicalHeight = logicalHeight;
    draw.scissorEnabled = m_stockScissorEnabled;
    draw.scissor = m_stockScissor;
    draw.vertexBuffer = m_dynamicVertexBuffer.buffer;
    draw.indexBuffer = m_dynamicIndexBuffer.buffer;
    draw.vertexBufferOffset = vertexOffset;
    draw.firstIndex = firstIndex;
    draw.indexCount = indexCount;
    m_panelImages.push_back(draw);
    m_dynamicVertexUsed += vertexBytes;
    m_dynamicIndexUsed += indexBytes;
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

void VulkanFrameRenderer::SetStockViewport(int x, int y, int width, int height,
                                            float logicalWidth, float logicalHeight)
{
    if (x == 0 && y == 0 && width == 0 && height == 0)
    {
        m_stockViewportSet = false;
        m_stockViewport = VkViewport{};
        return;
    }
    if (!m_swapchain.width || !m_swapchain.height ||
        !(logicalWidth > 0.0f) || !(logicalHeight > 0.0f) ||
        !std::isfinite(logicalWidth) || !std::isfinite(logicalHeight) || width <= 0 || height <= 0)
    {
        m_stockViewportSet = false;
        m_stockViewport = VkViewport{};
        return;
    }

    const float scaleX = static_cast<float>(m_swapchain.width) / logicalWidth;
    const float scaleY = static_cast<float>(m_swapchain.height) / logicalHeight;
    m_stockViewport.x = static_cast<float>(x) * scaleX;
    m_stockViewport.y = static_cast<float>(y) * scaleY;
    m_stockViewport.width = static_cast<float>(width) * scaleX;
    m_stockViewport.height = static_cast<float>(height) * scaleY;
    m_stockViewport.minDepth = m_stockMinDepth;
    m_stockViewport.maxDepth = m_stockMaxDepth;
    m_stockViewportSet = true;
}

void VulkanFrameRenderer::SetStockDepthRange(float minDepth, float maxDepth)
{
    if (!std::isfinite(minDepth) || !std::isfinite(maxDepth))
        return;
    m_stockMinDepth = std::max(0.0f, std::min(1.0f, minDepth));
    m_stockMaxDepth = std::max(0.0f, std::min(1.0f, maxDepth));
    if (m_stockViewportSet)
    {
        m_stockViewport.minDepth = m_stockMinDepth;
        m_stockViewport.maxDepth = m_stockMaxDepth;
    }
}

void VulkanFrameRenderer::SetStockProjector(int cubeAtlasTextureId,
                                             const float basis[9],
                                             float frustumScale)
{
    m_stockProjectorTextureId = cubeAtlasTextureId > 0 ? cubeAtlasTextureId : 0;
    if (basis)
        std::memcpy(m_stockProjectorBasis, basis, sizeof(m_stockProjectorBasis));
    else
        std::memset(m_stockProjectorBasis, 0, sizeof(m_stockProjectorBasis));
    m_stockProjectorFrustumScale = frustumScale > 1.0e-4f ? frustumScale : 1.0f;
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
                                                     int filterMode, float maxAnisotropy)
{
    if (!m_initialized || !m_resources || textureId <= 0 || !width || !height || !rgbaPixels ||
        width > 16384 || height > 16384)
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
    if (filterMode < VulkanFilterNearest || filterMode > VulkanFilterAnisotropic)
        filterMode = VulkanFilterTrilinear;
    const auto createTextureSampler = [&](VkSampler& sampler, float maxLod) -> bool
    {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        const bool nearest = filterMode == VulkanFilterNearest;
        samplerInfo.magFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        samplerInfo.minFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = (filterMode == VulkanFilterTrilinear ||
                                  filterMode == VulkanFilterAnisotropic) ?
            VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = clampU ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = clampV ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.anisotropyEnable = filterMode == VulkanFilterAnisotropic &&
            m_context->SupportsAnisotropicFiltering() ? VK_TRUE : VK_FALSE;
        const float supportedAnisotropy = m_context->GetMaxSamplerAnisotropy();
        samplerInfo.maxAnisotropy = samplerInfo.anisotropyEnable ?
            (maxAnisotropy < 1.0f ? 1.0f :
             maxAnisotropy > supportedAnisotropy ? supportedAnisotropy : maxAnisotropy) : 1.0f;
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
                existing->second.filterMode == filterMode &&
                existing->second.maxAnisotropy == maxAnisotropy)
                return true;
            ReleaseLegacyTextureWrapVariants(existing->second);
            VkSampler newSampler = VK_NULL_HANDLE;
            if (!createTextureSampler(newSampler,
                    static_cast<float>(existing->second.texture.mipLevels - 1))) return false;
            const VkSampler oldSampler = existing->second.sampler;
            ReleaseProjectorTextureDescriptorSets();
            existing->second.sampler = newSampler;
            existing->second.clampU = clampU;
            existing->second.clampV = clampV;
            existing->second.filterMode = filterMode;
            existing->second.maxAnisotropy = maxAnisotropy;
            VkDescriptorImageInfo imageInfo{};
            imageInfo.sampler = newSampler;
            imageInfo.imageView = existing->second.texture.view;
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet writes[2]{};
            for (uint32_t i = 0; i < 2; ++i)
            {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = existing->second.descriptorSet;
                writes[i].dstBinding = i == 0 ? 0 : 2;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].pImageInfo = &imageInfo;
            }
            m_updateDescriptorSets(m_context->GetDevice(), 2, writes, 0, nullptr);
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
    mirror.maxAnisotropy = maxAnisotropy;
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
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = mirror.descriptorSet;
        writes[i].dstBinding = i == 0 ? 0 : 2;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imageInfo;
    }
    m_updateDescriptorSets(m_context->GetDevice(), 2, writes, 0, nullptr);
    mirror.bytes = bytes;
    m_legacyTextures[textureId] = mirror;
    m_legacyTextureBytes += bytes;
    return UpdateTextureTransformDescriptors();
}

bool VulkanFrameRenderer::RegisterLegacyRgbaCubeTexture(int textureId, uint32_t width,
                                                         uint32_t height,
                                                         const uint8_t* const facePixels[6],
                                                         bool noMipmaps, int filterMode,
                                                         float maxAnisotropy)
{
    if (!facePixels || width == 0 || height == 0 || width > 16384u / 3u ||
        height > 8192u || static_cast<size_t>(width) >
            std::numeric_limits<size_t>::max() / height / 24u)
        return false;
    for (uint32_t face = 0; face < 6; ++face)
        if (!facePixels[face])
            return false;

    const uint32_t atlasWidth = width * 3u;
    const uint32_t atlasHeight = height * 2u;
    std::vector<uint8_t> atlas(static_cast<size_t>(atlasWidth) * atlasHeight * 4u);
    for (uint32_t face = 0; face < 6; ++face)
    {
        const uint32_t tileX = face % 3u;
        const uint32_t tileY = face / 3u;
        for (uint32_t row = 0; row < height; ++row)
        {
            const size_t sourceOffset = static_cast<size_t>(row) * width * 4u;
            const size_t destinationOffset =
                (static_cast<size_t>(tileY * height + row) * atlasWidth + tileX * width) * 4u;
            std::memcpy(atlas.data() + destinationOffset,
                        facePixels[face] + sourceOffset,
                        static_cast<size_t>(width) * 4u);
        }
    }

    // Mip generation across a face atlas blends unrelated faces at tile
    // boundaries. Keep the projected cookie at level zero until a native cube
    // image view is wired into the scene descriptors.
    (void)noMipmaps;
    return RegisterLegacyRgbaTexture(textureId, atlasWidth, atlasHeight, atlas.data(),
                                     true, true, false, true, filterMode, maxAnisotropy);
}

VkDescriptorSet VulkanFrameRenderer::GetLegacyTextureDescriptorSet(int textureId, int wrapMode)
{
    std::map<int, LegacyTexture>::iterator found = m_legacyTextures.find(textureId);
    if (found == m_legacyTextures.end())
        return VK_NULL_HANDLE;
    LegacyTexture& texture = found->second;
    if (wrapMode < 0)
        return texture.descriptorSet;
    if (wrapMode > 1)
        return VK_NULL_HANDLE;
    const bool clamp = wrapMode == 1;
    if (texture.clampU == clamp && texture.clampV == clamp)
        return texture.descriptorSet;
    if (texture.wrapDescriptorSets[wrapMode])
        return texture.wrapDescriptorSets[wrapMode];

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    const bool nearest = texture.filterMode == VulkanFilterNearest;
    samplerInfo.magFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    samplerInfo.minFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = (texture.filterMode == VulkanFilterTrilinear ||
                              texture.filterMode == VulkanFilterAnisotropic) ?
        VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = clamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE :
                                       VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = samplerInfo.addressModeU;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = texture.filterMode == VulkanFilterAnisotropic &&
        m_context->SupportsAnisotropicFiltering() ? VK_TRUE : VK_FALSE;
    const float maxAnisotropy = m_context->GetMaxSamplerAnisotropy();
    samplerInfo.maxAnisotropy = samplerInfo.anisotropyEnable ?
        (texture.maxAnisotropy < 1.0f ? 1.0f :
         texture.maxAnisotropy > maxAnisotropy ? maxAnisotropy : texture.maxAnisotropy) : 1.0f;
    samplerInfo.maxLod = (texture.noMipmaps || texture.filterMode == VulkanFilterLinear) ?
        0.0f : static_cast<float>(texture.texture.mipLevels - 1);
    if (m_createSampler(m_context->GetDevice(), &samplerInfo, nullptr,
                        &texture.wrapSamplers[wrapMode]) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    VkDescriptorSetAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool = m_descriptorPool;
    allocation.descriptorSetCount = 1;
    allocation.pSetLayouts = &m_descriptorSetLayout;
    if (m_allocateDescriptorSets(m_context->GetDevice(), &allocation,
                                 &texture.wrapDescriptorSets[wrapMode]) != VK_SUCCESS)
    {
        m_destroySampler(m_context->GetDevice(), texture.wrapSamplers[wrapMode], nullptr);
        texture.wrapSamplers[wrapMode] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }
    VkDescriptorImageInfo image{};
    image.sampler = texture.wrapSamplers[wrapMode];
    image.imageView = texture.texture.view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = texture.wrapDescriptorSets[wrapMode];
        writes[i].dstBinding = i == 0 ? 0 : 2;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &image;
    }
    m_updateDescriptorSets(m_context->GetDevice(), 2, writes, 0, nullptr);
    if (!UpdateTextureTransformDescriptors())
        return VK_NULL_HANDLE;
    return texture.wrapDescriptorSets[wrapMode];
}

VkDescriptorSet VulkanFrameRenderer::GetProjectorTextureDescriptorSet(int baseTextureId,
                                                                       int cookieTextureId,
                                                                       int wrapMode)
{
    if (baseTextureId <= 0)
        baseTextureId = cookieTextureId;
    const auto base = m_legacyTextures.find(baseTextureId);
    const auto cookie = m_legacyTextures.find(cookieTextureId);
    if (base == m_legacyTextures.end() || cookie == m_legacyTextures.end())
        return VK_NULL_HANDLE;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(baseTextureId)) << 32) ^
        (static_cast<uint64_t>(static_cast<uint32_t>(cookieTextureId)) << 2) ^
        static_cast<uint64_t>(static_cast<uint32_t>(wrapMode + 1));
    const auto cached = m_projectorDescriptorSets.find(key);
    if (cached != m_projectorDescriptorSets.end())
        return cached->second;

    const VkDescriptorSet baseSet = GetLegacyTextureDescriptorSet(baseTextureId, wrapMode);
    if (!baseSet)
        return VK_NULL_HANDLE;
    const LegacyTexture& baseTexture = base->second;
    const LegacyTexture& cookieTexture = cookie->second;
    VkSampler cookieSampler = cookieTexture.sampler;
    if (!cookieSampler)
        return VK_NULL_HANDLE;

    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool = m_descriptorPool;
    allocation.descriptorSetCount = 1;
    allocation.pSetLayouts = &m_descriptorSetLayout;
    if (m_allocateDescriptorSets(m_context->GetDevice(), &allocation, &set) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    VkDescriptorImageInfo images[2]{};
    images[0].sampler = wrapMode >= 0 && wrapMode < 2 && baseTexture.wrapSamplers[wrapMode] ?
        baseTexture.wrapSamplers[wrapMode] : baseTexture.sampler;
    images[0].imageView = baseTexture.texture.view;
    images[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    images[1].sampler = cookieSampler;
    images[1].imageView = cookieTexture.texture.view;
    images[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorBufferInfo buffer{};
    buffer.buffer = m_textureTransformBuffer.buffer;
    buffer.offset = 0;
    buffer.range = sizeof(float) * (8 * 3 * 4 + 32 + 12);
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i == 0 ? 0 : (i == 1 ? 2 : 1);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC :
                                            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = i < 2 ? &images[i] : nullptr;
        writes[i].pBufferInfo = i == 2 ? &buffer : nullptr;
    }
    m_updateDescriptorSets(m_context->GetDevice(), 3, writes, 0, nullptr);
    m_projectorDescriptorSets[key] = set;
    return set;
}

void VulkanFrameRenderer::ReleaseLegacyTextureWrapVariants(LegacyTexture& texture)
{
    for (int i = 0; i < 2; ++i)
    {
        if (texture.wrapDescriptorSets[i] && m_freeDescriptorSets && m_descriptorPool)
            m_freeDescriptorSets(m_context->GetDevice(), m_descriptorPool, 1,
                                 &texture.wrapDescriptorSets[i]);
        if (texture.wrapSamplers[i] && m_destroySampler)
            m_destroySampler(m_context->GetDevice(), texture.wrapSamplers[i], nullptr);
        texture.wrapDescriptorSets[i] = VK_NULL_HANDLE;
        texture.wrapSamplers[i] = VK_NULL_HANDLE;
    }
}

void VulkanFrameRenderer::DestroyLegacyTexture(LegacyTexture& texture)
{
    ReleaseLegacyTextureWrapVariants(texture);
    if (texture.descriptorSet && m_freeDescriptorSets && m_descriptorPool)
        m_freeDescriptorSets(m_context->GetDevice(), m_descriptorPool, 1, &texture.descriptorSet);
    if (texture.sampler && m_destroySampler)
        m_destroySampler(m_context->GetDevice(), texture.sampler, nullptr);
    if (m_resources && texture.texture.image)
        m_resources->DestroyTexture(texture.texture);
    texture.descriptorSet = VK_NULL_HANDLE;
    texture.sampler = VK_NULL_HANDLE;
}

void VulkanFrameRenderer::CollectDeferredLegacyTextureReleases()
{
    if (m_deferredLegacyTextureReleases.empty())
        return;
    ReleaseProjectorTextureDescriptorSets();
    for (size_t i = 0; i < m_deferredLegacyTextureReleases.size(); ++i)
        DestroyLegacyTexture(m_deferredLegacyTextureReleases[i]);
    m_deferredLegacyTextureReleases.clear();
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

void VulkanFrameRenderer::ReleaseProjectorTextureDescriptorSets()
{
    if (m_context && m_descriptorPool && m_freeDescriptorSets)
        for (const auto& entry : m_projectorDescriptorSets)
            if (entry.second)
                m_freeDescriptorSets(m_context->GetDevice(), m_descriptorPool, 1, &entry.second);
    m_projectorDescriptorSets.clear();
}

void VulkanFrameRenderer::ReleaseLegacyTexture(int textureId)
{
    std::map<int, LegacyTexture>::iterator found = m_legacyTextures.find(textureId);
    if (found == m_legacyTextures.end()) return;

    // This callback can run while a frame is in flight. Move the full resource
    // set out of the public texture map and destroy it after BeginFrame has
    // waited for that frame's fence; never wait or free Vulkan resources here.
    m_legacyTextureBytes -= found->second.bytes;
    m_deferredLegacyTextureReleases.push_back(std::move(found->second));
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
    VkDescriptorSetLayoutBinding descriptorBindings[3]{};
    descriptorBindings[0].binding = 0;
    descriptorBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorBindings[0].descriptorCount = 1;
    descriptorBindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    descriptorBindings[1].binding = 1;
    descriptorBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    descriptorBindings[1].descriptorCount = 1;
    descriptorBindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    descriptorBindings[2] = descriptorBindings[0];
    descriptorBindings[2].binding = 2;
    VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo{};
    descriptorLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorLayoutInfo.bindingCount = 3;
    descriptorLayoutInfo.pBindings = descriptorBindings;
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
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
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

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 16384;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    poolSizes[1].descriptorCount = 4096;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 4096;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;
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
    const VkDeviceSize alignment = m_context->GetMinUniformBufferOffsetAlignment() ?
        m_context->GetMinUniformBufferOffsetAlignment() : 1;
    constexpr VkDeviceSize textureTransformBlockSize = sizeof(float) * (8 * 3 * 4 + 32 + 12);
    m_textureTransformStride = ((textureTransformBlockSize + alignment - 1) / alignment) * alignment;
    if (!m_resources || !m_resources->CreateBuffer(
            m_textureTransformStride * 2048,
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            m_textureTransformBuffer))
    {
        SetError("failed to create per-draw texture transform buffer");
        return false;
    }
    if (!UpdateTextureTransformDescriptors())
    {
        SetError("failed to bind the per-draw texture transform buffer");
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
    panelPipeline.renderState = 0x00020000u | 0x2u | 0x20u; // no depth test, GS_BLSRC_ONE/GS_BLDST_ONE
    const bool additivePipelineCreated = blendPipelineCreated &&
        m_pipelineFactory.CreateGraphicsPipeline(panelPipeline, m_panelAdditivePipeline);

    VkShaderModule geometryVertexShader = VK_NULL_HANDLE;
    VkShaderModule geometryFragmentShader = VK_NULL_HANDLE;
    shaderInfo.codeSize = sizeof(kVrPanelGeometryVertexSpirv);
    shaderInfo.pCode = kVrPanelGeometryVertexSpirv;
    bool geometryPipelineCreated =
        m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr,
                             &geometryVertexShader) == VK_SUCCESS;
    shaderInfo.codeSize = sizeof(kVrPanelGeometryFragmentSpirv);
    shaderInfo.pCode = kVrPanelGeometryFragmentSpirv;
    geometryPipelineCreated = geometryPipelineCreated &&
        m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr,
                             &geometryFragmentShader) == VK_SUCCESS;
    if (geometryPipelineCreated)
    {
        VulkanGraphicsPipelineDesc geometryPipeline = panelPipeline;
        geometryPipeline.vertexShader = geometryVertexShader;
        geometryPipeline.fragmentShader = geometryFragmentShader;
        geometryPipeline.useVertexInput = true;
        geometryPipeline.cryVertexFormat = 4; // P3F_COL4UB_TEX2F
        geometryPipeline.renderState = 0x00020000u | 0x65u; // no depth test, SRCALPHA/INVSRCALPHA
        geometryPipelineCreated = m_pipelineFactory.CreateGraphicsPipeline(
            geometryPipeline, m_panelGeometryPipeline);
        geometryPipeline.renderState = 0x00020000u;
        geometryPipelineCreated = geometryPipelineCreated && m_pipelineFactory.CreateGraphicsPipeline(
            geometryPipeline, m_panelGeometryOpaquePipeline);
        geometryPipeline.renderState = 0x00020000u | 0x2u | 0x20u;
        geometryPipelineCreated = geometryPipelineCreated && m_pipelineFactory.CreateGraphicsPipeline(
            geometryPipeline, m_panelGeometryAdditivePipeline);
    }
    if (!opaquePipelineCreated || !blendPipelineCreated || !additivePipelineCreated ||
        !geometryPipelineCreated)
    {
        if (geometryFragmentShader)
            m_destroyShaderModule(m_context->GetDevice(), geometryFragmentShader, nullptr);
        if (geometryVertexShader)
            m_destroyShaderModule(m_context->GetDevice(), geometryVertexShader, nullptr);
        m_destroyShaderModule(m_context->GetDevice(), fragmentShader, nullptr);
        m_destroyShaderModule(m_context->GetDevice(), vertexShader, nullptr);
        SetError(m_pipelineFactory.GetLastError());
        return false;
    }
    m_panelVertexShader = vertexShader;
    m_panelFragmentShader = fragmentShader;
    m_panelGeometryVertexShader = geometryVertexShader;
    m_panelGeometryFragmentShader = geometryFragmentShader;
    return true;
}

bool VulkanFrameRenderer::CreateOutputPipeline()
{
    VkShaderModuleCreateInfo shaderInfo{};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = sizeof(kVrResolveVertexSpirv);
    shaderInfo.pCode = kVrResolveVertexSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr,
                             &m_outputVertexShader) != VK_SUCCESS)
    {
        SetError("failed to create Vulkan output vertex shader module");
        return false;
    }
    shaderInfo.codeSize = sizeof(kVrResolveFragmentSpirv);
    shaderInfo.pCode = kVrResolveFragmentSpirv;
    if (m_createShaderModule(m_context->GetDevice(), &shaderInfo, nullptr,
                             &m_outputFragmentShader) != VK_SUCCESS)
    {
        SetError("failed to create Vulkan output fragment shader module");
        return false;
    }
    VulkanGraphicsPipelineDesc desc{};
    desc.renderPass = m_outputRenderPass;
    desc.layout = m_pipelineLayout;
    desc.vertexShader = m_outputVertexShader;
    desc.fragmentShader = m_outputFragmentShader;
    desc.useVertexInput = false;
    desc.dynamicViewport = false;
    desc.viewportExtent.width = m_swapchain.width;
    desc.viewportExtent.height = m_swapchain.height;
    desc.renderState = 0x00020000u; // no depth test
    desc.cullMode = 0;
    if (!m_pipelineFactory.CreateGraphicsPipeline(desc, m_outputPipeline))
    {
        SetError(m_pipelineFactory.GetLastError());
        return false;
    }
    return true;
}

VkPipeline VulkanFrameRenderer::GetPanelPipeline(uint32_t blendState, bool indexedGeometry)
{
    blendState &= 0xffu;
    if (blendState == 0u)
        return indexedGeometry ? m_panelGeometryOpaquePipeline : m_panelPipeline;
    if (blendState == 0x65u)
        return indexedGeometry ? m_panelGeometryPipeline : m_panelBlendPipeline;
    if (blendState == 0x22u)
        return indexedGeometry ? m_panelGeometryAdditivePipeline : m_panelAdditivePipeline;

    const uint32_t key = blendState | (indexedGeometry ? 0x100u : 0u);
    const std::map<uint32_t, VkPipeline>::const_iterator found = m_panelPipelineCache.find(key);
    if (found != m_panelPipelineCache.end())
        return found->second;
    VulkanGraphicsPipelineDesc desc{};
    desc.renderPass = m_renderPass;
    desc.layout = m_pipelineLayout;
    desc.vertexShader = indexedGeometry ? m_panelGeometryVertexShader : m_panelVertexShader;
    desc.fragmentShader = indexedGeometry ? m_panelGeometryFragmentShader : m_panelFragmentShader;
    desc.useVertexInput = indexedGeometry;
    desc.dynamicViewport = false;
    desc.viewportExtent.width = m_swapchain.width;
    desc.viewportExtent.height = m_swapchain.height;
    desc.cryVertexFormat = indexedGeometry ? 4u : 0u;
    desc.renderState = 0x00020000u | blendState; // GS_NODEPTHTEST plus the exact OpenGL blend pair
    desc.cullMode = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!m_pipelineFactory.CreateGraphicsPipeline(desc, pipeline))
        return VK_NULL_HANDLE;
    m_panelPipelineCache[key] = pipeline;
    return pipeline;
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
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_descriptorSet;
        writes[i].dstBinding = i == 0 ? 0 : 2;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imageInfo;
    }
    m_updateDescriptorSets(m_context->GetDevice(), 2, writes, 0, nullptr);
    return true;
}

bool VulkanFrameRenderer::CreateRenderTargets()
{
    VkFormatProperties formatProperties{};
    m_getPhysicalDeviceFormatProperties(m_context->GetPhysicalDevice(), m_format, &formatProperties);
    if ((formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0)
    {
        SetError("OpenXR color format cannot be sampled for the Vulkan output pass");
        return false;
    }
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
            if (!m_resources->CreateColorTarget(m_swapchain.width, m_swapchain.height,
                                                m_format, target.color[viewIndex]))
            {
                SetError(m_resources->GetLastError());
                return false;
            }
            VkImageView framebufferAttachments[2] = {
                target.color[viewIndex].view, target.depth[viewIndex].view
            };

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

            VkFramebufferCreateInfo outputFramebufferInfo{};
            outputFramebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            outputFramebufferInfo.renderPass = m_outputRenderPass;
            outputFramebufferInfo.attachmentCount = 1;
            outputFramebufferInfo.pAttachments = &target.views[viewIndex];
            outputFramebufferInfo.width = m_swapchain.width;
            outputFramebufferInfo.height = m_swapchain.height;
            outputFramebufferInfo.layers = 1;
            if (m_createFramebuffer(m_context->GetDevice(), &outputFramebufferInfo, nullptr,
                                    &target.outputFramebuffers[viewIndex]) != VK_SUCCESS)
            {
                SetError("vkCreateFramebuffer(output) failed");
                return false;
            }

            VkDescriptorSetAllocateInfo descriptorAlloc{};
            descriptorAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            descriptorAlloc.descriptorPool = m_descriptorPool;
            descriptorAlloc.descriptorSetCount = 1;
            descriptorAlloc.pSetLayouts = &m_descriptorSetLayout;
            if (m_allocateDescriptorSets(m_context->GetDevice(), &descriptorAlloc,
                                         &target.resolveDescriptorSets[viewIndex]) != VK_SUCCESS)
            {
                SetError("vkAllocateDescriptorSets(output) failed");
                return false;
            }
            VkDescriptorImageInfo imageInfo{};
            imageInfo.sampler = m_gameSampler;
            imageInfo.imageView = target.color[viewIndex].view;
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet descriptorWrite{};
            descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrite.dstSet = target.resolveDescriptorSets[viewIndex];
            descriptorWrite.dstBinding = 0;
            descriptorWrite.descriptorCount = 1;
            descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrite.pImageInfo = &imageInfo;
            m_updateDescriptorSets(m_context->GetDevice(), 1, &descriptorWrite, 0, nullptr);
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

float VulkanFrameRenderer::GetHeadRotationDeltaRadians() const
{
    if (!m_referenceHeadPoseValid || !m_frame.viewsValid || m_frame.viewCount == 0)
        return 0.0f;

    XrQuaternionf relative{};
    if (!GetOpenXrRelativeOrientation(m_referenceHeadPose.orientation,
                                     m_frame.views[0].pose.orientation, relative))
        return 0.0f;
    return 2.0f * std::acos(std::fmin(1.0f, std::fabs(relative.w)));
}

float VulkanFrameRenderer::GetHeadYawDeltaRadians() const
{
    if (!m_referenceHeadPoseValid || !m_frame.viewsValid || m_frame.viewCount == 0)
        return 0.0f;
    float yaw = 0.0f, pitch = 0.0f;
    return GetHeadGazeDeltas(m_referenceHeadPose.orientation,
                             m_frame.views[0].pose.orientation, yaw, pitch) ? yaw : 0.0f;
}

float VulkanFrameRenderer::GetHeadPitchDeltaRadians() const
{
    if (!m_referenceHeadPoseValid || !m_frame.viewsValid || m_frame.viewCount == 0)
        return 0.0f;
    float yaw = 0.0f, pitch = 0.0f;
    return GetHeadGazeDeltas(m_referenceHeadPose.orientation,
                             m_frame.views[0].pose.orientation, yaw, pitch) ? pitch : 0.0f;
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
    ++m_frameBeginAttempts;
    static uint32_t frameLifecycleAuditCount = 0;
    const uint32_t auditCall = frameLifecycleAuditCount++;
    if (auditCall < 64)
        CryLogAlways("OpenXR/Vulkan audit: BeginFrame enter call=%u instance=%p tid=%llu initialized=%u active=%u pending=%u",
            auditCall, static_cast<void*>(this), static_cast<unsigned long long>(GetAuditThreadId()),
            m_initialized ? 1u : 0u, m_frameActive ? 1u : 0u,
            m_frameSubmissionPending ? 1u : 0u);
    if (!m_initialized || m_frameActive)
    {
        if (auditCall < 64)
            CryLogAlways("OpenXR/Vulkan audit: BeginFrame rejected call=%u initialized=%u active=%u",
                auditCall, m_initialized ? 1u : 0u, m_frameActive ? 1u : 0u);
        return false;
    }
    if (m_frameSubmissionPending)
    {
        if (m_waitForFences(m_context->GetDevice(), 1, &m_frameFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        {
            SetError("waiting for the previous Vulkan frame before recycling dynamic buffers failed");
            return false;
        }
        m_frameSubmissionPending = false;
    }
    CollectDeferredLegacyTextureReleases();
    m_visibilityResults.clear();
    if (m_visibilityQueriesEnabled && m_visibilityQueryPool && !m_previousVisibilityQueries.empty())
    {
        for (size_t i = 0; i < m_previousVisibilityQueries.size(); ++i)
        {
            uint64_t samplesPassed = 0;
            const VisibilityQuery& query = m_previousVisibilityQueries[i];
            const VkResult queryResult = m_getQueryPoolResults(m_context->GetDevice(),
                m_visibilityQueryPool, query.index, 1, sizeof(samplesPassed), &samplesPassed,
                sizeof(samplesPassed), VK_QUERY_RESULT_64_BIT);
            if (queryResult == VK_SUCCESS)
            {
                VisibilitySamples& samples = m_visibilityResults[query.key];
                if (query.totalCoverage)
                {
                    samples.totalSamples = samplesPassed;
                    samples.hasTotalSamples = true;
                }
                else
                {
                    samples.visibleSamples = samplesPassed;
                    samples.hasVisibleSamples = true;
                }
            }
        }
    }
    m_previousVisibilityQueries.clear();
    m_currentVisibilityQueries.clear();
    m_currentVisibilityQueryByKey.clear();
    m_pendingVisibilityKey = 0;
    m_pendingVisibilityTotalCoverage = false;
    for (size_t i = 0; i < m_previousDynamicVertexBuffers.size(); ++i)
        m_resources->DestroyBuffer(m_previousDynamicVertexBuffers[i]);
    m_previousDynamicVertexBuffers.clear();
    for (size_t i = 0; i < m_previousDynamicIndexBuffers.size(); ++i)
        m_resources->DestroyBuffer(m_previousDynamicIndexBuffers[i]);
    m_previousDynamicIndexBuffers.clear();
    for (size_t i = 0; i < m_previousTextureTransformBuffers.size(); ++i)
        m_resources->DestroyBuffer(m_previousTextureTransformBuffers[i]);
    m_previousTextureTransformBuffers.clear();
    if (!m_runtime->BeginFrame(m_frame))
    {
        if (auditCall < 64)
            CryLogAlways("OpenXR/Vulkan audit: BeginFrame runtime failed call=%u active=%u render=%u views=%u/%u error=%s",
                auditCall, m_frameActive ? 1u : 0u, m_frame.shouldRender ? 1u : 0u,
                m_frame.viewsValid ? 1u : 0u, m_frame.viewCount, m_runtime->GetLastError());
        return false;
    }
    m_frameActive = true;
    ++m_frameBeginSuccesses;
    if (auditCall < 64)
        CryLogAlways("OpenXR/Vulkan audit: BeginFrame runtime ok call=%u active=%u render=%u views=%u/%u",
            auditCall, m_frameActive ? 1u : 0u, m_frame.shouldRender ? 1u : 0u,
            m_frame.viewsValid ? 1u : 0u, m_frame.viewCount);
    m_stockDraws.clear();
    m_stockFogRangeScale = 1.0f;
    m_panelImages.clear();
    m_stockScissorEnabled = false;
    m_stockScissor = VkRect2D{};
    m_reusableClientGeometry.valid = false;
    m_untranslatedDrawCount = 0;
    m_sceneDiagnostics = VulkanSceneDiagnostics{};
    m_dynamicVertexUsed = 0;
    m_dynamicIndexUsed = 0;
    if (m_frame.viewsValid && m_frame.viewCount >= 2 && !m_referenceHeadPoseValid)
    {
        m_referenceHeadPose = m_frame.views[0].pose;
        if (m_frame.viewPositionsValid)
        {
            m_referenceHeadPose.position.x = 0.5f * (m_frame.views[0].pose.position.x + m_frame.views[1].pose.position.x);
            m_referenceHeadPose.position.y = 0.5f * (m_frame.views[0].pose.position.y + m_frame.views[1].pose.position.y);
            m_referenceHeadPose.position.z = 0.5f * (m_frame.views[0].pose.position.z + m_frame.views[1].pose.position.z);
            const float qLength = std::sqrt(
                m_referenceHeadPose.orientation.x*m_referenceHeadPose.orientation.x +
                m_referenceHeadPose.orientation.y*m_referenceHeadPose.orientation.y +
                m_referenceHeadPose.orientation.z*m_referenceHeadPose.orientation.z +
                m_referenceHeadPose.orientation.w*m_referenceHeadPose.orientation.w);
            if (qLength > 0.0f && std::isfinite(qLength))
            {
                const XrQuaternionf inverseReference{
                    -m_referenceHeadPose.orientation.x / qLength,
                    -m_referenceHeadPose.orientation.y / qLength,
                    -m_referenceHeadPose.orientation.z / qLength,
                     m_referenceHeadPose.orientation.w / qLength};
                for (uint32_t eye = 0; eye < 2; ++eye)
                {
                    const XrVector3f worldOffset{
                        m_frame.views[eye].pose.position.x - m_referenceHeadPose.position.x,
                        m_frame.views[eye].pose.position.y - m_referenceHeadPose.position.y,
                        m_frame.views[eye].pose.position.z - m_referenceHeadPose.position.z};
                    m_referenceEyeOffsets[eye] = RotateVector(inverseReference, worldOffset);
                }
            }
        }
        else
        {
            m_referenceHeadPose.position.x = 0.0f;
            m_referenceHeadPose.position.y = 0.0f;
            m_referenceHeadPose.position.z = 0.0f;
        }
        if (!m_frame.viewPositionsValid)
        {
            // OpenXR view positions are unavailable in 3DoF mode. Keep IPD
            // in head-local coordinates and rotate it with each current pose.
            m_referenceEyeOffsets[0] = XrVector3f{-0.032f, 0.0f, 0.0f};
            m_referenceEyeOffsets[1] = XrVector3f{ 0.032f, 0.0f, 0.0f};
        }
        m_referenceHeadPoseValid = true;
    }
    if (m_frame.viewsValid && m_referenceHeadPoseValid && !m_frame.viewPositionsValid)
    {
        // Keep composition layers and flat UI on the same fixed 3DoF origin
        // as the scene when the runtime reports orientation without position.
        for (uint32_t eye = 0; eye < 2; ++eye)
        {
            const XrVector3f eyeOffset = RotateVector(m_frame.views[eye].pose.orientation,
                                                       m_referenceEyeOffsets[eye]);
            m_frame.views[eye].pose.position.x = m_referenceHeadPose.position.x + eyeOffset.x;
            m_frame.views[eye].pose.position.y = m_referenceHeadPose.position.y + eyeOffset.y;
            m_frame.views[eye].pose.position.z = m_referenceHeadPose.position.z + eyeOffset.z;
        }
    }
    if (m_frame.viewsValid && m_frame.viewCount >= 2)
    {
        ++m_eyeTransformAuditFrame;
        if (m_eyeTransformAuditFrame <= 4 || (m_eyeTransformAuditFrame % 120u) == 0u)
        {
            const XrView& left = m_frame.views[0];
            const XrView& right = m_frame.views[1];
            CryLogAlways("OpenXR/Vulkan eye audit frame=%u extent=%ux%u refP=(%.6f,%.6f,%.6f) refQ=(%.6f,%.6f,%.6f,%.6f) leftP=(%.6f,%.6f,%.6f) leftQ=(%.6f,%.6f,%.6f,%.6f) leftFov=(%.6f,%.6f,%.6f,%.6f) rightP=(%.6f,%.6f,%.6f) rightQ=(%.6f,%.6f,%.6f,%.6f) rightFov=(%.6f,%.6f,%.6f,%.6f)",
                m_eyeTransformAuditFrame, m_swapchain.width, m_swapchain.height,
                m_referenceHeadPose.position.x, m_referenceHeadPose.position.y,
                m_referenceHeadPose.position.z, m_referenceHeadPose.orientation.x,
                m_referenceHeadPose.orientation.y, m_referenceHeadPose.orientation.z,
                m_referenceHeadPose.orientation.w,
                left.pose.position.x, left.pose.position.y, left.pose.position.z,
                left.pose.orientation.x, left.pose.orientation.y, left.pose.orientation.z,
                left.pose.orientation.w, left.fov.angleLeft, left.fov.angleRight,
                left.fov.angleDown, left.fov.angleUp,
                right.pose.position.x, right.pose.position.y, right.pose.position.z,
                right.pose.orientation.x, right.pose.orientation.y, right.pose.orientation.z,
                right.pose.orientation.w, right.fov.angleLeft, right.fov.angleRight,
                right.fov.angleDown, right.fov.angleUp);
        }
    }
    if (!m_frame.shouldRender || m_frame.viewCount == 0)
        return true;
    if (!m_runtime->AcquireSwapchainImage(m_swapchain, m_imageIndex))
    {
        if (auditCall < 64)
            CryLogAlways("OpenXR/Vulkan audit: BeginFrame acquire failed call=%u error=%s",
                auditCall, m_runtime->GetLastError());
        m_runtime->EndFrame(nullptr, 0);
        m_frameActive = false;
        return false;
    }
    m_imageAcquired = true;
    if (!m_runtime->WaitSwapchainImage(m_swapchain))
    {
        if (auditCall < 64)
            CryLogAlways("OpenXR/Vulkan audit: BeginFrame wait image failed call=%u error=%s",
                auditCall, m_runtime->GetLastError());
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

bool VulkanFrameRenderer::GetStockVisibility(uint64_t key, bool& visible) const
{
    std::map<uint64_t, VisibilitySamples>::const_iterator found = m_visibilityResults.find(key);
    if (found == m_visibilityResults.end() || !found->second.hasVisibleSamples)
        return false;
    visible = found->second.visibleSamples != 0;
    return true;
}

bool VulkanFrameRenderer::GetStockVisibilityFraction(uint64_t key, float& fraction) const
{
    std::map<uint64_t, VisibilitySamples>::const_iterator found = m_visibilityResults.find(key);
    if (found == m_visibilityResults.end() || !found->second.hasVisibleSamples ||
        !found->second.hasTotalSamples || found->second.totalSamples == 0)
        return false;
    fraction = static_cast<float>(static_cast<double>(found->second.visibleSamples) /
                                  static_cast<double>(found->second.totalSamples));
    if (fraction > 1.0f)
        fraction = 1.0f;
    return true;
}

bool VulkanFrameRenderer::RecordAndSubmit(uint32_t viewIndex)
{
    const uint32_t zeroTextureTransformOffset = 0;
    VkCommandBuffer commandBuffer = m_commandBuffers[m_imageIndex * m_viewCount + viewIndex];
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (m_beginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS)
        return false;

    if (viewIndex == 0 && m_visibilityQueriesEnabled && m_visibilityQueryPool &&
        !m_currentVisibilityQueries.empty())
        m_cmdResetQueryPool(commandBuffer, m_visibilityQueryPool, 0,
                            static_cast<uint32_t>(m_currentVisibilityQueries.size()));

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
    barrier.image = m_targets[m_imageIndex].color[viewIndex].image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    m_cmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkClearValue clear{};
    clear.color.float32[0] = m_stockClearColor[0];
    clear.color.float32[1] = m_stockClearColor[1];
    clear.color.float32[2] = m_stockClearColor[2];
    // CGLRenderer::EF_ClearBuffers clears the scene with alpha zero; keep
    // that destination alpha for fixed-function blend operations.
    clear.color.float32[3] = 0.0f;
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
                                    0, 1, &m_descriptorSet, 1, &zeroTextureTransformOffset);
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
            {
                ++m_sceneDiagnostics.emptyScissor;
                continue;
            }
            const VkRect2D drawScissor = draw.scissorEnabled ? draw.scissor : scissor;
            m_cmdSetScissor(commandBuffer, 0, 1, &drawScissor);
            if (!(draw.viewport.width > 0.0f) || !(draw.viewport.height > 0.0f))
                continue;
            m_cmdSetViewport(commandBuffer, 0, 1, &draw.viewport);
            VkPipeline pipeline = draw.pipeline;
            VkDescriptorSet textureSets[8]{};
            if (draw.textureId)
            {
                std::map<int, LegacyTexture>::const_iterator texture = m_legacyTextures.find(draw.textureId);
                if (texture == m_legacyTextures.end())
                {
                    ++m_sceneDiagnostics.missingTextureAtRecord;
                    continue;
                }
                textureSets[0] = GetLegacyTextureDescriptorSet(
                    draw.textureId, draw.textureWrapMode[0]);
                if (!textureSets[0])
                {
                    ++m_sceneDiagnostics.missingTextureAtRecord;
                    continue;
                }
                textureSets[1] = textureSets[0];
                textureSets[2] = textureSets[0];
                textureSets[3] = textureSets[0];
        for (uint32_t stageIndex = 0; stageIndex < 4; ++stageIndex)
                    textureSets[stageIndex + 4] = textureSets[0];
            }
            if (draw.useSecondTexture || draw.useNormalMap)
            {
                const int secondaryTextureId = draw.useNormalMap ? draw.normalMapTextureId : draw.textureId1;
                std::map<int, LegacyTexture>::const_iterator texture = m_legacyTextures.find(secondaryTextureId);
                if (texture == m_legacyTextures.end())
                {
                    ++m_sceneDiagnostics.missingTextureAtRecord;
                    continue;
                }
                textureSets[1] = GetLegacyTextureDescriptorSet(
                    secondaryTextureId, draw.textureWrapMode[1]);
                if (!textureSets[1])
                {
                    ++m_sceneDiagnostics.missingTextureAtRecord;
                    continue;
                }
            }
            if (draw.useThirdTexture)
            {
                std::map<int, LegacyTexture>::const_iterator texture =
                    m_legacyTextures.find(draw.textureStage2.textureId);
                if (texture == m_legacyTextures.end())
                {
                    ++m_sceneDiagnostics.missingTextureAtRecord;
                    continue;
                }
                textureSets[2] = GetLegacyTextureDescriptorSet(
                    draw.textureStage2.textureId, draw.textureWrapMode[2]);
                if (!textureSets[2])
                {
                    ++m_sceneDiagnostics.missingTextureAtRecord;
                    continue;
                }
            }
            if (draw.useFourthTexture)
            {
                std::map<int, LegacyTexture>::const_iterator texture =
                    m_legacyTextures.find(draw.textureStage3.textureId);
                if (texture == m_legacyTextures.end())
                {
                    ++m_sceneDiagnostics.missingTextureAtRecord;
                    continue;
                }
                textureSets[3] = GetLegacyTextureDescriptorSet(
                    draw.textureStage3.textureId, draw.textureWrapMode[3]);
                if (!textureSets[3])
                {
                    ++m_sceneDiagnostics.missingTextureAtRecord;
                    continue;
                }
            }
            uint32_t textureSetCount = draw.textureId ? 1u : 0u;
            const bool usesEightStageShader = draw.useFourthTexture || draw.useTextureStages4To7[0] ||
                draw.useTextureStages4To7[1] || draw.useTextureStages4To7[2] ||
                draw.useTextureStages4To7[3];
            bool missingExtraTexture = false;
            if (draw.useSecondTexture || draw.useNormalMap) textureSetCount = 2;
            if (draw.useThirdTexture) textureSetCount = 3;
            if (draw.useFourthTexture) textureSetCount = 4;
            for (uint32_t stageIndex = 0; stageIndex < 4; ++stageIndex)
            {
                if (!draw.useTextureStages4To7[stageIndex])
                    continue;
                const VulkanStockTextureStage& stage = draw.textureStages4To7[stageIndex];
                std::map<int, LegacyTexture>::const_iterator texture = m_legacyTextures.find(stage.textureId);
                if (texture == m_legacyTextures.end())
                {
                    ++m_sceneDiagnostics.missingTextureAtRecord;
                    missingExtraTexture = true;
                    break;
                }
                textureSets[stageIndex + 4] = GetLegacyTextureDescriptorSet(stage.textureId, stage.wrapMode);
                if (!textureSets[stageIndex + 4])
                {
                    ++m_sceneDiagnostics.missingTextureAtRecord;
                    missingExtraTexture = true;
                    break;
                }
            textureSetCount = stageIndex + 5;
        }
            if (draw.projectorCookieEnabled)
            {
                textureSets[0] = GetProjectorTextureDescriptorSet(
                    draw.textureId, draw.projectorCookieTextureId, draw.textureWrapMode[0]);
                if (!textureSets[0])
                {
                    ++m_sceneDiagnostics.missingTextureAtRecord;
                    continue;
                }
                if (textureSetCount == 0)
                    textureSetCount = 1;
            }
            if (usesEightStageShader)
                textureSetCount = 8;
            if (missingExtraTexture)
                continue;
            float pushConstants[32];
            float* mvp = pushConstants;
            float* eyeModelView = pushConstants + 16;
            if (!pipeline)
            {
                ++m_sceneDiagnostics.nullPipelineAtRecord;
                continue;
            }
            // Three degree of freedom camera: keep current head orientation,
            // but anchor translation to the initial head position. Rebuild each
            // eye around that anchor to preserve stereo separation (IPD).
            XrView cameraView = m_frame.views[viewIndex];
            float eyeOffsetX = m_referenceEyeOffsets[viewIndex].x;
            float eyeOffsetY = m_referenceEyeOffsets[viewIndex].y;
            float eyeOffsetZ = m_referenceEyeOffsets[viewIndex].z;
            if (m_frame.viewPositionsValid)
            {
                const float headX = 0.5f * (m_frame.views[0].pose.position.x + m_frame.views[1].pose.position.x);
                const float headY = 0.5f * (m_frame.views[0].pose.position.y + m_frame.views[1].pose.position.y);
                const float headZ = 0.5f * (m_frame.views[0].pose.position.z + m_frame.views[1].pose.position.z);
                eyeOffsetX = cameraView.pose.position.x - headX;
                eyeOffsetY = cameraView.pose.position.y - headY;
                eyeOffsetZ = cameraView.pose.position.z - headZ;
            }
            else
            {
                const XrVector3f eyeOffset = RotateVector(cameraView.pose.orientation,
                                                           m_referenceEyeOffsets[viewIndex]);
                eyeOffsetX = eyeOffset.x;
                eyeOffsetY = eyeOffset.y;
                eyeOffsetZ = eyeOffset.z;
            }
            cameraView.pose.position.x = m_referenceHeadPose.position.x + eyeOffsetX;
            cameraView.pose.position.y = m_referenceHeadPose.position.y + eyeOffsetY;
            cameraView.pose.position.z = m_referenceHeadPose.position.z + eyeOffsetZ;
            if (!BuildOpenXrEyeMvp(cameraView, m_referenceHeadPose,
                                   draw.modelView, draw.nearPlane, draw.farPlane,
                                   mvp, eyeModelView, draw.nearestObject))
            {
                ++m_sceneDiagnostics.invalidEyeTransform;
                continue;
            }
            if (i == 0 && (m_eyeTransformAuditFrame <= 4 ||
                           (m_eyeTransformAuditFrame % 120u) == 0u))
                CryLogAlways("OpenXR/Vulkan MVP audit frame=%u eye=%u near=%.6f far=%.6f nearest=%u mvpC0=(%.6f,%.6f,%.6f,%.6f) mvpC1=(%.6f,%.6f,%.6f,%.6f) mvpC2=(%.6f,%.6f,%.6f,%.6f) mvpC3=(%.6f,%.6f,%.6f,%.6f)",
                    m_eyeTransformAuditFrame, viewIndex, draw.nearPlane, draw.farPlane,
                    draw.nearestObject ? 1u : 0u,
                    mvp[0], mvp[1], mvp[2], mvp[3],
                    mvp[4], mvp[5], mvp[6], mvp[7],
                    mvp[8], mvp[9], mvp[10], mvp[11],
                    mvp[12], mvp[13], mvp[14], mvp[15]);
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
            if (textureSetCount)
            {
                uint32_t transformOffsets[8]{};
                for (uint32_t setIndex = 0; setIndex < textureSetCount; ++setIndex)
                    transformOffsets[setIndex] = draw.textureTransformOffset +
                        static_cast<uint32_t>(viewIndex * m_textureTransformStride);
                m_cmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_scenePipelineLayout, 0,
                                        textureSetCount,
                                        textureSets, textureSetCount, transformOffsets);
            }
            else
            {
                const uint32_t transformOffset = draw.textureTransformOffset +
                    static_cast<uint32_t>(viewIndex * m_textureTransformStride);
                m_cmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_scenePipelineLayout, 0, 1, &m_descriptorSet,
                                        1, &transformOffset);
            }
            m_cmdPushConstants(commandBuffer, m_scenePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(pushConstants), pushConstants);
            const VkDeviceSize offsets[3] = {
                draw.vertexBufferOffset, 0, draw.lightmapTexCoordOffset
            };
            if (draw.tangentBuffer || draw.lightmapTexCoordBuffer)
            {
                const VkBuffer vertexBuffers[3] = {
                    draw.vertexBuffer,
                    draw.tangentBuffer ? draw.tangentBuffer : draw.vertexBuffer,
                    draw.lightmapTexCoordBuffer ? draw.lightmapTexCoordBuffer : draw.vertexBuffer
                };
                const uint32_t bindingCount = draw.lightmapTexCoordBuffer ? 3u : 2u;
                m_cmdBindVertexBuffers(commandBuffer, 0, bindingCount, vertexBuffers, offsets);
            }
            else
                m_cmdBindVertexBuffers(commandBuffer, 0, 1, &draw.vertexBuffer, offsets);
            m_cmdBindIndexBuffer(commandBuffer, draw.indexBuffer, 0, VK_INDEX_TYPE_UINT16);
            const bool collectVisibility = viewIndex == 0 && draw.visibilityQueryIndex != UINT32_MAX &&
                                           m_visibilityQueryPool != VK_NULL_HANDLE;
            const bool collectCoverage = viewIndex == 0 &&
                draw.visibilityCoverageQueryIndex != UINT32_MAX &&
                m_visibilityQueryPool != VK_NULL_HANDLE;
            if (collectVisibility)
                m_cmdBeginQuery(commandBuffer, m_visibilityQueryPool,
                                draw.visibilityQueryIndex, 0);
            if (collectCoverage)
                m_cmdBeginQuery(commandBuffer, m_visibilityQueryPool,
                                draw.visibilityCoverageQueryIndex, 0);
            m_cmdDrawIndexed(commandBuffer, draw.indexCount, 1, draw.firstIndex, draw.vertexOffset, 0);
            if (collectVisibility)
                m_cmdEndQuery(commandBuffer, m_visibilityQueryPool, draw.visibilityQueryIndex);
            if (collectCoverage)
                m_cmdEndQuery(commandBuffer, m_visibilityQueryPool,
                              draw.visibilityCoverageQueryIndex);
            ++m_sceneDiagnostics.recordedEyeDraws;
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
        float panelAspect = static_cast<float>(m_swapchain.width) /
                            static_cast<float>(m_swapchain.height);
        if (!m_panelImages.empty())
        {
            const PanelImage& firstPanelImage = m_panelImages.front();
            const float logicalAspect = firstPanelImage.logicalWidth / firstPanelImage.logicalHeight;
            if (std::isfinite(logicalAspect) && logicalAspect > 0.0f)
                panelAspect = logicalAspect;
        }
        float baseMvp[16];
        if (BuildFlatPanelMvp(m_frame.views[0], m_frame.views[1], viewIndex,
                              panelAspect, 1.8f, baseMvp))
        {
            for (size_t i = 0; i < m_panelImages.size(); ++i)
            {
                const PanelImage& image = m_panelImages[i];
                m_cmdSetScissor(commandBuffer, 0, 1,
                    image.scissorEnabled ? &image.scissor : &uiScissor);
                std::map<int, LegacyTexture>::const_iterator texture = m_legacyTextures.find(image.textureId);
                if (texture == m_legacyTextures.end() || !texture->second.descriptorSet)
                    continue;
                if (image.indexedGeometry)
                {
                    float constants[24] = {};
                    memcpy(constants, baseMvp, sizeof(float) * 16);
                    constants[16] = constants[17] = 1.0f;
                    constants[20] = constants[21] = constants[22] = constants[23] = 1.0f;
                    const VkPipeline geometryPipeline = GetPanelPipeline(image.blendState, true);
                    if (!geometryPipeline)
                    {
                        ++m_sceneDiagnostics.nullPipelineAtRecord;
                        continue;
                    }
                    m_cmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      geometryPipeline);
                    m_cmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            m_pipelineLayout, 0, 1,
                                            &texture->second.descriptorSet, 1,
                                            &zeroTextureTransformOffset);
                    m_cmdPushConstants(commandBuffer, m_pipelineLayout,
                                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                       0, sizeof(constants), constants);
                    const VkDeviceSize offsets[2] = { image.vertexBufferOffset, 0 };
                    m_cmdBindVertexBuffers(commandBuffer, 0, 1, &image.vertexBuffer, offsets);
                    m_cmdBindIndexBuffer(commandBuffer, image.indexBuffer, 0,
                                         VK_INDEX_TYPE_UINT16);
                    m_cmdDrawIndexed(commandBuffer, image.indexCount, 1,
                                     image.firstIndex, 0, 0);
                    continue;
                }
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
                const VkPipeline panelPipeline = GetPanelPipeline(image.blendState, false);
                if (!panelPipeline)
                {
                    ++m_sceneDiagnostics.nullPipelineAtRecord;
                    continue;
                }
                m_cmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, panelPipeline);
                m_cmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        m_pipelineLayout, 0, 1, &texture->second.descriptorSet, 1,
                                        &zeroTextureTransformOffset);
                m_cmdPushConstants(commandBuffer, m_pipelineLayout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(constants), constants);
                m_cmdDraw(commandBuffer, 6, 1, 0, 0);
            }
        }
    }
    m_cmdEndRenderPass(commandBuffer);

    VkImageMemoryBarrier sampleBarrier{};
    sampleBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    sampleBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    sampleBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    sampleBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    sampleBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sampleBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sampleBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sampleBarrier.image = m_targets[m_imageIndex].color[viewIndex].image;
    sampleBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    sampleBarrier.subresourceRange.levelCount = 1;
    sampleBarrier.subresourceRange.layerCount = 1;
    m_cmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &sampleBarrier);

    VkImageMemoryBarrier outputBarrier{};
    outputBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    outputBarrier.srcAccessMask = 0;
    outputBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    outputBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    outputBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    outputBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputBarrier.image = m_targets[m_imageIndex].image;
    outputBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    outputBarrier.subresourceRange.levelCount = 1;
    outputBarrier.subresourceRange.baseArrayLayer = viewIndex;
    outputBarrier.subresourceRange.layerCount = 1;
    m_cmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &outputBarrier);

    VkClearValue outputClear{};
    outputClear.color.float32[0] = m_stockClearColor[0];
    outputClear.color.float32[1] = m_stockClearColor[1];
    outputClear.color.float32[2] = m_stockClearColor[2];
    outputClear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo outputRenderPassBegin{};
    outputRenderPassBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    outputRenderPassBegin.renderPass = m_outputRenderPass;
    outputRenderPassBegin.framebuffer = m_targets[m_imageIndex].outputFramebuffers[viewIndex];
    outputRenderPassBegin.renderArea.extent.width = m_swapchain.width;
    outputRenderPassBegin.renderArea.extent.height = m_swapchain.height;
    outputRenderPassBegin.clearValueCount = 1;
    outputRenderPassBegin.pClearValues = &outputClear;
    m_cmdBeginRenderPass(commandBuffer, &outputRenderPassBegin, VK_SUBPASS_CONTENTS_INLINE);
    m_cmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_outputPipeline);
    m_cmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            m_pipelineLayout, 0, 1,
                            &m_targets[m_imageIndex].resolveDescriptorSets[viewIndex],
                            0, nullptr);
    m_cmdDraw(commandBuffer, 3, 1, 0, 0);
    m_cmdEndRenderPass(commandBuffer);
    return m_endCommandBuffer(commandBuffer) == VK_SUCCESS;
}

bool VulkanFrameRenderer::EndFrame()
{
    ++m_frameEndCalls;
    static uint32_t frameLifecycleEndAuditCount = 0;
    const uint32_t auditCall = frameLifecycleEndAuditCount++;
    if (auditCall < 64)
        CryLogAlways("OpenXR/Vulkan audit: EndFrame enter call=%u instance=%p tid=%llu active=%u render=%u views=%u/%u acquired=%u queued=%u",
            auditCall, static_cast<void*>(this), static_cast<unsigned long long>(GetAuditThreadId()),
            m_frameActive ? 1u : 0u, m_frame.shouldRender ? 1u : 0u,
            m_frame.viewsValid ? 1u : 0u, m_frame.viewCount, m_imageAcquired ? 1u : 0u,
            static_cast<uint32_t>(m_stockDraws.size()));
    if (!m_frameActive)
        return false;
    m_lastError[0] = '\0';
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
            if (!result)
                break;
            if (!RecordAndSubmit(viewIndex))
            {
                SetError("recording Vulkan commands for an XR eye failed");
                result = false;
            }
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
            const VkResult submitResult = fenceReset ?
                m_queueSubmit(m_context->GetGraphicsQueue(), 1, &submit, m_frameFence) : VK_ERROR_UNKNOWN;
            if (!fenceReset || submitResult != VK_SUCCESS)
            {
                SetError(fenceReset ? "vkQueueSubmit failed for an XR eye" : "vkResetFences failed for an XR frame");
                result = false;
            }
            else
            {
                m_previousVisibilityQueries = m_currentVisibilityQueries;
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
                {
                    SetError("waiting for XR eye rendering to finish failed");
                    result = false;
                }
            }
            if (queueCompleted && firstViewRecorded)
                for (std::map<int, LegacyTexture>::iterator it = m_legacyTextures.begin();
                     it != m_legacyTextures.end(); ++it)
                    it->second.uploadPending = false;
            if (!queueCompleted)
                result = false;
        }
        if (commandCount != renderViewCount)
        {
            if (!m_lastError[0]) SetError("not all XR eye command buffers were submitted");
            result = false;
        }
        if (!m_runtime->ReleaseSwapchainImage(m_swapchain))
        {
            SetError(m_runtime->GetLastError());
            result = false;
        }

        if (!m_frame.viewsValid || m_frame.viewCount < 2)
        {
            if (!m_runtime->EndFrame(nullptr, 0))
            {
                SetError(m_runtime->GetLastError());
                result = false;
            }
            m_imageAcquired = false;
            m_frameActive = false;
            return result;
        }

        if (!result)
        {
            if (!m_runtime->EndFrame(nullptr, 0))
            {
                if (!m_lastError[0]) SetError(m_runtime->GetLastError());
                result = false;
            }
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
        if (!m_runtime->EndFrame(&layer, 1))
        {
            SetError(m_runtime->GetLastError());
            result = false;
        }
    }
    else
    {
        result = m_runtime->EndFrame(nullptr, 0);
        if (!result)
            SetError(m_runtime->GetLastError());
    }
    m_imageAcquired = false;
    m_frameActive = false;
    if (auditCall < 64)
        CryLogAlways("OpenXR/Vulkan audit: EndFrame exit call=%u result=%u error=%s",
            auditCall, result ? 1u : 0u, m_lastError);
    return result;
}

void VulkanFrameRenderer::Shutdown()
{
    if (m_context && m_context->IsInitialized())
    {
        if (m_frameSubmissionPending && m_frameFence && m_waitForFences)
            m_waitForFences(m_context->GetDevice(), 1, &m_frameFence, VK_TRUE, UINT64_MAX);
        CollectDeferredLegacyTextureReleases();
        if (m_visibilityQueryPool && m_destroyQueryPool)
            m_destroyQueryPool(m_context->GetDevice(), m_visibilityQueryPool, nullptr);
        if (m_commandBuffers.size() && m_freeCommandBuffers && m_commandPool)
            m_freeCommandBuffers(m_context->GetDevice(), m_commandPool,
                                 static_cast<uint32_t>(m_commandBuffers.size()), m_commandBuffers.data());
        if (m_frameFence && m_destroyFence)
            m_destroyFence(m_context->GetDevice(), m_frameFence, nullptr);
        for (Target& target : m_targets)
        {
            for (uint32_t viewIndex = 0; viewIndex < m_viewCount; ++viewIndex)
            {
                if (target.outputFramebuffers[viewIndex] && m_destroyFramebuffer)
                    m_destroyFramebuffer(m_context->GetDevice(), target.outputFramebuffers[viewIndex], nullptr);
                if (target.framebuffers[viewIndex] && m_destroyFramebuffer)
                    m_destroyFramebuffer(m_context->GetDevice(), target.framebuffers[viewIndex], nullptr);
                if (target.views[viewIndex] && m_destroyImageView)
                    m_destroyImageView(m_context->GetDevice(), target.views[viewIndex], nullptr);
                if (m_resources && target.color[viewIndex].image)
                    m_resources->DestroyTexture(target.color[viewIndex]);
                if (m_resources && target.depth[viewIndex].image)
                    m_resources->DestroyTexture(target.depth[viewIndex]);
            }
        }
        if (m_panelPipeline && m_destroyPipeline)
            m_destroyPipeline(m_context->GetDevice(), m_panelPipeline, nullptr);
        if (m_panelBlendPipeline && m_destroyPipeline)
            m_destroyPipeline(m_context->GetDevice(), m_panelBlendPipeline, nullptr);
        if (m_panelAdditivePipeline && m_destroyPipeline)
            m_destroyPipeline(m_context->GetDevice(), m_panelAdditivePipeline, nullptr);
        if (m_panelGeometryPipeline && m_destroyPipeline)
            m_destroyPipeline(m_context->GetDevice(), m_panelGeometryPipeline, nullptr);
        if (m_panelGeometryOpaquePipeline && m_destroyPipeline)
            m_destroyPipeline(m_context->GetDevice(), m_panelGeometryOpaquePipeline, nullptr);
        if (m_panelGeometryAdditivePipeline && m_destroyPipeline)
            m_destroyPipeline(m_context->GetDevice(), m_panelGeometryAdditivePipeline, nullptr);
        if (m_outputPipeline && m_destroyPipeline)
            m_destroyPipeline(m_context->GetDevice(), m_outputPipeline, nullptr);
        for (std::map<uint32_t, VkPipeline>::iterator it = m_panelPipelineCache.begin();
             it != m_panelPipelineCache.end(); ++it)
            if (it->second && m_destroyPipeline)
                m_destroyPipeline(m_context->GetDevice(), it->second, nullptr);
    for (std::map<std::array<uint32_t, 61>, VkPipeline>::iterator it = m_scenePipelineCache.begin();
             it != m_scenePipelineCache.end(); ++it)
            if (it->second && m_destroyPipeline)
                m_destroyPipeline(m_context->GetDevice(), it->second, nullptr);
        if (m_resources)
            for (std::map<int, LegacyTexture>::iterator it = m_legacyTextures.begin();
             it != m_legacyTextures.end(); ++it)
            {
                ReleaseLegacyTextureWrapVariants(it->second);
                if (it->second.sampler && m_destroySampler)
                    m_destroySampler(m_context->GetDevice(), it->second.sampler, nullptr);
                if (it->second.texture.image)
                    m_resources->DestroyTexture(it->second.texture);
            }
        if (m_resources)
        {
            m_resources->DestroyBuffer(m_dynamicVertexBuffer);
            m_resources->DestroyBuffer(m_dynamicIndexBuffer);
            m_resources->DestroyBuffer(m_textureTransformBuffer);
            for (size_t i = 0; i < m_previousDynamicVertexBuffers.size(); ++i)
                m_resources->DestroyBuffer(m_previousDynamicVertexBuffers[i]);
            for (size_t i = 0; i < m_previousDynamicIndexBuffers.size(); ++i)
                m_resources->DestroyBuffer(m_previousDynamicIndexBuffers[i]);
            for (size_t i = 0; i < m_previousTextureTransformBuffers.size(); ++i)
                m_resources->DestroyBuffer(m_previousTextureTransformBuffers[i]);
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
        if (m_sceneMultiTextureLitColorVertexShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneMultiTextureLitColorVertexShader, nullptr);
        if (m_sceneMultiTextureLitNoColorVertexShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneMultiTextureLitNoColorVertexShader, nullptr);
        if (m_sceneMultiTextureNoColorVertexShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneMultiTextureNoColorVertexShader, nullptr);
        if (m_sceneMultiTextureColorFragmentShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneMultiTextureColorFragmentShader, nullptr);
        if (m_sceneThreeTextureColorFragmentShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneThreeTextureColorFragmentShader, nullptr);
        if (m_sceneFourTextureColorFragmentShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_sceneFourTextureColorFragmentShader, nullptr);
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
        if (m_panelVertexShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_panelVertexShader, nullptr);
        if (m_panelFragmentShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_panelFragmentShader, nullptr);
        if (m_panelGeometryVertexShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_panelGeometryVertexShader, nullptr);
        if (m_panelGeometryFragmentShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_panelGeometryFragmentShader, nullptr);
        if (m_outputVertexShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_outputVertexShader, nullptr);
        if (m_outputFragmentShader && m_destroyShaderModule)
            m_destroyShaderModule(m_context->GetDevice(), m_outputFragmentShader, nullptr);
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
        if (m_outputRenderPass && m_destroyRenderPass)
            m_destroyRenderPass(m_context->GetDevice(), m_outputRenderPass, nullptr);
        if (m_commandPool && m_destroyCommandPool)
            m_destroyCommandPool(m_context->GetDevice(), m_commandPool, nullptr);
    }
    if (m_resources && m_gameTexture.image)
        m_resources->DestroyTexture(m_gameTexture);
    if (m_runtime && m_swapchain.handle)
        m_runtime->DestroyVulkanSwapchain(m_swapchain);
    m_commandBuffers.clear();
    m_targets.clear();
    m_visibilityQueryPool = VK_NULL_HANDLE;
    m_visibilityQueriesEnabled = false;
    m_currentVisibilityQueries.clear();
    m_previousVisibilityQueries.clear();
    m_currentVisibilityQueryByKey.clear();
    m_visibilityResults.clear();
    m_pendingVisibilityKey = 0;
    m_pendingVisibilityTotalCoverage = false;
    m_renderPass = VK_NULL_HANDLE;
    m_outputRenderPass = VK_NULL_HANDLE;
    m_panelPipeline = VK_NULL_HANDLE;
    m_panelBlendPipeline = VK_NULL_HANDLE;
    m_panelAdditivePipeline = VK_NULL_HANDLE;
    m_panelGeometryPipeline = VK_NULL_HANDLE;
    m_panelGeometryOpaquePipeline = VK_NULL_HANDLE;
    m_panelGeometryAdditivePipeline = VK_NULL_HANDLE;
    m_outputPipeline = VK_NULL_HANDLE;
    m_panelVertexShader = VK_NULL_HANDLE;
    m_panelFragmentShader = VK_NULL_HANDLE;
    m_panelGeometryVertexShader = VK_NULL_HANDLE;
    m_panelGeometryFragmentShader = VK_NULL_HANDLE;
    m_outputVertexShader = VK_NULL_HANDLE;
    m_outputFragmentShader = VK_NULL_HANDLE;
    m_panelPipelineCache.clear();
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
    m_sceneMultiTextureLitColorVertexShader = VK_NULL_HANDLE;
    m_sceneMultiTextureLitNoColorVertexShader = VK_NULL_HANDLE;
    m_sceneMultiTextureNoColorVertexShader = VK_NULL_HANDLE;
    m_sceneMultiTextureColorFragmentShader = VK_NULL_HANDLE;
    m_sceneThreeTextureColorFragmentShader = VK_NULL_HANDLE;
    m_sceneFourTextureColorFragmentShader = VK_NULL_HANDLE;
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
    m_textureTransformStride = 0;
    m_gameSampler = VK_NULL_HANDLE;
    m_gameTextureUploadPending = false;
    m_dynamicVertexUsed = 0;
    m_dynamicIndexUsed = 0;
    m_previousDynamicVertexBuffers.clear();
    m_previousDynamicIndexBuffers.clear();
    m_previousTextureTransformBuffers.clear();
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
    for (uint32_t stage = 0; stage < 8; ++stage)
    {
        m_stockTextureColorModes[stage] = 1u;
        m_stockTextureAlphaModes[stage] = 1u;
        const uint32_t defaultArgs = stage == 0 ? 17u : 25u;
        m_stockTextureColorArgs[stage] = defaultArgs;
        m_stockTextureAlphaArgs[stage] = defaultArgs;
        m_stockTextureEffectiveColorArgs[stage] = 281u;
        m_stockTextureEffectiveAlphaArgs[stage] = 281u;
    }
}
}
