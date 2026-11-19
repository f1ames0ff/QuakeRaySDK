#include "TransparencyFroxelVolume.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace
{

int g_failures = 0;
int g_checks = 0;

#define CHECK(condition, ...)                          \
    do                                                 \
    {                                                  \
        g_checks++;                                    \
        if (!(condition))                              \
        {                                              \
            g_failures++;                              \
            std::printf("FAIL: ");                     \
            std::printf(__VA_ARGS__);                  \
            std::printf("\n");                         \
        }                                              \
    } while (0)

bool NearlyEqual(float a, float b, float epsilon = 1e-3f)
{
    return std::fabs(a - b) <= epsilon * (1.0f + std::fabs(a) + std::fabs(b));
}

void TestLayout()
{
    CHECK(qray::TransparencyFroxelVolume::kDimX == 64u, "64 froxels on X");
    CHECK(qray::TransparencyFroxelVolume::kDimY == 32u, "32 froxels on Y");
    CHECK(qray::TransparencyFroxelVolume::kDimZ == 64u, "64 froxels on Z");
    CHECK(qray::TransparencyFroxelVolume::kProbeCount == 131072u, "64 * 32 * 64 probes");
    CHECK(sizeof(qray::TransparencyFroxelProbe) == 48u, "the probe keeps the 48-byte SH4 layout");
}

void TestBounds()
{
    qray::TransparencyFroxelVolume volume;
    const float origin[3] = { -100.0f, -200.0f, -300.0f };
    const float size[3] = { 1024.0f, 512.0f, 2048.0f };
    float outOrigin[3] = {};
    float outSize[3] = {};

    volume.SetBounds(origin, size);
    CHECK(volume.GetBounds(outOrigin, outSize), "the bounds are readable");
    CHECK(NearlyEqual(outOrigin[0], -100.0f) && NearlyEqual(outOrigin[1], -200.0f) &&
              NearlyEqual(outOrigin[2], -300.0f),
          "the origin round-trips");
    CHECK(NearlyEqual(outSize[0], 1024.0f) && NearlyEqual(outSize[1], 512.0f) &&
              NearlyEqual(outSize[2], 2048.0f),
          "the size round-trips");
}

void TestOutside()
{
    qray::TransparencyFroxelVolume volume;
    const float origin[3] = { 0.0f, 0.0f, 0.0f };
    const float size[3] = { 64.0f, 32.0f, 64.0f };
    volume.SetBounds(origin, size);

    const float outside[3] = { -100.0f, 0.0f, 0.0f };
    const float normal[3] = { 0.0f, 1.0f, 0.0f };
    float rgb[3] = {};

    CHECK(!volume.SampleIrradiance(outside, normal, rgb), "a point outside the box is rejected");
    CHECK(volume.SampleIrradiance(origin, normal, rgb), "the box corner is inside");
}

void TestProbeConstants()
{
    qray::TransparencyFroxelVolume volume;
    const float origin[3] = { 0.0f, 0.0f, 0.0f };
    const float size[3] = { 640.0f, 320.0f, 640.0f };
    volume.SetBounds(origin, size);

    // A constant environment in every froxel: L0 = 4*pi*0.282095*radiance.
    for (uint32_t z = 0; z < qray::TransparencyFroxelVolume::kDimZ; z++)
    {
        for (uint32_t y = 0; y < qray::TransparencyFroxelVolume::kDimY; y++)
        {
            for (uint32_t x = 0; x < qray::TransparencyFroxelVolume::kDimX; x++)
            {
                qray::TransparencyFroxelProbe &probe = volume.Probe(x, y, z);
                probe.sh[0][0] = 3.5449077f; // 4*pi*0.282095, unit radiance
                probe.sh[0][1] = 3.5449077f;
                probe.sh[0][2] = 3.5449077f;
            }
        }
    }

    const float position[3] = { 320.0f, 160.0f, 320.0f };
    const float normal[3] = { 0.0f, 1.0f, 0.0f };
    float rgb[3] = {};

    CHECK(volume.SampleIrradiance(position, normal, rgb), "the centre sample is inside");
    CHECK(NearlyEqual(rgb[0], 3.1415927f) && NearlyEqual(rgb[1], 3.1415927f) &&
              NearlyEqual(rgb[2], 3.1415927f),
          "a constant environment evaluates to pi * radiance");
}

void TestTrilinearBlend()
{
    qray::TransparencyFroxelVolume volume;
    const float origin[3] = { 0.0f, 0.0f, 0.0f };
    const float size[3] = { 640.0f, 320.0f, 640.0f };
    volume.SetBounds(origin, size);

    for (uint32_t z = 0; z < qray::TransparencyFroxelVolume::kDimZ; z++)
    {
        for (uint32_t y = 0; y < qray::TransparencyFroxelVolume::kDimY; y++)
        {
            for (uint32_t x = 0; x < qray::TransparencyFroxelVolume::kDimX; x++)
            {
                qray::TransparencyFroxelProbe &probe = volume.Probe(x, y, z);
                probe.sh[0][0] = x == 0 ? 1.0f : (x == 1 ? 0.0f : 1.0f);
            }
        }
    }

    const float cellX = size[0] / float(qray::TransparencyFroxelVolume::kDimX);
    const float position[3] = { origin[0] + cellX, 160.0f, 320.0f };
    const float normal[3] = { 0.0f, 1.0f, 0.0f };
    float rgb[3] = {};

    CHECK(volume.SampleIrradiance(position, normal, rgb), "the blend sample is inside");
    CHECK(NearlyEqual(rgb[0], 0.886227f * 0.5f),
          "halfway between the first two froxels blends L0 by half");
}

void TestShHelpers()
{
    float sh[4][3] = {};
    const float direction[3] = { 0.0f, 1.0f, 0.0f };
    const float radiance[3] = { 1.0f, 0.5f, 0.25f };

    qray::TransparencyFroxelVolume::AccumulateRadiance(direction, radiance, sh);
    qray::TransparencyFroxelVolume::NormalizeSh(sh, 1);

    const float up[3] = { 0.0f, 1.0f, 0.0f };
    const float down[3] = { 0.0f, -1.0f, 0.0f };
    float upRgb[3] = {};
    float downRgb[3] = {};

    qray::TransparencyFroxelVolume::EvaluateIrradiance(sh, up, upRgb);
    qray::TransparencyFroxelVolume::EvaluateIrradiance(sh, down, downRgb);

    CHECK(upRgb[0] > downRgb[0], "the directional lobe is brighter along its direction");
    CHECK(downRgb[0] >= 0.0f, "the convolution stays non-negative");
}

}

int main()
{
    TestLayout();
    TestBounds();
    TestOutside();
    TestProbeConstants();
    TestTrilinearBlend();
    TestShHelpers();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
