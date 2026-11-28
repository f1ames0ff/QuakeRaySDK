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

#include <qray/qray.h>

#include "EditorParticles.h"
#include "EditorSpatial.h"
#include "RHI/RhiGpuParticlePass.h"
#include "SpatialFrame.h"

#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

void Check(bool cond, const char *msg)
{
    if (!cond)
    {
        throw std::runtime_error(msg);
    }
}

bool Near(float a, float b, float eps = 1e-4f)
{
    return std::abs(a - b) <= eps;
}

float HalfToFloat(uint16_t bits)
{
    const uint32_t exp = (bits >> 10u) & 31u;
    float value = 0.0f;
    if (exp == 0u)
    {
        value = std::ldexp(float(bits & 1023u), -24);
    }
    else if (exp == 31u)
    {
        value = std::bit_cast<float>(uint32_t(0x7F800000u | ((bits & 1023u) << 13u)));
    }
    else
    {
        value = std::ldexp(1.0f + float(bits & 1023u) / 1024.0f, int(exp) - 15);
    }
    return (bits & 0x8000u) != 0u ? -value : value;
}

void TestFgaParserAndDirectoryGuard()
{
    const char *kValidFga =
        "2,2,2,\n"
        "-100,-100,-100,\n"
        "100,100,100,\n"
        "10,20,30, 10,20,30, 10,20,30, 10,20,30,\n"
        "10,20,30, 10,20,30, 10,20,30, 10,20,30,\n";

    EditorFgaGrid grid;
    std::string error;
    Check(ParseEditorFgaMemory(kValidFga, grid, error), error.c_str());
    Check(grid.width == 2u && grid.height == 2u && grid.depth == 2u, "FGA dims mismatch");
    Check(grid.vectorsXYZ.size() == 24u, "FGA payload size mismatch");
    Check(Near(grid.vectorsXYZ[0], 10.0f) && Near(grid.vectorsXYZ[1], 20.0f) && Near(grid.vectorsXYZ[2], 30.0f),
          "FGA first vector mismatch");

    EditorFgaGrid bad;
    Check(!ParseEditorFgaMemory("0,2,2,-1,-1,-1,1,1,1", bad, error), "Expected failure for zero dim");
    Check(!ParseEditorFgaMemory("2,2,2,-1,-1,-1,1,1,1,1,2,3", bad, error), "Expected failure for short payload");
    Check(!ParseEditorFgaMemory("2,2,2,-1,-1,-1,1,1,NaN,0,0,0", bad, error), "Expected failure for NaN");

    const std::string dirPath = std::filesystem::temp_directory_path().string();
    Check(!LoadEditorFgaFile(dirPath, nullptr, bad, error), "Directory path must be cleanly rejected");

    const std::filesystem::path tmpFile =
        std::filesystem::temp_directory_path() / "quakeraysdk_test_field.fga";
    {
        std::ofstream out(tmpFile, std::ios::binary);
        out << kValidFga;
    }
    Check(LoadEditorFgaFile(tmpFile.string(), nullptr, grid, error), error.c_str());
    std::error_code ec;
    std::filesystem::remove(tmpFile, ec);
}

void TestBrickResampleAndSpatialFrame()
{
    std::vector<float> vecs(2u * 2u * 2u * 3u, 0.0f);
    for (size_t i = 0; i < 8u; i++)
    {
        vecs[i * 3u + 0u] = 4.0f;
        vecs[i * 3u + 1u] = 0.0f;
        vecs[i * 3u + 2u] = 0.0f;
    }

    const QrVectorFieldCreateInfo src = { 2u, 2u, 2u, vecs.data() };
    qray::SpatialFrame frame;
    QrSpatialConvention conv = {
        sizeof(QrSpatialConvention),
        QR_SPATIAL_CONVENTION_VERSION,
        QR_AXIS_POSITIVE_Y,
        QR_AXIS_NEGATIVE_X,
        QR_AXIS_POSITIVE_Z,
        QR_TRUE,
        0,
        {},
    };
    Check(frame.Configure(conv), "SpatialFrame::Configure failed");

    std::vector<uint16_t> brick;
    qray::RhiGpuParticlePass::ResampleVectorFieldToBrick(src, frame, brick);
    Check(brick.size() == 32u * 32u * 32u * 4u, "Brick half-float buffer size mismatch");

    // Check interior voxel and clamped 1-voxel border preserve rotated vector (4,0,0) -> (0,-4,0).
    for (const uint32_t sampleIdx : { 0u, 16u * 32u * 32u + 16u * 32u + 16u, 32u * 32u * 32u - 1u })
    {
        const float vx = HalfToFloat(brick[sampleIdx * 4u + 0u]);
        const float vy = HalfToFloat(brick[sampleIdx * 4u + 1u]);
        const float vz = HalfToFloat(brick[sampleIdx * 4u + 2u]);
        Check(Near(vx, 0.0f, 1e-2f) && Near(vy, -4.0f, 1e-2f) && Near(vz, 0.0f, 1e-2f),
              "Brick resampled vector rotation/clamping mismatch");
    }

    QrParticleBurstInfo burst = {};
    burst.origin = { { 1.0f, 2.0f, 3.0f } };
    burst.direction = { { 1.0f, 0.0f, 0.0f } };
    const QrParticleBurstInfo convBurst = frame.ParticleBurst(burst);
    Check(Near(convBurst.origin.data[0], 2.0f) &&
              Near(convBurst.origin.data[1], -1.0f) &&
              Near(convBurst.origin.data[2], 3.0f),
          "SpatialFrame::ParticleBurst origin mapping mismatch");
    Check(Near(convBurst.direction.data[0], 0.0f) &&
              Near(convBurst.direction.data[1], -1.0f) &&
              Near(convBurst.direction.data[2], 0.0f),
          "SpatialFrame::ParticleBurst direction mapping mismatch");
}

void TestEditorSpatialScaling()
{
    QrParticleEmitterInfo em = {};
    em.spawnRate = 25.0f;
    em.burst.origin = { { 80.0f, -40.0f, 120.0f } };
    em.burst.originJitterRadius = 20.0f;
    em.burst.speedMin = 40.0f;
    em.burst.speedMax = 200.0f;
    em.burst.sizeStartMin = 4.0f;
    em.burst.sizeStartMax = 8.0f;
    EditorMetersParticleEmitter(em);

    Check(Near(em.burst.origin.data[0], 2.0f) &&
              Near(em.burst.origin.data[1], -1.0f) &&
              Near(em.burst.origin.data[2], 3.0f),
          "EditorMetersParticleEmitter origin scale mismatch");
    Check(Near(em.burst.originJitterRadius, 0.5f) &&
              Near(em.burst.speedMin, 1.0f) &&
              Near(em.burst.speedMax, 5.0f) &&
              Near(em.burst.sizeStartMin, 0.1f) &&
              Near(em.burst.sizeStartMax, 0.2f),
          "EditorMetersParticleEmitter speed/size scale mismatch");

    EditorVectorFieldDef vfDef;
    vfDef.kind = QR_VECTOR_FIELD_KIND_CURL_NOISE;
    vfDef.center[0] = 40.0f;
    vfDef.halfExtents[0] = 80.0f;
    vfDef.intensityUnits = 160.0f;
    vfDef.maxSpeedUnits = 320.0f;
    vfDef.curlFrequencyPerUnit = 0.025f;
    vfDef.curlScrollUnitsPerSec = 40.0f;
    QrVectorFieldVolume vol = BuildEditorVectorFieldVolumeQuakeUnits(vfDef, QR_EMPTY_VECTOR_FIELD);
    EditorMetersVectorFieldVolume(vol);

    Check(Near(vol.worldFromLocal.matrix[0][0], 2.0f) &&
              Near(vol.worldFromLocal.matrix[0][3], 1.0f) &&
              Near(vol.intensity, 4.0f) &&
              Near(vol.maxSpeed, 8.0f) &&
              Near(vol.curlFrequency, 1.0f) &&
              Near(vol.curlScrollSpeed, 1.0f),
          "EditorMetersVectorFieldVolume scale mismatch");
}

void TestPresetsAndYamlRoundTrip()
{
    const float origin[3] = { 10.0f, 20.0f, 30.0f };
    const float normal[3] = { 0.0f, 0.0f, 1.0f };
    std::vector<QrParticleBurstInfo> bursts;

    BuildEditorPresetBurstsQuakeUnits(EditorParticlePreset::LightningConcreteHit, origin, normal, 7u, 1.0f, bursts);
    Check(bursts.size() == 3u, "LightningConcreteHit must emit 3 layers (debris, arcs, dust)");
    Check(bursts[0].renderMode == QR_GPU_PARTICLE_RENDER_DEBRIS_3D &&
              bursts[1].renderMode == QR_GPU_PARTICLE_RENDER_SPARK &&
              bursts[2].renderMode == QR_GPU_PARTICLE_RENDER_BILLBOARD,
          "LightningConcreteHit layer render modes mismatch");

    BuildEditorPresetBurstsQuakeUnits(EditorParticlePreset::QuakeRetroShotgun, origin, normal, 3u, 1.0f, bursts);
    Check(bursts.size() == 1u &&
              bursts[0].renderMode == QR_GPU_PARTICLE_RENDER_RETRO_SQUARE &&
              (bursts[0].flags & QR_GPU_PARTICLE_FLAG_COLLIDE_WORLD_BIT) != 0u &&
              (bursts[0].flags & QR_GPU_PARTICLE_FLAG_REST_ON_FLOOR_BIT) != 0u,
          "QuakeRetroShotgun preset flags/mode mismatch");

    BuildEditorPresetBurstsQuakeUnits(EditorParticlePreset::QuakeRetroBlood, origin, normal, 4u, 1.0f, bursts);
    Check(bursts.size() == 1u && bursts[0].renderMode == QR_GPU_PARTICLE_RENDER_RETRO_SQUARE,
          "QuakeRetroBlood mode mismatch");

    const std::string baseYaml =
        "q1dm4:\n"
        "  sky:\n"
        "    sun_elevation: 45\n"
        "  fog:\n"
        "    density: 0.002\n"
        "  lights:\n"
        "    - origin: [0, 0, 128]\n"
        "start:\n"
        "  fog:\n"
        "    density: 0\n";

    std::string merged = baseYaml;
    EditorParticleEmitterDef em;
    ApplyEditorParticlePreset(EditorParticlePreset::FireEmbers, em);
    em.name = "torch_sparks";
    em.origin[0] = 64.0f;
    em.origin[1] = -32.0f;
    em.origin[2] = 96.0f;

    EditorVectorFieldDef vf;
    vf.name = "wind_lut";
    vf.kind = QR_VECTOR_FIELD_KIND_LUT_3D;
    vf.fgaPath = "vfx/wind.fga";
    vf.center[2] = 128.0f;

    MergeEditorLevelParticlesYaml(merged, "q1dm4", { em }, { vf });
    Check(merged.find("sun_elevation: 45") != std::string::npos &&
              merged.find("start:\n  fog:\n    density: 0") != std::string::npos,
          "MergeEditorLevelParticlesYaml altered unrelated YAML blocks");

    std::vector<EditorParticleEmitterDef> readEmitters;
    std::vector<EditorVectorFieldDef> readFields;
    ReadEditorLevelParticlesYaml(merged, "q1dm4", readEmitters, readFields);
    Check(readEmitters.size() == 1u && readFields.size() == 1u, "YAML particle/field reload count mismatch");
    Check(readEmitters[0].name == "torch_sparks" &&
              Near(readEmitters[0].origin[0], 64.0f) &&
              Near(readEmitters[0].spawnRate, em.spawnRate),
          "YAML emitter round-trip field mismatch");
    Check(readFields[0].name == "wind_lut" &&
              readFields[0].kind == QR_VECTOR_FIELD_KIND_LUT_3D &&
              readFields[0].fgaPath == "vfx/wind.fga",
          "YAML vector field round-trip mismatch");

    MergeEditorLevelParticlesYaml(merged, "q1dm4", {}, {});
    Check(merged == baseYaml, "Clearing particles/fields did not restore exact original YAML");
}

void TestPublicApiValidation()
{
    const QrParticleBurstInfo burst = {};
    const QrVectorFieldCreateInfo vf = {};
    QrVectorField outField = QR_EMPTY_VECTOR_FIELD;

    Check(qrSpawnParticleBurst(nullptr, &burst) == QR_WRONG_INSTANCE, "Expected QR_WRONG_INSTANCE");
    Check(qrUploadParticleEmitters(nullptr, 0u, nullptr) == QR_WRONG_INSTANCE, "Expected QR_WRONG_INSTANCE");
    Check(qrResetGpuParticles(nullptr) == QR_WRONG_INSTANCE, "Expected QR_WRONG_INSTANCE");
    Check(qrCreateVectorField(nullptr, &vf, &outField) == QR_WRONG_INSTANCE, "Expected QR_WRONG_INSTANCE");
    Check(qrDestroyVectorField(nullptr, 1u) == QR_WRONG_INSTANCE, "Expected QR_WRONG_INSTANCE");
    Check(qrSetVectorFieldVolumes(nullptr, 0u, nullptr) == QR_WRONG_INSTANCE, "Expected QR_WRONG_INSTANCE");
}

void TestRotationMathAndVectorFieldAxis()
{
    const float eulerIn[3] = { 25.0f, -40.0f, 115.0f };
    float rotA[3][3] = {};
    BuildEditorEulerRotation3x3(eulerIn, rotA);
    float eulerOut[3] = {};
    ExtractEditorEulerAnglesDeg3x3(rotA, eulerOut);
    float rotB[3][3] = {};
    BuildEditorEulerRotation3x3(eulerOut, rotB);
    for (int r = 0; r < 3; r++)
    {
        for (int c = 0; c < 3; c++)
        {
            Check(Near(rotA[r][c], rotB[r][c], 1e-3f), "Euler 3x3 extraction round-trip mismatch");
        }
    }

    const float vecIn[3] = { 1.0f, 0.0f, 0.0f };
    const float zAxis[3] = { 0.0f, 0.0f, 1.0f };
    float vecRot[3] = {};
    RotateEditorVectorAroundAxis(vecIn, zAxis, 90.0f, vecRot);
    Check(Near(vecRot[0], 0.0f, 1e-4f) && Near(vecRot[1], 1.0f, 1e-4f) && Near(vecRot[2], 0.0f, 1e-4f),
          "RotateEditorVectorAroundAxis +90 deg Z mismatch");

    float rotatedAngles[3] = {};
    const float zeroAngles[3] = { 0.0f, 0.0f, 0.0f };
    const float xAxis[3] = { 1.0f, 0.0f, 0.0f };
    RotateEditorEulerAnglesAroundAxis(zeroAngles, xAxis, 90.0f, rotatedAngles);
    EditorVectorFieldDef vf;
    std::copy_n(rotatedAngles, 3, vf.anglesDeg);
    vf.axis[0] = 0.0f;
    vf.axis[1] = 0.0f;
    vf.axis[2] = 1.0f;
    const QrVectorFieldVolume vol = BuildEditorVectorFieldVolumeQuakeUnits(vf, QR_EMPTY_VECTOR_FIELD);
    Check(Near(vol.axis.data[0], 0.0f, 1e-4f) &&
              Near(vol.axis.data[1], -1.0f, 1e-4f) &&
              Near(vol.axis.data[2], 0.0f, 1e-4f),
          "BuildEditorVectorFieldVolumeQuakeUnits rotated flow axis mismatch");
}

void TestExplosionHdrPresetAndEmissionScale()
{
    const float origin[3] = { 10.0f, 20.0f, 30.0f };
    const float normal[3] = { 0.0f, 0.0f, 1.0f };
    std::vector<QrParticleBurstInfo> bursts;
    BuildEditorPresetBurstsQuakeUnits(EditorParticlePreset::ExplosionSparks, origin, normal, 2u,
                                      1.0f, bursts);
    Check(bursts.size() == 3u, "ExplosionSparks must emit 3 HDR additive layers");
    Check(bursts[0].renderMode == QR_GPU_PARTICLE_RENDER_SPARK &&
              bursts[1].renderMode == QR_GPU_PARTICLE_RENDER_BILLBOARD &&
              bursts[2].renderMode == QR_GPU_PARTICLE_RENDER_SPARK,
          "ExplosionSparks layer render modes mismatch");
    for (const QrParticleBurstInfo &b : bursts)
    {
        Check(b.emission >= 20.0f, "ExplosionSparks layer emission must reach HDR bloom range");
        Check((b.flags & QR_GPU_PARTICLE_FLAG_ADDITIVE_BLEND_BIT) != 0u,
              "ExplosionSparks layer must use additive blend");
    }

    std::vector<QrParticleBurstInfo> scaledBursts;
    BuildEditorPresetBurstsQuakeUnits(EditorParticlePreset::ExplosionSparks, origin, normal, 2u,
                                      1.0f, scaledBursts, 2.5f);
    Check(scaledBursts.size() == bursts.size(), "Scaled ExplosionSparks layer count mismatch");
    for (size_t i = 0; i < bursts.size(); i++)
    {
        Check(Near(scaledBursts[i].emission, bursts[i].emission * 2.5f, 1e-3f),
              "ExplosionSparks emissionScale multiplier mismatch");
    }

    std::vector<QrParticleBurstInfo> retroBursts;
    BuildEditorPresetBurstsQuakeUnits(EditorParticlePreset::QuakeRetroShotgun, origin, normal, 3u,
                                      1.0f, retroBursts, 2.5f);
    Check(retroBursts.size() == 1u && Near(retroBursts[0].emission, 0.0f, 1e-5f),
          "Unlit QuakeRetroShotgun emission must stay 0 under emissionScale");
}

} // namespace

int main()
{
    try
    {
        TestFgaParserAndDirectoryGuard();
        TestBrickResampleAndSpatialFrame();
        TestEditorSpatialScaling();
        TestPresetsAndYamlRoundTrip();
        TestPublicApiValidation();
        TestRotationMathAndVectorFieldAxis();
        TestExplosionHdrPresetAndEmissionScale();
        std::cout << "qray_gpu_particles_tests: all checks passed\n";
        return 0;
    }
    catch (const std::exception &ex)
    {
        std::cerr << "qray_gpu_particles_tests failed: " << ex.what() << '\n';
        return 1;
    }
}
