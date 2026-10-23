#pragma once

#include <qray/qray.h>

// The Global tab of the game editor, as a record. Every field mirrors one of the
// `QRE_G_*` rows in Quake/qr_editor.c and defaults to the cvar value the game ships
// with, so an editor frame on a fresh scene looks like a game frame.
//
// The derived values -- the sky multiplier, the cloud block, the sun colour -- are
// computed in BuildFrameParams the same way Quake/gl_vidsdl.c computes them, because
// a host that recomputes them differently gets a different sky for the same sliders.
struct EditorRenderSettings
{
    // Global
    float brightness = 1.0f;

    // Lighting model. rt_truelight 0 is the legacy path, where the map's own light entities are
    // the light sources; 1 and 2 light the world from the emissive materials of its surfaces and
    // RT_UploadAllElights returns at once, so an entity light is not a light source at all. The
    // level area lights of that path are gated by rt_level_lights and the model ones by
    // rt_model_lights, as they are in RT_AddAliasEmissiveLights.
    float truelight = 1.0f;
    bool levelLights = false;
    bool modelLights = false;

    // Sky
    int32_t skyType = QR_SKY_TYPE_PROCEDURAL;
    float skyScale = 1.0f;
    float skyBrightness = 1.0f;
    float skyColor[3] = { 0.0f, 0.663f, 1.0f };
    int32_t skyAmbientLod = 4;
    float skyLightMultiplier = 1.0f;
    bool skyNee = true;

    // Clouds -- only drawn by QR_SKY_TYPE_PROCEDURAL.
    bool clouds = true;
    float cloudsColor[3] = { 1.0f, 1.0f, 1.0f };
    float cloudsAlpha = 1.0f;
    float cloudsCoverage = 0.2f;
    float cloudsDensity = 0.8f;
    float cloudsSpeed = 0.3f;
    int32_t cloudsQuality = 2;
    float cloudsHeight = 140000.0f;
    float cloudsThickness = 90000.0f;

    // Sun. The disc and the shafts are the same sun as the directional light the
    // editor uploads; `sunDiscScale` only sizes how bright the disc is painted.
    // The light itself -- whether it is drawn, its strength and its angles -- is
    // EditorSunState, because the level's `sky:` block owns two of those keys.
    float sunDiscScale = 4.0f;
    float sunColor[3] = { 1.0f, 0.933f, 0.749f };
    float sunDiscSize = 1.0f;

    // God rays
    bool godRays = true;
    float godRaysIntensity = 1.0f;
    int32_t godRaysQuality = 2;
    // How bright a sky area must be for the rays to come from it instead of the
    // centre of everything above the threshold. The search walks the classic sky
    // texture, which a standalone editor has none of, so the value is authored
    // and saved for the game rather than changing this preview.
    float godRaysSkyThreshold = 0.75f;

    // Volumetric fog. The ambient is not the sky tint: gl_vidsdl.c scales skyflatcolor,
    // the sky texture's own average, so `volumeAmbient` alone would make the fog the
    // colour of rt_sky_color -- pure blue for a Quake sky, and a world lit through that
    // fog loses every red channel it has.
    int32_t volumeType = 2;
    float volumeScatter = 0.3f;
    float volumeAmbient = 2.0f;
    float volumeFar = 1000.0f;
    float volumeLightIntensity = 250.0f;
    float volumeAsymmetry = 0.0f;

    // Tonemap
    float tonemapWhite = 20.0f;
    float tonemapChroma = 0.8f;
    float tonemapDesat = 0.0f;
    float tonemapMaxLog = 0.0f;
    float tonemapMinLog = -8.0f;
    int32_t tonemapType = 1;

    // Water / Acid: how every `water` and `acid` surface of the scene looks. The game's
    // editor keeps these in the System tab rather than on a surface, because they are
    // one look for the whole level. The speeds and the scale are stated in metres, as
    // their cvars are, and BuildEditorFrameParams turns them into the scene units the
    // frame block carries.
    float waterSpeed = 0.4f;
    float waterNormalStrength = 1.0f;
    float waterNormalSharpness = 5.0f;
    float waterScale = 1.0f;
    float waterColor[3] = { 171.0f / 255.0f, 193.0f / 255.0f, 210.0f / 255.0f };
    float acidColor[3] = { 122.0f / 255.0f, 143.0f / 255.0f, 21.0f / 255.0f };

    // Materials. gl_vidsdl.c fills QrDrawFrameTexturesParams from these cvars, and the panel
    // authors none of them, so the value is read from the game config and only falls back to
    // the cvar's own default. `emissionMapBoost` is the one that changes a level's brightness
    // most: the game multiplies rt_emis_mapboost by rt_emis_light_intensity and shades every
    // emissive texel with it.
    float normalMapStrength = 1.0f;      // rt_normalmap_stren
    float emissionMapBoost = 30.0f;      // rt_emis_mapboost
    float emissionLightIntensity = 1.0f; // rt_emis_light_intensity
    float emissionMaxScreenColor = 4.0f; // rt_emis_maxscrcolor
    float emissionSharpMask = 1.0f;      // rt_emis_sharpmask
    float talSelfLitOffset = 6.0f;       // rt_tal_selflit
    float minRoughness = 0.02f;          // rt_roughmin

    // Bloom. gl_vidsdl.c feeds two independent bloom stages: the emission multiplier the SDK
    // bloom pass reads, and the screen-space post-effect bloom. rt_bloom_emis_mult is the first
    // one; rt_bloom_intensity and its companions are the second. The SDK pass is handed a
    // strength of zero because the game does the same -- its halos come from the post effect --
    // and a host that leaves QrDrawFrameInfo::pBloomParams null instead gets the SDK's default
    // strength of one with an emission multiplier of 64, which blooms the whole frame.
    float bloomEmissionMultiplier = 50.0f; // rt_bloom_emis_mult
    float bloomIntensity = 0.022f;         // rt_bloom_intensity, post effect
    float bloomThreshold = 6.0f;           // rt_bloom_threshold
    float bloomKnee = 0.5f;                // rt_bloom_knee
    float bloomScatter = 0.7f;             // rt_bloom_scatter
    float bloomRadius = 0.04f;             // rt_bloom_radius
    int32_t bloomQuality = 2;              // rt_bloom_quality

    // Screen post effects. gl_vidsdl.c builds these from rt_vignette, rt_filmgrain,
    // rt_sharpen_strength, rt_local_exposure and rt_ef_crt on every frame, and the panel
    // authors none of them, so the defaults are the game config values: a preview that
    // left them out would be the renderer's ungraded output, brighter at the corners and
    // without the grade the game puts over the same frame.
    float vignetteIntensity = 0.5f;    // rt_vignette
    float vignetteStart = 0.45f;       // rt_vignette_start
    float vignetteEnd = 1.0f;          // rt_vignette_end
    float vignetteRoundness = 0.35f;   // rt_vignette_roundness
    float filmGrainIntensity = 0.5f;   // rt_filmgrain
    float filmGrainSize = 1.0f;        // rt_filmgrain_size
    float sharpenStrength = 0.5f;      // rt_sharpen_strength
    float localExposure = 0.1f;        // rt_local_exposure
    bool crtEnabled = false;           // rt_ef_crt

    // The field of view. The game adapts its 4:3 `fov` to the window and derives the
    // vertical field from that, so fov 90 with fov_adapt 1 is a 73.7 degree vertical
    // field whatever the aspect -- the renderer's own 60 degree default frames a level
    // through a lens the game never uses.
    float fovDegrees = 90.0f;          // fov
    bool fovAdapt = true;              // fov_adapt

    // Illumination. gl_vidsdl.c hands the SDK a QrDrawFrameIlluminationParams block every
    // frame, and a host that leaves the pointer null does not get these cvars ignored but
    // replaced: VulkanDevice.cpp:575 fills a fixed fallback instead, whose gradient cell is
    // one scene unit where the game asks for two metres -- eighty of them -- and whose
    // sensitivity gradients are the renderer's own rather than the level's. The defaults
    // below are the values the game ships in id1/config.cfg, so a preview of a level with
    // no config file is still the estimator the game would run on it.
    float denoiser = 1.0f;              // rt_denoiser
    float restir = 0.0f;                // rt_restir
    float restirCandidates = 8.0f;      // rt_restir_candidates
    float giLevel = 1.0f;               // rt_gi_level
    float neeSamples = 1.0f;            // rt_nee_samples
    float sensitivityDirect = 0.4f;     // rt_sensit_dir
    float sensitivityIndirect = 0.06f;  // rt_sensit_indir
    float sensitivitySpecular = 0.03f;  // rt_sensit_spec
    // gl_vidsdl.c states METRIC_TO_QUAKEUNIT(2.0f); the editor's scene unit is the Quake
    // unit -- kEditorUnitsPerMeter is 40 and QUAKEUNIT_IN_METERS is 0.025 -- so the same
    // two metres are eighty scene units here.
    float gradientCellMeters = 2.0f;
    float sunBounceRange = 2000.0f;     // rt_sky_sun_bounce_range
    float sunBounceScale = 1.0f;        // rt_sky_sun_bounce_scale
    float noTextures = 0.0f;            // rt_no_textures: 1 shades every surface with a flat albedo
    float antifirefly = 1.0f;           // rt_antifirefly, which the game passes outside the block
    int32_t q2DepthGrad = 1;            // rt_q2_depthgrad
    int32_t q2LightStats = 1;           // rt_q2_lightstats
    bool reflRefrEarlyOut = true;       // rt_reflrefr_earlyout
    // rt_renderscale: 100 renders at the window, and anything below renders smaller and
    // stretches the result, as the game's ResolutionToQray does. No upscaler is offered,
    // because the SDK's own upscalers need a host that can query them per device.
    float renderScale = 100.0f;

    // Level fog: a density of 0 draws none, which is what a map without a `fog`
    // key gets. The colour starts white because that is the engine's own default
    // for a level whose file carries no `color:` key, so the preview of such a
    // level is the fog the game shows rather than a black one.
    float fogColor[3] = { 1.0f, 1.0f, 1.0f };
    float fogDensity = 0.0f;
    float fogSkyBlend = 0.0f;
};

// The sun the editor draws: the directional light, the disc and the shafts are one
// sun, and the level's `sky:` block owns two of its keys, so the state lives
// beside the settings rather than inside them.
struct EditorSunState
{
    // rt_physical_sun: the game uploads no sun light while the cvar is off, and
    // without it there is nothing for the shafts to scatter.
    bool enabled = false;
    // The directional light's radiance before its colour: rt_sky_sun times the
    // fixup the game applies, so the default cvar value of 1 is
    // kEditorSkySunRadiance.
    float radiance = 16.0f;
    // rt_sky_sun_pitch and rt_sky_sun_yaw, which state the direction towards the
    // sun the way Quake's AngleVectors does: the game reads -forward as that
    // direction, so a pitch of 140 is a sun 40 degrees above the horizon and a
    // pitch of 40 would be the same sun behind the viewer. The values are kept in
    // the game's own words rather than as elevation and azimuth so a block the
    // panel captured reads back unchanged.
    float pitch = 140.0f;
    float yaw = 120.0f;
    // Not a cvar: RT_UploadSunLight uploads every sun with this angular size, so
    // the editor's own light has to be the same disc to shade the same way. The
    // size the sky shader paints is rt_sky_sun_size, which is EditorRenderSettings'
    // sunDiscSize because the block owns it.
    float angularDiameter = 0.05f;
};

// The radiance one unit of `rt_sky_sun` is worth, matching RT_FIXUP_LIGHT_INTENSITY
// followed by RT_SUN_LIGHT_INTENSITY_SCALE: rt_globallight_mult (10) times the
// Quake-to-meter area fix (1600) times 0.001.
inline constexpr float kEditorSkySunRadiance = 16.0f;

// The parameter blocks a frame built from EditorRenderSettings carries. Kept
// together so the pointers handed to QrDrawFrameInfo stay valid for the whole call.
struct EditorFrameParams
{
    QrDrawFrameSkyParams sky = {};
    QrDrawFrameTonemappingParams tonemap = {};
    QrDrawFrameVolumetricParams volume = {};
    QrDrawFrameLevelFogParams fog = {};
    QrDrawFrameReflectRefractParams reflectRefract = {};
    QrDrawFrameTexturesParams textures = {};
    QrDrawFrameIlluminationParams illumination = {};
    // The resolution block is the one the host owns rather than the scene: its custom size
    // is the window's, which only the caller knows, so Frame() fills it each frame.
    QrDrawFrameRenderResolutionParams resolution = {};
    // QrDrawFrameInfo::forceAntiFirefly sits outside the illumination block, so it is carried
    // beside it rather than inside the record the SDK reads.
    QrBool32 antifirefly = 0;
    QrDrawFrameBloomParams bloom = {};
    QrPostEffectsBloomParams bloomEffect = {};
    QrPostEffectsVignetteParams vignette = {};
    QrPostEffectsFilmGrainParams filmGrain = {};
    QrPostEffectsSharpenParams sharpen = {};
    QrPostEffectCRT crt = {};
    // Points at the blocks above, so they stay inside this record: the pointers are
    // re-established by the caller right before the draw, because a copy of this struct
    // would otherwise leave them pointing at the copy's source.
    QrDrawFramePostEffectsParams postEffects = {};
};

// Fills `out` from `settings`, deriving the values the SDK expects rather than
// passing the sliders straight through.
//
// `sunDirection` points towards the sun in scene units and drives the shafts, and
// `sunIntensity` is the directional light that sun already uploads; the volumetric
// scattering is that same sun, so the two stay in step when it moves or goes dark.
//
// `skyFlatColor` is the level's skyflatcolor (EditorScene::skyFlatColor), the average of
// the sky texture's opaque pixels: the game scales that by rt_volume_ambient to get the
// fog's ambient, and a scene without a sky texture -- or a null pointer -- falls back to
// the authored sky colour, which is the only colour such a scene has to stand for its sky.
void BuildEditorFrameParams(const EditorRenderSettings &settings, const float sunDirection[3],
                            float sunIntensity, const float viewerPosition[3],
                            const float *skyFlatColor, EditorFrameParams &out);

// The vertical field of view the game draws `settings` at in a window of this size, ported
// from AdaptFovx and CalcFovy (Quake/gl_screen.c). The game states a 4:3 horizontal `fov`,
// widens it to the window while fov_adapt is on and derives the vertical field from the
// result, so fov 90 is a 73.7 degree vertical field whatever the aspect -- not the 60 the
// renderer's own constant names. A window with no size yet falls back to 4:3.
float EditorFovYRadians(const EditorRenderSettings &settings, float width, float height);
