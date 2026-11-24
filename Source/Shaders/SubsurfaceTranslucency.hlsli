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

#ifndef SUBSURFACE_TRANSLUCENCY_HLSLI_
#define SUBSURFACE_TRANSLUCENCY_HLSLI_

#include "Subsurface.hlsli"

// Measures thickness through the back of a closed mesh along light direction `l`
// using rayFlags = 0u so back-face culling never skips the exit face.
float3 traceSubsurfaceTransmittance(
    const Surface surf,
    const float3 lightPos,
    const float3 l,
    float lightDist,
    const float3 sssD,
    uint lightIndex,
    bool isSun)
{
    if (dot(surf.normalGeom, l) > 0.0 || lightDist <= 1e-4)
    {
        return (float3)0.0;
    }

    const float maxD = max(sssD.r, max(sssD.g, sssD.b));
    if (maxD <= 0.0)
    {
        return (float3)0.0;
    }

    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER> query;

    RayDesc rayDesc;
    rayDesc.Origin = surf.position - surf.normalGeom * RAY_ORIGIN_LEAK_BIAS;
    rayDesc.Direction = l;
    rayDesc.TMin = 2.5e-5;
    rayDesc.TMax = max(lightDist - 1e-3, 1e-4);

    query.TraceRayInline(topLevelAS, 0u, getShadowCullMask(surf.instCustomIndex), rayDesc);

    while (query.Proceed())
    {
    }

    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        if (query.CommittedTriangleFrontFace())
        {
            return (float3)0.0;
        }

        const float thickness = query.CommittedRayT() + RAY_ORIGIN_LEAK_BIAS;
        if (thickness > maxD * 6.0)
        {
            return (float3)0.0;
        }

        const float3 exitOrigin = rayDesc.Origin + l * (query.CommittedRayT() + RAY_ORIGIN_LEAK_BIAS);
        const bool ignoreFpv = (!isSun) && (globalUniform.lightIndexIgnoreFPVShadows == lightIndex);
        const float3 vis = traceShadowRay(surf.instCustomIndex, exitOrigin, lightPos, ignoreFpv);
        return vis * exp(-thickness / max(sssD, (float3)1e-4));
    }

    // Open single-sided mesh (such as thin foliage) with an unobstructed path to the light.
    const float thinThickness = 0.25 * maxD;
    return exp(-thinThickness / max(sssD, (float3)1e-4));
}

float3 shadeSubsurfaceBackLit(
    const Surface surf,
    const LightSample light,
    float oneOverPdf,
    const float4 sssData,
    uint lightIndex,
    bool isSun)
{
    const float sssMask = getSubsurfaceMask(sssData);
    const float3 sssD = getSubsurfaceMeanFreePath(sssData);
    if (sssMask <= 0.0 || max(sssD.r, max(sssD.g, sssD.b)) <= 0.0)
    {
        return (float3)0.0;
    }

    const float3 toLight = light.position - surf.position;
    const float lightDist = length(toLight);
    const float3 l = toLight / max(lightDist, 1e-6);

    const float3 transmittance = traceSubsurfaceTransmittance(
        surf, light.position, l, lightDist, sssD, lightIndex, isSun);
    if (max(transmittance.r, max(transmittance.g, transmittance.b)) <= 0.0)
    {
        return (float3)0.0;
    }

    const float backFacing = saturate(dot(-surf.normal, l) * 0.7 + 0.3);
    const float forwardScatter = 0.25 + 0.75 * pow(saturate(dot(-l, surf.toViewerDir)), 3.0);
    const float3 color = boostChroma(light.color);

    return light.dw * backFacing * forwardScatter * color * evalBRDFLambertian(1.0) * transmittance * sssMask * oneOverPdf;
}

#endif
