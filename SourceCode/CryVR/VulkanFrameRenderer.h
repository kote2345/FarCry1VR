#ifndef CRY_VR_VULKAN_FRAME_RENDERER_H
#define CRY_VR_VULKAN_FRAME_RENDERER_H

#include "CryVR.h"
#include "VulkanContext.h"
#include "VulkanResourceManager.h"
#include "VulkanPipelineFactory.h"

#include <vulkan/vulkan.h>
#include <vector>
#include <map>
#include <array>

namespace CryVR
{
enum VulkanTextureFilterMode
{
    VulkanFilterNearest = 0,
    VulkanFilterLinear,
    VulkanFilterBilinear,
    VulkanFilterTrilinear
};

// OpenXR projection renderer for native Vulkan scene draws and stereo UI.
class VulkanFrameRenderer
{
public:
    VulkanFrameRenderer() = default;
    ~VulkanFrameRenderer();

    bool Initialize(Runtime& runtime, VulkanContext& context);
    void SetResourceManager(VulkanResourceManager* resources) { m_resources = resources; }
    bool SetGameFrameRGBA(const uint8_t* pixels, uint32_t width, uint32_t height);
    void Shutdown();
    bool BeginFrame();
    bool EndFrame();
    void SetStockClearColor(float red, float green, float blue);
    bool QueueStockClearDepth();
    bool QueueStockClearStencil();
    // Kept as the engine callback name; the NULL-backed Vulkan renderer has no
    // legacy framebuffer to capture, so record the untranslated draw instead.
    void RequirePanelFallback() { if (m_frameActive && m_untranslatedDrawCount < 0xffffffffu) ++m_untranslatedDrawCount; }
    uint32_t GetUntranslatedDrawCount() const { return m_untranslatedDrawCount; }
    bool ShouldCaptureGameFrame() const;
    bool QueueStockIndexedDraw(const VulkanBuffer* vertexBuffer, const VulkanBuffer* indexBuffer,
                               int vertexFormat, uint32_t indexCount, uint32_t firstIndex,
                               int primitiveMode, uint32_t renderState, int cullMode, int textureId, int textureId1,
                               int textureStage0ColorOp, int textureStage0AlphaOp,
                               int textureStage1ColorOp, int textureStage1AlphaOp,
                               uint32_t textureStage0ColorArg, uint32_t textureStage0AlphaArg,
                               uint32_t textureStage0Constant, uint32_t textureStage1ColorArg,
                               uint32_t textureStage1AlphaArg, uint32_t textureStage1Constant,
                               uint32_t stencilState, uint32_t stencilRef, uint32_t stencilMask,
                               const float modelView[16], const float textureMatrix0[16],
                               const float textureMatrix1[16], int32_t vertexOffset = 0,
                               const float* materialLighting = nullptr,
                               float globalOpacity = 1.0f, float alphaTestRef = 0.0f,
                               const VulkanBuffer* tangentBuffer = nullptr,
                               int normalMapTextureId = 0);
    bool QueueStockClientIndexedDraw(const void* vertices, uint32_t vertexCount,
                                     const uint16_t* indices, uint32_t indexCount,
                                     int vertexFormat, int primitiveMode,
                                     uint32_t renderState, int cullMode, int textureId, int textureId1,
                                     int textureStage0ColorOp, int textureStage0AlphaOp,
                                     int textureStage1ColorOp, int textureStage1AlphaOp,
                                     uint32_t textureStage0ColorArg, uint32_t textureStage0AlphaArg,
                                     uint32_t textureStage0Constant, uint32_t textureStage1ColorArg,
                                     uint32_t textureStage1AlphaArg, uint32_t textureStage1Constant,
                                     uint32_t stencilState, uint32_t stencilRef, uint32_t stencilMask,
                                     const float modelView[16], const float textureMatrix0[16],
                                     const float textureMatrix1[16],
                                     const float* materialLighting = nullptr,
                                     bool reusePreviousClientGeometry = false,
                                     float globalOpacity = 1.0f, float alphaTestRef = 0.0f,
                                     const VulkanBuffer* tangentBuffer = nullptr,
                                     int normalMapTextureId = 0);
    bool QueuePanelImage(int textureId, float x, float y, float width, float height,
                         float s0, float t0, float s1, float t1, float angleDegrees,
                         float red, float green, float blue, float alpha,
                         float logicalWidth, float logicalHeight);
    void SetStockScissor(bool enabled, int x, int y, int width, int height,
                         float logicalWidth, float logicalHeight);
    void SetStockFog(bool enabled, float density, float start, float end,
                     const float color[3], int mode);
    bool RegisterLegacyRgbaTexture(int textureId, uint32_t width, uint32_t height,
                                   const uint8_t* rgbaPixels, bool clampU, bool clampV,
                                   bool dynamicTexture, bool noMipmaps = false,
                                   int filterMode = VulkanFilterTrilinear);
    bool RegisterLegacyRgbaTextureRegion(int textureId, uint32_t x, uint32_t y,
                                         uint32_t width, uint32_t height,
                                         const uint8_t* rgbaPixels);
    void ReleaseLegacyTexture(int textureId);
    bool IsInitialized() const { return m_initialized; }
    const char* GetLastError() const { return m_lastError; }

private:
    struct Target
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageView views[2]{};
        VulkanTexture depth[2];
        VkFramebuffer framebuffers[2]{};
    };
    struct StockDraw
    {
        bool clearDepth = false;
        bool clearStencil = false;
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkBuffer tangentBuffer = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        int vertexFormat = 0;
        uint32_t indexCount = 0;
        uint32_t firstIndex = 0;
        int32_t vertexOffset = 0;
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipeline pipeline = VK_NULL_HANDLE;
        int textureId = 0;
        int textureId1 = 0;
        bool useSecondTexture = false;
        bool useNormalMap = false;
        int normalMapTextureId = 0;
        float globalOpacity = 1.0f;
        float alphaTestRef = 0.0f;
        bool additiveMaterial = false;
        uint32_t stage1ColorMode = 0;
        uint32_t stage1AlphaMode = 0;
        uint32_t stage0ColorMode = 1;
        uint32_t stage0AlphaMode = 1;
        uint32_t stage0ColorArg = 0x0a1u;
        uint32_t stage0AlphaArg = 0x0a1u;
        uint32_t stage0Constant = 0xffffffffu;
        uint32_t stage1ColorArg = 0x0a1u;
        uint32_t stage1AlphaArg = 0x0a1u;
        uint32_t stage1Constant = 0xffffffffu;
        uint32_t stencilState = 0;
        uint32_t stencilRef = 0;
        uint32_t stencilMask = 0xffffffffu;
        float modelView[16]{};
        float textureMatrix0[16]{};
        float textureMatrix1[16]{};
        // object-space light direction, ambient scale, diffuse RGB tint and
        // diffuse strength from the stock render item/material.
        float materialLighting[8] = { -0.35f, 0.72f, 0.60f, 0.22f,
                                      1.0f, 1.0f, 1.0f, 0.78f };
        bool scissorEnabled = false;
        VkRect2D scissor{};
        bool fogEnabled = false;
        float fogColor[3]{};
        float fogDensity = 0.0f;
        float fogStart = 0.0f;
        float fogEnd = 1.0f;
        int fogMode = 0;
    };
    struct LegacyTexture
    {
        VulkanTexture texture;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDeviceSize bytes = 0;
        bool clampU = false;
        bool clampV = false;
        bool noMipmaps = false;
        int filterMode = VulkanFilterTrilinear;
        bool uploadPending = false;
        bool cpuBacked = false;
        std::vector<uint8_t> rgbaPixels;
    };
    struct PanelImage
    {
        int textureId = 0;
        float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
        float s0 = 0.0f, t0 = 0.0f, s1 = 1.0f, t1 = 1.0f;
        float angleDegrees = 0.0f;
        float red = 1.0f, green = 1.0f, blue = 1.0f, alpha = 1.0f;
        float logicalWidth = 0.0f, logicalHeight = 0.0f;
    };
    struct ReusableClientGeometry
    {
        bool valid = false;
        const void* sourceVertices = nullptr;
        const uint16_t* sourceIndices = nullptr;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        int vertexFormat = 0;
        int primitiveMode = 0;
        VulkanBuffer vertexBuffer;
        VulkanBuffer indexBuffer;
        uint32_t firstIndex = 0;
        int32_t vertexOffset = 0;
    };

    bool LoadFunctions();
    bool CreatePanelPipeline();
    bool CreateScenePipelines();
    bool CreateRenderTargets();
    bool EnsureDynamicBufferCapacity(VulkanBuffer& buffer,
                                     std::vector<VulkanBuffer>& previousBuffers,
                                     VkDeviceSize requiredSize, VkBufferUsageFlags usage);
    bool RecordAndSubmit(uint32_t viewIndex);
    void SetError(const char* message);

    Runtime* m_runtime = nullptr;
    VulkanContext* m_context = nullptr;
    VulkanResourceManager* m_resources = nullptr;
    VulkanPipelineFactory m_pipelineFactory;
    VulkanTexture m_gameTexture;
    VulkanBuffer m_dynamicVertexBuffer;
    VulkanBuffer m_dynamicIndexBuffer;
    std::vector<VulkanBuffer> m_previousDynamicVertexBuffers;
    std::vector<VulkanBuffer> m_previousDynamicIndexBuffers;
    VkDeviceSize m_dynamicVertexUsed = 0;
    VkDeviceSize m_dynamicIndexUsed = 0;
    VulkanSwapchain m_swapchain;
    std::vector<Target> m_targets;
    std::vector<VkCommandBuffer> m_commandBuffers;
    std::vector<StockDraw> m_stockDraws;
    std::vector<PanelImage> m_panelImages;
    ReusableClientGeometry m_reusableClientGeometry;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_panelPipeline = VK_NULL_HANDLE;
    VkPipeline m_panelBlendPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_scenePipelineLayout = VK_NULL_HANDLE;
    std::map<std::array<uint32_t, 13>, VkPipeline> m_scenePipelineCache;
    std::map<int, LegacyTexture> m_legacyTextures;
    VkDeviceSize m_legacyTextureBytes = 0;
    VkShaderModule m_sceneVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneFragmentShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneColorVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneColorFragmentShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneLitVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneLitColorVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneLitFragmentShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneTextureVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneTextureFragmentShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneTextureColorVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneTextureColorFragmentShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneMultiTextureColorVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneMultiTextureColorFragmentShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneTextureLitVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneTextureLitColorVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneTextureBumpVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneTextureBumpColorVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneTextureBumpFragmentShader = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_descriptorSet = VK_NULL_HANDLE;
    VkSampler m_gameSampler = VK_NULL_HANDLE;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkFence m_frameFence = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    VkFormat m_depthFormat = VK_FORMAT_UNDEFINED;
    uint32_t m_viewCount = 2;
    uint32_t m_imageIndex = 0;
    float m_stockClearColor[3] = { 0.0f, 0.0f, 0.0f };
    Frame m_frame;
    XrPosef m_referenceHeadPose{};
    bool m_referenceHeadPoseValid = false;
    bool m_initialized = false;
    bool m_frameActive = false;
    uint32_t m_untranslatedDrawCount = 0;
    bool m_stockScissorEnabled = false;
    VkRect2D m_stockScissor{};
    bool m_stockFogEnabled = false;
    float m_stockFogColor[3]{};
    float m_stockFogDensity = 0.0f;
    float m_stockFogStart = 0.0f;
    float m_stockFogEnd = 1.0f;
    int m_stockFogMode = 0;
    bool m_imageAcquired = false;
    bool m_gameTextureUploadPending = false;
    bool m_frameSubmissionPending = false;
    char m_lastError[256]{};

    PFN_vkGetDeviceProcAddr m_getDeviceProcAddr = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties m_getPhysicalDeviceFormatProperties = nullptr;
    PFN_vkDestroyImageView m_destroyImageView = nullptr;
    PFN_vkCreateImageView m_createImageView = nullptr;
    PFN_vkDestroyRenderPass m_destroyRenderPass = nullptr;
    PFN_vkCreateRenderPass m_createRenderPass = nullptr;
    PFN_vkDestroyPipelineLayout m_destroyPipelineLayout = nullptr;
    PFN_vkCreatePipelineLayout m_createPipelineLayout = nullptr;
    PFN_vkDestroyPipeline m_destroyPipeline = nullptr;
    PFN_vkCreateGraphicsPipelines m_createGraphicsPipelines = nullptr;
    PFN_vkCreateShaderModule m_createShaderModule = nullptr;
    PFN_vkDestroyShaderModule m_destroyShaderModule = nullptr;
    PFN_vkDestroyFramebuffer m_destroyFramebuffer = nullptr;
    PFN_vkCreateFramebuffer m_createFramebuffer = nullptr;
    PFN_vkDestroyCommandPool m_destroyCommandPool = nullptr;
    PFN_vkCreateCommandPool m_createCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers m_allocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers m_freeCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer m_beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer m_endCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier m_cmdPipelineBarrier = nullptr;
    PFN_vkCmdBeginRenderPass m_cmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass m_cmdEndRenderPass = nullptr;
    PFN_vkCmdClearAttachments m_cmdClearAttachments = nullptr;
    PFN_vkCmdBindPipeline m_cmdBindPipeline = nullptr;
    PFN_vkCmdDraw m_cmdDraw = nullptr;
    PFN_vkCmdDrawIndexed m_cmdDrawIndexed = nullptr;
    PFN_vkCmdSetStencilCompareMask m_cmdSetStencilCompareMask = nullptr;
    PFN_vkCmdSetStencilWriteMask m_cmdSetStencilWriteMask = nullptr;
    PFN_vkCmdSetStencilReference m_cmdSetStencilReference = nullptr;
    PFN_vkCmdBindVertexBuffers m_cmdBindVertexBuffers = nullptr;
    PFN_vkCmdBindIndexBuffer m_cmdBindIndexBuffer = nullptr;
    PFN_vkCmdSetViewport m_cmdSetViewport = nullptr;
    PFN_vkCmdSetScissor m_cmdSetScissor = nullptr;
    PFN_vkCmdPushConstants m_cmdPushConstants = nullptr;
    PFN_vkCmdBindDescriptorSets m_cmdBindDescriptorSets = nullptr;
    PFN_vkCreateDescriptorSetLayout m_createDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout m_destroyDescriptorSetLayout = nullptr;
    PFN_vkCreateDescriptorPool m_createDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool m_destroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets m_allocateDescriptorSets = nullptr;
    PFN_vkFreeDescriptorSets m_freeDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets m_updateDescriptorSets = nullptr;
    PFN_vkCreateSampler m_createSampler = nullptr;
    PFN_vkDestroySampler m_destroySampler = nullptr;
    PFN_vkQueueSubmit m_queueSubmit = nullptr;
    PFN_vkCreateFence m_createFence = nullptr;
    PFN_vkDestroyFence m_destroyFence = nullptr;
    PFN_vkWaitForFences m_waitForFences = nullptr;
    PFN_vkResetFences m_resetFences = nullptr;
};
}

#endif
