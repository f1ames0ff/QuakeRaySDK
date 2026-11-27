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

#include "RealPlksrPipeline.h"
#include "GgmlOps.h"

#include <ggml-alloc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace qray_ai
{
namespace
{

constexpr uint32_t kTileCut = 16u;
constexpr uint32_t kTileRamp = 16u;
constexpr uint32_t kTileHalo = kTileCut + kTileRamp;
constexpr size_t kRealPlksrGraphCap = 8192u;
constexpr size_t kRealPlksrCtxNodes = 16384u;
constexpr uint64_t kVramSafetyHeadroomBytes = 64ULL << 20;

size_t RealPlksrGraphCtxMem()
{
    return ggml_tensor_overhead() * kRealPlksrCtxNodes +
           ggml_graph_overhead_custom(kRealPlksrGraphCap, false);
}

struct RealPlksrConfig
{
    uint32_t scale = 4u;
    uint32_t dim = 64u;
    uint32_t nBlocks = 28u;
    uint32_t kernelSize = 17u;
    uint32_t pdim = 16u;
    uint32_t normGroups = 4u;
    bool dysample = true;
    uint32_t dysampleGroups = 4u;
    float normEps = 1e-5f;
};

struct RealPlksrBuildCtx
{
    ggml_context *ctx = nullptr;
    const GgufStageWeights &w;
    const RealPlksrConfig &cfg;
    std::string &err;
};

struct RealPlksrTileCoord
{
    int32_t x0 = 0;
    int32_t y0 = 0;
    uint32_t tileW = 0;
    uint32_t tileH = 0;
    bool clampEdge = false;
};

struct DySampleEndConvParams
{
    uint32_t inCh = 0;
    std::vector<float> weight; // [3 * inCh], row-major [oc * inCh + ic]
    std::vector<float> bias;   // [3]
};

RealPlksrConfig ReadRealPlksrConfig(const GgufModelFile &model)
{
    RealPlksrConfig cfg;
    cfg.scale = std::max(1u, model.GetMetaU32("realplksr.scale", 4u));
    cfg.dim = std::max(4u, model.GetMetaU32("realplksr.dim", 64u));
    cfg.nBlocks = std::max(1u, model.GetMetaU32("realplksr.n_blocks", 28u));
    cfg.kernelSize = std::max(3u, model.GetMetaU32("realplksr.kernel_size", 17u));
    cfg.pdim = std::min(cfg.dim, std::max(1u, model.GetMetaU32("realplksr.pdim", 16u)));
    cfg.normGroups = std::max(1u, model.GetMetaU32("realplksr.norm_groups", 4u));
    cfg.dysample = model.GetMetaU32("realplksr.dysample", 1u) != 0u;
    cfg.dysampleGroups = std::max(1u, model.GetMetaU32("realplksr.dysample_groups", 4u));
    cfg.normEps = model.GetMetaF32("realplksr.norm_eps", 1e-5f);
    return cfg;
}

ggml_tensor *RepeatSingleChannel(ggml_context *ctx, ggml_tensor *c1, uint32_t repeats)
{
    if (repeats <= 1u)
    {
        return c1;
    }
    ggml_tensor *acc = nullptr;
    ggml_tensor *pow2 = c1;
    uint32_t rem = repeats;
    while (rem > 0u)
    {
        if ((rem & 1u) != 0u)
        {
            acc = (acc == nullptr) ? pow2 : ggml_concat(ctx, acc, pow2, 2);
        }
        rem >>= 1u;
        if (rem > 0u)
        {
            pow2 = ggml_concat(ctx, pow2, pow2, 2);
        }
    }
    return acc;
}

ggml_tensor *BuildRepeatInterleaveRgb(ggml_context *ctx, ggml_tensor *x, uint32_t repeatsPerCh)
{
    ggml_tensor *r = ggml_cont(
        ctx, ggml_view_4d(ctx, x, x->ne[0], x->ne[1], 1, 1, x->nb[1], x->nb[2], x->nb[3], 0u));
    ggml_tensor *g = ggml_cont(
        ctx, ggml_view_4d(ctx, x, x->ne[0], x->ne[1], 1, 1, x->nb[1], x->nb[2], x->nb[3], x->nb[2]));
    ggml_tensor *b = ggml_cont(ctx, ggml_view_4d(ctx, x, x->ne[0], x->ne[1], 1, 1, x->nb[1],
                                                   x->nb[2], x->nb[3], 2u * x->nb[2]));
    ggml_tensor *repR = RepeatSingleChannel(ctx, r, repeatsPerCh);
    ggml_tensor *repG = RepeatSingleChannel(ctx, g, repeatsPerCh);
    ggml_tensor *repB = RepeatSingleChannel(ctx, b, repeatsPerCh);
    return ggml_concat(ctx, ggml_concat(ctx, repR, repG, 2), repB, 2);
}

ggml_tensor *BuildPLKBlock(const RealPlksrBuildCtx &b, uint32_t blockIdx, ggml_tensor *x,
                           uint32_t tileW, uint32_t tileH)
{
    const std::string pfx = "feats." + std::to_string(blockIdx) + ".";
    ggml_tensor *cm0W = b.w.Require(pfx + "channel_mixer.0.weight", b.err);
    ggml_tensor *cm0B = b.w.Find(pfx + "channel_mixer.0.bias");
    ggml_tensor *cm2W = b.w.Require(pfx + "channel_mixer.2.weight", b.err);
    ggml_tensor *cm2B = b.w.Find(pfx + "channel_mixer.2.bias");
    ggml_tensor *lkW = b.w.Require(pfx + "lk.conv.weight", b.err);
    ggml_tensor *lkB = b.w.Find(pfx + "lk.conv.bias");
    ggml_tensor *refW = b.w.Require(pfx + "refine.weight", b.err);
    ggml_tensor *refB = b.w.Find(pfx + "refine.bias");
    ggml_tensor *normW = b.w.Require(pfx + "norm.weight", b.err);
    ggml_tensor *normB = b.w.Find(pfx + "norm.bias");
    if (cm0W == nullptr || cm2W == nullptr || lkW == nullptr || refW == nullptr || normW == nullptr)
    {
        return nullptr;
    }

    ggml_tensor *y = Conv2d(b.ctx, cm0W, cm0B, x, { 1, 1, false });
    y = Mish(b.ctx, y);
    y = Conv2d(b.ctx, cm2W, cm2B, y, { 1, 1, false });
    if (!ggml_is_contiguous(y))
    {
        y = ggml_cont(b.ctx, y);
    }

    if (b.cfg.pdim < b.cfg.dim)
    {
        ggml_tensor *y1 =
            ggml_view_4d(b.ctx, y, tileW, tileH, b.cfg.pdim, 1, y->nb[1], y->nb[2], y->nb[3], 0u);
        ggml_tensor *y2 = ggml_cont(
            b.ctx, ggml_view_4d(b.ctx, y, tileW, tileH, b.cfg.dim - b.cfg.pdim, 1, y->nb[1],
                                y->nb[2], y->nb[3], size_t(b.cfg.pdim) * y->nb[2]));
        y1 = Conv2d(b.ctx, lkW, lkB, y1, { 1, int(b.cfg.kernelSize / 2u), false });
        y = ggml_concat(b.ctx, y1, y2, 2);
    }
    else
    {
        y = Conv2d(b.ctx, lkW, lkB, y, { 1, int(b.cfg.kernelSize / 2u), false });
    }

    ggml_tensor *attnW = b.w.Find(pfx + "attn.f.0.weight");
    if (attnW != nullptr)
    {
        ggml_tensor *attnB = b.w.Find(pfx + "attn.f.0.bias");
        ggml_tensor *gate = ggml_sigmoid(b.ctx, Conv2d(b.ctx, attnW, attnB, y, { 1, 1, false }));
        y = ggml_mul(b.ctx, y, gate);
    }

    y = Conv2d(b.ctx, refW, refB, y, { 1, 0, false });
    y = GroupNorm(b.ctx, normW, normB, y, int(b.cfg.normGroups), b.cfg.normEps);
    return ggml_add(b.ctx, y, x);
}

struct RealPlksrGraphSpec
{
    ggml_context *ctx = nullptr;
    const GgufStageWeights &weights;
    const RealPlksrConfig &cfg;
    uint32_t tileW = 0;
    uint32_t tileH = 0;
};

struct RealPlksrGraphTensors
{
    ggml_tensor *in = nullptr;
    ggml_tensor *feat = nullptr;
    ggml_tensor *offset = nullptr;
};

struct RealPlksrBackendQuery
{
    const GgufModelFile &model;
    const WorkerVulkanDevice &vkDev;
    const WorkerRunOptions &opts;
    const RealPlksrConfig &cfg;
    uint32_t needW = 0;
    uint32_t needH = 0;
};

ggml_cgraph *CreateRealPlksrTileCGraph(const RealPlksrGraphSpec &spec, RealPlksrGraphTensors &out,
                                       std::string &err)
{
    ggml_context *ctx = spec.ctx;
    const GgufStageWeights &w = spec.weights;
    const RealPlksrConfig &cfg = spec.cfg;
    out = {};
    out.in = ggml_new_tensor_4d(ctx, GGML_TYPE_F32, spec.tileW, spec.tileH, 3, 1);
    ggml_set_input(out.in);

    const RealPlksrBuildCtx b = { ctx, w, cfg, err };
    ggml_tensor *headW = w.Require("feats.0.weight", err);
    ggml_tensor *headB = w.Find("feats.0.bias");
    if (headW == nullptr)
    {
        return nullptr;
    }

    ggml_tensor *h = Conv2d(ctx, headW, headB, out.in, { 1, 1, false });
    for (uint32_t i = 1u; i <= cfg.nBlocks; i++)
    {
        h = BuildPLKBlock(b, i, h, spec.tileW, spec.tileH);
        if (h == nullptr)
        {
            return nullptr;
        }
    }

    const std::string tailPrimary = "feats." + std::to_string(cfg.nBlocks + 2u);
    const std::string tailFallback = "feats." + std::to_string(cfg.nBlocks + 1u);
    const std::string &tailPfx = w.Find(tailPrimary + ".weight") != nullptr ? tailPrimary : tailFallback;
    ggml_tensor *tailW = w.Require(tailPfx + ".weight", err);
    ggml_tensor *tailB = w.Find(tailPfx + ".bias");
    if (tailW == nullptr)
    {
        return nullptr;
    }
    h = Conv2d(ctx, tailW, tailB, h, { 1, 1, false });

    const uint32_t s2 = cfg.scale * cfg.scale;
    ggml_tensor *skip = BuildRepeatInterleaveRgb(ctx, out.in, s2);
    out.feat = ggml_add(ctx, h, skip);
    ggml_set_output(out.feat);

    ggml_cgraph *gf = ggml_new_graph_custom(ctx, kRealPlksrGraphCap, false);
    ggml_build_forward_expand(gf, out.feat);

    if (cfg.dysample)
    {
        ggml_tensor *offW = w.Require("to_img.offset.weight", err);
        ggml_tensor *offB = w.Find("to_img.offset.bias");
        ggml_tensor *scpW = w.Require("to_img.scope.weight", err);
        ggml_tensor *initPos = w.Require("to_img.init_pos", err);
        if (offW == nullptr || scpW == nullptr || initPos == nullptr)
        {
            return nullptr;
        }
        ggml_tensor *off = Conv2d(ctx, offW, offB, out.feat, { 1, 0, false });
        ggml_tensor *scp = ggml_sigmoid(ctx, Conv2d(ctx, scpW, nullptr, out.feat, { 1, 0, false }));
        off = ggml_scale(ctx, ggml_mul(ctx, off, scp), 0.5f);
        out.offset = AddBiasSpatial(ctx, off, initPos);
        ggml_set_output(out.offset);
        ggml_build_forward_expand(gf, out.offset);
    }

    return gf;
}

bool SelectRealPlksrBackend(const RealPlksrBackendQuery &q, WorkerBackendHandle &backend,
                            uint32_t &outTileMax, std::string &outProvider, std::string &outError)
{
    if (!q.opts.forceCpu && q.vkDev.index >= 0)
    {
        const WorkerVramBudget budget =
            ComputeWorkerVramBudget(q.vkDev.index, q.opts.vramCapFraction);
        GgufStageWeights dry;
        if (q.model.CreateDryRunStage({}, dry, outError))
        {
            ggml_backend_buffer_type_t vkBuft = WorkerVulkanBufferType(q.vkDev.index);
            const uint64_t weightEst = q.model.EstimateStageBytes({}, vkBuft);
            static constexpr uint32_t kCandidateMax[] = { 192u, 128u };
            for (uint32_t candMax : kCandidateMax)
            {
                const uint32_t probeW = std::min(q.needW, candMax);
                const uint32_t probeH = std::min(q.needH, candMax);
                size_t computeEst = 0;
                const auto measureFn = [&](ggml_context *ctx) -> ggml_cgraph * {
                    RealPlksrGraphTensors t;
                    std::string dummyErr;
                    return CreateRealPlksrTileCGraph({ ctx, dry, q.cfg, probeW, probeH }, t,
                                                     dummyErr);
                };
                if (MeasureGraphComputeBytes(vkBuft, measureFn, computeEst))
                {
                    const uint64_t totalEst =
                        weightEst + static_cast<uint64_t>(computeEst) + kVramSafetyHeadroomBytes;
                    if (totalEst <= budget.effectiveBudgetBytes &&
                        backend.InitVulkan(q.vkDev.index))
                    {
                        outTileMax = candMax;
                        outProvider = backend.Label();
                        return true;
                    }
                }
            }
        }
    }

    if (!backend.InitCpu())
    {
        outError = "Failed initializing CPU fallback backend for RealPLKSR.";
        return false;
    }
    outTileMax = 128u;
    outProvider =
        (!q.opts.forceCpu && q.vkDev.index >= 0) ? "ggml-cpu (VRAM cap)" : "ggml-cpu";
    return true;
}

bool LoadDySampleEndConvParams(const GgufStageWeights &weights, DySampleEndConvParams &out,
                               std::string &err)
{
    ggml_tensor *endW = weights.Require("to_img.end_conv.weight", err);
    ggml_tensor *endB = weights.Find("to_img.end_conv.bias");
    if (endW == nullptr)
    {
        return false;
    }
    if (endW->ne[3] != 3 || endW->ne[0] != 1 || endW->ne[1] != 1 || endW->ne[2] <= 0)
    {
        err = "Unexpected to_img.end_conv.weight shape in RealPLKSR GGUF.";
        return false;
    }

    out.inCh = static_cast<uint32_t>(endW->ne[2]);
    const size_t weightCount = static_cast<size_t>(out.inCh) * 3u;
    out.weight.resize(weightCount);
    if (endW->type == GGML_TYPE_F16)
    {
        std::vector<ggml_fp16_t> raw(weightCount);
        ggml_backend_tensor_get(endW, raw.data(), 0u, weightCount * sizeof(ggml_fp16_t));
        for (size_t i = 0; i < weightCount; i++)
        {
            out.weight[i] = ggml_fp16_to_fp32(raw[i]);
        }
    }
    else if (endW->type == GGML_TYPE_F32)
    {
        ggml_backend_tensor_get(endW, out.weight.data(), 0u, weightCount * sizeof(float));
    }
    else
    {
        err = "Unsupported to_img.end_conv.weight tensor dtype in RealPLKSR GGUF.";
        return false;
    }

    out.bias.assign(3u, 0.0f);
    if (endB != nullptr)
    {
        if (endB->type == GGML_TYPE_F32)
        {
            ggml_backend_tensor_get(endB, out.bias.data(), 0u, 3u * sizeof(float));
        }
        else if (endB->type == GGML_TYPE_F16)
        {
            std::vector<ggml_fp16_t> rawB(3u);
            ggml_backend_tensor_get(endB, rawB.data(), 0u, 3u * sizeof(ggml_fp16_t));
            for (size_t c = 0; c < 3u; c++)
            {
                out.bias[c] = ggml_fp16_to_fp32(rawB[c]);
            }
        }
    }
    return true;
}

void RunDySampleAndEndConvCpu(const RealPlksrConfig &cfg, const DySampleEndConvParams &endConv,
                              const std::vector<float> &featData, const std::vector<float> &offData,
                              uint32_t tileW, uint32_t tileH, std::vector<float> &outTileRgb)
{
    const uint32_t scale = cfg.scale;
    const uint32_t outW = tileW * scale;
    const uint32_t outH = tileH * scale;
    const size_t tilePlane = static_cast<size_t>(tileW) * tileH;
    const size_t outPlane = static_cast<size_t>(outW) * outH;
    const uint32_t inCh = 3u * scale * scale;
    const uint32_t groups = cfg.dysampleGroups;
    const uint32_t chPerGroup = inCh / groups;
    const uint32_t s2 = scale * scale;
    const float maxX = static_cast<float>(tileW - 1u);
    const float maxY = static_cast<float>(tileH - 1u);

    outTileRgb.resize(outPlane * 3u);

    for (uint32_t oy = 0u; oy < outH; oy++)
    {
        const uint32_t y = oy / scale;
        const uint32_t sy = oy % scale;
        for (uint32_t ox = 0u; ox < outW; ox++)
        {
            const uint32_t x = ox / scale;
            const uint32_t sx = ox % scale;
            const size_t srcIdx = static_cast<size_t>(y) * tileW + x;

            float acc[3] = { endConv.bias[0], endConv.bias[1], endConv.bias[2] };
            for (uint32_t g = 0u; g < groups; g++)
            {
                const uint32_t cx = g * s2 + sy * scale + sx;
                const uint32_t cy = groups * s2 + cx;
                const float px =
                    std::clamp(static_cast<float>(x) + offData[cx * tilePlane + srcIdx], 0.0f, maxX);
                const float py =
                    std::clamp(static_cast<float>(y) + offData[cy * tilePlane + srcIdx], 0.0f, maxY);

                const uint32_t x0 = static_cast<uint32_t>(px);
                const uint32_t y0 = static_cast<uint32_t>(py);
                const uint32_t x1 = std::min(x0 + 1u, tileW - 1u);
                const uint32_t y1 = std::min(y0 + 1u, tileH - 1u);
                const float fx = px - static_cast<float>(x0);
                const float fy = py - static_cast<float>(y0);
                const float w00 = (1.0f - fx) * (1.0f - fy);
                const float w10 = fx * (1.0f - fy);
                const float w01 = (1.0f - fx) * fy;
                const float w11 = fx * fy;
                const size_t i00 = static_cast<size_t>(y0) * tileW + x0;
                const size_t i10 = static_cast<size_t>(y0) * tileW + x1;
                const size_t i01 = static_cast<size_t>(y1) * tileW + x0;
                const size_t i11 = static_cast<size_t>(y1) * tileW + x1;

                for (uint32_t k = 0u; k < chPerGroup; k++)
                {
                    const uint32_t ic = g * chPerGroup + k;
                    const float *ch = featData.data() + ic * tilePlane;
                    const float val = w00 * ch[i00] + w10 * ch[i10] + w01 * ch[i01] + w11 * ch[i11];
                    acc[0] += endConv.weight[0u * inCh + ic] * val;
                    acc[1] += endConv.weight[1u * inCh + ic] * val;
                    acc[2] += endConv.weight[2u * inCh + ic] * val;
                }
            }

            const size_t dstIdx = static_cast<size_t>(oy) * outW + ox;
            outTileRgb[0u * outPlane + dstIdx] = std::clamp(acc[0], 0.0f, 1.0f);
            outTileRgb[1u * outPlane + dstIdx] = std::clamp(acc[1], 0.0f, 1.0f);
            outTileRgb[2u * outPlane + dstIdx] = std::clamp(acc[2], 0.0f, 1.0f);
        }
    }
}

void RunPixelShuffleCpu(uint32_t scale, const std::vector<float> &featData, uint32_t tileW,
                        uint32_t tileH, std::vector<float> &outTileRgb)
{
    const uint32_t outW = tileW * scale;
    const uint32_t outH = tileH * scale;
    const size_t tilePlane = static_cast<size_t>(tileW) * tileH;
    const size_t outPlane = static_cast<size_t>(outW) * outH;
    const uint32_t s2 = scale * scale;
    outTileRgb.resize(outPlane * 3u);

    for (uint32_t c = 0u; c < 3u; c++)
    {
        for (uint32_t oy = 0u; oy < outH; oy++)
        {
            const uint32_t y = oy / scale;
            const uint32_t sy = oy % scale;
            for (uint32_t ox = 0u; ox < outW; ox++)
            {
                const uint32_t x = ox / scale;
                const uint32_t sx = ox % scale;
                const uint32_t srcCh = c * s2 + sy * scale + sx;
                const float val = featData[srcCh * tilePlane + static_cast<size_t>(y) * tileW + x];
                outTileRgb[c * outPlane + static_cast<size_t>(oy) * outW + ox] =
                    std::clamp(val, 0.0f, 1.0f);
            }
        }
    }
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

void ExtractPaddedRgbTile(const EditorAiTensorNchw &input, const RealPlksrTileCoord &tc,
                          std::vector<float> &tileIn)
{
    const int32_t inW = static_cast<int32_t>(input.width);
    const int32_t inH = static_cast<int32_t>(input.height);
    const size_t inPlane = static_cast<size_t>(input.width) * input.height;
    const size_t tilePlane = static_cast<size_t>(tc.tileW) * tc.tileH;
    tileIn.resize(tilePlane * 3u);

    for (uint32_t dy = 0u; dy < tc.tileH; dy++)
    {
        const uint32_t sy = static_cast<uint32_t>(
            SampleCoordSigned(tc.y0 + static_cast<int32_t>(dy), inH, tc.clampEdge));
        for (uint32_t dx = 0u; dx < tc.tileW; dx++)
        {
            const uint32_t sx = static_cast<uint32_t>(
                SampleCoordSigned(tc.x0 + static_cast<int32_t>(dx), inW, tc.clampEdge));
            const size_t srcIdx = static_cast<size_t>(sy) * input.width + sx;
            const size_t dstIdx = static_cast<size_t>(dy) * tc.tileW + dx;
            tileIn[0u * tilePlane + dstIdx] = input.data[0u * inPlane + srcIdx];
            tileIn[1u * tilePlane + dstIdx] = input.data[1u * inPlane + srcIdx];
            tileIn[2u * tilePlane + dstIdx] = input.data[2u * inPlane + srcIdx];
        }
    }
}

std::vector<int32_t> ComputeRealPlksrTileOrigins(uint32_t dim, uint32_t tileDim)
{
    const int32_t d = static_cast<int32_t>(dim);
    const int32_t t = static_cast<int32_t>(tileDim);
    const int32_t halo = static_cast<int32_t>(kTileHalo);
    const int32_t core = t - 2 * halo;
    if (d <= core)
    {
        return { -halo };
    }

    const int32_t first = -halo;
    const int32_t last = d - (t - halo);
    const int32_t span = last - first;
    const int32_t maxStep = t - static_cast<int32_t>(2u * kTileCut + kTileRamp);
    const int32_t numSteps = std::max(1, (span + maxStep - 1) / maxStep);
    std::vector<int32_t> origins;
    origins.reserve(static_cast<size_t>(numSteps) + 1u);
    for (int32_t step = 0; step <= numSteps; step++)
    {
        origins.push_back(first + (span * step) / numSteps);
    }
    return origins;
}

std::vector<float> BuildOutputFeatherWeights(uint32_t outTileW, uint32_t outTileH, uint32_t scale)
{
    const uint32_t cutOut = kTileCut * scale;
    const uint32_t rampOut = kTileRamp * scale;
    const uint32_t haloOut = kTileHalo * scale;
    const auto build1d = [&](uint32_t len) {
        std::vector<float> w1d(len, 1.0f);
        for (uint32_t i = 0u; i < len; i++)
        {
            if (i < cutOut || i >= len - cutOut)
            {
                w1d[i] = 0.0f;
            }
            else if (i < haloOut)
            {
                w1d[i] = (static_cast<float>(i - cutOut) + 0.5f) / static_cast<float>(rampOut);
            }
            else if (i >= len - haloOut)
            {
                w1d[i] =
                    (static_cast<float>((len - cutOut) - i) - 0.5f) / static_cast<float>(rampOut);
            }
        }
        return w1d;
    };

    const std::vector<float> wx = build1d(outTileW);
    const std::vector<float> wy = build1d(outTileH);
    std::vector<float> weights(static_cast<size_t>(outTileW) * outTileH, 1.0f);
    for (uint32_t y = 0u; y < outTileH; y++)
    {
        for (uint32_t x = 0u; x < outTileW; x++)
        {
            weights[static_cast<size_t>(y) * outTileW + x] = wy[y] * wx[x];
        }
    }
    return weights;
}

void AccumulateFeatheredUpscaledTile(const std::vector<float> &tileOut,
                                     const std::vector<float> &weights,
                                     const RealPlksrTileCoord &tc, uint32_t scale,
                                     EditorAiTensorNchw &output, std::vector<float> &weightSum)
{
    const uint32_t tileOutW = tc.tileW * scale;
    const uint32_t tileOutH = tc.tileH * scale;
    const size_t tilePlane = static_cast<size_t>(tileOutW) * tileOutH;
    const size_t outPlane = static_cast<size_t>(output.width) * output.height;
    const int32_t ox0 = tc.x0 * static_cast<int32_t>(scale);
    const int32_t oy0 = tc.y0 * static_cast<int32_t>(scale);
    const int32_t outW = static_cast<int32_t>(output.width);
    const int32_t outH = static_cast<int32_t>(output.height);

    for (uint32_t dy = 0u; dy < tileOutH; dy++)
    {
        const int32_t sy = oy0 + static_cast<int32_t>(dy);
        if (sy < 0 || sy >= outH)
        {
            continue;
        }
        for (uint32_t dx = 0u; dx < tileOutW; dx++)
        {
            const int32_t sx = ox0 + static_cast<int32_t>(dx);
            if (sx < 0 || sx >= outW)
            {
                continue;
            }
            const size_t tileIdx = static_cast<size_t>(dy) * tileOutW + dx;
            const float w = weights[tileIdx];
            if (!(w > 0.0f))
            {
                continue;
            }
            const size_t dstIdx =
                static_cast<size_t>(sy) * output.width + static_cast<size_t>(sx);
            weightSum[dstIdx] += w;
            for (uint32_t c = 0u; c < 3u; c++)
            {
                output.data[c * outPlane + dstIdx] += tileOut[c * tilePlane + tileIdx] * w;
            }
        }
    }
}

void NormalizeAndClampOutput(const std::vector<float> &weightSum, EditorAiTensorNchw &output)
{
    const size_t outPlane = static_cast<size_t>(output.width) * output.height;
    for (size_t i = 0; i < outPlane; i++)
    {
        const float w = weightSum[i];
        if (w > 1e-6f)
        {
            const float inv = 1.0f / w;
            for (uint32_t c = 0u; c < 3u; c++)
            {
                output.data[c * outPlane + i] =
                    std::clamp(output.data[c * outPlane + i] * inv, 0.0f, 1.0f);
            }
        }
    }
}

void CropSingleTileToOutput(const std::vector<float> &tileOut, uint32_t tileOutW, uint32_t tileOutH,
                            uint32_t scale, uint32_t dstW, uint32_t dstH,
                            EditorAiTensorNchw &output)
{
    const size_t tilePlane = static_cast<size_t>(tileOutW) * tileOutH;
    const size_t dstPlane = static_cast<size_t>(dstW) * dstH;
    const uint32_t haloOut = kTileHalo * scale;
    output.channels = 3u;
    output.width = dstW;
    output.height = dstH;
    output.data.resize(dstPlane * 3u);

    for (uint32_t c = 0u; c < 3u; c++)
    {
        for (uint32_t y = 0u; y < dstH; y++)
        {
            for (uint32_t x = 0u; x < dstW; x++)
            {
                const size_t srcIdx =
                    static_cast<size_t>(y + haloOut) * tileOutW + (x + haloOut);
                const size_t dstIdx = static_cast<size_t>(y) * dstW + x;
                output.data[c * dstPlane + dstIdx] = tileOut[c * tilePlane + srcIdx];
            }
        }
    }
}

struct RealPlksrGraphRunner
{
    ggml_context *ctx = nullptr;
    ggml_cgraph *graph = nullptr;
    ggml_gallocr_t alloc = nullptr;
    RealPlksrGraphTensors tensors;

    ~RealPlksrGraphRunner()
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

    bool Init(const GgufStageWeights &weights, const RealPlksrConfig &cfg, uint32_t tileW,
              uint32_t tileH, ggml_backend_buffer_type_t buft, std::string &err)
    {
        ggml_init_params params = { RealPlksrGraphCtxMem(), nullptr, true };
        ctx = ggml_init(params);
        graph = ctx != nullptr
                    ? CreateRealPlksrTileCGraph({ ctx, weights, cfg, tileW, tileH }, tensors, err)
                    : nullptr;
        alloc = graph != nullptr ? ggml_gallocr_new(buft) : nullptr;
        if (alloc == nullptr || !ggml_gallocr_alloc_graph(alloc, graph))
        {
            if (err.empty())
            {
                err = "Failed allocating RealPLKSR compute graph.";
            }
            return false;
        }
        return true;
    }
};

struct RealPlksrDispatchBuffers
{
    const RealPlksrConfig &cfg;
    const DySampleEndConvParams &endConv;
    WorkerBackendHandle &backend;
    RealPlksrGraphRunner &runner;
    uint32_t tileW = 0;
    uint32_t tileH = 0;
    std::vector<float> tileIn;
    std::vector<float> featData;
    std::vector<float> offData;
    std::vector<float> tileOutRgb;
};

bool DispatchRealPlksrTile(const EditorAiTensorNchw &input, const RealPlksrTileCoord &tc,
                           RealPlksrDispatchBuffers &buf, std::string &outError)
{
    ExtractPaddedRgbTile(input, tc, buf.tileIn);
    ggml_backend_tensor_set(buf.runner.tensors.in, buf.tileIn.data(), 0u,
                            buf.tileIn.size() * sizeof(float));
    if (ggml_backend_graph_compute(buf.backend.Get(), buf.runner.graph) != GGML_STATUS_SUCCESS)
    {
        outError = "ggml_backend_graph_compute failed during RealPLKSR.";
        return false;
    }
    ggml_backend_tensor_get(buf.runner.tensors.feat, buf.featData.data(), 0u,
                            buf.featData.size() * sizeof(float));
    if (buf.cfg.dysample)
    {
        ggml_backend_tensor_get(buf.runner.tensors.offset, buf.offData.data(), 0u,
                                buf.offData.size() * sizeof(float));
        RunDySampleAndEndConvCpu(buf.cfg, buf.endConv, buf.featData, buf.offData, buf.tileW,
                                 buf.tileH, buf.tileOutRgb);
    }
    else
    {
        RunPixelShuffleCpu(buf.cfg.scale, buf.featData, buf.tileW, buf.tileH, buf.tileOutRgb);
    }
    return true;
}

bool RunRealPlksrTiledLoop(const EditorAiTensorNchw &input, const WorkerRunOptions &opts,
                           RealPlksrDispatchBuffers &buf, WorkerRunResult &result)
{
    const uint32_t outW = static_cast<uint32_t>(input.width) * buf.cfg.scale;
    const uint32_t outH = static_cast<uint32_t>(input.height) * buf.cfg.scale;
    const size_t outPlane = static_cast<size_t>(outW) * outH;
    result.output.channels = 3u;
    result.output.width = outW;
    result.output.height = outH;
    result.output.data.assign(outPlane * 3u, 0.0f);
    std::vector<float> weightSum(outPlane, 0.0f);
    const std::vector<float> tileWeights =
        BuildOutputFeatherWeights(buf.tileW * buf.cfg.scale, buf.tileH * buf.cfg.scale,
                                  buf.cfg.scale);

    const std::vector<int32_t> xs =
        ComputeRealPlksrTileOrigins(static_cast<uint32_t>(input.width), buf.tileW);
    const std::vector<int32_t> ys =
        ComputeRealPlksrTileOrigins(static_cast<uint32_t>(input.height), buf.tileH);
    const uint32_t totalTiles = static_cast<uint32_t>(xs.size() * ys.size());
    uint32_t tileDone = 0u;

    for (int32_t y0 : ys)
    {
        for (int32_t x0 : xs)
        {
            const RealPlksrTileCoord tc = { x0, y0, buf.tileW, buf.tileH, opts.clampEdge };
            if (!DispatchRealPlksrTile(input, tc, buf, result.error))
            {
                return false;
            }
            AccumulateFeatheredUpscaledTile(buf.tileOutRgb, tileWeights, tc, buf.cfg.scale,
                                            result.output, weightSum);
            tileDone++;
            if (opts.onProgress)
            {
                char msg[96] = {};
                std::snprintf(msg, sizeof(msg), "RealPLKSR tile %u/%u", tileDone, totalTiles);
                opts.onProgress(10 + static_cast<int32_t>(tileDone * 85u / totalTiles), msg);
            }
        }
    }

    NormalizeAndClampOutput(weightSum, result.output);
    return true;
}

bool RunRealPlksrSingleTileDispatch(const EditorAiTensorNchw &input, const WorkerRunOptions &opts,
                                    RealPlksrDispatchBuffers &buf, WorkerRunResult &result)
{
    if (opts.onProgress)
    {
        opts.onProgress(15, "Upscaling texture (RealPLKSR)");
    }
    const RealPlksrTileCoord tc = {
        -static_cast<int32_t>(kTileHalo), -static_cast<int32_t>(kTileHalo), buf.tileW, buf.tileH,
        opts.clampEdge,
    };
    if (!DispatchRealPlksrTile(input, tc, buf, result.error))
    {
        return false;
    }
    const uint32_t outW = static_cast<uint32_t>(input.width) * buf.cfg.scale;
    const uint32_t outH = static_cast<uint32_t>(input.height) * buf.cfg.scale;
    CropSingleTileToOutput(buf.tileOutRgb, buf.tileW * buf.cfg.scale, buf.tileH * buf.cfg.scale,
                           buf.cfg.scale, outW, outH, result.output);
    if (opts.onProgress)
    {
        opts.onProgress(95, "Upscale complete");
    }
    return true;
}

} // namespace

bool RunRealPlksrGgufPipeline(const GgufModelFile &model, const WorkerVulkanDevice &vkDev,
                              const WorkerRunOptions &opts, const EditorAiTensorNchw &input,
                              WorkerRunResult &result)
{
    if (input.channels != 3u || input.width == 0u || input.height == 0u)
    {
        result.error = "RealPLKSR expects 3-channel float32 NCHW RGB input.";
        return false;
    }

    const RealPlksrConfig cfg = ReadRealPlksrConfig(model);
    if (cfg.dysample && ((3u * cfg.scale * cfg.scale) % cfg.dysampleGroups != 0u))
    {
        result.error = "Invalid RealPLKSR DySample group count.";
        return false;
    }

    const uint32_t needW = static_cast<uint32_t>(input.width) + 2u * kTileHalo;
    const uint32_t needH = static_cast<uint32_t>(input.height) + 2u * kTileHalo;
    WorkerBackendHandle backend;
    uint32_t tileMax = 128u;
    const RealPlksrBackendQuery q{ model, vkDev, opts, cfg, needW, needH };
    if (!SelectRealPlksrBackend(q, backend, tileMax, result.provider, result.error))
    {
        return false;
    }

    GgufStageWeights weights;
    DySampleEndConvParams endConv;
    if (!model.LoadStageWeights({}, backend.Get(), weights, result.error) ||
        (cfg.dysample && !LoadDySampleEndConvParams(weights, endConv, result.error)))
    {
        return false;
    }

    const uint32_t tileW = std::min(needW, tileMax);
    const uint32_t tileH = std::min(needH, tileMax);
    RealPlksrGraphRunner runner;
    if (!runner.Init(weights, cfg, tileW, tileH, backend.BufferType(), result.error))
    {
        return false;
    }

    const size_t tilePlane = static_cast<size_t>(tileW) * tileH;
    RealPlksrDispatchBuffers buf{
        cfg,
        endConv,
        backend,
        runner,
        tileW,
        tileH,
        {},
        std::vector<float>(tilePlane * 3u * cfg.scale * cfg.scale, 0.0f),
        std::vector<float>(
            cfg.dysample ? tilePlane * (2u * cfg.dysampleGroups * cfg.scale * cfg.scale) : 0u,
            0.0f),
        {},
    };

    if ((needW > tileMax) || (needH > tileMax))
    {
        return RunRealPlksrTiledLoop(input, opts, buf, result);
    }
    return RunRealPlksrSingleTileDispatch(input, opts, buf, result);
}

} // namespace qray_ai
