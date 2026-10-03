#ifndef CRY_VULKAN_GPU_SKINNING_H
#define CRY_VULKAN_GPU_SKINNING_H
#include "VulkanResourceManager.h"
#include "GpuSkinning.h"
#include <map>
#include <array>

namespace CryVR {
// Deformation is recorded before shadow maps, reflections and both XR eyes.
// No device readback or additional submission is needed for mesh rendering.
class VulkanGpuSkinning {
public:
    bool Initialize(VulkanContext& context, VulkanResourceManager& resources);
    void Shutdown();
    void BeginFrame();
    bool Queue(const void* identity, const SGpuSkinningData& mesh,
               const float* bones, uint32_t boneCount);
    void Clear(const void* identity) { m_poseByIdentity.erase(identity); }
    bool Remap(const void* identity, const void* source, const uint32_t* mapping, uint32_t count);
    bool Morph(const void* identity, const SGpuSkinningData& data, float weight, float normalAmplify);
    bool Shadow(const void* identity, const void* source, const SGpuSkinningData& mesh,
                const SGpuSkinShadowData& topology, const float* bones, uint32_t boneCount,
                const float* light, float extent, uint32_t first = 0);
    bool HasPose(const void* identity) const { return m_poseByIdentity.count(identity) != 0; }
    bool AddPatch(const void* identity, VkBuffer buffer, uint32_t first, uint32_t count,
                  VkDeviceSize offset, uint32_t stride, uint32_t normalOffset,
                  VkDeviceSize tangentOffset);
    bool Prepare(uint32_t slot);
    void Record(VkCommandBuffer command);
    void CollectUnused(); // called only after the preceding submission retires
    bool Readback(const void* identity, const GpuSkinReadback& callback);
    void Complete(uint32_t slot);
private:
    struct Mesh { VulkanBuffer input; uint32_t vertexCount, linksWordOffset, lastFrame, requiredBones; };
    struct Pose { uint64_t meshKey; uint32_t paletteOffset, resultWordOffset, boneCount; VkDescriptorSet descriptor; };
    struct Patch { VkBuffer destination; uint32_t constants[8]; VkDescriptorSet descriptor; uint64_t shadowKey; };
    struct Binding { uint32_t pose, remapOffset, count; uint64_t shadowKey; uint32_t shadowFirst = 0; };
    struct ShadowMesh { VulkanBuffer input; uint32_t faces, edges, lastFrame; };
    struct MorphJob { uint64_t meshKey; uint32_t pose; float weight, normalAmplify; VkDescriptorSet descriptor; };
    struct ReadbackJob { uint32_t offset, count; GpuSkinReadback callback; };
    struct Slot { VulkanBuffer palette, results; VkDescriptorPool pool = VK_NULL_HANDLE; std::vector<ReadbackJob> readbacks; };
    bool AllocateSet(VkDescriptorSet& set, const VkBuffer buffers[4]);
    Mesh* CaptureMesh(const SGpuSkinningData& data);
    VulkanContext* m_context = nullptr;
    VulkanResourceManager* m_resources = nullptr;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_skinPipeline = VK_NULL_HANDLE, m_patchPipeline = VK_NULL_HANDLE, m_morphPipeline = VK_NULL_HANDLE;
    VkPipeline m_shadowPipeline = VK_NULL_HANDLE;
    Slot m_slots[2];
    uint32_t m_slot = 0, m_frame = 0, m_resultWords = 0;
    std::map<uint64_t, Mesh> m_meshes;
    std::map<uint64_t, ShadowMesh> m_shadowMeshes;
    std::map<std::pair<uint64_t, const float*>, uint32_t> m_poseSources;
    std::map<const void*, Binding> m_poseByIdentity;
    std::vector<Pose> m_poses;
    std::vector<Patch> m_patches;
    std::vector<MorphJob> m_morphs;
    std::vector<ReadbackJob> m_readbacks;
    std::vector<float> m_palette;
    PFN_vkCreateShaderModule m_createShaderModule = nullptr;
    PFN_vkDestroyShaderModule m_destroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout m_createDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout m_destroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout m_createPipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout m_destroyPipelineLayout = nullptr;
    PFN_vkCreateComputePipelines m_createComputePipelines = nullptr;
    PFN_vkDestroyPipeline m_destroyPipeline = nullptr;
    PFN_vkCreateDescriptorPool m_createDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool m_destroyDescriptorPool = nullptr;
    PFN_vkResetDescriptorPool m_resetDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets m_allocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets m_updateDescriptorSets = nullptr;
    PFN_vkCmdBindPipeline m_cmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets m_cmdBindDescriptorSets = nullptr;
    PFN_vkCmdPushConstants m_cmdPushConstants = nullptr;
    PFN_vkCmdDispatch m_cmdDispatch = nullptr;
    PFN_vkCmdPipelineBarrier m_cmdPipelineBarrier = nullptr;
};
}
#endif
