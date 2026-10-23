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
    float sunDiscScale = 4.0f;
    float sunColor[3] = { 1.0f, 0.933f, 0.749f };
    float sunDiscSize = 1.0f;

    // God rays
    bool godRays = true;
    float godRaysIntensity = 1.0f;
    int32_t godRaysQuality = 2;

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
    // key gets.
    float fogColor[3] = { 0.0f, 0.0f, 0.0f };
    float fogDensity = 0.0f;
    float fogSkyBlend = 0.0f;
};

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
