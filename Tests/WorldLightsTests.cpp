#include "WorldLights.h"

#include <cstdio>

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

    return 0;
}
