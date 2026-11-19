#include "LightVolume.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

namespace qray
{

namespace
{

constexpr float kPi = 3.14159265358979323846f;
constexpr float kEpsilon = 1e-4f;

float Clamp01(float value)
{
    return std::max(0.0f, std::min(1.0f, value));
}

}

LightVolume::LightVolume()
{
    probes.assign(kProbeCount, LightVolumeProbe{});
    Reset();
}

void LightVolume::Reset()
{
    std::memset(cameraOrigin, 0, sizeof(cameraOrigin));
    std::memset(localVolumes, 0, sizeof(localVolumes));
    std::fill(probes.begin(), probes.end(), LightVolumeProbe{});
    RebuildStats();
}

void LightVolume::SetCameraOrigin(const float origin[3])
{
    for (int axis = 0; axis < 3; axis++)
    {
        cameraOrigin[axis] = std::round(origin[axis] / kSnap) * kSnap;
    }
}

void LightVolume::GetCameraOrigin(float outOrigin[3]) const
{
    std::memcpy(outOrigin, cameraOrigin, sizeof(cameraOrigin));
}

void LightVolume::GetCascade(uint32_t cascade, Cascade &outCascade) const
{
    assert(cascade < kCascades);

    outCascade.bounds = kBaseBounds * std::exp2(float(cascade));
    outCascade.cellSize = outCascade.bounds / float(kDim);
    std::memcpy(outCascade.origin, cameraOrigin, sizeof(cameraOrigin));
}

int32_t LightVolume::FindCascade(const float position[3]) const
{
    for (uint32_t cascade = 0; cascade < kCascades; cascade++)
    {
        Cascade description;
        GetCascade(cascade, description);

        const float half = description.bounds * 0.5f;
        bool inside = true;

        for (int axis = 0; axis < 3; axis++)
        {
            if (std::fabs(position[axis] - description.origin[axis]) > half + kEpsilon)
            {
                inside = false;
                break;
            }
        }

        if (inside)
        {
            return int32_t(cascade);
        }
    }

    return -1;
}

void LightVolume::SetLocalVolume(uint32_t index, const float origin[3], float bounds, bool enabled)
{
    assert(index < kMaxLocalVolumes);

    LocalVolume &volume = localVolumes[index];
    std::memcpy(volume.origin, origin, sizeof(volume.origin));
    volume.bounds = std::max(bounds, 0.0f);
    volume.enabled = enabled && volume.bounds > 0.0f;

    RebuildStats();
}

int32_t LightVolume::FindLocalVolume(const float position[3]) const
{
    int32_t best = -1;
    float bestBounds = 0.0f;

    for (uint32_t index = 0; index < kMaxLocalVolumes; index++)
    {
        const LocalVolume &volume = localVolumes[index];

        if (!volume.enabled)
        {
            continue;
        }

        const float half = volume.bounds * 0.5f;
        bool inside = true;

        for (int axis = 0; axis < 3; axis++)
        {
            if (std::fabs(position[axis] - volume.origin[axis]) > half + kEpsilon)
            {
                inside = false;
                break;
            }
        }

        if (inside && (best < 0 || volume.bounds < bestBounds))
        {
            best = int32_t(index);
            bestBounds = volume.bounds;
        }
    }

    return best;
}

uint32_t LightVolume::GetCascadeProbeIndex(uint32_t cascade, uint32_t x, uint32_t y, uint32_t z) const
{
    assert(cascade < kCascades && x < kDim && y < kDim && z < kDim);
    return cascade * kProbesPerCascade + (z * kDim + y) * kDim + x;
}

uint32_t LightVolume::GetLocalProbeIndex(uint32_t localVolume, uint32_t x, uint32_t y, uint32_t z) const
{
    assert(localVolume < kMaxLocalVolumes && x < kLocalDim && y < kLocalDim && z < kLocalDim);
    return kCascadeProbeCount + localVolume * kProbesPerLocalVolume +
           (z * kLocalDim + y) * kLocalDim + x;
}

bool LightVolume::GetProbePosition(uint32_t probeIndex, float outPosition[3], float &outCellSize) const
{
    if (probeIndex >= kProbeCount)
    {
        return false;
    }

    if (probeIndex < kCascadeProbeCount)
    {
        const uint32_t cascade = probeIndex / kProbesPerCascade;
        const uint32_t local = probeIndex % kProbesPerCascade;
        const uint32_t x = local % kDim;
        const uint32_t y = (local / kDim) % kDim;
        const uint32_t z = local / (kDim * kDim);

        Cascade description;
        GetCascade(cascade, description);

        outCellSize = description.cellSize;

        const uint32_t coordinates[3] = { x, y, z };

        for (int axis = 0; axis < 3; axis++)
        {
            outPosition[axis] = description.origin[axis] - description.bounds * 0.5f +
                (float(coordinates[axis]) + 0.5f) * description.cellSize;
        }

        return true;
    }

    const uint32_t localIndex = (probeIndex - kCascadeProbeCount) / kProbesPerLocalVolume;
    const uint32_t local = (probeIndex - kCascadeProbeCount) % kProbesPerLocalVolume;
    const uint32_t x = local % kLocalDim;
    const uint32_t y = (local / kLocalDim) % kLocalDim;
    const uint32_t z = local / (kLocalDim * kLocalDim);

    if (localIndex >= kMaxLocalVolumes)
    {
        return false;
    }

    const LocalVolume &volume = localVolumes[localIndex];

    if (!volume.enabled)
    {
        return false;
    }

    outCellSize = volume.bounds / float(kLocalDim);

    const uint32_t coordinates[3] = { x, y, z };

    for (int axis = 0; axis < 3; axis++)
    {
        outPosition[axis] = volume.origin[axis] - volume.bounds * 0.5f +
            (float(coordinates[axis]) + 0.5f) * outCellSize;
    }

    return true;
}

bool LightVolume::GetCascadeLookup(const float position[3], uint32_t &outCascade,
                                   uint32_t &outBaseIndex, float outWeights[2][2][2]) const
{
    const int32_t cascade = FindCascade(position);

    if (cascade < 0)
    {
        return false;
    }

    Cascade description;
    GetCascade(uint32_t(cascade), description);

    int base[3];
    float frac[3];

    for (int axis = 0; axis < 3; axis++)
    {
        const float index = (position[axis] - description.origin[axis]) / description.cellSize + float(kDim) * 0.5f;
        const float baseValue = std::floor(index - 0.5f);

        base[axis] = std::clamp(int32_t(baseValue), 0, int32_t(kDim) - 2);
        frac[axis] = Clamp01(index - 0.5f - float(base[axis]));
    }

    outCascade = uint32_t(cascade);
    outBaseIndex = GetCascadeProbeIndex(uint32_t(cascade), uint32_t(base[0]), uint32_t(base[1]), uint32_t(base[2]));

    const float x = frac[0];
    const float y = frac[1];
    const float z = frac[2];

    for (uint32_t dz = 0; dz < 2; dz++)
    {
        for (uint32_t dy = 0; dy < 2; dy++)
        {
            for (uint32_t dx = 0; dx < 2; dx++)
            {
                const float wx = dx ? x : (1.0f - x);
                const float wy = dy ? y : (1.0f - y);
                const float wz = dz ? z : (1.0f - z);
                outWeights[dz][dy][dx] = wx * wy * wz;
            }
        }
    }
    return true;
}

bool LightVolume::GetLocalLookup(const float position[3], uint32_t &outLocalVolume,
                                 uint32_t &outBaseIndex, float outWeights[2][2][2]) const
{
    const int32_t localIndex = FindLocalVolume(position);

    if (localIndex < 0)
    {
        return false;
    }

    const LocalVolume &volume = localVolumes[localIndex];
    const float cellSize = volume.bounds / float(kLocalDim);

    int base[3];
    float frac[3];

    for (int axis = 0; axis < 3; axis++)
    {
        const float index = (position[axis] - volume.origin[axis]) / cellSize + float(kLocalDim) * 0.5f;
        const float baseValue = std::floor(index - 0.5f);

        base[axis] = std::clamp(int32_t(baseValue), 0, int32_t(kLocalDim) - 2);
        frac[axis] = Clamp01(index - 0.5f - float(base[axis]));
    }

    outLocalVolume = uint32_t(localIndex);
    outBaseIndex = GetLocalProbeIndex(uint32_t(localIndex), uint32_t(base[0]), uint32_t(base[1]), uint32_t(base[2]));

    const float x = frac[0];
    const float y = frac[1];
    const float z = frac[2];

    for (uint32_t dz = 0; dz < 2; dz++)
    {
        for (uint32_t dy = 0; dy < 2; dy++)
        {
            for (uint32_t dx = 0; dx < 2; dx++)
            {
                const float wx = dx ? x : (1.0f - x);
                const float wy = dy ? y : (1.0f - y);
                const float wz = dz ? z : (1.0f - z);
                outWeights[dz][dy][dx] = wx * wy * wz;
            }
        }
    }
    return true;
}

bool LightVolume::SampleIrradiance(const float position[3], const float normal[3], float outRgb[3]) const
{
    outRgb[0] = outRgb[1] = outRgb[2] = 0.0f;

    uint32_t baseIndex = 0;
    uint32_t localVolume = 0;
    uint32_t cascade = 0;
    uint32_t dim = kDim;
    float weights[2][2][2] = {};

    if (GetLocalLookup(position, localVolume, baseIndex, weights))
    {
        dim = kLocalDim;
    }
    else if (!GetCascadeLookup(position, cascade, baseIndex, weights))
    {
        return false;
    }

    for (uint32_t dz = 0; dz < 2; dz++)
    {
        for (uint32_t dy = 0; dy < 2; dy++)
        {
            for (uint32_t dx = 0; dx < 2; dx++)
            {
                const float weight = weights[dz][dy][dx];

                if (weight <= 0.0f)
                {
                    continue;
                }

                const uint32_t probeIndex = baseIndex + dx + dim * dy + dim * dim * dz;
                const LightVolumeProbe &probe = probes[probeIndex];
                float irradiance[3];
                EvaluateIrradiance(probe.sh, normal, irradiance);

                for (int channel = 0; channel < 3; channel++)
                {
                    outRgb[channel] += irradiance[channel] * weight;
                }
            }
        }
    }

    return true;
}

void LightVolume::AccumulateRadiance(const float direction[3], const float rgb[3], float sh[4][3])
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

void LightVolume::NormalizeSh(float sh[4][3], uint32_t sampleCount)
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

void LightVolume::EvaluateIrradiance(const float sh[4][3], const float normal[3], float outRgb[3])
{
    const float basis[4] =
    {
        0.886227f,
        1.023328f * normal[0],
        1.023328f * normal[1],
        1.023328f * normal[2],
    };

    for (uint32_t channel = 0; channel < 3; channel++)
    {
        outRgb[channel] = 0.0f;

        for (uint32_t band = 0; band < 4; band++)
        {
            outRgb[channel] += sh[band][channel] * basis[band];
        }
    }
}

void LightVolume::GetUpdateSchedule(uint32_t frameIndex, uint32_t &outCascade, int32_t &outLocalVolume) const
{
    outCascade = frameIndex % kCascades;
    outLocalVolume = -1;

    const uint32_t active = stats.activeLocalVolumes;

    if (active == 0)
    {
        return;
    }

    uint32_t ordinal = frameIndex % active;

    for (uint32_t index = 0; index < kMaxLocalVolumes; index++)
    {
        if (!localVolumes[index].enabled)
        {
            continue;
        }

        if (ordinal == 0)
        {
            outLocalVolume = int32_t(index);
            return;
        }

        ordinal--;
    }
}

LightVolumeProbe &LightVolume::Probe(uint32_t probeIndex)
{
    assert(probeIndex < kProbeCount);
    return probes[probeIndex];
}

const LightVolumeProbe &LightVolume::Probe(uint32_t probeIndex) const
{
    assert(probeIndex < kProbeCount);
    return probes[probeIndex];
}

void LightVolume::RebuildStats()
{
    stats.cascades = kCascades;
    stats.probesPerCascade = kProbesPerCascade;
    stats.localVolumes = kMaxLocalVolumes;
    stats.activeLocalVolumes = 0;
    stats.baseBounds = kBaseBounds;

    for (const LocalVolume &volume : localVolumes)
    {
        if (volume.enabled)
        {
            stats.activeLocalVolumes++;
        }
    }
}

}
