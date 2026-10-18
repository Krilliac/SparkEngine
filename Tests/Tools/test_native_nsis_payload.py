"""Contract tests for static NSIS payload inspection; no installer is executed."""
import hashlib
import importlib.util
import io
import lzma
from contextlib import ExitStack
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import zlib


SPEC = importlib.util.spec_from_file_location(
    "native_nsis_payload", Path(__file__).resolve().parents[2] / "tools/native_nsis_payload.py")
nsis = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(nsis)


def packed(data, compressed=True):
    if compressed:
        data = b"\x5d\0\0\x80\0" + lzma.compress(data, format=lzma.FORMAT_RAW, filters=[
            {"id": lzma.FILTER_LZMA1, "dict_size": 8 << 20, "lc": 3, "lp": 0, "pb": 2}])
    return struct.pack("<I", len(data) | (0x80000000 if compressed else 0)) + data


def script_header():
    """Minimal full-ABI fixture derived from the documented NSIS structs."""
    strings = bytearray(b"\0\0")
    def string(value):
        offset = len(strings) // 2
        strings.extend(value.encode("utf-16le") + b"\0\0")
        return offset
    entries = [(0,) * 7] * 8
    sections = []
    for i, name in enumerate(("", "Runtime Binaries", "Sample Content", "Developer Tools")):
        start = len(entries)
        entries.extend([(11, string(nsis.variable(21)), 1, 0, 0, 0, 0),
                        (25, 31, string(nsis.variable(22)), 0, 0, 0, 0),
                        (11, string(nsis.variable(31) + "\\bin"), 1, 0, 0, 0, 0),
                        (20, 0, string(f"file{i}.dll"), 4*i, 0, 0, 0),
                        (11, string(nsis.variable(31)), 1, 0, 0, 0, 0), (1, 0, 0, 0, 0, 0, 0)])
        sections.append((string(name), -1, 17 if i < 2 else 1, start, 5, 0))
    for _ in range(3):
        sections.append((0, -1, 1, len(entries), 1, 0))
        entries.extend([(2, 0, 0, 0, 0, 0, 0), (1, 0, 0, 0, 0, 0, 0)])
    entries.extend([(20, 0, string(nsis.variable(26) + "\\System.dll"), 16, 0, 0, 0),
                    (1, 0, 0, 0, 0, 0, 0)])
    pages, section_base = 300, 364
    entry_base = section_base + 7 * 2072
    string_base = entry_base + len(entries) * 28
    language_base = string_base + len(strings)
    colors = language_base + 10
    raw = bytearray(colors)
    struct.pack_into("<17I", raw, 0, 160, pages, 1, section_base, 7, entry_base, len(entries),
                     string_base, 0, language_base, 1, colors, 0, 0, 0, 0, 0)
    struct.pack_into("<I", raw, 100, 10)
    struct.pack_into("<10i", raw, 108, *([-1]*10))
    struct.pack_into("<3i", raw, pages + 8, -1, -1, -1)
    for i, row in enumerate(sections):
        struct.pack_into("<6i", raw, section_base + i * 2072, *row)
    for i, row in enumerate(entries):
        struct.pack_into("<7i", raw, entry_base + i * 28, *row)
    raw[string_base:language_base] = strings
    return raw, entry_base, section_base


def evidence_fixture():
    header = nsis.Header(script_header()[0])
    files, support = nsis.payload_plan(header)
    files[1]["path"] = "THIRD_PARTY_NOTICES.txt"
    empty = hashlib.sha256(b"").hexdigest()
    blocks = [{"offset": i*4, "storedSize": 0, "compressed": False, "size": 0, "sha256": empty}
              for i in range(5)]
    blocks.extend([{"offset": 20, "storedSize": 4, "compressed": False, "size": 4, "sha256": "a"*64},
                   {"offset": 28, "storedSize": 50, "compressed": False, "size": 50, "sha256": "b"*64}])
    nsis.bind_blocks(files, support, blocks, (20, 28))
    nested_blocks = [dict(blocks[0])]
    nested_support = [{"path": "$PLUGINSDIR/System.dll", "block": 0, "instructions": [8]}]
    nsis.bind_blocks([], nested_support, nested_blocks)
    uninstaller = {"path": "Uninstall.exe", "instruction": 32, "iconBlock": 20, "bodyBlock": 28,
                   "size": 50000, "sha256": "c"*64, "headerSha256": "d"*64,
                   "blocks": nested_blocks, "support": nested_support}
    return {"method": nsis.METHOD, "scope": "all-embedded-file-instructions", "root": "$INSTDIR",
            "headerSha256": "e"*64, "sections": header.sections, "blocks": blocks, "files": files,
            "support": support, "uninstaller": uninstaller, "certificate": None,
            "manifestSha256": nsis.manifest_digest(files)}


class NativeNsisTests(unittest.TestCase):
    def test_private_copy_remains_bound_to_original_through_inspection(self):
        # These disposable bytes are deliberately not an executable. Format
        # callbacks isolate the first-party copy/inspection lifetime invariant.
        for phase in ("unchanged", "inspection", "reconciliation"):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory() as directory, ExitStack() as stack:
                artifact = Path(directory) / "fixture.exe"
                raw = bytearray(2048)
                struct.pack_into("<I", raw, 1048, 1024)
                artifact.write_bytes(raw)
                evidence = evidence_fixture()
                header = SimpleNamespace(raw=b"header", sections=evidence["sections"])
                def change_private(path):
                    with path.open("r+b") as stream:
                        stream.seek(2047)
                        stream.write(b"x")
                def extract(stream, _base, _end, workspace, _prefix, _budget):
                    paths = {}
                    for i, row in enumerate(evidence["blocks"]):
                        target = workspace / f"payload-{i}"
                        target.write_bytes(bytes(row["size"]))
                        paths[row["offset"]] = target
                    if phase == "inspection":
                        change_private(Path(stream.name))
                    return evidence["blocks"], paths
                for name, result in (("pe_envelope", (1024, (0, 1024), 0, 0)),
                                     ("archive_header", (header, 1056)),
                                     ("payload_plan", (evidence["files"], evidence["support"])),
                                     ("uninstaller_payload", evidence["uninstaller"])):
                    stack.enter_context(mock.patch.object(nsis, name, return_value=result))
                stack.enter_context(mock.patch.object(nsis, "extract_blocks", side_effect=extract))
                reached_yield = False
                def inspect():
                    nonlocal reached_yield
                    with nsis.extracted_payload(artifact) as (_, notice, digest, _):
                        reached_yield = True
                        self.assertEqual(digest, hashlib.sha256(raw).hexdigest())
                        if phase == "reconciliation":
                            change_private(notice.parent / "package")
                if phase == "unchanged":
                    inspect()
                    self.assertTrue(reached_yield)
                else:
                    with self.assertRaises(nsis.NativeNsisError):
                        inspect()
                    self.assertEqual(reached_yield, phase == "reconciliation")
                self.assertEqual(artifact.read_bytes(), raw)

    def test_support_instruction_cannot_be_uninstaller_or_component_code(self):
        for instruction in (32, 8, 9, 12, 13):
            evidence = evidence_fixture()
            evidence["support"][0]["instructions"] = [instruction]
            with self.subTest(instruction=instruction), self.assertRaises(nsis.NativeNsisError):
                nsis.validate_evidence(evidence, 4)

    def test_persisted_evidence_and_authoritative_profile(self):
        evidence = evidence_fixture()
        nsis.validate_evidence(evidence, 4)
        policy = {"windowsRuntime": {"components": ["samples", "tools", "redist", "runtime"]}}
        assignments = nsis.prove_profile(evidence, policy)
        self.assertEqual(assignments["THIRD_PARTY_NOTICES.txt"], "runtime")
        policy["windowsRuntime"]["components"].append("sdk")
        with self.assertRaises(nsis.NativeNsisError):
            nsis.prove_profile(evidence, policy)
        for malformed in (None, {}, {"windowsRuntime": []}, {"windowsRuntime": {"components": [[], "runtime"]}},
                          {"windowsRuntime": {"components": ["samples", "tools", "redist", "runtime", "runtime"]}}):
            with self.subTest(policy=malformed), self.assertRaises(nsis.NativeNsisError):
                nsis.prove_profile(evidence, malformed)

    def test_malformed_persisted_evidence_fails_closed(self):
        mutations = [lambda e: e.pop("uninstaller"),
                     lambda e: e.update(method="7zip-listing"),
                     lambda e: e.update(manifestSha256="0"*64),
                     lambda e: e["sections"][0].update(component="runtime"),
                     lambda e: e["sections"][0].update(flags=True),
                     lambda e: e["sections"][1].update(start=8),
                     lambda e: e["blocks"][0].update(storedSize=1),
                     lambda e: e["blocks"][0].update(sha256="not-a-digest"),
                     lambda e: e["uninstaller"].update(bodyBlock=20),
                     lambda e: e["uninstaller"].update(sha256=123),
                     lambda e: e["uninstaller"]["support"][0].update(sha256="f"*64),
                     lambda e: e["support"][0].update(instructions=[38, 38]),
                     lambda e: e["support"][0].update(path="$PLUGINSDIR/hidden.exe"),
                     lambda e: e.update(certificate={"offset": 1024, "size": 16, "sha256": "a"*64,
                                                     "authentication": "verified"})]
        for mutate in mutations:
            evidence = evidence_fixture()
            mutate(evidence)
            with self.subTest(mutation=mutate), self.assertRaises(nsis.NativeNsisError):
                nsis.validate_evidence(evidence, 4)
        for changes in ({"instruction": 8}, {"section": 7}, {"sha256": "f"*64},
                        {"path": "bin/file0.dll"}, {"block": 1}, {"path": "../outside"}):
            evidence = evidence_fixture()
            evidence["files"][1].update(changes)
            evidence["manifestSha256"] = nsis.manifest_digest(evidence["files"])
            with self.subTest(changes=changes), self.assertRaises(nsis.NativeNsisError):
                nsis.validate_evidence(evidence, 4)

    def test_all_components_and_support_are_inventoried(self):
        raw, _, _ = script_header()
        header = nsis.Header(raw)
        files, support = nsis.payload_plan(header)
        self.assertEqual([row["path"] for row in files], [f"bin/file{i}.dll" for i in range(4)])
        self.assertEqual([row["component"] for row in header.sections[:4]], ["redist", "runtime", "samples", "tools"])
        self.assertEqual(support[0]["path"], "$PLUGINSDIR/System.dll")

    def test_unknown_and_nonlinear_payload_instructions_fail_closed(self):
        for opcode in (70, 66, 5, 25):
            raw, entries, _ = script_header()
            struct.pack_into("<I", raw, entries + 10 * 28, opcode)
            with self.subTest(opcode=opcode), self.assertRaises(nsis.NativeNsisError):
                nsis.payload_plan(nsis.Header(raw))

    def test_entry_into_middle_of_section_is_rejected(self):
        # MessageBox's SECOND return target is offsets[5], despite the older
        # shorthand comment in fileform.h. exec.c is the semantic authority.
        for branch in ((5, 12, 0, 0, 0, 0, 0), (22, 0, 0, 6, 0, 7, 12)):
            raw, entries, _ = script_header()
            struct.pack_into("<7i", raw, entries + 32 * 28, *branch)
            with self.assertRaisesRegex(nsis.NativeNsisError, "control transfer"):
                nsis.payload_plan(nsis.Header(raw))

    def test_section_overlap_and_header_block_overlap_are_rejected(self):
        raw, _, sections = script_header()
        struct.pack_into("<I", raw, sections + 2072 + 12, 8)
        with self.assertRaises(nsis.NativeNsisError):
            nsis.Header(raw)
        raw, _, _ = script_header()
        struct.pack_into("<I", raw, 28, 300)
        with self.assertRaises(nsis.NativeNsisError):
            nsis.Header(raw)

    def test_output_alias_prologue_and_outside_relative_file_are_rejected(self):
        raw, entries, _ = script_header()
        struct.pack_into("<I", raw, entries + 9 * 28 + 4, 30)
        with self.assertRaisesRegex(nsis.NativeNsisError, "prologue"):
            nsis.payload_plan(nsis.Header(raw))
        raw, entries, _ = script_header()
        filename = struct.unpack_from("<I", raw, entries + 11 * 28 + 8)[0]
        struct.pack_into("<I", raw, entries + 38 * 28 + 8, filename)
        with self.assertRaisesRegex(nsis.NativeNsisError, "unclassified"):
            nsis.payload_plan(nsis.Header(raw))

    def test_block_classification_rejects_missing_hidden_and_cross_role_aliases(self):
        blocks = [{"offset": 0, "size": 1, "sha256": "a"*64}, {"offset": 5, "size": 2, "sha256": "b"*64}]
        cases = [([{"block": 0}], []), ([{"block": 1}], [{"block": 5}]),
                 ([{"block": 0}], [{"block": 0}, {"block": 5}])]
        for app, support in cases:
            with self.subTest(app=app), self.assertRaises(nsis.NativeNsisError):
                nsis.bind_blocks(app, support, blocks)
        app, support = [{"block": 0}], [{"block": 5}]
        nsis.bind_blocks(app, support, blocks)
        self.assertEqual(app[0]["sha256"], "a"*64)
        self.assertEqual(support[0]["size"], 2)

    def test_destination_case_and_file_directory_aliases_rejected(self):
        for paths in (("bin/a", "BIN/A"), ("a", "a/b")):
            with self.assertRaises(nsis.NativeNsisError):
                nsis.no_collisions(paths)

    def test_compressed_and_uncompressed_block_identity(self):
        for compressed in (True, False):
            raw = b"payload\0" * 200
            stream = io.BytesIO(packed(raw, compressed))
            target = io.BytesIO()
            record = nsis.read_block(stream, 0, len(stream.getvalue()), target, 4096)
            self.assertEqual(target.getvalue(), raw)
            self.assertEqual(record["sha256"], hashlib.sha256(raw).hexdigest())
            self.assertEqual(record["size"], len(raw))

    def test_block_bounds_compression_tail_and_caps(self):
        good = packed(b"a" * 10000)
        cases = [good[:-1], struct.pack("<I", 0x80000005) + b"\x5d\0\0\x80\0",
                 struct.pack("<I", len(good) - 3 | 0x80000000) + good[4:] + b"x",
                 struct.pack("<I", 0x80000005) + b"\x5d\0\0\0\x40"]
        for data in cases:
            with self.subTest(data=data[:8]), self.assertRaises(nsis.NativeNsisError):
                nsis.read_block(io.BytesIO(data), 0, len(data), io.BytesIO(), 20000)
        with self.assertRaises(nsis.NativeNsisError):
            nsis.read_block(io.BytesIO(good), 0, len(good), io.BytesIO(), 9999)

    def test_archive_crc_header_length_and_flags_are_enforced(self):
        raw = script_header()[0]
        block = packed(raw)
        body = struct.pack("<I", 1) + nsis.SIGNATURE + struct.pack("<II", len(raw), len(block)+32) + block
        archive = body + struct.pack("<I", zlib.crc32(body))
        header, offset = nsis.archive_header(io.BytesIO(archive), 0, len(archive), uninstall=True)
        self.assertEqual(header.raw, raw)
        self.assertEqual(offset, len(archive)-4)
        for position in (0, 20, 24, len(archive)-1):
            corrupt = bytearray(archive)
            corrupt[position] ^= 1
            with self.subTest(position=position), self.assertRaises(nsis.NativeNsisError):
                nsis.archive_header(io.BytesIO(corrupt), 0, len(corrupt), uninstall=True)

    def test_paths_reject_windows_aliases_and_traversal(self):
        self.assertEqual(nsis.safe_path("bin/SparkEngine.exe"), "bin/sparkengine.exe")
        for value in ("../x", "x/../y", "x//y", "C:/x", "x:stream", "x\\y", "x.", "x ",
                      "AUX.txt", "com1", "CONIN$", "x~1", "/root", "x/<file>", "x/\u00e9"):
            with self.subTest(value=value), self.assertRaises(nsis.NativeNsisError):
                nsis.safe_path(value)

    def test_unsigned_and_signed_exact_boundary(self):
        raw = b"x" * 29
        self.assertIsNone(nsis.certificate_boundary(io.BytesIO(raw), 29, 29, 0, 0))
        certificate = struct.pack("<IHH", 11, 0x200, 2) + b"abc" + b"\0" * 5
        signed = raw + b"\0" * 3 + certificate
        result = nsis.certificate_boundary(io.BytesIO(signed), len(signed), 29, 32, 16)
        self.assertEqual(result["sha256"], hashlib.sha256(certificate).hexdigest())
        self.assertEqual(result["authentication"], "not-verified")

    def test_signed_overlap_truncation_trailing_and_padding(self):
        cert = struct.pack("<IHH", 11, 0x200, 2) + b"abc" + b"\0" * 5
        good = b"x" * 29 + b"\0" * 3 + cert
        cases = [(good, 24, 24), (good[:-1], 32, 16), (good+b"z", 32, 16),
                 (good, 31, 17), (good[:29]+b"a\0\0"+cert, 32, 16),
                 (good[:-1]+b"z", 32, 16), (good[:32]+struct.pack("<IHH", 7, 0x200, 2)+cert[8:], 32, 16),
                 (good[:32]+struct.pack("<IHH", 11, 0x100, 2)+cert[8:], 32, 16)]
        for raw, offset, size in cases:
            with self.subTest(offset=offset, size=size), self.assertRaises(nsis.NativeNsisError):
                nsis.certificate_boundary(io.BytesIO(raw), len(raw), 29, offset, size)
        with self.assertRaises(nsis.NativeNsisError):
            nsis.certificate_boundary(io.BytesIO(good), len(good), 29, 0, 0)

    def test_first_header_cannot_be_shadowed_inside_resources(self):
        raw = bytearray(2048)
        nsis.reject_early_header(raw)
        raw[512:532] = struct.pack("<I", 0) + nsis.SIGNATURE
        with self.assertRaisesRegex(nsis.NativeNsisError, "before PE overlay"):
            nsis.reject_early_header(raw)
        struct.pack_into("<I", raw, 512, 16)
        nsis.reject_early_header(raw)  # NSIS ignores flags outside its mask.

    def test_icon_patches_cannot_touch_code_or_overlap(self):
        stub = bytes(64)
        patches = struct.pack("<II", 3, 35)+b"abc"+bytes(4)
        self.assertEqual(nsis.patch_icons(stub, patches, (32, 64))[35:38], b"abc")
        for patch in (struct.pack("<II", 3, 31)+b"abc"+bytes(4),
                      struct.pack("<II", 3, 63)+b"abc"+bytes(4),
                      patches[:-4]+struct.pack("<II", 2, 36)+b"xy"+bytes(4),
                      patches[:-1], patches+b"x", struct.pack("<II", 256, 32)+bytes(260)):
            with self.subTest(patch=patch), self.assertRaises(nsis.NativeNsisError):
                nsis.patch_icons(stub, patch, (32, 64))


if __name__ == "__main__":
    unittest.main()
