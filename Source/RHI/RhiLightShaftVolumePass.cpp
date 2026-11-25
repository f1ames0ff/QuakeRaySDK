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

#include "RhiLightShaftVolumePass.h"

#include "RhiFrameContext.h"
#include "RhiPipeline.h"
#include "RhiResources.h"
#include "RhiTextureSource.h"
#include "RhiTextureTable.h"

#include "../Framebuffers.h"
#include "../Generated/ShaderCommonC.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace qray
{

namespace
{

constexpr const char *VOLUME_SHADER_FILE_NAME = "CmLightShaftVolume.comp.spv";
constexpr const char *PREFIX_SHADER_FILE_NAME = "CmLightShaftPrefix.comp.spv";
constexpr const char *ASSIGN_SHADER_FILE_NAME = "CmLightShaftAssign.comp.spv";
constexpr const char *DEPTH_BOUNDS_SHADER_FILE_NAME = "CmLightShaftDepthBounds.comp.spv";

void LogMessage(const RhiLightShaftVolumePass::PrintFunction &print, const char *pMessage)
{
    if (print)
    {
        print(pMessage);
    }
}

nvrhi::BufferHandle CreateUavStructuredBuffer(nvrhi::IDevice *pDevice,
                                              uint64_t byteSize,
                                              uint32_t stride,
                                              const std::string &debugName)
{
    nvrhi::BufferDesc desc;
    desc.byteSize = byteSize;
    desc.structStride = stride;
    desc.canHaveUAVs = true;
    desc.keepInitialState = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.debugName = debugName;
    return rhi::createBuffer(pDevice, desc, desc.debugName);
}

nvrhi::BindingSetHandle CreateSingleTextureUavSet(nvrhi::IDevice *pDevice,
                                                  nvrhi::IBindingLayout *pLayout,
                                                  nvrhi::ITexture *pTexture)
{
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::Texture_UAV(
        0, pTexture, nvrhi::Format::RGBA16_FLOAT, nvrhi::TextureSubresourceSet(),
        nvrhi::TextureDimension::Texture3D));
    return pDevice->createBindingSet(desc, pLayout);
}

}

RhiLightShaftVolumePass::RhiLightShaftVolumePass() = default;

RhiLightShaftVolumePass::~RhiLightShaftVolumePass() = default;

bool RhiLightShaftVolumePass::LoadShaders()
{
    volumeShader = rhi::loadShader(device, shaderFolderPath + VOLUME_SHADER_FILE_NAME,
                                   nvrhi::ShaderType::Compute, "RhiLightShaftVolumePass shader");
    prefixShader = rhi::loadShader(device, shaderFolderPath + PREFIX_SHADER_FILE_NAME,
                                   nvrhi::ShaderType::Compute, "RhiLightShaftVolumePass prefix shader");
    assignShader = rhi::loadShader(device, shaderFolderPath + ASSIGN_SHADER_FILE_NAME,
                                   nvrhi::ShaderType::Compute, "RhiLightShaftVolumePass assign shader");
    depthBoundsShader = rhi::loadShader(device, shaderFolderPath + DEPTH_BOUNDS_SHADER_FILE_NAME,
                                        nvrhi::ShaderType::Compute, "RhiLightShaftVolumePass depth bounds shader");
    if (volumeShader == nullptr || prefixShader == nullptr || assignShader == nullptr ||
        depthBoundsShader == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to load the light-shaft volume shader");
        return false;
    }

    return true;
}

bool RhiLightShaftVolumePass::CreateBindingLayouts()
{
    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::All;
        desc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
        tlasLayout = device->createBindingLayout(desc);
    }

    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                   .setConstantBufferOffset(0)
                                   .setShaderResourceOffset(0)
                                   .setSamplerOffset(0)
                                   .setUnorderedAccessViewOffset(0));
        desc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(1));
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2));
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3));
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
        desc.addItem(nvrhi::BindingLayoutItem::Sampler(5));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(6));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(7));
        paramsLayout = device->createBindingLayout(desc);
    }

    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setConstantBufferOffset(0));
        desc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
        uniformLayout = device->createBindingLayout(desc);
    }

    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setUnorderedAccessViewOffset(0));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(0));
        volumeLayout = device->createBindingLayout(desc);
    }

    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                   .setShaderResourceOffset(0)
                                   .setUnorderedAccessViewOffset(0));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(1));
        prefixLayout = device->createBindingLayout(desc);
    }

    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets().setUnorderedAccessViewOffset(0));
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0));
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));
        assignLayout = device->createBindingLayout(desc);
    }

    {
        nvrhi::BindingLayoutDesc desc;
        desc.visibility = nvrhi::ShaderType::Compute;
        desc.setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                   .setShaderResourceOffset(0)
                                   .setUnorderedAccessViewOffset(0));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(1));
        depthBoundsLayout = device->createBindingLayout(desc);
    }

    sampler = rhi::createSampler(device, nvrhi::SamplerDesc()
                                             .setMinFilter(true)
                                             .setMagFilter(true)
                                             .setMipFilter(true)
                                             .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp),
                                 "RhiLightShaftVolumePass sampler");

    if (tlasLayout == nullptr || paramsLayout == nullptr || uniformLayout == nullptr ||
        volumeLayout == nullptr || prefixLayout == nullptr || assignLayout == nullptr ||
        depthBoundsLayout == nullptr || sampler == nullptr ||
        !cloudShadowBinding.Create(device, frameContext, nvrhi::ShaderType::Compute))
    {
        LogMessage(print, "Warning: RHI: failed to create a light-shaft volume binding layout");
        return false;
    }

    return true;
}

bool RhiLightShaftVolumePass::CreatePipelines()
{
    if (textureTable == nullptr || textureTable->GetLayout() == nullptr || lightLayout == nullptr)
    {
        LogMessage(print, "Warning: RHI: the light-shaft volume pass requires texture and light layouts");
        return false;
    }

    {
        nvrhi::ComputePipelineDesc desc;
        desc.setComputeShader(volumeShader);
        desc.addBindingLayout(tlasLayout);
        desc.addBindingLayout(paramsLayout);
        desc.addBindingLayout(uniformLayout);
        desc.addBindingLayout(volumeLayout);
        desc.addBindingLayout(textureTable->GetLayout());
        desc.addBindingLayout(lightLayout);
        desc.addBindingLayout(cloudShadowBinding.GetLayout());
        volumePipeline = rhi::createComputePipeline(device, desc, "RhiLightShaftVolumePass");
    }

    {
        nvrhi::ComputePipelineDesc desc;
        desc.setComputeShader(prefixShader);
        desc.addBindingLayout(prefixLayout);
        desc.addBindingLayout(uniformLayout);
        desc.addBindingLayout(paramsLayout);
        prefixPipeline = rhi::createComputePipeline(device, desc, "RhiLightShaftVolumePass prefix");
    }

    {
        nvrhi::ComputePipelineDesc desc;
        desc.setComputeShader(assignShader);
        desc.addBindingLayout(paramsLayout);
        desc.addBindingLayout(assignLayout);
        desc.addBindingLayout(uniformLayout);
        desc.addBindingLayout(tlasLayout);
        assignPipeline = rhi::createComputePipeline(device, desc, "RhiLightShaftVolumePass assign");
    }

    {
        nvrhi::ComputePipelineDesc desc;
        desc.setComputeShader(depthBoundsShader);
        desc.addBindingLayout(depthBoundsLayout);
        desc.addBindingLayout(uniformLayout);
        depthBoundsPipeline = rhi::createComputePipeline(device, desc, "RhiLightShaftVolumePass depth bounds");
    }

    if (volumePipeline == nullptr || prefixPipeline == nullptr || assignPipeline == nullptr ||
        depthBoundsPipeline == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create the light-shaft volume pipeline");
        return false;
    }

    return true;
}

bool RhiLightShaftVolumePass::Create(nvrhi::IDevice *pDevice,
                                     rhi::RhiFrameContext *pFrameContext,
                                     rhi::RhiTextureTable *pTextureTable,
                                     nvrhi::IBindingLayout *pLightLayout,
                                     const char *pShaderFolderPath,
                                     PrintFunction pfnPrint)
{
    if (created)
    {
        return true;
    }

    device = pDevice;
    frameContext = pFrameContext;
    textureTable = pTextureTable;
    lightLayout = pLightLayout;
    shaderFolderPath = pShaderFolderPath != nullptr ? pShaderFolderPath : "";
    print = std::move(pfnPrint);

    if (device == nullptr || frameContext == nullptr || !frameContext->IsCreated())
    {
        LogMessage(print, "Warning: RHI: the light-shaft volume pass needs a device and frame context");
        return false;
    }

    if (!LoadShaders() || !CreateBindingLayouts() || !CreatePipelines())
    {
        return false;
    }

    if (!CreateVolumeTargets())
    {
        LogMessage(print, "Warning: RHI: failed to create a light-shaft volume target");
        return false;
    }

    created = true;
    return true;
}

void RhiLightShaftVolumePass::RetireTarget(Target &target)
{
    if (frameContext != nullptr)
    {
        frameContext->Retire(std::move(target.uavSet));
        frameContext->Retire(std::move(target.integratedUavSet));
        frameContext->Retire(std::move(target.prefixSet));
        frameContext->Retire(std::move(target.paramsSet));
        frameContext->Retire(std::move(target.assignSet));
        frameContext->Retire(std::move(target.depthBoundsSet));
        frameContext->Retire(std::move(target.uniformSet));
        frameContext->Retire(std::move(target.tlasSet));
        frameContext->Retire(std::move(target.texture));
        frameContext->Retire(std::move(target.integratedTexture));
        frameContext->Retire(std::move(target.depthBoundsTexture));
        frameContext->Retire(std::move(target.depthWorldTexture));
        frameContext->Retire(std::move(target.paramsBuffer));
        frameContext->Retire(std::move(target.lightListBuffer));
        frameContext->Retire(std::move(target.sunVisibilityBuffer));
        frameContext->Retire(std::move(target.rayCounterBuffer));
    }

    target = {};
}

bool RhiLightShaftVolumePass::AllocateTargetResources(uint32_t slotIndex, uint32_t lightListEntries)
{
    Target &target = targets[slotIndex];

    nvrhi::TextureDesc textureDesc;
    textureDesc.dimension = nvrhi::TextureDimension::Texture3D;
    textureDesc.format = nvrhi::Format::RGBA16_FLOAT;
    textureDesc.width = volumeSizeX;
    textureDesc.height = volumeSizeY;
    textureDesc.depth = volumeSizeZ;
    textureDesc.mipLevels = 1;
    textureDesc.isUAV = true;
    textureDesc.initialState = nvrhi::ResourceStates::NonPixelShaderResource;
    textureDesc.keepInitialState = true;

    const std::string prefix = "RhiLightShaftVolumePass slot " + std::to_string(slotIndex);
    target.texture = rhi::createTexture(device, textureDesc, prefix + " inject");
    target.integratedTexture = rhi::createTexture(device, textureDesc, prefix + " integrated");

    nvrhi::TextureDesc depthBoundsDesc;
    depthBoundsDesc.dimension = nvrhi::TextureDimension::Texture2D;
    depthBoundsDesc.format = nvrhi::Format::R32_FLOAT;
    depthBoundsDesc.width = volumeSizeX;
    depthBoundsDesc.height = volumeSizeY;
    depthBoundsDesc.mipLevels = 1;
    depthBoundsDesc.isUAV = true;
    depthBoundsDesc.initialState = nvrhi::ResourceStates::NonPixelShaderResource;
    depthBoundsDesc.keepInitialState = true;
    target.depthBoundsTexture = rhi::createTexture(device, depthBoundsDesc, prefix + " depth bounds");

    if (target.texture == nullptr || target.integratedTexture == nullptr ||
        target.depthBoundsTexture == nullptr)
    {
        return false;
    }

    nvrhi::BufferDesc bufferDesc;
    bufferDesc.isConstantBuffer = true;
    bufferDesc.byteSize = sizeof(Params);
    bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    bufferDesc.keepInitialState = true;
    bufferDesc.debugName = prefix + " params";
    target.paramsBuffer = rhi::createBuffer(device, bufferDesc, bufferDesc.debugName);

    target.lightListBuffer = CreateUavStructuredBuffer(
        device, uint64_t(lightListEntries) * sizeof(uint32_t), sizeof(uint32_t), prefix + " light list");
    target.sunVisibilityBuffer = CreateUavStructuredBuffer(
        device, uint64_t(LIGHT_LIST_MAX_CLUSTERS) * sizeof(float), sizeof(float), prefix + " sun visibility");
    target.rayCounterBuffer = CreateUavStructuredBuffer(
        device, sizeof(uint32_t), sizeof(uint32_t), prefix + " ray counter");

    return target.paramsBuffer != nullptr && target.lightListBuffer != nullptr &&
           target.sunVisibilityBuffer != nullptr && target.rayCounterBuffer != nullptr;
}

bool RhiLightShaftVolumePass::CreateTargetBindingSets(uint32_t slotIndex)
{
    Target &target = targets[slotIndex];
    const uint32_t previous = (slotIndex + 1) % MAX_FRAMES_IN_FLIGHT;

    target.uavSet = CreateSingleTextureUavSet(device, volumeLayout, target.texture);
    target.integratedUavSet = CreateSingleTextureUavSet(device, volumeLayout, target.integratedTexture);

    nvrhi::BindingSetDesc prefixSetDesc;
    prefixSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, target.texture));
    prefixSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(
        1, target.integratedTexture, nvrhi::Format::RGBA16_FLOAT, nvrhi::TextureSubresourceSet(),
        nvrhi::TextureDimension::Texture3D));
    target.prefixSet = device->createBindingSet(prefixSetDesc, prefixLayout);

    nvrhi::BindingSetDesc paramsSetDesc;
    paramsSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, target.paramsBuffer));
    paramsSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(1, targets[previous].texture));
    paramsSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, target.lightListBuffer));
    paramsSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(3, target.rayCounterBuffer));
    paramsSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, target.sunVisibilityBuffer));
    paramsSetDesc.addItem(nvrhi::BindingSetItem::Sampler(5, sampler));
    paramsSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(6, target.depthBoundsTexture));
    paramsSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(7, targets[previous].depthBoundsTexture));
    target.paramsSet = device->createBindingSet(paramsSetDesc, paramsLayout);

    nvrhi::BindingSetDesc assignSetDesc;
    assignSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, target.lightListBuffer));
    assignSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, target.sunVisibilityBuffer));
    target.assignSet = device->createBindingSet(assignSetDesc, assignLayout);

    target.depthBoundsSet = nullptr;
    target.depthWorldTexture = nullptr;
    target.depthWorldHandle = 0;
    target.depthWorldWidth = 0;
    target.depthWorldHeight = 0;
    target.uniformSet = nullptr;
    target.uniformBuffer = nullptr;
    target.tlasSet = nullptr;
    target.topLevel = nullptr;

    return target.uavSet != nullptr && target.integratedUavSet != nullptr &&
           target.prefixSet != nullptr && target.paramsSet != nullptr && target.assignSet != nullptr;
}

bool RhiLightShaftVolumePass::CreateVolumeTargets()
{
    const uint32_t lightListEntries =
        (1u + LIGHT_LIST_CAPACITY) * LIGHT_LIST_MAX_CLUSTERS;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        RetireTarget(targets[i]);
        if (!AllocateTargetResources(i, lightListEntries))
        {
            return false;
        }
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (!CreateTargetBindingSets(i))
        {
            return false;
        }
    }

    historyReset = true;
    depthBoundsCleared = false;
    return true;
}

bool RhiLightShaftVolumePass::PrepareUniformSet(Target &target, nvrhi::IBuffer *pUniformBuffer)
{
    if (target.uniformSet != nullptr && target.uniformBuffer == pUniformBuffer)
    {
        return true;
    }

    if (pUniformBuffer == nullptr)
    {
        return false;
    }

    if (target.uniformSet != nullptr)
    {
        frameContext->Retire(target.uniformSet);
    }
    target.uniformSet = nullptr;

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, pUniformBuffer));
    target.uniformSet = device->createBindingSet(setDesc, uniformLayout);
    if (target.uniformSet == nullptr)
    {
        return false;
    }

    target.uniformBuffer = pUniformBuffer;
    return true;
}

bool RhiLightShaftVolumePass::PrepareTlasSet(Target &target, nvrhi::rt::IAccelStruct *pTopLevel)
{
    if (pTopLevel == nullptr)
    {
        return false;
    }

    if (target.tlasSet != nullptr && target.topLevel == pTopLevel)
    {
        return true;
    }

    if (target.tlasSet != nullptr)
    {
        frameContext->Retire(target.tlasSet);
    }
    target.tlasSet = nullptr;

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, pTopLevel));
    target.tlasSet = device->createBindingSet(setDesc, tlasLayout);
    if (target.tlasSet == nullptr)
    {
        return false;
    }

    target.topLevel = pTopLevel;
    return true;
}

bool RhiLightShaftVolumePass::EnsureVolumeSize(const Params &params)
{
    const uint32_t requestedX = std::clamp(params.volumeSizeX, 32u, MAX_VOLUME_SIZE_X);
    const uint32_t requestedY = std::clamp(params.volumeSizeY, 32u, MAX_VOLUME_SIZE_Y);
    const uint32_t requestedZ = std::clamp(params.volumeSizeZ, 16u, MAX_VOLUME_SIZE_Z);

    if (requestedX == volumeSizeX && requestedY == volumeSizeY && requestedZ == volumeSizeZ)
    {
        return true;
    }

    volumeSizeX = requestedX;
    volumeSizeY = requestedY;
    volumeSizeZ = requestedZ;
    return CreateVolumeTargets();
}

bool RhiLightShaftVolumePass::IsTargetReady(const Target &target) const
{
    return target.texture != nullptr && target.integratedTexture != nullptr &&
           target.depthBoundsTexture != nullptr && target.uavSet != nullptr &&
           target.paramsSet != nullptr && target.assignSet != nullptr;
}

void RhiLightShaftVolumePass::RetireDepthBoundsResources(Target &target)
{
    if (frameContext != nullptr)
    {
        frameContext->Retire(std::move(target.depthBoundsSet));
        frameContext->Retire(std::move(target.depthWorldTexture));
    }
    target.depthBoundsSet = nullptr;
    target.depthWorldTexture = nullptr;
    target.depthWorldHandle = 0;
    target.depthWorldWidth = 0;
    target.depthWorldHeight = 0;
}

bool RhiLightShaftVolumePass::PrepareDepthBoundsSet(Target &target,
                                                    uint32_t frameIndex,
                                                    const DepthBoundsInput &depthBounds)
{
    if (depthBounds.framebuffers == nullptr || depthBounds.renderWidth == 0 ||
        depthBounds.renderHeight == 0 || target.depthBoundsTexture == nullptr)
    {
        return false;
    }

    const ResolutionState resolutionState = { depthBounds.renderWidth, depthBounds.renderHeight, 0, 0 };
    const auto [image, view, format, extent] =
        depthBounds.framebuffers->GetImageHandles(FB_IMAGE_INDEX_DEPTH_WORLD, frameIndex, resolutionState);
    const uint64_t imageHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(image));
    const uint64_t imageView = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(view));
    if (imageHandle == 0 || extent.width != depthBounds.renderWidth ||
        extent.height != depthBounds.renderHeight)
    {
        return false;
    }

    const bool ready = target.depthWorldHandle == imageHandle &&
                       target.depthWorldWidth == extent.width &&
                       target.depthWorldHeight == extent.height &&
                       target.depthWorldTexture != nullptr &&
                       target.depthBoundsSet != nullptr;
    if (ready)
    {
        return true;
    }

    RetireDepthBoundsResources(target);

    const std::string debugName =
        "RhiLightShaftVolumePass depthWorld frame " + std::to_string(frameIndex);
    target.depthWorldTexture = rhi::wrapEngineStorageImage(
        device, imageHandle, imageView, format, extent.width, extent.height, debugName);
    if (target.depthWorldTexture == nullptr)
    {
        return false;
    }

    nvrhi::BindingSetDesc setDesc;
    setDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, target.depthWorldTexture));
    setDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(
        1, target.depthBoundsTexture, nvrhi::Format::R32_FLOAT, nvrhi::TextureSubresourceSet(),
        nvrhi::TextureDimension::Texture2D));
    target.depthBoundsSet = device->createBindingSet(setDesc, depthBoundsLayout);
    if (target.depthBoundsSet == nullptr)
    {
        RetireDepthBoundsResources(target);
        return false;
    }

    target.depthWorldHandle = imageHandle;
    target.depthWorldWidth = extent.width;
    target.depthWorldHeight = extent.height;
    return true;
}

void RhiLightShaftVolumePass::ClearDepthBoundsIfNeeded(nvrhi::ICommandList *pCommandList)
{
    if (depthBoundsCleared)
    {
        return;
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        pCommandList->clearTextureFloat(targets[i].depthBoundsTexture, nvrhi::AllSubresources,
                                        nvrhi::Color(1e6f, 0.f, 0.f, 0.f));
        pCommandList->setTextureState(targets[i].depthBoundsTexture, nvrhi::AllSubresources,
                                      nvrhi::ResourceStates::NonPixelShaderResource);
    }
    depthBoundsCleared = true;
}

void RhiLightShaftVolumePass::DispatchDepthBounds(nvrhi::ICommandList *pCommandList, Target &target)
{
    pCommandList->beginTrackingTextureState(target.depthWorldTexture, nvrhi::AllSubresources,
                                            nvrhi::ResourceStates::UnorderedAccess);
    pCommandList->setTextureState(target.depthBoundsTexture, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::UnorderedAccess);

    nvrhi::ComputeState depthBoundsState;
    depthBoundsState.setPipeline(depthBoundsPipeline);
    depthBoundsState.addBindingSet(target.depthBoundsSet);
    depthBoundsState.addBindingSet(target.uniformSet);
    pCommandList->setComputeState(depthBoundsState);
    pCommandList->dispatch((volumeSizeX + 7) / 8, (volumeSizeY + 7) / 8, 1);

    pCommandList->setTextureState(target.depthBoundsTexture, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::NonPixelShaderResource);
    pCommandList->setTextureState(target.depthWorldTexture, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::UnorderedAccess);
    pCommandList->commitBarriers();
}

bool RhiLightShaftVolumePass::Render(nvrhi::ICommandList *pCommandList,
                                     uint32_t frameIndex,
                                     nvrhi::IBuffer *pUniformBuffer,
                                     nvrhi::rt::IAccelStruct *pTopLevel,
                                     nvrhi::IBindingSet *pLightSet,
                                     const Params &params,
                                     const DepthBoundsInput &depthBounds)
{
    if (!created || pCommandList == nullptr || frameIndex >= MAX_FRAMES_IN_FLIGHT ||
        !EnsureVolumeSize(params) || pLightSet == nullptr ||
        textureTable == nullptr || textureTable->GetTable() == nullptr)
    {
        return false;
    }

    Target &target = targets[frameIndex];
    if (!IsTargetReady(target) ||
        !PrepareUniformSet(target, pUniformBuffer) ||
        !PrepareTlasSet(target, pTopLevel))
    {
        return false;
    }

    ClearDepthBoundsIfNeeded(pCommandList);

    const bool hasDepthBounds = PrepareDepthBoundsSet(target, frameIndex, depthBounds);

    Params effectiveParams = params;
    effectiveParams.depthBoundsEnabled = hasDepthBounds ? 1u : 0u;

    rhi::writeBuffer(pCommandList, target.paramsBuffer, &effectiveParams, sizeof(effectiveParams));
    pCommandList->clearBufferUInt(target.rayCounterBuffer, 0);

    if (hasDepthBounds)
    {
        DispatchDepthBounds(pCommandList, target);
    }

    if (effectiveParams.lightListClusterCount > 0)
    {
        nvrhi::ComputeState assignState;
        assignState.setPipeline(assignPipeline);
        assignState.addBindingSet(target.paramsSet);
        assignState.addBindingSet(target.assignSet);
        assignState.addBindingSet(target.uniformSet);
        assignState.addBindingSet(target.tlasSet);
        pCommandList->setComputeState(assignState);
        pCommandList->dispatch((effectiveParams.lightListClusterCount + 63) / 64, 1, 1);
        pCommandList->commitBarriers();
    }

    pCommandList->setTextureState(target.texture, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::UnorderedAccess);

    nvrhi::ComputeState state;
    state.setPipeline(volumePipeline);
    state.addBindingSet(target.tlasSet);
    state.addBindingSet(target.paramsSet);
    state.addBindingSet(target.uniformSet);
    state.addBindingSet(target.uavSet);
    state.addBindingSet(textureTable->GetTable());
    state.addBindingSet(pLightSet);
    state.addBindingSet(cloudShadowBinding.GetSet());
    pCommandList->setComputeState(state);

    const uint32_t groupsX = (volumeSizeX + GROUP_SIZE_X - 1) / GROUP_SIZE_X;
    const uint32_t groupsY = (volumeSizeY + GROUP_SIZE_Y - 1) / GROUP_SIZE_Y;
    const uint32_t groupsZ = (volumeSizeZ + GROUP_SIZE_Z - 1) / GROUP_SIZE_Z;
    pCommandList->dispatch(groupsX, groupsY, groupsZ);

    pCommandList->commitBarriers();

    nvrhi::ComputeState prefixState;
    prefixState.setPipeline(prefixPipeline);
    prefixState.addBindingSet(target.prefixSet);
    prefixState.addBindingSet(target.uniformSet);
    prefixState.addBindingSet(target.paramsSet);
    pCommandList->setComputeState(prefixState);
    pCommandList->dispatch((volumeSizeX + 7) / 8, (volumeSizeY + 7) / 8, 1);

    pCommandList->commitBarriers();

    pCommandList->setTextureState(target.integratedTexture, nvrhi::AllSubresources,
                                  nvrhi::ResourceStates::NonPixelShaderResource);

    return true;
}

nvrhi::ITexture *RhiLightShaftVolumePass::GetTexture(uint32_t frameIndex) const
{
    if (frameIndex >= MAX_FRAMES_IN_FLIGHT)
    {
        return nullptr;
    }

    return targets[frameIndex].integratedTexture;
}

nvrhi::ITexture *RhiLightShaftVolumePass::GetInjectionTexture(uint32_t frameIndex) const
{
    if (frameIndex >= MAX_FRAMES_IN_FLIGHT)
    {
        return nullptr;
    }

    return targets[frameIndex].texture;
}

bool RhiLightShaftVolumePass::ConsumeHistoryReset()
{
    const bool reset = historyReset;
    historyReset = false;
    return reset;
}

}
