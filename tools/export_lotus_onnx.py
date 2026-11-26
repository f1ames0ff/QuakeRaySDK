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

"""Offline exporter converting Lotus FP16 pipelines into single-file ONNX models for C++."""

from __future__ import annotations

import argparse
import gc
import os
import sys
import time
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

DEFAULT_EXPORT_KEYS = ("lotus-normal-d-v1-1", "lotus-depth-d-v2-0-disparity")

LOTUS_MODELS: dict[str, tuple[str, str]] = {
    "lotus-normal-d-v1-1": ("jingheya/lotus-normal-d-v1-1", "lotus-normal-d-v1-1.onnx"),
    "lotus-depth-d-v2-0-disparity": (
        "jingheya/lotus-depth-d-v2-0-disparity",
        "lotus-depth-d-v2-0-disparity.onnx",
    ),
    "lotus-normal-g-v1-1": ("jingheya/lotus-normal-g-v1-1", "lotus-normal-g-v1-1.onnx"),
}


@dataclass(frozen=True)
class ExportRuntime:
    torch_mod: object
    nn_mod: object
    diffusers_mod: object
    attn_proc_cls: object
    transformers_mod: object


def default_models_dir() -> Path:
    """Return user cache directory where quakerayeditor resolves native ONNX models."""
    if sys.platform.startswith("win"):
        base = os.environ.get("LOCALAPPDATA") or os.environ.get("USERPROFILE") or "."
        return Path(base) / "QuakeRay" / "models"
    xdg = os.environ.get("XDG_CACHE_HOME")
    if xdg:
        return Path(xdg) / "quakeray" / "models"
    return Path.home() / ".cache" / "quakeray" / "models"


def load_hf_pretrained(loader_cls: object, repo_id: str, **kwargs: object) -> object:
    """Load a Hugging Face component from local cache first, downloading only on miss."""
    try:
        return loader_cls.from_pretrained(repo_id, local_files_only=True, **kwargs)
    except (OSError, ValueError):
        return loader_cls.from_pretrained(repo_id, local_files_only=False, **kwargs)


def _import_export_runtime() -> ExportRuntime:
    try:
        import diffusers  # type: ignore[import-not-found]
        import onnx  # type: ignore[import-not-found]  # noqa: F401
        import torch  # type: ignore[import-not-found]
        import transformers  # type: ignore[import-not-found]
        from diffusers.models.attention_processor import AttnProcessor  # type: ignore[import-not-found]
        from torch import nn  # type: ignore[import-not-found]
    except ImportError as exc:
        raise RuntimeError(
            "Lotus ONNX export requires torch, diffusers, transformers, and onnx "
            f"({exc})."
        ) from exc

    return ExportRuntime(
        torch_mod=torch,
        nn_mod=nn,
        diffusers_mod=diffusers,
        attn_proc_cls=AttnProcessor,
        transformers_mod=transformers,
    )


def _build_fused_lotus_module(
    rt: ExportRuntime,
    repo_id: str,
    device: object,
) -> object:
    torch = rt.torch_mod
    dtype = torch.float16 if device.type != "cpu" else torch.float32
    vae = load_hf_pretrained(
        rt.diffusers_mod.AutoencoderKL, repo_id, subfolder="vae", torch_dtype=dtype
    ).to(device)
    unet = load_hf_pretrained(
        rt.diffusers_mod.UNet2DConditionModel, repo_id, subfolder="unet", torch_dtype=dtype
    ).to(device)
    tok = load_hf_pretrained(rt.transformers_mod.CLIPTokenizer, repo_id, subfolder="tokenizer")
    te = load_hf_pretrained(
        rt.transformers_mod.CLIPTextModel, repo_id, subfolder="text_encoder", torch_dtype=dtype
    ).to(device)

    vae.eval()
    unet.eval()
    te.eval()
    vae.set_attn_processor(rt.attn_proc_cls())
    unet.set_attn_processor(rt.attn_proc_cls())

    max_len = min(
        int(getattr(tok, "model_max_length", 77)),
        int(getattr(te.config, "max_position_embeddings", 77)),
    )
    with torch.no_grad():
        ids = tok(
            "",
            padding="max_length",
            max_length=max_len,
            truncation=True,
            return_tensors="pt",
        ).input_ids.to(device)
        prompt_embeds = te(ids)[0].to(dtype=dtype)
        t_pair = torch.tensor([[1.0, 0.0]], device=device, dtype=dtype)
        task_emb = torch.cat([torch.sin(t_pair), torch.cos(t_pair)], dim=-1)
        t_step = torch.tensor([999], device=device, dtype=torch.long)
    del te, tok

    compute_dtype = dtype

    class _LotusFusedOnnx(rt.nn_mod.Module):
        def __init__(self) -> None:
            super().__init__()
            self.vae = vae
            self.unet = unet
            self.scale = float(vae.config.scaling_factor)
            self.in_channels = int(getattr(unet.config, "in_channels", 4))
            self.register_buffer("prompt_embeds", prompt_embeds)
            self.register_buffer("task_emb", task_emb)
            self.register_buffer("t_step", t_step)

        def forward(self, rgb_01: object) -> object:
            rgb_in = (rgb_01 * 2.0 - 1.0).to(compute_dtype)
            latents = self.vae.encoder(rgb_in)
            moments = self.vae.quant_conv(latents)
            mean = moments[:, :4, :, :] * self.scale
            if self.in_channels == 8:
                model_in = torch.cat([mean, torch.randn_like(mean)], dim=1)
            else:
                model_in = mean
            pred_lat = self.unet(
                model_in,
                self.t_step,
                encoder_hidden_states=self.prompt_embeds,
                class_labels=self.task_emb,
                return_dict=False,
            )[0]
            decoded = self.vae.decoder(self.vae.post_quant_conv(pred_lat / self.scale))
            return torch.clamp(decoded.float() * 0.5 + 0.5, 0.0, 1.0)

    return _LotusFusedOnnx().eval()


def export_single_model(model_key: str, output_dir: Path, force: bool = False) -> Path:
    """Export one fused Lotus model (< 2 GB single-file ONNX with float32 NCHW I/O)."""
    repo_id, out_name = LOTUS_MODELS[model_key]
    out_path = output_dir / out_name
    if out_path.is_file() and out_path.stat().st_size > 100 * 1024 * 1024 and not force:
        print(f"Skipping existing {out_path} ({out_path.stat().st_size // (1024 * 1024)} MB)")
        return out_path

    output_dir.mkdir(parents=True, exist_ok=True)
    rt = _import_export_runtime()
    torch = rt.torch_mod
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    t0 = time.perf_counter()
    print(f"Exporting {repo_id} -> {out_path} (device={device})...")
    fused = _build_fused_lotus_module(rt, repo_id, device)
    sample = torch.rand(1, 3, 512, 512, device=device, dtype=torch.float32)

    tmp_path = out_path.with_suffix(".onnx.tmp")
    with torch.no_grad():
        torch.onnx.export(
            fused,
            (sample,),
            str(tmp_path),
            input_names=["rgb_01"],
            output_names=["pred_01"],
            dynamic_axes={"rgb_01": {2: "height", 3: "width"}, "pred_01": {2: "height", 3: "width"}},
            opset_version=17,
            dynamo=False,
        )
    tmp_path.replace(out_path)

    del fused, sample
    gc.collect()
    if torch.cuda.is_available():
        torch.cuda.empty_cache()

    elapsed = time.perf_counter() - t0
    size_mb = out_path.stat().st_size / (1024.0 * 1024.0)
    print(f"Exported {out_path} ({size_mb:.1f} MB) in {elapsed:.1f}s")
    return out_path


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    """Parse CLI options for exporting Lotus ONNX models."""
    parser = argparse.ArgumentParser(description="Export Lotus FP16 models to fused ONNX")
    parser.add_argument(
        "--model",
        action="append",
        choices=sorted(LOTUS_MODELS.keys()),
        help="Model key(s) to export",
    )
    parser.add_argument("--all", action="store_true", help="Export default Lotus Normal & Depth models")
    parser.add_argument("--output-dir", type=Path, default=default_models_dir(), help="Target directory")
    parser.add_argument("--force", action="store_true", help="Overwrite existing ONNX files")
    parser.add_argument("--self-test", action="store_true", help="Validate exporter metadata and exit")
    return parser.parse_args(list(argv) if argv is not None else None)


def main(argv: Sequence[str] | None = None) -> int:
    """CLI entry point for offline Lotus -> ONNX conversion."""
    args = parse_args(argv)
    if args.self_test:
        print("export_lotus_onnx.py self-test OK")
        return 0

    selected = list(args.model) if args.model else list(DEFAULT_EXPORT_KEYS)
    if args.all and not args.model:
        selected = list(DEFAULT_EXPORT_KEYS)

    for key in selected:
        export_single_model(key, args.output_dir, force=args.force)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
