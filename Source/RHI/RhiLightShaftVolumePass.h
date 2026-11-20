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

// Independent implementation of a frustum froxel lighting/prefix pass and its history
// resources; no reference renderer source is copied.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include <nvrhi/vulkan.h>

#include "../Common.h"

namespace qray
{

namespace rhi
{
class RhiFrameContext;
class RhiTextureTable;
}

class RhiLightShaftVolumePass final
{
public:
    using PrintFunction = std::function<void(const char *)>;

    static constexpr uint32_t MAX_LIGHT_SHAFTS = 64;
    static constexpr uint32_t VOLUME_SIZE_X = 384;
    static constexpr uint32_t VOLUME_SIZE_Y = 216;
    static constexpr uint32_t VOLUME_SIZE_Z = 96;
    static constexpr uint32_t GROUP_SIZE_X = 4;
    static constexpr uint32_t GROUP_SIZE_Y = 4;
    static constexpr uint32_t LIGHT_LIST_CAPACITY = 32;
    static constexpr uint32_t LIGHT_LIST_TILE = 8;
    static constexpr uint32_t LIGHT_LIST_Z_BINS = 8;
    static constexpr uint32_t LIGHT_LIST_MAX_TILES_X =
        (VOLUME_SIZE_X + LIGHT_LIST_TILE - 1) / LIGHT_LIST_TILE;
    static constexpr uint32_t LIGHT_LIST_MAX_TILES_Y =
        (VOLUME_SIZE_Y + LIGHT_LIST_TILE - 1) / LIGHT_LIST_TILE;
    static constexpr uint32_t LIGHT_LIST_MAX_CLUSTERS =
        LIGHT_LIST_MAX_TILES_X * LIGHT_LIST_MAX_TILES_Y * LIGHT_LIST_Z_BINS;

    struct LightShaft
    {
        float position[4];
        float direction[4];
        float color[4];
        float cone[4];
        float tangentU[4];
        float tangentV[4];
        float surface[4];
        float emission[4];
        float source[4];
    };

    struct Params
    {
        uint32_t lightShaftCount;
        uint32_t lightShaftSamples;
        uint32_t volumeSizeX;
        uint32_t volumeSizeY;
        uint32_t volumeSizeZ;
        uint32_t debugMode;
        uint32_t rayBudget;
        uint32_t lightListCapacity;
        uint32_t tilesX;
        uint32_t tilesY;
        uint32_t zBins;
        uint32_t lightListClusterCount;
        float mediumSigma;
        float mediumAlbedo;
        float mediumLocalScale;
        float mediumHistoryWeight;
        float worldCenter[4];
        float worldHalfSizeInv[4];
        float sunDirection[4];
        float sunColor[4];
        float mediumEmissive[4];
        float prevCameraPosition[4];
        uint32_t historyValid;
        uint32_t sunEnabled;
        uint32_t temporalEnabled;
        uint32_t pad0;
        LightShaft lightShafts[MAX_LIGHT_SHAFTS];
    };

    RhiLightShaftVolumePass();
    ~RhiLightShaftVolumePass();

    RhiLightShaftVolumePass(const RhiLightShaftVolumePass &other) = delete;
    RhiLightShaftVolumePass(RhiLightShaftVolumePass &&other) noexcept = delete;
    RhiLightShaftVolumePass &operator=(const RhiLightShaftVolumePass &other) = delete;
    RhiLightShaftVolumePass &operator=(RhiLightShaftVolumePass &&other) noexcept = delete;

    bool Create(nvrhi::IDevice *pDevice,
                rhi::RhiFrameContext *pFrameContext,
                rhi::RhiTextureTable *pTextureTable,
                nvrhi::IBindingLayout *pLightLayout,
                const char *pShaderFolderPath,
                PrintFunction pfnPrint);

    bool IsCreated() const { return created; }

    bool Render(nvrhi::ICommandList *pCommandList,
                uint32_t frameIndex,
                nvrhi::IBuffer *pUniformBuffer,
                nvrhi::rt::IAccelStruct *pTopLevel,
                nvrhi::IBindingSet *pLightSet,
                const Params &params);

    nvrhi::ITexture *GetTexture(uint32_t frameIndex) const;
    nvrhi::ITexture *GetInjectionTexture(uint32_t frameIndex) const;
    nvrhi::ISampler *GetSampler() const { return sampler; }
    bool ConsumeHistoryReset();

private:
    struct Target
    {
        nvrhi::TextureHandle texture;
        nvrhi::TextureHandle integratedTexture;
        nvrhi::BindingSetHandle uavSet;
        nvrhi::BindingSetHandle integratedUavSet;
        nvrhi::BindingSetHandle prefixSet;
        nvrhi::BindingSetHandle paramsSet;
        nvrhi::BindingSetHandle assignSet;
        nvrhi::BindingSetHandle uniformSet;
        nvrhi::BindingSetHandle tlasSet;
        nvrhi::BufferHandle paramsBuffer;
        nvrhi::BufferHandle lightListBuffer;
        nvrhi::BufferHandle sunVisibilityBuffer;
        nvrhi::BufferHandle rayCounterBuffer;
        nvrhi::IBuffer *uniformBuffer = nullptr;
        nvrhi::rt::IAccelStruct *topLevel = nullptr;
    };

    bool CreateVolumeTargets();
    bool PrepareUniformSet(Target &target, nvrhi::IBuffer *pUniformBuffer);
    bool PrepareTlasSet(Target &target, nvrhi::rt::IAccelStruct *pTopLevel);

    nvrhi::IDevice *device = nullptr;
    rhi::RhiFrameContext *frameContext = nullptr;
    rhi::RhiTextureTable *textureTable = nullptr;
    nvrhi::BindingLayoutHandle lightLayout;
    std::string shaderFolderPath;
    PrintFunction print;
    bool created = false;

    nvrhi::ShaderHandle volumeShader;
    nvrhi::ShaderHandle prefixShader;
    nvrhi::ShaderHandle assignShader;
    nvrhi::BindingLayoutHandle tlasLayout;
    nvrhi::BindingLayoutHandle paramsLayout;
    nvrhi::BindingLayoutHandle uniformLayout;
    nvrhi::BindingLayoutHandle volumeLayout;
    nvrhi::BindingLayoutHandle prefixLayout;
    nvrhi::BindingLayoutHandle assignLayout;
    nvrhi::ComputePipelineHandle volumePipeline;
    nvrhi::ComputePipelineHandle prefixPipeline;
    nvrhi::ComputePipelineHandle assignPipeline;
    nvrhi::SamplerHandle sampler;

    uint32_t volumeSizeX = VOLUME_SIZE_X;
    uint32_t volumeSizeY = VOLUME_SIZE_Y;
    uint32_t volumeSizeZ = VOLUME_SIZE_Z;
    bool historyReset = true;

    Target targets[MAX_FRAMES_IN_FLIGHT];
};

static_assert(sizeof(RhiLightShaftVolumePass::Params) == 9392,
              "LightShaftVolumeParams_BT layout does not match the C++ mirror");

}
