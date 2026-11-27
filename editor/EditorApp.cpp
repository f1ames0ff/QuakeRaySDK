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

#include "EditorApp.h"

#include "EditorGui.h"
#include "EditorLightSelection.h"
#include "EditorOnnxRuntime.h"
#include "EditorSpatial.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <imgui.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace
{

uint64_t LightUniqueID(size_t index)
{
    return EditorLightUniqueID(index);
}

// Texture names are matched without case: Quake data mixes "FLOOR1_1" and "floor1_1" spellings
// for the same image.
std::string LowercaseName(const std::string &name)
{
    std::string lowered(name);

    for (char &character : lowered)
    {
        if (character >= 'A' && character <= 'Z')
        {
            character = char(character - 'A' + 'a');
        }
    }

    return lowered;
}

uint64_t CustomLightUniqueID(size_t index)
{
    return EditorCustomLightUniqueID(index);
}

// -1 is the Entity tab's NONE: the game's own combo offers it for a map light that must stay
// unmodulated, so the editor has to name it rather than clamp it to steady.
const char *EditorStyleName(int32_t style)
{
    if (style < 0 || style >= kEditorStyleCount)
    {
        return "none";
    }

    return kEditorStyleNames[style];
}

// A light the editor invents starts where the game's Custom tab starts: RT_CUSTOM_RADIUS_DEFAULT
// (0.4 m, 16 units at 40 units per metre) and twice the unit a level's `intensity` key states,
// which is the 200 the map's `light` key defaults to.
constexpr float kEditorNewLightRadiusUnits = 0.4f * kEditorUnitsPerMeter;
constexpr float kEditorNewLightIntensity = 2.0f * kEditorCustomLightIntensityUnits;

// A light is picked when the ray passes this close to its origin, the way QRE_LightUnderCrosshair
// accepts a light the crosshair is near rather than exactly on.
constexpr float kLightPickRadiusUnits = 24.0f;

// The game's DrawNotification lifetime: six seconds, fading out over the last one. The editor
// keeps the same numbers so a toast does not outlive the save it reports.
constexpr float kNotifySeconds = 6.0f;
constexpr float kNotifyFadeSeconds = 1.0f;

// The game's QRE_GIZMO_LEN: the drawn length of one axis arrow. The drag divides the frozen
// screen direction by the pixels this length spans, so dragging a whole arrow moves the light by
// exactly one arrow length whatever the distance to the camera.
constexpr float kGizmoLengthUnits = 12.0f;

// The game's grab tolerance around an axis line, arc or center glyph, in pixels.
constexpr float kGizmoGrabPixels = 10.0f;
constexpr float kGizmoCenterGrabPixels = 10.0f;
constexpr float kGizmoArcGrabPixels = 10.0f;
constexpr ImU32 kGizmoHoverColor = IM_COL32(255, 214, 64, 255);

// The length of the arm a drag adds its movement to, as a fraction of one arrow: the game draws
// its arrow heads at 28% of the arrow back from the tip.
constexpr float kGizmoHeadLength = 0.28f;

// A drag against this tolerance counts as a click rather than a move, so a press that does not
// travel leaves the light where the panel typed it.
constexpr float kGizmoClickPixels = 2.0f;

// QRE_UpdateLightDrag leaves the grabbed light this far in front of whatever the ray hits, and
// hangs it this far ahead of the camera when the ray hits nothing at all.
constexpr float kGrabSurfaceOffsetUnits = 8.0f;
constexpr float kGrabAirDistanceUnits = 128.0f;

// Distance from a point to a segment, in the pixels the gizmo is hit-tested in.
float DistanceToSegment(float px, float py, float x0, float y0, float x1, float y1)
{
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float lengthSquared = dx * dx + dy * dy;
    float t = 0.0f;

    if (lengthSquared > 0.0f)
    {
        t = std::clamp(((px - x0) * dx + (py - y0) * dy) / lengthSquared, 0.0f, 1.0f);
    }

    const float closestX = x0 + dx * t;
    const float closestY = y0 + dy * t;

    return std::sqrt((px - closestX) * (px - closestX) + (py - closestY) * (py - closestY));
}

void EffectiveLightPosition(const EditorLight &light, float outPos[3])
{
    outPos[0] = light.position[0] + (light.hasOffset ? light.offset[0] : 0.0f);
    outPos[1] = light.position[1] + (light.hasOffset ? light.offset[1] : 0.0f);
    outPos[2] = light.position[2] + (light.hasOffset ? light.offset[2] : 0.0f);
}

void ComputeGizmoAxis(const EditorLight &light, int32_t axis, float outDir[3])
{
    outDir[0] = 0.0f;
    outDir[1] = 0.0f;
    outDir[2] = 0.0f;
    outDir[axis] = 1.0f;

    if (light.spot && axis < 2)
    {
        const float length = std::sqrt(light.direction[0] * light.direction[0] +
                                       light.direction[1] * light.direction[1]);

        if (length > 1e-6f)
        {
            outDir[0] = (axis == 0 ? light.direction[0] : -light.direction[1]) / length;
            outDir[1] = (axis == 0 ? light.direction[1] : light.direction[0]) / length;
        }
    }
}

bool ProjectWorldPoint(const EditorCameraBasis &cam, const float point[3], float &outX, float &outY)
{
    if (cam.viewportWidth <= 0.0f || cam.viewportHeight <= 0.0f)
    {
        return false;
    }

    const float relative[3] = {
        point[0] - cam.origin[0],
        point[1] - cam.origin[1],
        point[2] - cam.origin[2],
    };

    const float depth = relative[0] * cam.forward[0] + relative[1] * cam.forward[1] +
                        relative[2] * cam.forward[2];

    if (depth <= 1e-3f)
    {
        return false;
    }

    const float horizontal = relative[0] * cam.right[0] + relative[1] * cam.right[1] +
                             relative[2] * cam.right[2];
    const float vertical = relative[0] * cam.up[0] + relative[1] * cam.up[1] +
                           relative[2] * cam.up[2];

    const float ndcX = horizontal / (depth * cam.tanHalfFovX);
    const float ndcY = vertical / (depth * cam.tanHalfFovY);

    outX = (ndcX * 0.5f + 0.5f) * cam.viewportWidth;
    outY = (1.0f - (ndcY * 0.5f + 0.5f)) * cam.viewportHeight;
    return true;
}

constexpr int kSpotArcSegs = 24;
constexpr size_t kSpotArcPointCount = size_t(kSpotArcSegs) + 1;
constexpr float kSpotArcSweepDeg = 300.0f;
constexpr float kSpotArcRadius = kGizmoLengthUnits * 1.2f;

struct GizmoArrowGeo
{
    float tip[3] = {};
    float head[2][3] = {};
};

GizmoArrowGeo BuildGizmoArrowGeo(const EditorCameraBasis &cam, const EditorLight &light,
                                 const float pos[3], int32_t axis)
{
    GizmoArrowGeo geo;
    float adir[3] = {};
    float tup[3] = {};
    ComputeGizmoAxis(light, axis, adir);
    ComputeGizmoAxis(light, (axis + 1) % 3, tup);

    for (int k = 0; k < 3; k++)
    {
        geo.tip[k] = pos[k] + kGizmoLengthUnits * adir[k];
    }

    const float depth = (pos[0] - cam.origin[0]) * cam.forward[0] +
                        (pos[1] - cam.origin[1]) * cam.forward[1] +
                        (pos[2] - cam.origin[2]) * cam.forward[2];
    const float widthScale = std::clamp(depth * 0.003f, 0.4f, 8.0f);

    for (int side = 0; side < 2; side++)
    {
        const float sign = (side == 0) ? 1.0f : -1.0f;
        for (int k = 0; k < 3; k++)
        {
            geo.head[side][k] = geo.tip[k] - kGizmoLengthUnits * kGizmoHeadLength * adir[k] +
                                sign * widthScale * 6.0f * tup[k];
        }
    }

    return geo;
}

void DrawGizmoAxisArrows(ImDrawList *drawList, const EditorCameraBasis &cam,
                         const EditorLight &light, const float pos[3], int32_t hoverAxis)
{
    static constexpr ImU32 axisColors[3] = {
        IM_COL32(255, 64, 64, 255),
        IM_COL32(64, 255, 64, 255),
        IM_COL32(64, 128, 255, 255),
    };

    float originX = 0.0f;
    float originY = 0.0f;

    if (!ProjectWorldPoint(cam, pos, originX, originY))
    {
        return;
    }

    for (int32_t axis = 0; axis < 3; axis++)
    {
        const GizmoArrowGeo geo = BuildGizmoArrowGeo(cam, light, pos, axis);
        float tipX = 0.0f;
        float tipY = 0.0f;

        if (!ProjectWorldPoint(cam, geo.tip, tipX, tipY))
        {
            continue;
        }

        const ImU32 color = (axis == hoverAxis) ? kGizmoHoverColor : axisColors[axis];
        drawList->AddLine(ImVec2(originX, originY), ImVec2(tipX, tipY), color, 6.0f);

        for (int side = 0; side < 2; side++)
        {
            float headX = 0.0f;
            float headY = 0.0f;

            if (ProjectWorldPoint(cam, geo.head[side], headX, headY))
            {
                drawList->AddLine(ImVec2(tipX, tipY), ImVec2(headX, headY), color, 6.0f);
            }
        }
    }
}

bool BuildSpotArcPoints(const EditorCameraBasis &cam, const EditorLight &light,
                        const float center[3], int32_t axis,
                        std::array<ImVec2, kSpotArcPointCount> &outPts)
{
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    float u[3] = {};
    float v[3] = {};
    ComputeGizmoAxis(light, (axis + 1) % 3, u);
    ComputeGizmoAxis(light, (axis + 2) % 3, v);

    for (size_t i = 0; i < kSpotArcPointCount; i++)
    {
        const float a = (kSpotArcSweepDeg * float(i) / float(kSpotArcSegs)) * kDegToRad;
        const float cs = std::cos(a);
        const float sn = std::sin(a);
        const float p[3] = {
            center[0] + kSpotArcRadius * (cs * u[0] + sn * v[0]),
            center[1] + kSpotArcRadius * (cs * u[1] + sn * v[1]),
            center[2] + kSpotArcRadius * (cs * u[2] + sn * v[2]),
        };

        if (!ProjectWorldPoint(cam, p, outPts[i].x, outPts[i].y))
        {
            return false;
        }
    }

    return true;
}

void DrawSpotDirArc(ImDrawList *drawList, const EditorCameraBasis &cam, const EditorLight &light,
                    const float center[3], int32_t axis, ImU32 color)
{
    std::array<ImVec2, kSpotArcPointCount> pts = {};

    if (!BuildSpotArcPoints(cam, light, center, axis, pts))
    {
        return;
    }

    drawList->AddPolyline(pts.data(), int(kSpotArcPointCount), color, ImDrawFlags_None, 6.0f);

    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    constexpr float kEndAngle = kSpotArcSweepDeg * kDegToRad;
    constexpr float kHeadAngle = 28.0f * kDegToRad;
    constexpr float kHeadLen = kSpotArcRadius * 0.22f;

    float u[3] = {};
    float v[3] = {};
    ComputeGizmoAxis(light, (axis + 1) % 3, u);
    ComputeGizmoAxis(light, (axis + 2) % 3, v);

    const float ec = std::cos(kEndAngle);
    const float es = std::sin(kEndAngle);
    const float ch = std::cos(kHeadAngle);
    const float sh = std::sin(kHeadAngle);
    const float ex = pts[kSpotArcPointCount - 1].x;
    const float ey = pts[kSpotArcPointCount - 1].y;

    for (int i = 0; i < 2; i++)
    {
        const float side = (i == 0) ? 1.0f : -1.0f;
        float wing[3] = {};

        for (int k = 0; k < 3; k++)
        {
            const float radial = ec * u[k] + es * v[k];
            const float tangent = -es * u[k] + ec * v[k];
            wing[k] = center[k] + kSpotArcRadius * radial + kHeadLen * (-ch * tangent + side * sh * radial);
        }

        float wx = 0.0f;
        float wy = 0.0f;

        if (ProjectWorldPoint(cam, wing, wx, wy))
        {
            drawList->AddLine(ImVec2(ex, ey), ImVec2(wx, wy), color, 6.0f);
        }
    }
}

struct GizmoAxisHit
{
    bool valid = false;
    float distance = 1e30f;
    float dir[2] = {};
    float pixelsPerUnit = 0.0f;
};

GizmoAxisHit HitTestGizmoAxisArrow(const EditorCameraBasis &cam, const EditorLight &light,
                                   const float pos[3], int32_t axis, ImVec2 cursor)
{
    float originX = 0.0f;
    float originY = 0.0f;
    const GizmoArrowGeo geo = BuildGizmoArrowGeo(cam, light, pos, axis);
    float tipX = 0.0f;
    float tipY = 0.0f;

    if (!ProjectWorldPoint(cam, pos, originX, originY) ||
        !ProjectWorldPoint(cam, geo.tip, tipX, tipY))
    {
        return {};
    }

    const float dx = tipX - originX;
    const float dy = tipY - originY;
    const float pixelLength = std::hypot(dx, dy);

    if (pixelLength < 4.0f)
    {
        return {};
    }

    GizmoAxisHit hit;
    hit.valid = true;
    hit.distance = DistanceToSegment(cursor.x, cursor.y, originX, originY, tipX, tipY);
    hit.dir[0] = dx / pixelLength;
    hit.dir[1] = dy / pixelLength;
    hit.pixelsPerUnit = pixelLength / kGizmoLengthUnits;

    for (int side = 0; side < 2; side++)
    {
        float hx = 0.0f;
        float hy = 0.0f;

        if (ProjectWorldPoint(cam, geo.head[side], hx, hy))
        {
            hit.distance =
                std::min(hit.distance, DistanceToSegment(cursor.x, cursor.y, tipX, tipY, hx, hy));
        }
    }

    return hit;
}

bool HitTestSpotDirArc(const EditorCameraBasis &cam, const EditorLight &light,
                       const float center[3], int32_t axis, ImVec2 cursor, float &outDist)
{
    std::array<ImVec2, kSpotArcPointCount> pts = {};

    if (!BuildSpotArcPoints(cam, light, center, axis, pts))
    {
        return false;
    }

    outDist = 1e30f;

    for (size_t i = 0; i + 1 < kSpotArcPointCount; i++)
    {
        const float d =
            DistanceToSegment(cursor.x, cursor.y, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y);
        outDist = std::min(outDist, d);
    }

    return true;
}

struct GizmoHitResult
{
    EditorApp::GizmoPart part = EditorApp::GizmoPart::None;
    int32_t axis = -1;
    float distance = 1e30f;
    float dir[2] = {};
    float pixelsPerUnit = 0.0f;
};

GizmoHitResult HitTestCustomLightGizmo(const EditorCameraBasis &cam, const EditorLight &light,
                                       ImVec2 cursor)
{
    float pos[3] = {};
    EffectiveLightPosition(light, pos);

    float originX = 0.0f;
    float originY = 0.0f;

    if (!ProjectWorldPoint(cam, pos, originX, originY))
    {
        return {};
    }

    const float originDist = std::hypot(cursor.x - originX, cursor.y - originY);

    if (originDist <= kGizmoCenterGrabPixels)
    {
        return { EditorApp::GizmoPart::Center, -1, originDist, {}, 0.0f };
    }

    GizmoHitResult best;

    for (int32_t axis = 0; axis < 3; axis++)
    {
        const GizmoAxisHit ah = HitTestGizmoAxisArrow(cam, light, pos, axis, cursor);

        if (ah.valid && ah.distance <= kGizmoGrabPixels && ah.distance < best.distance)
        {
            best.part = EditorApp::GizmoPart::Axis;
            best.axis = axis;
            best.distance = ah.distance;
            best.dir[0] = ah.dir[0];
            best.dir[1] = ah.dir[1];
            best.pixelsPerUnit = ah.pixelsPerUnit;
        }
    }

    if (light.spot)
    {
        for (int32_t axis = 1; axis < 3; axis++)
        {
            float arcDist = 0.0f;

            if (HitTestSpotDirArc(cam, light, pos, axis, cursor, arcDist) &&
                arcDist <= kGizmoArcGrabPixels && arcDist < best.distance)
            {
                best.part = EditorApp::GizmoPart::SpotArc;
                best.axis = axis;
                best.distance = arcDist;
            }
        }
    }

    return best;
}

void RotateAroundAxis(const float axis[3], const float vec[3], float degrees, float out[3])
{
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    const float rad = degrees * kDegToRad;
    const float cs = std::cos(rad);
    const float sn = std::sin(rad);
    const float dot = axis[0] * vec[0] + axis[1] * vec[1] + axis[2] * vec[2];
    const float cross[3] = {
        axis[1] * vec[2] - axis[2] * vec[1],
        axis[2] * vec[0] - axis[0] * vec[2],
        axis[0] * vec[1] - axis[1] * vec[0],
    };

    for (int i = 0; i < 3; i++)
    {
        out[i] = vec[i] * cs + cross[i] * sn + axis[i] * dot * (1.0f - cs);
    }

    const float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);

    if (len > 1e-6f)
    {
        out[0] /= len;
        out[1] /= len;
        out[2] /= len;
    }
}

// The vertical field of view the frame is drawn with. Picking and the selection overlay invert it,
// so they must not carry their own copy -- EditorApp::FovYRadians is the only source, and it
// ports the game's fov/fov_adapt conversion rather than naming a lens of its own.

const char *SurfaceKindName(EditorSurfaceKind kind)
{
    switch (kind)
    {
    case EditorSurfaceKind::Fence:
        return "fence";
    case EditorSurfaceKind::Water:
        return "water";
    case EditorSurfaceKind::Slime:
        return "slime";
    case EditorSurfaceKind::Lava:
        return "lava";
    case EditorSurfaceKind::Teleport:
        return "teleport";
    case EditorSurfaceKind::Sky:
        return "sky";
    default:
        return "opaque";
    }
}


struct FileBlob
{
    std::vector<uint8_t> bytes;
};

std::string ResolveDefaultAssetPath(const std::string &relativePath)
{
    std::error_code ec;

    if (std::filesystem::exists(relativePath, ec))
    {
        return relativePath;
    }

    const std::string exeDir = EditorExecutableDirectory();

    if (!exeDir.empty())
    {
        const std::filesystem::path candidate = std::filesystem::path(exeDir) / relativePath;

        if (std::filesystem::exists(candidate, ec))
        {
            return candidate.string();
        }
    }

    return relativePath;
}

std::string NormalizeAssetKey(const std::string &name)
{
    std::string normalized = LowercaseName(name);

    for (char &character : normalized)
    {
        if (character == '\\')
        {
            character = '/';
        }
    }

    return normalized;
}

bool HasZipPackExtension(const std::filesystem::path &path)
{
    const std::string ext = LowercaseName(path.extension().string());
    if (ext == ".pkz" || ext == ".pk3")
    {
        return true;
    }
    if (const EditorFormatAdapter *adapter = GetEditorFormatAdapter())
    {
        for (const std::string &extraExt : adapter->extraArchiveExtensions)
        {
            if (LowercaseName(extraExt) == ext)
            {
                return true;
            }
        }
    }
    return false;
}

bool HasPakExtension(const std::filesystem::path &path)
{
    return LowercaseName(path.extension().string()) == ".pak";
}

void NormalizeFolder(std::string &folder)
{
    if (!folder.empty())
    {
        const char last = folder.back();

        if (last != '/' && last != '\\')
        {
            folder.push_back('/');
        }
    }
}

void LookAt(const float eye[3], const float target[3], const float up[3], float view[16])
{
    float forward[3] = {
        target[0] - eye[0],
        target[1] - eye[1],
        target[2] - eye[2],
    };

    const float forwardLength = std::sqrt(forward[0] * forward[0] +
                                          forward[1] * forward[1] +
                                          forward[2] * forward[2]);
    forward[0] /= forwardLength;
    forward[1] /= forwardLength;
    forward[2] /= forwardLength;

    float side[3] = {
        forward[1] * up[2] - forward[2] * up[1],
        forward[2] * up[0] - forward[0] * up[2],
        forward[0] * up[1] - forward[1] * up[0],
    };

    const float sideLength = std::sqrt(side[0] * side[0] +
                                       side[1] * side[1] +
                                       side[2] * side[2]);
    side[0] /= sideLength;
    side[1] /= sideLength;
    side[2] /= sideLength;

    const float correctedUp[3] = {
        side[1] * forward[2] - side[2] * forward[1],
        side[2] * forward[0] - side[0] * forward[2],
        side[0] * forward[1] - side[1] * forward[0],
    };

    view[0] = side[0];
    view[1] = correctedUp[0];
    view[2] = -forward[0];
    view[3] = 0.0f;

    view[4] = side[1];
    view[5] = correctedUp[1];
    view[6] = -forward[1];
    view[7] = 0.0f;

    view[8] = side[2];
    view[9] = correctedUp[2];
    view[10] = -forward[2];
    view[11] = 0.0f;

    view[12] = -(side[0] * eye[0] + side[1] * eye[1] + side[2] * eye[2]);
    view[13] = -(correctedUp[0] * eye[0] + correctedUp[1] * eye[1] + correctedUp[2] * eye[2]);
    view[14] = forward[0] * eye[0] + forward[1] * eye[1] + forward[2] * eye[2];
    view[15] = 1.0f;
}

}

EditorApp::EditorApp(void *nativeInstance)
    : m_nativeInstance(nativeInstance)
    , m_scene(m_runtime.scene)
    , m_materials(m_runtime.materials)
    , m_materialBakes(m_runtime.materialBakes)
    , m_settings(m_runtime.settings)
    , m_sun(m_runtime.sun)
    , m_customLights(m_runtime.customLights)
    , m_emissiveLights(m_runtime.emissiveLights)
    , m_generatedLightBases(m_runtime.generatedLightBases)
    , m_emitterLightOverrides(m_runtime.emitterLightOverrides)
    , m_textureMaterials(m_runtime.textureMaterials)
    , m_textureEmissions(m_runtime.textureEmissions)
    , m_textureIndexByName(m_runtime.textureIndexByName)
    , m_missingExternalTextures(m_runtime.missingExternalTextures)
{
}

EditorApp::~EditorApp()
{
    Shutdown();
}

void EditorApp::PrintMessage(const char *message, void *)
{
    if (message != nullptr)
    {
        EditorDebugLog(message);

        if (FILE *file = std::fopen("editor.log", "a"))
        {
            std::fprintf(file, "%s\n", message);
            std::fclose(file);
        }
    }
}

void EditorApp::OpenFile(const char *path, void *, const void **data, uint32_t *size,
                         void **fileHandle)
{
    *data = nullptr;
    *size = 0;
    *fileHandle = nullptr;

    if (path == nullptr)
    {
        return;
    }

    std::ifstream file(path, std::ios::binary | std::ios::ate);

    if (!file)
    {
        return;
    }

    const std::streamsize length = file.tellg();

    if (length <= 0)
    {
        return;
    }

    FileBlob *blob = new FileBlob();
    blob->bytes.resize(size_t(length));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(blob->bytes.data()), length);

    if (!file)
    {
        delete blob;
        return;
    }

    *data = blob->bytes.data();
    *size = uint32_t(blob->bytes.size());
    *fileHandle = blob;
}

void EditorApp::CloseFile(void *fileHandle, void *)
{
    delete static_cast<FileBlob *>(fileHandle);
}

bool EditorApp::CreateWindowInstance(int showCommand)
{
    return m_window.Create("QuakeRay Editor", 1280, 720, m_nativeInstance, showCommand);
}

bool EditorApp::CreateRenderer()
{
    EditorSurfaceStorage surfaceStorage = {};

    QrInstanceCreateInfo createInfo = {};
    createInfo.pAppName = "QuakeRay Editor";
    createInfo.pAppGUID = "5a7f79c2-38a2-4f3d-9a34-9b0c4f2a9c71";
    m_window.BindSurface(createInfo, surfaceStorage);
    createInfo.pfnPrint = PrintMessage;
    createInfo.pfnOpenFile = OpenFile;
    createInfo.pfnCloseFile = CloseFile;
    createInfo.pUserLoadFileData = this;
    createInfo.pShaderFolderPath = m_shaderFolder.c_str();
    createInfo.pBlueNoiseFilePath = m_blueNoisePath.c_str();
    createInfo.pWaterNormalTexturePath =
        m_waterNormalPath.empty() ? nullptr : m_waterNormalPath.c_str();
    createInfo.primaryRaysMaxAlbedoLayers = 2;
    createInfo.indirectIlluminationMaxAlbedoLayers = 1;
    createInfo.rayCullBackFacingTriangles = 1;
    createInfo.allowGeometryWithSkyFlag = 1;
    createInfo.rasterizedMaxVertexCount = 1u << 20;
    createInfo.rasterizedMaxIndexCount = 1u << 21;
    createInfo.rasterizedVertexColorGamma = 1;
    createInfo.rasterizedSkyCubemapSize = 256;
    createInfo.maxTextureCount = 4096;
    createInfo.textureSamplerForceMinificationFilterLinear = 1;
    createInfo.textureSamplerForceNormalMapFilterLinear = 1;
    createInfo.originalAlbedoAlphaTextureIsSRGB = 1;
    createInfo.originalRoughnessMetallicEmissionTextureIsSRGB = 0;
    createInfo.originalNormalTextureIsSRGB = 0;
    createInfo.overridenAlbedoAlphaTextureIsSRGB = 1;
    createInfo.overridenRoughnessMetallicEmissionTextureIsSRGB = 0;
    createInfo.overridenNormalTextureIsSRGB = 0;

    const QrResult result = qrCreateInstance(&createInfo, &m_instance);
    PrintMessage(("instance: " + std::string(qrGetResultDescription(result))).c_str(), this);

    if (result != QR_SUCCESS)
    {
        EditorShowError(&m_window, "QuakeRay Editor", qrGetResultDescription(result));
        return false;
    }

    return true;
}

bool EditorApp::CreateMaterial()
{
    if (m_instance == nullptr)
    {
        return false;
    }

    const uint8_t albedo[16] = {
        220, 190, 150, 255, 120, 100, 80, 255,
        120, 100, 80, 255, 220, 190, 150, 255,
    };

    QrMaterialCreateInfo info = {};
    info.size = { 2, 2 };
    info.textures.pDataAlbedoAlpha = albedo;
    info.filter = QR_SAMPLER_FILTER_LINEAR;
    info.addressModeU = QR_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = QR_SAMPLER_ADDRESS_MODE_REPEAT;

    const QrResult result = qrCreateMaterial(m_instance, &info, &m_material);
    PrintMessage(("material: " + std::string(qrGetResultDescription(result))).c_str(), this);
    return result == QR_SUCCESS;
}

QrGeometryPassThroughType EditorApp::PassThroughTypeForSurface(EditorSurfaceKind kind)
{
    return EditorSceneRuntime::PassThroughTypeForSurface(kind);
}

namespace
{

bool AnySceneSurfaceUsesTexture(const EditorScene &scene, int32_t textureIndex)
{
    for (const EditorSurface &surf : scene.surfaces)
    {
        if (surf.texture == textureIndex)
        {
            return true;
        }
    }
    return false;
}

} // namespace

int32_t EditorApp::RegisterOrUpdateGeneratedTexture(const std::string &relPath, uint32_t width,
                                                    uint32_t height,
                                                    const std::vector<uint8_t> &rgba)
{
    const std::string key = NormalizeAssetKey(relPath);
    if (key.empty() || width == 0 || height == 0 || rgba.size() != size_t(width) * size_t(height) * 4u)
    {
        return -1;
    }

    m_missingExternalTextures.erase(key);
    m_textureBrowserCatalogBuilt = false;

    const int32_t existing = TextureIndexByName(key);
    if (existing >= 0 && size_t(existing) < m_scene.textures.size())
    {
        EditorTexture &tex = m_scene.textures[size_t(existing)];
        tex.width = width;
        tex.height = height;
        tex.rgba = rgba;
        tex.fullbright.clear();
        tex.roughnessMask.clear();
        tex.metallicMask.clear();

        if (m_instance != nullptr && size_t(existing) < m_textureMaterials.size())
        {
            if (m_textureMaterials[size_t(existing)] != QR_NO_MATERIAL)
            {
                qrDestroyMaterial(m_instance, m_textureMaterials[size_t(existing)]);
                m_textureMaterials[size_t(existing)] = QR_NO_MATERIAL;
            }
            EditorMaterialEmission emission;
            EditorSceneRuntime::CreatePoolMaterial(m_instance, tex,
                                                   m_textureMaterials[size_t(existing)], emission);
            if (size_t(existing) < m_textureEmissions.size())
            {
                m_textureEmissions[size_t(existing)] = emission;
            }
            if (m_materialBakes.InvalidateTexture(m_instance, existing) ||
                AnySceneSurfaceUsesTexture(m_scene, existing))
            {
                m_materialsChanged = true;
            }
            m_previewMaterialBakes.InvalidateTexture(m_instance, existing);
        }
        return existing;
    }

    const int32_t newIndex = int32_t(m_scene.textures.size());
    EditorTexture tex;
    tex.name = key;
    tex.width = width;
    tex.height = height;
    tex.rgba = rgba;
    tex.isExternal = true;
    m_scene.textures.push_back(std::move(tex));

    QrMaterial material = QR_NO_MATERIAL;
    EditorMaterialEmission emission;
    if (m_instance != nullptr)
    {
        EditorSceneRuntime::CreatePoolMaterial(m_instance, m_scene.textures.back(), material,
                                               emission);
    }

    m_textureMaterials.push_back(material);
    m_textureEmissions.push_back(emission);
    m_textureIndexByName.emplace(key, newIndex);
    static constexpr const char kPrefix[] = "textures/";
    if (key.rfind(kPrefix, 0) == 0)
    {
        m_textureIndexByName.emplace(key.substr(sizeof(kPrefix) - 1), newIndex);
    }
    return newIndex;
}

QrMaterial EditorApp::SurfaceMaterial(const EditorSurface &surface) const
{
    return m_runtime.SurfaceMaterial(surface, m_material);
}

int32_t EditorApp::TextureIndexByName(const std::string &name) const
{
    return m_runtime.TextureIndexByName(name);
}

int32_t EditorApp::ResolveOrLoadTexture(const std::string &name)
{
    return m_runtime.ResolveOrLoadTexture(m_instance, m_assets, name);
}

EditorMaterialEmission EditorApp::MaterialEmissionOf(QrMaterial material) const
{
    return m_runtime.MaterialEmissionOf(material);
}

QrMaterial EditorApp::SurfaceMaterialWithOverride(const EditorSurface &surface,
                                                 const EditorMaterialParams &params)
{
    return m_runtime.SurfaceMaterialWithOverride(m_instance, m_assets, surface, params, m_material);
}

void EditorApp::DestroyPoolMaterials()
{
    m_runtime.DestroyPoolMaterials(m_instance);
    m_textureBrowserCatalogBuilt = false;
}

void EditorApp::UploadScrollingSky(double timeSeconds, float skyDistance)
{
    m_runtime.UploadSky(m_instance, m_cameraPosition, timeSeconds, skyDistance);
}

bool EditorApp::CreateTextureMaterials()
{
    if (m_instance == nullptr)
    {
        return false;
    }

    m_previewMaterialBakes.Destroy(m_instance);
    m_textureBrowserCatalogBuilt = false;
    return m_runtime.CreateTextureMaterials(
        m_instance, m_assets, [this](const char *msg) { PrintMessage(msg, this); });
}

namespace
{

std::string FileExtensionLower(const std::string &path)
{
    const std::filesystem::path fsPath(path);
    return LowercaseName(fsPath.extension().string());
}

std::string LevelKeyFromPath(const std::string &scenePath)
{
    return LowercaseName(std::filesystem::path(scenePath).stem().string());
}

bool HasAdapterModMarkers(const std::filesystem::path &dir)
{
    const EditorFormatAdapter *adapter = GetEditorFormatAdapter();
    if (adapter == nullptr)
    {
        return false;
    }
    std::error_code ec;
    for (const std::string &subdir : adapter->extraModSubdirs)
    {
        if (std::filesystem::is_directory(dir / subdir, ec))
        {
            return true;
        }
    }
    for (const std::string &cfg : adapter->extraModConfigFiles)
    {
        if (std::filesystem::is_regular_file(dir / cfg, ec))
        {
            return true;
        }
    }
    return false;
}

bool IsQuakeModDirectory(const std::filesystem::path &dir)
{
    std::error_code ec;

    if (!std::filesystem::is_directory(dir, ec))
    {
        return false;
    }

    if (std::filesystem::is_directory(dir / "maps", ec) ||
        std::filesystem::is_regular_file(dir / "config.cfg", ec) ||
        std::filesystem::is_regular_file(dir / "qray.materials.yaml", ec) ||
        std::filesystem::is_regular_file(dir / "qray.lights.yaml", ec) ||
        HasAdapterModMarkers(dir))
    {
        return true;
    }

    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(dir, ec))
    {
        if (!entry.is_regular_file(ec))
        {
            continue;
        }

        if (HasPakExtension(entry.path()) || HasZipPackExtension(entry.path()))
        {
            return true;
        }
    }

    return false;
}

bool IsSceneUnderModMapsDir(const std::string &scenePath, const std::string &gamePath)
{
    if (gamePath.empty() || scenePath.empty())
    {
        return false;
    }

    std::error_code ec;
    const std::filesystem::path sceneParent =
        std::filesystem::weakly_canonical(std::filesystem::path(scenePath).parent_path(), ec);
    const std::filesystem::path modMaps =
        std::filesystem::weakly_canonical(std::filesystem::path(gamePath) / "maps", ec);
    return !ec && sceneParent == modMaps;
}

EditorSessionPaths BuildEffectiveSessionPaths(const std::string &materialsOverride,
                                              const std::string &lightsOverride,
                                              const std::string &gamePath,
                                              const std::string &scenePath)
{
    std::string matTarget = materialsOverride;
    std::string lightTarget = lightsOverride;

    std::error_code ec;
    const std::string sceneDir = EditorDirectoryOf(scenePath);
    const bool useGameDir =
        !gamePath.empty() &&
        (scenePath.empty() || sceneDir.empty() || !std::filesystem::is_regular_file(scenePath, ec) ||
         IsSceneUnderModMapsDir(scenePath, gamePath));

    if (matTarget.empty() && useGameDir)
    {
        matTarget = (std::filesystem::path(gamePath) / "qray.materials.yaml").string();
    }

    if (lightTarget.empty() && useGameDir)
    {
        lightTarget = (std::filesystem::path(gamePath) / "qray.lights.yaml").string();
    }

    return BuildEditorSessionPaths(matTarget, lightTarget, scenePath);
}

void MountAdapterAncestorBaseDirs(EditorAssetSource &assets, const std::string &directory)
{
    const EditorFormatAdapter *adapter = GetEditorFormatAdapter();
    if (adapter == nullptr)
    {
        return;
    }
    for (const std::string &baseDir : adapter->extraBaseGameDirs)
    {
        std::string direct = directory;
        direct.push_back('/');
        direct.append(baseDir);
        MountEditorGameDirectory(assets, direct);
        std::string debugDir = directory;
        debugDir.append("/Debug/");
        debugDir.append(baseDir);
        MountEditorGameDirectory(assets, debugDir);
    }
}

void MountSceneAncestors(EditorAssetSource &assets, const std::string &scenePath)
{
    const std::string sceneDirectory = EditorDirectoryOf(scenePath);
    std::string base = sceneDirectory.empty() ? std::string(".") : sceneDirectory;
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(base, error);

    if (!error)
    {
        base = absolute.lexically_normal().string();
    }

    std::string directory = base;

    for (int level = 0; level < 6; level++)
    {
        const std::string candidates[3] = {
            directory,
            directory + "/id1",
            directory + "/Debug/id1",
        };

        for (const std::string &candidate : candidates)
        {
            MountEditorGameDirectory(assets, candidate);
        }
        MountAdapterAncestorBaseDirs(assets, directory);

        const size_t slash = directory.find_last_of("\\/");

        if (slash == std::string::npos)
        {
            break;
        }

        directory = slash == 0 ? directory.substr(0, 1) : directory.substr(0, slash);
    }
}

void CollectModsInRoot(const std::filesystem::path &root, std::vector<std::string> &out)
{
    std::error_code ec;

    if (!std::filesystem::is_directory(root, ec))
    {
        return;
    }

    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(root, ec))
    {
        if (!entry.is_directory(ec) || !IsQuakeModDirectory(entry.path()))
        {
            continue;
        }

        const std::string norm = entry.path().lexically_normal().string();

        if (std::find(out.begin(), out.end(), norm) == out.end())
        {
            out.push_back(norm);
        }
    }
}

bool LoadNextEditorScene(const std::string &path, const EditorAssetSource &assets,
                         bool multiplayerMode, const uint8_t *palette, EditorScene &outScene,
                         std::string &error)
{
    const EditorSceneLoadOptions options{ &assets, multiplayerMode };
    std::error_code ec;

    if (std::filesystem::is_regular_file(path, ec))
    {
        return LoadEditorScene(path, palette, outScene, error, options);
    }

    if (FileExtensionLower(path) == ".bsp")
    {
        std::vector<uint8_t> bytes;

        if (!assets.ReadFile(path, bytes))
        {
            error = "cannot find BSP asset " + path;
            return false;
        }

        return LoadEditorSceneBspMemory(bytes, path, palette, outScene, error, options);
    }

    return LoadEditorScene(path, palette, outScene, error, options);
}

std::filesystem::path CanonicalOrLexicalPath(const std::filesystem::path &path)
{
    std::error_code ec;
    const std::filesystem::path canon = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : canon;
}

void MountDirectoryIfNew(EditorAssetSource &assets, const std::filesystem::path &dir,
                         std::vector<std::filesystem::path> &mounted)
{
    std::error_code ec;

    if (dir.empty() || !std::filesystem::is_directory(dir, ec))
    {
        return;
    }

    const std::filesystem::path canon = CanonicalOrLexicalPath(dir);

    if (std::find(mounted.begin(), mounted.end(), canon) != mounted.end())
    {
        return;
    }

    mounted.push_back(canon);
    MountEditorGameDirectory(assets, dir.string());
}

void InitBrowserDirectory(std::string &dir, char *input, size_t inputSize,
                          const std::string &scenePath, const std::string &gamePath)
{
    std::error_code ec;

    if (!dir.empty() && std::filesystem::is_directory(dir, ec))
    {
        std::snprintf(input, inputSize, "%s", dir.c_str());
        return;
    }

    if (!scenePath.empty())
    {
        const std::filesystem::path p = std::filesystem::path(scenePath).parent_path();

        if (!p.empty() && std::filesystem::is_directory(p, ec))
        {
            dir = std::filesystem::absolute(p, ec).lexically_normal().string();
        }
    }

    if (dir.empty() && !gamePath.empty() && std::filesystem::is_directory(gamePath, ec))
    {
        dir = std::filesystem::absolute(gamePath, ec).lexically_normal().string();
    }

    if (dir.empty())
    {
        dir = std::filesystem::current_path(ec).string();
    }

    std::snprintf(input, inputSize, "%s", dir.c_str());
}

void MountAdapterGameBaseDirs(EditorAssetSource &assets, const std::filesystem::path &modPath,
                              std::vector<std::filesystem::path> &mounted)
{
    const EditorFormatAdapter *adapter = GetEditorFormatAdapter();
    if (adapter == nullptr)
    {
        return;
    }
    for (const std::string &baseDir : adapter->extraBaseGameDirs)
    {
        MountDirectoryIfNew(assets, modPath.parent_path() / baseDir, mounted);
        MountDirectoryIfNew(assets, modPath.parent_path().parent_path() / baseDir, mounted);
    }
}

} // namespace

void EditorApp::RebuildAssetMounts()
{
    m_assets.Clear();
    m_palette.clear();

    std::error_code ec;

    if (!m_gamePath.empty())
    {
        std::vector<std::filesystem::path> mountedDirs;
        const std::filesystem::path modPath = std::filesystem::path(m_gamePath).lexically_normal();

        MountDirectoryIfNew(m_assets, modPath, mountedDirs);
        MountDirectoryIfNew(m_assets, modPath.parent_path() / "id1", mountedDirs);
        MountDirectoryIfNew(m_assets, modPath.parent_path().parent_path() / "id1", mountedDirs);
        MountAdapterGameBaseDirs(m_assets, modPath, mountedDirs);

        if (!m_initialGamePath.empty())
        {
            MountDirectoryIfNew(m_assets, std::filesystem::path(m_initialGamePath).lexically_normal(),
                                mountedDirs);
        }
    }

    if (!m_scenePath.empty() && (std::filesystem::exists(m_scenePath, ec) || m_gamePath.empty()))
    {
        MountSceneAncestors(m_assets, m_scenePath);
    }

    std::vector<uint8_t> palette;
    const bool hasZipPack = std::any_of(m_assets.Archives().begin(), m_assets.Archives().end(),
                                        [](const std::string &a) { return HasZipPackExtension(a); });

    if (m_assets.ReadFile("gfx/palette.lmp", palette) && palette.size() >= 768)
    {
        m_palette.assign(palette.begin(), palette.begin() + 768);
    }
    else if (!hasZipPack && (!m_gamePath.empty() || !m_scenePath.empty()))
    {
        PrintMessage("palette gfx/palette.lmp not found, using BSP embedded palettes", this);
    }

    m_textureBrowserCatalogBuilt = false;
    RebuildMapCatalog();
    RebuildModelCatalog();
    RefreshDiscoveredMods();
}

void EditorApp::RebuildMapCatalog()
{
    m_mapCatalog.clear();
    m_selectedCatalogMap = -1;

    std::vector<EditorAssetCatalogEntry> entries;

    if (!m_gamePath.empty())
    {
        EditorAssetSource modOnlyAssets;
        MountEditorGameDirectory(modOnlyAssets, m_gamePath);
        modOnlyAssets.ListMapAssets(entries);
    }
    else
    {
        m_assets.ListMapAssets(entries);
    }

    const std::string activeKey = m_scenePath.empty() ? std::string() : LevelKeyFromPath(m_scenePath);

    for (size_t i = 0; i < entries.size(); i++)
    {
        const EditorAssetCatalogEntry &entry = entries[i];
        MapCatalogItem item;
        item.levelKey = LevelKeyFromPath(entry.path);
        item.path = entry.path;
        item.source = entry.source;
        item.isBrushSubmodel = item.levelKey.rfind("b_", 0) == 0;
        m_mapCatalog.push_back(std::move(item));

        if (!activeKey.empty() && m_mapCatalog.back().levelKey == activeKey &&
            m_selectedCatalogMap < 0)
        {
            m_selectedCatalogMap = int32_t(i);
        }
    }

    if (m_selectedCatalogMap < 0 && !m_mapCatalog.empty())
    {
        for (size_t i = 0; i < m_mapCatalog.size(); i++)
        {
            if (!m_mapCatalog[i].isBrushSubmodel)
            {
                m_selectedCatalogMap = int32_t(i);
                break;
            }
        }
        if (m_selectedCatalogMap < 0)
        {
            m_selectedCatalogMap = 0;
        }
    }

    RefreshMapCatalogSavedFlags();
    PrintMessage(("maps: " + std::to_string(m_mapCatalog.size()) + " map asset(s) discovered").c_str(),
                 this);
}

namespace
{

constexpr uint64_t kPreviewModelUniqueId = 0x70000001ULL;
constexpr uint64_t kPreviewLightUniqueId = kEditorPreviewLightUniqueID;
constexpr float kPreviewWorldRadius = 6.5f;

void ComputePreviewOrbitRotation(const EditorCameraBasis &cam, float yaw, float pitch,
                                 float outRotation[3][3])
{
    const float col0[3] = { -cam.forward[0], -cam.forward[1], -cam.forward[2] };
    const float col1[3] = { cam.right[0], cam.right[1], cam.right[2] };
    const float col2[3] = { cam.up[0], cam.up[1], cam.up[2] };

    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);

    const float r[3][3] = {
        { cp * cy, -cp * sy, sp },
        { sy, cy, 0.0f },
        { -sp * cy, sp * sy, cp },
    };

    for (int row = 0; row < 3; row++)
    {
        for (int col = 0; col < 3; col++)
        {
            outRotation[row][col] =
                col0[row] * r[0][col] + col1[row] * r[1][col] + col2[row] * r[2][col];
        }
    }
}

uint32_t ResolvePreviewModelCluster(const EditorScene &scene, const float worldCenter[3],
                                    const float cameraPosition[3])
{
    uint32_t cluster = ResolveEditorLightCluster(scene, worldCenter);
    if (cluster == 0 || cluster == QR_CLUSTER_LIGHT_NO_CLUSTER)
    {
        cluster = ResolveEditorLightCluster(scene, cameraPosition);
    }
    return cluster == QR_CLUSTER_LIGHT_NO_CLUSTER ? 0 : cluster;
}

} // namespace

void EditorApp::RebuildModelCatalog()
{
    m_modelCatalog.clear();
    m_selectedModelCatalog = -1;
    m_assets.ListModelAssets(m_modelCatalog);

    if (!m_previewModelPath.empty())
    {
        const std::string needle = LowercaseName(m_previewModelPath);
        for (size_t i = 0; i < m_modelCatalog.size(); i++)
        {
            if (LowercaseName(m_modelCatalog[i].path) == needle)
            {
                m_selectedModelCatalog = int32_t(i);
                break;
            }
        }
    }

    PrintMessage(("models: " + std::to_string(m_modelCatalog.size()) + " model asset(s) discovered")
                     .c_str(),
                 this);
}

bool EditorApp::IsModelPreviewActive() const
{
    return m_sceneLoaded && m_editorMode == EditorMode::Material && m_materialTab == 1 &&
           m_previewModelLoaded && !m_previewModel.frames.empty() &&
           (!m_previewModel.skins.empty() || m_previewModel.hasMultiSections) && !m_choosingMode;
}

bool EditorApp::IsModelOrbitMode() const
{
    return IsModelPreviewActive() && m_showPanel && !HasModalOpen();
}

void EditorApp::ResetPreviewView()
{
    m_previewYaw = -0.35f;
    m_previewPitch = 0.12f;
    m_previewZoom = 1.0f;
}

int32_t EditorApp::EnsurePreviewSkinTexture(const EditorTexture &skin)
{
    const std::string key = NormalizeAssetKey(skin.name);
    if (key.empty())
    {
        return -1;
    }

    if (const int32_t existing = TextureIndexByName(key); existing >= 0)
    {
        return existing;
    }

    const int32_t newIndex = int32_t(m_scene.textures.size());
    EditorTexture copy = skin;
    copy.isExternal = true;
    m_scene.textures.push_back(std::move(copy));

    QrMaterial material = QR_NO_MATERIAL;
    EditorMaterialEmission emission;
    if (m_instance != nullptr)
    {
        EditorSceneRuntime::CreatePoolMaterial(m_instance, m_scene.textures.back(), material,
                                               emission);
    }

    m_textureMaterials.push_back(material);
    m_textureEmissions.push_back(emission);
    m_textureIndexByName.emplace(key, newIndex);
    return newIndex;
}

int32_t EditorApp::EnsureModelSkinMaterial(const std::string &skinName)
{
    if (const int32_t existing = m_materials.IndexOf(skinName); existing >= 0)
    {
        return existing;
    }

    const int32_t idx = m_materials.Ensure(skinName);
    EditorMaterialParams params;
    if (!m_materialsDocument.ReadMaterial(skinName, params))
    {
        m_baseMaterialsDocument.ReadMaterial(skinName, params);
    }
    m_materials.At(size_t(idx)) = params;

    const int32_t baseIdx = m_baselineMaterials.Ensure(skinName);
    m_baselineMaterials.At(size_t(baseIdx)) = params;
    return idx;
}

int32_t EditorApp::ActivePreviewSkinCount() const
{
    if (m_previewModel.hasMultiSections)
    {
        return int32_t(m_previewModel.skinPresets.size());
    }
    return int32_t(m_previewModel.skins.size());
}

const char *EditorApp::ActivePreviewSkinName(int32_t index) const
{
    if (m_previewModel.hasMultiSections)
    {
        if (index >= 0 && size_t(index) < m_previewModel.skinPresets.size())
        {
            return m_previewModel.skinPresets[size_t(index)].name.c_str();
        }
        return "default";
    }
    if (index >= 0 && size_t(index) < m_previewModel.skins.size())
    {
        return m_previewModel.skins[size_t(index)].name.c_str();
    }
    return "";
}

std::string EditorApp::ActivePreviewSectionMaterialName(
    const EditorPreviewModelSection &section) const
{
    if (m_previewModel.hasMultiSections && !m_previewModel.skinPresets.empty())
    {
        const size_t skinIdx = std::clamp<size_t>(size_t(std::max(0, m_previewModelSkin)), 0,
                                                  m_previewModel.skinPresets.size() - 1);
        const auto &map = m_previewModel.skinPresets[skinIdx].surfaceMaterials;
        const auto it = map.find(LowercaseName(section.surfaceName));
        if (it != map.end() && !it->second.empty())
        {
            return it->second;
        }
    }
    return section.materialName;
}

void EditorApp::SelectPreviewModelSection(int32_t sectionIndex)
{
    if (!m_previewModelLoaded || !m_previewModel.hasMultiSections || m_previewModel.frames.empty())
    {
        m_previewModelSection = 0;
        return;
    }

    const size_t frameIdx = std::clamp<size_t>(size_t(std::max(0, m_previewModelFrame)), 0,
                                               m_previewModel.frames.size() - 1);
    const auto &sections = m_previewModel.frames[frameIdx].sections;
    if (sections.empty())
    {
        m_previewModelSection = 0;
        m_previewMaterialIndex = -1;
        return;
    }

    m_previewModelSection = std::clamp(sectionIndex, 0, int32_t(sections.size()) - 1);
    const std::string matName =
        ActivePreviewSectionMaterialName(sections[size_t(m_previewModelSection)]);
    m_previewMaterialIndex = matName.empty() ? -1 : EnsureModelSkinMaterial(matName);
    m_selectedLumaBlock = 0;
    m_selectedGlossBlock = 0;
    m_selectedSssBlock = 0;
}

void EditorApp::SelectPreviewModelSkin(int32_t skinIndex)
{
    if (!m_previewModelLoaded)
    {
        m_previewModelSkin = 0;
        m_previewMaterialIndex = -1;
        return;
    }

    if (m_previewModel.hasMultiSections)
    {
        const int32_t count = ActivePreviewSkinCount();
        m_previewModelSkin = count > 0 ? std::clamp(skinIndex, 0, count - 1) : 0;
        SelectPreviewModelSection(m_previewModelSection);
        return;
    }

    if (m_previewModel.skins.empty())
    {
        m_previewModelSkin = 0;
        m_previewMaterialIndex = -1;
        return;
    }

    m_previewModelSkin =
        std::clamp(skinIndex, 0, int32_t(m_previewModel.skins.size()) - 1);
    const std::string &skinName = m_previewModel.skins[size_t(m_previewModelSkin)].name;
    m_previewMaterialIndex = EnsureModelSkinMaterial(skinName);
    m_selectedLumaBlock = 0;
    m_selectedGlossBlock = 0;
    m_selectedSssBlock = 0;
}

bool EditorApp::SelectPreviewModel(int32_t catalogIndex)
{
    if (catalogIndex < 0 || size_t(catalogIndex) >= m_modelCatalog.size())
    {
        return false;
    }

    const std::string &modelPath = m_modelCatalog[size_t(catalogIndex)].path;
    EditorPreviewModel loaded;
    std::string error;

    if (!LoadEditorPreviewModel(modelPath, m_assets, m_palette.empty() ? nullptr : m_palette.data(),
                                loaded, error))
    {
        PrintMessage(("preview model failed: " + error).c_str(), this);
        Notify(error);
        return false;
    }

    for (const EditorTexture &skin : loaded.skins)
    {
        EnsurePreviewSkinTexture(skin);
        EnsureModelSkinMaterial(skin.name);
    }
    for (const EditorTexture &tex : loaded.surfaceTextures)
    {
        EnsurePreviewSkinTexture(tex);
        EnsureModelSkinMaterial(tex.name);
    }
    for (const EditorPreviewModelSkinPreset &preset : loaded.skinPresets)
    {
        for (const auto &kv : preset.surfaceMaterials)
        {
            if (!kv.second.empty())
            {
                EnsureModelSkinMaterial(kv.second);
            }
        }
    }

    m_previewModel = std::move(loaded);
    m_previewModelLoaded = true;
    m_selectedModelCatalog = catalogIndex;
    m_previewModelPath = modelPath;
    m_previewModelFrame = 0;
    m_previewModelSection = 0;
    SelectPreviewModelSkin(0);
    ResetPreviewView();

    PrintMessage(("preview model: " + modelPath + " (" +
                  std::to_string(ActivePreviewSkinCount()) + " skins, " +
                  std::to_string(m_previewModel.frames.size()) + " frames)")
                     .c_str(),
                 this);
    return true;
}

void EditorApp::ComputePreviewPose(float outCenter[3], float &outScale,
                                     float outRotation[3][3]) const
{
    const EditorCameraBasis cam = CameraBasis();
    const float clientW = cam.viewportWidth > 1.0f ? cam.viewportWidth : 1.0f;
    const float viewportW = (m_panelVisible && m_panelLeft > 64.0f) ? m_panelLeft : clientW;
    const float ndcX = ((0.5f * viewportW) / clientW) * 2.0f - 1.0f;
    const float zoom = std::clamp(m_previewZoom, 0.35f, 2.5f);
    const float baseDist = kPreviewWorldRadius / (0.48f * std::max(cam.tanHalfFovY, 0.2f));
    const float dist = std::max(kPreviewWorldRadius + 4.5f, baseDist / zoom);

    for (int axis = 0; axis < 3; axis++)
    {
        outCenter[axis] = cam.origin[axis] + cam.forward[axis] * dist +
                          cam.right[axis] * (ndcX * dist * cam.tanHalfFovX);
    }

    const size_t frameIdx =
        std::clamp<size_t>(size_t(std::max(0, m_previewModelFrame)), 0,
                           m_previewModel.frames.size() - 1);
    const float radius = std::max(m_previewModel.frames[frameIdx].radius, 1.0f);
    outScale = kPreviewWorldRadius / radius;
    ComputePreviewOrbitRotation(cam, m_previewYaw, m_previewPitch, outRotation);
}

QrMaterial EditorApp::ResolvePreviewMaterial(const std::string &skinName,
                                             const EditorMaterialParams &params,
                                             bool forceEnvBase)
{
    const auto resolve = [this](const std::string &n) { return ResolveOrLoadTexture(n); };

    const QrMaterial baked = m_previewMaterialBakes.Get(m_instance, skinName, params, m_scene,
                                                        m_textureMaterials, resolve, forceEnvBase);

    if (baked != QR_NO_MATERIAL)
    {
        return baked;
    }

    int32_t index = ResolveOrLoadTexture(params.baseTexture);
    if (index < 0)
    {
        index = TextureIndexByName(skinName);
    }

    if (index >= 0 && size_t(index) < m_textureMaterials.size() &&
        m_textureMaterials[size_t(index)] != QR_NO_MATERIAL)
    {
        return m_textureMaterials[size_t(index)];
    }

    return m_material;
}

namespace
{

void ApplyPreviewPoseTransform(const float rotation[3][3], float scale,
                               const float worldCenter[3], QrGeometryUploadInfo &upload)
{
    for (int row = 0; row < 3; row++)
    {
        for (int col = 0; col < 3; col++)
        {
            upload.transform.matrix[row][col] = rotation[row][col] * scale;
        }
        upload.transform.matrix[row][3] = worldCenter[row];
    }
}

void TransformPreviewVerticesToWorld(const float rotation[3][3], float scale,
                                     const float worldCenter[3], std::vector<QrVertex> &verts)
{
    for (QrVertex &v : verts)
    {
        const float lx = v.position[0];
        const float ly = v.position[1];
        const float lz = v.position[2];
        for (int row = 0; row < 3; row++)
        {
            v.position[row] = worldCenter[row] +
                              scale * (rotation[row][0] * lx + rotation[row][1] * ly +
                                       rotation[row][2] * lz);
        }

        const float nx = v.normal[0];
        const float ny = v.normal[1];
        const float nz = v.normal[2];
        float wn[3] = {};
        for (int row = 0; row < 3; row++)
        {
            wn[row] = rotation[row][0] * nx + rotation[row][1] * ny + rotation[row][2] * nz;
        }
        const float nlen = std::sqrt(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
        if (nlen > 1.0e-6f)
        {
            v.normal[0] = wn[0] / nlen;
            v.normal[1] = wn[1] / nlen;
            v.normal[2] = wn[2] / nlen;
        }
    }
}

struct PreviewSectionUploadPose
{
    uint32_t cluster = 0;
    int32_t skinIndex = 0;
    float scale = 1.0f;
    float rotation[3][3] = {};
    float worldCenter[3] = {};
};

std::string ResolveSectionMaterialForPreset(const EditorPreviewModel &model,
                                            const EditorPreviewModelSection &sec, int32_t skinIndex)
{
    if (model.skinPresets.empty())
    {
        return sec.materialName;
    }
    const size_t clampedSkin =
        std::clamp<size_t>(size_t(std::max(0, skinIndex)), 0, model.skinPresets.size() - 1);
    const auto &map = model.skinPresets[clampedSkin].surfaceMaterials;
    std::string surfLower = sec.surfaceName;
    for (char &ch : surfLower)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    const auto it = map.find(surfLower);
    return (it != map.end() && !it->second.empty()) ? it->second : sec.materialName;
}

struct PreviewSectionUploadContext
{
    QrInstance instance = nullptr;
    const EditorPreviewModel *model = nullptr;
    const PreviewSectionUploadPose *pose = nullptr;
    const EditorCameraBasis *cam = nullptr;
    float timeSeconds = 0.0f;
    const EditorMaterialTable *materials = nullptr;
    QrMaterial cutoutBackingMaterial = QR_NO_MATERIAL;
    const std::function<QrMaterial(const std::string &, const EditorMaterialParams &, bool)>
        *resolveMat = nullptr;
    const std::function<QrMaterial(const std::string &, bool, bool, bool)> *resolveOverlayMat =
        nullptr;
};

struct PreviewSectionState
{
    size_t secIndex = 0;
    std::string matName;
    EditorMaterialParams params;
    bool promoteOverlayToTraced = false;
    bool baseStageDisabled = false;
    bool hasEnabledUnderlay = false;
};

struct PreviewOverlaySliceRange
{
    size_t firstStage = 0;
    size_t endStage = 0;
    float normalOffset = 0.0f;
    bool cutoutUnderlay = false;
};

void UploadPreviewTracedSection(const PreviewSectionUploadContext &ctx,
                                const EditorPreviewModelSection &sec,
                                const PreviewSectionState &state, std::vector<QrVertex> &verts,
                                std::vector<QrVertex> &meterVertices)
{
    const PreviewSectionUploadPose &pose = *ctx.pose;
    const bool isEnvBase =
        sec.baseStageIndex < sec.stages.size() && sec.stages[sec.baseStageIndex].isEnvironment;
    QrGeometryUploadInfo upload = {};
    upload.uniqueID = 0x70000000u + uint64_t(state.secIndex);
    upload.flags = QR_GEOMETRY_UPLOAD_EMISSION_RAY_ONLY_BIT |
                   (sec.hasAuthoredNormals ? 0u : QR_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT);
    upload.geomType = QR_GEOMETRY_TYPE_DYNAMIC;
    upload.visibilityType = QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON;
    upload.vertexCount = uint32_t(verts.size());
    upload.pVertices = verts.data();
    upload.indexCount = uint32_t(sec.indices.size());
    upload.pIndices = sec.indices.data();
    upload.layerColors[0] = { 1.0f, 1.0f, 1.0f, 1.0f };
    upload.layerBlendingTypes[0] = QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
    upload.geomMaterial.layerMaterials[0] = (*ctx.resolveMat)(state.matName, state.params, isEnvBase);
    ApplyEditorMaterial(state.params,
                        sec.alphaTested ? QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED
                                        : QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE,
                        upload);
    ApplyPreviewPoseTransform(pose.rotation, pose.scale, pose.worldCenter, upload);
    EditorMetersGeometry(upload, meterVertices);
    qrUploadGeometry(ctx.instance, &upload);

    if (!state.hasEnabledUnderlay || ctx.cutoutBackingMaterial == QR_NO_MATERIAL)
    {
        return;
    }
    std::vector<QrVertex> backingVerts = verts;
    OffsetEditorVerticesAlongSmoothNormals(backingVerts, sec.indices, -1.6f);
    QrGeometryUploadInfo backingUpload = {};
    backingUpload.uniqueID = 0x71000000u + uint64_t(state.secIndex);
    backingUpload.flags = QR_GEOMETRY_UPLOAD_EMISSION_RAY_ONLY_BIT |
                          (sec.hasAuthoredNormals ? 0u : QR_GEOMETRY_UPLOAD_GENERATE_NORMALS_BIT);
    backingUpload.geomType = QR_GEOMETRY_TYPE_DYNAMIC;
    backingUpload.visibilityType = QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON;
    backingUpload.passThroughType = QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE;
    backingUpload.vertexCount = uint32_t(backingVerts.size());
    backingUpload.pVertices = backingVerts.data();
    backingUpload.indexCount = uint32_t(sec.indices.size());
    backingUpload.pIndices = sec.indices.data();
    backingUpload.layerColors[0] = { 1.0f, 1.0f, 1.0f, 1.0f };
    backingUpload.layerBlendingTypes[0] = QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
    backingUpload.texCoordScale[0] = 1.0f;
    backingUpload.texCoordScale[1] = 1.0f;
    backingUpload.defaultRoughness = 0.9f;
    backingUpload.geomMaterial.layerMaterials[0] = ctx.cutoutBackingMaterial;
    ApplyPreviewPoseTransform(pose.rotation, pose.scale, pose.worldCenter, backingUpload);
    EditorMetersGeometry(backingUpload, meterVertices);
    qrUploadGeometry(ctx.instance, &backingUpload);
}

void UploadPreviewSectionOverlaySlice(const PreviewSectionUploadContext &ctx,
                                      const EditorPreviewModelSection &sec,
                                      const PreviewSectionState &state,
                                      const std::vector<QrVertex> &verts,
                                      const PreviewOverlaySliceRange &slice)
{
    if (slice.firstStage >= slice.endStage)
    {
        return;
    }
    std::vector<QrVertex> overlayVerts = verts;
    std::vector<uint32_t> overlayIndices = sec.indices;
    if (slice.normalOffset != 0.0f)
    {
        OffsetEditorVerticesAlongSmoothNormals(overlayVerts, overlayIndices, slice.normalOffset);
    }
    TransformPreviewVerticesToWorld(ctx.pose->rotation, ctx.pose->scale, ctx.pose->worldCenter,
                                    overlayVerts);
    DeformEditorAutoSpriteVertices(overlayVerts, overlayIndices, sec.autoSprite, sec.autoSprite2,
                                   *ctx.cam);

    EditorOverlayUploadInput input;
    input.cutoutUnderlay = slice.cutoutUnderlay;
    input.tracedBase = sec.tracedBase;
    input.metallicEnvBase = sec.metallicEnvBase;
    input.suppressEnvironmentStages =
        state.params.mirror && state.params.type == EditorMaterialType::None;
    input.promotedOverlayToTraced = state.promoteOverlayToTraced;
    input.surfaceLight = sec.surfaceLight;
    input.colorScale = state.params.baseFactor;
    input.alphaScale =
        (state.params.type == EditorMaterialType::Glass) ? state.params.transparency : 1.0f;
    input.disabledStagesMask = state.params.disabledStagesMask;
    input.baseStageIndex = sec.baseStageIndex;
    input.firstStage = slice.firstStage;
    input.endStage = slice.endStage;
    input.stages = &sec.stages;
    input.vertices = &overlayVerts;
    input.indices = &overlayIndices;
    UploadEditorOverlayStageSet(ctx.instance, input, *ctx.cam, ctx.timeSeconds,
                                *ctx.resolveOverlayMat);
}

bool PreviewSectionHasEnabledUnderlay(const EditorPreviewModelSection &sec,
                                      uint32_t disabledStagesMask)
{
    if (!sec.cutoutFrameUnderlay || sec.baseStageIndex == 0u ||
        sec.baseStageIndex > sec.stages.size())
    {
        return false;
    }
    for (size_t i = 0u; i < sec.baseStageIndex; i++)
    {
        if (!sec.stages[i].isLightmap &&
            (i >= 32u || ((disabledStagesMask >> i) & 1u) == 0u))
        {
            return true;
        }
    }
    return false;
}

void UploadPreviewSectionOverlays(const PreviewSectionUploadContext &ctx,
                                  const EditorPreviewModelSection &sec,
                                  const PreviewSectionState &state,
                                  const std::vector<QrVertex> &verts)
{
    if (!sec.usesOverlay || sec.stages.empty())
    {
        return;
    }
    if (sec.cutoutFrameUnderlay && sec.baseStageIndex > 0u &&
        sec.baseStageIndex <= sec.stages.size())
    {
        const PreviewOverlaySliceRange underlaySlice{
            0u, sec.baseStageIndex, !state.baseStageDisabled ? -0.8f : 0.0f, !state.baseStageDisabled,
        };
        UploadPreviewSectionOverlaySlice(ctx, sec, state, verts, underlaySlice);
        if (sec.baseStageIndex + 1u < sec.stages.size())
        {
            const PreviewOverlaySliceRange upperSlice{
                sec.baseStageIndex + 1u, sec.stages.size(), !state.baseStageDisabled ? 0.25f : 0.0f,
                false,
            };
            UploadPreviewSectionOverlaySlice(ctx, sec, state, verts, upperSlice);
        }
        return;
    }
    const size_t firstStage =
        (sec.tracedBase && sec.baseStageIndex < sec.stages.size()) ? (sec.baseStageIndex + 1u) : 0u;
    const float offset = ((sec.tracedBase || state.promoteOverlayToTraced) && !state.baseStageDisabled)
                             ? 0.25f
                             : 0.0f;
    const PreviewOverlaySliceRange slice{ firstStage, sec.stages.size(), offset, false };
    UploadPreviewSectionOverlaySlice(ctx, sec, state, verts, slice);
}

void UploadOnePreviewModelSection(const PreviewSectionUploadContext &ctx, size_t secIndex,
                                  const EditorPreviewModelSection &sec,
                                  std::vector<QrVertex> &verts, std::vector<QrVertex> &meterVertices)
{
    if (sec.vertices.empty() || sec.indices.empty())
    {
        return;
    }
    const std::string matName = ResolveSectionMaterialForPreset(*ctx.model, sec, ctx.pose->skinIndex);
    if (!matName.empty() && ctx.model->hiddenMaterials.count(LowercaseName(matName)) != 0u)
    {
        return;
    }

    EditorMaterialParams secParams;
    const int32_t matIdx = ctx.materials->IndexOf(matName);
    if (matIdx >= 0 && size_t(matIdx) < ctx.materials->Count())
    {
        secParams = ctx.materials->At(size_t(matIdx));
    }
    if (!secParams.enabled)
    {
        return;
    }

    verts = sec.vertices;
    for (QrVertex &v : verts)
    {
        v.cluster = ctx.pose->cluster;
    }
    const EditorWaveDeformParams deform{
        sec.deformWave, sec.deformSpread, sec.deformBase,
        sec.deformAmplitude, sec.deformPhase, sec.deformFrequency,
    };
    DeformEditorWaveVertices(verts, deform, ctx.timeSeconds);

    const bool baseDisabled = sec.baseStageIndex < sec.stages.size() && sec.baseStageIndex < 32u &&
                              ((secParams.disabledStagesMask >> sec.baseStageIndex) & 1u) != 0u;
    const PreviewSectionState state{
        secIndex,
        matName,
        secParams,
        sec.isNonTracedOverlay && EditorMaterialPromotesOverlayToTraced(secParams),
        baseDisabled,
        PreviewSectionHasEnabledUnderlay(sec, secParams.disabledStagesMask),
    };

    if ((!sec.isNonTracedOverlay || state.promoteOverlayToTraced) && !state.baseStageDisabled)
    {
        UploadPreviewTracedSection(ctx, sec, state, verts, meterVertices);
    }
    UploadPreviewSectionOverlays(ctx, sec, state, verts);
}

void UploadPreviewModelSections(const PreviewSectionUploadContext &ctx,
                                const EditorPreviewModelFrame &frame)
{
    std::vector<QrVertex> verts;
    std::vector<QrVertex> meterVertices;
    for (size_t s = 0; s < frame.sections.size(); s++)
    {
        UploadOnePreviewModelSection(ctx, s, frame.sections[s], verts, meterVertices);
    }
}

} // namespace

QrMaterial EditorApp::GetCutoutBackingMaterial()
{
    return m_runtime.overlayCache.GetCutoutBackingMaterial(m_instance);
}

QrMaterial EditorApp::GetOverlayStageMaterial(const std::string &imageName, bool clampMap,
                                              bool cleanAdditiveBlack, bool addScreenEmission)
{
    const std::vector<const std::vector<EditorTexture> *> extraLists = {
        &m_previewModel.surfaceTextures,
        &m_previewModel.skins,
    };
    const EditorOverlayMaterialRequest request{
        imageName, clampMap, cleanAdditiveBlack, addScreenEmission, &extraLists,
    };
    return m_runtime.overlayCache.GetOverlayStageMaterial(m_instance, m_assets, m_scene, request);
}

void EditorApp::DestroyOverlayMaterials()
{
    m_runtime.DestroyOverlayMaterials(m_instance);
}

void EditorApp::UploadOverlayGeometry(float timeSeconds)
{
    if (m_instance == nullptr || m_scene.overlaySurfaces.empty())
    {
        return;
    }

    const std::vector<const std::vector<EditorTexture> *> extraLists = {
        &m_previewModel.surfaceTextures,
        &m_previewModel.skins,
    };
    const EditorCameraBasis cam = CameraBasis();
    m_runtime.UploadWorldOverlays(m_instance, m_assets, cam, timeSeconds, &extraLists);
}

void EditorApp::UploadPreviewModel(float timeSeconds)
{
    if (!IsModelPreviewActive() || m_instance == nullptr)
    {
        return;
    }

    const size_t frameIdx =
        std::clamp<size_t>(size_t(std::max(0, m_previewModelFrame)), 0,
                           m_previewModel.frames.size() - 1);
    const EditorPreviewModelFrame &frame = m_previewModel.frames[frameIdx];

    float worldCenter[3] = {};
    float scale = 1.0f;
    float rotation[3][3] = {};
    ComputePreviewPose(worldCenter, scale, rotation);
    const uint32_t cluster = ResolvePreviewModelCluster(m_scene, worldCenter, m_cameraPosition);

    m_previewMaterialBakes.BeginPass();
    std::vector<QrVertex> verts;
    std::vector<QrVertex> meterVertices;

    if (m_previewModel.hasMultiSections && !frame.sections.empty())
    {
        PreviewSectionUploadPose pose{ cluster, m_previewModelSkin, scale, {}, {} };
        std::memcpy(pose.rotation, rotation, sizeof(pose.rotation));
        std::memcpy(pose.worldCenter, worldCenter, sizeof(pose.worldCenter));
        const EditorCameraBasis cam = CameraBasis();
        const QrMaterial cutoutBacking = GetCutoutBackingMaterial();
        const std::function<QrMaterial(const std::string &, const EditorMaterialParams &, bool)>
            resolveMat = [this](const std::string &name, const EditorMaterialParams &p,
                                bool forceEnvBase) {
                return ResolvePreviewMaterial(name, p, forceEnvBase);
            };
        const std::function<QrMaterial(const std::string &, bool, bool, bool)> resolveOverlayMat =
            [this](const std::string &imageName, bool clampMap, bool cleanAdditiveBlack,
                   bool addScreenEmission) {
                return GetOverlayStageMaterial(imageName, clampMap, cleanAdditiveBlack,
                                               addScreenEmission);
            };
        const PreviewSectionUploadContext secCtx{
            m_instance, &m_previewModel, &pose, &cam, timeSeconds,
            &m_materials, cutoutBacking, &resolveMat, &resolveOverlayMat,
        };
        UploadPreviewModelSections(secCtx, frame);
        m_previewMaterialBakes.EndPass(m_instance);
        return;
    }

    if (m_previewModel.skins.empty() || frame.vertices.empty() || frame.indices.empty())
    {
        m_previewMaterialBakes.EndPass(m_instance);
        return;
    }

    const size_t skinIdx =
        std::clamp<size_t>(size_t(std::max(0, m_previewModelSkin)), 0,
                           m_previewModel.skins.size() - 1);
    const std::string &skinName = m_previewModel.skins[skinIdx].name;
    EditorMaterialParams params;
    if (m_previewMaterialIndex >= 0 && size_t(m_previewMaterialIndex) < m_materials.Count())
    {
        params = m_materials.At(size_t(m_previewMaterialIndex));
    }

    verts = frame.vertices;
    for (QrVertex &v : verts)
    {
        v.cluster = cluster;
    }

    QrGeometryUploadInfo upload = {};
    upload.uniqueID = kPreviewModelUniqueId;
    upload.flags = QR_GEOMETRY_UPLOAD_EMISSION_RAY_ONLY_BIT;
    upload.geomType = QR_GEOMETRY_TYPE_DYNAMIC;
    upload.visibilityType = QR_GEOMETRY_VISIBILITY_TYPE_FIRST_PERSON;
    upload.vertexCount = uint32_t(verts.size());
    upload.pVertices = verts.data();
    upload.indexCount = uint32_t(frame.indices.size());
    upload.pIndices = frame.indices.data();
    upload.layerColors[0] = { 1.0f, 1.0f, 1.0f, 1.0f };
    upload.layerBlendingTypes[0] = QR_GEOMETRY_MATERIAL_BLEND_TYPE_OPAQUE;
    upload.geomMaterial.layerMaterials[0] = ResolvePreviewMaterial(skinName, params);

    const QrGeometryPassThroughType passThrough =
        m_previewModel.holey ? QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED
                             : QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE;
    ApplyEditorMaterial(params, passThrough, upload);
    ApplyPreviewPoseTransform(rotation, scale, worldCenter, upload);

    EditorMetersGeometry(upload, meterVertices);
    const QrResult result = qrUploadGeometry(m_instance, &upload);
    m_previewMaterialBakes.EndPass(m_instance);

    if (result != QR_SUCCESS)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            PrintMessage(("preview model upload: " + std::string(qrGetResultDescription(result)))
                             .c_str(),
                         this);
        }
    }
}

void EditorApp::BuildPreviewLight(EditorLight &outLight) const
{
    float worldCenter[3] = {};
    float scale = 1.0f;
    float rotation[3][3] = {};
    ComputePreviewPose(worldCenter, scale, rotation);
    (void)scale;
    (void)rotation;

    const EditorCameraBasis cam = CameraBasis();
    const float dx = worldCenter[0] - cam.origin[0];
    const float dy = worldCenter[1] - cam.origin[1];
    const float dz = worldCenter[2] - cam.origin[2];
    const float toCamera = std::max(8.0f, std::sqrt(dx * dx + dy * dy + dz * dz) * 0.55f);

    outLight = EditorLight{};
    outLight.position[0] = worldCenter[0] - cam.forward[0] * toCamera + cam.right[0] * 6.0f +
                           cam.up[0] * 5.0f;
    outLight.position[1] = worldCenter[1] - cam.forward[1] * toCamera + cam.right[1] * 6.0f +
                           cam.up[1] * 5.0f;
    outLight.position[2] = worldCenter[2] - cam.forward[2] * toCamera + cam.right[2] * 6.0f +
                           cam.up[2] * 5.0f;
    outLight.radius = 4.0f;
    outLight.cluster = ResolvePreviewModelCluster(m_scene, worldCenter, m_cameraPosition);
    const float fillColor[3] = { 1.0f, 0.98f, 0.95f };
    // Match preview model distance (~0.33 m at default zoom) so irradiance stays comparable
    // to normal scene lighting (~6 W/m^2) rather than overexposing at close range.
    const float lightDistSqMeters =
        (toCamera * toCamera + 6.0f * 6.0f + 5.0f * 5.0f + outLight.radius * outLight.radius) /
        (kEditorUnitsPerMeter * kEditorUnitsPerMeter);
    const float intensity = std::clamp(6.0f * lightDistSqMeters / 0.1f, 4.0f, 250.0f);
    SetEditorLightTint(outLight, fillColor);
    SetEditorLightIntensity(outLight, intensity);
}

void EditorApp::UploadPreviewLight(QrResult &lastResult)
{
    EditorLight light;
    BuildPreviewLight(light);
    UploadLight(light, kPreviewLightUniqueId, lastResult);
}

void EditorApp::RefreshMapCatalogSavedFlags()
{
    const EditorSessionPaths paths = BuildEffectiveSessionPaths(
        m_materialsOverride, m_lightsOverride, m_gamePath, m_scenePath);

    EditorLevelDocument tempDoc;
    const EditorLevelDocument *doc = &m_levelDocument;

    if (!m_sceneLoaded && !paths.lights.empty())
    {
        std::string ignoreError;
        tempDoc.Load(paths.lights, ignoreError);
        doc = &tempDoc;
    }

    for (MapCatalogItem &item : m_mapCatalog)
    {
        item.hasSavedSettings = doc->ReadLevel(item.levelKey).present;
    }
}

void EditorApp::RefreshDiscoveredMods()
{
    m_discoveredMods.clear();
    std::error_code ec;

    if (!m_gamePath.empty())
    {
        const std::filesystem::path modRoot =
            std::filesystem::path(m_gamePath).lexically_normal().parent_path();
        CollectModsInRoot(modRoot, m_discoveredMods);
    }

    if (!m_modBrowserDir.empty())
    {
        const std::filesystem::path browserPath(m_modBrowserDir);
        CollectModsInRoot(browserPath, m_discoveredMods);
        CollectModsInRoot(browserPath.parent_path(), m_discoveredMods);
    }

    if (!m_scenePath.empty() && std::filesystem::exists(m_scenePath, ec))
    {
        const std::filesystem::path parent =
            std::filesystem::path(m_scenePath).lexically_normal().parent_path();
        CollectModsInRoot(parent, m_discoveredMods);
        CollectModsInRoot(parent.parent_path(), m_discoveredMods);
    }

    std::sort(m_discoveredMods.begin(), m_discoveredMods.end(),
              [](const std::string &a, const std::string &b) {
                  return LowercaseName(a) < LowercaseName(b);
              });
}

void EditorApp::UnloadSceneResources()
{
    FinishGizmoDrag(false);
    FinishLightGrab(false);
    CancelMarqueeSelection();
    ClearSelection();
    m_hoverSurface = -1;
    m_hoverFace = -1;
    m_hoverLight = -1;
    m_hoverCustomLight = -1;
    m_hoverGizmoPart = GizmoPart::None;
    m_hoverGizmoAxis = -1;
    m_customPlacing = false;
    m_customCloning = -1;
    m_placingSun = false;
    m_torchMode = false;
    m_textureBrowserMaterial.clear();
    m_textureBrowserSelection = -1;
    m_textureBrowserRequested = false;
    ShutdownAiTextureGeneratorDialog();
    m_materialsChanged = false;
    m_previewModelDragging = false;
    m_previewModelLoaded = false;
    m_previewMaterialIndex = -1;
    m_previewModel = EditorPreviewModel{};

    m_previewMaterialBakes.Destroy(m_instance);
    m_runtime.DestroyGpuResources(m_instance);
    m_textureBrowserCatalogBuilt = false;
    m_hasLightBuildSettings = false;
    m_selectedMaterial = -1;
}

bool EditorApp::OpenSceneNow(const std::string &path)
{
    if (path.empty())
    {
        return false;
    }

    const std::string previousScenePath = m_scenePath;
    m_scenePath = path;
    RebuildAssetMounts();

    EditorScene nextScene;
    std::string error;

    if (!LoadNextEditorScene(m_scenePath, m_assets, m_multiplayerMode,
                             m_palette.empty() ? nullptr : m_palette.data(), nextScene, error))
    {
        m_scenePath = previousScenePath;
        RebuildAssetMounts();
        PrintMessage(("scene load failed: " + error).c_str(), this);

        if (m_running)
        {
            Notify(error);
        }

        if (!m_sceneLoaded && previousScenePath.empty())
        {
            EditorShowError(&m_window, "QuakeRay Editor", error.c_str());
        }

        return false;
    }

    UnloadSceneResources();
    m_scene = std::move(nextScene);
    m_framesSinceSceneLoad = 0;
    m_lightStatsLogged = false;
    m_sessionPaths = BuildEffectiveSessionPaths(m_materialsOverride, m_lightsOverride, m_gamePath,
                                                 m_scenePath);

    PrintMessage(("scene loaded: " + m_scenePath + " (" + std::to_string(m_scene.lights.size()) +
                  " lights)")
                     .c_str(),
                 this);

    m_materials.Rebuild(m_scene);
    m_selectedMaterial = m_materials.Empty() ? -1 : 0;

    m_sun = EditorSunState{};
    m_settings.skyType = EditorRenderSettings{}.skyType;
    m_settings.volumeType = EditorRenderSettings{}.volumeType;
    m_settings.lightShafts = EditorRenderSettings{}.lightShafts;
    m_settings.godRays = EditorRenderSettings{}.godRays;

    if (!m_skyGlobals.Empty())
    {
        ApplyEditorSkyScope(m_skyGlobals, false, m_settings, m_sun);
    }

    m_settings.fogColor[0] = 1.0f;
    m_settings.fogColor[1] = 1.0f;
    m_settings.fogColor[2] = 1.0f;
    m_settings.fogDensity = 0.0f;

    m_runtime.ApplySceneRenderDefaults();

    LoadMaterialsSession();
    LoadGameConfigGlobals();
    LoadLightsSession();

    if (m_instance != nullptr && (!UploadScene() || !UploadWorldClusters()))
    {
        UnloadSceneResources();
        m_sceneLoaded = false;
        return false;
    }

    m_sceneLoaded = true;
    m_openLevelDialog = false;
    m_openModDialog = false;
    ResetCameraToScene();
    ApplyBaselineCapture();
    RefreshMapCatalogSavedFlags();

    if (m_renderedFrames > 0 && m_maxFrames == 0)
    {
        m_choosingMode = false;
        SetCursorMode(true);
        Notify("loaded " + m_sessionPaths.level);
    }

    return true;
}

bool EditorApp::LoadScene()
{
    return OpenSceneNow(m_scenePath);
}

void EditorApp::CloseSceneNow()
{
    UnloadSceneResources();
    m_sceneLoaded = false;
    m_scenePath.clear();
    m_sessionPaths = BuildEffectiveSessionPaths(m_materialsOverride, m_lightsOverride, m_gamePath,
                                                 m_scenePath);
    m_touchedMaterials.clear();
    m_levelTouched = false;
    m_sessionDirty = false;
    m_levelFingerprint.clear();
    m_choosingMode = false;
    m_panelVisible = false;
    m_selectedCatalogMap = -1;
    SetCursorMode(false);
    ApplyBaselineCapture();
    PrintMessage("scene closed", this);
    Notify("level closed");
}

bool EditorApp::ReloadSceneNow()
{
    if (m_scenePath.empty())
    {
        return false;
    }

    const std::string target = m_scenePath;
    return OpenSceneNow(target);
}

namespace
{

std::string SelectDefaultModMap(const std::vector<EditorApp::MapCatalogItem> &catalog)
{
    for (const EditorApp::MapCatalogItem &item : catalog)
    {
        if (item.levelKey == "start")
        {
            return item.path;
        }
    }

    for (const EditorApp::MapCatalogItem &item : catalog)
    {
        if (!item.isBrushSubmodel)
        {
            return item.path;
        }
    }

    return catalog.empty() ? std::string{} : catalog.front().path;
}

} // namespace

bool EditorApp::OpenModNow(const std::string &modPath, bool useModYamlPaths)
{
    std::error_code ec;

    if (modPath.empty() || !std::filesystem::is_directory(modPath, ec))
    {
        Notify("invalid mod directory");
        return false;
    }

    CloseSceneNow();

    std::filesystem::path normalized = std::filesystem::path(modPath).lexically_normal();
    m_gamePath = normalized.string();

    while (m_gamePath.size() > 1 && (m_gamePath.back() == '/' || m_gamePath.back() == '\\'))
    {
        m_gamePath.pop_back();
    }

    if (useModYamlPaths)
    {
        m_materialsOverride = (std::filesystem::path(m_gamePath) / "qray.materials.yaml").string();
        m_lightsOverride = (std::filesystem::path(m_gamePath) / "qray.lights.yaml").string();
    }

    m_gameConfigApplied = false;
    m_skyGlobals = EditorLevelSky{};
    m_skyLevel = EditorLevelSky{};
    m_skyGlobal = true;
    m_settings = EditorRenderSettings{};
    m_sun = EditorSunState{};

    m_openLevelSearch[0] = '\0';
    m_selectedBrowserFile.clear();
    m_selectedModSubdir.clear();
    m_fileBrowserDir = m_gamePath;
    std::snprintf(m_fileBrowserPathInput, sizeof(m_fileBrowserPathInput), "%s",
                  m_fileBrowserDir.c_str());

    RebuildAssetMounts();
    LoadGameConfigGlobals();
    m_sessionPaths = BuildEffectiveSessionPaths(m_materialsOverride, m_lightsOverride, m_gamePath,
                                                 m_scenePath);
    ApplyBaselineCapture();

    const std::string modName = std::filesystem::path(m_gamePath).filename().string();
    PrintMessage(("mod loaded: " + m_gamePath + " (" + std::to_string(m_mapCatalog.size()) +
                  " maps)")
                     .c_str(),
                 this);
    m_openModDialog = false;

    const std::string defaultMapPath = SelectDefaultModMap(m_mapCatalog);

    if (!defaultMapPath.empty())
    {
        OpenSceneNow(defaultMapPath);
        if (m_maxFrames == 0)
        {
            OpenLevelBrowserDialog();
        }
    }
    else
    {
        Notify("mod has no map archives or maps: " + modName);
    }

    return true;
}

void EditorApp::UnloadModNow()
{
    CloseSceneNow();
    m_gamePath.clear();
    m_selectedModSubdir.clear();
    m_gameConfigApplied = false;
    m_skyGlobals = EditorLevelSky{};
    m_skyLevel = EditorLevelSky{};
    m_skyGlobal = true;
    m_settings = EditorRenderSettings{};
    m_sun = EditorSunState{};
    RebuildAssetMounts();
    m_sessionPaths = BuildEffectiveSessionPaths(m_materialsOverride, m_lightsOverride, m_gamePath,
                                                 m_scenePath);
    ApplyBaselineCapture();
    PrintMessage("mod unloaded", this);
    Notify("mod unloaded");
}

void EditorApp::OpenLevelBrowserDialog()
{
    m_openLevelDialog = true;
    m_openModDialog = false;
    InitBrowserDirectory(m_fileBrowserDir, m_fileBrowserPathInput, sizeof(m_fileBrowserPathInput),
                         m_scenePath, m_gamePath);
    m_openLevelTab = m_mapCatalog.empty() ? 1 : 0;

    if (m_looking)
    {
        m_looking = false;
        m_window.SetMouseCapture(false);
        m_ui.SetCursorVisible(true);
    }
}

void EditorApp::OpenModBrowserDialog()
{
    m_openModDialog = true;
    m_openLevelDialog = false;
    m_selectedModSubdir.clear();
    InitBrowserDirectory(m_modBrowserDir, m_modBrowserPathInput, sizeof(m_modBrowserPathInput),
                         m_scenePath, m_gamePath);

    if (!m_gamePath.empty())
    {
        std::error_code ec;
        const std::filesystem::path parent =
            std::filesystem::path(m_gamePath).lexically_normal().parent_path();

        if (!parent.empty() && std::filesystem::is_directory(parent, ec))
        {
            m_modBrowserDir = parent.string();
            std::snprintf(m_modBrowserPathInput, sizeof(m_modBrowserPathInput), "%s",
                          m_modBrowserDir.c_str());
        }

        m_selectedModSubdir = m_gamePath;
    }

    RefreshDiscoveredMods();

    if (m_looking)
    {
        m_looking = false;
        m_window.SetMouseCapture(false);
        m_ui.SetCursorVisible(true);
    }
}

bool EditorApp::UploadScene()
{
    if (m_instance == nullptr)
    {
        return false;
    }

    if (!CreateTextureMaterials())
    {
        return false;
    }

    return UploadStaticGeometry();
}

namespace
{
struct SavedLightToken
{
    int32_t oldIndex = -1;
    uint64_t uid = 0;
    std::string className;
};

std::vector<SavedLightToken> CaptureLightSelectionTokens(const std::vector<EditorLight> &lights,
                                                         const std::vector<int32_t> &selectedLights,
                                                         int32_t selectedLight)
{
    std::vector<SavedLightToken> savedSel;
    auto pushSaved = [&](int32_t idx) {
        if (idx >= 0 && size_t(idx) < lights.size())
        {
            savedSel.push_back(
                { idx, lights[size_t(idx)].emitterUniqueID, lights[size_t(idx)].className });
        }
    };
    for (int32_t idx : selectedLights)
    {
        pushSaved(idx);
    }
    if (savedSel.empty() && selectedLight >= 0)
    {
        pushSaved(selectedLight);
    }
    return savedSel;
}

int32_t MatchSavedLightTokenIndex(const std::vector<EditorLight> &lights,
                                  const SavedLightToken &token)
{
    if (token.uid != 0)
    {
        for (size_t i = 0; i < lights.size(); i++)
        {
            if (lights[i].emitterUniqueID == token.uid && lights[i].className == token.className)
            {
                return int32_t(i);
            }
        }
        return -1;
    }
    if (token.oldIndex >= 0 && size_t(token.oldIndex) < lights.size())
    {
        return token.oldIndex;
    }
    return -1;
}
} // namespace

bool EditorApp::UploadStaticGeometry()
{
    if (m_instance == nullptr)
    {
        return false;
    }

    const std::vector<SavedLightToken> savedSel =
        CaptureLightSelectionTokens(m_scene.lights, m_selectedLights, m_selectedLight);
    const bool ok = m_runtime.UploadStaticGeometry(
        m_instance, m_assets, m_material, [this](const char *msg) { PrintMessage(msg, this); });
    if (!ok)
    {
        return false;
    }

    m_lastLightBuildSettings = m_settings;
    m_hasLightBuildSettings = true;

    m_selectedLights.clear();
    m_selectedLight = -1;
    for (const SavedLightToken &s : savedSel)
    {
        const int32_t matched = MatchSavedLightTokenIndex(m_scene.lights, s);
        if (matched >= 0 &&
            std::find(m_selectedLights.begin(), m_selectedLights.end(), matched) ==
                m_selectedLights.end())
        {
            m_selectedLights.push_back(matched);
        }
    }
    SyncPrimaryLightSelection();
    return true;
}

bool EditorApp::UploadWorldClusters()
{
    return m_runtime.UploadWorldClusters(
        m_instance, [this](const char *msg) { PrintMessage(msg, this); });
}

void EditorApp::AdvanceCelestialClock(float deltaSeconds)
{
    m_runtime.AdvanceCelestialClock(deltaSeconds);
}

void EditorApp::RenderSceneFrameContents(float deltaSeconds)
{
    if (m_sceneLoaded)
    {
        AdvanceCelestialClock(deltaSeconds);
        if (IsModelPreviewActive() && m_previewAutoRotate && !m_previewModelDragging)
        {
            m_previewYaw += deltaSeconds * 0.65f;
        }
        UpdateLightGrab();
        UploadSun();
        UploadLights();
        UploadEmissiveLights();
        UploadClusterLightSources();
    }

    m_ui.BeginFrame(deltaSeconds, m_clientWidth, m_clientHeight);
    const bool wasPanelOpen = m_showPanel;
    const bool wasModalOpen = HasModalOpen();
    DrawEditorOverlay(deltaSeconds);

    if (m_sceneLoaded && m_maxFrames == 0 && !HasModalOpen() && !m_showPanel &&
        (wasPanelOpen || wasModalOpen))
    {
        SetCursorMode(false);
    }

    if (m_sceneLoaded)
    {
        UpdateHoverPick();
        DrawSelectionOverlay();
        UpdateCamera(deltaSeconds);
    }

    DrawSessionPrompt();
    DrawResetPrompt();
    DrawRegenPrompt();
    DrawNotification();

    if (m_sceneLoaded)
    {
        DrawViewportOverlay();
    }

    m_ui.EndFrame(m_instance, m_clientWidth, m_clientHeight);
}

void EditorApp::FlushPendingSceneUpdates()
{
    if (!m_sceneLoaded || ImGui::IsAnyItemActive())
    {
        return;
    }

    if (m_materialsChanged)
    {
        m_materialsChanged = false;
        if (UploadStaticGeometry())
        {
            PrintMessage("materials applied: static scene re-uploaded", this);
        }
    }

    RefreshGeneratedLightsIfSettingsChanged();
}

void EditorApp::MaintainFlyLookCapture()
{
    if (!m_sceneLoaded || !m_looking || !m_hasFocus || m_showPanel || HasModalOpen() ||
        m_maxFrames != 0 || m_window.IsMouseCaptured())
    {
        return;
    }

    m_ui.SetCursorVisible(false);
    m_window.SetMouseCapture(true);
    if (m_window.IsMouseCaptured())
    {
        m_window.WarpCursorToCenter(m_lastMouseX, m_lastMouseY);
    }
}

void EditorApp::Frame()
{
    if (m_instance == nullptr)
    {
        return;
    }

    ProcessDeferredSessionAction();

    if (!m_running || m_playProcess.handle != 0)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    float deltaSeconds = std::chrono::duration<float>(now - m_lastFrameTime).count();
    m_lastFrameTime = now;

    if (!(deltaSeconds > 0.0f) || deltaSeconds > 0.25f)
    {
        deltaSeconds = 1.0f / 60.0f;
    }

    m_frameTimeMs = deltaSeconds * 1000.0f;

    FlushPendingSceneUpdates();
    MaintainFlyLookCapture();

    m_window.GetClientSize(m_clientWidth, m_clientHeight);

    if (m_clientWidth == 0 || m_clientHeight == 0)
    {
        return;
    }

    QrStartFrameInfo startInfo = {
        .presentMode = QR_PRESENT_MODE_VSYNC,
        .maxFrameLatency = 1,
        .requestShaderReload = 0,
        .wantFullscreenExclusive = 0,
        .hasFocus = 1,
    };

    if (qrStartFrame(m_instance, &startInfo) != QR_SUCCESS)
    {
        return;
    }

    RenderSceneFrameContents(deltaSeconds);
    SubmitSceneFrame();
}

void EditorApp::SubmitSceneFrame()
{
    const float cosPitch = std::cos(m_cameraPitch);
    const float eye[3] = { m_cameraPosition[0], m_cameraPosition[1], m_cameraPosition[2] };
    const float forward[3] = {
        cosPitch * std::cos(m_cameraYaw),
        cosPitch * std::sin(m_cameraYaw),
        std::sin(m_cameraPitch),
    };

    float target[3] = {
        eye[0] + forward[0],
        eye[1] + forward[1],
        eye[2] + forward[2],
    };

    const float up[3] = { 0.0f, 0.0f, 1.0f };
    float sceneExtent = 10.0f;

    if (m_scene.hasBounds)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            sceneExtent = std::max(sceneExtent,
                                   m_scene.boundsMax[axis] - m_scene.boundsMin[axis]);
        }
    }

    QrDrawFrameInfo draw = {};
    LookAt(eye, target, up, draw.view);
    draw.worldUpVector = { 0.0f, 0.0f, 1.0f };
    draw.fovYRadians = FovYRadians();
    draw.cameraNear = std::max(0.1f, sceneExtent * 0.0005f);
    draw.cameraFar = std::max(1000.0f, sceneExtent * 8.0f);
    draw.rayLength = draw.cameraFar;
    draw.rayCullMaskWorld = QR_DRAW_FRAME_RAY_CULL_WORLD_0_BIT |
                            QR_DRAW_FRAME_RAY_CULL_WORLD_1_BIT |
                            QR_DRAW_FRAME_RAY_CULL_SKY_BIT;
    draw.currentTime = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_startTime).count();
    draw.renderUiOnly = m_sceneLoaded ? 0 : 1;
    draw.enableCpuProfiling = 0;

    m_runtime.PopulateDrawFrame(m_instance, eye, m_clientWidth, m_clientHeight, m_frameParams, draw);

    if (m_sceneLoaded)
    {
        UploadScrollingSky(draw.currentTime, draw.cameraFar);
        UploadOverlayGeometry(float(draw.currentTime));
        UploadPreviewModel(float(draw.currentTime));
    }

    TryRequestScreenshot();

    const QrResult drawResult = EditorDrawFrame(m_instance, &draw);

    if (drawResult != QR_SUCCESS)
    {
        static bool loggedDrawFailure = false;

        if (!loggedDrawFailure)
        {
            loggedDrawFailure = true;
            PrintMessage(("draw: " + std::string(qrGetResultDescription(drawResult))).c_str(), this);
        }
    }

    AdvanceRenderedFrameCount();

    if (m_sceneLoaded && !m_lightStatsLogged && ++m_framesSinceSceneLoad == 120)
    {
        m_lightStatsLogged = true;

        QrClusterLightStats clusterStats = {};

        if (qrGetClusterLightStats(m_instance, &clusterStats) == QR_SUCCESS)
        {
            char message[320];
            std::snprintf(message, sizeof(message),
                          "cluster lights: %u sources, %u grants, %u entries, %u denied, "
                          "%u clusters, %u full, %u tail, %u tailDropped, %u candidates max",
                          clusterStats.sources, clusterStats.grants, clusterStats.listEntries,
                          clusterStats.denied, clusterStats.clusters, clusterStats.fullClusters,
                          clusterStats.tailEntries, clusterStats.tailCandidatesDropped,
                          clusterStats.candidateMax);
            PrintMessage(message, this);
        }
    }
}

bool EditorApp::HasModalOpen() const
{
    return !m_sceneLoaded || m_choosingMode || m_openLevelDialog || m_openModDialog ||
           !m_textureBrowserMaterial.empty() || IsAiTextureDialogOpen() ||
           m_sessionPrompt != 0 || m_resetPrompt != 0 || m_regenPrompt != 0;
}

EditorCameraBasis EditorApp::CameraBasis() const
{
    EditorCameraBasis cam;
    std::copy_n(m_cameraPosition, 3, cam.origin);
    ViewBasis(cam.right, cam.up, cam.forward);
    cam.viewportWidth = float(m_clientWidth);
    cam.viewportHeight = float(m_clientHeight);
    const float aspect = cam.viewportHeight > 0.0f ? cam.viewportWidth / cam.viewportHeight : 1.0f;
    cam.tanHalfFovY = std::tan(0.5f * FovYRadians());
    cam.tanHalfFovX = cam.tanHalfFovY * aspect;
    return cam;
}

void EditorApp::SetCursorMode(bool panelOpen)
{
    m_previewModelDragging = false;
    CancelMarqueeSelection();

    if (!m_sceneLoaded)
    {
        m_showPanel = false;
        m_looking = false;
        m_panelVisible = false;
        std::fill(std::begin(m_keys), std::end(m_keys), false);
        m_window.SetMouseCapture(false);
        m_ui.SetCursorVisible(true);
        return;
    }

    m_showPanel = panelOpen;
    m_looking = !panelOpen && m_maxFrames == 0 && m_hasFocus && !HasModalOpen();
    std::fill(std::begin(m_keys), std::end(m_keys), false);

    if (panelOpen)
    {
        FinishGizmoDrag(false);
        FinishLightGrab(false);
        m_hoverSurface = -1;
        m_hoverFace = -1;
        m_hoverLight = -1;
        m_hoverCustomLight = -1;
        m_hoverGizmoPart = GizmoPart::None;
        m_hoverGizmoAxis = -1;
        m_window.SetMouseCapture(false);
        m_ui.SetCursorVisible(true);
        return;
    }

    if (m_looking)
    {
        m_ui.SetCursorVisible(false);
        m_window.SetMouseCapture(true);
        m_window.WarpCursorToCenter(m_lastMouseX, m_lastMouseY);
    }
    else
    {
        m_window.SetMouseCapture(false);
        m_ui.SetCursorVisible(true);
    }
}

void EditorApp::HandleMouseMoveEvent(const EditorEvent &event)
{
    const int32_t x = int32_t(std::lround(event.x));
    const int32_t y = int32_t(std::lround(event.y));

    if (m_looking)
    {
        if (x == m_lastMouseX && y == m_lastMouseY)
        {
            return;
        }

        const float dx = float(x - m_lastMouseX);
        const float dy = float(y - m_lastMouseY);
        m_lastMouseX = x;
        m_lastMouseY = y;

        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        const float sens = m_settings.mouseSensitivity;
        m_cameraYaw -= dx * sens * m_settings.mouseYaw * kDegToRad;
        m_cameraPitch = std::clamp(
            m_cameraPitch - dy * sens * m_settings.mousePitch * kDegToRad, -1.55f, 1.55f);

        const int32_t cx = int32_t(m_clientWidth / 2);
        const int32_t cy = int32_t(m_clientHeight / 2);

        if (std::abs(x - cx) > int32_t(m_clientWidth / 4) ||
            std::abs(y - cy) > int32_t(m_clientHeight / 4))
        {
            m_window.WarpCursorToCenter(m_lastMouseX, m_lastMouseY);
        }

        return;
    }

    if (!m_looking && m_previewModelDragging)
    {
        UpdatePreviewOrbitDrag(x, y);
        return;
    }

    m_lastMouseX = x;
    m_lastMouseY = y;

    if (m_marqueePending)
    {
        m_marqueeEnd[0] = event.x;
        m_marqueeEnd[1] = event.y;
        if (IsMarqueeDragPastThreshold(m_marqueeStart[0], m_marqueeStart[1], m_marqueeEnd[0],
                                       m_marqueeEnd[1]))
        {
            m_marqueeDragging = true;
        }
        return;
    }

    UpdateGizmoDrag(event.x, event.y);
}

void EditorApp::UpdatePreviewOrbitDrag(int32_t x, int32_t y)
{
    const float dx = float(x - m_lastMouseX);
    const float dy = float(y - m_lastMouseY);
    m_lastMouseX = x;
    m_lastMouseY = y;

    m_previewYaw += dx * 0.008f;
    m_previewPitch = std::clamp(m_previewPitch + dy * 0.008f, -1.45f, 1.45f);
}

void EditorApp::HandleFlyMouseButton(const EditorEvent &event)
{
    if (!m_looking && event.down)
    {
        SetCursorMode(false);
        return;
    }

    if (event.button == 0 && event.down)
    {
        Pick();

        if (m_editorMode == EditorMode::Material && m_selectedSurface >= 0)
        {
            m_materialTab = 0;
            SetCursorMode(true);
        }
        else if (m_editorMode == EditorMode::Light && SelectedLight() != nullptr)
        {
            SetCursorMode(true);
        }
    }
}

void EditorApp::HandlePreviewOrbitMouseButton(const EditorEvent &event)
{
    if (event.button != 0)
    {
        return;
    }

    if (event.down && !m_ui.WantsMouse())
    {
        m_previewModelDragging = true;
        m_lastMouseX = int32_t(std::lround(event.x));
        m_lastMouseY = int32_t(std::lround(event.y));
    }
    else if (!event.down)
    {
        m_previewModelDragging = false;
    }
}

void EditorApp::HandleMouseWheelEvent(const EditorEvent &event)
{
    if (IsModelOrbitMode() && !m_ui.WantsMouse() && event.wheelY != 0.0f)
    {
        m_previewZoom = std::clamp(m_previewZoom * std::pow(1.12f, event.wheelY), 0.35f, 2.5f);
    }
}

void EditorApp::HandleViewportLeftPress(const EditorEvent &event)
{
    const bool additive = event.modifiers.ctrl || event.modifiers.shift ||
                          m_keys[size_t(EditorKey::Ctrl)] || m_keys[size_t(EditorKey::Shift)];
    bool handled = (m_editorMode == EditorMode::Light) && !additive &&
                   !HasMultiLightSelection() && BeginGizmoDrag(m_ui.MouseX(), m_ui.MouseY());
    if (!handled && m_editorMode == EditorMode::Light && !additive)
    {
        handled = BeginLightGrab();
    }
    if (handled)
    {
        return;
    }

    if (m_editorMode == EditorMode::Light && !m_placingSun && !m_customPlacing &&
        m_customCloning < 0)
    {
        m_marqueePending = true;
        m_marqueeDragging = false;
        m_marqueeAdditive = additive;
        m_marqueeStart[0] = event.x;
        m_marqueeStart[1] = event.y;
        m_marqueeEnd[0] = event.x;
        m_marqueeEnd[1] = event.y;
        m_window.SetMouseCapture(true);
        return;
    }

    m_pendingAdditivePick = additive;
    Pick();
    m_pendingAdditivePick = false;
}

void EditorApp::HandleViewportLeftRelease(const EditorEvent &event)
{
    if (m_marqueePending)
    {
        m_marqueeEnd[0] = event.x;
        m_marqueeEnd[1] = event.y;
        const bool commitMarquee = m_marqueeDragging;
        const bool additiveRelease = m_marqueeAdditive || event.modifiers.ctrl ||
                                     event.modifiers.shift || m_keys[size_t(EditorKey::Ctrl)] ||
                                     m_keys[size_t(EditorKey::Shift)];
        if (commitMarquee)
        {
            CommitMarqueeSelection();
        }
        CancelMarqueeSelection();
        if (!commitMarquee)
        {
            m_pendingAdditivePick = additiveRelease;
            Pick();
            m_pendingAdditivePick = false;
        }
    }
    FinishGizmoDrag(true);
    FinishLightGrab(true);
}

void EditorApp::HandleMouseButtonEvent(const EditorEvent &event)
{
    if (!m_sceneLoaded)
    {
        return;
    }
    if (IsModelOrbitMode())
    {
        HandlePreviewOrbitMouseButton(event);
        return;
    }
    if (!m_showPanel && !HasModalOpen())
    {
        HandleFlyMouseButton(event);
        return;
    }
    if (event.button != 0)
    {
        return;
    }
    if (event.down && !m_ui.WantsMouse())
    {
        HandleViewportLeftPress(event);
    }
    else if (!event.down)
    {
        HandleViewportLeftRelease(event);
    }
}

void EditorApp::HandleModeCtrlShortcut(EditorKey key)
{
    if (!m_sceneLoaded)
    {
        return;
    }

    if (key == EditorKey::S && HasUnsavedEditorChanges())
    {
        SaveSessionToDisk();
    }
    else if (key == EditorKey::P && CanPlayCurrentLevel() && m_playProcess.handle == 0)
    {
        RequestPlayLevel();
    }
    else if (key == EditorKey::C && m_editorMode == EditorMode::Light)
    {
        CopySelectedLights();
    }
    else if (key == EditorKey::V && m_editorMode == EditorMode::Light)
    {
        PasteCopiedLights();
    }
}

void EditorApp::HandleCtrlShortcut(EditorKey key)
{
    switch (key)
    {
    case EditorKey::O:
        OpenLevelBrowserDialog();
        break;
    case EditorKey::M:
        OpenModBrowserDialog();
        break;
    case EditorKey::W:
        if (m_sceneLoaded)
        {
            RequestSessionAction(PendingSessionAction::CloseScene);
        }
        break;
    case EditorKey::R:
        if (m_sceneLoaded)
        {
            RequestSessionAction(PendingSessionAction::ReloadScene);
        }
        break;
    default:
        HandleModeCtrlShortcut(key);
        break;
    }
}

void EditorApp::HandleUnmodifiedEditorKey(EditorKey key)
{
    if (key == EditorKey::Home)
    {
        ResetCameraToScene();
        return;
    }

    if (key == EditorKey::Delete && m_editorMode == EditorMode::Light)
    {
        RemoveSelectedLights();
    }
}

void EditorApp::HandleKeyEvent(const EditorEvent &event)
{
    const size_t keyIndex = size_t(event.key);

    if (keyIndex < size_t(EditorKey::Count))
    {
        m_keys[keyIndex] = event.down;
    }

    if (!event.down || event.repeat)
    {
        return;
    }

    if (event.key == EditorKey::Escape)
    {
        HandleEditorModeKeyDown(int(event.key));
        return;
    }

    const bool ctrl = event.modifiers.ctrl || m_keys[size_t(EditorKey::Ctrl)];
    const bool wantsText = m_ui.WantsTextInput();
    const bool hasPrompt = m_sessionPrompt != 0 || m_resetPrompt != 0 || m_regenPrompt != 0;

    if (ctrl && !wantsText && !hasPrompt)
    {
        HandleCtrlShortcut(event.key);
        return;
    }

    if (!wantsText && !HasModalOpen() && m_sceneLoaded)
    {
        HandleUnmodifiedEditorKey(event.key);
    }
}

void EditorApp::HandleFocusLost()
{
    m_hasFocus = false;
    std::fill(std::begin(m_keys), std::end(m_keys), false);

    m_looking = false;
    m_previewModelDragging = false;
    m_window.SetMouseCapture(false);
    m_ui.SetCursorVisible(true);

    FinishGizmoDrag(false);
    FinishLightGrab(false);
    CancelMarqueeSelection();
}

namespace
{
bool IsCursorModeToggleEvent(const EditorEvent &event, bool hasModal, bool showPanel,
                             bool wantsTextInput)
{
    return event.type == EditorEventType::Key && event.down && !event.repeat && !hasModal &&
           (event.key == EditorKey::Tab || event.key == EditorKey::F1) &&
           (!showPanel || !wantsTextInput);
}
} // namespace

void EditorApp::HandleFocusEvent(bool focused)
{
    m_hasFocus = focused;

    if (!focused)
    {
        HandleFocusLost();
    }
    else if (!m_showPanel && m_maxFrames == 0 && !HasModalOpen())
    {
        SetCursorMode(false);
    }
}

bool EditorApp::HandlePlayProcessEvent(const EditorEvent &event)
{
    if (m_playProcess.handle == 0)
    {
        return false;
    }

    if (event.type == EditorEventType::Focus)
    {
        m_ui.HandleEvent(event);
        if (event.down)
        {
            m_hasFocus = true;
        }
        else
        {
            HandleFocusLost();
        }
    }
    return true;
}

void EditorApp::RestoreFocusFromInputEvent(const EditorEvent &event)
{
    if (m_hasFocus || !event.down ||
        (event.type != EditorEventType::Key && event.type != EditorEventType::MouseButton))
    {
        return;
    }

    m_hasFocus = true;
    EditorEvent focusEvent = {};
    focusEvent.type = EditorEventType::Focus;
    focusEvent.down = true;
    m_ui.HandleEvent(focusEvent);
}

void EditorApp::HandleEvent(const EditorEvent &event)
{
    if (event.type == EditorEventType::Close)
    {
        RequestExit();
        return;
    }

    if (HandlePlayProcessEvent(event))
    {
        return;
    }

    if (event.type == EditorEventType::Focus)
    {
        m_ui.HandleEvent(event);
        HandleFocusEvent(event.down);
        return;
    }

    RestoreFocusFromInputEvent(event);

    if (IsCursorModeToggleEvent(event, HasModalOpen(), m_showPanel, m_ui.WantsTextInput()))
    {
        SetCursorMode(!m_showPanel);
        return;
    }

    if (m_showPanel || HasModalOpen())
    {
        m_ui.HandleEvent(event);
    }

    switch (event.type)
    {
    case EditorEventType::MouseMove:
        HandleMouseMoveEvent(event);
        break;

    case EditorEventType::MouseButton:
        HandleMouseButtonEvent(event);
        break;

    case EditorEventType::MouseWheel:
        HandleMouseWheelEvent(event);
        break;

    case EditorEventType::Key:
        HandleKeyEvent(event);
        break;

    default:
        break;
    }
}

bool EditorApp::CreateUi()
{
    std::string error;

    m_fontPath = ResolveDefaultAssetPath("fonts/Roboto-Regular.ttf");
    const std::string iconFontPath = ResolveDefaultAssetPath("fonts/fa-solid-900.ttf");

    if (!m_ui.Initialize(&m_window, m_instance, m_fontPath, error, iconFontPath))
    {
        PrintMessage(("ui: " + error).c_str(), this);
        return false;
    }

    PrintMessage("ui ready", this);
    return true;
}

void EditorApp::ResetCameraToScene()
{
    if (m_scene.hasCamera)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            m_cameraPosition[axis] = m_scene.cameraPosition[axis];
        }

        m_cameraYaw = m_scene.cameraYawDegrees * 0.01745329252f;
        m_cameraPitch = 0.0f;
        return;
    }

    float center[3] = { 0.0f, 0.0f, 0.0f };
    float extent = 10.0f;

    if (m_scene.hasBounds)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            center[axis] = (m_scene.boundsMin[axis] + m_scene.boundsMax[axis]) * 0.5f;
            extent = std::max(extent, m_scene.boundsMax[axis] - m_scene.boundsMin[axis]);
        }
    }

    m_cameraPosition[0] = center[0] + extent * 0.8f;
    m_cameraPosition[1] = center[1] - extent * 1.0f;
    m_cameraPosition[2] = center[2] + extent * 0.6f;

    const float dx = center[0] - m_cameraPosition[0];
    const float dy = center[1] - m_cameraPosition[1];
    const float dz = center[2] - m_cameraPosition[2];

    m_cameraYaw = std::atan2(dy, dx);
    m_cameraPitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy));
    m_cameraSpeed = std::max(64.0f, extent * 0.5f);
}

void EditorApp::UpdateCamera(float deltaSeconds)
{
    if (m_showPanel || HasModalOpen())
    {
        return;
    }

    const float cosPitch = std::cos(m_cameraPitch);

    const float forward[3] = {
        cosPitch * std::cos(m_cameraYaw),
        cosPitch * std::sin(m_cameraYaw),
        std::sin(m_cameraPitch),
    };

    const float right[3] = {
        std::sin(m_cameraYaw),
        -std::cos(m_cameraYaw),
        0.0f,
    };

    float speed = m_cameraSpeed * deltaSeconds;

    if (m_keys[size_t(EditorKey::Shift)])
    {
        speed *= 3.0f;
    }

    float movement[3] = { 0.0f, 0.0f, 0.0f };

    const auto addAxis = [&](EditorKey key, const float *axis, float scale) {
        if (m_keys[size_t(key)])
        {
            movement[0] += axis[0] * scale;
            movement[1] += axis[1] * scale;
            movement[2] += axis[2] * scale;
        }
    };

    addAxis(EditorKey::W, forward, speed);
    addAxis(EditorKey::S, forward, -speed);
    addAxis(EditorKey::D, right, speed);
    addAxis(EditorKey::A, right, -speed);

    if (m_keys[size_t(EditorKey::E)] || m_keys[size_t(EditorKey::Space)])
    {
        movement[2] += speed;
    }

    if (m_keys[size_t(EditorKey::Q)] || m_keys[size_t(EditorKey::Ctrl)])
    {
        movement[2] -= speed;
    }

    for (int axis = 0; axis < 3; axis++)
    {
        m_cameraPosition[axis] += movement[axis];
    }
}

void EditorApp::SunDirection(float direction[3]) const
{
    EditorSunDirectionToSun(m_sun, direction);
}

void EditorApp::UploadSun()
{
    m_runtime.UploadSun(m_instance, [this](const char *msg) { PrintMessage(msg, this); });
}

void EditorApp::UploadLight(const EditorLight &light, uint64_t uniqueID, QrResult &lastResult)
{
    lastResult = EditorSceneRuntime::UploadSingleLight(m_instance, light, uniqueID);
}

void EditorApp::UploadLights()
{
    const auto logFn = [this](const char *msg) { PrintMessage(msg, this); };
    QrResult lastResult = m_runtime.UploadLights(m_instance, 0.0, false, logFn);

    if (m_torchMode && lastResult == QR_SUCCESS)
    {
        UploadTorchLight(lastResult);
    }

    if (IsModelPreviewActive() && m_previewLight && lastResult == QR_SUCCESS)
    {
        UploadPreviewLight(lastResult);
    }

    if (lastResult != QR_SUCCESS)
    {
        static bool logged = false;

        if (!logged)
        {
            logged = true;
            PrintMessage(("lights: " + std::string(qrGetResultDescription(lastResult))).c_str(),
                         this);
        }
    }
}

void EditorApp::LogLightCluster(const EditorLight &light, uint64_t uniqueID, const char *label)
{
    uint64_t uids[128] = {};
    uint32_t count = 0;

    if (qrGetClusterLightList(m_instance, light.cluster, uids, 128, &count) != QR_SUCCESS)
    {
        PrintMessage((std::string(label) + ": cluster " + std::to_string(light.cluster) +
                      " unreadable")
                         .c_str(),
                     this);
        return;
    }

    // A light its own cluster does not list cannot be shaded there, whatever it was authored to:
    // this is what says so, instead of a dark viewport that looks like a bad radiance value.
    bool listed = false;

    for (uint32_t slot = 0; slot < count; slot++)
    {
        listed = listed || uids[slot] == uniqueID;
    }

    PrintMessage((std::string(label) + ": cluster " + std::to_string(light.cluster) + " lists " +
                  std::to_string(count) + (listed ? " with it" : " without it") + ", radiance " +
                  std::to_string(light.color[0]) + " " + std::to_string(light.color[1]) + " " +
                  std::to_string(light.color[2]) + ", radius " + std::to_string(light.radius))
                     .c_str(),
                 this);
}

namespace
{

void AppendPriorityClusterSource(const EditorLight &light, uint64_t uniqueID, float reach,
                                 std::vector<QrClusterLightSource> &sources)
{
    const float px = light.position[0] + (light.hasOffset ? light.offset[0] : 0.0f);
    const float py = light.position[1] + (light.hasOffset ? light.offset[1] : 0.0f);
    const float pz = light.position[2] + (light.hasOffset ? light.offset[2] : 0.0f);
    QrClusterLightSource source = {};
    source.uniqueID = uniqueID;
    source.origin = { px, py, pz };
    source.cluster = light.cluster;
    source.reach = reach;
    source.power = 1.0e7f;
    sources.push_back(source);
}

} // namespace

void EditorApp::UploadClusterLightSources()
{
    std::vector<QrClusterLightSource> extraSources;
    if (m_torchMode)
    {
        EditorLight torchLight;
        std::copy_n(m_cameraPosition, 3, torchLight.position);
        torchLight.cluster = ResolveEditorLightCluster(m_scene, m_cameraPosition);
        AppendPriorityClusterSource(torchLight, kEditorTorchUniqueID,
                                    kEditorDynamicLightReachUnits, extraSources);
    }
    if (IsModelPreviewActive() && m_previewLight)
    {
        EditorLight previewLight;
        BuildPreviewLight(previewLight);
        AppendPriorityClusterSource(previewLight, kPreviewLightUniqueId,
                                    kEditorDynamicLightReachUnits, extraSources);
    }

    const EditorClusterLightUploadExtras extras{
        extraSources.empty() ? nullptr : &extraSources,
        nullptr,
    };
    m_runtime.UploadClusterLightSources(
        m_instance, &extras, [this](const char *msg) { PrintMessage(msg, this); });
}

namespace
{
std::string NormalizeEmitterBase(const std::string &name)
{
    return NormalizeEmitterNameBase(name);
}

void BuildEmitterLookupCandidates(const std::string &name, std::vector<std::string> &outNames)
{
    BuildEmitterOverrideCandidates(name, outNames);
}

const EditorEmitterLightOverride *FindOverrideByExactName(
    const std::vector<EditorEmitterLightOverride> &list, const std::string &key)
{
    return FindEmitterOverrideExact(list, key);
}

const EditorGeneratedLightBase *FindGeneratedBaseForLight(
    const std::vector<EditorGeneratedLightBase> &bases, const EditorLight &light)
{
    for (const EditorGeneratedLightBase &b : bases)
    {
        if (b.uniqueID == light.emitterUniqueID && b.name == light.className)
        {
            return &b;
        }
    }
    return nullptr;
}

void CopyEffectiveRadiometry(const EditorLight &light, const EditorGeneratedLightBase *basePtr,
                             const EditorEmitterLightOverride *ov,
                             EditorEmitterLightOverride &out)
{
    out.radius =
        basePtr != nullptr ? basePtr->defaultRadiusMeters : (light.radius / kEditorUnitsPerMeter);
    if (basePtr != nullptr)
    {
        std::copy_n(basePtr->baseColor, 3, out.color);
    }

    if (ov == nullptr)
    {
        return;
    }

    out.hasRadius = ov->hasRadius;
    if (ov->hasRadius)
    {
        out.radius = ov->radius;
    }
    out.hasIntensity = ov->hasIntensity;
    if (ov->hasIntensity)
    {
        out.intensity = ov->intensity;
    }
    out.hasOffset = ov->hasOffset;
    if (ov->hasOffset)
    {
        std::copy_n(ov->offset, 3, out.offset);
    }
    out.hasColor = ov->hasColor;
    if (ov->hasColor)
    {
        std::copy_n(ov->color, 3, out.color);
    }
}

void CopyEffectiveSpotAndFlags(const EditorLight &light, const EditorGeneratedLightBase *basePtr,
                               const EditorEmitterLightOverride *ov, bool hasOwnInstance,
                               EditorEmitterLightOverride &out)
{
    out.spot = basePtr != nullptr ? basePtr->spot : light.spot;
    std::copy_n(basePtr != nullptr ? basePtr->direction : light.direction, 3, out.dir);
    out.angleInner = basePtr != nullptr ? basePtr->angleInner : light.angleInner;
    out.angleOuter = basePtr != nullptr ? basePtr->angleOuter : light.angleOuter;
    out.groupEdit = !hasOwnInstance;
    out.enabled = light.enabled;

    if (ov == nullptr)
    {
        return;
    }

    out.hasStyle = ov->hasStyle;
    out.style = ov->hasStyle ? ov->style : 0;
    out.hasSpot = ov->hasSpot;
    if (ov->hasSpot)
    {
        out.spot = ov->spot;
    }
    out.hasDir = ov->hasDir;
    if (ov->hasDir)
    {
        std::copy_n(ov->dir, 3, out.dir);
    }
    out.hasAngles = ov->hasAngles;
    if (ov->hasAngles)
    {
        out.angleInner = ov->angleInner;
        out.angleOuter = ov->angleOuter;
    }
    out.forceRasterize = ov->forceRasterize;
    out.enabled = ov->enabled;
    if (hasOwnInstance || !ov->groupEdit)
    {
        out.groupEdit = false;
    }
}

void MaybeAppendEmitterLightsYaml(std::string &merged,
                                  const std::vector<EditorEmitterLightOverride> &current,
                                  const std::vector<EditorEmitterLightOverride> &baseline)
{
    if (RenderEmitterLightsBlock(current) != RenderEmitterLightsBlock(baseline))
    {
        merged = MergeEmitterLightsYaml(merged, current);
    }
}

} // namespace

void EditorApp::BuildEmissiveLights()
{
    m_lastLightBuildSettings = m_settings;
    m_hasLightBuildSettings = true;

    const std::vector<SavedLightToken> savedSel =
        CaptureLightSelectionTokens(m_scene.lights, m_selectedLights, m_selectedLight);
    m_runtime.BuildEmissiveLights(
        m_instance, m_assets, m_material, [this](const char *msg) { PrintMessage(msg, this); });

    m_selectedLights.clear();
    m_selectedLight = -1;
    for (const SavedLightToken &s : savedSel)
    {
        const int32_t matched = MatchSavedLightTokenIndex(m_scene.lights, s);
        if (matched >= 0 &&
            std::find(m_selectedLights.begin(), m_selectedLights.end(), matched) ==
                m_selectedLights.end())
        {
            m_selectedLights.push_back(matched);
        }
    }
    SyncPrimaryLightSelection();
}

const EditorEmitterLightOverride *EditorApp::FindEmitterOverride(const std::string &name,
                                                                 uint64_t uniqueID) const
{
    return FindEditorEmitterOverride(m_emitterLightOverrides, name, uniqueID);
}

bool EditorApp::HasOwnEmitterInstanceOverride(int32_t lightIndex) const
{
    if (lightIndex < 0 || size_t(lightIndex) >= m_scene.lights.size())
    {
        return false;
    }

    const EditorLight &light = m_scene.lights[size_t(lightIndex)];
    if (light.emitterUniqueID == 0 || light.className.empty())
    {
        return false;
    }

    std::vector<std::string> candidates;
    BuildEmitterLookupCandidates(light.className, candidates);
    const std::string suffix = "#" + std::to_string(light.emitterUniqueID);

    for (const std::string &base : candidates)
    {
        if (FindOverrideByExactName(m_emitterLightOverrides, base + suffix) != nullptr)
        {
            return true;
        }
    }
    return false;
}

EditorEmitterLightOverride EditorApp::EffectiveEmitterOverride(int32_t lightIndex) const
{
    EditorEmitterLightOverride out;
    if (lightIndex < 0 || size_t(lightIndex) >= m_scene.lights.size())
    {
        return out;
    }

    const EditorLight &light = m_scene.lights[size_t(lightIndex)];
    const EditorEmitterLightOverride *ov =
        FindEmitterOverride(light.className, light.emitterUniqueID);
    const EditorGeneratedLightBase *basePtr = FindGeneratedBaseForLight(m_generatedLightBases, light);
    const bool hasOwn = HasOwnEmitterInstanceOverride(lightIndex);

    out.name = NormalizeEmitterBase(light.className);
    CopyEffectiveRadiometry(light, basePtr, ov, out);
    CopyEffectiveSpotAndFlags(light, basePtr, ov, hasOwn, out);
    return out;
}

void EditorApp::ApplyEntityEmitterEdit(int32_t lightIndex,
                                       const EditorEmitterLightOverride &edited,
                                       bool reapply)
{
    if (lightIndex < 0 || size_t(lightIndex) >= m_scene.lights.size())
    {
        return;
    }

    const EditorLight &light = m_scene.lights[size_t(lightIndex)];
    if (light.emitterUniqueID == 0 || light.className.empty())
    {
        return;
    }

    const bool ownInstance = HasOwnEmitterInstanceOverride(lightIndex) || !edited.groupEdit;
    std::vector<std::string> candidates;
    BuildEmitterLookupCandidates(light.className, candidates);
    std::string targetKey = candidates.empty() ? NormalizeEmitterBase(light.className) : candidates.front();
    if (ownInstance)
    {
        targetKey += "#" + std::to_string(light.emitterUniqueID);
    }

    EditorEmitterLightOverride *slot = nullptr;
    for (EditorEmitterLightOverride &ov : m_emitterLightOverrides)
    {
        if (LowercaseName(ov.name) == targetKey)
        {
            slot = &ov;
            break;
        }
    }

    if (slot == nullptr)
    {
        m_emitterLightOverrides.emplace_back();
        slot = &m_emitterLightOverrides.back();
    }

    *slot = edited;
    slot->name = targetKey;
    slot->groupEdit = !ownInstance;
    if (reapply)
    {
        ReapplyGeneratedLights();
    }
}

void EditorApp::SetEntityLightGroupEdit(int32_t lightIndex, bool groupEdit)
{
    if (lightIndex < 0 || size_t(lightIndex) >= m_scene.lights.size())
    {
        return;
    }

    const EditorLight &light = m_scene.lights[size_t(lightIndex)];
    if (light.emitterUniqueID == 0 || light.className.empty())
    {
        return;
    }

    std::vector<std::string> candidates;
    BuildEmitterLookupCandidates(light.className, candidates);
    const std::string suffix = "#" + std::to_string(light.emitterUniqueID);

    if (groupEdit)
    {
        m_emitterLightOverrides.erase(
            std::remove_if(m_emitterLightOverrides.begin(), m_emitterLightOverrides.end(),
                           [&candidates, &suffix](const EditorEmitterLightOverride &ov) {
                               const std::string lower = LowercaseName(ov.name);
                               for (const std::string &base : candidates)
                               {
                                   if (lower == base + suffix)
                                   {
                                       return true;
                                   }
                               }
                               return false;
                           }),
            m_emitterLightOverrides.end());
        ReapplyGeneratedLights();
        return;
    }

    EditorEmitterLightOverride own = EffectiveEmitterOverride(lightIndex);
    own.groupEdit = false;
    ApplyEntityEmitterEdit(lightIndex, own);
}

void EditorApp::ResetEntityEmitterLight(int32_t lightIndex)
{
    if (lightIndex < 0 || size_t(lightIndex) >= m_scene.lights.size())
    {
        return;
    }

    const EditorLight &light = m_scene.lights[size_t(lightIndex)];
    if (light.emitterUniqueID == 0)
    {
        if (const EditorLight *base =
                BaselineMapLightAt(lightIndex, m_scene.lights, m_baselineMapLights))
        {
            m_scene.lights[size_t(lightIndex)] = *base;
        }
        ClearDirtyFlagIfClean();
        return;
    }

    if (ResetSingleEmitterOverride(light.className, light.emitterUniqueID, m_emitterLightOverrides,
                                   m_baselineEmitterOverrides))
    {
        ReapplyGeneratedLights();
    }
    ClearDirtyFlagIfClean();
}

void EditorApp::ReapplyGeneratedLights()
{
    const std::vector<SavedLightToken> savedSel =
        CaptureLightSelectionTokens(m_scene.lights, m_selectedLights, m_selectedLight);
    m_runtime.ReapplyGeneratedLights();

    m_selectedLights.clear();
    m_selectedLight = -1;
    for (const SavedLightToken &s : savedSel)
    {
        const int32_t matched = MatchSavedLightTokenIndex(m_scene.lights, s);
        if (matched >= 0 &&
            std::find(m_selectedLights.begin(), m_selectedLights.end(), matched) ==
                m_selectedLights.end())
        {
            m_selectedLights.push_back(matched);
        }
    }
    SyncPrimaryLightSelection();
}

void EditorApp::RestoreSceneLightsAfterSessionReload()
{
    ReapplyGeneratedLights();
    SyncPrimaryLightSelection();
}

void EditorApp::ClearDirtyFlagIfClean()
{
    if (m_sceneLoaded && m_touchedMaterials.empty() && LevelFingerprint() == m_levelFingerprint &&
        !HasUnsavedSystemSettings())
    {
        m_levelTouched = false;
        m_sessionDirty = false;
    }
}

namespace
{

bool DidDtalCoreSettingsChange(const EditorRenderSettings &a, const EditorRenderSettings &b)
{
    return a.truelight != b.truelight || a.levelLights != b.levelLights ||
           a.modelLights != b.modelLights || a.dtalProxy != b.dtalProxy ||
           a.dtalMinArea != b.dtalMinArea || a.dtalProxyMinArea != b.dtalProxyMinArea ||
           a.dtalGroups != b.dtalGroups || a.dtalSpacing != b.dtalSpacing ||
           a.emissionLightIntensity != b.emissionLightIntensity ||
           a.emissiveNee != b.emissiveNee;
}

bool DidDtalLimitSettingsChange(const EditorRenderSettings &a, const EditorRenderSettings &b)
{
    return a.dtalMaxPolys != b.dtalMaxPolys || a.dtalMaxGroups != b.dtalMaxGroups ||
           a.dtalModelMinArea != b.dtalModelMinArea ||
           a.dtalProxyModelMinArea != b.dtalProxyModelMinArea ||
           a.dtalModelMaxPolys != b.dtalModelMaxPolys ||
           a.levelLightMaxPolys != b.levelLightMaxPolys ||
           a.dtalModelBudget != b.dtalModelBudget;
}

} // namespace

void EditorApp::RefreshGeneratedLightsIfSettingsChanged()
{
    if (!m_hasLightBuildSettings)
    {
        BuildEmissiveLights();
        return;
    }

    const EditorRenderSettings &prev = m_lastLightBuildSettings;
    const bool rebuildChanged =
        DidDtalCoreSettingsChange(m_settings, prev) || DidDtalLimitSettingsChange(m_settings, prev);

    const bool radiometryChanged =
        m_settings.brightness != prev.brightness ||
        m_settings.globalLightMult != prev.globalLightMult;

    if (rebuildChanged)
    {
        if (m_settings.emissiveNee != prev.emissiveNee || m_settings.truelight != prev.truelight)
        {
            m_materialsChanged = true;
        }
        BuildEmissiveLights();
    }
    else if (radiometryChanged)
    {
        m_lastLightBuildSettings = m_settings;
        ReapplyGeneratedLights();
    }
}


void EditorApp::UploadEmissiveLights()
{
    const float timeSeconds = float(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - m_startTime).count());
    m_runtime.UploadEmissiveLights(
        m_instance, timeSeconds, nullptr,
        [this](const char *msg) { PrintMessage(msg, this); });
}

void EditorApp::ViewBasis(float side[3], float up[3], float forward[3]) const
{
    // The same basis LookAt builds the view matrix from, so a ray cast here and a vertex
    // projected with the same basis agree with what the frame draws.
    const float cosPitch = std::cos(m_cameraPitch);

    forward[0] = cosPitch * std::cos(m_cameraYaw);
    forward[1] = cosPitch * std::sin(m_cameraYaw);
    forward[2] = std::sin(m_cameraPitch);

    const float worldUp[3] = { 0.0f, 0.0f, 1.0f };

    side[0] = forward[1] * worldUp[2] - forward[2] * worldUp[1];
    side[1] = forward[2] * worldUp[0] - forward[0] * worldUp[2];
    side[2] = forward[0] * worldUp[1] - forward[1] * worldUp[0];

    const float sideLength = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);

    if (sideLength > 1e-6f)
    {
        side[0] /= sideLength;
        side[1] /= sideLength;
        side[2] /= sideLength;
    }
    else
    {
        side[0] = 1.0f;
        side[1] = 0.0f;
        side[2] = 0.0f;
    }

    up[0] = side[1] * forward[2] - side[2] * forward[1];
    up[1] = side[2] * forward[0] - side[0] * forward[2];
    up[2] = side[0] * forward[1] - side[1] * forward[0];
}

float EditorApp::FovYRadians() const
{
    return EditorFovYRadians(m_settings, float(m_clientWidth), float(m_clientHeight));
}

namespace
{
// One light of either list, so the pick walks the map's lights and the editor's own in the
// same pass and reports which list the winner came from.
struct EditorLightRef
{
    const EditorLight *light = nullptr;
    bool custom = false;
    int32_t index = -1;
};

void CollectEditorLights(const std::vector<EditorLight> &mapLights,
                         const std::vector<EditorLight> &customLights,
                         bool includeMapLights,
                         std::vector<EditorLightRef> &outLights)
{
    outLights.clear();
    outLights.reserve((includeMapLights ? mapLights.size() : 0) + customLights.size());

    for (size_t index = 0; includeMapLights && index < mapLights.size(); index++)
    {
        outLights.push_back({ &mapLights[index], false, int32_t(index) });
    }

    for (size_t index = 0; index < customLights.size(); index++)
    {
        outLights.push_back({ &customLights[index], true, int32_t(index) });
    }
}
} // namespace

int32_t EditorApp::PickLight(const float origin[3], const float direction[3], float &outDistance,
                             bool &outCustom) const
{
    // QRE_LightUnderCrosshair accepts the light nearest the crosshair ray rather than one exactly
    // under it, because a light is drawn as a small gizmo, not as a surface.
    int32_t bestLight = -1;
    bool bestCustom = false;
    float bestDistance = 0.0f;

    std::vector<EditorLightRef> lights;
    CollectEditorLights(m_scene.lights, m_customLights, true, lights);

    for (const EditorLightRef &candidate : lights)
    {
        const EditorLight &light = *candidate.light;
        float pos[3] = {};
        EffectiveLightPosition(light, pos);
        const float toLight[3] = {
            pos[0] - origin[0],
            pos[1] - origin[1],
            pos[2] - origin[2],
        };

        const float along = toLight[0] * direction[0] + toLight[1] * direction[1] +
                            toLight[2] * direction[2];

        if (along <= 0.0f)
        {
            continue;
        }

        const float perpendicular[3] = {
            toLight[0] - direction[0] * along,
            toLight[1] - direction[1] * along,
            toLight[2] - direction[2] * along,
        };

        const float distanceSquared = perpendicular[0] * perpendicular[0] +
                                      perpendicular[1] * perpendicular[1] +
                                      perpendicular[2] * perpendicular[2];
        const float pickRadius = std::max(light.radius, kLightPickRadiusUnits);

        if (distanceSquared > pickRadius * pickRadius)
        {
            continue;
        }

        if (bestLight < 0 || along < bestDistance)
        {
            bestLight = candidate.index;
            bestCustom = candidate.custom;
            bestDistance = along;
        }
    }

    outDistance = bestDistance;
    outCustom = bestCustom;
    return bestLight;
}

void EditorApp::ClearSelection()
{
    m_selectedSurface = -1;
    m_selectedFace = -1;
    m_selectedLight = -1;
    m_selectedCustomLight = -1;
    m_selectedLights.clear();
    m_selectedCustomLights.clear();
    m_hasPickPoint = false;
    m_pickCluster = 0;
}

EditorLight *EditorApp::SelectedLight()
{
    if (m_selectedLight >= 0 && size_t(m_selectedLight) < m_scene.lights.size())
    {
        return &m_scene.lights[size_t(m_selectedLight)];
    }

    if (m_selectedCustomLight >= 0 && size_t(m_selectedCustomLight) < m_customLights.size())
    {
        return &m_customLights[size_t(m_selectedCustomLight)];
    }

    return nullptr;
}

std::string EditorApp::SelectedLightLabel() const
{
    if (m_selectedLight >= 0 && size_t(m_selectedLight) < m_scene.lights.size())
    {
        return "light " + std::to_string(m_selectedLight);
    }

    if (m_selectedCustomLight >= 0 && size_t(m_selectedCustomLight) < m_customLights.size())
    {
        return "custom " + std::to_string(m_selectedCustomLight);
    }

    return "no light";
}

uint64_t EditorApp::SelectedLightUniqueID() const
{
    if (m_selectedLight >= 0 && size_t(m_selectedLight) < m_scene.lights.size())
    {
        return LightUniqueID(size_t(m_selectedLight));
    }

    if (m_selectedCustomLight >= 0 && size_t(m_selectedCustomLight) < m_customLights.size())
    {
        return CustomLightUniqueID(size_t(m_selectedCustomLight));
    }

    return 0;
}

// A copy the author has to find again: the game editor steps a Duplicate 32 units up, which is
// just enough to see the second light next to the first.
constexpr float kDuplicateStepUnits = 32.0f;

// Stands a light off the surface it was cloned onto, so it does not sit inside the face.
constexpr float kCloneOffsetUnits = 8.0f;

void EditorApp::AddCustomLight(const EditorLight *source, LightSpawn spawn)
{
    EditorLight light;

    if (source != nullptr)
    {
        light = *source;
        // The copy is a light of its own: it keeps the authored colour, cone and style but gets
        // its own position slot, so the two can be aimed separately.
        light.className.clear();
    }
    else
    {
        SetEditorLightIntensity(light, kEditorNewLightIntensity);
        light.radius = kEditorNewLightRadiusUnits;
    }

    float ray[3] = {};
    float rayLength = 0.0f;

    if (spawn == LightSpawn::Clone && m_hasPickPoint)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            ray[axis] = m_pickPoint[axis] - m_cameraPosition[axis];
        }

        rayLength = std::sqrt(ray[0] * ray[0] + ray[1] * ray[1] + ray[2] * ray[2]);
    }

    if (rayLength > 1e-4f)
    {
        // Clone lands on what the author looks at, aimed the same way as Place at pick: a copy
        // that kept the original's aim would light the space behind the picked face.
        for (int axis = 0; axis < 3; axis++)
        {
            ray[axis] /= rayLength;
            light.position[axis] = m_pickPoint[axis] - ray[axis] * kCloneOffsetUnits;
            light.direction[axis] = ray[axis];
        }
    }
    else if (spawn == LightSpawn::Duplicate && source != nullptr)
    {
        // A duplicate is not aimed at anything new, so it keeps the original's direction and
        // only steps out of it.
        light.position[2] += kDuplicateStepUnits;
    }
    else
    {
        // A fresh light lands where the camera looks: inside the map and in front of the
        // author, rather than at the world origin, where there is usually a wall.
        float side[3] = {};
        float up[3] = {};
        float forward[3] = {};
        ViewBasis(side, up, forward);

        constexpr float kSpawnDistanceUnits = 96.0f;

        for (int axis = 0; axis < 3; axis++)
        {
            light.position[axis] = m_cameraPosition[axis] + forward[axis] * kSpawnDistanceUnits;
        }
    }

    light.cluster = ResolveEditorLightCluster(m_scene, light.position);

    if (light.cluster == QR_CLUSTER_LIGHT_NO_CLUSTER && m_pickCluster != 0)
    {
        light.cluster = m_pickCluster;
    }

    m_customLights.push_back(light);
    SelectLight(int32_t(m_customLights.size()) - 1, true);

    PrintMessage((SelectedLightLabel() + ": added at " + std::to_string(int32_t(light.position[0])) +
                  " " + std::to_string(int32_t(light.position[1])) + " " +
                  std::to_string(int32_t(light.position[2])) + " cluster " +
                  std::to_string(light.cluster))
                     .c_str(),
                 this);
}

void EditorApp::DuplicateSelectedLight()
{
    const EditorLight *selected = SelectedLight();

    if (selected == nullptr)
    {
        PrintMessage("duplicate: pick a light first", this);
        return;
    }

    AddCustomLight(selected, LightSpawn::Duplicate);
    PrintMessage((SelectedLightLabel() + ": duplicated").c_str(), this);
}

void EditorApp::CloneSelectedLight()
{
    const EditorLight *selected = SelectedLight();

    if (selected == nullptr)
    {
        PrintMessage("clone: pick a light first", this);
        return;
    }

    AddCustomLight(selected, LightSpawn::Clone);
    PrintMessage((SelectedLightLabel() + ": cloned").c_str(), this);
}

void EditorApp::CopySelectedLights()
{
    std::vector<EditorLight> copied = BuildCopiedLights(
        m_scene.lights, m_selectedLights, m_selectedLight, m_customLights,
        m_selectedCustomLights, m_selectedCustomLight);
    if (copied.empty())
    {
        Notify("copy: select light(s) first");
        return;
    }

    float side[3] = {};
    float up[3] = {};
    float forward[3] = {};
    ViewBasis(side, up, forward);

    m_lightClipboard.lights = std::move(copied);
    std::copy_n(m_cameraPosition, 3, m_lightClipboard.copyCameraPos);
    std::copy_n(forward, 3, m_lightClipboard.copyCameraForward);
    m_lightClipboard.pasteCount = 0u;

    const std::string msg = "copied " + std::to_string(m_lightClipboard.lights.size()) +
                            (m_lightClipboard.lights.size() == 1 ? " light" : " lights");
    PrintMessage(("light: " + msg).c_str(), this);
    Notify(msg);
}

void EditorApp::PasteCopiedLights()
{
    if (m_lightClipboard.lights.empty())
    {
        Notify("paste: light clipboard is empty");
        return;
    }

    float side[3] = {};
    float up[3] = {};
    float forward[3] = {};
    ViewBasis(side, up, forward);

    std::vector<EditorLight> pasted =
        BuildPastedLights(m_lightClipboard, m_cameraPosition, forward);
    if (pasted.empty())
    {
        return;
    }

    std::vector<int32_t> newIndices;
    newIndices.reserve(pasted.size());
    for (EditorLight &light : pasted)
    {
        light.cluster = ResolveEditorLightCluster(m_scene, light.position);
        if (light.cluster == QR_CLUSTER_LIGHT_NO_CLUSTER && m_pickCluster != 0)
        {
            light.cluster = m_pickCluster;
        }
        const int32_t idx = int32_t(m_customLights.size());
        m_customLights.push_back(std::move(light));
        newIndices.push_back(idx);
    }

    m_selectedLight = -1;
    m_selectedLights.clear();
    m_selectedCustomLights = std::move(newIndices);
    m_selectedCustomLight = m_selectedCustomLights.back();
    m_selectedSurface = -1;
    m_selectedFace = -1;
    m_lightTab = 1;

    const EditorLight &last = m_customLights[size_t(m_selectedCustomLight)];
    m_hasPickPoint = true;
    m_pickCluster = last.cluster;
    std::copy_n(last.position, 3, m_pickPoint);

    const std::string msg = "pasted " + std::to_string(pasted.size()) +
                            (pasted.size() == 1 ? " light" : " lights");
    PrintMessage(("light: " + msg).c_str(), this);
    Notify(msg);
}

namespace
{
std::vector<int32_t> CollectUniqueIndicesDesc(const std::vector<int32_t> &multi, int32_t single,
                                            size_t limit)
{
    std::vector<int32_t> out = multi;
    if (out.empty() && single >= 0)
    {
        out.push_back(single);
    }

    out.erase(std::remove_if(out.begin(), out.end(),
                             [limit](int32_t idx) { return idx < 0 || size_t(idx) >= limit; }),
              out.end());
    std::sort(out.begin(), out.end(), std::greater<int32_t>());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string FormatLightRemoveMessage(size_t removedEntity, int32_t lastEntityIdx,
                                     size_t removedCustom, int32_t lastCustomIdx,
                                     size_t disabledEmitters)
{
    const size_t totalRemoved = removedEntity + removedCustom;
    if (totalRemoved == 1 && disabledEmitters == 0)
    {
        return (removedEntity == 1) ? ("light " + std::to_string(lastEntityIdx) + ": removed")
                                    : ("custom " + std::to_string(lastCustomIdx) + ": removed");
    }
    if (totalRemoved > 0)
    {
        return "removed " + std::to_string(totalRemoved) + " lights";
    }
    if (disabledEmitters == 1)
    {
        return "emitter " + std::to_string(lastEntityIdx) + ": disabled";
    }
    return "disabled " + std::to_string(disabledEmitters) + " emitter lights";
}
} // namespace

void EditorApp::RemoveSelectedLights()
{
    const std::vector<int32_t> entityIndices =
        CollectUniqueIndicesDesc(m_selectedLights, m_selectedLight, m_scene.lights.size());
    const std::vector<int32_t> customIndices = CollectUniqueIndicesDesc(
        m_selectedCustomLights, m_selectedCustomLight, m_customLights.size());

    if (entityIndices.empty() && customIndices.empty())
    {
        return;
    }

    size_t removedEntity = 0;
    size_t disabledEmitters = 0;
    int32_t lastEntityIdx = -1;
    for (int32_t idx : entityIndices)
    {
        lastEntityIdx = idx;
        if (m_scene.lights[size_t(idx)].emitterUniqueID == 0)
        {
            m_scene.lights.erase(m_scene.lights.begin() + std::ptrdiff_t(idx));
            removedEntity++;
            continue;
        }

        EditorEmitterLightOverride ov = EffectiveEmitterOverride(idx);
        ov.enabled = false;
        ov.groupEdit = false;
        ApplyEntityEmitterEdit(idx, ov, false);
        disabledEmitters++;
    }

    if (disabledEmitters > 0)
    {
        ReapplyGeneratedLights();
    }

    size_t removedCustom = 0;
    int32_t lastCustomIdx = -1;
    for (int32_t idx : customIndices)
    {
        lastCustomIdx = idx;
        m_customLights.erase(m_customLights.begin() + std::ptrdiff_t(idx));
        removedCustom++;
    }

    ClearSelection();
    FinishGizmoDrag(false);
    FinishLightGrab(false);
    m_customPlacing = false;
    m_customCloning = -1;
    m_hoverLight = -1;
    m_hoverCustomLight = -1;

    const std::string msg = FormatLightRemoveMessage(removedEntity, lastEntityIdx, removedCustom,
                                                     lastCustomIdx, disabledEmitters);
    PrintMessage(msg.c_str(), this);
}

void EditorApp::RemoveSelectedCustomLight()
{
    if (m_selectedCustomLights.size() > 1)
    {
        std::vector<int32_t> sorted = m_selectedCustomLights;
        std::sort(sorted.begin(), sorted.end(), std::greater<int32_t>());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        size_t removed = 0;
        for (int32_t idx : sorted)
        {
            if (idx >= 0 && size_t(idx) < m_customLights.size())
            {
                m_customLights.erase(m_customLights.begin() + std::ptrdiff_t(idx));
                removed++;
            }
        }
        m_selectedCustomLights.clear();
        m_selectedCustomLight = -1;
        FinishGizmoDrag(false);
        FinishLightGrab(false);
        m_customPlacing = false;
        m_customCloning = -1;
        m_hoverCustomLight = -1;
        if (m_selectedLights.empty())
        {
            ClearSelection();
        }
        else
        {
            SyncPrimaryLightSelection();
        }
        PrintMessage(("removed " + std::to_string(removed) + " custom lights").c_str(), this);
        return;
    }

    if (m_selectedCustomLight < 0 || size_t(m_selectedCustomLight) >= m_customLights.size())
    {
        PrintMessage("remove: a map light belongs to the level, pick a custom one", this);
        return;
    }

    const size_t index = size_t(m_selectedCustomLight);
    const std::string label = "custom " + std::to_string(index);

    m_customLights.erase(m_customLights.begin() + std::ptrdiff_t(index));
    m_selectedCustomLights.clear();
    m_selectedCustomLight = -1;
    FinishGizmoDrag(false);
    FinishLightGrab(false);
    m_customPlacing = false;
    m_customCloning = -1;
    m_hoverCustomLight = -1;
    if (m_selectedLights.empty())
    {
        // The lights past the removed one shift down, so an index that named one of them would now
        // silently name its neighbour: drop the selection rather than point at a different light.
        ClearSelection();
    }
    else
    {
        SyncPrimaryLightSelection();
    }

    PrintMessage((label + ": removed").c_str(), this);
}

void EditorApp::SelectLight(int32_t index, bool custom)
{
    const std::vector<EditorLight> &lights = custom ? m_customLights : m_scene.lights;

    if (index < 0 || size_t(index) >= lights.size())
    {
        return;
    }

    m_selectedLight = custom ? -1 : index;
    m_selectedCustomLight = custom ? index : -1;
    m_selectedLights.clear();
    m_selectedCustomLights.clear();
    if (custom)
    {
        m_selectedCustomLights.push_back(index);
    }
    else
    {
        m_selectedLights.push_back(index);
    }
    m_selectedSurface = -1;
    m_selectedFace = -1;

    const EditorLight &light = lights[size_t(index)];

    m_hasPickPoint = true;
    m_pickCluster = light.cluster;

    for (int axis = 0; axis < 3; axis++)
    {
        m_pickPoint[axis] = light.position[axis];
    }
}

size_t EditorApp::TotalSelectedLights() const
{
    return m_selectedLights.size() + m_selectedCustomLights.size();
}

bool EditorApp::HasMultiLightSelection() const
{
    return TotalSelectedLights() > 1u;
}

bool EditorApp::IsLightSelected(int32_t index, bool custom) const
{
    if (custom)
    {
        return m_selectedCustomLight == index ||
               std::find(m_selectedCustomLights.begin(), m_selectedCustomLights.end(), index) !=
                   m_selectedCustomLights.end();
    }
    return m_selectedLight == index ||
           std::find(m_selectedLights.begin(), m_selectedLights.end(), index) !=
               m_selectedLights.end();
}

void EditorApp::SyncPrimaryLightSelection()
{
    m_selectedLights.erase(
        std::remove_if(m_selectedLights.begin(), m_selectedLights.end(),
                       [this](int32_t idx) {
                           return idx < 0 || size_t(idx) >= m_scene.lights.size();
                       }),
        m_selectedLights.end());
    m_selectedCustomLights.erase(
        std::remove_if(m_selectedCustomLights.begin(), m_selectedCustomLights.end(),
                       [this](int32_t idx) {
                           return idx < 0 || size_t(idx) >= m_customLights.size();
                       }),
        m_selectedCustomLights.end());

    m_selectedLight = m_selectedLights.empty() ? -1 : m_selectedLights.front();
    m_selectedCustomLight =
        m_selectedCustomLights.empty() ? -1 : m_selectedCustomLights.front();
}

LightProjectionCamera EditorApp::MarqueeCamera() const
{
    const EditorCameraBasis basis = CameraBasis();
    LightProjectionCamera cam;
    std::copy_n(basis.origin, 3, cam.origin);
    std::copy_n(basis.forward, 3, cam.forward);
    std::copy_n(basis.right, 3, cam.right);
    std::copy_n(basis.up, 3, cam.up);
    cam.tanHalfFovX = basis.tanHalfFovX;
    cam.tanHalfFovY = basis.tanHalfFovY;
    cam.viewportWidth = basis.viewportWidth;
    cam.viewportHeight = basis.viewportHeight;
    cam.visibleMinX = 0.0f;
    cam.visibleMinY = std::max(0.0f, m_menuBarBottom);
    cam.visibleMaxX = (m_panelVisible && m_panelLeft > 0.0f) ? m_panelLeft : basis.viewportWidth;
    cam.visibleMaxY = basis.viewportHeight;
    return cam;
}

void EditorApp::CancelMarqueeSelection()
{
    const bool releaseCapture = m_marqueePending && !m_looking;
    m_marqueePending = false;
    m_marqueeDragging = false;
    m_marqueeAdditive = false;
    if (releaseCapture)
    {
        m_window.SetMouseCapture(false);
    }
}

void EditorApp::CommitMarqueeSelection()
{
    const LightProjectionCamera cam = MarqueeCamera();
    const MarqueeSelectionQuery query{
        m_marqueeStart[0], m_marqueeStart[1], m_marqueeEnd[0], m_marqueeEnd[1],
        m_marqueeAdditive, &m_selectedLights,  &m_selectedCustomLights,
    };
    const MarqueeLightSelectionResult res =
        SelectVisibleLightsInMarquee(cam, m_scene.lights, m_customLights, query);

    m_selectedSurface = -1;
    m_selectedFace = -1;
    m_selectedLights = res.mapIndices;
    m_selectedCustomLights = res.customIndices;
    SyncPrimaryLightSelection();

    const size_t total = TotalSelectedLights();
    if (total == 0)
    {
        ClearSelection();
        PrintMessage("marquee: no visible lights in selection box", this);
        return;
    }

    if (m_selectedLights.empty() && !m_selectedCustomLights.empty())
    {
        m_lightTab = 1;
    }
    else if (!m_selectedLights.empty() && m_selectedCustomLights.empty())
    {
        m_lightTab = 0;
    }
    else if (m_lightTab == 2)
    {
        m_lightTab = 0;
    }

    if (const EditorLight *first = SelectedLight())
    {
        m_hasPickPoint = true;
        m_pickCluster = first->cluster;
        std::copy_n(first->position, 3, m_pickPoint);
    }

    PrintMessage(("marquee: selected " + std::to_string(total) + " dlight" +
                  (total == 1 ? "" : "s"))
                     .c_str(),
                 this);
}

void EditorApp::ResetMultiSelectedLightsToBaseline()
{
    const std::vector<int32_t> selectedLights = m_selectedLights;
    const std::vector<int32_t> selectedCustomLights = m_selectedCustomLights;
    bool anyEmitterReset = false;
    for (int32_t idx : selectedLights)
    {
        if (idx < 0 || size_t(idx) >= m_scene.lights.size())
        {
            continue;
        }
        const EditorLight &light = m_scene.lights[size_t(idx)];
        if (light.emitterUniqueID == 0)
        {
            if (const EditorLight *base =
                    BaselineMapLightAt(idx, m_scene.lights, m_baselineMapLights))
            {
                m_scene.lights[size_t(idx)] = *base;
            }
        }
        else if (ResetSingleEmitterOverride(light.className, light.emitterUniqueID,
                                            m_emitterLightOverrides, m_baselineEmitterOverrides))
        {
            anyEmitterReset = true;
        }
    }
    if (anyEmitterReset)
    {
        ReapplyGeneratedLights();
    }
    if (m_customLights.size() == m_baselineCustomLights.size())
    {
        for (int32_t idx : selectedCustomLights)
        {
            if (idx >= 0 && size_t(idx) < m_customLights.size())
            {
                m_customLights[size_t(idx)] = m_baselineCustomLights[size_t(idx)];
            }
        }
    }
    ClearDirtyFlagIfClean();
}

namespace
{
struct GizmoHoverQuery
{
    EditorApp::EditorMode mode = EditorApp::EditorMode::Material;
    int32_t selectedCustomLight = -1;
    bool dragging = false;
    bool dragDir = false;
    int32_t dragAxis = -1;
    bool grabDragging = false;
    bool uiWantsMouse = false;
    ImVec2 cursor = {};
};

void ResolvePanelGizmoHover(const EditorCameraBasis &cam, const std::vector<EditorLight> &customLights,
                            const GizmoHoverQuery &q, EditorApp::GizmoPart &outPart,
                            int32_t &outAxis)
{
    if (q.mode != EditorApp::EditorMode::Light || q.selectedCustomLight < 0 ||
        size_t(q.selectedCustomLight) >= customLights.size())
    {
        return;
    }

    if (q.dragging)
    {
        outPart = q.dragDir ? EditorApp::GizmoPart::SpotArc : EditorApp::GizmoPart::Axis;
        outAxis = q.dragAxis;
        return;
    }

    if (q.grabDragging)
    {
        outPart = EditorApp::GizmoPart::Center;
        outAxis = -1;
        return;
    }

    if (!q.uiWantsMouse)
    {
        const GizmoHitResult hit =
            HitTestCustomLightGizmo(cam, customLights[size_t(q.selectedCustomLight)], q.cursor);
        outPart = hit.part;
        outAxis = hit.axis;
    }
}
bool ShouldSkipSceneHoverPick(bool choosingMode, bool hasModal, bool modelPreviewActive)
{
    return choosingMode || hasModal || modelPreviewActive;
}

const char *ActiveViewportDragHint(bool grabDragging, bool gizmoDragging, bool modelOrbitMode)
{
    if (grabDragging)
    {
        return "Release LMB to drop the light, Esc to return it";
    }
    if (gizmoDragging)
    {
        return "Release LMB to keep the move, Esc to return it";
    }
    return modelOrbitMode ? "LMB drag - rotate model   Wheel - zoom" : nullptr;
}
} // namespace

void EditorApp::UpdateHoverPick()
{
    m_hoverSurface = -1;
    m_hoverFace = -1;
    m_hoverLight = -1;
    m_hoverCustomLight = -1;
    m_hoverGizmoPart = GizmoPart::None;
    m_hoverGizmoAxis = -1;

    if (ShouldSkipSceneHoverPick(m_choosingMode, HasModalOpen(), IsModelPreviewActive()))
    {
        return;
    }

    if (m_placingSun)
    {
        float sunRay[3] = {};
        if (CursorDirection(sunRay))
        {
            ApplySunDirectionFromRay(sunRay, m_sun);
        }
        return;
    }

    if (m_showPanel)
    {
        const GizmoHoverQuery q{
            m_editorMode,  m_selectedCustomLight, m_gizmoDragging, m_gizmoDragDir,
            m_gizmoAxis,   m_grabDragging,        m_ui.WantsMouse(),
            ImVec2(m_ui.MouseX(), m_ui.MouseY()),
        };
        ResolvePanelGizmoHover(CameraBasis(), m_customLights, q, m_hoverGizmoPart, m_hoverGizmoAxis);
        return;
    }

    if (m_scene.surfaces.empty() && m_scene.lights.empty() && m_customLights.empty())
    {
        return;
    }

    float direction[3] = {};

    if (!CursorDirection(direction))
    {
        return;
    }

    if (m_editorMode == EditorMode::Material)
    {
        const EditorFacePick facePick =
            PickSceneFace(m_scene, m_cameraPosition, direction, &m_materials);
        if (facePick.surface >= 0)
        {
            m_hoverSurface = facePick.surface;
            m_hoverFace = facePick.face;
        }
        return;
    }

    float lightDistance = 0.0f;
    bool lightCustom = false;
    const int32_t light = PickLight(m_cameraPosition, direction, lightDistance, lightCustom);
    if (light >= 0)
    {
        m_hoverLight = lightCustom ? -1 : light;
        m_hoverCustomLight = lightCustom ? light : -1;
    }
}

void EditorApp::Pick()
{
    if (m_choosingMode ||
        (m_scene.surfaces.empty() && m_scene.lights.empty() && m_customLights.empty()))
    {
        return;
    }

    float direction[3] = {};

    if (!CursorDirection(direction))
    {
        return;
    }

    const EditorFacePick facePick =
        PickSceneFace(m_scene, m_cameraPosition, direction, &m_materials);

    if (m_editorMode == EditorMode::Light)
    {
        HandleLightModePick(direction, facePick.distance, facePick.surface >= 0);
        return;
    }

    m_selectedLight = -1;
    m_selectedCustomLight = -1;
    m_selectedLights.clear();
    m_selectedCustomLights.clear();
    m_selectedSurface = facePick.surface >= 0 ? facePick.surface : -1;
    m_selectedFace = facePick.surface >= 0 ? facePick.face : -1;
    m_pickCluster = 0;

    if (m_selectedSurface >= 0)
    {
        const EditorSurface &picked = m_scene.surfaces[size_t(m_selectedSurface)];
        const int32_t pickedMaterial = m_materials.IndexOf(picked.textureName);

        if (pickedMaterial >= 0)
        {
            m_selectedMaterial = pickedMaterial;
        }

        m_hasPickPoint = true;
        m_pickCluster = facePick.cluster;
        std::copy_n(facePick.point, 3, m_pickPoint);
    }
    else if (EditorLight *picked = SelectedLight())
    {
        m_hasPickPoint = true;
        m_pickCluster = picked->cluster;
        std::copy_n(picked->position, 3, m_pickPoint);
    }
    else
    {
        m_hasPickPoint = false;
    }

    if (m_selectedSurface < 0 && SelectedLight() == nullptr)
    {
        ClearSelection();
        PrintMessage("pick: nothing under the cursor", this);
        return;
    }

    if (m_selectedSurface < 0)
    {
        PrintMessage(("pick: " + SelectedLightLabel()).c_str(), this);
    }
    else
    {
        const EditorSurface &picked = m_scene.surfaces[size_t(m_selectedSurface)];

        PrintMessage(("pick: surface " + std::to_string(m_selectedSurface) + " face " +
                      std::to_string(m_selectedFace) + " texture " + picked.textureName +
                      " cluster " + std::to_string(m_pickCluster))
                         .c_str(),
                     this);
    }
}

namespace
{
bool FindAliasInstanceBounds(const EditorScene &scene, const EditorSurface &surface,
                             uint32_t aliasInstanceId, float outMin[3], float outMax[3])
{
    for (const EditorAliasInstance &inst : scene.aliasInstances)
    {
        if (inst.aliasInstanceId == aliasInstanceId && inst.hasBounds)
        {
            std::copy_n(inst.boundsMin, 3, outMin);
            std::copy_n(inst.boundsMax, 3, outMax);
            return true;
        }
    }

    bool hasPoint = false;
    for (const EditorSurfaceFace &f : surface.faces)
    {
        if (f.aliasInstanceId != aliasInstanceId)
        {
            continue;
        }
        for (const QrFloat3D &p : f.positions)
        {
            for (int axis = 0; axis < 3; axis++)
            {
                outMin[axis] = hasPoint ? std::min(outMin[axis], p.data[axis]) : p.data[axis];
                outMax[axis] = hasPoint ? std::max(outMax[axis], p.data[axis]) : p.data[axis];
            }
            hasPoint = true;
        }
    }
    return hasPoint;
}

void DrawSceneFaceOutline(ImDrawList *drawList, const EditorCameraBasis &cam,
                          const EditorScene &scene, int32_t surfaceIndex, int32_t faceIndex,
                          uint32_t color, float thickness)
{
    if (surfaceIndex < 0 || size_t(surfaceIndex) >= scene.surfaces.size())
    {
        return;
    }

    const EditorSurface &surface = scene.surfaces[size_t(surfaceIndex)];
    if (faceIndex < 0 || size_t(faceIndex) >= surface.faces.size())
    {
        return;
    }

    const EditorSurfaceFace &face = surface.faces[size_t(faceIndex)];
    if (face.aliasInstanceId != 0)
    {
        float boundsMin[3] = {};
        float boundsMax[3] = {};
        if (FindAliasInstanceBounds(scene, surface, face.aliasInstanceId, boundsMin, boundsMax))
        {
            EmitClippedAabbOutline(drawList, cam, boundsMin, boundsMax, color, thickness);
            return;
        }
    }

    EmitClippedPolygonOutline(drawList, cam, face, color, thickness);
}

bool ShouldDrawDtalLight(const EditorEmissiveLight &light, int32_t dtalDebug)
{
    if (dtalDebug == 1)
    {
        return !light.isAliasModel;
    }
    if (dtalDebug == 2)
    {
        return light.isAliasModel;
    }
    return dtalDebug == 3;
}

void DrawSingleDtalDebugPoly(ImDrawList *drawList, const EditorCameraBasis &cam,
                             const EditorEmissiveLight &light)
{
    const QrTexturedAreaLightUploadInfo &info = light.info;
    if (info.numVerts < 3 || info.numVerts > MAX_TEXTURED_AREA_LIGHT_VERTS)
    {
        return;
    }

    EditorSurfaceFace poly;
    std::copy_n(info.normal.data, 3, poly.normal);
    poly.positions.reserve(size_t(info.numVerts));
    for (int k = 0; k < info.numVerts; k++)
    {
        const float u = info.uvVerts[k].data[0];
        const float v = info.uvVerts[k].data[1];
        poly.positions.push_back(QrFloat3D{ {
            info.C.data[0] + info.A.data[0] * u + info.B.data[0] * v,
            info.C.data[1] + info.A.data[1] * u + info.B.data[1] * v,
            info.C.data[2] + info.A.data[2] * u + info.B.data[2] * v,
        } });
    }

    const uint32_t color =
        light.isAliasModel ? IM_COL32(96, 220, 255, 240) : IM_COL32(48, 164, 255, 240);
    EmitClippedPolygonOutline(drawList, cam, poly, color, 2.0f);

    const float tip[3] = {
        light.center[0] + 8.0f * info.normal.data[0],
        light.center[1] + 8.0f * info.normal.data[1],
        light.center[2] + 8.0f * info.normal.data[2],
    };
    float seg[4] = {};
    if (ProjectClippedEdge(cam, light.center, tip, seg))
    {
        drawList->AddLine(ImVec2(seg[0], seg[1]), ImVec2(seg[2], seg[3]),
                          IM_COL32(135, 215, 255, 240), 1.8f);
    }
}

void DrawDtalDebugOverlay(ImDrawList *drawList, const EditorCameraBasis &cam,
                          const std::vector<EditorEmissiveLight> &lights, int32_t dtalDebug)
{
    if (drawList == nullptr || dtalDebug <= 0)
    {
        return;
    }

    for (const EditorEmissiveLight &light : lights)
    {
        if (ShouldDrawDtalLight(light, dtalDebug))
        {
            DrawSingleDtalDebugPoly(drawList, cam, light);
        }
    }
}

bool IsSurfaceOutlineVisible(const EditorScene &scene, const EditorMaterialTable &materials,
                             int32_t surfaceIndex)
{
    return surfaceIndex >= 0 && size_t(surfaceIndex) < scene.surfaces.size() &&
           materials.For(scene.surfaces[size_t(surfaceIndex)]).enabled;
}

struct MaterialPickOutlineState
{
    bool hasPickPoint = false;
    bool showPanel = false;
    int32_t selectedSurface = -1;
    int32_t selectedFace = -1;
    int32_t hoverSurface = -1;
    int32_t hoverFace = -1;
};

void DrawMaterialPickOutlines(ImDrawList *drawList, const EditorCameraBasis &cam,
                              const EditorScene &scene, const EditorMaterialTable &materials,
                              const MaterialPickOutlineState &state)
{
    if (state.hasPickPoint && IsSurfaceOutlineVisible(scene, materials, state.selectedSurface))
    {
        DrawSceneFaceOutline(drawList, cam, scene, state.selectedSurface, state.selectedFace,
                             IM_COL32(255, 170, 40, 255), 2.5f);
    }
    if (!state.showPanel && IsSurfaceOutlineVisible(scene, materials, state.hoverSurface))
    {
        DrawSceneFaceOutline(drawList, cam, scene, state.hoverSurface, state.hoverFace,
                             IM_COL32(255, 214, 64, 255), 2.0f);
    }
}

void DrawViewportCrosshair(ImDrawList *draw, float width, float height)
{
    const ImVec2 centre(width * 0.5f, height * 0.5f);
    const float gap = 4.0f;
    const float length = 7.0f;
    const float thickness = 1.6f;
    const ImU32 shadow = IM_COL32(0, 0, 0, 130);
    const ImU32 colour = IM_COL32(255, 255, 255, 225);
    const ImVec2 from[4] = {
        ImVec2(centre.x - gap - length, centre.y),
        ImVec2(centre.x + gap, centre.y),
        ImVec2(centre.x, centre.y - gap - length),
        ImVec2(centre.x, centre.y + gap),
    };
    const ImVec2 to[4] = {
        ImVec2(centre.x - gap, centre.y),
        ImVec2(centre.x + gap + length, centre.y),
        ImVec2(centre.x, centre.y - gap),
        ImVec2(centre.x, centre.y + gap + length),
    };
    for (int tick = 0; tick < 4; tick++)
    {
        draw->AddLine(from[tick], to[tick], shadow, thickness + 2.0f);
        draw->AddLine(from[tick], to[tick], colour, thickness);
    }
}

void DrawViewportKeyHints(ImDrawList *draw, float height, bool notifyActive)
{
    static const char *const kHints[] = {
        "QR EDITOR",
        "LMB - pick face or light   (opens panel)",
        "WASD - fly   Space/Ctrl - up/down   Shift - x3",
        "Tab - panel / cursor   Home - reset camera   Ctrl+O - open level",
        "Esc - cancel / return to fly",
    };

    const int hintCount = int(std::size(kHints));
    const float padding = 14.0f;
    const float lineHeight = ImGui::GetTextLineHeight() + 3.0f;
    float y = height - padding - lineHeight * float(hintCount);
    if (notifyActive)
    {
        y -= lineHeight * float(hintCount);
    }
    for (int line = 0; line < hintCount; line++)
    {
        const ImU32 colour =
            line == 0 ? IM_COL32(140, 205, 255, 235) : IM_COL32(228, 234, 244, 220);
        draw->AddText(ImVec2(padding + 1.0f, y + 1.0f), IM_COL32(0, 0, 0, 160), kHints[line]);
        draw->AddText(ImVec2(padding, y), colour, kHints[line]);
        y += lineHeight;
    }
}
} // namespace

void EditorApp::DrawCustomLightGizmoAndLog(ImDrawList *drawList, const EditorCameraBasis &cam)
{
    const EditorLight &selected = m_customLights[size_t(m_selectedCustomLight)];
    float pos[3] = {};
    EffectiveLightPosition(selected, pos);

    float originX = 0.0f;
    float originY = 0.0f;
    if (!ProjectPoint(pos, originX, originY))
    {
        return;
    }

    const int32_t hoverAxis = (m_hoverGizmoPart == GizmoPart::Axis) ? m_hoverGizmoAxis : -1;
    DrawGizmoAxisArrows(drawList, cam, selected, pos, hoverAxis);

    if (selected.spot)
    {
        const ImU32 arcCol1 =
            (m_hoverGizmoPart == GizmoPart::SpotArc && m_hoverGizmoAxis == 1)
                ? kGizmoHoverColor
                : IM_COL32(64, 255, 64, 255);
        const ImU32 arcCol2 =
            (m_hoverGizmoPart == GizmoPart::SpotArc && m_hoverGizmoAxis == 2)
                ? kGizmoHoverColor
                : IM_COL32(64, 128, 255, 255);
        DrawSpotDirArc(drawList, cam, selected, pos, 1, arcCol1);
        DrawSpotDirArc(drawList, cam, selected, pos, 2, arcCol2);
    }

    std::string axes;
    for (int32_t axis = 0; axis < 3; axis++)
    {
        float adir[3] = {};
        ComputeGizmoAxis(selected, axis, adir);
        const float tip[3] = {
            pos[0] + kGizmoLengthUnits * adir[0],
            pos[1] + kGizmoLengthUnits * adir[1],
            pos[2] + kGizmoLengthUnits * adir[2],
        };
        float tipX = 0.0f;
        float tipY = 0.0f;
        if (ProjectPoint(tip, tipX, tipY))
        {
            axes += " " + std::to_string(axis) + " " + std::to_string(int32_t(originX)) + "," +
                    std::to_string(int32_t(originY)) + " " + std::to_string(int32_t(tipX)) + "," +
                    std::to_string(int32_t(tipY));
        }
    }

    const uint64_t layout = std::hash<std::string>{}(axes);
    if (!m_gizmoDragging && !m_grabDragging && m_loggedGizmoLayout != layout)
    {
        m_loggedGizmoLayout = layout;
        PrintMessage(("gizmo axes:" + axes).c_str(), this);
    }
}

void EditorApp::DrawSelectionOverlay()
{
    if (m_clientWidth == 0 || m_clientHeight == 0)
    {
        return;
    }

    ImDrawList *drawList = ImGui::GetBackgroundDrawList();
    if (drawList == nullptr)
    {
        return;
    }

    const EditorCameraBasis cam = CameraBasis();
    const bool showWorldExtras = !m_choosingMode && !IsModelPreviewActive();
    if (showWorldExtras && m_editorMode == EditorMode::Material)
    {
        const MaterialPickOutlineState state{
            m_hasPickPoint, m_showPanel, m_selectedSurface, m_selectedFace, m_hoverSurface,
            m_hoverFace,
        };
        DrawMaterialPickOutlines(drawList, cam, m_scene, m_materials, state);
    }

    if (showWorldExtras && m_settings.dtalDebug > 0)
    {
        DrawDtalDebugOverlay(drawList, cam, m_emissiveLights, m_settings.dtalDebug);
    }

    DrawLightEditorViewportExtras();

    if (m_choosingMode || m_editorMode != EditorMode::Light || HasMultiLightSelection() ||
        m_selectedCustomLight < 0 || size_t(m_selectedCustomLight) >= m_customLights.size())
    {
        return;
    }
    DrawCustomLightGizmoAndLog(drawList, cam);
}

void EditorApp::DrawViewportMarquee(ImDrawList *draw, float width, float height) const
{
    const float rightEdge = (m_panelVisible && m_panelLeft > 0.0f) ? m_panelLeft : width;
    const float topEdge = std::max(0.0f, m_menuBarBottom);
    const MarqueeRect rect =
        NormalizeMarqueeRect(m_marqueeStart[0], m_marqueeStart[1], m_marqueeEnd[0], m_marqueeEnd[1]);

    draw->PushClipRect(ImVec2(0.0f, topEdge), ImVec2(rightEdge, height), true);
    draw->AddRectFilled(ImVec2(rect.minX, rect.minY), ImVec2(rect.maxX, rect.maxY),
                        IM_COL32(78, 152, 232, 48));
    draw->AddRect(ImVec2(rect.minX, rect.minY), ImVec2(rect.maxX, rect.maxY),
                  IM_COL32(102, 182, 255, 225), 0.0f, 0,
                  std::max(1.2f, 1.5f * EditorGuiScale()));
    draw->PopClipRect();
}

void EditorApp::DrawViewportOverlay()
{
    if (m_clientWidth == 0 || m_clientHeight == 0)
    {
        return;
    }

    ImDrawList *draw = ImGui::GetForegroundDrawList();
    if (draw == nullptr)
    {
        return;
    }

    const float width = float(m_clientWidth);
    const float height = float(m_clientHeight);
    if (m_looking)
    {
        DrawViewportCrosshair(draw, width, height);
    }
    if (m_textureBrowserMaterial.empty())
    {
        DrawViewportKeyHints(draw, height, m_notifyActive);
    }

    const char *dragLine =
        ActiveViewportDragHint(m_grabDragging, m_gizmoDragging, IsModelOrbitMode());
    if (dragLine != nullptr)
    {
        const float padding = 14.0f * EditorGuiScale();
        const ImVec2 size = ImGui::CalcTextSize(dragLine);
        const float rightEdge = m_panelVisible ? m_panelLeft - padding : width - padding;
        const ImVec2 position(std::max(padding, rightEdge - size.x), height - padding - size.y);
        draw->AddText(ImVec2(position.x + 1.0f, position.y + 1.0f), IM_COL32(0, 0, 0, 160),
                      dragLine);
        draw->AddText(position, IM_COL32(255, 230, 150, 235), dragLine);
    }

    if (m_marqueeDragging && m_editorMode == EditorMode::Light && m_showPanel && !HasModalOpen())
    {
        DrawViewportMarquee(draw, width, height);
    }
}

bool EditorApp::ProjectPoint(const float point[3], float &outX, float &outY) const
{
    if (m_clientWidth == 0 || m_clientHeight == 0)
    {
        return false;
    }

    float side[3] = {};
    float up[3] = {};
    float forward[3] = {};
    ViewBasis(side, up, forward);

    const float aspect = float(m_clientWidth) / float(m_clientHeight);
    const float tanHalfFov = std::tan(0.5f * FovYRadians());
    const float relative[3] = {
        point[0] - m_cameraPosition[0],
        point[1] - m_cameraPosition[1],
        point[2] - m_cameraPosition[2],
    };

    const float depth = relative[0] * forward[0] + relative[1] * forward[1] + relative[2] * forward[2];

    if (depth <= 1e-3f)
    {
        return false;
    }

    const float horizontal = relative[0] * side[0] + relative[1] * side[1] + relative[2] * side[2];
    const float vertical = relative[0] * up[0] + relative[1] * up[1] + relative[2] * up[2];

    const float ndcX = horizontal / (depth * tanHalfFov * aspect);
    const float ndcY = vertical / (depth * tanHalfFov);

    outX = (ndcX * 0.5f + 0.5f) * float(m_clientWidth);
    outY = (1.0f - (ndcY * 0.5f + 0.5f)) * float(m_clientHeight);
    return true;
}

bool EditorApp::BeginGizmoDrag(float cursorX, float cursorY)
{
    if (m_selectedCustomLight < 0 || m_selectedCustomLight >= int32_t(m_customLights.size()))
    {
        return false;
    }

    const EditorLight &light = m_customLights[size_t(m_selectedCustomLight)];
    const GizmoHitResult hit = HitTestCustomLightGizmo(CameraBasis(), light, ImVec2(cursorX, cursorY));

    if (hit.part == GizmoPart::Center || hit.part == GizmoPart::None)
    {
        return false;
    }

    m_gizmoDragging = true;
    m_gizmoDragDir = (hit.part == GizmoPart::SpotArc);
    m_gizmoAxis = hit.axis;
    m_gizmoStart[0] = cursorX;
    m_gizmoStart[1] = cursorY;
    ComputeGizmoAxis(light, hit.axis, m_gizmoAxisVector);

    if (m_gizmoDragDir)
    {
        std::copy_n(light.direction, 3, m_gizmoDirStart);
        const float lenSq = m_gizmoDirStart[0] * m_gizmoDirStart[0] +
                            m_gizmoDirStart[1] * m_gizmoDirStart[1] +
                            m_gizmoDirStart[2] * m_gizmoDirStart[2];
        if (lenSq <= 1e-8f)
        {
            m_gizmoDirStart[0] = 1.0f;
            m_gizmoDirStart[1] = 0.0f;
            m_gizmoDirStart[2] = 0.0f;
        }
        PrintMessage(("gizmo: rotate custom " + std::to_string(m_selectedCustomLight) + " axis " +
                      std::to_string(m_gizmoAxis)).c_str(), this);
        return true;
    }

    m_gizmoDirection[0] = hit.dir[0];
    m_gizmoDirection[1] = hit.dir[1];
    m_gizmoPixelsPerUnit = hit.pixelsPerUnit;
    std::copy_n(light.position, 3, m_gizmoOrigin);

    PrintMessage(("gizmo: grab custom " + std::to_string(m_selectedCustomLight) + " axis " +
                  std::to_string(m_gizmoAxis) + " from " +
                  std::to_string(int32_t(m_gizmoOrigin[0])) + " " +
                  std::to_string(int32_t(m_gizmoOrigin[1])) + " " +
                  std::to_string(int32_t(m_gizmoOrigin[2]))).c_str(), this);
    return true;
}

void EditorApp::UpdateGizmoDrag(float cursorX, float cursorY)
{
    if (!m_gizmoDragging || m_selectedCustomLight < 0 ||
        m_selectedCustomLight >= int32_t(m_customLights.size()))
    {
        return;
    }

    EditorLight &light = m_customLights[size_t(m_selectedCustomLight)];

    if (m_gizmoDragDir)
    {
        float side[3] = {};
        float up[3] = {};
        float forward[3] = {};
        ViewBasis(side, up, forward);

        float deltaDeg = ((m_gizmoAxis == 1) ? (cursorY - m_gizmoStart[1])
                                             : (cursorX - m_gizmoStart[0])) * 0.5f;
        const float dot = forward[0] * m_gizmoAxisVector[0] +
                          forward[1] * m_gizmoAxisVector[1] +
                          forward[2] * m_gizmoAxisVector[2];
        if (dot < 0.0f)
        {
            deltaDeg = -deltaDeg;
        }
        RotateAroundAxis(m_gizmoAxisVector, m_gizmoDirStart, deltaDeg, light.direction);
        return;
    }

    if (m_gizmoPixelsPerUnit <= 0.0f)
    {
        return;
    }

    // How far the cursor travelled along the axis' frozen screen direction, in world units.
    const float travelled = (cursorX - m_gizmoStart[0]) * m_gizmoDirection[0] +
                            (cursorY - m_gizmoStart[1]) * m_gizmoDirection[1];

    // Whole units, like the game's QRE_CustomGizmoMove: the light stays on the integer grid the
    // map's own entities sit on while the cursor moves by fractions of one arrow length.
    const float delta = std::round(travelled / m_gizmoPixelsPerUnit);

    for (int axis = 0; axis < 3; axis++)
    {
        light.position[axis] = m_gizmoOrigin[axis] + delta * m_gizmoAxisVector[axis];
    }
    light.cluster = ResolveEditorLightCluster(m_scene, light.position);
}

void EditorApp::FinishGizmoDrag(bool kept)
{
    if (!m_gizmoDragging)
    {
        return;
    }

    const bool wasDir = m_gizmoDragDir;
    m_gizmoDragging = false;
    m_gizmoDragDir = false;

    if (m_selectedCustomLight < 0 || m_selectedCustomLight >= int32_t(m_customLights.size()))
    {
        return;
    }

    EditorLight &light = m_customLights[size_t(m_selectedCustomLight)];
    const std::string label = "custom " + std::to_string(m_selectedCustomLight);

    if (wasDir)
    {
        if (!kept)
        {
            std::copy_n(m_gizmoDirStart, 3, light.direction);
            PrintMessage((label + ": rotation cancelled").c_str(), this);
        }
        else
        {
            PrintMessage((label + ": rotated spot dir").c_str(), this);
        }
        return;
    }

    if (!kept)
    {
        // Escape or a lost button puts the light back where the press found it, the way
        // QRE_CustomGizmoCancel restores the origin.
        for (int axis = 0; axis < 3; axis++)
        {
            light.position[axis] = m_gizmoOrigin[axis];
        }

        light.cluster = ResolveEditorLightCluster(m_scene, light.position);
        PrintMessage((label + ": gizmo cancelled").c_str(), this);
        LogLightCluster(light, CustomLightUniqueID(size_t(m_selectedCustomLight)), label.c_str());
        return;
    }

    PrintMessage((label + ": gizmo moved to " + std::to_string(int32_t(light.position[0])) + " " +
                  std::to_string(int32_t(light.position[1])) + " " +
                  std::to_string(int32_t(light.position[2])) + " axis " +
                  std::to_string(m_gizmoAxis) + " cluster " + std::to_string(light.cluster))
                     .c_str(),
                 this);
    LogLightCluster(light, CustomLightUniqueID(size_t(m_selectedCustomLight)), label.c_str());
}

bool EditorApp::CursorDirection(float outDirection[3]) const
{
    if (m_clientWidth == 0 || m_clientHeight == 0)
    {
        return false;
    }

    float side[3] = {};
    float up[3] = {};
    float forward[3] = {};
    ViewBasis(side, up, forward);

    // A click while the mouse is captured comes from the crosshair, so it uses the centre of the
    // viewport; otherwise it comes from the cursor.
    float cursorX = float(m_clientWidth) * 0.5f;
    float cursorY = float(m_clientHeight) * 0.5f;

    if (!m_looking)
    {
        cursorX = m_ui.MouseX();
        cursorY = m_ui.MouseY();
    }

    const float aspect = float(m_clientWidth) / float(m_clientHeight);
    const float tanHalfFov = std::tan(0.5f * FovYRadians());
    const float ndcX = (cursorX / float(m_clientWidth)) * 2.0f - 1.0f;
    const float ndcY = 1.0f - (cursorY / float(m_clientHeight)) * 2.0f;

    outDirection[0] = forward[0] + side[0] * (ndcX * tanHalfFov * aspect) + up[0] * (ndcY * tanHalfFov);
    outDirection[1] = forward[1] + side[1] * (ndcX * tanHalfFov * aspect) + up[1] * (ndcY * tanHalfFov);
    outDirection[2] = forward[2] + side[2] * (ndcX * tanHalfFov * aspect) + up[2] * (ndcY * tanHalfFov);

    const float length = std::sqrt(outDirection[0] * outDirection[0] +
                                   outDirection[1] * outDirection[1] +
                                   outDirection[2] * outDirection[2]);

    if (length <= 1e-6f)
    {
        return false;
    }

    for (int axis = 0; axis < 3; axis++)
    {
        outDirection[axis] /= length;
    }

    return true;
}

bool EditorApp::IsCursorNearProjectedLight(const EditorLight &light) const
{
    float origin[3] = {};
    EffectiveLightPosition(light, origin);
    float sx = 0.0f;
    float sy = 0.0f;
    return ProjectPoint(origin, sx, sy) &&
           std::hypot(m_ui.MouseX() - sx, m_ui.MouseY() - sy) <= kGizmoCenterGrabPixels;
}

bool EditorApp::ResolveGrabTargetLight(int32_t &outIndex, bool &outCustom) const
{
    if (!m_looking && m_selectedCustomLight >= 0 &&
        size_t(m_selectedCustomLight) < m_customLights.size() &&
        IsCursorNearProjectedLight(m_customLights[size_t(m_selectedCustomLight)]))
    {
        outIndex = m_selectedCustomLight;
        outCustom = true;
        return true;
    }

    float direction[3] = {};
    if (!CursorDirection(direction))
    {
        return false;
    }

    float distance = 0.0f;
    outIndex = PickLight(m_cameraPosition, direction, distance, outCustom);
    if (outIndex < 0 || (!m_looking && outCustom && outIndex == m_selectedCustomLight))
    {
        return false;
    }

    if (m_looking)
    {
        return true;
    }
    const std::vector<EditorLight> &lights = outCustom ? m_customLights : m_scene.lights;
    return size_t(outIndex) < lights.size() && IsCursorNearProjectedLight(lights[size_t(outIndex)]);
}

bool EditorApp::BeginLightGrab()
{
    if (m_editorMode != EditorMode::Light)
    {
        return false;
    }

    if (!m_looking && (HasMultiLightSelection() || m_keys[size_t(EditorKey::Ctrl)] ||
                       m_keys[size_t(EditorKey::Shift)]))
    {
        return false;
    }

    int32_t index = -1;
    bool custom = false;
    if (!ResolveGrabTargetLight(index, custom))
    {
        return false;
    }

    SelectLight(index, custom);
    const EditorLight *light = SelectedLight();
    if (light == nullptr)
    {
        return false;
    }

    m_grabDragging = true;
    m_grabMoved = false;
    m_grabCustom = custom;
    m_grabIndex = index;
    m_grabStart[0] = m_ui.MouseX();
    m_grabStart[1] = m_ui.MouseY();
    for (int axis = 0; axis < 3; axis++)
    {
        m_grabOrigin[axis] = light->position[axis];
        m_grabLoggedPosition[axis] = light->position[axis];
    }

    PrintMessage(("grab: " + SelectedLightLabel() + " at " + std::to_string(int32_t(light->position[0])) +
                  " " + std::to_string(int32_t(light->position[1])) + " " +
                  std::to_string(int32_t(light->position[2])))
                     .c_str(),
                 this);
    return true;
}

namespace
{
void PlaceLightAlongGrabRay(const EditorScene &scene, const EditorMaterialTable &materials,
                            const float camOrigin[3], const float direction[3], EditorLight &light)
{
    const EditorFacePick facePick = PickSceneFace(scene, camOrigin, direction, &materials);
    const float distance = (facePick.surface >= 0) ? (facePick.distance - kGrabSurfaceOffsetUnits)
                                                   : kGrabAirDistanceUnits;

    for (int axis = 0; axis < 3; axis++)
    {
        const float target = camOrigin[axis] + direction[axis] * distance;
        light.position[axis] = target - (light.hasOffset ? light.offset[axis] : 0.0f);
    }

    light.cluster = ResolveEditorLightCluster(scene, light.position);
}
} // namespace

void EditorApp::UpdateLightGrab()
{
    if (!m_grabDragging)
    {
        return;
    }

    if (!m_grabCustom && (m_grabIndex < 0 || m_grabIndex >= int32_t(m_scene.lights.size())))
    {
        return;
    }

    if (m_grabCustom && (m_grabIndex < 0 || m_grabIndex >= int32_t(m_customLights.size())))
    {
        return;
    }

    // A press that has not travelled yet is still a click, so it selects the light without moving
    // it: the game separates the two because a grab happens on a different button than a pick,
    // and a standalone editor has only the one the author presses.
    if (!m_looking)
    {
        const float travel = std::abs(m_ui.MouseX() - m_grabStart[0]) +
                             std::abs(m_ui.MouseY() - m_grabStart[1]);

        if (travel < kGizmoClickPixels)
        {
            return;
        }
    }

    float direction[3] = {};

    if (!CursorDirection(direction))
    {
        return;
    }

    m_grabMoved = true;

    EditorLight *light = SelectedLight();

    if (light == nullptr)
    {
        return;
    }

    PlaceLightAlongGrabRay(m_scene, m_materials, m_cameraPosition, direction, *light);

    // The drag lands on a new position every frame it moves; the log carries the ones that
    // changed, so a scripted drag can read where the light ended up.
    float moved = 0.0f;

    for (int axis = 0; axis < 3; axis++)
    {
        moved = std::max(moved, std::abs(light->position[axis] - m_grabLoggedPosition[axis]));
    }

    if (moved >= 1.0f)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            m_grabLoggedPosition[axis] = light->position[axis];
        }

        PrintMessage((SelectedLightLabel() + ": drag to " + std::to_string(int32_t(light->position[0])) +
                      " " + std::to_string(int32_t(light->position[1])) + " " +
                      std::to_string(int32_t(light->position[2])) + " cluster " +
                      std::to_string(light->cluster))
                         .c_str(),
                     this);
    }
}

void EditorApp::FinishLightGrab(bool kept)
{
    if (!m_grabDragging)
    {
        return;
    }

    m_grabDragging = false;

    EditorLight *light = SelectedLight();

    if (light == nullptr)
    {
        return;
    }

    const std::string label = SelectedLightLabel();

    if (!kept && m_grabMoved)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            light->position[axis] = m_grabOrigin[axis];
        }

        light->cluster = ResolveEditorLightCluster(m_scene, light->position);
        PrintMessage((label + ": drag cancelled").c_str(), this);
        LogLightCluster(*light, SelectedLightUniqueID(), label.c_str());
        return;
    }

    if (!m_grabMoved)
    {
        return;
    }

    PrintMessage((label + ": dropped at " + std::to_string(int32_t(light->position[0])) + " " +
                  std::to_string(int32_t(light->position[1])) + " " +
                  std::to_string(int32_t(light->position[2])) + " cluster " +
                  std::to_string(light->cluster))
                     .c_str(),
                 this);
    LogLightCluster(*light, SelectedLightUniqueID(), label.c_str());
}

void EditorApp::DrawLightList()
{
    const bool hasSelection = SelectedLight() != nullptr;
    ImGui::Text("Map lights     %d", int32_t(m_scene.lights.size()));
    ImGui::Text("Custom lights  %d", int32_t(m_customLights.size()));

    {
        if (!m_hasPickPoint)
        {
            ImGui::TextUnformatted("Nothing picked: a light would land in front of the camera");
        }

        // Button rects are logged below so a scripted session can press them by name.
        std::string rects;

        auto appendRect = [&rects](const char *name) {
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();

            rects += " ";
            rects += name;
            rects += " " + std::to_string(int32_t(min.x)) + "," + std::to_string(int32_t(min.y)) + " " +
                     std::to_string(int32_t(max.x)) + "," + std::to_string(int32_t(max.y));
        };

        if (ImGui::Button("Add light"))
        {
            AddCustomLight(nullptr, LightSpawn::Camera);
        }

        appendRect("add");

        ImGui::SameLine();
        ImGui::BeginDisabled(!hasSelection);

        if (ImGui::Button("Duplicate"))
        {
            DuplicateSelectedLight();
        }

        appendRect("duplicate");

        ImGui::SameLine();

        if (ImGui::Button("Clone"))
        {
            CloneSelectedLight();
        }

        appendRect("clone");

        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(m_selectedCustomLight < 0);

        if (ImGui::Button("Remove"))
        {
            RemoveSelectedCustomLight();
        }

        appendRect("remove");

        ImGui::EndDisabled();

        auto appendRowRect = [&appendRect](int32_t index) {
            const std::string name = "row" + std::to_string(index);
            appendRect(name.c_str());
        };

        ImGui::Separator();

        auto drawRow = [this, &appendRowRect](int32_t index, bool custom) {
            const std::vector<EditorLight> &lights = custom ? m_customLights : m_scene.lights;
            const EditorLight &light = lights[size_t(index)];

            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            const std::string rowLabel = (custom ? "custom " : "light ") + std::to_string(index);
            const bool selected = custom ? m_selectedCustomLight == index : m_selectedLight == index;

            if (ImGui::Selectable(rowLabel.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
            {
                SelectLight(index, custom);
                PrintMessage((SelectedLightLabel() + ": selected from the list").c_str(), this);
            }

            if (custom && index < 8)
            {
                appendRowRect(index);
            }

            ImGui::TableNextColumn();
            ImGui::Text("%.0f %.0f %.0f", light.position[0], light.position[1], light.position[2]);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", light.radius);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(EditorStyleName(light.style));
            ImGui::TableNextColumn();
            ImGui::Text("%u", light.cluster);
        };

        auto drawTable = [&drawRow](const char *id, const char *childId, float height,
                                    const std::vector<EditorLight> &lights, bool custom) {
            if (!ImGui::BeginChild(childId, ImVec2(0.0f, height), ImGuiChildFlags_Borders))
            {
                ImGui::EndChild();
                return;
            }

            const ImGuiTableFlags tableFlags =
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;

            if (ImGui::BeginTable(id, 5, tableFlags))
            {
                ImGui::TableSetupColumn("Light");
                ImGui::TableSetupColumn("Position");
                ImGui::TableSetupColumn("Radius");
                ImGui::TableSetupColumn("Style");
                ImGui::TableSetupColumn("Cluster");
                ImGui::TableHeadersRow();

                // A map carries hundreds of lights; only the visible rows are submitted.
                ImGuiListClipper clipper;
                clipper.Begin(int32_t(lights.size()));

                while (clipper.Step())
                {
                    for (int32_t index = clipper.DisplayStart; index < clipper.DisplayEnd; index++)
                    {
                        drawRow(index, custom);
                    }
                }

                ImGui::EndTable();
            }

            ImGui::EndChild();
        };

        ImGui::TextUnformatted("Map");
        drawTable("MapLights", "MapLightsChild", 190.0f, m_scene.lights, false);

        ImGui::TextUnformatted("Custom");
        drawTable("CustomLights", "CustomLightsChild", 150.0f, m_customLights, true);

        const uint64_t layout = std::hash<std::string>{}(rects) | 1u;

        if (m_loggedLightListLayout != layout)
        {
            m_loggedLightListLayout = layout;
            PrintMessage(("light list rect:" + rects).c_str(), this);
        }
    }
}

namespace
{
    // Opens a tab and records its rect straight away: ImGui has just submitted the tab's own item,
    // so this is the only place the rect is still readable. `clickedTab` names the tab that took a
    // left click this frame, whichever one is open afterwards.
    bool BeginLoggedTab(const char *label, ImGuiTabItemFlags flags, std::string &rects,
                        std::string &clickedTab)
    {
        const bool open = ImGui::BeginTabItem(label, nullptr, flags);

        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            clickedTab = label;
        }

        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();

        rects += " ";
        rects += label;
        rects += " " + std::to_string(int32_t(min.x)) + "," + std::to_string(int32_t(min.y)) + " " +
                 std::to_string(int32_t(max.x)) + "," + std::to_string(int32_t(max.y));

        return open;
    }

    void ConfigureRightDockedPanel(uint32_t clientWidth, uint32_t clientHeight)
    {
        const float maxWidth = std::max(240.0f, float(clientWidth) * 0.50f);
        const float panelWidth = std::min(std::max(388.0f, float(clientWidth) * 0.275f), maxWidth);

        ImGui::SetNextWindowPos(ImVec2(float(clientWidth) - panelWidth, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(panelWidth, float(clientHeight)), ImGuiCond_Always);
    }

    ImGuiTabItemFlags TabFlagsForLabel(const char *name, std::string &requestedTab,
                                       const std::string &initialTab, bool &initialTabApplied)
    {
        if (!requestedTab.empty() && requestedTab == name)
        {
            requestedTab.clear();
            return ImGuiTabItemFlags_SetSelected;
        }

        if (!initialTabApplied && initialTab == name)
        {
            initialTabApplied = true;
            return ImGuiTabItemFlags_SetSelected;
        }

        return ImGuiTabItemFlags_None;
    }
}

void EditorApp::DrawPanel(float deltaSeconds)
{
    m_panelVisible = false;

    if (!m_showPanel)
    {
        return;
    }

    ConfigureRightDockedPanel(m_clientWidth, m_clientHeight);

    // Tabs keep every group reachable: the panel is a fixed height now that there are
    // more sections than fit on a 720p screen at once.
    if (ImGui::Begin("QuakeRay Editor", &m_showPanel,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoCollapse))
    {
        // The overlay anchors its hint block and drag line to the panel's left edge, so the rect is
        // taken after Begin has laid the window out.
        const ImVec2 panelPosition = ImGui::GetWindowPos();

        m_panelLeft = panelPosition.x;
        m_panelVisible = true;

        const float fps = deltaSeconds > 0.0f ? 1.0f / deltaSeconds : 0.0f;
        ImGui::Text("Frame  %.2f ms  (%.0f FPS)", m_frameTimeMs, fps);
        ImGui::Text("Drawable  %ux%u", m_clientWidth, m_clientHeight);
        // Above the tabs, so the save controls never depend on how tall the open tab is.
        DrawSessionBar();

        // Tab rects are logged so a scripted session can click a neighbouring tab instead of
        // guessing where ImGui laid the bar out.
        std::string tabRects;
        // The game's QR_GUI_Tabs reports a click on the tab that is already open as a reset
        // request; the label open last frame is what tells the two apart.
        const std::string previousTab = m_activePanelTab;
        std::string clickedTab;

        if (ImGui::BeginTabBar("EditorTabs"))
        {
            // --tab <name> opens that tab on the first frame only, so later clicks behave as usual.
            auto tabFlags = [this](const char *name) -> ImGuiTabItemFlags {
                return TabFlagsForLabel(name, m_requestedTab, m_initialTab, m_initialTabApplied);
            };

            if (BeginLoggedTab("Scene", tabFlags("Scene"), tabRects, clickedTab))
            {
                m_activePanelTab = "Scene";
                // The selection, light and sun blocks are taller than a 720p window once the
                // panel is fixed at 660, so the tab scrolls and the key hints stay visible.
                const float footerHeight =
                    ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y;
                ImGui::BeginChild("SceneParams", ImVec2(0.0f, -footerHeight), false);

                ImGui::Separator();
                ImGui::TextUnformatted("Scene");
                ImGui::Text("Vertices  %zu", m_scene.vertices.size());
                ImGui::Text("Triangles %zu", m_scene.indices.size() / 3);
                ImGui::Text("Surfaces  %zu  Textures %zu", m_scene.surfaces.size(),
                            m_scene.textures.size());
                ImGui::Text("Lights    %zu", m_scene.lights.size());

                QrClusterLightStats clusterStats = {};

                if (qrGetClusterLightStats(m_instance, &clusterStats) == QR_SUCCESS)
                {
                    ImGui::Text("Clusters  %u  sources %u", clusterStats.clusters, clusterStats.sources);
                    ImGui::Text("Grants    %u  entries %u  denied %u", clusterStats.grants,
                                clusterStats.listEntries, clusterStats.denied);
                }

                if (m_scene.hasBounds)
                {
                    ImGui::Text("Bounds    %.1f %.1f %.1f .. %.1f %.1f %.1f",
                                m_scene.boundsMin[0], m_scene.boundsMin[1], m_scene.boundsMin[2],
                                m_scene.boundsMax[0], m_scene.boundsMax[1], m_scene.boundsMax[2]);
                }

                ImGui::Separator();
                ImGui::TextUnformatted("Camera");
                ImGui::Text("%.1f  %.1f  %.1f", m_cameraPosition[0], m_cameraPosition[1],
                            m_cameraPosition[2]);
                ImGui::Text("yaw %.1f deg  pitch %.1f deg", m_cameraYaw * 57.29577951f,
                            m_cameraPitch * 57.29577951f);
                ImGui::SliderFloat("Fly speed", &m_cameraSpeed, 16.0f, 8192.0f, "%.0f u/s");

                if (ImGui::Button("Reset camera"))
                {
                    ResetCameraToScene();
                }

                ImGui::Separator();
                ImGui::TextUnformatted("Selection");

                if (m_selectedSurface >= 0 && m_selectedSurface < int32_t(m_scene.surfaces.size()))
                {
                    const EditorSurface &surface = m_scene.surfaces[size_t(m_selectedSurface)];

                    ImGui::Text("Surface   %d  face %d", m_selectedSurface, m_selectedFace);
                    ImGui::Text("Texture   %s", surface.textureName.c_str());
                    ImGui::Text("Kind      %s", SurfaceKindName(surface.kind));
                    ImGui::Text("Cluster   %u", m_pickCluster);
                }
                else if (EditorLight *selected = SelectedLight())
                {
                    EditorLight &light = *selected;
                    const std::string selectionLabel = SelectedLightLabel();
                    const uint64_t selectionID = SelectedLightUniqueID();

                    ImGui::Text("%s  %s", selectionLabel.c_str(),
                                light.className.empty() ? "light" : light.className.c_str());
                    ImGui::Text("Position  %.1f %.1f %.1f", light.position[0], light.position[1],
                                light.position[2]);
                    ImGui::Text("Radiance  %.0f %.0f %.0f", light.color[0], light.color[1], light.color[2]);
                    ImGui::Text("Cluster   %s (%u)",
                                light.cluster == QR_CLUSTER_LIGHT_NO_CLUSTER ? "none" : "resolved",
                                light.cluster);

                    // Authoring mirrors the map entity keys the game honours: `light` (which becomes
                    // the radiance through kEditorLightIntensityScale) and rt_elight_radius.
                    //
                    // Every widget's item rect is collected here so a scripted session can click a
                    // field without reading pixels; see `panel rect` below.
                    std::string rects;

                    auto appendRect = [&rects](const char *name) {
                        const ImVec2 min = ImGui::GetItemRectMin();
                        const ImVec2 max = ImGui::GetItemRectMax();

                        rects += " ";
                        rects += name;
                        rects += " " + std::to_string(int32_t(min.x)) + "," + std::to_string(int32_t(min.y)) +
                                 " " + std::to_string(int32_t(max.x)) + "," +
                                 std::to_string(int32_t(max.y));
                    };

                    // Style is the map's `style` key. The preview keeps full brightness because a
                    // standalone editor has no lightstyle feed to modulate it, so this is authored
                    // metadata: it names the style the light will animate with once saved.
                    // A map light also offers NONE, which is what the game's Entity tab shows.
                    // The popup's item rects are logged while it is open, so a scripted session can
                    // pick a style without reading pixels.
                    std::string styleItems;

                    if (ImGui::BeginCombo("Light style##light", EditorStyleName(light.style)))
                    {
                        for (int32_t style = m_selectedLight >= 0 ? -1 : 0; style < kEditorStyleCount;
                             style++)
                        {
                            const bool current = style == light.style;

                            if (ImGui::Selectable(EditorStyleName(style), current))
                            {
                                light.style = style;
                                PrintMessage((selectionLabel + ": style " + std::to_string(style) + " " +
                                              EditorStyleName(style))
                                                 .c_str(),
                                             this);
                            }

                            const ImVec2 itemMin = ImGui::GetItemRectMin();
                            const ImVec2 itemMax = ImGui::GetItemRectMax();

                            styleItems += " " + std::to_string(style) + " " +
                                          std::to_string(int32_t(itemMin.x)) + "," +
                                          std::to_string(int32_t(itemMin.y)) + " " +
                                          std::to_string(int32_t(itemMax.x)) + "," +
                                          std::to_string(int32_t(itemMax.y));

                            if (current)
                            {
                                ImGui::SetItemDefaultFocus();
                            }
                        }

                        ImGui::EndCombo();
                    }

                    if (!styleItems.empty())
                    {
                        const uint64_t styleLayout = std::hash<std::string>{}(styleItems);

                        if (m_loggedStyleLayout != styleLayout)
                        {
                            m_loggedStyleLayout = styleLayout;
                            PrintMessage(("style items:" + styleItems).c_str(), this);
                        }
                    }

                    appendRect("style");

                    float intensity = light.intensity;
                    bool intensityDrawn = ImGui::SliderFloat("Light key##light", &intensity, 0.0f, 800.0f,
                                                             "%.0f");

                    appendRect("lightkey");

                    if (intensityDrawn)
                    {
                        SetEditorLightIntensity(light, intensity);
                        PrintMessage((selectionLabel + ": light key " +
                                      std::to_string(int32_t(intensity)))
                                         .c_str(),
                                     this);
                    }

                    const bool radiusDrawn = ImGui::SliderFloat("Radius##light", &light.radius, 0.0f, 256.0f,
                                                                "%.1f units");

                    appendRect("radius");

                    if (radiusDrawn)
                    {
                        PrintMessage((selectionLabel + ": radius " +
                                      std::to_string(int32_t(light.radius)))
                                         .c_str(),
                                     this);
                    }

                    const bool colourDrawn = ImGui::ColorEdit3("Colour##light", light.tint);

                    appendRect("colour");

                    if (colourDrawn)
                    {
                        SetEditorLightTint(light, light.tint);
                        PrintMessage((selectionLabel + ": colour " +
                                      std::to_string(light.tint[0]) + " " + std::to_string(light.tint[1]) +
                                      " " + std::to_string(light.tint[2]))
                                         .c_str(),
                                     this);
                    }

                    const bool positionDrawn =
                        ImGui::DragFloat3("Position##light", light.position, 1.0f, -16384.0f, 16384.0f,
                                          "%.1f units");

                    appendRect("position");

                    if (positionDrawn)
                    {
                        light.cluster = ResolveEditorLightCluster(m_scene, light.position);
                        PrintMessage((selectionLabel + ": position " +
                                      std::to_string(int32_t(light.position[0])) + " " +
                                      std::to_string(int32_t(light.position[1])) + " " +
                                      std::to_string(int32_t(light.position[2])) + " cluster " +
                                      std::to_string(light.cluster))
                                         .c_str(),
                                     this);
                    }

                    // Aim and drop: the light lands just off the picked face on the camera side,
                    // so it sits in the room instead of inside the wall.
                    ImGui::BeginDisabled(!m_hasPickPoint);

                    if (ImGui::Button("Place at pick##light") && m_hasPickPoint)
                    {
                        float ray[3] = { m_pickPoint[0] - m_cameraPosition[0],
                                         m_pickPoint[1] - m_cameraPosition[1],
                                         m_pickPoint[2] - m_cameraPosition[2] };
                        const float rayLength =
                            std::sqrt(ray[0] * ray[0] + ray[1] * ray[1] + ray[2] * ray[2]);

                        if (rayLength > 1e-4f)
                        {
                            // Stand off the surface along the pick ray so the light does not sit
                            // inside the face it lights, but by the same short step whatever its
                            // radius: a radius-sized step walks a large light out of the map,
                            // where it resolves to no cluster and shades nothing at all.
                            constexpr float kPlaceOffsetUnits = 8.0f;

                            // The cone is aimed down the pick ray, at what was picked: a spot
                            // that faced the camera would light the space behind it and leave
                            // the surface the author clicked on dark.
                            for (int axis = 0; axis < 3; axis++)
                            {
                                ray[axis] /= rayLength;
                                light.position[axis] = m_pickPoint[axis] - ray[axis] * kPlaceOffsetUnits;
                                light.direction[axis] = ray[axis];
                            }

                            light.cluster = ResolveEditorLightCluster(m_scene, light.position);

                            // A light that fell outside every leaf still belongs to what was
                            // picked: the surface's own cluster is where its list has to be.
                            if (light.cluster == QR_CLUSTER_LIGHT_NO_CLUSTER)
                            {
                                light.cluster = m_pickCluster;
                            }

                            PrintMessage((selectionLabel + ": placed at " +
                                          std::to_string(int32_t(light.position[0])) + " " +
                                          std::to_string(int32_t(light.position[1])) + " " +
                                          std::to_string(int32_t(light.position[2])) + " cluster " +
                                          std::to_string(light.cluster))
                                             .c_str(),
                                         this);
                            LogLightCluster(light, selectionID, selectionLabel.c_str());
                        }
                    }

                    ImGui::EndDisabled();
                    appendRect("place");

                    // The `spot` key and its cone, uploaded through qrUploadSpotLight so the
                    // light stops being a point source.
                    const bool spotDrawn = ImGui::Checkbox("Spot##light", &light.spot);

                    appendRect("spot");

                    if (spotDrawn)
                    {
                        PrintMessage((selectionLabel + ": spot " +
                                      (light.spot ? "on" : "off"))
                                         .c_str(),
                                     this);
                        LogLightCluster(light, selectionID, selectionLabel.c_str());
                    }

                    if (light.spot)
                    {
                        // Three separate fields so each axis can be pointed at on its own - a
                        // DragFloat3 packs them into one item rect and hides the axis under the
                        // pointer, which makes both authoring and scripted checks guesswork.
                        ImGui::TextUnformatted("Spot dir");

                        const float fieldWidth =
                            (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) /
                            3.0f;

                        bool directionDrawn = false;

                        for (int axis = 0; axis < 3; axis++)
                        {
                            const char *ids[3] = {"##dirx", "##diry", "##dirz"};

                            if (axis > 0)
                            {
                                ImGui::SameLine();
                            }

                            ImGui::SetNextItemWidth(fieldWidth);
                            directionDrawn |=
                                ImGui::DragFloat(ids[axis], &light.direction[axis], 0.05f, -1.0f, 1.0f,
                                                 "%.2f");
                            appendRect(axis == 0 ? "dirx" : (axis == 1 ? "diry" : "dirz"));
                        }

                        if (directionDrawn)
                        {
                            PrintMessage((selectionLabel + ": direction " +
                                          std::to_string(light.direction[0]) + " " +
                                          std::to_string(light.direction[1]) + " " +
                                          std::to_string(light.direction[2]))
                                             .c_str(),
                                         this);
                        }

                        bool coneDrawn = ImGui::SliderFloat("Inner deg##light", &light.angleInner, 0.0f,
                                                            90.0f, "%.1f deg");

                        appendRect("inner");

                        coneDrawn |= ImGui::SliderFloat("Outer deg##light", &light.angleOuter, 0.0f, 90.0f,
                                                        "%.1f deg");

                        appendRect("outer");

                        if (coneDrawn)
                        {
                            ClampEditorLightCone(light);
                            PrintMessage((selectionLabel + ": cone " +
                                          std::to_string(int32_t(light.angleInner)) + ".." +
                                          std::to_string(int32_t(light.angleOuter)) + " deg")
                                             .c_str(),
                                         this);
                        }
                    }

                    {
                        const uint64_t layout = std::hash<std::string>{}(rects);
                        const uint64_t key = layout | 1u;

                        if (m_loggedLightLayout != key)
                        {
                            m_loggedLightLayout = key;
                            PrintMessage(("panel rect:" + rects + " scroll " +
                                          std::to_string(int32_t(ImGui::GetScrollY())))
                                             .c_str(),
                                         this);
                        }
                    }
                }
                else
                {
                    ImGui::TextUnformatted("Nothing picked - LMB in the viewport");
                }

                if (m_hasPickPoint)
                {
                    ImGui::Text("At        %.1f %.1f %.1f", m_pickPoint[0], m_pickPoint[1], m_pickPoint[2]);
                }

                if (ImGui::Button("Clear selection"))
                {
                    ClearSelection();
                }

                ImGui::Separator();
                // The sun is level-wide and now carries the sky block's scope, so it lives in one
                // place: the Render tab's Sun group, next to the value it writes to `sky:`.
                ImGui::TextUnformatted("Sun: Render tab, Sky scope");

                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            if (BeginLoggedTab("Entities", tabFlags("Entities"), tabRects, clickedTab))
            {
                m_activePanelTab = "Entities";
                DrawLightList();
                ImGui::EndTabItem();
            }

            if (BeginLoggedTab("Render", tabFlags("Render"), tabRects, clickedTab))
            {
                m_activePanelTab = "Render";
                DrawGlobalSettings();
                ImGui::EndTabItem();
            }

            if (BeginLoggedTab("Materials", tabFlags("Materials"), tabRects, clickedTab))
            {
                m_activePanelTab = "Materials";
                DrawMaterials();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        // A click on the tab that was already open is the reset gesture, the way the game's tab
        // bar reports it. The question comes first: this is the one control that removes files.
        if (!clickedTab.empty() && clickedTab == previousTab && m_resetPrompt == 0 &&
            m_sessionPrompt == 0)
        {
            m_resetPrompt = 1;
            m_loggedResetLayout = 0;
            PrintMessage(("session: reset requested from the " + clickedTab + " tab").c_str(), this);
        }

        const uint64_t tabLayout = std::hash<std::string>{}(tabRects) | 1u;

        if (m_loggedTabLayout != tabLayout)
        {
            m_loggedTabLayout = tabLayout;
            PrintMessage(("panel tabs:" + tabRects).c_str(), this);
        }
    }

    ImGui::End();
}

void EditorApp::LogRect(const char *name)
{
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    const std::string text = std::string("panel rect: ") + name + " " +
                             std::to_string(int32_t(minimum.x)) + "," + std::to_string(int32_t(minimum.y)) +
                             " " + std::to_string(int32_t(maximum.x)) + "," + std::to_string(int32_t(maximum.y));
    const uint64_t layout = std::hash<std::string>{}(text) | 1u;
    uint64_t &logged = m_loggedRects[name];

    if (logged != layout)
    {
        logged = layout;
        PrintMessage(text.c_str(), this);
    }
}

void EditorApp::DrawGlobalSettings()
{
    EditorRenderSettings &settings = m_settings;

    ImGui::Separator();

    // Every group is open by default and the whole block scrolls, so the panel
    // stays inside the viewport however many settings the game editor grows.
    const float available = ImGui::GetIO().DisplaySize.y * 0.45f;
    const float scrollHeight = available < 520.0f ? available : 520.0f;

    if (!ImGui::BeginChild("GlobalScroll", ImVec2(360.0f, scrollHeight)))
    {
        ImGui::EndChild();
        return;
    }

    const ImGuiTreeNodeFlags openGroup = ImGuiTreeNodeFlags_DefaultOpen;

    if (ImGui::CollapsingHeader("Global", openGroup))
    {
        // rt_brightness: the one sky value the block does not carry, so it follows the config
        // whatever scope the panel edits.
        ImGui::SliderFloat("Brightness", &settings.brightness, 0.0f, 3.0f, "%.2f");
    }

    if (ImGui::CollapsingHeader("Sky", openGroup))
    {
        // The scope of the whole block, not just this group: the game's checkbox reads
        // !LevelHasBlock() at load and false means the level's `sky:` block is written.
        bool global = m_skyGlobal;

        if (ImGui::Checkbox("Global settings", &global))
        {
            SetSkyScope(global);
        }

        LogRect("sky scope");

        const char *skyTypes[] = { "Colour", "Cubemap", "Rasterized", "Procedural" };
        ImGui::Combo("Sky type", &settings.skyType, skyTypes, IM_ARRAYSIZE(skyTypes));
        LogRect("sky rt_physical_sky");

        ImGui::SliderFloat("Sky scale", &settings.skyScale, 0.0f, 8.0f, "%.2f");
        LogRect("sky rt_sky");
        ImGui::SliderFloat("Sky brightness", &settings.skyBrightness, 0.0f, 10.0f, "%.2f");
        LogRect("sky rt_sky_brightness");
        ImGui::ColorEdit3("Sky colour", settings.skyColor);
        LogRect("sky rt_sky_color");
        ImGui::SliderFloat("Sky light mult", &settings.skyLightMultiplier, 0.0f, 8.0f, "%.2f");
        LogRect("sky rt_sky_light_mult");
        ImGui::SliderInt("Sky ambient lod", &settings.skyAmbientLod, 0, 10);
        LogRect("sky rt_sky_ambient_lod");
        ImGui::Checkbox("Sky nee", &settings.skyNee);
        LogRect("sky rt_sky_nee");
    }

    if (ImGui::CollapsingHeader("Clouds", openGroup))
    {
        ImGui::Checkbox("Clouds##enabled", &settings.clouds);
        ImGui::ColorEdit3("Cloud colour", settings.cloudsColor);
        ImGui::SliderFloat("Cloud alpha", &settings.cloudsAlpha, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Coverage", &settings.cloudsCoverage, 0.0f, 1.0f, "%.3f");
        ImGui::SliderFloat("Density", &settings.cloudsDensity, 0.0f, 10.0f, "%.2f");
        ImGui::SliderFloat("Speed", &settings.cloudsSpeed, 0.0f, 4.0f, "%.2f");
        ImGui::SliderInt("Quality", &settings.cloudsQuality, 0, QR_SKY_CLOUDS_MAX_QUALITY);
        ImGui::SliderFloat("Height", &settings.cloudsHeight, 1.0f, 300000.0f, "%.0f units");
        ImGui::SliderFloat("Thickness", &settings.cloudsThickness, 1.0f, 300000.0f, "%.0f units");
    }

    if (ImGui::CollapsingHeader("Sun", openGroup))
    {
        // rt_physical_sun, off by default like the shipped config: the disc below is drawn whether
        // or not anything casts light.
        ImGui::Checkbox("Light##sun", &m_sun.enabled);

        // The checkbox and this slider are rt_physical_sun and rt_sky_sun; the game's cvar is a
        // multiplier the light fixup scales by 16, which is the unit this slider shows.
        float strength = m_sun.radiance / kEditorSkySunRadiance;

        if (ImGui::SliderFloat("Light strength", &strength, 0.0f, 10.0f, "%.2f"))
        {
            m_sun.radiance = strength * kEditorSkySunRadiance;
        }

        LogRect("sun rt_sky_sun");

        // Pitch and yaw are the game's own cvar values: 140 and 120 put the sun where the shipped
        // config has it, and they round-trip through the block without an angle conversion.
        ImGui::SliderFloat("Pitch", &m_sun.pitch, -180.0f, 180.0f, "%.1f");
        LogRect("sun rt_sky_sun_pitch");
        ImGui::SliderFloat("Yaw", &m_sun.yaw, 0.0f, 360.0f, "%.1f");
        LogRect("sun rt_sky_sun_yaw");

        ImGui::Separator();
        ImGui::SliderFloat("Disc scale", &settings.sunDiscScale, 0.0f, 20.0f, "%.2f");
        ImGui::ColorEdit3("Disc colour", settings.sunColor);
        ImGui::SliderFloat("Disc size", &settings.sunDiscSize, 0.0f, 10.0f, "%.2f");
        ImGui::Text("Angular size %.2f deg", double(m_sun.angularDiameter));
    }

    if (ImGui::CollapsingHeader("God rays", openGroup))
    {
        ImGui::Checkbox("God rays##enabled", &settings.godRays);
        ImGui::SliderFloat("Shaft strength", &settings.godRaysIntensity, 0.0f, 4.0f, "%.2f");
        ImGui::SliderInt("Shaft quality", &settings.godRaysQuality, 0, 3);
        // rt_sky_godrays_sky_threshold: how bright a sky pixel must be before it emits a shaft.
        ImGui::SliderFloat("Sky threshold", &settings.godRaysSkyThreshold, 0.0f, 1.0f, "%.2f");
        LogRect("godrays rt_sky_godrays_sky_threshold");
    }

    if (ImGui::CollapsingHeader("Volumetric fog", openGroup))
    {
        const char *volumeTypes[] = { "Off", "Depth based", "Full" };
        ImGui::Combo("Volume type", &settings.volumeType, volumeTypes, IM_ARRAYSIZE(volumeTypes));
        ImGui::SliderFloat("Scattering", &settings.volumeScatter, 0.0f, 1.0f, "%.3f");
        ImGui::SliderFloat("Ambient", &settings.volumeAmbient, 0.0f, 8.0f, "%.2f");
        ImGui::SliderFloat("Far", &settings.volumeFar, 0.0f, 4000.0f, "%.0f units");
        ImGui::SliderFloat("Light intensity", &settings.volumeLightIntensity, 0.0f, 1000.0f, "%.0f");
        ImGui::SliderFloat("Asymmetry", &settings.volumeAsymmetry, -0.99f, 0.99f, "%.2f");
    }

    if (ImGui::CollapsingHeader("Tonemap", openGroup))
    {
        ImGui::SliderFloat("White point", &settings.tonemapWhite, 1.0f, 40.0f, "%.2f");
        ImGui::SliderFloat("Chroma", &settings.tonemapChroma, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Desaturation", &settings.tonemapDesat, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Max log", &settings.tonemapMaxLog, -4.0f, 4.0f, "%.2f");
        ImGui::SliderFloat("Min log", &settings.tonemapMinLog, -12.0f, 0.0f, "%.2f");
        ImGui::SliderInt("Tonemap type", &settings.tonemapType, 0, 4);
    }

    if (ImGui::CollapsingHeader("Water / Acid", openGroup))
    {
        // The look every `water` and `acid` surface shares: which surface is one is a
        // property of its material, the way the game splits the two.
        ImGui::SliderFloat("Wave speed", &settings.waterSpeed, 0.0f, 2.0f, "%.2f");
        LogRect("water rt_water_speed");
        ImGui::SliderFloat("Wave strength", &settings.waterNormalStrength, 0.0f, 4.0f, "%.2f");
        LogRect("water rt_water_normal_strength");
        ImGui::SliderFloat("Wave sharpness", &settings.waterNormalSharpness, 0.0f, 16.0f, "%.2f");
        LogRect("water rt_water_normal_sharpness");
        ImGui::SliderFloat("Wave scale", &settings.waterScale, 0.0f, 4.0f, "%.2f");
        LogRect("water rt_water_scale");
        ImGui::ColorEdit3("Water colour", settings.waterColor);
        LogRect("water rt_water_color");
        ImGui::ColorEdit3("Acid colour", settings.acidColor);
        LogRect("water rt_acid_color");
    }

    if (ImGui::CollapsingHeader("Level fog", openGroup))
    {
        ImGui::ColorEdit3("Fog colour", settings.fogColor);
        ImGui::SliderFloat("Density", &settings.fogDensity, 0.0f, 1.0f, "%.4f");
        ImGui::SliderFloat("Sky blend", &settings.fogSkyBlend, 0.0f, 1.0f, "%.2f");

        // The game's button sits at the bottom of the same fog group, because regenerate is a
        // level-wide action of the tab the fog belongs to rather than a material edit.
        ImGui::Separator();

        if (ImGui::Button("Regenerate from map"))
        {
            m_regenPrompt = 1;
        }

        LogRect("regenerate from map");

        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Restore the level's standard predefined lights from the map and "
                              "save them to qray.lights.yaml.");
        }
    }

    ImGui::EndChild();
}

void EditorApp::DrawMaterials()
{
    ImGui::Separator();
    ImGui::TextUnformatted("Materials");

    if (m_materials.Empty())
    {
        ImGui::TextUnformatted("No surfaces loaded");
        return;
    }

    // One entry per texture name, sorted. The list scrolls on its own so the
    // parameter sections below stay reachable on a short screen.
    const float availableListHeight = ImGui::GetIO().DisplaySize.y * 0.22f;
    const float listHeight = availableListHeight < 160.0f ? availableListHeight : 160.0f;

    if (ImGui::BeginChild("MaterialList", ImVec2(360.0f, listHeight), true))
    {
        for (size_t i = 0; i < m_materials.Count(); i++)
        {
            const std::string &name = m_materials.NameAt(i);
            // The marker is the game list's "the file carries this entry": the tick is what a
            // save would rewrite here, so it tells an edit apart from a name the file lacks.
            const std::string label = FormatMaterialListLabel(name);
            const bool selected = int32_t(i) == m_selectedMaterial;

            if (ImGui::Selectable(label.c_str(), selected))
            {
                m_selectedMaterial = int32_t(i);
            }
        }
    }

    ImGui::EndChild();

    if (m_selectedMaterial < 0 || size_t(m_selectedMaterial) >= m_materials.Count())
    {
        m_selectedMaterial = -1;
        // A browser opened on the row that has just been deselected still has to be closable.
        DrawTextureBrowser();
        return;
    }

    // The sections scroll on their own so the panel stays inside the viewport on a
    // short screen, however many parameters a material grows.
    if (!ImGui::BeginChild("MaterialParams", ImVec2(0.0f, 0.0f)))
    {
        ImGui::EndChild();
        return;
    }

        EditorMaterialParams &material = m_materials.At(size_t(m_selectedMaterial));
        bool changed = false;

        if (ImGui::CollapsingHeader("Textures", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // The game lists texture_base first, before the scale and offset pair: it is the
            // image the other three transform.
            const int32_t baseIndex = TextureIndexByName(material.baseTexture);
            const bool baseResolved = baseIndex >= 0 &&
                                      size_t(baseIndex) < m_textureMaterials.size() &&
                                      m_textureMaterials[size_t(baseIndex)] != QR_NO_MATERIAL;

            if (baseResolved)
            {
                const EditorTexture &texture = m_scene.textures[size_t(baseIndex)];
                const float side = ImGui::GetFrameHeight();

                ImGui::Image(
                    ImTextureRef(ImTextureID(uintptr_t(m_textureMaterials[size_t(baseIndex)]))),
                    ImVec2(side, side));
                LogRect("material base preview");
                ImGui::SameLine();
                ImGui::Text("%s  %ux%u", texture.name.c_str(), texture.width, texture.height);
            }
            else if (material.baseTexture.empty())
            {
                ImGui::TextUnformatted("Base  NONE - the map's own image");
            }
            else
            {
                // The name is kept as written even though nothing here can show it: the file
                // still has to carry what the author put there, and the embedder may resolve it.
                ImGui::Text("Base  %s - not in this scene", material.baseTexture.c_str());
            }

            if (ImGui::Button("Browse...##base"))
            {
                m_textureBrowserRequested = true;
            }

            LogRect("material base browse");

            ImGui::SameLine();

            if (ImGui::Button("NONE##base"))
            {
                material.baseTexture.clear();
                changed = true;
            }

            LogRect("material base none");

            changed |= ImGui::SliderFloat("Scale U##tex", &material.textureScaleU, 0.05f, 16.0f, "%.2f");
            changed |= ImGui::SliderFloat("Scale V##tex", &material.textureScaleV, 0.05f, 16.0f, "%.2f");
            changed |= ImGui::SliderFloat("Offset U##tex", &material.textureOffsetU, -8.0f, 8.0f, "%.2f");
            changed |= ImGui::SliderFloat("Offset V##tex", &material.textureOffsetV, -8.0f, 8.0f, "%.2f");

            if (ImGui::Button("Reset texture transform"))
            {
                material.textureScaleU = 1.0f;
                material.textureScaleV = 1.0f;
                material.textureOffsetU = 0.0f;
                material.textureOffsetV = 0.0f;
                changed = true;
            }

            // The extra images a packed material set carries. NONE keeps whatever the source
            // image already puts in that channel.
            const auto textureCombo = [this, &changed](const char *label, std::string &name) {
                bool edited = false;

                if (ImGui::BeginCombo(label, name.empty() ? "NONE" : name.c_str()))
                {
                    if (ImGui::Selectable("NONE", name.empty()))
                    {
                        name.clear();
                        edited = true;
                    }

                    for (size_t i = 0; i < m_scene.textures.size(); ++i)
                    {
                        const std::string &candidate = m_scene.textures[i].name;
                        const bool selected = !name.empty() && candidate == name;

                        if (ImGui::Selectable(candidate.c_str(), selected))
                        {
                            name = candidate;
                            edited = true;
                        }

                        if (selected)
                        {
                            ImGui::SetItemDefaultFocus();
                        }
                    }

                    // A name the scene does not carry still has to stay visible here, otherwise
                    // opening the material would look like it had cleared what the file holds.
                    if (!name.empty() && TextureIndexByName(name) < 0)
                    {
                        ImGui::Separator();
                        ImGui::TextDisabled("%s - not in this scene", name.c_str());
                    }

                    ImGui::EndCombo();
                }

                LogRect(label);
                changed |= edited;
            };

            textureCombo("Gloss##tex", material.glossTexture);
            textureCombo("Normal##tex", material.normalTexture);
            textureCombo("Height##tex", material.heightTexture);
            textureCombo("Emissive##tex", material.emissiveTexture);
            textureCombo("Overlay##tex", material.overlayTexture);
            textureCombo("Subsurface##tex", material.textureSss);

            if (ImGui::Button("Reset extra textures"))
            {
                material.glossTexture.clear();
                material.normalTexture.clear();
                material.heightTexture.clear();
                material.emissiveTexture.clear();
                material.overlayTexture.clear();
                material.textureSss.clear();
                changed = true;
            }
        }

        if (ImGui::CollapsingHeader("Surface", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // roughness_override at 0 means "no override" in the game editor, so the
            // checkbox owns whether a value is sent at all.
            bool roughnessOverride = material.roughnessOverride >= 0.0f;

            if (ImGui::Checkbox("Roughness override", &roughnessOverride))
            {
                material.roughnessOverride = roughnessOverride ? kEditorDefaultRoughness : -1.0f;
                changed = true;
            }

            if (roughnessOverride)
            {
                changed |= ImGui::SliderFloat("Roughness##surf", &material.roughnessOverride, 0.0f, 1.0f,
                                              "%.3f");
            }

            changed |= ImGui::SliderFloat("Metalness##surf", &material.metalnessFactor, 0.0f, 1.0f,
                                          "%.3f");
            changed |= ImGui::SliderFloat("Base factor##surf", &material.baseFactor, 0.0f, 4.0f, "%.2f");

            int32_t type = int32_t(material.type);
            const char *materialTypes[] = { "None", "Glass", "Water" };

            if (ImGui::Combo("Material##surf", &type, materialTypes, IM_ARRAYSIZE(materialTypes)))
            {
                material.type = EditorMaterialType(type);
                changed = true;
            }

            changed |= ImGui::Checkbox("Mirror##surf", &material.mirror);

            // The game also scales the normal map's tilt, and its roughness can come from the
            // normal map's alpha channel instead of the scalar.
            changed |= ImGui::SliderFloat("Bump scale##surf", &material.bumpScale, 0.0f, 4.0f, "%.2f");
            LogRect("material bump scale");
            changed |= ImGui::SliderFloat("Parallax scale##surf", &material.parallaxScale, 0.0f,
                                          0.15f, "%.3f");
            changed |= ImGui::Checkbox("Metalness from normal alpha##surf",
                                       &material.metalnessFromNormalAlpha);

            // The game locks the pane's own fields off a material that is not glass, which is
            // also why they only reach the layer colors when material_glass is set.
            const bool glassPane = material.type == EditorMaterialType::Glass;

            ImGui::BeginDisabled(!glassPane);
            changed |= ImGui::SliderFloat("Glass IOR##surf", &material.glassIor, 0.0f, 5.0f, "%.2f");
            changed |= ImGui::SliderFloat("Glass thickness##surf", &material.glassThickness, 0.0f,
                                          64.0f, "%.1f");
            changed |= ImGui::SliderFloat("Transparency##surf", &material.transparency, 0.0f, 1.0f,
                                          "%.3f");
            LogRect("material transparency");
            changed |= ImGui::ColorEdit3("Glass colour##surf", material.glassColor);
            ImGui::EndDisabled();
        }

        if (ImGui::CollapsingHeader("Model", ImGuiTreeNodeFlags_DefaultOpen))
        {
            changed |= ImGui::Checkbox("Exact normals##model", &material.exactNormals);

            // The game's force_rasterize only reaches alias models and sprites, which this editor
            // does not upload, and the SDK's upload info has no such flag. The checkbox exists so
            // the key survives a trip through the editor, and the tooltip says it changes nothing.
            changed |= ImGui::Checkbox("Force rasterize##model", &material.forceRasterize);

            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("The game reads this for alias models and sprites only; "
                                  "BSP surfaces and the traced path ignore it.");
            }
        }

        if (ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen))
        {
            changed |= ImGui::SliderFloat("Emissive factor##luma", &material.emissiveFactor, 0.0f, 1.0f,
                                          "%.3f");
            // The checkbox uploads EMISSION_RAY_ONLY, which the game's is_light key also reaches,
            // so the label and the file key agree on what the flag means.
            changed |= ImGui::Checkbox("Is light##luma", &material.luma);

            // The rest of the light group is what the surface casts in the game, which builds it
            // from the geometry of the surface itself. Shown, not edited: a slider here would
            // write the file but change nothing on screen, and "Regenerate from map" is what
            // fills the values in.
            if (material.hasLightColor || material.lightBrightness != 1.0f || material.lightStyles ||
                material.lightFocus > 0.0f || material.lightProjector)
            {
                char cast[160];

                if (material.hasLightColor)
                {
                    std::snprintf(cast, sizeof(cast), "casts a light: %d %d %d, brightness %.3g%s",
                                  int(std::lround(material.lightColor[0] * 255.0f)),
                                  int(std::lround(material.lightColor[1] * 255.0f)),
                                  int(std::lround(material.lightColor[2] * 255.0f)),
                                  material.lightBrightness,
                                  material.lightStyles ? ", animated styles" : "");
                }
                else
                {
                    std::snprintf(cast, sizeof(cast), "no cast light: the material has no "
                                                      "light_color");
                }

                ImGui::TextDisabled("%s", cast);
            }

            // luma_blend picks how the emissive layer is composited: the game stores -1 for "use
            // the global setting" and 0..5 for a style this material forces.
            {
                const char *blendNames[] = { "Cvar", "Off", "Normal", "Screen", "Overlay",
                                             "Hard light", "Color dodge" };
                int32_t blend = material.lumaBlend + 1;
                blend = std::clamp(blend, 0, int32_t(IM_ARRAYSIZE(blendNames)) - 1);

                if (ImGui::Combo("Luma blend##luma", &blend, blendNames, IM_ARRAYSIZE(blendNames)))
                {
                    material.lumaBlend = blend - 1;
                    changed = true;
                }

                LogRect("material luma blend");
            }
        }

        if (ImGui::CollapsingHeader("Luma blocks", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // The material-level fallbacks: a block that omits a key inherits these, and a block
            // the author appends starts from them.
            changed |= ImGui::SliderFloat("Luma threshold##lumadef", &material.lumaThreshold, 0.0f, 1.0f,
                                          "%.3f");
            LogRect("material luma threshold");
            changed |= ImGui::SliderFloat("Luma feather##lumadef", &material.lumaFeather, 0.0f,
                                          float(kEditorMaskFeatherMax), "%.1f");
            LogRect("material luma feather");

            int32_t removeIndex = -1;

            // The image a block is authored against: the material's base texture, or - when the file
            // leaves base as NONE - the texture the row itself overrides, which is what the map
            // shows there and therefore what the author is matching.
            int32_t lumaTexture = TextureIndexByName(material.baseTexture);

            if (lumaTexture < 0)
            {
                lumaTexture = TextureIndexByName(m_materials.NameAt(size_t(m_selectedMaterial)));
            }

            const bool lumaTextureResolved = lumaTexture >= 0 &&
                                             size_t(lumaTexture) < m_textureMaterials.size() &&
                                             size_t(lumaTexture) < m_scene.textures.size() &&
                                             m_textureMaterials[size_t(lumaTexture)] != QR_NO_MATERIAL;

            for (size_t blockIndex = 0; blockIndex < material.emissiveBlocks.size(); ++blockIndex)
            {
                EditorEmissiveBlock &block = material.emissiveBlocks[blockIndex];
                const bool polygon = !block.polyUv.empty();
                char label[64];

                if (polygon)
                {
                    std::snprintf(label, sizeof(label), "Mask  %d points",
                                  int(block.polyUv.size() / 2));
                }
                else
                {
                    std::snprintf(label, sizeof(label), "Colour  %02X%02X%02X",
                                  int(std::lround(block.color[0] * 255.0f)),
                                  int(std::lround(block.color[1] * 255.0f)),
                                  int(std::lround(block.color[2] * 255.0f)));
                }

                ImGui::PushID(int(blockIndex));

                if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen))
                {
                    if (polygon)
                    {
                        // The block selects by uv, so the mask is painted over the image the
                        // surface shows, with the game's own polygon handles.
                        if (lumaTextureResolved)
                        {
                            const EditorTexture &texture = m_scene.textures[size_t(lumaTexture)];

                            // RT_MAT_EMIS_POLY_MAX: the game reads no more points than this.
                            changed |= EditorPolygonEdit(
                                "##lumamask", uint64_t(m_textureMaterials[size_t(lumaTexture)]),
                                texture.width, texture.height, block.polyUv, 16);
                        }
                        else
                        {
                            ImGui::TextDisabled("The material has no image to paint a mask on.");
                        }

                        LogRect("material luma mask");

                        ImGui::TextDisabled(
                            "Drag a point, a side or the outline. Ctrl+click adds a point, "
                            "right-click removes one, Ctrl+wheel zooms.");

                        if (ImGui::Button("Reset mask"))
                        {
                            block.polyUv = { 0.25f, 0.25f, 0.75f, 0.25f, 0.75f, 0.75f, 0.25f, 0.75f };
                            changed = true;
                        }

                        ImGui::SameLine();
                    }
                    else
                    {
                        changed |= ImGui::ColorEdit3("Luma colour##lumacol", block.color);

                        // The game's eyedropper: the block shows the image it matches and a click
                        // takes that pixel, which is how a colour is found without guessing.
                        if (lumaTextureResolved)
                        {
                            const EditorTexture &texture = m_scene.textures[size_t(lumaTexture)];
                            float u = 0.0f;
                            float v = 0.0f;

                            if (EditorImagePick(
                                    "##lumapick",
                                    uint64_t(m_textureMaterials[size_t(lumaTexture)]), texture.width,
                                    texture.height, u, v))
                            {
                                const size_t pixels =
                                    size_t(texture.width) * size_t(texture.height) * 4;

                                if (texture.rgba.size() >= pixels)
                                {
                                    const int32_t px = std::clamp(
                                        int32_t(u * float(texture.width)), 0,
                                        int32_t(texture.width) - 1);
                                    const int32_t py = std::clamp(
                                        int32_t(v * float(texture.height)), 0,
                                        int32_t(texture.height) - 1);
                                    const uint8_t *pixel =
                                        texture.rgba.data() +
                                        (size_t(py) * size_t(texture.width) + size_t(px)) * 4;

                                    block.color[0] = float(pixel[0]) / 255.0f;
                                    block.color[1] = float(pixel[1]) / 255.0f;
                                    block.color[2] = float(pixel[2]) / 255.0f;
                                    changed = true;
                                }
                            }

                            LogRect("material luma pick");
                            ImGui::TextDisabled("Click a colour in the image to take it.");
                        }

                        changed |= ImGui::SliderFloat("Luma threshold##lumablk", &block.threshold,
                                                      0.0f, 1.0f, "%.3f");
                    }

                    {
                        int32_t feather = std::clamp(int32_t(std::lround(block.feather)), 0,
                                                     kEditorMaskFeatherMax);

                        if (ImGui::SliderInt("Luma feather##lumablk", &feather, 0,
                                             kEditorMaskFeatherMax, "%d"))
                        {
                            // The game rounds the stored value, so the slider owns an integer.
                            block.feather = float(feather);
                            changed = true;
                        }
                    }

                    changed |= ImGui::SliderFloat("Emissive factor##lumablk", &block.factor, 0.0f, 5.0f,
                                                  "%.3f");

                    {
                        const char *blendNames[] = { "Cvar", "Off", "Normal", "Screen", "Overlay",
                                                     "Hard light", "Color dodge" };
                        int32_t blend = block.blend + 1;
                        blend = std::clamp(blend, 0, int32_t(IM_ARRAYSIZE(blendNames)) - 1);

                        if (ImGui::Combo("Luma blend##lumablk", &blend, blendNames,
                                         IM_ARRAYSIZE(blendNames)))
                        {
                            block.blend = blend - 1;
                            changed = true;
                        }
                    }

                    {
                        // The game keeps one block clipboard; pasting only into a block of the
                        // same kind, since a polygon cannot become a colour.
                        const bool canPaste = m_lumaClipboardValid &&
                                              m_lumaClipboard.polyUv.empty() == block.polyUv.empty();

                        if (ImGui::Button("Copy##lumablk"))
                        {
                            m_lumaClipboard = block;
                            m_lumaClipboardValid = true;
                        }

                        ImGui::SameLine();
                        ImGui::BeginDisabled(!canPaste);

                        if (ImGui::Button("Paste##lumablk"))
                        {
                            block = m_lumaClipboard;
                            changed = true;
                        }

                        ImGui::EndDisabled();
                        ImGui::SameLine();
                    }

                    if (ImGui::Button("Remove block"))
                    {
                        removeIndex = int32_t(blockIndex);
                    }

                    ImGui::TreePop();
                }

                ImGui::PopID();
            }

            if (removeIndex >= 0)
            {
                material.emissiveBlocks.erase(material.emissiveBlocks.begin() + removeIndex);

                changed = true;
            }

            const bool canAdd = material.emissiveBlocks.size() < kEditorEmissiveBlockMax;

            ImGui::BeginDisabled(!canAdd);

            const auto appendBlock = [&material, &changed](bool polygon) {
                EditorEmissiveBlock block;
                block.threshold = material.lumaThreshold > 0.0f ? material.lumaThreshold : 0.02f;
                block.feather = material.lumaFeather;
                block.factor = material.emissiveFactor;
                block.blend = material.lumaBlend;

                if (polygon)
                {
                    block.polyUv = { 0.25f, 0.25f, 0.75f, 0.25f, 0.75f, 0.75f, 0.25f, 0.75f };
                }

                material.emissiveBlocks.push_back(block);
                changed = true;
            };

            if (ImGui::Button("Add colour block"))
            {
                appendBlock(false);
            }

            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !canAdd)
            {
                ImGui::SetTooltip("Ten blocks is the most the game reads");
            }

            LogRect("material luma add colour");

            ImGui::SameLine();

            if (ImGui::Button("Add mask block"))
            {
                appendBlock(true);
            }

            LogRect("material luma add mask");
            ImGui::EndDisabled();
        }

    if (changed)
    {
        m_materialsChanged = true;
        TouchMaterial(m_materials.NameAt(size_t(m_selectedMaterial)));
    }

    ImGui::EndChild();

    // Drawn from the tab that owns the Base row, so the browser sits over the panel it was
    // opened from and closes back into it.
    DrawTextureBrowser();
}

namespace
{
std::string &MaterialSlotRef(EditorMaterialParams &params, EditorApp::EditorTextureSlot slot)
{
    switch (slot)
    {
    case EditorApp::EditorTextureSlot::Normals:
        return params.normalTexture;
    case EditorApp::EditorTextureSlot::Height:
        return params.heightTexture;
    case EditorApp::EditorTextureSlot::Emissive:
        return params.emissiveTexture;
    case EditorApp::EditorTextureSlot::Overlay:
        return params.overlayTexture;
    case EditorApp::EditorTextureSlot::Gloss:
        return params.glossTexture;
    case EditorApp::EditorTextureSlot::Subsurface:
        return params.textureSss;
    default:
        return params.baseTexture;
    }
}

std::string ActiveModDisplayName(const std::string &gamePath)
{
    if (gamePath.empty())
    {
        return "-";
    }

    const std::filesystem::path normalized = std::filesystem::path(gamePath).lexically_normal();
    std::string label = normalized.filename().string();

    if (label.empty())
    {
        label = normalized.parent_path().filename().string();
    }

    return label.empty() ? std::string("-") : label;
}

std::string BrowserTreeOf(const std::string &name)
{
    const size_t slash = name.find_last_of('/');
    return slash == std::string::npos ? std::string("textures") : name.substr(0, slash);
}

int32_t EnsureBrowserFolder(std::vector<EditorApp::TextureBrowserFolder> &folders,
                            std::unordered_map<std::string, int32_t> &indexByPath,
                            const std::string &path)
{
    const auto found = indexByPath.find(path);

    if (found != indexByPath.end())
    {
        return found->second;
    }

    const size_t slash = path.find_last_of('/');
    const int32_t parent = slash == std::string::npos
                               ? -1
                               : EnsureBrowserFolder(folders, indexByPath, path.substr(0, slash));

    EditorApp::TextureBrowserFolder folder;
    folder.path = path;
    folder.label = slash == std::string::npos ? path : path.substr(slash + 1);
    folder.parent = parent;

    const int32_t idx = int32_t(folders.size());
    folders.push_back(std::move(folder));
    indexByPath.emplace(path, idx);

    if (parent >= 0)
    {
        folders[size_t(parent)].children.push_back(idx);
    }

    return idx;
}

bool IsBrowserFolderAncestor(const std::string &parent, const std::string &child)
{
    return child.size() > parent.size() && child.compare(0, parent.size(), parent) == 0 &&
           child[parent.size()] == '/';
}

void SortBrowserFolders(std::vector<EditorApp::TextureBrowserFolder> &folders)
{
    for (EditorApp::TextureBrowserFolder &folder : folders)
    {
        std::sort(folder.children.begin(), folder.children.end(),
                  [&folders](int32_t a, int32_t b) {
                      return LowercaseName(folders[size_t(a)].label) <
                             LowercaseName(folders[size_t(b)].label);
                  });
    }
}

void DrawBrowserFolderNode(const std::vector<EditorApp::TextureBrowserFolder> &folders,
                           int32_t index, int32_t &activeFolder, char (&search)[64])
{
    const EditorApp::TextureBrowserFolder &folder = folders[size_t(index)];
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;

    if (folder.parent < 0 ||
        (activeFolder >= 0 &&
         IsBrowserFolderAncestor(folder.path, folders[size_t(activeFolder)].path)))
    {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }

    if (activeFolder == index)
    {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    if (folder.children.empty())
    {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }

    const bool open = ImGui::TreeNodeEx(folder.path.c_str(), flags, "%s", folder.label.c_str());

    if (ImGui::IsItemClicked())
    {
        activeFolder = index;
        search[0] = '\0';
    }

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", folder.path.c_str());
    }

    if ((flags & ImGuiTreeNodeFlags_NoTreePushOnOpen) == 0 && open)
    {
        for (int32_t child : folder.children)
        {
            DrawBrowserFolderNode(folders, child, activeFolder, search);
        }

        ImGui::TreePop();
    }
}

void DrawBrowserFolderPane(const std::vector<EditorApp::TextureBrowserFolder> &folders,
                           float width, float height, int32_t &activeFolder, char (&search)[64])
{
    ImGui::BeginChild("##texture_folders", ImVec2(width, height), true);
    ImGui::TextDisabled("Folders");
    ImGui::Separator();

    if (ImGui::Selectable("All textures", activeFolder < 0))
    {
        activeFolder = -1;
        search[0] = '\0';
    }

    std::vector<int32_t> roots;

    for (size_t i = 0; i < folders.size(); i++)
    {
        if (folders[i].parent < 0)
        {
            roots.push_back(int32_t(i));
        }
    }

    std::sort(roots.begin(), roots.end(), [&folders](int32_t a, int32_t b) {
        return LowercaseName(folders[size_t(a)].label) < LowercaseName(folders[size_t(b)].label);
    });

    for (int32_t root : roots)
    {
        DrawBrowserFolderNode(folders, root, activeFolder, search);
    }

    ImGui::EndChild();
}

std::string EllipsizeTileLabel(const std::string &name, float maxWidth)
{
    std::string label = name;

    if (ImGui::CalcTextSize(label.c_str()).x <= maxWidth)
    {
        return label;
    }

    while (!label.empty() && ImGui::CalcTextSize((label + "...").c_str()).x > maxWidth)
    {
        label.pop_back();
    }

    return label + "...";
}

int32_t FindCatalogSelectionIndex(const std::vector<EditorApp::TextureBrowserItem> &catalog,
                                  const std::string &target)
{
    const std::string key = NormalizeAssetKey(target);

    if (key.empty() || key == "none")
    {
        return -1;
    }

    for (size_t i = 0; i < catalog.size(); i++)
    {
        if (NormalizeAssetKey(catalog[i].name) == key)
        {
            return int32_t(i);
        }
    }

    const std::string base = key.rfind("textures/", 0) == 0 ? key.substr(9) : key;

    for (size_t i = 0; i < catalog.size(); i++)
    {
        const std::string itemKey = NormalizeAssetKey(catalog[i].name);

        if (itemKey == base || itemKey == "textures/" + base)
        {
            return int32_t(i);
        }
    }

    return -1;
}

bool MatchesBrowserFilters(const std::vector<EditorApp::TextureBrowserFolder> &folders,
                           const EditorApp::TextureBrowserItem &item, int32_t activeFolder,
                           const std::string &filter)
{
    if (activeFolder >= 0 && size_t(activeFolder) < folders.size())
    {
        if (item.folderIndex < 0 || size_t(item.folderIndex) >= folders.size())
        {
            return false;
        }

        const std::string &selected = folders[size_t(activeFolder)].path;
        const std::string &candidate = folders[size_t(item.folderIndex)].path;

        if (candidate != selected && !IsBrowserFolderAncestor(selected, candidate))
        {
            return false;
        }
    }

    return filter.empty() || LowercaseName(item.name).find(filter) != std::string::npos;
}

struct BrowserTileInput
{
    int32_t itemIndex = -1;
    const std::string *name = nullptr;
    QrMaterial material = QR_NO_MATERIAL;
    bool failed = false;
    bool selected = false;
    bool logFirstRect = false;
};

void DrawTextureBrowserTile(const BrowserTileInput &tileInput, float imageSize,
                            int32_t &outSelection, int32_t &outAction,
                            const std::function<void(const char *)> &logRect)
{
    ImGui::PushID(tileInput.itemIndex);
    ImGui::BeginGroup();

    const ImVec2 tile = ImGui::GetCursorScreenPos();
    const ImVec2 tileMax(tile.x + imageSize, tile.y + imageSize);

    ImGui::InvisibleButton("##tile", ImVec2(imageSize, imageSize));

    if (ImGui::IsItemClicked())
    {
        outSelection = tileInput.itemIndex;

        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            outAction = 1;
        }
    }

    if (ImGui::IsItemHovered() && tileInput.name != nullptr)
    {
        ImGui::SetTooltip("%s", tileInput.name->c_str());
    }

    if (tileInput.logFirstRect)
    {
        logRect("texbrowse first tile");
    }

    ImDrawList *drawList = ImGui::GetWindowDrawList();

    if (tileInput.material != QR_NO_MATERIAL)
    {
        drawList->AddImage(ImTextureRef(ImTextureID(uintptr_t(tileInput.material))), tile, tileMax);
    }
    else
    {
        drawList->AddRectFilled(tile, tileMax, IM_COL32(28, 30, 36, 255));
        drawList->AddText(ImVec2(tile.x + 8.0f, tile.y + 8.0f), IM_COL32(130, 134, 145, 255),
                          tileInput.failed ? "no preview" : "loading...");
    }

    if (tileInput.selected)
    {
        drawList->AddRect(tile, tileMax, IM_COL32(255, 180, 60, 255), 0.0f, 0, 2.5f);
    }
    else if (ImGui::IsItemHovered())
    {
        drawList->AddRect(tile, tileMax, IM_COL32(210, 215, 230, 255), 0.0f, 0, 1.0f);
    }

    const std::string label =
        EllipsizeTileLabel(tileInput.name ? *tileInput.name : std::string(), imageSize);
    ImGui::TextUnformatted(label.c_str());
    ImGui::EndGroup();
    ImGui::PopID();
}

} // namespace

void EditorApp::RebuildTextureBrowserCatalog()
{
    m_textureBrowserCatalog.clear();
    m_textureBrowserFolders.clear();
    m_textureBrowserCatalogBuilt = true;

    std::unordered_set<std::string> seen;

    for (size_t i = 0; i < m_scene.textures.size(); i++)
    {
        const EditorTexture &tex = m_scene.textures[i];

        if (tex.isExternal || tex.name.empty() || i >= m_textureMaterials.size() ||
            m_textureMaterials[i] == QR_NO_MATERIAL)
        {
            continue;
        }

        const std::string key = NormalizeAssetKey(tex.name);

        if (!seen.insert(key).second)
        {
            continue;
        }

        m_textureBrowserCatalog.push_back(
            TextureBrowserItem{ tex.name, "level", BrowserTreeOf(tex.name), -1 });
    }

    std::vector<EditorAssetCatalogEntry> assetEntries;
    m_assets.ListTextureAssets(assetEntries);

    for (const EditorAssetCatalogEntry &entry : assetEntries)
    {
        const std::string key = NormalizeAssetKey(entry.path);

        if (!seen.insert(key).second)
        {
            continue;
        }

        m_textureBrowserCatalog.push_back(
            TextureBrowserItem{ entry.path, entry.source, entry.tree, -1 });
    }

    std::unordered_map<std::string, int32_t> folderIndex;

    for (TextureBrowserItem &item : m_textureBrowserCatalog)
    {
        const std::string source = item.source.empty() ? std::string("game") : item.source;
        const std::string folderPath = item.tree.empty() ? source : source + "/" + item.tree;
        item.folderIndex = EnsureBrowserFolder(m_textureBrowserFolders, folderIndex, folderPath);
    }

    SortBrowserFolders(m_textureBrowserFolders);
}

void EditorApp::OpenTextureBrowserIfRequested()
{
    int32_t targetIdx = !m_textureBrowserMaterial.empty()
                            ? m_materials.IndexOf(m_textureBrowserMaterial)
                            : m_selectedMaterial;

    if (!m_textureBrowserRequested || targetIdx < 0)
    {
        m_textureBrowserRequested = false;
        return;
    }

    m_textureBrowserRequested = false;
    m_textureBrowserMaterial = m_materials.NameAt(size_t(targetIdx));

    if (!m_textureBrowserCatalogBuilt)
    {
        RebuildTextureBrowserCatalog();
    }

    EditorMaterialParams &curParams = m_materials.At(size_t(targetIdx));
    std::string target = MaterialSlotRef(curParams, m_textureBrowserSlot);

    if (target.empty() && m_textureBrowserSlot == EditorTextureSlot::Base)
    {
        target = m_textureBrowserMaterial;
    }

    m_textureBrowserSelection = FindCatalogSelectionIndex(m_textureBrowserCatalog, target);
    m_textureBrowserFolder =
        (m_textureBrowserSelection >= 0 &&
         size_t(m_textureBrowserSelection) < m_textureBrowserCatalog.size())
            ? m_textureBrowserCatalog[size_t(m_textureBrowserSelection)].folderIndex
            : -1;
    m_textureBrowserSearch[0] = '\0';
    m_textureBrowserFocus = true;
    PrintMessage(("texture browser: slot of '" + m_textureBrowserMaterial + "'").c_str(), this);
}

int32_t EditorApp::DrawTextureBrowserGrid(const std::vector<int32_t> &visible, float height)
{
    int32_t action = 0;

    if (!ImGui::BeginChild("##texture_list", ImVec2(0.0f, height), true))
    {
        ImGui::EndChild();
        return 0;
    }

    if (visible.empty())
    {
        ImGui::TextDisabled("no textures match");
        ImGui::EndChild();
        return 0;
    }

    const float scale = EditorGuiScale();
    const float imageSize = 128.0f * scale;
    const float tileGap = 10.0f * scale;
    const float rowStep =
        imageSize + ImGui::GetTextLineHeight() + 10.0f * scale + ImGui::GetStyle().ItemSpacing.y;
    const ImVec2 start = ImGui::GetCursorPos();
    const int32_t columns =
        std::max(1, int((ImGui::GetContentRegionAvail().x + tileGap) / (imageSize + tileGap)));
    const int32_t rowCount = (int32_t(visible.size()) + columns - 1) / columns;
    const int32_t firstRow = std::max(0, int32_t(ImGui::GetScrollY() / rowStep) - 1);
    const int32_t lastRow =
        std::min(rowCount, firstRow + int32_t(ImGui::GetWindowHeight() / rowStep) + 4);
    int32_t loadsThisFrame = 0;

    for (int32_t row = firstRow; row < lastRow; row++)
    {
        ImGui::SetCursorPos(ImVec2(start.x, start.y + float(row) * rowStep));

        for (int32_t col = 0; col < columns; col++)
        {
            const int32_t visibleIdx = row * columns + col;

            if (visibleIdx >= int32_t(visible.size()))
            {
                break;
            }

            if (col > 0)
            {
                ImGui::SameLine(0.0f, tileGap);
            }

            const int32_t itemIdx = visible[size_t(visibleIdx)];
            const TextureBrowserItem &item = m_textureBrowserCatalog[size_t(itemIdx)];
            int32_t texIdx = TextureIndexByName(item.name);

            if (texIdx < 0 && loadsThisFrame < 8 &&
                m_missingExternalTextures.count(NormalizeAssetKey(item.name)) == 0)
            {
                texIdx = ResolveOrLoadTexture(item.name);
                loadsThisFrame++;
            }

            const QrMaterial previewMat =
                (texIdx >= 0 && size_t(texIdx) < m_textureMaterials.size())
                    ? m_textureMaterials[size_t(texIdx)]
                    : QR_NO_MATERIAL;
            const bool failed =
                previewMat == QR_NO_MATERIAL &&
                m_missingExternalTextures.count(NormalizeAssetKey(item.name)) != 0;

            DrawTextureBrowserTile(
                BrowserTileInput{
                    itemIdx,
                    &item.name,
                    previewMat,
                    failed,
                    itemIdx == m_textureBrowserSelection,
                    visibleIdx == 0,
                },
                imageSize, m_textureBrowserSelection, action,
                [this](const char *name) { LogRect(name); });
        }
    }

    ImGui::SetCursorPos(ImVec2(start.x, start.y + float(rowCount) * rowStep));
    ImGui::Dummy(ImVec2(1.0f, 1.0f));
    ImGui::EndChild();
    return action;
}

void EditorApp::DrawTextureBrowser()
{
    OpenTextureBrowserIfRequested();

    if (m_textureBrowserMaterial.empty())
    {
        return;
    }

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float scale = EditorGuiScale();
    const float margin = 12.0f * scale;
    const float topOffset = ImGui::GetFrameHeight() + margin;
    EditorGuiBackdrop(viewport->Size.x, viewport->Size.y, 0.58f);

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + margin, viewport->Pos.y + topOffset),
                            ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(
        ImVec2(viewport->Size.x * 0.72f, std::max(320.0f * scale, viewport->Size.y - topOffset - margin)),
        ImGuiCond_Appearing);

    if (m_textureBrowserFocus)
    {
        ImGui::SetNextWindowFocus();
    }

    bool open = true;
    int32_t action = 0;

    if (ImGui::Begin("Texture Browser", &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f), "TEXTURE BROWSER");
        ImGui::SameLine();
        const std::string activeMod = ActiveModDisplayName(m_gamePath);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("active mod: %s", activeMod.c_str());

        const float closeWidth = EditorGuiFitButtonWidth("Close", 72.0f);
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - closeWidth);

        if (ImGui::Button("Close", ImVec2(closeWidth, 0.0f)))
        {
            action = 2;
        }

        ImGui::Separator();

        if (m_textureBrowserFocus)
        {
            ImGui::SetKeyboardFocusHere();
            m_textureBrowserFocus = false;
        }

        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##texture_filter", "filter textures by name...",
                                 m_textureBrowserSearch, IM_ARRAYSIZE(m_textureBrowserSearch));
        LogRect("texbrowse filter");

        const std::string filter = LowercaseName(m_textureBrowserSearch);
        std::vector<int32_t> visible;
        visible.reserve(m_textureBrowserCatalog.size());

        for (size_t i = 0; i < m_textureBrowserCatalog.size(); i++)
        {
            if (MatchesBrowserFilters(m_textureBrowserFolders, m_textureBrowserCatalog[i],
                                      m_textureBrowserFolder, filter))
            {
                visible.push_back(int32_t(i));
            }
        }

        if (m_textureBrowserFolder >= 0 &&
            size_t(m_textureBrowserFolder) < m_textureBrowserFolders.size())
        {
            ImGui::TextDisabled("%d of %d textures - %s", int(visible.size()),
                                int(m_textureBrowserCatalog.size()),
                                m_textureBrowserFolders[size_t(m_textureBrowserFolder)].path.c_str());
        }
        else
        {
            ImGui::TextDisabled("%d of %d textures", int(visible.size()),
                                int(m_textureBrowserCatalog.size()));
        }

        const float footerHeight =
            ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() + 14.0f * scale;
        const float contentHeight = std::max(100.0f * scale, ImGui::GetContentRegionAvail().y - footerHeight);
        const float folderWidth = std::max(220.0f * scale, ImGui::GetContentRegionAvail().x * 0.30f);

        DrawBrowserFolderPane(m_textureBrowserFolders, folderWidth, contentHeight,
                              m_textureBrowserFolder, m_textureBrowserSearch);
        ImGui::SameLine();

        const int32_t gridAction = DrawTextureBrowserGrid(visible, contentHeight);

        if (gridAction != 0)
        {
            action = gridAction;
        }

        ImGui::Separator();

        const bool anySelected = m_textureBrowserSelection >= 0 &&
                                 size_t(m_textureBrowserSelection) < m_textureBrowserCatalog.size();

        if (anySelected)
        {
            ImGui::TextUnformatted(
                m_textureBrowserCatalog[size_t(m_textureBrowserSelection)].name.c_str());
        }
        else
        {
            ImGui::TextDisabled("no texture selected");
        }

        if (!anySelected)
        {
            ImGui::BeginDisabled();
        }

        if (ImGui::Button("Select", ImVec2(EditorGuiFitButtonWidth("Select", 96.0f), 0.0f)))
        {
            action = 1;
        }

        if (!anySelected)
        {
            ImGui::EndDisabled();
        }

        LogRect("texbrowse select");
        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(EditorGuiFitButtonWidth("Cancel", 96.0f), 0.0f)))
        {
            action = 2;
        }

        LogRect("texbrowse cancel");
    }

    ImGui::End();

    if (action == 0 && (!open || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
    {
        action = 2;
    }

    if (action == 0)
    {
        return;
    }

    if (action == 1)
    {
        ApplyTextureBrowserSelection();
    }

    m_textureBrowserMaterial.clear();
    m_textureBrowserSelection = -1;
}

void EditorApp::AutoEnableMaterialSlot(EditorMaterialParams &params, EditorTextureSlot slot)
{
    if (slot == EditorTextureSlot::Emissive)
    {
        params.emissiveEnabled = true;
    }
    else if (slot == EditorTextureSlot::Gloss)
    {
        params.glossEnabled = true;
    }
    else if (slot == EditorTextureSlot::Subsurface)
    {
        params.sssEnabled = true;
        if (params.sssStrength <= 0.0f)
        {
            params.sssStrength = 1.0f;
        }
    }
    else if (slot == EditorTextureSlot::Height && params.parallaxScale <= 0.0f)
    {
        params.parallaxScale = 0.04f;
    }
}

void EditorApp::ApplyTextureBrowserSelection()
{
    if (m_textureBrowserSelection < 0 ||
        size_t(m_textureBrowserSelection) >= m_textureBrowserCatalog.size())
    {
        return;
    }
    const int32_t materialIndex = m_materials.IndexOf(m_textureBrowserMaterial);
    if (materialIndex < 0)
    {
        return;
    }
    const std::string &chosen = m_textureBrowserCatalog[size_t(m_textureBrowserSelection)].name;
    EditorMaterialParams &params = m_materials.At(size_t(materialIndex));
    MaterialSlotRef(params, m_textureBrowserSlot) = chosen;
    if (!chosen.empty())
    {
        AutoEnableMaterialSlot(params, m_textureBrowserSlot);
    }
    ResolveOrLoadTexture(chosen);
    m_materialsChanged = true;
    TouchMaterial(m_textureBrowserMaterial);
    PrintMessage(("texture browser: slot '" + m_textureBrowserMaterial + "' -> " + chosen).c_str(),
                 this);
}

namespace
{

void AppendAdapterBaseMaterialsCandidates(const std::filesystem::path &modPath,
                                          std::vector<std::filesystem::path> &candidates)
{
    const EditorFormatAdapter *adapter = GetEditorFormatAdapter();
    if (adapter == nullptr)
    {
        return;
    }
    for (const std::string &baseDir : adapter->extraBaseGameDirs)
    {
        candidates.push_back(modPath.parent_path() / baseDir / "qray.materials.yaml");
        candidates.push_back(modPath.parent_path().parent_path() / baseDir /
                             "qray.materials.yaml");
    }
}

std::string ResolveBaseMaterialsYamlPath(const std::string &activeMaterialsPath,
                                         const std::string &gamePath,
                                         const std::string &initialGamePath)
{
    const std::filesystem::path activeCanon =
        activeMaterialsPath.empty() ? std::filesystem::path{}
                                    : CanonicalOrLexicalPath(std::filesystem::path(activeMaterialsPath));

    std::vector<std::filesystem::path> candidates;

    if (!gamePath.empty())
    {
        const std::filesystem::path modPath = std::filesystem::path(gamePath).lexically_normal();
        candidates.push_back(modPath.parent_path() / "id1" / "qray.materials.yaml");
        candidates.push_back(modPath.parent_path().parent_path() / "id1" / "qray.materials.yaml");
        AppendAdapterBaseMaterialsCandidates(modPath, candidates);
    }

    if (!initialGamePath.empty())
    {
        candidates.push_back(std::filesystem::path(initialGamePath).lexically_normal() /
                             "qray.materials.yaml");
    }

    std::error_code ec;

    for (const std::filesystem::path &candidate : candidates)
    {
        if (!std::filesystem::is_regular_file(candidate, ec))
        {
            continue;
        }

        if (CanonicalOrLexicalPath(candidate) != activeCanon)
        {
            return candidate.string();
        }
    }

    return {};
}

} // namespace

void EditorApp::LoadMaterialsSession()
{
    std::string error;

    m_touchedMaterials.clear();
    m_sessionDirty = false;
    m_baseMaterialsDocument = EditorMaterialsDocument{};
    m_baseMaterialsPath =
        ResolveBaseMaterialsYamlPath(m_sessionPaths.materials, m_gamePath, m_initialGamePath);

    if (!m_baseMaterialsPath.empty())
    {
        std::string baseError;

        if (!m_baseMaterialsDocument.Load(m_baseMaterialsPath, baseError))
        {
            PrintMessage(("base materials: " + baseError).c_str(), this);
            m_baseMaterialsPath.clear();
            m_baseMaterialsDocument = EditorMaterialsDocument{};
        }
    }

    if (!m_materialsDocument.Load(m_sessionPaths.materials, error))
    {
        PrintMessage(("session: " + error).c_str(), this);
        Notify(error);
        return;
    }

    size_t loaded = 0;
    size_t inherited = 0;
    m_runtime.ApplyMaterialsDocuments(m_materialsDocument,
                                      m_baseMaterialsPath.empty() ? nullptr : &m_baseMaterialsDocument,
                                      &loaded, &inherited);

    const std::string message = "session: " + m_sessionPaths.materials +
                                (m_materialsDocument.Exists() ? " read, " : " absent, new file on apply, ") +
                                std::to_string(loaded) + " entries" +
                                (inherited > 0 ? (" (+" + std::to_string(inherited) + " inherited from " +
                                                  m_baseMaterialsPath + ")")
                                               : std::string{});
    PrintMessage(message.c_str(), this);
}

namespace
{

struct SkyFloatBinding
{
    const char *name;
    float EditorRenderSettings::*field;
};

struct SkyBoolBinding
{
    const char *name;
    bool EditorRenderSettings::*field;
};

struct SkyColorBinding
{
    const char *name;
    float (EditorRenderSettings::*field)[3];
};

constexpr SkyFloatBinding kSkyFloatBindings[] = {
    { "rt_sky", &EditorRenderSettings::skyScale },
    { "rt_sky_brightness", &EditorRenderSettings::skyBrightness },
    { "rt_sky_light_mult", &EditorRenderSettings::skyLightMultiplier },
    { "rt_sky_clouds_alpha", &EditorRenderSettings::cloudsAlpha },
    { "rt_sky_clouds_coverage", &EditorRenderSettings::cloudsCoverage },
    { "rt_sky_clouds_density", &EditorRenderSettings::cloudsDensity },
    { "rt_sky_clouds_speed", &EditorRenderSettings::cloudsSpeed },
    { "rt_sky_clouds_height", &EditorRenderSettings::cloudsHeight },
    { "rt_sky_clouds_thickness", &EditorRenderSettings::cloudsThickness },
    { "rt_sky_sun_size", &EditorRenderSettings::sunDiscSize },
    { "rt_sky_godrays_intensity", &EditorRenderSettings::godRaysIntensity },
    { "rt_sky_godrays_sky_threshold", &EditorRenderSettings::godRaysSkyThreshold },
    { "rt_sky_time", &EditorRenderSettings::timeOfDay },
    { "rt_sky_time_speed", &EditorRenderSettings::timeOfDaySpeed },
    { "rt_sky_latitude", &EditorRenderSettings::skyLatitude },
    { "rt_sky_azimuth", &EditorRenderSettings::skyAzimuth },
    { "rt_sky_atmosphere_density", &EditorRenderSettings::atmosphereDensity },
    { "rt_sky_mie", &EditorRenderSettings::atmosphereMie },
    { "rt_sky_ozone", &EditorRenderSettings::atmosphereOzone },
    { "rt_sky_stars_density", &EditorRenderSettings::starDensity },
    { "rt_sky_stars", &EditorRenderSettings::starIntensity },
    { "rt_sky_moon", &EditorRenderSettings::moonIntensity },
    { "rt_sky_moon_size", &EditorRenderSettings::moonSize },
    { "rt_sky_moon_phase", &EditorRenderSettings::moonPhase },
};

constexpr SkyBoolBinding kSkyBoolBindings[] = {
    { "rt_sky_nee", &EditorRenderSettings::skyNee },
    { "rt_sky_clouds", &EditorRenderSettings::clouds },
    { "rt_sky_godrays", &EditorRenderSettings::godRays },
    { "rt_sky_time_enable", &EditorRenderSettings::timeOfDayEnable },
    { "rt_sky_stars_enable", &EditorRenderSettings::starsEnabled },
};

constexpr SkyColorBinding kSkyColorBindings[] = {
    { "rt_sky_color", &EditorRenderSettings::skyColor },
    { "rt_sky_clouds_color", &EditorRenderSettings::cloudsColor },
    { "rt_sky_sun_color", &EditorRenderSettings::sunColor },
    { "rt_sky_moon_color", &EditorRenderSettings::moonColor },
};

void FormatSkyBoundText(const EditorRenderSettings &settings, const std::string &name,
                        std::string &text)
{
    for (const SkyColorBinding &binding : kSkyColorBindings)
    {
        if (name == binding.name)
        {
            text = FormatSkyText(settings.*(binding.field));
            return;
        }
    }

    for (const SkyBoolBinding &binding : kSkyBoolBindings)
    {
        if (name == binding.name)
        {
            text = FormatSkyText(settings.*(binding.field));
            return;
        }
    }

    for (const SkyFloatBinding &binding : kSkyFloatBindings)
    {
        if (name == binding.name)
        {
            text = FormatSkyText(settings.*(binding.field));
            return;
        }
    }
}

// One `sky:` key as the block states it, taken from the values the panel edits. A name the block
// does not carry leaves `text` alone, so the two lists cannot drift into writing a value to a key
// that means something else.
void SkyTextOf(const EditorRenderSettings &settings, const EditorSunState &sun,
               const std::string &name, std::string &text)
{
    if (name == "rt_sky_ambient_lod")
    {
        text = FormatSkyText(settings.skyAmbientLod);
    }
    else if (name == "rt_physical_sky")
    {
        // The block carries the one sky type it can express: r_fastsky's colour sky has no key.
        text = FormatSkyText(settings.skyType == QR_SKY_TYPE_PROCEDURAL);
    }
    else if (name == "rt_sky_clouds_quality")
    {
        text = FormatSkyText(settings.cloudsQuality);
    }
    else if (name == "rt_physical_sun")
    {
        text = FormatSkyText(sun.enabled);
    }
    else if (name == "rt_sky_sun")
    {
        // The panel holds the radiance the light is uploaded with, the block the cvar multiplier.
        text = FormatSkyText(sun.radiance / kEditorSkySunRadiance);
    }
    else if (name == "rt_sky_sun_pitch")
    {
        text = FormatSkyText(sun.pitch);
    }
    else if (name == "rt_sky_sun_yaw")
    {
        text = FormatSkyText(sun.yaw);
    }
    else
    {
        FormatSkyBoundText(settings, name, text);
    }
}

// The live state as the block would read it, every key set. This is what a capture compares a
// key against, so an edit is recognised by the text the file would hold rather than by the float.
EditorLevelSky RenderedSky(const EditorRenderSettings &settings, const EditorSunState &sun)
{
    EditorLevelSky sky;

    for (int32_t i = 0; i < kEditorSkyKeyCount; i++)
    {
        SkyTextOf(settings, sun, kEditorSkyKeyNames[i], sky.values[i]);
        sky.set[i] = !sky.values[i].empty();
    }

    return sky;
}

} // namespace

EditorLevelFog EditorApp::LevelFog() const
{
    EditorLevelFog fog;

    for (int axis = 0; axis < 3; axis++)
    {
        fog.color[axis] = m_settings.fogColor[axis];
    }

    fog.density = std::max(0.0f, m_settings.fogDensity);
    // The panel has one density slider and no second switch, so a non-zero density is what shows
    // the fog: a file that says `enabled: false` reads back as density 0 rather than as a value
    // the level does not draw.
    fog.enabled = fog.density > 0.0f;
    return fog;
}

EditorLevelLight EditorApp::LevelLight(const EditorLight &light, bool isCustom) const
{
    EditorLevelLight level;

    for (int axis = 0; axis < 3; axis++)
    {
        level.origin[axis] = light.position[axis];
        level.offset[axis] = light.offset[axis];
        level.color[axis] = light.tint[axis];
        level.direction[axis] = light.direction[axis];
    }

    if (isCustom)
    {
        level.className = "custom";
    }
    else
    {
        level.className = (!light.className.empty() && LowercaseName(light.className) != "custom")
                              ? light.className
                              : std::string("light");
    }

    level.hasOffset = light.hasOffset;
    level.radius = light.radius / kEditorUnitsPerMeter;
    level.intensity = light.intensity / kEditorCustomLightIntensityUnits;
    level.spot = light.spot;
    level.angleInner = light.angleInner;
    level.angleOuter = light.angleOuter;
    level.lightShafts = light.lightShafts;
    level.lightShaftsPower = light.lightShaftsPower;
    level.lightShaftsDistance = light.lightShaftsDistance;
    level.lightShaftsFalloff = light.lightShaftsFalloff;
    level.style = light.style;
    level.forceRasterize = light.forceRasterize;
    level.enabled = light.enabled;
    return level;
}

std::vector<EditorLevelLight> EditorApp::LevelLights() const
{
    std::vector<EditorLevelLight> lights;
    lights.reserve(m_scene.lights.size() + m_customLights.size());

    for (const EditorLight &light : m_scene.lights)
    {
        if (light.emitterUniqueID == 0)
        {
            lights.push_back(LevelLight(light, false));
        }
    }

    for (const EditorLight &light : m_customLights)
    {
        lights.push_back(LevelLight(light, true));
    }

    return lights;
}

bool EditorApp::ShouldWriteEmptyLightsBlock() const
{
    return !m_scene.bspLights.empty() || m_levelDocument.ReadLevel(m_sessionPaths.level).hasLights;
}

EditorLevelSky EditorApp::LevelSky() const
{
    EditorLevelSky sky;

    // The global scope is the config, so the file carries no block at all; the game's checkbox
    // reads the same way.
    if (m_skyGlobal)
    {
        return sky;
    }

    const EditorLevelSky live = RenderedSky(m_settings, m_sun);

    for (int32_t i = 0; i < kEditorSkyKeyCount; i++)
    {
        // A key nobody touched writes back the text the file already holds, which is what keeps a
        // load and save of the same level byte for byte - the panel's float would not survive the
        // trip through the console's own formatting.
        const bool edited = live.values[i] != m_skyRendered.values[i];
        const std::string &text = edited ? live.values[i] : m_skyLevel.values[i];

        // An edited key that says what the global scope says is not an override, so it goes back
        // to following the config - the comparison RT_SkySettings_CaptureScope makes.
        if (text.empty() || (edited && text == m_skyGlobals.values[i]))
        {
            continue;
        }

        sky.values[i] = text;
        sky.set[i] = true;
    }

    return sky;
}

void EditorApp::CaptureSky()
{
    if (m_skyGlobal)
    {
        // The global values are the baseline a level key is dropped against, so the panel's own
        // numbers become what a level has to differ from to be written.
        m_skyGlobals = RenderedSky(m_settings, m_sun);
    }
    else
    {
        m_skyLevel = LevelSky();
    }

    m_skyRendered = RenderedSky(m_settings, m_sun);
}

void EditorApp::SetSkyScope(bool global)
{
    if (global == m_skyGlobal)
    {
        return;
    }

    // The scope being left keeps whatever the panel holds before the other one is laid over it.
    CaptureSky();
    m_skyGlobal = global;

    if (global)
    {
        // Every key, because the global scope owns the whole block: the level's overrides do not
        // stand in for a config value the panel is editing.
        ApplyEditorSkyScope(m_skyGlobals, false, m_settings, m_sun);
    }
    else
    {
        // The panel already shows the global values, so only the level's own keys change.
        ApplyEditorSkyScope(m_skyLevel, true, m_settings, m_sun);
    }

    m_skyRendered = RenderedSky(m_settings, m_sun);

    const std::string message =
        std::string("sky: ") +
        (global ? std::string("global settings") : "level " + m_sessionPaths.level);
    PrintMessage(message.c_str(), this);
    Notify(message);
}

std::string EditorApp::LevelFingerprint() const
{
    return RenderEditorLevelSection(m_sessionPaths.level, LevelSky(), LevelFog(), LevelLights(),
                                    ShouldWriteEmptyLightsBlock()) +
           RenderEmitterLightsBlock(m_emitterLightOverrides);
}

void EditorApp::LoadLightsSession()
{
    std::string error;

    if (!m_levelDocument.Load(m_sessionPaths.lights, error))
    {
        PrintMessage(("session: " + error).c_str(), this);
        Notify(error);
        return;
    }

    // The values a level key is dropped against are the game's config, which the light editor
    // reads as its global set. The config is read by LoadGameConfigGlobals before the first level
    // section, so the panel's state already is that global set; a run that found no config falls
    // back to the defaults the panel holds, which are the engine's own cvar values.
    if (m_skyGlobals.Empty())
    {
        m_skyGlobals = RenderedSky(m_settings, m_sun);
    }

    const EditorLevelApplyResult applied =
        m_runtime.ApplyLevelDocument(m_levelDocument, m_sessionPaths.level);
    AdoptSkyScope(applied.read);

    if (applied.hasLights)
    {
        if (applied.replacedStandardLights)
        {
            m_selectedLight = -1;
            m_selectedLights.clear();
        }
        m_selectedCustomLight = -1;
        m_selectedCustomLights.clear();
    }

    m_levelTouched = false;
    m_levelFingerprint = LevelFingerprint();

    const std::string message = "session: " + m_sessionPaths.lights +
                                (m_levelDocument.Exists() ? " read, " : " absent, new file on apply, ") +
                                std::to_string(m_scene.lights.size()) + " entity and " +
                                std::to_string(m_customLights.size()) + " custom lights for " +
                                m_sessionPaths.level + ", sky scope " +
                                (m_skyGlobal ? "global" : "level");
    PrintMessage(message.c_str(), this);
}

namespace
{

void TrimLeadingSpaceAndSetPrefix(std::string &name, std::string *outSetPrefix = nullptr)
{
    auto trimFront = [](std::string &s) {
        size_t first = 0;
        while (first < s.size() && (s[first] == ' ' || s[first] == '\t' || s[first] == '\r'))
        {
            first++;
        }
        if (first > 0)
        {
            s.erase(0, first);
        }
    };
    trimFront(name);
    for (const char *prefix : { "seta ", "seta\t", "set ", "set\t" })
    {
        const size_t len = std::strlen(prefix);
        if (name.compare(0, len, prefix) == 0)
        {
            if (outSetPrefix != nullptr)
            {
                *outSetPrefix = name.substr(0, len - 1) + " ";
            }
            name.erase(0, len);
            trimFront(name);
            break;
        }
    }
}

bool ParseConfigCvarLine(const std::string &line, std::string &name, std::string &value,
                         std::string *outSetPrefix = nullptr)
{
    if (outSetPrefix != nullptr)
    {
        outSetPrefix->clear();
    }
    const size_t open = line.find('"');

    if (open == std::string::npos || open == 0)
    {
        return false;
    }

    const size_t close = line.find('"', open + 1);

    if (close == std::string::npos)
    {
        return false;
    }

    name = line.substr(0, open);
    value = line.substr(open + 1, close - open - 1);

    while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\r'))
    {
        name.pop_back();
    }
    TrimLeadingSpaceAndSetPrefix(name, outSetPrefix);

    return !name.empty() && name[0] != '/' && name[0] != '#';
}

std::string FormatConfigFloat(float value)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6g", double(value));
    return buf;
}

using ConfigEntryList = std::vector<std::pair<std::string, std::string>>;

void CollectDtalConfigEntries(const EditorRenderSettings &s, ConfigEntryList &out)
{
    out.emplace_back("rt_pause_world", s.pauseWorld ? "1" : "0");
    out.emplace_back("rt_dtal_debug", std::to_string(s.dtalDebug));
    out.emplace_back("rt_dtal_proxy", s.dtalProxy ? "1" : "0");
    out.emplace_back("rt_dtal_clearance", FormatConfigFloat(s.dtalClearance));
    out.emplace_back("rt_dtal_minarea", FormatConfigFloat(s.dtalMinArea));
    out.emplace_back("rt_dtal_proxy_minarea", FormatConfigFloat(s.dtalProxyMinArea));
    out.emplace_back("rt_dtal_proxy_model_minarea", FormatConfigFloat(s.dtalProxyModelMinArea));
    out.emplace_back("rt_dtal_spacing", FormatConfigFloat(s.dtalSpacing));
    out.emplace_back("rt_dtal_model_minarea", FormatConfigFloat(s.dtalModelMinArea));
    out.emplace_back("rt_dtal_maxpolys", std::to_string(s.dtalMaxPolys));
    out.emplace_back("rt_dtal_groups", std::to_string(s.dtalGroups));
    out.emplace_back("rt_dtal_maxgroups", std::to_string(s.dtalMaxGroups));
    out.emplace_back("rt_level_light_maxpolys", std::to_string(s.levelLightMaxPolys));
    out.emplace_back("rt_dtal_model_maxpolys", std::to_string(s.dtalModelMaxPolys));
    out.emplace_back("rt_dtal_model_budget", std::to_string(s.dtalModelBudget));
}

void CollectRenderAndGlobalConfigEntries(const EditorRenderSettings &s, ConfigEntryList &out)
{
    struct FPair
    {
        const char *name;
        float value;
    };
    const FPair floats[] = {
        { "rt_gi_level", s.giLevel },
        { "rt_sky_sun_bounce_range", s.sunBounceRange },
        { "rt_sky_sun_bounce_scale", s.sunBounceScale },
        { "rt_restir", s.restir },
        { "rt_restir_candidates", s.restirCandidates },
        { "rt_antifirefly", s.antifirefly },
        { "rt_sensit_indir", s.sensitivityIndirect },
        { "rt_light_shafts_intensity", s.lightShaftsIntensity },
        { "rt_renderscale", s.renderScale },
        { "rt_denoiser", s.denoiser },
        { "rt_nee_samples", s.neeSamples },
        { "sensitivity", s.mouseSensitivity },
        { "rt_normalmap_stren", s.normalMapStrength },
        { "rt_filmgrain", s.filmGrainIntensity },
        { "rt_truelight", s.truelight },
        { "rt_brightness", s.brightness },
        { "rt_globallight_mult", s.globalLightMult },
        { "rt_emis_mapboost", s.emissionMapBoost },
        { "rt_emis_light_intensity", s.emissionLightIntensity },
        { "rt_tonemap_white", s.tonemapWhite },
        { "rt_tonemap_chroma", s.tonemapChroma },
        { "rt_tonemap_desat", s.tonemapDesat },
        { "rt_tonemap_max_log", s.tonemapMaxLog },
        { "rt_tonemap_min_log", s.tonemapMinLog },
        { "rt_volume_scatter", s.volumeScatter },
        { "rt_volume_ambient", s.volumeAmbient },
        { "rt_volume_far", s.volumeFar },
    };
    for (const FPair &item : floats)
    {
        out.emplace_back(item.name, FormatConfigFloat(item.value));
    }
    out.emplace_back("rt_light_shafts", s.lightShafts ? "1" : "0");
    out.emplace_back("rt_emissive_nee", s.emissiveNee ? "1" : "0");
    out.emplace_back("rt_light_shafts_resolution", std::to_string(s.lightShaftsResolution));
    out.emplace_back("rt_upscale_fsr31", std::to_string(s.upscaleFsr31));
    out.emplace_back("rt_sharpen", std::to_string(s.sharpenMode));
    out.emplace_back("rt_shadowrays", std::to_string(s.shadowRays));
    out.emplace_back("rt_model_lights", s.modelLights ? "1" : "0");
    out.emplace_back("rt_level_lights", s.levelLights ? "1" : "0");
    out.emplace_back("rt_tonemap", std::to_string(s.tonemapType));
    out.emplace_back("rt_volume_type", std::to_string(s.volumeType));
}

ConfigEntryList CollectSystemConfigEntries(const EditorRenderSettings &s,
                                           const EditorSunState &sun, bool includeSky)
{
    ConfigEntryList out;
    CollectDtalConfigEntries(s, out);
    CollectRenderAndGlobalConfigEntries(s, out);
    if (includeSky)
    {
        const EditorLevelSky sky = RenderedSky(s, sun);
        for (int32_t i = 0; i < kEditorSkyKeyCount; i++)
        {
            if (!sky.values[i].empty())
            {
                out.emplace_back(kEditorSkyKeyNames[i], sky.values[i]);
            }
        }
    }
    return out;
}

std::string MergeConfigCvarText(const std::string &existing, const ConfigEntryList &entries)
{
    std::unordered_map<std::string, size_t> indexByName;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        indexByName[entries[i].first] = i;
    }
    std::vector<bool> written(entries.size(), false);
    std::string result;
    result.reserve(existing.size() + entries.size() * 32u);

    size_t offset = 0;
    std::string name;
    std::string value;
    std::string setPrefix;
    while (offset < existing.size())
    {
        const size_t end = existing.find('\n', offset);
        const bool hasNl = end != std::string::npos;
        const size_t lineEnd = hasNl ? end : existing.size();
        std::string line = existing.substr(offset, lineEnd - offset);
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        offset = hasNl ? (end + 1u) : existing.size();

        if (ParseConfigCvarLine(line, name, value, &setPrefix))
        {
            const auto it = indexByName.find(name);
            if (it != indexByName.end())
            {
                result += setPrefix + name + " \"" + entries[it->second].second + "\"\n";
                written[it->second] = true;
                continue;
            }
        }
        result += line;
        result += '\n';
    }

    for (size_t i = 0; i < entries.size(); ++i)
    {
        if (!written[i])
        {
            result += entries[i].first + " \"" + entries[i].second + "\"\n";
        }
    }
    return result;
}

} // namespace

void EditorApp::LoadGameConfigGlobals()
{
    if (m_gameConfigApplied)
    {
        return;
    }

    std::vector<uint8_t> file;
    bool loadedAny = m_assets.ReadFile("config.cfg", file);
    if (!loadedAny)
    {
        if (const EditorFormatAdapter *adapter = GetEditorFormatAdapter(); adapter != nullptr)
        {
            for (const std::string &cfg : adapter->extraModConfigFiles)
            {
                if (m_assets.ReadFile(cfg, file))
                {
                    loadedAny = true;
                    break;
                }
            }
        }
    }

    int applied = 0;
    if (loadedAny)
    {
        const std::string text(file.begin(), file.end());
        applied += m_runtime.ApplyConfigText(text);
    }

    const std::string diskCfgPath = ResolveSystemConfigPath();
    if (!diskCfgPath.empty())
    {
        std::ifstream diskFile(diskCfgPath, std::ios::binary);
        if (diskFile)
        {
            const std::string diskText((std::istreambuf_iterator<char>(diskFile)),
                                       std::istreambuf_iterator<char>());
            if (!diskText.empty())
            {
                loadedAny = true;
                applied += m_runtime.ApplyConfigText(diskText);
            }
        }
    }

    if (!loadedAny)
    {
        PrintMessage("config: no config.cfg found, the panel starts from the engine's own cvar "
                     "values rather than from the game's global set",
                     this);
        return;
    }

    m_gameConfigApplied = true;

    if (m_settings.skyBrightness <= 0.001f)
    {
        m_settings.skyBrightness = 1.0f;
    }

    char message[128];
    std::snprintf(message, sizeof(message), "config: %d global values applied", applied);
    PrintMessage(message, this);

    // The lighting model decides whether a light entity is a light source at all, so say which one
    // the file asked for: a viewport that ignores it looks lit when the game's frame is not.
    if (m_settings.truelight > 0.0f && !m_scene.usesExternalLightingModel)
    {
        std::snprintf(message, sizeof(message),
                      "lights: materials lighting (rt_truelight %.1f), %d map lights not uploaded",
                      double(m_settings.truelight), int(m_scene.lights.size()));
        PrintMessage(message, this);
    }
}

void EditorApp::AdoptSkyScope(const EditorLevelDocument::ReadResult &read)
{
    // A level that carries a `sky:` block opens in level scope, the way the game's editor leaves
    // its checkbox off for it; one without a block has nothing to override and opens global.
    m_skyGlobal = !read.hasSky;
    m_skyLevel = read.hasSky ? read.sky : EditorLevelSky{};

    // The level's keys go over the panel as it stands, which is the global baseline a key the
    // block does not carry falls back to.
    ApplyEditorSkyScope(m_skyLevel, true, m_settings, m_sun);
    m_skyRendered = RenderedSky(m_settings, m_sun);
}

bool EditorApp::IsMaterialInActiveMod(const std::string &name) const
{
    return m_materialsDocument.HasMaterial(name) ||
           std::find(m_touchedMaterials.begin(), m_touchedMaterials.end(), name) !=
               m_touchedMaterials.end();
}

bool EditorApp::IsMaterialFromBaseMod(const std::string &name) const
{
    return !IsMaterialInActiveMod(name) && m_baseMaterialsDocument.HasMaterial(name);
}

namespace
{

std::string ModelCatalogPrefixForLookup(const std::string &lowerPath)
{
    if (const EditorFormatAdapter *adapter = GetEditorFormatAdapter();
        adapter != nullptr && adapter->resolveModelMaterialPrefix != nullptr)
    {
        std::string prefix;
        if (adapter->resolveModelMaterialPrefix(lowerPath, prefix))
        {
            return prefix;
        }
    }
    return {};
}

bool HasTouchedMaterialWithPrefix(const std::vector<std::string> &touched,
                                  const std::string &prefixLower)
{
    for (const std::string &name : touched)
    {
        const std::string lower = LowercaseName(name);
        if (lower.compare(0, prefixLower.size(), prefixLower) == 0)
        {
            return true;
        }
    }
    return false;
}

} // namespace

bool EditorApp::IsPreviewPresetInActiveMod(int32_t skinIndex) const
{
    if (!m_previewModel.hasMultiSections || skinIndex < 0 ||
        size_t(skinIndex) >= m_previewModel.skinPresets.size())
    {
        return false;
    }
    for (const auto &kv : m_previewModel.skinPresets[size_t(skinIndex)].surfaceMaterials)
    {
        if (!kv.second.empty() && IsMaterialInActiveMod(kv.second))
        {
            return true;
        }
    }
    return false;
}

bool EditorApp::IsPreviewPresetFromBaseMod(int32_t skinIndex) const
{
    if (IsPreviewPresetInActiveMod(skinIndex) || !m_previewModel.hasMultiSections || skinIndex < 0 ||
        size_t(skinIndex) >= m_previewModel.skinPresets.size())
    {
        return false;
    }
    for (const auto &kv : m_previewModel.skinPresets[size_t(skinIndex)].surfaceMaterials)
    {
        if (!kv.second.empty() && IsMaterialFromBaseMod(kv.second))
        {
            return true;
        }
    }
    return false;
}

bool EditorApp::IsModelCatalogEntryInActiveMod(const EditorAssetCatalogEntry &entry) const
{
    const std::string lower = LowercaseName(entry.path);
    const std::string prefix = ModelCatalogPrefixForLookup(lower);
    if (prefix.empty())
    {
        return IsMaterialInActiveMod(lower + ":frame0");
    }

    if (m_previewModelLoaded && LowercaseName(m_previewModelPath) == lower)
    {
        for (size_t i = 0; i < m_previewModel.skinPresets.size(); i++)
        {
            if (IsPreviewPresetInActiveMod(int32_t(i)))
            {
                return true;
            }
        }
        for (const EditorTexture &tex : m_previewModel.surfaceTextures)
        {
            if (IsMaterialInActiveMod(tex.name))
            {
                return true;
            }
        }
    }

    return m_materialsDocument.HasMaterialWithPrefix(prefix) ||
           HasTouchedMaterialWithPrefix(m_touchedMaterials, prefix);
}

bool EditorApp::IsModelCatalogEntryFromBaseMod(const EditorAssetCatalogEntry &entry) const
{
    if (IsModelCatalogEntryInActiveMod(entry))
    {
        return false;
    }

    const std::string lower = LowercaseName(entry.path);
    const std::string prefix = ModelCatalogPrefixForLookup(lower);
    if (prefix.empty())
    {
        return IsMaterialFromBaseMod(lower + ":frame0");
    }

    if (m_previewModelLoaded && LowercaseName(m_previewModelPath) == lower)
    {
        for (size_t i = 0; i < m_previewModel.skinPresets.size(); i++)
        {
            if (IsPreviewPresetFromBaseMod(int32_t(i)))
            {
                return true;
            }
        }
        for (const EditorTexture &tex : m_previewModel.surfaceTextures)
        {
            if (IsMaterialFromBaseMod(tex.name))
            {
                return true;
            }
        }
    }

    return m_baseMaterialsDocument.HasMaterialWithPrefix(prefix);
}

std::string EditorApp::FormatMaterialListLabel(const std::string &name) const
{
    std::string label = name;
    if (IsMaterialInActiveMod(name))
    {
        label += "  *";
    }
    else if (IsMaterialFromBaseMod(name))
    {
        label += "  [base]";
    }

    const int32_t idx = m_materials.IndexOf(name);
    if (idx >= 0 && !m_materials.At(size_t(idx)).enabled)
    {
        label += "  [off]";
    }
    return label;
}

void EditorApp::TouchMaterial(const std::string &name)
{
    if (std::find(m_touchedMaterials.begin(), m_touchedMaterials.end(), name) == m_touchedMaterials.end())
    {
        m_touchedMaterials.push_back(name);
        PrintMessage(("session: touched " + name).c_str(), this);
    }

    m_sessionDirty = true;
}

bool EditorApp::SaveMaterialsSession()
{
    std::vector<EditorMaterialEntry> entries;
    entries.reserve(m_touchedMaterials.size());

    for (const std::string &name : m_touchedMaterials)
    {
        const int32_t index = m_materials.IndexOf(name);
        if (index >= 0)
        {
            const EditorMaterialParams sceneDefaults = m_runtime.DefaultMaterialParamsFor(name);
            const EditorMaterialParams &params = m_materials.At(size_t(index));
            EditorMaterialEntry entry;
            entry.name = name;
            entry.params = params;
            entry.clearDefaultEmissive =
                !sceneDefaults.emissiveTexture.empty() && params.emissiveTexture.empty();
            entry.clearDefaultMirror = sceneDefaults.mirror && !params.mirror;
            entries.push_back(std::move(entry));
        }
    }

    const std::string merged = m_materialsDocument.WriteMerged(entries);
    const bool hadFile = m_materialsDocument.Exists();
    std::string error;

    // The previous generation is kept beside the target, as the game keeps one on every save,
    // so a merge that dropped a line can be undone by copying the backup back.
    if (hadFile && !CopyEditorFile(m_sessionPaths.materials, m_sessionPaths.materialsBackup, error))
    {
        PrintMessage(("session: " + error).c_str(), this);
        Notify(error);
        return false;
    }

    if (!WriteEditorFile(m_sessionPaths.materials, merged, error))
    {
        PrintMessage(("session: " + error).c_str(), this);
        Notify(error);
        return false;
    }

    m_touchedMaterials.clear();

    std::string reloadError;
    if (!m_materialsDocument.Load(m_sessionPaths.materials, reloadError))
    {
        PrintMessage(("session: " + reloadError).c_str(), this);
    }

    PrintMessage(("session: saved " + m_sessionPaths.materials).c_str(), this);
    Notify("saved " + m_sessionPaths.materials);
    return true;
}

bool EditorApp::SaveLevelLightsSession()
{
    std::string merged = m_levelDocument.WriteMerged(
        m_sessionPaths.level, LevelSky(), LevelFog(), LevelLights(), ShouldWriteEmptyLightsBlock());
    MaybeAppendEmitterLightsYaml(merged, m_emitterLightOverrides, m_baselineEmitterOverrides);

    std::string error;
    if (m_levelDocument.Exists() &&
        !CopyEditorFile(m_sessionPaths.lights, m_sessionPaths.lightsBackup, error))
    {
        PrintMessage(("session: " + error).c_str(), this);
        Notify(error);
        return false;
    }

    if (!WriteEditorFile(m_sessionPaths.lights, merged, error))
    {
        PrintMessage(("session: " + error).c_str(), this);
        Notify(error);
        return false;
    }

    // The document the next merge starts from has to be the file that was just written, so the
    // other levels' sections and the keys the editor cannot express stay byte for byte.
    std::string reloadError;
    if (!m_levelDocument.Load(m_sessionPaths.lights, reloadError))
    {
        PrintMessage(("session: " + reloadError).c_str(), this);
    }

    // The scope the write leaves behind is the one the file now shows, which is the state the
    // game's editor finds when it reopens: RT_SkySettings_CommitSession drops the level's own
    // set on a global save, so switching back to the level has nothing left to resurrect.
    AdoptSkyScope(m_levelDocument.ReadLevel(m_sessionPaths.level));

    m_levelTouched = false;
    // The session now matches the file: the text it was written from is the baseline the next
    // edit is told apart from.
    CaptureSky();
    m_levelFingerprint = LevelFingerprint();
    RefreshMapCatalogSavedFlags();

    PrintMessage(
        ("session: level " + m_sessionPaths.level + " written to " + m_sessionPaths.lights).c_str(),
        this);
    Notify("saved " + m_sessionPaths.lights);
    return true;
}

bool EditorApp::HasUnsavedSystemSettings() const
{
    return m_settings != m_baselineSettings || (m_skyGlobal && m_sun != m_baselineSun);
}

std::string EditorApp::ResolveSystemConfigPath() const
{
    if (!m_gamePath.empty())
    {
        return (std::filesystem::path(m_gamePath) / "config.cfg").string();
    }
    if (!m_sessionPaths.materials.empty())
    {
        const std::filesystem::path parent =
            std::filesystem::path(m_sessionPaths.materials).parent_path();
        if (!parent.empty())
        {
            return (parent / "config.cfg").string();
        }
    }
    return "config.cfg";
}

bool EditorApp::SaveSystemConfigSession()
{
    const std::string cfgPath = ResolveSystemConfigPath();
    std::string existing;
    bool hadFile = false;
    {
        std::ifstream inFile(cfgPath, std::ios::binary);
        if (inFile)
        {
            hadFile = true;
            existing.assign((std::istreambuf_iterator<char>(inFile)),
                            std::istreambuf_iterator<char>());
        }
    }

    const ConfigEntryList entries =
        CollectSystemConfigEntries(m_settings, m_sun, m_skyGlobal);
    const std::string merged = MergeConfigCvarText(existing, entries);
    std::string error;
    if (hadFile && !CopyEditorFile(cfgPath, cfgPath + ".bak", error))
    {
        PrintMessage(("config: " + error).c_str(), this);
        Notify(error);
        return false;
    }
    if (!WriteEditorFile(cfgPath, merged, error))
    {
        PrintMessage(("config: " + error).c_str(), this);
        Notify(error);
        return false;
    }

    PrintMessage(("config: saved " + cfgPath).c_str(), this);
    Notify("saved " + cfgPath);
    return true;
}

bool EditorApp::ApplySession()
{
    // Rendering the section rather than trusting the latch also settles an edit that was taken
    // back: nothing differs from the file, so nothing is written for it.
    const bool levelChanged = LevelFingerprint() != m_levelFingerprint;
    const bool systemChanged = HasUnsavedSystemSettings();

    if (m_touchedMaterials.empty() && !levelChanged && !systemChanged)
    {
        PrintMessage("session: apply with nothing touched", this);
        Notify("nothing to save: no material or level was edited");
        return false;
    }

    // The game's apply writes the whole merged session, so a target that does not exist yet is
    // created by the first save even when only the level was edited.
    const bool writeMaterials = !m_touchedMaterials.empty() || !m_materialsDocument.Exists();

    if (writeMaterials && !SaveMaterialsSession())
    {
        return false;
    }

    if (levelChanged && !SaveLevelLightsSession())
    {
        return false;
    }

    if (systemChanged && !SaveSystemConfigSession())
    {
        return false;
    }

    m_sessionDirty = false;
    return true;
}

void EditorApp::DiscardSession()
{
    m_customLights.clear();
    m_selectedCustomLight = -1;
    m_selectedCustomLights.clear();
    m_selectedLight = -1;
    m_selectedLights.clear();
    m_hoverLight = -1;
    m_hoverCustomLight = -1;
    if (m_sceneLoaded)
    {
        m_scene.lights = m_scene.bspLights;
    }
    m_settings = m_baselineSettings;
    m_sun = m_baselineSun;
    if (!m_skyGlobals.Empty())
    {
        ApplyEditorSkyScope(m_skyGlobals, false, m_settings, m_sun);
    }
    LoadMaterialsSession();
    LoadLightsSession();
    if (m_sceneLoaded)
    {
        RestoreSceneLightsAfterSessionReload();
    }
    ApplyBaselineCapture();
    m_materialsChanged = true;
    PrintMessage("session: changes discarded", this);
    Notify("changes discarded");
}

namespace
{

// Removes one file the session wrote. A file that is already gone is the state a reset asks for,
// so only a removal that fails while the file is still there is an error.
bool RemoveSessionFile(const std::string &path, std::string &failed)
{
    if (path.empty() || std::remove(path.c_str()) == 0)
    {
        return true;
    }

    std::ifstream probe(path, std::ios::binary);

    if (!probe.good())
    {
        return true;
    }

    failed = path;
    return false;
}

}

void EditorApp::ResetSession()
{
    // QRE_ResetAll with the standalone's file set: a session writes the materials file and the
    // lights file and keeps one backup of each, and there is no editor file in flight. A removal
    // that fails aborts the whole reset before anything is reloaded, which is the order the game
    // uses so a half-removed session never looks reset.
    std::string failed;

    for (const std::string &path : { m_sessionPaths.materials, m_sessionPaths.lights,
                                     m_sessionPaths.materialsBackup, m_sessionPaths.lightsBackup })
    {
        if (!RemoveSessionFile(path, failed))
        {
            const std::string message = "session: cannot remove " + failed + "; reset failed";

            PrintMessage(message.c_str(), this);
            Notify("cannot remove " + failed);
            return;
        }
    }

    // The panel goes back to the values a first run would have given it. The materials load
    // themselves from the absent target, and the level-owned state is cleared here because an
    // absent level section deliberately leaves the panel's own values alone: without this the fog,
    // the sun and every `sky:` key would keep what the removed file had put there. A default
    // constructed state is exactly the defaults, since nothing but a session load or the panel
    // itself ever writes these.
    m_settings = EditorRenderSettings{};
    m_sun = EditorSunState{};
    m_runtime.ApplySceneRenderDefaults();
    m_skyGlobals = EditorLevelSky{};
    m_runtime.celestialClockSeconds = 0.0;
    m_emitterLightOverrides.clear();
    m_customLights.clear();
    m_selectedCustomLight = -1;
    m_selectedLight = -1;
    m_selectedLights.clear();
    m_selectedCustomLights.clear();
    m_hoverLight = -1;
    m_hoverCustomLight = -1;
    m_scene.lights = m_scene.bspLights;

    LoadMaterialsSession();
    LoadLightsSession();
    if (m_sceneLoaded)
    {
        RestoreSceneLightsAfterSessionReload();
    }
    ApplyBaselineCapture();
    m_materialsChanged = true;

    const std::string message = "session: reset, removed " + m_sessionPaths.materials + " and " +
                                m_sessionPaths.lights;

    PrintMessage(message.c_str(), this);
    Notify("session reset, defaults restored");
}

namespace
{
const char *PendingActionDescription(EditorApp::PendingSessionAction action)
{
    switch (action)
    {
    case EditorApp::PendingSessionAction::CloseScene:
        return "closing the level";
    case EditorApp::PendingSessionAction::ReloadScene:
        return "reloading the level";
    case EditorApp::PendingSessionAction::OpenScene:
        return "opening another level";
    case EditorApp::PendingSessionAction::OpenMod:
        return "switching the mod";
    case EditorApp::PendingSessionAction::UnloadMod:
        return "unloading the mod";
    case EditorApp::PendingSessionAction::PlayLevel:
        return "launching the level in game";
    default:
        return "exiting";
    }
}
} // namespace

void EditorApp::RequestSessionAction(PendingSessionAction action, const std::string &path,
                                     bool flag)
{
    if (action == PendingSessionAction::None)
    {
        return;
    }

    m_pendingAction = action;
    m_pendingActionPath = path;
    m_pendingActionFlag = flag;

    if (m_sceneLoaded && m_touchedMaterials.empty() && LevelFingerprint() == m_levelFingerprint &&
        !HasUnsavedSystemSettings())
    {
        m_levelTouched = false;
        m_sessionDirty = false;
    }

    if (!HasUnsavedEditorChanges())
    {
        ExecutePendingSessionAction();
        return;
    }

    SetCursorMode(true);
    m_sessionPrompt = 1;
    m_loggedPromptLayout = false;
    PrintMessage("session: action requested with unsaved changes", this);
}

void EditorApp::ExecutePendingSessionAction()
{
    const PendingSessionAction action = m_pendingAction;
    const std::string path = m_pendingActionPath;
    const bool flag = m_pendingActionFlag;
    m_pendingAction = PendingSessionAction::None;
    m_pendingActionPath.clear();
    m_pendingActionFlag = false;

    if (action == PendingSessionAction::Exit)
    {
        m_running = false;
        return;
    }

    m_deferredAction = action;
    m_deferredActionPath = path;
    m_deferredActionFlag = flag;
}

void EditorApp::ProcessDeferredSessionAction()
{
    if (m_deferredAction == PendingSessionAction::None)
    {
        return;
    }

    const PendingSessionAction action = m_deferredAction;
    const std::string path = m_deferredActionPath;
    const bool flag = m_deferredActionFlag;
    m_deferredAction = PendingSessionAction::None;
    m_deferredActionPath.clear();
    m_deferredActionFlag = false;

    switch (action)
    {
    case PendingSessionAction::CloseScene:
        CloseSceneNow();
        break;
    case PendingSessionAction::ReloadScene:
        ReloadSceneNow();
        break;
    case PendingSessionAction::OpenScene:
        OpenSceneNow(path);
        break;
    case PendingSessionAction::OpenMod:
        OpenModNow(path, flag);
        break;
    case PendingSessionAction::UnloadMod:
        UnloadModNow();
        break;
    case PendingSessionAction::PlayLevel:
        PlayLevelNow();
        break;
    default:
        break;
    }
}

namespace
{

std::string ToAbsoluteNormalizedPath(const std::string &path)
{
    if (path.empty())
    {
        return {};
    }
    std::error_code ec;
    std::filesystem::path p(path);
    if (p.is_relative())
    {
        p = std::filesystem::absolute(p, ec);
    }
    return p.lexically_normal().string();
}

bool IsPlayableExecutableFile(const std::filesystem::path &path)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
    {
        return false;
    }
#if !defined(_WIN32)
    return access(path.c_str(), X_OK) == 0;
#else
    return true;
#endif
}

std::string FindDefaultVkQuakeExecutable(const EditorPlayLevelRequest &request)
{
    if (!request.gameExecutable.empty())
    {
        return IsPlayableExecutableFile(request.gameExecutable) ? request.gameExecutable
                                                              : std::string();
    }
    if (const char *envExe = std::getenv("QRAY_GAME_EXE");
        envExe != nullptr && envExe[0] != '\0' && IsPlayableExecutableFile(envExe))
    {
        return envExe;
    }

    std::vector<std::filesystem::path> dirs;
    if (!request.executableDir.empty())
    {
        const std::filesystem::path exeDir(request.executableDir);
        dirs = { exeDir, exeDir.parent_path(), exeDir.parent_path().parent_path() };
    }
#if defined(_WIN32)
    static constexpr const char *kNames[] = { "vkquake.exe", "quakespasm-rt.exe" };
#else
    static constexpr const char *kNames[] = { "vkquake", "quakespasm-rt" };
#endif
    for (const std::filesystem::path &dir : dirs)
    {
        if (dir.empty())
        {
            continue;
        }
        for (const char *name : kNames)
        {
            const std::filesystem::path cand = dir / name;
            if (IsPlayableExecutableFile(cand))
            {
                return cand.string();
            }
        }
    }
    return {};
}

bool BuildDefaultPlayLevelCommand(const EditorPlayLevelRequest &request,
                                  EditorLaunchCommand &outCommand, std::string &outError)
{
    const std::string exe = FindDefaultVkQuakeExecutable(request);
    if (exe.empty())
    {
        outError = "game executable not found (pass --game-exe or set QRAY_GAME_EXE)";
        return false;
    }

    outCommand.executable = exe;
    outCommand.workingDirectory = std::filesystem::path(exe).parent_path().string();
    if (!request.gameDir.empty())
    {
        std::filesystem::path gdir(request.gameDir);
        if (gdir.filename().empty())
        {
            gdir = gdir.parent_path();
        }
        const std::string leaf = gdir.filename().string();
        const std::string parent = gdir.parent_path().string();
        if (!parent.empty())
        {
            outCommand.arguments.emplace_back("-basedir");
            outCommand.arguments.push_back(parent);
        }
        if (!leaf.empty() && LowercaseName(leaf) != "id1")
        {
            outCommand.arguments.emplace_back("-game");
            outCommand.arguments.push_back(leaf);
        }
    }
    outCommand.arguments.emplace_back("+map");
    outCommand.arguments.push_back(request.levelName);
    return true;
}

std::string FormatLaunchLogLine(const EditorLaunchCommand &cmd)
{
    std::string line = "play level: launching " + cmd.executable;
    for (const std::string &arg : cmd.arguments)
    {
        line += ' ';
        line += arg;
    }
    return line;
}

} // namespace

bool EditorApp::CanPlayCurrentLevel() const
{
    return m_sceneLoaded && !m_sessionPaths.level.empty();
}

void EditorApp::RequestPlayLevel()
{
    if (!CanPlayCurrentLevel() || m_playProcess.handle != 0)
    {
        return;
    }
    m_deferredAction = PendingSessionAction::PlayLevel;
    m_deferredActionPath.clear();
    m_deferredActionFlag = false;
}

bool EditorApp::PlayLevelNow()
{
    if (!CanPlayCurrentLevel() || m_playProcess.handle != 0)
    {
        return false;
    }

    if (m_touchedMaterials.empty() && LevelFingerprint() == m_levelFingerprint &&
        !HasUnsavedSystemSettings())
    {
        m_levelTouched = false;
        m_sessionDirty = false;
    }

    if (HasUnsavedEditorChanges() && !SaveSessionToDisk())
    {
        return false;
    }

    EditorPlayLevelRequest req;
    req.executableDir = EditorExecutableDirectory();
    req.gameExecutable = ToAbsoluteNormalizedPath(m_gameExecutableOverride);
    req.gameDir = ToAbsoluteNormalizedPath(m_gamePath);
    req.scenePath = ToAbsoluteNormalizedPath(m_scenePath);
    req.levelName = m_sessionPaths.level;
    req.shaderFolder = ToAbsoluteNormalizedPath(m_shaderFolder);
    req.blueNoisePath = ToAbsoluteNormalizedPath(m_blueNoisePath);
    req.waterNormalPath = ToAbsoluteNormalizedPath(m_waterNormalPath);
    req.materialsPath = ToAbsoluteNormalizedPath(m_sessionPaths.materials);
    req.lightsPath = ToAbsoluteNormalizedPath(m_sessionPaths.lights);
    req.configPath = ToAbsoluteNormalizedPath(ResolveSystemConfigPath());
    req.multiplayer = m_multiplayerMode;

    const EditorFormatAdapter *adapter = GetEditorFormatAdapter();
    EditorLaunchCommand cmd;
    std::string error;
    const bool built =
        (adapter != nullptr && adapter->buildPlayLevelCommand != nullptr)
            ? adapter->buildPlayLevelCommand(req, cmd, error)
            : BuildDefaultPlayLevelCommand(req, cmd, error);
    if (!built)
    {
        PrintMessage(("play level: " + error).c_str(), this);
        Notify(error);
        return false;
    }

    m_window.SetMouseCapture(false);
    SetCursorMode(true);

    const std::string logLine = FormatLaunchLogLine(cmd);
    EditorProcessLaunch procLaunch;
    procLaunch.executable = cmd.executable;
    procLaunch.workingDirectory = cmd.workingDirectory;
    procLaunch.arguments = std::move(cmd.arguments);
    procLaunch.envOverrides = std::move(cmd.envOverrides);
    if (!EditorSpawnProcess(procLaunch, m_playProcess, error))
    {
        PrintMessage(("play level: " + error).c_str(), this);
        Notify(error);
        return false;
    }

    PrintMessage(logLine.c_str(), this);
    Notify("launching " + m_sessionPaths.level + " in game");
    return true;
}

bool EditorApp::PollPlayProcess()
{
    if (m_playProcess.handle == 0)
    {
        return false;
    }

    int exitCode = 0;
    if (EditorIsProcessRunning(m_playProcess, &exitCode))
    {
        return true;
    }

    PrintMessage(("play level: game exited (code " + std::to_string(exitCode) + ")").c_str(),
                 this);
    Notify(exitCode == 0 ? "returned from game"
                         : ("game exited with code " + std::to_string(exitCode)));
    std::fill(std::begin(m_keys), std::end(m_keys), false);
    m_hasFocus = m_window.HasFocus();
    EditorEvent focusEvent = {};
    focusEvent.type = EditorEventType::Focus;
    focusEvent.down = m_hasFocus;
    m_ui.HandleEvent(focusEvent);
    return false;
}

void EditorApp::RequestExit()
{
    RequestSessionAction(PendingSessionAction::Exit);
}

void EditorApp::DrawSessionBar()
{
    // The latch runs the comparison only until the level is first edited: after that the flag is
    // the answer, and a save or a discard is what clears it.
    if (!m_levelTouched && LevelFingerprint() != m_levelFingerprint)
    {
        m_levelTouched = true;
        m_sessionDirty = true;
        PrintMessage(("session: level " + m_sessionPaths.level + " touched").c_str(), this);
    }

    std::string state = m_sessionDirty ? "unsaved" : "clean";

    if (!m_touchedMaterials.empty())
    {
        state += ", " + std::to_string(m_touchedMaterials.size()) + " touched";
    }

    if (m_levelTouched)
    {
        state += ", level " + m_sessionPaths.level;
    }

    // Only the file name fits the bar; the full path is in the log line the session prints and in
    // the save toast.
    const size_t slash = m_sessionPaths.materials.find_last_of("\\/");
    const char *target = slash == std::string::npos ? m_sessionPaths.materials.c_str()
                                                    : m_sessionPaths.materials.c_str() + slash + 1;

    ImGui::Text("Session %s  Target %s", state.c_str(), target);

    const float width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;

    // Button rects are logged like the tab rects, so a scripted session can click Apply without
    // guessing where the bar landed.
    std::string buttonRects;

    auto noteRect = [&buttonRects](const char *name) {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        buttonRects += " " + std::string(name) + " " + std::to_string(int(min.x)) + "," +
                       std::to_string(int(min.y)) + " " + std::to_string(int(max.x)) + "," +
                       std::to_string(int(max.y));
    };

    ImGui::BeginDisabled(!m_sessionDirty);

    if (ImGui::Button("Apply", ImVec2(width, 0.0f)))
    {
        SaveSessionToDisk();
    }

    noteRect("Apply");

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Write the touched materials into the session file\n"
                          "and this level's fog and lights into its level file.\n"
                          "Every other line of both files is kept as it was.");
    }

    ImGui::SameLine();

    if (ImGui::Button("Cancel", ImVec2(width, 0.0f)))
    {
        CancelSessionChanges();
    }

    noteRect("Cancel");

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Reload the materials, the fog and the lights from the session files.");
    }

    ImGui::EndDisabled();

    ImGui::SameLine();

    if (ImGui::Button("Exit", ImVec2(width, 0.0f)))
    {
        RequestExit();
    }

    noteRect("Exit");

    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Close the editor, asking about unsaved changes first.");
    }

    const uint64_t buttonLayout = std::hash<std::string>{}(buttonRects) | 1u;

    if (m_loggedSessionLayout != buttonLayout)
    {
        m_loggedSessionLayout = buttonLayout;
        PrintMessage(("panel session:" + buttonRects).c_str(), this);
    }
}

namespace
{
int32_t DrawSessionPromptButtons(std::string &buttonRects)
{
    const auto noteRect = [&buttonRects](const char *label) {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        buttonRects += std::string(" ") + label + " " + std::to_string(int(min.x)) + "," +
                       std::to_string(int(min.y)) + " " + std::to_string(int(max.x)) + "," +
                       std::to_string(int(max.y));
    };

    int32_t answer = 0;
    const float scale = EditorGuiScale();
    const float btnH = std::max(30.0f * scale, ImGui::GetFrameHeight() + 4.0f * scale);
    const float buttonWidth =
        (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

    if (ImGui::Button("Yes", ImVec2(buttonWidth, btnH)))
    {
        answer = 1;
    }

    noteRect("Yes");
    ImGui::SameLine();

    if (ImGui::Button("No", ImVec2(buttonWidth, btnH)))
    {
        answer = 2;
    }

    noteRect("No");
    return answer;
}
} // namespace

void EditorApp::DrawSessionPrompt()
{
    if (m_sessionPrompt == 0)
    {
        return;
    }

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float scale = EditorGuiScale();
    EditorGuiBackdrop(viewport->Size.x, viewport->Size.y, 0.58f);
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                   viewport->Pos.y + viewport->Size.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    // A fixed width with a fitted height: the wrapped text and the path decide how tall the dialog
    // is, and a height that is too small clips the buttons out of the window, where they cannot be
    // clicked at all.
    ImGui::SetNextWindowSize(
        ImVec2(std::min(viewport->Size.x - 32.0f * scale, 440.0f * scale), 0.0f),
        ImGuiCond_Always);

    bool open = true;
    int32_t answer = 0;
    std::string buttonRects;

    if (ImGui::Begin("Save your changes?", &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoResize))
    {
        ImGui::TextWrapped("Save unsaved session changes before %s?",
                           PendingActionDescription(m_pendingAction));
        if (!m_touchedMaterials.empty())
        {
            ImGui::TextWrapped("%s", m_sessionPaths.materials.c_str());
        }
        if (m_levelTouched || LevelFingerprint() != m_levelFingerprint)
        {
            ImGui::TextWrapped("%s", m_sessionPaths.lights.c_str());
        }
        if (HasUnsavedSystemSettings())
        {
            ImGui::TextWrapped("%s", ResolveSystemConfigPath().c_str());
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        answer = DrawSessionPromptButtons(buttonRects);
    }

    ImGui::End();

    if (!m_loggedPromptLayout)
    {
        m_loggedPromptLayout = true;
        PrintMessage(("panel prompt:" + buttonRects).c_str(), this);
    }

    if (answer == 0 && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || !open))
    {
        answer = 3;
    }

    if (answer == 0)
    {
        return;
    }

    m_sessionPrompt = 0;

    if (answer == 1)
    {
        if (ApplySession())
        {
            ApplyBaselineCapture();
            ExecutePendingSessionAction();
        }
    }
    else if (answer == 2)
    {
        DiscardSession();
        ApplyBaselineCapture();
        ExecutePendingSessionAction();
    }
    else
    {
        m_pendingAction = PendingSessionAction::None;
        m_pendingActionPath.clear();
        m_pendingActionFlag = false;
        PrintMessage("session: prompt dismissed", this);
    }
}

void EditorApp::DrawResetPrompt()
{
    if (m_resetPrompt == 0)
    {
        return;
    }

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float scale = EditorGuiScale();
    EditorGuiBackdrop(viewport->Size.x, viewport->Size.y, 0.58f);
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                   viewport->Pos.y + viewport->Size.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    // Fixed width, fitted height: this one names the files so a session cannot remove a file the
    // author did not expect, and the wrapped paths make the content taller than any height guessed
    // here would be.
    ImGui::SetNextWindowSize(
        ImVec2(std::min(viewport->Size.x - 32.0f * scale, 560.0f * scale), 0.0f),
        ImGuiCond_Always);

    bool open = true;
    int32_t answer = 0;
    std::string layout;

    const auto noteRect = [&layout](const char *label) {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        layout += std::string(" ") + label + " " + std::to_string(int(min.x)) + "," +
                  std::to_string(int(min.y)) + " " + std::to_string(int(max.x)) + "," +
                  std::to_string(int(max.y));
    };

    if (ImGui::Begin("Reset the session?", &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoResize))
    {
        ImGui::TextWrapped("This removes the saved work of this session and sets the materials, "
                           "the lights and the fog back to their defaults. The game loads the same "
                           "files, so its edits go with them.");
        ImGui::Spacing();
        ImGui::TextWrapped("%s", m_sessionPaths.materials.c_str());
        ImGui::TextWrapped("%s", m_sessionPaths.lights.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float btnH = std::max(30.0f * scale, ImGui::GetFrameHeight() + 4.0f * scale);
        const float buttonWidth =
            (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

        if (ImGui::Button("Yes", ImVec2(buttonWidth, btnH)))
        {
            answer = 1;
        }

        noteRect("Yes");

        ImGui::SameLine();

        if (ImGui::Button("No", ImVec2(buttonWidth, btnH)))
        {
            answer = 2;
        }

        noteRect("No");

        // The window rect is part of the line so a script can tell a button that sits inside the
        // dialog from one the dialog is too small to show.
        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        layout = " window " + std::to_string(int(pos.x)) + "," + std::to_string(int(pos.y)) + " " +
                 std::to_string(int(pos.x + size.x)) + "," + std::to_string(int(pos.y + size.y)) +
                 layout;
    }

    ImGui::End();

    const uint64_t layoutKey = std::hash<std::string>{}(layout) | 1u;

    if (layoutKey != m_loggedResetLayout)
    {
        m_loggedResetLayout = layoutKey;
        PrintMessage(("panel reset prompt:" + layout).c_str(), this);
    }

    // Escape and "No" both return to editing without touching a file, the way the game's Escape
    // clears its reset flag.
    if (answer == 0 && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        answer = 2;
    }

    if (answer == 0 && !open)
    {
        answer = 2;
    }

    if (answer == 1)
    {
        m_resetPrompt = 0;
        ResetSession();
    }
    else if (answer == 2)
    {
        m_resetPrompt = 0;
        PrintMessage("session: reset dismissed", this);
    }
}

void EditorApp::DrawRegenPrompt()
{
    if (m_regenPrompt == 0)
    {
        return;
    }

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float scale = EditorGuiScale();
    EditorGuiBackdrop(viewport->Size.x, viewport->Size.y, 0.58f);
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                   viewport->Pos.y + viewport->Size.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(
        ImVec2(std::min(viewport->Size.x - 32.0f * scale, 560.0f * scale), 0.0f),
        ImGuiCond_Always);

    bool open = true;
    int32_t answer = 0;
    std::string layout;

    const auto noteRect = [&layout](const char *label) {
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        layout += std::string(" ") + label + " " + std::to_string(int(min.x)) + "," +
                  std::to_string(int(min.y)) + " " + std::to_string(int(max.x)) + "," +
                  std::to_string(int(max.y));
    };

    if (ImGui::Begin("Regenerate from map?", &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoResize))
    {
        ImGui::TextWrapped("This restores all predefined lights from the map into the standard "
                           "Entity lights list, resets emitter light overrides, and saves the "
                           "level's lights to qray.lights.yaml (created if missing).");
        ImGui::Spacing();
        ImGui::TextWrapped("%s", m_sessionPaths.lights.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const float btnH = std::max(30.0f * scale, ImGui::GetFrameHeight() + 4.0f * scale);
        const float buttonWidth =
            (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

        if (ImGui::Button("Yes", ImVec2(buttonWidth, btnH)))
        {
            answer = 1;
        }

        noteRect("Yes");

        ImGui::SameLine();

        if (ImGui::Button("No", ImVec2(buttonWidth, btnH)))
        {
            answer = 2;
        }

        noteRect("No");

        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        layout = " window " + std::to_string(int(pos.x)) + "," + std::to_string(int(pos.y)) + " " +
                 std::to_string(int(pos.x + size.x)) + "," + std::to_string(int(pos.y + size.y)) +
                 layout;
    }

    ImGui::End();

    const uint64_t layoutKey = std::hash<std::string>{}(layout) | 1u;

    if (layoutKey != m_loggedRegenLayout)
    {
        m_loggedRegenLayout = layoutKey;
        PrintMessage(("panel regen prompt:" + layout).c_str(), this);
    }

    if (answer == 0 && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        answer = 2;
    }

    if (answer == 0 && !open)
    {
        answer = 2;
    }

    if (answer == 1)
    {
        m_regenPrompt = 0;
        RegenerateLightModels();
    }
    else if (answer == 2)
    {
        m_regenPrompt = 0;
        PrintMessage("session: regenerate dismissed", this);
    }
}

void EditorApp::RegenerateLightModels()
{
    if (!m_sceneLoaded)
    {
        return;
    }

    m_scene.lights = m_scene.bspLights;
    m_emitterLightOverrides.clear();
    ReapplyGeneratedLights();
    ClearSelection();
    FinishGizmoDrag(false);
    FinishLightGrab(false);
    m_customPlacing = false;
    m_customCloning = -1;
    m_hoverLight = -1;
    m_hoverCustomLight = -1;

    const bool hasPersistedLights =
        m_levelDocument.Exists() && m_levelDocument.ReadLevel(m_sessionPaths.level).hasLights;
    if (LevelFingerprint() != m_levelFingerprint || !hasPersistedLights)
    {
        if (!SaveLevelLightsSession())
        {
            return;
        }
        if (m_touchedMaterials.empty())
        {
            m_sessionDirty = false;
        }
    }

    m_baselineCustomLights = m_customLights;
    m_baselineMapLights = m_scene.lights;
    m_baselineEmitterOverrides = m_emitterLightOverrides;
    ClearDirtyFlagIfClean();

    const std::string countText = std::to_string(m_scene.lights.size());
    PrintMessage(("session: regenerated " + countText + " map lights").c_str(), this);
    Notify("regenerated " + countText + " map lights");
}

void EditorApp::DrawNotification()
{
    if (!m_notifyActive)
    {
        return;
    }

    const float elapsed =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - m_notifyTime).count();

    if (elapsed >= kNotifySeconds)
    {
        m_notifyActive = false;
        return;
    }

    const float fade = kNotifySeconds - elapsed;
    const float alpha = fade < kNotifyFadeSeconds ? fade / kNotifyFadeSeconds : 1.0f;

    // The game draws its toast over everything at 14,14; the editor's panel owns that corner, so
    // the line sits in the viewport's bottom-left instead, still over the whole window.
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float scale = EditorGuiScale();
    const ImVec2 position(viewport->Pos.x + 14.0f * scale,
                          viewport->Pos.y + viewport->Size.y - 58.0f * scale);
    ImDrawList *draw = ImGui::GetForegroundDrawList();

    draw->AddText(ImVec2(position.x + 1.0f, position.y + 1.0f),
                  IM_COL32(0, 0, 0, int32_t(170.0f * alpha)), m_notifyText.c_str());
    draw->AddText(position, IM_COL32(255, 226, 138, int32_t(235.0f * alpha)), m_notifyText.c_str());
}

void EditorApp::Notify(const std::string &text)
{
    m_notifyText = text;
    m_notifyTime = std::chrono::steady_clock::now();
    m_notifyActive = true;
}

namespace
{

void AppendBe32(std::vector<uint8_t> &out, uint32_t value)
{
    out.push_back(uint8_t((value >> 24u) & 0xFFu));
    out.push_back(uint8_t((value >> 16u) & 0xFFu));
    out.push_back(uint8_t((value >> 8u) & 0xFFu));
    out.push_back(uint8_t(value & 0xFFu));
}

uint32_t UpdateCrc32(uint32_t crc, const uint8_t *data, size_t size)
{
    uint32_t state = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++)
    {
        state ^= data[i];
        for (int bit = 0; bit < 8; bit++)
        {
            state = (state & 1u) != 0u ? ((state >> 1u) ^ 0xEDB88320u) : (state >> 1u);
        }
    }
    return state ^ 0xFFFFFFFFu;
}

uint32_t ComputeAdler32(const uint8_t *data, size_t size)
{
    constexpr uint32_t kModAdler = 65521u;
    uint32_t s1 = 1u;
    uint32_t s2 = 0u;
    for (size_t i = 0; i < size; i++)
    {
        s1 = (s1 + data[i]) % kModAdler;
        s2 = (s2 + s1) % kModAdler;
    }
    return (s2 << 16u) | s1;
}

void WritePngChunk(std::vector<uint8_t> &out, const char type[4], const uint8_t *data, size_t size)
{
    AppendBe32(out, uint32_t(size));
    const size_t typeOffset = out.size();
    out.insert(out.end(), type, type + 4);
    if (size > 0 && data != nullptr)
    {
        out.insert(out.end(), data, data + size);
    }
    const uint32_t crc = UpdateCrc32(0u, out.data() + typeOffset, 4u + size);
    AppendBe32(out, crc);
}

std::vector<uint8_t> BuildStoredZlibStream(const uint8_t *raw, size_t rawSize)
{
    std::vector<uint8_t> z;
    z.reserve(rawSize + 64u);
    z.push_back(0x78u);
    z.push_back(0x01u);

    size_t offset = 0;
    while (offset < rawSize || rawSize == 0)
    {
        const size_t blockLen = std::min<size_t>(65535u, rawSize - offset);
        const bool isFinal = (offset + blockLen) >= rawSize;
        const uint16_t len16 = uint16_t(blockLen);
        const uint16_t nlen16 = uint16_t(~len16);
        z.push_back(isFinal ? 0x01u : 0x00u);
        z.push_back(uint8_t(len16 & 0xFFu));
        z.push_back(uint8_t(len16 >> 8u));
        z.push_back(uint8_t(nlen16 & 0xFFu));
        z.push_back(uint8_t(nlen16 >> 8u));
        if (blockLen > 0)
        {
            z.insert(z.end(), raw + offset, raw + offset + blockLen);
        }
        offset += blockLen;
        if (isFinal)
        {
            break;
        }
    }

    AppendBe32(z, ComputeAdler32(raw, rawSize));
    return z;
}

bool WriteRgbaPng(const std::filesystem::path &path, uint32_t width, uint32_t height,
                  uint32_t rowPitch, const uint8_t *rgba)
{
    if (width == 0 || height == 0 || rgba == nullptr || rowPitch < width * 4u)
    {
        return false;
    }

    std::error_code ec;
    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::vector<uint8_t> raw(size_t(height) * (1u + size_t(width) * 4u), 0u);
    const size_t stride = 1u + size_t(width) * 4u;
    for (uint32_t y = 0; y < height; y++)
    {
        uint8_t *dst = raw.data() + size_t(y) * stride;
        dst[0] = 0u;
        std::copy_n(rgba + size_t(y) * rowPitch, size_t(width) * 4u, dst + 1u);
    }

    std::vector<uint8_t> png = { 137u, 80u, 78u, 71u, 13u, 10u, 26u, 10u };
    std::vector<uint8_t> ihdr;
    AppendBe32(ihdr, width);
    AppendBe32(ihdr, height);
    ihdr.push_back(8u);
    ihdr.push_back(6u);
    ihdr.push_back(0u);
    ihdr.push_back(0u);
    ihdr.push_back(0u);
    WritePngChunk(png, "IHDR", ihdr.data(), ihdr.size());

    const std::vector<uint8_t> idat = BuildStoredZlibStream(raw.data(), raw.size());
    WritePngChunk(png, "IDAT", idat.data(), idat.size());
    WritePngChunk(png, "IEND", nullptr, 0u);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        return false;
    }
    out.write(reinterpret_cast<const char *>(png.data()), std::streamsize(png.size()));
    return out.good();
}

struct EditorLaunchConfig
{
    std::string scenePath = "editor_scene.obj";
    std::string shaderFolder = "shaders";
    std::string blueNoisePath = "textures/BlueNoise_LDR_RGBA_128.png";
    std::string gamePath;
    std::string gameExecutable;
    std::string initialTab;
    std::string materialsOverride;
    std::string lightsOverride;
    std::string screenshotPath;
    uint32_t maxFrames = 0;
    bool hasScene = false;
    bool hasShaders = false;
    bool hasBlueNoise = false;
    bool noPanel = false;
};

struct StringFlagBinding
{
    const char *flag;
    std::string *target;
    bool *seenFlag;
};

void ParseEditorCliArgs(const std::vector<std::string> &args, EditorLaunchConfig &config)
{
    const StringFlagBinding bindings[] = {
        { "--scene", &config.scenePath, &config.hasScene },
        { "--shaders", &config.shaderFolder, &config.hasShaders },
        { "--blue-noise", &config.blueNoisePath, &config.hasBlueNoise },
        { "--gamedir", &config.gamePath, nullptr },
        { "--game-exe", &config.gameExecutable, nullptr },
        { "--tab", &config.initialTab, nullptr },
        { "--materials", &config.materialsOverride, nullptr },
        { "--lights", &config.lightsOverride, nullptr },
        { "--screenshot", &config.screenshotPath, nullptr },
    };

    for (size_t i = 1; i < args.size(); i++)
    {
        const std::string &argument = args[i];

        if (argument == "--no-panel")
        {
            config.noPanel = true;
            continue;
        }

        if (i + 1 >= args.size())
        {
            break;
        }

        if (argument == "--max-frames")
        {
            char *end = nullptr;
            const unsigned long parsed = std::strtoul(args[++i].c_str(), &end, 10);

            if (end != nullptr && *end == '\0')
            {
                config.maxFrames = uint32_t(parsed);
            }

            continue;
        }

        for (const StringFlagBinding &binding : bindings)
        {
            if (argument == binding.flag)
            {
                *binding.target = args[++i];

                if (binding.seenFlag != nullptr)
                {
                    *binding.seenFlag = true;
                }

                break;
            }
        }
    }
}

} // namespace

void EditorApp::TryRequestScreenshot()
{
    if (m_screenshotPath.empty() || m_screenshotRequested || m_instance == nullptr)
    {
        return;
    }

    const uint32_t targetFrame = (m_maxFrames > 0u) ? m_maxFrames : 1u;
    if (m_renderedFrames + 1u < targetFrame)
    {
        return;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rowPitch = 0;
    uint64_t bufferSize = 0;
    if (qrGetFrameReadbackRequirements(m_instance, QR_FRAME_READBACK_FORMAT_RGBA8_UNORM, QR_TRUE,
                                       &width, &height, &rowPitch, &bufferSize) != QR_SUCCESS ||
        bufferSize == 0u)
    {
        return;
    }

    m_screenshotWidth = width;
    m_screenshotHeight = height;
    m_screenshotRowPitch = rowPitch;
    m_screenshotPixels.assign(size_t(bufferSize), 0u);

    QrFrameReadbackInfo info = {};
    info.width = width;
    info.height = height;
    info.rowPitch = rowPitch;
    info.bufferSize = bufferSize;
    info.format = QR_FRAME_READBACK_FORMAT_RGBA8_UNORM;
    info.includeUi = QR_TRUE;
    info.pDst = m_screenshotPixels.data();
    info.frameId = QR_FRAME_ID_LATEST;

    if (qrRequestFrameReadback(m_instance, &info, &m_screenshotRequestId) == QR_SUCCESS)
    {
        m_screenshotRequested = true;
    }
}

void EditorApp::TryCompleteScreenshot()
{
    if (!m_screenshotRequested || m_screenshotWritten || m_instance == nullptr)
    {
        return;
    }

    QrBool32 done = QR_FALSE;
    if (qrGetFrameReadbackStatus(m_instance, m_screenshotRequestId, &done) != QR_SUCCESS ||
        done != QR_TRUE)
    {
        return;
    }

    if (WriteRgbaPng(m_screenshotPath, m_screenshotWidth, m_screenshotHeight,
                     m_screenshotRowPitch, m_screenshotPixels.data()))
    {
        PrintMessage(("screenshot: saved " + m_screenshotPath).c_str(), this);
    }
    else
    {
        PrintMessage(("screenshot: failed writing " + m_screenshotPath).c_str(), this);
    }
    m_screenshotWritten = true;
}

void EditorApp::AdvanceRenderedFrameCount()
{
    m_renderedFrames++;
    TryCompleteScreenshot();

    if (m_maxFrames == 0u || m_renderedFrames < m_maxFrames)
    {
        return;
    }

    const bool waitingScreenshot =
        !m_screenshotPath.empty() && !m_screenshotWritten && m_renderedFrames < m_maxFrames + 6u;
    if (!waitingScreenshot)
    {
        m_running = false;
    }
}

int EditorApp::Run(const std::vector<std::string> &args, int showCommand)
{
    m_startTime = std::chrono::steady_clock::now();

    EditorLaunchConfig config;
    ParseEditorCliArgs(args, config);

    m_scenePath = config.hasScene ? config.scenePath : ResolveDefaultAssetPath(config.scenePath);
    m_shaderFolder =
        config.hasShaders ? config.shaderFolder : ResolveDefaultAssetPath(config.shaderFolder);
    m_blueNoisePath =
        config.hasBlueNoise ? config.blueNoisePath : ResolveDefaultAssetPath(config.blueNoisePath);
    m_waterNormalPath = ResolveDefaultAssetPath("textures/WaterNormal_n.png");
    m_gamePath = config.gamePath;
    m_initialGamePath = config.gamePath;
    m_gameExecutableOverride = config.gameExecutable;
    m_initialTab = config.initialTab;
    m_materialsOverride = config.materialsOverride;
    m_lightsOverride = config.lightsOverride;
    m_screenshotPath = config.screenshotPath;
    m_maxFrames = config.maxFrames;
    m_noPanel = config.noPanel;

    NormalizeFolder(m_shaderFolder);
    m_sessionPaths =
        BuildEffectiveSessionPaths(m_materialsOverride, m_lightsOverride, m_gamePath, m_scenePath);

    if (!CreateWindowInstance(showCommand))
    {
        return 1;
    }

    if (!CreateRenderer())
    {
        m_window.Destroy();
        return 1;
    }

    CreateMaterial();

    if (!LoadScene())
    {
        Shutdown();
        m_window.Destroy();
        return 1;
    }

    CreateUi();
    m_window.GetClientSize(m_clientWidth, m_clientHeight);
    InitEditorSessionMode();
    if (m_noPanel)
    {
        SetCursorMode(false);
    }

    m_lastFrameTime = std::chrono::steady_clock::now();

    while (m_running && !m_window.QuitRequested())
    {
        m_window.PollEvents([this](const EditorEvent &event) { HandleEvent(event); });

        if (!m_running || m_window.QuitRequested())
        {
            break;
        }

        if (PollPlayProcess())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            m_lastFrameTime = std::chrono::steady_clock::now();
            continue;
        }

        Frame();
    }

    Shutdown();
    m_window.Destroy();
    return m_window.ExitCode();
}

void EditorApp::Shutdown()
{
    ShutdownAiTextureGeneratorDialog();
    ShutdownEditorOnnxRuntime();
    EditorDetachProcess(m_playProcess);
    m_window.SetMouseCapture(false);
    m_ui.SetCursorVisible(true);
    m_ui.Shutdown();

    m_runtime.DestroyGpuResources(m_instance);
    m_previewMaterialBakes.Destroy(m_instance);

    if (m_material != QR_NO_MATERIAL && m_instance != nullptr)
    {
        qrDestroyMaterial(m_instance, m_material);
        m_material = QR_NO_MATERIAL;
    }

    if (m_instance != nullptr)
    {
        qrDestroyInstance(m_instance);
        m_instance = nullptr;
    }
}
