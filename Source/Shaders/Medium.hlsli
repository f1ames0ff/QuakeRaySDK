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

#ifndef MEDIUM_HLSLI_
#define MEDIUM_HLSLI_

float mediumTransmittance(float sigma, float distance)
{
    return exp(-max(sigma, 0.0) * max(distance, 0.0));
}

float mediumIntegration(float sigma, float distance)
{
    const float s = max(sigma, 0.0);
    const float d = max(distance, 0.0);
    return s > 1e-6 ? (1.0 - exp(-s * d)) / s : d;
}

float3 mediumAccumulate(float3 source, float sigma, float distance, inout float transmittance)
{
    const float stepTransmittance = mediumTransmittance(sigma, distance);
    const float3 inScattering = transmittance * source * mediumIntegration(sigma, distance);
    transmittance *= stepTransmittance;
    return inScattering;
}

#endif
