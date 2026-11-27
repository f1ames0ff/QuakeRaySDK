// Copyright (C) 2018 Christoph Schied
// Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.
// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This file is a port of shader/indirect_lighting.rgen from Quake 2 RTX (https://github.com/NVIDIA/Q2RTX),
// which is distributed under the terms of the GNU General Public License
// version 2.  It has been adapted to the renderer interface of this project.
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
// Indirect lighting (one or two bounces) of the Q2RTX-style path tracer.
//
// HLSL counterpart of GLSL/RtQ2Indirect.rgen, the indirect raygen: the first-bounce specular ray
// with the sky-NEE weight, the cluster light NEE of the hit, the sun bounce with its range
// attenuation, the optional second diffuse bounce, and the three temporal outputs. The body is a
// statement by statement port into the already ported headers -- RaygenCommon.hlsli (the bounce
// and shadow ray casts, fetch, shadeDiffuse, boostChroma, getSkyAmbientMultiplied, evalSkyNeePdf,
// rayStatsAdd), Q2LightLists.hlsli (q2SampleClusterLights, q2AccumulateLightStats,
// q2ClusterSeesSky, q2GetIsGradient, q2RoughnessSquareToSpecPower), Q2Asvgf.hlsli
// (SKY_MIP_COUNT arrives from RaygenCommon.hlsli, the gradient constants from here),
// Light.hlsli, Random.hlsli, HitInfo.hlsli (getHitInfoBounce), Surface.hlsli
// (hitInfoToSurface_Indirect) and BRDF.hlsli.
//
// This file is one of the two raygen files of the base that define their own main(); the
// [shader("raygeneration")] attribute below is what makes it the OpEntryPoint of the lib_6_3
// module, and the entry stays named main, so the blob name and the host are untouched.
//
// Spellings that had to change:
//   * `layout (constant_id = 0) const uint maxAlbedoLayerCount = 0` ->
//     `[[vk::constant_id(0)]] const uint maxAlbedoLayerCount = 0`: the same SpecId 0, uint and
//     default 0 (the mechanism the accepted RaygenPrimary port and EfSimple.hlsli use).
//   * ivec2 -> int2, vec3 -> float3, vec2 -> float2, vec4 -> float4, gl_LaunchIDEXT.xy ->
//     DispatchRaysIndex().xy, i.e. the golden's ivec2(gl_LaunchIDEXT.xy) becomes the truncating
//     (int2) cast of the uint3 the intrinsic hands back, as in RaygenPrimary.hlsli.
//   * imageStore(img, pix, v) -> img[pix] = v, and texelFetch(t, pix, 0) -> t.Load(int3(pix, 0)).
//   * textureSize(framebufQ2RngSeed_Sampled, 0).y -> the seeded texture's own GetDimensions: HLSL
//     has no textureSize intrinsic, the size is a method of the texture and it hands back uints,
//     so the int pix.y comparison takes a (int) cast. Every framebuffer height of this renderer
//     is far below 2^31, so that cast is exact where the golden's ivec2 component is already an
//     int. This is the only site of the file that is not a one to one re-spelling.
//   * vecN(scalar) -> (floatN)scalar (the golden's explicit zero constants; the one three component
//     constructor of the file, float3( 10.0, 0.0, 0.0 ), is copied unchanged) and the scalar
//     conversions of the golden become C style casts of the same truncating conversion:
//     uint(x) -> (uint)x, int(x) -> (int)x, float(x) -> (float)x.
//   * mix -> lerp, atan(y, x) -> atan2(y, x), and every matrix site is inside the ported headers:
//     this file has no product, no index and no constructor of a matrix in its own text.
//
// What did not change: the whole GPL header, every #define and its value (the five RNG salts, the
// two bounce mip biases, Q2_INDIR_MAX_SPHERE_SOLID_ANGLE and the five rt_debugflags bits with
// their comment), the DESC_SET_* block and the LIGHT_SAMPLE_METHOD pin (INDIR), the order of the
// eleven functions, indirDbg / indirMaxSphereSolidAngle / indirSphotGain as the debug gates,
// every early out (the giBounceRays.x < 0.25 zero store, the half-res row out, the sky store, the
// sky NEE gate of sampleLambertian and of the luminance test), the RNG streams and their exact
// salts (Q2_RNG_INDIR_CELL_SELECT + 0/1/2u, Q2_RNG_INDIR_LIGHT_POINT, Q2_RNG_INDIR2_SUN_DISK,
// Q2_RNG_INDIR_SUN_DISK, Q2_RNG_INDIR_SKY_NEE, RANDOM_SALT_SPEC_BOUNCE(1), RANDOM_SALT_DIFF_BOUNCE(2)),
// the 0.99 of every random disk, the order of every accumulation, the 50.0 clamp, the 50.0
// second-bounce debug gain that is applied after that clamp, the skyBsdfWeight of the sky branch,
// the half-res scale and its site, the specular demodulation against the accumulated direct
// specular of the pixel, and the sanity store of the red SH.

[[vk::constant_id(0)]] const uint maxAlbedoLayerCount = 0;
#define MATERIAL_MAX_ALBEDO_LAYERS maxAlbedoLayerCount
#define DESC_SET_TLAS 0
#define DESC_SET_FRAMEBUFFERS 1
#define DESC_SET_GLOBAL_UNIFORM 2
#define DESC_SET_VERTEX_DATA 3
#define DESC_SET_TEXTURES 4
#define DESC_SET_RANDOM 5
#define DESC_SET_LIGHT_SOURCES 6
#define DESC_SET_CUBEMAPS 7
#define DESC_SET_RENDER_CUBEMAP 8
#define DESC_SET_RAY_STATS 11
#define DESC_SET_CLOUD_SHADOW 12
#define LIGHT_SAMPLE_METHOD (LIGHT_SAMPLE_METHOD_INDIR)
#include "RaygenCommon.hlsli"
#include "Q2Asvgf.hlsli"
#include "Q2LightLists.hlsli"

#define Q2_RNG_INDIR_CELL_SELECT 220
#define Q2_RNG_INDIR_LIGHT_POINT 223
#define Q2_RNG_INDIR_LIGHT_MEMBER 226
#define Q2_RNG_INDIR_SUN_DISK    225
// The first-bounce salts are used by the first-bounce light NEE. The 230/233 pair is
// reserved for a second-bounce light NEE, which Q2RTX does not have and which this pass
// therefore no longer traces (only 235, the second-bounce sun disk, is live).
#define Q2_RNG_INDIR2_CELL_SELECT 230
#define Q2_RNG_INDIR2_LIGHT_POINT 233
#define Q2_RNG_INDIR2_SUN_DISK    235
#define Q2_RNG_INDIR_SKY_NEE      240

#define Q2_INDIR_MAX_SPHERE_SOLID_ANGLE 0.02

// Q2RTX pt_sun_bounce_range / sun_bounce: the sun contribution of a bounce fades out with
// the distance the bounce ray travelled, and what is left is scaled by a straight
// multiplier. Both come from the host through ShGlobalUniform.sunBounce (cvars
// rt_sky_sun_bounce_range, 2000 in game units, and rt_sky_sun_bounce_scale, 1.0). Without the range
// every bounce hit anywhere in the map adds full sun energy and pays for its own sun shadow
// ray, which is where the salt-like noise in dark areas came from.

// Indirect-light debug probes, driven by rt_debugflags bits 15..19. Those bits sit
// above the QrDebugDrawFlagBits enum, so they reach the shader only through the
// pass-through in VulkanDevice.cpp. They have no display branch: the final image stays
// normal, which makes them usable as A/B tests of the GI estimator without a rebuild.
//   1<<15 (32768)   sphere/spot dw clamp 0.0001 - a floor, so it binds for any light
//                   closer than ~142 u (sphere radius 0.8 u) and therefore darkens
//                   almost everywhere if this clamp controls the image at all
//   1<<16 (65536)   sphere/spot dw clamp like the direct path (2*pi, no clamp at all)
//   1<<17 (131072)  sphere/spot indirect NEE gain x100 (clamp applied first, then scaled)
//   1<<18 (262144)  second bounce contribution x50 (applied after the final clamp)
//   1<<19 (524288)  SANITY: flat red indirect channel, to prove the bits arrive here
// 1<<18 is inert unless rt_gi_level is 2 (the second bounce is the high GI level).
#define INDIR_DBG_CLAMP_FLOOR   ( 1u << 15 )
#define INDIR_DBG_CLAMP_FULL    ( 1u << 16 )
#define INDIR_DBG_SPHOT_X100    ( 1u << 17 )
#define INDIR_DBG_BOUNCE2_X50   ( 1u << 18 )
#define INDIR_DBG_SANITY        ( 1u << 19 )

bool indirDbg( const uint bit )
{
    return ( globalUniform.debugShowFlags & bit ) != 0u;
}

float indirMaxSphereSolidAngle()
{
    if ( indirDbg( INDIR_DBG_CLAMP_FULL ) )
    {
        return 2.0 * M_PI;
    }
    if ( indirDbg( INDIR_DBG_CLAMP_FLOOR ) )
    {
        return 0.0001;
    }
    return Q2_INDIR_MAX_SPHERE_SOLID_ANGLE;
}

// Debug gain for the sphere/spot indirect NEE contribution. light.dw enters shade()
// linearly (diffuse = dw * nl * color * brdf * oneOverPdf), so scaling it is a clean
// multiplicative gain on exactly the light types the clamp above applies to.
float indirSphotGain()
{
    return indirDbg( INDIR_DBG_SPHOT_X100 ) ? 100.0 : 1.0;
}

float3 getSpecularBounce(const uint seed, uint bounceIndex,
                         const float3 n, const float roughness, const float3 surfSpecularColor,
                         const float3 v,
                         out float oneOverSourcePdf)
{
    const float2 u = rndBlueNoise8( seed, RANDOM_SALT_SPEC_BOUNCE( bounceIndex ) ).xy;
    return sampleSmithGGX( n, v, roughness, u[ 0 ], u[ 1 ], oneOverSourcePdf );
}

float3 getDiffuseBounce(const uint seed, uint bounceIndex,
                        const float3 n, out float oneOverSourcePdf)
{
    const float2 u = rndBlueNoise8( seed, RANDOM_SALT_DIFF_BOUNCE( bounceIndex ) ).xy;
    return sampleLambertian( n, u[ 0 ], u[ 1 ], oneOverSourcePdf );
}

// Q2RTX sun_attenuation: a smooth quartic falloff over the bounce ray's own length, so far
// away bounces neither receive sun light nor pay for a sun shadow ray. The first bounce ray
// is the specular one, so its range shrinks with the primary roughness exactly like Q2RTX
// does for `is_specular_ray`; the diffuse second bounce uses the full range.
float q2SunBounceAttenuation(float hitDistance, float sunBounceRange)
{
    // A zero range means "no indirect sun". The floor keeps the division finite so that a
    // hit at distance zero cannot turn the whole term into a NaN.
    sunBounceRange = max( sunBounceRange, 2.5e-6 );
    return square( clamp( 1.0 - square( square( hitDistance / sunBounceRange ) ), 0.0, 1.0 ) );
}

#define FIRST_BOUNCE_MIP_BIAS 0
#define SECOND_BOUNCE_MIP_BIAS 32.0

Surface traceBounce(const float3 originPosition, float originRoughness, uint originInstCustomIndex,
                    const float3 bounceDir, float bounceMipBias)
{
    const ShPayload p = traceIndirectRay( originInstCustomIndex, originPosition, bounceDir );
    rayStatsAdd( RAY_STATS_CATEGORY_INDIRECT, 1 );

    if ( !doesPayloadContainHitInfo( p ) )
    {
        Surface s;
        s.isSky = true;
        s.geometryInstanceFlags = 0u;
        return s;
    }

    return hitInfoToSurface_Indirect(
        getHitInfoBounce( p, originPosition, originRoughness, bounceMipBias ),
        bounceDir );
}

float3 q2RegularizeBounceEmission(const float3 emissionRadiance, float hitDistance,
                                  float oneOverPdf, float maxCap)
{
    // Path-space firefly regularizer: bound 1-spp bounce emission spikes using ray travel
    // distance and sample weight while leaving moderate or well-sampled emissive hits intact.
    const float lum = getLuminance(emissionRadiance);
    if (lum <= 0.0)
    {
        return (float3)0.0;
    }

    const float distScale = clamp(hitDistance, 0.35, 1.5);
    const float pdfScale = 1.0 / (1.0 + 0.15 * max(oneOverPdf - 1.0, 0.0));
    const float regCap = maxCap * distScale * pdfScale;
    if (lum <= regCap)
    {
        return emissionRadiance;
    }

    const float excess = lum - regCap;
    const float regLum = regCap + (excess * regCap) / (excess + regCap * 2.0);
    return emissionRadiance * (regLum / lum);
}

float3 processSecondDiffuseBounce(const uint seed, const Surface surf, const float3 bounceDir,
                                  float oneOverPdf)
{
    const Surface hitSurf = traceBounce(
        surf.position + surf.normalGeom * 2.5e-4,
        surf.roughness, surf.instCustomIndex, bounceDir, SECOND_BOUNCE_MIP_BIAS );

    if ( hitSurf.isSky )
    {
        return getSkyAmbientMultiplied( bounceDir, SKY_DIFFUSE_BOUNCE_LOD ) * oneOverPdf;
    }

    // Q2RTX processes analytic lights only in the first bounce pass, so the second bounce
    // of the diffuse path is lit by the sky it can reach and by the sun alone: the
    // `get_direct_illumination` call sits behind `spec_bounce_index == 0` and the light
    // list would otherwise be sampled twice for the same pixel.
    const float sunAttenuation = q2SunBounceAttenuation(
        length( hitSurf.position - surf.position ), globalUniform.sunBounce.x );

    float3 directDiffuse = (float3)0.0;

    if ( globalUniform.directionalLightExists != 0 && sunAttenuation > 0.0 )
    {
        const DirectionalLight sun =
            decodeAsDirectionalLight( lightSources[ LIGHT_ARRAY_DIRECTIONAL_LIGHT_OFFSET ] );
        const float2 sunRnd = rnd16_2( seed, Q2_RNG_INDIR2_SUN_DISK ) * 0.99;
        const LightSample sunLight = sampleDirectionalLight( sun, hitSurf.position, sunRnd );

        if ( getLuminance( sunLight.color ) > 0.0 )
        {
            bool sunTraced;
            const float3 sunVis = traceSunVisibility( hitSurf, sunLight, sunTraced );

            if ( sunTraced )
            {
                rayStatsAdd( RAY_STATS_CATEGORY_SHADOW_INDIRECT, 1 );
            }

            float3 d;
            shadeDiffuse( hitSurf, sunLight, 1.0, d );
            directDiffuse += d * sunVis * sunAttenuation * globalUniform.sunBounce.y;
        }
    }

    float3 radiance = boostChroma( directDiffuse * hitSurf.albedo );

    if ( ( hitSurf.geometryInstanceFlags & GEOM_INST_FLAG_EMISSION_RAY_ONLY ) != 0 )
    {
        const float bounceDist = length( hitSurf.position - surf.position );
        radiance += q2RegularizeBounceEmission(
            hitSurf.emission * hitSurf.albedo, bounceDist, oneOverPdf, 18.0 );
    }

    return radiance * oneOverPdf;
}

void q2ShadeIndirect(const Surface surf, const float3 hitPos, const float3 hitRadiance,
                     float oneOverPdf,
                     out float3 diffuse, out float3 specular)
{
    const float3 l  = safeNormalize( hitPos - surf.position );
    const float nl  = dot( surf.normal, l );
    const float ngl = dot( surf.normalGeom, l );

    if ( nl <= 0.0 || ngl <= 0.0 )
    {
        diffuse = specular = (float3)0.0;
        return;
    }

    const float3 radiance = boostChroma( hitRadiance );

    const float diffWeight = min( nl * evalBRDFLambertian( 1.0 ) * oneOverPdf, 4.0 );
    diffuse  = radiance * diffWeight;

    // Path-space firefly clamping for 1-spp specular: preserve crisp bright peaks on mirrors
    // while bounding high-variance emissive/radiance spikes on rougher surfaces.
    const float  specMaxRadiance = ( globalUniform.antiFireflyEnabled != 0u )
        ? lerp( 36.0, 8.0, smoothstep( 0.05, 0.40, surf.roughness ) )
        : 50.0;
    const float  specLum         = getLuminance( radiance );
    const float3 specRadiance    = ( specLum > specMaxRadiance ) ? radiance * ( specMaxRadiance / specLum ) : radiance;

    // Without FORCE_EVALBRDF_GGX_LOOSE, D_GGX cancels identically for 1-spp VNDF (<= F <= 1.0);
    // [0, 2] still bounds ReSTIR GI reused neighbor samples where oneOverPdf comes from reservoir shadingWeight.
    const float3 specWeight = clamp(
        nl * evalBRDFSmithGGX( surf.normal, surf.toViewerDir, l, surf.roughness, surf.specularColor ) * oneOverPdf,
        (float3)0.0,
        (float3)2.0 );
    specular = specRadiance * specWeight;
}

float q2SkyNeeBsdfWeight( const Surface surf, const float3 direction )
{
    if ( globalUniform.skyNee == 0.0 )
    {
        return 1.0;
    }

    const float pdfBsdf = evalSpecularBouncePdf(
        surf.normal, surf.toViewerDir, surf.roughness, direction );
    const float pdfSky = evalSkyNeePdf( surf.normal, direction );

    return pdfBsdf / max( pdfBsdf + pdfSky, 1e-8 );
}

float3 q2SkyNee( const Surface surf, const uint seed )
{
    if ( globalUniform.skyNee == 0.0 )
    {
        return (float3)0.0;
    }

    /* A cluster the host proved cannot see the sky never hits the dome: skip the ray the
       same way traceSunVisibility skips the sun shadow ray (q2ClusterSeesSky). */
    if ( !q2ClusterSeesSky( surf.cluster ) )
    {
        return (float3)0.0;
    }

    const float2 u = rnd16_2( seed, Q2_RNG_INDIR_SKY_NEE );

    float oneOverPdfSky;
    const float3 skyDirection = sampleLambertian( surf.normal, u[ 0 ], u[ 1 ], oneOverPdfSky );

    const float nl = dot( surf.normal, skyDirection );
    const float ngl = dot( surf.normalGeom, skyDirection );
    const float pdfSky = evalSkyNeePdf( surf.normal, skyDirection );

    if ( nl <= 0.0 || ngl <= 0.0 || pdfSky <= 0.0 )
    {
        return (float3)0.0;
    }

    const float pdfBsdf = evalSpecularBouncePdf(
        surf.normal, surf.toViewerDir, surf.roughness, skyDirection );
    const float misWeight = pdfSky / max( pdfSky + pdfBsdf, 1e-8 );

    const float3 radiance = boostChroma(
        getSkyAmbientMultiplied( skyDirection, surf.roughness * ( SKY_MIP_COUNT - 1.0 ) ) );

    if ( getLuminance( radiance ) * misWeight <= 0.0 )
    {
        return (float3)0.0;
    }

    const float3 vis = traceSkyVisibility( surf, skyDirection );
    rayStatsAdd( RAY_STATS_CATEGORY_SHADOW_INDIRECT, 1 );

    return evalBRDFLambertian( 1.0 ) * nl * radiance * vis * oneOverPdfSky * misWeight;
}

[shader("raygeneration")]
void main()
{
    int2 pix = (int2)DispatchRaysIndex().xy;

    if ( globalUniform.giBounceRays.x < 0.25 )
    {
        imageStoreUnfilteredIndirectSH( pix, irradianceToSH( (float3)0.0, float3( 0.0, 0.0, 1.0 ) ) );
        framebufUnfilteredIndirectSpecular[pix] = float4( 0.0, 0.0, 0.0, 0.0 );
        return;
    }

    const bool halfRes = globalUniform.giBounceRays.x < 0.75;
    if ( halfRes )
    {
        const int parity = (int)( globalUniform.frameId & 1u );
        const int2 pixOther = int2( pix.x, pix.y * 2 + ( parity ^ 1 ) );
        pix.y = pix.y * 2 + parity;

        uint seedTexWidth, seedTexHeight;
        framebufQ2RngSeed_Sampled.GetDimensions( seedTexWidth, seedTexHeight );

        if ( pixOther.y < (int)seedTexHeight )
        {
            framebufUnfilteredIndirectSpecular[pixOther] = float4( 0.0, 0.0, 0.0, 0.0 );
            imageStoreUnfilteredIndirectSH( pixOther, irradianceToSH( (float3)0.0, float3( 0.0, 0.0, 1.0 ) ) );
        }

        if ( pix.y >= (int)seedTexHeight )
        {
            return;
        }
    }

    const uint seed = framebufQ2RngSeed_Sampled.Load(int3(pix, 0)).r;

    Surface surf = fetchGbufferSurface( pix );
    surf.position += surf.toViewerDir * RAY_ORIGIN_LEAK_BIAS;

    if ( surf.isSky )
    {
        imageStoreUnfilteredIndirectSH( pix, irradianceToSH( (float3)0.0, float3( 0.0, 0.0, 1.0 ) ) );
        framebufUnfilteredIndirectSpecular[pix] = float4( 0.0, 0.0, 0.0, 0.0 );
        return;
    }

    float oneOverSourcePdf;
    const float3 bounceDir = getSpecularBounce(
        seed, 1, surf.normal, surf.roughness, surf.specularColor, surf.toViewerDir,
        oneOverSourcePdf );

    const Surface hitSurf = traceBounce(
        surf.position + surf.normalGeom * 2.5e-4,
        surf.roughness, surf.instCustomIndex, bounceDir, FIRST_BOUNCE_MIP_BIAS );

    float3 hitPos;
    float3 hitRadiance;
    float skyBsdfWeight = 1.0;
    if ( hitSurf.isSky )
    {
        skyBsdfWeight = q2SkyNeeBsdfWeight( surf, bounceDir );
        hitPos      = surf.position + bounceDir * globalUniform.rayLength;
        hitRadiance = getSkyAmbientMultiplied( bounceDir, surf.roughness * ( SKY_MIP_COUNT - 1.0 ) );
    }
    else
    {
        const bool isGradient = q2GetIsGradient( pix );
        const uint cluster    = hitSurf.cluster;

        const float regRoughness = max( hitSurf.roughness, surf.roughness );
        const float alpha       = square( regRoughness );
        const float phongExp    = q2RoughnessSquareToSpecPower( alpha );
        const float phongScale  = min( 100.0, 1.0 / ( M_PI * max( alpha * alpha, 1e-4 ) ) );
        const float phongWeight = clamp(
            getLuminance( hitSurf.specularColor ) /
                ( getLuminance( hitSurf.specularColor ) + getLuminance( hitSurf.albedo ) ),
            0.0, 0.9 );

        float3 directDiffuse = (float3)0.0;
        {
            const float3 rng = float3(
                rnd24( seed, Q2_RNG_INDIR_CELL_SELECT ),
                rnd24( seed, Q2_RNG_INDIR_CELL_SELECT + 1u ),
                rnd24( seed, Q2_RNG_INDIR_CELL_SELECT + 2u ) );

            uint  lightIndex = LIGHT_INDEX_NONE;
            uint  lightSlot  = 0u;
            float lightPdf   = 0.0;
            q2SampleClusterLights( cluster, hitSurf.position, hitSurf.normal, hitSurf.toViewerDir,
                                   1.0, 1.0, 0.0, isGradient, rng,
                                   lightIndex, lightSlot, lightPdf );

            if ( lightIndex != LIGHT_INDEX_NONE && lightPdf > 0.0 )
            {
                const float2 pointRnd = rnd16_2( seed, Q2_RNG_INDIR_LIGHT_POINT );
                const float2 memberRnd = float2(
                    rnd24( seed, Q2_RNG_INDIR_LIGHT_MEMBER ),
                    rnd24( seed, Q2_RNG_INDIR_LIGHT_MEMBER + 1u ) );
                float memberPdf = 1.0;
                LightSample light =
                    sampleLightNee( lightSources[ lightIndex ], hitSurf.position, pointRnd, memberRnd, memberPdf );

                const uint indirLightType = lightSources[ lightIndex ].lightType;
                if ( indirLightType == LIGHT_TYPE_SPHERE ||
                     indirLightType == LIGHT_TYPE_SPOT )
                {
                    light.dw = min( light.dw, indirMaxSphereSolidAngle() ) * indirSphotGain();
                }
                else if ( indirLightType == LIGHT_TYPE_TRIANGLE ||
                          indirLightType == LIGHT_TYPE_TEXTURED_AREA ||
                          indirLightType == LIGHT_TYPE_DTAL_GROUP )
                {
                    light.dw = min( light.dw, 0.5 );
                }

                if ( getLuminance( light.color ) > 0.0 )
                {
                    bool lightTraced;
                    const float3 vis =
                        traceLightVisibility( hitSurf, light, lightIndex, lightTraced );

                    if ( lightTraced )
                    {
                        rayStatsAdd( RAY_STATS_CATEGORY_SHADOW_INDIRECT, 1 );
                    }

                    if ( lightSources[ lightIndex ].lightType == LIGHT_TYPE_TRIANGLE ||
                         lightSources[ lightIndex ].lightType == LIGHT_TYPE_TEXTURED_AREA ||
                         lightSources[ lightIndex ].lightType == LIGHT_TYPE_DTAL_GROUP )
                    {
                        q2AccumulateLightStats( cluster, lightSlot, hitSurf.normal, getLuminance( vis ), 0u );
                    }

                    float3 d;
                    shadeDiffuse( hitSurf, light, 1.0 / max( lightPdf * memberPdf, 1e-9 ), d );
                    directDiffuse += d * vis;
                }
            }
        }

        // The first bounce ray is the specular one, so its sun bounce range shrinks with the
        // primary roughness the same way Q2RTX shortens it for is_specular_ray.
        const float sunAttenuation = q2SunBounceAttenuation(
            length( hitSurf.position - surf.position ),
            globalUniform.sunBounce.x * ( 1.0 - sqrt( surf.roughness ) ) );

        if ( globalUniform.directionalLightExists != 0 && sunAttenuation > 0.0 )
        {
            const DirectionalLight sun =
                decodeAsDirectionalLight( lightSources[ LIGHT_ARRAY_DIRECTIONAL_LIGHT_OFFSET ] );
            const float2 sunRnd = rnd16_2( seed, Q2_RNG_INDIR_SUN_DISK ) * 0.99;
            const LightSample sunLight = sampleDirectionalLight( sun, hitSurf.position, sunRnd );

            if ( getLuminance( sunLight.color ) > 0.0 )
            {
                bool sunTraced;
                const float3 sunVis = traceSunVisibility( hitSurf, sunLight, sunTraced );

                if ( sunTraced )
                {
                    rayStatsAdd( RAY_STATS_CATEGORY_SHADOW_INDIRECT, 1 );
                }

                float3 d;
                shadeDiffuse( hitSurf, sunLight, 1.0, d );
                directDiffuse += d * sunVis * sunAttenuation * globalUniform.sunBounce.y;
            }
        }

        float3 secondBounce = (float3)0.0;
        if ( globalUniform.indirSecondBounce != 0u )
        {
            float oneOverPdfSecond;
            const float3 bounceDirSecond =
                getDiffuseBounce( seed, 2u, hitSurf.normalGeom, oneOverPdfSecond );
            secondBounce = processSecondDiffuseBounce(
                seed, hitSurf, bounceDirSecond, oneOverPdfSecond );
            directDiffuse += secondBounce;
        }

        directDiffuse = clamp( directDiffuse, (float3)0.0, (float3)50.0 );

        if ( indirDbg( INDIR_DBG_BOUNCE2_X50 ) )
        {
            directDiffuse = clamp( directDiffuse - secondBounce, (float3)0.0, (float3)50.0 )
                + 50.0 * secondBounce;
        }

        hitPos = hitSurf.position;
        hitRadiance = boostChroma( directDiffuse * hitSurf.albedo );

        if ( ( hitSurf.geometryInstanceFlags & GEOM_INST_FLAG_EMISSION_RAY_ONLY ) != 0 )
        {
            const float bounceDist = length( hitSurf.position - surf.position );
            const float bounceCap = lerp( 50.0, 20.0, smoothstep( 0.08, 0.45, surf.roughness ) );
            hitRadiance += q2RegularizeBounceEmission(
                hitSurf.emission * hitSurf.albedo, bounceDist, oneOverSourcePdf, bounceCap );
        }
    }

    float3 diffuse, specular;
    q2ShadeIndirect( surf, hitPos, hitRadiance, oneOverSourcePdf, diffuse, specular );

    {
        const float3 direct = texelFetchUnfilteredSpecular( pix );
        const float3 rawDemodIndir = demodulateSpecular( specular, surf.specularColor );
        const float3 demodIndir = ( globalUniform.antiFireflyEnabled != 0u )
            ? q2RegularizeDemodulatedSpecular( rawDemodIndir, surf.roughness )
            : min( max( rawDemodIndir, (float3)0.0 ), (float3)32768.0 );
        const float indirLum = getLuminance( demodIndir );
        const float4 prevViewDir = framebufViewDirection.Load( pix );
        if ( indirLum > 1e-5 && ( prevViewDir.w < 0.0 || getLuminance( direct ) < indirLum ) )
        {
            const float hitDist = clamp( length( hitPos - surf.position ), 0.0, Q2_MAX_VIEW_DIR_HIT_DIST );
            framebufViewDirection[pix] = float4( -surf.toViewerDir, hitDist );
        }

        const float tracedTag = float( globalUniform.frameId & 1u ) + 1.0;
        framebufUnfilteredIndirectSpecular[pix] = float4( demodIndir, tracedTag );
    }

    {
        SH sh = irradianceToSH( diffuse * skyBsdfWeight, safeNormalize( hitPos - surf.position ) );
        accumulateSH( sh, irradianceToSH( q2SkyNee( surf, seed ), surf.normal ), 1.0 );

        if ( indirDbg( INDIR_DBG_SANITY ) )
        {
            sh = irradianceToSH( float3( 10.0, 0.0, 0.0 ), surf.normal );
        }

        imageStoreUnfilteredIndirectSH( pix, sh );
    }
}
