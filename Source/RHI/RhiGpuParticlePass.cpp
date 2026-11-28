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

#include "RhiGpuParticlePass.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include "../Generated/ShaderCommonC.h"
#include "../Generated/ShaderCommonCFramebuf.h"
#include "../SpatialFrame.h"
#include "RhiFrameContext.h"
#include "RhiPipeline.h"
#include "RhiResources.h"
#include "RhiTextureTable.h"

namespace qray
{

namespace
{

constexpr const char *SPAWN_SHADER_NAME = "CmParticleSpawn.comp.spv";
constexpr const char *SIM_SHADER_NAME = "CmParticleSim.comp.spv";
constexpr const char *DRAW_VERT_SHADER_NAME = "RsGpuParticle.vert.spv";
constexpr const char *DRAW_FRAG_SHADER_NAME = "RsGpuParticle.frag.spv";

constexpr uint32_t kInitialIndirectArgs[RhiGpuParticlePass::DRAW_BUCKET_COUNT * 4u] = {
    6u,  0u, 0u, 0u,
    6u,  0u, 0u, 0u,
    6u,  0u, 0u, 0u,
    6u,  0u, 0u, 0u,
    24u, 0u, 0u, 0u,
};

constexpr uint32_t kVertexDataBindings[8] = {
    BINDING_VERTEX_BUFFER_STATIC,
    BINDING_VERTEX_BUFFER_DYNAMIC,
    BINDING_INDEX_BUFFER_STATIC,
    BINDING_INDEX_BUFFER_DYNAMIC,
    BINDING_GEOMETRY_INSTANCES,
    BINDING_GEOMETRY_INSTANCES_MATCH_PREV,
    BINDING_PREV_POSITIONS_BUFFER_DYNAMIC,
    BINDING_PREV_INDEX_BUFFER_DYNAMIC,
};

void LogMsg(const RhiGpuParticlePass::PrintFunction &print, const std::string &msg)
{
    if (print)
    {
        print(msg.c_str());
    }
}

uint16_t FloatToHalfBits(float value)
{
    if (!std::isfinite(value))
    {
        return 0u;
    }

    const float clamped = std::clamp(value, -65504.0f, 65504.0f);
    const uint32_t bits = std::bit_cast<uint32_t>(clamped);
    const uint32_t sign = (bits >> 16u) & 0x8000u;
    const int32_t exp = int32_t((bits >> 23u) & 0xFFu) - 127 + 15;
    uint32_t mant = bits & 0x7FFFFFu;

    if (exp <= 0)
    {
        if (exp < -10)
        {
            return static_cast<uint16_t>(sign);
        }
        mant = (mant | 0x800000u) >> (14 - exp);
        return static_cast<uint16_t>(sign | mant);
    }

    if (exp >= 31)
    {
        return static_cast<uint16_t>(sign | 0x7BFFu);
    }

    return static_cast<uint16_t>(sign | (uint32_t(exp) << 10u) | (mant >> 13u));
}

void InvertAffine3x4(const QrTransform &m, float outInv[3][4], float outRot[3][3], float outCenter[3])
{
    const float a00 = m.matrix[0][0], a01 = m.matrix[0][1], a02 = m.matrix[0][2];
    const float a10 = m.matrix[1][0], a11 = m.matrix[1][1], a12 = m.matrix[1][2];
    const float a20 = m.matrix[2][0], a21 = m.matrix[2][1], a22 = m.matrix[2][2];

    outCenter[0] = m.matrix[0][3];
    outCenter[1] = m.matrix[1][3];
    outCenter[2] = m.matrix[2][3];

    const float c00 = a11 * a22 - a12 * a21;
    const float c01 = a02 * a21 - a01 * a22;
    const float c02 = a01 * a12 - a02 * a11;
    const float c10 = a12 * a20 - a10 * a22;
    const float c11 = a00 * a22 - a02 * a20;
    const float c12 = a02 * a10 - a00 * a12;
    const float c20 = a10 * a21 - a11 * a20;
    const float c21 = a01 * a20 - a00 * a21;
    const float c22 = a00 * a11 - a01 * a10;
    const float det = a00 * c00 + a01 * c10 + a02 * c20;

    if (std::abs(det) < 1e-12f)
    {
        std::memset(outInv, 0, sizeof(float) * 12);
        std::memset(outRot, 0, sizeof(float) * 9);
        outInv[0][0] = outInv[1][1] = outInv[2][2] = 1.0f;
        outRot[0][0] = outRot[1][1] = outRot[2][2] = 1.0f;
        return;
    }

    const float invDet = 1.0f / det;
    const float inv[3][3] = {
        { c00 * invDet, c01 * invDet, c02 * invDet },
        { c10 * invDet, c11 * invDet, c12 * invDet },
        { c20 * invDet, c21 * invDet, c22 * invDet },
    };

    for (int r = 0; r < 3; r++)
    {
        outInv[r][0] = inv[r][0];
        outInv[r][1] = inv[r][1];
        outInv[r][2] = inv[r][2];
        outInv[r][3] = -(inv[r][0] * outCenter[0] + inv[r][1] * outCenter[1] + inv[r][2] * outCenter[2]);
    }

    for (int c = 0; c < 3; c++)
    {
        const float len = std::sqrt(m.matrix[0][c] * m.matrix[0][c] +
                                    m.matrix[1][c] * m.matrix[1][c] +
                                    m.matrix[2][c] * m.matrix[2][c]);
        const float s = len > 1e-6f ? (1.0f / len) : 1.0f;
        outRot[0][c] = m.matrix[0][c] * s;
        outRot[1][c] = m.matrix[1][c] * s;
        outRot[2][c] = m.matrix[2][c] * s;
    }
}

void SampleGridTrilinear(const QrVectorFieldCreateInfo &src, float u, float v, float w, float outVec[3])
{
    const float fx = std::clamp(u, 0.0f, 1.0f) * float(src.width - 1u);
    const float fy = std::clamp(v, 0.0f, 1.0f) * float(src.height - 1u);
    const float fz = std::clamp(w, 0.0f, 1.0f) * float(src.depth - 1u);

    const uint32_t x0 = static_cast<uint32_t>(std::floor(fx));
    const uint32_t y0 = static_cast<uint32_t>(std::floor(fy));
    const uint32_t z0 = static_cast<uint32_t>(std::floor(fz));
    const uint32_t x1 = std::min(x0 + 1u, src.width - 1u);
    const uint32_t y1 = std::min(y0 + 1u, src.height - 1u);
    const uint32_t z1 = std::min(z0 + 1u, src.depth - 1u);

    const float tx = fx - float(x0);
    const float ty = fy - float(y0);
    const float tz = fz - float(z0);

    const auto at = [&](uint32_t x, uint32_t y, uint32_t z, uint32_t c)
    {
        const size_t idx = (size_t(z) * src.height + y) * src.width + x;
        return src.pVectorsXYZ[idx * 3u + c];
    };

    for (uint32_t c = 0; c < 3u; c++)
    {
        const float c00 = at(x0, y0, z0, c) * (1.0f - tx) + at(x1, y0, z0, c) * tx;
        const float c10 = at(x0, y1, z0, c) * (1.0f - tx) + at(x1, y1, z0, c) * tx;
        const float c01 = at(x0, y0, z1, c) * (1.0f - tx) + at(x1, y0, z1, c) * tx;
        const float c11 = at(x0, y1, z1, c) * (1.0f - tx) + at(x1, y1, z1, c) * tx;
        const float c0 = c00 * (1.0f - ty) + c10 * ty;
        const float c1 = c01 * (1.0f - ty) + c11 * ty;
        outVec[c] = c0 * (1.0f - tz) + c1 * tz;
    }
}

bool VertexDataReady(const rhi::RhiAccelStructs::VertexDataBuffers &vb)
{
    return vb.staticVertices != nullptr && vb.dynamicVertices != nullptr &&
           vb.staticIndices != nullptr && vb.dynamicIndices != nullptr &&
           vb.geometryInstances != nullptr && vb.geometryInstancesMatchPrev != nullptr &&
           vb.previousDynamicVertices != nullptr && vb.previousDynamicIndices != nullptr;
}

bool VertexDataMatches(const rhi::RhiAccelStructs::VertexDataBuffers &vb, nvrhi::IBuffer *const refs[8])
{
    return refs[0] == vb.staticVertices && refs[1] == vb.dynamicVertices &&
           refs[2] == vb.staticIndices && refs[3] == vb.dynamicIndices &&
           refs[4] == vb.geometryInstances && refs[5] == vb.geometryInstancesMatchPrev &&
           refs[6] == vb.previousDynamicVertices && refs[7] == vb.previousDynamicIndices;
}

nvrhi::BufferHandle CreateStructuredBuffer(nvrhi::IDevice *pDevice, uint64_t byteSize, uint32_t stride,
                                           bool uav, bool indirectArgs, nvrhi::ResourceStates state,
                                           const std::string &name)
{
    nvrhi::BufferDesc desc;
    desc.byteSize = byteSize;
    desc.structStride = stride;
    desc.canHaveUAVs = uav;
    desc.isDrawIndirectArgs = indirectArgs;
    desc.keepInitialState = true;
    desc.initialState = state;
    desc.debugName = name;
    return rhi::createBuffer(pDevice, desc, name);
}

nvrhi::BufferHandle CreateConstantBuffer(nvrhi::IDevice *pDevice, uint64_t byteSize, const std::string &name)
{
    nvrhi::BufferDesc desc;
    desc.byteSize = byteSize;
    desc.isConstantBuffer = true;
    desc.isVolatile = false;
    desc.keepInitialState = true;
    desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    desc.debugName = name;
    return rhi::createBuffer(pDevice, desc, name);
}

void ConfigureModeBlendAndDepth(uint32_t bucket, nvrhi::GraphicsPipelineDesc &desc)
{
    nvrhi::BlendState::RenderTarget rt0;
    rt0.setBlendOp(nvrhi::BlendOp::Add).setBlendOpAlpha(nvrhi::BlendOp::Add).setColorWriteMask(nvrhi::ColorMask::All);

    if (bucket == 0u || bucket == 2u)
    {
        rt0.setBlendEnable(true)
            .setSrcBlend(nvrhi::BlendFactor::One)
            .setDestBlend(nvrhi::BlendFactor::One)
            .setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
            .setDestBlendAlpha(nvrhi::BlendFactor::One);
        desc.renderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(false);
    }
    else if (bucket == 1u)
    {
        rt0.setBlendEnable(true)
            .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
            .setDestBlend(nvrhi::BlendFactor::OneMinusSrcAlpha)
            .setSrcBlendAlpha(nvrhi::BlendFactor::One)
            .setDestBlendAlpha(nvrhi::BlendFactor::OneMinusSrcAlpha);
        desc.renderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(false);
    }
    else
    {
        rt0.setBlendEnable(false);
        desc.renderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(true);
    }

    nvrhi::BlendState::RenderTarget rt1;
    rt1.setColorWriteMask(nvrhi::ColorMask::All);
    if (bucket <= 2u)
    {
        rt1.setBlendEnable(true)
            .setSrcBlend(nvrhi::BlendFactor::Zero)
            .setDestBlend(nvrhi::BlendFactor::One)
            .setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
            .setDestBlendAlpha(nvrhi::BlendFactor::One);
    }
    else
    {
        rt1.setBlendEnable(false);
    }

    desc.renderState.depthStencilState.setDepthFunc(nvrhi::ComparisonFunc::LessOrEqual).setStencilEnable(false);
    desc.renderState.blendState.setRenderTarget(0, rt0);
    desc.renderState.blendState.setRenderTarget(1, rt1);
}

} // namespace

RhiGpuParticlePass::RhiGpuParticlePass() = default;
RhiGpuParticlePass::~RhiGpuParticlePass()
{
    ReleaseTargets();
}

void RhiGpuParticlePass::ResampleVectorFieldToBrick(const QrVectorFieldCreateInfo &src,
                                                    const SpatialFrame &spatial,
                                                    std::vector<uint16_t> &outHalfRgba)
{
    constexpr uint32_t res = VECTOR_FIELD_BRICK_RES;
    outHalfRgba.assign(size_t(res) * res * res * 4u, 0u);
    if (src.width == 0u || src.height == 0u || src.depth == 0u || src.pVectorsXYZ == nullptr)
    {
        return;
    }

    for (uint32_t z = 0u; z < res; z++)
    {
        const float w = std::clamp((float(z) - 0.5f) / float(VECTOR_FIELD_INTERIOR_RES), 0.0f, 1.0f);
        for (uint32_t y = 0u; y < res; y++)
        {
            const float v = std::clamp((float(y) - 0.5f) / float(VECTOR_FIELD_INTERIOR_RES), 0.0f, 1.0f);
            for (uint32_t x = 0u; x < res; x++)
            {
                const float u = std::clamp((float(x) - 0.5f) / float(VECTOR_FIELD_INTERIOR_RES), 0.0f, 1.0f);
                float hostVec[3] = {};
                float sceneVec[3] = {};
                SampleGridTrilinear(src, u, v, w, hostVec);
                spatial.VectorFieldVelocity(hostVec, sceneVec);

                const size_t dst = ((size_t(z) * res + y) * res + x) * 4u;
                outHalfRgba[dst + 0u] = FloatToHalfBits(sceneVec[0]);
                outHalfRgba[dst + 1u] = FloatToHalfBits(sceneVec[1]);
                outHalfRgba[dst + 2u] = FloatToHalfBits(sceneVec[2]);
                outHalfRgba[dst + 3u] = FloatToHalfBits(1.0f);
            }
        }
    }
}

bool RhiGpuParticlePass::Create(const CreateInfo &info)
{
    ReleaseTargets();
    device = info.pDevice;
    frameContext = info.pFrameContext;
    textureTable = info.pTextureTable;
    shaderFolderPath = info.pShaderFolderPath != nullptr ? info.pShaderFolderPath : "";
    print = info.pfnPrint;

    const uint32_t req = info.maxParticles != 0u ? info.maxParticles : DEFAULT_MAX_PARTICLES;
    maxParticles = std::clamp(req, 1024u, MAX_PARTICLES_LIMIT);

    if (device == nullptr || frameContext == nullptr || textureTable == nullptr ||
        !textureTable->IsCreated() || shaderFolderPath.empty())
    {
        return false;
    }

    if (!LoadShaders() || !CreateLayouts() || !CreateSharedBuffers())
    {
        ReleaseTargets();
        return false;
    }

    created = true;
    return true;
}

void RhiGpuParticlePass::SetBorrowedLayouts(nvrhi::IBindingLayout *pLightLayout,
                                              nvrhi::IBindingLayout *pRayStatsLayout)
{
    borrowedLightLayout = pLightLayout;
    borrowedRayStatsLayout = pRayStatsLayout;
}

bool RhiGpuParticlePass::LoadShaders()
{
    spawnShader = rhi::loadShader(device, shaderFolderPath + SPAWN_SHADER_NAME,
                                  nvrhi::ShaderType::Compute, "RhiGpuParticle spawn");
    simShader = rhi::loadShader(device, shaderFolderPath + SIM_SHADER_NAME,
                                nvrhi::ShaderType::Compute, "RhiGpuParticle sim");
    drawVertShader = rhi::loadShader(device, shaderFolderPath + DRAW_VERT_SHADER_NAME,
                                     nvrhi::ShaderType::Vertex, "RhiGpuParticle vert");
    drawFragShader = rhi::loadShader(device, shaderFolderPath + DRAW_FRAG_SHADER_NAME,
                                     nvrhi::ShaderType::Pixel, "RhiGpuParticle frag");
    if (spawnShader == nullptr || simShader == nullptr ||
        drawVertShader == nullptr || drawFragShader == nullptr)
    {
        LogMsg(print, "Warning: RHI: failed to load GPU particle shaders");
        return false;
    }
    return true;
}

bool RhiGpuParticlePass::CreateDrawLayouts()
{
    const auto zeroOffsets = nvrhi::VulkanBindingOffsets()
        .setConstantBufferOffset(0)
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);

    {
        nvrhi::BindingLayoutDesc d;
        d.visibility = nvrhi::ShaderType::AllGraphics;
        d.setBindingOffsets(zeroOffsets);
        d.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
        d.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
        d.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(DrawPush)));
        drawSetLayout = device->createBindingLayout(d);
    }
    {
        nvrhi::BindingLayoutDesc d;
        d.visibility = nvrhi::ShaderType::AllGraphics;
        d.setBindingOffsets(nvrhi::VulkanBindingOffsets().setConstantBufferOffset(0));
        d.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(BINDING_GLOBAL_UNIFORM));
        drawUniformLayout = device->createBindingLayout(d);
    }
    {
        nvrhi::BindingLayoutDesc d;
        d.visibility = nvrhi::ShaderType::Pixel;
        d.setBindingOffsets(nvrhi::VulkanBindingOffsets()
                                .setShaderResourceOffset(ShFramebuffers_Count)
                                .setSamplerOffset(ShFramebuffers_Count * 2u));
        d.addItem(nvrhi::BindingLayoutItem::Texture_SRV(FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_S_H));
        d.addItem(nvrhi::BindingLayoutItem::Texture_SRV(FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_C_O_C_G));
        d.addItem(nvrhi::BindingLayoutItem::Sampler(FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_S_H));
        d.addItem(nvrhi::BindingLayoutItem::Sampler(FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_C_O_C_G));
        drawFramebuffersLayout = device->createBindingLayout(d);
    }
    {
        nvrhi::BindingLayoutDesc d;
        d.visibility = nvrhi::ShaderType::Vertex;
        d.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
        drawTlasLayout = device->createBindingLayout(d);
    }
    {
        nvrhi::BindingLayoutDesc d;
        d.visibility = nvrhi::ShaderType::Pixel;
        d.setBindingOffsets(nvrhi::VulkanBindingOffsets().setShaderResourceOffset(0));
        d.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
        drawTonemappingLayout = device->createBindingLayout(d);
    }

    return drawSetLayout != nullptr && drawUniformLayout != nullptr &&
           drawFramebuffersLayout != nullptr && drawTlasLayout != nullptr &&
           drawTonemappingLayout != nullptr;
}

bool RhiGpuParticlePass::CreateLayouts()
{
    const auto zeroOffsets = nvrhi::VulkanBindingOffsets()
        .setConstantBufferOffset(0)
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);

    {
        nvrhi::BindingLayoutDesc d;
        d.visibility = nvrhi::ShaderType::Compute;
        d.setBindingOffsets(zeroOffsets);
        d.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
        d.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
        d.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2));
        spawnLayout = device->createBindingLayout(d);
    }
    {
        nvrhi::BindingLayoutDesc d;
        d.visibility = nvrhi::ShaderType::Compute;
        d.setBindingOffsets(zeroOffsets);
        d.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
        d.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));
        d.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2));
        d.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3));
        d.addItem(nvrhi::BindingLayoutItem::Texture_SRV(4));
        d.addItem(nvrhi::BindingLayoutItem::Sampler(5));
        simLayout = device->createBindingLayout(d);
    }
    {
        nvrhi::BindingLayoutDesc d;
        d.visibility = nvrhi::ShaderType::Compute;
        d.setBindingOffsets(nvrhi::VulkanBindingOffsets().setConstantBufferOffset(0));
        d.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(BINDING_GLOBAL_UNIFORM));
        simUniformLayout = device->createBindingLayout(d);
    }
    {
        nvrhi::BindingLayoutDesc d;
        d.visibility = nvrhi::ShaderType::Compute;
        d.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
        simTlasLayout = device->createBindingLayout(d);
    }
    {
        nvrhi::BindingLayoutDesc d;
        d.visibility = nvrhi::ShaderType::Compute;
        d.setBindingOffsets(nvrhi::VulkanBindingOffsets().setShaderResourceOffset(0));
        for (uint32_t binding : kVertexDataBindings)
        {
            d.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(binding));
        }
        simVertexDataLayout = device->createBindingLayout(d);
    }

    if (spawnLayout == nullptr || simLayout == nullptr || simUniformLayout == nullptr ||
        simTlasLayout == nullptr || simVertexDataLayout == nullptr || !CreateDrawLayouts())
    {
        return false;
    }

    {
        nvrhi::ComputePipelineDesc p;
        p.CS = spawnShader;
        p.addBindingLayout(spawnLayout);
        spawnPipeline = rhi::createComputePipeline(device, p, "RhiGpuParticle spawn pipeline");
    }
    {
        nvrhi::ComputePipelineDesc p;
        p.CS = simShader;
        p.addBindingLayout(simLayout);
        p.addBindingLayout(simUniformLayout);
        p.addBindingLayout(simTlasLayout);
        p.addBindingLayout(simVertexDataLayout);
        simPipeline = rhi::createComputePipeline(device, p, "RhiGpuParticle sim pipeline");
    }
    return spawnPipeline != nullptr && simPipeline != nullptr;
}

bool RhiGpuParticlePass::CreateSharedBuffers()
{
    particlePoolBuffer = CreateStructuredBuffer(
        device, uint64_t(maxParticles) * sizeof(ShGpuParticle), sizeof(ShGpuParticle),
        true, false, nvrhi::ResourceStates::UnorderedAccess, "RhiGpuParticle pool");

    nvrhi::TextureDesc atlasDesc;
    atlasDesc.width = VECTOR_FIELD_BRICK_RES;
    atlasDesc.height = VECTOR_FIELD_BRICK_RES;
    atlasDesc.depth = VECTOR_FIELD_ATLAS_DEPTH;
    atlasDesc.dimension = nvrhi::TextureDimension::Texture3D;
    atlasDesc.format = nvrhi::Format::RGBA16_FLOAT;
    atlasDesc.isShaderResource = true;
    atlasDesc.keepInitialState = true;
    atlasDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    vectorFieldAtlas = rhi::createTexture(device, atlasDesc, "RhiGpuParticle vectorFieldAtlas");

    vectorFieldSampler = rhi::createSampler(
        device,
        nvrhi::SamplerDesc().setMinFilter(true).setMagFilter(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp),
        "RhiGpuParticle vectorFieldSampler");

    nvrhi::TextureDesc dummyDesc;
    dummyDesc.width = 1u;
    dummyDesc.height = 1u;
    dummyDesc.dimension = nvrhi::TextureDimension::Texture2D;
    dummyDesc.format = nvrhi::Format::RGBA16_FLOAT;
    dummyDesc.isShaderResource = true;
    dummyDesc.keepInitialState = true;
    dummyDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    dummyZeroTexture = rhi::createTexture(device, dummyDesc, "RhiGpuParticle dummyZero2D");
    dummySampler = rhi::createSampler(device, nvrhi::SamplerDesc(), "RhiGpuParticle dummySampler");
    dummyTonemappingBuffer = CreateStructuredBuffer(
        device, sizeof(ShTonemapping), sizeof(uint32_t), false, false,
        nvrhi::ResourceStates::ShaderResource, "RhiGpuParticle dummyTonemapping");

    atlasCpu.assign(size_t(VECTOR_FIELD_BRICK_RES) * VECTOR_FIELD_BRICK_RES * VECTOR_FIELD_ATLAS_DEPTH * 4u, 0u);
    atlasDirty = true;
    pendingPoolReset = true;
    slotInUse.fill(false);

    return particlePoolBuffer != nullptr && vectorFieldAtlas != nullptr &&
           vectorFieldSampler != nullptr && dummyZeroTexture != nullptr && dummySampler != nullptr &&
           dummyTonemappingBuffer != nullptr;
}

uint32_t RhiGpuParticlePass::AllocateVectorFieldSlot(const std::vector<uint16_t> &brickHalfRgba)
{
    const size_t expected = size_t(VECTOR_FIELD_BRICK_RES) * VECTOR_FIELD_BRICK_RES * VECTOR_FIELD_BRICK_RES * 4u;
    if (brickHalfRgba.size() != expected || atlasCpu.size() < expected * MAX_VECTOR_FIELD_SLOTS)
    {
        return UINT32_MAX;
    }

    for (uint32_t slot = 0u; slot < MAX_VECTOR_FIELD_SLOTS; slot++)
    {
        if (!slotInUse[slot])
        {
            slotInUse[slot] = true;
            std::memcpy(atlasCpu.data() + size_t(slot) * expected, brickHalfRgba.data(), expected * sizeof(uint16_t));
            atlasDirty = true;
            return slot;
        }
    }
    return UINT32_MAX;
}

void RhiGpuParticlePass::FreeVectorFieldSlot(uint32_t slot)
{
    if (slot < MAX_VECTOR_FIELD_SLOTS)
    {
        slotInUse[slot] = false;
    }
}

void RhiGpuParticlePass::RequestReset()
{
    pendingPoolReset = true;
    activeSlotLimit = 0u;
    ringWriteCursor = 0u;
    remainingMaxLifetime = 0.0f;
}

bool RhiGpuParticlePass::EnsureFrameBuffers(FrameResources &slot, uint32_t frameIndex)
{
    if (slot.spawnParamsBuffer != nullptr && slot.spawnCmdBuffer != nullptr &&
        slot.simParamsBuffer != nullptr && slot.indirectArgsBuffer != nullptr &&
        slot.renderIndicesBuffer != nullptr)
    {
        return true;
    }

    const std::string tag = " frame " + std::to_string(frameIndex);
    slot.spawnParamsBuffer = CreateConstantBuffer(device, 16u, "RhiGpuParticle spawnParams" + tag);
    slot.spawnCmdBuffer = CreateStructuredBuffer(
        device, uint64_t(MAX_SPAWN_CMDS_PER_FRAME) * sizeof(ShParticleBurstCmd),
        sizeof(ShParticleBurstCmd), false, false, nvrhi::ResourceStates::ShaderResource,
        "RhiGpuParticle spawnCmds" + tag);
    slot.simParamsBuffer = CreateConstantBuffer(device, sizeof(SimParams), "RhiGpuParticle simParams" + tag);
    slot.indirectArgsBuffer = CreateStructuredBuffer(
        device, sizeof(kInitialIndirectArgs), sizeof(uint32_t), true, true,
        nvrhi::ResourceStates::IndirectArgument, "RhiGpuParticle indirectArgs" + tag);
    slot.renderIndicesBuffer = CreateStructuredBuffer(
        device, uint64_t(maxParticles) * DRAW_BUCKET_COUNT * sizeof(uint32_t), sizeof(uint32_t), true, false,
        nvrhi::ResourceStates::ShaderResource, "RhiGpuParticle renderIndices" + tag);

    if (slot.spawnParamsBuffer == nullptr || slot.spawnCmdBuffer == nullptr ||
        slot.simParamsBuffer == nullptr || slot.indirectArgsBuffer == nullptr ||
        slot.renderIndicesBuffer == nullptr)
    {
        return false;
    }

    {
        nvrhi::BindingSetDesc d;
        d.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, slot.spawnParamsBuffer));
        d.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, slot.spawnCmdBuffer));
        d.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(2, particlePoolBuffer));
        slot.spawnSet = device->createBindingSet(d, spawnLayout);
    }
    {
        nvrhi::BindingSetDesc d;
        d.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, slot.simParamsBuffer));
        d.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, particlePoolBuffer));
        d.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(2, slot.indirectArgsBuffer));
        d.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(3, slot.renderIndicesBuffer));
        d.addItem(nvrhi::BindingSetItem::Texture_SRV(4, vectorFieldAtlas));
        d.addItem(nvrhi::BindingSetItem::Sampler(5, vectorFieldSampler));
        slot.simSet = device->createBindingSet(d, simLayout);
    }
    {
        nvrhi::BindingSetDesc d;
        d.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, particlePoolBuffer));
        d.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, slot.renderIndicesBuffer));
        d.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(DrawPush)));
        slot.drawSet = device->createBindingSet(d, drawSetLayout);
    }

    return slot.spawnSet != nullptr && slot.simSet != nullptr && slot.drawSet != nullptr;
}

bool RhiGpuParticlePass::EnsureSimBindingSets(FrameResources &slot, const SimInputs &inputs)
{
    if (slot.simUniformSet == nullptr || slot.simUniformRef != inputs.pUniformBuffer)
    {
        if (slot.simUniformSet != nullptr && frameContext != nullptr)
        {
            frameContext->Retire(slot.simUniformSet);
        }
        nvrhi::BindingSetDesc d;
        d.addItem(nvrhi::BindingSetItem::ConstantBuffer(BINDING_GLOBAL_UNIFORM, inputs.pUniformBuffer));
        slot.simUniformSet = device->createBindingSet(d, simUniformLayout);
        slot.simUniformRef = inputs.pUniformBuffer;
    }

    if (slot.simTlasSet == nullptr || slot.simTlasRef != inputs.pTopLevel)
    {
        if (slot.simTlasSet != nullptr && frameContext != nullptr)
        {
            frameContext->Retire(slot.simTlasSet);
        }
        nvrhi::BindingSetDesc d;
        d.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, inputs.pTopLevel));
        slot.simTlasSet = device->createBindingSet(d, simTlasLayout);
        slot.simTlasRef = inputs.pTopLevel;
    }

    if (slot.simVertexDataSet == nullptr || !VertexDataMatches(inputs.vertexData, slot.vertexDataRefs))
    {
        if (slot.simVertexDataSet != nullptr && frameContext != nullptr)
        {
            frameContext->Retire(slot.simVertexDataSet);
        }
        nvrhi::BindingSetDesc d;
        d.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(BINDING_VERTEX_BUFFER_STATIC, inputs.vertexData.staticVertices));
        d.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(BINDING_VERTEX_BUFFER_DYNAMIC, inputs.vertexData.dynamicVertices));
        d.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(BINDING_INDEX_BUFFER_STATIC, inputs.vertexData.staticIndices));
        d.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(BINDING_INDEX_BUFFER_DYNAMIC, inputs.vertexData.dynamicIndices));
        d.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(BINDING_GEOMETRY_INSTANCES, inputs.vertexData.geometryInstances));
        d.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(BINDING_GEOMETRY_INSTANCES_MATCH_PREV, inputs.vertexData.geometryInstancesMatchPrev));
        d.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(BINDING_PREV_POSITIONS_BUFFER_DYNAMIC, inputs.vertexData.previousDynamicVertices));
        d.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(BINDING_PREV_INDEX_BUFFER_DYNAMIC, inputs.vertexData.previousDynamicIndices));
        slot.simVertexDataSet = device->createBindingSet(d, simVertexDataLayout);
        slot.vertexDataRefs[0] = inputs.vertexData.staticVertices;
        slot.vertexDataRefs[1] = inputs.vertexData.dynamicVertices;
        slot.vertexDataRefs[2] = inputs.vertexData.staticIndices;
        slot.vertexDataRefs[3] = inputs.vertexData.dynamicIndices;
        slot.vertexDataRefs[4] = inputs.vertexData.geometryInstances;
        slot.vertexDataRefs[5] = inputs.vertexData.geometryInstancesMatchPrev;
        slot.vertexDataRefs[6] = inputs.vertexData.previousDynamicVertices;
        slot.vertexDataRefs[7] = inputs.vertexData.previousDynamicIndices;
    }

    return slot.simUniformSet != nullptr && slot.simTlasSet != nullptr && slot.simVertexDataSet != nullptr;
}

bool RhiGpuParticlePass::EnsureDrawUniformAndTlasSets(FrameResources &slot, const RenderInputs &inputs)
{
    if (slot.drawUniformSet == nullptr || slot.drawUniformRef != inputs.pUniformBuffer)
    {
        if (slot.drawUniformSet != nullptr && frameContext != nullptr)
        {
            frameContext->Retire(slot.drawUniformSet);
        }
        nvrhi::BindingSetDesc d;
        d.addItem(nvrhi::BindingSetItem::ConstantBuffer(BINDING_GLOBAL_UNIFORM, inputs.pUniformBuffer));
        slot.drawUniformSet = device->createBindingSet(d, drawUniformLayout);
        slot.drawUniformRef = inputs.pUniformBuffer;
    }

    if (slot.drawTlasSet == nullptr || slot.drawTlasRef != inputs.pTopLevel)
    {
        if (slot.drawTlasSet != nullptr && frameContext != nullptr)
        {
            frameContext->Retire(slot.drawTlasSet);
        }
        nvrhi::BindingSetDesc d;
        d.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, inputs.pTopLevel));
        slot.drawTlasSet = device->createBindingSet(d, drawTlasLayout);
        slot.drawTlasRef = inputs.pTopLevel;
    }

    return slot.drawUniformSet != nullptr && slot.drawTlasSet != nullptr;
}

bool RhiGpuParticlePass::EnsureDrawTonemappingSet(FrameResources &slot, const RenderInputs &inputs)
{
    nvrhi::IBuffer *tmBuf = inputs.pTonemappingBuffer != nullptr
                                ? inputs.pTonemappingBuffer
                                : dummyTonemappingBuffer.Get();
    if (slot.drawTonemappingSet == nullptr || slot.drawTonemappingRef != tmBuf)
    {
        if (slot.drawTonemappingSet != nullptr && frameContext != nullptr)
        {
            frameContext->Retire(slot.drawTonemappingSet);
        }
        nvrhi::BindingSetDesc d;
        d.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, tmBuf));
        slot.drawTonemappingSet = device->createBindingSet(d, drawTonemappingLayout);
        slot.drawTonemappingRef = tmBuf;
    }

    return slot.drawTonemappingSet != nullptr;
}

bool RhiGpuParticlePass::EnsureDrawBindingSets(FrameResources &slot, const RenderInputs &inputs)
{
    if (!EnsureDrawUniformAndTlasSets(slot, inputs) || !EnsureDrawTonemappingSet(slot, inputs))
    {
        return false;
    }

    nvrhi::ITexture *shTex = inputs.pPingLfShTexture != nullptr ? inputs.pPingLfShTexture : dummyZeroTexture.Get();
    nvrhi::ITexture *cocgTex = inputs.pPingLfCocgTexture != nullptr ? inputs.pPingLfCocgTexture : dummyZeroTexture.Get();
    if (slot.drawFramebuffersSet == nullptr || slot.pingLfShRef != shTex || slot.pingLfCocgRef != cocgTex)
    {
        if (slot.drawFramebuffersSet != nullptr && frameContext != nullptr)
        {
            frameContext->Retire(slot.drawFramebuffersSet);
        }
        nvrhi::BindingSetDesc d;
        d.addItem(nvrhi::BindingSetItem::Texture_SRV(FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_S_H, shTex));
        d.addItem(nvrhi::BindingSetItem::Texture_SRV(FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_C_O_C_G, cocgTex));
        d.addItem(nvrhi::BindingSetItem::Sampler(FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_S_H, dummySampler));
        d.addItem(nvrhi::BindingSetItem::Sampler(FB_IMAGE_INDEX_Q2_ATROUS_PING_L_F_C_O_C_G, dummySampler));
        slot.drawFramebuffersSet = device->createBindingSet(d, drawFramebuffersLayout);
        slot.pingLfShRef = shTex;
        slot.pingLfCocgRef = cocgTex;
    }

    return slot.drawFramebuffersSet != nullptr;
}

void RhiGpuParticlePass::BuildSpawnCommands(const SpawnSubmission *pSpawns, uint32_t spawnCount,
                                            std::vector<ShParticleBurstCmd> &outCmds, uint32_t &outTotalSpawn)
{
    outCmds.clear();
    outTotalSpawn = 0u;
    if (pSpawns == nullptr || spawnCount == 0u)
    {
        return;
    }

    const uint32_t cappedCmds = std::min(spawnCount, MAX_SPAWN_CMDS_PER_FRAME);
    outCmds.reserve(cappedCmds);

    for (uint32_t i = 0u; i < cappedCmds; i++)
    {
        const QrParticleBurstInfo &b = pSpawns[i].burst;
        if (b.count == 0u || outTotalSpawn >= maxParticles)
        {
            continue;
        }

        const uint32_t count = std::min(b.count, maxParticles - outTotalSpawn);
        ShParticleBurstCmd cmd = {};
        cmd.spawnRange[0] = outTotalSpawn;
        cmd.spawnRange[1] = outTotalSpawn + count;
        cmd.spawnRange[2] = (uint32_t(b.renderMode) & 0xFFu) | ((b.flags & 0xFFFFFFu) << 8u);
        cmd.spawnRange[3] = pSpawns[i].textureIndex;
        cmd.originAndJitter[0] = b.origin.data[0];
        cmd.originAndJitter[1] = b.origin.data[1];
        cmd.originAndJitter[2] = b.origin.data[2];
        cmd.originAndJitter[3] = std::max(b.originJitterRadius, 0.0f);
        cmd.dirAndCone[0] = b.direction.data[0];
        cmd.dirAndCone[1] = b.direction.data[1];
        cmd.dirAndCone[2] = b.direction.data[2];
        cmd.dirAndCone[3] = std::max(b.coneHalfAngleRadians, 0.0f);
        cmd.speedAndSize[0] = std::max(b.speedMin, 0.0f);
        cmd.speedAndSize[1] = std::max(b.speedMax, cmd.speedAndSize[0]);
        cmd.speedAndSize[2] = std::max(b.sizeStartMin, 0.001f);
        cmd.speedAndSize[3] = std::max(b.sizeStartMax, cmd.speedAndSize[2]);
        cmd.lifeAndScale[0] = std::max(b.lifetimeMin, 0.01f);
        cmd.lifeAndScale[1] = std::max(b.lifetimeMax, cmd.lifeAndScale[0]);
        cmd.lifeAndScale[2] = std::max(b.sizeEndScale, 0.0f);
        cmd.lifeAndScale[3] = std::max(b.velocityStretch, 0.0f);
        cmd.physics[0] = b.gravityScale;
        cmd.physics[1] = std::max(b.drag, 0.0f);
        cmd.physics[2] = std::clamp(b.restitution, 0.0f, 1.0f);
        cmd.physics[3] = std::clamp(b.friction, 0.0f, 1.0f);
        std::memcpy(cmd.colorStart, b.colorStart.data, sizeof(cmd.colorStart));
        std::memcpy(cmd.colorEnd, b.colorEnd.data, sizeof(cmd.colorEnd));
        cmd.emissionSpinClusterSeed[0] = std::max(b.emission, 0.0f);
        cmd.emissionSpinClusterSeed[1] = b.spinSpeed;
        cmd.emissionSpinClusterSeed[2] = std::bit_cast<float>(b.cluster);
        cmd.emissionSpinClusterSeed[3] = std::bit_cast<float>(frameSeedCounter++);
        outTotalSpawn += count;
        remainingMaxLifetime = std::max(remainingMaxLifetime, cmd.lifeAndScale[1] + 0.1f);
        outCmds.push_back(cmd);
    }
}

void RhiGpuParticlePass::BuildSimParams(const SimInputs &inputs, uint32_t activeLimit, SimParams &outParams) const
{
    outParams = {};
    outParams.worldUpAndDt[0] = inputs.worldUp[0];
    outParams.worldUpAndDt[1] = inputs.worldUp[1];
    outParams.worldUpAndDt[2] = inputs.worldUp[2];
    outParams.worldUpAndDt[3] = std::clamp(inputs.dt, 0.0f, 0.1f);

    const uint32_t volumeCount = inputs.pVolumes != nullptr
        ? std::min(inputs.volumeCount, MAX_VECTOR_FIELD_SLOTS)
        : 0u;
    outParams.counts[0] = maxParticles;
    outParams.counts[1] = activeLimit;
    outParams.counts[2] = volumeCount;
    outParams.counts[3] = inputs.enableCollision ? 1u : 0u;

    outParams.timeAndGravity[0] = inputs.time;
    outParams.timeAndGravity[1] = 9.81f;
    outParams.timeAndGravity[2] = 0.22f;
    outParams.timeAndGravity[3] = 0.0f;

    for (uint32_t i = 0u; i < volumeCount; i++)
    {
        const VolumeSubmission &sub = inputs.pVolumes[i];
        const QrVectorFieldVolume &v = sub.volume;
        ShVectorFieldVolume &dst = outParams.volumes[i];

        float inv[3][4] = {};
        float rot[3][3] = {};
        float center[3] = {};
        InvertAffine3x4(v.worldFromLocal, inv, rot, center);

        std::memcpy(dst.worldToLocalRow0, inv[0], sizeof(float) * 4);
        std::memcpy(dst.worldToLocalRow1, inv[1], sizeof(float) * 4);
        std::memcpy(dst.worldToLocalRow2, inv[2], sizeof(float) * 4);
        dst.localToWorldRotRow0[0] = rot[0][0];
        dst.localToWorldRotRow0[1] = rot[0][1];
        dst.localToWorldRotRow0[2] = rot[0][2];
        dst.localToWorldRotRow0[3] = center[0];
        dst.localToWorldRotRow1[0] = rot[1][0];
        dst.localToWorldRotRow1[1] = rot[1][1];
        dst.localToWorldRotRow1[2] = rot[1][2];
        dst.localToWorldRotRow1[3] = center[1];
        dst.localToWorldRotRow2[0] = rot[2][0];
        dst.localToWorldRotRow2[1] = rot[2][1];
        dst.localToWorldRotRow2[2] = rot[2][2];
        dst.localToWorldRotRow2[3] = center[2];
        dst.params0[0] = v.intensity;
        dst.params0[1] = v.maxSpeed;
        dst.params0[2] = std::clamp(v.tightness, 0.0f, 1.0f);
        dst.params0[3] = std::clamp(v.edgeFalloff, 0.0f, 1.0f);
        dst.params1[0] = v.curlFrequency;
        dst.params1[1] = v.curlScrollSpeed;
        dst.params1[2] = float(uint32_t(v.kind));
        dst.params1[3] = float(sub.atlasSlot);
        dst.axisAndPad[0] = v.axis.data[0];
        dst.axisAndPad[1] = v.axis.data[1];
        dst.axisAndPad[2] = v.axis.data[2];
        dst.axisAndPad[3] = 0.0f;
    }
}

void RhiGpuParticlePass::UploadDirtyAtlas(nvrhi::ICommandList *pCommandList)
{
    if (!atlasDirty || vectorFieldAtlas == nullptr || atlasCpu.empty())
    {
        return;
    }
    const size_t rowPitch = size_t(VECTOR_FIELD_BRICK_RES) * 8u;
    const size_t depthPitch = rowPitch * VECTOR_FIELD_BRICK_RES;
    pCommandList->writeTexture(vectorFieldAtlas, 0u, 0u, atlasCpu.data(), rowPitch, depthPitch);
    atlasDirty = false;
}

bool RhiGpuParticlePass::Simulate(nvrhi::ICommandList *pCommandList, const SimInputs &inputs)
{
    if (!created || pCommandList == nullptr || inputs.frameIndex >= MAX_FRAMES_IN_FLIGHT ||
        inputs.pUniformBuffer == nullptr || inputs.pTopLevel == nullptr ||
        !VertexDataReady(inputs.vertexData))
    {
        return false;
    }

    FrameResources &slot = frameSlots[inputs.frameIndex];
    if (!EnsureFrameBuffers(slot, inputs.frameIndex) || !EnsureSimBindingSets(slot, inputs))
    {
        return false;
    }

    if (pendingPoolReset)
    {
        pCommandList->clearBufferUInt(particlePoolBuffer, 0u);
        const uint16_t kZeroHalf[4] = {};
        pCommandList->writeTexture(dummyZeroTexture, 0u, 0u, kZeroHalf, sizeof(kZeroHalf));
        const std::vector<uint8_t> kZeroTm(sizeof(ShTonemapping), 0u);
        pCommandList->writeBuffer(dummyTonemappingBuffer, kZeroTm.data(), kZeroTm.size());
        ringWriteCursor = 0u;
        activeSlotLimit = 0u;
        remainingMaxLifetime = 0.0f;
        pendingPoolReset = false;
    }

    UploadDirtyAtlas(pCommandList);

    std::vector<ShParticleBurstCmd> cmds;
    uint32_t totalSpawn = 0u;
    BuildSpawnCommands(inputs.pSpawns, inputs.spawnCount, cmds, totalSpawn);

    if (totalSpawn == 0u && activeSlotLimit == 0u)
    {
        return true;
    }

    pCommandList->writeBuffer(slot.indirectArgsBuffer, kInitialIndirectArgs, sizeof(kInitialIndirectArgs));

    if (totalSpawn > 0u)
    {
        const uint32_t spawnHeader[4] = {
            totalSpawn,
            static_cast<uint32_t>(cmds.size()),
            ringWriteCursor,
            maxParticles,
        };
        pCommandList->writeBuffer(slot.spawnParamsBuffer, spawnHeader, sizeof(spawnHeader));
        pCommandList->writeBuffer(slot.spawnCmdBuffer, cmds.data(), cmds.size() * sizeof(ShParticleBurstCmd));

        nvrhi::ComputeState cs;
        cs.pipeline = spawnPipeline;
        cs.addBindingSet(slot.spawnSet);
        pCommandList->setComputeState(cs);
        pCommandList->dispatch((totalSpawn + 63u) / 64u, 1u, 1u);

        ringWriteCursor = (ringWriteCursor + totalSpawn) % maxParticles;
        activeSlotLimit = std::min(maxParticles, activeSlotLimit + totalSpawn);
    }

    if (activeSlotLimit > 0u)
    {
        SimParams simParams = {};
        BuildSimParams(inputs, activeSlotLimit, simParams);
        pCommandList->writeBuffer(slot.simParamsBuffer, &simParams, sizeof(simParams));

        nvrhi::ComputeState cs;
        cs.pipeline = simPipeline;
        cs.addBindingSet(slot.simSet);
        cs.addBindingSet(slot.simUniformSet);
        cs.addBindingSet(slot.simTlasSet);
        cs.addBindingSet(slot.simVertexDataSet);
        pCommandList->setComputeState(cs);
        pCommandList->dispatch((activeSlotLimit + 63u) / 64u, 1u, 1u);
    }

    remainingMaxLifetime = std::max(0.0f, remainingMaxLifetime - std::clamp(inputs.dt, 0.0f, 0.1f));
    if (remainingMaxLifetime <= 0.0f)
    {
        activeSlotLimit = 0u;
        ringWriteCursor = 0u;
    }
    return true;
}

bool RhiGpuParticlePass::CreatePipelines(nvrhi::Format color0Format, nvrhi::Format color1Format)
{
    if (color0Format == nvrhi::Format::UNKNOWN || color1Format == nvrhi::Format::UNKNOWN ||
        borrowedLightLayout == nullptr || borrowedRayStatsLayout == nullptr)
    {
        return false;
    }

    if (drawPipelines[0] != nullptr && pipelineColor0Format == color0Format && pipelineColor1Format == color1Format)
    {
        return true;
    }

    for (uint32_t bucket = 0u; bucket < DRAW_BUCKET_COUNT; bucket++)
    {
        if (drawPipelines[bucket] != nullptr && frameContext != nullptr)
        {
            frameContext->Retire(drawPipelines[bucket]);
        }
        drawPipelines[bucket] = nullptr;

        nvrhi::GraphicsPipelineDesc desc;
        desc.setVertexShader(drawVertShader);
        desc.setPixelShader(drawFragShader);
        desc.primType = nvrhi::PrimitiveType::TriangleList;
        desc.renderState.rasterState.setFillSolid().setCullMode(nvrhi::RasterCullMode::None)
            .setFrontCounterClockwise(true).setDepthClipEnable(true);
        ConfigureModeBlendAndDepth(bucket, desc);
        desc.addBindingLayout(textureTable->GetLayout());
        desc.addBindingLayout(drawUniformLayout);
        desc.addBindingLayout(drawSetLayout);
        desc.addBindingLayout(drawFramebuffersLayout);
        desc.addBindingLayout(drawTlasLayout);
        desc.addBindingLayout(borrowedLightLayout);
        desc.addBindingLayout(borrowedRayStatsLayout);
        desc.addBindingLayout(drawTonemappingLayout);

        nvrhi::FramebufferInfo fbInfo;
        fbInfo.addColorFormat(color0Format);
        fbInfo.addColorFormat(color1Format);
        fbInfo.setDepthFormat(nvrhi::Format::D32);
        fbInfo.setSampleCount(1);

        drawPipelines[bucket] = rhi::createGraphicsPipeline(
            device, desc, fbInfo, "RhiGpuParticle bucket " + std::to_string(bucket));
        if (drawPipelines[bucket] == nullptr)
        {
            return false;
        }
    }

    pipelineColor0Format = color0Format;
    pipelineColor1Format = color1Format;
    return true;
}

bool RhiGpuParticlePass::HasValidRenderInputs(const RenderInputs &inputs) const
{
    return created && activeSlotLimit > 0u && inputs.frameIndex < MAX_FRAMES_IN_FLIGHT &&
           inputs.pFramebuffer != nullptr && inputs.pUniformBuffer != nullptr &&
           inputs.pTopLevel != nullptr && inputs.pLightSet != nullptr &&
           inputs.pRayStatsSet != nullptr && inputs.pViewProj != nullptr &&
           inputs.width > 0u && inputs.height > 0u;
}

bool RhiGpuParticlePass::Render(nvrhi::ICommandList *pCommandList, const RenderInputs &inputs)
{
    if (pCommandList == nullptr || !HasValidRenderInputs(inputs))
    {
        return false;
    }

    FrameResources &slot = frameSlots[inputs.frameIndex];
    if (!EnsureFrameBuffers(slot, inputs.frameIndex) ||
        !EnsureDrawBindingSets(slot, inputs) ||
        !CreatePipelines(inputs.color0Format, inputs.color1Format))
    {
        return false;
    }

    const nvrhi::Viewport viewport(0.0f, float(inputs.width), float(inputs.height), 0.0f, 0.0f, 1.0f);
    const nvrhi::Rect scissor(0, static_cast<int>(inputs.width), 0, static_cast<int>(inputs.height));

    for (uint32_t bucket = 0u; bucket < DRAW_BUCKET_COUNT; bucket++)
    {
        nvrhi::GraphicsState gs;
        gs.pipeline = drawPipelines[bucket];
        gs.framebuffer = inputs.pFramebuffer;
        gs.viewport.addViewport(viewport).addScissorRect(scissor);
        gs.indirectParams = slot.indirectArgsBuffer;
        gs.addBindingSet(textureTable->GetTable());
        gs.addBindingSet(slot.drawUniformSet);
        gs.addBindingSet(slot.drawSet);
        gs.addBindingSet(slot.drawFramebuffersSet);
        gs.addBindingSet(slot.drawTlasSet);
        gs.addBindingSet(inputs.pLightSet);
        gs.addBindingSet(inputs.pRayStatsSet);
        gs.addBindingSet(slot.drawTonemappingSet);
        pCommandList->setGraphicsState(gs);

        DrawPush push = {};
        std::memcpy(push.viewProj, inputs.pViewProj, sizeof(push.viewProj));
        push.mode = bucket;
        push.maxParticles = maxParticles;
        push.renderSize[0] = inputs.width;
        push.renderSize[1] = inputs.height;
        pCommandList->setPushConstants(&push, sizeof(push));
        pCommandList->drawIndirect(bucket * 16u, 1u);
    }

    return true;
}

void RhiGpuParticlePass::RetireFrameResources(FrameResources &slot)
{
    slot.spawnSet = nullptr;
    slot.simSet = nullptr;
    slot.simUniformSet = nullptr;
    slot.simTlasSet = nullptr;
    slot.simVertexDataSet = nullptr;
    slot.drawSet = nullptr;
    slot.drawUniformSet = nullptr;
    slot.drawFramebuffersSet = nullptr;
    slot.drawTlasSet = nullptr;
    slot.drawTonemappingSet = nullptr;
    slot.spawnParamsBuffer = nullptr;
    slot.spawnCmdBuffer = nullptr;
    slot.simParamsBuffer = nullptr;
    slot.indirectArgsBuffer = nullptr;
    slot.renderIndicesBuffer = nullptr;
    slot.simUniformRef = nullptr;
    slot.simTlasRef = nullptr;
    std::fill(std::begin(slot.vertexDataRefs), std::end(slot.vertexDataRefs), nullptr);
    slot.drawUniformRef = nullptr;
    slot.drawTlasRef = nullptr;
    slot.drawTonemappingRef = nullptr;
    slot.pingLfShRef = nullptr;
    slot.pingLfCocgRef = nullptr;
}

void RhiGpuParticlePass::ReleaseTargets()
{
    if (device != nullptr)
    {
        device->waitForIdle();
    }
    for (FrameResources &slot : frameSlots)
    {
        RetireFrameResources(slot);
    }
    for (nvrhi::GraphicsPipelineHandle &p : drawPipelines)
    {
        p = nullptr;
    }
    spawnPipeline = nullptr;
    simPipeline = nullptr;
    pipelineColor0Format = nvrhi::Format::UNKNOWN;
    pipelineColor1Format = nvrhi::Format::UNKNOWN;
    particlePoolBuffer = nullptr;
    vectorFieldAtlas = nullptr;
    vectorFieldSampler = nullptr;
    dummyZeroTexture = nullptr;
    dummySampler = nullptr;
    dummyTonemappingBuffer = nullptr;
    spawnShader = nullptr;
    simShader = nullptr;
    drawVertShader = nullptr;
    drawFragShader = nullptr;
    spawnLayout = nullptr;
    simLayout = nullptr;
    simUniformLayout = nullptr;
    simTlasLayout = nullptr;
    simVertexDataLayout = nullptr;
    drawSetLayout = nullptr;
    drawUniformLayout = nullptr;
    drawFramebuffersLayout = nullptr;
    drawTlasLayout = nullptr;
    drawTonemappingLayout = nullptr;
    created = false;
}

} // namespace qray
