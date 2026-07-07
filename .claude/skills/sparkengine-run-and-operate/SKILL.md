---
name: sparkengine-run-and-operate
description: >-
  Runbook for launching and operating the built SparkEngine binaries — the engine
  (SparkEngine.exe), the ImGui editor (SparkEditor.exe), the standalone SparkConsole
  subprocess, and the 10 game-module DLLs — plus where logs, crash dumps, saves and
  screenshots land. TRIGGER when: "how do I run/launch/start the engine or editor",
  "which executable do I run", "what are the command-line flags", "run headless /
  dedicated server", "load a specific game module with -game", "the console subprocess
  isn't receiving my commands", "where do crash dumps / SparkCrash .dmp / logs / save
  files go", "how does the SparkConsole IPC pipe work". DO NOT TRIGGER when: you need to
  compile/configure/build (use global cmake-msvc / ci-troubleshoot), diagnose a compiler
  or CMake error, write engine C++ code, or debug rendering/physics internals — this skill
  is about operating already-built binaries, not producing them.
trilobite_compatible: true
trilobite_role: operations
---

# SparkEngine: Run and Operate

Operate the already-built SparkEngine binaries. This skill assumes a successful build
exists. If you have not built yet, build first (`cmake --build build --config Release`)
— that is a separate concern; this runbook does not cover compile/link errors.

Term key (defined once):
- **RHI** — Rendering Hardware Interface, the engine's API-agnostic render backend layer.
- **NullRHIDevice** — the GPU-less fallback backend; auto-activates headless when no GPU.
- **Game module** — a gameplay DLL (e.g. `SparkGameFPS.dll`) loaded at runtime by the engine.
- **SparkConsole subprocess** — a *separate* console UI process the engine spawns and talks
  to over a stdin/stdout pipe. Distinct from `SimpleConsole`, the in-process log buffer.

## 1. Where the binaries land

CMake sets `CMAKE_RUNTIME_OUTPUT_DIRECTORY` to `${CMAKE_BINARY_DIR}/bin`
(`CMakeLists.txt:171`). So for the standard `build/` tree, **every executable and every
game-module DLL lands in `build/bin/`** (single-config generators) — the SparkConsole
search paths and module directory-scan both assume co-location there.

| Target | Binary (Windows) | Source of truth | Notes |
|--------|------------------|-----------------|-------|
| Engine | `SparkEngine.exe` | `CMakeLists.txt:1139` (`WIN32` GUI subsystem) | The thing you usually run. |
| Editor | `SparkEditor.exe` | `SparkEditor/CMakeLists.txt:40` (`WIN32`) | ImGui editor; does NOT spawn SparkConsole. |
| Console UI | `SparkConsole.exe` | `SparkConsole/CMakeLists.txt:33` | Spawned by the engine; can also run standalone. |
| Launcher | `SparkLauncher.exe` | `SparkLauncher/CMakeLists.txt:28` | Optional front-end launcher. |
| Daemon | `SparkDaemon.exe` | `SparkDaemon/CMakeLists.txt:38` | Shared shader-cache service (opt-in). |
| Crash reporter | `SparkCrashReporter.exe` | `SparkCrashReporter/CMakeLists.txt:26` | Watchdog launched on crash if present. |
| Shader compiler | `SparkShaderCompiler.exe` | `SparkShaderCompiler/CMakeLists.txt:57` | Offline tool. |
| Game modules (10) | `SparkGame*.dll` | e.g. `add_library(SparkGameFPS SHARED …)` `GameModules/SparkGameFPS/CMakeLists.txt:93` | DLLs, auto-discovered. |

On Linux/macOS drop the `.exe`; game modules become `.so`/`.dylib`. On multi-config
generators (Visual Studio/Ninja Multi-Config) binaries land under
`build/bin/<Config>/` — but this repo pins the same `bin` dir for all configs
(`CMakeLists.txt:178`), so they stay in `build/bin`.

## 2. Launching — startup order and combinations

**Run the engine from `build/bin` (so relative asset/console/module paths resolve):**

```powershell
cd build/bin
./SparkEngine.exe
```

The engine is a self-contained process. On startup it (Windows path, `SparkEngineWindows.cpp`):
1. Parses flags from the command line (`WinMain`, ~`:1157`–`:1163`).
2. Creates the window + graphics/input/timer (`InitInstance`, `:1217`).
3. `InitializeWindowedSubsystems` — sets up EngineContext, then `InitConsole()`
   (which may spawn `SparkConsole.exe`), then loads game modules.
4. Enters `RunWindowedMainLoop` (`:1192`) which ticks every frame.

You do **not** launch SparkConsole yourself for the normal case — the engine spawns it.
The editor (`SparkEditor.exe`) is an independent process and does **not** use the
SparkConsole subprocess at all (verified: no `ConsoleProcessManager` references under
`SparkEditor/Source/`).

### Choosing a game module

Module load priority (`LoadGameModules`, `SparkEngineWindows.cpp:446`):
1. `-game <path>` on the command line → loads that single module DLL.
2. `spark.modules.json` manifest next to the engine exe → loads listed modules.
3. Otherwise: directory scan of the exe dir for `*Game*.dll` / `*Module*.dll`.

```powershell
# Run the FPS module explicitly
./SparkEngine.exe -game SparkGameFPS.dll
```

### Common launch flags (verified in source)

| Flag | Effect | Where parsed |
|------|--------|--------------|
| `-game <path>` | Load one specific module DLL | `SparkEngineWindows.cpp:423`, `SparkEngineLinux.cpp:180` |
| `-headless` / `-dedicated` | Run without a window (NullRHIDevice / software), dedicated-server style | `SparkEngineWindows.cpp:246,251`; `SparkEngineLinux.cpp:1220` |
| `-no-subprocess` | Skip spawning `SparkConsole.exe`; keep in-process `SimpleConsole` | `SparkEngine.cpp:345`, parsed `SparkEngineWindows.cpp:1158` |
| `-minimal-init` | Skip non-essential init (debug/gameplay systems, daemon) to reach the main loop fast | `SparkEngine.cpp:347`, `:1160` |
| `-threads N` | Cap JobSystem worker threads (`-threads 1` for flaky sandboxes) | `SparkEngineWindows.cpp:126`, `SparkEngineLinux.cpp:139` |
| `-test-frames N` | Auto-exit after N frames (smoke testing) | `SparkEngine.cpp:323`, `SparkEngineWindows.cpp:102` |
| `-test-seconds S` | Auto-exit after S seconds | `SparkEngineWindows.cpp:1142` |
| `-window-size WxH` | Override window dimensions | `SparkEngineLinux.cpp:149` |
| `-no-jobsystem` | Disable the JobSystem thread pool | `SparkEngineWindows.cpp:1161` |

Headless run for CI/servers:

```powershell
./SparkEngine.exe -headless -no-subprocess -test-frames 300
```

## 3. The SparkConsole IPC wiring (concrete)

CLAUDE.md states the rule; here is the actual mechanism, so you can reason about it.

**Roles**
- `ConsoleProcessManager` (singleton, `SparkEngine/Source/Utils/ConsoleProcessManager.{h,cpp}`)
  launches `SparkConsole.exe` and owns the stdin/stdout pipe.
- `SimpleConsole` is only the engine-side in-memory log sink — it does NOT own the pipe.
- Platform process/pipe details are delegated to `Spark::Process`
  (`ConsoleProcessManagerWin32.cpp` is intentionally empty — `:6`).

**Initialize — one call site.** `InitConsole()` calls
`ConsoleProcessManager::GetInstance().Initialize()` at `SparkEngine.cpp:174`, guarded by
`if (!g_noSubprocess)` (`:171`). `InitConsole()` runs during subsystem init, before module
loading, so modules can register console commands.

`Initialize()` (`ConsoleProcessManager.cpp:164`):
- Resolves the console binary by trying, in order (`:198`–`:205`):
  `SparkConsole.exe`, `<exeDir>/SparkConsole.exe`, `<exeDir>/../SparkConsole/SparkConsole.exe`,
  `bin/Debug/SparkConsole.exe`, `bin/Release/SparkConsole.exe`, `./SparkConsole.exe`.
- If **none exist**, it returns `true` anyway with `m_consoleRunning == false` and prints
  `[ConsoleProcessManager] SparkConsole not found. Using fallback logging.` (`:220`). This
  is the silent-degrade path — the engine keeps running with no console UI.
- On success it launches the child with piped stdin+stdout
  (`Process::Builder(path).CaptureStdin().CaptureStdout().Launch()`, `:299`), sets
  `m_consoleRunning = true`, and starts a background reader thread `ConsoleThreadMain`.

**Per-frame pump — main-loop call sites (all four verified):**

| Platform / mode | Call site |
|-----------------|-----------|
| Windows windowed loop | `SparkEngineWindows.cpp:676` |
| Windows headless loop | `SparkEngineWindows.cpp:1058` |
| Linux (two loops) | `SparkEngineLinux.cpp:269`, `:532` |

Each frame the loop runs, inside a `SPARK_GUARDED_UPDATE("Console", …)` block:

```cpp
Spark::ConsoleProcessManager::GetInstance().ProcessCommands();
console.Update();
```

**Protocol / data flow**
- Background thread `ConsoleThreadMain` (`:347`) loops: `ReadFromConsole()` pulls one
  line via `Process::TryReadLine` and pushes it onto `m_commandQueue` (`:315`–`:331`);
  `ProcessQueuedMessages()` drains `m_messageQueue` and writes each as `msg + "\n"` to the
  child's stdin (`WriteToConsole`, `:333`).
- `ProcessCommands()` (`:371`, main thread) returns immediately if `!m_consoleRunning`,
  else swaps out `m_commandQueue` under a mutex and runs each line through
  `m_commandRegistry->ExecuteCommand`; non-empty results are logged back to the child as
  `[RESULT] …`.
- Built-in commands registered in the constructor (`:58`–`:135`): `help`, `quit`,
  `assert_test`, `assert_mode <on|off>`, and (debug builds only) `crash_test`.
- Shutdown (`:241`): sets `m_shouldStopThread`, joins the thread, closes the child's stdin
  to signal EOF, waits 500 ms for exit, then kills if still running. Called from
  `ShutdownEngine` (`SparkEngine.cpp:272`, `:304`).

## 4. Data and artifact conventions

All of these are **relative to the process working directory** (so where you `cd` before
launch determines where they land). Run from `build/bin` to keep artifacts out of the
source tree.

| Artifact | Location / name | Source |
|----------|-----------------|--------|
| Crash minidump | `SparkCrash<timestamp>.dmp` (CWD) | prefix set `SparkEngine.cpp:401`; written `CrashHandler.cpp:394` |
| Crash log | `SparkCrash<timestamp>.log` | `CrashHandler.cpp:395` |
| Crash screenshot | `SparkCrash<timestamp>.png` | `CrashHandler.cpp:396` |
| Crash bundle | `SparkCrash<timestamp>.zip` | `CrashHandler.cpp:397` |
| Assert log | `SparkCrash<timestamp>_assert.log` | `CrashHandler.cpp:1240` |
| Save files | `Saves/<slot>.spark_save` | `SaveSystem::Initialize("Saves")` `SparkEngineWindows.cpp:583`, default `SaveSystem.h:350` |
| Screenshots | `Screenshots/` | `ScreenCapture` default dir `ScreenCapture.h:14` |
| Settings | `settings.ini` (`[CrashReporting]`, `[Graphics]`, `SavesDirectory`, …) | `EngineSettings::Load()` `SparkEngineWindows.cpp:1224` |

Crash reporting reads `[CrashReporting]` from `settings.ini` with env-var overrides:
`SPARK_GITHUB_REPO`, `SPARK_GITHUB_TOKEN`, `SPARK_CRASH_PROXY_URL`, `SPARK_CRASH_UPLOAD_URL`
(`SparkEngine.cpp:393`). If `SparkCrashReporter.exe` sits next to the engine exe, the
crash handler launches it in watchdog mode (`CrashHandler.cpp:224`).

**Known mess (housekeeping):** running from the repo root historically dumped ~1.4 GB of
stale `SparkCrash_*.dmp` plus loose `Logs*.log` / `ci_*.txt` into the repo root (all
untracked). See `D:\SparkEngine\HARDEN_FLEET_HANDOFF.md`. Prefer launching from
`build/bin`, and clear stale untracked dumps before they pile up.

## 5. Troubleshooting: "the console subprocess isn't receiving my commands"

Work down this list — it maps directly to the call sites above.

1. **Was the subprocess even spawned?** If you passed `-no-subprocess`, `Initialize()` is
   skipped entirely (`SparkEngine.cpp:171`) — there is no child to receive commands, only
   the in-process `SimpleConsole`. Remove the flag.
2. **Was `SparkConsole.exe` found?** If none of the search paths (§3) resolve,
   `Initialize()` degrades silently with `m_consoleRunning == false`. Look for
   `SparkConsole not found. Using fallback logging.` on stderr. **Fix:** run the engine
   from `build/bin` where `SparkConsole.exe` is co-located, or copy it next to the engine
   exe. With `m_consoleRunning == false`, `ProcessCommands()` early-returns at `:373` and
   nothing is dispatched.
3. **Did the child die?** `ConsoleThreadMain` sets `m_consoleRunning = false` if the child
   process is no longer running (`:358`). `IsConsoleRunning()` reflects this. A crashed or
   exited `SparkConsole.exe` silently disables command intake.
4. **Is the main loop actually pumping?** Commands are queued by the background thread but
   only *executed* by `ProcessCommands()` on the main thread (§3). If the engine is not
   ticking its main loop (stalled during init, wrong mode, or a `SPARK_GUARDED_UPDATE`
   "Console" guard tripped), the `m_commandQueue` fills but never drains. Confirm the
   process reached `RunWindowedMainLoop`/the headless loop.
5. **Is the command registered?** `ProcessCommands` runs each line through
   `m_commandRegistry->ExecuteCommand`; an unknown command yields whatever
   `ExecuteCommand` returns for misses. Built-ins are `help`, `quit`, `assert_test`,
   `assert_mode`, `crash_test` (debug). Game/subsystem commands only exist if the owning
   module/subsystem registered them (module load happens *after* `InitConsole`, so
   registration timing matters).
6. **Startup progress breadcrumbs.** `InitConsole` emits `SPARK_LOG_INFO` markers
   ("InitConsole: ConsoleProcessManager::Initialize", "InitConsole: complete", etc.,
   `SparkEngine.cpp:162`–`:210`) via the Logger's stderr sink — visible even when the
   console UI is absent. Use them to see exactly how far init got.

## When NOT to use this skill / siblings

- Build, CMake, preset, or compiler/link errors → global `cmake-msvc` / `ci-troubleshoot`, not this one.
- Writing or changing engine C++ (systems, RHI, ECS) → `sparkengine-architecture-contract`.
- Diagnosing a specific crash's root cause from a dump → `sparkengine-debugging-playbook`; this skill
  only tells you *where the dump lands* and *how to spawn the crash reporter*.

## Provenance and maintenance

Verified 2026-07-07 against the repo at `D:\SparkEngine` (branch `Working`).
Re-verify volatile facts with:

```powershell
# Executable/target names
rg -n "add_executable\(Spark|add_library\(SparkGame\w+ SHARED" CMakeLists.txt SparkEditor/CMakeLists.txt SparkConsole/CMakeLists.txt GameModules
# Runtime output dir
rg -n "CMAKE_RUNTIME_OUTPUT_DIRECTORY" CMakeLists.txt
# Initialize() call site (must stay in InitConsole, guarded by g_noSubprocess)
rg -n "ConsoleProcessManager::GetInstance\(\).Initialize" SparkEngine/Source/Core/SparkEngine.cpp
# Per-frame ProcessCommands() call sites (expect 4: 2 Windows, 2 Linux)
rg -n "ConsoleProcessManager::GetInstance\(\).ProcessCommands" SparkEngine/Source/Core
# Console binary search paths + early-return guard
rg -n "searchPaths|m_consoleRunning|SparkConsole not found" SparkEngine/Source/Utils/ConsoleProcessManager.cpp
# Crash/save/screenshot artifact names
rg -n "dumpPrefix|Initialize\(\"Saves\"\)|Initialize\(\"Screenshots\"\)" SparkEngine/Source
# Launch flags
rg -n "no-subprocess|-headless|-dedicated|-minimal-init|-test-frames|-game " SparkEngine/Source/Core
```

Lint this skill with the skill-linter under `~/.claude/skills/skill-linter/` (pass
`-Path .claude/skills/sparkengine-run-and-operate`).
