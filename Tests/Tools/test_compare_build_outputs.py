#!/usr/bin/env python3
"""BLD-100: tools/compare_build_outputs.py finds every non-reproducible output.

The tests run the tool on real and crafted fixtures:

* gcc ELF executables built from one source in two directories differ only in
  their DWARF when the build directory leaks (the comp_dir/.debug_line_str
  leak the root -ffile-prefix-map options remove); mapped, they are equivalent;
* an executable whose code differs is reported at its first differing content
  section, with the GNU build-id change, not at the derived build-id note;
* ar archives differing only in a member timestamp are reported at that member;
  long member names resolve through both the GNU ("/\\n") and the COFF (NUL,
  MSVC lib.exe) "//" name table;
* PE images differing only in the COFF timestamp are reported at that header
  timestamp with a changed timestamp identity when not linked with /Brepro,
  and at the content section when /Brepro derives the timestamp; CodeView RSDS
  GUID/age/PDB name and the REPRO entry are recorded;
* the manifest is closed, sorted, relative and host independent: equivalent
  trees in different directories give byte-identical manifests;
* missing, extra, empty and malformed inputs fail with the documented codes.
"""

from __future__ import annotations

import importlib.util
import io
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import unittest.mock
import uuid
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL = REPO_ROOT / "tools" / "compare_build_outputs.py"

GCC = shutil.which("gcc")
READELF = shutil.which("readelf")
ELF_TOOLS = GCC and READELF and sys.platform.startswith("linux")

SOURCE = "#include <stdio.h>\nint main(void) { puts(MESSAGE); return 0; }\n"


def _load_tool():
    spec = importlib.util.spec_from_file_location("compare_build_outputs", TOOL)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


tool = _load_tool()


def _run_tool(*arguments: str | Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(TOOL), *[str(argument) for argument in arguments]],
        capture_output=True, text=True, check=False, timeout=120,
    )


def _compile(directory: Path, message: str, *flags: str) -> Path:
    """Compile SOURCE inside directory (cwd there, so comp_dir is that directory)."""
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "main.c").write_text(SOURCE, encoding="utf-8")
    output = directory / "out" / "app"
    output.parent.mkdir(exist_ok=True)
    subprocess.run(
        [GCC, "-O2", "-g", f"-DMESSAGE=\"{message}\"", "-Wl,--build-id=sha1", *flags, "main.c", "-o", str(output)],
        cwd=directory, check=True, capture_output=True, timeout=120,
    )
    return output.parent


def _ar_archive(member_data: bytes, mtime: int) -> bytes:
    header = f"{'obj.o/':<16}{mtime:<12}{0:<6}{0:<6}{'644':<8}{len(member_data):<10}`\n".encode("ascii")
    assert len(header) == 60
    return b"!<arch>\n" + header + member_data + (b"\n" if len(member_data) % 2 else b"")


def _named_ar_member(name: str, member_data: bytes) -> bytes:
    header = f"{name:<16}{0:<12}{0:<6}{0:<6}{'644':<8}{len(member_data):<10}`\n".encode("ascii")
    return header + member_data + (b"\n" if len(member_data) % 2 else b"")


def _coff_member(debug_data: bytes, offset: int = 60) -> bytes:
    header = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, 0, 0)
    section = b".debug$S" + struct.pack("<IIIIIIHHI", len(debug_data), 0, len(debug_data), offset, 0, 0, 0, 0, 0)
    return header + section + bytes(offset - len(header) - len(section)) + debug_data


def _coff_sections(sections: list[tuple[bytes, bytes]]) -> bytes:
    """A COFF object whose named sections hold the given raw bytes, in order."""
    header = struct.pack("<HHIIIHH", 0x8664, len(sections), 0, 0, 0, 0, 0)
    offset = len(header) + 40 * len(sections)
    table = b""
    for name, data in sections:
        table += name.ljust(8, b"\0") + struct.pack("<IIIIIIHHI", len(data), 0, len(data), offset, 0, 0, 0, 0, 0)
        offset += len(data)
    return header + table + b"".join(data for _, data in sections)


def _long_name_archive(names: list[str], terminator: bytes) -> bytes:
    """An archive whose members all use the "//" long-name table.

    GNU ar ends each table entry with "/\\n"; MSVC lib.exe and llvm-lib end it
    with a NUL (PE/COFF archive format).
    """

    def member(name: str, data: bytes) -> bytes:
        header = f"{name:<16}{0:<12}{0:<6}{0:<6}{'644':<8}{len(data):<10}`\n".encode("ascii")
        return header + data + (b"\n" if len(data) % 2 else b"")

    table, members = b"", []
    for index, name in enumerate(names):
        members.append(member(f"/{len(table)}", bytes([index + 1]) * 3))
        table += name.encode("ascii") + terminator
    return b"!<arch>\n" + member("//", table) + b"".join(members)


def _pe_image(timestamp: int, text: bytes, guid: uuid.UUID, pdb: str, repro: bool = False) -> bytes:
    """A minimal PE32+ image with one .text section, a CodeView RSDS record and, for /Brepro, a REPRO entry."""
    file_alignment = 0x200
    section_rva = 0x1000
    code_view = b"RSDS" + guid.bytes_le + struct.pack("<I", 1) + pdb.encode("ascii") + b"\x00"
    debug_entry_offset = len(text)
    entry_count = 2 if repro else 1
    code_view_offset = debug_entry_offset + 28 * entry_count
    raw = bytearray(text + bytes(28 * entry_count) + code_view)
    raw += bytes(-len(raw) % file_alignment)
    struct.pack_into(
        "<IIHHIIII", raw, debug_entry_offset,
        0, timestamp, 0, 0, 2, len(code_view), section_rva + code_view_offset, file_alignment + code_view_offset,
    )
    if repro:
        struct.pack_into("<IIHHIIII", raw, debug_entry_offset + 28, 0, timestamp, 0, 0, 16, 0, 0, 0)
    pe_offset = 0x40
    optional_size = 112 + 16 * 8
    dos = bytearray(0x40)
    dos[:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, pe_offset)
    coff = b"PE\x00\x00" + struct.pack("<HHIIIHH", 0x8664, 1, timestamp, 0, 0, optional_size, 0x0022)
    optional = bytearray(optional_size)
    struct.pack_into("<H", optional, 0, 0x20B)
    struct.pack_into("<I", optional, 108, 16)
    struct.pack_into("<II", optional, 112 + 6 * 8, section_rva + debug_entry_offset, 28 * entry_count)
    section = b".text\x00\x00\x00" + struct.pack(
        "<IIIIIIHHI", len(raw), section_rva, len(raw), file_alignment, 0, 0, 0, 0, 0x60000020
    )
    headers = bytes(dos) + coff + bytes(optional) + section
    return headers + bytes(file_alignment - len(headers)) + bytes(raw)


class CoffNormalizationTests(unittest.TestCase):
    """OD-24 parser regressions use in-memory archives, without filesystem setup."""

    def test_bigobj_uses_the_published_class_id_and_preserves_layout(self) -> None:
        root = b"C:\\build\\alpha"
        header = bytearray(56)
        struct.pack_into("<HHHH", header, 0, 0, 0xFFFF, 2, 0x8664)
        header[12:28] = uuid.UUID("D1BAA1C7-BAEE-4BA9-AF20-FAF66AA4DCB8").bytes_le
        struct.pack_into("<I", header, 44, 1)
        section = _coff_member(root, offset=96)[20:60]
        original = bytes(header) + section + root
        normalized, changed = tool._replace_build_root(original, root)
        self.assertTrue(changed)
        self.assertEqual(normalized[:96], original[:96])
        self.assertEqual(len(normalized), len(original))
        header[12] ^= 1
        self.assertFalse(tool._replace_build_root(bytes(header) + section + root, root)[1])

    def test_coff_section_count_is_independent_of_pe_limit(self) -> None:
        root = b"C:\\build\\alpha"
        count = 100
        member = bytearray(_coff_member(root, offset=20 + count * 40))
        struct.pack_into("<H", member, 2, count)
        self.assertTrue(tool._replace_build_root(bytes(member), root)[1])
        self.assertEqual(tool.MAX_PE_SECTIONS, 96)

    def test_relocations_symbols_and_headers_cannot_alias_debug_bytes(self) -> None:
        root = b"C:\\build\\alpha"
        member = bytearray(_coff_member(root))
        reloc_offset = len(member)
        member.extend(bytes(10))
        symbol_offset = len(member)
        member.extend(bytes(18) + struct.pack("<I", 4))
        struct.pack_into("<II", member, 8, symbol_offset, 1)
        struct.pack_into("<I", member, 44, reloc_offset)
        struct.pack_into("<H", member, 52, 1)
        normalized, changed = tool._replace_build_root(bytes(member), root)
        self.assertTrue(changed)
        self.assertEqual(normalized[reloc_offset:], member[reloc_offset:])
        for field in (8, 40, 44):  # symbol table, raw section pointer, relocation pointer
            invalid = bytearray(member)
            struct.pack_into("<I", invalid, field, 20 if field == 40 else 60)
            self.assertFalse(tool._replace_build_root(bytes(invalid), root)[1])

    def test_layout_code_and_archive_headers_remain_exact(self) -> None:
        first_root, second_root = b"C:\\build\\alpha", b"C:\\build\\bravo"
        first = _coff_member(first_root)
        second = _coff_member(second_root, offset=64)
        a = tool._replace_build_root(first, first_root)[0]
        b = tool._replace_build_root(second, second_root)[0]
        self.assertNotEqual(a, b)
        for tail in (b"code A", b"code B"):
            changed, normalized = tool._replace_build_root(first + tail, first_root)
            self.assertTrue(normalized)
            self.assertTrue(changed.endswith(tail))
        archives = [_ar_archive(first, stamp) for stamp in (0, 1)]
        digests = [tool._ar_members(io.BytesIO(data), len(data), first_root)[1] for data in archives]
        self.assertNotEqual(*digests)

    def test_non_coff_member_never_normalizes(self) -> None:
        root = b"C:\\build\\alpha"
        member = bytearray(_coff_member(root))
        struct.pack_into("<H", member, 0, 0x1234)
        normalized, changed = tool._replace_build_root(bytes(member), root)
        self.assertFalse(changed)
        self.assertEqual(normalized, member)

    def test_pe_and_elf_manifests_accept_empty_normalization_only(self) -> None:
        image = _pe_image(0, b"\xc3" * 16, uuid.UUID(int=1), "app.pdb")
        image_path = type("ImagePath", (), {"open": lambda self, mode: io.BytesIO(image)})()
        fields = tool.describe(image_path)
        entry = {"path": "app.exe", "sha256": hashlib.sha256(image).hexdigest(), **fields}
        for kind, identity in (("pe", fields["identity"]), ("elf", {"machine": "x86-64", "buildId": "ab"})):
            entry = dict(entry, kind=kind, identity=identity)
            manifest = {"schema": tool.SCHEMA, "entries": [entry]}
            tool.validate_manifest(manifest)
            with self.assertRaises(tool.InputError):
                tool.validate_manifest({"schema": tool.SCHEMA, "entries": [dict(
                    entry, normalizedMembers=[{"member": "x", "index": 0, "rootLength": 3}])]})

    def test_lib_debug_section_root_is_normalized_and_recorded(self) -> None:
        root = b"C:\\build\\alpha"
        member = _coff_member(b"prefix " + root + b" suffix")
        archive = _ar_archive(member, 0)
        members, normalized_digest, recorded = tool._ar_members(io.BytesIO(archive), len(archive), root)
        self.assertEqual(recorded, [{"member": "obj.o", "index": 0, "rootLength": len(root)}])
        self.assertNotEqual(normalized_digest, hashlib.sha256(archive).hexdigest())
        self.assertEqual(members[0]["size"], len(member))
        other_root = b"C:\\build\\bravo"
        other = _ar_archive(_coff_member(b"prefix " + other_root + b" suffix"), 0)
        other_members, other_digest, other_recorded = tool._ar_members(io.BytesIO(other), len(other), other_root)
        self.assertEqual((members, normalized_digest, recorded), (other_members, other_digest, other_recorded))

    def test_cli_rejects_different_length_roots_before_reading_trees(self) -> None:
        with unittest.mock.patch("sys.stderr", new_callable=io.StringIO) as errors:
            code = tool.main(["trees", "absent-a", "absent-b", "--build-root-a", "C:\\a",
                              "--build-root-b", "C:\\bb"])
        self.assertEqual(code, 2)
        self.assertIn("same byte length", errors.getvalue())

    def test_root_outside_debug_section_is_not_normalized(self) -> None:
        root = b"C:\\build\\alpha"
        member = _coff_member(b"debug-data") + root
        archive = _ar_archive(member, 0)
        _, normalized_digest, recorded = tool._ar_members(io.BytesIO(archive), len(archive), root)
        self.assertEqual(recorded, [])
        self.assertEqual(normalized_digest, hashlib.sha256(archive).hexdigest())

    def test_root_in_other_sections_of_the_same_member_stays_exact(self) -> None:
        # MSVC /Z7 also writes the build directory into .debug$T (LF_BUILDINFO
        # strings); OD-24 does not cover it, nor any code or data section.
        root = b"C:\\build\\alpha"
        member = _coff_sections([(b".text", b"code " + root), (b".debug$T", b"cwd " + root),
                                 (b".debug$S", b"obj " + root), (b".rdata", root)])
        normalized, changed = tool._replace_build_root(member, root)
        self.assertTrue(changed)
        self.assertEqual(normalized.count(root), 3)
        self.assertEqual(len(normalized), len(member))
        debug_s = member.index(b"obj " + root) + 4
        self.assertNotEqual(normalized[debug_s:debug_s + len(root)], root)
        self.assertEqual(normalized[:debug_s], member[:debug_s])
        self.assertEqual(normalized[debug_s + len(root):], member[debug_s + len(root):])

    def test_non_lib_coff_archive_is_not_normalized(self) -> None:
        root = b"C:\\build\\alpha"
        archive = _ar_archive(_coff_member(root), 0)

        class InMemoryPath:
            suffix = ".a"

            def open(self, mode: str) -> io.BytesIO:
                if mode != "rb":
                    raise AssertionError(mode)
                return io.BytesIO(archive)

        fields = tool.describe(InMemoryPath(), root)
        self.assertIsNotNone(fields)
        self.assertEqual(fields["normalizedMembers"], [])
        self.assertEqual(fields["normalizedDigest"], hashlib.sha256(archive).hexdigest())

    def test_malformed_debug_section_layout_is_not_normalized(self) -> None:
        root = b"C:\\build\\alpha"
        member = bytearray(_coff_member(root))
        struct.pack_into("<I", member, 20 + 20, len(member) + 1)
        archive = _ar_archive(bytes(member), 0)
        _, normalized_digest, recorded = tool._ar_members(io.BytesIO(archive), len(archive), root)
        self.assertEqual(recorded, [])
        self.assertEqual(normalized_digest, hashlib.sha256(archive).hexdigest())

    def test_standalone_obj_is_not_an_archive_normalization_target(self) -> None:
        root = b"C:\\build\\alpha"
        obj_path = type(
            "ObjPath",
            (),
            {"suffix": ".obj", "open": lambda self, mode: io.BytesIO(_coff_member(root))},
        )()
        fields = tool.describe(obj_path, root)
        self.assertEqual(fields["kind"], "coff")
        self.assertEqual(fields["normalizedMembers"], [])
        self.assertEqual(fields["sections"][0]["sha256"], hashlib.sha256(_coff_member(root)).hexdigest())

    def test_archive_padding_difference_remains_visible(self) -> None:
        root = b"C:\\build\\alpha"
        member = _coff_member(root) + b"x"
        first = _named_ar_member("obj.o/", member)
        second = first[:-1] + b"z"
        first_archive = b"!<arch>\n" + first
        second_archive = b"!<arch>\n" + second
        first_digest = tool._ar_members(io.BytesIO(first_archive), len(first_archive), root)[1]
        second_digest = tool._ar_members(io.BytesIO(second_archive), len(second_archive), root)[1]
        self.assertNotEqual(first_digest, second_digest)

    def test_special_archive_members_are_not_parsed_as_coff(self) -> None:
        root = b"C:\\build\\alpha"
        member = _named_ar_member("/", _coff_member(root))
        archive = b"!<arch>\n" + member
        _, _, recorded = tool._ar_members(io.BytesIO(archive), len(archive), root)
        self.assertEqual(recorded, [])

    def test_different_normalized_root_lengths_are_rejected(self) -> None:
        short = {"schema": tool.SCHEMA, "entries": [{"path": "x.lib", "kind": "ar", "size": 1,
            "sha256": "0" * 64, "identity": {}, "sections": [],
            "normalizedMembers": [{"member": "obj.o", "index": 0, "rootLength": 3}]}]}
        long = json.loads(json.dumps(short))
        long["entries"][0]["normalizedMembers"][0]["rootLength"] = 4
        with self.assertRaisesRegex(tool.InputError, "different byte lengths"):
            tool.compare_manifests(short, long)


class ManifestSchemaTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = Path(tempfile.mkdtemp(prefix="spark-compare-outputs-"))

    def tearDown(self) -> None:
        shutil.rmtree(self.temp, ignore_errors=True)

    def _tree(self, name: str, files: dict[str, bytes]) -> Path:
        root = self.temp / name
        for relative, data in files.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        return root

    def test_od24_manifest_records_normalized_members_and_keeps_other_bytes_exact(self) -> None:
        def library(root: bytes, text: bytes = b"code", debug_t: bytes = b"types") -> bytes:
            return _ar_archive(_coff_sections([(b".text", text), (b".debug$T", debug_t),
                                               (b".debug$S", b"obj " + root + b"\\x.obj")]), 0)

        first_root, second_root = "C:\\work\\a\\build", "C:\\work\\b\\build"
        first = self._tree("a", {"lib/x.lib": library(first_root.encode())})
        second = self._tree("b", {"lib/x.lib": library(second_root.encode())})
        left = tool.build_manifest(first, first_root)
        right = tool.build_manifest(second, second_root)
        tool.validate_manifest(left)
        self.assertEqual(left["entries"][0]["normalizedMembers"],
                         [{"member": "obj.o", "index": 0, "rootLength": len(first_root)}])
        self.assertTrue(tool.compare_manifests(left, right)["equivalent"])
        self.assertFalse(tool.compare_manifests(tool.build_manifest(first), tool.build_manifest(second))["equivalent"],
                         "without --build-root nothing is normalized")

        # Same normalized .debug$S but a different code byte, each tree's build
        # root in .debug$T, or a shifted layout: each still differs.
        variants = {
            "code": (library(first_root.encode()), library(second_root.encode(), text=b"cod3")),
            "types": (library(first_root.encode(), debug_t=first_root.encode()),
                      library(second_root.encode(), debug_t=second_root.encode())),
            "layout": (library(first_root.encode()), library(second_root.encode(), text=b"code!")),
        }
        for label, (data_a, data_b) in variants.items():
            with self.subTest(label=label):
                pair = [tool.build_manifest(self._tree(f"v-{label}-{side}", {"lib/x.lib": data}), root)
                        for side, data, root in (("a", data_a, first_root), ("b", data_b, second_root))]
                self.assertFalse(tool.compare_manifests(*pair)["equivalent"])

        # The same archive under a non-.lib name is compared exactly.
        renamed = [self._tree(f"r-{label}", {"lib/libx.a": library(root.encode())})
                   for label, root in (("a", first_root), ("b", second_root))]
        manifests = [tool.build_manifest(tree, root) for tree, root in zip(renamed, (first_root, second_root))]
        self.assertEqual(manifests[0]["entries"][0]["normalizedMembers"], [])
        self.assertFalse(tool.compare_manifests(*manifests)["equivalent"])

    def test_archive_member_timestamp_is_reported_at_that_member(self) -> None:
        first = self._tree("a", {"lib/libx.a": _ar_archive(b"\x01\x02\x03", 0), "notes.txt": b"x"})
        second = self._tree("b", {"lib/libx.a": _ar_archive(b"\x01\x02\x03", 1700000000), "notes.txt": b"y"})
        left, right = tool.build_manifest(first), tool.build_manifest(second)
        self.assertEqual([entry["path"] for entry in left["entries"]], ["lib/libx.a"], "text files are not eligible")
        report = tool.compare_manifests(left, right)
        self.assertFalse(report["equivalent"])
        self.assertEqual(len(report["differing"]), 1)
        self.assertTrue(report["differing"][0]["firstDifference"].startswith("obj.o "))

    def test_archive_long_names_resolve_in_gnu_and_coff_tables(self) -> None:
        names = ["miniz.dir\\MinSizeRel\\miniz_zip.obj", "miniz.dir\\MinSizeRel\\miniz_tinfl.obj"]
        for terminator in (b"/\n", b"\x00"):
            with self.subTest(terminator=terminator):
                root = self._tree(f"t{len(terminator)}", {"lib/miniz.lib": _long_name_archive(names, terminator)})
                (entry,) = tool.build_manifest(root)["entries"]
                self.assertEqual([section["name"] for section in entry["sections"]], ["<long-names>", *names])
        broken = bytearray(_long_name_archive(names, b"\x00"))
        # Point the second member past the end of the name table.
        second = broken.index(b"/" + str(len(names[0]) + 1).encode("ascii") + b" ")
        broken[second : second + 16] = f"{'/999':<16}".encode("ascii")
        with self.assertRaisesRegex(tool.InputError, "long name points outside the name table"):
            tool.build_manifest(self._tree("bad", {"lib/bad.lib": bytes(broken)}))

    def test_pe_timestamp_and_codeview_identity(self) -> None:
        guid = uuid.UUID("12345678-1234-5678-9abc-def012345678")
        first = self._tree("a", {"bin/app.exe": _pe_image(0x11111111, b"\xc3" * 16, guid, "app.pdb")})
        second = self._tree("b", {"bin/app.exe": _pe_image(0x22222222, b"\xc3" * 16, guid, "app.pdb")})
        left = tool.build_manifest(first)
        identity = left["entries"][0]["identity"]
        self.assertEqual(identity["timestamp"], "11111111")
        self.assertEqual(identity["machine"], "pe-0x8664")
        self.assertEqual(identity["codeView"], {"guid": str(guid).upper(), "age": 1, "pdb": "app.pdb"})
        self.assertFalse(identity["repro"])
        report = tool.compare_manifests(left, tool.build_manifest(second))
        self.assertFalse(report["equivalent"])
        difference = report["differing"][0]
        self.assertEqual(difference["identityChanged"], ["timestamp"])
        # The COFF header holds the timestamp; .text holds the debug directory
        # entry that repeats it.
        self.assertEqual(difference["differingSections"], ["<pe-headers>", ".text"])
        # Without /Brepro the wall-clock link timestamp is the cause, not the
        # section that repeats it in the debug directory.
        self.assertEqual(difference["firstDifference"], "<pe-headers> (COFF timestamp; image not linked with /Brepro)")

    def test_brepro_timestamp_change_names_the_content_section(self) -> None:
        guid = uuid.UUID("12345678-1234-5678-9abc-def012345678")
        first = self._tree("a", {"bin/app.exe": _pe_image(0x11111111, b"\xc3" * 16, guid, "app.pdb", repro=True)})
        second = self._tree("b", {"bin/app.exe": _pe_image(0x22222222, b"\x90" * 16, guid, "app.pdb", repro=True)})
        left = tool.build_manifest(first)
        self.assertTrue(left["entries"][0]["identity"]["repro"])
        difference = tool.compare_manifests(left, tool.build_manifest(second))["differing"][0]
        # /Brepro derives the timestamp from content, so the content section is the cause.
        self.assertEqual(difference["identityChanged"], ["timestamp"])
        self.assertEqual(difference["firstDifference"], ".text (#1, 512 vs 512 bytes)")

    def test_identical_trees_in_different_directories_give_identical_manifests(self) -> None:
        files = {"bin/libx.a": _ar_archive(b"abc", 0), "lib/deep/liby.a": _ar_archive(b"abcd", 0)}
        first = self._tree("short", files)
        second = self._tree("a-much-longer-directory/nested", files)
        output_a, output_b = self.temp / "a.json", self.temp / "b.json"
        self.assertEqual(_run_tool("manifest", first, "--output", output_a).returncode, 0)
        self.assertEqual(_run_tool("manifest", second, "--output", output_b).returncode, 0)
        self.assertEqual(output_a.read_bytes(), output_b.read_bytes())
        manifest = json.loads(output_a.read_text(encoding="utf-8"))
        self.assertEqual(manifest["schema"], "spark.build-output-manifest/2")
        self.assertNotIn(str(self.temp), output_a.read_text(encoding="utf-8"))
        result = _run_tool("compare", output_a, output_b, "--report", self.temp / "report.json")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("EQUIVALENT", result.stdout)

    def test_missing_and_extra_outputs_are_not_equivalent(self) -> None:
        first = self._tree("a", {"x.a": _ar_archive(b"1", 0), "only-a.a": _ar_archive(b"2", 0)})
        second = self._tree("b", {"x.a": _ar_archive(b"1", 0), "only-b.a": _ar_archive(b"3", 0)})
        report_path = self.temp / "report.json"
        result = _run_tool("trees", first, second, "--report", report_path)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(report["onlyInFirst"], ["only-a.a"])
        self.assertEqual(report["onlyInSecond"], ["only-b.a"])
        self.assertEqual(report["differing"], [])
        self.assertFalse(report["equivalent"])

    def test_empty_root_is_an_error_not_a_vacuous_match(self) -> None:
        first = self._tree("a", {"readme.txt": b"no outputs"})
        second = self._tree("b", {"readme.txt": b"no outputs"})
        result = _run_tool("trees", first, second)
        self.assertEqual(result.returncode, 2)
        self.assertIn("no ELF, PE or archive outputs", result.stderr)

    def test_malformed_eligible_file_is_an_error(self) -> None:
        root = self._tree("a", {"good.a": _ar_archive(b"1", 0), "bad.a": b"!<arch>\n" + b"garbage" * 3})
        with self.assertRaisesRegex(tool.InputError, "bad.a"):
            tool.build_manifest(root)
        truncated = self._tree("b", {"bin/app": b"\x7fELF\x02\x01\x01" + bytes(9)})
        self.assertEqual(_run_tool("manifest", truncated, "--output", self.temp / "m.json").returncode, 2)
        self.assertFalse((self.temp / "m.json").exists())

    def test_manifest_schema_is_closed(self) -> None:
        manifest = tool.build_manifest(self._tree("a", {"x.a": _ar_archive(b"1", 0)}))
        tool.validate_manifest(manifest)
        mutations = [
            lambda m: m.update(extra=1),
            lambda m: m.update(schema="spark.build-output-manifest/0"),
            lambda m: m.update(entries=[]),
            lambda m: m["entries"][0].update(host="builder"),
            lambda m: m["entries"][0].update(path="/abs/x.a"),
            lambda m: m["entries"][0].update(sha256="00"),
            lambda m: m["entries"][0].update(size=-1),
            lambda m: m["entries"][0].update(kind="elf"),
            lambda m: m["entries"][0]["sections"].append({"name": "x"}),
            lambda m: m["entries"].append(dict(m["entries"][0])),
        ]
        for index, mutate in enumerate(mutations):
            candidate = json.loads(json.dumps(manifest))
            mutate(candidate)
            with self.subTest(mutation=index), self.assertRaises(tool.InputError):
                tool.validate_manifest(candidate)
        path = self.temp / "bad.json"
        path.write_text("{not json", encoding="utf-8")
        self.assertEqual(_run_tool("compare", path, path).returncode, 2)


class TwoTreeCommandTests(unittest.TestCase):
    """two-tree's configure and build commands, with the CMake runs recorded instead of executed."""

    def setUp(self) -> None:
        self.temp = Path(tempfile.mkdtemp(prefix="spark-two-tree-"))
        self.source = self.temp / "source"
        self.source.mkdir()
        (self.source / "CMakeLists.txt").write_text("project(Fixture NONE)\n", encoding="utf-8")
        self.commands: list[list[str]] = []

    def tearDown(self) -> None:
        shutil.rmtree(self.temp, ignore_errors=True)

    def record(self, command: list[str], log: Path, environment: dict[str, str]) -> None:
        self.commands.append(command)
        if "--build" in command:
            output = Path(command[command.index("--build") + 1]) / "bin" / "fixture.a"
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(_ar_archive(b"same", 0))

    def run_two_tree(self, *extra: str) -> list[list[str]]:
        argv = ["two-tree", "--source", str(self.source), "--work", str(self.temp / "work"),
                "--cmake", "cmake", "--jobs", "2", "--target", "SparkCooker", "--scan", "bin", *extra,
                "--", "-G", "Visual Studio 17 2022"]
        with unittest.mock.patch.object(tool, "_run_logged", side_effect=self.record):
            self.assertEqual(tool.main(argv), 0)
        return [command for command in self.commands if "--build" in command]

    def test_config_reaches_both_builds(self) -> None:
        builds = self.run_two_tree("--config", "MinSizeRel")
        self.assertEqual(len(builds), 2)
        for command in builds:
            self.assertEqual(command[command.index("--config") + 1], "MinSizeRel")
            self.assertEqual(command[-2:], ["--target", "SparkCooker"])
        configures = [command for command in self.commands if "-S" in command]
        self.assertEqual([command[-2:] for command in configures], [["-G", "Visual Studio 17 2022"]] * 2)

    def test_no_config_sends_none(self) -> None:
        builds = self.run_two_tree()
        self.assertEqual(len(builds), 2)
        for command in builds:
            self.assertNotIn("--config", command)


class TwoTreeRegistrationTests(unittest.TestCase):
    """The CTest registrations hand each lane's own toolchain choice to the inner builds."""

    def registration(self, name: str) -> str:
        text = (REPO_ROOT / "Tests" / "CMakeLists.txt").read_text(encoding="utf-8")
        start = text.index(f"NAME {name}")
        return text[start : text.index("set_tests_properties", start)]

    def test_linux_trees_inherit_the_lane_flags(self) -> None:
        # two-tree scrubs CFLAGS/CXXFLAGS/LDFLAGS; without these the Clang lane
        # (LTO off, libc++) would prove a GCC-default libstdc++ LTO build instead.
        block = self.registration("ReproducibleBuild_LinuxToolTargets")
        for argument in (
            "-DENABLE_LTO=${ENABLE_LTO}",
            '"-DCMAKE_C_FLAGS=$CACHE{CMAKE_C_FLAGS}"',
            '"-DCMAKE_CXX_FLAGS=$CACHE{CMAKE_CXX_FLAGS}"',
            '"-DCMAKE_EXE_LINKER_FLAGS=$CACHE{CMAKE_EXE_LINKER_FLAGS}"',
            '"-DCMAKE_SHARED_LINKER_FLAGS=$CACHE{CMAKE_SHARED_LINKER_FLAGS}"',
        ):
            self.assertIn(argument, block)

    def test_windows_trees_build_the_shipping_configuration(self) -> None:
        block = self.registration("ReproducibleBuild_WindowsToolTargets")
        for argument in ("--config MinSizeRel", "--scan bin/MinSizeRel", "--scan lib/MinSizeRel",
                         "-DCMAKE_CONFIGURATION_TYPES=MinSizeRel",
                         "CONFIGURATIONS MinSizeRel", "${_spark_repro_generator_args}"):
            self.assertIn(argument, block)

    def test_windows_repro_job_passes_build_roots_and_scans_static_libraries(self) -> None:
        workflow = (REPO_ROOT / ".github" / "workflows" / "build.yml").read_text(encoding="utf-8")
        block_start = workflow.index("reproducibility-windows")
        block = workflow[block_start:]
        for argument in ("--build-root", "reproducibility-stage-a", "reproducibility-stage-b", "tree-a", "tree-b"):
            self.assertIn(argument, block)


@unittest.skipUnless(ELF_TOOLS, "requires gcc and readelf on Linux")
class ElfFixtureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = Path(tempfile.mkdtemp(prefix="spark-compare-elf-"))

    def tearDown(self) -> None:
        shutil.rmtree(self.temp, ignore_errors=True)

    def test_build_directory_leak_is_found_and_prefix_map_removes_it(self) -> None:
        first_dir, second_dir = self.temp / "a", self.temp / "tree-b" / "nested"
        leaked = tool.compare_manifests(
            tool.build_manifest(_compile(first_dir, "hello")), tool.build_manifest(_compile(second_dir, "hello"))
        )
        self.assertFalse(leaked["equivalent"])
        difference = leaked["differing"][0]
        self.assertEqual(difference["path"], "app")
        self.assertTrue(
            any(name.startswith(".debug_") for name in difference["differingSections"]), difference
        )
        self.assertTrue(difference["firstDifference"].startswith(".debug_"), difference)
        self.assertIn("buildId", difference["identityChanged"])

        mapped = [
            _compile(directory, "hello", f"-ffile-prefix-map={directory}=.")
            for directory in (self.temp / "c", self.temp / "tree-d" / "nested")
        ]
        report = tool.compare_manifests(*(tool.build_manifest(root) for root in mapped))
        self.assertTrue(report["equivalent"], report)

    def test_code_difference_is_located_with_build_id_change(self) -> None:
        first = _compile(self.temp / "a", "hello", f"-ffile-prefix-map={self.temp / 'a'}=.")
        second = _compile(self.temp / "b", "HELLO", f"-ffile-prefix-map={self.temp / 'b'}=.")
        left, right = tool.build_manifest(first), tool.build_manifest(second)
        build_id = subprocess.run(
            [READELF, "-n", str(first / "app")], capture_output=True, text=True, check=True
        ).stdout
        self.assertIn(left["entries"][0]["identity"]["buildId"], build_id)
        report = tool.compare_manifests(left, right)
        difference = report["differing"][0]
        self.assertEqual(difference["identityChanged"], ["buildId"])
        self.assertTrue(difference["firstDifference"].startswith(".rodata"), difference)
        self.assertIn(".note.gnu.build-id", difference["differingSections"])

    def test_cli_reports_first_difference(self) -> None:
        first = _compile(self.temp / "a", "hello", f"-ffile-prefix-map={self.temp / 'a'}=.")
        second = _compile(self.temp / "b", "HELLO", f"-ffile-prefix-map={self.temp / 'b'}=.")
        result = _run_tool("trees", first, second)
        self.assertEqual(result.returncode, 1)
        self.assertIn("differs: app: first difference in .rodata", result.stdout)
        self.assertIn("identity changed: buildId", result.stdout)
        self.assertIn("NOT EQUIVALENT", result.stdout)


if __name__ == "__main__":
    unittest.main()
