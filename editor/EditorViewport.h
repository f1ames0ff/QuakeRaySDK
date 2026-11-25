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

#pragma once

#include <cstdint>
#include <vector>

#include <qray/qray.h>

#include "EditorMaterial.h"
#include "Scene.h"

struct ImDrawList;

// Camera projection basis in scene units matching qr_editor.c's clipped-edge projector.
struct EditorCameraBasis
{
    float origin[3] = {};
    float forward[3] = {};
    float right[3] = {};
    float up[3] = {};
    float tanHalfFovX = 1.0f;
    float tanHalfFovY = 1.0f;
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
};

// Result of casting a pick ray against whole polygonal faces in an EditorScene.
struct EditorFacePick
{
    int32_t surface = -1;
    int32_t face = -1;
    float distance = 0.0f;
    float point[3] = {};
    uint32_t cluster = 0;
};

// One clipped UV sub-polygon representing a single glowing repetition of an emissive texture.
struct EditorClippedGlowPoly
{
    QrFloat2D uvVerts[MAX_TEXTURED_AREA_LIGHT_VERTS] = {};
    int numVerts = 0;
    float area = 0.0f;
    float center[3] = {};
};

// Clips world-space segment [a, b] against the camera near plane and projects it to screen pixels.
bool ProjectClippedEdge(const EditorCameraBasis &cam, const float a[3], const float b[3],
                        float outSeg[4]);

// Draws the perimeter of a whole polygon face nudged toward the camera, clipping behind-camera edges.
void EmitClippedPolygonOutline(ImDrawList *drawList, const EditorCameraBasis &cam,
                               const EditorSurfaceFace &face, uint32_t color, float thickness);

// Picks the nearest surface polygon along the ray (whole face, without backface culling).
EditorFacePick PickSceneFace(const EditorScene &scene, const float origin[3],
                             const float direction[3]);

// Clips a fitted surface face against all active glow-box repetitions of an emissive texture.
// Returns true if glow clipping handled the face (outPolys may be empty if no UV falls in glow).
// Returns false if the caller should fall back to the unclipped whole-face light (> 64 tiles).
bool ClipFaceUvToGlowPolygons(const std::vector<QrFloat2D> &surfUv, const float A[3],
                               const float B[3], const float C[3],
                               const EditorMaterialEmission &emission,
                               std::vector<EditorClippedGlowPoly> &outPolys);
