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

#include "EditorSession.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "Assets.h"

namespace {

// The header of a target that does not exist yet. A file that does exist keeps its own header:
// the merge never touches a line it does not own.
const char kMaterialsHeader[] =
    "# Global material definitions for the qray ray-traced renderer.\n"
    "# Written by the QuakeRay editor session: the entries below carry the keys the editor\n"
    "# authored, merged into whatever this file already had.\n";

std::string FormatFloat(float value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.6g", double(value));
    return buffer;
}

std::string Lowercase(std::string_view text)
{
    std::string lowered(text);

    for (char &c : lowered)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = char(c - 'A' + 'a');
        }
    }

    return lowered;
}

// The name a world material carries in the game's file: the miptex the BSP stores is keyed with
// the `textures/` prefix there, so a scene name is written in that form and read in either, which
// is also what lets a file an older editor wrote keep resolving.
std::string CanonicalMaterialName(const std::string &name)
{
    static const std::string_view prefix = "textures/";
    const std::string lowered = Lowercase(name);

    if (lowered.compare(0, prefix.size(), prefix) == 0 ||
        lowered.compare(0, 6, "progs/") == 0 ||
        lowered.compare(0, 7, "models/") == 0 ||
        lowered.compare(0, 4, "gfx/") == 0 ||
        lowered.compare(0, 8, "sprites/") == 0 ||
        lowered.compare(0, 4, "env/") == 0 ||
        lowered.compare(0, 8, "scripts/") == 0 ||
        lowered.find(':') != std::string::npos)
    {
        return name;
    }

    return std::string(prefix) + name;
}

// A frame of an animated texture: Quake's `+0tex` .. `+9tex` and the `+atex` .. `+jtex` the
// engines that ran out of digits wrote. Note that stripped base names like `+basebtn` also begin
// with letters in `a..j`, so lookup always tries `+0` whenever index 1 is non-numeric.
bool IsLikelyExplicitFramePrefix(char c)
{
    const char lower = (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    return (lower >= '0' && lower <= '9') || lower == 'a';
}

std::string StarToHash(std::string text)
{
    for (char &c : text)
    {
        if (c == '*')
        {
            c = '#';
        }
    }

    return text;
}

// The key a material is written under: the game's per-miptex form, so an animated family with no
// entry yet is created as the frame the game reads its light from rather than as a name no lookup
// can reach.
std::string MaterialKeyName(const std::string &name)
{
    const std::string normalized = StarToHash(name);

    if (normalized.size() >= 2 && normalized[0] == '+' && !IsLikelyExplicitFramePrefix(normalized[1]))
    {
        return CanonicalMaterialName("+0" + normalized.substr(1));
    }

    return CanonicalMaterialName(normalized);
}

// The names one scene material can have in the file. Both per-miptex frame names (`+0basebtn`,
// `+1basebtn`, `+abasebtn`) and stripped family names (`+basebtn`, `+planet`) may be queried, and
// either form may or may not carry the textures/ prefix. Quake's `*liquid` prefix also swaps to
// `#liquid` in texture names (e.g. `*teleport` <-> `textures/#teleport`).
size_t MaterialNameCandidates(const std::string &name, std::string *out, size_t maxOut)
{
    size_t count = 0;

    auto add = [&](const std::string &value) {
        const std::string lowered = Lowercase(value);

        for (size_t i = 0; i < count; i++)
        {
            if (out[i] == lowered)
            {
                return;
            }
        }

        if (count < maxOut)
        {
            out[count++] = lowered;
        }
    };

    const std::string hashed = StarToHash(name);
    add(CanonicalMaterialName(hashed));
    add(hashed);
    add(CanonicalMaterialName(name));
    add(name);

    if (hashed.size() >= 2 && hashed[0] == '+' &&
        !std::isdigit(static_cast<unsigned char>(hashed[1])))
    {
        add(CanonicalMaterialName("+0" + hashed.substr(1)));
        add("+0" + hashed.substr(1));
    }

    return count;
}

// foundLower is a name as the file spells it, already lowercased.
bool MaterialNameMatches(const std::string &entryName, const std::string &foundLower)
{
    std::string candidates[8];
    const size_t count = MaterialNameCandidates(entryName, candidates, 8);

    for (size_t i = 0; i < count; i++)
    {
        if (candidates[i] == foundLower)
        {
            return true;
        }
    }

    return false;
}

bool IsSpace(char c)
{
    return c == ' ' || c == '\t';
}

size_t LeadingWhitespace(std::string_view line)
{
    size_t count = 0;

    while (count < line.size() && IsSpace(line[count]))
    {
        count++;
    }

    return count;
}

// A line that belongs to the body of the entry above it: indented, or empty. The game's merge
// scanner treats both the same way, which is what keeps an indented comment with its entry.
bool IsBodyLine(std::string_view line)
{
    if (line.empty())
    {
        return true;
    }

    if (IsSpace(line[0]))
    {
        return true;
    }

    return line[0] == '\r' || line[0] == '\n';
}

// The `- name: <value>` head of an entry.
bool NameOf(std::string_view line, std::string &name)
{
    const size_t start = LeadingWhitespace(line);

    if (line.compare(start, 7, "- name:") != 0)
    {
        return false;
    }

    size_t value = start + 7;

    while (value < line.size() && IsSpace(line[value]))
    {
        value++;
    }

    size_t end = value;

    while (end < line.size() && !IsSpace(line[end]) && line[end] != '\r' && line[end] != '\n')
    {
        end++;
    }

    if (end == value)
    {
        return false;
    }

    name = Lowercase(line.substr(value, end - value));
    return true;
}

// The key of a body line, or an empty string when the line is not `key: value` (a list item, a
// comment, a nested block).
std::string_view KeyOf(std::string_view line)
{
    const size_t start = LeadingWhitespace(line);

    if (start >= line.size() || line[start] == '#' || line[start] == '-')
    {
        return {};
    }

    size_t end = start;

    while (end < line.size() && (std::isalnum(uint8_t(line[end])) || line[end] == '_'))
    {
        end++;
    }

    if (end == start || end >= line.size() || line[end] != ':')
    {
        return {};
    }

    return line.substr(start, end - start);
}

// A root key the game's writer renamed in the current format: an old file is migrated while it is
// merged instead of keeping two roots that mean the same thing. Every other line is copied as it
// was, so a file the editor does not fully understand still round-trips.
std::string MigratedRootLine(std::string_view line)
{
    const size_t start = LeadingWhitespace(line);
    size_t end = line.size();

    // The view keeps its terminator, so the comparison trims it along with the trailing blanks.
    while (end > start && (IsSpace(line[end - 1]) || line[end - 1] == '\r' || line[end - 1] == '\n'))
    {
        end--;
    }

    if (start == 0)
    {
        const std::string_view key = line.substr(start, end - start);

        if (key == "materials:")
        {
            return "qray_materials:\n";
        }

        if (key == "lights:")
        {
            return "qray_lights:\n";
        }
    }

    return std::string(line);
}

std::string_view ValueOf(std::string_view line)
{
    const size_t colon = line.find(':');

    if (colon == std::string_view::npos)
    {
        return {};
    }

    size_t start = colon + 1;

    while (start < line.size() && IsSpace(line[start]))
    {
        start++;
    }

    size_t end = start;

    while (end < line.size() && line[end] != '\r' && line[end] != '\n')
    {
        if (line[end] == '#' && (end == start || IsSpace(line[end - 1])))
        {
            break;
        }

        end++;
    }

    while (end > start && IsSpace(line[end - 1]))
    {
        end--;
    }

    return line.substr(start, end - start);
}

float FloatOf(std::string_view value, float fallback)
{
    const std::string text(value);
    char *end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);

    return end == text.c_str() ? fallback : parsed;
}

bool BoolOf(std::string_view value, bool fallback)
{
    const std::string text = Lowercase(value);

    if (text == "true" || text == "1" || text == "yes")
    {
        return true;
    }

    if (text == "false" || text == "0" || text == "no")
    {
        return false;
    }

    return fallback;
}

// A text value: the writer spells it bare, but a hand-written file may quote it, and the game's
// NONE means "keep the map's own image", which is what the empty string stands for here.
std::string TextOf(std::string_view value)
{
    std::string text(value);

    if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') &&
        text.back() == text.front())
    {
        text = text.substr(1, text.size() - 2);
    }

    return Lowercase(text) == "none" ? std::string() : text;
}

// The blend mode by name or by number, the aliases RT_MAT_ParseEmissiveBlend accepts: "cvar"
// and anything unknown keep following the global setting.
int32_t EmissiveBlendOf(std::string_view value, int32_t fallback)
{
    const std::string text = Lowercase(value);

    if (text == "cvar")
    {
        return -1;
    }

    if (text == "off" || text == "none")
    {
        return 0;
    }

    if (text == "normal" || text == "coverage")
    {
        return 1;
    }

    if (text == "screen" || text == "add" || text == "additive")
    {
        return 2;
    }

    if (text == "overlay")
    {
        return 3;
    }

    if (text == "hard light" || text == "hard_light" || text == "hardlight")
    {
        return 4;
    }

    if (text == "color dodge" || text == "color_dodge" || text == "colordodge")
    {
        return 5;
    }

    const std::string number(text);
    char *end = nullptr;
    const long parsed = std::strtol(number.c_str(), &end, 10);

    if (end == number.c_str())
    {
        return fallback;
    }

    return int32_t(std::clamp<long>(parsed, -1, 5));
}

// The name RT_MAT_EmissiveBlendName reports for a stored mode, so a re-written file reads back
// as the same mode whether it came from the game or from here.
const char *BlendNameOf(int32_t blend)
{
    const char *const names[] = { "off",        "normal", "screen",
                                  "overlay",    "hard light", "color dodge" };

    return blend >= 0 && blend <= 5 ? names[blend] : "cvar";
}

// `rrggbb`, the only form RT_CustomColorFromString reads back.
std::string FormatColor(const float color[3])
{
    char buffer[16];
    int channels[3];

    for (int i = 0; i < 3; i++)
    {
        const float clamped = std::min(1.0f, std::max(0.0f, color[i]));
        channels[i] = int(clamped * 255.0f + 0.5f) & 0xff;
    }

    std::snprintf(buffer, sizeof(buffer), "%02x%02x%02x", channels[0], channels[1], channels[2]);
    return buffer;
}

void AppendSssOwnedValues(const EditorMaterialParams &params,
                          std::vector<std::pair<const char *, std::string>> &values)
{
    if (!params.textureSss.empty())
    {
        values.emplace_back("texture_sss", params.textureSss);
    }
    if (params.sssStrength != 0.0f)
    {
        values.emplace_back("sss_strength", FormatFloat(params.sssStrength));
    }
    const bool defaultSssColor = params.sssColor[0] == 1.0f && params.sssColor[1] == 0.35f &&
                                 params.sssColor[2] == 0.2f;
    if (!defaultSssColor)
    {
        values.emplace_back("sss_color", FormatColor(params.sssColor));
    }
    if (params.sssRadius != 0.02f)
    {
        values.emplace_back("sss_radius", FormatFloat(params.sssRadius));
    }
    if (!params.sssBlocks.empty())
    {
        values.emplace_back("sss", std::string());
    }
    if (params.sssThreshold != 0.02f)
    {
        values.emplace_back("sss_threshold", FormatFloat(params.sssThreshold));
    }
    if (params.sssFeather != 0.0f)
    {
        values.emplace_back("sss_feather", FormatFloat(params.sssFeather));
    }
}

void AppendOwnedLightAndShaftTailValues(const EditorMaterialParams &params,
                                        std::vector<std::pair<const char *, std::string>> &values)
{
    if (params.lightMinArea > 0.0f)
    {
        values.emplace_back("light_min_area", FormatFloat(params.lightMinArea));
    }
    if (params.disabledStagesMask != 0u)
    {
        values.emplace_back("disabled_stages", std::to_string(params.disabledStagesMask));
    }
    if (params.lightShafts)
    {
        values.emplace_back("light_shafts", "true");
    }
    if (params.lightShaftsPower != 1.0f)
    {
        values.emplace_back("light_shafts_power", FormatFloat(params.lightShaftsPower));
    }
    if (params.lightShaftsDistance > 0.0f)
    {
        values.emplace_back("light_shafts_distance", FormatFloat(params.lightShaftsDistance));
    }
    if (params.lightShaftsFalloff != 1.0f)
    {
        values.emplace_back("light_shafts_falloff", FormatFloat(params.lightShaftsFalloff));
    }
}

void AppendOwnedTailValues(const EditorMaterialParams &params,
                           std::vector<std::pair<const char *, std::string>> &values)
{
    if (!params.enabled)
    {
        values.emplace_back("enabled", "false");
    }
    AppendOwnedLightAndShaftTailValues(params, values);
    if (params.hasColorEmissive && !params.emissiveBlocks.empty())
    {
        values.emplace_back("luma", std::string());
    }
    if (params.lumaThreshold != 0.02f)
    {
        values.emplace_back("luma_threshold", FormatFloat(params.lumaThreshold));
    }
    if (params.lumaFeather != 0.0f)
    {
        values.emplace_back("luma_feather", FormatFloat(params.lumaFeather));
    }
    if (params.hasColorGloss && !params.glossBlocks.empty())
    {
        values.emplace_back("gloss", std::string());
    }
    if (params.glossThreshold != 0.02f)
    {
        values.emplace_back("gloss_threshold", FormatFloat(params.glossThreshold));
    }
    if (params.glossFeather != 0.0f)
    {
        values.emplace_back("gloss_feather", FormatFloat(params.glossFeather));
    }
    AppendSssOwnedValues(params, values);
}

// The owned keys of `params`, in canonical order, as `key`/`value` pairs. A key at its file
// default is left out, which is what keeps a save from growing the file by eleven dead lines.
std::vector<std::pair<const char *, std::string>> OwnedValues(const EditorMaterialParams &params)
{
    std::vector<std::pair<const char *, std::string>> values;

    auto append = [&values](const char *key, const std::string &value) {
        values.emplace_back(key, value);
    };

    // The game writes the diffuse image first, before the scale and offset pair.
    if (!params.baseTexture.empty())
    {
        append("texture_base", params.baseTexture);
    }

    if (!params.normalTexture.empty())
    {
        append("texture_normals", params.normalTexture);
    }

    if (!params.emissiveTexture.empty())
    {
        append("texture_emissive", params.emissiveTexture);
    }

    if (!params.overlayTexture.empty())
    {
        append("texture_overlay", params.overlayTexture);
    }

    if (!params.glossTexture.empty())
    {
        append("texture_gloss", params.glossTexture);
    }

    if (params.textureScaleU != 1.0f)
    {
        append("texture_scale_u", FormatFloat(params.textureScaleU));
    }

    if (params.textureScaleV != 1.0f)
    {
        append("texture_scale_v", FormatFloat(params.textureScaleV));
    }

    if (params.textureOffsetU != 0.0f)
    {
        append("texture_offset_u", FormatFloat(params.textureOffsetU));
    }

    if (params.textureOffsetV != 0.0f)
    {
        append("texture_offset_v", FormatFloat(params.textureOffsetV));
    }

    if (params.bumpScale != 1.0f)
    {
        append("bump_scale", FormatFloat(params.bumpScale));
    }

    if (params.roughnessOverride >= 0.0f)
    {
        append("roughness_override", FormatFloat(params.roughnessOverride));
    }

    if (params.metalnessFactor != 0.0f)
    {
        append("metalness_factor", FormatFloat(params.metalnessFactor));
    }

    if (params.metalnessFromNormalAlpha)
    {
        append("metalness_from_normal_alpha", "true");
    }

    // rt_material.c's default is 1, so only a value that differs from it has to be written.
    if (params.emissiveFactor != 1.0f)
    {
        append("emissive_factor", FormatFloat(params.emissiveFactor));
    }

    // The game stores -1 for "follow the global rt_emis_blend" and writes the mode by name, so a
    // file the game wrote and the editor re-writes keeps reading the same.
    if (params.lumaBlend >= 0)
    {
        append("luma_blend", BlendNameOf(params.lumaBlend));
    }

    if (params.baseFactor != 1.0f)
    {
        append("base_factor", FormatFloat(params.baseFactor));
    }

    // The editor's Luma checkbox uploads QR_GEOMETRY_UPLOAD_EMISSION_RAY_ONLY_BIT, and the game
    // reaches the same batch flag through is_light, so the file's own key is the one written.
    if (params.luma)
    {
        append("is_light", "true");
    }

    // The game writes the mirror next to the pass-through it overrides, then the pane's own
    // fields under material_glass only -- the same conditions its QRE_WriteMaterial uses.
    if (params.mirror)
    {
        append("mirror", "true");
    }

    if (params.type == EditorMaterialType::Glass)
    {
        append("material_glass", "true");
    }
    else if (params.type == EditorMaterialType::Water)
    {
        append("material_water", "true");
    }

    if (params.type == EditorMaterialType::Glass)
    {
        if (params.glassIor > 0.0f)
        {
            append("glass_ior", FormatFloat(params.glassIor));
        }

        if (params.glassThickness != 2.0f)
        {
            append("glass_thickness", FormatFloat(params.glassThickness));
        }

        if (params.transparency != 1.0f)
        {
            append("transparency", FormatFloat(params.transparency));
        }

        if (params.glassColor[0] != 1.0f || params.glassColor[1] != 1.0f ||
            params.glassColor[2] != 1.0f)
        {
            append("glass_color", FormatColor(params.glassColor));
        }
    }

    if (params.exactNormals)
    {
        append("exact_normals", "true");
    }

    if (params.forceRasterize)
    {
        append("force_rasterize", "true");
    }

    // QRE_WriteMaterial writes the light group's own keys, in this order, after the blocks.
    if (params.lightStyles)
    {
        append("light_styles", "true");
    }

    if (params.hasLightColor)
    {
        append("light_color", FormatColor(params.lightColor));
    }

    if (params.lightBrightness != 1.0f)
    {
        append("light_brightness", FormatFloat(params.lightBrightness));
    }

    if (params.lightFocus > 0.0f)
    {
        append("light_focus", FormatFloat(params.lightFocus));
    }

    if (params.lightFocusSoft >= 0.0f)
    {
        append("light_focus_soft", FormatFloat(params.lightFocusSoft));
    }

    if (params.lightProjector)
    {
        append("light_projector", "true");
    }

    AppendOwnedTailValues(params, values);
    return values;
}

void AppendBrushRleTokens(std::string &out, const std::vector<uint8_t> &mask)
{
    size_t i = 0;
    while (i < mask.size())
    {
        const uint8_t value = mask[i];
        size_t count = 1;
        while (i + count < mask.size() && mask[i + count] == value)
        {
            count++;
        }

        if (i > 0)
        {
            out += ',';
        }

        char tok[32];
        if (count > 1)
        {
            std::snprintf(tok, sizeof(tok), "%zu*%02x", count, unsigned(value));
        }
        else
        {
            std::snprintf(tok, sizeof(tok), "%02x", unsigned(value));
        }
        out += tok;
        i += count;
    }
}

template <typename Block>
void AppendPolygonOrBrushYaml(std::string &out, const std::string &indent, const Block &block)
{
    if (IsBrushBlock(block))
    {
        char dims[32];
        std::snprintf(dims, sizeof(dims), "%ux%u ", block.brushWidth, block.brushHeight);
        out += indent + "  - brush: ";
        out += dims;
        AppendBrushRleTokens(out, block.brushMask);
        out += '\n';
        return;
    }

    if (IsPolygonBlock(block))
    {
        char buffer[64];
        out += indent + "  - polygon:";

        for (size_t p = 0; p + 1 < block.polyUv.size(); p += 2)
        {
            std::snprintf(buffer, sizeof(buffer), " %.4f,%.4f", double(block.polyUv[p]),
                          double(block.polyUv[p + 1]));
            out += buffer;
        }

        out += '\n';
        return;
    }

    out += indent + "  - color: " + FormatColor(block.color) + "\n";
    out += indent + "    threshold: " + FormatFloat(block.threshold) + "\n";
}

void AppendLumaBlocksYaml(std::string &out, const std::string &indent,
                          const EditorMaterialParams &params)
{
    out += indent + "luma:\n";

    for (const EditorEmissiveBlock &block : params.emissiveBlocks)
    {
        AppendPolygonOrBrushYaml(out, indent, block);
        out += indent + "    feather: " + FormatFloat(block.feather) + "\n";
        out += indent + "    emissive_factor: " + FormatFloat(block.factor) + "\n";
        out += indent + "    blend: " + BlendNameOf(block.blend) + "\n";
    }
}

void AppendGlossBlocksYaml(std::string &out, const std::string &indent,
                           const EditorMaterialParams &params)
{
    out += indent + "gloss:\n";

    for (const EditorGlossBlock &block : params.glossBlocks)
    {
        AppendPolygonOrBrushYaml(out, indent, block);
        out += indent + "    feather: " + FormatFloat(block.feather) + "\n";
        out += indent + "    roughness: " + FormatFloat(block.roughness) + "\n";
        out += indent + "    metalness: " + FormatFloat(block.metalness) + "\n";
    }
}

template <typename Block>
void AppendFactorBlocksYaml(std::string &out, const std::string &indent, const char *headerKey,
                            const std::vector<Block> &blocks)
{
    out += indent + headerKey + ":\n";

    for (const Block &block : blocks)
    {
        AppendPolygonOrBrushYaml(out, indent, block);
        out += indent + "    feather: " + FormatFloat(block.feather) + "\n";
        out += indent + "    factor: " + FormatFloat(block.factor) + "\n";
    }
}

// One owned key as YAML. Every key but `luma`, `gloss` and `sss` is a scalar
// on its own line; `luma`, `gloss` and `sss` are lists of colour / polygon blocks.
void AppendOwnedValue(std::string &out, const std::string &indent, const char *key,
                      const std::string &value, const EditorMaterialParams &params)
{
    const std::string_view keyView(key);

    if (keyView == "luma")
    {
        AppendLumaBlocksYaml(out, indent, params);
        return;
    }

    if (keyView == "gloss")
    {
        AppendGlossBlocksYaml(out, indent, params);
        return;
    }

    if (keyView == "sss")
    {
        AppendFactorBlocksYaml(out, indent, "sss", params.sssBlocks);
        return;
    }


    out += indent;
    out += key;
    out += ": ";
    out += value;
    out += '\n';
}

// A line holding nothing but whitespace: inside a nested block it belongs to that block.
bool IsBlankLine(std::string_view line)
{
    return line.find_first_not_of(" \t\r") == std::string_view::npos;
}

struct TextLine
{
    size_t offset = 0;
    size_t length = 0;

    std::string_view View(const std::string &text) const { return { text.data() + offset, length }; }
};

std::vector<TextLine> SplitLines(const std::string &text)
{
    std::vector<TextLine> lines;
    size_t start = 0;

    while (start < text.size())
    {
        size_t end = text.find('\n', start);

        if (end == std::string::npos)
        {
            end = text.size();
        }
        else
        {
            end++;
        }

        lines.push_back(TextLine{ start, end - start });
        start = end;
    }

    return lines;
}

// The inverse of FormatColor: six hex digits, and anything else leaves `color` untouched.
void ParseColor(std::string_view value, float color[3])
{
    size_t start = 0;

    while (start < value.size() && (value[start] == ' ' || value[start] == '#' || value[start] == '\t'))
    {
        start++;
    }

    const std::string text(value.substr(start));

    unsigned int red = 0;
    unsigned int green = 0;
    unsigned int blue = 0;

    // The game's own reader accepts six hex digits and nothing else, so a malformed value leaves
    // the colour alone rather than turning into black.
    if (text.size() != 6 || std::sscanf(text.c_str(), "%2x%2x%2x", &red, &green, &blue) != 3)
    {
        return;
    }

    color[0] = float(red) / 255.0f;
    color[1] = float(green) / 255.0f;
    color[2] = float(blue) / 255.0f;
}

// The item of a `luma` list, key by key, the way rt_material.c reads a mapping node: a colour or a
// polygon selects the pixels and the four tone controls replace what the item inherited. `has`
// records which of those the file authored, so an omitted one can inherit the material's after the
// whole entry has been read.
enum EmissiveBlockOwned : uint32_t
{
    kOwnedThreshold = 1u << 0,
    kOwnedFeather = 1u << 1,
    kOwnedFactor = 1u << 2,
    kOwnedBlend = 1u << 3,
};

std::string_view Trimmed(std::string_view text)
{
    while (!text.empty() && (IsSpace(text.front()) || text.front() == '{' || text.front() == '\t'))
    {
        text.remove_prefix(1);
    }

    while (!text.empty() && (IsSpace(text.back()) || text.back() == '}' || text.back() == '\r' ||
                             text.back() == '\n'))
    {
        text.remove_suffix(1);
    }

    return text;
}

// RT_MAT_EMIS_POLY_MAX points of a `- polygon:` value, clamped to the unit square; fewer than
// three of them are no polygon at all, which is the reset the loader does.
void ReadUvPolygon(std::string_view value, std::vector<float> &outPolyUv)
{
    outPolyUv.clear();

    const std::string text(value);
    const char *cursor = text.c_str();

    while (*cursor && outPolyUv.size() < 32)
    {
        char *end = nullptr;
        const float u = std::strtof(cursor, &end);

        if (end == cursor)
        {
            break;
        }

        cursor = end;

        while (*cursor == ' ' || *cursor == '\t' || *cursor == ',')
        {
            cursor++;
        }

        const float v = std::strtof(cursor, &end);

        if (end == cursor)
        {
            break;
        }

        cursor = end;
        outPolyUv.push_back(std::clamp(u, 0.0f, 1.0f));
        outPolyUv.push_back(std::clamp(v, 0.0f, 1.0f));

        while (*cursor == ' ' || *cursor == '\t')
        {
            cursor++;
        }
    }

    if (outPolyUv.size() < 6)
    {
        outPolyUv.clear();
    }
}

bool ParseBrushUint(std::string_view text, size_t &pos, uint64_t &out)
{
    if (pos >= text.size() || !std::isdigit(uint8_t(text[pos])))
    {
        return false;
    }

    uint64_t val = 0;
    while (pos < text.size() && std::isdigit(uint8_t(text[pos])))
    {
        const uint64_t digit = uint64_t(text[pos] - '0');
        if (val > (4096ull * 4096ull - digit) / 10ull)
        {
            return false;
        }
        val = val * 10ull + digit;
        pos++;
    }
    out = val;
    return true;
}

bool ParseBrushHexByte(std::string_view hex, uint8_t &out)
{
    if (hex.size() != 2)
    {
        return false;
    }

    uint8_t val = 0;
    for (char c : hex)
    {
        val <<= 4;
        if (c >= '0' && c <= '9')
        {
            val |= uint8_t(c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            val |= uint8_t(c - 'a' + 10);
        }
        else if (c >= 'A' && c <= 'F')
        {
            val |= uint8_t(c - 'A' + 10);
        }
        else
        {
            return false;
        }
    }
    out = val;
    return true;
}

std::string_view TrimBrushWhitespace(std::string_view text)
{
    while (!text.empty() && (IsSpace(text.front()) || text.front() == '\r' || text.front() == '\n'))
    {
        text.remove_prefix(1);
    }

    while (!text.empty() && (IsSpace(text.back()) || text.back() == '\r' || text.back() == '\n'))
    {
        text.remove_suffix(1);
    }

    return text;
}

float ParseBlockFeather(std::string_view value, float fallback)
{
    const float pixels = FloatOf(value, fallback);
    if (!std::isfinite(pixels))
    {
        return std::clamp(fallback, 0.0f, 16.0f);
    }
    return float(std::lround(std::clamp(pixels, 0.0f, 16.0f)));
}

bool ParseBrushRleToken(std::string_view token, size_t remaining, size_t &outCount,
                        uint8_t &outByte)
{
    token = TrimBrushWhitespace(token);
    if (token.empty())
    {
        return false;
    }

    const size_t star = token.find('*');
    if (star == std::string_view::npos)
    {
        outCount = 1;
        return outCount <= remaining && ParseBrushHexByte(token, outByte);
    }

    size_t pos = 0;
    uint64_t count = 0;
    if (!ParseBrushUint(token.substr(0, star), pos, count) || pos != star || count == 0 ||
        count > remaining)
    {
        return false;
    }
    outCount = size_t(count);
    return ParseBrushHexByte(token.substr(star + 1), outByte);
}

bool IsXSeparator(char ch)
{
    return ch == 'x' || ch == 'X';
}

bool ParseBrushDims(std::string_view value, size_t &pos, uint32_t &outW, uint32_t &outH)
{
    uint64_t w = 0;
    uint64_t h = 0;
    if (!ParseBrushUint(value, pos, w) || pos >= value.size() || !IsXSeparator(value[pos]))
    {
        return false;
    }
    pos++;
    if (!ParseBrushUint(value, pos, h) || w == 0 || h == 0 || w * h > 4096ull * 4096ull)
    {
        return false;
    }
    if (pos >= value.size() || !IsSpace(value[pos]))
    {
        return false;
    }
    while (pos < value.size() && IsSpace(value[pos]))
    {
        pos++;
    }
    outW = uint32_t(w);
    outH = uint32_t(h);
    return true;
}

bool DecodeBrushRleStream(std::string_view stream, size_t total, std::vector<uint8_t> &decoded)
{
    decoded.reserve(std::min<size_t>(total, 65536));
    size_t pos = 0;

    while (pos <= stream.size())
    {
        const size_t comma = stream.find(',', pos);
        const std::string_view token =
            comma == std::string_view::npos ? stream.substr(pos) : stream.substr(pos, comma - pos);
        size_t count = 0;
        uint8_t byteVal = 0;
        if (!ParseBrushRleToken(token, total - decoded.size(), count, byteVal))
        {
            return false;
        }
        decoded.insert(decoded.end(), count, byteVal);
        if (comma == std::string_view::npos)
        {
            break;
        }
        pos = comma + 1;
    }

    return decoded.size() == total;
}

void ReadBrushMask(std::string_view value, uint32_t &outWidth, uint32_t &outHeight,
                   std::vector<uint8_t> &outMask)
{
    outWidth = 0;
    outHeight = 0;
    outMask.clear();

    size_t pos = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    if (!ParseBrushDims(value, pos, w, h))
    {
        return;
    }

    const size_t total = size_t(w) * size_t(h);
    std::vector<uint8_t> decoded;
    if (!DecodeBrushRleStream(value.substr(pos), total, decoded))
    {
        return;
    }

    outWidth = w;
    outHeight = h;
    outMask = std::move(decoded);
}

template <typename Block>
bool ApplyCommonMaskBlockKey(std::string_view key, std::string_view value, Block &block,
                             uint32_t &has)
{
    if (key == "color")
    {
        ParseColor(value, block.color);
        return true;
    }
    if (key == "polygon")
    {
        ReadUvPolygon(value, block.polyUv);
        return true;
    }
    if (key == "brush")
    {
        ReadBrushMask(value, block.brushWidth, block.brushHeight, block.brushMask);
        return true;
    }
    if (key == "threshold")
    {
        block.threshold = FloatOf(value, block.threshold);
        has |= kOwnedThreshold;
        return true;
    }
    if (key == "feather")
    {
        // The loader rounds to whole pixels and clamps to the 16 the blur can reach.
        block.feather = ParseBlockFeather(value, block.feather);
        has |= kOwnedFeather;
        return true;
    }
    return false;
}

void ApplyEmissiveBlockKey(std::string_view key, std::string_view value, EditorEmissiveBlock &block,
                           uint32_t &has)
{
    if (ApplyCommonMaskBlockKey(key, value, block, has))
    {
        return;
    }
    if (key == "emissive_factor" || key == "factor")
    {
        block.factor = FloatOf(value, block.factor);
        has |= kOwnedFactor;
    }
    else if (key == "blend")
    {
        const int32_t blend = EmissiveBlendOf(value, -2);

        if (blend != -2)
        {
            block.blend = blend;
            has |= kOwnedBlend;
        }
    }
}

template <typename Block, size_t MaxBlocks>
void ParseScalarColorBlockList(std::string_view scalar, std::vector<Block> &blocks,
                               std::vector<uint32_t> &has)
{
    size_t start = 0;

    while (start < scalar.size() && blocks.size() < MaxBlocks)
    {
        size_t stop = scalar.find_first_of(", \t", start);

        if (stop == std::string_view::npos)
        {
            stop = scalar.size();
        }

        if (stop > start)
        {
            Block block;
            ParseColor(scalar.substr(start, stop - start), block.color);
            blocks.push_back(block);
            has.push_back(0);
        }

        start = stop + 1;
    }
}

template <typename Block, size_t MaxBlocks, typename ApplyFn>
void ParseNestedMaskBlockLine(std::string_view item, std::vector<Block> &blocks,
                              std::vector<uint32_t> &has, size_t &current, ApplyFn applyKey)
{
    if (item.size() >= 2 && item.front() == '-' && IsSpace(item[1]))
    {
        // Past the loader's cap the items are skipped rather than written into a block the
        // game would not read anyway.
        if (blocks.size() >= MaxBlocks)
        {
            current = size_t(-1);
            return;
        }

        blocks.emplace_back();
        has.push_back(0);
        current = blocks.size() - 1;
        item = Trimmed(item.substr(1));
    }

    if (current == size_t(-1))
    {
        return;
    }

    const size_t colon = item.find(':');

    if (colon != std::string_view::npos)
    {
        applyKey(Trimmed(item.substr(0, colon)), Trimmed(item.substr(colon + 1)),
                 blocks[current], has[current]);
    }
}

struct YamlEntrySlice
{
    const std::string &text;
    const std::vector<TextLine> &lines;
    size_t begin;
    size_t end;
    size_t keyIndent;
};

template <typename Block, size_t MaxBlocks, typename HeaderFn, typename ApplyFn>
void ReadMaskBlockList(const YamlEntrySlice &slice, std::vector<Block> &blocks,
                       std::vector<uint32_t> &has, HeaderFn isHeader, ApplyFn applyKey)
{
    for (size_t i = slice.begin; i < slice.end; i++)
    {
        const std::string_view line = slice.lines[i].View(slice.text);

        if (LeadingWhitespace(line) != slice.keyIndent || !isHeader(KeyOf(line)))
        {
            continue;
        }

        const std::string_view scalar = Trimmed(ValueOf(line));

        if (!scalar.empty())
        {
            ParseScalarColorBlockList<Block, MaxBlocks>(scalar, blocks, has);
            continue;
        }

        size_t current = size_t(-1);

        for (size_t k = i + 1; k < slice.end; k++)
        {
            const std::string_view nested = slice.lines[k].View(slice.text);

            if (IsBlankLine(nested))
            {
                continue;
            }

            const size_t indent = LeadingWhitespace(nested);

            if (indent <= slice.keyIndent)
            {
                break;
            }

            ParseNestedMaskBlockLine<Block, MaxBlocks>(nested.substr(indent), blocks, has,
                                                       current, applyKey);
        }
    }
}

bool IsLumaBlockHeaderKey(std::string_view key)
{
    return key == "luma" || key == "color_emissive";
}

// Reads the `luma` key of one entry in either of its authored forms: the list of colour / polygon
// blocks, or the legacy `luma: ff0000,00ff00` scalar the game still accepts.
void ReadEmissiveBlocks(const std::string &text, const std::vector<TextLine> &lines, size_t begin,
                        size_t end, size_t keyIndent, EditorMaterialParams &params)
{
    std::vector<uint32_t> has;
    const YamlEntrySlice slice{ text, lines, begin, end, keyIndent };
    ReadMaskBlockList<EditorEmissiveBlock, kEditorEmissiveBlockMax>(
        slice, params.emissiveBlocks, has, IsLumaBlockHeaderKey, ApplyEmissiveBlockKey);

    // A block that carries no tone control of its own takes the material-level one, which is what
    // the loader does once it has read every key of the entry.
    for (size_t b = 0; b < params.emissiveBlocks.size() && b < has.size(); b++)
    {
        EditorEmissiveBlock &block = params.emissiveBlocks[b];

        if ((has[b] & kOwnedThreshold) == 0)
        {
            block.threshold = params.lumaThreshold;
        }

        if ((has[b] & kOwnedFeather) == 0)
        {
            block.feather = params.lumaFeather;
        }

        if ((has[b] & kOwnedFactor) == 0)
        {
            block.factor = params.emissiveFactor;
        }

        if ((has[b] & kOwnedBlend) == 0)
        {
            block.blend = params.lumaBlend;
        }
    }
}

bool IsGlossBlockHeaderKey(std::string_view key)
{
    return key == "gloss" || key == "color_gloss";
}

void ApplyGlossBlockKey(std::string_view key, std::string_view value, EditorGlossBlock &block,
                        uint32_t &has)
{
    if (ApplyCommonMaskBlockKey(key, value, block, has))
    {
        return;
    }
    if (key == "roughness")
    {
        block.roughness = std::clamp(FloatOf(value, block.roughness), 0.0f, 1.0f);
    }
    else if (key == "metalness")
    {
        block.metalness = std::clamp(FloatOf(value, block.metalness), 0.0f, 1.0f);
    }
}

void ApplyGlossBlockDefaults(EditorMaterialParams &params, const std::vector<uint32_t> &has)
{
    for (size_t b = 0; b < params.glossBlocks.size() && b < has.size(); b++)
    {
        EditorGlossBlock &block = params.glossBlocks[b];

        if ((has[b] & kOwnedThreshold) == 0)
        {
            block.threshold = params.glossThreshold;
        }

        if ((has[b] & kOwnedFeather) == 0)
        {
            block.feather = params.glossFeather;
        }
    }
}

void ReadGlossBlocks(const std::string &text, const std::vector<TextLine> &lines, size_t begin,
                     size_t end, size_t keyIndent, EditorMaterialParams &params)
{
    std::vector<uint32_t> has;
    const YamlEntrySlice slice{ text, lines, begin, end, keyIndent };
    ReadMaskBlockList<EditorGlossBlock, kEditorGlossBlockMax>(
        slice, params.glossBlocks, has, IsGlossBlockHeaderKey, ApplyGlossBlockKey);
    ApplyGlossBlockDefaults(params, has);
}

bool IsSssBlockHeaderKey(std::string_view key)
{
    return key == "sss";
}

template <typename Block>
void ApplyFactorBlockKey(std::string_view key, std::string_view value, Block &block, uint32_t &has)
{
    if (ApplyCommonMaskBlockKey(key, value, block, has))
    {
        return;
    }
    if (key == "factor")
    {
        block.factor = std::clamp(FloatOf(value, block.factor), 0.0f, 1.0f);
        has |= kOwnedFactor;
    }
}

template <typename Block>
void ApplyFactorBlockDefaults(std::vector<Block> &blocks, float defaultThreshold,
                              float defaultFeather, const std::vector<uint32_t> &has)
{
    for (size_t b = 0; b < blocks.size() && b < has.size(); b++)
    {
        Block &block = blocks[b];

        if ((has[b] & kOwnedThreshold) == 0)
        {
            block.threshold = defaultThreshold;
        }

        if ((has[b] & kOwnedFeather) == 0)
        {
            block.feather = defaultFeather;
        }

        if ((has[b] & kOwnedFactor) == 0)
        {
            block.factor = 1.0f;
        }
    }
}

void ReadSssBlocks(const std::string &text, const std::vector<TextLine> &lines, size_t begin,
                   size_t end, size_t keyIndent, EditorMaterialParams &params)
{
    std::vector<uint32_t> has;
    const YamlEntrySlice slice{ text, lines, begin, end, keyIndent };
    ReadMaskBlockList<EditorSssBlock, kEditorSssBlockMax>(
        slice, params.sssBlocks, has, IsSssBlockHeaderKey, ApplyFactorBlockKey<EditorSssBlock>);
    ApplyFactorBlockDefaults(params.sssBlocks, params.sssThreshold, params.sssFeather, has);
}

void ApplyGlossScalarKey(std::string_view key, std::string_view value, EditorMaterialParams &params)
{
    if (key == "gloss" || key == "color_gloss")
    {
        params.hasColorGloss = true;
    }
    else if (key == "gloss_threshold")
    {
        params.glossThreshold = FloatOf(value, params.glossThreshold);
    }
    else if (key == "gloss_feather")
    {
        params.glossFeather = FloatOf(value, params.glossFeather);
    }
    else if (key == "light_min_area")
    {
        params.lightMinArea = std::max(0.0f, FloatOf(value, params.lightMinArea));
    }
    else if (key == "disabled_stages")
    {
        const float raw = FloatOf(value, 0.0f);
        params.disabledStagesMask = raw > 0.0f ? uint32_t(raw + 0.5f) : 0u;
    }
}

void ApplySssScalarKey(std::string_view key, std::string_view value, EditorMaterialParams &params,
                       bool &hasSssStrength)
{
    if (key == "texture_sss")
    {
        params.textureSss = TextOf(value);
    }
    else if (key == "sss_strength")
    {
        params.sssStrength = std::clamp(FloatOf(value, params.sssStrength), 0.0f, 1.0f);
        hasSssStrength = true;
    }
    else if (key == "sss_color")
    {
        ParseColor(value, params.sssColor);
    }
    else if (key == "sss_radius")
    {
        params.sssRadius = std::clamp(FloatOf(value, params.sssRadius), 0.0f, 1.0f);
    }
    else if (key == "sss_threshold")
    {
        params.sssThreshold = FloatOf(value, params.sssThreshold);
    }
    else if (key == "sss_feather")
    {
        params.sssFeather = FloatOf(value, params.sssFeather);
    }
}

void ResetMaterialBlockState(EditorMaterialParams &params)
{
    params.emissiveBlocks.clear();
    params.hasColorEmissive = false;
    params.glossBlocks.clear();
    params.hasColorGloss = false;
    params.sssBlocks.clear();
}

void ApplyExtraMaterialScalarKey(std::string_view key, std::string_view value,
                                 EditorMaterialParams &params, bool &hasSssStrength)
{
    if (key == "enabled")
    {
        params.enabled = BoolOf(value, params.enabled);
    }
    else if (key == "light_shafts")
    {
        params.lightShafts = BoolOf(value, params.lightShafts);
    }
    else if (key == "light_shafts_power")
    {
        params.lightShaftsPower = std::clamp(FloatOf(value, params.lightShaftsPower), 0.0f, 16.0f);
    }
    else if (key == "light_shafts_distance")
    {
        params.lightShaftsDistance =
            std::clamp(FloatOf(value, params.lightShaftsDistance), 0.0f, 25.0f);
    }
    else if (key == "light_shafts_falloff")
    {
        params.lightShaftsFalloff =
            std::clamp(FloatOf(value, params.lightShaftsFalloff), 0.0f, 4.0f);
    }
    else if (key == "luma" || key == "color_emissive")
    {
        params.hasColorEmissive = true;
    }
    else if (key == "emissive_blend")
    {
        params.lumaBlend = EmissiveBlendOf(value, params.lumaBlend);
    }
    else if (key == "color_emissive_threshold")
    {
        params.lumaThreshold = FloatOf(value, params.lumaThreshold);
    }
    else if (key == "color_emissive_feather")
    {
        params.lumaFeather = FloatOf(value, params.lumaFeather);
    }
    else
    {
        ApplyGlossScalarKey(key, value, params);
        ApplySssScalarKey(key, value, params, hasSssStrength);
    }
}

void FinalizeExtraMaterialRead(EditorMaterialParams &params, bool hasSssStrength)
{
    params.hasColorEmissive = params.hasColorEmissive || !params.emissiveBlocks.empty();
    params.hasColorGloss = params.hasColorGloss || !params.glossBlocks.empty();
    if (!hasSssStrength && (!params.textureSss.empty() || !params.sssBlocks.empty()))
    {
        params.sssStrength = 1.0f;
    }
}

std::string FormatVector(const float value[3])
{
    return FormatFloat(value[0]) + " " + FormatFloat(value[1]) + " " + FormatFloat(value[2]);
}

void ParseVector(std::string_view value, float out[3])
{
    const std::string text(value);

    std::sscanf(text.c_str(), "%f %f %f", &out[0], &out[1], &out[2]);
}

std::string FormatBool(bool value)
{
    return value ? "true" : "false";
}

// `maps/start.bsp` and `start` both name the level `start`, as RT_CustomLights_LevelKey decides.
std::string LevelKeyOf(const std::string &scenePath)
{
    size_t base = 0;

    for (size_t i = 0; i < scenePath.size(); i++)
    {
        if (scenePath[i] == '/' || scenePath[i] == '\\')
        {
            base = i + 1;
        }
    }

    std::string key = scenePath.substr(base);
    const size_t dot = key.find_last_of('.');

    if (dot != std::string::npos)
    {
        key.erase(dot);
    }

    return Lowercase(key);
}

// The keys an entry of the level's `lights:` list owns; `offset:` and any key the game reads but
// the editor does not model are left in the file.
bool IsLightKey(std::string_view key)
{
    static const char *const keys[] = { "origin",
                                        "class",
                                        "classname",
                                        "offset",
                                        "radius",
                                        "intensity",
                                        "color",
                                        "spot",
                                        "dir",
                                        "angle_inner",
                                        "angle_outer",
                                        "light_shafts",
                                        "light_shafts_power",
                                        "light_shafts_distance",
                                        "light_shafts_falloff",
                                        "force_rasterize",
                                        "enabled",
                                        "style" };

    for (const char *owned : keys)
    {
        if (key == owned)
        {
            return true;
        }
    }

    return false;
}

// The key of a list item line, `- key: value` giving `key`, and empty for anything else.
std::string_view ItemKeyOf(std::string_view line)
{
    const size_t start = LeadingWhitespace(line);

    if (start >= line.size() || line[start] != '-')
    {
        return {};
    }

    return KeyOf(line.substr(start + 1));
}

// The `- origin:` head of a light entry, and the origin it carries.
bool LightOriginOf(std::string_view line, float origin[3])
{
    if (ItemKeyOf(line) != "origin")
    {
        return false;
    }

    ParseVector(ValueOf(line), origin);
    return true;
}

// The index RT_CustomStyleFromString resolves a `style:` value to: a name, or a bare number
// clamped into the table.
int32_t StyleFromValue(std::string_view value)
{
    const std::string text = Lowercase(value);

    for (int32_t i = 0; i < kEditorStyleCount; i++)
    {
        if (Lowercase(kEditorStyleNames[i]) == text)
        {
            return i;
        }
    }

    const int32_t number = std::atoi(std::string(value).c_str());
    return number < 0 ? 0 : (number >= kEditorStyleCount ? kEditorStyleCount - 1 : number);
}

std::string TrimmedLine(std::string_view line)
{
    size_t end = line.size();

    while (end > 0 && (line[end - 1] == '\r' || line[end - 1] == '\n'))
    {
        end--;
    }

    return std::string(line.substr(0, end));
}

// One `key:` block of a level section: `fog:`, `lights:`, or the game's own `sky:`.
struct SubBlock
{
    std::string key;
    size_t head = 0;
    size_t first = 0;
    size_t last = 0;
};

// One `- origin:` entry of a `lights:` block, carrying the keys the editor does not model so a
// rewrite keeps them where they were.
struct LightEntryBlock
{
    EditorLevelLight light;
    std::vector<std::string> extra;
    size_t first = 0;
    size_t last = 0;
};

// One past the last line of the section whose head is `head`.
size_t SectionEnd(const std::vector<TextLine> &lines, const std::string &text, size_t head)
{
    size_t end = head + 1;

    while (end < lines.size() && IsBodyLine(lines[end].View(text)))
    {
        end++;
    }

    return end;
}

// A section's children sit at its shallowest key, two spaces in every file the game writes; taking
// the minimum keeps a hand-written section readable rather than assuming the depth.
size_t SectionChildIndent(const std::vector<TextLine> &lines, const std::string &text, size_t first,
                          size_t last)
{
    size_t indent = 0;

    for (size_t i = first; i < last; i++)
    {
        const std::string_view line = lines[i].View(text);

        if (KeyOf(line).empty())
        {
            continue;
        }

        const size_t current = LeadingWhitespace(line);

        if (indent == 0 || current < indent)
        {
            indent = current;
        }
    }

    return indent == 0 ? 2 : indent;
}

std::vector<SubBlock> SectionBlocks(const std::vector<TextLine> &lines, const std::string &text,
                                    size_t first, size_t last)
{
    const size_t indent = SectionChildIndent(lines, text, first, last);
    std::vector<SubBlock> blocks;
    size_t i = first;

    while (i < last)
    {
        const std::string_view line = lines[i].View(text);
        const std::string key = Lowercase(KeyOf(line));

        if (key.empty() || LeadingWhitespace(line) != indent)
        {
            i++;
            continue;
        }

        SubBlock block;
        block.key = key;
        block.head = i;
        block.first = i + 1;

        size_t end = block.first;

        while (end < last)
        {
            const std::string_view body = lines[end].View(text);

            if (!IsBodyLine(body))
            {
                break;
            }

            if (LeadingWhitespace(body) <= indent && !KeyOf(body).empty())
            {
                break;
            }

            end++;
        }

        block.last = end;
        blocks.push_back(block);
        i = end;
    }

    return blocks;
}

void ApplyExtraLightEntryKey(std::string_view key, std::string_view value, std::string_view line,
                             EditorLevelLight &light, std::vector<std::string> &extra)
{
    if (key == "class" || key == "classname")
    {
        light.className = TextOf(value);
    }
    else if (key == "offset")
    {
        light.hasOffset = true;
        ParseVector(value, light.offset);
    }
    else if (key == "light_shafts")
    {
        light.lightShafts = BoolOf(value, light.lightShafts);
    }
    else if (key == "light_shafts_power")
    {
        light.lightShaftsPower = std::clamp(FloatOf(value, light.lightShaftsPower), 0.0f, 16.0f);
    }
    else if (key == "light_shafts_distance")
    {
        light.lightShaftsDistance =
            std::clamp(FloatOf(value, light.lightShaftsDistance), 0.0f, 25.0f);
    }
    else if (key == "light_shafts_falloff")
    {
        light.lightShaftsFalloff = std::clamp(FloatOf(value, light.lightShaftsFalloff), 0.0f, 4.0f);
    }
    else if (key == "force_rasterize")
    {
        light.forceRasterize = BoolOf(value, light.forceRasterize);
    }
    else if (key == "enabled")
    {
        light.enabled = BoolOf(value, light.enabled);
    }
    else
    {
        extra.push_back(TrimmedLine(line));
    }
}

std::vector<LightEntryBlock> ParseLightEntries(const std::vector<TextLine> &lines,
                                               const std::string &text, const SubBlock &block)
{
    std::vector<LightEntryBlock> entries;
    size_t i = block.first;

    while (i < block.last)
    {
        float origin[3] = {};

        if (!LightOriginOf(lines[i].View(text), origin))
        {
            i++;
            continue;
        }

        LightEntryBlock entry;
        entry.first = i;

        for (int axis = 0; axis < 3; axis++)
        {
            entry.light.origin[axis] = origin[axis];
        }

        const size_t indent = LeadingWhitespace(lines[i].View(text));
        i++;

        while (i < block.last)
        {
            const std::string_view line = lines[i].View(text);

            if (!IsBodyLine(line) || LeadingWhitespace(line) <= indent)
            {
                break;
            }

            const std::string key = Lowercase(KeyOf(line));
            const std::string_view value = ValueOf(line);
            EditorLevelLight &light = entry.light;

            if (key == "origin")
            {
                ParseVector(value, light.origin);
            }
            else if (key == "radius")
            {
                light.radius = FloatOf(value, light.radius);
            }
            else if (key == "intensity")
            {
                light.intensity = FloatOf(value, light.intensity);
            }
            else if (key == "color")
            {
                ParseColor(value, light.color);
            }
            else if (key == "spot")
            {
                light.spot = BoolOf(value, light.spot);
            }
            else if (key == "dir")
            {
                ParseVector(value, light.direction);
            }
            else if (key == "angle_inner")
            {
                light.angleInner = FloatOf(value, light.angleInner);
            }
            else if (key == "angle_outer")
            {
                light.angleOuter = FloatOf(value, light.angleOuter);
            }
            else if (key == "style")
            {
                light.style = StyleFromValue(value);
            }
            else
            {
                ApplyExtraLightEntryKey(key, value, line, light, entry.extra);
            }

            i++;
        }

        entry.last = i;
        entries.push_back(entry);
    }

    return entries;
}

} // namespace

// The `sky:` block's keys in the order RT_SkySettings writes them. Exported because the panel
// maps the same list onto its own settings.
const char *const kEditorSkyKeyNames[kEditorSkyKeyCount] = {
    "rt_sky",
    "rt_sky_brightness",
    "rt_sky_light_mult",
    "rt_sky_color",
    "rt_sky_ambient_lod",
    "rt_sky_nee",
    "rt_physical_sky",
    "rt_sky_clouds",
    "rt_sky_clouds_color",
    "rt_sky_clouds_alpha",
    "rt_sky_clouds_coverage",
    "rt_sky_clouds_density",
    "rt_sky_clouds_speed",
    "rt_sky_clouds_quality",
    "rt_sky_clouds_height",
    "rt_sky_clouds_thickness",
    "rt_physical_sun",
    "rt_sky_sun",
    "rt_sky_sun_color",
    "rt_sky_sun_size",
    "rt_sky_sun_pitch",
    "rt_sky_sun_yaw",
    "rt_sky_godrays",
    "rt_sky_godrays_intensity",
    "rt_sky_godrays_sky_threshold",
    "rt_sky_time_enable",
    "rt_sky_time",
    "rt_sky_time_speed",
    "rt_sky_latitude",
    "rt_sky_azimuth",
    "rt_sky_atmosphere_density",
    "rt_sky_mie",
    "rt_sky_ozone",
    "rt_sky_stars_enable",
    "rt_sky_stars_density",
    "rt_sky_stars",
    "rt_sky_moon",
    "rt_sky_moon_color",
    "rt_sky_moon_size",
    "rt_sky_moon_phase",
};

// One value as the block spells it: the console's own text, so the game's cvar parser and the
// editor's reader agree on it. Colours are `r g b` in 0..255, which is how rt_sky_color and
// rt_sky_sun_color are written; the light blocks' `rrggbb` form is a different reader's.
std::string FormatSkyText(float value)
{
    return FormatFloat(value);
}

std::string FormatSkyText(int32_t value)
{
    return std::to_string(value);
}

std::string FormatSkyText(bool value)
{
    return value ? "1" : "0";
}

std::string FormatSkyText(const float color[3])
{
    char buffer[48];
    int channels[3];

    for (int i = 0; i < 3; i++)
    {
        const float clamped = std::min(1.0f, std::max(0.0f, color[i]));
        channels[i] = int(clamped * 255.0f + 0.5f);
    }

    std::snprintf(buffer, sizeof(buffer), "%d %d %d", channels[0], channels[1], channels[2]);
    return buffer;
}

int32_t EditorSkyKeyIndex(const std::string &name)
{
    const std::string key = Lowercase(name);

    for (int32_t i = 0; i < kEditorSkyKeyCount; i++)
    {
        if (key == kEditorSkyKeyNames[i])
        {
            return i;
        }
    }

    return -1;
}

bool EditorLevelSky::Empty() const
{
    // The unknown lines do not count: the block's presence is the level's scope, and a set with no
    // key overrides nothing, so the writer drops the whole block and the lines in it with it.
    for (int32_t i = 0; i < kEditorSkyKeyCount; i++)
    {
        // An empty value writes no key, so a slot that holds one is as clear as an unset one.
        if (set[i] && !values[i].empty())
        {
            return false;
        }
    }

    return true;
}

// The `sky:` block's keys are cvar names and its values are the text the console takes, so a value
// is kept as the file spelled it: converting it here and back would rewrite a level the game wrote
// with a different but equal spelling of the same number. A line with no slot is kept as it was,
// which is how a cvar a newer engine knows survives a session that cannot show it.
void ParseSkyFields(const std::vector<TextLine> &lines, const std::string &text,
                    const SubBlock &block, EditorLevelSky &sky)
{
    for (size_t i = block.first; i < block.last; i++)
    {
        const std::string_view line = lines[i].View(text);
        const std::string_view key = KeyOf(line);
        const int32_t index = key.empty() ? -1 : EditorSkyKeyIndex(std::string(key));

        if (index < 0)
        {
            const std::string kept = TrimmedLine(line);

            // Blank lines carry nothing; skipping them keeps a re-indented block from growing.
            for (const char c : kept)
            {
                if (!IsSpace(c))
                {
                    sky.extra.push_back(kept);
                    break;
                }
            }

            continue;
        }

        const std::string_view value = ValueOf(line);

        if (value.empty())
        {
            continue;
        }

        sky.values[index] = std::string(value);
        sky.set[index] = true;
    }
}

namespace {

void ParseFogFields(const std::vector<TextLine> &lines, const std::string &text,
                    const SubBlock &block, EditorLevelFog &fog, bool &hasEnabled)
{
    for (size_t i = block.first; i < block.last; i++)
    {
        const std::string_view line = lines[i].View(text);
        const std::string key = Lowercase(KeyOf(line));
        const std::string_view value = ValueOf(line);

        if (key == "color")
        {
            ParseColor(value, fog.color);
        }
        else if (key == "density")
        {
            fog.density = std::max(0.0f, FloatOf(value, fog.density));
        }
        else if (key == "enabled")
        {
            fog.enabled = BoolOf(value, fog.enabled);
            hasEnabled = true;
        }
    }
}

std::string RenderFogBlock(const EditorLevelFog &fog, size_t childIndent)
{
    const std::string field(childIndent + 2, ' ');
    std::string text(childIndent, ' ');

    text += "fog:\n";
    text += field + "color: " + FormatColor(fog.color) + "\n";
    text += field + "density: " + FormatFloat(fog.density) + "\n";
    text += field + "enabled: " + FormatBool(fog.enabled) + "\n";
    return text;
}

// The block as RT_SkySettings_WriteLevel lays it out: only the keys the level overrides, in the
// module's own order, because the game's reader takes the block's presence as the level's scope
// and a level without overrides has to keep following the config. Lines the panel has no slot for
// follow the managed keys, where the game's writer would drop them.
std::string RenderSkyBlock(const EditorLevelSky &sky, size_t childIndent)
{
    const std::string head(childIndent, ' ');
    const std::string field(childIndent + 2, ' ');
    std::string text;

    for (int32_t i = 0; i < kEditorSkyKeyCount; i++)
    {
        if (!sky.set[i] || sky.values[i].empty())
        {
            continue;
        }

        if (text.empty())
        {
            text += head + "sky:\n";
        }

        text += field + kEditorSkyKeyNames[i] + ": " + sky.values[i] + "\n";
    }

    for (const std::string &line : sky.extra)
    {
        if (text.empty())
        {
            text += head + "sky:\n";
        }

        // The line keeps the indentation it was read with, so its own nesting survives: a key the
        // panel cannot show may still carry children under it.
        text += line + "\n";
    }

    return text;
}

void AppendLightEntryShafts(std::string &text, const std::string &field, const EditorLevelLight &light)
{
    if (light.lightShafts)
    {
        text += field + "light_shafts: true\n";
    }
    if (light.lightShaftsPower != 1.0f)
    {
        text += field + "light_shafts_power: " + FormatFloat(light.lightShaftsPower) + "\n";
    }
    if (light.lightShaftsDistance > 0.0f)
    {
        text += field + "light_shafts_distance: " + FormatFloat(light.lightShaftsDistance) + "\n";
    }
    if (light.lightShaftsFalloff != 1.0f)
    {
        text += field + "light_shafts_falloff: " + FormatFloat(light.lightShaftsFalloff) + "\n";
    }
}

// The entry as RT_CustomLights_WriteEntry lays it out. `extra` carries the keys the editor cannot
// express, which is how an entry's `offset:` survives a session.
std::string RenderLightEntry(const EditorLevelLight &light, const std::vector<std::string> &extra,
                             size_t childIndent)
{
    const std::string head(childIndent + 2, ' ');
    const std::string field(childIndent + 4, ' ');
    std::string text;

    text += head + "- origin: " + FormatVector(light.origin) + "\n";

    if (!light.className.empty())
    {
        text += field + "class: " + light.className + "\n";
    }

    if (light.hasOffset)
    {
        text += field + "offset: " + FormatVector(light.offset) + "\n";
    }

    for (const std::string &line : extra)
    {
        text += line + "\n";
    }

    text += field + "radius: " + FormatFloat(light.radius) + "\n";
    text += field + "intensity: " + FormatFloat(light.intensity) + "\n";
    text += field + "color: " + FormatColor(light.color) + "\n";

    if (light.spot)
    {
        text += field + "spot: true\n";
        text += field + "dir: " + FormatVector(light.direction) + "\n";
        text += field + "angle_inner: " + FormatFloat(light.angleInner) + "\n";
        text += field + "angle_outer: " + FormatFloat(light.angleOuter) + "\n";
        AppendLightEntryShafts(text, field, light);
    }

    if (light.forceRasterize)
    {
        text += field + "force_rasterize: true\n";
    }

    if (!light.enabled)
    {
        text += field + "enabled: false\n";
    }

    if (light.style > 0)
    {
        const int32_t style =
            light.style < kEditorStyleCount ? light.style : kEditorStyleCount - 1;
        text += field + "style: " + std::string(kEditorStyleNames[style]) + "\n";
    }

    return text;
}

void ApplyEmitterConeField(std::string_view key, std::string_view value,
                           EditorEmitterLightOverride &out)
{
    if (key == "spot")
    {
        out.spot = BoolOf(value, out.spot);
        out.hasSpot = true;
    }
    else if (key == "dir")
    {
        ParseVector(value, out.dir);
        out.hasDir = true;
    }
    else if (key == "angle_inner")
    {
        out.angleInner = FloatOf(value, out.angleInner);
        out.hasAngles = true;
    }
    else if (key == "angle_outer")
    {
        out.angleOuter = FloatOf(value, out.angleOuter);
        out.hasAngles = true;
    }
    else if (key == "force_rasterize")
    {
        out.forceRasterize = BoolOf(value, out.forceRasterize);
    }
    else if (key == "enabled")
    {
        out.enabled = BoolOf(value, out.enabled);
    }
    else if (key == "group_edit")
    {
        out.groupEdit = BoolOf(value, out.groupEdit);
    }
}

void ApplyEmitterLightField(std::string_view key, std::string_view value,
                            EditorEmitterLightOverride &out)
{
    if (key == "light_radius")
    {
        out.radius = FloatOf(value, out.radius);
        out.hasRadius = true;
    }
    else if (key == "light_intensity")
    {
        out.intensity = FloatOf(value, out.intensity);
        out.hasIntensity = true;
    }
    else if (key == "light_offset")
    {
        ParseVector(value, out.offset);
        out.hasOffset = true;
    }
    else if (key == "light_color")
    {
        ParseColor(value, out.color);
        out.hasColor = true;
    }
    else if (key == "light_style")
    {
        const std::string lower = Lowercase(value);
        if (lower == "none" || lower == "off" || lower == "default")
        {
            out.style = 0;
            out.hasStyle = false;
        }
        else
        {
            out.style = StyleFromValue(value);
            out.hasStyle = true;
        }
    }
    else
    {
        ApplyEmitterConeField(key, value, out);
    }
}

bool HasEmitterOverrideFields(const EditorEmitterLightOverride &ov)
{
    return ov.hasRadius || ov.hasIntensity || ov.hasOffset || ov.hasColor || ov.hasSpot ||
           ov.hasDir || ov.hasAngles || ov.forceRasterize || ov.hasStyle || !ov.groupEdit ||
           !ov.enabled;
}

std::string RenderEmitterLightEntry(const EditorEmitterLightOverride &ov)
{
    std::string text;
    text += "  - name: " + ov.name + "\n";

    if (ov.hasRadius)
    {
        text += "    light_radius: " + FormatFloat(ov.radius) + "\n";
    }
    if (ov.hasIntensity)
    {
        text += "    light_intensity: " + FormatFloat(ov.intensity) + "\n";
    }
    if (ov.hasOffset)
    {
        text += "    light_offset: " + FormatVector(ov.offset) + "\n";
    }
    if (ov.hasColor)
    {
        text += "    light_color: " + FormatColor(ov.color) + "\n";
    }
    if (ov.hasSpot)
    {
        text += "    spot: " + FormatBool(ov.spot) + "\n";
    }
    if (ov.hasDir)
    {
        text += "    dir: " + FormatVector(ov.dir) + "\n";
    }
    if (ov.hasAngles)
    {
        text += "    angle_inner: " + FormatFloat(ov.angleInner) + "\n";
        text += "    angle_outer: " + FormatFloat(ov.angleOuter) + "\n";
    }
    if (ov.forceRasterize)
    {
        text += "    force_rasterize: true\n";
    }
    if (ov.hasStyle)
    {
        const int32_t style = std::clamp(ov.style, 0, kEditorStyleCount - 1);
        text += "    light_style: " + std::string(kEditorStyleNames[style]) + "\n";
    }

    if (!ov.enabled)
    {
        text += "    enabled: false\n";
    }

    text += "    group_edit: " + FormatBool(ov.groupEdit) + "\n";
    return text;
}

bool IsTopLevelEmitterHeader(std::string_view line)
{
    if (LeadingWhitespace(line) != 0)
    {
        return false;
    }

    const std::string key = Lowercase(KeyOf(line));
    return key == "qray_lights" || key == "lights";
}

bool EndsEmitterSectionLine(std::string_view line)
{
    if (LeadingWhitespace(line) != 0 || line.empty() || line[0] == '#' || line[0] == '\r' ||
        line[0] == '\n')
    {
        return false;
    }

    std::string dummyName;
    if (NameOf(line, dummyName) || IsTopLevelEmitterHeader(line))
    {
        return false;
    }

    return !KeyOf(line).empty();
}

std::string MergeEmitterLightsText(const std::string &doc,
                                   const std::vector<EditorEmitterLightOverride> &emitters)
{
    const std::vector<TextLine> lines = SplitLines(doc);
    size_t head = size_t(-1);

    for (size_t i = 0; i < lines.size(); i++)
    {
        if (IsTopLevelEmitterHeader(lines[i].View(doc)))
        {
            head = i;
            break;
        }
    }

    const std::string block = RenderEmitterLightsBlock(emitters);
    std::string out;

    if (head != size_t(-1))
    {
        size_t end = head + 1;
        while (end < lines.size() && !EndsEmitterSectionLine(lines[end].View(doc)))
        {
            end++;
        }

        for (size_t i = 0; i < head; i++)
        {
            out += lines[i].View(doc);
        }
        out += block;
        for (size_t i = end; i < lines.size(); i++)
        {
            out += lines[i].View(doc);
        }
        return out;
    }

    if (block.empty())
    {
        return doc;
    }

    size_t insertAt = lines.size();
    for (size_t i = 0; i < lines.size(); i++)
    {
        if (EndsEmitterSectionLine(lines[i].View(doc)))
        {
            insertAt = i;
            break;
        }
    }

    for (size_t i = 0; i < insertAt; i++)
    {
        out += lines[i].View(doc);
    }
    out += block;
    for (size_t i = insertAt; i < lines.size(); i++)
    {
        out += lines[i].View(doc);
    }
    return out;
}

bool IsEmittedMaterialKey(std::string_view key,
                          const std::vector<std::pair<const char *, std::string>> &values)
{
    // Any key managed by ReadMaterial / OwnedValues must be replaced from the panel's current
    // state on save so clearing or resetting a field back to its default deletes the old YAML line.
    static constexpr const char *kOwnedMaterialKeys[] = {
        "enabled",
        "texture_base",
        "texture_normals",
        "texture_emissive",
        "texture_overlay",
        "texture_gloss",
        "texture_scale_u",
        "texture_scale_v",
        "texture_offset_u",
        "texture_offset_v",
        "bump_scale",
        "roughness_override",
        "metalness_factor",
        "metalness_from_normal_alpha",
        "base_factor",
        "emissive_factor",
        "luma_blend",
        "emissive_blend",
        "transparency",
        "is_light",
        "exact_normals",
        "force_rasterize",
        "light_styles",
        "light_color",
        "light_brightness",
        "light_focus",
        "light_focus_soft",
        "light_projector",
        "light_min_area",
        "disabled_stages",
        "light_shafts",
        "light_shafts_power",
        "light_shafts_distance",
        "light_shafts_falloff",
        "mirror",
        "material_glass",
        "material_water",
        "glass_ior",
        "glass_thickness",
        "glass_color",
        "luma",
        "color_emissive",
        "luma_threshold",
        "color_emissive_threshold",
        "luma_feather",
        "color_emissive_feather",
        "gloss",
        "color_gloss",
        "gloss_threshold",
        "gloss_feather",
        "texture_sss",
        "sss_strength",
        "sss_color",
        "sss_radius",
        "sss",
        "sss_threshold",
        "sss_feather",
    };

    for (const char *owned : kOwnedMaterialKeys)
    {
        if (key == owned)
        {
            return true;
        }
    }

    for (const auto &[emittedKey, emittedValue] : values)
    {
        if (key == emittedKey)
        {
            return true;
        }
    }

    return false;
}

FILE *OpenFilePortable(const std::string &path, const char *mode)
{
#if defined(_WIN32)
    FILE *file = nullptr;

    if (fopen_s(&file, path.c_str(), mode) != 0)
    {
        return nullptr;
    }

    return file;
#else
    return std::fopen(path.c_str(), mode);
#endif
}

bool ReplaceFilePortable(const std::string &temporary, const std::string &path)
{
#if defined(_WIN32)
    return MoveFileExA(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return std::rename(temporary.c_str(), path.c_str()) == 0;
#endif
}

bool ReadFileText(const std::string &path, std::string &text)
{
    FILE *file = OpenFilePortable(path, "rb");

    if (file == nullptr)
    {
        return false;
    }

    char buffer[8192];
    size_t read = 0;

    while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        text.append(buffer, read);
    }

    fclose(file);
    return true;
}

} // namespace

EditorSessionPaths BuildEditorSessionPaths(const std::string &materialsOverride,
                                           const std::string &lightsOverride,
                                           const std::string &scenePath)
{
    EditorSessionPaths paths;
    const std::string sceneDirectory = EditorDirectoryOf(scenePath);

    if (!materialsOverride.empty())
    {
        paths.materials = materialsOverride;
    }
    else
    {
        paths.materials = sceneDirectory.empty() ? "qray.materials.yaml"
                                                 : sceneDirectory + "/qray.materials.yaml";
    }

    if (!lightsOverride.empty())
    {
        paths.lights = lightsOverride;
    }
    else
    {
        paths.lights = sceneDirectory.empty() ? "qray.lights.yaml"
                                              : sceneDirectory + "/qray.lights.yaml";
    }

    const std::string materialsDirectory = EditorDirectoryOf(paths.materials);
    paths.materialsBackup = (materialsDirectory.empty() ? std::string(".") : materialsDirectory) +
                            "/qray.backup_materials.yaml";

    const std::string lightsDirectory = EditorDirectoryOf(paths.lights);
    paths.lightsBackup = (lightsDirectory.empty() ? std::string(".") : lightsDirectory) +
                         "/qray.backup_lights.yaml";

    paths.level = LevelKeyOf(scenePath);
    return paths;
}

bool EditorMaterialsDocument::Load(const std::string &path, std::string &error)
{
    m_text.clear();
    m_exists = false;

    FILE *probe = OpenFilePortable(path, "rb");

    if (probe == nullptr)
    {
        return true; // a target that does not exist yet: the writer emits the header
    }

    fclose(probe);

    if (!ReadFileText(path, m_text))
    {
        error = "cannot read " + path;
        return false;
    }

    m_exists = true;
    return true;
}

bool EditorMaterialsDocument::LoadFromText(std::string text)
{
    m_text = std::move(text);
    m_exists = true;
    return true;
}

std::vector<std::string> EditorMaterialsDocument::MaterialNames() const
{
    std::vector<std::string> names;
    if (m_text.empty())
    {
        return names;
    }

    const std::vector<TextLine> lines = SplitLines(m_text);
    for (size_t i = 0; i < lines.size(); i++)
    {
        std::string found;
        if (NameOf(lines[i].View(m_text), found))
        {
            names.push_back(std::move(found));
        }
    }
    return names;
}

size_t EditorMaterialsDocument::FindEntry(const std::string &name) const
{
    const std::vector<TextLine> lines = SplitLines(m_text);

    for (size_t i = 0; i < lines.size(); i++)
    {
        std::string found;

        if (NameOf(lines[i].View(m_text), found) && MaterialNameMatches(name, found))
        {
            return i;
        }
    }

    return size_t(-1);
}

bool EditorMaterialsDocument::HasMaterial(const std::string &name) const
{
    return FindEntry(name) != size_t(-1);
}

bool EditorMaterialsDocument::HasMaterialWithPrefix(const std::string &prefix) const
{
    const std::string lowerPrefix = Lowercase(prefix);
    if (lowerPrefix.empty() || m_text.empty())
    {
        return false;
    }

    const std::vector<TextLine> lines = SplitLines(m_text);
    for (size_t i = 0; i < lines.size(); i++)
    {
        std::string found;
        if (NameOf(lines[i].View(m_text), found) &&
            found.compare(0, lowerPrefix.size(), lowerPrefix) == 0)
        {
            return true;
        }
    }
    return false;
}

namespace
{

size_t FindMaterialEntryEnd(const std::vector<TextLine> &lines, const std::string &text, size_t head)
{
    size_t end = head + 1;

    for (; end < lines.size(); end++)
    {
        const std::string_view line = lines[end].View(text);
        std::string nextName;

        // The entry ends at the next sequence item of its own name, whatever indentation the file
        // uses, so a neighbour's keys never count as this entry's.
        if (!IsBodyLine(line) || NameOf(line, nextName))
        {
            break;
        }
    }

    return end;
}

size_t InferMaterialKeyIndent(const std::vector<TextLine> &lines, const std::string &text,
                              size_t begin, size_t end)
{
    size_t keyIndent = size_t(-1);

    for (size_t i = begin; i < end; i++)
    {
        const std::string_view line = lines[i].View(text);

        if (KeyOf(line).empty())
        {
            continue;
        }

        keyIndent = std::min(keyIndent, LeadingWhitespace(line));
    }

    return keyIndent;
}

} // namespace

bool EditorMaterialsDocument::ReadMaterial(const std::string &name, EditorMaterialParams &params) const
{
    const size_t head = FindEntry(name);

    if (head == size_t(-1))
    {
        return false;
    }

    const std::vector<TextLine> lines = SplitLines(m_text);
    bool glass = false;
    bool water = false;
    bool hasSssStrength = false;

    ResetMaterialBlockState(params);

    const size_t end = FindMaterialEntryEnd(lines, m_text, head);
    const size_t keyIndent = InferMaterialKeyIndent(lines, m_text, head + 1, end);

    for (size_t i = head + 1; i < end; i++)
    {
        const std::string_view line = lines[i].View(m_text);

        if (LeadingWhitespace(line) != keyIndent)
        {
            continue;
        }

        const std::string_view key = KeyOf(line);

        if (key.empty())
        {
            continue;
        }

        const std::string_view value = ValueOf(line);

        if (key == "texture_base")
        {
            params.baseTexture = TextOf(value);
        }
        else if (key == "texture_normals")
        {
            params.normalTexture = TextOf(value);
        }
        else if (key == "texture_emissive")
        {
            params.emissiveTexture = TextOf(value);
        }
        else if (key == "texture_gloss")
        {
            params.glossTexture = TextOf(value);
        }
        else if (key == "texture_overlay")
        {
            params.overlayTexture = TextOf(value);
        }
        else if (key == "texture_scale_u")
        {
            params.textureScaleU = FloatOf(value, params.textureScaleU);
        }
        else if (key == "texture_scale_v")
        {
            params.textureScaleV = FloatOf(value, params.textureScaleV);
        }
        else if (key == "texture_offset_u")
        {
            params.textureOffsetU = FloatOf(value, params.textureOffsetU);
        }
        else if (key == "texture_offset_v")
        {
            params.textureOffsetV = FloatOf(value, params.textureOffsetV);
        }
        else if (key == "roughness_override")
        {
            params.roughnessOverride = FloatOf(value, params.roughnessOverride);
        }
        else if (key == "bump_scale")
        {
            params.bumpScale = FloatOf(value, params.bumpScale);
        }
        else if (key == "metalness_factor")
        {
            params.metalnessFactor = FloatOf(value, params.metalnessFactor);
        }
        else if (key == "metalness_from_normal_alpha")
        {
            params.metalnessFromNormalAlpha =
                BoolOf(value, params.metalnessFromNormalAlpha);
        }
        else if (key == "base_factor")
        {
            params.baseFactor = FloatOf(value, params.baseFactor);
        }
        else if (key == "emissive_factor")
        {
            params.emissiveFactor = FloatOf(value, params.emissiveFactor);
        }
        else if (key == "luma_blend")
        {
            params.lumaBlend = EmissiveBlendOf(value, params.lumaBlend);
        }
        else if (key == "transparency")
        {
            params.transparency = FloatOf(value, params.transparency);
        }
        else if (key == "is_light")
        {
            params.luma = BoolOf(value, params.luma);
        }
        else if (key == "exact_normals")
        {
            params.exactNormals = BoolOf(value, params.exactNormals);
        }
        else if (key == "force_rasterize")
        {
            params.forceRasterize = BoolOf(value, params.forceRasterize);
        }
        else if (key == "light_styles")
        {
            params.lightStyles = BoolOf(value, params.lightStyles);
        }
        else if (key == "light_color")
        {
            params.hasLightColor = true;
            ParseColor(value, params.lightColor);
        }
        else if (key == "light_brightness")
        {
            params.lightBrightness = FloatOf(value, params.lightBrightness);
        }
        else if (key == "light_focus")
        {
            params.lightFocus = FloatOf(value, params.lightFocus);
        }
        else if (key == "light_focus_soft")
        {
            params.lightFocusSoft = FloatOf(value, params.lightFocusSoft);
        }
        else if (key == "light_projector")
        {
            params.lightProjector = BoolOf(value, params.lightProjector);
        }
        else if (key == "mirror")
        {
            params.mirror = BoolOf(value, params.mirror);
        }
        else if (key == "glass_ior")
        {
            params.glassIor = FloatOf(value, params.glassIor);
        }
        else if (key == "glass_thickness")
        {
            params.glassThickness = FloatOf(value, params.glassThickness);
        }
        else if (key == "glass_color")
        {
            ParseColor(value, params.glassColor);
        }
        else if (key == "material_glass")
        {
            glass = BoolOf(value, glass);
        }
        else if (key == "material_water")
        {
            water = BoolOf(value, water);
        }
        else if (key == "luma_threshold")
        {
            params.lumaThreshold = FloatOf(value, params.lumaThreshold);
        }
        else if (key == "luma_feather")
        {
            params.lumaFeather = FloatOf(value, params.lumaFeather);
        }
        else
        {
            ApplyExtraMaterialScalarKey(key, value, params, hasSssStrength);
        }
    }

    // The blocks inherit the material-level tone controls, so they are read once those are known.
    ReadEmissiveBlocks(m_text, lines, head + 1, end, keyIndent, params);
    ReadGlossBlocks(m_text, lines, head + 1, end, keyIndent, params);
    ReadSssBlocks(m_text, lines, head + 1, end, keyIndent, params);
    FinalizeExtraMaterialRead(params, hasSssStrength);

    // a material that asks for both is glass, as the loader decides
    params.type = glass ? EditorMaterialType::Glass
                        : (water ? EditorMaterialType::Water : EditorMaterialType::None);
    return true;
}

namespace
{

std::vector<std::pair<const char *, std::string>>
OwnedEntryValues(const EditorMaterialEntry &entry)
{
    std::vector<std::pair<const char *, std::string>> values = OwnedValues(entry.params);
    if (entry.clearDefaultEmissive && entry.params.emissiveTexture.empty())
    {
        values.emplace_back("texture_emissive", "NONE");
    }
    if (entry.clearDefaultMirror && !entry.params.mirror)
    {
        values.emplace_back("mirror", "false");
    }
    return values;
}

size_t FindTouchedMaterialEntry(const std::vector<EditorMaterialEntry> &entries,
                                const std::vector<bool> &written, std::string_view line)
{
    std::string found;
    if (!NameOf(line, found))
    {
        return size_t(-1);
    }

    for (size_t e = 0; e < entries.size(); e++)
    {
        // The same resolution a read uses, so an entry the file already carries under the
        // game's key form is updated in place rather than appended a second time under
        // the scene's bare miptex name.
        if (!written[e] && MaterialNameMatches(entries[e].name, found))
        {
            return e;
        }
    }

    return size_t(-1);
}

void SkipNestedMaterialBlock(const std::string &text, const std::vector<TextLine> &lines,
                             size_t body, size_t keyIndent, size_t &k)
{
    for (k++; k < body; k++)
    {
        const std::string_view nested = lines[k].View(text);
        if (!IsBlankLine(nested) && LeadingWhitespace(nested) <= keyIndent)
        {
            break;
        }
    }

    if (k < body)
    {
        k--;
    }
}

size_t AppendMergedExistingMaterialEntry(const std::string &text,
                                         const std::vector<TextLine> &lines, size_t head,
                                         const EditorMaterialEntry &entry, std::string &merged)
{
    const std::string_view line = lines[head].View(text);
    merged += line;
    if (!line.empty() && line.back() != '\n')
    {
        merged += '\n';
    }

    const size_t body = FindMaterialEntryEnd(lines, text, head);
    const size_t keyIndent = InferMaterialKeyIndent(lines, text, head + 1, body);
    const std::string indent(keyIndent != size_t(-1) ? keyIndent : LeadingWhitespace(line) + 2,
                             ' ');
    const std::vector<std::pair<const char *, std::string>> values = OwnedEntryValues(entry);

    for (const auto &[key, value] : values)
    {
        AppendOwnedValue(merged, indent, key, value, entry.params);
    }

    for (size_t k = head + 1; k < body; k++)
    {
        const std::string_view bodyLine = lines[k].View(text);
        const std::string_view key = KeyOf(bodyLine);
        const bool emitted = !key.empty() && LeadingWhitespace(bodyLine) == keyIndent &&
                             IsEmittedMaterialKey(key, values);
        if (emitted)
        {
            SkipNestedMaterialBlock(text, lines, body, keyIndent, k);
            continue;
        }

        merged += bodyLine;
    }

    return body - 1;
}

void AppendNewMaterialEntry(const EditorMaterialEntry &entry, std::string &merged)
{
    // A file whose last line had no terminator would otherwise swallow the new entry.
    if (!merged.empty() && merged.back() != '\n')
    {
        merged += '\n';
    }

    merged += "  - name: " + MaterialKeyName(entry.name) + "\n";

    for (const auto &[key, value] : OwnedEntryValues(entry))
    {
        AppendOwnedValue(merged, std::string(4, ' '), key, value, entry.params);
    }
}

} // namespace

std::string EditorMaterialsDocument::WriteMerged(const std::vector<EditorMaterialEntry> &entries) const
{
    std::string merged;
    std::vector<bool> written(entries.size(), false);

    if (!m_exists || m_text.empty())
    {
        merged += kMaterialsHeader;
        merged += "qray_materials:\n";
    }

    const std::vector<TextLine> lines = SplitLines(m_text);

    for (size_t i = 0; i < lines.size(); i++)
    {
        const std::string_view line = lines[i].View(m_text);
        const size_t entry = FindTouchedMaterialEntry(entries, written, line);

        if (entry == size_t(-1))
        {
            merged += MigratedRootLine(line);
            continue;
        }

        i = AppendMergedExistingMaterialEntry(m_text, lines, i, entries[entry], merged);
        written[entry] = true;
    }

    for (size_t e = 0; e < entries.size(); e++)
    {
        if (!written[e])
        {
            AppendNewMaterialEntry(entries[e], merged);
        }
    }

    return merged;
}

const char *const kEditorStyleNames[kEditorStyleCount] = {
    "steady",        "flicker",      "slow strong pulse", "candle",       "fast strobe",
    "gentle pulse",  "flicker B",    "candle B",          "candle C",     "slow strobe",
    "fluorescent",   "slow pulse",
};

std::string RenderEditorLevelSection(const std::string &level, const EditorLevelSky &sky,
                                     const EditorLevelFog &fog,
                                     const std::vector<EditorLevelLight> &lights,
                                     bool includeEmptyLights)
{
    std::string text = level + ":\n";

    text += RenderSkyBlock(sky, 2);
    text += RenderFogBlock(fog, 2);

    if (!lights.empty())
    {
        text += "  lights:\n";

        for (const EditorLevelLight &light : lights)
        {
            text += RenderLightEntry(light, {}, 2);
        }
    }
    else if (includeEmptyLights)
    {
        text += "  lights: []\n";
    }

    return text;
}

bool IsSavedStandardLightEntry(const EditorLevelLight &entry)
{
    return !entry.className.empty() && Lowercase(entry.className) != "custom";
}

bool HasSavedStandardLightsBlock(const std::vector<EditorLevelLight> &entries)
{
    return entries.empty() || std::any_of(entries.begin(), entries.end(), [](const EditorLevelLight &entry) {
               return !entry.className.empty();
           });
}

std::string RenderEmitterLightsBlock(const std::vector<EditorEmitterLightOverride> &emitters)
{
    std::string entries;

    for (const EditorEmitterLightOverride &ov : emitters)
    {
        if (ov.name.empty() || !HasEmitterOverrideFields(ov))
        {
            continue;
        }

        entries += RenderEmitterLightEntry(ov);
    }

    if (entries.empty())
    {
        return {};
    }

    return "qray_lights:\n" + entries;
}

std::string MergeEmitterLightsYaml(const std::string &yamlText,
                                   const std::vector<EditorEmitterLightOverride> &emitters)
{
    return MergeEmitterLightsText(yamlText, emitters);
}

bool EditorLevelDocument::Load(const std::string &path, std::string &error)
{
    m_text.clear();
    m_exists = ReadFileText(path, m_text);
    error.clear();

    // The merge appends whole lines, so a file whose last line has no newline would swallow the
    // first line of a section appended after it.
    if (!m_text.empty() && m_text.back() != '\n')
    {
        m_text += '\n';
    }

    return true;
}

bool EditorLevelDocument::LoadFromText(std::string text)
{
    m_text = std::move(text);
    if (!m_text.empty() && m_text.back() != '\n')
    {
        m_text += '\n';
    }
    m_exists = true;
    return true;
}

EditorLevelDocument::ReadResult EditorLevelDocument::ReadLevel(const std::string &level) const
{
    const std::vector<TextLine> lines = SplitLines(m_text);
    const std::string key = Lowercase(level);
    ReadResult result;

    for (size_t head = 0; head < lines.size(); head++)
    {
        const std::string_view line = lines[head].View(m_text);

        if (LeadingWhitespace(line) != 0 || Lowercase(KeyOf(line)) != key)
        {
            continue;
        }

        result.present = true;
        const size_t end = SectionEnd(lines, m_text, head);
        bool hasEnabled = false;

        for (const SubBlock &block : SectionBlocks(lines, m_text, head + 1, end))
        {
            if (block.key == "fog")
            {
                result.hasFog = true;
                ParseFogFields(lines, m_text, block, result.fog, hasEnabled);
            }
            else if (block.key == "sky")
            {
                ParseSkyFields(lines, m_text, block, result.sky);
                // An empty block writes no key, so one the file carries but nothing in it means
                // the level's scope is gone - the same reading RT_SkySettings_ApplyParsed makes.
                result.hasSky = !result.sky.Empty();
            }
            else if (block.key == "lights")
            {
                result.hasLights = true;

                for (const LightEntryBlock &entry : ParseLightEntries(lines, m_text, block))
                {
                    result.lights.push_back(entry.light);
                }
            }
        }

        // A section written before the editor had an `enabled:` key still shows its fog, so the
        // density is what decides; the game's own reader treats the key as optional the same way.
        if (result.hasFog && !hasEnabled)
        {
            result.fog.enabled = result.fog.density > 0.0f;
        }

        return result;
    }

    return result;
}

std::vector<EditorEmitterLightOverride> EditorLevelDocument::ReadEmitterLights() const
{
    const std::vector<TextLine> lines = SplitLines(m_text);
    std::vector<EditorEmitterLightOverride> emitters;
    bool inEmitters = false;
    EditorEmitterLightOverride *current = nullptr;

    for (const TextLine &textLine : lines)
    {
        const std::string_view line = textLine.View(m_text);

        if (IsTopLevelEmitterHeader(line))
        {
            inEmitters = true;
            current = nullptr;
            continue;
        }

        if (EndsEmitterSectionLine(line))
        {
            inEmitters = false;
            current = nullptr;
            continue;
        }

        if (!inEmitters)
        {
            continue;
        }

        std::string itemName;
        if (NameOf(line, itemName))
        {
            emitters.emplace_back();
            emitters.back().name = std::move(itemName);
            current = &emitters.back();
            continue;
        }

        if (current != nullptr)
        {
            const std::string key = Lowercase(KeyOf(line));
            if (!key.empty())
            {
                ApplyEmitterLightField(key, ValueOf(line), *current);
            }
        }
    }

    return emitters;
}

namespace
{

std::vector<std::string> ClaimMatchingOldLightExtra(const EditorLevelLight &light,
                                                    const std::vector<LightEntryBlock> &oldEntries,
                                                    std::vector<bool> &used)
{
    int fallbackIndex = -1;
    for (size_t e = 0; e < oldEntries.size(); e++)
    {
        const EditorLevelLight &old = oldEntries[e].light;
        if (used[e] || old.origin[0] != light.origin[0] || old.origin[1] != light.origin[1] ||
            old.origin[2] != light.origin[2])
        {
            continue;
        }
        if (old.className == light.className)
        {
            used[e] = true;
            return oldEntries[e].extra;
        }
        if (fallbackIndex < 0)
        {
            fallbackIndex = int(e);
        }
    }
    if (fallbackIndex >= 0)
    {
        used[size_t(fallbackIndex)] = true;
        return oldEntries[size_t(fallbackIndex)].extra;
    }
    return {};
}

bool WriteMergedLightsBlock(const std::vector<EditorLevelLight> &lights,
                            const std::vector<LightEntryBlock> &oldEntries, size_t childIndent,
                            bool includeEmptyLights, std::string &merged)
{
    if (!lights.empty())
    {
        merged += std::string(childIndent, ' ') + "lights:\n";
        std::vector<bool> used(oldEntries.size(), false);
        for (const EditorLevelLight &light : lights)
        {
            const std::vector<std::string> extra =
                ClaimMatchingOldLightExtra(light, oldEntries, used);
            merged += RenderLightEntry(light, extra, childIndent);
        }
        return true;
    }
    if (includeEmptyLights)
    {
        merged += std::string(childIndent, ' ') + "lights: []\n";
        return true;
    }
    return false;
}

} // namespace

std::string EditorLevelDocument::WriteMerged(const std::string &level, const EditorLevelSky &sky,
                                             const EditorLevelFog &fog,
                                             const std::vector<EditorLevelLight> &lights,
                                             bool includeEmptyLights) const
{
    const std::vector<TextLine> lines = SplitLines(m_text);
    const std::string key = Lowercase(level);
    std::string merged;
    bool found = false;

    for (size_t head = 0; head < lines.size();)
    {
        const std::string_view line = lines[head].View(m_text);

        if (LeadingWhitespace(line) != 0 || Lowercase(KeyOf(line)) != key)
        {
            merged += line;
            head++;
            continue;
        }

        found = true;
        merged += line;
        const size_t bodyStart = merged.size();

        const size_t end = SectionEnd(lines, m_text, head);
        const size_t childIndent = SectionChildIndent(lines, m_text, head + 1, end);
        const std::vector<SubBlock> blocks = SectionBlocks(lines, m_text, head + 1, end);
        const SubBlock *oldLights = nullptr;
        const SubBlock *oldSky = nullptr;
        bool fogWritten = false;
        bool skyWritten = false;
        bool lightsWritten = false;
        size_t cursor = head + 1;

        for (const SubBlock &block : blocks)
        {
            if (block.key == "lights")
            {
                oldLights = &block;
            }
            else if (block.key == "sky")
            {
                oldSky = &block;
            }
        }

        std::vector<LightEntryBlock> oldEntries;

        if (oldLights != nullptr)
        {
            oldEntries = ParseLightEntries(lines, m_text, *oldLights);
        }

        // The overrides take the panel's numbers, but the block also keeps the lines the panel has
        // no slot for, the way a light entry keeps the keys the editor cannot express.
        EditorLevelSky mergedSky = sky;

        if (oldSky != nullptr)
        {
            EditorLevelSky previous;
            ParseSkyFields(lines, m_text, *oldSky, previous);
            mergedSky.extra = previous.extra;
        }

        while (cursor < end)
        {
            const SubBlock *block = nullptr;

            for (const SubBlock &candidate : blocks)
            {
                if (candidate.head == cursor)
                {
                    block = &candidate;
                    break;
                }
            }

            if (block == nullptr)
            {
                merged += lines[cursor].View(m_text);
                cursor++;
                continue;
            }

            if (block->key == "fog")
            {
                merged += RenderFogBlock(fog, childIndent);
                fogWritten = true;
            }
            else if (block->key == "sky")
            {
                // Same rule as the game's: only the keys the level overrides are written, and a
                // level with none of them keeps no block at all, so it goes on following the
                // config instead of pinning a copy of it.
                if (!mergedSky.Empty())
                {
                    merged += RenderSkyBlock(mergedSky, childIndent);
                    skyWritten = true;
                }
            }
            else if (block->key == "lights")
            {
                lightsWritten = WriteMergedLightsBlock(lights, oldEntries, childIndent,
                                                       includeEmptyLights, merged);
            }
            else
            {
                for (size_t i = block->head; i < block->last; i++)
                {
                    merged += lines[i].View(m_text);
                }
            }

            cursor = block->last;
        }

        if (!fogWritten)
        {
            // A section the game wrote always carries its fog, so a missing block means a
            // hand-written one: put the fog first, where the game's writer keeps it.
            const std::string block = RenderFogBlock(fog, childIndent);
            merged.insert(bodyStart, block);
        }

        if (!skyWritten && !mergedSky.Empty())
        {
            // The sky goes in front of the fog, the order QRE_CustomWriteLevel writes them in, so
            // inserting it at the body's start runs after the fog insert above and lands first.
            merged.insert(bodyStart, RenderSkyBlock(mergedSky, childIndent));
        }

        if (!lightsWritten)
        {
            WriteMergedLightsBlock(lights, {}, childIndent, includeEmptyLights, merged);
        }

        head = end;
    }

    if (!found)
    {
        if (!merged.empty() && merged.back() != '\n')
        {
            merged += '\n';
        }

        if (!merged.empty())
        {
            merged += '\n';
        }

        merged += RenderEditorLevelSection(level, sky, fog, lights, includeEmptyLights);
    }

    return merged;
}

bool WriteEditorFile(const std::string &path, const std::string &text, std::string &error)
{
    const std::string temporary = path + ".tmp";
    FILE *file = OpenFilePortable(temporary, "wb");

    if (file == nullptr)
    {
        error = "cannot write " + temporary;
        return false;
    }

    const size_t written = text.empty() ? 0 : fwrite(text.data(), 1, text.size(), file);
    const bool failed = written != text.size() || fflush(file) != 0;

    if (fclose(file) != 0 || failed)
    {
        remove(temporary.c_str());
        error = "write error in " + temporary;
        return false;
    }

    if (!ReplaceFilePortable(temporary, path))
    {
        remove(temporary.c_str());
        error = "cannot replace " + path;
        return false;
    }

    return true;
}

bool CopyEditorFile(const std::string &from, const std::string &to, std::string &error)
{
    std::string text;

    if (!ReadFileText(from, text))
    {
        error = "cannot read " + from;
        return false;
    }

    return WriteEditorFile(to, text, error);
}
