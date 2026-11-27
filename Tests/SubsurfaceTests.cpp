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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "qray/qray.h"
#include "Generated/ShaderCommonC.h"
#include "SubsurfacePacking.h"
#include "../editor/EditorMaterial.h"
#include "../editor/Scene.h"

namespace
{

struct CapturedMaterialUpload
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> albedo;
    std::vector<uint8_t> rme;
    std::vector<uint8_t> normal;
};

CapturedMaterialUpload g_lastUpload;
QrMaterial g_nextMaterialHandle = 100;
uint32_t g_destroyCount = 0;

bool Check(bool condition, const char *message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAILED: %s\n", message);
        return false;
    }
    return true;
}

bool NearlyEqual(float a, float b, float eps = 1e-4f)
{
    return std::fabs(a - b) <= eps;
}

static_assert(sizeof(qray::ShGeometryInstance) == 256,
              "ShGeometryInstance must stay 256 bytes");
static_assert(GEOM_INST_FLAG_SUBSURFACE == (1u << 15),
              "GEOM_INST_FLAG_SUBSURFACE must occupy bit 15");

bool TestPackingEndpointsAndPrecision()
{
    const QrFloat3D zeroColor = { { 0.0f, 0.0f, 0.0f } };
    if (!Check(qray::PackSubsurfaceParams(zeroColor, 0.0f, 0.0f) == 0u,
               "zero SSS params should pack to 0"))
    {
        return false;
    }

    const QrFloat3D fullColor = { { 1.0f, 1.0f, 1.0f } };
    const uint32_t fullPacked =
        qray::PackSubsurfaceParams(fullColor, qray::kSubsurfaceMaxRadiusMeters, 1.0f);
    const qray::UnpackedSubsurfaceParams fullUnpacked = qray::UnpackSubsurfaceParams(fullPacked);
    const bool fullColorOk = NearlyEqual(fullUnpacked.color[0], 1.0f) &&
                             NearlyEqual(fullUnpacked.color[1], 1.0f) &&
                             NearlyEqual(fullUnpacked.color[2], 1.0f);
    const bool fullScalarsOk =
        NearlyEqual(fullUnpacked.radiusMeters, qray::kSubsurfaceMaxRadiusMeters) &&
        NearlyEqual(fullUnpacked.strength, 1.0f);
    if (!Check(fullColorOk && fullScalarsOk, "full-scale SSS pack/unpack mismatch"))
    {
        return false;
    }

    const QrFloat3D skinColor = { { 0.92f, 0.35f, 0.18f } };
    const uint32_t skinPacked = qray::PackSubsurfaceParams(skinColor, 0.005f, 0.8f);
    const qray::UnpackedSubsurfaceParams skinUnpacked = qray::UnpackSubsurfaceParams(skinPacked);
    const bool skinColorOk = NearlyEqual(skinUnpacked.color[0], 0.92f, 0.05f) &&
                             NearlyEqual(skinUnpacked.color[1], 0.35f, 0.03f) &&
                             NearlyEqual(skinUnpacked.color[2], 0.18f, 0.04f);
    const bool skinScalarsOk = NearlyEqual(skinUnpacked.radiusMeters, 0.005f, 0.001f) &&
                               NearlyEqual(skinUnpacked.strength, 0.8f, 0.006f);
    return Check(skinColorOk && skinScalarsOk, "skin millimeter radius / color precision mismatch");
}

bool TestPackingMonotonicAndSanitization()
{
    const QrFloat3D skinColor = { { 0.92f, 0.35f, 0.18f } };
    const auto uSmall = qray::UnpackSubsurfaceParams(qray::PackSubsurfaceParams(skinColor, 0.002f, 0.5f));
    const auto uMed = qray::UnpackSubsurfaceParams(qray::PackSubsurfaceParams(skinColor, 0.02f, 0.5f));
    const auto uLarge = qray::UnpackSubsurfaceParams(qray::PackSubsurfaceParams(skinColor, 0.25f, 0.5f));
    if (!Check(uSmall.radiusMeters < uMed.radiusMeters && uMed.radiusMeters < uLarge.radiusMeters,
               "packed SSS radius is not strictly monotonic"))
    {
        return false;
    }

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const QrFloat3D badColor = { { nan, -2.0f, 5.0f } };
    const auto badUnpacked =
        qray::UnpackSubsurfaceParams(qray::PackSubsurfaceParams(badColor, inf, -0.5f));
    const bool colorSanitized = NearlyEqual(badUnpacked.color[0], 0.0f) &&
                                NearlyEqual(badUnpacked.color[1], 0.0f) &&
                                NearlyEqual(badUnpacked.color[2], 1.0f);
    const bool scalarsSanitized =
        NearlyEqual(badUnpacked.radiusMeters, 0.0f) && NearlyEqual(badUnpacked.strength, 0.0f);
    return Check(colorSanitized && scalarsSanitized,
                 "non-finite / out-of-range SSS params were not sanitized");
}

bool TestPassThroughAndInstanceLayout()
{
    const bool allowedOk =
        qray::IsSubsurfaceAllowedPassThrough(QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE) &&
        qray::IsSubsurfaceAllowedPassThrough(QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED);
    const bool blockedTranslucent =
        qray::IsSubsurfaceAllowedPassThrough(QR_GEOMETRY_PASS_THROUGH_TYPE_GLASS_REFLECT_REFRACT) ||
        qray::IsSubsurfaceAllowedPassThrough(QR_GEOMETRY_PASS_THROUGH_TYPE_WATER_REFLECT_REFRACT) ||
        qray::IsSubsurfaceAllowedPassThrough(QR_GEOMETRY_PASS_THROUGH_TYPE_WATER_ONLY_REFLECT);
    const bool blockedOther =
        qray::IsSubsurfaceAllowedPassThrough(QR_GEOMETRY_PASS_THROUGH_TYPE_MIRROR) ||
        qray::IsSubsurfaceAllowedPassThrough(QR_GEOMETRY_PASS_THROUGH_TYPE_PORTAL) ||
        qray::IsSubsurfaceAllowedPassThrough(QR_GEOMETRY_PASS_THROUGH_TYPE_ACID_REFLECT_REFRACT);
    if (!Check(allowedOk && !blockedTranslucent && !blockedOther,
               "IsSubsurfaceAllowedPassThrough gate failed"))
    {
        return false;
    }

    const size_t emissionOffset = offsetof(qray::ShGeometryInstance, defaultEmission);
    const size_t sssOffset = offsetof(qray::ShGeometryInstance, subsurfacePacked);
    return Check(sssOffset > emissionOffset && sssOffset == emissionOffset + sizeof(float),
                 "subsurfacePacked must replace _unused1 directly after defaultEmission");
}

EditorMaterialParams MakeSkinTestParams()
{
    EditorMaterialParams params;
    params.sssEnabled = true;
    params.sssStrength = 0.7f;
    params.sssRadius = 0.05f;
    params.sssColor[0] = 0.9f;
    params.sssColor[1] = 0.25f;
    params.sssColor[2] = 0.15f;
    return params;
}

std::vector<uint8_t> MakeSkinAndMetalAlbedo()
{
    std::vector<uint8_t> albedo(4 * 4 * 4, 40);
    for (size_t i = 0; i < 4 * 4; i++)
    {
        albedo[i * 4 + 3] = 255;
    }
    albedo[0] = 220;
    albedo[1] = 140;
    albedo[2] = 110;
    return albedo;
}

bool TestApplyEditorSubsurface()
{
    const EditorMaterialParams params = MakeSkinTestParams();
    QrGeometryUploadInfo upload = {};
    ApplyEditorMaterial(params, QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE, upload);
    const bool appliedScalars = (upload.flags & QR_GEOMETRY_UPLOAD_SUBSURFACE_BIT) != 0u &&
                                NearlyEqual(upload.subsurfaceStrength, 0.7f) &&
                                NearlyEqual(upload.subsurfaceRadius, 0.05f);
    if (!Check(appliedScalars && NearlyEqual(upload.subsurfaceColor.data[0], 0.9f),
               "ApplyEditorMaterial failed to populate SSS upload fields"))
    {
        return false;
    }

    ApplyEditorMaterial(params, QR_GEOMETRY_PASS_THROUGH_TYPE_MIRROR, upload);
    const bool cleared = (upload.flags & QR_GEOMETRY_UPLOAD_SUBSURFACE_BIT) == 0u &&
                         upload.subsurfaceRadius == 0.0f && upload.subsurfaceStrength == 0.0f;
    if (!Check(cleared, "ApplyEditorMaterial failed to suppress SSS on mirror"))
    {
        return false;
    }

    EditorMaterialParams disabledParams = params;
    disabledParams.sssEnabled = false;
    ApplyEditorMaterial(disabledParams, QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE, upload);
    const bool toggleOffCleared =
        (upload.flags & QR_GEOMETRY_UPLOAD_SUBSURFACE_BIT) == 0u &&
        upload.subsurfaceRadius == 0.0f && upload.subsurfaceStrength == 0.0f;
    return Check(toggleOffCleared, "ApplyEditorMaterial failed to suppress SSS when sssEnabled=false");
}

bool TestColorKeyAndFeatherSssMask()
{
    EditorMaterialParams params = MakeSkinTestParams();
    const std::vector<uint8_t> albedo = MakeSkinAndMetalAlbedo();

    EditorSssBlock colorBlock;
    colorBlock.color[0] = 220.0f / 255.0f;
    colorBlock.color[1] = 140.0f / 255.0f;
    colorBlock.color[2] = 110.0f / 255.0f;
    colorBlock.threshold = 0.08f;
    colorBlock.feather = 0.0f;
    colorBlock.factor = 0.8f;
    params.sssBlocks.push_back(colorBlock);

    const std::vector<uint8_t> hardMask = BuildEditorSssMask(params, 4, 4, albedo.data());
    const bool hardOk = hardMask.size() == 16 && hardMask[0] >= 200 && hardMask[0] <= 208 &&
                        hardMask[1] == 0 && hardMask[15] == 0;
    if (!Check(hardOk, "color-keyed BuildEditorSssMask failed"))
    {
        return false;
    }

    params.sssBlocks[0].feather = 2.0f;
    const std::vector<uint8_t> feathered = BuildEditorSssMask(params, 4, 4, albedo.data());
    return Check(feathered[1] > 0 && feathered[1] < feathered[0],
                 "feathered BuildEditorSssMask did not soften boundary");
}

bool TestPolygonAndTextureSssMask()
{
    EditorMaterialParams params = MakeSkinTestParams();
    const std::vector<uint8_t> albedo = MakeSkinAndMetalAlbedo();

    EditorSssBlock polyBlock;
    polyBlock.polyUv = { 0.5f, 0.5f, 1.0f, 0.5f, 1.0f, 1.0f, 0.5f, 1.0f };
    polyBlock.factor = 1.0f;
    params.sssBlocks.push_back(polyBlock);

    std::vector<uint8_t> texSss(4 * 4 * 4, 0);
    texSss[0] = 128;
    texSss[1] = 128;
    texSss[2] = 128;
    texSss[3] = 255;

    const std::vector<uint8_t> polyPlusTex =
        BuildEditorSssMask(params, 4, 4, albedo.data(), texSss.data(), 4, 4);
    const bool texPixelOk = polyPlusTex[0] >= 126 && polyPlusTex[0] <= 130;
    const bool polyOk = polyPlusTex[1] == 0 && polyPlusTex[3 * 4 + 3] == 255;
    return Check(texPixelOk && polyOk, "polygon + texture_sss max combination failed");
}

EditorScene MakeTestSkinScene()
{
    EditorScene scene;
    EditorTexture skin;
    skin.name = "skin";
    skin.width = 2;
    skin.height = 2;
    skin.rgba = {
        220, 140, 110, 255, 220, 140, 110, 255,
        40,  40,  40,  255, 40,  40,  40,  255,
    };

    EditorTexture nrm;
    nrm.name = "skin_nrm";
    nrm.width = 2;
    nrm.height = 2;
    nrm.rgba = {
        128, 128, 255, 64, 128, 128, 255, 64,
        128, 128, 255, 64, 128, 128, 255, 64,
    };

    scene.textures.push_back(skin);
    scene.textures.push_back(nrm);
    return scene;
}

int32_t ResolveSceneTexByName(const EditorScene &scene, const std::string &texName)
{
    for (size_t i = 0; i < scene.textures.size(); i++)
    {
        if (scene.textures[i].name == texName)
        {
            return int32_t(i);
        }
    }
    return -1;
}

bool TestUniformSssBakePreservesMetalness(const EditorScene &scene, EditorMaterialBakes &bakes,
                                          QrInstance fakeInstance)
{
    const auto resolveTex = [&scene](const std::string &texName) {
        return ResolveSceneTexByName(scene, texName);
    };

    EditorMaterialParams uniformSss;
    uniformSss.glossEnabled = true;
    uniformSss.normalTexture = "skin_nrm";
    uniformSss.metalnessFromNormalAlpha = true;
    uniformSss.metalnessFactor = 1.0f;
    uniformSss.sssEnabled = true;
    uniformSss.sssStrength = 1.0f;
    uniformSss.sssRadius = 0.03f;

    const QrMaterial mat = bakes.Get(fakeInstance, "skin", uniformSss, scene, {}, resolveTex);
    const bool normalOk = g_lastUpload.normal.size() == 16 && g_lastUpload.normal[3] == 255;
    const bool rmeOk = g_lastUpload.rme.size() == 16 && g_lastUpload.rme[1] >= 62 && g_lastUpload.rme[1] <= 66;
    return Check(mat != QR_NO_MATERIAL && normalOk && rmeOk,
                 "uniform SSS bake altered metalness_from_normal_alpha or left non-255 Normal.a");
}

bool TestMaskedSssBakeSynthesizesNormal(const EditorScene &scene, EditorMaterialBakes &bakes,
                                        QrInstance fakeInstance)
{
    const auto resolveTex = [&scene](const std::string &texName) {
        return ResolveSceneTexByName(scene, texName);
    };

    EditorMaterialParams maskedNoNormal;
    maskedNoNormal.sssEnabled = true;
    maskedNoNormal.sssStrength = 1.0f;
    maskedNoNormal.sssRadius = 0.03f;
    EditorSssBlock topHalf;
    topHalf.polyUv = { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.5f, 0.0f, 0.5f };
    maskedNoNormal.sssBlocks.push_back(topHalf);

    const QrMaterial mat = bakes.Get(fakeInstance, "skin", maskedNoNormal, scene, {}, resolveTex);
    const bool flatRgbOk = g_lastUpload.normal.size() == 16 && g_lastUpload.normal[0] == 128 &&
                           g_lastUpload.normal[1] == 128 && g_lastUpload.normal[2] == 255;
    const bool maskAlphaOk = g_lastUpload.normal[3] == 255 && g_lastUpload.normal[15] == 0;
    return Check(mat != QR_NO_MATERIAL && flatRgbOk && maskAlphaOk,
                 "masked SSS without normal map failed to synthesize flat normal + mask alpha");
}

bool TestEditorMaterialBakeNormalAlpha()
{
    const EditorScene scene = MakeTestSkinScene();
    EditorMaterialBakes bakes;
    QrInstance fakeInstance = reinterpret_cast<QrInstance>(static_cast<uintptr_t>(1));

    if (!TestUniformSssBakePreservesMetalness(scene, bakes, fakeInstance) ||
        !TestMaskedSssBakeSynthesizesNormal(scene, bakes, fakeInstance))
    {
        return false;
    }

    bakes.Destroy(fakeInstance);
    return Check(g_destroyCount == 2, "EditorMaterialBakes::Destroy cleanup mismatch");
}

EditorScene MakeMirrorTestScene()
{
    EditorScene scene;
    EditorTexture envTex;
    envTex.name = "env_glass";
    envTex.width = 2;
    envTex.height = 2;
    envTex.rgba = {
        180, 90, 220, 255, 180, 90, 220, 255,
        180, 90, 220, 255, 180, 90, 220, 255,
    };
    EditorOverlayStage stEnv;
    stEnv.map = "textures/effects/tinfxb.tga";
    stEnv.isEnvironment = true;
    EditorOverlayStage stGlow;
    stGlow.map = "env_glow.tga";
    envTex.baseStageIndex = 0u;
    envTex.shaderStages = { stEnv, stGlow };

    EditorTexture plainTex = envTex;
    plainTex.name = "plain_metal";
    plainTex.shaderStages.clear();

    EditorTexture glowTex;
    glowTex.name = "env_glow";
    glowTex.width = 2;
    glowTex.height = 2;
    glowTex.rgba = {
        0, 255, 0, 255, 0, 255, 0, 255,
        0, 255, 0, 255, 0, 255, 0, 255,
    };
    scene.textures = { envTex, plainTex, glowTex };
    return scene;
}

bool UploadedAlbedoStartsWith(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return g_lastUpload.albedo.size() >= 4 && g_lastUpload.albedo[0] == r &&
           g_lastUpload.albedo[1] == g && g_lastUpload.albedo[2] == b &&
           g_lastUpload.albedo[3] == a;
}

bool TestMirrorOverlayPromotionPredicate()
{
    EditorMaterialParams plain;
    EditorMaterialParams mirror;
    mirror.mirror = true;
    EditorMaterialParams glass;
    glass.type = EditorMaterialType::Glass;
    return Check(!EditorMaterialPromotesOverlayToTraced(plain) &&
                     EditorMaterialPromotesOverlayToTraced(mirror) &&
                     EditorMaterialPromotesOverlayToTraced(glass),
                 "EditorMaterialPromotesOverlayToTraced predicate mismatch");
}

bool TestMirrorEnvAndDiffuseBake()
{
    if (!TestMirrorOverlayPromotionPredicate())
    {
        return false;
    }

    const EditorScene scene = MakeMirrorTestScene();
    const auto resolveTex = [&scene](const std::string &texName) {
        return ResolveSceneTexByName(scene, texName);
    };

    EditorMaterialBakes bakes;
    QrInstance fakeInstance = reinterpret_cast<QrInstance>(static_cast<uintptr_t>(3));
    const uint32_t destroyBefore = g_destroyCount;

    EditorMaterialParams envMirror;
    envMirror.mirror = true;
    const QrMaterial envMirrorMat =
        bakes.Get(fakeInstance, "env_glass", envMirror, scene, {}, resolveTex);
    const bool whiteEnvOk =
        UploadedAlbedoStartsWith(255, 255, 255, 255) && !g_lastUpload.rme.empty() &&
        g_lastUpload.rme[0] == 0;
    const QrMaterial plainMirrorMat =
        bakes.Get(fakeInstance, "plain_metal", envMirror, scene, {}, resolveTex);

    EditorMaterialParams bakedPlainMirror = envMirror;
    bakedPlainMirror.glossEnabled = true;
    bakedPlainMirror.glossTexture = "env_glow";
    const QrMaterial bakedPlainMirrorMat =
        bakes.Get(fakeInstance, "plain_metal", bakedPlainMirror, scene, {}, resolveTex);
    const bool keptDiffuseOk = UploadedAlbedoStartsWith(180, 90, 220, 255);

    const QrMaterial forcedEnvMat =
        bakes.Get(fakeInstance, "plain_metal", envMirror, scene, {}, resolveTex, true);
    const bool forcedWhiteOk = UploadedAlbedoStartsWith(255, 255, 255, 255);

    bakes.Destroy(fakeInstance);
    const bool bakesOk = envMirrorMat != QR_NO_MATERIAL && plainMirrorMat == QR_NO_MATERIAL &&
                         bakedPlainMirrorMat != QR_NO_MATERIAL && forcedEnvMat != QR_NO_MATERIAL;
    return Check(bakesOk && whiteEnvOk && keptDiffuseOk && forcedWhiteOk &&
                     g_destroyCount == destroyBefore + 3,
                 "mirror promotion / neutral env albedo bake mismatch");
}

bool TestDisabledEmissiveStageSuppressesBake()
{
    const EditorScene scene = MakeMirrorTestScene();
    const auto resolveTex = [&scene](const std::string &texName) {
        return ResolveSceneTexByName(scene, texName);
    };

    EditorMaterialBakes bakes;
    QrInstance fakeInstance = reinterpret_cast<QrInstance>(static_cast<uintptr_t>(4));
    const uint32_t destroyBefore = g_destroyCount;

    EditorMaterialParams maskedGlow;
    maskedGlow.emissiveEnabled = true;
    maskedGlow.emissiveTexture = "env_glow";
    maskedGlow.disabledStagesMask = (1u << 1);
    const QrMaterial suppressedGlowMat =
        bakes.Get(fakeInstance, "env_glass", maskedGlow, scene, {}, resolveTex);
    maskedGlow.disabledStagesMask = 0u;
    const QrMaterial activeGlowMat =
        bakes.Get(fakeInstance, "env_glass", maskedGlow, scene, {}, resolveTex);

    bakes.Destroy(fakeInstance);
    const bool cleanupOk = g_destroyCount == destroyBefore + 1;
    return Check(suppressedGlowMat == QR_NO_MATERIAL && activeGlowMat != QR_NO_MATERIAL && cleanupOk,
                 "disabling emissive shader stage must suppress baked emission");
}

} // namespace

extern "C" {

QrResult QRCONV qrCreateMaterial(QrInstance, const QrMaterialCreateInfo *pCreateInfo,
                                 QrMaterial *pResult)
{
    if (pCreateInfo == nullptr || pResult == nullptr)
    {
        return QR_WRONG_ARGUMENT;
    }

    const size_t bytes =
        size_t(pCreateInfo->size.width) * size_t(pCreateInfo->size.height) * 4;
    g_lastUpload = {};
    g_lastUpload.width = pCreateInfo->size.width;
    g_lastUpload.height = pCreateInfo->size.height;

    if (pCreateInfo->textures.pDataAlbedoAlpha != nullptr)
    {
        const auto *ptr =
            static_cast<const uint8_t *>(pCreateInfo->textures.pDataAlbedoAlpha);
        g_lastUpload.albedo.assign(ptr, ptr + bytes);
    }
    if (pCreateInfo->textures.pDataRoughnessMetallicEmission != nullptr)
    {
        const auto *ptr =
            static_cast<const uint8_t *>(pCreateInfo->textures.pDataRoughnessMetallicEmission);
        g_lastUpload.rme.assign(ptr, ptr + bytes);
    }
    if (pCreateInfo->textures.pDataNormal != nullptr)
    {
        const auto *ptr =
            static_cast<const uint8_t *>(pCreateInfo->textures.pDataNormal);
        g_lastUpload.normal.assign(ptr, ptr + bytes);
    }

    *pResult = g_nextMaterialHandle++;
    return QR_SUCCESS;
}

QrResult QRCONV qrDestroyMaterial(QrInstance, QrMaterial)
{
    g_destroyCount++;
    return QR_SUCCESS;
}

} // extern "C"

int main()
{
    const bool ok = TestPackingEndpointsAndPrecision() && TestPackingMonotonicAndSanitization() &&
                    TestPassThroughAndInstanceLayout() && TestApplyEditorSubsurface() &&
                    TestColorKeyAndFeatherSssMask() && TestPolygonAndTextureSssMask() &&
                    TestEditorMaterialBakeNormalAlpha() &&
                    TestMirrorEnvAndDiffuseBake() &&
                    TestDisabledEmissiveStageSuppressesBake();
    return ok ? 0 : 1;
}
