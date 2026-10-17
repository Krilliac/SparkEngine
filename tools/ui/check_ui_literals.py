#!/usr/bin/env python3
"""Per-lane ratchet on raw UI literals in first-party ImGui code (Ember Signal spec, section 2.8).

UI geometry and colour must come from SparkUI tokens (`Spark::Ui::Px`, `Px2`, `Em`, `Col`,
`ColV`, `StatusText`), not from raw numbers. Every migrating lane owns one baseline file,
`tools/ui/literal-baselines/<LANE>.json`, that records how many raw literals each of its files
still holds. A count may only go down: the check fails when any listed file holds more literals
than its baseline, and a listed file that does not exist yet (a new file a lane will create)
has a baseline of zero.

Counted rules (comments and string literals are ignored):
  colour-imvec4    ImVec4 built from three or four numeric literals
  colour-im-col32  IM_COL32 whose first channel is a numeric literal
  item-width       SetNextItemWidth / PushItemWidth with a positive numeric literal
  column-width     a positive literal after ImGuiTableColumnFlags_WidthFixed
  size-imvec2      an all-literal, non-negative, non-zero ImVec2 on a Button, SmallButton,
                   InvisibleButton, BeginChild, Dummy or SetNextWindowSize call line
  spacing          SameLine or Indent with a positive numeric literal offset

Lines inside a named constexpr table listed under a baseline's `allowlisted_tables` are exempt
(spec section 4.2 step 3: simulated HUD and canvas art hoisted to named tables).

Usage:
  python tools/ui/check_ui_literals.py check [--lane P1] [--verbose]
  python tools/ui/check_ui_literals.py update --lane P1     # lower P1's counts to the current ones
  python tools/ui/check_ui_literals.py seed --lane X --files a.cpp b.h   # create a new baseline
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASELINE_DIR = ROOT / "tools" / "ui" / "literal-baselines"
SCHEMA_VERSION = 1
SOURCE_SUFFIXES = (".h", ".hpp", ".inl", ".cpp", ".cc", ".cxx")

NUMBER = r"[-+]?(?:\d+\.?\d*|\.\d+)f?"
# A literal with at least one non-zero digit and no sign.
POSITIVE = r"(?=[\d.]*[1-9])(?:\d+\.?\d*|\.\d+)f?"
NON_NEGATIVE = r"(?:\d+\.?\d*|\.\d+)f?"
SIZE_CALLS = re.compile(r"\b(?:Button|SmallButton|InvisibleButton|BeginChild|Dummy|SetNextWindowSize)\s*\(")

RULES: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("colour-imvec4", re.compile(
        rf"\bImVec4\s*\(\s*{NUMBER}\s*,\s*{NUMBER}\s*,\s*{NUMBER}\s*(?:,\s*{NUMBER}\s*)?\)")),
    ("colour-im-col32", re.compile(r"\bIM_COL32\s*\(\s*\d")),
    ("item-width", re.compile(rf"\b(?:SetNextItemWidth|PushItemWidth)\s*\(\s*{POSITIVE}\s*\)")),
    ("column-width", re.compile(rf"\bImGuiTableColumnFlags_WidthFixed\s*,\s*{POSITIVE}\s*[,)]")),
    ("spacing", re.compile(
        rf"\bSameLine\s*\(\s*(?:{NON_NEGATIVE}\s*,\s*{POSITIVE}|{POSITIVE})\s*\)|\bIndent\s*\(\s*{POSITIVE}\s*\)")),
)
SIZE_IMVEC2 = re.compile(rf"\bImVec2\s*\(\s*({NON_NEGATIVE})\s*,\s*({NON_NEGATIVE})\s*\)")


class LiteralCheckError(Exception):
    """A malformed baseline or an unreadable tree; reported as a failure, never as a pass."""


@dataclass(frozen=True)
class Site:
    line: int
    rule: str
    text: str


@dataclass
class Baseline:
    lane: str
    path: Path
    files: dict[str, int]
    allowlisted_tables: dict[str, list[str]] = field(default_factory=dict)
    description: str = ""


def strip_comments_and_strings(text: str) -> str:
    """Blank comments and string/char literal contents, keeping every newline in place."""
    out: list[str] = []
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        pair = text[index:index + 2]
        if pair == "//":
            end = text.find("\n", index)
            end = length if end == -1 else end - (1 if text[end - 1] == "\r" else 0)
            out.append(" " * (end - index))
            index = end
        elif pair == "/*":
            end = text.find("*/", index + 2)
            end = length if end == -1 else end + 2
            out.append(re.sub(r"[^\r\n]", " ", text[index:end]))
            index = end
        elif text.startswith('R"', index) and (index == 0 or not (text[index - 1].isalnum() or text[index - 1] == "_")):
            open_paren = text.find("(", index + 2)
            delimiter = text[index + 2:open_paren] if open_paren != -1 else ""
            closing = ")" + delimiter + '"'
            end = text.find(closing, open_paren + 1) if open_paren != -1 else -1
            end = length if end == -1 else end + len(closing)
            out.append('""' + re.sub(r"[^\r\n]", " ", text[index + 2:end]))
            index = end
        elif char in "\"'":
            end = index + 1
            while end < length and text[end] != char and text[end] not in "\r\n":
                end += 2 if text[end] == "\\" else 1
            closed = end < length and text[end] == char
            body = re.sub(r"[^\r\n]", " ", text[index + 1:min(end, length)])
            # Keep both quotes so stripping an already stripped text changes nothing.
            out.append(char + body + (char if closed else ""))
            end = end + 1 if closed else min(end, length)
            index = end
        else:
            out.append(char)
            index += 1
    return "".join(out)


def allowlisted_lines(lines: list[str], tables: list[str]) -> set[int]:
    """1-based numbers of the lines inside the named constexpr tables."""
    if not tables:
        return set()
    starts = re.compile(r"\bconstexpr\b.*\b(?:" + "|".join(re.escape(name) for name in tables) + r")\b")
    exempt: set[int] = set()
    inside = False
    depth = 0
    opened = False
    for number, line in enumerate(lines, start=1):
        if not inside and starts.search(line):
            inside, depth, opened = True, 0, False
        if not inside:
            continue
        exempt.add(number)
        for char in line:
            if char == "{":
                depth += 1
                opened = True
            elif char == "}":
                depth -= 1
        if opened and depth <= 0 and ";" in line:
            inside = False
    return exempt


def scan_text(text: str, tables: list[str] | None = None) -> list[Site]:
    """Every counted literal in @p text, in line order."""
    raw_lines = text.splitlines()
    lines = strip_comments_and_strings(text).splitlines()
    exempt = allowlisted_lines(lines, tables or [])
    sites: list[Site] = []
    for number, line in enumerate(lines, start=1):
        if number in exempt:
            continue
        shown = raw_lines[number - 1].strip() if number - 1 < len(raw_lines) else ""
        for rule, pattern in RULES:
            sites.extend(Site(number, rule, shown) for _ in pattern.finditer(line))
        if SIZE_CALLS.search(line):
            for match in SIZE_IMVEC2.finditer(line):
                if float(match.group(1).rstrip("f")) > 0 or float(match.group(2).rstrip("f")) > 0:
                    sites.append(Site(number, "size-imvec2", shown))
    return sites


def scan_file(root: Path, relative: str, tables: list[str] | None = None) -> list[Site] | None:
    """Sites in one file, or None when the file does not exist (a new file has a zero baseline)."""
    path = root / relative
    if not path.is_file():
        return None
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        raise LiteralCheckError(f"{relative}: unreadable: {exc}") from exc
    return scan_text(text, tables)


def _require_relative(value: object, where: str) -> str:
    if not isinstance(value, str) or not value or value.startswith("/") or "\\" in value or ".." in value.split("/"):
        raise LiteralCheckError(f"{where}: {value!r} is not a repo-relative POSIX path")
    if not value.endswith(SOURCE_SUFFIXES):
        raise LiteralCheckError(f"{where}: {value!r} is not a C++ source file")
    return value


def load_baseline(path: Path) -> Baseline:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise LiteralCheckError(f"{path.name}: unreadable baseline: {exc}") from exc
    if not isinstance(data, dict) or data.get("schema_version") != SCHEMA_VERSION:
        raise LiteralCheckError(f"{path.name}: schema_version must be {SCHEMA_VERSION}")
    lane = data.get("lane")
    if lane != path.stem:
        raise LiteralCheckError(f"{path.name}: lane {lane!r} does not match the file name")
    files = data.get("files")
    if not isinstance(files, dict) or not files:
        raise LiteralCheckError(f"{path.name}: files must be a non-empty object")
    checked: dict[str, int] = {}
    for relative, count in files.items():
        _require_relative(relative, path.name)
        if not isinstance(count, int) or isinstance(count, bool) or count < 0:
            raise LiteralCheckError(f"{path.name}: {relative} count must be a non-negative integer")
        checked[relative] = count
    if data.get("total") != sum(checked.values()):
        raise LiteralCheckError(f"{path.name}: total must equal the sum of the file counts")
    tables = data.get("allowlisted_tables", {})
    if not isinstance(tables, dict):
        raise LiteralCheckError(f"{path.name}: allowlisted_tables must be an object")
    for relative, names in tables.items():
        if relative not in checked:
            raise LiteralCheckError(f"{path.name}: allowlisted_tables names unlisted file {relative}")
        if not isinstance(names, list) or not all(isinstance(n, str) and re.fullmatch(r"[A-Za-z_]\w*", n) for n in names):
            raise LiteralCheckError(f"{path.name}: allowlisted_tables[{relative}] must list C++ identifiers")
    return Baseline(lane, path, checked, {k: list(v) for k, v in tables.items()}, str(data.get("description", "")))


def load_baselines(directory: Path) -> list[Baseline]:
    paths = sorted(directory.glob("*.json"))
    if not paths:
        raise LiteralCheckError(f"no baselines in {directory}")
    baselines = [load_baseline(path) for path in paths]
    owners: dict[str, str] = {}
    for baseline in baselines:
        for relative in baseline.files:
            if relative in owners:
                raise LiteralCheckError(f"{relative} is listed by both {owners[relative]} and {baseline.lane}")
            owners[relative] = baseline.lane
    return baselines


def write_baseline(baseline: Baseline) -> None:
    payload = {
        "schema_version": SCHEMA_VERSION,
        "lane": baseline.lane,
        "description": baseline.description,
        "allowlisted_tables": {k: baseline.allowlisted_tables[k] for k in sorted(baseline.allowlisted_tables)},
        "total": sum(baseline.files.values()),
        "files": {k: baseline.files[k] for k in sorted(baseline.files)},
    }
    baseline.path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8", newline="\n")


@dataclass
class CheckResult:
    scanned: int = 0
    missing: int = 0
    total: int = 0
    regressions: list[str] = field(default_factory=list)
    lowerable: list[str] = field(default_factory=list)


def check(root: Path, baselines: list[Baseline], *, verbose: bool = False, out=sys.stdout) -> CheckResult:
    result = CheckResult()
    for baseline in baselines:
        lane_total = 0
        for relative, allowed in sorted(baseline.files.items()):
            sites = scan_file(root, relative, baseline.allowlisted_tables.get(relative))
            if sites is None:
                result.missing += 1
                continue
            result.scanned += 1
            count = len(sites)
            lane_total += count
            if count > allowed:
                result.regressions.append(f"{baseline.lane}: {relative} holds {count} UI literals, baseline {allowed}")
                for site in sites:
                    print(f"  {relative}:{site.line}: [{site.rule}] {site.text}", file=out)
            elif count < allowed:
                result.lowerable.append(f"{baseline.lane}: {relative} {allowed} -> {count}")
            if verbose and count:
                for site in sites:
                    print(f"  {relative}:{site.line}: [{site.rule}] {site.text}", file=out)
        result.total += lane_total
        print(f"{baseline.lane}: {lane_total} UI literals (baseline {sum(baseline.files.values())})", file=out)
    return result


def _cmd_check(args: argparse.Namespace) -> int:
    baselines = load_baselines(args.baselines)
    if args.lane:
        baselines = [b for b in baselines if b.lane == args.lane]
        if not baselines:
            raise LiteralCheckError(f"no baseline for lane {args.lane}")
    result = check(args.root, baselines, verbose=args.verbose)
    print(f"scanned {result.scanned} files ({result.missing} listed but not present) across "
          f"{len(baselines)} lanes; {result.total} UI literals")
    for note in result.lowerable:
        print(f"note: baseline can be lowered: {note} (run: update --lane {note.split(':', 1)[0]})")
    if result.scanned == 0:
        print("FAIL: scanned no files", file=sys.stderr)
        return 1
    if args.min_files and result.scanned < args.min_files:
        print(f"FAIL: scanned {result.scanned} files, expected at least {args.min_files}", file=sys.stderr)
        return 1
    for regression in result.regressions:
        print(f"FAIL: {regression}", file=sys.stderr)
    return 1 if result.regressions else 0


def _cmd_update(args: argparse.Namespace) -> int:
    baseline = next((b for b in load_baselines(args.baselines) if b.lane == args.lane), None)
    if baseline is None:
        raise LiteralCheckError(f"no baseline for lane {args.lane}")
    changed = False
    for relative, allowed in sorted(baseline.files.items()):
        sites = scan_file(args.root, relative, baseline.allowlisted_tables.get(relative))
        count = 0 if sites is None else len(sites)
        if count > allowed:
            print(f"FAIL: {relative} holds {count} UI literals, above baseline {allowed}; counts only go down",
                  file=sys.stderr)
            return 1
        if count < allowed:
            print(f"{relative}: {allowed} -> {count}")
            baseline.files[relative] = count
            changed = True
    if changed:
        write_baseline(baseline)
    print(f"{baseline.lane}: {sum(baseline.files.values())} UI literals")
    return 0


def _cmd_seed(args: argparse.Namespace) -> int:
    path = args.baselines / f"{args.lane}.json"
    if path.exists():
        print(f"FAIL: {path.name} exists; baselines are seeded once and only lowered afterwards", file=sys.stderr)
        return 1
    if not re.fullmatch(r"[A-Z][A-Z0-9]{0,7}", args.lane):
        raise LiteralCheckError(f"lane {args.lane!r} must be a short upper-case lane id")
    files: dict[str, int] = {}
    for relative in args.files:
        _require_relative(relative, "--files")
        sites = scan_file(args.root, relative)
        files[relative] = 0 if sites is None else len(sites)
    baseline = Baseline(args.lane, path, files, {}, args.description)
    args.baselines.mkdir(parents=True, exist_ok=True)
    write_baseline(baseline)
    print(f"{args.lane}: seeded {len(files)} files, {sum(files.values())} UI literals")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--baselines", type=Path, default=BASELINE_DIR)
    sub = parser.add_subparsers(dest="command", required=True)
    check_parser = sub.add_parser("check")
    check_parser.add_argument("--lane")
    check_parser.add_argument("--verbose", action="store_true")
    check_parser.add_argument("--min-files", type=int, default=0)
    update_parser = sub.add_parser("update")
    update_parser.add_argument("--lane", required=True)
    seed_parser = sub.add_parser("seed")
    seed_parser.add_argument("--lane", required=True)
    seed_parser.add_argument("--description", default="")
    seed_parser.add_argument("--files", nargs="+", required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "check":
            return _cmd_check(args)
        if args.command == "update":
            return _cmd_update(args)
        return _cmd_seed(args)
    except LiteralCheckError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
