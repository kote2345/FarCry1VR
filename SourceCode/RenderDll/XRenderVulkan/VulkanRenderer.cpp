#include "RenderPCH.h"
#include "../XRenderNULL/NULL_Renderer.h"
#include "VulkanFrameRenderer.h"
#include "VulkanRenderQuality.h"
#include "VulkanVertexFormat.h"
#include "VulkanTextureDecode.h"
#include "../../CryCommon/CryHeaders.h"
#include "../../CryCommon/ICryAnimation.h"
#include "../../CryCommon/IStatObj.h"
#include <IEntityRenderState.h>
#include "I3DEngine.h"
#include "../../CryCommon/CREOcLeaf.h"
#include "../Common/RendElements/CREBeam.h"
#include "../../CryCommon/CREPolyMesh.h"
#include "../Common/RendElements/CREScreenCommon.h"
#include "../Common/RendElements/CREFlares.h"
#include "../../CryCommon/CRETriMeshShadow.h"
#include "../../CryCommon/LeafBuffer.h"
#include "../../Cry3DEngine/StatObj.h"
#include "../../Cry3DEngine/terrain.h"
#include "../../Cry3DEngine/ObjMan.h"
#include "../Common/Shadow_Renderer.h"
#include "../Common/NvTriStrip/NvTriStrip.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
// Shader and CG parameter identifiers are ASCII. Keep these hot-path tests
// inline instead of calling libc strcasecmp for every draw/material pass.
inline int FastAsciiCaseCompare(const char* left, const char* right)
{
    if (!left || !right) return left == right ? 0 : (left ? 1 : -1);
    for (;; ++left, ++right)
    {
        unsigned char a = static_cast<unsigned char>(*left);
        unsigned char b = static_cast<unsigned char>(*right);
        if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b + ('a' - 'A'));
        if (a != b) return a < b ? -1 : 1;
        if (!a) return 0;
    }
}

inline int FastAsciiCaseCompareN(const char* left, const char* right, size_t count)
{
    if (!left || !right) return left == right ? 0 : (left ? 1 : -1);
    for (size_t i = 0; i < count; ++i, ++left, ++right)
    {
        unsigned char a = static_cast<unsigned char>(*left);
        unsigned char b = static_cast<unsigned char>(*right);
        if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b + ('a' - 'A'));
        if (a != b) return a < b ? -1 : 1;
        if (!a) return 0;
    }
    return 0;
}

// The comparison build keeps sunlight and ambient/baked illumination.
// Lightmap flags alone must not re-enable point/projector light passes.
constexpr bool kVulkanDynamicLightingEnabled = false;
bool ShouldRenderVulkanLight(const CDLight* light)
{
    if (!light) return false;
    if (kVulkanDynamicLightingEnabled) return true;
    const uint32 allowed = CryVR::kVulkanBasicRenderTest ?
        (DLF_SUN | DLF_DIRECTIONAL) :
        (DLF_SUN | DLF_DIRECTIONAL | DLF_LM | DLF_LMDOT3 | DLF_LMOCCL);
    return !(light->m_Flags & DLF_TEMP) && (light->m_Flags & allowed);
}

float g_vulkanPolygonOffsetFactor = -1.0f;
float g_vulkanPolygonOffsetUnits = -4.0f;
int g_vulkanClipPlanes = 1;
int g_vulkanAudit = 0;

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
    const auto encodeSignedNormal = [](float value)
    {
        const float normalized = value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value);
        return static_cast<uint8_t>(std::lround((normalized * 0.5f + 0.5f) * 255.0f));
    };
    if (format == eTF_SIGNED_HILO8 || format == eTF_V8U8 ||
        format == eTF_SIGNED_HILO16 || format == eTF_V16U16)
    {
        const bool use16Bit = format == eTF_SIGNED_HILO16 || format == eTF_V16U16;
        for (size_t i = 0; i < pixelCount; ++i)
        {
            float x, y;
            if (use16Bit)
            {
                int16_t pair[2]{};
                std::memcpy(pair, pixels + i * sizeof(pair), sizeof(pair));
                x = std::max(-1.0f, static_cast<float>(pair[0]) / 32767.0f);
                y = std::max(-1.0f, static_cast<float>(pair[1]) / 32767.0f);
            }
            else
            {
                const int8_t* pair = reinterpret_cast<const int8_t*>(pixels + i * 2);
                x = std::max(-1.0f, static_cast<float>(pair[0]) / 127.0f);
                y = std::max(-1.0f, static_cast<float>(pair[1]) / 127.0f);
            }
            const float z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
            rgba[i * 4 + 0] = encodeSignedNormal(x);
            rgba[i * 4 + 1] = encodeSignedNormal(y);
            rgba[i * 4 + 2] = encodeSignedNormal(z);
            rgba[i * 4 + 3] = 255;
        }
        return true;
    }
    if (format == eTF_SIGNED_RGB8)
    {
        const int8_t* source = reinterpret_cast<const int8_t*>(pixels);
        for (size_t i = 0; i < pixelCount; ++i)
        {
            rgba[i * 4 + 0] = encodeSignedNormal(std::max(-1.0f,
                static_cast<float>(source[i * 3 + 0]) / 127.0f));
            rgba[i * 4 + 1] = encodeSignedNormal(std::max(-1.0f,
                static_cast<float>(source[i * 3 + 1]) / 127.0f));
            rgba[i * 4 + 2] = encodeSignedNormal(std::max(-1.0f,
                static_cast<float>(source[i * 3 + 2]) / 127.0f));
            rgba[i * 4 + 3] = 255;
        }
        return true;
    }
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
            // SOcclusionMapTexel stores B/G/R/A in successive low-to-high
            // nibbles. GLTextures uploads those bytes as GL_BGRA, so the
            // texture's sampled RGBA order is R/G/B/A. Expand each nibble
            // with *17 so R8G8B8A8 sampling matches normalized GL_RGBA4.
            r = static_cast<uint8_t>(((packed >> 8) & 0x0fu) * 17u);
            g = static_cast<uint8_t>(((packed >> 4) & 0x0fu) * 17u);
            b = static_cast<uint8_t>((packed & 0x0fu) * 17u);
            a = static_cast<uint8_t>(((packed >> 12) & 0x0fu) * 17u);
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

size_t VulkanTexturePixelSize(ETEX_Format format, ETexType textureType)
{
    if (textureType == eTT_DSDTBump)
        return (format == eTF_DSDT_MAG || format == eTF_8888 || format == eTF_RGBA) ? 4u : 0u;
    switch (format)
    {
    case eTF_Index:
    case eTF_8888:
    case eTF_RGBA:
        return 4u;
    case eTF_SIGNED_HILO16:
    case eTF_V16U16:
        return 4u;
    case eTF_0888:
    case eTF_RGB8:
    case eTF_SIGNED_RGB8:
        return 3u;
    case eTF_8000:
        return 1u;
    case eTF_0565:
    case eTF_0555:
    case eTF_1555:
    case eTF_4444:
    case eTF_SIGNED_HILO8:
    case eTF_V8U8:
    case eTF_0088:
        return 2u;
    default:
        return 0u;
    }
}

bool DecodeVulkanUncompressedTextureMip(ETEX_Format format, ETexType textureType,
                                        const byte* pixels, uint32_t width,
                                        uint32_t height, std::vector<uint8_t>& rgba)
{
    if (!pixels || !width || !height)
        return false;
    const size_t pixelCount = static_cast<size_t>(width) * height;
    rgba.resize(pixelCount * 4);
    if (textureType == eTT_DSDTBump)
    {
        // CTexMan::LoadFromImage changes DDS_DSDT to DDS_RGBA8 while retaining
        // FIM_DSDT. GenerateNormalMap preserves those packed signed bytes,
        // so UploadImage passes eTF_8888 with eTT_DSDTBump. OpenGL chooses
        // GL_DSDT_MAG_NV from the texture type, not the image format.
        if (format != eTF_DSDT_MAG && format != eTF_8888 && format != eTF_RGBA)
            return false;
        for (size_t i = 0; i < pixelCount; ++i)
        {
            for (size_t channel = 0; channel < 3; ++channel)
            {
                const int signedValue = static_cast<int>(
                    reinterpret_cast<const signed char*>(pixels)[i * 4 + channel]);
                rgba[i * 4 + channel] = static_cast<uint8_t>(signedValue + 128);
            }
            rgba[i * 4 + 3] = 255;
        }
        return true;
    }
    if (format == eTF_0565 || format == eTF_0555 || format == eTF_1555 ||
        format == eTF_4444 || format == eTF_SIGNED_HILO8 || format == eTF_V8U8 ||
        format == eTF_SIGNED_HILO16 || format == eTF_V16U16 ||
        format == eTF_SIGNED_RGB8 || format == eTF_RGB8 || format == eTF_0088)
        return DecodeSpecialTexture(format, pixels, pixelCount, rgba);

    for (size_t i = 0; i < pixelCount; ++i)
    {
        switch (format)
        {
        case eTF_Index:
        case eTF_8888:
            // Indexed images have already been expanded to BGRA by TexMan.
            rgba[i * 4 + 0] = pixels[i * 4 + 2];
            rgba[i * 4 + 1] = pixels[i * 4 + 1];
            rgba[i * 4 + 2] = pixels[i * 4 + 0];
            rgba[i * 4 + 3] = pixels[i * 4 + 3];
            break;
        case eTF_RGBA:
            // XRenderOGL uploads this engine format with GL_BGRA_EXT. Match
            // the sampled Vulkan RGBA order instead of copying the bytes.
            rgba[i * 4 + 0] = pixels[i * 4 + 2];
            rgba[i * 4 + 1] = pixels[i * 4 + 1];
            rgba[i * 4 + 2] = pixels[i * 4 + 0];
            rgba[i * 4 + 3] = pixels[i * 4 + 3];
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
            return false;
        }
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

    void RemoveFromHash(int id, STexPic* texture) override
    {
        // Called when STexPic is actually released, after its reference
        // count has reached zero. A RemoveTexture request alone is not proof
        // that shared particle/material images can be destroyed.
        if (m_callbacks.releaseMirroredTexture)
            m_callbacks.releaseMirroredTexture(m_callbacks.drawUserData, id);
        CNULLTexMan::RemoveFromHash(id, texture);
    }

    void SetFilterOverride(int filterMode) { m_filterOverride = filterMode; }
    void ClearFilterOverride() { m_filterOverride = -1; }
    bool UpdateGlobalFilter(CryVR::VulkanFrameRenderer* renderer, int mode, float anisotropy)
    {
        m_defaultFilter = mode;
        bool result = true;
        for (int index = 0; index < m_Textures.Num(); ++index)
        {
            STexPic* texture = m_Textures[index];
            if (texture && texture->m_bBusy && !(texture->m_Flags & FT_NOMIPS))
                result = renderer->SetLegacyTextureFilter(texture->m_Bind, mode, anisotropy) && result;
        }
        return result;
    }

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
            default:
                // STexPic::SetFilter uses GL_LINEAR for an unqualified
                // FT_NOMIPS image regardless of the global mip/aniso setting.
                filterMode = (flags & FT_NOMIPS) ? eVTF_Linear :
                    (anisotropy > 1.0f ? eVTF_Anisotropic : m_defaultFilter);
                break;
            }
        }

        const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
        std::vector<unsigned char> rgba(pixelCount * 4);
        std::vector<std::vector<uint8_t>> decodedMipLevels;
        // CTexMan expands mipmapped 24-bit payloads to BGRA before it calls
        // the renderer, while a single-level eTF_0888 upload stays packed BGR.
        const ETEX_Format decodedFormat = format == eTF_0888 &&
            (flags & FT_HASMIPS) ? eTF_8888 : format;
        if (format == eTF_DXT1 || format == eTF_DXT3 || format == eTF_DXT5)
        {
            const bool dxt1 = format == eTF_DXT1;
            const bool dxt3 = format == eTF_DXT3;
            const bool dxt5 = format == eTF_DXT5;
            const size_t blockBytes = dxt1 ? 8u : 16u;
            int sourceMipCount = (flags & FT_NOMIPS) ? 1 : texture->m_nMips;
            if (sourceMipCount < 1)
                sourceMipCount = 1;
            int maxMipCount = 1;
            for (int mipWidth = width, mipHeight = height;
                 mipWidth > 1 || mipHeight > 1;
                 mipWidth = mipWidth > 1 ? mipWidth / 2 : 1,
                 mipHeight = mipHeight > 1 ? mipHeight / 2 : 1)
                ++maxMipCount;
            if (sourceMipCount > maxMipCount)
                sourceMipCount = maxMipCount;
            if (sourceMipCount > 1 && dxtSize <= 0)
                sourceMipCount = 1;
            size_t sourceOffset = 0;
            int mipWidth = width;
            int mipHeight = height;
            for (int level = 0; level < sourceMipCount; ++level)
            {
                const size_t levelBytes =
                    static_cast<size_t>((mipWidth + 3) / 4) *
                    static_cast<size_t>((mipHeight + 3) / 4) * blockBytes;
                if (dxtSize > 0 && sourceOffset + levelBytes > static_cast<size_t>(dxtSize))
                {
                    if (level == 0)
                        return texture;
                    break;
                }
                std::vector<uint8_t> decoded(static_cast<size_t>(mipWidth) * mipHeight * 4);
                if (!DecodeVulkanDxtBaseLevel(pixels + sourceOffset, mipWidth, mipHeight,
                                              dxt1, dxt3, dxt5, decoded))
                    return texture;
                if (level == 0)
                    rgba = decoded;
                decodedMipLevels.push_back(std::move(decoded));
                sourceOffset += levelBytes;
                mipWidth = mipWidth > 1 ? mipWidth / 2 : 1;
                mipHeight = mipHeight > 1 ? mipHeight / 2 : 1;
            }
        }
        else if (!DecodeVulkanUncompressedTextureMip(decodedFormat, textureType,
                                                     pixels, static_cast<uint32_t>(width),
                                                     static_cast<uint32_t>(height), rgba))
            return texture;

        if (decodedMipLevels.empty() && textureType != eTT_Cubemap &&
            format != eTF_DXT1 && format != eTF_DXT3 && format != eTF_DXT5)
        {
            int sourceMipCount = (flags & FT_NOMIPS) ? 1 : texture->m_nMips;
            if (sourceMipCount < 1)
                sourceMipCount = 1;
            int maxMipCount = 1;
            for (int mipWidth = width, mipHeight = height;
                 mipWidth > 1 || mipHeight > 1;
                 mipWidth = mipWidth > 1 ? mipWidth / 2 : 1,
                 mipHeight = mipHeight > 1 ? mipHeight / 2 : 1)
                ++maxMipCount;
            if (sourceMipCount > maxMipCount)
                sourceMipCount = maxMipCount;
            const size_t bytesPerPixel = VulkanTexturePixelSize(decodedFormat, textureType);
            if (sourceMipCount > 1 && bytesPerPixel)
            {
                decodedMipLevels.reserve(static_cast<size_t>(sourceMipCount));
                decodedMipLevels.push_back(rgba);
                size_t sourceOffset = pixelCount * bytesPerPixel;
                int mipWidth = width > 1 ? width / 2 : 1;
                int mipHeight = height > 1 ? height / 2 : 1;
                bool allSourceMipsDecoded = true;
                for (int level = 1; level < sourceMipCount; ++level)
                {
                    std::vector<uint8_t> decoded;
                    if (!DecodeVulkanUncompressedTextureMip(
                            decodedFormat, textureType, pixels + sourceOffset,
                            static_cast<uint32_t>(mipWidth),
                            static_cast<uint32_t>(mipHeight), decoded))
                    {
                        allSourceMipsDecoded = false;
                        break;
                    }
                    decodedMipLevels.push_back(std::move(decoded));
                    sourceOffset += static_cast<size_t>(mipWidth) * mipHeight * bytesPerPixel;
                    mipWidth = mipWidth > 1 ? mipWidth / 2 : 1;
                    mipHeight = mipHeight > 1 ? mipHeight / 2 : 1;
                }
                if (!allSourceMipsDecoded)
                    decodedMipLevels.clear();
            }
        }

        // GL stores FT_FONT as GL_ALPHA8: sampled RGB is always white.
        // Preserve the incoming alpha atlas even when the source has four channels.
        if (flags & FT_FONT)
        {
            for (size_t i = 0; i < pixelCount; ++i)
                rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
            for (auto& mip : decodedMipLevels)
                for (size_t i = 0; i < mip.size(); i += 4)
                    mip[i] = mip[i + 1] = mip[i + 2] = 255;
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

        const bool clampU = (flags & FT_CLAMP) || (flags2 & FT2_UCLAMP);
        const bool clampV = (flags & FT_CLAMP) || (flags2 & FT2_VCLAMP);
        bool mirrored = false;
        if (decodedMipLevels.size() > 1 && m_callbacks.mirrorRgbaTextureMipChain)
        {
            std::vector<const unsigned char*> mipPointers;
            mipPointers.reserve(decodedMipLevels.size());
            for (const std::vector<uint8_t>& mip : decodedMipLevels)
                mipPointers.push_back(mip.data());
            mirrored = m_callbacks.mirrorRgbaTextureMipChain(
                m_callbacks.drawUserData, static_cast<int>(texture->m_Bind),
                static_cast<unsigned int>(width), static_cast<unsigned int>(height),
                mipPointers.data(), static_cast<unsigned int>(mipPointers.size()),
                clampU, clampV, filterMode, anisotropy);
        }
        else
            mirrored = m_callbacks.mirrorRgbaTexture(
                m_callbacks.drawUserData, static_cast<int>(texture->m_Bind),
                static_cast<unsigned int>(width), static_cast<unsigned int>(height),
                rgba.data(), clampU, clampV,
                ((flags & FT_DYNAMIC) != 0 || (flags & FT_FONT) != 0),
                (flags & FT_NOMIPS) != 0, filterMode, anisotropy);
        if (mirrored)
        {
            texture->m_Size = static_cast<int>(pixelCount * 4);
#if defined(__ANDROID__)
            // Bounded upload snapshots for the two reported materials. This
            // records the actual decoded image, not a guessed shader output.
            static bool rayImageCaptured = false, boneImageCaptured = false;
            const bool rayImage = (name && (strstr(name, "sun_rays_textureb") ||
                strstr(name, "sun_rays_textureB"))) ||
                strstr(texture->m_SourceName.c_str(), "sun_rays_textureb") ||
                strstr(texture->m_SourceName.c_str(), "sun_rays_textureB") ||
                strstr(texture->m_SearchName.c_str(), "sun_rays_textureb") ||
                strstr(texture->m_SearchName.c_str(), "sun_rays_textureB");
            const bool boneImage = (name && strstr(name, "skeleton.dds")) ||
                strstr(texture->m_SourceName.c_str(), "skeleton.dds") ||
                strstr(texture->m_SearchName.c_str(), "skeleton.dds");
            bool* captured = rayImage ? &rayImageCaptured : (boneImage ? &boneImageCaptured : nullptr);
            if (captured && !*captured && rgba.size() <= 1024u * 1024u)
            {
                FILE* probe = fopen(rayImage ? "/sdcard/FarCry/vulkan_ray_texture.rgba" :
                    "/sdcard/FarCry/vulkan_bone_texture.rgba", "wb");
                if (probe)
                {
                    const uint32_t header[] = {static_cast<uint32_t>(width),
                        static_cast<uint32_t>(height), static_cast<uint32_t>(decodedFormat)};
                    fwrite(header, sizeof(header), 1, probe);
                    fwrite(rgba.data(), rgba.size(), 1, probe);
                    fclose(probe);
                    *captured = true;
                }
            }
#endif
            if (g_vulkanAudit)
            {
                uint32_t checksum = 2166136261u;
                for (unsigned char value : rgba)
                    checksum = (checksum ^ value) * 16777619u;
                CryLogAlways("Vulkan audit TEXTURE_UPLOAD id=%u name=%s sourceFormat=%d decodedFormat=%d type=%d size=%dx%d mips=%u flags=0x%x rgba0=(%u,%u,%u,%u) checksum=0x%x",
                    texture->m_Bind, texture->m_SearchName.c_str(), static_cast<int>(format),
                    static_cast<int>(decodedFormat), static_cast<int>(textureType), width, height,
                    static_cast<unsigned int>(decodedMipLevels.size()), flags,
                    rgba[0], rgba[1], rgba[2], rgba[3], checksum);
            }
        }
        return texture;
    }

private:
    SVulkanBufferCallbacks m_callbacks;
    int m_defaultFilter = eVTF_Trilinear;
    std::map<std::string, CubeUpload> m_cubeUploads;
    int m_filterOverride = -1;
};

class CVulkanRenderer final : public CNULLRenderer
{
public:
    bool QueueGpuSkinning(CVertexBuffer* vertices, const SGpuSkinningData& mesh,
                          const Matrix44* bones, unsigned count) override {
        return m_frameRenderer && bones && m_frameRenderer->QueueGpuSkinning(vertices, mesh, bones->GetData(), count);
    }
    bool SupportsGpuSkinning() const override { return m_frameRenderer && m_frameRenderer->IsInitialized(); }
    bool QueueGpuMorph(const void* vertices, const SGpuSkinningData& mesh, float weight, float normalAmplify) override {
        return m_frameRenderer && m_frameRenderer->QueueGpuMorph(vertices, mesh, weight, normalAmplify);
    }
    bool QueueGpuSkinShadow(const void* identity, const void* source, const SGpuSkinningData& mesh,
        const SGpuSkinShadowData& topology, const Matrix44* bones, unsigned count, const Vec3& light, float extent, unsigned first = 0) override {
        return m_frameRenderer && m_frameRenderer->QueueGpuSkinShadow(identity,source,mesh,topology,bones->GetData(),count,&light.x,extent,first);
    }
    bool QueueGpuSkinReadback(const void* identity, const GpuSkinReadback& callback) override {
        return m_frameRenderer && m_frameRenderer->QueueGpuSkinReadback(identity,callback);
    }
    void ClearGpuSkinning(const void* vertices) override {
        if (m_frameRenderer) m_frameRenderer->ClearGpuSkinning(vertices);
    }
    bool QueueGpuSkinningRemap(CVertexBuffer* vertices, CVertexBuffer* source, const unsigned* map, unsigned count) override {
        return m_frameRenderer && m_frameRenderer->QueueGpuSkinningRemap(vertices, source, map, count);
    }
    void ReleaseBuffer(CVertexBuffer* vertices) override {
        ClearGpuSkinning(vertices);
        m_frameBufferRevisions.erase(vertices);
        InvalidateFrameBufferRevision(vertices);
        for (auto entry = m_frameNormalGeometry.begin(); entry != m_frameNormalGeometry.end();)
        {
            if (entry->first[0] == reinterpret_cast<uintptr_t>(vertices))
            {
                m_frameNormalGeometryBytes -= entry->second.size();
                entry = m_frameNormalGeometry.erase(entry);
            }
            else ++entry;
        }
        CNULLRenderer::ReleaseBuffer(vertices);
    }
    void UpdateBuffer(CVertexBuffer* vertices, const void* source, int count,
                      bool unlock, int offset, int type) override
    {
        bool changed = source && count > 0;
        if (vertices && !source && type >= 0 && type <= 3)
            for (int stream = 0; stream < VSF_NUM; ++stream)
                if ((type == 0 && stream == VSF_GENERAL) || (type & (1 << stream)))
                    changed = changed || !unlock || vertices->m_VS[stream].m_bLocked;
        if (vertices && changed)
        {
            const uint64_t revision = ++m_geometryRevision;
            m_frameBufferRevisions[vertices] = revision;
            CacheFrameBufferRevision(vertices, revision);
        }
        CNULLRenderer::UpdateBuffer(vertices, source, count, unlock, offset, type);
    }
    void CreateIndexBuffer(SVertexStream* stream, const void* source, int count) override
    {
        const uint64_t revision = ++m_geometryRevision;
        m_ownedIndexRevisions[stream] = revision;
        CacheOwnedIndexRevision(stream, revision);
        CNULLRenderer::CreateIndexBuffer(stream, source, count);
    }
    void UpdateIndexBuffer(SVertexStream* stream, const void* source, int count, bool unlock = true) override
    {
        if (!stream) return;
        if (source && count > 0 && stream->m_VData && count <= stream->m_nItems &&
            !stream->m_bLocked && !memcmp(stream->m_VData, source, size_t(count) * sizeof(uint16_t)))
            return;
        if ((source && count > 0) || (!source && (!unlock || stream->m_bLocked)))
        {
            const uint64_t revision = ++m_geometryRevision;
            m_ownedIndexRevisions[stream] = revision;
            CacheOwnedIndexRevision(stream, revision);
        }
        if (!source) stream->m_bLocked = !unlock;
        CNULLRenderer::UpdateIndexBuffer(stream, source, count, unlock);
    }
    void ReleaseIndexBuffer(SVertexStream* stream) override
    {
        m_ownedIndexRevisions.erase(stream);
        InvalidateOwnedIndexRevision(stream);
        CNULLRenderer::ReleaseIndexBuffer(stream);
    }
    explicit CVulkanRenderer(CryVR::VulkanFrameRenderer* frameRenderer)
        : m_frameRenderer(frameRenderer)
    {
        SetType(R_VULKAN_RENDERER);
        m_nFrameUpdateID = m_nFrameID;
        // CGLRenderer starts every frame with depth writes enabled. This
        // renderer derives from CNULLRenderer, whose constructor does not
        // initialize m_CurState, but Vulkan uses that cache to construct the
        // first immutable pipeline as well as to resolve inherited pass bits.
        m_CurState = GS_DEPTHWRITE;
        // GLRendPipeline::EF_PipelineInit starts texture-environment color at
        // opaque white. Keep the NULL-backed renderer's equivalent state in
        // sync before the first Vulkan draw.
        m_RP.m_CurGlobalColor.dcolor = 0xffffffffu;
        if (iConsole)
        {
            if (!iConsole->GetCVar("GL_TextureFilter"))
                iConsole->CreateVariable("GL_TextureFilter", "GL_LINEAR_MIPMAP_LINEAR", 0,
                    "Texture minification/magnification filter, using the stock OpenGL mode names.");
            if (!iConsole->GetCVar("GL_OffsetFactor"))
                iConsole->Register("GL_OffsetFactor", &g_vulkanPolygonOffsetFactor, -1.0f);
            if (!iConsole->GetCVar("GL_OffsetUnits"))
                iConsole->Register("GL_OffsetUnits", &g_vulkanPolygonOffsetUnits, -4.0f);
            m_polygonOffsetFactorCVar = iConsole->GetCVar("GL_OffsetFactor");
            m_polygonOffsetUnitsCVar = iConsole->GetCVar("GL_OffsetUnits");
            if (!iConsole->GetCVar("GL_ClipPlanes"))
                iConsole->Register("GL_ClipPlanes", &g_vulkanClipPlanes, 1);
            m_clipPlanesCVar = iConsole->GetCVar("GL_ClipPlanes");
            if (!iConsole->GetCVar("r_VulkanAudit"))
                iConsole->Register("r_VulkanAudit", &g_vulkanAudit, 0);
        }
        // Advertise the shader/material capabilities the Vulkan translation
        // actually consumes so CryEngine's shader parser selects its high-end
        // NV4X/PS3-era material descriptions. Depth maps have a Vulkan caster
        // pass and receiver comparison path, so expose RFT_DEPTHMAPS: CryEngine
        // uses it to retain DEPTHMAP material branches and build the matching
        // caster lists. Keep self-shadowing disabled; its separate OpenGL
        // semantics are not implemented. The Vulkan texture manager decodes
        // DXT itself, so preserve compressed DDS data for upload.
        m_Features = RFT_HW_NV4X | RFT_HW_VS | RFT_HW_PS20 | RFT_HW_PS30 |
            RFT_MULTITEXTURE | RFT_BUMP | RFT_ALLOWSECONDCOLOR |
            RFT_COMPRESSTEXTURE | RFT_DEPTHMAPS | RFT_RGBA;
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
        const float rgba[4] = { color.x, color.y, color.z, 1.0f };
        if (m_frameOpen && m_frameRenderer)
        {
            if (m_frameRenderer->QueueStockClear(true, false, false, rgba))
                m_bWasCleared = true;
            else m_frameRenderer->RequirePanelFallback();
        }
    }

    void ClearDepthBuffer() override
    {
        CNULLRenderer::ClearDepthBuffer();
        // Match the stock GL side effect as well as the ordered attachment clear.
        EF_SetState(GS_DEPTHWRITE);
        if (m_frameOpen && m_frameRenderer)
        {
            if (m_frameRenderer->QueueStockClearDepth()) m_bWasCleared = true;
            else m_frameRenderer->RequirePanelFallback();
        }
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

    uint LoadTexture(const char* filename, int* textureType = nullptr,
                     unsigned int defaultId = 0, bool compressToDisk = false,
                     bool warn = true) override
    {
        // Match GL_Renderer.cpp; CNULLRenderer always returns Textures/red.
        ITexPic* image = EF_LoadTexture(filename, FT_NOREMOVE, 0, eTT_Base,
            -1, -1, defaultId ? static_cast<int>(defaultId) : -1);
        return image && image->IsTextureLoaded() ? image->GetTextureID() : 0;
    }

    int LoadAnimatedTexture(const char* filename, const int count) override
    {
        if (!filename || !filename[0] || count < 1) return 0;
        for (int i = 0; i < m_LoadedAnimatedTextures.Count(); ++i)
        {
            AnimTexInfo* info = m_LoadedAnimatedTextures[i];
            if (!strcmp(info->sName, filename) && info->nFramesCount == count)
            {
                ++info->nRefCounter;
                return i + 1;
            }
        }
        std::string frameFormat(filename);
        int firstFrame = 0;
        if (frameFormat.find('%') == std::string::npos)
        {
            const size_t extension = frameFormat.find_last_of('.');
            const size_t slash = frameFormat.find_last_of("/\\");
            if (extension == std::string::npos ||
                (slash != std::string::npos && extension < slash)) return 0;
            size_t digits = extension;
            while (digits > 0 && frameFormat[digits - 1] >= '0' && frameFormat[digits - 1] <= '9') --digits;
            if (digits < extension)
            {
                firstFrame = atoi(frameFormat.c_str() + digits);
                char format[32];
                snprintf(format, sizeof(format), "%%0%ud", static_cast<unsigned int>(extension - digits));
                frameFormat.replace(digits, extension - digits, format);
            }
        }
        AnimTexInfo* info = new AnimTexInfo();
        info->pBindIds = new int[count]();
        info->nFramesCount = 0;
        snprintf(info->sName, sizeof(info->sName), "%s", filename);
        for (int frame = 0; frame < count; ++frame)
        {
            char name[1024];
            snprintf(name, sizeof(name), frameFormat.c_str(), firstFrame + frame);
            const int id = LoadTexture(name);
            if (!id) break;
            info->pBindIds[info->nFramesCount++] = id;
        }
        if (!info->nFramesCount)
        {
            delete[] info->pBindIds;
            delete info;
            return 0;
        }
        info->nRefCounter = 1;
        m_LoadedAnimatedTextures.Add(info);
        return m_LoadedAnimatedTextures.Count();
    }

    int GenerateAlphaGlowTexture(float k) override
    {
        // Same radial alpha as OpenGL's generated particle glow.
        const int size = 256;
        std::vector<byte> alpha(size * size);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                const float dx = static_cast<float>(x - size / 2);
                const float dy = static_cast<float>(y - size / 2);
                alpha[y * size + x] = static_cast<byte>(clamp_tpl(
                    k * 2.0f * (size / 2 - sqrtf(dx * dx + dy * dy)), 0.0f, 255.0f));
            }
        return DownLoadToVideoMemory(alpha.data(), size, size, eTF_8000,
            eTF_8000, false, false, FILTER_LINEAR, 0, nullptr, 0);
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

    void ProjectToScreen(float x, float y, float z,
                         float* screenX, float* screenY, float* screenZ) override
    {
        if (!screenX || !screenY || !screenZ) return;
        // Match GLHelper::gluProject followed by GL_Renderer::ProjectToScreen.
        // Visibility traversal runs before eye rendering and uses this logical
        // camera, not an individual XR eye projection or a zero NULL stub.
        const float point[4] = { x, y, z, 1.0f };
        float eye[4]{}, clip[4]{};
        const float* view = m_ViewMatrix.GetData();
        const float* projection = m_ProjMatrix.GetData();
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                eye[row] += view[column * 4 + row] * point[column];
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                clip[row] += projection[column * 4 + row] * eye[column];
        if (clip[3] == 0.0f) return;
        const float width = static_cast<float>(GetWidth());
        const float height = static_cast<float>(GetHeight());
        if (width <= 0.0f || height <= 0.0f) return;
        const float viewportWidth = m_VWidth > 0 ? static_cast<float>(m_VWidth) : width;
        const float viewportHeight = m_VHeight > 0 ? static_cast<float>(m_VHeight) : height;
        const float windowX = m_VX + (1.0f + clip[0] / clip[3]) * viewportWidth * 0.5f;
        const float windowY = m_VY + (1.0f + clip[1] / clip[3]) * viewportHeight * 0.5f;
        *screenX = windowX * 100.0f / width;
        *screenY = 100.0f - windowY * 100.0f / height;
        *screenZ = (1.0f + clip[2] / clip[3]) * 0.5f;
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
        m_FS.m_FogDensity = density;
        m_FS.m_FogStart = fogStart;
        m_FS.m_FogEnd = fogEnd;
        m_FS.m_nFogMode = fogMode;
        // CGLRenderer::SetFog only calls glFogi for these two public modes.
        // Any other value leaves GL's previously selected mode in effect
        // (initially GL_EXP), so retain the effective mode here as well.
        if (fogMode == R_FOGMODE_LINEAR || fogMode == R_FOGMODE_EXP2)
            m_fogMode = fogMode;
        for (int i = 0; i < 3; ++i)
            m_fogColor[i] = m_bHeatVision ? 0.0f : color[i];
        m_FS.m_FogColor = CFColor(m_fogColor[0], m_fogColor[1], m_fogColor[2], 1.0f);
        UpdateVulkanFog();
    }

    void SetFogColor(float* color) override
    {
        if (!color)
            return;
        for (int i = 0; i < 3; ++i)
            m_fogColor[i] = color[i];
        m_FS.m_FogColor = CFColor(color);
        UpdateVulkanFog();
    }

    bool EnableFog(bool enable) override
    {
        const bool previous = m_fogEnabled;
        m_fogEnabled = enable;
        m_FS.m_bEnable = enable;
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
        // CGLRenderer::SetMaterialColor delegates to EF_SetGlobalColor:
        // it selects TMU 0 and installs the texture-env constant/arguments.
        for (int channel = 0; channel < 4; ++channel)
            m_RP.m_CurGlobalColor.bcolor[channel] = static_cast<byte>(
                clamp_tpl(m_materialColor[channel], 0.0f, 1.0f) * 255.0f);
        SelectTMU(0);
        SetColorOp(255, 255, eCA_Texture | (eCA_Constant << 3),
                   eCA_Texture | (eCA_Constant << 3));
    }

    void ResetToDefault() override
    {
        SetScissor(0, 0, 0, 0);
        if (m_RP.m_ClipPlaneWasOverrided == 1)
        {
            m_RP.m_ClipPlaneWasOverrided = 0;
            m_RP.m_ClipPlaneEnabled = 1;
        }
        for (int stage = 0; stage < 8; ++stage)
        {
            SelectTMU(stage);
            SetLodBias(CRenderer::CV_r_maxtexlod_bias);
            SetColorOp(eCO_MODULATE, eCO_MODULATE, 255, 255);
            // GL disables texture targets above TMU 0, but retains bindings
            // and combiner arguments. The NULL backend has no target state.
            if (stage > 0) m_stageTextureIds[stage] = 0;
        }
        SelectTMU(0);
        if (m_TexMan) m_TexMan->m_nCurStages = 1;
        if (m_frameRenderer) m_frameRenderer->ResetStockTextureOperations();
        m_CurState = GS_DEPTHWRITE;
        m_currentCullMode = R_CULL_NONE;
        m_RP.m_eCull = static_cast<ECull>(-1);
        m_activePolygonOffset = false;
        SetMaterialColor(1.0f, 1.0f, 1.0f, 1.0f);
        m_RP.m_FlagsPerFlush &= ~(RBSI_ALPHAGEN | RBSI_RGBGEN);
        m_RP.m_PersFlags &= ~(RBPF_PS1NEEDSET | RBPF_TSNEEDSET | RBPF_VSNEEDSET |
            RBPF_PS1WASSET | RBPF_PS2WASSET | RBPF_TSWASSET | RBPF_VSWASSET |
            RBPF_WASWORLDSPACE);
        m_RP.m_FlagsModificators = 0;
        m_RP.m_CurrentVLights = 0;
        m_RP.m_LastVP = nullptr;
        m_RP.m_CurVP = nullptr;
        m_RP.m_CurPS = nullptr;
    }

    void UpdateStockGamma(float gamma, float brightness, float contrast)
    {
        gamma = clamp_tpl(gamma, 0.5f, 3.0f);
        if (m_fLastGamma == gamma && m_fLastBrightness == brightness && m_fLastContrast == contrast)
            return;
        for (int index = 0; index < 256; ++index)
            m_GammaTable[index] = static_cast<ushort>(clamp_tpl(static_cast<int>(
                (contrast + 0.5f) * cry_powf(static_cast<float>(index) / 255.0f, 1.0f / gamma) * 65535.0f +
                (brightness - 0.5f) * 32768.0f - contrast * 32768.0f + 16384.0f), 0, 65535));
        m_fLastGamma = gamma;
        m_fLastBrightness = brightness;
        m_fLastContrast = contrast;
        // GLSystem::SetDeviceGamma has no device-ramp operation on Android.
        // Keep the same display behavior while preserving the CPU gamma state.
    }

    bool SetGammaDelta(const float gamma) override
    {
        m_fDeltaGamma = gamma;
        UpdateStockGamma(CV_r_gamma + gamma, CV_r_brightness, CV_r_contrast);
        return true;
    }

    void BeginFrame() override
    {
        m_framePrograms.clear();
        m_lastClassifiedPass = nullptr;
        m_lastProgramInfo = nullptr;
        CNULLRenderer::BeginFrame();
        InitializeStockFogTextures();
        UpdateStockGamma(CV_r_gamma + m_fDeltaGamma, CV_r_brightness, CV_r_contrast);
        if (CV_r_reloadshaders)
        {
            m_cEF.mfReloadAllShaders(CV_r_reloadshaders);
            CV_r_reloadshaders = 0;
        }
        if (CV_r_PolygonMode != m_polygonMode)
            SetPolygonMode(CV_r_PolygonMode);
        if (m_frameRenderer && m_vulkanTexMan)
        {
            ICVar* filter = iConsole ? iConsole->GetCVar("GL_TextureFilter") : nullptr;
            const int anisotropy = clamp_tpl(CRenderer::CV_r_texture_anisotropic_level, 1,
                crymax(1, static_cast<int>(m_frameRenderer->GetStockMaxAnisotropy())));
            CRenderer::CV_r_texture_anisotropic_level = anisotropy;
            if (filter && anisotropy > 1 && FastAsciiCaseCompare(filter->GetString(), "GL_LINEAR_MIPMAP_LINEAR"))
                filter->Set("GL_LINEAR_MIPMAP_LINEAR");
            const char* name = filter ? filter->GetString() : "GL_LINEAR_MIPMAP_LINEAR";
            if (m_lastTextureFilter != name || m_lastTextureAnisotropy != anisotropy)
            {
                const char* names[] = { "GL_NEAREST", "GL_LINEAR", "GL_NEAREST_MIPMAP_NEAREST",
                    "GL_LINEAR_MIPMAP_NEAREST", "GL_NEAREST_MIPMAP_LINEAR", "GL_LINEAR_MIPMAP_LINEAR" };
                const int modes[] = { eVTF_NearestNoMips, eVTF_Linear, eVTF_Nearest,
                    eVTF_Bilinear, eVTF_NearestMipLinear, eVTF_Trilinear };
                for (int index = 0; index < 6; ++index)
                    if (!FastAsciiCaseCompare(name, names[index]))
                    {
                        if (m_vulkanTexMan->UpdateGlobalFilter(m_frameRenderer,
                                anisotropy > 1 ? eVTF_Anisotropic : modes[index], static_cast<float>(anisotropy)))
                        {
                            m_lastTextureFilter = name;
                            m_lastTextureAnisotropy = anisotropy;
                        }
                        break;
                    }
            }
        }
        // The NULL backend does not advance CryEngine's frame counters or
        // reset shader pipeline state. World visibility caches key off these
        // counters; keeping one ID across frames empties subsequent scenes.
        m_cEF.mfBeginFrame();
        ++m_nFrameID;
        ++m_nFrameUpdateID;
        m_nPolygons = 0;
        m_nShadowVolumePolys = 0;
        m_bWasCleared = false;
        m_RP.m_RealTime = iTimer ? iTimer->GetCurrTime() : 0.0f;
        // GL recycles the alternating temporary-mesh pool at frame entry.
        TArray<CRETempMesh*>* tempMeshes = &m_RP.m_TempMeshes[m_nFrameID & 1];
        for (int index = 0; index < tempMeshes->Num(); ++index)
        {
            CRETempMesh* mesh = tempMeshes->Get(index);
            if (!mesh) continue;
            if (mesh->m_VBuffer)
            {
                ReleaseBuffer(mesh->m_VBuffer);
                mesh->m_VBuffer = nullptr;
            }
            ReleaseIndexBuffer(&mesh->m_Inds);
        }
        tempMeshes->SetUse(0);
        m_RP.m_CurTempMeshes = tempMeshes;
        ResetToDefault();
        // CSystem owns the OpenXR frame lifetime. The renderer only records
        // native scene work into that already-acquired frame.
        m_frameOpen = m_frameRenderer != nullptr;
        if (m_frameRenderer)
        {
            m_frameRenderer->SetDiagnosticLogging(g_vulkanAudit != 0);
            m_frameRenderer->SetStockDepthRange(0.0f, 1.0f);
        }
    }

    void Draw2dImage(float x, float y, float width, float height, int textureId,
                     float s0, float t0, float s1, float t1, float angle,
                     float red, float green, float blue, float alpha, float) override
    {
        SelectTMU(0);
        if (textureId > 0) SetTexture(textureId);
        if (m_frameOpen && m_frameRenderer)
            m_frameRenderer->QueuePanelImage(textureId,
                ScaleCoordX(x), ScaleCoordY(y)-1.0f, ScaleCoordX(width)+1.0f, ScaleCoordY(height)+2.0f,
                s0, t0, s1, t1,
                angle, red, green, blue, alpha,
                static_cast<float>(GetWidth()), static_cast<float>(GetHeight()),
                CurrentPanelBlendState());
    }

    void DrawImage(float x, float y, float width, float height, int textureId,
                   float s0, float t0, float s1, float t1,
                   float red, float green, float blue, float alpha) override
    {
        if (!m_frameOpen || !m_frameRenderer) return;
        SelectTMU(0);
        if (textureId > 0) SetTexture(textureId);
        // Unlike Draw2dImage, GL's DrawImage uses the current 2D matrix and
        // unscaled caller coordinates. Keep both its transform and explicit UVs.
        struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F vertices[4] = {};
        const float positions[4][2] = {{x,y},{x+width,y},{x+width,y+height},{x,y+height}};
        const float uvs[4][2] = {{s0,1.0f-t0},{s1,1.0f-t0},{s1,1.0f-t1},{s0,1.0f-t1}};
        for (int vertex = 0; vertex < 4; ++vertex)
        {
            vertices[vertex].xyz = Vec3(positions[vertex][0], positions[vertex][1], 0);
            vertices[vertex].st[0] = uvs[vertex][0]; vertices[vertex].st[1] = uvs[vertex][1];
            const float color[4] = {red,green,blue,alpha};
            for (int channel = 0; channel < 4; ++channel)
                vertices[vertex].color.bcolor[channel] = static_cast<byte>(clamp_tpl(color[channel],0.0f,1.0f)*255.0f);
        }
        const uint16_t indices[6] = {0,1,2,0,2,3};
        m_frameRenderer->QueuePanelIndexedGeometry(textureId, vertices, 4, indices, 6,
            m_screenSpaceMode ? m_screenSpaceWidth : static_cast<float>(GetWidth()),
            m_screenSpaceMode ? m_screenSpaceHeight : static_cast<float>(GetHeight()),
            m_screenMatrix.data(), CurrentPanelBlendState());
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

    void SelectTMU(int stage) override
    {
        if (stage >= 0 && stage < 8)
            CTexMan::m_CurStage = stage;
    }

    void SetTexture(int textureId, ETexType textureType = eTT_Base) override
    {
        m_currentTextureId = textureId;
        const int stage = m_TexMan ? m_TexMan->GetCurTMU() : 0;
        if (stage >= 0 && stage < 8)
        {
            m_stageTextureIds[stage] = textureId;
            if (m_activePass) m_stageStateOverrides[stage] |= 32u;
            if (textureId > 0 && m_TexMan)
                m_TexMan->m_nCurStages = crymax(m_TexMan->m_nCurStages, stage + 1);
        }
        CNULLRenderer::SetTexture(textureId, textureType);
    }

    uint MakeSprite(float objectScale, int textureSize, float angle, IStatObj* statObject,
                    uchar*, uint) override
    {
        CStatObj* object = static_cast<CStatObj*>(statObject);
        CLeafBuffer* leaf = object ? object->GetLeafBuffer() : nullptr;
        const auto reportBake = [&](const char* status, int sourceId,
                                    const char* shaderName = "", const char* diffuseName = "") {
#if defined(__ANDROID__)
            static unsigned reports = 0;
            if (fabsf(angle - 90.0f) > 0.01f || reports >= 256) return;
            if (FILE* file = fopen("/sdcard/FarCry/vulkan_sprite_bake.csv", reports ? "ab" : "wb")) {
                if (!reports) fprintf(file, "model,size,status,source_texture,shader,diffuse\n");
                fprintf(file, "%s,%d,%s,%d,%s,%s\n", object ? object->GetFileName() : "<null>",
                    textureSize, status, sourceId, shaderName, diffuseName);
                fclose(file);
                ++reports;
            }
#endif
        };
        if (!object || !leaf || !leaf->m_pSecVertBuffer || textureSize <= 0 ||
            textureSize > 256 || !leaf->m_pMats || leaf->m_pMats->Count() == 0 ||
            !m_frameRenderer || !m_TexMan)
            return 0;

        struct SpriteMaterial {
            int firstIndex = 0, indexCount = 0;
            bool vertexColors = false;
            bool sunDiffuse = false;
            uint32_t alphaTest = GS_ALPHATEST_GEQUAL128;
            float alphaRef = 0.0f;
            uint32_t width = 0, height = 0;
            std::vector<uint8_t> pixels;
            uint32_t bumpWidth = 0, bumpHeight = 0;
            std::vector<uint8_t> bumpPixels;
        };
        std::vector<SpriteMaterial> spriteMaterials;
        for (int materialIndex = 0; materialIndex < leaf->m_pMats->Count(); ++materialIndex)
        {
            CMatInfo* material = leaf->m_pMats->Get(materialIndex);
            SShader* materialShader = material && material->shaderItem.m_pShader ?
                static_cast<SShader*>(material->shaderItem.m_pShader->GetTemplate(-1)) : nullptr;
            // Match the normal GL/Vulkan submission path: helper geometry
            // with a NODRAW shader is not part of the tree silhouette.
            if (!material || !materialShader || (materialShader->m_Flags3 & EF3_NODRAW)) continue;
            SRenderShaderResources* resources = material ? material->shaderItem.m_pShaderResources : nullptr;
            SEfResTexture* diffuse = resources ? resources->m_Textures[EFTT_DIFFUSE] : nullptr;
            if (diffuse && !diffuse->m_TU.m_TexPic && !diffuse->m_Name.empty())
                diffuse->m_TU.m_TexPic = m_cEF.LoadVulkanResourceTexture(diffuse->m_Name.c_str(),
                    resources->m_TexturePath.c_str(), diffuse->m_TU.GetTexFlags() | FT_NOSTREAM,
                    diffuse->m_TU.GetTexFlags2(), eTT_Base, materialShader, diffuse, diffuse->m_Amount);
            ITexPic* image = diffuse ? diffuse->m_TU.m_ITexPic : nullptr;
            const auto passImage = [&](const SShaderPass& pass) -> ITexPic* {
                if (!pass.m_TUnits.Num()) return nullptr;
                STexPic* texture = pass.m_TUnits[0].m_TexPic;
                if (!texture) return nullptr;
                if (texture->m_Bind >= 0 && texture->m_Bind < EFTT_MAX) {
                    SEfResTexture* resource = resources ? resources->m_Textures[texture->m_Bind] : nullptr;
                    return resource ? resource->m_TU.m_ITexPic : nullptr;
                }
                return texture;
            };
            if (!image)
                for (int pass = 0; pass < materialShader->m_Passes.Num() && !image; ++pass)
                    image = passImage(materialShader->m_Passes[pass]);
            if (!image)
                for (int tech = 0; tech < materialShader->m_HWTechniques.Num() && !image; ++tech) {
                    const SShaderTechnique* technique = materialShader->m_HWTechniques[tech];
                    if (technique)
                        for (int pass = 0; pass < technique->m_Passes.Num() && !image; ++pass)
                            image = passImage(technique->m_Passes[pass]);
                }
            if (material && material->nNumIndices > 0)
            {
                SpriteMaterial source;
                source.firstIndex = material->nFirstIndexId;
                source.indexCount = material->nNumIndices;
                source.vertexColors = (materialShader->m_Flags3 & EF3_HASVCOLORS) != 0;
                if (resources && !(materialShader->m_Flags2 & EF2_IGNORERESOURCESTATES))
                    source.alphaRef = resources->m_AlphaRef;
                // Cg reads mesh colors independently of fixed-function
                // texture-environment/global color selectors.
                bool colorPassFound = false;
                for (int tech = 0; tech < materialShader->m_HWTechniques.Num() && !colorPassFound; ++tech)
                {
                    const SShaderTechnique* technique = materialShader->m_HWTechniques[tech];
                    if (!technique) continue;
                    for (int pass = 0; pass < technique->m_Passes.Num(); ++pass)
                    {
                        const SShaderPassHW& candidate = technique->m_Passes[pass];
                        if (candidate.m_ePassType != eSHP_General) continue;
                        source.alphaTest = candidate.m_RenderState & GS_ALPHATEST_MASK;
                        // Resource AlphaRef and the cutout shader's state are
                        // committed separately from the hardware pass in GL.
                        if (!source.alphaTest && (materialShader->m_Flags3 & EF3_HASALPHATEST))
                            source.alphaTest = GS_ALPHATEST_GEQUAL128;
                        if (!FastAsciiCaseCompare(candidate.m_StockFragmentProgram, "CGRCPlants"))
                            source.vertexColors = true;
                        if (!FastAsciiCaseCompare(candidate.m_StockFragmentProgram, "CGRCPlants_Bump"))
                        {
                            source.vertexColors = true;
                            source.sunDiffuse = true;
                        }
                        if (!FastAsciiCaseCompare(candidate.m_StockFragmentProgram, "CGRCAmbientTempl"))
                            source.vertexColors = candidate.m_StockUsesVertexColors;
                        colorPassFound = true;
                        break;
                    }
                }
                const int sourceId = image ? image->GetTextureID() : 0;
                bool available = sourceId > 0 &&
                    m_frameRenderer->CopyRecentSpriteSourceTexture(sourceId,
                        source.width, source.height, source.pixels);
                if (!available && image && sourceId > 0) {
                    // Existing GPU textures may outlive the bounded baking
                    // cache. Restore their CPU source once, during baking.
                    STexPic* pic = static_cast<STexPic*>(image);
                    EF_LoadTexture(pic->m_SearchName.c_str(), pic->m_Flags | FT_NOSTREAM,
                        pic->m_Flags2 | FT2_RELOAD, pic->m_eTT, pic->m_fAmount1,
                        pic->m_fAmount2, pic->m_Id, pic->m_Bind);
                    available = m_frameRenderer->CopyRecentSpriteSourceTexture(sourceId,
                        source.width, source.height, source.pixels);
                }
                if (!available || !source.width || !source.height)
                { reportBake("missing_material_source", sourceId,
                    materialShader->m_Name.c_str(), diffuse ? diffuse->m_Name.c_str() : "<none>"); return 0; }
                if (source.sunDiffuse && resources)
                {
                    SEfResTexture* bump = resources->m_Textures[EFTT_BUMP];
                    if (bump && !bump->m_TU.m_ITexPic && !bump->m_Name.empty())
                        bump->m_TU.m_ITexPic = m_cEF.LoadVulkanResourceTexture(bump->m_Name.c_str(),
                            resources->m_TexturePath.c_str(), bump->m_TU.GetTexFlags() | FT_NOSTREAM,
                            bump->m_TU.GetTexFlags2(), eTT_Base, materialShader, bump, bump->m_Amount);
                    if (bump && bump->m_TU.m_ITexPic)
                    {
                        STexPic* pic = static_cast<STexPic*>(bump->m_TU.m_ITexPic);
                        if (!m_frameRenderer->CopyRecentSpriteSourceTexture(pic->GetTextureID(),
                            source.bumpWidth, source.bumpHeight, source.bumpPixels))
                        {
                            EF_LoadTexture(pic->m_SearchName.c_str(), pic->m_Flags | FT_NOSTREAM,
                                pic->m_Flags2 | FT2_RELOAD, pic->m_eTT, pic->m_fAmount1,
                                pic->m_fAmount2, pic->m_Id, pic->m_Bind);
                            m_frameRenderer->CopyRecentSpriteSourceTexture(pic->GetTextureID(),
                                source.bumpWidth, source.bumpHeight, source.bumpPixels);
                        }
                    }
                }
                spriteMaterials.push_back(std::move(source));
            }
        }
        if (spriteMaterials.empty()) return 0;

        int positionStride = 0, uvStride = 0;
        const uint8_t* positions = leaf->GetPosPtr(positionStride, 0, true);
        const uint8_t* texcoords = leaf->GetUVPtr(uvStride, 0, true);
        int indexCount = 0;
        const uint16_t* indices = leaf->GetIndices(&indexCount);
        if (!positions || !texcoords || !indices || indexCount < 3 ||
            leaf->m_SecVertCount <= 0)
            return 0;

        int colorStride = 0;
        const uint8_t* colors = leaf->GetColorPtr(colorStride, 0, true);
        int secondaryStride = 0;
        const uint8_t* secondaryColors = leaf->GetSecColorPtr(secondaryStride, 0, true);
        const Vec3 sunColor = iSystem->GetI3DEngine()->GetSunColor();
        struct SpriteVertex { float x, y, z, u, v; float rgb[3]; float light[3]; };
        std::vector<SpriteVertex> projected(static_cast<size_t>(leaf->m_SecVertCount));
        const float radians = angle * (3.14159265358979323846f / 180.0f);
        const float cosine = cosf(radians), sine = sinf(radians);
        const float radiusX = crymax(object->GetRadiusHors(), 0.01f);
        const float radiusZ = crymax(object->GetRadiusVert(), 0.01f);
        // GL MakeSprite uses a narrow perspective camera looking along -X.
        const float drawDistance = radiusZ * crymax(objectScale, 1.0f);
        const float halfFov = (0.565f / crymax(objectScale, 1.0f) * 200.0f) *
            (3.14159265358979323846f / 360.0f);
        const float halfHeight = drawDistance * tanf(halfFov);
        const float halfWidth = halfHeight * radiusX / radiusZ;
        const Vec3 center = object->GetCenter();
        for (int vertex = 0; vertex < leaf->m_SecVertCount; ++vertex)
        {
            const Vec3& position = *reinterpret_cast<const Vec3*>(positions +
                static_cast<size_t>(vertex) * positionStride);
            const SMRendTexVert& uv = *reinterpret_cast<const SMRendTexVert*>(texcoords +
                static_cast<size_t>(vertex) * uvStride);
            const float x = position.x - center.x;
            const float y = position.y - center.y;
            projected[vertex] = { x * cosine - y * sine, x * sine + y * cosine,
                                  position.z - center.z, uv.vert[0], uv.vert[1] };
            for (int channel = 0; channel < 3; ++channel)
            {
                projected[vertex].light[channel] = secondaryColors && secondaryStride > 0 ?
                    secondaryColors[size_t(vertex)*secondaryStride+channel]*(2.0f/255.0f)-1.0f : 0.0f;
                projected[vertex].rgb[channel] = colors && colorStride > 0 ?
                    colors[size_t(vertex) * colorStride + channel] * (1.0f / 255.0f) : 1.0f;
            }
        }

        const size_t outputBytes = static_cast<size_t>(textureSize) * textureSize * 4;
        std::vector<uint8_t> sprite(outputBytes, 0);
        // The GL capture clears RGB to 0.15, not black. Those transparent
        // texels still participate in bilinear filtering at the silhouette.
        for (size_t pixel = 0; pixel < outputBytes; pixel += 4)
            sprite[pixel] = sprite[pixel+1] = sprite[pixel+2] = 38;
        std::vector<float> depth(static_cast<size_t>(textureSize) * textureSize,
                                 std::numeric_limits<float>::max());
        const auto edge = [](float ax, float ay, float bx, float by, float px, float py)
        { return (px - ax) * (by - ay) - (py - ay) * (bx - ax); };
        const auto sample = [](uint32_t width, uint32_t height, const std::vector<uint8_t>& pixels,
                               float u, float v, uint8_t rgba[4])
        {
            u -= floorf(u); v -= floorf(v);
            // This is the exact RGBA upload used by the GPU. Source UVs must
            // sample the same rows, independently of the output sprite's flip.
            const float x = u * width - 0.5f;
            const float y = v * height - 0.5f;
            const int ix = int(floorf(x)), iy = int(floorf(y));
            const float fx = x - ix, fy = y - iy;
            const auto texel = [&](int px, int py) {
                px = (px % int(width) + int(width)) % int(width);
                py = (py % int(height) + int(height)) % int(height);
                return pixels.data() + (size_t(py)*width+px)*4;
            };
            const uint8_t* a = texel(ix, iy);
            const uint8_t* b = texel(ix+1, iy);
            const uint8_t* c = texel(ix, iy+1);
            const uint8_t* d = texel(ix+1, iy+1);
            for (int channel = 0; channel < 4; ++channel)
                rgba[channel] = uint8_t(((a[channel]*(1.0f-fx)+b[channel]*fx)*(1.0f-fy) +
                    (c[channel]*(1.0f-fx)+d[channel]*fx)*fy) + 0.5f);
        };
        for (int triangle = 0; triangle + 2 < indexCount; triangle += 3)
        {
            const SpriteMaterial* source = nullptr;
            for (const SpriteMaterial& material : spriteMaterials)
                if (triangle >= material.firstIndex &&
                    int64_t(triangle) + 2 < int64_t(material.firstIndex) + material.indexCount)
                { source = &material; break; }
            if (!source) continue;
            const uint16_t ia = indices[triangle], ib = indices[triangle + 1], ic = indices[triangle + 2];
            if (ia >= projected.size() || ib >= projected.size() || ic >= projected.size()) continue;
            const SpriteVertex& a = projected[ia];
            const SpriteVertex& b = projected[ib];
            const SpriteVertex& c = projected[ic];
            const float da = drawDistance - a.x, db = drawDistance - b.x, dc = drawDistance - c.x;
            if (da <= 0.0f || db <= 0.0f || dc <= 0.0f) continue;
            const auto pixelX = [&](const SpriteVertex& v, float d) {
                return (v.y * drawDistance / (d * 2.0f * halfWidth) + 0.5f) * textureSize; };
            const auto pixelY = [&](const SpriteVertex& v, float d) {
                return (0.5f - v.z * drawDistance / (d * 2.0f * halfHeight)) * textureSize; };
            const float ax = pixelX(a, da), ay = pixelY(a, da);
            const float bx = pixelX(b, db), by = pixelY(b, db);
            const float cx = pixelX(c, dc), cy = pixelY(c, dc);
            const float area = edge(ax, ay, bx, by, cx, cy);
            if (fabsf(area) < 1.0e-5f) continue;
            const int minX = crymax(0, static_cast<int>(floorf(crymin(ax, crymin(bx, cx)))));
            const int maxX = crymin(textureSize - 1, static_cast<int>(ceilf(crymax(ax, crymax(bx, cx)))));
            const int minY = crymax(0, static_cast<int>(floorf(crymin(ay, crymin(by, cy)))));
            const int maxY = crymin(textureSize - 1, static_cast<int>(ceilf(crymax(ay, crymax(by, cy)))));
            for (int py = minY; py <= maxY; ++py)
                for (int px = minX; px <= maxX; ++px)
                {
                    float wa = edge(bx, by, cx, cy, px + 0.5f, py + 0.5f) / area;
                    float wb = edge(cx, cy, ax, ay, px + 0.5f, py + 0.5f) / area;
                    float wc = 1.0f - wa - wb;
                    if (wa < -1.0e-4f || wb < -1.0e-4f || wc < -1.0e-4f) continue;
                    const float reciprocalDepth = wa/da + wb/db + wc/dc;
                    const float z = 1.0f / reciprocalDepth;
                    wa = wa / da * z; wb = wb / db * z; wc = wc / dc * z;
                    const size_t pixel = static_cast<size_t>(py) * textureSize + px;
                    if (z >= depth[pixel]) continue;
                    uint8_t rgba[4];
                    const float u = wa*a.u + wb*b.u + wc*c.u;
                    const float v = wa*a.v + wb*b.v + wc*c.v;
                    sample(source->width, source->height, source->pixels, u, v, rgba);
                    if (source->alphaRef > 0.0f && float(rgba[3]) / 255.0f < source->alphaRef) continue;
                    if (source->alphaRef <= 0.0f &&
                        ((source->alphaTest == GS_ALPHATEST_GREATER0 && rgba[3] == 0) ||
                        (source->alphaTest == GS_ALPHATEST_LESS128 && rgba[3] >= 128) ||
                        (source->alphaTest == GS_ALPHATEST_GEQUAL128 && rgba[3] < 128) ||
                        (source->alphaTest == GS_ALPHATEST_GEQUAL64 && rgba[3] < 64))) continue;
                    float sun = 0.0f;
                    if (source->sunDiffuse)
                    {
                        float normal[3] = {0.0f, 0.0f, 1.0f};
                        if (!source->bumpPixels.empty() && source->bumpWidth && source->bumpHeight)
                        {
                            uint8_t bump[4];
                            sample(source->bumpWidth, source->bumpHeight, source->bumpPixels, u, v, bump);
                            for (int channel = 0; channel < 3; ++channel)
                                normal[channel] = bump[channel]*(2.0f/255.0f)-1.0f;
                        }
                        for (int channel = 0; channel < 3; ++channel)
                            sun += normal[channel]*(wa*a.light[channel]+wb*b.light[channel]+wc*c.light[channel]);
                        sun = clamp_tpl(sun, 0.0f, 1.0f);
                    }
                    if (source->vertexColors)
                        for (int channel = 0; channel < 3; ++channel)
                            rgba[channel] = uint8_t(clamp_tpl(float(rgba[channel]) *
                                (wa * a.rgb[channel] + wb * b.rgb[channel] + wc * c.rgb[channel] +
                                 sun*sunColor[channel]),
                                0.0f, 255.0f) + 0.5f);
                    depth[pixel] = z;
                    // MakeSprite in GL reads BGRA; CreateTexture(eTF_8888)
                    // expects the same engine byte order. Our sampler is RGBA.
                    std::swap(rgba[0], rgba[2]);
                    rgba[3] = 255;
                    memcpy(sprite.data() + pixel * 4, rgba, 4);
                }
        }

        // Use the same RGB silhouette test and border clearing as GL.
        SetTextureAlphaChannelFromRGB(sprite.data(), textureSize);
        char name[256];
        snprintf(name, sizeof(name), "$VulkanFarSprite$%s$%d$%.1f",
                 object->GetFileName(), textureSize, angle);
        STexPic* result = m_TexMan->CreateTexture(name, textureSize, textureSize, 1,
            FT_HASALPHA | FT_NOMIPS | FT_CLAMP, 0, sprite.data(), eTT_Base,
            -1.0f, -1.0f, 0, nullptr, 0, eTF_RGBA, object->GetFileName());
        (void)objectScale;
        reportBake(result ? "ready" : "texture_creation_failed", 0);
        return result ? result->m_Bind : 0;
    }

    void DrawObjSprites(list2<CStatObjInst*>* instances, float maxViewDistance,
                        CObjManager* objectManager) override
    {
        if (!m_frameOpen || !m_frameRenderer || !instances || !objectManager ||
            instances->Count() == 0)
            return;
        struct SpriteBatch
        {
            std::vector<struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F> vertices;
            std::vector<uint16_t> indices;
        };
        static std::map<int, SpriteBatch> batches;
        // Keep per-texture vectors and map nodes across frames. Clearing the
        // map destroyed every bucket's capacity each frame, rebuilding the
        // same billboard buffers and allocating on the CPU hot path.
        for (std::map<int, SpriteBatch>::iterator it = batches.begin(); it != batches.end(); ++it)
        {
            it->second.vertices.clear();
            it->second.indices.clear();
        }
        const Vec3 camera = m_RP.m_ViewOrg;
        const float maxDistance = maxViewDistance * 0.8f;
        const Vec3 world = iSystem->GetI3DEngine()->GetWorldColor();
        for (int instanceIndex = 0; instanceIndex < instances->Count(); ++instanceIndex)
        {
            CStatObjInst* instance = instances->GetAt(instanceIndex);
            if (!instance || instance->m_nObjectTypeID < 0 ||
                instance->m_nObjectTypeID >= objectManager->m_lstStaticTypes.Count()) continue;
            CStatObj* body = objectManager->m_lstStaticTypes[instance->m_nObjectTypeID].GetStatObj();
            if (!body || !body->IsSpritesCreated()) continue;
            CStatObj* spriteShape = body;
            if (body->m_nLoadedLodsNum && body->m_arrpLowLODs[body->m_nLoadedLodsNum-1])
                spriteShape = body->m_arrpLowLODs[body->m_nLoadedLodsNum-1];
            const int distanceSlot = clamp_tpl(SRendItem::m_RecurseLevel - 1, 0, 2);
            const float distance = crymax(instance->m_arrfDistance[distanceSlot], 0.01f);
            float fade = 1.0f;
            if (objectManager->m_lstStaticTypes[instance->m_nObjectTypeID].bFadeSize)
            {
                const float limit = crymin(instance->GetMaxViewDist(), maxDistance);
                fade = (1.0f - distance * objectManager->m_fZoomFactor / crymax(limit, 0.01f)) * 8.0f;
                if (fade <= 0.0f) continue;
                fade = crymin(fade, 1.0f);
            }
            const float dx = instance->m_vPos.x - camera.x;
            const float dy = instance->m_vPos.y - camera.y;
            float angle = atan2f(dx + instance->m_fScale*spriteShape->GetCenter().x,
                dy + instance->m_fScale*spriteShape->GetCenter().y) * (180.0f / 3.14159265358979323846f);
            if (angle < 0.0f) angle += 360.0f;
            const int slot = static_cast<int>(angle / FAR_TEX_ANGLE + 0.5f) % FAR_TEX_COUNT;
            if (SRendItem::m_RecurseLevel == 1) instance->m_ucAngleSlotId = slot;
            const int textureId = static_cast<int>(body->m_arrSpriteTexID[slot]);
            if (textureId <= 0) continue;
            const Vec3 center = spriteShape->GetCenter() * instance->m_fScale;
            const float vertical = instance->m_fScale * spriteShape->GetRadiusVert() * fade;
            const float horizontal = instance->m_fScale * spriteShape->GetRadiusHors() *
                objectManager->m_fZoomFactor * fade;
            const float halfX = dy * horizontal / distance;
            const float halfY = dx * horizontal / distance;
            const Vec3 origin = instance->m_vPos + center * fade;
            SpriteBatch& batch = batches[textureId];
            if (batch.vertices.size() > 65000)
                continue;
            const uint16_t base = static_cast<uint16_t>(batch.vertices.size());
            const float brightness = crymin(crymax(float(instance->m_ucBright), 32.0f) * (1.0f / 255.0f) *
                objectManager->m_lstStaticTypes[instance->m_nObjectTypeID].fBrightness, 1.0f);
            const auto colorByte = [](float v) -> uint8_t
            { return static_cast<uint8_t>(clamp_tpl(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
            const uint8_t rgb[3] = { colorByte(world.x * brightness),
                                     colorByte(world.y * brightness),
                                     colorByte(world.z * brightness) };
            const Vec3 p[4] = {
                Vec3(origin.x - halfX, origin.y + halfY, origin.z - vertical),
                Vec3(origin.x + halfX, origin.y - halfY, origin.z - vertical),
                Vec3(origin.x + halfX, origin.y - halfY, origin.z + vertical),
                Vec3(origin.x - halfX, origin.y + halfY, origin.z + vertical) };
            // CPU baking stores the top scanline first, unlike glReadPixels.
            const float uv[4][2] = { {0,1}, {1,1}, {1,0}, {0,0} };
            for (int corner = 0; corner < 4; ++corner)
            {
                struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F v{};
                v.xyz = p[corner];
                v.st[0] = uv[corner][0]; v.st[1] = uv[corner][1];
                v.color.bcolor[0] = rgb[0]; v.color.bcolor[1] = rgb[1];
                v.color.bcolor[2] = rgb[2]; v.color.bcolor[3] = 255;
                batch.vertices.push_back(v);
            }
            const uint16_t quad[6] = { base, static_cast<uint16_t>(base+1),
                static_cast<uint16_t>(base+2), base, static_cast<uint16_t>(base+2),
                static_cast<uint16_t>(base+3) };
            batch.indices.insert(batch.indices.end(), quad, quad + 6);
        }
        // GL's sprite executor binds its own one-texture program. The direct
        // Vulkan client draw must not inherit a model's pass or extra TMUs.
        SShaderPass* savedPass = m_activePass;
        SRenderShaderResources* savedResources = m_activeResources;
        const EShaderPassType savedPassType = m_activeHardwarePassType;
        SEfState* savedState = m_activeStateShaderState;
        m_activePass = nullptr;
        m_activeResources = nullptr;
        m_activeHardwarePassType = eSHP_MAX;
        m_activeStateShaderState = nullptr;
        ResetToDefault();
        SetCullMode(R_CULL_NONE);
        SetColorOp(eCO_MODULATE, eCO_MODULATE, DEF_TEXARG0, DEF_TEXARG0);
        EF_SetState(GS_ALPHATEST_GEQUAL128 | GS_DEPTHWRITE);
        m_frameRenderer->SetStockFarSprites(true);
        m_frameRenderer->SetStockFixedLights({}, 0);
        m_frameRenderer->SetStockProjector(0, nullptr, 1.0f);
        m_frameRenderer->ResetStockLinearTexgen();
        m_frameRenderer->SetStockShadowTransforms(nullptr, 0);
        m_frameRenderer->SetStockGpuSkinIdentity(nullptr);
        float spriteIdentity[16] = {};
        spriteIdentity[0] = spriteIdentity[5] = spriteIdentity[10] = spriteIdentity[15] = 1.0f;
        for (std::map<int, SpriteBatch>::iterator it = batches.begin(); it != batches.end(); ++it)
        {
            SpriteBatch& batch = it->second;
            if (batch.vertices.empty()) continue;
            // Billboard vertices are already in world space. Do not run them
            // through the previous model's object transform, RGBA generators,
            // light passes or resource opacity in DrawBuffer.
            m_frameRenderer->QueueStockClientIndexedDraw(batch.vertices.data(),
                static_cast<uint32_t>(batch.vertices.size()), batch.indices.data(),
                static_cast<uint32_t>(batch.indices.size()), VERTEX_FORMAT_P3F_COL4UB_TEX2F,
                R_PRIMV_TRIANGLES, GS_ALPHATEST_GEQUAL128 | GS_DEPTHWRITE, R_CULL_NONE,
                it->first, 0, eCO_MODULATE, eCO_MODULATE, eCO_MODULATE, eCO_MODULATE,
                DEF_TEXARG0, DEF_TEXARG0, 0xffffffffu, DEF_TEXARG1, DEF_TEXARG1, 0xffffffffu,
                0, 0, 0xff, m_CameraMatrix.GetData(), spriteIdentity, spriteIdentity);
        }
        m_frameRenderer->SetStockFarSprites(false);
        ResetToDefault();
        m_activePass = savedPass;
        m_activeResources = savedResources;
        m_activeHardwarePassType = savedPassType;
        m_activeStateShaderState = savedState;
    }

    void SetLodBias(float value) override
    {
        const int stage = m_TexMan ? m_TexMan->GetCurTMU() : 0;
        if (stage >= 0 && stage < 8 && std::isfinite(value))
        {
            m_stageLodBias[stage] = value;
            if (m_activePass) m_stageStateOverrides[stage] |= 16u;
        }
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
            texture->m_ETF == eTF_SIGNED_HILO8 || texture->m_ETF == eTF_V8U8 ||
            texture->m_ETF == eTF_SIGNED_HILO16 || texture->m_ETF == eTF_V16U16 ||
            texture->m_ETF == eTF_SIGNED_RGB8 ||
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
                // Match GL_BGRA_EXT for updates to lightmaps and dynamic RGBA
                // textures, just as for their initial Vulkan upload.
                rgba[i * 4] = data[i * 4 + 2]; rgba[i * 4 + 1] = data[i * 4 + 1];
                rgba[i * 4 + 2] = data[i * 4]; rgba[i * 4 + 3] = data[i * 4 + 3];
                break;
            case eTF_8000:
                rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
                rgba[i * 4 + 3] = data[i];
                break;
            default:
                return false;
            }
            // Match GL_ALPHA8 font storage on atlas updates as well.
            rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
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
        // GLFont installs its own blend state regardless of the preceding HUD pass.
        SetState(GS_BLSRC_SRCALPHA | GS_BLDST_ONEMINUSSRCALPHA |
                 GS_NODEPTHTEST | GS_ALPHATEST_GREATER0);
    }

    void FontRestoreRenderingState() override
    {
        m_fontRenderingState = false;
    }

    void RemoveTexture(unsigned int textureId) override
    {
        m_textureWrapOverrides.erase(static_cast<int>(textureId));
        CNULLRenderer::RemoveTexture(textureId);
    }

    unsigned int DownLoadToVideoMemory(unsigned char* data, int width, int height,
                                       ETEX_Format sourceFormat, ETEX_Format destinationFormat,
                                       int mipCount, bool repeat, int filter, int textureId,
                                       char* cacheName, int flags) override
    {
        if (!data || width <= 0 || height <= 0)
            return 0;
        // OpenGL interprets nummipmap == 0 as a base-level upload and
        // requests generated mipmaps for bilinear/trilinear filtering.
        // A nonzero value uploads an explicit mip level (1 - nummipmap),
        // while linear/nearest filters do not need a mip chain.
        if (mipCount != 0 ||
            (filter != FILTER_BILINEAR && filter != FILTER_TRILINEAR))
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
        // CGLRenderer::DownLoadToVideoMemory uses GL_NEAREST for FILTER_NONE
        // and any unrecognized filter value.
        int filterMode = eVTF_Nearest;
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
            sourceFormat == eTF_SIGNED_HILO8 || sourceFormat == eTF_V8U8 ||
            sourceFormat == eTF_SIGNED_HILO16 || sourceFormat == eTF_V16U16 ||
            sourceFormat == eTF_SIGNED_RGB8 ||
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

    void PrepareDepthMap(ShadowMapFrustum* frustum, bool makeNewTexture) override
    {
        if (!frustum || !frustum->pLs || !m_frameRenderer || m_RP.m_bDrawToTexture)
            return;
        if (frustum->depth_tex_id && !frustum->bUpdateRequested && !makeNewTexture)
            return;

        // OpenGL keeps the depth map alive on the frustum and updates it only
        // when requested. Give Vulkan shadow images private IDs so they share
        // the same legacy texture lookup path without colliding with game
        // STexPic IDs.
        static int nextShadowTextureId = 0x40000000;
        if (frustum->depth_tex_id == 0 || makeNewTexture)
        {
            if (nextShadowTextureId <= 0 || nextShadowTextureId == 0x7fffffff)
                nextShadowTextureId = 0x40000000;
            frustum->depth_tex_id = static_cast<uint32_t>(nextShadowTextureId++);
        }

        frustum->nTexSize = crymax(frustum->nTexSize, 32);
        uint32_t shadowSize = static_cast<uint32_t>(frustum->nTexSize);
        // GL_Shadows normalizes the capture to the renderer dimensions.
        while ((m_width > 0 && shadowSize > static_cast<uint32_t>(m_width)) ||
               (m_height > 0 && shadowSize > static_cast<uint32_t>(m_height)))
            shadowSize /= 2;
        if (!m_frameRenderer->RegisterLegacyDepthTexture(
                static_cast<int>(frustum->depth_tex_id), shadowSize, shadowSize))
        {
            static uint32_t shadowAllocationWarnings = 0;
            if (shadowAllocationWarnings++ < 4)
                CryLogAlways("OpenXR/Vulkan: failed to create %ux%u shadow map texture id=%u (further failures suppressed)",
                             shadowSize, shadowSize, frustum->depth_tex_id);
            frustum->depth_tex_id = 0;
            return;
        }

        makeProjectionMatrix(frustum->FOV, frustum->ProjRatio,
                             frustum->min_dist, frustum->max_dist,
                             frustum->debugLightFrustumMatrix);
        const Vec3d eye = frustum->pLs->vSrcPos;
        const Vec3d target = frustum->target;
        Vec3d zAxis = eye - target;
        if (zAxis.Normalize() == 0.0f)
            return;
        Vec3d yAxis(0.0f, 0.0f, 1.0f);
        Vec3d xAxis = yAxis ^ zAxis;
        if (xAxis.Normalize() == 0.0f)
            return;
        yAxis = zAxis ^ xAxis;
        if (yAxis.Normalize() == 0.0f)
            return;
        float* lightView = frustum->debugLightViewMatrix;
        memset(lightView, 0, sizeof(frustum->debugLightViewMatrix));
        lightView[0] = xAxis.x; lightView[4] = xAxis.y; lightView[8] = xAxis.z;
        lightView[1] = yAxis.x; lightView[5] = yAxis.y; lightView[9] = yAxis.z;
        lightView[2] = zAxis.x; lightView[6] = zAxis.y; lightView[10] = zAxis.z;
        lightView[15] = 1.0f;
        lightView[12] = -(lightView[0] * eye.x + lightView[4] * eye.y + lightView[8] * eye.z);
        lightView[13] = -(lightView[1] * eye.x + lightView[5] * eye.y + lightView[9] * eye.z);
        lightView[14] = -(lightView[2] * eye.x + lightView[6] * eye.y + lightView[10] * eye.z);

        // OpenGL's PrepareDepthMap switches to the light matrices, renders
        // both caster lists, then restores the camera. Queue the same model
        // and entity draws tagged for this Vulkan shadow framebuffer; the
        // frame recorder uses their captured object transforms and this
        // light projection instead of either XR eye projection.
        const Matrix44 savedCameraMatrix = m_CameraMatrix;
        const Matrix44 savedViewMatrix = m_ViewMatrix;
        const Matrix44 savedProjectionMatrix = m_ProjMatrix;
        const Matrix44 savedCameraProjectionMatrix = m_CameraProjMatrix;
        const Matrix44 savedInverseCameraProjectionMatrix = m_InvCameraProjMatrix;
        const int savedViewportX = m_VX;
        const int savedViewportY = m_VY;
        const int savedViewportWidth = m_VWidth;
        const int savedViewportHeight = m_VHeight;
        const bool savedFogEnabled = EnableFog(false);
        memcpy(m_CameraMatrix.GetData(), lightView, sizeof(float) * 16);
        m_ViewMatrix = m_CameraMatrix;
        memcpy(m_ProjMatrix.GetData(), frustum->debugLightFrustumMatrix,
               sizeof(frustum->debugLightFrustumMatrix));
        mathMatrixMultiply(m_CameraProjMatrix.GetData(), m_ProjMatrix.GetData(),
                           m_CameraMatrix.GetData(), g_CpuFlags);
        mathMatrixInverse(m_InvCameraProjMatrix.GetData(),
                          m_CameraProjMatrix.GetData(), g_CpuFlags);
        SetViewport(0, 0, static_cast<int>(shadowSize), static_cast<int>(shadowSize));
        m_frameRenderer->SetStockShadowMapPass(
            static_cast<int>(frustum->depth_tex_id), frustum->debugLightFrustumMatrix);
        m_collectingShadowCasters = true;

        IShader* const noCullState = EF_LoadShader("StateNoCull", eSH_World);
        if (frustum->pModelsList && frustum->pModelsList->Count())
        {
            EF_StartEf();
            for (int modelIndex = 0; modelIndex < frustum->pModelsList->Count(); ++modelIndex)
            {
                SRendParams renderParams;
                renderParams.pStateShader = noCullState;
                if (!frustum->m_fBending)
                    renderParams.nShaderTemplate = EFT_WHITESHADOW;
                if (frustum->pEntityList && frustum->pEntityList->Count())
                    renderParams.vPos -= (*frustum->pEntityList)[0]->GetPos();
                renderParams.dwFObjFlags |= FOB_TRANS_MASK;
                renderParams.dwFObjFlags |= FOB_RENDER_INTO_SHADOWMAP;
                renderParams.fBending = frustum->m_fBending;
                (*frustum->pModelsList)[modelIndex]->Render(renderParams, Vec3(zero), 0);
            }
            EF_EndEf3D(true);
        }

        if (frustum->pEntityList && frustum->pOwner && m_RP.m_pCurObject)
        {
            EF_StartEf();
            for (int entityIndex = 0; entityIndex < frustum->pEntityList->Count(); ++entityIndex)
            {
                IEntityRender* const entity = (*frustum->pEntityList)[entityIndex];
                if (!entity)
                    continue;
                Vec3d offset = GetNormalized(
                    m_RP.m_pCurObject->GetTranslation() - frustum->pLs->vSrcPos) *
                    (0.1f + 0.035f * (256.0f / static_cast<float>(shadowSize)));
                SRendParams renderParams;
                renderParams.nShaderTemplate =
                    (m_Features & RFT_DEPTHMAPS) ? EFT_WHITE : EFT_WHITESHADOW;
                renderParams.pStateShader = noCullState;
                renderParams.vPos = offset + (entity->GetPos() - frustum->pOwner->GetPos());
                renderParams.dwFObjFlags |= FOB_RENDER_INTO_SHADOWMAP;
                renderParams.dwFObjFlags |= FOB_TRANS_MASK;
                entity->DrawEntity(renderParams);
            }
            EF_EndEf3D(true);
        }

        m_collectingShadowCasters = false;
        m_frameRenderer->SetStockShadowMapPass(0, nullptr);
        m_CameraMatrix = savedCameraMatrix;
        m_ViewMatrix = savedViewMatrix;
        m_ProjMatrix = savedProjectionMatrix;
        m_CameraProjMatrix = savedCameraProjectionMatrix;
        m_InvCameraProjMatrix = savedInverseCameraProjectionMatrix;
        SetViewport(savedViewportX, savedViewportY,
                    savedViewportWidth, savedViewportHeight);
        UpdateLegacyCameraInfo();
        // GL_Shadows finishes the capture by disabling scissor and clearing
        // the restored main color/depth buffers (stencil is preserved).
        SetScissor(0, 0, 0, 0);
        EF_SetState(GS_DEPTHWRITE);
        const float restoredClearColor[4] = {
            m_vClearColor.x, m_vClearColor.y, m_vClearColor.z, 0.0f
        };
        if (!m_frameRenderer->QueueStockClear(true, true, false, restoredClearColor))
            m_frameRenderer->RequirePanelFallback();
        EnableFog(savedFogEnabled);
        const bool expectedCasters =
            (frustum->pModelsList && frustum->pModelsList->Count() > 0) ||
            (frustum->pEntityList && frustum->pEntityList->Count() > 0);
        frustum->bUpdateRequested = expectedCasters &&
            !m_frameRenderer->HasQueuedShadowMapDraws(
                static_cast<int>(frustum->depth_tex_id));
    }

    void SetupShadowOnlyPass(int textureStage, ShadowMapFrustum* frustum,
                             Vec3d* shadowTranslation, const float shadowScale,
                             Vec3d objectTranslation, float objectScale,
                             const Vec3d objectAngles, Matrix44* objectMatrix) override
    {
        (void)objectTranslation;
        if (!frustum || textureStage < 0 || textureStage >= 8 ||
            !frustum->pLs || !(shadowScale > 0.0f))
            return;

        float lightProjection[16]{};
        float lightView[16]{};
        if (shadowTranslation)
        {
            makeProjectionMatrix(frustum->FOV * shadowScale, frustum->ProjRatio,
                                 frustum->min_dist, frustum->max_dist,
                                 lightProjection);
            const Vec3d eye(frustum->pLs->vSrcPos.x + shadowTranslation->x,
                            frustum->pLs->vSrcPos.y + shadowTranslation->y,
                            frustum->pLs->vSrcPos.z + shadowTranslation->z);
            const Vec3d target(frustum->target.x * shadowScale + shadowTranslation->x,
                               frustum->target.y * shadowScale + shadowTranslation->y,
                               frustum->target.z * shadowScale + shadowTranslation->z);

            // Match SGLFuncs::gluLookAt's world-up (0,0,1) basis and matrix
            // layout. The OpenGL shadow setup uses this view before applying
            // either the receiver object's transform or scale/rotation.
            Vec3d zAxis = eye - target;
            if (zAxis.Normalize() == 0.0f)
                return;
            Vec3d yAxis(0.0f, 0.0f, 1.0f);
            Vec3d xAxis = yAxis ^ zAxis;
            if (xAxis.Normalize() == 0.0f)
                return;
            yAxis = zAxis ^ xAxis;
            if (yAxis.Normalize() == 0.0f)
                return;
            lightView[0] = xAxis.x; lightView[4] = xAxis.y; lightView[8] = xAxis.z;
            lightView[1] = yAxis.x; lightView[5] = yAxis.y; lightView[9] = yAxis.z;
            lightView[2] = zAxis.x; lightView[6] = zAxis.y; lightView[10] = zAxis.z;
            lightView[15] = 1.0f;
            lightView[12] = -(lightView[0] * eye.x + lightView[4] * eye.y + lightView[8] * eye.z);
            lightView[13] = -(lightView[1] * eye.x + lightView[5] * eye.y + lightView[9] * eye.z);
            lightView[14] = -(lightView[2] * eye.x + lightView[6] * eye.y + lightView[10] * eye.z);
            if (objectMatrix)
                mathMatrixMultiply(lightView, lightView, objectMatrix->GetData(), g_CpuFlags);
            else
            {
                mathRotateZ(lightView, objectAngles.z, g_CpuFlags);
                mathRotateY(lightView, objectAngles.y, g_CpuFlags);
                mathRotateX(lightView, objectAngles.x, g_CpuFlags);
                mathScale(lightView, Vec3d(objectScale, objectScale, objectScale), g_CpuFlags);
            }
        }
        else
        {
            // PrepareDepthMap stores the exact light matrices on the frustum;
            // non-translated receivers reuse those cached matrices in OpenGL.
            memcpy(lightProjection, frustum->debugLightFrustumMatrix, sizeof(lightProjection));
            memcpy(lightView, frustum->debugLightViewMatrix, sizeof(lightView));
        }

        if (frustum->depth_tex_id > 0)
        {
            if (m_RP.m_pRE)
            {
                m_RP.m_pRE->m_CustomTexBind[textureStage] =
                    static_cast<int>(frustum->depth_tex_id);
                m_RP.m_pRE->m_Color[textureStage] = frustum->fAlpha;
            }
            else
            {
                m_RP.m_RECustomTexBind[textureStage] =
                    static_cast<int>(frustum->depth_tex_id);
                m_RP.m_REColor[textureStage] = frustum->fAlpha;
            }
        }

        float textureBias[16] = {
            0.5f, 0.0f, 0.0f, 0.0f,
            0.0f, 0.5f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.5f, 0.0f,
            0.5f, 0.5f, 0.5f, 1.0f
        };
        float lightViewProjection[16]{};
        float shadowTextureMatrix[16]{};
        mathMatrixMultiply(lightViewProjection, lightProjection, lightView, g_CpuFlags);
        mathMatrixMultiply(shadowTextureMatrix, textureBias, lightViewProjection, g_CpuFlags);
        mathMatrixTranspose(m_cEF.m_TempMatrices[textureStage][0].GetData(),
                            shadowTextureMatrix, g_CpuFlags);
    }

    void EF_EndEf3D(int flags) override
    {
        if (SRendItem::m_RecurseLevel < 1)
        {
            if (iLog) iLog->Log("Error: CRenderer::EF_EndEf3D without CRenderer::EF_StartEf");
            return;
        }
        if (CV_r_nodrawshaders == 1)
        {
            SetClearColor(Vec3d(0, 0, 0));
            ClearStockSceneBuffers(false, false);
            --SRendItem::m_RecurseLevel;
            return;
        }
        static unsigned int endEfAuditCalls = 0;
        const unsigned int endEfAuditCall = endEfAuditCalls++;
        if (g_vulkanAudit && (endEfAuditCall < 12 || (endEfAuditCall % 120) == 0))
            CryLogAlways("Vulkan EF_EndEf3D path: call=%u frame=%d open=%u recurse=%d renderer=%p flags=0x%x",
                endEfAuditCall, GetFrameID(), m_frameOpen ? 1u : 0u,
                SRendItem::m_RecurseLevel - 1, m_frameRenderer, flags);
        // XRenderNULL deliberately drops the stock renderer's render-item
        // queues. Execute the opaque stock items here while their frame data
        // and visibility-object table are still valid.
        const int recurseLevel = SRendItem::m_RecurseLevel - 1;
        if (m_frameOpen && recurseLevel >= 0 && recurseLevel < 8)
        {
            m_RP.m_PersFlags &= ~(RBPF_DRAWNIGHTMAP | RBPF_DRAWHEATMAP);
            m_RP.m_RealTime = iTimer ? iTimer->GetCurrTime() : 0.0f;
            m_RP.m_ExcludeShader = CV_r_excludeshader &&
                CV_r_excludeshader->GetString()[0] != '0' ?
                CV_r_excludeshader->GetString() : nullptr;
            m_RP.m_ShowOnlyShader = CV_r_showonlyshader &&
                CV_r_showonlyshader->GetString()[0] != '0' ?
                CV_r_showonlyshader->GetString() : nullptr;
            EF_UpdateSplashes(m_RP.m_RealTime);
            EF_AddClientPolys3D();
            EF_AddClientPolys2D();
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

#if defined(__ANDROID__) && defined(CRYVR_RENDER_PROBES)
            static int sceneTraceFrames = 0;
            static int sceneTraceLastFrame = -1000;
            if (!m_collectingShadowCasters && recurseLevel == 0 &&
                m_frameRenderer && m_frameRenderer->IsFrameActive() &&
                sceneTraceFrames < 64 && GetFrameID() - sceneTraceLastFrame >= 90 &&
                !IsEquivalent(m_cam.GetPos(), Vec3(0, 0, 0)))
            {
                int worldItems = 0;
                for (int list = 0; list < NUMRI_LISTS; ++list)
                    for (int index = SRendItem::m_StartRI[recurseLevel][list];
                         index < SRendItem::m_EndRI[recurseLevel][list]; ++index)
                        if (SRendItem::m_RendItems[list][index].Item &&
                            SRendItem::m_RendItems[list][index].Item->mfGetType() == eDATA_OcLeaf)
                            ++worldItems;
                if (worldItems > 10)
                {
                    m_sceneTrace = fopen("/sdcard/FarCry/vulkan_cutscene_trace.txt",
                        sceneTraceFrames ? "ab" : "wb");
                    m_sceneTraceLines = 0;
                    if (m_sceneTrace)
                    {
                        fprintf(m_sceneTrace, "FRAME %d camera=%.3f,%.3f,%.3f angles=%.3f,%.3f,%.3f items=%d\n",
                            GetFrameID(), m_cam.GetPos().x, m_cam.GetPos().y, m_cam.GetPos().z,
                            m_cam.GetAngles().x, m_cam.GetAngles().y, m_cam.GetAngles().z, worldItems);
                        ++sceneTraceFrames;
                        sceneTraceLastFrame = GetFrameID();
                    }
                }
            }
#endif

            // Capture several scene frames after the renderer is active. Keep
            // this diagnostic bounded: the full item and draw trace is useful
            // for a device log, but must not grow for the lifetime of a game.
            static int auditFramesRemaining = 10;
            static int lastAuditedFrame = -1;
            int auditItemTotal = 0;
            bool auditHasWorldLeaf = false;
            for (int list = 0; g_vulkanAudit && list < NUMRI_LISTS; ++list)
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
            m_auditFrameActive = g_vulkanAudit && auditFramesRemaining > 0 && auditHasWorldLeaf &&
                (lastAuditedFrame < 0 || GetFrameID() - lastAuditedFrame >= 120) &&
                m_frameRenderer && m_frameRenderer->IsFrameActive();
            if (m_auditFrameActive)
            {
                --auditFramesRemaining;
                lastAuditedFrame = GetFrameID();
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
                // EF_RenderPipeLine tests suppression once, immediately after
                // PREPROCESS. Later lists may alter the flag, but that does
                // not retroactively change entry into the outer list block.
                if (order == 1 && (m_RP.m_PersFlags & RBPF_IGNORERENDERING))
                {
                    m_RP.m_PersFlags &= ~RBPF_IGNORERENDERING;
                    break;
                }
                if (list == EFSLIST_LAST_ID && recurseLevel > 0)
                    continue;
                int drawFirst = SRendItem::m_StartRI[recurseLevel][list];
                int last = SRendItem::m_EndRI[recurseLevel][list];
                if (drawFirst < 0 || drawFirst >= last)
                    continue;
                CCObject* savedObject = m_RP.m_pCurObject;
                // EF_PipeLine establishes the identity object before both
                // PreRender(1) and preprocess, not only before item drawing.
                m_RP.m_nCurLightParam = -1;
                m_RP.m_pCurObject = m_RP.m_NumVisObjects > 0 ? m_RP.m_VisObjects[0] : nullptr;
                m_RP.m_pPrevObject = m_RP.m_pCurObject;
                // OpenGL runs EF_PreRender(1) once for every non-empty list,
                // before sorting and preprocess handling.
                PrepareStockFrameState();

                // Match the stock XRenderOGL list preparation: shader-sort the
                // preprocess/general/last lists, distance-sort blended items,
                // and keep stencil clear/shadow runs in stencil-specific order.
                if (list == EFSLIST_PREPROCESS_ID || list == EFSLIST_GENERAL_ID ||
                    list == EFSLIST_LAST_ID)
                {
                    SRendItem::mfSort(&SRendItem::m_RendItems[list][drawFirst], last - drawFirst);
                    if ((SRendItem::m_RendItems[list][drawFirst].SortVal.i.High >> 26) == eS_PreProcess)
                        drawFirst += ProcessStockPreprocess(list, drawFirst, last);
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

                // GL checks this flag after preprocess, which can change it.
                if ((list == EFSLIST_PREPROCESS_ID || list == EFSLIST_GENERAL_ID ||
                     list == EFSLIST_LAST_ID) && (m_RP.m_PersFlags & RBPF_IGNORERENDERING))
                {
                    m_RP.m_pCurObject = savedObject;
                    continue;
                }
                // EF_PreRender(3) repeats the Stage & 1 preparation, then
                // applies Stage & 2 depth-range/clear state.
                PrepareStockFrameState();
                // This is the state established by EF_PreRender(3) and the
                // list walker immediately before entering its item loop.
                if (!m_RP.m_bStartPipeline && !m_bWasCleared &&
                    !(m_RP.m_PersFlags & RBPF_NOCLEARBUF))
                {
                    m_RP.m_bStartPipeline = true;
                    ClearStockSceneBuffers(false, false);
                }
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
                m_RP.m_fMinDepthRange = minDepthRange;
                m_RP.m_fMaxDepthRange = maxDepthRange;
                m_frameRenderer->SetStockDepthRange(minDepthRange, maxDepthRange);
                m_RP.m_pCurObject = m_RP.m_NumVisObjects > 0 ? m_RP.m_VisObjects[0] : nullptr;
                m_RP.m_Flags |= RBF_3D;
                SShader* previousStateShader = nullptr;
                SShader* previousStencilShader = nullptr;
                SRenderShaderResources* previousStencilResources = nullptr;
                int previousStencilObject = -1;
                int previousStencilFog = -1;
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
#if defined(__ANDROID__)
                    if (m_sceneTrace && !m_collectingShadowCasters && renderObject && m_sceneTraceLines++ < 256)
                        fprintf(m_sceneTrace, "ITEM list=%d shader=%s re=%d object=%d flags=%x lm=%d,%d opacity=%.4f fog=%d\n",
                            list, shader ? shader->GetName() : "<none>", item->Item->mfGetType(),
                            objectIndex, renderObject->m_ObjFlags, renderObject->m_nLMId,
                            renderObject->m_nLMDirId, renderObject->m_Color.a, fogIndex);
#endif
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
                    // EF_ObjectChange excludes the capture source by position,
                    // not pointer identity; sky remains visible in reflections.
                    if (shader && m_RP.m_pIgnoreObject && !(shader->m_Flags & EF_SKY) &&
                        IsEquivalent(m_RP.m_pIgnoreObject->GetTranslation(), renderObject->GetTranslation()))
                        continue;
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
                        CryLogAlways("Vulkan audit ITEM frame=%d list=%d index=%d type=%d shader=%s shaderId=%d sort=%d obj=%d objFlags=0x%x pos=(%.3f,%.3f,%.3f) fog=%d resources=%d opacity=%.3f dynLights=0x%x lightmaps=(%d,%d,%d) fixedPasses=%d hwTechniques=%d stateShader=%s chunk=%d+%d verts=%d firstVert=%d",
                            GetFrameID(), list, itemIndex, item->Item->mfGetType(),
                            shader ? shader->m_Name.c_str() : "<null>", shader ? shader->m_Id : -1,
                            item->Item->m_SortId, objectIndex, renderObject->m_ObjFlags,
                            renderObject->GetTranslation().x, renderObject->GetTranslation().y,
                            renderObject->GetTranslation().z, fogIndex,
                            resources ? resources->m_Id : -1, resources ? resources->m_Opacity : 1.0f,
                            item->DynLMask, renderObject->m_nLMId,
                            renderObject->m_nLMDirId, renderObject->m_nHDRLMId,
                            shader ? shader->m_Passes.Num() : 0,
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

                    if (renderObject != m_RP.m_pPrevObject)
                    {
                        UpdateStockNearestCamera(objectIndex != 0 &&
                            (renderObject->m_ObjFlags & FOB_NEAREST));
                        ++m_RP.m_FrameObject;
                        m_RP.m_pCurObject = renderObject;
                        UpdateLegacyObjectTransform(renderObject);
                        // EF_ObjectChange's cache denotes the object whose
                        // state is loaded, rather than the object it replaced.
                        m_RP.m_pPrevObject = renderObject;
                    }
                    m_RP.m_pRE = item->Item;
                    m_RP.m_pShader = shader;
                    m_RP.m_pStateShader = shaderState;
                    m_RP.m_pShaderResources = resources;
                    // EF_Start resets these for each new material batch.
                    // Vulkan flushes each item directly, so its item is the
                    // corresponding batch boundary.
                    m_RP.m_FlagsPerFlush = 0;
                    m_RP.m_FlagsModificators = 0;
                    m_RP.m_ResourceState = 0;
                    m_RP.m_pCurLightMaterial = nullptr;
                    m_RP.m_pCurTechnique = nullptr;
                    ++m_RP.m_Frame;
                    m_RP.m_RendNumIndices = 0;
                    m_RP.m_RendNumVerts = 0;
                    m_RP.m_RendIndices = m_RP.m_SysRendIndices;
                    // EF_Start owns the lifetime of this list as well as
                    // MergedREs/MergedObjs. Shared leaf preparation traverses
                    // it when uploading object-specific lightmap coordinates.
                    m_RP.m_MergedObjects.SetUse(0);
                    m_RP.m_MergedREs.SetUse(0);
                    m_RP.m_MergedObjs.SetUse(0);
                    if (shader && shader->m_VertexFormatId >= 0 &&
                        shader->m_VertexFormatId < VERTEX_FORMAT_NUMS)
                    {
                        const int format = shader->m_VertexFormatId;
                        const SBufInfoTable& offsets = gBufInfoTable[format];
                        m_RP.m_CurVFormat = format;
                        m_RP.m_Stride = m_VertexSize[format];
                        m_RP.m_OffsD = offsets.OffsColor;
                        m_RP.m_OffsT = offsets.OffsTC;
                        m_RP.m_OffsN = offsets.OffsNormal;
                        m_RP.m_NextPtr = m_RP.m_Ptr;
                    }
                    // EF_Start builds the initial light list from the live
                    // object's mask before selecting the hardware technique.
                    m_RP.m_DynLMask = renderObject->m_DynLMMask;
                    m_RP.m_ObjFlags = renderObject->m_ObjFlags;
                    m_RP.m_fCurOpacity = 1.0f;
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
                        m_RP.m_fCurOpacity = 1.0f;
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
                    // hardware technique. EF_Start initializes opacity to one;
                    // resource opacity is applied later by EF_FlushShader.
                    EF_BuildLightsList();
                    // CGLRenderer::EF_Start resets this for each shader/item
                    // batch. Vulkan emits the passes directly, so preserve the
                    // same per-item lifetime explicitly.
                    m_RP.m_RendPass = 0;
                    m_RP.m_pFogVolume = CV_r_VolumetricFog && fogIndex > 0 &&
                        fogIndex < m_RP.m_FogVolumes.Num() ?
                        &m_RP.m_FogVolumes[fogIndex] : nullptr;
                    if (shader && (shader->m_Flags & EF_FOGSHADER) && !m_RP.m_pFogVolume)
                        continue;
                    m_activeStateShaderState = shaderState ? shaderState->m_State : nullptr;
                    if (m_activeStateShaderState)
                    {
                        const SEfState& stateShader = *m_activeStateShaderState;
                        UCol color; color.dcolor = 0;
                        if (stateShader.m_Flags & ESF_RGBGEN)
                        {
                            m_RP.m_FlagsPerFlush |= RBSI_RGBGEN;
                            if (stateShader.m_eEvalRGB == eERGB_Fixed)
                                for (int channel = 0; channel < 4; ++channel)
                                    color.bcolor[channel] = stateShader.m_FixedColor[channel];
                        }
                        if (stateShader.m_Flags & ESF_ALPHAGEN)
                        {
                            m_RP.m_FlagsPerFlush |= RBSI_ALPHAGEN;
                            if (stateShader.m_eEvalAlpha == eEALPHA_Fixed)
                                color.bcolor[3] = stateShader.m_FixedColor[3];
                        }
                        if (stateShader.m_Flags & (ESF_RGBGEN | ESF_ALPHAGEN))
                        {
                            Exchange(color.bcolor[0], color.bcolor[3]);
                            m_RP.m_NeedGlobalColor = color;
                        }
                    }
                    const bool newStencilBatch = shaderState != previousStateShader ||
                        shader != previousStencilShader || resources != previousStencilResources ||
                        objectIndex != previousStencilObject || fogIndex != previousStencilFog;
                    if (newStencilBatch && m_activeStateShaderState)
                    {
                        if (m_activeStateShaderState->m_bClearStencil)
                        {
                            // EF_SetStateShaderState disables overdraw when
                            // stencil shadows claim the same attachment.
                            if (CV_r_measureoverdraw)
                            {
                                if (iLog) iLog->Log("Stencil shadows and overdraw measurement are mutually exclusive");
                                CV_r_measureoverdraw = 0;
                            }
                            if (!m_frameRenderer->QueueStockClearStencil())
                                m_frameRenderer->RequirePanelFallback();
                        }
                    }
                    // EF_SetStateShaderState reapplies stencil for every
                    // flush. Pointer equality does not imply that a previous
                    // render element left the packed state/ref/mask intact.
                    if (m_activeStateShaderState && m_activeStateShaderState->m_Stencil)
                    {
                        // The scene pipeline decoder consumes OpenGL's
                        // packed FSS compare/op bits directly and carries
                        // the reference and mask as dynamic stencil state.
                        m_CurStencilState = m_activeStateShaderState->m_Stencil->m_State;
                        m_CurStencRef = m_activeStateShaderState->m_Stencil->m_FuncRef;
                        m_CurStencMask = m_activeStateShaderState->m_Stencil->m_FuncMask;
                    }
                    previousStateShader = shaderState;
                    previousStencilShader = shader;
                    previousStencilResources = resources;
                    previousStencilObject = objectIndex;
                    previousStencilFog = fogIndex;

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
                        CaptureStockLeafStatus(item->Item, shader, "prepare-rejected");
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
                        PrepareStockFlushState(renderObject, shader);
                        CaptureStockLeafStatus(item->Item, shader,
                            techniqueIndex < 0 ? "no-technique" : "selected");
                        if (m_auditFrameActive)
                            CryLogAlways("Vulkan audit SELECT_TECHNIQUE frame=%d list=%d index=%d shader=%s selected=%d candidates=%d opacity=%.3f activeLights=%d fog=%p",
                                GetFrameID(), list, itemIndex, shader->m_Name.c_str(), techniqueIndex,
                                shader->m_HWTechniques.Num(), m_RP.m_fCurOpacity,
                                m_RP.m_NumActiveDLights, m_RP.m_pFogVolume);
                        if (techniqueIndex >= 0 && techniqueIndex < shader->m_HWTechniques.Num())
                        {
                            SShaderTechnique* technique = shader->m_HWTechniques[techniqueIndex];
                            m_RP.m_pCurTechnique = technique;
                            m_RP.m_fCurOpacity = resources && !resources->m_AlphaRef &&
                                !(shader->m_Flags2 & EF2_IGNORERESOURCESTATES) ?
                                resources->m_Opacity : 1.0f;
                            // EF_SelectHWTechnique runs during EF_Start, before
                            // EF_FlushShader applies resource opacity. That
                            // resource step clears only the cached LIGHTPASS
                            // flag before hardware pass dispatch, not the
                            // object's flag used by technique selection.
                            if (m_RP.m_fCurOpacity != 1.0f)
                                m_RP.m_ObjFlags &= ~FOB_LIGHTPASS;
                            bool submittedTranslatedTechniquePass = false;
                            int nextShadowCaster = -1;
                            bool inPerLightPassGroup = false;
                            int perLightPassGroupStart = -1;
                            bool perLightNoBumpBreak[32]{};
                            bool perLightPassTerminated[32]{};
                            const int cull = technique->m_eCull == static_cast<ECull>(-1) ?
                                MapStockCullMode(shader->m_eCull) :
                                MapStockCullMode(technique->m_eCull);
                            for (int passIndex = 0; passIndex < technique->m_Passes.Num(); ++passIndex)
                            {
                                SShaderPassHW* pass = &technique->m_Passes[passIndex];
                                // EF_DrawGeneralPasses skips ordinary material
                                // draws for FOB_FOGPASS. Submit one General
                                // pass's geometry below for the volume overlay,
                                // without dispatching its light/shadow passes.
                                if ((m_RP.m_ObjFlags & FOB_FOGPASS) &&
                                    pass->m_ePassType != eSHP_General)
                                    continue;
                                const bool perLightPass =
                                    pass->m_ePassType == eSHP_Light ||
                                    pass->m_ePassType == eSHP_DiffuseLight ||
                                    pass->m_ePassType == eSHP_SpecularLight;
                                if (!perLightPass)
                                    inPerLightPassGroup = false;
                                else if (!inPerLightPassGroup)
                                {
                                    memset(perLightNoBumpBreak, 0, sizeof(perLightNoBumpBreak));
                                    memset(perLightPassTerminated, 0, sizeof(perLightPassTerminated));
                                    inPerLightPassGroup = true;
                                    perLightPassGroupStart = passIndex;
                                }
                                if (pass->m_ePassType == eSHP_General)
                                {
                                    // GL::EF_DrawGeneralPasses omits the base
                                    // material on separately queued light objects.
                                    // Those objects can lack the original LM stream;
                                    // replaying General overwrites the lit base.
                                    if ((m_RP.m_ObjFlags & FOB_LIGHTPASS) &&
                                        (shader->m_Flags & EF_USELIGHTS))
                                    {
                                        // This is an intentional skip, so do not
                                        // invoke the fixed-function fallback below.
                                        submittedTranslatedTechniquePass = true;
                                        continue;
                                    }
                                    const bool firstOpaquePass = m_RP.m_RendPass == 0 &&
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
                                            if (!light || !(light->m_Flags &
                                                (DLF_DIRECTIONAL | DLF_POINT | DLF_PROJECT)))
                                                continue;
                                            const int passLightTypes =
                                                pass->m_LightFlags & DLF_LIGHTTYPE_MASK;
                                            if (passLightTypes &&
                                                !(passLightTypes &
                                                  (light->m_Flags & DLF_LIGHTTYPE_MASK)))
                                                continue;
                                            if ((pass->m_LMFlags & LMF_IGNOREPROJLIGHTS) &&
                                                (light->m_Flags & DLF_PROJECT))
                                                continue;
                                            if (hasObjectLightmap && (light->m_Flags & DLF_LM) &&
                                                (pass->m_LMFlags & LMF_NOSPECULAR))
                                                continue;
                                            matchingLightMask |= 1u << lightIndex;
                                            if (hasObjectLightmap &&
                                                (light->m_Flags & DLF_LM))
                                            {
                                                const uint8_t encodedLightId =
                                                    static_cast<uint8_t>(light->m_Id + 1);
                                                for (int occlusionIndex = 0; occlusionIndex < 4;
                                                     ++occlusionIndex)
                                                {
                                                    if (renderObject->m_OcclLights[occlusionIndex] ==
                                                        encodedLightId)
                                                    {
                                                        hasOccludedMatchingLight = true;
                                                        break;
                                                    }
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
                                        !(m_RP.m_ObjFlags & FOB_LIGHTPASS) &&
                                        (!(pass->m_LMFlags & LMF_HASDOT3LM) ||
                                         (resources && renderObject->m_nLMDirId));
                                    if ((matchingLightMask || hasAmbientLightPass) && occlusionPassEligible)
                                    {
                                        m_RP.m_DynLMask = matchingLightMask;
                                        const bool firstOpaquePass = m_RP.m_RendPass == 0 &&
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
                                    // EF_DrawLightPasses performs this legacy
                                    // DLF_LM diffuse rejection once, using only
                                    // the first pass in the light-pass range.
                                    // It does not reject every later DiffuseLight
                                    // pass based on any generic lightmap.
                                    const bool hasOpenGLDiffuseLightSuppressionMap =
                                        (resources && resources->m_Textures[EFTT_LIGHTMAP_DIR]) ||
                                        renderObject->m_nLMId;
                                    for (int lightIndex = 0;
                                         lightIndex < m_RP.m_DLights[lightLevel].Num() && lightIndex < 32;
                                         ++lightIndex)
                                    {
                                        if (!(itemLightMask & (1u << lightIndex)))
                                            continue;
                                        CDLight* light = m_RP.m_DLights[lightLevel][lightIndex];
                                        if (!light || perLightPassTerminated[lightIndex])
                                            continue;
                                        if ((light->m_Flags & DLF_LM) &&
                                            perLightPassGroupStart >= 0 &&
                                            technique->m_Passes[perLightPassGroupStart].m_ePassType ==
                                                eSHP_DiffuseLight &&
                                            hasOpenGLDiffuseLightSuppressionMap)
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
                                        if (passLightTypes && (pass->m_LightFlags & DLF_LM))
                                        {
                                            if (!(light->m_Flags & DLF_LM) ||
                                                !hasOpenGLDiffuseLightSuppressionMap)
                                                continue;
                                            if (light->m_SpecColor.r <= 0.01f &&
                                                light->m_SpecColor.g <= 0.01f &&
                                                light->m_SpecColor.b <= 0.01f)
                                                continue;
                                        }
                                        if (passLightTypes &&
                                            (pass->m_LMFlags & LMF_USEOCCLUSIONMAP) &&
                                            *reinterpret_cast<const int*>(renderObject->m_OcclLights) == 0)
                                            continue;
                                        if (pass->m_LMFlags & LMF_NOBUMP)
                                        {
                                            if (resources &&
                                                (!resources->m_Textures[EFTT_BUMP] ||
                                                 !(resources->m_Textures[EFTT_BUMP]->m_TU.m_nFlags & FTU_NOBUMP)))
                                                continue;
                                            perLightNoBumpBreak[lightIndex] = true;
                                        }
                                        else if (perLightNoBumpBreak[lightIndex])
                                        {
                                            perLightPassTerminated[lightIndex] = true;
                                            continue;
                                        }
                                        if (pass->m_LMFlags & LMF_DISABLE)
                                            continue;
                                        if (pass->m_ePassType == eSHP_SpecularLight &&
                                            light->m_SpecColor.r == 0.0f &&
                                            light->m_SpecColor.g == 0.0f &&
                                            light->m_SpecColor.b == 0.0f)
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
                                        const bool firstOpaquePass = m_RP.m_RendPass == 0 &&
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
                                else if (pass->m_ePassType == eSHP_Shadow)
                                {
                                    if ((m_RP.m_ObjFlags & FOB_LIGHTPASS) && (shader->m_Flags & EF_USELIGHTS))
                                        continue;
                                    // Port the stock per-caster shadow-pass
                                    // loop. ETC_ShadowMap addresses the caster
                                    // at m_nCurStartCaster plus the texture
                                    // stage, while SetupShadowOnlyPass builds
                                    // that receiver's light projection.
                                    list2<ShadowMapLightSourceInstance>* const shadowCasters =
                                        renderObject ? static_cast<list2<ShadowMapLightSourceInstance>*>(
                                            renderObject->m_pShadowCasters) : nullptr;
                                    if (!shadowCasters || !shadowCasters->Count())
                                        continue;

                                    int firstCaster = 0;
                                    if (shader->m_eSort != eS_TerrainShadowPass &&
                                        (!CV_r_selfshadow ||
                                         !(m_Features & RFT_SHADOWMAP_SELFSHADOW)))
                                    {
                                        ShadowMapLightSourceInstance& caster =
                                            shadowCasters->GetAt(0);
                                        ShadowMapFrustum* frustum = caster.m_pLS ?
                                            caster.m_pLS->GetShadowMapFrustum() : nullptr;
                                        if (frustum && frustum->pOwner == caster.m_pReceiver &&
                                            !(frustum->dwFlags & SMFF_ACTIVE_SHADOW_MAP))
                                            firstCaster = 1;
                                    }

                                    uint32_t samplesPerPass = 1;
                                    if (nextShadowCaster >= 0) firstCaster = crymax(firstCaster, nextShadowCaster);
                                    if (pass->m_LMFlags & LMF_SAMPLES)
                                    {
                                        if (pass->m_LMFlags & LMF_4SAMPLES) samplesPerPass = 4;
                                        else if (pass->m_LMFlags & LMF_3SAMPLES) samplesPerPass = 3;
                                        else if (pass->m_LMFlags & LMF_2SAMPLES) samplesPerPass = 2;
                                    }
                                    const int savedCasterStart = m_RP.m_nCurStartCaster;
                                    const uint32_t savedFlushFlags = m_RP.m_FlagsPerFlush;
                                    const uint32_t savedLightMask = m_RP.m_DynLMask;
                                    CDLight* const savedLight = m_RP.m_pCurLight;
                                    const int savedLightId = m_RP.m_nCurLight;
                                    for (int casterIndex = firstCaster;
                                         casterIndex + static_cast<int>(samplesPerPass) <=
                                             shadowCasters->Count();
                                         casterIndex += static_cast<int>(samplesPerPass))
                                    {
                                        ShadowMapLightSourceInstance& caster =
                                            shadowCasters->GetAt(casterIndex);
                                        if (!caster.m_pLS ||
                                            !caster.m_pLS->GetShadowMapFrustum())
                                            continue;

                                        CDLight* matchingLight = nullptr;
                                        int matchingLightIndex = -1;
                                        const int lightLevel = SRendItem::m_RecurseLevel;
                                        if (lightLevel >= 0 && lightLevel < 8)
                                        {
                                            for (int lightIndex = 0;
                                                 lightIndex < m_RP.m_DLights[lightLevel].Num() &&
                                                 lightIndex < 32;
                                                 ++lightIndex)
                                            {
                                                if (!(m_RP.m_DynLMask & (1u << lightIndex)))
                                                    continue;
                                                CDLight* light = m_RP.m_DLights[lightLevel][lightIndex];
                                                if (!light || light->m_Id != caster.m_pLS->nDLightId)
                                                    continue;
                                                const int requiredTypes =
                                                    pass->m_LightFlags & DLF_LIGHTTYPE_MASK;
                                                if (requiredTypes &&
                                                    !(requiredTypes &
                                                      (light->m_Flags & DLF_LIGHTTYPE_MASK)))
                                                    continue;
                                                matchingLight = light;
                                                matchingLightIndex = lightIndex;
                                                break;
                                            }
                                        }
                                        if ((pass->m_LightFlags & DLF_LIGHTTYPE_MASK) &&
                                            !matchingLight)
                                            continue;
                                        // OpenGL narrows the caster list to the
                                        // current light before EF_DrawShadowPasses.
                                        // Vulkan keeps the object's full list,
                                        // so apply that light ownership filter
                                        // explicitly for every light pass.
                                        if (savedLightMask && !matchingLight)
                                            continue;

                                        m_RP.m_nCurStartCaster = casterIndex;
                                        m_RP.m_FlagsPerFlush |= RBSI_SHADOWPASS;
                                        if (matchingLight)
                                        {
                                            m_RP.m_pCurLight = matchingLight;
                                            m_RP.m_nCurLight = matchingLight->m_Id;
                                            m_RP.m_DynLMask = 1u << matchingLightIndex;
                                        }
                                        const uint32_t shadowState =
                                            (m_RP.m_RendPass || (m_RP.m_ObjFlags & FOB_LIGHTPASS)) ?
                                                pass->m_SecondRenderState : pass->m_RenderState;
                                        DrawRenderItem(item->Item, shader, pass, resources,
                                                       shadowState, cull, true);
                                        nextShadowCaster = casterIndex + static_cast<int>(samplesPerPass);
                                        submittedTranslatedTechniquePass = true;
                                    }
                                    m_RP.m_nCurStartCaster = savedCasterStart;
                                    m_RP.m_FlagsPerFlush = savedFlushFlags;
                                    m_RP.m_DynLMask = savedLightMask;
                                    m_RP.m_pCurLight = savedLight;
                                    m_RP.m_nCurLight = savedLightId;
                                }
                                else if (pass->m_ePassType == eSHP_MultiShadows)
                                {
                                    // OpenGL treats MultiShadows as a delimiter
                                    // for a run of per-light Shadow/Light passes.
                                    // Execute that run against caster subsets
                                    // keyed by ShadowMapLightSourceInstance's
                                    // dynamic-light id, as EF_DrawMultiShadowPasses
                                    // does, instead of falling through to the
                                    // untranslated-shader fallback.
                                    int shadowStart = -1;
                                    int shadowEnd = -1;
                                    int lightmapShadowStart = -1;
                                    int lightmapShadowEnd = -1;
                                    int lightStart = -1;
                                    int lightEnd = -1;
                                    int groupEnd = passIndex;
                                    for (int groupPass = passIndex + 1;
                                         groupPass < technique->m_Passes.Num(); ++groupPass)
                                    {
                                        const EShaderPassType groupType =
                                            technique->m_Passes[groupPass].m_ePassType;
                                        if (groupType == eSHP_Shadow)
                                        {
                                            SShaderPassHW* shadowPass =
                                                &technique->m_Passes[groupPass];
                                            const bool lightmapShadow =
                                                (shadowPass->m_SecondRenderState & GS_BLEND_MASK) ==
                                                (GS_BLSRC_SRCALPHA | GS_BLDST_ONEMINUSSRCALPHA);
                                            if (lightmapShadow)
                                            {
                                                if (lightmapShadowStart < 0)
                                                    lightmapShadowStart = groupPass;
                                                lightmapShadowEnd = groupPass;
                                            }
                                            else
                                            {
                                                if (shadowStart < 0) shadowStart = groupPass;
                                                shadowEnd = groupPass;
                                            }
                                            groupEnd = groupPass;
                                        }
                                        else if (groupType == eSHP_Light ||
                                                 groupType == eSHP_DiffuseLight ||
                                                 groupType == eSHP_SpecularLight)
                                        {
                                            if (lightStart < 0) lightStart = groupPass;
                                            lightEnd = groupPass;
                                            groupEnd = groupPass;
                                        }
                                        else
                                            break;
                                    }

                                    list2<ShadowMapLightSourceInstance>* const allCasters =
                                        renderObject ? static_cast<list2<ShadowMapLightSourceInstance>*>(
                                            renderObject->m_pShadowCasters) : nullptr;
                                    const int lightLevel = SRendItem::m_RecurseLevel;
                                    const int lightCount = lightLevel >= 0 && lightLevel < 8 ?
                                        m_RP.m_NumActiveDLights : 0;
                                    const uint32_t savedLightMask = m_RP.m_DynLMask;
                                    const int savedCasterStart = m_RP.m_nCurStartCaster;
                                    const uint32_t savedFlushFlags = m_RP.m_FlagsPerFlush;
                                    CDLight* const savedLight = m_RP.m_pCurLight;
                                    const int savedLightId = m_RP.m_nCurLight;
                                    list2<ShadowMapLightSourceInstance> lightCasters[16];
                                    list2<ShadowMapLightSourceInstance> lightmapCasters;
                                    CDLight* lights[16]{};
                                    int lightIndices[16]{};
                                    int lightOrder[16]{};
                                    int groupedLights = 0;
                                    const bool hasDirectionalLightmap = resources &&
                                        renderObject && renderObject->m_nLMDirId > 0;

                                    if (allCasters && lightCount > 0)
                                    {
                                        const int maxLights = crymin(lightCount, 16);
                                        for (int activeLightIndex = 0;
                                             activeLightIndex < maxLights; ++activeLightIndex)
                                        {
                                            CDLight* light =
                                                m_RP.m_pActiveDLights[activeLightIndex];
                                            if (!light)
                                                continue;
                                            int dynamicLightIndex = -1;
                                            for (int lightIndex = 0;
                                                 lightIndex < m_RP.m_DLights[lightLevel].Num() &&
                                                 lightIndex < 32;
                                                 ++lightIndex)
                                            {
                                                if (m_RP.m_DLights[lightLevel][lightIndex] == light)
                                                {
                                                    dynamicLightIndex = lightIndex;
                                                    break;
                                                }
                                            }
                                            if (dynamicLightIndex < 0 ||
                                                !(savedLightMask & (1u << dynamicLightIndex)))
                                                continue;
                                            lights[groupedLights] = light;
                                            lightIndices[groupedLights] = dynamicLightIndex;
                                            lightOrder[groupedLights] = groupedLights;
                                            for (int casterIndex = 0;
                                                 casterIndex < allCasters->Count(); ++casterIndex)
                                            {
                                                ShadowMapLightSourceInstance& caster =
                                                    allCasters->GetAt(casterIndex);
                                                if (!caster.m_pLS ||
                                                    caster.m_pLS->nDLightId != light->m_Id)
                                                    continue;
                                                const bool lightmapCaster =
                                                    hasDirectionalLightmap &&
                                                    (light->m_Flags & DLF_LM);
                                                if (lightmapCaster)
                                                {
                                                    ShadowMapFrustum* frustum =
                                                        caster.m_pLS->GetShadowMapFrustum();
                                                    if (!frustum || !frustum->pOwner ||
                                                        !(frustum->pOwner->GetRndFlags() &
                                                          ERF_CASTSHADOWINTOLIGHTMAP))
                                                        lightmapCasters.Add(caster);
                                                }
                                                else
                                                    lightCasters[groupedLights].Add(caster);
                                            }
                                            ++groupedLights;
                                        }

                                        // Match OpenGL's SShadowLight sort: process
                                        // lights with the most caster frustums first.
                                        for (int i = 0; i < groupedLights; ++i)
                                            for (int j = i + 1; j < groupedLights; ++j)
                                                if (lightCasters[lightOrder[j]].Count() >
                                                    lightCasters[lightOrder[i]].Count())
                                                    std::swap(lightOrder[i], lightOrder[j]);
                                    }

                                    const auto drawShadowPassesForLight =
                                        [&](list2<ShadowMapLightSourceInstance>& casterList,
                                            int groupedLight, int firstPass, int lastPass)
                                    {
                                        if (firstPass < 0 || lastPass < firstPass ||
                                            casterList.IsEmpty() ||
                                            (groupedLight >= groupedLights))
                                            return;
                                        list2<ShadowMapLightSourceInstance>* const savedCasters =
                                            static_cast<list2<ShadowMapLightSourceInstance>*>(
                                                renderObject->m_pShadowCasters);
                                        renderObject->m_pShadowCasters = &casterList;
                                        m_RP.m_FlagsPerFlush |= RBSI_SHADOWPASS;
                                        const auto selectCasterLight =
                                            [&](ShadowMapLightSourceInstance& caster)
                                        {
                                            int selected = groupedLight;
                                            if (selected < 0)
                                            {
                                                for (int i = 0; i < groupedLights; ++i)
                                                    if (lights[i] && caster.m_pLS &&
                                                        lights[i]->m_Id == caster.m_pLS->nDLightId)
                                                    {
                                                        selected = i;
                                                        break;
                                                    }
                                            }
                                            if (selected >= 0 && selected < groupedLights)
                                            {
                                                m_RP.m_pCurLight = lights[selected];
                                                m_RP.m_nCurLight = lights[selected]->m_Id;
                                                m_RP.m_DynLMask = 1u << lightIndices[selected];
                                            }
                                            return selected;
                                        };
                                        int firstCaster = 0;
                                        if (shader->m_eSort != eS_TerrainShadowPass &&
                                            !CV_r_selfshadow &&
                                            !(m_Features & RFT_SHADOWMAP_SELFSHADOW))
                                        {
                                            ShadowMapLightSourceInstance& caster =
                                                casterList.GetAt(0);
                                            ShadowMapFrustum* frustum = caster.m_pLS ?
                                                caster.m_pLS->GetShadowMapFrustum() : nullptr;
                                            if (frustum && frustum->pOwner == caster.m_pReceiver &&
                                                !(frustum->dwFlags & SMFF_ACTIVE_SHADOW_MAP))
                                                firstCaster = 1;
                                        }
                                        int casterStep = 1;
                                        for (int casterIndex = firstCaster;
                                             casterIndex < casterList.Count();
                                             casterIndex += casterStep)
                                        {
                                            ShadowMapLightSourceInstance& caster =
                                                casterList.GetAt(casterIndex);
                                            if (!caster.m_pLS ||
                                                !caster.m_pLS->GetShadowMapFrustum())
                                                continue;
                                            const int casterLight = selectCasterLight(caster);
                                            if (casterLight < 0)
                                                continue;
                                            for (int shadowPassIndex = firstPass;
                                                 shadowPassIndex <= lastPass;
                                                 ++shadowPassIndex)
                                            {
                                                SShaderPassHW* shadowPass =
                                                    &technique->m_Passes[shadowPassIndex];
                                                if (shadowPass->m_ePassType != eSHP_Shadow)
                                                    continue;
                                                const int requiredTypes = shadowPass->m_LightFlags &
                                                    DLF_LIGHTTYPE_MASK;
                                                if (requiredTypes &&
                                                    !(requiredTypes &
                                                      (lights[casterLight]->m_Flags &
                                                       DLF_LIGHTTYPE_MASK)))
                                                    continue;
                                                uint32_t sampleCount = 1;
                                                if (shadowPass->m_LMFlags & LMF_SAMPLES)
                                                {
                                                    if (shadowPass->m_LMFlags & LMF_4SAMPLES)
                                                        sampleCount = 4;
                                                    else if (shadowPass->m_LMFlags & LMF_3SAMPLES)
                                                        sampleCount = 3;
                                                    else if (shadowPass->m_LMFlags & LMF_2SAMPLES)
                                                        sampleCount = 2;
                                                }
                                                if (casterIndex + static_cast<int>(sampleCount) >
                                                    casterList.Count())
                                                    continue;
                                                // EF_DrawShadowPasses updates the
                                                // outer caster stride while it
                                                // walks the pass list. The last
                                                // eligible sampled pass decides
                                                // the next caster index; taking
                                                // the maximum across passes skips
                                                // casters OpenGL actually draws.
                                                if (shadowPass->m_LMFlags & LMF_SAMPLES)
                                                    casterStep = static_cast<int>(sampleCount);
                                                bool sampleTypesMatch = true;
                                                if (requiredTypes)
                                                    for (uint32_t sample = 1;
                                                         sample < sampleCount; ++sample)
                                                    {
                                                        ShadowMapLightSourceInstance& sampleCaster =
                                                            casterList.GetAt(casterIndex + sample);
                                                        int sampleLight = selectCasterLight(sampleCaster);
                                                        if (sampleLight < 0 ||
                                                            !(requiredTypes &
                                                              (lights[sampleLight]->m_Flags &
                                                               DLF_LIGHTTYPE_MASK)))
                                                        {
                                                            sampleTypesMatch = false;
                                                            break;
                                                        }
                                                    }
                                                if (!sampleTypesMatch)
                                                {
                                                    selectCasterLight(caster);
                                                    continue;
                                                }
                                                selectCasterLight(caster);
                                                m_RP.m_nCurStartCaster = casterIndex;
                                                const uint32_t shadowState =
                                                    (m_RP.m_RendPass ||
                                                     (m_RP.m_ObjFlags & FOB_LIGHTPASS)) ?
                                                        shadowPass->m_SecondRenderState :
                                                        shadowPass->m_RenderState;
                                                DrawRenderItem(item->Item, shader, shadowPass,
                                                    resources, shadowState, cull, true);
                                                submittedTranslatedTechniquePass = true;
                                            }
                                        }
                                        renderObject->m_pShadowCasters = savedCasters;
                                    };

                                    const auto drawLightPassesForLight =
                                        [&](int groupedLight)
                                    {
                                        if (lightStart < 0 || lightEnd < lightStart ||
                                            groupedLight < 0 ||
                                            (m_RP.m_RendPass != 0 &&
                                             m_RP.m_fCurOpacity != 1.0f))
                                            return;
                                        // Match EF_DrawLightPasses' one early
                                        // test against its first pass and its
                                        // specific LM_DIR/object-lightmap test.
                                        const bool hasOpenGLDiffuseLightSuppressionMap =
                                            (resources &&
                                             resources->m_Textures[EFTT_LIGHTMAP_DIR]) ||
                                            renderObject->m_nLMId;
                                        if (technique->m_Passes[lightStart].m_ePassType ==
                                                eSHP_DiffuseLight &&
                                            (lights[groupedLight]->m_Flags & DLF_LM) &&
                                            hasOpenGLDiffuseLightSuppressionMap)
                                            return;
                                        m_RP.m_pCurLight = lights[groupedLight];
                                        m_RP.m_nCurLight = lights[groupedLight]->m_Id;
                                        m_RP.m_DynLMask = 1u << lightIndices[groupedLight];
                                        bool breakAfterNoBump = false;
                                        for (int lightPassIndex = lightStart;
                                             lightPassIndex <= lightEnd; ++lightPassIndex)
                                        {
                                            SShaderPassHW* lightPass =
                                                &technique->m_Passes[lightPassIndex];
                                            if (lightPass->m_ePassType != eSHP_Light &&
                                                lightPass->m_ePassType != eSHP_DiffuseLight &&
                                                lightPass->m_ePassType != eSHP_SpecularLight)
                                                continue;
                                            const int requiredTypes = lightPass->m_LightFlags &
                                                DLF_LIGHTTYPE_MASK;
                                            if (requiredTypes &&
                                                !(requiredTypes &
                                                  (lights[groupedLight]->m_Flags &
                                                   DLF_LIGHTTYPE_MASK)))
                                                continue;
                                            if ((lightPass->m_LMFlags & LMF_IGNOREPROJLIGHTS) &&
                                                (lights[groupedLight]->m_Flags & DLF_PROJECT))
                                                continue;
                                            if (requiredTypes &&
                                                (lightPass->m_LMFlags & LMF_USEOCCLUSIONMAP) &&
                                                *reinterpret_cast<const int*>(
                                                    renderObject->m_OcclLights) == 0)
                                                continue;
                                            if (requiredTypes &&
                                                (lightPass->m_LightFlags & DLF_LM))
                                            {
                                                if (!(lights[groupedLight]->m_Flags & DLF_LM) ||
                                                    !hasOpenGLDiffuseLightSuppressionMap ||
                                                    (lights[groupedLight]->m_SpecColor.r <= 0.01f &&
                                                     lights[groupedLight]->m_SpecColor.g <= 0.01f &&
                                                     lights[groupedLight]->m_SpecColor.b <= 0.01f))
                                                    continue;
                                            }
                                            if (lightPass->m_LMFlags & LMF_NOBUMP)
                                            {
                                                if (resources &&
                                                    (!resources->m_Textures[EFTT_BUMP] ||
                                                     !(resources->m_Textures[EFTT_BUMP]->m_TU.m_nFlags &
                                                       FTU_NOBUMP)))
                                                    continue;
                                                breakAfterNoBump = true;
                                            }
                                            else if (breakAfterNoBump)
                                                break;
                                            if (lightPass->m_LMFlags & LMF_DISABLE)
                                                continue;
                                            if (lightPass->m_ePassType == eSHP_SpecularLight &&
                                                lights[groupedLight]->m_SpecColor.r == 0.0f &&
                                                lights[groupedLight]->m_SpecColor.g == 0.0f &&
                                                lights[groupedLight]->m_SpecColor.b == 0.0f)
                                                continue;
                                            SLightIndicies* const previousLightIndices =
                                                m_RP.m_pCurLightIndices;
                                            m_RP.m_pCurLightIndices = &m_RP.m_FakeLightIndices;
                                            if (CullGeometryForLightsEnabled() &&
                                                EF_IsOnlyLightPass(lightPass) &&
                                                item->Item->mfGetType() == eDATA_OcLeaf)
                                            {
                                                m_RP.m_pCurLightIndices =
                                                    static_cast<CREOcLeaf*>(item->Item)->
                                                        mfGetIndiciesForLight(lights[groupedLight]);
                                                if (!m_RP.m_pCurLightIndices)
                                                {
                                                    m_RP.m_pCurLightIndices = previousLightIndices;
                                                    continue;
                                                }
                                            }
                                            const uint32_t lightState =
                                                (m_RP.m_RendPass ||
                                                 (m_RP.m_ObjFlags & FOB_LIGHTPASS)) ?
                                                    lightPass->m_SecondRenderState :
                                                    lightPass->m_RenderState;
                                            DrawRenderItem(item->Item, shader, lightPass,
                                                resources, lightState, cull, true);
                                            m_RP.m_pCurLightIndices = previousLightIndices;
                                            submittedTranslatedTechniquePass = true;
                                        }
                                    };

                                    if (allCasters)
                                    {
                                        if (!lightmapCasters.IsEmpty() &&
                                            lightmapShadowStart >= 0)
                                            drawShadowPassesForLight(lightmapCasters, -1,
                                                lightmapShadowStart, lightmapShadowEnd);
                                        for (int groupedLight = 0;
                                             groupedLight < groupedLights; ++groupedLight)
                                        {
                                            const int sortedLight = lightOrder[groupedLight];
                                            drawShadowPassesForLight(lightCasters[sortedLight],
                                                sortedLight,
                                                shadowStart, shadowEnd);
                                            drawLightPassesForLight(sortedLight);
                                        }
                                    }
                                    m_RP.m_pCurObject->m_pShadowCasters = allCasters;
                                    m_RP.m_DynLMask = savedLightMask;
                                    m_RP.m_nCurStartCaster = savedCasterStart;
                                    m_RP.m_FlagsPerFlush = savedFlushFlags;
                                    m_RP.m_pCurLight = savedLight;
                                    m_RP.m_nCurLight = savedLightId;
                                    passIndex = groupEnd;
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
                                    const bool firstOpaquePass = m_RP.m_RendPass == 0 &&
                                        !(m_RP.m_pCurObject->m_ObjFlags & FOB_LIGHTPASS);
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
                        PrepareStockFlushState(renderObject, shader);
                        m_RP.m_fCurOpacity = resources && !resources->m_AlphaRef &&
                            !(shader->m_Flags2 & EF2_IGNORERESOURCESTATES) ?
                            resources->m_Opacity : 1.0f;
                        for (int passIndex = 0; passIndex < shader->m_Passes.Num(); ++passIndex)
                        {
                            SShaderPass* pass = &shader->m_Passes[passIndex];
                            const bool firstOpaquePass = m_RP.m_RendPass == 0 &&
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
                UpdateStockNearestCamera(false);
                if (m_RP.m_PersFlags & RBPF_SETCLIPPLANE)
                {
                    EF_SetClipPlane(false, nullptr, false);
                    m_RP.m_PersFlags &= ~RBPF_SETCLIPPLANE;
                }
                m_frameRenderer->SetStockDepthRange(m_RP.m_fMinDepthRange,
                                                    m_RP.m_fMaxDepthRange);
                m_RP.m_PersFlags &= ~(RBPF_MATRIXNOTLOADED | RBPF_PS1NEEDSET |
                    RBPF_PS2NEEDSET | RBPF_TSNEEDSET | RBPF_VSNEEDSET);
                // CGLTexMan::BindNULL(1) disables every higher texture target
                // and clears its bind cache, while retaining combiner state.
                for (int stage = 1; stage < 8; ++stage)
                    m_stageTextureIds[stage] = 0;
                if (m_TexMan) m_TexMan->m_nCurStages = 1;
                SelectTMU(0);
                SetScissor(0, 0, 0, 0);
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
#if defined(__ANDROID__)
        if (!m_collectingShadowCasters && recurseLevel == 0 && m_sceneTrace)
        {
            fclose(m_sceneTrace);
            m_sceneTrace = nullptr;
        }
#endif
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

        // LeafBuffer::AddRenderElements submits only the first material when
        // a client texture is bound. Terrain sectors and OutdoorWaterCircle
        // use that path: their one item represents every strip in the buffer,
        // as in GL_VertBuffer's multi-strip submission callback. Preserve
        // separate strip draws so adjoining ranges are never joined together.
        if (primitiveType == R_PRIMV_MULTI_STRIPS && m_RP.m_pRE &&
            m_RP.m_pRE->mfGetType() == eDATA_OcLeaf)
        {
            CREOcLeaf* element = static_cast<CREOcLeaf*>(m_RP.m_pRE);
            CLeafBuffer* leaf = element->m_pBuffer;
            list2<CMatInfo>* materials = m_RP.m_pRE->mfGetMatInfoList();
            if (leaf && leaf->m_nClientTextureBindID && materials)
            {
                const int indexBase = firstIndex -
                    (materialInfo ? materialInfo->nFirstIndexId : 0);
                for (int strip = 0; strip < materials->Count(); ++strip)
                {
                    CMatInfo* material = materials->Get(strip);
                    if (material->nNumIndices < 3)
                        continue;
                    DrawBuffer(vertices, indices, material->nNumIndices,
                               indexBase + material->nFirstIndexId,
                               R_PRIMV_TRIANGLE_STRIP, flags,
                               material->nNumVerts, material);
                }
                return;
            }
        }

        // Buffers without a shared client texture enqueue each chunk itself.
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
        float shadowMapTransforms[8][16]{};
        uint32_t shadowMapStageMask = 0;
        bool textureStage1UsesTexCoord1 = true;
        CryVR::VulkanStockTextureStage textureStage2{};
        const CryVR::VulkanStockTextureStage* textureStage2Ptr = nullptr;
        CryVR::VulkanStockTextureStage textureStage3{};
        const CryVR::VulkanStockTextureStage* textureStage3Ptr = nullptr;
        CryVR::VulkanStockTextureStage textureStages4To7[4]{};
        const CryVR::VulkanStockTextureStage* textureStages4To7Ptr = nullptr;
        float textureLodBias[2] = { m_stageLodBias[0], m_stageLodBias[1] };
        float normalMapLodBias = 0.0f;
        int colorOps[2] = { eCO_MODULATE, eCO_MODULATE };
        int alphaOps[2] = { eCO_MODULATE, eCO_MODULATE };
        uint32_t colorArgs[2] = { DEF_TEXARG0, DEF_TEXARG1 };
        uint32_t alphaArgs[2] = { DEF_TEXARG0, DEF_TEXARG1 };
        float textureMatrices[2][16] = {};
        CryVR::VulkanStockLinearTexgen linearTexgen[8]{};
        // Keep scratch capacities across draws; nesting gets a separate slot.
        struct ScratchDepthRelease
        {
            size_t& depth;
            ~ScratchDepthRelease() { --depth; }
        } scratchRelease{m_drawScratchDepth};
        const size_t scratchIndex = m_drawScratchDepth++;
        if (m_drawScratch.size() <= scratchIndex) m_drawScratch.emplace_back();
        DrawScratch& scratchStorage = m_drawScratch[scratchIndex];
        auto& generatedVertexData = scratchStorage.generatedVertices;
        auto& lightmapTexCoords = scratchStorage.lightmapCoordinates;
        generatedVertexData.clear();
        lightmapTexCoords.clear();
        float terrainProjectionRows[2][8][4]{};
        bool hasTerrainProjection = false;
        bool beamDeformActive = false;
        bool flareDeformActive = false;
        std::vector<struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F> flareVertexData;
        std::vector<uint16_t> flareIndexData;
        int lightmapStage = -1;
        bool explicitLightmapPointer = false;
        const SArrayPointer* textureCoordinatePointers[8]{};
        const SArrayPointer* normalPointer = nullptr;
        int textureCoordinatePointerOrder[8]{};
        int nextTextureCoordinatePointerOrder = 0;
        bool techniqueHasLightmap = false;
        bool techniqueHasDirectionalLightmap = false;
        if (m_RP.m_pCurTechnique)
        {
            if (m_RP.m_pShader)
            {
                for (int techniqueIndex = 0;
                     techniqueIndex < m_RP.m_pShader->m_HWTechniques.Num() &&
                     techniqueIndex < m_RP.m_pShader->m_HWConditions.Num(); ++techniqueIndex)
                {
                    if (m_RP.m_pShader->m_HWTechniques[techniqueIndex] == m_RP.m_pCurTechnique)
                    {
                        techniqueHasLightmap =
                            (m_RP.m_pShader->m_HWConditions[techniqueIndex].m_Flags &
                             SHCF_HASLM) != 0;
                        techniqueHasDirectionalLightmap =
                            (m_RP.m_pShader->m_HWConditions[techniqueIndex].m_Flags &
                             SHCF_HASDOT3LM) != 0;
                        break;
                    }
                }
            }
            for (int pointerIndex = 0;
                 pointerIndex < m_RP.m_pCurTechnique->m_Pointers.Num(); ++pointerIndex)
            {
                SArrayPointer* pointer = m_RP.m_pCurTechnique->m_Pointers[pointerIndex];
                if (pointer && pointer->eDst == eDstPointer_Normal)
                    normalPointer = pointer;
                if (pointer && pointer->eDst == eDstPointer_Tex0 &&
                    pointer->Stage >= 0 && pointer->Stage < 8)
                {
                    textureCoordinatePointers[pointer->Stage] = pointer;
                    textureCoordinatePointerOrder[pointer->Stage] = ++nextTextureCoordinatePointerOrder;
                }
            }
        }
        const bool hasMaterialLightmap = m_activeResources &&
            m_activeResources->m_Textures[EFTT_LIGHTMAP];
        const bool hasObjectDirectionalLightmap = m_RP.m_pCurObject &&
            m_RP.m_pCurObject->m_nLMDirId != 0;
        const bool hasMaterialDirectionalLightmap = m_activeResources &&
            m_activeResources->m_Textures[EFTT_LIGHTMAP_DIR];
        const bool hasMultiLightsDirectionalLightmap =
            hasObjectDirectionalLightmap || hasMaterialDirectionalLightmap;
        const bool hasObjectLightmap = m_RP.m_pCurObject &&
            m_RP.m_pCurObject->m_nLMId != 0;
        bool isFirstHardwareGeneralPass = false;
        if (m_activeHardwarePassType == eSHP_General && m_RP.m_pCurTechnique && m_activePass)
        {
            for (int passIndex = 0; passIndex < m_RP.m_pCurTechnique->m_Passes.Num(); ++passIndex)
            {
                SShaderPassHW* pass = &m_RP.m_pCurTechnique->m_Passes[passIndex];
                if (pass->m_ePassType != eSHP_General)
                    continue;
                isFirstHardwareGeneralPass = pass == m_activePass;
                break;
            }
        }
        // Stock CG techniques sample object lightmaps from their program and
        // therefore have no declared SShaderPassHW texture units or array
        // pointers. If the NULL-backed technique selection falls back to an
        // otherwise valid General pass, still attach the object's baked map
        // to that one base pass. OpenGL's EF_FlushHW/CGRCAmbientTempl does
        // this implicitly through the HasLM program variant.
        const bool shaderLightingDisabled = m_RP.m_pShader &&
            (m_RP.m_pShader->m_LMFlags & LMF_DISABLE);
        const SShaderPassHW* stockHardwarePass = m_activePass &&
            m_activeHardwarePassType != eSHP_MAX ?
            static_cast<SShaderPassHW*>(m_activePass) : nullptr;
        const StockProgramInfo& programInfo = ClassifyStockProgram(stockHardwarePass);
        const int stockWaterMode = programInfo.water;
        const bool stockAmbientTemplate = stockHardwarePass &&
            programInfo.ambient;
        const bool stockLightTemplate = stockHardwarePass &&
            programInfo.light;
        const bool stockParticleAmbient = stockHardwarePass &&
            programInfo.particle;
        const bool stockReceiverShadow = stockHardwarePass &&
            !FastAsciiCaseCompare(stockHardwarePass->m_StockFragmentProgram, "CGRCShadowTempl");
        const bool stockAmbientDecal = stockHardwarePass &&
            !FastAsciiCaseCompare(stockHardwarePass->m_StockFragmentProgram, "CGRCAmbient_Decal");
        const bool stockTerrainLayerBase = stockHardwarePass &&
            programInfo.terrainLayer &&
            !(stockHardwarePass->m_StockProgramMask & 0x10000000u);
        // OpenGL's TerrainShadowPass is a two-stage material program, not a
        // generic shadowed texture combine. Its fragment program writes the
        // terrain albedo/lighting to RGB and the inverse shadow comparison to
        // alpha; the declared SRC_ALPHA blend then overlays only shadowed
        // pixels. Keep this identity through the stock Vulkan draw path.
        const bool stockTerrainShadowProgram = stockHardwarePass &&
            programInfo.terrainShadow;
        if (stockAmbientTemplate)
        {
            techniqueHasLightmap = (stockHardwarePass->m_StockProgramMask & 0x02000000ull) != 0;
            techniqueHasDirectionalLightmap =
                (stockHardwarePass->m_StockProgramMask & 0x01000000ull) != 0;
        }
        const bool implicitHardwareLightmapBasePass = !shaderLightingDisabled && isFirstHardwareGeneralPass &&
            !stockAmbientTemplate &&
            !(stockHardwarePass && stockHardwarePass->m_StockFragmentProgram[0]) &&
            (hasObjectLightmap || hasMaterialLightmap);
        const uint32_t activeHardwareLightFlags =
            m_activeHardwarePassType != eSHP_MAX && m_activePass ?
                static_cast<SShaderPassHW*>(m_activePass)->m_LMFlags :
                (m_RP.m_pShader ? m_RP.m_pShader->m_LMFlags : 0);
        // DrawRenderItem increments m_RendPass before calling DrawBuffer,
        // unlike GL's EF_FlushHW which tests the initial pass at zero.
        const bool activeHardwareLightmapBasePass =
            !shaderLightingDisabled && ((m_activeHardwarePassType == eSHP_General &&
              (techniqueHasLightmap || techniqueHasDirectionalLightmap ||
               implicitHardwareLightmapBasePass)) ||
             (m_activeHardwarePassType == eSHP_MultiLights &&
              (activeHardwareLightFlags & LMF_HASAMBIENT))) &&
            !(activeHardwareLightFlags & LMF_DISABLE) &&
            !(m_RP.m_ObjFlags & FOB_LIGHTPASS) && m_RP.m_RendPass <= 1 &&
            (!(activeHardwareLightFlags & LMF_HASDOT3LM) ||
             hasMultiLightsDirectionalLightmap);
        // LMF_NOAMBIENT removes the ambient term, not the baked lightmap.
        const bool activeHardwareAmbientPass = activeHardwareLightmapBasePass &&
            !(activeHardwareLightFlags & LMF_NOAMBIENT);
        const bool directionalLightmapPass =
            activeHardwareLightmapBasePass &&
            (techniqueHasDirectionalLightmap || implicitHardwareLightmapBasePass) &&
            m_RP.m_pCurObject &&
            (m_RP.m_pCurObject->m_nLMId > 0 || hasMaterialLightmap) &&
            (hasObjectDirectionalLightmap || hasMaterialDirectionalLightmap) &&
            m_activeResources && (stockAmbientTemplate || m_activeResources->m_Textures[EFTT_BUMP]);
        if (directionalLightmapPass)
            // AmbPassTempl orders the DOT3 samplers as diffuse, bump,
            // lightmap, and lightmap direction. UV set 1 belongs to stages 2/3.
            lightmapStage = 2;
        if (m_activePass &&
            m_activeHardwarePassType != eSHP_MAX)
        {
            SShaderPassHW* hardwarePass = static_cast<SShaderPassHW*>(m_activePass);
            for (int pointerIndex = 0; pointerIndex < hardwarePass->m_Pointers.Num(); ++pointerIndex)
            {
                SArrayPointer* pointer = hardwarePass->m_Pointers[pointerIndex];
                if (pointer && pointer->eDst == eDstPointer_Normal)
                    normalPointer = pointer;
                if (pointer && pointer->eDst == eDstPointer_Tex0 &&
                    pointer->Stage >= 0 && pointer->Stage < 8)
                {
                    textureCoordinatePointers[pointer->Stage] = pointer;
                    textureCoordinatePointerOrder[pointer->Stage] = ++nextTextureCoordinatePointerOrder;
                }
            }
        }
        // A pass may replace a technique's TexLM binding with ordinary UVs
        // on the same destination stage. Resolve destinations before deciding
        // which stages consume the separate object lightmap stream.
        int lastLightmapPointerOrder = 0;
        for (int stage = 0; stage < 8; ++stage)
        {
            const SArrayPointer* pointer = textureCoordinatePointers[stage];
            if (pointer && pointer->ePT == eSrcPointer_TexLM &&
                textureCoordinatePointerOrder[stage] > lastLightmapPointerOrder)
            {
                lightmapStage = stage;
                explicitLightmapPointer = true;
                lastLightmapPointerOrder = textureCoordinatePointerOrder[stage];
            }
        }
        if (m_activePass && lightmapStage < 0)
            for (int stage = 0; stage < m_activePass->m_TUnits.Num() && stage < 8; ++stage)
                if (m_activePass->m_TUnits[stage].m_eGenTC == eGTC_LightMap ||
                    (m_activePass->m_TUnits[stage].m_TexPic &&
                     (m_activePass->m_TUnits[stage].m_TexPic->m_Bind == EFTT_LIGHTMAP ||
                      m_activePass->m_TUnits[stage].m_TexPic->m_Bind == EFTT_LIGHTMAP_DIR ||
                      m_activePass->m_TUnits[stage].m_TexPic->m_Bind == EFTT_OCCLUSION)))
                {
                    lightmapStage = stage;
                    break;
                }
        // OpenGL's programmable hardware techniques keep their samplers in
        // the shader program, so SShaderPassHW::m_TUnits and its pointer list
        // are empty even when the object's baked lightmap is part of the
        // material. Translate that missing base-pass sampler explicitly;
        // never attach it to dynamic light/specular passes.
        const bool synthesizeHardwareBaseLightmap = lightmapStage < 0 &&
            activeHardwareLightmapBasePass && m_activePass &&
            m_activePass->m_TUnits.Num() == 0 && m_RP.m_pCurObject &&
            (m_RP.m_pCurObject->m_nLMId > 0 || hasMaterialLightmap);
        if (m_auditFrameActive && m_activeHardwarePassType == eSHP_General &&
            m_RP.m_pCurObject && m_RP.m_pCurObject->m_nLMId > 0)
            CryLogAlways("Vulkan audit LIGHTMAP_DECISION frame=%d list=%d item=%d pass=%p type=%d technique=%p firstGeneral=%d objectLm=%d objectDirLm=%d materialLm=%d techniqueLm=%d techniqueDot3=%d base=%d stage=%d units=%d rendPass=%d objFlags=0x%x lmFlags=0x%x synth=%d",
                GetFrameID(), m_auditCurrentList, m_auditCurrentItem, m_activePass,
                static_cast<int>(m_activeHardwarePassType), m_RP.m_pCurTechnique,
                isFirstHardwareGeneralPass ? 1 : 0, m_RP.m_pCurObject->m_nLMId,
                m_RP.m_pCurObject->m_nLMDirId, hasMaterialLightmap ? 1 : 0,
                techniqueHasLightmap ? 1 : 0, techniqueHasDirectionalLightmap ? 1 : 0,
                activeHardwareLightmapBasePass ? 1 : 0, lightmapStage,
                m_activePass ? m_activePass->m_TUnits.Num() : -1, m_RP.m_RendPass,
                m_RP.m_ObjFlags, activeHardwareLightFlags,
                synthesizeHardwareBaseLightmap ? 1 : 0);
        if (synthesizeHardwareBaseLightmap)
            lightmapStage = 1;
        // Match OpenGL: m_nLMId chooses the image only for a texture stage
        // that the selected pass already declares as a lightmap stage. An
        // object having a lightmap does not add a new stage to every pass.
        const bool hasLightmapTexCoordSource = lightmapStage >= 0;
        if (hasLightmapTexCoordSource)
        {
            CCObject* object = m_RP.m_pCurObject;
            CLeafBuffer* lmBuffer = object ? object->m_pLMTCBufferO : nullptr;
            // CREOcLeaf::mfGetPointer(TexLM) binds the primary LM stream.
            // Its CPU shadow is a fallback when the video stream has no
            // retained data; it is not the only valid source.
            CVertexBuffer* lmVertices = lmBuffer ? lmBuffer->m_pVertexBuffer : nullptr;
            if (!lmVertices || !lmVertices->m_VS[VSF_GENERAL].m_VData ||
                lmVertices->m_vertexformat != VERTEX_FORMAT_TEX2F)
                lmVertices = lmBuffer ? lmBuffer->m_pSecVertBuffer : nullptr;
            const int lmVertexCount = lmVertices == (lmBuffer ? lmBuffer->m_pVertexBuffer : nullptr) && lmVertices ?
                lmVertices->m_NumVerts : (lmBuffer ? lmBuffer->m_SecVertCount : 0);
            if (lightmapStage > 7)
            {
                m_frameRenderer->RequirePanelFallback();
                return;
            }
            if (lmVertices && lmVertices->m_VS[VSF_GENERAL].m_VData &&
                lmVertices->m_vertexformat == VERTEX_FORMAT_TEX2F && lmVertexCount > 0)
            {
                const struct_VERTEX_FORMAT_TEX2F* source =
                    static_cast<const struct_VERTEX_FORMAT_TEX2F*>(
                        lmVertices->m_VS[VSF_GENERAL].m_VData);
                // GL binds the LM stream independently of the main VBO. A
                // material chunk need not reference every vertex in that VBO.
                const uint16_t* chunkIndices =
                    static_cast<const uint16_t*>(indices->m_VData) + firstIndex;
                for (int index = 0; lmVertexCount < vertices->m_NumVerts &&
                     index < indexCount; ++index)
                    if (chunkIndices[index] >= lmVertexCount)
                    {
                        m_frameRenderer->RequirePanelFallback();
                        return;
                    }
                const int copiedVertices = crymin(vertices->m_NumVerts, lmVertexCount);
                if (copiedVertices == vertices->m_NumVerts)
                    lightmapTexCoords.Borrow(reinterpret_cast<float*>(lmVertices->m_VS[VSF_GENERAL].m_VData),
                        size_t(copiedVertices) * 2);
                else
                {
                    lightmapTexCoords.resize(static_cast<size_t>(vertices->m_NumVerts) * 2);
                    memcpy(lightmapTexCoords.data(), source, size_t(copiedVertices) * sizeof(float) * 2);
                }
                textureStage1UsesTexCoord1 = true;
            }
            else if (explicitLightmapPointer)
            {
                // CREOcLeaf::mfGetPointer(eSrcPointer_TexLM) returns the start
                // of the ordinary vertex stream when no separate LM buffer
                // exists. GL therefore reads position X/Y as its vec2 UV.
                textureStage1UsesTexCoord1 = false;
                CryVR::VulkanVertexFormat lmSourceFormat{};
                if (CryVR::GetVulkanVertexFormat(
                        static_cast<uint32_t>(vertices->m_vertexformat), lmSourceFormat))
                {
                    for (uint32_t attribute = 0; attribute < lmSourceFormat.attributeCount;
                         ++attribute)
                    {
                        const VkVertexInputAttributeDescription& position =
                            lmSourceFormat.attributes[attribute];
                        if (position.location != 0 ||
                            position.format != VK_FORMAT_R32G32B32_SFLOAT ||
                            position.offset + sizeof(float) * 2 > lmSourceFormat.stride)
                            continue;
                        const uint8_t* source = static_cast<const uint8_t*>(
                            vertices->m_VS[VSF_GENERAL].m_VData);
                        lightmapTexCoords.resize(static_cast<size_t>(vertices->m_NumVerts) * 2);
                        for (int vertex = 0; vertex < vertices->m_NumVerts; ++vertex)
                            memcpy(&lightmapTexCoords[static_cast<size_t>(vertex) * 2],
                                   source + static_cast<size_t>(vertex) * lmSourceFormat.stride +
                                       position.offset, sizeof(float) * 2);
                        textureStage1UsesTexCoord1 = true;
                        break;
                    }
                }
            }
            else if (vertices->m_vertexformat != 16)
                textureStage1UsesTexCoord1 = false;
        }
        const void* drawVertexData = vertices->m_VS[VSF_GENERAL].m_VData;
        CryVR::VulkanVertexFormat stockVertexFormat{};
        const bool stockVertexFormatValid = CryVR::GetVulkanVertexFormat(
            static_cast<uint32_t>(vertices->m_vertexformat), stockVertexFormat);
        uint32_t positionOffset = 0xffffffffu;
        uint32_t normalOffset = 0xffffffffu;
        uint32_t colorOffset = 0xffffffffu;
        uint32_t secondaryColorOffset = 0xffffffffu;
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
                else if (desc.location == 4) secondaryColorOffset = desc.offset;
                else if (desc.location == 5) uv1Offset = desc.offset;
            }
        }
        const SPipTangents* sourceTangentBasis = nullptr;
        const uint8_t* sourcePositionData = static_cast<const uint8_t*>(
            vertices->m_VS[VSF_GENERAL].m_VData);
        uint32_t sourcePositionStride = stockVertexFormat.stride;
        uint32_t sourcePositionOffset = positionOffset;
        CryVR::VulkanVertexFormat cpuSourceFormat = stockVertexFormat;
        if (m_RP.m_pRE && m_RP.m_pRE->mfGetType() == eDATA_OcLeaf)
        {
            CREOcLeaf* leafElement = static_cast<CREOcLeaf*>(m_RP.m_pRE);
            CLeafBuffer* leafBuffer = leafElement->m_pBuffer ?
                leafElement->m_pBuffer->GetVertexContainer() : nullptr;
            if (leafBuffer && leafBuffer->m_pSecVertBuffer &&
                leafBuffer->m_SecVertCount >= vertices->m_NumVerts &&
                leafBuffer->m_pSecVertBuffer->m_VS[VSF_GENERAL].m_VData)
            {
                CryVR::VulkanVertexFormat sourceFormat{};
                if (CryVR::GetVulkanVertexFormat(
                        leafBuffer->m_pSecVertBuffer->m_vertexformat, sourceFormat))
                    for (uint32_t attribute = 0; attribute < sourceFormat.attributeCount; ++attribute)
                    {
                        const VkVertexInputAttributeDescription& position = sourceFormat.attributes[attribute];
                        if (position.location == 0 && position.format == VK_FORMAT_R32G32B32_SFLOAT &&
                            position.offset + sizeof(float) * 3 <= sourceFormat.stride)
                        {
                            sourcePositionData = static_cast<const uint8_t*>(
                                leafBuffer->m_pSecVertBuffer->m_VS[VSF_GENERAL].m_VData);
                            sourcePositionStride = sourceFormat.stride;
                            sourcePositionOffset = position.offset;
                            cpuSourceFormat = sourceFormat;
                            break;
                        }
                    }
            }
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
                if (m_stockColorCacheFrame != m_RP.m_Frame)
                {
                    m_stockGeneratedColors.clear();
                    m_stockColorCacheFrame = m_RP.m_Frame;
                }
                const auto previous = m_stockGeneratedColors.find(vertices);
                if (colorOffset != 0xffffffffu && previous != m_stockGeneratedColors.end() &&
                    previous->second.vertexFormat == vertices->m_vertexformat &&
                    previous->second.colors.size() == static_cast<size_t>(vertices->m_NumVerts) * 4u)
                    for (int vertex = 0; vertex < vertices->m_NumVerts; ++vertex)
                        memcpy(generatedVertexData.data() + static_cast<size_t>(vertex) * stockVertexFormat.stride +
                            colorOffset, previous->second.colors.data() + static_cast<size_t>(vertex) * 4u, 4u);
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

        std::vector<const SDeform*>& stockDeforms = m_stockDeformScratch;
        stockDeforms.clear();
        if (m_activePass && m_RP.m_pShader && m_RP.m_pShader->m_Deforms)
            for (int index = 0; index < m_RP.m_pShader->m_Deforms->Num(); ++index)
                stockDeforms.push_back(&m_RP.m_pShader->m_Deforms->Get(index));
        if (m_activePass && (m_activeHardwarePassType == eSHP_General ||
             m_activeHardwarePassType == eSHP_Shadow ||
             m_activeHardwarePassType == eSHP_MultiShadows))
        {
            SShaderPassHW* deformPass = static_cast<SShaderPassHW*>(m_activePass);
            if (deformPass->m_Deforms)
                for (int index = 0; index < deformPass->m_Deforms->Num(); ++index)
                    stockDeforms.push_back(&deformPass->m_Deforms->Get(index));
        }
        // OpenGL applies SShader::m_Deforms to CPU mesh streams before UV
        // generation. Reproduce its common vertex/normal-based forms on a
        // private upload copy so shared leaf buffers remain untouched.
        if (!stockDeforms.empty())
        {
            const int deformFirstVertex = m_RP.m_pRE ? m_RP.m_FirstVertex : 0;
            const int deformVertexCount = m_RP.m_pRE ? m_RP.m_RendNumVerts : vertices->m_NumVerts;
            if (deformFirstVertex < 0 || deformFirstVertex > vertices->m_NumVerts ||
                deformVertexCount < 0 || deformVertexCount > vertices->m_NumVerts - deformFirstVertex)
            {
                m_frameRenderer->RequirePanelFallback();
                return;
            }
            const int deformEndVertex = deformFirstVertex + deformVertexCount;
            uint8_t* data = ensureVertexCopy();
            if (!data || positionOffset == 0xffffffffu)
                m_frameRenderer->RequirePanelFallback();
            else
            {
                for (int deformIndex = 0; deformIndex < static_cast<int>(stockDeforms.size()); ++deformIndex)
                {
                    const SDeform& deform = *stockDeforms[deformIndex];
                    // SEvalFuncs_C::VerticalWaveDeform is empty; only the
                    // render-element evaluator implements this operation.
                    if (deform.m_eType == eDT_VerticalWave && !m_RP.m_pRE)
                        continue;
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
                             followingIndex < static_cast<int>(stockDeforms.size()); ++followingIndex)
                        {
                            const EDeformType followingType =
                                stockDeforms[followingIndex]->m_eType;
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
                        for (int vertex = deformFirstVertex; vertex < deformEndVertex; ++vertex)
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
                    if (needsNormal && normalOffset == 0xffffffffu &&
                        !(m_RP.m_pRE && sourceTangentBasis))
                    {
                        m_frameRenderer->RequirePanelFallback();
                        continue;
                    }
                    if (deform.m_eType == eDT_Unknown)
                        // EF_Eval_DeformVerts logs an unknown type and leaves
                        // the current vertex stream unchanged.
                        continue;
                    for (int vertex = deformFirstVertex; vertex < deformEndVertex; ++vertex)
                    {
                        uint8_t* source = data + static_cast<size_t>(vertex) * stockVertexFormat.stride;
                        float position[3] = {};
                        float normal[3] = { 0.0f, 0.0f, 1.0f };
                        memcpy(position, source + positionOffset, sizeof(position));
                        if (needsNormal)
                        {
                            if (m_RP.m_pRE && sourceTangentBasis)
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
        for (int stage = 0; stage < 8; ++stage)
            shadowMapTransforms[stage][0] = shadowMapTransforms[stage][5] =
                shadowMapTransforms[stage][10] = shadowMapTransforms[stage][15] = 1.0f;

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
        const auto captureShadowStage = [&](int stage, const SShaderTexUnit* unit,
                                             int resolvedTextureId)
        {
            if (stage < 0 || stage >= 8)
                return;
            if (unit && unit->m_eGenTC == eGTC_ShadowMap &&
                resolvedTextureId >= 0x40000000)
            {
                memcpy(shadowMapTransforms[stage],
                       m_cEF.m_TempMatrices[stage][0].GetData(),
                       sizeof(shadowMapTransforms[stage]));
                shadowMapStageMask |= 1u << stage;
            }
            else
                shadowMapStageMask &= ~(1u << stage);
        };

        const auto generateLegacyTexCoords = [&](int stage, const SShaderTexUnit* unit,
                                                 const float* stageMatrix)
        {
            // CGVProgSimple_Plant_Bump copies the mesh UV to both samplers.
            // Fixed-function texgen is not executed by this vertex program.
            if (programInfo.plantsBump && stage < 2)
                return;
            if (flareDeformActive)
                return;
            // GL mfSetTexture resolves the image through pSTU but enables
            // hardware texgen from the pass unit (this), independently of
            // its software eGenTC mode.
            SGenTC* stageGenerator = unit ? unit->m_GTC : nullptr;
            if (m_activePass && stage < m_activePass->m_TUnits.Num())
                stageGenerator = m_activePass->m_TUnits[stage].m_GTC;
            if (!unit || (!unit->m_TexPic && !stageGenerator) ||
                (!stageGenerator && (unit->m_eGenTC == eGTC_NoFill || unit->m_eGenTC == eGTC_None ||
                unit->m_eGenTC == eGTC_Base || unit->m_eGenTC == eGTC_LightMap)))
                return;
            // OpenGL's eGTC_Quad CPU generator only fills coordinates when
            // there is no current render element. Leave stock leaf and other
            // render-element UVs untouched, as EF_Eval_TexGen does.
            if (!stageGenerator && unit->m_eGenTC == eGTC_Quad && m_RP.m_pRE)
                return;
            // SEvalFuncs_C::ETC_Projection is empty in the OpenGL reference.
            if (!stageGenerator && unit->m_eGenTC == eGTC_Projection && !m_RP.m_pRE)
                return;
            SGenTC_ObjectLinear* objectLinear = nullptr;
            SGenTC_EyeLinear* eyeLinear = nullptr;
            SGenTC_NormalMap* normalMap = nullptr;
            SGenTC_ReflectionMap* reflectionMap = nullptr;
            SGenTC_SphereMap* sphereMap = nullptr;
            TArray<SParam>* linearParameters = nullptr;
            int linearMask = 0;
            float linearPlanes[4][4] = {};
            if (stageGenerator)
            {
                if (stageGenerator->GetType() == eGTCType_ObjectLinear)
                {
                    objectLinear = static_cast<SGenTC_ObjectLinear*>(stageGenerator);
                    linearParameters = &objectLinear->m_Params;
                }
                else if (stageGenerator->GetType() == eGTCType_EyeLinear)
                {
                    eyeLinear = static_cast<SGenTC_EyeLinear*>(stageGenerator);
                    linearParameters = &eyeLinear->m_Params;
                }
                else if (stageGenerator->GetType() == eGTCType_NormalMap)
                    normalMap = static_cast<SGenTC_NormalMap*>(stageGenerator);
                else if (stageGenerator->GetType() == eGTCType_ReflectionMap)
                    reflectionMap = static_cast<SGenTC_ReflectionMap*>(stageGenerator);
                else if (stageGenerator->GetType() == eGTCType_SphereMap)
                    sphereMap = static_cast<SGenTC_SphereMap*>(stageGenerator);
                else
                {
                    m_frameRenderer->RequirePanelFallback();
                    return;
                }

                if (objectLinear || eyeLinear)
                {
                    // GLShaders.cpp resets the element parameter cursor
                    // before either linear generator reads its plane params.
                    if (m_RP.m_pRE)
                        m_RP.m_pRE->m_nCountCustomData = 0;
                    const int requestedMask = stageGenerator->m_Mask;
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
                        float* plane = (*linearParameters)[coordinate].mfGet();
                        memcpy(linearPlanes[coordinate], plane, sizeof(linearPlanes[coordinate]));
                    }
                    auto& captured = linearTexgen[stage];
                    captured.enabled = true;
                    captured.componentMask = static_cast<uint32_t>(linearMask);
                    memcpy(captured.planes, linearPlanes, sizeof(linearPlanes));
                    memcpy(captured.textureMatrix, stageMatrix, sizeof(captured.textureMatrix));
                    // Preserve original UVs; the fragment shader evaluates
                    // homogeneous planes independently for this TMU.
                    return;
                }
            }

            // Higher stages carry independent linear planes through the UBO.
            // Other generated modes must not overwrite the shared UV1 stream
            // or index the two legacy textureMatrices slots with stage >= 2.
            if (stage >= 2)
            {
                if (stageGenerator) m_frameRenderer->RequirePanelFallback();
                return;
            }
            const bool needsNormal = unit->m_eGenTC == eGTC_Environment ||
                unit->m_eGenTC == eGTC_SphereMap ||
                unit->m_eGenTC == eGTC_SphereMapEnvironment || normalMap ||
                reflectionMap || sphereMap;
            const bool usesTangentNormal =
                m_RP.m_pRE && (unit->m_eGenTC == eGTC_Environment ||
                unit->m_eGenTC == eGTC_SphereMap ||
                unit->m_eGenTC == eGTC_SphereMapEnvironment);
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
            const float* projection = unit->m_TexPic ? unit->m_TexPic->m_Matrix : nullptr;
            bool generatedShadowCoords = false;
            Matrix44 normalTransform;
            if (normalMap || reflectionMap || sphereMap)
            {
                mathMatrixInverse(normalTransform.GetData(), m_ViewMatrix.GetData(), g_CpuFlags);
                normalTransform.Transpose();
            }
            const int texgenFirstVertex = m_RP.m_pRE ? m_RP.m_FirstVertex : 0;
            const int texgenVertexCount = m_RP.m_pRE ? m_RP.m_RendNumVerts : vertices->m_NumVerts;
            if (texgenFirstVertex < 0 || texgenFirstVertex > vertices->m_NumVerts ||
                texgenVertexCount < 0 || texgenVertexCount > vertices->m_NumVerts - texgenFirstVertex)
            {
                m_frameRenderer->RequirePanelFallback();
                return;
            }
            const int texgenEndVertex = texgenFirstVertex + texgenVertexCount;
            for (int vertex = texgenFirstVertex; vertex < texgenEndVertex; ++vertex)
            {
                const uint8_t* source = data + static_cast<size_t>(vertex) * stockVertexFormat.stride;
                const uint8_t* originalVertex =
                    sourcePositionData + static_cast<size_t>(vertex) * sourcePositionStride;
                float originalPosition[3] = {};
                memcpy(originalPosition, originalVertex + sourcePositionOffset, sizeof(originalPosition));
                float position[3] = {};
                memcpy(position, source + positionOffset, sizeof(position));
                float uv[2] = {};
                if (normalMap || reflectionMap || sphereMap)
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
                    if (m_RP.m_pRE && usesTangentNormal && sourceTangentBasis)
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
                        const Vec3d viewOrigin = m_RP.m_pRE ? objectTranslation : m_RP.m_ViewOrg;
                        const float* environmentPosition = m_RP.m_pRE ? originalPosition : position;
                        float view[3] = { viewOrigin.x - environmentPosition[0],
                                         viewOrigin.y - environmentPosition[1],
                                         viewOrigin.z - environmentPosition[2] };
                        const float length = sqrtf(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]);
                        if (length > 1.0e-6f)
                        {
                            view[0] /= length; view[1] /= length; view[2] /= length;
                        }
                        const float dot = view[0] * normal[0] + view[1] * normal[1] + view[2] * normal[2];
                        uv[0] = ((2.0f * dot * normal[1] - view[1]) + 1.0f) * 0.5f;
                        const float reflectedZ = 2.0f * dot * normal[2] - view[2];
                        uv[1] = m_RP.m_pRE ? 0.5f - reflectedZ * 0.5f :
                            (reflectedZ + 1.0f) * 0.5f;
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
                else if (unit->m_eGenTC == eGTC_ShadowMap)
                {
                    // SEvalFuncs_RE::ETC_ShadowMap selects one frustum per
                    // shadow-map texture stage, offset by the current sample
                    // group. Install the same light * object matrix and bind
                    // the depth texture before generating receiver coords.
                    CCObject* const receiverObject = m_RP.m_pCurObject;
                    list2<ShadowMapLightSourceInstance>* const shadowCasters =
                        receiverObject ? static_cast<list2<ShadowMapLightSourceInstance>*>(
                            receiverObject->m_pShadowCasters) : nullptr;
                    const bool terrainShadowPass = m_RP.m_pShader &&
                        m_RP.m_pShader->m_eSort == eS_TerrainShadowPass;
                    const int shadowCasterIndex = terrainShadowPass ? 0 :
                        stage + m_RP.m_nCurStartCaster;
                    if (!shadowCasters || shadowCasterIndex < 0 ||
                        shadowCasterIndex >= shadowCasters->Count())
                        return;
                    ShadowMapLightSourceInstance& shadowCaster =
                        shadowCasters->GetAt(shadowCasterIndex);
                    ShadowMapFrustum* shadowFrustum = shadowCaster.m_pLS ?
                        shadowCaster.m_pLS->GetShadowMapFrustum() : nullptr;
                    if (shadowFrustum && shadowCasterIndex > 0 &&
                        shadowCasters->GetAt(shadowCasterIndex - 1).m_pLS ==
                            shadowCaster.m_pLS && shadowFrustum->pPenumbra)
                        shadowFrustum = shadowFrustum->pPenumbra;
                    if (!shadowFrustum || !shadowFrustum->depth_tex_id)
                        return;
                    Matrix44* const receiverMatrix =
                        (m_RP.m_ObjFlags & FOB_TRANS_MASK) ?
                            &receiverObject->m_Matrix : nullptr;
                    SetupShadowOnlyPass(stage, shadowFrustum,
                        &shadowCaster.m_vProjTranslation,
                        shadowCaster.m_fProjScale,
                        receiverObject ? receiverObject->GetTranslation() : Vec3d(0.0f, 0.0f, 0.0f),
                        1.0f, Vec3d(0.0f, 0.0f, 0.0f), receiverMatrix);
                    // ETC_ShadowMap installs the light bias * projection *
                    // view * object matrix in m_TempMatrices[stage][0]. GL
                    // applies that matrix after object-linear texgen; bake
                    // the same homogeneous transform into this generated UV
                    // stream and leave the Vulkan stage matrix at identity
                    // below so it is not applied twice.
                    const float* shadowMatrix = m_cEF.m_TempMatrices[stage][0].GetData();
                    memcpy(shadowMapTransforms[stage], shadowMatrix,
                           sizeof(shadowMapTransforms[stage]));
                    shadowMapStageMask |= 1u << stage;
                    const float tx = originalPosition[0] * shadowMatrix[0] +
                                     originalPosition[1] * shadowMatrix[4] +
                                     originalPosition[2] * shadowMatrix[8] + shadowMatrix[12];
                    const float ty = originalPosition[0] * shadowMatrix[1] +
                                     originalPosition[1] * shadowMatrix[5] +
                                     originalPosition[2] * shadowMatrix[9] + shadowMatrix[13];
                    const float tw = originalPosition[0] * shadowMatrix[3] +
                                     originalPosition[1] * shadowMatrix[7] +
                                     originalPosition[2] * shadowMatrix[11] + shadowMatrix[15];
                    if (fabsf(tw) > 1.0e-6f)
                    {
                        uv[0] = tx / tw;
                        uv[1] = ty / tw;
                        generatedShadowCoords = true;
                    }
                    else
                    {
                        m_frameRenderer->RequirePanelFallback();
                        return;
                    }
                }
                else
                {
                    m_frameRenderer->RequirePanelFallback();
                    return;
                }
                memcpy(data + static_cast<size_t>(vertex) * stockVertexFormat.stride + uvOffset,
                       uv, sizeof(uv));
            }
            if (generatedShadowCoords)
            {
                memset(textureMatrices[stage], 0, sizeof(textureMatrices[stage]));
                textureMatrices[stage][0] = textureMatrices[stage][5] =
                    textureMatrices[stage][10] = textureMatrices[stage][15] = 1.0f;
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
        int normalMapTextureStage = -1;
        float materialLighting[19] = { -0.35f, 0.72f, 0.60f, 0.0f,
                                       1.0f, 1.0f, 1.0f, 1.0f,
                                       1.0f, 1.0f, 1.0f, 1.0f, 0.0f };
        float materialDiffuse[3] = { 1.0f, 1.0f, 1.0f };
        // The stock lit vertex shaders evaluate the fallback directional
        // light in object space. Use the sun direction prepared from the
        // active scene light list and transform it through the object's
        // inverse matrix; a fixed world-space vector made lighting rotate
        // with the mesh (and could leave most normals permanently unlit).
        Vec3d objectSunDirection = m_RP.m_SunDir;
        if (m_RP.m_pCurObject)
            objectSunDirection = m_RP.m_pCurObject->GetInvMatrix().TransformVectorOLD(objectSunDirection);
        if (objectSunDirection.Normalize() == 0.0f)
            objectSunDirection = Vec3d(0.0f, 0.0f, 1.0f);
        materialLighting[0] = objectSunDirection.x;
        materialLighting[1] = objectSunDirection.y;
        materialLighting[2] = objectSunDirection.z;
        // GL_LIGHT_MODEL_LOCAL_VIEWER defaults to false, so the fixed-function
        // specular eye vector is constant in eye space (+Z). CryEngine camera
        // space looks along +Y; convert that direction and then into object
        // space for the fixed-function stock-light shader.
        Matrix34 cameraView = GetCamera().GetVMatrix();
        Matrix34 inverseCameraView = cameraView.GetInverted();
        Vec3d objectViewDirection = inverseCameraView.TransformVector(Vec3d(0.0f, -1.0f, 0.0f));
        if (m_RP.m_pCurObject)
            objectViewDirection = m_RP.m_pCurObject->GetInvMatrix().TransformVectorOLD(
                objectViewDirection);
        if (objectViewDirection.Normalize() == 0.0f)
            objectViewDirection = Vec3d(0.0f, 0.0f, 1.0f);
        materialLighting[16] = objectViewDirection.x;
        materialLighting[17] = objectViewDirection.y;
        materialLighting[18] = objectViewDirection.z;
        if (m_activeHardwarePassType == eSHP_MAX)
            materialLighting[7] = 0.0f;
        if (m_activeResources && m_activeResources->m_LMaterial)
        {
            const SSideMaterial& material = m_activeResources->m_LMaterial->Front;
            materialLighting[8] = material.m_Ambient.r;
            materialLighting[9] = material.m_Ambient.g;
            materialLighting[10] = material.m_Ambient.b;
            materialDiffuse[0] = material.m_Diffuse.r;
            materialDiffuse[1] = material.m_Diffuse.g;
            materialDiffuse[2] = material.m_Diffuse.b;
            materialLighting[4] = materialDiffuse[0];
            materialLighting[5] = materialDiffuse[1];
            materialLighting[6] = materialDiffuse[2];
            if (m_activeHardwarePassType == eSHP_MAX)
                materialLighting[7] = material.m_SpecShininess;
        }
        const bool hardwareLightPass =
            m_activeHardwarePassType == eSHP_Light ||
            m_activeHardwarePassType == eSHP_DiffuseLight ||
            m_activeHardwarePassType == eSHP_SpecularLight ||
            m_activeHardwarePassType == eSHP_MultiLights;
        const bool hardwareMaterialLightPass =
            m_activeHardwarePassType == eSHP_Light ||
            m_activeHardwarePassType == eSHP_DiffuseLight ||
            m_activeHardwarePassType == eSHP_SpecularLight;
        // Filter individual sources, rather than dropping the whole material
        // Light pass: its first contribution can also carry object ambient.
        // OpenGL routes these passes through SLightMaterial::mfApply and
        // EF_LightMaterial; LMF_IGNORELIGHTS deliberately skips EF_SetLights.
        // MultiLights is its own PS30 path and does not use that executor.
        const bool hardwareSpecularPass = m_activeHardwarePassType == eSHP_SpecularLight;
        const uint32_t hardwareLightFlags = activeHardwareLightFlags;
        float hardwareAmbientParameter[4]{};
        float hardwareTerrainFogColor[4]{};
        bool hardwareGeneralAmbientProgram = false;
        bool hardwareMaterialAmbientProgram = false;
        if ((m_activeHardwarePassType == eSHP_General ||
             ((stockTerrainShadowProgram || stockReceiverShadow) && m_activeHardwarePassType == eSHP_Shadow) ||
             (hardwareMaterialLightPass && !hardwareSpecularPass)) && m_activePass &&
            m_RP.m_pShader && (stockTerrainShadowProgram || stockReceiverShadow || !shaderLightingDisabled))
        {
            SShaderPassHW* hardwarePass = static_cast<SShaderPassHW*>(m_activePass);
            TArray<SCGParam4f>* parameterLists[] = {
                hardwarePass->m_CGFSParamsNoObj, hardwarePass->m_CGFSParamsObj,
                &hardwarePass->m_VPParamsNoObj, &hardwarePass->m_VPParamsObj };
            for (TArray<SCGParam4f>* parameters : parameterLists)
            {
                if (!parameters) continue;
                for (int parameter = 0; parameter < parameters->Num(); ++parameter)
                {
                    SCGParam4f& value = parameters->Get(parameter);
                    if (!FastAsciiCaseCompare(value.m_Name.c_str(), "Ambient"))
                    {
                        memcpy(hardwareAmbientParameter, value.mfGet(), sizeof(hardwareAmbientParameter));
                        hardwareGeneralAmbientProgram = m_activeHardwarePassType == eSHP_General;
                        hardwareMaterialAmbientProgram = hardwareMaterialLightPass &&
                            m_RP.m_RendPass == 1 && !(m_RP.m_ObjFlags & FOB_LIGHTPASS);
                    }
                    else if (!FastAsciiCaseCompare(value.m_Name.c_str(), "FogColor"))
                        memcpy(hardwareTerrainFogColor, value.mfGet(),
                               sizeof(hardwareTerrainFogColor));
                }
            }
        }
        // EF_LightMaterial returns before selecting lights for fog objects or
        // shaders without normals; EF_FlushShader clears lighting without a
        // light material. Apply those gates before building any Vulkan draws.
        const bool fixedFunctionMaterialLighting = m_activeHardwarePassType == eSHP_MAX &&
            !(hardwareLightFlags & (LMF_DISABLE | LMF_BUMPMATERIAL)) &&
            m_RP.m_pShader && (m_RP.m_pShader->m_Flags & EF_NEEDNORMALS) &&
            !(m_RP.m_ObjFlags & FOB_FOGPASS) && m_RP.m_pCurObject &&
            m_activeResources && m_activeResources->m_LMaterial;
        const bool canTranslateDynamicLights = (
            m_activeHardwarePassType == eSHP_MultiLights ||
            ((fixedFunctionMaterialLighting || hardwareMaterialLightPass) &&
             !(activeHardwareLightFlags & LMF_IGNORELIGHTS)));
        const bool hardwareMultiLightsAmbient =
            m_activeHardwarePassType == eSHP_MultiLights &&
            (hardwareLightFlags & LMF_HASAMBIENT) &&
            !(hardwareLightFlags & (LMF_DISABLE | LMF_NOAMBIENT)) &&
            !(m_RP.m_ObjFlags & FOB_LIGHTPASS) &&
            (!(hardwareLightFlags & LMF_HASDOT3LM) || hasMultiLightsDirectionalLightmap);
        // A GL MultiLights pass evaluates ambient together with its selected
        // direct lights. Keep the diffuse material term available when those
        // lights are translated to additive Vulkan draws. With no selected
        // lights the same pass reduces to ambient only.
        const bool hardwareAmbientPass = (activeHardwareAmbientPass || hardwareMultiLightsAmbient ||
            hardwareGeneralAmbientProgram) &&
            (!hardwareMultiLightsAmbient || m_RP.m_DynLMask == 0);
        const bool nightMapLighting = (m_RP.m_PersFlags & RBPF_DRAWNIGHTMAP) != 0;
        if (nightMapLighting)
        {
            // SParamComp_AmbLightColor returns opaque white immediately while
            // rendering night maps, before applying LMF_NOAMBIENT, object
            // ambient, the light material, or its divide scale.
            materialLighting[8] = materialLighting[9] = materialLighting[10] = 1.0f;
        }
        else if (m_RP.m_pCurObject)
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
        if (!nightMapLighting)
        {
            // SParamComp_AmbLightColor filters object/material ambient through
            // the material diffuse color before clamping each channel.
            for (int channel = 0; channel < 3; ++channel)
                materialLighting[8 + channel] *= materialDiffuse[channel];
            float ambientScale = 1.0f;
            if (hardwareLightFlags & LMF_DIVIDEAMB4)
                ambientScale = 0.25f;
            if (hardwareLightFlags & LMF_DIVIDEAMB2)
                ambientScale = 0.5f;
            for (int channel = 8; channel < 11; ++channel)
                materialLighting[channel] = crymin(materialLighting[channel] * ambientScale, 1.0f);
        }
        float diffuseScale = 1.0f;
        if ((hardwareGeneralAmbientProgram || hardwareMaterialAmbientProgram) &&
            !(hardwareLightFlags & LMF_NOAMBIENT))
        {
            // Use the same CGPSParam Ambient components as OpenGL, including
            // shader-specific multipliers, instead of leaving transparent
            // general passes at unlit albedo when they have no baked lightmap.
            materialLighting[8] = hardwareAmbientParameter[0];
            materialLighting[9] = hardwareAmbientParameter[1];
            materialLighting[10] = hardwareAmbientParameter[2];
        }
        if (hardwareLightFlags & LMF_DIVIDEDIFF4)
            diffuseScale = 0.25f;
        if (hardwareLightFlags & LMF_DIVIDEDIFF2)
            diffuseScale = 0.5f;
        for (int channel = 4; channel < 7; ++channel)
            materialLighting[channel] *= diffuseScale;
        if (m_activeHardwarePassType == eSHP_MAX && m_RP.m_pCurObject &&
            m_activeResources && m_activeResources->m_LMaterial)
        {
            // EF_LightMaterial uses the fixed-function material helpers,
            // rather than programmable AmbLightColor (which also multiplies
            // by diffuse). Share the exact GL formulas and channel clamps.
            const CFColor ambient = EF_GetCurrentAmbient(
                m_activeResources->m_LMaterial, hardwareLightFlags);
            const CFColor diffuse = EF_GetCurrentDiffuse(
                m_activeResources->m_LMaterial, hardwareLightFlags);
            materialLighting[8] = ambient.r;
            materialLighting[9] = ambient.g;
            materialLighting[10] = ambient.b;
            materialLighting[4] = diffuse.r;
            materialLighting[5] = diffuse.g;
            materialLighting[6] = diffuse.b;
        }
        if ((stockTerrainShadowProgram || stockReceiverShadow) && m_activeHardwarePassType == eSHP_Shadow)
        {
            // CGRCTerrainShadow uses its own CGPSParam Ambient directly; it
            // must not inherit the generic object/material ambient product.
            materialLighting[8] = hardwareAmbientParameter[0];
            materialLighting[9] = hardwareAmbientParameter[1];
            materialLighting[10] = hardwareAmbientParameter[2];
        }
        if (hardwareAmbientPass)
        {
            // OpenGL's programmable technique adds dynamic lights in its
            // dedicated light passes, not again in its general/base pass.
            materialLighting[4] = materialLighting[5] = materialLighting[6] = 0.0f;
            if (!nightMapLighting && (hardwareLightFlags & LMF_NOAMBIENT))
                materialLighting[8] = materialLighting[9] = materialLighting[10] = 0.0f;
        }
        else if (hardwareLightPass)
        {
            // Translated per-light passes must not repeat ambient; specular
            // replaces the diffuse tint and uses a negative exponent marker
            // in the existing eight-float lighting payload.
            if (!hardwareMultiLightsAmbient && !hardwareMaterialAmbientProgram && !nightMapLighting)
                materialLighting[8] = materialLighting[9] = materialLighting[10] = 0.0f;
            if (hardwareSpecularPass &&
                (hardwareLightFlags & (LMF_NOSPECULAR | LMF_NOADDSPECULAR)))
                materialLighting[4] = materialLighting[5] = materialLighting[6] = 0.0f;
        }

        // DrawBuffer is called once for every material draw. Keep the
        // translated-light scratch storage on the renderer so those draws do
        // not allocate three fresh vectors each time. The storage is only
        // consumed synchronously while this DrawBuffer call is active.
        std::vector<std::array<float, 19>>& lightPasses = m_lightPassScratch;
        std::vector<std::array<int, 2>>& lightPassSpecularOcclusion =
            m_lightPassSpecularOcclusionScratch;
        std::vector<bool>& ambientOnlyLightPasses = m_ambientOnlyLightPassScratch;
        std::vector<CDLight*>& translatedLights = m_translatedLightScratch;
        lightPasses.clear();
        lightPassSpecularOcclusion.clear();
        ambientOnlyLightPasses.clear();
        translatedLights.clear();
        const auto appendLightPass = [&](const std::array<float, 19>& parameters,
                                         int occlusionTextureId = 0,
                                         int occlusionChannel = -1,
                                         bool ambientOnly = false,
                                         CDLight* sourceLight = nullptr)
        {
            lightPasses.push_back(parameters);
            lightPassSpecularOcclusion.push_back(
                { occlusionTextureId, occlusionChannel });
            ambientOnlyLightPasses.push_back(ambientOnly);
            translatedLights.push_back(sourceLight);
        };
        bool hasTranslatedDynamicLightPass = false;
        const auto evaluateSpecularParameter = [&](CDLight* light, std::array<float, 19>& parameters)
        {
            if (!stockLightTemplate || parameters[3] >= 0.0f) return;
            CDLight* previousLight = m_RP.m_pCurLight;
            m_RP.m_pCurLight = light;
            const TArray<SCGParam4f>* lists[] = {
                stockHardwarePass->m_CGFSParamsNoObj, stockHardwarePass->m_CGFSParamsObj };
            for (const auto* list : lists)
                if (list)
                    for (int index = 0; index < list->Num(); ++index)
                    {
                        SCGParam4f& parameter = const_cast<SCGParam4f&>(list->Get(index));
                        if (FastAsciiCaseCompare(parameter.m_Name.c_str(), "Specular")) continue;
                        const float* value = parameter.mfGet();
                        for (int channel = 0; channel < 3; ++channel) parameters[4+channel] = value[channel];
                        parameters[3] = -crymax(value[3], 0.001f);
                    }
            m_RP.m_pCurLight = previousLight;
        };
        // OpenGL's MultiLights ambient pass is independent of its packed
        // direct-light passes. Keep it as the first Vulkan draw as well:
        // projected lights use materialAmbient.xyz for projector direction,
        // so they cannot carry the ambient RGB in that same draw.
        if (hardwareMultiLightsAmbient && !hardwareAmbientPass && m_RP.m_DynLMask)
        {
            std::array<float, 19> ambientParameters;
            memcpy(ambientParameters.data(), materialLighting, sizeof(materialLighting));
            ambientParameters[4] = ambientParameters[5] = ambientParameters[6] = 0.0f;
            appendLightPass(ambientParameters, 0, -1, true);
        }
        // Position/UV-only scene formats still receive lighting: the fragment
        // shader reconstructs their face normal from derivatives. Do not gate
        // the light list on a normal vertex attribute here.
        const bool fixedFunctionLightList = m_activeHardwarePassType == eSHP_MAX;
        if (canTranslateDynamicLights && !hardwareAmbientPass &&
            (fixedFunctionLightList ? m_RP.m_NumActiveDLights != 0 : m_RP.m_DynLMask != 0) &&
            m_RP.m_pCurObject)
        {
            const int lightLevel = SRendItem::m_RecurseLevel;
            if (lightLevel >= 0 && lightLevel < 8)
            {
                const int lightCount = fixedFunctionLightList ?
                    m_RP.m_NumActiveDLights : m_RP.m_DLights[lightLevel].Num();
                Vec3d lightBoundsCenter(0.0f, 0.0f, 0.0f);
                float lightBoundsRadius = 1000.0f;
                if (m_RP.m_pRE)
                {
                    Vec3d boundsMin, boundsMax;
                    m_RP.m_pRE->mfGetBBox(boundsMin, boundsMax);
                    lightBoundsCenter = (boundsMin + boundsMax) * 0.5f;
                    lightBoundsCenter += m_RP.m_pCurObject->GetTranslation();
                    lightBoundsRadius = (boundsMax - boundsMin).Length() * 0.5f;
                }
                const auto stockRangeAttenuation = [](float distance, float radius)
                {
                    if (distance > radius || !(radius > 0.0f))
                        return 0.0f;
                    // The stock b/a*a expression produces NaN at the source
                    // origin. Its finite limit is two; never upload NaN light
                    // attenuation to a movable receiver passing that point.
                    if (distance <= 0.0f) return 2.0f;
                    const float a = distance / radius;
                    const float b = 2.0f * a * a * a - 3.0f * a * a + 1.0f;
                    // Preserve GLRendPipeline.cpp::sAttenuation's operation
                    // order as well as its equation; EF_SetLights inverts this
                    // value to derive the render-element constant/linear terms.
                    return b / a * a * 2.0f;
                };
                // Eight is the fixed-function slot limit, not a limit on
                // scene light IDs. Programmable passes select by the mask.
                const int lightLimit = fixedFunctionLightList ? 8 : 32;
                bool fixedPointBoundsUsed = false;
                for (int lightIndex = 0; lightIndex < lightCount && lightIndex < lightLimit; ++lightIndex)
                {
                    if (!fixedFunctionLightList && !(m_RP.m_DynLMask & (1u << lightIndex)))
                        continue;
                    // GL's eight slots and LMF_LIGHT_MASK index the compact
                    // EF_BuildLightsList result, not the scene-wide light array.
                    if (fixedFunctionLightList && (hardwareLightFlags & LMF_LIGHT_MASK) &&
                        ((hardwareLightFlags & LMF_LIGHT_MASK) >> LMF_LIGHT_SHIFT) != lightIndex)
                        continue;
                    CDLight* light = fixedFunctionLightList ?
                        m_RP.m_pActiveDLights[lightIndex] : m_RP.m_DLights[lightLevel][lightIndex];
                    if (!ShouldRenderVulkanLight(light) || !(light->m_Flags &
                                    (DLF_DIRECTIONAL | DLF_POINT | DLF_PROJECT)))
                        continue;
                    // EF_SetLights filters projector lights before preparing
                    // either diffuse or specular fixed-function contributions.
                    if (m_activeHardwarePassType == eSHP_MAX &&
                        (hardwareLightFlags & LMF_IGNOREPROJLIGHTS) &&
                        (light->m_Flags & DLF_PROJECT))
                        continue;
                    // OpenGL treats DLF_LM as specular-only on a surface with
                    // a baked lightmap. Diffuse from that light is already in
                    // the map and must not be multiplied into the base pass.
                    const bool specularOnlyLight =
                        (m_RP.m_pCurObject->m_nLMId > 0 || hasMaterialLightmap) &&
                        (light->m_Flags & DLF_LM);
                    if (m_activeHardwarePassType == eSHP_MAX && specularOnlyLight &&
                        (hardwareLightFlags & LMF_NOSPECULAR))
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

                    std::array<float, 19> parameters;
                    memcpy(parameters.data(), materialLighting, sizeof(materialLighting));
                    parameters[0] = objectLightPosition.x;
                    parameters[1] = objectLightPosition.y;
                    parameters[2] = objectLightPosition.z;
                    parameters[3] = pointLight ? light->m_fRadius : 0.0f;
                    // EF_SetLights applies 1.5x only to fixed-function
                    // directional lights. Programmable LightColor/LightsColor
                    // components use the raw CDLight color.
                    const float directionalScale = directionalLight &&
                        m_activeHardwarePassType == eSHP_MAX ? 1.5f : 1.0f;
                    parameters[4] *= light->m_Color.r * directionalScale;
                    parameters[5] *= light->m_Color.g * directionalScale;
                    parameters[6] *= light->m_Color.b * directionalScale;
                    if (stockLightTemplate && !hardwareSpecularPass)
                    {
                        // Evaluate the stock CG parameter itself. Its scalar
                        // LightColor components clamp independently and may
                        // have material/user scales absent from a reconstructed
                        // raw CDLight color.
                        CDLight* previousLight = m_RP.m_pCurLight;
                        m_RP.m_pCurLight = light;
                        TArray<SCGParam4f>* lists[2] = {
                            stockHardwarePass->m_CGFSParamsNoObj,
                            stockHardwarePass->m_CGFSParamsObj };
                        for (TArray<SCGParam4f>* list : lists)
                            if (list)
                                for (int parameter = 0; parameter < list->Num(); ++parameter)
                                    if (!FastAsciiCaseCompare(list->Get(parameter).m_Name.c_str(), "Diffuse"))
                                    {
                                        const float* diffuse = list->Get(parameter).mfGet();
                                        for (int channel = 0; channel < 3; ++channel)
                                            parameters[4 + channel] = diffuse[channel];
                                    }
                        m_RP.m_pCurLight = previousLight;
                        if (!(stockHardwarePass->m_StockProgramMask & 0x1ull))
                            parameters[4] = parameters[5] = parameters[6] = 0.0f;
                    }
                    if (m_activeHardwarePassType == eSHP_MAX &&
                        m_activeResources && m_activeResources->m_LMaterial)
                    {
                        const SSideMaterial& material = m_activeResources->m_LMaterial->Front;
                        if (hardwareLightFlags & LMF_NOSPECULAR)
                        {
                            parameters[7] = 0.0f;
                            parameters[13] = parameters[14] = parameters[15] = 0.0f;
                        }
                        else
                        {
                            parameters[13] = light->m_SpecColor.r * material.m_Specular.r;
                            parameters[14] = light->m_SpecColor.g * material.m_Specular.g;
                            parameters[15] = light->m_SpecColor.b * material.m_Specular.b;
                        }
                    }
                    if (specularOnlyLight && !hardwareSpecularPass)
                        parameters[4] = parameters[5] = parameters[6] = 0.0f;
                    parameters[11] = 1.0f;
                    parameters[12] = 0.0f;
                    if (pointLight && light->m_fRadius > 0.0f)
                    {
                        Vec3d attenuationBoundsCenter = lightBoundsCenter;
                        float attenuationBoundsRadius = lightBoundsRadius;
                        if (fixedFunctionLightList)
                        {
                            // Preserve EF_SetLights' bCalcDist branch exactly:
                            // only the first accepted point light uses RE bounds.
                            if (!m_RP.m_pRE || fixedPointBoundsUsed)
                            {
                                attenuationBoundsCenter = Vec3d(0.0f, 0.0f, 0.0f);
                                attenuationBoundsRadius = 1000.0f;
                            }
                            else
                                fixedPointBoundsUsed = true;
                        }
                        const float centerDistance = crymax(0.1f,
                            (attenuationBoundsCenter - light->m_Origin).Length());
                        const float maxDistance = clamp_tpl(centerDistance + attenuationBoundsRadius,
                            light->m_fRadius * 0.1f, light->m_fRadius * 0.99f);
                        const float minDistance = clamp_tpl(centerDistance - attenuationBoundsRadius,
                            light->m_fRadius * 0.1f, light->m_fRadius * 0.99f);
                        const float minAttenuation = 1.0f /
                            stockRangeAttenuation(minDistance, light->m_fRadius);
                        const float maxAttenuation = 1.0f /
                            stockRangeAttenuation(maxDistance, light->m_fRadius);
                        if (fabsf(minAttenuation - maxAttenuation) < 0.00001f)
                            parameters[11] = minAttenuation;
                        else
                        {
                            const float attenuationSlope = (maxAttenuation - minAttenuation) /
                                (maxDistance - minDistance);
                            const float constantAttenuation = crymax(0.01f,
                                minAttenuation - attenuationSlope * minDistance);
                            parameters[11] = constantAttenuation;
                            parameters[12] = crymax(0.0f,
                                (minAttenuation - constantAttenuation) / minDistance);
                        }
                    }
                    if ((light->m_Flags & DLF_PROJECT) &&
                        m_activeHardwarePassType != eSHP_MAX)
                    {
                        // TEMP_AMBIENT is independent of projector attenuation
                        // and its cookie. Preserve it before the legacy payload
                        // repurposes RGB ambient for the projector direction.
                        if (hardwareMaterialAmbientProgram && lightPasses.empty() &&
                            !hardwareSpecularPass)
                        {
                            std::array<float, 19> ambientParameters;
                            memcpy(ambientParameters.data(), materialLighting, sizeof(materialLighting));
                            ambientParameters[4] = ambientParameters[5] = ambientParameters[6] = 0.0f;
                            ambientParameters[7] = 1.0f;
                            appendLightPass(ambientParameters, 0, -1, true);
                        }
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
                            // Distance and half angle belong to each surface
                            // point, not the object's origin. Evaluate in GLSL.
                            const float attenuationScale = 1.0f;
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
                            parameters[4] = lightSpecular.r * specularMaterial[0];
                            parameters[5] = lightSpecular.g * specularMaterial[1];
                            parameters[6] = lightSpecular.b * specularMaterial[2];
                        }
                        parameters[11] = objectLightPosition.x;
                        parameters[12] = objectLightPosition.y;
                        parameters[13] = objectLightPosition.z;
                        parameters[14] = light->m_fRadius;
                        parameters[15] = pointLight ?
                            ((light->m_Flags & DLF_PROJECT) ? 3.0f : 2.0f) : 1.0f;
                        parameters[16] = objectCameraPosition.x;
                        parameters[17] = objectCameraPosition.y;
                        parameters[18] = objectCameraPosition.z;
                    }
                    // Emit material ambient once; following lights use additive
                    // RGB blending and must contribute only their direct term.
                    if (!lightPasses.empty())
                    {
                        parameters[7] = 0.0f;
                        if (!(light->m_Flags & DLF_PROJECT) && !hardwareSpecularPass)
                            parameters[8] = parameters[9] = parameters[10] = 0.0f;
                    }
                    if ((light->m_Flags & DLF_PROJECT) &&
                        m_activeHardwarePassType != eSHP_MAX)
                    {
                        const float halfAngle = clamp_tpl(light->m_fLightFrustumAngle,
                                                          0.0f, 89.9f);
                        parameters[7] = -crymax(0.001f,
                            cry_cosf(halfAngle * gf_PI / 180.0f));
                    }
                    // Programmable OpenGL LightColor/LightsColor components
                    // normalize overbright RGB when HDR is off. The separate
                    // fixed-function EF_SetLights path does not, so leave the
                    // eSHP_MAX emulation untouched here.
                    if (m_activeHardwarePassType != eSHP_MAX &&
                        !(m_RP.m_PersFlags & RBPF_HDR))
                    {
                        const float maxLightChannel = crymax(parameters[4],
                            crymax(parameters[5], parameters[6]));
                        if (maxLightChannel > 1.1f)
                        {
                            parameters[4] /= maxLightChannel;
                            parameters[5] /= maxLightChannel;
                            parameters[6] /= maxLightChannel;
                        }
                    }
                    int specularOcclusionTextureId = 0;
                    int specularOcclusionChannel = -1;
                    if (hardwareMultiLightsAmbient && specularOnlyLight &&
                        m_RP.m_pCurObject->m_nLMId > 0 &&
                        m_RP.m_pCurObject->m_nOcclId > 0)
                    {
                        const uint8_t encodedLightId =
                            static_cast<uint8_t>(light->m_Id + 1);
                        for (int channel = 0; channel < 4; ++channel)
                        {
                            if (m_RP.m_pCurObject->m_OcclLights[channel] ==
                                encodedLightId)
                            {
                                specularOcclusionTextureId =
                                    m_RP.m_pCurObject->m_nOcclId;
                                specularOcclusionChannel = channel;
                                break;
                            }
                        }
                    }
                    evaluateSpecularParameter(light, parameters);
                    appendLightPass(parameters, 0, -1, false, light);
                    hasTranslatedDynamicLightPass = true;

                    // OpenGL marks DLF_LM lights as specular-only on baked
                    // lightmapped receivers. The original Light pass combines
                    // diffuse and specular in one shader, while MultiLights
                    // encodes this distinction per light. Vulkan's stock
                    // approximation emits the specular contribution as its
                    // own additive draw for both pass types.
                    if ((m_activeHardwarePassType == eSHP_Light ||
                         m_activeHardwarePassType == eSHP_MultiLights) &&
                        (!stockLightTemplate || (stockHardwarePass->m_StockProgramMask & 0x2ull)) &&
                        !(hardwareLightFlags & (LMF_NOSPECULAR | LMF_NOADDSPECULAR)) &&
                        (light->m_SpecColor.r != 0.0f || light->m_SpecColor.g != 0.0f ||
                         light->m_SpecColor.b != 0.0f))
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
                        // Programmable attenuation belongs to each fragment,
                        // not to the object's origin or fixed-function lights.
                        const float specularAttenuation = 1.0f;
                        safeNormalize(lightDirection);
                        safeNormalize(viewDirection);
                        Vec3d halfVector = lightDirection + viewDirection;
                        safeNormalize(halfVector);

                        std::array<float, 19> specularParameters = parameters;
                        specularParameters[0] = halfVector.x;
                        specularParameters[1] = halfVector.y;
                        specularParameters[2] = halfVector.z;
                        float specularMaterial[3] = { 1.0f, 1.0f, 1.0f };
                        if (m_activeResources && m_activeResources->m_LMaterial)
                        {
                            const SSideMaterial& material = m_activeResources->m_LMaterial->Front;
                            specularMaterial[0] = material.m_Specular.r;
                            specularMaterial[1] = material.m_Specular.g;
                            specularMaterial[2] = material.m_Specular.b;
                            specularParameters[3] = -crymax(1.0f, material.m_SpecShininess);
                        }
                        else
                            specularParameters[3] = -10.0f;
                        specularParameters[4] = light->m_SpecColor.r * specularMaterial[0] *
                            specularAttenuation;
                        specularParameters[5] = light->m_SpecColor.g * specularMaterial[1] *
                            specularAttenuation;
                        specularParameters[6] = light->m_SpecColor.b * specularMaterial[2] *
                            specularAttenuation;
                        if (!(m_RP.m_PersFlags & RBPF_HDR))
                        {
                            const float maxSpecularChannel = crymax(specularParameters[4],
                                crymax(specularParameters[5], specularParameters[6]));
                            if (maxSpecularChannel > 1.1f)
                            {
                                specularParameters[4] /= maxSpecularChannel;
                                specularParameters[5] /= maxSpecularChannel;
                                specularParameters[6] /= maxSpecularChannel;
                            }
                        }
                        specularParameters[8] = specularParameters[9] =
                            specularParameters[10] = 0.0f;
                        specularParameters[11] = objectLightPosition.x;
                        specularParameters[12] = objectLightPosition.y;
                        specularParameters[13] = objectLightPosition.z;
                        specularParameters[14] = light->m_fRadius;
                        specularParameters[15] = pointLight ?
                            ((light->m_Flags & DLF_PROJECT) ? 3.0f : 2.0f) : 1.0f;
                        specularParameters[16] = objectCameraPosition.x;
                        specularParameters[17] = objectCameraPosition.y;
                        specularParameters[18] = objectCameraPosition.z;
                        evaluateSpecularParameter(light, specularParameters);
                        appendLightPass(specularParameters,
                                        specularOcclusionTextureId,
                                        specularOcclusionChannel, false, light);
                    }
                }
            }
        }
        // CGLRenderer::EF_LightMaterial falls back to
        // EF_ConstantLightMaterial when no GL light slot was populated. Keep
        // that ambient-only result for fixed-function materials that require
        // normals; otherwise the prepared object/material ambient was lost
        // whenever the Vulkan dynamic-light list was empty or fully filtered.
        const bool fixedFunctionAmbientOnlyPass =
            fixedFunctionMaterialLighting &&
            !hasTranslatedDynamicLightPass;
        if (lightPasses.empty())
        {
            std::array<float, 19> parameters;
            memcpy(parameters.data(), materialLighting, sizeof(materialLighting));
            if (m_activeHardwarePassType == eSHP_MultiLights ||
                fixedFunctionAmbientOnlyPass)
                parameters[4] = parameters[5] = parameters[6] = 0.0f;
            appendLightPass(parameters);
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
        int objectAlphaTextureEnvMode = 0;
        float objectAlphaOpacity = 1.0f;
        float primaryColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        float primaryColorMask[4] = {};
        const bool applyResourceStates = !m_RP.m_pShader ||
            !(m_RP.m_pShader->m_Flags2 & EF2_IGNORERESOURCESTATES);
        if (m_activeResources && applyResourceStates)
        {
            alphaTestRef = m_activeResources->m_AlphaRef;
            additiveMaterial = (m_activeResources->m_ResFlags & MTLFLAG_ADDITIVE) != 0;
            // Match EF_SetResourcesState in the OpenGL renderer: AlphaRef
            // takes precedence over material opacity. That path installs an
            // alpha test and does not switch the pass to blending when both
            // resource values are set.
            if (alphaTestRef == 0.0f)
                globalOpacity = m_activeResources->m_Opacity;
            if (globalOpacity != 1.0f)
            {
                renderState &= ~(GS_BLEND_MASK | GS_DEPTHWRITE);
                renderState |= additiveMaterial ?
                    (GS_BLSRC_ONE | GS_BLDST_ONE) :
                    (GS_BLSRC_SRCALPHA | GS_BLDST_ONEMINUSSRCALPHA);
                // EF_SetResourcesState preserves this state for object-alpha
                // evaluation, which must not replace resource blending.
                m_RP.m_ResourceState = renderState;
                m_RP.m_fCurOpacity = m_activeResources->m_Opacity;
                m_RP.m_FlagsPerFlush |= RBSI_ALPHABLEND | RBSI_DEPTHWRITE | RBSI_ALPHAGEN;
                m_RP.m_ObjFlags &= ~FOB_LIGHTPASS;
                if (lightPasses.size() > 1 && !fixedFunctionMaterialLighting)
                {
                    lightPasses.resize(hardwareMultiLightsAmbient ? 2 : 1);
                    lightPassSpecularOcclusion.resize(lightPasses.size());
                    ambientOnlyLightPasses.resize(lightPasses.size());
                    translatedLights.resize(lightPasses.size());
                }
            }
        }
        globalOpacity = clamp_tpl(globalOpacity, 0.0f, 1.0f);
        if (m_activeHardwarePassType == eSHP_MAX)
        {
            // EF_SetResourcesState installs an 8-bit global color on the
            // fixed-function path, truncating rather than rounding opacity.
            globalOpacity = static_cast<uint8_t>(globalOpacity * 255.0f) / 255.0f;
        }
        if (fixedFunctionMaterialLighting)
        {
            // GL applies the light material once before the fixed pass loop.
            // Constant lighting installs a packed texture-environment color;
            // it does not multiply the vertex primary by ambient again.
            if (m_RP.m_RendPass <= 1)
            {
                if (fixedFunctionAmbientOnlyPass)
                {
                    EF_ConstantLightMaterial(m_activeResources->m_LMaterial,
                        hardwareLightFlags);
                    m_RP.m_CurGlobalColor = m_RP.m_NeedGlobalColor;
                }
                else
                {
                    m_RP.m_FlagsPerFlush &= ~(RBSI_GLOBALRGB | RBSI_GLOBALALPHA);
                    // EF_LightMaterial restores the primary-color selectors
                    // before mfSetTexture. A NOSET unit inherits these args;
                    // an explicit render-element override occurs afterward.
                    if (!(m_stageStateOverrides[0] & 4u))
                        m_stageColorArgs[0] = DEF_TEXARG0;
                    if (!(m_stageStateOverrides[0] & 8u))
                        m_stageAlphaArgs[0] = DEF_TEXARG0;
                }
            }
            if (fixedFunctionAmbientOnlyPass)
                globalOpacity = 1.0f;
        }
        if (m_activeStateShaderState &&
            (m_activeStateShaderState->m_Flags & (ESF_RGBGEN | ESF_ALPHAGEN)))
            for (int channel = 0; channel < 4; ++channel)
            {
                primaryColor[channel] = m_RP.m_NeedGlobalColor.bcolor[channel] / 255.0f;
                primaryColorMask[channel] = 1.0f;
            }
        // With a render element, these generators change the global color only.
        // Evaluate the same GL state transition without copying the vertex array.
        // Retained colors from a previous vertex-writing pass still take the full path.
        if (m_activePass && stockVertexFormatValid && colorOffset != 0xffffffffu &&
            IsGlobalOnlyColorPass(*m_activePass) &&
            !(m_stockColorCacheFrame == m_RP.m_Frame &&
              m_stockGeneratedColors.find(vertices) != m_stockGeneratedColors.end()))
        {
            if (EvaluateCpuDrawColor(m_activePass))
                for (int channel = 0; channel < 4; ++channel)
                {
                    primaryColor[channel] = m_RP.m_NeedGlobalColor.bcolor[channel] / 255.0f;
                    primaryColorMask[channel] = 1.0f;
                }
            m_RP.m_CurGlobalColor = m_RP.m_NeedGlobalColor;
        }
        else if (m_activePass && stockVertexFormatValid && colorOffset != 0xffffffffu)
        {
            uint8_t* scratch = ensureVertexCopy();
            const int first = m_RP.m_FirstVertex;
            const int count = m_RP.m_RendNumVerts;
            if (scratch && (m_activePass->m_eEvalAlpha != eEALPHA_Beam ||
                !m_RP.m_pRE || sourceTangentBasis) &&
                first >= 0 && count >= 0 && first <= vertices->m_NumVerts &&
                count <= vertices->m_NumVerts - first)
            {
                SCpuDrawStream stream;
                stream.source = sourcePositionData;
                stream.sourceStride = cpuSourceFormat.stride;
                for (uint32_t attribute = 0; attribute < cpuSourceFormat.attributeCount; ++attribute)
                {
                    const auto& sourceAttribute = cpuSourceFormat.attributes[attribute];
                    switch (sourceAttribute.location)
                    {
                    case 0: stream.sourcePositionOffset = sourceAttribute.offset; break;
                    case 1: stream.sourceNormalOffset = sourceAttribute.offset; break;
                    case 2: stream.sourceColorOffset = sourceAttribute.offset; break;
                    case 3: stream.sourceTexCoordOffset = sourceAttribute.offset; break;
                    case 4: stream.sourceSecondaryColorOffset = sourceAttribute.offset; break;
                    default: break;
                    }
                }
                stream.destination = scratch;
                stream.tangents = reinterpret_cast<const byte*>(sourceTangentBasis);
                stream.stride = stockVertexFormat.stride;
                stream.tangentStride = sizeof(SPipTangents);
                stream.firstVertex = first;
                stream.positionOffset = positionOffset == 0xffffffffu ? -1 : positionOffset;
                stream.normalOffset = normalOffset == 0xffffffffu ? -1 : normalOffset;
                stream.colorOffset = colorOffset;
                stream.secondaryColorOffset = secondaryColorOffset == 0xffffffffu ? -1 : secondaryColorOffset;
                stream.texCoordOffset = uv0Offset == 0xffffffffu ? -1 : uv0Offset;
                const SCpuDrawStream* savedStream = m_pCpuDrawStream;
                const auto savedPointer = m_RP.m_Ptr;
                const int savedStride = m_RP.m_Stride, savedColorOffset = m_RP.m_OffsD;
                const int savedNormalOffset = m_RP.m_OffsN, savedTexCoordOffset = m_RP.m_OffsT;
                m_pCpuDrawStream = &stream;
                m_RP.m_Ptr.PtrB = scratch + static_cast<size_t>(first) * stream.stride;
                m_RP.m_Stride = stream.stride;
                m_RP.m_OffsD = colorOffset;
                m_RP.m_OffsN = normalOffset == 0xffffffffu ? 0 : normalOffset;
                m_RP.m_OffsT = uv0Offset == 0xffffffffu ? 0 : uv0Offset;
                const bool generatedGlobalColor = EvaluateCpuDrawColor(m_activePass);
                // OpenGL's modified color stream survives subsequent passes
                // of this flush. The next private upload starts from it.
                StockGeneratedColors& retained = m_stockGeneratedColors[vertices];
                retained.vertexFormat = vertices->m_vertexformat;
                retained.colors.resize(static_cast<size_t>(vertices->m_NumVerts) * 4u);
                for (int vertex = 0; vertex < vertices->m_NumVerts; ++vertex)
                    memcpy(retained.colors.data() + static_cast<size_t>(vertex) * 4u,
                        scratch + static_cast<size_t>(vertex) * stream.stride + colorOffset, 4u);
                m_pCpuDrawStream = savedStream;
                m_RP.m_Ptr = savedPointer;
                m_RP.m_Stride = savedStride; m_RP.m_OffsD = savedColorOffset;
                m_RP.m_OffsN = savedNormalOffset; m_RP.m_OffsT = savedTexCoordOffset;
                if (generatedGlobalColor)
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        primaryColor[channel] = m_RP.m_NeedGlobalColor.bcolor[channel] / 255.0f;
                        primaryColorMask[channel] = 1.0f;
                    }
                // Even a guarded generator retains the preceding expected
                // texture-environment color at the OpenGL commit boundary.
                m_RP.m_CurGlobalColor = m_RP.m_NeedGlobalColor;
            }
            else
            {
                m_frameRenderer->RequirePanelFallback();
                EvaluatePassColor(*m_activePass, primaryColor, primaryColorMask);
            }
        }
        else if (m_activePass)
            EvaluatePassColor(*m_activePass, primaryColor, primaryColorMask);
        // OpenGL calls CCObject::SetAlphaState for FOB_HASALPHA objects on
        // general and light passes. Carry the object fade into the Vulkan
        // fragment opacity and reproduce its standard blend/depth-write
        // transition when neither the pass nor material selected a blend.
        if (applyResourceStates && m_RP.m_pRE &&
            m_RP.m_pRE->mfGetType() == eDATA_OcLeaf && m_RP.m_pCurObject &&
            (m_RP.m_pCurObject->m_ObjFlags & FOB_HASALPHA))
        {
            const float objectOpacity = clamp_tpl(m_RP.m_pCurObject->m_Color.a, 0.0f, 1.0f);
            objectAlphaOpacity = objectOpacity;
            m_RP.m_fCurOpacity = m_RP.m_pCurObject->m_Color.a;
            const uint32_t resourceBlendState = m_RP.m_ResourceState & GS_BLEND_MASK;
            const uint32_t objectBlendState = resourceBlendState ?
                resourceBlendState : (renderState & GS_BLEND_MASK);
            if (m_activeHardwarePassType == eSHP_MAX &&
                (objectBlendState == (GS_BLSRC_DSTCOL | GS_BLDST_ZERO) ||
                 objectBlendState == (GS_BLSRC_ZERO | GS_BLDST_SRCCOL)))
                objectAlphaTextureEnvMode = 1;
            else if (m_activeHardwarePassType == eSHP_MAX &&
                     objectBlendState == (GS_BLSRC_ONE | GS_BLDST_ONE))
                objectAlphaTextureEnvMode = 2;
            else if (m_activeHardwarePassType == eSHP_MAX)
                objectAlphaTextureEnvMode = 3;
            else
                globalOpacity *= objectOpacity;
            if (objectAlphaTextureEnvMode)
                globalOpacity = 1.0f;
            if (!(renderState & GS_BLEND_MASK) && !(m_RP.m_ResourceState & GS_BLEND_MASK))
            {
                renderState &= ~(GS_BLEND_MASK | GS_DEPTHWRITE);
                renderState |= GS_BLSRC_SRCALPHA | GS_BLDST_ONEMINUSSRCALPHA;
            }
            else if (renderState & GS_DEPTHFUNC_EQUAL)
                renderState &= ~GS_DEPTHFUNC_EQUAL;
            m_RP.m_FlagsPerFlush |= RBSI_ALPHABLEND | RBSI_DEPTHWRITE | RBSI_ALPHAGEN;
            if (objectOpacity != 1.0f &&
                m_activeHardwarePassType == eSHP_MAX &&
                ((renderState & GS_BLEND_MASK) == (GS_BLSRC_ONE | GS_BLDST_ONE)))
                additiveMaterial = true;
        }
        globalOpacity = clamp_tpl(globalOpacity, 0.0f, 1.0f);
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
                m_RP.m_NeedGlobalColor.bcolor[channel] =
                static_cast<uint8_t>(clamped * 255.0f);
                // The GL generator stores UCol before the color reaches
                // either primary-color modulation or texture env constants.
                // Both Vulkan inputs must consume that same truncated byte.
                primaryColor[channel] =
                    m_RP.m_NeedGlobalColor.bcolor[channel] / 255.0f;
            }
        }
        // Successful global generators install a complete packed color.
        // Keep the expected value available to later pass preparation,
        // separately from the committed texture environment cache.
        if (primaryColorMask[0] > 0.5f || primaryColorMask[1] > 0.5f ||
            primaryColorMask[2] > 0.5f || primaryColorMask[3] > 0.5f)
            m_RP.m_CurGlobalColor = m_RP.m_NeedGlobalColor;
        uint32_t textureEnvironmentColor = m_RP.m_CurGlobalColor.dcolor;
        if (objectAlphaTextureEnvMode)
        {
            UCol objectAlphaColor = m_RP.m_CurGlobalColor;
            const float rgbValue = objectAlphaTextureEnvMode == 1 ?
                1.0f - objectAlphaOpacity : objectAlphaOpacity;
            const uint8_t rgbByte = static_cast<uint8_t>(
                clamp_tpl(rgbValue, 0.0f, 1.0f) * 255.0f);
            const uint8_t alphaByte = static_cast<uint8_t>(objectAlphaOpacity * 255.0f);
            if (objectAlphaTextureEnvMode != 3)
            {
                objectAlphaColor.bcolor[0] = rgbByte;
                objectAlphaColor.bcolor[1] = rgbByte;
                objectAlphaColor.bcolor[2] = rgbByte;
            }
            objectAlphaColor.bcolor[3] = alphaByte;
            m_RP.m_NeedGlobalColor = objectAlphaColor;
            m_RP.m_CurGlobalColor = objectAlphaColor;
            m_RP.m_FlagsPerFlush |= objectAlphaTextureEnvMode == 3 ?
                RBSI_GLOBALALPHA : RBSI_GLOBALRGB;
            textureEnvironmentColor = objectAlphaColor.dcolor;
        }

        const auto resolveObjectTextureId = [&](int resourceSlot, int textureStage) -> int
        {
            if (!m_RP.m_pCurObject)
                return 0;
            switch (resourceSlot)
            {
            case EFTT_LIGHTMAP: return m_RP.m_pCurObject->m_nLMId;
            case EFTT_LIGHTMAP_DIR: return m_RP.m_pCurObject->m_nLMDirId;
            case EFTT_OCCLUSION: return m_RP.m_pCurObject->m_nOcclId;
            default: break;
            }
            // Techniques can declare their lightmap solely through the
            // eSrcPointer_TexLM pointer; in that case the stage carries no
            // EFTT_LIGHTMAP template bind, but OpenGL still uses the object's
            // m_nLMId for the selected lightmap stage. An explicit resource
            // bind (including LIGHTMAP_HDR) keeps its own OpenGL resource and
            // must not be replaced just because it occupies that UV stage.
            return resourceSlot < 0 && textureStage == lightmapStage ?
                m_RP.m_pCurObject->m_nLMId : 0;
        };
        const auto resolveCustomTextureId = [&](const SShaderTexUnit* unit) -> int
        {
            if (!unit || !unit->m_TexPic)
                return 0;
            const int bind = unit->m_TexPic->m_Bind;
            if (bind < TO_FROMRE0 || bind > TO_FROMRE7)
                return 0;
            const int customStage = bind - TO_FROMRE0;
            return m_RP.m_pRE ? m_RP.m_pRE->m_CustomTexBind[customStage] :
                                m_RP.m_RECustomTexBind[customStage];
        };
        // Cg terrain passes generate every detail UV from FromRE projection
        // planes. Their texture units deliberately have no fixed-function
        // texgen; the terrain translation below supplies those coordinates.
        const bool terrainProgramTexgen = stockHardwarePass &&
            programInfo.terrainTexgen;
        const auto resolveStageTextureId = [&](const SShaderTexUnit* unit,
                                                int resourceSlot,
                                                int textureStage) -> int
        {
            // mfSetTextures runs before mfDraw in GL. A later explicit
            // element binding wins over the pass/resource texture token.
            if (textureStage >= 0 && textureStage < 8 &&
                (m_stageStateOverrides[textureStage] & 32u))
                return m_stageTextureIds[textureStage];
            // GLTextures::mfSet uses the object's image for $FromObj
            // (particle sprites), rather than binding the placeholder.
            if (unit && unit->m_TexPic && unit->m_TexPic->m_Bind == TO_FROMOBJ)
                return m_RP.m_pCurObject ? crymax(0, m_RP.m_pCurObject->m_NumCM) : 0;
            const int objectTextureId = resolveObjectTextureId(resourceSlot, textureStage);
            if (objectTextureId > 0)
                return objectTextureId;
            const int customTextureId = resolveCustomTextureId(unit);
            if (customTextureId > 0)
                return customTextureId;
            // A FromRE unit's GetTextureID is its reserved binding token, not
            // the texture selected for the current render element. Do not
            // expose that token as a Vulkan legacy texture ID when the
            // render element has no binding for this stage.
            if (unit && unit->m_TexPic &&
                unit->m_TexPic->m_Bind >= TO_FROMRE0 &&
                unit->m_TexPic->m_Bind <= TO_FROMRE7)
                return 0;
            return unit && unit->m_ITexPic ? unit->m_ITexPic->GetTextureID() : 0;
        };

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
                        // Use the stock resolver: its first fallback combines
                        // the material directory with the basename, handling
                        // obsolete exporter/server paths in character assets.
                        ITexPic* loaded = m_cEF.LoadVulkanResourceTexture(diffuse->m_Name.c_str(),
                            m_activeResources->m_TexturePath.c_str(), textureFlags,
                            textureFlags2, eTT_Base, m_RP.m_pShader, diffuse,
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
            // WaterVolume has no template texture stages, although its
            // resources carry an animated normal map in EFTT_BUMP.
            if (textureIds[0] <= 0 && m_RP.m_pShader &&
                !FastAsciiCaseCompare(m_RP.m_pShader->m_Name.c_str(), "watervolume"))
            {
                SEfResTexture* waterNormal = m_activeResources->m_Textures[EFTT_BUMP];
                if (waterNormal)
                {
                    if (!waterNormal->m_TU.m_ITexPic && !waterNormal->m_Name.empty())
                    {
                        const uint textureFlags = waterNormal->m_TU.GetTexFlags();
                        const uint textureFlags2 = waterNormal->m_TU.GetTexFlags2();
                        ITexPic* loaded = EF_LoadTexture(waterNormal->m_Name.c_str(),
                            textureFlags, textureFlags2, eTT_Base,
                            static_cast<float>(waterNormal->m_Amount));
                        if ((!loaded || !loaded->IsTextureLoaded()) &&
                            !m_activeResources->m_TexturePath.empty())
                        {
                            if (loaded)
                                loaded->Release(false);
                            std::string texturePath = m_activeResources->m_TexturePath.c_str();
                            if (!texturePath.empty() && texturePath.back() != '/' &&
                                texturePath.back() != '\\')
                                texturePath.push_back('/');
                            texturePath += waterNormal->m_Name.c_str();
                            loaded = EF_LoadTexture(texturePath.c_str(), textureFlags,
                                textureFlags2, eTT_Base,
                                static_cast<float>(waterNormal->m_Amount));
                        }
                        if (loaded && loaded->IsTextureLoaded())
                            waterNormal->m_TU.m_ITexPic = loaded;
                        else if (loaded)
                            loaded->Release(false);
                    }
                    waterNormal->m_TU.mfUpdate();
                    if (waterNormal->m_TU.m_ITexPic)
                    {
                        textureIds[0] = waterNormal->m_TU.m_ITexPic->GetTextureID();
                        textureLodBias[0] = waterNormal->m_TU.m_fTexFilterLodBias;
                    }
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
                if (stage == 1 && textureCoordinatePointers[stage])
                    textureStage1UsesTexCoord1 =
                        textureCoordinatePointers[stage]->ePT == eSrcPointer_TexLM;
                else if (stage == 1 && hasLightmapTexCoordSource)
                    textureStage1UsesTexCoord1 = lightmapStage == 1 ||
                        resourceSlot == EFTT_LIGHTMAP ||
                        resourceSlot == EFTT_LIGHTMAP_DIR ||
                        resourceSlot == EFTT_OCCLUSION;
                applyStockTextureMatrixOps(stage, textureMatrices[stage]);
                generateLegacyTexCoords(stage, unit, textureMatrices[stage]);
                const_cast<SShaderTexUnit*>(unit)->mfUpdate();
                textureLodBias[stage] = unit->m_fTexFilterLodBias;
                // Match GL/D3D: object-owned lightmap, directional-lightmap,
                // and occlusion IDs override the corresponding resource slot.
                const int resolvedTextureId = resolveStageTextureId(unit, resourceSlot, stage);
                captureShadowStage(stage, unit, resolvedTextureId);
                if (m_stageStateOverrides[stage] & 32u)
                    textureIds[stage] = resolvedTextureId;
                if (resolvedTextureId > 0)
                {
                    textureIds[stage] = resolvedTextureId;
                    // Use the texture selected by this actual template pass.
                    // EFTT_BUMP may be a stock-combined BUMP+NORMALMAP image;
                    // EFTT_BUMP_DIFFUSE is the corresponding $BumpDiffuse map.
                    if (resourceSlot == EFTT_BUMP || resourceSlot == EFTT_BUMP_DIFFUSE)
                    {
                        normalMapTextureId = textureIds[stage];
                        normalMapTextureStage = stage;
                        normalMapLodBias = textureLodBias[stage];
                    }
                }
                // GL's mfSetTexture resolves the image through pSTU, but
                // installs combiner operations from the pass itself (this).
                // Material texture defaults must not replace decal opacity
                // or multiplicative-decal combiner arguments.
                const SShaderTexUnit& passUnit = m_activePass->m_TUnits[stage];
                colorOps[stage] = passUnit.m_eColorOp;
                alphaOps[stage] = passUnit.m_eAlphaOp;
                colorArgs[stage] = passUnit.m_eColorArg;
                alphaArgs[stage] = passUnit.m_eAlphaArg;
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
                generateLegacyTexCoords(2, unit, stage2Matrix);
                const_cast<SShaderTexUnit*>(unit)->mfUpdate();
                const int resolvedTextureId = resolveStageTextureId(unit, resourceSlot, 2);
                captureShadowStage(2, unit, resolvedTextureId);
                if (resolvedTextureId > 0)
                {
                    if (!terrainProgramTexgen && !linearTexgen[2].enabled && unit->m_eGenTC != eGTC_Base && unit->m_eGenTC != eGTC_LightMap &&
                        unit->m_eGenTC != eGTC_ShadowMap)
                    {
                        // This three-stage shader can address either UV set
                        // already carried by the stock vertex layout. Shader
                        // generated coordinates still need a dedicated path.
                        m_frameRenderer->RequirePanelFallback();
                    }
                    else
                    {
                        const bool useTexCoord1 = textureCoordinatePointers[2] ?
                            textureCoordinatePointers[2]->ePT == eSrcPointer_TexLM :
                            (unit->m_eGenTC == eGTC_LightMap ||
                                                  lightmapStage == 2 ||
                                                  resourceSlot == EFTT_LIGHTMAP ||
                                                  resourceSlot == EFTT_LIGHTMAP_DIR ||
                                                  resourceSlot == EFTT_OCCLUSION);
                        textureStage2.textureId = resolvedTextureId;
                        const SShaderTexUnit& passUnit = m_activePass->m_TUnits[2];
                        textureStage2.colorOp = passUnit.m_eColorOp;
                        textureStage2.alphaOp = passUnit.m_eAlphaOp;
                        textureStage2.colorArg = passUnit.m_eColorArg;
                        textureStage2.alphaArg = passUnit.m_eAlphaArg;
                        textureStage2.lodBias = unit->m_fTexFilterLodBias;
                        textureStage2.useTexCoord1 = useTexCoord1 &&
                            (!lightmapTexCoords.empty() || vertices->m_vertexformat == 16);
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
                generateLegacyTexCoords(3, unit, stage3Matrix);
                const_cast<SShaderTexUnit*>(unit)->mfUpdate();
                const int resolvedTextureId = resolveStageTextureId(unit, resourceSlot, 3);
                captureShadowStage(3, unit, resolvedTextureId);
                if (resolvedTextureId > 0)
                {
                    if (!terrainProgramTexgen && !linearTexgen[3].enabled && unit->m_eGenTC != eGTC_Base && unit->m_eGenTC != eGTC_LightMap &&
                        unit->m_eGenTC != eGTC_ShadowMap)
                        m_frameRenderer->RequirePanelFallback();
                    else
                    {
                        const bool useTexCoord1 = textureCoordinatePointers[3] ?
                            textureCoordinatePointers[3]->ePT == eSrcPointer_TexLM :
                            (unit->m_eGenTC == eGTC_LightMap ||
                                                  lightmapStage == 3 ||
                                                  directionalLightmapPass ||
                                                  resourceSlot == EFTT_LIGHTMAP ||
                                                  resourceSlot == EFTT_LIGHTMAP_DIR ||
                                                  resourceSlot == EFTT_OCCLUSION);
                        textureStage3.textureId = resolvedTextureId;
                        const SShaderTexUnit& passUnit = m_activePass->m_TUnits[3];
                        textureStage3.colorOp = passUnit.m_eColorOp;
                        textureStage3.alphaOp = passUnit.m_eAlphaOp;
                        textureStage3.colorArg = passUnit.m_eColorArg;
                        textureStage3.alphaArg = passUnit.m_eAlphaArg;
                        textureStage3.lodBias = unit->m_fTexFilterLodBias;
                        textureStage3.useTexCoord1 = useTexCoord1 &&
                            (!lightmapTexCoords.empty() || vertices->m_vertexformat == 16);
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
                generateLegacyTexCoords(stageIndex, unit, stageMatrix);
                const_cast<SShaderTexUnit*>(unit)->mfUpdate();
                CryVR::VulkanStockTextureStage& translated = textureStages4To7[stageIndex - 4];
                const SShaderTexUnit& passUnit = m_activePass->m_TUnits[stageIndex];
                translated.colorOp = passUnit.m_eColorOp;
                translated.alphaOp = passUnit.m_eAlphaOp;
                translated.colorArg = passUnit.m_eColorArg;
                translated.alphaArg = passUnit.m_eAlphaArg;
                translated.lodBias = unit->m_fTexFilterLodBias;
                translated.constant = 0xffffffffu;
                if (m_stageStateOverrides[stageIndex] & 1u) translated.colorOp = m_stageColorOps[stageIndex];
                if (m_stageStateOverrides[stageIndex] & 2u) translated.alphaOp = m_stageAlphaOps[stageIndex];
                if (m_stageStateOverrides[stageIndex] & 4u) translated.colorArg = m_stageColorArgs[stageIndex];
                if (m_stageStateOverrides[stageIndex] & 8u) translated.alphaArg = m_stageAlphaArgs[stageIndex];
                const int resolvedTextureId = resolveStageTextureId(unit, resourceSlot, stageIndex);
                captureShadowStage(stageIndex, unit, resolvedTextureId);
                if (resolvedTextureId <= 0)
                    continue;
                if (!linearTexgen[stageIndex].enabled && unit->m_eGenTC != eGTC_Base && unit->m_eGenTC != eGTC_LightMap &&
                    unit->m_eGenTC != eGTC_ShadowMap)
                {
                    m_frameRenderer->RequirePanelFallback();
                    continue;
                }
                translated.useTexCoord1 =
                    (textureCoordinatePointers[stageIndex] ?
                     textureCoordinatePointers[stageIndex]->ePT == eSrcPointer_TexLM :
                     (unit->m_eGenTC == eGTC_LightMap ||
                     stageIndex == lightmapStage ||
                     resourceSlot == EFTT_LIGHTMAP ||
                     resourceSlot == EFTT_LIGHTMAP_DIR ||
                     resourceSlot == EFTT_OCCLUSION)) &&
                    (!lightmapTexCoords.empty() || vertices->m_vertexformat == 16);
                setStockUvTransform(translated, stageMatrix);
                translated.textureId = resolvedTextureId;
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
                textureStage2.lodBias = m_stageLodBias[2];
                textureStage2.colorOp = m_stageColorOps[2];
                textureStage2.alphaOp = m_stageAlphaOps[2];
                textureStage2.colorArg = m_stageColorArgs[2];
                textureStage2.alphaArg = m_stageAlphaArgs[2];
                textureStage2.useTexCoord1 = !lightmapTexCoords.empty() ||
                    vertices->m_vertexformat == 16;
                textureStage2Ptr = &textureStage2;
            }
            if (m_stageTextureIds[3] > 0)
            {
                textureStage3.textureId = m_stageTextureIds[3];
                textureStage3.lodBias = m_stageLodBias[3];
                textureStage3.colorOp = m_stageColorOps[3];
                textureStage3.alphaOp = m_stageAlphaOps[3];
                textureStage3.colorArg = m_stageColorArgs[3];
                textureStage3.alphaArg = m_stageAlphaArgs[3];
                textureStage3.useTexCoord1 = !lightmapTexCoords.empty() ||
                    vertices->m_vertexformat == 16;
                textureStage3Ptr = &textureStage3;
            }
            for (int stageIndex = 4; stageIndex < 8; ++stageIndex)
            {
                if (m_stageTextureIds[stageIndex] <= 0)
                    continue;
                CryVR::VulkanStockTextureStage& translated = textureStages4To7[stageIndex - 4];
                translated.textureId = m_stageTextureIds[stageIndex];
                translated.lodBias = m_stageLodBias[stageIndex];
                translated.colorOp = m_stageColorOps[stageIndex];
                translated.alphaOp = m_stageAlphaOps[stageIndex];
                translated.colorArg = m_stageColorArgs[stageIndex];
                translated.alphaArg = m_stageAlphaArgs[stageIndex];
                translated.useTexCoord1 = true;
                translated.wrapMode = TextureWrapModeForId(translated.textureId);
            }
            textureStages4To7Ptr = textureStages4To7;
        }
        if (m_activePass)
        {
            // GL mfSetTexture gates all four combiner fields on the pass's
            // color op. NOSET preserves the complete prior TMU combiner,
            // even if alpha op/arguments in this unit contain other values.
            for (int stage = 0; stage < 8 && stage < m_activePass->m_TUnits.Num(); ++stage)
            {
                const SShaderTexUnit& passUnit = m_activePass->m_TUnits[stage];
                const uint8_t overrides = m_stageStateOverrides[stage];
                if (passUnit.m_eColorOp != eCO_NOSET)
                {
                    // mfSetTexture installs all four cached combiner fields.
                    // Keep the legacy cache current for later NOSET passes and
                    // global-color substitution, preserving element overrides.
                    if (!(overrides & 1u)) m_stageColorOps[stage] = passUnit.m_eColorOp;
                    if (!(overrides & 2u)) m_stageAlphaOps[stage] = passUnit.m_eAlphaOp;
                    if (!(overrides & 4u)) m_stageColorArgs[stage] = passUnit.m_eColorArg;
                    if (!(overrides & 8u)) m_stageAlphaArgs[stage] = passUnit.m_eAlphaArg;
                    continue;
                }
                if (stage < 2)
                {
                    if (!(overrides & 1u)) colorOps[stage] = 255;
                    if (!(overrides & 2u)) alphaOps[stage] = 255;
                    if (!(overrides & 4u)) colorArgs[stage] = m_stageColorArgs[stage];
                    if (!(overrides & 8u)) alphaArgs[stage] = m_stageAlphaArgs[stage];
                }
                else
                {
                    auto& translated = stage == 2 ? textureStage2 :
                        (stage == 3 ? textureStage3 : textureStages4To7[stage - 4]);
                    if (!(overrides & 1u)) translated.colorOp = 255;
                    if (!(overrides & 2u)) translated.alphaOp = 255;
                    if (!(overrides & 4u)) translated.colorArg = m_stageColorArgs[stage];
                    if (!(overrides & 8u)) translated.alphaArg = m_stageAlphaArgs[stage];
                }
            }
        }
        // A render element can refine bias after the material binds its
        // texture units. Preserve that final TMU value for later direct draws.
        const auto commitStockLodBias = [&](int stage, float& bias)
        {
            if (m_stageStateOverrides[stage] & 16u)
                bias = m_stageLodBias[stage];
            else
                m_stageLodBias[stage] = bias;
        };
        for (int stage = 0; stage < 2; ++stage)
        {
            if (m_stageStateOverrides[stage] & 1u) colorOps[stage] = m_stageColorOps[stage];
            if (m_stageStateOverrides[stage] & 2u) alphaOps[stage] = m_stageAlphaOps[stage];
            if (m_stageStateOverrides[stage] & 4u) colorArgs[stage] = m_stageColorArgs[stage];
            if (m_stageStateOverrides[stage] & 8u) alphaArgs[stage] = m_stageAlphaArgs[stage];
        }
        // EF_CommitTexStageState substitutes only argument 1 of stage zero.
        // These are effective draw arguments, not edits to cached material
        // state; keep the other selectors and every higher stage intact.
        if (m_RP.m_FlagsPerFlush & RBSI_GLOBALRGB)
        {
            if (colorArgs[0] == 255u) colorArgs[0] = m_stageColorArgs[0];
            colorArgs[0] = (colorArgs[0] & ~(7u << 3)) |
                (static_cast<uint32_t>(eCA_Constant) << 3);
        }
        if (m_RP.m_FlagsPerFlush & RBSI_GLOBALALPHA)
        {
            if (alphaArgs[0] == 255u) alphaArgs[0] = m_stageAlphaArgs[0];
            alphaArgs[0] = (alphaArgs[0] & ~(7u << 3)) |
                (static_cast<uint32_t>(eCA_Constant) << 3);
        }
        if (objectAlphaTextureEnvMode)
        {
            // Match CCObject::SetAlphaState's fixed-function texture-env
            // changes for its two nonstandard blend pairs.
            if (objectAlphaTextureEnvMode != 3)
                colorOps[0] = objectAlphaTextureEnvMode == 1 ? eCO_ADD : eCO_MODULATE;
            alphaOps[0] = eCO_MODULATE;
            const uint32_t textureTimesConstant = static_cast<uint32_t>(
                eCA_Texture | (eCA_Constant << 3));
            if (objectAlphaTextureEnvMode != 3)
                colorArgs[0] = textureTimesConstant;
            alphaArgs[0] = textureTimesConstant;
        }
        if (m_forceCurrentTextureForClientDraw)
        {
            if (m_currentTextureId > 0) textureIds[0] = m_currentTextureId;
            // The render element has already set its GL state. Do not
            // replace its global constant with the enclosing pass RGBGen.
            UCol clientConstant;
            for (int channel = 0; channel < 4; ++channel)
                clientConstant.bcolor[channel] = static_cast<byte>(
                    clamp_tpl(m_materialColor[channel], 0.0f, 1.0f) * 255.0f);
            textureEnvironmentColor = clientConstant.dcolor;
            memset(primaryColorMask, 0, sizeof(primaryColorMask));
        }

        // NULL's loaded flag does not prove that an image exists in Vulkan.
        // OpenGL's bind path uploads/reloads such images before sampling them.
        // Recover a missing file-backed base image once, keeping its bind ID.
        if (textureIds[0] > 0 && !m_frameRenderer->HasLegacyTexture(textureIds[0]))
        {
            static std::vector<int> attemptedTextureUploads;
            STexPic* image = m_TexMan ? m_TexMan->GetByID(textureIds[0]) : nullptr;
            if (image && image->m_SearchName.c_str()[0] && image->m_SearchName.c_str()[0] != '$' &&
                std::find(attemptedTextureUploads.begin(), attemptedTextureUploads.end(), textureIds[0]) == attemptedTextureUploads.end())
            {
                attemptedTextureUploads.push_back(textureIds[0]);
                EF_LoadTexture(image->m_SearchName.c_str(), image->m_Flags,
                    image->m_Flags2 | FT2_RELOAD, image->m_eTT,
                    image->m_fAmount1, image->m_fAmount2, image->m_Id, image->m_Bind);
            }
        }
        if (lightmapStage == 1 && textureIds[1] <= 0 && m_RP.m_pCurObject &&
            m_RP.m_pCurObject->m_nLMId > 0)
            textureIds[1] = m_RP.m_pCurObject->m_nLMId;
        if (lightmapStage == 1 && textureIds[1] <= 0 && m_activeResources &&
            m_activeResources->m_Textures[EFTT_LIGHTMAP])
        {
            SEfResTexture* lightmap = m_activeResources->m_Textures[EFTT_LIGHTMAP];
            lightmap->m_TU.mfUpdate();
            if (lightmap->m_TU.m_ITexPic && lightmap->m_TU.m_ITexPic->IsTextureLoaded())
                textureIds[1] = lightmap->m_TU.m_ITexPic->GetTextureID();
        }

        const bool hardwareBumpLightPass = hardwareMaterialLightPass &&
            !(activeHardwareLightFlags & LMF_NOBUMP) && m_RP.m_pShader &&
            m_RP.m_pShader->m_eSort != eS_Decal;
        if ((directionalLightmapPass || hardwareBumpLightPass) && m_activeResources && m_RP.m_pCurObject)
        {
            SEfResTexture* bump = m_activeResources->m_Textures[EFTT_BUMP];
            if (bump && (!bump->m_TU.m_ITexPic ||
                         !bump->m_TU.m_ITexPic->IsTextureLoaded()) &&
                !bump->m_Name.empty())
            {
                const char* resourcePath = m_activeResources->m_TexturePath.c_str();
                auto failedLoad = m_failedBumpTextureLoads.find(bump);
                bool retryLoad = failedLoad == m_failedBumpTextureLoads.end();
                if (failedLoad != m_failedBumpTextureLoads.end())
                {
                    const FailedBumpTextureLoad& cached = failedLoad->second;
                    const bool sameResource = cached.name == bump->m_Name.c_str() &&
                        cached.path == resourcePath;
                    retryLoad = !sameResource;
                    if (!sameResource)
                        m_failedBumpTextureLoads.erase(failedLoad);
                }
                if (retryLoad)
                {
                    const uint textureFlags = bump->m_TU.GetTexFlags();
                    const uint textureFlags2 = bump->m_TU.GetTexFlags2();
                    ITexPic* loaded = EF_LoadTexture(bump->m_Name.c_str(), textureFlags,
                        textureFlags2, eTT_Bumpmap, static_cast<float>(bump->m_Amount));
                    if ((!loaded || !loaded->IsTextureLoaded()) && resourcePath[0])
                    {
                        if (loaded)
                            loaded->Release(false);
                        std::string texturePath = resourcePath;
                        if (texturePath.back() != '/' && texturePath.back() != '\\')
                            texturePath.push_back('/');
                        texturePath += bump->m_Name.c_str();
                        loaded = EF_LoadTexture(texturePath.c_str(), textureFlags,
                            textureFlags2, eTT_Bumpmap, static_cast<float>(bump->m_Amount));
                    }
                    if (loaded && loaded->IsTextureLoaded())
                    {
                        bump->m_TU.m_ITexPic = loaded;
                        m_failedBumpTextureLoads.erase(bump);
                    }
                    else
                    {
                        if (loaded)
                            loaded->Release(false);
                        if (m_failedBumpTextureLoads.size() >= 2048 &&
                            m_failedBumpTextureLoads.find(bump) == m_failedBumpTextureLoads.end())
                            m_failedBumpTextureLoads.erase(m_failedBumpTextureLoads.begin());
                        FailedBumpTextureLoad& cached = m_failedBumpTextureLoads[bump];
                        cached.name = bump->m_Name.c_str();
                        cached.path = resourcePath;
                    }
                }
            }
            if (bump)
            {
                // AmbPassTempl binds EFTT_BUMP outside SShaderPassHW::m_TUnits,
                // but OpenGL still applies the material's stage-1 texture
                // transform and the active technique/pass matrix operations.
                updateStockTextureMatrix(bump, 1, textureMatrices[1]);
                applyStockTextureMatrixOps(1, textureMatrices[1]);
                bump->m_TU.mfUpdate();
                if (bump->m_TU.m_ITexPic)
                {
                    textureIds[1] = bump->m_TU.m_ITexPic->GetTextureID();
                    textureLodBias[1] = bump->m_TU.m_fTexFilterLodBias;
                    textureStage1UsesTexCoord1 = false;
                    if (hardwareBumpLightPass && !directionalLightmapPass)
                    {
                        normalMapTextureId = textureIds[1];
                        normalMapTextureStage = 1;
                        normalMapLodBias = textureLodBias[1];
                    }
                }
            }
            if (directionalLightmapPass && (!bump ||
                (stockAmbientTemplate && !(stockHardwarePass->m_StockProgramMask & 0x1000ull))))
            {
                // CGRCAmbientTempl initializes the tangent normal to (0,0,1)
                // when this variant has no bump sampler. It still evaluates
                // direction alpha and directional lightmap intensity.
                if (!m_flatLightmapNormalTextureId)
                {
                    byte flatNormal[4] = {128, 128, 255, 255};
                    STexPic* flat = m_TexMan->CreateTexture("$VulkanFlatLightmapNormal", 1, 1, 1,
                        FT_NOMIPS | FT_NOSTREAM, 0, flatNormal, eTT_Base,
                        -1.0f, -1.0f, 0, nullptr, 0, eTF_8888);
                    if (flat) m_flatLightmapNormalTextureId = flat->GetTextureID();
                }
                textureIds[1] = m_flatLightmapNormalTextureId;
                textureStage1UsesTexCoord1 = false;
            }
            if (directionalLightmapPass && textureIds[1] > 0)
            {
                int lightmapTextureId = m_RP.m_pCurObject->m_nLMId;
                if (lightmapTextureId <= 0 &&
                    m_activeResources->m_Textures[EFTT_LIGHTMAP])
                {
                    SEfResTexture* lightmap =
                        m_activeResources->m_Textures[EFTT_LIGHTMAP];
                    lightmap->m_TU.mfUpdate();
                    if (lightmap->m_TU.m_ITexPic)
                        lightmapTextureId = lightmap->m_TU.m_ITexPic->GetTextureID();
                }
                int lightmapDirectionTextureId = m_RP.m_pCurObject->m_nLMDirId;
                if (lightmapDirectionTextureId <= 0 &&
                    m_activeResources->m_Textures[EFTT_LIGHTMAP_DIR])
                {
                    SEfResTexture* lightmapDirection =
                        m_activeResources->m_Textures[EFTT_LIGHTMAP_DIR];
                    lightmapDirection->m_TU.mfUpdate();
                    if (lightmapDirection->m_TU.m_ITexPic)
                        lightmapDirectionTextureId =
                            lightmapDirection->m_TU.m_ITexPic->GetTextureID();
                }
                textureStage2.textureId = lightmapTextureId;
                textureStage2.colorOp = eCO_MODULATE;
                textureStage2.alphaOp = eCO_MODULATE;
                textureStage2.useTexCoord1 = !lightmapTexCoords.empty() ||
                    vertices->m_vertexformat == 16;
                textureStage2.wrapMode = TextureWrapModeForId(textureStage2.textureId);
                textureStage3.textureId = lightmapDirectionTextureId;
                textureStage3.colorOp = eCO_MODULATE;
                textureStage3.alphaOp = eCO_MODULATE;
                textureStage3.useTexCoord1 = !lightmapTexCoords.empty() ||
                    vertices->m_vertexformat == 16;
                textureStage3.wrapMode = TextureWrapModeForId(textureStage3.textureId);
                float lightmapMatrix[16]{};
                lightmapMatrix[0] = lightmapMatrix[5] =
                    lightmapMatrix[10] = lightmapMatrix[15] = 1.0f;
                setStockUvTransform(textureStage2, lightmapMatrix);
                setStockUvTransform(textureStage3, lightmapMatrix);
                textureStage2Ptr = &textureStage2;
                textureStage3Ptr = &textureStage3;
            }
            else if (directionalLightmapPass)
                m_frameRenderer->RequirePanelFallback();
        }

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
        float waterReflectionModelView[16]{};
        float waterReflectionClipPlane[4]{};
        const bool hasWaterReflectionTransform = CV_r_waterreflections && iSystem &&
            iSystem->GetI3DEngine();
        if (hasWaterReflectionTransform)
        {
            const float waterLevel = iSystem->GetI3DEngine()->GetWaterLevel();
            float reflection[16]{};
            reflection[0] = reflection[5] = reflection[15] = 1.0f;
            reflection[10] = -1.0f;
            reflection[14] = 2.0f * waterLevel;
            float identity[16]{};
            identity[0] = identity[5] = identity[10] = identity[15] = 1.0f;
            const float* objectMatrix = identity;
            if (m_RP.m_pCurObject && (m_RP.m_pCurObject->m_ObjFlags & FOB_TRANS_MASK))
                objectMatrix = m_RP.m_pCurObject->m_Matrix.GetData();
            float reflectedObject[16]{};
            mathMatrixMultiply(reflectedObject, reflection,
                               const_cast<float*>(objectMatrix), g_CpuFlags);
            mathMatrixMultiply(waterReflectionModelView, m_CameraMatrix.GetData(),
                               reflectedObject, g_CpuFlags);

            waterReflectionClipPlane[2] = 1.0f;
            waterReflectionClipPlane[3] = -waterLevel;
            if (GetCamera().GetPos().z < waterLevel)
            {
                waterReflectionClipPlane[2] = -1.0f;
                waterReflectionClipPlane[3] = waterLevel;
            }
            if (m_RP.m_pCurObject && (m_RP.m_pCurObject->m_ObjFlags & FOB_TRANS_MASK))
            {
                const float worldPlane[4] = {
                    0.0f, 0.0f, waterReflectionClipPlane[2], waterReflectionClipPlane[3]
                };
                for (int row = 0; row < 4; ++row)
                    waterReflectionClipPlane[row] =
                        worldPlane[0] * m_RP.m_pCurObject->m_Matrix(row, 0) +
                        worldPlane[1] * m_RP.m_pCurObject->m_Matrix(row, 1) +
                        worldPlane[2] * m_RP.m_pCurObject->m_Matrix(row, 2) +
                        worldPlane[3] * m_RP.m_pCurObject->m_Matrix(row, 3);
            }
        }
        CryVR::VulkanWaterReflectionUpdate waterReflectionUpdate{};
        const CryVR::VulkanWaterReflectionUpdate* waterReflectionUpdatePtr = nullptr;
        if (hasWaterReflectionTransform)
        {
            I3DEngine* engine = iSystem->GetI3DEngine();
            if (engine)
            {
                waterReflectionUpdate.realTime = m_RP.m_RealTime;
                waterReflectionUpdate.updateInterval =
                    engine->GetDistanceToSectorWithWater() * CRenderer::CV_r_waterupdateFactor;
                if (waterReflectionUpdate.updateInterval > 0.3f)
                    waterReflectionUpdate.updateInterval = 0.3f;
                waterReflectionUpdate.cameraDistanceThreshold = CRenderer::CV_r_waterupdateDistance;
                waterReflectionUpdate.cameraAngleThreshold = CRenderer::CV_r_waterupdateDeltaAngle;
                const Vec3d cameraPosition = GetCamera().GetPos();
                const Vec3d cameraAngles = GetCamera().GetAngles();
                waterReflectionUpdate.cameraPosition[0] = cameraPosition.x;
                waterReflectionUpdate.cameraPosition[1] = cameraPosition.y;
                waterReflectionUpdate.cameraPosition[2] = cameraPosition.z;
                waterReflectionUpdate.cameraAngles[0] = cameraAngles.x;
                waterReflectionUpdate.cameraAngles[1] = cameraAngles.y;
                waterReflectionUpdate.cameraAngles[2] = cameraAngles.z;
                waterReflectionUpdate.fieldOfView = GetCamera().GetFov();
                waterReflectionUpdatePtr = &waterReflectionUpdate;
            }
        }
        // Implicit hardware-resource binding (including AmbPassTempl bump)
        // is reconstructed above. In GL it precedes the element's mfDraw,
        // so an explicit element bind must remain the final sampler choice.
        const auto applyExplicitStageState = [&](int stage,
                                                  CryVR::VulkanStockTextureStage& translated)
        {
            const uint8_t overrides = m_stageStateOverrides[stage];
            const bool addedStage = m_activePass && stage >= m_activePass->m_TUnits.Num();
            if (addedStage || (overrides & 1u)) translated.colorOp = m_stageColorOps[stage];
            if (addedStage || (overrides & 2u)) translated.alphaOp = m_stageAlphaOps[stage];
            if (addedStage || (overrides & 4u)) translated.colorArg = m_stageColorArgs[stage];
            if (addedStage || (overrides & 8u)) translated.alphaArg = m_stageAlphaArgs[stage];
            if (addedStage || (overrides & 16u)) translated.lodBias = m_stageLodBias[stage];
        };
        for (int stage = 0; stage < 2; ++stage)
        {
            if (m_stageStateOverrides[stage] & 32u)
                textureIds[stage] = m_stageTextureIds[stage];
        }
        if (m_stageStateOverrides[2] & 32u)
        {
            textureStage2.textureId = m_stageTextureIds[2];
            applyExplicitStageState(2, textureStage2);
            textureStage2.wrapMode = TextureWrapModeForId(textureStage2.textureId);
            textureStage2Ptr = textureStage2.textureId > 0 ? &textureStage2 : nullptr;
        }
        if (m_stageStateOverrides[3] & 32u)
        {
            textureStage3.textureId = m_stageTextureIds[3];
            applyExplicitStageState(3, textureStage3);
            textureStage3.wrapMode = TextureWrapModeForId(textureStage3.textureId);
            textureStage3Ptr = textureStage3.textureId > 0 ? &textureStage3 : nullptr;
        }
        for (int stage = 4; stage < 8; ++stage)
        {
            if (!(m_stageStateOverrides[stage] & 32u)) continue;
            auto& translated = textureStages4To7[stage - 4];
            translated.textureId = m_stageTextureIds[stage];
            applyExplicitStageState(stage, translated);
            translated.wrapMode = TextureWrapModeForId(translated.textureId);
            textureStages4To7Ptr = textureStages4To7;
        }
        if (normalMapTextureStage >= 0 &&
            (m_stageStateOverrides[normalMapTextureStage] & 32u))
        {
            normalMapTextureId = m_stageTextureIds[normalMapTextureStage];
            normalMapLodBias = (m_stageStateOverrides[normalMapTextureStage] & 16u) ?
                m_stageLodBias[normalMapTextureStage] : textureLodBias[normalMapTextureStage];
        }
        memcpy(textureMatrix, textureMatrices[0], sizeof(textureMatrix));

        const uint16_t* indexData = static_cast<const uint16_t*>(indices->m_VData) + firstIndex;
        auto& triangulatedQuads = scratchStorage.triangulatedQuads;
        triangulatedQuads.clear();
        uint32_t drawIndexCount = static_cast<uint32_t>(indexCount);
        const void* finalVertexData = flareDeformActive ?
            static_cast<const void*>(flareVertexData.data()) : drawVertexData;
        const uint32_t finalVertexCount = flareDeformActive ? 16u :
            static_cast<uint32_t>(vertices->m_NumVerts);
        uint32_t nativeTerrainMode = 0;
        float nativeTerrainParameters[4]{};
        uint32_t validatedMinimumVertex = UINT32_MAX;
        uint32_t validatedMaximumVertex = 0;
        // Material groups reference only a part of their shared leaf buffer.
        // Convert that range; unrelated vertices are never fetched by this draw.
        uint32_t conversionFirst = 0, conversionEnd = finalVertexCount;
        if (!flareDeformActive && indexCount > 0)
        {
            uint32_t minimum, maximum;
            ValidatedIndexRange(indices, indexData, uint32_t(indexCount), finalVertexCount,
                minimum, maximum);
            if (maximum >= finalVertexCount)
            {
                m_frameRenderer->RequirePanelFallback();
                return;
            }
            validatedMinimumVertex = minimum;
            validatedMaximumVertex = maximum;
            conversionFirst = minimum;
            conversionEnd = maximum + 1;
        }
        int finalVertexFormat = flareDeformActive ? VERTEX_FORMAT_P3F_COL4UB_TEX2F :
            vertices->m_vertexformat;
        auto& waterDeformedVertices = scratchStorage.waterDeformedVertices;
        if ((stockWaterMode == 7 || stockWaterMode == 9) && finalVertexData)
        {
            CryVR::VulkanVertexFormat format{};
            if (CryVR::GetVulkanVertexFormat(static_cast<uint32_t>(finalVertexFormat), format))
            {
                // PosWaterDeform_PlusWaterLevel uses GL's 32-entry Perlin
                // gradient/permutation table. Evaluate the identical script
                // on CPU so P3F_COL4UB ocean meshes retain their input layout.
                static Vec3d gradients[66];
                static int permutation[32];
                static bool initialized = false;
                if (!initialized)
                {
                    for (int i = 0; i < 32; ++i)
                    {
                        permutation[i] = i;
                        gradients[i].x = static_cast<float>(rand()) / RAND_MAX;
                        gradients[i].y = static_cast<float>(rand()) / RAND_MAX;
                        gradients[i].z = static_cast<float>(rand()) / RAND_MAX;
                        gradients[i].Normalize();
                    }
                    for (int i = 0; i < 32; ++i)
                    {
                        int j = (rand() >> 4) & 31;
                        std::swap(permutation[j], permutation[i]);
                        gradients[i + 32] = gradients[i];
                    }
                    gradients[64] = gradients[0]; gradients[65] = gradients[1];
                    initialized = true;
                }
                SParamComp_User amplitudeParameter;
                amplitudeParameter.m_Name = "WaveAmplitude";
                float amplitude = amplitudeParameter.mfGet();
                float waterLevel = iSystem->GetI3DEngine()->GetWaterLevel();
                uint32_t beachUvOffset = ~0u;
                float beachShift[4]{};
                if (stockWaterMode == 9)
                {
                    for (uint32_t attribute = 0; attribute < format.attributeCount; ++attribute)
                        if (format.attributes[attribute].location == 3)
                            beachUvOffset = format.attributes[attribute].offset;
                    SShaderPassHW* beachPass = const_cast<SShaderPassHW*>(stockHardwarePass);
                    TArray<SCGParam4f>* lists[] = { &beachPass->m_VPParamsNoObj, &beachPass->m_VPParamsObj };
                    for (auto* list : lists)
                        for (int parameter = 0; parameter < list->Num(); ++parameter)
                            if (!FastAsciiCaseCompare(list->Get(parameter).m_Name.c_str(), "TexShift"))
                                memcpy(beachShift, list->Get(parameter).mfGet(), sizeof(beachShift));
                }
                waterDeformedVertices.resize(static_cast<size_t>(finalVertexCount) * format.stride);
                // QueueStockClientIndexedDraw uploads this validated index
                // range only. Copy/deform no vertices outside that range.
                memcpy(waterDeformedVertices.data() + size_t(conversionFirst) * format.stride,
                    static_cast<const byte*>(finalVertexData) + size_t(conversionFirst) * format.stride,
                    size_t(conversionEnd - conversionFirst) * format.stride);
                for (uint32_t i = conversionFirst; i < conversionEnd; ++i)
                {
                    Vec3d position;
                    byte* destination = waterDeformedVertices.data() + i * format.stride;
                    memcpy(&position, destination, sizeof(position));
                    float vx = position.x + m_RP.m_RealTime * (stockWaterMode == 9 ? 0.6f : 0.8f);
                    float vy = position.y + m_RP.m_RealTime * 0.6f;
                    int ix = static_cast<int>(floorf(vx)) & 31;
                    int iy = static_cast<int>(floorf(vy)) & 31;
                    float fx = vx-floorf(vx), fy = vy-floorf(vy);
                    int p0 = permutation[ix] + iy;
                    int p1 = permutation[(ix+1)&31] + iy;
                    float r0 = gradients[p0].x*fx + gradients[p0].y*fy;
                    float r1 = gradients[p1].x*(fx-1.0f) + gradients[p1].y*fy;
                    float r2 = gradients[p0+1].x*fx + gradients[p0+1].y*(fy-1.0f);
                    float r3 = gradients[p1+1].x*(fx-1.0f) + gradients[p1+1].y*(fy-1.0f);
                    float sx = fx*fx*(3.0f-2.0f*fx), sy = fy*fy*(3.0f-2.0f*fy);
                    float a = r0 + (r2-r0)*sy, b = r1 + (r3-r1)*sy;
                    const float noise = a + (b - a) * sx;
                    position.z = (stockWaterMode == 9 ? position.z : waterLevel) + noise * amplitude;
                    if (stockWaterMode == 9 && beachUvOffset != ~0u && fabsf(beachShift[1]) > 1.e-6f)
                    {
                        float uv[2];
                        memcpy(uv, destination + beachUvOffset, sizeof(uv));
                        uv[1] += noise * beachShift[2] / beachShift[1];
                        memcpy(destination + beachUvOffset, uv, sizeof(uv));
                    }
                    memcpy(destination, &position, sizeof(position));
                }
                finalVertexData = waterDeformedVertices.data();
            }
        }
        auto& hardwareTexturedVertices = scratchStorage.hardwareTexturedVertices;
        auto& separateUvVertices = scratchStorage.separateUvVertices;
        separateUvVertices.clear();
        auto& terrainLowResTexturedVertices = scratchStorage.terrainLowResTexturedVertices;
        auto& terrainFogLayeredTexturedVertices = scratchStorage.terrainFogLayeredTexturedVertices;
        auto& terrainTexturedVertices = scratchStorage.terrainTexturedVertices;
        int hardwareTempTexCoordCount = -1;
        if (!flareDeformActive && textureIds[0] > 0 &&
            uv0Offset == 0xffffffffu && !linearTexgen[0].enabled &&
            m_RP.m_pRE && m_RP.m_pRE->mfGetType() == eDATA_OcLeaf)
        {
            CREOcLeaf* leafElement = static_cast<CREOcLeaf*>(m_RP.m_pRE);
            CLeafBuffer* leafBuffer = leafElement->m_pBuffer ?
                leafElement->m_pBuffer->GetVertexContainer() : nullptr;
            if (leafBuffer && leafBuffer->m_TempTexCoords &&
                finalVertexCount <= static_cast<uint32_t>(leafBuffer->m_SecVertCount))
            {
                hardwareTempTexCoordCount = leafBuffer->m_SecVertCount;
                // CREOcLeaf exposes this source independently of the packed
                // position/normal/color stream. Preserve all those attributes
                // while adding UVs, including skinned normal-bearing layouts.
                const int withUv = m_RP.m_VFormatsMerge[finalVertexFormat][VERTEX_FORMAT_P3F_TEX2F];
                CryVR::VulkanVertexFormat sourceFormat{}, destinationFormat{};
                if (CryVR::GetVulkanVertexFormat(finalVertexFormat, sourceFormat) &&
                    CryVR::GetVulkanVertexFormat(withUv, destinationFormat))
                {
                    separateUvVertices.resize(static_cast<size_t>(finalVertexCount) * destinationFormat.stride, 0);
                    for (uint32_t vertex = 0; vertex < finalVertexCount; ++vertex)
                    {
                        byte* output = separateUvVertices.data() + vertex * destinationFormat.stride;
                        const byte* input = static_cast<const byte*>(finalVertexData) + vertex * sourceFormat.stride;
                        for (uint32_t dst = 0; dst < destinationFormat.attributeCount; ++dst)
                        {
                            const auto& target = destinationFormat.attributes[dst];
                            if (target.location == 3)
                            {
                                memcpy(output + target.offset, leafBuffer->m_TempTexCoords[vertex].vert, sizeof(float) * 2);
                                continue;
                            }
                            for (uint32_t src = 0; src < sourceFormat.attributeCount; ++src)
                            {
                                const auto& original = sourceFormat.attributes[src];
                                if (original.location != target.location || original.format != target.format) continue;
                                const size_t size = original.format == VK_FORMAT_R32G32B32_SFLOAT ? 12 :
                                    (original.format == VK_FORMAT_R32G32_SFLOAT ? 8 : 4);
                                memcpy(output + target.offset, input + original.offset, size);
                                break;
                            }
                        }
                    }
                    finalVertexData = separateUvVertices.data();
                    finalVertexFormat = withUv;
                }
            }
        }
        if (stockHardwarePass &&
            programInfo.texgenOne &&
            m_RP.m_pShader && !FastAsciiCaseCompareN(m_RP.m_pShader->GetName(), "TerrainWaterBottom", 18) &&
            finalVertexFormat == VERTEX_FORMAT_P3F)
        {
            float planes[2][4]{};
            const char* names[] = { "BaseTexGen0", "BaseTexGen1" };
            SShaderPassHW* pass = const_cast<SShaderPassHW*>(stockHardwarePass);
            TArray<SCGParam4f>* lists[] = { &pass->m_VPParamsNoObj, &pass->m_VPParamsObj };
            for (auto* list : lists)
                for (int i = 0; i < list->Num(); ++i)
                    for (int plane = 0; plane < 2; ++plane)
                        if (!FastAsciiCaseCompare(list->Get(i).m_Name.c_str(), names[plane]))
                            memcpy(planes[plane], list->Get(i).mfGet(), sizeof(planes[plane]));
            const Vec3d* positions = static_cast<const Vec3d*>(finalVertexData);
            hardwareTexturedVertices.resize(finalVertexCount);
            for (uint32_t i = 0; i < finalVertexCount; ++i)
            {
                auto& output = hardwareTexturedVertices[i];
                output.xyz = positions[i];
                Vec3d world = positions[i];
                if (m_RP.m_pCurObject)
                {
                    Vec3d local = positions[i];
                    TransformPosition(world, local, m_RP.m_pCurObject->m_Matrix);
                }
                for (int plane = 0; plane < 2; ++plane)
                    output.st[plane] = world.x*planes[plane][0] + world.y*planes[plane][1] +
                        world.z*planes[plane][2] + planes[plane][3];
            }
            finalVertexData = hardwareTexturedVertices.data();
            finalVertexFormat = VERTEX_FORMAT_P3F_TEX2F;
        }
        // CGVProgMuzzleFlash drives the Training sunlight meshes as well as
        // muzzle flashes. Its RGB comes from the object-space view/normal angle.
        const bool muzzleFlashShader = m_RP.m_pShader &&
            (!FastAsciiCaseCompare(m_RP.m_pShader->GetName(), "templmuzzleflash_auto") ||
             !FastAsciiCaseCompare(m_RP.m_pShader->GetName(), "templmuzzleflash_auto_fp"));
        const bool muzzleFlashAngularFade = muzzleFlashShader &&
            !FastAsciiCaseCompare(m_RP.m_pShader->GetName(), "templmuzzleflash_auto");
        const bool stockPlantsProgram = stockHardwarePass &&
            programInfo.plants;
        const bool stockSimplePlantsProgram = stockPlantsProgram &&
            (programInfo.simplePlant ||
             programInfo.bendedPlant);
        if ((stockAmbientTemplate || stockLightTemplate) &&
            !stockHardwarePass->m_StockUsesVertexColors)
        {
            // CG templates sample albedo/lighting independently of input
            // mesh RGB unless the VERTCOLORS program feature is enabled.
            // The fixed-function vertex color is not an extra lighting term.
            primaryColor[0] = primaryColor[1] = primaryColor[2] = 1.0f;
            primaryColorMask[0] = primaryColorMask[1] = primaryColorMask[2] = 1.0f;
        }
        if (stockAmbientTemplate || (stockHardwarePass &&
            programInfo.light &&
            (stockHardwarePass->m_StockProgramMask & 0x20000000ull)))
        {
            // CGRCAmbientTempl/CGRCLightTempl output diffuse alpha only
            // for the corresponding expanded material variant. A gloss or
            // glow channel is not automatically the surface opacity.
            const uint64 mask = stockHardwarePass->m_StockProgramMask;
            const bool diffuseAlpha = !(mask & 0x2000ull) && (mask & 0x4000ull);
            const bool useTextureAlpha = stockAmbientTemplate ?
                (diffuseAlpha || ((mask & 0x2000ull) && (mask & 0x01000000ull))) :
                (diffuseAlpha && !(mask & 0x20ull));
            alphaOps[0] = useTextureAlpha ? eCO_MODULATE : eCO_REPLACE;
            alphaArgs[0] = useTextureAlpha ?
                eCA_Texture | (eCA_Diffuse << 3) : eCA_Diffuse;
            primaryColor[3] = 1.0f;
            primaryColorMask[3] = 1.0f;
        }
        const int stockTerrainLayers = programInfo.terrainLayers;
        const int stockTerrainOnlyLayers = programInfo.terrainOnly;
        const int stockTerrainAmbientMode = programInfo.terrainAmbient;
        const int stockTerrainFogLayers = programInfo.terrainFog;
        if (stockPlantsProgram || stockTerrainLayers >= 0)
        {
            // CGRCPlants/CGRCTerrain encode the albedo/color product by x2.
            colorOps[0] = eCO_MODULATE2X;
            colorArgs[0] = eCA_Texture | (eCA_Diffuse << 3);
            alphaOps[0] = eCO_REPLACE;
            alphaArgs[0] = eCA_Texture;
            primaryColorMask[0] = primaryColorMask[1] = primaryColorMask[2] = 0.0f;
            primaryColor[3] = 1.0f;
            primaryColorMask[3] = 1.0f;
        }
        auto& muzzleFlashVertices = scratchStorage.muzzleFlashVertices;
        muzzleFlashVertices.clear();
        if (muzzleFlashShader)
        {
            renderState = (renderState & ~(GS_BLEND_MASK | GS_DEPTHWRITE)) |
                GS_BLSRC_ONE | GS_BLDST_ONE;
            colorOps[0] = muzzleFlashAngularFade ? eCO_MODULATE : eCO_REPLACE;
            alphaOps[0] = eCO_MODULATE;
            colorArgs[0] = alphaArgs[0] = eCA_Texture | (eCA_Diffuse << 3);
            memset(primaryColorMask, 0, sizeof(primaryColorMask));
            CryVR::VulkanVertexFormat format{};
            uint32_t posOffset = ~0u, texOffset = ~0u, normOffset = ~0u;
            if (CryVR::GetVulkanVertexFormat(finalVertexFormat, format))
                for (uint32_t i = 0; i < format.attributeCount; ++i)
                {
                    const auto& attribute = format.attributes[i];
                    if (attribute.location == 0) posOffset = attribute.offset;
                    if (attribute.location == 1) normOffset = attribute.offset;
                    if (attribute.location == 3) texOffset = attribute.offset;
                }
            const byte* normals = nullptr;
            int normalStride = 0;
            if (normOffset != ~0u)
            {
                normals = static_cast<const byte*>(finalVertexData) + normOffset;
                normalStride = format.stride;
            }
            else if (m_RP.m_pRE && m_RP.m_pRE->mfGetType() == eDATA_OcLeaf)
            {
                auto* element = static_cast<CREOcLeaf*>(m_RP.m_pRE);
                CLeafBuffer* leaf = element->m_pBuffer ?
                    element->m_pBuffer->GetVertexContainer() : nullptr;
                if (leaf && leaf->m_pSecVertBuffer &&
                    leaf->m_SecVertCount >= static_cast<int>(finalVertexCount) &&
                    (gBufInfoTable[leaf->m_pSecVertBuffer->m_vertexformat].OffsNormal ||
                     leaf->m_TempNormals))
                    normals = leaf->GetNormalPtr(normalStride);
            }
            if (muzzleFlashAngularFade && posOffset != ~0u && texOffset != ~0u && normals)
            {
                Vec3d camera = GetCamera().GetPos();
                if (m_RP.m_pCurObject)
                {
                    Vec3d worldCamera = camera;
                    TransformPosition(camera, worldCamera,
                        m_RP.m_pCurObject->GetInvMatrix());
                }
                muzzleFlashVertices.resize(finalVertexCount);
                for (uint32_t i = 0; i < finalVertexCount; ++i)
                {
                    const byte* source = static_cast<const byte*>(finalVertexData) + i * format.stride;
                    auto& output = muzzleFlashVertices[i];
                    memcpy(&output.xyz, source + posOffset, sizeof(output.xyz));
                    memcpy(output.st, source + texOffset, sizeof(output.st));
                    Vec3d normal;
                    memcpy(&normal, normals + i * normalStride, sizeof(normal));
                    Vec3d view = camera - output.xyz;
                    view.Normalize();
                    float d = 2.0f * fabsf(view.Dot(normal));
                    d *= d;
                    d = crymin(1.0f, d * d);
                    const byte intensity = static_cast<byte>(d * 255.0f);
                    output.color.bcolor[0] = output.color.bcolor[1] =
                        output.color.bcolor[2] = intensity;
                    output.color.bcolor[3] = 255;
                }
                finalVertexData = muzzleFlashVertices.data();
                finalVertexFormat = VERTEX_FORMAT_P3F_COL4UB_TEX2F;
            }
        }
        // EF_CommitStreams overrides the normal pointer with current TNormal
        // for resources requiring normals, including animated tangent streams.
        auto& leafLitVertices = scratchStorage.litVertices;
        const void* immutableNormalGeometry = nullptr;
        // Keep the previous size as well as capacity; the referenced range is
        // overwritten below, so clearing would zero unrelated mesh vertices.
        const bool resourceNormalOverride = m_RP.m_pShaderResources &&
            m_RP.m_pShaderResources->m_bNeedNormals;
        if (!muzzleFlashShader && !stockSimplePlantsProgram && (normalPointer || resourceNormalOverride || hardwareLightPass || hardwareAmbientPass ||
            fixedFunctionMaterialLighting) &&
            (normalPointer || resourceNormalOverride || !HasStockVertexNormal(finalVertexFormat)) &&
            m_RP.m_pRE && m_RP.m_pRE->mfGetType() == eDATA_OcLeaf)
        {
            auto* element = static_cast<CREOcLeaf*>(m_RP.m_pRE);
            CLeafBuffer* leaf = element->m_pBuffer ? element->m_pBuffer->GetVertexContainer() : nullptr;
            CryVR::VulkanVertexFormat sourceFormat{}, targetFormat{};
            uint32_t pos = ~0u, uv = ~0u, color = ~0u, secondary = ~0u, normalOffset = ~0u;
            if (CryVR::GetVulkanVertexFormat(finalVertexFormat, sourceFormat))
                for (uint32_t i = 0; i < sourceFormat.attributeCount; ++i)
                {
                    const auto& a = sourceFormat.attributes[i];
                    if (a.location == 0) pos = a.offset;
                    if (a.location == 1 && a.format == VK_FORMAT_R32G32B32_SFLOAT)
                        normalOffset = a.offset;
                    if (a.location == 3) uv = a.offset;
                    if (a.location == 2) color = a.offset;
                    if (a.location == 4) secondary = a.offset;
                }
            const ESrcPointer committedNormalSource = resourceNormalOverride ? eSrcPointer_TNormal :
                (normalPointer ? normalPointer->ePT : eSrcPointer_Unknown);
            const bool floatNormalPointer = resourceNormalOverride || !normalPointer ||
                normalPointer->Type == GL_FLOAT;
            const bool tangentNormalPointer = floatNormalPointer &&
                (committedNormalSource == eSrcPointer_TNormal ||
                 committedNormalSource == eSrcPointer_Tangent || committedNormalSource == eSrcPointer_Binormal);
            const SPipTangents* currentTangents = tangentNormalPointer && leaf &&
                leaf->m_pVertexBuffer && leaf->m_pVertexBuffer->m_NumVerts >= static_cast<int>(finalVertexCount) ?
                static_cast<const SPipTangents*>(leaf->m_pVertexBuffer->m_VS[VSF_TANGENTS].m_VData) : nullptr;
            const byte* committedTangentNormal = currentTangents ? reinterpret_cast<const byte*>(
                committedNormalSource == eSrcPointer_Tangent ? &currentTangents[0].m_Tangent :
                committedNormalSource == eSrcPointer_Binormal ? &currentTangents[0].m_Binormal :
                &currentTangents[0].m_TNormal) : nullptr;
            const byte* committedNormal = committedTangentNormal;
            int committedNormalStride = sizeof(SPipTangents);
            if (floatNormalPointer && committedNormalSource == eSrcPointer_Normal && leaf &&
                leaf->m_pVertexBuffer && leaf->m_pVertexBuffer->m_NumVerts >= static_cast<int>(finalVertexCount) &&
                leaf->m_pVertexBuffer->m_VS[VSF_GENERAL].m_VData)
            {
                const int sourceNormalOffset = gBufInfoTable[leaf->m_pVertexBuffer->m_vertexformat].OffsNormal;
                if (sourceNormalOffset)
                {
                    committedNormal = static_cast<const byte*>(leaf->m_pVertexBuffer->m_VS[VSF_GENERAL].m_VData) +
                        sourceNormalOffset;
                    committedNormalStride = m_VertexSize[leaf->m_pVertexBuffer->m_vertexformat];
                }
            }
            const bool secondaryNormals = leaf && leaf->m_pSecVertBuffer &&
                leaf->m_SecVertCount >= static_cast<int>(finalVertexCount) &&
                (gBufInfoTable[leaf->m_pSecVertBuffer->m_vertexformat].OffsNormal || leaf->m_TempNormals) &&
                (leaf->m_TempNormals || leaf->m_pSecVertBuffer->m_VS[VSF_GENERAL].m_VData);
            if (committedNormal && normalOffset != ~0u &&
                normalOffset + sizeof(float) * 3 <= sourceFormat.stride)
            {
                if (committedNormalStride != static_cast<int>(sourceFormat.stride) ||
                    committedNormal != static_cast<const byte*>(finalVertexData) + normalOffset)
                {
                // Preserve secondary colors, extra UVs and every other attribute
                // when the layout already has room for the committed normal.
                const void* cachedNormals = CacheFrameNormalGeometry(vertices, finalVertexData,
                    finalVertexCount, finalVertexFormat, finalVertexFormat,
                    committedNormal, committedNormalStride);
                if (cachedNormals)
                {
                    finalVertexData = cachedNormals;
                    immutableNormalGeometry = cachedNormals;
                }
                else
                {
                const size_t bytes = static_cast<size_t>(finalVertexCount) * sourceFormat.stride;
                leafLitVertices.resize(bytes);
                const size_t start = static_cast<size_t>(conversionFirst) * sourceFormat.stride;
                memcpy(leafLitVertices.data() + start, static_cast<const byte*>(finalVertexData) + start,
                    static_cast<size_t>(conversionEnd - conversionFirst) * sourceFormat.stride);
                for (uint32_t i = conversionFirst; i < conversionEnd; ++i)
                    memcpy(leafLitVertices.data() + static_cast<size_t>(i) * sourceFormat.stride + normalOffset,
                        committedNormal + static_cast<size_t>(i) * committedNormalStride, sizeof(float) * 3);
                finalVertexData = leafLitVertices.data();
                }
                }
            }
            else if ((committedNormal || (!resourceNormalOverride && !normalPointer && secondaryNormals)) &&
                pos != ~0u && (finalVertexFormat == VERTEX_FORMAT_P3F ||
                    finalVertexFormat == VERTEX_FORMAT_P3F_COL4UB ||
                    finalVertexFormat == VERTEX_FORMAT_P3F_COL4UB_COL4UB ||
                    finalVertexFormat == VERTEX_FORMAT_P3F_COL4UB_COL4UB_TEX2F ||
                    finalVertexFormat == VERTEX_FORMAT_P3F_TEX2F ||
                    finalVertexFormat == VERTEX_FORMAT_P3F_COL4UB_TEX2F ||
                    finalVertexFormat == VERTEX_FORMAT_P3F_N_TEX2F ||
                    finalVertexFormat == VERTEX_FORMAT_P3F_N_COL4UB_TEX2F))
            {
                const int target = secondary != ~0u ?
                    (uv == ~0u ? VERTEX_FORMAT_P3F_N_COL4UB_COL4UB : VERTEX_FORMAT_P3F_N_COL4UB_COL4UB_TEX2F) :
                    uv == ~0u ?
                    (color == ~0u ? VERTEX_FORMAT_P3F_N : VERTEX_FORMAT_P3F_N_COL4UB) :
                    (color == ~0u ? VERTEX_FORMAT_P3F_N_TEX2F : VERTEX_FORMAT_P3F_N_COL4UB_TEX2F);
                if (CryVR::GetVulkanVertexFormat(target, targetFormat))
                {
                    int stride = 0;
                    const byte* normals = committedNormal ?
                        committedNormal : leaf->GetNormalPtr(stride);
                    if (committedNormal) stride = committedNormalStride;
                    const void* cachedNormals = CacheFrameNormalGeometry(vertices, finalVertexData,
                        finalVertexCount, finalVertexFormat, target, normals, stride);
                    if (cachedNormals)
                    {
                        finalVertexData = cachedNormals;
                        immutableNormalGeometry = cachedNormals;
                    }
                    else
                    {
                    leafLitVertices.resize(static_cast<size_t>(finalVertexCount) * targetFormat.stride);
                    for (uint32_t i = conversionFirst; i < conversionEnd; ++i)
                    {
                        const byte* source = static_cast<const byte*>(finalVertexData) + i * sourceFormat.stride;
                        byte* output = leafLitVertices.data() + i * targetFormat.stride;
                        memcpy(output, source + pos, 12);
                        memcpy(output + 12, normals + i * stride, 12);
                        if (color != ~0u) memcpy(output + 24, source + color, 4);
                        if (secondary != ~0u) memcpy(output + 28, source + secondary, 4);
                        if (uv != ~0u)
                            memcpy(output + (secondary != ~0u ? 32 : color == ~0u ? 24 : 28), source + uv, 8);
                    }
                    finalVertexData = leafLitVertices.data();
                    }
                    finalVertexFormat = target;
                }
            }
        }
        // Terrain surface, light and fog passes keep base-cover texgen on the
        // render element instead of storing UVs. The low-resolution terrain
        // uses P3F; sectors use P3F_N_COL4UB_COL4UB and carry detail masks.
        // Materialize OpenGL's (y * scale + offsetY, x * scale + offsetX)
        // mapping into a Vulkan texture-coordinate layout. Terrain shadow
        // receivers retain their explicit shadow-map texgen path.
        const bool terrainShaderSort = m_RP.m_pShader &&
            (m_RP.m_pShader->m_eSort == eS_Terrain ||
             m_RP.m_pShader->m_eSort == eS_TerrainDetailTextures ||
             m_RP.m_pShader->m_eSort == eS_TerrainLightPass ||
             m_RP.m_pShader->m_eSort == eS_TerrainFogPass ||
             (stockTerrainShadowProgram &&
              m_RP.m_pShader->m_eSort == eS_TerrainShadowPass));
        const bool terrainDetailSort = m_RP.m_pShader &&
            m_RP.m_pShader->m_eSort == eS_TerrainDetailTextures;
        const bool terrainSectorVertices =
            vertices->m_vertexformat == VERTEX_FORMAT_P3F_N_COL4UB_COL4UB &&
            (finalVertexFormat == VERTEX_FORMAT_P3F_N_COL4UB_COL4UB ||
             finalVertexFormat == VERTEX_FORMAT_P3F_N_COL4UB_COL4UB_TEX2F);
        const bool terrainShadowVertices = stockTerrainShadowProgram &&
            finalVertexFormat == VERTEX_FORMAT_P3F_N_COL4UB;
        const bool terrainLowResVertices =
            finalVertexFormat == VERTEX_FORMAT_P3F &&
            m_RP.m_pShader && m_RP.m_pShader->m_eSort == eS_Terrain;
        // TerrainWithFog uses its own compact vertex layouts: the base and
        // one-layer variants carry P3F+Color, while the two-layer variant
        // carries P3F+Color+Color1. Include these layouts in terrain texgen
        // setup or the fog/layer shaders receive no RE terrain projection.
        const bool terrainFogColorVertices =
            finalVertexFormat == VERTEX_FORMAT_P3F_COL4UB &&
            (stockTerrainAmbientMode == 4 || stockTerrainFogLayers == 1);
        const bool terrainFogLayeredVertices =
            finalVertexFormat == VERTEX_FORMAT_P3F_COL4UB_COL4UB &&
            stockTerrainFogLayers == 2;
        if (!flareDeformActive && (terrainSectorVertices || terrainLowResVertices ||
            terrainShadowVertices || terrainFogColorVertices || terrainFogLayeredVertices) &&
            m_RP.m_pRE && m_RP.m_pRE->m_CustomData && finalVertexData &&
            finalVertexCount > 0 && terrainShaderSort)
        {
            const float* terrainTexgen = static_cast<const float*>(m_RP.m_pRE->m_CustomData);
            // TerrainLowLOD's HWScripts bind BaseTexGen0/1 to FromRE[2],
            // FromRE[3], and FromRE[4]. The first two custom-data values
            // are flags (both 1), not texgen offsets; the low-LOD offsets
            // are the final two values and are zero.
            const float offsetY = terrainLowResVertices ? terrainTexgen[3] : terrainTexgen[0];
            const float offsetX = terrainLowResVertices ? terrainTexgen[4] : terrainTexgen[1];
            const float scale = terrainTexgen[2];
            if (!terrainDetailSort)
            {
                // TerrainLowLOD's BaseTexGen parameters map to custom data
                // [2], [3], [4]: positive full-map scale and zero offsets.
                // Sector terrain uses its separate [0]/[1] origins.
                terrainProjectionRows[0][0][1] = scale;
                terrainProjectionRows[0][0][3] = offsetY;
                terrainProjectionRows[1][0][0] = scale;
                terrainProjectionRows[1][0][3] = offsetX;
                for (uint32_t stage = 1; stage < 8 && terrainSectorVertices; ++stage)
                {
                    const float* projection = terrainTexgen + 4 + (stage - 1) * 8;
                    for (int axis = 0; axis < 4; ++axis)
                    {
                        terrainProjectionRows[0][stage][axis] = projection[axis];
                        terrainProjectionRows[1][stage][axis] = projection[4 + axis];
                    }
                }
            }
            else if (terrainSectorVertices)
            {
                // TerrainDetailLayers binds $FromRE4..7 as shader stages 0..3.
                // Its Cg parameters address FromRE[28..58]; slots 0..3 are
                // occupied by the base terrain textures and are not these layers.
                for (uint32_t stage = 0; stage < 4; ++stage)
                {
                    const float* projection = terrainTexgen + 28 + stage * 8;
                    for (int axis = 0; axis < 4; ++axis)
                    {
                        terrainProjectionRows[0][stage][axis] = projection[axis];
                        terrainProjectionRows[1][stage][axis] = projection[4 + axis];
                    }
                }
            }
            hasTerrainProjection = true;
            if (stockTerrainLayers >= 0)
                terrainProjectionRows[1][7][3] = 100.0f + stockTerrainLayers;
            else if (terrainDetailSort && stockTerrainOnlyLayers > 0)
                terrainProjectionRows[1][7][3] = 200.0f + stockTerrainOnlyLayers;
            else if (stockTerrainAmbientMode > 0)
                terrainProjectionRows[1][7][3] = 300.0f + stockTerrainAmbientMode;
            else if (stockTerrainFogLayers > 0)
                terrainProjectionRows[1][7][3] = 410.0f + stockTerrainFogLayers;
            else if (stockTerrainShadowProgram)
                terrainProjectionRows[1][7][3] = 400.0f;
            if (stockTerrainAmbientMode == 4 || stockTerrainFogLayers > 0)
            {
                terrainProjectionRows[0][7][0] = hardwareTerrainFogColor[0];
                terrainProjectionRows[0][7][1] = hardwareTerrainFogColor[1];
                terrainProjectionRows[0][7][2] = hardwareTerrainFogColor[2];
                terrainProjectionRows[0][7][3] = hardwareAmbientParameter[3];
                CryVR::VulkanStockLinearTexgen fogEnterTexgen{};
                CryVR::VulkanStockLinearTexgen fogTexgen{};
                if (BuildStockFogTexgen(fogEnterTexgen, true) &&
                    BuildStockFogTexgen(fogTexgen, false))
                {
                    // FogEnter occupies projected stage 4; Fog occupies
                    // stage 5. Keep the independent maps/planes for the
                    // TerrainWithFog, _1Layers_VF and _2Layers_VF programs.
                    memcpy(terrainProjectionRows[0][4], fogEnterTexgen.planes[0],
                           sizeof(float) * 4);
                    memcpy(terrainProjectionRows[1][4], fogEnterTexgen.planes[1],
                           sizeof(float) * 4);
                    memcpy(terrainProjectionRows[0][5], fogTexgen.planes[0],
                           sizeof(float) * 4);
                    memcpy(terrainProjectionRows[1][5], fogTexgen.planes[1],
                           sizeof(float) * 4);
                }
            }
            if (terrainSectorVertices && finalVertexFormat == VERTEX_FORMAT_P3F_N_COL4UB_COL4UB &&
                !fixedFunctionMaterialLighting && !stockTerrainShadowProgram &&
                stockTerrainAmbientMode != 4 && !stockTerrainFogLayers)
            {
                // Keep the original terrain stream on GPU. Camera-dependent
                // detail weights and projected UVs are vertex shader work.
                Vec3d objectCamera = GetCamera().GetPos();
                if (m_RP.m_pCurObject && (m_RP.m_pCurObject->m_ObjFlags & FOB_TRANS_MASK))
                {
                    Vec3d worldCamera = objectCamera;
                    TransformPosition(objectCamera, worldCamera, m_RP.m_pCurObject->GetInvMatrix());
                }
                nativeTerrainMode = terrainDetailSort ? 4u : stockTerrainLayers > 0 ? 3u :
                    stockTerrainLayers == 0 ? 2u : 1u;
                nativeTerrainParameters[0] = objectCamera.x;
                nativeTerrainParameters[1] = objectCamera.y;
                nativeTerrainParameters[2] = objectCamera.z;
                nativeTerrainParameters[3] = crymax(0.001f, terrainTexgen[3]);
            }
            else if (terrainSectorVertices)
            {
                const auto* source = static_cast<const struct_VERTEX_FORMAT_P3F_N_COL4UB_COL4UB*>(
                    vertices->m_VS[VSF_GENERAL].m_VData);
                Vec3d objectCameraPosition(0.0f, 0.0f, 0.0f);
                const float terrainDetailFadeDistance = terrainDetailSort ?
                    crymax(0.001f, terrainTexgen[3]) : 1.0f;
                if ((terrainDetailSort || stockTerrainLayers > 0) && m_RP.m_pCurObject)
                {
                    Vec3d cameraPosition = GetCamera().GetPos();
                    objectCameraPosition = cameraPosition;
                    if (m_RP.m_pCurObject->m_ObjFlags & FOB_TRANS_MASK)
                        TransformPosition(objectCameraPosition, cameraPosition,
                                          m_RP.m_pCurObject->GetInvMatrix());
                }
                terrainTexturedVertices.resize(finalVertexCount);
                for (uint32_t vertex = conversionFirst; vertex < conversionEnd; ++vertex)
                {
                    struct_VERTEX_FORMAT_P3F_N_COL4UB_COL4UB_TEX2F& destination =
                        terrainTexturedVertices[vertex];
                    destination.xyz = source[vertex].xyz;
                    destination.normal = source[vertex].normal;
                    destination.color = source[vertex].color;
                    if (stockTerrainLayers >= 0 && !terrainDetailSort)
                    {
                        // Shaders/HWScripts/Techniques/terrain.csl routes the
                        // base terrain pass through CGVProgTerrain, whose
                        // output is IN.Color.www. Preserve the terrain
                        // lighting weight carried by the vertex alpha instead
                        // of forwarding the unrelated RGB channels.
                        destination.color.bcolor[0] = source[vertex].color.bcolor[3];
                        destination.color.bcolor[1] = source[vertex].color.bcolor[3];
                        destination.color.bcolor[2] = source[vertex].color.bcolor[3];
                    }
                    destination.seccolor = source[vertex].seccolor;
                    if (stockTerrainLayers > 0 && !terrainDetailSort)
                    {
                        const Vec3d position(source[vertex].xyz.x, source[vertex].xyz.y,
                                             source[vertex].xyz.z);
                        float fade = crymin((objectCameraPosition - position).GetLength() /
                            crymax(0.001f, terrainTexgen[3]), 1.0f);
                        fade *= fade;
                        fade *= fade;
                        for (int channel = 0; channel < 3; ++channel)
                            destination.seccolor.bcolor[channel] = static_cast<byte>(
                                source[vertex].color.bcolor[channel] * fade);
                    }
                    if (terrainDetailSort)
                    {
                        const Vec3d vertexPosition(source[vertex].xyz.x,
                            source[vertex].xyz.y, source[vertex].xyz.z);
                        float fade = crymin((objectCameraPosition - vertexPosition).GetLength() /
                                            terrainDetailFadeDistance, 1.0f);
                        fade *= fade;
                        fade *= fade;
                        // CGVProgTerrain_4Layers_Only writes detail weights to
                        // Color.b, Color.a, Color1.b and Color1.g respectively.
                        destination.color.bcolor[2] = static_cast<uint8_t>(
                            clamp_tpl(static_cast<float>(source[vertex].seccolor.bcolor[0]) * fade, 0.0f, 255.0f));
                        destination.color.bcolor[3] = static_cast<uint8_t>(
                            clamp_tpl(static_cast<float>(source[vertex].seccolor.bcolor[3]) * fade, 0.0f, 255.0f));
                        destination.seccolor.bcolor[2] = static_cast<uint8_t>(
                            clamp_tpl(static_cast<float>(source[vertex].seccolor.bcolor[1]) * fade, 0.0f, 255.0f));
                        destination.seccolor.bcolor[1] = static_cast<uint8_t>(
                            clamp_tpl(static_cast<float>(source[vertex].seccolor.bcolor[2]) * fade, 0.0f, 255.0f));
                    }
                    destination.st[0] = source[vertex].xyz.y * scale + offsetY;
                    destination.st[1] = source[vertex].xyz.x * scale + offsetX;
                }
                finalVertexData = terrainTexturedVertices.data();
                finalVertexFormat = VERTEX_FORMAT_P3F_N_COL4UB_COL4UB_TEX2F;
            }
            else if (terrainLowResVertices)
            {
                const auto* source = static_cast<const struct_VERTEX_FORMAT_P3F*>(finalVertexData);
                terrainLowResTexturedVertices.resize(finalVertexCount);
                for (uint32_t vertex = 0; vertex < finalVertexCount; ++vertex)
                {
                    struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F& destination =
                        terrainLowResTexturedVertices[vertex];
                    destination.xyz = source[vertex].xyz;
                    destination.color.bcolor[0] = static_cast<uint8_t>(clamp_tpl(
                        m_WorldColor.r * 255.0f, 0.0f, 255.0f));
                    destination.color.bcolor[1] = static_cast<uint8_t>(clamp_tpl(
                        m_WorldColor.g * 255.0f, 0.0f, 255.0f));
                    destination.color.bcolor[2] = static_cast<uint8_t>(clamp_tpl(
                        m_WorldColor.b * 255.0f, 0.0f, 255.0f));
                    destination.color.bcolor[3] = 255;
                    destination.st[0] = source[vertex].xyz.y * scale + offsetY;
                    destination.st[1] = source[vertex].xyz.x * scale + offsetX;
                }
                finalVertexData = terrainLowResTexturedVertices.data();
                finalVertexFormat = VERTEX_FORMAT_P3F_COL4UB_TEX2F;
            }
            else if (terrainFogColorVertices)
            {
                // The color channels are semantic inputs to TerrainWithFog
                // (lighting in RGB/alpha and detail weight in R). Preserve
                // them byte-for-byte while adding the base terrain UV.
                const auto* source = static_cast<const struct_VERTEX_FORMAT_P3F_COL4UB*>(
                    finalVertexData);
                terrainLowResTexturedVertices.resize(finalVertexCount);
                for (uint32_t vertex = 0; vertex < finalVertexCount; ++vertex)
                {
                    struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F& destination =
                        terrainLowResTexturedVertices[vertex];
                    destination.xyz = source[vertex].xyz;
                    destination.color = source[vertex].color;
                    destination.st[0] = source[vertex].xyz.y * scale + offsetY;
                    destination.st[1] = source[vertex].xyz.x * scale + offsetX;
                }
                finalVertexData = terrainLowResTexturedVertices.data();
                finalVertexFormat = VERTEX_FORMAT_P3F_COL4UB_TEX2F;
            }
            else if (terrainFogLayeredVertices)
            {
                // The second vertex color holds the second detail mask used
                // by CGRCTerrain_2Layers_VF; keep both color sets intact.
                const auto* source = static_cast<const struct_VERTEX_FORMAT_P3F_COL4UB_COL4UB*>(
                    finalVertexData);
                terrainFogLayeredTexturedVertices.resize(finalVertexCount);
                for (uint32_t vertex = 0; vertex < finalVertexCount; ++vertex)
                {
                    struct_VERTEX_FORMAT_P3F_COL4UB_COL4UB_TEX2F& destination =
                        terrainFogLayeredTexturedVertices[vertex];
                    destination.xyz = source[vertex].xyz;
                    destination.color = source[vertex].color;
                    destination.seccolor = source[vertex].seccolor;
                    destination.st[0] = source[vertex].xyz.y * scale + offsetY;
                    destination.st[1] = source[vertex].xyz.x * scale + offsetX;
                }
                finalVertexData = terrainFogLayeredTexturedVertices.data();
                finalVertexFormat = VERTEX_FORMAT_P3F_COL4UB_COL4UB_TEX2F;
            }
            if (stockTerrainShadowProgram)
            {
                // CGVProgTerrainShadow's base map and shadow map use distinct
                // generators. The base planes are already reconstructed from
                // this RE's terrain texgen above; retain ObjPos/ObjColor for
                // the original distance fade and ambient for the fragment.
                SShaderPassHW* const pass = const_cast<SShaderPassHW*>(stockHardwarePass);
                TArray<SCGParam4f>* lists[] = {
                    &pass->m_VPParamsNoObj, &pass->m_VPParamsObj };
                const char* names[] = { "ObjPos", "ObjColor" };
                float* destinations[] = {
                    terrainProjectionRows[0][2], terrainProjectionRows[0][3] };
                for (auto* list : lists)
                    for (int i = 0; i < list->Num(); ++i)
                        for (int parameter = 0; parameter < 2; ++parameter)
                            if (!FastAsciiCaseCompare(list->Get(i).m_Name.c_str(), names[parameter]))
                                memcpy(destinations[parameter], list->Get(i).mfGet(),
                                       sizeof(float) * 4);
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
            // BeamDeform substitutes a different index stream after the
            // initial bounds pass; let the queue validate this stream.
            validatedMinimumVertex = UINT32_MAX;
            validatedMaximumVertex = 0;
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
            if (object && (m_RP.m_ObjFlags & FOB_TRANS_MASK))
            {
                // Legacy Matrix44 uses row vectors (TransformPointOLD),
                // including translation in row 3. Substitute that world
                // position into dot(worldPlane, worldPosition): each object
                // coefficient is one legacy row dotted with the plane.
                for (int row = 0; row < 4; ++row)
                    objectClipPlane[row] = worldPlane[0] * object->m_Matrix(row, 0) +
                        worldPlane[1] * object->m_Matrix(row, 1) +
                        worldPlane[2] * object->m_Matrix(row, 2) +
                        worldPlane[3] * object->m_Matrix(row, 3);
            }
            else
                memcpy(objectClipPlane, worldPlane, sizeof(objectClipPlane));
            clipPlane = objectClipPlane;
        }
        // Ambient/bump templates can resolve additional textures after the
        // ordinary unit list, so commit final bias only after those bindings.
        commitStockLodBias(0, textureLodBias[0]);
        if (!directionalLightmapPass && normalMapTextureId)
            commitStockLodBias(1, normalMapLodBias);
        else
            commitStockLodBias(1, textureLodBias[1]);
        if (textureStage2Ptr) commitStockLodBias(2, textureStage2.lodBias);
        if (textureStage3Ptr) commitStockLodBias(3, textureStage3.lodBias);
        if (textureStages4To7Ptr)
            for (int stage = 4; stage < 8; ++stage)
                if (textureStages4To7[stage - 4].textureId > 0)
                    commitStockLodBias(stage, textureStages4To7[stage - 4].lodBias);
        const bool textureStage0UsesTexCoord1 = textureCoordinatePointers[0] ?
            textureCoordinatePointers[0]->ePT == eSrcPointer_TexLM : lightmapStage == 0;
        std::array<std::array<float, 16>, 8> fixedLights{};
        uint32_t fixedLightCount = 0;
        if (fixedFunctionMaterialLighting && hasTranslatedDynamicLightPass)
        {
            for (const auto& parameters : lightPasses)
            {
                if (fixedLightCount == fixedLights.size()) break;
                auto& light = fixedLights[fixedLightCount++];
                for (int channel = 0; channel < 4; ++channel) light[channel] = parameters[channel];
                for (int channel = 0; channel < 3; ++channel)
                {
                    light[4 + channel] = parameters[4 + channel];
                    light[8 + channel] = parameters[13 + channel];
                }
                light[11] = materialLighting[7];
                light[12] = parameters[11]; light[13] = parameters[12];
            }
            // GL enables all accepted fixed lights for one material draw.
            lightPasses.resize(1);
            lightPasses[0][7] = materialLighting[7];
        }
        if (textureCoordinatePointers[1])
            textureStage1UsesTexCoord1 =
                textureCoordinatePointers[1]->ePT == eSrcPointer_TexLM;
        for (size_t lightPass = 0; lightPass < lightPasses.size(); ++lightPass)
        {
            // Specular draws use negative radius, including combined Light passes.
            if (CryVR::kVulkanBasicRenderTest && lightPasses[lightPass][3] < 0.0f)
                continue;
            // The engine queues fog objects separately from their lit base
            // objects, without lightmap IDs. Redrawing that base here replaces
            // the baked lighting. Keep the geometry for the volume overlay
            // after this loop, matching GL's bFog general-pass dispatch.
            if (m_RP.m_ObjFlags & FOB_FOGPASS)
                continue;
            m_frameRenderer->ResetStockLinearTexgen();
            for (int stage = 0; stage < 8; ++stage)
            {
                if (!linearTexgen[stage].enabled) continue;
                if (stage < 2)
                    memcpy(linearTexgen[stage].textureMatrix, textureMatrices[stage],
                           sizeof(linearTexgen[stage].textureMatrix));
                linearTexgen[stage].useTexCoord1 = stage == 0 ?
                    textureStage0UsesTexCoord1 : (stage == 1 ? textureStage1UsesTexCoord1 :
                    (stage == 2 ? textureStage2.useTexCoord1 : (stage == 3 ?
                     textureStage3.useTexCoord1 : textureStages4To7[stage - 4].useTexCoord1)));
                m_frameRenderer->SetStockLinearTexgen(stage, linearTexgen[stage]);
            }
            const float* drawTextureMatrix0 = textureMatrix;
            const float* drawTextureMatrix1 = textureMatrices[1];
            // Texture matrices belong to destination stages, independently
            // of which coordinate array each stage selects in the fragment.
            uint32_t passState = renderState;
            // Each hardware Light/DiffuseLight/SpecularLight pass is submitted
            // by its own DrawRenderItem call, so its translated direct-light
            // contribution is lightPass 0 of a one-draw vector. OpenGL applies
            // these passes over the already-rendered General/lightmap pass;
            // preserve that result with additive RGB blending and do not
            // replace its depth. The per-vector lightPass > 0 case still
            // handles additional packed lights from MultiLights/fixed paths.
            if ((hardwareMaterialLightPass && hasTranslatedDynamicLightPass &&
                 (m_RP.m_RendPass > 1 || (m_RP.m_ObjFlags & FOB_LIGHTPASS))) || lightPass > 0)
            {
                passState &= ~(GS_BLEND_MASK | GS_DEPTHWRITE);
                passState |= GS_BLSRC_ONE | GS_BLDST_ONE | GS_COLMASKONLYRGB;
            }
            int projectorCookieTextureId = 0;
            int secondaryTextureId = textureIds[1];
            if (hardwareMultiLightsAmbient && !directionalLightmapPass &&
                lightmapStage == 1 && lightPass > 0)
                secondaryTextureId = 0;
            if ((stockTerrainAmbientMode == 4 || stockTerrainFogLayers > 0) && m_TexMan)
            {
                InitializeStockFogTextures();
                const int fogEnterTexture = m_TexMan->m_Text_Fog_Enter ?
                    m_TexMan->m_Text_Fog_Enter->GetTextureID() : 0;
                const int fogTexture = m_TexMan->m_Text_Fog ?
                    m_TexMan->m_Text_Fog->GetTextureID() : 0;
                const auto configureFogStage = [](CryVR::VulkanStockTextureStage& stage,
                                                  int textureId)
                {
                    stage = {};
                    stage.textureId = textureId;
                    stage.colorOp = stage.alphaOp = eCO_REPLACE;
                    stage.useTexCoord1 = false;
                };
                if (stockTerrainAmbientMode == 4)
                {
                    secondaryTextureId = textureIds[1] = fogEnterTexture;
                    configureFogStage(textureStage2, fogTexture);
                    textureStage2Ptr = fogTexture ? &textureStage2 : nullptr;
                }
                else if (stockTerrainFogLayers == 1)
                {
                    configureFogStage(textureStage2, fogEnterTexture);
                    configureFogStage(textureStage3, fogTexture);
                    textureStage2Ptr = fogEnterTexture ? &textureStage2 : nullptr;
                    textureStage3Ptr = fogTexture ? &textureStage3 : nullptr;
                }
                else
                {
                    configureFogStage(textureStage3, fogTexture);
                    textureStage3Ptr = fogTexture ? &textureStage3 : nullptr;
                }
            }
            float projectorBasis[9]{};
            float projectorFrustumScale = 1.0f;
            // MultiLights expands to separate draws here. Use the source of
            // this contribution, never the light left by a preceding item.
            CDLight* const currentLight = translatedLights[lightPass];
            if (!ambientOnlyLightPasses[lightPass] && m_activeHardwarePassType != eSHP_MAX && currentLight &&
                (currentLight->m_Flags & DLF_PROJECT) &&
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
                if (!lightmapTexCoords.empty())
                {
                    float minUv[2] = { lightmapTexCoords[0], lightmapTexCoords[1] };
                    float maxUv[2] = { minUv[0], minUv[1] };
                    uint32_t invalidUvCount = 0;
                    for (size_t coordinate = 0; coordinate < lightmapTexCoords.size(); ++coordinate)
                    {
                        const float value = lightmapTexCoords[coordinate];
                        if (!std::isfinite(value)) { ++invalidUvCount; continue; }
                        const size_t axis = coordinate % 2;
                        minUv[axis] = std::min(minUv[axis], value);
                        maxUv[axis] = std::max(maxUv[axis], value);
                    }
                    CryLogAlways("Vulkan audit LM_UV frame=%d list=%d item=%d count=%u first=(%.6f,%.6f) min=(%.6f,%.6f) max=(%.6f,%.6f) invalid=%u stage2=%d stage3=%d shadowMask=0x%x",
                        GetFrameID(), m_auditCurrentList, m_auditCurrentItem,
                        static_cast<unsigned int>(lightmapTexCoords.size() / 2),
                        lightmapTexCoords[0], lightmapTexCoords[1], minUv[0], minUv[1],
                        maxUv[0], maxUv[1], invalidUvCount,
                        textureStage2Ptr ? textureStage2Ptr->textureId : 0,
                        textureStage3Ptr ? textureStage3Ptr->textureId : 0, shadowMapStageMask);
                }
                CryLogAlways("Vulkan audit SUBMIT frame=%d list=%d item=%d lightPass=%d/%d indices=%u topology=%d vertexFormat=%d vertices=%u state=0x%x cull=%d textures=(%d,%d) normal=%d opacity=%.3f alphaRef=%.3f fogScale=%.3f lightmapStage=%d lmuv=%d dot3lm=%d bump=%d lmDir=%d generatedVertices=%d",
                    GetFrameID(), m_auditCurrentList, m_auditCurrentItem,
                    static_cast<int>(lightPass + 1), static_cast<int>(lightPasses.size()),
                    drawIndexCount, topology, finalVertexFormat, finalVertexCount, passState,
                    ResolveStockCullMode(),
                textureIds[0], secondaryTextureId, normalMapTextureId, globalOpacity,
                    alphaTestRef, fogRangeScale, lightmapStage,
                    lightmapTexCoords.empty() ? 0 : 1,
                    directionalLightmapPass ? 1 : 0,
                    directionalLightmapPass && textureIds[1] > 0 ? textureIds[1] : 0,
                    directionalLightmapPass ? m_RP.m_pCurObject->m_nLMDirId : 0,
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
            if (stockReceiverShadow)
            {
                auto* casters = static_cast<list2<ShadowMapLightSourceInstance>*>(m_RP.m_pCurObject->m_pShadowCasters);
                const int samples = (activeHardwareLightFlags & LMF_3SAMPLES) ? 3 :
                    ((activeHardwareLightFlags & LMF_2SAMPLES) ? 2 : 1);
                if (!casters || m_RP.m_nCurStartCaster + samples > casters->Count()) continue;
                hasTerrainProjection = true;
                terrainProjectionRows[1][7][3] = -15.0f;
                for (int channel = 0; channel < 3; ++channel)
                    terrainProjectionRows[0][6][channel] = hardwareAmbientParameter[channel];
                terrainProjectionRows[0][7][0] = float(samples);
                shadowMapStageMask = 0;
                for (int sample = 0; sample < samples; ++sample)
                {
                    auto& caster = casters->GetAt(m_RP.m_nCurStartCaster + sample);
                    ShadowMapFrustum* frustum = caster.m_pLS ? caster.m_pLS->GetShadowMapFrustum() : nullptr;
                    if (!frustum || !frustum->depth_tex_id) continue;
                    const int stage = sample + 1;
                    Matrix44* receiver = (m_RP.m_ObjFlags & FOB_TRANS_MASK) ? &m_RP.m_pCurObject->m_Matrix : nullptr;
                    SetupShadowOnlyPass(stage, frustum, &caster.m_vProjTranslation, caster.m_fProjScale,
                        m_RP.m_pCurObject->GetTranslation(), 1.0f, Vec3d(0,0,0), receiver);
                    memcpy(shadowMapTransforms[stage], m_cEF.m_TempMatrices[stage][0].GetData(), sizeof(shadowMapTransforms[stage]));
                    shadowMapStageMask |= 1u << stage;
                    terrainProjectionRows[1][6][sample] = frustum->fAlpha;
                    if (!sample) { secondaryTextureId = frustum->depth_tex_id; textureStage1UsesTexCoord1 = false; }
                    else
                    {
                        auto& unit = sample == 1 ? textureStage2 : textureStage3;
                        unit = {};
                        unit.textureId = frustum->depth_tex_id;
                        unit.colorOp = unit.alphaOp = eCO_MODULATE;
                        if (sample == 1) textureStage2Ptr = &textureStage2;
                        else textureStage3Ptr = &textureStage3;
                    }
                }
                normalMapTextureId = 0;
                textureStages4To7Ptr = nullptr;
                passState &= ~(GS_BLEND_MASK | GS_DEPTHWRITE);
                passState |= GS_BLSRC_SRCALPHA | GS_BLDST_ONEMINUSSRCALPHA | GS_DEPTHFUNC_EQUAL;
            }
            m_frameRenderer->SetStockShadowTransforms(
                shadowMapTransforms, shadowMapStageMask);
            // The original water programs generate UVs from object positions;
            // they do not require a mesh UV stream and do not use generic lighting.
            const bool translatedWaterEffect = stockWaterMode != 0;
            // CGVProgSimple_Plant computes WorldObjColor/Opacity per vertex;
            // CGRCPlants only samples albedo, encodes RGB and applies fog.
            // Keep bump, sprite, texgen and additional-light variants on their
            // own material paths rather than treating every leaf as this program.
            const bool simplePlantsVertex = stockSimplePlantsProgram;
            CryVR::VulkanVertexFormat plantsFormat{};
            bool plantsHasColor = false, plantsHasUv = false;
            if (simplePlantsVertex && CryVR::GetVulkanVertexFormat(finalVertexFormat, plantsFormat))
                for (uint32_t attribute = 0; attribute < plantsFormat.attributeCount; ++attribute)
                {
                    plantsHasColor |= plantsFormat.attributes[attribute].location == 2;
                    plantsHasUv |= plantsFormat.attributes[attribute].location == 3;
                }
            if (simplePlantsVertex && plantsHasColor && plantsHasUv &&
                lightPass == 0)
            {
                // Cg declarations supply default parameters even when the
                // technique does not repeat them as CGVPParam overrides.
                // Use the original component evaluators for those defaults.
                for (int channel = 0; channel < 3; ++channel)
                {
                    SParamComp_WorldColor world;
                    world.m_Offs = channel | 0x80000000u;
                    terrainProjectionRows[0][6][channel] = world.mfGet();
                }
                SParamComp_Opacity opacity;
                terrainProjectionRows[0][6][3] = opacity.mfGet();
                const bool bendedPlants = programInfo.bendedPlant;
                if (bendedPlants && m_RP.m_pCurObject)
                {
                    SParamComp_ObjWave wave;
                    wave.mfGet4f(terrainProjectionRows[1][6]);
                }
                else
                {
                    memset(terrainProjectionRows[1][6], 0, sizeof(float) * 4);
                    terrainProjectionRows[1][6][3] = 1.0f;
                }
                const TArray<SCGParam4f>* lists[] = {
                    &stockHardwarePass->m_VPParamsNoObj, &stockHardwarePass->m_VPParamsObj };
                for (const auto* list : lists)
                    if (list)
                        for (int parameter = 0; parameter < list->Num(); ++parameter)
                        {
                            SCGParam4f& value = const_cast<SCGParam4f&>(list->Get(parameter));
                            if (!FastAsciiCaseCompare(value.m_Name.c_str(), "Ambient"))
                            {
                                memcpy(terrainProjectionRows[0][6], value.mfGet(), sizeof(float) * 4);
                            }
                            else if (!FastAsciiCaseCompare(value.m_Name.c_str(), "Bend"))
                                memcpy(terrainProjectionRows[1][6], value.mfGet(), sizeof(float) * 4);
                        }
                {
                    hasTerrainProjection = true;
                    terrainProjectionRows[0][7][3] = -13.0f;
                    terrainProjectionRows[1][7][3] =
                        programInfo.bendedPlant ? 1.0f : 0.0f;
                    // Ambient.w is the original evaluated Opacity, including
                    // object fade. Do not apply the generic opacity again.
                    globalOpacity = 1.0f;
                    // CGRCPlants declares only texunit0. Other bound TMUs and
                    // fixed-function texgen do not participate in this Cg draw.
                    // They must neither block the program nor select a generic
                    // multitexture/bump pipeline for it.
                    secondaryTextureId = 0;
                    normalMapTextureId = 0;
                    textureStage2Ptr = textureStage3Ptr = nullptr;
                    textureStages4To7Ptr = nullptr;
                    m_frameRenderer->ResetStockLinearTexgen();
                }
            }
            if (programInfo.plantsBump)
            {
                // The Cg program declares exactly baseMap at texunit0 and
                // bumpMap at texunit1. Inherited fixed Layer bindings/extra
                // stages must not select a multitexture shader for this pass.
                const auto bindPlantResource = [&](int slot, int stage) -> int {
                    SEfResTexture* resource = m_activeResources ? m_activeResources->m_Textures[slot] : nullptr;
                    if (!resource) return 0;
                    if (!resource->m_TU.m_ITexPic && !resource->m_Name.empty())
                        resource->m_TU.m_ITexPic = m_cEF.LoadVulkanResourceTexture(resource->m_Name.c_str(),
                            m_activeResources->m_TexturePath.c_str(), resource->m_TU.GetTexFlags() | FT_NOSTREAM,
                            resource->m_TU.GetTexFlags2(), eTT_Base, m_RP.m_pShader, resource, resource->m_Amount);
                    resource->m_TU.mfUpdate();
                    if (!resource->m_TU.m_ITexPic) return 0;
                    textureLodBias[stage] = resource->m_TU.m_fTexFilterLodBias;
                    return resource->m_TU.m_ITexPic->GetTextureID();
                };
                int base = bindPlantResource(EFTT_DIFFUSE, 0);
                if (!base) base = bindPlantResource(EFTT_BUMP_DIFFUSE, 0);
                if (base) textureIds[0] = base;
                const int bump = bindPlantResource(EFTT_BUMP, 1);
                if (bump) normalMapTextureId = bump;
                normalMapLodBias = textureLodBias[1];
                commitStockLodBias(0, textureLodBias[0]);
                commitStockLodBias(1, normalMapLodBias);
                secondaryTextureId = 0;
                textureStage2Ptr = textureStage3Ptr = nullptr;
                textureStages4To7Ptr = nullptr;
                m_frameRenderer->ResetStockLinearTexgen();
                m_frameRenderer->SetStockProjector(0, nullptr, 1.0f);
                hasTerrainProjection = true;
                terrainProjectionRows[1][7][3] = -14.0f;
                for (int channel = 0; channel < 3; ++channel)
                {
                    terrainProjectionRows[0][6][channel] = hardwareAmbientParameter[channel];
                    SParamComp_SunColor sun;
                    sun.m_Offs = channel; sun.m_Mult = 1.0f;
                    terrainProjectionRows[1][6][channel] = sun.mfGet();
                }
                terrainProjectionRows[0][6][3] = hardwareAmbientParameter[3];
                const TArray<SCGParam4f>* lists[] = {
                    stockHardwarePass->m_CGFSParamsNoObj, stockHardwarePass->m_CGFSParamsObj };
                for (const auto* list : lists)
                    if (list)
                        for (int parameter = 0; parameter < list->Num(); ++parameter)
                        {
                            SCGParam4f& value = const_cast<SCGParam4f&>(list->Get(parameter));
                            if (!FastAsciiCaseCompare(value.m_Name.c_str(), "Diffuse"))
                                memcpy(terrainProjectionRows[1][6], value.mfGet(), sizeof(float)*4);
                        }
                globalOpacity = 1.0f;
            }
            if (stockParticleAmbient)
            {
                // Particle path below owns this payload independently.
                hasTerrainProjection = true;
                terrainProjectionRows[1][7][3] = -12.0f;
                terrainProjectionRows[0][6][3] = 1.0f;
                SShaderPassHW* particlePass = const_cast<SShaderPassHW*>(stockHardwarePass);
                TArray<SCGParam4f>* parameters[] = {
                    particlePass->m_CGFSParamsNoObj, particlePass->m_CGFSParamsObj };
                for (auto* list : parameters)
                    if (list)
                        for (int parameter = 0; parameter < list->Num(); ++parameter)
                            if (!FastAsciiCaseCompare(list->Get(parameter).m_Name.c_str(), "Ambient"))
                                memcpy(terrainProjectionRows[0][6], list->Get(parameter).mfGet(), sizeof(float) * 4);
                for (int channel = 0; channel < 4; ++channel)
                    primaryColorMask[channel] = 0.0f;
                normalMapTextureId = 0;
                // Opacity is already evaluated by the Cg Ambient.w
                // parameter (including the object's alpha). Do not fade the
                // fragment a second time through fixed-function overrides.
                globalOpacity = 1.0f;
                additiveMaterial = false;
            }
            if (stockTerrainLayerBase)
            {
                hasTerrainProjection = true;
                normalMapTextureId = 0;
                memset(terrainProjectionRows, 0, sizeof(terrainProjectionRows));
                auto layerParameter = [&](const char* name, float* destination)
                {
                    const TArray<SCGParam4f>* lists[] = {
                        &stockHardwarePass->m_VPParamsNoObj,
                        &stockHardwarePass->m_VPParamsObj,
                        stockHardwarePass->m_CGFSParamsNoObj,
                        stockHardwarePass->m_CGFSParamsObj };
                    for (const auto* list : lists)
                    {
                        if (!list) continue;
                        for (int i = 0; i < list->Num(); ++i)
                            if (!FastAsciiCaseCompare(list->Get(i).m_Name.c_str(), name))
                            {
                                memcpy(destination, const_cast<SCGParam4f&>(list->Get(i)).mfGet(),
                                    sizeof(float) * 4);
                                return;
                            }
                    }
                };
                layerParameter("LayerTexGen0", terrainProjectionRows[0][0]);
                layerParameter("LayerTexGen1", terrainProjectionRows[0][1]);
                layerParameter("CameraPos", terrainProjectionRows[0][2]);
                layerParameter("FadingDist", terrainProjectionRows[0][3]);
                if (m_RP.m_pRE && m_RP.m_pRE->m_CustomData && m_RP.m_pCurObject)
                {
                    // TerrainLayerTemplate supplies three-component planes;
                    // FromRE[3] is a fade distance, not a UV translation.
                    const float* data = static_cast<const float*>(m_RP.m_pRE->m_CustomData);
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        terrainProjectionRows[0][0][axis] = data[axis];
                        terrainProjectionRows[0][1][axis] = data[4 + axis];
                    }
                    terrainProjectionRows[0][0][3] = 0.0f;
                    terrainProjectionRows[0][1][3] = 0.0f;
                    terrainProjectionRows[0][3][0] = data[3];
                    Vec3d camera = GetCamera().GetPos(), localCamera = camera;
                    if (m_RP.m_pCurObject->m_ObjFlags & FOB_TRANS_MASK)
                        TransformPosition(localCamera, camera, m_RP.m_pCurObject->GetInvMatrix());
                    terrainProjectionRows[0][2][0] = localCamera.x;
                    terrainProjectionRows[0][2][1] = localCamera.y;
                    terrainProjectionRows[0][2][2] = localCamera.z;
                }
                layerParameter("LightPos", terrainProjectionRows[0][4]);
                layerParameter("Diffuse", terrainProjectionRows[0][5]);
                float layerBumpOffset[4]{};
                layerParameter("BumpOffset", layerBumpOffset);
                terrainProjectionRows[0][7][0] =
                    static_cast<float>(stockHardwarePass->m_StockProgramMask & 0xffffu);
                terrainProjectionRows[0][7][1] = layerBumpOffset[0];
                terrainProjectionRows[0][7][3] = -11.0f;
#if defined(__ANDROID__)
                static unsigned int detailSamples = 0;
                static int lastDetailFrame = -240;
                if (detailSamples < 32u && GetFrameID() - lastDetailFrame >= 240 && finalVertexData)
                {
                    if (FILE* probe = fopen("/sdcard/FarCry/vulkan_terrain_detail_probe.txt",
                        detailSamples == 0 ? "wb" : "ab"))
                    {
                        const float* position = static_cast<const float*>(finalVertexData);
                        fprintf(probe, "frame=%d texture=%d format=%d vertices=%u camera=%.5f,%.5f,%.5f fade=%.5f vertex0=%.5f,%.5f,%.5f planeS=%.5f,%.5f,%.5f planeT=%.5f,%.5f,%.5f state=%x clip=%.5f,%.5f,%.5f,%.5f\n",
                            GetFrameID(), textureIds[0], finalVertexFormat, finalVertexCount,
                            terrainProjectionRows[0][2][0], terrainProjectionRows[0][2][1], terrainProjectionRows[0][2][2],
                            terrainProjectionRows[0][3][0], position[0], position[1], position[2],
                            terrainProjectionRows[0][0][0], terrainProjectionRows[0][0][1], terrainProjectionRows[0][0][2],
                            terrainProjectionRows[0][1][0], terrainProjectionRows[0][1][1], terrainProjectionRows[0][1][2],
                            passState, clipPlane ? clipPlane[0] : 0.0f,
                            clipPlane ? clipPlane[1] : 0.0f,
                            clipPlane ? clipPlane[2] : 0.0f,
                            clipPlane ? clipPlane[3] : 1.0f);
                        fclose(probe);
                    }
                    lastDetailFrame = GetFrameID();
                    ++detailSamples;
                }
#endif
            }
            auto& waterColorVertices = scratchStorage.waterColorVertices;
            struct RestoreWaterVertexData
            {
                const void*& target;
                const void* original;
                ~RestoreWaterVertexData() { target = original; }
            } restoreWaterVertexData{ finalVertexData, finalVertexData };
            if (translatedWaterEffect)
            {
                const SArrayPointer* waterColorPointer = nullptr;
                const auto inspectWaterColorPointers = [&](const TArray<SArrayPointer*>& pointers)
                {
                    for (int pointerIndex = 0; pointerIndex < pointers.Num(); ++pointerIndex)
                    {
                        const SArrayPointer* pointer = pointers[pointerIndex];
                        if (pointer && pointer->eDst == eDstPointer_Color)
                            waterColorPointer = pointer;
                    }
                };
                if (m_RP.m_pCurTechnique)
                    inspectWaterColorPointers(m_RP.m_pCurTechnique->m_Pointers);
                inspectWaterColorPointers(stockHardwarePass->m_Pointers);
                const bool waterColorArrayDeclared = waterColorPointer != nullptr;
                if (waterColorPointer)
                {
                    CryVR::VulkanVertexFormat colorFormat{};
                    uint32_t sourceOffset = ~0u, destinationOffset = ~0u;
                    const bool byteColor = waterColorPointer->Type == 0x1401;
                    const bool validComponents = waterColorPointer->NumComponents == 3 ||
                        waterColorPointer->NumComponents == 4;
                    if (CryVR::GetVulkanVertexFormat(finalVertexFormat, colorFormat))
                        for (uint32_t attribute = 0; attribute < colorFormat.attributeCount; ++attribute)
                        {
                            const auto& value = colorFormat.attributes[attribute];
                            if (value.location == 2) destinationOffset = value.offset;
                            if ((waterColorPointer->ePT == eSrcPointer_Color && value.location == 2) ||
                                (waterColorPointer->ePT == eSrcPointer_SecColor && value.location == 4) ||
                                (waterColorPointer->ePT == eSrcPointer_Vert && value.location == 0) ||
                                (waterColorPointer->ePT == eSrcPointer_Normal && value.location == 1) ||
                                (waterColorPointer->ePT == eSrcPointer_Tex && value.location == 3))
                                sourceOffset = value.offset;
                        }
                    const byte* colorSource = static_cast<const byte*>(finalVertexData);
                    uint32_t colorSourceStride = colorFormat.stride;
                    const bool tangentColorSource = waterColorPointer->ePT == eSrcPointer_Tangent ||
                        waterColorPointer->ePT == eSrcPointer_Binormal ||
                        waterColorPointer->ePT == eSrcPointer_TNormal;
                    if (tangentColorSource)
                    {
                        colorSource = reinterpret_cast<const byte*>(sourceTangentBasis);
                        colorSourceStride = sizeof(SPipTangents);
                        sourceOffset = waterColorPointer->ePT == eSrcPointer_Tangent ? 0u :
                            (waterColorPointer->ePT == eSrcPointer_Binormal ? 12u : 24u);
                    }
                    if (byteColor && validComponents && sourceOffset != ~0u && destinationOffset != ~0u &&
                        colorSource && (!tangentColorSource || finalVertexCount <= static_cast<uint32_t>(vertices->m_NumVerts)) &&
                        sourceOffset + waterColorPointer->NumComponents <= colorSourceStride &&
                        destinationOffset + 4u <= colorFormat.stride)
                    {
                        if (tangentColorSource || sourceOffset != destinationOffset || waterColorPointer->NumComponents == 3)
                        {
                            const size_t size = static_cast<size_t>(finalVertexCount) * colorFormat.stride;
                            waterColorVertices.resize(size);
                            memcpy(waterColorVertices.data(), finalVertexData, size);
                            for (uint32_t vertex = 0; vertex < finalVertexCount; ++vertex)
                            {
                                const byte* source = colorSource +
                                    static_cast<size_t>(vertex) * colorSourceStride + sourceOffset;
                                byte* destination = waterColorVertices.data() +
                                    static_cast<size_t>(vertex) * colorFormat.stride + destinationOffset;
                                memcpy(destination, source, waterColorPointer->NumComponents);
                                if (waterColorPointer->NumComponents == 3) destination[3] = 255;
                            }
                            finalVertexData = waterColorVertices.data();
                        }
                    }
                    else
                        m_frameRenderer->RequirePanelFallback();
                }
                // This mask selects the shader's input array, not glColor or
                // the independently committed texture-environment constant.
                for (int channel = 0; channel < 4; ++channel)
                {
                    primaryColor[channel] = 1.0f;
                    primaryColorMask[channel] = waterColorArrayDeclared ? 0.0f : 1.0f;
                }
                normalMapTextureId = 0;
                hasTerrainProjection = true;
                memset(terrainProjectionRows, 0, sizeof(terrainProjectionRows));
                bool waterMatrixRectangle = false;
                auto waterParameter = [&](const char* name, float* target)
                {
                    SShaderPassHW* pass = const_cast<SShaderPassHW*>(stockHardwarePass);
                    TArray<SCGParam4f>* lists[] = { &pass->m_VPParamsNoObj, &pass->m_VPParamsObj,
                        pass->m_CGFSParamsNoObj, pass->m_CGFSParamsObj };
                    for (auto* list : lists)
                    {
                        if (!list) continue;
                        for (int i = 0; i < list->Num(); ++i)
                            if (!FastAsciiCaseCompare(list->Get(i).m_Name.c_str(), name))
                            {
                                memcpy(target, list->Get(i).mfGet(), sizeof(float) * 4);
                                if (!FastAsciiCaseCompare(name, "Matrix"))
                                    waterMatrixRectangle = (list->Get(i).m_dwBind & 0x80000) != 0;
                                return true;
                            }
                    }
                    return false;
                };
                const char* names[] = { "TexGenRipple0", "TexGenRipple1", "TexShiftRipple",
                    "TexDetailScale", "Matrix", "Ambient", "WaterColor", "CameraPos" };
                for (int i = 0; i < 8; ++i)
                    waterParameter(names[i], terrainProjectionRows[0][i]);
                terrainProjectionRows[0][7][3] = static_cast<float>(stockWaterMode);
                if (waterMatrixRectangle)
                {
                    // GLCGPShader::mfParameter4f multiplies CGPSParmRect by
                    // r_embm * screen dimensions. Convert those pixel offsets
                    // to normalized coordinates for the Vulkan screen sampler.
                    float width = static_cast<float>(crymax(1, GetWidth()));
                    float height = static_cast<float>(crymax(1, GetHeight()));
                    float* matrix = terrainProjectionRows[0][4];
                    matrix[0] *= CRenderer::CV_r_embm;
                    matrix[1] *= CRenderer::CV_r_embm * width / height;
                    matrix[2] *= CRenderer::CV_r_embm * height / width;
                    matrix[3] *= CRenderer::CV_r_embm;
                }
                if (stockWaterMode == 4)
                    terrainProjectionRows[0][6][0] = terrainProjectionRows[0][6][1] =
                        terrainProjectionRows[0][6][2] = 1.0f;
                if (stockWaterMode == 8 || stockWaterMode == 9 || stockWaterMode == 10)
                    waterParameter("TexShift", terrainProjectionRows[1][0]);
                if (stockWaterMode == 10)
                {
                    waterParameter("TexScale", terrainProjectionRows[1][1]);
                    terrainProjectionRows[0][5][0] = terrainProjectionRows[0][5][1] =
                        terrainProjectionRows[0][5][2] = terrainProjectionRows[0][5][3] = 1.0f;
                }
                if (stockWaterMode == 7)
                {
                    waterParameter("ReflectAmount", terrainProjectionRows[1][6]);
                    // Preserve the stock material/base-water pass until the
                    // separate mirrored-camera reflection pass is available.
                    terrainProjectionRows[1][6][0] = terrainProjectionRows[1][6][1] =
                        terrainProjectionRows[1][6][2] = 0.0f;
                    // CGRCWater has four samplers even though the mesh has no
                    // stored UV stream. Its Fresnel and base samplers are not
                    // optional fixed-function texture-combine stages.
                    static ITexPic* fresnel = EF_LoadTexture("Defaults/fresnel14",
                        FT_NOSTREAM | FT_CLAMP, 0, eTT_Base);
                    static ITexPic* baseWater = EF_LoadTexture("water_lm",
                        FT_NOSTREAM, 0, eTT_Base);
                    if (fresnel)
                    {
                        textureStage2.textureId = fresnel->GetTextureID();
                        textureStage2.colorOp = textureStage2.alphaOp = eCO_REPLACE;
                        textureStage2.useTexCoord1 = false;
                        textureStage2Ptr = &textureStage2;
                    }
                    if (baseWater)
                    {
                        textureStage3.textureId = baseWater->GetTextureID();
                        textureStage3.colorOp = textureStage3.alphaOp = eCO_REPLACE;
                        textureStage3.useTexCoord1 = false;
                        textureStage3Ptr = &textureStage3;
                    }
                }
                if (stockWaterMode == 6)
                {
                    waterParameter("RippleTexGen0", terrainProjectionRows[0][0]);
                    waterParameter("RippleTexGen1", terrainProjectionRows[0][1]);
                    waterParameter("WaterColor0", terrainProjectionRows[0][6]);
                    waterParameter("WaterColor1", terrainProjectionRows[1][5]);
                    waterParameter("ColorMinFresnel", terrainProjectionRows[1][0]);
                    waterParameter("ColorMaxFresnel", terrainProjectionRows[1][1]);
                    waterParameter("Constants", terrainProjectionRows[1][2]);
                }
                if (stockWaterMode == 6 || stockWaterMode == 7)
                {
                    // Retain the stock non-reflection fallback. Mode 6 uses
                    // the mirrored WaterMap descriptor when reflections are
                    // enabled; mode 7 uses this texture as its base-water map.
                    static ITexPic* environment = EF_LoadTexture("water_lm", FT_NOSTREAM, 0, eTT_Base);
                    if (environment)
                    {
                        STexPic* pic = static_cast<STexPic*>(environment);
                        if (!m_frameRenderer->HasLegacyTexture(pic->m_Bind))
                            EF_LoadTexture(pic->m_SearchName.c_str(), pic->m_Flags,
                                pic->m_Flags2 | FT2_RELOAD, pic->m_eTT,
                                pic->m_fAmount1, pic->m_fAmount2, pic->m_Id, pic->m_Bind);
                        secondaryTextureId = textureIds[1] = pic->m_Bind;
                    }
                }
            }
            if (fixedFunctionMaterialLighting && hasTranslatedDynamicLightPass)
            {
                // GL_COLOR_MATERIAL is disabled: pass-generated glColor
                // cannot replace the primary produced by vertex lighting.
                // Texture-environment constants remain independently active.
                for (int channel = 0; channel < 4; ++channel)
                    primaryColorMask[channel] = 0.0f;
            }
            Matrix44 fixedNormalMatrix;
            fixedNormalMatrix.SetIdentity();
            if (fixedLightCount)
            {
                mathMatrixInverse(fixedNormalMatrix.GetData(), modelView, g_CpuFlags);
                fixedNormalMatrix.Transpose();
            }
            m_frameRenderer->SetStockFixedLights(fixedLights, fixedLightCount,
                modelView, fixedNormalMatrix.GetData());
            m_frameRenderer->SetStockProfilePlants(stockPlantsProgram);
            if (stockAmbientDecal)
            {
                // DecalCharacter binds $FromRE, not a material diffuse map.
                // Match its Cg alpha/RGB product and preserve the lit skin.
                if (m_RP.m_pRE && m_RP.m_pRE->m_CustomTexBind[0] > 0)
                    textureIds[0] = m_RP.m_pRE->m_CustomTexBind[0];
                secondaryTextureId = normalMapTextureId = 0;
                textureStage2Ptr = textureStage3Ptr = nullptr;
                textureStages4To7Ptr = nullptr;
                hasTerrainProjection = true;
                terrainProjectionRows[1][7][3] = -16.0f;
                memcpy(terrainProjectionRows[0][6], hardwareAmbientParameter, sizeof(hardwareAmbientParameter));
                passState &= ~(GS_BLEND_MASK | GS_DEPTHWRITE);
                passState |= GS_BLSRC_SRCALPHA | GS_BLDST_ONEMINUSSRCALPHA | GS_DEPTHFUNC_EQUAL;
            }
            m_frameRenderer->SetStockDecalDraw(m_RP.m_pShader &&
                m_RP.m_pShader->m_eSort == eS_Decal);
            // The A/B GPU capture must include terrain techniques and all
            // translated water programs (including WaterVolume, whose draws
            // do not set QueueStockIndexedDraw's ocean waterEffect flag).
            m_frameRenderer->SetStockGpuProfileCategories(terrainShaderSort,
                stockWaterMode > 0,
                m_RP.m_pCurObject && m_RP.m_pCurObject->m_pCharInstance,
                m_RP.m_pShader && !FastAsciiCaseCompare(m_RP.m_pShader->GetName(), "TerrainDetailObjects"));
            const uint64 specularMask = stockLightTemplate ? stockHardwarePass->m_StockProgramMask : 0;
            int specularGlossTexture = 0;
            if (stockLightTemplate && lightPasses[lightPass][3] < 0.0f &&
                (specularMask & (0x10ull | 0x800ull)) && m_activeResources)
            {
                SEfResTexture* gloss = m_activeResources->m_Textures[EFTT_GLOSS];
                if (gloss && !gloss->m_TU.m_ITexPic && !gloss->m_Name.empty())
                    gloss->m_TU.m_ITexPic = m_cEF.LoadVulkanResourceTexture(gloss->m_Name.c_str(),
                        m_activeResources->m_TexturePath.c_str(), gloss->m_TU.GetTexFlags() | FT_NOSTREAM,
                        gloss->m_TU.GetTexFlags2(), eTT_Base, m_RP.m_pShader, gloss, gloss->m_Amount);
                if (gloss) gloss->m_TU.mfUpdate();
                if (gloss && gloss->m_TU.m_ITexPic)
                {
                    STexPic* pic = static_cast<STexPic*>(gloss->m_TU.m_ITexPic);
                    specularGlossTexture = pic->GetTextureID();
                    if (!m_frameRenderer->HasLegacyTexture(specularGlossTexture))
                        EF_LoadTexture(pic->m_SearchName.c_str(), pic->m_Flags | FT_NOSTREAM,
                            pic->m_Flags2 | FT2_RELOAD, pic->m_eTT, pic->m_fAmount1,
                            pic->m_fAmount2, pic->m_Id, pic->m_Bind);
                }
            }
            m_frameRenderer->SetStockSpecularProgram(stockLightTemplate ?
                (8u | ((specularMask & 0x400ull) ? 1u : 0u) |
                 ((specularMask & 0x800ull) ? 2u : 0u) |
                 ((specularMask & 0x20ull) ? 4u : 0u) |
                 ((specularMask & 0x10ull) ? 32u : 0u)) : 0u, specularGlossTexture);
            m_frameRenderer->SetStockNativeTerrain(nativeTerrainParameters, nativeTerrainMode);
            const bool queued = m_frameRenderer->QueueStockClientIndexedDraw(
                finalVertexData,
                finalVertexCount, indexData,
                drawIndexCount, finalVertexFormat, topology,
                passState, ResolveStockCullMode(),
                textureIds[0], secondaryTextureId,
                colorOps[0], alphaOps[0], colorOps[1], alphaOps[1],
                colorArgs[0], alphaArgs[0], textureEnvironmentColor,
                colorArgs[1], alphaArgs[1], textureEnvironmentColor,
                static_cast<uint32_t>(m_CurStencilState), m_CurStencRef, m_CurStencMask,
                modelView, drawTextureMatrix0, drawTextureMatrix1, lightPasses[lightPass].data(),
                lightPass > 0, globalOpacity, alphaTestRef,
                reinterpret_cast<const CryVR::VulkanBuffer*>(
                    vertices->m_VS[VSF_TANGENTS].m_VulkanBufferHandle),
                directionalLightmapPass ? 0 : normalMapTextureId,
                primaryColor, primaryColorMask, colorWriteMaskOverride,
                textureLodBias[0],
                !directionalLightmapPass && normalMapTextureId ?
                    normalMapLodBias : textureLodBias[1],
                textureStage2Ptr, textureStage3Ptr, m_activePolygonOffset,
                m_activePolygonOffsetFactor, m_activePolygonOffsetUnits, clipPlane,
                TextureWrapModeForId(textureIds[0]),
                TextureWrapModeForId(normalMapTextureId ? normalMapTextureId : textureIds[1]),
                TextureWrapModeForId(textureStage2Ptr ? textureStage2Ptr->textureId : 0),
                TextureWrapModeForId(textureStage3Ptr ? textureStage3Ptr->textureId : 0),
                textureStages4To7Ptr,
                lightmapTexCoords.empty() ? nullptr : lightmapTexCoords.data(),
                textureStage1UsesTexCoord1, textureStage0UsesTexCoord1,
                (m_activePass && m_activePass->m_eEvalRGB == eERGB_OneMinusFromClient) ||
                    terrainDetailSort,
                m_RP.m_pCurObject &&
                    (m_RP.m_pCurObject->m_ObjFlags & FOB_NEAREST) != 0,
                translatedWaterEffect,
                // The baked-lightmap fragment branch adds Ambient itself.
                // Applying evaluateStockLighting to its primary color first
                // would multiply that term by ambient a second time.
                !shaderLightingDisabled && !translatedWaterEffect &&
                    !stockParticleAmbient && !programInfo.plantsBump &&
                    !stockTerrainLayerBase &&
                    // CGRCTerrain programs own their ambient term in the
                    // fragment program (CGPSParam Ambient). Feeding them
                    // through generic per-pixel lighting multiplies the
                    // terrain vertex weights a second time and adds a
                    // diffuse term absent from the OpenGL terrain shaders.
                    stockTerrainLayers < 0 && stockTerrainOnlyLayers == 0 &&
                    stockTerrainAmbientMode == 0 && stockTerrainFogLayers < 0 &&
                    !(activeHardwareLightmapBasePass && lightPass == 0) &&
                    (hardwareLightPass || hardwareAmbientPass ||
                     (!hardwareAmbientPass && hasTranslatedDynamicLightPass) ||
                     (fixedFunctionAmbientOnlyPass && !fixedFunctionMaterialLighting)) ?
                    (m_activeHardwarePassType == eSHP_MAX && m_RP.m_pShader &&
                     (m_RP.m_pShader->m_Flags & EF_NEEDNORMALS) ? 2.0f : 1.0f) : 0.0f,
                directionalLightmapPass && textureStage2Ptr && textureStage3Ptr,
                activeHardwareLightmapBasePass && lightPass == 0 &&
                    (directionalLightmapPass ||
                     (lightmapStage == 1 && textureIds[1] > 0)),
                hasTerrainProjection ? &terrainProjectionRows[0][0][0] : nullptr,
                lightPassSpecularOcclusion[lightPass][0],
                lightPassSpecularOcclusion[lightPass][1],
                // CommonSubroutines::HDREncodeLM uses x4 in the ordinary
                // LDR pass, even when r_HDRFake defines _HDR_FAKE. Its HDR
                // branch requires BOTH _HDR and _HDR_FAKE and uses x16,
                // followed by HDR processing. This Vulkan pass is LDR.
                4.0f,
                hasWaterReflectionTransform ? waterReflectionModelView : nullptr,
                hasWaterReflectionTransform ? waterReflectionClipPlane : nullptr,
                waterReflectionUpdatePtr, vertices,
                validatedMinimumVertex, validatedMaximumVertex,
                ImmutableGeometryRevision(vertices, indices, finalVertexData, immutableNormalGeometry,
                    indexData, firstIndex),
                vertices->m_VS[VSF_TANGENTS].m_VData ?
                    vertices->m_VS[VSF_TANGENTS].m_VData : sourceTangentBasis);
            m_frameRenderer->SetStockNativeTerrain(nullptr, 0);
            // Auxiliary and subsequent material draws must not inherit this array.
            m_frameRenderer->SetStockProfilePlants(false);
            m_frameRenderer->SetStockDecalDraw(false);
            m_frameRenderer->SetStockGpuProfileCategories(false, false, false);
            m_frameRenderer->SetStockFixedLights({}, 0);
            m_frameRenderer->SetStockSpecularProgram(0);
#if defined(__ANDROID__)
            // Capture distance-dependent material changes with a hard bound.
            // The general material census cannot distinguish the same pass
            // at two distances, or identify a missing CPU tangent upload.
            if (queued && !m_collectingShadowCasters && GetFrameID() % 60 == 0 &&
                m_RP.m_pCurObject && m_activeResources)
            {
                SEfResTexture* diffuse = m_activeResources->m_Textures[EFTT_DIFFUSE];
                const char* name = diffuse ? diffuse->m_Name.c_str() : "";
                const bool physical = strstr(name, "bucket") || strstr(name, "meat") ||
                    strstr(name, "Bucket") || strstr(name, "Meat");
                const bool character = m_RP.m_pCurObject->m_pCharInstance ||
                    (m_RP.m_ObjFlags & FOB_NEAREST);
                static std::set<std::array<uint64, 4>> physicalStates, characterStates;
                auto& states = physical ? physicalStates : characterStates;
                if ((physical || character) && states.size() < 128)
                {
                    const float distance = (m_RP.m_pCurObject->GetTranslation() -
                        GetCamera().GetPos()).Length();
                    const std::array<uint64, 4> key = {{
                        reinterpret_cast<uint64>(m_activeResources),
                        reinterpret_cast<uint64>(m_activePass),
                        static_cast<uint64>(crymin(32, int(distance))),
                        uint64(m_RP.m_DynLMask) | (uint64(passState) << 32) }};
                    if (states.insert(key).second)
                        if (FILE* file = fopen("/sdcard/FarCry/vulkan_model_lighting.txt",
                            physicalStates.size() + characterStates.size() == 1 ? "wb" : "ab"))
                        {
                            fprintf(file, "frame=%d diffuse=%s distance=%g lod=%d shader=%s fp=%s mask=%llx hw=%d flags=%x lights=%x light=%d state=%x vf=%d bump=%d cpuTangents=%d lm=%d diffuseRGB=%g,%g,%g ambient=%g,%g,%g exponent=%g\n",
                                GetFrameID(), name, distance, m_RP.m_pCurObject->m_nLod,
                                m_RP.m_pShader ? m_RP.m_pShader->GetName() : "?",
                                stockHardwarePass ? stockHardwarePass->m_StockFragmentProgram : "fixed",
                                static_cast<unsigned long long>(stockHardwarePass ? stockHardwarePass->m_StockProgramMask : 0),
                                m_activeHardwarePassType, m_RP.m_ObjFlags, m_RP.m_DynLMask,
                                currentLight ? currentLight->m_Id : -1, passState, finalVertexFormat,
                                normalMapTextureId, vertices->m_VS[VSF_TANGENTS].m_VData != nullptr || sourceTangentBasis != nullptr,
                                m_RP.m_pCurObject->m_nLMId,
                                lightPasses[lightPass][4], lightPasses[lightPass][5], lightPasses[lightPass][6],
                                lightPasses[lightPass][8], lightPasses[lightPass][9], lightPasses[lightPass][10], lightPasses[lightPass][3]);
                            fclose(file);
                        }
                }
            }
            if (queued && programInfo.plantsBump)
            {
                static std::unordered_set<uint64> barkInputs;
                const uint64 key = uint64(uint32(textureIds[0])) |
                    (uint64(uint32(normalMapTextureId)) << 32);
                const bool first = barkInputs.empty();
                if (barkInputs.size() < 32 && barkInputs.insert(key).second)
                    if (FILE* file = fopen("/sdcard/FarCry/vulkan_bark_trace.txt", first ? "wb" : "ab"))
                    {
                        STexPic* baseImage = m_TexMan->GetByID(textureIds[0]);
                        STexPic* bumpImage = m_TexMan->GetByID(normalMapTextureId);
                        fprintf(file, "frame=%d shader=%s base=%d:%s bump=%d:%s vf=%d uvSource=%d ambient=%g,%g,%g diffuse=%g,%g,%g state=%x\n",
                            GetFrameID(), m_RP.m_pShader->GetName(), textureIds[0],
                            baseImage ? baseImage->m_SearchName.c_str() : "missing", normalMapTextureId,
                            bumpImage ? bumpImage->m_SearchName.c_str() : "missing", finalVertexFormat,
                            hardwareTempTexCoordCount, terrainProjectionRows[0][6][0],
                            terrainProjectionRows[0][6][1], terrainProjectionRows[0][6][2],
                            terrainProjectionRows[1][6][0], terrainProjectionRows[1][6][1],
                            terrainProjectionRows[1][6][2], passState);
                        fprintf(file, "gpuBase=%d gpuBump=%d tangentHandle=%p\n",
                            m_frameRenderer->HasLegacyTexture(textureIds[0]),
                            m_frameRenderer->HasLegacyTexture(normalMapTextureId),
                            reinterpret_cast<void*>(vertices->m_VS[VSF_TANGENTS].m_VulkanBufferHandle));
                        CryVR::VulkanVertexFormat barkFormat;
                        if (finalVertexData && CryVR::GetVulkanVertexFormat(finalVertexFormat, barkFormat))
                            for (uint32_t attribute = 0; attribute < barkFormat.attributeCount; ++attribute)
                                if (barkFormat.attributes[attribute].location == 3)
                                    for (int vertex = 0; vertex < crymin(finalVertexCount, 4); ++vertex)
                                    {
                                        const float* uv = reinterpret_cast<const float*>(
                                            static_cast<const byte*>(finalVertexData) + vertex * barkFormat.stride +
                                            barkFormat.attributes[attribute].offset);
                                        fprintf(file, "uv%d=%g,%g\n", vertex, uv[0], uv[1]);
                                    }
                        fclose(file);
                    }
            }
#endif
#if defined(__ANDROID__)
            // Bounded material census for the reported outdoor/character/
            // sprite failures. Capture unique inputs, not every frame.
            if (queued && !m_collectingShadowCasters)
            {
                // World and decal submissions can be queued without a current
                // object. Keep them in the material census so GPU draw IDs can
                // be resolved to their actual texture pair.
                const int elementType = m_RP.m_pRE ? m_RP.m_pRE->mfGetType() : -1;
                const bool relevant = (passState & (GS_BLEND_MASK | GS_ALPHATEST_MASK)) ||
                    terrainShaderSort || (m_RP.m_pCurObject && m_RP.m_pCurObject->m_pCharInstance) ||
                    (m_RP.m_pCurObject && (m_RP.m_ObjFlags & FOB_NEAREST)) ||
                    elementType == eDATA_ParticleSpray ||
                    elementType == eDATA_PolyBlend || elementType == eDATA_AnimPolyBlend ||
                    elementType == eDATA_ClientPoly;
                struct TraceHash {
                    size_t operator()(const std::array<uint64, 4>& key) const {
                        size_t hash = 1469598103934665603ull;
                        for (auto value : key) hash = (hash ^ value) * 1099511628211ull;
                        return hash;
                    }
                };
                static std::unordered_set<std::array<uint64, 4>, TraceHash> captured;
                const bool firstCapture = captured.empty();
                const std::array<uint64, 4> key = {{
                    reinterpret_cast<uint64>(m_RP.m_pShader),
                    static_cast<uint64>(static_cast<uint32>(textureIds[0])) |
                        (static_cast<uint64>(static_cast<uint32>(secondaryTextureId)) << 32),
                    static_cast<uint64>(finalVertexFormat) | (static_cast<uint64>(elementType) << 32),
                    static_cast<uint64>(passState) | (static_cast<uint64>(m_activeHardwarePassType) << 32) }};
                if (relevant && captured.size() < 512 &&
                    captured.insert(key).second)
                {
                    if (FILE* file = fopen("/sdcard/FarCry/vulkan_material_trace.txt", firstCapture ? "wb" : "ab"))
                    {
                        STexPic* image = m_TexMan ? m_TexMan->GetByID(textureIds[0]) : nullptr;
                        STexPic* image1 = m_TexMan && secondaryTextureId > 0 ?
                            m_TexMan->GetByID(secondaryTextureId) : nullptr;
                        SEfResTexture* diffuse = m_activeResources ? m_activeResources->m_Textures[EFTT_DIFFUSE] : nullptr;
                        fprintf(file, "frame=%d shader=%s program=%s mask=%llx re=%d char=%d flags=%x vf=%d uvSource=%d tex=%d image=%s tex1=%d image1=%s gpu=%d diffuse=%s objTex=%d custom=%d state=%x terrain=%d vp=%s fastPlants=%d\n",
                            GetFrameID(), m_RP.m_pShader ? m_RP.m_pShader->GetName() : "<none>",
                            stockHardwarePass ? stockHardwarePass->m_StockFragmentProgram : "fixed",
                            static_cast<unsigned long long>(stockHardwarePass ? stockHardwarePass->m_StockProgramMask : 0),
                            elementType, m_RP.m_pCurObject && m_RP.m_pCurObject->m_pCharInstance != nullptr,
                            m_RP.m_pCurObject ? m_RP.m_ObjFlags : 0,
                            finalVertexFormat, hardwareTempTexCoordCount, textureIds[0],
                            image ? image->m_SearchName.c_str() : "<none>", secondaryTextureId,
                            image1 ? image1->m_SearchName.c_str() : "<none>",
                            m_frameRenderer->HasLegacyTexture(textureIds[0]),
                            diffuse ? diffuse->m_Name.c_str() : "<none>",
                            m_RP.m_pCurObject ? m_RP.m_pCurObject->m_NumCM : -1,
                            m_RP.m_pRE ? m_RP.m_pRE->m_CustomTexBind[0] : -1, passState,
                            stockTerrainLayerBase,
                            stockHardwarePass ? stockHardwarePass->m_StockVertexProgram : "fixed",
                            hasTerrainProjection && terrainProjectionRows[0][7][3] == -13.0f);
                        fclose(file);

                    }
                }
            }
#endif
#if defined(__ANDROID__)
            if (m_sceneTrace && !m_collectingShadowCasters && m_sceneTraceLines++ < 256)
                fprintf(m_sceneTrace, "DRAW shader=%s program=%s type=%d flags=%x state=%x queued=%d baked=%d dot3=%d tex=%d,%d,%d,%d ambient=%.4f,%.4f,%.4f opacity=%.4f\n",
                    m_RP.m_pShader ? m_RP.m_pShader->GetName() : "<none>",
                    stockHardwarePass ? stockHardwarePass->m_StockFragmentProgram : "fixed",
                    m_activeHardwarePassType, m_RP.m_ObjFlags, passState, queued,
                    activeHardwareLightmapBasePass, directionalLightmapPass,
                    textureIds[0], secondaryTextureId, textureStage2.textureId, textureStage3.textureId,
                    lightPasses[lightPass][8], lightPasses[lightPass][9], lightPasses[lightPass][10], globalOpacity);
#endif
#if defined(__ANDROID__) && defined(CRYVR_RENDER_PROBES)
            if (translatedWaterEffect && queued)
            {
                static unsigned waterQueueProbeCounts[9] = {};
                static bool waterQueueProbeStarted = false;
                if (stockWaterMode > 0 && stockWaterMode < 9 &&
                    waterQueueProbeCounts[stockWaterMode] < 8 && GetFrameID() % 120 == 0)
                {
                    FILE* probe = fopen("/sdcard/FarCry/vulkan_water_queue_probe.txt",
                        waterQueueProbeStarted ? "ab" : "wb");
                    if (probe)
                    {
                        const float noClipPlane[4] = {};
                        const float* waterProbeClipPlane = clipPlane ? clipPlane : noClipPlane;
                        Vec3d firstPosition(0, 0, 0), lastPosition(0, 0, 0);
                        if (finalVertexData && finalVertexCount)
                        {
                            memcpy(&firstPosition, finalVertexData, sizeof(firstPosition));
                            CryVR::VulkanVertexFormat format{};
                            if (CryVR::GetVulkanVertexFormat(static_cast<uint32_t>(finalVertexFormat), format))
                            {
                                memcpy(&lastPosition,
                                    static_cast<const byte*>(finalVertexData) +
                                        (finalVertexCount - 1) * format.stride,
                                    sizeof(lastPosition));
                            }
                        }
                        fprintf(probe,
                            "shader=%s mode=%d queued=%d vertices=%u indices=%u vf=%d tex=%d,%d clip=%.5f,%.5f,%.5f,%.5f shift=%.5f,%.5f,%.5f,%.5f detail=%.5f,%.5f,%.5f,%.5f matrix=%.6f,%.6f,%.6f,%.6f ambient=%.4f,%.4f,%.4f,%.4f water=%.4f,%.4f,%.4f,%.4f camera=%.3f,%.3f,%.3f first=%.3f,%.3f,%.3f last=%.3f,%.3f,%.3f\n",
                            m_RP.m_pShader ? m_RP.m_pShader->GetName() : "<none>",
                            stockWaterMode, queued ? 1 : 0, finalVertexCount,
                            drawIndexCount, finalVertexFormat, textureIds[0],
                            secondaryTextureId, waterProbeClipPlane[0], waterProbeClipPlane[1],
                            waterProbeClipPlane[2], waterProbeClipPlane[3],
                            terrainProjectionRows[0][2][0], terrainProjectionRows[0][2][1],
                            terrainProjectionRows[0][2][2], terrainProjectionRows[0][2][3],
                            terrainProjectionRows[0][3][0], terrainProjectionRows[0][3][1],
                            terrainProjectionRows[0][3][2], terrainProjectionRows[0][3][3],
                            terrainProjectionRows[0][4][0], terrainProjectionRows[0][4][1],
                            terrainProjectionRows[0][4][2], terrainProjectionRows[0][4][3],
                            terrainProjectionRows[0][5][0], terrainProjectionRows[0][5][1],
                            terrainProjectionRows[0][5][2], terrainProjectionRows[0][5][3],
                            terrainProjectionRows[0][6][0], terrainProjectionRows[0][6][1],
                            terrainProjectionRows[0][6][2], terrainProjectionRows[0][6][3],
                            terrainProjectionRows[0][7][0], terrainProjectionRows[0][7][1],
                            terrainProjectionRows[0][7][2],
                            firstPosition.x, firstPosition.y, firstPosition.z,
                            lastPosition.x, lastPosition.y, lastPosition.z);
                        fclose(probe);
                        waterQueueProbeStarted = true;
                    }
                    ++waterQueueProbeCounts[stockWaterMode];
                }
            }
#endif
            CaptureStockLeafStatus(m_RP.m_pRE, m_RP.m_pShader,
                                   queued ? "draw-queued" : "queue-rejected", passState);
#if defined(__ANDROID__) && defined(CRYVR_RENDER_PROBES)
            // Inventory the actual material/program variants of the scene,
            // rather than limiting the audit to named problem assets. Once
            // per variant, with a hard bound and no per-frame engine logging.
            static std::vector<std::array<uint64, 4>> materialProgramProbeKeys;
            const std::array<uint64, 4> programKey = {
                m_RP.m_pShader ? static_cast<uint64>(m_RP.m_pShader->m_Id) : 0,
                stockHardwarePass ? stockHardwarePass->m_StockProgramMask : 0,
                static_cast<uint64>(passState) |
                    (static_cast<uint64>(m_activeHardwarePassType) << 32),
                static_cast<uint64>(activeHardwareLightmapBasePass) |
                    (static_cast<uint64>(directionalLightmapPass) << 1) |
                    (static_cast<uint64>(queued) << 2) };
            if (materialProgramProbeKeys.size() < 256 &&
                std::find(materialProgramProbeKeys.begin(), materialProgramProbeKeys.end(),
                          programKey) == materialProgramProbeKeys.end())
            {
                FILE* probe = fopen("/sdcard/FarCry/vulkan_material_program_probe.txt",
                    materialProgramProbeKeys.empty() ? "wb" : "ab");
                if (probe)
                {
                    fprintf(probe, "shader=%s vp=%s fp=%s mask=%llx state=%x hw=%d queued=%d vf=%d textures=%d,%d,%d,%d baked=%d directional=%d ambient=%.5f,%.5f,%.5f opacity=%.5f\n",
                        m_RP.m_pShader ? m_RP.m_pShader->GetName() : "<none>",
                        stockHardwarePass ? stockHardwarePass->m_StockVertexProgram : "<fixed>",
                        stockHardwarePass ? stockHardwarePass->m_StockFragmentProgram : "<fixed>",
                        static_cast<unsigned long long>(programKey[1]), passState,
                        m_activeHardwarePassType, queued, finalVertexFormat, textureIds[0],
                        secondaryTextureId, textureStage2.textureId, textureStage3.textureId,
                        activeHardwareLightmapBasePass, directionalLightmapPass,
                        lightPasses[lightPass][8], lightPasses[lightPass][9],
                        lightPasses[lightPass][10], globalOpacity);
                    fclose(probe);
                }
                materialProgramProbeKeys.push_back(programKey);
            }
            // Capture only the reported rays/bone materials, once per state.
            // No per-frame logging or full texture dumps during level loading.
            static std::vector<std::array<uint32_t, 4>> rayBoneProbeKeys;
            STexPic* rayBoneImage = m_TexMan ? m_TexMan->GetByID(textureIds[0]) : nullptr;
            if (queued && m_RP.m_pShader && rayBoneProbeKeys.size() < 32 &&
                (muzzleFlashShader || (rayBoneImage &&
                 (strstr(rayBoneImage->m_SourceName.c_str(), "skeleton") ||
                  strstr(rayBoneImage->m_SourceName.c_str(), "Skeleton")))))
            {
                const std::array<uint32_t, 4> key = {
                    static_cast<uint32_t>(m_RP.m_pShader->m_Id),
                    static_cast<uint32_t>(textureIds[0]), passState,
                    static_cast<uint32_t>(m_activeHardwarePassType) };
                if (std::find(rayBoneProbeKeys.begin(), rayBoneProbeKeys.end(), key) == rayBoneProbeKeys.end())
                {
                    FILE* probe = fopen("/sdcard/FarCry/vulkan_ray_bone_probe.txt",
                        rayBoneProbeKeys.empty() ? "wb" : "ab");
                    if (probe)
                    {
                        fprintf(probe, "shader=%s texture=%s state=%x hw=%d shaderLM=%x passLM=%x vf=%d ops=%d,%d args=%u,%u baked=%d directional=%d lmStage=%d stage1=%d normal=%d generatedRay=%u lighting=%d ambient=%.5f,%.5f,%.5f gpuTexture=%d primaryMask=%.0f,%.0f,%.0f residentBytes=%llu uploadError=%s\n",
                            m_RP.m_pShader->GetName(), rayBoneImage ? rayBoneImage->m_SourceName.c_str() : "<missing>",
                            passState, m_activeHardwarePassType, m_RP.m_pShader->m_LMFlags,
                            activeHardwareLightFlags, finalVertexFormat, colorOps[0], alphaOps[0],
                            colorArgs[0], alphaArgs[0], activeHardwareLightmapBasePass,
                            directionalLightmapPass, lightmapStage, secondaryTextureId,
                            normalMapTextureId, static_cast<unsigned>(muzzleFlashVertices.size()),
                            hardwareAmbientPass || hardwareLightPass,
                            lightPasses[lightPass][8], lightPasses[lightPass][9], lightPasses[lightPass][10],
                            m_frameRenderer->HasLegacyTexture(textureIds[0]),
                            primaryColorMask[0], primaryColorMask[1], primaryColorMask[2],
                            static_cast<unsigned long long>(m_frameRenderer->GetLegacyTextureBytes()),
                            m_frameRenderer->GetLastError());
                        fclose(probe);
                        rayBoneProbeKeys.push_back(key);
                    }
                }
            }
            // Sample lighting across camera cuts without a per-frame audit.
            // Two categories retain both baked meshes and unbaked models.
            static int lightingProbeFrame = -1;
            static uint32_t lightingProbeRecords = 0;
            static std::vector<std::array<uint32_t, 3>> lightingProbeKeys[2];
            if (queued && lightPass == 0 && m_RP.m_pShader &&
                m_RP.m_pCurObject && m_RP.m_pRE && lightingProbeRecords < 256 &&
                (activeHardwareLightmapBasePass ||
                 (m_RP.m_pShader->m_Flags & EF_USELIGHTS)))
            {
                if (lightingProbeFrame < 0 || GetFrameID() - lightingProbeFrame >= 600)
                {
                    lightingProbeFrame = GetFrameID();
                    lightingProbeKeys[0].clear();
                    lightingProbeKeys[1].clear();
                }
                const int category = activeHardwareLightmapBasePass ? 0 : 1;
                auto& keys = lightingProbeKeys[category];
                const std::array<uint32_t, 3> key = {
                    static_cast<uint32_t>(m_RP.m_pShader->m_Id),
                    static_cast<uint32_t>(textureIds[0]), passState };
                if (GetFrameID() == lightingProbeFrame && keys.size() < 8 &&
                    std::find(keys.begin(), keys.end(), key) == keys.end())
                {
                    FILE* probe = fopen("/sdcard/FarCry/vulkan_lighting_probe.txt",
                                        lightingProbeRecords ? "ab" : "wb");
                    if (probe)
                    {
                        const auto& p = lightPasses[lightPass];
                        const Vec3d camera = GetCamera().GetPos();
                        fprintf(probe, "frame=%d camera=%.2f,%.2f,%.2f shader=%s re=%d vf=%d state=0x%x hw=%d flags=0x%x lm=%d dir=%d baked=%d ambientPass=%d multiAmbient=%d lights=0x%x draws=%u ambient=%.5f,%.5f,%.5f diffuse=%.5f,%.5f,%.5f primary=%.5f,%.5f,%.5f fog=%d texture=%d\n",
                            GetFrameID(), camera.x, camera.y, camera.z,
                            m_RP.m_pShader->GetName(), m_RP.m_pRE->mfGetType(),
                            finalVertexFormat, passState, m_activeHardwarePassType,
                            activeHardwareLightFlags, m_RP.m_pCurObject->m_nLMId,
                            m_RP.m_pCurObject->m_nLMDirId, activeHardwareLightmapBasePass,
                            hardwareAmbientPass, hardwareMultiLightsAmbient, m_RP.m_DynLMask,
                            static_cast<unsigned>(lightPasses.size()), p[8], p[9], p[10],
                            p[4], p[5], p[6], primaryColor[0], primaryColor[1], primaryColor[2],
                            m_RP.m_pFogVolume != nullptr, textureIds[0]);
                        fclose(probe);
                        keys.push_back(key);
                        ++lightingProbeRecords;
                    }
                }
            }
            // Capture each decal material/state once, bounded to 32 records.
            // This remains cheap while identifying the actual wall material,
            // rather than assuming every decal uses alpha blending.
            static std::vector<std::array<uint32_t, 3>> decalProbeKeys;
            if (queued && lightPass == 0 && m_RP.m_pShader &&
                (m_RP.m_pShader->m_eSort == eS_Decal ||
                 strstr(m_RP.m_pShader->GetName(), "decal")) &&
                decalProbeKeys.size() < 32)
            {
                const std::array<uint32_t, 3> key = {
                    static_cast<uint32_t>(m_RP.m_pShader->m_Id),
                    static_cast<uint32_t>(textureIds[0]), passState };
                if (std::find(decalProbeKeys.begin(), decalProbeKeys.end(), key) == decalProbeKeys.end())
                {
                    FILE* probe = fopen("/sdcard/FarCry/vulkan_decal_probe.txt",
                                        decalProbeKeys.empty() ? "wb" : "ab");
                    if (probe)
                    {
                        STexPic* image = m_TexMan ? m_TexMan->GetByID(textureIds[0]) : nullptr;
                        fprintf(probe, "shader=%s sort=%d texture=%s id=%d state=0x%x hw=%d objectFlags=0x%x ops=%d,%d args=%u,%u opacity=%.4f primaryAlpha=%.4f ambient=%.4f,%.4f,%.4f lighting=%d resources=%d\n",
                            m_RP.m_pShader->GetName(), m_RP.m_pShader->m_eSort,
                            image ? image->m_SourceName.c_str() : "<missing>", textureIds[0],
                            passState, m_activeHardwarePassType,
                            m_RP.m_pCurObject ? m_RP.m_pCurObject->m_ObjFlags : 0,
                            colorOps[0], alphaOps[0], colorArgs[0], alphaArgs[0],
                            globalOpacity, primaryColor[3], materialLighting[8],
                            materialLighting[9], materialLighting[10],
                            hardwareAmbientPass, m_activeResources ? m_activeResources->m_Id : -1);
                        fclose(probe);
                        decalProbeKeys.push_back(key);
                    }
                }
            }
            // One bounded snapshot of opaque LM draws. Unlike the old audit,
            // this never traces subsequent frames or texture upload traffic.
            // The binary contains the exact private streams submitted above,
            // allowing atlas/triangle correspondence to be inspected offline.
            static int probeFrame = -1;
            static uint32_t probeRecords = 0;
            static size_t probeBytes = 0;
            if (queued && directionalLightmapPass && lightPass == 0 &&
                (passState & GS_BLEND_MASK) == 0 &&
                lightmapTexCoords.size() == static_cast<size_t>(finalVertexCount) * 2)
            {
                CryVR::VulkanVertexFormat probeFormat{};
                if (probeFrame < 0) probeFrame = GetFrameID();
                const size_t recordBytes = static_cast<size_t>(finalVertexCount) *
                    (sizeof(float) * 2 +
                     (CryVR::GetVulkanVertexFormat(finalVertexFormat, probeFormat) ? probeFormat.stride : 0)) +
                    static_cast<size_t>(drawIndexCount) * sizeof(uint16_t) + 2048;
                if (GetFrameID() == probeFrame && probeRecords < 16 &&
                    probeFormat.stride && recordBytes < 4u * 1024u * 1024u - probeBytes)
                {
                    FILE* probe = fopen("/sdcard/FarCry/vulkan_lightmap_probe.bin",
                                        probeRecords == 0 ? "wb" : "ab");
                    if (probe)
                    {
                        const uint32_t header[24] = {
                            0x4c4d5652u, 1u, static_cast<uint32_t>(GetFrameID()), probeRecords,
                            static_cast<uint32_t>(finalVertexFormat), probeFormat.stride,
                            finalVertexCount, drawIndexCount, static_cast<uint32_t>(topology),
                            passState, static_cast<uint32_t>(textureIds[0]),
                            static_cast<uint32_t>(secondaryTextureId),
                            static_cast<uint32_t>(textureStage2.textureId),
                            static_cast<uint32_t>(textureStage3.textureId), shadowMapStageMask,
                            static_cast<uint32_t>(m_activeHardwarePassType), activeHardwareLightFlags,
                            static_cast<uint32_t>(m_RP.m_pCurObject->m_ObjFlags),
                            textureStage1UsesTexCoord1 ? 1u : 0u,
                            textureStage2.useTexCoord1 ? 1u : 0u,
                            textureStage3.useTexCoord1 ? 1u : 0u,
                            static_cast<uint32_t>(firstIndex), 0u, 0u
                        };
                        char names[5][256]{};
                        if (m_RP.m_pShader)
                            snprintf(names[0], sizeof(names[0]), "%s", m_RP.m_pShader->m_Name.c_str());
                        for (uint32_t stage = 0; stage < 4; ++stage)
                            if (STexPic* image = m_TexMan->GetByID(static_cast<int>(header[10 + stage])))
                                snprintf(names[stage + 1], sizeof(names[stage + 1]), "%s",
                                         image->m_SearchName.c_str());
                        fwrite(header, sizeof(header), 1, probe);
                        fwrite(names, sizeof(names), 1, probe);
                        fwrite(modelView, sizeof(float) * 16, 1, probe);
                        fwrite(lightPasses[lightPass].data(), sizeof(float) * 19, 1, probe);
                        fwrite(drawTextureMatrix0, sizeof(float) * 16, 1, probe);
                        fwrite(drawTextureMatrix1, sizeof(float) * 16, 1, probe);
                        fwrite(textureStage2.uvTransform, sizeof(float) * 9, 1, probe);
                        fwrite(textureStage3.uvTransform, sizeof(float) * 9, 1, probe);
                        fwrite(finalVertexData, static_cast<size_t>(finalVertexCount) * probeFormat.stride, 1, probe);
                        fwrite(lightmapTexCoords.data(), static_cast<size_t>(finalVertexCount) * sizeof(float) * 2, 1, probe);
                        fwrite(indexData, static_cast<size_t>(drawIndexCount) * sizeof(uint16_t), 1, probe);
                        fclose(probe);
                        if (probeRecords == 0)
                            CryLogAlways("Vulkan: saved bounded lightmap input snapshot to vulkan_lightmap_probe.bin (at most one frame, 16 draws, 4 MiB)");
                        ++probeRecords;
                        probeBytes += recordBytes;
                    }
                }
            }
#endif
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

        // CGLRenderer draws detail overlays once after the material's final
        // pass. Reuse this chunk's geometry and the engine's generated $Fog
        // texture so Vulkan applies the same multiplicative detail blend.
        // Resource/object overrides have been resolved for the main draw.
        // OpenGL's subsequent auxiliary passes inherit that effective state.
        m_CurState = static_cast<int>(renderState);
        bool finalMaterialPass = false;
        if (m_RP.m_pShader && m_activePass)
        {
            if (m_activeHardwarePassType == eSHP_MAX && m_RP.m_pShader->m_Passes.Num() > 0)
                finalMaterialPass = m_activePass ==
                    &m_RP.m_pShader->m_Passes[m_RP.m_pShader->m_Passes.Num() - 1];
            else if (m_RP.m_pCurTechnique && m_RP.m_pCurTechnique->m_Passes.Num() > 0 &&
                     m_activePass == &m_RP.m_pCurTechnique->m_Passes[
                         m_RP.m_pCurTechnique->m_Passes.Num() - 1])
            {
                finalMaterialPass = m_activeHardwarePassType == eSHP_General ||
                                    m_activeHardwarePassType == eSHP_MultiLights;
                if (!finalMaterialPass &&
                    (m_activeHardwarePassType == eSHP_Light ||
                     m_activeHardwarePassType == eSHP_DiffuseLight ||
                     m_activeHardwarePassType == eSHP_SpecularLight) &&
                    m_RP.m_pCurLight && m_RP.m_NumActiveDLights > 0)
                {
                    const uint32_t passLightTypes = activeHardwareLightFlags & DLF_LIGHTTYPE_MASK;
                    const uint32_t passLightFlags =
                        static_cast<SShaderPassHW*>(m_activePass)->m_LightFlags;
                    const bool hasBakedLightmap =
                        (m_activeResources && m_activeResources->m_Textures[EFTT_LIGHTMAP]) ||
                        (m_RP.m_pCurObject && m_RP.m_pCurObject->m_nLMId);
                    const bool currentLightIsLast = [&]()
                    {
                        bool foundCurrent = false;
                        for (int lightIndex = 0; lightIndex < m_RP.m_NumActiveDLights; ++lightIndex)
                        {
                            CDLight* light = m_RP.m_pActiveDLights[lightIndex];
                            if (!light || (passLightTypes &&
                                !(passLightTypes & (light->m_Flags & DLF_LIGHTTYPE_MASK))) ||
                                ((activeHardwareLightFlags & LMF_IGNOREPROJLIGHTS) &&
                                 (light->m_Flags & DLF_PROJECT)) ||
                                !(light->m_Flags & (DLF_DIRECTIONAL | DLF_POINT | DLF_PROJECT)))
                                continue;
                            if (passLightFlags & DLF_LM)
                            {
                                if (!(light->m_Flags & DLF_LM) || !hasBakedLightmap ||
                                    (light->m_SpecColor.r <= 0.01f &&
                                     light->m_SpecColor.g <= 0.01f &&
                                     light->m_SpecColor.b <= 0.01f))
                                    continue;
                            }
                            if (m_activeHardwarePassType == eSHP_DiffuseLight &&
                                (light->m_Flags & DLF_LM) && hasBakedLightmap)
                                continue;
                            if (m_activeHardwarePassType == eSHP_SpecularLight &&
                                light->m_SpecColor.r == 0.0f &&
                                light->m_SpecColor.g == 0.0f &&
                                light->m_SpecColor.b == 0.0f)
                                continue;
                            if ((activeHardwareLightFlags & LMF_USEOCCLUSIONMAP) &&
                                (!m_RP.m_pCurObject ||
                                 *reinterpret_cast<const int*>(m_RP.m_pCurObject->m_OcclLights) == 0))
                                continue;
                            if ((activeHardwareLightFlags & LMF_NOBUMP) && m_activeResources &&
                                (!m_activeResources->m_Textures[EFTT_BUMP] ||
                                 !(m_activeResources->m_Textures[EFTT_BUMP]->m_TU.m_nFlags & FTU_NOBUMP)))
                                continue;
                            if (light == m_RP.m_pCurLight)
                            {
                                foundCurrent = true;
                                continue;
                            }
                            if (foundCurrent)
                                return false;
                        }
                        return foundCurrent;
                    }();
                    finalMaterialPass = currentLightIsLast;
                }
            }
        }
        CryVR::VulkanVertexFormat detailVertexFormat{};
        uint32_t detailPositionOffset = 0xffffffffu;
        uint32_t detailUvOffset = 0xffffffffu;
        if (CryVR::GetVulkanVertexFormat(finalVertexFormat, detailVertexFormat))
            for (uint32_t attribute = 0; attribute < detailVertexFormat.attributeCount; ++attribute)
            {
                const VkVertexInputAttributeDescription& input = detailVertexFormat.attributes[attribute];
                if (input.location == 0 && input.format == VK_FORMAT_R32G32B32_SFLOAT)
                    detailPositionOffset = input.offset;
                if (input.location == 3 && input.format == VK_FORMAT_R32G32_SFLOAT)
                    detailUvOffset = input.offset;
            }
        SEfResTexture* detailTexture = m_activeResources ?
            m_activeResources->m_Textures[EFTT_DETAIL_OVERLAY] : nullptr;
        if (finalMaterialPass && detailTexture && CV_r_detailtextures &&
            !(m_RP.m_ObjFlags & (FOB_ZPASS | FOB_FOGPASS)) && m_RP.m_pRE && m_RP.m_pCurObject &&
            finalVertexData && detailPositionOffset != 0xffffffffu &&
            detailUvOffset != 0xffffffffu && m_TexMan && m_TexMan->m_Text_Fog)
        {
            detailTexture->m_TU.mfUpdate();
            const int detailTextureId = detailTexture->m_TU.m_ITexPic ?
                detailTexture->m_TU.m_ITexPic->GetTextureID() : 0;
            const int detailFogTextureId = m_TexMan->m_Text_Fog->GetTextureID();
            const float detailDistance = CV_r_detaildistance;
            const float objectDistance = m_RP.m_pRE->mfMinDistanceToCamera(m_RP.m_pCurObject);
            if (detailTextureId > 0 && detailFogTextureId > 0 &&
                detailDistance > 0.0f && objectDistance <= detailDistance + 1.0f)
            {
                // EF_DrawDetailOverlayPasses explicitly disables scissor;
                // direct STexPic::Set bindings retain each TMU's LOD bias.
                SetScissor(0, 0, 0, 0);
                float detailUScale = detailTexture->m_TexModificator.m_Tiling[0];
                float detailVScale = detailTexture->m_TexModificator.m_Tiling[1];
                if (detailUScale == 0.0f) detailUScale = CV_r_detailscale;
                if (detailVScale == 0.0f) detailVScale = CV_r_detailscale;
                const float* const finalVertices = static_cast<const float*>(finalVertexData);
                float layerDistance = detailDistance;
                const int detailLayerCount = clamp_tpl(CV_r_detailnumlayers, 1, 4);
                for (int layerIndex = 0; layerIndex < detailLayerCount; ++layerIndex)
                {
                    if (objectDistance > layerDistance + 1.0f)
                        break;
                    SMFog detailVolume{};
                    detailVolume.m_FogInfo.m_WaveFogGen.m_eWFType = eWF_None;
                    detailVolume.m_fMaxDist = layerDistance;
                    SMFog* savedFogVolume = m_RP.m_pFogVolume;
                    m_RP.m_pFogVolume = &detailVolume;
                    CryVR::VulkanStockLinearTexgen detailFogTexgen;
                    BuildStockFogTexgen(detailFogTexgen, false);
                    m_RP.m_pFogVolume = savedFogVolume;
                    const float* fogPlane = detailFogTexgen.planes[0];
                    std::vector<float>& detailFogCoordinates = m_detailFogCoordinatesScratch;
                    detailFogCoordinates.resize(static_cast<size_t>(finalVertexCount) * 2u);
                    for (uint32_t vertex = 0; vertex < finalVertexCount; ++vertex)
                    {
                        const float* position = reinterpret_cast<const float*>(
                            reinterpret_cast<const uint8_t*>(finalVertices) +
                            static_cast<size_t>(vertex) * detailVertexFormat.stride + detailPositionOffset);
                        detailFogCoordinates[static_cast<size_t>(vertex) * 2u] =
                            fogPlane[0] * position[0] + fogPlane[1] * position[1] +
                            fogPlane[2] * position[2] + fogPlane[3];
                        detailFogCoordinates[static_cast<size_t>(vertex) * 2u + 1u] = 0.49f;
                    }
                    float detailTextureMatrix[16]{};
                    float detailFogMatrix[16]{};
                    SetIdentityScreenMatrix(detailTextureMatrix);
                    SetIdentityScreenMatrix(detailFogMatrix);
                    detailTextureMatrix[0] = detailUScale;
                    detailTextureMatrix[5] = detailVScale;
                    uint32_t detailState = GS_BLSRC_DSTCOL | GS_BLDST_SRCCOL;
                    if (m_RP.m_FlagsPerFlush & RBSI_WASDEPTHWRITE)
                        detailState |= GS_DEPTHFUNC_EQUAL;
                    EF_SetState(static_cast<int>(detailState));
                    detailState = static_cast<uint32_t>(m_CurState);
                    const uint32_t detailColorArg0 = static_cast<uint32_t>(
                        eCA_Texture | (eCA_Constant << 3));
                    const uint32_t detailColorArg1 = static_cast<uint32_t>(
                        eCA_Constant | (eCA_Previous << 3) | (eCA_Texture << 6));
                    m_frameRenderer->ResetStockLinearTexgen();
                    const bool detailQueued = m_frameRenderer->QueueStockClientIndexedDraw(
                        finalVertexData, finalVertexCount, indexData, drawIndexCount,
                        finalVertexFormat, topology, detailState,
                        ResolveStockCullMode(),
                        detailTextureId, detailFogTextureId,
                        // CGRCDetailAtten lerps the unscaled detail texel to
                        // 0.5. Multiplying it by 0.5 here darkened every layer
                        // before DST_COLOR/SRC_COLOR multiplied the scene.
                        eCO_REPLACE, eCO_REPLACE,
                        eCO_BLENDTEXTUREALPHA, eCO_REPLACE,
                        detailColorArg0, DEF_TEXARG0, 0xff808080u,
                        detailColorArg1, DEF_TEXARG0, 0xff808080u,
                        static_cast<uint32_t>(m_CurStencilState), m_CurStencRef, m_CurStencMask,
                        modelView, detailTextureMatrix, detailFogMatrix, nullptr, false,
                        1.0f, (m_RP.m_FlagsPerFlush & RBSI_ALPHATEST) ? alphaTestRef : 0.0f,
                        reinterpret_cast<const CryVR::VulkanBuffer*>(
                            vertices->m_VS[VSF_TANGENTS].m_VulkanBufferHandle),
                        0, nullptr, nullptr, colorWriteMaskOverride,
                        m_stageLodBias[0], m_stageLodBias[1], nullptr, nullptr,
                        m_activeHardwarePassType == eSHP_MAX && m_activePolygonOffset,
                        m_activePolygonOffsetFactor, m_activePolygonOffsetUnits,
                        clipPlane,
                        TextureWrapModeForId(detailTextureId),
                        TextureWrapModeForId(detailFogTextureId), -1, -1, nullptr,
                        detailFogCoordinates.data(), true, false, false,
                        (m_RP.m_pCurObject->m_ObjFlags & FOB_NEAREST) != 0,
                        false, false, false, false);
                    if (!detailQueued)
                        m_frameRenderer->RequirePanelFallback();
                    detailUScale *= 2.0f;
                    detailVScale *= 2.0f;
                    layerDistance *= 0.5f;
                }
                // EF_DrawDetailOverlayPasses ends with BindNULL(0).
                // The queued draws own their samplers; later render elements
                // must observe disabled targets rather than the material's
                // bindings retained before this auxiliary pass.
                for (int stage = 0; stage < 8; ++stage)
                    m_stageTextureIds[stage] = 0;
                if (m_TexMan) m_TexMan->m_nCurStages = 0;
                m_RP.m_FlagsModificators &= ~(RBMF_TCM | RBMF_TCG);
            }
        }
        if (finalMaterialPass && CV_r_VolumetricFog && m_RP.m_pFogVolume &&
            !(m_RP.m_ObjFlags & FOB_ZPASS) && !(m_RP.m_FlagsPerFlush & RBSI_FOGVOLUME) &&
            m_TexMan && m_TexMan->m_Text_Fog && m_TexMan->m_Text_Fog_Enter &&
            m_RP.m_pShader && finalVertexData)
        {
            SMFog* volume = m_RP.m_pFogVolume;
            const float waterLevel = iSystem->GetI3DEngine()->GetWaterLevel();
            const bool vertexFog = (m_Features & RFT_HW_VS) && CV_r_Quality_BumpMapping != 0;
            const bool drawCaustics = (vertexFog || (m_RP.m_pShader->m_Flags & EF_HASVSHADER)) &&
                m_RP.m_pShader->m_eSort != eS_Water &&
                fabs(volume->m_Dist - waterLevel) < 0.1f && SRendItem::m_RecurseLevel <= 1 &&
                (!m_RP.m_pRE || m_RP.m_pRE->mfMinDistanceToCamera(m_RP.m_pCurObject) < 40.0f);
            if (drawCaustics)
            {
                // Use the parsed OpenGL template's sequence, including its
                // frame count and timer-pause semantics. Do not assume names
                // or a fixed number of causq images in the installed assets.
                SShader* caustics = m_cEF.m_ShaderFogCaust;
                SShaderTexUnit* unit = nullptr;
                if (caustics && caustics->m_HWTechniques.Num() &&
                    caustics->m_HWTechniques[0]->m_Passes.Num() &&
                    caustics->m_HWTechniques[0]->m_Passes[0].m_TUnits.Num())
                    unit = &caustics->m_HWTechniques[0]->m_Passes[0].m_TUnits[0];
                if (unit && (!unit->m_AnimInfo || unit->m_AnimInfo->m_NumAnimTexs > 0))
                    unit->mfUpdate();
                if (unit && unit->m_TexPic &&
                    m_frameRenderer->HasLegacyTexture(unit->m_TexPic->GetTextureID()))
                {
                    float parameters[64]{};
                    for (int row = 0; row < 4; ++row)
                    {
                        // TranspObjMatrix uses legacy Matrix44 columns;
                        // translation is in its last row (TransformPointOLD).
                        SParamComp_ObjMatrix matrix;
                        matrix.m_Offs = 0x40000000 | row;
                        matrix.mfGet4f(parameters + row * 4);
                    }
                    const Vec3d camera = GetCamera().GetPos();
                    parameters[16] = camera.x; parameters[17] = camera.y; parameters[18] = camera.z;
                    parameters[19] = 0.025f;
                    parameters[20] = waterLevel;
                    SParamComp_Time time;
                    time.m_Scale = 0.06f; parameters[21] = time.mfGet();
                    time.m_Scale = 0.12f; parameters[22] = time.mfGet();
                    parameters[31] = -12.0f;
                    float identity[16]; SetIdentityScreenMatrix(identity);
                    float color[4] = { volume->m_Color.r, volume->m_Color.g, volume->m_Color.b, 1.0f };
                    const float mask[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                    float noLighting[19]{};
                    uint32_t state = GS_BLSRC_ONE | GS_BLDST_ONE;
                    if (m_RP.m_pShader->m_Flags2 & EF2_OPAQUE) state |= GS_DEPTHFUNC_EQUAL;
                    EF_SetState(static_cast<int>(state));
                    m_frameRenderer->ResetStockLinearTexgen();
                    m_frameRenderer->SetStockShadowTransforms(nullptr, 0u);
                    m_frameRenderer->SetStockFog(false, m_fogDensity, m_fogStart, m_fogEnd, m_fogColor, m_fogMode);
                    if (CRenderer::CV_r_scissor && m_RP.m_pCurObject)
                    {
                        CCObject* object = m_RP.m_pCurObject;
                        if (object->m_nScissorX2)
                            SetScissor(object->m_nScissorX1, object->m_nScissorY1,
                                object->m_nScissorX2 - object->m_nScissorX1,
                                object->m_nScissorY2 - object->m_nScissorY1);
                        else SetScissor(0, 0, 0, 0);
                    }
                    const bool queued = m_frameRenderer->QueueStockClientIndexedDraw(
                        finalVertexData, finalVertexCount, indexData, drawIndexCount, finalVertexFormat, topology,
                        static_cast<uint32_t>(m_CurState), ResolveStockCullMode(), unit->m_TexPic->GetTextureID(), 0,
                        eCO_REPLACE, eCO_REPLACE, eCO_NOSET, eCO_NOSET,
                        DEF_TEXARG0, DEF_TEXARG0, 0xffffffffu, DEF_TEXARG1, DEF_TEXARG1, 0xffffffffu,
                        static_cast<uint32_t>(m_CurStencilState), m_CurStencRef, m_CurStencMask,
                        modelView, identity, identity, noLighting, false, 1.0f,
                        (m_RP.m_FlagsPerFlush & RBSI_ALPHATEST) ? alphaTestRef : 0.0f,
                        nullptr, 0, color, mask, colorWriteMaskOverride, m_stageLodBias[0], 0.0f,
                        nullptr, nullptr, false, -1.0f, -4.0f, clipPlane,
                        TextureWrapModeForId(unit->m_TexPic->GetTextureID()), -1, -1, -1,
                        nullptr, nullptr, false, false, false, (m_RP.m_ObjFlags & FOB_NEAREST) != 0,
                        false, 0.0f, false, false, parameters);
                    if (!queued) m_frameRenderer->RequirePanelFallback();
                }
                else m_frameRenderer->RequirePanelFallback();
            }
            const float originalDistance = volume->m_Dist;
            if (m_RP.m_pShader->m_eSort == eS_Water) volume->m_Dist += 0.1f;
            CryVR::VulkanStockLinearTexgen enterTexgen, fogTexgen;
            BuildStockFogTexgen(enterTexgen, true);
            BuildStockFogTexgen(fogTexgen, false);
            volume->m_Dist = originalDistance;
            m_frameRenderer->ResetStockLinearTexgen();
            m_frameRenderer->SetStockLinearTexgen(0, enterTexgen);
            m_frameRenderer->SetStockLinearTexgen(1, fogTexgen);
            m_frameRenderer->SetStockShadowTransforms(nullptr, 0u);
            float fogColor[4] = { volume->m_Color.r, volume->m_Color.g, volume->m_Color.b, 1.0f };
            const float colorMask[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            float noLighting[19]{};
            uint32_t fogState = GS_BLSRC_SRCALPHA | GS_BLDST_ONEMINUSSRCALPHA;
            if (m_RP.m_pShader->m_Flags2 & EF2_OPAQUE) fogState |= GS_DEPTHFUNC_EQUAL;
            EF_SetState(static_cast<int>(fogState));
            fogState = static_cast<uint32_t>(m_CurState);
            // General fog passes reinstall the object's scissor. Detail
            // disables it, and light passes may leave a light rectangle.
            if (CRenderer::CV_r_scissor)
            {
                CCObject* object = m_RP.m_pCurObject;
                if (object && object->m_nScissorX2)
                    SetScissor(object->m_nScissorX1, object->m_nScissorY1,
                        object->m_nScissorX2 - object->m_nScissorX1,
                        object->m_nScissorY2 - object->m_nScissorY1);
                else
                    SetScissor(0, 0, 0, 0);
            }
            float identity[16];
            SetIdentityScreenMatrix(identity);
            // CGRCFog is explicitly NoFog. Ordinary scene fog must not be
            // applied again to the independently blended volume overlay.
            m_frameRenderer->SetStockFog(false, m_fogDensity, m_fogStart, m_fogEnd, m_fogColor, m_fogMode);
            const uint32_t textureDiffuse = eCA_Texture | (eCA_Diffuse << 3);
            const bool fogQueued = m_frameRenderer->QueueStockClientIndexedDraw(
                finalVertexData, finalVertexCount, indexData, drawIndexCount,
                finalVertexFormat, topology, fogState, ResolveStockCullMode(),
                m_TexMan->m_Text_Fog_Enter->GetTextureID(), m_TexMan->m_Text_Fog->GetTextureID(),
                eCO_MODULATE, eCO_REPLACE, eCO_MODULATE, eCO_MODULATE,
                textureDiffuse, eCA_Texture, 0xffffffffu, DEF_TEXARG1, DEF_TEXARG1, 0xffffffffu,
                static_cast<uint32_t>(m_CurStencilState), m_CurStencRef, m_CurStencMask,
                modelView, identity, identity, noLighting, false, 1.0f,
                (m_RP.m_FlagsPerFlush & RBSI_ALPHATEST) ? alphaTestRef : 0.0f,
                nullptr, 0, fogColor, colorMask, colorWriteMaskOverride,
                m_stageLodBias[0], m_stageLodBias[1], nullptr, nullptr,
                false, -1.0f, -4.0f, clipPlane, 1, 1, -1, -1, nullptr, nullptr,
                false, false, false, (m_RP.m_ObjFlags & FOB_NEAREST) != 0);
            m_frameRenderer->ResetStockLinearTexgen();
            UpdateVulkanFog();
            if (!fogQueued || (m_RP.m_PersFlags & RBPF_HDR))
                m_frameRenderer->RequirePanelFallback();
        }
    }

private:
    bool BuildStockFogTexgen(CryVR::VulkanStockLinearTexgen& texgen, bool enter)
    {
        if (!m_RP.m_pFogVolume) return false;
        texgen = {};
        for (int component = 0; component < 2; ++component)
        {
            if (enter)
            {
                SParamComp_FogEnterMatrix parameter;
                parameter.m_Offs = component;
                parameter.mfGet4f(texgen.planes[component]);
            }
            else
            {
                SParamComp_FogMatrix parameter;
                parameter.m_Offs = component;
                parameter.mfGet4f(texgen.planes[component]);
            }
        }
        texgen.componentMask = 3u;
        texgen.enabled = true;
        return true;
    }

    void InitializeStockFogTextures()
    {
        if (!m_frameRenderer || !m_TexMan) return;
        const uint flags = FT_CLAMP | FT_NOMIPS | FT_NOREMOVE | FT_HASALPHA;
        if (!m_TexMan->m_Text_Fog ||
            !m_frameRenderer->HasLegacyTexture(m_TexMan->m_Text_Fog->GetTextureID()))
        {
            // CGLTexMan::GenerateFogMaps, including byte truncation and
            // the forced opaque texture border.
            float attenuation[256];
            float value = 1.0f;
            for (int index = 0; index < 256; ++index)
            {
                attenuation[index] = value;
                value *= 0.982f;
            }
            attenuation[0] = attenuation[255] = 0.0f;
            byte pixels[128][128][4];
            for (int x = 0; x < 128; ++x)
                for (int y = 0; y < 128; ++y)
                {
                    const int dx = x - 64, dy = y - 64;
                    const float radiusSquared = static_cast<float>(dx * dx + dy * dy);
                    const float inverseRadius = radiusSquared ?
                        1.0f / cry_sqrtf(radiusSquared) : 1000000.0f;
                    const int sample = clamp_tpl(static_cast<int>(
                        inverseRadius * radiusSquared / 63.0f * 255.0f), 0, 255);
                    int alpha = static_cast<int>((1.0f - attenuation[sample]) * 255.0f);
                    if (!x || x == 127 || !y || y == 127) alpha = 255;
                    pixels[y][x][0] = pixels[y][x][1] = pixels[y][x][2] = 255;
                    pixels[y][x][3] = static_cast<byte>(alpha);
                }
            m_TexMan->m_Text_Fog = m_TexMan->CreateTexture("$Fog", 128, 128, 1,
                flags, FT2_NODXT, &pixels[0][0][0], eTT_Base,
                -1.0f, -1.0f, 0, m_TexMan->m_Text_Fog, 0, eTF_8888);
        }
        if (!m_TexMan->m_Text_Fog_Enter)
            m_TexMan->m_Text_Fog_Enter = static_cast<STexPic*>(
                EF_LoadTexture("Textures/FogEnter", flags, FT2_NODXT, eTT_Base));
    }

    bool IsGlobalOnlyColorPass(const SShaderPass& pass) const
    {
        if (!m_RP.m_pRE) return false;
        switch (pass.m_eEvalRGB)
        {
        case eERGB_NoFill: case eERGB_FromClient: case eERGB_Identity:
        case eERGB_Fixed: case eERGB_Object: case eERGB_OneMinusObject:
        case eERGB_RE: case eERGB_OneMinusRE: case eERGB_World:
        case eERGB_Wave: case eERGB_Noise: case eERGB_Comps:
        case eERGB_StyleColor: case eERGB_StyleIntens:
            break;
        default: return false;
        }
        switch (pass.m_eEvalAlpha)
        {
        case eEALPHA_NoFill: case eEALPHA_FromClient: case eEALPHA_Identity: case eEALPHA_Fixed:
        case eEALPHA_Object: case eEALPHA_OneMinusObject:
        case eEALPHA_RE: case eEALPHA_OneMinusRE: case eEALPHA_World:
        case eEALPHA_Wave: case eEALPHA_Noise: case eEALPHA_Comps:
        case eEALPHA_Style:
            return true;
        default: return false;
        }
    }

    uint32_t CurrentPanelBlendState() const
    {
        return static_cast<uint32_t>(m_CurState) &
            (GS_BLEND_MASK | GS_NOCOLMASK | GS_COLMASKONLYALPHA | GS_COLMASKONLYRGB);
    }

    bool CullGeometryForLightsEnabled()
    {
        if (!m_cullGeometryForLightsCVar && iConsole)
            m_cullGeometryForLightsCVar = iConsole->GetCVar("r_CullGeometryForLights");
        return m_cullGeometryForLightsCVar && m_cullGeometryForLightsCVar->GetIVal() != 0;
    }

    int TextureWrapModeForId(int textureId) const
    {
        const auto found = m_textureWrapOverrides.find(textureId);
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
            // EF_Eval_RGBAGen initializes UCol to 0xffffffff and the
            // identity branch installs the complete color, including alpha.
            color[0] = color[1] = color[2] = color[3] = 1.0f;
            mask[0] = mask[1] = mask[2] = mask[3] = 1.0f;
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
                UCol packed;
                packed.dcolor = style->m_Color.GetTrue();
                for (int channel = 0; channel < 4; ++channel)
                    color[channel] = packed.bcolor[channel] * invByte;
                mask[0] = mask[1] = mask[2] = mask[3] = 1.0f;
            }
            break;
        case eERGB_StyleIntens:
            if (CLightStyle* style = CLightStyle::mfGetStyle(pass.m_Style, m_RP.m_RealTime))
            {
                for (int channel = 0; channel < 3; ++channel)
                    color[channel] = static_cast<byte>(
                        static_cast<float>(pass.m_FixedColor.bcolor[channel]) * style->m_fIntensity) * invByte;
                // GL scales only RGB of a copy of m_FixedColor.
                color[3] = pass.m_FixedColor.bcolor[3] * invByte;
                mask[0] = mask[1] = mask[2] = mask[3] = 1.0f;
            }
            break;
        default:
            // NoFill, identity and client color generation keep the vertex
            // color stream already provided by the stock mesh.
            break;
        }

        // EF_Eval_RGBAGen starts its packed color at 0xffffffff and writes
        // the whole value when a global RGB generator succeeds. Generators
        // that write only RGB therefore supply alpha 255, not vertex alpha.
        // A subsequent alpha generator may replace it below.
        if (mask[0] > 0.5f && mask[1] > 0.5f && mask[2] > 0.5f)
            mask[3] = 1.0f;

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
                color[3] = static_cast<byte>(
                    static_cast<float>(pass.m_FixedColor.bcolor[3]) * style->m_fIntensity) * invByte;
                mask[3] = 1.0f;
            }
            break;
        default:
            break;
        }
        // bSetCol in EF_Eval_RGBAGen installs the entire local UCol even
        // when only an alpha generator wrote it. Unwritten RGB channels
        // retain its initial white value rather than the mesh's vertex RGB.
        if (mask[0] > 0.5f || mask[1] > 0.5f || mask[2] > 0.5f || mask[3] > 0.5f)
            mask[0] = mask[1] = mask[2] = mask[3] = 1.0f;
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

    void UpdateStockNearestCamera(bool nearest)
    {
        if (nearest == bool(m_RP.m_Flags & RBF_NEAREST)) return;
        if (nearest)
        {
            CCamera camera = GetCamera();
            m_RP.m_PrevCamera = camera;
            camera.SetZMin(0.01f);
            camera.SetZMax(40.0f);
            // fholger's VRRenderer::Hook_Renderer_SetCamera restores the
            // world FOV for DRAW_NEAR. The XR eye projection must likewise
            // preserve physical size and controller movement in metres.
            camera.Update();
            SetCamera(camera);
            m_RP.m_Flags |= RBF_NEAREST;
        }
        else
        {
            SetCamera(m_RP.m_PrevCamera);
            m_RP.m_Flags &= ~RBF_NEAREST;
        }
        if (m_frameRenderer)
            m_frameRenderer->SetStockDepthRange(nearest ? 0.0f : m_RP.m_fMinDepthRange,
                                               nearest ? 0.1f : m_RP.m_fMaxDepthRange);
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
    int ProcessStockPreprocess(int list, int first, int last)
    {
        struct Operation
        {
            int id;
            int object;
            SShader* shader;
            CRendElement* element;
            SRenderShaderResources* resources;
            int sourceIndex;
        };
        std::vector<Operation> operations;
        int consumed = 0;
        for (int index = first; index < last; ++index)
        {
            SRendItemPre& item = SRendItem::m_RendItems[list][index];
            if ((item.SortVal.i.High >> 26) != eS_PreProcess) break;
            ++consumed;
            int object = -1, fog = 0;
            SShader* shader = nullptr;
            SShader* state = nullptr;
            SRenderShaderResources* resources = nullptr;
            SRendItem::mfGet(item.SortVal, &object, &shader, &state, &fog, &resources);
            if (!shader) continue;
            for (int id = 0; id < 32; ++id)
            {
                const uint32_t mask = uint32_t(1) << id;
                if (mask >= FSPR_MAX || mask > shader->m_nPreprocess) break;
                if (shader->m_nPreprocess & mask)
                    operations.push_back(Operation{ id, object, shader, item.Item, resources, index });
            }
        }
        // OpenGL returns zero for an empty operation list, not consumed.
        if (operations.empty()) return 0;
        std::stable_sort(operations.begin(), operations.end(),
            [](const Operation& a, const Operation& b) { return a.id < b.id; });
        if (m_collectingShadowCasters || CV_r_nopreprocess) return consumed;
        CCObject* savedObject = m_RP.m_pCurObject;
        for (const Operation& operation : operations)
        {
            CCObject* object = operation.object >= 0 && operation.object < m_RP.m_NumVisObjects ?
                m_RP.m_VisObjects[operation.object] : nullptr;
            if (operation.id == SPRID_SCANCM || operation.id == SPRID_SCANLCM)
            {
                // Match the source eligibility before accounting for the
                // missing six-face environment target/render implementation.
                if (m_RP.m_bDrawToTexture || !object ||
                    IsEquivalent(object->GetTranslation(), Vec3d(0, 0, 0)))
                    continue;
                if (operation.id == SPRID_SCANLCM &&
                    (!(object->m_ObjFlags & FOB_ENVLIGHTING) || !CV_r_envlighting))
                    continue;
                if (m_frameRenderer) m_frameRenderer->RequirePanelFallback();
            }
            else if (operation.id == SPRID_REFRACTED)
            {
                if (!m_RP.m_bDrawToTexture && object) object->m_ObjFlags |= FOB_REFRACTED;
            }
            else if (operation.id == SPRID_SHADOWMAPGEN)
            {
                m_RP.m_pCurObject = object;
                if (!m_RP.m_bDrawToTexture && object && object->m_pShadowCasters &&
                    operation.element && (operation.element->mfGetType() == eDATA_OcLeaf ||
                    operation.element->mfGetType() == eDATA_ShadowMapGen))
                {
                    auto* casters = static_cast<list2<ShadowMapLightSourceInstance>*>(object->m_pShadowCasters);
                    for (int index = 0; index < casters->Count(); ++index)
                    {
                        ShadowMapLightSourceInstance& caster = casters->GetAt(index);
                        if (caster.m_pLS && caster.m_pLS->GetShadowMapFrustum())
                            PrepareDepthMap(caster.m_pLS->GetShadowMapFrustum(), false);
                        m_RP.m_pCurObject = object;
                    }
                }
            }
            else if (m_frameRenderer)
            {
                // Keep every unimplemented operation visible to the existing
                // unsupported-work counter instead of silently dropping it.
                m_frameRenderer->RequirePanelFallback();
            }
        }
        m_RP.m_pCurObject = savedObject;
        return consumed;
    }

    void ClearStockSceneBuffers(bool force, bool onlyDepth)
    {
        if (m_bWasCleared && !force) return;
        EF_SetState(GS_DEPTHWRITE);
        float rgba[4] = { m_vClearColor.x, m_vClearColor.y, m_vClearColor.z, 0.0f };
        if (m_bHeatVision) rgba[0] = rgba[1] = rgba[2] = 0.0f;
        else if (m_polygonMode == R_WIREFRAME_MODE)
        {
            rgba[0] = 0.25f; rgba[1] = 0.5f; rgba[2] = 1.0f;
        }
        if (m_frameRenderer && m_frameRenderer->QueueStockClear(!onlyDepth, true,
                m_sbpp && m_cbpp > 16, rgba))
            m_bWasCleared = true;
        else if (m_frameRenderer)
            m_frameRenderer->RequirePanelFallback();
    }

    void PrepareStockFrameState()
    {
        m_RP.m_RenderFrame++;
        m_RP.m_Flags = 0;
        m_RP.m_pPrevObject = nullptr;
        m_RP.m_FrameObject++;
        // GL reads the camera modelview at this boundary. Object transforms
        // belong to EF_ObjectChange and must not survive list preparation.
        m_ViewMatrix = m_CameraMatrix;
        UpdateLegacyCameraInfo();

        // Light registration uses the current recursion level; render-item
        // ranges use level - 1. These indices are intentionally different.
        const int lightLevel = SRendItem::m_RecurseLevel;
        const int lightLevelCount = sizeof(m_RP.m_DLights) / sizeof(m_RP.m_DLights[0]);
        const int lightCount = lightLevel >= 0 && lightLevel < lightLevelCount ?
            m_RP.m_DLights[lightLevel].Num() : 0;
        int lightIndex = 0;
        for (; lightIndex < lightCount; ++lightIndex)
        {
            CDLight* light = m_RP.m_DLights[lightLevel][lightIndex];
            if (!light || (light->m_Flags & DLF_FAKE) || !(light->m_Flags & DLF_SUN))
                continue;
            Vec3d direction = light->m_Origin - m_cam.GetPos();
            direction.Normalize();
            m_RP.m_SunDir = direction;
        }
        if (lightIndex == lightCount && IsEquivalent(m_RP.m_SunDir, Vec3d(0, 0, 0)))
            m_RP.m_SunDir = Vec3d(0, 0, 1);
    }

    void PrepareStockFlushState(CCObject* object, SShader* shader)
    {
        // EF_Start has already built the active light list and selected the
        // technique. EF_Flush then filters only the dynamic-light mask; it
        // does not rebuild that list or reselect the technique.
        uint32_t mask = CV_r_hwlights && object ? object->m_DynLMMask : 0u;
        if (!kVulkanDynamicLightingEnabled)
        {
            const int level = SRendItem::m_RecurseLevel;
            const int levels = sizeof(m_RP.m_DLights) / sizeof(m_RP.m_DLights[0]);
            if (level >= 0 && level < levels)
                for (int index = 0; index < m_RP.m_DLights[level].Num() && index < 32; ++index)
                    if (!ShouldRenderVulkanLight(m_RP.m_DLights[level][index]))
                        mask &= ~(uint32_t(1) << index);
        }
        const char* filter = CV_r_showlight ? CV_r_showlight->GetString() : nullptr;
        if (filter && filter[0] != '0')
        {
            const int level = SRendItem::m_RecurseLevel;
            const int levels = sizeof(m_RP.m_DLights) / sizeof(m_RP.m_DLights[0]);
            if (level >= 0 && level < levels)
            {
                for (int index = 0; index < m_RP.m_DLights[level].Num() && index < 32; ++index)
                {
                    CDLight* light = m_RP.m_DLights[level][index];
                    if (light && (!light->m_Name || !strstr(light->m_Name, filter)))
                        mask &= ~(uint32_t(1) << index);
                }
            }
        }
        m_RP.m_DynLMask = mask;
        // EF_Flush owns material clip planes. An existing capture plane is
        // external state and must not be replaced by a material water plane.
        if (shader && (shader->m_Flags3 & EF3_CLIPPLANE) &&
            !(m_RP.m_PersFlags & RBPF_SETCLIPPLANE) && !m_RP.m_ClipPlaneEnabled)
        {
            const uint32_t mode = shader->m_Flags3 & EF3_CLIPPLANE;
            if ((mode == EF3_CLIPPLANE_WATER_FRONT || mode == EF3_CLIPPLANE_WATER_BACK) &&
                iSystem && iSystem->GetI3DEngine())
            {
                const bool back = (mode & EF3_CLIPPLANE_WATER_BACK) != 0;
                const float height = iSystem->GetI3DEngine()->GetWaterLevel();
                float plane[4] = { 0.0f, 0.0f, back ? -1.0f : 1.0f,
                    back ? height : -height };
                EF_SetClipPlane(true, plane, false);
                m_RP.m_PersFlags |= RBPF_SETCLIPPLANE;
            }
        }
        else if (shader && !(shader->m_Flags3 & EF3_CLIPPLANE) &&
                 (m_RP.m_PersFlags & RBPF_SETCLIPPLANE))
        {
            EF_SetClipPlane(false, nullptr, false);
            m_RP.m_PersFlags &= ~RBPF_SETCLIPPLANE;
        }
    }

    int ResolveStockCullMode() const
    {
        if (m_RP.m_Flags & RBF_2D) return R_CULL_NONE;
        int cull = m_activePass ? m_activeCull : m_currentCullMode;
        if ((m_RP.m_PersFlags & RBPF_DRAWMIRROR) && cull != R_CULL_NONE)
            cull = cull == R_CULL_BACK ? R_CULL_FRONT : R_CULL_BACK;
        return cull;
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
        // Apply the exact EF_Flush selection before any material pass is queued.
        if (shader)
        {
            const char* name = shader->m_Name.c_str();
            if (m_RP.m_ExcludeShader &&
                (!strcmp(m_RP.m_ExcludeShader, name) ||
                 (m_RP.m_ExcludeShader[0] == '#' && strstr(name, m_RP.m_ExcludeShader))))
                return;
            if (m_RP.m_ShowOnlyShader && strcmp(m_RP.m_ShowOnlyShader, name))
                return;
        }
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
        // EF_FlushHW/EF_DrawGeneralPasses give the object's explicit state
        // priority over the shader pass. Particle emitters use this to choose
        // additive/color-based blending for RGB sprites without an alpha map.
        // Losing it replaces their black background with an opaque square.
        if (hardwareTechniquePass && m_RP.m_pCurObject &&
            m_RP.m_pCurObject->m_RenderState &&
            FastAsciiCaseCompare(static_cast<SShaderPassHW*>(pass)->m_StockFragmentProgram, "CGRCFog"))
            state = m_RP.m_pCurObject->m_RenderState | (state & GS_STENCIL);
        // EF_Flush applies resource overrides before state-shader overrides.
        // A state shader can therefore select front culling even when the
        // underlying material resource is two-sided.
        if (resources && shader && !(shader->m_Flags2 & EF2_IGNORERESOURCESTATES) &&
            (resources->m_ResFlags & MTLFLAG_2SIDED))
        {
            cull = R_CULL_NONE;
            m_RP.m_FlagsPerFlush |= RBSI_NOCULL;
        }
        if (resources && shader && !(shader->m_Flags2 & EF2_IGNORERESOURCESTATES) &&
            resources->m_AlphaRef)
        {
            // EF_SetResourcesState protects the material GL_GEQUAL test
            // before pass state and render-element state are evaluated.
            // Its actual threshold is carried separately to the fragment
            // shader; pass alpha-test bits remain only the cached GL state.
            m_RP.m_FlagsPerFlush |= RBSI_ALPHATEST;
        }
        if (m_activeStateShaderState)
        {
            const SEfState& stateShader = *m_activeStateShaderState;
            if (stateShader.m_Flags & ESF_POLYLINE)
                state |= GS_POLYLINE;
            if (stateShader.m_Flags & ESF_NOCULL)
            {
                cull = R_CULL_NONE;
                m_RP.m_FlagsPerFlush |= RBSI_NOCULL;
            }
            else if (stateShader.m_Flags & ESF_CULLFRONT)
            {
                cull = R_CULL_FRONT;
                m_RP.m_FlagsPerFlush |= RBSI_NOCULL;
            }
            if (stateShader.m_Flags & ESF_STATE)
            {
                if (stateShader.m_State & GS_NODEPTHTEST)
                {
                    state |= GS_NODEPTHTEST;
                    // Install the override before protecting it: EF_SetState
                    // otherwise preserves the previous cached depth state.
                    m_CurState |= GS_NODEPTHTEST;
                    m_RP.m_FlagsPerFlush |= RBSI_DEPTHTEST;
                }
                const uint32_t depthFuncMask = GS_DEPTHFUNC_EQUAL | GS_DEPTHFUNC_GREAT;
                if (stateShader.m_State & depthFuncMask)
                {
                    state = (state & ~depthFuncMask) | (stateShader.m_State & depthFuncMask);
                    m_CurState = (m_CurState & ~depthFuncMask) |
                        (stateShader.m_State & depthFuncMask);
                    m_RP.m_FlagsPerFlush |= RBSI_DEPTHFUNC;
                }
            }
        }
        EF_SetState(static_cast<int>(state));
        memset(m_stageStateOverrides, 0, sizeof(m_stageStateOverrides));
        if (m_frameRenderer) m_frameRenderer->ResetStockLinearTexgen();
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
        // EF_FlushHW installs the current light material before evaluating
        // shader components. Reset it per draw so RGB/alpha generators cannot
        // observe the material of a previous mesh or camera pass.
        m_RP.m_pCurLightMaterial = resources ? resources->m_LMaterial :
            (shader && (shader->m_Flags2 & EF2_USELIGHTMATERIAL) ?
                &m_RP.m_DefLightMaterial : nullptr);
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
        m_activePass = pass;
        m_activeResources = resources;
        if (!hardwareTechniquePass && pass)
        {
            // SShaderPass::mfSetTextures ends with BindNULL(stageCount)
            // before mfDraw. Direct TMU-binding elements must see that same
            // boundary instead of textures retained from a previous pass.
            const int stageCount = crymin(pass->m_TUnits.Num(), 8);
            for (int stage = stageCount; stage < 8; ++stage)
                m_stageTextureIds[stage] = 0;
            if (m_TexMan) m_TexMan->m_nCurStages = stageCount;
        }
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

    void CaptureStockLeafStatus(CRendElement* renderElement, SShader* shader,
                               const char* status, uint32_t resolvedState = ~0u)
    {
#if defined(__ANDROID__) && defined(CRYVR_RENDER_PROBES)
        if (!m_frameRenderer || m_frameRenderer->GetSceneDiagnostics().queuedDraws == 0 ||
            !renderElement || renderElement->mfGetType() != eDATA_OcLeaf)
            return;
        CREOcLeaf* element = static_cast<CREOcLeaf*>(renderElement);
        CLeafBuffer* leaf = element->m_pBuffer;
        if (!leaf || !leaf->m_sSource) return;
        const char* source = leaf->m_sSource;
        SRenderShaderResources* resources = m_RP.m_pShaderResources;
        SEfResTexture* diffuse = resources ? resources->m_Textures[EFTT_DIFFUSE] : nullptr;
        const char* texture = diffuse ? diffuse->m_Name.c_str() : "?";
        std::string textureLower = texture;
        std::transform(textureLower.begin(), textureLower.end(), textureLower.begin(),
                       [](unsigned char c) { return static_cast<char>(tolower(c)); });
        // One bounded record per material/state; no continuous level-load log.
        if (!strstr(source, "ceiling") && !strstr(source, "Ceiling") &&
            !strstr(source, "moss") && !strstr(source, "Moss") &&
            textureLower.find("concr") == std::string::npos &&
            textureLower.find("rust_wall3") == std::string::npos &&
            textureLower.find("shipwall") == std::string::npos &&
            textureLower.find("support") == std::string::npos &&
            textureLower.find("moss") == std::string::npos &&
            !(shader && !FastAsciiCaseCompare(shader->GetName(), "templmuzzleflash_auto")))
            return;
        static std::vector<std::string> keys;
        if (keys.size() >= 192) return;
        char key[1024];
        const CMatInfo* chunk = element->m_pChunk;
        snprintf(key, sizeof(key), "%s|%s|%s|%s|%d+%d|%d|%x", source, texture,
            shader ? shader->GetName() : "?", status,
            chunk ? chunk->nFirstIndexId : -1, chunk ? chunk->nNumIndices : -1,
            m_activeHardwarePassType, resolvedState == ~0u ? m_CurState : resolvedState);
        if (std::find(keys.begin(), keys.end(), key) != keys.end()) return;
        keys.push_back(key);
        FILE* probe = fopen("/sdcard/FarCry/vulkan_surface_probe.txt",
                            keys.size() == 1 ? "w" : "a");
        if (!probe) return;
        const auto& diagnostics = m_frameRenderer->GetSceneDiagnostics();
        fprintf(probe, "%s rendPass=%d lm=%d queued=%u rejectInput=%u rejectVertex=%u rejectCombine=%u rejectState=%u pipelineFailed=%u error=%s\n",
            key, m_RP.m_RendPass, m_RP.m_pCurObject ? m_RP.m_pCurObject->m_nLMId : 0,
            diagnostics.queuedDraws, diagnostics.rejectedInput,
            diagnostics.rejectedVertexFeature, diagnostics.rejectedCombine,
            diagnostics.rejectedPipelineState, diagnostics.pipelineCreationFailed,
            m_frameRenderer->GetLastError());
        fclose(probe);
#endif
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
        {
            CaptureStockLeafStatus(element, shader, "update-rejected");
            return false;
        }

        CLeafBuffer* leafBuffer = element->m_pBuffer->GetVertexContainer();
        if (!leafBuffer || !leafBuffer->m_pVertexBuffer ||
            !leafBuffer->m_Indices.m_VData)
        {
            CaptureStockLeafStatus(element, shader, "missing-buffer");
            return false;
        }
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
        if (vertexFormat < 0)
            return;
        // EF_ObjectChange does not gate ProcessSkinning on this result:
        // mfCheckUpdate can fail because indices are not ready yet, whereas
        // skinning still needs to populate/update the animated vertex streams.
        leafElement->mfCheckUpdate(vertexFormat, updateFlags | FHF_FORANIM);

        CLeafBuffer* leafBuffer = leafElement->m_pBuffer->GetVertexContainer();
        if (!leafBuffer)
            return;
        bool forceUpdate = leafBuffer->m_UpdateFrame == GetFrameID();
        if (leafBuffer->m_pVertexBuffer && leafBuffer->m_pVertexBuffer->m_bFenceSet)
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
        m_frameRenderer->SetStockGpuSkinIdentity(shadow);
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
        m_frameRenderer->SetStockGpuSkinIdentity(nullptr);
        shadow->m_nCurrInst = -1;
        return queued;
    }

    struct StockProgramInfo
    {
        int water = 0, terrainLayers = -1, terrainOnly = 0, terrainAmbient = 0, terrainFog = -1;
        bool ambient = false, light = false, particle = false, terrainLayer = false;
        bool terrainShadow = false, plants = false, plantsBump = false, texgenOne = false;
        bool simplePlant = false, bendedPlant = false, terrainTexgen = false;
    };
    std::unordered_map<const SShaderPassHW*, StockProgramInfo> m_framePrograms;
    const SShaderPassHW* m_lastClassifiedPass = nullptr;
    const StockProgramInfo* m_lastProgramInfo = nullptr;
    const StockProgramInfo& ClassifyStockProgram(const SShaderPassHW* pass)
    {
        static const StockProgramInfo empty;
        if (!pass) return empty;
        if (pass == m_lastClassifiedPass && m_lastProgramInfo) return *m_lastProgramInfo;
        auto found = m_framePrograms.find(pass);
        if (found == m_framePrograms.end())
        {
            StockProgramInfo info;
            int stockWaterMode = 0;
            if (pass)
            {
                const char* program = pass->m_StockFragmentProgram;
                if (!FastAsciiCaseCompare(program, "CGRCLowMedWater")) stockWaterMode = 1;
                else if (!FastAsciiCaseCompare(program, "CGRCIndoorWater_final")) stockWaterMode = 2;
                else if (!FastAsciiCaseCompare(program, "CGRCIndoorWaterSpec")) stockWaterMode = 3;
                else if (!FastAsciiCaseCompare(program, "CGRCOutdoorWaterRefraction")) stockWaterMode = 4;
                else if (!FastAsciiCaseCompare(program, "CGRCIndoorWater")) stockWaterMode = 5;
                else if (!FastAsciiCaseCompare(program, "CGRCWater")) stockWaterMode = 7;
                else if (!FastAsciiCaseCompare(program, "CGRCWater_Beach_Refr")) stockWaterMode = 8;
                else if (!FastAsciiCaseCompare(program, "CGRCWater_Beach")) stockWaterMode = 9;
                else if (!FastAsciiCaseCompare(program, "CGRCOcean_NoRefl") || !FastAsciiCaseCompare(program, "CGRCOcean")) stockWaterMode = 6;
                if (!stockWaterMode &&
                    !FastAsciiCaseCompareN(pass->m_StockVertexProgram, "CGVProgWater_Beach_Shift", 25))
                    stockWaterMode = 10;
            }
            int stockTerrainLayers = -1;
            int stockTerrainOnlyLayers = 0;
            int stockTerrainAmbientMode = 0;
            int stockTerrainFogLayers = -1;
            if (pass)
            {
                const char* program = pass->m_StockFragmentProgram;
                if (!FastAsciiCaseCompare(program, "CGRCTerrain")) stockTerrainLayers = 0;
                else if (!FastAsciiCaseCompare(program, "CGRCTerrain_1Layers")) stockTerrainLayers = 1;
                else if (!FastAsciiCaseCompare(program, "CGRCTerrain_2Layers")) stockTerrainLayers = 2;
                else if (!FastAsciiCaseCompare(program, "CGRCTerrain_3Layers")) stockTerrainLayers = 3;
                else if (!FastAsciiCaseCompare(program, "CGRCTerrain_1Layers_Only")) stockTerrainOnlyLayers = 1;
                else if (!FastAsciiCaseCompare(program, "CGRCTerrain_2Layers_Only")) stockTerrainOnlyLayers = 2;
                else if (!FastAsciiCaseCompare(program, "CGRCTerrain_3Layers_Only")) stockTerrainOnlyLayers = 3;
                else if (!FastAsciiCaseCompare(program, "CGRCTerrain_4Layers_Only")) stockTerrainOnlyLayers = 4;
                else if (!FastAsciiCaseCompare(program, "CGRCTerrain_NoCol")) stockTerrainAmbientMode = 1;
                else if (!FastAsciiCaseCompare(program, "CGRCTerrain_Far")) stockTerrainAmbientMode = 2;
                else if (!FastAsciiCaseCompare(program, "CGRCTerrain_DetTex")) stockTerrainAmbientMode = 3;
                else if (!FastAsciiCaseCompare(program, "CGRCTerrain_VF")) stockTerrainAmbientMode = 4;
                else if (!FastAsciiCaseCompare(program, "CGRCTerrain_1Layers_VF")) stockTerrainFogLayers = 1;
                else if (!FastAsciiCaseCompare(program, "CGRCTerrain_2Layers_VF")) stockTerrainFogLayers = 2;
            }

            info.water = stockWaterMode;
            info.terrainLayers = stockTerrainLayers;
            info.terrainOnly = stockTerrainOnlyLayers;
            info.terrainAmbient = stockTerrainAmbientMode;
            info.terrainFog = stockTerrainFogLayers;
            info.ambient = !FastAsciiCaseCompare(pass->m_StockFragmentProgram, "CGRCAmbientTempl");
            info.light = !FastAsciiCaseCompare(pass->m_StockFragmentProgram, "CGRCLightTempl");
            info.particle = !FastAsciiCaseCompare(pass->m_StockFragmentProgram, "CGRCAmbient_Particle");
            info.terrainLayer = !FastAsciiCaseCompare(pass->m_StockFragmentProgram, "CGRCTerrainLayerTempl");
            info.terrainShadow = !FastAsciiCaseCompare(pass->m_StockFragmentProgram, "CGRCTerrainShadow");
            info.plants = !FastAsciiCaseCompare(pass->m_StockFragmentProgram, "CGRCPlants");
            info.plantsBump = !FastAsciiCaseCompare(pass->m_StockFragmentProgram, "CGRCPlants_Bump");
            info.texgenOne = !FastAsciiCaseCompare(pass->m_StockVertexProgram, "CGVProgTexGen_1Unit");
            info.simplePlant = !FastAsciiCaseCompare(pass->m_StockVertexProgram, "CGVProgSimple_Plant");
            info.bendedPlant = !FastAsciiCaseCompare(pass->m_StockVertexProgram, "CGVProgSimple_Plant_Bended");
            info.terrainTexgen = !FastAsciiCaseCompareN(pass->m_StockFragmentProgram, "CGRCTerrain", 11);
            found = m_framePrograms.emplace(pass, info).first;
        }
        m_lastClassifiedPass = pass;
        m_lastProgramInfo = &found->second;
        return *m_lastProgramInfo;
    }

    CryVR::VulkanFrameRenderer* m_frameRenderer = nullptr;
    struct NormalGeometryHash
    {
        size_t operator()(const std::array<uintptr_t, 8>& key) const
        {
            size_t hash = 1469598103934665603ull;
            for (auto value : key) hash = (hash ^ value) * 1099511628211ull;
            return hash;
        }
    };
    std::unordered_map<std::array<uintptr_t, 8>, std::vector<byte>, NormalGeometryHash> m_frameNormalGeometry;
    std::unordered_map<const CVertexBuffer*, uint64_t> m_frameBufferRevisions;
    std::unordered_map<const SVertexStream*, uint64_t> m_ownedIndexRevisions;
    struct BufferRevisionLookupEntry
    {
        const CVertexBuffer* buffer = nullptr;
        uint64_t revision = 0;
        bool valid = false;
    };
    struct IndexRevisionLookupEntry
    {
        const SVertexStream* stream = nullptr;
        uint64_t revision = 0;
        bool valid = false;
    };
    mutable std::array<BufferRevisionLookupEntry, 2048> m_frameBufferRevisionLookup{};
    mutable std::array<IndexRevisionLookupEntry, 2048> m_ownedIndexRevisionLookup{};
    uint64_t m_geometryRevision = 0;
    struct IndexRangeCacheEntry
    {
        std::array<uint64_t, 4> key{};
        uint32_t minimum = 0, maximum = 0;
    };
    std::array<IndexRangeCacheEntry, 2048> m_indexRangeCache;
    static size_t RevisionLookupSlot(const void* pointer)
    {
        return (reinterpret_cast<uintptr_t>(pointer) >> 4) & 2047u;
    }
    uint64_t FindOwnedIndexRevision(const SVertexStream* stream) const
    {
        if (!stream) return 0;
        const size_t slot = RevisionLookupSlot(stream);
        auto& cached = m_ownedIndexRevisionLookup[slot];
        if (cached.valid && cached.stream == stream) return cached.revision;
        const auto found = m_ownedIndexRevisions.find(stream);
        const uint64_t revision = found == m_ownedIndexRevisions.end() ? 0 : found->second;
        cached = {stream, revision, true};
        return revision;
    }
    uint64_t FindFrameBufferRevision(const CVertexBuffer* buffer) const
    {
        if (!buffer) return 0;
        const size_t slot = RevisionLookupSlot(buffer);
        auto& cached = m_frameBufferRevisionLookup[slot];
        if (cached.valid && cached.buffer == buffer) return cached.revision;
        const auto found = m_frameBufferRevisions.find(buffer);
        const uint64_t revision = found == m_frameBufferRevisions.end() ? 0 : found->second;
        cached = {buffer, revision, true};
        return revision;
    }
    void CacheOwnedIndexRevision(const SVertexStream* stream, uint64_t revision)
    {
        if (stream) m_ownedIndexRevisionLookup[RevisionLookupSlot(stream)] = {stream, revision, true};
    }
    void InvalidateOwnedIndexRevision(const SVertexStream* stream)
    {
        if (!stream) return;
        auto& cached = m_ownedIndexRevisionLookup[RevisionLookupSlot(stream)];
        if (cached.valid && cached.stream == stream) cached = {};
    }
    void CacheFrameBufferRevision(const CVertexBuffer* buffer, uint64_t revision)
    {
        if (buffer) m_frameBufferRevisionLookup[RevisionLookupSlot(buffer)] = {buffer, revision, true};
    }
    void InvalidateFrameBufferRevision(const CVertexBuffer* buffer)
    {
        if (!buffer) return;
        auto& cached = m_frameBufferRevisionLookup[RevisionLookupSlot(buffer)];
        if (cached.valid && cached.buffer == buffer) cached = {};
    }
    void ValidatedIndexRange(SVertexStream* stream, const uint16_t* data,
        uint32_t count, uint32_t vertexCount, uint32_t& minimum, uint32_t& maximum)
    {
        const uint64_t revision = FindOwnedIndexRevision(stream);
        IndexRangeCacheEntry* entry = nullptr;
        std::array<uint64_t, 4> key{};
        if (revision != 0)
        {
            key = {{reinterpret_cast<uint64_t>(data), revision, count, vertexCount}};
            const size_t slot = ((key[0] >> 4) ^ (key[1] * 1099511628211ull) ^
                (key[2] * 31u) ^ key[3]) & (m_indexRangeCache.size() - 1);
            entry = &m_indexRangeCache[slot];
            if (entry->key == key)
            {
                minimum = entry->minimum;
                maximum = entry->maximum;
                return;
            }
        }
        minimum = vertexCount;
        maximum = 0;
        for (uint32_t index = 0; index < count; ++index)
        {
            minimum = crymin(minimum, uint32_t(data[index]));
            maximum = crymax(maximum, uint32_t(data[index]));
        }
        if (entry)
        {
            entry->key = key;
            entry->minimum = minimum;
            entry->maximum = maximum;
        }
    }
    uint64_t ImmutableGeometryRevision(CVertexBuffer* vertices, SVertexStream* indices,
        const void* data, const void* cachedNormals, const uint16_t* indexData,
        int firstIndex) const
    {
        if (!vertices || !indices || vertices->m_bDynamic ||
            (data != vertices->m_VS[VSF_GENERAL].m_VData && (!cachedNormals || data != cachedNormals)) ||
            (m_RP.m_pCurObject && m_RP.m_pCurObject->m_pCharInstance) ||
            indexData != static_cast<const uint16_t*>(indices->m_VData) + firstIndex)
            return 0;
        const uint64_t indexRevision = FindOwnedIndexRevision(indices);
        if (!indexRevision) return 0;
        return std::max(indexRevision, FindFrameBufferRevision(vertices));
    }
    size_t m_frameNormalGeometryBytes = 0;
    const void* CacheFrameNormalGeometry(CVertexBuffer* vertices, const void* source,
        uint32_t count, int sourceId, int targetId, const byte* normals, int normalStride)
    {
        if (!vertices || vertices->m_bDynamic || !normals || normalStride <= 0 ||
            source != vertices->m_VS[VSF_GENERAL].m_VData ||
            (m_RP.m_pCurObject && m_RP.m_pCurObject->m_pCharInstance)) return nullptr;
        CryVR::VulkanVertexFormat input{}, output{};
        if (!CryVR::GetVulkanVertexFormat(sourceId, input) ||
            !CryVR::GetVulkanVertexFormat(targetId, output)) return nullptr;
        const uint64_t revision = FindFrameBufferRevision(vertices);
        const std::array<uintptr_t, 8> key{{reinterpret_cast<uintptr_t>(vertices),
            reinterpret_cast<uintptr_t>(source), reinterpret_cast<uintptr_t>(normals),
            uintptr_t(sourceId), uintptr_t(targetId), count, uintptr_t(normalStride),
            uintptr_t(revision)}};
        const auto found = m_frameNormalGeometry.find(key);
        if (found != m_frameNormalGeometry.end()) return found->second.data();
        const size_t bytes = size_t(count) * output.stride;
        // Static attribute conversion survives frames. Buffer revisions reject
        // updates and ReleaseBuffer removes entries before addresses are reused.
        // Bound copies so unusually large scenes fall back to the scratch path.
        if (bytes > 32u*1024u*1024u - m_frameNormalGeometryBytes) return nullptr;
        auto& data = m_frameNormalGeometry[key];
        data.resize(bytes);
        for (uint32_t vertex = 0; vertex < count; ++vertex)
        {
            byte* dst = data.data() + size_t(vertex) * output.stride;
            const byte* src = static_cast<const byte*>(source) + size_t(vertex) * input.stride;
            if (sourceId == targetId) std::memcpy(dst, src, input.stride);
            for (uint32_t d = 0; d < output.attributeCount; ++d)
            {
                const auto& target = output.attributes[d];
                if (target.location == 1)
                {
                    std::memcpy(dst + target.offset, normals + size_t(vertex) * normalStride, 12);
                    continue;
                }
                if (sourceId == targetId) continue;
                for (uint32_t s = 0; s < input.attributeCount; ++s)
                {
                    const auto& original = input.attributes[s];
                    if (original.location != target.location || original.format != target.format) continue;
                    const size_t size = original.format == VK_FORMAT_R32G32B32_SFLOAT ? 12 :
                        original.format == VK_FORMAT_R32G32_SFLOAT ? 8 : 4;
                    std::memcpy(dst + target.offset, src + original.offset, size);
                    break;
                }
            }
        }
        m_frameNormalGeometryBytes += bytes;
        return data.data();
    }
    CVulkanTexMan* m_vulkanTexMan = nullptr;
    SShaderPass* m_activePass = nullptr;
    struct FailedBumpTextureLoad
    {
        std::string name;
        std::string path;
    };
    std::unordered_map<const SEfResTexture*, FailedBumpTextureLoad> m_failedBumpTextureLoads;
    struct StockGeneratedColors
    {
        int vertexFormat = -1;
        std::vector<uint8_t> colors;
    };
    std::map<const CVertexBuffer*, StockGeneratedColors> m_stockGeneratedColors;
    struct LightmapCoordinateStream
    {
        // Separate LM streams already have the exact Vulkan float2 layout.
        // Borrow until QueueStockClientIndexedDraw copies the referenced range.
        // The owned array is needed only for padding or strided fallback UVs.
        std::vector<float> owned;
        float* borrowed = nullptr;
        size_t borrowedCount = 0;
        void Borrow(float* source, size_t count) { borrowed = source; borrowedCount = count; }
        void clear() { borrowed = nullptr; borrowedCount = 0; owned.clear(); }
        void resize(size_t count) { borrowed = nullptr; borrowedCount = 0; owned.resize(count); }
        size_t size() const { return borrowed ? borrowedCount : owned.size(); }
        bool empty() const { return size() == 0; }
        float* data() { return borrowed ? borrowed : owned.data(); }
        const float* data() const { return borrowed ? borrowed : owned.data(); }
        float& operator[](size_t index) { return data()[index]; }
        const float& operator[](size_t index) const { return data()[index]; }
    };
    struct DrawScratch
    {
        std::vector<uint16_t> triangulatedQuads;
        std::vector<byte> waterDeformedVertices;
        std::vector<byte> waterColorVertices;
        std::vector<struct_VERTEX_FORMAT_P3F_TEX2F> hardwareTexturedVertices;
        std::vector<struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F> terrainLowResTexturedVertices;
        std::vector<struct_VERTEX_FORMAT_P3F_COL4UB_COL4UB_TEX2F> terrainFogLayeredTexturedVertices;
        std::vector<struct_VERTEX_FORMAT_P3F_N_COL4UB_COL4UB_TEX2F> terrainTexturedVertices;
        std::vector<struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F> muzzleFlashVertices;
        std::vector<uint8_t> generatedVertices;
        LightmapCoordinateStream lightmapCoordinates;
        std::vector<byte> separateUvVertices;
        std::vector<byte> litVertices;
    };
    std::deque<DrawScratch> m_drawScratch;
    size_t m_drawScratchDepth = 0;
    int m_stockColorCacheFrame = -1;
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
    bool m_collectingShadowCasters = false;
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
    std::string m_lastTextureFilter;
    int m_lastTextureAnisotropy = -1;
    std::unordered_map<int, int> m_textureWrapOverrides;
    bool m_forceCurrentTextureForClientDraw = false;
    int m_flatLightmapNormalTextureId = 0;
    float m_screenSpaceWidth = 800.0f;
    float m_screenSpaceHeight = 600.0f;
    int m_stageTextureIds[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    float m_stageLodBias[8]{};
    std::vector<const SDeform*> m_stockDeformScratch;
    std::vector<float> m_detailFogCoordinatesScratch;
    std::vector<std::array<float, 19>> m_lightPassScratch;
    std::vector<CDLight*> m_translatedLightScratch;
    std::vector<std::array<int, 2>> m_lightPassSpecularOcclusionScratch;
    std::vector<bool> m_ambientOnlyLightPassScratch;
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
    FILE* m_sceneTrace = nullptr;
    unsigned m_sceneTraceLines = 0;
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
