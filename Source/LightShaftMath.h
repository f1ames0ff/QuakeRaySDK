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

#pragma once

#include <algorithm>
#include <cmath>

namespace qray
{

inline constexpr double kLightShaftPi = 3.1415926535897932384626433;
inline constexpr float kLightShaftMinReachMeters = 1.6f;
inline constexpr float kLightShaftMaxReachMeters = 204.8f;
inline constexpr float kLightShaftDefaultVolumeFarMeters = 64.0f;

inline float ResolveVolumetricCameraFar(bool legacyVolumeOn,
                                        float volumetricFar,
                                        float cameraNear,
                                        float cameraFar,
                                        bool lightShaftsRequested)
{
    const float safeNear = std::max(cameraNear, 2.5e-5f);
    const bool hasExplicitFar = std::isfinite(volumetricFar) && volumetricFar > safeNear;
    float farTarget = 2.5f;

    if (legacyVolumeOn)
    {
        if (hasExplicitFar)
        {
            farTarget = volumetricFar;
        }
        else if (lightShaftsRequested)
        {
            farTarget = kLightShaftDefaultVolumeFarMeters;
        }
    }
    else if (lightShaftsRequested)
    {
        farTarget = hasExplicitFar
            ? std::max(volumetricFar, kLightShaftDefaultVolumeFarMeters)
            : kLightShaftDefaultVolumeFarMeters;
    }
    else if (hasExplicitFar)
    {
        farTarget = volumetricFar;
    }

    const float clampedFar = std::isfinite(cameraFar) ? std::min(cameraFar, farTarget) : farTarget;
    return std::max(safeNear + 0.1f, clampedFar);
}

inline float ShaftPhase(float cosTheta, float asymmetry)
{
    const float k = 1.55f * asymmetry - 0.55f * asymmetry * asymmetry * asymmetry;
    const float denominator = 1.0f + k * cosTheta;
    return (1.0f - k * k) / (4.0f * static_cast<float>(kLightShaftPi) * denominator * denominator);
}

inline float ComputeFluxShaftReach(const float fluxColor[3], float power)
{
    if (fluxColor == nullptr || !std::isfinite(power) || !(power > 0.0f))
    {
        return 0.0f;
    }

    const float maxColor = std::max(std::max(fluxColor[0], fluxColor[1]), std::max(fluxColor[2], 0.0f));
    const float maxFlux = maxColor * power;
    if (!std::isfinite(maxFlux) || !(maxFlux > 0.0f))
    {
        return 0.0f;
    }

    const float reach = std::sqrt(maxFlux / (static_cast<float>(kLightShaftPi) * 0.01f));
    return std::clamp(reach, kLightShaftMinReachMeters, kLightShaftMaxReachMeters);
}

inline float ComputeRadianceShaftReach(const float radianceColor[3], float area, float meanEmiss, float power)
{
    if (radianceColor == nullptr || !std::isfinite(power) || !(power > 0.0f))
    {
        return 0.0f;
    }

    const float maxColor = std::max(std::max(radianceColor[0], radianceColor[1]), std::max(radianceColor[2], 0.0f));
    const float safeArea = (std::isfinite(area) && area > 0.0f) ? area : 0.0f;
    const float emiss = (std::isfinite(meanEmiss) && meanEmiss > 0.0f) ? meanEmiss : 1.0f;
    const float maxFlux = maxColor * safeArea * emiss * power;
    if (!std::isfinite(maxFlux) || !(maxFlux > 0.0f))
    {
        return 0.0f;
    }

    const float reach = std::sqrt(maxFlux / (static_cast<float>(kLightShaftPi) * 0.01f));
    return std::clamp(reach, kLightShaftMinReachMeters, kLightShaftMaxReachMeters);
}

inline bool EncodeLightShaftCone(float angleInner, float angleOuter, bool isProjector, bool isSpot, float outCone[2])
{
    if (outCone == nullptr)
    {
        return false;
    }

    const float minOuter = static_cast<float>(kLightShaftPi / 180.0);
    float outer = 0.0f;

    if (isSpot)
    {
        if (!std::isfinite(angleOuter) || !(angleOuter > 0.0f))
        {
            return false;
        }
        outer = std::clamp(angleOuter, minOuter, static_cast<float>(kLightShaftPi * 0.99));
    }
    else
    {
        const bool validOuter = std::isfinite(angleOuter) && angleOuter > 0.0f &&
                                angleOuter <= static_cast<float>(kLightShaftPi * 0.5);
        const float fallbackOuter = isProjector
                                        ? static_cast<float>(kLightShaftPi / 3.0)
                                        : static_cast<float>(kLightShaftPi * 0.5);
        outer = validOuter ? std::max(angleOuter, minOuter) : fallbackOuter;
    }

    const float safeInner = (std::isfinite(angleInner) && angleInner >= 0.0f) ? angleInner : 0.0f;
    const float inner = std::min(safeInner, outer * 0.999f);
    outCone[0] = std::cos(inner);
    outCone[1] = std::cos(outer);
    return true;
}

inline float MediumTransmittance(float sigma, float distance)
{
    return std::exp(-std::max(sigma, 0.0f) * std::max(distance, 0.0f));
}

inline float MediumIntegration(float sigma, float distance)
{
    const float s = std::max(sigma, 0.0f);
    const float d = std::max(distance, 0.0f);
    return s > 1e-6f ? (1.0f - std::exp(-s * d)) / s : d;
}

inline float MediumAccumulate(float source, float sigma, float distance, float &transmittance)
{
    const float stepTransmittance = MediumTransmittance(sigma, distance);
    const float inScattering = transmittance * source * MediumIntegration(sigma, distance);
    transmittance *= stepTransmittance;
    return inScattering;
}

inline float EncodeShaftExtinction(float transmittance)
{
    return std::clamp(1.0f - std::clamp(transmittance, 0.0f, 1.0f), 0.0f, 1.0f);
}

inline float DecodeShaftTransmittance(float extinctionAlpha)
{
    return std::clamp(1.0f - std::clamp(extinctionAlpha, 0.0f, 1.0f), 0.0f, 1.0f);
}

}
