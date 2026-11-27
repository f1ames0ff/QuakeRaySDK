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

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct EditorAiTensorNchw
{
    uint32_t channels = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> data;
};

enum class EditorAiWorkerMsgKind
{
    Unknown = 0,
    Progress,
    Vram,
    Provider,
    Done,
    Error,
};

struct EditorAiWorkerMessage
{
    EditorAiWorkerMsgKind kind = EditorAiWorkerMsgKind::Unknown;
    int32_t percent = 0;
    uint64_t totalVramBytes = 0;
    uint64_t freeVramBytes = 0;
    uint64_t budgetVramBytes = 0;
    std::string text;
};

bool WriteAiBinaryTensorFile(const std::filesystem::path &path, const EditorAiTensorNchw &tensor,
                             std::string &outError);

bool ReadAiBinaryTensorFile(const std::filesystem::path &path, EditorAiTensorNchw &outTensor,
                            std::string &outError);

EditorAiWorkerMessage ParseAiWorkerStdoutLine(const std::string &rawLine);
