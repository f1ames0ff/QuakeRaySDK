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

#define DESC_SET_DEPTH_BOUNDS 0
#define DESC_SET_GLOBAL_UNIFORM 1

#include "ShaderCommonHLSLFunc.hlsli"

[[vk::binding(0, DESC_SET_DEPTH_BOUNDS)]] Texture2D<float4> depthWorldTex;
[[vk::binding(1, DESC_SET_DEPTH_BOUNDS), vk::image_format("r32f")]] RWTexture2D<float> depthBoundsOut;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint volW = 0;
    uint volH = 0;
    depthBoundsOut.GetDimensions(volW, volH);

    const int2 cell = int2(dispatchThreadID.xy);
    if (cell.x >= (int)volW || cell.y >= (int)volH)
    {
        return;
    }

    const int2 renderSize = int2(globalUniform.renderWidth, globalUniform.renderHeight);
    const float nearPlane = max(globalUniform.volumeCameraNear, 0.001);
    const float farPlane = max(globalUniform.volumeCameraFar, nearPlane + 0.001);
    if (renderSize.x <= 0 || renderSize.y <= 0 || volW == 0u || volH == 0u)
    {
        depthBoundsOut[cell] = farPlane;
        return;
    }

    const int px0 = (cell.x * renderSize.x) / (int)volW;
    const int px1 = min(renderSize.x, max(px0 + 1, ((cell.x + 1) * renderSize.x + (int)volW - 1) / (int)volW));
    const int py0 = (cell.y * renderSize.y) / (int)volH;
    const int py1 = min(renderSize.y, max(py0 + 1, ((cell.y + 1) * renderSize.y + (int)volH - 1) / (int)volH));

    float maxDepth = 0.0;
    for (int py = py0; py < py1; py++)
    {
        for (int px = px0; px < px1; px++)
        {
            const int2 cbPix = getCheckerboardPix(int2(px, py));
            const float d = depthWorldTex.Load(int3(cbPix, 0)).x;
            const bool isSky = !(d > 0.0) || d > globalUniform.rayLength;
            const float sampleDepth = isSky ? farPlane : min(d, farPlane);
            maxDepth = max(maxDepth, sampleDepth);
            if (maxDepth >= farPlane)
            {
                depthBoundsOut[cell] = farPlane;
                return;
            }
        }
    }

    depthBoundsOut[cell] = maxDepth;
}
