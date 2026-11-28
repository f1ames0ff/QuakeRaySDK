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

#include <vector>

#include <qray/qray.h>

#include "Scene.h"

// The editor authors its world in Quake units: a map is a BSP29 file whose vertices are counted in
// them and every distance the panel shows -- a light radius, a gather reach, a camera speed -- is
// one. The SDK speaks meters, so a value becomes meters here, at the one boundary it crosses, and
// nowhere else. Each conversion is the one the game's own host adapter applies to the same block
// (Quake/qr_spatial_adapter.cpp), which is what keeps a level lit in the editor the way the game
// that ships it lights it.

// A length: Quake units to meters.
float EditorMetersLength(float units);

// An area: square Quake units to square meters.
float EditorMetersArea(float squareUnits);

// A rate stated per Quake unit -- fog density, scattering -- to a rate per meter.
float EditorMetersPerUnitRate(float perUnit);

void EditorMetersPoint(QrFloat3D &point);

// A transmittance stated over one Quake unit to a transmittance over one meter.
void EditorMetersTransmittance(QrFloat3D &color);

// The vertices of an upload, retargeted at a converted copy the caller keeps alive for the call.
void EditorMetersGeometry(QrGeometryUploadInfo &upload, std::vector<QrVertex> &scratch);

// The cluster bounds of an upload. The editor passes no world light faces, so the face lane of the
// block keeps whatever the caller left in it.
void EditorMetersWorldLights(QrWorldLightsUploadInfo &upload, std::vector<QrFloat3D> &mins,
                             std::vector<QrFloat3D> &maxs);

void EditorMetersSphericalLight(QrSphericalLightUploadInfo &upload);
void EditorMetersSpotLight(QrSpotLightUploadInfo &upload);
void EditorMetersClusterLightSource(QrClusterLightSource &source);
void EditorMetersAreaLight(QrTexturedAreaLightUploadInfo &upload);
void EditorMetersParticleBurst(QrParticleBurstInfo &burst);
void EditorMetersParticleEmitter(QrParticleEmitterInfo &emitter);
void EditorMetersVectorFieldVolume(QrVectorFieldVolume &volume);
void EditorMetersFgaVectors(std::vector<float> &vectorsXYZ);

// The frame's own block: the view translation, the camera range, and every parameter block the
// draw info points at are converted into a copy of the frame, so the caller's blocks stay in the
// units the panel authors them in.
QrResult EditorDrawFrame(QrInstance instance, const QrDrawFrameInfo *drawInfo);
