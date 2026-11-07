// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include "VertexTransform.hlsli"

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

    probeOutput[0] = float4(normal, determinant(model));
    probeOutput[1] = float4(tangent.xyz, dot(normal, tangent.xyz));
    probeOutput[2] = float4(tangent.w, determinant(mirrored) < 0.0 ? -1.0 : 1.0, 0.0, 0.0);
}
