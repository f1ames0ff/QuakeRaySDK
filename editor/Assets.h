#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Resolves editor assets across loose game directories and Quake PAK archives.
// Lookup is case-insensitive because Quake data mixes "PAK0.PAK", "maps/E1M1.BSP"
// and "textures/city4_2.wal" spellings in the same installation.
class EditorAssetSource
{
public:
    bool AddDirectory(const std::string &path);
    bool AddPak(const std::string &path);
    bool AddArchiveOrDirectory(const std::string &path);

    bool Exists(const std::string &name) const;
    bool ReadFile(const std::string &name, std::vector<uint8_t> &out) const;

    const std::vector<std::string> &Directories() const { return m_directories; }
    const std::vector<std::string> &Archives() const { return m_archives; }

private:
    struct PakEntry
    {
        std::string name;
        uint32_t offset = 0;
        uint32_t size = 0;
    };

    struct Pak
    {
        std::string path;
        std::vector<PakEntry> entries;
    };

    const PakEntry *FindInPak(const Pak &pak, const std::string &name) const;

    std::vector<std::string> m_directories;
    std::vector<std::string> m_archives;
    std::vector<Pak> m_paks;
};

// Returns the directory part of a path, without the trailing separator.
std::string EditorDirectoryOf(const std::string &path);
