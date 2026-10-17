"""TagReleaseContract: stable tag / source version / changelog contract (REL-100)."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from stable_release_tag import ContractError, changelog_heading_count, default_version, verify_stable_tag

HELPER = Path(__file__).resolve().with_name("stable_release_tag.py")
REPO_ROOT = Path(__file__).resolve().parents[2]
DECLARATION = 'set(SPARK_ENGINE_VERSION "1.2.3" CACHE STRING "Engine version")\n'
HEADING = "## [1.2.3] - 2026-09-07\n\n### Fixed\n- Fixture release note.\n"


class StableReleaseTagTests(unittest.TestCase):
    def test_accepts_matching_tag_version_and_single_heading(self):
        for changelog in (HEADING, "## [1.2.3]\n", "## [Unreleased]\n\n" + HEADING + "## [1.2.2] - 2026-01-01\n"):
            with self.subTest(changelog=changelog):
                self.assertEqual(verify_stable_tag("v1.2.3", DECLARATION, changelog), "1.2.3")

    def test_rejects_every_contract_violation(self):
        cases = (
            ("version mismatch", "v9.8.7", DECLARATION, "## [9.8.7]\n"),
            ("duplicate default", "v1.2.3", DECLARATION * 2, HEADING),
            ("conflicting default", "v1.2.3", DECLARATION + DECLARATION.replace("1.2.3", "9.8.7"), HEADING),
            ("multiline duplicate", "v1.2.3", DECLARATION + 'set(\n SPARK_ENGINE_VERSION "9.8.7")\n', HEADING),
            ("indented duplicate", "v1.2.3", DECLARATION + "  " + DECLARATION, HEADING),
            ("missing default", "v1.2.3", "", HEADING),
            ("non-cache default", "v1.2.3", 'set(SPARK_ENGINE_VERSION "1.2.3")\n', HEADING),
            ("unreleased only", "v1.2.3", DECLARATION, "## [Unreleased]\n"),
            ("missing changelog", "v1.2.3", DECLARATION, None),
            ("duplicate heading", "v1.2.3", DECLARATION, HEADING * 2),
            ("other release", "v1.2.3", DECLARATION, "## [1.2.30]\n"),
            ("nonheading mention", "v1.2.3", DECLARATION, "See [1.2.3] for details.\n"),
            ("level-3 heading", "v1.2.3", DECLARATION, "### [1.2.3]\n"),
            ("regex lookalike", "v1.2.3", DECLARATION, "## [1x2x3]\n"),
            ("prerelease tag", "v1.2.3-rc1", DECLARATION, HEADING),
            ("unprefixed tag", "1.2.3", DECLARATION, HEADING),
            ("nightly tag", "nightly-1-1-aaaaaaaaaaaa", DECLARATION, HEADING),
        )
        for label, tag, cmake, changelog in cases:
            with self.subTest(label=label):
                with self.assertRaises(ContractError):
                    verify_stable_tag(tag, cmake, changelog)

    def test_heading_count_is_exact(self):
        self.assertEqual(changelog_heading_count(HEADING * 3, "1.2.3"), 3)
        self.assertEqual(changelog_heading_count("## [1.2.3] - 2026-9-7\n", "1.2.3"), 0)

    def test_repository_declares_one_default_version(self):
        text = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertRegex(default_version(text), r"^[0-9]+\.[0-9]+\.[0-9]+$")


class StableReleaseTagCliTests(unittest.TestCase):
    def run_helper(self, root: Path, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([sys.executable, str(HELPER), "--root", str(root), *args],
                              text=True, capture_output=True, check=False)

    def test_cli_prints_version_only_when_the_contract_holds(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            (root / "CMakeLists.txt").write_text(DECLARATION, encoding="utf-8")
            default = self.run_helper(root, "default-version")
            self.assertEqual((default.returncode, default.stdout), (0, "1.2.3\n"), default.stderr)

            missing = self.run_helper(root, "verify", "v1.2.3")
            self.assertEqual((missing.returncode, missing.stdout), (1, ""))
            self.assertIn("requires CHANGELOG.md", missing.stderr)

            (root / "CHANGELOG.md").write_text(HEADING, encoding="utf-8")
            accepted = self.run_helper(root, "verify", "v1.2.3")
            self.assertEqual((accepted.returncode, accepted.stdout), (0, "1.2.3\n"), accepted.stderr)

            mismatch = self.run_helper(root, "verify", "v1.2.4")
            self.assertEqual((mismatch.returncode, mismatch.stdout), (1, ""))
            self.assertIn("must equal the single CMake SPARK_ENGINE_VERSION default", mismatch.stderr)

    def test_cli_fails_closed_without_cmakelists(self):
        with tempfile.TemporaryDirectory() as raw:
            result = self.run_helper(Path(raw), "default-version")
            self.assertEqual((result.returncode, result.stdout), (1, ""))
            self.assertIn("CMakeLists.txt is missing", result.stderr)


if __name__ == "__main__":
    unittest.main()
