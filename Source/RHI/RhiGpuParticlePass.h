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

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nvrhi/vulkan.h>
#include <qray/qray.h>

#include "../Common.h"
#include "RhiAccelStructs.h"

namespace qray
{

class SpatialFrame;

namespace rhi
{
class RhiFrameContext;
class RhiTextureTable;
}

class RhiGpuParticlePass final
{
public:
    using PrintFunction = std::function<void(const char *)>;

    static constexpr uint32_t DEFAULT_MAX_PARTICLES = 65536u;
    static constexpr uint32_t MAX_PARTICLES_LIMIT = 262144u;
    static constexpr uint32_t MAX_SPAWN_CMDS_PER_FRAME = 256u;
    static constexpr uint32_t MAX_VECTOR_FIELD_SLOTS = QR_MAX_VECTOR_FIELD_VOLUMES;
    static constexpr uint32_t VECTOR_FIELD_BRICK_RES = 32u;
    static constexpr uint32_t VECTOR_FIELD_INTERIOR_RES = 30u;
    static constexpr uint32_t VECTOR_FIELD_ATLAS_DEPTH = MAX_VECTOR_FIELD_SLOTS * VECTOR_FIELD_BRICK_RES;
    static constexpr uint32_t DRAW_BUCKET_COUNT = 5u;

    struct alignas(16) ShGpuParticle
    {
        float posAndAge[4];
        float velAndLife[4];
        float colorStart[4];
        float colorEnd[4];
        float sizeAndStretch[4];
        float physics[4];
        uint32_t meta[4];
        float spinData[4];
    };

    struct alignas(16) ShParticleBurstCmd
    {
        uint32_t spawnRange[4];
        float originAndJitter[4];
        float dirAndCone[4];
        float speedAndSize[4];
        float lifeAndScale[4];
        float physics[4];
        float colorStart[4];
        float colorEnd[4];
        float emissionSpinClusterSeed[4];
    };

    struct alignas(16) ShVectorFieldVolume
    {
        float worldToLocalRow0[4];
        float worldToLocalRow1[4];
        float worldToLocalRow2[4];
        float localToWorldRotRow0[4];
        float localToWorldRotRow1[4];
        float localToWorldRotRow2[4];
        float params0[4];
        float params1[4];
        float axisAndPad[4];
    };

    struct alignas(16) SimParams
    {
        float worldUpAndDt[4];
        uint32_t counts[4];
        float timeAndGravity[4];
        ShVectorFieldVolume volumes[MAX_VECTOR_FIELD_SLOTS];
    };

    struct DrawPush
    {
        float viewProj[16];
        uint32_t mode;
        uint32_t maxParticles;
        uint32_t renderSize[2];
    };

    struct SpawnSubmission
    {
        QrParticleBurstInfo burst = {};
        uint32_t textureIndex = 0u;
    };

    struct VolumeSubmission
    {
        QrVectorFieldVolume volume = {};
        uint32_t atlasSlot = 0u;
    };

    struct SimInputs
    {
        uint32_t frameIndex = 0u;
        nvrhi::IBuffer *pUniformBuffer = nullptr;
        nvrhi::rt::IAccelStruct *pTopLevel = nullptr;
        rhi::RhiAccelStructs::VertexDataBuffers vertexData = {};
        const SpawnSubmission *pSpawns = nullptr;
        uint32_t spawnCount = 0u;
        const VolumeSubmission *pVolumes = nullptr;
        uint32_t volumeCount = 0u;
        float dt = 0.0f;
        float time = 0.0f;
        float worldUp[3] = { 0.0f, 0.0f, 1.0f };
        bool enableCollision = true;
    };

    struct RenderInputs
    {
        uint32_t frameIndex = 0u;
        nvrhi::IFramebuffer *pFramebuffer = nullptr;
        nvrhi::ITexture *pPingLfShTexture = nullptr;
        nvrhi::ITexture *pPingLfCocgTexture = nullptr;
        nvrhi::IBuffer *pUniformBuffer = nullptr;
        nvrhi::IBuffer *pTonemappingBuffer = nullptr;
        nvrhi::rt::IAccelStruct *pTopLevel = nullptr;
        nvrhi::IBindingSet *pLightSet = nullptr;
        nvrhi::IBindingSet *pRayStatsSet = nullptr;
        const float *pViewProj = nullptr;
        uint32_t width = 0u;
        uint32_t height = 0u;
        nvrhi::Format color0Format = nvrhi::Format::UNKNOWN;
        nvrhi::Format color1Format = nvrhi::Format::UNKNOWN;
    };

    struct CreateInfo
    {
        nvrhi::IDevice *pDevice = nullptr;
        rhi::RhiFrameContext *pFrameContext = nullptr;
        rhi::RhiTextureTable *pTextureTable = nullptr;
        const char *pShaderFolderPath = nullptr;
        uint32_t maxParticles = DEFAULT_MAX_PARTICLES;
        PrintFunction pfnPrint = {};
    };

    RhiGpuParticlePass();
    ~RhiGpuParticlePass();

    RhiGpuParticlePass(const RhiGpuParticlePass &) = delete;
    RhiGpuParticlePass(RhiGpuParticlePass &&) noexcept = delete;
    RhiGpuParticlePass &operator=(const RhiGpuParticlePass &) = delete;
    RhiGpuParticlePass &operator=(RhiGpuParticlePass &&) noexcept = delete;

    bool Create(const CreateInfo &info);
    void SetBorrowedLayouts(nvrhi::IBindingLayout *pLightLayout,
                            nvrhi::IBindingLayout *pRayStatsLayout);
    bool IsCreated() const { return created; }

    static void ResampleVectorFieldToBrick(const QrVectorFieldCreateInfo &src,
                                           const SpatialFrame &spatial,
                                           std::vector<uint16_t> &outHalfRgba);
    uint32_t AllocateVectorFieldSlot(const std::vector<uint16_t> &brickHalfRgba);
    void FreeVectorFieldSlot(uint32_t slot);
    void RequestReset();

    bool Simulate(nvrhi::ICommandList *pCommandList, const SimInputs &inputs);
    bool Render(nvrhi::ICommandList *pCommandList, const RenderInputs &inputs);
    void ReleaseTargets();

    uint32_t GetMaxParticles() const { return maxParticles; }
    uint32_t GetActiveSlotLimit() const { return activeSlotLimit; }
    nvrhi::IBuffer *GetParticlePoolBuffer() const { return particlePoolBuffer; }

private:
    struct FrameResources
    {
        nvrhi::BufferHandle spawnParamsBuffer;
        nvrhi::BufferHandle spawnCmdBuffer;
        nvrhi::BufferHandle simParamsBuffer;
        nvrhi::BufferHandle indirectArgsBuffer;
        nvrhi::BufferHandle renderIndicesBuffer;
        nvrhi::BindingSetHandle spawnSet;
        nvrhi::BindingSetHandle simSet;
        nvrhi::BindingSetHandle simUniformSet;
        nvrhi::BindingSetHandle simTlasSet;
        nvrhi::BindingSetHandle simVertexDataSet;
        nvrhi::BindingSetHandle drawSet;
        nvrhi::BindingSetHandle drawUniformSet;
        nvrhi::BindingSetHandle drawFramebuffersSet;
        nvrhi::BindingSetHandle drawTlasSet;
        nvrhi::BindingSetHandle drawTonemappingSet;
        nvrhi::IBuffer *simUniformRef = nullptr;
        nvrhi::rt::IAccelStruct *simTlasRef = nullptr;
        nvrhi::IBuffer *vertexDataRefs[8] = {};
        nvrhi::IBuffer *drawUniformRef = nullptr;
        nvrhi::rt::IAccelStruct *drawTlasRef = nullptr;
        nvrhi::IBuffer *drawTonemappingRef = nullptr;
        nvrhi::ITexture *pingLfShRef = nullptr;
        nvrhi::ITexture *pingLfCocgRef = nullptr;
    };

    bool LoadShaders();
    bool CreateDrawLayouts();
    bool CreateLayouts();
    bool CreateSharedBuffers();
    bool CreatePipelines(nvrhi::Format color0Format, nvrhi::Format color1Format);
    void RetireFrameResources(FrameResources &slot);
    bool EnsureFrameBuffers(FrameResources &slot, uint32_t frameIndex);
    bool EnsureSimBindingSets(FrameResources &slot, const SimInputs &inputs);
    bool EnsureDrawUniformAndTlasSets(FrameResources &slot, const RenderInputs &inputs);
    bool EnsureDrawTonemappingSet(FrameResources &slot, const RenderInputs &inputs);
    bool HasValidRenderInputs(const RenderInputs &inputs) const;
    bool EnsureDrawBindingSets(FrameResources &slot, const RenderInputs &inputs);
    void BuildSpawnCommands(const SpawnSubmission *pSpawns, uint32_t spawnCount,
                            std::vector<ShParticleBurstCmd> &outCmds, uint32_t &outTotalSpawn);
    void BuildSimParams(const SimInputs &inputs, uint32_t activeLimit, SimParams &outParams) const;
    void UploadDirtyAtlas(nvrhi::ICommandList *pCommandList);

    nvrhi::IDevice *device = nullptr;
    rhi::RhiFrameContext *frameContext = nullptr;
    rhi::RhiTextureTable *textureTable = nullptr;
    nvrhi::BindingLayoutHandle borrowedLightLayout;
    nvrhi::BindingLayoutHandle borrowedRayStatsLayout;
    std::string shaderFolderPath;
    PrintFunction print;
    uint32_t maxParticles = DEFAULT_MAX_PARTICLES;
    bool created = false;

    nvrhi::ShaderHandle spawnShader;
    nvrhi::ShaderHandle simShader;
    nvrhi::ShaderHandle drawVertShader;
    nvrhi::ShaderHandle drawFragShader;

    nvrhi::BindingLayoutHandle spawnLayout;
    nvrhi::BindingLayoutHandle simLayout;
    nvrhi::BindingLayoutHandle simUniformLayout;
    nvrhi::BindingLayoutHandle simTlasLayout;
    nvrhi::BindingLayoutHandle simVertexDataLayout;
    nvrhi::BindingLayoutHandle drawSetLayout;
    nvrhi::BindingLayoutHandle drawUniformLayout;
    nvrhi::BindingLayoutHandle drawFramebuffersLayout;
    nvrhi::BindingLayoutHandle drawTlasLayout;
    nvrhi::BindingLayoutHandle drawTonemappingLayout;

    nvrhi::ComputePipelineHandle spawnPipeline;
    nvrhi::ComputePipelineHandle simPipeline;
    nvrhi::GraphicsPipelineHandle drawPipelines[DRAW_BUCKET_COUNT];
    nvrhi::Format pipelineColor0Format = nvrhi::Format::UNKNOWN;
    nvrhi::Format pipelineColor1Format = nvrhi::Format::UNKNOWN;

    nvrhi::BufferHandle particlePoolBuffer;
    nvrhi::TextureHandle vectorFieldAtlas;
    nvrhi::SamplerHandle vectorFieldSampler;
    nvrhi::TextureHandle dummyZeroTexture;
    nvrhi::SamplerHandle dummySampler;
    nvrhi::BufferHandle dummyTonemappingBuffer;

    std::array<bool, MAX_VECTOR_FIELD_SLOTS> slotInUse = {};
    std::vector<uint16_t> atlasCpu;
    bool atlasDirty = true;

    FrameResources frameSlots[MAX_FRAMES_IN_FLIGHT];
    uint32_t ringWriteCursor = 0u;
    uint32_t activeSlotLimit = 0u;
    float remainingMaxLifetime = 0.0f;
    uint32_t frameSeedCounter = 1u;
    bool pendingPoolReset = true;
};

static_assert(sizeof(RhiGpuParticlePass::ShGpuParticle) == 128, "ShGpuParticle must be 128 bytes");
static_assert(sizeof(RhiGpuParticlePass::ShParticleBurstCmd) == 144, "ShParticleBurstCmd must be 144 bytes");
static_assert(sizeof(RhiGpuParticlePass::ShVectorFieldVolume) == 144, "ShVectorFieldVolume must be 144 bytes");
static_assert(sizeof(RhiGpuParticlePass::SimParams) == 48 + 16 * 144, "SimParams layout mismatch");
static_assert(sizeof(RhiGpuParticlePass::DrawPush) == 80, "DrawPush must be 80 bytes");

}
