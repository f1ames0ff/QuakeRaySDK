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

#if defined(VERTEX_PREPROCESS_PARTIAL_DYNAMIC)

    #define GET_POSITIONS getDynamicVerticesPositions
    #define GET_NORMALS getDynamicVerticesNormals
    #define SET_NORMALS setDynamicVerticesNormals
    #define INDICES dynamicIndices

#elif defined(VERTEX_PREPROCESS_PARTIAL_STATIC_ALL) || defined(VERTEX_PREPROCESS_PARTIAL_STATIC_MOVABLE)

    #define GET_POSITIONS getStaticVerticesPositions
    #define GET_NORMALS getStaticVerticesNormals
    #define SET_NORMALS setStaticVerticesNormals
    #define INDICES staticIndices

#else
    #error
#endif




const int geomIndexOffset = globalUniform.instanceGeomInfoOffset[tlasInstanceIndex / 4][tlasInstanceIndex % 4];
const int geomCount = globalUniform.instanceGeomCount[tlasInstanceIndex / 4][tlasInstanceIndex % 4];

const uint chunkCount = max(1u, dynamicChunkCount);
const uint chunkFirst = (uint(geomCount) * groupID.y) / chunkCount;
const uint chunkLast = (uint(geomCount) * (groupID.y + 1u)) / chunkCount;

for (uint localGeomIndex = chunkFirst; localGeomIndex < chunkLast; localGeomIndex++)
{
    const ShGeometryInstance inst = geometryInstances[geomIndexOffset + localGeomIndex];

#if defined(VERTEX_PREPROCESS_PARTIAL_STATIC_MOVABLE)
    const bool isMovable = (inst.flags & GEOM_INST_FLAG_IS_MOVABLE) != 0;

    if (!isMovable)
    {
        continue;
    }
#endif

    const bool useIndices = inst.baseIndexIndex != UINT32_MAX;
    const bool genNormals = (inst.flags & GEOM_INST_FLAG_GENERATE_NORMALS) != 0;
    if (!genNormals)
    {
        continue;
    }

    const float normalSign = float((inst.flags & GEOM_INST_FLAG_INVERTED_NORMALS) == 0) * 2.0 - 1.0;
    const uint triCount = useIndices ? (inst.indexCount / 3) : (inst.vertexCount / 3);

    // Cooperate across all workgroup lanes on each geometry's triangles so dynamic BLAS meshes
    // smaller than the group thread count don't serialize on 1 lane per geometry.
    for (uint tri = groupThreadID.x; tri < triCount; tri += COMPUTE_VERT_PREPROC_GROUP_SIZE_X)
    {
        const uint3 vertexIndices = useIndices
            ? uint3(inst.baseVertexIndex + INDICES[inst.baseIndexIndex + tri * 3 + 0],
                    inst.baseVertexIndex + INDICES[inst.baseIndexIndex + tri * 3 + 1],
                    inst.baseVertexIndex + INDICES[inst.baseIndexIndex + tri * 3 + 2])
            : uint3(inst.baseVertexIndex + tri * 3 + 0,
                    inst.baseVertexIndex + tri * 3 + 1,
                    inst.baseVertexIndex + tri * 3 + 2);

        const float3 localPos[3] =
        {
            GET_POSITIONS(vertexIndices[0]),
            GET_POSITIONS(vertexIndices[1]),
            GET_POSITIONS(vertexIndices[2])
        };

        const float3 rawNormal = cross(localPos[1] - localPos[0], localPos[2] - localPos[0]);
        const float maxComp = max(abs(rawNormal.x), max(abs(rawNormal.y), abs(rawNormal.z)));
        const float3 unitNormal = (all(isfinite(rawNormal)) && maxComp > 1.0e-20)
            ? ((rawNormal / maxComp) * rsqrt(dot(rawNormal / maxComp, rawNormal / maxComp)))
            : float3(0.0, 0.0, 1.0);
        const float3 localNormal = normalSign * unitNormal;

        SET_NORMALS(vertexIndices[0], localNormal);
        SET_NORMALS(vertexIndices[1], localNormal);
        SET_NORMALS(vertexIndices[2], localNormal);
    }
}


#undef GET_POSITIONS
#undef GET_NORMALS
#undef SET_NORMALS
#undef INDICES

#undef VERTEX_PREPROCESS_PARTIAL_STATIC_ALL
#undef VERTEX_PREPROCESS_PARTIAL_STATIC_MOVABLE
#undef VERTEX_PREPROCESS_PARTIAL_DYNAMIC
