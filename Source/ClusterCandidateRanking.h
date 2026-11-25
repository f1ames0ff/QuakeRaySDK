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

namespace qray
{

// One square scene unit in the former 40-units-per-metre space.
constexpr double kClusterCandidateMinArea = 0.000625;

inline double ClusterCandidateScore(double power, double radius, double distanceSquared)
{
    double scale = radius * radius;

    if (!(scale > kClusterCandidateMinArea))
    {
        scale = kClusterCandidateMinArea;
    }

    if (!(distanceSquared > scale))
    {
        distanceSquared = scale;
    }

    return power > 0.0 ? power / distanceSquared : 0.0;
}

}
