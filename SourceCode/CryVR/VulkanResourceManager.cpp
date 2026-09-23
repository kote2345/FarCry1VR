#include "VulkanResourceManager.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

namespace CryVR
{
namespace
{
uint32_t CountMipLevels(uint32_t width, uint32_t height)
{
    uint32_t count = 1;
    while (width > 1 || height > 1)
    {
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
        ++count;
    }
    return count;
}

bool BuildRgbaMipChain(const void* sourcePixels, uint32_t width, uint32_t height,
                       std::vector<uint8_t>& pixels, std::vector<VkBufferImageCopy>& regions)
{
    if (!sourcePixels || !width || !height)
        return false;
    pixels.clear();
    regions.clear();
    const uint32_t levels = CountMipLevels(width, height);
    uint32_t levelWidth = width, levelHeight = height;
    size_t previousOffset = 0;
    uint32_t previousWidth = 0, previousHeight = 0;
    VkDeviceSize bufferOffset = 0;
    regions.reserve(levels);
    for (uint32_t level = 0; level < levels; ++level)
    {
        const size_t levelBytes = static_cast<size_t>(levelWidth) * levelHeight * 4;
        const size_t levelOffset = pixels.size();
        if (levelBytes > pixels.max_size() - levelOffset)
            return false;
        pixels.resize(levelOffset + levelBytes);
        uint8_t* destination = pixels.data() + levelOffset;
        if (level == 0)
            std::memcpy(destination, sourcePixels, levelBytes);
        else
        {
            const uint8_t* previous = pixels.data() + previousOffset;
            for (uint32_t y = 0; y < levelHeight; ++y)
            for (uint32_t x = 0; x < levelWidth; ++x)
            for (uint32_t channel = 0; channel < 4; ++channel)
            {
                uint32_t sum = 0, samples = 0;
                const uint32_t firstX = x * previousWidth / levelWidth;
                const uint32_t lastX = (x + 1) * previousWidth / levelWidth;
                const uint32_t firstY = y * previousHeight / levelHeight;
                const uint32_t lastY = (y + 1) * previousHeight / levelHeight;
                for (uint32_t py = firstY; py < lastY; ++py)
                for (uint32_t px = firstX; px < lastX; ++px)
                {
                    sum += previous[(static_cast<size_t>(py) * previousWidth + px) * 4 + channel];
                    ++samples;
                }
                destination[(static_cast<size_t>(y) * levelWidth + x) * 4 + channel] =
                    static_cast<uint8_t>((sum + samples / 2) / samples);
            }
        }
        VkBufferImageCopy region{};
        region.bufferOffset = bufferOffset;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = level;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = { levelWidth, levelHeight, 1 };
        regions.push_back(region);
        bufferOffset += levelBytes;
        previousOffset = levelOffset;
        previousWidth = levelWidth;
        previousHeight = levelHeight;
        levelWidth = levelWidth > 1 ? levelWidth / 2 : 1;
        levelHeight = levelHeight > 1 ? levelHeight / 2 : 1;
    }
    return true;
}

template<class T>
T LoadDevice(PFN_vkGetDeviceProcAddr getProc, VkDevice device, const char* name)
{
    return reinterpret_cast<T>(getProc(device, name));
}
}

VulkanResourceManager::~VulkanResourceManager()
{
    Shutdown();
}

void VulkanResourceManager::SetError(const char* message)
{
    std::snprintf(m_lastError, sizeof(m_lastError), "%s", message ? message : "unknown error");
}

bool VulkanResourceManager::LoadFunctions()
{
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = m_context->GetInstanceProcAddr();
    m_getPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        getInstanceProcAddr(m_context->GetInstance(), "vkGetPhysicalDeviceMemoryProperties"));
    m_getDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        getInstanceProcAddr(m_context->GetInstance(), "vkGetDeviceProcAddr"));
    if (!m_getPhysicalDeviceMemoryProperties || !m_getDeviceProcAddr)
    {
        SetError("Vulkan memory functions are unavailable");
        return false;
    }

#define LOAD_VK(field, name) m_##field = LoadDevice<PFN_vk##name>(m_getDeviceProcAddr, m_context->GetDevice(), "vk" #name)
    LOAD_VK(createCommandPool, CreateCommandPool);
    LOAD_VK(destroyCommandPool, DestroyCommandPool);
    LOAD_VK(allocateCommandBuffers, AllocateCommandBuffers);
    LOAD_VK(freeCommandBuffers, FreeCommandBuffers);
    LOAD_VK(beginCommandBuffer, BeginCommandBuffer);
    LOAD_VK(endCommandBuffer, EndCommandBuffer);
    LOAD_VK(queueSubmit, QueueSubmit);
    LOAD_VK(queueWaitIdle, QueueWaitIdle);
    LOAD_VK(createBuffer, CreateBuffer);
    LOAD_VK(destroyBuffer, DestroyBuffer);
    LOAD_VK(getBufferMemoryRequirements, GetBufferMemoryRequirements);
    LOAD_VK(allocateMemory, AllocateMemory);
    LOAD_VK(freeMemory, FreeMemory);
    LOAD_VK(bindBufferMemory, BindBufferMemory);
    LOAD_VK(mapMemory, MapMemory);
    LOAD_VK(unmapMemory, UnmapMemory);
    LOAD_VK(flushMappedMemoryRanges, FlushMappedMemoryRanges);
    LOAD_VK(cmdCopyBuffer, CmdCopyBuffer);
    LOAD_VK(createImage, CreateImage);
    LOAD_VK(destroyImage, DestroyImage);
    LOAD_VK(getImageMemoryRequirements, GetImageMemoryRequirements);
    LOAD_VK(bindImageMemory, BindImageMemory);
    LOAD_VK(createImageView, CreateImageView);
    LOAD_VK(destroyImageView, DestroyImageView);
    LOAD_VK(cmdPipelineBarrier, CmdPipelineBarrier);
    LOAD_VK(cmdCopyBufferToImage, CmdCopyBufferToImage);
#undef LOAD_VK

    return m_createCommandPool && m_destroyCommandPool && m_allocateCommandBuffers &&
           m_freeCommandBuffers && m_beginCommandBuffer && m_endCommandBuffer &&
           m_queueSubmit && m_queueWaitIdle && m_createBuffer && m_destroyBuffer &&
           m_getBufferMemoryRequirements && m_allocateMemory && m_freeMemory &&
           m_bindBufferMemory && m_mapMemory && m_unmapMemory && m_flushMappedMemoryRanges && m_cmdCopyBuffer &&
           m_createImage && m_destroyImage && m_getImageMemoryRequirements &&
           m_bindImageMemory && m_createImageView && m_destroyImageView &&
           m_cmdPipelineBarrier && m_cmdCopyBufferToImage;
}

bool VulkanResourceManager::Initialize(VulkanContext& context)
{
    Shutdown();
    if (!context.IsInitialized())
    {
        SetError("Vulkan context is not initialized");
        return false;
    }
    m_context = &context;
    if (!LoadFunctions())
    {
        Shutdown();
        return false;
    }
    m_getPhysicalDeviceMemoryProperties(context.GetPhysicalDevice(), &m_memoryProperties);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = context.GetGraphicsQueueFamily();
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (m_createCommandPool(context.GetDevice(), &poolInfo, nullptr, &m_commandPool) != VK_SUCCESS)
    {
        SetError("vkCreateCommandPool(resource manager) failed");
        Shutdown();
        return false;
    }
    return true;
}

bool VulkanResourceManager::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags properties,
                                           uint32_t& typeIndex) const
{
    for (uint32_t i = 0; i < m_memoryProperties.memoryTypeCount; ++i)
    {
        if ((typeBits & (1u << i)) &&
            (m_memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
        {
            typeIndex = i;
            return true;
        }
    }
    return false;
}

bool VulkanResourceManager::AllocateMemory(const VkMemoryRequirements& requirements,
                                           VkMemoryPropertyFlags properties, VkDeviceMemory& memory) const
{
    uint32_t typeIndex = 0;
    if (!FindMemoryType(requirements.memoryTypeBits, properties, typeIndex))
        return false;
    VkMemoryAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = typeIndex;
    return m_allocateMemory(m_context->GetDevice(), &allocateInfo, nullptr, &memory) == VK_SUCCESS;
}

bool VulkanResourceManager::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                         VkMemoryPropertyFlags memoryProperties, VulkanBuffer& buffer)
{
    DestroyBuffer(buffer);
    if (!m_context || size == 0)
        return false;
    VkBufferCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    createInfo.size = size;
    createInfo.usage = usage;
    createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (m_createBuffer(m_context->GetDevice(), &createInfo, nullptr, &buffer.buffer) != VK_SUCCESS)
    {
        SetError("vkCreateBuffer failed");
        return false;
    }
    VkMemoryRequirements requirements{};
    m_getBufferMemoryRequirements(m_context->GetDevice(), buffer.buffer, &requirements);
    if (!AllocateMemory(requirements, memoryProperties, buffer.memory) ||
        m_bindBufferMemory(m_context->GetDevice(), buffer.buffer, buffer.memory, 0) != VK_SUCCESS)
    {
        SetError("Vulkan buffer memory allocation failed");
        DestroyBuffer(buffer);
        return false;
    }
    buffer.size = size;
    buffer.memoryProperties = memoryProperties;
    return true;
}

bool VulkanResourceManager::SubmitImmediate(const std::function<void(VkCommandBuffer)>& record)
{
    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = m_commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (m_allocateCommandBuffers(m_context->GetDevice(), &allocateInfo, &commandBuffer) != VK_SUCCESS)
        return false;
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    bool result = m_beginCommandBuffer(commandBuffer, &beginInfo) == VK_SUCCESS;
    if (result)
    {
        record(commandBuffer);
        result = m_endCommandBuffer(commandBuffer) == VK_SUCCESS;
    }
    if (result)
    {
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer;
        result = m_queueSubmit(m_context->GetGraphicsQueue(), 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
    }
    if (result)
        result = m_queueWaitIdle(m_context->GetGraphicsQueue()) == VK_SUCCESS;
    m_freeCommandBuffers(m_context->GetDevice(), m_commandPool, 1, &commandBuffer);
    return result;
}

bool VulkanResourceManager::CreateBufferWithData(const void* data, VkDeviceSize size,
                                                 VkBufferUsageFlags usage, VulkanBuffer& buffer)
{
    if (!data || size == 0 || !CreateBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buffer))
        return false;
    VulkanBuffer staging;
    if (!CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging))
    {
        DestroyBuffer(buffer);
        return false;
    }
    void* mapped = nullptr;
    bool result = m_mapMemory(m_context->GetDevice(), staging.memory, 0, size, 0, &mapped) == VK_SUCCESS;
    if (result)
    {
        std::memcpy(mapped, data, static_cast<size_t>(size));
        m_unmapMemory(m_context->GetDevice(), staging.memory);
        result = SubmitImmediate([&](VkCommandBuffer commandBuffer)
        {
            VkBufferCopy copy{};
            copy.size = size;
            m_cmdCopyBuffer(commandBuffer, staging.buffer, buffer.buffer, 1, &copy);
        });
    }
    DestroyBuffer(staging);
    if (!result)
        DestroyBuffer(buffer);
    return result;
}

bool VulkanResourceManager::UploadBuffer(VulkanBuffer& buffer, const void* data,
                                         VkDeviceSize size, VkDeviceSize offset)
{
    if (!data || !buffer.buffer || !(buffer.memoryProperties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
        offset > buffer.size || size > buffer.size - offset)
        return false;
    void* mapped = nullptr;
    // Geometry uploads commonly have arbitrary byte offsets. vkMapMemory's
    // offset must satisfy minMemoryMapAlignment, so map the allocation from 0
    // and apply the buffer offset to the CPU pointer instead.
    if (m_mapMemory(m_context->GetDevice(), buffer.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
        return false;
    std::memcpy(static_cast<uint8_t*>(mapped) + offset, data, static_cast<size_t>(size));
    if (!(buffer.memoryProperties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
    {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = buffer.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        const VkResult flushResult = m_flushMappedMemoryRanges(m_context->GetDevice(), 1, &range);
        m_unmapMemory(m_context->GetDevice(), buffer.memory);
        return flushResult == VK_SUCCESS;
    }
    m_unmapMemory(m_context->GetDevice(), buffer.memory);
    return true;
}

void VulkanResourceManager::DestroyBuffer(VulkanBuffer& buffer)
{
    if (m_context && buffer.buffer && m_destroyBuffer)
        m_destroyBuffer(m_context->GetDevice(), buffer.buffer, nullptr);
    if (m_context && buffer.memory && m_freeMemory)
        m_freeMemory(m_context->GetDevice(), buffer.memory, nullptr);
    buffer = VulkanBuffer{};
}

bool VulkanResourceManager::CreateTextureRGBA8(const void* rgbaData, uint32_t width, uint32_t height,
                                               VulkanTexture& texture, VkFormat format,
                                               bool retainUploadBuffer, bool generateMipmaps)
{
    DestroyTexture(texture);
    if (!rgbaData || width == 0 || height == 0 || format == VK_FORMAT_UNDEFINED)
        return false;
    std::vector<uint8_t> mipPixels;
    std::vector<VkBufferImageCopy> copyRegions;
    if (!BuildRgbaMipChain(rgbaData, width, height, mipPixels, copyRegions))
        return false;
    const uint32_t mipLevels = (retainUploadBuffer || !generateMipmaps) ?
        1u : static_cast<uint32_t>(copyRegions.size());
    if (retainUploadBuffer || !generateMipmaps)
    {
        mipPixels.resize(static_cast<size_t>(width) * height * 4);
        copyRegions.resize(1);
    }
    const VkDeviceSize dataSize = mipPixels.size();
    VulkanBuffer staging;
    if (!CreateBuffer(dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging))
        return false;
    if (!UploadBuffer(staging, mipPixels.data(), dataSize))
    {
        DestroyBuffer(staging);
        return false;
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = { width, height, 1 };
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (m_createImage(m_context->GetDevice(), &imageInfo, nullptr, &texture.image) != VK_SUCCESS)
    {
        DestroyBuffer(staging);
        SetError("vkCreateImage failed");
        return false;
    }
    VkMemoryRequirements requirements{};
    m_getImageMemoryRequirements(m_context->GetDevice(), texture.image, &requirements);
    if (!AllocateMemory(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, texture.memory) ||
        m_bindImageMemory(m_context->GetDevice(), texture.image, texture.memory, 0) != VK_SUCCESS)
    {
        DestroyBuffer(staging);
        DestroyTexture(texture);
        SetError("Vulkan image memory allocation failed");
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = texture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.layerCount = 1;
    if (m_createImageView(m_context->GetDevice(), &viewInfo, nullptr, &texture.view) != VK_SUCCESS)
    {
        DestroyBuffer(staging);
        DestroyTexture(texture);
        SetError("vkCreateImageView(texture) failed");
        return false;
    }

    const bool uploaded = SubmitImmediate([&](VkCommandBuffer commandBuffer)
    {
        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = texture.image;
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.levelCount = mipLevels;
        toTransfer.subresourceRange.layerCount = 1;
        m_cmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

        m_cmdCopyBufferToImage(commandBuffer, staging.buffer, texture.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(copyRegions.size()), copyRegions.data());

        VkImageMemoryBarrier toShader{};
        toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toShader.image = texture.image;
        toShader.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toShader.subresourceRange.levelCount = mipLevels;
        toShader.subresourceRange.layerCount = 1;
        m_cmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toShader);
    });
    if (!uploaded)
    {
        DestroyBuffer(staging);
        DestroyTexture(texture);
        SetError("Vulkan texture upload failed");
        return false;
    }
    if (retainUploadBuffer)
    {
        texture.uploadBuffer = staging;
        staging = VulkanBuffer{};
    }
    else
        DestroyBuffer(staging);
    texture.width = width;
    texture.height = height;
    texture.mipLevels = mipLevels;
    texture.format = format;
    return true;
}

bool VulkanResourceManager::UpdateTextureRGBA8(const void* rgbaData, uint32_t width, uint32_t height,
                                               VulkanTexture& texture)
{
    if (!rgbaData || !texture.image || texture.width != width || texture.height != height ||
        texture.format == VK_FORMAT_UNDEFINED)
        return false;
    std::vector<uint8_t> mipPixels;
    std::vector<VkBufferImageCopy> copyRegions;
    if (!BuildRgbaMipChain(rgbaData, width, height, mipPixels, copyRegions) ||
        copyRegions.size() < texture.mipLevels)
        return false;
    if (texture.mipLevels == 1)
    {
        mipPixels.resize(static_cast<size_t>(width) * height * 4);
        copyRegions.resize(1);
    }
    const VkDeviceSize dataSize = mipPixels.size();
    VulkanBuffer temporaryStaging;
    VulkanBuffer* uploadBuffer = &texture.uploadBuffer;
    if (!uploadBuffer->buffer)
    {
        if (!CreateBuffer(dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          temporaryStaging))
            return false;
        uploadBuffer = &temporaryStaging;
    }
    if (uploadBuffer->size < dataSize || !UploadBuffer(*uploadBuffer, mipPixels.data(), dataSize))
    {
        DestroyBuffer(temporaryStaging);
        return false;
    }
    const bool uploaded = SubmitImmediate([&](VkCommandBuffer commandBuffer)
    {
        VkImageMemoryBarrier toTransfer{};
        toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = texture.image;
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.levelCount = texture.mipLevels;
        toTransfer.subresourceRange.layerCount = 1;
        m_cmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

        m_cmdCopyBufferToImage(commandBuffer, uploadBuffer->buffer, texture.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(copyRegions.size()), copyRegions.data());

        VkImageMemoryBarrier toShader{};
        toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toShader.image = texture.image;
        toShader.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toShader.subresourceRange.levelCount = texture.mipLevels;
        toShader.subresourceRange.layerCount = 1;
        m_cmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toShader);
    });
    DestroyBuffer(temporaryStaging);
    if (!uploaded)
        SetError("Vulkan frame texture update failed");
    return uploaded;
}

bool VulkanResourceManager::PrepareTextureRGBA8Update(const void* rgbaData, uint32_t width, uint32_t height,
                                                       VulkanTexture& texture)
{
    if (!rgbaData || !texture.image || texture.width != width || texture.height != height ||
        texture.format == VK_FORMAT_UNDEFINED)
        return false;
    std::vector<uint8_t> mipPixels;
    std::vector<VkBufferImageCopy> regions;
    if (!BuildRgbaMipChain(rgbaData, width, height, mipPixels, regions) ||
        regions.size() < texture.mipLevels)
        return false;
    if (texture.mipLevels == 1)
        mipPixels.resize(static_cast<size_t>(width) * height * 4);
    const VkDeviceSize dataSize = mipPixels.size();
    if (!texture.uploadBuffer.buffer &&
        !CreateBuffer(dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      texture.uploadBuffer))
        return false;
    return texture.uploadBuffer.size >= dataSize &&
           UploadBuffer(texture.uploadBuffer, mipPixels.data(), dataSize);
}

bool VulkanResourceManager::RecordTextureRGBA8Update(VkCommandBuffer commandBuffer,
                                                       const VulkanTexture& texture)
{
    if (!commandBuffer || !texture.image || !texture.uploadBuffer.buffer ||
        texture.format == VK_FORMAT_UNDEFINED || texture.width == 0 || texture.height == 0)
        return false;
    std::vector<VkBufferImageCopy> copyRegions;
    copyRegions.reserve(texture.mipLevels);
    VkDeviceSize offset = 0;
    uint32_t mipWidth = texture.width, mipHeight = texture.height;
    for (uint32_t level = 0; level < texture.mipLevels; ++level)
    {
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = level;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = { mipWidth, mipHeight, 1 };
        copyRegions.push_back(region);
        offset += static_cast<VkDeviceSize>(mipWidth) * mipHeight * 4;
        mipWidth = mipWidth > 1 ? mipWidth / 2 : 1;
        mipHeight = mipHeight > 1 ? mipHeight / 2 : 1;
    }
    if (texture.uploadBuffer.size < offset)
        return false;
    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = texture.image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.levelCount = texture.mipLevels;
    toTransfer.subresourceRange.layerCount = 1;
    m_cmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    m_cmdCopyBufferToImage(commandBuffer, texture.uploadBuffer.buffer, texture.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(copyRegions.size()), copyRegions.data());

    VkImageMemoryBarrier toShader{};
    toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShader.image = texture.image;
    toShader.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toShader.subresourceRange.levelCount = texture.mipLevels;
    toShader.subresourceRange.layerCount = 1;
    m_cmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toShader);
    return true;
}

bool VulkanResourceManager::CreateDepthImage(uint32_t width, uint32_t height, VkFormat format,
                                             VulkanTexture& texture)
{
    DestroyTexture(texture);
    if (!m_context || width == 0 || height == 0 || format == VK_FORMAT_UNDEFINED)
        return false;
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = { width, height, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (m_createImage(m_context->GetDevice(), &imageInfo, nullptr, &texture.image) != VK_SUCCESS)
    {
        SetError("vkCreateImage(depth) failed");
        return false;
    }
    VkMemoryRequirements requirements{};
    m_getImageMemoryRequirements(m_context->GetDevice(), texture.image, &requirements);
    if (!AllocateMemory(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, texture.memory) ||
        m_bindImageMemory(m_context->GetDevice(), texture.image, texture.memory, 0) != VK_SUCCESS)
    {
        DestroyTexture(texture);
        SetError("Vulkan depth image memory allocation failed");
        return false;
    }
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = texture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT)
        viewInfo.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (m_createImageView(m_context->GetDevice(), &viewInfo, nullptr, &texture.view) != VK_SUCCESS)
    {
        DestroyTexture(texture);
        SetError("vkCreateImageView(depth) failed");
        return false;
    }
    texture.width = width;
    texture.height = height;
    texture.format = format;
    return true;
}

void VulkanResourceManager::DestroyTexture(VulkanTexture& texture)
{
    if (m_context && texture.view && m_destroyImageView)
        m_destroyImageView(m_context->GetDevice(), texture.view, nullptr);
    if (m_context && texture.image && m_destroyImage)
        m_destroyImage(m_context->GetDevice(), texture.image, nullptr);
    if (m_context && texture.memory && m_freeMemory)
        m_freeMemory(m_context->GetDevice(), texture.memory, nullptr);
    DestroyBuffer(texture.uploadBuffer);
    texture = VulkanTexture{};
}

void VulkanResourceManager::Shutdown()
{
    if (m_context && m_commandPool && m_queueWaitIdle)
        m_queueWaitIdle(m_context->GetGraphicsQueue());
    if (m_context && m_commandPool && m_destroyCommandPool)
        m_destroyCommandPool(m_context->GetDevice(), m_commandPool, nullptr);
    m_commandPool = VK_NULL_HANDLE;
    m_context = nullptr;
}
}
