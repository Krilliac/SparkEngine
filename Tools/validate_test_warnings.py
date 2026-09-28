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

Two further exception shapes are policed (schema 3):

* ``EXPECT_NO_CRASH`` only proves the process survived, so it is allowed only
  in the runner-semantics probes; every other test must assert observable state.
* every ``SKIP_TEST`` (and the ``SkipOrFail`` forwarder) in Tests/**/*.{cpp,h}
  must be classified. A string-literal reason must start with exactly one
  ``skipReasons`` prefix; a reason built at run time must sit in a test (or,
  with ``"test": null``, a helper outside any test body) listed in
  ``dynamicSkips`` with its exact site count. Each entry names an owner and a
  kind: ``environment`` (a missing platform capability; it does not expire),
  ``flaky`` (a timing or host-pressure skip; it needs a future expiry), or
  ``probe`` (the skip itself is the behaviour under test).
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
NO_CRASH_RE = re.compile(r"(?<![A-Za-z0-9_])EXPECT_NO_CRASH\s*\(")
SKIP_CALL_RE = re.compile(r"(?<![A-Za-z0-9_])(?:SKIP_TEST|SkipOrFail)\s*\(")
# A match preceded on its line by these is the macro or forwarder definition, not a call.
DEFINITION_TAIL_RE = re.compile(r"(?:#\s*define|\bvoid)\s+$")
CPP_STRING_RE = re.compile(r'"((?:\\.|[^"\\\n])*)"')
SKIP_KINDS = {"environment", "flaky", "probe"}
ENTRY_RE = re.compile(
    r'\{\s*"((?:\\.|[^"\\])*)"\s*,\s*"((?:\\.|[^"\\])*)"\s*\}',
    re.DOTALL,
)


class PolicyError(ValueError):
    """A repository policy or schema violation."""


def parse_cpp_string(value: str, where: str = "TestWarnings.h") -> str:
    try:
        return json.loads(f'"{value}"')
    except json.JSONDecodeError as exc:
        raise PolicyError(f"invalid C++ string literal in {where}: {exc}") from exc


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
    # Older schemas stay readable; they declare no per-assertion waivers or skip
    # classifications, so any EXPECT_WARN_ONLY or SKIP_TEST site fails as unregistered.
    expected_keys = {
        1: {"schemaVersion", "waivers"},
        2: {"schemaVersion", "waivers", "assertionWaivers"},
        3: {"schemaVersion", "waivers", "assertionWaivers", "skipReasons", "dynamicSkips"},
    }
    if isinstance(version, bool) or version not in expected_keys:
        raise PolicyError("warning metadata schemaVersion must be 1, 2 or 3")
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


def validate_skip_owner_and_kind(label: str, entry: dict, base_keys: set[str], as_of: date) -> str:
    """Check an owned skip classification; flaky skips expire, environment and probe skips do not."""
    kind = entry.get("kind")
    if kind not in SKIP_KINDS:
        raise PolicyError(f"{label} kind must be one of {', '.join(sorted(SKIP_KINDS))}")
    keys = base_keys | {"owner", "kind"} | ({"expires"} if kind == "flaky" else set())
    if set(entry) != keys:
        raise PolicyError(f"{label} with kind {kind!r} must contain exactly {', '.join(sorted(keys))}")
    owner = entry["owner"]
    if not isinstance(owner, str):
        raise PolicyError(f"{label} owner must be a string")
    if kind == "flaky":
        if not isinstance(entry["expires"], str):
            raise PolicyError(f"{label} expires must be a string")
        validate_owner_and_expiry(label, owner, entry["expires"], as_of)
    elif not OWNER_RE.fullmatch(owner) or owner.lower() in PLACEHOLDER_OWNERS:
        raise PolicyError(f"{label} owner must be a named owner, got {owner!r}")
    return kind


def load_skip_metadata(payload: dict, as_of: date) -> tuple[dict[str, str], dict[tuple[str, str | None], int]]:
    """Return ({prefix: kind}, {(file, test-or-None): sites}) from the schema-3 skip lists."""
    reasons = payload.get("skipReasons", [])
    dynamic = payload.get("dynamicSkips", [])
    if not isinstance(reasons, list) or not isinstance(dynamic, list):
        raise PolicyError("warning metadata skipReasons and dynamicSkips must be lists")
    if len(reasons) > MAX_WAIVERS or len(dynamic) > MAX_WAIVERS:
        raise PolicyError(f"warning metadata skip lists are limited to {MAX_WAIVERS} entries each")

    prefixes: dict[str, str] = {}
    for index, entry in enumerate(reasons):
        label = f"warning metadata skipReasons[{index}]"
        if not isinstance(entry, dict):
            raise PolicyError(f"{label} must be an object")
        kind = validate_skip_owner_and_kind(label, entry, {"prefix"}, as_of)
        prefix = entry["prefix"]
        if not isinstance(prefix, str) or not prefix.strip():
            raise PolicyError(f"{label} prefix must be a non-empty string")
        if prefix in prefixes:
            raise PolicyError(f"duplicate skip reason prefix: {prefix!r}")
        prefixes[prefix] = kind

    sites: dict[tuple[str, str | None], int] = {}
    for index, entry in enumerate(dynamic):
        label = f"warning metadata dynamicSkips[{index}]"
        if not isinstance(entry, dict):
            raise PolicyError(f"{label} must be an object")
        validate_skip_owner_and_kind(label, entry, {"file", "test", "sites"}, as_of)
        file_name, test_name, count = entry["file"], entry["test"], entry["sites"]
        if not isinstance(file_name, str) or not file_name.strip():
            raise PolicyError(f"{label} file must be a non-empty string")
        if test_name is not None and (not isinstance(test_name, str) or not test_name.strip()):
            raise PolicyError(f"{label} test must be a test name, or null for a helper outside any test body")
        if isinstance(count, bool) or not isinstance(count, int) or count < 1:
            raise PolicyError(f"{label} sites must be a positive integer")
        key = (file_name, test_name)
        if key in sites:
            raise PolicyError(f"duplicate dynamic skip entry for {file_name}::{test_name}")
        sites[key] = count
    return prefixes, sites


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


def matching_brace(text: str, opening: int, pair: str = "{}") -> int:
    depth = 0
    for position in range(opening, len(text)):
        if text[position] == pair[0]:
            depth += 1
        elif text[position] == pair[1]:
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


def macro_sites(tests_root: Path, pattern: re.Pattern[str], suffixes: tuple[str, ...]):
    """Yield (relative, line, test-or-None, match, source, text) for each call of pattern under tests_root.

    Comments and literals are blanked first, and a match that is the macro or
    forwarder definition itself (``#define X(`` or ``void X(``) is skipped.
    """
    if not tests_root.is_dir():
        raise PolicyError(f"tests root {tests_root} is not a directory")
    base = tests_root.parent
    for path in sorted(path for path in tests_root.rglob("*") if path.suffix in suffixes and path.is_file()):
        try:
            source = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            raise PolicyError(f"cannot read {path}: {exc}") from exc
        if not pattern.search(source):
            continue
        text = strip_comments_and_literals(source)
        relative = path.relative_to(base).as_posix()
        bodies = None
        for match in pattern.finditer(text):
            line_start = text.rfind("\n", 0, match.start()) + 1
            if DEFINITION_TAIL_RE.search(text[line_start : match.start()]):
                continue
            if bodies is None:
                bodies = test_bodies(text)
            line = text.count("\n", 0, match.start()) + 1
            owners = [name for opening, closing, name in bodies if opening < match.start() < closing]
            yield relative, line, (owners[-1] if owners else None), match, source, text


def is_runner_probe(relative: str, test_name: str | None) -> bool:
    return relative == RUNNER_PROBE_FILE and test_name is not None and test_name.startswith(RUNNER_PROBE_PREFIX)


def inventory_warn_only_sites(tests_root: Path) -> tuple[dict[tuple[str, str], list[int]], int]:
    """Return {(file, test): [lines]} for waivable sites, plus the exempt probe count."""
    sites: dict[tuple[str, str], list[int]] = {}
    probes = 0
    for relative, line, test_name, _, _, _ in macro_sites(tests_root, WARN_ONLY_RE, (".cpp",)):
        if test_name is None:
            raise PolicyError(
                f"{relative}:{line}: EXPECT_WARN_ONLY outside a TEST/TEST_F body cannot be attributed "
                "to a test; move it into the test body"
            )
        if is_runner_probe(relative, test_name):
            probes += 1
            continue
        sites.setdefault((relative, test_name), []).append(line)
    return sites, probes


def validate_no_crash_sites(tests_root: Path) -> int:
    """EXPECT_NO_CRASH only proves survival, so only the runner-semantics probes may use it."""
    problems = []
    probes = 0
    for relative, line, test_name, _, _, _ in macro_sites(
        tests_root, NO_CRASH_RE, (".cpp", ".h")
    ):
        if is_runner_probe(relative, test_name):
            probes += 1
            continue
        problems.append(
            f"EXPECT_NO_CRASH at {relative}:{line} ({test_name or 'outside a test body'}) only proves the process "
            "survived; assert observable state instead"
        )
    if problems:
        raise PolicyError("; ".join(problems))
    return probes


def literal_skip_reason(source: str, text: str, opening: int, where: str) -> str | None:
    """Return the reason when the call's argument is only string literals, or None when it is built at run time."""
    closing = matching_brace(text, opening, "()")
    if closing < 0:
        raise PolicyError(f"{where}: unterminated skip call")
    if text[opening + 1 : closing].strip():
        return None
    parts = CPP_STRING_RE.findall(source[opening + 1 : closing])
    if not parts:
        return None
    return "".join(parse_cpp_string(part, where) for part in parts)


def validate_skip_sites(tests_root: Path, payload: dict, as_of: date) -> tuple[int, dict[str, int]]:
    """Every SKIP_TEST/SkipOrFail site is classified; return (site count, sites per kind)."""
    prefixes, declared = load_skip_metadata(payload, as_of)
    used: set[str] = set()
    dynamic: dict[tuple[str, str | None], list[int]] = {}
    kinds = {kind: 0 for kind in sorted(SKIP_KINDS)}
    problems = []
    total = 0
    for relative, line, test_name, match, source, text in macro_sites(
        tests_root, SKIP_CALL_RE, (".cpp", ".h")
    ):
        total += 1
        where = f"{relative}:{line}"
        reason = literal_skip_reason(source, text, match.end() - 1, where)
        if reason is None:
            dynamic.setdefault((relative, test_name), []).append(line)
            continue
        matches = [prefix for prefix in prefixes if reason.startswith(prefix)]
        if len(matches) != 1:
            state = "unclassified" if not matches else "ambiguously classified"
            problems.append(f"{state} skip reason at {where}: {reason!r}")
            continue
        used.add(matches[0])
        kinds[prefixes[matches[0]]] += 1

    def ordered(keys):
        return sorted(keys, key=lambda item: (item[0], item[1] or ""))

    for key in ordered(dynamic):
        lines = dynamic[key]
        scope = key[1] or "outside a test body"
        where = f"{key[0]}::{scope} (line {', '.join(map(str, lines))})"
        if key not in declared:
            problems.append(f"unregistered run-time skip reason at {where}")
        elif declared[key] != len(lines):
            problems.append(f"dynamic skip entry for {key[0]}::{scope} declares {declared[key]} site(s), found {where}")
    for key in ordered(set(declared) - set(dynamic)):
        problems.append(f"stale dynamic skip entry: no run-time skip reason in {key[0]}::{key[1] or 'helpers'}")
    for prefix in sorted(set(prefixes) - used):
        problems.append(f"stale skip reason prefix: no SKIP_TEST reason starts with {prefix!r}")
    if problems:
        raise PolicyError("; ".join(problems))
    for entry in payload.get("dynamicSkips", []):
        kinds[entry["kind"]] += entry["sites"]
    return total, kinds


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
    no_crash_probes = validate_no_crash_sites(tests_root)
    skip_sites, skip_kinds = validate_skip_sites(tests_root, payload, as_of)
    print(f"validated {len(registry_patterns)} flaky waiver(s) with owner and expiry metadata")
    print(
        f"validated {assertion_sites} EXPECT_WARN_ONLY site(s) with owner and expiry metadata "
        f"({probes} runner-semantics probe(s) exempt)"
    )
    print(f"validated 0 EXPECT_NO_CRASH site(s) outside the {no_crash_probes} runner-semantics probe(s)")
    breakdown = ", ".join(f"{count} {kind}" for kind, count in skip_kinds.items())
    print(f"validated {skip_sites} SKIP_TEST site(s) with owner and kind metadata ({breakdown})")
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
