#!/usr/bin/env python3
"""Measure a staged Windows package's runtime dependency closure from PE imports.

A dependency closure that is typed into a plan is a claim.  This module turns
it into a measurement: it opens every PE32+ image in a staged package, reads
the import directory and the delay-import directory, and resolves each
imported DLL name the way the Windows loader would for an application-local
deployment:

  - ``api-ms-win-*`` / ``ext-ms-*`` API-set contracts are the operating
    system's, and are recorded as such;
  - a KnownDLL (``KNOWN_DLLS``) always comes from the system directory, so a
    same-named file in the package never satisfies it;
  - any other DLL that sits in the package root (the application directory)
    is package-local, and its bytes are hashed;
  - every other name must be a ``platformRuntime`` entry of the dependency
    authority (docs/certification/dependency-authority.json), which supplies
    its source; anything else is *unresolved*.

The result is an import graph.  ``closure_errors`` then compares that graph
with a declared closure: every measured non-API-set import must be declared
(or be one of the product's own first-party images), every declared entry
must actually be imported, and an unresolved import is always an error.  A
package-local DLL is declared under the authority identity it ships: an
app-local copy of a ``platformRuntime`` library under its own name and
source, a vendored library under its ``thirdParty`` name through that entry's
reviewed ``imageNames``.  A name the operating system owns (API set,
KnownDLL, ``system``-source runtime) can never be package-local or first-party.

The reader is an evidence tool that parses untrusted bytes, so it is strictly
bounded -- image size, section count, descriptor count and name length -- and
every malformed or out-of-range header fails closed with ``PEFormatError``.
It never guesses: an RVA outside every section, a truncated header, an
unterminated name or an unsupported image type is refused, not skipped.

Exit codes of the command-line entry point:
  0  the package was measured and matches the declaration
  1  the package was measured but the closure is inconsistent (the graph is
     still printed on stdout, the reasons go to stderr)
  2  the package could not be measured (nothing is printed on stdout)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any

import dependency_authority as da
import safe_fs

TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parents[1]

GRAPH_SCHEMA_VERSION = 1
GRAPH_KIND = "spark-pe-import-graph"
GRAPH_SUFFIX = ".imports.json"

# ── Resource limits ────────────────────────────────────────────────────────
MAX_IMAGE_BYTES = 512 * 1024 * 1024
MAX_E_LFANEW = 64 * 1024 * 1024
MAX_SECTIONS = 96  # the Windows loader's own ceiling
MAX_DATA_DIRECTORIES = 16
MAX_IMPORT_DESCRIPTORS = 4096
MAX_DLL_NAME_BYTES = 255
MAX_PACKAGE_FILES = 20000
MAX_PACKAGE_DEPTH = 16
MAX_PACKAGE_BYTES = 64 * 1024 * 1024 * 1024
MAX_PACKAGE_IMAGES = 1024
MAX_DECLARED_ENTRIES = da.MAX_THIRD_PARTY_ENTRIES + da.MAX_PLATFORM_ENTRIES
MAX_DOCUMENT_BYTES = 8 * 1024 * 1024

# ── PE constants ───────────────────────────────────────────────────────────
_DOS_HEADER_BYTES = 64
_COFF_HEADER_BYTES = 20
_SECTION_HEADER_BYTES = 40
_IMPORT_DESCRIPTOR_BYTES = 20
_DELAY_DESCRIPTOR_BYTES = 32
_PE32_PLUS_MAGIC = 0x20B
_PE32_MAGIC = 0x10B
_PE32_PLUS_DIRECTORY_OFFSET = 112
_MACHINE_AMD64 = 0x8664
_IMPORT_DIRECTORY = 1
_DELAY_IMPORT_DIRECTORY = 13
_DELAY_ATTRIBUTE_RVA_BASED = 0x1

IMAGE_SUFFIXES = frozenset({".exe", ".dll"})
RESOLUTIONS = ("package", "api-set", "platform", "unresolved")

_API_SET_RE = re.compile(r"\A(?:api-ms-win-|ext-ms-)[a-z0-9-]+\.dll\Z")
_FORBIDDEN_NAME_CHARS = frozenset('/\\:*?"<>|')
_SHA256_RE = re.compile(r"\A[0-9a-f]{64}\Z")

# The DLLs Windows 10/11 x64 register under HKLM\SYSTEM\CurrentControlSet\
# Control\Session Manager\KnownDLLs, plus ntdll.dll and kernelbase.dll, which
# the loader maps itself.  The loader takes these from the system directory
# and never from the application directory, so a package that ships one does
# not change what loads.  This is a refusal list, not an OS inventory: other
# OS DLLs still have to be platformRuntime entries of the authority.
KNOWN_DLLS = frozenset(
    {
        "advapi32.dll",
        "clbcatq.dll",
        "combase.dll",
        "comdlg32.dll",
        "coml2.dll",
        "difxapi.dll",
        "gdi32.dll",
        "gdiplus.dll",
        "imagehlp.dll",
        "imm32.dll",
        "kernel32.dll",
        "kernelbase.dll",
        "msctf.dll",
        "msvcrt.dll",
        "normaliz.dll",
        "nsi.dll",
        "ntdll.dll",
        "ole32.dll",
        "oleaut32.dll",
        "psapi.dll",
        "rpcrt4.dll",
        "sechost.dll",
        "setupapi.dll",
        "shcore.dll",
        "shell32.dll",
        "shlwapi.dll",
        "user32.dll",
        "wldap32.dll",
        "wow64.dll",
        "wow64base.dll",
        "wow64con.dll",
        "wow64cpu.dll",
        "wow64win.dll",
        "ws2_32.dll",
    }
)


class PEFormatError(Exception):
    """The bytes are not a PE32+ image this reader will vouch for."""


class ClosureError(Exception):
    """The package or the declaration cannot be measured honestly."""


# ── PE32+ reader ───────────────────────────────────────────────────────────


@dataclass(frozen=True)
class _Section:
    virtual_address: int
    virtual_size: int
    raw_pointer: int
    raw_size: int


@dataclass(frozen=True)
class ImageImports:
    """The DLL names one image imports, normalised to the loader's identity."""

    machine: str
    imports: tuple[str, ...]
    delay_imports: tuple[str, ...]


def _unpack(fmt: str, data: bytes, offset: int, what: str) -> tuple[int, ...]:
    size = struct.calcsize(fmt)
    if offset < 0 or offset + size > len(data):
        raise PEFormatError(f"{what} at offset {offset:#x} is truncated")
    return struct.unpack_from(fmt, data, offset)


class _Image:
    """Bounded address translation over one image's section table."""

    def __init__(self, data: bytes) -> None:
        self.data = data
        if len(data) > MAX_IMAGE_BYTES:
            raise PEFormatError(f"image is {len(data)} bytes (max {MAX_IMAGE_BYTES})")
        if len(data) < _DOS_HEADER_BYTES or data[:2] != b"MZ":
            raise PEFormatError("missing DOS header")
        (e_lfanew,) = _unpack("<I", data, 0x3C, "e_lfanew")
        if e_lfanew < _DOS_HEADER_BYTES or e_lfanew > MAX_E_LFANEW:
            raise PEFormatError(f"e_lfanew {e_lfanew:#x} is out of range")
        if data[e_lfanew : e_lfanew + 4] != b"PE\x00\x00":
            raise PEFormatError("missing PE signature")

        coff = e_lfanew + 4
        machine, section_count, _stamp, _symbols, _count, optional_size, _flags = _unpack(
            "<HHIIIHH", data, coff, "COFF header"
        )
        if machine != _MACHINE_AMD64:
            raise PEFormatError(f"unsupported machine {machine:#06x} (only AMD64 is certified)")
        if not 1 <= section_count <= MAX_SECTIONS:
            raise PEFormatError(
                f"section count {section_count} is out of range (1..{MAX_SECTIONS})"
            )

        optional = coff + _COFF_HEADER_BYTES
        (magic,) = _unpack("<H", data, optional, "optional header magic")
        if magic == _PE32_MAGIC:
            raise PEFormatError("PE32 (32-bit) images are not supported; expected PE32+")
        if magic != _PE32_PLUS_MAGIC:
            raise PEFormatError(f"unknown optional header magic {magic:#06x}")
        (self.image_base,) = _unpack("<Q", data, optional + 24, "ImageBase")
        (directory_count,) = _unpack("<I", data, optional + 108, "NumberOfRvaAndSizes")
        if directory_count > MAX_DATA_DIRECTORIES:
            raise PEFormatError(
                f"NumberOfRvaAndSizes {directory_count} exceeds {MAX_DATA_DIRECTORIES}"
            )
        if optional_size < _PE32_PLUS_DIRECTORY_OFFSET + 8 * directory_count:
            raise PEFormatError(
                f"SizeOfOptionalHeader {optional_size} cannot hold {directory_count} data directories"
            )
        self.directories: list[tuple[int, int]] = [
            _unpack("<II", data, optional + _PE32_PLUS_DIRECTORY_OFFSET + 8 * i, "data directory")
            for i in range(directory_count)
        ]

        table = optional + optional_size
        if table + section_count * _SECTION_HEADER_BYTES > len(data):
            raise PEFormatError("section table is truncated")
        self.sections: list[_Section] = []
        for index in range(section_count):
            base = table + index * _SECTION_HEADER_BYTES
            virtual_size, virtual_address, raw_size, raw_pointer = _unpack(
                "<IIII", data, base + 8, "section header"
            )
            if raw_size and raw_pointer + raw_size > len(data):
                raise PEFormatError(f"section {index} raw data extends past the end of the image")
            self.sections.append(_Section(virtual_address, virtual_size, raw_pointer, raw_size))
        self.machine = "amd64"

    def directory(self, index: int) -> tuple[int, int]:
        return self.directories[index] if index < len(self.directories) else (0, 0)

    def offset(self, rva: int, length: int, what: str) -> int:
        """File offset of `length` initialised bytes at `rva`, or refuse."""
        for section in self.sections:
            span = max(section.virtual_size, section.raw_size)
            if section.virtual_address <= rva < section.virtual_address + span:
                delta = rva - section.virtual_address
                if delta + length > section.raw_size:
                    raise PEFormatError(
                        f"{what} at RVA {rva:#x} lies outside the section's file data"
                    )
                return section.raw_pointer + delta
        raise PEFormatError(f"{what} at RVA {rva:#x} is outside every section")

    def dll_name(self, rva: int, what: str) -> str:
        start = self.offset(rva, 1, what)
        section_end = next(
            s.raw_pointer + s.raw_size
            for s in self.sections
            if s.raw_pointer <= start < s.raw_pointer + s.raw_size
        )
        limit = min(section_end, start + MAX_DLL_NAME_BYTES + 1)
        end = self.data.find(b"\x00", start, limit)
        if end < 0:
            raise PEFormatError(
                f"{what} at RVA {rva:#x} is unterminated or longer than {MAX_DLL_NAME_BYTES} bytes"
            )
        return normalise_dll_name(self.data[start:end], what)


def normalise_dll_name(raw: bytes, what: str = "DLL name") -> str:
    """The loader's identity for an imported name, or refuse it."""
    if not raw:
        raise PEFormatError(f"{what} is empty")
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError as exc:
        raise PEFormatError(f"{what} is not ASCII") from exc
    if any(ord(ch) < 0x20 or ord(ch) > 0x7E or ch in _FORBIDDEN_NAME_CHARS for ch in text):
        raise PEFormatError(f"{what} {text!r} contains a path or control character")
    if text.strip(".") == "":
        raise PEFormatError(f"{what} {text!r} is not a file name")
    name = text.casefold()
    # The loader appends ".dll" to an import name that has no extension.
    return name if "." in name else f"{name}.dll"


def _descriptor_names(image: _Image, directory: int, entry_bytes: int, *, delay: bool) -> list[str]:
    rva, size = image.directory(directory)
    if rva == 0:
        if size != 0:
            raise PEFormatError(f"data directory {directory} has a size but no address")
        return []
    kind = "delay-import" if delay else "import"
    names: list[str] = []
    for index in range(MAX_IMPORT_DESCRIPTORS + 1):
        if index == MAX_IMPORT_DESCRIPTORS:
            raise PEFormatError(
                f"{kind} directory has more than {MAX_IMPORT_DESCRIPTORS} descriptors"
            )
        offset = image.offset(rva + index * entry_bytes, entry_bytes, f"{kind} descriptor {index}")
        raw = image.data[offset : offset + entry_bytes]
        if raw == b"\x00" * entry_bytes:
            break
        if delay:
            attributes, name_ref = struct.unpack_from("<II", raw, 0)
            if attributes & _DELAY_ATTRIBUTE_RVA_BASED:
                name_rva = name_ref
            else:
                # Legacy descriptors hold a 32-bit virtual address, not an RVA.
                name_rva = name_ref - image.image_base
                if name_rva < 0:
                    raise PEFormatError(
                        f"{kind} descriptor {index} name address is below ImageBase"
                    )
        else:
            (name_rva,) = struct.unpack_from("<I", raw, 12)
        if name_rva == 0:
            raise PEFormatError(f"{kind} descriptor {index} has no name")
        names.append(image.dll_name(name_rva, f"{kind} descriptor {index} name"))
    return names


def parse_imports(data: bytes) -> ImageImports:
    """Read the import and delay-import DLL names from a PE32+ image."""
    image = _Image(data)
    imports = _descriptor_names(image, _IMPORT_DIRECTORY, _IMPORT_DESCRIPTOR_BYTES, delay=False)
    delay_imports = _descriptor_names(
        image, _DELAY_IMPORT_DIRECTORY, _DELAY_DESCRIPTOR_BYTES, delay=True
    )
    return ImageImports(
        image.machine, tuple(sorted(set(imports))), tuple(sorted(set(delay_imports)))
    )


def is_api_set(name: str) -> bool:
    return _API_SET_RE.match(name.casefold()) is not None


def os_owned_reason(name: str, platform: dict[str, str]) -> str | None:
    """Why the operating system, not the package, owns a DLL name (or None)."""
    if is_api_set(name):
        return "is an operating-system API set"
    if name in KNOWN_DLLS:
        return "is a KnownDLL the loader always maps from the system directory"
    if platform.get(name) == "system":
        return "is a 'system' platformRuntime library that must come from the OS"
    return None


# ── Package walk ───────────────────────────────────────────────────────────


def platform_sources(authority: da.Authority) -> dict[str, str]:
    """platformRuntime DLL name -> source, refusing an ambiguous name."""
    sources: dict[str, str] = {}
    for entry in authority.document["platformRuntime"]:
        name = entry["name"].casefold()
        if name in sources and sources[name] != entry["source"]:
            raise ClosureError(f"the dependency authority names {name!r} under two sources")
        sources[name] = entry["source"]
    return sources


def walk_package(
    package_root: Path, authority: da.Authority, first_party: list[str]
) -> dict[str, Any]:
    """Measure every PE image in a staged package and build its import graph."""
    try:
        files = safe_fs.iter_bundle_files(
            package_root,
            max_files=MAX_PACKAGE_FILES,
            max_depth=MAX_PACKAGE_DEPTH,
            max_total_bytes=MAX_PACKAGE_BYTES,
        )
    except safe_fs.FileSecurityError as exc:
        raise ClosureError(f"package root cannot be walked: {exc}") from exc

    candidates = [path for path in files if path.suffix.casefold() in IMAGE_SUFFIXES]
    if len(candidates) > MAX_PACKAGE_IMAGES:
        raise ClosureError(f"package holds more than {MAX_PACKAGE_IMAGES} PE images")

    images: list[dict[str, Any]] = []
    local: dict[str, str] = {}
    for path in candidates:
        relative = path.relative_to(package_root).as_posix()
        try:
            data = safe_fs.read_bounded(path, max_bytes=MAX_IMAGE_BYTES)
            parsed = parse_imports(data)
        except (safe_fs.FileSecurityError, PEFormatError) as exc:
            raise ClosureError(f"{relative}: {exc}") from exc
        if "/" not in relative:
            key = relative.casefold()
            if key in local:
                raise ClosureError(f"{relative} and {local[key]} are the same file name on Windows")
            local[key] = relative
        images.append(
            {
                "path": relative,
                "sha256": hashlib.sha256(data).hexdigest(),
                "sizeBytes": len(data),
                "machine": parsed.machine,
                "imports": list(parsed.imports),
                "delayImports": list(parsed.delay_imports),
            }
        )

    if not any(
        image["path"].casefold().endswith(".exe") and "/" not in image["path"] for image in images
    ):
        raise ClosureError("package root holds no executable image")

    platform = platform_sources(authority)
    importers: dict[str, set[str]] = {}
    for image in images:
        for name in image["imports"] + image["delayImports"]:
            importers.setdefault(name, set()).add(image["path"])

    dependencies: list[dict[str, Any]] = []
    for name in sorted(importers):
        entry: dict[str, Any] = {"name": name, "importedBy": sorted(importers[name])}
        if is_api_set(name):
            entry["resolution"] = "api-set"
        elif name in local and name not in KNOWN_DLLS:
            entry["resolution"] = "package"
            entry["packagePath"] = local[name]
        elif name in platform:
            entry["resolution"] = "platform"
            entry["source"] = platform[name]
        else:
            entry["resolution"] = "unresolved"
        dependencies.append(entry)

    return {
        "schemaVersion": GRAPH_SCHEMA_VERSION,
        "kind": GRAPH_KIND,
        "firstPartyImages": sorted({normalise_first_party(name) for name in first_party}),
        "images": sorted(images, key=lambda image: image["path"]),
        "dependencies": dependencies,
    }


def normalise_first_party(name: Any) -> str:
    if not isinstance(name, str):
        raise ClosureError("firstPartyImages entries must be strings")
    try:
        folded = normalise_dll_name(name.encode("ascii", errors="strict"), "firstPartyImages entry")
    except (UnicodeEncodeError, PEFormatError) as exc:
        raise ClosureError(f"firstPartyImages entry {name!r} is not a file name") from exc
    if PurePosixPath(folded).suffix not in IMAGE_SUFFIXES:
        raise ClosureError(f"firstPartyImages entry {name!r} is not a .exe or .dll")
    return folded


def encode_graph(graph: dict[str, Any]) -> bytes:
    """The canonical bytes of a graph; the artifact is named by their hash."""
    return (json.dumps(graph, indent=1, sort_keys=True, ensure_ascii=True) + "\n").encode("ascii")


# ── Declarations and comparison ────────────────────────────────────────────


@dataclass(frozen=True)
class Declaration:
    first_party: list[str]
    closure: list[dict[str, str]]


def parse_declaration(plan: dict[str, Any]) -> Declaration:
    """The closure and first-party images a plan declares, strictly shaped.

    A plan names dependencies; it never carries a path, hash or size, because
    those can only come from measuring the package.
    """
    first_party = plan.get("firstPartyImages", [])
    if not isinstance(first_party, list) or len(first_party) > MAX_PACKAGE_IMAGES:
        raise ClosureError("firstPartyImages must be a bounded list")
    normalised = [normalise_first_party(name) for name in first_party]
    if len(set(normalised)) != len(normalised):
        raise ClosureError("firstPartyImages lists an image twice")

    closure = plan.get("dependencyClosure", [])
    if not isinstance(closure, list) or len(closure) > MAX_DECLARED_ENTRIES:
        raise ClosureError("dependencyClosure must be a bounded list")
    entries: list[dict[str, str]] = []
    for index, entry in enumerate(closure):
        if not isinstance(entry, dict) or set(entry) != {"name", "version", "source"}:
            raise ClosureError(
                f"dependencyClosure[{index}] must hold exactly name, version and source; "
                "path, sha256 and sizeBytes are measured, not declared"
            )
        if not all(isinstance(entry[key], str) and entry[key] for key in entry):
            raise ClosureError(f"dependencyClosure[{index}] fields must be non-empty strings")
        if entry["source"] not in da.VALID_SOURCES:
            raise ClosureError(f"dependencyClosure[{index}].source {entry['source']!r} is unknown")
        entries.append(dict(entry))
    return Declaration(normalised, entries)


def closure_errors(graph: dict[str, Any], closure: Any, authority: da.Authority) -> list[str]:
    """Where a declared closure disagrees with the measured import graph."""
    if not isinstance(closure, list):
        return ["dependencyClosure must be a list"]
    try:
        platform = platform_sources(authority)
    except ClosureError as exc:
        return [str(exc)]
    errors: list[str] = []
    first_party = set(graph["firstPartyImages"])
    local = {image["path"].casefold() for image in graph["images"] if "/" not in image["path"]}
    for name in sorted(first_party - local):
        errors.append(f"first-party image {name!r} is not in the package root")
    for name in sorted(first_party):
        reason = os_owned_reason(name, platform)
        if reason is None and name in platform:
            reason = f"is the {platform[name]!r} platformRuntime library of that name"
        if reason is None and name in authority.package_images:
            reason = f"is the image of third-party dependency {authority.package_images[name][0]!r}"
        if reason is not None:
            errors.append(f"first-party image {name!r} {reason}, not a product image")

    measured = {dep["name"]: dep for dep in graph["dependencies"]}
    declared: dict[str, dict[str, Any]] = {}
    for entry in closure:
        if isinstance(entry, dict) and isinstance(entry.get("name"), str):
            declared[entry["name"].casefold()] = entry

    accounted: set[str] = set()
    for name, dep in sorted(measured.items()):
        importers = ", ".join(dep["importedBy"][:3])
        if dep["resolution"] == "unresolved":
            errors.append(
                f"{name!r} (imported by {importers}) is neither in the package nor a "
                f"platformRuntime entry of the dependency authority"
            )
            continue
        if dep["resolution"] == "api-set":
            continue
        if name in first_party:
            if name in declared:
                errors.append(f"{name!r} is declared both as first-party and as a dependency")
                accounted.add(name)
            continue

        # The identity a closure entry has to name, and the source it must carry.
        identity, source = name, platform.get(name)
        if dep["resolution"] == "package":
            reason = os_owned_reason(name, platform)
            if reason is not None:
                errors.append(f"package-local {name!r} {reason}; the package must not ship it")
                continue
            if name not in platform:
                if name not in authority.package_images:
                    errors.append(
                        f"package-local {name!r} (imported by {importers}) is neither a "
                        "first-party image nor a reviewed thirdParty imageNames entry of "
                        "the dependency authority"
                    )
                    continue
                identity, source = authority.package_images[name]
        key = identity.casefold()
        accounted.add(key)
        entry = declared.get(key)
        shipped = "" if identity == name else f" (package-local image of {identity!r})"
        if entry is None:
            errors.append(
                f"measured import {name!r} (imported by {importers}){shipped} is not declared "
                "in the dependency closure"
            )
        elif entry.get("source") != source:
            errors.append(
                f"{identity!r} is declared from {entry.get('source')!r} but the authority "
                f"resolves it from {source!r}"
            )

    for name in sorted(set(declared) - accounted):
        if is_api_set(name):
            errors.append(f"{name!r} is an operating-system API set and must not be declared")
        elif name in authority.package_images:
            errors.append(
                f"declared dependency {name!r} is an image file name; declare the "
                f"authority identity {authority.package_images[name][0]!r} instead"
            )
        else:
            errors.append(f"declared dependency {name!r} is not imported by any image in the package")
    return errors


# ── Graph documents ────────────────────────────────────────────────────────


def _reject_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate key {key!r}")
        result[key] = value
    return result


def _reject_constant(token: str) -> Any:
    raise ValueError(f"non-finite number {token}")


def load_json_bytes(raw: bytes, source: str) -> Any:
    if len(raw) > MAX_DOCUMENT_BYTES:
        raise ClosureError(f"{source}: {len(raw)} bytes exceeds {MAX_DOCUMENT_BYTES}")
    try:
        return json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=_reject_duplicates,
            parse_constant=_reject_constant,
        )
    except (UnicodeDecodeError, ValueError) as exc:
        raise ClosureError(f"{source}: not strict JSON: {exc}") from exc


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ClosureError(message)


def _is_name_list(value: Any) -> bool:
    return (
        isinstance(value, list)
        and len(value) <= MAX_IMPORT_DESCRIPTORS
        and all(isinstance(item, str) and 0 < len(item) <= MAX_DLL_NAME_BYTES + 4 for item in value)
    )


def validate_graph(document: Any) -> dict[str, Any]:
    """Refuse an import graph that is not exactly what walk_package emits."""
    _require(isinstance(document, dict), "import graph must be an object")
    _require(
        set(document) == {"schemaVersion", "kind", "firstPartyImages", "images", "dependencies"},
        "import graph has unexpected or missing fields",
    )
    _require(
        document["schemaVersion"] == GRAPH_SCHEMA_VERSION,
        "import graph schemaVersion is unsupported",
    )
    _require(document["kind"] == GRAPH_KIND, "import graph kind is wrong")
    _require(
        _is_name_list(document["firstPartyImages"]), "import graph firstPartyImages is malformed"
    )

    images = document["images"]
    _require(
        isinstance(images, list) and 0 < len(images) <= MAX_PACKAGE_IMAGES,
        "import graph images is malformed",
    )
    image_keys = {"path", "sha256", "sizeBytes", "machine", "imports", "delayImports"}
    paths: set[str] = set()
    for image in images:
        _require(
            isinstance(image, dict) and set(image) == image_keys,
            "import graph image entry is malformed",
        )
        _require(
            isinstance(image["path"], str) and 0 < len(image["path"]) <= 512,
            "image path is malformed",
        )
        _require(image["path"] not in paths, f"image {image['path']!r} is listed twice")
        paths.add(image["path"])
        _require(
            isinstance(image["sha256"], str) and _SHA256_RE.match(image["sha256"]) is not None,
            f"image {image['path']!r} sha256 is malformed",
        )
        _require(
            isinstance(image["sizeBytes"], int)
            and not isinstance(image["sizeBytes"], bool)
            and 0 < image["sizeBytes"] <= MAX_IMAGE_BYTES,
            f"image {image['path']!r} sizeBytes is malformed",
        )
        _require(image["machine"] == "amd64", f"image {image['path']!r} machine is not amd64")
        _require(
            _is_name_list(image["imports"]) and _is_name_list(image["delayImports"]),
            f"image {image['path']!r} import lists are malformed",
        )

    dependencies = document["dependencies"]
    _require(isinstance(dependencies, list), "import graph dependencies is malformed")
    names: set[str] = set()
    for dep in dependencies:
        _require(isinstance(dep, dict), "import graph dependency entry is malformed")
        resolution = dep.get("resolution")
        _require(resolution in RESOLUTIONS, "import graph dependency resolution is unknown")
        expected = {"name", "importedBy", "resolution"}
        if resolution == "package":
            expected.add("packagePath")
        if resolution == "platform":
            expected.add("source")
        _require(set(dep) == expected, "import graph dependency fields do not match its resolution")
        _require(
            isinstance(dep["name"], str) and dep["name"] not in names,
            "dependency name is malformed or repeated",
        )
        names.add(dep["name"])
        _require(
            isinstance(dep["importedBy"], list)
            and dep["importedBy"]
            and all(path in paths for path in dep["importedBy"]),
            f"dependency {dep['name']!r} names an importer that is not in the graph",
        )
        if resolution == "package":
            _require(
                isinstance(dep["packagePath"], str)
                and dep["packagePath"] in paths
                and "/" not in dep["packagePath"]
                and dep["packagePath"].casefold() == dep["name"],
                f"dependency {dep['name']!r} does not resolve to the package-root image of its name",
            )
        if resolution == "platform":
            _require(
                dep["source"] in da.VALID_SOURCES, f"dependency {dep['name']!r} source is unknown"
            )

    imported = {name for image in images for name in image["imports"] + image["delayImports"]}
    _require(imported == names, "import graph dependencies do not match the images' imports")
    return document


def graph_authority_errors(graph: dict[str, Any], authority: da.Authority) -> list[str]:
    """Re-derive every name-only classification in a graph from the authority.

    Whether a DLL is package-local depends on the package, which the bundle
    does not carry, so that fact is trusted from the attested collector run
    (validate_graph only pins it to a package-root image of the same name).
    Every other resolution follows from the name alone, and so does the rule
    that an OS-owned name is never package-local, so a graph edited to launder
    an import through the wrong class is refused here.
    """
    errors: list[str] = []
    try:
        platform = platform_sources(authority)
    except ClosureError as exc:
        return [str(exc)]
    for dep in graph["dependencies"]:
        name = dep["name"]
        resolution = dep["resolution"]
        if resolution == "package":
            if is_api_set(name) or name in KNOWN_DLLS:
                errors.append(
                    f"import graph resolves {name!r} to the package, but the loader "
                    "never takes that name from the application directory"
                )
            continue
        if is_api_set(name):
            derived, source = "api-set", None
        elif name in platform:
            derived, source = "platform", platform[name]
        else:
            derived, source = "unresolved", None
        if resolution != derived or dep.get("source") != source:
            errors.append(
                f"import graph classifies {name!r} as {resolution!r}, but the dependency "
                f"authority makes it {derived!r}"
            )
    return errors


def load_authority(path: Path) -> da.Authority:
    raw = safe_fs.read_bounded(path, max_bytes=da.MAX_AUTHORITY_BYTES)
    document = load_json_bytes(raw, path.name)
    try:
        da.validate_authority_document(document)
        return da.Authority(document)
    except da.AuthorityError as exc:
        raise ClosureError(f"{path.name}: {exc}") from exc


# ── Command line ───────────────────────────────────────────────────────────


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--package-root", type=Path, required=True)
    parser.add_argument(
        "--plan", type=Path, required=True, help="collector plan declaring the closure"
    )
    parser.add_argument("--authority", type=Path, default=REPO_ROOT / da.AUTHORITY_RELPATH)
    args = parser.parse_args(argv)

    try:
        authority = load_authority(args.authority)
        plan = load_json_bytes(
            safe_fs.read_bounded(args.plan, max_bytes=MAX_DOCUMENT_BYTES), args.plan.name
        )
        if not isinstance(plan, dict):
            raise ClosureError(f"{args.plan.name}: plan must be an object")
        declaration = parse_declaration(plan)
        graph = walk_package(args.package_root, authority, declaration.first_party)
    except (ClosureError, safe_fs.FileSecurityError) as exc:
        print(f"FATAL: {exc}", file=sys.stderr)
        return 2

    sys.stdout.buffer.write(encode_graph(graph))
    sys.stdout.flush()
    errors = closure_errors(graph, declaration.closure, authority)
    for error in errors:
        print(f"ERROR: {error}", file=sys.stderr)
    counts = {
        kind: sum(1 for dep in graph["dependencies"] if dep["resolution"] == kind)
        for kind in RESOLUTIONS
    }
    print(
        f"{len(graph['images'])} image(s); imports: "
        + ", ".join(f"{counts[kind]} {kind}" for kind in RESOLUTIONS),
        file=sys.stderr,
    )
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
