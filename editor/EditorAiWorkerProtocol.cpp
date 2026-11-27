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

#include "EditorAiWorkerProtocol.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>

namespace
{

constexpr std::array<char, 4> kQraiMagic = { 'Q', 'R', 'A', 'I' };
constexpr uint32_t kQraiVersion = 1u;

struct QraiDiskHeader
{
    char magic[4];
    uint32_t version;
    uint32_t channels;
    uint32_t width;
    uint32_t height;
};
static_assert(sizeof(QraiDiskHeader) == 20u, "Unexpected padding in QraiDiskHeader");

std::string TrimLine(std::string_view sv)
{
    size_t start = 0;
    while (start < sv.size() && std::isspace(static_cast<unsigned char>(sv[start])) != 0)
    {
        start++;
    }
    size_t end = sv.size();
    while (end > start && std::isspace(static_cast<unsigned char>(sv[end - 1])) != 0)
    {
        end--;
    }
    return std::string(sv.substr(start, end - start));
}

bool ParseUint64Token(std::string_view token, uint64_t &outVal)
{
    if (token.empty())
    {
        return false;
    }
    const std::string s(token);
    char *endPtr = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &endPtr, 10);
    if (endPtr == nullptr || *endPtr != '\0')
    {
        return false;
    }
    outVal = static_cast<uint64_t>(v);
    return true;
}

bool ParseVramLine(std::string_view rest, EditorAiWorkerMessage &msg)
{
    size_t pos = 0;
    while (pos < rest.size())
    {
        while (pos < rest.size() && rest[pos] == ' ')
        {
            pos++;
        }
        const size_t next = rest.find(' ', pos);
        const std::string_view tok =
            next == std::string_view::npos ? rest.substr(pos) : rest.substr(pos, next - pos);
        pos = next == std::string_view::npos ? rest.size() : next + 1;
        const size_t eq = tok.find('=');
        if (eq == std::string_view::npos)
        {
            continue;
        }
        const std::string_view key = tok.substr(0, eq);
        uint64_t val = 0;
        if (!ParseUint64Token(tok.substr(eq + 1), val))
        {
            continue;
        }
        if (key == "total")
        {
            msg.totalVramBytes = val;
        }
        else if (key == "free")
        {
            msg.freeVramBytes = val;
        }
        else if (key == "budget")
        {
            msg.budgetVramBytes = val;
        }
    }
    return msg.totalVramBytes > 0u;
}

} // namespace

bool WriteAiBinaryTensorFile(const std::filesystem::path &path, const EditorAiTensorNchw &tensor,
                             std::string &outError)
{
    outError.clear();
    if (tensor.channels == 0u || tensor.width == 0u || tensor.height == 0u)
    {
        outError = "Invalid zero dimension in binary tensor.";
        return false;
    }
    const size_t count = static_cast<size_t>(tensor.channels) * static_cast<size_t>(tensor.width) *
                         static_cast<size_t>(tensor.height);
    if (tensor.data.size() != count)
    {
        outError = "Binary tensor element count mismatch.";
        return false;
    }

    std::error_code ec;
    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    QraiDiskHeader hdr = {};
    std::memcpy(hdr.magic, kQraiMagic.data(), 4u);
    hdr.version = kQraiVersion;
    hdr.channels = tensor.channels;
    hdr.width = tensor.width;
    hdr.height = tensor.height;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        outError = "Failed opening binary tensor file for writing: " + path.string();
        return false;
    }
    out.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char *>(tensor.data.data()),
              static_cast<std::streamsize>(count * sizeof(float)));
    if (!out.good())
    {
        outError = "Failed writing binary tensor file: " + path.string();
        return false;
    }
    return true;
}

bool ReadAiBinaryTensorFile(const std::filesystem::path &path, EditorAiTensorNchw &outTensor,
                            std::string &outError)
{
    outTensor = {};
    outError.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        outError = "Failed opening binary tensor file for reading: " + path.string();
        return false;
    }

    QraiDiskHeader hdr = {};
    in.read(reinterpret_cast<char *>(&hdr), sizeof(hdr));
    if (!in.good() || std::memcmp(hdr.magic, kQraiMagic.data(), 4u) != 0 ||
        hdr.version != kQraiVersion || hdr.channels == 0u || hdr.width == 0u || hdr.height == 0u)
    {
        outError = "Invalid binary tensor file header: " + path.string();
        return false;
    }

    const size_t count = static_cast<size_t>(hdr.channels) * static_cast<size_t>(hdr.width) *
                         static_cast<size_t>(hdr.height);
    if (count > (256ULL * 1024ULL * 1024ULL))
    {
        outError = "Binary tensor exceeds maximum supported size.";
        return false;
    }

    outTensor.channels = hdr.channels;
    outTensor.width = hdr.width;
    outTensor.height = hdr.height;
    outTensor.data.resize(count);
    in.read(reinterpret_cast<char *>(outTensor.data.data()),
            static_cast<std::streamsize>(count * sizeof(float)));
    if (!in.good())
    {
        outTensor = {};
        outError = "Truncated binary tensor payload: " + path.string();
        return false;
    }
    return true;
}

EditorAiWorkerMessage ParseAiWorkerStdoutLine(const std::string &rawLine)
{
    EditorAiWorkerMessage msg;
    const std::string line = TrimLine(rawLine);
    const std::string_view sv(line);
    if (sv.empty())
    {
        return msg;
    }

    if (sv == "DONE")
    {
        msg.kind = EditorAiWorkerMsgKind::Done;
        return msg;
    }
    if (sv.rfind("PROVIDER ", 0) == 0)
    {
        msg.kind = EditorAiWorkerMsgKind::Provider;
        msg.text = TrimLine(sv.substr(9));
        return msg;
    }
    if (sv.rfind("ERROR ", 0) == 0)
    {
        msg.kind = EditorAiWorkerMsgKind::Error;
        msg.text = TrimLine(sv.substr(6));
        return msg;
    }
    if (sv.rfind("VRAM ", 0) == 0)
    {
        if (ParseVramLine(sv.substr(5), msg))
        {
            msg.kind = EditorAiWorkerMsgKind::Vram;
        }
        return msg;
    }
    if (sv.rfind("PROGRESS ", 0) == 0)
    {
        const std::string_view rest = sv.substr(9);
        const size_t sp = rest.find(' ');
        const std::string_view pctTok = sp == std::string_view::npos ? rest : rest.substr(0, sp);
        uint64_t pct = 0;
        if (!ParseUint64Token(pctTok, pct))
        {
            return msg;
        }
        msg.kind = EditorAiWorkerMsgKind::Progress;
        msg.percent = std::clamp(static_cast<int32_t>(pct), 0, 100);
        if (sp != std::string_view::npos)
        {
            msg.text = TrimLine(rest.substr(sp + 1));
        }
    }
    return msg;
}
