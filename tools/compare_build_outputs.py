#!/usr/bin/env python3
"""Compare the outputs of two builds for reproducibility (BLD-100).

Two clean builds of identical sources, made in different directories, must
produce equivalent outputs. This tool writes a normalized manifest of the
eligible outputs under a root and compares two of them:

* Eligible outputs are regular files whose content identifies them as ELF
  (images, split ``.debug`` files, objects), PE/COFF images (EXE/DLL) or ``ar``
  archives (static libraries on both toolchains). Other files, symlinks and
  PDBs are not eligible: an MSVC PDB is not byte-reproducible, and the image's
  RSDS record, which /Brepro derives from content, is what identifies it.
* Each entry records the path relative to the root, the size, the SHA-256,
  the identity a debugger or crash pipeline keys on (ELF GNU build-id; PE COFF
  timestamp, CodeView RSDS GUID/age/PDB name and whether the image carries the
  /Brepro REPRO debug entry) and a SHA-256 per section
  (ELF sections, PE sections, archive members) plus pseudo-sections for the
  file headers, so a difference can be located without the files.
* No absolute path, timestamp or host detail is recorded, so equivalent trees
  produce byte-identical manifests.

Two manifests are equivalent when they list the same paths with the same
content. The comparison reports every missing, extra and differing file, and
for a differing file its identity changes and its first differing section.

Subcommands:
    manifest ROOT --output FILE        write the manifest of ROOT
    compare  A.json B.json [--report]  compare two manifests
    trees    ROOT_A ROOT_B [--report]  manifest both roots and compare them
    two-tree --source DIR --work DIR --target T [--scan REL] [-- CMAKE ARGS]
             copy DIR into two differently named and nested source trees,
             configure and build T in each, and compare the scanned output
             dirs. The inner builds drop compiler launchers (no cache can
             mask a difference) and CFLAGS/CXXFLAGS/LDFLAGS from the
             environment: only the configure arguments choose flags. On an
             equivalent result the copies and build trees are deleted and
             the manifests, report and log stay in the work directory.

Exit codes: 0 equivalent (or manifest written), 1 not equivalent, 2 usage,
input or build error. A root with no eligible outputs is an error, never a
vacuous match. Only the Python standard library is used; structures are read
with bounded, checked offsets, and a malformed eligible file is an error.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from shipping_symbol_manifest import FormatError, inspect_elf, inspect_pe  # noqa: E402

SCHEMA = "spark.build-output-manifest/1"

ELF_MAGIC = b"\x7fELF"
AR_MAGIC = b"!<arch>\n"
AR_HEADER_BYTES = 60
MAX_ELF_SECTIONS = 65535
MAX_PE_SECTIONS = 96
MAX_DEBUG_ENTRIES = 64
IMAGE_DEBUG_TYPE_REPRO = 16
MAX_AR_MEMBERS = 1 << 20
MAX_NAME_TABLE_BYTES = 64 * 1024 * 1024
HASH_CHUNK = 1024 * 1024

ENTRY_KEYS = {"path", "kind", "size", "sha256", "identity", "sections"}
IDENTITY_KEYS = {
    "elf": {"machine", "buildId"},
    "pe": {"machine", "timestamp", "codeView", "repro"},
    "ar": set(),
}
CODEVIEW_KEYS = {"guid", "age", "pdb"}
SECTION_KEYS = {"name", "size", "sha256"}
# Sections whose bytes follow from the rest of the file: never the root cause
# of a difference while any other section also differs.
DERIVED_SECTIONS = {
    "<elf-header>", "<program-headers>", "<section-headers>", "<pe-headers>",
    ".note.gnu.build-id", ".gnu_debuglink", "<symbol-index>",
}


class InputError(Exception):
    """A root, manifest or build cannot be used."""


# ---------------------------------------------------------------------------
# Reading
# ---------------------------------------------------------------------------


def _read(handle, offset: int, size: int, file_size: int) -> bytes:
    if offset < 0 or size < 0 or offset > file_size or size > file_size - offset:
        raise FormatError("structure points outside the file")
    handle.seek(offset)
    data = handle.read(size)
    if len(data) != size:
        raise FormatError("file is truncated")
    return data


def _hash_range(handle, offset: int, size: int, file_size: int) -> str:
    if offset < 0 or size < 0 or offset > file_size or size > file_size - offset:
        raise FormatError("section points outside the file")
    digest = hashlib.sha256()
    handle.seek(offset)
    remaining = size
    while remaining:
        chunk = handle.read(min(HASH_CHUNK, remaining))
        if not chunk:
            raise FormatError("file is truncated")
        digest.update(chunk)
        remaining -= len(chunk)
    return digest.hexdigest()


def _section(handle, name: str, offset: int, size: int, file_size: int) -> dict:
    return {"name": name, "size": size, "sha256": _hash_range(handle, offset, size, file_size)}


def _elf_sections(handle, file_size: int) -> list[dict]:
    ident = _read(handle, 0, 16, file_size)
    if ident[4] not in (1, 2) or ident[5] not in (1, 2):
        raise FormatError("unsupported ELF class or byte order")
    is64 = ident[4] == 2
    endian = "<" if ident[5] == 1 else ">"
    header_size = 64 if is64 else 52
    header = _read(handle, 0, header_size, file_size)
    if is64:
        e_phoff, e_shoff = struct.unpack_from(endian + "QQ", header, 32)
        e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(endian + "HHHHH", header, 54)
    else:
        e_phoff, e_shoff = struct.unpack_from(endian + "II", header, 28)
        e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(endian + "HHHHH", header, 42)

    sections = [_section(handle, "<elf-header>", 0, header_size, file_size)]
    if e_phoff and e_phnum:
        sections.append(_section(handle, "<program-headers>", e_phoff, e_phnum * e_phentsize, file_size))
    if not e_shoff or not e_shnum:
        return sections
    if e_shentsize != (64 if is64 else 40) or e_shnum > MAX_ELF_SECTIONS or e_shstrndx >= e_shnum:
        raise FormatError("unsupported ELF section header table")
    table = _read(handle, e_shoff, e_shnum * e_shentsize, file_size)
    raw = []
    for index in range(e_shnum):
        base = index * e_shentsize
        if is64:
            name, sh_type, _, _, sh_offset, sh_size = struct.unpack_from(endian + "IIQQQQ", table, base)
        else:
            name, sh_type, _, _, sh_offset, sh_size = struct.unpack_from(endian + "IIIIII", table, base)
        raw.append((name, sh_type, sh_offset, sh_size))
    _, strtab_type, strtab_offset, strtab_size = raw[e_shstrndx]
    if strtab_type == 8 or strtab_size > MAX_NAME_TABLE_BYTES:
        raise FormatError("unsupported ELF section name table")
    names = _read(handle, strtab_offset, strtab_size, file_size)
    for index, (name_offset, sh_type, sh_offset, sh_size) in enumerate(raw):
        if index == 0:
            continue
        end = names.find(b"\x00", name_offset) if name_offset < len(names) else -1
        name = names[name_offset:end].decode("utf-8", "replace") if end >= 0 else f"<section-{index}>"
        # NOBITS (.bss, and every loadable section of a split .debug file)
        # occupies no file bytes; its size still has to match.
        size = 0 if sh_type == 8 else sh_size
        entry = _section(handle, name, sh_offset if size else 0, size, file_size)
        entry["size"] = sh_size
        sections.append(entry)
    sections.append(_section(handle, "<section-headers>", e_shoff, e_shnum * e_shentsize, file_size))
    return sections


def _pe_layout(handle, file_size: int) -> tuple[int, int, list[tuple[str, int, int]], int]:
    dos = _read(handle, 0, 64, file_size)
    (pe_offset,) = struct.unpack_from("<I", dos, 0x3C)
    coff = _read(handle, pe_offset, 24, file_size)
    if coff[:4] != b"PE\x00\x00":
        raise FormatError("MZ file without a PE signature")
    machine, section_count, timestamp, _, _, optional_size, _ = struct.unpack_from("<HHIIIHH", coff, 4)
    if section_count > MAX_PE_SECTIONS:
        raise FormatError("too many PE sections")
    table_offset = pe_offset + 24 + optional_size
    table = _read(handle, table_offset, 40 * section_count, file_size)
    sections = []
    for index in range(section_count):
        name = table[40 * index : 40 * index + 8].rstrip(b"\x00").decode("utf-8", "replace")
        raw_size, raw_pointer = struct.unpack_from("<II", table, 40 * index + 16)
        sections.append((name or f"<section-{index}>", raw_pointer if raw_size else 0, raw_size))
    return machine, timestamp, sections, table_offset + 40 * section_count


def _ar_members(handle, file_size: int) -> list[dict]:
    members = []
    long_names = b""
    offset = len(AR_MAGIC)
    while offset < file_size:
        if len(members) >= MAX_AR_MEMBERS:
            raise FormatError("too many archive members")
        header = _read(handle, offset, AR_HEADER_BYTES, file_size)
        if header[58:60] != b"`\n":
            raise FormatError("malformed archive member header")
        try:
            size = int(header[48:58].decode("ascii").strip())
        except ValueError as error:
            raise FormatError("malformed archive member size") from error
        raw_name = header[:16].decode("ascii", "replace").rstrip()
        data_offset = offset + AR_HEADER_BYTES
        if raw_name == "//":
            if size > MAX_NAME_TABLE_BYTES:
                raise FormatError("oversized archive name table")
            long_names = _read(handle, data_offset, size, file_size)
            name = "<long-names>"
        elif raw_name.startswith("/") and raw_name[1:].isdigit():
            start = int(raw_name[1:])
            end = long_names.find(b"\n", start)
            if start >= len(long_names) or end < 0:
                raise FormatError("archive long name points outside the name table")
            name = long_names[start:end].decode("utf-8", "replace").rstrip("/")
        elif raw_name in ("/", "/SYM64/"):
            name = "<symbol-index>"
        else:
            name = raw_name.rstrip("/")
        # The member header (timestamp, uid, gid, mode) is hashed with the data:
        # a deterministic archive has zeros there, a nondeterministic one differs.
        digest = hashlib.sha256(header)
        digest.update(bytes.fromhex(_hash_range(handle, data_offset, size, file_size)))
        members.append({"name": name, "size": size, "sha256": digest.hexdigest()})
        offset = data_offset + size + (size & 1)
    return members


def describe(path: Path) -> dict | None:
    """Return the manifest entry fields for an eligible file, or None."""
    with path.open("rb") as handle:
        handle.seek(0, os.SEEK_END)
        file_size = handle.tell()
        handle.seek(0)
        head = handle.read(len(AR_MAGIC))
        if head.startswith(ELF_MAGIC):
            elf = inspect_elf(handle)
            identity = {"machine": elf.machine, "buildId": elf.build_id}
            kind, sections = "elf", _elf_sections(handle, file_size)
        elif head.startswith(b"MZ"):
            machine, timestamp, layout, headers_end = _pe_layout(handle, file_size)
            handle.seek(0)
            code_view = None
            debug_types = _debug_entry_types(handle, file_size)
            image = inspect_pe(handle) if debug_types is not None else None
            if image is not None:
                code_view = {"guid": image.pdb_guid, "age": image.pdb_age, "pdb": image.pdb_path}
            identity = {
                "machine": f"pe-{machine:#06x}",
                "timestamp": f"{timestamp:08x}",
                "codeView": code_view,
                "repro": IMAGE_DEBUG_TYPE_REPRO in (debug_types or []),
            }
            kind = "pe"
            sections = [_section(handle, "<pe-headers>", 0, headers_end, file_size)]
            sections += [_section(handle, name, offset, size, file_size) for name, offset, size in layout]
        elif head == AR_MAGIC:
            identity, kind, sections = {}, "ar", _ar_members(handle, file_size)
        else:
            return None
        return {"kind": kind, "size": file_size, "identity": identity, "sections": sections}


def _debug_entry_types(handle, file_size: int) -> list[int] | None:
    """The debug directory entry types of a PE image, or None when it declares no debug directory.

    inspect_pe requires a debug directory; the REPRO entry (type 16) marks an
    image linked with /Brepro, whose COFF timestamp is a content hash.
    """
    dos = _read(handle, 0, 64, file_size)
    (pe_offset,) = struct.unpack_from("<I", dos, 0x3C)
    coff = _read(handle, pe_offset, 24, file_size)
    _, section_count, _, _, _, optional_size, characteristics = struct.unpack_from("<HHIIIHH", coff, 4)
    if not characteristics & 0x0002:
        return None
    optional = _read(handle, pe_offset + 24, optional_size, file_size)
    if len(optional) < 2:
        raise FormatError("missing PE optional header")
    (magic,) = struct.unpack_from("<H", optional, 0)
    directory_offset = {0x20B: 112, 0x10B: 96}.get(magic)
    if directory_offset is None:
        raise FormatError("unknown PE optional header magic")
    if directory_offset + 56 > len(optional):
        return None
    (directory_count,) = struct.unpack_from("<I", optional, directory_offset - 4)
    debug_rva, debug_size = struct.unpack_from("<II", optional, directory_offset + 48)
    if directory_count <= 6 or debug_rva == 0 or debug_size == 0:
        handle.seek(0)
        return None
    if debug_size % 28 or debug_size // 28 > MAX_DEBUG_ENTRIES:
        raise FormatError("malformed PE debug directory")
    if section_count > MAX_PE_SECTIONS:
        raise FormatError("too many PE sections")
    table = _read(handle, pe_offset + 24 + optional_size, 40 * section_count, file_size)
    directory_file_offset = None
    for index in range(section_count):
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<IIII", table, 40 * index + 8)
        if virtual_address <= debug_rva and debug_rva + debug_size <= virtual_address + raw_size:
            directory_file_offset = raw_pointer + (debug_rva - virtual_address)
            break
    if directory_file_offset is None:
        raise FormatError("PE debug directory lies outside every section's file data")
    entries = _read(handle, directory_file_offset, debug_size, file_size)
    handle.seek(0)
    return [struct.unpack_from("<I", entries, 28 * index + 12)[0] for index in range(debug_size // 28)]


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(HASH_CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


# ---------------------------------------------------------------------------
# Manifests
# ---------------------------------------------------------------------------


def build_manifest(root: Path) -> dict:
    if not root.is_dir():
        raise InputError(f"not a directory: {root}")
    entries = []
    errors = []
    for directory, subdirectories, files in os.walk(root, followlinks=False):
        subdirectories.sort()
        for name in sorted(files):
            path = Path(directory) / name
            if path.is_symlink() or not path.is_file():
                continue
            relative = path.relative_to(root).as_posix()
            try:
                fields = describe(path)
            except (FormatError, OSError, struct.error, UnicodeDecodeError, ValueError) as error:
                errors.append(f"{relative}: {error}")
                continue
            if fields is not None:
                entries.append({"path": relative, "sha256": _sha256(path), **fields})
    if errors:
        raise InputError("malformed eligible outputs:\n  " + "\n  ".join(errors))
    if not entries:
        raise InputError(f"no ELF, PE or archive outputs under {root}")
    manifest = {"schema": SCHEMA, "entries": sorted(entries, key=lambda entry: entry["path"])}
    validate_manifest(manifest)
    return manifest


def validate_manifest(manifest: object) -> None:
    """Reject anything that is not exactly the closed schema this tool writes."""
    if not isinstance(manifest, dict) or set(manifest) != {"schema", "entries"}:
        raise InputError("manifest must have exactly the keys schema and entries")
    if manifest["schema"] != SCHEMA:
        raise InputError(f"manifest schema must be {SCHEMA}")
    entries = manifest["entries"]
    if not isinstance(entries, list) or not entries:
        raise InputError("manifest entries must be a non-empty list")
    paths = [entry.get("path") if isinstance(entry, dict) else None for entry in entries]
    if None in paths or paths != sorted(set(paths), key=str):
        raise InputError("manifest paths must be unique and sorted")
    for entry in entries:
        location = f"entry {entry['path']!r}"
        if set(entry) != ENTRY_KEYS or entry["kind"] not in IDENTITY_KEYS:
            raise InputError(f"{location} does not match the entry schema")
        if not isinstance(entry["path"], str) or not entry["path"] or entry["path"].startswith("/"):
            raise InputError(f"{location} path must be a non-empty relative path")
        if not isinstance(entry["size"], int) or entry["size"] < 0:
            raise InputError(f"{location} size must be a non-negative integer")
        if not isinstance(entry["sha256"], str) or len(entry["sha256"]) != 64:
            raise InputError(f"{location} sha256 must be a SHA-256 hex digest")
        identity = entry["identity"]
        if not isinstance(identity, dict) or set(identity) != IDENTITY_KEYS[entry["kind"]]:
            raise InputError(f"{location} identity does not match the {entry['kind']} schema")
        if entry["kind"] == "pe" and not isinstance(identity["repro"], bool):
            raise InputError(f"{location} repro must be a boolean")
        code_view = identity.get("codeView")
        if code_view is not None and (not isinstance(code_view, dict) or set(code_view) != CODEVIEW_KEYS):
            raise InputError(f"{location} codeView must have exactly guid, age and pdb")
        sections = entry["sections"]
        if not isinstance(sections, list) or not all(
            isinstance(section, dict) and set(section) == SECTION_KEYS for section in sections
        ):
            raise InputError(f"{location} sections must be objects with exactly name, size and sha256")


def load_manifest(path: Path) -> dict:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise InputError(f"cannot read manifest {path}: {error}") from error
    validate_manifest(manifest)
    return manifest


def _first_difference(left: dict, right: dict) -> tuple[str, list[str]]:
    """Name the first differing section that is a cause, and every differing section.

    Headers, the build-id note and the debuglink CRC change whenever any
    content changes, so they are reported first only when nothing else differs.
    The exception is a PE image linked without /Brepro whose COFF timestamp
    differs: that timestamp is the wall-clock link time, it lives in the
    headers, and the debug directory and CodeView record that repeat it are
    the derived differences.
    """
    differing: list[tuple[int, str, str]] = []
    for index, (a, b) in enumerate(zip(left["sections"], right["sections"])):
        if a["name"] != b["name"]:
            return f"section layout differs at #{index}: {a['name']} vs {b['name']}", [a["name"], b["name"]]
        if a["size"] != b["size"] or a["sha256"] != b["sha256"]:
            differing.append((index, a["name"], f"{a['name']} (#{index}, {a['size']} vs {b['size']} bytes)"))
    if len(left["sections"]) != len(right["sections"]):
        return f"section count differs: {len(left['sections'])} vs {len(right['sections'])}", []
    if not differing:
        return "bytes outside every recorded section", []
    if (
        left["kind"] == "pe"
        and left["identity"]["timestamp"] != right["identity"]["timestamp"]
        and not (left["identity"]["repro"] and right["identity"]["repro"])
    ):
        return "<pe-headers> (COFF timestamp; image not linked with /Brepro)", [name for _, name, _ in differing]
    causes = [entry for entry in differing if entry[1] not in DERIVED_SECTIONS]
    first = (causes or differing)[0][2]
    return first, [name for _, name, _ in differing]


def compare_manifests(left: dict, right: dict) -> dict:
    left_entries = {entry["path"]: entry for entry in left["entries"]}
    right_entries = {entry["path"]: entry for entry in right["entries"]}
    differing = []
    for path in sorted(left_entries.keys() & right_entries.keys()):
        a, b = left_entries[path], right_entries[path]
        if a == b:
            continue
        if a["kind"] != b["kind"]:
            reason, sections, identity = f"kind differs: {a['kind']} vs {b['kind']}", [], ["kind"]
        else:
            reason, sections = _first_difference(a, b)
            identity = sorted(key for key in a["identity"] if a["identity"][key] != b["identity"][key])
        differing.append(
            {"path": path, "firstDifference": reason, "differingSections": sections, "identityChanged": identity}
        )
    return {
        "equivalent": not differing and left_entries.keys() == right_entries.keys(),
        "compared": len(left_entries.keys() & right_entries.keys()),
        "onlyInFirst": sorted(left_entries.keys() - right_entries.keys()),
        "onlyInSecond": sorted(right_entries.keys() - left_entries.keys()),
        "differing": differing,
    }


def print_report(report: dict, first: str, second: str) -> None:
    for path in report["onlyInFirst"]:
        print(f"only in {first}: {path}")
    for path in report["onlyInSecond"]:
        print(f"only in {second}: {path}")
    for entry in report["differing"]:
        changed = f"; identity changed: {', '.join(entry['identityChanged'])}" if entry["identityChanged"] else ""
        print(f"differs: {entry['path']}: first difference in {entry['firstDifference']}{changed}")
    verdict = "EQUIVALENT" if report["equivalent"] else "NOT EQUIVALENT"
    print(f"build outputs {verdict}: {report['compared']} compared, {len(report['differing'])} differing, "
          f"{len(report['onlyInFirst'])} only in {first}, {len(report['onlyInSecond'])} only in {second}")


def _write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _finish(report: dict, first: str, second: str, report_path: Path | None) -> int:
    print_report(report, first, second)
    if report_path is not None:
        _write_json(report_path, report)
    return 0 if report["equivalent"] else 1


# ---------------------------------------------------------------------------
# Two-tree build
# ---------------------------------------------------------------------------


def copy_source(source: Path, destination: Path, excluded: list[Path]) -> None:
    """Copy every regular file of source except .git and the excluded trees.

    Files are copied, never hard-linked: a configure or build step that
    rewrote a file in place would otherwise modify the original tree.
    """

    def ignore(directory: str, names: list[str]) -> set[str]:
        skipped = {name for name in names if name == ".git"}
        for name in names:
            candidate = Path(directory, name).resolve()
            if any(candidate == path for path in excluded):
                skipped.add(name)
        return skipped

    shutil.copytree(source, destination, symlinks=True, ignore=ignore)


def _run_logged(command: list[str], log: Path, environment: dict[str, str]) -> None:
    print("+ " + " ".join(command), flush=True)
    with log.open("a", encoding="utf-8") as handle:
        handle.write("+ " + " ".join(command) + "\n")
        handle.flush()
        result = subprocess.run(command, stdout=handle, stderr=subprocess.STDOUT, env=environment, check=False)
    if result.returncode != 0:
        tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-40:]
        raise InputError(f"command failed (exit {result.returncode}): {' '.join(command)}\n" + "\n".join(tail))


def two_tree(args: argparse.Namespace) -> int:
    source = args.source.resolve()
    work = args.work.resolve()
    if not (source / "CMakeLists.txt").is_file():
        raise InputError(f"no CMakeLists.txt in {source}")
    if work == source or work in source.parents:
        raise InputError("the work directory must not contain the source tree")
    excluded = [path.resolve() for path in args.exclude] + [work]
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)

    # Different names, lengths and depths: any absolute or build-relative path
    # that leaks into an output makes the two trees differ.
    trees = [(work / "a" / "src", work / "a" / "build"),
             (work / "tree-b" / "nested" / "src", work / "tree-b" / "out" / "obj" / "build")]
    # Hermetic inner builds: only the configure arguments choose flags, and no
    # compiler cache can hand back an object built for the other tree.
    environment = dict(os.environ, CCACHE_DISABLE="1")
    for variable in ("CMAKE_C_COMPILER_LAUNCHER", "CMAKE_CXX_COMPILER_LAUNCHER", "CFLAGS", "CXXFLAGS", "LDFLAGS"):
        environment.pop(variable, None)
    log = work / "two-tree.log"

    first_source = trees[0][0]
    print(f"copying {source} -> {first_source}", flush=True)
    copy_source(source, first_source, excluded)
    # The second tree is copied from the first so both hold the same snapshot
    # even while the original tree is being edited.
    print(f"copying {first_source} -> {trees[1][0]}", flush=True)
    copy_source(first_source, trees[1][0], [])

    for tree_source, tree_build in trees:
        _run_logged(
            [args.cmake, "-S", str(tree_source), "-B", str(tree_build),
             "-DCMAKE_C_COMPILER_LAUNCHER=", "-DCMAKE_CXX_COMPILER_LAUNCHER=", *args.cmake_args],
            log, environment)
        _run_logged(
            [args.cmake, "--build", str(tree_build), "--parallel", str(args.jobs), "--target", *args.target],
            log, environment)

    manifests = []
    for label, (_, tree_build) in zip(("a", "b"), trees):
        combined: list[dict] = []
        for scan in args.scan:
            manifest = build_manifest(tree_build / scan)
            combined += [dict(entry, path=f"{scan}/{entry['path']}") for entry in manifest["entries"]]
        manifest = {"schema": SCHEMA, "entries": sorted(combined, key=lambda entry: entry["path"])}
        validate_manifest(manifest)
        _write_json(work / f"manifest-{label}.json", manifest)
        manifests.append(manifest)
    status = _finish(compare_manifests(*manifests), "tree a", "tree b", work / "report.json")
    if status == 0:
        # Two source copies and two build trees take over a gigabyte; after an
        # equivalent result only the manifests, report and log are kept. A
        # failure keeps everything for diagnosis.
        for tree in (work / "a", work / "tree-b"):
            shutil.rmtree(tree)
    return status


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)

    manifest_parser = commands.add_parser("manifest", help="write the manifest of one output root")
    manifest_parser.add_argument("root", type=Path)
    manifest_parser.add_argument("--output", type=Path, required=True)

    compare_parser = commands.add_parser("compare", help="compare two manifests")
    compare_parser.add_argument("first", type=Path)
    compare_parser.add_argument("second", type=Path)
    compare_parser.add_argument("--report", type=Path)

    trees_parser = commands.add_parser("trees", help="manifest two output roots and compare them")
    trees_parser.add_argument("first", type=Path)
    trees_parser.add_argument("second", type=Path)
    trees_parser.add_argument("--report", type=Path)

    two_tree_parser = commands.add_parser("two-tree", help="build a target in two source copies and compare")
    two_tree_parser.add_argument("--source", type=Path, required=True)
    two_tree_parser.add_argument("--work", type=Path, required=True)
    two_tree_parser.add_argument("--target", action="append", required=True)
    two_tree_parser.add_argument("--scan", action="append", required=True, help="output dir relative to the build")
    two_tree_parser.add_argument("--exclude", type=Path, action="append", default=[], help="source path not copied")
    two_tree_parser.add_argument("--cmake", default=shutil.which("cmake") or "cmake")
    two_tree_parser.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    two_tree_parser.add_argument("cmake_args", nargs=argparse.REMAINDER, help="-- then configure arguments")

    args = parser.parse_args(argv)
    try:
        if args.command == "manifest":
            manifest = build_manifest(args.root.resolve())
            _write_json(args.output, manifest)
            print(f"build-output manifest: {len(manifest['entries'])} output(s) -> {args.output}")
            return 0
        if args.command == "compare":
            report = compare_manifests(load_manifest(args.first), load_manifest(args.second))
            return _finish(report, str(args.first), str(args.second), args.report)
        if args.command == "trees":
            report = compare_manifests(build_manifest(args.first.resolve()), build_manifest(args.second.resolve()))
            return _finish(report, str(args.first), str(args.second), args.report)
        if args.cmake_args and args.cmake_args[0] == "--":
            args.cmake_args = args.cmake_args[1:]
        return two_tree(args)
    except InputError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
