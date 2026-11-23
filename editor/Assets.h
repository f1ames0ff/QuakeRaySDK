#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct EditorAssetCatalogEntry
{
    std::string path;
    std::string source;
    std::string tree;
};

// Resolves editor assets across loose game directories, Quake PAK archives, and ZIP/PKZ/PK3
// packs. Lookup is case-insensitive because Quake data mixes "PAK0.PAK", "maps/E1M1.BSP" and
// "textures/city4_2.wal" spellings in the same installation.
class EditorAssetSource
{
public:
    void Clear();
    bool AddDirectory(const std::string &path);
    bool AddPak(const std::string &path);
    bool AddZip(const std::string &path);
    bool AddArchiveOrDirectory(const std::string &path);

    bool Exists(const std::string &name) const;
    bool ReadFile(const std::string &name, std::vector<uint8_t> &out) const;
    void ListTextureAssets(std::vector<EditorAssetCatalogEntry> &out) const;
    void ListMapAssets(std::vector<EditorAssetCatalogEntry> &out) const;
    void ListModelAssets(std::vector<EditorAssetCatalogEntry> &out) const;

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

    struct ZipEntry
    {
        std::string name;
        uint16_t method = 0;
        uint32_t compressedSize = 0;
        uint32_t uncompressedSize = 0;
        uint32_t localHeaderOffset = 0;
    };

    struct ZipArchive
    {
        std::string path;
        std::vector<ZipEntry> entries;
    };

    const PakEntry *FindInPak(const Pak &pak, const std::string &name) const;
    const ZipEntry *FindInZip(const ZipArchive &zip, const std::string &name) const;
    bool ReadFromDirectories(const std::string &normalized, std::vector<uint8_t> &out) const;
    bool ReadFromZips(const std::string &normalized, std::vector<uint8_t> &out) const;
    bool ReadFromPaks(const std::string &normalized, std::vector<uint8_t> &out) const;

    std::vector<std::string> m_directories;
    std::vector<std::string> m_archives;
    std::vector<Pak> m_paks;
    std::vector<ZipArchive> m_zips;
};

// Decodes PNG/TGA/JPG/etc. bytes in memory into tightly-packed 8-bit RGBA pixels.
bool DecodeEditorImageRgba(const std::vector<uint8_t> &bytes, uint32_t &outWidth,
                           uint32_t &outHeight, std::vector<uint8_t> &outRgba);

// Returns the directory part of a path, without the trailing separator.
std::string EditorDirectoryOf(const std::string &path);
