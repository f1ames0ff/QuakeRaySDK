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

#ifdef DESC_SET_VERTEX_DATA
#ifdef DESC_SET_GLOBAL_UNIFORM
#ifdef DESC_SET_TEXTURES
#include "Subsurface.hlsli"

#if defined(HITINFO_INL_PRIM)
float3 processAlbedoGrad(uint geometryInstanceFlags, const float2 texCoords[3], const uint3 materials[3], const float4 materialColors[3], const float2 dPdx[3], const float2 dPdy[3])
#elif defined(HITINFO_INL_RFL)
float3 processAlbedoRayConeDeriv(uint geometryInstanceFlags, const float2 texCoords[3], const uint3 materials[3], const float4 materialColors[3], const DerivativeSet derivSet)
#elif defined(HITINFO_INL_INDIR)
float3 processAlbedo(uint geometryInstanceFlags, const float2 texCoords[3], const uint3 materials[3], const float4 materialColors[3], float lod)
#endif
{
    const uint blendsFlags[3] =
    {
        (geometryInstanceFlags & MATERIAL_BLENDING_MASK_FIRST_LAYER)  >> (MATERIAL_BLENDING_FLAG_BIT_COUNT * 0),
        (geometryInstanceFlags & MATERIAL_BLENDING_MASK_SECOND_LAYER) >> (MATERIAL_BLENDING_FLAG_BIT_COUNT * 1),
        (geometryInstanceFlags & MATERIAL_BLENDING_MASK_THIRD_LAYER)  >> (MATERIAL_BLENDING_FLAG_BIT_COUNT * 2)
    };

    float3 dst = (float3)1.0;
    bool hasAnyAlbedoTexture = false;

    for (int i = 0; i < MATERIAL_MAX_ALBEDO_LAYERS; i++)
    {
        if (i > 0 && (geometryInstanceFlags & GEOM_INST_FLAG_MEDIA_TYPE_GLASS) != 0u)
        {
            continue;
        }
        if (materials[i][MATERIAL_ALBEDO_ALPHA_INDEX] != MATERIAL_NO_TEXTURE)
        {
            const float4 src = materialColors[i] *
        #if defined(HITINFO_INL_PRIM)
                getTextureSampleGrad(materials[i][MATERIAL_ALBEDO_ALPHA_INDEX], texCoords[i], dPdx[i], dPdy[i]);
        #elif defined(HITINFO_INL_RFL)
                getTextureSampleDerivSet(materials[i][MATERIAL_ALBEDO_ALPHA_INDEX], texCoords[i], derivSet, i);
        #elif defined(HITINFO_INL_INDIR)
                getTextureSampleLod(materials[i][MATERIAL_ALBEDO_ALPHA_INDEX], texCoords[i], lod);
        #endif

            bool opq = (blendsFlags[i] & MATERIAL_BLENDING_FLAG_OPAQUE) != 0;
            bool alp = (blendsFlags[i] & MATERIAL_BLENDING_FLAG_ALPHA)  != 0;
            bool add = (blendsFlags[i] & MATERIAL_BLENDING_FLAG_ADD)    != 0;
            bool shd = (blendsFlags[i] & MATERIAL_BLENDING_FLAG_SHADE)  != 0;

            opq = opq || (alp && i == 0);
            alp = alp && !opq;

            dst = float(opq) * (src.rgb) +
                  float(alp) * (src.rgb * src.a + dst * (1 - src.a)) +
                  float(add) * (src.rgb + dst) +
                  float(shd) * (src.rgb * dst * 2);

            hasAnyAlbedoTexture = true;
        }
    }

    dst = lerp(materialColors[0].rgb, dst, float(hasAnyAlbedoTexture));

    return clamp(dst, (float3)0.0, (float3)1.0);
}


#if defined(HITINFO_INL_INDIR)
float3 getHitInfoAlbedoOnly(ShPayload pl)
{
    int instanceId, instCustomIndex;
    int geomIndex, primIndex;

    unpackInstanceIdAndCustomIndex(pl.instIdAndIndex, instanceId, instCustomIndex);
    unpackGeometryAndPrimitiveIndex(pl.geomAndPrimIndex, geomIndex, primIndex);

    const ShTriangle tr = getTriangle(instanceId, instCustomIndex, geomIndex, primIndex);

    const float2 inBaryCoords = pl.baryCoords;
    const float3 baryCoords = float3(1.0f - inBaryCoords.x - inBaryCoords.y, inBaryCoords.x, inBaryCoords.y);

    const float2 texCoords[3] =
    {
        getSurfaceTexCoord(tr.geometryInstanceFlags, mul(tr.layerTexCoord[0], baryCoords)),
        mul(tr.layerTexCoord[1], baryCoords),
        mul(tr.layerTexCoord[2], baryCoords)
    };

    return processAlbedo(tr.geometryInstanceFlags, texCoords, tr.materials, tr.materialColors, 0);
}
#endif


#if defined(HITINFO_INL_PRIM)
float getBounceLOD(float roughness, float viewDist, float hitDist, float screenWidth, float bounceMipBias)
{
    const float range = 300.0 * pow((1.0 - roughness) * 0.9 + 0.1, 4.0);

    float2 f = float2(viewDist, hitDist);
    f = clamp(f / range, (float2)0.0, (float2)1.0);
    f = sqrt(f);

    float mip = max(log2(3840.0 / screenWidth), 0.0);

    mip += f.x * 10.0;
    mip += f.y * 10.0;

    return mip + bounceMipBias;
}
#endif


#if defined(HITINFO_INL_PRIM)
float3 intersectRayTriangle(const float3x3 positions, const float3 orig, const float3 dir)
{
    const float3 edge1 = getColumn(positions, 1) - getColumn(positions, 0);
    const float3 edge2 = getColumn(positions, 2) - getColumn(positions, 0);

    const float3 pvec = cross(dir, edge2);

    const float det = dot(edge1, pvec);
    if (abs(det) < 1e-12)
    {
        return float3(1.0, 0.0, 0.0);
    }
    const float invDet = 1.0 / det;

    const float3 tvec = orig - getColumn(positions, 0);
    const float3 qvec = cross(tvec, edge1);

    const float u = dot(tvec, pvec) * invDet;
    const float v = dot(dir, qvec) * invDet;

    return float3(1 - u - v, u, v);
}
#endif


#if (defined(HITINFO_INL_PRIM) || defined(HITINFO_INL_RFL)) && !defined(HITINFO_PARALLAX_GEOM_DEFINED)
#define HITINFO_PARALLAX_GEOM_DEFINED
bool setupParallaxGeometry(
    const ShTriangle tr,
    const float3 hitPosition,
    const float3 normalGeom,
    const float3 rayOrigin,
    out uint outNrmTex,
    out float outEffScale,
    out float3 outToCam,
    out float3 outViewTS,
    out float outMaxWorldDepth,
    out float2 outMaxDeltaUV)
{
    outNrmTex = tr.materials[0][MATERIAL_NORMAL_INDEX];
    outEffScale = 0.0;
    outToCam = (float3)0.0;
    outViewTS = float3(0.0, 0.0, 1.0);
    outMaxWorldDepth = 0.0;
    outMaxDeltaUV = (float2)0.0;

    const float parallaxScale =
        ((tr.geometryInstanceFlags & (GEOM_INST_FLAG_MEDIA_TYPE_GLASS |
                                      GEOM_INST_FLAG_MEDIA_TYPE_WATER |
                                      GEOM_INST_FLAG_SUBSURFACE)) == 0u
         && tr.materials[2][MATERIAL_ALBEDO_ALPHA_INDEX] == MATERIAL_NO_TEXTURE
         && outNrmTex != MATERIAL_NO_TEXTURE)
        ? clamp(tr.materialColors[2].z, 0.0, 0.25)
        : 0.0;
    if (!(parallaxScale > 1e-4))
    {
        return false;
    }

    outToCam = safeNormalize(rayOrigin - hitPosition);
    const float3 bitangent = cross(normalGeom, tr.tangent.xyz) * tr.tangent.w;
    outViewTS = float3(
        dot(outToCam, tr.tangent.xyz),
        dot(outToCam, bitangent),
        dot(outToCam, normalGeom));

    const float grazingFade = smoothstep(0.08, 0.28, outViewTS.z);
    outEffScale = parallaxScale * grazingFade;
    if (!(outEffScale > 1e-4))
    {
        return false;
    }

    const float3 e1 = getColumn(tr.positions, 1) - getColumn(tr.positions, 0);
    const float3 e2 = getColumn(tr.positions, 2) - getColumn(tr.positions, 0);
    const float3x2 tc = transpose(tr.layerTexCoord[0]);
    const float2 du1 = tc[1] - tc[0];
    const float2 du2 = tc[2] - tc[0];
    const float uvDet = du1.x * du2.y - du2.x * du1.y;
    if (!(abs(uvDet) > 1e-10))
    {
        outEffScale = 0.0;
        return false;
    }

    const float invDet = 1.0 / uvDet;
    const float3 dPdu = (e1 * du2.y - e2 * du1.y) * invDet;
    const float3 dPdv = (e2 * du1.x - e1 * du2.x) * invDet;
    const float metersPerU = max(length(dPdu), 1e-3);
    const float metersPerV = max(length(dPdv), 1e-3);
    const float avgTileMeters = sqrt(metersPerU * metersPerV);

    outMaxWorldDepth = outEffScale * avgTileMeters;
    outMaxDeltaUV = (-outViewTS.xy / max(outViewTS.z, 0.25)) * outMaxWorldDepth / float2(metersPerU, metersPerV);
    const float deltaLen = length(outMaxDeltaUV);
    if (deltaLen > 0.35)
    {
        const float clampRatio = 0.35 / deltaLen;
        outMaxDeltaUV *= clampRatio;
        outMaxWorldDepth *= clampRatio;
    }
    return true;
}
#endif

#if defined(HITINFO_INL_PRIM)
void computeParallaxOcclusionPrimary(
    const ShTriangle tr,
    const float3 hitPosition,
    const float3 normalGeom,
    const float3 rayOrigin,
    const float2 dTdx0,
    const float2 dTdy0,
    inout float2 uv,
    out float3 outToCam,
    out float outPomRayDist,
    out float outEffScale,
    out float outHitHeight)
{
    outPomRayDist = 0.0;
    outHitHeight = 1.0;

    uint nrmTex = MATERIAL_NO_TEXTURE;
    float3 viewTS = float3(0.0, 0.0, 1.0);
    float maxWorldDepth = 0.0;
    float2 maxDeltaUV = (float2)0.0;
    if (!setupParallaxGeometry(tr, hitPosition, normalGeom, rayOrigin, nrmTex, outEffScale, outToCam, viewTS, maxWorldDepth, maxDeltaUV))
    {
        return;
    }

    const float2 baseUV = uv;
    const int numLayers = (int)round(lerp(24.0, 8.0, saturate(viewTS.z)));
    float2 prevUV = baseUV;
    float prevDepth = 0.0;
    float2 curUV = baseUV;
    float curDepth = 0.0;
    float hVal = getTextureSampleGrad(nrmTex, baseUV, dTdx0, dTdy0).a;
    int hitStep = -1;

    for (int i = 0; i <= numLayers; i++)
    {
        curDepth = (i == numLayers) ? 1.0 : ((float)i / (float)numLayers);
        curUV = baseUV + maxDeltaUV * curDepth;
        hVal = getTextureSampleGrad(nrmTex, curUV, dTdx0, dTdy0).a;
        const float surfDepth = 1.0 - hVal;
        if (curDepth >= surfDepth)
        {
            hitStep = i;
            break;
        }
        prevUV = curUV;
        prevDepth = curDepth;
    }

    float hitDepth = curDepth;
    if (hitStep > 0)
    {
        float2 loUV = prevUV;
        float2 hiUV = curUV;
        float loD = prevDepth;
        float hiD = curDepth;
        [unroll]
        for (int b = 0; b < 5; b++)
        {
            const float2 midUV = 0.5 * (loUV + hiUV);
            const float midD = 0.5 * (loD + hiD);
            const float midH = getTextureSampleGrad(nrmTex, midUV, dTdx0, dTdy0).a;
            if (midD >= (1.0 - midH))
            {
                hiUV = midUV;
                hiD = midD;
            }
            else
            {
                loUV = midUV;
                loD = midD;
            }
        }
        uv = 0.5 * (loUV + hiUV);
        hitDepth = 0.5 * (loD + hiD);
        outHitHeight = getTextureSampleGrad(nrmTex, uv, dTdx0, dTdy0).a;
    }
    else if (hitStep == 0)
    {
        uv = baseUV;
        hitDepth = 0.0;
        outHitHeight = hVal;
    }
    else
    {
        uv = baseUV + maxDeltaUV;
        hitDepth = 1.0;
        outHitHeight = hVal;
    }

    const float pomWorldDepth = maxWorldDepth * hitDepth;
    outPomRayDist = pomWorldDepth / max(viewTS.z, 0.25);
}

float computeParallaxCavityAO(
    const uint nrmTex,
    const float2 uv,
    const float2 dTdx0,
    const float2 dTdy0,
    const float effScale,
    const float hitHeight)
{
    if (!(effScale > 1e-4) || nrmTex == MATERIAL_NO_TEXTURE)
    {
        return 1.0;
    }

    const float2 texel = 1.0 / max((float2)getTextureSize(nrmTex, 0), (float2)1.0);
    static const float2 kOffsets[8] = {
        float2( 1.5,  0.0), float2(-1.5,  0.0), float2( 0.0,  1.5), float2( 0.0, -1.5),
        float2( 2.5,  2.5), float2(-2.5,  2.5), float2( 2.5, -2.5), float2(-2.5, -2.5)
    };
    float occ = 0.0;
    [unroll]
    for (int k = 0; k < 8; k++)
    {
        const float nh = getTextureSampleGrad(nrmTex, uv + kOffsets[k] * texel, dTdx0, dTdy0).a;
        occ += max(0.0, nh - hitHeight);
    }
    occ *= 0.125;
    return clamp(1.0 - occ * saturate(effScale * 18.0) * 1.35, 0.35, 1.0);
}
#endif

#if defined(HITINFO_INL_RFL)
void computeParallaxOcclusionRefl(
    const ShTriangle tr,
    const float3 hitPosition,
    const float3 normalGeom,
    const float3 rayOrigin,
    const DerivativeSet derivSet,
    inout float2 uv)
{
    uint nrmTex = MATERIAL_NO_TEXTURE;
    float effScale = 0.0;
    float3 toCam = (float3)0.0;
    float3 viewTS = float3(0.0, 0.0, 1.0);
    float maxWorldDepth = 0.0;
    float2 maxDeltaUV = (float2)0.0;
    if (!setupParallaxGeometry(tr, hitPosition, normalGeom, rayOrigin, nrmTex, effScale, toCam, viewTS, maxWorldDepth, maxDeltaUV))
    {
        return;
    }

    const float2 baseUV = uv;
    const int numLayers = 8;
    float2 prevUV = baseUV;
    float prevDepth = 0.0;
    float2 curUV = baseUV;
    float curDepth = 0.0;
    int hitStep = -1;

    for (int i = 0; i <= numLayers; i++)
    {
        curDepth = (i == numLayers) ? 1.0 : ((float)i / (float)numLayers);
        curUV = baseUV + maxDeltaUV * curDepth;
        const float hVal = getTextureSampleDerivSet(nrmTex, curUV, derivSet, 0).a;
        if (curDepth >= (1.0 - hVal))
        {
            hitStep = i;
            break;
        }
        prevUV = curUV;
        prevDepth = curDepth;
    }

    if (hitStep > 0)
    {
        float2 loUV = prevUV;
        float2 hiUV = curUV;
        float loD = prevDepth;
        float hiD = curDepth;
        [unroll]
        for (int b = 0; b < 3; b++)
        {
            const float2 midUV = 0.5 * (loUV + hiUV);
            const float midD = 0.5 * (loD + hiD);
            const float midH = getTextureSampleDerivSet(nrmTex, midUV, derivSet, 0).a;
            if (midD >= (1.0 - midH))
            {
                hiUV = midUV;
                hiD = midD;
            }
            else
            {
                loUV = midUV;
                loD = midD;
            }
        }
        uv = 0.5 * (loUV + hiUV);
    }
    else if (hitStep == 0)
    {
        uv = baseUV;
    }
    else
    {
        uv = baseUV + maxDeltaUV;
    }
}
#endif


#if defined(HITINFO_INL_PRIM)

ShHitInfo getHitInfoPrimaryRay(
    const ShPayload pl,
    const float3 rayOrigin, const float3 rayDirAX, const float3 rayDirAY,
    out float2 motion, out float motionDepthLinear,
    out float3 gradDepth, out float depthNDC, out float depthLinear,
    out float screenEmission,
    out uint emissionBlendCode)

#elif defined(HITINFO_INL_RFL)

ShHitInfo getHitInfoWithRayCone_ReflectionRefraction(
    const ShPayload pl, const RayCone rayCone,
    const float3 rayOrigin, const float3 rayDir, const float3 viewDir,
    inout float3 virtualPosForMotion,
    out float rayLen,
    out float2 motion, out float motionDepthLinear,
    out float screenEmission,
    out uint emissionBlendCode,
    float roughnessBlur)

#elif defined(HITINFO_INL_INDIR)

ShHitInfo getHitInfoBounce(
    const ShPayload pl, const float3 rayOrigin, float originRoughness, float bounceMipBias)

#endif
{
    ShHitInfo h;
    h.transparency = 1.0f;
    h.glassParams = (float2)0.0f;
    h.glassColor = (float3)1.0f;
    h.subsurface = (float4)0.0f;

    int instanceId, instCustomIndex;
    int geomIndex, primIndex;

    unpackInstanceIdAndCustomIndex(pl.instIdAndIndex, instanceId, instCustomIndex);
    unpackGeometryAndPrimitiveIndex(pl.geomAndPrimIndex, geomIndex, primIndex);

    const ShTriangle tr = getTriangle(instanceId, instCustomIndex, geomIndex, primIndex);

    const float2 inBaryCoords = pl.baryCoords;
    const float3 baryCoords = float3(1.0f - inBaryCoords.x - inBaryCoords.y, inBaryCoords.x, inBaryCoords.y);

    float2 texCoords[3] =
    {
        getSurfaceTexCoord(tr.geometryInstanceFlags, mul(tr.layerTexCoord[0], baryCoords)),
        mul(tr.layerTexCoord[1], baryCoords),
        mul(tr.layerTexCoord[2], baryCoords)
    };

    h.hitPosition = mul(tr.positions, baryCoords);
    h.glassParams = tr.materialColors[2].xy;
    if ((tr.geometryInstanceFlags & GEOM_INST_FLAG_MEDIA_TYPE_GLASS) != 0)
    {
        /* The pane's tint rides the second layer's colour words, which a glass
           surface's unused second texture layer leaves free. */
        h.glassColor = tr.materialColors[1].xyz;
    }

    if( ( tr.geometryInstanceFlags & GEOM_INST_FLAG_EXACT_NORMALS ) == 0 )
    {
        h.normalGeom = normalize( mul(tr.normals, baryCoords) );
    }
    else
    {
        h.normalGeom = safeNormalize(
            cross( getColumn(tr.positions, 1) - getColumn(tr.positions, 0),
                   getColumn(tr.positions, 2) - getColumn(tr.positions, 0) ) );

        if( dot( h.normalGeom, h.hitPosition - rayOrigin ) > 0 )
        {
            h.normalGeom *= -1;
        }
    }


#if defined(HITINFO_INL_PRIM)
    const float3 baryCoordsAX = intersectRayTriangle(tr.positions, rayOrigin, rayDirAX);
    const float3 baryCoordsAY = intersectRayTriangle(tr.positions, rayOrigin, rayDirAY);

    const float2 dTdx[3] =
    {
        (getSurfaceTexCoord(tr.geometryInstanceFlags, mul(tr.layerTexCoord[0], baryCoordsAX)) - texCoords[0]),
        (mul(tr.layerTexCoord[1], baryCoordsAX) - texCoords[1]),
        (mul(tr.layerTexCoord[2], baryCoordsAX) - texCoords[2])
    };

    const float2 dTdy[3] =
    {
        (getSurfaceTexCoord(tr.geometryInstanceFlags, mul(tr.layerTexCoord[0], baryCoordsAY)) - texCoords[0]),
        (mul(tr.layerTexCoord[1], baryCoordsAY) - texCoords[1]),
        (mul(tr.layerTexCoord[2], baryCoordsAY) - texCoords[2])
    };

    float3 pomToCam = (float3)0.0;
    float pomRayDist = 0.0;
    float pomEffScale = 0.0;
    float pomHitHeight = 1.0;
    computeParallaxOcclusionPrimary(
        tr, h.hitPosition, h.normalGeom, rayOrigin,
        dTdx[0], dTdy[0], texCoords[0],
        pomToCam, pomRayDist, pomEffScale, pomHitHeight);

    const float3 pomDepthShift = pomToCam * pomRayDist;
    const float3 pomPosCur = h.hitPosition - pomDepthShift;
    const float3 pomPosPrev = mul(tr.prevPositions, baryCoords) - pomDepthShift;

    const float4 viewSpacePosCur   = mul(globalUniform.view, float4(pomPosCur, 1.0));
    const float4 viewSpacePosPrev  = mul(globalUniform.viewPrev, float4(pomPosPrev, 1.0));
    const float4 viewSpacePosAX    = mul(globalUniform.view, float4(mul(tr.positions, baryCoordsAX) - pomDepthShift, 1.0));
    const float4 viewSpacePosAY    = mul(globalUniform.view, float4(mul(tr.positions, baryCoordsAY) - pomDepthShift, 1.0));

    const float4 clipSpacePosCur   = mul(globalUniform.projection, viewSpacePosCur);
    const float4 clipSpacePosPrev  = mul(globalUniform.projectionPrev, viewSpacePosPrev);

    const float clipSpaceDepth   = clipSpacePosCur[2];
    const float clipSpaceDepthAX = dot(getColumn(globalUniform.projection, 2), viewSpacePosAX);
    const float clipSpaceDepthAY = dot(getColumn(globalUniform.projection, 2), viewSpacePosAY);

    const float3 ndcCur            = clipSpacePosCur.xyz  / clipSpacePosCur.w;
    const float3 ndcPrev           = clipSpacePosPrev.xyz / clipSpacePosPrev.w;

    const float2 screenSpaceCur    = ndcCur.xy  * 0.5 + 0.5;
    const float2 screenSpacePrev   = ndcPrev.xy * 0.5 + 0.5;
#endif


#if defined(HITINFO_INL_RFL)
    rayLen = length(h.hitPosition - rayOrigin);
#endif


#if defined(HITINFO_INL_RFL)
    virtualPosForMotion += viewDir * rayLen;

    const float4 viewSpacePosCur   = mul(globalUniform.view, float4(virtualPosForMotion, 1.0));
    const float4 viewSpacePosPrev  = mul(globalUniform.viewPrev, float4(virtualPosForMotion, 1.0));
    const float4 clipSpacePosCur   = mul(globalUniform.projection, viewSpacePosCur);
    const float4 clipSpacePosPrev  = mul(globalUniform.projectionPrev, viewSpacePosPrev);
    const float3 ndcCur            = clipSpacePosCur.xyz  / clipSpacePosCur.w;
    const float3 ndcPrev           = clipSpacePosPrev.xyz / clipSpacePosPrev.w;
    const float2 screenSpaceCur    = ndcCur.xy  * 0.5 + 0.5;
    const float2 screenSpacePrev   = ndcPrev.xy * 0.5 + 0.5;

    const float clipSpaceDepth   = clipSpacePosCur[2];
#endif


#if defined(HITINFO_INL_PRIM)
    depthNDC = ndcCur.z;
    depthLinear = length(viewSpacePosCur.xyz);
#endif


#if defined(HITINFO_INL_PRIM) || defined(HITINFO_INL_RFL)
    motion = (screenSpacePrev - screenSpaceCur);
#endif


#if defined(HITINFO_INL_PRIM)
    motionDepthLinear = length(viewSpacePosPrev.xyz) - depthLinear;
#elif defined(HITINFO_INL_RFL)
    motionDepthLinear = length(viewSpacePosPrev.xyz) - length(viewSpacePosCur.xyz);
#endif


#if defined(HITINFO_INL_PRIM)
    gradDepth = float3(
        clipSpaceDepthAX - clipSpaceDepth,
        clipSpaceDepthAY - clipSpaceDepth,
        abs(length(viewSpacePosAX.xyz) - depthLinear) + abs(length(viewSpacePosAY.xyz) - depthLinear));
#elif defined(HITINFO_INL_RFL)
#endif


#if defined(HITINFO_INL_PRIM)
    h.albedo = processAlbedoGrad(
        tr.geometryInstanceFlags, texCoords,
            tr.materials, tr.materialColors,
            dTdx, dTdy);
    if (pomEffScale > 1e-4)
    {
        h.albedo *= computeParallaxCavityAO(
            tr.materials[0][MATERIAL_NORMAL_INDEX],
            texCoords[0],
            dTdx[0],
            dTdy[0],
            pomEffScale,
            pomHitHeight);
    }
#endif


#if defined(HITINFO_INL_RFL)
    /* The roughness of the surface the ray left widens the cone over the
       segment: the hit is sampled at the mip a frosted pane would blur it to. */
    RayCone blurredCone = rayCone;
    blurredCone.width += roughnessBlur * rayLen;

    DerivativeSet derivSet = getTriangleUVDerivativesFromRayCone(tr, h.normalGeom, blurredCone, rayDir);

    computeParallaxOcclusionRefl(
        tr, h.hitPosition, h.normalGeom, rayOrigin,
        derivSet, texCoords[0]);

    h.albedo = processAlbedoRayConeDeriv(
        tr.geometryInstanceFlags, texCoords,
        tr.materials, tr.materialColors,
        derivSet);
#endif


#if defined(HITINFO_INL_INDIR)
    const float viewDist = length(h.hitPosition - globalUniform.cameraPosition.xyz);
    const float hitDistance = length(h.hitPosition - rayOrigin);

    const float lod = getBounceLOD(originRoughness, viewDist, hitDistance, globalUniform.renderWidth, bounceMipBias);

    h.albedo = processAlbedo(tr.geometryInstanceFlags, texCoords, tr.materials, tr.materialColors, lod);
#endif


    if (tr.materials[0][MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX] != MATERIAL_NO_TEXTURE)
    {
        float4 rme =
    #if defined(HITINFO_INL_PRIM)
            getTextureSampleGrad(tr.materials[0][MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX], texCoords[0], dTdx[0], dTdy[0]);
    #elif defined(HITINFO_INL_RFL)
            getTextureSampleDerivSet(tr.materials[0][MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX], texCoords[0], derivSet, 0);
    #elif defined(HITINFO_INL_INDIR)
            getTextureSampleLod(tr.materials[0][MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX], texCoords[0], lod);
    #endif

    #if defined(HITINFO_INL_PRIM) || defined(HITINFO_INL_RFL)
        if( globalUniform.emissionSharpMask != 0.0 )
        {
            const uint  rmeTexture = tr.materials[ 0 ][ MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX ];
        #if defined(HITINFO_INL_PRIM)
            const float2 rmeFootX  = dTdx[ 0 ];
            const float2 rmeFootY  = dTdy[ 0 ];
        #else
            const float2 rmeFootX  = float2( derivSet.u[ 0 ], 0.0 );
            const float2 rmeFootY  = float2( 0.0, derivSet.u[ 0 ] );
        #endif
            const int2   rmeTexSize = getTextureSize( rmeTexture, 0 );
            const float2 rmeFpx     = rmeFootX * (float2)rmeTexSize;
            const float2 rmeFpy     = rmeFootY * (float2)rmeTexSize;
            if( max( dot( rmeFpx, rmeFpx ), dot( rmeFpy, rmeFpy ) ) < 1.0 )
            {
                const float2 snappedUv = ( floor( texCoords[ 0 ] * (float2)rmeTexSize ) + (float2)0.5 ) / (float2)rmeTexSize;
                rme.b = getTextureSampleLod( rmeTexture, snappedUv, 0.0 ).b;
            }
        }
    #endif

        h.roughness = rme[ 0 ];
        h.metallic  = rme[ 1 ];
        h.emission  = rme[ 2 ];

    #if defined(HITINFO_INL_PRIM)
        const bool skipRoughReg =
            (tr.geometryInstanceFlags & (GEOM_INST_FLAG_REFLECT | GEOM_INST_FLAG_REFRACT |
                                         GEOM_INST_FLAG_MEDIA_TYPE_GLASS | GEOM_INST_FLAG_MEDIA_TYPE_WATER)) != 0u
            || h.roughness < globalUniform.minRoughness;
        if (!skipRoughReg && (any(dTdx[0] != 0.0) || any(dTdy[0] != 0.0)))
        {
            const uint rmeTex = tr.materials[0][MATERIAL_ROUGHNESS_METALLIC_EMISSION_INDEX];
            const int2 rmeSize = getTextureSize(rmeTex, 0);
            const float2 rmePxX = dTdx[0] * (float2)rmeSize;
            const float2 rmePxY = dTdy[0] * (float2)rmeSize;
            const float rmeFootprint = sqrt(max(dot(rmePxX, rmePxX), dot(rmePxY, rmePxY)));
            const float rmeSubpixel = saturate((rmeFootprint - 1.0) * 0.5);
            if (rmeSubpixel > 0.0)
            {
                const float r_dx = getTextureSampleGrad(rmeTex, texCoords[0] + dTdx[0], dTdx[0], dTdy[0]).r;
                const float r_dy = getTextureSampleGrad(rmeTex, texCoords[0] + dTdy[0], dTdx[0], dTdy[0]).r;
                const float alpha0 = square(max(h.roughness, MIN_GGX_ROUGHNESS));
                const float alphaDx = square(max(r_dx, MIN_GGX_ROUGHNESS));
                const float alphaDy = square(max(r_dy, MIN_GGX_ROUGHNESS));
                const float varAlpha = 0.25 * (square(alphaDx - alpha0) + square(alphaDy - alpha0));
                const float alphaReg = sqrt(clamp(square(alpha0) + 0.15 * rmeSubpixel * varAlpha, 0.0, 1.0));
                h.roughness = clamp(max(h.roughness, sqrt(alphaReg)), MIN_GGX_ROUGHNESS, 1.0);
            }
        }
    #endif

    #if defined(HITINFO_INL_PRIM) || defined(HITINFO_INL_RFL)
        emissionBlendCode = uint( rme.a * 255.0 + 0.5 );
    #endif

    }
    else
    {
        h.roughness = tr.geomRoughness;
        h.metallic  = tr.geomMetallicity;
        h.emission  = tr.geomEmission;

    #if defined(HITINFO_INL_PRIM) || defined(HITINFO_INL_RFL)
        emissionBlendCode = 0u;
    #endif
    }

    // Keep G-buffer roughness in perceptual space for Q2RTX ASVGF and light-sampling
    // passes; GGX BRDF entry points convert perceptual roughness to alpha = r^2 internally.
    h.roughness = max( h.roughness, MIN_GGX_ROUGHNESS );

    float lightStyleEmissionScale = 1.0;
    for( int i = 0; i < 4; i++ )
    {
        const uint styleByte = ( tr.lightStyleIndices >> ( i * 8 ) ) & 0xFFu;
        if( styleByte == 0u )
            break;
        const uint  style      = styleByte - 1u;
        const float styleValue = globalUniform.lightStyleScales[ style / 4u ][ style % 4u ];
        if( styleValue < ( 255.5 / 256.0 ) )
            lightStyleEmissionScale = min( lightStyleEmissionScale, styleValue );
    }
    h.emission *= lightStyleEmissionScale;

#if defined( HITINFO_INL_PRIM ) || defined( HITINFO_INL_RFL )
    screenEmission = rmeEmissionToScreenEmission( h.emission );
#endif
    h.emission *= globalUniform.emissionMapBoost;


#if !defined(HITINFO_INL_INDIR)
    float sssMaskAlpha = 1.0f;
    if (tr.materials[0][MATERIAL_NORMAL_INDEX] != MATERIAL_NO_TEXTURE)
    {
        const bool isGlassNormal = (tr.geometryInstanceFlags & GEOM_INST_FLAG_MEDIA_TYPE_GLASS) != 0u;
        const float suppressDetails = 1.0;

        const float4 nrmSample =
    #if defined(HITINFO_INL_PRIM)
            getTextureSampleGrad(tr.materials[0][MATERIAL_NORMAL_INDEX], texCoords[0], dTdx[0] * suppressDetails, dTdy[0] * suppressDetails)
    #elif defined(HITINFO_INL_RFL)
            getTextureSampleDerivSet(tr.materials[0][MATERIAL_NORMAL_INDEX], texCoords[0], derivSet, 0)
    #endif
            ;

        float2 nrm = nrmSample.xy;
        // the normal map's alpha carries the material's glass transparency (or POM height on opaque surfaces)
        if (isGlassNormal)
        {
            h.transparency = nrmSample.a;
        }
    #if defined(HITINFO_INL_PRIM)
        if ((tr.geometryInstanceFlags & GEOM_INST_FLAG_SUBSURFACE) != 0u)
        {
            sssMaskAlpha = getTextureSampleGrad(tr.materials[0][MATERIAL_NORMAL_INDEX], texCoords[0], dTdx[0], dTdy[0]).a;
        }
        else
        {
            sssMaskAlpha = nrmSample.a;
        }
    #else
        sssMaskAlpha = nrmSample.a;
    #endif
        nrm.xy = (nrmSample.xy * 255.0 - 128.0) / 127.0;

        const float3 bitangent = cross(h.normalGeom, tr.tangent.xyz) * tr.tangent.w;
        if (isGlassNormal)
        {
            float3 mappedNormal = (nrmSample.xyz * 255.0 - 128.0) / 127.0;
            mappedNormal.z = max(mappedNormal.z, 0.01);
            h.normal = safeNormalize(tr.tangent.xyz * mappedNormal.x + bitangent * mappedNormal.y + h.normalGeom * mappedNormal.z);
            h.normal = safeNormalize(lerp(h.normalGeom, h.normal, globalUniform.normalMapStrength));
        }
        else
        {
            const float nrmLenSq = dot(nrm.xy, nrm.xy);
            const float nrmZ = sqrt(max(0.02, 1.0 - min(nrmLenSq, 0.98)));
            float2 scaledSlope = (nrm.xy / nrmZ) * globalUniform.normalMapStrength;
            const float slopeLen = length(scaledSlope);
            if (slopeLen > 3.5)
            {
                scaledSlope *= (3.5 / slopeLen);
            }
            h.normal = safeNormalize(tr.tangent.xyz * scaledSlope.x + bitangent * scaledSlope.y + h.normalGeom);
            const float3 toCam = safeNormalize(rayOrigin - h.hitPosition);
            const float geomNdotV = dot(h.normalGeom, toCam);
            const float shadeNdotV = dot(h.normal, toCam);
            if (geomNdotV > 0.05 && shadeNdotV < 0.05)
            {
                const float t = saturate((0.05 - shadeNdotV) / max(geomNdotV - shadeNdotV, 1e-4));
                h.normal = safeNormalize(lerp(h.normal, h.normalGeom, t));
            }
        }

    #if defined(HITINFO_INL_PRIM)
        const bool skipNormalRoughReg =
            (tr.geometryInstanceFlags & (GEOM_INST_FLAG_REFLECT | GEOM_INST_FLAG_REFRACT |
                                         GEOM_INST_FLAG_MEDIA_TYPE_GLASS | GEOM_INST_FLAG_MEDIA_TYPE_WATER)) != 0u
            || h.roughness < globalUniform.minRoughness;
        if (!skipNormalRoughReg && globalUniform.normalMapStrength > 0.0 && (any(dTdx[0] != 0.0) || any(dTdy[0] != 0.0)))
        {
            // Toksvig/LEADR bump-to-roughness regularization: apply only when normal texels are
            // minified below pixel resolution so resolved close-up bump details keep crisp gloss.
            const uint nrmTex = tr.materials[0][MATERIAL_NORMAL_INDEX];
            const int2 nrmSize = getTextureSize(nrmTex, 0);
            const float2 nrmPxX = dTdx[0] * (float2)nrmSize;
            const float2 nrmPxY = dTdy[0] * (float2)nrmSize;
            const float nrmFootprint = sqrt(max(dot(nrmPxX, nrmPxX), dot(nrmPxY, nrmPxY)));
            const float nrmSubpixel = saturate((nrmFootprint - 1.0) * 0.5);
            if (nrmSubpixel > 0.0)
            {
                const float2 nrmFine0 = nrm.xy;
                const float2 nrmFineDx = (getTextureSampleGrad(nrmTex, texCoords[0] + dTdx[0], dTdx[0], dTdy[0]).xy * 255.0 - 128.0) / 127.0;
                const float2 nrmFineDy = (getTextureSampleGrad(nrmTex, texCoords[0] + dTdy[0], dTdx[0], dTdy[0]).xy * 255.0 - 128.0) / 127.0;
                const float2 diffDx = nrmFineDx - nrmFine0;
                const float2 diffDy = nrmFineDy - nrmFine0;
                const float toksvigStrength = min(globalUniform.normalMapStrength, 1.5);
                const float slopeVar = square(toksvigStrength) *
                                       0.25 * (dot(diffDx, diffDx) + dot(diffDy, diffDy));
                const float regGain = 0.08 * nrmSubpixel;
                const float alpha0 = square(h.roughness);
                const float alphaReg = sqrt(clamp(square(alpha0) + regGain * slopeVar, 0.0, 1.0));
                h.roughness = clamp(max(h.roughness, sqrt(alphaReg)), MIN_GGX_ROUGHNESS, 1.0);
            }
        }
    #endif
    }
    else
#endif
    {
        h.normal = h.normalGeom;
    }

    h.instCustomIndex = instCustomIndex;
    h.geometryInstanceFlags = tr.geometryInstanceFlags;
    h.portalIndex = tr.portalIndex;

    h.cluster = tr.cluster;

#if !defined(HITINFO_INL_INDIR)
    if ((tr.geometryInstanceFlags & GEOM_INST_FLAG_SUBSURFACE) != 0u)
    {
        h.subsurface = buildSubsurfaceGBuffer(tr.subsurfacePacked, sssMaskAlpha);
    }
#endif

    return h;
}

#endif
#endif
#endif
