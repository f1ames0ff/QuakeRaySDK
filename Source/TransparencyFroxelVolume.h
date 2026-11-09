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
// CPU reference for the report's transparency froxel irradiance volume: one world-space box of
// 64 x 32 x 64 froxels, each holding a 4-coefficient RGB SH probe. The volume is shared by every
// transparency (particles, glass, decals) so the per-transparency lookup is one trilinear sample
// plus the SH convolution. This is the ABI and sampling oracle; the GPU image and its light
// scattering update are the next step.

#pragma once

#include <cstdint>
#include <vector>

namespace qray
{

struct TransparencyFroxelProbe
{
    float sh[4][3];
};

static_assert(sizeof(TransparencyFroxelProbe) == 48,
              "TransparencyFroxelProbe has to keep the 48-byte SH4 RGB layout");

class TransparencyFroxelVolume
{
public:
    static constexpr uint32_t kDimX = 64;
    static constexpr uint32_t kDimY = 32;
    static constexpr uint32_t kDimZ = 64;
    static constexpr uint32_t kProbeCount = kDimX * kDimY * kDimZ;

    // The GPU tiers: the default oracle above is the medium grid. The high grid is the report's
    // ~50 MB budget (128 x 64 x 128 x 48 B = 50.3 MB); the low grid is the quality fallback.
    static constexpr uint32_t kLowDimX = 32;
    static constexpr uint32_t kLowDimY = 16;
    static constexpr uint32_t kLowDimZ = 32;
    static constexpr uint32_t kHighDimX = 128;
    static constexpr uint32_t kHighDimY = 64;
    static constexpr uint32_t kHighDimZ = 128;
    static constexpr uint32_t kHighProbeCount = kHighDimX * kHighDimY * kHighDimZ;

    TransparencyFroxelVolume();

    // The box the froxels cover. 'size' is the full side length on each axis; the volume is
    // independent of the camera snap so a level can keep it static.
    void SetBounds(const float origin[3], const float size[3]);
    bool GetBounds(float outOrigin[3], float outSize[3]) const;

    // Trilinear lookup with the Lambertian SH convolution. Returns false outside the box.
    bool SampleIrradiance(const float position[3], const float normal[3], float outRgb[3]) const;

    TransparencyFroxelProbe &Probe(uint32_t x, uint32_t y, uint32_t z);
    const TransparencyFroxelProbe &Probe(uint32_t x, uint32_t y, uint32_t z) const;

    static void AccumulateRadiance(const float direction[3], const float rgb[3], float sh[4][3]);
    static void NormalizeSh(float sh[4][3], uint32_t sampleCount);
    static void EvaluateIrradiance(const float sh[4][3], const float normal[3], float outRgb[3]);

private:
    float origin[3] = {};
    float size[3] = {};
    std::vector<TransparencyFroxelProbe> probes;
};

}
