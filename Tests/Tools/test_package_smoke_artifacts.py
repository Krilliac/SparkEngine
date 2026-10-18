#!/usr/bin/env python3
"""Negative contract tests for the package-smoke runtime fields."""

from __future__ import annotations

import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/module-evidence"))
import artifacts  # noqa: E402


def valid_log() -> str:
    return "\n".join([
        "[package-smoke] schema=package-smoke-v1",
        "[package-smoke] product=SparkEngine",
        "[package-smoke] profile=stable-v1",
        "[package-smoke] module=SparkGameFPS",
        f"[package-smoke] commit_sha={'a' * 40}",
        f"[package-smoke] msi_sha256={'b' * 64}",
        "[package-smoke] package_runtime=PASS",
        "[package-smoke] asset_integrity=PASS",
        "[package-smoke] asset_entries=12",
        "[package-smoke] authored_scene_visual=PASS",
        "[package-smoke] save_reload=PASS",
        "[package-smoke] repository_isolation=PASS",
        "[package-smoke] backend=nullrhi result=PASS",
        "[package-smoke] backend=d3d11-warp result=PASS",
        "[package-smoke] exit_code=0",
        "[package-smoke] PASS",
        "",
    ])


class PackageSmokeArtifactTests(unittest.TestCase):
    def errors(self, text: str) -> list[str]:
        return artifacts.validate_package_smoke_bytes(
            text.encode(), "package-smoke.log", "SparkGameFPS", expected_sha="a" * 40,
        )

    def test_missing_runtime_field_rejected(self) -> None:
        self.assertTrue(any("save_reload" in error for error in self.errors(
            valid_log().replace("[package-smoke] save_reload=PASS\n", ""))))

    def test_valid_record_passes(self) -> None:
        self.assertEqual(self.errors(valid_log()), [])

    def test_duplicate_runtime_field_rejected(self) -> None:
        self.assertTrue(any("duplicates field 'asset_entries'" in error for error in self.errors(
            valid_log().replace("[package-smoke] PASS\n", "[package-smoke] asset_entries=12\n[package-smoke] PASS\n"))))

    def test_zero_count_rejected(self) -> None:
        self.assertTrue(any("asset_entries" in error and "positive" in error for error in self.errors(
            valid_log().replace("asset_entries=12", "asset_entries=0"))))

    def test_bad_runtime_status_rejected(self) -> None:
        self.assertTrue(any("authored_scene" in error and "PASS" in error for error in self.errors(
            valid_log().replace("authored_scene_visual=PASS", "authored_scene_visual=FAIL"))))


if __name__ == "__main__":
    unittest.main()
