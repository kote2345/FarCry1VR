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
                       std::vector<uint8_t>& pixels, std::vector<VkBufferImageCopy>& regions,
                       uint32_t requestedLevels = 0)
{
    if (!sourcePixels || !width || !height)
        return false;
    pixels.clear();
    regions.clear();
    const uint32_t fullLevels = CountMipLevels(width, height);
    const uint32_t levels = requestedLevels && requestedLevels < fullLevels ? requestedLevels : fullLevels;
    size_t totalBytes = 0;
    uint32_t reserveWidth = width, reserveHeight = height;
    for (uint32_t level = 0; level < levels; ++level)
    {
        const size_t bytes = static_cast<size_t>(reserveWidth) * reserveHeight * 4;
        if (bytes > pixels.max_size() - totalBytes) return false;
        totalBytes += bytes;
        reserveWidth = reserveWidth > 1 ? reserveWidth / 2 : 1;
        reserveHeight = reserveHeight > 1 ? reserveHeight / 2 : 1;
    }
    pixels.reserve(totalBytes);
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
            {
                const uint32_t firstY = y * previousHeight / levelHeight;
                const uint32_t lastY = (y + 1) * previousHeight / levelHeight;
                for (uint32_t x = 0; x < levelWidth; ++x)
                {
                    const uint32_t firstX = x * previousWidth / levelWidth;
                    const uint32_t lastX = (x + 1) * previousWidth / levelWidth;
                    const uint32_t samples = (lastX - firstX) * (lastY - firstY);
                    uint32_t sums[4]{};
                    for (uint32_t py = firstY; py < lastY; ++py)
                    for (uint32_t px = firstX; px < lastX; ++px)
                    {
                        const uint8_t* sample = previous + (static_cast<size_t>(py) * previousWidth + px) * 4;
                        for (uint32_t channel = 0; channel < 4; ++channel) sums[channel] += sample[channel];
                    }
                    uint8_t* output = destination + (static_cast<size_t>(y) * levelWidth + x) * 4;
                    for (uint32_t channel = 0; channel < 4; ++channel)
                        output[channel] = static_cast<uint8_t>((sums[channel] + samples / 2) / samples);
                }
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
    LOAD_VK(createFence, CreateFence);
    LOAD_VK(destroyFence, DestroyFence);
    LOAD_VK(getFenceStatus, GetFenceStatus);
    LOAD_VK(waitForFences, WaitForFences);
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

void VulkanResourceManager::CollectUploads(VkImage waitImage, bool waitAll)
{
    for (auto it = m_pendingUploads.begin(); it != m_pendingUploads.end(); )
    {
        const bool wait = waitAll || (waitImage && it->image == waitImage);
        VkResult result = wait ? m_waitForFences(m_context->GetDevice(), 1, &it->fence,
            VK_TRUE, UINT64_MAX) : m_getFenceStatus(m_context->GetDevice(), it->fence);
        if (result == VK_NOT_READY) { ++it; continue; }
        if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST)
        {
            SetError("checking upload completion failed");
            ++it;
            continue;
        }
        m_freeCommandBuffers(m_context->GetDevice(), m_commandPool, 1, &it->command);
        m_destroyFence(m_context->GetDevice(), it->fence, nullptr);
        m_pendingUploadBytes -= it->staging.size;
        if (result == VK_SUCCESS) RecycleUploadBuffer(it->staging);
        else DestroyBuffer(it->staging);
        it = m_pendingUploads.erase(it);
    }
}

bool VulkanResourceManager::AcquireUploadBuffer(VkDeviceSize size, VulkanBuffer& buffer)
{
    CollectUploads();
    size_t best = m_uploadPool.size();
    for (size_t i = 0; i < m_uploadPool.size(); ++i)
        if (m_uploadPool[i].size >= size &&
            (best == m_uploadPool.size() || m_uploadPool[i].size < m_uploadPool[best].size)) best = i;
    if (best != m_uploadPool.size())
    {
        buffer = m_uploadPool[best];
        m_uploadPoolBytes -= buffer.size;
        m_uploadPool.erase(m_uploadPool.begin() + best);
        return true;
    }
    return CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffer);
}

void VulkanResourceManager::RecycleUploadBuffer(VulkanBuffer& buffer)
{
    // Only completed transfer allocations enter this pool. Keeping mapped
    // memory avoids allocation/map/free churn during sector and texture loads.
    if (buffer.size <= 16u*1024u*1024u - m_uploadPoolBytes && m_uploadPool.size() < 32)
    {
        m_uploadPoolBytes += buffer.size;
        m_uploadPool.push_back(buffer);
        buffer = VulkanBuffer{};
    }
    else DestroyBuffer(buffer);
}

bool VulkanResourceManager::SubmitImmediate(const std::function<void(VkCommandBuffer)>& record,
                                           VulkanBuffer* upload, VkImage image)
{
    CollectUploads();
    while (!m_pendingUploads.empty() && (m_pendingUploads.size() >= 64 ||
        (upload && m_pendingUploadBytes + upload->size > 32u*1024u*1024u)))
    {
        // Buffer transfers have no image handle. Waiting by image previously
        // failed to enforce the queue limit for those transfers.
        if (m_waitForFences(m_context->GetDevice(), 1, &m_pendingUploads.front().fence,
                VK_TRUE, UINT64_MAX) != VK_SUCCESS)
            return false;
        CollectUploads();
    }
    VkFence fence = VK_NULL_HANDLE;
    const bool deferred = upload && m_createFence && m_destroyFence &&
        m_getFenceStatus && m_waitForFences;
    if (deferred)
    {
        VkFenceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (m_createFence(m_context->GetDevice(), &info, nullptr, &fence) != VK_SUCCESS)
            return false;
    }
    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = m_commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (m_allocateCommandBuffers(m_context->GetDevice(), &allocateInfo, &commandBuffer) != VK_SUCCESS)
    {
        if (fence) m_destroyFence(m_context->GetDevice(), fence, nullptr);
        return false;
    }
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
        result = m_queueSubmit(m_context->GetGraphicsQueue(), 1, &submit, fence) == VK_SUCCESS;
    }
    if (result && deferred)
    {
        m_pendingUploads.push_back(PendingUpload{commandBuffer, fence, *upload, image});
        m_pendingUploadBytes += upload->size;
        *upload = VulkanBuffer{};
        return true;
    }
    if (result)
        result = m_queueWaitIdle(m_context->GetGraphicsQueue()) == VK_SUCCESS;
    m_freeCommandBuffers(m_context->GetDevice(), m_commandPool, 1, &commandBuffer);
    if (fence) m_destroyFence(m_context->GetDevice(), fence, nullptr);
    return result;
}

bool VulkanResourceManager::CreateBufferWithData(const void* data, VkDeviceSize size,
                                                 VkBufferUsageFlags usage, VulkanBuffer& buffer)
{
    if (!data || size == 0)
        return false;
    // Quest's unified memory can be both GPU-local and CPU-coherent. Populate
    // new immutable geometry directly instead of allocating a staging buffer
    // and draining the graphics queue for every streamed mesh.
    if (CreateBuffer(size, usage,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffer))
    {
        if (UploadBuffer(buffer, data, size))
            return true;
        DestroyBuffer(buffer);
    }
    if (!data || size == 0 || !CreateBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, buffer))
        return false;
    VulkanBuffer staging;
    if (!AcquireUploadBuffer(size, staging))
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
                                         VkDeviceSize size, VkDeviceSize offset, bool appendOnly)
{
    if (!data || !buffer.buffer || !(buffer.memoryProperties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
        offset > buffer.size || size > buffer.size - offset)
        return false;
    // Geometry uploads commonly have arbitrary byte offsets. vkMapMemory's
    // Append-only coherent arenas never modify bytes referenced by an older
    // submission. Ordinary uploads retain the conservative buffer fence guard.
    if (appendOnly && (offset < buffer.uploadedEnd ||
        !(buffer.memoryProperties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) return false;
    if (!appendOnly && m_bufferAccessGuard && !m_bufferAccessGuard(buffer.buffer)) return false;
    // offset must satisfy minMemoryMapAlignment, so map the allocation from 0
    // and apply the buffer offset to the CPU pointer instead. Keep that mapping
    // until destruction: uniform and dynamic geometry writes happen thousands
    // of times per frame and do not need repeated driver map/unmap calls.
    if (!buffer.mappedData && m_mapMemory(m_context->GetDevice(), buffer.memory,
            0, VK_WHOLE_SIZE, 0, &buffer.mappedData) != VK_SUCCESS)
        return false;
    std::memcpy(static_cast<uint8_t*>(buffer.mappedData) + offset, data, static_cast<size_t>(size));
    buffer.uploadedEnd = std::max(buffer.uploadedEnd, offset + size);
    if (!(buffer.memoryProperties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
    {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = buffer.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        const VkResult flushResult = m_flushMappedMemoryRanges(m_context->GetDevice(), 1, &range);
        return flushResult == VK_SUCCESS;
    }
    return true;
}

void VulkanResourceManager::DestroyBuffer(VulkanBuffer& buffer)
{
    if (buffer.buffer && m_bufferAccessGuard && !m_bufferAccessGuard(buffer.buffer)) return;
    if (m_context && buffer.memory && buffer.mappedData && m_unmapMemory)
        m_unmapMemory(m_context->GetDevice(), buffer.memory);
    if (m_context && buffer.buffer && m_destroyBuffer)
        m_destroyBuffer(m_context->GetDevice(), buffer.buffer, nullptr);
    if (m_context && buffer.memory && m_freeMemory)
        m_freeMemory(m_context->GetDevice(), buffer.memory, nullptr);
    buffer = VulkanBuffer{};
}

bool VulkanResourceManager::CreateTextureRGBA8(const void* rgbaData, uint32_t width, uint32_t height,
                                               VulkanTexture& texture, VkFormat format,
                                               bool retainUploadBuffer, bool generateMipmaps,
                                               const void* const* rgbaMipLevels,
                                               uint32_t suppliedMipCount)
{
    DestroyTexture(texture);
    if (!rgbaData || width == 0 || height == 0 || format == VK_FORMAT_UNDEFINED)
        return false;
    std::vector<uint8_t> mipPixels;
    std::vector<VkBufferImageCopy> copyRegions;
    if (rgbaMipLevels && suppliedMipCount > 1)
    {
        const uint32_t maxLevels = CountMipLevels(width, height);
        if (suppliedMipCount > maxLevels)
            return false;
        uint32_t levelWidth = width;
        uint32_t levelHeight = height;
        VkDeviceSize bufferOffset = 0;
        const uint32_t uploadLevels = retainUploadBuffer || !generateMipmaps ? 1u : suppliedMipCount;
        size_t totalBytes = 0;
        for (uint32_t level = 0, w = width, h = height; level < uploadLevels; ++level)
        {
            const size_t bytes = static_cast<size_t>(w) * h * 4;
            if (bytes > mipPixels.max_size() - totalBytes) return false;
            totalBytes += bytes;
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        mipPixels.reserve(totalBytes);
        copyRegions.reserve(uploadLevels);
        for (uint32_t level = 0; level < uploadLevels; ++level)
        {
            if (!rgbaMipLevels[level])
                return false;
            const size_t levelBytes = static_cast<size_t>(levelWidth) * levelHeight * 4;
            if (levelBytes > mipPixels.max_size() - mipPixels.size())
                return false;
            const uint8_t* source = static_cast<const uint8_t*>(rgbaMipLevels[level]);
            mipPixels.insert(mipPixels.end(), source, source + levelBytes);
            VkBufferImageCopy region{};
            region.bufferOffset = bufferOffset;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = level;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = { levelWidth, levelHeight, 1 };
            copyRegions.push_back(region);
            bufferOffset += levelBytes;
            levelWidth = levelWidth > 1 ? levelWidth / 2 : 1;
            levelHeight = levelHeight > 1 ? levelHeight / 2 : 1;
        }
    }
    else if (!BuildRgbaMipChain(rgbaData, width, height, mipPixels, copyRegions,
                              retainUploadBuffer || !generateMipmaps ? 1u : 0u))
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
    if (!AcquireUploadBuffer(dataSize, staging))
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
    }, retainUploadBuffer ? nullptr : &staging, texture.image);
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
    if (!rgbaData || !texture.image || !texture.mipLevels || texture.width != width || texture.height != height ||
        texture.format == VK_FORMAT_UNDEFINED)
        return false;
    std::vector<uint8_t> mipPixels;
    std::vector<VkBufferImageCopy> copyRegions;
    if (!BuildRgbaMipChain(rgbaData, width, height, mipPixels, copyRegions, texture.mipLevels) ||
        copyRegions.size() < texture.mipLevels)
        return false;
    // DDS textures can supply a partial mip chain. Never copy generated
    // levels beyond the number allocated in the destination VkImage.
    copyRegions.resize(texture.mipLevels);
    const VkBufferImageCopy& lastMip = copyRegions.back();
    mipPixels.resize(static_cast<size_t>(lastMip.bufferOffset) +
                     static_cast<size_t>(lastMip.imageExtent.width) *
                         lastMip.imageExtent.height * 4);
    const VkDeviceSize dataSize = mipPixels.size();
    VulkanBuffer temporaryStaging;
    // Each queued update owns its source bytes until its fence signals.
    // Reusing texture.uploadBuffer would require waiting for the GPU before
    // overwriting it and previously forced queueWaitIdle for every update.
    if (!AcquireUploadBuffer(dataSize, temporaryStaging))
        return false;
    VulkanBuffer* uploadBuffer = &temporaryStaging;
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
    }, &temporaryStaging, texture.image);
    DestroyBuffer(temporaryStaging);
    if (!uploaded)
        SetError("Vulkan frame texture update failed");
    return uploaded;
}

bool VulkanResourceManager::PrepareTextureRGBA8Update(const void* rgbaData, uint32_t width, uint32_t height,
                                                       VulkanTexture& texture)
{
    if (!rgbaData || !texture.image || !texture.mipLevels || texture.width != width || texture.height != height ||
        texture.format == VK_FORMAT_UNDEFINED)
        return false;
    std::vector<uint8_t> mipPixels;
    std::vector<VkBufferImageCopy> regions;
    if (!BuildRgbaMipChain(rgbaData, width, height, mipPixels, regions, texture.mipLevels) ||
        regions.size() < texture.mipLevels)
        return false;
    regions.resize(texture.mipLevels);
    const VkBufferImageCopy& lastMip = regions.back();
    mipPixels.resize(static_cast<size_t>(lastMip.bufferOffset) +
                     static_cast<size_t>(lastMip.imageExtent.width) *
                         lastMip.imageExtent.height * 4);
    const VkDeviceSize dataSize = mipPixels.size();
    if (!texture.uploadBuffer.buffer &&
        !AcquireUploadBuffer(dataSize, texture.uploadBuffer))
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
                                             VulkanTexture& texture, uint32_t layers)
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
    imageInfo.arrayLayers = layers;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    // OpenGL's shadow-map pass samples its depth texture in the receiver
    // shader. Keep Vulkan depth targets sampleable as well as renderable.
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
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
    viewInfo.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT)
        viewInfo.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = layers;
    if (m_createImageView(m_context->GetDevice(), &viewInfo, nullptr, &texture.view) != VK_SUCCESS)
    {
        DestroyTexture(texture);
        SetError("vkCreateImageView(depth) failed");
        return false;
    }
    if (viewInfo.subresourceRange.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT)
    {
        // A framebuffer attachment may use a combined depth/stencil view, but
        // Vulkan depth sampling requires a depth-only image view.
        VkImageViewCreateInfo sampledViewInfo = viewInfo;
        sampledViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (m_createImageView(m_context->GetDevice(), &sampledViewInfo, nullptr,
                              &texture.sampledView) != VK_SUCCESS)
        {
            DestroyTexture(texture);
            SetError("vkCreateImageView(sampled depth) failed");
            return false;
        }
    }
    texture.width = width;
    texture.height = height;
    texture.format = format;
    return true;
}

bool VulkanResourceManager::CreateColorTarget(uint32_t width, uint32_t height, VkFormat format,
                                              VulkanTexture& texture, uint32_t layers)
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
    imageInfo.arrayLayers = layers;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (m_createImage(m_context->GetDevice(), &imageInfo, nullptr, &texture.image) != VK_SUCCESS)
    {
        SetError("vkCreateImage(color target) failed");
        return false;
    }
    VkMemoryRequirements requirements{};
    m_getImageMemoryRequirements(m_context->GetDevice(), texture.image, &requirements);
    if (!AllocateMemory(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, texture.memory) ||
        m_bindImageMemory(m_context->GetDevice(), texture.image, texture.memory, 0) != VK_SUCCESS)
    {
        DestroyTexture(texture);
        SetError("Vulkan color target memory allocation failed");
        return false;
    }
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = texture.image;
    viewInfo.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = layers;
    if (m_createImageView(m_context->GetDevice(), &viewInfo, nullptr, &texture.view) != VK_SUCCESS)
    {
        DestroyTexture(texture);
        SetError("vkCreateImageView(color target) failed");
        return false;
    }
    texture.width = width;
    texture.height = height;
    texture.format = format;
    return true;
}

bool VulkanResourceManager::CreateCubeColorTarget(uint32_t size, VkFormat format,
                                                 VulkanCubeTarget& target)
{
    DestroyCubeColorTarget(target);
    if (!m_context || !size || format == VK_FORMAT_UNDEFINED)
        return false;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = { size, size, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 6;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VulkanTexture& texture = target.texture;
    if (m_createImage(m_context->GetDevice(), &imageInfo, nullptr, &texture.image) != VK_SUCCESS)
    {
        SetError("vkCreateImage(cube target) failed");
        return false;
    }
    VkMemoryRequirements requirements{};
    m_getImageMemoryRequirements(m_context->GetDevice(), texture.image, &requirements);
    if (!AllocateMemory(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, texture.memory) ||
        m_bindImageMemory(m_context->GetDevice(), texture.image, texture.memory, 0) != VK_SUCCESS)
    {
        DestroyCubeColorTarget(target);
        SetError("Vulkan cube target memory allocation failed");
        return false;
    }
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = texture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 6;
    if (m_createImageView(m_context->GetDevice(), &viewInfo, nullptr, &texture.view) != VK_SUCCESS)
    {
        DestroyCubeColorTarget(target);
        SetError("vkCreateImageView(cube target) failed");
        return false;
    }
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.subresourceRange.layerCount = 1;
    for (uint32_t face = 0; face < 6; ++face)
    {
        viewInfo.subresourceRange.baseArrayLayer = face;
        if (m_createImageView(m_context->GetDevice(), &viewInfo, nullptr,
                             &target.faceViews[face]) != VK_SUCCESS)
        {
            DestroyCubeColorTarget(target);
            SetError("vkCreateImageView(cube face) failed");
            return false;
        }
    }
    texture.width = size;
    texture.height = size;
    texture.format = format;
    return true;
}

void VulkanResourceManager::DestroyCubeColorTarget(VulkanCubeTarget& target)
{
    if (target.texture.image && m_imageDestroyGuard && !m_imageDestroyGuard()) return;
    for (VkImageView& face : target.faceViews)
    {
        if (m_context && face && m_destroyImageView)
            m_destroyImageView(m_context->GetDevice(), face, nullptr);
        face = VK_NULL_HANDLE;
    }
    DestroyTexture(target.texture);
}

void VulkanResourceManager::DestroyTexture(VulkanTexture& texture)
{
    if (texture.image && m_imageDestroyGuard && !m_imageDestroyGuard()) return;
    if (m_context && texture.image) CollectUploads(texture.image);
    if (m_context && texture.sampledView && m_destroyImageView)
        m_destroyImageView(m_context->GetDevice(), texture.sampledView, nullptr);
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
    if (m_context) CollectUploads(VK_NULL_HANDLE, true);
    for (auto& buffer : m_uploadPool) DestroyBuffer(buffer);
    m_uploadPool.clear();
    m_uploadPoolBytes = 0;
    m_pendingUploadBytes = 0;
    if (m_context && m_commandPool && m_destroyCommandPool)
        m_destroyCommandPool(m_context->GetDevice(), m_commandPool, nullptr);
    m_commandPool = VK_NULL_HANDLE;
    m_context = nullptr;
}
}
