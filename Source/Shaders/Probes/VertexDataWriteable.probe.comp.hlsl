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

// Hand written counterpart of GLSL/VertexDataWriteable.probe.comp.
//
// The second half of the VertexData probe pair. VertexData.inl wraps VERTEX_BUFFER_WRITEABLE around
// four of its eight buffers, and that is how CmVertexPreprocess.comp declares them, but a single
// probe can only see one of the two spellings, so each one gets its own pair. This one writes to
// those four buffers and calls the two setters of the header; the accessors themselves are pinned
// by VertexData.probe.comp, which stays read-only.

#define DESC_SET_GLOBAL_UNIFORM 0
#define DESC_SET_VERTEX_DATA    1
#define VERTEX_BUFFER_WRITEABLE

#include "ShaderCommonHLSLFunc.hlsli"

#define PROBE_DESC_SET 8

[[vk::binding(0, PROBE_DESC_SET)]] RWStructuredBuffer<float> probeOutput;

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    float v = 0.0;

    // All eight buffers of the set, read where the writeable mode leaves them readable
    v += float(g_staticVertices[0].cluster) + float(g_dynamicVertices[0].lightStyles);
    v += float(staticIndices[0]) + float(dynamicIndices[0]);
    v += geometryInstances[0].model[0][0] + float(geomIndexPrevToCur[0]);
    v += float(g_dynamicVertices_Prev[0].packedColor) + float(prevDynamicIndices[0]);

    // The two setters of the header, and the direct stores they are made of: with the flag defined
    // neither side may declare these four buffers as read only
    setStaticVerticesNormals(0u, float3(0.0, 0.0, 1.0));
    setDynamicVerticesNormals(0u, float3(0.0, 0.0, 1.0));

    g_staticVertices[0].normal = float4(0.0, 0.0, 0.0, 0.0);
    g_dynamicVertices[0].normal = float4(0.0, 0.0, 0.0, 0.0);
    g_dynamicVertices_Prev[0].normal = float4(0.0, 0.0, 0.0, 0.0);
    prevDynamicIndices[0] = 0u;

    v += getDynamicVerticesNormals(0u).x + getPrevDynamicVerticesPositions(0u).y;
    v += float(getVertIndicesDynamic(0u, 0u, 0u).z) + float(getPrevVertIndicesDynamic(0u, 0u, 0u).y);

    probeOutput[0] = v;
}
