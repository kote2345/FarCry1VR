/*=============================================================================
  PS2_RendPipeline.cpp : PS2 specific rendering using shaders pipeline.
  Copyright (c) 2001 Crytek Studios. All Rights Reserved.

    Revision history:
  		* Created by Honich Andrey
    
=============================================================================*/

#include "RenderPCH.h"
#include "NULL_Renderer.h"
#include "I3DEngine.h"

#include "platform.h"

//============================================================================================
// Shaders rendering
//============================================================================================

//============================================================================================
// Init Shaders rendering

void CNULLRenderer::EF_InitRandTables()
{
  int i;
  float f;

  for (i=0; i<256; i++)
  {
    f = (float)rand() / 32767.0f;
    m_RP.m_tRandFloats[i] = f + f - 1.0f;

    m_RP.m_tRandBytes[i] = (byte)((float)rand() / 32767.0f * 255.0f);
  }
}

void CNULLRenderer::EF_InitWaveTables()
{  
  int i;
  
  //Init wave Tables
  for (i=0; i<1024; i++)
  {
    float f = (float)i;
    
    m_RP.m_tSinTable[i] = cry_sinf(f * (360.0f/1023.0f) * M_PI / 180.0f);
    m_RP.m_tHalfSinTable[i] = cry_sinf(f * (360.0f/1023.0f) * M_PI / 180.0f);
    if (m_RP.m_tHalfSinTable[i] < 0)
      m_RP.m_tHalfSinTable[i] = 0;
    m_RP.m_tCosTable[i] = cry_cosf(f * (360.0f/1023.0f) * M_PI / 180.0f);
    m_RP.m_tHillTable[i] = cry_sinf(f * (180.0f/1023.0f) * M_PI / 180.0f);
    
    if (i < 512)
      m_RP.m_tSquareTable[i] = 1.0f;
    else
      m_RP.m_tSquareTable[i] = -1.0f;
    
    m_RP.m_tSawtoothTable[i] = f / 1024.0f;
    m_RP.m_tInvSawtoothTable[i] = 1.0f - m_RP.m_tSawtoothTable[i];
    
    if (i < 512)
    {
      if (i < 256)
        m_RP.m_tTriTable[i] = f / 256.0f;
      else
        m_RP.m_tTriTable[i] = 1.0f - m_RP.m_tTriTable[i-256];
    }
    else
      m_RP.m_tTriTable[i] = 1.0f - m_RP.m_tTriTable[i-512];
  }
}

void CNULLRenderer::EF_InitEvalFuncs(int num)
{
  switch(num)
  {
    case 0:
      m_RP.m_pCurFuncs = &m_RP.m_EvalFuncs_C;
      break;
    default:
    case 1:
      m_RP.m_pCurFuncs = &m_RP.m_EvalFuncs_RE;
      break;
  }
}

int CNULLRenderer::EF_RegisterFogVolume(float fMaxFogDist, float fFogLayerZ, CFColor color, int nIndex, bool bCaustics)
{
  if (nIndex < 0)
  {
    SMFog Fog;
    memset(&Fog,0,sizeof(Fog));

    Fog.m_fMaxDist = fMaxFogDist;
    Fog.m_FogInfo.m_FogColor = color;
    Fog.m_Dist = fFogLayerZ;
    Fog.m_Color = color;
    Fog.m_Color.a = 1.0f;
    Fog.bCaustics = bCaustics;
    //Fog.m_FogInfo.m_FogColor = m_FogColor;
    Fog.m_Normal = Vec3d(0,0,1);

    m_RP.m_FogVolumes.AddElem(Fog);
    return m_RP.m_FogVolumes.Num()-1;
  }
  else
  {
    assert (nIndex < m_RP.m_FogVolumes.Num());
    SMFog *pFog = &m_RP.m_FogVolumes[nIndex];
    pFog->m_fMaxDist = fMaxFogDist;
    pFog->m_FogInfo.m_FogColor = color;
    pFog->m_Dist = fFogLayerZ;
    pFog->m_Color = color;
    pFog->m_Color.a = 1.0f;
    pFog->bCaustics = bCaustics;
    return nIndex;
  }
}

void CNULLRenderer::EF_PipelineInit()
{
  bool nv = 0;

  m_RP.m_MaxVerts = 600;
  m_RP.m_MaxTris = 300;

  EF_InitWaveTables();
  EF_InitRandTables();
  EF_InitEvalFuncs(0);
  EF_InitFogVolumes();

//==================================================

  SAFE_DELETE_ARRAY(m_RP.m_VisObjects);

  CCObject::m_Waves.Create(32);
  CCObject::m_ObjMatrices.reinit(32);
  m_RP.m_VisObjects = new CCObject *[MAX_REND_OBJECTS];

  if (!m_RP.m_TempObjects.Num())
    m_RP.m_TempObjects.Reserve(MAX_REND_OBJECTS);
  if (!m_RP.m_Objects.Num())
  {
    m_RP.m_Objects.Reserve(MAX_REND_OBJECTS);
    m_RP.m_Objects.SetUse(1);
    SAFE_DELETE_ARRAY(m_RP.m_ObjectsPool);
    m_RP.m_nNumObjectsInPool = 384;
    m_RP.m_ObjectsPool = new CCObject[m_RP.m_nNumObjectsInPool];
    for (int i=0; i<m_RP.m_nNumObjectsInPool; i++)
    {
      m_RP.m_TempObjects[i] = &m_RP.m_ObjectsPool[i];
      m_RP.m_TempObjects[i]->Init();
      m_RP.m_TempObjects[i]->m_Color = Col_White;
      m_RP.m_TempObjects[i]->m_ObjFlags = 0;
      m_RP.m_TempObjects[i]->m_Matrix.SetIdentity();
      m_RP.m_TempObjects[i]->m_RenderState = 0;
    }
    m_RP.m_VisObjects[0] = &m_RP.m_ObjectsPool[0];
  }

  //m_RP.m_DLights.Create(64);
  //m_RP.m_DLights.SetUse(0);

  m_RP.m_pREGlare = (CREGlare *)EF_CreateRE(eDATA_Glare);

  for (int i=0; i<VERTEX_FORMAT_NUMS; i++)
  {
    for (int j=0; j<VERTEX_FORMAT_NUMS; j++)
    {
      SVertBufComps Cps[2];
      GetVertBufComps(&Cps[0], i);
      GetVertBufComps(&Cps[1], j);

      bool bNeedTC = Cps[1].m_bHasTC | Cps[0].m_bHasTC;
      bool bNeedCol = Cps[1].m_bHasColors | Cps[0].m_bHasColors;
      bool bNeedSecCol = Cps[1].m_bHasSecColors | Cps[0].m_bHasSecColors;
      bool bNeedNormals = Cps[1].m_bHasNormals | Cps[0].m_bHasNormals;
      m_RP.m_VFormatsMerge[i][j] = VertFormatForComponents(bNeedCol, bNeedSecCol, bNeedNormals, bNeedTC);
    }
  }
}

void CNULLRenderer::EF_ClearBuffers(bool bForce, float *Colors)
{
}

void CNULLRenderer::EF_SetClipPlane (bool bEnable, float *pPlane, bool bRefract)
{
}

void CNULLRenderer::EF_PipelineShutdown()
{
  int i, j;

  CCObject::m_Waves.Free();
  SAFE_DELETE_ARRAY(m_RP.m_VisObjects);

  //m_RP.m_DLights.Free();
  m_RP.m_FogVolumes.Free();
  SAFE_RELEASE(m_RP.m_pREGlare);
  
  for (i=0; i<CREClientPoly2D::mPolysStorage.GetSize(); i++)
  {
    SAFE_RELEASE(CREClientPoly2D::mPolysStorage[i]);
  }
  CREClientPoly2D::mPolysStorage.Free();

  for (j=0; j<4; j++)
  {
    for (i=0; i<CREClientPoly::mPolysStorage[j].GetSize(); i++)
    {
      SAFE_RELEASE(CREClientPoly::mPolysStorage[j][i]);
    }
    CREClientPoly::mPolysStorage[j].Free();
  }
}

void CNULLRenderer::EF_Release(int nFlags)
{
}

//==========================================================================

void CNULLRenderer::EF_SetCameraInfo()
{
}

void CNULLRenderer::EF_CalcObjectMatrix(CCObject *obj)
{
}

void CNULLRenderer::EF_SetObjectTransform(CCObject *obj)
{
}

void CNULLRenderer::EF_PreRender(int Stage)
{
}

//=======================================================================


void CNULLRenderer::EF_Eval_DeformVerts(TArray<SDeform>* Defs)
{
}

void CNULLRenderer::EF_Eval_TexGen(SShaderPass *sfm)
{
}

void CNULLRenderer::EF_Eval_RGBAGen(SShaderPass *sfm)
{
  m_LastRGBAGenSetGlobal = false;
  SShader *ef = m_RP.m_pShader;
  int n;
  UCol color;
  bool bSetCol = false;
  color.dcolor = -1;

  switch(sfm->m_eEvalRGB)
  {
    case eERGB_NoFill:
      break;

    case eERGB_Identity:
      if (m_RP.m_pRE)
      {
        if (!(m_RP.m_FlagsPerFlush & RBSI_RGBGEN))
        {
          color.dcolor = -1;
          bSetCol = true;
          m_RP.m_FlagsPerFlush |= RBSI_RGBGEN;
        }
      }
      else
      {
        byte *ptr = m_RP.m_Ptr.PtrB+m_RP.m_OffsD;
        for (n=0; n<m_RP.m_RendNumVerts; n++, ptr+=m_RP.m_Stride)
        {
          *(uint *)ptr = -1;
        }
      }
      break;

    case eERGB_FromClient:
      if (!m_RP.m_pRE)
      {
        if (!gbRgb)
        {
          byte *ptr = m_RP.m_Ptr.PtrB+m_RP.m_OffsD;
          byte *src = (byte *)(m_RP.m_pClientColors[0]);
          for (n=0; n<m_RP.m_RendNumVerts; n++, ptr+=m_RP.m_Stride, src+=4)
          {
            *(uint *)ptr = *(uint *)(src);
          }
        }
        else
        {
          byte *ptr = m_RP.m_Ptr.PtrB+m_RP.m_OffsD;
          byte *src = (byte *)(&m_RP.m_pClientColors[0]);
          for (n=0; n<m_RP.m_RendNumVerts; n++, ptr+=m_RP.m_Stride, src+=4)
          {
            ptr[2] = src[0];
            ptr[1] = src[1];
            ptr[0] = src[2];
            ptr[3] = src[3];
          }
        }
      }
      break;

    case eERGB_Fixed:
      color = sfm->m_FixedColor;
      bSetCol = true;
      m_RP.m_FlagsPerFlush |= RBSI_RGBGEN;
      break;

    case eERGB_StyleIntens:
      {
        CLightStyle *ls = CLightStyle::mfGetStyle(sfm->m_Style, m_RP.m_RealTime);
        color = sfm->m_FixedColor;
        color.bcolor[0] = (byte)((float)color.bcolor[0] * ls->m_fIntensity);
        color.bcolor[1] = (byte)((float)color.bcolor[1] * ls->m_fIntensity);
        color.bcolor[2] = (byte)((float)color.bcolor[2] * ls->m_fIntensity);
        bSetCol = true;
        m_RP.m_FlagsPerFlush |= RBSI_RGBGEN;
      }
      break;

    case eERGB_StyleColor:
      {
        CLightStyle *ls = CLightStyle::mfGetStyle(sfm->m_Style, m_RP.m_RealTime);
        color.dcolor = ls->m_Color.GetTrue();
        bSetCol = true;
        m_RP.m_FlagsPerFlush |= RBSI_RGBGEN;
      }
      break;

    case eERGB_Comps:
      {
        if (sfm->m_RGBComps)
        {
          float *vals = sfm->m_RGBComps->mfGet();
          color.bcolor[0] = (byte)(vals[0] * 255.0f);
          color.bcolor[1] = (byte)(vals[1] * 255.0f);
          color.bcolor[2] = (byte)(vals[2] * 255.0f);
          color.bcolor[3] = (byte)(vals[3] * 255.0f);
          bSetCol = true;
          m_RP.m_FlagsPerFlush |= RBSI_RGBGEN;
        }
      }
      break;

    case eERGB_OneMinusFromClient:
      if (!gbRgb)
      {
        byte *ptr = m_RP.m_Ptr.PtrB+m_RP.m_OffsD;
        for (n=0; n<m_RP.m_RendNumVerts; n++, ptr+=m_RP.m_Stride)
        {
          ptr[0] = 255 - m_RP.m_pClientColors[n][0];
          ptr[1] = 255 - m_RP.m_pClientColors[n][1];
          ptr[2] = 255 - m_RP.m_pClientColors[n][2];
        }
      }
      else
      {
        byte *ptr = m_RP.m_Ptr.PtrB+m_RP.m_OffsD;
        for (n=0; n<m_RP.m_RendNumVerts; n++, ptr+=m_RP.m_Stride)
        {
          ptr[0] = 255 - m_RP.m_pClientColors[n][2];
          ptr[1] = 255 - m_RP.m_pClientColors[n][1];
          ptr[2] = 255 - m_RP.m_pClientColors[n][0];
        }
      }
      break;

    case eERGB_Wave:
      if (sfm->m_WaveEvalRGB)
      {
        if (m_RP.m_pRE)
        {
          if (!(m_RP.m_FlagsPerFlush & RBSI_RGBGEN))
          {
            float val = SEvalFuncs::EvalWaveForm(sfm->m_WaveEvalRGB);
            if (val < 0)
              val = 0;
            if (val > 1)
              val = 1;
            
            color.bcolor[0] = color.bcolor[1] = color.bcolor[2] = (int)(val * 255.0f);
            COLCONV(color.dcolor);
            bSetCol = true;
            m_RP.m_FlagsPerFlush |= RBSI_RGBGEN;
          }
          else
            m_RP.m_pCurFuncs->ERGB_Wave(sfm->m_WaveEvalRGB, color);
        }
      }
      break;

    case eERGB_Noise:
      if (sfm->m_RGBNoise)
      {
        if (m_RP.m_pRE)
        {
          if (!(m_RP.m_FlagsPerFlush & RBSI_RGBGEN))
          {
            float v = RandomNum();
            byte r = (byte)(CLAMP(v * sfm->m_RGBNoise->m_RangeR + sfm->m_RGBNoise->m_ConstR, 0.0f, 1.0f) * 255.0f);
            v = RandomNum();
            byte g = (byte)(CLAMP(v * sfm->m_RGBNoise->m_RangeG + sfm->m_RGBNoise->m_ConstG, 0.0f, 1.0f) * 255.0f);
            v = RandomNum();
            byte b = (byte)(CLAMP(v * sfm->m_RGBNoise->m_RangeB + sfm->m_RGBNoise->m_ConstB, 0.0f, 1.0f) * 255.0f);
            
            color.bcolor[0] = r;
            color.bcolor[1] = g;
            color.bcolor[2] = b;
            COLCONV(color.dcolor);
            bSetCol = true;
            m_RP.m_FlagsPerFlush |= RBSI_RGBGEN;
          }
          else
            m_RP.m_pCurFuncs->ERGB_Noise(sfm->m_RGBNoise, color);
        }
      }
      break;

    case eERGB_Object:
      if (m_RP.m_pRE)
      {
        if (!(m_RP.m_FlagsPerFlush & RBSI_RGBGEN))
        {
          bSetCol = true;
          color.bcolor[0] = (byte)(m_RP.m_pCurObject->m_Color[0] * 255.0f);
          color.bcolor[1] = (byte)(m_RP.m_pCurObject->m_Color[1] * 255.0f);
          color.bcolor[2] = (byte)(m_RP.m_pCurObject->m_Color[2] * 255.0f);
          m_RP.m_FlagsPerFlush |= RBSI_RGBGEN;
        }
        else
          m_RP.m_pCurFuncs->ERGB_Object();
      }
      break;

    case eERGB_OneMinusObject:
      if (m_RP.m_pRE)
      {
        if (!(m_RP.m_FlagsPerFlush & RBSI_RGBGEN))
        {
          bSetCol = true;
          color.bcolor[0] = (byte)((1.0f - m_RP.m_pCurObject->m_Color[0]) * 255.0f);
          color.bcolor[1] = (byte)((1.0f - m_RP.m_pCurObject->m_Color[1]) * 255.0f);
          color.bcolor[2] = (byte)((1.0f - m_RP.m_pCurObject->m_Color[2]) * 255.0f);
          m_RP.m_FlagsPerFlush |= RBSI_RGBGEN;
        }
      }
      else
        m_RP.m_pCurFuncs->ERGB_OneMinusObject();
      break;

    case eERGB_RE:
      if (m_RP.m_pRE && !(m_RP.m_FlagsPerFlush & RBSI_RGBGEN))
      {
        bSetCol = true;
        color.bcolor[0] = (byte)(m_RP.m_pRE->m_Color[0] * 255.0f);
        color.bcolor[1] = (byte)(m_RP.m_pRE->m_Color[1] * 255.0f);
        color.bcolor[2] = (byte)(m_RP.m_pRE->m_Color[2] * 255.0f);
        m_RP.m_FlagsPerFlush |= RBSI_RGBGEN;
      }
      break;
      
    case eERGB_OneMinusRE:
      if (m_RP.m_pRE && !(m_RP.m_FlagsPerFlush & RBSI_RGBGEN))
      {
        bSetCol = true;
        color.bcolor[0] = (byte)((1.0f - m_RP.m_pRE->m_Color[0]) * 255.0f);
        color.bcolor[1] = (byte)((1.0f - m_RP.m_pRE->m_Color[1]) * 255.0f);
        color.bcolor[2] = (byte)((1.0f - m_RP.m_pRE->m_Color[2]) * 255.0f);
        m_RP.m_FlagsPerFlush |= RBSI_RGBGEN;
      }
      break;

    case eERGB_World:
      if (m_RP.m_pRE && !(m_RP.m_FlagsPerFlush & RBSI_RGBGEN))
      {
        bSetCol = true;
        color.bcolor[0] = (byte)(m_WorldColor[0] * 255.0f);
        color.bcolor[1] = (byte)(m_WorldColor[1] * 255.0f);
        color.bcolor[2] = (byte)(m_WorldColor[2] * 255.0f);
        m_RP.m_FlagsPerFlush |= RBSI_RGBGEN;
      }
      break;

    default:
      assert(0);
  }

  switch(sfm->m_eEvalAlpha)
  {
    case eEALPHA_NoFill:
      break;

    case eEALPHA_Identity:
      if (sfm->m_eEvalRGB!=eERGB_Identity && sfm->m_eEvalRGB!=eERGB_Fixed)
      {
        if (m_RP.m_pRE)
        {
          if (!(m_RP.m_FlagsPerFlush & RBSI_RGBGEN))
          {
            color.bcolor[3] = 255;
            bSetCol = true;
            m_RP.m_FlagsPerFlush |= RBSI_RGBGEN;
          }
        }
        else
        {
          byte *ptr = m_RP.m_Ptr.PtrB+m_RP.m_OffsD;
          for (n=0; n<m_RP.m_RendNumVerts; n++, ptr+=m_RP.m_Stride)
          {
            ptr[3] = 255;
          }
        }
      }
      break;

    case eEALPHA_Fixed:
      {
        if (sfm->m_eEvalRGB == eERGB_Fixed)
          break;
        if (!(m_RP.m_FlagsPerFlush & RBSI_ALPHAGEN))
        {
          color.bcolor[3] = sfm->m_FixedColor.bcolor[3];
          bSetCol = true;
          m_RP.m_FlagsPerFlush |= RBSI_ALPHAGEN;
        }
      }
      break;

    case eEALPHA_Style:
      {
        CLightStyle *ls = CLightStyle::mfGetStyle(sfm->m_Style, m_RP.m_RealTime);
        color.bcolor[3] = (byte)((float)sfm->m_FixedColor.bcolor[3] * ls->m_fIntensity);
        bSetCol = true;
        m_RP.m_FlagsPerFlush |= RBSI_ALPHAGEN;
      }
      break;

    case eEALPHA_Comps:
      {
        if (sfm->m_eEvalRGB == eERGB_Comps)
          break;
        if (sfm->m_RGBComps)
        {
          float *vals = sfm->m_RGBComps->mfGet();
          if (m_RP.m_pRE)
          {
            if (!(m_RP.m_FlagsPerFlush & RBSI_ALPHAGEN))
            {
              color.bcolor[3] = (byte)(vals[0] * 255.0f);
              bSetCol = true;
              m_RP.m_FlagsPerFlush = RBSI_ALPHAGEN;
            }
          }
          else
          {
            byte a = (byte)(vals[0] * 255.0f);
            byte *ptr = m_RP.m_Ptr.PtrB+m_RP.m_OffsD;
            for (n=0; n<m_RP.m_RendNumVerts; n++, ptr+=m_RP.m_Stride)
            {
              ptr[3] = a;
            }
          }
        }
      }
      break;

    case eEALPHA_Wave:
      if (sfm->m_WaveEvalAlpha)
      {
        if (m_RP.m_pRE)
        {
          if (!(m_RP.m_FlagsPerFlush & RBSI_ALPHAGEN))
          {
            float val = SEvalFuncs::EvalWaveForm(sfm->m_WaveEvalAlpha);
            if (val < 0)
              val = 0;
            if (val > 1)
              val = 1;
            
            color.bcolor[3] = (int)(val * 255.0f);
            bSetCol = true;
            m_RP.m_FlagsPerFlush = RBSI_ALPHAGEN;
          }
        }
        else
          m_RP.m_pCurFuncs->EALPHA_Wave(sfm->m_WaveEvalAlpha, color);
      }
      break;

    case eEALPHA_Noise:
      if (sfm->m_ANoise)
      {
        if (m_RP.m_pRE)
        {
          if (!(m_RP.m_FlagsPerFlush & RBSI_ALPHAGEN))
          {
            float v = RandomNum();
            byte a = (byte)(CLAMP(v * sfm->m_ANoise->m_RangeA + sfm->m_ANoise->m_ConstA, 0.0f, 1.0f) * 255.0f);
            
            color.bcolor[3] = a;
            bSetCol = true;
            m_RP.m_FlagsPerFlush |= RBSI_ALPHAGEN;
          }
        }
        else
          m_RP.m_pCurFuncs->EALPHA_Noise(sfm->m_ANoise, color);
      }
      break;

    case eEALPHA_Beam:
      m_RP.m_pCurFuncs->EALPHA_Beam();
      break;

    case eEALPHA_Object:
      if (m_RP.m_pRE)
      {
        if (!(m_RP.m_FlagsPerFlush & RBSI_ALPHAGEN))
        {
          bSetCol = true;
          color.bcolor[3] = (byte)(m_RP.m_pCurObject->m_Color[3] * 255.0f);
          m_RP.m_FlagsPerFlush |= RBSI_ALPHAGEN;
        }
      }
      else
        m_RP.m_pCurFuncs->EALPHA_Object();
      break;

    case eEALPHA_OneMinusObject:
      if (m_RP.m_pRE)
      {
        if (!(m_RP.m_FlagsPerFlush & RBSI_ALPHAGEN))
        {
          bSetCol = true;
          color.bcolor[3] = (byte)((1.0f - m_RP.m_pCurObject->m_Color[3]) * 255.0f);
          m_RP.m_FlagsPerFlush |= RBSI_ALPHAGEN;
        }
      }
      else
        m_RP.m_pCurFuncs->EALPHA_OneMinusObject();
      break;

    case eEALPHA_RE:
      if (m_RP.m_pRE)
      {
        if (!(m_RP.m_FlagsPerFlush & RBSI_ALPHAGEN))
        {
          bSetCol = true;
          color.bcolor[3] = (byte)(m_RP.m_pRE->m_Color[3] * 255.0f);
          m_RP.m_FlagsPerFlush |= RBSI_ALPHAGEN;
        }
      }
      break;
      
    case eEALPHA_OneMinusRE:
      if (m_RP.m_pRE)
      {
        if (!(m_RP.m_FlagsPerFlush & RBSI_ALPHAGEN))
        {
          bSetCol = true;
          color.bcolor[3] = (byte)((1.0f - m_RP.m_pCurObject->m_Color[3]) * 255.0f);
          m_RP.m_FlagsPerFlush |= RBSI_ALPHAGEN;
        }
      }
      break;

    case eEALPHA_World:
      if (m_RP.m_pRE)
      {
        if (!(m_RP.m_FlagsPerFlush & RBSI_ALPHAGEN))
        {
          bSetCol = true;
          color.bcolor[3] = (byte)(m_WorldColor[3] * 255.0f);
          m_RP.m_FlagsPerFlush |= RBSI_ALPHAGEN;
        }
      }
      break;

    case eEALPHA_FromClient:
      if (!m_RP.m_pRE)
      {
        if (sfm->m_eEvalRGB!=eERGB_FromClient)
        {
          byte *ptr = m_RP.m_Ptr.PtrB+m_RP.m_OffsD;
          for (n=0; n<m_RP.m_RendNumVerts; n++, ptr+=m_RP.m_Stride)
          {
            ptr[3] = m_RP.m_pClientColors[n][3];
          }
        }
      }
      break;

    default:
      assert(0);
  }

  if (bSetCol)
  {
    m_RP.m_NeedGlobalColor = color;
    m_LastRGBAGenSetGlobal = true;
  }
}

void CNULLRenderer::EF_EvalNormalsRB(SShader *ef)
{
}

//=================================================================================

void CNULLRenderer::PS2SetCull(ECull eCull)
{ 
  m_RP.m_eCull = eCull;
}


void CRenderer::EF_SetState(int st)
{
  // These transitions precede the changed-state early exit in OpenGL.
  // Auxiliary passes consume WASDEPTHWRITE even if the main pass repeats
  // the state already installed by the previous draw.
  if (m_RP.m_Flags & RBF_SHOWLINES)
    st |= GS_NODEPTHTEST;
  if ((st & (GS_DEPTHWRITE | GS_DEPTHFUNC_EQUAL)) &&
      !(m_RP.m_FlagsPerFlush & RBSI_ALPHABLEND))
    m_RP.m_FlagsPerFlush |= RBSI_WASDEPTHWRITE;
  if ((st & GS_DEPTHWRITE) && m_RP.m_LastVP && m_RP.m_pRE)
    m_RP.m_pRE->m_LastVP = m_RP.m_LastVP;
  // Match the state-mask resolution performed by the OpenGL renderer. Shader
  // passes use these masks to inherit state selected earlier in the current
  // flush; leaving the raw sentinels in m_CurState makes Vulkan reject the
  // state or select a different immutable pipeline.
  const int changed = st ^ m_CurState;
  if ((changed & (GS_DEPTHFUNC_EQUAL | GS_DEPTHFUNC_GREAT)) &&
      (m_RP.m_FlagsPerFlush & RBSI_DEPTHFUNC))
  {
    const int mask = GS_DEPTHFUNC_EQUAL | GS_DEPTHFUNC_GREAT;
    st = (st & ~mask) | (m_CurState & mask);
  }
  if (changed & GS_BLEND_MASK)
  {
    if ((st & GS_BLEND_MASK) == GS_BLEND_MASK ||
        (m_RP.m_FlagsPerFlush & RBSI_ALPHABLEND))
      st = (st & ~GS_BLEND_MASK) | (m_CurState & GS_BLEND_MASK);
  }
  if ((changed & GS_DEPTHWRITE) && (m_RP.m_FlagsPerFlush & RBSI_DEPTHWRITE))
    st = (st & ~GS_DEPTHWRITE) | (m_CurState & GS_DEPTHWRITE);
  if ((changed & GS_NODEPTHTEST) && (m_RP.m_FlagsPerFlush & RBSI_DEPTHTEST))
    st = (st & ~GS_NODEPTHTEST) | (m_CurState & GS_NODEPTHTEST);
  if ((changed & GS_STENCIL) &&
      ((m_RP.m_FlagsPerFlush & RBSI_STENCIL) ||
       (m_RP.m_PersFlags & RBPF_MEASUREOVERDRAW)))
    st = (st & ~GS_STENCIL) | (m_CurState & GS_STENCIL);
  if ((changed & GS_ALPHATEST_MASK) && (m_RP.m_FlagsPerFlush & RBSI_ALPHATEST))
    st = (st & ~GS_ALPHATEST_MASK) | (m_CurState & GS_ALPHATEST_MASK);
  m_CurState = st;
}

void CNULLRenderer::EF_SetColorOp(byte co)
{
}

//=================================================================================

DEFINE_ALIGNED_DATA_STATIC( Matrix44, sIdentityMatrix( 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 ), 16 ); 

// Get inverted matrix of the object matrix
// All matrices are 16 bytes alligned to speedup matrix calculations using SSE instructions
Matrix44 &CCObject::GetInvMatrix()
{
  // Vulkan's identity render objects can retain an inverse-cache index from
  // a previous frame, while the shared matrix array has been recycled.
  // With no object transform, object space is world space regardless of
  // that index. Never interpret another object's cached inverse as terrain's.
  if (gRenDev && gRenDev->GetType() == R_VULKAN_RENDERER &&
      !(m_ObjFlags & FOB_TRANS_MASK))
    return sIdentityMatrix;
  if (m_InvMatrixId == 0)
    return sIdentityMatrix;
  if (m_InvMatrixId > 0)
    return m_ObjMatrices[m_InvMatrixId];

  //PROFILE_FRAME(Objects_ObjInvTransform);

  int n = m_ObjMatrices.size();
  m_ObjMatrices.resize(n+1);
  m_InvMatrixId = n;

  CRenderer *rd = gRenDev;
  Matrix44 &m = m_ObjMatrices[m_InvMatrixId];

  if (m_ObjFlags & FOB_TRANS_ROTATE)
  {
    mathMatrixInverse(m.GetData(), m_Matrix.GetData(), g_CpuFlags);
  }
  else
  if (m_ObjFlags & FOB_TRANS_SCALE)
  {
    float fiScaleX = 1.0f / m_Matrix(0,0);
    float fiScaleY = 1.0f / m_Matrix(1,1);
    float fiScaleZ = 1.0f / m_Matrix(2,2);
    m(0,0) = fiScaleX;
    m(0,1) = m_Matrix(0,1);
    m(0,2) = m_Matrix(0,2);
    m(0,3) = m_Matrix(0,3);

    m(1,0) = m_Matrix(1,0);
    m(1,1) = fiScaleY;
    m(1,2) = m_Matrix(1,2);
    m(1,3) = m_Matrix(1,3);

    m(2,0) = m_Matrix(2,0);
    m(2,1) = m_Matrix(2,1);
    m(2,2) = fiScaleZ;
    m(2,3) = m_Matrix(2,3);

    m(3,0) = -m_Matrix(3,0) * fiScaleX;
    m(3,1) = -m_Matrix(3,1) * fiScaleY;
    m(3,2) = -m_Matrix(3,2) * fiScaleZ;
    m(3,3) = m_Matrix(3,3);
  }
  else
  if (m_ObjFlags & FOB_TRANS_TRANSLATE)
  {
    m(0,0) = m_Matrix(0,0);
    m(0,1) = m_Matrix(0,1);
    m(0,2) = m_Matrix(0,2);
    m(0,3) = m_Matrix(0,3);

    m(1,0) = m_Matrix(1,0);
    m(1,1) = m_Matrix(1,1);
    m(1,2) = m_Matrix(1,2);
    m(1,3) = m_Matrix(1,3);

    m(2,0) = m_Matrix(2,0);
    m(2,1) = m_Matrix(2,1);
    m(2,2) = m_Matrix(2,2);
    m(2,3) = m_Matrix(2,3);

    m(3,0) = -m_Matrix(3,0);
    m(3,1) = -m_Matrix(3,1);
    m(3,2) = -m_Matrix(3,2);
    m(3,3) = m_Matrix(3,3);
  }
  else
    m.SetIdentity();

  return m;
}


// Same object/camera-frame cache as the OpenGL shader pipeline.
Matrix44 &CCObject::GetVPMatrix()
{
  CRenderer *rd = gRenDev;
  if (m_VPMatrixId == 0)
    return rd->m_CameraProjMatrix;
  if (m_VPMatrixId > 0 && m_VPMatrixFrame == rd->m_RP.m_TransformFrame)
    return m_ObjMatrices[m_VPMatrixId];
  m_VPMatrixFrame = rd->m_RP.m_TransformFrame;

  int n = m_ObjMatrices.size();
  m_ObjMatrices.resize(n+1);
  m_VPMatrixId = n;

  Matrix44 &m = m_ObjMatrices[m_VPMatrixId];

  mathMatrixMultiply(m.GetData(), rd->m_CameraProjMatrix.GetData(), m_Matrix.GetData(), g_CpuFlags);
  //D3DXMatrixMultiplyTranspose((D3DXMATRIX *)m.GetData(), (D3DXMATRIX *)m_Matrix.GetData(), (D3DXMATRIX *)rd->m_CameraProjMatrix.GetData());

  return m;
}

bool CNULLRenderer::EF_ObjectChange(SShader *Shader, int nObject, CRendElement *pRE)
{
  return true;
}


// Initialize of the new shader pipeline (only 2d)
void CNULLRenderer::EF_Start(SShader *ef, SShader *efState, SRenderShaderResources *Res, CRendElement *re) 
{
  m_RP.m_Frame++;
}

// Initialize of the new shader pipeline
void CNULLRenderer::EF_Start(SShader *ef, SShader *efState, SRenderShaderResources *Res, int numFog, CRendElement *re)
{
  m_RP.m_Frame++;
}

void CNULLRenderer::EF_CheckOverflow(int nVerts, int nInds, CRendElement *re)
{
}

//========================================================================================

void CNULLRenderer::EF_LightMaterial(SLightMaterial *lm, int Flags)
{
}

//===================================================================================================

// Used for HW effectors for rendering of tri mesh (vertex array)
void CNULLRenderer::EF_DrawIndexedMesh (int nPrimType)
{
}


void CNULLRenderer::EF_FlushShader()
{
}

void CNULLRenderer::EF_Flush()
{
}

void CNULLRenderer::EF_EndEf3D(int nFlags)
{
  m_RP.m_RealTime = iTimer->GetCurrTime();
  EF_RemovePolysFromScene();
  SRendItem::m_RecurseLevel--;
}

void CNULLRenderer::EF_RenderPipeLine(void (*RenderFunc)())
{
}

void CNULLRenderer::EF_PipeLine(int nums, int nume, int nList, int nSortType, void (*RenderFunc)())
{
}

void CNULLRenderer::EF_DrawWire()
{
}

void CNULLRenderer::EF_DrawNormals()
{
}

void CNULLRenderer::EF_DrawTangents()
{
}

void CNULLRenderer::EF_DrawDebugLights()
{
}

void CNULLRenderer::EF_DrawDebugTools()
{
}


void CNULLRenderer::EF_PrintProfileInfo()
{
}

int CNULLRenderer::EF_Preprocess(SRendItemPre *ri, int nums, int nume)
{
  return 0;
}

//double timeFtoI, timeFtoL, timeQRound;
//int sSome;
void CNULLRenderer::EF_EndEf2D(bool bSort)
{
}
