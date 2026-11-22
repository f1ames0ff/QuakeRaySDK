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
          "dim light clamps at 3.2m minimum so low-intensity lamps keep visible beams");

    const float bright[3] = { 20.0f, 20.0f, 20.0f };
    const float huge[3] = { 1e6f, 1e6f, 1e6f };
    const float midReachLow = qray::ComputeFluxShaftReach(bright, 0.1f);
    const float midReach1 = qray::ComputeFluxShaftReach(bright, 1.0f);
    const float midReach4 = qray::ComputeFluxShaftReach(bright, 4.0f);
    const float maxReach = qray::ComputeFluxShaftReach(huge, 100.0f);
    const float expectedMid1 = std::sqrt(20.0f / (static_cast<float>(qray::kLightShaftPi) * 0.25f));
    CHECK(NearlyEqual(midReach1, expectedMid1, 1e-4f) &&
              midReach1 > qray::kLightShaftMinReachMeters && midReach1 < qray::kLightShaftAutoMaxReachMeters,
          "typical flux reach matches 0.25 lux threshold and stays strictly inside bounds");
    CHECK(NearlyEqual(midReachLow, midReach1, 1e-5f) && NearlyEqual(midReach4, midReach1, 1e-5f),
          "flux reach is independent of light_shafts_power so low power makes dimmer/more transparent shafts");
    CHECK(NearlyEqual(maxReach, qray::kLightShaftAutoMaxReachMeters), "huge auto light caps at 19.2m");

    CHECK(qray::ComputeFluxShaftReach(nullptr, 1.0f) == 0.0f, "null flux color returns 0");
    CHECK(qray::ComputeFluxShaftReach(bright, 0.0f) == 0.0f, "zero power returns 0");
    CHECK(qray::ComputeFluxShaftReach(bright, std::numeric_limits<float>::quiet_NaN()) == 0.0f,
          "NaN power returns 0");

    const float rad[3] = { 12.0f, 6.0f, 3.0f };
    const float area1Low = qray::ComputeRadianceShaftReach(rad, 1.0f, 1.0f, 0.1f);
    const float area1 = qray::ComputeRadianceShaftReach(rad, 1.0f, 1.0f, 1.0f);
    const float area4 = qray::ComputeRadianceShaftReach(rad, 4.0f, 1.0f, 1.0f);
    const float emiss4 = qray::ComputeRadianceShaftReach(rad, 1.0f, 4.0f, 1.0f);
    CHECK(qray::ComputeRadianceShaftReach(rad, 1.0f, 1.0f, 0.0f) == 0.0f,
          "zero power radiance reach returns 0");
    CHECK(NearlyEqual(area1Low, area1, 1e-5f), "radiance reach is independent of light_shafts_power");
    CHECK(NearlyEqual(area4, area1 * 2.0f, 1e-4f), "radiance reach scales with sqrt(area)");
    CHECK(NearlyEqual(emiss4, area1 * 2.0f, 1e-4f), "radiance reach scales with sqrt(meanEmiss)");

    CHECK(NearlyEqual(qray::ResolveLightShaftReach(6.0f, 0.0f), 6.0f),
          "unset explicit distance preserves auto reach");
    CHECK(NearlyEqual(qray::ResolveLightShaftReach(6.0f, 0.1f), qray::kLightShaftMinExplicitReachMeters),
          "explicit distance clamps to 0.5m minimum");
    CHECK(NearlyEqual(qray::ResolveLightShaftReach(6.0f, 12.5f), 12.5f),
          "explicit distance overrides auto reach");
    CHECK(NearlyEqual(qray::ResolveLightShaftReach(6.0f, 100.0f), qray::kLightShaftMaxReachMeters),
          "explicit distance clamps to 25.0m maximum");

    CHECK(NearlyEqual(qray::SanitizeLightShaftFalloff(0.0f), qray::kLightShaftDefaultFalloff),
          "zero falloff defaults to 1.0");
    CHECK(NearlyEqual(qray::SanitizeLightShaftFalloff(-2.0f), qray::kLightShaftDefaultFalloff),
          "negative falloff defaults to 1.0");
    CHECK(NearlyEqual(qray::SanitizeLightShaftFalloff(1e-4f), qray::kLightShaftMinFalloff),
          "tiny positive falloff clamps to 0.01");
    CHECK(NearlyEqual(qray::SanitizeLightShaftFalloff(9.0f), qray::kLightShaftMaxFalloff),
          "huge falloff clamps to 4.0");

    const float reach = 10.0f;
    CHECK(qray::ShaftDistanceAttenuation(10.0f, reach, 1.0f) == 0.0f &&
              qray::ShaftDistanceAttenuation(12.0f, reach, 1.0f) == 0.0f,
          "attenuation is zero at and beyond reach");
    const float attenNearSoft = qray::ShaftDistanceAttenuation(0.2f, reach, 0.2f);
    const float attenNearSteep = qray::ShaftDistanceAttenuation(0.2f, reach, 3.0f);
    const float attenMid = qray::ShaftDistanceAttenuation(3.0f, reach, 1.0f);
    const float attenFarFlat = qray::ShaftDistanceAttenuation(6.0f, reach, 0.01f);
    const float attenFarDefault = qray::ShaftDistanceAttenuation(6.0f, reach, 1.0f);
    const float attenFarSteep = qray::ShaftDistanceAttenuation(6.0f, reach, 3.0f);
    CHECK(attenNearSoft < attenNearSteep && attenMid > attenFarDefault,
          "attenuation decays smoothly without near-lamp blowout");
    CHECK(attenFarFlat > attenFarDefault && attenFarDefault > attenFarSteep,
          "lower falloff keeps beam stronger farther down range");
}

void TestVolumetricCameraFar()
{
    CHECK(NearlyEqual(qray::ResolveVolumetricCameraFar(false, 2.5f, 0.01f, 500.0f, true),
                      qray::kLightShaftMinVolumeFarMeters),
          "legacy off + shafts on clamps small explicit far up to 16m floor");
    CHECK(NearlyEqual(qray::ResolveVolumetricCameraFar(false, 25.0f, 0.01f, 500.0f, true), 25.0f),
          "explicit 25m volumetricFar is preserved");
    CHECK(NearlyEqual(qray::ResolveVolumetricCameraFar(false, 0.0f, 0.01f, 500.0f, true),
                      qray::kLightShaftDefaultVolumeFarMeters),
          "legacy off + shafts on without explicit far uses 25m default");
    CHECK(NearlyEqual(qray::ResolveVolumetricCameraFar(false, 120.0f, 0.01f, 500.0f, true), 120.0f),
          "explicit large volumetricFar is preserved");
    CHECK(NearlyEqual(qray::ResolveVolumetricCameraFar(false, 2.5f, 0.01f, 10.0f, true), 10.0f),
          "cameraFar still caps volumetric far plane");
    CHECK(NearlyEqual(qray::ResolveVolumetricCameraFar(false, 0.0f, 0.01f, 500.0f, false), 2.5f),
          "legacy off + shafts off preserves 2.5m fallback");
    CHECK(NearlyEqual(qray::ResolveVolumetricCameraFar(true, 20.0f, 0.01f, 500.0f, true), 20.0f),
          "legacy on respects explicit volumetricFar");
    CHECK(NearlyEqual(qray::ResolveVolumetricCameraFar(true, 0.0f, 0.01f, 500.0f, true),
                      qray::kLightShaftDefaultVolumeFarMeters),
          "legacy on without explicit far uses 25m when shafts active");
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
