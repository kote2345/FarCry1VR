#ifndef CRY_VR_VULKAN_FRAME_RENDERER_H
#define CRY_VR_VULKAN_FRAME_RENDERER_H

#include "CryVR.h"
#include "VulkanContext.h"
#include "VulkanResourceManager.h"
#include "VulkanPipelineFactory.h"
#include "VulkanFrameWorker.h"
#include "VulkanGpuSkinning.h"

#include <vulkan/vulkan.h>
#include <vector>
#include <map>
#include <array>
#include <utility>
#include <set>

namespace CryVR
{
enum VulkanTextureFilterMode
{
    VulkanFilterNearest = 0,
    VulkanFilterLinear,
    VulkanFilterBilinear,
    VulkanFilterTrilinear,
    VulkanFilterAnisotropic,
    VulkanFilterNearestMipLinear,
    VulkanFilterNearestNoMips
};

// Homogeneous linear texgen data is kept independently for each TMU.
// Planes are rows; textureMatrix uses the engine's column-major convention.
struct VulkanStockLinearTexgen
{
    float planes[4][4]{};
    float textureMatrix[16] = { 1, 0, 0, 0, 0, 1, 0, 0,
                               0, 0, 1, 0, 0, 0, 0, 1 };
    uint32_t componentMask = 0;
    bool enabled = false;
    bool useTexCoord1 = false;
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

struct VulkanWaterReflectionUpdate
{
    float realTime = 0.0f;
    float updateInterval = 0.0f;
    float cameraDistanceThreshold = 0.0f;
    float cameraAngleThreshold = 0.0f;
    float cameraPosition[3]{};
    float cameraAngles[3]{};
    float fieldOfView = 0.0f;
};

struct VulkanSceneDiagnostics
{
    uint32_t queuedDraws = 0;
    uint32_t queuedDecalDraws = 0;
    uint32_t mergedDecalDraws = 0;
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
    bool GetReferenceHeadPose(XrPosef& pose) const
    {
        if (!m_referenceHeadPoseValid) return false;
        pose = m_referenceHeadPose;
        return true;
    }
    float GetHeadRotationDeltaRadians() const;
    float GetHeadYawDeltaRadians() const;
    float GetHeadPitchDeltaRadians() const;
    void SetStockClearColor(float red, float green, float blue);
    bool SetLegacyTextureFilter(int textureId, int filterMode, float anisotropy);
    float GetStockMaxAnisotropy() const
    {
        return m_context && m_context->SupportsAnisotropicFiltering() ?
            m_context->GetMaxSamplerAnisotropy() : 1.0f;
    }
    void ResetStockTextureOperations()
    {
        for (uint32_t stage = 0; stage < 8; ++stage)
        {
            m_stockTextureColorModes[stage] = m_stockTextureAlphaModes[stage] = 1u;
            m_stockTextureColorOps[stage] = 5;
            // GL resets the argument cache to -1 without changing actual
            // sources when ResetToDefault calls SetColorOp(...,255,255).
            m_stockTextureColorArgs[stage] = m_stockTextureAlphaArgs[stage] = 255u;
        }
    }
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
    bool QueueStockClear(bool color, bool depth, bool stencil, const float* rgba = nullptr);
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
                               bool nearestObject = false,
                               bool waterEffect = false,
                               float stockLightingMode = 0.0f,
                               bool directionalLightmap = false,
                               bool bakedLightmap = false,
                               const float* terrainProjection = nullptr,
                               int specularOcclusionTextureId = 0,
                               int specularOcclusionChannel = -1,
                               float lightmapEncodeScale = 4.0f,
                               const float* reflectionModelView = nullptr,
                               const float* reflectionClipPlane = nullptr);
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
                                     bool nearestObject = false,
                                     bool waterEffect = false,
                                     float stockLightingMode = 0.0f,
                                     bool directionalLightmap = false,
                                     bool bakedLightmap = false,
                                     const float* terrainProjection = nullptr,
                                     int specularOcclusionTextureId = 0,
                                     int specularOcclusionChannel = -1,
                                     float lightmapEncodeScale = 4.0f,
                                     const float* reflectionModelView = nullptr,
                                     const float* reflectionClipPlane = nullptr,
                                     const VulkanWaterReflectionUpdate* reflectionUpdate = nullptr,
                                     const void* gpuSkinIdentity = nullptr);
    bool QueueGpuSkinning(const void* identity, const SGpuSkinningData& mesh,
                         const float* bones, uint32_t count) {
        return m_frameActive && m_gpuSkinning.Queue(identity, mesh, bones, count);
    }
    void ClearGpuSkinning(const void* identity) { m_gpuSkinning.Clear(identity); }
    bool QueueGpuMorph(const void* identity, const SGpuSkinningData& mesh, float weight, float normalAmplify) {
        return m_gpuSkinning.Morph(identity, mesh, weight, normalAmplify);
    }
    bool QueueGpuSkinningRemap(const void* identity, const void* source, const uint32_t* map, uint32_t count) {
        return m_gpuSkinning.Remap(identity, source, map, count);
    }
    bool QueueGpuSkinShadow(const void* identity, const void* source, const SGpuSkinningData& mesh,
        const SGpuSkinShadowData& topology, const float* bones, uint32_t count, const float* light, float extent, uint32_t first = 0) {
        return m_frameActive && m_gpuSkinning.Shadow(identity,source,mesh,topology,bones,count,light,extent,first);
    }
    void SetStockGpuSkinIdentity(const void* identity) { m_stockGpuSkinIdentity=identity; }
    bool QueueGpuSkinReadback(const void* identity, const GpuSkinReadback& callback) { return m_gpuSkinning.Readback(identity,callback); }
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
    void SetStockShadowMapPass(int textureId, const float projection[16]);
    bool HasQueuedShadowMapDraws(int textureId) const;
    void SetStockShadowTransforms(const float transforms[8][16], uint32_t stageMask);
    void ResetStockLinearTexgen()
    {
        // Disabled generators are never read. Keep their storage and only
        // clear the validity flag; SetStockLinearTexgen replaces the full value.
        for (auto& generator : m_stockLinearTexgen) generator.enabled = false;
    }
    // Four vec4s per fixed light: position/type, diffuse, specular, attenuation.
    void SetStockProfilePlants(bool value) { m_stockProfilePlants = value; }
    void SetStockDecalDraw(bool value) { m_stockDecalDraw = value; }
    void SetStockFixedLights(const std::array<std::array<float, 16>, 8>& lights, uint32_t count,
                            const float* modelView = nullptr, const float* normalMatrix = nullptr)
    {
        m_stockFixedLightCount = count <= 8 ? count : 8;
        if (!m_stockFixedLightCount) return;
        for (uint32_t light = 0; light < m_stockFixedLightCount; ++light)
            m_stockFixedLights[light] = lights[light];
        for (uint32_t i = 0; i < 16; ++i)
        {
            m_stockFixedMatrices[0][i] = modelView ? modelView[i] : (i % 5 == 0 ? 1.0f : 0.0f);
            m_stockFixedMatrices[1][i] = normalMatrix ? normalMatrix[i] : (i % 5 == 0 ? 1.0f : 0.0f);
        }
    }
    void SetStockLinearTexgen(uint32_t stage, const VulkanStockLinearTexgen& value)
    {
        if (stage < m_stockLinearTexgen.size()) m_stockLinearTexgen[stage] = value;
    }
    void SetStockProjector(int cubeAtlasTextureId, const float basis[9], float frustumScale);
    void SetStockFog(bool enabled, float density, float start, float end,
                     const float color[3], int mode);
    bool HasLegacyTexture(int textureId) const { return m_legacyTextures.find(textureId) != m_legacyTextures.end(); }
    VkDeviceSize GetLegacyTextureBytes() const { return m_legacyTextureBytes; }
    bool RegisterLegacyRgbaTexture(int textureId, uint32_t width, uint32_t height,
                                   const uint8_t* rgbaPixels, bool clampU, bool clampV,
                                   bool dynamicTexture, bool noMipmaps = false,
                                   int filterMode = VulkanFilterTrilinear,
                                   float maxAnisotropy = 1.0f,
                                   const uint8_t* const* rgbaMipPixels = nullptr,
                                   uint32_t suppliedMipCount = 0);
    bool RegisterLegacyDepthTexture(int textureId, uint32_t width, uint32_t height);
    bool RegisterLegacyRgbaCubeTexture(int textureId, uint32_t width, uint32_t height,
                                       const uint8_t* const facePixels[6], bool noMipmaps,
                                       int filterMode, float maxAnisotropy);
    bool RegisterLegacyRgbaTextureRegion(int textureId, uint32_t x, uint32_t y,
                                         uint32_t width, uint32_t height,
                                         const uint8_t* rgbaPixels);
    void ReleaseLegacyTexture(int textureId);
    bool IsInitialized() const { return m_initialized; }
    void SetDiagnosticLogging(bool enabled) { m_diagnosticLogging = enabled; }
    bool IsDiagnosticLoggingEnabled() const { return m_diagnosticLogging; }
    const char* GetLastError() const { return m_lastError; }

private:
    VulkanGpuSkinning m_gpuSkinning;
    const void* m_stockGpuSkinIdentity = nullptr;
    struct Target
    {
        VkImage image = VK_NULL_HANDLE;
        VkImageView views[2]{};
        VkImageView sceneColorViews[2]{};
        VkImageView sceneDepthViews[2]{};
        VkFramebuffer multiviewFramebuffer = VK_NULL_HANDLE;
        VulkanTexture color[2];
        VulkanTexture waterCopy[2];
        VulkanTexture waterReflection[2];
        VulkanTexture waterDepth[2];
        VulkanTexture depth[2];
        VkFramebuffer framebuffers[2]{};
        VkFramebuffer waterReflectionFramebuffers[2]{};
        VkFramebuffer outputFramebuffers[2]{};
        VkDescriptorSet resolveDescriptorSets[2]{};
        VkDescriptorSet waterDescriptorSets[2]{};
        VkDescriptorSet waterReflectionDescriptorSets[2]{};
        bool waterCopyInitialized[2]{};
        bool waterDepthInitialized[2]{};
        bool waterReflectionInitialized[2]{};
        uint32_t waterReflectionLastFrame[2]{};
        float waterReflectionModelView[2][16]{};
        float waterReflectionLastUpdateTime[2]{};
        float waterReflectionLastPosition[2][3]{};
        float waterReflectionLastAngles[2][3]{};
        float waterReflectionLastFov[2]{};
    };
    struct StockDrawAux
    {
        float linearPlanes[8][4][4];
        float linearTextureMatrices[8][16];
        uint32_t linearComponentMasks[8];
        bool linearEnabled[8];
        bool linearUseTexCoord1[8];
        float fixedLights[8][16];
        float fixedMatrices[2][16];
        float shadowStageMatrices[8][16];

        StockDrawAux() : linearEnabled{} {}
    };
    struct StockDrawTextureStage
    {
        int textureId;
        int colorOp;
        int alphaOp;
        uint32_t colorArg;
        uint32_t alphaArg;
        uint32_t constant;
        float lodBias;
        bool useTexCoord1;
        int wrapMode;
        float uvTransform[9];
    };
    struct StockDraw
    {
        bool profilePlants = false;
        bool decalDraw = false;
        bool simpleDecalMode = false;
        VkDeviceSize decalVertexEndOffset = 0;
        // Coherent upload data stays alive until the frame submission retires.
        // Verify exact mesh equality before merging adjacent plant draws.
        const void* plantsVertexData = nullptr;
        const void* plantsIndexData = nullptr;
        VkDeviceSize plantsVertexBytes = 0;
        bool clearDepth = false;
        bool clearColor = false;
        float clearRgba[4];
        bool clearStencil = false;
        bool nearestObject = false;
        int shadowMapTextureId = 0;
        float shadowProjection[16];
        uint32_t shadowStageMask = 0;
        uint32_t auxiliaryIndex = UINT32_MAX;
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkBuffer tangentBuffer = VK_NULL_HANDLE;
        VkDeviceSize tangentBufferOffset = 0;
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
        std::array<uint32_t, 64> pipelineKey;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipeline reflectionPipeline = VK_NULL_HANDLE;
        VkViewport viewport;
        int textureId = 0;
        int projectorCookieTextureId = 0;
        bool projectorCookieEnabled = false;
        float projectorBasis[9];
        float projectorFrustumScale = 1.0f;
        int textureId1 = 0;
        int textureWrapMode[4];
        StockDrawTextureStage textureStages4To7[4];
        bool useTextureStages4To7[4];
        StockDrawTextureStage textureStage2;
        StockDrawTextureStage textureStage3;
        bool useSecondTexture = false;
        bool useThirdTexture = false;
        bool useFourthTexture = false;
        bool useNormalMap = false;
        bool polygonOffset = false;
        float polygonOffsetFactor = -1.0f;
        float polygonOffsetUnits = -4.0f;
        float clipPlane[4];
        int normalMapTextureId = 0;
        int specularOcclusionTextureId = 0;
        int specularOcclusionChannel = -1;
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
        uint32_t textureConstants[8];
        uint32_t stencilState = 0;
        uint32_t stencilRef = 0;
        uint32_t stencilMask = 0xffffffffu;
        float modelView[16];
        float reflectionModelView[16];
        float reflectionClipPlane[4];
        bool hasWaterReflectionTransform = false;
        VulkanWaterReflectionUpdate waterReflectionUpdate{};
        bool hasWaterReflectionUpdate = false;
        float textureMatrix0[16];
        float textureMatrix1[16];
        float textureTransformRows[3][8][4]{};
        uint32_t fixedLightCount = 0;
        float terrainProjectionRows[2][8][4]{};
        float fogConstants[32];
        float lightingConstants[12];
        // object-space light direction/radius, diffuse RGB and opacity,
        // and RGB material/object ambient factors.
        float materialLighting[19];
        float primaryColor[4];
        float primaryColorMask[4];
        bool invertVertexRgb = false;
        bool waterEffect = false;
        bool directionalLightmap = false;
        bool bakedLightmap = false;
        float lightmapEncodeScale = 4.0f;
        float textureLodBias[4];
        bool scissorEnabled = false;
        VkRect2D scissor;
        bool fogEnabled = false;
        float fogColor[3];
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
        bool depthOnly = false;
        std::vector<uint8_t> rgbaPixels;
    };
    struct ShadowMapTarget
    {
        VulkanTexture color;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        bool initialized = false;
        bool dirty = true;
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
        const void* skinIdentity = nullptr;
        VkDeviceSize tangentBufferOffset = 0;
        const uint16_t* sourceIndices = nullptr;
        const void* sourceLightmapTexCoords = nullptr;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        int vertexFormat = 0;
        int primitiveMode = 0;
        VulkanBuffer vertexBuffer;
        VulkanBuffer indexBuffer;
        uint32_t firstIndex = 0;
        int32_t vertexOffset = 0;
        VkDeviceSize vertexBufferOffset = 0;
        VkDeviceSize lightmapTexCoordOffset = 0;
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
    bool CompletePendingSubmission();
    bool PrepareSceneUniforms();
    void BindStereoDescriptor(VkDescriptorSet set);
    void WriteDescriptorSets(uint32_t count, const VkWriteDescriptorSet* writes);
    XrView GetSceneCameraView(uint32_t eye) const;
    bool RecordShadowMapDraws(VkCommandBuffer commandBuffer, int textureId,
                              uint32_t width, uint32_t height);
    bool RecordWaterReflectionDraws(VkCommandBuffer commandBuffer, uint32_t viewIndex,
                                    uint32_t width, uint32_t height);
    void SetError(const char* message);

    Runtime* m_runtime = nullptr;
    VulkanContext* m_context = nullptr;
    VulkanResourceManager* m_resources = nullptr;
    VulkanPipelineFactory m_pipelineFactory;
    VulkanTexture m_gameTexture;
    VulkanBuffer m_dynamicVertexBuffer;
    VulkanBuffer m_dynamicIndexBuffer;
    VulkanBuffer m_spareDynamicVertexBuffer;
    VulkanBuffer m_spareDynamicIndexBuffer;
    std::vector<VulkanBuffer> m_pendingRetiredBuffers;
    std::set<VkBuffer> m_pendingReadBuffers;
    uint32_t m_pendingCommandCount = 0;
    uint32_t m_pendingFrameNumber = 0;
    size_t m_pendingDrawCount = 0;
    double m_cpuResourceWaitMs = 0.0;
    VulkanBuffer m_textureTransformBuffer;
    VulkanBuffer m_stereoTransformBuffer;
    std::vector<VulkanBuffer> m_previousStereoTransformBuffers;
    std::set<VkDescriptorSet> m_stereoDescriptorSets;
    bool m_multiview = false;
    std::vector<VulkanBuffer> m_previousDynamicVertexBuffers;
    std::vector<VulkanBuffer> m_previousDynamicIndexBuffers;
    std::vector<VulkanBuffer> m_previousTextureTransformBuffers;
    VkDeviceSize m_dynamicVertexUsed = 0;
    VkDeviceSize m_dynamicIndexUsed = 0;
    VkDeviceSize m_textureTransformStride = 0;
    size_t m_uniformDrawCapacity = 0;
    uint32_t m_uniformFrameSlot = 0;
    uint32_t m_pendingUniformFrameSlot = 0;
    uint32_t m_stereoDrawBase = 0;
    VulkanSwapchain m_swapchain;
    std::vector<Target> m_targets;
    std::vector<VkCommandBuffer> m_commandBuffers;
    std::vector<StockDraw> m_stockDraws;
    std::vector<StockDrawAux> m_stockDrawAux;
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
    VkQueryPool m_timingQueryPool = VK_NULL_HANDLE;
    struct GpuDrawProfile {
        uint32_t drawIndex = 0, indexCount = 0, textureCount = 0;
        uint32_t renderState = 0, shaderTag = 0;
        int vertexFormat = 0, texture0 = 0, texture1 = 0;
        const char* category = "opaque";
        float lightingMode = 0.0f;
        uint64_t viewportPixels = 0;
    };
    VkQueryPool m_gpuProfileQueryPool = VK_NULL_HANDLE;
    std::vector<GpuDrawProfile> m_recordedGpuProfiles;
    std::vector<GpuDrawProfile> m_pendingGpuProfiles;
    uint32_t m_gpuProfileReports = 0;
    bool m_gpuProfileArmed = false;
    bool m_stockProfilePlants = false;
    bool m_stockDecalDraw = false;
    bool m_gpuAbArmed = false, m_gpuAbBaselineReady = false;
    uint32_t m_gpuAbPairs = 0, m_recordedAbMode = 0, m_pendingAbMode = 0;
    uint32_t m_recordedAbSkipped = 0, m_pendingAbSkipped = 0, m_pendingAbGroup = 0;
    uint32_t m_pendingSimpleDecalDraws = 0;
    uint64_t m_recordedAbHash = 0, m_pendingAbHash = 0;
    double m_timestampPeriod = 0.0;
    uint32_t m_timestampValidBits = 0;
    bool m_visibilityQueriesEnabled = false;
    std::vector<PanelImage> m_panelImages;
    ReusableClientGeometry m_reusableClientGeometry;
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkRenderPass m_multiviewRenderPass = VK_NULL_HANDLE;
    VkRenderPass m_multiviewLoadRenderPass = VK_NULL_HANDLE;
    VkRenderPass m_loadRenderPass = VK_NULL_HANDLE;
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
    std::map<std::array<uint32_t, 64>, VkPipeline> m_scenePipelineCache;
    std::array<uint32_t, 64> m_lastScenePipelineKey{};
    VkPipeline m_lastScenePipeline = VK_NULL_HANDLE;
    bool m_lastScenePipelineValid = false;
    std::map<int, LegacyTexture> m_legacyTextures;
    std::map<int, ShadowMapTarget> m_shadowMapTargets;
    std::vector<ShadowMapTarget> m_deferredShadowMapTargets;
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
    VkShaderModule m_waterVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_terrainLayerVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_terrainLayerFragmentShader = VK_NULL_HANDLE;
    VkShaderModule m_causticsVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_causticsFragmentShader = VK_NULL_HANDLE;
    VkShaderModule m_plantsVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_plantsFragmentShader = VK_NULL_HANDLE;
    VkShaderModule m_waterColorVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_beachVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_seaFragmentShader = VK_NULL_HANDLE;
    VkShaderModule m_oceanVertexShader = VK_NULL_HANDLE;
    VkShaderModule m_sceneWaterFragmentShader = VK_NULL_HANDLE;
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
    VkShaderModule m_generatedTextureVertexShaders[24]{};
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
    bool m_sceneInitialized[2] = { false, false };
    float m_stockClearColor[3] = { 0.0f, 0.0f, 0.0f };
    Frame m_frame;
    XrPosef m_referenceHeadPose{};
    XrVector3f m_referenceEyeOffsets[2]{{-0.032f, 0.0f, 0.0f}, {0.032f, 0.0f, 0.0f}};
    bool m_referenceHeadPoseValid = false;
    uint32_t m_eyeTransformAuditFrame = 0;
    bool m_initialized = false;
    bool m_diagnosticLogging = false;
    bool m_frameActive = false;
    uint32_t m_frameBeginAttempts = 0;
    uint32_t m_frameBeginSuccesses = 0;
    uint32_t m_frameEndCalls = 0;
    VulkanFrameWorker m_uniformWorker;
    double m_cpuCaptureStartMs = 0.0;
    double m_cpuBeginDurationMs = 0.0;
    double m_cpuLastEndMs = 0.0;
    double m_cpuUpdateDurationMs = 0.0;
    uint32_t m_untranslatedDrawCount = 0;
    VulkanSceneDiagnostics m_sceneDiagnostics;
    bool m_scenePipelineErrorLogged = false;
    bool m_stockScissorEnabled = false;
    VkRect2D m_stockScissor{};
    VkViewport m_stockViewport{};
    bool m_stockViewportSet = false;
    int m_stockShadowMapTextureId = 0;
    float m_stockShadowProjection[16]{};
    uint32_t m_stockShadowStageMask = 0;
    float m_stockShadowStageMatrices[8][16]{};
    std::array<VulkanStockLinearTexgen, 8> m_stockLinearTexgen{};
    std::array<std::array<float, 16>, 8> m_stockFixedLights{};
    uint32_t m_stockFixedLightCount = 0;
    std::array<std::array<float, 16>, 2> m_stockFixedMatrices{};
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
    int m_stockTextureColorOps[8] = { 5, 5, 5, 5, 5, 5, 5, 5 };
    uint32_t m_stockTextureAlphaModes[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    uint32_t m_stockTextureColorArgs[8] = { 255, 255, 255, 255, 255, 255, 255, 255 };
    uint32_t m_stockTextureAlphaArgs[8] = { 255, 255, 255, 255, 255, 255, 255, 255 };
    // GL_COMBINE defaults: source0=TEXTURE, source1=PREVIOUS,
    // source2=CONSTANT. eCA_Specular in the legacy argument stream is a
    // no-op marker, not a shader source selector.
    uint32_t m_stockTextureEffectiveColorArgs[8] = { 281, 281, 281, 281, 281, 281, 281, 281 };
    bool m_stockTextureThirdOperandRgb[8]{};
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
    PFN_vkCmdCopyImage m_cmdCopyImage = nullptr;
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
    PFN_vkCmdWriteTimestamp m_cmdWriteTimestamp = nullptr;
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
