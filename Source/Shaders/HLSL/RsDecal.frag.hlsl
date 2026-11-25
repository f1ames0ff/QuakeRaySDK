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


#define FRAMEBUF_IGNORE_ATTACHMENTS
#define DESC_SET_GLOBAL_UNIFORM 0
#define DESC_SET_FRAMEBUFFERS 1
#define DESC_SET_TEXTURES 2
#define DESC_SET_DECALS 3
#include "ShaderCommonHLSLFunc.hlsli"

void main(
    float4 fragCoord : SV_Position,
    [[vk::location(0)]] nointerpolation uint instanceIndex : TEXCOORD0,
    [[vk::location(1)]] nointerpolation float4 worldToLocalC0 : TEXCOORD1,
    [[vk::location(2)]] nointerpolation float4 worldToLocalC1 : TEXCOORD2,
    [[vk::location(3)]] nointerpolation float4 worldToLocalC2 : TEXCOORD3,
    [[vk::location(4)]] nointerpolation float4 worldToLocalC3 : TEXCOORD4,
    out float4 outAlbedo : SV_Target0)
{
    const int2 pix = getCheckerboardPix(int2(fragCoord.xy));

    const ShDecalInstance decal = decalInstances[instanceIndex];

    const float3 worldPosition = framebufSurfacePosition_Sampled.Load(int3(pix, 0)).xyz;
    const float4 localPosition = worldPosition.x * worldToLocalC0 +
                                 worldPosition.y * worldToLocalC1 +
                                 worldPosition.z * worldToLocalC2 +
                                 worldToLocalC3;

    if (any(abs(localPosition.xyz) > (float3)0.5))
    {
        discard;
    }

    const float2 texCoord = localPosition.xy + 0.5;

    const float4 decalAlbedo = getTextureSample(decal.textureAlbedoAlpha, texCoord);

    outAlbedo = decalAlbedo;
}
