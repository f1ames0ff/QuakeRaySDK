// Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>

#include "qray/qray.h"

namespace qray
{

inline constexpr uint32_t kDefaultWorldAnisotropy = 16;
inline constexpr uint32_t kMaxWorldAnisotropy = 16;

inline uint32_t QuantizeSamplerAnisotropy(uint32_t value) noexcept
{
    if (value >= 16u)
    {
        return 16u;
    }
    if (value >= 8u)
    {
        return 8u;
    }
    if (value >= 4u)
    {
        return 4u;
    }
    if (value >= 2u)
    {
        return 2u;
    }
    return 1u;
}

inline uint32_t ClampSamplerAnisotropy(uint32_t level, uint32_t maxSupported) noexcept
{
    return std::min(QuantizeSamplerAnisotropy(level), QuantizeSamplerAnisotropy(maxSupported));
}

inline uint32_t ResolveSamplerAnisotropy(uint32_t requested, uint32_t maxSupported) noexcept
{
    const uint32_t target = (requested == 0u) ? kDefaultWorldAnisotropy : requested;
    return ClampSamplerAnisotropy(target, maxSupported);
}

inline bool IsSamplerAnisotropyEnabled(uint32_t effectiveAnisotropy) noexcept
{
    return effectiveAnisotropy > 1u;
}

inline VkSamplerMipmapMode ToVkSamplerMipmapMode(QrSamplerMipmapMode mode) noexcept
{
    return mode == QR_SAMPLER_MIPMAP_MODE_NEAREST
        ? VK_SAMPLER_MIPMAP_MODE_NEAREST
        : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

}
