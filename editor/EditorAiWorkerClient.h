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

#include "EditorAiWorkerProtocol.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

struct EditorAiVramBudget
{
    uint64_t totalVramBytes = 0;
    uint64_t freeVramBytes = 0;
    uint64_t maxAllowedBytes = 0;
    uint64_t reserveBytes = 0;
    uint64_t effectiveBudgetBytes = 0;
};

struct EditorAiWorkerInvokeRequest
{
    std::filesystem::path workerExecutable;
    std::filesystem::path modelPath;
    std::string arch;
    bool clampEdge = false;
    EditorAiTensorNchw input;
    const std::atomic<bool> *cancelRequested = nullptr;
    std::function<void(int32_t percent, const std::string &stage)> onProgress;
};

struct EditorAiWorkerInvokeResult
{
    bool ok = false;
    bool cancelled = false;
    EditorAiTensorNchw output;
    std::string provider;
    std::string errorMessage;
    uint64_t totalVramBytes = 0;
    uint64_t freeVramBytes = 0;
    uint64_t budgetVramBytes = 0;
};

// Pure budget helper matching worker enforcement: min(maxFraction * total, free - safetyReserve).
EditorAiVramBudget ComputeEditorAiVramBudget(uint64_t totalVramBytes, uint64_t freeVramBytes,
                                             double maxFraction = 0.75);

std::filesystem::path FindEditorAiWorkerExecutable(const std::filesystem::path &executableDir);

bool IsEditorAiWorkerAvailable(const std::filesystem::path &executableDir);

void CancelActiveEditorAiWorker();

bool RunEditorAiWorkerProcess(const EditorAiWorkerInvokeRequest &request,
                              EditorAiWorkerInvokeResult &outResult);
