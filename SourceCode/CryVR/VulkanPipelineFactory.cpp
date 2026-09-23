#include "VulkanPipelineFactory.h"

#include <cstdio>
#include <cstring>

namespace CryVR
{
void VulkanPipelineFactory::SetError(const char* message)
{
    std::snprintf(m_lastError, sizeof(m_lastError), "%s", message ? message : "unknown error");
}

bool VulkanPipelineFactory::Initialize(VulkanContext& context)
{
    Shutdown();
    if (!context.IsInitialized())
    {
        SetError("Vulkan context is not initialized");
        return false;
    }
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        context.GetInstanceProcAddr()(context.GetInstance(), "vkGetDeviceProcAddr"));
    if (!getDeviceProcAddr)
    {
        SetError("vkGetDeviceProcAddr is unavailable");
        return false;
    }
    m_createGraphicsPipelines = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
        getDeviceProcAddr(context.GetDevice(), "vkCreateGraphicsPipelines"));
    m_destroyPipeline = reinterpret_cast<PFN_vkDestroyPipeline>(
        getDeviceProcAddr(context.GetDevice(), "vkDestroyPipeline"));
    if (!m_createGraphicsPipelines || !m_destroyPipeline)
    {
        SetError("Vulkan graphics pipeline functions are unavailable");
        Shutdown();
        return false;
    }
    m_context = &context;
    return true;
}

bool VulkanPipelineFactory::CreateGraphicsPipeline(const VulkanGraphicsPipelineDesc& desc,
                                                    VkPipeline& pipeline)
{
    pipeline = VK_NULL_HANDLE;
    if (!m_context || !desc.renderPass || !desc.layout || !desc.vertexShader ||
        !desc.fragmentShader || !desc.vertexEntry || !desc.fragmentEntry)
    {
        SetError("graphics pipeline description is incomplete");
        return false;
    }

    VulkanVertexFormat vertexFormat{};
    VulkanPipelineState legacyState{};
    if ((desc.hasNormalMap && !desc.hasTangents) ||
        (desc.useVertexInput && !GetVulkanVertexFormat(desc.cryVertexFormat, vertexFormat)) ||
        !DecodeLegacyPipelineState(desc.renderState, desc.cullMode, desc.mirror,
                                   desc.stencilTestState, legacyState))
    {
        SetError("legacy vertex format or render state is unsupported");
        return false;
    }
    if (legacyState.alphaTest != LegacyAlphaTestNone && !desc.supportsAlphaTest)
    {
        SetError("alpha-test state requires a shader variant that implements it");
        return false;
    }
    if (legacyState.polygonLine && !desc.supportsWireframe)
    {
        SetError("wireframe state requires the non-solid-fill device feature");
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = desc.vertexShader;
    stages[0].pName = desc.vertexEntry;
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = desc.fragmentShader;
    stages[1].pName = desc.fragmentEntry;
    uint32_t specializationData[12] = {
        static_cast<uint32_t>(legacyState.alphaTest), desc.stage0ColorMode, desc.stage0AlphaMode,
        desc.stage1ColorMode, desc.stage1AlphaMode, desc.stage0ColorArg, desc.stage0AlphaArg,
        desc.stage0Constant, desc.stage1ColorArg, desc.stage1AlphaArg, desc.stage1Constant,
        desc.hasSecondaryColor ? 1u : 0u
    };
    VkSpecializationMapEntry specializationEntries[12]{};
    VkSpecializationInfo alphaTestSpecialization{};
    if (desc.supportsAlphaTest || desc.supportsStage0Combine || desc.supportsStage1Combine)
    {
        uint32_t specializationCount = 0;
        const auto addSpecialization = [&](uint32_t constantId, uint32_t dataIndex)
        {
            VkSpecializationMapEntry& entry = specializationEntries[specializationCount++];
            entry.constantID = constantId;
            entry.offset = sizeof(uint32_t) * dataIndex;
            entry.size = sizeof(uint32_t);
        };
        addSpecialization(0, 0);
        if (desc.supportsStage0Combine)
        {
            addSpecialization(1, 1); addSpecialization(2, 2);
            addSpecialization(5, 5); addSpecialization(6, 6); addSpecialization(7, 7);
        }
        if (desc.supportsStage1Combine)
        {
            addSpecialization(3, 3); addSpecialization(4, 4);
            addSpecialization(8, 8); addSpecialization(9, 9); addSpecialization(10, 10);
        }
        if (desc.supportsStage0Combine || desc.supportsStage1Combine)
            addSpecialization(11, 11);
        alphaTestSpecialization.mapEntryCount = specializationCount;
        alphaTestSpecialization.pMapEntries = specializationEntries;
        alphaTestSpecialization.dataSize = sizeof(specializationData);
        alphaTestSpecialization.pData = specializationData;
        stages[1].pSpecializationInfo = &alphaTestSpecialization;
    }

    VkVertexInputBindingDescription bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].stride = vertexFormat.stride;
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    uint32_t bindingCount = 1;
    if (desc.hasTangents)
    {
        bindings[1].binding = 1;
        bindings[1].stride = sizeof(float) * 9;
        bindings[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bindingCount = 2;
        const uint32_t tangentLocations[] = { 6, 7, 8 };
        for (uint32_t i = 0; i < 3; ++i)
        {
            VkVertexInputAttributeDescription& attribute = vertexFormat.attributes[vertexFormat.attributeCount++];
            attribute.location = tangentLocations[i];
            attribute.binding = 1;
            attribute.format = VK_FORMAT_R32G32B32_SFLOAT;
            attribute.offset = i * sizeof(float) * 3;
        }
    }
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (desc.useVertexInput)
    {
        vertexInput.vertexBindingDescriptionCount = bindingCount;
        vertexInput.pVertexBindingDescriptions = bindings;
        vertexInput.vertexAttributeDescriptionCount = vertexFormat.attributeCount;
        vertexInput.pVertexAttributeDescriptions = vertexFormat.attributes;
    }

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = desc.topology;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkViewport staticViewport{};
    VkRect2D staticScissor{};
    if (!desc.dynamicViewport)
    {
        if (desc.viewportExtent.width == 0 || desc.viewportExtent.height == 0)
        {
            SetError("static pipeline viewport extent is empty");
            return false;
        }
        staticViewport.width = static_cast<float>(desc.viewportExtent.width);
        staticViewport.height = static_cast<float>(desc.viewportExtent.height);
        staticViewport.minDepth = 0.0f;
        staticViewport.maxDepth = 1.0f;
        staticScissor.extent = desc.viewportExtent;
        viewport.pViewports = &staticViewport;
        viewport.pScissors = &staticScissor;
    }

    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.depthClampEnable = VK_FALSE;
    rasterization.rasterizerDiscardEnable = VK_FALSE;
    rasterization.polygonMode = legacyState.polygonLine ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rasterization.cullMode = legacyState.cullMode;
    rasterization.frontFace = legacyState.frontFace;
    rasterization.depthBiasEnable = VK_FALSE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = legacyState.depthTestEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = legacyState.depthWriteEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = legacyState.depthCompareOp;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = legacyState.stencilTestEnable ? VK_TRUE : VK_FALSE;
    depthStencil.front = desc.stencilFront;
    depthStencil.back = desc.stencilBack;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = legacyState.blendEnable ? VK_TRUE : VK_FALSE;
    blendAttachment.srcColorBlendFactor = legacyState.srcColorBlendFactor;
    blendAttachment.dstColorBlendFactor = legacyState.dstColorBlendFactor;
    blendAttachment.colorBlendOp = legacyState.colorBlendOp;
    blendAttachment.srcAlphaBlendFactor = legacyState.srcAlphaBlendFactor;
    blendAttachment.dstAlphaBlendFactor = legacyState.dstAlphaBlendFactor;
    blendAttachment.alphaBlendOp = legacyState.alphaBlendOp;
    blendAttachment.colorWriteMask = legacyState.colorWriteMask;
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    VkPipelineDynamicStateCreateInfo dynamic{};
    VkDynamicState dynamicStates[5];
    uint32_t dynamicStateCount = 0;
    if (desc.dynamicViewport)
    {
        dynamicStates[dynamicStateCount++] = VK_DYNAMIC_STATE_VIEWPORT;
        dynamicStates[dynamicStateCount++] = VK_DYNAMIC_STATE_SCISSOR;
    }
    if (desc.dynamicStencil)
    {
        dynamicStates[dynamicStateCount++] = VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK;
        dynamicStates[dynamicStateCount++] = VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
        dynamicStates[dynamicStateCount++] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
    }
    if (dynamicStateCount)
    {
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = dynamicStateCount;
        dynamic.pDynamicStates = dynamicStates;
    }

    VkGraphicsPipelineCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    createInfo.stageCount = 2;
    createInfo.pStages = stages;
    createInfo.pVertexInputState = &vertexInput;
    createInfo.pInputAssemblyState = &inputAssembly;
    createInfo.pViewportState = &viewport;
    createInfo.pRasterizationState = &rasterization;
    createInfo.pMultisampleState = &multisample;
    createInfo.pDepthStencilState = &depthStencil;
    createInfo.pColorBlendState = &colorBlend;
    createInfo.pDynamicState = dynamicStateCount ? &dynamic : nullptr;
    createInfo.layout = desc.layout;
    createInfo.renderPass = desc.renderPass;
    createInfo.subpass = 0;
    const VkResult result = m_createGraphicsPipelines(m_context->GetDevice(), VK_NULL_HANDLE,
                                                       1, &createInfo, nullptr, &pipeline);
    if (result != VK_SUCCESS)
    {
        SetError("vkCreateGraphicsPipelines failed");
        pipeline = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void VulkanPipelineFactory::DestroyPipeline(VkPipeline& pipeline)
{
    if (m_context && pipeline && m_destroyPipeline)
        m_destroyPipeline(m_context->GetDevice(), pipeline, nullptr);
    pipeline = VK_NULL_HANDLE;
}

void VulkanPipelineFactory::Shutdown()
{
    m_context = nullptr;
    m_createGraphicsPipelines = nullptr;
    m_destroyPipeline = nullptr;
}
}
