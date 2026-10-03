#include "VulkanGpuSkinning.h"
#include "VulkanSkinShaders.h"
#include <cstring>
#include <limits>
#include <algorithm>

namespace CryVR {
bool VulkanGpuSkinning::Initialize(VulkanContext& context, VulkanResourceManager& resources)
{
    m_context = &context; m_resources = &resources;
    auto get = reinterpret_cast<PFN_vkGetDeviceProcAddr>(context.GetInstanceProcAddr()(context.GetInstance(), "vkGetDeviceProcAddr"));
    if (!get) return false;
#define LOAD(field, name) field = reinterpret_cast<PFN_vk##name>(get(context.GetDevice(), "vk" #name)); if (!field) return false
    LOAD(m_createShaderModule, CreateShaderModule); LOAD(m_destroyShaderModule, DestroyShaderModule);
    LOAD(m_createDescriptorSetLayout, CreateDescriptorSetLayout); LOAD(m_destroyDescriptorSetLayout, DestroyDescriptorSetLayout);
    LOAD(m_createPipelineLayout, CreatePipelineLayout); LOAD(m_destroyPipelineLayout, DestroyPipelineLayout);
    LOAD(m_createComputePipelines, CreateComputePipelines); LOAD(m_destroyPipeline, DestroyPipeline);
    LOAD(m_createDescriptorPool, CreateDescriptorPool); LOAD(m_destroyDescriptorPool, DestroyDescriptorPool);
    LOAD(m_resetDescriptorPool, ResetDescriptorPool); LOAD(m_allocateDescriptorSets, AllocateDescriptorSets);
    LOAD(m_updateDescriptorSets, UpdateDescriptorSets); LOAD(m_cmdBindPipeline, CmdBindPipeline);
    LOAD(m_cmdBindDescriptorSets, CmdBindDescriptorSets); LOAD(m_cmdPushConstants, CmdPushConstants);
    LOAD(m_cmdDispatch, CmdDispatch); LOAD(m_cmdPipelineBarrier, CmdPipelineBarrier);
#undef LOAD
    VkDescriptorSetLayoutBinding bindings[4]{};
    for (uint32_t i=0; i<4; ++i) { bindings[i].binding=i; bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; bindings[i].descriptorCount=1; bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT; }
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount=4; setInfo.pBindings=bindings;
    if (m_createDescriptorSetLayout(context.GetDevice(), &setInfo, nullptr, &m_setLayout) != VK_SUCCESS) return false;
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount=1; layoutInfo.pSetLayouts=&m_setLayout;
    layoutInfo.pushConstantRangeCount=1; layoutInfo.pPushConstantRanges=&range;
    if (m_createPipelineLayout(context.GetDevice(), &layoutInfo, nullptr, &m_layout) != VK_SUCCESS) return false;
    const uint32_t* codes[4]={kVrSkinComputeSpirv, kVrSkinPatchComputeSpirv, kVrSkinMorphComputeSpirv, kVrSkinShadowComputeSpirv};
    const size_t sizes[4]={sizeof(kVrSkinComputeSpirv), sizeof(kVrSkinPatchComputeSpirv), sizeof(kVrSkinMorphComputeSpirv), sizeof(kVrSkinShadowComputeSpirv)};
    VkPipeline* pipelines[4]={&m_skinPipeline, &m_patchPipeline, &m_morphPipeline, &m_shadowPipeline};
    for (uint32_t i=0; i<4; ++i) {
        VkShaderModule module=VK_NULL_HANDLE;
        VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shaderInfo.codeSize=sizes[i]; shaderInfo.pCode=codes[i];
        if (m_createShaderModule(context.GetDevice(), &shaderInfo, nullptr, &module) != VK_SUCCESS) return false;
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; pipelineInfo.stage.module=module;
        pipelineInfo.stage.pName="main"; pipelineInfo.layout=m_layout;
        VkResult result=m_createComputePipelines(context.GetDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, pipelines[i]);
        m_destroyShaderModule(context.GetDevice(), module, nullptr);
        if (result != VK_SUCCESS) return false;
    }
    for (Slot& slot : m_slots) {
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4*16384};
        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.maxSets=16384; info.poolSizeCount=1; info.pPoolSizes=&size;
        if (m_createDescriptorPool(context.GetDevice(), &info, nullptr, &slot.pool) != VK_SUCCESS) return false;
    }
    return true;
}

void VulkanGpuSkinning::BeginFrame()
{
    ++m_frame; m_resultWords=0;
    m_poseByIdentity.clear(); m_poses.clear(); m_patches.clear(); m_morphs.clear(); m_palette.clear();
    m_poseSources.clear();
    m_readbacks.clear();
}

VulkanGpuSkinning::Mesh* VulkanGpuSkinning::CaptureMesh(const SGpuSkinningData& data)
{
    auto meshIt=m_meshes.find(data.key);
    if (meshIt==m_meshes.end()) {
        // One immutable upload per geometry/LOD, shared by all NPC instances.
        Mesh mesh{}; mesh.vertexCount=static_cast<uint32_t>(data.vertices.size());
        mesh.linksWordOffset=mesh.vertexCount*16;
        for (const SGpuSkinVertex& vertex : data.vertices) {
            if (uint64_t(vertex.positionFirst)+vertex.positionCount>data.influences.size() ||
                uint64_t(vertex.normalFirst)+vertex.normalCount>data.influences.size()) return nullptr;
            if (vertex.tangentBone!=UINT32_MAX) mesh.requiredBones=std::max(mesh.requiredBones,vertex.tangentBone+1);
        }
        for (const SGpuSkinInfluence& influence : data.influences) {
            if (influence.bone==UINT32_MAX) return nullptr;
            mesh.requiredBones=std::max(mesh.requiredBones,influence.bone+1);
        }
        const size_t vertexBytes=data.vertices.size()*sizeof(SGpuSkinVertex);
        std::vector<uint8_t> packed(vertexBytes+data.influences.size()*sizeof(SGpuSkinInfluence));
        std::memcpy(packed.data(), data.vertices.data(), vertexBytes);
        std::memcpy(packed.data()+vertexBytes, data.influences.data(), data.influences.size()*sizeof(SGpuSkinInfluence));
        if (!m_resources->CreateBuffer(packed.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, mesh.input)) return nullptr;
        if (!m_resources->UploadBuffer(mesh.input, packed.data(), packed.size())) {
            m_resources->DestroyBuffer(mesh.input); return nullptr;
        }
        meshIt=m_meshes.emplace(data.key, mesh).first;
    }
    meshIt->second.lastFrame=m_frame;
    return &meshIt->second;
}

bool VulkanGpuSkinning::Queue(const void* identity, const SGpuSkinningData& data, const float* bones, uint32_t boneCount)
{
    if (!m_skinPipeline || !identity || !bones || !boneCount || data.vertices.empty() || data.influences.empty()) return false;
    auto found=m_poseByIdentity.find(identity);
    if (found != m_poseByIdentity.end()) return true;
    auto shared=m_poseSources.find(std::make_pair(data.key,bones));
    if (shared!=m_poseSources.end()) {
        m_poseByIdentity[identity]=Binding{shared->second,UINT32_MAX,static_cast<uint32_t>(data.vertices.size()),0}; return true;
    }
    Mesh* captured=CaptureMesh(data);
    if (!captured || captured->requiredBones>boneCount) return false;
    Mesh& mesh=*captured;
    if (uint64_t(m_resultWords)+uint64_t(mesh.vertexCount)*20 > UINT32_MAX || m_poses.size()>=1024) return false;
    mesh.lastFrame=m_frame;
    Pose pose{}; pose.meshKey=data.key; pose.resultWordOffset=m_resultWords;
    m_palette.resize((m_palette.size()+15u)&~size_t(15u), 0.0f);
    pose.paletteOffset=static_cast<uint32_t>(m_palette.size()/16);
    m_palette.insert(m_palette.end(), bones, bones+size_t(boneCount)*16);
    m_resultWords+=mesh.vertexCount*20;
    m_poseByIdentity.emplace(identity, Binding{static_cast<uint32_t>(m_poses.size()), UINT32_MAX, mesh.vertexCount,0});
    m_poseSources.emplace(std::make_pair(data.key,bones),static_cast<uint32_t>(m_poses.size())); m_poses.push_back(pose);
    return true;
}

bool VulkanGpuSkinning::Morph(const void* identity, const SGpuSkinningData& data, float weight, float normalAmplify)
{
    if (data.influences.empty() || weight==0) return true;
    auto found=m_poseByIdentity.find(identity);
    if (found==m_poseByIdentity.end() || data.vertices.size()!=found->second.count) return false;
    for (MorphJob& morph : m_morphs)
        if (morph.pose==found->second.pose && morph.meshKey==data.key) {
            morph.weight=weight; morph.normalAmplify=normalAmplify; return true;
        }
    if (!CaptureMesh(data)) return false;
    m_morphs.push_back(MorphJob{data.key,found->second.pose,weight,normalAmplify,VK_NULL_HANDLE});
    return true;
}

bool VulkanGpuSkinning::Remap(const void* identity, const void* source, const uint32_t* mapping, uint32_t count)
{
    auto found=m_poseByIdentity.find(source);
    if (found==m_poseByIdentity.end() || !mapping || !count) return false;
    for (uint32_t i=0; i<count; ++i) if (mapping[i]>=found->second.count) return false;
    const uint32_t offset=static_cast<uint32_t>(m_palette.size());
    m_palette.resize(m_palette.size()+count);
    std::memcpy(m_palette.data()+offset,mapping,size_t(count)*4);
    m_poseByIdentity[identity]=Binding{found->second.pose,offset,count,0}; return true;
}

bool VulkanGpuSkinning::Readback(const void* identity, const GpuSkinReadback& callback)
{
    auto found=m_poseByIdentity.find(identity);
    if (found==m_poseByIdentity.end() || !callback || found->second.shadowKey) return false;
    const Pose& pose=m_poses[found->second.pose];
    m_readbacks.push_back(ReadbackJob{pose.resultWordOffset,m_meshes.at(pose.meshKey).vertexCount,callback});
    return true;
}

void VulkanGpuSkinning::Complete(uint32_t slot)
{
    // Called after the existing frame fence, never requesting a new wait.
    Slot& completed=m_slots[slot];
    std::vector<ReadbackJob> callbacks;
    callbacks.swap(completed.readbacks);
    const float* data=static_cast<const float*>(completed.results.mappedData);
    if (data) for (const ReadbackJob& job : callbacks) job.callback(data+job.offset,job.count);
}

bool VulkanGpuSkinning::Shadow(const void* identity, const void* source, const SGpuSkinningData& mesh,
    const SGpuSkinShadowData& topology, const float* bones, uint32_t boneCount, const float* light, float extent, uint32_t first)
{
    if (!topology.key || !Queue(source,mesh,bones,boneCount)) return false;
    auto found=m_shadowMeshes.find(topology.key);
    if (found==m_shadowMeshes.end()) {
        ShadowMesh shadow{}; shadow.faces=static_cast<uint32_t>(topology.faces.size()/3); shadow.edges=static_cast<uint32_t>(topology.edges.size()/4);
        std::vector<uint32_t> packed=topology.faces;
        packed.insert(packed.end(),topology.edges.begin(),topology.edges.end());
        if (!m_resources->CreateBuffer(packed.size()*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,shadow.input)) return false;
        if (!m_resources->UploadBuffer(shadow.input,packed.data(),packed.size()*4)) { m_resources->DestroyBuffer(shadow.input); return false; }
        found=m_shadowMeshes.emplace(topology.key,shadow).first;
    }
    found->second.lastFrame=m_frame;
    uint32_t offset=static_cast<uint32_t>(m_palette.size());
    m_palette.insert(m_palette.end(),light,light+3); m_palette.push_back(extent);
    m_poseByIdentity[identity]=Binding{m_poseByIdentity.at(source).pose,offset,
        (found->second.faces+found->second.edges)*6,topology.key,first}; return true;
}

bool VulkanGpuSkinning::AddPatch(const void* identity, VkBuffer destination, uint32_t first, uint32_t count,
                               VkDeviceSize offset, uint32_t stride, uint32_t normalOffset, VkDeviceSize tangentOffset)
{
    auto found=m_poseByIdentity.find(identity);
    if (found==m_poseByIdentity.end()) return false;
    const Binding& binding=found->second;
    if (binding.shadowKey) first+=binding.shadowFirst;
    const Pose& pose=m_poses[binding.pose];
    if (uint64_t(first)+count > binding.count || offset/4>UINT32_MAX ||
        (tangentOffset != VK_WHOLE_SIZE && tangentOffset/4>UINT32_MAX) || m_patches.size()+m_poses.size()+m_morphs.size()>=16384) return false;
    Patch patch{}; patch.destination=destination;
    patch.constants[0]=pose.resultWordOffset; patch.constants[1]=first; patch.constants[2]=count;
    patch.constants[3]=static_cast<uint32_t>(offset/4); patch.constants[4]=stride/4;
    patch.constants[5]=normalOffset==UINT32_MAX ? UINT32_MAX : normalOffset/4;
    patch.constants[6]=tangentOffset==VK_WHOLE_SIZE ? UINT32_MAX : static_cast<uint32_t>(tangentOffset/4);
    patch.constants[7]=binding.remapOffset;
    patch.shadowKey=binding.shadowKey;
    if (binding.shadowKey) {
        const ShadowMesh& shadow=m_shadowMeshes.at(binding.shadowKey);
        uint32_t constants[8]={pose.resultWordOffset,shadow.faces,shadow.edges,static_cast<uint32_t>(offset/4),first,count,stride/4,binding.remapOffset};
        std::memcpy(patch.constants,constants,sizeof(constants));
    }
    m_patches.push_back(patch); return true;
}

bool VulkanGpuSkinning::AllocateSet(VkDescriptorSet& set, const VkBuffer buffers[4])
{
    VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    info.descriptorPool=m_slots[m_slot].pool; info.descriptorSetCount=1; info.pSetLayouts=&m_setLayout;
    if (m_allocateDescriptorSets(m_context->GetDevice(), &info, &set) != VK_SUCCESS) return false;
    VkDescriptorBufferInfo bufferInfo[4]{}; VkWriteDescriptorSet writes[4]{};
    for (uint32_t i=0; i<4; ++i) {
        bufferInfo[i]={buffers[i],0,VK_WHOLE_SIZE};
        writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=set;
        writes[i].dstBinding=i; writes[i].descriptorCount=1; writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo=&bufferInfo[i];
    }
    m_updateDescriptorSets(m_context->GetDevice(),4,writes,0,nullptr); return true;
}

bool VulkanGpuSkinning::Prepare(uint32_t frameSlot)
{
    if (m_poses.empty()) return true;
    m_slot=frameSlot; Slot& slot=m_slots[m_slot];
    // This is the free frame slot. The opposite slot may still be on GPU.
    const auto ensure=[this](VulkanBuffer& buffer, VkDeviceSize size, VkMemoryPropertyFlags memory) {
        if (buffer.size>=size) return true;
        m_resources->DestroyBuffer(buffer);
        VkDeviceSize capacity=65536; while (capacity<size) capacity*=2;
        return m_resources->CreateBuffer(capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, memory, buffer);
    };
    if (!ensure(slot.palette, m_palette.size()*sizeof(float), VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
        !ensure(slot.results, VkDeviceSize(m_resultWords)*4, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) return false;
    const uint8_t zero=0;
    if (!slot.results.mappedData && !m_resources->UploadBuffer(slot.results,&zero,1)) return false;
    if (!m_resources->UploadBuffer(slot.palette, m_palette.data(), m_palette.size()*sizeof(float)) ||
        m_resetDescriptorPool(m_context->GetDevice(), slot.pool, 0)!=VK_SUCCESS) return false;
    for (Pose& pose : m_poses) {
        VkBuffer buffers[4]={m_meshes.at(pose.meshKey).input.buffer, slot.palette.buffer, slot.results.buffer,slot.results.buffer};
        if (!AllocateSet(pose.descriptor,buffers)) return false;
    }
    for (MorphJob& morph : m_morphs) {
        VkBuffer buffers[4]={m_meshes.at(morph.meshKey).input.buffer,slot.palette.buffer,slot.results.buffer,slot.results.buffer};
        if (!AllocateSet(morph.descriptor,buffers)) return false;
    }
    for (Patch& patch : m_patches) {
        VkBuffer buffers[4]={slot.results.buffer,patch.destination,slot.palette.buffer,
            patch.shadowKey ? m_shadowMeshes.at(patch.shadowKey).input.buffer : slot.results.buffer};
        if (!AllocateSet(patch.descriptor,buffers)) return false;
    }
    slot.readbacks.swap(m_readbacks);
    return true;
}

void VulkanGpuSkinning::Record(VkCommandBuffer command)
{
    if (m_poses.empty()) return;
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    m_cmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    m_cmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,m_skinPipeline);
    for (const Pose& pose : m_poses) {
        const Mesh& mesh=m_meshes.at(pose.meshKey);
        uint32_t constants[8]={mesh.vertexCount,mesh.linksWordOffset,pose.paletteOffset,pose.resultWordOffset};
        m_cmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,m_layout,0,1,&pose.descriptor,0,nullptr);
        m_cmdPushConstants(command,m_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(constants),constants);
        m_cmdDispatch(command,(mesh.vertexCount+63)/64,1,1);
    }
    barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
    m_cmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    m_cmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,m_morphPipeline);
    for (const MorphJob& morph : m_morphs) {
        const Pose& pose=m_poses[morph.pose]; const Mesh& mesh=m_meshes.at(morph.meshKey);
        struct { uint32_t count, links, palette, output; float weight, normalAmplify; uint32_t reserved[2]; }
            constants{mesh.vertexCount,mesh.linksWordOffset,pose.paletteOffset,pose.resultWordOffset,morph.weight,morph.normalAmplify,{0,0}};
        m_cmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,m_layout,0,1,&morph.descriptor,0,nullptr);
        m_cmdPushConstants(command,m_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(constants),&constants);
        m_cmdDispatch(command,(mesh.vertexCount+63)/64,1,1);
        m_cmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    }
    m_cmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,m_patchPipeline);
    for (const Patch& patch : m_patches) {
        m_cmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,patch.shadowKey ? m_shadowPipeline : m_patchPipeline);
        m_cmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,m_layout,0,1,&patch.descriptor,0,nullptr);
        m_cmdPushConstants(command,m_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(patch.constants),patch.constants);
        m_cmdDispatch(command,((patch.shadowKey ? patch.constants[5] : patch.constants[2])+63)/64,1,1);
    }
    barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    m_cmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    if (!m_slots[m_slot].readbacks.empty()) {
        barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        m_cmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    }
}

void VulkanGpuSkinning::CollectUnused()
{
    for (auto it=m_meshes.begin(); it!=m_meshes.end();) {
        if (m_frame-it->second.lastFrame>300) { m_resources->DestroyBuffer(it->second.input); it=m_meshes.erase(it); }
        else ++it;
    }
    for (auto it=m_shadowMeshes.begin(); it!=m_shadowMeshes.end();) {
        if (m_frame-it->second.lastFrame>300) { m_resources->DestroyBuffer(it->second.input); it=m_shadowMeshes.erase(it); }
        else ++it;
    }
}

void VulkanGpuSkinning::Shutdown()
{
    if (!m_context) return;
    VkDevice device=m_context->GetDevice();
    for (Slot& slot : m_slots) {
        if (slot.pool && m_destroyDescriptorPool) m_destroyDescriptorPool(device,slot.pool,nullptr);
        m_resources->DestroyBuffer(slot.palette); m_resources->DestroyBuffer(slot.results); slot.pool=VK_NULL_HANDLE;
        slot.readbacks.clear();
    }
    for (auto& mesh : m_meshes) m_resources->DestroyBuffer(mesh.second.input);
    m_meshes.clear();
    for (auto& mesh : m_shadowMeshes) m_resources->DestroyBuffer(mesh.second.input);
    m_shadowMeshes.clear();
    if (m_skinPipeline && m_destroyPipeline) m_destroyPipeline(device,m_skinPipeline,nullptr);
    if (m_patchPipeline && m_destroyPipeline) m_destroyPipeline(device,m_patchPipeline,nullptr);
    if (m_morphPipeline && m_destroyPipeline) m_destroyPipeline(device,m_morphPipeline,nullptr);
    if (m_shadowPipeline && m_destroyPipeline) m_destroyPipeline(device,m_shadowPipeline,nullptr);
    if (m_layout && m_destroyPipelineLayout) m_destroyPipelineLayout(device,m_layout,nullptr);
    if (m_setLayout && m_destroyDescriptorSetLayout) m_destroyDescriptorSetLayout(device,m_setLayout,nullptr);
    m_skinPipeline=m_patchPipeline=m_morphPipeline=VK_NULL_HANDLE; m_layout=VK_NULL_HANDLE; m_setLayout=VK_NULL_HANDLE; m_context=nullptr;
    m_shadowPipeline=VK_NULL_HANDLE;
    BeginFrame();
}
}
