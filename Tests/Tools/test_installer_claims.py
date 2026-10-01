"""INST-130: tools/installer/check_installer_claims.py ties installer docs to the code.

The real tree must pass, and each kind of drift between the README, --help,
main.cpp, the published build configuration and the exit codes must fail.
"""
from __future__ import annotations

import contextlib
import io
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "installer"))
import check_installer_claims as claims  # noqa: E402

FIXTURE_FILES = (
    claims.ROOT_README,
    claims.README,
    claims.MAIN,
    claims.INSTALLER_CMAKE,
    claims.RELEASE_WORKFLOW,
    *claims.EXIT_CODE_SOURCES,
    Path("SparkInstaller/src/InstallerPreflight.h"),
)


class InstallerClaimsTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory()
        self.root = Path(self._temporary.name)
        for relative in set(FIXTURE_FILES):
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPO_ROOT / relative, target)

    def tearDown(self) -> None:
        self._temporary.cleanup()

    def run_checker(self, root: Path) -> tuple[int, str]:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), contextlib.redirect_stdout(io.StringIO()):
            status = claims.main(["--root", str(root)])
        return status, stderr.getvalue()

    def edit(self, relative: Path, old: str, new: str) -> None:
        path = self.root / relative
        text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
        self.assertIn(old, text, f"fixture anchor missing from {relative}")
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    def assert_fails(self, needle: str) -> None:
        status, output = self.run_checker(self.root)
        self.assertEqual(1, status, output)
        self.assertIn(needle, output)

    def test_real_tree_passes(self) -> None:
        status, output = self.run_checker(REPO_ROOT)
        self.assertEqual(0, status, output)

    def test_unmodified_fixture_passes(self) -> None:
        status, output = self.run_checker(self.root)
        self.assertEqual(0, status, output)

    def test_readme_flag_missing_from_main_fails(self) -> None:
        self.edit(claims.README, "| `--headless` |", "| `--dry-run` | Pretend. |\n| `--headless` |")
        self.assert_fails("README documents --dry-run, which main.cpp does not accept")

    def test_main_flag_missing_from_readme_fails(self) -> None:
        self.edit(claims.README, "| `--skip-submodules` | Skip submodule update step in Update mode. |\n", "")
        self.assert_fails("main.cpp accepts --skip-submodules, which the README does not document")

    def test_main_flag_missing_from_help_fails(self) -> None:
        self.edit(claims.MAIN, '"  --repo <url>        Override repo URL\\n"', '""')
        self.assert_fails("main.cpp accepts --repo, which --help does not mention")

    def test_undocumented_exit_code_fails(self) -> None:
        self.edit(claims.README, "| 9 |", "| 99 |")
        status, output = self.run_checker(self.root)
        self.assertEqual(1, status, output)
        self.assertIn("the installer returns 9, which the README does not document", output)
        self.assertIn("the README documents 99, which the installer never returns", output)

    def test_gui_without_source_build_marker_fails(self) -> None:
        readme = (self.root / claims.README).read_text(encoding="utf-8").replace("\r\n", "\n")
        row = next(line for line in readme.splitlines() if line.startswith("| `--gui` |"))
        self.edit(claims.README, row, "| `--gui` | Launch the ImGui wizard instead of the terminal UI. |")
        self.assert_fails("--gui is compiled only with SPARKINSTALLER_ENABLE_GUI")

    def test_source_build_marker_on_published_capability_fails(self) -> None:
        self.edit(claims.RELEASE_WORKFLOW, "-DSPARKINSTALLER_ENABLE_GUI=OFF", "-DSPARKINSTALLER_ENABLE_GUI=ON")
        self.assert_fails("does not set SPARKINSTALLER_ENABLE_GUI OFF")

    def test_source_build_marker_on_ungated_flag_fails(self) -> None:
        self.edit(
            claims.README,
            "| `--headless` | Non-interactive; fails if required inputs are missing. |",
            "| `--headless` | Non-interactive. Source-build only. |",
        )
        self.assert_fails("README marks --headless 'source-build only', but main.cpp does not gate it")

    def test_missing_flags_table_is_unreadable_input(self) -> None:
        self.edit(claims.README, "### Flags", "### Options")
        status, _ = self.run_checker(self.root)
        self.assertEqual(2, status)

    def test_renamed_preflight_code_fails(self) -> None:
        self.edit(claims.README, "`destination-link`", "`destination-link-renamed`")
        self.assert_fails("README documents destination-link-renamed")

    def test_extra_preflight_code_fails(self) -> None:
        self.edit(
            claims.README,
            "| `cmake-unavailable` |",
            "| Extra check | `made-up-preflight-code` |\n| `cmake-unavailable` |",
        )
        self.assert_fails("README documents made-up-preflight-code")

    def test_changed_disk_budget_fails(self) -> None:
        self.edit(claims.README, "| Install with a build | 40 GiB |", "| Install with a build | 41 GiB |")
        self.assert_fails("install-with-build value")

    def test_root_readme_unknown_installer_flag_fails(self) -> None:
        self.edit(claims.ROOT_README, "The bootstrap installer clones", "The bootstrap installer --made-up clones")
        self.assert_fails("root README: documents --made-up")


class InstallerClaimsInMemoryTests(unittest.TestCase):
    """Exercise the real checker on source text without temporary-directory support."""

    def test_real_tree_and_hostile_capability_mutations(self) -> None:
        self.assertEqual(claims.check(REPO_ROOT), [])
        original_read = claims.read_text
        mutations = (
            (claims.README, "`destination-link`", "`wrong-code`", "preflight codes"),
            (claims.README, "`destination-link`", "`destination-link`, `extra-code`", "preflight codes"),
            (claims.README, "| Install with a build | 40 GiB |", "| Install with a build | 41 GiB |", "disk budget"),
            (claims.README, "| Install with `--skip-build` | 2 GiB |", "| Install with `--skip-build` | 3 GiB |", "disk budget"),
            (claims.ROOT_README, "[installer documentation]", "SparkInstaller --nonexistent [installer documentation]", "root README"),
        )
        for relative, before, after, error in mutations:
            with self.subTest(relative=relative, before=before):
                source = original_read(REPO_ROOT, relative)
                self.assertIn(before, source)

                def read(root, path):
                    return source.replace(before, after, 1) if path == relative else original_read(root, path)

                with mock.patch.object(claims, "read_text", side_effect=read):
                    self.assertTrue(any(error in message for message in claims.check(REPO_ROOT)))

    def test_disk_unit_definition_cannot_change_silently(self) -> None:
        readme = claims.read_text(REPO_ROOT, claims.README)
        header = claims.read_text(REPO_ROOT, Path("SparkInstaller/src/InstallerPreflight.h"))
        with self.assertRaisesRegex(claims.ClaimsError, "1024 cubed"):
            claims.preflight_budgets(readme, header.replace("1024ull", "1000ull"))


if __name__ == "__main__":
    unittest.main()
