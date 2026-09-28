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
#include <utility>

namespace CryVR
{
enum VulkanTextureFilterMode
{
    VulkanFilterNearest = 0,
    VulkanFilterLinear,
    VulkanFilterBilinear,
    VulkanFilterTrilinear,
    VulkanFilterAnisotropic
};

struct VulkanStockTextureStage
{
    int textureId = 0;
    int colorOp = 5;
    int alphaOp = 5;
    uint32_t colorArg = 0x0a1u;
    uint32_t alphaArg = 0x0a1u;
    uint32_t constant = 0xffffffffu;
    float lodBias = 0.0f;
    bool useTexCoord1 = true;
    int wrapMode = -1;
    float uvTransform[9] = { 1.0f, 0.0f, 0.0f,
                             0.0f, 1.0f, 0.0f,
                             0.0f, 0.0f, 1.0f };
};

struct VulkanSceneDiagnostics
{
    uint32_t queuedDraws = 0;
    uint32_t rejectedInput = 0;
    uint32_t rejectedVertexFeature = 0;
    uint32_t rejectedTexture = 0;
    uint32_t textureFallbacks = 0;
    uint32_t rejectedCombine = 0;
    uint32_t rejectedPipelineState = 0;
    uint32_t pipelineCreationFailed = 0;
    uint32_t missingTextureAtRecord = 0;
    uint32_t invalidEyeTransform = 0;
    uint32_t nullPipelineAtRecord = 0;
    uint32_t emptyScissor = 0;
    uint32_t recordedEyeDraws = 0;
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
    bool IsFrameActive() const { return m_frameActive; }
    const Frame& GetCurrentFrame() const { return m_frame; }
    float GetHeadRotationDeltaRadians() const;
    float GetHeadYawDeltaRadians() const;
    float GetHeadPitchDeltaRadians() const;
    void SetStockClearColor(float red, float green, float blue);
    void SetStockProjectionRange(float nearPlane, float farPlane)
    {
        if (nearPlane > 0.0f && farPlane > nearPlane)
        {
            m_stockNearPlane = nearPlane;
            m_stockFarPlane = farPlane;
        }
    }
    void SetStockFogRangeScale(float scale) { m_stockFogRangeScale = scale; }
    bool QueueStockClearDepth();
    bool QueueStockClearStencil();
    void SetStockVisibilityKey(uint64_t key, bool totalCoverage = false)
    {
        m_pendingVisibilityKey = key;
        m_pendingVisibilityTotalCoverage = totalCoverage;
    }
    bool GetStockVisibility(uint64_t key, bool& visible) const;
    bool GetStockVisibilityFraction(uint64_t key, float& fraction) const;
    bool SupportsStockVisibilityQueries() const { return m_visibilityQueriesEnabled; }
    // Kept as the engine callback name; the NULL-backed Vulkan renderer has no
    // legacy framebuffer to capture, so record the untranslated draw instead.
    void RequirePanelFallback() { if (m_frameActive && m_untranslatedDrawCount < 0xffffffffu) ++m_untranslatedDrawCount; }
    uint32_t GetUntranslatedDrawCount() const { return m_untranslatedDrawCount; }
    const VulkanSceneDiagnostics& GetSceneDiagnostics() const { return m_sceneDiagnostics; }
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
                               int normalMapTextureId = 0,
                               const float* primaryColor = nullptr,
                               const float* primaryColorMask = nullptr,
                               uint32_t colorWriteMaskOverride = 0xffffffffu,
                               float textureStage0LodBias = 0.0f,
                               float textureStage1LodBias = 0.0f,
                               const VulkanStockTextureStage* textureStage2 = nullptr,
                               const VulkanStockTextureStage* textureStage3 = nullptr,
                               bool polygonOffset = false,
                               float polygonOffsetFactor = -1.0f,
                               float polygonOffsetUnits = -4.0f,
                               const float* clipPlane = nullptr,
                               VkDeviceSize vertexBufferOffset = 0,
                               int textureWrapMode0 = -1,
                               int textureWrapMode1 = -1,
                               int textureWrapMode2 = -1,
                               int textureWrapMode3 = -1,
                               const VulkanStockTextureStage* textureStages4To7 = nullptr,
                               const VulkanBuffer* lightmapTexCoordBuffer = nullptr,
                               VkDeviceSize lightmapTexCoordOffset = 0,
                               bool textureStage1UsesTexCoord1 = true,
                               bool textureStage0UsesTexCoord1 = false,
                               bool invertVertexRgb = false,
                               bool nearestObject = false);
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
                                     int normalMapTextureId = 0,
                                     const float* primaryColor = nullptr,
                                     const float* primaryColorMask = nullptr,
                                     uint32_t colorWriteMaskOverride = 0xffffffffu,
                                     float textureStage0LodBias = 0.0f,
                                     float textureStage1LodBias = 0.0f,
                                     const VulkanStockTextureStage* textureStage2 = nullptr,
                                     const VulkanStockTextureStage* textureStage3 = nullptr,
                                     bool polygonOffset = false,
                                     float polygonOffsetFactor = -1.0f,
                                     float polygonOffsetUnits = -4.0f,
                                     const float* clipPlane = nullptr,
                                     int textureWrapMode0 = -1,
                                     int textureWrapMode1 = -1,
                                     int textureWrapMode2 = -1,
                                     int textureWrapMode3 = -1,
                                     const VulkanStockTextureStage* textureStages4To7 = nullptr,
                                     const void* lightmapTexCoords = nullptr,
                                     bool textureStage1UsesTexCoord1 = true,
                                     bool textureStage0UsesTexCoord1 = false,
                                     bool invertVertexRgb = false,
                                     bool nearestObject = false);
    bool QueuePanelImage(int textureId, float x, float y, float width, float height,
                         float s0, float t0, float s1, float t1, float angleDegrees,
                         float red, float green, float blue, float alpha,
                         float logicalWidth, float logicalHeight,
                         uint32_t blendState = 0x65u);
    bool QueuePanelIndexedGeometry(int textureId, const void* vertices, uint32_t vertexCount,
                                   const uint16_t* indices, uint32_t indexCount,
                                   float logicalWidth, float logicalHeight,
                                   const float* screenTransform = nullptr,
                                   uint32_t blendState = 0x65u);
    void SetStockScissor(bool enabled, int x, int y, int width, int height,
                         float logicalWidth, float logicalHeight);
    void SetStockViewport(int x, int y, int width, int height,
                          float logicalWidth, float logicalHeight);
    void SetStockDepthRange(float minDepth, float maxDepth);
    void SetStockProjector(int cubeAtlasTextureId, const float basis[9], float frustumScale);
    void SetStockFog(bool enabled, float density, float start, float end,
                     const float color[3], int mode);
    bool RegisterLegacyRgbaTexture(int textureId, uint32_t width, uint32_t height,
                                   const uint8_t* rgbaPixels, bool clampU, bool clampV,
                                   bool dynamicTexture, bool noMipmaps = false,
                                   int filterMode = VulkanFilterTrilinear,
                                   float maxAnisotropy = 1.0f);
    bool RegisterLegacyRgbaCubeTexture(int textureId, uint32_t width, uint32_t height,
                                       const uint8_t* const facePixels[6], bool noMipmaps,
                                       int filterMode, float maxAnisotropy);
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
        VulkanTexture color[2];
        VulkanTexture depth[2];
        VkFramebuffer framebuffers[2]{};
        VkFramebuffer outputFramebuffers[2]{};
        VkDescriptorSet resolveDescriptorSets[2]{};
    };
    struct StockDraw
    {
        bool clearDepth = false;
        bool clearStencil = false;
        bool nearestObject = false;
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkBuffer tangentBuffer = VK_NULL_HANDLE;
        VkBuffer lightmapTexCoordBuffer = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        int vertexFormat = 0;
        float nearPlane = 0.05f;
        float farPlane = 1000.0f;
        uint32_t indexCount = 0;
        uint32_t visibilityQueryIndex = UINT32_MAX;
        uint32_t visibilityCoverageQueryIndex = UINT32_MAX;
        uint32_t firstIndex = 0;
        int32_t vertexOffset = 0;
        VkDeviceSize vertexBufferOffset = 0;
        VkDeviceSize lightmapTexCoordOffset = 0;
        uint32_t textureTransformOffset = 0;
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkViewport viewport{};
        int textureId = 0;
        int projectorCookieTextureId = 0;
        bool projectorCookieEnabled = false;
        float projectorBasis[9]{};
        float projectorFrustumScale = 1.0f;
        int textureId1 = 0;
        int textureWrapMode[4] = { -1, -1, -1, -1 };
        VulkanStockTextureStage textureStages4To7[4]{};
        bool useTextureStages4To7[4]{};
        VulkanStockTextureStage textureStage2{};
        VulkanStockTextureStage textureStage3{};
        bool useSecondTexture = false;
        bool useThirdTexture = false;
        bool useFourthTexture = false;
        bool useNormalMap = false;
        bool polygonOffset = false;
        float polygonOffsetFactor = -1.0f;
        float polygonOffsetUnits = -4.0f;
        float clipPlane[4]{};
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
        float textureTransformRows[3][8][4]{};
        float fogConstants[32]{};
        float lightingConstants[12]{};
        // object-space light direction/radius, ambient scalar, diffuse RGB,
        // diffuse strength, and RGB material/object ambient factors.
        float materialLighting[11] = { -0.35f, 0.72f, 0.60f, 0.22f,
                                       1.0f, 1.0f, 1.0f, 0.78f,
                                       1.0f, 1.0f, 1.0f };
        float primaryColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float primaryColorMask[4] = {};
        bool invertVertexRgb = false;
        float textureLodBias[4] = {};
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
        VkDescriptorSet wrapDescriptorSets[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
        VkSampler wrapSamplers[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
        VkDeviceSize bytes = 0;
        bool clampU = false;
        bool clampV = false;
        bool noMipmaps = false;
        int filterMode = VulkanFilterTrilinear;
        float maxAnisotropy = 1.0f;
        bool uploadPending = false;
        bool cpuBacked = false;
        std::vector<uint8_t> rgbaPixels;
    };
    struct PanelImage
    {
        bool indexedGeometry = false;
        int textureId = 0;
        uint32_t blendState = 0x65u;
        float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
        float s0 = 0.0f, t0 = 0.0f, s1 = 1.0f, t1 = 1.0f;
        float angleDegrees = 0.0f;
        float red = 1.0f, green = 1.0f, blue = 1.0f, alpha = 1.0f;
        float logicalWidth = 0.0f, logicalHeight = 0.0f;
        bool scissorEnabled = false;
        VkRect2D scissor{};
        VkBuffer vertexBuffer = VK_NULL_HANDLE, indexBuffer = VK_NULL_HANDLE;
        VkDeviceSize vertexBufferOffset = 0;
        uint32_t firstIndex = 0, indexCount = 0;
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
        VkDeviceSize vertexBufferOffset = 0;
    };

    bool LoadFunctions();
    bool CreatePanelPipeline();
    bool CreateOutputPipeline();
    VkPipeline GetPanelPipeline(uint32_t blendState, bool indexedGeometry);
    bool CreateScenePipelines();
    bool CreateRenderTargets();
    bool EnsureDynamicBufferCapacity(VulkanBuffer& buffer,
                                     std::vector<VulkanBuffer>& previousBuffers,
                                     VkDeviceSize requiredSize, VkBufferUsageFlags usage);
    bool UpdateTextureTransformDescriptors();
    VkDescriptorSet GetLegacyTextureDescriptorSet(int textureId, int wrapMode);
    VkDescriptorSet GetProjectorTextureDescriptorSet(int baseTextureId, int cookieTextureId, int wrapMode);
    void ReleaseProjectorTextureDescriptorSets();
    void ReleaseLegacyTextureWrapVariants(LegacyTexture& texture);
    void DestroyLegacyTexture(LegacyTexture& texture);
    void CollectDeferredLegacyTextureReleases();
    bool RecordAndSubmit(uint32_t viewIndex);
    void SetError(const char* message);

    Runtime* m_runtime = nullptr;
    VulkanContext* m_context = nullptr;
    VulkanResourceManager* m_resources = nullptr;
    VulkanPipelineFactory m_pipelineFactory;
    VulkanTexture m_gameTexture;
    VulkanBuffer m_dynamicVertexBuffer;
    VulkanBuffer m_dynamicIndexBuffer;
    VulkanBuffer m_textureTransformBuffer;
    std::vector<VulkanBuffer> m_previousDynamicVertexBuffers;
    std::vector<VulkanBuffer> m_previousDynamicIndexBuffers;
    std::vector<VulkanBuffer> m_previousTextureTransformBuffers;
    VkDeviceSize m_dynamicVertexUsed = 0;
    VkDeviceSize m_dynamicIndexUsed = 0;
    VkDeviceSize m_textureTransformStride = 0;
    VulkanSwapchain m_swapchain;
    std::vector<Target> m_targets;
    std::vector<VkCommandBuffer> m_commandBuffers;
    std::vector<StockDraw> m_stockDraws;
    struct VisibilityQuery { uint64_t key; uint32_t index; bool totalCoverage; };
    struct VisibilitySamples
    {
        uint64_t visibleSamples = 0;
        uint64_t totalSamples = 0;
        bool hasVisibleSamples = false;
        bool hasTotalSamples = false;
    };
    std::vector<VisibilityQuery> m_currentVisibilityQueries;
    std::vector<VisibilityQuery> m_previousVisibilityQueries;
    std::map<uint64_t, VisibilitySamples> m_visibilityResults;
    std::map<uint64_t, std::pair<uint32_t, uint32_t> > m_currentVisibilityQueryByKey;
    uint64_t m_pendingVisibilityKey = 0;
    bool m_pendingVisibilityTotalCoverage = false;
    VkQueryPool m_visibilityQueryPool = VK_NULL_HANDLE;
    bool m_visibilityQueriesEnabled = false;
    std::vector<PanelImage> m_panelImages;
    ReusableClientGeometry m_reusableClientGeometry;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkRenderPass m_outputRenderPass = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_panelPipeline = VK_NULL_HANDLE;
    VkPipeline m_panelBlendPipeline = VK_NULL_HANDLE;
    VkPipeline m_panelAdditivePipeline = VK_NULL_HANDLE;
    VkPipeline m_panelGeometryPipeline = VK_NULL_HANDLE;
    VkPipeline m_panelGeometryOpaquePipeline = VK_NULL_HANDLE;
    VkPipeline m_panelGeometryAdditivePipeline = VK_NULL_HANDLE;
    VkPipeline m_outputPipeline = VK_NULL_HANDLE;
    VkShaderModule m_panelVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_panelFragmentShader = VK_NULL_HANDLE;
    VkShaderModule m_panelGeometryVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_panelGeometryFragmentShader = VK_NULL_HANDLE;
    VkShaderModule m_outputVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_outputFragmentShader = VK_NULL_HANDLE;
    std::map<uint32_t, VkPipeline> m_panelPipelineCache;
    VkPipelineLayout m_scenePipelineLayout = VK_NULL_HANDLE;
    std::map<std::array<uint32_t, 61>, VkPipeline> m_scenePipelineCache;
    std::map<int, LegacyTexture> m_legacyTextures;
    std::vector<LegacyTexture> m_deferredLegacyTextureReleases;
    std::map<uint64_t, VkDescriptorSet> m_projectorDescriptorSets;
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
    VkShaderModule m_sceneMultiTextureLitColorVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneMultiTextureLitNoColorVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneMultiTextureNoColorVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneMultiTextureColorFragmentShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneThreeTextureColorFragmentShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneFourTextureColorFragmentShader = VK_NULL_HANDLE;
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
    XrVector3f m_referenceEyeOffsets[2]{{-0.032f, 0.0f, 0.0f}, {0.032f, 0.0f, 0.0f}};
    bool m_referenceHeadPoseValid = false;
    uint32_t m_eyeTransformAuditFrame = 0;
    bool m_initialized = false;
    bool m_frameActive = false;
    uint32_t m_frameBeginAttempts = 0;
    uint32_t m_frameBeginSuccesses = 0;
    uint32_t m_frameEndCalls = 0;
    uint32_t m_untranslatedDrawCount = 0;
    VulkanSceneDiagnostics m_sceneDiagnostics;
    bool m_scenePipelineErrorLogged = false;
    bool m_stockScissorEnabled = false;
    VkRect2D m_stockScissor{};
    VkViewport m_stockViewport{};
    bool m_stockViewportSet = false;
    float m_stockMinDepth = 0.0f;
    float m_stockMaxDepth = 1.0f;
    int m_stockProjectorTextureId = 0;
    float m_stockProjectorBasis[9]{};
    float m_stockProjectorFrustumScale = 1.0f;
    bool m_stockFogEnabled = false;
    float m_stockFogColor[3]{};
    float m_stockFogDensity = 0.0f;
    float m_stockFogStart = 0.0f;
    float m_stockFogEnd = 1.0f;
    float m_stockFogRangeScale = 1.0f;
    float m_stockNearPlane = 0.05f;
    float m_stockFarPlane = 1000.0f;
    int m_stockFogMode = 0;
    // OpenGL's texture-environment combine state persists per TMU. Keep the
    // effective operations so commands which are GL no-ops retain the last
    // selected mode when constructing immutable Vulkan pipelines.
    uint32_t m_stockTextureColorModes[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    uint32_t m_stockTextureAlphaModes[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    uint32_t m_stockTextureColorArgs[8] = { 17, 25, 25, 25, 25, 25, 25, 25 };
    uint32_t m_stockTextureAlphaArgs[8] = { 17, 25, 25, 25, 25, 25, 25, 25 };
    // GL_COMBINE defaults: source0=TEXTURE, source1=PREVIOUS,
    // source2=CONSTANT. eCA_Specular in the legacy argument stream is a
    // no-op marker, not a shader source selector.
    uint32_t m_stockTextureEffectiveColorArgs[8] = { 281, 281, 281, 281, 281, 281, 281, 281 };
    uint32_t m_stockTextureEffectiveAlphaArgs[8] = { 281, 281, 281, 281, 281, 281, 281, 281 };
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
    PFN_vkCreateQueryPool m_createQueryPool = nullptr;
    PFN_vkDestroyQueryPool m_destroyQueryPool = nullptr;
    PFN_vkGetQueryPoolResults m_getQueryPoolResults = nullptr;
    PFN_vkCmdResetQueryPool m_cmdResetQueryPool = nullptr;
    PFN_vkCmdBeginQuery m_cmdBeginQuery = nullptr;
    PFN_vkCmdEndQuery m_cmdEndQuery = nullptr;
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
