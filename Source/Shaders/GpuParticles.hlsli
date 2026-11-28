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

#ifndef GPU_PARTICLES_HLSLI_
#define GPU_PARTICLES_HLSLI_

#define GPU_PARTICLE_MODE_SPARK         0u
#define GPU_PARTICLE_MODE_BILLBOARD     1u
#define GPU_PARTICLE_MODE_RETRO_SQUARE  2u
#define GPU_PARTICLE_MODE_DEBRIS_3D     3u
#define GPU_PARTICLE_MODE_COUNT         4u

#define GPU_PARTICLE_BUCKET_SPARK           0u
#define GPU_PARTICLE_BUCKET_BILLBOARD_ALPHA 1u
#define GPU_PARTICLE_BUCKET_BILLBOARD_ADD   2u
#define GPU_PARTICLE_BUCKET_RETRO_SQUARE    3u
#define GPU_PARTICLE_BUCKET_DEBRIS_3D       4u
#define GPU_PARTICLE_BUCKET_COUNT           5u

uint gpuParticleBucketToMode(uint bucket)
{
    if (bucket == GPU_PARTICLE_BUCKET_SPARK)
    {
        return GPU_PARTICLE_MODE_SPARK;
    }
    if (bucket <= GPU_PARTICLE_BUCKET_BILLBOARD_ADD)
    {
        return GPU_PARTICLE_MODE_BILLBOARD;
    }
    if (bucket == GPU_PARTICLE_BUCKET_RETRO_SQUARE)
    {
        return GPU_PARTICLE_MODE_RETRO_SQUARE;
    }
    return GPU_PARTICLE_MODE_DEBRIS_3D;
}

bool gpuParticleBucketIsAdditive(uint bucket)
{
    return bucket == GPU_PARTICLE_BUCKET_SPARK || bucket == GPU_PARTICLE_BUCKET_BILLBOARD_ADD;
}

#define GPU_PARTICLE_FLAG_COLLIDE_WORLD (1u << 0)
#define GPU_PARTICLE_FLAG_REST_ON_FLOOR (1u << 1)
#define GPU_PARTICLE_FLAG_VECTOR_FIELDS (1u << 2)
#define GPU_PARTICLE_FLAG_SELF_LIT      (1u << 3)
#define GPU_PARTICLE_FLAG_ADDITIVE      (1u << 4)
#define GPU_PARTICLE_FLAG_RESTING       (1u << 5)

#define GPU_VECTOR_FIELD_LUT_3D         0u
#define GPU_VECTOR_FIELD_CURL_NOISE     1u
#define GPU_VECTOR_FIELD_VORTEX         2u
#define GPU_VECTOR_FIELD_POINT_FORCE    3u
#define GPU_VECTOR_FIELD_DIRECTIONAL    4u

#define MAX_GPU_VECTOR_FIELD_VOLUMES    16u
#define VECTOR_FIELD_BRICK_RES          32.0
#define VECTOR_FIELD_INTERIOR_RES       30.0
#define VECTOR_FIELD_ATLAS_DEPTH        512.0

struct ShGpuParticle
{
    float4 posAndAge;
    float4 velAndLife;
    float4 colorStart;
    float4 colorEnd;
    float4 sizeAndStretch;
    float4 physics;
    uint4  meta;
    float4 spinData;
};

struct ShParticleBurstCmd
{
    uint4  spawnRange;
    float4 originAndJitter;
    float4 dirAndCone;
    float4 speedAndSize;
    float4 lifeAndScale;
    float4 physics;
    float4 colorStart;
    float4 colorEnd;
    float4 emissionSpinClusterSeed;
};

struct ShVectorFieldVolume
{
    float4 worldToLocalRow0;
    float4 worldToLocalRow1;
    float4 worldToLocalRow2;
    float4 localToWorldRotRow0;
    float4 localToWorldRotRow1;
    float4 localToWorldRotRow2;
    float4 params0;
    float4 params1;
    float4 axisAndPad;
};

uint gpuPcgHash(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float gpuRand01(inout uint rng)
{
    rng = gpuPcgHash(rng);
    return (float)(rng & 0x00FFFFFFu) * (1.0 / 16777216.0);
}

void gpuBuildOrthoBasis(const float3 axis, out float3 tangentU, out float3 tangentV)
{
    const float3 n = safeNormalize(axis);
    const float3 refAxis = abs(n.z) < 0.99 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    tangentU = safeNormalize(cross(refAxis, n));
    tangentV = cross(n, tangentU);
}

float3 gpuSampleConeDirection(const float3 axis, float coneHalfAngle, inout uint rng)
{
    const float clampedCone = clamp(coneHalfAngle, 0.0, M_PI);
    const float cosMax      = cos(clampedCone);
    const float cosTheta    = lerp(1.0, cosMax, gpuRand01(rng));
    const float sinTheta    = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    const float phi         = 2.0 * M_PI * gpuRand01(rng);

    float3 u, v;
    gpuBuildOrthoBasis(axis, u, v);
    const float3 n = safeNormalize(axis);
    return safeNormalize(n * cosTheta + (u * cos(phi) + v * sin(phi)) * sinTheta);
}

float gpuNoiseHash3(float3 p)
{
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return frac((p.x + p.y) * p.z);
}

float3 gpuNoiseVec3(float3 p)
{
    const float3 i = floor(p);
    const float3 f = frac(p);
    const float3 u = f * f * (3.0 - 2.0 * f);

    float3 accum = (float3)0.0;
    [unroll]
    for (int z = 0; z <= 1; z++)
    {
        [unroll]
        for (int y = 0; y <= 1; y++)
        {
            [unroll]
            for (int x = 0; x <= 1; x++)
            {
                const float3 corner = float3(float(x), float(y), float(z));
                const float wx = x == 0 ? (1.0 - u.x) : u.x;
                const float wy = y == 0 ? (1.0 - u.y) : u.y;
                const float wz = z == 0 ? (1.0 - u.z) : u.z;
                const float3 cell = i + corner;
                const float3 sampleV = float3(
                    gpuNoiseHash3(cell + float3(11.3, 47.1, 91.7)),
                    gpuNoiseHash3(cell + float3(73.9, 19.7, 37.3)),
                    gpuNoiseHash3(cell + float3(29.1, 83.3, 61.9))) * 2.0 - 1.0;
                accum += sampleV * (wx * wy * wz);
            }
        }
    }
    return accum;
}

float3 gpuParticleCurlNoise(float3 pos, float timeScroll)
{
    const float eps = 0.05;
    const float inv2Eps = 1.0 / (2.0 * eps);
    const float3 p = pos + float3(0.0, 0.0, timeScroll);

    const float3 px1 = gpuNoiseVec3(p + float3(eps, 0.0, 0.0));
    const float3 px0 = gpuNoiseVec3(p - float3(eps, 0.0, 0.0));
    const float3 py1 = gpuNoiseVec3(p + float3(0.0, eps, 0.0));
    const float3 py0 = gpuNoiseVec3(p - float3(0.0, eps, 0.0));
    const float3 pz1 = gpuNoiseVec3(p + float3(0.0, 0.0, eps));
    const float3 pz0 = gpuNoiseVec3(p - float3(0.0, 0.0, eps));

    const float dFz_dy = (py1.z - py0.z) * inv2Eps;
    const float dFy_dz = (pz1.y - pz0.y) * inv2Eps;
    const float dFx_dz = (pz1.x - pz0.x) * inv2Eps;
    const float dFz_dx = (px1.z - px0.z) * inv2Eps;
    const float dFy_dx = (px1.y - px0.y) * inv2Eps;
    const float dFx_dy = (py1.x - py0.x) * inv2Eps;

    return float3(dFz_dy - dFy_dz, dFx_dz - dFz_dx, dFy_dx - dFx_dy);
}

#endif
