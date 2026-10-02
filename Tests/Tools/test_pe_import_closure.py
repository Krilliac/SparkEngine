#!/usr/bin/env python3
"""ENG-220: tools/pe_import_closure.py proves a staged Windows package's DLL closure.

The fixtures are minimal PE32+ (and one PE32) images built in memory with real
import and delay-import directories, so the contract runs on every CI host:

* a package whose imports all resolve locally, to API sets or to present
  allowlisted system DLLs passes, and the local match is case-insensitive;
* a missing package DLL fails and names the importer and the DLL;
* a delay-import miss fails the same way;
* a system-looking DLL that is not allowlisted fails even when the system
  directory holds it (the Visual C++ runtime is the case that matters);
* an allowlisted system DLL missing from the system directory fails;
* a DLL that only resolves through PATH fails: PATH is never consulted;
* a DLL beside a different importer does not satisfy this one;
* truncated, non-PE and unterminated images fail closed, and an empty
  package fails instead of passing vacuously.
"""

from __future__ import annotations

import importlib.util
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CHECKER = REPO_ROOT / "tools" / "pe_import_closure.py"

_spec = importlib.util.spec_from_file_location("pe_import_closure", CHECKER)
closure = importlib.util.module_from_spec(_spec)
sys.modules["pe_import_closure"] = closure
_spec.loader.exec_module(closure)

SECTION_RVA = 0x1000
SECTION_FILE_OFFSET = 0x200
IMAGE_BASE = 0x140000000


def make_pe(
    imports: tuple[str, ...] = (),
    delay_imports: tuple[str, ...] = (),
    *,
    pe32: bool = False,
    legacy_delay_vas: bool = False,
    terminate_imports: bool = True,
) -> bytes:
    """Build a PE image with one section holding the import tables and names."""
    names = list(imports) + list(delay_imports)
    import_table = 20 * (len(imports) + 1) if imports else 0
    delay_table = 32 * (len(delay_imports) + 1) if delay_imports else 0
    names_offset = import_table + delay_table
    name_rvas = []
    blob = bytearray()
    for name in names:
        name_rvas.append(SECTION_RVA + names_offset + len(blob))
        blob += name.encode("ascii") + b"\0"

    section = bytearray()
    for index in range(len(imports)):
        section += struct.pack("<IIIII", 0, 0, 0, name_rvas[index], 0x10)
    if imports:
        section += b"\0" * 20 if terminate_imports else b"\xff" * 20
    for index in range(len(delay_imports)):
        name_rva = name_rvas[len(imports) + index]
        if legacy_delay_vas:
            section += struct.pack("<8I", 0, (IMAGE_BASE + name_rva) & 0xFFFFFFFF, 0, 0, 0, 0, 0, 0)
        else:
            section += struct.pack("<8I", 1, name_rva, 0, 0, 0, 0, 0, 0)
    if delay_imports:
        section += b"\0" * 32
    section += blob
    section += b"\0" * (-len(section) % 0x200 or 0x200)

    directories = [(0, 0)] * 16
    if imports:
        directories[1] = (SECTION_RVA, import_table)
    if delay_imports:
        directories[13] = (SECTION_RVA + import_table, delay_table)

    optional = bytearray()
    if pe32:
        optional += struct.pack("<H", 0x10B) + b"\0" * 26 + struct.pack("<I", IMAGE_BASE & 0xFFFFFFFF)
        optional += b"\0" * (92 - len(optional))
    else:
        optional += struct.pack("<H", 0x20B) + b"\0" * 22 + struct.pack("<Q", IMAGE_BASE)
        optional += b"\0" * (108 - len(optional))
    optional += struct.pack("<I", 16)
    for rva, size in directories:
        optional += struct.pack("<II", rva, size)

    coff = struct.pack("<4sHHIIIHH", b"PE\0\0", 0x14C if pe32 else 0x8664, 1, 0, 0, 0, len(optional), 0x2022)
    section_header = struct.pack(
        "<8sIIIIIIHHI", b".idata", len(section), SECTION_RVA, len(section), SECTION_FILE_OFFSET, 0, 0, 0, 0, 0xC0000040
    )
    dos = bytearray(b"MZ" + b"\0" * 62)
    struct.pack_into("<I", dos, 0x3C, 0x40)
    headers = bytes(dos) + coff + bytes(optional) + section_header
    assert len(headers) <= SECTION_FILE_OFFSET
    return headers + b"\0" * (SECTION_FILE_OFFSET - len(headers)) + bytes(section)


class ClosureTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.package = self.root / "package"
        self.bin = self.package / "bin"
        self.bin.mkdir(parents=True)
        self.system = self.root / "System32"
        self.system.mkdir()
        for name in ("kernel32.dll", "USER32.dll", "d3d11.dll", "msvcp140.dll"):
            (self.system / name).write_bytes(b"system")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def write(self, relative: str, data: bytes) -> Path:
        path = self.package / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def check(self) -> tuple[int, list[str]]:
        return closure.check_package(self.package, self.system)

    def assert_single_failure(self, *fragments: str) -> None:
        count, failures = self.check()
        self.assertGreater(count, 0)
        self.assertEqual(len(failures), 1, failures)
        for fragment in fragments:
            self.assertIn(fragment, failures[0])


class ParserTests(unittest.TestCase):
    def test_reads_imports_and_delay_imports(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "a.exe"
            path.write_bytes(make_pe(("KERNEL32.dll", "Local.dll"), ("d3d11.dll",)))
            parsed = closure.read_imports(path)
        self.assertEqual(parsed.imports, ["KERNEL32.dll", "Local.dll"])
        self.assertEqual(parsed.delay_imports, ["d3d11.dll"])

    def test_reads_pe32_and_legacy_virtual_address_delay_descriptors(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "a.dll"
            path.write_bytes(make_pe(("kernel32.dll",), ("winmm.dll",), pe32=True, legacy_delay_vas=True))
            parsed = closure.read_imports(path)
        self.assertEqual(parsed.imports, ["kernel32.dll"])
        self.assertEqual(parsed.delay_imports, ["winmm.dll"])

    @unittest.skipUnless(os.name == "nt", "a real Windows image is only available on Windows")
    def test_reads_a_real_windows_image(self) -> None:
        parsed = closure.read_imports(Path(sys.executable))
        self.assertIn("kernel32.dll", {name.lower() for name in parsed.imports})


class ClosureTests(ClosureTestCase):
    def test_all_local_api_set_and_system_imports_pass(self) -> None:
        self.write("bin/SparkEngine.exe", make_pe(("kernel32.dll", "SDL2.dll", "api-ms-win-crt-heap-l1-1-0.dll")))
        self.write("bin/SDL2.dll", make_pe(("KERNEL32.dll", "ext-ms-win-gdi-dc-l1-2-0.dll"), ("user32.dll",)))
        self.write("bin/Tools/Helper.exe", make_pe(("kernel32.dll",)))
        self.assertEqual(self.check(), (3, []))

    def test_local_match_is_case_insensitive(self) -> None:
        self.write("bin/SparkEngine.exe", make_pe(("SPARKGAMEFPS.DLL",)))
        self.write("bin/SparkGameFPS.dll", make_pe(("kernel32.dll",)))
        self.assertEqual(self.check(), (2, []))

    def test_missing_package_dll_names_importer(self) -> None:
        self.write("bin/SparkEngine.exe", make_pe(("kernel32.dll", "SDL2.dll")))
        self.assert_single_failure("bin/SparkEngine.exe -> SDL2.dll (import)", "not in the package")

    def test_delay_import_miss_fails(self) -> None:
        self.write("bin/SparkGameFPS.dll", make_pe(("kernel32.dll",), ("SparkPhysics.dll",)))
        self.assert_single_failure("bin/SparkGameFPS.dll -> SparkPhysics.dll (delay-import)")

    def test_visual_cpp_runtime_is_not_a_system_dll(self) -> None:
        # msvcp140.dll is present in this System32 (as on most developer machines);
        # the package must still ship it.
        self.write("bin/SparkEngine.exe", make_pe(("kernel32.dll", "MSVCP140.dll")))
        self.assert_single_failure("bin/SparkEngine.exe -> MSVCP140.dll (import)", "not an allowed system DLL")

    def test_shipped_visual_cpp_runtime_resolves(self) -> None:
        self.write("bin/SparkEngine.exe", make_pe(("kernel32.dll", "MSVCP140.dll", "VCRUNTIME140.dll")))
        self.write("bin/msvcp140.dll", make_pe(("kernel32.dll", "vcruntime140.dll")))
        self.write("bin/vcruntime140.dll", make_pe(("kernel32.dll",)))
        self.assertEqual(self.check(), (3, []))

    def test_allowlisted_system_dll_must_exist_in_system_directory(self) -> None:
        self.write("bin/SparkEngine.exe", make_pe(("kernel32.dll", "xaudio2_9.dll")))
        self.assert_single_failure("bin/SparkEngine.exe -> xaudio2_9.dll (import)", "missing from")

    def test_dll_beside_another_importer_does_not_resolve(self) -> None:
        self.write("bin/SparkEngine.exe", make_pe(("kernel32.dll", "SDL2.dll")))
        self.write("bin/plugins/SDL2.dll", make_pe(("kernel32.dll",)))
        self.assert_single_failure("bin/SparkEngine.exe -> SDL2.dll (import)")

    def test_dll_only_on_path_fails(self) -> None:
        self.write("bin/SparkEngine.exe", make_pe(("kernel32.dll", "SDL2.dll")))
        elsewhere = self.root / "on-path"
        elsewhere.mkdir()
        (elsewhere / "SDL2.dll").write_bytes(make_pe(("kernel32.dll",)))
        environment = dict(os.environ, PATH=f"{elsewhere}{os.pathsep}{os.environ.get('PATH', '')}")
        result = subprocess.run(
            [sys.executable, "-B", str(CHECKER), str(self.package), "--system-dir", str(self.system)],
            capture_output=True,
            text=True,
            env=environment,
            check=False,
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("bin/SparkEngine.exe -> SDL2.dll (import)", result.stderr)

    def test_system_directory_must_exist(self) -> None:
        self.write("bin/SparkEngine.exe", make_pe(("kernel32.dll",)))
        _count, failures = closure.check_package(self.package, self.root / "missing-system")
        self.assertEqual(len(failures), 1)
        self.assertIn("does not exist", failures[0])


class MalformedImageTests(ClosureTestCase):
    def test_truncated_image_fails_closed(self) -> None:
        self.write("bin/SparkEngine.exe", make_pe(("kernel32.dll",))[:0x150])
        self.assert_single_failure("bin/SparkEngine.exe: malformed PE image", "truncated")

    def test_non_pe_dll_fails_closed(self) -> None:
        self.write("bin/SparkGameFPS.dll", b"not an image")
        self.assert_single_failure("bin/SparkGameFPS.dll: malformed PE image")

    def test_image_whose_names_point_outside_sections_fails_closed(self) -> None:
        image = bytearray(make_pe(("kernel32.dll",)))
        struct.pack_into("<I", image, SECTION_FILE_OFFSET + 12, 0x7FFF0000)
        self.write("bin/SparkEngine.exe", bytes(image))
        self.assert_single_failure("bin/SparkEngine.exe: malformed PE image", "outside every section")

    def test_unterminated_import_directory_fails_closed(self) -> None:
        self.write("bin/SparkEngine.exe", make_pe(("kernel32.dll",), terminate_imports=False))
        self.assert_single_failure("bin/SparkEngine.exe: malformed PE image")


class CommandLineTests(ClosureTestCase):
    def run_checker(self, *arguments: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-B", str(CHECKER), *arguments], capture_output=True, text=True, check=False
        )

    def test_empty_package_fails_instead_of_passing(self) -> None:
        result = self.run_checker(str(self.package), "--system-dir", str(self.system))
        self.assertEqual(result.returncode, 1)
        self.assertIn("no *.exe or *.dll images", result.stderr)

    def test_missing_package_directory_fails(self) -> None:
        result = self.run_checker(str(self.root / "absent"), "--system-dir", str(self.system))
        self.assertEqual(result.returncode, 1)

    def test_passing_package_reports_its_image_count(self) -> None:
        self.write("bin/SparkEngine.exe", make_pe(("kernel32.dll",)))
        result = self.run_checker(str(self.package), "--system-dir", str(self.system))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("1 image(s)", result.stdout)


if __name__ == "__main__":
    unittest.main()
