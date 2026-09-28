#include "RenderPCH.h"
#include "../XRenderNULL/NULL_Renderer.h"
#include "VulkanFrameRenderer.h"
#include "VulkanVertexFormat.h"
#include "VulkanTextureDecode.h"
#include "../../CryCommon/CryHeaders.h"
#include "../../CryCommon/ICryAnimation.h"
#include "../../CryCommon/CREOcLeaf.h"
#include "../Common/RendElements/CREBeam.h"
#include "../../CryCommon/CREPolyMesh.h"
#include "../Common/RendElements/CREScreenCommon.h"
#include "../Common/RendElements/CREFlares.h"
#include "../../CryCommon/CRETriMeshShadow.h"
#include "../../CryCommon/LeafBuffer.h"
#include "../Common/NvTriStrip/NvTriStrip.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace
{
float g_vulkanPolygonOffsetFactor = -1.0f;
float g_vulkanPolygonOffsetUnits = -4.0f;
int g_vulkanClipPlanes = 1;

const uint16_t g_vulkanFlareIndices[54] = {
    0, 4, 5, 0, 5, 6, 0, 6, 7, 0, 7, 1,
    1, 7, 8, 1, 8, 9, 15, 4, 0, 15, 0, 3,
    3, 0, 1, 3, 1, 2, 2, 1, 9, 2, 9, 10,
    14, 15, 3, 14, 3, 13, 13, 3, 2, 13, 2, 12,
    12, 2, 11, 11, 2, 10
};

int VulkanTriangleIndicesToPolygon(const uint16_t* source, int indexCount,
                                   int vertexCount, uint16_t* destination)
{
    if (!source || !destination || indexCount < 3 || (indexCount % 3) != 0 ||
        vertexCount < 3)
        return 0;
    const int triangleCount = indexCount / 3;
    int destinationCount = 1;
    destination[0] = source[0];
    int triangle = 0;
    do
    {
        for (triangle = 0; triangle < triangleCount; ++triangle)
        {
            const int current = destination[destinationCount - 1];
            int edge = 0;
            for (; edge < 3; ++edge)
            {
                if (source[triangle * 3 + edge] != current)
                    continue;
                const uint16_t next = source[triangle * 3 + (edge + 1) % 3];
                if (destinationCount == 1)
                {
                    if (next == destination[0])
                        continue;
                }
                else
                {
                    int seen = 1;
                    for (; seen < destinationCount; ++seen)
                        if (destination[seen] == next)
                            break;
                    if (seen != destinationCount)
                        continue;
                }

                int otherTriangle = 0;
                for (; otherTriangle < triangleCount; ++otherTriangle)
                {
                    if (otherTriangle == triangle)
                        continue;
                    int otherEdge = 0;
                    for (; otherEdge < 3; ++otherEdge)
                        if (source[otherTriangle * 3 + otherEdge] == next &&
                            source[otherTriangle * 3 + (otherEdge + 1) % 3] == current)
                            break;
                    if (otherEdge != 3)
                        break;
                }
                if (otherTriangle == triangleCount)
                {
                    if (destinationCount >= vertexCount)
                        return 0;
                    destination[destinationCount++] = next;
                    break;
                }
            }
            if (edge != 3)
                break;
        }
        if (destinationCount == vertexCount)
            break;
    } while (triangle != triangleCount);
    return destinationCount;
}

int ParseCubeFace(const char* name, std::string& cubeName)
{
    if (!name)
        return -1;
    std::string path(name);
    const size_t extension = path.find_last_of('.');
    const size_t separator = path.find_last_of("/\\");
    if (extension != std::string::npos &&
        (separator == std::string::npos || extension > separator))
        path.resize(extension);
    const size_t underscore = path.find_last_of('_');
    if (underscore == std::string::npos)
        return -1;
    const std::string suffix = path.substr(underscore + 1);
    static const char* const faceNames[6] = { "posx", "negx", "posy", "negy", "posz", "negz" };
    for (int face = 0; face < 6; ++face)
    {
        if (!stricmp(suffix.c_str(), faceNames[face]))
        {
            cubeName = path.substr(0, underscore);
            return face;
        }
    }
    return -1;
}

bool DecodeSpecialTexture(ETEX_Format format, const byte* pixels, size_t pixelCount,
                          std::vector<uint8_t>& rgba)
{
    if (!pixels)
        return false;

    rgba.resize(pixelCount * 4);
    if (format == eTF_RGB8 || format == eTF_0088)
    {
        for (size_t i = 0; i < pixelCount; ++i)
        {
            if (format == eTF_RGB8)
            {
                rgba[i * 4 + 0] = pixels[i * 3 + 0];
                rgba[i * 4 + 1] = pixels[i * 3 + 1];
                rgba[i * 4 + 2] = pixels[i * 3 + 2];
                rgba[i * 4 + 3] = 255;
            }
            else
            {
                rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = pixels[i * 2 + 0];
                rgba[i * 4 + 3] = pixels[i * 2 + 1];
            }
        }
        return true;
    }
    if (format != eTF_0565 && format != eTF_0555 &&
        format != eTF_1555 && format != eTF_4444)
        return false;

    for (size_t i = 0; i < pixelCount; ++i)
    {
        uint16_t packed = 0;
        memcpy(&packed, pixels + i * sizeof(packed), sizeof(packed));
        uint8_t r, g, b, a = 255;
        if (format == eTF_4444)
        {
            // Match the legacy GL conversion in GLTextures.cpp: its packed
            // byte order is R/G followed by B/A, with the nibble in the high
            // four bits of each expanded channel.
            r = static_cast<uint8_t>((packed & 0x000fu) << 4);
            g = static_cast<uint8_t>((packed & 0x00f0u));
            b = static_cast<uint8_t>((packed & 0x0f00u) >> 4);
            a = static_cast<uint8_t>((packed & 0xf000u) >> 8);
        }
        else if (format == eTF_0565)
        {
            const uint8_t r5 = static_cast<uint8_t>((packed >> 11) & 0x1fu);
            const uint8_t g6 = static_cast<uint8_t>((packed >> 5) & 0x3fu);
            const uint8_t b5 = static_cast<uint8_t>(packed & 0x1fu);
            r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
            g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
            b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
        }
        else
        {
            const uint8_t r5 = static_cast<uint8_t>((packed >> 10) & 0x1fu);
            const uint8_t g5 = static_cast<uint8_t>((packed >> 5) & 0x1fu);
            const uint8_t b5 = static_cast<uint8_t>(packed & 0x1fu);
            r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
            g = static_cast<uint8_t>((g5 << 3) | (g5 >> 2));
            b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
            if (format == eTF_1555)
                a = (packed & 0x8000u) ? 255 : 0;
        }
        rgba[i * 4 + 0] = r;
        rgba[i * 4 + 1] = g;
        rgba[i * 4 + 2] = b;
        rgba[i * 4 + 3] = a;
    }
    return true;
}

bool HasStockVertexNormal(int format)
{
    // Legacy eVertexFormat values containing a normal; keep aligned with
    // CryVR's Vulkan vertex-input layouts.
    switch (format)
    {
    case 7: case 8: case 9: case 10: case 11: case 13:
        return true;
    default:
        return false;
    }
}

class CVulkanTexMan final : public CNULLTexMan
{
public:
    void SetCallbacks(const SVulkanBufferCallbacks& callbacks)
    {
        m_callbacks = callbacks;
    }

    void SetFilterOverride(int filterMode) { m_filterOverride = filterMode; }
    void ClearFilterOverride() { m_filterOverride = -1; }

    struct CubeUpload
    {
        int textureId = 0;
        int width = 0;
        int height = 0;
        bool noMipmaps = false;
        int filterMode = eVTF_Trilinear;
        float anisotropy = 1.0f;
        bool hasFace[6]{};
        std::vector<uint8_t> faces[6];
    };

protected:
    STexPic* CreateTexture(const char* name, int width, int height, int depth,
                           uint flags, uint flags2, byte* pixels, ETexType textureType,
                           float amount1, float amount2, int dxtSize, STexPic* texture,
                           int bind, ETEX_Format format, const char* sourceName) override
    {
        if (width <= 0 || height <= 0 ||
            (textureType != eTT_Base && textureType != eTT_Bumpmap &&
             textureType != eTT_DSDTBump && textureType != eTT_Cubemap))
            return texture;

        if (!texture)
        {
            texture = TextureInfoForName(name, -1, textureType, flags, flags2, bind);
            if (!texture)
                return nullptr;
            texture->m_bBusy = true;
            texture->m_Flags = flags;
            texture->m_Flags2 = flags2;
            texture->m_Bind = bind ? bind : TX_FIRSTBIND + texture->m_Id;
            AddToHash(texture->m_Bind, texture);
        }

        texture->m_Width = width;
        texture->m_Height = height;
        texture->m_WidthOriginal = width;
        texture->m_HeightOriginal = height;
        texture->m_Depth = depth;
        texture->m_eTT = textureType;
        texture->m_TargetType = textureType == eTT_Cubemap ? 0x8513u : 0x0DE1u;
        texture->m_ETF = format;
        texture->m_nMips = (flags & FT_NOMIPS) ? 1 : texture->m_nMips;
        texture->m_DXTSize = dxtSize;
        texture->m_fAmount1 = amount1;
        texture->m_fAmount2 = amount2;
        if (sourceName)
            texture->m_SourceName = sourceName;
        if (texture->m_Bind == 0)
            texture->m_Bind = TX_FIRSTBIND + texture->m_Id;

        if (!pixels || (textureType == eTT_Cubemap ? !m_callbacks.mirrorRgbaCubeTexture :
                                                     !m_callbacks.mirrorRgbaTexture))
            return texture;

        int filterMode = m_filterOverride;
        const int anisotropyLevel = CRenderer::CV_r_texture_anisotropic_level;
        const float anisotropy = anisotropyLevel > 1 ? static_cast<float>(anisotropyLevel) : 1.0f;
        if (filterMode < 0)
        {
            switch (flags2 & FT2_FILTER)
            {
            case FT2_FILTER_NEAREST: filterMode = eVTF_Nearest; break;
            case FT2_FILTER_BILINEAR: filterMode = eVTF_Bilinear; break;
            case FT2_FILTER_TRILINEAR:
                filterMode = eVTF_Trilinear; break;
            case FT2_FILTER_ANISOTROPIC: filterMode = eVTF_Anisotropic; break;
            default: filterMode = anisotropy > 1.0f ? eVTF_Anisotropic : eVTF_Trilinear; break;
            }
        }

        const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
        std::vector<unsigned char> rgba(pixelCount * 4);
        if (textureType == eTT_DSDTBump)
        {
            // CryEngine's eTF_DSDT_MAG payload is four signed bytes per
            // texel. The legacy GL_DSDT_MAG upload converts its first three
            // values to signed floating point; encode those signed values in
            // UNORM8 around 0.5 so a Vulkan shader can recover them with
            // sample.rgb * 2 - 1. The fourth source byte is not part of the
            // sampled DSDT vector.
            if (format != eTF_DSDT_MAG)
                return texture;
            for (size_t i = 0; i < pixelCount; ++i)
            {
                for (size_t channel = 0; channel < 3; ++channel)
                {
                    const int signedValue = static_cast<int>(
                        reinterpret_cast<const signed char*>(pixels)[i * 4 + channel]);
                    rgba[i * 4 + channel] = static_cast<unsigned char>(signedValue + 128);
                }
                rgba[i * 4 + 3] = 255;
            }
        }
        else if (format == eTF_DXT1 || format == eTF_DXT3 || format == eTF_DXT5)
        {
            if (!DecodeVulkanDxtBaseLevel(pixels, width, height,
                                          format == eTF_DXT1,
                                          format == eTF_DXT3,
                                          format == eTF_DXT5, rgba))
                return texture;
        }
        else if (format == eTF_0565 || format == eTF_0555 ||
                 format == eTF_1555 || format == eTF_4444 ||
                 format == eTF_RGB8 || format == eTF_0088)
        {
            if (!DecodeSpecialTexture(format, pixels, pixelCount, rgba))
                return texture;
        }
        else
        for (size_t i = 0; i < pixelCount; ++i)
        {
            switch (format)
            {
            case eTF_Index:
                // CTexMan::UploadImage expands indexed GIF/PCX/BMP data with
                // FillBGRA_8to32 before it calls CreateTexture; the payload at
                // this backend boundary is therefore BGRA8, not palette indices.
                rgba[i * 4 + 0] = pixels[i * 4 + 2];
                rgba[i * 4 + 1] = pixels[i * 4 + 1];
                rgba[i * 4 + 2] = pixels[i * 4 + 0];
                rgba[i * 4 + 3] = pixels[i * 4 + 3];
                break;
            case eTF_8888:
                rgba[i * 4 + 0] = pixels[i * 4 + 2];
                rgba[i * 4 + 1] = pixels[i * 4 + 1];
                rgba[i * 4 + 2] = pixels[i * 4 + 0];
                rgba[i * 4 + 3] = pixels[i * 4 + 3];
                break;
            case eTF_RGBA:
                memcpy(&rgba[i * 4], &pixels[i * 4], 4);
                break;
            case eTF_0888:
                rgba[i * 4 + 0] = pixels[i * 3 + 2];
                rgba[i * 4 + 1] = pixels[i * 3 + 1];
                rgba[i * 4 + 2] = pixels[i * 3 + 0];
                rgba[i * 4 + 3] = 255;
                break;
            case eTF_8000:
                rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
                rgba[i * 4 + 3] = pixels[i];
                break;
            default:
                return texture;
            }
        }

        if (textureType == eTT_Cubemap)
        {
            std::string cubeName;
            int face = ParseCubeFace(name, cubeName);
            if (face < 0)
                face = ParseCubeFace(texture->m_SearchName.c_str(), cubeName);
            if (face < 0)
                return texture;
            CubeUpload& cube = m_cubeUploads[cubeName];
            if (face == 0 || cube.width != width || cube.height != height)
            {
                cube = CubeUpload{};
                cube.width = width;
                cube.height = height;
                cube.textureId = static_cast<int>(texture->m_Bind);
                cube.noMipmaps = (flags & FT_NOMIPS) != 0;
                cube.filterMode = filterMode;
                cube.anisotropy = anisotropy;
            }
            if (face == 0)
                cube.textureId = static_cast<int>(texture->m_Bind);
            cube.faces[face].swap(rgba);
            cube.hasFace[face] = true;
            bool complete = cube.textureId > 0;
            const unsigned char* facePixels[6] = {};
            for (int i = 0; i < 6; ++i)
            {
                complete = complete && cube.hasFace[i] && !cube.faces[i].empty();
                facePixels[i] = cube.faces[i].empty() ? nullptr : cube.faces[i].data();
            }
            if (complete)
            {
                const bool mirrored = m_callbacks.mirrorRgbaCubeTexture(
                    m_callbacks.drawUserData, cube.textureId,
                    static_cast<unsigned int>(cube.width),
                    static_cast<unsigned int>(cube.height), facePixels,
                    cube.noMipmaps, cube.filterMode, cube.anisotropy);
                if (mirrored)
                    texture->m_Size = static_cast<int>(pixelCount * 24u);
                m_cubeUploads.erase(cubeName);
            }
            return texture;
        }

        const bool mirrored = m_callbacks.mirrorRgbaTexture(
            m_callbacks.drawUserData, static_cast<int>(texture->m_Bind),
            static_cast<unsigned int>(width), static_cast<unsigned int>(height),
            rgba.data(), (flags & FT_CLAMP) || (flags2 & FT2_UCLAMP),
            (flags & FT_CLAMP) || (flags2 & FT2_VCLAMP),
            ((flags & FT_DYNAMIC) != 0 || (flags & FT_FONT) != 0),
            (flags & FT_NOMIPS) != 0, filterMode, anisotropy);
        if (mirrored)
            texture->m_Size = static_cast<int>(pixelCount * 4);
        return texture;
    }

private:
    SVulkanBufferCallbacks m_callbacks;
    std::map<std::string, CubeUpload> m_cubeUploads;
    int m_filterOverride = -1;
};

class CVulkanRenderer final : public CNULLRenderer
{
public:
    explicit CVulkanRenderer(CryVR::VulkanFrameRenderer* frameRenderer)
        : m_frameRenderer(frameRenderer)
    {
        SetType(R_VULKAN_RENDERER);
        m_nFrameUpdateID = m_nFrameID;
        // GLRendPipeline::EF_PipelineInit starts texture-environment color at
        // opaque white. Keep the NULL-backed renderer's equivalent state in
        // sync before the first Vulkan draw.
        m_RP.m_CurGlobalColor.dcolor = 0xffffffffu;
        if (iConsole)
        {
            if (!iConsole->GetCVar("GL_OffsetFactor"))
                iConsole->Register("GL_OffsetFactor", &g_vulkanPolygonOffsetFactor, -1.0f);
            if (!iConsole->GetCVar("GL_OffsetUnits"))
                iConsole->Register("GL_OffsetUnits", &g_vulkanPolygonOffsetUnits, -4.0f);
            m_polygonOffsetFactorCVar = iConsole->GetCVar("GL_OffsetFactor");
            m_polygonOffsetUnitsCVar = iConsole->GetCVar("GL_OffsetUnits");
            if (!iConsole->GetCVar("GL_ClipPlanes"))
                iConsole->Register("GL_ClipPlanes", &g_vulkanClipPlanes, 1);
            m_clipPlanesCVar = iConsole->GetCVar("GL_ClipPlanes");
        }
        // Advertise the shader/material capabilities the Vulkan translation
        // actually consumes so CryEngine's shader parser selects its high-end
        // NV4X/PS3-era material descriptions. Do not advertise depth-map or
        // self-shadow support here: those OpenGL passes still need a Vulkan
        // shadow-map implementation. The Vulkan texture manager decodes DXT
        // itself, so preserve compressed DDS data for upload.
        m_Features = RFT_HW_NV4X | RFT_HW_VS | RFT_HW_PS20 | RFT_HW_PS30 |
            RFT_MULTITEXTURE | RFT_BUMP | RFT_ALLOWSECONDCOLOR |
            RFT_COMPRESSTEXTURE;
        delete m_TexMan;
        m_vulkanTexMan = new CVulkanTexMan;
        m_TexMan = m_vulkanTexMan;
    }

    void SetVulkanBufferCallbacks(const SVulkanBufferCallbacks& callbacks) override
    {
        m_vulkanTexMan->SetCallbacks(callbacks);
    }

    void SetClearColor(const Vec3& color) override
    {
        CRenderer::SetClearColor(color);
        if (m_frameRenderer)
            m_frameRenderer->SetStockClearColor(color.x, color.y, color.z);
    }

    void ClearColorBuffer(const Vec3d color) override
    {
        CNULLRenderer::ClearColorBuffer(color);
        if (m_frameRenderer)
            m_frameRenderer->SetStockClearColor(color.x, color.y, color.z);
    }

    void ClearDepthBuffer() override
    {
        CNULLRenderer::ClearDepthBuffer();
        // Match the stock GL side effect as well as the ordered attachment clear.
        EF_SetState(GS_DEPTHWRITE);
        if (m_frameOpen && m_frameRenderer && !m_frameRenderer->QueueStockClearDepth())
            m_frameRenderer->RequirePanelFallback();
    }

    void SetCamera(const CCamera& camera) override
    {
        CNULLRenderer::SetCamera(camera);
        // OpenGL passes GetVCMatrixD3D9().GetData() directly to glLoadMatrixf,
        // then EF_SetCameraInfo reads the same column-major bytes back with
        // glGetFloatv. Preserve those bytes here: transposing the Matrix44
        // changes the model-view used by the Vulkan scene draws and sends
        // world vertices through the inverse camera transform.
        m_CameraMatrix = camera.GetVCMatrixD3D9();
        m_ViewMatrix = m_CameraMatrix;
        if (m_frameRenderer)
            m_frameRenderer->SetStockProjectionRange(camera.GetZMin(), camera.GetZMax());
        UpdateLegacyProjection(camera);
        UpdateLegacyCameraInfo();
    }

    void SetPerspective(const CCamera& camera) override
    {
        CNULLRenderer::SetPerspective(camera);
        UpdateLegacyProjection(camera);
    }

    IShader* EF_LoadShader(const char* name, EShClass shaderClass, int flags = 0,
                           uint64 generationMask = 0) override
    {
        return m_cEF.mfForName(name, shaderClass, flags, nullptr, generationMask);
    }

    SShaderItem EF_LoadShaderItem(const char* name, EShClass shaderClass, bool share,
                                  const char* templateName, int flags = 0,
                                  SInputShaderResources* resources = nullptr,
                                  uint64 generationMask = 0) override
    {
        return m_cEF.mfShaderItemForName(name, shaderClass, share, templateName,
                                         flags, resources, generationMask);
    }

    void SetViewport(int x = 0, int y = 0, int width = 0, int height = 0) override
    {
        if (x == 0 && y == 0 && width == 0 && height == 0)
        {
            if (m_frameRenderer)
            {
                if (m_VWidth > 0 && m_VHeight > 0)
                {
                    const float logicalWidth = GetWidth() > 0 ?
                        static_cast<float>(GetWidth()) : 800.0f;
                    const float logicalHeight = GetHeight() > 0 ?
                        static_cast<float>(GetHeight()) : 600.0f;
                    m_frameRenderer->SetStockViewport(m_VX, m_VY, m_VWidth, m_VHeight,
                                                      logicalWidth, logicalHeight);
                }
                else
                    m_frameRenderer->SetStockViewport(0, 0, 0, 0, 0.0f, 0.0f);
            }
            CNULLRenderer::SetViewport(x, y, width, height);
            return;
        }

        m_VX = x;
        m_VY = y;
        m_VWidth = width;
        m_VHeight = height;
        if (m_frameRenderer)
        {
            const float logicalWidth = GetWidth() > 0 ? static_cast<float>(GetWidth()) : 800.0f;
            const float logicalHeight = GetHeight() > 0 ? static_cast<float>(GetHeight()) : 600.0f;
            m_frameRenderer->SetStockViewport(x, y, width, height, logicalWidth, logicalHeight);
        }
        CNULLRenderer::SetViewport(x, y, width, height);
    }

    void GetModelViewMatrix(float* matrix) override
    {
        if (matrix)
            memcpy(matrix, m_ViewMatrix.GetData(), sizeof(float) * 16);
    }

    void GetModelViewMatrix(double* matrix) override
    {
        if (!matrix)
            return;
        const float* source = m_ViewMatrix.GetData();
        for (int i = 0; i < 16; ++i)
            matrix[i] = source[i];
    }

    void GetProjectionMatrix(float* matrix) override
    {
        if (matrix)
            memcpy(matrix, m_ProjMatrix.GetData(), sizeof(float) * 16);
    }

    void GetProjectionMatrix(double* matrix) override
    {
        if (!matrix)
            return;
        const float* source = m_ProjMatrix.GetData();
        for (int i = 0; i < 16; ++i)
            matrix[i] = source[i];
    }

    void EF_SetClipPlane(bool enable, float* plane, bool refract) override
    {
        if (m_clipPlanesCVar && !m_clipPlanesCVar->GetIVal())
            return;
        if (enable)
        {
            if (m_RP.m_ClipPlaneEnabled || !plane)
                return;
            m_RP.m_bClipPlaneRefract = refract;
            m_RP.m_CurClipPlane.m_Normal.x = plane[0];
            m_RP.m_CurClipPlane.m_Normal.y = plane[1];
            m_RP.m_CurClipPlane.m_Normal.z = plane[2];
            m_RP.m_CurClipPlane.m_Dist = plane[3];
            m_RP.m_CurClipPlane.Init();
            m_RP.m_CurClipPlaneCull = m_RP.m_CurClipPlane;
            m_RP.m_CurClipPlaneCull.m_Dist = -m_RP.m_CurClipPlaneCull.m_Dist;
            m_RP.m_ClipPlaneWasOverrided = 0;
            m_RP.m_ClipPlaneEnabled = 1;
            return;
        }
        m_RP.m_ClipPlaneEnabled = 0;
        m_RP.m_ClipPlaneWasOverrided = 0;
    }

    void SetFog(float density, float fogStart, float fogEnd, const float* color,
                int fogMode) override
    {
        if (!color)
            return;
        m_fogDensity = density;
        m_fogStart = fogStart;
        m_fogEnd = fogEnd;
        // CGLRenderer::SetFog only calls glFogi for these two public modes.
        // Any other value leaves GL's previously selected mode in effect
        // (initially GL_EXP), so retain the effective mode here as well.
        if (fogMode == R_FOGMODE_LINEAR || fogMode == R_FOGMODE_EXP2)
            m_fogMode = fogMode;
        for (int i = 0; i < 3; ++i)
            m_fogColor[i] = color[i];
        UpdateVulkanFog();
    }

    void SetFogColor(float* color) override
    {
        if (!color)
            return;
        for (int i = 0; i < 3; ++i)
            m_fogColor[i] = color[i];
        UpdateVulkanFog();
    }

    bool EnableFog(bool enable) override
    {
        const bool previous = m_fogEnabled;
        m_fogEnabled = enable;
        UpdateVulkanFog();
        return previous;
    }

    void SetScissor(int x = 0, int y = 0, int width = 0, int height = 0) override
    {
        const bool enabled = x != 0 || y != 0 || width != 0 || height != 0;
        if (m_frameRenderer)
            m_frameRenderer->SetStockScissor(enabled, x, y, width, height,
                static_cast<float>(GetWidth()), static_cast<float>(GetHeight()));
        CNULLRenderer::SetScissor(x, y, width, height);
    }

    int SetPolygonMode(int mode) override
    {
        const int previousMode = m_polygonMode;
        m_polygonMode = mode == R_WIREFRAME_MODE ? R_WIREFRAME_MODE : R_SOLID_MODE;
        return previousMode;
    }

    void SetMaterialColor(float red, float green, float blue, float alpha) override
    {
        m_materialColor[0] = red;
        m_materialColor[1] = green;
        m_materialColor[2] = blue;
        m_materialColor[3] = alpha;
    }

    void BeginFrame() override
    {
        CNULLRenderer::BeginFrame();
        // The NULL backend does not advance CryEngine's frame counters or
        // reset shader pipeline state. World visibility caches key off these
        // counters; keeping one ID across frames empties subsequent scenes.
        m_cEF.mfBeginFrame();
        ++m_nFrameID;
        ++m_nFrameUpdateID;
        // CSystem owns the OpenXR frame lifetime. The renderer only records
        // native scene work into that already-acquired frame.
        m_frameOpen = m_frameRenderer != nullptr;
        if (m_frameRenderer)
            m_frameRenderer->SetStockDepthRange(0.0f, 1.0f);
    }

    void Draw2dImage(float x, float y, float width, float height, int textureId,
                     float s0, float t0, float s1, float t1, float angle,
                     float red, float green, float blue, float alpha, float) override
    {
        if (m_frameOpen && m_frameRenderer)
            m_frameRenderer->QueuePanelImage(textureId,
                ScaleCoordX(x), ScaleCoordY(y), ScaleCoordX(width), ScaleCoordY(height),
                s0, t0, s1, t1,
                angle, red, green, blue, alpha,
                static_cast<float>(GetWidth()), static_cast<float>(GetHeight()),
                CurrentPanelBlendState());
    }

    void DrawImage(float x, float y, float width, float height, int textureId,
                   float s0, float t0, float s1, float t1,
                   float red, float green, float blue, float alpha) override
    {
        Draw2dImage(x, y, width, height, textureId, s0, t0, s1, t1, 0.0f,
                    red, green, blue, alpha, 1.0f);
    }

    void Set2DMode(bool enable, int orthoWidth, int orthoHeight) override
    {
        if (enable && orthoWidth > 0 && orthoHeight > 0)
        {
            ScreenMatrixState saved;
            saved.matrix = m_screenMatrix;
            saved.stack = m_screenMatrixStack;
            saved.width = m_screenSpaceWidth;
            saved.height = m_screenSpaceHeight;
            m_screenMatrixModes.push_back(saved);
            SetIdentityScreenMatrix(m_screenMatrix.data());
            m_screenMatrixStack.clear();
            m_screenSpaceMode = true;
            m_screenSpaceWidth = static_cast<float>(orthoWidth);
            m_screenSpaceHeight = static_cast<float>(orthoHeight);
        }
        else if (!enable)
        {
            if (!m_screenMatrixModes.empty())
            {
                m_screenMatrix = m_screenMatrixModes.back().matrix;
                m_screenMatrixStack = m_screenMatrixModes.back().stack;
                m_screenSpaceWidth = m_screenMatrixModes.back().width;
                m_screenSpaceHeight = m_screenMatrixModes.back().height;
                m_screenMatrixModes.pop_back();
            }
            m_screenSpaceMode = !m_screenMatrixModes.empty();
        }
        CNULLRenderer::Set2DMode(enable, orthoWidth, orthoHeight);
    }

    void PushMatrix() override
    {
        m_screenMatrixStack.push_back(m_screenMatrix);
        CNULLRenderer::PushMatrix();
    }

    void PopMatrix() override
    {
        if (!m_screenMatrixStack.empty())
        {
            m_screenMatrix = m_screenMatrixStack.back();
            m_screenMatrixStack.pop_back();
        }
        CNULLRenderer::PopMatrix();
    }

    void TranslateMatrix(float x, float y, float z) override
    {
        float translation[16];
        SetIdentityScreenMatrix(translation);
        translation[12] = x; translation[13] = y; translation[14] = z;
        MultiplyScreenMatrix(m_screenMatrix.data(), translation);
        CNULLRenderer::TranslateMatrix(x, y, z);
    }

    void TranslateMatrix(const Vec3d& position) override
    {
        TranslateMatrix(position.x, position.y, position.z);
    }

    void ScaleMatrix(float x, float y, float z) override
    {
        float scale[16];
        SetIdentityScreenMatrix(scale);
        scale[0] = x; scale[5] = y; scale[10] = z;
        MultiplyScreenMatrix(m_screenMatrix.data(), scale);
        CNULLRenderer::ScaleMatrix(x, y, z);
    }

    void RotateMatrix(float angle, float x, float y, float z) override
    {
        const double length = std::sqrt(x*x + y*y + z*z);
        if (length > 0.0)
        {
            const float inverseLength = static_cast<float>(1.0 / length);
            x *= inverseLength; y *= inverseLength; z *= inverseLength;
            const float radians = angle * 0.01745329251994329577f;
            const float c = std::cos(radians), s = std::sin(radians), t = 1.0f - c;
            float rotation[16] = {
                t*x*x+c,       t*x*y+s*z,     t*x*z-s*y,     0.0f,
                t*x*y-s*z,     t*y*y+c,       t*y*z+s*x,     0.0f,
                t*x*z+s*y,     t*y*z-s*x,     t*z*z+c,       0.0f,
                0.0f,          0.0f,          0.0f,          1.0f
            };
            MultiplyScreenMatrix(m_screenMatrix.data(), rotation);
        }
        CNULLRenderer::RotateMatrix(angle, x, y, z);
    }

    void RotateMatrix(const Vec3d& angles) override
    {
        RotateMatrix(angles.z, 0.0f, 0.0f, 1.0f);
        RotateMatrix(angles.y, 0.0f, 1.0f, 0.0f);
        RotateMatrix(angles.x, 1.0f, 0.0f, 0.0f);
    }

    void MultMatrix(float* matrix) override
    {
        if (matrix)
            MultiplyScreenMatrix(m_screenMatrix.data(), matrix);
        CNULLRenderer::MultMatrix(matrix);
    }

    void LoadMatrix(const Matrix44* matrix) override
    {
        if (matrix)
            memcpy(m_screenMatrix.data(), matrix->GetData(), sizeof(float) * 16);
        else
            SetIdentityScreenMatrix(m_screenMatrix.data());
        CNULLRenderer::LoadMatrix(matrix);
    }

    void SetTexture(int textureId, ETexType textureType = eTT_Base) override
    {
        m_currentTextureId = textureId;
        const int stage = m_TexMan ? m_TexMan->GetCurTMU() : 0;
        if (stage >= 0 && stage < 8)
            m_stageTextureIds[stage] = textureId;
        CNULLRenderer::SetTexture(textureId, textureType);
    }

    void SetTexClampMode(bool clamp) override
    {
        if (m_currentTextureId > 0)
            m_textureWrapOverrides[m_currentTextureId] = clamp ? 1 : 0;
        CNULLRenderer::SetTexClampMode(clamp);
    }

    void SetColorOp(byte colorOp, byte alphaOp, byte colorArg, byte alphaArg) override
    {
        const int stage = m_TexMan ? m_TexMan->GetCurTMU() : 0;
        if (stage >= 0 && stage < 8)
        {
            if (colorOp != 255)
            {
                m_stageColorOps[stage] = colorOp;
                if (m_activePass) m_stageStateOverrides[stage] |= 1u;
            }
            if (alphaOp != 255)
            {
                m_stageAlphaOps[stage] = alphaOp;
                if (m_activePass) m_stageStateOverrides[stage] |= 2u;
            }
            if (colorArg != 255)
            {
                m_stageColorArgs[stage] = colorArg;
                if (m_activePass) m_stageStateOverrides[stage] |= 4u;
            }
            if (alphaArg != 255)
            {
                m_stageAlphaArgs[stage] = alphaArg;
                if (m_activePass) m_stageStateOverrides[stage] |= 8u;
            }
        }
        CNULLRenderer::SetColorOp(colorOp, alphaOp, colorArg, alphaArg);
    }

    void SetCullMode(int mode = R_CULL_BACK) override
    {
        m_currentCullMode = mode;
        if (m_activePass)
            m_activeCull = mode;
        CNULLRenderer::SetCullMode(mode);
    }

    void DrawDynVB(struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F* vertices, ushort* indices,
                   int vertexCount, int indexCount, int primitiveType) override
    {
        if (!m_frameOpen || !m_frameRenderer || vertexCount <= 0 || indexCount <= 0)
            return;
        if (!vertices || !indices || vertexCount > 65535)
        {
            m_frameRenderer->RequirePanelFallback();
            return;
        }

        if (!m_screenSpaceMode)
        {
            for (int i = 0; i < indexCount; ++i)
            {
                if (indices[i] >= vertexCount)
                {
                    m_frameRenderer->RequirePanelFallback();
                    return;
                }
            }
            CVertexBuffer clientVertices;
            clientVertices.m_VS[VSF_GENERAL].m_VData = vertices;
            clientVertices.m_vertexformat = VERTEX_FORMAT_P3F_COL4UB_TEX2F;
            clientVertices.m_NumVerts = vertexCount;
            SVertexStream indexStream;
            indexStream.m_VData = indices;
            indexStream.m_nItems = indexCount;
            DrawBuffer(&clientVertices, &indexStream, indexCount, 0,
                       R_PRIMV_TRIANGLES, 0, vertexCount, nullptr);
            return;
        }
        if (m_currentTextureId <= 0 || primitiveType != R_PRIMV_TRIANGLES || indexCount % 3 != 0)
        {
            m_frameRenderer->RequirePanelFallback();
            return;
        }
        if (!m_frameRenderer->QueuePanelIndexedGeometry(m_currentTextureId, vertices,
                static_cast<uint32_t>(vertexCount), indices, static_cast<uint32_t>(indexCount),
                m_screenSpaceWidth, m_screenSpaceHeight, m_screenMatrix.data(),
                CurrentPanelBlendState()))
            m_frameRenderer->RequirePanelFallback();
    }

    void DrawTriStrip(CVertexBuffer* source, int vertexCount) override
    {
        if (!m_frameOpen || !m_frameRenderer || !source || vertexCount < 3 ||
            vertexCount > 65535 || !source->m_VS[VSF_GENERAL].m_VData ||
            source->m_vertexformat < 1 || source->m_vertexformat > 16)
        {
            if (m_frameRenderer)
                m_frameRenderer->RequirePanelFallback();
            return;
        }

        if (m_screenSpaceMode)
        {
            if (source->m_vertexformat != VERTEX_FORMAT_P3F &&
                source->m_vertexformat != VERTEX_FORMAT_P3F_COL4UB &&
                source->m_vertexformat != VERTEX_FORMAT_P3F_TEX2F &&
                source->m_vertexformat != VERTEX_FORMAT_P3F_COL4UB_TEX2F)
            {
                m_frameRenderer->RequirePanelFallback();
                return;
            }

            const auto colorChannel = [](float value) -> uint8_t
            {
                return static_cast<uint8_t>(clamp_tpl(value, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            UCol materialTint;
            materialTint.bcolor[0] = colorChannel(m_materialColor[0]);
            materialTint.bcolor[1] = colorChannel(m_materialColor[1]);
            materialTint.bcolor[2] = colorChannel(m_materialColor[2]);
            materialTint.bcolor[3] = colorChannel(m_materialColor[3]);
            const auto multiplyTint = [&](UCol& color)
            {
                for (int channel = 0; channel < 4; ++channel)
                    color.bcolor[channel] = static_cast<uint8_t>(
                        (static_cast<unsigned int>(color.bcolor[channel]) *
                         materialTint.bcolor[channel] + 127u) / 255u);
            };
            std::vector<struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F> panelVertices(
                static_cast<size_t>(vertexCount));
            for (int i = 0; i < vertexCount; ++i)
            {
                struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F& output = panelVertices[static_cast<size_t>(i)];
                output.xyz = Vec3d(0.0f, 0.0f, 0.0f);
                output.color = materialTint;
                output.st[0] = output.st[1] = 0.0f;
                if (source->m_vertexformat == VERTEX_FORMAT_P3F)
                {
                    const struct_VERTEX_FORMAT_P3F* input =
                    static_cast<const struct_VERTEX_FORMAT_P3F*>(source->m_VS[VSF_GENERAL].m_VData);
                    output.xyz = input[i].xyz;
                }
                else if (source->m_vertexformat == VERTEX_FORMAT_P3F_COL4UB)
                {
                    const struct_VERTEX_FORMAT_P3F_COL4UB* input =
                        static_cast<const struct_VERTEX_FORMAT_P3F_COL4UB*>(source->m_VS[VSF_GENERAL].m_VData);
                    output.xyz = input[i].xyz;
                    output.color = input[i].color;
                    multiplyTint(output.color);
                }
                else if (source->m_vertexformat == VERTEX_FORMAT_P3F_TEX2F)
                {
                    const struct_VERTEX_FORMAT_P3F_TEX2F* input =
                        static_cast<const struct_VERTEX_FORMAT_P3F_TEX2F*>(source->m_VS[VSF_GENERAL].m_VData);
                    output.xyz = input[i].xyz;
                    output.st[0] = input[i].st[0];
                    output.st[1] = input[i].st[1];
                }
                else
                {
                    const struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F* input =
                        static_cast<const struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F*>(source->m_VS[VSF_GENERAL].m_VData);
                    output = input[i];
                    multiplyTint(output.color);
                }
            }

            std::vector<uint16_t> panelIndices(static_cast<size_t>(vertexCount - 2) * 3);
            for (int triangle = 0; triangle < vertexCount - 2; ++triangle)
            {
                const size_t base = static_cast<size_t>(triangle) * 3;
                if (triangle & 1)
                {
                    panelIndices[base + 0] = static_cast<uint16_t>(triangle + 1);
                    panelIndices[base + 1] = static_cast<uint16_t>(triangle);
                }
                else
                {
                    panelIndices[base + 0] = static_cast<uint16_t>(triangle);
                    panelIndices[base + 1] = static_cast<uint16_t>(triangle + 1);
                }
                panelIndices[base + 2] = static_cast<uint16_t>(triangle + 2);
            }
            if (m_currentTextureId <= 0 ||
                !m_frameRenderer->QueuePanelIndexedGeometry(m_currentTextureId,
                    panelVertices.data(), static_cast<uint32_t>(panelVertices.size()),
                    panelIndices.data(), static_cast<uint32_t>(panelIndices.size()),
                    m_screenSpaceWidth, m_screenSpaceHeight, m_screenMatrix.data(),
                    CurrentPanelBlendState()))
                m_frameRenderer->RequirePanelFallback();
            return;
        }

        // CRESky and several stock helpers provide a CPU-only CVertexBuffer
        // and rely on DrawTriStrip to submit it. The NULL backend drops this
        // call; make a transient client buffer and sequential strip indices
        // so it follows the same material, texture, stereo and pipeline path
        // as ordinary indexed geometry.
        CVertexBuffer clientVertices;
        const void* clientData = source->m_VS[VSF_GENERAL].m_VData;
        int clientFormat = source->m_vertexformat;
        std::vector<struct_VERTEX_FORMAT_P3F_COL4UB> coloredPositions;
        std::vector<struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F> coloredTextured;
        const auto colorChannel = [](float value) -> uint8_t
        {
            const float clamped = clamp_tpl(value, 0.0f, 1.0f);
            return static_cast<uint8_t>(clamped * 255.0f + 0.5f);
        };
        UCol tint{};
        tint.bcolor[0] = colorChannel(m_materialColor[0]);
        tint.bcolor[1] = colorChannel(m_materialColor[1]);
        tint.bcolor[2] = colorChannel(m_materialColor[2]);
        tint.bcolor[3] = colorChannel(m_materialColor[3]);
        if (source->m_vertexformat == VERTEX_FORMAT_P3F)
        {
            const struct_VERTEX_FORMAT_P3F* input = static_cast<const struct_VERTEX_FORMAT_P3F*>(clientData);
            coloredPositions.resize(static_cast<size_t>(vertexCount));
            for (int i = 0; i < vertexCount; ++i)
            {
                coloredPositions[static_cast<size_t>(i)].xyz = input[i].xyz;
                coloredPositions[static_cast<size_t>(i)].color = tint;
            }
            clientData = coloredPositions.data();
            clientFormat = VERTEX_FORMAT_P3F_COL4UB;
        }
        else if (source->m_vertexformat == VERTEX_FORMAT_P3F_TEX2F)
        {
            const struct_VERTEX_FORMAT_P3F_TEX2F* input =
                static_cast<const struct_VERTEX_FORMAT_P3F_TEX2F*>(clientData);
            coloredTextured.resize(static_cast<size_t>(vertexCount));
            for (int i = 0; i < vertexCount; ++i)
            {
                struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F& output = coloredTextured[static_cast<size_t>(i)];
                output.xyz = input[i].xyz;
                output.color = tint;
                output.st[0] = input[i].st[0];
                output.st[1] = input[i].st[1];
            }
            clientData = coloredTextured.data();
            clientFormat = VERTEX_FORMAT_P3F_COL4UB_TEX2F;
        }
        clientVertices.m_VS[VSF_GENERAL].m_VData = const_cast<void*>(clientData);
        clientVertices.m_vertexformat = clientFormat;
        clientVertices.m_NumVerts = vertexCount;
        std::vector<ushort> stripIndices(static_cast<size_t>(vertexCount));
        for (int i = 0; i < vertexCount; ++i)
            stripIndices[static_cast<size_t>(i)] = static_cast<ushort>(i);
        SVertexStream indexStream;
        indexStream.m_VData = stripIndices.data();
        indexStream.m_nItems = vertexCount;
        const bool previousTextureOverride = m_forceCurrentTextureForClientDraw;
        m_forceCurrentTextureForClientDraw = true;
        DrawBuffer(&clientVertices, &indexStream, vertexCount, 0,
                   R_PRIMV_TRIANGLE_STRIP, 0, 0, nullptr);
        m_forceCurrentTextureForClientDraw = previousTextureOverride;
    }

    void* GetDynVBPtr(int vertexCount, int& vertexOffset, int pool) override
    {
        if (!m_fontRenderingState)
        {
            if (pool != 0)
                return CNULLRenderer::GetDynVBPtr(vertexCount, vertexOffset, pool);
            if (vertexCount <= 0 || vertexCount > 65535)
                return nullptr;
            m_worldDynamicVertices.resize(static_cast<size_t>(vertexCount));
            vertexOffset = 0;
            return m_worldDynamicVertices.data();
        }
        vertexOffset = 0;
        if (pool != 0 || vertexCount <= 0 || vertexCount > 65535)
            return nullptr;
        m_fontVertices.resize(static_cast<size_t>(vertexCount));
        return m_fontVertices.data();
    }

    void DrawDynVB(int vertexOffset, int pool, int vertexCount) override
    {
        if (!m_fontRenderingState)
        {
            if (pool != 0 || vertexOffset < 0 || vertexCount <= 0 ||
                vertexCount % 3 != 0 ||
                static_cast<size_t>(vertexOffset) + static_cast<size_t>(vertexCount) >
                    m_worldDynamicVertices.size() || !m_frameOpen || !m_frameRenderer)
            {
                if (m_frameRenderer)
                    m_frameRenderer->RequirePanelFallback();
                return;
            }
            if (m_screenSpaceMode)
            {
                if (m_currentTextureId <= 0 || !m_frameOpen || !m_frameRenderer)
                {
                    if (m_frameRenderer)
                        m_frameRenderer->RequirePanelFallback();
                    return;
                }
                std::vector<ushort> indices(static_cast<size_t>(vertexCount));
                for (int i = 0; i < vertexCount; ++i)
                    indices[static_cast<size_t>(i)] = static_cast<ushort>(i);
                if (!m_frameRenderer->QueuePanelIndexedGeometry(m_currentTextureId,
                        m_worldDynamicVertices.data() + vertexOffset,
                        static_cast<uint32_t>(vertexCount), indices.data(),
                        static_cast<uint32_t>(indices.size()), m_screenSpaceWidth,
                        m_screenSpaceHeight, m_screenMatrix.data(),
                        CurrentPanelBlendState()))
                    m_frameRenderer->RequirePanelFallback();
                return;
            }
            std::vector<ushort> indices(static_cast<size_t>(vertexCount));
            for (int i = 0; i < vertexCount; ++i)
                indices[static_cast<size_t>(i)] = static_cast<ushort>(i);
            CVertexBuffer clientVertices;
            clientVertices.m_VS[VSF_GENERAL].m_VData = m_worldDynamicVertices.data() + vertexOffset;
            clientVertices.m_vertexformat = VERTEX_FORMAT_P3F_COL4UB_TEX2F;
            clientVertices.m_NumVerts = vertexCount;
            SVertexStream indexStream;
            indexStream.m_VData = indices.data();
            indexStream.m_nItems = vertexCount;
            DrawBuffer(&clientVertices, &indexStream, vertexCount, 0,
                       R_PRIMV_TRIANGLES, 0, vertexCount, nullptr);
            return;
        }
        if (!m_fontRenderingState || pool != 0 || vertexOffset != 0 || !m_frameOpen ||
            !m_frameRenderer || m_fontTextureId <= 0 || vertexCount <= 0 ||
            vertexCount % 6 != 0 || static_cast<size_t>(vertexCount) > m_fontVertices.size())
            return;

        const float invByte = 1.0f / 255.0f;
        const float screenWidth = static_cast<float>(GetWidth());
        const float screenHeight = static_cast<float>(GetHeight());
        for (int first = 0; first + 5 < vertexCount; first += 6)
        {
            const struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F& topLeft = m_fontVertices[first];
            const struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F& topRight = m_fontVertices[first + 1];
            const struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F& bottomLeft = m_fontVertices[first + 4];
            const float width = topRight.xyz.x - topLeft.xyz.x;
            const float height = bottomLeft.xyz.y - topLeft.xyz.y;
            if (!(width > 0.0f) || !(height > 0.0f))
                continue;

            // CryFont emits each atlas glyph as a six-vertex, axis-aligned quad.
            // Its UVs are already in the texture's GL convention; convert them
            // to the panel shader's top-down screen-coordinate convention.
            const UCol color = topLeft.color;
            m_frameRenderer->QueuePanelImage(m_fontTextureId,
                topLeft.xyz.x, topLeft.xyz.y, width, height,
                topLeft.st[0], 1.0f - topLeft.st[1],
                topRight.st[0], 1.0f - bottomLeft.st[1], 0.0f,
                color.bcolor[0] * invByte, color.bcolor[1] * invByte,
                color.bcolor[2] * invByte, color.bcolor[3] * invByte,
                screenWidth, screenHeight, CurrentPanelBlendState());
        }
    }

    int FontCreateTexture(int width, int height, byte* data, ETEX_Format format) override
    {
        if (!data || width <= 0 || height <= 0)
            return -1;
        char name[128];
        sprintf(name, "$AutoFont_%d", m_TexGenID++);
        const int flags = FT_HASALPHA | FT_NOREMOVE | FT_FONT | FT_NOSTREAM | FT_NOMIPS;
        STexPic* texture = m_TexMan->CreateTexture(name, width, height, 1, flags, 0, data,
            eTT_Base, -1.0f, -1.0f, 0, nullptr, 0, format);
        return texture ? texture->GetTextureID() : -1;
    }

    bool FontUpdateTexture(int textureId, int x, int y, int width, int height, byte* data) override
    {
        if (!data || !m_frameRenderer || textureId < TX_FIRSTBIND || width <= 0 || height <= 0)
            return false;
        STexPic* texture = m_TexMan->GetByID(textureId);
        if (!texture || texture->m_Bind != textureId)
            return false;
        const size_t count = static_cast<size_t>(width) * height;
        std::vector<uint8_t> rgba(count * 4);
        if (texture->m_ETF == eTF_0565 || texture->m_ETF == eTF_0555 ||
            texture->m_ETF == eTF_1555 || texture->m_ETF == eTF_4444 ||
            texture->m_ETF == eTF_RGB8 || texture->m_ETF == eTF_0088)
        {
            if (!DecodeSpecialTexture(texture->m_ETF, data, count, rgba))
                return false;
            return m_frameRenderer->RegisterLegacyRgbaTextureRegion(textureId,
                static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                static_cast<uint32_t>(width), static_cast<uint32_t>(height), rgba.data());
        }
        for (size_t i = 0; i < count; ++i)
        {
            switch (texture->m_ETF)
            {
            case eTF_8888:
                rgba[i * 4] = data[i * 4 + 2]; rgba[i * 4 + 1] = data[i * 4 + 1];
                rgba[i * 4 + 2] = data[i * 4]; rgba[i * 4 + 3] = data[i * 4 + 3];
                break;
            case eTF_RGBA:
                memcpy(&rgba[i * 4], &data[i * 4], 4);
                break;
            case eTF_8000:
                rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
                rgba[i * 4 + 3] = data[i];
                break;
            default:
                return false;
            }
        }
        return m_frameRenderer->RegisterLegacyRgbaTextureRegion(textureId,
            static_cast<uint32_t>(x), static_cast<uint32_t>(y),
            static_cast<uint32_t>(width), static_cast<uint32_t>(height), rgba.data());
    }

    void FontSetTexture(int textureId, int) override
    {
        m_fontTextureId = textureId;
    }

    void FontSetRenderingState(unsigned long, unsigned long) override
    {
        m_fontRenderingState = true;
    }

    void FontRestoreRenderingState() override
    {
        m_fontRenderingState = false;
    }

    void RemoveTexture(unsigned int textureId) override
    {
        m_textureWrapOverrides.erase(static_cast<int>(textureId));
        if (m_frameRenderer)
            m_frameRenderer->ReleaseLegacyTexture(static_cast<int>(textureId));
        CNULLRenderer::RemoveTexture(textureId);
    }

    unsigned int DownLoadToVideoMemory(unsigned char* data, int width, int height,
                                       ETEX_Format sourceFormat, ETEX_Format destinationFormat,
                                       int mipCount, bool repeat, int filter, int textureId,
                                       char* cacheName, int flags) override
    {
        if (!data || width <= 0 || height <= 0)
            return 0;
        if (mipCount == 0)
            flags |= FT_NOMIPS;
        if (!repeat)
            flags |= FT_CLAMP;
        if (destinationFormat == eTF_8888 || destinationFormat == eTF_RGBA)
            flags |= FT_HASALPHA;

        char generatedName[128];
        const char* name = cacheName;
        if (!name || !name[0])
        {
            sprintf(generatedName, "$AutoVulkan_%d", m_TexGenID++);
            name = generatedName;
        }
        int filterMode = eVTF_Trilinear;
        switch (filter)
        {
        case FILTER_LINEAR: filterMode = eVTF_Linear; break;
        case FILTER_BILINEAR: filterMode = eVTF_Bilinear; break;
        case FILTER_TRILINEAR: filterMode = eVTF_Trilinear; break;
        default: break;
        }
        m_vulkanTexMan->SetFilterOverride(filterMode);
        STexPic* texture = m_TexMan->CreateTexture(name, width, height, 1, flags, 0, data,
            eTT_Base, -1.0f, -1.0f, 0, nullptr, textureId, sourceFormat);
        m_vulkanTexMan->ClearFilterOverride();
        return texture ? texture->GetTextureID() : 0;
    }

    void UpdateTextureInVideoMemory(uint textureId, unsigned char* data, int x, int y,
                                    int width, int height, ETEX_Format sourceFormat) override
    {
        if (!m_frameRenderer || !data || width <= 0 || height <= 0 || x < 0 || y < 0)
            return;
        STexPic* texture = m_TexMan->GetByID(static_cast<int>(textureId));
        if (!texture || texture->m_Bind != static_cast<int>(textureId))
            return;

        const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
        std::vector<uint8_t> rgba(pixelCount * 4);
        if (sourceFormat == eTF_0565 || sourceFormat == eTF_0555 ||
            sourceFormat == eTF_1555 || sourceFormat == eTF_4444 ||
            sourceFormat == eTF_RGB8 || sourceFormat == eTF_0088)
        {
            if (!DecodeSpecialTexture(sourceFormat, data, pixelCount, rgba))
                return;
            m_frameRenderer->RegisterLegacyRgbaTextureRegion(static_cast<int>(textureId),
                static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                static_cast<uint32_t>(width), static_cast<uint32_t>(height), rgba.data());
            return;
        }
        for (size_t i = 0; i < pixelCount; ++i)
        {
            switch (sourceFormat)
            {
            case eTF_8888:
                rgba[i * 4] = data[i * 4 + 2]; rgba[i * 4 + 1] = data[i * 4 + 1];
                rgba[i * 4 + 2] = data[i * 4]; rgba[i * 4 + 3] = data[i * 4 + 3];
                break;
            case eTF_RGBA:
                memcpy(&rgba[i * 4], &data[i * 4], 4);
                break;
            case eTF_0888:
                rgba[i * 4] = data[i * 3 + 2]; rgba[i * 4 + 1] = data[i * 3 + 1];
                rgba[i * 4 + 2] = data[i * 3]; rgba[i * 4 + 3] = 255;
                break;
            case eTF_8000:
                rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
                rgba[i * 4 + 3] = data[i];
                break;
            default:
                return;
            }
        }
        m_frameRenderer->RegisterLegacyRgbaTextureRegion(static_cast<int>(textureId),
            static_cast<uint32_t>(x), static_cast<uint32_t>(y),
            static_cast<uint32_t>(width), static_cast<uint32_t>(height), rgba.data());
    }

    void Update() override
    {
        CNULLRenderer::Update();
        m_frameOpen = false;
    }

    void EF_EndEf3D(int flags) override
    {
        static unsigned int endEfAuditCalls = 0;
        const unsigned int endEfAuditCall = endEfAuditCalls++;
        if (endEfAuditCall < 12 || (endEfAuditCall % 120) == 0)
            CryLogAlways("Vulkan EF_EndEf3D path: call=%u frame=%d open=%u recurse=%d renderer=%p flags=0x%x",
                endEfAuditCall, GetFrameID(), m_frameOpen ? 1u : 0u,
                SRendItem::m_RecurseLevel - 1, m_frameRenderer, flags);
        // XRenderNULL deliberately drops the stock renderer's render-item
        // queues. Execute the opaque stock items here while their frame data
        // and visibility-object table are still valid.
        const int recurseLevel = SRendItem::m_RecurseLevel - 1;
        if (m_frameOpen && recurseLevel >= 0 && recurseLevel < 8)
        {
            m_RP.m_RealTime = iTimer ? iTimer->GetCurrTime() : 0.0f;
            static const int stockListOrder[NUMRI_LISTS] = {
                EFSLIST_PREPROCESS_ID,
                EFSLIST_STENCIL_ID,
                EFSLIST_GENERAL_ID,
                EFSLIST_UNSORTED_ID,
                EFSLIST_DISTSORT_ID,
                EFSLIST_LAST_ID
            };
            for (int list = 0; list < NUMRI_LISTS; ++list)
                SRendItem::m_EndRI[recurseLevel][list] = SRendItem::m_RendItems[list].Num();

            // Capture several scene frames after the renderer is active. Keep
            // this diagnostic bounded: the full item and draw trace is useful
            // for a device log, but must not grow for the lifetime of a game.
            static int auditFramesRemaining = 2;
            int auditItemTotal = 0;
            bool auditHasWorldLeaf = false;
            for (int list = 0; list < NUMRI_LISTS; ++list)
            {
                const int begin = SRendItem::m_StartRI[recurseLevel][list];
                const int end = SRendItem::m_EndRI[recurseLevel][list];
                if (begin >= 0 && end > begin)
                {
                    auditItemTotal += end - begin;
                    for (int index = begin; index < end; ++index)
                    {
                        CRendElement* element = SRendItem::m_RendItems[list][index].Item;
                        if (element && element->mfGetType() == eDATA_OcLeaf)
                        {
                            auditHasWorldLeaf = true;
                            break;
                        }
                    }
                }
            }
            m_auditFrameActive = auditFramesRemaining > 0 && auditHasWorldLeaf &&
                m_frameRenderer && m_frameRenderer->IsFrameActive();
            if (m_auditFrameActive)
            {
                --auditFramesRemaining;
                CryLogAlways("Vulkan audit BEGIN frame=%d recurse=%d items=%d visObjects=%d lights=%d flags=0x%x pers=0x%x cameraPos=(%.3f,%.3f,%.3f)",
                    GetFrameID(), recurseLevel, auditItemTotal, m_RP.m_NumVisObjects,
                    m_RP.m_DLights[recurseLevel].Num(), m_RP.m_Flags, m_RP.m_PersFlags,
                    m_cam.GetPos().x, m_cam.GetPos().y, m_cam.GetPos().z);
                for (int list = 0; list < NUMRI_LISTS; ++list)
                {
                    const int begin = SRendItem::m_StartRI[recurseLevel][list];
                    const int end = SRendItem::m_EndRI[recurseLevel][list];
                    CryLogAlways("Vulkan audit LIST frame=%d list=%d range=[%d,%d) count=%d",
                        GetFrameID(), list, begin, end, end > begin ? end - begin : 0);
                }
                const float* cameraMatrix = m_CameraMatrix.GetData();
                CryLogAlways("Vulkan audit CAMERA row0=(%.6f,%.6f,%.6f,%.6f) row1=(%.6f,%.6f,%.6f,%.6f) row2=(%.6f,%.6f,%.6f,%.6f) row3=(%.6f,%.6f,%.6f,%.6f)",
                    cameraMatrix[0], cameraMatrix[1], cameraMatrix[2], cameraMatrix[3],
                    cameraMatrix[4], cameraMatrix[5], cameraMatrix[6], cameraMatrix[7],
                    cameraMatrix[8], cameraMatrix[9], cameraMatrix[10], cameraMatrix[11],
                    cameraMatrix[12], cameraMatrix[13], cameraMatrix[14], cameraMatrix[15]);
            }

            for (int order = 0; order < NUMRI_LISTS; ++order)
            {
                const int list = stockListOrder[order];
                int drawFirst = SRendItem::m_StartRI[recurseLevel][list];
                int last = SRendItem::m_EndRI[recurseLevel][list];
                if (drawFirst < 0 || drawFirst >= last)
                    continue;
                CCObject* savedObject = m_RP.m_pCurObject;
                // OpenGL runs EF_PreRender(1) once for every non-empty list,
                // before sorting and preprocess handling.
                PrepareStockFrameState(recurseLevel);

                // Match the stock XRenderOGL list preparation: shader-sort the
                // preprocess/general/last lists, distance-sort blended items,
                // and keep stencil clear/shadow runs in stencil-specific order.
                if (list == EFSLIST_PREPROCESS_ID || list == EFSLIST_GENERAL_ID ||
                    list == EFSLIST_LAST_ID)
                {
                    SRendItem::mfSort(&SRendItem::m_RendItems[list][drawFirst], last - drawFirst);
                    if ((SRendItem::m_RendItems[list][drawFirst].SortVal.i.High >> 26) == eS_PreProcess)
                    {
                        // The NULL renderer has no stock preprocess executor. Do
                        // not accidentally submit its reflection/refraction/etc.
                        // source items as ordinary world geometry; consume the
                        // same leading preprocess run the GL pipeline removes
                        // after executing its off-screen passes.
                        while (drawFirst < last &&
                               (SRendItem::m_RendItems[list][drawFirst].SortVal.i.High >> 26) == eS_PreProcess)
                            ++drawFirst;
                    }
                }
                else if (list == EFSLIST_DISTSORT_ID)
                    SRendItem::mfSortByDist(&SRendItem::m_RendItems[list][drawFirst], last - drawFirst);
                else if (list == EFSLIST_STENCIL_ID)
                {
                    int runStart = drawFirst;
                    for (int itemIndex = drawFirst; itemIndex < last; ++itemIndex)
                    {
                        SRendItemPre* item = &SRendItem::m_RendItems[list][itemIndex];
                        const int type = item->Item ? item->Item->mfGetType() : -1;
                        if (type != eDATA_ClearStencil && type != eDATA_TriMeshShadow)
                            continue;
                        SRendItem::mfSortForStencil(&SRendItem::m_RendItems[list][runStart],
                                                    itemIndex - runStart);
                        do
                        {
                            ++itemIndex;
                            if (itemIndex >= last)
                                break;
                            item = &SRendItem::m_RendItems[list][itemIndex];
                            const int nextType = item->Item ? item->Item->mfGetType() : -1;
                            if (nextType != eDATA_ClearStencil && nextType != eDATA_TriMeshShadow)
                                break;
                        } while (true);
                        runStart = itemIndex;
                        --itemIndex;
                    }
                    SRendItem::mfSortForStencil(&SRendItem::m_RendItems[list][runStart],
                                                last - runStart);
                }

                // This is the state established by EF_PreRender(3) and the
                // list walker immediately before entering its item loop.
                float minDepthRange = 0.0f;
                float maxDepthRange = 1.0f;
                if (m_RP.m_WasPortals > 0)
                {
                    if (m_RP.m_CurPortal == 0)
                        maxDepthRange = 0.5f;
                    else
                    {
                        const float portalDepthSpan = 0.5f /
                            static_cast<float>(m_RP.m_WasPortals);
                        minDepthRange = 0.5f + portalDepthSpan *
                            static_cast<float>(m_RP.m_CurPortal - 1);
                        maxDepthRange = minDepthRange + portalDepthSpan;
                    }
                }
                m_frameRenderer->SetStockDepthRange(minDepthRange, maxDepthRange);
                m_RP.m_pCurObject = m_RP.m_NumVisObjects > 0 ? m_RP.m_VisObjects[0] : nullptr;
                m_RP.m_pPrevObject = m_RP.m_pCurObject;
                m_RP.m_Flags |= RBF_3D;
                SShader* previousStateShader = nullptr;
                int previousObjectIndex = -1;
                for (int itemIndex = drawFirst; itemIndex < last; ++itemIndex)
                {
                    SRendItemPre* item = &SRendItem::m_RendItems[list][itemIndex];
                    if (!item->Item)
                    {
                        if (m_auditFrameActive)
                            CryLogAlways("Vulkan audit ITEM frame=%d list=%d index=%d element=NULL sort=0x%llx",
                                GetFrameID(), list, itemIndex,
                                static_cast<unsigned long long>(item->SortVal.SortVal));
                        continue;
                    }

                    int objectIndex = 0;
                    SShader* shader = nullptr;
                    SShader* shaderState = nullptr;
                    SRenderShaderResources* resources = nullptr;
                    int fogIndex = 0;
                    SRendItem::mfGet(item->SortVal, &objectIndex, &shader,
                                     &shaderState, &fogIndex, &resources);
                    if (objectIndex < 0 || objectIndex >= m_RP.m_NumVisObjects)
                    {
                        if (m_auditFrameActive)
                            CryLogAlways("Vulkan audit SKIP frame=%d list=%d index=%d reason=object-index object=%d visObjects=%d elementType=%d sort=0x%llx",
                                GetFrameID(), list, itemIndex, objectIndex, m_RP.m_NumVisObjects,
                                item->Item->mfGetType(),
                                static_cast<unsigned long long>(item->SortVal.SortVal));
                        continue;
                    }

                    CCObject* renderObject = m_RP.m_VisObjects[objectIndex];
                    if (!renderObject)
                    {
                        if (m_auditFrameActive)
                            CryLogAlways("Vulkan audit SKIP frame=%d list=%d index=%d reason=null-object object=%d elementType=%d",
                                GetFrameID(), list, itemIndex, objectIndex, item->Item->mfGetType());
                        continue;
                    }
                    if ((renderObject->m_ObjFlags & FOB_NEAREST) &&
                        ((m_RP.m_PersFlags & RBPF_DONTDRAWNEAREST) || CV_r_nodrawnear))
                    {
                        if (m_auditFrameActive)
                            CryLogAlways("Vulkan audit SKIP frame=%d list=%d index=%d reason=nearest object=%d objFlags=0x%x pers=0x%x cvar=%d",
                                GetFrameID(), list, itemIndex, objectIndex, renderObject->m_ObjFlags,
                                m_RP.m_PersFlags, CV_r_nodrawnear);
                        continue;
                    }
                    if (shader && (m_RP.m_PersFlags & RBPF_ONLYREFRACTED) &&
                        !(renderObject->m_ObjFlags & FOB_REFRACTED))
                    {
                        if (m_auditFrameActive)
                            CryLogAlways("Vulkan audit SKIP frame=%d list=%d index=%d reason=only-refracted shader=%s objFlags=0x%x",
                                GetFrameID(), list, itemIndex, shader->m_Name.c_str(), renderObject->m_ObjFlags);
                        continue;
                    }
                    if (shader && (m_RP.m_PersFlags & RBPF_IGNOREREFRACTED) &&
                        (renderObject->m_ObjFlags & FOB_REFRACTED))
                    {
                        if (m_auditFrameActive)
                            CryLogAlways("Vulkan audit SKIP frame=%d list=%d index=%d reason=ignore-refracted shader=%s objFlags=0x%x",
                                GetFrameID(), list, itemIndex, shader->m_Name.c_str(), renderObject->m_ObjFlags);
                        continue;
                    }

                    if (m_auditFrameActive)
                    {
                        m_auditCurrentList = list;
                        m_auditCurrentItem = itemIndex;
                        CMatInfo* chunk = item->Item->mfGetMatInfo();
                        CryLogAlways("Vulkan audit ITEM frame=%d list=%d index=%d type=%d shader=%s shaderId=%d sort=%d obj=%d objFlags=0x%x pos=(%.3f,%.3f,%.3f) fog=%d resources=%d opacity=%.3f dynLights=0x%x fixedPasses=%d hwTechniques=%d stateShader=%s chunk=%d+%d verts=%d firstVert=%d",
                            GetFrameID(), list, itemIndex, item->Item->mfGetType(),
                            shader ? shader->m_Name.c_str() : "<null>", shader ? shader->m_Id : -1,
                            item->Item->m_SortId, objectIndex, renderObject->m_ObjFlags,
                            renderObject->GetTranslation().x, renderObject->GetTranslation().y,
                            renderObject->GetTranslation().z, fogIndex,
                            resources ? resources->m_Id : -1, resources ? resources->m_Opacity : 1.0f,
                            item->DynLMask, shader ? shader->m_Passes.Num() : 0,
                            shader ? shader->m_HWTechniques.Num() : 0,
                            shaderState ? shaderState->m_Name.c_str() : "<null>",
                            chunk ? chunk->nFirstIndexId : -1, chunk ? chunk->nNumIndices : -1,
                            chunk ? chunk->nNumVerts : -1, chunk ? chunk->nFirstVertId : -1);
                        if (resources)
                        {
                            const int slots[] = { EFTT_DIFFUSE, EFTT_BUMP_DIFFUSE,
                                                  EFTT_BUMP, EFTT_NORMALMAP,
                                                  EFTT_LIGHTMAP, EFTT_LIGHTMAP_DIR };
                            for (int slot : slots)
                            {
                                SEfResTexture* texture = resources->m_Textures[slot];
                                if (!texture) continue;
                                ITexPic* image = texture->m_TU.m_ITexPic;
                                CryLogAlways("Vulkan audit RESOURCE frame=%d list=%d index=%d slot=%d name=%s path=%s tex=%p loaded=%d id=%d",
                                    GetFrameID(), list, itemIndex, slot,
                                    texture->m_Name.c_str(), resources->m_TexturePath.c_str(),
                                    image, image ? image->IsTextureLoaded() : 0,
                                    image ? image->GetTextureID() : -1);
                            }
                        }
                        if (shader)
                        {
                            for (int passIndex = 0; passIndex < shader->m_Passes.Num(); ++passIndex)
                            {
                                const SShaderPass& pass = shader->m_Passes[passIndex];
                                CryLogAlways("Vulkan audit FIXED_PASS frame=%d list=%d index=%d pass=%d flags=0x%x evalRGB=%d evalAlpha=%d state=0x%x second=0x%x textures=%d",
                                    GetFrameID(), list, itemIndex, passIndex, pass.m_Flags,
                                    pass.m_eEvalRGB, pass.m_eEvalAlpha, pass.m_RenderState,
                                    pass.m_SecondRenderState, pass.m_TUnits.Num());
                            }
                            for (int techIndex = 0; techIndex < shader->m_HWTechniques.Num(); ++techIndex)
                            {
                                const SShaderTechnique* technique = shader->m_HWTechniques[techIndex];
                                CryLogAlways("Vulkan audit TECHNIQUE frame=%d list=%d index=%d tech=%d ptr=%p passes=%d pointers=%d flags=0x%x cull=%d",
                                    GetFrameID(), list, itemIndex, techIndex, technique,
                                    technique ? technique->m_Passes.Num() : -1,
                                    technique ? technique->m_Pointers.Num() : -1,
                                    technique ? technique->m_Flags : 0,
                                    technique ? technique->m_eCull : static_cast<ECull>(-1));
                                if (technique)
                                {
                                    for (int pointerIndex = 0;
                                         pointerIndex < technique->m_Pointers.Num(); ++pointerIndex)
                                    {
                                        const SArrayPointer* pointer = technique->m_Pointers[pointerIndex];
                                        if (!pointer) continue;
                                        CryLogAlways("Vulkan audit TECH_POINTER frame=%d list=%d index=%d tech=%d pointer=%d source=%d stage=%d type=%d components=%d",
                                            GetFrameID(), list, itemIndex, techIndex, pointerIndex,
                                            pointer->ePT, pointer->Stage, pointer->Type,
                                            pointer->NumComponents);
                                    }
                                    for (int passIndex = 0; passIndex < technique->m_Passes.Num(); ++passIndex)
                                    {
                                        const SShaderPassHW& pass = technique->m_Passes[passIndex];
                                        CryLogAlways("Vulkan audit HW_PASS frame=%d list=%d index=%d tech=%d pass=%d type=%d state=0x%x second=0x%x lmFlags=0x%x textures=%d pointers=%d",
                                            GetFrameID(), list, itemIndex, techIndex, passIndex,
                                            pass.m_ePassType, pass.m_RenderState, pass.m_SecondRenderState,
                                            pass.m_LMFlags, pass.m_TUnits.Num(), pass.m_Pointers.Num());
                                    }
                                }
                            }
                        }
                    }

                    if (renderObject != m_RP.m_pCurObject)
                    {
                        ++m_RP.m_FrameObject;
                        m_RP.m_pPrevObject = m_RP.m_pCurObject;
                        m_RP.m_pCurObject = renderObject;
                        UpdateLegacyObjectTransform(renderObject);
                    }
                    m_RP.m_pRE = item->Item;
                    m_RP.m_pShader = shader;
                    m_RP.m_pShaderResources = resources;
                    m_RP.m_DynLMask = item->DynLMask;
                    m_RP.m_ObjFlags = renderObject->m_ObjFlags;
                    m_RP.m_fCurOpacity = resources ? resources->m_Opacity : 1.0f;
                    if (item->Item->mfGetType() == eDATA_Beam)
                    {
                        // OpenGL's item prepare loads the beam model and
                        // replaces the queued shader/resource/element state
                        // with the model's first material before flushing it.
                        CREBeam* beam = static_cast<CREBeam*>(item->Item);
                        beam->mfPrepare();
                        if (!beam->m_pBuffer || !m_RP.m_pRE ||
                            m_RP.m_RendNumIndices <= 0 || m_RP.m_RendNumVerts <= 0)
                            continue;
                        shader = m_RP.m_pShader;
                        resources = m_RP.m_pShaderResources;
                        m_RP.m_fCurOpacity = resources ? resources->m_Opacity : 1.0f;
                    }
                    if (shader && m_RP.m_pIgnoreObject && !(shader->m_Flags & EF_SKY) &&
                        IsEquivalent(m_RP.m_pIgnoreObject->GetTranslation(),
                                     renderObject->GetTranslation()))
                        continue;
                    if (objectIndex != previousObjectIndex)
                    {
                        PrepareStockSkinnedObject(renderObject, shader, item->Item);
                        previousObjectIndex = objectIndex;
                    }
                    // OpenGL's EF_Start builds this list before selecting the
                    // hardware technique. EF_SelectHWTechnique consults both
                    // m_NumActiveDLights and m_fCurOpacity; leaving those
                    // fields stale makes light-count/opacity conditions pick
                    // the wrong technique (or reject every valid one).
                    EF_BuildLightsList();
                    // CGLRenderer::EF_Start resets this for each shader/item
                    // batch. Vulkan emits the passes directly, so preserve the
                    // same per-item lifetime explicitly.
                    m_RP.m_RendPass = 0;
                    m_RP.m_pFogVolume = fogIndex > 0 && fogIndex < m_RP.m_FogVolumes.Num() ?
                        &m_RP.m_FogVolumes[fogIndex] : nullptr;
                    if (shader && (shader->m_Flags & EF_FOGSHADER) && !m_RP.m_pFogVolume)
                        continue;
                    m_activeStateShaderState = shaderState ? shaderState->m_State : nullptr;
                    if (shaderState != previousStateShader && m_activeStateShaderState)
                    {
                        if (m_activeStateShaderState->m_bClearStencil &&
                            !m_frameRenderer->QueueStockClearStencil())
                            m_frameRenderer->RequirePanelFallback();
                        if (m_activeStateShaderState->m_Stencil)
                        {
                            // The scene pipeline decoder consumes OpenGL's
                            // packed FSS compare/op bits directly and carries
                            // the reference and mask as dynamic stencil state.
                            m_CurStencilState = m_activeStateShaderState->m_Stencil->m_State;
                            m_CurStencRef = m_activeStateShaderState->m_Stencil->m_FuncRef;
                            m_CurStencMask = m_activeStateShaderState->m_Stencil->m_FuncMask;
                        }
                    }
                    previousStateShader = shaderState;

                    if (m_auditFrameActive && !shader)
                        CryLogAlways("Vulkan audit NO_SHADER frame=%d list=%d index=%d type=%d re=%p; shader dispatch will submit no geometry",
                            GetFrameID(), list, itemIndex, item->Item->mfGetType(), item->Item);

                    // Port the non-batching work of CREOcLeaf::mfPrepare.
                    // Its GL pipe-array writes are deliberately omitted: the
                    // Vulkan leaf path submits the chunk directly from its
                    // vertex/index buffers below.
                    if (item->Item->mfGetType() == eDATA_OcLeaf &&
                        !PrepareStockLeafItem(static_cast<CREOcLeaf*>(item->Item), renderObject))
                    {
                        m_activeStateShaderState = nullptr;
                        continue;
                    }

                    // XRenderOGL::EF_FlushShader dispatches to EF_FlushHW
                    // whenever the shader owns hardware techniques. Preserve
                    // that selection here: choosing m_Passes first can pass a
                    // fixed-function pass to CREOcLeaf even though its mfDraw
                    // expects SShaderPassHW when m_HWTechniques is nonempty.
                    if (shader && shader->m_HWTechniques.Num() > 0)
                    {
                        const int techniqueIndex = EF_SelectHWTechnique(shader);
                        if (m_auditFrameActive)
                            CryLogAlways("Vulkan audit SELECT_TECHNIQUE frame=%d list=%d index=%d shader=%s selected=%d candidates=%d opacity=%.3f activeLights=%d fog=%p",
                                GetFrameID(), list, itemIndex, shader->m_Name.c_str(), techniqueIndex,
                                shader->m_HWTechniques.Num(), m_RP.m_fCurOpacity,
                                m_RP.m_NumActiveDLights, m_RP.m_pFogVolume);
                        if (techniqueIndex >= 0 && techniqueIndex < shader->m_HWTechniques.Num())
                        {
                            SShaderTechnique* technique = shader->m_HWTechniques[techniqueIndex];
                            m_RP.m_pCurTechnique = technique;
                            bool submittedTranslatedTechniquePass = false;
                            const int cull = technique->m_eCull == static_cast<ECull>(-1) ?
                                MapStockCullMode(shader->m_eCull) :
                                MapStockCullMode(technique->m_eCull);
                            for (int passIndex = 0; passIndex < technique->m_Passes.Num(); ++passIndex)
                            {
                                SShaderPassHW* pass = &technique->m_Passes[passIndex];
                                if (pass->m_ePassType == eSHP_General)
                                {
                                    const bool firstOpaquePass = passIndex == 0 &&
                                        !(m_RP.m_ObjFlags & FOB_LIGHTPASS);
                                    const uint32_t state = firstOpaquePass ? pass->m_RenderState :
                                        pass->m_SecondRenderState;
                                    DrawRenderItem(item->Item, shader, pass, resources, state, cull, true);
                                    submittedTranslatedTechniquePass = true;
                                    continue;
                                }

                                if (pass->m_ePassType == eSHP_MultiLights)
                                {
                                    // OpenGL batches several direct lights into a
                                    // programmable PS30 draw. Vulkan approximates
                                    // that shader with additive per-light stock
                                    // lighting draws while preserving the pass's
                                    // light-type, light-map and occlusion filters.
                                    const int lightLevel = SRendItem::m_RecurseLevel;
                                    if (m_RP.m_fCurOpacity != 1.0f && m_RP.m_RendPass != 0)
                                        continue;
                                    if (lightLevel < 0 || lightLevel >= 8)
                                        continue;
                                    const bool hasObjectLightmap = renderObject->m_nLMId != 0;
                                    const uint32_t itemLightMask = m_RP.m_DynLMask;
                                    uint32_t matchingLightMask = 0;
                                    bool hasOccludedMatchingLight = false;
                                    if (!(pass->m_LMFlags & LMF_DISABLE))
                                    {
                                        for (int lightIndex = 0;
                                             lightIndex < m_RP.m_DLights[lightLevel].Num() && lightIndex < 32;
                                             ++lightIndex)
                                        {
                                            if (!(itemLightMask & (1u << lightIndex)))
                                                continue;
                                            CDLight* light = m_RP.m_DLights[lightLevel][lightIndex];
                                            if (!light || !(light->m_Flags & (DLF_DIRECTIONAL | DLF_POINT)))
                                                continue;
                                            if (hasObjectLightmap && (light->m_Flags & DLF_LM) &&
                                                (pass->m_LMFlags & LMF_NOSPECULAR))
                                                continue;
                                            matchingLightMask |= 1u << lightIndex;
                                            const uint8_t encodedLightId =
                                                static_cast<uint8_t>(light->m_Id + 1);
                                            for (int occlusionIndex = 0; occlusionIndex < 4; ++occlusionIndex)
                                            {
                                                if (renderObject->m_OcclLights[occlusionIndex] == encodedLightId)
                                                {
                                                    hasOccludedMatchingLight = true;
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                    const bool occlusionPassEligible =
                                        !(pass->m_LMFlags & LMF_USEOCCLUSIONMAP) ||
                                        hasOccludedMatchingLight;
                                    const bool hasAmbientLightPass =
                                        (pass->m_LMFlags & LMF_HASAMBIENT) &&
                                        !(pass->m_LMFlags & LMF_DISABLE) &&
                                        !(pass->m_LMFlags & LMF_NOAMBIENT) &&
                                        !(m_RP.m_ObjFlags & FOB_LIGHTPASS) &&
                                        (!(pass->m_LMFlags & LMF_HASDOT3LM) ||
                                         (resources && renderObject->m_nLMDirId));
                                    if ((matchingLightMask || hasAmbientLightPass) && occlusionPassEligible)
                                    {
                                        m_RP.m_DynLMask = matchingLightMask;
                                        const bool firstOpaquePass = passIndex == 0 &&
                                            !(m_RP.m_ObjFlags & FOB_LIGHTPASS);
                                        const uint32_t state = firstOpaquePass ? pass->m_RenderState :
                                            pass->m_SecondRenderState;
                                        DrawRenderItem(item->Item, shader, pass, resources, state, cull, true);
                                        submittedTranslatedTechniquePass = true;
                                    }
                                    m_RP.m_DynLMask = itemLightMask;
                                    continue;
                                }

                                // OpenGL executes Light/DiffuseLight passes once
                                // per matching active light. Use Vulkan's
                                // material diffuse path for the corresponding
                            // common case and scope the item light mask to
                                // that one light. Specular uses a half-vector
                                // approximation; shadow/program passes still
                                // need their dedicated shader semantics.
                                if (pass->m_ePassType == eSHP_Light ||
                                    pass->m_ePassType == eSHP_DiffuseLight ||
                                    pass->m_ePassType == eSHP_SpecularLight)
                                {
                                    const int lightLevel = SRendItem::m_RecurseLevel;
                                    if (m_RP.m_fCurOpacity != 1.0f && m_RP.m_RendPass != 0)
                                        continue;
                                    if (lightLevel < 0 || lightLevel >= 8)
                                        continue;
                                    const uint32_t itemLightMask = m_RP.m_DynLMask;
                                    CDLight* const previousLight = m_RP.m_pCurLight;
                                    const int previousLightId = m_RP.m_nCurLight;
                                    const bool hasDirectionalLightmap =
                                        (resources && resources->m_Textures[EFTT_LIGHTMAP_DIR]) ||
                                        renderObject->m_nLMId;
                                    for (int lightIndex = 0;
                                         lightIndex < m_RP.m_DLights[lightLevel].Num() && lightIndex < 32;
                                         ++lightIndex)
                                    {
                                        if (!(itemLightMask & (1u << lightIndex)))
                                            continue;
                                        CDLight* light = m_RP.m_DLights[lightLevel][lightIndex];
                                        if (!light || (pass->m_LMFlags & LMF_DISABLE))
                                            continue;
                                        if ((pass->m_LMFlags & LMF_IGNOREPROJLIGHTS) &&
                                            (light->m_Flags & DLF_PROJECT))
                                            continue;
                                        if (!(light->m_Flags &
                                              (DLF_DIRECTIONAL | DLF_POINT | DLF_PROJECT)))
                                        {
                                            m_frameRenderer->RequirePanelFallback();
                                            continue;
                                        }
                                        const int passLightTypes = pass->m_LightFlags & DLF_LIGHTTYPE_MASK;
                                        if (passLightTypes &&
                                            !(passLightTypes & (light->m_Flags & DLF_LIGHTTYPE_MASK)))
                                            continue;
                                        if (pass->m_LightFlags & DLF_LM)
                                        {
                                            if (!(light->m_Flags & DLF_LM) || !hasDirectionalLightmap)
                                                continue;
                                            if (light->m_SpecColor.r <= 0.01f &&
                                                light->m_SpecColor.g <= 0.01f &&
                                                light->m_SpecColor.b <= 0.01f)
                                                continue;
                                        }
                                        if ((pass->m_LMFlags & LMF_USEOCCLUSIONMAP) &&
                                            *reinterpret_cast<const int*>(renderObject->m_OcclLights) == 0)
                                            continue;
                                        if ((pass->m_LMFlags & LMF_NOBUMP) && resources &&
                                            (!resources->m_Textures[EFTT_BUMP] ||
                                             !(resources->m_Textures[EFTT_BUMP]->m_TU.m_nFlags & FTU_NOBUMP)))
                                            continue;
                                        if ((pass->m_ePassType == eSHP_DiffuseLight) &&
                                            (light->m_Flags & DLF_LM) &&
                                            hasDirectionalLightmap)
                                            continue;
                                        if (pass->m_ePassType == eSHP_SpecularLight &&
                                            light->m_SpecColor.r <= 0.01f &&
                                            light->m_SpecColor.g <= 0.01f &&
                                            light->m_SpecColor.b <= 0.01f)
                                            continue;

                                        m_RP.m_DynLMask = 1u << lightIndex;
                                        m_RP.m_pCurLight = light;
                                        m_RP.m_nCurLight = light->m_Id;
                                        SLightIndicies* const previousLightIndices =
                                            m_RP.m_pCurLightIndices;
                                        m_RP.m_pCurLightIndices = &m_RP.m_FakeLightIndices;
                                        if (CullGeometryForLightsEnabled() &&
                                            EF_IsOnlyLightPass(pass) &&
                                            item->Item->mfGetType() == eDATA_OcLeaf)
                                        {
                                            m_RP.m_pCurLightIndices =
                                                static_cast<CREOcLeaf*>(item->Item)->mfGetIndiciesForLight(light);
                                            if (!m_RP.m_pCurLightIndices)
                                            {
                                                m_RP.m_pCurLightIndices = previousLightIndices;
                                                continue;
                                            }
                                        }
                                        const bool firstOpaquePass = passIndex == 0 &&
                                            !(m_RP.m_ObjFlags & FOB_LIGHTPASS);
                                        const uint32_t state = firstOpaquePass ? pass->m_RenderState :
                                            pass->m_SecondRenderState;
                                        DrawRenderItem(item->Item, shader, pass, resources, state, cull, true);
                                        m_RP.m_pCurLightIndices = previousLightIndices;
                                        submittedTranslatedTechniquePass = true;
                                    }
                                    m_RP.m_DynLMask = itemLightMask;
                                    m_RP.m_pCurLight = previousLight;
                                    m_RP.m_nCurLight = previousLightId;
                                }
                                else if (pass->m_ePassType == eSHP_Fur ||
                                         pass->m_ePassType == eSHP_SimulatedFur)
                                {
                                    // OpenGL's EF_DrawFurPasses currently only
                                    // marks the flush and clears its current
                                    // light; it does not submit fur geometry.
                                    // Preserve that behavior instead of
                                    // forcing a panel fallback for a no-op.
                                    m_RP.m_FlagsPerFlush |= RBSI_FURPASS;
                                    m_RP.m_pCurLight = nullptr;
                                }
                                else
                                {
                                    if (m_auditFrameActive)
                                        CryLogAlways("Vulkan audit UNTRANSLATED_HW_PASS frame=%d list=%d index=%d shader=%s pass=%d type=%d",
                                            GetFrameID(), list, itemIndex, shader->m_Name.c_str(),
                                            passIndex, pass->m_ePassType);
                                    m_frameRenderer->RequirePanelFallback();
                                }
                            }

                            // Some legacy shaders expose a programmable
                            // technique made only of specialized light/shadow
                            // passes while also retaining fixed-function
                            // passes. Vulkan cannot execute the Cg/NVParse
                            // program yet; keep the object visible through its
                            // stock fixed-function description instead of
                            // dropping it completely. Do not combine the two
                            // paths when a General pass was submitted, since
                            // that would render opaque geometry twice.
                            if (!submittedTranslatedTechniquePass && shader->m_Passes.Num() > 0)
                            {
                                for (int passIndex = 0; passIndex < shader->m_Passes.Num(); ++passIndex)
                                {
                                    SShaderPass* pass = &shader->m_Passes[passIndex];
                                    const bool firstOpaquePass = passIndex == 0 &&
                                        !(m_RP.m_ObjFlags & FOB_LIGHTPASS);
                                    const uint32_t state = firstOpaquePass ? pass->m_RenderState :
                                        pass->m_SecondRenderState;
                                    DrawRenderItem(item->Item, shader, pass, resources, state,
                                                   MapStockCullMode(shader->m_eCull));
                                }
                            }
                        }
                        else if (m_auditFrameActive)
                            CryLogAlways("Vulkan audit NO_TECHNIQUE frame=%d list=%d index=%d shader=%s flags=0x%x resources=%p opacity=%.3f lights=%d",
                                GetFrameID(), list, itemIndex, shader->m_Name.c_str(), shader->m_Flags,
                                resources, m_RP.m_fCurOpacity, m_RP.m_NumActiveDLights);
                    }
                    else if (shader && shader->m_Passes.Num() > 0)
                    {
                        for (int passIndex = 0; passIndex < shader->m_Passes.Num(); ++passIndex)
                        {
                            SShaderPass* pass = &shader->m_Passes[passIndex];
                            const bool firstOpaquePass = passIndex == 0 &&
                                !(m_RP.m_ObjFlags & FOB_LIGHTPASS);
                            const uint32_t state = firstOpaquePass ? pass->m_RenderState :
                                pass->m_SecondRenderState;
                            DrawRenderItem(item->Item, shader, pass, resources, state,
                                           MapStockCullMode(shader->m_eCull));
                        }
                    }
                    else if (shader && m_auditFrameActive)
                        CryLogAlways("Vulkan audit NO_EXECUTABLE_PASS frame=%d list=%d index=%d shader=%s hwTechniques=%d elementType=%d",
                            GetFrameID(), list, itemIndex, shader->m_Name.c_str(),
                            shader->m_HWTechniques.Num(), item->Item->mfGetType());
                    m_activeStateShaderState = nullptr;
                }
                // Mirror EF_PipeLine's per-list cleanup/state restoration.
                m_RP.m_pRE = nullptr;
                m_RP.m_pFogVolume = nullptr;
                m_RP.m_pCurTechnique = nullptr;
                m_RP.m_pShader = nullptr;
                m_RP.m_CurrentVLights = 0;
                m_RP.m_FlagsModificators = 0;
                m_RP.m_FlagsPerFlush = 0;
                m_RP.m_Flags &= ~RBF_3D;
                m_RP.m_pPrevObject = m_RP.m_NumVisObjects > 0 ? m_RP.m_VisObjects[0] : nullptr;
                m_RP.m_pCurObject = savedObject;
                m_ViewMatrix = m_CameraMatrix;
            }
        }

        m_auditFrameActive = false;
        CNULLRenderer::EF_EndEf3D(flags);
    }

    void DrawBuffer(CVertexBuffer* vertices, SVertexStream* indices, int indexCount,
                    int firstIndex, int primitiveType, int flags, int numVerts,
                    CMatInfo* materialInfo) override
    {
        if (m_auditFrameActive)
        {
            CryLogAlways("Vulkan audit DRAW_BUFFER frame=%d list=%d item=%d re=%p shader=%s pass=%p hwPass=%d prim=%d topologyInput=%d firstIndex=%d indexCount=%d numVerts=%d vb=%p vf=%d vbVerts=%d ib=%p ibItems=%d chunk=%p chunkRange=%d+%d state=0x%x cull=%d tex=(%d,%d)",
                GetFrameID(), m_auditCurrentList, m_auditCurrentItem, m_RP.m_pRE,
                m_RP.m_pShader ? m_RP.m_pShader->m_Name.c_str() : "<null>",
                m_activePass, m_activeHardwarePassType, primitiveType, primitiveType,
                firstIndex, indexCount, numVerts, vertices,
                vertices ? vertices->m_vertexformat : -1,
                vertices ? vertices->m_NumVerts : -1, indices,
                indices ? indices->m_nItems : -1, materialInfo,
                materialInfo ? materialInfo->nFirstIndexId : -1,
                materialInfo ? materialInfo->nNumIndices : -1,
                m_CurState, m_activeCull, CVulkanTexMan::m_TUState[0].m_Bind,
                CVulkanTexMan::m_TUState[1].m_Bind);
            if (indices && indices->m_VData && firstIndex >= 0 && indexCount > 0 &&
                firstIndex <= indices->m_nItems && indexCount <= indices->m_nItems - firstIndex)
            {
                const ushort* indexData = static_cast<const ushort*>(indices->m_VData) + firstIndex;
                ushort minIndex = 0xffff;
                ushort maxIndex = 0;
                for (int i = 0; i < indexCount; ++i)
                {
                    if (indexData[i] < minIndex) minIndex = indexData[i];
                    if (indexData[i] > maxIndex) maxIndex = indexData[i];
                }
                char firstIndices[160] = {};
                size_t used = 0;
                const int count = indexCount < 16 ? indexCount : 16;
                for (int i = 0; i < count && used < sizeof(firstIndices); ++i)
                {
                    const int written = snprintf(firstIndices + used,
                        sizeof(firstIndices) - used, "%s%u", i ? "," : "", indexData[i]);
                    if (written <= 0)
                        break;
                    used += static_cast<size_t>(written);
                }
                CryLogAlways("Vulkan audit INDICES frame=%d list=%d item=%d min=%u max=%u first=[%s]",
                    GetFrameID(), m_auditCurrentList, m_auditCurrentItem,
                    minIndex, maxIndex, firstIndices);
            }
        }
        const bool hasPrimitiveGroups = primitiveType == R_PRIMV_MULTI_GROUPS;
        if (!m_frameOpen || !m_frameRenderer || indexCount < 0 ||
            (indexCount == 0 && !hasPrimitiveGroups))
        {
            if (m_auditFrameActive)
                CryLogAlways("Vulkan audit DRAW_REJECT frame=%d list=%d item=%d reason=inactive-or-empty frameOpen=%d renderer=%p count=%d primitive=%d",
                    GetFrameID(), m_auditCurrentList, m_auditCurrentItem,
                    m_frameOpen ? 1 : 0, m_frameRenderer, indexCount, primitiveType);
            return;
        }
        if (!vertices || !indices || !vertices->m_VS[VSF_GENERAL].m_VData ||
            !indices->m_VData || vertices->m_NumVerts <= 0 || firstIndex < 0 ||
            firstIndex > indices->m_nItems ||
            (indexCount > 0 && indexCount > indices->m_nItems - firstIndex))
        {
            m_frameRenderer->RequirePanelFallback();
            return;
        }

        // Leaf buffers can pack lists, strips and fans into one index range.
        // Preserve the stock renderer's group order and topology instead of
        // treating the whole range as one strip (which corrupts connectivity).
        if (primitiveType == R_PRIMV_MULTI_GROUPS)
        {
            if (!materialInfo || !materialInfo->m_pPrimitiveGroups ||
                materialInfo->m_dwNumSections == 0)
            {
                m_frameRenderer->RequirePanelFallback();
                return;
            }

            // Some stock callers describe a grouped mesh entirely through
            // CMatInfo and pass indexCount == 0. In that form group offsets
            // are still relative to firstIndex and bounded by the index
            // buffer's remaining elements.
            const uint32_t groupRangeCount = indexCount > 0
                ? static_cast<uint32_t>(indexCount)
                : static_cast<uint32_t>(indices->m_nItems - firstIndex);
            bool queuedAnyGroup = false;
            for (uint32_t groupIndex = 0; groupIndex < materialInfo->m_dwNumSections; ++groupIndex)
            {
                const SPrimitiveGroup& group = materialInfo->m_pPrimitiveGroups[groupIndex];
                int groupPrimitiveType = -1;
                switch (group.type)
                {
                case PT_LIST:  groupPrimitiveType = R_PRIMV_TRIANGLES; break;
                case PT_STRIP: groupPrimitiveType = R_PRIMV_TRIANGLE_STRIP; break;
                case PT_FAN:   groupPrimitiveType = R_PRIMV_TRIANGLE_FAN; break;
                default:
                    m_frameRenderer->RequirePanelFallback();
                    continue;
                }

                if (group.numIndices == 0 || group.offsIndex > groupRangeCount ||
                    group.numIndices > groupRangeCount - group.offsIndex)
                {
                    m_frameRenderer->RequirePanelFallback();
                    continue;
                }

                DrawBuffer(vertices, indices, static_cast<int>(group.numIndices),
                           firstIndex + static_cast<int>(group.offsIndex), groupPrimitiveType,
                           flags, numVerts, materialInfo);
                queuedAnyGroup = true;
            }
            if (!queuedAnyGroup)
                m_frameRenderer->RequirePanelFallback();
            return;
        }

        // DrawBuffer is called once per render chunk. For
        // R_PRIMV_MULTI_STRIPS, firstIndex/indexCount already identify that
        // chunk's strip (as in GL_VertBuffer's DrawBuffer path); do not walk
        // the render element's entire CMatInfo list here.
        int topology = -1;
        if (primitiveType == R_PRIMV_TRIANGLES) topology = 0;
        else if (primitiveType == R_PRIMV_TRIANGLE_STRIP ||
                 primitiveType == R_PRIMV_MULTI_STRIPS) topology = 1;
        else if (primitiveType == R_PRIMV_TRIANGLE_FAN) topology = 2;
        else if (primitiveType == R_PRIMV_QUADS) topology = 3;
        if (topology < 0)
        {
            m_frameRenderer->RequirePanelFallback();
            return;
        }

        float modelView[16] = {};
        float textureMatrix[16] = {};
        int textureIds[2] = {};
        bool textureStage1UsesTexCoord1 = true;
        CryVR::VulkanStockTextureStage textureStage2{};
        const CryVR::VulkanStockTextureStage* textureStage2Ptr = nullptr;
        CryVR::VulkanStockTextureStage textureStage3{};
        const CryVR::VulkanStockTextureStage* textureStage3Ptr = nullptr;
        CryVR::VulkanStockTextureStage textureStages4To7[4]{};
        const CryVR::VulkanStockTextureStage* textureStages4To7Ptr = nullptr;
        float textureLodBias[2] = {};
        float normalMapLodBias = 0.0f;
        int colorOps[2] = { eCO_MODULATE, eCO_MODULATE };
        int alphaOps[2] = { eCO_MODULATE, eCO_MODULATE };
        uint32_t colorArgs[2] = { DEF_TEXARG0, DEF_TEXARG1 };
        uint32_t alphaArgs[2] = { DEF_TEXARG0, DEF_TEXARG1 };
        float textureMatrices[2][16] = {};
        std::vector<uint8_t> generatedVertexData;
        std::vector<float> lightmapTexCoords;
        bool beamDeformActive = false;
        bool flareDeformActive = false;
        std::vector<struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F> flareVertexData;
        std::vector<uint16_t> flareIndexData;
        int lightmapStage = -1;
        if (m_RP.m_pCurTechnique)
        {
            for (int pointerIndex = 0;
                 pointerIndex < m_RP.m_pCurTechnique->m_Pointers.Num(); ++pointerIndex)
            {
                SArrayPointer* pointer = m_RP.m_pCurTechnique->m_Pointers[pointerIndex];
                if (pointer && pointer->ePT == eSrcPointer_TexLM)
                {
                    lightmapStage = pointer->Stage;
                    break;
                }
            }
        }
        if (lightmapStage < 0 && m_activePass &&
            m_activeHardwarePassType != eSHP_MAX)
        {
            SShaderPassHW* hardwarePass = static_cast<SShaderPassHW*>(m_activePass);
            for (int pointerIndex = 0; pointerIndex < hardwarePass->m_Pointers.Num(); ++pointerIndex)
            {
                SArrayPointer* pointer = hardwarePass->m_Pointers[pointerIndex];
                if (pointer && pointer->ePT == eSrcPointer_TexLM)
                {
                    lightmapStage = pointer->Stage;
                    break;
                }
            }
        }
        if (m_activePass && lightmapStage < 0)
            for (int stage = 0; stage < m_activePass->m_TUnits.Num() && stage < 8; ++stage)
                if (m_activePass->m_TUnits[stage].m_eGenTC == eGTC_LightMap)
                {
                    lightmapStage = stage;
                    break;
                }
        const bool hasLightmapTexCoordSource = lightmapStage >= 0;
        if (hasLightmapTexCoordSource)
        {
            CCObject* object = m_RP.m_pCurObject;
            CLeafBuffer* lmBuffer = object ? object->m_pLMTCBufferO : nullptr;
            if (lightmapStage > 7 || !lmBuffer || !lmBuffer->m_pSecVertBuffer ||
                !lmBuffer->m_pSecVertBuffer->m_VS[VSF_GENERAL].m_VData ||
                lmBuffer->m_pSecVertBuffer->m_vertexformat != VERTEX_FORMAT_TEX2F ||
                lmBuffer->m_SecVertCount < vertices->m_NumVerts)
            {
                m_frameRenderer->RequirePanelFallback();
                return;
            }
            const struct_VERTEX_FORMAT_TEX2F* source =
                static_cast<const struct_VERTEX_FORMAT_TEX2F*>(
                    lmBuffer->m_pSecVertBuffer->m_VS[VSF_GENERAL].m_VData);
            lightmapTexCoords.resize(static_cast<size_t>(vertices->m_NumVerts) * 2);
            for (int vertex = 0; vertex < vertices->m_NumVerts; ++vertex)
            {
                lightmapTexCoords[static_cast<size_t>(vertex) * 2] = source[vertex].st[0];
                lightmapTexCoords[static_cast<size_t>(vertex) * 2 + 1] = source[vertex].st[1];
            }
        }
        const void* drawVertexData = vertices->m_VS[VSF_GENERAL].m_VData;
        CryVR::VulkanVertexFormat stockVertexFormat{};
        const bool stockVertexFormatValid = CryVR::GetVulkanVertexFormat(
            static_cast<uint32_t>(vertices->m_vertexformat), stockVertexFormat);
        uint32_t positionOffset = 0xffffffffu;
        uint32_t normalOffset = 0xffffffffu;
        uint32_t colorOffset = 0xffffffffu;
        uint32_t uv0Offset = 0xffffffffu;
        uint32_t uv1Offset = 0xffffffffu;
        if (stockVertexFormatValid)
        {
            for (uint32_t attribute = 0; attribute < stockVertexFormat.attributeCount; ++attribute)
            {
                const VkVertexInputAttributeDescription& desc = stockVertexFormat.attributes[attribute];
                if (desc.location == 0) positionOffset = desc.offset;
                else if (desc.location == 1) normalOffset = desc.offset;
                else if (desc.location == 2) colorOffset = desc.offset;
                else if (desc.location == 3) uv0Offset = desc.offset;
                else if (desc.location == 5) uv1Offset = desc.offset;
            }
        }
        const SPipTangents* sourceTangentBasis = nullptr;
        if (m_RP.m_pRE && m_RP.m_pRE->mfGetType() == eDATA_OcLeaf)
        {
            CREOcLeaf* leafElement = static_cast<CREOcLeaf*>(m_RP.m_pRE);
            CLeafBuffer* leafBuffer = leafElement->m_pBuffer ?
                leafElement->m_pBuffer->GetVertexContainer() : nullptr;
            if (leafBuffer && leafBuffer->m_pSecVertBuffer &&
                leafBuffer->m_SecVertCount >= vertices->m_NumVerts &&
                leafBuffer->m_pSecVertBuffer->m_VS[VSF_TANGENTS].m_VData)
            {
                sourceTangentBasis = static_cast<const SPipTangents*>(
                    leafBuffer->m_pSecVertBuffer->m_VS[VSF_TANGENTS].m_VData);
            }
        }
        const auto ensureVertexCopy = [&]() -> uint8_t*
        {
            if (!stockVertexFormatValid)
                return nullptr;
            if (generatedVertexData.empty())
            {
                const size_t byteCount = static_cast<size_t>(stockVertexFormat.stride) *
                    static_cast<size_t>(vertices->m_NumVerts);
                generatedVertexData.resize(byteCount);
                memcpy(generatedVertexData.data(), vertices->m_VS[VSF_GENERAL].m_VData, byteCount);
                drawVertexData = generatedVertexData.data();
            }
            return generatedVertexData.data();
        };

        // CGLRenderer::EF_EvalNormalsRB implements eNORM_Custom by replacing
        // the normal stream before applying vertex deforms and texture
        // generation. Apply the same operation to a private copy so later
        // lighting, bump and generated-coordinate paths consume those normals.
        if (m_RP.m_pShader && (m_RP.m_pShader->m_Flags & EF_NEEDNORMALS) &&
            m_RP.m_pShader->m_NormGen &&
            m_RP.m_pShader->m_NormGen->m_eNormal == eNORM_Custom &&
            normalOffset != 0xffffffffu)
        {
            uint8_t* data = ensureVertexCopy();
            if (!data)
                m_frameRenderer->RequirePanelFallback();
            else
            {
                const Vec3d& normal = m_RP.m_pShader->m_NormGen->m_CustomNormal;
                const float customNormal[3] = { normal.x, normal.y, normal.z };
                for (int vertex = 0; vertex < vertices->m_NumVerts; ++vertex)
                    memcpy(data + static_cast<size_t>(vertex) * stockVertexFormat.stride +
                           normalOffset, customNormal, sizeof(customNormal));
            }
        }

        // OpenGL applies SShader::m_Deforms to CPU mesh streams before UV
        // generation. Reproduce its common vertex/normal-based forms on a
        // private upload copy so shared leaf buffers remain untouched.
        if (m_activePass && m_RP.m_pShader && m_RP.m_pShader->m_Deforms &&
            m_RP.m_pShader->m_Deforms->Num() > 0)
        {
            uint8_t* data = ensureVertexCopy();
            if (!data || positionOffset == 0xffffffffu)
                m_frameRenderer->RequirePanelFallback();
            else
            {
                for (int deformIndex = 0; deformIndex < m_RP.m_pShader->m_Deforms->Num(); ++deformIndex)
                {
                    const SDeform& deform = m_RP.m_pShader->m_Deforms->Get(deformIndex);
                    const bool needsNormal = deform.m_eType == eDT_Wave ||
                        deform.m_eType == eDT_Squeeze || deform.m_eType == eDT_Bulge;
                    if (deform.m_eType == eDT_Flare)
                    {
                        // OpenGL changes the active render element to a temp
                        // mesh here. A preceding beam/flare already replaced
                        // the source geometry, so a later flare is a no-op.
                        if (flareDeformActive || beamDeformActive)
                            continue;

                        // Deforms before this stage have already been applied
                        // to the source copy and OpenGL builds the flare from
                        // those positions. Later geometry-changing deforms
                        // operate on the replacement mesh, so they cannot be
                        // applied to the original vertex copy here.
                        bool hasFollowingGeometryDeform = false;
                        for (int followingIndex = deformIndex + 1;
                             followingIndex < m_RP.m_pShader->m_Deforms->Num(); ++followingIndex)
                        {
                            const EDeformType followingType =
                                m_RP.m_pShader->m_Deforms->Get(followingIndex).m_eType;
                            if (followingType != eDT_Unknown && followingType != eDT_Flare)
                            {
                                hasFollowingGeometryDeform = true;
                                break;
                            }
                        }
                        if (hasFollowingGeometryDeform)
                        {
                            m_frameRenderer->RequirePanelFallback();
                            return;
                        }
                        if (!m_flaresEnabledCVar && iConsole)
                            m_flaresEnabledCVar = iConsole->GetCVar("r_ProcFlares");
                        if (m_flaresEnabledCVar && m_flaresEnabledCVar->GetIVal() == 0)
                            return;
                        if (!m_flareSizeCVar && iConsole)
                            m_flareSizeCVar = iConsole->GetCVar("r_FlareSize");
                        CREOcLeaf* leaf = m_RP.m_pRE && m_RP.m_pRE->mfGetType() == eDATA_OcLeaf
                            ? static_cast<CREOcLeaf*>(m_RP.m_pRE) : nullptr;
                        CMatInfo* chunk = leaf ? leaf->m_pChunk : nullptr;
                        const int sourceVertexCount = numVerts;
                        if (!leaf || !chunk || !m_RP.m_pCurObject ||
                            (sourceVertexCount != 4 && sourceVertexCount != 6) ||
                            chunk->nNumIndices != 6 || indexCount != 6 ||
                            firstIndex != chunk->nFirstIndexId ||
                            chunk->nFirstVertId < 0 ||
                            chunk->nFirstVertId > vertices->m_NumVerts - sourceVertexCount ||
                            positionOffset == 0xffffffffu || !stockVertexFormatValid)
                        {
                            m_frameRenderer->RequirePanelFallback();
                            return;
                        }

                        Vec3d sourcePositions[4];
                        for (int vertex = 0; vertex < 4; ++vertex)
                        {
                            const uint8_t* source = data +
                                static_cast<size_t>(chunk->nFirstVertId + vertex) * stockVertexFormat.stride;
                            float position[3];
                            memcpy(position, source + positionOffset, sizeof(position));
                            sourcePositions[vertex].Set(position[0], position[1], position[2]);
                        }
                        uint16_t sourceIndices[6];
                        if (sourceVertexCount == 6)
                        {
                            const uint16_t quadIndices[6] = { 0, 1, 2, 3, 2, 1 };
                            memcpy(sourceIndices, quadIndices, sizeof(sourceIndices));
                        }
                        else
                        {
                            const uint16_t* leafIndices =
                                static_cast<const uint16_t*>(indices->m_VData) + firstIndex;
                            for (int index = 0; index < 6; ++index)
                            {
                                if (leafIndices[index] < chunk->nFirstVertId ||
                                    leafIndices[index] >= chunk->nFirstVertId + sourceVertexCount)
                                {
                                    m_frameRenderer->RequirePanelFallback();
                                    return;
                                }
                                sourceIndices[index] = static_cast<uint16_t>(
                                    leafIndices[index] - chunk->nFirstVertId);
                            }
                        }

                        Vec3d planeNormal = sourcePositions[sourceIndices[2]] -
                            sourcePositions[sourceIndices[1]];
                        const Vec3d planeEdge = sourcePositions[sourceIndices[0]] -
                            sourcePositions[sourceIndices[1]];
                        planeNormal = planeNormal.Cross(planeEdge);
                        if (planeNormal.Normalize() <= 1.0e-6f)
                        {
                            m_frameRenderer->RequirePanelFallback();
                            return;
                        }
                        if (planeNormal.x == 0 && planeNormal.y == 0)
                            planeNormal.z = planeNormal.z <= 0 ? -1.0f : 1.0f;
                        else if (planeNormal.x == 0 && planeNormal.z == 0)
                            planeNormal.y = planeNormal.y <= 0 ? -1.0f : 1.0f;
                        else if (planeNormal.y == 0 && planeNormal.z == 0)
                            planeNormal.x = planeNormal.x <= 0 ? -1.0f : 1.0f;
                        else if (fabsf(planeNormal.x) == 1.0f)
                            planeNormal.y = planeNormal.z = 0;
                        else if (fabsf(planeNormal.y) == 1.0f)
                            planeNormal.x = planeNormal.z = 0;
                        else if (fabsf(planeNormal.z) == 1.0f)
                            planeNormal.x = planeNormal.y = 0;

                        Vec3d objectEye;
                        TransformPosition(objectEye, m_RP.m_ViewOrg,
                                          m_RP.m_pCurObject->GetInvMatrix());
                        const float planeDistance = -planeNormal.Dot(
                            sourcePositions[sourceIndices[1]]);
                        const float eyePlaneDistance = planeNormal.Dot(objectEye) + planeDistance;
                        if (eyePlaneDistance <= 0.0f)
                            return;

                        Vec3d midpoint = sourcePositions[0] + sourcePositions[1] +
                            sourcePositions[2] + sourcePositions[3];
                        midpoint *= 0.25f;
                        Vec3d eyeDirection = objectEye - midpoint;
                        if (eyeDirection.Normalize() <= 1.0e-6f)
                        {
                            m_frameRenderer->RequirePanelFallback();
                            return;
                        }
                        const float intensity = crymin(eyeDirection.Dot(planeNormal) * 8.0f, 1.0f);
                        float red = intensity, green = intensity, blue = intensity;
                        SShader* shader = m_RP.m_pShader;
                        if (shader && shader->m_EvalLights)
                        {
                            SLightEval* eval = shader->m_EvalLights;
                            if (eval->m_LightStyle > 0 &&
                                eval->m_LightStyle < CLightStyle::m_LStyles.Num() &&
                                CLightStyle::m_LStyles[eval->m_LightStyle])
                            {
                                CLightStyle* style = CLightStyle::m_LStyles[eval->m_LightStyle];
                                style->mfUpdate(iTimer ? iTimer->GetCurrTime() : m_RP.m_RealTime);
                                if (eval->m_EStyleType == eLS_RGB)
                                {
                                    red = style->m_Color.r * intensity;
                                    green = style->m_Color.g * intensity;
                                    blue = style->m_Color.b * intensity;
                                }
                                else
                                    red = green = blue = intensity * style->m_fIntensity;
                            }
                        }
                        if (m_activeResources && m_activeResources->m_LMaterial)
                        {
                            const SSideMaterial& ambient = m_activeResources->m_LMaterial->Front;
                            red *= ambient.m_Ambient.r;
                            green *= ambient.m_Ambient.g;
                            blue *= ambient.m_Ambient.b;
                        }
                        const uint8_t r = static_cast<uint8_t>(clamp_tpl(red, 0.0f, 1.0f) * 255.0f);
                        const uint8_t g = static_cast<uint8_t>(clamp_tpl(green, 0.0f, 1.0f) * 255.0f);
                        const uint8_t b = static_cast<uint8_t>(clamp_tpl(blue, 0.0f, 1.0f) * 255.0f);

                        uint16_t polygon[16]{};
                        if (VulkanTriangleIndicesToPolygon(sourceIndices, 6, 4, polygon) != 4)
                        {
                            m_frameRenderer->RequirePanelFallback();
                            return;
                        }
                        Vec3d flareVectors[4][3];
                        flareVertexData.resize(16);
                        for (int vertex = 0; vertex < 16; ++vertex)
                        {
                            struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F& output = flareVertexData[vertex];
                            if (gbRgb)
                            {
                                output.color.bcolor[0] = r;
                                output.color.bcolor[1] = g;
                                output.color.bcolor[2] = b;
                            }
                            else
                            {
                                output.color.bcolor[0] = b;
                                output.color.bcolor[1] = g;
                                output.color.bcolor[2] = r;
                            }
                            output.color.bcolor[3] = 255;
                        }
                        for (int corner = 0; corner < 4; ++corner)
                        {
                            struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F& output = flareVertexData[corner];
                            output.xyz = sourcePositions[polygon[corner]];
                            output.st[0] = output.st[1] = 0.5f;
                            Vec3d ray = sourcePositions[polygon[corner]] - objectEye;
                            if (ray.Normalize() <= 1.0e-6f)
                            {
                                m_frameRenderer->RequirePanelFallback();
                                return;
                            }
                            Vec3d nextRay = sourcePositions[polygon[(corner + 1) % 4]] - objectEye;
                            Vec3d previousRay = sourcePositions[polygon[(corner + 3) % 4]] - objectEye;
                            if (nextRay.Normalize() <= 1.0e-6f || previousRay.Normalize() <= 1.0e-6f)
                            {
                                m_frameRenderer->RequirePanelFallback();
                                return;
                            }
                            flareVectors[corner][1] = nextRay.Cross(ray);
                            flareVectors[corner][1].Normalize();
                            flareVectors[corner][1].Flip();
                            flareVectors[corner][0] = previousRay.Cross(ray);
                            flareVectors[corner][0].Normalize();
                            flareVectors[corner][2] = flareVectors[corner][0] + flareVectors[corner][1];
                            flareVectors[corner][2].Normalize();
                        }
                        const float flareSize = deform.m_fFlareSize *
                            (m_flareSizeCVar ? m_flareSizeCVar->GetFVal() : 1.0f);
                        for (int corner = 0; corner < 4; ++corner)
                        {
                            const int firstOuter = 4 + corner * 3;
                            flareVertexData[firstOuter + 0].xyz = sourcePositions[polygon[corner]] +
                                flareVectors[corner][0] * flareSize;
                            flareVertexData[firstOuter + 1].xyz = sourcePositions[polygon[corner]] +
                                flareVectors[corner][2] * flareSize;
                            flareVertexData[firstOuter + 2].xyz = sourcePositions[polygon[corner]] +
                                flareVectors[corner][1] * flareSize;
                        }
                        for (int vertex = 4; vertex < 16; ++vertex)
                        {
                            Vec3d ray = flareVertexData[vertex].xyz - objectEye;
                            const float rayLength = ray.Length();
                            if (rayLength > 1.0e-6f)
                            {
                                ray *= 1.0f / rayLength;
                                const float denominator = ray.Dot(planeNormal);
                                if (fabsf(denominator) > 1.0e-6f)
                                {
                                    const float distance = -(eyePlaneDistance / denominator);
                                    if (distance > 0.0f && rayLength > distance)
                                        flareVertexData[vertex].xyz = objectEye + ray * distance;
                                }
                            }
                            flareVertexData[vertex].st[0] = 0.0f;
                            flareVertexData[vertex].st[1] = 0.5f;
                        }
                        flareIndexData.assign(g_vulkanFlareIndices,
                                              g_vulkanFlareIndices + 54);
                        flareDeformActive = true;
                        break;
                    }
                    if (deform.m_eType == eDT_Beam)
                    {
                        // Port SEvalFuncs_RE::BeamDeform onto the private Vulkan
                        // upload copy. OpenGL transforms the beam along X, scales
                        // its cross-section by the interpolated radius, colors it
                        // between start/end parameters, and fades by view angle.
                        if (!m_beamsCVar && iConsole)
                            m_beamsCVar = iConsole->GetCVar("r_Beams");
                        const bool beamsEnabled = m_beamsCVar ?
                            m_beamsCVar->GetIVal() != 0 : true;
                        if (!beamsEnabled || !m_RP.m_pCurObject ||
                            !m_RP.m_pRE || m_RP.m_pRE->mfGetType() != eDATA_OcLeaf)
                            return;
                        beamDeformActive = true;
                        if (positionOffset == 0xffffffffu || normalOffset == 0xffffffffu ||
                            colorOffset == 0xffffffffu || uv0Offset == 0xffffffffu)
                        {
                            m_frameRenderer->RequirePanelFallback();
                            continue;
                        }
                        SParamComp_User userParameter;
                        const auto readBeamParameter = [&](const char* name)
                        {
                            userParameter.m_Name = name;
                            return userParameter.mfGet();
                        };
                        float startColor[4], endColor[4];
                        static const char* const startColorNames[4] = {
                            "startcolor[0]", "startcolor[1]", "startcolor[2]", "startcolor[3]"
                        };
                        static const char* const endColorNames[4] = {
                            "endcolor[0]", "endcolor[1]", "endcolor[2]", "endcolor[3]"
                        };
                        for (int component = 0; component < 4; ++component)
                        {
                            startColor[component] = readBeamParameter(startColorNames[component]);
                            endColor[component] = readBeamParameter(endColorNames[component]);
                        }
                        const float originalLength = readBeamParameter("origlength");
                        const float originalWidth = readBeamParameter("origwidth");
                        const float startRadius = readBeamParameter("startradius");
                        const float endRadius = readBeamParameter("endradius");
                        const float length = readBeamParameter("length");
                        if (fabsf(originalLength) < 1.0e-6f || fabsf(originalWidth) < 1.0e-6f)
                        {
                            m_frameRenderer->RequirePanelFallback();
                            continue;
                        }
                        Matrix44& inverseObject = m_RP.m_pCurObject->GetInvMatrix();
                        Vec3d cameraPosition;
                        TransformPosition(cameraPosition, m_RP.m_ViewOrg, inverseObject);
                        for (int vertex = 0; vertex < vertices->m_NumVerts; ++vertex)
                        {
                            uint8_t* source = data + static_cast<size_t>(vertex) * stockVertexFormat.stride;
                            float position[3], normal[3], sourcePosition[3];
                            memcpy(position, source + positionOffset, sizeof(position));
                            memcpy(sourcePosition, position, sizeof(sourcePosition));
                            memcpy(normal, source + normalOffset, sizeof(normal));
                            const float fraction = position[0] / originalLength;
                            const float radius = startRadius + (endRadius - startRadius) * fraction;
                            position[0] = fraction * length;
                            position[1] = position[1] / originalWidth * radius;
                            position[2] = position[2] / originalWidth * radius;
                            float color[4];
                            for (int component = 0; component < 4; ++component)
                                color[component] = clamp_tpl(startColor[component] +
                                    (endColor[component] - startColor[component]) * fraction,
                                    0.0f, 1.0f);
                            float cameraVector[3] = {
                                cameraPosition.x - sourcePosition[0],
                                cameraPosition.y - sourcePosition[1],
                                cameraPosition.z - sourcePosition[2]
                            };
                            const float viewLength = sqrtf(cameraVector[0] * cameraVector[0] +
                                cameraVector[1] * cameraVector[1] + cameraVector[2] * cameraVector[2]);
                            float facing = 0.0f;
                            if (viewLength > 1.0e-6f)
                                facing = (cameraVector[0] * normal[0] + cameraVector[1] * normal[1] +
                                          cameraVector[2] * normal[2]) / viewLength;
                            const float fade = facing * facing *
                                crymin(fraction * 10.0f, 1.0f);
                            color[3] *= fade;
                            memcpy(source + positionOffset, position, sizeof(position));
                            const uint8_t rgba[4] = {
                                static_cast<uint8_t>(color[0] * 255.0f),
                                static_cast<uint8_t>(color[1] * 255.0f),
                                static_cast<uint8_t>(color[2] * 255.0f),
                                static_cast<uint8_t>(clamp_tpl(color[3], 0.0f, 1.0f) * 255.0f)
                            };
                            if (gbRgb)
                                memcpy(source + colorOffset, rgba, sizeof(rgba));
                            else
                            {
                                const uint8_t bgra[4] = { rgba[2], rgba[1], rgba[0], rgba[3] };
                                memcpy(source + colorOffset, bgra, sizeof(bgra));
                            }
                        }
                        continue;
                    }
                    if (needsNormal && normalOffset == 0xffffffffu && !sourceTangentBasis)
                    {
                        m_frameRenderer->RequirePanelFallback();
                        continue;
                    }
                    if (deform.m_eType == eDT_Unknown)
                        // EF_Eval_DeformVerts logs an unknown type and leaves
                        // the current vertex stream unchanged.
                        continue;
                    for (int vertex = 0; vertex < vertices->m_NumVerts; ++vertex)
                    {
                        uint8_t* source = data + static_cast<size_t>(vertex) * stockVertexFormat.stride;
                        float position[3] = {};
                        float normal[3] = { 0.0f, 0.0f, 1.0f };
                        memcpy(position, source + positionOffset, sizeof(position));
                        if (needsNormal)
                        {
                            if (sourceTangentBasis)
                            {
                                const Vec3& sourceTNormal = sourceTangentBasis[vertex].m_TNormal;
                                normal[0] = sourceTNormal.x;
                                normal[1] = sourceTNormal.y;
                                normal[2] = sourceTNormal.z;
                            }
                            else
                                memcpy(normal, source + normalOffset, sizeof(normal));
                        }
                        float amount = 0.0f;
                        switch (deform.m_eType)
                        {
                        case eDT_Wave:
                        case eDT_VerticalWave:
                        case eDT_Bulge:
                        {
                            float phase = position[0] + position[1] + position[2];
                            if (deform.m_eType == eDT_Bulge)
                            {
                                if (uv0Offset == 0xffffffffu)
                                {
                                    m_frameRenderer->RequirePanelFallback();
                                    continue;
                                }
                                float baseUv[2] = {};
                                memcpy(baseUv, source + uv0Offset, sizeof(baseUv));
                                phase += baseUv[0] + baseUv[1];
                            }
                            phase = phase * deform.m_ScaleVerts + deform.m_DeformGen.m_Phase +
                                m_RP.m_RealTime * deform.m_DeformGen.m_Freq;
                            const int tableIndex = QRound(phase * 1024.0f) & 0x3ff;
                            float* waveTable = m_RP.m_tSinTable;
                            if (deform.m_eType != eDT_Bulge)
                            {
                                switch (deform.m_DeformGen.m_eWFType)
                                {
                                case eWF_Triangle: waveTable = m_RP.m_tTriTable; break;
                                case eWF_Square: waveTable = m_RP.m_tSquareTable; break;
                                case eWF_SawTooth: waveTable = m_RP.m_tSawtoothTable; break;
                                case eWF_InvSawTooth: waveTable = m_RP.m_tInvSawtoothTable; break;
                                case eWF_Hill: waveTable = m_RP.m_tHillTable; break;
                                default: break;
                                }
                            }
                            amount = deform.m_DeformGen.m_Amp * CRenderer::CV_r_wavescale *
                                waveTable[tableIndex] + deform.m_DeformGen.m_Level;
                            if (deform.m_eType == eDT_VerticalWave)
                                normal[0] = normal[1] = 0.0f;
                            break;
                        }
                        case eDT_Squeeze:
                            amount = SEvalFuncs::EvalWaveForm(
                                const_cast<SWaveForm*>(&deform.m_DeformGen));
                            break;
                        case eDT_FromCenter:
                            if (!m_RP.m_pCurObject)
                                continue;
                            amount = SEvalFuncs::EvalWaveForm2(
                                const_cast<SWaveForm*>(&deform.m_DeformGen),
                                m_RP.m_RealTime - m_RP.m_pCurObject->m_StartTime);
                            normal[0] = position[0] - m_RP.m_Center.x;
                            normal[1] = position[1] - m_RP.m_Center.y;
                            normal[2] = position[2] - m_RP.m_Center.z;
                            {
                                const float length = sqrtf(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
                                if (length > 1.0e-6f)
                                {
                                    normal[0] /= length; normal[1] /= length; normal[2] /= length;
                                }
                            }
                            break;
                        default:
                            // OpenGL's deform dispatcher ignores unrecognized
                            // types after logging instead of rejecting the draw.
                            continue;
                        }
                        position[0] += amount * normal[0];
                        position[1] += amount * normal[1];
                        position[2] += amount * normal[2];
                        memcpy(source + positionOffset, position, sizeof(position));
                    }
                }
            }
        }
        if (flareDeformActive && hasLightmapTexCoordSource)
        {
            if (m_auditFrameActive)
                CryLogAlways("Vulkan audit DRAW_REJECT frame=%d list=%d item=%d reason=invalid-buffer vb=%p ib=%p vertexData=%p indexData=%p vbVerts=%d ibItems=%d first=%d count=%d",
                    GetFrameID(), m_auditCurrentList, m_auditCurrentItem,
                    vertices, indices,
                    vertices ? vertices->m_VS[VSF_GENERAL].m_VData : nullptr,
                    indices ? indices->m_VData : nullptr,
                    vertices ? vertices->m_NumVerts : -1,
                    indices ? indices->m_nItems : -1, firstIndex, indexCount);
            m_frameRenderer->RequirePanelFallback();
            return;
        }
        for (int stage = 0; stage < 2; ++stage)
            textureMatrices[stage][0] = textureMatrices[stage][5] =
                textureMatrices[stage][10] = textureMatrices[stage][15] = 1.0f;

        // OpenGL starts from the texture matrix stack's identity and only
        // loads the material matrix when Update() reports a real transform.
        // Default resources can leave m_TexMatrix zero-initialized; copying it
        // unconditionally collapses every UV to (0, 0) in Vulkan.
        const auto updateStockTextureMatrix = [&](SEfResTexture* resource,
                                                   int stage, float* matrix)
        {
            if (!resource) return;
            resource->Update(stage);
            if (resource->m_TexModificator.m_UpdateFlags & RBMF_TCM)
                memcpy(matrix, resource->m_TexModificator.m_TexMatrix.GetData(),
                       sizeof(float) * 16);
        };

        const auto applyStockTextureMatrixOps = [&](int stage, float* values)
        {
            Matrix44 matrix;
            memcpy(matrix.GetData(), values, sizeof(float) * 16);
            const auto applyOps = [&](TArray<SMatrixTransform>* operations)
            {
                if (!operations) return;
                for (int index = 0; index < operations->Num(); ++index)
                {
                    SMatrixTransform& operation = operations->Get(index);
                    if (operation.m_Stage != stage) continue;
                    if (operation.m_Matrix != 0x1702)
                    {
                        // LightCMProject is implemented by the projected
                        // cookie sampler in the scene shaders. Its direction,
                        // orientation and frustum scale are carried separately
                        // per draw, so it must not be treated as an unknown GL
                        // texture-matrix register here.
                        if (operation.m_Matrix == 0x8638 && m_RP.m_pCurLight &&
                            (m_RP.m_pCurLight->m_Flags & DLF_PROJECT) &&
                            m_RP.m_pCurLight->m_pLightImage)
                            continue;
                        m_frameRenderer->RequirePanelFallback();
                        continue;
                    }
                    operation.mfSet(matrix);
                }
            };
            if (m_RP.m_pCurTechnique)
                applyOps(m_RP.m_pCurTechnique->m_MatrixOps);
            if (m_activeHardwarePassType != eSHP_MAX && m_activePass)
                applyOps(static_cast<SShaderPassHW*>(m_activePass)->m_MatrixOps);
            memcpy(values, matrix.GetData(), sizeof(float) * 16);
        };
        const auto setStockUvTransform = [](CryVR::VulkanStockTextureStage& stage,
                                             const float* matrix)
        {
            stage.uvTransform[0] = matrix[0];
            stage.uvTransform[1] = matrix[4];
            stage.uvTransform[2] = matrix[12];
            stage.uvTransform[3] = matrix[1];
            stage.uvTransform[4] = matrix[5];
            stage.uvTransform[5] = matrix[13];
            stage.uvTransform[6] = matrix[3];
            stage.uvTransform[7] = matrix[7];
            stage.uvTransform[8] = matrix[15];
        };

        const auto generateLegacyTexCoords = [&](int stage, const SShaderTexUnit* unit)
        {
            if (flareDeformActive)
                return;
            if (!unit || !unit->m_TexPic ||
                unit->m_eGenTC == eGTC_NoFill || unit->m_eGenTC == eGTC_None ||
                unit->m_eGenTC == eGTC_Base || unit->m_eGenTC == eGTC_LightMap)
                return;
            // OpenGL's eGTC_Quad CPU generator only fills coordinates when
            // there is no current render element. Leave stock leaf and other
            // render-element UVs untouched, as EF_Eval_TexGen does.
            if (unit->m_eGenTC == eGTC_Quad && m_RP.m_pRE)
                return;
            SGenTC_ObjectLinear* objectLinear = nullptr;
            SGenTC_EyeLinear* eyeLinear = nullptr;
            SGenTC_NormalMap* normalMap = nullptr;
            SGenTC_ReflectionMap* reflectionMap = nullptr;
            SGenTC_SphereMap* sphereMap = nullptr;
            TArray<SParam>* linearParameters = nullptr;
            int linearMask = 0;
            float linearPlanes[4][4] = {};
            if (unit->m_GTC)
            {
                if (unit->m_GTC->GetType() == eGTCType_ObjectLinear)
                {
                    objectLinear = static_cast<SGenTC_ObjectLinear*>(unit->m_GTC);
                    linearParameters = &objectLinear->m_Params;
                }
                else if (unit->m_GTC->GetType() == eGTCType_EyeLinear)
                {
                    eyeLinear = static_cast<SGenTC_EyeLinear*>(unit->m_GTC);
                    linearParameters = &eyeLinear->m_Params;
                }
                else if (unit->m_GTC->GetType() == eGTCType_NormalMap)
                    normalMap = static_cast<SGenTC_NormalMap*>(unit->m_GTC);
                else if (unit->m_GTC->GetType() == eGTCType_ReflectionMap)
                    reflectionMap = static_cast<SGenTC_ReflectionMap*>(unit->m_GTC);
                else if (unit->m_GTC->GetType() == eGTCType_SphereMap)
                    sphereMap = static_cast<SGenTC_SphereMap*>(unit->m_GTC);
                else
                {
                    m_frameRenderer->RequirePanelFallback();
                    return;
                }

                if (objectLinear || eyeLinear)
                {
                    const int requestedMask = unit->m_GTC->m_Mask;
                    if (requestedMask & ~0xf)
                    {
                        m_frameRenderer->RequirePanelFallback();
                        return;
                    }
                    for (int coordinate = 0; coordinate < 4; ++coordinate)
                    {
                        if (!(requestedMask & (1 << coordinate)))
                            continue;
                        // OpenGL only enables a coordinate generator when a
                        // corresponding valid plane exists in m_Params.
                        if (!linearParameters || coordinate >= linearParameters->Num() ||
                            !(*linearParameters)[coordinate].mfIsValid())
                            continue;
                        linearMask |= 1 << coordinate;
                        // R is not consumed by a 2D sampler. S, T and Q are
                        // evaluated below; Q is folded into S/T by the
                        // homogeneous divide before the texture matrix.
                        if (coordinate == 2)
                            continue;
                        float* plane = (*linearParameters)[coordinate].mfGet();
                        memcpy(linearPlanes[coordinate], plane, sizeof(linearPlanes[coordinate]));
                    }
                }
            }

            const bool needsNormal = unit->m_eGenTC == eGTC_Environment ||
                unit->m_eGenTC == eGTC_SphereMap ||
                unit->m_eGenTC == eGTC_SphereMapEnvironment || normalMap ||
                reflectionMap || sphereMap;
            const bool usesTangentNormal =
                unit->m_eGenTC == eGTC_Environment ||
                unit->m_eGenTC == eGTC_SphereMap ||
                unit->m_eGenTC == eGTC_SphereMapEnvironment;
            const bool hasRequiredNormal = usesTangentNormal ?
                (sourceTangentBasis || HasStockVertexNormal(vertices->m_vertexformat)) :
                HasStockVertexNormal(vertices->m_vertexformat);
            if (!m_RP.m_pCurObject || (needsNormal && !hasRequiredNormal))
            {
                m_frameRenderer->RequirePanelFallback();
                return;
            }

            if (!stockVertexFormatValid)
            {
                m_frameRenderer->RequirePanelFallback();
                return;
            }
            uint32_t uvOffset = 0xffffffffu;
            if (stage == 0) uvOffset = uv0Offset;
            else uvOffset = uv1Offset;
            if (uvOffset == 0xffffffffu || positionOffset == 0xffffffffu ||
                (needsNormal && normalOffset == 0xffffffffu &&
                 !(usesTangentNormal && sourceTangentBasis)))
            {
                m_frameRenderer->RequirePanelFallback();
                return;
            }

            if (generatedVertexData.empty())
            {
                if (!ensureVertexCopy())
                {
                    m_frameRenderer->RequirePanelFallback();
                    return;
                }
            }
            uint8_t* data = generatedVertexData.data();
            const Vec3d objectTranslation = m_RP.m_pCurObject->GetTranslation();
            const float* projection = unit->m_TexPic->m_Matrix;
            Matrix44 normalTransform;
            if (normalMap || reflectionMap || sphereMap)
            {
                mathMatrixInverse(normalTransform.GetData(), m_ViewMatrix.GetData(), g_CpuFlags);
                normalTransform.Transpose();
            }
            for (int vertex = 0; vertex < vertices->m_NumVerts; ++vertex)
            {
                const uint8_t* source = data + static_cast<size_t>(vertex) * stockVertexFormat.stride;
                const uint8_t* originalVertex =
                    static_cast<const uint8_t*>(vertices->m_VS[VSF_GENERAL].m_VData) +
                    static_cast<size_t>(vertex) * stockVertexFormat.stride;
                float originalPosition[3] = {};
                memcpy(originalPosition, originalVertex + positionOffset, sizeof(originalPosition));
                float position[3] = {};
                memcpy(position, source + positionOffset, sizeof(position));
                float uv[2] = {};
                if (objectLinear || eyeLinear)
                {
                    memcpy(uv, source + uvOffset, sizeof(uv));
                    Vec3d generatedPosition(position[0], position[1], position[2]);
                    if (eyeLinear)
                        generatedPosition = m_ViewMatrix.TransformPointOLD(generatedPosition);
                    const float position4[4] = {
                        generatedPosition.x, generatedPosition.y, generatedPosition.z, 1.0f
                    };
                    for (int coordinate = 0; coordinate < 2; ++coordinate)
                    {
                        if (!(linearMask & (1 << coordinate)))
                            continue;
                        uv[coordinate] = linearPlanes[coordinate][0] * position4[0] +
                            linearPlanes[coordinate][1] * position4[1] +
                            linearPlanes[coordinate][2] * position4[2] +
                            linearPlanes[coordinate][3] * position4[3];
                    }
                    if (linearMask & (1 << 3))
                    {
                        const float q = linearPlanes[3][0] * position4[0] +
                            linearPlanes[3][1] * position4[1] +
                            linearPlanes[3][2] * position4[2] +
                            linearPlanes[3][3] * position4[3];
                        if (fabsf(q) > 1.0e-6f)
                        {
                            uv[0] /= q;
                            uv[1] /= q;
                        }
                        else
                        {
                            m_frameRenderer->RequirePanelFallback();
                            return;
                        }
                    }
                }
                else if (normalMap || reflectionMap || sphereMap)
                {
                    float normal[3] = {};
                    memcpy(normal, source + normalOffset, sizeof(normal));
                    Vec3d eyeNormal = normalTransform.TransformVectorOLD(
                        Vec3d(normal[0], normal[1], normal[2]));
                    eyeNormal.Normalize();
                    if (normalMap)
                    {
                        uv[0] = eyeNormal.x;
                        uv[1] = eyeNormal.y;
                    }
                    else
                    {
                        const Vec3d eyePosition = m_ViewMatrix.TransformPointOLD(
                            Vec3d(position[0], position[1], position[2]));
                        Vec3d eyeDirection = eyePosition;
                        eyeDirection.Normalize();
                        const float normalDotEye = eyeNormal | eyeDirection;
                        const Vec3d reflection = eyeDirection -
                            eyeNormal * (2.0f * normalDotEye);
                        if (reflectionMap)
                        {
                            uv[0] = reflection.x;
                            uv[1] = reflection.y;
                        }
                        else
                        {
                            const float denominator = 2.0f * sqrtf(
                                reflection.x * reflection.x + reflection.y * reflection.y +
                                (reflection.z + 1.0f) * (reflection.z + 1.0f));
                            if (denominator > 1.0e-6f)
                            {
                                uv[0] = reflection.x / denominator + 0.5f;
                                uv[1] = reflection.y / denominator + 0.5f;
                            }
                            else
                            {
                                uv[0] = 0.5f;
                                uv[1] = 0.5f;
                            }
                        }
                    }
                }
                else if (needsNormal)
                {
                    float normal[3] = {};
                    if (usesTangentNormal && sourceTangentBasis)
                    {
                        const Vec3& sourceTNormal = sourceTangentBasis[vertex].m_TNormal;
                        normal[0] = sourceTNormal.x;
                        normal[1] = sourceTNormal.y;
                        normal[2] = sourceTNormal.z;
                    }
                    else
                        memcpy(normal, source + normalOffset, sizeof(normal));
                    if (unit->m_eGenTC == eGTC_Environment)
                    {
                        // Match SEvalFuncs_RE::ETC_Environment exactly: its
                        // FGP_SRC reads the original vertex stream even when
                        // EF_Eval_DeformVerts has already modified the draw
                        // stream. Use that same undeformed position here.
                        float view[3] = { objectTranslation.x - originalPosition[0],
                                           objectTranslation.y - originalPosition[1],
                                           objectTranslation.z - originalPosition[2] };
                        const float length = sqrtf(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
                        if (length > 1.0e-6f)
                        {
                            view[0] /= length; view[1] /= length; view[2] /= length;
                        }
                        const float dot = view[0] * normal[0] + view[1] * normal[1] + view[2] * normal[2];
                        uv[0] = ((2.0f * dot * normal[1] - view[1]) + 1.0f) * 0.5f;
                        uv[1] = 0.5f - ((2.0f * dot * normal[2] - view[2]) * 0.5f);
                    }
                    else
                    {
                        float r00, r01, r10, r11, r20, r21;
                        if (unit->m_eGenTC == eGTC_SphereMap)
                        {
                            r00 = m_ViewMatrix(0, 0); r01 = m_ViewMatrix(0, 1);
                            r10 = m_ViewMatrix(1, 0); r11 = m_ViewMatrix(1, 1);
                            r20 = m_ViewMatrix(2, 0); r21 = m_ViewMatrix(2, 1);
                        }
                        else
                        {
                            r00 = m_RP.m_pCurObject->m_Matrix(0, 0); r01 = m_RP.m_pCurObject->m_Matrix(0, 1);
                            r10 = m_RP.m_pCurObject->m_Matrix(1, 0); r11 = m_RP.m_pCurObject->m_Matrix(1, 1);
                            r20 = m_RP.m_pCurObject->m_Matrix(2, 0); r21 = m_RP.m_pCurObject->m_Matrix(2, 1);
                        }
                        uv[0] = 0.5f * (1.0f + normal[0] * r00 + normal[1] * r10 + normal[2] * r20);
                        uv[1] = 0.5f * (1.0f - normal[0] * r01 - normal[1] * r11 - normal[2] * r21);
                    }
                }
                else if (unit->m_eGenTC == eGTC_Quad)
                {
                    static const float quadUv[4][2] = {
                        { 0.0f, 0.0f }, { 1.0f, 0.0f },
                        { 1.0f, 1.0f }, { 0.0f, 1.0f }
                    };
                    uv[0] = quadUv[vertex & 3][0];
                    uv[1] = quadUv[vertex & 3][1];
                }
                else if (unit->m_eGenTC == eGTC_Projection && projection)
                {
                    // ETC_Projection also requests FGP_SRC, so project the
                    // undeformed input position, not the modified draw copy.
                    const float tx = originalPosition[0] * projection[0] + originalPosition[1] * projection[4] +
                                     originalPosition[2] * projection[8] + projection[12];
                    const float ty = originalPosition[0] * projection[1] + originalPosition[1] * projection[5] +
                                     originalPosition[2] * projection[9] + projection[13];
                    const float tw = originalPosition[0] * projection[3] + originalPosition[1] * projection[7] +
                                     originalPosition[2] * projection[11] + projection[15];
                    uv[0] = tx / tw;
                    uv[1] = ty / tw;
                }
                else
                {
                    // Shadow-map generation requires OpenGL's shadow-frustum
                    // setup and render-to-texture pass, neither of which is
                    // available in this Vulkan frame path yet.
                    m_frameRenderer->RequirePanelFallback();
                    return;
                }
                memcpy(data + static_cast<size_t>(vertex) * stockVertexFormat.stride + uvOffset,
                       uv, sizeof(uv));
            }
            drawVertexData = data;
        };
        if (!m_activePass)
        {
            for (int stage = 0; stage < 2; ++stage)
            {
                textureIds[stage] = m_stageTextureIds[stage];
                colorOps[stage] = m_stageColorOps[stage];
                alphaOps[stage] = m_stageAlphaOps[stage];
                colorArgs[stage] = m_stageColorArgs[stage];
                alphaArgs[stage] = m_stageAlphaArgs[stage];
            }
        }
        int normalMapTextureId = 0;
        float materialLighting[11] = { -0.35f, 0.72f, 0.60f, 0.0f,
                                       0.78f, 0.78f, 0.78f, 0.22f,
                                       1.0f, 1.0f, 1.0f };
        if (m_activeResources && m_activeResources->m_LMaterial)
        {
            const SSideMaterial& material = m_activeResources->m_LMaterial->Front;
            materialLighting[8] = material.m_Ambient.r;
            materialLighting[9] = material.m_Ambient.g;
            materialLighting[10] = material.m_Ambient.b;
            materialLighting[4] = 0.78f * material.m_Diffuse.r;
            materialLighting[5] = 0.78f * material.m_Diffuse.g;
            materialLighting[6] = 0.78f * material.m_Diffuse.b;
        }
        bool techniqueHasDedicatedLightPass = false;
        if (m_RP.m_pCurTechnique)
        {
            for (int passIndex = 0; passIndex < m_RP.m_pCurTechnique->m_Passes.Num(); ++passIndex)
            {
                const EShaderPassType type = m_RP.m_pCurTechnique->m_Passes[passIndex].m_ePassType;
                if (type == eSHP_Light || type == eSHP_DiffuseLight ||
                    type == eSHP_MultiLights)
                {
                    techniqueHasDedicatedLightPass = true;
                    break;
                }
            }
        }
        const bool hardwareAmbientPass =
            m_activeHardwarePassType == eSHP_General && techniqueHasDedicatedLightPass;
        const bool hardwareLightPass =
            m_activeHardwarePassType == eSHP_Light ||
            m_activeHardwarePassType == eSHP_DiffuseLight ||
            m_activeHardwarePassType == eSHP_SpecularLight ||
            m_activeHardwarePassType == eSHP_MultiLights;
        const bool hardwareSpecularPass = m_activeHardwarePassType == eSHP_SpecularLight;
        const uint32_t hardwareLightFlags =
            m_activeHardwarePassType != eSHP_MAX && m_activePass ?
                static_cast<SShaderPassHW*>(m_activePass)->m_LMFlags : 0;
        if (m_RP.m_pCurObject)
        {
            const CFColor objectAmbient = m_RP.m_pCurObject->m_AmbColor;
            const bool ignoreMaterialAmbient =
                (m_RP.m_ObjFlags & FOB_IGNOREMATERIALAMBIENT) != 0;
            if (hardwareLightFlags & LMF_NOAMBIENT)
                materialLighting[8] = materialLighting[9] = materialLighting[10] = 0.0f;
            else if (hardwareLightFlags & LMF_ONLYMATERIALAMBIENT)
            {
                if (!ignoreMaterialAmbient)
                    for (int channel = 8; channel < 11; ++channel)
                        materialLighting[channel] *= materialLighting[channel];
            }
            else if (!ignoreMaterialAmbient)
            {
                materialLighting[8] *= objectAmbient.r;
                materialLighting[9] *= objectAmbient.g;
                materialLighting[10] *= objectAmbient.b;
            }
            else
            {
                materialLighting[8] = objectAmbient.r;
                materialLighting[9] = objectAmbient.g;
                materialLighting[10] = objectAmbient.b;
            }
        }
        const bool hardwareMultiLightsAmbient =
            m_activeHardwarePassType == eSHP_MultiLights &&
            (hardwareLightFlags & LMF_HASAMBIENT) &&
            !(hardwareLightFlags & LMF_DISABLE) &&
            !(hardwareLightFlags & LMF_NOAMBIENT) &&
            !(m_RP.m_ObjFlags & FOB_LIGHTPASS) &&
            (!(hardwareLightFlags & LMF_HASDOT3LM) ||
             (m_activeResources && m_RP.m_pCurObject && m_RP.m_pCurObject->m_nLMDirId));
        float ambientScale = 1.0f;
        if (hardwareLightFlags & LMF_DIVIDEAMB4)
            ambientScale = 0.25f;
        if (hardwareLightFlags & LMF_DIVIDEAMB2)
            ambientScale = 0.5f;
        for (int channel = 8; channel < 11; ++channel)
            materialLighting[channel] = crymin(materialLighting[channel] * ambientScale, 1.0f);
        float diffuseScale = 1.0f;
        if (hardwareLightFlags & LMF_DIVIDEDIFF4)
            diffuseScale = 0.25f;
        if (hardwareLightFlags & LMF_DIVIDEDIFF2)
            diffuseScale = 0.5f;
        for (int channel = 4; channel < 7; ++channel)
            materialLighting[channel] *= diffuseScale;
        if (hardwareAmbientPass)
        {
            // OpenGL's programmable technique adds dynamic lights in its
            // dedicated light passes, not again in its general/base pass.
            materialLighting[4] = materialLighting[5] = materialLighting[6] = 0.0f;
            if (hardwareLightFlags & LMF_NOAMBIENT)
                materialLighting[8] = materialLighting[9] = materialLighting[10] = 0.0f;
        }
        else if (hardwareLightPass)
        {
            // Translated per-light passes must not repeat ambient; specular
            // replaces the diffuse tint and uses a negative exponent marker
            // in the existing eight-float lighting payload.
            if (!hardwareMultiLightsAmbient)
                materialLighting[8] = materialLighting[9] = materialLighting[10] = 0.0f;
            if (hardwareSpecularPass &&
                (hardwareLightFlags & (LMF_NOSPECULAR | LMF_NOADDSPECULAR)))
                materialLighting[4] = materialLighting[5] = materialLighting[6] = 0.0f;
        }

        std::vector<std::array<float, 11>> lightPasses;
        if (!hardwareAmbientPass && HasStockVertexNormal(vertices->m_vertexformat) && m_RP.m_DynLMask &&
            m_RP.m_pCurObject)
        {
            const int lightLevel = SRendItem::m_RecurseLevel;
            if (lightLevel >= 0 && lightLevel < 8)
            {
                const int lightCount = m_RP.m_DLights[lightLevel].Num();
                for (int lightIndex = 0; lightIndex < lightCount && lightIndex < 32; ++lightIndex)
                {
                    if (!(m_RP.m_DynLMask & (1u << lightIndex)))
                        continue;
                    CDLight* light = m_RP.m_DLights[lightLevel][lightIndex];
                    if (!light || !(light->m_Flags &
                                    (DLF_DIRECTIONAL | DLF_POINT | DLF_PROJECT)))
                        continue;
                    Vec3d objectLightPosition;
                    TransformPosition(objectLightPosition, light->m_Origin,
                                      m_RP.m_pCurObject->GetInvMatrix());
                    const float lightVectorLength = objectLightPosition.Length();
                    const bool directionalLight = (light->m_Flags & DLF_DIRECTIONAL) != 0;
                    // OpenGL schedules projected lights as their own light
                    // passes. Vulkan uses the cookie atlas when it is present
                    // while preserving position/radius attenuation and the
                    // frustum cone for its stock shader approximation.
                    const bool pointLight = !directionalLight &&
                        (light->m_Flags & (DLF_POINT | DLF_PROJECT)) != 0;
                    if (!pointLight && !(lightVectorLength > 1.0e-6f))
                        continue;

                    std::array<float, 11> parameters;
                    memcpy(parameters.data(), materialLighting, sizeof(materialLighting));
                    parameters[0] = objectLightPosition.x;
                    parameters[1] = objectLightPosition.y;
                    parameters[2] = objectLightPosition.z;
                    parameters[3] = pointLight ? light->m_fRadius : 0.0f;
                    const float lightScale = directionalLight ? 1.5f : 1.0f;
                    parameters[4] *= light->m_Color.r * lightScale;
                    parameters[5] *= light->m_Color.g * lightScale;
                    parameters[6] *= light->m_Color.b * lightScale;
                    if (light->m_Flags & DLF_PROJECT)
                    {
                        const Matrix44 inverseObject = m_RP.m_pCurObject->GetInvMatrix();
                        Vec3d projectorDirection = inverseObject.TransformVectorOLD(
                            light->m_Orientation.m_vForward);
                        if (projectorDirection.Normalize() == 0.0f)
                            projectorDirection = Vec3d(1.0f, 0.0f, 0.0f);
                        parameters[8] = projectorDirection.x;
                        parameters[9] = projectorDirection.y;
                        parameters[10] = projectorDirection.z;
                        const float halfAngle = clamp_tpl(light->m_fLightFrustumAngle,
                                                          0.0f, 89.9f);
                        parameters[7] = -crymax(0.001f,
                            cry_cosf(halfAngle * gf_PI / 180.0f));
                    }
                    if (hardwareSpecularPass)
                    {
                        const auto safeNormalize = [](Vec3d& value)
                        {
                            if (value.Normalize() == 0.0f)
                                value = Vec3d(0.0f, 0.0f, 1.0f);
                        };
                        Vec3d cameraPosition = GetCamera().GetPos();
                        Vec3d objectCameraPosition;
                        Matrix44 inverseObject = m_RP.m_pCurObject->GetInvMatrix();
                        TransformPosition(objectCameraPosition, cameraPosition, inverseObject);
                        Vec3d viewDirection = objectCameraPosition;
                        Vec3d lightDirection = objectLightPosition;
                        if (pointLight)
                        {
                            const float distance = lightDirection.Length();
                            const float normalizedDistance = light->m_fRadius > 0.0f ?
                                distance / light->m_fRadius : 1.0f;
                            if (normalizedDistance >= 1.0f)
                                continue;
                            const float x2 = normalizedDistance * normalizedDistance;
                            const float attenuation = 2.0f *
                                (2.0f * x2 * normalizedDistance - 3.0f * x2 + 1.0f);
                            const float attenuationScale = crymax(0.0f, attenuation);
                            safeNormalize(lightDirection);
                            safeNormalize(viewDirection);
                            Vec3d halfVector = lightDirection + viewDirection;
                            safeNormalize(halfVector);
                            parameters[0] = halfVector.x;
                            parameters[1] = halfVector.y;
                            parameters[2] = halfVector.z;
                            parameters[3] = -10.0f;
                            const bool hasSpecularColor = light->m_SpecColor.r > 0.0f ||
                                light->m_SpecColor.g > 0.0f || light->m_SpecColor.b > 0.0f;
                            const CFColor& lightSpecular = hasSpecularColor ?
                                light->m_SpecColor : light->m_Color;
                            float specularMaterial[3] = { 1.0f, 1.0f, 1.0f };
                            if (m_activeResources && m_activeResources->m_LMaterial)
                            {
                                const SSideMaterial& material = m_activeResources->m_LMaterial->Front;
                                specularMaterial[0] = material.m_Specular.r;
                                specularMaterial[1] = material.m_Specular.g;
                                specularMaterial[2] = material.m_Specular.b;
                                parameters[3] = -crymax(1.0f, material.m_SpecShininess);
                            }
                            parameters[4] = lightSpecular.r * specularMaterial[0] * attenuationScale;
                            parameters[5] = lightSpecular.g * specularMaterial[1] * attenuationScale;
                            parameters[6] = lightSpecular.b * specularMaterial[2] * attenuationScale;
                        }
                        else
                        {
                            safeNormalize(lightDirection);
                            safeNormalize(viewDirection);
                            Vec3d halfVector = lightDirection + viewDirection;
                            safeNormalize(halfVector);
                            parameters[0] = halfVector.x;
                            parameters[1] = halfVector.y;
                            parameters[2] = halfVector.z;
                            const bool hasSpecularColor = light->m_SpecColor.r > 0.0f ||
                                light->m_SpecColor.g > 0.0f || light->m_SpecColor.b > 0.0f;
                            const CFColor& lightSpecular = hasSpecularColor ?
                                light->m_SpecColor : light->m_Color;
                            float specularMaterial[3] = { 1.0f, 1.0f, 1.0f };
                            if (m_activeResources && m_activeResources->m_LMaterial)
                            {
                                const SSideMaterial& material = m_activeResources->m_LMaterial->Front;
                                specularMaterial[0] = material.m_Specular.r;
                                specularMaterial[1] = material.m_Specular.g;
                                specularMaterial[2] = material.m_Specular.b;
                                parameters[3] = -crymax(1.0f, material.m_SpecShininess);
                            }
                            else
                                parameters[3] = -10.0f;
                            parameters[4] = lightSpecular.r * specularMaterial[0] * lightScale;
                            parameters[5] = lightSpecular.g * specularMaterial[1] * lightScale;
                            parameters[6] = lightSpecular.b * specularMaterial[2] * lightScale;
                        }
                    }
                    // Emit material ambient once; following lights use additive
                    // RGB blending and must contribute only their diffuse term.
                    if (!lightPasses.empty())
                        parameters[7] = 0.0f;
                    if (light->m_Flags & DLF_PROJECT)
                    {
                        const float halfAngle = clamp_tpl(light->m_fLightFrustumAngle,
                                                          0.0f, 89.9f);
                        parameters[7] = -crymax(0.001f,
                            cry_cosf(halfAngle * gf_PI / 180.0f));
                    }
                    lightPasses.push_back(parameters);
                }
            }
        }
        if (lightPasses.empty())
        {
            std::array<float, 11> parameters;
            memcpy(parameters.data(), materialLighting, sizeof(materialLighting));
            lightPasses.push_back(parameters);
        }
        // Pass state is installed before mfDraw; render elements can refine it
        // locally through EF_SetState. The NULL implementation stores that
        // final value in m_CurState, so capture it at submission time.
        uint32_t renderState = static_cast<uint32_t>(m_CurState);
        if (m_polygonMode == R_WIREFRAME_MODE)
            renderState |= GS_POLYLINE;
        float globalOpacity = 1.0f;
        float alphaTestRef = 0.0f;
        bool additiveMaterial = false;
        float primaryColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float primaryColorMask[4] = {};
        const bool applyResourceStates = !m_RP.m_pShader ||
            !(m_RP.m_pShader->m_Flags2 & EF2_IGNORERESOURCESTATES);
        if (m_activeResources && applyResourceStates)
        {
            alphaTestRef = m_activeResources->m_AlphaRef;
            globalOpacity = m_activeResources->m_Opacity;
            additiveMaterial = (m_activeResources->m_ResFlags & MTLFLAG_ADDITIVE) != 0;
            if (globalOpacity != 1.0f)
            {
                renderState &= ~(GS_BLEND_MASK | GS_DEPTHWRITE);
                renderState |= additiveMaterial ?
                    (GS_BLSRC_ONE | GS_BLDST_ONE) :
                    (GS_BLSRC_SRCALPHA | GS_BLDST_ONEMINUSSRCALPHA);
                if (lightPasses.size() > 1)
                    lightPasses.resize(1);
            }
        }
        globalOpacity = clamp_tpl(globalOpacity, 0.0f, 1.0f);
        if (m_activePass)
            EvaluatePassColor(*m_activePass, primaryColor, primaryColorMask);
        if (m_activeStateShaderState)
        {
            const SEfState& stateShader = *m_activeStateShaderState;
            const float invByte = 1.0f / 255.0f;
            if ((stateShader.m_Flags & ESF_RGBGEN) && stateShader.m_eEvalRGB == eERGB_Fixed)
            {
                // Match the legacy renderer's byte exchange before it stores
                // the fixed-function global color (RGBA bytes are BGRA on the
                // old packed-color path).
                primaryColor[0] = stateShader.m_FixedColor[3] * invByte;
                primaryColor[1] = stateShader.m_FixedColor[1] * invByte;
                primaryColor[2] = stateShader.m_FixedColor[2] * invByte;
                primaryColor[3] = stateShader.m_FixedColor[0] * invByte;
                primaryColorMask[0] = primaryColorMask[1] = primaryColorMask[2] = 1.0f;
            }
            if ((stateShader.m_Flags & ESF_ALPHAGEN) && stateShader.m_eEvalAlpha == eEALPHA_Fixed)
            {
                primaryColor[3] = (stateShader.m_Flags & ESF_RGBGEN) ?
                    stateShader.m_FixedColor[0] * invByte : 0.0f;
                primaryColorMask[3] = 1.0f;
            }
        }

        // GL's EF_Eval_RGBAGen writes generated global channels to
        // m_NeedGlobalColor, then EF_SetGlobalColor installs the byte color
        // used by EF_SetColorOp as GL_TEXTURE_ENV_COLOR on each TMU. Keep that
        // byte state here and pass it as the Vulkan combiner constant for all
        // eight stages. Channels sourced from client/vertex colors leave the
        // prior global constant intact, matching the GL stateful behavior.
        for (int channel = 0; channel < 4; ++channel)
        {
            if (primaryColorMask[channel] > 0.5f)
            {
                const float clamped = clamp_tpl(primaryColor[channel], 0.0f, 1.0f);
                m_RP.m_CurGlobalColor.bcolor[channel] =
                    static_cast<uint8_t>(clamped * 255.0f);
            }
        }
        const uint32_t textureEnvironmentColor = m_RP.m_CurGlobalColor.dcolor;

        uint32_t colorWriteMaskOverride = 0xffffffffu;
        if (m_activeStateShaderState && (m_activeStateShaderState->m_Flags & ESF_COLORMASK))
        {
            colorWriteMaskOverride = 0;
            if (m_activeStateShaderState->m_ColorMask[0]) colorWriteMaskOverride |= VK_COLOR_COMPONENT_R_BIT;
            if (m_activeStateShaderState->m_ColorMask[1]) colorWriteMaskOverride |= VK_COLOR_COMPONENT_G_BIT;
            if (m_activeStateShaderState->m_ColorMask[2]) colorWriteMaskOverride |= VK_COLOR_COMPONENT_B_BIT;
            if (m_activeStateShaderState->m_ColorMask[3]) colorWriteMaskOverride |= VK_COLOR_COMPONENT_A_BIT;
        }

        if (m_activePass)
        {
            // Hardware shader passes describe their samplers through the material
            // resources, rather than SShaderPass::m_TUnits (which is empty).
            if (m_activeHardwarePassType != eSHP_MAX &&
                m_activePass->m_TUnits.Num() == 0 && m_activeResources)
            {
                SEfResTexture* diffuse = m_activeResources->m_Textures[EFTT_DIFFUSE];
                if ((!diffuse || !diffuse->m_TU.m_ITexPic) &&
                    m_activeResources->m_Textures[EFTT_BUMP_DIFFUSE])
                    diffuse = m_activeResources->m_Textures[EFTT_BUMP_DIFFUSE];
                if (diffuse)
                {
                    if (!diffuse->m_TU.m_ITexPic && !diffuse->m_Name.empty())
                    {
                        const uint textureFlags = diffuse->m_TU.GetTexFlags();
                        const uint textureFlags2 = diffuse->m_TU.GetTexFlags2();
                        ITexPic* loaded = EF_LoadTexture(diffuse->m_Name.c_str(),
                            textureFlags, textureFlags2, eTT_Base,
                            static_cast<float>(diffuse->m_Amount));
                        if ((!loaded || !loaded->IsTextureLoaded()) &&
                            !m_activeResources->m_TexturePath.empty())
                        {
                            if (loaded)
                                loaded->Release(false);
                            std::string texturePath = m_activeResources->m_TexturePath.c_str();
                            if (!texturePath.empty() && texturePath.back() != '/' &&
                                texturePath.back() != '\\')
                                texturePath.push_back('/');
                            texturePath += diffuse->m_Name.c_str();
                            loaded = EF_LoadTexture(texturePath.c_str(), textureFlags,
                                textureFlags2, eTT_Base,
                                static_cast<float>(diffuse->m_Amount));
                        }
                        if (loaded && loaded->IsTextureLoaded())
                            diffuse->m_TU.m_ITexPic = loaded;
                        else if (loaded)
                            loaded->Release(false);
                    }
                    updateStockTextureMatrix(diffuse, 0, textureMatrices[0]);
                    diffuse->m_TU.mfUpdate();
                    if (diffuse->m_TU.m_ITexPic)
                        textureIds[0] = diffuse->m_TU.m_ITexPic->GetTextureID();
                    textureLodBias[0] = diffuse->m_TU.m_fTexFilterLodBias;
                    applyStockTextureMatrixOps(0, textureMatrices[0]);
                }
            }
            if (m_activePass->m_TUnits.Num() > 8)
                m_frameRenderer->RequirePanelFallback();
            for (int stage = 0; stage < 2 && stage < m_activePass->m_TUnits.Num(); ++stage)
            {
                const SShaderTexUnit* unit = &m_activePass->m_TUnits[stage];
                const int resourceSlot = unit->m_TexPic ? unit->m_TexPic->m_Bind : -1;
                SEfResTexture* textureResource = nullptr;
                if (resourceSlot >= 0 && resourceSlot < EFTT_MAX && m_activeResources)
                    textureResource = m_activeResources->m_Textures[resourceSlot];
                if (textureResource)
                {
                    // This is the stock resource update point for animated UV transforms
                    // (tiling, offsets, rotation and panning). The fixed-function GL
                    // renderer applies the resulting matrix before sampling each stage.
                    updateStockTextureMatrix(textureResource, stage,
                                             textureMatrices[stage]);
                    unit = &textureResource->m_TU;
                }
                if (stage == 1 && hasLightmapTexCoordSource)
                    textureStage1UsesTexCoord1 = lightmapStage == 1;
                applyStockTextureMatrixOps(stage, textureMatrices[stage]);
                generateLegacyTexCoords(stage, unit);
                const_cast<SShaderTexUnit*>(unit)->mfUpdate();
                textureLodBias[stage] = unit->m_fTexFilterLodBias;
                if (unit->m_ITexPic)
                {
                    textureIds[stage] = unit->m_ITexPic->GetTextureID();
                    // Use the texture selected by this actual template pass.
                    // EFTT_BUMP may be a stock-combined BUMP+NORMALMAP image;
                    // EFTT_BUMP_DIFFUSE is the corresponding $BumpDiffuse map.
                    if (resourceSlot == EFTT_BUMP || resourceSlot == EFTT_BUMP_DIFFUSE)
                    {
                        normalMapTextureId = textureIds[stage];
                        normalMapLodBias = textureLodBias[stage];
                    }
                }
                colorOps[stage] = unit->m_eColorOp;
                alphaOps[stage] = unit->m_eAlphaOp;
                colorArgs[stage] = unit->m_eColorArg;
                alphaArgs[stage] = unit->m_eAlphaArg;
            }

            if (m_activePass->m_TUnits.Num() > 2)
            {
                const SShaderTexUnit* unit = &m_activePass->m_TUnits[2];
                const int resourceSlot = unit->m_TexPic ? unit->m_TexPic->m_Bind : -1;
                SEfResTexture* textureResource = nullptr;
                if (resourceSlot >= 0 && resourceSlot < EFTT_MAX && m_activeResources)
                    textureResource = m_activeResources->m_Textures[resourceSlot];
                float stage2Matrix[16] = {};
                stage2Matrix[0] = stage2Matrix[5] = stage2Matrix[10] = stage2Matrix[15] = 1.0f;
                if (textureResource)
                {
                    updateStockTextureMatrix(textureResource, 2, stage2Matrix);
                    unit = &textureResource->m_TU;
                }
                applyStockTextureMatrixOps(2, stage2Matrix);
                const_cast<SShaderTexUnit*>(unit)->mfUpdate();
                if (unit->m_ITexPic)
                {
                    if (unit->m_eGenTC != eGTC_Base && unit->m_eGenTC != eGTC_LightMap)
                    {
                        // This three-stage shader can address either UV set
                        // already carried by the stock vertex layout. Shader
                        // generated coordinates still need a dedicated path.
                        m_frameRenderer->RequirePanelFallback();
                    }
                    else
                    {
                        const bool useTexCoord1 = unit->m_eGenTC == eGTC_LightMap ||
                                                  lightmapStage == 2;
                        textureStage2.textureId = unit->m_ITexPic->GetTextureID();
                        textureStage2.colorOp = unit->m_eColorOp;
                        textureStage2.alphaOp = unit->m_eAlphaOp;
                        textureStage2.colorArg = unit->m_eColorArg;
                        textureStage2.alphaArg = unit->m_eAlphaArg;
                        textureStage2.lodBias = unit->m_fTexFilterLodBias;
                        textureStage2.useTexCoord1 = useTexCoord1;
                        setStockUvTransform(textureStage2, stage2Matrix);
                        if (m_stageStateOverrides[2] & 1u) textureStage2.colorOp = m_stageColorOps[2];
                        if (m_stageStateOverrides[2] & 2u) textureStage2.alphaOp = m_stageAlphaOps[2];
                        if (m_stageStateOverrides[2] & 4u) textureStage2.colorArg = m_stageColorArgs[2];
                        if (m_stageStateOverrides[2] & 8u) textureStage2.alphaArg = m_stageAlphaArgs[2];
                        textureStage2Ptr = textureStage2.textureId > 0 ? &textureStage2 : nullptr;
                    }
                }
            }
            if (m_activePass->m_TUnits.Num() > 3)
            {
                const SShaderTexUnit* unit = &m_activePass->m_TUnits[3];
                const int resourceSlot = unit->m_TexPic ? unit->m_TexPic->m_Bind : -1;
                SEfResTexture* textureResource = nullptr;
                if (resourceSlot >= 0 && resourceSlot < EFTT_MAX && m_activeResources)
                    textureResource = m_activeResources->m_Textures[resourceSlot];
                float stage3Matrix[16] = {};
                stage3Matrix[0] = stage3Matrix[5] = stage3Matrix[10] = stage3Matrix[15] = 1.0f;
                if (textureResource)
                {
                    updateStockTextureMatrix(textureResource, 3, stage3Matrix);
                    unit = &textureResource->m_TU;
                }
                applyStockTextureMatrixOps(3, stage3Matrix);
                const_cast<SShaderTexUnit*>(unit)->mfUpdate();
                if (unit->m_ITexPic)
                {
                    if (unit->m_eGenTC != eGTC_Base && unit->m_eGenTC != eGTC_LightMap)
                        m_frameRenderer->RequirePanelFallback();
                    else
                    {
                        const bool useTexCoord1 = unit->m_eGenTC == eGTC_LightMap ||
                                                  lightmapStage == 3;
                        textureStage3.textureId = unit->m_ITexPic->GetTextureID();
                        textureStage3.colorOp = unit->m_eColorOp;
                        textureStage3.alphaOp = unit->m_eAlphaOp;
                        textureStage3.colorArg = unit->m_eColorArg;
                        textureStage3.alphaArg = unit->m_eAlphaArg;
                        textureStage3.lodBias = unit->m_fTexFilterLodBias;
                        textureStage3.useTexCoord1 = useTexCoord1;
                        setStockUvTransform(textureStage3, stage3Matrix);
                        if (m_stageStateOverrides[3] & 1u) textureStage3.colorOp = m_stageColorOps[3];
                        if (m_stageStateOverrides[3] & 2u) textureStage3.alphaOp = m_stageAlphaOps[3];
                        if (m_stageStateOverrides[3] & 4u) textureStage3.colorArg = m_stageColorArgs[3];
                        if (m_stageStateOverrides[3] & 8u) textureStage3.alphaArg = m_stageAlphaArgs[3];
                        textureStage3Ptr = textureStage3.textureId > 0 ? &textureStage3 : nullptr;
                    }
                }
            }
            for (int stageIndex = 4; stageIndex < 8 && stageIndex < m_activePass->m_TUnits.Num(); ++stageIndex)
            {
                const SShaderTexUnit* unit = &m_activePass->m_TUnits[stageIndex];
                const int resourceSlot = unit->m_TexPic ? unit->m_TexPic->m_Bind : -1;
                SEfResTexture* textureResource = nullptr;
                if (resourceSlot >= 0 && resourceSlot < EFTT_MAX && m_activeResources)
                    textureResource = m_activeResources->m_Textures[resourceSlot];
                float stageMatrix[16] = {};
                stageMatrix[0] = stageMatrix[5] = stageMatrix[10] = stageMatrix[15] = 1.0f;
                if (textureResource)
                {
                    updateStockTextureMatrix(textureResource, stageIndex, stageMatrix);
                    unit = &textureResource->m_TU;
                }
                applyStockTextureMatrixOps(stageIndex, stageMatrix);
                const_cast<SShaderTexUnit*>(unit)->mfUpdate();
                CryVR::VulkanStockTextureStage& translated = textureStages4To7[stageIndex - 4];
                translated.colorOp = unit->m_eColorOp;
                translated.alphaOp = unit->m_eAlphaOp;
                translated.colorArg = unit->m_eColorArg;
                translated.alphaArg = unit->m_eAlphaArg;
                translated.lodBias = unit->m_fTexFilterLodBias;
                translated.constant = 0xffffffffu;
                if (m_stageStateOverrides[stageIndex] & 1u) translated.colorOp = m_stageColorOps[stageIndex];
                if (m_stageStateOverrides[stageIndex] & 2u) translated.alphaOp = m_stageAlphaOps[stageIndex];
                if (m_stageStateOverrides[stageIndex] & 4u) translated.colorArg = m_stageColorArgs[stageIndex];
                if (m_stageStateOverrides[stageIndex] & 8u) translated.alphaArg = m_stageAlphaArgs[stageIndex];
                if (!unit->m_ITexPic)
                    continue;
                if (unit->m_eGenTC != eGTC_Base && unit->m_eGenTC != eGTC_LightMap)
                {
                    m_frameRenderer->RequirePanelFallback();
                    continue;
                }
                translated.useTexCoord1 = unit->m_eGenTC == eGTC_LightMap ||
                    stageIndex == lightmapStage;
                setStockUvTransform(translated, stageMatrix);
                translated.textureId = unit->m_ITexPic->GetTextureID();
                translated.wrapMode = TextureWrapModeForId(translated.textureId);
            }
            textureStages4To7Ptr = textureStages4To7;
        }
        else
        {
            // Render elements that bind fixed-function TMUs directly do not
            // have an SShaderPass to supply stages 2 and 3. Preserve the
            // renderer's selected textures and combine state for those
            // already-supported Vulkan texture stages.
            if (m_stageTextureIds[2] > 0)
            {
                textureStage2.textureId = m_stageTextureIds[2];
                textureStage2.colorOp = m_stageColorOps[2];
                textureStage2.alphaOp = m_stageAlphaOps[2];
                textureStage2.colorArg = m_stageColorArgs[2];
                textureStage2.alphaArg = m_stageAlphaArgs[2];
                textureStage2.useTexCoord1 = true;
                textureStage2Ptr = &textureStage2;
            }
            if (m_stageTextureIds[3] > 0)
            {
                textureStage3.textureId = m_stageTextureIds[3];
                textureStage3.colorOp = m_stageColorOps[3];
                textureStage3.alphaOp = m_stageAlphaOps[3];
                textureStage3.colorArg = m_stageColorArgs[3];
                textureStage3.alphaArg = m_stageAlphaArgs[3];
                textureStage3.useTexCoord1 = true;
                textureStage3Ptr = &textureStage3;
            }
            for (int stageIndex = 4; stageIndex < 8; ++stageIndex)
            {
                if (m_stageTextureIds[stageIndex] <= 0)
                    continue;
                CryVR::VulkanStockTextureStage& translated = textureStages4To7[stageIndex - 4];
                translated.textureId = m_stageTextureIds[stageIndex];
                translated.colorOp = m_stageColorOps[stageIndex];
                translated.alphaOp = m_stageAlphaOps[stageIndex];
                translated.colorArg = m_stageColorArgs[stageIndex];
                translated.alphaArg = m_stageAlphaArgs[stageIndex];
                translated.useTexCoord1 = true;
                translated.wrapMode = TextureWrapModeForId(translated.textureId);
            }
            textureStages4To7Ptr = textureStages4To7;
        }
        for (int stage = 0; stage < 2; ++stage)
        {
            if (m_stageStateOverrides[stage] & 1u) colorOps[stage] = m_stageColorOps[stage];
            if (m_stageStateOverrides[stage] & 2u) alphaOps[stage] = m_stageAlphaOps[stage];
            if (m_stageStateOverrides[stage] & 4u) colorArgs[stage] = m_stageColorArgs[stage];
            if (m_stageStateOverrides[stage] & 8u) alphaArgs[stage] = m_stageAlphaArgs[stage];
        }
        if (m_forceCurrentTextureForClientDraw && m_currentTextureId > 0)
            textureIds[0] = m_currentTextureId;

        textureStage2.constant = textureEnvironmentColor;
        textureStage3.constant = textureEnvironmentColor;
        for (int stage = 0; stage < 4; ++stage)
            textureStages4To7[stage].constant = textureEnvironmentColor;

        if (lightmapStage == 0 && normalMapTextureId > 0)
        {
            m_frameRenderer->RequirePanelFallback();
            return;
        }

        if (m_RP.m_pCurObject && (m_RP.m_pCurObject->m_ObjFlags & FOB_TRANS_MASK))
            mathMatrixMultiply(modelView, m_CameraMatrix.GetData(),
                               m_RP.m_pCurObject->m_Matrix.GetData(), g_CpuFlags);
        else
            memcpy(modelView, m_CameraMatrix.GetData(), sizeof(modelView));
        memcpy(textureMatrix, textureMatrices[0], sizeof(textureMatrix));

        const uint16_t* indexData = static_cast<const uint16_t*>(indices->m_VData) + firstIndex;
        std::vector<uint16_t> triangulatedQuads;
        uint32_t drawIndexCount = static_cast<uint32_t>(indexCount);
        const void* finalVertexData = flareDeformActive ?
            static_cast<const void*>(flareVertexData.data()) : drawVertexData;
        const uint32_t finalVertexCount = flareDeformActive ? 16u :
            static_cast<uint32_t>(vertices->m_NumVerts);
        int finalVertexFormat = flareDeformActive ? VERTEX_FORMAT_P3F_COL4UB_TEX2F :
            vertices->m_vertexformat;
        std::vector<struct_VERTEX_FORMAT_P3F_TEX2F> hardwareTexturedVertices;
        int hardwareTempTexCoordCount = -1;
        if (!flareDeformActive && textureIds[0] > 0 &&
            m_activeHardwarePassType != eSHP_MAX &&
            vertices->m_vertexformat == VERTEX_FORMAT_P3F &&
            m_RP.m_pRE && m_RP.m_pRE->mfGetType() == eDATA_OcLeaf)
        {
            CREOcLeaf* leafElement = static_cast<CREOcLeaf*>(m_RP.m_pRE);
            CLeafBuffer* leafBuffer = leafElement->m_pBuffer ?
                leafElement->m_pBuffer->GetVertexContainer() : nullptr;
            if (leafBuffer && leafBuffer->m_TempTexCoords)
            {
                hardwareTempTexCoordCount = leafBuffer->m_SecVertCount;
                const Vec3* positions = static_cast<const Vec3*>(finalVertexData);
                hardwareTexturedVertices.resize(finalVertexCount);
                for (uint32_t vertex = 0; vertex < finalVertexCount; ++vertex)
                {
                    hardwareTexturedVertices[vertex].xyz = positions[vertex];
                    hardwareTexturedVertices[vertex].st[0] = leafBuffer->m_TempTexCoords[vertex].vert[0];
                    hardwareTexturedVertices[vertex].st[1] = leafBuffer->m_TempTexCoords[vertex].vert[1];
                }
                finalVertexData = hardwareTexturedVertices.data();
                finalVertexFormat = VERTEX_FORMAT_P3F_TEX2F;
            }
        }
        if (m_auditFrameActive && finalVertexData && finalVertexCount > 0)
        {
            CryVR::VulkanVertexFormat auditFormat{};
            if (CryVR::GetVulkanVertexFormat(static_cast<uint32_t>(finalVertexFormat), auditFormat))
            {
                uint32_t auditPositionOffset = 0xffffffffu;
                uint32_t auditUvOffset = 0xffffffffu;
                for (uint32_t attribute = 0; attribute < auditFormat.attributeCount; ++attribute)
                {
                    const VkVertexInputAttributeDescription& desc = auditFormat.attributes[attribute];
                    if (desc.location == 0) auditPositionOffset = desc.offset;
                    else if (desc.location == 3) auditUvOffset = desc.offset;
                }
                const uint8_t* data = static_cast<const uint8_t*>(finalVertexData);
                float minPosition[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
                float maxPosition[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
                float minUv[2] = { FLT_MAX, FLT_MAX };
                float maxUv[2] = { -FLT_MAX, -FLT_MAX };
                for (uint32_t vertex = 0; vertex < finalVertexCount; ++vertex)
                {
                    const uint8_t* source = data + static_cast<size_t>(vertex) * auditFormat.stride;
                    if (auditPositionOffset != 0xffffffffu)
                    {
                        const float* position = reinterpret_cast<const float*>(source + auditPositionOffset);
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            minPosition[axis] = crymin(minPosition[axis], position[axis]);
                            maxPosition[axis] = crymax(maxPosition[axis], position[axis]);
                        }
                    }
                    if (auditUvOffset != 0xffffffffu)
                    {
                        const float* uv = reinterpret_cast<const float*>(source + auditUvOffset);
                        for (int axis = 0; axis < 2; ++axis)
                        {
                            minUv[axis] = crymin(minUv[axis], uv[axis]);
                            maxUv[axis] = crymax(maxUv[axis], uv[axis]);
                        }
                    }
                }
                CryLogAlways("Vulkan audit VERTEX_DATA frame=%d list=%d item=%d sourceFormat=%d finalFormat=%d sourceVertices=%d finalVertices=%u stride=%u tempUvVertices=%d boundsP=(%.3f,%.3f,%.3f)-(%.3f,%.3f,%.3f) boundsUV=(%.3f,%.3f)-(%.3f,%.3f)",
                    GetFrameID(), m_auditCurrentList, m_auditCurrentItem,
                    vertices->m_vertexformat, finalVertexFormat, vertices->m_NumVerts,
                    finalVertexCount, auditFormat.stride, hardwareTempTexCoordCount,
                    minPosition[0], minPosition[1], minPosition[2],
                    maxPosition[0], maxPosition[1], maxPosition[2],
                    minUv[0], minUv[1], maxUv[0], maxUv[1]);
                if (auditPositionOffset != 0xffffffffu)
                    for (int corner = 0; corner < 3 && corner < indexCount; ++corner)
                    {
                        const uint16_t vertex = indexData[corner];
                        if (vertex >= finalVertexCount) continue;
                        const uint8_t* source = data + static_cast<size_t>(vertex) * auditFormat.stride;
                        const float* position = reinterpret_cast<const float*>(source + auditPositionOffset);
                        const float* uv = auditUvOffset == 0xffffffffu ? nullptr :
                            reinterpret_cast<const float*>(source + auditUvOffset);
                        CryLogAlways("Vulkan audit VERTEX_SAMPLE frame=%d list=%d item=%d corner=%d index=%u p=(%.3f,%.3f,%.3f) uv=(%.3f,%.3f)",
                            GetFrameID(), m_auditCurrentList, m_auditCurrentItem, corner, vertex,
                            position[0], position[1], position[2], uv ? uv[0] : 0.0f,
                            uv ? uv[1] : 0.0f);
                    }
            }
        }
        if (flareDeformActive)
        {
            indexData = flareIndexData.data();
            drawIndexCount = static_cast<uint32_t>(flareIndexData.size());
            topology = 0;
        }
        if (beamDeformActive)
        {
            // OpenGL BeamDeform replaces the element's index stream with the
            // leaf buffer's secondary indices and draws them as triangles.
            CREOcLeaf* leafElement = static_cast<CREOcLeaf*>(m_RP.m_pRE);
            if (!leafElement->m_pBuffer ||
                static_cast<uint32_t>(indexCount) >
                    static_cast<uint32_t>(leafElement->m_pBuffer->m_SecIndices.Num()))
            {
                m_frameRenderer->RequirePanelFallback();
                return;
            }
            indexData = &leafElement->m_pBuffer->m_SecIndices[0];
            topology = 0;
        }
        if (topology == 3)
        {
            if ((indexCount % 4) != 0 ||
                static_cast<uint64_t>(indexCount) * 3u / 2u > 0xffffffffu)
            {
                m_frameRenderer->RequirePanelFallback();
                return;
            }
            triangulatedQuads.reserve(static_cast<size_t>(indexCount / 4) * 6);
            for (int quad = 0; quad < indexCount; quad += 4)
            {
                const uint16_t a = indexData[quad + 0];
                const uint16_t b = indexData[quad + 1];
                const uint16_t c = indexData[quad + 2];
                const uint16_t d = indexData[quad + 3];
                triangulatedQuads.push_back(a);
                triangulatedQuads.push_back(b);
                triangulatedQuads.push_back(c);
                triangulatedQuads.push_back(a);
                triangulatedQuads.push_back(c);
                triangulatedQuads.push_back(d);
            }
            indexData = triangulatedQuads.data();
            drawIndexCount = static_cast<uint32_t>(triangulatedQuads.size());
            topology = 0;
        }
        float objectClipPlane[4]{};
        const float* clipPlane = nullptr;
        if (m_RP.m_ClipPlaneEnabled && (!m_clipPlanesCVar || m_clipPlanesCVar->GetIVal()))
        {
            const float worldPlane[4] = {
                m_RP.m_CurClipPlane.m_Normal.x,
                m_RP.m_CurClipPlane.m_Normal.y,
                m_RP.m_CurClipPlane.m_Normal.z,
                m_RP.m_CurClipPlane.m_Dist
            };
            CCObject* object = m_RP.m_pCurObject;
            if (object && (object->m_ObjFlags & FOB_TRANS_MASK))
            {
                // A world-space plane transforms to object space by M^T,
                // since worldPosition = objectMatrix * objectPosition.
                for (int column = 0; column < 4; ++column)
                    objectClipPlane[column] = worldPlane[0] * object->m_Matrix(0, column) +
                        worldPlane[1] * object->m_Matrix(1, column) +
                        worldPlane[2] * object->m_Matrix(2, column) +
                        worldPlane[3] * object->m_Matrix(3, column);
            }
            else
                memcpy(objectClipPlane, worldPlane, sizeof(objectClipPlane));
            clipPlane = objectClipPlane;
        }
        for (size_t lightPass = 0; lightPass < lightPasses.size(); ++lightPass)
        {
            const float* drawTextureMatrix0 = textureMatrix;
            const float* drawTextureMatrix1 = textureMatrices[1];
            if (lightmapStage == 0)
            {
                if (textureIds[1] > 0)
                {
                    drawTextureMatrix0 = textureMatrices[1];
                    drawTextureMatrix1 = textureMatrix;
                }
                else
                    drawTextureMatrix1 = textureMatrix;
            }
            uint32_t passState = renderState;
            if (lightPass > 0)
            {
                passState &= ~(GS_BLEND_MASK | GS_DEPTHWRITE);
                passState |= GS_BLSRC_ONE | GS_BLDST_ONE | GS_COLMASKONLYRGB;
            }
            int projectorCookieTextureId = 0;
            float projectorBasis[9]{};
            float projectorFrustumScale = 1.0f;
            CDLight* const currentLight = m_RP.m_pCurLight;
            if (currentLight && (currentLight->m_Flags & DLF_PROJECT) &&
                currentLight->m_pLightImage && m_RP.m_pCurObject)
            {
                projectorCookieTextureId = currentLight->m_pLightImage->GetTextureID();
                const Matrix44 inverseObject = m_RP.m_pCurObject->GetInvMatrix();
                const Vec3d directions[3] = {
                    currentLight->m_Orientation.m_vForward,
                    currentLight->m_Orientation.m_vRight,
                    currentLight->m_Orientation.m_vUp
                };
                for (int axis = 0; axis < 3; ++axis)
                {
                    Vec3d objectDirection = inverseObject.TransformVectorOLD(directions[axis]);
                    if (objectDirection.Normalize() == 0.0f)
                        objectDirection = axis == 0 ? Vec3d(1.0f, 0.0f, 0.0f) :
                            (axis == 1 ? Vec3d(0.0f, 1.0f, 0.0f) : Vec3d(0.0f, 0.0f, 1.0f));
                    projectorBasis[axis * 3] = objectDirection.x;
                    projectorBasis[axis * 3 + 1] = objectDirection.y;
                    projectorBasis[axis * 3 + 2] = objectDirection.z;
                }
                projectorFrustumScale = crymax(1.0e-4f,
                    cry_tanf((90.0f - clamp_tpl(currentLight->m_fLightFrustumAngle,
                                               0.0f, 89.9f)) * gf_PI / 180.0f));
            }
            m_frameRenderer->SetStockProjector(projectorCookieTextureId,
                projectorCookieTextureId ? projectorBasis : nullptr, projectorFrustumScale);
            float fogRangeScale = 1.0f;
            if (m_RP.m_pRE && m_RP.m_pRE->mfGetType() == eDATA_OcLeaf)
            {
                const float leafFogScale = static_cast<CREOcLeaf*>(m_RP.m_pRE)->m_fFogScale;
                if (leafFogScale != 0.0f)
                    fogRangeScale = leafFogScale;
            }
            // CREOcLeaf::mfDraw temporarily scales GL_FOG_START/END around
            // this leaf's indexed submission. Snapshot the same range into
            // each Vulkan draw and restore the shared value immediately after.
            m_frameRenderer->SetStockFogRangeScale(fogRangeScale);
            if (m_auditFrameActive)
            {
                CryLogAlways("Vulkan audit SUBMIT frame=%d list=%d item=%d lightPass=%d/%d indices=%u topology=%d vertexFormat=%d vertices=%u state=0x%x cull=%d textures=(%d,%d) normal=%d opacity=%.3f alphaRef=%.3f fogScale=%.3f lightmapStage=%d lmuv=%d generatedVertices=%d",
                    GetFrameID(), m_auditCurrentList, m_auditCurrentItem,
                    static_cast<int>(lightPass + 1), static_cast<int>(lightPasses.size()),
                    drawIndexCount, topology, finalVertexFormat, finalVertexCount, passState,
                    m_activePass ? m_activeCull : m_currentCullMode,
                    textureIds[0], textureIds[1], normalMapTextureId, globalOpacity,
                    alphaTestRef, fogRangeScale, lightmapStage,
                    lightmapTexCoords.empty() ? 0 : 1,
                    flareDeformActive ? 1 : (generatedVertexData.empty() ? 0 : 1));
                CryLogAlways("Vulkan audit MODELVIEW frame=%d row0=(%.6f,%.6f,%.6f,%.6f) row1=(%.6f,%.6f,%.6f,%.6f) row2=(%.6f,%.6f,%.6f,%.6f) row3=(%.6f,%.6f,%.6f,%.6f)",
                    GetFrameID(), modelView[0], modelView[1], modelView[2], modelView[3],
                    modelView[4], modelView[5], modelView[6], modelView[7],
                    modelView[8], modelView[9], modelView[10], modelView[11],
                    modelView[12], modelView[13], modelView[14], modelView[15]);
                CryLogAlways("Vulkan audit TEXMATRIX0 frame=%d row0=(%.6f,%.6f,%.6f,%.6f) row1=(%.6f,%.6f,%.6f,%.6f) row2=(%.6f,%.6f,%.6f,%.6f) row3=(%.6f,%.6f,%.6f,%.6f)",
                    GetFrameID(), drawTextureMatrix0[0], drawTextureMatrix0[1], drawTextureMatrix0[2], drawTextureMatrix0[3],
                    drawTextureMatrix0[4], drawTextureMatrix0[5], drawTextureMatrix0[6], drawTextureMatrix0[7],
                    drawTextureMatrix0[8], drawTextureMatrix0[9], drawTextureMatrix0[10], drawTextureMatrix0[11],
                    drawTextureMatrix0[12], drawTextureMatrix0[13], drawTextureMatrix0[14], drawTextureMatrix0[15]);
            }
            const bool queued = m_frameRenderer->QueueStockClientIndexedDraw(
                finalVertexData,
                finalVertexCount, indexData,
                drawIndexCount, finalVertexFormat, topology,
                passState, m_activePass ? m_activeCull : m_currentCullMode,
                textureIds[0], textureIds[1],
                colorOps[0], alphaOps[0], colorOps[1], alphaOps[1],
                colorArgs[0], alphaArgs[0], textureEnvironmentColor,
                colorArgs[1], alphaArgs[1], textureEnvironmentColor,
                static_cast<uint32_t>(m_CurStencilState), m_CurStencRef, m_CurStencMask,
                modelView, drawTextureMatrix0, drawTextureMatrix1, lightPasses[lightPass].data(),
                lightPass > 0, globalOpacity, alphaTestRef,
                reinterpret_cast<const CryVR::VulkanBuffer*>(
                    vertices->m_VS[VSF_TANGENTS].m_VulkanBufferHandle),
                normalMapTextureId, primaryColor, primaryColorMask, colorWriteMaskOverride,
                textureLodBias[0], normalMapTextureId ? normalMapLodBias : textureLodBias[1],
                textureStage2Ptr, textureStage3Ptr, m_activePolygonOffset,
                m_activePolygonOffsetFactor, m_activePolygonOffsetUnits, clipPlane,
                TextureWrapModeForId(textureIds[0]),
                TextureWrapModeForId(normalMapTextureId ? normalMapTextureId : textureIds[1]),
                TextureWrapModeForId(textureStage2Ptr ? textureStage2Ptr->textureId : 0),
                TextureWrapModeForId(textureStage3Ptr ? textureStage3Ptr->textureId : 0),
                textureStages4To7Ptr,
                lightmapTexCoords.empty() ? nullptr : lightmapTexCoords.data(),
                textureStage1UsesTexCoord1, lightmapStage == 0,
                m_activePass && m_activePass->m_eEvalRGB == eERGB_OneMinusFromClient,
                m_RP.m_pCurObject &&
                    (m_RP.m_pCurObject->m_ObjFlags & FOB_NEAREST) != 0);
            if (m_auditFrameActive)
                CryLogAlways("Vulkan audit SUBMIT_RESULT frame=%d list=%d item=%d lightPass=%d queued=%d pendingUntranslated=%u",
                    GetFrameID(), m_auditCurrentList, m_auditCurrentItem,
                    static_cast<int>(lightPass), queued ? 1 : 0,
                    m_frameRenderer->GetUntranslatedDrawCount());
            m_frameRenderer->SetStockFogRangeScale(1.0f);
            m_frameRenderer->SetStockProjector(0, nullptr, 1.0f);
            if (!queued)
                m_frameRenderer->RequirePanelFallback();
        }
    }

private:
    uint32_t CurrentPanelBlendState() const
    {
        return static_cast<uint32_t>(m_CurState) & GS_BLEND_MASK;
    }

    bool CullGeometryForLightsEnabled()
    {
        if (!m_cullGeometryForLightsCVar && iConsole)
            m_cullGeometryForLightsCVar = iConsole->GetCVar("r_CullGeometryForLights");
        return m_cullGeometryForLightsCVar && m_cullGeometryForLightsCVar->GetIVal() != 0;
    }

    int TextureWrapModeForId(int textureId) const
    {
        const std::map<int, int>::const_iterator found = m_textureWrapOverrides.find(textureId);
        return found == m_textureWrapOverrides.end() ? -1 : found->second;
    }

    struct ScreenMatrixState
    {
        std::array<float, 16> matrix{};
        std::vector<std::array<float, 16>> stack;
        float width = 800.0f;
        float height = 600.0f;
    };

    static void SetIdentityScreenMatrix(float* matrix)
    {
        memset(matrix, 0, sizeof(float) * 16);
        matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
    }

    static void MultiplyScreenMatrix(float* matrix, const float* rhs)
    {
        float result[16] = {};
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                for (int k = 0; k < 4; ++k)
                    result[column * 4 + row] +=
                        matrix[k * 4 + row] * rhs[column * 4 + k];
        memcpy(matrix, result, sizeof(result));
    }

    void EvaluatePassColor(const SShaderPass& pass, float color[4], float mask[4]) const
    {
        const float invByte = 1.0f / 255.0f;
        const auto setFixedRgb = [&]()
        {
            for (int channel = 0; channel < 3; ++channel)
                color[channel] = pass.m_FixedColor.bcolor[channel] * invByte;
        };
        const auto setObjectRgb = [&](bool invert)
        {
            if (!m_RP.m_pCurObject) return;
            const float values[3] = { m_RP.m_pCurObject->m_Color.r,
                                      m_RP.m_pCurObject->m_Color.g,
                                      m_RP.m_pCurObject->m_Color.b };
            for (int channel = 0; channel < 3; ++channel)
                color[channel] = invert ? 1.0f - values[channel] : values[channel];
        };
        const auto setElementRgb = [&](bool invert)
        {
            if (!m_RP.m_pRE) return;
            const float values[3] = { m_RP.m_pRE->m_Color.r,
                                      m_RP.m_pRE->m_Color.g,
                                      m_RP.m_pRE->m_Color.b };
            for (int channel = 0; channel < 3; ++channel)
                color[channel] = invert ? 1.0f - values[channel] : values[channel];
        };
        switch (pass.m_eEvalRGB)
        {
        case eERGB_Fixed:
            setFixedRgb();
            mask[0] = mask[1] = mask[2] = mask[3] = 1.0f;
            color[3] = pass.m_FixedColor.bcolor[3] * invByte;
            break;
        case eERGB_Identity:
            color[0] = color[1] = color[2] = 1.0f;
            mask[0] = mask[1] = mask[2] = 1.0f;
            break;
        case eERGB_Object:
            setObjectRgb(false);
            mask[0] = mask[1] = mask[2] = 1.0f;
            break;
        case eERGB_OneMinusObject:
            setObjectRgb(true);
            mask[0] = mask[1] = mask[2] = 1.0f;
            break;
        case eERGB_RE:
            setElementRgb(false);
            mask[0] = mask[1] = mask[2] = 1.0f;
            break;
        case eERGB_OneMinusRE:
            setElementRgb(true);
            mask[0] = mask[1] = mask[2] = 1.0f;
            break;
        case eERGB_World:
            color[0] = m_WorldColor.r; color[1] = m_WorldColor.g; color[2] = m_WorldColor.b;
            mask[0] = mask[1] = mask[2] = 1.0f;
            break;
        case eERGB_Comps:
            if (pass.m_RGBComps)
            {
                const float* values = pass.m_RGBComps->mfGet();
                if (values)
                {
                    color[0] = values[0]; color[1] = values[1];
                    color[2] = values[2]; color[3] = values[3];
                    mask[0] = mask[1] = mask[2] = mask[3] = 1.0f;
                }
            }
            break;
        case eERGB_Wave:
            if (pass.m_WaveEvalRGB)
            {
                const float value = clamp_tpl(SEvalFuncs::EvalWaveForm(pass.m_WaveEvalRGB), 0.0f, 1.0f);
                color[0] = color[1] = color[2] = value;
                mask[0] = mask[1] = mask[2] = 1.0f;
            }
            break;
        case eERGB_Noise:
            if (pass.m_RGBNoise)
            {
                color[0] = clamp_tpl(RandomNum() * pass.m_RGBNoise->m_RangeR + pass.m_RGBNoise->m_ConstR, 0.0f, 1.0f);
                color[1] = clamp_tpl(RandomNum() * pass.m_RGBNoise->m_RangeG + pass.m_RGBNoise->m_ConstG, 0.0f, 1.0f);
                color[2] = clamp_tpl(RandomNum() * pass.m_RGBNoise->m_RangeB + pass.m_RGBNoise->m_ConstB, 0.0f, 1.0f);
                mask[0] = mask[1] = mask[2] = 1.0f;
            }
            break;
        case eERGB_StyleColor:
            if (CLightStyle* style = CLightStyle::mfGetStyle(pass.m_Style, m_RP.m_RealTime))
            {
                color[0] = style->m_Color.r; color[1] = style->m_Color.g; color[2] = style->m_Color.b;
                mask[0] = mask[1] = mask[2] = 1.0f;
            }
            break;
        case eERGB_StyleIntens:
            if (CLightStyle* style = CLightStyle::mfGetStyle(pass.m_Style, m_RP.m_RealTime))
            {
                for (int channel = 0; channel < 3; ++channel)
                    color[channel] = pass.m_FixedColor.bcolor[channel] * invByte * style->m_fIntensity;
                mask[0] = mask[1] = mask[2] = 1.0f;
            }
            break;
        default:
            // NoFill, identity and client color generation keep the vertex
            // color stream already provided by the stock mesh.
            break;
        }

        switch (pass.m_eEvalAlpha)
        {
        case eEALPHA_Fixed:
            if (pass.m_eEvalRGB != eERGB_Fixed)
            {
                color[3] = pass.m_FixedColor.bcolor[3] * invByte;
                mask[3] = 1.0f;
            }
            break;
        case eEALPHA_Identity:
            // XRenderOGL leaves the existing alpha alone when the RGB
            // generator already supplies a complete fixed/identity color.
            if (pass.m_eEvalRGB != eERGB_Fixed && pass.m_eEvalRGB != eERGB_Identity)
            {
                color[3] = 1.0f;
                mask[3] = 1.0f;
            }
            break;
        case eEALPHA_Object:
            if (m_RP.m_pCurObject) { color[3] = m_RP.m_pCurObject->m_Color.a; mask[3] = 1.0f; }
            break;
        case eEALPHA_OneMinusObject:
            if (m_RP.m_pCurObject) { color[3] = 1.0f - m_RP.m_pCurObject->m_Color.a; mask[3] = 1.0f; }
            break;
        case eEALPHA_RE:
            if (m_RP.m_pRE) { color[3] = m_RP.m_pRE->m_Color.a; mask[3] = 1.0f; }
            break;
        case eEALPHA_OneMinusRE:
            // Match GLRendPipeline.cpp's legacy eEALPHA_OneMinusRE branch,
            // which evaluates the current object alpha for render elements.
            if (m_RP.m_pCurObject)
            {
                color[3] = 1.0f - m_RP.m_pCurObject->m_Color.a;
                mask[3] = 1.0f;
            }
            break;
        case eEALPHA_World:
            color[3] = m_WorldColor.a;
            mask[3] = 1.0f;
            break;
        case eEALPHA_Comps:
            if (pass.m_eEvalRGB != eERGB_Comps && pass.m_RGBComps)
            {
                const float* values = pass.m_RGBComps->mfGet();
                if (values) { color[3] = values[0]; mask[3] = 1.0f; }
            }
            break;
        case eEALPHA_Wave:
            if (pass.m_WaveEvalAlpha)
            {
                color[3] = clamp_tpl(SEvalFuncs::EvalWaveForm(pass.m_WaveEvalAlpha), 0.0f, 1.0f);
                mask[3] = 1.0f;
            }
            break;
        case eEALPHA_Noise:
            if (pass.m_ANoise)
            {
                color[3] = clamp_tpl(RandomNum() * pass.m_ANoise->m_RangeA + pass.m_ANoise->m_ConstA, 0.0f, 1.0f);
                mask[3] = 1.0f;
            }
            break;
        case eEALPHA_Style:
            if (CLightStyle* style = CLightStyle::mfGetStyle(pass.m_Style, m_RP.m_RealTime))
            {
                color[3] = (pass.m_FixedColor.bcolor[3] * invByte) * style->m_fIntensity;
                mask[3] = 1.0f;
            }
            break;
        default:
            break;
        }
        for (int channel = 0; channel < 4; ++channel)
            color[channel] = clamp_tpl(color[channel], 0.0f, 1.0f);
    }

    void UpdateVulkanFog()
    {
        if (m_frameRenderer)
            m_frameRenderer->SetStockFog(m_fogEnabled, m_fogDensity, m_fogStart,
                                         m_fogEnd, m_fogColor, m_fogMode);
    }

    void UpdateLegacyProjection(const CCamera& camera)
    {
        const float ratio = camera.GetProjRatio();
        const float nearPlane = camera.GetZMin();
        const float farPlane = camera.GetZMax();
        const float verticalFov = camera.GetFov() * ratio;
        const float tangent = tanf(verticalFov * 0.5f);
        float* projection = m_ProjMatrix.GetData();
        memset(projection, 0, sizeof(float) * 16);
        if (!(ratio > 0.0f) || !(nearPlane > 0.0f) || !(farPlane > nearPlane) ||
            !(tangent > 0.0f) || tangent > 1.0e20f)
        {
            projection[0] = projection[5] = projection[10] = projection[15] = 1.0f;
            return;
        }

        // gluPerspective(cam.GetFov() in degrees * projRatio,
        // 1 / projRatio, zNear, zFar), stored as GL column-major floats.
        projection[0] = 1.0f / (tangent / ratio);
        projection[5] = 1.0f / tangent;
        projection[10] = -(farPlane + nearPlane) / (farPlane - nearPlane);
        projection[11] = -1.0f;
        projection[14] = -(2.0f * farPlane * nearPlane) / (farPlane - nearPlane);
    }

    void UpdateLegacyCameraInfo()
    {
        m_RP.m_ViewOrg = m_cam.GetPos();
        Matrix44 cameraBasis = m_CameraMatrix;
        cameraBasis.Transpose();
        m_RP.m_CamVecs[0][0] = -cameraBasis(2, 0);
        m_RP.m_CamVecs[0][1] = -cameraBasis(2, 1);
        m_RP.m_CamVecs[0][2] = -cameraBasis(2, 2);
        m_RP.m_CamVecs[1][0] = cameraBasis(1, 0);
        m_RP.m_CamVecs[1][1] = cameraBasis(1, 1);
        m_RP.m_CamVecs[1][2] = cameraBasis(1, 2);
        m_RP.m_CamVecs[2][0] = cameraBasis(0, 0);
        m_RP.m_CamVecs[2][1] = cameraBasis(0, 1);
        m_RP.m_CamVecs[2][2] = cameraBasis(0, 2);

        // Match EF_SetCameraInfo's GL matrix readback and multiplication
        // order; these matrices serve legacy CPU/render-element APIs. OpenXR
        // eye matrices are still built independently from runtime views.
        mathMatrixMultiply(m_CameraProjMatrix.GetData(), m_ProjMatrix.GetData(),
                           m_CameraMatrix.GetData(), g_CpuFlags);
        mathMatrixInverse(m_InvCameraProjMatrix.GetData(), m_CameraProjMatrix.GetData(), g_CpuFlags);
        m_RP.m_TransformFrame++;
        m_RP.m_FrameObject++;
        m_RP.m_PersFlags &= ~RBPF_MATRIXNOTLOADED;
        m_RP.m_PersFlags &= ~RBPF_WASWORLDSPACE;
        m_RP.m_ObjFlags = FOB_TRANS_MASK;
    }

    void UpdateLegacyObjectTransform(CCObject* object)
    {
        if (object && (object->m_ObjFlags & FOB_TRANS_MASK))
            mathMatrixMultiply(m_ViewMatrix.GetData(), m_CameraMatrix.GetData(),
                               object->m_Matrix.GetData(), g_CpuFlags);
        else
            m_ViewMatrix = m_CameraMatrix;
    }

    // Port the CPU-visible part of CGLRenderer::EF_PreRender(1) and
    // EF_SetCameraInfo. OpenGL obtains these values by reading GL matrices;
    // Vulkan already has the normalized legacy camera matrix in SetCamera.
    // Render elements such as billboards and particles consume this state
    // during mfDraw/mfPrepare, so it must be current before walking item lists.
    void PrepareStockFrameState(int recurseLevel)
    {
        m_RP.m_RenderFrame++;
        m_RP.m_Flags = 0;
        m_RP.m_pPrevObject = nullptr;
        m_RP.m_FrameObject++;
        UpdateLegacyCameraInfo();

        const int lightCount = m_RP.m_DLights[recurseLevel].Num();
        int lightIndex = 0;
        for (; lightIndex < lightCount; ++lightIndex)
        {
            CDLight* light = m_RP.m_DLights[recurseLevel][lightIndex];
            if (!light || (light->m_Flags & DLF_FAKE) || !(light->m_Flags & DLF_SUN))
                continue;
            Vec3d direction = light->m_Origin - m_cam.GetPos();
            direction.Normalize();
            m_RP.m_SunDir = direction;
        }
        if (lightIndex == lightCount && IsEquivalent(m_RP.m_SunDir, Vec3d(0, 0, 0)))
            m_RP.m_SunDir = Vec3d(0, 0, 1);
    }

    static int MapStockCullMode(ECull cull)
    {
        switch (cull)
        {
        case eCULL_None: return 0;
        case eCULL_Front: return R_CULL_FRONT;
        case eCULL_Back: return R_CULL_BACK;
        default: return R_CULL_BACK;
        }
    }

    void DrawRenderItem(CRendElement* element, SShader* shader, SShaderPass* pass,
                        SRenderShaderResources* resources, uint32_t state, int cull,
                        bool hardwareTechniquePass = false)
    {
        if (m_auditFrameActive)
            CryLogAlways("Vulkan audit PASS_BEGIN frame=%d list=%d item=%d re=%p type=%d shader=%s pass=%p passType=%d hw=%d state=0x%x cull=%d opacity=%.3f texUnits=%d fog=%p object=%p objectFlags=0x%x",
                GetFrameID(), m_auditCurrentList, m_auditCurrentItem, element,
                element ? element->mfGetType() : -1,
                shader ? shader->m_Name.c_str() : "<null>", pass,
                pass ? (hardwareTechniquePass ?
                    static_cast<int>(static_cast<SShaderPassHW*>(pass)->m_ePassType) : -1) : -1,
                hardwareTechniquePass ? 1 : 0, state, cull, m_RP.m_fCurOpacity,
                pass ? pass->m_TUnits.Num() : 0, m_RP.m_pFogVolume,
                m_RP.m_pCurObject, m_RP.m_pCurObject ? m_RP.m_pCurObject->m_ObjFlags : 0);
        if (m_activeStateShaderState)
        {
            const SEfState& stateShader = *m_activeStateShaderState;
            if (stateShader.m_Flags & ESF_POLYLINE)
                state |= GS_POLYLINE;
            if (stateShader.m_Flags & ESF_NOCULL)
                cull = R_CULL_NONE;
            else if (stateShader.m_Flags & ESF_CULLFRONT)
                cull = R_CULL_FRONT;
            if (stateShader.m_Flags & ESF_STATE)
            {
                if (stateShader.m_State & GS_NODEPTHTEST)
                    state |= GS_NODEPTHTEST;
                const uint32_t depthFuncMask = GS_DEPTHFUNC_EQUAL | GS_DEPTHFUNC_GREAT;
                if (stateShader.m_State & depthFuncMask)
                    state = (state & ~depthFuncMask) | (stateShader.m_State & depthFuncMask);
            }
        }
        EF_SetState(static_cast<int>(state));
        memset(m_stageStateOverrides, 0, sizeof(m_stageStateOverrides));
        CCObject* object = m_RP.m_pCurObject;
        if (object && CRenderer::CV_r_scissor && object->m_nScissorX2)
        {
            SetScissor(object->m_nScissorX1, object->m_nScissorY1,
                       object->m_nScissorX2 - object->m_nScissorX1,
                       object->m_nScissorY2 - object->m_nScissorY1);
        }
        else
            SetScissor(0, 0, 0, 0);

        m_RP.m_RendPass++;
        if (element->mfGetType() == eDATA_ClearStencil)
        {
            // The NULL renderer's CREClearStencil implementation is empty.
            // Record the equivalent ordered Vulkan attachment clear exactly
            // once even when the legacy shader has multiple passes.
            if (m_RP.m_RendPass == 1 && m_frameRenderer &&
                !m_frameRenderer->QueueStockClearStencil())
                m_frameRenderer->RequirePanelFallback();
            SetScissor(0, 0, 0, 0);
            return;
        }
        if (element->mfGetType() == eDATA_TriMeshShadow)
        {
            // XRenderNULL leaves CRETriMeshShadow::mfDraw empty. Recreate the
            // stock OpenGL two-sided z-fail pass using Vulkan's front/back
            // stencil operations and the shadow volume's generated CPU mesh.
            if (m_RP.m_RendPass == 1 && !DrawStockShadowVolume(
                    static_cast<CRETriMeshShadow*>(element), shader))
                m_frameRenderer->RequirePanelFallback();
            SetScissor(0, 0, 0, 0);
            return;
        }
        if (!hardwareTechniquePass && element->mfGetType() == eDATA_OcLeaf)
        {
            // CGLRenderer::EF_FlushShader updates the leaf before its fixed
            // function draw so streamed/released buffers and required normals
            // exist. The NULL CREOcLeaf::mfDraw only reads the current buffer;
            // reproduce that preparation before it delegates to Vulkan DrawBuffer.
            int updateFlags = 0;
            if (resources && resources->m_bNeedNormals)
                updateFlags |= SHPF_NORMALS;
            if (!element->mfCheckUpdate(shader ? shader->m_VertexFormatId : 1, updateFlags))
            {
                if (m_frameRenderer)
                    m_frameRenderer->RequirePanelFallback();
                SetScissor(0, 0, 0, 0);
                return;
            }
        }
        // CGLRenderer::EF_SetResourcesState disables culling for two-sided
        // materials unless the shader explicitly ignores resource states.
        if (resources && shader && !(shader->m_Flags2 & EF2_IGNORERESOURCESTATES) &&
            (resources->m_ResFlags & MTLFLAG_2SIDED))
            cull = R_CULL_NONE;
        m_activePass = pass;
        m_activeResources = resources;
        m_activeCull = cull;
        m_activeHardwarePassType = hardwareTechniquePass ?
            static_cast<SShaderPassHW*>(pass)->m_ePassType : eSHP_MAX;
        m_activePolygonOffset = shader && (shader->m_Flags & EF_POLYGONOFFSET);
        if (hardwareTechniquePass &&
            (static_cast<SShaderPassHW*>(pass)->m_LMFlags & LMF_POLYOFFSET))
            m_activePolygonOffset = true;
        m_activePolygonOffsetFactor = m_polygonOffsetFactorCVar ?
            m_polygonOffsetFactorCVar->GetFVal() : -1.0f;
        m_activePolygonOffsetUnits = m_polygonOffsetUnitsCVar ?
            m_polygonOffsetUnitsCVar->GetFVal() : -4.0f;
        if (hardwareTechniquePass && element->mfGetType() == eDATA_OcLeaf)
        {
            // The stock hardware-technique CREOcLeaf path ends in
            // EF_DrawIndexedMesh. XRenderNULL implements that method as a
            // no-op, so submit this leaf's indexed section through the Vulkan
            // fixed-function translation while retaining the selected HW
            // pass textures, generators, material and render state.
            CREOcLeaf* leafElement = static_cast<CREOcLeaf*>(element);
            SShaderPassHW* hardwarePass = static_cast<SShaderPassHW*>(pass);
            if (!DrawHardwareTechniqueLeaf(leafElement, shader, hardwarePass, resources))
                m_frameRenderer->RequirePanelFallback();
        }
        else if (element->mfGetType() == eDATA_TempMesh)
        {
            // The OpenGL implementation feeds CRETempMesh through the same
            // indexed-mesh path as ordinary geometry; XRenderNULL's mfDraw is
            // empty, so route its transient buffers through Vulkan DrawBuffer.
            CRETempMesh* tempMesh = static_cast<CRETempMesh*>(element);
            if (tempMesh->m_VBuffer && tempMesh->m_Inds.m_VData &&
                tempMesh->m_Inds.m_nItems > 0)
            {
                DrawBuffer(tempMesh->m_VBuffer, &tempMesh->m_Inds,
                           tempMesh->m_Inds.m_nItems, 0, R_PRIMV_TRIANGLES,
                           0, 0, nullptr);
            }
            else if (m_frameRenderer)
                m_frameRenderer->RequirePanelFallback();
        }
        else if (element->mfGetType() == eDATA_Beam)
        {
            // CREBeam::mfPrepare selected the model's first material and
            // installed its CMatInfo/indices in m_RP. Execute that chunk via
            // the same stock buffer path so eDT_Beam evaluates the model's
            // original beam parameters instead of leaving mfDraw as a no-op.
            CREBeam* beam = static_cast<CREBeam*>(element);
            CREOcLeaf* materialElement = m_RP.m_pRE &&
                m_RP.m_pRE->mfGetType() == eDATA_OcLeaf ?
                static_cast<CREOcLeaf*>(m_RP.m_pRE) : nullptr;
            CLeafBuffer* leafBuffer = beam->m_pBuffer ?
                beam->m_pBuffer->GetVertexContainer() : nullptr;
            if (!materialElement || !materialElement->m_pChunk || !leafBuffer ||
                !leafBuffer->m_pVertexBuffer || !leafBuffer->m_Indices.m_VData)
                m_frameRenderer->RequirePanelFallback();
            else
            {
                const int updateFlags = m_activeResources && m_activeResources->m_bNeedNormals ?
                    SHPF_NORMALS : 0;
                if (!materialElement->mfCheckUpdate(
                        shader ? shader->m_VertexFormatId : 1, updateFlags))
                    m_frameRenderer->RequirePanelFallback();
                else
                    DrawBuffer(leafBuffer->m_pVertexBuffer, &leafBuffer->m_Indices,
                               m_RP.m_RendNumIndices, m_RP.m_FirstIndex,
                               leafBuffer->m_nPrimetiveType, 0,
                               m_RP.m_RendNumVerts, materialElement->m_pChunk);
            }
        }
        else if (element->mfGetType() == eDATA_Poly)
        {
            // CREPolyMesh owns CPU vertices and indices rather than a leaf
            // buffer. Its legacy mfPrepare only appends indices to the GL
            // pipe (the vertex-format switch is commented out), so translate
            // its source data into the selected stock Vulkan vertex layout.
            if (!DrawStockPolyMesh(static_cast<CREPolyMesh*>(element), shader))
                m_frameRenderer->RequirePanelFallback();
        }
        else if (element->mfGetType() == eDATA_ClientPoly)
        {
            // CREClientPoly::mfPrepare copies this element's self-contained
            // position/color/UV arrays into OpenGL's shared pipe buffer.
            // Submit its indexed triangles directly through the same Vulkan
            // client-buffer path used by other transient geometry.
            if (!DrawStockClientPoly(static_cast<CREClientPoly*>(element)))
                m_frameRenderer->RequirePanelFallback();
        }
        else if (element->mfGetType() == eDATA_PolyBlend ||
                 element->mfGetType() == eDATA_AnimPolyBlend)
        {
            if (!DrawStockPolyBlend(static_cast<CREPolyBlend_Base*>(element), shader))
                m_frameRenderer->RequirePanelFallback();
        }
        else if (element->mfGetType() == eDATA_ParticleSpray)
        {
            if (!DrawStockParticleSpray(static_cast<CREParticleSpray*>(element), shader))
                m_frameRenderer->RequirePanelFallback();
        }
        else if (element->mfGetType() == eDATA_Glare)
        {
            // XRenderNULL replaces CREGlare::mfDraw with a no-op. The stock
            // OpenGL element is a full-screen textured quad; represent it as
            // a stereo panel image using the active pass's first texture.
            if (hardwareTechniquePass || !DrawStockGlare(
                    pass, resources, CurrentPanelBlendState()))
                m_frameRenderer->RequirePanelFallback();
        }
        else if (element->mfGetType() == eDATA_ScreenProcess)
        {
            // OpenGL has a low-spec screen-process route that does not depend
            // on its Cg programs. Preserve its timed flashbang and fade
            // overlays through the per-eye Vulkan panel compositor.
            if (hardwareTechniquePass || !DrawStockScreenProcess(
                    static_cast<CREScreenProcess*>(element)))
                m_frameRenderer->RequirePanelFallback();
        }
        else if (element->mfGetType() == eDATA_FlashBang)
        {
            // The OpenGL flash-bang element advances its one-second fade and
            // draws the active pass texture as an 800x600 overlay. XRenderNULL
            // drops both operations, so preserve the timer and queue the same
            // texture through the stereo panel path.
            if (hardwareTechniquePass || !DrawStockFlashBang(
                    static_cast<CREFlashBang*>(element), pass, resources,
                    CurrentPanelBlendState()))
                m_frameRenderer->RequirePanelFallback();
        }
        else if (element->mfGetType() == eDATA_Flare)
        {
            if (!DrawStockFlare(static_cast<CREFlare*>(element), shader))
                m_frameRenderer->RequirePanelFallback();
        }
        else
            element->mfDraw(shader, pass);
        m_activeCull = R_CULL_BACK;
        m_activeResources = nullptr;
        m_activePass = nullptr;
        m_activeHardwarePassType = eSHP_MAX;
        m_activePolygonOffset = false;
        SetScissor(0, 0, 0, 0);
    }

    bool DrawHardwareTechniqueLeaf(CREOcLeaf* element, SShader* shader,
                                   SShaderPassHW* pass,
                                   SRenderShaderResources* resources)
    {
        if (!element || !element->m_pBuffer || !element->m_pChunk || !shader || !pass)
            return false;
        int updateFlags = m_RP.m_pCurTechnique ? m_RP.m_pCurTechnique->m_Flags : 0;
        if (resources && resources->m_bNeedNormals)
            updateFlags |= SHPF_NORMALS;
        if (shader->m_eSort == eS_TerrainLightPass || shader->m_eSort == eS_Terrain)
            updateFlags |= FHF_TERRAIN;
        if (!element->mfCheckUpdate(shader->m_VertexFormatId, updateFlags))
            return false;

        CLeafBuffer* leafBuffer = element->m_pBuffer->GetVertexContainer();
        if (!leafBuffer || !leafBuffer->m_pVertexBuffer ||
            !leafBuffer->m_Indices.m_VData)
            return false;
        if (CullGeometryForLightsEnabled() && EF_IsOnlyLightPass(pass) &&
            m_RP.m_pCurLightIndices &&
            m_RP.m_pCurLightIndices != &m_RP.m_FakeLightIndices)
        {
            int lightIndexCount = 0;
            unsigned short* lightIndices = m_RP.m_pCurLightIndices->GetIndices(lightIndexCount);
            if (!lightIndices || lightIndexCount <= 0)
                return true;
            SVertexStream lightIndexStream;
            lightIndexStream.m_VData = lightIndices;
            lightIndexStream.m_nItems = lightIndexCount;
            DrawBuffer(leafBuffer->m_pVertexBuffer, &lightIndexStream,
                       lightIndexCount, 0, R_PRIMV_TRIANGLES,
                       0, element->m_pChunk->nNumVerts, element->m_pChunk);
            return true;
        }
        DrawBuffer(leafBuffer->m_pVertexBuffer, &leafBuffer->m_Indices,
                   element->m_pChunk->nNumIndices,
                   element->m_pChunk->nFirstIndexId,
                   leafBuffer->m_nPrimetiveType,
                   0, element->m_pChunk->nNumVerts, element->m_pChunk);
        return true;
    }

    bool DrawStockPolyMesh(CREPolyMesh* element, SShader* shader)
    {
        if (!element || !shader || !element->TriVerts || !element->Indices ||
            element->NumVerts <= 0 || element->NumVerts > 65535 ||
            element->NumIndices <= 0 || element->NumIndices % 3 != 0)
            return false;

        CryVR::VulkanVertexFormat format{};
        const uint32_t formatId = static_cast<uint32_t>(shader->m_VertexFormatId);
        if (!CryVR::GetVulkanVertexFormat(formatId, format) ||
            formatId == 5 || formatId == 14 || formatId == 15)
            return false;
        for (int index = 0; index < element->NumIndices; ++index)
            if (element->Indices[index] >= element->NumVerts)
                return false;

        std::vector<uint8_t> vertices(static_cast<size_t>(format.stride) * element->NumVerts, 0);
        for (int vertexIndex = 0; vertexIndex < element->NumVerts; ++vertexIndex)
        {
            uint8_t* destination = vertices.data() +
                static_cast<size_t>(vertexIndex) * format.stride;
            const SMTriVert& source = element->TriVerts[vertexIndex];
            for (uint32_t attributeIndex = 0; attributeIndex < format.attributeCount; ++attributeIndex)
            {
                const VkVertexInputAttributeDescription& attribute = format.attributes[attributeIndex];
                switch (attribute.location)
                {
                case 0:
                    memcpy(destination + attribute.offset, &source.vert, sizeof(float) * 3);
                    break;
                case 1:
                    memcpy(destination + attribute.offset, &element->m_Plane.n, sizeof(float) * 3);
                    break;
                case 2:
                case 4:
                {
                    const uint32_t white = 0xffffffffu;
                    memcpy(destination + attribute.offset, &white, sizeof(white));
                    break;
                }
                case 3:
                    memcpy(destination + attribute.offset, source.dTC, sizeof(source.dTC));
                    break;
                case 5:
                    memcpy(destination + attribute.offset, source.lmTC, sizeof(source.lmTC));
                    break;
                default:
                    // Formats requiring tangent/binormal streams cannot be
                    // reconstructed from SMTriVert's source data.
                    return false;
                }
            }
        }

        CVertexBuffer vertexBuffer(vertices.data(), static_cast<int>(formatId), element->NumVerts);
        SVertexStream indexStream;
        indexStream.m_VData = element->Indices;
        indexStream.m_nItems = element->NumIndices;
        DrawBuffer(&vertexBuffer, &indexStream, element->NumIndices, 0,
                   R_PRIMV_TRIANGLES, 0, element->NumVerts, nullptr);
        return true;
    }

    bool DrawStockClientPoly(CREClientPoly* element)
    {
        if (!element || element->mNumVerts < 3 || element->mNumVerts > MAX_CLIENTPOLY_VERTS ||
            element->mNumIndices < 3 || element->mNumIndices % 3 != 0 ||
            element->mNumIndices > static_cast<int>(sizeof(element->mIndices)))
            return false;

        std::vector<struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F> vertices(element->mNumVerts);
        for (int vertexIndex = 0; vertexIndex < element->mNumVerts; ++vertexIndex)
        {
            vertices[vertexIndex].xyz = element->mVerts[vertexIndex].vert;
            vertices[vertexIndex].color = element->mVerts[vertexIndex].color;
            vertices[vertexIndex].st[0] = element->mVerts[vertexIndex].dTC[0];
            vertices[vertexIndex].st[1] = element->mVerts[vertexIndex].dTC[1];
        }

        std::vector<uint16_t> indices(element->mNumIndices);
        for (int index = 0; index < element->mNumIndices; ++index)
        {
            if (element->mIndices[index] >= element->mNumVerts)
                return false;
            indices[index] = element->mIndices[index];
        }

        CVertexBuffer vertexBuffer(vertices.data(), VERTEX_FORMAT_P3F_COL4UB_TEX2F,
                                   element->mNumVerts);
        SVertexStream indexStream;
        indexStream.m_VData = indices.data();
        indexStream.m_nItems = element->mNumIndices;
        DrawBuffer(&vertexBuffer, &indexStream, element->mNumIndices, 0,
                   R_PRIMV_TRIANGLES, 0, element->mNumVerts, nullptr);
        return true;
    }

    bool DrawStockPolyBlend(CREPolyBlend_Base* element, SShader* shader)
    {
        CCObject* object = m_RP.m_pCurObject;
        if (!element || !shader || !object || element->NumOrients < 0 ||
            element->NumOrients > 16 || (element->NumOrients && !element->Orients[0]))
            return false;

        CryVR::VulkanVertexFormat format{};
        const uint32_t formatId = static_cast<uint32_t>(shader->m_VertexFormatId);
        if (!CryVR::GetVulkanVertexFormat(formatId, format) ||
            formatId == 5 || formatId == 14 || formatId == 15)
            return false;

        Vec3d origin = object->GetTranslation();
        if (object->m_ObjFlags & FOB_DRSUN)
            origin = m_RP.m_ViewOrg + 2000.0f * m_RP.m_SunDir;

        CFColor color = object->m_Color;
        if (color.r + color.g + color.b == 0.0f)
            color = CFColor(1.0f);
        if (object->m_ObjFlags & FOB_DRSUN)
        {
            CREFlareGeom* flareGeom = object->m_RE && object->m_RE->mfGetType() == eDATA_FlareGeom ?
                static_cast<CREFlareGeom*>(object->m_RE) : nullptr;
            SFlareFrame* flareFrame = flareGeom ? &flareGeom->mFlareFr[0] : nullptr;
            bool visible = true;
            const uint64_t visibilityKey = reinterpret_cast<uint64_t>(flareGeom);
            bool hasVisibilityResult = false;
            if (visibilityKey && m_frameRenderer)
                hasVisibilityResult = m_frameRenderer->GetStockVisibility(visibilityKey, visible);
            if (flareFrame && hasVisibilityResult && visible != flareFrame->mbVis)
            {
                flareFrame->mbVis = visible;
                flareFrame->mDecayTime = m_RP.m_RealTime - 0.001f;
            }
            if (flareFrame)
            {
                if (!m_coronaFadeCVar && iConsole)
                    m_coronaFadeCVar = iConsole->GetCVar("r_CoronaFade");
                const float coronaFade = m_coronaFadeCVar ? m_coronaFadeCVar->GetFVal() : 0.125f;
                color = flareFrame->mColor;
                color.a = (visible ?
                    (m_RP.m_RealTime - flareFrame->mDecayTime) * coronaFade :
                    1.0f - (m_RP.m_RealTime - flareFrame->mDecayTime) * coronaFade);
                color.ClampAlpha();
            }
            if (!color.a)
                return true;
            color.ScaleCol(color.a);
            color.a = 1.0f;
        }
        else if (element->eColStyle || element->eAlphaStyle)
        {
            float fraction = 0.0f;
            if (element->eColStyle)
            {
                color = Col_White;
                if (element->eColStyle == ePBCS_Decay && element->LiveTime > 0.0f)
                {
                    fraction = (m_RP.m_RealTime - object->m_StartTime) / element->LiveTime;
                    color.ScaleCol(fraction);
                }
            }
            if (element->eAlphaStyle == ePBCS_Decay && element->LiveTimeA > 0.0f)
            {
                fraction = object->m_StartTime + element->LiveTimeA < m_RP.m_RealTime - 0.001f ?
                    1.0f : (m_RP.m_RealTime - object->m_StartTime) / element->LiveTimeA;
                color.a = element->ValA0 + fraction * (element->ValA1 - element->ValA0);
            }
        }
        else
        {
            const int lightStyleIndex = object->m_LightStyle;
            if (lightStyleIndex < 0 || lightStyleIndex >= CLightStyle::m_LStyles.Num())
                return false;
            CLightStyle* lightStyle = CLightStyle::m_LStyles[lightStyleIndex];
            if (!lightStyle)
                return false;
            lightStyle->mfUpdate(m_RP.m_RealTime);
            color = lightStyle->m_Color;
            color.a = 1.0f;
        }
        const uint32_t packedColor = color.GetTrue();

        static const uint16_t quadIndices[6] = { 3, 0, 2, 2, 0, 1 };
        static const float quadUv[4][2] = {
            { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f }
        };
        std::vector<uint8_t> vertices(static_cast<size_t>(format.stride) *
                                      static_cast<size_t>(element->NumOrients) * 4, 0);
        std::vector<uint16_t> indices(static_cast<size_t>(element->NumOrients) * 6);
        for (int orientIndex = 0; orientIndex < element->NumOrients; ++orientIndex)
        {
            const SOrient* orient = element->Orients[orientIndex];
            if (!orient)
                return false;
            Vec3d center = origin + orient->m_Coord.m_Org;
            Vec3d axisX, axisY, normal;
            if (orient->m_Flags & FOR_ORTHO)
            {
                axisX = m_RP.m_CamVecs[1];
                axisY = m_RP.m_CamVecs[2];
                normal = -m_RP.m_CamVecs[0];
            }
            else
            {
                axisX = orient->m_Coord.m_Vecs[1];
                axisY = orient->m_Coord.m_Vecs[2];
                normal = -orient->m_Coord.m_Vecs[0];
            }
            if (m_RP.m_PersFlags & RBPF_DRAWMIRROR)
                axisY = -axisY;

            const float distance = (center - m_RP.m_ViewOrg).GetLength();
            float scaleX = distance;
            float scaleY = distance;
            if (element->ScaleX > 0.0f)
            {
                scaleX = element->ScaleX;
                scaleY = element->ScaleY;
            }
            else if (element->ScaleX < 0.0f)
            {
                scaleX *= -element->ScaleX * 0.1f;
                scaleY *= -element->ScaleY * 0.1f;
            }
            else
                scaleX = scaleY = 10.0f;
            axisX *= scaleX;
            axisY *= scaleY;

            const Vec3d positions[4] = {
                center + axisX + axisY,
                center - axisX + axisY,
                center - axisX - axisY,
                center + axisX - axisY
            };
            const uint16_t baseVertex = static_cast<uint16_t>(orientIndex * 4);
            for (int i = 0; i < 6; ++i)
                indices[static_cast<size_t>(orientIndex) * 6 + i] =
                    static_cast<uint16_t>(baseVertex + quadIndices[i]);

            for (uint32_t vertexIndex = 0; vertexIndex < 4; ++vertexIndex)
            {
                uint8_t* destination = vertices.data() +
                    (static_cast<size_t>(orientIndex) * 4 + vertexIndex) * format.stride;
                for (uint32_t attributeIndex = 0; attributeIndex < format.attributeCount; ++attributeIndex)
                {
                    const VkVertexInputAttributeDescription& attribute = format.attributes[attributeIndex];
                    switch (attribute.location)
                    {
                    case 0:
                        memcpy(destination + attribute.offset, &positions[vertexIndex], sizeof(float) * 3);
                        break;
                    case 1:
                        memcpy(destination + attribute.offset, &normal, sizeof(float) * 3);
                        break;
                    case 2:
                    case 4:
                        memcpy(destination + attribute.offset, &packedColor, sizeof(packedColor));
                        break;
                    case 3:
                    case 5:
                        memcpy(destination + attribute.offset, quadUv[vertexIndex], sizeof(float) * 2);
                        break;
                    default:
                        return false;
                    }
                }
            }
        }

        CVertexBuffer vertexBuffer(vertices.data(), static_cast<int>(formatId),
                                   element->NumOrients * 4);
        SVertexStream indexStream;
        indexStream.m_VData = indices.data();
        indexStream.m_nItems = static_cast<int>(indices.size());
        const int originalObjectFlags = object->m_ObjFlags;
        const int originalRenderObjectFlags = m_RP.m_ObjFlags;
        // PolyBlend vertices are generated in world space by OpenGL's
        // FCEF_NEEDFILLBUF path, so do not apply the object's transform again.
        object->m_ObjFlags &= ~FOB_TRANS_MASK;
        m_RP.m_ObjFlags &= ~FOB_TRANS_MASK;
        if ((object->m_ObjFlags & FOB_DRSUN) && object->m_RE &&
            object->m_RE->mfGetType() == eDATA_FlareGeom && m_frameRenderer)
            m_frameRenderer->SetStockVisibilityKey(reinterpret_cast<uint64_t>(object->m_RE));
        DrawBuffer(&vertexBuffer, &indexStream, static_cast<int>(indices.size()), 0,
                   R_PRIMV_TRIANGLES, 0, element->NumOrients * 4, nullptr);
        if (m_frameRenderer)
            m_frameRenderer->SetStockVisibilityKey(0);
        object->m_ObjFlags = originalObjectFlags;
        m_RP.m_ObjFlags = originalRenderObjectFlags;
        return true;
    }

    bool DrawStockParticleSpray(CREParticleSpray* element, SShader* shader)
    {
        if (!element || !shader || !m_RP.m_pCurObject)
            return false;
        SEmitter& emitter = element->mEmitter;
        SParticleInfo& particleInfo = emitter.pi;
        if (particleInfo.ePT != ePTPoly && particleInfo.ePT != ePTPolySegs &&
            particleInfo.ePT != ePTBeam)
            return true; // OpenGL's point/line emitters are not built by mfPrepare either.

        CCObject* object = m_RP.m_pCurObject;
        if (emitter.Life && emitter.Life > 0.0f &&
            m_RP.m_RealTime - object->m_StartTime >= emitter.Life)
            return true;

        CryVR::VulkanVertexFormat format{};
        const uint32_t formatId = static_cast<uint32_t>(shader->m_VertexFormatId);
        if (!CryVR::GetVulkanVertexFormat(formatId, format) ||
            formatId == 5 || formatId == 14 || formatId == 15)
            return false;

        const Vec3d vecX = m_RP.m_CamVecs[1];
        const Vec3d vecY = m_RP.m_CamVecs[2];
        const Vec3d billboard[4] = { vecX + vecY, -vecX + vecY,
                                     -vecX - vecY, vecX - vecY };
        const float uv[4][2] = { {0.0f, 0.0f}, {1.0f, 0.0f},
                                 {1.0f, 1.0f}, {0.0f, 1.0f} };
        static const uint16_t quadIndices[6] = { 3, 0, 2, 2, 0, 1 };
        const Vec3d normal = -m_RP.m_CamVecs[0];

        std::vector<uint8_t> vertices;
        std::vector<uint16_t> indices;
        vertices.reserve(static_cast<size_t>(format.stride) * 1024);
        indices.reserve(1024 * 6);
        bool submittedAny = false;

        const auto flush = [&]() -> bool
        {
            if (indices.empty())
                return true;
            CVertexBuffer vertexBuffer(vertices.data(), static_cast<int>(formatId),
                                       static_cast<int>(vertices.size() / format.stride));
            SVertexStream indexStream;
            indexStream.m_VData = indices.data();
            indexStream.m_nItems = static_cast<int>(indices.size());
            DrawBuffer(&vertexBuffer, &indexStream, static_cast<int>(indices.size()), 0,
                       R_PRIMV_TRIANGLES, 0,
                       static_cast<int>(vertices.size() / format.stride), nullptr);
            submittedAny = true;
            vertices.clear();
            indices.clear();
            return true;
        };

        for (SParticle* particle = emitter.particle; particle; particle = particle->next)
        {
            const bool spark = particle->bSpark;
            const SParticleInfo& info = spark ? emitter.Spark : particleInfo;
            if (info.ePT != ePTPoly && info.ePT != ePTPolySegs && info.ePT != ePTBeam)
                continue;

            const int stack = info.StackSize;
            if (stack < 1 || stack > 8)
                return false;
            Vec3d start = info.ePT == ePTPoly ? particle->realPos : particle->prevPos[stack - 1];
            Vec3d step{};
            int steps = 1;
            if (info.ePT != ePTPoly)
            {
                step = particle->realPos - start;
                const float lengthSquared = GetLengthSquared(step);
                if (lengthSquared > 0.0f && particle->curSize > 0.0f && info.segmOffs > 0.0f)
                {
                    const float length = cry_sqrtf(lengthSquared);
                    const float requested = length / (particle->curSize * info.segmOffs);
                    if (requested > 1.0f)
                    {
                        step /= requested;
                        steps = QRound(requested);
                        if (steps > info.segmMax)
                            steps = info.segmMax;
                    }
                }
            }
            if (steps <= 0)
                continue;

            for (int segment = 0; segment < steps; ++segment)
            {
                if (vertices.size() / format.stride >= 65532)
                {
                    if (!flush())
                        return false;
                }
                const float halfSize = particle->curSize * 0.5f;
                const uint16_t base = static_cast<uint16_t>(vertices.size() / format.stride);
                for (int i = 0; i < 6; ++i)
                    indices.push_back(static_cast<uint16_t>(base + quadIndices[i]));

                uint8_t particleColor[4];
                if (gbRgb)
                {
                    particleColor[0] = static_cast<uint8_t>(QRound(particle->color[2] * 255.0f));
                    particleColor[1] = static_cast<uint8_t>(QRound(particle->color[1] * 255.0f));
                    particleColor[2] = static_cast<uint8_t>(QRound(particle->color[0] * 255.0f));
                }
                else
                {
                    particleColor[0] = static_cast<uint8_t>(QRound(particle->color[0] * 255.0f));
                    particleColor[1] = static_cast<uint8_t>(QRound(particle->color[1] * 255.0f));
                    particleColor[2] = static_cast<uint8_t>(QRound(particle->color[2] * 255.0f));
                }
                particleColor[3] = static_cast<uint8_t>(QRound(particle->color[3] * 255.0f));

                for (uint32_t vertexIndex = 0; vertexIndex < 4; ++vertexIndex)
                {
                    const Vec3d position = start + billboard[vertexIndex] * halfSize;
                    const size_t offset = vertices.size();
                    vertices.resize(offset + format.stride, 0);
                    uint8_t* destination = vertices.data() + offset;
                    for (uint32_t attributeIndex = 0; attributeIndex < format.attributeCount; ++attributeIndex)
                    {
                        const VkVertexInputAttributeDescription& attribute = format.attributes[attributeIndex];
                        switch (attribute.location)
                        {
                        case 0:
                            memcpy(destination + attribute.offset, &position, sizeof(float) * 3);
                            break;
                        case 1:
                            memcpy(destination + attribute.offset, &normal, sizeof(float) * 3);
                            break;
                        case 2:
                        case 4:
                            memcpy(destination + attribute.offset, particleColor, sizeof(particleColor));
                            break;
                        case 3:
                        case 5:
                            memcpy(destination + attribute.offset, uv[vertexIndex], sizeof(float) * 2);
                            break;
                        default:
                            return false;
                        }
                    }
                }
                start += step;
            }
        }

        if (!flush())
            return false;
        if (emitter.Life < 0.0f && !submittedAny)
            return true;
        return true;
    }

    bool DrawStockFlare(CREFlare* flare, SShader* shader)
    {
        CCObject* object = m_RP.m_pCurObject;
        if (!flare || !shader || !object || !m_frameOpen || !m_frameRenderer)
            return false;
        flare->mfPrepare();
        if (!m_flaresCVar && iConsole)
            m_flaresCVar = iConsole->GetCVar("r_Flares");
        if (!m_coronasCVar && iConsole)
            m_coronasCVar = iConsole->GetCVar("r_Coronas");
        if (!m_coronaSizeScaleCVar && iConsole)
            m_coronaSizeScaleCVar = iConsole->GetCVar("r_CoronaSizeScale");
        if (!m_coronaColorScaleCVar && iConsole)
            m_coronaColorScaleCVar = iConsole->GetCVar("r_CoronaColorScale");
        if (!m_sunStyleCoronasCVar && iConsole)
            m_sunStyleCoronasCVar = iConsole->GetCVar("r_SunStyleCoronas");
        if (!m_checkSunVisCVar && iConsole)
            m_checkSunVisCVar = iConsole->GetCVar("r_checkSunVis");

        const bool flaresEnabled = !m_flaresCVar || m_flaresCVar->GetIVal() != 0;
        const bool coronasEnabled = !m_coronasCVar || m_coronasCVar->GetIVal() != 0;
        if (!flaresEnabled && !coronasEnabled)
            return true;
        if ((object->m_ObjFlags & FOB_DRSUN) && (m_RP.m_PersFlags & RBPF_DONTDRAWSUN))
            return true;
        const Vec3d origin = object->GetTranslation();
        const Vec3d camera = m_RP.m_ViewOrg;
        const bool sun = (object->m_ObjFlags & FOB_DRSUN) != 0;
        const uint64_t visibilityKey = reinterpret_cast<uint64_t>(object);
        float visibilityFraction = 0.0f;
        const bool hasVisibilityResult = m_frameRenderer->GetStockVisibilityFraction(
            visibilityKey, visibilityFraction);
        float brightness = 1.0f;
        if (!sun && m_FS.m_FogEnd > 0.0f)
        {
            const float distance = (origin - camera).Length();
            const float fadeStart = m_FS.m_FogEnd * (2.0f / 3.0f);
            if (distance >= m_FS.m_FogEnd)
                brightness = 0.0f;
            else if (distance > fadeStart)
                brightness = (m_FS.m_FogEnd - distance) /
                    (m_FS.m_FogEnd - fadeStart);
        }
        object->m_TempVars[2] = 0.0f;
        CFColor objectColor = flare->m_Color;
        objectColor.a = brightness;
        const Vec3d cameraRight = m_RP.m_CamVecs[1];
        const Vec3d cameraUp = m_RP.m_CamVecs[2];
        const int oldState = m_CurState;
        const int oldCull = m_activePass ? m_activeCull : m_currentCullMode;
        const int oldObjectFlags = object->m_ObjFlags;
        const int oldRenderObjectFlags = m_RP.m_ObjFlags;
        const bool oldFog = m_fogEnabled;
        const bool oldTextureOverride = m_forceCurrentTextureForClientDraw;
        const int oldTexture = m_currentTextureId;
        SShaderPass* const oldActivePass = m_activePass;
        const EShaderPassType oldHardwarePassType = m_activeHardwarePassType;
        const bool useFlareLayer = flare->m_Pass && flare->m_Pass->m_TUnits.Num() > 0;
        object->m_ObjFlags &= ~FOB_TRANS_MASK;
        m_RP.m_ObjFlags &= ~FOB_TRANS_MASK;
        EnableFog(false);
        SetCullMode(R_CULL_NONE);

        bool submitted = true;
        const uint16_t quadIndices[6] = { 0, 1, 2, 0, 2, 3 };
        const auto submitMesh = [&](const struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F* vertices,
                                    int vertexCount, const uint16_t* indices,
                                    int indexCount, int primitiveType, int textureId,
                                    uint32_t state, bool flareLayer = false) -> bool
        {
            if (!vertices || !indices || vertexCount <= 0 || indexCount <= 0 || textureId <= 0)
                return textureId <= 0;
            CVertexBuffer vertexBuffer(const_cast<struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F*>(vertices),
                                       VERTEX_FORMAT_P3F_COL4UB_TEX2F, vertexCount);
            SVertexStream indexStream;
            indexStream.m_VData = const_cast<uint16_t*>(indices);
            indexStream.m_nItems = indexCount;
            m_currentTextureId = textureId;
            m_forceCurrentTextureForClientDraw = !flareLayer;
            if (flareLayer)
            {
                m_activePass = flare->m_Pass;
                m_activeHardwarePassType = eSHP_General;
            }
            EF_SetState(static_cast<int>(state));
            DrawBuffer(&vertexBuffer, &indexStream, indexCount, 0, primitiveType,
                       0, vertexCount, nullptr);
            m_activePass = oldActivePass;
            m_activeHardwarePassType = oldHardwarePassType;
            return true;
        };
        // CREFlare::mfDraw submits this color-masked quad to the OpenGL
        // occlusion query before drawing the corona/lens elements. Record the
        // matching camera-facing visibility quad, keyed by the owning object;
        // its sample result becomes available to the following frame.
        if (brightness > 0.0f && m_TexMan && m_TexMan->m_Text_White)
        {
            Vec3d toLight = origin - camera;
            float distance = toLight.Length();
            if (flare->m_fDistSizeFactor != 1.0f)
                distance = cry_powf(distance, flare->m_fDistSizeFactor);
            float coronaScale = flare->m_fScaleCorona;
            if (flare->m_pScaleCoronaParams)
                coronaScale = flare->m_pScaleCoronaParams->mfGet();
            distance *= coronaScale *
                (m_coronaSizeScaleCVar ? m_coronaSizeScaleCVar->GetFVal() : 1.0f);
            const float queryHalfSize = distance * 0.1f * flare->m_fVisAreaScale;
            const Vec3d queryX = cameraRight * queryHalfSize;
            const Vec3d queryY = cameraUp * queryHalfSize;
            const Vec3d queryPositions[4] = {
                origin + queryX + queryY, origin + queryX - queryY,
                origin - queryX - queryY, origin - queryX + queryY
            };
            struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F queryVertices[4]{};
            const float queryUv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
            for (int vertex = 0; vertex < 4; ++vertex)
            {
                queryVertices[vertex].xyz = queryPositions[vertex];
                queryVertices[vertex].st[0] = queryUv[vertex][0];
                queryVertices[vertex].st[1] = queryUv[vertex][1];
                queryVertices[vertex].color.dcolor = 0xffffffffu;
            }
            const int whiteTexture = m_TexMan->m_Text_White->GetTextureID();
            m_frameRenderer->SetStockVisibilityKey(visibilityKey);
            submitMesh(queryVertices, 4, quadIndices, 6, R_PRIMV_TRIANGLES,
                       whiteTexture, GS_NOCOLMASK);
            m_frameRenderer->SetStockVisibilityKey(visibilityKey, true);
            submitMesh(queryVertices, 4, quadIndices, 6, R_PRIMV_TRIANGLES,
                       whiteTexture, GS_NOCOLMASK | GS_NODEPTHTEST);
            m_frameRenderer->SetStockVisibilityKey(0);
        }
        float intensity = brightness > 0.0f ?
            (hasVisibilityResult ? visibilityFraction :
             (m_frameRenderer->SupportsStockVisibilityQueries() ? 0.0f : 1.0f)) : 0.0f;
        if (intensity < 0.05f)
            intensity = 0.0f;
        const bool sunStyle = m_sunStyleCoronasCVar &&
            m_sunStyleCoronasCVar->GetIVal() != 0;
        const bool sunRays = (sun || sunStyle) && m_checkSunVisCVar &&
            m_checkSunVisCVar->GetIVal() == 2;
        if (intensity >= 0.05f && sunRays)
            intensity = crymax(0.75f, intensity);
        // Match CREFlare::mfDraw's three-slot temporal visibility history.
        object->m_AmbColor[0] = object->m_AmbColor[1];
        object->m_AmbColor[1] = object->m_AmbColor[2];
        object->m_AmbColor[2] = object->m_TempVars[3];
        object->m_TempVars[3] = object->m_Angs2[0];
        object->m_Angs2[0] = object->m_Angs2[1];
        object->m_Angs2[1] = object->m_Angs2[2];
        object->m_Angs2[2] = object->m_TempVars[4];
        object->m_TempVars[4] = intensity;
        brightness = (object->m_AmbColor[0] + object->m_AmbColor[1] +
            object->m_AmbColor[2] + object->m_Angs2[0] + object->m_Angs2[1] +
            object->m_Angs2[2] + object->m_TempVars[3] + object->m_TempVars[4]) * 0.125f;
        objectColor.a = brightness;
        const auto setColor = [](struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F& vertex,
                                 const CFColor& color, bool useAlpha)
        {
            const float red = clamp_tpl(color[0], 0.0f, 1.0f);
            const float green = clamp_tpl(color[1], 0.0f, 1.0f);
            const float blue = clamp_tpl(color[2], 0.0f, 1.0f);
            vertex.color.bcolor[0] = static_cast<uint8_t>((gbRgb ? red : blue) * 255.0f);
            vertex.color.bcolor[1] = static_cast<uint8_t>(green * 255.0f);
            vertex.color.bcolor[2] = static_cast<uint8_t>((gbRgb ? blue : red) * 255.0f);
            vertex.color.bcolor[3] = static_cast<uint8_t>(
                clamp_tpl(useAlpha ? color[3] : 1.0f, 0.0f, 1.0f) * 255.0f);
        };
        if (coronasEnabled)
        {
            if (!(sun && m_bHeatVision))
            {
                Vec3d toLight = origin - camera;
                const float distance = toLight.Length();
                float distanceSize = distance;
                if (flare->m_fDistSizeFactor != 1.0f)
                    distanceSize = cry_powf(distanceSize, flare->m_fDistSizeFactor);
                float scale = flare->m_fScaleCorona;
                if (flare->m_pScaleCoronaParams)
                    scale = flare->m_pScaleCoronaParams->mfGet();
                const float sizeScale = m_coronaSizeScaleCVar ?
                    m_coronaSizeScaleCVar->GetFVal() : 1.0f;
                float width = distanceSize * scale * sizeScale;
                float height = width;
                float decay = objectColor.a * (m_coronaColorScaleCVar ?
                    m_coronaColorScaleCVar->GetFVal() : 1.0f);
                bool blind = false;
                Vec3d x0 = cameraUp, x1 = cameraUp;
                Vec3d y0 = cameraRight, y1 = cameraRight;
                if (flare->m_bBlind && object->m_pLight &&
                    (object->m_pLight->m_Flags & DLF_PROJECT))
                {
                    blind = true;
                    if (distance > 1.0e-6f)
                        toLight /= distance;
                    const float facing = -(toLight * object->m_pLight->m_Orientation.m_vForward);
                    if (facing >= 0.0f)
                    {
                        const float blindSize = facing * flare->m_fSizeBlindScale +
                            flare->m_fSizeBlindBias;
                        const float blindIntensity = facing * flare->m_fIntensBlindScale +
                            flare->m_fIntensBlindBias;
                        decay *= blindIntensity;
                        width *= blindSize;
                        height *= blindSize;
                        x0 *= width;
                        y0 *= height;
                        x1 *= -width;
                        y1 *= -height;
                        const float k = 0.1f;
                        float side = (cameraRight * object->m_pLight->m_Orientation.m_vForward) * (1.0f - k) + k;
                        x0 *= side + 1.0f;
                        x1 *= 1.0f - side + k;
                        float up = (cameraUp * object->m_pLight->m_Orientation.m_vForward) * (1.0f - k) + k;
                        y0 *= up + 1.0f;
                        y1 *= 1.0f - up + k;
                    }
                    else
                        decay = 0.0f;
                }
                if (!blind)
                {
                    x0 *= width; y0 *= height;
                    x1 *= -width; y1 *= -height;
                }
                if (flare->m_fDistIntensityFactor != 1.0f)
                    decay *= cry_powf(distance, flare->m_fDistIntensityFactor);
                if (decay > 1.0f) decay = 1.0f;
                if (decay > 0.001f)
                {
                    CFColor color = objectColor;
                    color[0] *= decay; color[1] *= decay; color[2] *= decay;
                    const bool sunStyle = m_sunStyleCoronasCVar &&
                        m_sunStyleCoronasCVar->GetIVal() != 0;
                    const bool sunRays = (sun || sunStyle) && m_checkSunVisCVar &&
                        m_checkSunVisCVar->GetIVal() == 2;
                    uint32_t state = flare->m_Pass ?
                        static_cast<uint32_t>(flare->m_Pass->m_RenderState) :
                        (GS_BLSRC_ONE | GS_BLDST_ONE | GS_NODEPTHTEST);
                    if (!flare->m_Pass || sunRays)
                        state = GS_BLSRC_ONE | GS_BLDST_ONE | GS_NODEPTHTEST;
                    else if ((state & GS_BLEND_MASK) ==
                             (GS_BLSRC_SRCALPHA | GS_BLDST_ONEMINUSSRCALPHA))
                        color[3] = decay;
                    std::vector<struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F> fan(10);
                    const Vec3d fanPositions[10] = {
                        origin, origin + x0 + y0, origin + x0, origin + x0 + y1,
                        origin + y1, origin + x1 + y1, origin + x1,
                        origin + x1 + y0, origin + y0, origin + x0 + y0
                    };
                    const float fanUv[10][2] = {
                        {0.5f,0.5f}, {0,0}, {0,0.5f}, {0,1}, {0.5f,1},
                        {1,1}, {1,0.5f}, {1,0}, {0.5f,0}, {0,0}
                    };
                    for (int vertex = 0; vertex < 10; ++vertex)
                    {
                        fan[vertex].xyz = fanPositions[vertex];
                        fan[vertex].st[0] = fanUv[vertex][0];
                        fan[vertex].st[1] = fanUv[vertex][1];
                        setColor(fan[vertex], color, true);
                    }
                    const int whiteTexture = flare->m_Map ? flare->m_Map->GetTextureID() :
                        (m_TexMan && m_TexMan->m_Text_White ?
                         m_TexMan->m_Text_White->GetTextureID() : 0);
                    const uint16_t fanIndices[10] = { 0,1,2,3,4,5,6,7,8,9 };
                    submitted = submitMesh(fan.data(), 10, fanIndices, 10,
                        R_PRIMV_TRIANGLE_FAN, whiteTexture, state, useFlareLayer) && submitted;
                }
            }
        }

        if (flaresEnabled && !(object->m_ObjFlags & FOB_DRSUN && m_bHeatVision) &&
            shader->m_Flares)
        {
            Vec3d lightDirection = origin - camera;
            if (lightDirection.Normalize() > 1.0e-6f)
            {
                const float facing = lightDirection | m_RP.m_CamVecs[0];
                if (facing > 0.2f)
                {
                    const Vec3d newLightPoint = camera + (1.0f / facing) * lightDirection;
                    const Vec3d centerPoint = camera + m_RP.m_CamVecs[0];
                    const Vec3d axis = newLightPoint - centerPoint;
                    const float intensity = facing * objectColor.a;
                    for (int flareIndex = 0; flareIndex < shader->m_Flares->m_NumFlares; ++flareIndex)
                    {
                        SSunFlare& lensFlare = shader->m_Flares->m_Flares[flareIndex];
                        lensFlare.m_Position = centerPoint + axis * lensFlare.m_Loc;
                        lensFlare.m_RenderSize = lensFlare.m_Scale;
                        const Vec3d x = cameraRight * lensFlare.m_RenderSize;
                        const Vec3d y = cameraUp * lensFlare.m_RenderSize;
                        CFColor color = lensFlare.m_Color * intensity;
                        const Vec3d positions[4] = {
                            lensFlare.m_Position + x + y, lensFlare.m_Position - x + y,
                            lensFlare.m_Position - x - y, lensFlare.m_Position + x - y
                        };
                        const float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
                        struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F quad[4];
                        for (int vertex = 0; vertex < 4; ++vertex)
                        {
                            quad[vertex].xyz = positions[vertex];
                            quad[vertex].st[0] = uv[vertex][0];
                            quad[vertex].st[1] = uv[vertex][1];
                            setColor(quad[vertex], color, false);
                        }
                        const int textureId = lensFlare.m_Tex ? lensFlare.m_Tex->GetTextureID() : 0;
                        submitted = submitMesh(quad, 4, quadIndices, 6, R_PRIMV_TRIANGLES,
                            textureId, GS_BLSRC_ONE | GS_BLDST_ONE | GS_NODEPTHTEST) && submitted;
                    }
                }
            }
        }

        m_forceCurrentTextureForClientDraw = oldTextureOverride;
        m_currentTextureId = oldTexture;
        object->m_ObjFlags = oldObjectFlags;
        m_RP.m_ObjFlags = oldRenderObjectFlags;
        EF_SetState(oldState);
        SetCullMode(oldCull);
        EnableFog(oldFog);
        return submitted;
    }

    bool DrawStockGlare(SShaderPass* pass, SRenderShaderResources* resources,
                        uint32_t blendState = GS_BLSRC_SRCALPHA | GS_BLDST_ONEMINUSSRCALPHA)
    {
        if (!pass || pass->m_TUnits.Num() == 0 || !m_frameRenderer)
            return false;
        SShaderTexUnit* unit = &pass->m_TUnits[0];
        const int resourceSlot = unit->m_TexPic ? unit->m_TexPic->m_Bind : -1;
        if (resourceSlot >= 0 && resourceSlot < EFTT_MAX && resources &&
            resources->m_Textures[resourceSlot])
        {
            resources->m_Textures[resourceSlot]->Update(0);
            unit = &resources->m_Textures[resourceSlot]->m_TU;
        }
        unit->mfUpdate();
        if (!unit->m_ITexPic)
            return false;

        const int textureId = unit->m_ITexPic->GetTextureID();
        if (textureId <= 0)
            return false;
        // CREGlare's GL quad spans its 800x600 logical screen, with full-range
        // UVs. QueuePanelImage keeps those legacy coordinates and carries the
        // pass/material tint plus resource opacity into both XR eyes.
        const float opacity = resources ? clamp_tpl(resources->m_Opacity, 0.0f, 1.0f) : 1.0f;
        return m_frameRenderer->QueuePanelImage(textureId, 0.0f, 0.0f, 800.0f, 600.0f,
            0.0f, 0.0f, 1.0f, 1.0f, 0.0f,
            m_materialColor[0], m_materialColor[1], m_materialColor[2],
            m_materialColor[3] * opacity, 800.0f, 600.0f, blendState);
    }

    bool DrawStockFlashBang(CREFlashBang* flashBang, SShaderPass* pass,
                            SRenderShaderResources* resources,
                            uint32_t blendState)
    {
        if (!flashBang || !iSystem || !iSystem->GetITimer())
            return false;
        ITimer* timer = iSystem->GetITimer();
        flashBang->UpdateFlashTimeOut(timer->GetFrameTime());
        return DrawStockGlare(pass, resources, blendState);
    }

    bool DrawStockScreenProcess(CREScreenProcess* screenProcess)
    {
        if (!screenProcess || !screenProcess->GetVars() || !m_frameRenderer ||
            !iSystem || !iSystem->GetITimer() || !m_TexMan || !m_TexMan->m_Text_White)
            return false;
        CScreenVars* vars = screenProcess->GetVars();
        ITimer* timer = iSystem->GetITimer();
        const float frameTime = timer->GetFrameTime();
        const int whiteTexture = m_TexMan->m_Text_White->GetTextureID();
        if (whiteTexture <= 0)
            return false;
        if (!m_cryvisionCVar && iConsole)
            m_cryvisionCVar = iConsole->GetCVar("r_Cryvision");
        vars->m_iNightVisionActive = m_cryvisionCVar ? m_cryvisionCVar->GetIVal() : 0;

        const auto queueSolidOverlay = [&](float red, float green, float blue, float alpha,
                                           uint32_t blendState)
        {
            return m_frameRenderer->QueuePanelImage(whiteTexture, 0.0f, 0.0f,
                800.0f, 600.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f,
                red, green, blue, alpha, 800.0f, 600.0f, blendState);
        };

        // Match CREScreenProcess::mfDrawLowSpec's cubic brightness curve and
        // one-over-time-scale countdown before drawing its white flash.
        if (vars->m_bFlashBangActive)
        {
            const float time = vars->m_fFlashBangTimeOut;
            const float time2 = time * time;
            const float brightness = time2 - (7.0f / 12.0f) * time2 * time +
                (7.0f / 12.0f) * time;
            if (!queueSolidOverlay(1.0f, 1.0f, 1.0f, brightness,
                                   GS_BLSRC_ONE | GS_BLDST_ONE))
                return false;
            const float timeScale = vars->m_fFlashBangTimeScale != 0.0f ?
                1.0f / vars->m_fFlashBangTimeScale : 1.0f;
            vars->m_fFlashBangTimeOut -= timeScale * frameTime;
            if (vars->m_fFlashBangTimeOut <= 0.01f)
            {
                vars->m_bFlashBangActive = false;
                vars->m_fFlashBangTimeOut = 1.0f;
            }
        }

        if (vars->m_bFadeActive)
        {
            const float fadeSign = vars->m_fFadeTime < 0.0f ? -1.0f : 1.0f;
            if (vars->m_fFadeCurrPreTime >= vars->m_fFadePreTime)
            {
                vars->m_fFadeCurrTime -= frameTime;
                if (vars->m_fFadeCurrTime < 0.0f)
                {
                    vars->m_fFadeCurrTime = 0.0f;
                    vars->m_fFadePreTime = 0.0f;
                    vars->m_bFadeActive = false;
                    vars->m_fFadeCurrPreTime = 0.0f;
                }
            }
            float fadeStep = 0.0f;
            if (fabsf(vars->m_fFadeTime) > 0.000001f)
                fadeStep = vars->m_fFadeCurrTime / fabsf(vars->m_fFadeTime);
            if (fadeSign > 0.0f)
                fadeStep = 1.0f - fadeStep;
            vars->m_fFadeCurrPreTime += 1.0f;
            if (!m_fadeAmountCVar && iConsole)
                m_fadeAmountCVar = iConsole->GetCVar("r_FadeAmount");
            if (m_fadeAmountCVar)
                m_fadeAmountCVar->Set(1.0f - fadeStep);
            if (!vars->m_bFadeActive)
            {
                if (m_fadeAmountCVar)
                    m_fadeAmountCVar->Set(1.0f);
            }
            if (!queueSolidOverlay(vars->m_pFadeColor.r, vars->m_pFadeColor.g,
                                   vars->m_pFadeColor.b, fadeStep,
                                   GS_BLSRC_SRCALPHA | GS_BLDST_ONEMINUSSRCALPHA))
                return false;
        }
        return true;
    }

    bool PrepareStockLeafItem(CREOcLeaf* element, CCObject* object)
    {
        if (!element || !object || !element->m_pBuffer || !element->m_pChunk)
            return false;

        // CREOcLeaf::mfPrepare performs this object-space bounds test before
        // setting up GL stream pointers. Vulkan also clips fragments in its
        // scene shaders; the coarse test avoids queuing fully rejected leaves.
        if (m_RP.m_ClipPlaneEnabled && CRenderer::CV_r_cullbyclipplanes &&
            element->mfCullByClipPlane(object))
        {
            m_RP.m_pRE = nullptr;
            m_RP.m_RendNumIndices = 0;
            m_RP.m_RendNumVerts = 0;
            return false;
        }

        CLeafBuffer* leaf = element->m_pBuffer;
        if (element->m_Flags & FCEF_MODIF_MASK)
        {
            element->m_Flags &= ~FCEF_MODIF_MASK;
            if (leaf->m_pSecVertBuffer &&
                leaf->m_pSecVertBuffer->m_VS[VSF_GENERAL].m_VData)
                leaf->UpdateVidVertices(leaf->m_pSecVertBuffer->m_VS[VSF_GENERAL].m_VData,
                                        leaf->m_SecVertCount);
        }

        const CMatInfo* chunk = element->m_pChunk;
        CLeafBuffer* vertexContainer = leaf->GetVertexContainer();
        if (!vertexContainer)
            return false;
        m_RP.m_pRE = element;
        if (vertexContainer->m_nPrimetiveType == R_PRIMV_TRIANGLE_STRIP)
        {
            m_RP.m_FirstVertex = 0;
            m_RP.m_FirstIndex = 0;
        }
        else
        {
            m_RP.m_FirstVertex = chunk->nFirstVertId;
            m_RP.m_FirstIndex = chunk->nFirstIndexId;
            if (vertexContainer->m_pVertexBuffer)
                m_RP.m_BaseVertex = vertexContainer->m_pVertexBuffer->m_fence;
        }
        m_RP.m_RendNumIndices = chunk->nNumIndices;
        m_RP.m_RendNumVerts = chunk->nNumVerts;
        return true;
    }

    void PrepareStockSkinnedObject(CCObject* object, SShader* shader,
                                   CRendElement* element)
    {
        if (!object || !object->m_pCharInstance || CRenderer::CV_r_character_nodeform ||
            !shader || !element || element->mfGetType() != eDATA_OcLeaf)
            return;
        CREOcLeaf* leafElement = static_cast<CREOcLeaf*>(element);
        if (!leafElement->m_pBuffer || !leafElement->m_pBuffer->m_pMats)
            return;

        int vertexFormat = -1;
        int updateFlags = 0;
        for (int materialIndex = 0; materialIndex < leafElement->m_pBuffer->m_pMats->Count(); ++materialIndex)
        {
            CMatInfo* material = leafElement->m_pBuffer->m_pMats->Get(materialIndex);
            if (!material || !material->pRE || !material->shaderItem.m_pShader)
                continue;
            SShader* materialShader = static_cast<SShader*>(
                material->shaderItem.m_pShader->GetTemplate(-1));
            if (!materialShader || (materialShader->m_Flags3 & EF3_NODRAW))
                continue;
            vertexFormat = vertexFormat < 0 ? materialShader->m_VertexFormatId :
                m_RP.m_VFormatsMerge[vertexFormat][materialShader->m_VertexFormatId];
            if (shader->m_Flags & EF_NEEDTANGENTS)
                updateFlags |= SHPF_TANGENTS;
            if (material->shaderItem.m_pShaderResources &&
                material->shaderItem.m_pShaderResources->m_bNeedNormals)
                updateFlags |= SHPF_NORMALS;
        }
        if (vertexFormat < 0 || !leafElement->mfCheckUpdate(
                vertexFormat, updateFlags | FHF_FORANIM))
            return;

        CLeafBuffer* leafBuffer = leafElement->m_pBuffer->GetVertexContainer();
        if (!leafBuffer || !leafBuffer->m_pVertexBuffer)
            return;
        bool forceUpdate = leafBuffer->m_UpdateFrame == GetFrameID();
        if (leafBuffer->m_pVertexBuffer->m_bFenceSet)
            forceUpdate = true;
        object->m_pCharInstance->ProcessSkinning(
            Vec3(zero), object->m_Matrix, object->m_nTemplId, object->m_nLod, forceUpdate);
    }

    bool DrawStockShadowVolume(CRETriMeshShadow* shadow, SShader* shader)
    {
        if (!shadow || !m_frameRenderer || !shadow->mfCheckUpdate(
                shader ? shader->m_VertexFormatId : 1, 0))
        {
            if (shadow) shadow->m_nCurrInst = -1;
            return false;
        }
        const int instance = shadow->m_nCurrInst;
        if (instance < 0 || instance >= MAX_SV_INSTANCES)
        {
            shadow->m_nCurrInst = -1;
            return false;
        }
        CLeafBuffer* leaf = shadow->m_arrLBuffers[instance].pVB;
        CVertexBuffer* vertices = leaf ? leaf->m_pVertexBuffer : nullptr;
        if (!vertices || !vertices->m_VS[VSF_GENERAL].m_VData ||
            !leaf->m_Indices.m_VData || vertices->m_NumVerts <= 0)
        {
            shadow->m_nCurrInst = -1;
            return false;
        }
        const int availableIndices = shadow->m_nRendIndices > 0 ?
            shadow->m_nRendIndices : leaf->m_Indices.m_nItems;
        if (availableIndices <= 0 || availableIndices > leaf->m_Indices.m_nItems)
        {
            shadow->m_nCurrInst = -1;
            return false;
        }

        float modelView[16] = {};
        float identity[16] = {};
        identity[0] = identity[5] = identity[10] = identity[15] = 1.0f;
        if (m_RP.m_pCurObject && (m_RP.m_pCurObject->m_ObjFlags & FOB_TRANS_MASK))
            mathMatrixMultiply(modelView, m_CameraMatrix.GetData(),
                               m_RP.m_pCurObject->m_Matrix.GetData(), g_CpuFlags);
        else
            memcpy(modelView, m_CameraMatrix.GetData(), sizeof(modelView));

        const uint32_t stencilState =
            STENC_FUNC(FSS_STENCFUNC_ALWAYS) |
            STENCOP_FAIL(FSS_STENCOP_KEEP) |
            STENCOP_ZFAIL(FSS_STENCOP_DECR_WRAP) |
            STENCOP_PASS(FSS_STENCOP_KEEP) |
            STENC_CCW_FUNC(FSS_STENCFUNC_ALWAYS) |
            STENCOP_CCW_FAIL(FSS_STENCOP_KEEP) |
            STENCOP_CCW_ZFAIL(FSS_STENCOP_INCR_WRAP) |
            STENCOP_CCW_PASS(FSS_STENCOP_KEEP) |
            FSS_STENCIL_TWOSIDED;
        const int lightLevel = SRendItem::m_RecurseLevel;
        if (lightLevel >= 0 && lightLevel < 8)
        {
            const int lightCount = m_RP.m_DLights[lightLevel].Num();
            for (int lightIndex = 0; lightIndex < lightCount && lightIndex < 32; ++lightIndex)
            {
                if (!(m_RP.m_DynLMask & (1u << lightIndex)))
                    continue;
                CDLight* light = m_RP.m_DLights[lightLevel][lightIndex];
                if (light && light->m_sWidth && light->m_sHeight)
                    SetScissor(light->m_sX, light->m_sY, light->m_sWidth, light->m_sHeight);
                break;
            }
        }
        const bool queued = m_frameRenderer->QueueStockClientIndexedDraw(
            vertices->m_VS[VSF_GENERAL].m_VData,
            static_cast<uint32_t>(vertices->m_NumVerts),
            static_cast<const uint16_t*>(leaf->m_Indices.m_VData),
            static_cast<uint32_t>(availableIndices), vertices->m_vertexformat, 0,
            GS_NOCOLMASK | GS_STENCIL, R_CULL_NONE, 0, 0,
            eCO_DISABLE, eCO_DISABLE, eCO_DISABLE, eCO_DISABLE,
            DEF_TEXARG0, DEF_TEXARG0, 0xffffffffu,
            DEF_TEXARG0, DEF_TEXARG0, 0xffffffffu,
            stencilState, 0, 0xffffffffu, modelView, identity, identity);
        shadow->m_nCurrInst = -1;
        return queued;
    }

    CryVR::VulkanFrameRenderer* m_frameRenderer = nullptr;
    CVulkanTexMan* m_vulkanTexMan = nullptr;
    SShaderPass* m_activePass = nullptr;
    EShaderPassType m_activeHardwarePassType = eSHP_MAX;
    bool m_activePolygonOffset = false;
    float m_activePolygonOffsetFactor = -1.0f;
    float m_activePolygonOffsetUnits = -4.0f;
    ICVar* m_polygonOffsetFactorCVar = nullptr;
    ICVar* m_polygonOffsetUnitsCVar = nullptr;
    ICVar* m_clipPlanesCVar = nullptr;
    ICVar* m_cullGeometryForLightsCVar = nullptr;
    ICVar* m_beamsCVar = nullptr;
    ICVar* m_flaresEnabledCVar = nullptr;
    ICVar* m_flareSizeCVar = nullptr;
    ICVar* m_flaresCVar = nullptr;
    ICVar* m_coronasCVar = nullptr;
    ICVar* m_coronaSizeScaleCVar = nullptr;
    ICVar* m_coronaColorScaleCVar = nullptr;
    ICVar* m_coronaFadeCVar = nullptr;
    ICVar* m_sunStyleCoronasCVar = nullptr;
    ICVar* m_checkSunVisCVar = nullptr;
    ICVar* m_cryvisionCVar = nullptr;
    ICVar* m_fadeAmountCVar = nullptr;
    SRenderShaderResources* m_activeResources = nullptr;
    int m_activeCull = R_CULL_BACK;
    int m_currentCullMode = R_CULL_BACK;
    SEfState* m_activeStateShaderState = nullptr;
    int m_polygonMode = R_SOLID_MODE;
    float m_materialColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    std::vector<struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F> m_fontVertices;
    std::vector<struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F> m_worldDynamicVertices;
    int m_fontTextureId = -1;
    bool m_fontRenderingState = false;
    bool m_frameOpen = false;
    bool m_auditFrameActive = false;
    int m_auditCurrentList = -1;
    int m_auditCurrentItem = -1;
    bool m_screenSpaceMode = false;
    std::array<float, 16> m_screenMatrix{{1.0f, 0.0f, 0.0f, 0.0f,
                                          0.0f, 1.0f, 0.0f, 0.0f,
                                          0.0f, 0.0f, 1.0f, 0.0f,
                                          0.0f, 0.0f, 0.0f, 1.0f}};
    std::vector<std::array<float, 16>> m_screenMatrixStack;
    std::vector<ScreenMatrixState> m_screenMatrixModes;
    int m_currentTextureId = 0;
    std::map<int, int> m_textureWrapOverrides;
    bool m_forceCurrentTextureForClientDraw = false;
    float m_screenSpaceWidth = 800.0f;
    float m_screenSpaceHeight = 600.0f;
    int m_stageTextureIds[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    int m_stageColorOps[8] = { eCO_MODULATE, eCO_MODULATE, eCO_MODULATE, eCO_MODULATE,
                               eCO_MODULATE, eCO_MODULATE, eCO_MODULATE, eCO_MODULATE };
    int m_stageAlphaOps[8] = { eCO_MODULATE, eCO_MODULATE, eCO_MODULATE, eCO_MODULATE,
                               eCO_MODULATE, eCO_MODULATE, eCO_MODULATE, eCO_MODULATE };
    uint32_t m_stageColorArgs[8] = { DEF_TEXARG0, DEF_TEXARG1, DEF_TEXARG1, DEF_TEXARG1,
                                     DEF_TEXARG1, DEF_TEXARG1, DEF_TEXARG1, DEF_TEXARG1 };
    uint32_t m_stageAlphaArgs[8] = { DEF_TEXARG0, DEF_TEXARG1, DEF_TEXARG1, DEF_TEXARG1,
                                     DEF_TEXARG1, DEF_TEXARG1, DEF_TEXARG1, DEF_TEXARG1 };
    uint8_t m_stageStateOverrides[8] = {};
    bool m_fogEnabled = false;
    float m_fogDensity = 0.0f;
    float m_fogStart = 0.0f;
    float m_fogEnd = 1.0f;
    float m_fogColor[3] = {};
    int m_fogMode = 0;
};
}

extern "C" DLL_EXPORT IRenderer* PackageRenderConstructor(int, char**, SCryRenderInterface* renderInterface)
{
    if (!renderInterface || !renderInterface->pVulkanFrameRenderer)
        return nullptr;
    iLog = renderInterface->ipLog;
    iConsole = renderInterface->ipConsole;
    iTimer = renderInterface->ipTimer;
    iSystem = renderInterface->ipSystem;
    pIPhysicalWorld = renderInterface->pIPhysicalWorld;
    pTest_int = renderInterface->ipTest_int;
    return new CVulkanRenderer(renderInterface->pVulkanFrameRenderer);
}
