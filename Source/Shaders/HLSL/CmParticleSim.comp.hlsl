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

#define DESC_SET_GLOBAL_UNIFORM 1
#define DESC_SET_TLAS           2
#define DESC_SET_VERTEX_DATA    3
#include "ShaderCommonHLSLFunc.hlsli"
#include "GpuParticles.hlsli"

struct ParticleSimParams_BT
{
    float4              worldUpAndDt;
    uint4               counts;
    float4              timeAndGravity;
    ShVectorFieldVolume volumes[MAX_GPU_VECTOR_FIELD_VOLUMES];
};

[[vk::binding(0, 0)]] ConstantBuffer<ParticleSimParams_BT> simParams;
[[vk::binding(1, 0)]] RWStructuredBuffer<ShGpuParticle> particlePool;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> indirectArgsRaw;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> renderIndices;
[[vk::binding(4, 0)]] Texture3D<float4> vectorFieldAtlas;
[[vk::binding(5, 0)]] SamplerState vectorFieldSampler;

[[vk::binding(0, DESC_SET_TLAS)]] RaytracingAccelerationStructure topLevelAS;

float3 evalVolumeFieldVector(const ShVectorFieldVolume vol, const float3 worldPos, const float3 localPos, float simTime)
{
    const uint kind = uint(vol.params1.z + 0.5);
    const float3 worldCenter = float3(vol.localToWorldRotRow0.w, vol.localToWorldRotRow1.w, vol.localToWorldRotRow2.w);
    const float3 axis = safeNormalize(vol.axisAndPad.xyz);

    if (kind == GPU_VECTOR_FIELD_LUT_3D)
    {
        const float3 uvw01 = saturate(localPos * 0.5 + 0.5);
        const float brick = floor(vol.params1.w + 0.5);
        const float3 atlasUvw = float3(
            (uvw01.xy * VECTOR_FIELD_INTERIOR_RES + 1.0) / VECTOR_FIELD_BRICK_RES,
            (uvw01.z * VECTOR_FIELD_INTERIOR_RES + 1.0 + brick * VECTOR_FIELD_BRICK_RES) / VECTOR_FIELD_ATLAS_DEPTH);
        const float3 localVec = vectorFieldAtlas.SampleLevel(vectorFieldSampler, atlasUvw, 0.0).xyz;
        return float3(
            dot(vol.localToWorldRotRow0.xyz, localVec),
            dot(vol.localToWorldRotRow1.xyz, localVec),
            dot(vol.localToWorldRotRow2.xyz, localVec));
    }

    if (kind == GPU_VECTOR_FIELD_CURL_NOISE)
    {
        return gpuParticleCurlNoise(worldPos * max(vol.params1.x, 0.01), simTime * vol.params1.y);
    }

    if (kind == GPU_VECTOR_FIELD_VORTEX)
    {
        const float3 rel = worldPos - worldCenter;
        const float3 radial = rel - axis * dot(rel, axis);
        const float radLen = length(radial);
        const float3 tangent = radLen > 1e-4 ? safeNormalize(cross(axis, radial)) : (float3)0.0;
        const float3 swirl = tangent * saturate(radLen * 1.5 + 0.25) - radial * 0.2 + axis * 0.45;
        return swirl + gpuParticleCurlNoise(worldPos * max(vol.params1.x, 0.5), simTime * vol.params1.y) * 0.35;
    }

    if (kind == GPU_VECTOR_FIELD_POINT_FORCE)
    {
        const float3 delta = worldPos - worldCenter;
        const float dist = max(length(delta), 0.15);
        return (delta / dist) / (1.0 + dist * dist * 0.25);
    }

    const float3 turb = vol.params1.x > 0.0
        ? gpuParticleCurlNoise(worldPos * vol.params1.x, simTime * vol.params1.y) * 0.25
        : (float3)0.0;
    return axis + turb;
}

void applyVectorFields(const float3 worldPos, inout float3 vel, inout uint flags, float dt)
{
    const uint volumeCount = min(simParams.counts.z, MAX_GPU_VECTOR_FIELD_VOLUMES);
    const float simTime = simParams.timeAndGravity.x;

    for (uint i = 0u; i < volumeCount; i++)
    {
        const ShVectorFieldVolume vol = simParams.volumes[i];
        const float4 wp = float4(worldPos, 1.0);
        const float3 localPos = float3(
            dot(vol.worldToLocalRow0, wp),
            dot(vol.worldToLocalRow1, wp),
            dot(vol.worldToLocalRow2, wp));

        if (any(abs(localPos) > 1.0))
        {
            continue;
        }

        const float edgeDist = 1.0 - max(abs(localPos.x), max(abs(localPos.y), abs(localPos.z)));
        const float falloff = vol.params0.w;
        const float weight = falloff > 1e-4 ? smoothstep(0.0, falloff, edgeDist) : 1.0;

        const float3 fieldVec = evalVolumeFieldVector(vol, worldPos, localPos, simTime);
        const float intensity = vol.params0.x;
        const float maxSpeed = max(vol.params0.y, 0.0);
        const float tightness = saturate(vol.params0.z);

        vel += fieldVec * (intensity * (1.0 - tightness) * weight * dt);
        if (tightness > 0.0)
        {
            const float3 targetVel = maxSpeed > 0.0 ? fieldVec * maxSpeed : fieldVec * intensity;
            const float blend = saturate(tightness * weight * (1.0 - exp(-8.0 * dt)));
            vel = lerp(vel, targetVel, blend);
        }

        if (length(vel) > simParams.timeAndGravity.z * 1.5)
        {
            flags &= ~GPU_PARTICLE_FLAG_RESTING;
        }
    }
}

void advanceWithWorldCollision(
    inout float3 pos,
    inout float3 vel,
    inout float spinSpeed,
    inout uint cluster,
    inout uint flags,
    const float4 physics,
    const float curSize,
    const uint mode,
    const float dt)
{
    if (simParams.counts.w == 0u || (flags & GPU_PARTICLE_FLAG_COLLIDE_WORLD) == 0u)
    {
        pos += vel * dt;
        return;
    }

    const float3 worldUp = safeNormalize(simParams.worldUpAndDt.xyz);
    const float rClear = (mode == GPU_PARTICLE_MODE_RETRO_SQUARE || mode == GPU_PARTICLE_MODE_BILLBOARD)
        ? max(0.5 * curSize, 0.008)
        : max(0.45 * curSize, 0.008);

    float remDt = dt;
    [loop]
    for (uint sub = 0u; sub < 2u; sub++)
    {
        const float3 stepVec = vel * remDt;
        const float stepLen = length(stepVec);
        if (stepLen <= 1e-5)
        {
            break;
        }

        const float3 stepDir = stepVec / stepLen;

        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> query;
        RayDesc rayDesc;
        rayDesc.Origin    = pos;
        rayDesc.Direction = stepDir;
        rayDesc.TMin      = 0.0;
        rayDesc.TMax      = stepLen + rClear;

        query.TraceRayInline(topLevelAS, RAY_FLAG_NONE, globalUniform.rayCullMaskWorld, rayDesc);
        while (query.Proceed())
        {
        }

        if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        {
            pos += stepVec;
            break;
        }

        const ShTriangle tr = getTriangle(
            (int)query.CommittedInstanceIndex(),
            (int)query.CommittedInstanceID(),
            (int)query.CommittedGeometryIndex(),
            (int)query.CommittedPrimitiveIndex());

        const float3 p0 = getColumn(tr.positions, 0);
        const float3 p1 = getColumn(tr.positions, 1);
        const float3 p2 = getColumn(tr.positions, 2);
        float3 N = safeNormalize(cross(p1 - p0, p2 - p0));
        if (dot(N, stepDir) > 0.0)
        {
            N = -N;
        }

        if (tr.cluster != 0u)
        {
            cluster = tr.cluster;
        }

        const float hitDist = query.CommittedRayT();
        const float safeTravel = max(hitDist - rClear, 0.0);
        pos += stepDir * safeTravel;
        if (hitDist < rClear)
        {
            pos += N * (rClear - hitDist + 5e-4);
        }
        else
        {
            pos += N * 5e-4;
        }

        const float vDotN = dot(vel, N);
        if (vDotN < 0.0)
        {
            const float3 vN = vDotN * N;
            const float3 vT = vel - vN;
            vel = -vN * clamp(physics.z, 0.0, 1.0) + vT * saturate(1.0 - physics.w);
        }

        spinSpeed *= saturate(1.0 - 0.5 * physics.w);

        if ((flags & GPU_PARTICLE_FLAG_REST_ON_FLOOR) != 0u &&
            dot(N, worldUp) > 0.55 &&
            length(vel) < simParams.timeAndGravity.z)
        {
            vel = (float3)0.0;
            spinSpeed = 0.0;
            flags |= GPU_PARTICLE_FLAG_RESTING;
            break;
        }

        remDt *= saturate(1.0 - safeTravel / max(stepLen, 1e-5));
        if (remDt <= 1e-5)
        {
            break;
        }
    }
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint slot = dispatchThreadId.x;
    const uint maxParticles = simParams.counts.x;
    const uint activeLimit  = min(simParams.counts.y, maxParticles);

    if (slot >= activeLimit)
    {
        return;
    }

    ShGpuParticle p = particlePool[slot];
    if (p.velAndLife.w <= 0.0 || p.posAndAge.w >= p.velAndLife.w)
    {
        return;
    }

    const float dt = clamp(simParams.worldUpAndDt.w, 0.0, 0.1);
    p.posAndAge.w += dt;
    if (p.posAndAge.w >= p.velAndLife.w)
    {
        particlePool[slot] = p;
        return;
    }

    const uint mode = min(p.meta.x & 0xFFu, GPU_PARTICLE_MODE_COUNT - 1u);
    uint flags = p.meta.x >> 8u;
    const float lifeFrac = saturate(p.posAndAge.w / p.velAndLife.w);
    const float curSize = lerp(p.sizeAndStretch.x, p.sizeAndStretch.y, lifeFrac);

    float3 pos = p.posAndAge.xyz;
    float3 vel = p.velAndLife.xyz;
    float spinSpeed = p.spinData.y;
    uint cluster = p.meta.z;

    if (dt > 0.0)
    {
        if ((flags & GPU_PARTICLE_FLAG_VECTOR_FIELDS) != 0u && simParams.counts.z > 0u)
        {
            applyVectorFields(pos, vel, flags, dt);
        }

        if ((flags & GPU_PARTICLE_FLAG_RESTING) == 0u)
        {
            const float3 worldUp = safeNormalize(simParams.worldUpAndDt.xyz);
            vel -= worldUp * (simParams.timeAndGravity.y * p.physics.x * dt);
            vel *= exp(-max(p.physics.y, 0.0) * dt);

            advanceWithWorldCollision(pos, vel, spinSpeed, cluster, flags,
                                      p.physics, curSize, mode, dt);
            p.spinData.x += spinSpeed * dt;
        }
    }

    p.posAndAge.xyz = pos;
    p.velAndLife.xyz = vel;
    p.meta.x = mode | (flags << 8u);
    p.meta.z = cluster;
    p.spinData.y = spinSpeed;
    particlePool[slot] = p;

    uint bucket = GPU_PARTICLE_BUCKET_DEBRIS_3D;
    if (mode == GPU_PARTICLE_MODE_SPARK)
    {
        bucket = GPU_PARTICLE_BUCKET_SPARK;
    }
    else if (mode == GPU_PARTICLE_MODE_BILLBOARD)
    {
        bucket = (flags & GPU_PARTICLE_FLAG_ADDITIVE) != 0u
                     ? GPU_PARTICLE_BUCKET_BILLBOARD_ADD
                     : GPU_PARTICLE_BUCKET_BILLBOARD_ALPHA;
    }
    else if (mode == GPU_PARTICLE_MODE_RETRO_SQUARE)
    {
        bucket = GPU_PARTICLE_BUCKET_RETRO_SQUARE;
    }

    uint dstIdx = 0u;
    InterlockedAdd(indirectArgsRaw[bucket * 4u + 1u], 1u, dstIdx);
    if (dstIdx < maxParticles)
    {
        renderIndices[bucket * maxParticles + dstIdx] = slot;
    }
}
