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

"""Local AI-assisted PBR map generator (normals, gloss, emissive) from diffuse textures."""

from __future__ import annotations

import argparse
from pathlib import Path
import tempfile
import urllib.request

import numpy as np
import onnxruntime as ort
from PIL import Image, ImageFilter

DEEPBUMP_URL = "https://github.com/HugoTini/DeepBump/raw/master/deepbump256.onnx"
DEFAULT_CACHE_PATH = Path.home() / ".cache" / "quakeray" / "deepbump256.onnx"
TILE_SIZE = 256
TILE_STRIDE = 192


def ensure_deepbump_model(model_path: Path = DEFAULT_CACHE_PATH) -> Path:
    """Ensure the DeepBump ONNX model exists in the local cache."""
    if model_path.is_file() and model_path.stat().st_size > 0:
        return model_path
    model_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = model_path.with_suffix(".onnx.tmp")
    urllib.request.urlretrieve(DEEPBUMP_URL, str(tmp_path))
    tmp_path.replace(model_path)
    return model_path


def _tile_blend_window(size: int) -> np.ndarray:
    margin = size - TILE_STRIDE
    ramp = np.linspace(0.0, 1.0, margin + 2, dtype=np.float32)[1:-1]
    w1d = np.ones(size, dtype=np.float32)
    w1d[:margin] = ramp
    w1d[-margin:] = ramp[::-1]
    return np.outer(w1d, w1d)


def _sample_wrapped(arr: np.ndarray, y0: int, x0: int, h_tile: int, w_tile: int) -> np.ndarray:
    h, w = arr.shape[:2]
    ys = (np.arange(y0, y0 + h_tile) % h)[:, None]
    xs = (np.arange(x0, x0 + w_tile) % w)[None, :]
    return arr[ys, xs]


def _accumulate_wrapped(
    acc: np.ndarray,
    wsum: np.ndarray,
    tile: np.ndarray,
    weight: np.ndarray,
    y0: int,
    x0: int,
) -> None:
    h, w = acc.shape[:2]
    ys = (np.arange(y0, y0 + TILE_SIZE) % h)[:, None]
    xs = (np.arange(x0, x0 + TILE_SIZE) % w)[None, :]
    np.add.at(acc, (ys, xs), tile * weight[:, :, None])
    np.add.at(wsum, (ys, xs), weight)


def run_deepbump_normals(luma: np.ndarray, model_path: Path) -> np.ndarray:
    """Predict seamless tangent-space normals [0, 1] RGB (H, W, 3) using DeepBump ONNX."""
    h, w = luma.shape
    sess = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    in_name = sess.get_inputs()[0].name
    weight = _tile_blend_window(TILE_SIZE)

    acc = np.zeros((h, w, 3), dtype=np.float32)
    wsum = np.zeros((h, w), dtype=np.float32)

    y_starts = list(range(0, max(1, h), TILE_STRIDE))
    x_starts = list(range(0, max(1, w), TILE_STRIDE))
    for y0 in y_starts:
        for x0 in x_starts:
            patch = _sample_wrapped(luma, y0, x0, TILE_SIZE, TILE_SIZE)
            inp = patch[None, None, :, :].astype(np.float32)
            out = sess.run(None, {in_name: inp})[0][0]
            _accumulate_wrapped(acc, wsum, np.transpose(out, (1, 2, 0)), weight, y0, x0)

    rgb = acc / np.maximum(wsum[:, :, None], 1e-6)
    vec = rgb * 2.0 - 1.0
    vec[:, :, 2] = np.maximum(vec[:, :, 2], 0.08)
    norm = np.linalg.norm(vec, axis=2, keepdims=True)
    vec = vec / np.maximum(norm, 1e-6)
    return np.clip(vec * 0.5 + 0.5, 0.0, 1.0)


def _wrapped_blur(field: np.ndarray, radius: float) -> np.ndarray:
    h, w = field.shape
    padded = _sample_wrapped(field, -16, -16, h + 32, w + 32)
    u8 = np.clip(padded * 255.0 + 0.5, 0.0, 255.0).astype(np.uint8)
    blurred = np.asarray(
        Image.fromarray(u8, mode="L").filter(ImageFilter.GaussianBlur(radius=radius)),
        dtype=np.float32,
    )
    return blurred[16 : 16 + h, 16 : 16 + w] / 255.0


def _normals_to_curvature(normals01: np.ndarray) -> np.ndarray:
    # OpenGL tangent-space convention: nx = -dH/dx, ny = +dH/dy.
    # Convex bumps therefore have dx(nx) < 0 and dy(ny) > 0? Check directly:
    # Wait: left edge (rising x): nx < 0; right edge (falling x): nx > 0 => dx(nx) > 0 at edges,
    # Wait: inside plateau left edge has dH/dx > 0 -> nx < 0; right edge dH/dx < 0 -> nx > 0,
    # so Laplacian = d2H/dx2 + d2H/dy2 = -d(nx)/dx + d(ny)/dy. Peaks have Laplacian < 0,
    # so convex ridge score is d(nx)/dx - d(ny)/dy.
    nx = normals01[:, :, 0] * 2.0 - 1.0
    ny = normals01[:, :, 1] * 2.0 - 1.0
    dnx = 0.5 * (np.roll(nx, -1, axis=1) - np.roll(nx, 1, axis=1))
    dny = 0.5 * (np.roll(ny, -1, axis=0) - np.roll(ny, 1, axis=0))
    curv = dnx - dny
    return _wrapped_blur(np.clip(curv * 0.5 + 0.5, 0.0, 1.0), radius=1.5) * 2.0 - 1.0


def generate_gloss_rgba(rgba: np.ndarray, model_path: Path) -> np.ndarray:
    """Derive seamless gloss/smoothness RGBA image from DeepBump normals + albedo cues."""
    rgb = rgba[:, :, :3].astype(np.float32) / 255.0
    luma = 0.299 * rgb[:, :, 0] + 0.587 * rgb[:, :, 1] + 0.114 * rgb[:, :, 2]
    cmax = rgb.max(axis=2)
    cmin = rgb.min(axis=2)
    sat = np.where(cmax > 1e-4, (cmax - cmin) / np.maximum(cmax, 1e-4), 0.0)

    normals01 = run_deepbump_normals(luma, model_path)
    curv = _normals_to_curvature(normals01)
    luma_smooth = _wrapped_blur(luma, radius=2.5)
    local_relief = luma - luma_smooth
    metallic_cue = luma * (1.0 - 0.55 * sat)

    gloss = np.clip(0.20 + 0.48 * metallic_cue + 0.45 * curv + 0.75 * local_relief, 0.06, 0.92)
    g_u8 = np.clip(gloss * 255.0 + 0.5, 0.0, 255.0).astype(np.uint8)
    out = np.empty_like(rgba)
    out[:, :, 0] = g_u8
    out[:, :, 1] = g_u8
    out[:, :, 2] = g_u8
    out[:, :, 3] = 255
    return out


def generate_emissive_rgba(rgba: np.ndarray) -> np.ndarray:
    """Extract high-energy emissive regions with seamless soft halo."""
    rgb = rgba[:, :, :3].astype(np.float32) / 255.0
    luma = 0.299 * rgb[:, :, 0] + 0.587 * rgb[:, :, 1] + 0.114 * rgb[:, :, 2]
    cmax = rgb.max(axis=2)
    cmin = rgb.min(axis=2)
    sat = np.where(cmax > 1e-4, (cmax - cmin) / np.maximum(cmax, 1e-4), 0.0)

    gate = float(np.clip(np.median(luma) + 0.20, 0.45, 0.82))
    luma_excess = np.maximum(0.0, luma - gate)
    chroma_glow = sat * np.maximum(0.0, luma - 0.28)
    energy = luma_excess * 2.5 + chroma_glow * 1.35
    t = np.clip((energy - 0.12) / (0.55 - 0.12), 0.0, 1.0)
    core = t * t * (3.0 - 2.0 * t)
    halo = _wrapped_blur(core, radius=1.2)
    mask = np.clip(np.maximum(core, halo * 0.65), 0.0, 1.0)

    out = np.empty_like(rgba)
    out[:, :, :3] = np.clip(rgba[:, :, :3].astype(np.float32) * mask[:, :, None] + 0.5, 0.0, 255.0).astype(np.uint8)
    out[:, :, 3] = 255
    return out


def generate_map(input_path: Path, output_path: Path, kind: str, model_path: Path) -> None:
    """Generate the requested PBR map (`normals`, `gloss`, `emissive`) from diffuse image."""
    rgba = np.asarray(Image.open(input_path).convert("RGBA"), dtype=np.uint8)
    if kind == "normals":
        rgb = rgba[:, :, :3].astype(np.float32) / 255.0
        luma = 0.299 * rgb[:, :, 0] + 0.587 * rgb[:, :, 1] + 0.114 * rgb[:, :, 2]
        n01 = run_deepbump_normals(luma, model_path)
        out = np.empty_like(rgba)
        out[:, :, :3] = np.clip(n01 * 255.0 + 0.5, 0.0, 255.0).astype(np.uint8)
        out[:, :, 3] = 255
    elif kind == "gloss":
        out = generate_gloss_rgba(rgba, model_path)
    elif kind == "emissive":
        out = generate_emissive_rgba(rgba)
    else:
        raise ValueError(f"Unsupported kind: {kind}")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(out, mode="RGBA").save(output_path)


def run_self_test() -> None:
    """Verify ONNX model download/load and all 3 generation modes on a synthetic tile."""
    model_path = ensure_deepbump_model()
    tile = np.full((64, 64, 4), 48, dtype=np.uint8)
    tile[:, :, 3] = 255
    tile[16:48, 16:48, :3] = 210
    tile[28:36, 28:36, :3] = (255, 220, 80)

    with tempfile.TemporaryDirectory(prefix="qray_ai_tex_") as tmp:
        tmp_dir = Path(tmp)
        src = tmp_dir / "diffuse.tga"
        Image.fromarray(tile, mode="RGBA").save(src)
        for kind in ("normals", "gloss", "emissive"):
            dst = tmp_dir / f"out_{kind}.tga"
            generate_map(src, dst, kind, model_path)
            result = np.asarray(Image.open(dst).convert("RGBA"), dtype=np.uint8)
            assert result.shape == tile.shape, f"{kind} shape mismatch"
            assert result[:, :, 2].mean() > 128.0 if kind == "normals" else True
        emiss = np.asarray(Image.open(tmp_dir / "out_emissive.tga").convert("RGBA"))
        assert emiss[32, 32, 0] > emiss[4, 4, 0], "emissive should highlight hot core"
    print("generate_texture_ai.py self-test OK")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Generate PBR map from diffuse texture with AI.")
    parser.add_argument("--input", type=Path, help="Input diffuse image path")
    parser.add_argument("--output", type=Path, help="Output generated image path")
    parser.add_argument("--kind", choices=("normals", "gloss", "emissive"), help="Map type")
    parser.add_argument("--model", type=Path, default=DEFAULT_CACHE_PATH, help="DeepBump ONNX path")
    parser.add_argument("--self-test", action="store_true", help="Run built-in verification")
    args = parser.parse_args(argv)

    if args.self_test:
        run_self_test()
        return 0
    if not args.input or not args.output or not args.kind:
        parser.error("--input, --output, and --kind are required unless --self-test is set")

    model_path = ensure_deepbump_model(args.model) if args.kind in ("normals", "gloss") else args.model
    generate_map(args.input, args.output, args.kind, model_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
