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

namespace
{
bool TestAllVisiblePvs(const QrFloat3D &clusterMin, const QrFloat3D &clusterMax, uint8_t clusterFlags)
{
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
        return false;
    }

    const uint8_t *vis = lights.GetClusterVis(0);
    if (vis == nullptr || vis[0] != 0xFF)
    {
        std::fprintf(stderr, "all-visible PVS bits missing\n");
        return false;
    }
    return true;
}

bool TestReverseWorldLightFaceOrder()
{
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
        return false;
    }

    faces[0].firstVertex = 3;
    faces[0].numVertices = 2;
    if (qray::ReverseWorldLightFaceOrder(faces, vertices))
    {
        std::fprintf(stderr, "out-of-range world-light face was accepted\n");
        return false;
    }
    return true;
}

bool TestCompressedTailPvs(const QrFloat3D &clusterMin, const QrFloat3D &clusterMax)
{
    // Compressed tail PVS row shorter than uncompressed pvsRowBytes must still resolve, and
    // out-of-range offsets must be rejected with 0 remaining bytes.
    std::vector<QrFloat3D> tailMins(64, clusterMin);
    std::vector<QrFloat3D> tailMaxs(64, clusterMax);
    std::vector<uint8_t> tailFlags(64, 0);
    std::vector<int32_t> tailOffsets(64, 0);
    std::vector<uint8_t> tailVisData(10, 0xFF);
    tailVisData[8] = 0x00;
    tailVisData[9] = 0x08;
    tailOffsets[63] = 8;
    tailOffsets[62] = 10; // out of bounds

    QrWorldLightsUploadInfo tailInfo = {};
    tailInfo.numClusters = 64;
    tailInfo.pClusterMins = tailMins.data();
    tailInfo.pClusterMaxs = tailMaxs.data();
    tailInfo.pClusterFlags = tailFlags.data();
    tailInfo.pVisOffsets = tailOffsets.data();
    tailInfo.pVisData = tailVisData.data();
    tailInfo.visDataSize = static_cast<uint32_t>(tailVisData.size());
    tailInfo.pvsRowBytes = 8;
    tailInfo.clusterGeneration = 8;

    qray::WorldLights tailLights;
    tailLights.Upload(tailInfo, nullptr);

    size_t remainingBytes = 0;
    const uint8_t *tailVis = tailLights.GetClusterVis(63, &remainingBytes);
    if (tailVis == nullptr || remainingBytes != 2 || tailVis[0] != 0x00 || tailVis[1] != 0x08)
    {
        std::fprintf(stderr, "compressed tail PVS row was rejected or had wrong remainingBytes=%zu\n",
                     remainingBytes);
        return false;
    }

    size_t invalidRemaining = 999;
    if (tailLights.GetClusterVis(62, &invalidRemaining) != nullptr || invalidRemaining != 0)
    {
        std::fprintf(stderr, "out-of-range PVS offset was not rejected\n");
        return false;
    }
    return true;
}
} // namespace

int main()
{
    const QrFloat3D clusterMin = { { -1.0f, -1.0f, -1.0f } };
    const QrFloat3D clusterMax = { { 1.0f, 1.0f, 1.0f } };
    const uint8_t clusterFlags = 0;

    if (!TestAllVisiblePvs(clusterMin, clusterMax, clusterFlags) ||
        !TestReverseWorldLightFaceOrder() ||
        !TestCompressedTailPvs(clusterMin, clusterMax))
    {
        return 1;
    }

    return 0;
}
