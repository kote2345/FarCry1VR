#ifndef CRY_GPU_SKINNING_H
#define CRY_GPU_SKINNING_H
#include <vector>
#include <stdint.h>
#include <functional>
typedef std::function<void(const float*, uint32_t)> GpuSkinReadback;
uint64_t AllocateGpuSkinningKey();
// std430 layouts. Offsets are in bone space, exactly as in CrySkinFull.
// Variable length ranges retain every link without truncating bone weights.
struct SGpuSkinInfluence {
    float pointWeight[4];
    uint32_t bone, padding[3];
};
struct SGpuSkinVertex {
    uint32_t positionFirst, positionCount, normalFirst, normalCount;
    float tangent[4], binormal[4];
    uint32_t tangentBone, flipped, padding[2];
};
struct SGpuSkinningData {
    uint64_t key = 0;
    std::vector<SGpuSkinVertex> vertices;
    std::vector<SGpuSkinInfluence> influences;
    std::vector<uint32_t> internalToExternal;
};
struct SGpuSkinShadowData {
    uint64_t key = 0;
    std::vector<uint32_t> faces; // three external vertex indices per face
    std::vector<uint32_t> edges; // v0, v1, face0, face1 (UINT32_MAX at boundaries)
};
static_assert(sizeof(SGpuSkinInfluence) == 32, "GPU skin link layout");
static_assert(sizeof(SGpuSkinVertex) == 64, "GPU skin vertex layout");
#endif
