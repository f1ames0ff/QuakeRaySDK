#include "EditorSettings.h"

#include <algorithm>
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

void PopulateSkyLightShaftParams(const EditorRenderSettings &settings, QrDrawFrameSkyParams &sky)
{
    sky.lightShaftsEnabled = settings.lightShafts ? 1 : 0;
    sky.lightShaftsIntensity = settings.lightShaftsIntensity;
    sky.lightShaftsResolution = uint32_t(std::clamp(settings.lightShaftsResolution, 0, 2));
    sky.lightShaftsDensity = Clamp(settings.lightShaftsDensity, 0.0f, 1.0f);
    sky.lightShaftsAlbedo = Clamp(settings.lightShaftsAlbedo, 0.0f, 1.0f);
    sky.lightShaftsLocalFogScale = Clamp(settings.lightShaftsLocalFogScale, 0.0f, 16.0f);
    sky.lightShaftsEmissive = { { Clamp(settings.lightShaftsEmissive[0], 0.0f, 16.0f),
                                  Clamp(settings.lightShaftsEmissive[1], 0.0f, 16.0f),
                                  Clamp(settings.lightShaftsEmissive[2], 0.0f, 16.0f) } };
    sky.lightShaftsTemporal = settings.lightShaftsTemporal ? 1 : 0;
    sky.lightShaftsSun = settings.lightShaftsSun ? 1 : 0;
    sky.lightShaftsDebug = uint32_t(std::clamp(settings.lightShaftsDebug, 0, 5));
    sky.lightShaftsRayBudget = uint32_t(std::clamp(settings.lightShaftsRayBudget, 0, 1048576));
}

void PopulateVolumetricParams(const EditorRenderSettings &settings, const float sunDirection[3],
                              float sunIntensity, const float *skyFlatColor,
                              QrDrawFrameVolumetricParams &volume)
{
    volume = {};
    volume.enable = (settings.volumeType != 0 || settings.lightShafts) ? 1 : 0;
    volume.useSimpleDepthBased = (settings.volumeType == 1 && !settings.lightShafts) ? 1 : 0;
    volume.volumetricFar = settings.volumeFar;
    volume.scaterring = settings.volumeScatter;
    // The block takes the direction the light travels, which is how gl_vidsdl.c passes
    // RT_AnglesToDir's forward: the game reads -forward as the direction towards the sun, so the
    // shafts of a level look the same here as in the game for the same rt_sky_sun_pitch/yaw.
    volume.sourceDirection = { { -sunDirection[0], -sunDirection[1], -sunDirection[2] } };
    volume.sourceAssymetry = settings.volumeAsymmetry;

    // The scattering source is the sun the directional light draws, so a scene
    // without a sun scatters nothing rather than glowing from nowhere.
    const float lightScale =
        sunIntensity > 0.0f ? settings.volumeLightIntensity * settings.brightness : 0.0f;
    volume.sourceColor = { { settings.sunColor[0] * lightScale, settings.sunColor[1] * lightScale,
                             settings.sunColor[2] * lightScale } };

    // gl_vidsdl.c: VectorScale (skyflatcolor, rt_volume_ambient * rt_brightness,
    // volume_ambient_color). The flat colour is the sky texture's average, not rt_sky_color,
    // because the fog is lit by the picture rather than by the tint the sky is painted with.
    const float *ambientBase = skyFlatColor != nullptr ? skyFlatColor : settings.skyColor;
    const float ambientScale = settings.volumeAmbient * settings.brightness;
    volume.ambientColor = { { ambientBase[0] * ambientScale, ambientBase[1] * ambientScale,
                              ambientBase[2] * ambientScale } };
}

void PopulateSkyParams(const EditorRenderSettings &settings, const float sunDirection[3],
                       const float moonDirection[3], float celestialRotationRad,
                       float sunIntensity, const float viewerPosition[3],
                       QrDrawFrameSkyParams &sky)
{
    const float skyBrightness = kSkyRadianceScale * settings.skyBrightness * settings.brightness;

    sky = {};
    sky.skyType = (settings.skyType == QR_SKY_TYPE_CUBEMAP)
                      ? QR_SKY_TYPE_RASTERIZED_GEOMETRY
                      : static_cast<QrSkyType>(settings.skyType);
    sky.skyColorDefault = { { settings.skyColor[0], settings.skyColor[1], settings.skyColor[2] } };
    sky.sunDiscColor = { { settings.sunColor[0] * settings.sunDiscScale,
                           settings.sunColor[1] * settings.sunDiscScale,
                           settings.sunColor[2] * settings.sunDiscScale } };
    sky.sunDiscSize = settings.sunDiscSize;
    sky.skyAmbientLod = float(settings.skyAmbientLod);
    sky.skyLightMultiplier = settings.skyLightMultiplier;
    sky.skyNee = settings.skyNee ? 1 : 0;
    sky.skyViewerPosition = { { viewerPosition[0], viewerPosition[1], viewerPosition[2] } };
    // gl_vidsdl.c passes rt_sky_godrays straight through, whatever the sky type
    // is; the renderer is what decides a sky has no sun to scatter.
    sky.godRaysEnabled = settings.godRays ? 1 : 0;
    sky.godRaysIntensity = settings.godRaysIntensity;
    sky.godRaysQuality = uint32_t(settings.godRaysQuality);
    sky.godRaysFromSkyTexture = 0;
    sky.skyCloudsQuality = uint32_t(settings.cloudsQuality);
    PopulateSkyLightShaftParams(settings, sky);

    if (settings.skyType == QR_SKY_TYPE_PROCEDURAL)
    {
        // The procedural sky paints its own colour, so the multiplier reaches it
        // once; a classic sky would need the 1/luminance term on top of it.
        sky.skyColorMultiplier = skyBrightness;

        // The cloud block rides in the otherwise unused rotation matrix, in the
        // order QrDrawFrameSkyParams documents.
        float *clouds = &sky.skyCubemapRotationTransform.matrix[0][0];
        clouds[0] = settings.cloudsColor[0];
        clouds[1] = settings.cloudsColor[1];
        clouds[2] = settings.cloudsColor[2];
        clouds[3] = settings.cloudsCoverage;
        clouds[4] = settings.cloudsDensity;
        clouds[5] = settings.cloudsSpeed;
        clouds[6] = settings.clouds ? 1.0f : 0.0f;
        clouds[7] = settings.cloudsHeight;
        clouds[8] = settings.cloudsThickness;

        sky.proceduralAtmosphereConfigured = 1;
        sky.proceduralSunEnabled =
            (sunIntensity > 0.0f || settings.timeOfDayEnable || settings.sunDiscScale > 0.0f) ? 1 : 0;
        sky.proceduralSunDirection = { { sunDirection[0], sunDirection[1], sunDirection[2] } };
        sky.proceduralSunTint = { { settings.sunColor[0], settings.sunColor[1], settings.sunColor[2] } };
        sky.proceduralSunStrength = (sunIntensity > 0.0f)
            ? std::max(sunIntensity / kEditorSkySunRadiance, 0.25f)
            : 1.0f;
        sky.proceduralMoonDirection = { { moonDirection[0], moonDirection[1], moonDirection[2] } };
        const float moonGain = std::max(0.0f, settings.moonIntensity);
        sky.proceduralMoonColor = { { settings.moonColor[0] * moonGain,
                                      settings.moonColor[1] * moonGain,
                                      settings.moonColor[2] * moonGain } };
        sky.proceduralMoonSize = Clamp(settings.moonSize, 0.0f, 10.0f);
        sky.proceduralMoonPhase = settings.moonPhase;
        sky.proceduralStarsEnabled = settings.starsEnabled ? 1 : 0;
        sky.proceduralStarDensity = Clamp(settings.starDensity, 0.0f, 4.0f);
        sky.proceduralStarIntensity = Clamp(settings.starIntensity, 0.0f, 5.0f);
        sky.celestialRotationRadians = celestialRotationRad;
        sky.atmosphereDensity = Clamp(settings.atmosphereDensity, 0.0f, 4.0f);
        sky.atmosphereMie = Clamp(settings.atmosphereMie, 0.0f, 4.0f);
        sky.atmosphereOzone = Clamp(settings.atmosphereOzone, 0.0f, 4.0f);
    }
    else
    {
        // Visible raster sky already carries rt_sky * rt_sky_brightness * rt_brightness; cancel
        // rt_sky_brightness here so rt_sky + rt_sky_light_mult drive emitted light independently.
        const float lumNorm = 1.0f / Clamp(Luminance(settings.skyColor), 0.02f, 1.0f);
        sky.skyColorMultiplier =
            (settings.skyBrightness > 1e-4f) ? ((lumNorm * kSkyRadianceScale) / settings.skyBrightness)
                                             : 0.0f;
    }

    // The procedural sky is the only one whose clouds are composited with an
    // opacity; see QrDrawFrameSkyParams::skyColorSaturation.
    sky.skyColorSaturation = settings.cloudsAlpha;
}

void PopulatePostEffectParams(const EditorRenderSettings &settings, EditorFrameParams &out)
{
    // The game hands the SDK bloom pass a strength of zero and lets the post-effect bloom paint
    // the halos, so the pass keeps only its emission multiplier here. Leaving pBloomParams off
    // the frame instead selects the renderer's fallback of intensity 1 with an emission
    // multiplier of 64, which blooms every emissive surface by another order of magnitude.
    out.bloom = {};
    out.bloom.bloomIntensity = 0.0f;
    out.bloom.inputThreshold = 0.0f;
    out.bloom.bloomEmissionMultiplier = settings.bloomEmissionMultiplier;

    out.bloomEffect = {};
    out.bloomEffect.isActive = settings.bloomIntensity > 0.0f ? 1 : 0;
    out.bloomEffect.intensity = Clamp(settings.bloomIntensity, 0.0f, 0.2f);
    out.bloomEffect.threshold = Clamp(settings.bloomThreshold, 0.0f, 20.0f);
    out.bloomEffect.knee = Clamp(settings.bloomKnee, 0.0f, 1.0f);
    out.bloomEffect.scatter = Clamp(settings.bloomScatter, 0.0f, 1.0f);
    out.bloomEffect.radius = Clamp(settings.bloomRadius, 0.005f, 0.15f);
    out.bloomEffect.quality = uint32_t(settings.bloomQuality < 0 ? 0
                                       : (settings.bloomQuality > 2 ? 2 : settings.bloomQuality));

    out.postEffects = {};
    out.postEffects.pBloom = &out.bloomEffect;

    // The grade the game puts over its frame. Vignette, grain, sharpen and local exposure are
    // read from cvars by gl_vidsdl.c on every frame and the CRT flag from rt_ef_crt, so a
    // preview that omits them is not the same image even when every other block matches.
    out.vignette = {};
    out.vignette.intensity = Clamp(settings.vignetteIntensity, 0.0f, 1.0f);
    out.vignette.start = Clamp(settings.vignetteStart, 0.0f, 0.99f);
    out.vignette.end = Clamp(settings.vignetteEnd, 0.01f, 2.0f);
    out.vignette.roundness = Clamp(settings.vignetteRoundness, 0.0f, 1.0f);

    out.filmGrain = {};
    out.filmGrain.intensity = Clamp(settings.filmGrainIntensity, 0.0f, 1.0f);
    out.filmGrain.size = Clamp(settings.filmGrainSize, 0.25f, 8.0f);

    out.sharpen = {};
    out.sharpen.strength = Clamp(settings.sharpenStrength, 0.0f, 1.0f);
    out.sharpen.isActive = out.sharpen.strength > 0.0f ? 1 : 0;

    out.crt = {};
    out.crt.isActive = settings.crtEnabled ? 1 : 0;

    out.postEffects.pVignette = &out.vignette;
    out.postEffects.pFilmGrain = &out.filmGrain;
    out.postEffects.pSharpen = &out.sharpen;
    out.postEffects.pCRT = &out.crt;
    out.postEffects.localExposure = Clamp(settings.localExposure, 0.0f, 1.0f);
}

void PopulateIlluminationParams(const EditorRenderSettings &settings, EditorFrameParams &out)
{
    // The estimator the game runs. Every derivation here is VulkanDevice.cpp:575's, applied
    // to the authored value instead, so the block is already inside the range that code
    // would clamp it into and the preview matches the game's own numbers.
    out.illumination = {};
    out.illumination.maxBounceShadows = settings.shadowRays;
    out.illumination.enableSecondBounceForIndirect = settings.giLevel >= 1.5f;
    out.illumination.cellWorldSize = std::max(settings.gradientCellMeters, 0.0f) * kEditorUnitsPerMeter;
    out.illumination.directDiffuseSensitivityToChange = Clamp(settings.sensitivityDirect, 0.0f, 1.0f);
    out.illumination.indirectDiffuseSensitivityToChange = Clamp(settings.sensitivityIndirect, 0.0f, 1.0f);
    out.illumination.specularSensitivityToChange = Clamp(settings.sensitivitySpecular, 0.0f, 1.0f);
    out.illumination.polygonalLightSpotlightFactor = 2.0f;
    out.illumination.q2DepthGradMode = settings.q2DepthGrad != 0 ? 1u : 0u;
    out.illumination.q2LightStatsMode = settings.q2LightStats < 0 ? 1u
                                        : (settings.q2LightStats > 4 ? 1u
                                                                     : uint32_t(settings.q2LightStats));
    out.illumination.reflRefrEarlyOut = settings.reflRefrEarlyOut ? 1u : 0u;
    out.illumination.neeLightSamples = settings.neeSamples >= 1.5f ? 2u : 1u;
    out.illumination.restirEnabled = settings.restir > 0.0f ? 1u : 0u;
    out.illumination.restirCandidates = uint32_t(Clamp(settings.restirCandidates, 1.0f, 64.0f));
    out.illumination.giBounceRays = Clamp(settings.giLevel, 0.0f, 2.0f);
    out.illumination.denoiserEnabled = settings.denoiser > 0.0f;
    out.illumination.fixedAlbedo = settings.noTextures > 0.0f ? 2.0f : 0.0f;
    out.illumination.sunBounceRange = std::max(settings.sunBounceRange, 0.0f);
    out.illumination.sunBounceScale = std::max(settings.sunBounceScale, 0.0f);
    out.illumination.lightUniqueIdIgnoreFirstPersonViewerShadows = nullptr;

    out.antifirefly = settings.antifirefly > 0.0f ? 1 : 0;
}

} // namespace

void BuildEditorFrameParams(const EditorRenderSettings &settings, const float sunDirection[3],
                            const float moonDirection[3], float celestialRotationRad,
                            float sunIntensity, const float viewerPosition[3],
                            const float *skyFlatColor, EditorFrameParams &out)
{
    PopulateSkyParams(settings, sunDirection, moonDirection, celestialRotationRad, sunIntensity,
                      viewerPosition, out.sky);

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

    PopulateVolumetricParams(settings, sunDirection, sunIntensity, skyFlatColor, out.volume);

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
    out.textures.normalMapStrength = settings.normalMapStrength;
    // gl_vidsdl.c multiplies the cvar by rt_emis_light_intensity before handing the boost over.
    out.textures.emissionMapBoost = settings.emissionMapBoost * settings.emissionLightIntensity;
    out.textures.emissionMaxScreenColor = settings.emissionMaxScreenColor;
    out.textures.emissionSharpMask = settings.emissionSharpMask;
    out.textures.talSelfLitOffset = settings.talSelfLitOffset;
    out.textures.squareInputRoughness = 1;
    out.textures.minRoughness = settings.minRoughness;
    out.textures.emissionBlendMode = 1;
    out.textures.emissionBlendStrength = 1.0f;

    for (uint32_t i = 0; i < QR_LIGHT_STYLE_COUNT; i++)
    {
        out.textures.lightStyleScales[i] = 1.0f;
    }

    PopulatePostEffectParams(settings, out);
    PopulateIlluminationParams(settings, out);
}

float EditorFovYRadians(const EditorRenderSettings &settings, float width, float height)
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kDegToRad = kPi / 180.0f;
    constexpr float kRadToDeg = 180.0f / kPi;

    float fovX = settings.fovDegrees;

    if (!(fovX >= 1.0f && fovX <= 179.0f))
    {
        fovX = 90.0f;
    }

    if (!(width > 0.0f) || !(height > 0.0f))
    {
        width = 4.0f;
        height = 3.0f;
    }

    // AdaptFovx: the authored fov is horizontal on a 4:3 screen, so a wider window widens the
    // horizontal field until the vertical one stays put.
    if (settings.fovAdapt)
    {
        const float ratio = height / width;

        if (ratio != 0.75f)
        {
            fovX = 2.0f * (std::atan(0.75f / ratio * std::tan(fovX * 0.5f * kDegToRad)) * kRadToDeg);
        }
    }

    // CalcFovy.
    const float x = width / std::tan(fovX * 0.5f * kDegToRad);
    return 2.0f * std::atan(height / x);
}

void ApplySunDirectionFromRay(const float direction[3], EditorSunState &sun)
{
    constexpr float kRadToDeg = 57.2957795f;
    sun.pitch = std::asin(std::clamp(direction[2], -1.0f, 1.0f)) * kRadToDeg;
    float yaw = std::atan2(direction[1], direction[0]) * kRadToDeg + 180.0f;
    if (yaw < 0.0f)
    {
        yaw += 360.0f;
    }
    if (yaw >= 360.0f)
    {
        yaw -= 360.0f;
    }
    sun.yaw = yaw;
    sun.enabled = true;
}

void ComputeEditorCelestialFrame(const EditorRenderSettings &settings, const EditorSunState &sun,
                                 double elapsedSeconds, float outSunDir[3], float outMoonDir[3],
                                 float &outCelestialRotationRad)
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kDegToRad = kPi / 180.0f;

    if (!settings.timeOfDayEnable)
    {
        const float pitch = sun.pitch * kDegToRad;
        const float yaw = sun.yaw * kDegToRad;
        const float cosPitch = std::cos(pitch);
        outSunDir[0] = -cosPitch * std::cos(yaw);
        outSunDir[1] = -cosPitch * std::sin(yaw);
        outSunDir[2] = std::sin(pitch);

        float mx = -outSunDir[0] + 0.15f;
        float my = -outSunDir[1] - 0.10f;
        float mz = -outSunDir[2];
        const float mLen = std::sqrt(mx * mx + my * my + mz * mz);
        const float invMLen = mLen > 1e-6f ? 1.0f / mLen : 1.0f;
        outMoonDir[0] = mx * invMLen;
        outMoonDir[1] = my * invMLen;
        outMoonDir[2] = mz * invMLen;
        outCelestialRotationRad = 0.0f;
        return;
    }

    double hours = double(settings.timeOfDay) +
                   elapsedSeconds * (double(std::max(0.0f, settings.timeOfDaySpeed)) / 60.0);
    hours = std::fmod(hours, 24.0);
    if (hours < 0.0)
    {
        hours += 24.0;
    }

    const float hourAngle = float((hours - 12.0) * (kPi / 12.0));
    const float lat = Clamp(settings.skyLatitude, -80.0f, 80.0f) * kDegToRad;
    const float az = settings.skyAzimuth * kDegToRad;
    const float cosAz = std::cos(az);
    const float sinAz = std::sin(az);

    const auto arcDir = [&](float hAngle, float latRad, float outVec[3]) {
        const float localEast = -std::sin(hAngle);
        const float localNorth = -std::cos(hAngle) * std::sin(latRad);
        const float localUp = std::cos(hAngle) * std::cos(latRad);
        outVec[0] = localEast * cosAz - localNorth * sinAz;
        outVec[1] = localEast * sinAz + localNorth * cosAz;
        outVec[2] = localUp;
    };

    arcDir(hourAngle, lat, outSunDir);
    const float moonLatDeg = Clamp(settings.skyLatitude + (settings.skyLatitude >= 0.0f ? 15.0f : -15.0f),
                                   -70.0f, 70.0f);
    arcDir(hourAngle + kPi + 0.30f, moonLatDeg * kDegToRad, outMoonDir);
    outCelestialRotationRad = hourAngle;
}

void EvalEditorSunGroundTransmittance(const EditorRenderSettings &settings,
                                      const float sunDirection[3], float outTrans[3])
{
    const float z = sunDirection[2];
    const float horizon = Clamp((z + 0.024f) / 0.039f, 0.0f, 1.0f);
    const float smoothHorizon = horizon * horizon * (3.0f - 2.0f * horizon);
    if (smoothHorizon <= 0.0f)
    {
        outTrans[0] = outTrans[1] = outTrans[2] = 0.0f;
        return;
    }

    const float density = Clamp(settings.atmosphereDensity, 0.0f, 4.0f);
    if (density <= 1e-5f)
    {
        outTrans[0] = outTrans[1] = outTrans[2] = smoothHorizon;
        return;
    }

    const float mie = Clamp(settings.atmosphereMie, 0.0f, 4.0f);
    const float ozone = Clamp(settings.atmosphereOzone, 0.0f, 4.0f);
    const float skyPeak = std::max({ settings.skyColor[0], settings.skyColor[1], settings.skyColor[2], 0.0f });
    const float invPeak = skyPeak > 1e-5f ? 1.0f / skyPeak : 0.0f;
    const float skyNorm[3] = {
        skyPeak > 1e-5f ? settings.skyColor[0] * invPeak : 0.0f,
        skyPeak > 1e-5f ? settings.skyColor[1] * invPeak : 0.663f,
        skyPeak > 1e-5f ? settings.skyColor[2] * invPeak : 1.0f,
    };

    const float earthExt[3] = { 5.8e-6f, 12.2e-6f, 30.0e-6f };
    const float ozoneExt[3] = { 0.65e-6f * ozone * density, 1.88e-6f * ozone * density, 0.085e-6f * ozone * density };
    const float mieExt = 4.7e-6f * mie * density;
    float rayExt[3];
    for (int c = 0; c < 3; c++)
    {
        const float custom = std::max(skyNorm[c], 0.08f) * 26.0e-6f;
        rayExt[c] = (earthExt[c] + (custom - earthExt[c]) * 0.22f) * density;
    }

    constexpr float kPlanetR = 6360000.0f;
    constexpr float kTopR = 6440000.0f;
    const float roZ = kPlanetR + 10.0f;
    const float rdZ = std::max(z, 0.022f);
    const float rdH = std::sqrt(std::max(0.0f, 1.0f - rdZ * rdZ));
    const float b = roZ * rdZ;
    const float cTerm = roZ * roZ - kTopR * kTopR;
    const float rayLen = std::max(0.0f, -b + std::sqrt(std::max(0.0f, b * b - cTerm)));

    float odR = 0.0f, odM = 0.0f, odO = 0.0f;
    const float stepLen = rayLen * 0.25f;
    for (int i = 0; i < 4; i++)
    {
        const float dist = (float(i) + 0.5f) * stepLen;
        const float px = rdH * dist;
        const float pz = roZ + rdZ * dist;
        const float h = std::max(std::sqrt(px * px + pz * pz) - kPlanetR, 0.0f);
        odR += std::exp(-h / 8000.0f) * stepLen;
        odM += std::exp(-h / 1200.0f) * stepLen;
        odO += std::max(0.0f, 1.0f - std::abs(h - 25000.0f) / 15000.0f) * stepLen;
    }

    float directT[3];
    for (int ch = 0; ch < 3; ch++)
    {
        const float tau = rayExt[ch] * odR + mieExt * odM + ozoneExt[ch] * odO;
        directT[ch] = std::exp(-tau);
    }
    const float peakT = std::max({ directT[0], directT[1], directT[2] });
    const float grazingT = std::clamp(std::max(z, 0.0f) / 0.18f, 0.0f, 1.0f);
    const float grazing = 1.0f - grazingT * grazingT * (3.0f - 2.0f * grazingT);
    const float floorVal = peakT * 0.36f;
    for (int ch = 0; ch < 3; ch++)
    {
        const float ms = std::max(directT[ch], floorVal);
        outTrans[ch] = smoothHorizon * (directT[ch] + (ms - directT[ch]) * (grazing * 0.42f));
    }
}
