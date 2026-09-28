#!/usr/bin/env python3
"""Fail-closed check that public docs label each game module as what it is (MOD-300/340/360).

A module's public label is what README.md, GameModules/README.md, the module's own
README and the wiki say it is. Each rule in ``RULES`` names a module, the labels it
must never carry and the disclaimers its README must keep. For every standalone
mention of the module name (``SparkGame`` but not ``SparkGameFPS``, a path such as
``GameModules/SparkGame/`` or a file such as ``SparkGame.dll``) the checker reads
the claim made about it:

* in a Markdown table row whose first cell names the module, the rest of the row;
* anywhere else, the text after the mention up to the end of its clause
  (``.``, ``;``, ``(``, ``|`` or `` and ``).

A claim that matches a forbidden label fails unless it negates being a game
("not a game", "not a finished game"). Findings print as ``path:line: message`` and
the exit status is non-zero when there are any.

Usage:
    python3 tools/check-module-positioning.py [--root <repo>]
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

_GAME_CLAIM = r"\b(?:finished|complete|completed|playable|shippable|release-ready|released)\s+game\b"


@dataclass(frozen=True)
class PositioningRule:
    module: str
    work_item: str
    forbidden: tuple[tuple[str, str], ...]  # (regex, why it is wrong)
    required: tuple[tuple[str, str], ...]  # (repo-relative path, exact phrase)


RULES: tuple[PositioningRule, ...] = (
    PositioningRule(
        module="SparkGame",
        work_item="MOD-300",
        forbidden=(
            (_GAME_CLAIM, "SparkGame is an engine-systems showcase, not a game"),
            (r"\bFPS\b|\bfirst-person\b|\bshooter\b|\barena\b", "the FPS arena is SparkGameFPS, not SparkGame"),
        ),
        required=(
            ("GameModules/SparkGame/README.md", "it is not a game"),
            ("GameModules/SparkGame/README.md", "**Release classification:** experimental showcase"),
        ),
    ),
    PositioningRule(
        module="SparkGamePlatformer",
        work_item="MOD-340",
        forbidden=((_GAME_CLAIM, "SparkGamePlatformer is an experimental prototype, not a finished game"),),
        required=(
            ("GameModules/SparkGamePlatformer/README.md", "**Release classification:** experimental prototype"),
            ("GameModules/SparkGamePlatformer/README.md", "not a packaged run"),
        ),
    ),
    PositioningRule(
        module="SparkGameOpenWorld",
        work_item="MOD-360",
        forbidden=((_GAME_CLAIM, "SparkGameOpenWorld is an experimental prototype, not a finished game"),),
        required=(
            ("GameModules/SparkGameOpenWorld/README.md", "**Release classification:** experimental prototype"),
            ("GameModules/SparkGameOpenWorld/README.md", "not a packaged run"),
        ),
    ),
)

_NEGATED_GAME = re.compile(r"\bnot\s+(?:an?\s+)?(?:[\w-]+\s+)?game\b", re.IGNORECASE)
_CLAUSE_END = re.compile(r"[.;(|]|\sand\s")


def scanned_files(root: Path) -> list[Path]:
    """Public docs that carry module labels: root and module READMEs plus the wiki."""
    files = [root / "README.md", root / "GameModules" / "README.md"]
    files += sorted((root / "GameModules").glob("*/README.md"))
    files += sorted((root / "wiki").rglob("*.md"))
    return [path for path in files if path.is_file()]


def _mention_pattern(module: str) -> re.Pattern[str]:
    return re.compile(rf"(?<![\w/.-]){re.escape(module)}(?![\w/-]|\.\w)")


def claim_spans(line: str, mention: re.Pattern[str]) -> list[str]:
    """Text each standalone mention of the module makes a claim with."""
    spans: list[str] = []
    first_cell_end = -1
    if line.lstrip().startswith("|"):
        first_cell_end = line.find("|", line.index("|") + 1)
    for match in mention.finditer(line):
        if match.start() < first_cell_end:
            spans.append(line[first_cell_end + 1 :])
            continue
        rest = line[match.end():]
        end = _CLAUSE_END.search(rest)
        spans.append(rest[: end.start()] if end else rest)
    return spans


def check(root: Path, rules: tuple[PositioningRule, ...] = RULES) -> list[str]:
    findings: list[str] = []
    for rule in rules:
        mention = _mention_pattern(rule.module)
        forbidden = [(re.compile(pattern, re.IGNORECASE), why) for pattern, why in rule.forbidden]
        for path in scanned_files(root):
            relative = path.relative_to(root).as_posix()
            for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                for span in claim_spans(line, mention):
                    if _NEGATED_GAME.search(span):
                        continue
                    for pattern, why in forbidden:
                        hit = pattern.search(span)
                        if hit:
                            findings.append(
                                f"{relative}:{number}: {rule.work_item} forbidden label '{hit.group(0)}' for "
                                f"{rule.module}: {why}"
                            )
        for relative, phrase in rule.required:
            path = root / relative
            text = path.read_text(encoding="utf-8", errors="replace") if path.is_file() else ""
            if phrase not in text:
                findings.append(f"{relative}:1: {rule.work_item} {rule.module} disclaimer missing: '{phrase}'")
    return findings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=REPO_ROOT)
    args = parser.parse_args(argv)
    findings = check(args.root.resolve())
    for finding in findings:
        print(finding)
    if not findings:
        print(f"module positioning: {len(RULES)} rule(s) clean")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
