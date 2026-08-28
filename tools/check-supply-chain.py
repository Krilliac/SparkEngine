#!/usr/bin/env python3
"""SparkEngine supply-chain policy checker.

Fail-closed verification of dependency identity, content integrity,
license coverage, action pinning, and manifest consistency.

Exit codes:
    0  All checks passed
    1  One or more policy violations detected
    2  Checker itself failed (missing lockfile, bad JSON, etc.)

Usage:
    python tools/check-supply-chain.py              # verify
    python tools/check-supply-chain.py --update-lockfile  # regenerate lockfile
    python tools/check-supply-chain.py --json        # machine-readable output
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

LOCKFILE_REL = "ThirdParty/supply-chain.lock"
MANIFEST_REL = "ThirdParty/dependencies.lock"
GITMODULES_REL = ".gitmodules"
WORKFLOWS_DIR = ".github/workflows"

MIN_LICENSE_SIZE = 200
LICENSE_KEYWORDS_TERMS = re.compile(
    r"Permission is (hereby )?granted|Redistribution and use|"
    r"public domain|TERMS AND CONDITIONS FOR USE|"
    r"THE SOFTWARE IS PROVIDED",
    re.IGNORECASE,
)
LICENSE_KEYWORDS_COPYRIGHT = re.compile(r"[Cc]opyright")

ACTION_PIN_RE = re.compile(
    r"uses:\s*([^@\s]+)@([0-9a-fA-F]{40})\s*(#.*)?$"
)
ACTION_USES_RE = re.compile(r"uses:\s*(\S+)")


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


def project_root() -> Path:
    here = Path(__file__).resolve().parent
    root = here.parent
    if not (root / ".git").exists() and not (root / ".git").is_file():
        print("ERROR: cannot locate project root", file=sys.stderr)
        sys.exit(2)
    return root


def git_cmd(args: list[str], cwd: Path) -> str:
    result = subprocess.run(
        ["git", *args],
        capture_output=True,
        text=True,
        cwd=str(cwd),
    )
    if result.returncode != 0:
        return ""
    return result.stdout.strip()


def git_blob_sha(path: str, cwd: Path) -> str | None:
    output = git_cmd(["ls-tree", "HEAD", "--", path], cwd)
    if not output:
        return None
    parts = output.split()
    if len(parts) >= 3 and parts[0] != "160000":
        return parts[2]
    return None


def content_sha256(filepath: Path) -> str:
    h = hashlib.sha256()
    with open(filepath, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def load_lockfile(root: Path) -> dict[str, Any]:
    lockpath = root / LOCKFILE_REL
    if not lockpath.is_file():
        print(f"FATAL: lockfile not found: {LOCKFILE_REL}", file=sys.stderr)
        sys.exit(2)
    try:
        with open(lockpath) as f:
            data = json.load(f)
    except (json.JSONDecodeError, OSError) as e:
        print(f"FATAL: cannot parse lockfile: {e}", file=sys.stderr)
        sys.exit(2)
    if data.get("version") != 1:
        print("FATAL: unsupported lockfile version", file=sys.stderr)
        sys.exit(2)
    return data


def check_submodule_gitlinks(
    root: Path, lockfile: dict[str, Any], result: CheckResult
) -> None:
    expected = lockfile.get("submodule_gitlinks", {})
    if not expected:
        result.error("submodule", LOCKFILE_REL, "no submodule gitlinks in lockfile")
        return

    actual_output = git_cmd(
        ["ls-tree", "-r", "HEAD", "ThirdParty/"], root
    )
    actual: dict[str, str] = {}
    for line in actual_output.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[0] == "160000":
            actual[parts[3]] = parts[2]

    for path, expected_sha in expected.items():
        if path not in actual:
            result.error(
                "submodule",
                path,
                f"expected submodule gitlink not found in repository tree",
            )
        elif actual[path] != expected_sha:
            result.error(
                "submodule",
                path,
                f"gitlink drift: lockfile={expected_sha}, "
                f"repository={actual[path]}",
            )

    for path in actual:
        if path not in expected:
            result.error(
                "submodule",
                path,
                "unmanaged submodule in ThirdParty — add to supply-chain.lock",
            )


def check_sentinel_files(
    root: Path, lockfile: dict[str, Any], result: CheckResult
) -> None:
    sentinels = lockfile.get("sentinel_files", {})
    if not sentinels:
        result.error("sentinel", LOCKFILE_REL, "no sentinel files in lockfile")
        return

    for rel_path, expected in sentinels.items():
        filepath = root / rel_path
        if not filepath.is_file():
            result.error(
                "sentinel",
                rel_path,
                "sentinel file missing from working tree",
            )
            continue

        actual_sha = content_sha256(filepath)
        expected_sha = expected.get("sha256", "")
        if actual_sha != expected_sha:
            result.error(
                "integrity",
                rel_path,
                f"content hash mismatch: lockfile={expected_sha[:16]}..., "
                f"actual={actual_sha[:16]}...",
            )

        expected_size = expected.get("size")
        if expected_size is not None:
            actual_size = filepath.stat().st_size
            if actual_size != expected_size:
                result.error(
                    "integrity",
                    rel_path,
                    f"size mismatch: lockfile={expected_size}, actual={actual_size}",
                )

        if expected.get("type") == "license":
            _check_license_content(filepath, rel_path, result)


def _check_license_content(
    filepath: Path, rel_path: str, result: CheckResult
) -> None:
    size = filepath.stat().st_size
    if size < MIN_LICENSE_SIZE:
        result.error(
            "license",
            rel_path,
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


def check_unmanaged_dirs(
    root: Path, lockfile: dict[str, Any], result: CheckResult
) -> None:
    managed_vendored = set(lockfile.get("managed_vendored_dirs", []))
    project_owned = set(lockfile.get("project_owned_dirs", []))
    submodule_paths = set(lockfile.get("submodule_gitlinks", {}).keys())

    known_paths = managed_vendored | project_owned | submodule_paths
    known_basenames = {os.path.basename(p) for p in known_paths}

    thirdparty = root / "ThirdParty"
    if not thirdparty.is_dir():
        result.error("inventory", "ThirdParty/", "ThirdParty directory missing")
        return

    for entry in sorted(thirdparty.iterdir()):
        if entry.name.startswith("."):
            continue
        if entry.is_file():
            rel = f"ThirdParty/{entry.name}"
            if rel in (LOCKFILE_REL, MANIFEST_REL, "ThirdParty/README.md",
                       "ThirdParty/POLICY.md"):
                continue
            pass
        elif entry.is_dir():
            rel = f"ThirdParty/{entry.name}"
            if rel in known_paths:
                continue
            has_managed_child = False
            for child in sorted(entry.iterdir()):
                if child.is_dir():
                    child_rel = f"ThirdParty/{entry.name}/{child.name}"
                    if child_rel in known_paths:
                        has_managed_child = True
                    else:
                        result.error(
                            "inventory",
                            child_rel,
                            "unmanaged directory in ThirdParty — add to supply-chain.lock or remove",
                        )
            if not has_managed_child and not any(
                p.startswith(rel + "/") for p in known_paths
            ):
                result.error(
                    "inventory",
                    rel,
                    "unmanaged directory in ThirdParty — add to supply-chain.lock or remove",
                )


def check_action_pins(
    root: Path, result: CheckResult
) -> None:
    workflows = root / WORKFLOWS_DIR
    if not workflows.is_dir():
        result.warn("actions", WORKFLOWS_DIR, "no workflows directory found")
        return

    for yml in sorted(workflows.glob("*.yml")):
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
            if not ACTION_PIN_RE.search(stripped):
                result.error(
                    "actions",
                    f"{rel}:{i}",
                    f"unpinned action: {uses_value} — must use 40-char commit SHA",
                )


def check_gitmodules_consistency(
    root: Path, lockfile: dict[str, Any], result: CheckResult
) -> None:
    gitmodules = root / GITMODULES_REL
    if not gitmodules.is_file():
        result.error("gitmodules", GITMODULES_REL, ".gitmodules not found")
        return

    submodule_paths_output = git_cmd(
        ["config", "--file", GITMODULES_REL,
         "--get-regexp", "^submodule\\..*\\.path$"],
        root,
    )
    gitmodule_paths: set[str] = set()
    for line in submodule_paths_output.splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2:
            gitmodule_paths.add(parts[1])

    lockfile_submodules = set(lockfile.get("submodule_gitlinks", {}).keys())

    for path in gitmodule_paths:
        if path not in lockfile_submodules:
            result.error(
                "gitmodules",
                path,
                "submodule in .gitmodules but not in supply-chain.lock",
            )

    for path in lockfile_submodules:
        if path not in gitmodule_paths:
            result.error(
                "gitmodules",
                path,
                "submodule in supply-chain.lock but not in .gitmodules",
            )


def check_manifest_consistency(
    root: Path, lockfile: dict[str, Any], result: CheckResult
) -> None:
    manifest = root / MANIFEST_REL
    if not manifest.is_file():
        result.error("manifest", MANIFEST_REL, "dependencies.lock not found")
        return

    try:
        text = manifest.read_text(encoding="utf-8")
    except OSError:
        result.error("manifest", MANIFEST_REL, "cannot read manifest")
        return

    entries = re.findall(r'"([^"]+)"', text)
    manifest_paths: set[str] = set()
    manifest_notices: set[str] = set()
    for entry in entries:
        fields = entry.split("|")
        if len(fields) != 10:
            continue
        # Skip comment/header lines that happen to have 10 pipe-separated fields
        local_path = fields[4]
        if not local_path.startswith("ThirdParty/"):
            continue
        manifest_paths.add(local_path)
        notice_csv = fields[9]
        for notice in notice_csv.split(","):
            notice = notice.strip()
            if notice and notice.startswith("ThirdParty/"):
                manifest_notices.add(notice)

    sentinels = set(lockfile.get("sentinel_files", {}).keys())
    license_sentinels = {
        p
        for p, v in lockfile.get("sentinel_files", {}).items()
        if v.get("type") == "license"
    }

    for notice in manifest_notices:
        if notice not in sentinels:
            result.error(
                "manifest",
                notice,
                "license file referenced in dependencies.lock but missing from supply-chain.lock sentinels",
            )


def update_lockfile(root: Path) -> None:
    lockpath = root / LOCKFILE_REL

    submodule_output = git_cmd(
        ["ls-tree", "-r", "HEAD", "ThirdParty/"], root
    )
    gitlinks: dict[str, str] = {}
    for line in submodule_output.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[0] == "160000":
            gitlinks[parts[3]] = parts[2]

    if lockpath.is_file():
        with open(lockpath) as f:
            existing = json.load(f)
    else:
        existing = {
            "version": 1,
            "managed_vendored_dirs": [],
            "project_owned_dirs": ["ThirdParty/Licenses"],
        }

    sentinel_paths = list(existing.get("sentinel_files", {}).keys())
    sentinels: dict[str, dict[str, Any]] = {}
    for rel_path in sorted(sentinel_paths):
        filepath = root / rel_path
        if not filepath.is_file():
            print(f"  SKIP (missing): {rel_path}")
            continue
        blob = git_blob_sha(rel_path, root)
        sha = content_sha256(filepath)
        sz = filepath.stat().st_size
        is_license = any(
            kw in os.path.basename(rel_path).upper()
            for kw in ("LICENSE", "COPYING", "NOTICE", "APACHE")
        )
        sentinels[rel_path] = {
            "sha256": sha,
            "git_blob": blob,
            "size": sz,
            "type": "license" if is_license else "source",
        }
        print(f"  OK: {rel_path}")

    from datetime import date

    lockfile = {
        "version": 1,
        "generated": str(date.today()),
        "description": existing.get("description", ""),
        "submodule_gitlinks": dict(sorted(gitlinks.items())),
        "managed_vendored_dirs": existing.get("managed_vendored_dirs", []),
        "project_owned_dirs": existing.get("project_owned_dirs", []),
        "sentinel_files": sentinels,
    }
    with open(lockpath, "w", newline="\n") as f:
        json.dump(lockfile, f, indent=2)
        f.write("\n")
    print(f"\nWrote {lockpath.relative_to(root)}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="SparkEngine supply-chain policy checker"
    )
    parser.add_argument(
        "--update-lockfile",
        action="store_true",
        help="regenerate supply-chain.lock from current repository state",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="output results as JSON",
    )
    args = parser.parse_args()
    root = project_root()

    if args.update_lockfile:
        update_lockfile(root)
        return 0

    lockfile = load_lockfile(root)
    result = CheckResult()

    check_submodule_gitlinks(root, lockfile, result)
    check_sentinel_files(root, lockfile, result)
    check_unmanaged_dirs(root, lockfile, result)
    check_action_pins(root, result)
    check_gitmodules_consistency(root, lockfile, result)
    check_manifest_consistency(root, lockfile, result)

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
    BLUE = "\033[0;34m"
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

    if result.passed:
        print(
            f"{GREEN}[SUPPLY-CHAIN]{NC} All checks passed "
            f"({len(lockfile.get('sentinel_files', {}))} sentinel files, "
            f"{len(lockfile.get('submodule_gitlinks', {}))} submodules)"
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
