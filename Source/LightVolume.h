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
// The CPU reference for the id Tech 8 cascaded/local irradiance volumes:
//   * 6 cascades x 16^3 probes, nested around the camera origin and snapped to 640 u;
//   * hand-placed local 10^3 volumes for interiors the cascades resolve too coarsely;
//   * one 4-coefficient RGB SH per probe (L0, L1x, L1y, L1z);
//   * interleaved updates: one cascade and one local volume per frame.
//
// This is the ABI and sampling oracle, not the runtime update path. The GPU builder will own the
// probe atlas; the constants and the lookup math here are what the shader has to match. The
// cascade bounds follow the light grid: the first cascade is 1,280 u (16 cells x 80 u, about 32 m),
// and every following cascade doubles the side.

#pragma once

#include <cstdint>
#include <vector>

namespace qray
{

struct LightVolumeProbe
{
    // Integrated SH coefficients: L0, L1x, L1y, L1z, one RGB triple per band.
    float sh[4][3];
};

static_assert(sizeof(LightVolumeProbe) == 48, "LightVolumeProbe has to keep the 48-byte SH4 RGB layout");

class LightVolume
{
public:
    static constexpr uint32_t kCascades = 6;
    static constexpr uint32_t kDim = 16;
    static constexpr uint32_t kProbesPerCascade = kDim * kDim * kDim;
    static constexpr uint32_t kLocalDim = 10;
    static constexpr uint32_t kProbesPerLocalVolume = kLocalDim * kLocalDim * kLocalDim;
    static constexpr uint32_t kMaxLocalVolumes = 128;
    static constexpr uint32_t kCascadeProbeCount = kCascades * kProbesPerCascade;
    static constexpr uint32_t kProbeCount = kCascadeProbeCount + kMaxLocalVolumes * kProbesPerLocalVolume;
    static constexpr float kBaseBounds = 1280.0f;
    static constexpr float kSnap = 640.0f;

    struct Cascade
    {
        float origin[3];
        float bounds;
        float cellSize;
    };

    struct LocalVolume
    {
        float origin[3];
        float bounds;
        bool enabled;
    };

    struct Stats
    {
        uint32_t cascades;
        uint32_t probesPerCascade;
        uint32_t localVolumes;
        uint32_t activeLocalVolumes;
        float baseBounds;
    };

    LightVolume();

    void Reset();

    // Snaps the cascade centre to kSnap, like the light grid's 640 u rebuild snap.
    void SetCameraOrigin(const float origin[3]);
    void GetCameraOrigin(float outOrigin[3]) const;

    void GetCascade(uint32_t cascade, Cascade &outCascade) const;
    int32_t FindCascade(const float position[3]) const;

    // Local volumes are hand-placed. 'bounds' is the full side length of the cube; the probe
    // positions sit at the centres of the 10^3 cells.
    void SetLocalVolume(uint32_t index, const float origin[3], float bounds, bool enabled);
    int32_t FindLocalVolume(const float position[3]) const;

    // Probe indexing: cascades first, then the local volumes in declaration order.
    uint32_t GetCascadeProbeIndex(uint32_t cascade, uint32_t x, uint32_t y, uint32_t z) const;
    uint32_t GetLocalProbeIndex(uint32_t localVolume, uint32_t x, uint32_t y, uint32_t z) const;
    bool GetProbePosition(uint32_t probeIndex, float outPosition[3], float &outCellSize) const;

    // Trilinear lookup. 'outBaseIndex' is the (0,0,0) corner; weights are indexed [z][y][x],
    // matching the probe layout's (z * dim + y) * dim + x order.
    bool GetCascadeLookup(const float position[3], uint32_t &outCascade,
                          uint32_t &outBaseIndex, float outWeights[2][2][2]) const;
    bool GetLocalLookup(const float position[3], uint32_t &outLocalVolume,
                        uint32_t &outBaseIndex, float outWeights[2][2][2]) const;

    // Local volumes win over cascades when both contain the point; the smallest enabled local
    // volume wins among them. With no local volume the finest containing cascade is used.
    bool SampleIrradiance(const float position[3], const float normal[3], float outRgb[3]) const;

    // SH helpers shared with the future GPU builder/tests. AccumulateRadiance adds one direction
    // sample; NormalizeSh multiplies by 4*pi / sampleCount, turning the sum into integrated
    // coefficients. EvaluateIrradiance applies the Lambertian convolution (0.886227 for L0 and
    // 1.023328 for L1).
    static void AccumulateRadiance(const float direction[3], const float rgb[3], float sh[4][3]);
    static void NormalizeSh(float sh[4][3], uint32_t sampleCount);
    static void EvaluateIrradiance(const float sh[4][3], const float normal[3], float outRgb[3]);

    // One cascade and one local volume per frame, wrapping independently.
    void GetUpdateSchedule(uint32_t frameIndex, uint32_t &outCascade, int32_t &outLocalVolume) const;

    LightVolumeProbe &Probe(uint32_t probeIndex);
    const LightVolumeProbe &Probe(uint32_t probeIndex) const;

    const Stats &GetStats() const { return stats; }

private:
    void RebuildStats();

    float cameraOrigin[3] = {};
    LocalVolume localVolumes[kMaxLocalVolumes] = {};
    std::vector<LightVolumeProbe> probes;
    Stats stats = {};
};

}
