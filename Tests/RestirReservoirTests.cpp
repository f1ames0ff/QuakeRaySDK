#include "RestirReservoir.h"

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

bool NearlyEqual(float a, float b, float epsilon = 1e-5f)
{
    return std::fabs(a - b) <= epsilon * (1.0f + std::fabs(a) + std::fabs(b));
}

qray::RestirReservoir MakeReservoir(uint32_t lightIndex, uint32_t sampleCount,
                                    float weightSum, float selectedTarget)
{
    qray::RestirReservoir reservoir;
    reservoir.lightIndex = lightIndex;
    reservoir.sampleCount = sampleCount;
    reservoir.weightSum = weightSum;
    reservoir.selectedTarget = selectedTarget;
    return reservoir;
}

void TestEmptyReservoir()
{
    const qray::RestirReservoir reservoir;

    CHECK(!qray::RestirIsUsable(reservoir), "a zero reservoir must not be usable");
    CHECK(qray::RestirWeight(reservoir) == 0.0f, "a zero reservoir must report a zero weight");
}

void TestSingleSampleMatchesRis()
{
    // The legacy global RIS picks from `count` uniform candidates and reports
    // pdf = target / (count * meanTarget). With target == 5, count == 10 and one candidate the
    // reservoir weight has to be 1 / pdf == count.
    qray::RestirReservoir reservoir;
    qray::RestirAddSample(reservoir, 3u, 5.0f * 10.0f, 5.0f, 0.0f);

    CHECK(reservoir.lightIndex == 3u, "the only sample must be selected");
    CHECK(reservoir.sampleCount == 1u, "one added sample must count once");
    CHECK(NearlyEqual(qray::RestirWeight(reservoir), 10.0f), "one uniform candidate over ten lights must weight ten");
}

void TestMultiCandidateMatchesRis()
{
    // Three candidates over ten lights, targets 5, 0 and 2. The legacy loop's mean target is
    // 7 / 3 and its selected target is 5, so 1 / pdf == 10 * (7 / 3) / 5. The reservoir has to
    // reproduce that value exactly after the same draw.
    qray::RestirReservoir reservoir;
    qray::RestirAddSample(reservoir, 1u, 5.0f * 10.0f, 5.0f, 0.0f);
    qray::RestirAddSample(reservoir, 2u, 0.0f, 0.0f, 0.0f);
    qray::RestirAddSample(reservoir, 3u, 2.0f * 10.0f, 2.0f, 0.99f);

    const float expected = 10.0f * ((5.0f + 0.0f + 2.0f) / 3.0f) / 5.0f;

    CHECK(reservoir.lightIndex == 1u, "a draw above the last sample's share must keep the first sample");
    CHECK(reservoir.sampleCount == 3u, "all three candidates must count even when their target is zero");
    CHECK(NearlyEqual(qray::RestirWeight(reservoir), expected), "the reservoir weight must match the RIS pdf");
}

void TestReservoirDrawReplacesRepresentative()
{
    qray::RestirReservoir reservoir;
    qray::RestirAddSample(reservoir, 1u, 50.0f, 5.0f, 0.0f);
    qray::RestirAddSample(reservoir, 2u, 0.0f, 0.0f, 0.0f);

    CHECK(reservoir.lightIndex == 1u, "a zero-weight candidate must never be selected");

    qray::RestirAddSample(reservoir, 3u, 20.0f, 2.0f, 0.1f);

    CHECK(reservoir.lightIndex == 3u, "a draw inside the new sample's share must select it");
    CHECK(NearlyEqual(reservoir.selectedTarget, 2.0f), "the selected target must follow the new sample");
}

void TestTemporalMerge()
{
    const qray::RestirReservoir history = MakeReservoir(7u, 3u, 70.0f, 5.0f);
    qray::RestirReservoir       current = MakeReservoir(1u, 3u, 70.0f, 5.0f);

    // The history's selected light has target 6 at the new pixel, so its contribution is
    // 6 * 70 / 5 = 84 and it brings three source samples.
    qray::RestirMerge(current, history, 6.0f, 0.0f);

    CHECK(current.lightIndex == 7u, "a zero draw must take the merged history sample");
    CHECK(current.sampleCount == 6u, "the merged history must add its source-sample count");
    CHECK(NearlyEqual(current.weightSum, 154.0f), "the merged weight sum must use the target ratio");
    CHECK(NearlyEqual(current.selectedTarget, 6.0f), "the selected target must be evaluated at the new pixel");
    CHECK(NearlyEqual(qray::RestirWeight(current), 154.0f / (6.0f * 6.0f)),
          "the merged weight must use the combined sample count");
}

void TestTemporalMergeKeepsCurrentOnHighDraw()
{
    const qray::RestirReservoir history = MakeReservoir(7u, 3u, 30.0f, 5.0f);
    qray::RestirReservoir       current = MakeReservoir(1u, 2u, 100.0f, 4.0f);

    qray::RestirMerge(current, history, 5.0f, 0.99f);

    CHECK(current.lightIndex == 1u, "a high draw must keep the current representative");
    CHECK(current.sampleCount == 5u, "a rejected representative still contributes its samples");
    CHECK(NearlyEqual(current.weightSum, 130.0f), "a rejected representative still contributes its weight");
    CHECK(NearlyEqual(current.selectedTarget, 4.0f), "a rejected representative must not change the target");
}

void TestMergeRejectsUnusableSources()
{
    qray::RestirReservoir current = MakeReservoir(1u, 2u, 100.0f, 4.0f);
    const qray::RestirReservoir zeroCount = MakeReservoir(7u, 0u, 30.0f, 5.0f);
    const qray::RestirReservoir zeroTarget = MakeReservoir(7u, 2u, 30.0f, 0.0f);
    const qray::RestirReservoir nanWeight = MakeReservoir(7u, 2u, NAN, 5.0f);
    const qray::RestirReservoir tooMany = MakeReservoir(7u, qray::kRestirMaxSamples + 1u, 30.0f, 5.0f);

    qray::RestirMerge(current, zeroCount, 5.0f, 0.0f);
    qray::RestirMerge(current, zeroTarget, 5.0f, 0.0f);
    qray::RestirMerge(current, nanWeight, 5.0f, 0.0f);
    qray::RestirMerge(current, tooMany, 5.0f, 0.0f);
    qray::RestirMerge(current, MakeReservoir(7u, 2u, 30.0f, 5.0f), NAN, 0.0f);

    CHECK(current.sampleCount == 2u, "invalid merges must not add samples");
    CHECK(NearlyEqual(current.weightSum, 100.0f), "invalid merges must not add weight");
    CHECK(current.lightIndex == 1u, "invalid merges must not replace the representative");
}

void TestClampPreservesWeight()
{
    qray::RestirReservoir reservoir = MakeReservoir(2u, 64u, 100.0f, 2.0f);
    const float          before = qray::RestirWeight(reservoir);

    qray::RestirClampSamples(reservoir, 32u);

    CHECK(reservoir.sampleCount == 32u, "the clamp must bound the sample count");
    CHECK(NearlyEqual(reservoir.weightSum, 50.0f), "the clamp must scale the weight sum with the count");
    CHECK(NearlyEqual(qray::RestirWeight(reservoir), before), "the clamp must not change the estimator");
    CHECK(NearlyEqual(reservoir.selectedTarget, 2.0f), "the clamp must keep the selected target");
}

void TestClampIgnoresShortHistory()
{
    qray::RestirReservoir reservoir = MakeReservoir(2u, 8u, 10.0f, 2.0f);
    qray::RestirClampSamples(reservoir, 32u);

    CHECK(reservoir.sampleCount == 8u, "a history below the cap must stay untouched");
    CHECK(NearlyEqual(reservoir.weightSum, 10.0f), "a history below the cap keeps its weight sum");
}

void TestRepeatedMergeAccumulates()
{
    qray::RestirReservoir current = MakeReservoir(1u, 1u, 10.0f, 1.0f);
    const qray::RestirReservoir same = MakeReservoir(1u, 1u, 10.0f, 1.0f);

    for (int i = 0; i < 7; i++)
    {
        qray::RestirMerge(current, same, 1.0f, 0.99f);
    }

    CHECK(current.sampleCount == 8u, "each merge must add one source sample");
    CHECK(NearlyEqual(current.weightSum, 80.0f), "each merge must add the target ratio");
    CHECK(NearlyEqual(qray::RestirWeight(current), 10.0f), "identical merges must keep the estimator");
}

}

int main()
{
    TestEmptyReservoir();
    TestSingleSampleMatchesRis();
    TestMultiCandidateMatchesRis();
    TestReservoirDrawReplacesRepresentative();
    TestTemporalMerge();
    TestTemporalMergeKeepsCurrentOnHighDraw();
    TestMergeRejectsUnusableSources();
    TestClampPreservesWeight();
    TestClampIgnoresShortHistory();
    TestRepeatedMergeAccumulates();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
