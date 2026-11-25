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

#include "RhiResources.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <string>

namespace qray::rhi
{

nvrhi::BufferHandle createBuffer(nvrhi::IDevice *device, const nvrhi::BufferDesc &desc, std::string_view debugName)
{
    nvrhi::BufferDesc namedDesc = desc;
    namedDesc.debugName = std::string(debugName);

    return device->createBuffer(namedDesc);
}

nvrhi::TextureHandle createTexture(nvrhi::IDevice *device, const nvrhi::TextureDesc &desc, std::string_view debugName)
{
    nvrhi::TextureDesc namedDesc = desc;
    namedDesc.debugName = std::string(debugName);

    return device->createTexture(namedDesc);
}

nvrhi::SamplerHandle createSampler(nvrhi::IDevice *device, const nvrhi::SamplerDesc &desc, std::string_view debugName)
{
    // nvrhi::SamplerDesc has no debugName at the pinned revision, so the name is not applied.
    (void)debugName;

    return device->createSampler(desc);
}

void writeBuffer(nvrhi::ICommandList *commandList, nvrhi::IBuffer *buffer, const void *data, size_t size, size_t offset)
{
    commandList->writeBuffer(buffer, data, size, uint64_t(offset));
}

nvrhi::TextureHandle createWhiteTexture(nvrhi::IDevice *device, nvrhi::ICommandList *commandList, std::string_view debugName)
{
    nvrhi::TextureDesc desc;
    desc.width = 1;
    desc.height = 1;
    desc.format = nvrhi::Format::RGBA8_UNORM;
    desc.isShaderResource = true;

    // The texture rests in ShaderResource, and that is what initialState has to say. It is not
    // CopyDest: NVRHI's writeTexture moves the texture to CopyDest by itself (it places the
    // undefined -> TransferDst barrier as the first use), while initialState is the state that
    // keepInitialState restores at the end of every command list. With CopyDest there, the
    // setTextureState(ShaderResource) below would be undone on close and every sampling pass
    // would have to transition the texture again. With ShaderResource there, the tracker starts
    // every later command list already knowing the texture is samplable, so no pass needs a
    // beginTrackingTextureState call and no state change is emitted per frame.
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;

    nvrhi::TextureHandle texture = createTexture(device, desc, debugName);
    if (texture == nullptr)
    {
        return nullptr;
    }

    const uint32_t white = uint32_t(0xFFFFFFFF);
    commandList->writeTexture(texture, 0, 0, &white, sizeof(white));

    // writeTexture only guarantees CopyDest; this turns the texture into a shader resource now
    // (a CopyDest -> ShaderResource barrier goes into this command list) and, because the
    // resting state above equals it, the close of the command list leaves it there.
    commandList->setTextureState(texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);

    return texture;
}

void generateCubemapMips(nvrhi::ICommandList *commandList,
                         nvrhi::ITexture *cubemap,
                         uint32_t baseSize,
                         uint32_t mipLevels,
                         nvrhi::ResourceStates finalState)
{
    if (commandList == nullptr || cubemap == nullptr)
    {
        return;
    }

    if (mipLevels <= 1)
    {
        commandList->setTextureState(cubemap, nvrhi::AllSubresources, finalState);
        return;
    }

    const uint32_t arraySize = cubemap->getDesc().arraySize;

    // Move mip 0 to CopySource and mips 1..N-1 to CopyDest so vkCmdBlitImage can walk down
    // the chain across all layers, then flush NVRHI's queued barriers onto VkCommandBuffer.
    commandList->setTextureState(
        cubemap,
        nvrhi::TextureSubresourceSet(0, 1, 0, nvrhi::TextureSubresourceSet::AllArraySlices),
        nvrhi::ResourceStates::CopySource);
    commandList->setTextureState(
        cubemap,
        nvrhi::TextureSubresourceSet(1, nvrhi::MipLevel(mipLevels - 1), 0, nvrhi::TextureSubresourceSet::AllArraySlices),
        nvrhi::ResourceStates::CopyDest);
    commandList->commitBarriers();

    const VkCommandBuffer cmd = static_cast<VkCommandBuffer>(
        commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer).pointer);
    const VkImage image = static_cast<VkImage>(
        cubemap->getNativeObject(nvrhi::ObjectTypes::VK_Image).pointer);

    if (cmd == VK_NULL_HANDLE || image == VK_NULL_HANDLE)
    {
        commandList->setTextureState(cubemap, nvrhi::AllSubresources, finalState);
        return;
    }

    for (uint32_t mip = 1; mip < mipLevels; ++mip)
    {
        const uint32_t srcSize = std::max(1u, baseSize >> (mip - 1));
        const uint32_t dstSize = std::max(1u, baseSize >> mip);

        VkImageBlit blit = {};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = mip - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = arraySize;
        blit.srcOffsets[0] = { 0, 0, 0 };
        blit.srcOffsets[1] = { int32_t(srcSize), int32_t(srcSize), 1 };

        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = mip;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = arraySize;
        blit.dstOffsets[0] = { 0, 0, 0 };
        blit.dstOffsets[1] = { int32_t(dstSize), int32_t(dstSize), 1 };

        vkCmdBlitImage(
            cmd,
            image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);

        if (mip + 1 < mipLevels)
        {
            VkImageMemoryBarrier barrier = {};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = mip;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = arraySize;

            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0,
                0, nullptr,
                0, nullptr,
                1, &barrier);
        }
    }

    // Re-sync NVRHI's state tracker with the actual Vulkan layouts before transitioning all mips to finalState.
    commandList->beginTrackingTextureState(
        cubemap,
        nvrhi::TextureSubresourceSet(0, nvrhi::MipLevel(mipLevels - 1), 0, nvrhi::TextureSubresourceSet::AllArraySlices),
        nvrhi::ResourceStates::CopySource);
    commandList->beginTrackingTextureState(
        cubemap,
        nvrhi::TextureSubresourceSet(nvrhi::MipLevel(mipLevels - 1), 1, 0, nvrhi::TextureSubresourceSet::AllArraySlices),
        nvrhi::ResourceStates::CopyDest);
    commandList->setTextureState(cubemap, nvrhi::AllSubresources, finalState);
}

}
