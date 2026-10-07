"""Static, bounded inventory of the supported CPack NSIS 3.12 Unicode x86 ABI.

Only the pinned non-solid LZMA decoder and straight-line component File sections
are supported. Every data block must be classified, including plugin resources
and the recursively inspected uninstaller. This inventories embedded payloads;
it does not prove installation behavior or authenticate an Authenticode signer.
No installer, plugin, external .nsi script, or archive utility is executed.

Format authorities (NSIS sources are zlib/libpng licensed):
https://github.com/NSIS-Dev/nsis/blob/v312/Source/exehead/fileform.h
https://github.com/NSIS-Dev/nsis/blob/v312/Source/exehead/fileform.c
https://github.com/NSIS-Dev/nsis/blob/v312/Source/exehead/exec.c
https://github.com/NSIS-Dev/nsis/blob/v312/Source/exehead/util.c
https://github.com/NSIS-Dev/nsis/blob/v312/Source/build.cpp
https://learn.microsoft.com/en-us/windows/win32/debug/pe-format

Decoder hashes were checked against the installed NSIS v3.12 distribution's
Stubs/lzma-x86-unicode, whole SHA256
91c07cc1c37f2f4f367255e09cd3617c0ced0a9198ecbac6c1a8ea8921b4fe14.
They are fixed compatibility identities, never caller-configurable waivers.
"""
from __future__ import annotations

from contextlib import contextmanager
import hashlib
import io
import json
import lzma
from pathlib import Path, PurePosixPath
import re
import stat
import struct
import tempfile
import zlib


METHOD = "nsis-3.12-unicode-x86-nonsolid-lzma-static-v1"
MAX_BYTES = 1 << 30
MAX_EXPANDED = 2 << 30
MAX_BLOCK = 256 << 20
MAX_HEADER = 4 << 20
MAX_FILES = 32767
MAX_ENTRIES = 65536
SIGNATURE = b"\xef\xbe\xad\xdeNullsoftInst"
HEADER_HASH = "561fc689a951b1747aa833268ad24876b95b6f121c733636e8b73800dcaa002d"
STUB_HASHES = (
    (1024, 26112, "6521305f60c940d9ed7f3ee69c7f7855438ac7e9b27acd359b7538ab12146727"),
    (27136, 5120, "067fb88304317a6ae430bb3e9f052b66cc6d42f4971fd04f96452c382829863b"),
    (32256, 1536, "6831465af3ea6242cb83990c0f4ee78c343e7513e27af8dd7021321aa7942e3b"),
)
DIGEST = re.compile(r"[0-9a-f]{64}")
RESERVED = {"con", "prn", "aux", "nul", "conin$", "conout$", "clock$",
            *(f"com{i}" for i in range(10)), *(f"lpt{i}" for i in range(10))}
SUPPORT_NAMES = {"InstallOptions.dll", "System.dll", "StartMenu.dll", "UserInfo.dll", "ioSpecial.ini",
                 "modern-wizard.bmp", "modern-header.bmp", "NSIS.InstallOptions.ini"}
COMPONENTS = (("redist", "", 17), ("runtime", "Runtime Binaries", 17),
              ("samples", "Sample Content", 1), ("tools", "Developer Tools", 1))
# Full stock opcode ABI has logging disabled. All explicit branch operands are
# one-based; page/header callback offsets are zero-based. Indirect jumps fail.
BRANCHES = {2: (1,), 5: (1,), 12: (2, 3), 14: (1, 2), 22: (4, 6),
            26: (3, 4), 28: (3, 4, 5), 34: (2, 3)}


class NativeNsisError(ValueError):
    """An unsupported, ambiguous, changed, or malformed native package."""


def require(condition, message):
    if not condition:
        raise NativeNsisError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read_at(stream, offset, count):
    require(offset >= 0 and count >= 0, "negative NSIS byte range")
    stream.seek(offset)
    data = stream.read(count)
    require(len(data) == count, "truncated NSIS byte range")
    return data


def regular(path):
    info = path.lstat()
    require(stat.S_ISREG(info.st_mode) and not getattr(info, "st_file_attributes", 0) & 0x400
            and info.st_nlink == 1, "native package must be a regular non-reparse, non-hardlinked file")
    return info


def digest(path):
    remaining = regular(path).st_size
    require(remaining <= MAX_BYTES, "native package exceeds size limit")
    value = hashlib.sha256()
    with path.open("rb") as stream:
        while remaining:
            chunk = stream.read(min(1 << 20, remaining))
            require(chunk, "native package shrank while hashing")
            remaining -= len(chunk)
            value.update(chunk)
        require(not stream.read(1), "native package grew while hashing")
    return value.hexdigest()


def safe_path(value):
    require(isinstance(value, str) and 0 < len(value) < 1024, "invalid NSIS payload path")
    for part in value.split("/"):
        require(part not in ("", ".", "..") and not part.endswith((".", " "))
                and all(32 <= ord(c) < 127 and c not in '\\:*?"<>|~' for c in part)
                and part.split(".", 1)[0].casefold() not in RESERVED,
                f"unsafe or unsupported NSIS path: {value!r}")
    return value.casefold()


def no_collisions(paths):
    folded = [safe_path(path) for path in paths]
    require(len(set(folded)) == len(folded), "duplicate/case-aliased NSIS payload destination")
    keys = set(folded)
    require(all(not any(p.as_posix() in keys for p in PurePosixPath(path).parents if p.as_posix() != ".")
                for path in folded), "NSIS file/directory collision")


def certificate_boundary(stream, file_size, payload_end, offset, size):
    """Prove a trailing WIN_CERTIFICATE boundary, without asserting authenticity."""
    if not offset and not size:
        require(payload_end == file_size, "unclassified bytes after unsigned NSIS payload")
        return None
    require(offset == ((payload_end + 7) & ~7) and 8 <= size <= 1 << 20
            and offset + size == file_size and size % 8 == 0,
            "certificate table overlaps payload, is truncated, or has trailing data")
    require(not any(read_at(stream, payload_end, offset - payload_end)), "nonzero pre-certificate padding")
    raw = read_at(stream, offset, size)
    length, revision, kind = struct.unpack_from("<IHH", raw)
    require(8 < length <= size and ((length + 7) & ~7) == size
            and revision == 0x200 and kind == 2 and not any(raw[length:]),
            "unsupported WIN_CERTIFICATE table, revision, type, or padding")
    return {"offset": offset, "size": size, "sha256": sha(raw), "authentication": "not-verified"}


def reject_early_header(stub):
    # fileform.c discovers the FIRST matching firstheader on a 512-byte boundary,
    # even inside PE resources. A later valid overlay must not hide an earlier one.
    for offset in range(0, len(stub), 512):
        candidate = stub[offset:offset + 20]
        require(len(candidate) < 20 or candidate[4:] != SIGNATURE
                or struct.unpack_from("<I", candidate)[0] & ~15,
                "ambiguous NSIS firstheader before PE overlay")


def pe_envelope(stream, file_size):
    raw = bytearray(read_at(stream, 0, 1024))
    # All loader-affecting bytes other than the below tightly checked fields are
    # pinned too: checking only .text would accept a changed entry point/imports.
    word = lambda at: struct.unpack_from("<I", raw, at)[0]
    image_size, rva, virtual_size = word(296), word(352), word(356)
    cert_offset, cert_size, variables = word(368), word(372), word(592)
    resource_size = word(640)
    require(0 < variables <= 1 << 20 and variables % 2048 == 0
            and rva == ((0x2A000 + variables + 4095) & ~4095)
            and 0 < virtual_size <= resource_size <= 1 << 20
            and resource_size == ((virtual_size + 511) & ~511)
            and word(632) == virtual_size and word(636) == rva
            and image_size == ((rva + virtual_size + 4095) & ~4095),
            "unsupported NSIS PE section geometry")
    for at in (296, 304, 352, 356, 368, 372, 592, 632, 636, 640):
        raw[at:at + 4] = bytes(4)
    require(sha(raw) == HEADER_HASH, "unsupported NSIS PE loader/decoder ABI")
    for offset, size, expected in STUB_HASHES:
        require(sha(read_at(stream, offset, size)) == expected, "unknown NSIS decoder executable/data bytes")
    overlay = 33792 + resource_size
    require(overlay + 32 < file_size, "missing NSIS PE overlay")
    require(not any(read_at(stream, 33792 + virtual_size, resource_size - virtual_size)),
            "nonzero PE resource padding")
    reject_early_header(read_at(stream, 0, overlay))
    return overlay, (33792, 33792 + virtual_size), cert_offset, cert_size


def read_block(stream, offset, end, target, limit=MAX_BLOCK):
    """Stream one complete non-solid block with bounded input, output and memory."""
    require(type(limit) is int and 0 <= limit <= MAX_BLOCK, "invalid NSIS expansion limit")
    require(0 <= offset <= end - 4, "NSIS block header outside data boundary")
    encoded, = struct.unpack("<I", read_at(stream, offset, 4))
    stored, compressed = encoded & 0x7FFFFFFF, bool(encoded & 0x80000000)
    require(stored <= MAX_BLOCK and offset + 4 + stored <= end, "NSIS block crosses data boundary")
    remaining, total, value = stored, 0, hashlib.sha256()
    decoder = None
    if compressed:
        require(stored >= 6 and read_at(stream, offset + 4, 5) == b"\x5d\0\0\x80\0",
                "unsupported NSIS LZMA properties/dictionary or compressor")
        decoder = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=[
            {"id": lzma.FILTER_LZMA1, "dict_size": 8 << 20, "lc": 3, "lp": 0, "pb": 2}])
        remaining -= 5
    else:
        stream.seek(offset + 4)
    try:
        while remaining or (decoder is not None and not decoder.eof and not decoder.needs_input):
            chunk = stream.read(min(65536, remaining)) if decoder is None or decoder.needs_input else b""
            require(chunk or not remaining or decoder is not None and not decoder.needs_input, "truncated NSIS block")
            remaining -= len(chunk)
            out = decoder.decompress(chunk, max_length=min(65536, limit - total + 1)) if decoder else chunk
            total += len(out)
            require(total <= limit, "NSIS block expands beyond byte limit")
            value.update(out)
            target.write(out)
            if decoder and decoder.eof:
                require(not remaining and not decoder.unused_data, "trailing/concatenated NSIS compressed data")
                break
        require(decoder is None or decoder.eof, "truncated NSIS compressed stream")
    except lzma.LZMAError as error:
        raise NativeNsisError("invalid NSIS compressed stream") from error
    return {"offset": offset, "storedSize": stored, "compressed": compressed,
            "size": total, "sha256": value.hexdigest()}


def archive_header(stream, start, end, uninstall=False, prefix=b""):
    raw = read_at(stream, start, 28)
    flags, = struct.unpack_from("<I", raw)
    header_size, archive_size = struct.unpack_from("<II", raw, 20)
    require(raw[4:20] == SIGNATURE and flags == int(uninstall)
            and 300 <= header_size <= MAX_HEADER and archive_size == end - start,
            "unsupported NSIS archive flags/signature/length")
    crc = zlib.crc32(prefix[512:]) if uninstall else 0
    pos = start if uninstall else 512
    while pos < end - 4:
        data = read_at(stream, pos, min(1 << 20, end - 4 - pos))
        crc = zlib.crc32(data, crc)
        pos += len(data)
    require(crc == struct.unpack("<I", read_at(stream, end - 4, 4))[0], "NSIS CRC mismatch")
    output = io.BytesIO()
    block = read_block(stream, start + 28, end - 4, output, MAX_HEADER)
    require(block["size"] == header_size and block["compressed"], "NSIS header size/compression mismatch")
    return Header(output.getvalue()), start + 32 + block["storedSize"]


class Header:
    """The pinned 32-bit Unicode full-feature NSIS header ABI."""
    def __init__(self, raw):
        self.raw = raw
        require(300 <= len(raw) <= MAX_HEADER, "invalid NSIS header size")
        blocks = [struct.unpack_from("<II", raw, 4 + i * 8) for i in range(8)]
        (pages, np), (sections, ns), (entries, ne), (strings, nstr), (langs, nl), (colors, nc), bg, data = blocks
        lang_size, = struct.unpack_from("<I", raw, 100)
        require(pages == 300 and 0 < np <= 32 and sections == pages + np * 64
                and 0 < ns <= 32 and entries == sections + ns * 2072
                and 8 < ne <= MAX_ENTRIES and strings == entries + ne * 28
                and strings < langs <= colors <= len(raw) and (langs - strings) % 2 == 0
                and nstr == nc == 0 and 0 < nl <= 128 and 10 <= lang_size <= 4096
                and colors == langs + nl * lang_size and (len(raw) - colors) % 24 == 0
                and bg == data == (0, 0), "unsupported NSIS header block layout")
        self.strings = raw[strings:langs]
        self.entries = [struct.unpack_from("<7i", raw, entries + i * 28) for i in range(ne)]
        self.sections = []
        occupied = set()
        for i in range(ns):
            name, _types, flags, start, count, _size = struct.unpack_from("<6i", raw, sections + i * 2072)
            require(8 <= start < ne and 0 < count < ne and start + count < ne
                    and not flags & ~511, "invalid NSIS section bounds/flags")
            span = set(range(start, start + count + 1))
            require(not span & occupied and self.entries[start + count] == (1, 0, 0, 0, 0, 0, 0),
                    "overlapping or unterminated NSIS sections")
            occupied |= span
            require(not any(raw[sections + i * 2072 + 24:sections + (i + 1) * 2072]),
                    "unexpected initialized NSIS section name buffer")
            self.sections.append({"index": i, "name": self.string(name), "flags": flags,
                                  "start": start, "count": count})
        self.targets = []
        for i, entry in enumerate(self.entries):
            op = entry[0]
            require(0 <= op <= 69 and op != 66 and (op != 0 or i < 8 and not any(entry)),
                    "unknown NSIS opcode/decoder variant")
            for operand in BRANCHES.get(op, ()):
                target = entry[operand]
                require(0 <= target <= ne, "indirect/out-of-bounds NSIS control transfer")
                if target:
                    self.targets.append(target - 1)
            require(op != 63 or entry[3] >= 0 or entry[3] in (-1, -2, -3, -6),
                    "NSIS changes section code bounds")
        callbacks = list(struct.unpack_from("<10i", raw, 108))
        for i in range(np):
            callbacks.extend(struct.unpack_from("<3i", raw, pages + i * 64 + 8))
        require(all(-1 <= target < ne for target in callbacks), "invalid NSIS callback offset")
        self.targets.extend(target for target in callbacks if target >= 0)

    def string(self, offset):
        require(type(offset) is int and 0 <= offset < len(self.strings) // 2, "dynamic/out-of-bounds NSIS path string")
        start, stop = offset * 2, offset * 2
        while stop < len(self.strings) and stop - start <= 2048 and self.strings[stop:stop + 2] != b"\0\0":
            stop += 2
        require(stop < len(self.strings) and stop - start < 2048, "unterminated/oversized NSIS string")
        try:
            return self.strings[start:stop].decode("utf-16le")
        except UnicodeDecodeError as error:
            raise NativeNsisError("invalid NSIS Unicode string") from error


def variable(index):
    return "\x03" + chr((index & 127) | ((index & 0x3F80) << 1) | 0x8080)


def payload_plan(header, uninstall=False):
    files, support, consumed = [], {}, set()
    if not uninstall:
        require(len(header.sections) == 7, "unsupported CPack NSIS section graph")
        for section, (component, name, flags) in zip(header.sections, COMPONENTS):
            require(section["name"] == name and section["flags"] == flags, "NSIS component section identity drift")
            start, count = section["start"], section["count"]
            rows = header.entries[start:start + count]
            require(count >= 4 and rows[0][0] == 11 and rows[0][2:] == (1, 0, 0, 0, 0)
                    and header.string(rows[0][1]) == variable(21)
                    and rows[1][0:2] == (25, 31) and rows[1][3:] == (0, 0, 0, 0)
                    and header.string(rows[1][2]) == variable(22), "unsupported NSIS component output-root prologue")
            require(not any(start <= target <= start + count for target in header.targets),
                    "control transfer enters NSIS payload section")
            require(rows[-1][0] == 11 and rows[-1][2:] == (1, 0, 0, 0, 0)
                    and header.string(rows[-1][1]) == variable(31), "unsupported NSIS component output-root epilogue")
            directory = ""
            for instruction in range(start + 2, start + count - 1):
                entry = header.entries[instruction]
                if entry[0] == 11:
                    value = header.string(entry[1])
                    require(entry[2:] == (1, 0, 0, 0, 0) and value.startswith(variable(31) + "\\"),
                            "dynamic/unsupported NSIS SetOutPath")
                    directory = value[3:].replace("\\", "/")
                    safe_path(directory)
                else:
                    require(entry[0] == 20, "nonlinear/unsupported instruction in NSIS payload section")
                    name = header.string(entry[2])
                    safe_path(name)
                    require("/" not in name, "NSIS File destination must be a basename")
                    path = directory + "/" + name if directory else name
                    files.append({"path": path, "section": section["index"],
                                  "instruction": instruction, "block": entry[3]})
                    consumed.add(instruction)
            section["component"] = component
        require(files and len(files) <= MAX_FILES, "empty/oversized NSIS application manifest")
        no_collisions([item["path"] for item in files])
    for i, entry in enumerate(header.entries):
        if entry[0] != 20 or i in consumed:
            continue
        name = header.string(entry[2])
        require(name.startswith(variable(26) + "\\") and name[3:] in SUPPORT_NAMES,
                "unclassified NSIS File instruction outside component sections")
        path = "$PLUGINSDIR/" + name[3:]
        if path in support:
            require(support[path]["block"] == entry[3], "conflicting NSIS support aliases")
            support[path]["instructions"].append(i)
        else:
            support[path] = {"path": path, "block": entry[3], "instructions": [i]}
    return sorted(files, key=lambda item: item["path"]), sorted(support.values(), key=lambda item: item["path"])


def patch_icons(stub, raw, resources):
    result, spans, pos = bytearray(stub), [], 0
    while True:
        require(pos + 4 <= len(raw), "truncated NSIS uninstaller icon table")
        size, = struct.unpack_from("<I", raw, pos)
        if not size:
            require(pos + 4 == len(raw) and spans, "trailing/empty NSIS icon patch data")
            return bytes(result)
        # exec.c tests the first BYTE of the next record, not its DWORD size.
        # Also bound the overlap comparison work independently of the byte cap.
        require(raw[pos] != 0 and len(spans) < 256, "unsupported NSIS icon patch count/terminator")
        require(pos + 8 + size <= len(raw), "truncated NSIS icon patch")
        offset, = struct.unpack_from("<I", raw, pos + 4)
        require(resources[0] <= offset < offset + size <= resources[1]
                and all(offset + size <= low or offset >= high for low, high in spans),
                "NSIS icon patch overlaps or modifies code/outside resources")
        spans.append((offset, offset + size))
        result[offset:offset + size] = raw[pos + 8:pos + 8 + size]
        pos += 8 + size


def extract_blocks(stream, base, end, workspace, prefix, budget):
    blocks, paths, position = [], {}, base
    while position < end:
        require(len(blocks) < MAX_FILES, "too many NSIS blocks")
        target = workspace / f"{prefix}-{len(blocks)}"
        with target.open("xb") as output:
            row = read_block(stream, position, end, output, min(MAX_BLOCK, MAX_EXPANDED - budget[0]))
        budget[0] += row["size"]
        row["offset"] = position - base
        blocks.append(row)
        paths[row["offset"]] = target
        position += 4 + row["storedSize"]
    require(position == end and blocks, "NSIS data has an incomplete/trailing block")
    return blocks, paths


def bind_blocks(files, support, blocks, extra=()):
    # NSIS legitimately deduplicates equal contents used at different application
    # paths. Only exact block starts may be shared, never different block roles.
    by_offset = {row["offset"]: row for row in blocks}
    groups = [{item["block"] for item in files}, {item["block"] for item in support}, set(extra)]
    require(not groups[0] & groups[1] and not groups[0] & groups[2] and not groups[1] & groups[2],
            "NSIS data block has conflicting application/support/uninstaller roles")
    require(set.union(*groups) == set(by_offset), "unreferenced/interior-aliased/missing NSIS data blocks")
    for item in files + support:
        row = by_offset[item["block"]]
        item.update(size=row["size"], sha256=row["sha256"])


def uninstaller_payload(header, blocks, paths, stub, resources, workspace, budget):
    rows = [(i, e) for i, e in enumerate(header.entries) if e[0] == 62]
    require(len(rows) == 1, "NSIS must have exactly one statically described uninstaller")
    instruction, entry = rows[0]
    require(header.string(entry[1]) == variable(21) + "\\Uninstall.exe" and entry[3] > 0
            and header.string(entry[4]) == variable(21) + "\\" + variable(21) + "\\Uninstall.exe"
            and entry[5:] == (0, 0),
            "unsupported NSIS uninstaller destination/layout")
    indexes = {row["offset"]: i for i, row in enumerate(blocks)}
    require(entry[2] in indexes and indexes[entry[2]] == len(blocks) - 2,
            "NSIS uninstaller must occupy the final two distinct blocks")
    icons, body = blocks[-2:]
    require(icons["size"] == entry[3] and icons["size"] <= MAX_HEADER and body["size"] <= MAX_HEADER,
            "NSIS uninstaller/icon bounds mismatch")
    patched = patch_icons(stub, paths[icons["offset"]].read_bytes(), resources)
    reject_early_header(patched)
    # Signing only mutates PE bytes before the CRC start (512); the generated
    # uninstaller retains those bytes too. No authenticity assertion is made.
    with paths[body["offset"]].open("rb") as stream:
        nested, base = archive_header(stream, 0, body["size"], uninstall=True, prefix=patched)
        require(not any(e[0] == 62 for e in nested.entries), "recursive NSIS uninstaller")
        files, support = payload_plan(nested, uninstall=True)
        inner_blocks, _inner_paths = extract_blocks(stream, base, body["size"] - 4, workspace, "uninstaller", budget)
        bind_blocks(files, support, inner_blocks)
    identity = hashlib.sha256(patched)
    identity.update(paths[body["offset"]].read_bytes())
    return {"path": "Uninstall.exe", "instruction": instruction, "iconBlock": icons["offset"],
            "bodyBlock": body["offset"], "size": len(patched) + body["size"], "sha256": identity.hexdigest(),
            "headerSha256": sha(nested.raw), "blocks": inner_blocks, "support": support}


def manifest_digest(payload):
    return sha(json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode())


def prove_profile(evidence, policy):
    require(isinstance(policy, dict) and isinstance(policy.get("windowsRuntime"), dict),
            "missing committed NSIS profile authority")
    components = policy["windowsRuntime"].get("components")
    require(isinstance(components, list) and all(isinstance(c, str) for c in components)
            and len(components) == len(set(components)) and set(components) == {c[0] for c in COMPONENTS},
            "committed windows-runtime profile does not match supported NSIS component set")
    require(isinstance(evidence, dict) and isinstance(evidence.get("sections"), list)
            and isinstance(evidence.get("files"), list), "missing NSIS section/file manifest")
    sections = evidence["sections"]
    require(len(sections) == 7, "unsupported NSIS profile section count")
    for section, (component, name, flags) in zip(sections, COMPONENTS):
        require(isinstance(section, dict) and section.get("component") == component
                and section.get("name") == name and type(section.get("flags")) is int and section["flags"] == flags,
                "NSIS section graph does not match windows-runtime profile")
    require(all(isinstance(item, dict) and type(item.get("section")) is int and 0 <= item["section"] < 4
                and isinstance(item.get("path"), str) for item in evidence["files"]), "invalid NSIS profile file membership")
    return {item["path"]: sections[item["section"]]["component"] for item in evidence["files"]}


def validate_evidence(evidence, count):
    """Validate persisted inventory structure and complete block classification."""
    require(isinstance(evidence, dict) and set(evidence) == {"method", "scope", "root", "headerSha256",
            "sections", "blocks", "files", "support", "uninstaller", "certificate", "manifestSha256"},
            "missing/invalid NSIS extraction evidence")
    require(evidence["method"] == METHOD and evidence["scope"] == "all-embedded-file-instructions"
            and evidence["root"] == "$INSTDIR", "unsupported NSIS extraction method/scope")
    files = evidence["files"]
    require(type(count) is int and isinstance(files, list) and len(files) == count and 0 < count <= MAX_FILES,
            "NSIS file count mismatch")
    require(evidence["manifestSha256"] == manifest_digest(files), "NSIS application manifest digest mismatch")
    for digest_value in (evidence["headerSha256"], evidence["manifestSha256"]):
        require(isinstance(digest_value, str) and DIGEST.fullmatch(digest_value), "invalid NSIS evidence digest")
    uninstaller = evidence["uninstaller"]
    require(isinstance(uninstaller, dict) and set(uninstaller) == {"path", "instruction", "iconBlock", "bodyBlock",
            "size", "sha256", "headerSha256", "blocks", "support"} and uninstaller["path"] == "Uninstall.exe",
            "missing/invalid NSIS uninstaller evidence")
    require(type(uninstaller["instruction"]) is int and 0 <= uninstaller["instruction"] < MAX_ENTRIES
            and all(type(uninstaller[key]) is int and 0 <= uninstaller[key] <= MAX_BYTES
                    for key in ("iconBlock", "bodyBlock", "size"))
            and 0 < uninstaller["size"] <= MAX_BLOCK
            and all(isinstance(uninstaller[key], str) and DIGEST.fullmatch(uninstaller[key])
                    for key in ("sha256", "headerSha256")), "invalid NSIS uninstaller identity")
    sections = evidence["sections"]
    require(isinstance(sections, list) and len(sections) == 7, "invalid NSIS section manifest")
    last_end = 7
    for index, section in enumerate(sections):
        keys = {"index", "name", "flags", "start", "count"} | ({"component"} if index < 4 else set())
        require(isinstance(section, dict) and set(section) == keys and section["index"] == index
                and type(section["index"]) is int and isinstance(section["name"], str)
                and type(section["flags"]) is int and 0 <= section["flags"] <= 511
                and type(section["start"]) is int and type(section["count"]) is int
                and last_end < section["start"] < section["start"] + section["count"] < MAX_ENTRIES,
                "invalid/overlapping NSIS section identity")
        last_end = section["start"] + section["count"]
    for records in (evidence["blocks"], uninstaller["blocks"]):
        require(isinstance(records, list) and 0 < len(records) <= MAX_FILES, "invalid NSIS block manifest")
        offset = 0
        for row in records:
            require(isinstance(row, dict) and set(row) == {"offset", "storedSize", "compressed", "size", "sha256"}
                    and type(row["offset"]) is int and row["offset"] == offset
                    and type(row["storedSize"]) is int and 0 <= row["storedSize"] <= MAX_BLOCK
                    and type(row["compressed"]) is bool and type(row["size"]) is int and 0 <= row["size"] <= MAX_BLOCK
                    and isinstance(row["sha256"], str) and DIGEST.fullmatch(row["sha256"]), "invalid NSIS block identity")
            require(row["storedSize"] >= 6 if row["compressed"] else row["storedSize"] == row["size"],
                    "invalid NSIS stored/expanded block size")
            offset += row["storedSize"] + 4
    for group, support in ((files, False), (evidence["support"], True), (uninstaller["support"], True)):
        require(isinstance(group, list) and len(group) <= (len(SUPPORT_NAMES) if support else MAX_FILES),
                "invalid NSIS file/support manifest")
        for row in group:
            expected = {"path", "block", "size", "sha256", "instructions"} if support else {
                "path", "block", "size", "sha256", "section", "instruction"}
            require(isinstance(row, dict) and set(row) == expected and type(row["block"]) is int
                    and type(row["size"]) is int and 0 <= row["size"] <= MAX_BLOCK
                    and isinstance(row["sha256"], str) and DIGEST.fullmatch(row["sha256"]), "invalid NSIS file identity")
            if support:
                require(isinstance(row["path"], str) and row["path"].startswith("$PLUGINSDIR/")
                        and row["path"][12:] in SUPPORT_NAMES
                        and isinstance(row["instructions"], list) and 0 < len(row["instructions"]) <= MAX_ENTRIES
                        and all(type(n) is int and 0 <= n < MAX_ENTRIES for n in row["instructions"]),
                        "invalid NSIS support identity")
            else:
                require(type(row["section"]) is int and 0 <= row["section"] < 4
                        and type(row["instruction"]) is int and 0 <= row["instruction"] < MAX_ENTRIES,
                        "invalid NSIS file section/instruction")
                section = sections[row["section"]]
                require(section["start"] + 2 <= row["instruction"] < section["start"] + section["count"] - 1,
                        "NSIS File is outside its component section")
    no_collisions([item["path"] for item in files] + [uninstaller["path"]])
    require(any(item["path"] == "THIRD_PARTY_NOTICES.txt" for item in files), "NSIS evidence omits root notices")
    for support in (evidence["support"], uninstaller["support"]):
        no_collisions([item["path"] for item in support])
        instructions = [n for item in support for n in item["instructions"]]
        require(len(set(instructions)) == len(instructions), "duplicate NSIS support instruction")
    file_instructions = {item["instruction"] for item in files}
    support_instructions = {n for item in evidence["support"] for n in item["instructions"]}
    require(not file_instructions & support_instructions
            and uninstaller["instruction"] not in file_instructions | support_instructions,
            "conflicting NSIS instruction roles")
    require(not any(section["start"] <= instruction <= section["start"] + section["count"]
                    for section in sections[:4] for instruction in support_instructions),
            "NSIS support instruction overlaps component code")
    require(sum(row["size"] for row in evidence["blocks"] + uninstaller["blocks"]) <= MAX_EXPANDED,
            "NSIS evidence exceeds expanded byte limit")
    require(sum(row["size"] for row in files) <= MAX_EXPANDED, "NSIS application aliases exceed logical byte limit")
    require(len(evidence["blocks"]) >= 2
            and [row["offset"] for row in evidence["blocks"][-2:]]
            == [uninstaller["iconBlock"], uninstaller["bodyBlock"]], "invalid NSIS uninstaller block position")
    certificate = evidence["certificate"]
    if certificate is not None:
        require(isinstance(certificate, dict) and set(certificate) == {"offset", "size", "sha256", "authentication"}
                and type(certificate["offset"]) is int and 0 < certificate["offset"] <= MAX_BYTES
                and certificate["offset"] % 8 == 0 and type(certificate["size"]) is int
                and 8 <= certificate["size"] <= 1 << 20 and certificate["size"] % 8 == 0
                and certificate["authentication"] == "not-verified" and isinstance(certificate["sha256"], str)
                and DIGEST.fullmatch(certificate["sha256"]), "invalid NSIS certificate boundary identity")
    # bind_blocks also compares every declared file identity with its data block.
    for app, support, blocks, extra in ((files, evidence["support"], evidence["blocks"],
                                       (uninstaller["iconBlock"], uninstaller["bodyBlock"])),
                                      ([], uninstaller["support"], uninstaller["blocks"], ())):
        copies = [dict(item) for item in app + support]
        bind_blocks(copies[:len(app)], copies[len(app):], blocks, extra)
        require(copies == app + support, "NSIS file/block digest mismatch")
    require(len({item["instruction"] for item in files}) == len(files), "duplicate NSIS File instruction")
    prove_profile(evidence, {"windowsRuntime": {"components": [c[0] for c in COMPONENTS]}})


@contextmanager
def extracted_payload(artifact):
    """Yield evidence, numeric notice path, outer digest and trusted raw-file map."""
    artifact = Path(artifact)
    before = regular(artifact)
    require(artifact.suffix.lower() == ".exe" and 1024 < before.st_size <= MAX_BYTES, "unsupported NSIS package/size")
    outer = digest(artifact)
    with tempfile.TemporaryDirectory(prefix="spark-nsis-") as directory:
        workspace = Path(directory).resolve()
        private = workspace / "package"
        with artifact.open("rb") as source, private.open("xb") as target:
            remaining = before.st_size
            while remaining:
                chunk = source.read(min(1 << 20, remaining))
                require(chunk, "NSIS package shrank during private copy")
                target.write(chunk)
                remaining -= len(chunk)
            require(not source.read(1), "NSIS package grew during private copy")
        require(digest(private) == outer and digest(artifact) == outer, "NSIS package changed during private copy")
        with private.open("rb") as stream:
            start, resources, cert_offset, cert_size = pe_envelope(stream, before.st_size)
            archive_size, = struct.unpack("<I", read_at(stream, start + 24, 4))
            end = start + archive_size
            require(start + 32 < end <= before.st_size, "NSIS archive crosses file boundary")
            certificate = certificate_boundary(stream, before.st_size, end, cert_offset, cert_size)
            header, base = archive_header(stream, start, end)
            files, support = payload_plan(header)
            budget = [len(header.raw)]
            blocks, paths = extract_blocks(stream, base, end - 4, workspace, "payload", budget)
            uninstaller = uninstaller_payload(header, blocks, paths, read_at(stream, 0, start),
                                              resources, workspace, budget)
            bind_blocks(files, support, blocks, (uninstaller["iconBlock"], uninstaller["bodyBlock"]))
        evidence = {"method": METHOD, "scope": "all-embedded-file-instructions", "root": "$INSTDIR",
                    "headerSha256": sha(header.raw), "sections": header.sections, "blocks": blocks,
                    "files": files, "support": support, "uninstaller": uninstaller, "certificate": certificate,
                    "manifestSha256": manifest_digest(files)}
        validate_evidence(evidence, len(files))
        payload_files = {item["path"]: paths[item["block"]] for item in files}
        require("THIRD_PARTY_NOTICES.txt" in payload_files, "NSIS has no root THIRD_PARTY_NOTICES.txt")
        snapshot = {path.name: (regular(path).st_size, digest(path)) for path in workspace.iterdir()}
        require(digest(private) == outer, "NSIS private package changed during inspection")
        yield evidence, payload_files["THIRD_PARTY_NOTICES.txt"], outer, payload_files
        require({path.name for path in workspace.iterdir()} == set(snapshot), "unexpected extracted NSIS payload")
        for name, identity in snapshot.items():
            path = workspace / name
            require((regular(path).st_size, digest(path)) == identity, "extracted NSIS payload mutated")
        after = regular(artifact)
        regular(private)
        require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns)
                == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns)
                and digest(artifact) == outer and digest(private) == outer,
                "NSIS package changed during reconciliation")
