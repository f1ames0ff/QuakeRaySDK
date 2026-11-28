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

#include "EditorParticles.h"

#include "Assets.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr uint64_t kMaxFgaVoxels = 4194304ull;

void NormalizeSafe3(const float in[3], float out[3], const float fallback[3])
{
    const float lenSq = in[0] * in[0] + in[1] * in[1] + in[2] * in[2];
    if (lenSq <= 1e-10f || !std::isfinite(lenSq))
    {
        std::copy_n(fallback, 3, out);
        return;
    }
    const float invLen = 1.0f / std::sqrt(lenSq);
    out[0] = in[0] * invLen;
    out[1] = in[1] * invLen;
    out[2] = in[2] * invLen;
}

void BuildEulerRotation3x3(const float anglesDeg[3], float rot[3][3])
{
    const float rx = anglesDeg[0] * kDegToRad;
    const float ry = anglesDeg[1] * kDegToRad;
    const float rz = anglesDeg[2] * kDegToRad;
    const float cx = std::cos(rx), sx = std::sin(rx);
    const float cy = std::cos(ry), sy = std::sin(ry);
    const float cz = std::cos(rz), sz = std::sin(rz);

    rot[0][0] = cz * cy;
    rot[0][1] = cz * sy * sx - sz * cx;
    rot[0][2] = cz * sy * cx + sz * sx;

    rot[1][0] = sz * cy;
    rot[1][1] = sz * sy * sx + cz * cx;
    rot[1][2] = sz * sy * cx - cz * sx;

    rot[2][0] = -sy;
    rot[2][1] = cy * sx;
    rot[2][2] = cy * cx;
}

std::string_view TrimSv(std::string_view sv)
{
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())))
    {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back())))
    {
        sv.remove_suffix(1);
    }
    return sv;
}

std::string_view UnquoteSv(std::string_view sv)
{
    sv = TrimSv(sv);
    const bool doubleQ = sv.size() >= 2 && sv.front() == '"' && sv.back() == '"';
    const bool singleQ = sv.size() >= 2 && sv.front() == '\'' && sv.back() == '\'';
    if (doubleQ || singleQ)
    {
        sv.remove_prefix(1);
        sv.remove_suffix(1);
    }
    return sv;
}

size_t LeadingSpaces(std::string_view line)
{
    size_t i = 0;
    while (i < line.size() && line[i] == ' ')
    {
        i++;
    }
    return i;
}

bool IsMeaningfulYamlLine(std::string_view line)
{
    const std::string_view trimmed = TrimSv(line);
    return !trimmed.empty() && trimmed.front() != '#';
}

std::string FormatFloatCompact(float v)
{
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "%.4g", double(std::abs(v) < 1e-6f ? 0.0f : v));
    return std::string(buf);
}

std::string FormatVec(const float *v, size_t count)
{
    std::string out = "[";
    for (size_t i = 0; i < count; i++)
    {
        if (i > 0)
        {
            out += ", ";
        }
        out += FormatFloatCompact(v[i]);
    }
    out += "]";
    return out;
}

bool ParseBoolValue(std::string_view val, bool fallback)
{
    val = UnquoteSv(val);
    if (val == "1" || val == "true" || val == "yes")
    {
        return true;
    }
    if (val == "0" || val == "false" || val == "no")
    {
        return false;
    }
    return fallback;
}

float ParseFloatValue(std::string_view val, float fallback)
{
    std::string s(UnquoteSv(val));
    if (s.empty())
    {
        return fallback;
    }
    char *end = nullptr;
    const float parsed = std::strtof(s.c_str(), &end);
    return (end != s.c_str() && std::isfinite(parsed)) ? parsed : fallback;
}

uint32_t ParseUIntValue(std::string_view val, uint32_t fallback)
{
    std::string s(UnquoteSv(val));
    if (s.empty())
    {
        return fallback;
    }
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(s.c_str(), &end, 10);
    return end != s.c_str() ? static_cast<uint32_t>(parsed) : fallback;
}

void ParseFloatArray(std::string_view val, float *out, size_t count)
{
    val = TrimSv(val);
    if (!val.empty() && val.front() == '[')
    {
        val.remove_prefix(1);
    }
    if (!val.empty() && val.back() == ']')
    {
        val.remove_suffix(1);
    }

    size_t idx = 0;
    while (idx < count && !val.empty())
    {
        const size_t comma = val.find(',');
        const std::string_view part = comma == std::string_view::npos ? val : val.substr(0, comma);
        out[idx] = ParseFloatValue(part, out[idx]);
        idx++;
        if (comma == std::string_view::npos)
        {
            break;
        }
        val.remove_prefix(comma + 1);
    }
}

EditorParticlePreset ParsePresetName(std::string_view val)
{
    val = UnquoteSv(val);
    for (uint32_t i = 0; i < uint32_t(EditorParticlePreset::Count); i++)
    {
        const auto p = static_cast<EditorParticlePreset>(i);
        if (val == EditorParticlePresetLabel(p))
        {
            return p;
        }
    }
    return EditorParticlePreset::Custom;
}

QrGpuParticleRenderMode ParseRenderModeName(std::string_view val)
{
    static constexpr struct
    {
        const char *name;
        QrGpuParticleRenderMode mode;
    } kModes[] = {
        { "spark", QR_GPU_PARTICLE_RENDER_SPARK },
        { "billboard", QR_GPU_PARTICLE_RENDER_BILLBOARD },
        { "retro_square", QR_GPU_PARTICLE_RENDER_RETRO_SQUARE },
        { "debris_3d", QR_GPU_PARTICLE_RENDER_DEBRIS_3D },
    };
    val = UnquoteSv(val);
    for (const auto &entry : kModes)
    {
        if (val == entry.name)
        {
            return entry.mode;
        }
    }
    return QR_GPU_PARTICLE_RENDER_SPARK;
}

QrVectorFieldKind ParseFieldKindName(std::string_view val)
{
    static constexpr struct
    {
        const char *name;
        QrVectorFieldKind kind;
    } kKinds[] = {
        { "lut_3d", QR_VECTOR_FIELD_KIND_LUT_3D },
        { "curl_noise", QR_VECTOR_FIELD_KIND_CURL_NOISE },
        { "vortex", QR_VECTOR_FIELD_KIND_VORTEX },
        { "point_force", QR_VECTOR_FIELD_KIND_POINT_FORCE },
        { "directional", QR_VECTOR_FIELD_KIND_DIRECTIONAL },
    };
    val = UnquoteSv(val);
    for (const auto &entry : kKinds)
    {
        if (val == entry.name)
        {
            return entry.kind;
        }
    }
    return QR_VECTOR_FIELD_KIND_CURL_NOISE;
}

template <typename T>
struct YamlFieldBinding
{
    const char *key;
    void (*apply)(T &dst, std::string_view val);
};

template <typename T, typename BindingArray>
void ApplyYamlBinding(const BindingArray &bindings, std::string_view key, std::string_view val, T &dst)
{
    for (const auto &b : bindings)
    {
        if (key == b.key)
        {
            b.apply(dst, val);
            return;
        }
    }
}

void ApplyEmitterField(std::string_view key, std::string_view val, EditorParticleEmitterDef &dst)
{
    using B = YamlFieldBinding<EditorParticleEmitterDef>;
    static constexpr B kBindings[] = {
        { "name", [](EditorParticleEmitterDef &d, std::string_view v) { d.name = std::string(UnquoteSv(v)); } },
        { "enabled", [](EditorParticleEmitterDef &d, std::string_view v) { d.enabled = ParseBoolValue(v, d.enabled); } },
        { "preset", [](EditorParticleEmitterDef &d, std::string_view v) { d.preset = ParsePresetName(v); } },
        { "origin", [](EditorParticleEmitterDef &d, std::string_view v) { ParseFloatArray(v, d.origin, 3); } },
        { "direction", [](EditorParticleEmitterDef &d, std::string_view v) { ParseFloatArray(v, d.direction, 3); } },
        { "cone_half_angle", [](EditorParticleEmitterDef &d, std::string_view v) { d.coneHalfAngleDeg = ParseFloatValue(v, d.coneHalfAngleDeg); } },
        { "spawn_rate", [](EditorParticleEmitterDef &d, std::string_view v) { d.spawnRate = ParseFloatValue(v, d.spawnRate); } },
        { "burst_count", [](EditorParticleEmitterDef &d, std::string_view v) { d.burstCount = ParseUIntValue(v, d.burstCount); } },
        { "origin_jitter", [](EditorParticleEmitterDef &d, std::string_view v) { d.originJitterUnits = ParseFloatValue(v, d.originJitterUnits); } },
        { "speed_min", [](EditorParticleEmitterDef &d, std::string_view v) { d.speedMinUnits = ParseFloatValue(v, d.speedMinUnits); } },
        { "speed_max", [](EditorParticleEmitterDef &d, std::string_view v) { d.speedMaxUnits = ParseFloatValue(v, d.speedMaxUnits); } },
        { "size_min", [](EditorParticleEmitterDef &d, std::string_view v) { d.sizeStartMinUnits = ParseFloatValue(v, d.sizeStartMinUnits); } },
        { "size_max", [](EditorParticleEmitterDef &d, std::string_view v) { d.sizeStartMaxUnits = ParseFloatValue(v, d.sizeStartMaxUnits); } },
        { "size_end_scale", [](EditorParticleEmitterDef &d, std::string_view v) { d.sizeEndScale = ParseFloatValue(v, d.sizeEndScale); } },
        { "velocity_stretch", [](EditorParticleEmitterDef &d, std::string_view v) { d.velocityStretch = ParseFloatValue(v, d.velocityStretch); } },
        { "lifetime_min", [](EditorParticleEmitterDef &d, std::string_view v) { d.lifetimeMin = ParseFloatValue(v, d.lifetimeMin); } },
        { "lifetime_max", [](EditorParticleEmitterDef &d, std::string_view v) { d.lifetimeMax = ParseFloatValue(v, d.lifetimeMax); } },
        { "gravity_scale", [](EditorParticleEmitterDef &d, std::string_view v) { d.gravityScale = ParseFloatValue(v, d.gravityScale); } },
        { "drag", [](EditorParticleEmitterDef &d, std::string_view v) { d.drag = ParseFloatValue(v, d.drag); } },
        { "restitution", [](EditorParticleEmitterDef &d, std::string_view v) { d.restitution = ParseFloatValue(v, d.restitution); } },
        { "friction", [](EditorParticleEmitterDef &d, std::string_view v) { d.friction = ParseFloatValue(v, d.friction); } },
        { "color_start", [](EditorParticleEmitterDef &d, std::string_view v) { ParseFloatArray(v, d.colorStart, 4); } },
        { "color_end", [](EditorParticleEmitterDef &d, std::string_view v) { ParseFloatArray(v, d.colorEnd, 4); } },
        { "emission", [](EditorParticleEmitterDef &d, std::string_view v) { d.emission = ParseFloatValue(v, d.emission); } },
        { "spin_speed", [](EditorParticleEmitterDef &d, std::string_view v) { d.spinSpeed = ParseFloatValue(v, d.spinSpeed); } },
        { "render_mode", [](EditorParticleEmitterDef &d, std::string_view v) { d.renderMode = ParseRenderModeName(v); } },
        { "collide_world", [](EditorParticleEmitterDef &d, std::string_view v) { d.collideWorld = ParseBoolValue(v, d.collideWorld); } },
        { "rest_on_floor", [](EditorParticleEmitterDef &d, std::string_view v) { d.restOnFloor = ParseBoolValue(v, d.restOnFloor); } },
        { "affected_by_vector_fields", [](EditorParticleEmitterDef &d, std::string_view v) { d.affectedByVectorFields = ParseBoolValue(v, d.affectedByVectorFields); } },
        { "self_lit", [](EditorParticleEmitterDef &d, std::string_view v) { d.selfLit = ParseBoolValue(v, d.selfLit); } },
        { "additive_blend", [](EditorParticleEmitterDef &d, std::string_view v) { d.additiveBlend = ParseBoolValue(v, d.additiveBlend); } },
        { "texture", [](EditorParticleEmitterDef &d, std::string_view v) { d.textureName = std::string(UnquoteSv(v)); } },
    };
    ApplyYamlBinding(kBindings, key, val, dst);
}

void ApplyVectorFieldKey(std::string_view key, std::string_view val, EditorVectorFieldDef &dst)
{
    using B = YamlFieldBinding<EditorVectorFieldDef>;
    static constexpr B kBindings[] = {
        { "name", [](EditorVectorFieldDef &d, std::string_view v) { d.name = std::string(UnquoteSv(v)); } },
        { "enabled", [](EditorVectorFieldDef &d, std::string_view v) { d.enabled = ParseBoolValue(v, d.enabled); } },
        { "kind", [](EditorVectorFieldDef &d, std::string_view v) { d.kind = ParseFieldKindName(v); } },
        { "fga_path", [](EditorVectorFieldDef &d, std::string_view v) { d.fgaPath = std::string(UnquoteSv(v)); } },
        { "center", [](EditorVectorFieldDef &d, std::string_view v) { ParseFloatArray(v, d.center, 3); } },
        { "half_extents", [](EditorVectorFieldDef &d, std::string_view v) { ParseFloatArray(v, d.halfExtents, 3); } },
        { "angles", [](EditorVectorFieldDef &d, std::string_view v) { ParseFloatArray(v, d.anglesDeg, 3); } },
        { "axis", [](EditorVectorFieldDef &d, std::string_view v) { ParseFloatArray(v, d.axis, 3); } },
        { "intensity", [](EditorVectorFieldDef &d, std::string_view v) { d.intensityUnits = ParseFloatValue(v, d.intensityUnits); } },
        { "max_speed", [](EditorVectorFieldDef &d, std::string_view v) { d.maxSpeedUnits = ParseFloatValue(v, d.maxSpeedUnits); } },
        { "tightness", [](EditorVectorFieldDef &d, std::string_view v) { d.tightness = ParseFloatValue(v, d.tightness); } },
        { "edge_falloff", [](EditorVectorFieldDef &d, std::string_view v) { d.edgeFalloff = ParseFloatValue(v, d.edgeFalloff); } },
        { "curl_frequency", [](EditorVectorFieldDef &d, std::string_view v) { d.curlFrequencyPerUnit = ParseFloatValue(v, d.curlFrequencyPerUnit); } },
        { "curl_scroll_speed", [](EditorVectorFieldDef &d, std::string_view v) { d.curlScrollUnitsPerSec = ParseFloatValue(v, d.curlScrollUnitsPerSec); } },
    };
    ApplyYamlBinding(kBindings, key, val, dst);
}

bool SplitKeyVal(std::string_view line, std::string_view &outKey, std::string_view &outVal)
{
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos)
    {
        return false;
    }
    outKey = TrimSv(line.substr(0, colon));
    outVal = TrimSv(line.substr(colon + 1));
    return !outKey.empty();
}

std::string_view StripLineEnd(std::string_view line)
{
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
    {
        line.remove_suffix(1);
    }
    return line;
}

std::vector<std::string_view> SplitLinesKeepEnd(std::string_view text)
{
    std::vector<std::string_view> lines;
    size_t pos = 0;
    while (pos < text.size())
    {
        const size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos)
        {
            lines.push_back(text.substr(pos));
            break;
        }
        lines.push_back(text.substr(pos, nl - pos + 1));
        pos = nl + 1;
    }
    return lines;
}

bool FindLevelSectionRange(const std::vector<std::string_view> &lines, const std::string &levelKey,
                           size_t &outHead, size_t &outEnd)
{
    outHead = lines.size();
    outEnd = lines.size();
    for (size_t i = 0; i < lines.size(); i++)
    {
        const std::string_view clean = StripLineEnd(lines[i]);
        if (!IsMeaningfulYamlLine(clean) || LeadingSpaces(clean) != 0)
        {
            continue;
        }
        std::string_view k, v;
        if (!SplitKeyVal(clean, k, v) || UnquoteSv(k) != levelKey)
        {
            continue;
        }
        outHead = i + 1;
        for (size_t j = outHead; j < lines.size(); j++)
        {
            const std::string_view c2 = StripLineEnd(lines[j]);
            if (IsMeaningfulYamlLine(c2) && LeadingSpaces(c2) == 0)
            {
                outEnd = j;
                break;
            }
        }
        return true;
    }
    return false;
}

std::vector<float> TokenizeFgaNumbers(std::string_view text)
{
    std::vector<float> numbers;
    size_t i = 0;
    while (i < text.size())
    {
        const char ch = text[i];
        if (std::isspace(static_cast<unsigned char>(ch)) || ch == ',')
        {
            i++;
            continue;
        }
        size_t j = i;
        while (j < text.size() && !std::isspace(static_cast<unsigned char>(text[j])) && text[j] != ',')
        {
            j++;
        }
        const std::string token(text.substr(i, j - i));
        char *end = nullptr;
        const float val = std::strtof(token.c_str(), &end);
        if (end == token.c_str() || !std::isfinite(val))
        {
            return {};
        }
        numbers.push_back(val);
        i = j;
    }
    return numbers;
}

struct PresetTemplate
{
    EditorParticlePreset preset;
    QrGpuParticleRenderMode renderMode;
    float coneDeg;
    float spawnRate;
    uint32_t burstCount;
    float speedMin;
    float speedMax;
    float sizeMin;
    float sizeMax;
    float sizeEndScale;
    float velocityStretch;
    float lifeMin;
    float lifeMax;
    float gravityScale;
    float drag;
    float restitution;
    float friction;
    float colorStart[4];
    float colorEnd[4];
    float emission;
    float spinSpeed;
    bool collideWorld;
    bool restOnFloor;
    bool affectedByFields;
    bool selfLit;
    bool additiveBlend;
};

constexpr PresetTemplate kPresetTemplates[] = {
    {
        EditorParticlePreset::ExplosionSparks, QR_GPU_PARTICLE_RENDER_SPARK,
        72.0f, 0.0f, 96u, 160.0f, 420.0f, 2.2f, 4.6f, 0.15f, 0.035f, 0.9f, 2.1f,
        1.15f, 0.7f, 0.58f, 0.25f,
        { 1.0f, 0.76f, 0.28f, 1.0f }, { 1.0f, 0.14f, 0.02f, 0.0f },
        32.0f, 0.0f, true, false, true, true, true,
    },
    {
        EditorParticlePreset::MetalStrikeSparks, QR_GPU_PARTICLE_RENDER_SPARK,
        28.0f, 0.0f, 48u, 220.0f, 500.0f, 1.6f, 3.2f, 0.10f, 0.040f, 0.6f, 1.4f,
        1.25f, 0.4f, 0.72f, 0.15f,
        { 1.0f, 0.92f, 0.65f, 1.0f }, { 1.0f, 0.30f, 0.04f, 0.0f },
        26.0f, 0.0f, true, false, true, true, true,
    },
    {
        EditorParticlePreset::LightningConcreteHit, QR_GPU_PARTICLE_RENDER_DEBRIS_3D,
        46.0f, 0.0f, 24u, 95.0f, 260.0f, 2.8f, 5.8f, 0.90f, 0.0f, 2.0f, 4.5f,
        1.35f, 0.35f, 0.35f, 0.65f,
        { 0.24f, 0.22f, 0.19f, 1.0f }, { 0.16f, 0.15f, 0.13f, 1.0f },
        0.0f, 6.0f, true, true, false, false, false,
    },
    {
        EditorParticlePreset::FireEmbers, QR_GPU_PARTICLE_RENDER_SPARK,
        35.0f, 48.0f, 24u, 45.0f, 110.0f, 1.4f, 2.8f, 0.25f, 0.020f, 1.4f, 3.0f,
        -0.18f, 0.9f, 0.40f, 0.25f,
        { 1.0f, 0.55f, 0.14f, 1.0f }, { 0.95f, 0.12f, 0.02f, 0.0f },
        10.0f, 0.0f, true, false, true, true, true,
    },
    {
        EditorParticlePreset::WindDust, QR_GPU_PARTICLE_RENDER_BILLBOARD,
        85.0f, 30.0f, 24u, 18.0f, 55.0f, 3.5f, 7.0f, 1.60f, 0.0f, 2.4f, 4.6f,
        0.02f, 0.7f, 0.10f, 0.50f,
        { 0.85f, 0.82f, 0.76f, 0.35f }, { 0.75f, 0.72f, 0.68f, 0.0f },
        0.0f, 1.5f, true, false, true, false, false,
    },
    {
        EditorParticlePreset::QuakeRetroShotgun, QR_GPU_PARTICLE_RENDER_RETRO_SQUARE,
        38.0f, 0.0f, 28u, 90.0f, 240.0f, 2.0f, 3.6f, 1.00f, 0.0f, 4.5f, 7.0f,
        1.15f, 0.45f, 0.40f, 0.62f,
        { 0.72f, 0.66f, 0.54f, 1.0f }, { 0.58f, 0.52f, 0.42f, 1.0f },
        0.0f, 0.0f, true, true, false, false, false,
    },
    {
        EditorParticlePreset::QuakeRetroBlood, QR_GPU_PARTICLE_RENDER_RETRO_SQUARE,
        42.0f, 0.0f, 32u, 80.0f, 220.0f, 2.2f, 4.0f, 1.00f, 0.0f, 5.0f, 8.0f,
        1.15f, 0.40f, 0.25f, 0.75f,
        { 0.62f, 0.04f, 0.03f, 1.0f }, { 0.42f, 0.02f, 0.02f, 1.0f },
        0.0f, 0.0f, true, true, false, false, false,
    },
};

void AppendEmitterYaml(std::string &out, const EditorParticleEmitterDef &e)
{
    out += "    - name: " + (e.name.empty() ? std::string("emitter") : e.name) + "\n";
    out += std::string("      enabled: ") + (e.enabled ? "1" : "0") + "\n";
    out += std::string("      preset: ") + EditorParticlePresetLabel(e.preset) + "\n";
    out += "      origin: " + FormatVec(e.origin, 3) + "\n";
    out += "      direction: " + FormatVec(e.direction, 3) + "\n";
    out += "      cone_half_angle: " + FormatFloatCompact(e.coneHalfAngleDeg) + "\n";
    out += "      spawn_rate: " + FormatFloatCompact(e.spawnRate) + "\n";
    out += "      burst_count: " + std::to_string(e.burstCount) + "\n";
    out += "      origin_jitter: " + FormatFloatCompact(e.originJitterUnits) + "\n";
    out += "      speed_min: " + FormatFloatCompact(e.speedMinUnits) + "\n";
    out += "      speed_max: " + FormatFloatCompact(e.speedMaxUnits) + "\n";
    out += "      size_min: " + FormatFloatCompact(e.sizeStartMinUnits) + "\n";
    out += "      size_max: " + FormatFloatCompact(e.sizeStartMaxUnits) + "\n";
    out += "      size_end_scale: " + FormatFloatCompact(e.sizeEndScale) + "\n";
    out += "      velocity_stretch: " + FormatFloatCompact(e.velocityStretch) + "\n";
    out += "      lifetime_min: " + FormatFloatCompact(e.lifetimeMin) + "\n";
    out += "      lifetime_max: " + FormatFloatCompact(e.lifetimeMax) + "\n";
    out += "      gravity_scale: " + FormatFloatCompact(e.gravityScale) + "\n";
    out += "      drag: " + FormatFloatCompact(e.drag) + "\n";
    out += "      restitution: " + FormatFloatCompact(e.restitution) + "\n";
    out += "      friction: " + FormatFloatCompact(e.friction) + "\n";
    out += "      color_start: " + FormatVec(e.colorStart, 4) + "\n";
    out += "      color_end: " + FormatVec(e.colorEnd, 4) + "\n";
    out += "      emission: " + FormatFloatCompact(e.emission) + "\n";
    out += "      spin_speed: " + FormatFloatCompact(e.spinSpeed) + "\n";
    out += std::string("      render_mode: ") + EditorParticleRenderModeLabel(e.renderMode) + "\n";
    out += std::string("      collide_world: ") + (e.collideWorld ? "1" : "0") + "\n";
    out += std::string("      rest_on_floor: ") + (e.restOnFloor ? "1" : "0") + "\n";
    out += std::string("      affected_by_vector_fields: ") + (e.affectedByVectorFields ? "1" : "0") + "\n";
    out += std::string("      self_lit: ") + (e.selfLit ? "1" : "0") + "\n";
    out += std::string("      additive_blend: ") + (e.additiveBlend ? "1" : "0") + "\n";
    if (!e.textureName.empty())
    {
        out += "      texture: \"" + e.textureName + "\"\n";
    }
}

void AppendVectorFieldYaml(std::string &out, const EditorVectorFieldDef &f)
{
    out += "    - name: " + (f.name.empty() ? std::string("field") : f.name) + "\n";
    out += std::string("      enabled: ") + (f.enabled ? "1" : "0") + "\n";
    out += std::string("      kind: ") + EditorVectorFieldKindLabel(f.kind) + "\n";
    if (!f.fgaPath.empty())
    {
        out += "      fga_path: \"" + f.fgaPath + "\"\n";
    }
    out += "      center: " + FormatVec(f.center, 3) + "\n";
    out += "      half_extents: " + FormatVec(f.halfExtents, 3) + "\n";
    out += "      angles: " + FormatVec(f.anglesDeg, 3) + "\n";
    out += "      axis: " + FormatVec(f.axis, 3) + "\n";
    out += "      intensity: " + FormatFloatCompact(f.intensityUnits) + "\n";
    out += "      max_speed: " + FormatFloatCompact(f.maxSpeedUnits) + "\n";
    out += "      tightness: " + FormatFloatCompact(f.tightness) + "\n";
    out += "      edge_falloff: " + FormatFloatCompact(f.edgeFalloff) + "\n";
    out += "      curl_frequency: " + FormatFloatCompact(f.curlFrequencyPerUnit) + "\n";
    out += "      curl_scroll_speed: " + FormatFloatCompact(f.curlScrollUnitsPerSec) + "\n";
}

bool IsTargetParticleBlockHeader(std::string_view line)
{
    const std::string_view clean = StripLineEnd(line);
    if (!IsMeaningfulYamlLine(clean) || LeadingSpaces(clean) > 2)
    {
        return false;
    }
    std::string_view k, v;
    return SplitKeyVal(TrimSv(clean), k, v) && (k == "particle_emitters" || k == "vector_fields");
}

void ApplyParticleItemLine(std::string_view trimmed, int active,
                           std::vector<EditorParticleEmitterDef> &outEmitters,
                           std::vector<EditorVectorFieldDef> &outFields)
{
    const bool startsItem =
        trimmed == "-" || (trimmed.size() >= 2 && trimmed[0] == '-' &&
                           std::isspace(static_cast<unsigned char>(trimmed[1])));
    std::string_view k, v;
    const std::string_view kv = startsItem ? TrimSv(trimmed.substr(2)) : trimmed;
    const bool hasKv = SplitKeyVal(kv, k, v);

    if (active == 1)
    {
        if (startsItem)
        {
            outEmitters.emplace_back();
        }
        if (hasKv && !outEmitters.empty())
        {
            ApplyEmitterField(k, v, outEmitters.back());
        }
        return;
    }

    if (startsItem)
    {
        outFields.emplace_back();
    }
    if (hasKv && !outFields.empty())
    {
        ApplyVectorFieldKey(k, v, outFields.back());
    }
}

std::string RewriteLevelParticleLines(const std::vector<std::string_view> &lines,
                                      size_t head, size_t end,
                                      const std::string &rendered, bool &outHadBlocks)
{
    std::string merged;
    bool skipping = false;
    outHadBlocks = false;
    for (size_t i = 0; i < lines.size(); i++)
    {
        if (i == end && !rendered.empty())
        {
            merged += rendered;
        }
        if (i >= head && i < end)
        {
            const std::string_view clean = StripLineEnd(lines[i]);
            if (IsMeaningfulYamlLine(clean) && LeadingSpaces(clean) <= 2)
            {
                skipping = IsTargetParticleBlockHeader(lines[i]);
                outHadBlocks = outHadBlocks || skipping;
            }
            if (skipping)
            {
                continue;
            }
        }
        merged.append(lines[i]);
    }
    return merged;
}

void AppendExplosionPresetBursts(const EditorParticleEmitterDef &base, float clampedScale,
                                 std::vector<QrParticleBurstInfo> &outBursts)
{
    EditorParticleEmitterDef sparks = base;
    sparks.burstCount = std::max(8u, uint32_t(std::round(96.0f * clampedScale)));
    sparks.emission = 32.0f;
    outBursts.push_back(BuildEditorBurstQuakeUnits(sparks));

    EditorParticleEmitterDef flash = base;
    flash.renderMode = QR_GPU_PARTICLE_RENDER_BILLBOARD;
    flash.coneHalfAngleDeg = 85.0f;
    flash.burstCount = std::max(4u, uint32_t(std::round(18.0f * clampedScale)));
    flash.originJitterUnits = 5.0f;
    flash.speedMinUnits = 20.0f;
    flash.speedMaxUnits = 95.0f;
    flash.sizeStartMinUnits = 8.5f;
    flash.sizeStartMaxUnits = 16.5f;
    flash.sizeEndScale = 1.75f;
    flash.velocityStretch = 0.0f;
    flash.lifetimeMin = 0.18f;
    flash.lifetimeMax = 0.42f;
    flash.gravityScale = -0.15f;
    flash.drag = 3.2f;
    flash.collideWorld = false;
    flash.restOnFloor = false;
    flash.affectedByVectorFields = false;
    flash.selfLit = true;
    flash.additiveBlend = true;
    flash.colorStart[0] = 1.0f; flash.colorStart[1] = 0.72f; flash.colorStart[2] = 0.26f; flash.colorStart[3] = 0.95f;
    flash.colorEnd[0] = 1.0f;   flash.colorEnd[1] = 0.16f;   flash.colorEnd[2] = 0.02f;   flash.colorEnd[3] = 0.0f;
    flash.emission = 48.0f;
    outBursts.push_back(BuildEditorBurstQuakeUnits(flash));

    EditorParticleEmitterDef cinders = base;
    cinders.renderMode = QR_GPU_PARTICLE_RENDER_SPARK;
    cinders.coneHalfAngleDeg = 68.0f;
    cinders.burstCount = std::max(4u, uint32_t(std::round(28.0f * clampedScale)));
    cinders.speedMinUnits = 55.0f;
    cinders.speedMaxUnits = 155.0f;
    cinders.sizeStartMinUnits = 1.8f;
    cinders.sizeStartMaxUnits = 3.4f;
    cinders.sizeEndScale = 0.35f;
    cinders.velocityStretch = 0.015f;
    cinders.lifetimeMin = 1.4f;
    cinders.lifetimeMax = 2.8f;
    cinders.gravityScale = 0.85f;
    cinders.drag = 0.55f;
    cinders.restitution = 0.35f;
    cinders.friction = 0.55f;
    cinders.collideWorld = true;
    cinders.restOnFloor = true;
    cinders.selfLit = true;
    cinders.additiveBlend = true;
    cinders.colorStart[0] = 1.0f;  cinders.colorStart[1] = 0.58f; cinders.colorStart[2] = 0.16f; cinders.colorStart[3] = 1.0f;
    cinders.colorEnd[0] = 0.95f;   cinders.colorEnd[1] = 0.10f;   cinders.colorEnd[2] = 0.01f;   cinders.colorEnd[3] = 0.0f;
    cinders.emission = 22.0f;
    outBursts.push_back(BuildEditorBurstQuakeUnits(cinders));
}

void AppendLightningPresetBursts(const EditorParticleEmitterDef &base, float clampedScale,
                                 std::vector<QrParticleBurstInfo> &outBursts)
{
    EditorParticleEmitterDef debris = base;
    debris.burstCount = std::max(4u, uint32_t(std::round(20.0f * clampedScale)));
    outBursts.push_back(BuildEditorBurstQuakeUnits(debris));

    EditorParticleEmitterDef arcs = base;
    ApplyEditorParticlePreset(EditorParticlePreset::MetalStrikeSparks, arcs);
    arcs.coneHalfAngleDeg = 55.0f;
    arcs.burstCount = std::max(6u, uint32_t(std::round(28.0f * clampedScale)));
    arcs.colorStart[0] = 0.72f; arcs.colorStart[1] = 0.94f; arcs.colorStart[2] = 1.0f;
    arcs.colorEnd[0] = 0.25f;   arcs.colorEnd[1] = 0.55f;   arcs.colorEnd[2] = 1.0f;
    arcs.emission = 28.0f;
    outBursts.push_back(BuildEditorBurstQuakeUnits(arcs));

    EditorParticleEmitterDef dust = base;
    ApplyEditorParticlePreset(EditorParticlePreset::WindDust, dust);
    dust.coneHalfAngleDeg = 60.0f;
    dust.burstCount = std::max(4u, uint32_t(std::round(14.0f * clampedScale)));
    dust.sizeStartMinUnits = 7.0f;
    dust.sizeStartMaxUnits = 13.0f;
    dust.sizeEndScale = 2.4f;
    dust.lifetimeMin = 0.8f;
    dust.lifetimeMax = 1.6f;
    dust.drag = 2.5f;
    dust.gravityScale = 0.12f;
    outBursts.push_back(BuildEditorBurstQuakeUnits(dust));
}

} // namespace

const char *EditorParticlePresetLabel(EditorParticlePreset preset)
{
    static constexpr const char *kLabels[] = {
        "Custom",
        "ExplosionSparks",
        "MetalStrikeSparks",
        "LightningConcreteHit",
        "FireEmbers",
        "WindDust",
        "QuakeRetroShotgun",
        "QuakeRetroBlood",
    };
    const uint32_t idx = uint32_t(preset);
    return idx < std::size(kLabels) ? kLabels[idx] : "Custom";
}

const char *EditorParticleRenderModeLabel(QrGpuParticleRenderMode mode)
{
    static constexpr const char *kLabels[] = { "spark", "billboard", "retro_square", "debris_3d" };
    const uint32_t idx = uint32_t(mode);
    return idx < std::size(kLabels) ? kLabels[idx] : "spark";
}

const char *EditorVectorFieldKindLabel(QrVectorFieldKind kind)
{
    static constexpr const char *kLabels[] = { "lut_3d", "curl_noise", "vortex", "point_force", "directional" };
    const uint32_t idx = uint32_t(kind);
    return idx < std::size(kLabels) ? kLabels[idx] : "curl_noise";
}

void ApplyEditorParticlePreset(EditorParticlePreset preset, EditorParticleEmitterDef &ioDef)
{
    ioDef.preset = preset;
    for (const PresetTemplate &t : kPresetTemplates)
    {
        if (t.preset != preset)
        {
            continue;
        }
        ioDef.renderMode = t.renderMode;
        ioDef.coneHalfAngleDeg = t.coneDeg;
        ioDef.spawnRate = t.spawnRate;
        ioDef.burstCount = t.burstCount;
        ioDef.speedMinUnits = t.speedMin;
        ioDef.speedMaxUnits = t.speedMax;
        ioDef.sizeStartMinUnits = t.sizeMin;
        ioDef.sizeStartMaxUnits = t.sizeMax;
        ioDef.sizeEndScale = t.sizeEndScale;
        ioDef.velocityStretch = t.velocityStretch;
        ioDef.lifetimeMin = t.lifeMin;
        ioDef.lifetimeMax = t.lifeMax;
        ioDef.gravityScale = t.gravityScale;
        ioDef.drag = t.drag;
        ioDef.restitution = t.restitution;
        ioDef.friction = t.friction;
        std::copy_n(t.colorStart, 4, ioDef.colorStart);
        std::copy_n(t.colorEnd, 4, ioDef.colorEnd);
        ioDef.emission = t.emission;
        ioDef.spinSpeed = t.spinSpeed;
        ioDef.collideWorld = t.collideWorld;
        ioDef.restOnFloor = t.restOnFloor;
        ioDef.affectedByVectorFields = t.affectedByFields;
        ioDef.selfLit = t.selfLit;
        ioDef.additiveBlend = t.additiveBlend;
        return;
    }
}

QrParticleBurstInfo BuildEditorBurstQuakeUnits(const EditorParticleEmitterDef &def, QrMaterial material)
{
    static constexpr float kUp[3] = { 0.0f, 0.0f, 1.0f };
    float dir[3] = {};
    NormalizeSafe3(def.direction, dir, kUp);

    QrParticleBurstInfo out = {};
    std::memcpy(out.origin.data, def.origin, sizeof(out.origin.data));
    std::memcpy(out.direction.data, dir, sizeof(out.direction.data));
    out.coneHalfAngleRadians = std::clamp(def.coneHalfAngleDeg, 0.0f, 180.0f) * kDegToRad;
    out.count = def.burstCount;
    out.originJitterRadius = std::max(0.0f, def.originJitterUnits);
    out.speedMin = std::max(0.0f, def.speedMinUnits);
    out.speedMax = std::max(out.speedMin, def.speedMaxUnits);
    out.sizeStartMin = std::max(0.25f, def.sizeStartMinUnits);
    out.sizeStartMax = std::max(out.sizeStartMin, def.sizeStartMaxUnits);
    out.sizeEndScale = std::max(0.0f, def.sizeEndScale);
    out.velocityStretch = std::max(0.0f, def.velocityStretch);
    out.lifetimeMin = std::max(0.05f, def.lifetimeMin);
    out.lifetimeMax = std::max(out.lifetimeMin, def.lifetimeMax);
    out.gravityScale = def.gravityScale;
    out.drag = std::max(0.0f, def.drag);
    out.restitution = std::clamp(def.restitution, 0.0f, 1.0f);
    out.friction = std::clamp(def.friction, 0.0f, 1.0f);
    std::memcpy(out.colorStart.data, def.colorStart, sizeof(out.colorStart.data));
    std::memcpy(out.colorEnd.data, def.colorEnd, sizeof(out.colorEnd.data));
    out.emission = std::max(0.0f, def.emission);
    out.spinSpeed = def.spinSpeed;
    out.material = material;
    out.cluster = def.cluster;
    out.renderMode = def.renderMode;
    out.flags = (def.collideWorld ? QR_GPU_PARTICLE_FLAG_COLLIDE_WORLD_BIT : 0u) |
                (def.restOnFloor ? QR_GPU_PARTICLE_FLAG_REST_ON_FLOOR_BIT : 0u) |
                (def.affectedByVectorFields ? QR_GPU_PARTICLE_FLAG_AFFECTED_BY_VECTOR_FIELDS_BIT : 0u) |
                (def.selfLit ? QR_GPU_PARTICLE_FLAG_SELF_LIT_BIT : 0u) |
                (def.additiveBlend ? QR_GPU_PARTICLE_FLAG_ADDITIVE_BLEND_BIT : 0u);
    return out;
}

void BuildEditorPresetBurstsQuakeUnits(EditorParticlePreset preset, const float origin[3],
                                       const float normal[3], uint32_t cluster, float scale,
                                       std::vector<QrParticleBurstInfo> &outBursts,
                                       float emissionScale)
{
    outBursts.clear();
    static constexpr float kUp[3] = { 0.0f, 0.0f, 1.0f };
    float dir[3] = {};
    NormalizeSafe3(normal, dir, kUp);

    const float clampedScale = std::clamp(scale, 0.1f, 8.0f);
    EditorParticleEmitterDef base;
    ApplyEditorParticlePreset(preset, base);
    for (int i = 0; i < 3; i++)
    {
        base.origin[i] = origin[i] + dir[i] * 1.5f;
        base.direction[i] = dir[i];
    }
    base.cluster = cluster;

    if (preset == EditorParticlePreset::ExplosionSparks)
    {
        AppendExplosionPresetBursts(base, clampedScale, outBursts);
    }
    else if (preset == EditorParticlePreset::LightningConcreteHit)
    {
        AppendLightningPresetBursts(base, clampedScale, outBursts);
    }
    else
    {
        base.burstCount = std::max(1u, uint32_t(std::round(float(base.burstCount) * clampedScale)));
        outBursts.push_back(BuildEditorBurstQuakeUnits(base));
    }

    const float clampedEmis = std::clamp(emissionScale, 0.0f, 20.0f);
    for (QrParticleBurstInfo &b : outBursts)
    {
        b.emission *= clampedEmis;
    }
}

QrVectorFieldVolume BuildEditorVectorFieldVolumeQuakeUnits(const EditorVectorFieldDef &def,
                                                           QrVectorField fieldHandle)
{
    float rot[3][3] = {};
    BuildEulerRotation3x3(def.anglesDeg, rot);

    QrVectorFieldVolume vol = {};
    vol.field = fieldHandle;
    vol.kind = def.kind;
    for (int r = 0; r < 3; r++)
    {
        for (int c = 0; c < 3; c++)
        {
            vol.worldFromLocal.matrix[r][c] = rot[r][c] * std::max(def.halfExtents[c], 1.0f);
        }
        vol.worldFromLocal.matrix[r][3] = def.center[r];
    }
    vol.intensity = def.intensityUnits;
    vol.maxSpeed = std::max(def.maxSpeedUnits, 1.0f);
    vol.tightness = std::clamp(def.tightness, 0.0f, 1.0f);
    vol.edgeFalloff = std::clamp(def.edgeFalloff, 0.0f, 1.0f);
    vol.curlFrequency = std::max(def.curlFrequencyPerUnit, 0.0f);
    vol.curlScrollSpeed = def.curlScrollUnitsPerSec;

    float worldAxis[3] = {};
    ComputeEditorVectorFieldFlowAxis(def, worldAxis);
    std::memcpy(vol.axis.data, worldAxis, sizeof(vol.axis.data));
    return vol;
}

void BuildEditorEulerRotation3x3(const float anglesDeg[3], float outRot[3][3])
{
    BuildEulerRotation3x3(anglesDeg, outRot);
}

void ExtractEditorEulerAnglesDeg3x3(const float rot[3][3], float outAnglesDeg[3])
{
    const float sy = std::clamp(-rot[2][0], -1.0f, 1.0f);
    const float ry = std::asin(sy);
    float rx = 0.0f;
    float rz = 0.0f;
    if (std::abs(rot[2][0]) < 0.9999f)
    {
        rx = std::atan2(rot[2][1], rot[2][2]);
        rz = std::atan2(rot[1][0], rot[0][0]);
    }
    else
    {
        rx = std::atan2(-rot[1][2], rot[1][1]);
        rz = 0.0f;
    }
    const float radToDeg = 180.0f / kPi;
    outAnglesDeg[0] = rx * radToDeg;
    outAnglesDeg[1] = ry * radToDeg;
    outAnglesDeg[2] = rz * radToDeg;
}

void RotateEditorVectorAroundAxis(const float vec[3], const float unitAxis[3],
                                  float angleDegrees, float outVec[3])
{
    static constexpr float kFallback[3] = { 0.0f, 0.0f, 1.0f };
    float axis[3] = {};
    NormalizeSafe3(unitAxis, axis, kFallback);

    const float rad = angleDegrees * kDegToRad;
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    const float dot = axis[0] * vec[0] + axis[1] * vec[1] + axis[2] * vec[2];
    const float cross[3] = {
        axis[1] * vec[2] - axis[2] * vec[1],
        axis[2] * vec[0] - axis[0] * vec[2],
        axis[0] * vec[1] - axis[1] * vec[0],
    };
    const float rotated[3] = {
        vec[0] * c + cross[0] * s + axis[0] * dot * (1.0f - c),
        vec[1] * c + cross[1] * s + axis[1] * dot * (1.0f - c),
        vec[2] * c + cross[2] * s + axis[2] * dot * (1.0f - c),
    };
    NormalizeSafe3(rotated, outVec, kFallback);
}

void RotateEditorEulerAnglesAroundAxis(const float startAnglesDeg[3], const float unitAxis[3],
                                       float angleDegrees, float outAnglesDeg[3])
{
    float startRot[3][3] = {};
    BuildEulerRotation3x3(startAnglesDeg, startRot);

    float rotatedCols[3][3] = {};
    for (int col = 0; col < 3; col++)
    {
        const float srcCol[3] = { startRot[0][col], startRot[1][col], startRot[2][col] };
        float dstCol[3] = {};
        RotateEditorVectorAroundAxis(srcCol, unitAxis, angleDegrees, dstCol);
        rotatedCols[0][col] = dstCol[0];
        rotatedCols[1][col] = dstCol[1];
        rotatedCols[2][col] = dstCol[2];
    }
    ExtractEditorEulerAnglesDeg3x3(rotatedCols, outAnglesDeg);
}

void ComputeEditorVectorFieldBoxCorners(const EditorVectorFieldDef &def, float outCorners[8][3])
{
    const QrVectorFieldVolume vol = BuildEditorVectorFieldVolumeQuakeUnits(def, QR_EMPTY_VECTOR_FIELD);
    for (int i = 0; i < 8; i++)
    {
        const float lx = (i & 1) ? 1.0f : -1.0f;
        const float ly = (i & 2) ? 1.0f : -1.0f;
        const float lz = (i & 4) ? 1.0f : -1.0f;
        for (int r = 0; r < 3; r++)
        {
            outCorners[i][r] =
                vol.worldFromLocal.matrix[r][0] * lx +
                vol.worldFromLocal.matrix[r][1] * ly +
                vol.worldFromLocal.matrix[r][2] * lz +
                vol.worldFromLocal.matrix[r][3];
        }
    }
}

void ComputeEditorVectorFieldFlowAxis(const EditorVectorFieldDef &def, float outWorldAxis[3])
{
    static constexpr float kDefaultAxis[3] = { 0.0f, 0.0f, 1.0f };
    float unitLocal[3] = {};
    NormalizeSafe3(def.axis, unitLocal, kDefaultAxis);
    float rot[3][3] = {};
    BuildEulerRotation3x3(def.anglesDeg, rot);
    for (int r = 0; r < 3; r++)
    {
        outWorldAxis[r] = rot[r][0] * unitLocal[0] + rot[r][1] * unitLocal[1] + rot[r][2] * unitLocal[2];
    }
}

bool ParseEditorFgaMemory(std::string_view text, EditorFgaGrid &outGrid, std::string &error)
{
    outGrid = {};
    const std::vector<float> numbers = TokenizeFgaNumbers(text);
    if (numbers.size() < 9u)
    {
        error = "FGA file header is incomplete or contains non-numeric tokens";
        return false;
    }

    const int64_t w = int64_t(std::llround(numbers[0]));
    const int64_t h = int64_t(std::llround(numbers[1]));
    const int64_t d = int64_t(std::llround(numbers[2]));
    if (w <= 0 || h <= 0 || d <= 0)
    {
        error = "FGA grid dimensions must be strictly positive";
        return false;
    }

    const uint64_t voxels = uint64_t(w) * uint64_t(h) * uint64_t(d);
    if (voxels > kMaxFgaVoxels || numbers.size() < 9u + size_t(voxels) * 3u)
    {
        error = "FGA file vector payload does not match grid dimensions";
        return false;
    }

    outGrid.width = static_cast<uint32_t>(w);
    outGrid.height = static_cast<uint32_t>(h);
    outGrid.depth = static_cast<uint32_t>(d);
    std::copy_n(numbers.data() + 3, 3, outGrid.boundsMin);
    std::copy_n(numbers.data() + 6, 3, outGrid.boundsMax);
    outGrid.vectorsXYZ.assign(numbers.begin() + 9, numbers.begin() + 9 + ptrdiff_t(voxels * 3u));
    return true;
}

bool LoadEditorFgaFile(const std::string &path, const EditorAssetSource *assets,
                       EditorFgaGrid &outGrid, std::string &error)
{
    outGrid = {};
    if (path.empty())
    {
        error = "FGA path is empty";
        return false;
    }

    std::error_code ec;
    const std::filesystem::path fsPath(path);
    if (std::filesystem::is_regular_file(fsPath, ec))
    {
        std::ifstream file(fsPath, std::ios::binary);
        if (!file)
        {
            error = "Unable to open FGA file: " + path;
            return false;
        }
        std::ostringstream ss;
        ss << file.rdbuf();
        return ParseEditorFgaMemory(ss.str(), outGrid, error);
    }

    if (assets != nullptr)
    {
        std::vector<uint8_t> bytes;
        if (assets->ReadFile(path, bytes))
        {
            const std::string_view sv(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            return ParseEditorFgaMemory(sv, outGrid, error);
        }
    }

    error = "FGA file not found: " + path;
    return false;
}

void ReadEditorLevelParticlesYaml(std::string_view yamlText, const std::string &levelKey,
                                  std::vector<EditorParticleEmitterDef> &outEmitters,
                                  std::vector<EditorVectorFieldDef> &outFields)
{
    outEmitters.clear();
    outFields.clear();
    const std::vector<std::string_view> lines = SplitLinesKeepEnd(yamlText);
    size_t head = 0, end = 0;
    if (!FindLevelSectionRange(lines, levelKey, head, end))
    {
        return;
    }

    int active = 0; // 0 = none, 1 = emitters, 2 = fields
    for (size_t i = head; i < end; i++)
    {
        const std::string_view raw = StripLineEnd(lines[i]);
        if (!IsMeaningfulYamlLine(raw))
        {
            continue;
        }
        const std::string_view trimmed = TrimSv(raw);
        if (LeadingSpaces(raw) <= 2)
        {
            std::string_view k, v;
            SplitKeyVal(trimmed, k, v);
            active = (k == "particle_emitters") ? 1 : (k == "vector_fields" ? 2 : 0);
            continue;
        }
        if (active == 0)
        {
            continue;
        }

        ApplyParticleItemLine(trimmed, active, outEmitters, outFields);
    }
}

std::string RenderEditorLevelParticlesYaml(const std::vector<EditorParticleEmitterDef> &emitters,
                                           const std::vector<EditorVectorFieldDef> &fields)
{
    std::string out;
    if (!emitters.empty())
    {
        out += "  particle_emitters:\n";
        for (const EditorParticleEmitterDef &e : emitters)
        {
            AppendEmitterYaml(out, e);
        }
    }
    if (!fields.empty())
    {
        out += "  vector_fields:\n";
        for (const EditorVectorFieldDef &f : fields)
        {
            AppendVectorFieldYaml(out, f);
        }
    }
    return out;
}

void MergeEditorLevelParticlesYaml(std::string &ioYamlText, const std::string &levelKey,
                                   const std::vector<EditorParticleEmitterDef> &emitters,
                                   const std::vector<EditorVectorFieldDef> &fields)
{
    const std::string rendered = RenderEditorLevelParticlesYaml(emitters, fields);
    const std::vector<std::string_view> lines = SplitLinesKeepEnd(ioYamlText);
    size_t head = 0, end = 0;
    if (!FindLevelSectionRange(lines, levelKey, head, end))
    {
        if (rendered.empty())
        {
            return;
        }
        if (!ioYamlText.empty() && ioYamlText.back() != '\n')
        {
            ioYamlText += '\n';
        }
        ioYamlText += levelKey + ":\n" + rendered;
        return;
    }

    bool hadBlocks = false;
    std::string merged = RewriteLevelParticleLines(lines, head, end, rendered, hadBlocks);
    if (end == lines.size() && !rendered.empty())
    {
        if (!merged.empty() && merged.back() != '\n')
        {
            merged += '\n';
        }
        merged += rendered;
    }
    if (hadBlocks || !rendered.empty())
    {
        ioYamlText = std::move(merged);
    }
}
