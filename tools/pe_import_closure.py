#!/usr/bin/env python3
"""Check that a staged Windows package carries every DLL its binaries import.

ENG-220: "The Windows package retains all dependencies." The asset half of a
package is checked by tools/asset-integrity/verify_asset_integrity.py; this is
the binary half. Every ``*.exe`` and ``*.dll`` under the package directory is
parsed as a PE image (PE32 or PE32+), and each DLL named by its import table or
its delay-import table must resolve to exactly one of:

* a file in the importer's own directory (package-local, case-insensitive);
* an API set (``api-ms-win-*`` / ``ext-ms-*``), which the loader maps itself;
* an operating-system DLL on ``SYSTEM_DLLS``. When a system directory is
  available (``%SystemRoot%\\System32`` on Windows, or ``--system-dir``) the
  DLL must also be present there.

PATH, the build tree and the source tree are never consulted: a DLL that only
resolves because a developer machine has it on PATH is exactly the dependency
a clean machine lacks. The Visual C++ runtime (vcruntime140*.dll,
msvcp140*.dll, concrt140.dll) is deliberately not a system DLL; the package
must ship it.

A malformed or truncated image, a package with no images at all, and an
unresolved import all fail with exit status 1, and every unresolved
``importer -> dll`` pair is named. The parser is stdlib-only so the contract
test runs on every CI host.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

# DLLs Windows 10+ ships in System32 that SparkEngine binaries may import.
# Keep this list explicit: a new entry is a new clean-machine assumption.
SYSTEM_DLLS = frozenset(
    name.lower()
    for name in (
        "advapi32.dll",
        "bcrypt.dll",
        "cfgmgr32.dll",
        "comctl32.dll",
        "comdlg32.dll",
        "crypt32.dll",
        "d3d11.dll",
        "d3d12.dll",
        "d3dcompiler_47.dll",
        "dbghelp.dll",
        "dwmapi.dll",
        "dxgi.dll",
        "gdi32.dll",
        "hid.dll",
        "imm32.dll",
        "iphlpapi.dll",
        "kernel32.dll",
        "mswsock.dll",
        "ntdll.dll",
        "ole32.dll",
        "oleaut32.dll",
        "opengl32.dll",
        "psapi.dll",
        "setupapi.dll",
        "shcore.dll",
        "shell32.dll",
        "shlwapi.dll",
        "ucrtbase.dll",
        "user32.dll",
        "userenv.dll",
        "uxtheme.dll",
        "version.dll",
        "winhttp.dll",
        "winmm.dll",
        "ws2_32.dll",
        "xaudio2_9.dll",
        "xinput1_4.dll",
    )
)

API_SET_PREFIXES = ("api-ms-win-", "ext-ms-")
IMAGE_EXTENSIONS = (".exe", ".dll")

_IMPORT_DIRECTORY = 1
_DELAY_IMPORT_DIRECTORY = 13
_MAX_DESCRIPTORS = 4096
_MAX_NAME = 512


class PEFormatError(Exception):
    """The file is not a well-formed PE image."""


@dataclass
class ImageImports:
    path: Path
    imports: list[str] = field(default_factory=list)
    delay_imports: list[str] = field(default_factory=list)


def _unpack(fmt: str, data: bytes, offset: int, what: str) -> tuple:
    size = struct.calcsize(fmt)
    if offset < 0 or offset + size > len(data):
        raise PEFormatError(f"truncated {what} at offset {offset:#x}")
    return struct.unpack_from(fmt, data, offset)


class _Image:
    def __init__(self, data: bytes) -> None:
        self.data = data
        (magic,) = _unpack("<H", data, 0, "DOS header")
        if magic != 0x5A4D:
            raise PEFormatError("missing MZ signature")
        (pe_offset,) = _unpack("<I", data, 0x3C, "DOS header")
        (signature,) = _unpack("<I", data, pe_offset, "PE signature")
        if signature != 0x00004550:
            raise PEFormatError("missing PE signature")
        coff = pe_offset + 4
        _machine, section_count, _, _, _, optional_size, _ = _unpack("<HHIIIHH", data, coff, "COFF header")
        optional = coff + 20
        (optional_magic,) = _unpack("<H", data, optional, "optional header")
        if optional_magic == 0x20B:
            (self.image_base,) = _unpack("<Q", data, optional + 24, "optional header")
            directory_count_offset = optional + 108
        elif optional_magic == 0x10B:
            (self.image_base,) = _unpack("<I", data, optional + 28, "optional header")
            directory_count_offset = optional + 92
        else:
            raise PEFormatError(f"unknown optional header magic {optional_magic:#x}")
        (directory_count,) = _unpack("<I", data, directory_count_offset, "optional header")
        directories_offset = directory_count_offset + 4
        if directories_offset + 8 * directory_count > optional + optional_size:
            raise PEFormatError("data directories overrun the optional header")
        self.directories = [
            _unpack("<II", data, directories_offset + 8 * index, "data directory")
            for index in range(min(directory_count, 16))
        ]
        sections_offset = optional + optional_size
        self.sections = []
        for index in range(section_count):
            entry = sections_offset + 40 * index
            virtual_size, virtual_address, raw_size, raw_pointer = _unpack("<IIII", data, entry + 8, "section header")
            self.sections.append((virtual_address, max(virtual_size, raw_size), raw_pointer, raw_size))

    def offset(self, rva: int, what: str) -> int:
        for virtual_address, span, raw_pointer, raw_size in self.sections:
            if virtual_address <= rva < virtual_address + span:
                delta = rva - virtual_address
                if delta >= raw_size:
                    raise PEFormatError(f"{what} RVA {rva:#x} lies in uninitialized section data")
                return raw_pointer + delta
        raise PEFormatError(f"{what} RVA {rva:#x} is outside every section")

    def name(self, rva: int) -> str:
        start = self.offset(rva, "DLL name")
        end = self.data.find(b"\0", start, start + _MAX_NAME)
        if end < 0:
            raise PEFormatError(f"unterminated DLL name at RVA {rva:#x}")
        raw = self.data[start:end]
        if not raw:
            raise PEFormatError(f"empty DLL name at RVA {rva:#x}")
        try:
            return raw.decode("ascii")
        except UnicodeDecodeError as error:
            raise PEFormatError(f"non-ASCII DLL name at RVA {rva:#x}") from error

    def directory(self, index: int) -> tuple[int, int]:
        return self.directories[index] if index < len(self.directories) else (0, 0)

    def imports(self) -> list[str]:
        rva, _size = self.directory(_IMPORT_DIRECTORY)
        if rva == 0:
            return []
        base = self.offset(rva, "import directory")
        names = []
        for index in range(_MAX_DESCRIPTORS):
            descriptor = _unpack("<IIIII", self.data, base + 20 * index, "import descriptor")
            if descriptor == (0, 0, 0, 0, 0):
                return names
            names.append(self.name(descriptor[3]))
        raise PEFormatError("import directory has no terminator")

    def delay_imports(self) -> list[str]:
        rva, _size = self.directory(_DELAY_IMPORT_DIRECTORY)
        if rva == 0:
            return []
        base = self.offset(rva, "delay-import directory")
        names = []
        for index in range(_MAX_DESCRIPTORS):
            descriptor = _unpack("<8I", self.data, base + 32 * index, "delay-import descriptor")
            if descriptor == (0,) * 8:
                return names
            attributes, name_address = descriptor[0], descriptor[1]
            # Attribute bit 0 set means RVAs; legacy VC6 descriptors hold VAs.
            name_rva = name_address if attributes & 1 else name_address - self.image_base
            names.append(self.name(name_rva))
        raise PEFormatError("delay-import directory has no terminator")


def read_imports(path: Path) -> ImageImports:
    """Parse one PE image. Raises PEFormatError when the image is malformed."""
    image = _Image(path.read_bytes())
    return ImageImports(path=path, imports=image.imports(), delay_imports=image.delay_imports())


def default_system_directory() -> Path | None:
    if os.name != "nt":
        return None
    return Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"


def _directory_listing(directory: Path, cache: dict[Path, set[str]]) -> set[str]:
    if directory not in cache:
        cache[directory] = {entry.name.lower() for entry in directory.iterdir() if entry.is_file()}
    return cache[directory]


def check_package(root: Path, system_directory: Path | None) -> tuple[int, list[str]]:
    """Return (image count, failures) for every PE image under root."""
    failures: list[str] = []
    listings: dict[Path, set[str]] = {}
    system_listing = None
    if system_directory is not None:
        if not system_directory.is_dir():
            return 0, [f"system directory {system_directory} does not exist"]
        system_listing = _directory_listing(system_directory, listings)

    images = sorted(
        path for path in root.rglob("*") if path.is_file() and path.suffix.lower() in IMAGE_EXTENSIONS
    )
    for path in images:
        label = path.relative_to(root).as_posix()
        try:
            parsed = read_imports(path)
        except (PEFormatError, OSError) as error:
            failures.append(f"{label}: malformed PE image: {error}")
            continue
        local = _directory_listing(path.parent, listings)
        for kind, names in (("import", parsed.imports), ("delay-import", parsed.delay_imports)):
            for name in names:
                lowered = name.lower()
                if lowered in local or lowered.startswith(API_SET_PREFIXES):
                    continue
                if lowered in SYSTEM_DLLS:
                    if system_listing is None or lowered in system_listing:
                        continue
                    failures.append(f"{label} -> {name} ({kind}): system DLL missing from {system_directory}")
                    continue
                failures.append(f"{label} -> {name} ({kind}): not in the package and not an allowed system DLL")
    return len(images), failures


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("package_dir", type=Path, help="staged directory holding the package binaries (e.g. <prefix>/bin)")
    parser.add_argument(
        "--system-dir",
        type=Path,
        help="directory that must hold allowed system DLLs (default: %%SystemRoot%%\\System32 on Windows)",
    )
    parser.add_argument("--list", action="store_true", help="print every image's imports as well")
    args = parser.parse_args(argv)

    root = args.package_dir
    if not root.is_dir():
        print(f"pe_import_closure: {root} is not a directory", file=sys.stderr)
        return 1
    system_directory = args.system_dir if args.system_dir is not None else default_system_directory()

    if args.list:
        for path in sorted(p for p in root.rglob("*") if p.is_file() and p.suffix.lower() in IMAGE_EXTENSIONS):
            try:
                parsed = read_imports(path)
            except (PEFormatError, OSError) as error:
                print(f"{path.relative_to(root).as_posix()}: malformed: {error}")
                continue
            print(f"{path.relative_to(root).as_posix()}: {', '.join(parsed.imports)}"
                  f"{' | delay: ' + ', '.join(parsed.delay_imports) if parsed.delay_imports else ''}")

    image_count, failures = check_package(root, system_directory)
    if image_count == 0 and not failures:
        failures.append(f"no *.exe or *.dll images under {root}")
    for failure in failures:
        print(f"pe_import_closure: FAIL {failure}", file=sys.stderr)
    if failures:
        print(f"pe_import_closure: {len(failures)} unresolved dependency problem(s) in {image_count} image(s)",
              file=sys.stderr)
        return 1
    checked = system_directory if system_directory is not None else "allowlist only"
    print(f"pe_import_closure: {image_count} image(s) under {root} resolve (system DLLs: {checked})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
