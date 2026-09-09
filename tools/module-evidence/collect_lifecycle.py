#!/usr/bin/env python3
"""Produce runtime lifecycle evidence by really running a module.

This is the *producer* half of the lifecycle contract. It launches the exact
stable-v1 Windows command against its built game DLL and accepts one direct,
host-owned terminal record emitted after teardown:

    SPARK_MODULE_LIFECYCLE module=SparkGameFPS create=1 load=1 update=4 \
    fixed=2 render=4 unload=1 destroy=1 faults=0

If the engine binary is missing, the run fails, or the trace contains none of
the required markers, this program writes **no evidence file** and exits
non-zero.  A partial or empty document is never emitted: absent evidence must
present as absent, not as a module that ran and did nothing.

Usage:
    python tools/module-evidence/collect_lifecycle.py \
        --engine package/SparkEngine.exe --module SparkGameFPS \
        --module-image package/SparkGameFPS.dll --working-directory package \
        --rhi-backend d3d11 --out build/module-evidence/module-lifecycle.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
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


_TERMINAL_COUNTS = (
    ("create", "CreateModule"), ("load", "OnLoad"), ("update", "OnUpdate"),
    ("fixed", "OnFixedUpdate"), ("render", "OnRender"),
    ("unload", "OnUnload"), ("destroy", "DestroyModule"),
)


def _validate_image(path: Path, root: Path, *, role: str,
                    expected_name: str | None = None) -> str | None:
    """Reject an image that is not a real, in-package PE file."""
    try:
        root = root.resolve(strict=True)
        candidate = path.resolve(strict=True)
    except OSError as exc:
        return f"cannot resolve {role} image {path}: {exc}"
    if not root.is_dir() or paths_mod._is_reparse_point(root):
        return f"working directory {root} must be a real non-reparse directory"
    if root not in candidate.parents:
        return f"{role} image {path} lies outside working directory {root}"
    if not path.is_file() or paths_mod._is_reparse_point(path):
        return f"{role} image {path} must be a regular non-reparse file"
    if expected_name is not None and path.name != expected_name:
        return f"{role} image {path.name!r} must be the Windows module {expected_name!r}"
    if path.suffix.lower() in _SCRIPT_EXTENSIONS:
        return f"{role} image {path} is a script, not a compiled binary"
    try:
        with path.open("rb") as image:
            header = image.read(64)
    except OSError as exc:
        return f"cannot read {role} image {path}: {exc}"
    if len(header) < 2 or header[:2] != b"MZ":
        return f"{role} image {path} is not a PE binary"
    return None


def validate_image_pair(engine: Path, module_image: Path, module: str,
                        working_directory: Path) -> str | None:
    """Validate the exact engine/module image pair that will be launched."""
    error = _validate_image(engine, working_directory, role="engine")
    if error:
        return error
    return _validate_image(
        module_image, working_directory, role="module",
        expected_name=expected_library_names(module)["windows"],
    )


def parse_terminal_record(text: str, module: str) -> dict[str, int]:
    """Parse exactly one direct, standalone host lifecycle record."""
    fields = " ".join(
        f"{name}=(?P<{name}>[0-9]+)" for name, _ in _TERMINAL_COUNTS
    ) + " faults=(?P<faults>[0-9]+)"
    record_re = re.compile(
        rf"^SPARK_MODULE_LIFECYCLE module={re.escape(module)} {fields}$"
    )
    matching = [line for line in text.splitlines() if line.startswith("SPARK_MODULE_LIFECYCLE")]
    if len(matching) != 1:
        raise ValueError(f"expected exactly one standalone lifecycle record for {module}")
    match = record_re.fullmatch(matching[0])
    if match is None:
        raise ValueError(f"malformed, wrong-module, or unknown-key lifecycle record for {module}")
    values = {name: int(match.group(name)) for name, _ in _TERMINAL_COUNTS}
    faults = int(match.group("faults"))
    if any(value == 0 for value in values.values()) or faults != 0:
        raise ValueError(f"incomplete or faulted lifecycle record for {module}")
    return {phase: values[name] for name, phase in _TERMINAL_COUNTS}


def run_engine(engine: Path, module_image: Path, module: str,
               working_directory: Path, rhi_backend: str,
               timeout: int) -> tuple[str, str | None]:
    """Run only the stable-v1 Windows lifecycle command."""
    if rhi_backend != "d3d11":
        return "", "stable-v1 lifecycle evidence requires --rhi-backend d3d11"
    err = validate_image_pair(engine, module_image, module, working_directory)
    if err:
        return "", err
    cmd = [
        str(engine), "-game", str(module_image), "-require-game",
        "-test-seconds", "1.0", "-threads", "2", "-window-size", "640x360",
        "-no-subprocess",
    ]
    print(f"[collect_lifecycle] {' '.join(cmd)}", flush=True)
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout,
            check=False, cwd=str(working_directory),
            env={**os.environ, "SPARK_RHI_BACKEND": "d3d11",
                 "SPARK_D3D11_DRIVER": "warp"},
        )
    except subprocess.TimeoutExpired:
        return "", f"engine run exceeded {timeout}s without completing"
    except OSError as exc:
        return "", f"cannot launch engine: {exc}"

    captured = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode != 0:
        return captured, (
            f"engine exited {proc.returncode}; a failed run is not evidence of "
            "a working lifecycle"
        )
    return captured, None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--module", action="append", required=True, dest="modules")
    parser.add_argument("--module-image", type=Path, required=True)
    parser.add_argument("--working-directory", type=Path, required=True)
    parser.add_argument("--rhi-backend", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--commit-sha", default=None)
    args = parser.parse_args()

    sha = args.commit_sha
    if sha is None:
        sha, err = resolve_head_sha(REPO_ROOT)
        if sha is None:
            print(f"FATAL: {err}", file=sys.stderr)
            return 1

    if len(args.modules) != 1:
        print("FATAL: stable-v1 lifecycle evidence accepts exactly one module", file=sys.stderr)
        return 1
    image_err = validate_image_pair(
        args.engine, args.module_image, args.modules[0], args.working_directory
    )
    if image_err:
        print(f"FATAL: {image_err}", file=sys.stderr)
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
        captured, err = run_engine(args.engine, args.module_image, module,
                                   args.working_directory, args.rhi_backend,
                                   args.timeout)
        if err:
            failures.append(f"{module}: {err}")
            continue

        try:
            phases = parse_terminal_record(captured, module)
        except ValueError as exc:
            failures.append(
                f"{module}: {exc}; evidence is not written for an unproven run."
            )
            continue

        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text(captured, encoding="utf-8", errors="replace")

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
            "moduleSHA256": hash_engine_binary(args.module_image),
            "modulePath": str(args.module_image),
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
