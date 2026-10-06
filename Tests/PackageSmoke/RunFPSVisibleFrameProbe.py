#!/usr/bin/env python3
"""Prove the Windows FPS visible-frame check rejects a real procedural fallback."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import uuid


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-bin", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--unicode-path", action="store_true", help="exercise a non-ASCII copied runtime directory")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("this probe requires Windows D3D11 WARP and Windows PowerShell")
    package_bin = args.package_bin.resolve(strict=True)
    work_root = args.work_root.resolve()
    if work_root == package_bin or package_bin in work_root.parents:
        parser.error("work root must be outside the input bin directory")
    for name in ("SparkEngine.exe", "SparkGameFPS.dll", "SparkGameFPS.dll.sparkabi", "Assets/Scenes/level1.scene"):
        if not (package_bin / name).is_file():
            parser.error(f"missing runtime input: {name}")

    run_root = work_root / ("fps-visible-" + uuid.uuid4().hex)
    run_root.mkdir(parents=True)
    print(f"FPS visible-frame evidence: {run_root}", flush=True)
    binary_dir = run_root / ("bin-\u03a9" if args.unicode_path else "bin")
    # Preserve runtime dependencies/assets, but avoid copying debug symbols or
    # launching unrelated game modules. Only this private copy is ever changed.
    shutil.copytree(package_bin, binary_dir, ignore=shutil.ignore_patterns("*.pdb", "*.ilk"))
    manifest = {"input_bin": str(package_bin), "sha256": {}}
    for name in ("SparkEngine.exe", "SparkGameFPS.dll"):
        digest = hashlib.sha256()
        with (binary_dir / name).open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        manifest["sha256"][name] = digest.hexdigest()
    (run_root / "binaries.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    checker = Path(__file__).with_name("CheckFPSVisibleFrame.ps1")
    powershell = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
    scene = binary_dir / "Assets/Scenes/level1.scene"
    original = scene.read_bytes()

    try:
        # RED first: keep Models (and therefore the chosen asset root) intact,
        # but remove its scene. Ancestor assets must not rescue this load.
        for case in ("fallback", "authored"):
            if case == "fallback":
                scene.rename(scene.with_suffix(".scene.disabled"))
            else:
                scene.write_bytes(original)
            case_root = run_root / case
            case_root.mkdir()
            timeline = case_root / "visual.exec"
            timeline.write_text("0 gfx_screenshot fps-visible.png\n", encoding="utf-8")
            env = os.environ.copy()
            env.update(SPARK_RHI_BACKEND="d3d11", SPARK_D3D11_DRIVER="warp",
                       LOCALAPPDATA=str(case_root / "localappdata"))
            command = [str(binary_dir / "SparkEngine.exe"), "-game", str(binary_dir / "SparkGameFPS.dll"),
                       "-require-game", "-test-frames", "8", "-threads", "2", "-window-size", "640x360",
                       "-no-subprocess", "-exec", str(timeline)]
            with (case_root / "stdout.log").open("wb") as stdout, (case_root / "stderr.log").open("wb") as stderr:
                result = subprocess.run(command, cwd=case_root, env=env, stdout=stdout, stderr=stderr, timeout=120)
            if result.returncode != 0:
                raise RuntimeError(f"{case}: engine exited {result.returncode}; logs: {case_root}")
            audit = case_root / "exec_audit.log"
            # Some unrelated legacy console messages still narrow wide paths.
            # Match PowerShell ReadAllText's replacement decoding; the identity
            # path itself must round-trip exactly through the real checker.
            log = audit.read_text(encoding="utf-8", errors="replace")
            if "| ok  | gfx_screenshot fps-visible.png" not in log:
                raise RuntimeError(f"{case}: engine did not execute the screenshot command")
            image = case_root / "fps-visible.png"
            with image.open("rb") as stream:
                if stream.read(8) != b"\x89PNG\r\n\x1a\n":
                    raise RuntimeError(f"{case}: engine did not produce a PNG")
            check = subprocess.run(
                [str(powershell), "-NoProfile", "-NonInteractive", "-File", str(checker),
                 "-ImagePath", str(image), "-LogPath", str(audit),
                 "-ExpectedSceneDirectory", str(scene.parent)], capture_output=True, timeout=30)
            (case_root / "check.stdout.log").write_bytes(check.stdout)
            (case_root / "check.stderr.log").write_bytes(check.stderr)
            if case == "fallback":
                if "FPS scene identity: procedural fallback arena" not in log:
                    raise RuntimeError("RED run never reached the procedural fallback")
                if check.returncode == 0 or b"FPS run used the procedural fallback arena" not in check.stderr:
                    raise RuntimeError(
                        f"RED check did not reject the fallback for its scene identity; logs: {case_root}")
                print("RED: real fallback frame rejected for procedural scene identity", flush=True)
            else:
                if check.returncode != 0:
                    raise RuntimeError(f"GREEN authored frame check failed; logs: {case_root}")
                print("GREEN: real authored frame passed scene identity and pixel checks", flush=True)
    finally:
        scene.write_bytes(original)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
