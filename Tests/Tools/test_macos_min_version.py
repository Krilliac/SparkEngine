#!/usr/bin/env python3
"""PLT-220: tools/check_macos_min_version.py keeps the macOS floor in parity.

* synthetic thin, byte-swapped and universal Mach-O images built with
  struct.pack decode to their LC_BUILD_VERSION / LC_VERSION_MIN_MACOSX minimum;
* universal slices that disagree, images with no version command, non-macOS
  platforms, and truncated or garbage headers are errors, never a pass;
* the live tree agrees: SPARK_MACOS_MIN_VERSION == every CI value == every
  documentation anchor, and the documented runner is the workflow runner;
* a copy of the tree with one drifted anchor, workflow value, runner, or a
  removed anchor fails, both through parity_errors and the CLI exit code.
"""

from __future__ import annotations

import importlib.util
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CHECKER_PATH = REPO_ROOT / "tools" / "check_macos_min_version.py"

_spec = importlib.util.spec_from_file_location("check_macos_min_version", CHECKER_PATH)
assert _spec is not None and _spec.loader is not None
checker = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = checker
_spec.loader.exec_module(checker)

CPU_TYPE_ARM64 = 0x0100000C
CPU_TYPE_X86_64 = 0x01000007
LC_SEGMENT_64 = 0x19


def packed(major: int, minor: int, patch: int = 0) -> int:
    return (major << 16) | (minor << 8) | patch


def build_version_cmd(order: str, minos: int, platform: int = 1) -> bytes:
    # cmd, cmdsize, platform, minos, sdk, ntools
    return struct.pack(order + "IIIIII", 0x32, 24, platform, minos, packed(15, 0), 0)


def version_min_cmd(order: str, version: int) -> bytes:
    return struct.pack(order + "IIII", 0x24, 16, version, packed(14, 0))


def filler_cmd(order: str) -> bytes:
    # An unrelated load command the walker must step over.
    return struct.pack(order + "II", LC_SEGMENT_64, 16) + b"\0" * 8


def thin_image(commands: list[bytes], order: str = "<", cputype: int = CPU_TYPE_ARM64) -> bytes:
    body = b"".join(commands)
    header = struct.pack(order + "IIIIIIII", 0xFEEDFACF, cputype, 0, 2, len(commands), len(body), 0, 0)
    return header + body


def fat_image(slices: list[bytes], fat64: bool = False) -> bytes:
    magic = 0xCAFEBABF if fat64 else 0xCAFEBABE
    entry = ">IIQQII" if fat64 else ">IIIII"
    header_size = 8 + len(slices) * struct.calcsize(entry)
    offset = (header_size + 0xFFF) & ~0xFFF
    table = b""
    payload = b""
    for index, image in enumerate(slices):
        cputype = CPU_TYPE_ARM64 if index % 2 == 0 else CPU_TYPE_X86_64
        fields = (cputype, 0, offset + len(payload), len(image), 12)
        table += struct.pack(entry, *(fields + ((0,) if fat64 else ())))
        payload += image
    header = struct.pack(">II", magic, len(slices)) + table
    return header + b"\0" * (offset - len(header)) + payload


class MachOParsingTests(unittest.TestCase):
    def test_thin_arm64_build_version(self) -> None:
        image = thin_image([filler_cmd("<"), build_version_cmd("<", packed(13, 3))])
        self.assertEqual((13, 3), checker.macho_min_version_bytes(image))

    def test_patch_component_is_preserved(self) -> None:
        image = thin_image([build_version_cmd("<", packed(13, 3, 1))])
        self.assertEqual((13, 3, 1), checker.macho_min_version_bytes(image))

    def test_legacy_version_min_macosx(self) -> None:
        image = thin_image([version_min_cmd("<", packed(12, 0))], cputype=CPU_TYPE_X86_64)
        self.assertEqual((12,), checker.macho_min_version_bytes(image))

    def test_big_endian_image(self) -> None:
        image = thin_image([build_version_cmd(">", packed(13, 3))], order=">")
        self.assertEqual((13, 3), checker.macho_min_version_bytes(image))

    def test_universal_image_with_agreeing_slices(self) -> None:
        arm = thin_image([build_version_cmd("<", packed(13, 3))])
        intel = thin_image([build_version_cmd("<", packed(13, 3))], cputype=CPU_TYPE_X86_64)
        for fat64 in (False, True):
            with self.subTest(fat64=fat64):
                self.assertEqual((13, 3), checker.macho_min_version_bytes(fat_image([arm, intel], fat64)))

    def test_universal_image_with_differing_slices_is_rejected(self) -> None:
        arm = thin_image([build_version_cmd("<", packed(13, 3))])
        intel = thin_image([build_version_cmd("<", packed(11, 0))], cputype=CPU_TYPE_X86_64)
        with self.assertRaisesRegex(checker.MachOError, "different minimum versions: 11, 13.3"):
            checker.macho_min_version_bytes(fat_image([arm, intel]))

    def test_conflicting_commands_in_one_image_are_rejected(self) -> None:
        image = thin_image([build_version_cmd("<", packed(13, 3)), version_min_cmd("<", packed(11, 0))])
        with self.assertRaisesRegex(checker.MachOError, "conflicting"):
            checker.macho_min_version_bytes(image)

    def test_image_without_version_command_is_rejected(self) -> None:
        with self.assertRaisesRegex(checker.MachOError, "no LC_BUILD_VERSION"):
            checker.macho_min_version_bytes(thin_image([filler_cmd("<")]))

    def test_non_macos_platform_is_rejected(self) -> None:
        image = thin_image([build_version_cmd("<", packed(17, 0), platform=2)])
        with self.assertRaisesRegex(checker.MachOError, "not macOS"):
            checker.macho_min_version_bytes(image)

    def test_truncated_and_garbage_input_is_rejected(self) -> None:
        good = thin_image([build_version_cmd("<", packed(13, 3))])
        for label, data in (
            ("empty", b""),
            ("garbage", b"MZ\x90\x00" + b"\0" * 64),
            ("truncated header", good[:20]),
            ("truncated commands", good[:-4]),
            ("fat slice outside file", fat_image([good])[:-8]),
            ("zero fat slices", struct.pack(">II", 0xCAFEBABE, 0)),
        ):
            with self.subTest(label=label), self.assertRaises(checker.MachOError):
                checker.macho_min_version_bytes(data)

    def test_binary_argument_is_checked_against_the_canonical_value(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            good = Path(tmp) / "good"
            stale = Path(tmp) / "stale"
            good.write_bytes(thin_image([build_version_cmd("<", packed(13, 3))]))
            stale.write_bytes(thin_image([build_version_cmd("<", packed(11, 0))]))
            self.assertEqual([], checker.parity_errors(REPO_ROOT, [good]))
            errors = checker.parity_errors(REPO_ROOT, [good, stale])
            self.assertEqual(1, len(errors), errors)
            self.assertIn("Mach-O minimum 11 != SPARK_MACOS_MIN_VERSION 13.3", errors[0])


def copy_tree_subset(destination: Path) -> None:
    """Copy exactly the files the checker reads into ``destination``."""
    relatives = {"CMakeLists.txt", "CMakePresets.json"}
    relatives.update(anchor[0] for anchor in checker.DOC_VERSION_ANCHORS)
    relatives.update(anchor[0] for anchor in checker.DOC_RUNNER_ANCHORS)
    relatives.update(p.relative_to(REPO_ROOT).as_posix() for p in (REPO_ROOT / ".github" / "workflows").glob("*.yml"))
    for relative in relatives:
        source = REPO_ROOT / relative
        if source.is_file():
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)


def replace_in(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    if old not in text:
        raise AssertionError(f"{path}: fixture text {old!r} not found")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


class LiveTreeParityTests(unittest.TestCase):
    def test_live_tree_is_in_parity(self) -> None:
        self.assertEqual([], checker.parity_errors(REPO_ROOT))

    def test_live_tree_has_values_to_compare(self) -> None:
        expected = checker.canonical(REPO_ROOT)
        ci = checker.ci_values(REPO_ROOT)
        docs, doc_errors = checker.doc_values(REPO_ROOT)
        self.assertEqual([], doc_errors)
        self.assertGreaterEqual(len(ci), 2, "build.yml and release.yml must both pass the deployment target")
        self.assertGreaterEqual(len(docs), len(checker.DOC_VERSION_ANCHORS))
        for location, raw in ci + docs:
            with self.subTest(location=location):
                self.assertEqual(expected, checker.parse_version(raw))
        self.assertEqual({"macos-15"}, checker.ci_runners(REPO_ROOT))


class DriftDetectionTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        copy_tree_subset(self.root)
        self.assertEqual([], checker.parity_errors(self.root))

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def run_cli(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-B", str(CHECKER_PATH), "--repo-root", str(self.root)],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_clean_copy_passes_the_cli(self) -> None:
        result = self.run_cli()
        self.assertEqual(0, result.returncode, result.stderr)

    def test_drifted_doc_anchor_fails(self) -> None:
        replace_in(self.root / "wiki/Home.md", "| macOS 13.3+ |", "| macOS 12.0+ |")
        errors = checker.parity_errors(self.root)
        self.assertEqual(1, len(errors), errors)
        self.assertIn("wiki/Home.md", errors[0])
        result = self.run_cli()
        self.assertEqual(1, result.returncode)
        self.assertIn("wiki/Home.md", result.stderr)

    def test_drifted_workflow_value_fails(self) -> None:
        replace_in(
            self.root / ".github/workflows/release.yml",
            "CMAKE_OSX_DEPLOYMENT_TARGET=13.3",
            "CMAKE_OSX_DEPLOYMENT_TARGET=14.0",
        )
        errors = checker.parity_errors(self.root)
        self.assertEqual(1, len(errors), errors)
        self.assertIn(".github/workflows/release.yml", errors[0])

    def test_changed_canonical_value_fails_every_unchanged_consumer(self) -> None:
        replace_in(self.root / "CMakeLists.txt", 'SPARK_MACOS_MIN_VERSION "13.3"', 'SPARK_MACOS_MIN_VERSION "14.0"')
        errors = checker.parity_errors(self.root)
        self.assertTrue(any("release.yml" in error for error in errors), errors)
        self.assertTrue(any("README.md" in error for error in errors), errors)

    def test_removed_anchor_fails_instead_of_passing_vacuously(self) -> None:
        replace_in(self.root / "README.md", "| OS (build floor", "| Operating system (build floor")
        errors = checker.parity_errors(self.root)
        self.assertEqual(1, len(errors), errors)
        self.assertIn("README.md: no line matches", errors[0])

    def test_missing_canonical_declaration_fails(self) -> None:
        replace_in(self.root / "CMakeLists.txt", "set(SPARK_MACOS_MIN_VERSION", "set(SPARK_MACOS_FLOOR")
        errors = checker.parity_errors(self.root)
        self.assertEqual(1, len(errors), errors)
        self.assertIn("exactly once", errors[0])

    def test_documented_runner_must_match_the_workflows(self) -> None:
        replace_in(self.root / "wiki/advanced/Testing.md", "| `build-macos` | macos-15 |", "| `build-macos` | macos-latest |")
        errors = checker.parity_errors(self.root)
        self.assertEqual(1, len(errors), errors)
        self.assertIn("documents runner macos-latest", errors[0])


if __name__ == "__main__":
    unittest.main()
