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

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "qray/qray.h"

namespace qray
{

inline constexpr float kSubsurfaceMaxRadiusMeters = 1.0f;

constexpr bool IsSubsurfaceAllowedPassThrough( QrGeometryPassThroughType type )
{
    return type == QR_GEOMETRY_PASS_THROUGH_TYPE_OPAQUE ||
           type == QR_GEOMETRY_PASS_THROUGH_TYPE_ALPHA_TESTED;
}

inline float SanitizeUnitFloat( float value )
{
    if( !std::isfinite( value ) )
    {
        return 0.0f;
    }
    return std::clamp( value, 0.0f, 1.0f );
}

inline uint32_t PackSubsurfaceQuantized( float normalizedValue, uint32_t maxCode )
{
    const float clamped = SanitizeUnitFloat( normalizedValue );
    const float scaled  = clamped * static_cast< float >( maxCode );
    const long  rounded = std::lround( scaled );
    return static_cast< uint32_t >( std::clamp< long >( rounded, 0L, static_cast< long >( maxCode ) ) );
}

inline uint32_t PackSubsurfaceParams( const QrFloat3D &color, float radiusMeters, float strength )
{
    const float rLin = SanitizeUnitFloat( color.data[ 0 ] );
    const float gLin = SanitizeUnitFloat( color.data[ 1 ] );
    const float bLin = SanitizeUnitFloat( color.data[ 2 ] );
    const float radNorm =
        std::isfinite( radiusMeters )
            ? std::clamp( radiusMeters / kSubsurfaceMaxRadiusMeters, 0.0f, 1.0f )
            : 0.0f;
    const float strNorm = SanitizeUnitFloat( strength );

    // Perceptual / sqrt encoding preserves millimeter radius steps and dark scatter tints.
    const uint32_t redQuant      = PackSubsurfaceQuantized( std::sqrt( rLin ), 31u );
    const uint32_t greenQuant    = PackSubsurfaceQuantized( std::sqrt( gLin ), 63u );
    const uint32_t blueQuant     = PackSubsurfaceQuantized( std::sqrt( bLin ), 31u );
    const uint32_t radiusQuant   = PackSubsurfaceQuantized( std::sqrt( radNorm ), 255u );
    const uint32_t strengthQuant = PackSubsurfaceQuantized( strNorm, 255u );

    return redQuant | ( greenQuant << 5u ) | ( blueQuant << 11u ) | ( radiusQuant << 16u ) |
           ( strengthQuant << 24u );
}

struct UnpackedSubsurfaceParams
{
    float color[ 3 ];
    float radiusMeters;
    float strength;
};

inline UnpackedSubsurfaceParams UnpackSubsurfaceParams( uint32_t packed )
{
    const float rSqrt = static_cast< float >( packed & 31u ) / 31.0f;
    const float gSqrt = static_cast< float >( ( packed >> 5u ) & 63u ) / 63.0f;
    const float bSqrt = static_cast< float >( ( packed >> 11u ) & 31u ) / 31.0f;
    const float radSq = static_cast< float >( ( packed >> 16u ) & 255u ) / 255.0f;
    const float str   = static_cast< float >( ( packed >> 24u ) & 255u ) / 255.0f;

    UnpackedSubsurfaceParams out {};
    out.color[ 0 ]   = rSqrt * rSqrt;
    out.color[ 1 ]   = gSqrt * gSqrt;
    out.color[ 2 ]   = bSqrt * bSqrt;
    out.radiusMeters = ( radSq * radSq ) * kSubsurfaceMaxRadiusMeters;
    out.strength     = str;
    return out;
}

} // namespace qray
