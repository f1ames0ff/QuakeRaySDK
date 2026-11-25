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

// Result of casting a pick ray against whole polygonal faces in an EditorScene.
struct EditorFacePick
{
    int32_t surface = -1;
    int32_t face = -1;
    float distance = 0.0f;
    float point[3] = {};
    uint32_t cluster = 0;
};

// Clips world-space segment [a, b] against the camera near plane and projects it to screen pixels.
bool ProjectClippedEdge(const EditorCameraBasis &cam, const float a[3], const float b[3],
                        float outSeg[4]);

// Draws the perimeter of a whole polygon face nudged toward the camera, clipping behind-camera edges.
void EmitClippedPolygonOutline(ImDrawList *drawList, const EditorCameraBasis &cam,
                               const EditorSurfaceFace &face, uint32_t color, float thickness);

// Draws the 12-edge 3D bounding-box outline expanded slightly for visibility and clipped to the near plane.
void EmitClippedAabbOutline(ImDrawList *drawList, const EditorCameraBasis &cam,
                            const float boundsMin[3], const float boundsMax[3], uint32_t color,
                            float thickness);

// Picks the nearest enabled surface polygon along the ray (whole face, without backface culling).
EditorFacePick PickSceneFace(const EditorScene &scene, const float origin[3],
                             const float direction[3],
                             const EditorMaterialTable *materials = nullptr);
