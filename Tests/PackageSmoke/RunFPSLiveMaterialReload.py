#!/usr/bin/env python3
"""Change one installed FPS material while the same engine process is running."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import time


def complete_png(path: Path) -> bool:
    if not path.is_file():
        return False
    try:
        data = path.read_bytes()
    except OSError:
        return False
    return len(data) >= 20 and data[:8] == b"\x89PNG\r\n\x1a\n" and data[-8:-4] == b"IEND"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-bin", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    args = parser.parse_args()

    package_bin = args.package_bin.resolve(strict=True)
    work_root = args.work_root.resolve(strict=False)
    if not package_bin.is_dir() or work_root.exists():
        parser.error("package bin must exist and work root must be new")
    work_root.mkdir(parents=True)

    material_path = package_bin / "Assets" / "Materials" / "Arena_CenterBuilding.json"
    original = material_path.read_bytes()
    old_albedo = b"Textures/concrete_diffuse.png"
    if original.count(old_albedo) != 1:
        raise RuntimeError("center-building material has no unique concrete baseline")
    replacement = original.replace(old_albedo, b"Textures/wood_diffuse.png", 1)

    script = work_root / "live-reload.exec"
    script.write_text(
        "t1 gfx_screenshot live-before.png\n"
        "t5 scene_load level1.scene\n"
        "t6 gfx_screenshot live-after.png\n",
        encoding="utf-8",
    )
    env = os.environ.copy()
    env.update(
        SPARK_RHI_BACKEND="d3d11",
        SPARK_D3D11_DRIVER="warp",
        LOCALAPPDATA=str(work_root / "localappdata"),
    )
    command = [
        str(package_bin / "SparkEngine.exe"),
        "-game", str(package_bin / "SparkGameFPS.dll"), "-require-game",
        "-test-seconds", "8", "-threads", "2", "-window-size", "640x360",
        "-no-subprocess", "-exec", str(script),
    ]
    before = work_root / "live-before.png"
    after = work_root / "live-after.png"
    process: subprocess.Popen[str] | None = None
    try:
        with (work_root / "stdout.log").open("w", encoding="utf-8") as stdout, \
             (work_root / "stderr.log").open("w", encoding="utf-8") as stderr:
            process = subprocess.Popen(command, cwd=work_root, env=env, stdout=stdout, stderr=stderr)
            deadline = time.monotonic() + 18
            while time.monotonic() < deadline and process.poll() is None:
                if complete_png(before):
                    break
                time.sleep(0.05)
            else:
                raise RuntimeError("engine did not produce the baseline PNG before the deadline")

            material_path.write_bytes(replacement)
            try:
                exit_code = process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                raise RuntimeError("engine exceeded bounded live-reload runtime") from None
    finally:
        try:
            if process is not None and process.poll() is None:
                process.kill()
                process.wait(timeout=5)
        finally:
            material_path.write_bytes(original)

    if exit_code != 0 or not complete_png(after):
        raise RuntimeError(f"engine failed live material reload run: exit={exit_code}, after={after.exists()}")
    audit = (work_root / "exec_audit.log").read_text(encoding="utf-8", errors="replace")
    for marker in (
        "| ok  | gfx_screenshot live-before.png",
        "| ok  | scene_load level1.scene",
        "Scene reloaded successfully: level1.scene",
        "| ok  | gfx_screenshot live-after.png",
    ):
        if marker not in audit:
            raise RuntimeError(f"installed live-reload audit missed {marker!r}")
    print(f"Installed same-process scene_load completed; before={before}; after={after}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
