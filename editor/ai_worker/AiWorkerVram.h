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

#include <ggml-backend.h>
#include <ggml.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct WorkerVulkanDevice
{
    int index = -1;
    std::string description;
    uint64_t totalVramBytes = 0;
    uint64_t freeVramBytes = 0;
};

struct WorkerVramBudget
{
    uint64_t totalVramBytes = 0;
    uint64_t freeVramBytes = 0;
    uint64_t maxAllowedBytes = 0;     // capped fraction (default 75%) of totalVramBytes
    uint64_t reserveBytes = 0;        // safety reserve kept free for editor & OS
    uint64_t effectiveBudgetBytes = 0; // min(maxAllowedBytes, freeVramBytes - reserveBytes)
    bool hasVulkan = false;
};

using WorkerProgressCallback = std::function<void(int32_t percent, const std::string &stage)>;

struct WorkerRunOptions
{
    double vramCapFraction = 0.75;
    bool forceCpu = false;
    bool clampEdge = false;
    WorkerProgressCallback onProgress;
};

struct WorkerRunResult
{
    EditorAiTensorNchw output;
    std::string provider;
    std::string error;
};

class WorkerBackendHandle
{
  public:
    WorkerBackendHandle() = default;
    ~WorkerBackendHandle();

    WorkerBackendHandle(const WorkerBackendHandle &) = delete;
    WorkerBackendHandle &operator=(const WorkerBackendHandle &) = delete;

    bool InitVulkan(int deviceIndex);
    bool InitCpu(int threadCount = 0);
    void Reset();

    ggml_backend_t Get() const
    {
        return m_backend;
    }
    ggml_backend_buffer_type_t BufferType() const;
    bool IsVulkan() const
    {
        return m_isVulkan;
    }
    int DeviceIndex() const
    {
        return m_deviceIndex;
    }
    const std::string &Label() const
    {
        return m_label;
    }

  private:
    ggml_backend_t m_backend = nullptr;
    bool m_isVulkan = false;
    int m_deviceIndex = -1;
    std::string m_label;
};

WorkerVulkanDevice SelectBestWorkerVulkanDevice();

ggml_backend_buffer_type_t WorkerVulkanBufferType(int deviceIndex);

WorkerVramBudget ComputeWorkerVramBudget(int deviceIndex, double maxFraction = 0.75);

using GraphMeasureBuildFn = std::function<ggml_cgraph *(ggml_context *ctx)>;

bool MeasureGraphComputeBytes(ggml_backend_buffer_type_t buft, const GraphMeasureBuildFn &buildFn,
                              size_t &outBytes);
