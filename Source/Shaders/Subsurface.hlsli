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

#ifndef SUBSURFACE_HLSLI_
#define SUBSURFACE_HLSLI_

#define Q2_SSS_MAX_RADIUS_METERS 1.0

float3 decodeSubsurfaceColor(const uint packed)
{
    const float r = (float)(packed & 31u) / 31.0;
    const float g = (float)((packed >> 5u) & 63u) / 63.0;
    const float b = (float)((packed >> 11u) & 31u) / 31.0;
    return float3(r * r, g * g, b * b);
}

float decodeSubsurfaceRadius(const uint packed)
{
    const float rad = (float)((packed >> 16u) & 255u) / 255.0;
    return (rad * rad) * Q2_SSS_MAX_RADIUS_METERS;
}

float decodeSubsurfaceStrength(const uint packed)
{
    return (float)((packed >> 24u) & 255u) / 255.0;
}

// Packs per-channel mean free path d.rgb (in meters) alongside the surface mask in [0, 1].
float4 buildSubsurfaceGBuffer(const uint packed, const float normalAlphaMask)
{
    if (packed == 0u)
    {
        return (float4)0.0;
    }

    const float mask = decodeSubsurfaceStrength(packed) * saturate(normalAlphaMask);
    const float radius = decodeSubsurfaceRadius(packed);
    if (mask <= 0.0 || radius <= 0.0)
    {
        return (float4)0.0;
    }

    const float3 d = decodeSubsurfaceColor(packed) * radius;
    if (max(d.r, max(d.g, d.b)) <= 0.0)
    {
        return (float4)0.0;
    }

    return float4(d, mask);
}

float getSubsurfaceMask(const float4 sssData)
{
    return saturate(sssData.a);
}

float3 getSubsurfaceMeanFreePath(const float4 sssData)
{
    return max(sssData.rgb, (float3)0.0);
}

// 1D normalized Burley diffusion profile over signed distance |x| in meters.
float3 evalBurleyDiffusion1D(const float absDistMeters, const float3 meanFreePath)
{
    const float3 safeD = max(meanFreePath, (float3)1e-4);
    const float3 xOverD = (float3)absDistMeters / safeD;
    return (exp(-xOverD) + exp(-xOverD / 3.0)) / (8.0 * safeD);
}

#endif
