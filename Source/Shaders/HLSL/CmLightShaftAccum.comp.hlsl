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
};

[[vk::binding(0, DESC_SET_LIGHT_SHAFT_PARAMS)]] StructuredBuffer<LightShaftParams_BT> lightShaftParams;

#define LIGHT_SHAFT_VOLUME_SIZE_X 160
#define LIGHT_SHAFT_VOLUME_SIZE_Y 88
#define LIGHT_SHAFT_VOLUME_SIZE_Z 64

float3 lightShaftVolumeUVW(float3 world)
{
    const float4 clip = mul(globalUniform.volumeViewProj, float4(world, 1.0));
    const float2 screen = clip.xy * (0.5 / clip.w) + 0.5;
    const float nearPlane = globalUniform.volumeCameraNear;
    const float farPlane = max(globalUniform.volumeCameraFar, nearPlane + 0.001);
    const float radialDistance = length(world - globalUniform.cameraPosition.xyz);
    const float normalizedDepth = (radialDistance - nearPlane) / (farPlane - nearPlane);
    return float3(screen, saturate(normalizedDepth));
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

    if (shaftCount == 0 || rayDistance <= 0.0 || rayDistance > MAX_RAY_LENGTH)
    {
        lightShaftHistory[pixelPos] = (float4)0;
        return;
    }

    const float4 surface = framebufSurfacePosition_Sampled.Load(int3(cbPix, 0));
    const float3 surfacePos = surface.xyz;
    const uint surfaceInstanceFlags = asuint(surface.w);

    if ((surfaceInstanceFlags & (INSTANCE_CUSTOM_INDEX_FLAG_FIRST_PERSON |
                                 INSTANCE_CUSTOM_INDEX_FLAG_FIRST_PERSON_VIEWER)) != 0)
    {
        lightShaftHistory[pixelPos] = (float4)0;
        return;
    }

    const float3 normal = texelFetchNormal(cbPix);
    const uint seed = getRandomSeed(pixelPos, globalUniform.frameId);
    const float3 noise = rndBlueNoise8(seed, 1).xyz;

    float3 uvw = lightShaftVolumeUVW(surfacePos);
    uvw += (noise * 2.0 - 1.0) *
           float3(2.0 / LIGHT_SHAFT_VOLUME_SIZE_X,
                  2.0 / LIGHT_SHAFT_VOLUME_SIZE_Y,
                  2.0 / LIGHT_SHAFT_VOLUME_SIZE_Z);

    const float3 current = lightShaftVolume.SampleLevel(
        lightShaftVolumeSampler, saturate(uvw), 0.0).rgb;

    const float4 previousClip = mul(globalUniform.projectionPrev,
                                    mul(globalUniform.viewPrev, float4(surfacePos, 1.0)));
    const float2 previousPos = (previousClip.xy / previousClip.w * 0.5 + 0.5) *
                               float2(globalUniform.renderWidth, globalUniform.renderHeight);
    const float2 previousBaseFloat = floor(previousPos - 0.5);
    const int2 previousBase = int2(previousBaseFloat);
    const float2 previousSub = previousPos - 0.5 - previousBaseFloat;

    float3 accumulated = (float3)0;
    float historyLength = 0.0;
    float weightSum = 0.0;

    for (int yy = 0; yy <= 1; yy++)
    {
        for (int xx = 0; xx <= 1; xx++)
        {
            const int2 previousPixel = previousBase + int2(xx, yy);
            if (any(previousPixel < (int2)0) || any(previousPixel >= renderSize))
            {
                continue;
            }

            const int2 previousCbPixel = getCheckerboardPix(previousPixel);
            const float previousDepth = framebufDepthWorld_Prev_Sampled.Load(
                int3(previousCbPixel, 0)).x;
            const float3 previousNormal = texelFetchNormal_Prev(previousCbPixel);

            if (!testReprojectedDepth(rayDistance, previousDepth, 0.0) ||
                !testReprojectedNormal(normal, previousNormal))
            {
                continue;
            }

            const float weight = (xx != 0 ? previousSub.x : 1.0 - previousSub.x) *
                                 (yy != 0 ? previousSub.y : 1.0 - previousSub.y);
            const float4 previous = lightShaftHistoryPrev.Load(int3(previousPixel, 0));
            accumulated += previous.rgb * weight;
            historyLength += previous.a * weight;
            weightSum += weight;
        }
    }

    if (weightSum > 0.0001)
    {
        accumulated /= weightSum;
        historyLength /= weightSum;
    }
    else
    {
        accumulated = current;
        historyLength = 0.0;
    }

    const float maxHistory = (float)clamp(lightShaftParams[0].lightShaftSteps, 4u, 32u);
    historyLength = clamp(historyLength + 1.0, 1.0, maxHistory);
    accumulated = lerp(accumulated, current, 1.0 / historyLength);
    lightShaftHistory[pixelPos] = float4(
        accumulated * 0.01 * lightShaftParams[0].lightShaftIntensity, historyLength);
}
