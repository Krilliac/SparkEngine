#!/usr/bin/env python3
"""Check that every Font Awesome codepoint the UI draws exists in the icon font the UI loads.

Spark::Ui::LoadFontSet (SparkUI/Source/Fonts.cpp) merges SparkEditor/Fonts/fa-solid-900.ttf into
every text face, restricted to the `kIconRanges` private-use block. A codepoint the font does not
map renders ImGui's fallback glyph, and a mistyped escape renders a *different* icon (the Start
page once showed "circle-minus" for ICON_FA_FOLDER_OPEN because its bytes encoded U+F056 while the
comment claimed U+F076). Neither fails a build, so this script is the gate:

* Icon definition files (SparkEditor/Source/Core/EditorIcons.h, SparkUI/Include/SparkUI/Glyphs.h):
  every `#define ICON_*` / `kName = "..."` entry must encode exactly one codepoint, carry a
  `U+XXXX glyph-name` comment whose codepoint equals the encoded bytes, and whose glyph name equals
  the font's own name for that codepoint (from its `post` table).
* Every first-party C++ source under SCAN_ROOTS: each private-use codepoint written as a UTF-8
  escape (`\\xef\\x81\\xbc`), a universal character name (`\\uF07C`) or a raw character must be
  mapped by the font and lie inside the ranges Fonts.cpp loads.

The font is read with the standard library only (cmap formats 4 and 12, post format 2), so the
check runs wherever Python does. It refuses to pass vacuously: fewer definitions than the per-file
minimum, an empty cmap, or a missing `kIconRanges` declaration is an error.

Usage:
  python tools/ui/check_icon_glyphs.py check [--verbose]
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FONT = Path("SparkEditor/Fonts/fa-solid-900.ttf")
FONTS_CPP = Path("SparkUI/Source/Fonts.cpp")
# Definition file -> the fewest entries it may hold before the check counts as broken.
DEFINITION_FILES: dict[Path, int] = {
    Path("SparkEditor/Source/Core/EditorIcons.h"): 150,
    Path("SparkUI/Include/SparkUI/Glyphs.h"): 10,
}
SCAN_ROOTS = (
    Path("SparkEditor/Source"),
    Path("SparkUI"),
    Path("SparkLauncher/src"),
    Path("SparkInstaller/src"),
    Path("SparkEngine/Source"),
    Path("SparkConsole/src"),
    Path("SparkCrashReporter/src"),
    Path("GameModules"),
)
SOURCE_SUFFIXES = (".h", ".hpp", ".inl", ".cpp", ".cc", ".cxx")
PRIVATE_USE = (0xE000, 0xF8FF)

# Macintosh standard glyph order names for the post-table indices below 258 that an icon font can
# reference (the printable ASCII block, indices 0..97).
_MAC_STANDARD_NAMES = (
    [".notdef", ".null", "nonmarkingreturn", "space", "exclam", "quotedbl", "numbersign", "dollar",
     "percent", "ampersand", "quotesingle", "parenleft", "parenright", "asterisk", "plus", "comma",
     "hyphen", "period", "slash", "zero", "one", "two", "three", "four", "five", "six", "seven",
     "eight", "nine", "colon", "semicolon", "less", "equal", "greater", "question", "at"]
    + [chr(c) for c in range(ord("A"), ord("Z") + 1)]
    + ["bracketleft", "backslash", "bracketright", "asciicircum", "underscore", "grave"]
    + [chr(c) for c in range(ord("a"), ord("z") + 1)]
    + ["braceleft", "bar", "braceright", "asciitilde"]
)

DEFINE_LINE = re.compile(r'^\s*#define\s+(ICON_\w+)\s+"((?:[^"\\]|\\.)*)"\s*(?://\s*(.*))?$')
CONSTANT_LINE = re.compile(
    r'^\s*inline\s+constexpr\s+const\s+char\s*\*\s*(k\w+)\s*=\s*"((?:[^"\\]|\\.)*)"\s*;\s*(?:///<\s*(.*))?$')
COMMENT_CODEPOINT = re.compile(r"^U\+([0-9A-Fa-f]{4,6})\s+([a-z0-9][a-z0-9-]*)\b")
UTF8_ESCAPE_RUN = re.compile(r"(?:\\x[0-9A-Fa-f]{2})+")
UCN = re.compile(r"\\u([0-9A-Fa-f]{4})|\\U([0-9A-Fa-f]{8})")
ICON_RANGES = re.compile(r"\bkIconRanges\s*=\s*\{([^}]*)\}")


@dataclass
class IconFont:
    """Codepoint -> glyph name of the loaded icon font."""

    path: Path
    glyph_names: dict[int, str]


@dataclass
class Definition:
    path: Path
    line: int
    name: str
    codepoints: list[int]
    comment: str


@dataclass
class Result:
    definitions: int = 0
    uses: int = 0
    errors: list[str] = field(default_factory=list)


def _u16(data: bytes, offset: int) -> int:
    return struct.unpack_from(">H", data, offset)[0]


def _u32(data: bytes, offset: int) -> int:
    return struct.unpack_from(">I", data, offset)[0]


def _tables(data: bytes) -> dict[str, tuple[int, int]]:
    count = _u16(data, 4)
    tables = {}
    for i in range(count):
        tag, _checksum, offset, length = struct.unpack_from(">4sIII", data, 12 + 16 * i)
        tables[tag.decode("latin-1")] = (offset, length)
    return tables


def _cmap(data: bytes, offset: int) -> dict[int, int]:
    mapping: dict[int, int] = {}
    for i in range(_u16(data, offset + 2)):
        subtable = offset + _u32(data, offset + 4 + 8 * i + 4)
        fmt = _u16(data, subtable)
        if fmt == 4:
            segments = _u16(data, subtable + 6) // 2
            ends = subtable + 14
            starts = ends + 2 * segments + 2
            deltas = starts + 2 * segments
            range_offsets = deltas + 2 * segments
            for s in range(segments):
                start, end = _u16(data, starts + 2 * s), _u16(data, ends + 2 * s)
                delta = struct.unpack_from(">h", data, deltas + 2 * s)[0]
                range_offset = _u16(data, range_offsets + 2 * s)
                for code in range(start, end + 1):
                    if code == 0xFFFF:
                        continue
                    if range_offset == 0:
                        glyph = (code + delta) & 0xFFFF
                    else:
                        glyph = _u16(data, range_offsets + 2 * s + range_offset + 2 * (code - start))
                        glyph = (glyph + delta) & 0xFFFF if glyph else 0
                    if glyph:
                        mapping[code] = glyph
        elif fmt == 12:
            for group in range(_u32(data, subtable + 12)):
                first, last, glyph = struct.unpack_from(">III", data, subtable + 16 + 12 * group)
                for code in range(first, last + 1):
                    mapping[code] = glyph + code - first
    return mapping


def _post_names(data: bytes, offset: int, length: int) -> dict[int, str]:
    if _u32(data, offset) != 0x00020000:
        return {}
    glyph_count = _u16(data, offset + 32)
    indices = struct.unpack_from(f">{glyph_count}H", data, offset + 34)
    extra: list[str] = []
    cursor, end = offset + 34 + 2 * glyph_count, offset + length
    while cursor < end:
        size = data[cursor]
        extra.append(data[cursor + 1:cursor + 1 + size].decode("latin-1"))
        cursor += 1 + size
    names = {}
    for glyph, index in enumerate(indices):
        if index >= 258:
            names[glyph] = extra[index - 258]
        elif index < len(_MAC_STANDARD_NAMES):
            names[glyph] = _MAC_STANDARD_NAMES[index]
        else:
            names[glyph] = f"#{index}"
    return names


def read_icon_font(path: Path) -> IconFont:
    """Read the codepoint map and glyph names of a TrueType font."""
    data = path.read_bytes()
    tables = _tables(data)
    if "cmap" not in tables:
        raise ValueError(f"{path}: no cmap table")
    cmap = _cmap(data, tables["cmap"][0])
    names = _post_names(data, *tables["post"]) if "post" in tables else {}
    return IconFont(path, {code: names.get(glyph, f"glyph{glyph}") for code, glyph in cmap.items()})


def load_icon_ranges(fonts_cpp: Path) -> list[tuple[int, int]]:
    """The [first, last] pairs of Fonts.cpp's zero-terminated kIconRanges array."""
    match = ICON_RANGES.search(fonts_cpp.read_text(encoding="utf-8"))
    if match is None:
        raise ValueError(f"{fonts_cpp}: kIconRanges declaration not found")
    values = [int(token, 0) for token in re.findall(r"0[xX][0-9A-Fa-f]+|\d+", match.group(1))]
    if not values or values[-1] != 0 or len(values) % 2 != 1:
        raise ValueError(f"{fonts_cpp}: kIconRanges is not a zero-terminated list of pairs: {values}")
    return [(values[i], values[i + 1]) for i in range(0, len(values) - 1, 2)]


def _decode_escapes(literal: str) -> str:
    """Decode a C++ narrow string literal's \\x escapes as UTF-8 (other text kept as is)."""

    def utf8(match: re.Match[str]) -> str:
        raw = bytes(int(h, 16) for h in re.findall(r"\\x([0-9A-Fa-f]{2})", match.group(0)))
        return raw.decode("utf-8", errors="replace")

    text = UTF8_ESCAPE_RUN.sub(utf8, literal)
    return UCN.sub(lambda m: chr(int(m.group(1) or m.group(2), 16)), text)


def parse_definitions(path: Path) -> list[Definition]:
    """Every ICON_* define and kName constant with its decoded codepoints and comment."""
    definitions = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        match = DEFINE_LINE.match(line) or CONSTANT_LINE.match(line)
        if match is None:
            continue
        codepoints = [ord(ch) for ch in _decode_escapes(match.group(2))]
        definitions.append(Definition(path, number, match.group(1), codepoints, (match.group(3) or "").strip()))
    return definitions


def private_use_codepoints(line: str) -> list[int]:
    """Private-use codepoints a source line spells as escapes, UCNs or raw characters."""
    codepoints = [ord(ch) for ch in _decode_escapes(line)]
    return [cp for cp in codepoints if PRIVATE_USE[0] <= cp <= PRIVATE_USE[1]]


def _in_ranges(codepoint: int, ranges: list[tuple[int, int]]) -> bool:
    return any(first <= codepoint <= last for first, last in ranges)


def check(root: Path, font_path: Path | None = None, fonts_cpp: Path | None = None,
          definition_files: dict[Path, int] | None = None,
          scan_roots: tuple[Path, ...] | None = None) -> Result:
    """Run every rule; paths are relative to @p root unless absolute."""
    result = Result()
    font = read_icon_font(root / (font_path or FONT))
    if not font.glyph_names:
        result.errors.append(f"{font.path}: the font maps no codepoints")
        return result
    ranges = load_icon_ranges(root / (fonts_cpp or FONTS_CPP))

    for relative, minimum in (definition_files or DEFINITION_FILES).items():
        path = root / relative
        definitions = parse_definitions(path)
        result.definitions += len(definitions)
        if len(definitions) < minimum:
            result.errors.append(f"{relative.as_posix()}: found {len(definitions)} icon definitions, expected at least "
                                 f"{minimum}; the parser no longer matches the file")
        for definition in definitions:
            where = f"{relative.as_posix()}:{definition.line} {definition.name}"
            if len(definition.codepoints) != 1:
                result.errors.append(f"{where}: encodes {len(definition.codepoints)} codepoints, expected one")
                continue
            codepoint = definition.codepoints[0]
            actual = font.glyph_names.get(codepoint)
            comment = COMMENT_CODEPOINT.match(definition.comment)
            if comment is None:
                result.errors.append(f"{where}: comment must start with 'U+XXXX glyph-name' "
                                     f"(bytes encode U+{codepoint:04X} {actual or '<missing>'})")
            else:
                documented, documented_name = int(comment.group(1), 16), comment.group(2)
                if documented != codepoint:
                    result.errors.append(f"{where}: bytes encode U+{codepoint:04X} ({actual or 'missing'}) "
                                         f"but the comment says U+{documented:04X}")
                elif actual is not None and documented_name != actual:
                    result.errors.append(f"{where}: U+{codepoint:04X} is '{actual}' in {FONT.name}, "
                                         f"the comment says '{documented_name}'")

    for scan_root in scan_roots or SCAN_ROOTS:
        base = root / scan_root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for number, line in enumerate(text.splitlines(), start=1):
                for codepoint in private_use_codepoints(line):
                    result.uses += 1
                    where = f"{path.relative_to(root).as_posix()}:{number}"
                    if not _in_ranges(codepoint, ranges):
                        result.errors.append(f"{where}: U+{codepoint:04X} is outside the icon ranges "
                                             f"{FONTS_CPP.as_posix()} loads")
                    if codepoint not in font.glyph_names:
                        result.errors.append(f"{where}: U+{codepoint:04X} is not in {FONT.name}")
    if result.uses == 0:
        result.errors.append("no icon codepoint found in any scanned source; the scanner stopped scanning")
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("command", nargs="?", default="check", choices=["check"])
    parser.add_argument("--root", type=Path, default=ROOT, help="repository root (default: this checkout)")
    parser.add_argument("--verbose", action="store_true", help="list every definition with its glyph name")
    args = parser.parse_args(argv)

    result = check(args.root)
    if args.verbose:
        font = read_icon_font(args.root / FONT)
        for relative in DEFINITION_FILES:
            for definition in parse_definitions(args.root / relative):
                names = ", ".join(f"U+{cp:04X} {font.glyph_names.get(cp, '<missing>')}"
                                  for cp in definition.codepoints)
                print(f"{relative.as_posix()}:{definition.line} {definition.name}: {names}")
    for error in result.errors:
        print(f"error: {error}", file=sys.stderr)
    if result.errors:
        print(f"check_icon_glyphs: FAILED - {len(result.errors)} problem(s) in {result.definitions} icon "
              f"definitions and {result.uses} literal codepoint uses", file=sys.stderr)
        return 1
    print(f"check_icon_glyphs: OK - {result.definitions} icon definitions and {result.uses} literal codepoint "
          f"uses match {FONT.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
