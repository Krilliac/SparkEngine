#!/usr/bin/env python3
"""Map every shipped image to its private symbols by build ID (BLD-100).

A Shipping build keeps private symbols out of the runtime package but must keep
them for crash symbolication. This tool proves that each shipped image has
exactly one symbol file that really belongs to it, and writes a closed JSON
manifest of the mapping:

* ELF images carry a GNU build-id (NT_GNU_BUILD_ID) and a .gnu_debuglink. The
  symbol file is the split ``<image>.debug`` written by
  cmake/SparkSplitDebugLink.cmake. It must have the same build-id, carry DWARF,
  have the file name the debuglink names, and match the debuglink CRC-32.
* PE images carry a CodeView RSDS record in the debug directory: PDB GUID, age
  and PDB file name. The symbol file is the PDB whose info stream has that GUID
  and whose DBI stream has that age. The recorded PDB name must be a bare file
  name (/PDBALTPATH:%_PDB%), never a build-machine path.

The run fails (exit 1), and writes no manifest, when an image has no build ID,
still carries debug information or a symbol table, has no symbol file or more
than one, or maps to a symbol file that does not match; when two different
images share a build ID; when a symbol file sits in an image root outside every
symbol root (symbols leaking into the runtime package); and when a symbol file
maps to no image. Usage errors and unreadable roots exit 2.

Only the Python standard library is used. Structures are read with bounded,
checked offsets; a malformed image or symbol file is an error, never skipped.

Usage:
    shipping_symbol_manifest.py --images <dir> [--images <dir>...]
                                --symbols <dir> [--symbols <dir>...]
                                --output <manifest.json>

Manifest paths are relative to the common parent directory of all roots.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
import uuid
import zlib
from dataclasses import dataclass
from pathlib import Path, PureWindowsPath
from typing import BinaryIO

SCHEMA = "spark.shipping-symbol-manifest/1"

ELF_MAGIC = b"\x7fELF"
PDB_MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\x00\x00\x00"

ET_EXEC = 2
ET_DYN = 3
SHT_NOBITS = 8
SHT_NOTE = 7
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
PT_NOTE = 4
NT_GNU_BUILD_ID = 3

IMAGE_FILE_EXECUTABLE_IMAGE = 0x0002
IMAGE_DEBUG_TYPE_CODEVIEW = 2
IMAGE_DIRECTORY_ENTRY_DEBUG = 6

MIN_BUILD_ID_BYTES = 16
MAX_BUILD_ID_BYTES = 64
MAX_ELF_SECTIONS = 65535
MAX_NOTE_BYTES = 1024 * 1024
MAX_SHSTRTAB_BYTES = 4 * 1024 * 1024
MAX_PE_SECTIONS = 96
MAX_DEBUG_ENTRIES = 64
MAX_CODEVIEW_BYTES = 4096
MAX_PDB_STREAMS = 1 << 20
MAX_PDB_DIRECTORY_BYTES = 64 * 1024 * 1024
PDB_BLOCK_SIZES = (512, 1024, 2048, 4096, 8192, 16384, 32768)
PDB_INFO_STREAM = 1
PDB_DBI_STREAM = 3

ELF_MACHINES = {3: "x86", 62: "x86-64", 183: "arm64"}
PE_MACHINES = {0x14C: "x86", 0x8664: "x86-64", 0xAA64: "arm64"}


class FormatError(Exception):
    """A file claims a known format but its structure is invalid."""


@dataclass(frozen=True)
class ElfFile:
    machine: str
    build_id: str | None
    is_image: bool
    has_debug_info: bool
    has_symtab: bool
    debuglink_name: str | None
    debuglink_crc: int | None


@dataclass(frozen=True)
class PeImage:
    machine: str
    pdb_guid: str
    pdb_age: int
    pdb_path: str


@dataclass(frozen=True)
class PdbFile:
    pdb_guid: str
    pdb_age: int


@dataclass(frozen=True)
class Found:
    path: Path
    relative: str
    in_image_root: bool
    in_symbol_root: bool


def _read(handle: BinaryIO, offset: int, size: int, file_size: int) -> bytes:
    if offset < 0 or size < 0 or offset > file_size or size > file_size - offset:
        raise FormatError("structure points outside the file")
    handle.seek(offset)
    data = handle.read(size)
    if len(data) != size:
        raise FormatError("file is truncated")
    return data


def _file_size(handle: BinaryIO) -> int:
    handle.seek(0, os.SEEK_END)
    return handle.tell()


# ---------------------------------------------------------------------------
# ELF
# ---------------------------------------------------------------------------


def _build_id_from_notes(notes: bytes, endian: str) -> str | None:
    offset = 0
    while offset + 12 <= len(notes):
        name_size, desc_size, note_type = struct.unpack_from(endian + "III", notes, offset)
        name_offset = offset + 12
        desc_offset = name_offset + ((name_size + 3) & ~3)
        if name_size > len(notes) - name_offset or desc_offset > len(notes) or desc_size > len(notes) - desc_offset:
            raise FormatError("malformed ELF note")
        if note_type == NT_GNU_BUILD_ID and notes[name_offset : name_offset + name_size] == b"GNU\x00":
            return notes[desc_offset : desc_offset + desc_size].hex()
        offset = desc_offset + ((desc_size + 3) & ~3)
    return None


def inspect_elf(handle: BinaryIO) -> ElfFile:
    file_size = _file_size(handle)
    ident = _read(handle, 0, 16, file_size)
    if ident[:4] != ELF_MAGIC or ident[4] not in (1, 2) or ident[5] not in (1, 2):
        raise FormatError("unsupported ELF class or byte order")
    is64 = ident[4] == 2
    endian = "<" if ident[5] == 1 else ">"
    if is64:
        header = _read(handle, 0, 64, file_size)
        e_type, e_machine = struct.unpack_from(endian + "HH", header, 16)
        e_phoff, e_shoff = struct.unpack_from(endian + "QQ", header, 32)
        e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(endian + "HHHHH", header, 54)
    else:
        header = _read(handle, 0, 52, file_size)
        e_type, e_machine = struct.unpack_from(endian + "HH", header, 16)
        e_phoff, e_shoff = struct.unpack_from(endian + "II", header, 28)
        e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(endian + "HHHHH", header, 42)

    if e_type not in (ET_EXEC, ET_DYN):
        return ElfFile(ELF_MACHINES.get(e_machine, f"elf-{e_machine}"), None, False, False, False, None, None)
    if not e_shoff or not e_shnum:
        raise FormatError("ELF file has no section header table")
    if e_shentsize != (64 if is64 else 40) or e_shnum > MAX_ELF_SECTIONS or e_shstrndx >= e_shnum:
        raise FormatError("unsupported ELF section header table")

    table = _read(handle, e_shoff, e_shnum * e_shentsize, file_size)
    sections = []
    for index in range(e_shnum):
        base = index * e_shentsize
        if is64:
            name, sh_type, sh_flags, _, sh_offset, sh_size = struct.unpack_from(endian + "IIQQQQ", table, base)
        else:
            name, sh_type, sh_flags, _, sh_offset, sh_size = struct.unpack_from(endian + "IIIIII", table, base)
        sections.append((name, sh_type, sh_flags, sh_offset, sh_size))

    _, strtab_type, _, strtab_offset, strtab_size = sections[e_shstrndx]
    if strtab_type == SHT_NOBITS or strtab_size > MAX_SHSTRTAB_BYTES:
        raise FormatError("unsupported ELF section name table")
    names = _read(handle, strtab_offset, strtab_size, file_size)

    build_id = None
    is_image = False
    has_debug_info = False
    has_symtab = False
    debuglink_name = None
    debuglink_crc = None
    for name_offset, sh_type, sh_flags, sh_offset, sh_size in sections:
        end = names.find(b"\x00", name_offset) if name_offset < len(names) else -1
        section_name = names[name_offset:end] if end >= 0 else b""
        present = sh_type != SHT_NOBITS and sh_size > 0
        # A split debug file keeps the section headers of the image but turns
        # every loadable section into NOBITS, so real code marks a real image.
        if present and sh_flags & SHF_ALLOC and sh_flags & SHF_EXECINSTR:
            is_image = True
        if section_name in (b".debug_info", b".zdebug_info") and present:
            has_debug_info = True
        if section_name == b".symtab" and present:
            has_symtab = True
        if section_name == b".gnu_debuglink" and present:
            if sh_size > 4096:
                raise FormatError("oversized .gnu_debuglink")
            link = _read(handle, sh_offset, sh_size, file_size)
            end_of_name = link.find(b"\x00")
            crc_offset = (end_of_name + 4) & ~3
            if end_of_name <= 0 or crc_offset + 4 > len(link):
                raise FormatError("malformed .gnu_debuglink")
            debuglink_name = link[:end_of_name].decode("utf-8", "strict")
            (debuglink_crc,) = struct.unpack_from(endian + "I", link, crc_offset)
        if sh_type == SHT_NOTE and build_id is None and present and sh_size <= MAX_NOTE_BYTES:
            build_id = _build_id_from_notes(_read(handle, sh_offset, sh_size, file_size), endian)

    if build_id is None and e_phoff and e_phnum:
        if e_phentsize != (56 if is64 else 32):
            raise FormatError("unsupported ELF program header table")
        table = _read(handle, e_phoff, e_phnum * e_phentsize, file_size)
        for index in range(e_phnum):
            base = index * e_phentsize
            if is64:
                p_type, _, p_offset, _, _, p_filesz = struct.unpack_from(endian + "IIQQQQ", table, base)
            else:
                p_type, p_offset, _, _, p_filesz = struct.unpack_from(endian + "IIIII", table, base)
            if p_type == PT_NOTE and 0 < p_filesz <= MAX_NOTE_BYTES and p_offset + p_filesz <= file_size:
                build_id = _build_id_from_notes(_read(handle, p_offset, p_filesz, file_size), endian)
                if build_id is not None:
                    break

    return ElfFile(
        machine=ELF_MACHINES.get(e_machine, f"elf-{e_machine}"),
        build_id=build_id,
        is_image=is_image,
        has_debug_info=has_debug_info,
        has_symtab=has_symtab,
        debuglink_name=debuglink_name,
        debuglink_crc=debuglink_crc,
    )


# ---------------------------------------------------------------------------
# PE / PDB
# ---------------------------------------------------------------------------


def _guid_text(raw: bytes) -> str:
    return str(uuid.UUID(bytes_le=raw)).upper()


def inspect_pe(handle: BinaryIO) -> PeImage | None:
    """Return the CodeView record of a PE image, or None for a non-image PE (object, resource DLL stub)."""
    file_size = _file_size(handle)
    dos = _read(handle, 0, 64, file_size)
    (pe_offset,) = struct.unpack_from("<I", dos, 0x3C)
    coff = _read(handle, pe_offset, 24, file_size)
    if coff[:4] != b"PE\x00\x00":
        raise FormatError("MZ file without a PE signature")
    machine, section_count, _, _, _, optional_size, characteristics = struct.unpack_from("<HHIIIHH", coff, 4)
    if not characteristics & IMAGE_FILE_EXECUTABLE_IMAGE:
        return None
    if section_count > MAX_PE_SECTIONS:
        raise FormatError("too many PE sections")
    optional = _read(handle, pe_offset + 24, optional_size, file_size)
    if len(optional) < 2:
        raise FormatError("missing PE optional header")
    (magic,) = struct.unpack_from("<H", optional, 0)
    if magic == 0x20B:
        directory_offset = 112
    elif magic == 0x10B:
        directory_offset = 96
    else:
        raise FormatError("unknown PE optional header magic")
    (directory_count,) = struct.unpack_from("<I", optional, directory_offset - 4)
    if directory_count <= IMAGE_DIRECTORY_ENTRY_DEBUG or directory_offset + 8 * (IMAGE_DIRECTORY_ENTRY_DEBUG + 1) > len(
        optional
    ):
        raise FormatError("PE image has no debug directory entry")
    debug_rva, debug_size = struct.unpack_from("<II", optional, directory_offset + 8 * IMAGE_DIRECTORY_ENTRY_DEBUG)

    section_table = _read(handle, pe_offset + 24 + optional_size, 40 * section_count, file_size)
    sections = [struct.unpack_from("<IIII", section_table, 40 * index + 8) for index in range(section_count)]

    def rva_to_offset(rva: int, size: int) -> int:
        for virtual_size, virtual_address, raw_size, raw_pointer in sections:
            span = max(virtual_size, raw_size)
            if virtual_address <= rva and rva + size <= virtual_address + span:
                if rva + size > virtual_address + raw_size:
                    raise FormatError("PE debug data lies outside its section's file data")
                return raw_pointer + (rva - virtual_address)
        raise FormatError("PE debug RVA maps to no section")

    if debug_rva == 0 or debug_size == 0:
        raise FormatError("PE image has an empty debug directory")
    if debug_size % 28 or debug_size // 28 > MAX_DEBUG_ENTRIES:
        raise FormatError("malformed PE debug directory")
    entries = _read(handle, rva_to_offset(debug_rva, debug_size), debug_size, file_size)
    records = []
    for index in range(debug_size // 28):
        _, _, _, _, debug_type, data_size, _, data_pointer = struct.unpack_from("<IIHHIIII", entries, 28 * index)
        if debug_type != IMAGE_DEBUG_TYPE_CODEVIEW:
            continue
        if data_size < 24 or data_size > MAX_CODEVIEW_BYTES:
            raise FormatError("malformed CodeView record")
        records.append(_read(handle, data_pointer, data_size, file_size))
    if len(records) != 1:
        raise FormatError(f"PE image has {len(records)} CodeView records, expected exactly 1")
    record = records[0]
    if record[:4] != b"RSDS":
        raise FormatError("CodeView record is not RSDS (PDB 7.0)")
    (age,) = struct.unpack_from("<I", record, 20)
    end = record.find(b"\x00", 24)
    if end < 0:
        raise FormatError("unterminated PDB path in CodeView record")
    return PeImage(
        machine=PE_MACHINES.get(machine, f"pe-{machine:#x}"),
        pdb_guid=_guid_text(record[4:20]),
        pdb_age=age,
        pdb_path=record[24:end].decode("utf-8", "strict"),
    )


def inspect_pdb(handle: BinaryIO) -> PdbFile:
    """Read the GUID (PDB info stream) and age (DBI stream) that a debugger matches against RSDS."""
    file_size = _file_size(handle)
    superblock = _read(handle, 0, len(PDB_MAGIC) + 24, file_size)
    block_size, _, block_count, directory_bytes, _, block_map_address = struct.unpack_from(
        "<IIIIII", superblock, len(PDB_MAGIC)
    )
    if block_size not in PDB_BLOCK_SIZES or block_count * block_size > file_size:
        raise FormatError("invalid MSF superblock")
    if directory_bytes == 0 or directory_bytes > MAX_PDB_DIRECTORY_BYTES:
        raise FormatError("invalid MSF stream directory size")

    def block(index: int) -> bytes:
        if index >= block_count:
            raise FormatError("MSF block index out of range")
        return _read(handle, index * block_size, block_size, file_size)

    def blocks_for(size: int) -> int:
        return (size + block_size - 1) // block_size

    directory_block_count = blocks_for(directory_bytes)
    if directory_block_count * 4 > block_size:
        raise FormatError("MSF stream directory does not fit one block map")
    directory_blocks = struct.unpack_from(f"<{directory_block_count}I", block(block_map_address), 0)
    directory = b"".join(block(index) for index in directory_blocks)[:directory_bytes]

    (stream_count,) = struct.unpack_from("<I", directory, 0)
    if stream_count > MAX_PDB_STREAMS or 4 + 4 * stream_count > len(directory):
        raise FormatError("invalid MSF stream count")
    sizes = [0 if size == 0xFFFFFFFF else size for size in struct.unpack_from(f"<{stream_count}I", directory, 4)]
    cursor = 4 + 4 * stream_count
    stream_blocks = []
    for size in sizes:
        count = blocks_for(size)
        if cursor + 4 * count > len(directory):
            raise FormatError("truncated MSF stream directory")
        stream_blocks.append(struct.unpack_from(f"<{count}I", directory, cursor))
        cursor += 4 * count

    def stream_prefix(index: int, size: int) -> bytes:
        if index >= stream_count or sizes[index] < size:
            raise FormatError(f"PDB stream {index} is missing or too short")
        data = b"".join(block(block_index) for block_index in stream_blocks[index][: blocks_for(size)])
        return data[:size]

    info = stream_prefix(PDB_INFO_STREAM, 28)
    dbi = stream_prefix(PDB_DBI_STREAM, 12)
    (dbi_signature,) = struct.unpack_from("<i", dbi, 0)
    if dbi_signature != -1:
        raise FormatError("invalid DBI stream header")
    (dbi_age,) = struct.unpack_from("<I", dbi, 8)
    return PdbFile(pdb_guid=_guid_text(info[12:28]), pdb_age=dbi_age)


# ---------------------------------------------------------------------------
# Scanning and mapping
# ---------------------------------------------------------------------------


def _is_within(path: Path, root: Path) -> bool:
    return path == root or root in path.parents


def discover(image_roots: list[Path], symbol_roots: list[Path]) -> list[Found]:
    """List every regular file under the roots once; symlinks are not artifacts and are skipped."""
    # Paths are recorded relative to the roots' common parent, so equal file
    # names under different roots (bin/x and lib/x) stay distinct.
    base = Path(os.path.commonpath(image_roots + symbol_roots))
    found: dict[Path, Found] = {}
    for root in image_roots + symbol_roots:
        for directory, subdirectories, files in os.walk(root, followlinks=False):
            subdirectories.sort()
            for name in sorted(files):
                path = Path(directory) / name
                if path in found or path.is_symlink() or not path.is_file():
                    continue
                in_images = any(_is_within(path, image_root) for image_root in image_roots)
                in_symbols = any(_is_within(path, symbol_root) for symbol_root in symbol_roots)
                found[path] = Found(path, path.relative_to(base).as_posix(), in_images, in_symbols)
    return sorted(found.values(), key=lambda entry: entry.relative)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _crc32(path: Path) -> int:
    crc = 0
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF


def _pdb_file_name(pdb_path: str) -> str:
    return PureWindowsPath(pdb_path).name if "\\" in pdb_path or ":" in pdb_path else Path(pdb_path).name


def build_manifest(image_roots: list[Path], symbol_roots: list[Path]) -> tuple[dict | None, list[str]]:
    entries = discover(image_roots, symbol_roots)
    errors: list[str] = []

    elf_images: list[tuple[Found, ElfFile]] = []
    pe_images: list[tuple[Found, PeImage]] = []
    elf_symbols: dict[str, list[tuple[Found, ElfFile]]] = {}
    pdb_symbols: dict[tuple[str, int], list[Found]] = {}

    for entry in entries:
        try:
            with entry.path.open("rb") as handle:
                head = handle.read(len(PDB_MAGIC))
                handle.seek(0)
                if head.startswith(ELF_MAGIC):
                    elf = inspect_elf(handle)
                    if elf.is_image and entry.in_image_root:
                        elf_images.append((entry, elf))
                    elif not elf.is_image and elf.build_id is not None:
                        if not entry.in_symbol_root:
                            errors.append(f"{entry.relative}: private symbols in the runtime tree")
                        elif not elf.has_debug_info:
                            errors.append(f"{entry.relative}: symbol file has no DWARF debug info")
                        else:
                            elf_symbols.setdefault(elf.build_id, []).append((entry, elf))
                elif head.startswith(b"MZ"):
                    pe = inspect_pe(handle)
                    if pe is not None and entry.in_image_root:
                        pe_images.append((entry, pe))
                elif head == PDB_MAGIC:
                    if not entry.in_symbol_root:
                        errors.append(f"{entry.relative}: private symbols in the runtime tree")
                    else:
                        pdb = inspect_pdb(handle)
                        pdb_symbols.setdefault((pdb.pdb_guid, pdb.pdb_age), []).append(entry)
        except (FormatError, OSError, struct.error, UnicodeDecodeError) as error:
            errors.append(f"{entry.relative}: {error}")

    images: list[dict] = []
    used_symbols: set[Path] = set()
    image_hash_by_key: dict[str, tuple[str, str]] = {}

    def claim(key: str, entry: Found) -> str | None:
        digest = _sha256(entry.path)
        previous = image_hash_by_key.setdefault(key, (digest, entry.relative))
        if previous[0] != digest:
            errors.append(f"{entry.relative}: build ID {key} is shared with different image {previous[1]}")
            return None
        return digest

    for entry, elf in elf_images:
        if elf.build_id is None:
            errors.append(f"{entry.relative}: ELF image has no GNU build-id")
            continue
        if not MIN_BUILD_ID_BYTES * 2 <= len(elf.build_id) <= MAX_BUILD_ID_BYTES * 2:
            errors.append(f"{entry.relative}: GNU build-id {elf.build_id} is not {MIN_BUILD_ID_BYTES}-64 bytes")
            continue
        if elf.has_debug_info or elf.has_symtab:
            errors.append(f"{entry.relative}: runtime image still carries debug info or a symbol table")
            continue
        candidates = elf_symbols.get(elf.build_id, [])
        if len(candidates) != 1:
            errors.append(f"{entry.relative}: {len(candidates)} symbol files for build-id {elf.build_id}, expected 1")
            continue
        symbol, symbol_elf = candidates[0]
        used_symbols.add(symbol.path)
        if symbol_elf.machine != elf.machine:
            errors.append(f"{entry.relative}: symbol file {symbol.relative} is {symbol_elf.machine}, not {elf.machine}")
            continue
        if elf.debuglink_name is None or elf.debuglink_crc is None:
            errors.append(f"{entry.relative}: ELF image has no .gnu_debuglink")
            continue
        if elf.debuglink_name != symbol.path.name:
            errors.append(
                f"{entry.relative}: .gnu_debuglink names {elf.debuglink_name}, symbol file is {symbol.relative}"
            )
            continue
        if _crc32(symbol.path) != elf.debuglink_crc:
            errors.append(f"{entry.relative}: {symbol.relative} does not match the .gnu_debuglink CRC-32")
            continue
        digest = claim(elf.build_id, entry)
        if digest is None:
            continue
        images.append(
            {
                "path": entry.relative,
                "format": "elf",
                "machine": elf.machine,
                "sha256": digest,
                "symbolKey": elf.build_id,
                "buildId": elf.build_id,
                "debugLinkCrc32": f"{elf.debuglink_crc:08x}",
                "symbols": {"path": symbol.relative, "sha256": _sha256(symbol.path)},
            }
        )

    for entry, pe in pe_images:
        candidates = pdb_symbols.get((pe.pdb_guid, pe.pdb_age), [])
        if len(candidates) != 1:
            errors.append(
                f"{entry.relative}: {len(candidates)} PDBs for GUID {pe.pdb_guid} age {pe.pdb_age}, expected 1"
            )
            continue
        symbol = candidates[0]
        used_symbols.add(symbol.path)
        pdb_name = _pdb_file_name(pe.pdb_path)
        if pe.pdb_path != pdb_name:
            errors.append(f"{entry.relative}: CodeView PDB path {pe.pdb_path!r} is not a bare file name")
            continue
        if symbol.path.name.lower() != pdb_name.lower():
            errors.append(f"{entry.relative}: image names {pdb_name}, matching PDB is {symbol.relative}")
            continue
        key = f"{pe.pdb_guid.replace('-', '')}{pe.pdb_age:X}"
        digest = claim(key, entry)
        if digest is None:
            continue
        images.append(
            {
                "path": entry.relative,
                "format": "pe",
                "machine": pe.machine,
                "sha256": digest,
                "symbolKey": key,
                "pdbGuid": pe.pdb_guid,
                "pdbAge": pe.pdb_age,
                "pdbName": pdb_name,
                "symbols": {"path": symbol.relative, "sha256": _sha256(symbol.path)},
            }
        )

    for candidates in elf_symbols.values():
        for symbol, _ in candidates:
            if symbol.path not in used_symbols:
                errors.append(f"{symbol.relative}: symbol file maps to no image")
    for candidates in pdb_symbols.values():
        for symbol in candidates:
            if symbol.path not in used_symbols:
                errors.append(f"{symbol.relative}: symbol file maps to no image")

    if not elf_images and not pe_images:
        errors.append("no ELF or PE images found under the image roots")
    if errors:
        return None, errors
    manifest = {"schema": SCHEMA, "images": sorted(images, key=lambda image: image["path"])}
    validate_manifest(manifest)
    return manifest, []


IMAGE_KEYS = {
    "elf": {"path", "format", "machine", "sha256", "symbolKey", "buildId", "debugLinkCrc32", "symbols"},
    "pe": {"path", "format", "machine", "sha256", "symbolKey", "pdbGuid", "pdbAge", "pdbName", "symbols"},
}


def validate_manifest(manifest: object) -> None:
    """Reject any manifest that is not exactly the closed schema this tool writes."""
    if not isinstance(manifest, dict) or set(manifest) != {"schema", "images"}:
        raise ValueError("manifest must have exactly the keys schema and images")
    if manifest["schema"] != SCHEMA:
        raise ValueError(f"manifest schema must be {SCHEMA}")
    images = manifest["images"]
    if not isinstance(images, list) or not images:
        raise ValueError("manifest images must be a non-empty list")
    paths = [image.get("path") if isinstance(image, dict) else None for image in images]
    if paths != sorted(set(paths), key=str) or None in paths:
        raise ValueError("manifest image paths must be unique and sorted")
    for image in images:
        keys = IMAGE_KEYS.get(image.get("format"))
        if keys is None or set(image) != keys:
            raise ValueError(f"image {image.get('path')!r} does not match the {image.get('format')!r} schema")
        if not isinstance(image["symbols"], dict) or set(image["symbols"]) != {"path", "sha256"}:
            raise ValueError(f"image {image['path']!r} symbols must have exactly path and sha256")
        for field in ("path", "machine", "sha256", "symbolKey"):
            if not isinstance(image[field], str) or not image[field]:
                raise ValueError(f"image {image['path']!r} field {field} must be a non-empty string")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--images", type=Path, action="append", required=True, help="runtime image root")
    parser.add_argument("--symbols", type=Path, action="append", required=True, help="private symbol root")
    parser.add_argument("--output", type=Path, required=True, help="manifest JSON to write")
    args = parser.parse_args(argv)

    roots = [root.resolve() for root in args.images + args.symbols]
    missing = [str(root) for root in roots if not root.is_dir()]
    if missing:
        print(f"error: not a directory: {', '.join(missing)}", file=sys.stderr)
        return 2

    image_roots = [root.resolve() for root in args.images]
    symbol_roots = [root.resolve() for root in args.symbols]
    manifest, errors = build_manifest(image_roots, symbol_roots)
    if manifest is None:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        print(f"symbol manifest FAILED: {len(errors)} error(s)", file=sys.stderr)
        return 1

    args.output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"symbol manifest OK: {len(manifest['images'])} image(s) mapped -> {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
