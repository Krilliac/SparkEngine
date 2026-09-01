#!/usr/bin/env python3
"""Isolated, exact-commit documentation currentness and health evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Sequence

import docs_contract

REPO_ROOT = Path(__file__).resolve().parents[1]
CONTRACT_PATH = REPO_ROOT / "docs" / "generated-docs-manifest.json"
SHA_RE = __import__("re").compile(r"^[0-9a-f]{40}$")
REQUIRED_GENERATORS = (
    "wiki-sync",
    "api-docs",
    "symbol-indexes",
    "file-tree",
    "class-hierarchy",
    "architecture-flowchart",
    "codebase-statistics",
    "readme-badges",
    "ai-context",
)
COPY_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".mm",
    ".py", ".sh", ".md", ".json", ".txt", ".cmake", ".yml", ".yaml",
    ".hlsl", ".glsl", ".vert", ".frag", ".comp", ".as",
}
COPY_NAMES = {"CMakeLists.txt", "LICENSE", "NOTICE"}
MAX_COPY_FILES = 10000
MAX_COPY_BYTES = 384 * 1024 * 1024
MAX_COPY_FILE_BYTES = 16 * 1024 * 1024
MAX_OUTPUT_FILES = 5000
MAX_OUTPUT_BYTES = 256 * 1024 * 1024
MAX_JSON_BYTES = 8 * 1024 * 1024
OUTPUT_OVERRIDE_ENVIRONMENT = (
    "SPARK_DOC_API_OUTPUT_DIR",
    "SPARK_DOC_API_DIR",
    "SPARK_SYMBOL_INDEX_OUTPUT_DIR",
    "SPARK_FILE_TREE_OUTPUT",
    "SPARK_CLASS_HIERARCHY_OUTPUT",
    "SPARK_WIKI_DIR",
    "SPARK_DOC_HEALTH_OUTPUT",
    "SPARK_DOC_HEALTH_INNER",
    "SPARK_DOC_BASH",
    "GENERATED_DATE",
)


class CurrentnessError(RuntimeError):
    pass


def safe_relative(raw: str) -> PurePosixPath:
    path = PurePosixPath(raw)
    if path.is_absolute() or not path.parts or any(part in {"", ".", ".."} for part in path.parts):
        raise CurrentnessError(f"unsafe repository path: {raw!r}")
    if "\\" in raw or "\x00" in raw:
        raise CurrentnessError(f"non-canonical repository path: {raw!r}")
    return path


def atomic_json(path: Path, payload: dict) -> None:
    try:
        docs_contract.atomic_write_bytes(
            path, (json.dumps(payload, indent=2, sort_keys=True) + "\n").encode("utf-8")
        )
    except docs_contract.ContractError as exc:
        raise CurrentnessError(f"cannot safely write documentation evidence: {exc}") from exc


def load_contract() -> dict:
    try:
        contract = docs_contract.load_bounded_json(
            CONTRACT_PATH, label="generated-docs manifest", maximum=MAX_JSON_BYTES
        )
    except docs_contract.ContractError as exc:
        raise CurrentnessError(f"cannot load generated-docs manifest: {exc}") from exc
    if not isinstance(contract, dict):
        raise CurrentnessError("generated-docs manifest must be a JSON object")
    if contract.get("schemaVersion") != 1:
        raise CurrentnessError("generated-docs manifest schemaVersion must be 1")
    generators = contract.get("generators")
    if not isinstance(generators, list):
        raise CurrentnessError("generated-docs generators must be an array")
    ids = tuple(row.get("id") for row in generators if isinstance(row, dict))
    if ids != REQUIRED_GENERATORS:
        raise CurrentnessError(
            "generated-docs manifest must declare every required generator exactly once and in canonical order"
        )
    scripts: set[str] = set()
    outputs: set[str] = set()
    for row in generators:
        script = row.get("script")
        mode = row.get("mode")
        declared = row.get("outputs")
        if not isinstance(script, str) or script in scripts:
            raise CurrentnessError("generator scripts must be unique strings")
        scripts.add(script)
        if not isinstance(mode, str) or not mode:
            raise CurrentnessError(f"generator {row['id']} has invalid mode")
        if not isinstance(declared, list) or not declared:
            raise CurrentnessError(f"generator {row['id']} declares no outputs")
        for output in declared:
            if not isinstance(output, dict) or not isinstance(output.get("path"), str):
                raise CurrentnessError(f"generator {row['id']} has malformed output")
            canonical = safe_relative(output["path"]).as_posix()
            if canonical in outputs:
                raise CurrentnessError(f"generated output is owned twice: {canonical}")
            outputs.add(canonical)
            if not isinstance(output.get("tracked"), bool):
                raise CurrentnessError(f"generated output lacks tracked boolean: {canonical}")
            if output.get("tree", False) not in {True, False}:
                raise CurrentnessError(f"generated output tree flag is invalid: {canonical}")
    return contract


def git_output(arguments: list[str], *, text: bool = True) -> str | bytes:
    process = subprocess.run(
        ["git", "-C", str(REPO_ROOT), *arguments],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=text,
        timeout=60,
    )
    return process.stdout


def exact_identity(source_sha: str | None, committed_at: str | None) -> tuple[str, str]:
    dirty = str(git_output(["status", "--porcelain=v1", "--untracked-files=no"])).strip()
    if dirty:
        preview = dirty.splitlines()[0]
        raise CurrentnessError(
            f"exact-currentness evidence refuses a dirty tracked worktree ({preview})"
        )
    head = str(git_output(["rev-parse", "HEAD"])).strip()
    sha = source_sha or head
    if not SHA_RE.fullmatch(sha) or sha != head:
        raise CurrentnessError(f"source SHA must exactly match checked-out HEAD {head}")
    actual_timestamp = str(git_output(["show", "-s", "--format=%cI", head])).strip()
    timestamp = committed_at or actual_timestamp
    try:
        parsed = datetime.fromisoformat(timestamp.replace("Z", "+00:00"))
    except ValueError as exc:
        raise CurrentnessError("source committed-at timestamp must be RFC 3339") from exc
    if parsed.tzinfo is None:
        raise CurrentnessError("source committed-at timestamp must include an offset")
    if timestamp != actual_timestamp:
        raise CurrentnessError("source committed-at timestamp does not match checked-out HEAD")
    return sha, timestamp


def source_commit_utc_date(committed_at: str) -> str:
    try:
        parsed = datetime.fromisoformat(committed_at.replace("Z", "+00:00"))
    except (AttributeError, ValueError) as exc:
        raise CurrentnessError("source committed-at timestamp must be RFC 3339") from exc
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise CurrentnessError("source committed-at timestamp must include an offset")
    return parsed.astimezone(timezone.utc).date().isoformat()


def tracked_inventory() -> tuple[list[str], dict[str, str]]:
    raw = git_output(["ls-files", "-s", "-z", "--cached"], text=False)
    assert isinstance(raw, bytes)
    paths: list[str] = []
    modes: dict[str, str] = {}
    for entry in raw.split(b"\0"):
        if not entry:
            continue
        try:
            metadata, raw_path = entry.split(b"\t", 1)
            mode = metadata.split(b" ", 1)[0].decode("ascii")
            path = raw_path.decode("utf-8")
        except (ValueError, UnicodeDecodeError) as exc:
            raise CurrentnessError("Git tracked inventory is malformed") from exc
        canonical = safe_relative(path).as_posix()
        if mode == "120000":
            raise CurrentnessError(f"tracked symlink is not allowed in isolated documentation inputs: {canonical}")
        paths.append(canonical)
        modes[canonical] = mode
    if len(paths) != len(set(paths)):
        raise CurrentnessError("Git tracked inventory contains duplicate paths")
    return sorted(paths), modes


def should_copy(path: PurePosixPath) -> bool:
    return path.name in COPY_NAMES or path.suffix.lower() in COPY_SUFFIXES


def copy_snapshot(destination: Path, tracked: list[str], modes: dict[str, str]) -> Path:
    copied = 0
    total = 0
    for raw in tracked:
        rel = safe_relative(raw)
        target = destination.joinpath(*rel.parts)
        copied += 1
        if copied > MAX_COPY_FILES:
            raise CurrentnessError("isolated documentation input exceeds file-count bound")
        if modes.get(raw) == "160000":
            target.mkdir(parents=True, exist_ok=True)
            continue
        source = REPO_ROOT.joinpath(*rel.parts)
        try:
            docs_contract.assert_contained(source, REPO_ROOT, label="tracked documentation input")
            payload = docs_contract.read_regular_bytes(
                source,
                label=f"tracked documentation input {raw}",
                maximum=MAX_COPY_FILE_BYTES,
            )
        except docs_contract.ContractError as exc:
            raise CurrentnessError(str(exc)) from exc
        if should_copy(rel):
            total += len(payload)
        else:
            payload = b""
        if total > MAX_COPY_BYTES:
            raise CurrentnessError("isolated documentation input exceeds resource bounds")
        try:
            docs_contract.atomic_write_bytes(target, payload)
        except docs_contract.ContractError as exc:
            raise CurrentnessError(f"cannot safely write isolated input {raw}: {exc}") from exc
    tracked_manifest = destination / ".docs-tracked-files"
    try:
        docs_contract.atomic_write_bytes(
            tracked_manifest, b"\0".join(path.encode("utf-8") for path in tracked) + b"\0"
        )
    except docs_contract.ContractError as exc:
        raise CurrentnessError(f"cannot safely write tracked inventory: {exc}") from exc
    return tracked_manifest


def find_bash(*, allow_override: bool = True) -> str:
    explicit = os.environ.get("SPARK_DOC_BASH") if allow_override else None
    if explicit and Path(explicit).is_file():
        return explicit
    found = shutil.which("bash")
    if found:
        return found
    common = Path(r"C:\Program Files\Git\bin\bash.exe")
    if common.is_file():
        return str(common)
    raise CurrentnessError("bash is required for isolated documentation generation")


def run_bounded_process(
    command: list[str], *, cwd: Path, environment: dict[str, str], timeout: float, label: str
) -> subprocess.CompletedProcess[str]:
    """Run an isolated generator without allowing descendants to outlive its wall bound."""

    try:
        return docs_contract.run_bounded_process(
            command,
            cwd=cwd,
            environment=environment,
            timeout=timeout,
            label=label,
        )
    except docs_contract.ContractError as exc:
        raise CurrentnessError(str(exc)) from exc


def run_snapshot(root: Path, tracked_manifest: Path, sha: str, committed_at: str) -> None:
    env = os.environ.copy()
    for key in OUTPUT_OVERRIDE_ENVIRONMENT:
        env.pop(key, None)
    env.update({
        "SPARK_DOC_TRACKED_PATHS": str(tracked_manifest),
        "SPARKENGINE_DOC_SOURCE_SHA": sha,
        "SPARKENGINE_DOC_SOURCE_COMMITTED_AT": committed_at,
        "GENERATED_DATE": source_commit_utc_date(committed_at),
        "SPARK_DOC_HEALTH_OUTPUT": str(root / "docs" / ".health.json"),
        "SPARK_DOC_HEALTH_INNER": "1",
    })
    process = run_bounded_process(
        [find_bash(allow_override=False), str(root / "docs" / "update-all-docs.sh"), "update"],
        cwd=root,
        environment=env,
        timeout=600,
        label="isolated documentation generation",
    )
    output = process.stdout + process.stderr
    if output:
        print(output, end="")
    if process.returncode != 0:
        raise CurrentnessError(f"isolated documentation generation exited {process.returncode}")
    validations = (
        [
            sys.executable,
            str(root / "tools" / "docs_contract.py"),
            "validate",
            "--api-dir",
            str(root / "docs" / "api"),
            "--wiki-root",
            str(root / "wiki"),
        ],
        [
            sys.executable,
            str(root / "tools" / "site-data" / "validate_docs_links.py"),
            "--generated-root",
            str(root / "docs" / "api"),
            "--source-sha",
            sha,
        ],
    )
    for command in validations:
        validation = run_bounded_process(
            command,
            cwd=root,
            environment=env,
            timeout=180,
            label="isolated documentation validator",
        )
        output = validation.stdout + validation.stderr
        if output:
            print(output, end="")
        if validation.returncode != 0:
            raise CurrentnessError(f"isolated validator exited {validation.returncode}")


def parse_timestamp(value: object, field: str) -> datetime:
    if not isinstance(value, str):
        raise CurrentnessError(f"health {field} must be a string")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise CurrentnessError(f"health {field} must be RFC 3339") from exc
    if parsed.tzinfo is None:
        raise CurrentnessError(f"health {field} must have timezone")
    return parsed


def validate_health(path: Path, sha: str, committed_at: str) -> None:
    try:
        health = docs_contract.load_bounded_json(
            path, label="documentation health evidence", maximum=MAX_JSON_BYTES
        )
    except docs_contract.ContractError as exc:
        raise CurrentnessError(f"cannot read documentation health evidence: {exc}") from exc
    if not isinstance(health, dict):
        raise CurrentnessError("documentation health evidence must be a JSON object")
    if health.get("schemaVersion") != 1 or health.get("sourceCommit") != sha:
        raise CurrentnessError("documentation health is not schema-v1 exact-SHA evidence")
    if health.get("sourceCommittedAt") != committed_at:
        raise CurrentnessError("documentation health committed-at value is not exact")
    if health.get("overall") != "pass" or health.get("failures") != 0:
        raise CurrentnessError("documentation health contradicts successful generation")
    if health.get("exitCode") != 0:
        raise CurrentnessError("documentation health pass has a nonzero exit code")
    if health.get("successes") != len(REQUIRED_GENERATORS):
        raise CurrentnessError("documentation health success count is incomplete")
    started = parse_timestamp(health.get("startedAt"), "startedAt")
    completed = parse_timestamp(health.get("completedAt"), "completedAt")
    parse_timestamp(health.get("sourceCommittedAt"), "sourceCommittedAt")
    if completed < started:
        raise CurrentnessError("documentation health completion precedes start")
    results = health.get("results")
    if not isinstance(results, list):
        raise CurrentnessError("documentation health results must be an array")
    ids = [row.get("id") for row in results if isinstance(row, dict)]
    if tuple(ids) != REQUIRED_GENERATORS:
        raise CurrentnessError("documentation health does not contain every generator exactly once")
    if any(row.get("status") != "current" for row in results):
        raise CurrentnessError("documentation health reports a non-current generator")
    if any(not isinstance(row.get("message"), str) or not row["message"] for row in results):
        raise CurrentnessError("documentation health has an invalid generator message")
    if health.get("successes") + health.get("failures") != len(results):
        raise CurrentnessError("documentation health aggregate counts are inconsistent")


def file_digest(path: Path) -> str:
    try:
        payload = docs_contract.read_regular_bytes(
            path, label="documentation evidence file", maximum=MAX_COPY_FILE_BYTES
        )
    except docs_contract.ContractError as exc:
        raise CurrentnessError(str(exc)) from exc
    return hashlib.sha256(payload).hexdigest()


def tree_projection(root: Path) -> dict[str, tuple[int, str]]:
    result: dict[str, tuple[int, str]] = {}
    try:
        snapshot = docs_contract.generated_tree_snapshot(
            root,
            label="generated documentation tree",
            max_files=MAX_OUTPUT_FILES,
            max_bytes=MAX_OUTPUT_BYTES,
        )
    except docs_contract.ContractError as exc:
        raise CurrentnessError(str(exc)) from exc
    for relative, identity in snapshot.items():
        path = root.joinpath(*PurePosixPath(relative).parts)
        result[relative] = (identity.size, file_digest(path))
    return result


def compare_outputs(contract: dict, first: Path, second: Path, tracked: list[str]) -> None:
    tracked_set = set(tracked)
    declared_tracked: set[str] = set()
    for generator in contract["generators"]:
        for row in generator["outputs"]:
            relative = safe_relative(row["path"])
            canonical = relative.as_posix()
            left = first.joinpath(*relative.parts)
            right = second.joinpath(*relative.parts)
            if row.get("tree", False):
                if tree_projection(left) != tree_projection(right):
                    raise CurrentnessError(f"generated tree is nondeterministic: {canonical}")
                continue
            if not left.is_file() or not right.is_file():
                raise CurrentnessError(f"declared generated file is missing: {canonical}")
            if file_digest(left) != file_digest(right):
                raise CurrentnessError(f"generated file is nondeterministic: {canonical}")
            if row["tracked"]:
                declared_tracked.add(canonical)
                if canonical not in tracked_set:
                    raise CurrentnessError(f"manifest says output is tracked but Git does not: {canonical}")
                actual = REPO_ROOT.joinpath(*relative.parts)
                if not actual.is_file() or file_digest(actual) != file_digest(left):
                    raise CurrentnessError(f"tracked generated output is stale: {canonical}")

    changed: set[str] = set()
    for raw in tracked:
        rel = safe_relative(raw)
        if not should_copy(rel):
            continue
        generated = first.joinpath(*rel.parts)
        actual = REPO_ROOT.joinpath(*rel.parts)
        generated_file = generated.is_file() and not generated.is_symlink()
        actual_file = actual.is_file() and not actual.is_symlink()
        if generated_file != actual_file:
            changed.add(raw)
        elif generated_file and file_digest(generated) != file_digest(actual):
            changed.add(raw)
    undeclared = sorted(changed - declared_tracked)
    if undeclared:
        raise CurrentnessError(f"generator changed undeclared tracked output: {undeclared[0]}")


def working_tree_projection(paths: list[str], modes: dict[str, str]) -> dict[str, str]:
    result: dict[str, str] = {}
    for raw in paths:
        rel = safe_relative(raw)
        if modes.get(raw) == "160000":
            # A Gitlink is deliberately represented by an empty placeholder in
            # isolated snapshots.  It has no byte payload to project from the
            # worktree, and treating its checkout directory as a regular file
            # makes exact-currentness fail on every repository with submodules.
            continue
        full = REPO_ROOT.joinpath(*rel.parts)
        if not os.path.lexists(full):
            continue
        try:
            result[raw] = file_digest(full)
        except CurrentnessError as exc:
            raise CurrentnessError(f"working-tree projection rejected {raw}: {exc}") from exc
    return result


def check_currentness(source_sha: str | None, committed_at: str | None) -> None:
    contract = load_contract()
    sha, timestamp = exact_identity(source_sha, committed_at)
    tracked, modes = tracked_inventory()
    before = working_tree_projection(tracked, modes)
    with tempfile.TemporaryDirectory(prefix="spark-doc-check-") as parent:
        parent_path = Path(parent)
        first = parent_path / "first"
        second = parent_path / "second"
        first.mkdir()
        second.mkdir()
        first_manifest = copy_snapshot(first, tracked, modes)
        second_manifest = copy_snapshot(second, tracked, modes)
        run_snapshot(first, first_manifest, sha, timestamp)
        run_snapshot(second, second_manifest, sha, timestamp)
        validate_health(first / "docs" / ".health.json", sha, timestamp)
        validate_health(second / "docs" / ".health.json", sha, timestamp)
        compare_outputs(contract, first, second, tracked)
    after = working_tree_projection(tracked, modes)
    if before != after:
        raise CurrentnessError("documentation check mutated the tracked working tree")
    print(f"Documentation is deterministic, exact-current, and bound to {sha}.")


def write_health(args: argparse.Namespace) -> bool:
    contract = load_contract()
    sha = args.source_sha
    if not SHA_RE.fullmatch(sha):
        raise CurrentnessError("health source SHA must be exact")
    if args.mode not in {"update", "full", "quick"}:
        raise CurrentnessError("health mode is invalid")
    parse_timestamp(args.source_committed_at, "sourceCommittedAt")
    started = parse_timestamp(args.started_at, "startedAt")
    if not 0 <= args.exit_code <= 255:
        raise CurrentnessError("health exit code is outside 0..255")
    rows: dict[str, dict[str, str]] = {}
    try:
        payload = docs_contract.read_regular_bytes(
            args.results,
            label="documentation health results",
            maximum=MAX_JSON_BYTES,
        )
        lines = payload.decode("utf-8").splitlines()
    except (docs_contract.ContractError, UnicodeDecodeError):
        lines = []
    for line in lines:
        fields = line.split("\t", 2)
        if len(fields) != 3:
            raise CurrentnessError("health result row is malformed")
        generator_id, status, message = fields
        if generator_id not in REQUIRED_GENERATORS or generator_id in rows:
            raise CurrentnessError("health result generator is unknown or duplicated")
        if status not in {"current", "failed", "missing", "stale", "skipped"}:
            raise CurrentnessError("health result status is invalid")
        rows[generator_id] = {"id": generator_id, "status": status, "message": message[:500]}
    results = [
        rows.get(generator_id, {
            "id": generator_id,
            "status": "missing",
            "message": "generator did not reach a terminal result",
        })
        for generator_id in REQUIRED_GENERATORS
    ]
    failures = sum(row["status"] != "current" for row in results)
    if args.exit_code != 0 and failures == 0:
        results[-1] = {
            "id": results[-1]["id"],
            "status": "failed",
            "message": f"master documentation update exited {args.exit_code}",
        }
        failures = 1
    successes = len(results) - failures
    effective_exit_code = args.exit_code if args.exit_code != 0 else (1 if failures else 0)
    overall = "pass" if failures == 0 and effective_exit_code == 0 else "fail"
    completed = datetime.now(timezone.utc)
    if completed < started:
        raise CurrentnessError("health completion precedes start")
    payload = {
        "schemaVersion": 1,
        "mode": args.mode,
        "sourceCommit": sha,
        "sourceCommittedAt": args.source_committed_at,
        "startedAt": args.started_at,
        "completedAt": completed.isoformat().replace("+00:00", "Z"),
        "overall": overall,
        "successes": successes,
        "failures": failures,
        "exitCode": effective_exit_code,
        "results": results,
    }
    atomic_json(args.output, payload)
    return overall == "pass"


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    check = subparsers.add_parser("check")
    check.add_argument("--source-sha")
    check.add_argument("--source-committed-at")
    health = subparsers.add_parser("write-health")
    health.add_argument("--mode", required=True)
    health.add_argument("--results", type=Path, required=True)
    health.add_argument("--output", type=Path, required=True)
    health.add_argument("--source-sha", required=True)
    health.add_argument("--source-committed-at", required=True)
    health.add_argument("--started-at", required=True)
    health.add_argument("--exit-code", type=int, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "check":
            check_currentness(args.source_sha, args.source_committed_at)
        else:
            if not write_health(args):
                return 1
    except (CurrentnessError, OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
