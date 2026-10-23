#include "EditorSettings.h"

#include <cmath>

#include "Scene.h"

namespace
{

// Matches RT_SKY_RADIANCE_SCALE in Quake/glquake.h: the scale that turns the
// authored sky brightness into a radiance the renderer can use.
constexpr float kSkyRadianceScale = 0.1f;

float Clamp(float value, float low, float high)
{
    return value < low ? low : (value > high ? high : value);
}

float Luminance(const float color[3])
{
    return 0.2125f * color[0] + 0.7154f * color[1] + 0.0721f * color[2];
}

} // namespace

void BuildEditorFrameParams(const EditorRenderSettings &settings, const float sunDirection[3],
                            float sunIntensity, const float viewerPosition[3],
                            EditorFrameParams &out)
{
    const float skyBrightness = kSkyRadianceScale * settings.skyBrightness * settings.brightness;

    out.sky = {};
    out.sky.skyType = static_cast<QrSkyType>(settings.skyType);
    out.sky.skyColorDefault = { { settings.skyColor[0], settings.skyColor[1], settings.skyColor[2] } };
    out.sky.sunDiscColor = { { settings.sunColor[0] * settings.sunDiscScale,
                               settings.sunColor[1] * settings.sunDiscScale,
                               settings.sunColor[2] * settings.sunDiscScale } };
    out.sky.sunDiscSize = settings.sunDiscSize;
    out.sky.skyAmbientLod = float(settings.skyAmbientLod);
    out.sky.skyLightMultiplier = settings.skyLightMultiplier;
    out.sky.skyNee = settings.skyNee ? 1 : 0;
    out.sky.skyViewerPosition = { { viewerPosition[0], viewerPosition[1], viewerPosition[2] } };
    // gl_vidsdl.c passes rt_sky_godrays straight through, whatever the sky type
    // is; the renderer is what decides a sky has no sun to scatter.
    out.sky.godRaysEnabled = settings.godRays ? 1 : 0;
    out.sky.godRaysIntensity = settings.godRaysIntensity;
    out.sky.godRaysQuality = uint32_t(settings.godRaysQuality);
    out.sky.godRaysFromSkyTexture = 0;
    out.sky.skyCloudsQuality = uint32_t(settings.cloudsQuality);

    if (settings.skyType == QR_SKY_TYPE_PROCEDURAL)
    {
        // The procedural sky paints its own colour, so the multiplier reaches it
        // once; a classic sky would need the 1/luminance term on top of it.
        out.sky.skyColorMultiplier = skyBrightness;

        // The cloud block rides in the otherwise unused rotation matrix, in the
        // order QrDrawFrameSkyParams documents.
        float *clouds = &out.sky.skyCubemapRotationTransform.matrix[0][0];
        clouds[0] = settings.cloudsColor[0];
        clouds[1] = settings.cloudsColor[1];
        clouds[2] = settings.cloudsColor[2];
        clouds[3] = settings.cloudsCoverage;
        clouds[4] = settings.cloudsDensity;
        clouds[5] = settings.cloudsSpeed;
        clouds[6] = settings.clouds ? 1.0f : 0.0f;
        clouds[7] = settings.cloudsHeight;
        clouds[8] = settings.cloudsThickness;
    }
    else
    {
        const float skyMult = (1.0f / Clamp(Luminance(settings.skyColor), 0.02f, 1.0f)) * settings.skyScale;
        out.sky.skyColorMultiplier = skyMult * skyBrightness;
    }

    // The procedural sky is the only one whose clouds are composited with an
    // opacity; see QrDrawFrameSkyParams::skyColorSaturation.
    out.sky.skyColorSaturation = settings.cloudsAlpha;

    out.tonemap = {};
    out.tonemap.minLogLuminance = settings.tonemapMinLog;
    out.tonemap.maxLogLuminance = settings.tonemapMaxLog;
    out.tonemap.luminanceWhitePoint = settings.tonemapWhite;
    out.tonemap.exposureBias = 0.0f;
    out.tonemap.tonemapPower = 0.9f;
    out.tonemap.highlightDesaturation = settings.tonemapDesat;
    out.tonemap.chromaPreservation = settings.tonemapChroma;
    out.tonemap.exposureSpeedUp = 3.0f;
    out.tonemap.exposureSpeedDown = 1.0f;
    out.tonemap.exposureLowPercentile = 70.0f;
    out.tonemap.exposureHighPercentile = 90.0f;
    out.tonemap.minAdaptedLuminance = 0.02f;
    out.tonemap.maxAdaptedLuminance = 1.0f;
    out.tonemap.tonemapType = uint32_t(settings.tonemapType);

    out.volume = {};
    out.volume.enable = settings.volumeType != 0 ? 1 : 0;
    out.volume.useSimpleDepthBased = settings.volumeType == 1 ? 1 : 0;
    out.volume.volumetricFar = settings.volumeFar;
    out.volume.scaterring = settings.volumeScatter;
    // The block takes the direction the light travels, which is how gl_vidsdl.c passes
    // RT_AnglesToDir's forward: the game reads -forward as the direction towards the sun, so the
    // shafts of a level look the same here as in the game for the same rt_sky_sun_pitch/yaw.
    out.volume.sourceDirection = {
        { -sunDirection[0], -sunDirection[1], -sunDirection[2] }
    };
    out.volume.sourceAssymetry = settings.volumeAsymmetry;

    // The scattering source is the sun the directional light draws, so a scene
    // without a sun scatters nothing rather than glowing from nowhere.
    const float lightScale = sunIntensity > 0.0f ? settings.volumeLightIntensity * settings.brightness
                                                 : 0.0f;
    out.volume.sourceColor = { { settings.sunColor[0] * lightScale, settings.sunColor[1] * lightScale,
                                 settings.sunColor[2] * lightScale } };

    const float ambientScale = settings.volumeAmbient * settings.brightness;
    out.volume.ambientColor = { { settings.skyColor[0] * ambientScale, settings.skyColor[1] * ambientScale,
                                  settings.skyColor[2] * ambientScale } };

    out.fog.color = { { settings.fogColor[0], settings.fogColor[1], settings.fogColor[2] } };
    // The game divides the worldspawn density by the 64 the classic renderer
    // scaled it with, so a mid-range 0.05 fades the distance instead of everything.
    out.fog.density = settings.fogDensity > 0.0f ? settings.fogDensity / 64.0f : 0.0f;
    out.fog.skyBlend = settings.fogDensity > 0.0f ? settings.fogSkyBlend : 0.0f;

    out.reflectRefract = {};
    // The panel authors the water look; the rest of the block is the game's own cvar
    // defaults, so a frame here refracts and traces glass the way a game frame does.
    out.reflectRefract.maxReflectRefractDepth = 2;
    out.reflectRefract.typeOfMediaAroundCamera = QR_MEDIA_TYPE_VACUUM;
    out.reflectRefract.indexOfRefractionGlass = 1.52f;
    out.reflectRefract.indexOfRefractionWater = 1.33f;
    out.reflectRefract.waterWaveNormalStrength = settings.waterNormalStrength;
    out.reflectRefract.waterWaveTextureDerivativesMultiplier = settings.waterNormalSharpness;
    // gl_vidsdl.c divides the metres through QUAKEUNIT_IN_METERS, which is the same
    // factor a scene unit is worth here.
    out.reflectRefract.waterWaveSpeed = settings.waterSpeed * kEditorUnitsPerMeter;
    out.reflectRefract.waterTextureAreaScale = settings.waterScale * kEditorUnitsPerMeter;
    // The colours reach the shader as a per-unit transmittance rather than as a plain
    // colour, so the game raises them to the 1/40 a scene unit is worth before it
    // hands them over; the same exponent keeps a water surface the same shade in both.
    const float colorExponent = 1.0f / kEditorUnitsPerMeter;
    out.reflectRefract.waterColor = { { std::pow(settings.waterColor[0], colorExponent),
                                        std::pow(settings.waterColor[1], colorExponent),
                                        std::pow(settings.waterColor[2], colorExponent) } };
    out.reflectRefract.acidColor = { { std::pow(settings.acidColor[0], colorExponent),
                                       std::pow(settings.acidColor[1], colorExponent),
                                       std::pow(settings.acidColor[2], colorExponent) } };
    // Two values the game states as zero rather than authoring them: nothing sets an
    // acid density, and the turbulent warp is off while its editor runs -- which a
    // standalone editor always is. Traced glass and the portal twirl are on, denoising
    // is not; the panel has no cvar for any of them either.
    out.reflectRefract.acidDensity = 0.0f;
    out.reflectRefract.turbWarpStrength = 0.0f;
    out.reflectRefract.glassShadows = 1;
    out.reflectRefract.glassParticles = 1;
    out.reflectRefract.portalNormalTwirl = 1;

    // The texture block is handed over so a material can author its own emission blend,
    // and it repeats what the renderer falls back to when the block is absent, except for
    // the blend mode: the fallback is 0, the game's rt_emis_blend default is 1, and a
    // material that leaves luma_blend alone has to land on the same composite it does.
    out.textures = {};
    out.textures.dynamicSamplerFilter = QR_SAMPLER_FILTER_LINEAR;
    out.textures.normalMapStrength = 1.0f;
    out.textures.emissionMapBoost = 100.0f;
    out.textures.emissionMaxScreenColor = 1.5f;
    out.textures.emissionSharpMask = 1.0f;
    out.textures.talSelfLitOffset = 0.0f;
    out.textures.squareInputRoughness = 1;
    out.textures.minRoughness = 0.0f;
    out.textures.emissionBlendMode = 1;
    out.textures.emissionBlendStrength = 1.0f;

    for (uint32_t i = 0; i < QR_LIGHT_STYLE_COUNT; i++)
    {
        out.textures.lightStyleScales[i] = 1.0f;
    }
}
