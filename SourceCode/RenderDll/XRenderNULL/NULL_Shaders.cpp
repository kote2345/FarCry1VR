/*=============================================================================
  PS2_Shaders.cpp : PS2 specific effectors/shaders functions implementation.
  Copyright 2001 Crytek Studios. All Rights Reserved.

  Revision history:
    * Created by Honitch Andrey

=============================================================================*/

#include "RenderPCH.h"
#include "NULL_Renderer.h"
#include "I3DEngine.h"


#undef THIS_FILE
static char THIS_FILE[] = __FILE__;

//============================================================================


void CShader::mfCompileVarsPak(char *scr, TArray<CVarCond>& Vars, SShader *ef)
{
  char *var;
  char *val;

  while ((shGetVar (&scr, &var, &val)) >= 0)
  {
    if (!var)
      continue;

    ICVar *vr = iConsole->GetCVar(var);
    if (!vr)
    {
      iLog->Log("Warning: Couldn't find console variable '%s' in shader '%s'\n", var, ef->m_Name.c_str());
      continue;
    }
    float v = shGetFloat(val);
    CVarCond vc;
    vc.m_Var = vr;
    vc.m_Val = v;
    Vars.AddElem(vc);
  }
}

bool CShader::mfCompileHWShadeLayer(SShader *ef, char *scr, TArray<SShaderPassHW>& Layers)
{
  if (!scr)
    return false;
  const int index = Layers.Num();
  Layers.ReserveNew(index + 1);
  SShaderPassHW *pass = &Layers[index];
  pass->m_RenderState = GS_DEPTHWRITE;
  enum { eLayer = 1, eLightType, eNoLights, eNoBump, eVertexLight, ePolyOffset,
         eNoAmbient, eNoSpecular, eNoAddSpecular, eOnlyMaterialAmbient,
         eIgnoreLights, eIgnoreProjectors, eNoAlpha, eRendState,
         eSecondPassRendState, eOcclusionMap, eBump, eDivideAmb4,
         eDivideAmb2, eDivideDif4, eDivideDif2, eColorMaterial,
         eHasAmbient, eHasDOT3LM, eAmbMaxLights,
         eSamples1, eSamples2, eSamples3, eSamples4, eArray, eMatrix,
         eCGVProgram, eCGPShader, eCGVPParam, eCGPSParam, eCGPSParmRect,
         eDeformVertexes, eAffectMask };
  static tokenDesc commands[] =
  {
    {eLayer, "Layer"}, {eLightType, "LightType"}, {eNoLights, "NoLight"},
    {eNoBump, "NoBump"}, {eVertexLight, "LMVertexLight"},
    {ePolyOffset, "PolyOffset"}, {eNoAmbient, "LMNoAmbient"},
    {eNoSpecular, "LMNoSpecular"}, {eNoAddSpecular, "LMNoAddSpecular"},
    {eOnlyMaterialAmbient, "LMOnlyMaterialAmbient"},
    {eIgnoreLights, "LMIgnoreLights"}, {eIgnoreProjectors, "LMIgnoreProjLights"},
    {eNoAlpha, "LMNoAlpha"}, {eBump, "LMBump"},
    {eDivideAmb4, "LMDivideAmb4"}, {eDivideAmb2, "LMDivideAmb2"},
    {eDivideDif4, "LMDivideDif4"}, {eDivideDif2, "LMDivideDif2"},
    {eColorMaterial, "ColorMaterial"}, {eSamples1, "1Samples"},
    {eSamples2, "2Samples"}, {eSamples3, "3Samples"}, {eSamples4, "4Samples"},
    {eHasAmbient, "HasAmbient"}, {eHasDOT3LM, "HasDOT3LM"},
    {eAmbMaxLights, "AmbMaxLights"},
    {eArray, "Array"}, {eMatrix, "Matrix"}, {eRendState, "RendState"},
    {eSecondPassRendState, "SecondPassRendState"},
    {eOcclusionMap, "OcclusionMap"},
    {eCGVProgram, "CGVProgram"}, {eCGPShader, "CGPShader"},
    {eCGVPParam, "CGVPParam"}, {eCGPSParam, "CGPSParam"},
    {eCGPSParmRect, "CGPSParmRect"},
    {eDeformVertexes, "DeformVertexes"}, {eAffectMask, "AffectMask"},
    {0, 0}
  };
  char *name = NULL, *params = NULL, *data = NULL;
  bool hasVertexProgram = false;
  bool vertexLight = false;
  bool parametersOnly = false;
  long command;
  while ((command = shGetObject(&scr, commands, &name, &params)) > 0)
  {
    // Full CG pass translation is not implemented for all stock techniques.
    // Keep their established fallback until each program is translated;
    // surface decals have a verified fixed combiner path and need their
    // Layer/Blend statements after CGVProgram.
    if (command >= eCGVProgram && ef->m_eSort != eS_Decal)
      parametersOnly = true;
    // Keep the same expanded program mask OpenGL uses at the declaration.
    // Object LM IDs alone cannot identify a shader's lightmap variant.
    if (command == eCGVProgram || command == eCGPShader)
    {
      const char* program = name ? name : params;
      char* target = command == eCGVProgram ? pass->m_StockVertexProgram :
                                            pass->m_StockFragmentProgram;
      if (program) snprintf(target, 96, "%s", program);
      if (command == eCGPShader)
      {
        // Programs without generated variants need no macro snapshot (GL
        // also omits the lookup when AffectMask is zero). Such declarations
        // can legitimately be absent from m_LocalMacros.
        const int offset = static_cast<int>(pCurCommand - m_pCurScript);
        for (int macro = 0; macro < m_LocalMacros.Num(); ++macro)
          if (m_LocalMacros[macro].m_nOffset == offset)
          {
            pass->m_StockProgramMask = mfScriptPreprocessorMask(ef, offset);
            const ShaderMacro& macros = *m_LocalMacros[macro].m_Macros;
            ShaderMacro::const_iterator colors = macros.find("%VERTCOLORS");
            pass->m_StockUsesVertexColors = colors != macros.end() &&
                strtoull(colors->second.c_str(), nullptr, 0) != 0;
            break;
          }
      }
    }
    // Retain CG pixel parameters without changing the established fallback
    // state/stream metadata of untranslated programs.
    const bool translatedLayers = !stricmp(pass->m_StockFragmentProgram, "CGRCAmbient_Decal") ||
      !stricmp(pass->m_StockFragmentProgram, "CGRCPlants") ||
      !stricmp(pass->m_StockFragmentProgram, "CGRCPlants_Bump") ||
      strstr(pass->m_StockFragmentProgram, "_Particle") != nullptr ||
      !stricmp(pass->m_StockFragmentProgram, "CGRCCaust") ||
      !stricmp(pass->m_StockFragmentProgram, "CGRCFog") ||
      !stricmp(pass->m_StockFragmentProgram, "CGRCTerrainLayerTempl") ||
      !stricmp(pass->m_StockFragmentProgram, "CGRCTerrainShadow") ||
      !strnicmp(pass->m_StockFragmentProgram, "CGRCTerrain", 11) ||
      !stricmp(ef->GetName(), "WaterVolume") ||
      !strnicmp(ef->GetName(), "TerrainWaterBottom", 18) ||
      !strnicmp(pass->m_StockFragmentProgram, "CGRCOcean", 9) ||
      !stricmp(pass->m_StockFragmentProgram, "CGRCWater") ||
      !stricmp(pass->m_StockFragmentProgram, "CGRCWater_Beach_Refr") ||
      !stricmp(pass->m_StockFragmentProgram, "CGRCWater_Beach") ||
      !strnicmp(pass->m_StockVertexProgram, "CGVProgWater_Beach_Shift", 25);
    if (parametersOnly && !translatedLayers && command != eCGPSParam &&
        command != eCGPSParmRect && command != eCGVPParam)
      continue;
    data = name ? name : params;
    switch (command)
    {
      // shGetObject returns zero on an unknown token, ending this loop.
      // Consume the complete GL grammar even though Vulkan translates the
      // programs itself; otherwise CGVProgram silently discards Layer/Blend.
      case eCGVProgram:
        hasVertexProgram = true;
        break;
      case eCGPShader:
      case eAffectMask:
        break;
      case eCGVPParam:
        mfCompileCGParam(params, ef, &pass->m_VPParamsNoObj);
        break;
      case eCGPSParam:
      case eCGPSParmRect:
      {
        if (!pass->m_CGFSParamsNoObj)
          pass->m_CGFSParamsNoObj = new TArray<SCGParam4f>;
        const int first = pass->m_CGFSParamsNoObj->Num();
        mfCompileCGParam(params, ef, pass->m_CGFSParamsNoObj);
        if (command == eCGPSParmRect)
          for (int i = first; i < pass->m_CGFSParamsNoObj->Num(); ++i)
            pass->m_CGFSParamsNoObj->Get(i).m_dwBind |= 0x80000;
        break;
      }
      case eDeformVertexes:
      {
        if (!pass->m_Deforms) pass->m_Deforms = new TArray<SDeform>;
        const int index = pass->m_Deforms->Num();
        pass->m_Deforms->ReserveNew(index + 1);
        mfCompileDeform(ef, &pass->m_Deforms->Get(index), name, params);
        break;
      }
      case eLayer:
        mfCompileLayer(ef, name ? atoi(name) : 0, params, pass);
        break;
      case eRendState:
        pass->m_RenderState = mfCompileRendState(ef, pass, params);
        break;
      case eSecondPassRendState:
        pass->m_SecondRenderState = mfCompileRendState(ef, pass, params);
        pass->m_Flags |= SHPF_USEDSECONDRS;
        break;
      case eLightType:
        if (data && !strnicmp(data, "Direct", 6)) pass->m_LightFlags |= DLF_DIRECTIONAL;
        else if (data && !strnicmp(data, "Point", 5)) pass->m_LightFlags |= DLF_POINT;
        else if (data && !strnicmp(data, "Project", 7))
        {
          ef->m_Flags |= EF_USEPROJLIGHTS;
          pass->m_LightFlags |= DLF_PROJECT;
        }
        else if (data && !strnicmp(data, "OnlySpec", 8)) pass->m_LightFlags |= DLF_LM;
        break;
      case eNoLights: pass->m_LMFlags |= LMF_DISABLE; break;
      case eNoBump: pass->m_LMFlags |= LMF_NOBUMP; break;
      case eVertexLight: vertexLight = true; break;
      case ePolyOffset: pass->m_LMFlags |= LMF_POLYOFFSET; break;
      case eNoAmbient: pass->m_LMFlags |= LMF_NOAMBIENT; break;
      case eNoSpecular: pass->m_LMFlags |= LMF_NOSPECULAR; break;
      case eNoAddSpecular: pass->m_LMFlags |= LMF_NOADDSPECULAR; break;
      case eOnlyMaterialAmbient: pass->m_LMFlags |= LMF_ONLYMATERIALAMBIENT; break;
      case eIgnoreLights: pass->m_LMFlags |= LMF_IGNORELIGHTS; break;
      case eIgnoreProjectors: pass->m_LMFlags |= LMF_IGNOREPROJLIGHTS; break;
      case eNoAlpha: pass->m_LMFlags |= LMF_NOALPHA; break;
      case eOcclusionMap: pass->m_LMFlags |= LMF_USEOCCLUSIONMAP; break;
      case eBump: pass->m_LMFlags |= LMF_BUMPMATERIAL; break;
      case eDivideAmb4: pass->m_LMFlags |= LMF_DIVIDEAMB4; break;
      case eDivideAmb2: pass->m_LMFlags |= LMF_DIVIDEAMB2; break;
      case eDivideDif4: pass->m_LMFlags |= LMF_DIVIDEDIFF4; break;
      case eDivideDif2: pass->m_LMFlags |= LMF_DIVIDEDIFF2; break;
      case eColorMaterial: pass->m_LMFlags |= LMF_COLMAT_AMB; break;
      case eSamples1: pass->m_LMFlags |= LMF_1SAMPLES; break;
      case eSamples2: pass->m_LMFlags |= LMF_2SAMPLES; break;
      case eSamples3: pass->m_LMFlags |= LMF_3SAMPLES; break;
      case eSamples4: pass->m_LMFlags |= LMF_4SAMPLES; break;
      // Keep the NULL-backed Vulkan renderer's technique metadata in sync
      // with the OpenGL/D3D shader parsers. These flags select the ambient
      // lightmap path and gate directional-lightmap rendering at runtime.
      case eHasAmbient: pass->m_LMFlags |= LMF_HASAMBIENT; break;
      case eHasDOT3LM: pass->m_LMFlags |= LMF_HASDOT3LM; break;
      case eAmbMaxLights:
        pass->m_nAmbMaxLights = shGetInt(data);
        break;
      case eArray: mfCompileArrayPointer(pass->m_Pointers, params, ef); break;
      case eMatrix:
        if (!pass->m_MatrixOps)
          pass->m_MatrixOps = new TArray<SMatrixTransform>;
        mfCompileMatrixOp(pass->m_MatrixOps, params, name, ef);
        break;
    }
  }
  // Match GL's distinction between shader-managed bump lighting and
  // LMVertexLight/fixed-function material lighting.
  if (hasVertexProgram && !vertexLight) pass->m_LMFlags |= LMF_BUMPMATERIAL;
  // GLShaders.cpp applies this template default after parsing the pass.
  // A zero second state overwrites earlier lighting instead of adding to it.
  if (ef->m_Flags & EF_TEMPLNAMES)
    pass->m_SecondRenderState = GS_BLSRC_ONE | GS_BLDST_ONE | GS_DEPTHFUNC_EQUAL;
  mfCheckObjectDependParams(&pass->m_VPParamsNoObj, &pass->m_VPParamsObj);
  if (pass->m_CGFSParamsNoObj)
  {
    TArray<SCGParam4f>* objectParams = new TArray<SCGParam4f>;
    mfCheckObjectDependParams(pass->m_CGFSParamsNoObj, objectParams);
    if (objectParams->Num()) pass->m_CGFSParamsObj = objectParams;
    else delete objectParams;
  }
  return true;
}

void CShader::mfCompileLayers(SShader *ef, char *scr, TArray<SShaderPassHW>& Layers, EShaderPassType eType)
{
  enum { eShadeLayer = 1, ePass };
  static tokenDesc commands[] = {{eShadeLayer, "ShadeLayer"}, {ePass, "Pass"}, {0, 0}};
  if (eType == eSHP_DiffuseLight || eType == eSHP_SpecularLight)
    ef->m_Flags |= EF_USELIGHTS;
  char *name = NULL, *params = NULL;
  long command;
  while ((command = shGetObject(&scr, commands, &name, &params)) > 0)
  {
    if (command != eShadeLayer && command != ePass)
      continue;
    const int index = Layers.Num();
    if (!mfCompileHWShadeLayer(ef, params, Layers))
      continue;
    Layers[index].m_ePassType = eType;
    Layers[index].m_LightFlags |= DLF_ACTIVE;
  }
}

void CShader::mfCompileHWConditions(SShader *ef, char *scr, SShaderTechnique *hs, int Id)
{
  if (Id < 0 || !scr)
    return;
  ef->m_HWConditions.Expand(Id + 1);
  SHWConditions *conditions = &ef->m_HWConditions[Id];
  enum { eSingleLight = 1, eMultipleLights, eNoLights, eOnlyDirectional,
         eProjected, eInShadow, eSpecular, eBended, eAlphaBlended,
         eNoBump, eHasLM, eHasDot3LM, eHasVColors, eAlphaTest,
         eHasAlphaBlend, eHeatVision, eHotAmbient, eFogVolume,
         eHasEnvLCMap, eHasResource, eVars, eRETexBind1, eRETexBind2,
         eRETexBind3, eRETexBind4, eRETexBind5, eRETexBind6, eRETexBind7 };
  static tokenDesc commands[] =
  {
    {eMultipleLights, "MultipleLights"}, {eSingleLight, "SingleLight"},
    {eNoLights, "NoLights"}, {eOnlyDirectional, "OnlyDirectional"},
    {eProjected, "HasProjectedLights"}, {eInShadow, "InShadow"},
    {eSpecular, "Specular"}, {eBended, "Bended"}, {eAlphaBlended, "AlphaBlended"},
    {eNoBump, "NoBump"}, {eHasLM, "HasLM"}, {eHasDot3LM, "HasDOT3LM"},
    {eHasVColors, "HasVColors"}, {eAlphaTest, "HasAlphaTest"},
    {eHasAlphaBlend, "HasAlphaBlend"}, {eHeatVision, "HeatVision"},
    {eHotAmbient, "HotAmbient"}, {eFogVolume, "InFogVolume"},
    {eHasEnvLCMap, "HasEnvLCMap"}, {eHasResource, "HasResource"},
    {eVars, "Vars"}, {eRETexBind1, "RETexBind1"}, {eRETexBind2, "RETexBind2"},
    {eRETexBind3, "RETexBind3"}, {eRETexBind4, "RETexBind4"},
    {eRETexBind5, "RETexBind5"}, {eRETexBind6, "RETexBind6"},
    {eRETexBind7, "RETexBind7"}, {0, 0}
  };
  char *name = NULL, *params = NULL;
  long command;
  while ((command = shGetObject(&scr, commands, &name, &params)) > 0)
  {
    switch (command)
    {
      case eSingleLight: conditions->m_Flags |= SHCF_SINGLELIGHT; break;
      case eMultipleLights: conditions->m_Flags |= SHCF_MULTIPLELIGHTS; break;
      case eNoLights: conditions->m_Flags |= SHCF_NOLIGHTS; break;
      case eOnlyDirectional: conditions->m_Flags |= SHCF_ONLYDIRECTIONAL; break;
      case eProjected: conditions->m_Flags |= SHCF_HASPROJECTEDLIGHTS; break;
      case eInShadow: conditions->m_Flags |= SHCF_INSHADOW; break;
      case eSpecular: conditions->m_Flags |= SHCF_SPECULAR; break;
      case eBended: conditions->m_Flags |= SHCF_BENDED; break;
      case eAlphaBlended: conditions->m_Flags |= SHCF_ALPHABLENDED; break;
      case eNoBump: conditions->m_Flags |= SHCF_NOBUMP; break;
      case eHasLM: conditions->m_Flags |= SHCF_HASLM; ef->m_Flags3 |= EF3_HASLM; break;
      case eHasDot3LM: conditions->m_Flags |= SHCF_HASDOT3LM; ef->m_Flags3 |= EF3_HASLM; break;
      case eHasVColors: conditions->m_Flags |= SHCF_HASVCOLORS; break;
      case eAlphaTest: conditions->m_Flags |= SHCF_HASALPHATEST; break;
      case eHasAlphaBlend: conditions->m_Flags |= SHCF_HASALPHABLEND; break;
      case eHeatVision: conditions->m_Flags |= SHCF_HEATVISION; break;
      case eHotAmbient: conditions->m_Flags |= SHCF_HOTAMBIENT; break;
      case eFogVolume: conditions->m_Flags |= SHCF_INFOGVOLUME; break;
      case eHasEnvLCMap: conditions->m_Flags |= SHCF_ENVLCMAP; break;
      case eHasResource: conditions->m_Flags |= SHCF_HASRESOURCE; break;
      case eRETexBind1: case eRETexBind2: case eRETexBind3: case eRETexBind4:
      case eRETexBind5: case eRETexBind6: case eRETexBind7:
        conditions->m_Flags |= (command - eRETexBind1 + 1) << 12;
        ef->m_Flags2 |= EF2_REDEPEND;
        break;
      case eVars:
      {
        TArray<CVarCond> vars;
        mfCompileVarsPak(params, vars, ef);
        if (vars.Num())
        {
          conditions->m_NumVars = vars.Num();
          conditions->m_Vars = new CVarCond[conditions->m_NumVars];
          memcpy(conditions->m_Vars, &vars[0], sizeof(CVarCond) * conditions->m_NumVars);
        }
        break;
      }
    }
  }
}

SShaderTechnique *CShader::mfCompileHW(SShader *ef, char *scr, int Id)
{
  if (!scr || !m_CurEfsNum)
    return NULL;
  SShaderTechnique *technique = new SShaderTechnique;
  enum { eCull = 1, eShadeLayer, ePass, eLight, eFirstLight, eConditions,
         eShadow, eNoMerge, eFur, eSimulatedFur, eMultiShadows, eMultiLights,
         eArray, eMatrix, eDeclareCGScript };
  static tokenDesc commands[] =
  {
    {eDeclareCGScript, "DeclareCGScript"}, {eShadeLayer, "ShadeLayer"},
    {ePass, "Pass"}, {eLight, "Light"}, {eMultiLights, "MultiLights"},
    {eFirstLight, "FirstLight"}, {eArray, "Array"}, {eCull, "Cull"},
    {eShadow, "Shadow"}, {eMatrix, "Matrix"}, {eConditions, "Conditions"},
    {eNoMerge, "NoMerge"}, {eFur, "Fur"}, {eSimulatedFur, "SimulatedFur"},
    {eMultiShadows, "MultiShadows"}, {0, 0}
  };
  char *name = NULL, *params = NULL, *data = NULL;
  long command;
  while ((command = shGetObject(&scr, commands, &name, &params)) > 0)
  {
    data = name ? name : params;
    switch (command)
    {
      case eCull:
        if (data && (!stricmp(data, "None") || !stricmp(data, "TwoSided") || !stricmp(data, "Disable")))
          technique->m_eCull = eCULL_None;
        else if (data && !strnicmp(data, "Back", 4)) technique->m_eCull = eCULL_Back;
        else if (data && !strnicmp(data, "Front", 5)) technique->m_eCull = eCULL_Front;
        if (data) ef->m_Flags |= EF_HASCULL;
        break;
      case eShadeLayer:
      case ePass:
        if (mfCompileHWShadeLayer(ef, params, technique->m_Passes))
          ef->m_Flags3 |= EF3_HASAMBPASSES;
        break;
      case eConditions:
        mfCompileHWConditions(ef, params, technique, Id);
        break;
      case eLight:
      case eFirstLight:
      {
        if (!technique->m_Passes.Num()) technique->m_Flags |= FHF_FIRSTLIGHT;
        const EShaderPassType passType = name && !strnicmp(name, "Spec", 4) ?
            eSHP_SpecularLight : eSHP_DiffuseLight;
        mfCompileLayers(ef, params, technique->m_Passes, passType);
        break;
      }
      case eMultiLights: mfCompileLayers(ef, params, technique->m_Passes, eSHP_MultiLights); break;
      case eShadow: mfCompileLayers(ef, params, technique->m_Passes, eSHP_Shadow); break;
      case eFur: mfCompileLayers(ef, params, technique->m_Passes, eSHP_Fur); break;
      case eSimulatedFur: mfCompileLayers(ef, params, technique->m_Passes, eSHP_SimulatedFur); break;
      case eMultiShadows:
      {
        const int index = technique->m_Passes.Num();
        technique->m_Passes.ReserveNew(index + 1);
        technique->m_Passes[index].m_ePassType = eSHP_MultiShadows;
        break;
      }
      case eArray: mfCompileArrayPointer(technique->m_Pointers, params, ef); break;
      case eNoMerge: technique->m_Flags |= FHF_NOMERGE; break;
      case eDeclareCGScript:
        break;
      case eMatrix:
        if (!technique->m_MatrixOps)
          technique->m_MatrixOps = new TArray<SMatrixTransform>;
        mfCompileMatrixOp(technique->m_MatrixOps, params, name, ef);
        break;
    }
  }
  return technique;
}

//===================================================================

//====================================================================

SGenTC *SGenTC_NormalMap::mfCopy()
{
  SGenTC_NormalMap *copy = new SGenTC_NormalMap;
  copy->m_Mask = m_Mask;
  copy->m_bDependsOnObject = m_bDependsOnObject;
  return copy;
}

bool SGenTC_NormalMap::mfSet(bool bEnable)
{
  return true;
}


void SGenTC_NormalMap::mfCompile(char *params, SShader *ef)
{
}

SGenTC *SGenTC_ReflectionMap::mfCopy()
{
  SGenTC_ReflectionMap *copy = new SGenTC_ReflectionMap;
  copy->m_Mask = m_Mask;
  copy->m_bDependsOnObject = m_bDependsOnObject;
  return copy;
}

bool SGenTC_ReflectionMap::mfSet(bool bEnable)
{
  return true;
}

void SGenTC_ReflectionMap::mfCompile(char *params, SShader *ef)
{
}

SGenTC *SGenTC_ObjectLinear::mfCopy()
{
  SGenTC_ObjectLinear *copy = new SGenTC_ObjectLinear;
  copy->m_Mask = m_Mask;
  copy->m_bDependsOnObject = m_bDependsOnObject;
  for (int i = 0; i < m_Params.Num(); ++i)
    copy->m_Params.AddElem(m_Params[i]);
  return copy;
}

bool SGenTC_ObjectLinear::mfSet(bool bEnable)
{
  return true;
}

void SGenTC_ObjectLinear::mfCompile(char *scr, SShader *ef)
{
  if (!scr)
    return;

  char* name;
  char *params;
  enum { eMask = 1, ePlaneS, ePlaneT, ePlaneR, ePlaneQ, eComponents };
  static tokenDesc commands[] = {
    {eMask, "Mask"}, {ePlaneS, "PlaneS"}, {ePlaneT, "PlaneT"},
    {ePlaneR, "PlaneR"}, {ePlaneQ, "PlaneQ"}, {eComponents, "Components"},
    {0, 0}
  };
  long command;
  while ((command = shGetObject(&scr, commands, &name, &params)) > 0)
  {
    char* data = name ? name : params;
    if (command == eMask)
      m_Mask = shGetInt(data);
    else if (data)
      gRenDev->m_cEF.mfCompileParam(data, ef, &m_Params);
  }

  for (int i = 0; i < m_Params.Num(); ++i)
    for (int component = 0; component < 4; ++component)
      if (m_Params[i].m_Comps[component] &&
          m_Params[i].m_Comps[component]->m_bDependsOnObject)
      {
        m_bDependsOnObject = true;
        return;
      }
}

SGenTC *SGenTC_EyeLinear::mfCopy()
{
  SGenTC_EyeLinear *copy = new SGenTC_EyeLinear;
  copy->m_Mask = m_Mask;
  copy->m_bDependsOnObject = m_bDependsOnObject;
  for (int i = 0; i < m_Params.Num(); ++i)
    copy->m_Params.AddElem(m_Params[i]);
  return copy;
}

bool SGenTC_EyeLinear::mfSet(bool bEnable)
{
  return true;
}

void SGenTC_EyeLinear::mfCompile(char *scr, SShader *ef)
{
  if (!scr)
    return;

  char* name;
  char *params;
  enum { eMask = 1, ePlaneS, ePlaneT, ePlaneR, ePlaneQ };
  static tokenDesc commands[] = {
    {eMask, "Mask"}, {ePlaneS, "PlaneS"}, {ePlaneT, "PlaneT"},
    {ePlaneR, "PlaneR"}, {ePlaneQ, "PlaneQ"}, {0, 0}
  };
  long command;
  while ((command = shGetObject(&scr, commands, &name, &params)) > 0)
  {
    char* data = name ? name : params;
    if (command == eMask)
      m_Mask = shGetInt(data);
    else if (data)
      gRenDev->m_cEF.mfCompileParam(data, ef, &m_Params);
  }

  for (int i = 0; i < m_Params.Num(); ++i)
    for (int component = 0; component < 4; ++component)
      if (m_Params[i].m_Comps[component] &&
          m_Params[i].m_Comps[component]->m_bDependsOnObject)
      {
        m_bDependsOnObject = true;
        return;
      }
}

SGenTC *SGenTC_SphereMap::mfCopy()
{
  SGenTC_SphereMap *copy = new SGenTC_SphereMap;
  copy->m_Mask = m_Mask;
  copy->m_bDependsOnObject = m_bDependsOnObject;
  return copy;
}

bool SGenTC_SphereMap::mfSet(bool bEnable)
{
  return true;
}

void SGenTC_SphereMap::mfCompile(char *params, SShader *ef)
{
}


SGenTC *SGenTC_EmbossMap::mfCopy()
{
  SGenTC_EmbossMap *copy = new SGenTC_EmbossMap;
  copy->m_Mask = m_Mask;
  copy->m_bDependsOnObject = m_bDependsOnObject;
  return copy;
}


bool SGenTC_EmbossMap::mfSet(bool bEnable)
{
  return true;
}

void SGenTC_EmbossMap::mfCompile(char *params, SShader *ef)
{
}

bool CShader::mfCompileTexGen(char *name, char *params, SShader *ef, SShaderTexUnit *ml)
{
  if (!name)
  {
    name = params;
    params = NULL;
  }
  if (!name || !ml)
    return false;
  if (!stricmp(name, "HW_NormalMap"))
  {
    ml->m_GTC = new SGenTC_NormalMap;
    ml->m_GTC->mfCompile(params, ef);
    return true;
  }
  if (!strnicmp(name, "HW_Reflection", 13))
  {
    ml->m_GTC = new SGenTC_ReflectionMap;
    ml->m_GTC->mfCompile(params, ef);
    return true;
  }
  if (!stricmp(name, "HW_SphereMap"))
  {
    ml->m_GTC = new SGenTC_SphereMap;
    ml->m_GTC->mfCompile(params, ef);
    return true;
  }
  if (!stricmp(name, "HW_EmbossMap"))
  {
    ml->m_GTC = new SGenTC_EmbossMap;
    ml->m_GTC->mfCompile(params, ef);
    return true;
  }
  if (!stricmp(name, "HW_ObjectLinear"))
  {
    ml->m_GTC = new SGenTC_ObjectLinear;
    ml->m_GTC->mfCompile(params, ef);
    return true;
  }
  if (!stricmp(name, "HW_EyeLinear"))
  {
    ml->m_GTC = new SGenTC_EyeLinear;
    ml->m_GTC->mfCompile(params, ef);
    return true;
  }
  return false;
}

//====================================================================
// Matrix operations


void CShader::mfCompileMatrixOp(TArray<SMatrixTransform>* List, char *scr, char *nmMat, SShader *ef)
{
  if (!List || !nmMat || !nmMat[0])
    return;

  // Keep the OpenGL matrix target values here without importing the GL
  // renderer headers into the NULL/Vulkan renderer.
  int matrix = 0;
  if (!stricmp(nmMat, "GL_TEXTURE")) matrix = 0x1702;
  else if (!stricmp(nmMat, "GL_MATRIX0_NV")) matrix = 0x8630;
  else if (!stricmp(nmMat, "GL_MATRIX1_NV")) matrix = 0x8631;
  else if (!stricmp(nmMat, "GL_MATRIX2_NV")) matrix = 0x8632;
  else if (!stricmp(nmMat, "GL_MATRIX3_NV")) matrix = 0x8633;
  else if (!stricmp(nmMat, "GL_MATRIX4_NV")) matrix = 0x8634;
  else if (!stricmp(nmMat, "GL_MATRIX5_NV")) matrix = 0x8635;
  else if (!stricmp(nmMat, "GL_MATRIX6_NV")) matrix = 0x8636;
  else if (!stricmp(nmMat, "GL_MATRIX7_NV")) matrix = 0x8637;
  else return;

  enum { eIdentity = 1, eTranslate, eRotateX, eRotateY, eRotateZ,
         eRotateXY, eRotateXZ, eRotateYZ, eScale, eTexStage, eMatrix,
         eLightCMProject, eProjected, eCoords };
  static tokenDesc commands[] =
  {
    {eIdentity, "Identity"}, {eTranslate, "Translate"},
    {eRotateX, "RotateX"}, {eRotateY, "RotateY"}, {eRotateZ, "RotateZ"},
    {eRotateXY, "Rotate_XY"}, {eRotateXZ, "Rotate_XZ"},
    {eRotateYZ, "Rotate_YZ"}, {eTexStage, "TexStage"},
    {eScale, "Scale"}, {eMatrix, "Matrix"},
    {eLightCMProject, "LightCMProject"}, {eProjected, "Projected"},
    {eCoords, "Coords"}, {0, 0}
  };
  int stage = 0;
  char *name = NULL, *params = NULL;
  long command;
  while ((command = shGetObject(&scr, commands, &name, &params)) > 0)
  {
    char *data = name ? name : params;
    if (command == eTexStage)
    {
      if (data) stage = shGetInt(data);
      continue;
    }
    if (command == eProjected || command == eCoords)
      continue; // OpenGL's parser also leaves these modifiers unimplemented.

    SMatrixTransform *op = NULL;
    SMatrixTransform_Identity identity;
    SMatrixTransform_Translate translate;
    SMatrixTransform_Scale scale;
    SMatrixTransform_Rotate rotate;
    SMatrixTransform_Matrix customMatrix;
    SMatrixTransform_LightCMProject lightProject;
    switch (command)
    {
      case eIdentity: op = &identity; break;
      case eTranslate: op = &translate; break;
      case eScale: op = &scale; break;
      case eRotateX: rotate.m_Offs = 1; op = &rotate; break;
      case eRotateY: rotate.m_Offs = 2; op = &rotate; break;
      case eRotateZ: rotate.m_Offs = 4; op = &rotate; break;
      case eRotateXY: rotate.m_Offs = 3; op = &rotate; break;
      case eRotateXZ: rotate.m_Offs = 5; op = &rotate; break;
      case eRotateYZ: rotate.m_Offs = 6; op = &rotate; break;
      case eMatrix: op = &customMatrix; break;
      case eLightCMProject: op = &lightProject; break;
      default: break;
    }
    if (!op) continue;
    // This operation depends on the active light and object. Keep it
    // distinguishable so Vulkan can report a precise unsupported pass.
    op->m_Matrix = command == eLightCMProject ? 0x8638 : matrix;
    op->m_Stage = stage;
    if (command == eTranslate || command == eScale ||
        (command >= eRotateX && command <= eRotateYZ))
    {
      TArray<SParam> parsed;
      mfCompileParam(params, ef, &parsed);
      if (parsed.Num()) op->m_Params[0] = parsed[0];
    }
    else if (command == eMatrix)
    {
      TArray<SParam> parsed;
      mfCompileParam(data, ef, &parsed);
      for (int i = 0; i < parsed.Num() && i < 4; ++i)
        op->m_Params[i] = parsed[i];
    }
    const int index = List->Num();
    List->AddIndex(1);
    memcpy(&List->Get(index), op, sizeof(SMatrixTransform));
  }
}

void SMatrixTransform_LightCMProject::mfSet(bool bSet)
{
}
void SMatrixTransform_LightCMProject::mfSet(Matrix44& matr)
{
}


void SMatrixTransform_Identity::mfSet(bool bSet)
{
}
void SMatrixTransform_Identity::mfSet(Matrix44& matr)
{
  matr.SetIdentity();
}

void SMatrixTransform_Translate::mfSet(bool bSet)
{
}
void SMatrixTransform_Translate::mfSet(Matrix44& matr)
{
  float *p = m_Params[0].mfGet();
  float *m = &matr(0, 0);
  for (int row = 0; row < 4; ++row)
    m[12 + row] += m[row] * p[0] + m[4 + row] * p[1] + m[8 + row] * p[2];
}

void SMatrixTransform_Scale::mfSet(bool bSet)
{
}
void SMatrixTransform_Scale::mfSet(Matrix44& matr)
{
  float *p = m_Params[0].mfGet();
  float *m = &matr(0, 0);
  for (int row = 0; row < 4; ++row)
  {
    m[row] *= p[0];
    m[4 + row] *= p[1];
    m[8 + row] *= p[2];
  }
}

void SMatrixTransform_Matrix::mfSet(bool bSet)
{
}
void SMatrixTransform_Matrix::mfSet(Matrix44& matr)
{
  for (int row = 0; row < 4; ++row)
  {
    float *p = m_Params[row].mfGet();
    memcpy(&matr(row, 0), p, 4 * sizeof(float));
  }
}

void SMatrixTransform_Rotate::mfSet(bool bSet)
{
}
void SMatrixTransform_Rotate::mfSet(Matrix44& matr)
{
  float *p = m_Params[0].mfGet();
  float x = 0.0f, y = 0.0f, z = 0.0f;
  switch (m_Offs)
  {
    case 1: x = 1; break;
    case 2: y = 1; break;
    case 4: z = 1; break;
    case 3: x = y = 1; break;
    case 5: x = z = 1; break;
    // Preserve the legacy GL renderer's Rotate_YZ axis selection, which uses
    // the same XY axis as Rotate_XY in its matrix-stack implementation.
    case 6: x = y = 1; break;
    case 7: x = y = z = 1; break;
    default: return;
  }
  float magnitude = sqrtf(x * x + y * y + z * z);
  x /= magnitude; y /= magnitude; z /= magnitude;
  const float angle = p[0] * (3.14159265358979323846f / 180.0f);
  const float s = sinf(angle), c = cosf(angle), oneMinusC = 1.0f - c;
  const float xx=x*x, yy=y*y, zz=z*z, xy=x*y, yz=y*z, zx=z*x;
  const float xs=x*s, ys=y*s, zs=z*s;
  const float rotation[16] = {
    oneMinusC*xx+c, oneMinusC*xy+zs, oneMinusC*zx-ys, 0,
    oneMinusC*xy-zs, oneMinusC*yy+c, oneMinusC*yz+xs, 0,
    oneMinusC*zx+ys, oneMinusC*yz-xs, oneMinusC*zz+c, 0,
    0, 0, 0, 1
  };
  float result[16];
  float *m = &matr(0, 0);
  for (int col = 0; col < 4; ++col)
    for (int row = 0; row < 4; ++row)
    {
      result[col * 4 + row] = 0.0f;
      for (int k = 0; k < 4; ++k)
        result[col * 4 + row] += m[k * 4 + row] * rotation[col * 4 + k];
    }
  memcpy(m, result, sizeof(result));
}

//====================================================================
// Array pointers for PS2
//================================================================

void SArrayPointer_Vertex::mfSet(int Id)
{
}

//=========================================================================================

void SArrayPointer_Normal::mfSet(int Id)
{
}

//=========================================================================================

void SArrayPointer_Texture::mfSet(int Id)
{
}

//=========================================================================================

void SArrayPointer_Color::mfSet(int Id)
{
}

//=========================================================================================

void SArrayPointer_SecColor::mfSet(int Id)
{
}

//=========================================================================================

float SParamComp_Fog::mfGet()
{
  return 0;
}

