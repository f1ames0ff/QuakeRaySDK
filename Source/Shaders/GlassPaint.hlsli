#ifndef GLASS_PAINT_HLSLI_
#define GLASS_PAINT_HLSLI_

#include "Q2LightGrid.hlsli"
#include "TransparencyFroxels.hlsli"

#ifdef Q2_REFL_REFR_SHADER
float4 shadeGlassPaint(const ShHitInfo pane, const RayCone cone, const float3 rayDir, const int2 pix)
{
    if ((pane.geometryInstanceFlags & GEOM_INST_FLAG_MEDIA_TYPE_GLASS) == 0u ||
        (pane.geometryInstanceFlags & GEOM_INST_FLAG_IGNORE_REFRACT_AFTER) != 0u)
    {
        return (float4)0.0;
    }
    int instanceId, customIndex, geometryIndex, primitiveIndex;
    float2 bary;
    unpackVisibilityBuffer(framebufVisibilityBuffer.Load(pix), instanceId, customIndex,
                           geometryIndex, primitiveIndex, bary);
    const ShTriangle tr = getTriangle(instanceId, customIndex, geometryIndex, primitiveIndex);
    const uint texture = tr.materials[1][MATERIAL_ALBEDO_ALPHA_INDEX];
    if (texture == MATERIAL_NO_TEXTURE)
    {
        return (float4)0.0;
    }
    const float3 weights = float3(1.0 - bary.x - bary.y, bary);
    const float2 uv = getSurfaceTexCoord(tr.geometryInstanceFlags, mul(tr.layerTexCoord[0], weights));
    const DerivativeSet derivatives = getTriangleUVDerivativesFromRayCone(tr, pane.normalGeom, cone, rayDir);
    const float4 paint = getTextureSampleDerivSet(texture, uv, derivatives, 0);
    const float coverage = saturate(paint.a);
    if (globalUniform.fixedAlbedo.x != 0.0)
    {
        return float4((float3)coverage, 1.0);
    }
    if (coverage == 0.0)
    {
        return (float4)0.0;
    }

    Surface surf = hitInfoToSurface_Indirect(pane, rayDir);
    surf.normalGeom = dot(surf.normalGeom, rayDir) > 0.0 ? -surf.normalGeom : surf.normalGeom;
    surf.normal = surf.normalGeom;
    surf.position += surf.normalGeom * 5e-4;
    surf.albedo = paint.rgb;
    surf.roughness = 1.0;
    float3 direct = (float3)0.0;
    if (globalUniform.directionalLightExists != 0 && q2ClusterSeesSky(surf.cluster))
    {
        const DirectionalLight sun = decodeAsDirectionalLight(lightSources[LIGHT_ARRAY_DIRECTIONAL_LIGHT_OFFSET]);
        const LightSample light = sampleDirectionalLight(sun, surf.position, float2(0.5, 0.5));
        const float3 direction = safeNormalize(light.position - surf.position);
        if (dot(surf.normal, direction) > 0.0 &&
            smokeVisible(surf.position, direction, length(light.position - surf.position)))
        {
            float3 diffuse;
            shadeDiffuse(surf, light, 1.0, diffuse);
            direct += diffuse;
        }
    }

    const int  gridCell  = q2GetLightGridCell(surf.position);
    const uint gridCount = q2GetLightGridCount(gridCell);

    if (gridCount > 0u)
    {
        const uint count = min(gridCount, uint(SMOKE_CLUSTER_SCAN));

        for (uint i = 0u; i < count; i++)
        {
            const uint index = q2GetLightGridLight(gridCell, i);

            if (index == uint(LIGHT_INDEX_NONE) ||
                index < LIGHT_ARRAY_REGULAR_LIGHTS_OFFSET ||
                index >= LIGHT_ARRAY_REGULAR_LIGHTS_OFFSET + globalUniform.lightCount)
            {
                continue;
            }

            float memberPdf = 1.0;
            const LightSample light = sampleLightNee(lightSources[index], surf.position,
                                                float2(0.5, 0.5), float2(0.5, 0.5), memberPdf);
            const float3 direction = safeNormalize(light.position - surf.position);
            if (light.dw > 0.0 && dot(surf.normal, direction) > 0.0 &&
                smokeVisible(surf.position, direction, length(light.position - surf.position)))
            {
                float3 diffuse;
                shadeDiffuse(surf, light, 1.0 / max(memberPdf, 1e-9), diffuse);
                direct += diffuse;
            }
        }
    }
    else if (surf.cluster < Q2_MAX_CLUSTERS)
    {
        const uint count = min(q2GetClusterLightCount(surf.cluster), uint(SMOKE_CLUSTER_SCAN));
        for (uint i = 0u; i < count; i++)
        {
            const uint index = q2GetClusterLight(surf.cluster, i);
            if (index < LIGHT_ARRAY_REGULAR_LIGHTS_OFFSET ||
                index >= LIGHT_ARRAY_REGULAR_LIGHTS_OFFSET + globalUniform.lightCount)
            {
                continue;
            }
            float memberPdf = 1.0;
            const LightSample light = sampleLightNee(lightSources[index], surf.position,
                                                float2(0.5, 0.5), float2(0.5, 0.5), memberPdf);
            const float3 direction = safeNormalize(light.position - surf.position);
            if (light.dw > 0.0 && dot(surf.normal, direction) > 0.0 &&
                smokeVisible(surf.position, direction, length(light.position - surf.position)))
            {
                float3 diffuse;
                shadeDiffuse(surf, light, 1.0 / max(memberPdf, 1e-9), diffuse);
                direct += diffuse;
            }
        }
    }

    if (globalUniform.transparencyFroxelParams.x != 0u)
    {
        float3 froxel;
        const float3 origin = float3(-4096.0, -2048.0, -4096.0);
        const float3 size = float3(8192.0, 4096.0, 8192.0);

        if (q2SampleTransparencyFroxel(surf.position, surf.normal, origin, size, froxel))
        {
            if (globalUniform.transparencyFroxelParams.x == 2u)
            {
                // Developer proof: value 2 makes the contribution visible as green.
                direct += float3(0.0, 10.0, 0.0);
            }
            else
            {
                // The froxel stores irradiance; the glass path works in radiance/BRDF units.
                // The blend weight is the policy switch (0 = off, 1 = full).
                direct += froxel / 3.14159265 * globalUniform.transparencyFroxelBlendParams.x;
            }
        }
    }

    const float3 ambient = getSkyAmbientMultiplied(surf.normal, SKY_DIFFUSE_BOUNCE_LOD);
    const float3 lit = max(paint.rgb * (direct + ambient), (float3)0.0);
    return float4(lit * coverage, coverage);
}

void compositeGlassPaint(const int2 pix, const float4 paint)
{
    if (paint.a > 0.0)
    {
        const int2 regularPix = getRegularPixFromCheckerboardPix(pix);
        framebufQ2ParticleLayer[regularPix] = q2AlphaBlendPremultiplied(
            paint, framebufQ2ParticleLayer[regularPix]);
    }
}
#endif
#endif
