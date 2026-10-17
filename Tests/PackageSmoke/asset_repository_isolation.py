#!/usr/bin/env python3
"""Run a copied installed Linux FPS package with the checkout absent from its mount namespace.

Requires bubblewrap and strace. Missing tools or namespace support fail the test.
Only system runtime directories, the read-only package, and isolated user data
are mounted. This is NullRHI asset isolation evidence, not Windows/D3D11 proof.
"""
from __future__ import annotations

import argparse
import ast
from pathlib import Path, PurePosixPath
import posixpath
import re
import shutil
import subprocess
import sys
import tempfile


SYSTEM_ROOTS = ("/usr", "/bin", "/lib", "/lib64", "/etc")
QUOTED = re.compile(r'"(?:[^"\\]|\\.)*"')
ASSET_SUFFIXES = {".obj", ".mtl", ".scene", ".sparkscene", ".png", ".dds", ".wav", ".ogg", ".glb", ".gltf"}


def within(path: str, root: str) -> bool:
    return path == root or path.startswith(root.rstrip("/") + "/")


def audit_trace(lines: list[str], forbidden: list[str]) -> list[str]:
    """Reject attempted source/build access, including failed opens and relative asset probes.

    strace -yy annotates directory descriptors. A relative asset call without
    an annotated base fails closed rather than guessing its process cwd.
    """
    errors = []
    asset_opens = 0
    for line in lines:
        if not re.search(r"\b[a-z_0-9]+\(", line):
            continue
        resolved = re.search(r"= [0-9]+<(/[^>]*)>", line)
        if resolved is not None:
            target = posixpath.normpath(resolved.group(1))
            if any(within(target, root) for root in forbidden):
                errors.append("resolved descriptor reaches repository/build: " + line.strip())
            target_is_asset = ("assets" in target.lower().split("/") or
                               PurePosixPath(target).suffix.lower() in ASSET_SUFFIXES)
            if target_is_asset and not within(target, "/package/bin/Assets"):
                errors.append("resolved asset descriptor outside installed Assets: " + line.strip())
        for match in QUOTED.finditer(line):
            try:
                value = ast.literal_eval(match.group())
            except (ValueError, SyntaxError):
                errors.append("unparseable trace path: " + line.strip())
                continue
            if not isinstance(value, str):
                continue
            is_asset = "assets" in value.lower().split("/") or PurePosixPath(value).suffix.lower() in ASSET_SUFFIXES
            if not value.startswith("/"):
                if not is_asset:
                    continue
                descriptor = re.search(r"(?:AT_FDCWD|-?\d+)<([^>]+)>\s*,\s*$", line[:match.start()])
                if descriptor is None:
                    errors.append("unresolved relative asset lookup: " + line.strip())
                    continue
                value = posixpath.join(descriptor.group(1), value)
            value = posixpath.normpath(value)
            if any(within(value, root) for root in forbidden):
                errors.append("repository/build lookup: " + line.strip())
            elif is_asset and not within(value, "/package/bin/Assets"):
                errors.append("asset lookup outside installed Assets: " + line.strip())
            elif is_asset and re.search(r"\bopen(?:at|at2)?\(", line) and resolved is not None:
                if within(posixpath.normpath(resolved.group(1)), "/package/bin/Assets"):
                    asset_opens += 1
    if asset_opens == 0:
        errors.append("trace contains no successful installed asset open")
    return errors


def sandbox_command(package: Path, output: Path, forbidden: list[str]) -> list[str]:
    for root in forbidden:
        if not root.startswith("/") or any(within(root, system) or within(system, root) for system in SYSTEM_ROOTS):
            raise ValueError("source/build root overlaps the system runtime mounts: " + root)
    bwrap, strace = shutil.which("bwrap"), shutil.which("strace")
    if not bwrap or not strace:
        raise RuntimeError("installed asset isolation requires bubblewrap and strace")
    if not within(strace, "/usr") and not within(strace, "/bin"):
        raise RuntimeError("strace must be installed in a mounted system directory")
    command = [bwrap, "--die-with-parent", "--unshare-all", "--new-session", "--clearenv"]
    for root in SYSTEM_ROOTS:
        if Path(root).exists():
            command += ["--ro-bind", root, root]
    command += ["--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp",
                "--ro-bind", str(package), "/package", "--bind", str(output), "/result",
                "--chdir", "/result", "--setenv", "PATH", "/usr/bin:/bin",
                "--setenv", "HOME", "/result/home", "--setenv", "XDG_DATA_HOME", "/result/home/data",
                "--setenv", "XDG_CONFIG_HOME", "/result/home/config",
                "--setenv", "XDG_CACHE_HOME", "/result/home/cache",
                "--setenv", "XDG_STATE_HOME", "/result/home/state",
                "--setenv", "LD_LIBRARY_PATH", "/package/bin:/package/lib",
                "--setenv", "SPARK_RHI_BACKEND", "null"]
    # Verify the actual source/build directories are unreachable before the
    # trace begins. The namespace never renames or changes the host checkout.
    probe = 'for root do if test -e "$root"; then echo "source still reachable: $root" >&2; exit 91; fi; done; '
    probe += 'exec "$SPARK_TRACE" -f -qq -yy -s 8192 -e trace=file -o /result/files.trace '
    probe += '/package/bin/SparkEngine -headless -game /package/bin/libSparkGameFPS.so '
    probe += '-require-game -test-frames 8 -threads 2 -no-subprocess'
    command += ["--setenv", "SPARK_TRACE", strace, "/bin/sh", "-c", probe, "asset-isolation", *forbidden]
    return command


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--test-root", type=Path, required=True)
    parser.add_argument("--config", required=True)
    args = parser.parse_args()
    if sys.platform != "linux":
        raise RuntimeError("repository-unreachable package execution requires Linux")
    args.test_root.mkdir(parents=True, exist_ok=True)
    forbidden = [str(args.source_root.resolve()), str(args.build_dir.resolve())]
    # Keep evidence per invocation; cleanup of the large installed copy is
    # automatic even if an engine run or audit fails.
    with tempfile.TemporaryDirectory(prefix="asset-isolation-", dir=args.test_root) as temporary:
        run = Path(temporary)
        installed, package, output = run / "installed", run / "copy", run / "output"
        output.mkdir()
        (output / "home").mkdir()
        for component in ("runtime", "samples"):
            subprocess.run(["cmake", "--install", str(args.build_dir), "--config", args.config,
                            "--prefix", str(installed), "--component", component], check=True, timeout=180)
        shutil.copytree(installed, package, symlinks=True)
        command = sandbox_command(package, output, forbidden)
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        (args.test_root / "asset-isolation.stdout.log").write_text(result.stdout, encoding="utf-8")
        (args.test_root / "asset-isolation.stderr.log").write_text(result.stderr, encoding="utf-8")
        trace = output / "files.trace"
        errors = []
        if trace.is_file():
            shutil.copyfile(trace, args.test_root / "asset-isolation.trace")
            errors += audit_trace(trace.read_text(encoding="utf-8").splitlines(), forbidden)
        else:
            errors.append("strace did not produce a file-access trace")
        if result.returncode != 0:
            errors.append(f"isolated runtime exited {result.returncode}: {result.stderr}")
        lifecycle = re.findall(
            r"^SPARK_MODULE_LIFECYCLE module=SparkGameFPS create=1 load=1 update=[1-9][0-9]* "
            r"fixed=[1-9][0-9]* render=0 unload=1 destroy=[01] faults=0$", result.stdout, re.MULTILINE)
        if len(lifecycle) != 1 or "SPARK_MODULE_READY count=1" not in result.stdout:
            errors.append("missing unique clean FPS lifecycle and ready records")
        if errors:
            raise RuntimeError("\n".join(errors))
    print("installed FPS asset isolation passed (Linux NullRHI; repository unreachable)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"asset isolation failed: {error}", file=sys.stderr)
        sys.exit(1)
