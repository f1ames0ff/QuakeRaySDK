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
#define DESC_SET_TEXTURES       0
#define DESC_SET_LIGHT_SOURCES  6
#include "ShaderCommonHLSLFunc.hlsli"
#include "Random.hlsli"
#include "Smoke.hlsli"
#include "Light.hlsli"
#include "Q2ClusterLights.hlsli"
#include "SmokeLight.hlsli"

#define PROBE_DESC_SET 8

[[vk::binding(0, PROBE_DESC_SET)]] RWStructuredBuffer<float> probeOutput;

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const float3 worldPos = (float3)dispatchThreadID * 0.25;
    const uint   cluster  = dispatchThreadID.x;

    const float3 lit = smokeLightAt(worldPos, cluster, (float)dispatchThreadID.y);

    probeOutput[dispatchThreadID.x] = lit.x + lit.y + lit.z;
}
