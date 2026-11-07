#include "LightVolume.h"

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

bool NearlyEqual(float a, float b, float epsilon = 1e-4f)
{
    return std::fabs(a - b) <= epsilon * (1.0f + std::fabs(a) + std::fabs(b));
}

void TestDefaults()
{
    qray::LightVolume volume;
    const qray::LightVolume::Stats &stats = volume.GetStats();

    CHECK(qray::LightVolume::kProbeCount == 152576u, "the probe count must be 6 * 16^3 + 128 * 10^3");
    CHECK(stats.cascades == 6u, "six cascades");
    CHECK(stats.probesPerCascade == 4096u, "16^3 probes per cascade");
    CHECK(stats.localVolumes == qray::LightVolume::kMaxLocalVolumes, "the local slots are reported");
    CHECK(stats.activeLocalVolumes == 0u, "a reset volume has no active local volume");
    CHECK(stats.baseBounds == qray::LightVolume::kBaseBounds, "the base bounds are 1280 u");
}

void TestCameraSnap()
{
    qray::LightVolume volume;
    const float origin[3] = { 700.0f, -700.0f, 0.0f };
    float snapped[3];

    volume.SetCameraOrigin(origin);
    volume.GetCameraOrigin(snapped);

    CHECK(NearlyEqual(snapped[0], 640.0f), "x must snap to 640");
    CHECK(NearlyEqual(snapped[1], -640.0f), "y must snap to -640");
    CHECK(NearlyEqual(snapped[2], 0.0f), "z must stay at zero");
}

void TestCascadeSelection()
{
    qray::LightVolume volume;
    const float origin[3] = { 0.0f, 0.0f, 0.0f };
    volume.SetCameraOrigin(origin);

    const float originPoint[3] = { 0.0f, 0.0f, 0.0f };
    const float insideZero[3] = { 600.0f, 0.0f, 0.0f };
    const float edgeZero[3] = { 640.0f, 0.0f, 0.0f };
    const float cascadeOne[3] = { 700.0f, 0.0f, 0.0f };
    const float cascadeTwo[3] = { 1300.0f, 0.0f, 0.0f };
    const float cascadeThree[3] = { 3000.0f, 0.0f, 0.0f };
    const float outside[3] = { 50000.0f, 0.0f, 0.0f };

    CHECK(volume.FindCascade(originPoint) == 0, "the origin must be in cascade 0");
    CHECK(volume.FindCascade(insideZero) == 0, "600 u must be in cascade 0");
    CHECK(volume.FindCascade(edgeZero) == 0, "the cascade boundary is inclusive");
    CHECK(volume.FindCascade(cascadeOne) == 1, "700 u must be in cascade 1");
    CHECK(volume.FindCascade(cascadeTwo) == 2, "1300 u must be in cascade 2");
    CHECK(volume.FindCascade(cascadeThree) == 3, "3000 u must be in cascade 3");
    CHECK(volume.FindCascade(outside) == -1, "a point outside all cascades must be rejected");
}

void TestProbePositions()
{
    qray::LightVolume volume;
    const float origin[3] = { 0.0f, 0.0f, 0.0f };
    volume.SetCameraOrigin(origin);

    const uint32_t probe = volume.GetCascadeProbeIndex(0, 0, 0, 0);
    float position[3];
    float cellSize = 0.0f;

    CHECK(volume.GetProbePosition(probe, position, cellSize), "the first probe must resolve");
    CHECK(NearlyEqual(cellSize, 80.0f), "cascade 0 must have 80 u cells");
    CHECK(NearlyEqual(position[0], -600.0f), "the first probe must sit half a cell inside the boundary");
    CHECK(NearlyEqual(position[1], -600.0f), "the first probe must sit half a cell inside the boundary");
    CHECK(NearlyEqual(position[2], -600.0f), "the first probe must sit half a cell inside the boundary");

    const uint32_t last = volume.GetCascadeProbeIndex(0, 15, 15, 15);
    CHECK(volume.GetProbePosition(last, position, cellSize), "the last cascade probe must resolve");
    CHECK(NearlyEqual(position[0], 600.0f), "the last probe must sit half a cell inside the boundary");

    const uint32_t center = volume.GetCascadeProbeIndex(0, 8, 8, 8);
    CHECK(volume.GetProbePosition(center, position, cellSize), "the center probe must resolve");
    CHECK(NearlyEqual(position[0], 40.0f), "the center probe must be one cell away from the origin");

    const uint32_t farProbe = volume.GetCascadeProbeIndex(5, 0, 0, 0);
    CHECK(volume.GetProbePosition(farProbe, position, cellSize), "the last cascade probe must resolve");
    CHECK(NearlyEqual(cellSize, 2560.0f), "cascade 5 must have 2560 u cells");
}

void TestTrilinearWeights()
{
    qray::LightVolume volume;
    const float origin[3] = { 0.0f, 0.0f, 0.0f };
    volume.SetCameraOrigin(origin);

    const float probeCenter[3] = { 40.0f, 40.0f, 40.0f };
    uint32_t cascade = 0;
    uint32_t base = 0;
    float weights[2][2][2] = {};

    CHECK(volume.GetCascadeLookup(probeCenter, cascade, base, weights), "a probe centre must resolve");
    CHECK(cascade == 0u, "the centre must stay in cascade 0");
    CHECK(base == volume.GetCascadeProbeIndex(0, 8, 8, 8), "the base corner must be the centre probe");
    CHECK(NearlyEqual(weights[0][0][0], 1.0f), "a probe centre must give all weight to its corner");
    CHECK(NearlyEqual(weights[1][1][1], 0.0f), "the far corner must have no weight");

    const float between[3] = { 80.0f, 40.0f, 40.0f };
    CHECK(volume.GetCascadeLookup(between, cascade, base, weights), "a point between probes must resolve");

    float sum = 0.0f;

    for (const auto &plane : weights)
    {
        for (const auto &row : plane)
        {
            for (float weight : row)
            {
                sum += weight;
            }
        }
    }

    CHECK(NearlyEqual(sum, 1.0f), "the trilinear weights must sum to one");
    CHECK(NearlyEqual(weights[0][0][0], 0.5f), "the point halfway along x must split the two x corners");
    CHECK(NearlyEqual(weights[0][0][1], 0.5f), "the point halfway along x must split the two x corners");

    const float outside[3] = { 10000.0f, 0.0f, 0.0f };
    CHECK(!volume.GetCascadeLookup(outside, cascade, base, weights) || cascade > 0u,
          "a far point must not resolve as cascade 0");
}

void TestLocalVolume()
{
    qray::LightVolume volume;
    const float origin[3] = { 0.0f, 0.0f, 0.0f };
    const float localOrigin[3] = { 100.0f, 0.0f, 0.0f };
    volume.SetCameraOrigin(origin);
    volume.SetLocalVolume(2, localOrigin, 100.0f, true);

    CHECK(volume.GetStats().activeLocalVolumes == 1u, "one local volume must be active");

    const float inside[3] = { 100.0f, 0.0f, 0.0f };
    const float outside[3] = { 400.0f, 0.0f, 0.0f };

    CHECK(volume.FindLocalVolume(inside) == 2, "the point inside must find local volume 2");
    CHECK(volume.FindLocalVolume(outside) == -1, "the point outside must not find a local volume");

    uint32_t localIndex = 0;
    uint32_t base = 0;
    float weights[2][2][2] = {};

    CHECK(volume.GetLocalLookup(inside, localIndex, base, weights), "the local lookup must resolve");
    CHECK(localIndex == 2u, "the local lookup must name volume 2");
    CHECK(base == volume.GetLocalProbeIndex(2, 4, 4, 4),
          "the local base corner must be the cell containing the centre");
}

void TestLocalProbeIndex()
{
    qray::LightVolume volume;
    const uint32_t first = volume.GetLocalProbeIndex(0, 0, 0, 0);
    const uint32_t last = volume.GetLocalProbeIndex(127, 9, 9, 9);

    CHECK(first == qray::LightVolume::kCascadeProbeCount, "local probes start after the cascades");
    CHECK(last < qray::LightVolume::kProbeCount, "the last local probe must be inside the storage");
    CHECK(volume.GetLocalProbeIndex(1, 0, 0, 0) == first + qray::LightVolume::kProbesPerLocalVolume,
          "local volumes must be contiguous");
}

void TestLocalPreference()
{
    qray::LightVolume volume;
    const float origin[3] = { 0.0f, 0.0f, 0.0f };
    const float localOrigin[3] = { 100.0f, 0.0f, 0.0f };
    volume.SetCameraOrigin(origin);
    volume.SetLocalVolume(0, localOrigin, 100.0f, true);

    for (uint32_t index = 0; index < qray::LightVolume::kCascadeProbeCount; index++)
    {
        volume.Probe(index).sh[0][0] = 2.0f;
    }

    for (uint32_t index = 0; index < qray::LightVolume::kProbesPerLocalVolume; index++)
    {
        volume.Probe(qray::LightVolume::kCascadeProbeCount + index).sh[0][0] = 1.0f;
    }

    const float normal[3] = { 0.0f, 0.0f, 1.0f };
    const float localPoint[3] = { 100.0f, 0.0f, 0.0f };
    const float cascadePoint[3] = { 400.0f, 0.0f, 0.0f };
    float localColor[3];
    float cascadeColor[3];

    CHECK(volume.SampleIrradiance(localPoint, normal, localColor), "the local point must sample");
    CHECK(volume.SampleIrradiance(cascadePoint, normal, cascadeColor), "the cascade point must sample");
    CHECK(NearlyEqual(localColor[0], 0.886227f), "the local volume must win inside its bounds");
    CHECK(NearlyEqual(cascadeColor[0], 1.772454f), "the cascade must be used outside the local volume");
}

void TestProbeCornerMapping()
{
    qray::LightVolume volume;
    const float origin[3] = { 0.0f, 0.0f, 0.0f };
    volume.SetCameraOrigin(origin);

    volume.Probe(volume.GetCascadeProbeIndex(0, 8, 8, 8)).sh[0][0] = 1.0f;
    volume.Probe(volume.GetCascadeProbeIndex(0, 9, 8, 8)).sh[0][0] = 2.0f;
    volume.Probe(volume.GetCascadeProbeIndex(0, 8, 9, 8)).sh[0][0] = 3.0f;
    volume.Probe(volume.GetCascadeProbeIndex(0, 8, 8, 9)).sh[0][0] = 4.0f;

    const float normal[3] = { 0.0f, 0.0f, 1.0f };
    const float centre[3] = { 40.0f, 40.0f, 40.0f };
    const float betweenX[3] = { 80.0f, 40.0f, 40.0f };
    float colour[3];

    CHECK(volume.SampleIrradiance(centre, normal, colour), "the centre sample must resolve");
    CHECK(NearlyEqual(colour[0], 0.886227f), "the centre must read probe (8,8,8)");

    CHECK(volume.SampleIrradiance(betweenX, normal, colour), "the halfway sample must resolve");
    CHECK(NearlyEqual(colour[0], 0.5f * 0.886227f + 0.5f * 1.772454f),
          "the halfway x sample must blend probes (8,8,8) and (9,8,8)");

    const float betweenY[3] = { 40.0f, 80.0f, 40.0f };
    const float betweenZ[3] = { 40.0f, 40.0f, 80.0f };

    CHECK(volume.SampleIrradiance(betweenY, normal, colour), "the y sample must resolve");
    CHECK(NearlyEqual(colour[0], 0.5f * 0.886227f + 0.5f * 2.658681f),
          "the halfway y sample must blend probes (8,8,8) and (8,9,8)");

    CHECK(volume.SampleIrradiance(betweenZ, normal, colour), "the z sample must resolve");
    CHECK(NearlyEqual(colour[0], 0.5f * 0.886227f + 0.5f * 3.544908f),
          "the halfway z sample must blend probes (8,8,8) and (8,8,9)");
}

void TestShConstantEnvironment()
{
    const float directions[6][3] =
    {
        {  1.0f,  0.0f,  0.0f },
        { -1.0f,  0.0f,  0.0f },
        {  0.0f,  1.0f,  0.0f },
        {  0.0f, -1.0f,  0.0f },
        {  0.0f,  0.0f,  1.0f },
        {  0.0f,  0.0f, -1.0f },
    };
    const float radiance[3] = { 1.0f, 2.0f, 3.0f };

    float sh[4][3] = {};

    for (const float *direction : directions)
    {
        qray::LightVolume::AccumulateRadiance(direction, radiance, sh);
    }

    qray::LightVolume::NormalizeSh(sh, 6);

    const float normal[3] = { 0.0f, 0.0f, 1.0f };
    float irradiance[3];
    qray::LightVolume::EvaluateIrradiance(sh, normal, irradiance);

    CHECK(NearlyEqual(irradiance[0], 3.14159265f * 1.0f), "constant red radiance must give pi * red");
    CHECK(NearlyEqual(irradiance[1], 3.14159265f * 2.0f), "constant green radiance must give pi * green");
    CHECK(NearlyEqual(irradiance[2], 3.14159265f * 3.0f), "constant blue radiance must give pi * blue");

    const float otherNormal[3] = { -1.0f, 0.0f, 0.0f };
    qray::LightVolume::EvaluateIrradiance(sh, otherNormal, irradiance);
    CHECK(NearlyEqual(irradiance[0], 3.14159265f * 1.0f), "a constant environment must be direction independent");
}

void TestShDirectionalLobe()
{
    float sh[4][3] = {};
    const float direction[3] = { 0.0f, 0.0f, 1.0f };
    const float radiance[3] = { 1.0f, 1.0f, 1.0f };

    qray::LightVolume::AccumulateRadiance(direction, radiance, sh);
    qray::LightVolume::NormalizeSh(sh, 1);

    const float up[3] = { 0.0f, 0.0f, 1.0f };
    const float down[3] = { 0.0f, 0.0f, -1.0f };
    float upIrradiance[3];
    float downIrradiance[3];

    qray::LightVolume::EvaluateIrradiance(sh, up, upIrradiance);
    qray::LightVolume::EvaluateIrradiance(sh, down, downIrradiance);

    CHECK(upIrradiance[0] > downIrradiance[0], "the lobe must favour its own direction");
    CHECK(upIrradiance[0] > 0.0f, "the lobe must be positive along its direction");
    CHECK(downIrradiance[0] < 0.0f, "the opposite direction must not be brighter than zero");
}

void TestShZeroSamples()
{
    float sh[4][3];

    for (auto &band : sh)
    {
        for (float &channel : band)
        {
            channel = 5.0f;
        }
    }

    qray::LightVolume::NormalizeSh(sh, 0);

    for (const auto &band : sh)
    {
        for (float channel : band)
        {
            CHECK(channel == 0.0f, "zero samples must clear the coefficients");
        }
    }
}

void TestUpdateSchedule()
{
    qray::LightVolume volume;
    uint32_t cascade = 0;
    int32_t local = 0;

    volume.GetUpdateSchedule(0, cascade, local);
    CHECK(cascade == 0u && local == -1, "frame 0 must update cascade 0 and no local volume");

    volume.GetUpdateSchedule(7, cascade, local);
    CHECK(cascade == 1u && local == -1, "the cascade must wrap modulo six");

    const float localTwo[3] = { 100.0f, 0.0f, 0.0f };
    const float localFive[3] = { 500.0f, 0.0f, 0.0f };
    volume.SetLocalVolume(2, localTwo, 100.0f, true);
    volume.SetLocalVolume(5, localFive, 100.0f, true);

    volume.GetUpdateSchedule(0, cascade, local);
    CHECK(cascade == 0u && local == 2, "frame 0 must pick the first active local volume");

    volume.GetUpdateSchedule(1, cascade, local);
    CHECK(cascade == 1u && local == 5, "frame 1 must pick the second active local volume");

    volume.GetUpdateSchedule(2, cascade, local);
    CHECK(cascade == 2u && local == 2, "the local schedule must wrap independently");

    volume.GetUpdateSchedule(7, cascade, local);
    CHECK(cascade == 1u && local == 5, "frame 7 must wrap both schedules");
}

void TestProbeAccess()
{
    qray::LightVolume volume;
    const uint32_t last = qray::LightVolume::kProbeCount - 1;

    volume.Probe(last).sh[3][2] = 7.0f;
    CHECK(NearlyEqual(volume.Probe(last).sh[3][2], 7.0f), "the last probe must be writable");

    float position[3];
    float cellSize = 0.0f;
    CHECK(!volume.GetProbePosition(qray::LightVolume::kProbeCount, position, cellSize),
          "a probe past the storage must be rejected");
}

}

int main()
{
    TestDefaults();
    TestCameraSnap();
    TestCascadeSelection();
    TestProbePositions();
    TestTrilinearWeights();
    TestLocalVolume();
    TestLocalProbeIndex();
    TestLocalPreference();
    TestProbeCornerMapping();
    TestShConstantEnvironment();
    TestShDirectionalLobe();
    TestShZeroSamples();
    TestUpdateSchedule();
    TestProbeAccess();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
