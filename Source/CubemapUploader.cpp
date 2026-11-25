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

#include "CubemapUploader.h"

qray::CubemapUploader::CubemapUploader(VkDevice device, std::shared_ptr<MemoryAllocator> memAllocator)
    : TextureUploader(device, std::move(memAllocator))
{
}

qray::TextureUploader::UploadResult qray::CubemapUploader::UploadImage(const UploadInfo &info)
{
    assert(info.isCubemap);
    assert(!info.isUpdateable);

    constexpr uint32_t FaceCount = 6;

    const QrExtent2D &size = info.baseSize;

    UploadResult result{};
    result.wasUploaded = false;

    const VkDeviceSize faceSize = static_cast<VkDeviceSize>(info.dataSize);

    VkBufferCreateInfo stagingInfo{};
    stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingInfo.size = faceSize * FaceCount;
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    void *mappedData = nullptr;
    VkBuffer stagingBuffer = memAllocator->CreateStagingSrcTextureBuffer(&stagingInfo, info.pDebugName, &mappedData);
    if (stagingBuffer == VK_NULL_HANDLE)
    {
        return result;
    }

    SET_DEBUG_NAME(device, stagingBuffer, VK_OBJECT_TYPE_BUFFER, info.pDebugName);

    VkImage image = VK_NULL_HANDLE;
    const bool wasCreated = CreateImage(info, &image);

    if (!wasCreated)
    {
        memAllocator->DestroyStagingSrcTextureBuffer(stagingBuffer);
        return result;
    }

    VkBuffer stagingBuffers[FaceCount] = {};
    VkDeviceSize stagingOffsets[FaceCount] = {};
    uint8_t *dstBytes = static_cast<uint8_t *>(mappedData);

    for (uint32_t i = 0; i < FaceCount; i++)
    {
        const VkDeviceSize offset = static_cast<VkDeviceSize>(i) * faceSize;
        memcpy(dstBytes + offset, info.cubemap.pFaces[i], faceSize);
        stagingBuffers[i] = stagingBuffer;
        stagingOffsets[i] = offset;
    }

    PrepareImage(image, stagingBuffers, info, ImagePrepareType::INIT, stagingOffsets);

    const VkImageView imageView = CreateImageView(
        image, info.format, info.isCubemap, GetMipmapCount(size, info),
        QR_TEXTURE_SWIZZLING_ROUGHNESS_METALLIC_EMISSIVE);

    SET_DEBUG_NAME(device, imageView, VK_OBJECT_TYPE_IMAGE_VIEW, info.pDebugName);

    stagingToFree[info.frameIndex].push_back(stagingBuffer);

    result.wasUploaded = true;
    result.image = image;
    result.view = imageView;
    return result;
}
