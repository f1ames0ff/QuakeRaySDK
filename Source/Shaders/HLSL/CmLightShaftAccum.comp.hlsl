#define DESC_SET_LIGHT_SHAFT_VOLUME 0
#define DESC_SET_LIGHT_SHAFT_HISTORY 1
#define DESC_SET_FRAMEBUFFERS 2
#define DESC_SET_GLOBAL_UNIFORM 3
#define DESC_SET_RANDOM 4
#define DESC_SET_LIGHT_SHAFT_PARAMS 5

#include "ShaderCommonHLSLFunc.hlsli"
#include "Random.hlsli"
#include "LightShafts.hlsli"

[[vk::binding(0, DESC_SET_LIGHT_SHAFT_VOLUME)]] Texture3D<float4> lightShaftVolume;
[[vk::binding(1, DESC_SET_LIGHT_SHAFT_VOLUME)]] SamplerState lightShaftVolumeSampler;
[[vk::binding(0, DESC_SET_LIGHT_SHAFT_HISTORY)]] Texture2D<float4> lightShaftHistoryPrev;
[[vk::binding(1, DESC_SET_LIGHT_SHAFT_HISTORY), vk::image_format("rgba16f")]] RWTexture2D<float4> lightShaftHistory;
[[vk::binding(2, DESC_SET_LIGHT_SHAFT_HISTORY)]] SamplerState lightShaftHistorySampler;

struct LightShaftParams_BT
{
    float4 sunDirection;
    float4 sunColor;
    float4 worldCenter;
    float4 worldHalfSizeInv;
    float4x4 shadowMapVP;
    float shadowMapDepthScale;
    float godRaysIntensity;
    float godRaysEccentricity;
    uint godRaysEnabled;
    float sunStepScale;
    uint lightShaftCount;
    float lightShaftIntensity;
    uint lightShaftSteps;
    LightShaft_BT lightShafts[MAX_LIGHT_SHAFTS];
    uint accumVolumeSizeX;
    uint accumVolumeSizeY;
    uint accumVolumeSizeZ;
    uint accumDebugMode;
};

[[vk::binding(0, DESC_SET_LIGHT_SHAFT_PARAMS)]] StructuredBuffer<LightShaftParams_BT> lightShaftParams;

float3 lightShaftVolumeUVW(float3 world)
{
    const float4 clip = mul(globalUniform.volumeViewProj, float4(world, 1.0));
    const float2 screen = clip.xy * (0.5 / clip.w) + 0.5;
    const float nearPlane = max(globalUniform.volumeCameraNear, 0.001);
    const float farPlane = max(globalUniform.volumeCameraFar, nearPlane + 0.001);
    const float radialDistance = length(world - globalUniform.cameraPosition.xyz);
    const float normalizedDepth = (radialDistance - nearPlane) / (farPlane - nearPlane);
    return float3(screen, saturate(sqrt(saturate(normalizedDepth))));
}

[numthreads(16, 16, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pixelPos = int2(dispatchThreadID.xy);
    const int2 renderSize = int2(globalUniform.renderWidth, globalUniform.renderHeight);

    if (any(pixelPos >= renderSize))
    {
        return;
    }

    const uint shaftCount = min(lightShaftParams[0].lightShaftCount, (uint)MAX_LIGHT_SHAFTS);
    const int2 cbPix = getCheckerboardPix(pixelPos);
    const float rayDistance = framebufDepthWorld_Sampled.Load(int3(cbPix, 0)).x;

    if (shaftCount == 0)
    {
        lightShaftHistory[pixelPos] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    const bool sky = rayDistance <= 0.0 || rayDistance > globalUniform.rayLength;
    float3 uvw = float3((float2(pixelPos) + 0.5) / float2(renderSize), 1.0);

    if (!sky)
    {
        const float4 surface = framebufSurfacePosition_Sampled.Load(int3(cbPix, 0));
        const float3 surfacePos = surface.xyz;
        const uint surfaceInstanceFlags = asuint(surface.w);

        if ((surfaceInstanceFlags & (INSTANCE_CUSTOM_INDEX_FLAG_FIRST_PERSON |
                                     INSTANCE_CUSTOM_INDEX_FLAG_FIRST_PERSON_VIEWER)) != 0)
        {
            lightShaftHistory[pixelPos] = float4(0.0, 0.0, 0.0, 1.0);
            return;
        }

        uvw = lightShaftVolumeUVW(surfacePos);
    }
    const float3 cell = 1.0 / float3((float)lightShaftParams[0].accumVolumeSizeX,
                                     (float)lightShaftParams[0].accumVolumeSizeY,
                                     (float)lightShaftParams[0].accumVolumeSizeZ);

    const float4 centerSample = lightShaftVolume.SampleLevel(lightShaftVolumeSampler, saturate(uvw), 0.0);
    float3 shaft = centerSample.rgb * 2.0;
    float transmittance = centerSample.a * 2.0;

    const float3 offsets[6] =
    {
        float3(cell.x, 0.0, 0.0), float3(-cell.x, 0.0, 0.0),
        float3(0.0, cell.y, 0.0), float3(0.0, -cell.y, 0.0),
        float3(0.0, 0.0, cell.z), float3(0.0, 0.0, -cell.z)
    };

    for (int i = 0; i < 6; i++)
    {
        const float4 tap = lightShaftVolume.SampleLevel(
            lightShaftVolumeSampler, saturate(uvw + offsets[i]), 0.0);
        shaft += tap.rgb;
        transmittance += tap.a;
    }

    shaft *= 0.125;
    transmittance = saturate(transmittance * 0.125);

    const uint debugMode = lightShaftParams[0].accumDebugMode;
    if (debugMode != 0u)
    {
        float3 debugColor = shaft;
        if (debugMode == 3u)
        {
            debugColor = transmittance * (float3)1.0;
        }
        lightShaftHistory[pixelPos] = float4(debugColor, 1.0);
        return;
    }

    lightShaftHistory[pixelPos] = float4(
        shaft * lightShaftParams[0].lightShaftIntensity, transmittance);
}
