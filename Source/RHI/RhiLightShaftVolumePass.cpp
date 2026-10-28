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
#include "RhiTextureTable.h"

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

void LogMessage(const RhiLightShaftVolumePass::PrintFunction &print, const char *pMessage)
{
    if (print)
    {
        print(pMessage);
    }
}

}

RhiLightShaftVolumePass::RhiLightShaftVolumePass() = default;

RhiLightShaftVolumePass::~RhiLightShaftVolumePass() = default;

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

    volumeShader = rhi::loadShader(device, shaderFolderPath + VOLUME_SHADER_FILE_NAME,
                                   nvrhi::ShaderType::Compute, "RhiLightShaftVolumePass shader");
    prefixShader = rhi::loadShader(device, shaderFolderPath + PREFIX_SHADER_FILE_NAME,
                                   nvrhi::ShaderType::Compute, "RhiLightShaftVolumePass prefix shader");
    assignShader = rhi::loadShader(device, shaderFolderPath + ASSIGN_SHADER_FILE_NAME,
                                   nvrhi::ShaderType::Compute, "RhiLightShaftVolumePass assign shader");
    if (volumeShader == nullptr || prefixShader == nullptr || assignShader == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to load the light-shaft volume shader");
        return false;
    }

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
                                   .setUnorderedAccessViewOffset(0));
        desc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
        desc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(1));
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2));
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3));
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
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

    sampler = rhi::createSampler(device, nvrhi::SamplerDesc()
                                             .setMinFilter(true)
                                             .setMagFilter(true)
                                             .setMipFilter(true)
                                             .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp),
                                 "RhiLightShaftVolumePass sampler");

    if (tlasLayout == nullptr || paramsLayout == nullptr || uniformLayout == nullptr ||
        volumeLayout == nullptr || prefixLayout == nullptr || assignLayout == nullptr || sampler == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create a light-shaft volume binding layout");
        return false;
    }

    {
        nvrhi::ComputePipelineDesc desc;
        desc.setComputeShader(volumeShader);
        desc.addBindingLayout(tlasLayout);
        desc.addBindingLayout(paramsLayout);
        desc.addBindingLayout(uniformLayout);
        desc.addBindingLayout(volumeLayout);
        if (textureTable != nullptr && textureTable->GetLayout() != nullptr)
        {
            desc.addBindingLayout(textureTable->GetLayout());
        }
        if (lightLayout != nullptr)
        {
            desc.addBindingLayout(lightLayout);
        }
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

    if (volumePipeline == nullptr || prefixPipeline == nullptr || assignPipeline == nullptr)
    {
        LogMessage(print, "Warning: RHI: failed to create the light-shaft volume pipeline");
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

bool RhiLightShaftVolumePass::CreateVolumeTargets()
{
    const uint32_t lightListEntries =
        (1u + LIGHT_LIST_CAPACITY) * LIGHT_LIST_MAX_CLUSTERS;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        Target &target = targets[i];

        if (frameContext != nullptr)
        {
            frameContext->Retire(std::move(target.uavSet));
            frameContext->Retire(std::move(target.integratedUavSet));
            frameContext->Retire(std::move(target.prefixSet));
            frameContext->Retire(std::move(target.paramsSet));
            frameContext->Retire(std::move(target.assignSet));
            frameContext->Retire(std::move(target.uniformSet));
            frameContext->Retire(std::move(target.tlasSet));
            frameContext->Retire(std::move(target.texture));
            frameContext->Retire(std::move(target.integratedTexture));
            frameContext->Retire(std::move(target.paramsBuffer));
            frameContext->Retire(std::move(target.lightListBuffer));
            frameContext->Retire(std::move(target.sunVisibilityBuffer));
            frameContext->Retire(std::move(target.rayCounterBuffer));
        }

        target.uavSet = nullptr;
        target.integratedUavSet = nullptr;
        target.prefixSet = nullptr;
        target.paramsSet = nullptr;
        target.assignSet = nullptr;
        target.uniformSet = nullptr;
        target.tlasSet = nullptr;
        target.texture = nullptr;
        target.integratedTexture = nullptr;
        target.paramsBuffer = nullptr;
        target.lightListBuffer = nullptr;
        target.sunVisibilityBuffer = nullptr;
        target.rayCounterBuffer = nullptr;
        target.uniformBuffer = nullptr;
        target.topLevel = nullptr;

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

        const std::string prefix = "RhiLightShaftVolumePass slot " + std::to_string(i);
        target.texture = rhi::createTexture(device, textureDesc, prefix + " inject");
        target.integratedTexture = rhi::createTexture(device, textureDesc, prefix + " integrated");
        if (target.texture == nullptr || target.integratedTexture == nullptr)
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
        if (target.paramsBuffer == nullptr)
        {
            return false;
        }

        nvrhi::BufferDesc listDesc;
        listDesc.byteSize = uint64_t(lightListEntries) * sizeof(uint32_t);
        listDesc.structStride = sizeof(uint32_t);
        listDesc.canHaveUAVs = true;
        listDesc.keepInitialState = true;
        listDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        listDesc.debugName = prefix + " light list";
        target.lightListBuffer = rhi::createBuffer(device, listDesc, listDesc.debugName);
        if (target.lightListBuffer == nullptr)
        {
            return false;
        }

        nvrhi::BufferDesc sunDesc;
        sunDesc.byteSize = uint64_t(LIGHT_LIST_MAX_CLUSTERS) * sizeof(float);
        sunDesc.structStride = sizeof(float);
        sunDesc.canHaveUAVs = true;
        sunDesc.keepInitialState = true;
        sunDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        sunDesc.debugName = prefix + " sun visibility";
        target.sunVisibilityBuffer = rhi::createBuffer(device, sunDesc, sunDesc.debugName);
        if (target.sunVisibilityBuffer == nullptr)
        {
            return false;
        }

        nvrhi::BufferDesc counterDesc;
        counterDesc.byteSize = sizeof(uint32_t);
        counterDesc.structStride = sizeof(uint32_t);
        counterDesc.canHaveUAVs = true;
        counterDesc.keepInitialState = true;
        counterDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        counterDesc.debugName = prefix + " ray counter";
        target.rayCounterBuffer = rhi::createBuffer(device, counterDesc, counterDesc.debugName);
        if (target.rayCounterBuffer == nullptr)
        {
            return false;
        }
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        Target &target = targets[i];
        const uint32_t previous = (i + 1) % MAX_FRAMES_IN_FLIGHT;

        nvrhi::BindingSetDesc uavSetDesc;
        uavSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(
            0, target.texture, nvrhi::Format::RGBA16_FLOAT, nvrhi::TextureSubresourceSet(),
            nvrhi::TextureDimension::Texture3D));
        target.uavSet = device->createBindingSet(uavSetDesc, volumeLayout);

        nvrhi::BindingSetDesc integratedSetDesc;
        integratedSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(
            0, target.integratedTexture, nvrhi::Format::RGBA16_FLOAT, nvrhi::TextureSubresourceSet(),
            nvrhi::TextureDimension::Texture3D));
        target.integratedUavSet = device->createBindingSet(integratedSetDesc, volumeLayout);

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
        target.paramsSet = device->createBindingSet(paramsSetDesc, paramsLayout);

        nvrhi::BindingSetDesc assignSetDesc;
        assignSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, target.lightListBuffer));
        assignSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, target.sunVisibilityBuffer));
        target.assignSet = device->createBindingSet(assignSetDesc, assignLayout);

        if (target.uavSet == nullptr || target.integratedUavSet == nullptr ||
            target.prefixSet == nullptr || target.paramsSet == nullptr || target.assignSet == nullptr)
        {
            return false;
        }

        target.uniformSet = nullptr;
        target.uniformBuffer = nullptr;
        target.tlasSet = nullptr;
        target.topLevel = nullptr;
    }

    historyReset = true;
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

bool RhiLightShaftVolumePass::Render(nvrhi::ICommandList *pCommandList,
                                     uint32_t frameIndex,
                                     nvrhi::IBuffer *pUniformBuffer,
                                     nvrhi::rt::IAccelStruct *pTopLevel,
                                     nvrhi::IBindingSet *pLightSet,
                                     const Params &params)
{
    if (!created || pCommandList == nullptr || frameIndex >= MAX_FRAMES_IN_FLIGHT)
    {
        return false;
    }

    const uint32_t requestedX = std::clamp(params.volumeSizeX, 32u, 512u);
    const uint32_t requestedY = std::clamp(params.volumeSizeY, 32u, 512u);
    const uint32_t requestedZ = std::clamp(params.volumeSizeZ, 16u, 256u);

    if (requestedX != volumeSizeX || requestedY != volumeSizeY || requestedZ != volumeSizeZ)
    {
        volumeSizeX = requestedX;
        volumeSizeY = requestedY;
        volumeSizeZ = requestedZ;

        if (!CreateVolumeTargets())
        {
            return false;
        }
    }

    if (pLightSet == nullptr || textureTable == nullptr || textureTable->GetTable() == nullptr)
    {
        return false;
    }

    Target &target = targets[frameIndex];
    if (target.texture == nullptr || target.integratedTexture == nullptr ||
        target.uavSet == nullptr || target.paramsSet == nullptr || target.assignSet == nullptr)
    {
        return false;
    }

    if (!PrepareUniformSet(target, pUniformBuffer) || !PrepareTlasSet(target, pTopLevel))
    {
        return false;
    }

    rhi::writeBuffer(pCommandList, target.paramsBuffer, &params, sizeof(params));
    pCommandList->clearBufferUInt(target.rayCounterBuffer, 0);

    if (params.lightListClusterCount > 0)
    {
        nvrhi::ComputeState assignState;
        assignState.setPipeline(assignPipeline);
        assignState.addBindingSet(target.paramsSet);
        assignState.addBindingSet(target.assignSet);
        assignState.addBindingSet(target.uniformSet);
        assignState.addBindingSet(target.tlasSet);
        pCommandList->setComputeState(assignState);
        pCommandList->dispatch((params.lightListClusterCount + 63) / 64, 1, 1);
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
    pCommandList->setComputeState(state);

    const uint32_t groupsX = (volumeSizeX + GROUP_SIZE_X - 1) / GROUP_SIZE_X;
    const uint32_t groupsY = (volumeSizeY + GROUP_SIZE_Y - 1) / GROUP_SIZE_Y;
    pCommandList->dispatch(groupsX, groupsY, volumeSizeZ);

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

bool RhiLightShaftVolumePass::ConsumeHistoryReset()
{
    const bool reset = historyReset;
    historyReset = false;
    return reset;
}

}
