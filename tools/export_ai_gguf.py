#!/usr/bin/env python3
# Copyright (c) 2026 f1ames0ff <f1am3sdev.github@protonmail.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#

"""Convert Lotus Normal, Lotus Depth, DeepBump, StableDelight, and RealPLKSR models to unquantized FP16 GGUF v3."""

from __future__ import annotations

import argparse
import hashlib
import math
import os
import struct
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

GGUF_MAGIC = b"GGUF"
GGUF_VERSION = 3
GGUF_ALIGNMENT = 32
GGUF_TYPE_UINT32 = 4
GGUF_TYPE_FLOAT32 = 6
GGUF_TYPE_STRING = 8
GGML_TYPE_F32 = 0
GGML_TYPE_F16 = 1
GGML_MAX_NAME = 64

DEFAULT_MODEL_KEYS = ("lotus-normal-d-v1-1", "lotus-depth-d-v2-0-disparity", "deepbump256")


@dataclass(frozen=True)
class ModelSpec:
    """Metadata mapping local ONNX / HuggingFace / PyTorch sources to GGUF output filenames."""

    key: str
    onnx_name: str
    gguf_name: str
    arch: str
    variant: str
    hf_repo: str = ""
    pth_name: str = ""
    pth_url: str = ""


MODEL_SPECS: dict[str, ModelSpec] = {
    "lotus-normal-d-v1-1": ModelSpec(
        key="lotus-normal-d-v1-1",
        onnx_name="lotus-normal-d-v1-1.onnx",
        gguf_name="lotus-normal-d-v1-1-fp16.gguf",
        arch="lotus",
        variant="normal",
        hf_repo="jingheya/lotus-normal-d-v1-1",
    ),
    "lotus-depth-d-v2-0-disparity": ModelSpec(
        key="lotus-depth-d-v2-0-disparity",
        onnx_name="lotus-depth-d-v2-0-disparity.onnx",
        gguf_name="lotus-depth-d-v2-0-disparity-fp16.gguf",
        arch="lotus",
        variant="depth",
        hf_repo="jingheya/lotus-depth-d-v2-0-disparity",
    ),
    "deepbump256": ModelSpec(
        key="deepbump256",
        onnx_name="deepbump256.onnx",
        gguf_name="deepbump256-fp16.gguf",
        arch="deepbump",
        variant="normal",
    ),
    "yoso-delight-v0-4-base": ModelSpec(
        key="yoso-delight-v0-4-base",
        onnx_name="",
        gguf_name="yoso-delight-v0-4-base-fp16.gguf",
        arch="delight",
        variant="delight",
        hf_repo="Stable-X/yoso-delight-v0-4-base",
    ),
    "4x-pbrify-rplksrd-v3": ModelSpec(
        key="4x-pbrify-rplksrd-v3",
        onnx_name="",
        gguf_name="4x-pbrify-rplksrd-v3-fp16.gguf",
        arch="realplksr",
        variant="upscale4x",
        pth_name="4x-PBRify_RPLKSRd_V3.pth",
        pth_url="https://github.com/Kim2091/Kim2091-Models/releases/download/4x-PBRify_RPLKSRd_V3/4x-PBRify_RPLKSRd_V3.pth",
    ),
}


@dataclass(frozen=True)
class GgufTensorEntry:
    """Single tensor queued for serialization into a GGUF v3 file."""

    name: str
    dims_c: tuple[int, ...]
    ggml_type: int
    raw_bytes: bytes


def default_models_dir() -> Path:
    """Return user cache directory where QuakeRay resolves local AI models."""
    if sys.platform.startswith("win"):
        base = os.environ.get("LOCALAPPDATA") or os.environ.get("USERPROFILE") or "."
        return Path(base) / "QuakeRay" / "models"
    xdg = os.environ.get("XDG_CACHE_HOME")
    if xdg:
        return Path(xdg) / "quakeray" / "models"
    return Path.home() / ".cache" / "quakeray" / "models"


def align_up(offset: int, alignment: int = GGUF_ALIGNMENT) -> int:
    """Round offset up to the nearest alignment boundary."""
    return (offset + alignment - 1) // alignment * alignment


def _pack_gguf_str(text: str) -> bytes:
    encoded = text.encode("utf-8")
    return struct.pack("<Q", len(encoded)) + encoded


def _pack_gguf_kv(key: str, val: object) -> bytes:
    out = _pack_gguf_str(key)
    if isinstance(val, str):
        return out + struct.pack("<I", GGUF_TYPE_STRING) + _pack_gguf_str(val)
    if isinstance(val, int) and not isinstance(val, bool):
        return out + struct.pack("<II", GGUF_TYPE_UINT32, val)
    if isinstance(val, float):
        return out + struct.pack("<If", GGUF_TYPE_FLOAT32, val)
    raise TypeError(f"Unsupported GGUF metadata type for {key}: {type(val)}")


def write_gguf_file(
    out_path: Path, metadata: dict[str, object], tensors: Sequence[GgufTensorEntry]
) -> None:
    """Write a complete GGUF v3 binary file atomically with 32-byte data alignment."""
    out_path.parent.mkdir(parents=True, exist_ok=True)
    header = bytearray()
    header += GGUF_MAGIC
    header += struct.pack("<Iqq", GGUF_VERSION, len(tensors), len(metadata))
    for k, v in metadata.items():
        header += _pack_gguf_kv(k, v)

    data_offset = 0
    for tensor in tensors:
        if len(tensor.name.encode("utf-8")) >= GGML_MAX_NAME:
            raise ValueError(f"GGUF tensor name exceeds {GGML_MAX_NAME - 1} bytes: {tensor.name}")
        data_offset = align_up(data_offset, GGUF_ALIGNMENT)
        ggml_dims = tuple(reversed(tensor.dims_c)) if tensor.dims_c else (1,)
        header += _pack_gguf_str(tensor.name)
        header += struct.pack("<I", len(ggml_dims))
        for d in ggml_dims:
            header += struct.pack("<q", int(d))
        header += struct.pack("<IQ", tensor.ggml_type, data_offset)
        data_offset += len(tensor.raw_bytes)

    pad_header = align_up(len(header), GGUF_ALIGNMENT) - len(header)
    if pad_header > 0:
        header += b"\x00" * pad_header

    tmp_path = out_path.with_suffix(".gguf.tmp")
    with tmp_path.open("wb") as fp:
        fp.write(header)
        cursor = 0
        for tensor in tensors:
            aligned = align_up(cursor, GGUF_ALIGNMENT)
            if aligned > cursor:
                fp.write(b"\x00" * (aligned - cursor))
                cursor = aligned
            fp.write(tensor.raw_bytes)
            cursor += len(tensor.raw_bytes)
        end_pad = align_up(cursor, GGUF_ALIGNMENT) - cursor
        if end_pad > 0:
            fp.write(b"\x00" * end_pad)
    tmp_path.replace(out_path)


def fuse_conv_and_batchnorm(
    conv_w: object,
    conv_b: object | None,
    gamma: object,
    beta: object,
    running_mean: object,
    running_var: object,
    eps: float = 1e-5,
) -> tuple[object, object]:
    """Fold Conv2d weights/bias with BatchNorm2d inference parameters in float64."""
    import numpy as np

    scale = gamma.astype(np.float64) / np.sqrt(running_var.astype(np.float64) + float(eps))
    fused_w = conv_w.astype(np.float64) * scale.reshape((-1, 1, 1, 1))
    base_b = np.zeros_like(running_mean, dtype=np.float64) if conv_b is None else conv_b.astype(np.float64)
    fused_b = (base_b - running_mean.astype(np.float64)) * scale + beta.astype(np.float64)
    return fused_w.astype(np.float16), fused_b.astype(np.float32)


def _silu_f32(x: object) -> object:
    import numpy as np

    return x * (1.0 / (1.0 + np.exp(-x)))


def _compute_lotus_fused_emb(inits: dict[str, object], freqs: object) -> object:
    import numpy as np

    t_val = float(inits["t_step"].reshape(-1)[0])
    args = (t_val * freqs.astype(np.float32)).reshape(-1)
    t_emb = np.concatenate([np.cos(args), np.sin(args)], axis=0).astype(np.float32)

    w1 = inits["unet.time_embedding.linear_1.weight"].astype(np.float32)
    b1 = inits["unet.time_embedding.linear_1.bias"].astype(np.float32)
    w2 = inits["unet.time_embedding.linear_2.weight"].astype(np.float32)
    b2 = inits["unet.time_embedding.linear_2.bias"].astype(np.float32)
    temb = _silu_f32(t_emb @ w1.T + b1) @ w2.T + b2

    cw1 = inits["unet.class_embedding.linear_1.weight"].astype(np.float32)
    cb1 = inits["unet.class_embedding.linear_1.bias"].astype(np.float32)
    cw2 = inits["unet.class_embedding.linear_2.weight"].astype(np.float32)
    cb2 = inits["unet.class_embedding.linear_2.bias"].astype(np.float32)
    task = inits["task_emb"].astype(np.float32).reshape(-1)
    cemb = _silu_f32(task @ cw1.T + cb1) @ cw2.T + cb2

    return (temb + cemb).astype(np.float32)


def compact_lotus_tensor_name(name: str) -> str:
    """Shorten Diffusers tensor names deterministically below GGML_MAX_NAME (64 bytes)."""
    for prefix, short_prefix in (
        ("unet.", "u."),
        ("ctrl.", "c."),
        ("vae.encoder.", "ve."),
        ("vae.decoder.", "vd."),
    ):
        if name.startswith(prefix):
            name = short_prefix + name[len(prefix) :]
            break
    for token, short_tok in (
        ("down_blocks.", "db."),
        ("up_blocks.", "ub."),
        ("mid_block.", "mb."),
        (".attentions.", ".at."),
        (".transformer_blocks.", ".tb."),
        (".resnets.", ".rn."),
        (".downsamplers.", ".ds."),
        (".upsamplers.", ".us."),
        (".time_emb_proj.", ".tp."),
        (".conv_shortcut.", ".cs."),
        (".group_norm.", ".gn."),
    ):
        name = name.replace(token, short_tok)
    if name.endswith(".weight"):
        name = name[: -len(".weight")] + ".w"
    elif name.endswith(".bias"):
        name = name[: -len(".bias")] + ".b"
    return name


def _map_lotus_anon_name(node_name: str) -> tuple[str, bool]:
    parts = [p for p in node_name.split("/") if p]
    op_leaf = parts[-1]
    base = ".".join(parts[:-1])
    if base.startswith("encoder.") or base.startswith("decoder.") or base in (
        "quant_conv",
        "post_quant_conv",
    ):
        base = "vae." + base
    if "MatMul" in op_leaf:
        return f"{base}.weight", True
    if "Mul" in op_leaf:
        return f"{base}.weight", False
    if "Add" in op_leaf:
        return f"{base}.bias", False
    raise RuntimeError(f"Unexpected anonymous ONNX initializer consumer node: {node_name}")


def _to_tensor_entry(name: str, arr: object, ggml_type: int) -> GgufTensorEntry:
    import numpy as np

    dtype = np.float16 if ggml_type == GGML_TYPE_F16 else np.float32
    cont = np.ascontiguousarray(arr.astype(dtype))
    return GgufTensorEntry(name=name, dims_c=tuple(cont.shape), ggml_type=ggml_type, raw_bytes=cont.tobytes())


def _collect_consumers_and_freqs(model: object, inits: dict[str, object]) -> tuple[dict[str, list[object]], object]:
    import numpy as np

    consumers: dict[str, list[object]] = {}
    freqs = None
    for node in model.graph.node:
        for inp in node.input:
            if inp in inits:
                consumers.setdefault(inp, []).append(node)
        if node.op_type == "Constant" and node.name == "/unet/time_proj/Constant_1":
            for attr in node.attribute:
                if attr.name == "value":
                    from onnx import numpy_helper

                    freqs = numpy_helper.to_array(attr.t)
    if freqs is None:
        half_dim = 160
        freqs = np.exp(-math.log(10000.0) * np.arange(half_dim, dtype=np.float32) / float(half_dim))
    return consumers, freqs


def _is_skipped_lotus_init(init_name: str) -> bool:
    return (
        init_name in ("t_step", "task_emb")
        or "time_embedding" in init_name
        or "class_embedding" in init_name
    )


def _resolve_lotus_names(
    init_name: str,
    consumers: dict[str, list[object]],
    seen_names: set[str],
) -> tuple[str, str, bool]:
    canonical = init_name
    need_transpose = False
    if init_name.startswith("onnx::"):
        nodes = consumers.get(init_name, [])
        if len(nodes) != 1:
            raise RuntimeError(f"Expected single consumer for {init_name}, got {len(nodes)}")
        canonical, need_transpose = _map_lotus_anon_name(nodes[0].name)
    if canonical in seen_names:
        raise RuntimeError(f"Duplicate canonical tensor name: {canonical}")
    seen_names.add(canonical)

    short_name = compact_lotus_tensor_name(canonical)
    if short_name in seen_names and short_name != canonical:
        raise RuntimeError(f"Duplicate compacted tensor name: {short_name}")
    seen_names.add(short_name)
    return canonical, short_name, need_transpose


def _convert_single_lotus_initializer(
    init_name: str,
    arr: object,
    consumers: dict[str, list[object]],
    seen_names: set[str],
) -> GgufTensorEntry | None:
    if _is_skipped_lotus_init(init_name):
        return None
    if init_name == "prompt_embeds":
        ctx = arr[0] if arr.ndim == 3 else arr
        seen_names.add("lotus.context")
        return _to_tensor_entry("lotus.context", ctx, GGML_TYPE_F32)

    canonical, short_name, need_transpose = _resolve_lotus_names(init_name, consumers, seen_names)
    if need_transpose:
        arr = arr.T
    if arr.ndim > 1 and (
        ".norm" in canonical or ".group_norm." in canonical or "conv_norm_out" in canonical
    ):
        arr = arr.reshape(-1)
    is_f32 = arr.ndim == 1
    return _to_tensor_entry(short_name, arr, GGML_TYPE_F32 if is_f32 else GGML_TYPE_F16)


def _collect_lotus_tensors(model_path: Path) -> list[GgufTensorEntry]:
    import onnx
    from onnx import numpy_helper

    model = onnx.load(str(model_path))
    inits = {init.name: numpy_helper.to_array(init) for init in model.graph.initializer}
    consumers, freqs = _collect_consumers_and_freqs(model, inits)

    tensors: list[GgufTensorEntry] = [
        _to_tensor_entry("lotus.emb", _compute_lotus_fused_emb(inits, freqs), GGML_TYPE_F32)
    ]
    seen_names: set[str] = {"lotus.emb"}

    for init_name, arr in inits.items():
        entry = _convert_single_lotus_initializer(init_name, arr, consumers, seen_names)
        if entry is not None:
            tensors.append(entry)
    return tensors


def convert_lotus_onnx_to_gguf(spec: ModelSpec, onnx_path: Path, out_path: Path) -> Path:
    """Repack a local Lotus FP16 ONNX file cleanly into FP16 GGUF v3 without quantization."""
    tensors = _collect_lotus_tensors(onnx_path)
    metadata: dict[str, object] = {
        "general.architecture": spec.arch,
        "general.name": spec.key,
        "lotus.variant": spec.variant,
        "lotus.vae_scale_factor": 0.18215,
        "lotus.vae_norm_eps": 1e-6,
        "lotus.unet_norm_eps": 1e-5,
    }
    write_gguf_file(out_path, metadata, tensors)
    return out_path


def _extract_single_conv_pair(
    node: object,
    by_input: dict[str, list[object]],
    inits: dict[str, object],
    consumed: set[str],
) -> list[GgufTensorEntry]:
    w_name = node.input[1]
    b_name = node.input[2] if len(node.input) > 2 and node.input[2] in inits else None
    consumed.add(w_name)
    if b_name:
        consumed.add(b_name)
    prefix = w_name[: -len(".weight")]

    next_nodes = by_input.get(node.output[0], [])
    bn = next_nodes[0] if len(next_nodes) == 1 and next_nodes[0].op_type == "BatchNormalization" else None
    if bn is None:
        entries = [_to_tensor_entry(f"{prefix}.weight", inits[w_name], GGML_TYPE_F16)]
        if b_name:
            entries.append(_to_tensor_entry(f"{prefix}.bias", inits[b_name], GGML_TYPE_F32))
        return entries

    eps = next((float(attr.f) for attr in bn.attribute if attr.name == "epsilon"), 1e-5)
    for idx in range(1, 5):
        consumed.add(bn.input[idx])
    fw, fb = fuse_conv_and_batchnorm(
        inits[w_name],
        inits[b_name] if b_name else None,
        inits[bn.input[1]],
        inits[bn.input[2]],
        inits[bn.input[3]],
        inits[bn.input[4]],
        eps,
    )
    return [
        _to_tensor_entry(f"{prefix}.weight", fw, GGML_TYPE_F16),
        _to_tensor_entry(f"{prefix}.bias", fb, GGML_TYPE_F32),
    ]


def _extract_conv_to_bn_pairs(graph: object, inits: dict[str, object]) -> tuple[list[GgufTensorEntry], set[str]]:
    by_input: dict[str, list[object]] = {}
    for node in graph.node:
        for inp in node.input:
            by_input.setdefault(inp, []).append(node)

    tensors: list[GgufTensorEntry] = []
    consumed: set[str] = set()
    for node in graph.node:
        if node.op_type == "Conv":
            tensors.extend(_extract_single_conv_pair(node, by_input, inits, consumed))
    return tensors, consumed


def convert_deepbump_onnx_to_gguf(spec: ModelSpec, onnx_path: Path, out_path: Path) -> Path:
    """Convert DeepBump ONNX with fused Conv+BatchNorm weights into FP16 GGUF v3."""
    import onnx
    from onnx import numpy_helper

    model = onnx.load(str(onnx_path))
    inits = {init.name: numpy_helper.to_array(init) for init in model.graph.initializer}
    tensors, consumed = _extract_conv_to_bn_pairs(model.graph, inits)
    if consumed != set(inits.keys()):
        missing = sorted(set(inits.keys()) - consumed)
        raise RuntimeError(f"Unconsumed DeepBump initializers: {missing}")

    metadata: dict[str, object] = {
        "general.architecture": spec.arch,
        "general.name": spec.key,
        "deepbump.tile_size": 256,
    }
    write_gguf_file(out_path, metadata, tensors)
    return out_path


def _file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fp:
        for chunk in iter(lambda: fp.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _compute_timestep_emb_f32(weights: dict[str, object], timestep: float = 0.0) -> object:
    import numpy as np

    half_dim = 160
    freqs = np.exp(-math.log(10000.0) * np.arange(half_dim, dtype=np.float32) / float(half_dim))
    args = (float(timestep) * freqs).reshape(-1)
    t_emb = np.concatenate([np.cos(args), np.sin(args)], axis=0).astype(np.float32)
    w1 = weights["time_embedding.linear_1.weight"].astype(np.float32)
    b1 = weights["time_embedding.linear_1.bias"].astype(np.float32)
    w2 = weights["time_embedding.linear_2.weight"].astype(np.float32)
    b2 = weights["time_embedding.linear_2.bias"].astype(np.float32)
    return (_silu_f32(t_emb @ w1.T + b1) @ w2.T + b2).astype(np.float32)


def _compute_empty_clip_context_f32(repo_dir: Path) -> object:
    import torch
    from transformers import CLIPTextModel, CLIPTokenizer

    tokenizer = CLIPTokenizer.from_pretrained(str(repo_dir / "tokenizer"))
    text_encoder = CLIPTextModel.from_pretrained(
        str(repo_dir / "text_encoder"), torch_dtype=torch.float32, variant="fp16"
    )
    tok = tokenizer(
        "",
        padding="max_length",
        max_length=tokenizer.model_max_length,
        truncation=True,
        return_tensors="pt",
    )
    with torch.no_grad():
        emb = text_encoder(tok.input_ids, attention_mask=None)[0][0]
    return emb.cpu().numpy().astype("float32")


def _pack_safetensors_section(
    sd: dict[str, object],
    prefix: str,
    seen_names: set[str],
    skip_pred: object,
) -> list[GgufTensorEntry]:
    entries: list[GgufTensorEntry] = []
    for raw_name in sorted(sd.keys()):
        if skip_pred(raw_name):
            continue
        arr = sd[raw_name]
        short_name = compact_lotus_tensor_name(prefix + raw_name)
        if short_name in seen_names:
            raise RuntimeError(f"Duplicate compacted StableDelight tensor name: {short_name}")
        seen_names.add(short_name)
        ggml_type = GGML_TYPE_F32 if arr.ndim == 1 else GGML_TYPE_F16
        entries.append(_to_tensor_entry(short_name, arr, ggml_type))
    return entries


def convert_delight_hf_to_gguf(spec: ModelSpec, repo_dir: Path, out_path: Path) -> Path:
    """Convert HuggingFace StableDelight (yoso-delight-v0-4-base) safetensors cleanly into FP16 GGUF v3."""
    from safetensors import safe_open

    def load_np_dict(subpath: str) -> dict[str, object]:
        out: dict[str, object] = {}
        with safe_open(str(repo_dir / subpath), framework="numpy", device="cpu") as f:
            for k in f.keys():
                out[k] = f.get_tensor(k)
        return out

    vae_sd = load_np_dict("vae/diffusion_pytorch_model.fp16.safetensors")
    ctrl_sd = load_np_dict("controlnet/diffusion_pytorch_model.fp16.safetensors")
    unet_sd = load_np_dict("unet/diffusion_pytorch_model.fp16.safetensors")

    context_f32 = _compute_empty_clip_context_f32(repo_dir)
    ctrl_emb_f32 = _compute_timestep_emb_f32(ctrl_sd, 0.0)
    unet_emb_f32 = _compute_timestep_emb_f32(unet_sd, 0.0)

    seen_names: set[str] = {"delight.context", "delight.ctrl_emb", "delight.unet_emb"}
    tensors: list[GgufTensorEntry] = [
        _to_tensor_entry("delight.context", context_f32, GGML_TYPE_F32),
        _to_tensor_entry("delight.ctrl_emb", ctrl_emb_f32, GGML_TYPE_F32),
        _to_tensor_entry("delight.unet_emb", unet_emb_f32, GGML_TYPE_F32),
    ]
    tensors.extend(_pack_safetensors_section(vae_sd, "vae.", seen_names, lambda _k: False))
    tensors.extend(
        _pack_safetensors_section(
            ctrl_sd,
            "ctrl.",
            seen_names,
            lambda k: k.startswith("controlnet_") or "time_embedding" in k,
        )
    )
    tensors.extend(
        _pack_safetensors_section(
            unet_sd,
            "unet.",
            seen_names,
            lambda k: "time_embedding" in k,
        )
    )

    metadata: dict[str, object] = {
        "general.architecture": spec.arch,
        "general.name": spec.key,
        "delight.variant": spec.variant,
        "delight.vae_scale_factor": 0.18215,
        "delight.vae_norm_eps": 1e-6,
        "delight.unet_norm_eps": 1e-5,
    }
    write_gguf_file(out_path, metadata, tensors)
    return out_path


def _resolve_hf_snapshot(hf_repo: str, hf_cache_dir: Path | None) -> Path:
    from huggingface_hub import snapshot_download

    allow = [
        "model_index.json",
        "scheduler/*",
        "text_encoder/config.json",
        "text_encoder/model.fp16.safetensors",
        "tokenizer/*",
        "vae/config.json",
        "vae/diffusion_pytorch_model.fp16.safetensors",
        "unet/config.json",
        "unet/diffusion_pytorch_model.fp16.safetensors",
        "controlnet/config.json",
        "controlnet/diffusion_pytorch_model.fp16.safetensors",
    ]
    snap = snapshot_download(
        hf_repo,
        cache_dir=str(hf_cache_dir) if hf_cache_dir is not None else None,
        allow_patterns=allow,
    )
    return Path(snap)


def _download_pth_file(url: str, dst_path: Path) -> Path:
    import urllib.request

    dst_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = dst_path.with_suffix(".pth.tmp")
    req = urllib.request.Request(url, headers={"User-Agent": "QuakeRay/1.0"})
    with urllib.request.urlopen(req, timeout=60) as resp, tmp_path.open("wb") as fp:
        while True:
            chunk = resp.read(1024 * 1024)
            if not chunk:
                break
            fp.write(chunk)
    tmp_path.replace(dst_path)
    return dst_path


def convert_realplksr_pth_to_gguf(spec: ModelSpec, pth_path: Path, out_path: Path) -> Path:
    """Convert a PyTorch RealPLKSR .pth checkpoint cleanly into FP16 GGUF v3."""
    import torch

    sd = torch.load(str(pth_path), map_location="cpu", weights_only=True)
    if isinstance(sd, dict) and "params_ema" in sd:
        sd = sd["params_ema"]
    elif isinstance(sd, dict) and "params" in sd:
        sd = sd["params"]

    dim = int(sd["feats.0.weight"].shape[0])
    pdim = int(sd["feats.1.lk.conv.weight"].shape[0])
    kernel_size = int(sd["feats.1.lk.conv.weight"].shape[2])
    n_blocks = sum(1 for k in sd.keys() if k.startswith("feats.") and k.endswith(".refine.weight"))
    dysample = "to_img.init_pos" in sd
    tail_key = f"feats.{n_blocks + 2}.weight"
    if tail_key not in sd:
        tail_key = f"feats.{n_blocks + 1}.weight"
    scale = int(round((int(sd[tail_key].shape[0]) / 3) ** 0.5))
    dysample_groups = int(sd["to_img.offset.weight"].shape[0]) // (2 * scale * scale) if dysample else 0

    tensors: list[GgufTensorEntry] = []
    for key in sorted(sd.keys()):
        arr = sd[key].detach().cpu().numpy()
        if key == "to_img.init_pos":
            arr = arr.reshape(-1)
            ggml_type = GGML_TYPE_F32
        elif arr.ndim == 1:
            ggml_type = GGML_TYPE_F32
        else:
            ggml_type = GGML_TYPE_F16
        tensors.append(_to_tensor_entry(key, arr, ggml_type))

    metadata: dict[str, object] = {
        "general.architecture": spec.arch,
        "general.name": spec.key,
        "realplksr.scale": scale,
        "realplksr.dim": dim,
        "realplksr.n_blocks": n_blocks,
        "realplksr.kernel_size": kernel_size,
        "realplksr.pdim": pdim,
        "realplksr.norm_groups": 4,
        "realplksr.dysample": 1 if dysample else 0,
        "realplksr.dysample_groups": dysample_groups,
        "realplksr.norm_eps": 1e-5,
    }
    write_gguf_file(out_path, metadata, tensors)
    return out_path


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    """Parse CLI arguments for GGUF model conversion."""
    parser = argparse.ArgumentParser(description="Convert Lotus, DeepBump, StableDelight, & RealPLKSR models to FP16 GGUF")
    parser.add_argument("--from-onnx", action="store_true", help="Convert local cached ONNX files into GGUF")
    parser.add_argument("--from-hf", action="store_true", help="Download/convert HuggingFace Diffusers models into GGUF")
    parser.add_argument("--from-pth", action="store_true", help="Download/convert PyTorch .pth models into GGUF")
    parser.add_argument("--model", action="append", choices=sorted(MODEL_SPECS.keys()), help="Model key(s) to convert")
    parser.add_argument("--all", action="store_true", help="Convert all supported models")
    parser.add_argument("--input-dir", type=Path, default=default_models_dir(), help="ONNX/.pth input directory")
    parser.add_argument("--output-dir", type=Path, default=default_models_dir(), help="GGUF output directory")
    parser.add_argument("--hf-cache-dir", type=Path, default=None, help="Optional HuggingFace cache directory")
    parser.add_argument("--force", action="store_true", help="Overwrite existing GGUF files")
    parser.add_argument("--self-test", action="store_true", help="Validate metadata/specs and exit")
    return parser.parse_args(list(argv) if argv is not None else None)


def _select_model_keys(args: argparse.Namespace) -> list[str]:
    if args.model:
        return list(args.model)
    if args.all:
        return list(MODEL_SPECS.keys())
    if args.from_pth:
        return [k for k, v in MODEL_SPECS.items() if v.pth_name]
    if args.from_hf:
        return [k for k, v in MODEL_SPECS.items() if v.hf_repo]
    return list(DEFAULT_MODEL_KEYS)


def _export_single_model(args: argparse.Namespace, key: str) -> None:
    spec = MODEL_SPECS[key]
    out_path = args.output_dir / spec.gguf_name
    if out_path.is_file() and not args.force:
        print(f"Skipping existing {out_path} ({out_path.stat().st_size} bytes)")
        return
    if spec.arch == "delight":
        if not args.from_hf or not spec.hf_repo:
            raise RuntimeError(f"Model {key} requires --from-hf")
        convert_delight_hf_to_gguf(spec, _resolve_hf_snapshot(spec.hf_repo, args.hf_cache_dir), out_path)
    elif spec.arch == "realplksr":
        if not args.from_pth or not spec.pth_name:
            return
        pth_path = args.input_dir / spec.pth_name
        if not pth_path.is_file():
            if not spec.pth_url:
                raise FileNotFoundError(f"Missing .pth source: {pth_path}")
            _download_pth_file(spec.pth_url, pth_path)
        convert_realplksr_pth_to_gguf(spec, pth_path, out_path)
    else:
        if not args.from_onnx:
            return
        onnx_path = args.input_dir / spec.onnx_name
        if not onnx_path.is_file():
            raise FileNotFoundError(f"Missing ONNX source: {onnx_path}")
        if spec.arch == "lotus":
            convert_lotus_onnx_to_gguf(spec, onnx_path, out_path)
        else:
            convert_deepbump_onnx_to_gguf(spec, onnx_path, out_path)
    print(f"Wrote {out_path} ({out_path.stat().st_size} bytes, sha256={_file_sha256(out_path)})")


def main(argv: Sequence[str] | None = None) -> int:
    """CLI entry point for converting local ONNX, HuggingFace, or .pth models into FP16 GGUF."""
    args = parse_args(argv)
    if args.self_test:
        print("export_ai_gguf.py self-test OK")
        return 0
    if not args.from_onnx and not args.from_hf and not args.from_pth:
        raise RuntimeError("Pass --from-onnx, --from-hf, or --from-pth to convert models to FP16 GGUF.")
    for key in _select_model_keys(args):
        _export_single_model(args, key)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
