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

#include "EditorViewport.h"

#include <algorithm>
#include <cmath>

#include <imgui.h>

namespace
{

constexpr float kOutlineNearClip = 1.0f;
constexpr float kOutlineNudgeUnits = 0.35f;
constexpr size_t kOutlineMaxVerts = 256;

float Dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

bool RayTriangleIntersect(const float origin[3], const float direction[3], const float a[3],
                          const float b[3], const float c[3], float &outDistance)
{
    const float edge1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float edge2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };

    const float pvec[3] = {
        direction[1] * edge2[2] - direction[2] * edge2[1],
        direction[2] * edge2[0] - direction[0] * edge2[2],
        direction[0] * edge2[1] - direction[1] * edge2[0],
    };

    const float det = edge1[0] * pvec[0] + edge1[1] * pvec[1] + edge1[2] * pvec[2];

    if (std::fabs(det) < 1e-9f)
    {
        return false;
    }

    const float inv = 1.0f / det;
    const float tvec[3] = { origin[0] - a[0], origin[1] - a[1], origin[2] - a[2] };
    const float u = (tvec[0] * pvec[0] + tvec[1] * pvec[1] + tvec[2] * pvec[2]) * inv;

    if (u < 0.0f || u > 1.0f)
    {
        return false;
    }

    const float qvec[3] = {
        tvec[1] * edge1[2] - tvec[2] * edge1[1],
        tvec[2] * edge1[0] - tvec[0] * edge1[2],
        tvec[0] * edge1[1] - tvec[1] * edge1[0],
    };

    const float v = (direction[0] * qvec[0] + direction[1] * qvec[1] + direction[2] * qvec[2]) * inv;

    if (v < 0.0f || u + v > 1.0f)
    {
        return false;
    }

    const float dist = (edge2[0] * qvec[0] + edge2[1] * qvec[1] + edge2[2] * qvec[2]) * inv;

    if (dist <= 0.0f)
    {
        return false;
    }

    outDistance = dist;
    return true;
}

bool PickFaceFan(const float origin[3], const float direction[3], const EditorSurfaceFace &face,
                 float &outDistance)
{
    if (face.positions.size() < 3)
    {
        return false;
    }

    bool hit = false;
    float best = 0.0f;
    const float *hub = face.positions[0].data;

    for (size_t k = 1; k + 1 < face.positions.size(); k++)
    {
        float dist = 0.0f;

        if (!RayTriangleIntersect(origin, direction, hub, face.positions[k].data,
                                  face.positions[k + 1].data, dist))
        {
            continue;
        }

        if (!hit || dist < best)
        {
            hit = true;
            best = dist;
        }
    }

    if (hit)
    {
        outDistance = best;
    }

    return hit;
}

bool PickSurfaceTrianglesFallback(const float origin[3], const float direction[3],
                                  const EditorSurface &surface, float &outDistance,
                                  uint32_t &outCluster)
{
    bool hit = false;
    float best = 0.0f;

    for (size_t i = 0; i + 2 < surface.indices.size(); i += 3)
    {
        const uint32_t ia = surface.indices[i + 0];
        const uint32_t ib = surface.indices[i + 1];
        const uint32_t ic = surface.indices[i + 2];

        if (ia >= surface.vertices.size() || ib >= surface.vertices.size() ||
            ic >= surface.vertices.size())
        {
            continue;
        }

        float dist = 0.0f;

        if (!RayTriangleIntersect(origin, direction, surface.vertices[ia].position,
                                  surface.vertices[ib].position, surface.vertices[ic].position,
                                  dist))
        {
            continue;
        }

        if (!hit || dist < best)
        {
            hit = true;
            best = dist;
            outCluster = surface.vertices[ia].cluster;
        }
    }

    if (hit)
    {
        outDistance = best;
    }

    return hit;
}

bool PickSingleSurface(const EditorSurface &surface, const float origin[3],
                       const float direction[3], int32_t &outFace, float &outDist,
                       uint32_t &outCluster)
{
    if (surface.faces.empty())
    {
        outFace = -1;
        return PickSurfaceTrianglesFallback(origin, direction, surface, outDist, outCluster);
    }

    bool hit = false;
    for (size_t f = 0; f < surface.faces.size(); f++)
    {
        float dist = 0.0f;
        if (PickFaceFan(origin, direction, surface.faces[f], dist) && (!hit || dist < outDist))
        {
            hit = true;
            outFace = int32_t(f);
            outDist = dist;
            outCluster = surface.faces[f].cluster;
        }
    }
    return hit;
}

} // namespace

bool ProjectClippedEdge(const EditorCameraBasis &cam, const float a[3], const float b[3],
                        float outSeg[4])
{
    if (cam.tanHalfFovX <= 0.0f || cam.tanHalfFovY <= 0.0f || cam.viewportWidth <= 0.0f ||
        cam.viewportHeight <= 0.0f)
    {
        return false;
    }

    const float da[3] = { a[0] - cam.origin[0], a[1] - cam.origin[1], a[2] - cam.origin[2] };
    const float db[3] = { b[0] - cam.origin[0], b[1] - cam.origin[1], b[2] - cam.origin[2] };

    float za = Dot3(da, cam.forward);
    float zb = Dot3(db, cam.forward);

    if (za <= kOutlineNearClip && zb <= kOutlineNearClip)
    {
        return false;
    }

    float xa = Dot3(da, cam.right);
    float ya = Dot3(da, cam.up);
    float xb = Dot3(db, cam.right);
    float yb = Dot3(db, cam.up);

    if (za < kOutlineNearClip)
    {
        const float t = (kOutlineNearClip - za) / (zb - za);
        xa += t * (xb - xa);
        ya += t * (yb - ya);
        za = kOutlineNearClip;
    }
    else if (zb < kOutlineNearClip)
    {
        const float t = (kOutlineNearClip - za) / (zb - za);
        xb = xa + t * (xb - xa);
        yb = ya + t * (yb - ya);
        zb = kOutlineNearClip;
    }

    outSeg[0] = (0.5f + 0.5f * (xa / za) / cam.tanHalfFovX) * cam.viewportWidth;
    outSeg[1] = (0.5f - 0.5f * (ya / za) / cam.tanHalfFovY) * cam.viewportHeight;
    outSeg[2] = (0.5f + 0.5f * (xb / zb) / cam.tanHalfFovX) * cam.viewportWidth;
    outSeg[3] = (0.5f - 0.5f * (yb / zb) / cam.tanHalfFovY) * cam.viewportHeight;
    return true;
}

void EmitClippedPolygonOutline(ImDrawList *drawList, const EditorCameraBasis &cam,
                               const EditorSurfaceFace &face, uint32_t color, float thickness)
{
    if (drawList == nullptr || face.positions.size() < 3)
    {
        return;
    }

    const size_t n = std::min(face.positions.size(), kOutlineMaxVerts);
    const float toView[3] = {
        cam.origin[0] - face.positions[0].data[0],
        cam.origin[1] - face.positions[0].data[1],
        cam.origin[2] - face.positions[0].data[2],
    };
    const float nudge = Dot3(face.normal, toView) < 0.0f ? -kOutlineNudgeUnits : kOutlineNudgeUnits;

    std::vector<QrFloat3D> world(n);

    for (size_t i = 0; i < n; i++)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            world[i].data[axis] = face.positions[i].data[axis] + face.normal[axis] * nudge;
        }
    }

    for (size_t i = 0; i < n; i++)
    {
        float seg[4] = {};

        if (ProjectClippedEdge(cam, world[i].data, world[(i + 1) % n].data, seg))
        {
            drawList->AddLine(ImVec2(seg[0], seg[1]), ImVec2(seg[2], seg[3]), ImU32(color),
                              thickness);
        }
    }
}

void EmitClippedAabbOutline(ImDrawList *drawList, const EditorCameraBasis &cam,
                            const float boundsMin[3], const float boundsMax[3], uint32_t color,
                            float thickness)
{
    if (drawList == nullptr || boundsMin == nullptr || boundsMax == nullptr)
    {
        return;
    }

    float mn[3] = {};
    float mx[3] = {};
    for (int axis = 0; axis < 3; axis++)
    {
        const float lo = std::min(boundsMin[axis], boundsMax[axis]);
        const float hi = std::max(boundsMin[axis], boundsMax[axis]);
        mn[axis] = lo - kOutlineNudgeUnits;
        mx[axis] = hi + kOutlineNudgeUnits;
        if (mx[axis] - mn[axis] < 2.0f)
        {
            const float mid = 0.5f * (lo + hi);
            mn[axis] = mid - 1.0f;
            mx[axis] = mid + 1.0f;
        }
    }

    const float corners[8][3] = {
        { mn[0], mn[1], mn[2] }, { mx[0], mn[1], mn[2] }, { mx[0], mx[1], mn[2] },
        { mn[0], mx[1], mn[2] }, { mn[0], mn[1], mx[2] }, { mx[0], mn[1], mx[2] },
        { mx[0], mx[1], mx[2] }, { mn[0], mx[1], mx[2] },
    };
    static constexpr int kEdges[12][2] = {
        { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
        { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
        { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
    };

    for (const auto &edge : kEdges)
    {
        float seg[4] = {};
        if (ProjectClippedEdge(cam, corners[edge[0]], corners[edge[1]], seg))
        {
            drawList->AddLine(ImVec2(seg[0], seg[1]), ImVec2(seg[2], seg[3]), ImU32(color),
                              thickness);
        }
    }
}

EditorFacePick PickSceneFace(const EditorScene &scene, const float origin[3],
                             const float direction[3], const EditorMaterialTable *materials)
{
    EditorFacePick best;

    for (size_t s = 0; s < scene.surfaces.size(); s++)
    {
        const EditorSurface &surface = scene.surfaces[s];
        if (materials != nullptr && !materials->For(surface).enabled)
        {
            continue;
        }

        int32_t face = -1;
        float dist = 0.0f;
        uint32_t cluster = 0;
        if (PickSingleSurface(surface, origin, direction, face, dist, cluster) &&
            (best.surface < 0 || dist < best.distance))
        {
            best = { int32_t(s), face, dist, {}, cluster };
        }
    }

    if (best.surface >= 0)
    {
        for (int axis = 0; axis < 3; axis++)
        {
            best.point[axis] = origin[axis] + direction[axis] * best.distance;
        }
    }

    return best;
}
