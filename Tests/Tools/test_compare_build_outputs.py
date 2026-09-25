#!/usr/bin/env python3
"""BLD-100: tools/compare_build_outputs.py finds every non-reproducible output.

The tests run the tool on real and crafted fixtures:

* gcc ELF executables built from one source in two directories differ only in
  their DWARF when the build directory leaks (the comp_dir/.debug_line_str
  leak the root -ffile-prefix-map options remove); mapped, they are equivalent;
* an executable whose code differs is reported at its first differing content
  section, with the GNU build-id change, not at the derived build-id note;
* ar archives differing only in a member timestamp are reported at that member;
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
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
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

    def test_archive_member_timestamp_is_reported_at_that_member(self) -> None:
        first = self._tree("a", {"lib/libx.a": _ar_archive(b"\x01\x02\x03", 0), "notes.txt": b"x"})
        second = self._tree("b", {"lib/libx.a": _ar_archive(b"\x01\x02\x03", 1700000000), "notes.txt": b"y"})
        left, right = tool.build_manifest(first), tool.build_manifest(second)
        self.assertEqual([entry["path"] for entry in left["entries"]], ["lib/libx.a"], "text files are not eligible")
        report = tool.compare_manifests(left, right)
        self.assertFalse(report["equivalent"])
        self.assertEqual(len(report["differing"]), 1)
        self.assertTrue(report["differing"][0]["firstDifference"].startswith("obj.o "))

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
        self.assertEqual(manifest["schema"], "spark.build-output-manifest/1")
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
