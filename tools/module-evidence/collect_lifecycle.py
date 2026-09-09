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
import stat
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import lifecycle as lifecycle_mod  # noqa: E402
import paths as paths_mod  # noqa: E402
from provenance import resolve_head_sha  # noqa: E402
from schema import expected_library_names  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]

_ENGINE_NAME = "SparkEngine.exe"
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
    if engine.name != _ENGINE_NAME:
        return (
            f"engine filename {engine.name!r} is not the required stable-v1 "
            f"engine {_ENGINE_NAME!r}"
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
    if not _is_pe_image(header, engine):
        return f"engine binary {engine} is not a valid PE image"
    return None


_TERMINAL_COUNTS = (
    ("create", "CreateModule"), ("load", "OnLoad"), ("update", "OnUpdate"),
    ("fixed", "OnFixedUpdate"), ("render", "OnRender"),
    ("unload", "OnUnload"), ("destroy", "DestroyModule"),
)


def _is_pe_image(header: bytes, path: Path) -> bool:
    """Check the inexpensive PE identity properties needed before launch."""
    if len(header) < 64 or header[:2] != b"MZ":
        return False
    pe_offset = int.from_bytes(header[0x3C:0x40], "little")
    try:
        with path.open("rb") as image:
            image.seek(pe_offset)
            return image.read(4) == b"PE\0\0"
    except OSError:
        return False


def _absolute_raw(path: Path) -> Path:
    return Path(os.path.abspath(path))


def _canonicalize_images(engine: Path, module_image: Path,
                         working_directory: Path) -> tuple[tuple[Path, Path, Path] | None, str | None]:
    """Reject raw reparse paths before producing stable canonical identities."""
    root_raw = _absolute_raw(working_directory)
    images_raw = (("engine", _absolute_raw(engine)), ("module", _absolute_raw(module_image)))
    if not root_raw.is_dir() or paths_mod._is_reparse_point(root_raw):
        return None, f"working directory {root_raw} must be a real non-reparse directory"
    for role, raw_image in images_raw:
        try:
            relative = raw_image.relative_to(root_raw)
        except ValueError:
            return None, f"{role} image {raw_image} lies outside working directory {root_raw}"
        current = root_raw
        if paths_mod._is_reparse_point(current):
            return None, f"working directory {current} is a reparse point"
        for component in relative.parts:
            current /= component
            if paths_mod._is_reparse_point(current):
                return None, f"{role} image component {current} is a reparse point"
    try:
        root = root_raw.resolve(strict=True)
        canonical_engine = images_raw[0][1].resolve(strict=True)
        canonical_module = images_raw[1][1].resolve(strict=True)
    except OSError as exc:
        return None, f"cannot resolve lifecycle image paths: {exc}"
    if root not in canonical_engine.parents or root not in canonical_module.parents:
        return None, "canonical lifecycle image path escaped the working directory"
    return (root, canonical_engine, canonical_module), None


def _validate_image(path: Path, root: Path, *, role: str,
                    expected_name: str | None = None) -> str | None:
    """Reject an image that is not a real, in-package PE file."""
    if root not in path.parents:
        return f"{role} image {path} lies outside working directory {root}"
    try:
        mode = path.stat().st_mode
    except OSError as exc:
        return f"cannot stat {role} image {path}: {exc}"
    if not stat.S_ISREG(mode) or paths_mod._is_reparse_point(path):
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
    if not _is_pe_image(header, path):
        return f"{role} image {path} is not a PE binary"
    return None


def validate_image_pair(engine: Path, module_image: Path, module: str,
                        working_directory: Path) -> str | None:
    """Validate the exact engine/module image pair that will be launched."""
    prepared, error = _canonicalize_images(engine, module_image, working_directory)
    if error:
        return error
    assert prepared is not None
    root, canonical_engine, canonical_module = prepared
    error = validate_engine_binary(canonical_engine)
    if error:
        return error
    return _validate_image(
        canonical_module, root, role="module",
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
    if os.name != "nt":
        return "", "stable-v1 lifecycle evidence is Windows-only"
    if module != "SparkGameFPS":
        return "", "stable-v1 lifecycle evidence accepts only module SparkGameFPS"
    if rhi_backend != "d3d11":
        return "", "stable-v1 lifecycle evidence requires --rhi-backend d3d11"
    prepared, err = _canonicalize_images(engine, module_image, working_directory)
    if err:
        return "", err
    assert prepared is not None
    root, engine, module_image = prepared
    err = validate_engine_binary(engine)
    if err:
        return "", err
    err = _validate_image(module_image, root, role="module",
                          expected_name=expected_library_names(module)["windows"])
    if err:
        return "", err
    try:
        before_engine = hash_engine_binary(engine)
        before_module = hash_engine_binary(module_image)
    except OSError as exc:
        return "", f"cannot hash lifecycle image before launch: {exc}"
    cmd = [
        str(engine), "-game", str(module_image), "-require-game",
        "-test-seconds", "1.0", "-threads", "2", "-window-size", "640x360",
        "-no-subprocess",
    ]
    print(f"[collect_lifecycle] {' '.join(cmd)}", flush=True)
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout,
            check=False, cwd=str(root),
            env={**os.environ, "SPARK_RHI_BACKEND": "d3d11",
                 "SPARK_D3D11_DRIVER": "warp"},
        )
    except subprocess.TimeoutExpired:
        return "", f"engine run exceeded {timeout}s without completing"
    except OSError as exc:
        return "", f"cannot launch engine: {exc}"

    captured = (proc.stdout or "") + (proc.stderr or "")
    try:
        if (hash_engine_binary(engine), hash_engine_binary(module_image)) != (before_engine, before_module):
            return captured, "engine or module image changed while the lifecycle run executed"
    except OSError as exc:
        return captured, f"cannot hash lifecycle image after launch: {exc}"
    if proc.returncode != 0:
        return captured, (
            f"engine exited {proc.returncode}; a failed run is not evidence of "
            "a working lifecycle"
        )
    return captured, None


def _clear_artifacts(*paths: Path) -> str | None:
    """Remove only the two declared final artifacts, never a directory tree."""
    for path in paths:
        try:
            if path.is_dir() and not paths_mod._is_reparse_point(path):
                return f"artifact path {path} is a directory and cannot be safely cleared"
            if path.exists() or paths_mod._is_reparse_point(path):
                path.unlink()
        except OSError as exc:
            return f"cannot clear stale lifecycle artifact {path}: {exc}"
    return None


def _write_temp(final_path: Path, content: str) -> Path:
    final_path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temp_name = tempfile.mkstemp(prefix=f".{final_path.name}.", suffix=".tmp",
                                             dir=final_path.parent, text=True)
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(content)
    return Path(temp_name)


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
    out_path = _absolute_raw(args.out)
    module = args.modules[0] if len(args.modules) == 1 else "invalid-module"
    log_path = out_path.parent / f"module-lifecycle-{module}.log"
    cleared = _clear_artifacts(out_path, log_path)
    if cleared:
        print(f"FATAL: {cleared}", file=sys.stderr)
        return 1

    log_temp: Path | None = None
    json_temp: Path | None = None
    success = False
    try:
        if os.name != "nt":
            raise RuntimeError("stable-v1 lifecycle evidence is Windows-only")
        if len(args.modules) != 1 or args.modules[0] != "SparkGameFPS":
            raise RuntimeError("stable-v1 lifecycle evidence accepts only module SparkGameFPS")
        if args.rhi_backend != "d3d11":
            raise RuntimeError("stable-v1 lifecycle evidence requires --rhi-backend d3d11")
        sha = args.commit_sha
        if sha is None:
            sha, err = resolve_head_sha(REPO_ROOT)
            if sha is None:
                raise RuntimeError(err)
        prepared, err = _canonicalize_images(
            args.engine, args.module_image, args.working_directory
        )
        if err:
            raise RuntimeError(err)
        assert prepared is not None
        root, engine, module_image = prepared
        if (err := validate_engine_binary(engine)) is not None:
            raise RuntimeError(err)
        if (err := _validate_image(module_image, root, role="module",
                                  expected_name=expected_library_names(module)["windows"])) is not None:
            raise RuntimeError(err)
        try:
            engine_digest = hash_engine_binary(engine)
            module_digest = hash_engine_binary(module_image)
        except OSError as exc:
            raise RuntimeError(f"cannot hash lifecycle image before launch: {exc}") from exc
        source_dir = f"GameModules/{module}/Source"
        tree_sha, err = lifecycle_mod.source_tree_sha(REPO_ROOT, sha, source_dir)
        if tree_sha is None:
            raise RuntimeError(err)
        captured, err = run_engine(engine, module_image, module, root,
                                   args.rhi_backend, args.timeout)
        if err:
            raise RuntimeError(err)
        phases = parse_terminal_record(captured, module)
        try:
            if (hash_engine_binary(engine), hash_engine_binary(module_image)) != (engine_digest, module_digest):
                raise RuntimeError("engine or module image changed during lifecycle collection")
        except OSError as exc:
            raise RuntimeError(f"cannot hash lifecycle image after launch: {exc}") from exc
        record = {
            "module": module,
            "sharedLibrary": expected_library_names(module)["windows"],
            "sourceDirectory": source_dir,
            "sourceTreeSHA": tree_sha,
            "runner": "headless-exec",
            "phases": phases,
            "engineSHA256": engine_digest,
            "enginePath": str(engine),
            "moduleSHA256": module_digest,
            "modulePath": str(module_image),
        }
        document = {
            "schemaVersion": lifecycle_mod.LIFECYCLE_SCHEMA_VERSION,
            "generatedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "commitSHA": sha,
            "records": [record],
        }
        log_temp = _write_temp(log_path, captured)
        json_temp = _write_temp(out_path, json.dumps(document, indent=2, sort_keys=True) + "\n")
        os.replace(log_temp, log_path)
        log_temp = None
        os.replace(json_temp, out_path)
        json_temp = None
        success = True
        print(f"OK: wrote {out_path} with 1 lifecycle record")
        return 0
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"FATAL: lifecycle evidence could not be produced: {exc}", file=sys.stderr)
        return 1
    finally:
        for temporary in (log_temp, json_temp):
            if temporary is not None:
                try:
                    temporary.unlink(missing_ok=True)
                except OSError:
                    pass
        if not success:
            _clear_artifacts(out_path, log_path)


if __name__ == "__main__":
    raise SystemExit(main())
