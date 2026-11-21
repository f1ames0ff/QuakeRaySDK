#include "LightShaftMath.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

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

void TestConeEncoding()
{
    float cone[2] = {};
    CHECK(!qray::EncodeLightShaftCone(0.2f, 0.5f, false, false, nullptr), "null cone output fails");

    CHECK(qray::EncodeLightShaftCone(0.5f, 0.5f, false, false, cone), "equal inner/outer area cone succeeds");
    CHECK(std::isfinite(cone[0]) && std::isfinite(cone[1]) && cone[0] > cone[1],
          "equal inner/outer produces strictly ordered cosines for smoothstep");

    CHECK(qray::EncodeLightShaftCone(0.0f, 0.0f, true, false, cone), "projector default outer angle");
    CHECK(NearlyEqual(cone[1], std::cos(static_cast<float>(qray::kLightShaftPi / 3.0)), 1e-4f),
          "projector outer defaults to 60 deg");

    CHECK(qray::EncodeLightShaftCone(0.0f, 0.0f, false, false, cone), "non-projector default outer angle");
    CHECK(NearlyEqual(cone[1], std::cos(static_cast<float>(qray::kLightShaftPi * 0.5)), 1e-4f),
          "non-projector outer defaults to 90 deg");

    CHECK(!qray::EncodeLightShaftCone(0.0f, 0.0f, false, true, cone), "spotlight requires positive outer");
    CHECK(!qray::EncodeLightShaftCone(0.0f, std::numeric_limits<float>::quiet_NaN(), false, true, cone),
          "spotlight rejects NaN outer");

    CHECK(qray::EncodeLightShaftCone(1e-4f, 1e-4f, false, true, cone), "narrow spotlight clamps cleanly");
    CHECK(cone[0] > cone[1], "narrow clamped spotlight keeps strictly ordered cosines");
}

void TestShaftReach()
{
    const float dim[3] = { 0.01f, 0.01f, 0.01f };
    const float dimReach = qray::ComputeFluxShaftReach(dim, 1.0f);
    CHECK(NearlyEqual(dimReach, qray::kLightShaftMinReachMeters),
          "dim light clamps at 1.6m minimum rather than 64m pre-metric floor");

    const float bright[3] = { 100.0f, 100.0f, 100.0f };
    const float huge[3] = { 1e6f, 1e6f, 1e6f };
    const float midReach1 = qray::ComputeFluxShaftReach(bright, 1.0f);
    const float midReach4 = qray::ComputeFluxShaftReach(bright, 4.0f);
    const float maxReach = qray::ComputeFluxShaftReach(huge, 100.0f);
    CHECK(midReach1 > qray::kLightShaftMinReachMeters && midReach1 < qray::kLightShaftMaxReachMeters,
          "typical flux reach stays strictly inside bounds");
    CHECK(midReach4 > midReach1 && NearlyEqual(midReach4, midReach1 * 2.0f, 1e-4f),
          "flux reach scales as sqrt(power)");
    CHECK(NearlyEqual(maxReach, qray::kLightShaftMaxReachMeters), "huge light caps at 204.8m");

    CHECK(qray::ComputeFluxShaftReach(nullptr, 1.0f) == 0.0f, "null flux color returns 0");
    CHECK(qray::ComputeFluxShaftReach(bright, 0.0f) == 0.0f, "zero power returns 0");
    CHECK(qray::ComputeFluxShaftReach(bright, std::numeric_limits<float>::quiet_NaN()) == 0.0f,
          "NaN power returns 0");

    const float rad[3] = { 2.0f, 1.0f, 0.5f };
    const float area1 = qray::ComputeRadianceShaftReach(rad, 1.0f, 1.0f, 1.0f);
    const float area4 = qray::ComputeRadianceShaftReach(rad, 4.0f, 1.0f, 1.0f);
    const float emiss4 = qray::ComputeRadianceShaftReach(rad, 1.0f, 4.0f, 1.0f);
    CHECK(NearlyEqual(area4, area1 * 2.0f, 1e-4f), "radiance reach scales with sqrt(area)");
    CHECK(NearlyEqual(emiss4, area1 * 2.0f, 1e-4f), "radiance reach scales with sqrt(meanEmiss)");
}

void TestVolumetricCameraFar()
{
    CHECK(NearlyEqual(qray::ResolveVolumetricCameraFar(false, 2.5f, 0.01f, 500.0f, true),
                      qray::kLightShaftDefaultVolumeFarMeters),
          "legacy off + shafts on raises far plane to 64m");
    CHECK(NearlyEqual(qray::ResolveVolumetricCameraFar(false, 120.0f, 0.01f, 500.0f, true), 120.0f),
          "explicit large volumetricFar is preserved");
    CHECK(NearlyEqual(qray::ResolveVolumetricCameraFar(false, 2.5f, 0.01f, 20.0f, true), 20.0f),
          "cameraFar still caps volumetric far plane");
    CHECK(NearlyEqual(qray::ResolveVolumetricCameraFar(false, 0.0f, 0.01f, 500.0f, false), 2.5f),
          "legacy off + shafts off preserves 2.5m fallback");
    CHECK(NearlyEqual(qray::ResolveVolumetricCameraFar(true, 20.0f, 0.01f, 500.0f, true), 20.0f),
          "legacy on respects explicit volumetricFar");
    CHECK(NearlyEqual(qray::ResolveVolumetricCameraFar(true, 0.0f, 0.01f, 500.0f, true),
                      qray::kLightShaftDefaultVolumeFarMeters),
          "legacy on without explicit far uses 64m when shafts active");
    CHECK(NearlyEqual(qray::ResolveVolumetricCameraFar(true, 0.0f, 0.01f, 500.0f, false), 2.5f),
          "legacy on without explicit far or shafts keeps 2.5m");
    CHECK(qray::ResolveVolumetricCameraFar(false, 2.5f, 0.05f, 0.01f, true) > 0.05f,
          "degenerate cameraFar remains strictly greater than cameraNear");
}

void TestShaftPhase()
{
    const float forward = qray::ShaftPhase(-1.0f, 0.5f);
    const float backward = qray::ShaftPhase(1.0f, 0.5f);
    CHECK(forward > 5.0f * backward, "forward scattering toward light strongly exceeds backscattering");

    const float iso = 1.0f / (4.0f * static_cast<float>(qray::kLightShaftPi));
    CHECK(NearlyEqual(qray::ShaftPhase(-0.3f, 0.0f), iso) &&
              NearlyEqual(qray::ShaftPhase(0.7f, 0.0f), iso),
          "zero asymmetry produces isotropic 1/(4*pi) phase");
}

void VerifyQuadraticSliceIntegration(uint32_t slices, float sigma, float emission)
{
    const float nearPlane = 0.05f;
    const float farPlane = 64.0f;
    std::vector<float> prefixRadiance(slices, 0.0f);
    std::vector<float> prefixTransmittance(slices, 1.0f);

    float accum = 0.0f;
    float trans = 1.0f;
    for (uint32_t z = 0; z < slices; z++)
    {
        const float t0 = static_cast<float>(z) / static_cast<float>(slices);
        const float t1 = static_cast<float>(z + 1u) / static_cast<float>(slices);
        const float stepLen = (farPlane - nearPlane) * (t1 * t1 - t0 * t0);
        accum += qray::MediumAccumulate(emission, sigma, stepLen, trans);
        prefixRadiance[z] = accum;
        prefixTransmittance[z] = trans;
    }

    for (uint32_t sampleIdx = 0; sampleIdx < 16u; sampleIdx++)
    {
        const float u = (static_cast<float>(sampleIdx) + 0.37f) / 16.0f;
        const float cellIndexF = std::floor(u * static_cast<float>(slices));
        const uint32_t cellIndex = std::min(static_cast<uint32_t>(cellIndexF), slices - 1u);
        const float tStart = static_cast<float>(cellIndex) / static_cast<float>(slices);
        const float tEnd = static_cast<float>(cellIndex + 1u) / static_cast<float>(slices);
        const float cellStart = nearPlane + (farPlane - nearPlane) * tStart * tStart;
        const float cellEnd = nearPlane + (farPlane - nearPlane) * tEnd * tEnd;
        const float radialDistance = nearPlane + (farPlane - nearPlane) * u * u;
        const float visibleLen = std::clamp(radialDistance - cellStart, 0.0f, cellEnd - cellStart);

        const float baseL = cellIndex > 0u ? prefixRadiance[cellIndex - 1u] : 0.0f;
        float t = cellIndex > 0u ? prefixTransmittance[cellIndex - 1u] : 1.0f;
        const float totalL = baseL + qray::MediumAccumulate(emission, sigma, visibleLen, t);

        const float traveled = radialDistance - nearPlane;
        const float exactT = qray::MediumTransmittance(sigma, traveled);
        const float exactL = emission * qray::MediumIntegration(sigma, traveled);
        CHECK(NearlyEqual(t, exactT, 1e-5f), "prefix+terminal transmittance matches exact exp(-sigma*d)");
        CHECK(NearlyEqual(totalL, exactL, 1e-5f), "prefix+terminal scattering matches analytical integral");
    }
}

void TestBeerLambertPrefixAndTerminal()
{
    VerifyQuadraticSliceIntegration(64u, 0.03f, 2.5f);
    VerifyQuadraticSliceIntegration(96u, 0.08f, 1.25f);
    VerifyQuadraticSliceIntegration(64u, 0.0f, 3.0f);
}

void TestExtinctionAlphaEncoding()
{
    CHECK(NearlyEqual(qray::DecodeShaftTransmittance(0.0f), 1.0f),
          "zero-cleared alpha decodes as full transmittance");
    CHECK(NearlyEqual(qray::DecodeShaftTransmittance(1.0f), 0.0f),
          "unit extinction decodes as zero transmittance");

    const float samples[5] = { -0.25f, 0.0f, 0.35f, 1.0f, 1.5f };
    for (float t : samples)
    {
        const float encoded = qray::EncodeShaftExtinction(t);
        const float decoded = qray::DecodeShaftTransmittance(encoded);
        CHECK(NearlyEqual(decoded, std::clamp(t, 0.0f, 1.0f), 1e-6f),
              "extinction alpha round-trips transmittance within [0, 1]");
    }
}

}

int main()
{
    TestConeEncoding();
    TestShaftReach();
    TestVolumetricCameraFar();
    TestShaftPhase();
    TestBeerLambertPrefixAndTerminal();
    TestExtinctionAlphaEncoding();

    std::printf("LightShaftTests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
