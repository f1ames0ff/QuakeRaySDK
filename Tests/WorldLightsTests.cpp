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

#include "WorldLights.h"

#include <cstdio>
#include <vector>

int main()
{
    const QrFloat3D clusterMin = { { -1.0f, -1.0f, -1.0f } };
    const QrFloat3D clusterMax = { { 1.0f, 1.0f, 1.0f } };
    const uint8_t clusterFlags = 0;

    QrWorldLightsUploadInfo info = {};
    info.numClusters = 1;
    info.pClusterMins = &clusterMin;
    info.pClusterMaxs = &clusterMax;
    info.pClusterFlags = &clusterFlags;
    info.clusterGeneration = 7;

    qray::WorldLights lights;
    lights.Upload(info, nullptr);

    if (lights.GetClusterCount() != 1 ||
        lights.GetPvsRowBytes() != 1 ||
        lights.GetVisDataSize() != 1 ||
        !lights.IsVisUncompressed() ||
        lights.GetGeneration() != 7)
    {
        std::fprintf(stderr, "all-visible PVS shape mismatch\n");
        return 1;
    }

    const uint8_t *vis = lights.GetClusterVis(0);

    if (vis == nullptr || vis[0] != 0xFF)
    {
        std::fprintf(stderr, "all-visible PVS bits missing\n");
        return 1;
    }

    QrWorldLightFace face = {};
    face.firstVertex = 0;
    face.numVertices = 4;

    std::vector<QrWorldLightFace> faces = { face };
    std::vector<QrVertex> vertices(4);

    for (uint32_t i = 0; i < 4; i++)
    {
        vertices[i].position[0] = float(i);
    }

    if (!qray::ReverseWorldLightFaceOrder(faces, vertices) ||
        vertices[0].position[0] != 3.0f || vertices[1].position[0] != 2.0f ||
        vertices[2].position[0] != 1.0f || vertices[3].position[0] != 0.0f)
    {
        std::fprintf(stderr, "mirrored world-light face order was not reversed\n");
        return 1;
    }

    faces[0].firstVertex = 3;
    faces[0].numVertices = 2;

    if (qray::ReverseWorldLightFaceOrder(faces, vertices))
    {
        std::fprintf(stderr, "out-of-range world-light face was accepted\n");
        return 1;
    }

    return 0;
}
