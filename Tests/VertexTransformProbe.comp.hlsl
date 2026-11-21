// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include "VertexTransform.hlsli"
#include "Utils.hlsli"

[[vk::binding(0, 0)]] RWStructuredBuffer<float4> probeOutput;

[numthreads(1, 1, 1)]
void main()
{
    const float3x3 model = float3x3(
        2.0, 0.0, 0.0,
        0.0, 3.0, 0.0,
        0.0, 0.0, 4.0);
    const float3 normal = normalize(transformNormalInverseTranspose(model, float3(1.0, 1.0, 1.0)));
    const float4 tangent = transformTangentByModel(model, normal, float4(1.0, 0.0, 1.0, 1.0));

    const float3x3 mirrored = float3x3(
        -2.0, 0.0, 0.0,
         0.0, 3.0, 0.0,
         0.0, 0.0, 4.0);

    const float3 smallTriNormal = safeNormalize(cross(float3(0.008, 0.0, 0.0), float3(0.0, 0.005, 0.0)));
    const float3x3 smallModel = float3x3(
        0.0015, 0.0,     0.0,
        0.0,    0.00016, 0.0,
        0.0,    0.0,     0.00053);
    const float3 smallModelNormal = safeNormalize(transformNormalInverseTranspose(smallModel, float3(1.0, 1.0, 1.0)));
    const float4 smallModelTangent = transformTangentByModel(smallModel, smallModelNormal, float4(1.0, 0.0, 1.0, 1.0));

    probeOutput[0] = float4(normal, determinant(model));
    probeOutput[1] = float4(tangent.xyz, dot(normal, tangent.xyz));
    probeOutput[2] = float4(tangent.w, determinant(mirrored) < 0.0 ? -1.0 : 1.0, 0.0, 0.0);
    probeOutput[3] = float4(smallTriNormal, length(smallTriNormal));
    probeOutput[4] = float4(smallModelNormal, length(smallModelNormal));
    probeOutput[5] = float4(smallModelTangent.xyz, dot(smallModelNormal, smallModelTangent.xyz));
    probeOutput[6] = float4(safeNormalize(float3(0.0, 0.0, 0.0)), safeNormalize(float3(1.0e-30, 0.0, 0.0)).y);
}
