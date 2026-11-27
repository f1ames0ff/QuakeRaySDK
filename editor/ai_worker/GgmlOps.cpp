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

#include "GgmlOps.h"

#include <cmath>

namespace qray_ai
{

ggml_tensor *AddBiasSpatial(ggml_context *ctx, ggml_tensor *x, ggml_tensor *bias)
{
    if (bias == nullptr)
    {
        return x;
    }
    ggml_tensor *b4 = ggml_reshape_4d(ctx, bias, 1, 1, bias->ne[0], 1);
    return ggml_add(ctx, x, b4);
}

ggml_tensor *Conv2d(ggml_context *ctx, ggml_tensor *w, ggml_tensor *b, ggml_tensor *x,
                    const Conv2dParams &params)
{
    if (ggml_n_dims(w) < 4)
    {
        w = ggml_reshape_4d(ctx, w, w->ne[0], w->ne[1], w->ne[2], w->ne[3]);
    }
    if (!ggml_is_contiguous(x))
    {
        x = ggml_cont(ctx, x);
    }
    int pad = params.pad;
    if (params.asymRightBottomPad)
    {
        x = ggml_pad(ctx, x, 1, 1, 0, 0);
        pad = 0;
    }
    ggml_tensor *y = ggml_conv_2d(ctx, w, x, params.stride, params.stride, pad, pad, 1, 1);
    return AddBiasSpatial(ctx, y, b);
}

ggml_tensor *DepthwiseConv2d(ggml_context *ctx, ggml_tensor *w, ggml_tensor *b, ggml_tensor *x,
                             const Conv2dParams &params)
{
    if (ggml_n_dims(w) < 4)
    {
        w = ggml_reshape_4d(ctx, w, w->ne[0], w->ne[1], w->ne[2], w->ne[3]);
    }
    if (!ggml_is_contiguous(x))
    {
        x = ggml_cont(ctx, x);
    }
    ggml_tensor *y = ggml_conv_2d_dw_direct(ctx, w, x, params.stride, params.stride, params.pad,
                                            params.pad, 1, 1);
    return AddBiasSpatial(ctx, y, b);
}

ggml_tensor *Linear(ggml_context *ctx, ggml_tensor *w, ggml_tensor *b, ggml_tensor *x)
{
    if (!ggml_is_contiguous(x))
    {
        x = ggml_cont(ctx, x);
    }
    ggml_tensor *y = ggml_mul_mat(ctx, w, x);
    return b != nullptr ? ggml_add(ctx, y, b) : y;
}

ggml_tensor *GroupNorm(ggml_context *ctx, ggml_tensor *w, ggml_tensor *b, ggml_tensor *x,
                       int numGroups, float eps)
{
    if (!ggml_is_contiguous(x))
    {
        x = ggml_cont(ctx, x);
    }
    ggml_tensor *y = ggml_group_norm(ctx, x, numGroups, eps);
    if (w != nullptr)
    {
        y = ggml_mul(ctx, y, ggml_reshape_4d(ctx, w, 1, 1, w->ne[0], 1));
    }
    return AddBiasSpatial(ctx, y, b);
}

ggml_tensor *GroupNorm32(ggml_context *ctx, ggml_tensor *w, ggml_tensor *b, ggml_tensor *x,
                         float eps)
{
    return GroupNorm(ctx, w, b, x, 32, eps);
}

ggml_tensor *Mish(ggml_context *ctx, ggml_tensor *x)
{
    if (!ggml_is_contiguous(x))
    {
        x = ggml_cont(ctx, x);
    }
    return ggml_mul(ctx, x, ggml_tanh(ctx, ggml_softplus(ctx, x)));
}

ggml_tensor *LayerNorm(ggml_context *ctx, ggml_tensor *w, ggml_tensor *b, ggml_tensor *x, float eps)
{
    if (!ggml_is_contiguous(x))
    {
        x = ggml_cont(ctx, x);
    }
    ggml_tensor *y = ggml_norm(ctx, x, eps);
    if (w != nullptr)
    {
        y = ggml_mul(ctx, y, w);
    }
    return b != nullptr ? ggml_add(ctx, y, b) : y;
}

ggml_tensor *SpatialToTokens(ggml_context *ctx, ggml_tensor *x)
{
    const int64_t wh = x->ne[0] * x->ne[1];
    const int64_t c = x->ne[2];
    ggml_tensor *flat = ggml_reshape_2d(ctx, ggml_is_contiguous(x) ? x : ggml_cont(ctx, x), wh, c);
    return ggml_cont(ctx, ggml_transpose(ctx, flat));
}

ggml_tensor *TokensToSpatial(ggml_context *ctx, ggml_tensor *x, int64_t width, int64_t height)
{
    const int64_t c = x->ne[0];
    ggml_tensor *tr = ggml_cont(ctx, ggml_transpose(ctx, x));
    return ggml_reshape_4d(ctx, tr, width, height, c, 1);
}

ggml_tensor *ExactMultiHeadAttention(ggml_context *ctx, ggml_tensor *q, ggml_tensor *k,
                                     ggml_tensor *v, int64_t heads)
{
    const int64_t dInner = q->ne[0];
    const int64_t dHead = dInner / heads;
    const int64_t seqQ = q->ne[1];
    const int64_t seqKv = k->ne[1];

    ggml_tensor *qHeads = ggml_cont(
        ctx, ggml_permute(ctx, ggml_reshape_3d(ctx, q, dHead, heads, seqQ), 0, 2, 1, 3));
    ggml_tensor *kHeads = ggml_cont(
        ctx, ggml_permute(ctx, ggml_reshape_3d(ctx, k, dHead, heads, seqKv), 0, 2, 1, 3));
    const float scale = 1.0f / std::sqrt(static_cast<float>(dHead));

    if ((dHead % 8) == 0 && dHead >= 8)
    {
        ggml_tensor *vHeads = ggml_cont(
            ctx, ggml_permute(ctx, ggml_reshape_3d(ctx, v, dHead, heads, seqKv), 0, 2, 1, 3));
        ggml_tensor *kF16 = ggml_cast(ctx, kHeads, GGML_TYPE_F16);
        ggml_tensor *vF16 = ggml_cast(ctx, vHeads, GGML_TYPE_F16);
        ggml_tensor *fa = ggml_flash_attn_ext(ctx, qHeads, kF16, vF16, nullptr, scale, 0.0f, 0.0f);
        ggml_prec_set_acc(fa, GGML_PREC_F32);
        return ggml_reshape_2d(ctx, fa, dInner, seqQ);
    }

    ggml_tensor *vHeads =
        ggml_permute(ctx, ggml_reshape_3d(ctx, v, dHead, heads, seqKv), 1, 2, 0, 3);
    ggml_tensor *kq = ggml_mul_mat(ctx, kHeads, qHeads);
    ggml_tensor *attn = ggml_soft_max_ext(ctx, kq, nullptr, scale, 0.0f);
    ggml_tensor *av = ggml_mul_mat(ctx, ggml_cont(ctx, vHeads), attn);

    ggml_tensor *merged = ggml_cont(ctx, ggml_permute(ctx, av, 0, 2, 1, 3));
    return ggml_reshape_2d(ctx, merged, dInner, seqQ);
}

} // namespace qray_ai
