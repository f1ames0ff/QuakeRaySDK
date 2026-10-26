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

#include "VulkanTestContext.h"
#include "RHI/RhiPipeline.h"
#include "Generated/ShaderCommonC.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{

struct PerfCase
{
    const char *name;
    uint32_t threads;
    uint32_t lights;
    uint32_t samplesPerThread;
};

struct CaseResult
{
    std::string name;
    double milliseconds;
    double normalized;
};

struct BaselineEntry
{
    bool hasNormalized = false;
    double normalized = 0.0;
    double absolute = 0.0;
};

const PerfCase kCases[] =
{
    { "small",  16384,   256,   256 },
    { "medium", 262144,  4096,  128 },
    { "large",  524288,  16384, 256 },
    { "huge",   1048576, 65536, 256 },
};

// Measured in the same round-robin series as the cases and used only as a
// divisor: a clock or power state change moves the reference and the cases
// alike, so their ratio stays comparable across machines while the absolute
// milliseconds do not. It runs long enough to be dominated by the same shader
// work as the cases rather than by the dispatch overhead a short probe measures.
const PerfCase kReferenceCase = { "reference", 262144, 4096, 256 };

constexpr int kCaseCount = int(sizeof(kCases) / sizeof(kCases[0]));

// This class of GPU switches between clock/power states several times per
// second, and a case timed on its own lands in whichever state happens to be
// active - a 2x spread that no divisor taken minutes earlier can cancel. The
// cases and the reference therefore run round-robin inside one series, so every
// sample draws from the same distribution of states and the lowest round of a
// case comes from the same state as the lowest reference round.
constexpr int kTimedRounds = 8;

// The first series only settles the pipeline and the caches; the rounds of the
// following ones are what the gate reads. A round is cheap enough that several
// series cost a fraction of the warmup while giving the minimum - the estimate
// contention and clock transitions can only inflate - many more chances to be
// sampled in the fastest state the hardware actually reaches.
constexpr int kSeriesCount = 3;
static_assert(kSeriesCount > 1, "one series is discarded as a warmup");

// Warmup is time based rather than a pass count: it has to hold the GPU in its
// sustained state long enough for the driver to stop ramping clocks.
constexpr double kWarmupSeconds = 0.25;

qray::ShLightEncoded MakeLight()
{
    qray::ShLightEncoded light = {};
    light.lightType = LIGHT_TYPE_TEXTURED_AREA;

    for (int i = 0; i < 3; i++)
    {
        light.color[i] = 1.0f;
        light.data_7[i] = (i == 2) ? 1.0f : 0.0f;
    }

    light.data_0[0] = 0.0f; light.data_0[1] = 0.0f; light.data_0[2] = 1.0f; light.data_0[3] = 0.0f;
    light.data_1[0] = 1.0f; light.data_1[1] = 0.0f; light.data_1[2] = 1.0f; light.data_1[3] = 1.0f;
    light.data_2[0] = 1.0f; light.data_2[1] = 1.0f; light.data_2[2] = 1.0f; light.data_2[3] = 8.0f;

    float *const uvSlots[4] = { light.data_3, light.data_4, light.data_5, light.data_6 };

    for (int i = 0; i < 8; i++)
    {
        const float angle = float(i) / 8.0f * 6.28318530718f;
        float *slot = uvSlots[i >> 1] + (i & 1) * 2;
        slot[0] = 0.5f + 0.4f * std::cos(angle);
        slot[1] = 0.5f + 0.4f * std::sin(angle);
    }

    light.data_7[3] = 1.0f;
    return light;
}

struct CaseRunner
{
    void Prepare(VulkanTestContext &gpu, nvrhi::IShader *shader, const PerfCase &perfCase);
    double Dispatch();
    float ReadChecksum();

private:
    nvrhi::IDevice *m_device = nullptr;
    nvrhi::ComputeState m_state;
    nvrhi::BindingLayoutHandle m_uniformLayout;
    nvrhi::BindingLayoutHandle m_lightLayout;
    nvrhi::BindingLayoutHandle m_probeLayout;
    nvrhi::BindingSetHandle m_uniformSet;
    nvrhi::BindingSetHandle m_lightSet;
    nvrhi::BindingSetHandle m_probeSet;
    nvrhi::ComputePipelineHandle m_pipeline;
    nvrhi::BufferHandle m_uniformBuffer;
    nvrhi::BufferHandle m_paramsBuffer;
    nvrhi::BufferHandle m_lightsBuffer;
    nvrhi::BufferHandle m_outputBuffer;
    nvrhi::BufferHandle m_readbackBuffer;
    std::vector<nvrhi::BufferHandle> m_lightDummies;
    nvrhi::BufferHandle m_lightStatsDummy;
    uint32_t m_groups = 0;
};

void CaseRunner::Prepare(VulkanTestContext &gpu, nvrhi::IShader *shader, const PerfCase &perfCase)
{
    nvrhi::IDevice *device = gpu.context.GetDevice();

    const uint32_t stride = uint32_t(sizeof(qray::ShLightEncoded));
    const std::vector<qray::ShLightEncoded> lights(perfCase.lights, MakeLight());
    const std::vector<uint8_t> zeros(4096);
    const uint32_t params[4] = { perfCase.lights, perfCase.samplesPerThread, 1u, 0u };

    nvrhi::BufferDesc desc;
    desc.byteSize = 4096;
    desc.isConstantBuffer = true;
    desc.initialState = nvrhi::ResourceStates::CopyDest;
    desc.keepInitialState = true;
    auto uniformBuffer = device->createBuffer(desc);

    desc = {};
    desc.byteSize = 16;
    desc.isConstantBuffer = true;
    desc.initialState = nvrhi::ResourceStates::CopyDest;
    desc.keepInitialState = true;
    auto paramsBuffer = device->createBuffer(desc);

    desc = {};
    desc.byteSize = uint64_t(perfCase.lights) * stride;
    desc.structStride = stride;
    desc.initialState = nvrhi::ResourceStates::CopyDest;
    desc.keepInitialState = true;
    auto lightsBuffer = device->createBuffer(desc);

    desc = {};
    desc.byteSize = uint64_t(perfCase.threads) * sizeof(float);
    desc.structStride = sizeof(float);
    desc.canHaveUAVs = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    auto outputBuffer = device->createBuffer(desc);

    desc = {};
    desc.byteSize = sizeof(float);
    desc.cpuAccess = nvrhi::CpuAccessMode::Read;
    desc.initialState = nvrhi::ResourceStates::CopyDest;
    desc.keepInitialState = true;
    auto readbackBuffer = device->createBuffer(desc);

    Require(uniformBuffer != nullptr && paramsBuffer != nullptr && lightsBuffer != nullptr &&
            outputBuffer != nullptr && readbackBuffer != nullptr, "create perf buffers");

    nvrhi::BufferDesc dummyDesc;
    dummyDesc.byteSize = 64;
    dummyDesc.structStride = 4;
    dummyDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    dummyDesc.keepInitialState = true;

    std::vector<nvrhi::BufferHandle> lightDummies;

    for (int i = 0; i < 7; i++)
    {
        lightDummies.push_back(device->createBuffer(dummyDesc));
        Require(lightDummies.back() != nullptr, "create perf light dummy");
    }

    dummyDesc.canHaveUAVs = true;
    dummyDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    auto lightStatsDummy = device->createBuffer(dummyDesc);
    Require(lightStatsDummy != nullptr, "create perf light stats dummy");

    nvrhi::VulkanBindingOffsets zeroOffsets;
    zeroOffsets.shaderResource = 0;
    zeroOffsets.sampler = 0;
    zeroOffsets.constantBuffer = 0;
    zeroOffsets.unorderedAccess = 0;

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.setBindingOffsets(zeroOffsets);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
    auto uniformLayout = device->createBindingLayout(layoutDesc);
    auto uniformSet = device->createBindingSet(
        nvrhi::BindingSetDesc().addItem(nvrhi::BindingSetItem::ConstantBuffer(0, uniformBuffer)), uniformLayout);

    layoutDesc = {};
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.setBindingOffsets(zeroOffsets);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(6));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(8));
    auto lightLayout = device->createBindingLayout(layoutDesc);
    auto lightSet = device->createBindingSet(nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, lightsBuffer))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, lightDummies[0]))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, lightDummies[1]))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, lightDummies[2]))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, lightDummies[3]))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(5, lightDummies[4]))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(6, lightStatsDummy))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, lightDummies[5]))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(8, lightDummies[6])), lightLayout);

    layoutDesc = {};
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.setBindingOffsets(zeroOffsets);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));
    auto probeLayout = device->createBindingLayout(layoutDesc);
    auto probeSet = device->createBindingSet(nvrhi::BindingSetDesc()
        .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, paramsBuffer))
        .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, outputBuffer)), probeLayout);

    Require(uniformLayout != nullptr && uniformSet != nullptr && lightLayout != nullptr && lightSet != nullptr &&
            probeLayout != nullptr && probeSet != nullptr, "create perf bindings");

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = shader;
    pipelineDesc.bindingLayouts.push_back(uniformLayout.Get());
    pipelineDesc.bindingLayouts.push_back(lightLayout.Get());
    pipelineDesc.bindingLayouts.push_back(probeLayout.Get());
    auto pipeline = device->createComputePipeline(pipelineDesc);
    Require(pipeline != nullptr, "create perf pipeline");

    nvrhi::ComputeState state;
    state.pipeline = pipeline;
    state.bindings.push_back(uniformSet.Get());
    state.bindings.push_back(lightSet.Get());
    state.bindings.push_back(probeSet.Get());

    // ComputeState stores raw pointers, so the pipeline, its layouts and the
    // binding sets - and through them the buffers - have to outlive Prepare and
    // are owned here instead of by its locals.
    m_device = device;
    m_state = state;
    m_uniformLayout = uniformLayout;
    m_lightLayout = lightLayout;
    m_probeLayout = probeLayout;
    m_uniformSet = uniformSet;
    m_lightSet = lightSet;
    m_probeSet = probeSet;
    m_pipeline = pipeline;
    m_uniformBuffer = uniformBuffer;
    m_paramsBuffer = paramsBuffer;
    m_lightsBuffer = lightsBuffer;
    m_outputBuffer = outputBuffer;
    m_readbackBuffer = readbackBuffer;
    m_lightDummies = lightDummies;
    m_lightStatsDummy = lightStatsDummy;

    {
        auto setup = device->createCommandList();
        setup->open();
        setup->writeBuffer(uniformBuffer, zeros.data(), zeros.size());
        setup->writeBuffer(paramsBuffer, params, sizeof(params));
        setup->writeBuffer(lightsBuffer, lights.data(), lights.size() * stride);
        setup->close();
        device->executeCommandList(setup);
        device->waitForIdle();
    }

    m_groups = (perfCase.threads + 63) / 64;
}

double CaseRunner::Dispatch()
{
    auto timer = m_device->createTimerQuery();
    auto cmd = m_device->createCommandList();
    cmd->open();
    cmd->setComputeState(m_state);
    cmd->beginTimerQuery(timer);
    cmd->dispatch(m_groups, 1, 1);
    cmd->endTimerQuery(timer);
    cmd->copyBuffer(m_readbackBuffer, 0, m_outputBuffer, 0, sizeof(float));
    cmd->close();
    m_device->executeCommandList(cmd);
    m_device->waitForIdle();

    return m_device->getTimerQueryTime(timer) * 1000.0;
}

float CaseRunner::ReadChecksum()
{
    float checksum = 0.0f;
    const void *data = m_device->mapBuffer(m_readbackBuffer, nvrhi::CpuAccessMode::Read);
    Require(data != nullptr, "map perf readback");
    std::memcpy(&checksum, data, sizeof(float));
    m_device->unmapBuffer(m_readbackBuffer);
    return checksum;
}

// Runs the case for at least the requested time and returns its checksum, so the
// dispatches that only warm up the pipeline are still verified end to end.
float WarmUp(VulkanTestContext &gpu, nvrhi::IShader *shader, const PerfCase &perfCase, double seconds)
{
    CaseRunner runner;
    runner.Prepare(gpu, shader, perfCase);

    const auto start = std::chrono::steady_clock::now();
    do
    {
        runner.Dispatch();
    } while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds);

    return runner.ReadChecksum();
}

void WriteBaseline(const std::filesystem::path &path, const std::vector<CaseResult> &results)
{
    std::ofstream file(path);
    Require(file.is_open(), "open baseline for writing: " + path.string());
    file << "# dtal perf baseline: case ratio to the in-run reference and absolute milliseconds\n";
    for (const CaseResult &result : results)
    {
        file << result.name << ' ' << result.normalized << ' ' << result.milliseconds << '\n';
    }
}

std::map<std::string, BaselineEntry> ReadBaseline(const std::filesystem::path &path)
{
    std::ifstream file(path);
    Require(file.is_open(), "open baseline for reading: " + path.string());

    std::map<std::string, BaselineEntry> baseline;
    std::string line;

    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        std::istringstream stream(line);
        std::string name;
        double first = 0.0;
        double second = 0.0;

        if (!(stream >> name >> first))
        {
            continue;
        }

        BaselineEntry entry;

        if (stream >> second)
        {
            entry.hasNormalized = true;
            entry.normalized = first;
            entry.absolute = second;
        }
        else
        {
            // Two columns are the earlier absolute-only format; it still gates,
            // but on the machine state it was captured in.
            entry.absolute = first;
        }

        baseline[name] = entry;
    }

    return baseline;
}

} // namespace

int main(int argc, char **argv)
{
    try
    {
        std::string shaderPath = argc > 1 ? argv[1] : "DtalPerf.comp.spv";
        std::filesystem::path baselinePath = argc > 2 ? argv[2] : "DtalPerf.baseline.txt";
        bool update = false;
        double tolerance = 0.15;

        for (int i = 1; i < argc; i++)
        {
            const std::string argument = argv[i];
            if (argument == "--update")
            {
                update = true;
            }
            else if (argument == "--tolerance" && i + 1 < argc)
            {
                tolerance = std::stod(argv[++i]);
            }
        }

        VulkanTestContext gpu;
        nvrhi::IDevice *device = gpu.context.GetDevice();
        auto shader = qray::rhi::loadShader(device, shaderPath, nvrhi::ShaderType::Compute, "Dtal perf probe");
        Require(shader != nullptr, "load " + shaderPath);

        for (const PerfCase &perfCase : kCases)
        {
            Require(std::isfinite(WarmUp(gpu, shader, perfCase, kWarmupSeconds)), "non-finite warmup checksum");
        }
        Require(std::isfinite(WarmUp(gpu, shader, kReferenceCase, kWarmupSeconds)), "non-finite reference warmup checksum");

        CaseRunner runners[kCaseCount];
        for (int caseIndex = 0; caseIndex < kCaseCount; caseIndex++)
        {
            runners[caseIndex].Prepare(gpu, shader, kCases[caseIndex]);
        }

        CaseRunner referenceRunner;
        referenceRunner.Prepare(gpu, shader, kReferenceCase);

        std::vector<double> caseTimings[kCaseCount];
        std::vector<double> referenceTimings;

        for (int series = 0; series < kSeriesCount; series++)
        {
            for (int round = 0; round < kTimedRounds; round++)
            {
                for (int caseIndex = 0; caseIndex < kCaseCount; caseIndex++)
                {
                    const double milliseconds = runners[caseIndex].Dispatch();
                    if (series > 0)
                    {
                        caseTimings[caseIndex].push_back(milliseconds);
                    }
                }

                const double referenceMilliseconds = referenceRunner.Dispatch();
                if (series > 0)
                {
                    referenceTimings.push_back(referenceMilliseconds);
                }
            }
        }

        const double reference = *std::min_element(referenceTimings.begin(), referenceTimings.end());
        Require(reference > 0.0, "non-positive reference timing");

        std::vector<CaseResult> results;
        for (int caseIndex = 0; caseIndex < kCaseCount; caseIndex++)
        {
            const PerfCase &perfCase = kCases[caseIndex];
            const double milliseconds = *std::min_element(caseTimings[caseIndex].begin(), caseTimings[caseIndex].end());
            Require(std::isfinite(runners[caseIndex].ReadChecksum()), "non-finite perf checksum");

            const double normalized = milliseconds / reference;
            std::cout << "dtal " << perfCase.name << ": " << milliseconds << " ms, " << normalized
                      << " normalized (reference " << reference << " ms; " << perfCase.threads
                      << " threads, " << perfCase.lights << " lights, " << perfCase.samplesPerThread
                      << " samples/thread, " << double(perfCase.threads) * perfCase.samplesPerThread / 1e6
                      << "M samples)\n";
            results.push_back({ perfCase.name, milliseconds, normalized });
        }

        Require(gpu.errors == 0, "Vulkan/NVRHI validation errors=" + std::to_string(gpu.errors.load()));

        if (update || !std::filesystem::exists(baselinePath))
        {
            WriteBaseline(baselinePath, results);
            std::cout << "baseline written to " << baselinePath.string() << '\n';
            std::cout << "Dtal perf passed (baseline)\n";
            return 0;
        }

        const std::map<std::string, BaselineEntry> baseline = ReadBaseline(baselinePath);
        bool failed = false;

        for (const CaseResult &result : results)
        {
            const auto it = baseline.find(result.name);
            if (it == baseline.end())
            {
                std::cerr << "baseline has no case '" << result.name << "'\n";
                failed = true;
                continue;
            }

            const double measured = it->second.hasNormalized ? result.normalized : result.milliseconds;
            const double expected = it->second.hasNormalized ? it->second.normalized : it->second.absolute;
            const double ratio = measured / expected;
            const bool regression = ratio > 1.0 + tolerance;
            std::cout << "dtal " << result.name << ": " << result.milliseconds << " ms, "
                      << result.normalized << " normalized vs baseline " << expected
                      << (it->second.hasNormalized ? " normalized" : " ms (legacy absolute baseline)")
                      << " (" << (ratio * 100.0 - 100.0) << "%)" << (regression ? " REGRESSION" : "")
                      << '\n';

            if (regression)
            {
                failed = true;
            }
        }

        if (failed)
        {
            std::cerr << "Dtal perf failed; run with --update to accept the new numbers\n";
            return 1;
        }

        std::cout << "Dtal perf passed\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAILED: " << e.what() << '\n';
        return 1;
    }
}
