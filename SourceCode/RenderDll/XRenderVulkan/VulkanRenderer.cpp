#include "RenderPCH.h"
#include "../XRenderNULL/NULL_Renderer.h"
#include "VulkanFrameRenderer.h"
#include "VulkanTextureDecode.h"
#include "../../CryCommon/CryHeaders.h"
#include "../../CryCommon/CRETriMeshShadow.h"
#include "../../CryCommon/LeafBuffer.h"
#include "../Common/NvTriStrip/NvTriStrip.h"
#include <array>
#include <vector>

namespace
{
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

protected:
    STexPic* CreateTexture(const char* name, int width, int height, int depth,
                           uint flags, uint flags2, byte* pixels, ETexType textureType,
                           float amount1, float amount2, int dxtSize, STexPic* texture,
                           int bind, ETEX_Format format, const char* sourceName) override
    {
        // Normal and DSDT bump maps use the same sampled 2D image interface as
        // diffuse textures in the translated Vulkan materials.
        if (width <= 0 || height <= 0 ||
            (textureType != eTT_Base && textureType != eTT_Bumpmap &&
             textureType != eTT_DSDTBump))
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
        texture->m_TargetType = 0x0DE1u; // Vulkan texture IDs use the stock 2D-texture namespace.
        texture->m_ETF = format;
        texture->m_nMips = (flags & FT_NOMIPS) ? 1 : texture->m_nMips;
        texture->m_DXTSize = dxtSize;
        texture->m_fAmount1 = amount1;
        texture->m_fAmount2 = amount2;
        if (sourceName)
            texture->m_SourceName = sourceName;
        if (texture->m_Bind == 0)
            texture->m_Bind = TX_FIRSTBIND + texture->m_Id;

        if (!pixels || !m_callbacks.mirrorRgbaTexture)
            return texture;

        int filterMode = m_filterOverride;
        if (filterMode < 0)
        {
            switch (flags2 & FT2_FILTER)
            {
            case FT2_FILTER_NEAREST: filterMode = eVTF_Nearest; break;
            case FT2_FILTER_BILINEAR: filterMode = eVTF_Bilinear; break;
            case FT2_FILTER_TRILINEAR:
            case FT2_FILTER_ANISOTROPIC: filterMode = eVTF_Trilinear; break;
            default: filterMode = eVTF_Trilinear; break;
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

        const bool mirrored = m_callbacks.mirrorRgbaTexture(
            m_callbacks.drawUserData, static_cast<int>(texture->m_Bind),
            static_cast<unsigned int>(width), static_cast<unsigned int>(height),
            rgba.data(), (flags & FT_CLAMP) || (flags2 & FT2_UCLAMP),
            (flags & FT_CLAMP) || (flags2 & FT2_VCLAMP),
            (flags & FT_DYNAMIC) != 0, (flags & FT_NOMIPS) != 0, filterMode);
        if (mirrored)
            texture->m_Size = static_cast<int>(pixelCount * 4);
        return texture;
    }

private:
    SVulkanBufferCallbacks m_callbacks;
    int m_filterOverride = -1;
};

class CVulkanRenderer final : public CNULLRenderer
{
public:
    explicit CVulkanRenderer(CryVR::VulkanFrameRenderer* frameRenderer)
        : m_frameRenderer(frameRenderer)
    {
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
        m_CameraMatrix = camera.GetVCMatrixD3D9();
        m_ViewMatrix = m_CameraMatrix;
    }

    void SetFog(float density, float fogStart, float fogEnd, const float* color,
                int fogMode) override
    {
        if (!color)
            return;
        m_fogDensity = density;
        m_fogStart = fogStart;
        m_fogEnd = fogEnd;
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
        // CSystem owns the OpenXR frame lifetime. The renderer only records
        // native scene work into that already-acquired frame.
        m_frameOpen = m_frameRenderer != nullptr;
    }

    void Draw2dImage(float x, float y, float width, float height, int textureId,
                     float s0, float t0, float s1, float t1, float angle,
                     float red, float green, float blue, float alpha, float) override
    {
        if (m_frameOpen && m_frameRenderer)
            m_frameRenderer->QueuePanelImage(textureId, x, y, width, height, s0, t0, s1, t1,
                angle, red, green, blue, alpha,
                static_cast<float>(GetWidth()), static_cast<float>(GetHeight()));
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
        m_screenSpaceMode = enable;
        if (enable && orthoWidth > 0 && orthoHeight > 0)
        {
            m_screenSpaceWidth = static_cast<float>(orthoWidth);
            m_screenSpaceHeight = static_cast<float>(orthoHeight);
        }
        CNULLRenderer::Set2DMode(enable, orthoWidth, orthoHeight);
    }

    void SetTexture(int textureId, ETexType textureType = eTT_Base) override
    {
        m_currentTextureId = textureId;
        CNULLRenderer::SetTexture(textureId, textureType);
    }

    void DrawDynVB(struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F* vertices, ushort* indices,
                   int vertexCount, int indexCount, int primitiveType) override
    {
        if (!m_frameOpen || !m_frameRenderer || vertexCount <= 0 || indexCount <= 0)
            return;
        if (!m_screenSpaceMode || !vertices || !indices || m_currentTextureId <= 0 ||
            primitiveType != R_PRIMV_TRIANGLES || indexCount % 6 != 0)
        {
            m_frameRenderer->RequirePanelFallback();
            return;
        }

        // ScriptObjectRenderer emits screen-space quads with the stock index
        // pattern (0,1,3 / 1,2,3). Preserve each quad's texture rectangle and
        // vertex tint when routing it to the shared, stereo UI panel.
        for (int baseIndex = 0; baseIndex < indexCount; baseIndex += 6)
        {
            const ushort i0 = indices[baseIndex];
            const ushort i1 = indices[baseIndex + 1];
            const ushort i3 = indices[baseIndex + 2];
            const ushort i1Again = indices[baseIndex + 3];
            const ushort i2 = indices[baseIndex + 4];
            const ushort i3Again = indices[baseIndex + 5];
            if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount || i3 >= vertexCount ||
                i1Again != i1 || i3Again != i3)
            {
                m_frameRenderer->RequirePanelFallback();
                continue;
            }

            const struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F& topLeft = vertices[i0];
            const struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F& topRight = vertices[i1];
            const struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F& bottomRight = vertices[i2];
            const struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F& bottomLeft = vertices[i3];
            const float width = topRight.xyz.x - topLeft.xyz.x;
            const float height = bottomLeft.xyz.y - topLeft.xyz.y;
            if (!(width > 0.0f) || !(height > 0.0f) ||
                topRight.xyz.y != topLeft.xyz.y || bottomLeft.xyz.x != topLeft.xyz.x ||
                bottomRight.xyz.x != topRight.xyz.x || bottomRight.xyz.y != bottomLeft.xyz.y ||
                memcmp(&topLeft.color, &topRight.color, sizeof(UCol)) != 0 ||
                memcmp(&topLeft.color, &bottomRight.color, sizeof(UCol)) != 0 ||
                memcmp(&topLeft.color, &bottomLeft.color, sizeof(UCol)) != 0)
            {
                m_frameRenderer->RequirePanelFallback();
                continue;
            }
            const UCol color = topLeft.color;
            const float invByte = 1.0f / 255.0f;
            m_frameRenderer->QueuePanelImage(m_currentTextureId,
                topLeft.xyz.x, topLeft.xyz.y, width, height,
                topLeft.st[0], 1.0f - topLeft.st[1],
                bottomRight.st[0], 1.0f - bottomRight.st[1],
                0.0f, color.bcolor[0] * invByte, color.bcolor[1] * invByte,
                color.bcolor[2] * invByte, color.bcolor[3] * invByte,
                m_screenSpaceWidth, m_screenSpaceHeight);
        }
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
            return CNULLRenderer::GetDynVBPtr(vertexCount, vertexOffset, pool);
        vertexOffset = 0;
        if (pool != 0 || vertexCount <= 0 || vertexCount > 65535)
            return nullptr;
        m_fontVertices.resize(static_cast<size_t>(vertexCount));
        return m_fontVertices.data();
    }

    void DrawDynVB(int vertexOffset, int pool, int vertexCount) override
    {
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
                screenWidth, screenHeight);
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

            for (int order = 0; order < NUMRI_LISTS; ++order)
            {
                const int list = stockListOrder[order];
                int drawFirst = SRendItem::m_StartRI[recurseLevel][list];
                int last = SRendItem::m_EndRI[recurseLevel][list];
                if (drawFirst < 0 || drawFirst >= last)
                    continue;

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

                for (int itemIndex = drawFirst; itemIndex < last; ++itemIndex)
                {
                    SRendItemPre* item = &SRendItem::m_RendItems[list][itemIndex];
                    if (!item->Item)
                        continue;

                    int objectIndex = 0;
                    SShader* shader = nullptr;
                    SShader* shaderState = nullptr;
                    SRenderShaderResources* resources = nullptr;
                    int fogIndex = 0;
                    SRendItem::mfGet(item->SortVal, &objectIndex, &shader,
                                     &shaderState, &fogIndex, &resources);
                    if (objectIndex < 0 || objectIndex >= m_RP.m_NumVisObjects)
                        continue;

                    m_RP.m_pCurObject = m_RP.m_VisObjects[objectIndex];
                    m_RP.m_pRE = item->Item;
                    m_RP.m_pShader = shader;
                    m_RP.m_pShaderResources = resources;
                    m_RP.m_DynLMask = item->DynLMask;
                    // CGLRenderer::EF_Start resets this for each shader/item
                    // batch. Vulkan emits the passes directly, so preserve the
                    // same per-item lifetime explicitly.
                    m_RP.m_RendPass = 0;
                    m_RP.m_ObjFlags = m_RP.m_pCurObject->m_ObjFlags;
                    m_RP.m_pFogVolume = fogIndex > 0 && fogIndex < m_RP.m_FogVolumes.Num() ?
                        &m_RP.m_FogVolumes[fogIndex] : nullptr;

                    if (shader && shader->m_Passes.Num() > 0)
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
                    else if (shader && shader->m_HWTechniques.Num() > 0)
                    {
                        const int techniqueIndex = EF_SelectHWTechnique(shader);
                        if (techniqueIndex >= 0 && techniqueIndex < shader->m_HWTechniques.Num())
                        {
                            SShaderTechnique* technique = shader->m_HWTechniques[techniqueIndex];
                            m_RP.m_pCurTechnique = technique;
                            const int cull = technique->m_eCull == static_cast<ECull>(-1) ?
                                MapStockCullMode(shader->m_eCull) :
                                MapStockCullMode(technique->m_eCull);
                            for (int passIndex = 0; passIndex < technique->m_Passes.Num(); ++passIndex)
                            {
                                SShaderPassHW* pass = &technique->m_Passes[passIndex];
                                if (pass->m_ePassType != eSHP_General)
                                {
                                    // Keep missing HW pass translations visible in diagnostics;
                                    // do not accidentally execute them through the general shader.
                                    m_frameRenderer->RequirePanelFallback();
                                    continue;
                                }
                                const bool firstOpaquePass = passIndex == 0 &&
                                    !(m_RP.m_ObjFlags & FOB_LIGHTPASS);
                                const uint32_t state = firstOpaquePass ? pass->m_RenderState :
                                    pass->m_SecondRenderState;
                                DrawRenderItem(item->Item, shader, pass, resources, state, cull);
                            }
                        }
                    }
                }
            }
            m_RP.m_pRE = nullptr;
            m_RP.m_pFogVolume = nullptr;
            m_RP.m_pCurTechnique = nullptr;
        }

        CNULLRenderer::EF_EndEf3D(flags);
    }

    void DrawBuffer(CVertexBuffer* vertices, SVertexStream* indices, int indexCount,
                    int firstIndex, int primitiveType, int flags, int numVerts,
                    CMatInfo* materialInfo) override
    {
        const bool hasPrimitiveGroups = primitiveType == R_PRIMV_MULTI_GROUPS;
        if (!m_frameOpen || !m_frameRenderer || indexCount < 0 ||
            (indexCount == 0 && !hasPrimitiveGroups))
            return;
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
        int colorOps[2] = { eCO_MODULATE, eCO_MODULATE };
        int alphaOps[2] = { eCO_MODULATE, eCO_MODULATE };
        uint32_t colorArgs[2] = { DEF_TEXARG0, DEF_TEXARG1 };
        uint32_t alphaArgs[2] = { DEF_TEXARG0, DEF_TEXARG1 };
        float textureMatrices[2][16] = {};
        for (int stage = 0; stage < 2; ++stage)
            textureMatrices[stage][0] = textureMatrices[stage][5] =
                textureMatrices[stage][10] = textureMatrices[stage][15] = 1.0f;
        int normalMapTextureId = 0;
        float materialLighting[8] = { -0.35f, 0.72f, 0.60f, 0.0f,
                                      0.78f, 0.78f, 0.78f, 0.22f };
        if (m_activeResources && m_activeResources->m_LMaterial)
        {
            const SSideMaterial& material = m_activeResources->m_LMaterial->Front;
            materialLighting[7] = 0.22f *
                (0.30f * material.m_Ambient.r + 0.59f * material.m_Ambient.g +
                 0.11f * material.m_Ambient.b);
            materialLighting[4] = 0.78f * material.m_Diffuse.r;
            materialLighting[5] = 0.78f * material.m_Diffuse.g;
            materialLighting[6] = 0.78f * material.m_Diffuse.b;
        }
        if (m_RP.m_ObjFlags & FOB_IGNOREMATERIALAMBIENT)
            materialLighting[7] = 0.0f;

        std::vector<std::array<float, 8>> lightPasses;
        if (HasStockVertexNormal(vertices->m_vertexformat) && m_RP.m_DynLMask &&
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
                    if (!light || !(light->m_Flags & (DLF_DIRECTIONAL | DLF_POINT)))
                        continue;
                    Vec3d objectLightPosition;
                    TransformPosition(objectLightPosition, light->m_Origin,
                                      m_RP.m_pCurObject->GetInvMatrix());
                    const float lightVectorLength = objectLightPosition.Length();
                    const bool directionalLight = (light->m_Flags & DLF_DIRECTIONAL) != 0;
                    const bool pointLight = !directionalLight &&
                                            (light->m_Flags & DLF_POINT) != 0;
                    if (!pointLight && !(lightVectorLength > 1.0e-6f))
                        continue;

                    std::array<float, 8> parameters;
                    memcpy(parameters.data(), materialLighting, sizeof(materialLighting));
                    parameters[0] = objectLightPosition.x;
                    parameters[1] = objectLightPosition.y;
                    parameters[2] = objectLightPosition.z;
                    parameters[3] = pointLight ? light->m_fRadius : 0.0f;
                    const float lightScale = directionalLight ? 1.5f : 1.0f;
                    parameters[4] *= light->m_Color.r * lightScale;
                    parameters[5] *= light->m_Color.g * lightScale;
                    parameters[6] *= light->m_Color.b * lightScale;
                    // Emit material ambient once; following lights use additive
                    // RGB blending and must contribute only their diffuse term.
                    if (!lightPasses.empty())
                        parameters[7] = 0.0f;
                    lightPasses.push_back(parameters);
                }
            }
        }
        if (lightPasses.empty())
        {
            std::array<float, 8> parameters;
            memcpy(parameters.data(), materialLighting, sizeof(materialLighting));
            lightPasses.push_back(parameters);
        }
        uint32_t renderState = m_activePass ? m_activeRenderState :
            static_cast<uint32_t>(m_CurState);
        if (m_polygonMode == R_WIREFRAME_MODE)
            renderState |= GS_POLYLINE;
        float globalOpacity = 1.0f;
        float alphaTestRef = 0.0f;
        bool additiveMaterial = false;
        if (m_activeResources)
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
        {
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
                    textureResource->Update(stage);
                    unit = &textureResource->m_TU;
                    memcpy(textureMatrices[stage],
                           textureResource->m_TexModificator.m_TexMatrix.GetData(),
                           sizeof(textureMatrices[stage]));
                }
                const_cast<SShaderTexUnit*>(unit)->mfUpdate();
                if (unit->m_ITexPic)
                {
                    textureIds[stage] = unit->m_ITexPic->GetTextureID();
                    // Use the texture selected by this actual template pass.
                    // EFTT_BUMP may be a stock-combined BUMP+NORMALMAP image;
                    // EFTT_BUMP_DIFFUSE is the corresponding $BumpDiffuse map.
                    if (resourceSlot == EFTT_BUMP || resourceSlot == EFTT_BUMP_DIFFUSE)
                        normalMapTextureId = textureIds[stage];
                }
                colorOps[stage] = unit->m_eColorOp;
                alphaOps[stage] = unit->m_eAlphaOp;
                colorArgs[stage] = unit->m_eColorArg;
                alphaArgs[stage] = unit->m_eAlphaArg;
            }
        }
        if (m_forceCurrentTextureForClientDraw && m_currentTextureId > 0)
            textureIds[0] = m_currentTextureId;

        if (m_RP.m_pCurObject && (m_RP.m_pCurObject->m_ObjFlags & FOB_TRANS_MASK))
            mathMatrixMultiply(modelView, m_CameraMatrix.GetData(),
                               m_RP.m_pCurObject->m_Matrix.GetData(), g_CpuFlags);
        else
            memcpy(modelView, m_CameraMatrix.GetData(), sizeof(modelView));
        memcpy(textureMatrix, textureMatrices[0], sizeof(textureMatrix));

        const uint16_t* indexData = static_cast<const uint16_t*>(indices->m_VData) + firstIndex;
        std::vector<uint16_t> triangulatedQuads;
        uint32_t drawIndexCount = static_cast<uint32_t>(indexCount);
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
        for (size_t lightPass = 0; lightPass < lightPasses.size(); ++lightPass)
        {
            uint32_t passState = renderState;
            if (lightPass > 0)
            {
                passState &= ~(GS_BLEND_MASK | GS_DEPTHWRITE);
                passState |= GS_BLSRC_ONE | GS_BLDST_ONE | GS_COLMASKONLYRGB;
            }
            if (!m_frameRenderer->QueueStockClientIndexedDraw(
                vertices->m_VS[VSF_GENERAL].m_VData,
                static_cast<uint32_t>(vertices->m_NumVerts), indexData,
                drawIndexCount, vertices->m_vertexformat, topology,
                passState, m_activePass ? m_activeCull : R_CULL_BACK,
                textureIds[0], textureIds[1],
                colorOps[0], alphaOps[0], colorOps[1], alphaOps[1],
                colorArgs[0], alphaArgs[0], 0xffffffffu,
                colorArgs[1], alphaArgs[1], 0xffffffffu,
                static_cast<uint32_t>(m_CurStencilState), m_CurStencRef, m_CurStencMask,
                modelView, textureMatrix, textureMatrices[1], lightPasses[lightPass].data(),
                lightPass > 0, globalOpacity, alphaTestRef,
                reinterpret_cast<const CryVR::VulkanBuffer*>(
                    vertices->m_VS[VSF_TANGENTS].m_VulkanBufferHandle),
                normalMapTextureId))
                m_frameRenderer->RequirePanelFallback();
        }
    }

private:
    void UpdateVulkanFog()
    {
        if (m_frameRenderer)
            m_frameRenderer->SetStockFog(m_fogEnabled, m_fogDensity, m_fogStart,
                                         m_fogEnd, m_fogColor, m_fogMode);
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
                        SRenderShaderResources* resources, uint32_t state, int cull)
    {
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
        m_activePass = pass;
        m_activeResources = resources;
        m_activeRenderState = state;
        m_activeCull = cull;
        if (element->mfGetType() == eDATA_TempMesh)
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
        else
            element->mfDraw(shader, pass);
        m_activeCull = R_CULL_BACK;
        m_activeRenderState = 0;
        m_activeResources = nullptr;
        m_activePass = nullptr;
        SetScissor(0, 0, 0, 0);
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
    SRenderShaderResources* m_activeResources = nullptr;
    uint32_t m_activeRenderState = 0;
    int m_activeCull = R_CULL_BACK;
    int m_polygonMode = R_SOLID_MODE;
    float m_materialColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    std::vector<struct_VERTEX_FORMAT_P3F_COL4UB_TEX2F> m_fontVertices;
    int m_fontTextureId = -1;
    bool m_fontRenderingState = false;
    bool m_frameOpen = false;
    bool m_screenSpaceMode = false;
    int m_currentTextureId = 0;
    bool m_forceCurrentTextureForClientDraw = false;
    float m_screenSpaceWidth = 800.0f;
    float m_screenSpaceHeight = 600.0f;
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
