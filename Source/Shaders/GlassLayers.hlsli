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

#ifndef GLASS_LAYERS_HLSLI
#define GLASS_LAYERS_HLSLI

bool glassLayerMatches(int2 pix, bool reflection)
{
    const float field = framebufQ2GlassFilter_Sampled.Load(int3(getCheckerboardPix(pix), 0)).a;
    return reflection ? field <= -1.0 : field >= 1.0;
}

float4 reconstructGlassLayer(int2 pix, bool reflection)
{
    if (glassLayerMatches(pix, reflection))
    {
        return sampleGlassLayer(pix, reflection);
    }
    const int2 offsets[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
    const int2 extent = int2(globalUniform.renderWidth, globalUniform.renderHeight);
    float4 sum = (float4)0.0;
    float weight = 0.0;
    for (int i = 0; i < 4; i++)
    {
        const int2 tap = pix + offsets[i];
        if (any(tap < 0) || any(tap >= extent) || !glassLayerMatches(tap, reflection))
        {
            continue;
        }
        sum += sampleGlassLayer(tap, reflection);
        weight += 1.0;
    }
    return sum / max(weight, 1.0);
}

#endif
