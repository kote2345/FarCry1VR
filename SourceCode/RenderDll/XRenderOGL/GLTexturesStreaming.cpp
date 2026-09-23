/*=============================================================================
  GLTexturesStreaming.cpp : OpenGL specific texture streaming technology.
  Copyright (c) 2001 Crytek Studios. All Rights Reserved.

  Revision history:
    * Created by Honitch Andrey

=============================================================================*/

#include "RenderPCH.h"
#include "GL_Renderer.h"
#include "GLVulkanTextureDecode.h"
#include "GLPBuffer.h"
#include "I3DEngine.h"

#include <vector>

//===============================================================================

static void MirrorStreamingTextureBase(int textureId, ETEX_Format format,
                                       unsigned int width, unsigned int height,
                                       const byte* pixels, int pixelBytes,
                                       bool clampU, bool clampV, bool dynamicTexture)
{
  if (!pixels || !width || !height)
    return;
  const size_t pixelCount = static_cast<size_t>(width) * height;
  if (format == eTF_DXT1 || format == eTF_DXT3 || format == eTF_DXT5)
  {
    const bool dxt1 = format == eTF_DXT1;
    const size_t blockBytes = dxt1 ? 8 : 16;
    const size_t requiredBytes = static_cast<size_t>((width + 3) / 4) * ((height + 3) / 4) * blockBytes;
    if (pixelBytes < 0 || static_cast<size_t>(pixelBytes) < requiredBytes)
      return;
    std::vector<byte> rgba;
    if (DecodeDxtBaseLevel(pixels, static_cast<int>(width), static_cast<int>(height),
                           dxt1, format == eTF_DXT3, format == eTF_DXT5, rgba))
      gcpOGL->MirrorVulkanTexture(textureId, width, height, rgba.data(), clampU, clampV, dynamicTexture);
    return;
  }
  if (format == eTF_8888)
  {
    if (pixelBytes >= static_cast<int>(pixelCount * 4))
      gcpOGL->MirrorVulkanTexture(textureId, width, height, pixels, clampU, clampV, dynamicTexture);
    return;
  }
  if (format == eTF_8000)
  {
    if (pixelBytes < static_cast<int>(pixelCount))
      return;
    std::vector<byte> rgba(pixelCount * 4);
    for (size_t i = 0; i < pixelCount; ++i)
    {
      rgba[i * 4 + 0] = 255;
      rgba[i * 4 + 1] = 255;
      rgba[i * 4 + 2] = 255;
      rgba[i * 4 + 3] = pixels[i];
    }
    gcpOGL->MirrorVulkanTexture(textureId, width, height, rgba.data(), clampU, clampV, dynamicTexture);
    return;
  }
  if (format == eTF_0088)
  {
    if (pixelBytes < static_cast<int>(pixelCount * 2))
      return;
    std::vector<byte> rgba(pixelCount * 4);
    for (size_t i = 0; i < pixelCount; ++i)
    {
      const byte luminance = pixels[i * 2];
      rgba[i * 4 + 0] = luminance;
      rgba[i * 4 + 1] = luminance;
      rgba[i * 4 + 2] = luminance;
      rgba[i * 4 + 3] = pixels[i * 2 + 1];
    }
    gcpOGL->MirrorVulkanTexture(textureId, width, height, rgba.data(), clampU, clampV, dynamicTexture);
    return;
  }
  if ((format != eTF_0888 && format != eTF_RGB8) ||
      pixelBytes < static_cast<int>(pixelCount * 3))
    return;
  std::vector<byte> rgba(pixelCount * 4);
  for (size_t i = 0; i < pixelCount; ++i)
  {
    rgba[i * 4 + 0] = pixels[i * 3 + 0];
    rgba[i * 4 + 1] = pixels[i * 3 + 1];
    rgba[i * 4 + 2] = pixels[i * 3 + 2];
    rgba[i * 4 + 3] = 255;
  }
  gcpOGL->MirrorVulkanTexture(textureId, width, height, rgba.data(), clampU, clampV, dynamicTexture);
}

void STexPic::BuildMips()
{
  if (!m_Mips[0])
    CreateMips();

  glBindTexture(m_TargetType, m_Bind);
  if (m_eTT != eTT_Cubemap)
  {
    for (int i=0; i<m_nMips; i++)
    {
      int w, h;
      glGetTexLevelParameteriv(m_TargetType, i, GL_TEXTURE_WIDTH,  &w);
      glGetTexLevelParameteriv(m_TargetType, i, GL_TEXTURE_HEIGHT, &h);
      assert (w && h);
      int tf = CGLTexMan::GetTexSrcFormat(m_ETF);
      int tfd = CGLTexMan::GetTexDstFormat(m_ETF);
      int size = CGLTexMan::TexSize(w, h, 1, tfd);
      SAFE_DELETE(m_Mips[0][i]);
      SMipmap *mp = new SMipmap(w, h, size);
      m_Mips[0][i] = mp;
      mp->m_bUploaded = true;
      if (m_ETF == eTF_DXT1 || m_ETF == eTF_DXT3 || m_ETF == eTF_DXT5)
        glGetCompressedTexImageARB(m_TargetType, i, &mp->DataArray[0]);
      else
        glGetTexImage(m_TargetType, i, tf, GL_UNSIGNED_BYTE, &mp->DataArray[0]);
    }
  }
  else
  {
    static const GLenum cubefaces[6] = 
    {
      GL_TEXTURE_CUBE_MAP_POSITIVE_X_EXT,
      GL_TEXTURE_CUBE_MAP_NEGATIVE_X_EXT,
      GL_TEXTURE_CUBE_MAP_POSITIVE_Y_EXT,
      GL_TEXTURE_CUBE_MAP_NEGATIVE_Y_EXT,
      GL_TEXTURE_CUBE_MAP_POSITIVE_Z_EXT,
      GL_TEXTURE_CUBE_MAP_NEGATIVE_Z_EXT,
    };
    STexPic *tp = this;
    for (int n=0; n<6; n++)
    {
      int tgt = cubefaces[n];
      for (int i=0; i<m_nMips; i++)
      {
        int w, h;
        glGetTexLevelParameteriv(tgt, i, GL_TEXTURE_WIDTH,  &w);
        glGetTexLevelParameteriv(tgt, i, GL_TEXTURE_HEIGHT, &h);
        assert (w && h);
        int tf = CGLTexMan::GetTexSrcFormat(m_ETF);
        int tfd = CGLTexMan::GetTexDstFormat(m_ETF);
        int size = CGLTexMan::TexSize(w, h, 1, tfd);
        SAFE_DELETE(m_Mips[n][i]);
        SMipmap *mp = new SMipmap(w, h, size);
        m_Mips[n][i] = mp;
        mp->m_bUploaded = true;
        if (m_ETF == eTF_DXT1 || m_ETF == eTF_DXT3 || m_ETF == eTF_DXT5)
          glGetCompressedTexImageARB(tgt, i, &mp->DataArray[0]);
        else
          glGetTexImage(tgt, i, tf, GL_UNSIGNED_BYTE, &mp->DataArray[0]);
      }
    }
  }
}

bool STexPic::UploadMips(int nStartMip, int nEndMip)
{
  glBindTexture(m_TargetType, m_Bind);
  if (SUPPORTS_GL_SGIS_texture_lod/* && m_eTT != eTT_Cubemap*/)
  {
		glTexParameteri(m_TargetType, GL_TEXTURE_BASE_LEVEL_SGIS, nStartMip);
		//glTexParameteri(m_TargetType, GL_TEXTURE_MAX_LEVEL_SGIS, m_nMips-1);
		//glTexParameterf(m_TargetType, GL_TEXTURE_MIN_LOD_SGIS, (float)nStartMip);
		//glTexParameterf(m_TargetType, GL_TEXTURE_MAX_LOD_SGIS, (float)nEndMip);
    if (m_eTT != eTT_Cubemap)
    {
      int tfs = CGLTexMan::GetTexSrcFormat(m_ETF);
      int tfd = CGLTexMan::GetTexDstFormat(m_ETF);
      for (int i=nStartMip; i<=nEndMip; i++)
      {
        int nLod = i;
        SMipmap *mp = m_Mips[0][i];
        if (mp->m_bUploaded)
          continue;
        m_LoadedSize += m_pFileTexMips[i].m_Size;
        gRenDev->m_TexMan->m_StatsCurTexMem += m_pFileTexMips[i].m_Size;
        if (m_ETF == eTF_DXT1 || m_ETF == eTF_DXT3 || m_ETF == eTF_DXT5)
        {
          if (nLod && (!m_Mips[0][0] || !m_Mips[0][0]->m_bUploaded))
            glTexImage2D(m_TargetType, 0, tfd, m_Width, m_Height, 0, tfs, GL_UNSIGNED_BYTE, NULL);
          glCompressedTexImage2DARB(m_TargetType, nLod, tfd, mp->USize, mp->VSize, 0, mp->DataArray.Num(), &mp->DataArray[0]);
        }
        else
        {
          if (nLod && (!m_Mips[0][0] || !m_Mips[0][0]->m_bUploaded))
            glTexImage2D(m_TargetType, 0, tfd, m_Width, m_Height, 0, tfs, GL_UNSIGNED_BYTE, NULL);
          glTexImage2D(m_TargetType, nLod, tfd, mp->USize, mp->VSize, 0, tfs, GL_UNSIGNED_BYTE, &mp->DataArray[0]);
        }
        if (i == 0 && m_TargetType == GL_TEXTURE_2D)
          MirrorStreamingTextureBase(m_Bind, m_ETF, mp->USize, mp->VSize,
              &mp->DataArray[0], mp->DataArray.GetSize(),
              (m_Flags & FT_CLAMP) || (m_Flags2 & FT2_UCLAMP),
              (m_Flags & FT_CLAMP) || (m_Flags2 & FT2_VCLAMP),
              (m_Flags & FT_DYNAMIC) != 0);
        mp->m_bUploaded = true;
      }
    }
    else
    {
      static const GLenum cubefaces[6] = 
      {
        GL_TEXTURE_CUBE_MAP_POSITIVE_X_EXT,
        GL_TEXTURE_CUBE_MAP_NEGATIVE_X_EXT,
        GL_TEXTURE_CUBE_MAP_POSITIVE_Y_EXT,
        GL_TEXTURE_CUBE_MAP_NEGATIVE_Y_EXT,
        GL_TEXTURE_CUBE_MAP_POSITIVE_Z_EXT,
        GL_TEXTURE_CUBE_MAP_NEGATIVE_Z_EXT,
      };
      for (int n=0; n<6; n++)
      {
        int tgt = cubefaces[n];
        int tfs = CGLTexMan::GetTexSrcFormat(m_ETF);
        int tfd = CGLTexMan::GetTexDstFormat(m_ETF);
        for (int i=nStartMip; i<=nEndMip; i++)
        {
          int nLod = i;
          SMipmap *mp = m_Mips[n][i];
          if (mp->m_bUploaded)
            continue;
          m_LoadedSize += m_pFileTexMips[i].m_Size;
          gRenDev->m_TexMan->m_StatsCurTexMem += m_pFileTexMips[i].m_Size;
          if (m_ETF == eTF_DXT1 || m_ETF == eTF_DXT3 || m_ETF == eTF_DXT5)
          {
            if (nLod && (!m_Mips[0][0] || !m_Mips[0][0]->m_bUploaded))
              glTexImage2D(tgt, 0, tfd, m_Width, m_Height, 0, tfs, GL_UNSIGNED_BYTE, NULL);
            glCompressedTexImage2DARB(tgt, nLod, tfd, mp->USize, mp->VSize, 0, mp->DataArray.Num(), &mp->DataArray[0]);
          }
          else
          {
            if (nLod && (!m_Mips[0][0] || !m_Mips[0][0]->m_bUploaded))
              glTexImage2D(tgt, 0, tfd, m_Width, m_Height, 0, tfs, GL_UNSIGNED_BYTE, NULL);
            glTexImage2D(tgt, nLod, tfd, mp->USize, mp->VSize, 0, tfs, GL_UNSIGNED_BYTE, &mp->DataArray[0]);
          }
          mp->m_bUploaded = true;
        }
      }
    }
  }
  else
  {
    if (m_eTT != eTT_Cubemap)
    {
      int tfs = CGLTexMan::GetTexSrcFormat(m_ETF);
      int tfd = CGLTexMan::GetTexDstFormat(m_ETF);
      for (int i=nStartMip; i<=nEndMip; i++)
      {
        SMipmap *mp = m_Mips[0][i];
        if (mp->m_bUploaded)
          continue;
        int nLod = i-nStartMip;
        if (m_ETF == eTF_DXT1 || m_ETF == eTF_DXT3 || m_ETF == eTF_DXT5)
          glCompressedTexImage2DARB(m_TargetType, nLod, tfd, mp->USize, mp->VSize, 0, mp->DataArray.Num(), &mp->DataArray[0]);
        else
          glTexImage2D(m_TargetType, nLod, tfd, mp->USize, mp->VSize, 0, tfs, GL_UNSIGNED_BYTE, &mp->DataArray[0]);
        if (i == 0 && m_TargetType == GL_TEXTURE_2D)
          MirrorStreamingTextureBase(m_Bind, m_ETF, mp->USize, mp->VSize,
              &mp->DataArray[0], mp->DataArray.GetSize(),
              (m_Flags & FT_CLAMP) || (m_Flags2 & FT2_UCLAMP),
              (m_Flags & FT_CLAMP) || (m_Flags2 & FT2_VCLAMP),
              (m_Flags & FT_DYNAMIC) != 0);
        mp->m_bUploaded = true;
      }
    }
    else
    {
      static const GLenum cubefaces[6] = 
      {
        GL_TEXTURE_CUBE_MAP_POSITIVE_X_EXT,
        GL_TEXTURE_CUBE_MAP_NEGATIVE_X_EXT,
        GL_TEXTURE_CUBE_MAP_POSITIVE_Y_EXT,
        GL_TEXTURE_CUBE_MAP_NEGATIVE_Y_EXT,
        GL_TEXTURE_CUBE_MAP_POSITIVE_Z_EXT,
        GL_TEXTURE_CUBE_MAP_NEGATIVE_Z_EXT,
      };
      for (int n=0; n<6; n++)
      {
        int tgt = cubefaces[n];
        int tfs = CGLTexMan::GetTexSrcFormat(m_ETF);
        int tfd = CGLTexMan::GetTexDstFormat(m_ETF);
        for (int i=nStartMip; i<=nEndMip; i++)
        {
          SMipmap *mp = m_Mips[n][i];
          if (mp->m_bUploaded)
            continue;
          int nLod = i-nStartMip;
          if (m_ETF == eTF_DXT1 || m_ETF == eTF_DXT3 || m_ETF == eTF_DXT5)
            glCompressedTexImage2DARB(tgt, nLod, tfd, mp->USize, mp->VSize, 0, mp->DataArray.Num(), &mp->DataArray[0]);
          else
            glTexImage2D(tgt, nLod, tfd, mp->USize, mp->VSize, 0, tfs, GL_UNSIGNED_BYTE, &mp->DataArray[0]);
          mp->m_bUploaded = true;
        }
      }
    }
  }
  SetFilter();
  SetWrapping();

  return true;
}

void STexPic::RemoveFromPool()
{
  if (!m_pPoolItem)
    return;
  STexPoolItem *pIT = m_pPoolItem;
  m_pPoolItem = NULL;
  pIT->m_pTex = NULL;
  m_LoadedSize = 0;
}

void CTexMan::UnloadOldTextures(STexPic *pExclude)
{
  if (!CRenderer::CV_r_texturesstreampoolsize)
    return;
  //ValidateTexSize();

  STexPic *pTP = STexPic::m_Root.m_Prev;
  while (m_StatsCurTexMem >= CRenderer::CV_r_texturesstreampoolsize*1024*1024)
  {
    if (pTP == &STexPic::m_Root)
    {
      ICVar *var = iConsole->GetCVar("r_TexturesStreamPoolSize");
      var->Set(m_StatsCurTexMem/(1024*1024)+30);
      iLog->Log("WARNING: Texture pool was changed to %d Mb", CRenderer::CV_r_texturesstreampoolsize);
      return;
    }

    STexPic *Next = pTP->m_Prev;
    if (pTP != pExclude)
      pTP->Unload();
    pTP = Next;
  }
}


void STexPic::PrecacheAsynchronously(float fDist, int Flags)
{
}


void CTexMan::CheckTexLimits(STexPic *pExclude)
{
  if (!(gRenDev->m_TexMan->m_Streamed & 1))
    return;
  if (CRenderer::CV_r_texturesstreampoolsize < 10)
    CRenderer::CV_r_texturesstreampoolsize = 10;

  ValidateTexSize();

  if (gRenDev->m_TexMan->m_StatsCurTexMem >= CRenderer::CV_r_texturesstreampoolsize*1024*1024)
    UnloadOldTextures(pExclude);
}


