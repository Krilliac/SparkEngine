#!/usr/bin/env python3
"""BLD-100 diagnostics locate remaining differences without changing acceptance."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import struct
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / ".github/scripts/compare_windows_shipping_repro.py"
spec = importlib.util.spec_from_file_location("windows_repro", SCRIPT)
helper = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(helper)


def coff(sections: list[tuple[bytes, bytes]], timestamp: int = 0, outside: bytes = b"") -> bytes:
    header = struct.pack("<HHIIIHH", 0x8664, len(sections), timestamp, 0, 0, 0, 0)
    offset = 20 + 40 * len(sections)
    table = bytearray()
    for name, data in sections:
        table.extend(name.ljust(8, b"\0") + struct.pack("<IIIIIIHHI", 0, 0, len(data), offset, 0, 0, 0, 0, 0))
        offset += len(data)
    return header + bytes(table) + b"".join(data for _, data in sections) + outside


def archive(member: bytes, timestamp: int = 0) -> bytes:
    header = (f"unit.obj/       {timestamp:<12}{0:<6}{0:<6}{0:<8}{len(member):<10}`\n").encode("ascii")
    assert len(header) == 60
    return b"!<arch>\n" + header + member + (b"\n" if len(member) & 1 else b"")


class ShippingReproDiagnosticTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.roots = [self.directory / "first", self.directory / "second"]
        self.build_roots = ["C:\\tree-a\\build", "C:\\tree-b\\build"]

    def run_pair(self, first: bytes, second: bytes) -> tuple[int, dict | None, dict | None]:
        arguments = []
        for label, root, build_root, content in zip(("first", "second"), self.roots, self.build_roots, (first, second)):
            (root / "lib").mkdir(parents=True, exist_ok=True)
            (root / "lib/core.lib").write_bytes(content)
            manifest = self.directory / f"{label}.json"
            manifest.write_text(json.dumps(helper.comparator.build_manifest(root, build_root)), encoding="utf-8")
            arguments.extend((f"--{label}-manifest", str(manifest), f"--{label}-root", str(root),
                              f"--{label}-build-root", build_root))
        report, diagnostics = self.directory / "report.json", self.directory / "diagnostics.json"
        arguments.extend(("--report", str(report), "--diagnostics", str(diagnostics)))
        with contextlib.redirect_stdout(io.StringIO()):
            result = helper.main(arguments)
        return (result, json.loads(report.read_text()) if report.exists() else None,
                json.loads(diagnostics.read_text()) if diagnostics.exists() else None)

    def pair(self, section: bytes = b".debug$T", different: bool = True) -> tuple[bytes, bytes]:
        result = []
        for root in self.build_roots:
            member = coff([(b".text", b"code"), (section, root.encode() if different else b"types"),
                           (b".debug$S", root.encode() + b"\\unit.obj")])
            result.append(archive(member))
        return tuple(result)

    def test_type_section_root_is_reported_and_still_fails_original_comparison(self) -> None:
        code, report, detail = self.run_pair(*self.pair())
        self.assertEqual(code, 1)
        first = helper.comparator.load_manifest(self.directory / "first.json")
        second = helper.comparator.load_manifest(self.directory / "second.json")
        self.assertEqual(report, helper.comparator.compare_manifests(first, second))
        self.assertTrue(detail["diagnosticOnly"])
        library = detail["libraries"][0]
        self.assertEqual(library["member"], "unit.obj")
        self.assertEqual(library["memberIndex"], 0)
        self.assertEqual(library["differingRegionCount"], 1)
        region = library["differingRegions"][0]
        self.assertEqual(region["first"]["name"], ".debug$T (#1)")
        self.assertEqual(region["first"]["buildRootOccurrences"], 1)
        self.assertEqual(region["second"]["buildRootOccurrences"], 1)
        self.assertNotEqual(region["first"]["normalizedSha256"], region["second"]["normalizedSha256"])

    def test_only_authorized_debug_s_difference_remains_equivalent(self) -> None:
        code, report, detail = self.run_pair(*self.pair(different=False))
        self.assertEqual(code, 0)
        self.assertTrue(report["equivalent"])
        self.assertTrue(detail["acceptanceEquivalent"])
        self.assertEqual(detail["libraries"], [])

    def test_code_bytes_are_reported_without_normalization(self) -> None:
        pair = [archive(coff([(b".text", value), (b".debug$S", root.encode())]))
                for value, root in zip((b"one", b"two"), self.build_roots)]
        code, _, detail = self.run_pair(*pair)
        self.assertEqual(code, 1)
        region = detail["libraries"][0]["differingRegions"][0]
        self.assertEqual(region["first"]["name"], ".text (#0)")
        self.assertEqual(region["first"]["rawSha256"], region["first"]["normalizedSha256"])

    def test_archive_header_coff_header_and_uncovered_bytes_are_retained(self) -> None:
        pair = [archive(coff([(b".debug$S", root.encode())], timestamp=index, outside=bytes([index])),
                        timestamp=index)
                for index, root in enumerate(self.build_roots)]
        code, _, detail = self.run_pair(*pair)
        self.assertEqual(code, 1)
        names = [region["first"]["name"] for region in detail["libraries"][0]["differingRegions"]]
        self.assertEqual(names, ["<archive-member-header>", "<coff-header>", "<outside-sections>"])

    def test_member_budget_is_explicit_and_does_not_promote_failure(self) -> None:
        with mock.patch.object(helper, "MAX_MEMBER_BYTES", 8):
            code, report, detail = self.run_pair(*self.pair())
        self.assertEqual(code, 1)
        self.assertFalse(report["equivalent"])
        self.assertEqual(detail["libraries"][0]["status"], "member-exceeds-diagnostic-byte-limit")

    def test_changed_region_budget_records_omission_and_preserves_failure(self) -> None:
        pair = [archive(coff([(b".text", bytes([index])), (b".debug$T", root.encode()),
                             (b".debug$S", root.encode())]))
                for index, root in enumerate(self.build_roots)]
        with mock.patch.object(helper, "MAX_CHANGED_REGIONS", 1):
            code, _, detail = self.run_pair(*pair)
        self.assertEqual(code, 1)
        library = detail["libraries"][0]
        self.assertEqual(library["differingRegionCount"], 2)
        self.assertEqual(len(library["differingRegions"]), 1)
        self.assertEqual(library["omittedDifferingRegions"], 1)

    def test_reader_rejects_bytes_that_no_longer_match_the_manifest(self) -> None:
        content = self.pair()[0]
        root = self.roots[0]
        root.mkdir()
        file = root / "core.lib"
        file.write_bytes(content)
        entry = helper.comparator.build_manifest(root, self.build_roots[0])["entries"][0]["sections"][0]
        file.write_bytes(content.replace(b"code", b"evil"))
        with self.assertRaisesRegex(helper.comparator.InputError, "digest differs"):
            helper._member(file, 0, entry, self.build_roots[0].encode())

    def test_section_parser_rejects_out_of_bounds_and_overlapping_layouts(self) -> None:
        data = bytearray(coff([(b".text", b"code"), (b".debug$T", b"types")]))
        struct.pack_into("<I", data, 20 + 20, len(data) + 1)
        with self.assertRaisesRegex(helper.comparator.InputError, "exceeds"):
            helper._regions(bytes(data))
        data = bytearray(coff([(b".text", b"code"), (b".debug$T", b"types")]))
        struct.pack_into("<I", data, 60 + 20, 100)
        with self.assertRaisesRegex(helper.comparator.InputError, "overlapping"):
            helper._regions(bytes(data))

    def test_bigobj_sections_are_diagnosed_with_permitted_normalization_only(self) -> None:
        members = []
        for root in self.build_roots:
            ordinary = coff([(b".debug$T", root.encode()), (b".debug$S", root.encode())])
            header = struct.pack("<HHHHI", 0, 0xFFFF, 2, 0x8664, 0)
            header += bytes.fromhex("c7a1bad1eebaa94baf20faf66aa4dcb8") + bytes(16)
            header += struct.pack("<III", 2, 0, 0)
            sections = bytearray(ordinary[20:100])
            for index in range(2):
                offset = struct.unpack_from("<I", sections, 40 * index + 20)[0]
                struct.pack_into("<I", sections, 40 * index + 20, offset + 36)
            members.append(archive(header + bytes(sections) + ordinary[100:]))
        code, _, detail = self.run_pair(*members)
        self.assertEqual(code, 1)
        regions = detail["libraries"][0]["differingRegions"]
        self.assertEqual(len(regions), 1)
        self.assertEqual(regions[0]["first"]["name"], ".debug$T (#0)")

    def test_unequal_root_length_keeps_original_input_error(self) -> None:
        self.build_roots[1] += "x"
        with contextlib.redirect_stderr(io.StringIO()):
            code, report, detail = self.run_pair(*self.pair())
        self.assertEqual(code, 2)
        self.assertIsNone(report)
        self.assertIsNone(detail)

    def test_valid_short_import_members_retain_comparison_failure_and_unavailable_evidence(self) -> None:
        members = []
        for symbol in (b"symbolA", b"symbolB"):
            names = symbol + b"\0module.dll\0"
            # MSVC IMPORT_OBJECT_HEADER: version 0 distinguishes this valid
            # short import object from BigObj despite the shared signature.
            header = struct.pack("<HHHHIIHH", 0, 0xFFFF, 0, 0x8664, 0, len(names), 0, 4)
            members.append(archive(header + names))
        code, report, detail = self.run_pair(*members)
        self.assertEqual(code, 1)
        first = helper.comparator.load_manifest(self.directory / "first.json")
        second = helper.comparator.load_manifest(self.directory / "second.json")
        self.assertEqual(report, helper.comparator.compare_manifests(first, second))
        self.assertEqual(detail["libraries"][0]["status"], "inspection-unavailable")
        self.assertIn("short import object", detail["libraries"][0]["error"])

    def test_diagnostic_write_failure_preserves_both_comparison_outcomes(self) -> None:
        write_bytes = Path.write_bytes

        def write(path: Path, data: bytes) -> int:
            if path.name == "diagnostics.json":
                raise PermissionError(13, "diagnostic fixture write refused")
            return write_bytes(path, data)

        for different, expected in ((True, 1), (False, 0)):
            with self.subTest(different=different):
                errors = io.StringIO()
                with mock.patch.object(Path, "write_bytes", write), contextlib.redirect_stderr(errors):
                    code, report, detail = self.run_pair(*self.pair(different=different))
                self.assertEqual(code, expected)
                self.assertEqual(report["equivalent"], not different)
                self.assertIsNone(detail)
                self.assertIn("diagnostics unavailable", errors.getvalue())

    def test_diagnostic_read_error_is_retained_without_changing_failure(self) -> None:
        with mock.patch.object(helper, "_member", side_effect=OSError(5, "fixture I/O unavailable", "absolute/path")):
            code, report, detail = self.run_pair(*self.pair())
        self.assertEqual(code, 1)
        self.assertFalse(report["equivalent"])
        library = detail["libraries"][0]
        self.assertEqual(library["status"], "inspection-unavailable")
        self.assertEqual(library["error"], "fixture I/O unavailable")

    def test_global_diagnostic_failure_retains_bounded_error_and_original_status(self) -> None:
        with mock.patch.object(helper, "diagnostics", side_effect=ValueError("fixture unavailable")):
            code, report, detail = self.run_pair(*self.pair())
        self.assertEqual(code, 1)
        self.assertFalse(report["equivalent"])
        self.assertEqual(detail["status"], "inspection-unavailable")
        self.assertEqual(detail["error"], "fixture unavailable")


if __name__ == "__main__":
    unittest.main()
