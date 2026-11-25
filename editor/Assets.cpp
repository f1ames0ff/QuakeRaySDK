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

#include "Assets.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_set>

#include <stb_image.h>

namespace
{

constexpr uint32_t PakEntrySize = 64;
constexpr uint32_t PakHeaderSize = 12;

char LowerAscii(char value)
{
    return (value >= 'A' && value <= 'Z') ? char(value - 'A' + 'a') : value;
}

bool EqualsIgnoreCase(const std::string &left, const std::string &right)
{
    if (left.size() != right.size())
    {
        return false;
    }

    for (size_t i = 0; i < left.size(); i++)
    {
        if (LowerAscii(left[i]) != LowerAscii(right[i]))
        {
            return false;
        }
    }

    return true;
}

std::string NormalizeAssetName(const std::string &name)
{
    std::string result;
    result.reserve(name.size());

    for (char character : name)
    {
        result.push_back(character == '\\' ? '/' : character);
    }

    return result;
}

bool ReadWholeFile(const std::string &path, std::vector<uint8_t> &out)
{
    std::ifstream file(path, std::ios::binary);

    if (!file)
    {
        return false;
    }

    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

uint16_t ReadUint16(const uint8_t *bytes)
{
    return uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8);
}

uint32_t ReadUint32(const uint8_t *bytes)
{
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) |
           (uint32_t(bytes[3]) << 24);
}

constexpr size_t kZipEocdSize = 22;
constexpr size_t kZipMaxComment = 65535;
constexpr uint32_t kZipEocdSignature = 0x06054b50u;
constexpr uint32_t kZipCentralSignature = 0x02014b50u;
constexpr uint32_t kZipLocalSignature = 0x04034b50u;
constexpr uint32_t kZipMaxEntryBytes = 256u * 1024u * 1024u;

bool FindZipEocd(const std::vector<uint8_t> &bytes, size_t &outOffset)
{
    if (bytes.size() < kZipEocdSize)
    {
        return false;
    }

    const size_t minOffset =
        bytes.size() > kZipEocdSize + kZipMaxComment ? bytes.size() - (kZipEocdSize + kZipMaxComment) : 0;

    for (size_t pos = bytes.size() - kZipEocdSize;; pos--)
    {
        if (ReadUint32(&bytes[pos]) == kZipEocdSignature)
        {
            const uint16_t commentLen = ReadUint16(&bytes[pos + 20]);

            if (pos + kZipEocdSize + commentLen <= bytes.size())
            {
                outOffset = pos;
                return true;
            }
        }

        if (pos == minOffset)
        {
            break;
        }
    }

    return false;
}

bool PathsMatch(const std::string &left, const std::string &right)
{
#if defined(_WIN32)
    return EqualsIgnoreCase(left, right);
#else
    return left == right;
#endif
}

bool HasArchivePath(const std::vector<std::string> &archives, const std::string &path)
{
    for (const std::string &existing : archives)
    {
        if (PathsMatch(existing, path))
        {
            return true;
        }
    }

    return false;
}

std::string NormalizeZipEntryName(const char *raw, size_t length)
{
    std::string name = NormalizeAssetName(std::string(raw, length));
    size_t leading = 0;

    while (leading < name.size() && name[leading] == '/')
    {
        leading++;
    }

    return leading > 0 ? name.substr(leading) : name;
}

#if !defined(_WIN32)
bool FindEntryIgnoreCase(const std::filesystem::path &directory, const std::string &segment,
                         std::filesystem::path &out)
{
    std::error_code ec;
    const std::filesystem::path direct = directory / segment;

    if (std::filesystem::exists(direct, ec))
    {
        out = direct;
        return true;
    }

    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(directory, ec))
    {
        if (EqualsIgnoreCase(entry.path().filename().string(), segment))
        {
            out = entry.path();
            return true;
        }
    }

    return false;
}

bool ResolveCaseInsensitivePath(const std::string &directory, const std::string &normalized,
                                std::string &resolved)
{
    std::error_code ec;
    std::filesystem::path current(directory);

    if (!std::filesystem::is_directory(current, ec))
    {
        return false;
    }

    size_t start = 0;

    while (start < normalized.size())
    {
        const size_t slash = normalized.find('/', start);
        const std::string segment = (slash == std::string::npos)
                                        ? normalized.substr(start)
                                        : normalized.substr(start, slash - start);
        start = (slash == std::string::npos) ? normalized.size() : slash + 1;

        if (segment.empty())
        {
            continue;
        }

        if (!FindEntryIgnoreCase(current, segment, current))
        {
            return false;
        }
    }

    resolved = current.string();
    return !resolved.empty();
}
#endif

} // namespace

std::string EditorDirectoryOf(const std::string &path)
{
    const size_t separator = path.find_last_of("\\/");

    if (separator == std::string::npos)
    {
        return {};
    }

    return path.substr(0, separator);
}

void EditorAssetSource::Clear()
{
    m_directories.clear();
    m_archives.clear();
    m_paks.clear();
    m_zips.clear();
}

bool EditorAssetSource::AddDirectory(const std::string &path)
{
    if (path.empty())
    {
        return false;
    }

    for (const std::string &existing : m_directories)
    {
        if (PathsMatch(existing, path))
        {
            return false;
        }
    }

    m_directories.push_back(path);
    return true;
}

namespace
{

bool ReadPakDirectory(const std::string &path, std::vector<uint8_t> &directoryBytes,
                      uint64_t &outFileSize)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);

    if (!file)
    {
        return false;
    }

    const std::streamoff endPos = file.tellg();

    if (endPos < std::streamoff(PakHeaderSize))
    {
        return false;
    }

    const uint64_t fileSize = uint64_t(endPos);
    uint8_t header[PakHeaderSize] = {};
    file.seekg(0);
    file.read(reinterpret_cast<char *>(header), PakHeaderSize);

    if (!file || std::memcmp(header, "PACK", 4) != 0)
    {
        return false;
    }

    const uint32_t directoryOffset = ReadUint32(header + 4);
    const uint32_t directoryLength = ReadUint32(header + 8);

    if (directoryLength % PakEntrySize != 0 || uint64_t(directoryOffset) > fileSize ||
        uint64_t(directoryLength) > fileSize - uint64_t(directoryOffset))
    {
        return false;
    }

    directoryBytes.resize(directoryLength);

    if (directoryLength > 0)
    {
        file.seekg(std::streamoff(directoryOffset));
        file.read(reinterpret_cast<char *>(directoryBytes.data()),
                  std::streamsize(directoryLength));

        if (!file)
        {
            return false;
        }
    }

    outFileSize = fileSize;
    return true;
}

} // namespace

bool EditorAssetSource::AddPak(const std::string &path)
{
    if (path.empty() || HasArchivePath(m_archives, path))
    {
        return false;
    }

    std::vector<uint8_t> directoryBytes;
    uint64_t fileSize = 0;

    if (!ReadPakDirectory(path, directoryBytes, fileSize))
    {
        return false;
    }

    Pak pak;
    pak.path = path;

    const uint32_t entryCount = uint32_t(directoryBytes.size() / PakEntrySize);
    pak.entries.reserve(entryCount);

    for (uint32_t i = 0; i < entryCount; i++)
    {
        const uint8_t *entry = directoryBytes.data() + size_t(i) * PakEntrySize;

        PakEntry pakEntry;
        const char *nameEnd = static_cast<const char *>(std::memchr(entry, '\0', 56));
        pakEntry.name = std::string(static_cast<const char *>(static_cast<const void *>(entry)),
                                    nameEnd ? size_t(nameEnd - reinterpret_cast<const char *>(entry)) : 56);
        pakEntry.offset = ReadUint32(entry + 56);
        pakEntry.size = ReadUint32(entry + 60);

        if (pakEntry.name.empty() || uint64_t(pakEntry.offset) > fileSize ||
            uint64_t(pakEntry.size) > fileSize - uint64_t(pakEntry.offset))
        {
            continue;
        }

        pak.entries.push_back(std::move(pakEntry));
    }

    m_archives.push_back(path);
    m_paks.push_back(std::move(pak));
    return true;
}

bool EditorAssetSource::AddZip(const std::string &path)
{
    if (path.empty() || HasArchivePath(m_archives, path))
    {
        return false;
    }

    std::vector<uint8_t> bytes;
    size_t eocdOffset = 0;

    if (!ReadWholeFile(path, bytes) || !FindZipEocd(bytes, eocdOffset))
    {
        return false;
    }

    const uint16_t entryCount = ReadUint16(&bytes[eocdOffset + 10]);
    const uint32_t cdSize = ReadUint32(&bytes[eocdOffset + 12]);
    const uint32_t cdOffset = ReadUint32(&bytes[eocdOffset + 16]);

    if (cdOffset > bytes.size() || cdSize > bytes.size() - cdOffset)
    {
        return false;
    }

    ZipArchive zip;
    zip.path = path;
    zip.entries.reserve(entryCount);

    size_t cursor = cdOffset;
    const size_t cdEnd = size_t(cdOffset) + cdSize;

    while (cursor + 46 <= cdEnd && zip.entries.size() < entryCount)
    {
        if (ReadUint32(&bytes[cursor]) != kZipCentralSignature)
        {
            break;
        }

        const uint16_t nameLen = ReadUint16(&bytes[cursor + 28]);
        const uint16_t extraLen = ReadUint16(&bytes[cursor + 30]);
        const uint16_t commentLen = ReadUint16(&bytes[cursor + 32]);
        const size_t next = cursor + 46 + nameLen + extraLen + commentLen;

        if (next > cdEnd)
        {
            break;
        }

        const uint16_t method = ReadUint16(&bytes[cursor + 10]);
        std::string entryName =
            NormalizeZipEntryName(reinterpret_cast<const char *>(&bytes[cursor + 46]), nameLen);

        if (!entryName.empty() && entryName.back() != '/' && (method == 0 || method == 8))
        {
            ZipEntry entry;
            entry.name = std::move(entryName);
            entry.method = method;
            entry.compressedSize = ReadUint32(&bytes[cursor + 20]);
            entry.uncompressedSize = ReadUint32(&bytes[cursor + 24]);
            entry.localHeaderOffset = ReadUint32(&bytes[cursor + 42]);
            zip.entries.push_back(std::move(entry));
        }

        cursor = next;
    }

    m_archives.push_back(path);
    m_zips.push_back(std::move(zip));
    return true;
}

bool EditorAssetSource::AddArchiveOrDirectory(const std::string &path)
{
    std::error_code ec;

    if (std::filesystem::is_directory(path, ec))
    {
        return AddDirectory(path);
    }

    return AddPak(path) || AddZip(path);
}

const EditorAssetSource::PakEntry *EditorAssetSource::FindInPak(const Pak &pak,
                                                               const std::string &name) const
{
    for (const PakEntry &entry : pak.entries)
    {
        if (EqualsIgnoreCase(entry.name, name))
        {
            return &entry;
        }
    }

    return nullptr;
}

const EditorAssetSource::ZipEntry *EditorAssetSource::FindInZip(const ZipArchive &zip,
                                                               const std::string &name) const
{
    for (const ZipEntry &entry : zip.entries)
    {
        if (EqualsIgnoreCase(entry.name, name))
        {
            return &entry;
        }
    }

    return nullptr;
}

namespace
{

bool ReadZipEntryPayload(const std::string &archivePath, uint16_t method, uint32_t compressedSize,
                         uint32_t uncompressedSize, uint32_t localHeaderOffset,
                         std::vector<uint8_t> &out)
{
    if (compressedSize > kZipMaxEntryBytes || uncompressedSize > kZipMaxEntryBytes)
    {
        return false;
    }

    std::ifstream file(archivePath, std::ios::binary);

    if (!file)
    {
        return false;
    }

    uint8_t local[30] = {};
    file.seekg(std::streamoff(localHeaderOffset));
    file.read(reinterpret_cast<char *>(local), sizeof(local));

    if (!file || ReadUint32(local) != kZipLocalSignature)
    {
        return false;
    }

    const uint16_t localNameLen = ReadUint16(local + 26);
    const uint16_t localExtraLen = ReadUint16(local + 28);
    file.seekg(std::streamoff(localHeaderOffset) + 30 + std::streamoff(localNameLen) +
               std::streamoff(localExtraLen));

    std::vector<uint8_t> compressed(compressedSize);

    if (!compressed.empty())
    {
        file.read(reinterpret_cast<char *>(compressed.data()), std::streamsize(compressed.size()));
    }

    if (!file)
    {
        return false;
    }

    if (method == 0)
    {
        if (compressedSize != uncompressedSize)
        {
            return false;
        }

        out = std::move(compressed);
        return true;
    }

    if (method == 8)
    {
        if (uncompressedSize == 0)
        {
            out.clear();
            return true;
        }

        if (compressedSize > uint32_t(INT_MAX) - 8 || uncompressedSize > uint32_t(INT_MAX))
        {
            return false;
        }

        // stbi__zhuffman_decode bails out when num_bits < 16 at EOF; pad 8 bytes past raw DEFLATE.
        compressed.resize(size_t(compressedSize) + 8, 0);
        out.resize(uncompressedSize);
        const int decoded = stbi_zlib_decode_noheader_buffer(
            reinterpret_cast<char *>(out.data()), int(out.size()),
            reinterpret_cast<const char *>(compressed.data()), int(compressed.size()));
        return decoded == int(uncompressedSize);
    }

    return false;
}

} // namespace

bool EditorAssetSource::ReadFromDirectories(const std::string &normalized,
                                            std::vector<uint8_t> &out) const
{
    for (const std::string &directory : m_directories)
    {
        std::string candidate = directory;

        if (!candidate.empty() && candidate.back() != '/' && candidate.back() != '\\')
        {
            candidate.push_back('/');
        }

        candidate += normalized;

        if (ReadWholeFile(candidate, out))
        {
            return true;
        }

#if !defined(_WIN32)
        std::string resolved;

        if (ResolveCaseInsensitivePath(directory, normalized, resolved) &&
            ReadWholeFile(resolved, out))
        {
            return true;
        }
#endif
    }

    return false;
}

bool EditorAssetSource::ReadFromZips(const std::string &normalized, std::vector<uint8_t> &out) const
{
    for (const ZipArchive &zip : m_zips)
    {
        const ZipEntry *entry = FindInZip(zip, normalized);

        if (entry != nullptr &&
            ReadZipEntryPayload(zip.path, entry->method, entry->compressedSize,
                                entry->uncompressedSize, entry->localHeaderOffset, out))
        {
            return true;
        }
    }

    return false;
}

bool EditorAssetSource::ReadFromPaks(const std::string &normalized, std::vector<uint8_t> &out) const
{
    for (const Pak &pak : m_paks)
    {
        const PakEntry *entry = FindInPak(pak, normalized);

        if (entry == nullptr)
        {
            continue;
        }

        std::ifstream file(pak.path, std::ios::binary);

        if (!file)
        {
            continue;
        }

        out.resize(entry->size);
        file.seekg(std::streamoff(entry->offset));

        if (!out.empty())
        {
            file.read(reinterpret_cast<char *>(out.data()), std::streamsize(out.size()));
        }

        if (file)
        {
            return true;
        }
    }

    return false;
}

bool EditorAssetSource::Exists(const std::string &name) const
{
    std::vector<uint8_t> bytes;
    return ReadFile(name, bytes);
}

bool EditorAssetSource::ReadFile(const std::string &name, std::vector<uint8_t> &out) const
{
    const std::string normalized = NormalizeAssetName(name);

    if (ReadFromDirectories(normalized, out) || ReadFromZips(normalized, out) ||
        ReadFromPaks(normalized, out))
    {
        return true;
    }

    out.clear();
    return false;
}

namespace
{

std::string LowerString(const std::string &text)
{
    std::string lowered = text;

    for (char &character : lowered)
    {
        character = LowerAscii(character);
    }

    return lowered;
}

std::string ExtensionOf(const std::string &path)
{
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of('/');

    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    {
        return {};
    }

    return LowerString(path.substr(dot));
}

bool HasImageExtension(const std::string &path)
{
    const std::string ext = ExtensionOf(path);
    return ext == ".png" || ext == ".tga" || ext == ".jpg" || ext == ".jpeg";
}

bool HasMapExtension(const std::string &path)
{
    const std::string ext = ExtensionOf(path);
    return ext == ".bsp" || ext == ".obj";
}

bool HasModelExtension(const std::string &path)
{
    return ExtensionOf(path) == ".mdl";
}

std::string TreeDirOf(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string MapArchiveSourceLabel(const std::string &path)
{
    const std::filesystem::path normalized = std::filesystem::path(path).lexically_normal();
    const std::string name = normalized.filename().string();
    const std::string parent = normalized.parent_path().filename().string();

    if (!parent.empty() && parent != "." && !name.empty())
    {
        return parent + "/" + name;
    }

    return !name.empty() ? name : path;
}

std::string CatalogSourceLabel(const std::string &path)
{
    const std::filesystem::path normalized = std::filesystem::path(path).lexically_normal();
    std::string parent = normalized.parent_path().filename().string();

    if (!parent.empty())
    {
        return parent;
    }

    std::string name = normalized.filename().string();
    return !name.empty() ? name : path;
}

template <typename Predicate>
void AppendMatchingCatalogEntry(const std::string &rawPath, const std::string &source,
                                Predicate &&predicate, std::unordered_set<std::string> &seen,
                                std::vector<EditorAssetCatalogEntry> &out)
{
    std::string normalized = NormalizeAssetName(rawPath);

    while (!normalized.empty() && normalized.front() == '/')
    {
        normalized.erase(normalized.begin());
    }

    if (normalized.empty() || !predicate(normalized))
    {
        return;
    }

    const std::string key = LowerString(normalized);

    if (!seen.insert(key).second)
    {
        return;
    }

    out.push_back(EditorAssetCatalogEntry{ normalized, source, TreeDirOf(normalized) });
}

void AppendCatalogEntry(const std::string &rawPath, const std::string &source,
                        std::unordered_set<std::string> &seen,
                        std::vector<EditorAssetCatalogEntry> &out)
{
    AppendMatchingCatalogEntry(rawPath, source, HasImageExtension, seen, out);
}

void AppendMapCatalogEntry(const std::string &rawPath, const std::string &source,
                           std::unordered_set<std::string> &seen,
                           std::vector<EditorAssetCatalogEntry> &out)
{
    AppendMatchingCatalogEntry(rawPath, source, HasMapExtension, seen, out);
}

void AppendModelCatalogEntry(const std::string &rawPath, const std::string &source,
                             std::unordered_set<std::string> &seen,
                             std::vector<EditorAssetCatalogEntry> &out)
{
    AppendMatchingCatalogEntry(rawPath, source, HasModelExtension, seen, out);
}

void ScanDirectorySubtree(const std::string &directory, std::unordered_set<std::string> &seen,
                          std::vector<EditorAssetCatalogEntry> &out)
{
    static constexpr const char *kAssetFolders[] = {
        "textures", "progs", "gfx", "vault", "maps", "env", "models", "mat", "mdl_skins",
    };

    const std::filesystem::path root(directory);
    std::error_code ec;

    if (!std::filesystem::is_directory(root, ec))
    {
        return;
    }

    const std::string source = root.lexically_normal().filename().string();
    const std::string sourceLabel = source.empty() ? directory : source;

    for (const char *folder : kAssetFolders)
    {
        const std::filesystem::path sub = root / folder;

        if (!std::filesystem::is_directory(sub, ec))
        {
            continue;
        }

        for (const std::filesystem::directory_entry &entry :
             std::filesystem::recursive_directory_iterator(sub, ec))
        {
            if (!entry.is_regular_file(ec))
            {
                continue;
            }

            const std::filesystem::path rel = std::filesystem::relative(entry.path(), root, ec);

            if (!ec)
            {
                AppendCatalogEntry(rel.generic_string(), sourceLabel, seen, out);
            }
        }
    }
}

void ScanDirectoryMaps(const std::string &directory, std::unordered_set<std::string> &seen,
                       std::vector<EditorAssetCatalogEntry> &out)
{
    const std::filesystem::path root(directory);
    std::error_code ec;

    if (!std::filesystem::is_directory(root, ec))
    {
        return;
    }

    const std::string source = root.lexically_normal().filename().string();
    const std::string sourceLabel = source.empty() ? directory : source;

    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(root, ec))
    {
        if (!entry.is_regular_file(ec))
        {
            continue;
        }

        const std::filesystem::path rel = std::filesystem::relative(entry.path(), root, ec);

        if (!ec)
        {
            AppendMapCatalogEntry(rel.generic_string(), sourceLabel, seen, out);
        }
    }

    const std::filesystem::path mapsSub = root / "maps";

    if (!std::filesystem::is_directory(mapsSub, ec))
    {
        return;
    }

    for (const std::filesystem::directory_entry &entry :
         std::filesystem::recursive_directory_iterator(mapsSub, ec))
    {
        if (!entry.is_regular_file(ec))
        {
            continue;
        }

        const std::filesystem::path rel = std::filesystem::relative(entry.path(), root, ec);

        if (!ec)
        {
            AppendMapCatalogEntry(rel.generic_string(), sourceLabel, seen, out);
        }
    }
}

void ScanDirectoryModels(const std::string &directory, std::unordered_set<std::string> &seen,
                         std::vector<EditorAssetCatalogEntry> &out)
{
    static constexpr const char *kModelFolders[] = { "progs", "models" };

    const std::filesystem::path root(directory);
    std::error_code ec;

    if (!std::filesystem::is_directory(root, ec))
    {
        return;
    }

    const std::string source = root.lexically_normal().filename().string();
    const std::string sourceLabel = source.empty() ? directory : source;

    for (const char *folder : kModelFolders)
    {
        const std::filesystem::path sub = root / folder;

        if (!std::filesystem::is_directory(sub, ec))
        {
            continue;
        }

        for (const std::filesystem::directory_entry &entry :
             std::filesystem::recursive_directory_iterator(sub, ec))
        {
            if (!entry.is_regular_file(ec))
            {
                continue;
            }

            const std::filesystem::path rel = std::filesystem::relative(entry.path(), root, ec);

            if (!ec)
            {
                AppendModelCatalogEntry(rel.generic_string(), sourceLabel, seen, out);
            }
        }
    }
}

} // namespace

void EditorAssetSource::ListTextureAssets(std::vector<EditorAssetCatalogEntry> &out) const
{
    out.clear();
    std::unordered_set<std::string> seen;

    for (const std::string &directory : m_directories)
    {
        ScanDirectorySubtree(directory, seen, out);
    }

    for (const ZipArchive &zip : m_zips)
    {
        const std::string source = CatalogSourceLabel(zip.path);

        for (const ZipEntry &entry : zip.entries)
        {
            AppendCatalogEntry(entry.name, source, seen, out);
        }
    }

    for (const Pak &pak : m_paks)
    {
        const std::string source = CatalogSourceLabel(pak.path);

        for (const PakEntry &entry : pak.entries)
        {
            AppendCatalogEntry(entry.name, source, seen, out);
        }
    }

    std::sort(out.begin(), out.end(),
              [](const EditorAssetCatalogEntry &a, const EditorAssetCatalogEntry &b) {
                  return LowerString(a.path) < LowerString(b.path);
              });
}

void EditorAssetSource::ListMapAssets(std::vector<EditorAssetCatalogEntry> &out) const
{
    out.clear();
    std::unordered_set<std::string> seen;

    for (const std::string &directory : m_directories)
    {
        ScanDirectoryMaps(directory, seen, out);
    }

    for (const ZipArchive &zip : m_zips)
    {
        const std::string source = MapArchiveSourceLabel(zip.path);

        for (const ZipEntry &entry : zip.entries)
        {
            AppendMapCatalogEntry(entry.name, source, seen, out);
        }
    }

    for (const Pak &pak : m_paks)
    {
        const std::string source = MapArchiveSourceLabel(pak.path);

        for (const PakEntry &entry : pak.entries)
        {
            AppendMapCatalogEntry(entry.name, source, seen, out);
        }
    }

    std::sort(out.begin(), out.end(),
              [](const EditorAssetCatalogEntry &a, const EditorAssetCatalogEntry &b) {
                  return LowerString(a.path) < LowerString(b.path);
              });
}

void EditorAssetSource::ListModelAssets(std::vector<EditorAssetCatalogEntry> &out) const
{
    out.clear();
    std::unordered_set<std::string> seen;

    for (const std::string &directory : m_directories)
    {
        ScanDirectoryModels(directory, seen, out);
    }

    for (const ZipArchive &zip : m_zips)
    {
        const std::string source = CatalogSourceLabel(zip.path);

        for (const ZipEntry &entry : zip.entries)
        {
            AppendModelCatalogEntry(entry.name, source, seen, out);
        }
    }

    for (const Pak &pak : m_paks)
    {
        const std::string source = CatalogSourceLabel(pak.path);

        for (const PakEntry &entry : pak.entries)
        {
            AppendModelCatalogEntry(entry.name, source, seen, out);
        }
    }

    std::sort(out.begin(), out.end(),
              [](const EditorAssetCatalogEntry &a, const EditorAssetCatalogEntry &b) {
                  return LowerString(a.path) < LowerString(b.path);
              });
}

bool DecodeEditorImageRgba(const std::vector<uint8_t> &bytes, uint32_t &outWidth,
                           uint32_t &outHeight, std::vector<uint8_t> &outRgba)
{
    if (bytes.empty() || bytes.size() > size_t(INT_MAX))
    {
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *pixels =
        stbi_load_from_memory(bytes.data(), int(bytes.size()), &width, &height, &channels, 4);

    if (pixels == nullptr || width <= 0 || height <= 0)
    {
        if (pixels != nullptr)
        {
            stbi_image_free(pixels);
        }

        return false;
    }

    outWidth = uint32_t(width);
    outHeight = uint32_t(height);
    outRgba.assign(pixels, pixels + size_t(width) * size_t(height) * 4);
    stbi_image_free(pixels);
    return true;
}
