#include "SamplerFilterPolicy.h"

#include <iostream>
#include <stdexcept>

using namespace qray;

namespace
{

void Check(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void TestDefaultParamsAndPresets()
{
    const QrDrawFrameTexturesParams defaultParams{};
    Check(defaultParams.samplerMipmapMode == QR_SAMPLER_MIPMAP_MODE_LINEAR,
          "Zero-initialized samplerMipmapMode must default to trilinear (linear mipmap)");
    Check(ResolveSamplerAnisotropy(defaultParams.samplerAnisotropy, 16u) == 16u,
          "Zero-initialized samplerAnisotropy must default to 16x when hardware supports it");
    Check(IsSamplerAnisotropyEnabled(16u), "16x anisotropy must be enabled");

    Check(ResolveSamplerAnisotropy(1u, 16u) == 1u, "samplerAnisotropy == 1 must disable anisotropy");
    Check(!IsSamplerAnisotropyEnabled(1u), "1x anisotropy must report disabled");

    const uint32_t validLevels[] = { 2u, 4u, 8u, 16u };
    for (const uint32_t level : validLevels)
    {
        Check(ResolveSamplerAnisotropy(level, 16u) == level, "Supported anisotropy level must pass through unchanged");
        Check(IsSamplerAnisotropyEnabled(level), "2x/4x/8x/16x anisotropy must report enabled");
    }
}

void TestQuantizationAndHardwareClamping()
{
    Check(QuantizeSamplerAnisotropy(0u) == 1u, "0 must quantize to isotropic 1");
    Check(QuantizeSamplerAnisotropy(1u) == 1u, "1 must quantize to isotropic 1");
    Check(QuantizeSamplerAnisotropy(3u) == 2u, "3 must quantize down to 2x");
    Check(QuantizeSamplerAnisotropy(6u) == 4u, "6 must quantize down to 4x");
    Check(QuantizeSamplerAnisotropy(12u) == 8u, "12 must quantize down to 8x");
    Check(QuantizeSamplerAnisotropy(32u) == 16u, "32 must clamp down to 16x");

    Check(ResolveSamplerAnisotropy(0u, 8u) == 8u, "Default anisotropy must clamp to 8x GPU limit");
    Check(ResolveSamplerAnisotropy(16u, 8u) == 8u, "16x request must clamp to 8x GPU limit");
    Check(ResolveSamplerAnisotropy(16u, 4u) == 4u, "16x request must clamp to 4x GPU limit");
    Check(ResolveSamplerAnisotropy(16u, 1u) == 1u, "16x request must clamp to 1x when GPU lacks >1x anisotropy");
    Check(ResolveSamplerAnisotropy(16u, 0u) == 1u, "16x request must clamp safely to 1x when GPU reports 0");

    Check(ClampSamplerAnisotropy(0u, 16u) == 1u, "Direct clamp with 0 must turn anisotropy off");
    Check(ClampSamplerAnisotropy(16u, 8u) == 8u, "Direct clamp must respect device limit");
}

void TestMipmapModeConversion()
{
    Check(QR_SAMPLER_MIPMAP_MODE_LINEAR == 0u,
          "QR_SAMPLER_MIPMAP_MODE_LINEAR must remain 0 for zero-initialized structs");
    Check(ToVkSamplerMipmapMode(QR_SAMPLER_MIPMAP_MODE_LINEAR) == VK_SAMPLER_MIPMAP_MODE_LINEAR,
          "Linear mipmap mode must map to VK_SAMPLER_MIPMAP_MODE_LINEAR");
    Check(ToVkSamplerMipmapMode(QR_SAMPLER_MIPMAP_MODE_NEAREST) == VK_SAMPLER_MIPMAP_MODE_NEAREST,
          "Nearest mipmap mode must map to VK_SAMPLER_MIPMAP_MODE_NEAREST");
}

}

int main()
{
    try
    {
        TestDefaultParamsAndPresets();
        TestQuantizationAndHardwareClamping();
        TestMipmapModeConversion();
    }
    catch (const std::exception &ex)
    {
        std::cerr << ex.what() << '\n';
        return 1;
    }

    return 0;
}
