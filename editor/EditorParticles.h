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
#include <string>
#include <string_view>
#include <vector>

#include <qray/qray.h>

class EditorAssetSource;

enum class EditorParticlePreset : uint32_t
{
    Custom = 0,
    ExplosionSparks,
    MetalStrikeSparks,
    LightningConcreteHit,
    FireEmbers,
    WindDust,
    QuakeRetroShotgun,
    QuakeRetroBlood,
    Count,
};

struct EditorParticleEmitterDef
{
    std::string name = "emitter";
    bool enabled = true;
    EditorParticlePreset preset = EditorParticlePreset::FireEmbers;
    float origin[3] = {};
    float direction[3] = { 0.0f, 0.0f, 1.0f };
    float coneHalfAngleDeg = 35.0f;
    float spawnRate = 45.0f;
    uint32_t burstCount = 32u;
    float originJitterUnits = 4.0f;
    float speedMinUnits = 80.0f;
    float speedMaxUnits = 200.0f;
    float sizeStartMinUnits = 2.0f;
    float sizeStartMaxUnits = 4.0f;
    float sizeEndScale = 0.2f;
    float velocityStretch = 0.025f;
    float lifetimeMin = 1.0f;
    float lifetimeMax = 2.2f;
    float gravityScale = 0.25f;
    float drag = 0.6f;
    float restitution = 0.5f;
    float friction = 0.25f;
    float colorStart[4] = { 1.0f, 0.62f, 0.18f, 1.0f };
    float colorEnd[4] = { 1.0f, 0.16f, 0.02f, 0.0f };
    float emission = 8.0f;
    float spinSpeed = 2.5f;
    QrGpuParticleRenderMode renderMode = QR_GPU_PARTICLE_RENDER_SPARK;
    bool collideWorld = true;
    bool restOnFloor = false;
    bool affectedByVectorFields = true;
    bool selfLit = true;
    bool additiveBlend = true;
    std::string textureName;
    uint32_t cluster = 0u;
};

struct EditorVectorFieldDef
{
    std::string name = "field";
    bool enabled = true;
    QrVectorFieldKind kind = QR_VECTOR_FIELD_KIND_CURL_NOISE;
    std::string fgaPath;
    float center[3] = {};
    float halfExtents[3] = { 160.0f, 160.0f, 160.0f };
    float anglesDeg[3] = {};
    float axis[3] = { 0.0f, 0.0f, 1.0f };
    float intensityUnits = 240.0f;
    float maxSpeedUnits = 320.0f;
    float tightness = 0.12f;
    float edgeFalloff = 0.20f;
    float curlFrequencyPerUnit = 0.025f;
    float curlScrollUnitsPerSec = 40.0f;
};

struct EditorFgaGrid
{
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t depth = 0u;
    float boundsMin[3] = {};
    float boundsMax[3] = {};
    std::vector<float> vectorsXYZ;
};

const char *EditorParticlePresetLabel(EditorParticlePreset preset);
const char *EditorParticleRenderModeLabel(QrGpuParticleRenderMode mode);
const char *EditorVectorFieldKindLabel(QrVectorFieldKind kind);

void ApplyEditorParticlePreset(EditorParticlePreset preset, EditorParticleEmitterDef &ioDef);

QrParticleBurstInfo BuildEditorBurstQuakeUnits(const EditorParticleEmitterDef &def,
                                               QrMaterial material = QR_NO_MATERIAL);

void BuildEditorPresetBurstsQuakeUnits(EditorParticlePreset preset, const float origin[3],
                                       const float normal[3], uint32_t cluster, float scale,
                                       std::vector<QrParticleBurstInfo> &outBursts,
                                       float emissionScale = 1.0f);

QrVectorFieldVolume BuildEditorVectorFieldVolumeQuakeUnits(const EditorVectorFieldDef &def,
                                                           QrVectorField fieldHandle);

void ComputeEditorVectorFieldBoxCorners(const EditorVectorFieldDef &def, float outCorners[8][3]);
void ComputeEditorVectorFieldFlowAxis(const EditorVectorFieldDef &def, float outWorldAxis[3]);
void BuildEditorEulerRotation3x3(const float anglesDeg[3], float outRot[3][3]);
void ExtractEditorEulerAnglesDeg3x3(const float rot[3][3], float outAnglesDeg[3]);
void RotateEditorVectorAroundAxis(const float vec[3], const float unitAxis[3],
                                  float angleDegrees, float outVec[3]);
void RotateEditorEulerAnglesAroundAxis(const float startAnglesDeg[3], const float unitAxis[3],
                                       float angleDegrees, float outAnglesDeg[3]);

bool ParseEditorFgaMemory(std::string_view text, EditorFgaGrid &outGrid, std::string &error);
bool LoadEditorFgaFile(const std::string &path, const EditorAssetSource *assets,
                       EditorFgaGrid &outGrid, std::string &error);

void ReadEditorLevelParticlesYaml(std::string_view yamlText, const std::string &levelKey,
                                  std::vector<EditorParticleEmitterDef> &outEmitters,
                                  std::vector<EditorVectorFieldDef> &outFields);

std::string RenderEditorLevelParticlesYaml(const std::vector<EditorParticleEmitterDef> &emitters,
                                           const std::vector<EditorVectorFieldDef> &fields);

void MergeEditorLevelParticlesYaml(std::string &ioYamlText, const std::string &levelKey,
                                   const std::vector<EditorParticleEmitterDef> &emitters,
                                   const std::vector<EditorVectorFieldDef> &fields);
