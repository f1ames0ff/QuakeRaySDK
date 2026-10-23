#include "EditorSession.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include <windows.h>

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

    while (end < line.size() && line[end] != '\r' && line[end] != '\n' && line[end] != '#')
    {
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

    if (params.emissiveFactor != 0.0f)
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

    // QRE_WriteEmissiveBlocks writes the list, then QRE_WriteMaterial writes the two material
    // defaults the blocks inherit, and only when they differ from the file's own defaults.
    if (!params.emissiveBlocks.empty())
    {
        append("luma", std::string());
    }

    if (params.lumaThreshold != 0.02f)
    {
        append("luma_threshold", FormatFloat(params.lumaThreshold));
    }

    if (params.lumaFeather != 0.0f)
    {
        append("luma_feather", FormatFloat(params.lumaFeather));
    }

    return values;
}

// One owned key as YAML. Every key but `luma` is a scalar on its own line; `luma` is the list of
// colour blocks, so it lays them out under the key the way QRE_WriteEmissiveBlocks does.
void AppendOwnedValue(std::string &out, const std::string &indent, const char *key,
                      const std::string &value, const EditorMaterialParams &params)
{
    if (std::string_view(key) != "luma")
    {
        out += indent;
        out += key;
        out += ": ";
        out += value;
        out += '\n';
        return;
    }

    out += indent + "luma:\n";

    for (const EditorEmissiveBlock &block : params.emissiveBlocks)
    {
        char buffer[64];

        if (block.polyUv.size() >= 6)
        {
            out += indent + "  - polygon:";

            for (size_t p = 0; p + 1 < block.polyUv.size(); p += 2)
            {
                std::snprintf(buffer, sizeof(buffer), " %.4f,%.4f", block.polyUv[p],
                              block.polyUv[p + 1]);
                out += buffer;
            }

            out += '\n';
        }
        else
        {
            out += indent + "  - color: " + FormatColor(block.color) + "\n";
            out += indent + "    threshold: " + FormatFloat(block.threshold) + "\n";
        }

        out += indent + "    feather: " + FormatFloat(block.feather) + "\n";
        out += indent + "    emissive_factor: " + FormatFloat(block.factor) + "\n";
        out += indent + "    blend: " + BlendNameOf(block.blend) + "\n";
    }
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

    while (!text.empty() &&
           (IsSpace(text.back()) || text.back() == '}' || text.back() == '\r'))
    {
        text.remove_suffix(1);
    }

    return text;
}

// RT_MAT_EMIS_POLY_MAX points of a `- polygon:` value, clamped to the unit square; fewer than
// three of them are no polygon at all, which is the reset the loader does.
void ReadEmissivePolygon(std::string_view value, EditorEmissiveBlock &block)
{
    block.polyUv.clear();

    const std::string text(value);
    const char *cursor = text.c_str();

    while (*cursor && block.polyUv.size() < 32)
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
        block.polyUv.push_back(std::clamp(u, 0.0f, 1.0f));
        block.polyUv.push_back(std::clamp(v, 0.0f, 1.0f));

        while (*cursor == ' ' || *cursor == '\t')
        {
            cursor++;
        }
    }

    if (block.polyUv.size() < 6)
    {
        block.polyUv.clear();
    }
}

void ApplyEmissiveBlockKey(std::string_view key, std::string_view value, EditorEmissiveBlock &block,
                           uint32_t &has)
{
    if (key == "color")
    {
        ParseColor(value, block.color);
    }
    else if (key == "polygon")
    {
        ReadEmissivePolygon(value, block);
    }
    else if (key == "threshold")
    {
        block.threshold = FloatOf(value, block.threshold);
        has |= kOwnedThreshold;
    }
    else if (key == "feather")
    {
        // The loader rounds to whole pixels and clamps to the 16 the blur can reach.
        const float pixels = FloatOf(value, block.feather);
        block.feather = std::clamp(float(int(pixels + 0.5f)), 0.0f, 16.0f);
        has |= kOwnedFeather;
    }
    else if (key == "emissive_factor" || key == "factor")
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

// Reads the `luma` key of one entry in either of its authored forms: the list of colour / polygon
// blocks, or the legacy `luma: ff0000,00ff00` scalar the game still accepts.
void ReadEmissiveBlocks(const std::string &text, const std::vector<TextLine> &lines, size_t begin,
                        size_t end, size_t keyIndent, EditorMaterialParams &params)
{
    std::vector<uint32_t> has;

    for (size_t i = begin; i < end; i++)
    {
        const std::string_view line = lines[i].View(text);

        if (LeadingWhitespace(line) != keyIndent || KeyOf(line) != "luma")
        {
            continue;
        }

        const std::string_view scalar = Trimmed(ValueOf(line));

        if (!scalar.empty())
        {
            size_t start = 0;

            while (start < scalar.size() && params.emissiveBlocks.size() < kEditorEmissiveBlockMax)
            {
                size_t stop = scalar.find_first_of(", \t", start);

                if (stop == std::string_view::npos)
                {
                    stop = scalar.size();
                }

                if (stop > start)
                {
                    EditorEmissiveBlock block;
                    ParseColor(scalar.substr(start, stop - start), block.color);
                    params.emissiveBlocks.push_back(block);
                    has.push_back(0);
                }

                start = stop + 1;
            }

            continue;
        }

        size_t current = size_t(-1);

        for (size_t k = i + 1; k < end; k++)
        {
            const std::string_view nested = lines[k].View(text);

            if (IsBlankLine(nested))
            {
                continue;
            }

            const size_t indent = LeadingWhitespace(nested);

            if (indent <= keyIndent)
            {
                break;
            }

            std::string_view item = nested.substr(indent);

            if (item.size() >= 2 && item.front() == '-' && IsSpace(item[1]))
            {
                // Past the loader's cap the items are skipped rather than written into a block the
                // game would not read anyway.
                if (params.emissiveBlocks.size() >= kEditorEmissiveBlockMax)
                {
                    current = size_t(-1);
                    continue;
                }

                params.emissiveBlocks.emplace_back();
                has.push_back(0);
                current = params.emissiveBlocks.size() - 1;
                item = Trimmed(item.substr(1));
            }

            if (current == size_t(-1))
            {
                continue;
            }

            const size_t colon = item.find(':');

            if (colon != std::string_view::npos)
            {
                ApplyEmissiveBlockKey(Trimmed(item.substr(0, colon)),
                                     Trimmed(item.substr(colon + 1)),
                                     params.emissiveBlocks[current], has[current]);
            }
        }
    }

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
    static const char *const keys[] = { "origin",  "radius",     "intensity",   "color",
                                        "spot",    "dir",        "angle_inner", "angle_outer",
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
                entry.extra.push_back(TrimmedLine(line));
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

// The entry as RT_CustomLights_WriteEntry lays it out. `extra` carries the keys the editor cannot
// express, which is how an entry's `offset:` survives a session.
std::string RenderLightEntry(const EditorLevelLight &light, const std::vector<std::string> &extra,
                             size_t childIndent)
{
    const std::string head(childIndent + 2, ' ');
    const std::string field(childIndent + 4, ' ');
    std::string text;

    text += head + "- origin: " + FormatVector(light.origin) + "\n";

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
    }

    if (light.style > 0)
    {
        const int32_t style =
            light.style < kEditorStyleCount ? light.style : kEditorStyleCount - 1;
        text += field + "style: " + std::string(kEditorStyleNames[style]) + "\n";
    }

    return text;
}

bool ReadFileText(const std::string &path, std::string &text)
{
    FILE *file = nullptr;

    if (fopen_s(&file, path.c_str(), "rb") != 0 || file == nullptr)
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
                                                 : sceneDirectory + "\\qray.materials.yaml";
    }

    if (!lightsOverride.empty())
    {
        paths.lights = lightsOverride;
    }
    else
    {
        paths.lights = sceneDirectory.empty() ? "qray.lights.yaml"
                                              : sceneDirectory + "\\qray.lights.yaml";
    }

    const std::string materialsDirectory = EditorDirectoryOf(paths.materials);
    paths.materialsBackup = (materialsDirectory.empty() ? std::string(".") : materialsDirectory) +
                            "\\qray.backup_materials.yaml";

    const std::string lightsDirectory = EditorDirectoryOf(paths.lights);
    paths.lightsBackup = (lightsDirectory.empty() ? std::string(".") : lightsDirectory) +
                         "\\qray.backup_lights.yaml";

    paths.level = LevelKeyOf(scenePath);
    return paths;
}

bool EditorMaterialsDocument::Load(const std::string &path, std::string &error)
{
    m_text.clear();
    m_exists = false;

    FILE *probe = nullptr;

    if (fopen_s(&probe, path.c_str(), "rb") != 0 || probe == nullptr)
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

size_t EditorMaterialsDocument::FindEntry(const std::string &name) const
{
    const std::string wanted = Lowercase(name);
    const std::vector<TextLine> lines = SplitLines(m_text);

    for (size_t i = 0; i < lines.size(); i++)
    {
        std::string found;

        if (NameOf(lines[i].View(m_text), found) && found == wanted)
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

    params.emissiveBlocks.clear();

    size_t end = head + 1;

    for (; end < lines.size(); end++)
    {
        const std::string_view line = lines[end].View(m_text);
        std::string nextName;

        // The entry ends at the next sequence item of its own name, whatever indentation the file
        // uses, so a neighbour's keys never count as this entry's.
        if (!IsBodyLine(line) || NameOf(line, nextName))
        {
            break;
        }
    }

    size_t keyIndent = size_t(-1);

    for (size_t i = head + 1; i < end; i++)
    {
        const std::string_view line = lines[i].View(m_text);

        if (KeyOf(line).empty())
        {
            continue;
        }

        const size_t level = LeadingWhitespace(line);

        if (level < keyIndent)
        {
            keyIndent = level;
        }
    }

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
    }

    // The blocks inherit the material-level tone controls, so they are read once those are known.
    ReadEmissiveBlocks(m_text, lines, head + 1, end, keyIndent, params);

    // a material that asks for both is glass, as the loader decides
    params.type = glass ? EditorMaterialType::Glass
                        : (water ? EditorMaterialType::Water : EditorMaterialType::None);
    return true;
}

std::string EditorMaterialsDocument::WriteMerged(const std::vector<EditorMaterialEntry> &entries) const
{
    std::string merged;
    std::vector<bool> written(entries.size(), false);

    if (!m_exists || m_text.empty())
    {
        merged += kMaterialsHeader;
        merged += "qray_materials:\n";
    }

    std::vector<std::string> wanted(entries.size());

    for (size_t i = 0; i < entries.size(); i++)
    {
        wanted[i] = Lowercase(entries[i].name);
    }

    const std::vector<TextLine> lines = SplitLines(m_text);

    for (size_t i = 0; i < lines.size(); i++)
    {
        const std::string_view line = lines[i].View(m_text);
        std::string found;
        size_t entry = size_t(-1);

        if (NameOf(line, found))
        {
            for (size_t e = 0; e < wanted.size(); e++)
            {
                if (wanted[e] == found && !written[e])
                {
                    entry = e;
                    break;
                }
            }
        }

        if (entry == size_t(-1))
        {
            merged += MigratedRootLine(line);
            continue;
        }

        merged += line;

        if (!line.empty() && line.back() != '\n')
        {
            merged += '\n';
        }

        // the rest of the entry is kept as it was: the keys the editor cannot express, the
        // comments, the nested luma blocks and the original ordering
        size_t body = i + 1;

        for (; body < lines.size(); body++)
        {
            const std::string_view bodyLine = lines[body].View(m_text);
            std::string nextName;

            // A sequence item of its own name starts the next entry whatever indentation the file
            // happens to use, so it never becomes part of this entry's body.
            if (!IsBodyLine(bodyLine) || NameOf(bodyLine, nextName))
            {
                break;
            }
        }

        // The entry's own keys sit at the shallowest indentation of its body; a nested block is
        // deeper, so a key the editor owns that appears inside one stays with that block.
        size_t keyIndent = size_t(-1);

        for (size_t k = i + 1; k < body; k++)
        {
            const std::string_view bodyLine = lines[k].View(m_text);

            if (KeyOf(bodyLine).empty())
            {
                continue;
            }

            const size_t level = LeadingWhitespace(bodyLine);

            if (level < keyIndent)
            {
                keyIndent = level;
            }
        }

        // New values take the indentation the entry already used, so a hand-written file keeps its
        // shape; an entry with no keys at all gets the writer's own two spaces deeper.
        const std::string indent(keyIndent != size_t(-1) ? keyIndent : LeadingWhitespace(line) + 2, ' ');

        const std::vector<std::pair<const char *, std::string>> values =
            OwnedValues(entries[entry].params);

        for (const auto &[key, value] : values)
        {
            AppendOwnedValue(merged, indent, key, value, entries[entry].params);
        }

        for (size_t k = i + 1; k < body; k++)
        {
            const std::string_view bodyLine = lines[k].View(m_text);
            const std::string_view key = KeyOf(bodyLine);
            bool emitted = false;

            // Only a key the writer just wrote is taken out of the body: one it left at its file
            // default stays where it was, so an untouched value keeps its own line.
            if (!key.empty() && LeadingWhitespace(bodyLine) == keyIndent)
            {
                for (const auto &[emittedKey, emittedValue] : values)
                {
                    if (key == emittedKey)
                    {
                        emitted = true;
                        break;
                    }
                }
            }

            if (emitted)
            {
                // A list-valued key carries everything nested under it, so the block goes with the
                // key line; leaving it behind would duplicate the blocks the writer just wrote.
                for (k++; k < body; k++)
                {
                    const std::string_view nested = lines[k].View(m_text);

                    if (!IsBlankLine(nested) && LeadingWhitespace(nested) <= keyIndent)
                    {
                        break;
                    }
                }

                if (k < body)
                {
                    k--;
                }

                continue;
            }

            merged += bodyLine;
        }

        written[entry] = true;
        i = body - 1;
    }

    for (size_t e = 0; e < entries.size(); e++)
    {
        if (written[e])
        {
            continue;
        }

        // A file whose last line had no terminator would otherwise swallow the new entry.
        if (!merged.empty() && merged.back() != '\n')
        {
            merged += '\n';
        }

        merged += "  - name: " + entries[e].name + "\n";

        for (const auto &[key, value] : OwnedValues(entries[e].params))
        {
            AppendOwnedValue(merged, std::string(4, ' '), key, value, entries[e].params);
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
                                     const std::vector<EditorLevelLight> &lights)
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

    return text;
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

std::string EditorLevelDocument::WriteMerged(const std::string &level, const EditorLevelSky &sky,
                                             const EditorLevelFog &fog,
                                             const std::vector<EditorLevelLight> &lights) const
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
                // The game writes no `lights:` block for a level without custom lights, and
                // neither does the editor: an empty list removes the block it owned.
                if (!lights.empty())
                {
                    merged += std::string(childIndent, ' ') + "lights:\n";

                    std::vector<bool> used(oldEntries.size(), false);

                    for (const EditorLevelLight &light : lights)
                    {
                        std::vector<std::string> extra;

                        for (size_t e = 0; e < oldEntries.size(); e++)
                        {
                            const EditorLevelLight &old = oldEntries[e].light;

                            if (used[e] || old.origin[0] != light.origin[0] ||
                                old.origin[1] != light.origin[1] ||
                                old.origin[2] != light.origin[2])
                            {
                                continue;
                            }

                            used[e] = true;
                            extra = oldEntries[e].extra;
                            break;
                        }

                        merged += RenderLightEntry(light, extra, childIndent);
                    }

                    lightsWritten = true;
                }
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

        if (!lightsWritten && !lights.empty())
        {
            merged += std::string(childIndent, ' ') + "lights:\n";

            for (const EditorLevelLight &light : lights)
            {
                merged += RenderLightEntry(light, {}, childIndent);
            }
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

        merged += RenderEditorLevelSection(level, sky, fog, lights);
    }

    return merged;
}

bool WriteEditorFile(const std::string &path, const std::string &text, std::string &error)
{
    const std::string temporary = path + ".tmp";
    FILE *file = nullptr;

    if (fopen_s(&file, temporary.c_str(), "wb") != 0 || file == nullptr)
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

    if (!MoveFileExA(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
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
