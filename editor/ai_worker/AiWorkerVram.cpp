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

#include "AiWorkerVram.h"

#include <ggml-alloc.h>
#include <ggml-cpu.h>
#include <ggml-vulkan.h>

#include <algorithm>
#include <thread>
#include <vector>

namespace
{

constexpr uint64_t kMinReserveBytes = 512ULL << 20;
constexpr uint64_t kMaxReserveBytes = 2048ULL << 20;
constexpr size_t kMeasureGraphNodes = 65536u;
constexpr size_t kMeasureGraphCap = 16384u;

} // namespace

WorkerBackendHandle::~WorkerBackendHandle()
{
    Reset();
}

void WorkerBackendHandle::Reset()
{
    if (m_backend != nullptr)
    {
        ggml_backend_free(m_backend);
        m_backend = nullptr;
    }
    m_isVulkan = false;
    m_deviceIndex = -1;
    m_label.clear();
}

bool WorkerBackendHandle::InitVulkan(int deviceIndex)
{
    Reset();
    if (deviceIndex < 0 || deviceIndex >= ggml_backend_vk_get_device_count())
    {
        return false;
    }
    ggml_backend_t vk = ggml_backend_vk_init(static_cast<size_t>(deviceIndex));
    if (vk == nullptr)
    {
        return false;
    }
    char desc[256] = {};
    ggml_backend_vk_get_device_description(deviceIndex, desc, sizeof(desc));
    m_backend = vk;
    m_isVulkan = true;
    m_deviceIndex = deviceIndex;
    m_label = desc[0] != '\0' ? std::string("ggml-vulkan (") + desc + ")" : "ggml-vulkan";
    return true;
}

bool WorkerBackendHandle::InitCpu(int threadCount)
{
    Reset();
    ggml_backend_t cpu = ggml_backend_cpu_init();
    if (cpu == nullptr)
    {
        return false;
    }
    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    const int threads = threadCount > 0 ? threadCount : std::clamp(hw > 0 ? hw / 2 : 4, 2, 16);
    ggml_backend_cpu_set_n_threads(cpu, threads);
    m_backend = cpu;
    m_isVulkan = false;
    m_deviceIndex = -1;
    m_label = "ggml-cpu";
    return true;
}

ggml_backend_buffer_type_t WorkerBackendHandle::BufferType() const
{
    if (m_backend == nullptr)
    {
        return nullptr;
    }
    return ggml_backend_get_default_buffer_type(m_backend);
}

WorkerVulkanDevice SelectBestWorkerVulkanDevice()
{
    WorkerVulkanDevice best;
    const int count = ggml_backend_vk_get_device_count();
    for (int i = 0; i < count; i++)
    {
        size_t freeMem = 0;
        size_t totalMem = 0;
        ggml_backend_vk_get_device_memory(i, &freeMem, &totalMem);
        if (static_cast<uint64_t>(totalMem) > best.totalVramBytes)
        {
            char desc[256] = {};
            ggml_backend_vk_get_device_description(i, desc, sizeof(desc));
            best.index = i;
            best.description = desc;
            best.totalVramBytes = static_cast<uint64_t>(totalMem);
            best.freeVramBytes = static_cast<uint64_t>(freeMem);
        }
    }
    return best;
}

ggml_backend_buffer_type_t WorkerVulkanBufferType(int deviceIndex)
{
    if (deviceIndex < 0 || deviceIndex >= ggml_backend_vk_get_device_count())
    {
        return nullptr;
    }
    return ggml_backend_vk_buffer_type(static_cast<size_t>(deviceIndex));
}

WorkerVramBudget ComputeWorkerVramBudget(int deviceIndex, double maxFraction)
{
    WorkerVramBudget budget;
    if (deviceIndex < 0 || deviceIndex >= ggml_backend_vk_get_device_count())
    {
        return budget;
    }

    size_t freeMem = 0;
    size_t totalMem = 0;
    ggml_backend_vk_get_device_memory(deviceIndex, &freeMem, &totalMem);
    if (totalMem == 0)
    {
        return budget;
    }

    const uint64_t total = static_cast<uint64_t>(totalMem);
    const uint64_t freeBytes = static_cast<uint64_t>(freeMem);
    const uint64_t reserve =
        std::clamp(total / uint64_t(10u), kMinReserveBytes, kMaxReserveBytes);
    const uint64_t max75 = static_cast<uint64_t>(static_cast<double>(total) * maxFraction);
    const uint64_t headroom = freeBytes > reserve ? (freeBytes - reserve) : 0ULL;

    budget.totalVramBytes = total;
    budget.freeVramBytes = freeBytes;
    budget.maxAllowedBytes = max75;
    budget.reserveBytes = reserve;
    budget.effectiveBudgetBytes = std::min(max75, headroom);
    budget.hasVulkan = true;
    return budget;
}

bool MeasureGraphComputeBytes(ggml_backend_buffer_type_t buft, const GraphMeasureBuildFn &buildFn,
                              size_t &outBytes)
{
    outBytes = 0;
    if (buft == nullptr || !buildFn)
    {
        return false;
    }

    const size_t ctxBytes =
        ggml_tensor_overhead() * kMeasureGraphNodes + ggml_graph_overhead_custom(kMeasureGraphCap, false);
    ggml_init_params params = { ctxBytes, nullptr, true };
    ggml_context *ctx = ggml_init(params);
    if (ctx == nullptr)
    {
        return false;
    }

    ggml_cgraph *graph = buildFn(ctx);
    if (graph == nullptr)
    {
        ggml_free(ctx);
        return false;
    }

    ggml_gallocr_t galloc = ggml_gallocr_new(buft);
    if (galloc == nullptr)
    {
        ggml_free(ctx);
        return false;
    }

    std::vector<size_t> sizes(1u, 0u);
    ggml_gallocr_reserve_n_size(galloc, graph, nullptr, nullptr, sizes.data());
    outBytes = sizes[0];

    ggml_gallocr_free(galloc);
    ggml_free(ctx);
    return true;
}
