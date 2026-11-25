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



#define DESC_SET_GLOBAL_UNIFORM 0
#define DESC_SET_DECALS 3
#include "ShaderCommonHLSLFunc.hlsli"

float determinant3x3(float3 a, float3 b, float3 c)
{
    return a.x * (b.y * c.z - b.z * c.y) +
           a.y * (b.z * c.x - b.x * c.z) +
           a.z * (b.x * c.y - b.y * c.x);
}

float4x4 inverse4x4(float4x4 m)
{
    const float4 c0 = getColumn(m, 0);
    const float4 c1 = getColumn(m, 1);
    const float4 c2 = getColumn(m, 2);
    const float4 c3 = getColumn(m, 3);

    const float d0 = determinant3x3(c1.yzw, c2.yzw, c3.yzw);
    const float d1 = determinant3x3(c1.xzw, c2.xzw, c3.xzw);
    const float d2 = determinant3x3(c1.xyw, c2.xyw, c3.xyw);
    const float d3 = determinant3x3(c1.xyz, c2.xyz, c3.xyz);

    const float4x4 adjugate = float4x4(
        d0,
        -d1,
        d2,
        -d3,

        -determinant3x3(c0.yzw, c2.yzw, c3.yzw),
        determinant3x3(c0.xzw, c2.xzw, c3.xzw),
        -determinant3x3(c0.xyw, c2.xyw, c3.xyw),
        determinant3x3(c0.xyz, c2.xyz, c3.xyz),

        determinant3x3(c0.yzw, c1.yzw, c3.yzw),
        -determinant3x3(c0.xzw, c1.xzw, c3.xzw),
        determinant3x3(c0.xyw, c1.xyw, c3.xyw),
        -determinant3x3(c0.xyz, c1.xyz, c3.xyz),

        -determinant3x3(c0.yzw, c1.yzw, c2.yzw),
        determinant3x3(c0.xzw, c1.xzw, c2.xzw),
        -determinant3x3(c0.xyw, c1.xyw, c2.xyw),
        determinant3x3(c0.xyz, c1.xyz, c2.xyz));

    return adjugate / (c0.x * d0 - c0.y * d1 + c0.z * d2 - c0.w * d3);
}

float4 getPosition(uint vertexIndex)
{
    const uint b = 1u << (vertexIndex % 14);

    return float4(
        float((0x287A & b) != 0) - 0.5,
        float((0x02AF & b) != 0) - 0.5,
        float((0x31E3 & b) != 0) - 0.5,
        1.0
    );
}

void main(
    uint vertexIndex : SV_VertexID,
    uint instanceIndex : SV_InstanceID,
    [[vk::location(0)]] nointerpolation out uint outInstanceIndex : TEXCOORD0,
    [[vk::location(1)]] nointerpolation out float4 outWorldToLocalC0 : TEXCOORD1,
    [[vk::location(2)]] nointerpolation out float4 outWorldToLocalC1 : TEXCOORD2,
    [[vk::location(3)]] nointerpolation out float4 outWorldToLocalC2 : TEXCOORD3,
    [[vk::location(4)]] nointerpolation out float4 outWorldToLocalC3 : TEXCOORD4,
    out float4 outPosition : SV_Position)
{
    const float4x4 model = decalInstances[instanceIndex].transform;
    const float4x4 worldToLocal = inverse4x4(model);

    outInstanceIndex = instanceIndex;
    outWorldToLocalC0 = getColumn(worldToLocal, 0);
    outWorldToLocalC1 = getColumn(worldToLocal, 1);
    outWorldToLocalC2 = getColumn(worldToLocal, 2);
    outWorldToLocalC3 = getColumn(worldToLocal, 3);
    outPosition = mul(mul(mul(globalUniform.projection, globalUniform.view), model), getPosition(vertexIndex));
}
