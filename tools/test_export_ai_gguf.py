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

"""Unit tests for the GGUF converter without requiring heavy ML packages."""

from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import export_ai_gguf as mod  # noqa: E402


class ExportAiGgufTests(unittest.TestCase):
    """Tests GGUF v3 binary framing, metadata keys, and CLI defaults."""

    def test_self_test_and_specs(self) -> None:
        self.assertEqual(mod.main(["--self-test"]), 0)
        self.assertEqual(
            set(mod.MODEL_SPECS.keys()),
            {
                "lotus-normal-d-v1-1",
                "lotus-depth-d-v2-0-disparity",
                "deepbump256",
                "yoso-delight-v0-4-base",
                "4x-pbrify-rplksrd-v3",
            },
        )
        for key, spec in mod.MODEL_SPECS.items():
            self.assertEqual(spec.key, key)
            self.assertTrue(spec.gguf_name.endswith("-fp16.gguf"))

    def test_write_gguf_file_header_and_alignment(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out_file = Path(tmp) / "tiny.gguf"
            raw_a = struct.pack("<6f", 1.0, 2.0, 3.0, 4.0, 5.0, 6.0)
            raw_b = struct.pack("<2H", 0x3C00, 0x4000)
            tensors = [
                mod.GgufTensorEntry("a.bias", (6,), mod.GGML_TYPE_F32, raw_a),
                mod.GgufTensorEntry("b.weight", (2, 1), mod.GGML_TYPE_F16, raw_b),
            ]
            meta = {"general.architecture": "lotus", "lotus.vae_scale_factor": 0.18215, "deepbump.tile_size": 256}
            mod.write_gguf_file(out_file, meta, tensors)
            blob = out_file.read_bytes()
            self.assertTrue(blob.startswith(b"GGUF"))
            ver, n_tensors, n_kv = struct.unpack_from("<Iqq", blob, 4)
            self.assertEqual((ver, n_tensors, n_kv), (3, 2, 3))
            self.assertEqual(len(blob) % mod.GGUF_ALIGNMENT, 0)
            self.assertIn(raw_b, blob)

    def test_map_lotus_anon_name_matmul_before_mul(self) -> None:
        name_mm, tr_mm = mod._map_lotus_anon_name(
            "/unet/down_blocks.0/attentions.0/transformer_blocks.0/attn2/to_k/MatMul"
        )
        self.assertEqual(name_mm, "unet.down_blocks.0.attentions.0.transformer_blocks.0.attn2.to_k.weight")
        self.assertTrue(tr_mm)

        name_mul, tr_mul = mod._map_lotus_anon_name("/encoder/conv_norm_out/Mul")
        self.assertEqual(name_mul, "vae.encoder.conv_norm_out.weight")
        self.assertFalse(tr_mul)

    def test_compact_lotus_name_and_ggml_max_name_guard(self) -> None:
        long_name = "unet.up_blocks.3.attentions.2.transformer_blocks.0.attn2.to_out.0.weight"
        short_name = mod.compact_lotus_tensor_name(long_name)
        self.assertEqual(short_name, "u.ub.3.at.2.tb.0.attn2.to_out.0.w")
        self.assertLess(len(short_name.encode("utf-8")), mod.GGML_MAX_NAME)

        with tempfile.TemporaryDirectory() as tmp:
            too_long = "a" * mod.GGML_MAX_NAME
            raw = struct.pack("<f", 1.0)
            with self.assertRaises(ValueError):
                mod.write_gguf_file(
                    Path(tmp) / "bad.gguf",
                    {"general.architecture": "lotus"},
                    [mod.GgufTensorEntry(too_long, (1,), mod.GGML_TYPE_F32, raw)],
                )


if __name__ == "__main__":
    unittest.main()
