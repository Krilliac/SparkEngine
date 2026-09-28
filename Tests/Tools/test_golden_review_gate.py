#!/usr/bin/env python3
"""PERF-100: tools/perf-budget/check_golden_review.py rejects unreviewed baseline changes.

Each case builds a throwaway git repository with a base commit (a manifest and
its PNG) and one head commit, then runs the gate's CLI on base..head.
"""

from __future__ import annotations

import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "perf-budget"))

import check_golden_review  # noqa: E402

REVIEWED = "Owner Name, 2026-09-01 (WARP 10.0.26100.9278)"
PENDING = "Claude agent, RHI-210 (WARP 10.0.26100.9278; owner review pending)"


def _entry(scene: str, row: str, sha: str, reviewer: str, **overrides: Any) -> dict[str, Any]:
    entry = {
        "scene": scene,
        "backendRow": row,
        "software": row != "d3d11-hw",
        "perPixelThreshold": 2,
        "tolerancePercent": 0.5,
        "reviewer": reviewer,
        "baselineSha256": sha * 64,
    }
    entry.update(overrides)
    return entry


class GoldenReviewGateTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name)
        self.git("init", "-q")
        self.git("config", "user.email", "gate@example.invalid")
        self.git("config", "user.name", "gate")
        self.git("config", "commit.gpgsign", "false")
        self.golden = self.repo / "Tests" / "GoldenImages"
        (self.golden / "d3d11-warp").mkdir(parents=True)
        self.entries = [_entry("PassA", "d3d11-warp", "a", PENDING), _entry("PassB", "d3d11-warp", "b", REVIEWED)]
        self.write_png("PassA", b"png-a")
        self.write_png("PassB", b"png-b")
        self.base = self.commit("base")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def git(self, *args: str) -> str:
        return subprocess.run(
            ["git", "-C", str(self.repo), *args], check=True, capture_output=True, text=True
        ).stdout.strip()

    def write_png(self, scene: str, data: bytes, row: str = "d3d11-warp") -> None:
        (self.golden / row).mkdir(exist_ok=True)
        (self.golden / row / f"{scene}.png").write_bytes(data)

    def commit(self, message: str) -> str:
        document = {"schemaVersion": 1, "entries": self.entries}
        (self.golden / "manifest.json").write_text(json.dumps(document, indent=2), encoding="utf-8")
        self.git("add", "-A")
        self.git("commit", "-q", "--allow-empty", "-m", message)
        return self.git("rev-parse", "HEAD")

    def run_gate(self, *extra: str, base: str | None = None) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        argv = ["--repo", str(self.repo), "--base", self.base if base is None else base, *extra]
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = check_golden_review.main(argv)
        return code, out.getvalue(), err.getvalue()

    def assertRejected(self, needle: str) -> None:
        code, _, err = self.run_gate()
        self.assertEqual(1, code, err)
        self.assertIn(needle, err)

    def test_threshold_change_with_unchanged_reviewer_is_rejected(self) -> None:
        self.entries[1]["perPixelThreshold"] = 6
        self.commit("loosen threshold")
        self.assertRejected("d3d11-warp/PassB: baseline or threshold changed but the reviewer record is unchanged")

    def test_whitespace_only_reviewer_edit_does_not_count_as_a_new_review(self) -> None:
        self.entries[1]["tolerancePercent"] = 2
        self.entries[1]["reviewer"] = "  " + REVIEWED.replace(" ", "   ") + " "
        self.commit("whitespace review")
        self.assertRejected("reviewer record is unchanged")

    def test_png_and_hash_change_with_new_reviewer_is_accepted(self) -> None:
        self.write_png("PassA", b"png-a-v2")
        self.entries[0].update(baselineSha256="c" * 64, reviewer=PENDING.replace("RHI-210", "RHI-210 v2"))
        self.commit("rebaseline")
        code, out, err = self.run_gate()
        self.assertEqual(0, code, err)
        self.assertIn("checked 1 changed entries", out)

    def test_reviewed_to_pending_downgrade_is_rejected(self) -> None:
        self.write_png("PassB", b"png-b-v2")
        self.entries[1].update(baselineSha256="d" * 64, reviewer=PENDING)
        self.commit("downgrade")
        self.assertRejected("a reviewed baseline cannot be replaced")

    def test_pending_marker_on_hardware_row_is_rejected(self) -> None:
        self.write_png("PassA", b"png-hw", row="d3d11-hw")
        self.entries.append(_entry("PassA", "d3d11-hw", "e", PENDING))
        self.commit("hardware pending")
        self.assertRejected("'owner review pending' cannot back hardware row 'd3d11-hw'")

    def test_png_change_without_manifest_change_is_rejected(self) -> None:
        self.write_png("PassB", b"png-b-silently-replaced")
        self.commit("swap png")
        self.assertRejected("d3d11-warp/PassB.png: PNG changed but its manifest entry did not")

    def test_new_entry_is_checked_and_accepted(self) -> None:
        self.write_png("PassC", b"png-c")
        self.entries.append(_entry("PassC", "d3d11-warp", "f", PENDING))
        self.commit("add scene")
        code, out, err = self.run_gate()
        self.assertEqual(0, code, err)
        self.assertIn("checked 1 changed entries", out)

    def test_removed_entry_and_png_are_accepted(self) -> None:
        (self.golden / "d3d11-warp" / "PassA.png").unlink()
        self.entries.pop(0)
        self.commit("drop scene")
        code, out, err = self.run_gate()
        self.assertEqual(0, code, err)
        self.assertIn("checked 0 changed entries", out)

    def test_invalid_head_manifest_is_rejected(self) -> None:
        self.entries[0]["baselineSha256"] = "not-a-sha"
        self.commit("broken manifest")
        self.assertRejected("invalid manifest")

    def test_unknown_base_fails_closed(self) -> None:
        for base in ("f" * 40, "0" * 40, "", "no-such-ref"):
            with self.subTest(base=base):
                code, out, err = self.run_gate(base=base)
                self.assertEqual(2, code, out + err)
                self.assertIn("cannot resolve base revision", err)

    def test_unknown_base_uses_fallback_merge_base(self) -> None:
        self.git("branch", "Working", self.base)
        self.entries[1]["perPixelThreshold"] = 6
        self.commit("loosen threshold")
        code, _, err = self.run_gate("--fallback-base", "Working", base="0" * 40)
        self.assertEqual(1, code, err)
        self.assertIn("reviewer record is unchanged", err)

    def test_no_golden_change_is_accepted_visibly(self) -> None:
        (self.repo / "unrelated.txt").write_text("x", encoding="utf-8")
        self.commit("unrelated")
        code, out, err = self.run_gate()
        self.assertEqual(0, code, err)
        self.assertIn("checked 0 changed entries", out)


class GoldenReviewGateLiveTests(unittest.TestCase):
    def test_committed_manifest_loads_through_the_gate(self) -> None:
        """The real manifest parses at HEAD (works on a depth-1 checkout)."""
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = check_golden_review.main(["--base", "HEAD"])
        self.assertEqual(0, code, err.getvalue())
        self.assertIn("checked 0 changed entries", out.getvalue())
        entries, errors = check_golden_review.manifest_at(REPO_ROOT, "HEAD")
        self.assertEqual([], errors)
        self.assertGreater(len(entries), 0)


if __name__ == "__main__":
    unittest.main()
