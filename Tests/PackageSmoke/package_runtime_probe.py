#!/usr/bin/env python3
"""Qualify an installed SparkGameFPS package without requiring elevation.

This driver deliberately composes the repository's existing CMake and
AppContainer probes.  It owns orchestration and the small, closed evidence
record consumed by ``tools/module-evidence/artifacts.py``; it does not build,
install, or elevate anything.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
from dataclasses import dataclass


SCENE_RELATIVE = Path("bin") / "Assets" / "Scenes" / "level1.scene"
ASSETS_RELATIVE = Path("bin") / "Assets"
_COUNT_RE = re.compile(r"^OK: ([0-9]+) entries verified", re.MULTILINE)


@dataclass(frozen=True)
class CommandResult:
    """Captured result of one bounded qualification command."""

    returncode: int
    stdout: str
    stderr: str


def run_command(command: list[str], *, cwd: Path | None = None,
                env: dict[str, str] | None = None, timeout: int = 300) -> CommandResult:
    """Run one child without shell interpretation or elevation."""
    result = subprocess.run(
        command,
        cwd=str(cwd) if cwd else None,
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
        check=False,
    )
    return CommandResult(result.returncode, result.stdout, result.stderr)


def _require_regular(path: Path, label: str) -> None:
    if (not path.is_file() or path.is_symlink()
            or getattr(path.lstat(), "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)):
        raise RuntimeError(f"{label} is missing or link-like: {path}")


def validate_package_layout(package_root: Path) -> None:
    """Reject a missing or link-like installed runtime before launching it."""
    if not package_root.is_dir() or package_root.is_symlink():
        raise RuntimeError(f"package root is missing or link-like: {package_root}")
    # Copying or deleting a control must never traverse a package-provided
    # junction. Check directories as well as the required leaf files.
    for directory, dirs, files in os.walk(package_root):
        for name in (*dirs, *files):
            entry = Path(directory) / name
            if (entry.is_symlink() or getattr(entry.lstat(), "st_file_attributes", 0)
                    & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)):
                raise RuntimeError(f"package payload contains a link: {entry}")
    for relative in (
        Path("bin/SparkEngine.exe"),
        Path("bin/SparkGameFPS.dll"),
        Path("bin/Assets/assets.integrity.json"),
        SCENE_RELATIVE,
    ):
        _require_regular(package_root / relative, f"package payload {relative}")
    assets = package_root / ASSETS_RELATIVE
    if not assets.is_dir() or assets.is_symlink():
        raise RuntimeError(f"package asset root is missing or link-like: {assets}")


def verify_assets(args: argparse.Namespace, package_root: Path, output_dir: Path) -> int:
    manifest = package_root / ASSETS_RELATIVE / "assets.integrity.json"
    command = [
        str(args.python), "-B", str(args.source_root / "tools/asset-integrity/verify_asset_integrity.py"),
        "verify", str(manifest), "--root", str(package_root / ASSETS_RELATIVE),
        "--profile", "stable-v1", "--source-manifest",
        str(args.source_root / "Assets/assets.integrity.json"),
    ]
    result = run_command(command, timeout=180)
    (output_dir / "asset-integrity.stdout.log").write_text(result.stdout, encoding="utf-8")
    (output_dir / "asset-integrity.stderr.log").write_text(result.stderr, encoding="utf-8")
    if result.returncode != 0:
        raise RuntimeError(f"asset integrity failed ({result.returncode}): {result.stdout}{result.stderr}")
    counts = _COUNT_RE.findall(result.stdout)
    if len(counts) != 1 or int(counts[0]) <= 0:
        raise RuntimeError(f"asset verifier emitted no positive terminal count: {result.stdout!r}")
    reference_result = run_command([
        str(args.python), "-B", str(args.source_root / "tools/asset-integrity/verify_asset_integrity.py"),
        "references", str(manifest), "--root", str(package_root / ASSETS_RELATIVE),
    ], timeout=180)
    (output_dir / "asset-references.stdout.log").write_text(reference_result.stdout, encoding="utf-8")
    (output_dir / "asset-references.stderr.log").write_text(reference_result.stderr, encoding="utf-8")
    references = re.findall(
        r"^OK: ([0-9]+) references in ([0-9]+) scene and material files resolve to listed staged assets$",
        reference_result.stdout, re.MULTILINE)
    if (reference_result.returncode != 0 or len(references) != 1
            or any(int(count) <= 0 for count in references[0])):
        raise RuntimeError(f"installed asset reference closure failed: {reference_result.stdout}{reference_result.stderr}")
    return int(counts[0])


def run_save_reload(args: argparse.Namespace, package_root: Path, output_dir: Path) -> None:
    command = [
        str(args.cmake),
        f"-DSPARK_ENGINE_EXECUTABLE={package_root / 'bin/SparkEngine.exe'}",
        f"-DSPARK_GAME_MODULE={package_root / 'bin/SparkGameFPS.dll'}",
        f"-DSPARK_PACKAGE_ROOT={package_root}",
        f"-DSPARK_FORBIDDEN_ROOTS={args.source_root}|{args.build_root}",
        f"-DSPARK_TEST_ROOT={output_dir / 'save-reload'}",
        "-P", str(args.source_root / "cmake/RunSparkHeadlessFPSSaveReload.cmake"),
    ]
    result = run_command(command, timeout=360)
    (output_dir / "save-reload.stdout.log").write_text(result.stdout, encoding="utf-8")
    (output_dir / "save-reload.stderr.log").write_text(result.stderr, encoding="utf-8")
    if result.returncode != 0:
        raise RuntimeError(f"package save/reload failed ({result.returncode}): {result.stdout}{result.stderr}")


def run_authored_scene(args: argparse.Namespace, package_root: Path, output_dir: Path) -> None:
    """Run the existing D3D11 authored-scene/material terminal contract."""
    command = [
        str(args.cmake),
        f"-DSPARK_PACKAGE_BIN={package_root / 'bin'}",
        f"-DSPARK_TEST_ROOT={output_dir / 'authored-scene'}",
        f"-DSPARK_SOURCE_ROOT={args.source_root}",
        "-P", str(args.source_root / "Tests/PackageSmoke/VerifyFPSAuthoredScene.cmake"),
    ]
    result = run_command(command, timeout=360)
    (output_dir / "authored-scene.stdout.log").write_text(result.stdout, encoding="utf-8")
    (output_dir / "authored-scene.stderr.log").write_text(result.stderr, encoding="utf-8")
    if result.returncode != 0:
        raise RuntimeError(f"authored-scene probe failed ({result.returncode}): {result.stdout}{result.stderr}")


def run_repository_isolation(args: argparse.Namespace, package_root: Path, output_dir: Path) -> None:
    """Run the existing AppContainer positive and negative controls."""
    if os.name != "nt":
        raise RuntimeError("repository isolation requires the Windows AppContainer token")
    isolation = output_dir / "repository-isolation"
    copies = {
        "package": isolation / "package",
        "scene-less-package": isolation / "package-no-scene",
        "asset-less-package": isolation / "package-no-assets",
    }
    for copy in copies.values():
        shutil.copytree(package_root, copy, symlinks=True)
    (copies["scene-less-package"] / SCENE_RELATIVE).unlink()
    control_root = copies["asset-less-package"].resolve(strict=True)
    control_assets = (control_root / ASSETS_RELATIVE).resolve(strict=True)
    if control_assets == control_root or not control_assets.is_relative_to(control_root):
        raise RuntimeError("asset-less control removal escaped the owned copy")
    shutil.rmtree(control_assets)
    runs = isolation / "runs"
    runs.mkdir(parents=True)
    command = [
        str(args.python), "-B", str(args.source_root / "Tests/PackageSmoke/windows_appcontainer_run.py"),
        "--source-root", str(args.source_root), "--build-root", str(args.build_root),
        "--package", str(copies["package"]),
        "--scene-less-package", str(copies["scene-less-package"]),
        "--asset-less-package", str(copies["asset-less-package"]),
        "--output", str(runs),
        "--frame-check", str(args.source_root / "Tests/PackageSmoke/CheckFPSVisibleFrame.ps1"),
    ]
    result = run_command(command, timeout=600)
    (isolation / "stdout.log").write_text(result.stdout, encoding="utf-8")
    (isolation / "stderr.log").write_text(result.stderr, encoding="utf-8")
    if result.returncode != 0:
        raise RuntimeError(f"repository isolation failed ({result.returncode}): {result.stdout}{result.stderr}")


def write_result(path: Path, args: argparse.Namespace, *, asset_entries: int) -> None:
    """Write runtime-only evidence; the coordinator owns canonical smoke records."""
    result = {
        "schemaVersion": 1,
        "result": "PASS",
        "module": "SparkGameFPS",
        "commitSha": args.commit_sha,
        "msiSha256": args.msi_sha256,
        "assetIntegrity": {"result": "PASS", "entries": asset_entries},
        "authoredSceneVisual": {"result": "PASS", "contract": "VerifyFPSAuthoredScene.cmake"},
        "saveReload": {"result": "PASS", "contract": "RunSparkHeadlessFPSSaveReload.cmake"},
        "repositoryIsolation": {"result": "PASS", "contract": "windows_appcontainer_run.py"},
        "headlessNoDisplay": "unproven",
    }
    path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def publish_diagnostics(output_dir: Path, destination: Path) -> None:
    """Retain generated logs and frames, excluding all copies of the product."""
    destination.mkdir(parents=True, exist_ok=False)
    total = 0
    for directory, dirs, files in os.walk(output_dir):
        dirs[:] = [name for name in dirs if name not in {
            "bin", "Assets", "package", "package-no-scene", "package-no-assets"}
            and not (Path(directory) / name).is_symlink()
            and not (getattr((Path(directory) / name).lstat(), "st_file_attributes", 0)
                     & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0))]
        for name in files:
            source = Path(directory) / name
            if source.suffix not in {".log", ".png"} and name not in {"appcontainer-summary.json", "evidence.txt"}:
                continue
            _require_regular(source, "runtime diagnostic")
            size = source.stat().st_size
            total += size
            if size > 64 * 1024 * 1024 or total > 512 * 1024 * 1024:
                raise RuntimeError("runtime diagnostics exceed bounded artifact size")
            target = destination / source.relative_to(output_dir)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)


def qualify(args: argparse.Namespace) -> None:
    args.source_root = args.source_root.resolve(strict=True)
    args.build_root = args.build_root.resolve(strict=True)
    package_root = args.package_root.resolve()
    output_dir = args.output_dir.resolve()
    for root in (args.source_root.resolve(), args.build_root.resolve(), package_root):
        try:
            output_dir.relative_to(root)
        except ValueError:
            continue
        raise RuntimeError(f"runtime scratch must be outside source/build/package roots: {output_dir}")
    if args.diagnostics_dir is not None and args.diagnostics_dir.resolve().is_relative_to(output_dir):
        raise RuntimeError("diagnostic publication must be outside runtime scratch")
    output_dir.mkdir(parents=True, exist_ok=False)
    try:
        validate_package_layout(package_root)
        asset_entries = verify_assets(args, package_root, output_dir)
        run_authored_scene(args, package_root, output_dir)
        run_save_reload(args, package_root, output_dir)
        run_repository_isolation(args, package_root, output_dir)
        write_result(args.record_file or output_dir / "runtime-result.json", args,
                     asset_entries=asset_entries)
    finally:
        if args.diagnostics_dir is not None:
            publish_diagnostics(output_dir, args.diagnostics_dir)
    print(json.dumps({"asset_entries": asset_entries, "result": "PASS"}, sort_keys=True))


def parse_args(argv: list[str]) -> argparse.Namespace:
    def hex_value(value: str, length: int, label: str) -> str:
        if not re.fullmatch(rf"[0-9a-f]{{{length}}}", value):
            raise argparse.ArgumentTypeError(f"{label} must be {length} lowercase hex characters")
        return value

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-root", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--commit-sha", required=True,
                        type=lambda value: hex_value(value, 40, "commit SHA"))
    parser.add_argument("--msi-sha256", required=True,
                        type=lambda value: hex_value(value, 64, "MSI SHA-256"))
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument("--cmake", type=Path, default=Path("cmake"))
    parser.add_argument("--record-file", type=Path)
    parser.add_argument("--diagnostics-dir", type=Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    try:
        qualify(parse_args(argv or sys.argv[1:]))
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"package runtime qualification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
