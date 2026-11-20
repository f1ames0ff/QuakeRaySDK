// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#pragma once

float3 transformNormalInverseTranspose(float3x3 model, float3 normal)
{
    const float3x3 columns = transpose(model);
    const float3 cofactor0 = cross(columns[1], columns[2]);
    const float3 cofactor1 = cross(columns[2], columns[0]);
    const float3 cofactor2 = cross(columns[0], columns[1]);
    return cofactor0 * normal.x + cofactor1 * normal.y + cofactor2 * normal.z;
}

float4 transformTangentByModel(float3x3 model, float3 normal, float4 tangent)
{
    const float3 scaled = mul(model, tangent.xyz);
    const float3 orthogonal = scaled - normal * dot(normal, scaled);
    const float maxComp = max(abs(orthogonal.x), max(abs(orthogonal.y), abs(orthogonal.z)));
    if (all(isfinite(orthogonal)) && maxComp > 1.0e-20)
    {
        const float3 unitScaled = orthogonal / maxComp;
        tangent.xyz = unitScaled * rsqrt(dot(unitScaled, unitScaled));
    }
    else
    {
        tangent.xyz = float3(0.0, 1.0, 0.0);
    }
    tangent.w *= determinant(model) < 0.0 ? -1.0 : 1.0;
    return tangent;
}
