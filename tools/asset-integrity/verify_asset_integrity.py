#!/usr/bin/env python3
"""Asset integrity manifest generator and verifier for SparkEngine.

Generates deterministic SHA-256 integrity manifests and verifies them
against the filesystem with strict path safety, traversal protection,
symlink/junction rejection, case-collision detection, and completeness
checking.

Usage:
    python3 tools/asset-integrity/verify_asset_integrity.py generate Assets
    python3 tools/asset-integrity/verify_asset_integrity.py verify Assets/assets.integrity.json
    python3 tools/asset-integrity/verify_asset_integrity.py check-all
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import sys
from pathlib import Path, PurePosixPath

MANIFEST_SCHEMA_VERSION = 1
HASH_ALGORITHM = "sha256"
MANIFEST_FILENAME = "assets.integrity.json"

REPO_ROOT = Path(__file__).resolve().parent.parent.parent

KNOWN_MANIFESTS: list[dict[str, str]] = [
    {"manifest": "Assets/assets.integrity.json", "root": "Assets"},
]

IGNORED_FILENAMES = frozenset({
    MANIFEST_FILENAME,
    ".gitkeep",
    ".gitignore",
    "Thumbs.db",
    ".DS_Store",
    "desktop.ini",
})


class IntegrityError:
    """A single integrity check failure."""

    __slots__ = ("path", "category", "message")

    def __init__(self, path: str, category: str, message: str) -> None:
        self.path = path
        self.category = category
        self.message = message

    def __repr__(self) -> str:
        return f"IntegrityError({self.category}: {self.path}: {self.message})"

    def __str__(self) -> str:
        return f"[{self.category}] {self.path}: {self.message}"


def _is_safe_path(relative: str) -> str | None:
    """Return an error message if the relative path is unsafe, else None."""
    if not relative:
        return "empty path"
    if os.path.isabs(relative):
        return "absolute path"
    for ch in relative:
        cp = ord(ch)
        if cp < 0x20 or cp == 0x7F:
            return f"control character U+{cp:04X}"
    parts = PurePosixPath(relative).parts
    if ".." in parts:
        return "path traversal (..)"
    if any(p in (".", "") for p in parts):
        return "degenerate path segment"
    if relative != relative.strip():
        return "leading or trailing whitespace"
    if "//" in relative:
        return "consecutive separators"
    if "\\" in relative:
        return "backslash separator"
    reserved_names = frozenset({
        "CON", "PRN", "AUX", "NUL",
        *(f"COM{i}" for i in range(1, 10)),
        *(f"LPT{i}" for i in range(1, 10)),
    })
    for part in parts:
        stem = part.split(".")[0].upper()
        if stem in reserved_names:
            return f"reserved Windows device name: {part}"
    return None


def _is_symlink_or_junction(path: Path) -> bool:
    """Detect symlinks, junctions, and reparse points.

    Uses lstat to avoid following the reparse point itself.
    """
    try:
        linfo = path.lstat()
    except OSError:
        return False
    if stat.S_ISLNK(linfo.st_mode):
        return True
    if sys.platform == "win32":
        try:
            FILE_ATTRIBUTE_REPARSE_POINT = 0x400
            attrs = linfo.st_file_attributes  # type: ignore[attr-defined]
            if attrs & FILE_ATTRIBUTE_REPARSE_POINT:
                return True
        except AttributeError:
            pass
    return False


def _check_ancestry_for_reparse(path: Path, root: Path) -> Path | None:
    """Walk from path up to root checking for symlinks/junctions.

    Returns the offending path or None if clean.
    """
    current = path
    root_resolved = root.resolve()
    while True:
        if _is_symlink_or_junction(current):
            return current
        if current.resolve() == root_resolved or current == current.parent:
            break
        current = current.parent
    return None


def _file_sha256(path: Path) -> str:
    """Compute SHA-256 hex digest of a file."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 16)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def _normalize_relative(root: Path, full: Path) -> str:
    """Produce a forward-slash-separated relative path."""
    rel = full.relative_to(root)
    return str(PurePosixPath(rel))


def scan_directory(root: Path) -> tuple[list[dict], list[IntegrityError]]:
    """Walk a directory tree and produce integrity entries + errors.

    Returns (entries, errors) where entries is a sorted list of
    {path, sha256, size} dicts and errors is a list of problems found.
    """
    root = root.resolve()
    if not root.is_dir():
        return [], [IntegrityError(str(root), "missing", "root directory does not exist")]

    reparse = _check_ancestry_for_reparse(root, root.parent)
    if reparse is not None:
        return [], [IntegrityError(str(reparse), "symlink",
                                   "root directory ancestry contains a symlink or junction")]

    entries: list[dict] = []
    errors: list[IntegrityError] = []
    case_map: dict[str, str] = {}

    for dirpath_str, dirnames, filenames in os.walk(root, followlinks=False):
        dirpath = Path(dirpath_str)

        rel_dir = _normalize_relative(root, dirpath) if dirpath != root else ""

        if _is_symlink_or_junction(dirpath) and dirpath != root:
            errors.append(IntegrityError(
                rel_dir, "symlink",
                "directory is a symlink or junction"))
            dirnames.clear()
            continue

        dirs_to_remove = []
        for d in dirnames:
            child = dirpath / d
            if _is_symlink_or_junction(child):
                child_rel = _normalize_relative(root, child)
                errors.append(IntegrityError(
                    child_rel, "symlink",
                    "directory is a symlink or junction"))
                dirs_to_remove.append(d)
        for d in dirs_to_remove:
            dirnames.remove(d)

        dirnames.sort()

        for filename in sorted(filenames):
            if filename in IGNORED_FILENAMES:
                continue

            full = dirpath / filename

            if _is_symlink_or_junction(full):
                rel = _normalize_relative(root, full)
                errors.append(IntegrityError(
                    rel, "symlink", "file is a symlink"))
                continue

            rel = _normalize_relative(root, full)

            path_err = _is_safe_path(rel)
            if path_err is not None:
                errors.append(IntegrityError(rel, "path-safety", path_err))
                continue

            real = full.resolve()
            if not str(real).startswith(str(root)):
                errors.append(IntegrityError(
                    rel, "traversal",
                    f"resolved path escapes root: {real}"))
                continue

            lower_key = rel.lower()
            if lower_key in case_map:
                existing = case_map[lower_key]
                if existing != rel:
                    errors.append(IntegrityError(
                        rel, "case-collision",
                        f"collides with '{existing}' on case-insensitive filesystems"))
            else:
                case_map[lower_key] = rel

            try:
                size = full.stat().st_size
                sha = _file_sha256(full)
            except OSError as e:
                errors.append(IntegrityError(rel, "io-error", str(e)))
                continue

            entries.append({
                "path": rel,
                "sha256": sha,
                "size": size,
            })

    entries.sort(key=lambda e: e["path"])
    return entries, errors


def generate_manifest(root: Path) -> tuple[dict, list[IntegrityError]]:
    """Generate a complete integrity manifest for a directory tree."""
    entries, errors = scan_directory(root)
    manifest = {
        "version": MANIFEST_SCHEMA_VERSION,
        "algorithm": HASH_ALGORITHM,
        "root": root.name,
        "fileCount": len(entries),
        "entries": entries,
    }
    return manifest, errors


def write_manifest(manifest: dict, output: Path) -> None:
    """Write manifest as deterministic JSON."""
    text = json.dumps(manifest, indent=2, sort_keys=False, ensure_ascii=True)
    if not text.endswith("\n"):
        text += "\n"
    output.write_text(text, encoding="utf-8", newline="\n")


def load_manifest(path: Path) -> dict:
    """Load and validate a manifest file's structure."""
    text = path.read_text(encoding="utf-8")
    data = json.loads(text)
    if not isinstance(data, dict):
        raise ValueError("manifest root is not an object")
    version = data.get("version")
    if version != MANIFEST_SCHEMA_VERSION:
        raise ValueError(f"unsupported manifest version: {version}")
    algo = data.get("algorithm")
    if algo != HASH_ALGORITHM:
        raise ValueError(f"unsupported algorithm: {algo}")
    entries = data.get("entries")
    if not isinstance(entries, list):
        raise ValueError("entries is not an array")
    return data


def verify_manifest(manifest_path: Path) -> list[IntegrityError]:
    """Verify a manifest against the filesystem.

    Checks:
    - Every declared file exists with correct hash and size
    - No undeclared files exist in the root
    - Path safety for every entry
    - Case-collision detection
    - Symlink/junction/traversal rejection
    """
    errors: list[IntegrityError] = []

    try:
        manifest = load_manifest(manifest_path)
    except (json.JSONDecodeError, ValueError, OSError) as e:
        return [IntegrityError(str(manifest_path), "manifest-load", str(e))]

    root_name = manifest.get("root", "")
    root = manifest_path.parent / root_name
    if root_name == manifest_path.parent.name:
        root = manifest_path.parent

    if not root.is_dir():
        return [IntegrityError(str(root), "missing", "manifest root directory does not exist")]

    root = root.resolve()

    reparse = _check_ancestry_for_reparse(root, root.parent)
    if reparse is not None:
        errors.append(IntegrityError(str(reparse), "symlink",
                                     "root directory ancestry contains a symlink or junction"))

    declared_entries = manifest.get("entries", [])
    declared_paths: set[str] = set()
    case_map: dict[str, str] = {}

    for entry in declared_entries:
        rel = entry.get("path", "")

        if rel in declared_paths:
            errors.append(IntegrityError(rel, "duplicate", "duplicate path in manifest"))
            continue
        declared_paths.add(rel)

        path_err = _is_safe_path(rel)
        if path_err is not None:
            errors.append(IntegrityError(rel, "path-safety", path_err))
            continue

        lower_key = rel.lower()
        if lower_key in case_map:
            existing = case_map[lower_key]
            if existing != rel:
                errors.append(IntegrityError(
                    rel, "case-collision",
                    f"collides with '{existing}' on case-insensitive filesystems"))
        else:
            case_map[lower_key] = rel

        full = root / rel.replace("/", os.sep)

        if not full.exists():
            errors.append(IntegrityError(rel, "missing", "file not found on disk"))
            continue

        if _is_symlink_or_junction(full):
            errors.append(IntegrityError(rel, "symlink", "file is a symlink or junction"))
            continue

        real = full.resolve()
        if not str(real).startswith(str(root)):
            errors.append(IntegrityError(rel, "traversal",
                                         f"resolved path escapes root: {real}"))
            continue

        try:
            actual_size = full.stat().st_size
            actual_sha = _file_sha256(full)
        except OSError as e:
            errors.append(IntegrityError(rel, "io-error", str(e)))
            continue

        expected_sha = entry.get("sha256", "")
        expected_size = entry.get("size", -1)

        if actual_sha != expected_sha:
            errors.append(IntegrityError(
                rel, "hash-mismatch",
                f"expected {expected_sha}, actual {actual_sha}"))

        if actual_size != expected_size:
            errors.append(IntegrityError(
                rel, "size-mismatch",
                f"expected {expected_size}, actual {actual_size}"))

    disk_entries, scan_errors = scan_directory(root)
    errors.extend(scan_errors)

    disk_paths = {e["path"] for e in disk_entries}
    undeclared = disk_paths - declared_paths
    for path in sorted(undeclared):
        errors.append(IntegrityError(path, "undeclared", "file on disk is not in manifest"))

    file_count = manifest.get("fileCount")
    if file_count is not None and file_count != len(declared_entries):
        errors.append(IntegrityError(
            str(manifest_path), "count-mismatch",
            f"fileCount={file_count} but entries has {len(declared_entries)} items"))

    return errors


def verify_template_manifests(repo_root: Path) -> list[IntegrityError]:
    """Cross-validate template asset manifests against the lock file."""
    errors: list[IntegrityError] = []
    lock_path = repo_root / "Templates" / "assets.lock.json"

    if not lock_path.is_file():
        errors.append(IntegrityError(str(lock_path), "missing", "template lock file not found"))
        return errors

    try:
        lock_data = json.loads(lock_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as e:
        errors.append(IntegrityError(str(lock_path), "manifest-load", str(e)))
        return errors

    lock_assets = lock_data.get("assets", {})
    templates_root = repo_root / "Templates"

    manifest_files = sorted(templates_root.glob("*/Assets/manifest.json"))
    case_map: dict[str, str] = {}

    for mf in manifest_files:
        try:
            mdata = json.loads(mf.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError) as e:
            errors.append(IntegrityError(str(mf), "manifest-load", str(e)))
            continue

        template_dir = mf.parent.parent
        template_name = template_dir.name
        assets_dir = mf.parent

        for asset in mdata.get("assets", []):
            rel_path = asset.get("path", "")
            declared_hash = asset.get("sha256", "")

            path_err = _is_safe_path(rel_path)
            if path_err is not None:
                errors.append(IntegrityError(
                    f"{template_name}/Assets/{rel_path}", "path-safety", path_err))
                continue

            lock_key = f"{template_name}/Assets/{rel_path}"

            lower_key = lock_key.lower()
            if lower_key in case_map:
                existing = case_map[lower_key]
                if existing != lock_key:
                    errors.append(IntegrityError(
                        lock_key, "case-collision",
                        f"collides with '{existing}'"))
            else:
                case_map[lower_key] = lock_key

            lock_hash = lock_assets.get(lock_key)
            if lock_hash is None:
                errors.append(IntegrityError(
                    lock_key, "lock-missing",
                    "manifest asset is not present in lock file"))
            elif lock_hash != declared_hash:
                errors.append(IntegrityError(
                    lock_key, "lock-mismatch",
                    f"manifest hash {declared_hash} != lock hash {lock_hash}"))

            disk_path = assets_dir / rel_path.replace("/", os.sep)
            if not disk_path.exists():
                errors.append(IntegrityError(lock_key, "missing", "file not found on disk"))
                continue

            if _is_symlink_or_junction(disk_path):
                errors.append(IntegrityError(lock_key, "symlink", "file is a symlink"))
                continue

            real = disk_path.resolve()
            templates_resolved = templates_root.resolve()
            if not str(real).startswith(str(templates_resolved)):
                errors.append(IntegrityError(
                    lock_key, "traversal",
                    f"resolved path escapes Templates root: {real}"))
                continue

            try:
                actual_hash = _file_sha256(disk_path)
            except OSError as e:
                errors.append(IntegrityError(lock_key, "io-error", str(e)))
                continue

            if actual_hash != declared_hash:
                errors.append(IntegrityError(
                    lock_key, "hash-mismatch",
                    f"expected {declared_hash}, actual {actual_hash}"))

    return errors


def cmd_generate(args: argparse.Namespace) -> int:
    """Generate an integrity manifest for a directory."""
    root = Path(args.root).resolve()
    if not root.is_dir():
        print(f"Error: '{args.root}' is not a directory", file=sys.stderr)
        return 1

    print(f"Scanning {root}...")
    manifest, errors = generate_manifest(root)

    if errors:
        print(f"\n{len(errors)} error(s) found during scan:", file=sys.stderr)
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        if any(e.category in ("symlink", "traversal", "path-safety") for e in errors):
            print("\nRefusing to generate manifest with safety errors.", file=sys.stderr)
            return 1

    output = Path(args.output) if args.output else root / MANIFEST_FILENAME
    write_manifest(manifest, output)
    print(f"Generated {output} with {manifest['fileCount']} entries")
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    """Verify an integrity manifest."""
    manifest_path = Path(args.manifest).resolve()
    if not manifest_path.is_file():
        print(f"Error: '{args.manifest}' is not a file", file=sys.stderr)
        return 1

    print(f"Verifying {manifest_path}...")
    errors = verify_manifest(manifest_path)

    if errors:
        print(f"\n{len(errors)} error(s):", file=sys.stderr)
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        return 1

    manifest = load_manifest(manifest_path)
    print(f"OK: {manifest.get('fileCount', '?')} entries verified")
    return 0


def cmd_check_all(args: argparse.Namespace) -> int:
    """Verify all known manifests and template cross-validation."""
    repo_root = Path(args.repo_root).resolve() if args.repo_root else REPO_ROOT
    overall_errors: list[IntegrityError] = []
    checks_run = 0

    for known in KNOWN_MANIFESTS:
        manifest_path = repo_root / known["manifest"]
        if manifest_path.is_file():
            print(f"Verifying {known['manifest']}...")
            errors = verify_manifest(manifest_path)
            overall_errors.extend(errors)
            checks_run += 1
            if errors:
                print(f"  FAIL: {len(errors)} error(s)")
            else:
                m = load_manifest(manifest_path)
                print(f"  OK: {m.get('fileCount', '?')} entries")
        else:
            overall_errors.append(IntegrityError(
                known["manifest"], "missing", "expected manifest not found"))
            print(f"  MISSING: {known['manifest']}")

    print(f"\nCross-validating template manifests...")
    template_errors = verify_template_manifests(repo_root)
    overall_errors.extend(template_errors)
    checks_run += 1
    if template_errors:
        print(f"  FAIL: {len(template_errors)} error(s)")
    else:
        print(f"  OK")

    print(f"\n{'='*60}")
    if overall_errors:
        print(f"FAILED: {len(overall_errors)} error(s) across {checks_run} checks")
        for e in overall_errors:
            print(f"  {e}", file=sys.stderr)
        return 1
    else:
        print(f"PASSED: {checks_run} checks, all clean")
        return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="SparkEngine asset integrity manifest tool")
    subparsers = parser.add_subparsers(dest="command", required=True)

    gen = subparsers.add_parser("generate",
                                help="Generate an integrity manifest")
    gen.add_argument("root", help="Root directory to scan")
    gen.add_argument("-o", "--output", help="Output path (default: <root>/assets.integrity.json)")

    ver = subparsers.add_parser("verify",
                                help="Verify a manifest against disk")
    ver.add_argument("manifest", help="Path to manifest JSON")

    chk = subparsers.add_parser("check-all",
                                help="Verify all known manifests")
    chk.add_argument("--repo-root",
                     help="Repository root (default: auto-detect)")

    args = parser.parse_args()

    if args.command == "generate":
        return cmd_generate(args)
    elif args.command == "verify":
        return cmd_verify(args)
    elif args.command == "check-all":
        return cmd_check_all(args)
    return 1


if __name__ == "__main__":
    sys.exit(main())
