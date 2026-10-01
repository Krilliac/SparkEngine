#!/usr/bin/env python3
"""Run the packaged SparkGameVisualScript demo from a foreign directory.

The caller supplies a package produced by the real Spark CLI packager.  This
runner deliberately does not fall back to a build tree: the engine, module,
ABI sidecar, module manifest, and generated AngelScript files must all be in
the package before the process is launched.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


WIN_MARKER = "*** YOU WIN! ***"
AUTOPLAY_SEGMENTS = (
    # DemoWorld::Spawn: x=-10+5*i, z=8+4*(i%2), for i in [0,4].
    (0, "-10", "8"),
    (30, "-5", "12"),
    (60, "0", "8"),
    (90, "5", "12"),
    (120, "10", "8"),
)


def build_autoplay_script() -> str:
    """Return the fixed input timeline used by the packaged smoke."""

    return "\n".join(f"{frame} vs_autoplay {x} {z}" for frame, x, z in AUTOPLAY_SEGMENTS) + "\n"


def validate_output(output: str, returncode: int) -> str | None:
    """Return an actionable failure, or None when the packaged run passed."""

    if returncode != 0:
        return f"packaged engine exited with status {returncode}"
    wins = output.count(WIN_MARKER)
    if wins != 1:
        return f"expected exactly one {WIN_MARKER!r}, found {wins}"
    lifecycle = [line for line in output.splitlines() if line.startswith("SPARK_HEADLESS_LIFECYCLE")]
    if len(lifecycle) != 1 or not re.fullmatch(
        r"SPARK_HEADLESS_LIFECYCLE initialized=1 updated=[1-9][0-9]* fixed=[0-9]+ rendered=0 unloaded=1 faults=0",
        lifecycle[0],
    ):
        return "missing or dirty packaged module lifecycle record"
    for failure in ("*** GAME OVER ***", "ERROR: AddressSanitizer", "ERROR: LeakSanitizer",
                    "WARNING: ThreadSanitizer", "Script disabled until it is re-attached or hot-reloaded."):
        if failure in output:
            return f"packaged engine reported {failure}"
    return None


def validate_package(package_dir: Path, engine: Path, module: Path) -> list[str]:
    """Return missing or escaping package members."""

    required = [
        engine,
        module,
        Path(str(module) + ".sparkabi"),
        package_dir / "spark.modules.json",
    ]
    generated = package_dir / "Assets" / "Scripts" / "Generated"
    root = package_dir.resolve()
    missing = [str(path) for path in required if not path.is_file() or not path.resolve().is_relative_to(root)]
    if not generated.is_dir() or not any(generated.glob("*.as")):
        missing.append(str(generated / "*.as"))
    return missing


def run_package(
    package_dir: Path,
    engine_name: str = "SparkEngine",
    module_name: str = "SparkGameVisualScript",
    foreign_cwd: Path | None = None,
    log_path: Path | None = None,
    timeout: float = 120.0,
) -> int:
    """Launch the package and enforce one successful visual-script win."""

    package_dir = package_dir.resolve()
    engine = package_dir / engine_name
    module = package_dir / module_name
    if os.name == "nt":
        if engine.suffix.lower() != ".exe":
            engine = engine.with_suffix(".exe")
        if module.suffix.lower() != ".dll":
            module = module.with_suffix(".dll")
    missing = validate_package(package_dir, engine, module)
    if missing:
        print("visual-script package is incomplete:", file=sys.stderr)
        for path in missing:
            print(f"  missing: {path}", file=sys.stderr)
        return 2

    run_cwd = (foreign_cwd or package_dir.parent / "foreign-cwd").resolve()
    if run_cwd.is_relative_to(package_dir):
        print("foreign working directory must be outside the package", file=sys.stderr)
        return 2
    run_cwd.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="visual-script-package-") as temp_dir:
        temp_root = Path(temp_dir)
        exec_file = temp_root / "autoplay.exec"
        exec_file.write_text(build_autoplay_script(), encoding="utf-8", newline="\n")
        output_log = log_path.resolve() if log_path else temp_root / "visual-script-package.log"
        output_log.parent.mkdir(parents=True, exist_ok=True)
        command = [
            str(engine),
            "-headless",
            "-manifest",
            str(package_dir / "spark.modules.json"),
            "-require-game",
            "-exec",
            str(exec_file),
            "-exec-audit",
            str(temp_root / "exec_audit.log"),
            "-test-frames",
            "480",
            "-threads",
            "2",
            "-no-subprocess",
        ]
        print("launch:", " ".join(command))
        try:
            env = os.environ.copy()
            for key in ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME",
                        "LOCALAPPDATA", "APPDATA"):
                user_path = temp_root / key.lower()
                user_path.mkdir()
                env[key] = str(user_path)
            completed = subprocess.run(
                command,
                cwd=run_cwd,
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=timeout,
                check=False,
            )
        except subprocess.TimeoutExpired as error:
            output = error.stdout or ""
            if isinstance(output, bytes):
                output = output.decode("utf-8", errors="replace")
            output_log.write_text(output, encoding="utf-8")
            print(f"visual-script package timed out after {timeout:.1f}s", file=sys.stderr)
            return 124

        output = completed.stdout or ""
        output_log.write_text(output, encoding="utf-8")
        wins = output.count(WIN_MARKER)
        print(f"exit={completed.returncode} win_markers={wins} log={output_log}")
        failure = validate_output(output, completed.returncode)
        if failure:
            print(failure, file=sys.stderr)
            print(output, file=sys.stderr)
            return completed.returncode or 1
        return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-dir", type=Path, required=True)
    parser.add_argument("--engine", default="SparkEngine")
    parser.add_argument("--module", default="SparkGameVisualScript")
    parser.add_argument("--foreign-cwd", type=Path)
    parser.add_argument("--log", type=Path)
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args(argv)
    return run_package(args.package_dir, args.engine, args.module, args.foreign_cwd, args.log, args.timeout)


if __name__ == "__main__":
    raise SystemExit(main())
