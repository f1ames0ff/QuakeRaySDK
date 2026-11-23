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

#ifndef LIGHT_SHAFT_BLUR_HLSLI_
#define LIGHT_SHAFT_BLUR_HLSLI_

float shaftBilateralDepthWeight(float tapDepth, float refDepth, float depthSlope, float offsetPx)
{
    const float expectedDepth = max(refDepth + depthSlope * offsetPx, 0.0);
    const float relDiff = abs(tapDepth - expectedDepth) / max(refDepth, 1e-4);
    const float normalized = relDiff / 0.06;
    return exp(-0.5 * normalized * normalized);
}

float4 filterLightShaft1D(Texture2D<float4> scatterTex,
                          Texture2D<float4> viewDepthTex,
                          int2 pixelPos,
                          int2 renderSize,
                          int2 axis,
                          float froxelPixels)
{
    const float refDepth = abs(viewDepthTex.Load(int3(getCheckerboardPix(pixelPos), 0)).r);
    const int2 prevPos = clamp(pixelPos - axis, (int2)0, renderSize - 1);
    const int2 nextPos = clamp(pixelPos + axis, (int2)0, renderSize - 1);
    const float dPrev = abs(viewDepthTex.Load(int3(getCheckerboardPix(prevPos), 0)).r);
    const float dNext = abs(viewDepthTex.Load(int3(getCheckerboardPix(nextPos), 0)).r);
    const float sBack = refDepth - dPrev;
    const float sFwd = dNext - refDepth;
    float depthSlope = abs(sBack) < abs(sFwd) ? sBack : sFwd;
    if (abs(depthSlope) > refDepth * 0.25)
    {
        depthSlope = 0.0;
    }

    const int o1 = max(1, (int)round(0.18 * froxelPixels));
    const int o2 = max(o1 + 1, (int)round(0.45 * froxelPixels));
    const int o3 = max(o2 + 1, (int)round(0.78 * froxelPixels));
    const int o4 = max(o3 + 1, (int)round(1.15 * froxelPixels));

    const int offsets[4] = { o1, o2, o3, o4 };
    const float intervals[4] = {
        0.5 * (float)o2,
        0.5 * (float)(o3 - o1),
        0.5 * (float)(o4 - o2),
        (float)(o4 - o3),
    };

    const float sigma = max(froxelPixels * 0.65, 1.5);
    const float invTwoSigmaSq = 0.5 / (sigma * sigma);

    const float4 centerSample = scatterTex.Load(int3(pixelPos, 0));
    float4 accum = centerSample * (float)o1;
    float weightSum = (float)o1;

    [unroll]
    for (int i = 0; i < 4; i++)
    {
        const int off = offsets[i];
        const float baseW = intervals[i] * exp(-((float)(off * off)) * invTwoSigmaSq);

        [unroll]
        for (int sign = -1; sign <= 1; sign += 2)
        {
            const int signedOff = sign * off;
            const int2 tapPos = pixelPos + axis * signedOff;
            if (any(tapPos < (int2)0) || any(tapPos >= renderSize))
            {
                continue;
            }

            const float tapDepth = abs(viewDepthTex.Load(int3(getCheckerboardPix(tapPos), 0)).r);
            const float w = baseW * shaftBilateralDepthWeight(tapDepth, refDepth, depthSlope, (float)signedOff);
            accum += scatterTex.Load(int3(tapPos, 0)) * w;
            weightSum += w;
        }
    }

    return weightSum > 1e-6 ? (accum / weightSum) : centerSample;
}

#endif
