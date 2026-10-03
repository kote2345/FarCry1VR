#include "StdAfx.h"
#include "CrySkinMorph.h"

const SGpuSkinningData& CrySkinMorph::getGpuSkinningData(const unsigned* mapping, unsigned count) const
{
    if (!m_gpuData.vertices.empty()) return m_gpuData;
    m_gpuData.key = AllocateGpuSkinningKey();
    std::vector<std::vector<SGpuSkinInfluence> > links(m_numDests);
    unsigned aux=0, vertex=0;
    for (unsigned bone=m_numSkipBones; bone<m_numBones; ++bone)
        for (unsigned group=0; group<2; ++group) {
            const unsigned groupCount=m_arrAux[aux++];
            for (unsigned i=0; i<groupCount; ++i,++vertex) {
                const Vertex& source=m_arrVertices[vertex];
                SGpuSkinInfluence link={};
                link.pointWeight[0]=source.pt.x; link.pointWeight[1]=source.pt.y; link.pointWeight[2]=source.pt.z;
                link.pointWeight[3]=group==0 ? 1.0f : source.fWeight; link.bone=bone;
                links[group==0 ? source.nDest : m_arrAux[aux++]].push_back(link);
            }
        }
    m_gpuData.vertices.resize(count);
    for (unsigned i=0; i<count; ++i) {
        SGpuSkinVertex& vertexData=m_gpuData.vertices[i];
        vertexData.positionFirst=(unsigned)m_gpuData.influences.size();
        if (mapping[i]<links.size()) {
            vertexData.positionCount=(unsigned)links[mapping[i]].size();
            m_gpuData.influences.insert(m_gpuData.influences.end(),links[mapping[i]].begin(),links[mapping[i]].end());
        }
    }
    return m_gpuData;
}

//////////////////////////////////////////////////////////////////////////
// does the skinning out of the given array of global matrices
void CrySkinMorph::skin (const Matrix44* pBones, float fWeight, Vec3d* pDest)const
{
#ifdef PROFILE_FRAME_SELF
	//PROFILE_FRAME_SELF(PureSkin);
#endif
	const Matrix44* pBone = pBones + m_numSkipBones, *pBonesEnd = pBones + m_numBones;
	const CrySkinAuxInt* pAux = &m_arrAux[0];
	const Vertex* pVertex = &m_arrVertices[0];

#ifdef _DEBUG
	TFixedArray<float> arrW;
	arrW.reinit(m_numDests, 0);
#endif

	for (; pBone!= pBonesEnd; ++pBone)
	{
		// each bone has a group of vertices

		// first process the rigid vertices
		const Vertex* pGroupEnd = pVertex + *pAux++;
		for (;pVertex < pGroupEnd; ++pVertex)
		{
			//CHANGED_BY_IVO - INVALID CHANGE, PLEASE REVISE
			pDest[pVertex->nDest] += pBone->TransformVectorOLD(pVertex->pt) * fWeight;
			//pDest[pVertex->nDest] += (GetTransposed44(*pBone)*(pVertex->pt)) * fWeight;
#ifdef _DEBUG
			assert (arrW[pVertex->nDest] == 0);
			arrW[pVertex->nDest] = 1;
#endif
		}

		// then process the smooth vertices
		pGroupEnd = pVertex + *pAux++;
		for (;pVertex < pGroupEnd; ++pVertex, ++pAux)
		{
		pDest[*pAux] += pBone->TransformVectorOLD(pVertex->pt) * (fWeight * pVertex->fWeight);
		//	pDest[*pAux] += (GetTransposed44(*pBone)*pVertex->pt) * fWeight * pVertex->fWeight;

#ifdef _DEBUG
			assert (arrW[*pAux] >= 0 && arrW[*pAux] < 1.005f);
			arrW[*pAux] += pVertex->fWeight;
			assert (arrW[*pAux] >= 0 && arrW[*pAux] < 1.005f);
#endif
		}
	}
#ifdef _DEBUG
	for (unsigned i = 0; i < m_numDests; ++i)
		assert (arrW[i] == 0 || (arrW[i] > 0.995f && arrW[i] < 1.005f));
#endif
}

//////////////////////////////////////////////////////////////////////////
// does the skinning out of the given array of global matrices,
// tries to estimate the changes in normals
void CrySkinMorph::skin (const Matrix44* pBones, float fWeight, Vec3d* pDest, Vec3dA16* pDestNormalsA16, float fAmplify)const
{
	//PROFILE_FRAME_SELF(PureSkin);

	const Matrix44* pBone = pBones + m_numSkipBones, *pBonesEnd = pBones + m_numBones;
	const CrySkinAuxInt* pAux = &m_arrAux[0];
	const Vertex* pVertex = &m_arrVertices[0];

#ifdef _DEBUG
	TFixedArray<float> arrW;
	arrW.reinit(m_numDests, 0);
#endif

	for (; pBone!= pBonesEnd; ++pBone)
	{
		// each bone has a group of vertices

		// first process the rigid vertices
		const Vertex* pGroupEnd = pVertex + *pAux++;
		for (;pVertex < pGroupEnd; ++pVertex)
		{
			//CHANGED_BY_IVO - INVALID CHANGE, PLEASE REVISE
			Vec3d vOffset = pBone->TransformVectorOLD(pVertex->pt) * fWeight;
			pDest[pVertex->nDest] += vOffset;
			pDestNormalsA16[pVertex->nDest].v += vOffset * fAmplify;
			//pDest[pVertex->nDest] += (GetTransposed44(*pBone)*(pVertex->pt)) * fWeight;
#ifdef _DEBUG
			assert (arrW[pVertex->nDest] == 0);
			arrW[pVertex->nDest] = 1;
#endif
		}

		// then process the smooth vertices
		pGroupEnd = pVertex + *pAux++;
		for (;pVertex < pGroupEnd; ++pVertex, ++pAux)
		{
			Vec3d vOffset = pBone->TransformVectorOLD(pVertex->pt) * (fWeight * pVertex->fWeight);
			pDest[*pAux] += vOffset;
			pDestNormalsA16[*pAux].v += vOffset * fAmplify;
			//	pDest[*pAux] += (GetTransposed44(*pBone)*pVertex->pt) * fWeight * pVertex->fWeight;

#ifdef _DEBUG
			assert (arrW[*pAux] >= 0 && arrW[*pAux] < 1.005f);
			arrW[*pAux] += pVertex->fWeight;
			assert (arrW[*pAux] >= 0 && arrW[*pAux] < 1.005f);
#endif
		}
	}
#ifdef _DEBUG
	for (unsigned i = 0; i < m_numDests; ++i)
		assert (arrW[i] == 0 || (arrW[i] > 0.995f && arrW[i] < 1.005f));
#endif
}


void CrySkinMorph::CStatistics::init (const CrySkinMorph* pSkin)
{
	const CrySkinAuxInt* pAux = &pSkin->m_arrAux[0];
	const Vertex* pVertex = &pSkin->m_arrVertices[0];
	numRigid = numSmooth = 0;
	fMinOffset = fMaxOffset = -1;
	for (unsigned nBone = pSkin->m_numSkipBones; nBone < pSkin->m_numBones; ++nBone)
	{
		// each bone has a group of vertices

		// first process the rigid vertices
		const Vertex* pGroupEnd = pVertex + *pAux++;
		for (;pVertex < pGroupEnd; ++pVertex)
		{
			//pDest[pVertex->nDest] += pBone->TransformVector(pVertex->pt) * fWeight;
			setDests.insert (pVertex->nDest);
			addOffset (pVertex->pt);
			++numRigid;
		}

		// then process the smooth vertices
		pGroupEnd = pVertex + *pAux++;
		for (;pVertex < pGroupEnd; ++pVertex, ++pAux)
		{
			//pDest[*pAux] += pBone->TransformVector(pVertex->pt) * (fWeight * pVertex->fWeight);
			setDests.insert (*pAux);
			addOffset (pVertex->pt);
			++numSmooth;
		}
	}
}

void CrySkinMorph::CStatistics::addOffset (const Vec3d& v)
{
	float d = v.Length();
	if (fMaxOffset < 0 || d > fMaxOffset)
		fMaxOffset = d;
	if (fMinOffset < 0 || d < fMinOffset)
		fMinOffset = d;
}
