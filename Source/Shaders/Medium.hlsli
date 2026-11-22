#ifndef MEDIUM_HLSLI_
#define MEDIUM_HLSLI_

float mediumTransmittance(float sigma, float distance)
{
    return exp(-max(sigma, 0.0) * max(distance, 0.0));
}

float mediumIntegration(float sigma, float distance)
{
    const float s = max(sigma, 0.0);
    const float d = max(distance, 0.0);
    return s > 1e-6 ? (1.0 - exp(-s * d)) / s : d;
}

float3 mediumAccumulate(float3 source, float sigma, float distance, inout float transmittance)
{
    const float stepTransmittance = mediumTransmittance(sigma, distance);
    const float3 inScattering = transmittance * source * mediumIntegration(sigma, distance);
    transmittance *= stepTransmittance;
    return inScattering;
}

#endif
