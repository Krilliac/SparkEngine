#!/usr/bin/env python3
"""SparkEngine supply-chain policy checker.

Fail-closed verification of dependency identity, content integrity,
license coverage, action pinning, and manifest consistency.

Exit codes:
    0  All checks passed
    1  One or more policy violations detected
    2  Checker itself failed (internal error, missing lockfile, bad schema)

Usage:
    python tools/check-supply-chain.py              # verify
    python tools/check-supply-chain.py --update     # regenerate lockfile (atomic)
    python tools/check-supply-chain.py --json       # machine-readable output
    python tools/check-supply-chain.py --ci         # CI mode (identical to default)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath
from typing import Any

LOCKFILE_REL = "ThirdParty/supply-chain.lock"
GITMODULES_REL = ".gitmodules"
WORKFLOWS_DIR = ".github/workflows"

AUTHORITATIVE_ROOTS = frozenset({"ThirdParty"})

MIN_LICENSE_SIZE = 200
LICENSE_KEYWORDS_TERMS = re.compile(
    r"Permission is (hereby )?granted|Redistribution and use|"
    r"public domain|TERMS AND CONDITIONS FOR USE|"
    r"THE SOFTWARE IS PROVIDED",
    re.IGNORECASE,
)
LICENSE_KEYWORDS_COPYRIGHT = re.compile(r"[Cc]opyright")

SHA_HEX_RE = re.compile(r"^[0-9a-f]{64}$")
SHA40_HEX_RE = re.compile(r"^[0-9a-f]{40}$")
SAFE_PATH_RE = re.compile(r"^[A-Za-z0-9_\-./]+$")

ACTION_USES_RE = re.compile(r"uses:\s*(\S+)")
ACTION_SHA_PIN_RE = re.compile(
    r"uses:\s*[^@\s]+@([0-9a-fA-F]{40})\s*(#.*)?$"
)


def _fatal(msg: str) -> None:
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

    @property
    def passed(self) -> bool:
        return not any(v.severity == "error" for v in self.violations)

    def error(self, category: str, path: str, message: str) -> None:
        self.violations.append(Violation(category, path, message, "error"))

    def warn(self, category: str, path: str, message: str) -> None:
        self.violations.append(Violation(category, path, message, "warning"))


# ── Project root ──────────────────────────────────────────────────────

def project_root() -> Path:
    """Locate project root via git, not file-relative heuristics."""
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            capture_output=True, text=True, timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired):
        _fatal("cannot run git to locate project root")
    if out.returncode != 0:
        _fatal("git rev-parse --show-toplevel failed — not in a git repo?")
    root = Path(out.stdout.strip()).resolve()
    if not root.is_dir():
        _fatal(f"git root is not a directory: {root}")
    return root


# ── Git helpers (fail-closed) ─────────────────────────────────────────

def git_cmd(args: list[str], cwd: Path) -> str:
    """Run a git command. On failure, raise RuntimeError (caller must handle)."""
    try:
        result = subprocess.run(
            ["git", *args],
            capture_output=True, text=True, cwd=str(cwd), timeout=60,
        )
    except (OSError, subprocess.TimeoutExpired) as e:
        raise RuntimeError(f"git {' '.join(args)}: {e}")
    if result.returncode != 0:
        raise RuntimeError(
            f"git {' '.join(args)} exited {result.returncode}: "
            f"{result.stderr.strip()}"
        )
    return result.stdout.strip()


def git_blob_hash(path: str, cwd: Path) -> str:
    """Return the git blob hash of a tracked file via hash-object on its content.

    This produces the same hash on all platforms regardless of autocrlf,
    because git hash-object hashes the content as git would store it
    (with LF normalization per .gitattributes).
    """
    try:
        return git_cmd(["hash-object", "--", path], cwd)
    except RuntimeError:
        return ""


def git_ls_tree_thirdparty(cwd: Path) -> str:
    """Get ls-tree output for ThirdParty/. Fail-closed on error."""
    return git_cmd(["ls-tree", "-r", "HEAD", "--", "ThirdParty/"], cwd)


# ── Path safety ───────────────────────────────────────────────────────

def validate_repo_relative_path(
    rel_path: str, *, allowed_roots: frozenset[str] | None = None
) -> str | None:
    """Validate a repository-relative path. Returns error message or None."""
    if not rel_path:
        return "empty path"
    if os.path.isabs(rel_path):
        return "absolute path rejected"
    if "\\" in rel_path:
        return "backslash in path rejected"
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
        return f"path root {posix.parts[0]!r} not in allowed roots {allowed_roots}"
    return None


def assert_regular_file_no_escape(
    filepath: Path, root: Path
) -> str | None:
    """Verify filepath is a regular file inside root, not a symlink/junction/etc.
    Returns error message or None.
    """
    if not filepath.exists():
        return "file does not exist"
    if filepath.is_symlink():
        return "symlink rejected"
    if sys.platform == "win32":
        try:
            st = filepath.stat()
            if st.st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                return "reparse point (junction/symlink) rejected"
        except (OSError, AttributeError):
            pass
    if not filepath.is_file():
        return "not a regular file"
    try:
        resolved = filepath.resolve(strict=True)
        root_resolved = root.resolve(strict=True)
        resolved.relative_to(root_resolved)
    except (ValueError, OSError):
        return "path escapes repository root after resolution"
    return None


# ── Lockfile loading and schema validation ────────────────────────────

def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    """JSON object_pairs_hook that rejects duplicate keys."""
    seen: dict[str, Any] = {}
    for key, value in pairs:
        if key in seen:
            _fatal(f"duplicate JSON key in lockfile: {key!r}")
        seen[key] = value
    return seen


def load_lockfile(root: Path) -> dict[str, Any]:
    lockpath = root / LOCKFILE_REL
    if not lockpath.is_file():
        _fatal(f"lockfile not found: {LOCKFILE_REL}")
    try:
        text = lockpath.read_text(encoding="utf-8")
    except OSError as e:
        _fatal(f"cannot read lockfile: {e}")
    try:
        data = json.loads(text, object_pairs_hook=_reject_duplicate_keys)
    except json.JSONDecodeError as e:
        _fatal(f"cannot parse lockfile: {e}")

    if not isinstance(data.get("version"), int) or data["version"] != 1:
        _fatal(f"unsupported lockfile version: {data.get('version')!r}")

    _validate_lockfile_schema(data)
    return data


def _validate_lockfile_schema(data: dict[str, Any]) -> None:
    """Validate all lockfile fields have correct types and safe paths."""
    for key in ("submodule_gitlinks", "sentinel_files"):
        if key not in data or not isinstance(data[key], dict):
            _fatal(f"lockfile missing or invalid field: {key}")

    for key in ("managed_vendored_dirs", "project_owned_dirs"):
        if key not in data or not isinstance(data[key], list):
            _fatal(f"lockfile missing or invalid field: {key}")

    for path, sha in data["submodule_gitlinks"].items():
        err = validate_repo_relative_path(path, allowed_roots=AUTHORITATIVE_ROOTS)
        if err:
            _fatal(f"submodule_gitlinks path {path!r}: {err}")
        if not isinstance(sha, str) or not SHA40_HEX_RE.match(sha):
            _fatal(f"submodule_gitlinks[{path!r}]: invalid SHA: {sha!r}")

    for path, entry in data["sentinel_files"].items():
        err = validate_repo_relative_path(path, allowed_roots=AUTHORITATIVE_ROOTS)
        if err:
            _fatal(f"sentinel_files path {path!r}: {err}")
        if not isinstance(entry, dict):
            _fatal(f"sentinel_files[{path!r}]: entry must be an object")
        sha256 = entry.get("sha256")
        if not isinstance(sha256, str) or not SHA_HEX_RE.match(sha256):
            _fatal(f"sentinel_files[{path!r}]: invalid sha256: {sha256!r}")
        git_blob = entry.get("git_blob")
        if git_blob is not None:
            if not isinstance(git_blob, str) or not SHA40_HEX_RE.match(git_blob):
                _fatal(f"sentinel_files[{path!r}]: invalid git_blob: {git_blob!r}")
        size = entry.get("size")
        if not isinstance(size, int) or size < 0:
            _fatal(f"sentinel_files[{path!r}]: invalid size: {size!r}")
        stype = entry.get("type")
        if stype not in ("license", "source"):
            _fatal(f"sentinel_files[{path!r}]: invalid type: {stype!r}")

    for lst_key in ("managed_vendored_dirs", "project_owned_dirs"):
        for path in data[lst_key]:
            if not isinstance(path, str):
                _fatal(f"{lst_key}: non-string entry: {path!r}")
            err = validate_repo_relative_path(
                path, allowed_roots=AUTHORITATIVE_ROOTS
            )
            if err:
                _fatal(f"{lst_key} path {path!r}: {err}")


# ── Check: submodule gitlinks ─────────────────────────────────────────

def check_submodule_gitlinks(
    root: Path, lockfile: dict[str, Any], result: CheckResult
) -> None:
    expected = lockfile.get("submodule_gitlinks", {})
    if not expected:
        result.error("submodule", LOCKFILE_REL, "no submodule gitlinks in lockfile")
        return

    try:
        actual_output = git_ls_tree_thirdparty(root)
    except RuntimeError as e:
        _fatal(f"git ls-tree failed (cannot verify submodules): {e}")

    actual: dict[str, str] = {}
    for line in actual_output.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[0] == "160000":
            actual[parts[3]] = parts[2]

    for path, expected_sha in expected.items():
        if path not in actual:
            result.error(
                "submodule", path,
                "expected submodule gitlink not found in repository tree",
            )
        elif actual[path] != expected_sha:
            result.error(
                "submodule", path,
                f"gitlink drift: lockfile={expected_sha}, "
                f"repository={actual[path]}",
            )

    for path in actual:
        if path not in expected:
            result.error(
                "submodule", path,
                "unmanaged submodule — add to supply-chain.lock",
            )


# ── Check: sentinel files ─────────────────────────────────────────────

def check_sentinel_files(
    root: Path, lockfile: dict[str, Any], result: CheckResult
) -> None:
    sentinels = lockfile.get("sentinel_files", {})
    if not sentinels:
        result.error("sentinel", LOCKFILE_REL, "no sentinel files in lockfile")
        return

    for rel_path, expected in sentinels.items():
        filepath = root / rel_path

        file_err = assert_regular_file_no_escape(filepath, root)
        if file_err:
            result.error("sentinel", rel_path, f"sentinel file: {file_err}")
            continue

        actual_sha = _content_sha256(filepath)
        expected_sha = expected["sha256"]
        if actual_sha != expected_sha:
            result.error(
                "integrity", rel_path,
                f"content hash mismatch: lockfile={expected_sha[:16]}..., "
                f"actual={actual_sha[:16]}...",
            )

        expected_size = expected["size"]
        actual_size = filepath.stat().st_size
        if actual_size != expected_size:
            result.error(
                "integrity", rel_path,
                f"size mismatch: lockfile={expected_size}, actual={actual_size}",
            )

        git_blob_expected = expected.get("git_blob")
        if git_blob_expected:
            actual_blob = git_blob_hash(rel_path, root)
            if not actual_blob:
                result.error(
                    "integrity", rel_path,
                    "git hash-object failed — cannot verify blob identity",
                )
            elif actual_blob != git_blob_expected:
                result.error(
                    "integrity", rel_path,
                    f"git blob drift: lockfile={git_blob_expected[:16]}..., "
                    f"actual={actual_blob[:16]}...",
                )

        if expected["type"] == "license":
            _check_license_content(filepath, rel_path, result)


def _content_sha256(filepath: Path) -> str:
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


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
    try:
        text = filepath.read_text(encoding="utf-8", errors="replace")
    except OSError:
        result.error("license", rel_path, "cannot read license file")
        return
    if not LICENSE_KEYWORDS_COPYRIGHT.search(text):
        result.error("license", rel_path, "license lacks copyright statement")
    if not LICENSE_KEYWORDS_TERMS.search(text):
        result.error("license", rel_path, "license lacks operative terms")


# ── Check: unmanaged directories ──────────────────────────────────────

def check_unmanaged_dirs(
    root: Path, lockfile: dict[str, Any], result: CheckResult
) -> None:
    managed_vendored = set(lockfile.get("managed_vendored_dirs", []))
    project_owned = set(lockfile.get("project_owned_dirs", []))
    submodule_paths = set(lockfile.get("submodule_gitlinks", {}).keys())

    known_paths = managed_vendored | project_owned | submodule_paths

    allowed_files = {
        LOCKFILE_REL,
        "ThirdParty/README.md",
        "ThirdParty/POLICY.md",
        "ThirdParty/dependencies.lock",
    }

    thirdparty = root / "ThirdParty"
    if not thirdparty.is_dir():
        result.error("inventory", "ThirdParty/", "ThirdParty directory missing")
        return

    for entry in sorted(thirdparty.iterdir()):
        if entry.name.startswith("."):
            continue
        if entry.is_symlink():
            result.error(
                "inventory",
                f"ThirdParty/{entry.name}",
                "symlink in ThirdParty rejected",
            )
            continue

        rel = f"ThirdParty/{entry.name}"

        if entry.is_file():
            if rel not in allowed_files:
                result.error(
                    "inventory", rel,
                    "unmanaged file in ThirdParty root",
                )
        elif entry.is_dir():
            if rel in known_paths:
                continue
            has_managed_child = False
            for child in sorted(entry.iterdir()):
                if child.is_symlink():
                    result.error(
                        "inventory",
                        f"ThirdParty/{entry.name}/{child.name}",
                        "symlink in ThirdParty rejected",
                    )
                    continue
                if child.is_dir():
                    child_rel = f"ThirdParty/{entry.name}/{child.name}"
                    if child_rel in known_paths:
                        has_managed_child = True
                    else:
                        result.error(
                            "inventory", child_rel,
                            "unmanaged directory — add to supply-chain.lock",
                        )
            if not has_managed_child and not any(
                p.startswith(rel + "/") for p in known_paths
            ):
                result.error(
                    "inventory", rel,
                    "unmanaged directory — add to supply-chain.lock",
                )


# ── Check: sentinel coverage of managed dirs ─────────────────────────

def check_sentinel_coverage(
    lockfile: dict[str, Any], result: CheckResult
) -> None:
    managed = set(lockfile.get("managed_vendored_dirs", []))
    sentinels = set(lockfile.get("sentinel_files", {}).keys())
    for d in sorted(managed):
        if not any(s.startswith(d + "/") for s in sentinels):
            result.error(
                "coverage", d,
                "managed vendored directory has no sentinel files — "
                "content could be replaced undetected",
            )


# ── Check: action pinning ─────────────────────────────────────────────

_YAML_COMMENT_RE = re.compile(r"\s+#.*$")


def _strip_yaml_comment(line: str) -> str:
    """Remove trailing YAML comment (# ...) from a line.

    Handles the common case of `uses: owner/repo@ref # vX.Y` by stripping
    everything from the first ` #` to end-of-line.  This prevents a crafted
    comment containing a fake SHA-pinned reference from fooling the pin check.
    """
    return _YAML_COMMENT_RE.sub("", line)


def check_action_pins(root: Path, result: CheckResult) -> None:
    workflows = root / WORKFLOWS_DIR
    if not workflows.is_dir():
        result.warn("actions", WORKFLOWS_DIR, "no workflows directory found")
        return

    for yml in sorted(workflows.iterdir()):
        if yml.suffix not in (".yml", ".yaml"):
            continue
        if yml.is_symlink():
            result.error("actions", str(yml.relative_to(root)), "symlink rejected")
            continue
        rel = str(yml.relative_to(root)).replace("\\", "/")
        try:
            lines = yml.read_text(encoding="utf-8").splitlines()
        except OSError:
            result.error("actions", rel, "cannot read workflow file")
            continue
        for i, line in enumerate(lines, 1):
            stripped = line.strip()
            if stripped.startswith("#"):
                continue
            match = ACTION_USES_RE.search(stripped)
            if not match:
                continue
            uses_value = match.group(1)
            if uses_value.startswith("./"):
                continue
            code_before_comment = _strip_yaml_comment(stripped)
            if not ACTION_SHA_PIN_RE.search(code_before_comment):
                result.error(
                    "actions", f"{rel}:{i}",
                    f"unpinned action: {uses_value} — must use 40-char commit SHA",
                )


# ── Check: .gitmodules consistency ────────────────────────────────────

def check_gitmodules_consistency(
    root: Path, lockfile: dict[str, Any], result: CheckResult
) -> None:
    gitmodules = root / GITMODULES_REL
    if not gitmodules.is_file():
        result.error("gitmodules", GITMODULES_REL, ".gitmodules not found")
        return

    try:
        submodule_paths_output = git_cmd(
            ["config", "--file", GITMODULES_REL,
             "--get-regexp", r"^submodule\..*\.path$"],
            root,
        )
    except RuntimeError as e:
        _fatal(f"git config --file .gitmodules failed: {e}")

    gitmodule_paths: set[str] = set()
    for line in submodule_paths_output.splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2:
            gitmodule_paths.add(parts[1])

    lockfile_submodules = set(lockfile.get("submodule_gitlinks", {}).keys())

    for path in gitmodule_paths:
        if path not in lockfile_submodules:
            result.error(
                "gitmodules", path,
                "submodule in .gitmodules but not in supply-chain.lock",
            )

    for path in lockfile_submodules:
        if path not in gitmodule_paths:
            result.error(
                "gitmodules", path,
                "submodule in supply-chain.lock but not in .gitmodules",
            )


# ── Check: dependencies.lock manifest reconciliation ─────────────────

def check_manifest_reconciliation(
    root: Path, lockfile: dict[str, Any], result: CheckResult
) -> None:
    manifest_path = root / "ThirdParty" / "dependencies.lock"
    if not manifest_path.is_file():
        result.error("manifest", "ThirdParty/dependencies.lock",
                      "dependencies.lock not found")
        return

    try:
        text = manifest_path.read_text(encoding="utf-8")
    except OSError:
        result.error("manifest", "ThirdParty/dependencies.lock",
                      "cannot read manifest")
        return

    manifest_deps: dict[str, str] = {}
    manifest_notices: set[str] = set()

    in_entries = False
    for line in text.splitlines():
        stripped = line.strip()
        if "SPARK_THIRDPARTY_AUDIT_ENTRIES" in stripped:
            in_entries = True
            continue
        if in_entries and stripped == ")":
            break
        if not in_entries:
            continue
        match = re.search(r'"([^"]*)"', stripped)
        if not match:
            continue
        entry = match.group(1)
        fields = entry.split("|")
        if len(fields) != 10:
            result.error(
                "manifest",
                "ThirdParty/dependencies.lock",
                f"malformed manifest entry (expected 10 fields, got {len(fields)}): "
                f"{entry[:60]}...",
            )
            continue
        name = fields[0]
        local_path = fields[4]
        notice_csv = fields[9]

        if not local_path.startswith("ThirdParty/"):
            continue
        manifest_deps[name] = local_path

        for notice in notice_csv.split(","):
            notice = notice.strip()
            if notice and notice.startswith("ThirdParty/"):
                manifest_notices.add(notice)

    sentinels = set(lockfile.get("sentinel_files", {}).keys())
    license_sentinels = {
        p for p, v in lockfile.get("sentinel_files", {}).items()
        if v.get("type") == "license"
    }

    for notice in manifest_notices:
        if notice not in sentinels:
            result.error(
                "manifest", notice,
                "license file in dependencies.lock not tracked in supply-chain.lock",
            )

    managed_all = (
        set(lockfile.get("managed_vendored_dirs", []))
        | set(lockfile.get("project_owned_dirs", []))
        | set(lockfile.get("submodule_gitlinks", {}).keys())
    )
    for name, local_path in manifest_deps.items():
        if not any(
            local_path == mp or local_path.startswith(mp + "/")
            for mp in managed_all
        ):
            result.error(
                "manifest", local_path,
                f"dependency {name!r} path not in supply-chain.lock managed set",
            )


# ── Update lockfile (atomic, deterministic) ───────────────────────────

def update_lockfile(root: Path) -> None:
    lockpath = root / LOCKFILE_REL

    try:
        tree_output = git_ls_tree_thirdparty(root)
    except RuntimeError as e:
        _fatal(f"git ls-tree failed: {e}")

    gitlinks: dict[str, str] = {}
    for line in tree_output.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[0] == "160000":
            gitlinks[parts[3]] = parts[2]

    if lockpath.is_file():
        try:
            text = lockpath.read_text(encoding="utf-8")
            existing = json.loads(text, object_pairs_hook=_reject_duplicate_keys)
        except (json.JSONDecodeError, OSError) as e:
            _fatal(f"cannot read existing lockfile for sentinel list: {e}")
    else:
        existing = {
            "version": 1,
            "managed_vendored_dirs": [],
            "project_owned_dirs": ["ThirdParty/Licenses"],
        }

    sentinel_paths = sorted(existing.get("sentinel_files", {}).keys())
    sentinels: dict[str, dict[str, Any]] = {}

    for rel_path in sentinel_paths:
        err = validate_repo_relative_path(
            rel_path, allowed_roots=AUTHORITATIVE_ROOTS
        )
        if err:
            _fatal(f"sentinel path {rel_path!r} in existing lockfile: {err}")

        filepath = root / rel_path
        file_err = assert_regular_file_no_escape(filepath, root)
        if file_err:
            _fatal(f"sentinel {rel_path}: {file_err}")

        blob = git_blob_hash(rel_path, root)
        if not blob:
            _fatal(f"sentinel {rel_path}: cannot compute git blob hash")
        sha = _content_sha256(filepath)
        sz = filepath.stat().st_size

        basename_upper = os.path.basename(rel_path).upper()
        is_license = any(
            kw in basename_upper
            for kw in ("LICENSE", "COPYING", "NOTICE", "APACHE")
        )

        sentinels[rel_path] = {
            "sha256": sha,
            "git_blob": blob,
            "size": sz,
            "type": "license" if is_license else "source",
        }
        print(f"  OK: {rel_path}")

    lockfile_data = {
        "version": 1,
        "description": existing.get(
            "description",
            "Authoritative supply-chain content lockfile. "
            "Verified by tools/check-supply-chain.py on every CI run. "
            "Update with: python tools/check-supply-chain.py --update",
        ),
        "submodule_gitlinks": dict(sorted(gitlinks.items())),
        "managed_vendored_dirs": sorted(
            existing.get("managed_vendored_dirs", [])
        ),
        "project_owned_dirs": sorted(
            existing.get("project_owned_dirs", [])
        ),
        "sentinel_files": sentinels,
    }

    output = json.dumps(lockfile_data, indent=2, sort_keys=False) + "\n"

    fd, tmp_path = tempfile.mkstemp(
        dir=str(lockpath.parent),
        prefix=".supply-chain.lock.",
        suffix=".tmp",
    )
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(output)
        os.replace(tmp_path, str(lockpath))
    except BaseException:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass
        raise

    print(f"\nWrote {LOCKFILE_REL}")


# ── Main ──────────────────────────────────────────────────────────────

def main() -> int:
    parser = argparse.ArgumentParser(
        description="SparkEngine supply-chain policy checker"
    )
    parser.add_argument(
        "--update", action="store_true",
        help="regenerate supply-chain.lock from current repository state",
    )
    parser.add_argument(
        "--json", action="store_true",
        help="output results as JSON",
    )
    parser.add_argument(
        "--ci", action="store_true",
        help="CI mode (identical to default verify)",
    )
    args = parser.parse_args()
    root = project_root()

    if args.update:
        update_lockfile(root)
        return 0

    lockfile = load_lockfile(root)
    result = CheckResult()

    check_submodule_gitlinks(root, lockfile, result)
    check_sentinel_files(root, lockfile, result)
    check_sentinel_coverage(lockfile, result)
    check_unmanaged_dirs(root, lockfile, result)
    check_action_pins(root, result)
    check_gitmodules_consistency(root, lockfile, result)
    check_manifest_reconciliation(root, lockfile, result)

    if args.json:
        output = {
            "passed": result.passed,
            "violation_count": len(result.violations),
            "violations": [
                {
                    "category": v.category,
                    "path": v.path,
                    "message": v.message,
                    "severity": v.severity,
                }
                for v in result.violations
            ],
        }
        json.dump(output, sys.stdout, indent=2)
        print()
        return 0 if result.passed else 1

    RED = "\033[0;31m"
    GREEN = "\033[0;32m"
    YELLOW = "\033[1;33m"
    NC = "\033[0m"

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

    sentinel_count = len(lockfile.get("sentinel_files", {}))
    submodule_count = len(lockfile.get("submodule_gitlinks", {}))

    if result.passed:
        print(
            f"{GREEN}[SUPPLY-CHAIN]{NC} All checks passed "
            f"({sentinel_count} sentinel files, {submodule_count} submodules)"
        )
        return 0
    else:
        print(
            f"{RED}[SUPPLY-CHAIN]{NC} {len(errors)} error(s), "
            f"{len(warnings)} warning(s)"
        )
        return 1


if __name__ == "__main__":
    sys.exit(main())
