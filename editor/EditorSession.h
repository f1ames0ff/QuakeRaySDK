#pragma once

#include <string>
#include <vector>

#include "EditorMaterial.h"

// The session of the game editor: a material edit lives in memory until Apply writes it into
// qray.materials.yaml, and Save keeps the previous file as qray.backup_materials.yaml. The
// standalone editor derives the same two paths from --materials, defaulting to the scene's own
// folder, so a session started next to a map edits the file that map would load.
struct EditorSessionPaths
{
    std::string materials;
    std::string materialsBackup;
};

EditorSessionPaths BuildEditorSessionPaths(const std::string &materialsOverride,
                                           const std::string &scenePath);

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

// Writes through `<path>.tmp` and moves it over the target, so a failure leaves the previous
// generation intact instead of a truncated file.
bool WriteEditorFile(const std::string &path, const std::string &text, std::string &error);
bool CopyEditorFile(const std::string &from, const std::string &to, std::string &error);
