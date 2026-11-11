#include "RestirGiReservoir.h"

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

void TestLayout()
{
    CHECK(sizeof(qray::RestirGiReservoir) == 48u, "the reservoir stays at 48 bytes");
    CHECK(qray::kRestirGiMaxSamples == 64u, "history caps at 64 source samples");
}

void TestEmpty()
{
    qray::RestirGiReservoir reservoir;
    CHECK(!qray::RestirGiIsUsable(reservoir), "an empty reservoir is not usable");
    CHECK(qray::RestirGiWeight(reservoir) == 0.0f, "an empty reservoir has zero weight");
}

void TestSingleSample()
{
    qray::RestirGiReservoir reservoir;
    const float direction[3] = { 0.0f, 1.0f, 0.0f };
    const float radiance[3] = { 2.0f, 3.0f, 4.0f };

    qray::RestirGiAddSample(reservoir, direction, 128.0f, radiance, 0.5f, 0.5f, 0.25f);

    CHECK(qray::RestirGiIsUsable(reservoir), "a valid sample makes the reservoir usable");
    CHECK(reservoir.sampleCount == 1u, "one source sample");
    CHECK(reservoir.direction[0] == 0.0f && reservoir.direction[1] == 1.0f &&
              reservoir.direction[2] == 0.0f && reservoir.direction[3] == 128.0f,
          "the selected direction and distance round-trip");
    CHECK(reservoir.radiance[0] == 2.0f && reservoir.radiance[1] == 3.0f &&
              reservoir.radiance[2] == 4.0f,
          "the selected radiance round-trips");
    CHECK(qray::RestirGiWeight(reservoir) == 1.0f, "W = sum / (M * target) = 1");
}

void TestRepresentativePick()
{
    qray::RestirGiReservoir reservoir;
    const float left[3] = { -1.0f, 0.0f, 0.0f };
    const float right[3] = { 1.0f, 0.0f, 0.0f };
    const float radiance[3] = { 1.0f, 1.0f, 1.0f };

    qray::RestirGiAddSample(reservoir, left, 10.0f, radiance, 1.0f, 1.0f, 0.5f);
    qray::RestirGiAddSample(reservoir, right, 20.0f, radiance, 1.0f, 1.0f, 0.9f);

    CHECK(reservoir.sampleCount == 2u, "two source samples");
    CHECK(reservoir.weightSum == 2.0f, "the weights accumulate");
    CHECK(reservoir.direction[0] == -1.0f, "u = 0.9 keeps the first representative");
    CHECK(qray::RestirGiWeight(reservoir) == 1.0f, "W is 2 / (2 * 1)");

    qray::RestirGiReservoir second;
    qray::RestirGiAddSample(second, left, 10.0f, radiance, 1.0f, 1.0f, 0.5f);
    qray::RestirGiAddSample(second, right, 20.0f, radiance, 1.0f, 1.0f, 0.1f);

    CHECK(second.direction[0] == 1.0f, "u = 0.1 switches to the second representative");
}

void TestNormalAgeValidation()
{
    const float up[3] = { 0.0f, 0.0f, 1.0f };
    const float down[3] = { 0.0f, 0.0f, -1.0f };
    const float tilted[3] = { 0.0f, 0.3f, 0.9539392f };

    const uint32_t packed = qray::RestirGiPackNormalFrame(up, 100u);
    CHECK(qray::RestirGiUnpackFrame(packed) == 100u, "the frame round-trips");

    float decoded[3];
    qray::RestirGiUnpackNormal(packed, decoded);
    CHECK(decoded[2] > 0.99f, "the octahedral normal round-trips");

    qray::RestirGiReservoir reservoir;
    reservoir.pad = packed;
    CHECK(qray::RestirGiHistoryValid(reservoir, up, 100u), "same normal and frame is valid");
    CHECK(qray::RestirGiHistoryValid(reservoir, tilted, 110u), "a 17 degree tilt is valid");
    CHECK(!qray::RestirGiHistoryValid(reservoir, down, 100u), "an opposite normal is rejected");
    CHECK(!qray::RestirGiHistoryValid(reservoir, up, 100u + qray::kRestirGiMaxAgeFrames + 1u),
          "an old frame is rejected");

    const float side[3] = { 1.0f, 0.0f, 0.0f };
    const uint32_t sidePacked = qray::RestirGiPackNormalFrame(side, 100u);
    reservoir.pad = sidePacked;
    qray::RestirGiUnpackNormal(sidePacked, decoded);
    CHECK(decoded[0] > 0.99f, "a side normal round-trips");
}

void TestAgeTouch()
{
    const float up[3] = { 0.0f, 0.0f, 1.0f };
    qray::RestirGiReservoir reservoir;
    reservoir.pad = qray::RestirGiPackNormalFrame(up, 5u);

    qray::RestirGiTouch(reservoir, 100u);
    CHECK(qray::RestirGiUnpackFrame(reservoir.pad) == 100u, "touch refreshes the frame");

    float decoded[3];
    qray::RestirGiUnpackNormal(reservoir.pad, decoded);
    CHECK(decoded[2] > 0.99f, "touch keeps the packed normal");

    CHECK(qray::RestirGiHistoryValid(reservoir, up, 110u), "a touched reservoir stays valid");
    CHECK(!qray::RestirGiHistoryValid(reservoir, up, 100u + qray::kRestirGiMaxAgeFrames + 1u),
          "the refreshed age still expires");
}

void TestReceiverNormal()
{
    const float up[3] = { 0.0f, 0.0f, 1.0f };
    const float tilted[3] = { 0.0f, 0.6f, 0.8f };
    qray::RestirGiReservoir reservoir;

    qray::RestirGiUpdateReceiver(reservoir, up, 5u);
    CHECK(qray::RestirGiHistoryValid(reservoir, up, 6u), "the receiver normal validates at the next frame");

    qray::RestirGiUpdateReceiver(reservoir, tilted, 100u);

    float decoded[3];
    qray::RestirGiUnpackNormal(reservoir.pad, decoded);
    CHECK(decoded[1] > 0.55f, "the update replaces the receiver normal");
    CHECK(qray::RestirGiUnpackFrame(reservoir.pad) == 100u, "the update refreshes the frame");
    CHECK(qray::RestirGiHistoryValid(reservoir, tilted, 110u), "the new normal validates");
    CHECK(!qray::RestirGiHistoryValid(reservoir, up, 110u), "the old normal is rejected");
}

void TestMerge()
{
    qray::RestirGiReservoir source;
    const float direction[3] = { 0.0f, 0.0f, 1.0f };
    const float radiance[3] = { 0.25f, 0.5f, 0.75f };

    qray::RestirGiAddSample(source, direction, 64.0f, radiance, 1.0f, 1.0f, 0.5f);
    qray::RestirGiAddSample(source, direction, 64.0f, radiance, 1.0f, 1.0f, 0.9f);
    qray::RestirGiClampHistory(source);

    qray::RestirGiReservoir destination;
    qray::RestirGiMerge(destination, source, 2.0f, 0.25f);

    CHECK(destination.sampleCount == 2u, "the merge adds the whole source count");
    CHECK(destination.weightSum == 4.0f, "the merge weight is target(y) * sum / target(x)");
    CHECK(destination.selectedTarget == 2.0f, "the merged representative uses the new target");
    CHECK(destination.direction[2] == 1.0f, "the merged direction is the selected source");
    CHECK(qray::RestirGiWeight(destination) == 1.0f, "W = 4 / (2 * 2)");
}

void TestRejectsBadSources()
{
    qray::RestirGiReservoir destination;
    qray::RestirGiReservoir empty;
    qray::RestirGiMerge(destination, empty, 1.0f, 0.5f);

    CHECK(destination.sampleCount == 0u, "an empty source adds nothing");

    qray::RestirGiReservoir source;
    const float direction[3] = { 1.0f, 0.0f, 0.0f };
    const float radiance[3] = { 1.0f, 1.0f, 1.0f };
    qray::RestirGiAddSample(source, direction, 1.0f, radiance, 1.0f, 1.0f, 0.5f);

    qray::RestirGiMerge(destination, source, 0.0f, 0.5f);
    CHECK(destination.sampleCount == 0u, "a zero target at the new pixel adds nothing");

    qray::RestirGiMerge(destination, source, NAN, 0.5f);
    CHECK(destination.sampleCount == 0u, "a non-finite target adds nothing");
}

void TestClampHistory()
{
    qray::RestirGiReservoir reservoir;
    const float direction[3] = { 0.0f, 1.0f, 0.0f };
    const float radiance[3] = { 1.0f, 1.0f, 1.0f };

    qray::RestirGiAddSample(reservoir, direction, 1.0f, radiance, 1.0f, 1.0f, 0.5f);
    reservoir.sampleCount = 100u;
    reservoir.weightSum = 100.0f;

    const float before = reservoir.weightSum / (float(reservoir.sampleCount) * reservoir.selectedTarget);
    qray::RestirGiClampHistory(reservoir);

    CHECK(reservoir.sampleCount == qray::kRestirGiMaxSamples, "the count is capped");
    CHECK(std::fabs(reservoir.weightSum - 64.0f) < 1e-6f, "the weight sum scales with the cap");
    CHECK(std::fabs(qray::RestirGiWeight(reservoir) - before) < 1e-6f, "W is unchanged by the cap");
}

void TestNonFiniteSample()
{
    qray::RestirGiReservoir reservoir;
    const float direction[3] = { 0.0f, 1.0f, 0.0f };
    const float radiance[3] = { 1.0f, 1.0f, 1.0f };

    qray::RestirGiAddSample(reservoir, direction, 1.0f, radiance, INFINITY, 1.0f, 0.5f);
    CHECK(reservoir.sampleCount == 1u, "the sample count still advances");
    CHECK(reservoir.weightSum == 0.0f, "a non-finite weight is clamped to zero");

    qray::RestirGiAddSample(reservoir, direction, 1.0f, radiance, 0.0f, 0.0f, 0.5f);
    CHECK(reservoir.weightSum == 0.0f, "a zero target adds no weight");
}

}

int main()
{
    TestLayout();
    TestEmpty();
    TestSingleSample();
    TestRepresentativePick();
    TestMerge();
    TestNormalAgeValidation();
    TestAgeTouch();
    TestReceiverNormal();
    TestRejectsBadSources();
    TestClampHistory();
    TestNonFiniteSample();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
