// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#include "TextureUploader.h"

#include <algorithm>
#include <cmath>

#include "Const.h"
#include "Utils.h"

using namespace qray;

namespace
{
    VkImageSubresourceRange MakeSubresourceRange(uint32_t baseMipLevel, uint32_t levelCount, uint32_t baseArrayLayer, uint32_t layerCount)
    {
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = baseMipLevel;
        range.levelCount = levelCount;
        range.baseArrayLayer = baseArrayLayer;
        range.layerCount = layerCount;
        return range;
    }

    VkComponentMapping MakeComponentMapping(QrTextureSwizzling swizzling)
    {
        switch (swizzling)
        {
            case QR_TEXTURE_SWIZZLING_ROUGHNESS_METALLIC_EMISSIVE:
                return { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A };

            case QR_TEXTURE_SWIZZLING_ROUGHNESS_METALLIC:
                return { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_A };

            case QR_TEXTURE_SWIZZLING_METALLIC_ROUGHNESS_EMISSIVE:
                return { VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A };

            case QR_TEXTURE_SWIZZLING_METALLIC_ROUGHNESS:
                return { VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_A };

            case QR_TEXTURE_SWIZZLING_NULL_ROUGHNESS_METALLIC:
                return { VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_A };

            default:
                assert(0);
                return { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A };
        }
    }
}

TextureUploader::TextureUploader(VkDevice _device, std::shared_ptr<MemoryAllocator> _memAllocator)
    : device(_device)
    , memAllocator(std::move(_memAllocator))
{
}

TextureUploader::~TextureUploader()
{
    for (auto &frameStaging : stagingToFree)
    {
        for (VkBuffer stagingBuffer : frameStaging)
        {
            memAllocator->DestroyStagingSrcTextureBuffer(stagingBuffer);
        }
    }

    for (const auto &entry : updateableImageInfos)
    {
        for (VkBuffer stagingBuffer : entry.second.stagingBuffers)
        {
            if (stagingBuffer != VK_NULL_HANDLE)
            {
                memAllocator->DestroyStagingSrcTextureBuffer(stagingBuffer);
            }
        }
    }
}

void TextureUploader::ClearStaging(uint32_t frameIndex)
{
    auto &frameStaging = stagingToFree[frameIndex];

    for (VkBuffer stagingBuffer : frameStaging)
    {
        memAllocator->DestroyStagingSrcTextureBuffer(stagingBuffer);
    }

    frameStaging.clear();
}

bool TextureUploader::DoesFormatSupportBlit(VkFormat format) const
{
    return format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_R8G8B8A8_UNORM;
}

bool TextureUploader::AreMipmapsPregenerated(const UploadInfo &info) const
{
    return info.pregeneratedLevelCount > 0;
}

uint32_t TextureUploader::GetMipmapCount(const QrExtent2D &size, const UploadInfo &info) const
{
    if (!info.useMipmaps)
    {
        return 1;
    }

    if (AreMipmapsPregenerated(info))
    {
        return std::min(info.pregeneratedLevelCount, MAX_PREGENERATED_MIPMAP_LEVELS);
    }

    if (!DoesFormatSupportBlit(info.format))
    {
        return 1;
    }

    const auto widthLevelCount = static_cast<uint32_t>(log2(size.width));
    const auto heightLevelCount = static_cast<uint32_t>(log2(size.height));

    return std::min(widthLevelCount, heightLevelCount) + 1;
}

void TextureUploader::PrepareMipmaps(VkCommandBuffer cmd, VkImage image, uint32_t baseWidth, uint32_t baseHeight, uint32_t mipmapCount, uint32_t layerCount)
{
    if (mipmapCount <= 1)
    {
        return;
    }

    uint32_t mipWidth = baseWidth;
    uint32_t mipHeight = baseHeight;

    for (uint32_t mipLevel = 1; mipLevel < mipmapCount; mipLevel++)
    {
        const uint32_t srcWidth = mipWidth;
        const uint32_t srcHeight = mipHeight;

        mipWidth = std::max(1u, mipWidth >> 1);
        mipHeight = std::max(1u, mipHeight >> 1);

        assert(mipWidth > 0 && mipHeight > 0);
        assert(mipLevel != mipmapCount - 1 || (mipWidth == 1 || mipHeight == 1));

        const VkImageSubresourceRange mipRange = MakeSubresourceRange(mipLevel, 1, 0, layerCount);

        Utils::BarrierImage(
            cmd, image,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            mipRange);

        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = mipLevel - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = layerCount;
        blit.srcOffsets[0] = { 0, 0, 0 };
        blit.srcOffsets[1] = { static_cast<int32_t>(srcWidth), static_cast<int32_t>(srcHeight), 1 };
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = mipLevel;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = layerCount;
        blit.dstOffsets[0] = { 0, 0, 0 };
        blit.dstOffsets[1] = { static_cast<int32_t>(mipWidth), static_cast<int32_t>(mipHeight), 1 };

        vkCmdBlitImage(
            cmd,
            image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);

        Utils::BarrierImage(
            cmd, image,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            mipRange);
    }
}

void TextureUploader::CopyStagingToImage(VkCommandBuffer cmd, VkBuffer staging, VkImage image, const QrExtent2D &size, uint32_t baseLayer, uint32_t layerCount,
                                           VkDeviceSize bufferOffset)
{
    VkBufferImageCopy copyRegion{};
    copyRegion.bufferOffset = bufferOffset;
    copyRegion.bufferRowLength = 0;
    copyRegion.bufferImageHeight = 0;
    copyRegion.imageExtent = { size.width, size.height, 1 };
    copyRegion.imageOffset = { 0, 0, 0 };
    copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.mipLevel = 0;
    copyRegion.imageSubresource.baseArrayLayer = baseLayer;
    copyRegion.imageSubresource.layerCount = layerCount;

    vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
}

void TextureUploader::CopyStagingToImageMipmaps(VkCommandBuffer cmd, VkBuffer staging, VkImage image, uint32_t layerIndex, const UploadInfo &info)
{
    uint32_t mipWidth = info.baseSize.width;
    uint32_t mipHeight = info.baseSize.height;

    const uint32_t levelCount = GetMipmapCount(info.baseSize, info);

    VkBufferImageCopy copyRegions[MAX_PREGENERATED_MIPMAP_LEVELS] = {};

    for (uint32_t mipLevel = 0; mipLevel < levelCount; mipLevel++)
    {
        VkBufferImageCopy &region = copyRegions[mipLevel];

        region.bufferOffset = info.pLevelDataOffsets[mipLevel];
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageExtent = { mipWidth, mipHeight, 1 };
        region.imageOffset = { 0, 0, 0 };
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = mipLevel;
        region.imageSubresource.baseArrayLayer = layerIndex;
        region.imageSubresource.layerCount = 1;

        mipWidth = std::max(1u, mipWidth >> 1);
        mipHeight = std::max(1u, mipHeight >> 1);
    }

    vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levelCount, copyRegions);
}

bool TextureUploader::CreateImage(const UploadInfo &info, VkImage *result)
{
    const QrExtent2D &size = info.baseSize;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.flags = info.isCubemap ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    imageInfo.format = info.format;
    imageInfo.extent = { size.width, size.height, 1 };
    imageInfo.mipLevels = GetMipmapCount(size, info);
    imageInfo.arrayLayers = info.isCubemap ? 6u : 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    const VkImage image = memAllocator->CreateDstTextureImage(&imageInfo, info.pDebugName);

    if (image == VK_NULL_HANDLE)
    {
        return false;
    }

    SET_DEBUG_NAME(device, image, VK_OBJECT_TYPE_IMAGE, info.pDebugName);

    *result = image;
    return true;
}

void TextureUploader::PrepareImage(VkImage image, VkBuffer staging[], const UploadInfo &info, ImagePrepareType prepareType,
                                   const VkDeviceSize *stagingOffsets)
{
    VkCommandBuffer cmd = info.cmd;
    const QrExtent2D &size = info.baseSize;
    const uint32_t layerCount = info.isCubemap ? 6u : 1u;
    const uint32_t mipmapCount = GetMipmapCount(size, info);

    const VkImageSubresourceRange firstMipmap = MakeSubresourceRange(0, 1, 0, layerCount);
    const VkImageSubresourceRange allMipmaps = MakeSubresourceRange(0, mipmapCount, 0, layerCount);

    VkAccessFlags curAccessMask = 0;
    VkImageLayout curLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags curStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

    if (prepareType == ImagePrepareType::UPDATE)
    {
        curAccessMask = VK_ACCESS_SHADER_READ_BIT;
        curLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        curStageMask = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }

    if (prepareType != ImagePrepareType::INIT_WITHOUT_COPYING)
    {
        if (AreMipmapsPregenerated(info))
        {
            assert(layerCount == 1);

            Utils::BarrierImage(
                cmd, image,
                curAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT,
                curLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                curStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                allMipmaps);

            curAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            curLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            curStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;

            CopyStagingToImageMipmaps(cmd, staging[0], image, 0, info);
        }
        else
        {
            Utils::BarrierImage(
                cmd, image,
                curAccessMask, VK_ACCESS_TRANSFER_WRITE_BIT,
                curLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                curStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                firstMipmap);

            curAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            curLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            curStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;

            for (uint32_t layer = 0; layer < layerCount; layer++)
            {
                const VkDeviceSize offset = stagingOffsets != nullptr ? stagingOffsets[layer] : 0;
                CopyStagingToImage(cmd, staging[layer], image, size, layer, 1, offset);
            }
        }
    }

    if (mipmapCount > 1)
    {
        if (!AreMipmapsPregenerated(info) && DoesFormatSupportBlit(info.format))
        {
            Utils::BarrierImage(
                cmd, image,
                curAccessMask, VK_ACCESS_TRANSFER_READ_BIT,
                curLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                curStageMask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                firstMipmap);

            PrepareMipmaps(cmd, image, size.width, size.height, mipmapCount, layerCount);

            curLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }

        Utils::BarrierImage(
            cmd, image,
            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
            curLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            allMipmaps);
    }
    else
    {
        Utils::BarrierImage(
            cmd, image,
            curAccessMask, VK_ACCESS_SHADER_READ_BIT,
            curLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            curStageMask, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            firstMipmap);
    }
}

VkImageView TextureUploader::CreateImageView( VkImage                             image,
                                              VkFormat                            format,
                                              bool                                isCubemap,
                                              uint32_t                            mipmapCount,
                                              std::optional< QrTextureSwizzling > swizzling )
{
    VkComponentMapping mapping = {
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
    };

    if (swizzling)
    {
        mapping = MakeComponentMapping(swizzling.value());
    }

    const VkImageViewCreateInfo viewInfo = {
        .sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image      = image,
        .viewType   = isCubemap ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
        .format     = format,
        .components = mapping,
        .subresourceRange = {
            .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel   = 0,
            .levelCount     = mipmapCount,
            .baseArrayLayer = 0,
            .layerCount     = isCubemap ? 6u : 1u,
        },
    };

    VkImageView view = VK_NULL_HANDLE;
    const VkResult result = vkCreateImageView(device, &viewInfo, nullptr, &view);
    VK_CHECKERROR(result);

    return view;
}

TextureUploader::UploadResult TextureUploader::UploadImage(const UploadInfo &info)
{
    assert(!info.isCubemap);
    assert(info.frameIndex < MAX_FRAMES_IN_FLIGHT);

    const void *data = info.pData;
    const VkDeviceSize dataSize = info.dataSize;
    const QrExtent2D &size = info.baseSize;

    if (!info.isUpdateable)
    {
        assert(data != nullptr);
    }

    VkBufferCreateInfo stagingInfo{};
    stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingInfo.size = dataSize;
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VkBuffer stagingBuffers[MAX_FRAMES_IN_FLIGHT] = {};
    void *mappedDatas[MAX_FRAMES_IN_FLIGHT] = {};
    const uint32_t stagingCount = info.isUpdateable ? MAX_FRAMES_IN_FLIGHT : 1u;

    for (uint32_t i = 0; i < stagingCount; i++)
    {
        stagingBuffers[i] = memAllocator->CreateStagingSrcTextureBuffer(&stagingInfo, info.pDebugName, &mappedDatas[i]);
        if (stagingBuffers[i] == VK_NULL_HANDLE)
        {
            for (uint32_t j = 0; j < i; j++)
            {
                memAllocator->DestroyStagingSrcTextureBuffer(stagingBuffers[j]);
            }
            return {};
        }

        SET_DEBUG_NAME(device, stagingBuffers[i], VK_OBJECT_TYPE_BUFFER, info.pDebugName);
    }

    VkImage image = VK_NULL_HANDLE;

    if (!CreateImage(info, &image))
    {
        for (uint32_t i = 0; i < stagingCount; i++)
        {
            memAllocator->DestroyStagingSrcTextureBuffer(stagingBuffers[i]);
        }
        return {};
    }

    if (info.isUpdateable)
    {
        if (data == nullptr)
        {
            PrepareImage(image, nullptr, info, ImagePrepareType::INIT_WITHOUT_COPYING);
        }
        else
        {
            memcpy(mappedDatas[info.frameIndex], data, dataSize);
            PrepareImage(image, &stagingBuffers[info.frameIndex], info, ImagePrepareType::INIT);
        }
    }
    else
    {
        memcpy(mappedDatas[0], data, dataSize);
        PrepareImage(image, &stagingBuffers[0], info, ImagePrepareType::INIT);
    }

    const VkImageView imageView = CreateImageView(
        image, info.format, info.isCubemap, GetMipmapCount(size, info), info.swizzling);
    SET_DEBUG_NAME(device, imageView, VK_OBJECT_TYPE_IMAGE_VIEW, info.pDebugName);

    if (info.isUpdateable)
    {
        UpdateableImageInfo updateable = {};
        for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
        {
            updateable.stagingBuffers[i] = stagingBuffers[i];
            updateable.mappedData[i] = mappedDatas[i];
        }
        updateable.dataSize = static_cast<uint32_t>(dataSize);
        updateable.imageSize = size;
        updateable.generateMipmaps = info.useMipmaps;
        updateable.format = info.format;
        updateable.pregenerated = AreMipmapsPregenerated(info);

        updateableImageInfos[image] = updateable;
    }
    else
    {
        stagingToFree[info.frameIndex].push_back(stagingBuffers[0]);
    }

    return UploadResult
    {
        .wasUploaded = true,
        .image = image,
        .view = imageView,
    };
}

void TextureUploader::UpdateImage(VkCommandBuffer cmd, uint32_t frameIndex, VkImage targetImage, const void *data)
{
    assert(targetImage != VK_NULL_HANDLE);
    assert(data != nullptr);
    assert(frameIndex < MAX_FRAMES_IN_FLIGHT);

    const auto it = updateableImageInfos.find(targetImage);

    if (it == updateableImageInfos.end())
    {
        return;
    }

    UpdateableImageInfo &updateInfo = it->second;

    assert(updateInfo.mappedData[frameIndex] != nullptr);
    memcpy(updateInfo.mappedData[frameIndex], data, updateInfo.dataSize);

    UploadInfo info{};
    info.cmd = cmd;
    info.frameIndex = frameIndex;
    info.baseSize = updateInfo.imageSize;
    info.useMipmaps = updateInfo.generateMipmaps;
    info.format = updateInfo.format;

    PrepareImage(targetImage, &updateInfo.stagingBuffers[frameIndex], info, ImagePrepareType::UPDATE);
}

bool TextureUploader::CanUpdateImageFromHostData(VkImage image) const
{
    auto it = updateableImageInfos.find(image);

    return it != updateableImageInfos.end() && !it->second.pregenerated;
}

void TextureUploader::DestroyImage(VkImage image, VkImageView view)
{
    const auto it = updateableImageInfos.find(image);

    if (it != updateableImageInfos.end())
    {
        for (VkBuffer stagingBuffer : it->second.stagingBuffers)
        {
            if (stagingBuffer != VK_NULL_HANDLE)
            {
                memAllocator->DestroyStagingSrcTextureBuffer(stagingBuffer);
            }
        }
        updateableImageInfos.erase(it);
    }

    memAllocator->DestroyTextureImage(image);
    vkDestroyImageView(device, view, nullptr);
}
