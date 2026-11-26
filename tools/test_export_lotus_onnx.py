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

"""Unit tests for tools/export_lotus_onnx.py (stdlib-only for CI safety)."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import export_lotus_onnx as exp


class ExportLotusOnnxTests(unittest.TestCase):
    def test_self_test_flag(self) -> None:
        self.assertEqual(exp.main(["--self-test"]), 0)

    def test_model_catalog_and_output_paths(self) -> None:
        self.assertEqual(
            exp.LOTUS_MODELS["lotus-normal-d-v1-1"],
            ("jingheya/lotus-normal-d-v1-1", "lotus-normal-d-v1-1.onnx"),
        )
        self.assertEqual(
            exp.LOTUS_MODELS["lotus-depth-d-v2-0-disparity"],
            ("jingheya/lotus-depth-d-v2-0-disparity", "lotus-depth-d-v2-0-disparity.onnx"),
        )
        args = exp.parse_args(["--all", "--output-dir", "/tmp/models"])
        self.assertTrue(args.all)
        self.assertEqual(args.output_dir, Path("/tmp/models"))

    def test_load_hf_pretrained_prefers_local_cache(self) -> None:
        calls: list[dict[str, object]] = []

        class _Loader:
            @staticmethod
            def from_pretrained(repo_id: str, **kwargs: object) -> str:
                calls.append({"repo_id": repo_id, **kwargs})
                if kwargs.get("local_files_only") and repo_id == "miss":
                    raise OSError("cache miss")
                return f"ok:{repo_id}:{kwargs.get('local_files_only')}"

        self.assertEqual(exp.load_hf_pretrained(_Loader, "hit", subfolder="vae"), "ok:hit:True")
        self.assertEqual(exp.load_hf_pretrained(_Loader, "miss", subfolder="unet"), "ok:miss:False")
        self.assertEqual([c["local_files_only"] for c in calls], [True, True, False])

    def test_skip_existing_large_onnx_without_calling_torch(self) -> None:
        with tempfile.TemporaryDirectory(prefix="qray_exp_test_") as tmp:
            out_dir = Path(tmp)
            target = out_dir / "lotus-normal-d-v1-1.onnx"
            with target.open("wb") as fh:
                fh.seek(101 * 1024 * 1024)
                fh.write(b"\x00")
            with mock.patch.object(exp, "_import_export_runtime", side_effect=AssertionError("should skip")):
                res = exp.export_single_model("lotus-normal-d-v1-1", out_dir, force=False)
                self.assertEqual(res, target)


if __name__ == "__main__":
    unittest.main()
