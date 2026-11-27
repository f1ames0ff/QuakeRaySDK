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

#include <ggml.h>

namespace qray_ai
{

struct Conv2dParams
{
    int stride = 1;
    int pad = 0;
    bool asymRightBottomPad = false;
};

ggml_tensor *AddBiasSpatial(ggml_context *ctx, ggml_tensor *x, ggml_tensor *bias);

ggml_tensor *Conv2d(ggml_context *ctx, ggml_tensor *w, ggml_tensor *b, ggml_tensor *x,
                    const Conv2dParams &params = {});

ggml_tensor *DepthwiseConv2d(ggml_context *ctx, ggml_tensor *w, ggml_tensor *b, ggml_tensor *x,
                             const Conv2dParams &params = {});

ggml_tensor *Linear(ggml_context *ctx, ggml_tensor *w, ggml_tensor *b, ggml_tensor *x);

ggml_tensor *GroupNorm(ggml_context *ctx, ggml_tensor *w, ggml_tensor *b, ggml_tensor *x,
                       int numGroups, float eps);

ggml_tensor *GroupNorm32(ggml_context *ctx, ggml_tensor *w, ggml_tensor *b, ggml_tensor *x,
                         float eps);

ggml_tensor *Mish(ggml_context *ctx, ggml_tensor *x);

ggml_tensor *LayerNorm(ggml_context *ctx, ggml_tensor *w, ggml_tensor *b, ggml_tensor *x,
                       float eps);

ggml_tensor *SpatialToTokens(ggml_context *ctx, ggml_tensor *x);

ggml_tensor *TokensToSpatial(ggml_context *ctx, ggml_tensor *x, int64_t width, int64_t height);

// Exact multi-head scaled dot-product attention without Flash Attention.
// q: [d_inner, seq_q], k: [d_inner, seq_kv], v: [d_inner, seq_kv].
ggml_tensor *ExactMultiHeadAttention(ggml_context *ctx, ggml_tensor *q, ggml_tensor *k,
                                     ggml_tensor *v, int64_t heads);

} // namespace qray_ai
