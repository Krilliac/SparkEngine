"""Bounded Windows standalone build when CMake's compiler probe cannot run.

Run inside an x64 VS developer shell for clang-cl (including UBSan), or use
--compiler g++ for MinGW. Sources come from the upstream CMake source list.
Each compiler invocation has a two-minute timeout; only one runs at a time.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core", type=Path, help="Already patched core (otherwise prepare a fresh copy)")
    parser.add_argument("--patch", type=Path, help="Override patch, e.g. for a negative control")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--compiler", default="clang-cl")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--generic", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    tests = Path(__file__).resolve().parent
    addons = tests.parents[1] / "angelscript-mirror/sdk/add_on"
    if args.core:
        core = args.core.resolve()
    else:
        core = output / "core"
        # Never remove an existing tree: use a new --output or explicit --core.
        shutil.copytree(addons.parent / "angelscript", core)
        patch = (args.patch or tests.parent / "angelscript-packed-bytecode.patch").resolve()
        env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(output))
        for flags in (["--check"], []):
            subprocess.run(["git", "apply", *flags, "--whitespace=error-all", str(patch)],
                           cwd=core, env=env, check=True, timeout=30)
    cmake = (core / "projects/cmake/CMakeLists.txt").read_text()
    block = re.search(r"set\(ANGELSCRIPT_SOURCE\s+(.*?)\)", cmake, re.S).group(1)
    sources = [(core / "projects/cmake" / name).resolve() for name in block.split()]
    core_count = len(sources)
    sources += [addons / f"{name}/{name}.cpp" for name in ("scriptstdstring", "scriptarray", "scriptbuilder")]
    sources += [tests / "stack_alignment.cpp"]
    msvc = "clang-cl" in args.compiler
    if args.sanitize and not msvc:
        parser.error("This Windows direct runner supports UBSan with clang-cl only")
    objects = []
    with (output / "build.log").open("w", encoding="utf-8") as log:
        def run(command):
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=120)
            log.flush()
            if result.returncode:
                raise RuntimeError(f"command failed ({result.returncode}): {command}; see {output / 'build.log'}")

        for index, source in enumerate(sources):
            obj = output / (source.stem + (".obj" if msvc else ".o"))
            objects.append(str(obj))
            if msvc:
                # LLVM's Windows UBSan runtime is built against the static CRT.
                command = [args.compiler, "/nologo", "/c", "/std:c++17", "/EHsc", "/MT" if args.sanitize else "/MD", "/Od", "/Z7",
                           f"/I{core / 'include'}", f"/I{addons}", f"/Fo{obj}", str(source)]
            else:
                command = [args.compiler, "-c", "-std=c++17", "-O0", "-g", "-pthread",
                           f"-I{core / 'include'}", f"-I{addons}", "-o", str(obj), str(source)]
            if args.generic:
                command += ["-DAS_MAX_PORTABILITY"]
            if args.sanitize and index >= core_count:
                command += ["-fsanitize=alignment", "-fno-sanitize-recover=alignment"]
            print(f"[{index + 1}/{len(sources)}] {source.name}", flush=True)
            run(command)
        if msvc and not args.generic:
            obj = output / "as_callfunc_x64_msvc_asm.obj"
            run(["ml64", "/nologo", "/c", f"/Fo{obj}", str(core / "source/as_callfunc_x64_msvc_asm.asm")])
            objects.append(str(obj))
        executable = output / "stack_alignment.exe"
        command = [args.compiler, *objects]
        command += [f"/Fe{executable}"] if msvc else ["-pthread", "-o", str(executable)]
        if args.sanitize:
            command += ["-fsanitize=alignment"]
        run(command)
    print(f"Built {executable}", flush=True)


if __name__ == "__main__":
    main()
