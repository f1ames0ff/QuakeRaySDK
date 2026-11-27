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

#include "LotusPipeline.h"
#include "GgmlOps.h"

#include <ggml-alloc.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace qray_ai
{
namespace
{

constexpr size_t kLotusGraphCap = 24576u;
constexpr size_t kLotusCtxNodes = 40960u;
constexpr uint64_t kStageSafetyMarginBytes = 64ULL << 20;

size_t LotusGraphCtxMem()
{
    return ggml_tensor_overhead() * kLotusCtxNodes +
           ggml_graph_overhead_custom(kLotusGraphCap, false);
}

void ReplaceAllToken(std::string &s, const char *from, const char *to)
{
    const size_t fromLen = std::char_traits<char>::length(from);
    const size_t toLen = std::char_traits<char>::length(to);
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos)
    {
        s.replace(pos, fromLen, to);
        pos += toLen;
    }
}

std::string CompactLotusName(std::string name)
{
    static constexpr std::array<std::pair<const char *, const char *>, 4> kPrefixes = { {
        { "unet.", "u." },
        { "ctrl.", "c." },
        { "vae.encoder.", "ve." },
        { "vae.decoder.", "vd." },
    } };
    for (const auto &p : kPrefixes)
    {
        const size_t len = std::char_traits<char>::length(p.first);
        if (name.compare(0u, len, p.first) == 0)
        {
            name.replace(0u, len, p.second);
            break;
        }
    }
    static constexpr std::array<std::pair<const char *, const char *>, 11> kTokens = { {
        { "down_blocks.", "db." },
        { "up_blocks.", "ub." },
        { "mid_block.", "mb." },
        { ".attentions.", ".at." },
        { ".transformer_blocks.", ".tb." },
        { ".resnets.", ".rn." },
        { ".downsamplers.", ".ds." },
        { ".upsamplers.", ".us." },
        { ".time_emb_proj.", ".tp." },
        { ".conv_shortcut.", ".cs." },
        { ".group_norm.", ".gn." },
    } };
    for (const auto &t : kTokens)
    {
        ReplaceAllToken(name, t.first, t.second);
    }
    if (name.ends_with(".weight"))
    {
        name.replace(name.size() - 7u, 7u, ".w");
    }
    else if (name.ends_with(".bias"))
    {
        name.replace(name.size() - 5u, 5u, ".b");
    }
    return name;
}

struct LotusBuildCtx
{
    ggml_context *ctx = nullptr;
    const GgufStageWeights &w;
    std::string &err;

    ggml_tensor *Require(const std::string &name) const
    {
        return w.Require(CompactLotusName(name), err);
    }

    ggml_tensor *Find(const std::string &name) const
    {
        return w.Find(CompactLotusName(name));
    }
};

ggml_tensor *BuildConv(const LotusBuildCtx &b, const std::string &prefix, ggml_tensor *x,
                       int stride, bool asymPad = false)
{
    ggml_tensor *kw = b.Require(prefix + ".weight");
    ggml_tensor *kb = b.Find(prefix + ".bias");
    if (kw == nullptr)
    {
        return nullptr;
    }
    const int pad = asymPad ? 0 : (kw->ne[0] >= 3 ? 1 : 0);
    return Conv2d(b.ctx, kw, kb, x, { stride, pad, asymPad });
}

ggml_tensor *BuildGroupNormPrefix(const LotusBuildCtx &b, const std::string &prefix,
                                  ggml_tensor *x, float eps)
{
    ggml_tensor *nw = b.Require(prefix + ".weight");
    ggml_tensor *nb = b.Find(prefix + ".bias");
    return nw != nullptr ? GroupNorm32(b.ctx, nw, nb, x, eps) : nullptr;
}

ggml_tensor *BuildLayerNormPrefix(const LotusBuildCtx &b, const std::string &prefix,
                                  ggml_tensor *x, float eps)
{
    ggml_tensor *nw = b.Require(prefix + ".weight");
    ggml_tensor *nb = b.Find(prefix + ".bias");
    return nw != nullptr ? LayerNorm(b.ctx, nw, nb, x, eps) : nullptr;
}

ggml_tensor *BuildLinearPrefix(const LotusBuildCtx &b, const std::string &prefix, ggml_tensor *x)
{
    ggml_tensor *lw = b.Require(prefix + ".weight");
    ggml_tensor *lb = b.Find(prefix + ".bias");
    return lw != nullptr ? Linear(b.ctx, lw, lb, x) : nullptr;
}

struct ResNetInput
{
    ggml_tensor *x = nullptr;
    ggml_tensor *embAct = nullptr;
    float normEps = 1e-6f;
};

ggml_tensor *BuildResNet(const LotusBuildCtx &b, const std::string &prefix, const ResNetInput &in)
{
    ggml_tensor *h = BuildGroupNormPrefix(b, prefix + ".norm1", in.x, in.normEps);
    if (h == nullptr)
    {
        return nullptr;
    }
    h = BuildConv(b, prefix + ".conv1", ggml_silu(b.ctx, h), 1);
    if (h == nullptr)
    {
        return nullptr;
    }
    if (in.embAct != nullptr)
    {
        ggml_tensor *tProj = BuildLinearPrefix(b, prefix + ".time_emb_proj", in.embAct);
        if (tProj == nullptr)
        {
            return nullptr;
        }
        h = ggml_add(b.ctx, h, ggml_reshape_4d(b.ctx, tProj, 1, 1, tProj->ne[0], 1));
    }
    h = BuildGroupNormPrefix(b, prefix + ".norm2", h, in.normEps);
    if (h == nullptr)
    {
        return nullptr;
    }
    h = BuildConv(b, prefix + ".conv2", ggml_silu(b.ctx, h), 1);
    if (h == nullptr)
    {
        return nullptr;
    }
    ggml_tensor *skip = b.Find(prefix + ".conv_shortcut.weight") != nullptr
                            ? BuildConv(b, prefix + ".conv_shortcut", in.x, 1)
                            : in.x;
    return skip != nullptr ? ggml_add(b.ctx, skip, h) : nullptr;
}

ggml_tensor *BuildVaeMidAttention(const LotusBuildCtx &b, const std::string &prefix,
                                  ggml_tensor *x, float normEps)
{
    ggml_tensor *normed = BuildGroupNormPrefix(b, prefix + ".group_norm", x, normEps);
    if (normed == nullptr)
    {
        return nullptr;
    }
    ggml_tensor *tok = SpatialToTokens(b.ctx, normed);
    ggml_tensor *q = BuildLinearPrefix(b, prefix + ".to_q", tok);
    ggml_tensor *k = BuildLinearPrefix(b, prefix + ".to_k", tok);
    ggml_tensor *v = BuildLinearPrefix(b, prefix + ".to_v", tok);
    if (q == nullptr || k == nullptr || v == nullptr)
    {
        return nullptr;
    }
    ggml_tensor *attn = ExactMultiHeadAttention(b.ctx, q, k, v, 1);
    ggml_tensor *proj = BuildLinearPrefix(b, prefix + ".to_out.0", attn);
    if (proj == nullptr)
    {
        return nullptr;
    }
    return ggml_add(b.ctx, x, TokensToSpatial(b.ctx, proj, x->ne[0], x->ne[1]));
}

ggml_tensor *BuildUnetAttnBlock(const LotusBuildCtx &b, const std::string &prefix, ggml_tensor *x,
                                ggml_tensor *kvInput, int64_t heads)
{
    ggml_tensor *q = BuildLinearPrefix(b, prefix + ".to_q", x);
    ggml_tensor *k = BuildLinearPrefix(b, prefix + ".to_k", kvInput);
    ggml_tensor *v = BuildLinearPrefix(b, prefix + ".to_v", kvInput);
    if (q == nullptr || k == nullptr || v == nullptr)
    {
        return nullptr;
    }
    ggml_tensor *attn = ExactMultiHeadAttention(b.ctx, q, k, v, heads);
    return BuildLinearPrefix(b, prefix + ".to_out.0", attn);
}

ggml_tensor *BuildTransformerFeedForward(const LotusBuildCtx &b, const std::string &tbPrefix,
                                         ggml_tensor *h)
{
    ggml_tensor *n3 = BuildLayerNormPrefix(b, tbPrefix + ".norm3", h, 1e-5f);
    ggml_tensor *ff = n3 != nullptr ? BuildLinearPrefix(b, tbPrefix + ".ff.net.0.proj", n3) : nullptr;
    if (ff == nullptr)
    {
        return nullptr;
    }
    ff = BuildLinearPrefix(b, tbPrefix + ".ff.net.2", ggml_geglu_erf_swapped(b.ctx, ff));
    return ff != nullptr ? ggml_add(b.ctx, h, ff) : nullptr;
}

ggml_tensor *BuildSpatialTransformer(const LotusBuildCtx &b, const std::string &prefix,
                                     ggml_tensor *x, ggml_tensor *context, int64_t heads)
{
    ggml_tensor *h = BuildGroupNormPrefix(b, prefix + ".norm", x, 1e-6f);
    if (h == nullptr)
    {
        return nullptr;
    }
    h = BuildLinearPrefix(b, prefix + ".proj_in", SpatialToTokens(b.ctx, h));
    if (h == nullptr)
    {
        return nullptr;
    }

    const std::string tb = prefix + ".transformer_blocks.0";
    ggml_tensor *n1 = BuildLayerNormPrefix(b, tb + ".norm1", h, 1e-5f);
    ggml_tensor *a1 = n1 != nullptr ? BuildUnetAttnBlock(b, tb + ".attn1", n1, n1, heads) : nullptr;
    if (a1 == nullptr)
    {
        return nullptr;
    }
    h = ggml_add(b.ctx, h, a1);

    ggml_tensor *n2 = BuildLayerNormPrefix(b, tb + ".norm2", h, 1e-5f);
    ggml_tensor *a2 =
        n2 != nullptr ? BuildUnetAttnBlock(b, tb + ".attn2", n2, context, heads) : nullptr;
    if (a2 == nullptr)
    {
        return nullptr;
    }
    h = BuildTransformerFeedForward(b, tb, ggml_add(b.ctx, h, a2));
    if (h == nullptr)
    {
        return nullptr;
    }
    h = BuildLinearPrefix(b, prefix + ".proj_out", h);
    return h != nullptr ? ggml_add(b.ctx, x, TokensToSpatial(b.ctx, h, x->ne[0], x->ne[1]))
                        : nullptr;
}

ggml_tensor *BuildVaeEncoderGraph(const LotusBuildCtx &b, ggml_tensor *x, float eps)
{
    ggml_tensor *h = BuildConv(b, "vae.encoder.conv_in", x, 1);
    for (int i = 0; i < 4 && h != nullptr; i++)
    {
        const std::string base = "vae.encoder.down_blocks." + std::to_string(i);
        h = BuildResNet(b, base + ".resnets.0", { h, nullptr, eps });
        if (h != nullptr)
        {
            h = BuildResNet(b, base + ".resnets.1", { h, nullptr, eps });
        }
        if (h != nullptr && i < 3)
        {
            h = BuildConv(b, base + ".downsamplers.0.conv", h, 2, true);
        }
    }
    if (h != nullptr)
    {
        h = BuildResNet(b, "vae.encoder.mid_block.resnets.0", { h, nullptr, eps });
    }
    if (h != nullptr)
    {
        h = BuildVaeMidAttention(b, "vae.encoder.mid_block.attentions.0", h, eps);
    }
    if (h != nullptr)
    {
        h = BuildResNet(b, "vae.encoder.mid_block.resnets.1", { h, nullptr, eps });
    }
    if (h != nullptr)
    {
        h = BuildGroupNormPrefix(b, "vae.encoder.conv_norm_out", h, eps);
    }
    if (h != nullptr)
    {
        h = BuildConv(b, "vae.encoder.conv_out", ggml_silu(b.ctx, h), 1);
    }
    return h != nullptr ? BuildConv(b, "vae.quant_conv", h, 1) : nullptr;
}

struct UnetPathState
{
    ggml_tensor *h = nullptr;
    ggml_tensor *embAct = nullptr;
    ggml_tensor *context = nullptr;
    float eps = 1e-5f;
    std::vector<ggml_tensor *> skips;
};

ggml_tensor *BuildUnetDownPath(const LotusBuildCtx &b, const std::string &rootPrefix,
                               UnetPathState &st)
{
    static constexpr std::array<int64_t, 4> kDownHeads = { 5, 10, 20, 0 };
    st.skips.push_back(st.h);
    for (int i = 0; i < 4; i++)
    {
        const std::string base = rootPrefix + ".down_blocks." + std::to_string(i);
        for (int j = 0; j < 2; j++)
        {
            st.h = BuildResNet(b, base + ".resnets." + std::to_string(j),
                               { st.h, st.embAct, st.eps });
            if (st.h != nullptr && i < 3)
            {
                st.h = BuildSpatialTransformer(b, base + ".attentions." + std::to_string(j), st.h,
                                               st.context, kDownHeads[static_cast<size_t>(i)]);
            }
            if (st.h == nullptr)
            {
                return nullptr;
            }
            st.skips.push_back(st.h);
        }
        if (i < 3)
        {
            st.h = BuildConv(b, base + ".downsamplers.0.conv", st.h, 2);
            if (st.h == nullptr)
            {
                return nullptr;
            }
            st.skips.push_back(st.h);
        }
    }
    return st.h;
}

ggml_tensor *BuildUnetMidBlock(const LotusBuildCtx &b, const std::string &rootPrefix,
                              ggml_tensor *h, ggml_tensor *embAct, ggml_tensor *context, float eps)
{
    h = BuildResNet(b, rootPrefix + ".mid_block.resnets.0", { h, embAct, eps });
    if (h != nullptr)
    {
        h = BuildSpatialTransformer(b, rootPrefix + ".mid_block.attentions.0", h, context, 20);
    }
    if (h != nullptr)
    {
        h = BuildResNet(b, rootPrefix + ".mid_block.resnets.1", { h, embAct, eps });
    }
    return h;
}

ggml_tensor *BuildUnetUpPath(const LotusBuildCtx &b, UnetPathState &st)
{
    static constexpr std::array<int64_t, 4> kUpHeads = { 0, 20, 10, 5 };
    for (int i = 0; i < 4; i++)
    {
        const std::string base = "unet.up_blocks." + std::to_string(i);
        for (int j = 0; j < 3; j++)
        {
            ggml_tensor *skip = st.skips.back();
            st.skips.pop_back();
            st.h = BuildResNet(b, base + ".resnets." + std::to_string(j),
                               { ggml_concat(b.ctx, st.h, skip, 2), st.embAct, st.eps });
            if (st.h != nullptr && i > 0)
            {
                st.h = BuildSpatialTransformer(b, base + ".attentions." + std::to_string(j), st.h,
                                               st.context, kUpHeads[static_cast<size_t>(i)]);
            }
            if (st.h == nullptr)
            {
                return nullptr;
            }
        }
        if (i < 3)
        {
            st.h = BuildConv(b, base + ".upsamplers.0.conv",
                             ggml_upscale(b.ctx, st.h, 2, GGML_SCALE_MODE_NEAREST), 1);
            if (st.h == nullptr)
            {
                return nullptr;
            }
        }
    }
    return st.h;
}

ggml_tensor *BuildUnetGraph(const LotusBuildCtx &b, ggml_tensor *x, float eps)
{
    ggml_tensor *emb = b.Require("lotus.emb");
    ggml_tensor *context = b.Require("lotus.context");
    if (emb == nullptr || context == nullptr)
    {
        return nullptr;
    }

    UnetPathState st;
    st.embAct = ggml_silu(b.ctx, emb);
    st.context = context;
    st.eps = eps;
    st.skips.reserve(12u);
    st.h = BuildConv(b, "unet.conv_in", x, 1);
    if (st.h == nullptr || BuildUnetDownPath(b, "unet", st) == nullptr)
    {
        return nullptr;
    }
    st.h = BuildUnetMidBlock(b, "unet", st.h, st.embAct, context, eps);
    if (st.h == nullptr || BuildUnetUpPath(b, st) == nullptr)
    {
        return nullptr;
    }
    st.h = BuildGroupNormPrefix(b, "unet.conv_norm_out", st.h, eps);
    return st.h != nullptr ? BuildConv(b, "unet.conv_out", ggml_silu(b.ctx, st.h), 1) : nullptr;
}

ggml_tensor *BuildVaeDecoderGraph(const LotusBuildCtx &b, ggml_tensor *x, float eps)
{
    ggml_tensor *h = BuildConv(b, "vae.post_quant_conv", x, 1);
    if (h != nullptr)
    {
        h = BuildConv(b, "vae.decoder.conv_in", h, 1);
    }
    if (h != nullptr)
    {
        h = BuildResNet(b, "vae.decoder.mid_block.resnets.0", { h, nullptr, eps });
    }
    if (h != nullptr)
    {
        h = BuildVaeMidAttention(b, "vae.decoder.mid_block.attentions.0", h, eps);
    }
    if (h != nullptr)
    {
        h = BuildResNet(b, "vae.decoder.mid_block.resnets.1", { h, nullptr, eps });
    }
    for (int i = 0; i < 4 && h != nullptr; i++)
    {
        const std::string base = "vae.decoder.up_blocks." + std::to_string(i);
        for (int j = 0; j < 3 && h != nullptr; j++)
        {
            h = BuildResNet(b, base + ".resnets." + std::to_string(j), { h, nullptr, eps });
        }
        if (h != nullptr && i < 3)
        {
            h = BuildConv(b, base + ".upsamplers.0.conv",
                          ggml_upscale(b.ctx, h, 2, GGML_SCALE_MODE_NEAREST), 1);
        }
    }
    if (h != nullptr)
    {
        h = BuildGroupNormPrefix(b, "vae.decoder.conv_norm_out", h, eps);
    }
    return h != nullptr ? BuildConv(b, "vae.decoder.conv_out", ggml_silu(b.ctx, h), 1) : nullptr;
}

using StageGraphBuilder = std::function<ggml_tensor *(const LotusBuildCtx &, ggml_tensor *)>;

struct StagePlan
{
    bool useVulkan = false;
    uint32_t tileW = 0;
    uint32_t tileH = 0;
};

struct StageGraphRunner
{
    ggml_context *ctx = nullptr;
    ggml_cgraph *graph = nullptr;
    ggml_gallocr_t alloc = nullptr;
    ggml_tensor *inTensor = nullptr;
    ggml_tensor *outTensor = nullptr;

    ~StageGraphRunner()
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

    bool Init(const GgufStageWeights &weights, uint32_t inCh, const StagePlan &plan,
              const StageGraphBuilder &builder, std::string &err)
    {
        ggml_init_params params = { LotusGraphCtxMem(), nullptr, true };
        ctx = ggml_init(params);
        if (ctx == nullptr)
        {
            err = "Failed creating GGML stage context.";
            return false;
        }
        inTensor = ggml_new_tensor_4d(ctx, GGML_TYPE_F32, plan.tileW, plan.tileH, inCh, 1);
        ggml_set_input(inTensor);
        const LotusBuildCtx b = { ctx, weights, err };
        outTensor = builder(b, inTensor);
        if (outTensor == nullptr)
        {
            return false;
        }
        ggml_set_output(outTensor);
        graph = ggml_new_graph_custom(ctx, kLotusGraphCap, false);
        ggml_build_forward_expand(graph, outTensor);
        return true;
    }

    bool Allocate(ggml_backend_buffer_type_t buft, std::string &err)
    {
        alloc = graph != nullptr ? ggml_gallocr_new(buft) : nullptr;
        if (alloc == nullptr || !ggml_gallocr_alloc_graph(alloc, graph))
        {
            err = "Failed allocating GGML compute graph buffers.";
            return false;
        }
        return true;
    }
};

struct StageSelectConfig
{
    std::vector<std::string> prefixes;
    uint32_t inChannels = 0;
    uint32_t fullW = 0;
    uint32_t fullH = 0;
    std::vector<uint32_t> candidateTiles;
    StageGraphBuilder builder;
};

bool FitsBudgetForTile(const GgufModelFile &model, const GgufStageWeights &dry,
                       const StageSelectConfig &cfg, const StagePlan &cand,
                       const WorkerVramBudget &budget, int vkIdx)
{
    ggml_backend_buffer_type_t buft = WorkerVulkanBufferType(vkIdx);
    const uint64_t weightEst = model.EstimateStageBytes(cfg.prefixes, buft);
    size_t computeEst = 0;
    const auto measureFn = [&](ggml_context *ctx) -> ggml_cgraph * {
        ggml_tensor *inT =
            ggml_new_tensor_4d(ctx, GGML_TYPE_F32, cand.tileW, cand.tileH, cfg.inChannels, 1);
        ggml_set_input(inT);
        std::string dummyErr;
        const LotusBuildCtx b = { ctx, dry, dummyErr };
        ggml_tensor *outT = cfg.builder(b, inT);
        if (outT == nullptr)
        {
            return nullptr;
        }
        ggml_set_output(outT);
        ggml_cgraph *gf = ggml_new_graph_custom(ctx, kLotusGraphCap, false);
        ggml_build_forward_expand(gf, outT);
        return gf;
    };
    if (!MeasureGraphComputeBytes(buft, measureFn, computeEst))
    {
        return false;
    }
    const uint64_t total = weightEst + static_cast<uint64_t>(computeEst) + kStageSafetyMarginBytes;
    return total <= budget.effectiveBudgetBytes;
}

bool SelectStagePlan(const GgufModelFile &model, const WorkerVulkanDevice &vkDev,
                     const WorkerRunOptions &opts, const StageSelectConfig &cfg, StagePlan &outPlan,
                     std::string &outError)
{
    outPlan = {};
    if (!opts.forceCpu && vkDev.index >= 0)
    {
        const WorkerVramBudget budget = ComputeWorkerVramBudget(vkDev.index, opts.vramCapFraction);
        GgufStageWeights dry;
        if (model.CreateDryRunStage(cfg.prefixes, dry, outError))
        {
            const StagePlan fullCand = { true, cfg.fullW, cfg.fullH };
            if (FitsBudgetForTile(model, dry, cfg, fullCand, budget, vkDev.index))
            {
                outPlan = fullCand;
                return true;
            }
            for (uint32_t cand : cfg.candidateTiles)
            {
                const uint32_t tw = std::min(cand, cfg.fullW);
                const uint32_t th = std::min(cand, cfg.fullH);
                if ((tw == cfg.fullW && th == cfg.fullH) || (tw % 8u) != 0u || (th % 8u) != 0u)
                {
                    continue;
                }
                const StagePlan tileCand = { true, tw, th };
                if (FitsBudgetForTile(model, dry, cfg, tileCand, budget, vkDev.index))
                {
                    outPlan = tileCand;
                    return true;
                }
            }
        }
    }
    outPlan.useVulkan = false;
    outPlan.tileW = cfg.fullW;
    outPlan.tileH = cfg.fullH;
    return true;
}

std::vector<uint32_t> ComputeTileStarts(uint32_t totalLen, uint32_t tileLen, uint32_t overlap)
{
    if (totalLen <= tileLen)
    {
        return { 0u };
    }
    const uint32_t stride = std::max(1u, tileLen - overlap);
    std::vector<uint32_t> starts;
    for (uint32_t pos = 0;;)
    {
        starts.push_back(pos);
        if (pos + tileLen >= totalLen)
        {
            break;
        }
        const uint32_t next = std::min(pos + stride, totalLen - tileLen);
        pos = next > pos ? next : totalLen - tileLen;
    }
    return starts;
}

std::vector<float> BuildWeights1D(uint32_t len, uint32_t overlap, uint32_t cut, bool hasLeft,
                                  bool hasRight)
{
    std::vector<float> w(len, 1.0f);
    for (uint32_t i = 0; i < len; i++)
    {
        if (hasLeft)
        {
            if (i < cut)
            {
                w[i] = 0.0f;
            }
            else if (i < overlap)
            {
                w[i] = std::min(w[i], static_cast<float>(i - cut + 1u) /
                                          static_cast<float>(overlap - cut + 1u));
            }
        }
        if (hasRight)
        {
            const uint32_t d = len - 1u - i;
            if (d < cut)
            {
                w[i] = 0.0f;
            }
            else if (d < overlap)
            {
                w[i] = std::min(w[i], static_cast<float>(d - cut + 1u) /
                                          static_cast<float>(overlap - cut + 1u));
            }
        }
    }
    return w;
}

struct TileCopyParams
{
    uint32_t x0 = 0;
    uint32_t y0 = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    float scale = 1.0f;
    float bias = 0.0f;
};

void CopySubTileNchw(const EditorAiTensorNchw &src, const TileCopyParams &cp,
                     std::vector<float> &dst)
{
    dst.resize(static_cast<size_t>(src.channels) * cp.w * cp.h);
    const size_t srcPlane = static_cast<size_t>(src.width) * src.height;
    const size_t dstPlane = static_cast<size_t>(cp.w) * cp.h;
    for (uint32_t c = 0; c < src.channels; c++)
    {
        const float *sPtr = src.data.data() + c * srcPlane;
        float *dPtr = dst.data() + c * dstPlane;
        for (uint32_t dy = 0; dy < cp.h; dy++)
        {
            const float *row = sPtr + static_cast<size_t>(cp.y0 + dy) * src.width + cp.x0;
            for (uint32_t dx = 0; dx < cp.w; dx++)
            {
                dPtr[static_cast<size_t>(dy) * cp.w + dx] = row[dx] * cp.scale + cp.bias;
            }
        }
    }
}

struct TileWriteDesc
{
    uint32_t x0 = 0;
    uint32_t y0 = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t overlap = 0;
    uint32_t cut = 0;
    float scale = 1.0f;
    float bias = 0.0f;
};

void AccumulateTileNchw(const std::vector<float> &src, const TileWriteDesc &td,
                        EditorAiTensorNchw &dst, std::vector<float> &weightSum)
{
    const std::vector<float> wx =
        BuildWeights1D(td.w, td.overlap, td.cut, td.x0 > 0u, td.x0 + td.w < dst.width);
    const std::vector<float> wy =
        BuildWeights1D(td.h, td.overlap, td.cut, td.y0 > 0u, td.y0 + td.h < dst.height);
    const size_t srcPlane = static_cast<size_t>(td.w) * td.h;
    const size_t dstPlane = static_cast<size_t>(dst.width) * dst.height;

    for (uint32_t dy = 0; dy < td.h; dy++)
    {
        for (uint32_t dx = 0; dx < td.w; dx++)
        {
            const float weight = wx[dx] * wy[dy];
            if (weight <= 0.0f)
            {
                continue;
            }
            const size_t sIdx = static_cast<size_t>(dy) * td.w + dx;
            const size_t dIdx = static_cast<size_t>(td.y0 + dy) * dst.width + (td.x0 + dx);
            weightSum[dIdx] += weight;
            for (uint32_t c = 0; c < dst.channels; c++)
            {
                const float v = src[c * srcPlane + sIdx] * td.scale + td.bias;
                dst.data[c * dstPlane + dIdx] += v * weight;
            }
        }
    }
}

void FinalizeAccumulatedNchw(const std::vector<float> &weightSum, bool clamp01,
                             EditorAiTensorNchw &dst)
{
    const size_t plane = static_cast<size_t>(dst.width) * dst.height;
    for (size_t i = 0; i < plane; i++)
    {
        const float w = weightSum[i];
        const float inv = w > 1e-6f ? (1.0f / w) : 0.0f;
        for (uint32_t c = 0; c < dst.channels; c++)
        {
            float v = dst.data[c * plane + i] * inv;
            if (clamp01)
            {
                v = std::clamp(v, 0.0f, 1.0f);
            }
            dst.data[c * plane + i] = v;
        }
    }
}

struct LotusStageRunDesc
{
    const char *stageName = "";
    StageSelectConfig selectCfg;
    uint32_t outChannels = 0;
    uint32_t inScaleShift = 0;
    uint32_t outScaleMul = 1;
    uint32_t inOverlap = 0;
    uint32_t outOverlap = 0;
    uint32_t outCut = 0;
    float inScale = 1.0f;
    float inBias = 0.0f;
    float outScale = 1.0f;
    float outBias = 0.0f;
    bool clampOut01 = false;
    int32_t progStart = 0;
    int32_t progEnd = 100;
};

struct StageExecutionContext
{
    const WorkerBackendHandle &backend;
    const StagePlan &plan;
    const LotusStageRunDesc &desc;
    StageGraphRunner &runner;
};

bool InitStageBackend(const WorkerVulkanDevice &vkDev, const WorkerRunOptions &opts,
                      const StagePlan &plan, WorkerBackendHandle &backend, std::string &outLabel,
                      std::string &outError)
{
    if (plan.useVulkan && backend.InitVulkan(vkDev.index))
    {
        outLabel = backend.Label();
        return true;
    }
    if (!backend.InitCpu())
    {
        outError = "Failed initializing GGML CPU fallback backend.";
        return false;
    }
    outLabel = (!opts.forceCpu && vkDev.index >= 0) ? "ggml-cpu (VRAM cap)" : "ggml-cpu";
    return true;
}

bool RunStageTileGrid(const StageExecutionContext &exec, const EditorAiTensorNchw &stageIn,
                      EditorAiTensorNchw &stageOut, std::vector<float> &weightSum,
                      const WorkerProgressCallback &onProgress, std::string &outError)
{
    const std::vector<uint32_t> xs =
        ComputeTileStarts(stageIn.width, exec.plan.tileW, exec.desc.inOverlap);
    const std::vector<uint32_t> ys =
        ComputeTileStarts(stageIn.height, exec.plan.tileH, exec.desc.inOverlap);
    const size_t totalTiles = xs.size() * ys.size();
    std::vector<float> tileIn;
    std::vector<float> tileOut(ggml_nelements(exec.runner.outTensor), 0.0f);
    size_t doneTiles = 0;

    for (uint32_t y0 : ys)
    {
        for (uint32_t x0 : xs)
        {
            const TileCopyParams cp = { x0,          y0,          exec.plan.tileW,
                                        exec.plan.tileH, exec.desc.inScale, exec.desc.inBias };
            CopySubTileNchw(stageIn, cp, tileIn);
            ggml_backend_tensor_set(exec.runner.inTensor, tileIn.data(), 0u,
                                    tileIn.size() * sizeof(float));
            if (ggml_backend_graph_compute(exec.backend.Get(), exec.runner.graph) !=
                GGML_STATUS_SUCCESS)
            {
                outError = std::string("ggml_backend_graph_compute failed in ") +
                           exec.desc.stageName;
                return false;
            }
            ggml_backend_tensor_get(exec.runner.outTensor, tileOut.data(), 0u,
                                    tileOut.size() * sizeof(float));
            const TileWriteDesc wd = {
                (x0 >> exec.desc.inScaleShift) * exec.desc.outScaleMul,
                (y0 >> exec.desc.inScaleShift) * exec.desc.outScaleMul,
                (exec.plan.tileW >> exec.desc.inScaleShift) * exec.desc.outScaleMul,
                (exec.plan.tileH >> exec.desc.inScaleShift) * exec.desc.outScaleMul,
                exec.desc.outOverlap,
                exec.desc.outCut,
                exec.desc.outScale,
                exec.desc.outBias,
            };
            AccumulateTileNchw(tileOut, wd, stageOut, weightSum);
            doneTiles++;
            if (onProgress)
            {
                char msg[96] = {};
                std::snprintf(msg, sizeof(msg), "%s tile %zu/%zu", exec.desc.stageName, doneTiles,
                              totalTiles);
                const int32_t span = exec.desc.progEnd - exec.desc.progStart;
                onProgress(exec.desc.progStart +
                               static_cast<int32_t>(doneTiles * static_cast<size_t>(span) /
                                                    totalTiles),
                           msg);
            }
        }
    }
    return true;
}

struct LotusStageRequest
{
    const GgufModelFile &model;
    const WorkerVulkanDevice &vkDev;
    const WorkerRunOptions &opts;
    const LotusStageRunDesc &desc;
};

bool ExecuteLotusStage(const LotusStageRequest &req, const EditorAiTensorNchw &stageIn,
                       EditorAiTensorNchw &stageOut, std::string &outProvider,
                       std::string &outError)
{
    StagePlan plan;
    if (!SelectStagePlan(req.model, req.vkDev, req.opts, req.desc.selectCfg, plan, outError))
    {
        return false;
    }
    WorkerBackendHandle backend;
    if (!InitStageBackend(req.vkDev, req.opts, plan, backend, outProvider, outError))
    {
        return false;
    }
    if (req.opts.onProgress)
    {
        const bool isFull =
            plan.tileW == req.desc.selectCfg.fullW && plan.tileH == req.desc.selectCfg.fullH;
        const std::string tag = isFull ? (std::string(req.desc.stageName) + " (full)")
                                       : (std::string(req.desc.stageName) + " (" +
                                          std::to_string(plan.tileW) + "x" +
                                          std::to_string(plan.tileH) + ")");
        req.opts.onProgress(req.desc.progStart, tag);
    }

    GgufStageWeights weights;
    if (!req.model.LoadStageWeights(req.desc.selectCfg.prefixes, backend.Get(), weights, outError))
    {
        return false;
    }
    StageGraphRunner runner;
    if (!runner.Init(weights, req.desc.selectCfg.inChannels, plan, req.desc.selectCfg.builder,
                     outError) ||
        !runner.Allocate(backend.BufferType(), outError))
    {
        return false;
    }

    const uint32_t fullOutW =
        (req.desc.selectCfg.fullW >> req.desc.inScaleShift) * req.desc.outScaleMul;
    const uint32_t fullOutH =
        (req.desc.selectCfg.fullH >> req.desc.inScaleShift) * req.desc.outScaleMul;
    stageOut.channels = req.desc.outChannels;
    stageOut.width = fullOutW;
    stageOut.height = fullOutH;
    const size_t outPlane = static_cast<size_t>(fullOutW) * fullOutH;
    stageOut.data.assign(outPlane * req.desc.outChannels, 0.0f);
    std::vector<float> weightSum(outPlane, 0.0f);

    const StageExecutionContext exec = { backend, plan, req.desc, runner };
    if (!RunStageTileGrid(exec, stageIn, stageOut, weightSum, req.opts.onProgress, outError))
    {
        return false;
    }
    FinalizeAccumulatedNchw(weightSum, req.desc.clampOut01, stageOut);
    return true;
}

LotusStageRunDesc MakeEncodeStageDesc(uint32_t width, uint32_t height, float vaeEps,
                                      float scaleFactor)
{
    return {
        "VAE Encode",
        { { "ve.", "vae.quant_conv." },
          3u,
          width,
          height,
          { 768u, 640u, 512u, 384u, 256u },
          [vaeEps](const LotusBuildCtx &b, ggml_tensor *x) {
              return BuildVaeEncoderGraph(b, x, vaeEps);
          } },
        4u,
        3u,
        1u,
        64u,
        8u,
        2u,
        2.0f,
        -1.0f,
        scaleFactor,
        0.0f,
        false,
        10,
        30,
    };
}

LotusStageRunDesc MakeUnetStageDesc(uint32_t width, uint32_t height, float unetEps)
{
    return {
        "UNet",
        { { "u.", "lotus." },
          4u,
          width,
          height,
          { 128u, 96u, 80u, 64u, 48u, 32u },
          [unetEps](const LotusBuildCtx &b, ggml_tensor *x) {
              return BuildUnetGraph(b, x, unetEps);
          } },
        4u,
        0u,
        1u,
        16u,
        16u,
        4u,
        1.0f,
        0.0f,
        1.0f,
        0.0f,
        false,
        35,
        75,
    };
}

LotusStageRunDesc MakeDecodeStageDesc(uint32_t width, uint32_t height, float vaeEps,
                                      float scaleFactor)
{
    return {
        "VAE Decode",
        { { "vd.", "vae.post_quant_conv." },
          4u,
          width,
          height,
          { 96u, 80u, 64u, 48u, 32u },
          [vaeEps](const LotusBuildCtx &b, ggml_tensor *x) {
              return BuildVaeDecoderGraph(b, x, vaeEps);
          } },
        3u,
        0u,
        8u,
        8u,
        64u,
        16u,
        1.0f / scaleFactor,
        0.0f,
        0.5f,
        0.5f,
        true,
        80,
        96,
    };
}

} // namespace

bool RunLotusGgufPipeline(const GgufModelFile &model, const WorkerVulkanDevice &vkDev,
                          const WorkerRunOptions &opts, const EditorAiTensorNchw &input,
                          WorkerRunResult &result)
{
    if (input.channels != 3u || input.width < 64u || input.height < 64u ||
        (input.width % 64u) != 0u || (input.height % 64u) != 0u)
    {
        result.error = "Lotus expects 3-channel float32 NCHW RGB aligned to 64 pixels.";
        return false;
    }

    const float vaeEps = model.GetMetaF32("lotus.vae_norm_eps", 1e-6f);
    const float unetEps = model.GetMetaF32("lotus.unet_norm_eps", 1e-5f);
    const float scaleFactor = model.GetMetaF32("lotus.vae_scale_factor", 0.18215f);

    EditorAiTensorNchw latent;
    std::string encProv;
    const LotusStageRunDesc encDesc =
        MakeEncodeStageDesc(input.width, input.height, vaeEps, scaleFactor);
    if (!ExecuteLotusStage({ model, vkDev, opts, encDesc }, input, latent, encProv, result.error))
    {
        return false;
    }

    EditorAiTensorNchw denoised;
    std::string unetProv;
    const LotusStageRunDesc unetDesc = MakeUnetStageDesc(latent.width, latent.height, unetEps);
    if (!ExecuteLotusStage({ model, vkDev, opts, unetDesc }, latent, denoised, unetProv,
                           result.error))
    {
        return false;
    }
    latent = {};

    std::string decProv;
    const LotusStageRunDesc decDesc =
        MakeDecodeStageDesc(denoised.width, denoised.height, vaeEps, scaleFactor);
    if (!ExecuteLotusStage({ model, vkDev, opts, decDesc }, denoised, result.output, decProv,
                           result.error))
    {
        return false;
    }

    result.provider = !unetProv.empty() ? unetProv : (!decProv.empty() ? decProv : encProv);
    return true;
}

namespace
{

constexpr size_t kDelightResidualCount = 13u;

struct DelightResidualSpec
{
    uint32_t channels = 0u;
    uint32_t downShift = 0u;
};

constexpr std::array<DelightResidualSpec, kDelightResidualCount> kDelightResidualSpecs = { {
    { 320u, 0u },
    { 320u, 0u },
    { 320u, 0u },
    { 320u, 1u },
    { 640u, 1u },
    { 640u, 1u },
    { 640u, 2u },
    { 1280u, 2u },
    { 1280u, 2u },
    { 1280u, 3u },
    { 1280u, 3u },
    { 1280u, 3u },
    { 1280u, 3u },
} };

using DelightTileResiduals = std::array<std::vector<float>, kDelightResidualCount>;

struct DelightPlan
{
    bool ctrlVulkan = false;
    bool unetVulkan = false;
    uint32_t tileW = 0;
    uint32_t tileH = 0;
};

struct DelightGraphSpec
{
    ggml_context *ctx = nullptr;
    const GgufStageWeights &weights;
    float unetEps = 1e-5f;
    uint32_t tileW = 0;
    uint32_t tileH = 0;
};

struct DelightBudgetQuery
{
    const GgufModelFile &model;
    const GgufStageWeights &dryWeights;
    const std::vector<std::string> &prefixes;
    float unetEps = 1e-5f;
    uint32_t tw = 0;
    uint32_t th = 0;
    const WorkerVramBudget &budget;
    int vkIdx = -1;
};

struct DelightExecEnv
{
    const GgufModelFile &model;
    const WorkerVulkanDevice &vkDev;
    const WorkerRunOptions &opts;
};

struct DelightStageInputs
{
    const DelightPlan &plan;
    float unetEps = 1e-5f;
    const EditorAiTensorNchw &imageLatent;
    const std::vector<uint32_t> &xs;
    const std::vector<uint32_t> &ys;
};

ggml_cgraph *BuildDelightControlNetCGraph(const DelightGraphSpec &spec, ggml_tensor **outInput,
                                         std::array<ggml_tensor *, kDelightResidualCount> &outRes,
                                         std::string &err)
{
    ggml_context *ctx = spec.ctx;
    ggml_tensor *x = ggml_new_tensor_4d(ctx, GGML_TYPE_F32, spec.tileW, spec.tileH, 4, 1);
    ggml_set_input(x);
    if (outInput != nullptr)
    {
        *outInput = x;
    }

    const LotusBuildCtx b = { ctx, spec.weights, err };
    ggml_tensor *emb = b.Require("delight.ctrl_emb");
    ggml_tensor *context = b.Require("delight.context");
    if (emb == nullptr || context == nullptr)
    {
        return nullptr;
    }

    UnetPathState st;
    st.embAct = ggml_silu(ctx, emb);
    st.context = context;
    st.eps = spec.unetEps;
    st.skips.reserve(12u);
    st.h = BuildConv(b, "ctrl.conv_in", x, 1);
    if (st.h == nullptr || BuildUnetDownPath(b, "ctrl", st) == nullptr || st.skips.size() != 12u)
    {
        return nullptr;
    }
    st.h = BuildUnetMidBlock(b, "ctrl", st.h, st.embAct, context, spec.unetEps);
    if (st.h == nullptr)
    {
        return nullptr;
    }

    ggml_cgraph *graph = ggml_new_graph_custom(ctx, kLotusGraphCap, false);
    for (size_t i = 0; i < 12u; i++)
    {
        outRes[i] = st.skips[i];
        ggml_set_output(outRes[i]);
        ggml_build_forward_expand(graph, outRes[i]);
    }
    outRes[12] = st.h;
    ggml_set_output(outRes[12]);
    ggml_build_forward_expand(graph, outRes[12]);
    return graph;
}

ggml_cgraph *BuildDelightUnetCGraph(const DelightGraphSpec &spec, ggml_tensor **outInput,
                                   std::array<ggml_tensor *, kDelightResidualCount> &outResIn,
                                   ggml_tensor **outOutput, std::string &err)
{
    ggml_context *ctx = spec.ctx;
    ggml_tensor *x = ggml_new_tensor_4d(ctx, GGML_TYPE_F32, spec.tileW, spec.tileH, 4, 1);
    ggml_set_input(x);
    if (outInput != nullptr)
    {
        *outInput = x;
    }
    for (size_t i = 0; i < kDelightResidualCount; i++)
    {
        const DelightResidualSpec &resSpec = kDelightResidualSpecs[i];
        outResIn[i] = ggml_new_tensor_4d(ctx, GGML_TYPE_F32, spec.tileW >> resSpec.downShift,
                                         spec.tileH >> resSpec.downShift, resSpec.channels, 1);
        ggml_set_input(outResIn[i]);
    }

    const LotusBuildCtx b = { ctx, spec.weights, err };
    ggml_tensor *emb = b.Require("delight.unet_emb");
    ggml_tensor *context = b.Require("delight.context");
    if (emb == nullptr || context == nullptr)
    {
        return nullptr;
    }

    UnetPathState st;
    st.embAct = ggml_silu(ctx, emb);
    st.context = context;
    st.eps = spec.unetEps;
    st.skips.reserve(12u);
    st.h = BuildConv(b, "unet.conv_in", x, 1);
    if (st.h == nullptr || BuildUnetDownPath(b, "unet", st) == nullptr || st.skips.size() != 12u)
    {
        return nullptr;
    }
    for (size_t i = 0; i < 12u; i++)
    {
        st.skips[i] = ggml_add(ctx, st.skips[i], outResIn[i]);
    }
    st.h = BuildUnetMidBlock(b, "unet", st.h, st.embAct, context, spec.unetEps);
    if (st.h == nullptr)
    {
        return nullptr;
    }
    st.h = ggml_add(ctx, st.h, outResIn[12]);
    if (BuildUnetUpPath(b, st) == nullptr)
    {
        return nullptr;
    }
    st.h = BuildGroupNormPrefix(b, "unet.conv_norm_out", st.h, spec.unetEps);
    ggml_tensor *out =
        st.h != nullptr ? BuildConv(b, "unet.conv_out", ggml_silu(ctx, st.h), 1) : nullptr;
    if (out == nullptr)
    {
        return nullptr;
    }
    ggml_set_output(out);
    if (outOutput != nullptr)
    {
        *outOutput = out;
    }
    ggml_cgraph *graph = ggml_new_graph_custom(ctx, kLotusGraphCap, false);
    ggml_build_forward_expand(graph, out);
    return graph;
}

bool FitsDelightCtrlBudget(const DelightBudgetQuery &q)
{
    ggml_backend_buffer_type_t buft = WorkerVulkanBufferType(q.vkIdx);
    const uint64_t weightEst = q.model.EstimateStageBytes(q.prefixes, buft);
    size_t computeEst = 0;
    const auto measureFn = [&](ggml_context *ctx) -> ggml_cgraph * {
        ggml_tensor *inT = nullptr;
        std::array<ggml_tensor *, kDelightResidualCount> outRes{};
        std::string dummyErr;
        return BuildDelightControlNetCGraph({ ctx, q.dryWeights, q.unetEps, q.tw, q.th }, &inT,
                                            outRes, dummyErr);
    };
    if (!MeasureGraphComputeBytes(buft, measureFn, computeEst))
    {
        return false;
    }
    return weightEst + static_cast<uint64_t>(computeEst) + kStageSafetyMarginBytes <=
           q.budget.effectiveBudgetBytes;
}

bool FitsDelightUnetBudget(const DelightBudgetQuery &q)
{
    ggml_backend_buffer_type_t buft = WorkerVulkanBufferType(q.vkIdx);
    const uint64_t weightEst = q.model.EstimateStageBytes(q.prefixes, buft);
    size_t computeEst = 0;
    const auto measureFn = [&](ggml_context *ctx) -> ggml_cgraph * {
        ggml_tensor *inT = nullptr;
        ggml_tensor *outT = nullptr;
        std::array<ggml_tensor *, kDelightResidualCount> resIn{};
        std::string dummyErr;
        return BuildDelightUnetCGraph({ ctx, q.dryWeights, q.unetEps, q.tw, q.th }, &inT, resIn,
                                      &outT, dummyErr);
    };
    if (!MeasureGraphComputeBytes(buft, measureFn, computeEst))
    {
        return false;
    }
    return weightEst + static_cast<uint64_t>(computeEst) + kStageSafetyMarginBytes <=
           q.budget.effectiveBudgetBytes;
}

bool SelectDelightLatentPlan(const DelightExecEnv &env, float unetEps, uint32_t fullW,
                             uint32_t fullH, DelightPlan &outPlan, std::string &outError)
{
    outPlan = {};
    outPlan.tileW = fullW;
    outPlan.tileH = fullH;
    if (env.opts.forceCpu || env.vkDev.index < 0)
    {
        return true;
    }

    const std::vector<std::string> ctrlPrefixes = { "c.", "delight.ctrl_emb", "delight.context" };
    const std::vector<std::string> unetPrefixes = { "u.", "delight.unet_emb", "delight.context" };
    const WorkerVramBudget budget =
        ComputeWorkerVramBudget(env.vkDev.index, env.opts.vramCapFraction);
    GgufStageWeights dryCtrl;
    GgufStageWeights dryUnet;
    if (!env.model.CreateDryRunStage(ctrlPrefixes, dryCtrl, outError) ||
        !env.model.CreateDryRunStage(unetPrefixes, dryUnet, outError))
    {
        return false;
    }

    const std::vector<uint32_t> candSides = { std::max(fullW, fullH), 128u, 96u, 80u, 64u, 48u, 32u };
    for (size_t idx = 0; idx < candSides.size(); idx++)
    {
        const uint32_t cand = candSides[idx];
        const uint32_t tw = idx == 0 ? fullW : std::min(cand, fullW);
        const uint32_t th = idx == 0 ? fullH : std::min(cand, fullH);
        if ((tw % 8u) != 0u || (th % 8u) != 0u)
        {
            continue;
        }
        if (idx > 0 && tw == fullW && th == fullH)
        {
            continue;
        }
        const DelightBudgetQuery unetQ{ env.model, dryUnet, unetPrefixes, unetEps,
                                        tw,        th,      budget,       env.vkDev.index };
        if (!FitsDelightUnetBudget(unetQ))
        {
            continue;
        }
        outPlan.tileW = tw;
        outPlan.tileH = th;
        outPlan.unetVulkan = true;
        const DelightBudgetQuery ctrlQ{ env.model, dryCtrl, ctrlPrefixes, unetEps,
                                        tw,        th,      budget,       env.vkDev.index };
        outPlan.ctrlVulkan = FitsDelightCtrlBudget(ctrlQ);
        return true;
    }

    const DelightBudgetQuery fallbackCtrlQ{
        env.model, dryCtrl, ctrlPrefixes, unetEps, fullW, fullH, budget, env.vkDev.index,
    };
    outPlan.ctrlVulkan = FitsDelightCtrlBudget(fallbackCtrlQ);
    return true;
}

struct DelightCtrlGraphHandles
{
    WorkerBackendHandle &backend;
    ggml_cgraph *graph = nullptr;
    ggml_tensor *inTensor = nullptr;
    std::array<ggml_tensor *, kDelightResidualCount> resOut{};
};

bool RunDelightControlTileLoop(const DelightExecEnv &env, const DelightStageInputs &in,
                               DelightCtrlGraphHandles &handles,
                               std::vector<DelightTileResiduals> &outResiduals,
                               std::string &outError)
{
    const size_t totalTiles = in.xs.size() * in.ys.size();
    outResiduals.resize(totalTiles);
    std::vector<float> tileIn;
    size_t tileIdx = 0;
    for (uint32_t y0 : in.ys)
    {
        for (uint32_t x0 : in.xs)
        {
            const TileCopyParams cp = { x0, y0, in.plan.tileW, in.plan.tileH, 1.0f, 0.0f };
            CopySubTileNchw(in.imageLatent, cp, tileIn);
            ggml_backend_tensor_set(handles.inTensor, tileIn.data(), 0u,
                                    tileIn.size() * sizeof(float));
            if (ggml_backend_graph_compute(handles.backend.Get(), handles.graph) !=
                GGML_STATUS_SUCCESS)
            {
                outError = "ggml_backend_graph_compute failed in ControlNet";
                return false;
            }
            for (size_t r = 0; r < kDelightResidualCount; r++)
            {
                std::vector<float> &dst = outResiduals[tileIdx][r];
                dst.resize(static_cast<size_t>(ggml_nelements(handles.resOut[r])));
                ggml_backend_tensor_get(handles.resOut[r], dst.data(), 0u,
                                        dst.size() * sizeof(float));
            }
            tileIdx++;
            if (env.opts.onProgress)
            {
                char msg[96] = {};
                std::snprintf(msg, sizeof(msg), "ControlNet tile %zu/%zu", tileIdx, totalTiles);
                env.opts.onProgress(26 + static_cast<int32_t>(tileIdx * 24u / totalTiles), msg);
            }
        }
    }
    return true;
}

bool ExecuteDelightControlStage(const DelightExecEnv &env, const DelightStageInputs &in,
                                std::vector<DelightTileResiduals> &outResiduals,
                                std::string &outProvider, std::string &outError)
{
    const std::vector<std::string> prefixes = { "c.", "delight.ctrl_emb", "delight.context" };
    WorkerBackendHandle backend;
    const StagePlan ctrlPlan = { in.plan.ctrlVulkan, in.plan.tileW, in.plan.tileH };
    if (!InitStageBackend(env.vkDev, env.opts, ctrlPlan, backend, outProvider, outError))
    {
        return false;
    }
    if (env.opts.onProgress)
    {
        const bool isFull =
            in.plan.tileW == in.imageLatent.width && in.plan.tileH == in.imageLatent.height;
        const std::string tag =
            isFull ? "ControlNet (full)"
                   : ("ControlNet (" + std::to_string(in.plan.tileW) + "x" +
                      std::to_string(in.plan.tileH) + ")");
        env.opts.onProgress(26, tag);
    }

    GgufStageWeights weights;
    if (!env.model.LoadStageWeights(prefixes, backend.Get(), weights, outError))
    {
        return false;
    }

    ggml_init_params params = { LotusGraphCtxMem(), nullptr, true };
    ggml_context *ctx = ggml_init(params);
    if (ctx == nullptr)
    {
        outError = "Failed creating GGML ControlNet stage context.";
        return false;
    }

    DelightCtrlGraphHandles handles{ backend };
    const DelightGraphSpec spec{ ctx, weights, in.unetEps, in.plan.tileW, in.plan.tileH };
    handles.graph =
        BuildDelightControlNetCGraph(spec, &handles.inTensor, handles.resOut, outError);
    ggml_gallocr_t alloc =
        handles.graph != nullptr ? ggml_gallocr_new(backend.BufferType()) : nullptr;
    const bool allocOk = alloc != nullptr && ggml_gallocr_alloc_graph(alloc, handles.graph);
    if (!allocOk)
    {
        if (alloc != nullptr)
        {
            ggml_gallocr_free(alloc);
        }
        ggml_free(ctx);
        outError = outError.empty() ? "Failed allocating GGML ControlNet graph buffers." : outError;
        return false;
    }

    const bool ok =
        RunDelightControlTileLoop(env, in, handles, outResiduals, outError);
    ggml_gallocr_free(alloc);
    ggml_free(ctx);
    return ok;
}

struct DelightUnetGraphHandles
{
    ggml_cgraph *graph = nullptr;
    ggml_tensor *inTensor = nullptr;
    ggml_tensor *outTensor = nullptr;
    std::array<ggml_tensor *, kDelightResidualCount> resIn{};
};

bool RunDelightUnetTileLoop(const DelightExecEnv &env, const DelightStageInputs &in,
                            WorkerBackendHandle &backend, const DelightUnetGraphHandles &handles,
                            const std::vector<DelightTileResiduals> &residuals,
                            EditorAiTensorNchw &denoisedOut, std::string &outError)
{
    const size_t totalTiles = in.xs.size() * in.ys.size();
    const std::vector<float> zeroLatent(static_cast<size_t>(in.plan.tileW) * in.plan.tileH * 4u,
                                        0.0f);
    std::vector<float> tileOut(static_cast<size_t>(ggml_nelements(handles.outTensor)), 0.0f);

    denoisedOut.channels = 4u;
    denoisedOut.width = in.imageLatent.width;
    denoisedOut.height = in.imageLatent.height;
    const size_t outPlane = static_cast<size_t>(in.imageLatent.width) * in.imageLatent.height;
    denoisedOut.data.assign(outPlane * 4u, 0.0f);
    std::vector<float> weightSum(outPlane, 0.0f);

    size_t tileIdx = 0;
    for (uint32_t y0 : in.ys)
    {
        for (uint32_t x0 : in.xs)
        {
            ggml_backend_tensor_set(handles.inTensor, zeroLatent.data(), 0u,
                                    zeroLatent.size() * sizeof(float));
            for (size_t r = 0; r < kDelightResidualCount; r++)
            {
                const std::vector<float> &srcRes = residuals[tileIdx][r];
                ggml_backend_tensor_set(handles.resIn[r], srcRes.data(), 0u,
                                        srcRes.size() * sizeof(float));
            }
            if (ggml_backend_graph_compute(backend.Get(), handles.graph) != GGML_STATUS_SUCCESS)
            {
                outError = "ggml_backend_graph_compute failed in Delight UNet";
                return false;
            }
            ggml_backend_tensor_get(handles.outTensor, tileOut.data(), 0u,
                                    tileOut.size() * sizeof(float));
            const TileWriteDesc wd = { x0, y0, in.plan.tileW, in.plan.tileH, 16u, 4u, 1.0f, 0.0f };
            AccumulateTileNchw(tileOut, wd, denoisedOut, weightSum);
            tileIdx++;
            if (env.opts.onProgress)
            {
                char msg[96] = {};
                std::snprintf(msg, sizeof(msg), "UNet tile %zu/%zu", tileIdx, totalTiles);
                env.opts.onProgress(52 + static_cast<int32_t>(tileIdx * 28u / totalTiles), msg);
            }
        }
    }
    FinalizeAccumulatedNchw(weightSum, false, denoisedOut);
    return true;
}

bool ExecuteDelightUnetStage(const DelightExecEnv &env, const DelightStageInputs &in,
                             const std::vector<DelightTileResiduals> &residuals,
                             EditorAiTensorNchw &denoisedOut, std::string &outProvider,
                             std::string &outError)
{
    const std::vector<std::string> prefixes = { "u.", "delight.unet_emb", "delight.context" };
    WorkerBackendHandle backend;
    const StagePlan unetPlan = { in.plan.unetVulkan, in.plan.tileW, in.plan.tileH };
    if (!InitStageBackend(env.vkDev, env.opts, unetPlan, backend, outProvider, outError))
    {
        return false;
    }
    if (env.opts.onProgress)
    {
        const bool isFull =
            in.plan.tileW == in.imageLatent.width && in.plan.tileH == in.imageLatent.height;
        const std::string tag =
            isFull ? "UNet (full)"
                   : ("UNet (" + std::to_string(in.plan.tileW) + "x" +
                      std::to_string(in.plan.tileH) + ")");
        env.opts.onProgress(52, tag);
    }

    GgufStageWeights weights;
    if (!env.model.LoadStageWeights(prefixes, backend.Get(), weights, outError))
    {
        return false;
    }

    ggml_init_params params = { LotusGraphCtxMem(), nullptr, true };
    ggml_context *ctx = ggml_init(params);
    if (ctx == nullptr)
    {
        outError = "Failed creating GGML Delight UNet stage context.";
        return false;
    }

    DelightUnetGraphHandles handles;
    const DelightGraphSpec spec{ ctx, weights, in.unetEps, in.plan.tileW, in.plan.tileH };
    handles.graph = BuildDelightUnetCGraph(spec, &handles.inTensor, handles.resIn,
                                           &handles.outTensor, outError);
    ggml_gallocr_t alloc = handles.graph != nullptr ? ggml_gallocr_new(backend.BufferType()) : nullptr;
    const bool allocOk = alloc != nullptr && ggml_gallocr_alloc_graph(alloc, handles.graph);
    if (!allocOk)
    {
        if (alloc != nullptr)
        {
            ggml_gallocr_free(alloc);
        }
        ggml_free(ctx);
        outError = outError.empty() ? "Failed allocating GGML Delight UNet graph buffers." : outError;
        return false;
    }

    const bool ok =
        RunDelightUnetTileLoop(env, in, backend, handles, residuals, denoisedOut, outError);
    ggml_gallocr_free(alloc);
    ggml_free(ctx);
    return ok;
}

} // namespace

bool RunDelightGgufPipeline(const GgufModelFile &model, const WorkerVulkanDevice &vkDev,
                            const WorkerRunOptions &opts, const EditorAiTensorNchw &input,
                            WorkerRunResult &result)
{
    if (input.channels != 3u || input.width < 64u || input.height < 64u ||
        (input.width % 64u) != 0u || (input.height % 64u) != 0u)
    {
        result.error = "StableDelight expects 3-channel float32 NCHW RGB aligned to 64 pixels.";
        return false;
    }

    const float vaeEps = model.GetMetaF32("delight.vae_norm_eps", 1e-6f);
    const float unetEps = model.GetMetaF32("delight.unet_norm_eps", 1e-5f);
    const float scaleFactor = model.GetMetaF32("delight.vae_scale_factor", 0.18215f);

    EditorAiTensorNchw imageLatent;
    std::string encProv;
    LotusStageRunDesc encDesc = MakeEncodeStageDesc(input.width, input.height, vaeEps, scaleFactor);
    encDesc.progStart = 10;
    encDesc.progEnd = 24;
    if (!ExecuteLotusStage({ model, vkDev, opts, encDesc }, input, imageLatent, encProv,
                           result.error))
    {
        return false;
    }

    const DelightExecEnv env{ model, vkDev, opts };
    DelightPlan plan;
    if (!SelectDelightLatentPlan(env, unetEps, imageLatent.width, imageLatent.height, plan,
                                 result.error))
    {
        return false;
    }
    const std::vector<uint32_t> xs = ComputeTileStarts(imageLatent.width, plan.tileW, 16u);
    const std::vector<uint32_t> ys = ComputeTileStarts(imageLatent.height, plan.tileH, 16u);
    const DelightStageInputs stageIn{ plan, unetEps, imageLatent, xs, ys };

    std::vector<DelightTileResiduals> residuals;
    std::string ctrlProv;
    if (!ExecuteDelightControlStage(env, stageIn, residuals, ctrlProv, result.error))
    {
        return false;
    }

    EditorAiTensorNchw denoised;
    std::string unetProv;
    if (!ExecuteDelightUnetStage(env, stageIn, residuals, denoised, unetProv, result.error))
    {
        return false;
    }
    residuals.clear();
    residuals.shrink_to_fit();
    imageLatent = {};

    std::string decProv;
    LotusStageRunDesc decDesc =
        MakeDecodeStageDesc(denoised.width, denoised.height, vaeEps, scaleFactor);
    decDesc.progStart = 82;
    decDesc.progEnd = 96;
    if (!ExecuteLotusStage({ model, vkDev, opts, decDesc }, denoised, result.output, decProv,
                           result.error))
    {
        return false;
    }

    result.provider =
        !unetProv.empty() ? unetProv
                          : (!ctrlProv.empty() ? ctrlProv : (!decProv.empty() ? decProv : encProv));
    return true;
}

} // namespace qray_ai
