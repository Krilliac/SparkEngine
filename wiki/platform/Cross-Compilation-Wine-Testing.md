# Cross-Compilation: Wine Testing (experimental)

> **Audience:** Programmers and AI agents | Mixed
>
> **Thread Context:** Build host and application main threads.
>
> **Platform/Backend Scope:** Linux/WSL -> Windows x64 D3D11 engine and editor.

## Scope

MinGW/Wine is experimental. Its advisory CI lane runs only on manual
`workflow_dispatch`. OD-30 preserves the CPU-rendering capability for GPU-less
servers and agents, covering both SparkEngine and SparkEditor. Current runtime
proof is pending: the last documented hosted run built successfully but failed
Wine tests. This remains outside stable-v1 support and does not certify native Windows or D3D12.

The acceptance path uses DXVK 2.5.3 and Mesa Lavapipe; WineD3D/llvmpipe remains
a fallback for development.

NullRHI rasterizes nothing. Native Linux fallback does not exercise Windows.
Neither proves Windows CPU rendering. DirectXMath is fetched and hash-verified by the toolchain;
do not install mutable `main` headers manually.

## Build, setup and execute

Install prerequisites only through signed APT if passwordless sudo is available.
The [canonical MinGW guide](../development/MinGW-Wine-Cross-Compilation.md)
contains prerequisites, exact commands, every Wine-only exclusion and its reason.

```bash
# From the Linux/WSL checkout root; initialize the recorded submodules first.
git submodule update --init --recursive
cmake --preset linux-mingw-release -DBUILD_TESTS=ON -DENABLE_EDITOR=ON
cmake --build build/linux-mingw-release --target SparkEngine SparkEditor --parallel 2
cmake --build build/linux-mingw-release --parallel 2

# Use a dedicated prefix. --dxvk-only verifies the archive and copies its x64 DLLs here.
export WINEPREFIX="$PWD/build/linux-mingw-release/.wineprefix-mingw"
xvfb-run -a bash tools/wine-run.sh --setup-only
bash tools/setup-mingw-wine.sh --dxvk-only

# Each command invokes xvfb-run + Wine + pinned DXVK + Lavapipe internally.
python3 .github/scripts/mingw-wine-smoke.py engine
python3 .github/scripts/mingw-wine-smoke.py editor
export SPARK_TEST_EXCLUDE="$(python3 .github/scripts/mingw-wine-smoke.py exclusions)"
python3 .github/scripts/mingw-wine-smoke.py tests
python3 .github/scripts/mingw-wine-smoke.py summary
```

The smoke runner disables the Wine wrapper's automatic headless flags and
requires real rendering, automation results and exit 0. It rejects rc=255,
which the older diagnostic suite sometimes accepted. The editor is driven by
its project/save/open-scene CLI hooks. Engine commands and two frame captures,
editor successful-presentation counters, CPU device logs and the test summary
are preserved in `build/mingw-wine-evidence/`.

## Known limitations

The named Wine-only exclusions cover native Git/WARP/D3DCompiler/stack walking,
Windows protected ACLs and file ownership, named-pipe disconnect timing,
profile-directory mapping, mapped-PE reload fixtures and the documented no-ALSA
FAudio crash. Native Windows tests remain unchanged; credential/replay logic
still runs under Wine. The remaining run must pass the passing-test floor (`MINIMUM_TESTS=7500`) with
`--warn-is-error`. See the canonical guide for the full per-test list and the
provisional classification of profile-path and hot-reload behavior.

## Source & Freshness

Updated 2026-10-01. Repository implementation is reviewable; local Wine and
exact-commit hosted execution remain unverified. Historical timing/test totals
are not current evidence.
