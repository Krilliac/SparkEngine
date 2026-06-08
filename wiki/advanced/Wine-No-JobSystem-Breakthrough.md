# Wine No-JobSystem Breakthrough

> **Audience:** Programmers
>
> **Thread Context:** This is fundamentally a thread-creation problem. Every new Windows thread under Wine + gVisor rolls the dice on Wine's gs.base race. The fix eliminates the last *engine-controlled* thread (the JobSystem worker) so the engine runs single-threaded and JobSystem dispatches fall back to inline execution on the main thread.
>
> **Platform/Backend Scope:** Linux + MinGW-cross PE binary, run under Wine 9.0 inside gVisor, rendering via Lavapipe (CPU Vulkan). This is fallback tier 1 in the [Wine Role and Fallback Tiers](Wine-Role-and-Fallback-Tiers.md) ladder.

## Overview

Running the MinGW-cross-compiled `SparkEngine.exe` under Wine inside gVisor was failing on essentially every run because new threads faulted on Wine's gs.base race before the LD_PRELOAD shim could repair them. Adding a `-no-jobsystem` flag eliminated the last engine-spawned thread, and combined with auto-enabling the gVisor shim and suppressing Wine's crash debugger, the bare `tools/wine-run.sh … SparkEngine.exe -test-frames N` invocation reached **4/5 (80%) RC=0** end-to-end runs. The previous baseline was 0%.

## The Problem

Even with `-threads 1 -no-subprocess -minimal-init`, the engine still spawned one JobSystem worker thread via `Spark::EngineSetup::InitializeJobSystem(1)` inside `InitHeadlessEngineContext`. On gVisor, every new Windows thread races Wine's gs.base setup — if the thread faults before the shim's `arch_prctl` interception or `/proc/self/maps` TEB discovery catches it, Wine's SEH dispatcher sees an invalid frame (`call_stack_handlers invalid frame`) or a stack overflow, and the process dies.

With one worker thread, the observed failure was a second TID faulting with `EXCEPTION_ACCESS_VIOLATION` immediately after the main thread's `Timer` construction, before `InitHeadlessEngineContext` completed. The shim only got one repair cycle on the main thread; the worker lost the race.

## The Fix

A new `-no-jobsystem` flag (`g_noJobSystem` global) parsed from the command line in both the Linux and Windows entry points. When set, it skips `InitializeJobSystem()` entirely. Code paths that call `JobSystem::Get().Dispatch(...)` fall back to inline main-thread execution because `JobSystem::IsInitialized()` returns false.

Files changed:

- `SparkEngine/Source/Core/SparkEngine.cpp` — `g_noJobSystem` global
- `SparkEngine/Source/Core/SparkEngineWindows.cpp` — parse flag + guard `InitializeJobSystem`
- `SparkEngine/Source/Core/SparkEngineLinux.cpp` — parse flag + guard `InitializeJobSystem`

(All three files confirmed to still reference the no-jobsystem flag as of 2026-06-08.)

## wine-run.sh Improvements (same change batch)

- **Auto-flag `-no-jobsystem`** — appended to the `SparkEngine.exe` auto-flag block alongside `-headless`, `-threads 1`, `-no-subprocess`, `-minimal-init`. All five are added automatically; opt out with `SPARK_WINE_NO_AUTO_FLAGS=1`.
- **gVisor shim auto-activation** — changed from opt-in (`SPARK_WINE_GVISOR_SHIM=1`) to auto-detect: the shim loads whenever `tools/gvisor-wine-shim.so` exists. Opt out with `SPARK_WINE_GVISOR_SHIM=0`. The shim is strictly additive where Wine's native SEH already works.
- **Crash-debugger disable** — appends `AeDebug Auto=0` to the Wine prefix's `system.reg` so unhandled faults call `ExitProcess` instead of spawning `winedbg` (which itself loses the gs.base race and hangs forever on gVisor).
- **Cleanup trap** — `trap cleanup_wineserver EXIT INT TERM` kills the wineserver on script exit so orphans don't hold the prefix lock.

## Verified Results

5-run reliability sweep (gVisor / Wine 9.0 / Lavapipe):

```
Run 1: RC=0  lines=72  full shutdown ✓  Timer destructor ✓
Run 2: RC=0  lines=64  full shutdown ✓  Timer destructor ✓
Run 3: RC=1  lines=17  early fault (shim lost race on Wine-internal thread)
Run 4: RC=0  lines=63  full shutdown ✓  Timer destructor ✓
Run 5: RC=0  lines=68  full shutdown ✓  Timer destructor ✓
```

**4/5 = 80% success rate.** Baseline without `-no-jobsystem`: 0% — every run crashed at the worker thread before the main loop.

## Remaining 20% Failure Mode

Wine-internal threads the LD_PRELOAD shim can't control:

- Wine timer thread (spawned by ntdll during PE-loader init)
- Wine service threads (part of early DLL init)

These fault before the shim's `sigaction` trampoline is installed for them, so the gs.base race is unprotected. Fully closing the gap needs one of:

1. Upstream Wine fix (wine-mirror/wine #61/#63 merged into stock Wine)
2. Upstream gVisor fix (proper `arch_prctl ARCH_SET_GS` emulation)
3. Further shim evolution — intercept `clone`/`clone3` to pre-set gs.base on new threads before they execute any code

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

## Source & Freshness

- **Original entry date:** 2026-04-16 (`wine-no-jobsystem-breakthrough-2026-04-16.md`, type: Issue + Pattern)
- **Verified against codebase 2026-06-08.**
- Status bullets:
  - **Still accurate.** The `no-jobsystem` flag is referenced in all three entry-point files (`SparkEngine.cpp`, `SparkEngineWindows.cpp`, `SparkEngineLinux.cpp`).
  - The 80% success figure and remaining Wine-internal-thread failure mode are environment-dependent (Wine 9.0 / gVisor / Lavapipe) and not re-benchmarked here.
  - `tools/wine-run.sh` and `tools/gvisor-wine-shim.c` remain the relevant script and shim.

## Related Pages

- [Wine Role and Fallback Tiers](Wine-Role-and-Fallback-Tiers.md) — the full fallback ladder this run sits in (tier 1)
- [GPU/CPU Separation Plan](GPU-CPU-Separation-Plan.md) — NullRHIDevice / software-render context
