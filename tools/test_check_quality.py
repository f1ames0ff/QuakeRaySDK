#!/usr/bin/env python3
"""Unit tests for diff-gated quality checks in QuakeRaySDK."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_quality as cq  # noqa: E402


class CheckQualityTests(unittest.TestCase):
    def test_parse_git_diff_hunks(self) -> None:
        diff = "\n".join([
            "diff --git a/Source/Utils.cpp b/Source/Utils.cpp",
            "--- a/Source/Utils.cpp",
            "+++ b/Source/Utils.cpp",
            "@@ -10,0 +11,3 @@",
            "@@ -42,2 +45 @@",
            "diff --git a/Source/Old.cpp b/dev/null",
            "--- a/Source/Old.cpp",
            "+++ /dev/null",
            "@@ -1,5 +0,0 @@",
        ])
        parsed = cq.parse_git_diff_hunks(diff)
        self.assertEqual(parsed, {"Source/Utils.cpp": {11, 12, 13, 45}})

    def test_exclusions_skip_vendored_and_generated(self) -> None:
        self.assertTrue(cq.is_excluded("Source/FidelityFX_SDK_1.1.4/src/foo.cpp"))
        self.assertTrue(cq.is_excluded("Source/Shaders/Reflection/HLSL/CmFoo.txt"))
        self.assertFalse(cq.is_excluded("Source/VulkanDevice.cpp"))
        self.assertFalse(cq.is_excluded("Tests/VertexTransformProbe.cpp"))

    def test_clang_tidy_filters_to_changed_lines(self) -> None:
        root = Path("/repo")
        out = "\n".join([
            "/repo/Source/Foo.cpp:25:5: warning: uninitialized value [clang-analyzer-core.uninitialized.Assign]",
            "/repo/Source/Foo.cpp:99:5: warning: legacy issue untouched",
            "/repo/Source/Bar.cpp:10:1: error: untouched file",
        ])
        findings = cq.parse_clang_tidy_output(out, root, {"Source/Foo.cpp": {24, 25, 26}})
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].line, 25)
        self.assertEqual(findings[0].rule, "clang-analyzer-core.uninitialized.Assign")

    def test_cppcheck_filters_warnings_and_keeps_changed_file_errors(self) -> None:
        root = Path("/repo")
        out = "\n".join([
            "Source/Foo.cpp:12:warning:unreadVariable:Variable 'x' is assigned a value that is never used.",
            "Source/Foo.cpp:80:warning:unusedStructMember:Legacy warning outside diff.",
            "Source/Foo.cpp:40:error:nullPointer:Null pointer dereference.",
            "Source/Other.cpp:5:error:memleak:Memory leak.",
        ])
        findings = cq.parse_cppcheck_output(out, root, {"Source/Foo.cpp": {12}})
        self.assertEqual([(f.line, f.rule) for f in findings], [(12, "unreadVariable"), (40, "nullPointer")])

    def test_lizard_complexity_ratchet(self) -> None:
        limits = cq.QualityLimits(ccn_max=15, nloc_max=120, params_max=7)
        cur = [
            cq.FuncMetric("clean_new", 2, 20, 4, 1, 20),
            cq.FuncMetric("bloated_new", 8, 130, 18, 25, 160),
            cq.FuncMetric("legacy_unchanged", 4, 150, 20, 200, 355),
            cq.FuncMetric("legacy_worsened", 4, 150, 22, 400, 555),
        ]
        base = [
            cq.FuncMetric("legacy_unchanged", 4, 148, 20, 200, 353),
            cq.FuncMetric("legacy_worsened", 4, 140, 20, 400, 545),
        ]
        changed = {10, 30, 210, 410}
        findings = cq.evaluate_complexity_ratchet("Source/Foo.cpp", cur, base, changed, limits)
        rules = {(f.line, f.rule) for f in findings}
        self.assertEqual(
            rules,
            {
                (25, "ccn"),
                (25, "nloc"),
                (25, "params"),
                (400, "ccn-regression"),
                (400, "nloc-regression"),
            },
        )

    def test_ruff_filters_lines_and_catches_undefined_name(self) -> None:
        root = Path("/repo")
        out = "\n".join([
            "/repo/tools/helper.py:4:1: F401 `sys` imported but unused",
            "/repo/tools/helper.py:99:1: F401 legacy unused import",
            "/repo/tools/helper.py:88:5: F821 Undefined name `boom`",
        ])
        findings = cq.parse_ruff_output(out, root, {"tools/helper.py": {4}})
        self.assertEqual([(f.line, f.rule) for f in findings], [(4, "F401"), (88, "F821")])


if __name__ == "__main__":
    unittest.main()
