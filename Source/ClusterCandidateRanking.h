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
