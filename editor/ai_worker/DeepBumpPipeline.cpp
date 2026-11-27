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

#include "DeepBumpPipeline.h"
#include "GgmlOps.h"

#include <ggml-alloc.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

namespace qray_ai
{
namespace
{

constexpr uint32_t kTileSize = 256u;
constexpr uint32_t kTileCut = 32u;
constexpr uint32_t kTileRamp = 32u;
constexpr uint32_t kTileHalo = kTileCut + kTileRamp;
constexpr uint32_t kTileMaxStep = kTileSize - 2u * kTileCut - kTileRamp;
constexpr size_t kDeepBumpGraphCap = 4096u;

size_t DeepBumpGraphCtxMem()
{
    return ggml_tensor_overhead() * kDeepBumpGraphCap +
           ggml_graph_overhead_custom(kDeepBumpGraphCap, false);
}

struct MobilenetBlockSpec
{
    int index;
    int stride;
    bool residual;
};

constexpr std::array<MobilenetBlockSpec, 16> kMobilenetSpecs = { {
    { 2, 2, false },
    { 3, 1, true },
    { 4, 2, false },
    { 5, 1, true },
    { 6, 1, true },
    { 7, 2, false },
    { 8, 1, true },
    { 9, 1, true },
    { 10, 1, true },
    { 11, 1, false },
    { 12, 1, true },
    { 13, 1, true },
    { 14, 2, false },
    { 15, 1, true },
    { 16, 1, true },
    { 17, 1, false },
} };

struct DeepBumpBuildCtx
{
    ggml_context *ctx = nullptr;
    const GgufStageWeights &w;
    std::string &err;
};

struct DeepBumpTileCoord
{
    int32_t x0 = 0;
    int32_t y0 = 0;
    bool clampEdge = false;
};

int SkipIndexForFeature(int featIdx)
{
    switch (featIdx)
    {
    case 3:
        return 1;
    case 6:
        return 2;
    case 10:
        return 3;
    case 13:
        return 4;
    default:
        return -1;
    }
}

ggml_tensor *ConvReLU6(const DeepBumpBuildCtx &b, const std::string &prefix, ggml_tensor *x,
                       const Conv2dParams &cp)
{
    ggml_tensor *kw = b.w.Require(prefix + ".weight", b.err);
    ggml_tensor *kb = b.w.Find(prefix + ".bias");
    if (kw == nullptr)
    {
        return nullptr;
    }
    return ggml_clamp(b.ctx, Conv2d(b.ctx, kw, kb, x, cp), 0.0f, 6.0f);
}

ggml_tensor *InvertedResidual(const DeepBumpBuildCtx &b, const MobilenetBlockSpec &spec,
                              ggml_tensor *x)
{
    const std::string base = "encoder.features." + std::to_string(spec.index) + ".conv.";
    ggml_tensor *h = ConvReLU6(b, base + "0.0", x, { 1, 0, false });
    ggml_tensor *dwW = b.w.Require(base + "1.0.weight", b.err);
    ggml_tensor *dwB = b.w.Find(base + "1.0.bias");
    ggml_tensor *pwW = b.w.Require(base + "2.weight", b.err);
    ggml_tensor *pwB = b.w.Find(base + "2.bias");
    if (h == nullptr || dwW == nullptr || pwW == nullptr)
    {
        return nullptr;
    }
    h = ggml_clamp(b.ctx, DepthwiseConv2d(b.ctx, dwW, dwB, h, { spec.stride, 1, false }), 0.0f, 6.0f);
    h = Conv2d(b.ctx, pwW, pwB, h, { 1, 0, false });
    return spec.residual ? ggml_add(b.ctx, x, h) : h;
}

ggml_tensor *DecoderStage(const DeepBumpBuildCtx &b, int idx, ggml_tensor *x, ggml_tensor *skip)
{
    ggml_tensor *up = ggml_upscale(b.ctx, x, 2, GGML_SCALE_MODE_NEAREST);
    ggml_tensor *h = skip != nullptr ? ggml_concat(b.ctx, up, skip, 2) : up;
    const std::string base = "decoder.blocks." + std::to_string(idx);
    ggml_tensor *w1 = b.w.Require(base + ".conv1.0.weight", b.err);
    ggml_tensor *b1 = b.w.Find(base + ".conv1.0.bias");
    ggml_tensor *w2 = b.w.Require(base + ".conv2.0.weight", b.err);
    ggml_tensor *b2 = b.w.Find(base + ".conv2.0.bias");
    if (w1 == nullptr || w2 == nullptr)
    {
        return nullptr;
    }
    h = ggml_relu(b.ctx, Conv2d(b.ctx, w1, b1, h, { 1, 1, false }));
    return ggml_relu(b.ctx, Conv2d(b.ctx, w2, b2, h, { 1, 1, false }));
}

ggml_tensor *BuildDeepBumpGraph(const DeepBumpBuildCtx &b, ggml_tensor *x)
{
    std::array<ggml_tensor *, 5> skips = {};
    ggml_tensor *h = ConvReLU6(b, "encoder.features.0.0", x, { 2, 1, false });
    ggml_tensor *dw1W = b.w.Require("encoder.features.1.conv.0.0.weight", b.err);
    ggml_tensor *dw1B = b.w.Find("encoder.features.1.conv.0.0.bias");
    ggml_tensor *pw1W = b.w.Require("encoder.features.1.conv.1.weight", b.err);
    ggml_tensor *pw1B = b.w.Find("encoder.features.1.conv.1.bias");
    if (h == nullptr || dw1W == nullptr || pw1W == nullptr)
    {
        return nullptr;
    }
    h = ggml_clamp(b.ctx, DepthwiseConv2d(b.ctx, dw1W, dw1B, h, { 1, 1, false }), 0.0f, 6.0f);
    h = Conv2d(b.ctx, pw1W, pw1B, h, { 1, 0, false });
    skips[0] = h;

    for (const MobilenetBlockSpec &spec : kMobilenetSpecs)
    {
        h = InvertedResidual(b, spec, h);
        if (h == nullptr)
        {
            return nullptr;
        }
        const int skipSlot = SkipIndexForFeature(spec.index);
        if (skipSlot >= 0)
        {
            skips[static_cast<size_t>(skipSlot)] = h;
        }
    }

    h = ConvReLU6(b, "encoder.features.18.0", h, { 1, 0, false });
    if (h == nullptr)
    {
        return nullptr;
    }

    static constexpr std::array<int, 5> kSkipIdx = { 4, 2, 1, 0, -1 };
    for (int i = 0; i < 5; i++)
    {
        const int si = kSkipIdx[static_cast<size_t>(i)];
        h = DecoderStage(b, i, h, si >= 0 ? skips[static_cast<size_t>(si)] : nullptr);
        if (h == nullptr)
        {
            return nullptr;
        }
    }

    ggml_tensor *segW = b.w.Require("segmentation_head.0.weight", b.err);
    ggml_tensor *segB = b.w.Find("segmentation_head.0.bias");
    if (segW == nullptr)
    {
        return nullptr;
    }
    return ggml_sigmoid(b.ctx, Conv2d(b.ctx, segW, segB, h, { 1, 1, false }));
}

ggml_cgraph *CreateTileCGraph(ggml_context *ctx, const GgufStageWeights &w, ggml_tensor *&outIn,
                              ggml_tensor *&outOut, std::string &err)
{
    outIn = ggml_new_tensor_4d(ctx, GGML_TYPE_F32, kTileSize, kTileSize, 1, 1);
    ggml_set_input(outIn);
    const DeepBumpBuildCtx b = { ctx, w, err };
    outOut = BuildDeepBumpGraph(b, outIn);
    if (outOut == nullptr)
    {
        return nullptr;
    }
    ggml_set_output(outOut);
    ggml_cgraph *gf = ggml_new_graph_custom(ctx, kDeepBumpGraphCap, false);
    ggml_build_forward_expand(gf, outOut);
    return gf;
}

bool SelectDeepBumpBackend(const GgufModelFile &model, const WorkerVulkanDevice &vkDev,
                           const WorkerRunOptions &opts, WorkerBackendHandle &backend,
                           std::string &outProvider, std::string &outError)
{
    if (!opts.forceCpu && vkDev.index >= 0)
    {
        const WorkerVramBudget budget = ComputeWorkerVramBudget(vkDev.index, opts.vramCapFraction);
        GgufStageWeights dry;
        if (model.CreateDryRunStage({}, dry, outError))
        {
            ggml_backend_buffer_type_t vkBuft = WorkerVulkanBufferType(vkDev.index);
            const uint64_t weightEst = model.EstimateStageBytes({}, vkBuft);
            size_t computeEst = 0;
            const auto measureFn = [&](ggml_context *ctx) -> ggml_cgraph * {
                ggml_tensor *inT = nullptr;
                ggml_tensor *outT = nullptr;
                std::string dummyErr;
                return CreateTileCGraph(ctx, dry, inT, outT, dummyErr);
            };
            if (MeasureGraphComputeBytes(vkBuft, measureFn, computeEst))
            {
                const uint64_t totalEst =
                    weightEst + static_cast<uint64_t>(computeEst) + (32ULL << 20);
                if (totalEst <= budget.effectiveBudgetBytes && backend.InitVulkan(vkDev.index))
                {
                    outProvider = backend.Label();
                    return true;
                }
            }
        }
    }
    if (!backend.InitCpu())
    {
        outError = "Failed initializing CPU fallback backend for DeepBump.";
        return false;
    }
    outProvider = (!opts.forceCpu && vkDev.index >= 0) ? "ggml-cpu (VRAM cap)" : "ggml-cpu";
    return true;
}

std::vector<float> BuildDeepBumpTileWeights()
{
    std::vector<float> weights(static_cast<size_t>(kTileSize) * kTileSize, 1.0f);
    std::array<float, kTileSize> w1d{};
    for (uint32_t i = 0; i < kTileSize; i++)
    {
        if (i < kTileCut || i >= kTileSize - kTileCut)
        {
            w1d[i] = 0.0f;
        }
        else if (i < kTileHalo)
        {
            w1d[i] = (static_cast<float>(i - kTileCut) + 0.5f) / static_cast<float>(kTileRamp);
        }
        else if (i >= kTileSize - kTileHalo)
        {
            w1d[i] = (static_cast<float>((kTileSize - kTileCut) - i) - 0.5f) /
                     static_cast<float>(kTileRamp);
        }
        else
        {
            w1d[i] = 1.0f;
        }
    }
    for (uint32_t y = 0; y < kTileSize; y++)
    {
        for (uint32_t x = 0; x < kTileSize; x++)
        {
            weights[static_cast<size_t>(y) * kTileSize + x] = w1d[y] * w1d[x];
        }
    }
    return weights;
}

std::vector<int32_t> ComputeDeepBumpTileOrigins(uint32_t dim)
{
    const int32_t d = static_cast<int32_t>(dim);
    const int32_t core = static_cast<int32_t>(kTileSize - 2u * kTileHalo);
    if (d <= core)
    {
        return { (d - static_cast<int32_t>(kTileSize)) / 2 };
    }

    const int32_t first = -static_cast<int32_t>(kTileHalo);
    const int32_t last = d - static_cast<int32_t>(kTileSize - kTileHalo);
    const int32_t span = last - first;
    const int32_t maxStep = static_cast<int32_t>(kTileMaxStep);
    const int32_t numSteps = (span + maxStep - 1) / maxStep;
    std::vector<int32_t> origins;
    origins.reserve(static_cast<size_t>(numSteps) + 1u);
    for (int32_t t = 0; t <= numSteps; t++)
    {
        origins.push_back(first + (span * t) / numSteps);
    }
    return origins;
}

int32_t SampleCoordSigned(int32_t pos, int32_t limit, bool clampEdge)
{
    if (limit <= 1)
    {
        return 0;
    }
    if (!clampEdge)
    {
        const int32_t rem = pos % limit;
        return rem < 0 ? rem + limit : rem;
    }
    const int32_t period = 2 * limit;
    int32_t m = pos % period;
    if (m < 0)
    {
        m += period;
    }
    return m < limit ? m : (period - 1 - m);
}

void ExtractPaddedTile(const EditorAiTensorNchw &input, const DeepBumpTileCoord &tc,
                       std::vector<float> &tileIn)
{
    const int32_t inW = static_cast<int32_t>(input.width);
    const int32_t inH = static_cast<int32_t>(input.height);
    for (uint32_t dy = 0; dy < kTileSize; dy++)
    {
        const uint32_t sy = static_cast<uint32_t>(
            SampleCoordSigned(tc.y0 + static_cast<int32_t>(dy), inH, tc.clampEdge));
        for (uint32_t dx = 0; dx < kTileSize; dx++)
        {
            const uint32_t sx = static_cast<uint32_t>(
                SampleCoordSigned(tc.x0 + static_cast<int32_t>(dx), inW, tc.clampEdge));
            tileIn[static_cast<size_t>(dy) * kTileSize + dx] =
                input.data[static_cast<size_t>(sy) * input.width + sx];
        }
    }
}

void AccumulateFeatheredTile(const std::vector<float> &tileOut, const std::vector<float> &weights,
                             const DeepBumpTileCoord &tc, EditorAiTensorNchw &output,
                             std::vector<float> &weightSum)
{
    const size_t plane = static_cast<size_t>(output.width) * output.height;
    const size_t tilePlane = static_cast<size_t>(kTileSize) * kTileSize;
    const int32_t outW = static_cast<int32_t>(output.width);
    const int32_t outH = static_cast<int32_t>(output.height);
    for (uint32_t dy = 0; dy < kTileSize; dy++)
    {
        const int32_t sy = tc.y0 + static_cast<int32_t>(dy);
        if (sy < 0 || sy >= outH)
        {
            continue;
        }
        for (uint32_t dx = 0; dx < kTileSize; dx++)
        {
            const int32_t sx = tc.x0 + static_cast<int32_t>(dx);
            if (sx < 0 || sx >= outW)
            {
                continue;
            }
            const size_t tileIdx = static_cast<size_t>(dy) * kTileSize + dx;
            const float w = weights[tileIdx];
            if (!(w > 0.0f))
            {
                continue;
            }
            const size_t dstIdx =
                static_cast<size_t>(sy) * output.width + static_cast<size_t>(sx);
            weightSum[dstIdx] += w;
            for (uint32_t c = 0; c < 3u; c++)
            {
                output.data[c * plane + dstIdx] += tileOut[c * tilePlane + tileIdx] * w;
            }
        }
    }
}

void NormalizeWeightedOutput(const std::vector<float> &weightSum, EditorAiTensorNchw &output)
{
    const size_t plane = static_cast<size_t>(output.width) * output.height;
    for (size_t i = 0; i < plane; i++)
    {
        const float w = weightSum[i];
        if (w > 1e-6f)
        {
            const float inv = 1.0f / w;
            for (uint32_t c = 0; c < 3u; c++)
            {
                output.data[c * plane + i] *= inv;
            }
        }
    }
}

struct DeepBumpGraphRunner
{
    ggml_context *ctx = nullptr;
    ggml_cgraph *graph = nullptr;
    ggml_gallocr_t alloc = nullptr;
    ggml_tensor *inTensor = nullptr;
    ggml_tensor *outTensor = nullptr;

    ~DeepBumpGraphRunner()
    {
        if (alloc != nullptr)
        {
            ggml_gallocr_free(alloc);
        }
        if (ctx != nullptr)
        {
            ggml_free(ctx);
        }
    }

    bool Init(const GgufStageWeights &weights, ggml_backend_buffer_type_t buft, std::string &err)
    {
        ggml_init_params params = { DeepBumpGraphCtxMem(), nullptr, true };
        ctx = ggml_init(params);
        graph = ctx != nullptr ? CreateTileCGraph(ctx, weights, inTensor, outTensor, err) : nullptr;
        alloc = graph != nullptr ? ggml_gallocr_new(buft) : nullptr;
        if (alloc == nullptr || !ggml_gallocr_alloc_graph(alloc, graph))
        {
            if (err.empty())
            {
                err = "Failed allocating DeepBump compute graph.";
            }
            return false;
        }
        return true;
    }
};

void NormalizeDeepBumpLumaInput(const EditorAiTensorNchw &input, EditorAiTensorNchw &out)
{
    out = input;
    if (out.data.empty())
    {
        return;
    }
    std::vector<float> sorted = out.data;
    std::sort(sorted.begin(), sorted.end());
    const size_t last = sorted.size() - 1u;
    const float p01 = sorted[last / 100u];
    const float p99 = sorted[(last * 99u) / 100u];
    const float span = p99 - p01;
    const float mid = 0.5f * (p01 + p99);
    const float denom = std::max(span, 0.25f);
    for (float &v : out.data)
    {
        v = std::clamp(0.5f + (v - mid) / denom, 0.0f, 1.0f);
    }
}

bool ExecuteDeepBumpTiles(const WorkerBackendHandle &backend, DeepBumpGraphRunner &runner,
                          const WorkerRunOptions &opts, const EditorAiTensorNchw &input,
                          WorkerRunResult &result)
{
    const size_t plane = static_cast<size_t>(input.width) * input.height;
    result.output.channels = 3u;
    result.output.width = input.width;
    result.output.height = input.height;
    result.output.data.assign(plane * 3u, 0.0f);
    std::vector<float> weightSum(plane, 0.0f);
    const std::vector<float> tileWeights = BuildDeepBumpTileWeights();

    const std::vector<int32_t> xs = ComputeDeepBumpTileOrigins(input.width);
    const std::vector<int32_t> ys = ComputeDeepBumpTileOrigins(input.height);
    const uint32_t totalTiles = static_cast<uint32_t>(xs.size() * ys.size());
    std::vector<float> tileIn(static_cast<size_t>(kTileSize) * kTileSize, 0.0f);
    std::vector<float> tileOut(static_cast<size_t>(3u) * kTileSize * kTileSize, 0.0f);

    uint32_t tileDone = 0;
    for (int32_t y0 : ys)
    {
        for (int32_t x0 : xs)
        {
            const DeepBumpTileCoord tc = { x0, y0, opts.clampEdge };
            ExtractPaddedTile(input, tc, tileIn);
            ggml_backend_tensor_set(runner.inTensor, tileIn.data(), 0u,
                                    tileIn.size() * sizeof(float));
            if (ggml_backend_graph_compute(backend.Get(), runner.graph) != GGML_STATUS_SUCCESS)
            {
                result.error = "ggml_backend_graph_compute failed during DeepBump.";
                return false;
            }
            ggml_backend_tensor_get(runner.outTensor, tileOut.data(), 0u,
                                    tileOut.size() * sizeof(float));
            AccumulateFeatheredTile(tileOut, tileWeights, tc, result.output, weightSum);
            tileDone++;
            if (opts.onProgress)
            {
                char msg[96] = {};
                std::snprintf(msg, sizeof(msg), "DeepBump tile %u/%u", tileDone, totalTiles);
                opts.onProgress(10 + static_cast<int32_t>(tileDone * 85u / totalTiles), msg);
            }
        }
    }
    NormalizeWeightedOutput(weightSum, result.output);
    return true;
}

} // namespace

bool RunDeepBumpGgufPipeline(const GgufModelFile &model, const WorkerVulkanDevice &vkDev,
                             const WorkerRunOptions &opts, const EditorAiTensorNchw &input,
                             WorkerRunResult &result)
{
    if (input.channels != 1u || input.width == 0u || input.height == 0u)
    {
        result.error = "DeepBump expects 1-channel float32 NCHW luma input.";
        return false;
    }

    WorkerBackendHandle backend;
    if (!SelectDeepBumpBackend(model, vkDev, opts, backend, result.provider, result.error))
    {
        return false;
    }

    GgufStageWeights weights;
    if (!model.LoadStageWeights({}, backend.Get(), weights, result.error))
    {
        return false;
    }

    DeepBumpGraphRunner runner;
    if (!runner.Init(weights, backend.BufferType(), result.error))
    {
        return false;
    }

    EditorAiTensorNchw normalizedInput;
    NormalizeDeepBumpLumaInput(input, normalizedInput);
    return ExecuteDeepBumpTiles(backend, runner, opts, normalizedInput, result);
}

} // namespace qray_ai
