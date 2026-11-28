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

#include "Utils.hlsli"
#include "GpuParticles.hlsli"

struct ParticleSpawnParams_BT
{
    uint4 spawnInfo;
};

[[vk::binding(0, 0)]] ConstantBuffer<ParticleSpawnParams_BT> spawnParams;
[[vk::binding(1, 0)]] StructuredBuffer<ShParticleBurstCmd> spawnCmds;
[[vk::binding(2, 0)]] RWStructuredBuffer<ShGpuParticle> particlePool;

uint findSpawnCommandIndex(uint tid, uint cmdCount)
{
    uint lo = 0u;
    uint hi = cmdCount;
    while (lo + 1u < hi)
    {
        const uint mid = (lo + hi) >> 1u;
        if (tid < spawnCmds[mid].spawnRange.x)
        {
            hi = mid;
        }
        else
        {
            lo = mid;
        }
    }
    return lo;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint tid = dispatchThreadId.x;
    const uint totalSpawn = spawnParams.spawnInfo.x;
    const uint cmdCount   = spawnParams.spawnInfo.y;
    const uint ringBase   = spawnParams.spawnInfo.z;
    const uint maxSlots   = spawnParams.spawnInfo.w;

    if (tid >= totalSpawn || cmdCount == 0u || maxSlots == 0u)
    {
        return;
    }

    const uint cmdIdx = findSpawnCommandIndex(tid, cmdCount);
    const ShParticleBurstCmd cmd = spawnCmds[cmdIdx];

    uint rng = tid * 747796405u ^ asuint(cmd.emissionSpinClusterSeed.w) ^ (cmdIdx * 2654435761u) ^ 17u;
    (void)gpuRand01(rng);

    const float3 dir = gpuSampleConeDirection(cmd.dirAndCone.xyz, cmd.dirAndCone.w, rng);
    const float speed = lerp(cmd.speedAndSize.x, cmd.speedAndSize.y, gpuRand01(rng));
    const float sizeStart = lerp(cmd.speedAndSize.z, cmd.speedAndSize.w, gpuRand01(rng));
    const float sizeEnd = sizeStart * max(cmd.lifeAndScale.z, 0.0);
    const float life = max(lerp(cmd.lifeAndScale.x, cmd.lifeAndScale.y, gpuRand01(rng)), 0.01);

    float3 jitterOffset = (float3)0.0;
    if (cmd.originAndJitter.w > 0.0)
    {
        const float3 randDir = gpuSampleConeDirection(float3(0.0, 0.0, 1.0), M_PI, rng);
        const float rad = cmd.originAndJitter.w * pow(gpuRand01(rng), 1.0 / 3.0);
        jitterOffset = randDir * rad;
    }

    ShGpuParticle p;
    p.posAndAge      = float4(cmd.originAndJitter.xyz + jitterOffset, 0.0);
    p.velAndLife     = float4(dir * speed, life);
    p.colorStart     = cmd.colorStart;
    p.colorEnd       = cmd.colorEnd;
    p.sizeAndStretch = float4(sizeStart, sizeEnd, cmd.lifeAndScale.w, cmd.emissionSpinClusterSeed.x);
    p.physics        = cmd.physics;
    p.meta           = uint4(cmd.spawnRange.z, cmd.spawnRange.w, asuint(cmd.emissionSpinClusterSeed.z), gpuPcgHash(rng));
    p.spinData       = float4(2.0 * M_PI * gpuRand01(rng),
                              cmd.emissionSpinClusterSeed.y * (gpuRand01(rng) * 2.0 - 1.0),
                              0.0, 0.0);

    const uint slot = (ringBase + tid) % maxSlots;
    particlePool[slot] = p;
}
