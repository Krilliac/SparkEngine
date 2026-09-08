#!/usr/bin/env python3
"""The dependency authority PLT-200 evidence is checked against.

A `dependencyClosure` block inside an evidence record is attacker-influenced
text.  On its own it proves nothing: a record can name any library at any
version and a validator that only checks the *shape* of those strings will
certify a fabricated list.  This module supplies the missing half -- an
external list of what SparkEngine is actually allowed to depend on.

Two sources feed it, and neither is the evidence record:

  1. `ThirdParty/dependencies.lock` -- the manifest CMake already consumes at
     configure time.  It is the repository's real answer to "what third-party
     code is vendored here", and it is enforced today by the
     `check-thirdparty-manifest` CI job.  Submodule pins in it are CMake
     variables resolved from the repository gitlinks, so the authority records
     the exact 40-hex revision the tree holds.
  2. A curated `platformRuntime` block for the operating-system and
     redistributable libraries a Windows install pulls in, which cannot come
     from a source manifest.  These are committed and reviewed, and an
     evidence record still has to produce the real bytes of each one.

The generated authority is committed to
`docs/certification/dependency-authority.json`.  `--check` re-derives it and
fails on any drift, so a submodule bump or a manifest edit forces the closure
to be re-certified instead of silently inheriting the old verdict.

The validator only ever *reads* the committed authority and confirms its
recorded digest still matches the manifest bytes.  It never shells out to git,
so validation stays deterministic and hermetic; the git-backed drift gate is
this module's `--check` mode, wired into CI and CTest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parents[1]

LOCK_RELPATH = "ThirdParty/dependencies.lock"
AUTHORITY_RELPATH = "docs/certification/dependency-authority.json"

AUTHORITY_SCHEMA_VERSION = 1

# The manifest is a CMake file; it is small and must stay small.
MAX_LOCK_BYTES = 512 * 1024
MAX_AUTHORITY_BYTES = 512 * 1024

MAX_THIRD_PARTY_ENTRIES = 200
MAX_PLATFORM_ENTRIES = 200

_LOCK_FIELDS = (
    "name",
    "sourceUrl",
    "version",
    "license",
    "localPath",
    "requiredFiles",
    "featureMacro",
    "fallback",
    "severity",
    "licenseNotices",
)

_GITLINK_CALL_RE = re.compile(
    r'_spark_manifest_gitlink_revision\(\s*"([^"\n]+)"\s+(_[A-Za-z0-9_]+)\s*\)'
)
_ENTRY_LINE_RE = re.compile(r'^\s*"(.*)"\s*$')
_VARIABLE_RE = re.compile(r"\$\{([A-Za-z0-9_]+)\}")
_GITLINK_RECORD_RE = re.compile(r"^160000 commit ([0-9a-f]{40})\s")
_SHA1_RE = re.compile(r"\A[0-9a-f]{40}\Z")

_SET_OPEN = "set(SPARK_THIRDPARTY_AUDIT_ENTRIES"


class AuthorityError(Exception):
    """The dependency authority cannot be derived or is inconsistent."""


# ── Manifest parsing ───────────────────────────────────────────────────────


def parse_lock(text: str) -> tuple[list[dict[str, str]], dict[str, str]]:
    """Parse the audit-entry block and the gitlink variable declarations.

    Returns (entries, gitlink_vars) where gitlink_vars maps a CMake variable
    name to the repository-relative submodule path whose gitlink fills it.
    """
    gitlink_vars: dict[str, str] = {}
    for path, var in _GITLINK_CALL_RE.findall(text):
        if var in gitlink_vars and gitlink_vars[var] != path:
            raise AuthorityError(f"gitlink variable {var!r} declared twice")
        gitlink_vars[var] = path

    lines = text.splitlines()
    starts = [i for i, line in enumerate(lines) if line.strip().startswith(_SET_OPEN)]
    if len(starts) != 1:
        raise AuthorityError(
            f"expected exactly one {_SET_OPEN}...) block, found {len(starts)}"
        )

    entries: list[dict[str, str]] = []
    closed = False
    for line in lines[starts[0] + 1 :]:
        stripped = line.strip()
        if stripped == ")":
            closed = True
            break
        if not stripped:
            continue
        match = _ENTRY_LINE_RE.match(line)
        if match is None:
            raise AuthorityError(f"unparsable manifest entry line: {line.strip()!r}")
        entries.append(_split_entry(match.group(1)))
    if not closed:
        raise AuthorityError("manifest audit-entry block is not terminated by ')'")
    if not entries:
        raise AuthorityError("manifest audit-entry block is empty")
    if len(entries) > MAX_THIRD_PARTY_ENTRIES:
        raise AuthorityError(
            f"manifest declares {len(entries)} entries (max {MAX_THIRD_PARTY_ENTRIES})"
        )
    return entries, gitlink_vars


def _split_entry(raw: str) -> dict[str, str]:
    parts = raw.split("|")
    if len(parts) != len(_LOCK_FIELDS):
        raise AuthorityError(
            f"manifest entry has {len(parts)} fields, expected "
            f"{len(_LOCK_FIELDS)}: {raw[:120]!r}"
        )
    entry = dict(zip(_LOCK_FIELDS, parts))
    for field, value in entry.items():
        if not value.strip():
            raise AuthorityError(f"manifest entry field {field!r} is blank: {raw[:120]!r}")
    if entry["severity"] not in ("ERROR", "WARN"):
        raise AuthorityError(
            f"manifest entry {entry['name']!r} has severity {entry['severity']!r}"
        )
    return entry


def resolve_gitlinks(repo_root: Path, gitlink_vars: dict[str, str]) -> dict[str, str]:
    """Read the exact 40-hex revision each submodule gitlink holds in HEAD."""
    resolved: dict[str, str] = {}
    for var, submodule_path in sorted(gitlink_vars.items()):
        try:
            completed = subprocess.run(
                ["git", "ls-tree", "HEAD", "--", submodule_path],
                cwd=str(repo_root),
                capture_output=True,
                text=True,
                check=False,
            )
        except OSError as exc:  # pragma: no cover - git absent
            raise AuthorityError(f"cannot run git to resolve {submodule_path!r}: {exc}") from exc
        if completed.returncode != 0:
            raise AuthorityError(
                f"git ls-tree failed for {submodule_path!r}: "
                f"{completed.stderr.strip()[:200]}"
            )
        record = completed.stdout.strip()
        match = _GITLINK_RECORD_RE.match(record)
        if match is None:
            raise AuthorityError(
                f"{submodule_path!r} is not a mode-160000 gitlink in HEAD: {record[:120]!r}"
            )
        resolved[var] = match.group(1)
    return resolved


def substitute(value: str, resolved: dict[str, str]) -> str:
    """Replace ${var} references, refusing any that cannot be resolved."""

    def _one(match: re.Match[str]) -> str:
        name = match.group(1)
        if name not in resolved:
            raise AuthorityError(f"unresolved manifest variable ${{{name}}}")
        return resolved[name]

    out = _VARIABLE_RE.sub(_one, value)
    if _VARIABLE_RE.search(out):  # pragma: no cover - defensive
        raise AuthorityError(f"variable substitution left a reference in {out!r}")
    return out


# ── Authority construction ─────────────────────────────────────────────────


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    raw = path.read_bytes()
    if len(raw) > MAX_LOCK_BYTES:
        raise AuthorityError(f"{path.name}: {len(raw)} bytes exceeds {MAX_LOCK_BYTES}")
    digest.update(raw)
    return digest.hexdigest()


def build_third_party(repo_root: Path) -> tuple[list[dict[str, Any]], str]:
    """Derive the third-party section of the authority from the manifest."""
    lock_path = repo_root / LOCK_RELPATH
    if not lock_path.is_file():
        raise AuthorityError(f"dependency manifest missing: {LOCK_RELPATH}")
    raw = lock_path.read_bytes()
    if len(raw) > MAX_LOCK_BYTES:
        raise AuthorityError(f"{LOCK_RELPATH}: {len(raw)} bytes exceeds {MAX_LOCK_BYTES}")
    text = raw.decode("utf-8")

    entries, gitlink_vars = parse_lock(text)
    resolved = resolve_gitlinks(repo_root, gitlink_vars)

    derived: list[dict[str, Any]] = []
    seen: set[str] = set()
    for entry in entries:
        name = entry["name"].strip()
        key = name.casefold()
        if key in seen:
            raise AuthorityError(f"manifest declares {name!r} twice")
        seen.add(key)
        derived.append(
            {
                "name": name,
                "version": substitute(entry["version"].strip(), resolved),
                "source": "bundled",
                "localPath": entry["localPath"].strip(),
                "severity": entry["severity"],
                # Vendored third-party code is compiled into the engine
                # binaries, so it is legitimate to *name* in a closure but is
                # never a separate runtime file a row must ship.
                "requiredForRows": [],
            }
        )
    derived.sort(key=lambda item: item["name"].casefold())
    return derived, hashlib.sha256(raw).hexdigest()


def default_platform_runtime() -> list[dict[str, Any]]:
    """The operating-system and redistributable libraries a Windows row ships.

    These cannot be derived from a source manifest, so they are committed and
    reviewed here.  Version strings on OS libraries are host-specific, so each
    entry pins a bounded anchored *shape* rather than an exact string; the
    file's real bytes are still measured out of the evidence bundle, so an
    entry that matches the shape but not a real file cannot certify.
    """
    d3d11_row = "win11-x64-msvc143-d3d11"
    nullrhi_row = "win11-x64-msvc143-nullrhi"
    msvc_pattern = r"^14\.[0-9]{1,3}\.[0-9]{1,5}\.[0-9]{1,5}$"
    os_pattern = r"^10\.0\.[0-9]{1,6}\.[0-9]{1,6}$"
    return [
        {
            "name": "msvcp140.dll",
            "versionPattern": msvc_pattern,
            "source": "vcredist",
            "requiredForRows": [d3d11_row, nullrhi_row],
            "justification": "MSVC C++ runtime; every MSVC-built Shipping binary links it.",
        },
        {
            "name": "vcruntime140.dll",
            "versionPattern": msvc_pattern,
            "source": "vcredist",
            "requiredForRows": [d3d11_row, nullrhi_row],
            "justification": "MSVC C runtime.",
        },
        {
            "name": "vcruntime140_1.dll",
            "versionPattern": msvc_pattern,
            "source": "vcredist",
            "requiredForRows": [d3d11_row, nullrhi_row],
            "justification": "MSVC x64 exception-handling runtime.",
        },
        {
            "name": "d3d11.dll",
            "versionPattern": os_pattern,
            "source": "system",
            "requiredForRows": [d3d11_row],
            "justification": "Direct3D 11 runtime for the primary RHI backend.",
        },
        {
            "name": "dxgi.dll",
            "versionPattern": os_pattern,
            "source": "system",
            "requiredForRows": [d3d11_row],
            "justification": "DXGI adapter/swapchain runtime used by the D3D11 backend.",
        },
        {
            "name": "d3dcompiler_47.dll",
            "versionPattern": os_pattern,
            "source": "directx",
            "requiredForRows": [d3d11_row],
            "justification": "Runtime HLSL compilation path in SparkShaderCompiler.",
        },
        {
            "name": "xaudio2_9.dll",
            "versionPattern": os_pattern,
            "source": "system",
            "requiredForRows": [d3d11_row],
            "justification": "XAudio2 backend; the NullRHI row declares audio api 'none'.",
        },
    ]


def build_authority(
    repo_root: Path, *, platform_runtime: list[dict[str, Any]] | None = None
) -> dict[str, Any]:
    third_party, lock_digest = build_third_party(repo_root)
    runtime = (
        default_platform_runtime() if platform_runtime is None else platform_runtime
    )
    if len(runtime) > MAX_PLATFORM_ENTRIES:
        raise AuthorityError(
            f"platformRuntime declares {len(runtime)} entries "
            f"(max {MAX_PLATFORM_ENTRIES})"
        )
    return {
        "schemaVersion": AUTHORITY_SCHEMA_VERSION,
        "generatedFrom": LOCK_RELPATH,
        "sourceSha256": lock_digest,
        "note": (
            "Generated by Tools/platform-cert/dependency_authority.py --generate. "
            "thirdParty is derived from the manifest CMake consumes; platformRuntime "
            "is curated and reviewed. Regenerate after any manifest or submodule "
            "change, then re-collect certification evidence."
        ),
        "thirdParty": third_party,
        "platformRuntime": runtime,
    }


# ── Loading and matching (no git, no subprocess) ───────────────────────────


class Authority:
    """The committed authority, indexed for exact (name, source) lookup."""

    def __init__(self, document: dict[str, Any]) -> None:
        self.document = document
        self.by_identity: dict[tuple[str, str], dict[str, Any]] = {}
        self.required_for_row: dict[str, list[tuple[str, str]]] = {}

        for section in ("thirdParty", "platformRuntime"):
            for entry in document[section]:
                key = (entry["name"].casefold(), entry["source"])
                if key in self.by_identity:
                    raise AuthorityError(
                        f"dependency authority declares {entry['name']!r} "
                        f"from {entry['source']!r} twice"
                    )
                self.by_identity[key] = entry
                for row_id in entry.get("requiredForRows", []):
                    self.required_for_row.setdefault(row_id, []).append(key)

    def names(self) -> set[str]:
        return {name for name, _ in self.by_identity}

    def lookup(self, name: str, source: str) -> dict[str, Any] | None:
        return self.by_identity.get((name.casefold(), source))

    def required_keys(self, row_id: str) -> list[tuple[str, str]]:
        return list(self.required_for_row.get(row_id, []))


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise AuthorityError(message)


def validate_authority_document(document: Any) -> dict[str, Any]:
    """Structurally validate a loaded authority document, fail-closed."""
    _require(isinstance(document, dict), "dependency authority must be an object")
    allowed = {
        "schemaVersion",
        "generatedFrom",
        "sourceSha256",
        "note",
        "thirdParty",
        "platformRuntime",
    }
    extra = sorted(set(document) - allowed)
    _require(not extra, f"dependency authority has unknown fields {extra}")
    for field in ("schemaVersion", "generatedFrom", "sourceSha256", "thirdParty", "platformRuntime"):
        _require(field in document, f"dependency authority is missing {field!r}")
    _require(
        document["schemaVersion"] == AUTHORITY_SCHEMA_VERSION
        and not isinstance(document["schemaVersion"], bool),
        f"dependency authority schemaVersion must be {AUTHORITY_SCHEMA_VERSION}",
    )
    _require(
        document["generatedFrom"] == LOCK_RELPATH,
        f"dependency authority must be generated from {LOCK_RELPATH}",
    )
    _require(
        isinstance(document["sourceSha256"], str)
        and _SHA256_RE.match(document["sourceSha256"]) is not None,
        "dependency authority sourceSha256 must be 64 lowercase hex characters",
    )

    third_party = document["thirdParty"]
    runtime = document["platformRuntime"]
    _require(isinstance(third_party, list), "thirdParty must be a list")
    _require(isinstance(runtime, list), "platformRuntime must be a list")
    _require(
        len(third_party) <= MAX_THIRD_PARTY_ENTRIES,
        f"thirdParty declares more than {MAX_THIRD_PARTY_ENTRIES} entries",
    )
    _require(
        len(runtime) <= MAX_PLATFORM_ENTRIES,
        f"platformRuntime declares more than {MAX_PLATFORM_ENTRIES} entries",
    )

    for index, entry in enumerate(third_party):
        _validate_entry(entry, f"thirdParty[{index}]", exact_version=True)
    for index, entry in enumerate(runtime):
        _validate_entry(entry, f"platformRuntime[{index}]", exact_version=False)
    return document


_SHA256_RE = re.compile(r"\A[0-9a-f]{64}\Z")
_DEP_NAME_RE = re.compile(r"\A[A-Za-z0-9][A-Za-z0-9 ._+/-]{0,259}\Z")
_ROW_ID_RE = re.compile(r"\A[a-z0-9][a-z0-9._-]{0,63}\Z")
VALID_SOURCES = frozenset({"system", "bundled", "vcredist", "directx", "sdk"})


def _validate_entry(entry: Any, context: str, *, exact_version: bool) -> None:
    _require(isinstance(entry, dict), f"{context}: must be an object")
    _require(
        isinstance(entry.get("name"), str)
        and _DEP_NAME_RE.match(entry["name"]) is not None,
        f"{context}: name is missing or malformed",
    )
    _require(
        entry.get("source") in VALID_SOURCES,
        f"{context}: source must be one of {sorted(VALID_SOURCES)}",
    )
    has_exact = isinstance(entry.get("version"), str) and entry["version"].strip() != ""
    has_pattern = (
        isinstance(entry.get("versionPattern"), str) and entry["versionPattern"] != ""
    )
    _require(
        has_exact != has_pattern,
        f"{context}: exactly one of version / versionPattern is required",
    )
    if exact_version:
        _require(has_exact, f"{context}: third-party entries need an exact version")
    if has_exact:
        _require(len(entry["version"]) <= 400, f"{context}: version is too long")
    if has_pattern:
        pattern = entry["versionPattern"]
        _require(len(pattern) <= 200, f"{context}: versionPattern is too long")
        _require(
            pattern.startswith("^") and pattern.endswith("$"),
            f"{context}: versionPattern must be anchored with ^ and $",
        )
        try:
            re.compile(pattern)
        except re.error as exc:
            raise AuthorityError(f"{context}: invalid versionPattern: {exc}") from exc
    rows = entry.get("requiredForRows", [])
    _require(isinstance(rows, list), f"{context}: requiredForRows must be a list")
    _require(len(rows) <= 64, f"{context}: requiredForRows is too long")
    for row_id in rows:
        _require(
            isinstance(row_id, str) and _ROW_ID_RE.match(row_id) is not None,
            f"{context}: requiredForRows entry {row_id!r} is malformed",
        )
    _require(len(set(rows)) == len(rows), f"{context}: requiredForRows has duplicates")


def compile_version_pattern(entry: dict[str, Any]) -> re.Pattern[str] | None:
    pattern = entry.get("versionPattern")
    if not isinstance(pattern, str):
        return None
    body = pattern[1:-1] if pattern.startswith("^") and pattern.endswith("$") else pattern
    return re.compile(r"\A" + body + r"\Z")


# ── CLI ────────────────────────────────────────────────────────────────────


def _write_authority(path: Path, document: dict[str, Any]) -> None:
    text = json.dumps(document, indent=2, sort_keys=False, ensure_ascii=False) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(text)


def _load_committed(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise AuthorityError(f"committed dependency authority missing: {path}")
    raw = path.read_bytes()
    if len(raw) > MAX_AUTHORITY_BYTES:
        raise AuthorityError(f"{path.name}: {len(raw)} bytes exceeds {MAX_AUTHORITY_BYTES}")
    return json.loads(raw.decode("utf-8"))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help=f"authority path (default: <repo-root>/{AUTHORITY_RELPATH})",
    )
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--generate", action="store_true", help="write the authority")
    mode.add_argument(
        "--check",
        action="store_true",
        help="re-derive from the manifest and fail on any drift",
    )
    args = parser.parse_args(argv)

    repo_root = args.repo_root.resolve()
    out_path = args.out if args.out is not None else repo_root / AUTHORITY_RELPATH

    try:
        if args.generate:
            existing_runtime: list[dict[str, Any]] | None = None
            if out_path.is_file():
                existing = _load_committed(out_path)
                if isinstance(existing.get("platformRuntime"), list):
                    existing_runtime = existing["platformRuntime"]
            document = build_authority(repo_root, platform_runtime=existing_runtime)
            validate_authority_document(document)
            Authority(document)
            _write_authority(out_path, document)
            print(
                f"Wrote {out_path} "
                f"({len(document['thirdParty'])} third-party, "
                f"{len(document['platformRuntime'])} platform-runtime entries)"
            )
            return 0

        committed = validate_authority_document(_load_committed(out_path))
        Authority(committed)
        derived = build_authority(
            repo_root, platform_runtime=committed["platformRuntime"]
        )
        if committed != derived:
            print(
                "FAIL: dependency authority has drifted from "
                f"{LOCK_RELPATH}. Regenerate with:\n"
                "  python Tools/platform-cert/dependency_authority.py --generate",
                file=sys.stderr,
            )
            for line in _describe_drift(committed, derived):
                print(f"  X {line}", file=sys.stderr)
            return 1
        print(
            f"PASS: dependency authority matches {LOCK_RELPATH} "
            f"({len(committed['thirdParty'])} third-party entries)"
        )
        return 0
    except AuthorityError as exc:
        print(f"FATAL: {exc}", file=sys.stderr)
        return 1
    except (OSError, json.JSONDecodeError, UnicodeDecodeError) as exc:
        print(f"FATAL: cannot read dependency authority: {exc}", file=sys.stderr)
        return 1


def _describe_drift(committed: dict[str, Any], derived: dict[str, Any]) -> list[str]:
    lines: list[str] = []
    if committed.get("sourceSha256") != derived.get("sourceSha256"):
        lines.append(
            f"{LOCK_RELPATH} digest changed: recorded "
            f"{str(committed.get('sourceSha256'))[:12]}, measured "
            f"{str(derived.get('sourceSha256'))[:12]}"
        )
    old = {(e["name"], e["version"]) for e in committed.get("thirdParty", [])}
    new = {(e["name"], e["version"]) for e in derived.get("thirdParty", [])}
    for name, version in sorted(new - old):
        lines.append(f"manifest now declares {name} at {version}")
    for name, version in sorted(old - new):
        lines.append(f"authority still records {name} at {version}")
    return lines or ["authority and manifest differ in a field not summarised above"]


if __name__ == "__main__":
    sys.exit(main())
