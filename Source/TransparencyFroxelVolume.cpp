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

#include "TransparencyFroxelVolume.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace qray
{

namespace
{

constexpr float kPi = 3.14159265358979323846f;

}

TransparencyFroxelVolume::TransparencyFroxelVolume()
    : probes(kProbeCount)
{
    const float defaultOrigin[3] = { -4096.0f, -2048.0f, -4096.0f };
    const float defaultSize[3] = { 8192.0f, 4096.0f, 8192.0f };
    SetBounds(defaultOrigin, defaultSize);
}

void TransparencyFroxelVolume::SetBounds(const float newOrigin[3], const float newSize[3])
{
    for (int axis = 0; axis < 3; axis++)
    {
        origin[axis] = newOrigin[axis];
        size[axis] = std::max(newSize[axis], 1.0f);
    }
}

bool TransparencyFroxelVolume::GetBounds(float outOrigin[3], float outSize[3]) const
{
    if (outOrigin == nullptr || outSize == nullptr)
    {
        return false;
    }

    std::memcpy(outOrigin, origin, sizeof(origin));
    std::memcpy(outSize, size, sizeof(size));
    return true;
}

TransparencyFroxelProbe &TransparencyFroxelVolume::Probe(uint32_t x, uint32_t y, uint32_t z)
{
    const uint32_t index = (z * kDimY + y) * kDimX + x;
    return probes[index];
}

const TransparencyFroxelProbe &TransparencyFroxelVolume::Probe(uint32_t x, uint32_t y, uint32_t z) const
{
    const uint32_t index = (z * kDimY + y) * kDimX + x;
    return probes[index];
}

void TransparencyFroxelVolume::AccumulateRadiance(const float direction[3], const float rgb[3],
                                                   float sh[4][3])
{
    const float basis[4] =
    {
        0.282095f,
        0.488603f * direction[0],
        0.488603f * direction[1],
        0.488603f * direction[2],
    };

    for (uint32_t band = 0; band < 4; band++)
    {
        for (uint32_t channel = 0; channel < 3; channel++)
        {
            sh[band][channel] += rgb[channel] * basis[band];
        }
    }
}

void TransparencyFroxelVolume::NormalizeSh(float sh[4][3], uint32_t sampleCount)
{
    if (sampleCount == 0)
    {
        std::memset(sh, 0, sizeof(float) * 12);
        return;
    }

    const float scale = 4.0f * kPi / float(sampleCount);

    for (uint32_t band = 0; band < 4; band++)
    {
        for (uint32_t channel = 0; channel < 3; channel++)
        {
            sh[band][channel] *= scale;
        }
    }
}

void TransparencyFroxelVolume::EvaluateIrradiance(const float sh[4][3], const float normal[3],
                                                   float outRgb[3])
{
    const float n = normal[0] * 0.488603f;
    const float m = normal[1] * 0.488603f;
    const float l = normal[2] * 0.488603f;

    for (uint32_t channel = 0; channel < 3; channel++)
    {
        outRgb[channel] = 0.886227f * sh[0][channel] +
                          1.023328f * (sh[1][channel] * n + sh[2][channel] * m + sh[3][channel] * l);
    }
}

bool TransparencyFroxelVolume::SampleIrradiance(const float position[3], const float normal[3],
                                                float outRgb[3]) const
{
    if (position == nullptr || normal == nullptr || outRgb == nullptr)
    {
        return false;
    }

    float cell[3];
    float index[3];

    for (int axis = 0; axis < 3; axis++)
    {
        const uint32_t dim = axis == 0 ? kDimX : axis == 1 ? kDimY : kDimZ;
        cell[axis] = size[axis] / float(dim);
        index[axis] = (position[axis] - origin[axis]) / cell[axis] - 0.5f;

        if (index[axis] < -0.5f || index[axis] > float(dim - 1) + 0.5f)
        {
            return false;
        }
    }

    uint32_t base[3];
    float frac[3];

    for (int axis = 0; axis < 3; axis++)
    {
        const uint32_t dim = axis == 0 ? kDimX : axis == 1 ? kDimY : kDimZ;
        const float value = std::floor(index[axis]);
        base[axis] = uint32_t(std::max(0.0f, std::min(value, float(dim - 2))));
        frac[axis] = std::clamp(index[axis] - float(base[axis]), 0.0f, 1.0f);
    }

    float sh[4][3] = {};

    for (uint32_t dz = 0; dz < 2; dz++)
    {
        for (uint32_t dy = 0; dy < 2; dy++)
        {
            for (uint32_t dx = 0; dx < 2; dx++)
            {
                const float wx = dx ? frac[0] : 1.0f - frac[0];
                const float wy = dy ? frac[1] : 1.0f - frac[1];
                const float wz = dz ? frac[2] : 1.0f - frac[2];
                const float w = wx * wy * wz;
                const TransparencyFroxelProbe &probe = Probe(base[0] + dx, base[1] + dy, base[2] + dz);

                for (uint32_t band = 0; band < 4; band++)
                {
                    for (uint32_t channel = 0; channel < 3; channel++)
                    {
                        sh[band][channel] += probe.sh[band][channel] * w;
                    }
                }
            }
        }
    }

    EvaluateIrradiance(sh, normal, outRgb);
    return true;
}

}
