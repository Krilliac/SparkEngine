#!/usr/bin/env python3
"""Validate ownership and expiry metadata for flaky-test waivers.

Two waiver kinds are policed:

* whole-test waivers: name patterns in Tests/TestWarnings.h, which must match
  the metadata ``waivers`` list exactly;
* per-assertion waivers: every ``EXPECT_WARN_ONLY`` call site in Tests/**/*.cpp,
  which must be covered by an ``assertionWaivers`` entry keyed by file and test
  name with the exact number of call sites in that test.

Both kinds need a named owner and a future ISO expiry. The runner-semantics
probes in Tests/TestRunnerSemanticsReal.cpp exercise the macro itself and are
the only exempt call sites.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from datetime import date
from pathlib import Path


MAX_WAIVERS = 256
OWNER_RE = re.compile(r"^[A-Za-z][A-Za-z0-9._/-]{1,63}$")
PLACEHOLDER_OWNERS = {"none", "n/a", "tbd", "todo", "unassigned", "unknown"}
WARN_ONLY_RE = re.compile(r"\bEXPECT_WARN_ONLY\s*\(")
TEST_HEADER_RE = re.compile(
    r"(?<![A-Za-z0-9_])(TEST|TEST_F)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?:,\s*([A-Za-z_][A-Za-z0-9_]*)\s*)?\)"
)
# The runner-semantics suite deliberately fires EXPECT_WARN_ONLY to prove the
# runner's accounting; those probes are not environment waivers.
RUNNER_PROBE_FILE = "Tests/TestRunnerSemanticsReal.cpp"
RUNNER_PROBE_PREFIX = "RunnerSemanticsReal_"
TOKEN_RE = re.compile(r"[0-9][0-9A-Za-z_.']*|[A-Za-z_][A-Za-z0-9_]*")
RAW_STRING_PREFIXES = {"R", "LR", "uR", "UR", "u8R"}
ASSERTION_KEYS = {"file", "test", "sites", "owner", "expires"}
ENTRY_RE = re.compile(
    r'\{\s*"((?:\\.|[^"\\])*)"\s*,\s*"((?:\\.|[^"\\])*)"\s*\}',
    re.DOTALL,
)


class PolicyError(ValueError):
    """A repository policy or schema violation."""


def parse_cpp_string(value: str) -> str:
    try:
        return json.loads(f'"{value}"')
    except json.JSONDecodeError as exc:
        raise PolicyError(f"invalid C++ string literal in TestWarnings.h: {exc}") from exc


def load_registry(path: Path) -> list[str]:
    try:
        source = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise PolicyError(f"cannot read registry {path}: {exc}") from exc

    marker = "g_testWarningPatterns[]"
    marker_index = source.find(marker)
    if marker_index < 0:
        raise PolicyError("TestWarnings.h does not declare g_testWarningPatterns")
    opening = source.find("=", marker_index)
    closing = source.find("};", opening)
    if opening < 0 or closing < 0:
        raise PolicyError("TestWarnings.h warning registry has no complete initializer")

    patterns = [parse_cpp_string(match.group(1)) for match in ENTRY_RE.finditer(source[opening:closing])]
    if not patterns:
        raise PolicyError("TestWarnings.h warning registry is empty")
    if len(patterns) > MAX_WAIVERS:
        raise PolicyError(f"warning registry has {len(patterns)} entries; maximum is {MAX_WAIVERS}")

    duplicates = sorted({pattern for pattern in patterns if patterns.count(pattern) > 1})
    if duplicates:
        raise PolicyError(f"duplicate pattern in TestWarnings.h: {', '.join(duplicates)}")
    if any(not pattern.strip() for pattern in patterns):
        raise PolicyError("TestWarnings.h contains an empty warning pattern")
    return patterns


def validate_owner_and_expiry(label: str, owner: str, expires: str, as_of: date) -> None:
    if not OWNER_RE.fullmatch(owner) or owner.lower() in PLACEHOLDER_OWNERS:
        raise PolicyError(f"{label} owner must be a named owner, got {owner!r}")
    try:
        expiry = date.fromisoformat(expires)
    except ValueError as exc:
        raise PolicyError(f"{label} expires must be an ISO date (YYYY-MM-DD)") from exc
    if expiry.isoformat() != expires:
        raise PolicyError(f"{label} expires must use YYYY-MM-DD")
    if expiry <= as_of:
        raise PolicyError(f"{label} is expired as of {as_of.isoformat()}")


def load_payload(path: Path) -> dict:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise PolicyError(f"cannot read warning metadata {path}: {exc}") from exc
    if not isinstance(payload, dict) or "schemaVersion" not in payload:
        raise PolicyError("warning metadata must be an object with schemaVersion")
    version = payload["schemaVersion"]
    # Schema 1 (whole-test waivers only) stays readable; it simply declares no
    # per-assertion waivers, so any EXPECT_WARN_ONLY site fails as unregistered.
    expected_keys = {1: {"schemaVersion", "waivers"}, 2: {"schemaVersion", "waivers", "assertionWaivers"}}
    if isinstance(version, bool) or version not in expected_keys:
        raise PolicyError("warning metadata schemaVersion must be 1 or 2")
    if set(payload) != expected_keys[version]:
        names = ", ".join(sorted(expected_keys[version]))
        raise PolicyError(f"warning metadata schemaVersion {version} must contain exactly {names}")
    return payload


def load_metadata(payload: dict, as_of: date) -> dict[str, dict[str, str]]:
    waivers = payload["waivers"]
    if not isinstance(waivers, list):
        raise PolicyError("warning metadata waivers must be a list")
    if len(waivers) > MAX_WAIVERS:
        raise PolicyError(f"warning metadata has {len(waivers)} entries; maximum is {MAX_WAIVERS}")

    result: dict[str, dict[str, str]] = {}
    for index, entry in enumerate(waivers):
        label = f"warning metadata waivers[{index}]"
        if not isinstance(entry, dict) or set(entry) != {"pattern", "owner", "expires"}:
            raise PolicyError(f"{label} must contain exactly pattern, owner, and expires")
        pattern, owner, expires = (entry[key] for key in ("pattern", "owner", "expires"))
        if not all(isinstance(value, str) for value in (pattern, owner, expires)):
            raise PolicyError(f"{label} pattern, owner, and expires must be strings")
        if not pattern.strip():
            raise PolicyError(f"{label} pattern must not be empty")
        if pattern in result:
            raise PolicyError(f"duplicate pattern in warning metadata: {pattern}")
        validate_owner_and_expiry(label, owner, expires, as_of)
        result[pattern] = {"owner": owner, "expires": expires}
    return result


def load_assertion_metadata(payload: dict, as_of: date) -> dict[tuple[str, str], int]:
    entries = payload.get("assertionWaivers", [])
    if not isinstance(entries, list):
        raise PolicyError("warning metadata assertionWaivers must be a list")
    if len(entries) > MAX_WAIVERS:
        raise PolicyError(f"warning metadata has {len(entries)} assertion waivers; maximum is {MAX_WAIVERS}")

    result: dict[tuple[str, str], int] = {}
    for index, entry in enumerate(entries):
        label = f"warning metadata assertionWaivers[{index}]"
        if not isinstance(entry, dict) or set(entry) != ASSERTION_KEYS:
            raise PolicyError(f"{label} must contain exactly file, test, sites, owner, and expires")
        file_name, test_name, owner, expires = (entry[key] for key in ("file", "test", "owner", "expires"))
        if not all(isinstance(value, str) for value in (file_name, test_name, owner, expires)):
            raise PolicyError(f"{label} file, test, owner, and expires must be strings")
        sites = entry["sites"]
        if isinstance(sites, bool) or not isinstance(sites, int) or sites < 1:
            raise PolicyError(f"{label} sites must be a positive integer")
        if not file_name.strip() or not test_name.strip():
            raise PolicyError(f"{label} file and test must not be empty")
        key = (file_name, test_name)
        if key in result:
            raise PolicyError(f"duplicate assertion waiver for {file_name}::{test_name}")
        validate_owner_and_expiry(label, owner, expires, as_of)
        result[key] = sites
    return result


def strip_comments_and_literals(source: str) -> str:
    """Blank C++ comments and string/char literals, preserving offsets and newlines."""
    out = list(source)
    index = 0
    length = len(source)

    def blank(start: int, end: int) -> None:
        for position in range(start, min(end, length)):
            if out[position] != "\n":
                out[position] = " "

    while index < length:
        char = source[index]
        if source.startswith("//", index):
            end = source.find("\n", index)
            end = length if end < 0 else end
            blank(index, end)
            index = end
        elif source.startswith("/*", index):
            end = source.find("*/", index + 2)
            end = length if end < 0 else end + 2
            blank(index, end)
            index = end
        elif char.isascii() and (char.isalnum() or char == "_"):
            # Consume a whole identifier or pp-number so digit separators
            # (1'000) are not mistaken for character literals.
            match = TOKEN_RE.match(source, index)
            token_end = match.end()
            token = match.group(0)
            if token in RAW_STRING_PREFIXES and token_end < length and source[token_end] == '"':
                delimiter_end = source.find("(", token_end + 1)
                if delimiter_end < 0:
                    raise PolicyError("malformed raw string literal")
                terminator = ")" + source[token_end + 1 : delimiter_end] + '"'
                end = source.find(terminator, delimiter_end + 1)
                end = length if end < 0 else end + len(terminator)
                blank(index, end)
                index = end
            else:
                index = token_end
        elif char in "\"'":
            position = index + 1
            while position < length and source[position] != char and source[position] != "\n":
                position += 2 if source[position] == "\\" else 1
            blank(index, position + 1)
            index = position + 1
        else:
            index += 1
    return "".join(out)


def matching_brace(text: str, opening: int) -> int:
    depth = 0
    for position in range(opening, len(text)):
        if text[position] == "{":
            depth += 1
        elif text[position] == "}":
            depth -= 1
            if depth == 0:
                return position
    return -1


def test_bodies(text: str) -> list[tuple[int, int, str]]:
    bodies = []
    for match in TEST_HEADER_RE.finditer(text):
        macro, first, second = match.groups()
        if macro == "TEST_F" and second is None:
            continue
        name = f"{first}.{second}" if macro == "TEST_F" else first
        opening = match.end()
        while opening < len(text) and text[opening].isspace():
            opening += 1
        if opening >= len(text) or text[opening] != "{":
            continue
        closing = matching_brace(text, opening)
        if closing < 0:
            raise PolicyError(f"unterminated body for test {name}")
        bodies.append((opening, closing, name))
    return bodies


def inventory_warn_only_sites(tests_root: Path) -> tuple[dict[tuple[str, str], list[int]], int]:
    """Return {(file, test): [lines]} for waivable sites, plus the exempt probe count."""
    if not tests_root.is_dir():
        raise PolicyError(f"tests root {tests_root} is not a directory")
    base = tests_root.parent
    sites: dict[tuple[str, str], list[int]] = {}
    probes = 0
    for path in sorted(tests_root.rglob("*.cpp")):
        try:
            source = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            raise PolicyError(f"cannot read {path}: {exc}") from exc
        if "EXPECT_WARN_ONLY" not in source:
            continue
        text = strip_comments_and_literals(source)
        relative = path.relative_to(base).as_posix()
        bodies = None
        for match in WARN_ONLY_RE.finditer(text):
            if bodies is None:
                bodies = test_bodies(text)
            line = text.count("\n", 0, match.start()) + 1
            owners = [name for opening, closing, name in bodies if opening < match.start() < closing]
            if not owners:
                raise PolicyError(
                    f"{relative}:{line}: EXPECT_WARN_ONLY outside a TEST/TEST_F body cannot be attributed "
                    "to a test; move it into the test body"
                )
            test_name = owners[-1]
            if relative == RUNNER_PROBE_FILE and test_name.startswith(RUNNER_PROBE_PREFIX):
                probes += 1
                continue
            sites.setdefault((relative, test_name), []).append(line)
    return sites, probes


def validate_assertion_waivers(tests_root: Path, payload: dict, as_of: date) -> tuple[int, int]:
    declared = load_assertion_metadata(payload, as_of)
    sites, probes = inventory_warn_only_sites(tests_root)
    problems = []
    for key in sorted(sites):
        lines = sites[key]
        where = f"{key[0]}::{key[1]} (line {', '.join(map(str, lines))})"
        if key not in declared:
            problems.append(f"unregistered EXPECT_WARN_ONLY waiver at {where}")
        elif declared[key] != len(lines):
            problems.append(
                f"assertion waiver for {key[0]}::{key[1]} declares {declared[key]} site(s) but the test has "
                f"{len(lines)} at line {', '.join(map(str, lines))}"
            )
    for key in sorted(set(declared) - set(sites)):
        problems.append(f"stale assertion waiver: no EXPECT_WARN_ONLY in {key[0]}::{key[1]}")
    if problems:
        raise PolicyError("; ".join(problems))
    return sum(len(lines) for lines in sites.values()), probes


def validate(registry_path: Path, metadata_path: Path, tests_root: Path, as_of: date) -> int:
    registry_patterns = load_registry(registry_path)
    payload = load_payload(metadata_path)
    metadata = load_metadata(payload, as_of)
    registry_set = set(registry_patterns)
    metadata_set = set(metadata)
    missing = sorted(registry_set - metadata_set)
    extra = sorted(metadata_set - registry_set)
    if missing:
        raise PolicyError(f"missing metadata for registry pattern(s): {', '.join(missing)}")
    if extra:
        raise PolicyError(
            "metadata pattern(s) not present in TestWarnings.h: " + ", ".join(extra)
        )
    assertion_sites, probes = validate_assertion_waivers(tests_root, payload, as_of)
    print(f"validated {len(registry_patterns)} flaky waiver(s) with owner and expiry metadata")
    print(
        f"validated {assertion_sites} EXPECT_WARN_ONLY site(s) with owner and expiry metadata "
        f"({probes} runner-semantics probe(s) exempt)"
    )
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registry", type=Path, default=Path("Tests/TestWarnings.h"))
    parser.add_argument("--metadata", type=Path, default=Path("Tests/test-warning-waivers.json"))
    parser.add_argument(
        "--tests-root",
        type=Path,
        default=Path("Tests"),
        help="directory scanned for EXPECT_WARN_ONLY sites; file keys are relative to its parent",
    )
    parser.add_argument(
        "--as-of",
        type=date.fromisoformat,
        default=date.today(),
        help="policy date for deterministic tests (default: today)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        return validate(args.registry, args.metadata, args.tests_root, args.as_of)
    except PolicyError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
