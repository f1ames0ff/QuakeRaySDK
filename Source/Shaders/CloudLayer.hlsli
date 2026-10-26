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

static const float CLOUD_VERTICAL_STRETCH = 2.0;
static const float CLOUD_FREQUENCY = 2.0;
static const int   CLOUD_OCTAVES = 3;
static const int   CLOUD_DETAIL_OCTAVES = 2;
static const float CLOUD_DETAIL_FREQUENCY = 4.5;
static const float CLOUD_NOISE_MEAN = 0.5;
static const float CLOUD_NOISE_SIGMA = 0.12;
static const float CLOUD_SHAPE_POWER = 1.6;
static const float CLOUD_BASE_RAKE = 0.15;
static const float CLOUD_EXTINCTION = 8.0;
static const float CLOUD_LIGHT_CONE = 0.15;
static const int   CLOUD_SUN_STEPS_MAX = 64;
static const int   CLOUD_SHADOW_STEPS = 32;
static const float CLOUD_SEQUENCE_STEP = 0.6180339887;
static const float2 CLOUD_SUN_SAMPLE_PHASES[CLOUD_SUN_STEPS_MAX] =
{
    float2(0.36237489f, 0.932032424f),
    float2(-0.896782822f, -0.442470982f),
    float2(0.960144597f, -0.279503761f),
    float2(-0.519178667f, 0.854665731f),
    float2(-0.194492214f, -0.980904062f),
    float2(0.806003679f, 0.591910525f),
    float2(-0.994151843f, 0.107991263f),
    float2(0.66010958f, -0.751169317f),
    float2(0.0206633226f, 0.999786491f),
    float2(-0.690582562f, -0.723253569f),
    float2(0.997764855f, 0.0668228555f),
    float2(-0.780858942f, 0.624707381f),
    float2(0.15379731f, -0.988102418f),
    float2(0.554048243f, 0.832484561f),
    float2(-0.970873172f, -0.239593996f),
    float2(0.877735081f, -0.479146248f),
    float2(-0.323555892f, 0.94620906f),
    float2(-0.400574991f, -0.916263978f),
    float2(0.914298955f, 0.405040023f),
    float2(-0.947776199f, 0.318936163f),
    float2(0.483422391f, -0.875387224f),
    float2(0.234854948f, 0.972030428f),
    float2(-0.829771849f, -0.558102749f),
    float2(0.988840928f, -0.148975232f),
    float2(-0.628509202f, 0.777802149f),
    float2(-0.0619546771f, -0.998078964f),
    float2(0.719876104f, 0.694102583f),
    float2(-0.999673793f, -0.0255403221f),
    float2(0.754380583f, -0.656437306f),
    float2(-0.112839736f, 0.993613201f),
    float2(-0.587971564f, -0.808881598f),
    float2(0.979943601f, 0.199275032f),
    float2(-0.857188264f, 0.515003185f),
    float2(0.284184296f, -0.958769673f),
    float2(0.438090953f, 0.898930652f),
    float2(-0.930253565f, -0.3669173f),
    float2(0.933789102f, -0.357823856f),
    float2(-0.446840481f, 0.894613651f),
    float2(-0.274816574f, -0.961496672f),
    float2(0.852122859f, 0.523341794f),
    float2(-0.981841179f, 0.189704769f),
    float2(0.595835399f, -0.803106579f),
    float2(0.10314022f, 0.994666826f),
    float2(-0.747940175f, -0.663766145f),
    float2(0.999875396f, -0.0157858314f),
    float2(-0.726613823f, 0.687046106f),
    float2(0.0716894436f, -0.997427002f),
    float2(0.620890694f, 0.783897153f),
    float2(-0.987340393f, -0.158615727f),
    float2(0.835177462f, -0.549980552f),
    float2(-0.244327343f, 0.969692812f),
    float2(-0.474858703f, -0.88006205f),
    float2(0.944619402f, 0.328167921f),
    float2(-0.918207195f, 0.396100426f),
    float2(0.409495416f, -0.912312175f),
    float2(0.314308844f, 0.949320784f),
    float2(-0.873018535f, -0.487687028f),
    float2(0.973164552f, -0.23011031f),
    float2(-0.562143973f, 0.82703939f),
    float2(-0.14414961f, -0.989555905f),
    float2(0.774726846f, 0.632296065f),
    float2(-0.99836932f, 0.0570850243f),
    float2(0.697606086f, -0.716481506f),
    float2(-0.0304167138f, 0.999537305f),
};

struct CloudLayer
{
    float altitude;
    float thickness;
    float coverage;
    float density;
    float detail;
    float time;
    float speed;
    float2 wind;
    float frequency;
};

void cloudPrepareLayer(inout CloudLayer layer)
{
    layer.wind = float2(layer.time * layer.speed * 0.75, layer.time * layer.speed * 0.30);
    layer.frequency = CLOUD_FREQUENCY / layer.thickness;
}

void cloudSunSampleBasis(float3 sunDir, float stepSize,
                         out float3 tangent, out float3 bitangent, out float sampleStep)
{
    float3 helper = abs(sunDir.z) < 0.99 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    tangent = normalize(cross(helper, sunDir));
    bitangent = cross(sunDir, tangent);
    sampleStep = max(stepSize, 2.5e-5);
}

float3 cloudSunSample(float3 p, float3 sunDir, float dist,
                      float3 tangent, float3 bitangent, float sampleStep,
                      int sampleIndex)
{
    float2 phase;
    if (sampleStep > 2.5e-5)
    {
        phase = CLOUD_SUN_SAMPLE_PHASES[clamp(sampleIndex, 0, CLOUD_SUN_STEPS_MAX - 1)];
    }
    else
    {
        float angle = dist / sampleStep * 2.39996323;
        phase = float2(cos(angle), sin(angle));
    }

    float radius = CLOUD_LIGHT_CONE * dist * 0.5;

    return p + sunDir * dist + (phase.x * tangent + phase.y * bitangent) * radius;
}

float hash13(float3 p)
{
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

float cloudMarchSeed(int2 texel, int face)
{
    return hash13(float3(float2(texel), float(face)));
}

float valueNoise3(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);

    float n000 = hash13(i);
    float n100 = hash13(i + float3(1.0, 0.0, 0.0));
    float n010 = hash13(i + float3(0.0, 1.0, 0.0));
    float n110 = hash13(i + float3(1.0, 1.0, 0.0));
    float n001 = hash13(i + float3(0.0, 0.0, 1.0));
    float n101 = hash13(i + float3(1.0, 0.0, 1.0));
    float n011 = hash13(i + float3(0.0, 1.0, 1.0));
    float n111 = hash13(i + float3(1.0, 1.0, 1.0));

    return lerp(
        lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y),
        lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y),
        f.z);
}

float cloudNoise(float3 p, int octaves)
{
    float v = 0.0;
    float amp = 0.5;
    float norm = 0.0;

    for (int i = 0; i < octaves; i++)
    {
        v += amp * valueNoise3(p);
        norm += amp;
        p = p * 2.03 + float3(1.7, 9.2, 3.9);
        amp *= 0.5;
    }

    return v / norm;
}

float cloudShape(float3 p, int octaves)
{
    float n = cloudNoise(p, octaves);
    float z = (n - CLOUD_NOISE_MEAN) / CLOUD_NOISE_SIGMA;
    return clamp(0.5 + 0.5 * z / sqrt(1.0 + z * z), 0.0, 1.0);
}

float cloudHeightProfile(float h)
{
    const float base = 0.12;
    const float top = 0.55;
    return smoothstep(0.0, base, h) * (1.0 - smoothstep(top, 1.0, h));
}

float cloudDensity(CloudLayer layer, float3 p, bool detail)
{
    float h = (p.z - layer.altitude) / layer.thickness;
    if (h <= 0.0 || h >= 1.0)
    {
        return 0.0;
    }

    float shape = cloudShape(
        float3(p.xy + layer.wind, p.z / CLOUD_VERTICAL_STRETCH) * layer.frequency,
        CLOUD_OCTAVES);

    float profile = cloudHeightProfile(h + shape * CLOUD_BASE_RAKE);
    if (profile <= 0.0)
    {
        return 0.0;
    }

    float d = (shape - layer.coverage) / max(1.0 - layer.coverage, 1.0e-3);
    d = pow(clamp(d, 0.0, 1.0), CLOUD_SHAPE_POWER);
    if (d <= 0.0)
    {
        return 0.0;
    }

    if (detail)
    {
        float3 q = float3(p.xy + layer.wind, p.z * 0.75 / CLOUD_VERTICAL_STRETCH) *
                   layer.frequency * CLOUD_DETAIL_FREQUENCY;
        float erosion = layer.detail * cloudShape(q, CLOUD_DETAIL_OCTAVES);
        d = clamp((d - erosion) / max(1.0 - erosion, 1.0e-3), 0.0, 1.0);
    }

    return d * profile;
}

float cloudOpticalDepth(CloudLayer layer, float column)
{
    return column * layer.density * CLOUD_EXTINCTION / layer.thickness;
}

float cloudSunDepth(CloudLayer layer, float3 p, float3 sunDir, int steps, int slices)
{
    steps = clamp(steps, 1, CLOUD_SUN_STEPS_MAX);
    slices = max(slices, 2);

    float height = clamp((p.z - layer.altitude) / layer.thickness, 0.0, 1.0);

    float dt = layer.thickness / max(sunDir.z, 1.0e-3) / (float)steps;
    float3 base = p + sunDir * ((layer.altitude - p.z) / max(sunDir.z, 1.0e-3));
    float3 tangent;
    float3 bitangent;
    float sampleStep;
    cloudSunSampleBasis(sunDir, dt, tangent, bitangent, sampleStep);

    float slice = height * (float)(slices - 1);
    float low = floor(slice) / (float)(slices - 1);
    float high = min(low + 1.0 / (float)(slices - 1), 1.0);
    float through = slice - floor(slice);

    float columnLow = 0.0;
    float columnHigh = 0.0;

    for (int i = 0; i < steps; i++)
    {
        float column = cloudDensity(
            layer, cloudSunSample(base, sunDir, ((float)i + 0.5) * dt,
                                  tangent, bitangent, sampleStep, i),
            false) * dt;
        float step = (float)(i + 1);
        columnLow  += column * clamp(step - low * (float)steps, 0.0, 1.0);
        columnHigh += column * clamp(step - high * (float)steps, 0.0, 1.0);
    }

    return cloudOpticalDepth(layer, lerp(columnLow, columnHigh, through));
}
