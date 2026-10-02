#include "VulkanPipelineFactory.h"

#include <cstdio>
#include <cstring>
#include <vector>

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
    m_createPipelineCache = reinterpret_cast<PFN_vkCreatePipelineCache>(
        getDeviceProcAddr(context.GetDevice(), "vkCreatePipelineCache"));
    m_destroyPipelineCache = reinterpret_cast<PFN_vkDestroyPipelineCache>(
        getDeviceProcAddr(context.GetDevice(), "vkDestroyPipelineCache"));
    m_getPipelineCacheData = reinterpret_cast<PFN_vkGetPipelineCacheData>(
        getDeviceProcAddr(context.GetDevice(), "vkGetPipelineCacheData"));
    if (m_createPipelineCache && m_destroyPipelineCache)
    {
        VkPipelineCacheCreateInfo cacheInfo{};
        cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        std::vector<unsigned char> savedCache;
#if defined(__ANDROID__)
        if (FILE* file = fopen("/sdcard/FarCry/vulkan_pipeline_cache.bin", "rb"))
        {
            if (fseek(file, 0, SEEK_END) == 0)
            {
                const long size = ftell(file);
                if (size > 0 && size <= 16 * 1024 * 1024 && fseek(file, 0, SEEK_SET) == 0)
                {
                    savedCache.resize(static_cast<size_t>(size));
                    if (fread(savedCache.data(), 1, savedCache.size(), file) != savedCache.size())
                        savedCache.clear();
                }
            }
            fclose(file);
        }
#endif
        cacheInfo.initialDataSize = savedCache.size();
        cacheInfo.pInitialData = savedCache.empty() ? nullptr : savedCache.data();
        // Share driver compilation results across all material/state variants
        // encountered during streaming; callers still own the pipelines.
        if (m_createPipelineCache(context.GetDevice(), &cacheInfo, nullptr,
                                  &m_pipelineCache) != VK_SUCCESS)
        {
            // A driver update or truncated file must not prevent rendering.
            cacheInfo.initialDataSize = 0;
            cacheInfo.pInitialData = nullptr;
            if (m_createPipelineCache(context.GetDevice(), &cacheInfo, nullptr,
                                      &m_pipelineCache) != VK_SUCCESS)
                m_pipelineCache = VK_NULL_HANDLE;
        }
    }
    return true;
}

bool VulkanPipelineFactory::CreateGraphicsPipeline(const VulkanGraphicsPipelineDesc& desc,
                                                    VkPipeline& pipeline)
{
    m_lastResult = VK_SUCCESS;
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
    if (desc.hasColorWriteMaskOverride)
        legacyState.colorWriteMask = desc.colorWriteMaskOverride;
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
    const uint32_t stereoEnabled = desc.multiview ? 1u : 0u;
    VkSpecializationMapEntry stereoEntry{63, 0, sizeof(uint32_t)};
    VkSpecializationInfo stereoSpecialization{1, &stereoEntry, sizeof(uint32_t), &stereoEnabled};
    if (desc.supportsStereoTransform)
        stages[0].pSpecializationInfo = &stereoSpecialization;
    uint32_t specializationData[64] = {
        static_cast<uint32_t>(legacyState.alphaTest), desc.stage0ColorMode, desc.stage0AlphaMode,
        desc.stage1ColorMode, desc.stage1AlphaMode, desc.stage0ColorArg, desc.stage0AlphaArg,
        desc.stage0Constant, desc.stage1ColorArg, desc.stage1AlphaArg, desc.stage1Constant,
        desc.hasSecondaryColor ? 1u : 0u, desc.stage2ColorMode, desc.stage2AlphaMode,
        desc.stage2ColorArg, desc.stage2AlphaArg, desc.stage2Constant,
        desc.supportsStage1Combine ? 1u : 0u,
        desc.stage2UsesTexCoord1 ? 1u : 0u,
        desc.stage3ColorMode, desc.stage3AlphaMode, desc.stage3ColorArg,
        desc.stage3AlphaArg, desc.stage3Constant,
        desc.stage3UsesTexCoord1 ? 1u : 0u,
        desc.supportsStage3Combine ? 1u : 0u,
        desc.supportsStage2Combine ? 1u : 0u,
        desc.stages4To7[0].colorMode, desc.stages4To7[0].alphaMode,
        desc.stages4To7[0].colorArg, desc.stages4To7[0].alphaArg,
        desc.stages4To7[0].constant, desc.stages4To7[0].useTexCoord1 ? 1u : 0u,
        desc.stages4To7[0].enabled ? 1u : 0u,
        desc.stages4To7[1].colorMode, desc.stages4To7[1].alphaMode,
        desc.stages4To7[1].colorArg, desc.stages4To7[1].alphaArg,
        desc.stages4To7[1].constant, desc.stages4To7[1].useTexCoord1 ? 1u : 0u,
        desc.stages4To7[1].enabled ? 1u : 0u,
        desc.stages4To7[2].colorMode, desc.stages4To7[2].alphaMode,
        desc.stages4To7[2].colorArg, desc.stages4To7[2].alphaArg,
        desc.stages4To7[2].constant, desc.stages4To7[2].useTexCoord1 ? 1u : 0u,
        desc.stages4To7[2].enabled ? 1u : 0u,
        desc.stages4To7[3].colorMode, desc.stages4To7[3].alphaMode,
        desc.stages4To7[3].colorArg, desc.stages4To7[3].alphaArg,
        desc.stages4To7[3].constant, desc.stages4To7[3].useTexCoord1 ? 1u : 0u,
        desc.stages4To7[3].enabled ? 1u : 0u
    };
    // IDs 55..58 are LOD biases, 59/60 select UV sets, and 61 enables
    // directional lightmap lighting. Assign the tail explicitly: placing
    // the lightmap flag after only four LOD slots previously wrote index 59,
    // leaving index 61 zero and combining normal/direction maps as colors.
    specializationData[59] = desc.stage1UsesTexCoord1 ? 1u : 0u;
    specializationData[60] = desc.stage0UsesTexCoord1 ? 1u : 0u;
    specializationData[61] = desc.directionalLightmap ? 1u : 0u;
    specializationData[62] = desc.fragmentDiscardEnabled ? 1u : 0u;
    for (uint32_t stageIndex = 0; stageIndex < 4; ++stageIndex)
        std::memcpy(&specializationData[55 + stageIndex],
                    &desc.stages4To7[stageIndex].lodBias, sizeof(uint32_t));
    specializationData[63] = desc.simpleDecalMode ? 1u : 0u;
    VkSpecializationMapEntry specializationEntries[64]{};
    VkSpecializationInfo alphaTestSpecialization{};
    uint32_t specializationCount = 0;
    if (desc.supportsDiscardSpecialization || desc.supportsAlphaTest || desc.supportsStage0Combine || desc.supportsStage1Combine ||
        desc.supportsStage2Combine || desc.supportsStage3Combine ||
        desc.stages4To7[0].enabled || desc.stages4To7[1].enabled ||
        desc.stages4To7[2].enabled || desc.stages4To7[3].enabled)
    {
        const auto addSpecialization = [&](uint32_t constantId, uint32_t dataIndex)
        {
            VkSpecializationMapEntry& entry = specializationEntries[specializationCount++];
            entry.constantID = constantId;
            entry.offset = sizeof(uint32_t) * dataIndex;
            entry.size = sizeof(uint32_t);
        };
        addSpecialization(0, 0);
        if (desc.supportsDiscardSpecialization)
            addSpecialization(62, 62);
        if (desc.supportsStage0Combine)
        {
            addSpecialization(1, 1); addSpecialization(2, 2);
            addSpecialization(5, 5); addSpecialization(6, 6); addSpecialization(7, 7);
            addSpecialization(60, 60);
        }
        if (desc.supportsStage1Combine)
        {
            addSpecialization(3, 3); addSpecialization(4, 4);
            addSpecialization(8, 8); addSpecialization(9, 9); addSpecialization(10, 10);
            addSpecialization(59, 59);
        }
        if (desc.supportsStage2Combine)
        {
            addSpecialization(12, 12); addSpecialization(13, 13);
            addSpecialization(14, 14); addSpecialization(15, 15);
            addSpecialization(16, 16);
            addSpecialization(17, 17);
            addSpecialization(18, 18);
        }
        if (desc.supportsStage3Combine)
        {
            addSpecialization(19, 19); addSpecialization(20, 20);
            addSpecialization(21, 21); addSpecialization(22, 22);
            addSpecialization(23, 23); addSpecialization(24, 24);
            addSpecialization(25, 25); addSpecialization(26, 26);
        }
        for (uint32_t stageIndex = 0; stageIndex < 4; ++stageIndex)
        {
            if (!desc.stages4To7[stageIndex].enabled)
                continue;
            const uint32_t dataIndex = 27 + stageIndex * 7;
            const uint32_t constantId = 27 + stageIndex * 7;
            addSpecialization(constantId + 0, dataIndex + 0);
            addSpecialization(constantId + 1, dataIndex + 1);
            addSpecialization(constantId + 2, dataIndex + 2);
            addSpecialization(constantId + 3, dataIndex + 3);
            addSpecialization(constantId + 4, dataIndex + 4);
            addSpecialization(constantId + 5, dataIndex + 5);
            addSpecialization(constantId + 6, dataIndex + 6);
            addSpecialization(55 + stageIndex, 55 + stageIndex);
        }
        if (desc.supportsStage0Combine || desc.supportsStage1Combine)
            addSpecialization(11, 11);
        if (desc.directionalLightmap)
            addSpecialization(61, 61);
        if (desc.simpleDecalMode)
            addSpecialization(64, 63);
        alphaTestSpecialization.mapEntryCount = specializationCount;
        alphaTestSpecialization.pMapEntries = specializationEntries;
        alphaTestSpecialization.dataSize = sizeof(specializationData);
        alphaTestSpecialization.pData = specializationData;
        stages[1].pSpecializationInfo = &alphaTestSpecialization;
    }

    VkVertexInputBindingDescription bindings[3]{};
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
    if (desc.hasLightmapTexCoords)
    {
        if (!desc.hasTangents)
        {
            // Keep binding numbers dense when the lightmap stream occupies
            // binding 2 but no tangent stream uses binding 1.
            bindings[1].binding = 1;
            bindings[1].stride = sizeof(float) * 2;
            bindings[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        }
        bindings[2].binding = 2;
        bindings[2].stride = sizeof(float) * 2;
        bindings[2].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bindingCount = 3;
        bool replacedUv1 = false;
        for (uint32_t i = 0; i < vertexFormat.attributeCount; ++i)
        {
            VkVertexInputAttributeDescription& attribute = vertexFormat.attributes[i];
            if (attribute.location == 5)
            {
                attribute.binding = 2;
                attribute.offset = 0;
                replacedUv1 = true;
                break;
            }
        }
        if (!replacedUv1)
        {
            if (vertexFormat.attributeCount >=
                sizeof(vertexFormat.attributes) / sizeof(vertexFormat.attributes[0]))
            {
                SetError("lightmap texture coordinates exceed the vertex attribute limit");
                return false;
            }
            VkVertexInputAttributeDescription& attribute =
                vertexFormat.attributes[vertexFormat.attributeCount++];
            attribute.location = 5;
            attribute.binding = 2;
            attribute.format = VK_FORMAT_R32G32_SFLOAT;
            attribute.offset = 0;
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
    // The source renderer enables GL_POLYGON_OFFSET_FILL, never OFFSET_LINE.
    rasterization.depthBiasEnable = desc.depthBias && !legacyState.polygonLine ? VK_TRUE : VK_FALSE;
    rasterization.depthBiasConstantFactor = desc.depthBiasConstantFactor;
    rasterization.depthBiasSlopeFactor = desc.depthBiasSlopeFactor;
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
    m_lastResult = m_createGraphicsPipelines(m_context->GetDevice(), m_pipelineCache,
                                              1, &createInfo, nullptr, &pipeline);
    if (m_lastResult != VK_SUCCESS)
    {
        std::snprintf(m_lastError, sizeof(m_lastError),
                      "vkCreateGraphicsPipelines failed (VkResult %d, vertex format %u, topology %u, specialization entries %u)",
                      static_cast<int>(m_lastResult), desc.cryVertexFormat,
                      static_cast<uint32_t>(desc.topology), specializationCount);
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
#if defined(__ANDROID__)
    // Save only during orderly shutdown, never in the frame/streaming path.
    if (m_context && m_pipelineCache && m_getPipelineCacheData)
    {
        size_t size = 0;
        if (m_getPipelineCacheData(m_context->GetDevice(), m_pipelineCache, &size, nullptr) == VK_SUCCESS &&
            size > 0 && size <= 16 * 1024 * 1024)
        {
            std::vector<unsigned char> bytes(size);
            if (m_getPipelineCacheData(m_context->GetDevice(), m_pipelineCache, &size, bytes.data()) == VK_SUCCESS)
                if (FILE* file = fopen("/sdcard/FarCry/vulkan_pipeline_cache.bin", "wb"))
                {
                    fwrite(bytes.data(), 1, size, file);
                    fclose(file);
                }
        }
    }
#endif
    if (m_context && m_pipelineCache && m_destroyPipelineCache)
        m_destroyPipelineCache(m_context->GetDevice(), m_pipelineCache, nullptr);
    m_pipelineCache = VK_NULL_HANDLE;
    m_createPipelineCache = nullptr;
    m_destroyPipelineCache = nullptr;
    m_getPipelineCacheData = nullptr;
    m_context = nullptr;
    m_createGraphicsPipelines = nullptr;
    m_destroyPipeline = nullptr;
}
}
