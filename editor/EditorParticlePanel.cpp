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
#include "EditorParticles.h"
#include "EditorSpatial.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace
{

constexpr float kGizmoDegToRad = 3.14159265358979323846f / 180.0f;

constexpr int kBoxEdgeIndices[12][2] = {
    { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },
    { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },
    { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
};

void EmitWorldSegment(ImDrawList *drawList, const EditorCameraBasis &cam, const float a[3],
                      const float b[3], ImU32 color, float thickness)
{
    float seg[4] = {};
    if (ProjectClippedEdge(cam, a, b, seg))
    {
        drawList->AddLine(ImVec2(seg[0], seg[1]), ImVec2(seg[2], seg[3]), color, thickness);
    }
}

void BuildOrthonormalFrame(const float dirIn[3], float outFwd[3], float outRight[3], float outUp[3])
{
    const float lenSq = dirIn[0] * dirIn[0] + dirIn[1] * dirIn[1] + dirIn[2] * dirIn[2];
    const float invLen = lenSq > 1e-8f ? (1.0f / std::sqrt(lenSq)) : 1.0f;
    outFwd[0] = lenSq > 1e-8f ? dirIn[0] * invLen : 0.0f;
    outFwd[1] = lenSq > 1e-8f ? dirIn[1] * invLen : 0.0f;
    outFwd[2] = lenSq > 1e-8f ? dirIn[2] * invLen : 1.0f;

    const float ref[3] = {
        std::abs(outFwd[2]) < 0.9f ? 0.0f : 1.0f,
        0.0f,
        std::abs(outFwd[2]) < 0.9f ? 1.0f : 0.0f,
    };
    outRight[0] = outFwd[1] * ref[2] - outFwd[2] * ref[1];
    outRight[1] = outFwd[2] * ref[0] - outFwd[0] * ref[2];
    outRight[2] = outFwd[0] * ref[1] - outFwd[1] * ref[0];
    const float rLen = std::sqrt(outRight[0] * outRight[0] + outRight[1] * outRight[1] + outRight[2] * outRight[2]);
    if (rLen > 1e-6f)
    {
        outRight[0] /= rLen;
        outRight[1] /= rLen;
        outRight[2] /= rLen;
    }
    outUp[0] = outRight[1] * outFwd[2] - outRight[2] * outFwd[1];
    outUp[1] = outRight[2] * outFwd[0] - outRight[0] * outFwd[2];
    outUp[2] = outRight[0] * outFwd[1] - outRight[1] * outFwd[0];
}

void DrawEmitterGizmo(ImDrawList *drawList, const EditorCameraBasis &cam,
                      const EditorParticleEmitterDef &em, bool selected)
{
    const ImU32 color = selected ? IM_COL32(255, 200, 72, 240)
                                 : (em.enabled ? IM_COL32(255, 130, 56, 170) : IM_COL32(140, 140, 140, 110));
    const float thick = selected ? 2.2f : 1.4f;
    const float cross = 8.0f;
    for (int axis = 0; axis < 3; axis++)
    {
        float a[3] = { em.origin[0], em.origin[1], em.origin[2] };
        float b[3] = { em.origin[0], em.origin[1], em.origin[2] };
        a[axis] -= cross;
        b[axis] += cross;
        EmitWorldSegment(drawList, cam, a, b, color, thick);
    }

    float fwd[3] = {}, right[3] = {}, up[3] = {};
    BuildOrthonormalFrame(em.direction, fwd, right, up);
    const float len = 36.0f;
    const float rad = len * std::tan(std::clamp(em.coneHalfAngleDeg, 2.0f, 80.0f) * kGizmoDegToRad);
    const float tip[3] = {
        em.origin[0] + fwd[0] * len,
        em.origin[1] + fwd[1] * len,
        em.origin[2] + fwd[2] * len,
    };
    EmitWorldSegment(drawList, cam, em.origin, tip, color, thick);

    for (int i = 0; i < 4; i++)
    {
        const float angle = float(i) * (3.14159265f * 0.5f);
        const float c = std::cos(angle) * rad;
        const float s = std::sin(angle) * rad;
        const float rim[3] = {
            tip[0] + right[0] * c + up[0] * s,
            tip[1] + right[1] * c + up[1] * s,
            tip[2] + right[2] * c + up[2] * s,
        };
        EmitWorldSegment(drawList, cam, em.origin, rim, color, 1.3f);
    }
}

void DrawVectorFieldGizmo(ImDrawList *drawList, const EditorCameraBasis &cam,
                          const EditorVectorFieldDef &vf, bool selected)
{
    const ImU32 color = selected ? IM_COL32(90, 235, 255, 235)
                                 : (vf.enabled ? IM_COL32(64, 180, 230, 165) : IM_COL32(130, 130, 140, 100));
    const float thick = selected ? 2.1f : 1.3f;

    float corners[8][3] = {};
    ComputeEditorVectorFieldBoxCorners(vf, corners);
    for (int e = 0; e < 12; e++)
    {
        EmitWorldSegment(drawList, cam, corners[kBoxEdgeIndices[e][0]], corners[kBoxEdgeIndices[e][1]],
                         color, thick);
    }

    float flowAxis[3] = {};
    ComputeEditorVectorFieldFlowAxis(vf, flowAxis);
    const float arrowLen = 40.0f;
    const float endPt[3] = {
        vf.center[0] + flowAxis[0] * arrowLen,
        vf.center[1] + flowAxis[1] * arrowLen,
        vf.center[2] + flowAxis[2] * arrowLen,
    };
    EmitWorldSegment(drawList, cam, vf.center, endPt, IM_COL32(180, 255, 160, 230), thick + 0.4f);
}

constexpr int kGizmoRingSegs = 36;
constexpr float kGizmoGrabRadiusPx = 11.0f;
constexpr float kGizmoPickRadiusPx = 14.0f;

struct ParticleGizmoContext
{
    EditorSceneRuntime &runtime;
    int32_t &selectedEmitter;
    int32_t &selectedField;
    int32_t particleTab;
    bool hasPick;
    float *pickPoint;
    float *pickNormal;
    EditorApp::ParticleGizmoState &gizmo;
};

struct ParticleGizmoBasis
{
    bool valid = false;
    EditorApp::ParticleGizmoTarget target = EditorApp::ParticleGizmoTarget::None;
    std::string label;
    float center[3] = {};
    float rotAxes[3][3] = {};
    float ringRadius = 32.0f;
    float arrowLen = 40.0f;
};

struct ParticleGizmoHit
{
    EditorApp::ParticleGizmoHandle handle = EditorApp::ParticleGizmoHandle::None;
    int32_t axis = -1;
    float scoreDist = 1e30f;
    float tangent[2] = {};
    float pixelsPerUnit = 1.0f;
    float worldAxis[3] = {};
};

bool ProjectCamPoint(const EditorCameraBasis &cam, const float pt[3], float &outX, float &outY)
{
    const float rel[3] = {
        pt[0] - cam.origin[0],
        pt[1] - cam.origin[1],
        pt[2] - cam.origin[2],
    };
    const float depth = rel[0] * cam.forward[0] + rel[1] * cam.forward[1] + rel[2] * cam.forward[2];
    if (depth <= 1e-2f || cam.viewportWidth <= 0.0f || cam.viewportHeight <= 0.0f)
    {
        return false;
    }
    const float hx = rel[0] * cam.right[0] + rel[1] * cam.right[1] + rel[2] * cam.right[2];
    const float hy = rel[0] * cam.up[0] + rel[1] * cam.up[1] + rel[2] * cam.up[2];
    const float ndcX = hx / (depth * cam.tanHalfFovX);
    const float ndcY = hy / (depth * cam.tanHalfFovY);
    outX = (ndcX * 0.5f + 0.5f) * cam.viewportWidth;
    outY = (1.0f - (ndcY * 0.5f + 0.5f)) * cam.viewportHeight;
    return true;
}

float DistPointToSegment2D(float px, float py, const float seg[4])
{
    const float dx = seg[2] - seg[0];
    const float dy = seg[3] - seg[1];
    const float lenSq = dx * dx + dy * dy;
    const float t = lenSq > 1e-6f
                        ? std::clamp(((px - seg[0]) * dx + (py - seg[1]) * dy) / lenSq, 0.0f, 1.0f)
                        : 0.0f;
    const float qx = seg[0] + t * dx;
    const float qy = seg[1] + t * dy;
    return std::hypot(px - qx, py - qy);
}

ImU32 GizmoAxisColor(int axis, uint8_t alpha, bool active)
{
    if (active)
    {
        return IM_COL32(255, 232, 64, alpha);
    }
    if (axis == 0)
    {
        return IM_COL32(255, 78, 78, alpha);
    }
    if (axis == 1)
    {
        return IM_COL32(72, 242, 96, alpha);
    }
    return IM_COL32(80, 158, 255, alpha);
}

void ClampParticleSelections(ParticleGizmoContext &ctx)
{
    const int32_t emCount = static_cast<int32_t>(ctx.runtime.particleEmitters.size());
    const int32_t vfCount = static_cast<int32_t>(ctx.runtime.vectorFields.size());
    ctx.selectedEmitter = std::clamp(ctx.selectedEmitter, emCount > 0 ? 0 : -1, emCount - 1);
    ctx.selectedField = std::clamp(ctx.selectedField, vfCount > 0 ? 0 : -1, vfCount - 1);
}

EditorApp::ParticleGizmoTarget ResolveTabDefaultTarget(const ParticleGizmoContext &ctx,
                                                       bool emValid, bool vfValid)
{
    if (ctx.particleTab == 1)
    {
        return emValid ? EditorApp::ParticleGizmoTarget::Emitter
                       : (vfValid ? EditorApp::ParticleGizmoTarget::VectorField
                                  : EditorApp::ParticleGizmoTarget::None);
    }
    if (ctx.particleTab == 2)
    {
        return vfValid ? EditorApp::ParticleGizmoTarget::VectorField
                       : (emValid ? EditorApp::ParticleGizmoTarget::Emitter
                                  : EditorApp::ParticleGizmoTarget::None);
    }
    if (ctx.hasPick)
    {
        return EditorApp::ParticleGizmoTarget::BurstAim;
    }
    if (vfValid)
    {
        return EditorApp::ParticleGizmoTarget::VectorField;
    }
    return emValid ? EditorApp::ParticleGizmoTarget::Emitter
                   : EditorApp::ParticleGizmoTarget::None;
}

void SyncActiveParticleGizmoTarget(ParticleGizmoContext &ctx)
{
    ClampParticleSelections(ctx);
    if (ctx.gizmo.dragging)
    {
        return;
    }

    const int32_t emCount = static_cast<int32_t>(ctx.runtime.particleEmitters.size());
    const int32_t vfCount = static_cast<int32_t>(ctx.runtime.vectorFields.size());
    const bool emValid = ctx.selectedEmitter >= 0 && ctx.selectedEmitter < emCount;
    const bool vfValid = ctx.selectedField >= 0 && ctx.selectedField < vfCount;

    if (ctx.particleTab == 0)
    {
        const bool keepEm = ctx.gizmo.activeTarget == EditorApp::ParticleGizmoTarget::Emitter && emValid;
        const bool keepVf = ctx.gizmo.activeTarget == EditorApp::ParticleGizmoTarget::VectorField && vfValid;
        const bool keepAim = ctx.gizmo.activeTarget == EditorApp::ParticleGizmoTarget::BurstAim && ctx.hasPick;
        if (keepEm || keepVf || keepAim)
        {
            return;
        }
    }
    ctx.gizmo.activeTarget = ResolveTabDefaultTarget(ctx, emValid, vfValid);
}

void AssignIdentityAxes(float outAxes[3][3])
{
    for (int r = 0; r < 3; r++)
    {
        for (int c = 0; c < 3; c++)
        {
            outAxes[r][c] = (r == c) ? 1.0f : 0.0f;
        }
    }
}

ParticleGizmoBasis BuildParticleGizmoBasis(ParticleGizmoContext &ctx, const EditorCameraBasis &cam)
{
    SyncActiveParticleGizmoTarget(ctx);
    ParticleGizmoBasis basis;
    AssignIdentityAxes(basis.rotAxes);
    basis.target = ctx.gizmo.activeTarget;

    if (basis.target == EditorApp::ParticleGizmoTarget::Emitter && ctx.selectedEmitter >= 0 &&
        size_t(ctx.selectedEmitter) < ctx.runtime.particleEmitters.size())
    {
        const EditorParticleEmitterDef &em = ctx.runtime.particleEmitters[size_t(ctx.selectedEmitter)];
        std::copy_n(em.origin, 3, basis.center);
        basis.label = "Emitter: " + em.name;
        basis.valid = true;
    }
    else if (basis.target == EditorApp::ParticleGizmoTarget::VectorField && ctx.selectedField >= 0 &&
             size_t(ctx.selectedField) < ctx.runtime.vectorFields.size())
    {
        const EditorVectorFieldDef &vf = ctx.runtime.vectorFields[size_t(ctx.selectedField)];
        std::copy_n(vf.center, 3, basis.center);
        float rot[3][3] = {};
        BuildEditorEulerRotation3x3(vf.anglesDeg, rot);
        for (int ax = 0; ax < 3; ax++)
        {
            basis.rotAxes[ax][0] = rot[0][ax];
            basis.rotAxes[ax][1] = rot[1][ax];
            basis.rotAxes[ax][2] = rot[2][ax];
        }
        basis.label = "Field: " + vf.name;
        basis.valid = true;
    }
    else if (basis.target == EditorApp::ParticleGizmoTarget::BurstAim && ctx.hasPick &&
             ctx.pickPoint != nullptr)
    {
        std::copy_n(ctx.pickPoint, 3, basis.center);
        basis.label = "Burst Aim";
        basis.valid = true;
    }

    if (basis.valid)
    {
        const float rel[3] = {
            basis.center[0] - cam.origin[0],
            basis.center[1] - cam.origin[1],
            basis.center[2] - cam.origin[2],
        };
        const float depth =
            std::max(1.0f, rel[0] * cam.forward[0] + rel[1] * cam.forward[1] + rel[2] * cam.forward[2]);
        basis.ringRadius = std::clamp(depth * 0.16f, 16.0f, 115.0f);
        basis.arrowLen = basis.ringRadius * 1.24f;
    }
    return basis;
}

void ComputeRingSegmentPoints(const ParticleGizmoBasis &basis, const float u[3], const float v[3],
                              int segIdx, float outP0[3], float outP1[3], float outMidDir[3])
{
    const float step = (2.0f * 3.14159265f) / float(kGizmoRingSegs);
    const float a0 = step * float(segIdx);
    const float a1 = step * float(segIdx + 1);
    const float aMid = 0.5f * (a0 + a1);
    const float c0 = std::cos(a0), s0 = std::sin(a0);
    const float c1 = std::cos(a1), s1 = std::sin(a1);
    for (int k = 0; k < 3; k++)
    {
        outP0[k] = basis.center[k] + basis.ringRadius * (c0 * u[k] + s0 * v[k]);
        outP1[k] = basis.center[k] + basis.ringRadius * (c1 * u[k] + s1 * v[k]);
        outMidDir[k] = std::cos(aMid) * u[k] + std::sin(aMid) * v[k];
    }
}

bool IsRingFrontFacing(const EditorCameraBasis &cam, const float center[3], const float midDir[3])
{
    const float toCam[3] = {
        cam.origin[0] - center[0],
        cam.origin[1] - center[1],
        cam.origin[2] - center[2],
    };
    return (midDir[0] * toCam[0] + midDir[1] * toCam[1] + midDir[2] * toCam[2]) >= -0.05f;
}

void DrawParticleRotationRing(ImDrawList *drawList, const EditorCameraBasis &cam,
                              const ParticleGizmoBasis &basis, int axis, bool highlighted)
{
    float dummy[3] = {}, u[3] = {}, v[3] = {};
    BuildOrthonormalFrame(basis.rotAxes[axis], dummy, u, v);

    for (int s = 0; s < kGizmoRingSegs; s++)
    {
        float p0[3] = {}, p1[3] = {}, midDir[3] = {};
        ComputeRingSegmentPoints(basis, u, v, s, p0, p1, midDir);
        const bool front = IsRingFrontFacing(cam, basis.center, midDir);
        const ImU32 col = GizmoAxisColor(axis, front ? 240u : 68u, highlighted);
        const float thick = highlighted ? (front ? 3.8f : 2.0f) : (front ? 2.6f : 1.3f);

        float seg[4] = {};
        if (ProjectClippedEdge(cam, p0, p1, seg))
        {
            if (highlighted && front)
            {
                drawList->AddLine(ImVec2(seg[0], seg[1]), ImVec2(seg[2], seg[3]),
                                  IM_COL32(0, 0, 0, 180), thick + 2.0f);
            }
            drawList->AddLine(ImVec2(seg[0], seg[1]), ImVec2(seg[2], seg[3]), col, thick);
        }
    }
}

void DrawParticleMoveArrow(ImDrawList *drawList, const EditorCameraBasis &cam,
                           const ParticleGizmoBasis &basis, int axis, bool highlighted)
{
    const float axisVec[3] = {
        axis == 0 ? 1.0f : 0.0f,
        axis == 1 ? 1.0f : 0.0f,
        axis == 2 ? 1.0f : 0.0f,
    };
    const float tip[3] = {
        basis.center[0] + axisVec[0] * basis.arrowLen,
        basis.center[1] + axisVec[1] * basis.arrowLen,
        basis.center[2] + axisVec[2] * basis.arrowLen,
    };
    const ImU32 col = GizmoAxisColor(axis, 245u, highlighted);
    const float thick = highlighted ? 3.8f : 2.6f;

    float seg[4] = {};
    if (ProjectClippedEdge(cam, basis.center, tip, seg))
    {
        drawList->AddLine(ImVec2(seg[0], seg[1]), ImVec2(seg[2], seg[3]),
                          IM_COL32(0, 0, 0, 170), thick + 2.0f);
        drawList->AddLine(ImVec2(seg[0], seg[1]), ImVec2(seg[2], seg[3]), col, thick);
    }

    float dummy[3] = {}, sideA[3] = {}, sideB[3] = {};
    BuildOrthonormalFrame(axisVec, dummy, sideA, sideB);
    const float headLen = basis.arrowLen * 0.16f;
    const float headRad = basis.arrowLen * 0.06f;
    for (int side = -1; side <= 1; side += 2)
    {
        const float wing[3] = {
            tip[0] - axisVec[0] * headLen + sideA[0] * (float(side) * headRad),
            tip[1] - axisVec[1] * headLen + sideA[1] * (float(side) * headRad),
            tip[2] - axisVec[2] * headLen + sideA[2] * (float(side) * headRad),
        };
        EmitWorldSegment(drawList, cam, tip, wing, col, thick);
    }
}

void DrawBadgeCircle(ImDrawList *drawList, const EditorCameraBasis &cam,
                     const float pos[3], bool active, ImU32 baseCol)
{
    float sx = 0.0f, sy = 0.0f;
    if (ProjectCamPoint(cam, pos, sx, sy))
    {
        const ImU32 activeCol = IM_COL32(255, 228, 96, 245);
        drawList->AddCircleFilled(ImVec2(sx, sy), active ? 5.5f : 4.2f, active ? activeCol : baseCol);
        drawList->AddCircle(ImVec2(sx, sy), active ? 6.5f : 5.0f, IM_COL32(15, 18, 24, 220), 0, 1.5f);
    }
}

void DrawParticleSelectableBadges(ImDrawList *drawList, const EditorCameraBasis &cam,
                                  const ParticleGizmoContext &ctx)
{
    for (size_t i = 0; i < ctx.runtime.particleEmitters.size(); i++)
    {
        const bool active =
            ctx.gizmo.activeTarget == EditorApp::ParticleGizmoTarget::Emitter &&
            int32_t(i) == ctx.selectedEmitter;
        DrawBadgeCircle(drawList, cam, ctx.runtime.particleEmitters[i].origin, active,
                        IM_COL32(255, 145, 64, 210));
    }
    for (size_t i = 0; i < ctx.runtime.vectorFields.size(); i++)
    {
        const bool active =
            ctx.gizmo.activeTarget == EditorApp::ParticleGizmoTarget::VectorField &&
            int32_t(i) == ctx.selectedField;
        DrawBadgeCircle(drawList, cam, ctx.runtime.vectorFields[i].center, active,
                        IM_COL32(80, 220, 255, 215));
    }
    if (ctx.gizmo.activeTarget == EditorApp::ParticleGizmoTarget::BurstAim && ctx.hasPick &&
        ctx.pickPoint != nullptr && ctx.pickNormal != nullptr)
    {
        const float tip[3] = {
            ctx.pickPoint[0] + ctx.pickNormal[0] * 44.0f,
            ctx.pickPoint[1] + ctx.pickNormal[1] * 44.0f,
            ctx.pickPoint[2] + ctx.pickNormal[2] * 44.0f,
        };
        EmitWorldSegment(drawList, cam, ctx.pickPoint, tip, IM_COL32(255, 225, 72, 245), 2.6f);
    }
}

void DrawParticleGizmoManipulator(ImDrawList *drawList, const EditorCameraBasis &cam,
                                  const ParticleGizmoBasis &basis,
                                  const EditorApp::ParticleGizmoState &gizmo)
{
    if (!basis.valid)
    {
        return;
    }
    const EditorApp::ParticleGizmoHandle activeHandle =
        gizmo.dragging ? gizmo.dragHandle : gizmo.hoverHandle;
    const int32_t activeAxis = gizmo.dragging ? gizmo.dragAxis : gizmo.hoverAxis;

    if (gizmo.tool != EditorApp::ParticleGizmoTool::TranslateOnly)
    {
        for (int ax = 0; ax < 3; ax++)
        {
            const bool hi = (activeHandle == EditorApp::ParticleGizmoHandle::RotateRing) && (activeAxis == ax);
            DrawParticleRotationRing(drawList, cam, basis, ax, hi);
        }
    }
    if (gizmo.tool != EditorApp::ParticleGizmoTool::RotateOnly)
    {
        for (int ax = 0; ax < 3; ax++)
        {
            const bool hi = (activeHandle == EditorApp::ParticleGizmoHandle::MoveAxis) && (activeAxis == ax);
            DrawParticleMoveArrow(drawList, cam, basis, ax, hi);
        }
    }
}

void HitTestRingAxis(const EditorCameraBasis &cam, const ParticleGizmoBasis &basis,
                     int axis, ImVec2 cursor, ParticleGizmoHit &best)
{
    float dummy[3] = {}, u[3] = {}, v[3] = {};
    BuildOrthonormalFrame(basis.rotAxes[axis], dummy, u, v);

    for (int s = 0; s < kGizmoRingSegs; s++)
    {
        float p0[3] = {}, p1[3] = {}, midDir[3] = {};
        ComputeRingSegmentPoints(basis, u, v, s, p0, p1, midDir);
        float seg[4] = {};
        if (!ProjectClippedEdge(cam, p0, p1, seg))
        {
            continue;
        }
        const float d = DistPointToSegment2D(cursor.x, cursor.y, seg);
        const bool front = IsRingFrontFacing(cam, basis.center, midDir);
        const float scored = d + (front ? 0.0f : 3.0f);
        if (d > kGizmoGrabRadiusPx || scored >= best.scoreDist)
        {
            continue;
        }

        const float offset[3] = {
            basis.ringRadius * midDir[0],
            basis.ringRadius * midDir[1],
            basis.ringRadius * midDir[2],
        };
        float nextOffset[3] = {};
        RotateEditorVectorAroundAxis(offset, basis.rotAxes[axis], 2.0f, nextOffset);
        const float hitPt[3] = { basis.center[0] + offset[0], basis.center[1] + offset[1], basis.center[2] + offset[2] };
        const float stepPt[3] = { basis.center[0] + nextOffset[0] * basis.ringRadius,
                                  basis.center[1] + nextOffset[1] * basis.ringRadius,
                                  basis.center[2] + nextOffset[2] * basis.ringRadius };
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        if (!ProjectCamPoint(cam, hitPt, x0, y0) || !ProjectCamPoint(cam, stepPt, x1, y1))
        {
            continue;
        }
        const float tLen = std::hypot(x1 - x0, y1 - y0);
        best.handle = EditorApp::ParticleGizmoHandle::RotateRing;
        best.axis = axis;
        best.scoreDist = scored;
        best.tangent[0] = tLen > 1e-4f ? (x1 - x0) / tLen : 1.0f;
        best.tangent[1] = tLen > 1e-4f ? (y1 - y0) / tLen : 0.0f;
        std::copy_n(basis.rotAxes[axis], 3, best.worldAxis);
    }
}

void HitTestMoveArrows(const EditorCameraBasis &cam, const ParticleGizmoBasis &basis,
                       ImVec2 cursor, ParticleGizmoHit &best)
{
    for (int ax = 0; ax < 3; ax++)
    {
        const float axisVec[3] = {
            ax == 0 ? 1.0f : 0.0f,
            ax == 1 ? 1.0f : 0.0f,
            ax == 2 ? 1.0f : 0.0f,
        };
        const float tip[3] = {
            basis.center[0] + axisVec[0] * basis.arrowLen,
            basis.center[1] + axisVec[1] * basis.arrowLen,
            basis.center[2] + axisVec[2] * basis.arrowLen,
        };
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        if (!ProjectCamPoint(cam, basis.center, x0, y0) || !ProjectCamPoint(cam, tip, x1, y1))
        {
            continue;
        }
        const float seg[4] = { x0, y0, x1, y1 };
        const float d = DistPointToSegment2D(cursor.x, cursor.y, seg);
        const float len = std::hypot(x1 - x0, y1 - y0);
        if (d <= kGizmoGrabRadiusPx && len >= 8.0f && d < best.scoreDist)
        {
            best.handle = EditorApp::ParticleGizmoHandle::MoveAxis;
            best.axis = ax;
            best.scoreDist = d;
            best.tangent[0] = (x1 - x0) / len;
            best.tangent[1] = (y1 - y0) / len;
            best.pixelsPerUnit = len / basis.arrowLen;
            std::copy_n(axisVec, 3, best.worldAxis);
        }
    }
}

ParticleGizmoHit HitTestParticleGizmo(const EditorCameraBasis &cam, const ParticleGizmoBasis &basis,
                                      const EditorApp::ParticleGizmoState &gizmo, ImVec2 cursor)
{
    ParticleGizmoHit best;
    if (!basis.valid)
    {
        return best;
    }
    if (gizmo.tool != EditorApp::ParticleGizmoTool::RotateOnly)
    {
        HitTestMoveArrows(cam, basis, cursor, best);
    }
    if (gizmo.tool != EditorApp::ParticleGizmoTool::TranslateOnly)
    {
        for (int ax = 0; ax < 3; ax++)
        {
            HitTestRingAxis(cam, basis, ax, cursor, best);
        }
    }
    return best;
}

void BackupOrRestoreGizmoTarget(ParticleGizmoContext &ctx, bool restore)
{
    auto &g = ctx.gizmo;
    if (g.activeTarget == EditorApp::ParticleGizmoTarget::Emitter && ctx.selectedEmitter >= 0 &&
        size_t(ctx.selectedEmitter) < ctx.runtime.particleEmitters.size())
    {
        EditorParticleEmitterDef &em = ctx.runtime.particleEmitters[size_t(ctx.selectedEmitter)];
        if (restore)
        {
            std::copy_n(g.backupPos, 3, em.origin);
            std::copy_n(g.backupDir, 3, em.direction);
            em.cluster = ResolveEditorLightCluster(ctx.runtime.scene, em.origin);
            ctx.runtime.particlesDirty = true;
        }
        else
        {
            std::copy_n(em.origin, 3, g.backupPos);
            std::copy_n(em.direction, 3, g.backupDir);
        }
        return;
    }
    if (g.activeTarget == EditorApp::ParticleGizmoTarget::VectorField && ctx.selectedField >= 0 &&
        size_t(ctx.selectedField) < ctx.runtime.vectorFields.size())
    {
        EditorVectorFieldDef &vf = ctx.runtime.vectorFields[size_t(ctx.selectedField)];
        if (restore)
        {
            std::copy_n(g.backupPos, 3, vf.center);
            std::copy_n(g.backupAnglesDeg, 3, vf.anglesDeg);
            ctx.runtime.particlesDirty = true;
        }
        else
        {
            std::copy_n(vf.center, 3, g.backupPos);
            std::copy_n(vf.anglesDeg, 3, g.backupAnglesDeg);
        }
        return;
    }
    if (g.activeTarget == EditorApp::ParticleGizmoTarget::BurstAim && ctx.pickPoint != nullptr &&
        ctx.pickNormal != nullptr)
    {
        if (restore)
        {
            std::copy_n(g.backupPos, 3, ctx.pickPoint);
            std::copy_n(g.backupDir, 3, ctx.pickNormal);
        }
        else
        {
            std::copy_n(ctx.pickPoint, 3, g.backupPos);
            std::copy_n(ctx.pickNormal, 3, g.backupDir);
        }
    }
}

void TranslatePoint3(const float base[3], float deltaUnits, const float axis[3], float out[3])
{
    for (int k = 0; k < 3; k++)
    {
        out[k] = base[k] + deltaUnits * axis[k];
    }
}

void ApplyParticleGizmoDragDelta(ParticleGizmoContext &ctx, float deltaUnits, float deltaDeg)
{
    const auto &g = ctx.gizmo;
    const bool rot = (g.dragHandle == EditorApp::ParticleGizmoHandle::RotateRing);

    if (g.activeTarget == EditorApp::ParticleGizmoTarget::Emitter && ctx.selectedEmitter >= 0 &&
        size_t(ctx.selectedEmitter) < ctx.runtime.particleEmitters.size())
    {
        EditorParticleEmitterDef &em = ctx.runtime.particleEmitters[size_t(ctx.selectedEmitter)];
        if (rot)
        {
            RotateEditorVectorAroundAxis(g.backupDir, g.worldAxis, deltaDeg, em.direction);
        }
        else
        {
            TranslatePoint3(g.backupPos, deltaUnits, g.worldAxis, em.origin);
            em.cluster = ResolveEditorLightCluster(ctx.runtime.scene, em.origin);
        }
        ctx.runtime.particlesDirty = true;
        return;
    }
    if (g.activeTarget == EditorApp::ParticleGizmoTarget::VectorField && ctx.selectedField >= 0 &&
        size_t(ctx.selectedField) < ctx.runtime.vectorFields.size())
    {
        EditorVectorFieldDef &vf = ctx.runtime.vectorFields[size_t(ctx.selectedField)];
        if (rot)
        {
            RotateEditorEulerAnglesAroundAxis(g.backupAnglesDeg, g.worldAxis, deltaDeg, vf.anglesDeg);
        }
        else
        {
            TranslatePoint3(g.backupPos, deltaUnits, g.worldAxis, vf.center);
        }
        ctx.runtime.particlesDirty = true;
        return;
    }
    if (g.activeTarget == EditorApp::ParticleGizmoTarget::BurstAim && ctx.pickPoint != nullptr &&
        ctx.pickNormal != nullptr)
    {
        if (rot)
        {
            RotateEditorVectorAroundAxis(g.backupDir, g.worldAxis, deltaDeg, ctx.pickNormal);
        }
        else
        {
            TranslatePoint3(g.backupPos, deltaUnits, g.worldAxis, ctx.pickPoint);
        }
    }
}

void ResetActiveParticleGizmoRotation(ParticleGizmoContext &ctx)
{
    SyncActiveParticleGizmoTarget(ctx);
    if (ctx.gizmo.activeTarget == EditorApp::ParticleGizmoTarget::Emitter && ctx.selectedEmitter >= 0 &&
        size_t(ctx.selectedEmitter) < ctx.runtime.particleEmitters.size())
    {
        EditorParticleEmitterDef &em = ctx.runtime.particleEmitters[size_t(ctx.selectedEmitter)];
        em.direction[0] = 0.0f;
        em.direction[1] = 0.0f;
        em.direction[2] = 1.0f;
        ctx.runtime.particlesDirty = true;
    }
    else if (ctx.gizmo.activeTarget == EditorApp::ParticleGizmoTarget::VectorField && ctx.selectedField >= 0 &&
             size_t(ctx.selectedField) < ctx.runtime.vectorFields.size())
    {
        EditorVectorFieldDef &vf = ctx.runtime.vectorFields[size_t(ctx.selectedField)];
        std::fill_n(vf.anglesDeg, 3, 0.0f);
        ctx.runtime.particlesDirty = true;
    }
    else if (ctx.gizmo.activeTarget == EditorApp::ParticleGizmoTarget::BurstAim &&
             ctx.pickNormal != nullptr)
    {
        ctx.pickNormal[0] = 0.0f;
        ctx.pickNormal[1] = 0.0f;
        ctx.pickNormal[2] = 1.0f;
    }
}

const char *ActiveGizmoTargetLabel(const ParticleGizmoContext &ctx)
{
    if (ctx.gizmo.activeTarget == EditorApp::ParticleGizmoTarget::Emitter && ctx.selectedEmitter >= 0 &&
        size_t(ctx.selectedEmitter) < ctx.runtime.particleEmitters.size())
    {
        return ctx.runtime.particleEmitters[size_t(ctx.selectedEmitter)].name.c_str();
    }
    if (ctx.gizmo.activeTarget == EditorApp::ParticleGizmoTarget::VectorField && ctx.selectedField >= 0 &&
        size_t(ctx.selectedField) < ctx.runtime.vectorFields.size())
    {
        return ctx.runtime.vectorFields[size_t(ctx.selectedField)].name.c_str();
    }
    if (ctx.gizmo.activeTarget == EditorApp::ParticleGizmoTarget::BurstAim && ctx.hasPick)
    {
        return "Surface Burst Aim";
    }
    return "None";
}

void DrawParticleGizmoToolbar(ParticleGizmoContext &ctx)
{
    SyncActiveParticleGizmoTarget(ctx);
    static constexpr const char *kToolNames[3] = { "Move + Rotate", "Rotate", "Move" };
    int32_t toolIdx = static_cast<int32_t>(ctx.gizmo.tool);
    if (EditorGuiCombo("gizmo_tool", &toolIdx, kToolNames, 3))
    {
        ctx.gizmo.tool = static_cast<EditorApp::ParticleGizmoTool>(toolIdx);
    }
    ImGui::Text("Gizmo Target: %s", ActiveGizmoTargetLabel(ctx));
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset Rotation"))
    {
        ResetActiveParticleGizmoRotation(ctx);
    }
}

bool EditStringInput(const char *label, std::string &value)
{
    std::array<char, 256> buf = {};
    const size_t copyLen = std::min(value.size(), buf.size() - 1u);
    std::memcpy(buf.data(), value.data(), copyLen);
    if (ImGui::InputText(label, buf.data(), buf.size()))
    {
        value = buf.data();
        return true;
    }
    return false;
}

struct ParticleUiContext
{
    EditorSceneRuntime &runtime;
    const EditorRenderSettings &baselineSettings;
    QrInstance instance = nullptr;
    const float *cameraPos = nullptr;
    const float *viewForward = nullptr;
    bool hasPick = false;
    const float *pickPoint = nullptr;
    const float *pickNormal = nullptr;
    EditorApp::ParticleGizmoState &gizmo;
};

void DrawParticleHdrBloomSection(EditorRenderSettings &settings,
                                 const EditorRenderSettings &baseline)
{
    EditorGuiSectionHeader("Scene HDR Bloom",
                           "Optical post-effect Bloom reacting to HDR particle and scene radiance.");
    bool resetInt = false;
    EditorGuiSliderFloat(
        "bloom_intensity", &settings.bloomIntensity, 0.0f, 0.12f,
        { std::abs(settings.bloomIntensity - baseline.bloomIntensity) > 1e-5f, true,
          "Post-effect optical HDR Bloom halo strength around bright particles and lights.",
          "%.3f", &resetInt });
    if (resetInt)
    {
        settings.bloomIntensity = baseline.bloomIntensity;
    }

    bool resetThr = false;
    EditorGuiSliderFloat(
        "bloom_threshold", &settings.bloomThreshold, 0.5f, 20.0f,
        { std::abs(settings.bloomThreshold - baseline.bloomThreshold) > 1e-4f, true,
          "Exposure-normalized HDR luminance threshold where particles begin to cast Bloom glow.",
          "%.2f", &resetThr });
    if (resetThr)
    {
        settings.bloomThreshold = baseline.bloomThreshold;
    }
}

void DrawBurstTesterTab(const ParticleUiContext &ctx, bool &clickArmed,
                        EditorParticlePreset &preset, float &scale, float &emissionScale)
{
    EditorGuiSectionHeader("Interactive Surface Burst Tester",
                           "Click 3D geometry in the viewport or fire a preset burst immediately.");
    EditorGuiCheckbox("click surface to burst", &clickArmed,
                      { clickArmed, true, "Trigger selected burst preset on LMB viewport pick." });

    static constexpr int32_t kPresetCount = int32_t(EditorParticlePreset::Count) - 1;
    const char *kPresetNames[kPresetCount] = {
        EditorParticlePresetLabel(EditorParticlePreset::ExplosionSparks),
        EditorParticlePresetLabel(EditorParticlePreset::MetalStrikeSparks),
        EditorParticlePresetLabel(EditorParticlePreset::LightningConcreteHit),
        EditorParticlePresetLabel(EditorParticlePreset::FireEmbers),
        EditorParticlePresetLabel(EditorParticlePreset::WindDust),
        EditorParticlePresetLabel(EditorParticlePreset::QuakeRetroShotgun),
        EditorParticlePresetLabel(EditorParticlePreset::QuakeRetroBlood),
    };
    int32_t presetIdx = std::clamp(int32_t(preset) - 1, 0, kPresetCount - 1);
    if (EditorGuiCombo("burst_preset", &presetIdx, kPresetNames, kPresetCount))
    {
        preset = static_cast<EditorParticlePreset>(presetIdx + 1);
    }
    EditorGuiSliderFloat("burst_scale", &scale, 0.25f, 4.0f, { false, true, nullptr, "%.2f" });
    EditorGuiSliderFloat("hdr_brightness", &emissionScale, 0.0f, 5.0f,
                         { false, true,
                           "Scales burst HDR emission into optical Bloom and tonemapping.",
                           "%.2fx" });

    ImGui::BeginDisabled(!ctx.hasPick || ctx.instance == nullptr);
    if (ImGui::Button("Fire at Pick Point", ImVec2(-1.0f, 0.0f)))
    {
        ctx.runtime.SpawnPresetBurst(ctx.instance, preset, ctx.pickPoint, ctx.pickNormal,
                                     scale, emissionScale);
    }
    ImGui::EndDisabled();

    ImGui::BeginDisabled(ctx.instance == nullptr);
    if (ImGui::Button("Fire Ahead of Camera", ImVec2(-1.0f, 0.0f)))
    {
        const float origin[3] = {
            ctx.cameraPos[0] + ctx.viewForward[0] * 96.0f,
            ctx.cameraPos[1] + ctx.viewForward[1] * 96.0f,
            ctx.cameraPos[2] + ctx.viewForward[2] * 96.0f,
        };
        const float up[3] = { 0.0f, 0.0f, 1.0f };
        ctx.runtime.SpawnPresetBurst(ctx.instance, preset, origin, up, scale, emissionScale);
    }
    if (ImGui::Button("Clear Active GPU Particles", ImVec2(-1.0f, 0.0f)))
    {
        qrResetGpuParticles(ctx.instance);
    }
    ImGui::EndDisabled();

    DrawParticleHdrBloomSection(ctx.runtime.settings, ctx.baselineSettings);
}

void DrawEmitterTransformAndRates(EditorParticleEmitterDef &em, bool &dirty)
{
    EditorGuiSectionHeader("Transform & Emission");
    dirty |= EditorGuiCheckbox("enabled", &em.enabled);
    dirty |= EditorGuiVec3Input("origin", em.origin, 2.0f);
    dirty |= EditorGuiVec3Input("direction", em.direction, 0.05f);
    dirty |= EditorGuiSliderFloat("cone_half_angle", &em.coneHalfAngleDeg, 0.0f, 180.0f,
                                  { false, true, nullptr, "%.1f deg" });
    dirty |= EditorGuiSliderFloat("spawn_rate", &em.spawnRate, 0.0f, 500.0f,
                                  { false, true, nullptr, "%.1f /s" });
    int32_t burstCount = static_cast<int32_t>(em.burstCount);
    if (EditorGuiSliderInt("burst_count", &burstCount, 1, 2048))
    {
        em.burstCount = static_cast<uint32_t>(burstCount);
        dirty = true;
    }
    dirty |= EditorGuiSliderFloat("origin_jitter", &em.originJitterUnits, 0.0f, 64.0f,
                                  { false, true, nullptr, "%.1f u" });
    dirty |= EditorGuiSliderFloat("speed_min", &em.speedMinUnits, 0.0f, 1000.0f,
                                  { false, true, nullptr, "%.1f u/s" });
    dirty |= EditorGuiSliderFloat("speed_max", &em.speedMaxUnits, 0.0f, 1200.0f,
                                  { false, true, nullptr, "%.1f u/s" });
}

void DrawEmitterAppearanceAndPhysics(EditorParticleEmitterDef &em, bool &dirty)
{
    static constexpr const char *kModeNames[4] = {
        "Spark", "Billboard", "Retro Square", "Debris 3D"
    };
    EditorGuiSectionHeader("Rendering & Physics");
    int32_t modeIdx = static_cast<int32_t>(em.renderMode);
    if (EditorGuiCombo("render_mode", &modeIdx, kModeNames, 4))
    {
        em.renderMode = static_cast<QrGpuParticleRenderMode>(modeIdx);
        dirty = true;
    }
    dirty |= EditorGuiSliderFloat("size_min", &em.sizeStartMinUnits, 0.5f, 32.0f,
                                  { false, true, nullptr, "%.2f u" });
    dirty |= EditorGuiSliderFloat("size_max", &em.sizeStartMaxUnits, 0.5f, 48.0f,
                                  { false, true, nullptr, "%.2f u" });
    dirty |= EditorGuiSliderFloat("size_end_scale", &em.sizeEndScale, 0.0f, 4.0f);
    dirty |= EditorGuiSliderFloat("velocity_stretch", &em.velocityStretch, 0.0f, 0.15f,
                                  { false, true, nullptr, "%.3f s" });
    dirty |= EditorGuiSliderFloat("lifetime_min", &em.lifetimeMin, 0.05f, 12.0f);
    dirty |= EditorGuiSliderFloat("lifetime_max", &em.lifetimeMax, 0.05f, 15.0f);
    dirty |= EditorGuiSliderFloat("gravity_scale", &em.gravityScale, -2.0f, 3.0f);
    dirty |= EditorGuiSliderFloat("drag", &em.drag, 0.0f, 8.0f);
    dirty |= EditorGuiSliderFloat("restitution", &em.restitution, 0.0f, 1.0f);
    dirty |= EditorGuiSliderFloat("friction", &em.friction, 0.0f, 1.0f);
    dirty |= EditorGuiColorHex("color_start", em.colorStart);
    dirty |= EditorGuiColorHex("color_end", em.colorEnd);
    dirty |= EditorGuiSliderFloat(
        "hdr_emission", &em.emission, 0.0f, 150.0f,
        { false, true,
          "Linear HDR self-emission strength (values above ~10 trigger optical HDR Bloom halos).",
          "%.1f" });
    dirty |= EditorGuiSliderFloat("spin_speed", &em.spinSpeed, -20.0f, 20.0f);
    dirty |= EditorGuiCheckbox("collide_world", &em.collideWorld);
    dirty |= EditorGuiCheckbox("rest_on_floor", &em.restOnFloor);
    dirty |= EditorGuiCheckbox("affected_by_fields", &em.affectedByVectorFields);
    dirty |= EditorGuiCheckbox("self_lit", &em.selfLit);
    dirty |= EditorGuiCheckbox("additive_blend", &em.additiveBlend);
}

void SpawnEmitterItem(const ParticleUiContext &ctx, int32_t &selectedIdx, const float pos[3])
{
    EditorParticleEmitterDef em;
    ApplyEditorParticlePreset(EditorParticlePreset::FireEmbers, em);
    em.name = "emitter_" + std::to_string(ctx.runtime.particleEmitters.size());
    std::copy_n(pos, 3, em.origin);
    std::copy_n(ctx.pickNormal, 3, em.direction);
    em.cluster = ResolveEditorLightCluster(ctx.runtime.scene, em.origin);
    em.cluster = em.cluster == QR_CLUSTER_LIGHT_NO_CLUSTER ? 0u : em.cluster;
    ctx.runtime.particleEmitters.push_back(em);
    selectedIdx = static_cast<int32_t>(ctx.runtime.particleEmitters.size() - 1);
    ctx.runtime.particlesDirty = true;
}

void DrawEmitterQuickBurstButton(const ParticleUiContext &ctx, const EditorParticleEmitterDef &em)
{
    ImGui::BeginDisabled(ctx.instance == nullptr);
    if (ImGui::Button("Fire Single Burst Now", ImVec2(-1.0f, 0.0f)))
    {
        QrParticleBurstInfo burst = BuildEditorBurstQuakeUnits(em);
        EditorMetersParticleBurst(burst);
        qrSpawnParticleBurst(ctx.instance, &burst);
    }
    ImGui::EndDisabled();
}

void DrawEmittersTab(const ParticleUiContext &ctx, int32_t &selectedIdx, uint32_t pickCluster)
{
    const float placePos[3] = {
        ctx.hasPick ? ctx.pickPoint[0] : ctx.cameraPos[0] + ctx.viewForward[0] * 96.0f,
        ctx.hasPick ? ctx.pickPoint[1] : ctx.cameraPos[1] + ctx.viewForward[1] * 96.0f,
        ctx.hasPick ? ctx.pickPoint[2] : ctx.cameraPos[2] + ctx.viewForward[2] * 96.0f,
    };
    if (ImGui::Button("Add Emitter"))
    {
        SpawnEmitterItem(ctx, selectedIdx, placePos);
        ctx.gizmo.activeTarget = EditorApp::ParticleGizmoTarget::Emitter;
    }
    ImGui::SameLine();
    const bool validIdx = selectedIdx >= 0 && size_t(selectedIdx) < ctx.runtime.particleEmitters.size();
    ImGui::BeginDisabled(!validIdx);
    if (ImGui::Button("Delete Emitter"))
    {
        ctx.runtime.particleEmitters.erase(ctx.runtime.particleEmitters.begin() + selectedIdx);
        selectedIdx = std::min(selectedIdx, static_cast<int32_t>(ctx.runtime.particleEmitters.size()) - 1);
        ctx.runtime.particlesDirty = true;
    }
    ImGui::EndDisabled();

    for (size_t i = 0; i < ctx.runtime.particleEmitters.size(); i++)
    {
        const std::string itemLabel =
            std::to_string(i) + ": " + ctx.runtime.particleEmitters[i].name + "##Emitter" + std::to_string(i);
        if (ImGui::Selectable(itemLabel.c_str(), int32_t(i) == selectedIdx))
        {
            selectedIdx = static_cast<int32_t>(i);
            ctx.gizmo.activeTarget = EditorApp::ParticleGizmoTarget::Emitter;
        }
    }

    if (selectedIdx < 0 || size_t(selectedIdx) >= ctx.runtime.particleEmitters.size())
    {
        return;
    }

    EditorParticleEmitterDef &em = ctx.runtime.particleEmitters[size_t(selectedIdx)];
    bool dirty = EditStringInput("Name##EmName", em.name);
    int32_t presetIdx = static_cast<int32_t>(em.preset);
    static constexpr const char *kPresets[int32_t(EditorParticlePreset::Count)] = {
        "Custom", "ExplosionSparks", "MetalStrikeSparks", "LightningConcreteHit",
        "FireEmbers", "WindDust", "QuakeRetroShotgun", "QuakeRetroBlood"
    };
    if (EditorGuiCombo("apply_preset", &presetIdx, kPresets, int32_t(EditorParticlePreset::Count)))
    {
        ApplyEditorParticlePreset(static_cast<EditorParticlePreset>(presetIdx), em);
        dirty = true;
    }
    if (ctx.hasPick && ImGui::Button("Move Origin to Pick Point"))
    {
        std::copy_n(ctx.pickPoint, 3, em.origin);
        std::copy_n(ctx.pickNormal, 3, em.direction);
        em.cluster = pickCluster;
        dirty = true;
    }
    DrawEmitterQuickBurstButton(ctx, em);
    DrawEmitterTransformAndRates(em, dirty);
    DrawEmitterAppearanceAndPhysics(em, dirty);
    if (dirty)
    {
        ctx.runtime.particlesDirty = true;
    }
}

void DrawVectorFieldInspector(const ParticleUiContext &ctx, int32_t selectedIdx)
{
    if (selectedIdx < 0 || size_t(selectedIdx) >= ctx.runtime.vectorFields.size())
    {
        return;
    }

    EditorVectorFieldDef &vf = ctx.runtime.vectorFields[size_t(selectedIdx)];
    bool dirty = false;

    EditorGuiSectionHeader("Vector Field Properties");
    dirty |= EditStringInput("Name##VFieldName", vf.name);
    dirty |= EditorGuiCheckbox("enabled##VField", &vf.enabled);

    static constexpr const char *kKindNames[5] = {
        "LUT 3D (.fga)", "Curl Noise", "Vortex", "Point Force", "Directional"
    };
    int32_t kindIdx = static_cast<int32_t>(vf.kind);
    if (EditorGuiCombo("kind", &kindIdx, kKindNames, 5))
    {
        vf.kind = static_cast<QrVectorFieldKind>(kindIdx);
        dirty = true;
    }

    if (vf.kind == QR_VECTOR_FIELD_KIND_LUT_3D)
    {
        dirty |= EditStringInput(".fga file path", vf.fgaPath);
        if (ImGui::Button("Reload .fga Cache"))
        {
            ctx.runtime.vectorFieldCache.erase(vf.fgaPath);
            dirty = true;
        }
    }

    dirty |= EditorGuiVec3Input("center", vf.center, 4.0f);
    if (ctx.hasPick && ImGui::Button("Move Box to Pick Point"))
    {
        std::copy_n(ctx.pickPoint, 3, vf.center);
        dirty = true;
    }
    dirty |= EditorGuiVec3Input("half_extents", vf.halfExtents, 4.0f);
    dirty |= EditorGuiVec3Input("angles_deg", vf.anglesDeg, 1.0f);
    dirty |= EditorGuiVec3Input("axis", vf.axis, 0.05f);
    dirty |= EditorGuiSliderFloat("intensity", &vf.intensityUnits, 0.0f, 1600.0f,
                                  { false, true, nullptr, "%.1f" });
    dirty |= EditorGuiSliderFloat("max_speed", &vf.maxSpeedUnits, 10.0f, 1600.0f,
                                  { false, true, nullptr, "%.1f u/s" });
    dirty |= EditorGuiSliderFloat("tightness", &vf.tightness, 0.0f, 1.0f);
    dirty |= EditorGuiSliderFloat("edge_falloff", &vf.edgeFalloff, 0.0f, 1.0f);
    dirty |= EditorGuiSliderFloat("curl_frequency", &vf.curlFrequencyPerUnit, 0.001f, 0.25f,
                                  { false, true, nullptr, "%.3f /u" });
    dirty |= EditorGuiSliderFloat("curl_scroll_speed", &vf.curlScrollUnitsPerSec, -400.0f, 400.0f,
                                  { false, true, nullptr, "%.1f u/s" });
    if (dirty)
    {
        ctx.runtime.particlesDirty = true;
    }
}

void DrawVectorFieldsTab(const ParticleUiContext &ctx, int32_t &selectedIdx)
{
    const float placePos[3] = {
        ctx.hasPick ? ctx.pickPoint[0] : ctx.cameraPos[0] + ctx.viewForward[0] * 128.0f,
        ctx.hasPick ? ctx.pickPoint[1] : ctx.cameraPos[1] + ctx.viewForward[1] * 128.0f,
        ctx.hasPick ? ctx.pickPoint[2] : ctx.cameraPos[2] + ctx.viewForward[2] * 128.0f,
    };
    if (ImGui::Button("Add Vector Field"))
    {
        EditorVectorFieldDef vf;
        vf.name = "field_" + std::to_string(ctx.runtime.vectorFields.size());
        std::copy_n(placePos, 3, vf.center);
        ctx.runtime.vectorFields.push_back(vf);
        selectedIdx = static_cast<int32_t>(ctx.runtime.vectorFields.size() - 1);
        ctx.gizmo.activeTarget = EditorApp::ParticleGizmoTarget::VectorField;
        ctx.runtime.particlesDirty = true;
    }
    ImGui::SameLine();
    const bool validIdx = selectedIdx >= 0 && size_t(selectedIdx) < ctx.runtime.vectorFields.size();
    ImGui::BeginDisabled(!validIdx);
    if (ImGui::Button("Delete Vector Field"))
    {
        ctx.runtime.vectorFields.erase(ctx.runtime.vectorFields.begin() + selectedIdx);
        selectedIdx = std::min(selectedIdx, static_cast<int32_t>(ctx.runtime.vectorFields.size()) - 1);
        ctx.runtime.particlesDirty = true;
    }
    ImGui::EndDisabled();

    for (size_t i = 0; i < ctx.runtime.vectorFields.size(); i++)
    {
        const std::string itemLabel =
            std::to_string(i) + ": " + ctx.runtime.vectorFields[i].name + "##VField" + std::to_string(i);
        if (ImGui::Selectable(itemLabel.c_str(), int32_t(i) == selectedIdx))
        {
            selectedIdx = static_cast<int32_t>(i);
            ctx.gizmo.activeTarget = EditorApp::ParticleGizmoTarget::VectorField;
        }
    }

    DrawVectorFieldInspector(ctx, selectedIdx);
}

} // namespace

void EditorApp::HandleParticleModePick(const EditorFacePick &facePick)
{
    if (facePick.surface < 0 || size_t(facePick.surface) >= m_scene.surfaces.size())
    {
        m_hasPickPoint = false;
        PrintMessage("pick: nothing under the cursor", this);
        return;
    }

    const EditorSurface &surf = m_scene.surfaces[size_t(facePick.surface)];
    if (facePick.face >= 0 && size_t(facePick.face) < surf.faces.size())
    {
        std::copy_n(surf.faces[size_t(facePick.face)].normal, 3, m_pickNormal);
    }
    else
    {
        m_pickNormal[0] = 0.0f;
        m_pickNormal[1] = 0.0f;
        m_pickNormal[2] = 1.0f;
    }

    m_hasPickPoint = true;
    m_pickCluster = facePick.cluster;
    std::copy_n(facePick.point, 3, m_pickPoint);
    if (m_particleTab == 0)
    {
        m_particleGizmo.activeTarget = ParticleGizmoTarget::BurstAim;
    }

    if (m_burstTestClickArmed && m_particleTab == 0 && m_instance != nullptr)
    {
        m_runtime.SpawnPresetBurst(m_instance, m_burstTestPreset, facePick.point, m_pickNormal,
                                   m_burstTestScale, m_burstTestEmissionScale);
    }
}

void EditorApp::UpdateParticleGizmoHover(float cursorX, float cursorY, bool uiWantsMouse)
{
    m_particleGizmo.hoverHandle = ParticleGizmoHandle::None;
    m_particleGizmo.hoverAxis = -1;
    if (m_editorMode != EditorMode::Particles || !m_showPanel)
    {
        return;
    }
    if (m_particleGizmo.dragging)
    {
        m_particleGizmo.hoverHandle = m_particleGizmo.dragHandle;
        m_particleGizmo.hoverAxis = m_particleGizmo.dragAxis;
        return;
    }
    if (uiWantsMouse)
    {
        return;
    }

    ParticleGizmoContext gCtx{
        m_runtime, m_selectedParticleEmitter, m_selectedVectorField, m_particleTab,
        m_hasPickPoint, m_pickPoint, m_pickNormal, m_particleGizmo
    };
    const EditorCameraBasis cam = CameraBasis();
    const ParticleGizmoBasis basis = BuildParticleGizmoBasis(gCtx, cam);
    const ParticleGizmoHit hit = HitTestParticleGizmo(cam, basis, m_particleGizmo, ImVec2(cursorX, cursorY));
    m_particleGizmo.hoverHandle = hit.handle;
    m_particleGizmo.hoverAxis = hit.axis;
}

bool EditorApp::BeginParticleGizmoDrag(float cursorX, float cursorY)
{
    if (m_editorMode != EditorMode::Particles || !m_showPanel)
    {
        return false;
    }
    ParticleGizmoContext gCtx{
        m_runtime, m_selectedParticleEmitter, m_selectedVectorField, m_particleTab,
        m_hasPickPoint, m_pickPoint, m_pickNormal, m_particleGizmo
    };
    const EditorCameraBasis cam = CameraBasis();
    const ParticleGizmoBasis basis = BuildParticleGizmoBasis(gCtx, cam);
    const ParticleGizmoHit hit = HitTestParticleGizmo(cam, basis, m_particleGizmo, ImVec2(cursorX, cursorY));
    if (hit.handle == ParticleGizmoHandle::None)
    {
        return false;
    }

    m_particleGizmo.dragging = true;
    m_particleGizmo.dragHandle = hit.handle;
    m_particleGizmo.dragAxis = hit.axis;
    m_particleGizmo.startCursor[0] = cursorX;
    m_particleGizmo.startCursor[1] = cursorY;
    m_particleGizmo.screenTangent[0] = hit.tangent[0];
    m_particleGizmo.screenTangent[1] = hit.tangent[1];
    m_particleGizmo.pixelsPerUnit = hit.pixelsPerUnit;
    std::copy_n(hit.worldAxis, 3, m_particleGizmo.worldAxis);
    BackupOrRestoreGizmoTarget(gCtx, false);
    return true;
}

void EditorApp::UpdateParticleGizmoDrag(float cursorX, float cursorY)
{
    if (!m_particleGizmo.dragging || m_editorMode != EditorMode::Particles)
    {
        return;
    }

    const float travelled =
        (cursorX - m_particleGizmo.startCursor[0]) * m_particleGizmo.screenTangent[0] +
        (cursorY - m_particleGizmo.startCursor[1]) * m_particleGizmo.screenTangent[1];
    const float deltaUnits =
        m_particleGizmo.pixelsPerUnit > 1e-4f ? std::round(travelled / m_particleGizmo.pixelsPerUnit) : 0.0f;
    const float deltaDeg = travelled * 0.65f;

    ParticleGizmoContext gCtx{
        m_runtime, m_selectedParticleEmitter, m_selectedVectorField, m_particleTab,
        m_hasPickPoint, m_pickPoint, m_pickNormal, m_particleGizmo
    };
    ApplyParticleGizmoDragDelta(gCtx, deltaUnits, deltaDeg);
}

void EditorApp::FinishParticleGizmoDrag(bool kept)
{
    if (!m_particleGizmo.dragging)
    {
        return;
    }
    if (!kept)
    {
        ParticleGizmoContext gCtx{
            m_runtime, m_selectedParticleEmitter, m_selectedVectorField, m_particleTab,
            m_hasPickPoint, m_pickPoint, m_pickNormal, m_particleGizmo
        };
        BackupOrRestoreGizmoTarget(gCtx, true);
    }
    m_particleGizmo.dragging = false;
    m_particleGizmo.dragHandle = ParticleGizmoHandle::None;
    m_particleGizmo.dragAxis = -1;
}

bool EditorApp::TryPickParticleViewportItem(float cursorX, float cursorY)
{
    if (m_editorMode != EditorMode::Particles || !m_showPanel)
    {
        return false;
    }

    const EditorCameraBasis cam = CameraBasis();
    float bestDist = kGizmoPickRadiusPx;
    ParticleGizmoTarget bestTarget = ParticleGizmoTarget::None;
    int32_t bestIdx = -1;

    for (size_t i = 0; i < m_runtime.particleEmitters.size(); i++)
    {
        float sx = 0.0f, sy = 0.0f;
        if (ProjectCamPoint(cam, m_runtime.particleEmitters[i].origin, sx, sy))
        {
            const float d = std::hypot(cursorX - sx, cursorY - sy);
            if (d <= bestDist)
            {
                bestDist = d;
                bestTarget = ParticleGizmoTarget::Emitter;
                bestIdx = static_cast<int32_t>(i);
            }
        }
    }
    for (size_t i = 0; i < m_runtime.vectorFields.size(); i++)
    {
        float sx = 0.0f, sy = 0.0f;
        if (ProjectCamPoint(cam, m_runtime.vectorFields[i].center, sx, sy))
        {
            const float d = std::hypot(cursorX - sx, cursorY - sy);
            if (d <= bestDist)
            {
                bestDist = d;
                bestTarget = ParticleGizmoTarget::VectorField;
                bestIdx = static_cast<int32_t>(i);
            }
        }
    }

    if (bestIdx < 0)
    {
        return false;
    }
    m_particleGizmo.activeTarget = bestTarget;
    if (bestTarget == ParticleGizmoTarget::Emitter)
    {
        m_selectedParticleEmitter = bestIdx;
    }
    else if (bestTarget == ParticleGizmoTarget::VectorField)
    {
        m_selectedVectorField = bestIdx;
    }
    return true;
}

void EditorApp::DrawParticleEditorViewportExtras(ImDrawList *drawList, const EditorCameraBasis &cam,
                                                 bool showWorldExtras)
{
    if (!showWorldExtras || m_editorMode != EditorMode::Particles || drawList == nullptr)
    {
        return;
    }

    for (size_t i = 0; i < m_runtime.particleEmitters.size(); i++)
    {
        const bool selected =
            m_particleGizmo.activeTarget == ParticleGizmoTarget::Emitter &&
            int32_t(i) == m_selectedParticleEmitter;
        DrawEmitterGizmo(drawList, cam, m_runtime.particleEmitters[i], selected);
    }

    for (size_t i = 0; i < m_runtime.vectorFields.size(); i++)
    {
        const bool selected =
            m_particleGizmo.activeTarget == ParticleGizmoTarget::VectorField &&
            int32_t(i) == m_selectedVectorField;
        DrawVectorFieldGizmo(drawList, cam, m_runtime.vectorFields[i], selected);
    }

    ParticleGizmoContext gCtx{
        m_runtime, m_selectedParticleEmitter, m_selectedVectorField, m_particleTab,
        m_hasPickPoint, m_pickPoint, m_pickNormal, m_particleGizmo
    };
    const ParticleGizmoBasis basis = BuildParticleGizmoBasis(gCtx, cam);
    DrawParticleSelectableBadges(drawList, cam, gCtx);
    DrawParticleGizmoManipulator(drawList, cam, basis, m_particleGizmo);
}

void EditorApp::DrawParticleEditorPanel()
{
    if (!EditorGuiBeginPanel("PARTICLES & VFX", float(m_clientWidth), float(m_clientHeight)))
    {
        EditorGuiEndPanel();
        return;
    }
    m_panelVisible = true;
    m_panelLeft = ImGui::GetWindowPos().x;

    DrawPanelActionBar(false);
    static constexpr const char *kTabs[] = { "Burst Tester", "Emitters", "Vector Fields" };
    bool resetClicked = false;
    EditorGuiTabs(kTabs, 3, &m_particleTab, true, &resetClicked,
                  "Restore particle emitters & vector fields to session baseline");
    if (resetClicked)
    {
        m_runtime.particleEmitters = m_baselineParticleEmitters;
        m_runtime.vectorFields = m_baselineVectorFields;
        m_selectedParticleEmitter = -1;
        m_selectedVectorField = -1;
        m_runtime.particlesDirty = true;
    }

    float side[3] = {}, up[3] = {}, fwd[3] = {};
    ViewBasis(side, up, fwd);
    const ParticleUiContext ctx{
        m_runtime, m_baselineSettings, m_instance, m_cameraPosition, fwd, m_hasPickPoint,
        m_pickPoint, m_pickNormal, m_particleGizmo
    };
    ParticleGizmoContext gCtx{
        m_runtime, m_selectedParticleEmitter, m_selectedVectorField, m_particleTab,
        m_hasPickPoint, m_pickPoint, m_pickNormal, m_particleGizmo
    };
    DrawParticleGizmoToolbar(gCtx);

    EditorGuiBeginScroll("##ParticleScroll");
    if (m_particleTab == 0)
    {
        DrawBurstTesterTab(ctx, m_burstTestClickArmed, m_burstTestPreset, m_burstTestScale,
                           m_burstTestEmissionScale);
    }
    else if (m_particleTab == 1)
    {
        DrawEmittersTab(ctx, m_selectedParticleEmitter, m_pickCluster);
    }
    else
    {
        DrawVectorFieldsTab(ctx, m_selectedVectorField);
    }
    EditorGuiEndScroll();
    EditorGuiEndPanel();
}
