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

#include "Assets.h"
#include "EditorGui.h"
#include "EditorLightSelection.h"
#include "EditorMaterial.h"
#include "EditorSession.h"
#include "EditorUi.h"
#include "EditorViewport.h"
#include "Scene.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace
{
QrMaterial g_nextTestMaterial = 1;
std::vector<uint8_t> g_lastBakedRme;
std::vector<uint8_t> g_lastBakedNormal;
uint32_t g_lastBakedWidth = 0;
uint32_t g_lastBakedHeight = 0;
uint32_t g_testBakeCreateCount = 0;
} // namespace

extern "C" {
QrResult QRCONV qrCreateMaterial(QrInstance qrInstance, const QrMaterialCreateInfo *pCreateInfo,
                                 QrMaterial *pResult)
{
    (void)qrInstance;
    if (pCreateInfo != nullptr)
    {
        g_lastBakedWidth = pCreateInfo->size.width;
        g_lastBakedHeight = pCreateInfo->size.height;
        const size_t bytes =
            size_t(pCreateInfo->size.width) * size_t(pCreateInfo->size.height) * 4;
        if (pCreateInfo->textures.pDataRoughnessMetallicEmission != nullptr)
        {
            const auto *src = static_cast<const uint8_t *>(
                pCreateInfo->textures.pDataRoughnessMetallicEmission);
            g_lastBakedRme.assign(src, src + bytes);
        }
        if (pCreateInfo->textures.pDataNormal != nullptr)
        {
            const auto *srcN = static_cast<const uint8_t *>(pCreateInfo->textures.pDataNormal);
            g_lastBakedNormal.assign(srcN, srcN + bytes);
        }
    }
    g_testBakeCreateCount++;
    if (pResult != nullptr)
    {
        *pResult = g_nextTestMaterial++;
    }
    return QR_SUCCESS;
}

QrResult QRCONV qrDestroyMaterial(QrInstance qrInstance, QrMaterial material)
{
    (void)qrInstance;
    (void)material;
    return QR_SUCCESS;
}

QrResult QRCONV qrUploadRasterizedGeometry(QrInstance qrInstance,
                                           const QrRasterizedGeometryUploadInfo *pUploadInfo,
                                           const float *pViewProjection,
                                           const QrViewport *pViewport)
{
    (void)qrInstance;
    (void)pUploadInfo;
    (void)pViewProjection;
    (void)pViewport;
    return QR_SUCCESS;
}

const char *QRCONV qrGetResultDescription(QrResult result)
{
    (void)result;
    return "ok";
}
}

void EditorWindow::GetCursorPosition(float &outX, float &outY) const
{
    outX = 0.0f;
    outY = 0.0f;
}

void EditorWindow::SetCursorVisible(bool visible)
{
    (void)visible;
}

void EditorWindow::SetMouseCapture(bool capture)
{
    (void)capture;
}

namespace
{

bool Check(bool cond, const char *msg)
{
    if (!cond)
    {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        return false;
    }
    return true;
}

bool Near(float a, float b, float eps = 1e-3f)
{
    return std::fabs(a - b) <= eps;
}

struct TestLightSpec
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float intensity = 300.0f;
    float radius = 16.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    int32_t style = 0;
    bool enabled = true;
};

EditorLight MakeTestLight(const TestLightSpec &spec)
{
    EditorLight l;
    l.position[0] = spec.x;
    l.position[1] = spec.y;
    l.position[2] = spec.z;
    l.radius = spec.radius;
    l.style = spec.style;
    l.enabled = spec.enabled;
    const float tint[3] = { spec.r, spec.g, spec.b };
    SetEditorLightTint(l, tint);
    SetEditorLightIntensity(l, spec.intensity);
    return l;
}

LightProjectionCamera MakeDefaultCam(float panelLeft = -1.0f, float minY = 0.0f)
{
    LightProjectionCamera cam;
    cam.origin[0] = 0.0f;
    cam.origin[1] = 0.0f;
    cam.origin[2] = 0.0f;
    cam.forward[0] = 1.0f;
    cam.forward[1] = 0.0f;
    cam.forward[2] = 0.0f;
    cam.right[0] = 0.0f;
    cam.right[1] = -1.0f;
    cam.right[2] = 0.0f;
    cam.up[0] = 0.0f;
    cam.up[1] = 0.0f;
    cam.up[2] = 1.0f;
    cam.tanHalfFovX = 1.0f;
    cam.tanHalfFovY = 1.0f;
    cam.viewportWidth = 1000.0f;
    cam.viewportHeight = 1000.0f;
    cam.visibleMinX = 0.0f;
    cam.visibleMinY = minY;
    cam.visibleMaxX = panelLeft;
    cam.visibleMaxY = 1000.0f;
    return cam;
}

void PopulateMarqueeTestLights(std::vector<EditorLight> &mapLights,
                               std::vector<EditorLight> &customLights)
{
    // At x = 100 forward, screenX = 500 - 5*y and screenY = 500 - 5*z.
    mapLights.push_back(MakeTestLight({ 100.0f, 20.0f, 0.0f, 300.0f }));
    mapLights.push_back(
        MakeTestLight({ 100.0f, -20.0f, -10.0f, 500.0f, 24.0f, 1.0f, 0.0f, 0.0f, 1, false }));
    mapLights.push_back(MakeTestLight({ 100.0f, -60.0f, 0.0f, 400.0f }));
    mapLights.push_back(MakeTestLight({ -50.0f, 0.0f, 0.0f, 200.0f }));
    mapLights.push_back(MakeTestLight({ 100.0f, 0.0f, 97.0f, 200.0f }));

    EditorLight offsetLight = MakeTestLight({ 100.0f, 80.0f, 0.0f, 350.0f });
    offsetLight.hasOffset = true;
    offsetLight.offset[1] = -70.0f;
    mapLights.push_back(offsetLight);

    customLights.push_back(MakeTestLight({ 100.0f, 0.0f, 10.0f, 600.0f }));
    customLights.push_back(MakeTestLight({ 100.0f, 80.0f, 0.0f, 600.0f }));
}

bool TestMarqueeVisibilityAndSelection()
{
    const LightProjectionCamera cam = MakeDefaultCam(760.0f, 24.0f);
    std::vector<EditorLight> mapLights;
    std::vector<EditorLight> customLights;
    PopulateMarqueeTestLights(mapLights, customLights);

    if (!Check(!IsMarqueeDragPastThreshold(100.0f, 100.0f, 102.0f, 101.0f),
               "sub-threshold jitter must not trigger marquee") ||
        !Check(IsMarqueeDragPastThreshold(100.0f, 100.0f, 105.0f, 100.0f),
               "drag past threshold must activate marquee"))
    {
        return false;
    }

    const MarqueeLightSelectionResult allVisible = SelectVisibleLightsInMarquee(
        cam, mapLights, customLights, { 999.0f, 999.0f, 0.0f, 0.0f, false, nullptr, nullptr });
    if (!Check(allVisible.mapIndices == std::vector<int32_t>({ 0, 1, 5 }),
               "full-screen marquee must exclude behind-camera, panel-covered, and menu-covered lights") ||
        !Check(allVisible.customIndices == std::vector<int32_t>({ 0, 1 }),
               "full-screen marquee missed visible custom lights"))
    {
        return false;
    }

    const MarqueeLightSelectionResult boxed = SelectVisibleLightsInMarquee(
        cam, mapLights, customLights, { 650.0f, 600.0f, 350.0f, 400.0f, false, nullptr, nullptr });
    if (!Check(boxed.mapIndices == std::vector<int32_t>({ 0, 1, 5 }),
               "tight marquee map selection mismatch") ||
        !Check(boxed.customIndices == std::vector<int32_t>({ 0 }),
               "tight marquee should exclude out-of-box custom light"))
    {
        return false;
    }

    const MarqueeLightSelectionResult added = SelectVisibleLightsInMarquee(
        cam, mapLights, customLights,
        { 50.0f, 450.0f, 150.0f, 550.0f, true, &boxed.mapIndices, &boxed.customIndices });
    if (!Check(added.mapIndices == std::vector<int32_t>({ 0, 1, 5 }),
               "additive marquee altered map indices unexpectedly") ||
        !Check(added.customIndices == std::vector<int32_t>({ 0, 1 }),
               "additive marquee failed to merge new custom index"))
    {
        return false;
    }

    LightProjectionCamera zeroCam;
    const MarqueeLightSelectionResult emptyRes = SelectVisibleLightsInMarquee(
        zeroCam, mapLights, customLights, { 0.0f, 0.0f, 100.0f, 100.0f, false, nullptr, nullptr });
    return Check(emptyRes.mapIndices.empty() && emptyRes.customIndices.empty(),
                 "zero viewport camera should return empty marquee selection");
}

EditorScene MakeTwoLeafTestScene()
{
    EditorScene scene;
    EditorBspNode root{};
    root.normal[0] = 1.0f;
    root.distance = 0.0f;
    root.children[0] = -1;
    root.children[1] = -2;
    scene.nodes.push_back(root);
    scene.worldClusters.numLeafs = 2;
    scene.worldClusters.leafCluster = { 2u, 1u };
    scene.worldClusters.contents = { -2, 0 };
    return scene;
}

bool VerifyInitialAggregateSpatialAndCone(const MultiLightAggregate &agg)
{
    const bool spatialOk = !agg.mixedSpot && agg.spot &&
                           agg.mixedPositionAxes == (0x1u | 0x4u) && agg.mixedDirectionAxes == 0u;
    return Check(spatialOk, "spot/position/direction mixed state mismatch") &&
           Check(!agg.mixedAngleInner && agg.mixedAngleOuter,
                 "angle_inner equal / angle_outer mixed mismatch");
}

bool VerifyInitialMixedAggregate(const MultiLightAggregate &agg)
{
    const bool flagsOk = agg.count == 2 && agg.allCustom && agg.anySpot;
    const bool enabledRadiusOk =
        !agg.mixedEnabled && agg.enabled && !agg.mixedRadius && Near(agg.radiusUnits, 16.0f);
    if (!Check(flagsOk && enabledRadiusOk, "initial flags/enabled/radius mismatch") ||
        !Check(agg.mixedIntensity && agg.mixedColor && agg.mixedStyle,
               "differing radiometry/style must be mixed"))
    {
        return false;
    }
    return VerifyInitialAggregateSpatialAndCone(agg);
}

bool VerifyBatchIntensityOverwrite(EditorLight &a, EditorLight &b)
{
    std::vector<const EditorLight *> cPtrs = { &a, &b };
    std::vector<bool> customFlags = { true, true };
    std::vector<EditorLight *> mPtrs = { &a, &b };

    ApplyBatchLightIntensity(mPtrs, 450.0f);
    if (!Check(Near(a.intensity, 450.0f) && Near(b.intensity, 450.0f),
               "batch intensity failed to overwrite both") ||
        !Check(!Near(a.tint[0], b.tint[0]) && Near(a.position[0], 10.0f) &&
                   Near(b.position[0], 30.0f),
               "batch intensity clobbered unrelated color/origin"))
    {
        return false;
    }

    const MultiLightAggregate agg = AggregateMultiLightState(cPtrs, &customFlags);
    return Check(!agg.mixedIntensity && Near(agg.intensity, 450.0f),
                 "intensity must become unmixed after batch set") &&
           Check(agg.mixedColor && agg.mixedStyle && agg.mixedPositionAxes == 0x5u &&
                     agg.mixedAngleOuter,
                 "other mixed flags disappeared prematurely");
}

bool VerifyBatchIntensityAndOrigin(EditorScene &scene, EditorLight &a, EditorLight &b)
{
    if (!VerifyBatchIntensityOverwrite(a, b))
    {
        return false;
    }
    std::vector<EditorLight *> mPtrs = { &a, &b };
    const float newPos[3] = { -20.0f, 999.0f, 999.0f };
    ApplyBatchLightOrigin(scene, mPtrs, 0x1u, newPos);
    const bool keptX = Near(a.position[0], -20.0f) && Near(b.position[0], -20.0f);
    const bool keptYZ = Near(a.position[1], 50.0f) && Near(b.position[1], 50.0f) &&
                        Near(a.position[2], 100.0f) && Near(b.position[2], 200.0f);
    return Check(keptX && keptYZ, "axis-masked origin X edit overwrote unrelated axes") &&
           Check(a.cluster == 1u && b.cluster == 1u,
                 "batch origin edit failed to re-resolve BSP cluster");
}

bool VerifyBatchTintStyleAndCone(EditorLight &a, EditorLight &b)
{
    std::vector<const EditorLight *> cPtrs = { &a, &b };
    std::vector<bool> customFlags = { true, true };
    std::vector<EditorLight *> mPtrs = { &a, &b };

    const float newTint[3] = { 0.25f, 0.50f, 0.75f };
    ApplyBatchLightTint(mPtrs, newTint);
    ApplyBatchLightEnabled(mPtrs, false);
    ApplyBatchLightStyle(mPtrs, 7);
    ApplyBatchLightAngleOuter(mPtrs, 60.0f);
    const MultiLightAggregate agg = AggregateMultiLightState(cPtrs, &customFlags);
    const bool converged =
        !agg.mixedColor && !agg.mixedEnabled && !agg.mixedStyle && !agg.mixedAngleOuter;
    const bool radianceOk = Near(a.color[1], 450.0f * kEditorLightIntensityScale * 0.5f) &&
                            Near(b.color[2], 450.0f * kEditorLightIntensityScale * 0.75f);
    return Check(converged, "batch color/enabled/style/angleOuter did not converge") &&
           Check(radianceOk, "batch tint did not recompute radiance using intensity scale");
}

bool VerifyEmitterShaftConvergence(const MultiLightAggregate &agg)
{
    const bool shaftsOk = !agg.mixedLightShafts && agg.lightShafts &&
                          !agg.mixedLightShaftsPower && Near(agg.lightShaftsPower, 2.5f);
    const bool distFalloffOk = !agg.mixedLightShaftsDistance &&
                               Near(agg.lightShaftsDistance, 512.0f) &&
                               !agg.mixedLightShaftsFalloff && Near(agg.lightShaftsFalloff, 1.75f);
    return Check(shaftsOk, "batch light_shafts/power failed to converge") &&
           Check(distFalloffOk, "batch shafts distance/falloff failed to converge") &&
           Check(!agg.mixedForceRasterize && agg.forceRasterize,
                 "batch forceRasterize failed to converge");
}

bool VerifyEmitterConvergenceFromDirectLights(EditorLight &a, EditorLight &b)
{
    a.emitterUniqueID = 101u;
    b.emitterUniqueID = 202u;
    std::vector<const EditorLight *> cPtrs = { &a, &b };
    std::vector<bool> emitterCustomFlags = { false, false };
    const MultiLightAggregate agg = AggregateMultiLightState(cPtrs, &emitterCustomFlags);

    const bool emitterMetaOk = agg.allEmitters && !agg.allCustom && agg.emitterCount == 2;
    const bool radiusOk = !agg.mixedRadius && Near(agg.radiusUnits, 32.0f);
    const bool coneOk =
        !agg.mixedSpot && !agg.spot && !agg.mixedAngleInner && Near(agg.angleInner, 22.0f);
    return Check(emitterMetaOk && radiusOk, "allEmitters flags or batch radius mismatch") &&
           Check(coneOk, "batch spot/angleInner failed to converge") &&
           VerifyEmitterShaftConvergence(agg);
}

bool VerifyAxisOffsetAndEmitterOverrides(EditorScene &scene, EditorLight &a, EditorLight &b)
{
    std::vector<const EditorLight *> cPtrs = { &a, &b };
    std::vector<EditorLight *> mPtrs = { &a, &b };
    const float newPos[3] = { -20.0f, 999.0f, 999.0f };
    const float batchOffset[3] = { -15.0f, 5.0f, -2.0f };
    ApplyBatchLightOrigin(scene, mPtrs, 0x1u, newPos);
    ApplyBatchLightHasOffset(scene, mPtrs, true);
    ApplyBatchLightOffset(scene, mPtrs, 0x2u, batchOffset);
    const bool offOk = a.hasOffset && b.hasOffset && Near(a.offset[1], 5.0f) &&
                       Near(b.offset[1], 5.0f) && Near(a.offset[0], 0.0f) && Near(b.offset[2], 0.0f);
    if (!Check(offOk, "axis-masked batch offset mutated unedited axes"))
    {
        return false;
    }

    const float batchDir[3] = { 0.0f, 1.0f, 0.0f };
    ApplyBatchLightDirection(mPtrs, 0x7u, batchDir);
    ApplyBatchLightAngleInner(mPtrs, 22.0f);
    ApplyBatchLightRadiusUnits(mPtrs, 32.0f);
    ApplyBatchLightShafts(mPtrs, true);
    ApplyBatchLightShaftsPower(mPtrs, 2.5f);
    ApplyBatchLightShaftsDistance(mPtrs, 512.0f);
    ApplyBatchLightShaftsFalloff(mPtrs, 1.75f);
    ApplyBatchLightForceRasterize(mPtrs, true);
    ApplyBatchLightSpot(mPtrs, false);

    if (!VerifyEmitterConvergenceFromDirectLights(a, b))
    {
        return false;
    }

    a.emitterUniqueID = 0u;
    std::vector<bool> mixedSourceFlags = { false, true };
    const MultiLightAggregate mixedSrc = AggregateMultiLightState(cPtrs, &mixedSourceFlags);
    return Check(!mixedSrc.allCustom && !mixedSrc.allEmitters && mixedSrc.mapEntityCount == 1 &&
                     mixedSrc.customCount == 1,
                 "mixed map+custom source counters incorrect");
}

bool TestMixedAggregationAndBatchOverwrite()
{
    EditorScene scene = MakeTwoLeafTestScene();
    EditorLight a =
        MakeTestLight({ 10.0f, 50.0f, 100.0f, 300.0f, 16.0f, 1.0f, 0.5f, 0.2f, 2, true });
    EditorLight b =
        MakeTestLight({ 30.0f, 50.0f, 200.0f, 600.0f, 16.0f, 0.2f, 0.8f, 1.0f, 5, true });
    a.spot = true;
    b.spot = true;
    a.direction[0] = 1.0f;
    b.direction[0] = 1.0f;
    a.angleInner = 10.0f;
    a.angleOuter = 30.0f;
    b.angleInner = 10.0f;
    b.angleOuter = 45.0f;

    std::vector<const EditorLight *> cPtrs = { &a, &b };
    std::vector<bool> customFlags = { true, true };
    if (!VerifyInitialMixedAggregate(AggregateMultiLightState(cPtrs, &customFlags)) ||
        !VerifyBatchIntensityAndOrigin(scene, a, b) ||
        !VerifyBatchTintStyleAndCone(a, b) ||
        !VerifyAxisOffsetAndEmitterOverrides(scene, a, b))
    {
        return false;
    }

    EditorEmitterLightOverride ov1;
    ov1.name = "flame.mdl";
    ov1.radius = 0.5f;
    ov1.intensity = 1.0f;
    ov1.offset[2] = 4.0f;
    EditorEmitterLightOverride ov2 = ov1;
    ov2.intensity = 3.0f;
    ov2.style = 4;
    ov2.hasStyle = true;
    const MultiEmitterAggregate eAgg = AggregateMultiEmitterOverrides({ ov1, ov2 });
    return Check(!eAgg.mixedRadius && eAgg.mixedIntensity && eAgg.mixedStyle &&
                     eAgg.mixedOffsetAxes == 0u,
                 "emitter multi-override aggregation mismatch");
}

bool ContainsColoredTextVertex(const ImDrawData *drawData, uint32_t expectedRgba)
{
    if (drawData == nullptr)
    {
        return false;
    }
    for (int i = 0; i < drawData->CmdListsCount; ++i)
    {
        const ImDrawList *dl = drawData->CmdLists[i];
        for (int v = 0; v < dl->VtxBuffer.Size; ++v)
        {
            if (dl->VtxBuffer[v].col == expectedRgba)
            {
                return true;
            }
        }
    }
    return false;
}

void CountQuestionGlyphMatches(const ImDrawData *drawData, const ImFontGlyph *qGlyph,
                               uint32_t expectedColor, int &outMatches, int &outGrayMatches)
{
    const uint32_t swatchGray = IM_COL32(170, 178, 190, 255);
    for (int i = 0; i < drawData->CmdListsCount; ++i)
    {
        const ImDrawList *dl = drawData->CmdLists[i];
        for (int v = 0; v + 3 < dl->VtxBuffer.Size; v++)
        {
            const ImDrawVert &vt = dl->VtxBuffer[v];
            if (std::fabs(vt.uv.x - qGlyph->U0) >= 1e-4f ||
                std::fabs(vt.uv.y - qGlyph->V0) >= 1e-4f)
            {
                continue;
            }
            if (vt.col == expectedColor)
            {
                outMatches++;
                outGrayMatches++;
            }
            else if (vt.col == swatchGray)
            {
                outGrayMatches++;
            }
        }
    }
}

void LogQuestionGlyphVertices(const ImDrawData *drawData, const ImFontGlyph *qGlyph)
{
    for (int i = 0; i < drawData->CmdListsCount; ++i)
    {
        const ImDrawList *dl = drawData->CmdLists[i];
        for (int v = 0; v + 3 < dl->VtxBuffer.Size; v++)
        {
            const ImDrawVert &vt = dl->VtxBuffer[v];
            if (std::fabs(vt.uv.x - qGlyph->U0) < 1e-4f &&
                std::fabs(vt.uv.y - qGlyph->V0) < 1e-4f)
            {
                std::fprintf(stderr, "  list=%d v=%d col=0x%08x pos=(%.1f,%.1f)\n", i, v, vt.col,
                             vt.pos.x, vt.pos.y);
            }
        }
    }
}

bool FindQuestionGlyphInDrawData(const ImDrawData *drawData, ImFontAtlas *atlas,
                                 uint32_t expectedColor)
{
    if (drawData == nullptr || atlas == nullptr)
    {
        return false;
    }
    ImFont *font = ImGui::GetFont();
    const ImFontBaked *baked = font->GetFontBaked(ImGui::GetFontSize());
    const ImFontGlyph *qGlyph =
        baked != nullptr ? const_cast<ImFontBaked *>(baked)->FindGlyphNoFallback('?') : nullptr;
    if (qGlyph == nullptr)
    {
        return false;
    }

    int matches = 0;
    int grayMatches = 0;
    CountQuestionGlyphMatches(drawData, qGlyph, expectedColor, matches, grayMatches);

    // 8 mixed "?" controls total (checkbox, sliderF, sliderI, combo, swatch, hex, X, Z); Y unmixed.
    if (matches < 7 || grayMatches != 8)
    {
        std::fprintf(
            stderr,
            "FindQuestionGlyphInDrawData: matches=%d (<7), grayMatches=%d (!=8), expectedCol=0x%08x\n",
            matches, grayMatches, expectedColor);
        LogQuestionGlyphVertices(drawData, qGlyph);
    }
    return matches >= 7 && grayMatches == 8;
}

bool TestEmitterMixedAggregationAndEdgeCases()
{
    std::vector<EditorEmitterLightOverride> overrides(2);
    overrides[0].name = "flame2";
    overrides[0].groupEdit = true;
    overrides[0].hasRadius = true;
    overrides[0].radius = 0.5f;
    overrides[0].hasIntensity = true;
    overrides[0].intensity = 2.0f;
    overrides[0].hasSpot = true;
    overrides[0].spot = true;
    overrides[0].hasDir = false;
    overrides[0].dir[2] = 1.0f;

    overrides[1].name = "flame2#1";
    overrides[1].groupEdit = false;
    overrides[1].hasRadius = true;
    overrides[1].radius = 1.5f;
    overrides[1].hasIntensity = true;
    overrides[1].intensity = 2.0f;
    overrides[1].hasSpot = true;
    overrides[1].spot = true;
    overrides[1].hasDir = true;
    overrides[1].dir[0] = 1.0f;
    overrides[1].dir[2] = 0.0f;

    const MultiEmitterAggregate agg = AggregateMultiEmitterOverrides(overrides);
    if (!Check(agg.count == 2 && agg.anyGroupEdit && agg.anyInstanceEdit && agg.mixedRadius &&
                   !agg.mixedIntensity && agg.anySpot && !agg.mixedSpot && !agg.mixedDirAxes,
               "mixed groupEdit + instance emitter aggregation mismatch"))
    {
        return false;
    }

    EditorLight mapL{};
    mapL.position[2] = 10.0f;
    mapL.intensity = 100.0f;
    EditorLight custL{};
    custL.position[1] = 50.0f;
    custL.position[2] = 10.0f;
    custL.intensity = 300.0f;
    const std::vector<const EditorLight *> mixedPtrs = { &mapL, &custL };
    const std::vector<bool> isCustom = { false, true };
    const MultiLightAggregate lagg = AggregateMultiLightState(mixedPtrs, &isCustom);
    return Check(!lagg.allCustom && lagg.mapEntityCount == 1 && lagg.customCount == 1 &&
                     lagg.mixedPositionAxes == 0x2u && lagg.mixedIntensity,
                 "mixed map + custom light aggregation mismatch");
}

bool VerifyGroupEditOverrideDirHandlers()
{
    EditorEmitterLightOverride groupOv;
    groupOv.name = "torch_group";
    groupOv.groupEdit = true;
    groupOv.spot = true;
    groupOv.hasDir = false;
    groupOv.dir[0] = 0.0f;
    groupOv.dir[1] = 0.0f;
    groupOv.dir[2] = 1.0f;

    EditorEmitterLightOverride instOv = groupOv;
    instOv.name = "torch_group#1";
    instOv.groupEdit = false;

    const float axisEditDir[3] = { 1.0f, 0.0f, 0.0f };
    ApplyEmitterDirectionAxes(groupOv, 0x1u, axisEditDir);
    ApplyEmitterDirectionAxes(instOv, 0x1u, axisEditDir);
    const bool groupPreservedAfterAxis =
        !groupOv.hasDir && Near(groupOv.dir[0], 0.0f) && Near(groupOv.dir[2], 1.0f);
    const bool instUpdatedAfterAxis =
        instOv.hasDir && Near(instOv.dir[0], std::sqrt(0.5f)) && Near(instOv.dir[2], std::sqrt(0.5f));
    if (!Check(groupPreservedAfterAxis, "axis direction edit modified groupEdit emitter override") ||
        !Check(instUpdatedAfterAxis, "axis direction edit failed to update instance emitter override"))
    {
        return false;
    }

    const float aimDir[3] = { 0.0f, -1.0f, 0.0f };
    ApplyEmitterAimDirection(groupOv, aimDir);
    ApplyEmitterAimDirection(instOv, aimDir);
    const bool groupPreservedAfterAim = !groupOv.hasDir && Near(groupOv.dir[2], 1.0f);
    const bool instUpdatedAfterAim =
        instOv.hasDir && Near(instOv.dir[1], -1.0f) && Near(instOv.dir[2], 0.0f);
    return Check(groupPreservedAfterAim, "camera aim modified groupEdit emitter override") &&
           Check(instUpdatedAfterAim, "camera aim failed to update instance emitter override");
}

bool TestGroupEditEmitterDirPreservation()
{
    EditorLight mapLight = MakeTestLight({ 0.0f, 0.0f, 0.0f });
    mapLight.spot = true;
    mapLight.direction[0] = 0.0f;
    mapLight.direction[1] = 1.0f;
    mapLight.direction[2] = 0.0f;

    EditorLight groupEmitter = MakeTestLight({ 10.0f, 0.0f, 0.0f });
    groupEmitter.emitterUniqueID = 10u;
    groupEmitter.groupEdit = true;
    groupEmitter.spot = true;
    groupEmitter.direction[0] = 0.0f;
    groupEmitter.direction[1] = 0.0f;
    groupEmitter.direction[2] = 1.0f;

    EditorLight instEmitter = MakeTestLight({ 20.0f, 0.0f, 0.0f });
    instEmitter.emitterUniqueID = 20u;
    instEmitter.groupEdit = false;
    instEmitter.spot = true;
    instEmitter.direction[0] = 0.0f;
    instEmitter.direction[1] = 1.0f;
    instEmitter.direction[2] = 0.0f;

    const std::vector<const EditorLight *> ptrs = { &mapLight, &groupEmitter, &instEmitter };
    const std::vector<bool> isCustom = { false, false, false };
    const MultiLightAggregate agg = AggregateMultiLightState(ptrs, &isCustom);
    const bool aggOk = !agg.allEmitters && agg.emitterCount == 2 && agg.mapEntityCount == 1 &&
                       agg.mixedDirectionAxes == 0u && Near(agg.direction[1], 1.0f);
    return Check(aggOk, "groupEdit emitter direction polluted mixed-light direction aggregation") &&
           VerifyGroupEditOverrideDirHandlers();
}

bool TestMixedStyleAggregationAndSteadyVsNone()
{
    EditorLight mapSteady = MakeTestLight({ 0.0f, 0.0f, 0.0f });
    mapSteady.style = 0;
    mapSteady.hasStyle = true;

    EditorLight customSteady = MakeTestLight({ 10.0f, 0.0f, 0.0f });
    customSteady.style = 0;
    customSteady.hasStyle = true;

    EditorLight emitterSteady = MakeTestLight({ 20.0f, 0.0f, 0.0f });
    emitterSteady.emitterUniqueID = 1u;
    emitterSteady.style = 0;
    emitterSteady.hasStyle = true;

    EditorLight emitterNone = MakeTestLight({ 30.0f, 0.0f, 0.0f });
    emitterNone.emitterUniqueID = 2u;
    emitterNone.style = 0;
    emitterNone.hasStyle = false;

    const std::vector<bool> flags3 = { false, true, false };
    const MultiLightAggregate steadyAgg =
        AggregateMultiLightState({ &mapSteady, &customSteady, &emitterSteady }, &flags3);
    if (!Check(!steadyAgg.mixedStyle && steadyAgg.styleIndex == 1 && steadyAgg.style == 0,
               "steady direct lights + steady emitter must aggregate to styleIndex=1 (steady)"))
    {
        return false;
    }

    const std::vector<bool> flags2 = { false, false };
    const MultiLightAggregate mixedAgg =
        AggregateMultiLightState({ &mapSteady, &emitterNone }, &flags2);
    if (!Check(mixedAgg.mixedStyle && mixedAgg.styleIndex == 1,
               "steady direct light + NONE emitter must flag mixedStyle"))
    {
        return false;
    }

    const MultiLightAggregate allNoneAgg =
        AggregateMultiLightState({ &emitterNone, &emitterNone }, &flags2);
    if (!Check(!allNoneAgg.mixedStyle && allNoneAgg.styleIndex == 0 && allNoneAgg.style == -1,
               "two NONE emitters must aggregate to styleIndex=0 (NONE)"))
    {
        return false;
    }

    EditorEmitterLightOverride ovSteady;
    ovSteady.name = "flame_steady";
    ovSteady.hasStyle = false;
    ovSteady.style = 4;
    ApplyEmitterStyleIndex(ovSteady, 1); // 1 == "steady" (style 0)
    if (!Check(ovSteady.hasStyle && ovSteady.style == 0,
               "assigning steady (index 1) must set hasStyle=true and style=0"))
    {
        return false;
    }

    EditorEmitterLightOverride ovNone = ovSteady;
    ovNone.name = "flame_none";
    ovNone.hasRadius = true;
    ovNone.radius = 0.75f;
    ApplyEmitterStyleIndex(ovNone, 0); // 0 == "NONE"
    if (!Check(!ovNone.hasStyle && ovNone.style == 0,
               "assigning NONE (index 0) must clear hasStyle=false"))
    {
        return false;
    }

    const std::string yamlBlock = RenderEmitterLightsBlock({ ovSteady, ovNone });
    const size_t nonePos = yamlBlock.find("- name: flame_none");
    return Check(yamlBlock.find("light_style: steady") != std::string::npos,
                 "steady emitter must serialize light_style: steady") &&
           Check(nonePos != std::string::npos &&
                     yamlBlock.find("light_style:", nonePos) == std::string::npos,
                 "NONE emitter must omit light_style when serialized alongside steady emitter");
}

struct ImGuiTestContextGuard
{
    ImGuiContext *ctx = nullptr;
    explicit ImGuiTestContextGuard(ImGuiContext *context) : ctx(context) {}
    ~ImGuiTestContextGuard()
    {
        if (ctx != nullptr)
        {
            ImGui::DestroyContext(ctx);
        }
    }
    ImGuiTestContextGuard(const ImGuiTestContextGuard &) = delete;
    ImGuiTestContextGuard &operator=(const ImGuiTestContextGuard &) = delete;
};

struct MixedWidgetHarness
{
    bool chkVal = false;
    float slFloatVal = 300.0f;
    int32_t slIntVal = 3;
    int32_t cmbVal = 0;
    float colorVal[3] = { 1.0f, 1.0f, 1.0f };
    float vecVal[3] = { 10.0f, 20.0f, 30.0f };
    uint8_t editedMask = 0;

    ImVec2 chkMin{};
    ImVec2 chkMax{};
    ImVec2 slFMin{};
    ImVec2 slFMax{};
    ImVec2 slIMin{};
    ImVec2 slIMax{};
    ImVec2 cmbMin{};
    ImVec2 cmbMax{};
    ImVec2 hexMin{};
    ImVec2 hexMax{};
    ImVec2 vecZMin{};
    ImVec2 vecZMax{};

    bool chkEdited = false;
    bool slFloatEdited = false;
    bool slIntEdited = false;
    bool cmbEdited = false;
    bool colorEdited = false;
    bool vecEdited = false;

    static EditorGuiRowOpts MakeMixedRowOpts(const char *rectName,
                                             std::function<void(const char *)> logFn,
                                             uint8_t mixedAxisMask = 0,
                                             uint8_t *editedAxisMask = nullptr)
    {
        EditorGuiRowOpts opts{};
        opts.logRect = std::move(logFn);
        opts.rectName = rectName;
        opts.mixed = true;
        opts.mixedAxisMask = mixedAxisMask;
        opts.editedAxisMask = editedAxisMask;
        return opts;
    }

    void RenderFrame()
    {
        static constexpr const char *kComboItems[] = { "steady", "flicker", "pulse" };
        chkEdited = false;
        slFloatEdited = false;
        slIntEdited = false;
        cmbEdited = false;
        colorEdited = false;
        vecEdited = false;
        editedMask = 0;

        ImGui::NewFrame();
        EditorGuiBeginPanel("LIGHT EDITOR", 1280.0f, 720.0f);

        chkEdited = EditorGuiCheckbox(
            "enabled", &chkVal,
            MakeMixedRowOpts("chk", [&](const char *) {
                chkMin = ImGui::GetItemRectMin();
                chkMax = ImGui::GetItemRectMax();
            }));
        EditorGuiRowOpts slFOpts = MakeMixedRowOpts("slF", [&](const char *) {
            slFMin = ImGui::GetItemRectMin();
            slFMax = ImGui::GetItemRectMax();
        });
        slFOpts.format = "%.0f";
        slFloatEdited = EditorGuiSliderFloat("light", &slFloatVal, 0.0f, 2000.0f, slFOpts);

        EditorGuiRowOpts slIOpts = MakeMixedRowOpts("slI", [&](const char *) {
            slIMin = ImGui::GetItemRectMin();
            slIMax = ImGui::GetItemRectMax();
        });
        slIOpts.format = "%d";
        slIntEdited = EditorGuiSliderInt("style_i", &slIntVal, 0, 10, slIOpts);

        cmbEdited = EditorGuiCombo(
            "style", &cmbVal, kComboItems, 3,
            MakeMixedRowOpts("cmb", [&](const char *) {
                cmbMin = ImGui::GetItemRectMin();
                cmbMax = ImGui::GetItemRectMax();
            }));
        colorEdited = EditorGuiColorHex(
            "color", colorVal,
            MakeMixedRowOpts("hex", [&](const char *) {
                hexMin = ImGui::GetItemRectMin();
                hexMax = ImGui::GetItemRectMax();
            }));
        vecEdited = EditorGuiVec3Input(
            "origin", vecVal, 2.0f,
            MakeMixedRowOpts(
                "vecZ",
                [&](const char *) {
                    vecZMin = ImGui::GetItemRectMin();
                    vecZMax = ImGui::GetItemRectMax();
                },
                0x5u, &editedMask));

        EditorGuiEndPanel();
        ImGui::Render();
    }

    void ClickAt(ImGuiIO &io, float x, float y, bool *outEditedOnDown = nullptr,
                 const bool *watchedFlag = nullptr)
    {
        io.AddMousePosEvent(x, y);
        RenderFrame();
        io.AddMouseButtonEvent(0, true);
        RenderFrame();
        if (outEditedOnDown != nullptr && watchedFlag != nullptr)
        {
            *outEditedOnDown = *watchedFlag;
        }
        io.AddMouseButtonEvent(0, false);
        RenderFrame();
    }
};

bool VerifyMixedGlyphsAndHover(MixedWidgetHarness &h, ImGuiIO &io)
{
    h.RenderFrame();
    const uint32_t expectedGray = IM_COL32(148, 156, 168, 255);
    const bool hasGrayGlyphs =
        ContainsColoredTextVertex(ImGui::GetDrawData(), expectedGray) &&
        FindQuestionGlyphInDrawData(ImGui::GetDrawData(), io.Fonts, expectedGray);
    if (!Check(hasGrayGlyphs, "EditorGui mixed widgets did not emit gray '?' glyphs into ImDrawList"))
    {
        return false;
    }

    io.AddMousePosEvent(h.slFMin.x + (h.slFMax.x - h.slFMin.x) * 0.5f,
                        (h.slFMin.y + h.slFMax.y) * 0.5f);
    h.RenderFrame();
    return Check(!h.chkEdited && !h.slFloatEdited && !h.slIntEdited && !h.cmbEdited &&
                     !h.colorEdited && !h.vecEdited && Near(h.slFloatVal, 300.0f),
                 "hovering mixed '?' widgets triggered spurious edit");
}

bool VerifyMixedCheckboxAndSliders(MixedWidgetHarness &h, ImGuiIO &io)
{
    h.ClickAt(io, (h.chkMin.x + h.chkMax.x) * 0.5f, (h.chkMin.y + h.chkMax.y) * 0.5f);
    if (!Check(h.chkEdited && h.chkVal, "clicking mixed '?' checkbox must apply checked=true"))
    {
        return false;
    }

    bool slFEditedDown = false;
    h.ClickAt(io, h.slFMin.x + (h.slFMax.x - h.slFMin.x) * 0.75f,
              (h.slFMin.y + h.slFMax.y) * 0.5f, &slFEditedDown, &h.slFloatEdited);
    if (!Check((slFEditedDown || h.slFloatEdited) && h.slFloatVal > 1000.0f,
               "clicking mixed '?' SliderFloat did not update value"))
    {
        return false;
    }

    bool slIMidEditedDown = false;
    h.ClickAt(io, h.slIMin.x + (h.slIMax.x - h.slIMin.x) * 0.50f,
              (h.slIMin.y + h.slIMax.y) * 0.5f, &slIMidEditedDown, &h.slIntEdited);
    if (!Check((slIMidEditedDown || h.slIntEdited) && h.slIntVal == 5,
               "clicking mixed '?' SliderInt at exact midpoint failed to emit edit"))
    {
        return false;
    }

    bool slIEditedDown = false;
    h.ClickAt(io, h.slIMin.x + (h.slIMax.x - h.slIMin.x) * 0.80f,
              (h.slIMin.y + h.slIMax.y) * 0.5f, &slIEditedDown, &h.slIntEdited);
    return Check((slIEditedDown || h.slIntEdited) && h.slIntVal >= 7,
                 "clicking mixed '?' SliderInt did not update value");
}

bool VerifyMixedHexAndCombo(MixedWidgetHarness &h, ImGuiIO &io)
{
    h.ClickAt(io, (h.hexMin.x + h.hexMax.x) * 0.5f, (h.hexMin.y + h.hexMax.y) * 0.5f);
    io.AddInputCharactersUTF8("FF0000");
    h.RenderFrame();
    io.AddKeyEvent(ImGuiKey_Enter, true);
    h.RenderFrame();
    const bool hexCommitted = h.colorEdited;
    io.AddKeyEvent(ImGuiKey_Enter, false);
    h.RenderFrame();
    if (!Check((hexCommitted || h.colorEdited) && Near(h.colorVal[0], 1.0f) &&
                   Near(h.colorVal[1], 0.0f) && Near(h.colorVal[2], 0.0f),
               "typing FF0000 + Enter into mixed '?' ColorHex failed to commit"))
    {
        return false;
    }

    const float cmbMidX = (h.cmbMin.x + h.cmbMax.x) * 0.5f;
    const float cmbMidY = (h.cmbMin.y + h.cmbMax.y) * 0.5f;
    const float itemH = (h.cmbMax.y - h.cmbMin.y);

    h.ClickAt(io, cmbMidX, cmbMidY);
    bool cmbEditedOnClick = false;
    h.ClickAt(io, cmbMidX, h.cmbMax.y + itemH * 1.25f, &cmbEditedOnClick, &h.cmbEdited);
    if (!Check((cmbEditedOnClick || h.cmbEdited) && h.cmbVal == 1,
               "selecting item 1 from mixed '?' Combo did not apply value=1"))
    {
        return false;
    }

    h.ClickAt(io, cmbMidX, cmbMidY);
    bool cmbEditedSameFirstVal = false;
    h.ClickAt(io, cmbMidX, h.cmbMax.y + itemH * 1.25f, &cmbEditedSameFirstVal, &h.cmbEdited);
    return Check((cmbEditedSameFirstVal || h.cmbEdited) && h.cmbVal == 1,
                 "selecting first light's existing value from mixed '?' Combo failed to emit batch edit");
}

bool VerifyMixedVec3PartialEdit(MixedWidgetHarness &h, ImGuiIO &io)
{
    const ImVec2 zCenter((h.vecZMin.x + h.vecZMax.x) * 0.5f,
                         (h.vecZMin.y + h.vecZMax.y) * 0.5f);
    io.AddMousePosEvent(zCenter.x, zCenter.y);
    h.RenderFrame();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddMouseButtonEvent(0, true);
    h.RenderFrame();
    io.AddMouseButtonEvent(0, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    h.RenderFrame();
    io.AddKeyEvent(ImGuiKey_Enter, true);
    h.RenderFrame();
    const bool ctrlEnterEdited = h.vecEdited;
    const uint8_t ctrlEnterMask = h.editedMask;
    io.AddKeyEvent(ImGuiKey_Enter, false);
    h.RenderFrame();
    if (!Check(ctrlEnterEdited && ctrlEnterMask == 0x4u && Near(h.vecVal[2], 30.0f),
               "Ctrl+Click + Enter keeping first-light value on mixed '?' Vec3 failed to emit"))
    {
        return false;
    }

    io.DeltaTime = 0.5f;
    h.RenderFrame();
    io.DeltaTime = 1.0f / 60.0f;

    io.AddMousePosEvent(zCenter.x, zCenter.y);
    h.RenderFrame();
    io.AddMouseButtonEvent(0, true);
    h.RenderFrame();
    io.AddMousePosEvent(zCenter.x + 50.0f, zCenter.y);
    h.RenderFrame();
    const bool vecEditedDuringDrag = h.vecEdited;
    const uint8_t maskDuringDrag = h.editedMask;
    io.AddMouseButtonEvent(0, false);
    h.RenderFrame();

    return Check((vecEditedDuringDrag || h.vecEdited) && maskDuringDrag == 0x4u &&
                     Near(h.vecVal[0], 10.0f) && Near(h.vecVal[1], 20.0f) &&
                     !Near(h.vecVal[2], 30.0f),
                 "dragging mixed '?' Vec3 Z axis failed to isolate Z edit or clobbered X/Y");
}

bool TestImGuiMixedQuestionRenderingAndEdit()
{
    IMGUI_CHECKVERSION();
    ImGuiTestContextGuard ctxGuard(ImGui::CreateContext());
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;

    unsigned char *pixels = nullptr;
    int texW = 0;
    int texH = 0;
    io.Fonts->AddFontDefault();
    io.Fonts->GetTexDataAsRGBA32(&pixels, &texW, &texH);
    io.Fonts->SetTexID(ImTextureID(uintptr_t(1)));

    EditorGuiApplyStyle(1280.0f, 720.0f);

    MixedWidgetHarness h;
    return VerifyMixedGlyphsAndHover(h, io) && VerifyMixedCheckboxAndSliders(h, io) &&
           VerifyMixedHexAndCombo(h, io) && VerifyMixedVec3PartialEdit(h, io);
}

bool VerifyNoOpDirectionMutationAndStyleGate()
{
    EditorEmitterLightOverride groupOv;
    groupOv.name = "textures/sfx/torch";
    groupOv.groupEdit = true;
    groupOv.spot = true;
    groupOv.hasSpot = true;
    groupOv.dir[0] = 0.0f;
    groupOv.dir[1] = 1.0f;
    groupOv.dir[2] = 0.0f;
    const EditorEmitterLightOverride beforeGroup = groupOv;

    const float newDir[3] = { 0.0f, 0.0f, -1.0f };
    ApplyEmitterDirectionAxes(groupOv, 0x7u, newDir);
    if (!(groupOv == beforeGroup) || groupOv.hasDir)
    {
        return false;
    }
    ApplyEmitterAimDirection(groupOv, newDir);
    if (!(groupOv == beforeGroup) || groupOv.hasDir)
    {
        return false;
    }

    EditorEmitterLightOverride instOv = beforeGroup;
    instOv.groupEdit = false;
    const EditorEmitterLightOverride beforeInst = instOv;
    ApplyEmitterStyleIndex(instOv, 0);
    if (!(instOv == beforeInst))
    {
        return false;
    }
    ApplyEmitterStyleIndex(instOv, 1);
    if (instOv == beforeInst || !instOv.hasStyle || instOv.style != 0)
    {
        return false;
    }
    ApplyEmitterAimDirection(instOv, newDir);
    return instOv.hasDir && std::fabs(instOv.dir[2] + 1.0f) < 1e-4f;
}

bool VerifyEmitterResetAfterGroupToggleAndAlias()
{
    EditorEmitterLightOverride inst10;
    inst10.name = "sfx/flame2#10";
    inst10.groupEdit = false;
    inst10.hasIntensity = true;
    inst10.intensity = 3.5f;

    EditorEmitterLightOverride inst11;
    inst11.name = "sfx/flame2#11";
    inst11.groupEdit = false;
    inst11.hasIntensity = true;
    inst11.intensity = 4.0f;

    const std::vector<EditorEmitterLightOverride> baseline = { inst10, inst11 };

    EditorEmitterLightOverride other;
    other.name = "textures/sfx/torch";
    other.groupEdit = true;
    other.hasIntensity = true;
    other.intensity = 9.0f;
    std::vector<EditorEmitterLightOverride> current = { inst11, other };

    if (!ResetSingleEmitterOverride("textures/sfx/flame2.tga", 10u, current, baseline))
    {
        return false;
    }
    const EditorEmitterLightOverride *restored10 = FindEmitterOverrideExact(current, "sfx/flame2#10");
    return restored10 != nullptr && !restored10->groupEdit &&
           std::fabs(restored10->intensity - 3.5f) < 1e-4f &&
           FindEmitterOverrideExact(current, "sfx/flame2#11") != nullptr &&
           FindEmitterOverrideExact(current, "textures/sfx/torch") != nullptr;
}

bool VerifyEmitterResetOrderAndInstanceCleanup()
{
    EditorEmitterLightOverride a;
    a.name = "textures/sfx/flame1#1";
    a.groupEdit = false;
    a.hasIntensity = true;
    a.intensity = 2.0f;

    EditorEmitterLightOverride b;
    b.name = "textures/sfx/flame2#2";
    b.groupEdit = false;
    b.hasIntensity = true;
    b.intensity = 5.0f;

    const std::vector<EditorEmitterLightOverride> baseline = { a, b };
    std::vector<EditorEmitterLightOverride> current = baseline;
    current[0].intensity = 12.0f;

    if (!ResetSingleEmitterOverride("textures/sfx/flame1", 1u, current, baseline) ||
        !(current == baseline) ||
        ResetSingleEmitterOverride("textures/sfx/flame1", 1u, current, baseline))
    {
        return false;
    }

    EditorEmitterLightOverride grp;
    grp.name = "sfx/torch";
    grp.groupEdit = true;
    grp.hasIntensity = true;
    grp.intensity = 2.0f;
    EditorEmitterLightOverride inst = grp;
    inst.name = "textures/sfx/torch#20";
    inst.groupEdit = false;
    inst.intensity = 7.0f;

    const std::vector<EditorEmitterLightOverride> grpBaseline = { grp };
    std::vector<EditorEmitterLightOverride> grpCurrent = { grp, inst };
    return ResetSingleEmitterOverride("textures/sfx/torch", 20u, grpCurrent, grpBaseline) &&
           grpCurrent == grpBaseline;
}

bool VerifyEmitterNameCandidatesAndReset()
{
    std::vector<std::string> cands;
    BuildEmitterOverrideCandidates("textures/SFX/Flame2:42.tga", cands);
    const bool hasCanonical =
        std::find(cands.begin(), cands.end(), "textures/sfx/flame2:42") != cands.end();
    const bool hasShort = std::find(cands.begin(), cands.end(), "sfx/flame2:42") != cands.end();
    const bool hasBase =
        std::find(cands.begin(), cands.end(), "textures/sfx/flame2") != cands.end();
    return NormalizeEmitterNameBase("*12") == "#12" &&
           NormalizeEmitterNameBase("textures/SFX/Flame2.TGA") == "textures/sfx/flame2" &&
           hasCanonical && hasShort && hasBase && VerifyNoOpDirectionMutationAndStyleGate() &&
           VerifyEmitterResetAfterGroupToggleAndAlias() &&
           VerifyEmitterResetOrderAndInstanceCleanup();
}

bool VerifyAssetReadIgnoresDirectory()
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "qray_editor_asset_dir_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "default", ec);

    EditorAssetSource assets;
    assets.AddDirectory(root.string());
    std::vector<uint8_t> bytes;
    const bool ok = Check(!assets.Exists("default"), "Exists must return false for a directory") &&
                    Check(!assets.ReadFile("default", bytes) && bytes.empty(),
                          "ReadFile must return false without throwing on a directory");

    std::filesystem::remove_all(root, ec);
    return ok;
}

EditorLevelLight MakeSampleLevelLight(const std::string &className, float x, bool enabled,
                                        bool forceRasterize, bool spot)
{
    EditorLevelLight light;
    light.className = className;
    light.origin[0] = x;
    light.origin[1] = 20.0f;
    light.origin[2] = 64.0f;
    light.color[0] = 0.9f;
    light.color[1] = 0.5f;
    light.color[2] = 0.2f;
    light.radius = 0.5f;
    light.intensity = 2.5f;
    light.spot = spot;
    light.direction[0] = 0.0f;
    light.direction[1] = 0.0f;
    light.direction[2] = -1.0f;
    light.angleInner = 15.0f;
    light.angleOuter = 35.0f;
    light.enabled = enabled;
    light.forceRasterize = forceRasterize;
    return light;
}

bool WriteTextAndLoadDoc(const std::filesystem::path &path, const std::string &text,
                         EditorLevelDocument &doc)
{
    std::string error;
    if (!Check(WriteEditorFile(path.string(), text, error), "WriteEditorFile failed"))
    {
        return false;
    }
    return Check(doc.Load(path.string(), error), "EditorLevelDocument::Load failed");
}

bool VerifyPrimaryStandardLightProps(const EditorLevelLight &light)
{
    if (!Check(light.className == "textures/gothic_light/ironcrosslt2",
               "standard light class mismatch") ||
        !Check(light.forceRasterize && !light.enabled && light.spot,
               "standard light flags mismatch"))
    {
        return false;
    }

    return Check(std::fabs(light.intensity - 2.5f) < 0.01f &&
                     std::fabs(light.color[0] - 0.9f) < 0.01f,
                 "standard light radiometry mismatch");
}

bool VerifyFullStandardAndCustomRead(const EditorLevelDocument::ReadResult &fullRead)
{
    if (!Check(fullRead.hasLights && fullRead.lights.size() == 3,
               "expected 3 saved standard+custom lights on reload") ||
        !VerifyPrimaryStandardLightProps(fullRead.lights[0]))
    {
        return false;
    }

    return Check(fullRead.lights[1].className == "light" && fullRead.lights[1].enabled,
                 "second standard light mismatch") &&
           Check(fullRead.lights[2].className == "custom",
                 "custom light should round-trip class: custom") &&
           Check(HasSavedStandardLightsBlock(fullRead.lights),
                 "full read must have HasSavedStandardLightsBlock == true") &&
           Check(IsSavedStandardLightEntry(fullRead.lights[0]) &&
                     IsSavedStandardLightEntry(fullRead.lights[1]) &&
                     !IsSavedStandardLightEntry(fullRead.lights[2]),
                 "standard+custom entry classification mismatch");
}

bool VerifyPartialStandardAndCustomRead(const EditorLevelDocument::ReadResult &partialRead)
{
    return Check(partialRead.hasLights && partialRead.lights.size() == 2,
                 "expected 2 lights after partial delete") &&
           Check(partialRead.lights[0].className == "textures/gothic_light/ironcrosslt2" &&
                     partialRead.lights[1].className == "custom",
                 "partial standard light deletion should persist across reload") &&
           Check(HasSavedStandardLightsBlock(partialRead.lights) &&
                     IsSavedStandardLightEntry(partialRead.lights[0]) &&
                     !IsSavedStandardLightEntry(partialRead.lights[1]),
                 "partial read classification mismatch");
}

bool VerifyStandardAndCustomRoundTrip(const std::filesystem::path &tempDir)
{
    const std::filesystem::path yamlPath = tempDir / "qray.lights.yaml";
    const std::string seedYaml =
        "q3dm1:\n"
        "  lights:\n"
        "    - origin: 10 20 64\n"
        "      class: textures/gothic_light/ironcrosslt2\n"
        "      custom_note: keep_me\n";

    EditorLevelDocument doc;
    if (!WriteTextAndLoadDoc(yamlPath, seedYaml, doc))
    {
        return false;
    }

    std::vector<EditorLevelLight> lights = {
        MakeSampleLevelLight("textures/gothic_light/ironcrosslt2", 10.0f, false, true, true),
        MakeSampleLevelLight("light", 100.0f, true, false, false),
        MakeSampleLevelLight("custom", 220.0f, true, false, false),
    };

    const std::string merged =
        doc.WriteMerged("q3dm1", EditorLevelSky{}, EditorLevelFog{}, lights, true);
    if (!Check(merged.find("class: custom") != std::string::npos,
               "custom light should serialize class: custom") ||
        !WriteTextAndLoadDoc(yamlPath, merged, doc) ||
        !VerifyFullStandardAndCustomRead(doc.ReadLevel("q3dm1")))
    {
        return false;
    }

    lights.erase(lights.begin() + 1);
    const std::string partialMerged =
        doc.WriteMerged("q3dm1", EditorLevelSky{}, EditorLevelFog{}, lights, true);
    if (!Check(partialMerged.find("custom_note: keep_me") != std::string::npos,
               "unknown light subkey should stay preserved") ||
        !WriteTextAndLoadDoc(yamlPath, partialMerged, doc))
    {
        return false;
    }

    return VerifyPartialStandardAndCustomRead(doc.ReadLevel("q3dm1"));
}

bool VerifyLegacyLightsBlock(const std::filesystem::path &tempDir, EditorLevelDocument &doc)
{
    const std::string legacyYaml =
        "q3dm1:\n"
        "  lights:\n"
        "    - origin: 1 2 3\n"
        "      intensity: 1.5\n"
        "      color: ffffff\n"
        "      enabled: true\n"
        "    - origin: 4 5 6\n"
        "      intensity: 2.0\n"
        "      color: 3366cc\n"
        "      enabled: false\n";
    if (!WriteTextAndLoadDoc(tempDir / "legacy.lights.yaml", legacyYaml, doc))
    {
        return false;
    }

    const EditorLevelDocument::ReadResult legacyRead = doc.ReadLevel("q3dm1");
    if (!Check(legacyRead.hasLights && legacyRead.lights.size() == 2,
               "legacy custom-only lights count mismatch") ||
        !Check(legacyRead.lights[0].className.empty() && legacyRead.lights[1].className.empty(),
               "legacy custom-only lights block should load with empty classNames"))
    {
        return false;
    }

    return Check(!legacyRead.lights[0].forceRasterize && !legacyRead.lights[1].enabled,
                 "legacy light flags mismatch") &&
           Check(!HasSavedStandardLightsBlock(legacyRead.lights),
                 "legacy all-empty className entries must keep HasSavedStandardLightsBlock false") &&
           Check(!IsSavedStandardLightEntry(legacyRead.lights[0]) &&
                     !IsSavedStandardLightEntry(legacyRead.lights[1]),
                 "legacy entries must not classify as standard lights");
}

bool VerifyEmptyLightsAndLegacyRoundTrip(const std::filesystem::path &tempDir)
{
    const std::filesystem::path emptyPath = tempDir / "empty.lights.yaml";
    EditorLevelDocument doc;
    const std::string emptyYaml = doc.WriteMerged("q3dm1", EditorLevelSky{}, EditorLevelFog{}, {}, true);
    if (!Check(emptyYaml.find("lights: []") != std::string::npos,
               "all-deleted level should serialize lights: []") ||
        !WriteTextAndLoadDoc(emptyPath, emptyYaml, doc))
    {
        return false;
    }

    const EditorLevelDocument::ReadResult emptyRead = doc.ReadLevel("q3dm1");
    if (!Check(emptyRead.hasLights && emptyRead.lights.empty(),
               "lights: [] must reload with hasLights == true and 0 lights") ||
        !Check(HasSavedStandardLightsBlock(emptyRead.lights),
               "lights: [] must have HasSavedStandardLightsBlock == true"))
    {
        return false;
    }

    const std::string omitted =
        doc.WriteMerged("q3dm2", EditorLevelSky{}, EditorLevelFog{}, {}, false);
    if (!WriteTextAndLoadDoc(emptyPath, omitted, doc))
    {
        return false;
    }
    const EditorLevelDocument::ReadResult omittedRead = doc.ReadLevel("q3dm2");
    return Check(!omittedRead.hasLights, "omitted empty lights should reload with hasLights == false") &&
           VerifyLegacyLightsBlock(tempDir, doc);
}

bool VerifyCustomOnlyAfterAllStandardDeletedRoundTrip(const std::filesystem::path &tempDir)
{
    const std::filesystem::path customOnlyPath = tempDir / "custom_only.lights.yaml";
    EditorLevelDocument doc;
    const EditorLevelLight customOnly = MakeSampleLevelLight("custom", 320.0f, true, false, false);
    const std::string customOnlyYaml =
        doc.WriteMerged("q3dm3", EditorLevelSky{}, EditorLevelFog{}, { customOnly }, true);
    if (!Check(customOnlyYaml.find("class: custom") != std::string::npos &&
                   customOnlyYaml.find("lights: []") == std::string::npos,
               "0 standard + 1 custom should serialize non-empty lights with class: custom") ||
        !WriteTextAndLoadDoc(customOnlyPath, customOnlyYaml, doc))
    {
        return false;
    }

    const EditorLevelDocument::ReadResult customOnlyRead = doc.ReadLevel("q3dm3");
    if (!Check(customOnlyRead.hasLights && customOnlyRead.lights.size() == 1 &&
                   customOnlyRead.lights[0].className == "custom",
               "0 standard + 1 class: custom entry failed to round-trip") ||
        !Check(HasSavedStandardLightsBlock(customOnlyRead.lights) &&
                   !IsSavedStandardLightEntry(customOnlyRead.lights[0]),
               "0 standard + 1 class: custom entry must keep m_scene.lights empty while loading as custom"))
    {
        return false;
    }

    const EditorLevelLight legacyEntry = MakeSampleLevelLight("", 10.0f, true, false, false);
    const std::vector<EditorLevelLight> mixedLegacyAndCustom = { legacyEntry, customOnly };
    return Check(HasSavedStandardLightsBlock(mixedLegacyAndCustom) &&
                     !IsSavedStandardLightEntry(mixedLegacyAndCustom[0]) &&
                     !IsSavedStandardLightEntry(mixedLegacyAndCustom[1]),
                 "mixed empty + class: custom block classification mismatch");
}

bool VerifyCustomClassCaseAndLegacyUpgrade(const std::filesystem::path &tempDir)
{
    const EditorLevelLight customOnly = MakeSampleLevelLight("custom", 320.0f, true, false, false);
    const std::string sectionText =
        RenderEditorLevelSection("q3dm3", EditorLevelSky{}, EditorLevelFog{}, { customOnly }, true);
    if (!Check(sectionText.find("class: custom") != std::string::npos &&
                   sectionText.find("lights: []") == std::string::npos,
               "RenderEditorLevelSection with 0 standard + 1 custom must emit class: custom"))
    {
        return false;
    }

    EditorLevelDocument doc;
    const std::string mixedCaseYaml =
        "q3dm4:\n"
        "  lights:\n"
        "    - origin: 10 20 64\n"
        "      class: Custom\n"
        "      custom_note: keep_legacy\n"
        "    - origin: 30 40 64\n"
        "      class: CUSTOM\n";
    if (!WriteTextAndLoadDoc(tempDir / "mixed_case_custom.lights.yaml", mixedCaseYaml, doc))
    {
        return false;
    }
    const EditorLevelDocument::ReadResult caseRead = doc.ReadLevel("q3dm4");
    if (!Check(caseRead.hasLights && caseRead.lights.size() == 2,
               "mixed-case custom YAML read count mismatch") ||
        !Check(HasSavedStandardLightsBlock(caseRead.lights) &&
                   !IsSavedStandardLightEntry(caseRead.lights[0]) &&
                   !IsSavedStandardLightEntry(caseRead.lights[1]),
               "mixed-case class: Custom / CUSTOM must classify as custom"))
    {
        return false;
    }

    const std::string legacyWithNote =
        "q3dm5:\n"
        "  lights:\n"
        "    - origin: 10 20 64\n"
        "      custom_note: keep_legacy\n";
    if (!WriteTextAndLoadDoc(tempDir / "legacy_upgrade.lights.yaml", legacyWithNote, doc))
    {
        return false;
    }
    const EditorLevelLight upgraded = MakeSampleLevelLight("custom", 10.0f, true, false, false);
    const std::string merged =
        doc.WriteMerged("q3dm5", EditorLevelSky{}, EditorLevelFog{}, { upgraded }, true);
    return Check(merged.find("class: custom") != std::string::npos &&
                     merged.find("custom_note: keep_legacy") != std::string::npos,
                 "legacy custom light upgrade to class: custom must preserve extra keys");
}

bool VerifyBaselineMapLightAtAlignment()
{
    EditorLight bsp0;
    bsp0.className = "light";
    EditorLight bsp1;
    bsp1.className = "light_torch";
    EditorLight gen0;
    gen0.className = "flame";
    gen0.emitterUniqueID = 77u;
    std::vector<EditorLight> baseline = { bsp0, bsp1, gen0 };
    std::vector<EditorLight> current = baseline;

    if (!Check(CountLeadingBspLights(current) == 2 &&
                   BaselineMapLightAt(0, current, baseline) == &baseline[0] &&
                   BaselineMapLightAt(1, current, baseline) == &baseline[1] &&
                   BaselineMapLightAt(2, current, baseline) == nullptr &&
                   BaselineMapLightAt(-1, current, baseline) == nullptr,
               "aligned BaselineMapLightAt failed"))
    {
        return false;
    }

    current.erase(current.begin());
    return Check(CountLeadingBspLights(current) == 1 &&
                     BaselineMapLightAt(0, current, baseline) == nullptr,
                 "shifted BaselineMapLightAt after deletion must return nullptr");
}

bool TestLevelDocumentStandardAndCustomLightsRoundTrip()
{
    std::error_code ec;
    const std::filesystem::path tempDir =
        std::filesystem::temp_directory_path() / "qray_lights_roundtrip_test";
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);
    if (!Check(!ec, "failed to create temp test directory"))
    {
        return false;
    }

    const bool ok = VerifyStandardAndCustomRoundTrip(tempDir) &&
                    VerifyEmptyLightsAndLegacyRoundTrip(tempDir) &&
                    VerifyCustomOnlyAfterAllStandardDeletedRoundTrip(tempDir) &&
                    VerifyCustomClassCaseAndLegacyUpgrade(tempDir) &&
                    VerifyBaselineMapLightAtAlignment();
    std::filesystem::remove_all(tempDir, ec);
    return ok;
}

bool TestBaselineCustomAndMapLightGuards()
{
    std::vector<EditorLight> customBaseline = {
        MakeTestLight({ 10.0f, 0.0f, 0.0f, 100.0f }),
        MakeTestLight({ 20.0f, 0.0f, 0.0f, 200.0f }),
    };
    if (BaselineCustomLightAt(1, customBaseline, customBaseline) != &customBaseline[1] ||
        BaselineCustomLightAt(-1, customBaseline, customBaseline) != nullptr ||
        BaselineCustomLightAt(2, customBaseline, customBaseline) != nullptr)
    {
        return Check(false, "BaselineCustomLightAt should resolve matching-size valid indices");
    }

    std::vector<EditorLight> customAfterDelete = { customBaseline[1] };
    return Check(BaselineCustomLightAt(0, customAfterDelete, customBaseline) == nullptr,
                 "BaselineCustomLightAt must return nullptr when custom light count shifts");
}

bool TestColocatedExtraClaimPrefersClassName()
{
    const std::filesystem::path tmpPath =
        std::filesystem::temp_directory_path() / "qray_colocated_extra_test.lights.yaml";
    std::error_code ec;
    std::filesystem::remove(tmpPath, ec);

    const std::string initialYaml =
        "q3dm17:\n"
        "  lights:\n"
        "    - origin: 10 20 30\n"
        "      class: light\n"
        "      custom_tag: keep_entity\n"
        "    - origin: 10 20 30\n"
        "      class: fixtures/torch\n"
        "      custom_tag: keep_torch\n";
    std::string ioErr;
    if (!WriteEditorFile(tmpPath.string(), initialYaml, ioErr))
    {
        return Check(false, "WriteEditorFile failed for colocated extra test");
    }

    EditorLevelDocument doc;
    const bool loaded = doc.Load(tmpPath.string(), ioErr);
    std::filesystem::remove(tmpPath, ec);
    if (!Check(loaded, "EditorLevelDocument::Load failed for colocated extra test"))
    {
        return false;
    }

    EditorLevelSky sky;
    EditorLevelFog fog;
    EditorLevelLight remainingTorch;
    remainingTorch.origin[0] = 10.0f;
    remainingTorch.origin[1] = 20.0f;
    remainingTorch.origin[2] = 30.0f;
    remainingTorch.className = "fixtures/torch";
    const std::string merged = doc.WriteMerged("q3dm17", sky, fog, { remainingTorch });
    return Check(merged.find("custom_tag: keep_torch") != std::string::npos &&
                     merged.find("custom_tag: keep_entity") == std::string::npos,
                 "Colocated light removal must preserve extra keys by matching className");
}

std::vector<uint8_t> MakeSplitBlackWhiteRgba(uint32_t width, uint32_t height, uint32_t splitX)
{
    std::vector<uint8_t> rgba(size_t(width) * size_t(height) * 4, 255);
    for (uint32_t y = 0; y < height; y++)
    {
        for (uint32_t x = 0; x < width; x++)
        {
            const uint8_t v = x >= splitX ? 255 : 0;
            const size_t off = (size_t(y) * width + x) * 4;
            rgba[off + 0] = v;
            rgba[off + 1] = v;
            rgba[off + 2] = v;
        }
    }
    return rgba;
}

bool VerifySmoothFeatherCurveAndThinStrip(const std::vector<uint8_t> &rgba)
{
    const float white[3] = { 1.0f, 1.0f, 1.0f };
    std::vector<float> m4;
    std::vector<float> m8;
    ComputeEditorMask({ white, 0.25f, 4.0f, 1.0f, nullptr, nullptr, 0, 0, true }, rgba.data(), 64,
                      64, m4, nullptr);
    ComputeEditorMask({ white, 0.25f, 8.0f, 1.0f, nullptr, nullptr, 0, 0, true }, rgba.data(), 64,
                      64, m8, nullptr);

    const size_t row = 32u * 64u;
    if (!Check(m4.size() == 4096 && m4[row + 60] > 0.99f && m4[row + 4] < 0.01f,
               "gloss feathered endpoints mismatch") ||
        !Check(m4[row + 31] < 0.85f && m4[row + 32] < 0.99f,
               "gloss feather still leaves sharp cliff at edge"))
    {
        return false;
    }

    for (uint32_t x = 28; x < 35; x++)
    {
        if (!Check(m4[row + x + 1] > m4[row + x],
                   "gloss feather transition must be strictly monotonic across edge"))
        {
            return false;
        }
    }

    const float slope4 = m4[row + 33] - m4[row + 29];
    const float slope8 = m8[row + 33] - m8[row + 29];
    return Check(m4[row + 28] > 0.01f && m8[row + 24] > m4[row + 24] && m8[row + 24] > 0.02f &&
                     slope8 < slope4,
                 "feather=8 did not soften slope and widen tail relative to feather=4");
}

bool VerifyFeatherRange1To16(const std::vector<uint8_t> &rgba)
{
    const float white[3] = { 1.0f, 1.0f, 1.0f };
    float prevTail = 0.0f;
    for (int f = 1; f <= 16; f++)
    {
        std::vector<float> m;
        ComputeEditorMask({ white, 0.25f, float(f), 1.0f, nullptr, nullptr, 0, 0, true },
                          rgba.data(), 64, 64, m, nullptr);
        const size_t row = 32u * 64u;
        const bool mono = m[row + 33] > m[row + 32] && m[row + 32] > m[row + 31] &&
                          m[row + 31] > m[row + 30];
        const bool notFlat = m[row + 63] > 0.85f && m[row + 0] < 0.15f;
        const bool tailGrew = m[row + 24] >= prevTail;
        if (!Check(mono && notFlat && tailGrew, "gloss feather failed monotonic 1..16 check"))
        {
            return false;
        }
        prevTail = m[row + 24];
    }
    return Check(prevTail > 0.05f, "gloss feather=16 tail too narrow");
}

bool TestSmoothGlossFeathering()
{
    const std::vector<uint8_t> splitRgba = MakeSplitBlackWhiteRgba(64, 64, 32);
    if (!VerifySmoothFeatherCurveAndThinStrip(splitRgba) || !VerifyFeatherRange1To16(splitRgba))
    {
        return false;
    }

    std::vector<uint8_t> stripRgba(64u * 64u * 4u, 0);
    for (uint32_t y = 0; y < 64; y++)
    {
        const size_t off = (size_t(y) * 64u + 32u) * 4u;
        stripRgba[off + 0] = 255;
        stripRgba[off + 1] = 255;
        stripRgba[off + 2] = 255;
        stripRgba[off + 3] = 255;
    }
    const float white[3] = { 1.0f, 1.0f, 1.0f };
    std::vector<float> stripMask;
    ComputeEditorMask({ white, 0.25f, 4.0f, 1.0f, nullptr, nullptr, 0, 0, true },
                      stripRgba.data(), 64, 64, stripMask, nullptr);
    return Check(stripMask[32u * 64u + 32u] >= 0.25f && stripMask[32u * 64u + 30u] > 0.01f,
                 "thin 1px highlight collapsed under gloss feathering");
}

bool VerifySoftAndHardBrushDabs()
{
    const std::vector<uint8_t> empty(64u * 64u, 0);
    std::vector<float> accum(64u * 64u, 0.0f);
    std::vector<uint8_t> softOut = empty;
    const bool softChanged =
        ApplyEditorBrushSegment({ 0.5f, 0.5f, 0.5f, 0.5f, 8.0f, 0.0f, 1.0f, false }, 64, 64, empty,
                                accum, softOut);
    if (!Check(softChanged && softOut[32u * 64u + 32u] > softOut[32u * 64u + 36u] &&
                   softOut[32u * 64u + 36u] > 0 && softOut[32u * 64u + 40u] == 0,
               "soft brush falloff or radius mismatch"))
    {
        return false;
    }

    accum.assign(64u * 64u, 0.0f);
    std::vector<uint8_t> hardOut = empty;
    ApplyEditorBrushSegment({ 0.5f, 0.5f, 0.5f, 0.5f, 6.0f, 1.0f, 1.0f, false }, 64, 64, empty,
                            accum, hardOut);
    return Check(hardOut[32u * 64u + 32u] == 255 && hardOut[32u * 64u + 35u] == 255 &&
                     hardOut[32u * 64u + 40u] == 0,
                 "hard brush interior or outside radius mismatch");
}

bool VerifyBrushInvalidAndAdditiveStrokes()
{
    const std::vector<uint8_t> base(32u * 32u, 0);
    std::vector<float> accum(32u * 32u, 0.0f);
    std::vector<uint8_t> mask = base;
    const float qnan = std::numeric_limits<float>::quiet_NaN();
    if (!Check(!ApplyEditorBrushSegment({ 0.5f, 0.5f, 0.5f, 0.5f, qnan, 0.5f, 1.0f, false }, 32,
                                        32, base, accum, mask) &&
                   !ApplyEditorBrushSegment({ qnan, 0.5f, 0.5f, 0.5f, 4.0f, 0.5f, 1.0f, false },
                                            32, 32, base, accum, mask),
               "non-finite brush stroke inputs must be rejected"))
    {
        return false;
    }

    accum.assign(32u * 32u, 0.0f);
    ApplyEditorBrushSegment({ 0.5f, 0.5f, 0.5f, 0.5f, 1e20f, 1.0f, 0.5f, false }, 32, 32, base,
                            accum, mask);
    if (!Check(mask[0] == 128 && mask[32u * 32u - 1] == 128,
               "huge brush radius overflowed or failed to fill canvas cleanly"))
    {
        return false;
    }

    const std::vector<uint8_t> halfBase(32u * 32u, 128);
    accum.assign(32u * 32u, 0.0f);
    std::vector<uint8_t> secondStroke = halfBase;
    ApplyEditorBrushSegment({ 0.5f, 0.5f, 0.5f, 0.5f, 4.0f, 1.0f, 0.5f, false }, 32, 32,
                            halfBase, accum, secondStroke);
    return Check(secondStroke[16u * 32u + 16u] == 192,
                 "second stroke at opacity=0.5 failed alpha-over buildup from 128 to 192");
}

bool TestBrushStrokeEvaluation()
{
    if (!VerifySoftAndHardBrushDabs() || !VerifyBrushInvalidAndAdditiveStrokes())
    {
        return false;
    }

    const std::vector<uint8_t> base(64u * 64u, 0);
    std::vector<float> accum(64u * 64u, 0.0f);
    std::vector<uint8_t> mask = base;
    for (int pass = 0; pass < 4; pass++)
    {
        const float u0 = (pass % 2 == 0) ? 0.3f : 0.7f;
        const float u1 = (pass % 2 == 0) ? 0.7f : 0.3f;
        ApplyEditorBrushSegment({ u0, 0.5f, u1, 0.5f, 6.0f, 1.0f, 0.5f, false }, 64, 64, base,
                                accum, mask);
    }

    const uint8_t maxByte = *std::max_element(mask.begin(), mask.end());
    if (!Check(maxByte == 128 && mask[32u * 64u + 32u] == 128,
               "single-stroke opacity=0.5 exceeded 128/255 cap across repeated passes"))
    {
        return false;
    }

    const std::vector<uint8_t> base2 = mask;
    accum.assign(64u * 64u, 0.0f);
    ApplyEditorBrushSegment({ 0.5f, 0.5f, 0.5f, 0.5f, 6.0f, 1.0f, 1.0f, true }, 64, 64, base2,
                            accum, mask);
    return Check(mask[32u * 64u + 32u] == 0, "erase brush stroke did not clear painted texel");
}

EditorMaterialParams MakeBrushRoundTripMaterial()
{
    EditorMaterialParams mat;
    mat.hasColorEmissive = true;
    EditorEmissiveBlock lumaBrush;
    lumaBrush.brushWidth = 16;
    lumaBrush.brushHeight = 16;
    lumaBrush.brushMask.assign(256, 0);
    for (size_t i = 0; i < 256; i++)
    {
        lumaBrush.brushMask[i] = (i < 40 || i > 200) ? uint8_t(0) : uint8_t((i * 17) & 0xffu);
    }
    lumaBrush.feather = 2.0f;
    lumaBrush.factor = 1.5f;
    lumaBrush.blend = 2;
    mat.emissiveBlocks.push_back(lumaBrush);

    EditorEmissiveBlock lumaColor;
    lumaColor.color[0] = 0.2f;
    lumaColor.color[1] = 0.8f;
    lumaColor.color[2] = 0.4f;
    lumaColor.threshold = 0.15f;
    mat.emissiveBlocks.push_back(lumaColor);

    mat.hasColorGloss = true;
    EditorGlossBlock glossBrush;
    glossBrush.brushWidth = 8;
    glossBrush.brushHeight = 8;
    glossBrush.brushMask.assign(64, 42);
    glossBrush.brushMask[0] = 255;
    glossBrush.brushMask[63] = 0;
    glossBrush.feather = 3.0f;
    glossBrush.roughness = 0.12f;
    glossBrush.metalness = 0.85f;
    mat.glossBlocks.push_back(glossBrush);
    return mat;
}

bool VerifyMalformedBrushYamlRejected(const std::filesystem::path &tempDir)
{
    const std::string badYaml =
        "qray_materials:\n"
        "  - name: textures/bad_brush\n"
        "    luma:\n"
        "      - brush: 0x4 4*00\n"
        "      - brush: 4097x4097 16785409*00\n"
        "      - brush: 2x2 00,ff,80\n"
        "      - brush: 2x2 5*00\n"
        "      - brush: 2x2 4*00,\n"
        "      - brush: 2x2 4*zz\n"
        "      - brush: 2x2 {00},ff,00,ff\n"
        "        feather: 1e20\n"
        "    gloss:\n"
        "      - brush: 2x2 0*ff,4*00\n"
        "        feather: nan\n";
    const std::filesystem::path badPath = tempDir / "bad.materials.yaml";
    std::string err;
    if (!Check(WriteEditorFile(badPath.string(), badYaml, err), "write bad brush yaml"))
    {
        return false;
    }
    EditorMaterialsDocument doc;
    EditorMaterialParams readBad;
    if (!Check(doc.Load(badPath.string(), err) && doc.ReadMaterial("bad_brush", readBad),
               "load bad brush yaml"))
    {
        return false;
    }
    for (const EditorEmissiveBlock &blk : readBad.emissiveBlocks)
    {
        if (!Check(!IsBrushBlock(blk) && blk.brushMask.empty() && blk.feather >= 0.0f &&
                       blk.feather <= 16.0f,
                   "malformed luma brush YAML entry must be rejected cleanly"))
        {
            return false;
        }
    }
    for (const EditorGlossBlock &blk : readBad.glossBlocks)
    {
        if (!Check(!IsBrushBlock(blk) && blk.brushMask.empty() && std::isfinite(blk.feather),
                   "malformed gloss brush YAML entry must be rejected cleanly"))
        {
            return false;
        }
    }
    return true;
}

bool VerifyBrushRoundTripMatch(const EditorMaterialParams &expected,
                               const EditorMaterialParams &actual)
{
    if (!Check(actual.emissiveBlocks.size() == 2 && actual.glossBlocks.size() == 1,
               "round-trip block counts mismatch"))
    {
        return false;
    }
    const EditorEmissiveBlock &el = expected.emissiveBlocks[0];
    const EditorEmissiveBlock &al = actual.emissiveBlocks[0];
    const EditorGlossBlock &eg = expected.glossBlocks[0];
    const EditorGlossBlock &ag = actual.glossBlocks[0];
    const bool lumaOk = IsBrushBlock(al) && al.brushWidth == el.brushWidth &&
                        al.brushHeight == el.brushHeight && al.brushMask == el.brushMask;
    const bool glossOk = IsBrushBlock(ag) && ag.brushWidth == eg.brushWidth &&
                         ag.brushHeight == eg.brushHeight && ag.brushMask == eg.brushMask;
    return Check(lumaOk && glossOk, "luma/gloss brush mask YAML round-trip mismatch");
}

bool TestBrushMaskYamlRoundTrip()
{
    std::error_code ec;
    const std::filesystem::path tempDir =
        std::filesystem::temp_directory_path() / "qray_brush_yaml_roundtrip";
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);

    const EditorMaterialParams expected = MakeBrushRoundTripMaterial();
    EditorMaterialsDocument doc;
    const std::string yaml = doc.WriteMerged({ { "brush_mat", expected } });
    const std::filesystem::path yamlPath = tempDir / "qray.materials.yaml";
    std::string err;
    EditorMaterialParams actual;
    const bool loaded = WriteEditorFile(yamlPath.string(), yaml, err) &&
                        doc.Load(yamlPath.string(), err) &&
                        doc.ReadMaterial("brush_mat", actual);

    const bool ok = Check(loaded, "failed to load round-trip brush yaml") &&
                    VerifyBrushRoundTripMatch(expected, actual) &&
                    VerifyMalformedBrushYamlRejected(tempDir);
    std::filesystem::remove_all(tempDir, ec);
    return ok;
}

bool TestGlossMultiBlockBlendAndBakeInvalidation()
{
    EditorScene scene;
    EditorTexture tex;
    tex.name = "test_albedo";
    tex.width = 16;
    tex.height = 16;
    tex.rgba.assign(16u * 16u * 4u, 255);
    scene.textures.push_back(tex);

    EditorMaterialParams params;
    params.hasColorGloss = true;
    EditorGlossBlock b0;
    b0.brushWidth = 16;
    b0.brushHeight = 16;
    b0.brushMask.assign(256, 255);
    b0.roughness = 0.0f;
    EditorGlossBlock b1 = b0;
    b1.roughness = 1.0f;
    params.glossBlocks = { b0, b1 };

    EditorMaterialBakes bakes;
    const std::vector<QrMaterial> pool = { 10 };
    const auto resolve = [](const std::string &n) -> int32_t {
        return n == "test_albedo" ? 0 : -1;
    };
    const QrInstance dummyInstance = reinterpret_cast<QrInstance>(uintptr_t(1));
    g_testBakeCreateCount = 0;
    bakes.BeginPass();
    const QrMaterial m0 = bakes.Get(dummyInstance, "test_albedo", params, scene, pool, resolve);
    const uint8_t blendedRough = g_lastBakedRme.empty() ? 0 : g_lastBakedRme[0];
    bakes.EndPass(dummyInstance);

    params.glossBlocks[0].brushMask[0] = 128;
    bakes.BeginPass();
    const QrMaterial m1 = bakes.Get(dummyInstance, "test_albedo", params, scene, pool, resolve);
    bakes.EndPass(dummyInstance);
    bakes.Destroy(dummyInstance);

    return Check(m0 != QR_NO_MATERIAL && m1 != QR_NO_MATERIAL && m0 != m1 &&
                     g_testBakeCreateCount == 2,
                 "brush mask edit did not invalidate EditorMaterialBakes cache") &&
           Check(blendedRough >= 120 && blendedRough <= 135,
                 "overlapping gloss blocks failed to blend roughness smoothly");
}

bool TestComputeMaskEdgeCases()
{
    const float white[3] = { 1.0f, 1.0f, 1.0f };
    std::vector<float> zeroOut;
    ComputeEditorMask({ white, 0.25f, 4.0f, 1.0f, nullptr, nullptr, 0, 0, true }, nullptr, 0, 0,
                      zeroOut, nullptr);
    if (!Check(zeroOut.empty(), "zero-size ComputeEditorMask must clear outMask"))
    {
        return false;
    }

    const uint8_t gradientRgba[12] = { 255, 255, 255, 255, 200, 200, 200, 255, 0, 0, 0, 255 };
    std::vector<float> gradMask;
    ComputeEditorMask({ white, 0.25f, -20.0f, 1.0f, nullptr, nullptr, 0, 0, true }, gradientRgba,
                      3, 1, gradMask, nullptr);
    if (!Check(gradMask.size() == 3 && Near(gradMask[0], 1.0f) && gradMask[1] > 0.05f &&
                   gradMask[1] < 0.95f && Near(gradMask[2], 0.0f),
               "isGloss negative/zero feather color-distance falloff failed to interpolate"))
    {
        return false;
    }

    const std::vector<uint8_t> brush2x2 = { 0, 255, 0, 255 };
    std::vector<float> upsampled;
    ComputeEditorMask({ nullptr, 0.0f, 0.0f, 1.0f, nullptr, &brush2x2, 2, 2, false }, nullptr, 4,
                      4, upsampled, nullptr);
    return Check(upsampled.size() == 16 && Near(upsampled[0], 0.0f) && Near(upsampled[3], 1.0f) &&
                     upsampled[1] > 0.1f && upsampled[1] < 0.9f,
                 "bilinear brush resampling with nullptr rgba mismatch");
}

int CountPurpleOverlayVertices(const ImDrawData *drawData, uint8_t *outMaxAlpha = nullptr)
{
    int count = 0;
    uint8_t maxA = 0;
    const uint32_t targetRgb = IM_COL32(216, 24, 255, 0) & 0x00ffffffu;
    for (int i = 0; drawData != nullptr && i < drawData->CmdListsCount; ++i)
    {
        const ImDrawList *dl = drawData->CmdLists[i];
        for (int v = 0; v < dl->VtxBuffer.Size; ++v)
        {
            const uint32_t col = dl->VtxBuffer[v].col;
            const uint8_t a = uint8_t(col >> 24);
            if ((col & 0x00ffffffu) == targetRgb && a > 0 && a <= 128)
            {
                count++;
                maxA = std::max(maxA, a);
            }
        }
    }
    if (outMaxAlpha != nullptr)
    {
        *outMaxAlpha = maxA;
    }
    return count;
}

template <typename DrawFn>
void RenderPanelUiFrame(float &outPanelW, ImVec2 &outMin, ImVec2 &outMax, DrawFn &&fn)
{
    ImGui::NewFrame();
    EditorGuiBeginPanel("MATERIAL EDITOR", 1280.0f, 720.0f);
    outPanelW = ImGui::GetWindowWidth();
    fn();
    outMin = ImGui::GetItemRectMin();
    outMax = ImGui::GetItemRectMax();
    EditorGuiEndPanel();
    ImGui::Render();
}

bool VerifyImagePickAndOverlayCap(ImGuiIO &io)
{
    std::vector<float> overlay64(64u * 64u, 0.0f);
    std::fill(overlay64.begin() + 32u * 64u, overlay64.end(), 1.0f);
    float panelW = 0.0f;
    ImVec2 rMin{};
    ImVec2 rMax{};
    float pickU = 0.0f;
    float pickV = 0.0f;
    RenderPanelUiFrame(panelW, rMin, rMax, [&]() {
        EditorImagePick("##pick", 1, 64, 64, pickU, pickV, &overlay64);
    });

    uint8_t maxAlpha = 0;
    const int vtx64 = CountPurpleOverlayVertices(ImGui::GetDrawData(), &maxAlpha);
    if (!Check(Near(panelW, 536.0f, 1.0f) && Near(kEditorGuiPanelWidth, 536.0f) &&
                   Near(kEditorGuiLabelWidth, 176.0f) && maxAlpha == 128 && vtx64 == 32 * 4,
               "widened panel or 1:1 run-merged purple overlay mismatch"))
    {
        return false;
    }

    io.AddMousePosEvent((rMin.x + rMax.x) * 0.5f, (rMin.y + rMax.y) * 0.5f);
    RenderPanelUiFrame(panelW, rMin, rMax, [&]() {});
    io.AddMouseButtonEvent(0, true);
    bool picked = false;
    RenderPanelUiFrame(panelW, rMin, rMax, [&]() {
        picked = EditorImagePick("##pick", 1, 64, 64, pickU, pickV, &overlay64);
    });
    io.AddMouseButtonEvent(0, false);
    if (!Check(picked && Near(pickU, 0.5f, 0.05f) && Near(pickV, 0.5f, 0.05f),
               "EditorImagePick click coordinates mismatch"))
    {
        return false;
    }

    std::vector<float> checker256(256u * 256u, 0.0f);
    for (size_t i = 0; i < checker256.size(); i++)
    {
        checker256[i] = ((i + i / 256) % 2 == 0) ? 1.0f : 0.25f;
    }
    RenderPanelUiFrame(panelW, rMin, rMax, [&]() {
        EditorImagePick("##pick256", 1, 256, 256, pickU, pickV, &checker256);
    });
    const int vtx256 = CountPurpleOverlayVertices(ImGui::GetDrawData());
    return Check(vtx256 > 0 && vtx256 <= 96 * 96 * 4,
                 "large texture purple overlay exceeded 96x96 grid vertex cap");
}

bool VerifyPolygonOverlaySuppression()
{
    std::vector<float> poly = { 0.2f, 0.2f, 0.8f, 0.2f, 0.8f, 0.8f, 0.2f, 0.8f };
    float panelW = 0.0f;
    ImVec2 rMin{};
    ImVec2 rMax{};
    RenderPanelUiFrame(panelW, rMin, rMax, [&]() {
        EditorPolygonEdit("##poly_nofill", 1, 64, 64, poly, 16, nullptr);
    });
    const uint32_t whitePolyFill = IM_COL32(255, 255, 255, 128);
    if (!Check(ContainsColoredTextVertex(ImGui::GetDrawData(), whitePolyFill),
               "EditorPolygonEdit without overlayMask must draw white concave fill"))
    {
        return false;
    }

    const std::vector<float> overlay64(64u * 64u, 1.0f);
    RenderPanelUiFrame(panelW, rMin, rMax, [&]() {
        EditorPolygonEdit("##poly_with_overlay", 1, 64, 64, poly, 16, &overlay64);
    });
    return Check(!ContainsColoredTextVertex(ImGui::GetDrawData(), whitePolyFill) &&
                     CountPurpleOverlayVertices(ImGui::GetDrawData()) > 0 &&
                     ContainsColoredTextVertex(ImGui::GetDrawData(), IM_COL32(255, 255, 255, 220)),
                 "EditorPolygonEdit with overlayMask must suppress white fill and keep outline");
}

bool VerifyBrushUiDragAndErase(ImGuiIO &io, std::vector<uint8_t> &mask, ImVec2 &outMin,
                               ImVec2 &outMax)
{
    EditorBrushToolSettings tool{ 6.0f, 0.5f, 0.5f, false };
    float panelW = 0.0f;
    const auto renderBrush = [&](const EditorBrushToolSettings &t) {
        bool edited = false;
        RenderPanelUiFrame(panelW, outMin, outMax, [&]() {
            edited = EditorBrushEdit("##ui_brush", 1, 64, 64, mask, t, nullptr);
        });
        return edited;
    };

    renderBrush(tool);
    const float midY = (outMin.y + outMax.y) * 0.5f;
    const float leftX = outMin.x + (outMax.x - outMin.x) * 0.25f;
    const float rightX = outMin.x + (outMax.x - outMin.x) * 0.75f;
    io.AddMousePosEvent((outMin.x + outMax.x) * 0.5f, midY);
    renderBrush(tool);
    const uint32_t outerRingCol = IM_COL32(236, 140, 255, 240);
    const uint32_t innerRingCol = IM_COL32(255, 255, 255, 175);
    if (!Check(ContainsColoredTextVertex(ImGui::GetDrawData(), outerRingCol) &&
                   ContainsColoredTextVertex(ImGui::GetDrawData(), innerRingCol),
               "hovered EditorBrushEdit missing cursor rings"))
    {
        return false;
    }

    io.AddMousePosEvent(leftX, midY);
    renderBrush(tool);
    io.AddMouseButtonEvent(0, true);
    const bool downChanged = renderBrush(tool);
    io.AddMousePosEvent(rightX, midY);
    const bool dragChanged = renderBrush(tool);
    io.AddMousePosEvent(outMax.x + 8.0f, midY);
    renderBrush(tool);
    const bool activeRingOk = ContainsColoredTextVertex(ImGui::GetDrawData(), outerRingCol);
    io.AddMouseButtonEvent(0, false);
    renderBrush(tool);

    const uint8_t maxVal = *std::max_element(mask.begin(), mask.end());
    if (!Check(downChanged && dragChanged && activeRingOk && maxVal == 128 &&
                   mask[32u * 64u + 32u] == 128,
               "EditorBrushEdit LMB paint stroke or active edge cursor ring failed"))
    {
        return false;
    }

    tool.hardness = 1.0f;
    tool.opacity = 1.0f;
    io.AddMousePosEvent((outMin.x + outMax.x) * 0.5f, midY);
    renderBrush(tool);
    if (!Check(!ContainsColoredTextVertex(ImGui::GetDrawData(), innerRingCol),
               "hardness=1.0 brush should not draw inner hardness ring"))
    {
        return false;
    }

    io.AddMouseButtonEvent(1, true);
    renderBrush(tool);
    io.AddMousePosEvent(rightX, midY);
    renderBrush(tool);
    io.AddMouseButtonEvent(1, false);
    renderBrush(tool);
    return Check(mask[32u * 64u + 32u] == 0, "EditorBrushEdit RMB drag did not erase texel");
}

bool VerifyBrushUiCtrlWheelZoom(ImGuiIO &io, std::vector<uint8_t> &mask, const ImVec2 &rMin,
                                const ImVec2 &rMax)
{
    std::fill(mask.begin(), mask.end(), uint8_t(0));
    const EditorBrushToolSettings tool{ 2.0f, 1.0f, 1.0f, false };
    float panelW = 0.0f;
    ImVec2 bMin{};
    ImVec2 bMax{};
    const auto render = [&]() {
        RenderPanelUiFrame(panelW, bMin, bMax, [&]() {
            EditorBrushEdit("##ui_brush", 1, 64, 64, mask, tool, nullptr);
        });
    };

    const float cx = rMin.x + (rMax.x - rMin.x) * 0.8f;
    const float cy = rMin.y + (rMax.y - rMin.y) * 0.5f;
    io.AddMousePosEvent(cx, cy);
    render();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddMouseWheelEvent(0.0f, 3.0f);
    render();
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    render();

    io.AddMousePosEvent(rMin.x + (rMax.x - rMin.x) * 0.2f, cy);
    render();
    io.AddMouseButtonEvent(0, true);
    render();
    io.AddMouseButtonEvent(0, false);
    render();

    const uint8_t unzoomedCol = mask[32u * 64u + 13u];
    const uint8_t zoomedColMax =
        *std::max_element(mask.begin() + 32u * 64u + 20u, mask.begin() + 32u * 64u + 64u);
    return Check(unzoomedCol == 0 && zoomedColMax == 255,
                 "EditorBrushEdit Ctrl+wheel zoom did not remap pointer UV");
}

bool TestEditorUiOverlayAndBrushCanvas()
{
    IMGUI_CHECKVERSION();
    ImGuiTestContextGuard ctxGuard(ImGui::CreateContext());
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *pixels = nullptr;
    int texW = 0;
    int texH = 0;
    io.Fonts->AddFontDefault();
    io.Fonts->GetTexDataAsRGBA32(&pixels, &texW, &texH);
    io.Fonts->SetTexID(ImTextureID(uintptr_t(1)));
    EditorGuiApplyStyle(1280.0f, 720.0f);

    std::vector<uint8_t> mask(64u * 64u, 0);
    ImVec2 rMin{};
    ImVec2 rMax{};
    return VerifyImagePickAndOverlayCap(io) && VerifyPolygonOverlaySuppression() &&
           VerifyBrushUiDragAndErase(io, mask, rMin, rMax) &&
           VerifyBrushUiCtrlWheelZoom(io, mask, rMin, rMax);
}

bool TestEmissiveBrushBake()
{
    EditorScene scene;
    EditorTexture tex;
    tex.name = "emiss_brush_tex";
    tex.width = 8;
    tex.height = 8;
    tex.rgba.assign(8u * 8u * 4u, 255);
    scene.textures.push_back(tex);
    EditorMaterialParams params;
    params.hasColorEmissive = true;
    EditorEmissiveBlock eb;
    eb.brushWidth = 8;
    eb.brushHeight = 8;
    eb.brushMask.assign(64, 0);
    eb.brushMask[3u * 8u + 4u] = 255;
    params.emissiveBlocks.push_back(eb);

    EditorMaterialBakes bakes;
    const QrInstance dummyInstance = reinterpret_cast<QrInstance>(uintptr_t(1));
    const auto resolve = [](const std::string &n) -> int32_t {
        return n == "emiss_brush_tex" ? 0 : -1;
    };
    bakes.BeginPass();
    const QrMaterial mat =
        bakes.Get(dummyInstance, "emiss_brush_tex", params, scene, { 1 }, resolve);
    const EditorMaterialEmission emiss = bakes.EmissionOf(mat);
    const uint8_t bakedEmis =
        g_lastBakedRme.size() >= 64u * 4u ? g_lastBakedRme[(3u * 8u + 4u) * 4u + 2] : 0;
    bakes.EndPass(dummyInstance);
    bakes.Destroy(dummyInstance);
    return Check(mat != QR_NO_MATERIAL && bakedEmis == 255 && emiss.valid && emiss.hasGlow,
                 "emissive brush bake did not emit expected RME channel and glow bounds");
}

std::vector<uint8_t> BuildSyntheticAiTestDiffuse(uint32_t size)
{
    std::vector<uint8_t> diffuse(size_t(size) * size_t(size) * 4u, 44u);
    for (uint32_t y = 0; y < size; y++)
    {
        for (uint32_t x = 0; x < size; x++)
        {
            const size_t idx = (size_t(y) * size + x) * 4u;
            diffuse[idx + 3u] = 255u;
            const bool onPlateau = x >= 8u && x < 24u && y >= 8u && y < 24u;
            const bool onCore = x >= 14u && x < 18u && y >= 14u && y < 18u;
            if (onCore)
            {
                diffuse[idx + 0u] = 255u;
                diffuse[idx + 1u] = 220u;
                diffuse[idx + 2u] = 70u;
            }
            else if (onPlateau)
            {
                diffuse[idx + 0u] = 200u;
                diffuse[idx + 1u] = 200u;
                diffuse[idx + 2u] = 200u;
            }
        }
    }
    return diffuse;
}

bool VerifyAiNormalsAndGloss(const std::vector<uint8_t> &diffuse, uint32_t size,
                             std::vector<uint8_t> &outNormals)
{
    if (!Check(GenerateEditorAiTextureFromDiffuse(diffuse.data(), size, size, nullptr,
                                                  EditorAiMapKind::Normals, outNormals) &&
                   outNormals.size() == diffuse.size(),
               "AI normal generation failed"))
    {
        return false;
    }

    const bool leftOk = outNormals[(16u * size + 8u) * 4u + 0u] < 120u;
    const bool topOk = outNormals[(8u * size + 16u) * 4u + 1u] > 136u;
    const bool flatZOk = outNormals[0u * 4u + 2u] > 240u;
    if (!Check(leftOk && topOk && flatZOk, "AI normal tangent signs or flat Z mismatch"))
    {
        return false;
    }

    std::vector<uint8_t> gloss;
    if (!Check(GenerateEditorAiTextureFromDiffuse(diffuse.data(), size, size, nullptr,
                                                  EditorAiMapKind::Gloss, gloss) &&
                   gloss.size() == diffuse.size(),
               "AI gloss generation failed"))
    {
        return false;
    }
    return Check(gloss[(12u * size + 12u) * 4u + 0u] > gloss[(2u * size + 2u) * 4u + 0u],
                 "AI gloss should rank bright raised surface higher than dark crevice");
}

bool VerifyAiEmissiveAndTgaRoundTrip(const std::vector<uint8_t> &diffuse, uint32_t size,
                                     const std::vector<uint8_t> &normals)
{
    std::vector<uint8_t> emissive;
    if (!Check(GenerateEditorAiTextureFromDiffuse(diffuse.data(), size, size, nullptr,
                                                  EditorAiMapKind::Emissive, emissive) &&
                   emissive.size() == diffuse.size(),
               "AI emissive generation failed"))
    {
        return false;
    }
    const bool coreBright = emissive[(16u * size + 16u) * 4u + 0u] > 180u;
    const bool bgDark = emissive[(2u * size + 2u) * 4u + 0u] == 0u;
    if (!Check(coreBright && bgDark,
               "AI emissive should isolate bright glowing core from dark background"))
    {
        return false;
    }

    const std::vector<uint8_t> tga = EncodeEditorTgaRgba(normals.data(), size, size);
    uint32_t outW = 0;
    uint32_t outH = 0;
    std::vector<uint8_t> decoded;
    return Check(DecodeEditorImageRgba(tga, outW, outH, decoded) && outW == size &&
                     outH == size && decoded == normals,
                 "EncodeEditorTgaRgba round-trip mismatch");
}

bool VerifyAiGeneratedMapsAndTgaRoundTrip()
{
    constexpr uint32_t kSize = 32u;
    const std::vector<uint8_t> diffuse = BuildSyntheticAiTestDiffuse(kSize);
    std::vector<uint8_t> normals;
    return VerifyAiNormalsAndGloss(diffuse, kSize, normals) &&
           VerifyAiEmissiveAndTgaRoundTrip(diffuse, kSize, normals);
}

bool TestAiTextureGenerationAndButton()
{
    if (!VerifyAiGeneratedMapsAndTgaRoundTrip())
    {
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGuiTestContextGuard ctxGuard(ImGui::CreateContext());
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *pixels = nullptr;
    int texW = 0;
    int texH = 0;
    io.Fonts->AddFontDefault();
    io.Fonts->GetTexDataAsRGBA32(&pixels, &texW, &texH);
    io.Fonts->SetTexID(ImTextureID(uintptr_t(1)));
    EditorGuiApplyStyle(1280.0f, 720.0f);

    float panelW = 0.0f;
    ImVec2 rMin{};
    ImVec2 rMax{};
    std::string path = "textures/test_norm.tga";
    bool browseClicked = false;
    bool aiClicked = false;
    RenderPanelUiFrame(panelW, rMin, rMax, [&]() {
        EditorGuiTexturePath("texture_normals", path, &browseClicked, {}, &aiClicked);
    });
    return Check(!browseClicked && !aiClicked && rMax.x > rMin.x,
                 "EditorGuiTexturePath with Generate with AI button failed to lay out");
}

void AppendTestQuadSurface(EditorScene &scene, const char *name, float xPos, bool withFaces)
{
    EditorSurface surf;
    surf.textureName = name;
    const float coords[4][2] = { { -16.0f, -16.0f }, { 16.0f, -16.0f },
                                 { 16.0f, 16.0f },   { -16.0f, 16.0f } };
    for (int i = 0; i < 4; i++)
    {
        QrVertex v = {};
        v.position[0] = xPos;
        v.position[1] = coords[i][0];
        v.position[2] = coords[i][1];
        v.normal[0] = -1.0f;
        v.packedColor = 0xFFFFFFFFu;
        surf.vertices.push_back(v);
    }
    surf.indices = { 0, 1, 2, 0, 2, 3 };
    if (withFaces)
    {
        EditorSurfaceFace face;
        face.normal[0] = -1.0f;
        for (int i = 0; i < 4; i++)
        {
            face.positions.push_back(QrFloat3D{ { xPos, coords[i][0], coords[i][1] } });
            face.texCoords.push_back(QrFloat2D{ { 0.0f, 0.0f } });
        }
        surf.faces.push_back(std::move(face));
    }
    scene.surfaces.push_back(std::move(surf));
}

bool TestPickSceneFaceSkipsDisabledMaterials()
{
    EditorScene scene;
    AppendTestQuadSurface(scene, "textures/front_mat", 100.0f, true);
    AppendTestQuadSurface(scene, "textures/back_mat", 200.0f, true);
    AppendTestQuadSurface(scene, "textures/fallback_mat", 50.0f, false);

    EditorMaterialTable materials;
    materials.Rebuild(scene);
    const float origin[3] = { 0.0f, 0.0f, 0.0f };
    const float dir[3] = { 1.0f, 0.0f, 0.0f };

    if (!Check(PickSceneFace(scene, origin, dir, &materials).surface == 2 &&
                   PickSceneFace(scene, origin, dir, nullptr).surface == 2,
               "PickSceneFace should initially hit nearest fallback surface"))
    {
        return false;
    }

    materials.At(size_t(materials.IndexOf("textures/fallback_mat"))).enabled = false;
    if (!Check(PickSceneFace(scene, origin, dir, &materials).surface == 0,
               "PickSceneFace should skip disabled fallback surface"))
    {
        return false;
    }

    materials.At(size_t(materials.IndexOf("textures/front_mat"))).enabled = false;
    if (!Check(PickSceneFace(scene, origin, dir, &materials).surface == 1,
               "PickSceneFace should skip disabled face surface and pick behind it"))
    {
        return false;
    }

    materials.At(size_t(materials.IndexOf("textures/back_mat"))).enabled = false;
    return Check(PickSceneFace(scene, origin, dir, &materials).surface == -1 &&
                     PickSceneFace(scene, origin, dir, nullptr).surface == 2,
                 "PickSceneFace should return -1 when all hit surfaces are disabled");
}

bool TestEmitClippedAabbOutlineProjects12Edges()
{
    IMGUI_CHECKVERSION();
    ImGuiTestContextGuard ctxGuard(ImGui::CreateContext());
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *pixels = nullptr;
    int texW = 0;
    int texH = 0;
    io.Fonts->AddFontDefault();
    io.Fonts->GetTexDataAsRGBA32(&pixels, &texW, &texH);
    io.Fonts->SetTexID(ImTextureID(uintptr_t(1)));

    const EditorCameraBasis cam = {
        { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },
        1.0f, 1.0f, 800.0f, 600.0f,
    };

    ImGui::NewFrame();
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    const int baseIdx = bg->IdxBuffer.Size;
    bg->AddLine(ImVec2(10.0f, 10.0f), ImVec2(50.0f, 50.0f), IM_COL32(255, 255, 255, 255), 2.0f);
    const int oneLineIdx = bg->IdxBuffer.Size - baseIdx;

    const float boxMin[3] = { 80.0f, -10.0f, -10.0f };
    const float boxMax[3] = { 120.0f, 10.0f, 10.0f };
    const int boxStartIdx = bg->IdxBuffer.Size;
    EmitClippedAabbOutline(bg, cam, boxMin, boxMax, IM_COL32(255, 170, 40, 255), 2.0f);
    const int boxIdx = bg->IdxBuffer.Size - boxStartIdx;

    const float flatMin[3] = { 80.0f, -10.0f, 5.0f };
    const float flatMax[3] = { 120.0f, 10.0f, 5.0f };
    const int flatStartIdx = bg->IdxBuffer.Size;
    EmitClippedAabbOutline(bg, cam, flatMin, flatMax, IM_COL32(255, 170, 40, 255), 2.0f);
    const int flatIdx = bg->IdxBuffer.Size - flatStartIdx;

    const float behindMin[3] = { -120.0f, -10.0f, -10.0f };
    const float behindMax[3] = { -80.0f, 10.0f, 10.0f };
    const int behindStartIdx = bg->IdxBuffer.Size;
    EmitClippedAabbOutline(bg, cam, behindMin, behindMax, IM_COL32(255, 170, 40, 255), 2.0f);
    const int behindIdx = bg->IdxBuffer.Size - behindStartIdx;
    ImGui::Render();

    return Check(oneLineIdx > 0 && boxIdx == 12 * oneLineIdx && flatIdx == 12 * oneLineIdx &&
                     behindIdx == 0,
                 "EmitClippedAabbOutline should emit 12 edges for visible/flat AABBs and 0 behind");
}

void AppendLe32(std::vector<uint8_t> &buf, uint32_t value)
{
    for (int i = 0; i < 4; i++)
    {
        buf.push_back(uint8_t((value >> (i * 8)) & 0xFFu));
    }
}

void AppendLeF32(std::vector<uint8_t> &buf, float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    AppendLe32(buf, bits);
}

std::vector<uint8_t> BuildMinimalTestIdpoMdl()
{
    std::vector<uint8_t> bytes;
    AppendLe32(bytes, 0x4F504449u); // "IDPO"
    AppendLe32(bytes, 6u);          // version 6
    for (int i = 0; i < 3; i++)
    {
        AppendLeF32(bytes, 1.0f);
    }
    for (int i = 0; i < 7; i++)
    {
        AppendLe32(bytes, 0u);
    }
    for (uint32_t count : { 1u, 2u, 2u, 3u, 1u, 1u, 0u, 0u, 0u })
    {
        AppendLe32(bytes, count);
    }
    AppendLe32(bytes, 0u);
    bytes.insert(bytes.end(), 4, 1u);
    bytes.insert(bytes.end(), 36, 0u);
    for (uint32_t v : { 1u, 0u, 1u, 2u })
    {
        AppendLe32(bytes, v);
    }
    bytes.insert(bytes.end(), 28, 0u);
    const uint8_t verts[3][4] = {
        { 0, 0, 0, 0 },
        { 10, 2, 4, 0 },
        { 0, 20, 12, 0 },
    };
    for (const auto &v : verts)
    {
        bytes.insert(bytes.end(), v, v + 4);
    }
    return bytes;
}

std::vector<uint8_t> BuildMinimalTestBsp29WithEntity(const std::string &entities)
{
    std::vector<uint8_t> bsp(124, 0);
    const auto write32 = [&bsp](size_t offset, uint32_t value) {
        for (int i = 0; i < 4; i++)
        {
            bsp[offset + size_t(i)] = uint8_t((value >> (i * 8)) & 0xFFu);
        }
    };
    write32(0, 29u);
    for (size_t i = 0; i < 15; i++)
    {
        write32(4 + i * 8, 124u);
    }

    const uint32_t entOffset = uint32_t(bsp.size());
    bsp.insert(bsp.end(), entities.begin(), entities.end());
    bsp.push_back('\0');
    write32(4 + 0 * 8, entOffset);
    write32(8 + 0 * 8, uint32_t(entities.size() + 1));

    const uint32_t leafOffset = uint32_t(bsp.size());
    bsp.resize(bsp.size() + 56, 0);
    write32(leafOffset, uint32_t(-2));
    write32(leafOffset + 4, uint32_t(-1));
    write32(leafOffset + 28, uint32_t(-1));
    write32(leafOffset + 32, uint32_t(-1));
    write32(4 + 10 * 8, leafOffset);
    write32(8 + 10 * 8, 56u);
    return bsp;
}

bool WriteBinaryFile(const std::filesystem::path &path, const std::vector<uint8_t> &bytes)
{
    std::FILE *fp = std::fopen(path.string().c_str(), "wb");
    if (fp == nullptr)
    {
        return false;
    }
    const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), fp);
    std::fclose(fp);
    return written == bytes.size();
}

struct ScopedTempDir
{
    std::filesystem::path path;
    ~ScopedTempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

EditorCameraBasis MakeMdlBoundsTestCam()
{
    EditorCameraBasis cam{};
    cam.origin[1] = 55.0f;
    cam.origin[2] = -14.0f;
    cam.forward[0] = 1.0f;
    cam.right[1] = -1.0f;
    cam.up[2] = 1.0f;
    cam.tanHalfFovX = 1.0f;
    cam.tanHalfFovY = 0.75f;
    cam.viewportWidth = 800.0f;
    cam.viewportHeight = 600.0f;
    return cam;
}

bool BoundsNear(const EditorAliasInstance &inst, const float expectedMin[3],
                const float expectedMax[3])
{
    for (int axis = 0; axis < 3; axis++)
    {
        if (!Near(inst.boundsMin[axis], expectedMin[axis]) ||
            !Near(inst.boundsMax[axis], expectedMax[axis]))
        {
            return false;
        }
    }
    return true;
}

bool VerifyMdlBoundsMatchSurface(const EditorAliasInstance &inst, const EditorSurface &surface)
{
    const float expectedMin[3] = { 80.0f, 50.0f, -20.0f };
    const float expectedMax[3] = { 100.0f, 60.0f, -8.0f };
    if (!Check(inst.hasBounds && inst.aliasInstanceId != 0 &&
                   BoundsNear(inst, expectedMin, expectedMax) && !surface.vertices.empty(),
               "alias model world bounds did not match rotated/translated pose"))
    {
        return false;
    }
    for (const QrVertex &v : surface.vertices)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            if (v.position[axis] < inst.boundsMin[axis] - 1.0e-3f ||
                v.position[axis] > inst.boundsMax[axis] + 1.0e-3f)
            {
                return Check(false, "alias vertex exceeded aliasInstance world AABB");
            }
        }
    }
    return true;
}

int DrawTestAabbAndCountIndices(const float boundsMin[3], const float boundsMax[3])
{
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGuiTestContextGuard guard(ctx);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    unsigned char *pixels = nullptr;
    int fontW = 0;
    int fontH = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fontW, &fontH);
    ImGui::NewFrame();
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    const int startIdx = dl->IdxBuffer.Size;
    EmitClippedAabbOutline(dl, MakeMdlBoundsTestCam(), boundsMin, boundsMax,
                           IM_COL32(255, 190, 0, 255), 2.0f);
    const int drawnIdx = dl->IdxBuffer.Size - startIdx;
    ImGui::Render();
    return drawnIdx;
}

bool TestAliasModelWorldBounds()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    ScopedTempDir tmp{ std::filesystem::temp_directory_path() /
                       ("qray_test_mdl_bounds_" + std::to_string(stamp)) };
    std::error_code ec;
    std::filesystem::create_directories(tmp.path / "progs", ec);
    const std::filesystem::path mdlPath = tmp.path / "progs" / "test_box.mdl";
    if (!Check(!ec && WriteBinaryFile(mdlPath, BuildMinimalTestIdpoMdl()),
               "failed to write temp test_box.mdl"))
    {
        return false;
    }

    const std::string entities =
        "{\n\"classname\" \"worldspawn\"\n}\n"
        "{\n\"classname\" \"misc_model\"\n\"model\" \"progs/test_box.mdl\"\n"
        "\"origin\" \"100 50 -20\"\n\"angles\" \"0 90 0\"\n}\n";
    EditorAssetSource assets;
    EditorScene scene;
    std::string error;
    EditorSceneLoadOptions options;
    options.assets = &assets;
    if (!Check(assets.AddDirectory(tmp.path.string()), "AddDirectory failed") ||
        !Check(LoadEditorSceneBspMemory(BuildMinimalTestBsp29WithEntity(entities), "test_mdl.bsp",
                                        nullptr, scene, error, options),
               error.c_str()) ||
        !Check(scene.aliasInstances.size() == 1 && scene.surfaces.size() == 1,
               "expected 1 aliasInstance and 1 alias surface") ||
        !VerifyMdlBoundsMatchSurface(scene.aliasInstances[0], scene.surfaces[0]))
    {
        return false;
    }

    const int drawnIdx = DrawTestAabbAndCountIndices(scene.aliasInstances[0].boundsMin,
                                                     scene.aliasInstances[0].boundsMax);
    return Check(drawnIdx > 0 && (drawnIdx % 12) == 0,
                 "EmitClippedAabbOutline should emit 12 edges for transformed Q1 mdl AABB");
}

bool TestHighResNormalBakePreservesDetail()
{
    EditorScene scene;
    EditorTexture albedo;
    albedo.name = "low_albedo";
    albedo.width = 16;
    albedo.height = 16;
    albedo.rgba.assign(16u * 16u * 4u, 200u);
    scene.textures.push_back(std::move(albedo));

    EditorTexture normal;
    normal.name = "high_normal";
    normal.width = 64;
    normal.height = 64;
    normal.rgba.assign(64u * 64u * 4u, 128u);
    for (size_t i = 0; i < 64u * 64u; ++i)
    {
        normal.rgba[i * 4u + 2u] = 255u;
        normal.rgba[i * 4u + 3u] = 255u;
    }
    normal.rgba[(10u * 64u + 12u) * 4u + 0u] = 160u;
    scene.textures.push_back(std::move(normal));

    EditorMaterialParams params;
    params.normalTexture = "high_normal";
    params.bumpScale = 1.0f;

    EditorMaterialBakes bakes;
    const std::vector<QrMaterial> pool = { 10, 11 };
    const auto resolve = [](const std::string &n) -> int32_t {
        if (n == "low_albedo")
        {
            return 0;
        }
        if (n == "high_normal")
        {
            return 1;
        }
        return -1;
    };
    const QrInstance dummyInstance = reinterpret_cast<QrInstance>(uintptr_t(1));
    g_lastBakedNormal.clear();
    g_lastBakedWidth = 0;
    g_lastBakedHeight = 0;
    bakes.BeginPass();
    const QrMaterial baked = bakes.Get(dummyInstance, "low_albedo", params, scene, pool, resolve);
    bakes.EndPass(dummyInstance);
    bakes.Destroy(dummyInstance);

    const uint8_t boostedNx =
        g_lastBakedNormal.size() > ((10u * 64u + 12u) * 4u)
            ? g_lastBakedNormal[(10u * 64u + 12u) * 4u + 0u]
            : 0u;
    return Check(baked != QR_NO_MATERIAL && g_lastBakedWidth == 64u && g_lastBakedHeight == 64u &&
                     boostedNx > 160u,
                 "EditorMaterialBakes should preserve high-res normal dimensions and slope gain");
}

bool TestBakeInvalidateTexturePreservesUnrelatedMapBakes()
{
    auto makeSolidTex = [](const char *name) {
        EditorTexture tex;
        tex.name = name;
        tex.width = 4;
        tex.height = 4;
        tex.rgba.assign(4u * 4u * 4u, 180u);
        return tex;
    };

    EditorScene scene;
    scene.textures.push_back(makeSolidTex("map_wall"));
    scene.textures.push_back(makeSolidTex("map_wall_norm"));
    scene.textures.push_back(makeSolidTex("preview_skin"));
    scene.textures.push_back(makeSolidTex("preview_skin_gloss"));

    EditorMaterialParams mapParams;
    mapParams.normalTexture = "map_wall_norm";

    EditorMaterialParams previewParams;
    previewParams.glossTexture = "preview_skin_gloss";

    const std::vector<QrMaterial> pool = { 10, 11, 12, 13 };
    const auto resolve = [](const std::string &n) -> int32_t {
        if (n == "map_wall")
            return 0;
        if (n == "map_wall_norm")
            return 1;
        if (n == "preview_skin")
            return 2;
        if (n == "preview_skin_gloss")
            return 3;
        return -1;
    };

    EditorMaterialBakes bakes;
    const QrInstance dummyInstance = reinterpret_cast<QrInstance>(uintptr_t(1));
    bakes.BeginPass();
    const QrMaterial mapMat = bakes.Get(dummyInstance, "map_wall", mapParams, scene, pool, resolve);
    const QrMaterial previewMat =
        bakes.Get(dummyInstance, "preview_skin", previewParams, scene, pool, resolve);
    bakes.EndPass(dummyInstance);

    const bool removedPreview = bakes.InvalidateTexture(dummyInstance, 3);
    const bool unrelatedRemoved = bakes.InvalidateTexture(dummyInstance, 99);
    const QrMaterial mapMatAfter =
        bakes.Get(dummyInstance, "map_wall", mapParams, scene, pool, resolve);
    const bool ok = Check(mapMat != QR_NO_MATERIAL && previewMat != QR_NO_MATERIAL &&
                              removedPreview && !unrelatedRemoved && bakes.Count() == 1u &&
                              mapMatAfter == mapMat,
                          "InvalidateTexture should drop only targeted bakes and keep map bakes intact");
    bakes.Destroy(dummyInstance);
    return ok;
}

bool VerifyClearedYamlDroppedOwnedKeys(const std::string &merged)
{
    static constexpr const char *kAbsentKeys[] = {
        "texture_gloss", "texture_normals", "is_light", "bump_scale",
        "light_projector", "light_color", "light_brightness",
    };
    for (const char *key : kAbsentKeys)
    {
        if (!Check(merged.find(key) == std::string::npos,
                   "WriteMerged must drop cleared owned material keys"))
        {
            return false;
        }
    }
    return Check(merged.find("texture_emissive: NONE") != std::string::npos &&
                     merged.find("mirror: false") != std::string::npos &&
                     merged.find("light_upoffset: 12") != std::string::npos &&
                     merged.find("# keep unowned comment") != std::string::npos,
                 "WriteMerged must override shader defaults and keep unowned lines");
}

bool VerifyReloadedClearedParams(const EditorMaterialParams &afterReload)
{
    const bool clearedOk = afterReload.normalTexture.empty() &&
                           afterReload.glossTexture.empty() &&
                           afterReload.emissiveTexture.empty() &&
                           !afterReload.luma && !afterReload.mirror &&
                           !afterReload.lightProjector && !afterReload.hasLightColor;
    const bool floatsOk = std::fabs(afterReload.bumpScale - 1.0f) < 1e-4f &&
                          std::fabs(afterReload.lightBrightness - 1.0f) < 1e-4f;
    return Check(clearedOk && floatsOk,
                 "Reloaded cleared material must stay cleared and disabled");
}

bool TestMaterialYamlClearAndDefaultResetRoundTrip()
{
    const std::filesystem::path tempDir =
        std::filesystem::temp_directory_path() / "qray_material_clear_roundtrip";
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);

    const std::filesystem::path yamlPath = tempDir / "qray.materials.yaml";
    const std::string initialYaml =
        "qray_materials:\n"
        "  - name: models/mapobjects/bitch/orb\n"
        "    # keep unowned comment\n"
        "    light_upoffset: 12\n"
        "    texture_normals: models/mapobjects/bitch/orb_norm.tga\n"
        "    texture_gloss: models/mapobjects/bitch/orb_gloss.tga\n"
        "    texture_emissive: models/mapobjects/bitch/orb\n"
        "    bump_scale: 2.5\n"
        "    mirror: true\n"
        "    is_light: true\n"
        "    light_projector: true\n"
        "    light_color: ff4d4d\n"
        "    light_brightness: 0.5\n";

    std::string err;
    EditorMaterialsDocument doc;
    EditorMaterialParams cleared;
    if (!Check(WriteEditorFile(yamlPath.string(), initialYaml, err) &&
                   doc.Load(yamlPath.string(), err) &&
                   doc.ReadMaterial("models/mapobjects/bitch/orb", cleared),
               "failed to load initial qray.materials.yaml for clear round-trip test"))
    {
        std::filesystem::remove_all(tempDir, ec);
        return false;
    }

    cleared.normalTexture.clear();
    cleared.glossTexture.clear();
    cleared.emissiveTexture.clear();
    cleared.mirror = false;
    cleared.luma = false;
    cleared.bumpScale = 1.0f;
    cleared.lightProjector = false;
    cleared.hasLightColor = false;
    cleared.lightBrightness = 1.0f;

    EditorMaterialEntry entry;
    entry.name = "models/mapobjects/bitch/orb";
    entry.params = cleared;
    entry.clearDefaultEmissive = true;
    entry.clearDefaultMirror = true;

    const std::string merged = doc.WriteMerged({ entry });
    EditorMaterialsDocument reloaded;
    EditorMaterialParams afterReload;
    afterReload.emissiveTexture = "models/mapobjects/bitch/orb";
    afterReload.mirror = true;
    const bool savedOk = WriteEditorFile(yamlPath.string(), merged, err) &&
                         reloaded.Load(yamlPath.string(), err) &&
                         reloaded.ReadMaterial("models/mapobjects/bitch/orb", afterReload);

    const bool checksOk = Check(savedOk, "failed to save/reload cleared material") &&
                          VerifyClearedYamlDroppedOwnedKeys(merged) &&
                          VerifyReloadedClearedParams(afterReload);

    std::filesystem::remove_all(tempDir, ec);
    return checksOk;
}

EditorLightClipboard BuildSampleLightClipboard()
{
    EditorLight sceneLight;
    sceneLight.position[0] = 100.0f;
    sceneLight.position[1] = 200.0f;
    sceneLight.position[2] = 50.0f;
    sceneLight.hasOffset = true;
    sceneLight.offset[0] = 5.0f;
    sceneLight.offset[1] = -10.0f;
    sceneLight.offset[2] = 15.0f;
    sceneLight.className = "light";
    sceneLight.emitterUniqueID = 42u;
    sceneLight.modelSizeMeters = 1.5f;
    sceneLight.enabled = false;
    sceneLight.intensity = 600.0f;

    EditorLight customLight;
    customLight.position[0] = 125.0f;
    customLight.position[1] = 210.0f;
    customLight.position[2] = 65.0f;
    customLight.spot = true;
    customLight.angleOuter = 40.0f;

    EditorLightClipboard clip;
    clip.lights = BuildCopiedLights({ sceneLight }, { 0, 0, -1 }, 0, { customLight }, { 0 }, 0);
    clip.copyCameraForward[0] = 1.0f;
    return clip;
}

bool TestLightClipboardCopy()
{
    const auto floatEq = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
    const EditorLightClipboard clip = BuildSampleLightClipboard();
    if (!Check(clip.lights.size() == 2u, "BuildCopiedLights count/dedup mismatch"))
    {
        return false;
    }

    const EditorLight &c0 = clip.lights[0];
    const bool posOk = !c0.hasOffset && floatEq(c0.offset[0], 0.0f) &&
                       floatEq(c0.offset[1], 0.0f) && floatEq(c0.offset[2], 0.0f) &&
                       floatEq(c0.position[0], 105.0f) && floatEq(c0.position[1], 190.0f) &&
                       floatEq(c0.position[2], 65.0f);
    const bool metaOk = c0.className.empty() && c0.emitterUniqueID == 0u &&
                        floatEq(c0.modelSizeMeters, 0.0f) && c0.enabled &&
                        floatEq(c0.intensity, 600.0f);
    return Check(posOk && metaOk, "BuildCopiedLights sanitization / offset fold failed");
}

bool TestLightClipboardPaste()
{
    const auto floatEq = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
    EditorLightClipboard clip = BuildSampleLightClipboard();

    const float samePos[3] = { 4.0f, 0.0f, 0.0f };
    const float forward[3] = { 1.0f, 0.0f, 0.0f };
    const std::vector<EditorLight> paste1 = BuildPastedLights(clip, samePos, forward, 16.0f, 96.0f);
    const std::vector<EditorLight> paste2 = BuildPastedLights(clip, samePos, forward, 16.0f, 96.0f);
    const bool inPlaceOk = paste1.size() == 2u && paste2.size() == 2u &&
                           floatEq(paste1[0].position[0], 105.0f) &&
                           floatEq(paste1[0].position[2], 81.0f) &&
                           floatEq(paste1[1].position[0], 125.0f) &&
                           floatEq(paste2[1].position[2], 97.0f);

    const float movedPos[3] = { 500.0f, 100.0f, 50.0f };
    const std::vector<EditorLight> paste3 =
        BuildPastedLights(clip, movedPos, forward, 16.0f, 96.0f);
    const bool movedOk = paste3.size() == 2u &&
                         floatEq(paste3[0].position[0], 586.0f) &&
                         floatEq(paste3[0].position[1], 90.0f) &&
                         floatEq(paste3[1].position[0], 606.0f) &&
                         floatEq(paste3[1].position[1], 110.0f);
    return Check(inPlaceOk && movedOk, "BuildPastedLights stepping / group placement failed");
}

} // namespace

int main()
{
    using TestFn = bool (*)();
    static constexpr TestFn kTests[] = {
        TestMarqueeVisibilityAndSelection,
        TestMixedAggregationAndBatchOverwrite,
        TestEmitterMixedAggregationAndEdgeCases,
        TestGroupEditEmitterDirPreservation,
        TestMixedStyleAggregationAndSteadyVsNone,
        TestImGuiMixedQuestionRenderingAndEdit,
        VerifyEmitterNameCandidatesAndReset,
        VerifyAssetReadIgnoresDirectory,
        TestLevelDocumentStandardAndCustomLightsRoundTrip,
        TestBaselineCustomAndMapLightGuards,
        TestColocatedExtraClaimPrefersClassName,
        TestSmoothGlossFeathering,
        TestBrushStrokeEvaluation,
        TestBrushMaskYamlRoundTrip,
        TestGlossMultiBlockBlendAndBakeInvalidation,
        TestComputeMaskEdgeCases,
        TestEmissiveBrushBake,
        TestEditorUiOverlayAndBrushCanvas,
        TestAiTextureGenerationAndButton,
        TestPickSceneFaceSkipsDisabledMaterials,
        TestEmitClippedAabbOutlineProjects12Edges,
        TestAliasModelWorldBounds,
        TestHighResNormalBakePreservesDetail,
        TestBakeInvalidateTexturePreservesUnrelatedMapBakes,
        TestMaterialYamlClearAndDefaultResetRoundTrip,
        TestLightClipboardCopy,
        TestLightClipboardPaste,
    };

    for (TestFn test : kTests)
    {
        if (!test())
        {
            return 1;
        }
    }
    std::printf("All EditorMultiSelectTests passed.\n");
    return 0;
}
