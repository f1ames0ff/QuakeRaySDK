#include "Assets.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

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

uint32_t ReadUint32(const uint8_t *bytes)
{
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) |
           (uint32_t(bytes[3]) << 24);
}

bool PathsMatch(const std::string &left, const std::string &right)
{
#if defined(_WIN32)
    return EqualsIgnoreCase(left, right);
#else
    return left == right;
#endif
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

bool EditorAssetSource::AddPak(const std::string &path)
{
    if (path.empty())
    {
        return false;
    }

    for (const std::string &existing : m_archives)
    {
        if (PathsMatch(existing, path))
        {
            return false;
        }
    }

    std::vector<uint8_t> bytes;

    if (!ReadWholeFile(path, bytes) || bytes.size() < PakHeaderSize)
    {
        return false;
    }

    if (std::memcmp(bytes.data(), "PACK", 4) != 0)
    {
        return false;
    }

    const uint32_t directoryOffset = ReadUint32(bytes.data() + 4);
    const uint32_t directoryLength = ReadUint32(bytes.data() + 8);

    if (directoryLength % PakEntrySize != 0 || directoryOffset > bytes.size() ||
        directoryLength > bytes.size() - directoryOffset)
    {
        return false;
    }

    Pak pak;
    pak.path = path;

    const uint32_t entryCount = directoryLength / PakEntrySize;

    for (uint32_t i = 0; i < entryCount; i++)
    {
        const uint8_t *entry = bytes.data() + directoryOffset + size_t(i) * PakEntrySize;

        PakEntry pakEntry;
        const char *nameEnd = static_cast<const char *>(std::memchr(entry, '\0', 56));
        pakEntry.name = std::string(static_cast<const char *>(static_cast<const void *>(entry)),
                                    nameEnd ? size_t(nameEnd - reinterpret_cast<const char *>(entry)) : 56);
        pakEntry.offset = ReadUint32(entry + 56);
        pakEntry.size = ReadUint32(entry + 60);

        if (pakEntry.name.empty() || pakEntry.offset > bytes.size() ||
            pakEntry.size > bytes.size() - pakEntry.offset)
        {
            continue;
        }

        pak.entries.push_back(std::move(pakEntry));
    }

    m_archives.push_back(path);
    m_paks.push_back(std::move(pak));
    return true;
}

bool EditorAssetSource::AddArchiveOrDirectory(const std::string &path)
{
    std::error_code ec;

    if (std::filesystem::is_directory(path, ec))
    {
        return AddDirectory(path);
    }

    return AddPak(path);
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

bool EditorAssetSource::Exists(const std::string &name) const
{
    std::vector<uint8_t> bytes;
    return ReadFile(name, bytes);
}

bool EditorAssetSource::ReadFile(const std::string &name, std::vector<uint8_t> &out) const
{
    const std::string normalized = NormalizeAssetName(name);

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

    out.clear();
    return false;
}
