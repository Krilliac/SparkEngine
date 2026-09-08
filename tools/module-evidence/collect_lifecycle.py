#!/usr/bin/env python3
"""Produce runtime lifecycle evidence by really running a module.

This is the *producer* half of the lifecycle contract.  It launches the
headless engine against a built game module, captures the ModuleManager phase
trace, and serialises what actually executed.

Phase trace contract
--------------------
The engine must emit one line per entered phase, on stdout or into the
captured log, in the form:

    [module-lifecycle] <ModuleName> <PhaseName>

`validate_manifest.py` requires `CreateModule`, `OnLoad`, `OnUpdate`,
`OnUnload` and `DestroyModule` to appear for every module a profile ships.

If the engine binary is missing, the run fails, or the trace contains none of
the required markers, this program writes **no evidence file** and exits
non-zero.  A partial or empty document is never emitted: absent evidence must
present as absent, not as a module that ran and did nothing.

Usage:
    python tools/module-evidence/collect_lifecycle.py \
        --engine build/bin/SparkEngine --module SparkGameFPS \
        --out build/module-evidence/module-lifecycle.json --frames 120
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import lifecycle as lifecycle_mod  # noqa: E402
import paths as paths_mod  # noqa: E402
from provenance import resolve_head_sha  # noqa: E402
from schema import expected_library_names  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]

_ALLOWED_ENGINE_NAMES = re.compile(
    r"^(?:SparkEngine|SparkConsole)(?:\.exe)?$", re.IGNORECASE
)
_SCRIPT_EXTENSIONS = frozenset({".cmd", ".bat", ".sh", ".ps1", ".py", ".pl", ".rb"})
_SCRIPT_SIGNATURES = (b"#!", b"@echo", b"@ECHO", b"@rem", b"@REM")
_MIN_ENGINE_SIZE = 4096


def hash_engine_binary(engine: Path) -> str:
    h = hashlib.sha256()
    with open(engine, "rb") as f:
        while True:
            chunk = f.read(1 << 16)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def validate_engine_binary(engine: Path) -> str | None:
    """Return an error string if `engine` is not a plausible SparkEngine binary."""
    if not engine.is_file():
        return (
            f"engine executable not found at {engine} — lifecycle evidence "
            f"requires a real build; there is nothing to run"
        )
    if paths_mod._is_reparse_point(engine):
        return (
            f"engine path {engine} is a symlink, junction, or reparse point — "
            f"lifecycle evidence must be produced by a real engine binary, not "
            f"an indirection that can point anywhere"
        )
    if engine.suffix.lower() in _SCRIPT_EXTENSIONS:
        return (
            f"engine path {engine.name!r} is a script ({engine.suffix}) — "
            f"lifecycle evidence requires a compiled engine binary, not a "
            f"script that can print arbitrary lifecycle markers"
        )
    if not _ALLOWED_ENGINE_NAMES.match(engine.name):
        return (
            f"engine filename {engine.name!r} does not match the expected "
            f"pattern (SparkEngine, SparkEngine.exe, SparkConsole, "
            f"SparkConsole.exe) — lifecycle evidence must come from the real "
            f"engine executable"
        )
    try:
        size = engine.stat().st_size
    except OSError as exc:
        return f"cannot stat engine binary {engine}: {exc}"
    if size < _MIN_ENGINE_SIZE:
        return (
            f"engine binary {engine} is only {size} bytes — a compiled engine "
            f"executable is orders of magnitude larger; this looks like a stub "
            f"or script masquerading as a binary"
        )
    try:
        with open(engine, "rb") as f:
            header = f.read(64)
    except OSError as exc:
        return f"cannot read engine binary header: {exc}"
    for sig in _SCRIPT_SIGNATURES:
        if header.lstrip().startswith(sig):
            return (
                f"engine binary {engine} starts with script signature "
                f"{sig!r} — lifecycle evidence requires a compiled executable"
            )
    return None


TRACE_RE = re.compile(
    r"^\[module-lifecycle\]\s+(?P<module>[A-Za-z][A-Za-z0-9]*)\s+"
    r"(?P<phase>[A-Za-z][A-Za-z0-9]*)\s*$"
)


def parse_trace(text: str, module: str) -> dict[str, int]:
    """Count phase entries for `module` in a captured engine log."""
    counts: dict[str, int] = {}
    for line in text.splitlines():
        match = TRACE_RE.match(line.strip())
        if not match or match.group("module") != module:
            continue
        phase = match.group("phase")
        if phase not in lifecycle_mod.OBSERVABLE_PHASES:
            continue
        counts[phase] = counts.get(phase, 0) + 1
    return counts


def run_engine(engine: Path, module: str, frames: int, timeout: int,
               log_path: Path) -> tuple[str, str | None]:
    """Run the headless engine and return (captured output, error)."""
    err = validate_engine_binary(engine)
    if err:
        return "", err
    cmd = [
        str(engine), "-headless", "-game", module,
        "-test-frames", str(frames),
    ]
    print(f"[collect_lifecycle] {' '.join(cmd)}", flush=True)
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout,
            check=False, cwd=str(REPO_ROOT),
        )
    except subprocess.TimeoutExpired:
        return "", f"engine run exceeded {timeout}s without completing"
    except OSError as exc:
        return "", f"cannot launch engine: {exc}"

    captured = (proc.stdout or "") + (proc.stderr or "")
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(captured, encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        return captured, (
            f"engine exited {proc.returncode}; a failed run is not evidence "
            f"of a working lifecycle (captured log: {log_path})"
        )
    return captured, None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--module", action="append", required=True, dest="modules")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--frames", type=int, default=120)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--commit-sha", default=None)
    args = parser.parse_args()

    sha = args.commit_sha
    if sha is None:
        sha, err = resolve_head_sha(REPO_ROOT)
        if sha is None:
            print(f"FATAL: {err}", file=sys.stderr)
            return 1

    engine_err = validate_engine_binary(args.engine)
    if engine_err:
        print(f"FATAL: {engine_err}", file=sys.stderr)
        return 1
    engine_digest = hash_engine_binary(args.engine)
    engine_path_str = str(args.engine)

    records = []
    failures: list[str] = []
    for module in args.modules:
        source_dir = f"GameModules/{module}/Source"
        tree_sha, err = lifecycle_mod.source_tree_sha(REPO_ROOT, sha, source_dir)
        if tree_sha is None:
            failures.append(f"{module}: {err}")
            continue

        log_path = args.out.parent / f"module-lifecycle-{module}.log"
        captured, err = run_engine(args.engine, module, args.frames,
                                   args.timeout, log_path)
        if err:
            failures.append(f"{module}: {err}")
            continue

        phases = parse_trace(captured, module)
        missing = [p for p in lifecycle_mod.REQUIRED_RUNTIME_PHASES if not phases.get(p)]
        if missing:
            failures.append(
                f"{module}: the run produced no '[module-lifecycle]' trace for "
                f"{missing}. Either the module did not reach those phases, or "
                f"ModuleManager does not yet emit the phase trace this contract "
                f"requires. Evidence is not written for an unproven run."
            )
            continue

        records.append({
            "module": module,
            "sharedLibrary": expected_library_names(module)[
                "windows" if sys.platform == "win32" else "linux"
            ],
            "sourceDirectory": source_dir,
            "sourceTreeSHA": tree_sha,
            "runner": "headless-exec",
            "phases": phases,
            "engineSHA256": engine_digest,
            "enginePath": engine_path_str,
        })

    if failures:
        print("FATAL: lifecycle evidence could not be produced:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        print("No evidence file written.", file=sys.stderr)
        return 1

    document = {
        "schemaVersion": lifecycle_mod.LIFECYCLE_SCHEMA_VERSION,
        "generatedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "commitSHA": sha,
        "records": records,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    print(f"OK: wrote {args.out} with {len(records)} lifecycle record(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
