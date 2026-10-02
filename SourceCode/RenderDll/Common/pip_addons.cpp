/////////////////////////////////////////////////////////////////////////////////////
// Buffer optimizer
/////////////////////////////////////////////////////////////////////////////////////

#include "RenderPCH.h"

#include <vector>
#include <algorithm>

////////////////////////////////////////////////////////////////////////////////////////////////////
// PipVertex 
////////////////////////////////////////////////////////////////////////////////////////////////////

#define PIP_TEX_EPS 0.001f
#define PIP_VER_EPS 0.001f

bool struct_VERTEX_FORMAT_P3F_N_COL4UB_TEX2F::operator == (struct_VERTEX_FORMAT_P3F_N_COL4UB_TEX2F & other)
{
  assert(this != &other);

	// do y check first since x was used for hash
  return fabs(xyz.y-other.xyz.y)<PIP_VER_EPS && fabs(xyz.x-other.xyz.x)<PIP_VER_EPS && fabs(xyz.z-other.xyz.z)<PIP_VER_EPS &&
         fabs(normal.x-other.normal.x)<PIP_VER_EPS && fabs(normal.y-other.normal.y)<PIP_VER_EPS && fabs(normal.z-other.normal.z)<PIP_VER_EPS &&
         fabs(st[0]-other.st[0])<PIP_TEX_EPS && fabs(st[1]-other.st[1])<PIP_TEX_EPS &&
         (color.dcolor&0xffffff) == (other.color.dcolor&0xffffff);
}

int CLeafBuffer::FindInBuffer(struct_VERTEX_FORMAT_P3F_N_COL4UB_TEX2F &opt, SPipTangents &origBasis, uint nMatInfo, uint *uiInfo, struct_VERTEX_FORMAT_P3F_N_COL4UB_TEX2F* _vbuff, SPipTangents *_vbasis, int _vcount, list2<unsigned short> * pHash, TArray<uint>& ShareNewInfo)
{
  if (!_vbuff || !pHash)
    return -1;
  for(int i=0; i<pHash->Count(); i++) 
  {
    int id = (*pHash)[i];
    if (id < 0 || id >= _vcount)
      continue;
    if(_vbuff[id] == opt) 
    {
      if (id < ShareNewInfo.Num() && ShareNewInfo[id] != nMatInfo)
        continue;
      if (CRenderer::CV_r_indexingWithTangents && _vbasis)
      {
        if (origBasis.m_Binormal.Dot(_vbasis[id].m_Binormal) > 0.005f && origBasis.m_Tangent.Dot(_vbasis[id].m_Tangent) > 0.005f)
          return (*pHash)[i];  
      }
      else
        return (*pHash)[i];  
    }
  }

  return -1;
}

void CLeafBuffer::CompactBuffer(struct_VERTEX_FORMAT_P3F_N_COL4UB_TEX2F * _vbuff, SPipTangents *_tbuff, int * _vcount, TArray<unsigned short> * pindices, bool bShareVerts[128], uint *uiInfo)
{
  if (!_vbuff || !_vcount || *_vcount <= 0)
    return;
  
  int vert_num_before = *_vcount;
  if (!pindices)
    return;

  if (vert_num_before <= 1)
  {
    pindices->Free();
    if (vert_num_before == 1)
      pindices->AddElem(0);
    return;
  }

  struct VertRef
  {
    float x;
    int origIndex;
  };

  std::vector<VertRef> sorted(vert_num_before);
  for (int i = 0; i < vert_num_before; ++i)
  {
    sorted[i].x = _vbuff[i].xyz.x;
    sorted[i].origIndex = i;
  }

  std::sort(sorted.begin(), sorted.end(), [](const VertRef& a, const VertRef& b) {
    return a.x < b.x;
  });

  std::vector<int> remap(vert_num_before, -1);
  std::vector<struct_VERTEX_FORMAT_P3F_N_COL4UB_TEX2F> unique_vbuff;
  std::vector<SPipTangents> unique_tbuff;
  std::vector<uint> unique_uiInfo;
  unique_vbuff.reserve(vert_num_before);
  if (_tbuff) unique_tbuff.reserve(vert_num_before);
  if (uiInfo) unique_uiInfo.reserve(vert_num_before);

  for (int i = 0; i < vert_num_before; ++i)
  {
    int idxI = sorted[i].origIndex;
    if (remap[idxI] != -1)
      continue;

    uint nMInfo = uiInfo ? uiInfo[idxI] : 0;
    uint nMatId = nMInfo & 255;
    bool bCanShare = bShareVerts ? bShareVerts[nMatId] : true;

    int newUniqueId = (int)unique_vbuff.size();
    unique_vbuff.push_back(_vbuff[idxI]);
    if (_tbuff) unique_tbuff.push_back(_tbuff[idxI]);
    if (uiInfo) unique_uiInfo.push_back(nMInfo);
    remap[idxI] = newUniqueId;

    if (!bCanShare)
      continue;

    for (int j = i + 1; j < vert_num_before; ++j)
    {
      if ((sorted[j].x - sorted[i].x) > PIP_VER_EPS)
        break;

      int idxJ = sorted[j].origIndex;
      if (remap[idxJ] != -1)
        continue;

      uint nMInfoJ = uiInfo ? uiInfo[idxJ] : 0;
      if (nMInfo != nMInfoJ)
        continue;

      if (!(_vbuff[idxI] == _vbuff[idxJ]))
        continue;

      if (CRenderer::CV_r_indexingWithTangents && _tbuff)
      {
        if (_tbuff[idxI].m_Binormal.Dot(_tbuff[idxJ].m_Binormal) <= 0.005f ||
            _tbuff[idxI].m_Tangent.Dot(_tbuff[idxJ].m_Tangent) <= 0.005f)
        {
          continue;
        }
      }

      remap[idxJ] = newUniqueId;
    }
  }

  // The sorted references accelerate duplicate lookup only. The original
  // OpenGL compactor emits vertices in first-occurrence order, which is also
  // the order of the independently baked lightmap coordinate stream. Emitting
  // the X-sorted vertices breaks that association even without vertex sharing.
  std::vector<int> orderedIds(unique_vbuff.size(), -1);
  std::vector<struct_VERTEX_FORMAT_P3F_N_COL4UB_TEX2F> orderedVertices;
  std::vector<SPipTangents> orderedTangents;
  orderedVertices.reserve(unique_vbuff.size());
  if (_tbuff) orderedTangents.reserve(unique_tbuff.size());

  pindices->Free();
  for (int i = 0; i < vert_num_before; ++i)
  {
    const int sortedId = remap[i];
    if (orderedIds[sortedId] == -1)
    {
      orderedIds[sortedId] = static_cast<int>(orderedVertices.size());
      orderedVertices.push_back(unique_vbuff[sortedId]);
      if (_tbuff) orderedTangents.push_back(unique_tbuff[sortedId]);
    }
    pindices->AddElem((unsigned short)orderedIds[sortedId]);
  }

  int uniqueCount = (int)unique_vbuff.size();
  *_vcount = uniqueCount;
  cryMemcpy(_vbuff, orderedVertices.data(), uniqueCount * sizeof(struct_VERTEX_FORMAT_P3F_N_COL4UB_TEX2F));
  if (_tbuff && !orderedTangents.empty())
    cryMemcpy(_tbuff, orderedTangents.data(), uniqueCount * sizeof(SPipTangents));

  int ratio = 100 * (*_vcount) / vert_num_before;
  CryLogComment("  Size after compression = %d %s ( %d -> %d )", ratio, "%", vert_num_before, *_vcount);
}
