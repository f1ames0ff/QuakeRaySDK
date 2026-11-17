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

void TestTargetFunction()
{
    const float normal[3] = { 0.0f, 0.0f, 1.0f };
    const float frontDir[3] = { 0.0f, 0.6f, 0.8f };
    const float orthogonalDir[3] = { 1.0f, 0.0f, 0.0f };
    const float backDir[3] = { 0.0f, 0.0f, -1.0f };
    const float radiance[3] = { 1.0f, 2.0f, 3.0f };

    const float expectedLum = 0.2125f * 1.0f + 0.7154f * 2.0f + 0.0721f * 3.0f;
    const float target = qray::RestirGiTarget(radiance, normal, frontDir);
    CHECK(std::fabs(target - expectedLum * 0.8f) < 1e-6f,
          "target must equal luminance times positive cosine term");

    CHECK(qray::RestirGiTarget(radiance, normal, orthogonalDir) == 0.0f,
          "orthogonal direction must have zero target");
    CHECK(qray::RestirGiTarget(radiance, normal, backDir) == 0.0f,
          "back-facing direction must have zero target");

    const float zeroRad[3] = { 0.0f, 0.0f, 0.0f };
    const float negRad[3] = { -1.0f, 0.0f, 0.0f };
    const float nanRad[3] = { NAN, 1.0f, 1.0f };
    CHECK(qray::RestirGiTarget(zeroRad, normal, frontDir) == 0.0f, "zero radiance gives zero target");
    CHECK(qray::RestirGiTarget(negRad, normal, frontDir) == 0.0f, "negative radiance gives zero target");
    CHECK(qray::RestirGiTarget(nanRad, normal, frontDir) == 0.0f, "non-finite radiance gives zero target");
}

void TestSingleSampleWeightMatchesOneOverPdf()
{
    const float normal[3] = { 0.0f, 0.0f, 1.0f };
    const float dir[3] = { 0.0f, 0.6f, 0.8f };
    const float radiance[3] = { 2.0f, 3.0f, 4.0f };
    const float oneOverPdf = 3.5f;

    const float target = qray::RestirGiTarget(radiance, normal, dir);
    qray::RestirGiReservoir reservoir;
    reservoir.radiance[3] = 99.0f;
    qray::RestirGiAddSample(reservoir, dir, 12.0f, radiance, target * oneOverPdf, target, 0.0f);

    CHECK(reservoir.radiance[3] == 0.0f, "radiance[3] must stay zeroed padding");
    CHECK(std::fabs(qray::RestirGiWeight(reservoir) - oneOverPdf) < 1e-5f,
          "single-sample W must reproduce oneOverSourcePdf without storing 1/pdf in radiance[3]");
}

void TestTemporalSpatialMergeEstimatorW()
{
    const float normalA[3] = { 0.0f, 0.0f, 1.0f };
    const float dirA[3] = { 0.0f, 0.0f, 1.0f };
    const float radA[3] = { 4.0f, 4.0f, 4.0f };
    const float oneOverPdfA = 2.0f;
    const float targetA = qray::RestirGiTarget(radA, normalA, dirA);

    qray::RestirGiReservoir history;
    qray::RestirGiAddSample(history, dirA, 5.0f, radA, targetA * oneOverPdfA, targetA, 0.0f);

    const float normalB[3] = { 0.0f, 0.6f, 0.8f };
    const float dirB[3] = { 0.0f, 0.6f, 0.8f };
    const float radB[3] = { 2.0f, 2.0f, 2.0f };
    const float oneOverPdfB = 5.0f;
    const float targetB = qray::RestirGiTarget(radB, normalB, dirB);

    qray::RestirGiReservoir current;
    qray::RestirGiAddSample(current, dirB, 7.0f, radB, targetB * oneOverPdfB, targetB, 0.0f);

    const float historyTargetAtB = qray::RestirGiTarget(history.radiance, normalB, history.direction);
    qray::RestirGiMerge(current, history, historyTargetAtB, 0.0f);

    const float expectedW = 0.5f * (oneOverPdfA + (targetB / historyTargetAtB) * oneOverPdfB);
    CHECK(current.sampleCount == 2u, "merge must accumulate both sample counts");
    CHECK(current.direction[2] == 1.0f && current.radiance[3] == 0.0f,
          "u = 0 picks history sample and preserves zero padding");
    CHECK(std::fabs(qray::RestirGiWeight(current) - expectedW) < 1e-5f,
          "combined W must reflect source PDFs across differing receiver normals without radiance[3]");

    // Equal-target merge: two samples from same target at different PDFs yield mean 1/pdf.
    qray::RestirGiReservoir sameCurr;
    const float sameTarget = qray::RestirGiTarget(radA, normalA, dirA);
    qray::RestirGiAddSample(sameCurr, dirA, 5.0f, radA, sameTarget * oneOverPdfB, sameTarget, 0.0f);
    qray::RestirGiMerge(sameCurr, history, sameTarget, 0.0f);
    CHECK(std::fabs(qray::RestirGiWeight(sameCurr) - 0.5f * (oneOverPdfA + oneOverPdfB)) < 1e-5f,
          "equal-target merge must average the two 1/pdf estimators");
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
    TestTargetFunction();
    TestSingleSampleWeightMatchesOneOverPdf();
    TestTemporalSpatialMergeEstimatorW();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
