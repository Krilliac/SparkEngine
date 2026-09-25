#!/usr/bin/env python3
"""Build-id-indexed, fail-closed symbolication for SparkEngine Linux crash logs.

The Linux crash handler writes a ``*** SYMBOLIC FRAMES ***`` section into every
crash log (see SparkEngine/Source/Utils/CrashSymbolication.h): one ``MODULE``
record per module that holds a frame (GNU build-id, load bias, basename) and
one ``SYMFRAME`` record per frame with a module-relative offset. This tool
resolves those frames offline against a local symbol store laid out like the
debuginfod / GDB convention::

    <store>/.build-id/<first 2 hex digits>/<remaining hex digits>.debug

Two subcommands:

``store``    split each binary's debug info (``objcopy --only-keep-debug``) and
             file it under its build-id. A binary without a build-id or without
             DWARF debug info is refused, so a stripped Release binary is never
             silently published as "symbols".
``resolve``  parse one crash log and resolve every frame with ``addr2line``.

Fail-closed rules: the log and the section are size- and count-bounded and
every record must match an exact grammar; store paths are opened without
following symlinks and must stay regular files inside the store; a debug file
whose own build-id differs from the one the crash recorded is refused (exit 2)
instead of producing plausible-looking but wrong source locations. A module
with no debug file in the store is reported as ``missing-symbols`` (a normal
state for system libraries), never guessed.

There is no relay or upload here: this proves offline symbolication only.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import stat
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import BinaryIO

MAX_LOG_BYTES = 8 * 1024 * 1024
MAX_MODULES = 512
MAX_FRAMES = 256
MIN_BUILD_ID_BYTES = 2
MAX_BUILD_ID_BYTES = 64
MAX_ELF_SECTIONS = 65535
MAX_NOTE_BYTES = 1024 * 1024
MAX_SHSTRTAB_BYTES = 4 * 1024 * 1024
MAX_ADDR2LINE_OUTPUT_BYTES = 4 * 1024 * 1024
TOOL_TIMEOUT_SECONDS = 120

SECTION_BEGIN = "*** SYMBOLIC FRAMES ***"
SECTION_END = "*** END SYMBOLIC FRAMES ***"

MODULE_RE = re.compile(
    r"MODULE (?P<index>\d{1,4}) build_id=(?P<build_id>[0-9a-f]+) "
    r"bias=0x(?P<bias>[0-9a-f]{1,16}) name=(?P<name>[A-Za-z0-9._+-]{1,127})"
)
FRAME_IN_MODULE_RE = re.compile(
    r"SYMFRAME (?P<index>\d{1,3}) kind=(?P<kind>pc|ra) module=(?P<module>\d{1,4}) "
    r"offset=0x(?P<offset>[0-9a-f]{1,16})"
)
FRAME_UNMAPPED_RE = re.compile(
    r"SYMFRAME (?P<index>\d{1,3}) kind=(?P<kind>pc|ra) module=- address=0x(?P<address>[0-9a-f]{1,16})"
)

SHT_NOTE = 7
SHT_NOBITS = 8
PT_NOTE = 4
NT_GNU_BUILD_ID = 3


class SymbolicationError(Exception):
    """Input or symbol store violates the fail-closed policy."""


@dataclass(frozen=True)
class CrashModule:
    index: int
    build_id: str
    bias: int
    name: str


@dataclass(frozen=True)
class CrashFrame:
    index: int
    kind: str
    module: int | None
    offset: int | None
    address: int | None


@dataclass
class SymbolicSection:
    modules: dict[int, CrashModule] = field(default_factory=dict)
    frames: list[CrashFrame] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Crash-log parsing
# ---------------------------------------------------------------------------


def validate_build_id(build_id: str) -> str:
    if not re.fullmatch(r"[0-9a-f]+", build_id):
        raise SymbolicationError(f"build-id '{build_id[:32]}' is not lowercase hexadecimal")
    if len(build_id) % 2 != 0:
        raise SymbolicationError(f"build-id '{build_id[:32]}' has an odd number of hex digits")
    if not MIN_BUILD_ID_BYTES * 2 <= len(build_id) <= MAX_BUILD_ID_BYTES * 2:
        raise SymbolicationError(f"build-id length {len(build_id) // 2} bytes is outside the accepted range")
    return build_id


def parse_symbolic_section(text: str) -> SymbolicSection:
    """Parse exactly one bounded symbolic-frame section from a crash log."""
    lines = text.splitlines()
    begins = [number for number, line in enumerate(lines) if line == SECTION_BEGIN]
    if not begins:
        raise SymbolicationError("crash log has no symbolic-frame section (not a build-id-capable Linux report)")
    if len(begins) != 1:
        raise SymbolicationError("crash log has more than one symbolic-frame section")
    begin = begins[0]
    try:
        end = lines.index(SECTION_END, begin + 1)
    except ValueError as exc:
        raise SymbolicationError("symbolic-frame section is truncated (no end marker)") from exc

    section = SymbolicSection()
    for line in lines[begin + 1 : end]:
        if match := MODULE_RE.fullmatch(line):
            if section.frames:
                raise SymbolicationError("MODULE record appears after a SYMFRAME record")
            index = int(match["index"])
            if index in section.modules:
                raise SymbolicationError(f"duplicate MODULE index {index}")
            if len(section.modules) >= MAX_MODULES:
                raise SymbolicationError(f"more than {MAX_MODULES} MODULE records")
            section.modules[index] = CrashModule(
                index=index,
                build_id=validate_build_id(match["build_id"]),
                bias=int(match["bias"], 16),
                name=match["name"],
            )
            continue

        in_module = FRAME_IN_MODULE_RE.fullmatch(line)
        unmapped = None if in_module else FRAME_UNMAPPED_RE.fullmatch(line)
        match = in_module or unmapped
        if not match:
            raise SymbolicationError(f"malformed symbolic-frame record: {line[:120]!r}")
        index = int(match["index"])
        if index != len(section.frames):
            raise SymbolicationError(f"SYMFRAME {index} is out of sequence (expected {len(section.frames)})")
        if len(section.frames) >= MAX_FRAMES:
            raise SymbolicationError(f"more than {MAX_FRAMES} SYMFRAME records")
        if match["kind"] == "pc" and index != 0:
            raise SymbolicationError(f"SYMFRAME {index}: only frame 0 may be an exact pc")
        if in_module:
            module = int(in_module["module"])
            if module not in section.modules:
                raise SymbolicationError(f"SYMFRAME {index} references undeclared MODULE {module}")
            offset = int(in_module["offset"], 16)
            if in_module["kind"] == "ra" and offset == 0:
                raise SymbolicationError(f"SYMFRAME {index}: a return address cannot be at offset 0")
            section.frames.append(CrashFrame(index, in_module["kind"], module, offset, None))
        else:
            section.frames.append(CrashFrame(index, match["kind"], None, None, int(match["address"], 16)))

    if not section.frames:
        raise SymbolicationError("symbolic-frame section has no frames")
    referenced = {frame.module for frame in section.frames if frame.module is not None}
    unreferenced = sorted(set(section.modules) - referenced)
    if unreferenced:
        raise SymbolicationError(f"MODULE records {unreferenced} are not referenced by any frame")
    return section


def read_crash_log(path: Path) -> str:
    try:
        with open(path, "rb") as handle:
            data = handle.read(MAX_LOG_BYTES + 1)
    except OSError as exc:
        raise SymbolicationError(f"cannot read crash log {path}: {exc.strerror}") from exc
    if len(data) > MAX_LOG_BYTES:
        raise SymbolicationError(f"crash log exceeds {MAX_LOG_BYTES} bytes")
    # Free-form log text (assertion messages) may hold arbitrary bytes; only
    # the section grammar above is trusted, and it is pure ASCII.
    return data.decode("utf-8", errors="replace")


# ---------------------------------------------------------------------------
# ELF inspection (bounded, no third-party parser)
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class ElfInfo:
    build_id: str | None
    has_debug_info: bool


def _read_exact(handle: BinaryIO, offset: int, size: int, file_size: int) -> bytes:
    if offset < 0 or size < 0 or offset > file_size or size > file_size - offset:
        raise SymbolicationError("ELF structure points outside the file")
    handle.seek(offset)
    data = handle.read(size)
    if len(data) != size:
        raise SymbolicationError("ELF file is truncated")
    return data


def _build_id_from_notes(notes: bytes, endian: str) -> str | None:
    offset = 0
    while offset + 12 <= len(notes):
        name_size, desc_size, note_type = struct.unpack_from(endian + "III", notes, offset)
        name_offset = offset + 12
        desc_offset = name_offset + ((name_size + 3) & ~3)
        if name_size > len(notes) - name_offset or desc_offset > len(notes) or desc_size > len(notes) - desc_offset:
            return None
        if note_type == NT_GNU_BUILD_ID and notes[name_offset : name_offset + name_size] == b"GNU\x00":
            return notes[desc_offset : desc_offset + desc_size].hex()
        next_offset = desc_offset + ((desc_size + 3) & ~3)
        if next_offset <= offset:
            return None
        offset = next_offset
    return None


def inspect_elf(handle: BinaryIO) -> ElfInfo:
    """Return the GNU build-id and whether DWARF is present, reading bounded structures only."""
    handle.seek(0, os.SEEK_END)
    file_size = handle.tell()
    ident = _read_exact(handle, 0, 16, file_size)
    if ident[:4] != b"\x7fELF":
        raise SymbolicationError("not an ELF file")
    if ident[4] not in (1, 2) or ident[5] not in (1, 2):
        raise SymbolicationError("unsupported ELF class or byte order")
    is64 = ident[4] == 2
    endian = "<" if ident[5] == 1 else ">"

    if is64:
        header = _read_exact(handle, 0, 64, file_size)
        (e_phoff, e_shoff) = struct.unpack_from(endian + "QQ", header, 32)
        (e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from(endian + "HHHHH", header, 54)
    else:
        header = _read_exact(handle, 0, 52, file_size)
        (e_phoff, e_shoff) = struct.unpack_from(endian + "II", header, 28)
        (e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from(endian + "HHHHH", header, 42)

    build_id: str | None = None
    has_debug_info = False

    expected_shentsize = 64 if is64 else 40
    if e_shoff and e_shnum:
        if e_shentsize != expected_shentsize or e_shnum > MAX_ELF_SECTIONS:
            raise SymbolicationError("unsupported ELF section header table")
        table = _read_exact(handle, e_shoff, e_shnum * e_shentsize, file_size)
        sections = []
        for index in range(e_shnum):
            base = index * e_shentsize
            if is64:
                name, sh_type, _, _, sh_offset, sh_size = struct.unpack_from(endian + "IIQQQQ", table, base)
            else:
                name, sh_type, _, _, sh_offset, sh_size = struct.unpack_from(endian + "IIIIII", table, base)
            sections.append((name, sh_type, sh_offset, sh_size))

        names = b""
        if e_shstrndx < len(sections):
            _, strtab_type, strtab_offset, strtab_size = sections[e_shstrndx]
            if strtab_type != SHT_NOBITS and strtab_size <= MAX_SHSTRTAB_BYTES:
                names = _read_exact(handle, strtab_offset, strtab_size, file_size)

        for name_offset, sh_type, sh_offset, sh_size in sections:
            terminator = names.find(b"\x00", name_offset) if name_offset < len(names) else -1
            section_name = names[name_offset:terminator] if terminator >= 0 else b""
            if section_name in (b".debug_info", b".zdebug_info") and sh_type != SHT_NOBITS and sh_size > 0:
                has_debug_info = True
            if sh_type == SHT_NOTE and build_id is None and 0 < sh_size <= MAX_NOTE_BYTES:
                build_id = _build_id_from_notes(_read_exact(handle, sh_offset, sh_size, file_size), endian)

    expected_phentsize = 56 if is64 else 32
    if build_id is None and e_phoff and e_phnum:
        if e_phentsize != expected_phentsize:
            raise SymbolicationError("unsupported ELF program header table")
        table = _read_exact(handle, e_phoff, e_phnum * e_phentsize, file_size)
        for index in range(e_phnum):
            base = index * e_phentsize
            if is64:
                p_type, _, p_offset, _, _, p_filesz = struct.unpack_from(endian + "IIQQQQ", table, base)
            else:
                p_type, p_offset, _, _, p_filesz = struct.unpack_from(endian + "IIIII", table, base)
            if p_type == PT_NOTE and 0 < p_filesz <= MAX_NOTE_BYTES:
                build_id = _build_id_from_notes(_read_exact(handle, p_offset, p_filesz, file_size), endian)
                if build_id is not None:
                    break

    return ElfInfo(build_id=build_id, has_debug_info=has_debug_info)


# ---------------------------------------------------------------------------
# Symbol store
# ---------------------------------------------------------------------------


def open_store_root(store: Path) -> Path:
    try:
        info = os.lstat(store)
    except OSError as exc:
        raise SymbolicationError(f"symbol store {store} is not accessible: {exc.strerror}") from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
        raise SymbolicationError(f"symbol store {store} must be a real directory, not a symlink")
    return store.resolve(strict=True)


def debug_relative_path(build_id: str) -> Path:
    validate_build_id(build_id)
    return Path(".build-id") / build_id[:2] / f"{build_id[2:]}.debug"


def _require_real_directory(path: Path) -> bool:
    """True when @p path is a real directory, False when absent; symlinks and other types are refused."""
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        return False
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
        raise SymbolicationError(f"symbol store path {path} is not a real directory")
    return True


def open_debug_file(root: Path, build_id: str) -> int | None:
    """Open the store entry for @p build_id without following symlinks; None when absent."""
    relative = debug_relative_path(build_id)
    directory = root
    for part in relative.parts[:-1]:
        directory = directory / part
        if not _require_real_directory(directory):
            return None
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0)
    try:
        descriptor = os.open(root / relative, flags)
    except FileNotFoundError:
        return None
    except OSError as exc:
        raise SymbolicationError(f"symbol store entry {relative} refused: {exc.strerror}") from exc
    if not stat.S_ISREG(os.fstat(descriptor).st_mode):
        os.close(descriptor)
        raise SymbolicationError(f"symbol store entry {relative} is not a regular file")
    return descriptor


def store_binary(root: Path, binary: Path, objcopy: str) -> Path:
    """Split @p binary's debug info into the store under its build-id; returns the store entry."""
    with open(binary, "rb") as handle:
        info = inspect_elf(handle)
    if info.build_id is None:
        raise SymbolicationError(f"{binary} has no GNU build-id (link with -Wl,--build-id)")
    if not info.has_debug_info:
        raise SymbolicationError(f"{binary} has no DWARF debug info (compile with -g)")
    relative = debug_relative_path(info.build_id)

    directory = root
    for part in relative.parts[:-1]:
        directory = directory / part
        if not _require_real_directory(directory):
            os.mkdir(directory, 0o755)
            _require_real_directory(directory)

    final = root / relative
    descriptor, temporary = tempfile.mkstemp(prefix=".split-", suffix=".debug", dir=directory)
    os.close(descriptor)
    try:
        subprocess.run(
            [objcopy, "--only-keep-debug", str(binary), temporary],
            check=True,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            timeout=TOOL_TIMEOUT_SECONDS,
        )
        with open(temporary, "rb") as handle:
            split = inspect_elf(handle)
        if split.build_id != info.build_id or not split.has_debug_info:
            raise SymbolicationError(f"split debug file for {binary} lost its build-id or DWARF sections")
        try:
            os.link(temporary, final)  # Never overwrites an existing entry.
        except FileExistsError:
            existing = open_debug_file(root, info.build_id)
            if existing is None:
                raise SymbolicationError(f"symbol store entry {relative} vanished during publication") from None
            with os.fdopen(existing, "rb") as handle:
                if inspect_elf(handle).build_id != info.build_id:
                    raise SymbolicationError(f"existing symbol store entry {relative} has a different build-id")
    except subprocess.CalledProcessError as exc:
        detail = exc.stderr.decode("utf-8", errors="replace").strip()[:400]
        raise SymbolicationError(f"objcopy failed for {binary}: {detail}") from exc
    finally:
        os.unlink(temporary)
    return final


# ---------------------------------------------------------------------------
# Resolution
# ---------------------------------------------------------------------------


def run_addr2line(addr2line: str, descriptor: int, addresses: list[int]) -> list[tuple[str, str]]:
    """Return (function, file:line) for each address, reading the pinned debug file descriptor."""
    command = [addr2line, "-e", f"/dev/fd/{descriptor}", "-f", "-C", "-a"] + [hex(value) for value in addresses]
    try:
        completed = subprocess.run(
            command,
            check=True,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=TOOL_TIMEOUT_SECONDS,
            pass_fds=(descriptor,),
        )
    except subprocess.CalledProcessError as exc:
        detail = exc.stderr.decode("utf-8", errors="replace").strip()[:400]
        raise SymbolicationError(f"addr2line failed: {detail}") from exc
    if len(completed.stdout) > MAX_ADDR2LINE_OUTPUT_BYTES:
        raise SymbolicationError("addr2line output exceeds the bounded size")
    lines = completed.stdout.decode("utf-8", errors="replace").splitlines()
    if len(lines) != 3 * len(addresses):
        raise SymbolicationError("addr2line output does not match the requested addresses")
    results = []
    for index, address in enumerate(addresses):
        echoed, function, location = lines[3 * index : 3 * index + 3]
        if int(echoed, 16) != address:
            raise SymbolicationError("addr2line output is out of order")
        results.append((function, location))
    return results


def _split_location(location: str) -> tuple[str | None, int | None]:
    path, separator, rest = location.rpartition(":")
    if not separator or path in ("", "??"):
        return None, None
    line_text = rest.split(" ", 1)[0]
    return path, (int(line_text) if line_text.isdigit() and int(line_text) > 0 else None)


def resolve(section: SymbolicSection, root: Path, addr2line: str) -> list[dict[str, object]]:
    results: list[dict[str, object]] = []
    for frame in section.frames:
        entry: dict[str, object] = {"index": frame.index, "kind": frame.kind}
        if frame.module is None:
            entry.update(status="no-module", address=hex(frame.address or 0))
        else:
            module = section.modules[frame.module]
            entry.update(module=module.name, buildId=module.build_id, offset=hex(frame.offset or 0))
        results.append(entry)

    for module in section.modules.values():
        frames = [frame for frame in section.frames if frame.module == module.index]
        descriptor = open_debug_file(root, module.build_id)
        if descriptor is None:
            for frame in frames:
                results[frame.index]["status"] = "missing-symbols"
            continue
        try:
            with os.fdopen(os.dup(descriptor), "rb") as handle:
                stored = inspect_elf(handle)
            if stored.build_id != module.build_id:
                raise SymbolicationError(
                    f"build-id mismatch for {module.name}: crash recorded {module.build_id}, "
                    f"symbol store entry holds {stored.build_id or 'no build-id'}"
                )
            if not stored.has_debug_info:
                raise SymbolicationError(f"symbol store entry for {module.name} has no DWARF debug info")
            # A return address points after the call; step back into the call instruction.
            addresses = [(frame.offset or 0) - (1 if frame.kind == "ra" else 0) for frame in frames]
            for frame, (function, location) in zip(frames, run_addr2line(addr2line, descriptor, addresses)):
                path, line = _split_location(location)
                entry = results[frame.index]
                entry["function"] = None if function == "??" else function
                entry["file"] = path
                entry["line"] = line
                entry["status"] = "resolved" if (function != "??" and line is not None) else "unresolved"
        finally:
            os.close(descriptor)
    return results


def format_text(results: list[dict[str, object]]) -> str:
    lines = []
    for entry in results:
        where = (
            f"{entry['module']}+{entry['offset']}" if "module" in entry else f"{entry.get('address', '?')} (no module)"
        )
        detail = entry["status"]
        if entry["status"] in ("resolved", "unresolved"):
            detail = f"{entry.get('function') or '??'} at {entry.get('file') or '??'}:{entry.get('line') or 0}"
        lines.append(f"#{entry['index']:<3} {entry['kind']} {where} {detail}")
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)

    store_parser = commands.add_parser("store", help="split debug info into a build-id symbol store")
    store_parser.add_argument("--store", type=Path, required=True)
    store_parser.add_argument("--objcopy", default="objcopy")
    store_parser.add_argument("binaries", type=Path, nargs="+")

    resolve_parser = commands.add_parser("resolve", help="symbolicate one crash log")
    resolve_parser.add_argument("--store", type=Path, required=True)
    resolve_parser.add_argument("--log", type=Path, required=True)
    resolve_parser.add_argument("--addr2line", default="addr2line")
    resolve_parser.add_argument("--json", action="store_true", help="emit JSON instead of text")

    args = parser.parse_args(argv)
    try:
        root = open_store_root(args.store)
        if args.command == "store":
            for binary in args.binaries:
                print(store_binary(root, binary, args.objcopy).relative_to(root))
            return 0

        section = parse_symbolic_section(read_crash_log(args.log))
        results = resolve(section, root, args.addr2line)
        if args.json:
            print(json.dumps({"schema": "spark.crash-symbolication.v1", "frames": results}, indent=2))
        else:
            sys.stdout.write(format_text(results))
        return 0
    except SymbolicationError as exc:
        print(f"symbolicate_crash: refused: {exc}", file=sys.stderr)
        return 2
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"symbolicate_crash: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
