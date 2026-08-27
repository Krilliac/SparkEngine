---
name: sparkengine-run-package-and-release
description: >-
  Run, package, and publish SparkEngine binaries. TRIGGER when the user says "launch the engine/editor",
  "run headless", "start a dedicated server", "which flags does SparkEngine.exe take", "where did the build
  output go", "package the game", "make a zip/installer", "cpack", "SparkInstaller", "SparkBuild", "nightly
  release", "publish a release", "download the artifacts", "smoke test the exe", or "the engine won't start /
  no game module found / project selector". DO NOT TRIGGER for compile or CMake-configure errors (use
  sparkengine-build-ci-and-dependencies), for merge/ship verdicts and readiness gates (use
  sparkengine-change-control-and-release-readiness), for module ABI/hot-reload internals (use
  sparkengine-modules-sdk-abi-and-hot-reload), or for .spk asset-archive corruption (use
  sparkengine-assets-import-and-package-integrity).
---

# SparkEngine — run, package, and release

> **Live-tree guard (reviewed 2026-08-24).** SparkEngine's `Working` line is
> changing daily. Re-read root `AGENTS.md`, `CLAUDE.md`, the relevant wiki
> pages, and current `git status`/`git log` before acting. Branch labels, counts,
> line numbers, open-gap/status claims, and binary inventories below are dated
> evidence, not current truth. Preserve unrelated dirty work. During read-only
> orientation, do not build, generate, format, update submodules, or run Git
> mutations. On Nathan's Windows setup, route an authorized MSVC build through
> `C:\Users\Nathan\.claude\scripts\build.ps1` so vcvars and `CC`/`CXX`
> pinning are applied.

Operator runbook: how to launch every SparkEngine binary, bound a run for smoke testing, produce
distributable packages, and read the release pipeline's evidence. All paths are repo-relative; all
facts below were verified against the working tree on 2026-08-23.

**Jargon, defined once:**

- **Headless** — engine runs its fixed 60 Hz server loop with no window, no graphics, no input
  (`SparkEngineWindowsHeadless.cpp` / `SparkEngineLinuxHeadless.cpp`).
- **Dedicated (server)** — same loop; `-dedicated` is parsed as an alias of `-headless`
  (`SparkEngineWindows.cpp`, `ParseHeadlessFlag`).
- **Game module** — a gameplay DLL/SO under `GameModules/` loaded by the engine exe at runtime.
- **Module manifest** — `spark.modules.json` placed next to the engine exe; lists which module(s) to load.
- **CPack** — CMake's packaging tool; produces the ZIP/TGZ/NSIS/WIX artifacts the release workflow publishes.
- **NullRHIDevice** — GPU-less rendering fallback; engine continues headless when no graphics backend exists.

## When NOT to use this skill (sibling routing)

| You are actually asking about | Go to |
|---|---|
| Compile errors, CMake configure, dependency/submodule setup, CI build-job failures | `sparkengine-build-ci-and-dependencies` |
| "Is this safe to merge/ship?" — readiness verdicts, gate policy | `sparkengine-change-control-and-release-readiness` |
| Module ABI, `IModule`, hot-reload seams, DLL export contracts | `sparkengine-modules-sdk-abi-and-hot-reload` |
| `.spk` archives, asset import, package (asset) integrity | `sparkengine-assets-import-and-package-integrity` |
| Crashes/hangs you need to root-cause (beyond launch triage) | `sparkengine-debugging-playbook` |

## The status ladder — say which rung you are on

Never report "released" when you mean "compiled". Use these terms exactly:

| Status | Meaning | Evidence command |
|---|---|---|
| **Built** | Binary exists in `build/<preset>/bin/` | `ls build/windows-release/bin/SparkEngine.exe` |
| **Smoke-tested** | A bounded run (`-test-frames`/`-test-seconds`) exited 0 *and* the log shows the run actually happened | run commands in "Smoke tests" below, then check exit code **and** log |
| **Workflow-enforced** | A blocking CI job would fail if it broke. `Tests/PackageSmoke` + CPack run only in `release.yml`, **not** in per-PR `build.yml` | `grep -n PackageSmoke .github/workflows/*.yml` |
| **Published** | Asset attached to a GitHub Release (rolling `nightly` or versioned `v*`) | `gh release view nightly --json assets` |
| **Release-ready** | A verdict, not a fact of this skill — route to `sparkengine-change-control-and-release-readiness` | — |

Remember: a killed process exits 0 on some paths and a run that never started prints nothing. Pair
every exit code with log evidence (`Logs/`, see Diagnostics).

## Binary inventory (verified in `build/windows-debug/bin/`, 2026-08-23)

| Binary | What it is | Built by target |
|---|---|---|
| `SparkEngine.exe` | The game runtime (windowed `WIN32` app on Windows; also the headless/dedicated server) | `SparkEngine` (root `CMakeLists.txt` ~line 1204) |
| `SparkEditor.exe` | ImGui editor (`ENABLE_EDITOR=ON`, default) | `SparkEditor` |
| `SparkConsole.exe` | External console subprocess the engine spawns via `ConsoleProcessManager` | `SparkConsole` |
| `SparkShaderCompiler.exe` | Shader compilation tool | `SparkShaderCompiler` |
| `SparkTests.exe` | Unit test runner (also run by CTest) | `SparkTests` |
| `SparkLauncher.exe` | Project picker (`ENABLE_LAUNCHER=ON`, default) | `SparkLauncher` |
| `SparkBuild.exe` | Terminal-UI CMake configurator (`ENABLE_SPARKBUILD=ON`, default) | `SparkBuild` |
| `SparkInstaller.exe` | Bootstrap installer/updater (`ENABLE_INSTALLER=ON`, default) | `SparkInstaller` |
| `SparkCrashReporter.exe` | Out-of-process crash reporter | `SparkCrashReporter` |
| `SparkDaemon` | Shared-state service (Asset/Shader/Collab/Build); target configured (`SparkDaemon/CMakeLists.txt`) but **no `SparkDaemon.exe` was present in the inspected build dirs** — treat its runtime story as `open`, verify locally before claiming it runs | `SparkDaemon` |

**Game modules: 11** (not 10 — the old ten-module claim predates `SparkGameMMOFPS`/"TERRAFRONT").
Verified both as source dirs and as built DLLs:
`SparkGame, SparkGameARPG, SparkGameFPS, SparkGameMMO, SparkGameMMOFPS, SparkGameOpenWorld,
SparkGamePlatformer, SparkGameRPG, SparkGameRTS, SparkGameRacing, SparkGameVisualScript`

```bash
ls -d GameModules/*/ | wc -l          # → 11
ls build/windows-debug/bin/SparkGame*.dll | wc -l   # → 11 (in an up-to-date build)
```

## Where builds land

`CMakePresets.json` sets `binaryDir = ${sourceDir}/build/${presetName}`, and the root CMakeLists sets
`CMAKE_RUNTIME_OUTPUT_DIRECTORY = ${CMAKE_BINARY_DIR}/bin`. Therefore:

| How you configured | Executables land in |
|---|---|
| `cmake --preset windows-release` | `build/windows-release/bin/` |
| `cmake --preset windows-debug` | `build/windows-debug/bin/` |
| `cmake --preset linux-gcc-release` | `build/linux-gcc-release/bin/` |
| Raw `cmake -B build` (no preset — what `release.yml` does in CI) | `build/bin/` |

Build with the matching build preset — `cmake --build build --config Release` against a preset
configure is **wrong** (that path has no cache):

```bash
cmake --build --preset windows-release        # or: cmake --build build/windows-release --config Release
```

Do not go further into configure/compile problems here — that is `sparkengine-build-ci-and-dependencies`.

## Launching

### Runtime flags (engine exe — all single-dash; Linux also accepts double-dash)

Parsed in `SparkEngine/Source/Core/SparkEngineWindows.cpp`, `SparkEngineWindowsInit.cpp`, and
`SparkEngineLinux.cpp`:

| Flag | Effect |
|---|---|
| `-headless` | No window/graphics/input; fixed 60 Hz server loop |
| `-dedicated` | Alias of `-headless` (same parse) |
| `-game <path.dll>` | Load exactly this game module; skips discovery and the project selector |
| `-exec <file>` | Scripted console playback — runs a `.cfg` console script (see `Tools/tf_*.cfg`) |
| `-scene <path>` | Render a reflected-scene JSON via WorldBasicRenderer when no game module loads |
| `-test-frames N` | Exit cleanly after N frames (smoke-test bound) |
| `-test-seconds N` | Exit after N wall-clock seconds (init time excluded) |
| `-threads N` | Worker-thread count; overrides `SPARK_MAX_WORKER_THREADS` env var |
| `-no-jobsystem` | Skip JobSystem worker threads |
| `-no-subprocess` | Don't spawn the `SparkConsole` external console subprocess |
| `-minimal-init` | Minimal engine init |
| `-window-size WxH` | Window size (parsed in the Linux/SDL2 entry) |

Environment: `SPARK_MAX_WORKER_THREADS` is the only env var the entry points read (verified by grep).

### Module selection order (bare launch no longer bulk-loads)

From `SparkEngineWindowsModules.cpp::LoadGameModules` — priority:

1. `-game <path>` on the command line → load that one module.
2. `spark.modules.json` next to the exe → `LoadModulesFromManifest`.
3. Bare launch → **discover but never bulk-load**: one candidate loads directly; several candidates
   arm an ImGui **project selector panel** (windowed) or print pick-one guidance (headless).
   `ModuleManager` hard-refuses a second Game-kind module.

**Decision rule:** anything scripted (CI, smoke, server) must pass `-game` or ship a manifest —
never rely on discovery, because 2+ candidate DLLs means nothing loads until a human clicks.

### Copy-paste launches (from repo root, `windows-release` preset)

```powershell
# Windowed, explicit module
.\build\windows-release\bin\SparkEngine.exe -game .\build\windows-release\bin\SparkGameFPS.dll

# Headless dedicated server, scripted console, bounded thread count
.\build\windows-release\bin\SparkEngine.exe -headless -no-subprocess `
    -game .\build\windows-release\bin\SparkGameMMOFPS.dll -exec .\Tools\Terrafront\server.cfg -threads 2

# Editor
.\build\windows-release\bin\SparkEditor.exe
```

The headless-server line is exactly what `Tools/Launch-Terrafront.ps1` does (it then polls UDP port
27020 to confirm the server actually came up — a good pattern: **prove the listener, don't trust the spawn**).

Linux equivalents use the same flags (`build/linux-gcc-release/bin/SparkEngine`). Windows exes can be
run on Linux via `tools/wine-run.sh <exe>` (CI does `tools/wine-run.sh build/bin/SparkTests.exe`).
Note: the directory is `tools/` (lowercase) in the git index; Windows displays it as `Tools`.

## Smoke tests

A smoke test = bounded run + exit code + log evidence. Minimal recipes:

```powershell
# 1. Headless engine boots, runs 60 frames, exits
.\build\windows-release\bin\SparkEngine.exe -headless -no-subprocess -test-frames 60 `
    -game .\build\windows-release\bin\SparkGame.dll
echo $LASTEXITCODE          # must be 0
# THEN check the newest file under Logs/ actually shows frames ran — an exe that
# died before logging also "exits", and a check that stops checking always looks green.
```

```powershell
# 2. Wall-clock bound (for gameplay that isn't frame-driven)
.\build\windows-release\bin\SparkEngine.exe -headless -no-subprocess -test-seconds 10 `
    -game .\build\windows-release\bin\SparkGameFPS.dll
```

Scripted smokes: `-exec Tools\tf_smoke_host.cfg` style console scripts (many `tf_*.cfg` recipes live
in `Tools/`).

**SDK package smoke** (`Tests/PackageSmoke/` — a standalone CMake project that consumes the
*installed* SDK via `find_package`, proving external consumers can link). The canonical
local reproduction recipe lives in `sparkengine-validation-and-qa` §7; it mirrors the
"Validate external package consumption" step in `.github/workflows/release.yml`.
It is **workflow-enforced only in the release pipeline** — per-PR `build.yml` does not run it, so a
green PR does not prove the installed package works.

## Packaging

### The shipped path: CMake install + CPack (this is what releases publish)

Defined in root `CMakeLists.txt` (~lines 2716-2790). Components: `runtime` (required), `sdk`,
`tools`, `templates`, `samples`. Generators: ZIP everywhere; +NSIS/WIX on Windows; +TGZ elsewhere.
Package name: `SparkEngine-<version>-<System>-<Processor>` (project version currently `1.0.0`,
cache var `SPARK_ENGINE_VERSION`).

```powershell
cmake --install build/windows-release --config Release
cpack --config build/windows-release/CPackConfig.cmake -C Release -B build/windows-release/packages
ls build/windows-release/packages   # → SparkEngine-1.0.0-Windows-AMD64.zip (+ .exe/.msi if NSIS/WiX installed)
```

### In-engine packagers — know their real status

There are **two** GamePackager singletons; do not conflate them:

| Class | Location | Wired? | Status |
|---|---|---|---|
| `Spark::Build::GamePackager` | `SparkEngine/Source/Engine/Build/GamePackager.h` | Lifecycle-wired: `Initialize()`/`Shutdown()` called in `Core/Lifecycle/GameplayLifecycleShared.cpp`; `Package()` exercised only by `Tests/TestGamePackager.cpp` — no editor/console production caller | **candidate** — API works under test; consolidation tracked as `ASSET-220` (details: `sparkengine-assets-import-and-package-integrity`) |
| `Spark::GamePackager` | `SparkEngine/Source/Core/GamePackager.cpp` | No caller outside its own translation unit found | **open** — consolidation/deletion candidate per `ASSET-220`; layout it emits: `<out>/Bin`, `<out>/Assets`, `<out>/Config`, `manifest.txt` |

Decision rule: for anything you must actually ship today, use CPack. Treat the in-engine packagers
as unproven for production until a wired caller exists.

### Project-level tooling

- `Tools/spark-cli/spark_cli.py package` — packages a *generated game project* (expects
  `spark.project.json` + a plain `build/` dir) into `dist/<name>-<platform>-<config>/{bin,Assets}`.
  It assumes `build/<Config>` layout, **not** the engine's `build/<preset>` layout — don't point it
  at the engine repo. Its `run` subcommand does not auto-launch yet (prints instructions).
- `SparkBuild` — interactive TUI that configures/builds the engine and can launch the built exe
  ("Run Engine" menu item). See `SparkBuild/README.md`.
- `SparkInstaller` — bootstrap installer (clone + build). Release CI builds it standalone by
  generating a combo CMakeLists over `SparkBuild` + `SparkInstaller` with
  `-DSPARKINSTALLER_ENABLE_GUI=OFF`; artifact names `SparkInstaller-{Windows-x64.exe,Linux-x64,macOS-arm64}`.

## Release workflow and artifact evidence

Workflow: `.github/workflows/release.yml`, display name **"Publish Builds"**.

| Fact | Value (verified 2026-08-23) |
|---|---|
| Triggers | push to `release/**` branches; tags `v*`; nightly cron `0 4 * * *` UTC; manual `workflow_dispatch` (inputs: `release_tag`, `build_configs` = both/release/debug) |
| Build jobs | `build-windows` (windows-2022, VS2022 `-T v143`, raw `cmake -B build`), `build-linux` (ubuntu-24.04, gcc-14), `build-installer` (3-OS matrix) |
| Gates inside the workflow | full build, `ctest`, **Tests/PackageSmoke against the installed SDK**, then `cmake --install` + `cpack` |
| Artifacts | `SparkEngine-{Windows,Linux}-{Debug,Release}-packages` (CPack output), `SparkInstaller-*` binaries; retention 7 days |
| Publishing | tag/dispatch-with-tag → versioned Release marked latest; otherwise rolling **`nightly`** release is replaced |
| Side effects | download-counter badges committed back to `Working` (fails on supersession by design; rerun regenerates), then dispatches `build.yml` on the new head |

Evidence commands (require `gh` auth):

```bash
gh run list --workflow "Publish Builds" --limit 5           # did the pipeline run, and did it pass?
gh run view <RUN_ID> --log-failed                            # why a publish failed
gh release view nightly --json assets --jq '.assets[].name'  # what is actually published
gh release view nightly --json assets --jq '[.assets[].download_count] | add'
```

**Decision rules:**
- Nightly missing/stale → check the 04:00 UTC cron run first, then the badge-commit "supersession"
  step (it intentionally exits 1 if `Working` advanced mid-run — rerun the workflow, don't patch it).
- "CI is green but the release failed" is expected sometimes: PackageSmoke + CPack only run here.
  Reproduce locally with the install/cpack/PackageSmoke recipes above.
- Whether a given commit *should* be tagged is a change-control question — route to
  `sparkengine-change-control-and-release-readiness`.

## Operator diagnostics

| Symptom | Cause | Action |
|---|---|---|
| Engine starts, "N module candidates found — project selector armed" (headless: pick-one guidance, nothing loads) | Bare launch with ≥2 module DLLs next to the exe | Pass `-game <dll>` or drop a `spark.modules.json` next to the exe |
| "no game modules" guidance | No module DLL next to the exe (e.g. `BUILD_GAME_MODULES=OFF`, or running from the wrong dir) | Launch from the `bin/` dir of the same preset that built the modules |
| Second gameplay module refuses to load | `ModuleManager` hard-refuses a second Game-kind module (the old bulk-load double-stepped physics) | Intentional; load exactly one Game module |
| No external console window appears | You passed `-no-subprocess`, or `SparkConsole.exe` isn't beside the engine exe | Check `ConsoleProcessManager` log lines; `SimpleConsole` still captures logs in-process |
| Blank window, no GPU errors | No GPU backend → `NullRHIDevice` headless fallback engaged | Expected on GPU-less hosts; use WARP/Lavapipe/llvmpipe for software rendering |
| Wrong/old binary behavior | You built one preset and launched another (`build/windows-debug` vs `build/windows-release`) | Check binary timestamp vs your build log; a stale binary still fails a RED proof |
| Crash | `CrashHandler` writes a full-memory minidump `<dumpPrefix><timestamp>.dmp` and a manifest dir under `%TEMP%/spark_crash_<pid>`; `SparkCrashReporter.exe` is the out-of-process reporter | Grab the `.dmp` + newest `Logs/` file, then switch to `sparkengine-debugging-playbook` |

**Logs:** the file logger writes `Logs/<prefix>_<timestamp>.log` relative to the working directory
(`Utils/Logger.h` default `directory = "Logs/"`) — so launch working-directory matters. A stray
`server.log` at repo root is redirected server output from a local session, not an engine-created path.

## Provenance and maintenance

Facts verified 2026-08-23 against the working tree of branch
`claude/whole-nine-yards-20260823` (uncommitted changes ahead of `0e1fe7e7`). Re-verify with:

```bash
ls -d GameModules/*/ | wc -l                                             # module count (11)
grep -n 'binaryDir' CMakePresets.json                                    # build/<preset> layout
grep -n 'RUNTIME_OUTPUT_DIRECTORY' CMakeLists.txt | head -2              # bin/ subdir
grep -oE '\-\-?[a-z][a-z-]+' SparkEngine/Source/Core/SparkEngineWindows.cpp | sort -u   # runtime flags
grep -n '"-game \|spark.modules.json\|DiscoverModuleCandidates' SparkEngine/Source/Core/SparkEngineWindowsModules.cpp
grep -n 'CPACK_GENERATOR\|CPACK_PACKAGE_FILE_NAME' CMakeLists.txt        # package generators/naming
grep -n 'cron\|PackageSmoke\|softprops' .github/workflows/release.yml    # release triggers + gates
grep -n 'PackageSmoke' .github/workflows/build.yml                       # (expect: no matches — release-only gate)
grep -rn 'Build::GamePackager::GetInstance' SparkEngine/Source SparkEditor/Source  # packager wiring status
grep -n 'SPARK_ENGINE_VERSION' CMakeLists.txt                            # version used in package names
```

Volatile items most likely to drift: module count (CMake auto-discovers `GameModules/*/CMakeLists.txt`),
preset names, release retention/trigger schedule, and the `open`/`candidate` status of the two
GamePackagers and SparkDaemon.
