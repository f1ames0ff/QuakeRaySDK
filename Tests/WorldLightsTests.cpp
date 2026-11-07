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
