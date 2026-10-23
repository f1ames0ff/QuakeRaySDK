#include "EditorSession.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include <windows.h>

#include "Assets.h"

namespace {

// The keys EditorMaterialParams owns. Written in the game writer's own order for the fields the
// two share, so a block the game wrote and the editor re-writes keeps its shape.
const char *const kOwnedKeys[] = {
    "texture_scale_u",  "texture_scale_v", "texture_offset_u", "texture_offset_v",
    "roughness_override", "metalness_factor", "base_factor",    "emissive_factor",
    "is_light",         "exact_normals",    "material_glass",   "material_water",
};

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

bool IsOwnedKey(std::string_view key)
{
    for (const char *owned : kOwnedKeys)
    {
        if (key == owned)
        {
            return true;
        }
    }

    return false;
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

// The owned keys of `params`, in canonical order, as `key`/`value` pairs. A key at its file
// default is left out, which is what keeps a save from growing the file by eleven dead lines.
std::vector<std::pair<const char *, std::string>> OwnedValues(const EditorMaterialParams &params)
{
    std::vector<std::pair<const char *, std::string>> values;

    auto append = [&values](const char *key, const std::string &value) {
        values.emplace_back(key, value);
    };

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

    if (params.roughnessOverride >= 0.0f)
    {
        append("roughness_override", FormatFloat(params.roughnessOverride));
    }

    if (params.metalnessFactor != 0.0f)
    {
        append("metalness_factor", FormatFloat(params.metalnessFactor));
    }

    if (params.baseFactor != 1.0f)
    {
        append("base_factor", FormatFloat(params.baseFactor));
    }

    if (params.emissiveFactor != 0.0f)
    {
        append("emissive_factor", FormatFloat(params.emissiveFactor));
    }

    // The editor's Luma checkbox uploads QR_GEOMETRY_UPLOAD_EMISSION_RAY_ONLY_BIT, and the game
    // reaches the same batch flag through is_light, so the file's own key is the one written.
    if (params.luma)
    {
        append("is_light", "true");
    }

    if (params.exactNormals)
    {
        append("exact_normals", "true");
    }

    if (params.type == EditorMaterialType::Glass)
    {
        append("material_glass", "true");
    }
    else if (params.type == EditorMaterialType::Water)
    {
        append("material_water", "true");
    }

    return values;
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
                                           const std::string &scenePath)
{
    EditorSessionPaths paths;

    if (!materialsOverride.empty())
    {
        paths.materials = materialsOverride;
    }
    else
    {
        const std::string directory = EditorDirectoryOf(scenePath);
        paths.materials = directory.empty() ? "qray.materials.yaml" : directory + "\\qray.materials.yaml";
    }

    const std::string directory = EditorDirectoryOf(paths.materials);
    paths.materialsBackup = (directory.empty() ? std::string(".") : directory) +
                            "\\qray.backup_materials.yaml";
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

        if (key == "texture_scale_u")
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
        else if (key == "metalness_factor")
        {
            params.metalnessFactor = FloatOf(value, params.metalnessFactor);
        }
        else if (key == "base_factor")
        {
            params.baseFactor = FloatOf(value, params.baseFactor);
        }
        else if (key == "emissive_factor")
        {
            params.emissiveFactor = FloatOf(value, params.emissiveFactor);
        }
        else if (key == "is_light")
        {
            params.luma = BoolOf(value, params.luma);
        }
        else if (key == "exact_normals")
        {
            params.exactNormals = BoolOf(value, params.exactNormals);
        }
        else if (key == "material_glass")
        {
            glass = BoolOf(value, glass);
        }
        else if (key == "material_water")
        {
            water = BoolOf(value, water);
        }
    }

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

        for (const auto &[key, value] : OwnedValues(entries[entry].params))
        {
            merged += indent + key + ": " + value + "\n";
        }

        for (size_t k = i + 1; k < body; k++)
        {
            const std::string_view bodyLine = lines[k].View(m_text);
            const std::string_view key = KeyOf(bodyLine);

            if (!key.empty() && LeadingWhitespace(bodyLine) == keyIndent && IsOwnedKey(key))
            {
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
            merged += std::string(4, ' ') + key + ": " + value + "\n";
        }
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
