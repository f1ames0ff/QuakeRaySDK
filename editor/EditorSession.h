#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "EditorMaterial.h"

// The session of the game editor: an edit lives in memory until Apply writes it into
// qray.materials.yaml for a material and into the level's own section of qray.lights.yaml for the
// sky, the fog and the custom lights, and Save keeps the previous file as
// qray.backup_materials.yaml or qray.backup_lights.yaml. The standalone editor derives the paths
// from --materials, --lights and the scene, defaulting to the scene's own folder, so a session
// started next to a map edits the files that map would load.
struct EditorSessionPaths
{
    std::string materials;
    std::string materialsBackup;
    std::string lights;
    std::string lightsBackup;
    // The level section the lights file is keyed by: the map's base name without its extension,
    // lower case, exactly as RT_CustomLights_LevelKey derives it.
    std::string level;
};

EditorSessionPaths BuildEditorSessionPaths(const std::string &materialsOverride,
                                           const std::string &lightsOverride,
                                           const std::string &scenePath);

// The level section of qray.lights.yaml, in the units the file uses: `origin` is a map-space
// position, `radius` is metres (a Quake unit is 0.025 m, so RT_CUSTOM_RADIUS_DEFAULT's 0.4 is
// sixteen of the renderer's units), the angles are degrees, and `intensity` is the scale the game
// gives a custom light directly: RT_FIXUP_LIGHT_INTENSITY alone rather than the fixup divided by
// rt_elight_normaliz a map light's `light` key goes through, so one unit is
// kEditorCustomLightIntensityUnits of the editor's own value. The defaults are the ones
// RT_CustomLights_Ensure starts an entry with.
struct EditorLevelLight
{
    float origin[3] = {};
    float radius = 0.4f;
    float intensity = 3.0f;
    float color[3] = { 1.0f, 1.0f, 1.0f };
    bool spot = false;
    float direction[3] = { 1.0f, 0.0f, 0.0f };
    float angleInner = 0.0f;
    float angleOuter = 30.0f;
    int32_t style = 0;
};

// The fog of the level section, in the scale the `fog` console command takes: the renderer divides
// the density by 64, so a mid-range 0.05 fades the distance instead of everything.
struct EditorLevelFog
{
    float color[3] = { 1.0f, 1.0f, 1.0f };
    float density = 0.0f;
    bool enabled = false;
};

// The keys of the level's `sky:` block, in the order rt_lights.c writes them. The
// sky is the one block the game's editor keeps in two scopes -- the config's globals
// and the level's own overrides -- so a value here is the console text of one
// rt_sky_* cvar rather than a parsed field: the game compares those strings to
// decide what a level still overrides.
constexpr int32_t kEditorSkyKeyCount = 25;
extern const char *const kEditorSkyKeyNames[kEditorSkyKeyCount];

// The slot `name` occupies in kEditorSkyKeyNames, or -1 when the sky does not own it.
// Matched case-insensitively, the way the loader resolves a key.
int32_t EditorSkyKeyIndex(const std::string &name);
// One `sky:` value as the block spells it: floats in the block's own number spelling, integers and
// switches as `%d`, colours as `r g b` in 0..255 the way rt_sky_color is written.
std::string FormatSkyText(float value);
std::string FormatSkyText(int32_t value);
std::string FormatSkyText(bool value);
std::string FormatSkyText(const float color[3]);

// One level's `sky:` block. A slot that is clear inherits the global scope, which is
// what the game's rt_sky_level_set[] records, and an empty value writes no key at
// all, so a level keeps its own text for everything the author did not touch.
struct EditorLevelSky
{
    std::string values[kEditorSkyKeyCount];
    bool set[kEditorSkyKeyCount] = {};

    // The lines inside the block that have no slot: a cvar a newer build knows, or a comment. Kept
    // as they were written and written back after the managed keys, so a save that keeps the block
    // cannot drop what the panel cannot show. A save that drops the block drops them with it.
    std::vector<std::string> extra;

    bool Empty() const;
};

// The names rt_custom_style_names carries, in the engine's own order: the index is the
// d_lightstylevalue[] slot a map's `style` key also selects, so the file's `style:` key holds the
// name and the editor holds the index.
constexpr int32_t kEditorStyleCount = 12;
extern const char *const kEditorStyleNames[kEditorStyleCount];

// The section as the editor writes it: `<level>:` with a `sky:` block, a `fog:` block and, when
// the level has custom lights, a `lights:` block -- the order QR_CustomWriteLevel writes them. It
// is also the fingerprint the session compares against to tell whether the level needs saving.
std::string RenderEditorLevelSection(const std::string &level, const EditorLevelSky &sky,
                                     const EditorLevelFog &fog,
                                     const std::vector<EditorLevelLight> &lights);

// One touched material, as the panel left it.
struct EditorMaterialEntry
{
    std::string name;
    EditorMaterialParams params;
};

// The target qray.materials.yaml. The editor owns eleven of the file's keys and the file may
// carry thirty more it cannot express, plus comments and formatting, so an entry is merged key
// by key: the keys the editor owns are rewritten in canonical order and every other line of the
// entry is kept byte for byte. A name the target did not carry is appended.
class EditorMaterialsDocument
{
public:
    // Reads the target. A missing file is not an error - the writer then emits the header and
    // the qray_materials: key - but an unreadable one is reported through `error`.
    bool Load(const std::string &path, std::string &error);

    bool Exists() const { return m_exists; }
    const std::string &Text() const { return m_text; }
    bool HasMaterial(const std::string &name) const;
    // Reads the keys the editor owns out of `name`'s entry. Keys the entry does not carry leave
    // the matching field of `params` untouched.
    bool ReadMaterial(const std::string &name, EditorMaterialParams &params) const;

    // The merged text. Entries are matched case-insensitively, the way the loader resolves them.
    std::string WriteMerged(const std::vector<EditorMaterialEntry> &entries) const;

private:
    size_t FindEntry(const std::string &name) const;

    std::string m_text;
    bool m_exists = false;
};

// The target qray.lights.yaml, which `RT_CustomLights_ChangeMap` reads: a mapping of level names,
// each a section holding the level's `fog:`, its `lights:` and the game's own `sky:` settings.
// The editor owns all three - the section's other keys and every other level's section are kept
// byte for byte, so a file the game's editor also writes survives a session. The fog block is
// fully rewritten, the sky block keeps whatever it holds that is not one of its twenty-five keys,
// and a light entry keeps the keys the editor cannot express, which is what preserves an entry's
// `offset:`.
class EditorLevelDocument
{
public:
    bool Load(const std::string &path, std::string &error);

    bool Exists() const { return m_exists; }
    const std::string &Text() const { return m_text; }

    // What a level's section holds. `present` tells a section the file does not carry from one it
    // carries without fog or without lights, which is what a discard needs: without a section the
    // editor's own state is the only one there is.
    struct ReadResult
    {
        bool present = false;
        bool hasFog = false;
        bool hasLights = false;
        // The level carries a `sky:` block: that is what opens the game's editor in
        // level scope rather than in global scope.
        bool hasSky = false;
        EditorLevelSky sky;
        EditorLevelFog fog;
        std::vector<EditorLevelLight> lights;
    };

    ReadResult ReadLevel(const std::string &level) const;

    // The merged text. The section's `sky:`, `fog:` and `lights:` blocks are replaced where they
    // stand and appended to a section that does not carry them yet; a level the file did not
    // carry at all gets its section appended at the end.
    std::string WriteMerged(const std::string &level, const EditorLevelSky &sky,
                            const EditorLevelFog &fog,
                            const std::vector<EditorLevelLight> &lights) const;

private:
    std::string m_text;
    bool m_exists = false;
};

// Writes through `<path>.tmp` and moves it over the target, so a failure leaves the previous
// generation intact instead of a truncated file.
bool WriteEditorFile(const std::string &path, const std::string &text, std::string &error);
bool CopyEditorFile(const std::string &from, const std::string &to, std::string &error);
