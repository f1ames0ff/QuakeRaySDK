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

    // Volumetric fog
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

// The four parameter blocks a frame built from EditorRenderSettings carries. Kept
// together so the pointers handed to QrDrawFrameInfo stay valid for the whole call.
struct EditorFrameParams
{
    QrDrawFrameSkyParams sky = {};
    QrDrawFrameTonemappingParams tonemap = {};
    QrDrawFrameVolumetricParams volume = {};
    QrDrawFrameLevelFogParams fog = {};
};

// Fills `out` from `settings`, deriving the values the SDK expects rather than
// passing the sliders straight through.
//
// `sunDirection` points towards the sun in scene units and drives the shafts, and
// `sunIntensity` is the directional light that sun already uploads; the volumetric
// scattering is that same sun, so the two stay in step when it moves or goes dark.
void BuildEditorFrameParams(const EditorRenderSettings &settings, const float sunDirection[3],
                            float sunIntensity, const float viewerPosition[3],
                            EditorFrameParams &out);
