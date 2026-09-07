#!/usr/bin/env python3
"""SparkEngine supply-chain policy checker.

Fail-closed verification of dependency identity, content integrity, license
coverage, GitHub Actions pinning, and manifest consistency.

Design rules this file is required to obey:

  * A check that cannot run is a failure, never a skip.  Missing tooling,
    unreadable inputs, and undecodable text all exit non-zero.
  * Every tracked path under ThirdParty/ is accounted for.  Coverage is not a
    sample: directory tree digests pin the complete tracked payload, and the
    sentinel set adds on-disk content and license verification on top.
  * Structured inputs are parsed by their real parsers.  The dependency
    manifest is read through CMake, workflows through a YAML parser.  A text
    scrape of a structured file reports a clean tree it never examined.
  * Every input is bounded in size, count, and depth before it is read.

Exit codes:
    0  All checks passed
    1  One or more policy violations detected
    2  Checker itself failed (internal error, missing tooling, bad schema)

Usage:
    python tools/check-supply-chain.py              # verify
    python tools/check-supply-chain.py --update     # regenerate lockfile
    python tools/check-supply-chain.py --json       # machine-readable output
    python tools/check-supply-chain.py --ci         # CI mode (identical to default)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import Any, Iterable

LOCKFILE_REL = "ThirdParty/supply-chain.lock"
MANIFEST_REL = "ThirdParty/dependencies.lock"
GITMODULES_REL = ".gitmodules"
AUDIT_CMAKE_REL = "cmake/SparkThirdPartyAudit.cmake"
WORKFLOWS_DIR = ".github/workflows"

LOCKFILE_VERSION = 2
AUTHORITATIVE_ROOT = "ThirdParty"
AUTHORITATIVE_ROOTS = frozenset({AUTHORITATIVE_ROOT})

# ── Resource bounds ───────────────────────────────────────────────────
# Every one of these is an order of magnitude above the real repository.  They
# exist so that a hostile lockfile, manifest, workflow, or directory tree
# cannot turn the gate into an out-of-memory kill or a CI hang, either of which
# produces an exit code that is not 1 and is easily misread as "no findings".

MAX_LOCKFILE_BYTES = 8 * 1024 * 1024
MAX_MANIFEST_BYTES = 2 * 1024 * 1024
MAX_WORKFLOW_BYTES = 4 * 1024 * 1024
MAX_ENTRIES_EXPORT_BYTES = 2 * 1024 * 1024
MAX_SENTINEL_BYTES = 128 * 1024 * 1024
MAX_LICENSE_BYTES = 1024 * 1024
MAX_JSON_DEPTH = 24
MAX_SENTINELS = 4096
MAX_CONTAINERS = 512
MAX_SUBMODULES = 256
MAX_MANIFEST_ENTRIES = 512
MAX_TRACKED_PATHS = 200_000
MAX_WALK_ENTRIES = 400_000
MAX_WALK_DEPTH = 24
MAX_WORKFLOW_FILES = 512
MAX_ACTION_PIN_KEYS = 512
MAX_ACTION_PIN_SHAS = 32
MAX_VIOLATIONS = 500

MIN_LICENSE_SIZE = 200
LICENSE_KEYWORDS_TERMS = re.compile(
    r"Permission is (hereby )?granted|Redistribution and use|"
    r"public domain|TERMS AND CONDITIONS FOR USE|"
    r"THE SOFTWARE IS PROVIDED",
    re.IGNORECASE,
)
LICENSE_KEYWORDS_COPYRIGHT = re.compile(r"[Cc]opyright")

SHA256_HEX_RE = re.compile(r"^[0-9a-f]{64}$")
SHA40_HEX_RE = re.compile(r"^[0-9a-f]{40}$")
SAFE_PATH_RE = re.compile(r"^[A-Za-z0-9_\-./]+$")

# Regular blob modes git records for ordinary tracked files.
GIT_FILE_MODES = frozenset({"100644", "100755"})
GIT_SYMLINK_MODE = "120000"
GIT_GITLINK_MODE = "160000"

ACTION_REF_RE = re.compile(
    r"^(?P<repo>[A-Za-z0-9][A-Za-z0-9._-]*/[A-Za-z0-9._-]+)"
    r"(?P<subpath>(?:/[A-Za-z0-9._-]+)*)"
    r"@(?P<sha>[0-9a-f]{40})$"
)
DOCKER_REF_RE = re.compile(r"^docker://[A-Za-z0-9._/:-]+@sha256:[0-9a-f]{64}$")
EXPRESSION_RE = re.compile(r"\$\{\{")

MANIFEST_FIELD_COUNT = 10
(
    F_NAME,
    F_SOURCE,
    F_VERSION,
    F_LICENSE,
    F_LOCAL_PATH,
    F_REQUIRED_FILES,
    F_FEATURE_MACRO,
    F_FALLBACK,
    F_SEVERITY,
    F_NOTICES,
) = range(MANIFEST_FIELD_COUNT)

VALID_SEVERITIES = frozenset({"ERROR", "WARN"})


def _fatal(msg: str) -> None:
    """Abort with exit 2 — the checker itself could not complete."""
    print(f"FATAL: {msg}", file=sys.stderr)
    sys.exit(2)


@dataclass
class Violation:
    category: str
    path: str
    message: str
    severity: str = "error"


@dataclass
class CheckResult:
    violations: list[Violation] = field(default_factory=list)
    truncated: bool = False

    @property
    def passed(self) -> bool:
        return not self.truncated and not any(
            v.severity == "error" for v in self.violations
        )

    def _add(self, v: Violation) -> None:
        if len(self.violations) >= MAX_VIOLATIONS:
            if not self.truncated:
                self.truncated = True
                self.violations.append(
                    Violation(
                        "bounds",
                        LOCKFILE_REL,
                        f"violation limit reached ({MAX_VIOLATIONS}); "
                        "remaining findings suppressed",
                    )
                )
            return
        self.violations.append(v)

    def error(self, category: str, path: str, message: str) -> None:
        self._add(Violation(category, path, message, "error"))

    def warn(self, category: str, path: str, message: str) -> None:
        self._add(Violation(category, path, message, "warning"))


# ── Project root and git helpers ──────────────────────────────────────

def project_root() -> Path:
    """Locate project root via git, not file-relative heuristics."""
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            capture_output=True, text=True, timeout=60,
        )
    except (OSError, subprocess.TimeoutExpired) as e:
        _fatal(f"cannot run git to locate project root: {e}")
    if out.returncode != 0:
        _fatal("git rev-parse --show-toplevel failed — not in a git repo?")
    root = Path(out.stdout.strip()).resolve()
    if not root.is_dir():
        _fatal(f"git root is not a directory: {root}")
    return root


def git_cmd(args: list[str], cwd: Path) -> str:
    """Run a git command, returning stripped stdout. Raises on failure."""
    return _git_raw(args, cwd).strip()


def _git_raw(args: list[str], cwd: Path, stdin: str | None = None) -> str:
    try:
        result = subprocess.run(
            ["git", *args],
            input=stdin,
            capture_output=True, text=True, cwd=str(cwd), timeout=300,
        )
    except (OSError, subprocess.TimeoutExpired) as e:
        raise RuntimeError(f"git {' '.join(args)}: {e}")
    if result.returncode != 0:
        raise RuntimeError(
            f"git {' '.join(args)} exited {result.returncode}: "
            f"{result.stderr.strip()}"
        )
    return result.stdout


def _split_nul(text: str) -> list[str]:
    return [part for part in text.split("\0") if part]


@dataclass(frozen=True)
class TrackedEntry:
    mode: str
    blob: str
    path: str


def git_tracked_thirdparty(root: Path) -> list[TrackedEntry]:
    """Every tracked index entry under ThirdParty/, with mode and blob id.

    The tracked set — not the filesystem listing — is the attack surface a
    supply-chain policy governs, and it is identical on every machine.
    """
    try:
        raw = _git_raw(
            ["-c", "core.quotePath=false", "ls-files", "-s", "-z", "--",
             f"{AUTHORITATIVE_ROOT}/"],
            root,
        )
    except RuntimeError as e:
        _fatal(f"cannot enumerate tracked ThirdParty paths: {e}")

    records = _split_nul(raw)
    if len(records) > MAX_TRACKED_PATHS:
        _fatal(
            f"tracked ThirdParty path count {len(records)} exceeds "
            f"MAX_TRACKED_PATHS ({MAX_TRACKED_PATHS})"
        )

    entries: list[TrackedEntry] = []
    for record in records:
        meta, _, path = record.partition("\t")
        if not path:
            _fatal(f"unparseable git ls-files record: {record!r}")
        parts = meta.split()
        if len(parts) != 3:
            _fatal(f"unparseable git ls-files metadata: {record!r}")
        entries.append(TrackedEntry(parts[0], parts[1], path))
    return entries


def git_worktree_status_thirdparty(root: Path) -> list[tuple[str, str]]:
    """Porcelain status entries under ThirdParty/ (untracked, modified, deleted)."""
    try:
        raw = _git_raw(
            ["-c", "core.quotePath=false", "status", "--porcelain", "-z",
             "--untracked-files=normal", "--", f"{AUTHORITATIVE_ROOT}/"],
            root,
        )
    except RuntimeError as e:
        _fatal(f"cannot read ThirdParty worktree status: {e}")

    # Porcelain -z emits fixed-width "XY <path>" records; a rename or copy adds
    # a second NUL-separated field holding the source path.
    fields = raw.split("\0")
    out: list[tuple[str, str]] = []
    index = 0
    while index < len(fields):
        record = fields[index]
        index += 1
        if len(record) < 4:
            continue
        code, path = record[:2], record[3:]
        if code[0] in ("R", "C"):
            index += 1  # consume the rename/copy source path
        out.append((code, path))
    return out


def git_blob_hashes(paths: list[str], root: Path) -> dict[str, str]:
    """Hash working-tree files as git would store them, in a single process.

    One `git hash-object` process per sentinel turns an N-entry lockfile into N
    process creations; batching keeps the gate bounded.
    """
    if not paths:
        return {}
    try:
        raw = _git_raw(
            ["hash-object", "--stdin-paths"], root, stdin="\n".join(paths) + "\n"
        )
    except RuntimeError as e:
        _fatal(f"git hash-object failed — cannot verify blob identity: {e}")

    hashes = raw.split()
    if len(hashes) != len(paths):
        _fatal(
            f"git hash-object returned {len(hashes)} hashes for "
            f"{len(paths)} paths — refusing to guess the mapping"
        )
    return dict(zip(paths, hashes))


# ── Link / reparse-point hygiene ──────────────────────────────────────

def link_reason(path: Path) -> str | None:
    """Return why `path` is a link or reparse point, or None if it is neither.

    Uses lstat, which never follows.  `Path.stat()` follows a Windows junction
    and reports FILE_ATTRIBUTE_REPARSE_POINT clear, so a stat-based reparse
    check can never fire for the thing it names.  An lstat failure is reported,
    never swallowed: the unfollowable cases (dangling junction, denied
    traverse, reparse cycle, offline placeholder) are exactly the ones a
    reparse check exists to catch.
    """
    try:
        st = os.lstat(path)
    except OSError as e:
        return f"cannot lstat path: {e}"
    if stat.S_ISLNK(st.st_mode):
        return "symbolic link rejected"
    attributes = getattr(st, "st_file_attributes", 0)
    reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    if reparse_flag and (attributes & reparse_flag):
        tag = getattr(st, "st_reparse_tag", 0)
        return f"reparse point (junction/link) rejected — tag 0x{tag:08x}"
    return None


def check_link_hygiene(root: Path, result: CheckResult) -> None:
    """Reject any symlink or reparse point anywhere under ThirdParty/.

    A junction redirects a whole managed directory at content-read time while
    every tracked-path check still sees the in-repo name.  The walk is bounded
    in depth and entry count and never descends through a rejected entry.
    """
    base = root / AUTHORITATIVE_ROOT
    if not base.is_dir():
        result.error("inventory", f"{AUTHORITATIVE_ROOT}/",
                     "ThirdParty directory missing")
        return

    reason = link_reason(base)
    if reason:
        result.error("link", f"{AUTHORITATIVE_ROOT}/", reason)
        return

    seen = 0
    stack: list[tuple[Path, str, int]] = [(base, AUTHORITATIVE_ROOT, 0)]
    while stack:
        current, current_rel, depth = stack.pop()
        if depth >= MAX_WALK_DEPTH:
            result.error(
                "bounds", current_rel,
                f"directory nesting exceeds MAX_WALK_DEPTH ({MAX_WALK_DEPTH})",
            )
            continue
        try:
            entries = sorted(os.scandir(current), key=lambda e: e.name)
        except OSError as e:
            result.error("inventory", current_rel, f"cannot list directory: {e}")
            continue

        for entry in entries:
            seen += 1
            if seen > MAX_WALK_ENTRIES:
                _fatal(
                    f"ThirdParty entry count exceeds MAX_WALK_ENTRIES "
                    f"({MAX_WALK_ENTRIES})"
                )
            rel = f"{current_rel}/{entry.name}"
            reason = link_reason(Path(entry.path))
            if reason:
                result.error("link", rel, reason)
                continue
            if entry.name == ".git":
                continue
            try:
                is_dir = entry.is_dir(follow_symlinks=False)
            except OSError as e:
                result.error("inventory", rel, f"cannot stat entry: {e}")
                continue
            if is_dir:
                stack.append((Path(entry.path), rel, depth + 1))


# ── Path safety ───────────────────────────────────────────────────────

def validate_repo_relative_path(
    rel_path: str, *, allowed_roots: frozenset[str] | None = None
) -> str | None:
    """Validate a repository-relative path. Returns error message or None."""
    if not isinstance(rel_path, str) or not rel_path:
        return "empty path"
    if os.path.isabs(rel_path):
        return "absolute path rejected"
    if "\\" in rel_path:
        return "backslash in path rejected"
    if rel_path.endswith("/"):
        return "trailing slash in path rejected"
    if not SAFE_PATH_RE.match(rel_path):
        return f"unsafe characters in path: {rel_path!r}"
    segments = rel_path.split("/")
    if ".." in segments:
        return "dot-segment (..) in path rejected"
    if "." in segments:
        return "dot-segment (.) in path rejected"
    if "" in segments:
        return "empty segment in path rejected"
    posix = PurePosixPath(rel_path)
    if allowed_roots and posix.parts[0] not in allowed_roots:
        return f"path root {posix.parts[0]!r} not in allowed roots {sorted(allowed_roots)}"
    return None


def assert_regular_file_no_escape(filepath: Path, root_resolved: Path) -> str | None:
    """Verify filepath is a regular, non-linked, contained file. Error or None."""
    reason = link_reason(filepath)
    if reason:
        return reason
    try:
        st = os.lstat(filepath)
    except OSError as e:
        return f"cannot stat file: {e}"
    if not stat.S_ISREG(st.st_mode):
        return "not a regular file"
    if getattr(st, "st_nlink", 1) > 1:
        return f"hardlinked file rejected (st_nlink={st.st_nlink})"
    try:
        resolved = filepath.resolve(strict=True)
    except OSError as e:
        return f"cannot resolve path: {e}"
    try:
        resolved.relative_to(root_resolved)
    except ValueError:
        return "path escapes repository root after resolution"
    return None


def _bounded_size(filepath: Path, limit: int, label: str) -> int:
    try:
        size = filepath.stat().st_size
    except OSError as e:
        _fatal(f"cannot stat {label}: {e}")
    if size > limit:
        _fatal(f"{label} is {size} bytes, exceeding the {limit}-byte limit")
    return size


def _bounded_read_text(filepath: Path, limit: int, label: str) -> str:
    _bounded_size(filepath, limit, label)
    try:
        return filepath.read_text(encoding="utf-8")
    except UnicodeDecodeError as e:
        _fatal(f"{label} is not valid UTF-8: {e}")
    except OSError as e:
        _fatal(f"cannot read {label}: {e}")
    raise AssertionError("unreachable")


# ── Lockfile loading and schema validation ────────────────────────────

def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    seen: dict[str, Any] = {}
    for key, value in pairs:
        if key in seen:
            _fatal(f"duplicate JSON key in lockfile: {key!r}")
        seen[key] = value
    return seen


def _json_depth(node: Any, depth: int = 0) -> int:
    if depth > MAX_JSON_DEPTH:
        return depth
    if isinstance(node, dict):
        return max((_json_depth(v, depth + 1) for v in node.values()), default=depth)
    if isinstance(node, list):
        return max((_json_depth(v, depth + 1) for v in node), default=depth)
    return depth


def load_lockfile(root: Path, root_resolved: Path) -> dict[str, Any]:
    lockpath = root / LOCKFILE_REL
    file_err = assert_regular_file_no_escape(lockpath, root_resolved)
    if file_err:
        _fatal(f"lockfile {LOCKFILE_REL}: {file_err}")
    text = _bounded_read_text(lockpath, MAX_LOCKFILE_BYTES, "lockfile")
    try:
        data = json.loads(text, object_pairs_hook=_reject_duplicate_keys)
    except json.JSONDecodeError as e:
        _fatal(f"cannot parse lockfile: {e}")
    except RecursionError:
        _fatal("lockfile JSON nesting is too deep to parse")
    if _json_depth(data) > MAX_JSON_DEPTH:
        _fatal(f"lockfile JSON nesting exceeds MAX_JSON_DEPTH ({MAX_JSON_DEPTH})")

    if data.get("version") != LOCKFILE_VERSION:
        _fatal(
            f"unsupported lockfile version: {data.get('version')!r} "
            f"(expected {LOCKFILE_VERSION}; regenerate with --update)"
        )
    validate_lockfile_schema(data)
    return data


def _validate_container_path(label: str, path: str) -> None:
    err = validate_repo_relative_path(path, allowed_roots=AUTHORITATIVE_ROOTS)
    if err:
        _fatal(f"{label} path {path!r}: {err}")


def validate_lockfile_schema(data: dict[str, Any]) -> None:
    """Structural validation. Any violation here is a checker-level failure."""
    for key in ("submodule_gitlinks", "sentinel_files", "tree_digests",
                "project_owned_dirs", "action_pins"):
        if not isinstance(data.get(key), dict):
            _fatal(f"lockfile missing or invalid object field: {key}")
    for key in ("managed_vendored_dirs", "allowed_root_files"):
        if not isinstance(data.get(key), list):
            _fatal(f"lockfile missing or invalid list field: {key}")

    gitlinks = data["submodule_gitlinks"]
    if len(gitlinks) > MAX_SUBMODULES:
        _fatal(f"submodule count exceeds MAX_SUBMODULES ({MAX_SUBMODULES})")
    for path, sha in gitlinks.items():
        _validate_container_path("submodule_gitlinks", path)
        if not isinstance(sha, str) or not SHA40_HEX_RE.match(sha):
            _fatal(f"submodule_gitlinks[{path!r}]: invalid SHA: {sha!r}")

    sentinels = data["sentinel_files"]
    if len(sentinels) > MAX_SENTINELS:
        _fatal(f"sentinel count exceeds MAX_SENTINELS ({MAX_SENTINELS})")
    for path, entry in sentinels.items():
        _validate_container_path("sentinel_files", path)
        if not isinstance(entry, dict):
            _fatal(f"sentinel_files[{path!r}]: entry must be an object")
        sha256 = entry.get("sha256")
        if not isinstance(sha256, str) or not SHA256_HEX_RE.match(sha256):
            _fatal(f"sentinel_files[{path!r}]: invalid sha256: {sha256!r}")
        git_blob = entry.get("git_blob")
        if not isinstance(git_blob, str) or not SHA40_HEX_RE.match(git_blob):
            _fatal(f"sentinel_files[{path!r}]: invalid git_blob: {git_blob!r}")
        size = entry.get("size")
        if not isinstance(size, int) or isinstance(size, bool) or size < 0:
            _fatal(f"sentinel_files[{path!r}]: invalid size: {size!r}")
        if size > MAX_SENTINEL_BYTES:
            _fatal(f"sentinel_files[{path!r}]: size exceeds MAX_SENTINEL_BYTES")
        if entry.get("type") not in ("license", "source"):
            _fatal(f"sentinel_files[{path!r}]: invalid type: {entry.get('type')!r}")

    managed = data["managed_vendored_dirs"]
    for path in managed:
        _validate_container_path("managed_vendored_dirs", path)
    if len(managed) != len(set(managed)):
        _fatal("managed_vendored_dirs contains duplicate entries")

    owned = data["project_owned_dirs"]
    for path, justification in owned.items():
        _validate_container_path("project_owned_dirs", path)
        if not isinstance(justification, str) or len(justification.strip()) < 16:
            _fatal(
                f"project_owned_dirs[{path!r}]: a first-party exemption requires "
                "a recorded justification of at least 16 characters"
            )

    containers = list(managed) + list(owned) + list(gitlinks)
    if len(containers) > MAX_CONTAINERS:
        _fatal(f"container count exceeds MAX_CONTAINERS ({MAX_CONTAINERS})")

    digests = data["tree_digests"]
    for path, entry in digests.items():
        _validate_container_path("tree_digests", path)
        if not isinstance(entry, dict):
            _fatal(f"tree_digests[{path!r}]: entry must be an object")
        digest = entry.get("digest")
        if not isinstance(digest, str) or not SHA256_HEX_RE.match(digest):
            _fatal(f"tree_digests[{path!r}]: invalid digest: {digest!r}")
        count = entry.get("file_count")
        if not isinstance(count, int) or isinstance(count, bool) or count < 0:
            _fatal(f"tree_digests[{path!r}]: invalid file_count: {count!r}")

    allowed_root_files = data["allowed_root_files"]
    for path in allowed_root_files:
        _validate_container_path("allowed_root_files", path)
        if PurePosixPath(path).parent != PurePosixPath(AUTHORITATIVE_ROOT):
            _fatal(f"allowed_root_files[{path!r}] is not a ThirdParty root file")

    pins = data["action_pins"]
    if len(pins) > MAX_ACTION_PIN_KEYS:
        _fatal(f"action_pins count exceeds MAX_ACTION_PIN_KEYS ({MAX_ACTION_PIN_KEYS})")
    for repo, shas in pins.items():
        if not isinstance(repo, str) or not re.fullmatch(
            r"[A-Za-z0-9][A-Za-z0-9._-]*/[A-Za-z0-9._-]+", repo
        ):
            _fatal(f"action_pins key is not an owner/repo slug: {repo!r}")
        if not isinstance(shas, list) or not shas:
            _fatal(f"action_pins[{repo!r}]: expected a non-empty list of SHAs")
        if len(shas) > MAX_ACTION_PIN_SHAS:
            _fatal(f"action_pins[{repo!r}]: too many pinned SHAs")
        for sha in shas:
            if not isinstance(sha, str) or not SHA40_HEX_RE.match(sha):
                _fatal(f"action_pins[{repo!r}]: invalid SHA: {sha!r}")


# ── Container model ───────────────────────────────────────────────────

def containers_of(lockfile: dict[str, Any]) -> dict[str, str]:
    """Map every declared container path to its category."""
    out: dict[str, str] = {}
    for path in lockfile["managed_vendored_dirs"]:
        out[path] = "managed_vendored"
    for path in lockfile["project_owned_dirs"]:
        out[path] = "project_owned"
    for path in lockfile["submodule_gitlinks"]:
        out[path] = "submodule"
    return out


def check_container_model(lockfile: dict[str, Any], result: CheckResult) -> None:
    """Categories must be disjoint and non-nesting.

    Overlap lets one lockfile edit move a directory into the weakest category
    while every other check keeps passing; nesting lets a child's coverage be
    claimed by its parent.
    """
    managed = set(lockfile["managed_vendored_dirs"])
    owned = set(lockfile["project_owned_dirs"])
    submodules = set(lockfile["submodule_gitlinks"])

    for left_name, left, right_name, right in (
        ("managed_vendored_dirs", managed, "project_owned_dirs", owned),
        ("managed_vendored_dirs", managed, "submodule_gitlinks", submodules),
        ("project_owned_dirs", owned, "submodule_gitlinks", submodules),
    ):
        for path in sorted(left & right):
            result.error(
                "container", path,
                f"path declared in both {left_name} and {right_name} — "
                "categories must be disjoint",
            )

    all_paths = sorted(managed | owned | submodules)
    for path in all_paths:
        for other in all_paths:
            if other != path and path.startswith(other + "/"):
                result.error(
                    "container", path,
                    f"container is nested inside container {other!r} — "
                    "nesting hides the child's coverage inside the parent",
                )

    lowered: dict[str, str] = {}
    for path in all_paths:
        key = path.casefold()
        if key in lowered and lowered[key] != path:
            result.error(
                "container", path,
                f"case-variant alias of container {lowered[key]!r} — "
                "aliases verify on case-insensitive filesystems only",
            )
        lowered.setdefault(key, path)


# ── Check: complete tracked inventory and tree digests ────────────────

def _assign_container(path: str, containers: dict[str, str]) -> str | None:
    """Longest declared container prefix owning `path`, or None."""
    best: str | None = None
    for container in containers:
        if path == container or path.startswith(container + "/"):
            if best is None or len(container) > len(best):
                best = container
    return best


def _tree_digest(entries: Iterable[TrackedEntry]) -> tuple[str, int]:
    """Deterministic digest over the tracked (mode, blob, path) triples."""
    digest = hashlib.sha256()
    count = 0
    for entry in sorted(entries, key=lambda e: e.path):
        digest.update(f"{entry.mode} {entry.blob} {entry.path}\n".encode("utf-8"))
        count += 1
    return digest.hexdigest(), count


def check_tracked_inventory(
    root: Path,
    lockfile: dict[str, Any],
    tracked: list[TrackedEntry],
    result: CheckResult,
) -> dict[str, list[TrackedEntry]]:
    """Account for every tracked path under ThirdParty/.

    The previous inventory walked two directory levels and skipped dot-entries,
    so anything at depth >= 2, any file at depth 1, and any dot-path was
    invisible.  Driving the inventory from the git index removes the blind
    spots by construction: the set enumerated is exactly the set the policy
    governs.
    """
    containers = containers_of(lockfile)
    allowed_root_files = set(lockfile["allowed_root_files"])
    submodules = lockfile["submodule_gitlinks"]
    assigned: dict[str, list[TrackedEntry]] = {c: [] for c in containers}

    for entry in tracked:
        if entry.mode == GIT_SYMLINK_MODE:
            result.error(
                "inventory", entry.path,
                "tracked symlink under ThirdParty rejected",
            )
            continue
        if entry.mode == GIT_GITLINK_MODE:
            if containers.get(entry.path) != "submodule":
                result.error(
                    "inventory", entry.path,
                    "submodule gitlink not declared in supply-chain.lock",
                )
            elif submodules[entry.path] != entry.blob:
                result.error(
                    "submodule", entry.path,
                    f"gitlink drift: lockfile={submodules[entry.path]}, "
                    f"repository={entry.blob}",
                )
            else:
                assigned[entry.path].append(entry)
            continue
        if entry.mode not in GIT_FILE_MODES:
            result.error(
                "inventory", entry.path,
                f"unexpected git mode {entry.mode} — refusing to classify",
            )
            continue

        if entry.path in allowed_root_files:
            continue

        owner = _assign_container(entry.path, containers)
        if owner is None:
            result.error(
                "inventory", entry.path,
                "tracked file is in no declared container — add its directory "
                "to supply-chain.lock, or the file to allowed_root_files",
            )
            continue
        if containers[owner] == "submodule":
            result.error(
                "inventory", entry.path,
                f"tracked file inside submodule container {owner!r} — "
                "submodule content must live in the submodule, not the superproject",
            )
            continue
        assigned[owner].append(entry)

    for path in sorted(submodules):
        if containers.get(path) == "submodule" and not assigned.get(path):
            result.error(
                "submodule", path,
                "declared submodule has no gitlink in the repository tree",
            )

    for container, category in sorted(containers.items()):
        if category == "submodule":
            continue
        if not assigned[container]:
            result.error(
                "container", container,
                "declared container holds no tracked files — remove it or "
                "populate it; an empty container pre-authorizes a future path",
            )
        directory = root / container
        if not directory.is_dir():
            result.error(
                "container", container,
                "declared container directory does not exist on disk",
            )

    # Payload coverage is pinned against the git index, so the working tree
    # must agree with it. Governance files at the ThirdParty root are exempt:
    # they are verified by reading their working-tree content directly, and
    # --update legitimately rewrites the lockfile in place.
    for code, path in git_worktree_status_thirdparty(root):
        if path in allowed_root_files:
            continue
        result.error(
            "worktree", path,
            f"ThirdParty worktree differs from the index ({code}) — "
            "untracked, modified, or deleted payload is unverifiable",
        )

    return assigned


def check_tree_digests(
    lockfile: dict[str, Any],
    assigned: dict[str, list[TrackedEntry]],
    result: CheckResult,
) -> None:
    """Pin the complete tracked payload of every non-submodule container.

    Sentinels sample a directory; a digest covers it.  Without this, a
    directory listed in the lockfile was skipped wholesale and its content
    could be replaced with a single sentinel left untouched.
    """
    containers = containers_of(lockfile)
    expected = lockfile["tree_digests"]
    non_submodule = {c for c, cat in containers.items() if cat != "submodule"}

    for container in sorted(non_submodule):
        record = expected.get(container)
        if record is None:
            result.error(
                "digest", container,
                "container has no tree digest — its payload is unverified",
            )
            continue
        digest, count = _tree_digest(assigned.get(container, []))
        if digest != record["digest"]:
            result.error(
                "digest", container,
                f"tracked tree digest drift: lockfile={record['digest'][:16]}..., "
                f"actual={digest[:16]}... ({count} tracked files)",
            )
        elif count != record["file_count"]:
            result.error(
                "digest", container,
                f"tracked file count drift: lockfile={record['file_count']}, "
                f"actual={count}",
            )

    for container in sorted(set(expected) - non_submodule):
        result.error(
            "digest", container,
            "tree digest for a path that is not a declared non-submodule "
            "container",
        )


# ── Check: sentinel files ─────────────────────────────────────────────

def _content_sha256(filepath: Path, size_limit: int) -> tuple[str, int] | None:
    """Stream-hash a file, refusing to read past size_limit bytes."""
    digest = hashlib.sha256()
    read = 0
    try:
        with open(filepath, "rb") as handle:
            while True:
                chunk = handle.read(65536)
                if not chunk:
                    break
                read += len(chunk)
                if read > size_limit:
                    return None
                digest.update(chunk)
    except OSError:
        return None
    return digest.hexdigest(), read


def check_sentinel_files(
    root: Path,
    root_resolved: Path,
    lockfile: dict[str, Any],
    result: CheckResult,
) -> None:
    sentinels = lockfile["sentinel_files"]
    if not sentinels:
        result.error("sentinel", LOCKFILE_REL, "no sentinel files in lockfile")
        return

    containers = containers_of(lockfile)
    verifiable: list[str] = []

    for rel_path, expected in sorted(sentinels.items()):
        if _assign_container(rel_path, containers) is None:
            result.error(
                "sentinel", rel_path,
                "sentinel is in no declared container",
            )
            continue

        filepath = root / rel_path
        file_err = assert_regular_file_no_escape(filepath, root_resolved)
        if file_err:
            result.error("sentinel", rel_path, f"sentinel file: {file_err}")
            continue

        # Size first: a stat is one syscall, and hashing an oversized planted
        # file before comparing sizes is unbounded work for a known answer.
        actual_size = filepath.stat().st_size
        if actual_size != expected["size"]:
            result.error(
                "integrity", rel_path,
                f"size mismatch: lockfile={expected['size']}, actual={actual_size}",
            )
            continue

        hashed = _content_sha256(filepath, MAX_SENTINEL_BYTES)
        if hashed is None:
            result.error(
                "integrity", rel_path,
                "cannot hash sentinel file within the size limit",
            )
            continue
        if hashed[0] != expected["sha256"]:
            result.error(
                "integrity", rel_path,
                f"content hash mismatch: lockfile={expected['sha256'][:16]}..., "
                f"actual={hashed[0][:16]}...",
            )
            continue

        verifiable.append(rel_path)
        if expected["type"] == "license":
            _check_license_content(filepath, rel_path, result)

    blobs = git_blob_hashes(verifiable, root)
    for rel_path in verifiable:
        expected_blob = sentinels[rel_path]["git_blob"]
        actual_blob = blobs.get(rel_path, "")
        if not actual_blob:
            result.error(
                "integrity", rel_path,
                "git hash-object produced no hash — cannot verify blob identity",
            )
        elif actual_blob != expected_blob:
            result.error(
                "integrity", rel_path,
                f"git blob drift: lockfile={expected_blob[:16]}..., "
                f"actual={actual_blob[:16]}...",
            )


def _check_license_content(
    filepath: Path, rel_path: str, result: CheckResult
) -> None:
    size = filepath.stat().st_size
    if size < MIN_LICENSE_SIZE:
        result.error(
            "license", rel_path,
            f"license file implausibly short ({size} bytes)",
        )
        return
    if size > MAX_LICENSE_BYTES:
        result.error(
            "license", rel_path,
            f"license file exceeds MAX_LICENSE_BYTES ({MAX_LICENSE_BYTES})",
        )
        return
    try:
        text = filepath.read_text(encoding="utf-8", errors="replace")
    except OSError:
        result.error("license", rel_path, "cannot read license file")
        return
    if not LICENSE_KEYWORDS_COPYRIGHT.search(text):
        result.error("license", rel_path, "license lacks copyright statement")
    if not LICENSE_KEYWORDS_TERMS.search(text):
        result.error("license", rel_path, "license lacks operative terms")


# ── Check: GitHub Actions pinning ─────────────────────────────────────

def _load_yaml_module() -> Any:
    try:
        import yaml  # noqa: PLC0415 — optional dependency, checked at use site
    except ImportError:
        _fatal(
            "PyYAML is required to verify GitHub Actions pinning. "
            "A regex scan of `uses:` lines cannot tell a mapping key from "
            "script text, and degrading to one would report a clean result it "
            "never established. Install python3-yaml / pyyaml."
        )
    return yaml


def _iter_uses(node: Any, trail: str = "") -> Iterable[tuple[str, Any]]:
    """Yield every `uses` value anywhere in a parsed workflow document.

    Quoting, flow style, block scalars, anchors, aliases, merge keys, explicit
    keys, and line continuations all resolve to the same object here, so the
    whole family of line-regex bypasses collapses into one code path.  Text
    that merely looks like `uses:` inside a `run:` script is a string value,
    never a mapping key, so it is correctly ignored.
    """
    if isinstance(node, dict):
        for key, value in node.items():
            child = f"{trail}.{key}" if trail else str(key)
            if key == "uses":
                yield child, value
            else:
                yield from _iter_uses(value, child)
    elif isinstance(node, list):
        for index, value in enumerate(node):
            yield from _iter_uses(value, f"{trail}[{index}]")


def _workflow_and_action_files(root: Path) -> list[str]:
    """Every workflow and composite-action definition tracked in the repo.

    A composite action executes `uses:` entries of its own; scanning only
    .github/workflows leaves those unpinned and unexamined.
    """
    try:
        raw = _git_raw(
            ["-c", "core.quotePath=false", "ls-files", "-z", "--",
             f"{WORKFLOWS_DIR}/", "*action.yml", "*action.yaml"],
            root,
        )
    except RuntimeError as e:
        _fatal(f"cannot enumerate workflow files: {e}")

    files: list[str] = []
    for path in sorted(_split_nul(raw)):
        name = PurePosixPath(path).name
        if path.startswith(WORKFLOWS_DIR + "/"):
            if not name.endswith((".yml", ".yaml")):
                continue
        elif name not in ("action.yml", "action.yaml"):
            continue
        files.append(path)
    if len(files) > MAX_WORKFLOW_FILES:
        _fatal(f"workflow file count exceeds MAX_WORKFLOW_FILES ({MAX_WORKFLOW_FILES})")
    return files


def check_action_pins(
    root: Path, root_resolved: Path, lockfile: dict[str, Any], result: CheckResult
) -> None:
    yaml = _load_yaml_module()
    workflows_dir = root / WORKFLOWS_DIR
    if not workflows_dir.is_dir():
        # Not a warning: a missing directory means pin enforcement did not run.
        result.error("actions", WORKFLOWS_DIR, "workflows directory not found")
        return

    files = _workflow_and_action_files(root)
    if not files:
        result.error("actions", WORKFLOWS_DIR, "no workflow files found to verify")
        return

    pins: dict[str, list[str]] = lockfile["action_pins"]
    observed: set[str] = set()

    for rel in files:
        filepath = root / rel
        file_err = assert_regular_file_no_escape(filepath, root_resolved)
        if file_err:
            result.error("actions", rel, f"workflow file: {file_err}")
            continue
        _bounded_size(filepath, MAX_WORKFLOW_BYTES, f"workflow {rel}")
        try:
            text = filepath.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as e:
            result.error("actions", rel, f"cannot decode workflow file: {e}")
            continue
        try:
            documents = list(yaml.safe_load_all(text))
        except yaml.YAMLError as e:
            result.error("actions", rel, f"cannot parse workflow YAML: {e}")
            continue

        for document in documents:
            for trail, value in _iter_uses(document):
                _check_one_use(root, rel, trail, value, pins, observed, result)

    for repo in sorted(set(pins) - observed):
        result.warn(
            "actions", LOCKFILE_REL,
            f"action_pins entry {repo!r} is not referenced by any workflow",
        )


def _check_one_use(
    root: Path,
    rel: str,
    trail: str,
    value: Any,
    pins: dict[str, list[str]],
    observed: set[str],
    result: CheckResult,
) -> None:
    where = f"{rel}:{trail}"
    if not isinstance(value, str):
        result.error("actions", where, f"uses value is not a string: {value!r}")
        return
    ref = value.strip()
    if not ref:
        result.error("actions", where, "empty uses value")
        return
    if EXPRESSION_RE.search(ref):
        result.error(
            "actions", where,
            f"uses value is computed at runtime and cannot be pinned: {ref}",
        )
        return

    if ref.startswith("./") or ref.startswith("../"):
        _check_local_action(root, where, ref, result)
        return

    if ref.startswith("docker://"):
        if not DOCKER_REF_RE.match(ref):
            result.error(
                "actions", where,
                f"docker action must be digest-pinned as "
                f"docker://<image>@sha256:<64 hex>: {ref}",
            )
        return

    match = ACTION_REF_RE.match(ref)
    if not match:
        result.error(
            "actions", where,
            f"unpinned action: {ref} — must be owner/repo[/path]@<40 lowercase hex>",
        )
        return

    repo = match.group("repo")
    sha = match.group("sha")
    observed.add(repo)
    allowed = pins.get(repo)
    if allowed is None:
        result.error(
            "actions", where,
            f"action {repo!r} is not in supply-chain.lock action_pins — "
            "a SHA pin answers 'did it change', not 'whose is it'",
        )
    elif sha not in allowed:
        result.error(
            "actions", where,
            f"action {repo!r} pinned to {sha} which is not a recorded pin "
            f"({', '.join(s[:12] + '...' for s in allowed)})",
        )


def _check_local_action(
    root: Path, where: str, ref: str, result: CheckResult
) -> None:
    candidate = ref[2:] if ref.startswith("./") else ref
    err = validate_repo_relative_path(candidate)
    if err:
        result.error("actions", where, f"local action path {ref}: {err}")
        return
    directory = root / candidate
    if not any((directory / name).is_file()
               for name in ("action.yml", "action.yaml")):
        result.error(
            "actions", where,
            f"local action {ref} has no action.yml/action.yaml",
        )


# ── Check: .gitmodules consistency ────────────────────────────────────

def _parse_gitmodules(root: Path, root_resolved: Path) -> dict[str, dict[str, str]]:
    gitmodules = root / GITMODULES_REL
    file_err = assert_regular_file_no_escape(gitmodules, root_resolved)
    if file_err:
        _fatal(f"{GITMODULES_REL}: {file_err}")
    # `git config --get-regexp` exits 1 with empty output when nothing matches,
    # which is a legitimate state (a repository with no submodules). Only a
    # different failure, or exit 1 with output, is a checker-level error.
    try:
        completed = subprocess.run(
            ["git", "config", "--file", GITMODULES_REL, "-z", "--get-regexp",
             r"^submodule\..*\.(path|url|branch)$"],
            capture_output=True, text=True, cwd=str(root), timeout=120,
        )
    except (OSError, subprocess.TimeoutExpired) as e:
        _fatal(f"git config --file .gitmodules failed: {e}")
    if completed.returncode not in (0, 1) or (
        completed.returncode == 1 and completed.stdout.strip()
    ):
        _fatal(
            f"git config --file .gitmodules exited {completed.returncode}: "
            f"{completed.stderr.strip()}"
        )
    raw = completed.stdout

    modules: dict[str, dict[str, str]] = {}
    for record in _split_nul(raw):
        key, _, value = record.partition("\n")
        parts = key.split(".")
        if len(parts) < 3:
            _fatal(f"unparseable .gitmodules key: {key!r}")
        name = ".".join(parts[1:-1])
        modules.setdefault(name, {})[parts[-1]] = value
    return modules


def check_gitmodules_consistency(
    modules: dict[str, dict[str, str]],
    lockfile: dict[str, Any],
    result: CheckResult,
) -> None:
    declared = lockfile["submodule_gitlinks"]
    seen_paths: dict[str, str] = {}

    for name, fields in sorted(modules.items()):
        path = fields.get("path", "")
        if not path:
            result.error(
                "gitmodules", f"submodule.{name}",
                "submodule declares no path — silently dropping it would hide it",
            )
            continue
        if path in seen_paths:
            result.error(
                "gitmodules", path,
                f"path declared by both submodule {seen_paths[path]!r} and {name!r}",
            )
            continue
        seen_paths[path] = name

        err = validate_repo_relative_path(path, allowed_roots=AUTHORITATIVE_ROOTS)
        if err:
            result.error("gitmodules", path, f"submodule path: {err}")
            continue
        if path not in declared:
            result.error(
                "gitmodules", path,
                "submodule in .gitmodules but not in supply-chain.lock",
            )

        url = fields.get("url", "")
        if not url:
            result.error("gitmodules", path, "submodule declares no url")
        elif not url.startswith("https://"):
            result.error(
                "gitmodules", path,
                f"submodule url must be https://: {url}",
            )

    for path in sorted(declared):
        if path not in seen_paths:
            result.error(
                "gitmodules", path,
                "submodule in supply-chain.lock but not in .gitmodules",
            )


# ── Check: dependencies.lock reconciliation ───────────────────────────

def export_manifest_entries(root: Path) -> list[list[str]]:
    """Read the manifest through CMake, the parser that actually evaluates it.

    A text scrape sees neither `list(APPEND ...)` after the closing paren, nor
    a record split across lines, nor a variable expansion — and reports zero
    findings for all three.
    """
    cmake = shutil.which("cmake")
    if not cmake:
        _fatal(
            "cmake is required to expand ThirdParty/dependencies.lock. "
            "Reconciling against a text scrape of the manifest would check a "
            "different file than the build reads."
        )

    manifest = root / MANIFEST_REL
    if not manifest.is_file():
        _fatal(f"{MANIFEST_REL} not found")
    _bounded_size(manifest, MAX_MANIFEST_BYTES, "dependency manifest")

    handle, out_path = tempfile.mkstemp(prefix="spark-tp-entries-", suffix=".txt")
    os.close(handle)
    try:
        completed = subprocess.run(
            [
                cmake,
                "-DSPARK_THIRDPARTY_AUDIT_VALIDATE_ONLY=ON",
                f"-DSPARK_THIRDPARTY_MANIFEST={(root / MANIFEST_REL).as_posix()}",
                f"-DSPARK_THIRDPARTY_ENTRIES_OUTPUT={Path(out_path).as_posix()}",
                "-P", str(root / AUDIT_CMAKE_REL),
            ],
            capture_output=True, text=True, cwd=str(root), timeout=300,
        )
        if completed.returncode != 0:
            _fatal(
                "cmake rejected ThirdParty/dependencies.lock:\n"
                f"{completed.stderr.strip()}"
            )
        exported = Path(out_path)
        _bounded_size(exported, MAX_ENTRIES_EXPORT_BYTES, "expanded manifest")
        text = exported.read_text(encoding="utf-8")
    except (OSError, subprocess.TimeoutExpired) as e:
        _fatal(f"cannot expand the dependency manifest via cmake: {e}")
    finally:
        try:
            os.unlink(out_path)
        except OSError:
            pass

    entries = [line for line in text.splitlines() if line.strip()]
    if not entries:
        _fatal("expanded dependency manifest is empty")
    if len(entries) > MAX_MANIFEST_ENTRIES:
        _fatal(f"manifest entry count exceeds MAX_MANIFEST_ENTRIES")
    return [entry.split("|") for entry in entries]


def check_manifest_reconciliation(
    root: Path,
    lockfile: dict[str, Any],
    entries: list[list[str]],
    modules: dict[str, dict[str, str]],
    result: CheckResult,
) -> None:
    containers = containers_of(lockfile)
    sentinels = lockfile["sentinel_files"]
    submodule_urls = {
        fields["path"]: fields.get("url", "")
        for fields in modules.values()
        if fields.get("path")
    }

    seen_names: dict[str, str] = {}
    seen_paths: dict[str, str] = {}
    seen_folded: dict[str, str] = {}
    declared_paths: set[str] = set()

    for fields in entries:
        if len(fields) != MANIFEST_FIELD_COUNT:
            result.error(
                "manifest", MANIFEST_REL,
                f"malformed manifest entry (expected {MANIFEST_FIELD_COUNT} "
                f"fields, got {len(fields)}): {'|'.join(fields)[:80]}...",
            )
            continue

        name = fields[F_NAME].strip()
        local_path = fields[F_LOCAL_PATH].strip()
        if not name:
            result.error("manifest", MANIFEST_REL, "manifest entry has no name")
            continue
        if name in seen_names:
            result.error(
                "manifest", MANIFEST_REL,
                f"duplicate dependency name {name!r} — later entries silently "
                "replaced earlier ones and were never reconciled",
            )
            continue
        seen_names[name] = local_path

        path_err = validate_repo_relative_path(
            local_path, allowed_roots=AUTHORITATIVE_ROOTS
        )
        if path_err:
            result.error(
                "manifest", local_path or name,
                f"dependency {name!r} local path: {path_err} — vendored code "
                f"must live under {AUTHORITATIVE_ROOT}/",
            )
            continue
        if local_path in seen_paths:
            result.error(
                "manifest", local_path,
                f"dependency {name!r} shares its path with {seen_paths[local_path]!r}",
            )
            continue
        folded = local_path.casefold()
        if folded in seen_folded:
            result.error(
                "manifest", local_path,
                f"dependency {name!r} is a case-variant alias of "
                f"{seen_folded[folded]!r}",
            )
            continue
        seen_paths[local_path] = name
        seen_folded[folded] = local_path
        declared_paths.add(local_path)

        category = containers.get(local_path)
        if category is None:
            result.error(
                "manifest", local_path,
                f"dependency {name!r} path is not an exact container in "
                "supply-chain.lock",
            )
            continue
        if category == "project_owned":
            result.error(
                "manifest", local_path,
                f"dependency {name!r} points at a project-owned directory — "
                "a first-party path cannot also be a third-party dependency",
            )

        _check_manifest_fields(
            root, name, fields, category, submodule_urls, lockfile, result
        )

        for notice in fields[F_NOTICES].split(","):
            notice = notice.strip()
            if not notice:
                result.error(
                    "manifest", local_path,
                    f"dependency {name!r} has an empty license notice path",
                )
                continue
            notice_err = validate_repo_relative_path(
                notice, allowed_roots=AUTHORITATIVE_ROOTS
            )
            if notice_err:
                result.error(
                    "manifest", notice,
                    f"dependency {name!r} license notice: {notice_err}",
                )
                continue
            entry = sentinels.get(notice)
            if entry is None:
                result.error(
                    "manifest", notice,
                    "license notice in dependencies.lock is not a sentinel in "
                    "supply-chain.lock — its content is unpinned",
                )
            elif entry["type"] != "license":
                result.error(
                    "manifest", notice,
                    f"license notice is recorded as type {entry['type']!r}; "
                    "only 'license' sentinels get content validation",
                )

    # The direction that was missing entirely: every third-party container must
    # have a manifest entry recording its provenance.
    for container, category in sorted(containers.items()):
        if category == "project_owned":
            continue
        if container not in declared_paths:
            result.error(
                "manifest", container,
                f"{category} container has no dependencies.lock entry — no "
                "recorded source, version, license, or notice file",
            )


def _looks_like_repo_path(root: Path, token: str) -> bool:
    """Distinguish a fallback that names a source file from one that is prose.

    Several fallbacks are legitimately descriptive ("NullRHI/headless fallback
    when OpenGL loader missing"). Requiring the leading segment to be a real
    top-level directory keeps prose out of the existence check without letting
    a genuinely broken path slip through as prose.
    """
    if "/" not in token or validate_repo_relative_path(token) is not None:
        return False
    return (root / PurePosixPath(token).parts[0]).is_dir()


def _check_manifest_fields(
    root: Path,
    name: str,
    fields: list[str],
    category: str,
    submodule_urls: dict[str, str],
    lockfile: dict[str, Any],
    result: CheckResult,
) -> None:
    local_path = fields[F_LOCAL_PATH].strip()
    source = fields[F_SOURCE].strip()
    version = fields[F_VERSION].strip()

    if not source.startswith("https://"):
        result.error(
            "manifest", local_path,
            f"dependency {name!r} source must be an https:// URL: {source!r}",
        )
    elif category == "submodule":
        declared_url = submodule_urls.get(local_path, "")
        if declared_url and source.split()[0] != declared_url:
            result.error(
                "manifest", local_path,
                f"dependency {name!r} source {source.split()[0]!r} does not "
                f"match the .gitmodules url {declared_url!r}",
            )

    if category == "submodule":
        expected_sha = lockfile["submodule_gitlinks"].get(local_path, "")
        if not SHA40_HEX_RE.match(version):
            result.error(
                "manifest", local_path,
                f"dependency {name!r} version must expand to the 40-hex gitlink "
                f"revision, got {version!r}",
            )
        elif version != expected_sha:
            result.error(
                "manifest", local_path,
                f"dependency {name!r} version {version} does not match the "
                f"locked gitlink {expected_sha}",
            )
    elif not version:
        result.error(
            "manifest", local_path, f"dependency {name!r} has no version",
        )

    if not fields[F_LICENSE].strip():
        result.error("manifest", local_path, f"dependency {name!r} has no license")
    if not fields[F_FEATURE_MACRO].strip():
        result.error(
            "manifest", local_path, f"dependency {name!r} has no feature macro",
        )

    severity = fields[F_SEVERITY].strip()
    if severity not in VALID_SEVERITIES:
        result.error(
            "manifest", local_path,
            f"dependency {name!r} severity {severity!r} is not one of "
            f"{sorted(VALID_SEVERITIES)}",
        )

    required = [f.strip() for f in fields[F_REQUIRED_FILES].split(",") if f.strip()]
    if not required:
        result.error(
            "manifest", local_path,
            f"dependency {name!r} declares no required files",
        )
    for required_file in required:
        err = validate_repo_relative_path(f"{local_path}/{required_file}")
        if err:
            result.error(
                "manifest", local_path,
                f"dependency {name!r} required file {required_file!r}: {err}",
            )
            continue
        if category == "submodule" and not (root / local_path / ".git").exists():
            # Submodule not checked out. Its content is pinned by the gitlink,
            # which check_tracked_inventory verifies; requiring files that were
            # deliberately not fetched would fail the gate for being correct.
            continue
        if not (root / local_path / required_file).exists():
            result.error(
                "manifest", f"{local_path}/{required_file}",
                f"dependency {name!r} required file does not exist",
            )

    fallback = fields[F_FALLBACK].strip()
    if not fallback:
        result.error(
            "manifest", local_path, f"dependency {name!r} declares no fallback",
        )
    else:
        token = fallback.split()[0]
        if _looks_like_repo_path(root, token) and not (root / token).exists():
            result.error(
                "manifest", token,
                f"dependency {name!r} names a fallback path that does not exist",
            )


# ── Lockfile regeneration ─────────────────────────────────────────────

def update_lockfile(root: Path, root_resolved: Path, *, quiet: bool = False) -> dict[str, Any]:
    """Rebuild every derived field from repository state.

    The previous implementation seeded the sentinel list from the file it was
    replacing, so it could only ever narrow coverage, and carried the existing
    managed-directory set through untouched, laundering it into a file that
    looked freshly derived.  Sentinels are now discovered from the manifest's
    own required files and license notices; the declared containers are still
    author-controlled, but they are schema-validated before use and fully
    re-verified afterwards.
    """
    lockpath = root / LOCKFILE_REL
    if not lockpath.is_file():
        _fatal(
            f"{LOCKFILE_REL} not found. --update refreshes derived fields; it "
            "cannot invent the container declarations that carry the policy "
            "decisions. Restore the lockfile from version control first."
        )
    file_err = assert_regular_file_no_escape(lockpath, root_resolved)
    if file_err:
        _fatal(f"lockfile {LOCKFILE_REL}: {file_err}")

    text = _bounded_read_text(lockpath, MAX_LOCKFILE_BYTES, "lockfile")
    try:
        existing = json.loads(text, object_pairs_hook=_reject_duplicate_keys)
    except json.JSONDecodeError as e:
        _fatal(f"cannot parse existing lockfile: {e}")
    if existing.get("version") != LOCKFILE_VERSION:
        _fatal(
            f"existing lockfile is version {existing.get('version')!r}; "
            f"this tool writes version {LOCKFILE_VERSION}"
        )
    validate_lockfile_schema(existing)

    tracked = git_tracked_thirdparty(root)
    gitlinks = {
        entry.path: entry.blob
        for entry in tracked
        if entry.mode == GIT_GITLINK_MODE
    }

    managed = sorted(existing["managed_vendored_dirs"])
    owned = dict(sorted(existing["project_owned_dirs"].items()))
    containers = {
        **{p: "managed_vendored" for p in managed},
        **{p: "project_owned" for p in owned},
        **{p: "submodule" for p in gitlinks},
    }

    assigned: dict[str, list[TrackedEntry]] = {c: [] for c in containers}
    for entry in tracked:
        if entry.mode == GIT_GITLINK_MODE:
            continue
        if entry.path in set(existing["allowed_root_files"]):
            continue
        owner = _assign_container(entry.path, containers)
        if owner is not None and containers[owner] != "submodule":
            assigned[owner].append(entry)

    digests: dict[str, dict[str, Any]] = {}
    for container in sorted(c for c, cat in containers.items() if cat != "submodule"):
        digest, count = _tree_digest(assigned[container])
        digests[container] = {"digest": digest, "file_count": count}

    sentinel_paths = _discover_sentinel_paths(root, existing, containers)
    sentinels = _build_sentinels(root, root_resolved, sentinel_paths, quiet=quiet)
    action_pins = _discover_action_pins(root, root_resolved)

    lockfile_data = {
        "version": LOCKFILE_VERSION,
        "description": existing.get(
            "description",
            "Authoritative supply-chain content lockfile. Verified by "
            "tools/check-supply-chain.py on every CI run. Update with: "
            "python tools/check-supply-chain.py --update",
        ),
        "submodule_gitlinks": dict(sorted(gitlinks.items())),
        "managed_vendored_dirs": managed,
        "project_owned_dirs": owned,
        "allowed_root_files": sorted(existing["allowed_root_files"]),
        "tree_digests": digests,
        "sentinel_files": sentinels,
        "action_pins": action_pins,
    }

    _write_atomic(lockpath, json.dumps(lockfile_data, indent=2) + "\n")
    return lockfile_data


def _discover_sentinel_paths(
    root: Path, existing: dict[str, Any], containers: dict[str, str]
) -> list[str]:
    """Sentinels come from the manifest, not from the file being replaced."""
    paths: set[str] = set()
    for fields in export_manifest_entries(root):
        if len(fields) != MANIFEST_FIELD_COUNT:
            continue
        local_path = fields[F_LOCAL_PATH].strip()
        if containers.get(local_path) == "submodule":
            # A submodule's content is pinned by its gitlink, and its files are
            # not tracked in the superproject.
            for notice in fields[F_NOTICES].split(","):
                notice = notice.strip()
                if notice:
                    paths.add(notice)
            continue
        for required_file in fields[F_REQUIRED_FILES].split(","):
            required_file = required_file.strip()
            if required_file:
                paths.add(f"{local_path}/{required_file}")
        for notice in fields[F_NOTICES].split(","):
            notice = notice.strip()
            if notice:
                paths.add(notice)

    # Retain any additional sentinel the maintainers pinned by hand.
    paths.update(existing["sentinel_files"].keys())
    return sorted(paths)


def _build_sentinels(
    root: Path, root_resolved: Path, sentinel_paths: list[str], *, quiet: bool
) -> dict[str, dict[str, Any]]:
    if len(sentinel_paths) > MAX_SENTINELS:
        _fatal(f"sentinel count exceeds MAX_SENTINELS ({MAX_SENTINELS})")

    blobs = git_blob_hashes(sentinel_paths, root)
    sentinels: dict[str, dict[str, Any]] = {}
    for rel_path in sentinel_paths:
        err = validate_repo_relative_path(
            rel_path, allowed_roots=AUTHORITATIVE_ROOTS
        )
        if err:
            _fatal(f"sentinel path {rel_path!r}: {err}")
        filepath = root / rel_path
        file_err = assert_regular_file_no_escape(filepath, root_resolved)
        if file_err:
            _fatal(f"sentinel {rel_path}: {file_err}")
        blob = blobs.get(rel_path, "")
        if not blob:
            _fatal(f"sentinel {rel_path}: cannot compute git blob hash")
        hashed = _content_sha256(filepath, MAX_SENTINEL_BYTES)
        if hashed is None:
            _fatal(f"sentinel {rel_path}: cannot hash within the size limit")
        basename_upper = PurePosixPath(rel_path).name.upper()
        is_license = any(
            keyword in basename_upper
            for keyword in ("LICENSE", "COPYING", "NOTICE", "APACHE")
        )
        sentinels[rel_path] = {
            "sha256": hashed[0],
            "git_blob": blob,
            "size": hashed[1],
            "type": "license" if is_license else "source",
        }
        if not quiet:
            print(f"  OK: {rel_path}")
    return sentinels


def _discover_action_pins(root: Path, root_resolved: Path) -> dict[str, list[str]]:
    """Record the owner/repo → SHA identities the workflows currently use.

    Regeneration records identity; it does not approve it.  A repointed pin
    shows up as a lockfile diff for a reviewer, which is precisely the property
    a bare 40-hex syntax check cannot provide.
    """
    yaml = _load_yaml_module()
    pins: dict[str, set[str]] = {}
    for rel in _workflow_and_action_files(root):
        filepath = root / rel
        if assert_regular_file_no_escape(filepath, root_resolved):
            continue
        _bounded_size(filepath, MAX_WORKFLOW_BYTES, f"workflow {rel}")
        try:
            documents = list(yaml.safe_load_all(
                filepath.read_text(encoding="utf-8")))
        except (OSError, UnicodeDecodeError, yaml.YAMLError) as e:
            _fatal(f"cannot parse {rel} while recording action pins: {e}")
        for document in documents:
            for _, value in _iter_uses(document):
                if not isinstance(value, str):
                    continue
                match = ACTION_REF_RE.match(value.strip())
                if match:
                    pins.setdefault(match.group("repo"), set()).add(match.group("sha"))
    return {repo: sorted(shas) for repo, shas in sorted(pins.items())}


def _write_atomic(target: Path, content: str) -> None:
    handle, tmp_path = tempfile.mkstemp(
        dir=str(target.parent), prefix=".supply-chain.lock.", suffix=".tmp",
    )
    try:
        with os.fdopen(handle, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp_path, 0o644)
        os.replace(tmp_path, str(target))
        if hasattr(os, "O_DIRECTORY"):
            dir_fd = os.open(str(target.parent), os.O_DIRECTORY)
            try:
                os.fsync(dir_fd)
            finally:
                os.close(dir_fd)
    except BaseException:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass
        raise


# ── Verification driver ───────────────────────────────────────────────

def run_all_checks(root: Path, root_resolved: Path) -> tuple[CheckResult, dict[str, Any]]:
    lockfile = load_lockfile(root, root_resolved)
    result = CheckResult()

    check_container_model(lockfile, result)
    check_link_hygiene(root, result)
    tracked = git_tracked_thirdparty(root)
    assigned = check_tracked_inventory(root, lockfile, tracked, result)
    check_tree_digests(lockfile, assigned, result)
    check_sentinel_files(root, root_resolved, lockfile, result)
    check_action_pins(root, root_resolved, lockfile, result)
    modules = _parse_gitmodules(root, root_resolved)
    check_gitmodules_consistency(modules, lockfile, result)
    entries = export_manifest_entries(root)
    check_manifest_reconciliation(root, lockfile, entries, modules, result)
    return result, lockfile


def _emit_json(result: CheckResult, lockfile: dict[str, Any], extra: dict[str, Any]) -> None:
    payload = {
        "passed": result.passed,
        "violation_count": len(result.violations),
        "sentinel_count": len(lockfile.get("sentinel_files", {})),
        "submodule_count": len(lockfile.get("submodule_gitlinks", {})),
        "tree_digest_count": len(lockfile.get("tree_digests", {})),
        "action_pin_count": len(lockfile.get("action_pins", {})),
        "violations": [
            {
                "category": v.category,
                "path": v.path,
                "message": v.message,
                "severity": v.severity,
            }
            for v in result.violations
        ],
        **extra,
    }
    json.dump(payload, sys.stdout, indent=2)
    print()


def _emit_text(result: CheckResult, lockfile: dict[str, Any]) -> None:
    RED, GREEN, YELLOW, NC = "\033[0;31m", "\033[0;32m", "\033[1;33m", "\033[0m"
    errors = [v for v in result.violations if v.severity == "error"]
    warnings = [v for v in result.violations if v.severity == "warning"]

    if errors:
        print(f"\n{RED}=== Supply-Chain Policy Violations ==={NC}\n")
        for v in errors:
            print(f"  {RED}ERROR{NC} [{v.category}] {v.path}")
            print(f"        {v.message}")
        print()
    if warnings:
        print(f"\n{YELLOW}=== Warnings ==={NC}\n")
        for v in warnings:
            print(f"  {YELLOW}WARN{NC}  [{v.category}] {v.path}")
            print(f"        {v.message}")
        print()

    if result.passed:
        print(
            f"{GREEN}[SUPPLY-CHAIN]{NC} All checks passed "
            f"({len(lockfile['sentinel_files'])} sentinel files, "
            f"{len(lockfile['submodule_gitlinks'])} submodules, "
            f"{len(lockfile['tree_digests'])} tree digests, "
            f"{len(lockfile['action_pins'])} pinned actions)"
        )
    else:
        print(
            f"{RED}[SUPPLY-CHAIN]{NC} {len(errors)} error(s), "
            f"{len(warnings)} warning(s)"
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="SparkEngine supply-chain policy checker"
    )
    parser.add_argument(
        "--update", action="store_true",
        help="regenerate supply-chain.lock, then verify the result",
    )
    parser.add_argument("--json", action="store_true", help="output results as JSON")
    parser.add_argument(
        "--ci", action="store_true", help="CI mode (identical to default verify)",
    )
    args = parser.parse_args()
    root = project_root()
    try:
        root_resolved = root.resolve(strict=True)
    except OSError as e:
        _fatal(f"cannot resolve repository root: {e}")

    extra: dict[str, Any] = {}
    if args.update:
        written = update_lockfile(root, root_resolved, quiet=args.json)
        extra = {"updated": LOCKFILE_REL, "sentinels_written": len(written["sentinel_files"])}
        if not args.json:
            print(f"\nWrote {LOCKFILE_REL}")
            print("Re-verifying the regenerated lockfile...")

    # An update that is not verified is an update that reports success without
    # establishing anything. The exit code below is the verification's.
    result, lockfile = run_all_checks(root, root_resolved)

    if args.json:
        _emit_json(result, lockfile, extra)
    else:
        _emit_text(result, lockfile)
    return 0 if result.passed else 1


if __name__ == "__main__":
    sys.exit(main())
