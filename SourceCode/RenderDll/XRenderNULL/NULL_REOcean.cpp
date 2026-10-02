/*=============================================================================
  PS2_REOcean.cpp : implementation of the Ocean Rendering.
  Copyright (c) 2001 Crytek Studios. All Rights Reserved.

  Revision history:
    * Created by Honitch Andrey

=============================================================================*/

#include "RenderPCH.h"
#include "NULL_Renderer.h"
#include "I3DEngine.h"

#undef THIS_FILE
static char THIS_FILE[] = __FILE__;

//=======================================================================

void CREOcean::mfReset()
{
}

bool CREOcean::mfPreDraw(SShaderPass *sl)
{
  return true;
}

void CREOcean::GenerateIndices(int nLodCode)
{
  SPrimitiveGroup pg;
  TArray<ushort> Indicies;
  int size;

  if (m_OceanIndicies[nLodCode])
    return;
  SOceanIndicies *oi = new SOceanIndicies;
  m_OceanIndicies[nLodCode] = oi;
  if (!(nLodCode & ~LOD_MASK))
  {
    int nL = nLodCode & LOD_MASK;
    pg.offsIndex = 0;
    pg.numIndices = m_pIndices[nL].Num();
    pg.numTris = pg.numIndices-2;
    pg.type = PT_STRIP;
    oi->m_Groups.AddElem(pg);
    oi->m_nInds = pg.numIndices;
    int size = pg.numIndices * sizeof(ushort);
    oi->m_pIndicies = new ushort[size];
    memcpy(oi->m_pIndicies, &m_pIndices[nL][0], size);
    return;
  }
  int nLod = nLodCode & LOD_MASK;
  int nl = 1<<nLod;
  int nGrid = (OCEANGRID+1);
  // set indices
  int iIndex = nGrid*nl+nl;
  int yStep = nGrid * nl;
  for(int a=nl; a<nGrid-1-nl; a+=nl)
  {
    for(int i=nl; i<nGrid-nl; i+=nl, iIndex+=nl)
    {
      Indicies.AddElem(iIndex);
      Indicies.AddElem(iIndex + yStep);
    }

    int iNextIndex = (a+nl) * nGrid + nl;

    // connect two strips by inserting two degenerated triangles
    if(a < nGrid-1-nl*2)
    {
      Indicies.AddElem(iIndex + yStep - nl);
      Indicies.AddElem(iNextIndex);
    }
    iIndex = iNextIndex;
  }
  pg.numIndices = Indicies.Num();
  pg.numTris = pg.numIndices-2;
  pg.type = PT_STRIP;
  pg.offsIndex = 0;
  oi->m_Groups.AddElem(pg);

  // Left
  pg.offsIndex = Indicies.Num();
  iIndex = nGrid*(nGrid-1);
  if (!(nLodCode & (1<<LOD_LEFTSHIFT)))
  {
    Indicies.AddElem(iIndex);
    Indicies.AddElem(iIndex - nGrid * nl + nl);
    Indicies.AddElem(iIndex - nGrid * nl);
    iIndex = iIndex - nGrid * nl + nl;
    yStep = -(nGrid * nl) - nl;
    for(int i=nl; i<nGrid-nl; i+=nl, iIndex-=nGrid*nl)
    {
      Indicies.AddElem(iIndex);
      Indicies.AddElem(iIndex + yStep);
    }
  }
  else
  {
    for(int i=0; i<nGrid-nl; i+=nl*2)
    {
      Indicies.AddElem(iIndex);
      Indicies.AddElem(iIndex - (nGrid * nl) + nl);
      Indicies.AddElem(iIndex - nGrid * nl * 2);
      if (i < nGrid-nl-nl*2)
        Indicies.AddElem(iIndex - nGrid * nl * 2 + nl);
      iIndex -= nGrid * nl * 2;
    }
  }
  pg.numIndices = Indicies.Num()-pg.offsIndex;
  pg.numTris = pg.numIndices-2;
  oi->m_Groups.AddElem(pg);

  // Bottom
  pg.offsIndex = Indicies.Num();
  iIndex = 0;
  if (!(nLodCode & (1<<LOD_BOTTOMSHIFT)))
  {
    Indicies.AddElem(iIndex);
    Indicies.AddElem(iIndex + nGrid*nl + nl);
    Indicies.AddElem(iIndex + nl);
    iIndex = iIndex + nGrid*nl + nl;
    yStep = -(nGrid * nl) + nl;
    for(int i=nl; i<nGrid-nl; i+=nl, iIndex+=nl)
    {
      Indicies.AddElem(iIndex);
      Indicies.AddElem(iIndex + yStep);
    }
  }
  else
  {
    for(int i=0; i<nGrid-nl; i+=nl*2)
    {
      Indicies.AddElem(iIndex);
      Indicies.AddElem(iIndex + (nGrid * nl) + nl);
      Indicies.AddElem(iIndex + nl * 2);
      if (i < nGrid-nl-nl*2)
        Indicies.AddElem(iIndex + nGrid * nl + nl * 2);
      iIndex += nl * 2;
    }
  }
  pg.numIndices = Indicies.Num()-pg.offsIndex;
  pg.numTris = pg.numIndices-2;
  oi->m_Groups.AddElem(pg);

  // Right
  pg.offsIndex = Indicies.Num();
  iIndex = nGrid-1;
  if (!(nLodCode & (1<<LOD_RIGHTSHIFT)))
  {
    Indicies.AddElem(iIndex);
    Indicies.AddElem(iIndex + nGrid * nl - nl);
    Indicies.AddElem(iIndex + nGrid * nl);
    iIndex = iIndex + nGrid * nl - nl;
    yStep = nGrid * nl + nl;
    for(int i=nl; i<nGrid-nl; i+=nl, iIndex+=nGrid*nl)
    {
      Indicies.AddElem(iIndex);
      Indicies.AddElem(iIndex + yStep);
    }
  }
  else
  {
    for(int i=0; i<nGrid-nl; i+=nl*2)
    {
      Indicies.AddElem(iIndex);
      Indicies.AddElem(iIndex + (nGrid * nl) - nl);
      Indicies.AddElem(iIndex + nGrid * nl * 2);
      if (i < nGrid-nl-nl*2)
        Indicies.AddElem(iIndex + nGrid * nl * 2 - nl);
      iIndex += nGrid * nl * 2;
    }
  }
  pg.numIndices = Indicies.Num()-pg.offsIndex;
  pg.numTris = pg.numIndices-2;
  oi->m_Groups.AddElem(pg);

  // Top
  pg.offsIndex = Indicies.Num();
  iIndex = nGrid*(nGrid-1)+nGrid-1;
  if (!(nLodCode & (1<<LOD_TOPSHIFT)))
  {
    Indicies.AddElem(iIndex);
    Indicies.AddElem(iIndex - nGrid*nl - nl);
    Indicies.AddElem(iIndex - nl);
    iIndex = iIndex - nGrid*nl - nl;
    yStep = nGrid * nl - nl;
    for(int i=nl; i<nGrid-nl; i+=nl, iIndex-=nl)
    {
      Indicies.AddElem(iIndex);
      Indicies.AddElem(iIndex + yStep);
    }
  }
  else
  {
    for(int i=0; i<nGrid-nl; i+=nl*2)
    {
      Indicies.AddElem(iIndex);
      Indicies.AddElem(iIndex - (nGrid * nl) - nl);
      Indicies.AddElem(iIndex - nl * 2);
      if (i < nGrid-nl-nl*2)
        Indicies.AddElem(iIndex - nGrid * nl - nl * 2);
      iIndex -= nl * 2;
    }
  }
  pg.numIndices = Indicies.Num()-pg.offsIndex;
  pg.numTris = pg.numIndices-2;
  oi->m_Groups.AddElem(pg);

  size = Indicies.Num()*sizeof(ushort);
  oi->m_pIndicies = new ushort[size];
  oi->m_nInds = Indicies.Num();
  cryMemcpy(oi->m_pIndicies, &Indicies[0], size);
}


void CREOcean::UpdateTexture()
{
  // GL_DSDT_NV stores signed XY. Vulkan mirrors these as biased UNORM bytes.
  byte pixels[OCEANGRID * OCEANGRID * 4];
  for (int y = 0; y < OCEANGRID; ++y)
    for (int x = 0; x < OCEANGRID; ++x)
    {
      byte* pixel = pixels + (y * OCEANGRID + x) * 4;
      pixel[2] = (byte)CLAMP(QRound(m_Normals[y][x].x * 127.5f) + 128, 0, 255);
      pixel[1] = (byte)CLAMP(QRound(m_Normals[y][x].y * 127.5f) + 128, 0, 255);
      pixel[0] = 255; pixel[3] = 255;
    }
  if (m_CustomTexBind[0] <= 0)
    m_CustomTexBind[0] = gRenDev->DownLoadToVideoMemory(pixels, OCEANGRID, OCEANGRID,
      eTF_8888, eTF_8888, 1, true, FILTER_BILINEAR);
  else
    gRenDev->UpdateTextureInVideoMemory(m_CustomTexBind[0], pixels, 0, 0, OCEANGRID, OCEANGRID, eTF_8888);
}

void CREOcean::DrawOceanSector(SOceanIndicies *oi)
{
}

static _inline int Compare(SOceanSector *& p1, SOceanSector *& p2)
{
  if(p1->m_Flags > p2->m_Flags)
    return 1;
  else
  if(p1->m_Flags < p2->m_Flags)
    return -1;
  
  return 0;
}

static _inline float sCalcSplash(SSplash *spl, float fX, float fY)
{
  CNULLRenderer *r = gcpNULL;

  float fDeltaTime = r->m_RP.m_RealTime - spl->m_fStartTime;
  float fScaleFactor = 1.0f / (r->m_RP.m_RealTime - spl->m_fLastTime + 1.0f);

  float vDelt[2];

  // Calculate 2D distance
  vDelt[0] = spl->m_Pos[0] - fX; vDelt[1] = spl->m_Pos[1] - fY;
  float fSqDist = vDelt[0]*vDelt[0] + vDelt[1]*vDelt[1];

  // Inverse square root
  unsigned int *n1 = (unsigned int *)&fSqDist;
  unsigned int nn = 0x5f3759df - (*n1 >> 1);
  float *n2 = (float *)&nn;
  float fDistSplash = 1.0f / ((1.5f - (fSqDist * 0.5f) * *n2 * *n2) * *n2);

  // Emulate sin waves
  float fDistFactor = fDeltaTime*10.0f - fDistSplash + 4.0f;
  fDistFactor = CLAMP(fDistFactor, 0.0f, 1.0f);
  float fRad = (fDistSplash - fDeltaTime*10) * 0.4f / 3.1416f * 1024.0f;
  float fSin = gRenDev->m_RP.m_tSinTable[QRound(fRad)&0x3ff] * fDistFactor;

  return fSin * fScaleFactor * spl->m_fForce;
}

float *CREOcean::mfFillAdditionalBuffer(SOceanSector *os, int nSplashes, SSplash *pSplashes[], int& nCurSize, int nLod, float fSize)
{
  return NULL;
}

void CREOcean::mfDrawOceanSectors()
{
  // Use the same FFT grid, view bounds, sector LOD and stitched index groups
  // as GLREOcean. The original vertex-program displacement is evaluated here
  // because the NULL backend has no CG vertex-program executor.
  if (!m_pBuffer || !m_pBuffer->m_VS[VSF_GENERAL].m_VData || !m_HMap)
    return;
  I3DEngine* engine = iSystem->GetI3DEngine();
  CCamera camera = gRenDev->GetCamera();
  Vec3d eye = camera.GetPos();
  float waterLevel = engine->GetWaterLevel();
  float size = (float)CRenderer::CV_r_oceansectorsize;
  if (size <= 0.0f) return;
  float heightScale = (float)CRenderer::CV_r_oceanheightscale;
  float distance = engine->GetMaxViewDistance();
  float centerX = (float)((int)eye.x & ~255) + 128.0f;
  float centerY = (float)((int)eye.y & ~255) + 128.0f;
  if (size != m_fSectorSize)
  {
    m_fSectorSize = size;
    for (int i = 0; i < 256; ++i) m_OceanSectorsHash[i].Free();
  }
  m_VisOceanSectors.SetUse(0);
  // Populate the hashes first: TArray growth invalidates pointers. Collect
  // pointers only after all sectors (including stitch neighbors) exist.
  for (float y = centerY-distance-size; y < centerY+distance+size; y += size)
    for (float x = centerX-distance-size; x < centerX+distance+size; x += size)
      GetSectorByPos(x, y);
  for (float y = centerY-distance; y < centerY+distance; y += size)
    for (float x = centerX-distance; x < centerX+distance; x += size)
    {
      SOceanSector* sector = GetSectorByPos(x, y);
      if (!(sector->m_Flags & (OSF_FIRSTTIME | OSF_VISIBLE))) continue;
      Vec3d mins(x + m_MinBound.x*size, y + m_MinBound.y*size, waterLevel + m_MinBound.z*heightScale);
      Vec3d maxs(x+size + m_MaxBound.x*size, y+size + m_MaxBound.y*size, waterLevel + m_MaxBound.z*heightScale);
      if (camera.IsAABBVisible_hierarchical(AABB(mins,maxs)) == CULL_EXCLUSION) continue;
      sector->nLod = GetLOD(eye, (mins+maxs)*0.5f);
      sector->m_Frame = gRenDev->m_cEF.m_Frame;
      sector->m_Flags &= ~OSF_LODUPDATED;
      m_VisOceanSectors.AddElem(sector);
    }
  if (m_VisOceanSectors.Num())
  {
    LinkVisSectors(size);
    ::Sort(&m_VisOceanSectors[0], m_VisOceanSectors.Num());
  }
  struct_VERTEX_FORMAT_P3F_N* source = (struct_VERTEX_FORMAT_P3F_N*)m_pBuffer->m_VS[VSF_GENERAL].m_VData;
  struct_VERTEX_FORMAT_P3F_N_COL4UB_TEX2F vertices[(OCEANGRID+1)*(OCEANGRID+1)] = {};
  m_RS.m_StatsNumRendOceanSectors = 0;
  int savedState = gRenDev->m_CurState;
  for (int i = 0; i < m_VisOceanSectors.Num(); ++i)
  {
    SOceanSector* sector = m_VisOceanSectors[i];
    bool blend = false;
    float minHeight = 99999.0f;
    SSplash* splashes[16];
    int splashCount = 0;
    const int maxSplashes = CLAMP(CRenderer::CV_r_oceanmaxsplashes,0,16);
    for (int j = 0; j < gRenDev->m_RP.m_Splashes.Num() && splashCount < maxSplashes; ++j)
    {
      SSplash* splash = &gRenDev->m_RP.m_Splashes[j];
      float radius = splash->m_fCurRadius;
      if (splash->m_Pos[0]-radius > sector->x+size || splash->m_Pos[1]-radius > sector->y+size ||
          splash->m_Pos[0]+radius < sector->x || splash->m_Pos[1]+radius < sector->y) continue;
      splashes[splashCount++] = splash;
    }
    for (int y = 0; y <= OCEANGRID; ++y)
      for (int x = 0; x <= OCEANGRID; ++x)
      {
        int index = y*(OCEANGRID+1)+x;
        auto& output = vertices[index];
        output.xyz.x = source[index].xyz.x*size + sector->x;
        output.xyz.y = source[index].xyz.y*size + sector->y;
        float ground = GetHMap(output.xyz.x,output.xyz.y);
        minHeight = crymin(minHeight,ground);
        float depth = waterLevel-ground;
        blend |= depth <= 1.0f;
        float fade = CLAMP(depth*0.066f,0.0f,1.0f);
        float dx = output.xyz.x-eye.x, dy = output.xyz.y-eye.y;
        // CGVProgOcean applies depth scale to wave height only. Curvature is
        // added afterward and remains independent of seabed depth.
        output.xyz.z = source[index].xyz.z*heightScale*fade -
            0.00001f*(dx*dx+dy*dy) + waterLevel;
        for (int splash = 0; splash < splashCount; ++splash)
          output.xyz.z += sCalcSplash(splashes[splash],output.xyz.x,output.xyz.y);
        output.normal = source[index].normal;
        output.normal.x *= fade; output.normal.y *= fade;
        output.normal.NormalizeFast();
        byte alpha = (byte)CLAMP(QRound(depth*0.5f*255.0f),0,255);
        output.color.dcolor = 0x00ffffffu | ((uint32)alpha<<24);
        output.st[0] = output.xyz.x/128.0f; output.st[1] = output.xyz.y/128.0f;
      }
    sector->m_Flags &= ~OSF_FIRSTTIME;
    if (minHeight > waterLevel) { sector->m_Flags &= ~OSF_VISIBLE; continue; }
    sector->m_Flags |= OSF_VISIBLE;
    int lod = sector->nLod;
    int code = lod |
      ((lod < GetSectorByPos(sector->x-size,sector->y)->nLod)<<LOD_LEFTSHIFT) |
      ((lod < GetSectorByPos(sector->x+size,sector->y)->nLod)<<LOD_RIGHTSHIFT) |
      ((lod < GetSectorByPos(sector->x,sector->y+size)->nLod)<<LOD_TOPSHIFT) |
      ((lod < GetSectorByPos(sector->x,sector->y-size)->nLod)<<LOD_BOTTOMSHIFT);
    GenerateIndices(code);
    SOceanIndicies* indices = m_OceanIndicies[code];
    gRenDev->EF_SetState(GS_DEPTHWRITE | (blend ? GS_BLSRC_SRCALPHA | GS_BLDST_ONEMINUSSRCALPHA : 0));
    CVertexBuffer buffer(vertices, VERTEX_FORMAT_P3F_N_COL4UB_TEX2F, (OCEANGRID+1)*(OCEANGRID+1));
    // Join the original stitched groups into one triangle list. This keeps
    // their topology while uploading each sector's vertex grid only once.
    TArray<ushort> triangles;
    for (int group = 0; group < indices->m_Groups.Num(); ++group)
    {
      const SPrimitiveGroup& primitive = indices->m_Groups[group];
      const ushort* sourceIndices = indices->m_pIndicies + primitive.offsIndex;
      int step = primitive.type == PT_LIST ? 3 : 1;
      for (int j = 0; j+2 < primitive.numIndices; j += step)
      {
        ushort a = sourceIndices[j], b = sourceIndices[j+1], c = sourceIndices[j+2];
        if (primitive.type == PT_FAN) a = sourceIndices[0];
        if (primitive.type == PT_STRIP && (j&1)) { ushort swap = a; a = b; b = swap; }
        if (a == b || a == c || b == c) continue;
        triangles.AddElem(a); triangles.AddElem(b); triangles.AddElem(c);
      }
    }
    if (triangles.Num())
    {
      SVertexStream indexStream;
      indexStream.m_VData = &triangles[0];
      indexStream.m_nItems = triangles.Num();
      gRenDev->DrawBuffer(&buffer,&indexStream,triangles.Num(),0,
        R_PRIMV_TRIANGLES,0,(OCEANGRID+1)*(OCEANGRID+1),NULL);
    }
    ++m_RS.m_StatsNumRendOceanSectors;
  }
  gRenDev->EF_SetState(savedState);
}

void CREOcean::mfDrawOceanScreenLod()
{
  // The stock projected-grid backend uses GL program objects. Until that
  // optimization is ported, submit the equivalent visible FFT sector surface.
  mfDrawOceanSectors();
}

bool CREOcean::mfDraw(SShader *ef, SShaderPass *sfm)
{ 
  // Vulkan walks render items directly, unlike GL's EF_Flush preparation.
  // Generate the FFT grid before submitting this element's first surface pass.
  mfPrepare();
  if (!m_HMap) PrepareHMap();
  double time0 = 0;
  ticks(time0);

  if (CRenderer::CV_r_oceanrendtype == 0)
    mfDrawOceanSectors();
  else
    mfDrawOceanScreenLod();
  unticks(time0);
  m_RS.m_StatsTimeRendOcean = (float)(time0*1000.0*g_SecondsPerCycle);

  return true;
}



// AABBSV camera-frustum culling uses the same face table as GLREOcean.
char BoxSides[0x40*8] = {
	0,0,0,0, 0,0,0,0, //00
		0,4,6,2, 0,0,0,4, //01
		7,5,1,3, 0,0,0,4, //02
		0,0,0,0, 0,0,0,0, //03
		0,1,5,4, 0,0,0,4, //04
		0,1,5,4, 6,2,0,6, //05
		7,5,4,0, 1,3,0,6, //06
		0,0,0,0, 0,0,0,0, //07
		7,3,2,6, 0,0,0,4, //08
		0,4,6,7, 3,2,0,6, //09
		7,5,1,3, 2,6,0,6, //0a
		0,0,0,0, 0,0,0,0, //0b
		0,0,0,0, 0,0,0,0, //0c
		0,0,0,0, 0,0,0,0, //0d
		0,0,0,0, 0,0,0,0, //0e
		0,0,0,0, 0,0,0,0, //0f
		0,2,3,1, 0,0,0,4, //10
		0,4,6,2, 3,1,0,6, //11
		7,5,1,0, 2,3,0,6, //12
		0,0,0,0, 0,0,0,0, //13
		0,2,3,1, 5,4,0,6, //14
		1,5,4,6, 2,3,0,6, //15
		7,5,4,0, 2,3,0,6, //16
		0,0,0,0, 0,0,0,0, //17
		0,2,6,7, 3,1,0,6, //18
		0,4,6,7, 3,1,0,6, //19
		7,5,1,0, 2,6,0,6, //1a
		0,0,0,0, 0,0,0,0, //1b
		0,0,0,0, 0,0,0,0, //1c
		0,0,0,0, 0,0,0,0, //1d
		0,0,0,0, 0,0,0,0, //1e
		0,0,0,0, 0,0,0,0, //1f
		7,6,4,5, 0,0,0,4, //20
		0,4,5,7, 6,2,0,6, //21
		7,6,4,5, 1,3,0,6, //22
		0,0,0,0, 0,0,0,0, //23
		7,6,4,0, 1,5,0,6, //24
		0,1,5,7, 6,2,0,6, //25
		7,6,4,0, 1,3,0,6, //26
		0,0,0,0, 0,0,0,0, //27
		7,3,2,6, 4,5,0,6, //28
		0,4,5,7, 3,2,0,6, //29
		6,4,5,1, 3,2,0,6, //2a
		0,0,0,0, 0,0,0,0, //2b
		0,0,0,0, 0,0,0,0, //2c
		0,0,0,0, 0,0,0,0, //2d
		0,0,0,0, 0,0,0,0, //2e
		0,0,0,0, 0,0,0,0, //2f
		0,0,0,0, 0,0,0,0, //30
		0,0,0,0, 0,0,0,0, //31
		0,0,0,0, 0,0,0,0, //32
		0,0,0,0, 0,0,0,0, //33
		0,0,0,0, 0,0,0,0, //34
		0,0,0,0, 0,0,0,0, //35
		0,0,0,0, 0,0,0,0, //36
		0,0,0,0, 0,0,0,0, //37
		0,0,0,0, 0,0,0,0, //38
		0,0,0,0, 0,0,0,0, //39
		0,0,0,0, 0,0,0,0, //3a
		0,0,0,0, 0,0,0,0, //3b
		0,0,0,0, 0,0,0,0, //3c
		0,0,0,0, 0,0,0,0, //3d
		0,0,0,0, 0,0,0,0, //3e
		0,0,0,0, 0,0,0,0, //3f
};
