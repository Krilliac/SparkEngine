#!/usr/bin/env python3
"""Exact-match codemod from raw UI literals to SparkUI tokens (Ember Signal spec, section 4.2 step 1).

Only the fixed table below is rewritten; every other literal is left for the lane's hand pass
and listed as "ambiguous". A line that holds any string the play-truth guard
(Tests/Tools/test_editor_play_truth.py) asserts on is never touched and is listed as
"skipped". Without --apply the script only reports what it would change.

Table:
  ImVec4(0.6,0.6,0.6,1)                    -> ColV(Role::TextSecondary)
  ImVec4(0.5,0.5,0.5,1)                    -> ColV(Role::TextDisabled)
  green  (0.2,0.8,0.2,1) (0.3,0.9,0.3,1)   -> ColV(Role::SuccessText)
  reds   (1,.3,.3) (.9,.3,.3) (1,.4,.4) (.9,.2,.2) (1,.2,.2), alpha 1 -> ColV(Role::ErrorText)
  ambers (1,.8,0) (1,.6,0) (1,.8,.2), alpha 1                         -> ColV(Role::WarningText)
  ImGui::TextColored(<green|red|amber>, ...) -> Spark::Ui::StatusText(Status::<family>, ...)
  IM_COL32(255,255,255,200)                -> Col(Role::TextPrimary, .78f)
  SetNextItemWidth(N) / PushItemWidth(N)   -> ...(Spark::Ui::Px(N))         (N > 0)
  ImGuiTableColumnFlags_WidthFixed, N      -> ..., Spark::Ui::Px(N)          (N > 0)
  ImVec2(a, b) on a Button/SmallButton/InvisibleButton/BeginChild/Dummy/SetNextWindowSize
  line, a and b non-negative literals, not both zero -> Spark::Ui::Px2(a, b)

Usage:
  python tools/ui/migrate_ui_literals.py --files <lane files...>            # report only
  python tools/ui/migrate_ui_literals.py --files <lane files...> --apply    # rewrite in place
"""

from __future__ import annotations

import argparse
import ast
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_ui_literals as literals  # noqa: E402  (sibling module, path set above)

ROOT = literals.ROOT
PLAY_TRUTH_TEST = ROOT / "Tests" / "Tools" / "test_editor_play_truth.py"
MIN_GUARD_CHARS = 6

GREY = {(0.6, 0.6, 0.6, 1.0): "TextSecondary", (0.5, 0.5, 0.5, 1.0): "TextDisabled"}
STATUS_FAMILIES = {
    "Success": ((0.2, 0.8, 0.2), (0.3, 0.9, 0.3)),
    "Error": ((1.0, 0.3, 0.3), (0.9, 0.3, 0.3), (1.0, 0.4, 0.4), (0.9, 0.2, 0.2), (1.0, 0.2, 0.2)),
    "Warning": ((1.0, 0.8, 0.0), (1.0, 0.6, 0.0), (1.0, 0.8, 0.2)),
}

NUM = r"([-+]?(?:\d+\.?\d*|\.\d+))f?"
IMVEC4 = re.compile(rf"\bImVec4\s*\(\s*{NUM}\s*,\s*{NUM}\s*,\s*{NUM}\s*,\s*{NUM}\s*\)")
TEXT_COLORED = re.compile(rf"\bImGui::TextColored\s*\(\s*{IMVEC4.pattern[2:]}\s*,\s*")
WHITE_78 = re.compile(r"\bIM_COL32\s*\(\s*255\s*,\s*255\s*,\s*255\s*,\s*200\s*\)")
ITEM_WIDTH = re.compile(rf"\b(SetNextItemWidth|PushItemWidth)\s*\(\s*({literals.POSITIVE})\s*\)")
COLUMN_WIDTH = re.compile(rf"\b(ImGuiTableColumnFlags_WidthFixed\s*,\s*)({literals.POSITIVE})(\s*[,)])")
SIZE = re.compile(rf"\bImVec2\s*\(\s*({literals.NON_NEGATIVE})\s*,\s*({literals.NON_NEGATIVE})\s*\)")


def play_truth_strings(path: Path = PLAY_TRUTH_TEST) -> list[str]:
    """Every string literal the play-truth guard asserts on (long enough to be distinctive)."""
    tree = ast.parse(path.read_text(encoding="utf-8"))
    found = {node.value for node in ast.walk(tree)
             if isinstance(node, ast.Constant) and isinstance(node.value, str) and len(node.value) >= MIN_GUARD_CHARS}
    return sorted(found)


def _family(values: tuple[float, ...]) -> str | None:
    if len(values) != 4 or values[3] != 1.0:
        return None
    for family, members in STATUS_FAMILIES.items():
        if values[:3] in members:
            return family
    return None


def _values(match: re.Match[str]) -> tuple[float, ...]:
    return tuple(float(group) for group in match.groups()[:4])


def _role_for(values: tuple[float, ...]) -> str | None:
    if values in GREY:
        return GREY[values]
    family = _family(values)
    return f"{family}Text" if family else None


def migrate_line(line: str) -> tuple[str, set[str]]:
    """Rewrite one line by the exact table; returns the new line and the SparkUI headers it needs."""
    needs: set[str] = set()

    def text_colored(match: re.Match[str]) -> str:
        family = _family(_values(match))
        if family is None:
            return match.group(0)
        needs.add("SparkUI/Widgets.h")
        return f"Spark::Ui::StatusText(Spark::Ui::Status::{family}, "

    line = TEXT_COLORED.sub(text_colored, line)

    def imvec4(match: re.Match[str]) -> str:
        role = _role_for(_values(match))
        if role is None:
            return match.group(0)
        needs.add("SparkUI/Runtime.h")
        return f"Spark::Ui::ColV(Spark::Ui::Role::{role})"

    line = IMVEC4.sub(imvec4, line)
    if WHITE_78.search(line):
        needs.add("SparkUI/Runtime.h")
        line = WHITE_78.sub("Spark::Ui::Col(Spark::Ui::Role::TextPrimary, .78f)", line)
    if ITEM_WIDTH.search(line):
        needs.add("SparkUI/Runtime.h")
        line = ITEM_WIDTH.sub(lambda m: f"{m.group(1)}(Spark::Ui::Px({m.group(2)}))", line)
    if COLUMN_WIDTH.search(line):
        needs.add("SparkUI/Runtime.h")
        line = COLUMN_WIDTH.sub(lambda m: f"{m.group(1)}Spark::Ui::Px({m.group(2)}){m.group(3)}", line)
    if literals.SIZE_CALLS.search(line):
        def size(match: re.Match[str]) -> str:
            if float(match.group(1).rstrip("f")) == 0 and float(match.group(2).rstrip("f")) == 0:
                return match.group(0)
            needs.add("SparkUI/Runtime.h")
            return f"Spark::Ui::Px2({match.group(1)}, {match.group(2)})"

        line = SIZE.sub(size, line)
    return line, needs


def _add_includes(text: str, headers: set[str], newline: str) -> str:
    missing = [h for h in sorted(headers) if f'#include "{h}"' not in text]
    if not missing:
        return text
    lines = text.split(newline)
    last = max((i for i, line in enumerate(lines) if line.lstrip().startswith("#include")), default=None)
    block = [f'#include "{h}"' for h in missing]
    if last is None:
        pragma = next((i for i, line in enumerate(lines) if line.strip() == "#pragma once"), -1)
        lines[pragma + 1:pragma + 1] = ["", *block]
    else:
        lines[last + 1:last + 1] = block
    return newline.join(lines)


def _trailing_comment_start(line: str, code: str) -> int:
    """Index of a trailing // comment in @p line (its stripped form @p code is blank from there), or -1."""
    for index in range(len(line) - 1):
        if line.startswith("//", index) and not code[index:].strip():
            return index
    return -1


def migrate_text(text: str, guards: list[str], tables: list[str] | None = None) -> tuple[str, list[str], list[str], int]:
    """Returns (new text, skipped sites, ambiguous sites, rewritten line count)."""
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(newline)
    stripped = literals.strip_comments_and_strings(text).split(newline)
    exempt = literals.allowlisted_lines(stripped, tables or [])
    skipped: list[str] = []
    ambiguous: list[str] = []
    headers: set[str] = set()
    rewritten = 0
    for index, line in enumerate(lines):
        number = index + 1
        code = stripped[index] if index < len(stripped) else ""
        if number in exempt or not literals.scan_text(code):
            continue
        if any(guard in line for guard in guards):
            skipped.append(f"{number}: {line.strip()}")
            continue
        # Rewrite only the code part; a trailing // comment is carried over untouched.
        comment_at = _trailing_comment_start(line, code)
        head, tail = (line[:comment_at], line[comment_at:]) if comment_at >= 0 else (line, "")
        new_head, needs = migrate_line(head)
        if new_head != head:
            lines[index] = new_head + tail
            headers |= needs
            rewritten += 1
        for site in literals.scan_text(literals.strip_comments_and_strings(lines[index])):
            ambiguous.append(f"{number}: [{site.rule}] {line.strip()}")
    return _add_includes(newline.join(lines), headers, newline), skipped, ambiguous, rewritten


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--files", nargs="+", required=True)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--allow-table", action="append", default=[],
                        help="name of a constexpr table to leave untouched (repeatable)")
    args = parser.parse_args(argv)

    guards = play_truth_strings()
    total = 0
    for relative in args.files:
        path = args.root / relative
        if not path.is_file():
            print(f"{relative}: missing", file=sys.stderr)
            return 1
        original = path.read_bytes().decode("utf-8")
        updated, skipped, ambiguous, rewritten = migrate_text(original, guards, args.allow_table)
        total += rewritten
        print(f"{relative}: {rewritten} lines rewritten, {len(skipped)} skipped, {len(ambiguous)} ambiguous")
        for site in skipped:
            print(f"  skipped (play-truth) {site}")
        for site in ambiguous:
            print(f"  ambiguous {site}")
        if args.apply and updated != original:
            path.write_bytes(updated.encode("utf-8"))
    print(f"{'rewrote' if args.apply else 'would rewrite'} {total} lines in {len(args.files)} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
