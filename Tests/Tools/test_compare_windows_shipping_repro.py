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


def bigobj(sections: list[tuple[bytes, bytes]], timestamp: int = 0) -> bytes:
    ordinary = coff(sections)
    header = struct.pack("<HHHHI", 0, 0xFFFF, 2, 0x8664, timestamp)
    header += bytes.fromhex("c7a1bad1eebaa94baf20faf66aa4dcb8") + bytes(16)
    header += struct.pack("<III", len(sections), 0, 0)
    table_end = 20 + 40 * len(sections)
    table = bytearray(ordinary[20:table_end])
    for index in range(len(sections)):
        offset = struct.unpack_from("<I", table, 40 * index + 20)[0]
        struct.pack_into("<I", table, 40 * index + 20, offset + 36)
    return header + bytes(table) + ordinary[table_end:]


def type_record(kind: int, payload: bytes) -> bytes:
    count = (-len(payload) - 4) % 4
    padding = bytes(0xF0 + value for value in range(count, 0, -1))
    return struct.pack("<HH", len(payload) + len(padding) + 2, kind) + payload + padding


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

    def test_v2_header_fields_checksum_bytes_and_directive_strings_retain_strict_failure(self) -> None:
        members = [archive(bigobj([(b".chks64", struct.pack("<QQ", 7, value)),
                                  (b".drectve", b'/FAILIFMISMATCH:"pch=tree-' + suffix + b'"')],
                                 timestamp=value))
                   for value, suffix in ((3, b"a"), (4, b"b"))]
        code, report, detail = self.run_pair(*members)
        self.assertEqual(code, 1)
        self.assertFalse(report["equivalent"])
        self.assertEqual(detail["schema"], "spark.windows-shipping-repro-diagnostics/2")
        regions = detail["libraries"][0]["differingRegions"]
        header, checksum, directive = [region["details"] for region in regions]
        self.assertEqual(header["headers"]["first"]["format"], "coff-bigobj")
        self.assertEqual(header["fieldDifferences"], [{"field": "timeDateStamp", "first": 3, "second": 4}])
        self.assertEqual(header["byteDifferences"]["samples"], [{"offset": 8, "firstHex": "03", "secondHex": "04"}])
        self.assertEqual(checksum["byteDifferences"]["samples"], [{"offset": 8, "firstHex": "03", "secondHex": "04"}])
        self.assertEqual(checksum["byteDifferences"]["omittedDifferingBytes"], 0)
        self.assertEqual(directive["stringWindows"]["first"]["windows"][0]["text"], '/FAILIFMISMATCH:"pch=tree-a"')
        self.assertEqual(directive["stringWindows"]["second"]["windows"][0]["text"], '/FAILIFMISMATCH:"pch=tree-b"')
        self.assertEqual(detail["limits"]["bytesPerSample"], 16)
        self.assertEqual(detail["limits"]["codeViewRecordsPerRegion"], 65536)

    def test_ordinary_header_decoder_identifies_timestamp_without_discarding_bytes(self) -> None:
        code, _, detail = self.run_pair(archive(coff([(b".text", b"same")], timestamp=9)),
                                       archive(coff([(b".text", b"same")], timestamp=10)))
        self.assertEqual(code, 1)
        decoded = detail["libraries"][0]["differingRegions"][0]["details"]
        self.assertEqual(decoded["headers"]["first"]["format"], "coff")
        self.assertEqual(decoded["fieldDifferences"], [{"field": "timeDateStamp", "first": 9, "second": 10}])

    def test_residual_codeview_records_and_source_strings_are_identified(self) -> None:
        pair = []
        for suffix in (b"a", b"b"):
            types = struct.pack("<I", 4) + type_record(0x1605, bytes(4) + b"C:\\source-" + suffix + b"\\file.cpp\0")
            symbols = struct.pack("<III", 4, 0xF3, 4) + b"x" + suffix + b"\0\0"
            pair.append(archive(coff([(b".debug$T", types), (b".debug$S", symbols)])))
        code, _, detail = self.run_pair(*pair)
        self.assertEqual(code, 1)
        types, symbols = [region["details"] for region in detail["libraries"][0]["differingRegions"]]
        record = types["codeView"]["first"]["sampledChangeRecords"][0]
        self.assertEqual((record["offset"], record["typeIndex"], record["kindName"]), (4, 0x1000, "LF_STRING_ID"))
        self.assertEqual(types["stringWindows"]["first"]["windows"][0]["text"], "C:\\source-a\\file.cpp")
        self.assertEqual(symbols["codeView"]["second"]["sampledChangeRecords"][0]["kindName"], "string-table")

    def test_unknown_and_truncated_codeview_are_advisory_with_exact_sample_values(self) -> None:
        for data in (b"not-c13", struct.pack("<IHH", 4, 100, 0x1605),
                     struct.pack("<I", 4) + type_record(0x9999, b"value")):
            with self.subTest(data=data):
                first, second = bytearray(data), bytearray(data)
                second[-1] ^= 1
                code, _, detail = self.run_pair(archive(coff([(b".debug$T", bytes(first))])),
                                               archive(coff([(b".debug$T", bytes(second))])))
                self.assertEqual(code, 1)
                decoded = detail["libraries"][0]["differingRegions"][0]["details"]
                self.assertEqual(decoded["byteDifferences"]["differingByteCount"], 1)
                if data[:4] != struct.pack("<I", 4) or len(data) == 8:
                    self.assertEqual(decoded["codeView"]["first"]["status"], "inspection-unavailable")
                else:
                    self.assertEqual(decoded["codeView"]["first"]["sampledChangeRecords"][0]["kindName"], "unrecognized")
                    self.assertEqual(decoded["codeView"]["first"]["sampledChangeRecords"][0]["status"],
                                     "inspection-unavailable")

    def test_byte_string_and_codeview_work_limits_are_explicit(self) -> None:
        delta = helper._byte_differences(b"a" * 2048, b"b" * 2048)
        self.assertEqual(delta["differingByteCount"], 2048)
        self.assertEqual(len(delta["samples"]), helper.MAX_BYTE_SAMPLES)
        self.assertEqual(delta["omittedDifferingBytes"], 2048 - helper.MAX_BYTE_SAMPLES * helper.MAX_SAMPLE_BYTES)
        self.assertTrue(all(len(sample["firstHex"]) <= helper.MAX_SAMPLE_BYTES * 2 for sample in delta["samples"]))
        text = b"\0".join(b"a" * 400 for _ in range(12))
        strings = helper._string_windows(text, [{"offset": 401 * index + 200} for index in range(12)])
        windows = strings["windows"]
        self.assertEqual(len(windows), helper.MAX_STRING_SAMPLES)
        self.assertEqual(strings["omittedWindows"], 4)
        self.assertTrue(all(len(window["text"]) <= helper.MAX_STRING_BYTES for window in windows))
        self.assertTrue(all(window["truncatedBefore"] and window["truncatedAfter"] for window in windows))
        data = struct.pack("<I", 4) + type_record(0x1605, b"a\0") + type_record(0x1605, b"b\0")
        with mock.patch.object(helper, "MAX_CODEVIEW_RECORDS", 1):
            decoded = helper._region_details(".debug$T (#0)", data, data[:-1] + b"x")
        self.assertEqual(decoded["codeView"]["first"]["status"], "inspection-unavailable")
        self.assertIn("limit", decoded["codeView"]["first"]["error"])

    def test_library_cap_reports_omitted_libraries_without_changing_comparison(self) -> None:
        self.run_pair(*self.pair())
        for root in self.roots:
            content = (root / "lib/core.lib").read_bytes()
            for name in ("extra-a.lib", "extra-b.lib"):
                (root / "lib" / name).write_bytes(content)
        manifests = [helper.comparator.build_manifest(root, build_root)
                     for root, build_root in zip(self.roots, self.build_roots)]
        report = helper.comparator.compare_manifests(*manifests)
        with mock.patch.object(helper, "MAX_LIBRARIES", 1):
            detail = helper.diagnostics(*manifests, *self.roots, *self.build_roots, report)
        self.assertFalse(report["equivalent"])
        self.assertEqual(len(report["differing"]), 3)
        self.assertEqual(len(detail["libraries"]), 1)
        self.assertEqual(detail["omittedLibraries"], 2)

    def test_invalid_codeview_symbol_subsections_do_not_hide_precise_byte_deltas(self) -> None:
        for data in (struct.pack("<III", 4, 0xF3, 100) + b"a",
                     struct.pack("<III", 4, 0xF3, 1) + b"a"):
            with self.subTest(data=data):
                detail = helper._region_details(".debug$S (#0)", data, data[:-1] + b"b")
                self.assertEqual(detail["byteDifferences"]["samples"],
                                 [{"offset": 12, "firstHex": "61", "secondHex": "62"}])
                self.assertEqual(detail["codeView"]["first"]["status"], "inspection-unavailable")

    def test_new_decode_errors_and_output_cap_preserve_authoritative_failure(self) -> None:
        members = [archive(coff([(b".text", b"same")], timestamp=index)) for index in (1, 2)]
        with mock.patch.object(helper, "_header_fields", side_effect=struct.error("fixture undecodable")):
            code, report, detail = self.run_pair(*members)
        self.assertEqual(code, 1)
        self.assertFalse(report["equivalent"])
        self.assertEqual(detail["libraries"][0]["differingRegions"][0]["details"]["status"], "inspection-unavailable")
        for pair, expected in ((members, 1), ((members[0], members[0]), 0)):
            with self.subTest(expected=expected), mock.patch.object(helper, "MAX_DIAGNOSTIC_BYTES", 400):
                code, report, detail = self.run_pair(*pair)
            self.assertEqual(code, expected)
            self.assertEqual(report["equivalent"], expected == 0)
            self.assertLessEqual((self.directory / "diagnostics.json").stat().st_size, 400)
            self.assertEqual(detail["schema"], "spark.windows-shipping-repro-diagnostics/2")
            self.assertEqual(detail["status"], "inspection-unavailable")
            self.assertEqual(detail["error"], "diagnostic output exceeds its byte limit")

    def test_sampled_byte_runs_map_across_codeview_record_boundaries_with_record_cap(self) -> None:
        first = struct.pack("<I", 4) + b"".join(type_record(0x1605, bytes([index]) * 4) for index in range(20))
        second = struct.pack("<I", 4) + b"".join(type_record(0x1605, bytes([index + 1]) * 4) for index in range(20))
        with mock.patch.object(helper, "MAX_BYTE_SAMPLES", 1), mock.patch.object(helper, "MAX_SAMPLE_BYTES", 128):
            details = helper._region_details(".debug$T (#0)", first, second)
        # A byte-run sample crosses a record boundary only if the surrounding
        # record-header bytes differ too; explicitly sample the two headers.
        samples = [{"offset": 4, "firstHex": first[4:20].hex(), "secondHex": second[4:20].hex()}]
        with mock.patch.object(helper, "MAX_BYTE_SAMPLES", 1):
            records = helper._codeview_records(first, ".debug$T", samples)
        self.assertEqual(records["recordCount"], 20)
        self.assertEqual(len(records["sampledChangeRecords"]), 1)
        self.assertEqual(records["omittedSampledChangeRecords"], 1)
        self.assertEqual(details["byteDifferences"]["omittedDifferingBytes"], 76)

    def test_missing_bytes_are_counted_and_region_layout_changes_never_decode_unrelated_bytes(self) -> None:
        delta = helper._byte_differences(b"abcd", b"a")
        self.assertEqual(delta["differingByteCount"], 3)
        self.assertEqual(delta["samples"], [{"offset": 1, "firstHex": "626364", "secondHex": ""}])
        details = helper._diagnose_member((bytes(60), coff([(b".debug$T", b"first")])),
                                         (bytes(60), coff([(b".drectve", b"other")])), b"root-a", b"root-b")
        self.assertEqual(details["differingRegions"][1]["details"]["status"], "inspection-unavailable")
        self.assertEqual(details["differingRegions"][1]["details"]["error"], "region layout differs")


if __name__ == "__main__":
    unittest.main()
