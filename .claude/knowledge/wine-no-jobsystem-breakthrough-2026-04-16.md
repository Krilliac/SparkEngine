# Wine + gVisor: -no-jobsystem breakthrough (80% RC=0 success rate)

**Last updated:** 2026-04-16
**Type:** Issue + Pattern
**Status:** Active

## TL;DR

Adding `-no-jobsystem` to the Wine engine flags eliminated the last
engine-controlled source of thread creation that was losing the gVisor
gs.base race. Combined with auto-enabling the LD_PRELOAD shim and
suppressing Wine's debugger hang, bare `tools/wine-run.sh
.../SparkEngine.exe -test-frames N` now achieves **4/5 (80%) RC=0**
end-to-end runs on gVisor + Wine 9.0 + Lavapipe.

## The problem

Even with `-threads 1 -no-subprocess -minimal-init`, the engine still
spawned one JobSystem worker thread via
`Spark::EngineSetup::InitializeJobSystem(1)` inside
`InitHeadlessEngineContext`. On gVisor, every new Windows thread rolls
the dice on Wine's gs.base race — if the thread faults before the shim's
`arch_prctl` interception or `/proc/self/maps` TEB discovery catches it,
Wine's SEH dispatcher sees an invalid frame (`call_stack_handlers
invalid frame`) or stack overflow, and the process dies.

With one worker thread: observed failure was a second TID (0030) faulting
with EXCEPTION_ACCESS_VIOLATION immediately after the main thread's
Timer construction, before InitHeadlessEngineContext completed. The shim
only got one repair cycle on the main thread; the worker lost the race.

## The fix

New `-no-jobsystem` flag (`g_noJobSystem` global) parsed from command
line in both Linux and Windows entry points. When set, skips
`Spark::EngineSetup::InitializeJobSystem()` entirely. Code paths that
use `JobSystem::Get().Dispatch(...)` fall back to inline execution on
the main thread because `JobSystem::IsInitialized()` returns false.

Files changed:
- `SparkEngine/Source/Core/SparkEngine.cpp` — `g_noJobSystem` global
- `SparkEngine/Source/Core/SparkEngineWindows.cpp` — parse flag + guard InitializeJobSystem
- `SparkEngine/Source/Core/SparkEngineLinux.cpp` — parse flag + guard InitializeJobSystem

## wine-run.sh improvements (same commit batch)

### Auto-flag `-no-jobsystem`
Added to the SparkEngine.exe auto-flag block alongside `-headless`,
`-threads 1`, `-no-subprocess`, `-minimal-init`. All five flags are
now appended automatically; opt out with `SPARK_WINE_NO_AUTO_FLAGS=1`.

### gVisor shim auto-activation
Changed from opt-in (`SPARK_WINE_GVISOR_SHIM=1`) to auto-detect: the
shim loads whenever `tools/gvisor-wine-shim.so` exists. Opt out with
`SPARK_WINE_GVISOR_SHIM=0`. The shim is strictly additive on hosts
where Wine's native SEH works.

### Crash debugger disable
Appends `AeDebug Auto=0` to the Wine prefix's `system.reg` so
unhandled faults call `ExitProcess` instead of spawning `winedbg` (which
itself loses the gs.base race and hangs forever on gVisor). Done via
direct text append so it works even in the stub-prefix path.

### Cleanup trap
`trap cleanup_wineserver EXIT INT TERM` kills the wineserver on script
exit so orphan processes don't hold the prefix lock after a hung run.

## Verified results

### 5-run reliability sweep (gVisor / Wine 9.0 / Lavapipe)

```
Run 1: RC=0  lines=72  full shutdown ✓  Timer destructor ✓
Run 2: RC=0  lines=64  full shutdown ✓  Timer destructor ✓
Run 3: RC=1  lines=17  early fault (shim lost race on Wine-internal thread)
Run 4: RC=0  lines=63  full shutdown ✓  Timer destructor ✓
Run 5: RC=0  lines=68  full shutdown ✓  Timer destructor ✓
```

**4/5 = 80% success rate.** Previous session baseline (no -no-jobsystem):
0% — every run crashed at the worker thread before reaching main loop.

### Successful RC=0 run trace

```
gvisor-shim installs SIGSEGV trampoline + wrgsbase fallback
Running under Wine 9.0 (ntdll.dll::wine_get_version)
Timer constructed
-no-jobsystem: JobSystem worker threads skipped
SaveSystem::Initialize
SimpleConsole initialized
InitConsole: EngineStartEvent published
RunHeadlessWindows: main loop runs 10-20 frames
Shutting down all plugins
FoliageRenderer → ClipmapTerrain → TransientResourcePool → ...
Timer destructor called
RC=0
```

## Remaining 20% failure mode

Wine-internal threads that the LD_PRELOAD shim can't control:
- Wine timer thread (spawned by ntdll during PE loader init)
- Wine service threads (part of early DLL init)
- These threads fault before the shim's `sigaction` trampoline is
  installed for them, so the gs.base race is unprotected.

This cannot be fixed without either:
1. Upstream Wine fix (Wine PRs #61/#63 merged into stock Wine)
2. Upstream gVisor fix (proper arch_prctl ARCH_SET_GS emulation)
3. Further shim evolution (intercept `clone` / `clone3` to pre-set
   gs.base on new threads before they execute any code)

## Recipe

```bash
# Bare invocation — all flags auto-appended, shim auto-loaded:
tools/wine-run.sh build/linux-mingw-release/bin/SparkEngine.exe -test-frames 20

# Equivalent explicit invocation:
SPARK_WINE_GVISOR_SHIM=1 tools/wine-run.sh \
  build/linux-mingw-release/bin/SparkEngine.exe \
  -test-frames 20 -threads 1 -no-subprocess -minimal-init -no-jobsystem \
  -window-size 800x600
```

## Cross-references

- `knowledge/wine-user-space-hacks-2026-04-15.md` — the shim design
- `knowledge/wine-gvisor-root-cause-found-2026-04-14.md` — gs.base race root cause
- `knowledge/live-editor-testing.md` — Linux native boot (Vulkan + OpenGL paths)
- `tools/wine-run.sh` — the script with all auto-flags
- `tools/gvisor-wine-shim.c` — the LD_PRELOAD shim source
