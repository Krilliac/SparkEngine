# Wine Role and Fallback Tiers

> **Audience:** Programmers
>
> **Thread Context:** N/A at this layer — this is an execution-environment and tooling decision. The thread-creation hazards specific to Wine + gVisor are covered in [Wine No-JobSystem Breakthrough](Wine-No-JobSystem-Breakthrough.md).
>
> **Platform/Backend Scope:** Linux developer hosts (and sandboxes) running the MinGW-cross PE binary under Wine, software-rendered via Lavapipe (CPU Vulkan) or llvmpipe (CPU OpenGL), with NullRHIDevice as the no-graphics floor.

## Overview

Wine in SparkEngine is a **live execution path**, not just a CI smoke test. The intent: a developer without a GPU — or inside a sandbox with no GPU passthrough — can cross-compile the engine on Linux with MinGW-w64, run it under Wine, and see real frame-by-frame rendering via Lavapipe or llvmpipe. The `build-linux-mingw-wine` CI job exists to *protect* this path, not define it. Decisions about Wine version, DXVK pairing, fork choice, and escape hatches should optimize for live developer experience first, CI second.

A future session asking "should we swap upstream Wine for ValveSoftware/wine?" or "should we drop the flaky Wine job?" should read this page first. Both answers are no, for the reasons below.

## Why Upstream Wine, Not ValveSoftware/wine

`ValveSoftware/wine` is the base of Proton, optimized for shipping games to players via Steam — not for headless/sandboxed/software-rendered developer use:

- Valve carries upstream `dlls/ntdll/unix/*` verbatim — that's exactly where the bugs we care about live. Same syscalls, same gVisor failure modes, no benefit.
- Valve's value-add is graphics/audio/input shims (fsync, DXVK pairing, Wayland fixes, anticheat shims). None help a CPU-rasterized headless binary.
- Proton is a moving target tied to Steam releases, not tagged upstream — CI reproducibility tanks if we rebase on it.
- Our carried patches (wine-mirror/wine #61, #62, #63) are written against tagged upstream Wine; carrying them against an unpredictably-rebasing fork multiplies maintenance cost.

The only legitimate reason to add real Proton would be a **separate** Steam Deck / Proton compatibility tier — a CI job that downloads a pinned Proton release and runs a `GameModules/SparkGame*` DLL through it on a real Linux host (not gVisor — Proton hates sandboxed syscalls). That's a future enhancement, not a base swap.

## The Fallback Ladder

The Wine + software-rendering stack has multiple legitimate paths. Each lower tier is slower but uses a different code path in Wine, DXVK, Mesa, and the kernel — so when one tier breaks, the next often still works. **Always preserve all rungs. Never delete a slow tier "because the fast one works."**

| Tier | Path | When used | Speed |
|------|------|-----------|-------|
| **0 — Native** | Real Windows + real GPU | Primary dev/ship target | ~1× |
| **1 — Wine + DXVK + Lavapipe** | MinGW PE → Wine → DXVK → Vulkan → Lavapipe (CPU) | Default Linux developer path; `tools/wine-run.sh` first choice | ~10–50× slower |
| **2 — Wine + WineD3D + llvmpipe** | MinGW PE → Wine → WineD3D → OpenGL → llvmpipe (CPU) | Tier 1 unavailable / Lavapipe broken / DXVK trips a gVisor syscall | ~100–500× slower |
| **3 — Wine + NullRHIDevice** | MinGW PE → Wine → NullRHI (no graphics calls) | Logic-only smoke; tier 2 also broken | ~wall-clock |
| **4 — Native Linux + NullRHIDevice** | GCC ELF → NullRHI (no Wine) | Wine itself broken in the environment | Fastest, but skips the PE/Win32 path |

The intended selection logic:

```
if DXVK present and Lavapipe importable and ! gvisor_known_bad:  use tier 1
elif WineD3D + llvmpipe importable:                              use tier 2
elif Wine itself runs:                                           use tier 3 (-rhi=null)
else:                                                            use tier 4 (skip Wine, native Linux)
```

## Escape Hatches by Layer

Each layer needs an opt-out so a developer can isolate which layer is failing without rebuilding the world. Several originally-"needed" hatches have since landed.

### Layer: Wine itself

| Hatch | Purpose | Status |
|-------|---------|--------|
| `WINEDEBUG=+seh,+virtual` | Verbose signal/memory trace | Built-in |
| `WINEPRELOADRESERVE=` | Disable Wine's address-space preloader | Built-in (no help under gVisor) |
| `SPARK_WINE_PROBE=…probe.exe` | Pre-flight a known-good PE so we fail fast | Landed |
| `SPARK_WINE_GVISOR_SHIM=1` | LD_PRELOAD the gs.base shim | Landed (now auto-detected) |
| `SPARK_SKIP_WINE=1` | Skip Wine, fall through to tier 4 | **Landed** (present in `tools/wine-run.sh`) |
| `SPARK_WINE_VERSION=…/wine64` | Pick a specific Wine binary | Still **not** present — script auto-selects |

### Layer: D3D11 → Vulkan/OpenGL translator

| Hatch | Purpose | Status |
|-------|---------|--------|
| `WINEDLLOVERRIDES=d3d11=n,b` | Force WineD3D over DXVK (tier 1→2) | Built-in |
| `DXVK_HUD=fps,memory` | Confirm DXVK is executing | Built-in |
| `SPARK_WINE_BACKEND={dxvk\|wined3d\|null}` | One var picks the tier | **Landed** (present in `tools/wine-run.sh`) |
| `DXVK_LOG_LEVEL=info` | DXVK self-diagnostics | Built-in |

### Layer: Vulkan / OpenGL ICD

| Hatch | Purpose | Status |
|-------|---------|--------|
| `VK_ICD_FILENAMES=…lvp_icd…json` | Force Lavapipe | Built-in |
| `LIBGL_ALWAYS_SOFTWARE=1` | Force llvmpipe (OpenGL) | Built-in |
| `MESA_LOADER_DRIVER_OVERRIDE=llvmpipe` | Same, targeted | Built-in |
| `GALLIUM_DRIVER=llvmpipe` | Same, lower-level | Built-in |
| `SPARK_FORCE_SOFTWARE_GFX=1` | Umbrella: sets all the above + WineD3D override | **Landed** (present in `tools/wine-run.sh`) |

### Layer: SparkEngine RHI

| Hatch | Purpose | Status |
|-------|---------|--------|
| `-rhi=null` | Force `NullRHIDevice` from start | Built-in |
| `-rhi=opengl` | OpenGL backend (bypass DXVK) | Built-in |
| `-headless` | Skip window creation | Built-in |
| `-test-frames N` | Render N frames then exit | Built-in |
| `SPARK_RHI_BACKEND=null\|opengl\|d3d11\|vulkan` | Env-var equivalent of `-rhi=` | **Landed** (present in `tools/wine-run.sh`) |

### Layer: SparkEngine itself

| Hatch | Purpose | Status |
|-------|---------|--------|
| `EngineContext::Get()->IsRunningUnderWine()` | Runtime branch for Wine quirks | Landed (`Utils/WineDetection.h`) |
| `Spark::LogWineEnvironmentIfApplicable()` | Startup banner `Running under Wine 9.0` | Landed |
| `IsRunningUnderGvisor()` | Detect gVisor to auto-pick a safer tier | **Landed** (`Utils/WineDetection.{h,cpp}`, also used by `Graphics/RHI/RHIFactory.cpp`) |

## Why the Ladder Matters More Than Fixing Tier 1

The instinct on a gVisor regression is "fix tier 1." Correct as the long-term goal (patches #61/#62/#63), but even when tier 1 is healthy, tiers 2–3 must remain working code paths:

- **Mesa drift** — Lavapipe correctness has regressed on edge cases (timeline semaphores, sparse bindings); tier 2 keeps the engine visible while you wait for the next Mesa release.
- **DXVK drift** — DXVK occasionally bumps its minimum Vulkan driver version; older distros fall out of tier 1 and tier 2 picks up the slack.
- **Sandbox proliferation** — gVisor is one example; Firecracker, runD, and future harness sandboxes each may break a different layer. A working ladder means "swap rungs," not "won't run."
- **Test-signal granularity** — when tier 1 fails and tier 2 succeeds, you know the bug is in DXVK or Lavapipe, not Wine or SparkEngine.

**Rule of thumb:** if tempted to delete a slower path because the faster one works, add it as an escape hatch instead.

## Source & Freshness

- **Original entry date:** 2026-04-14 (`wine-role-and-fallback-tiers-2026-04-14.md`, type: Decision + Observation)
- **Verified against codebase 2026-06-08.**
- Status bullets:
  - **Still holds.** The "upstream Wine, not Valve" rationale and the five-tier ladder are unchanged.
  - **Most "needed" action items have landed:** `SPARK_WINE_BACKEND`, `SPARK_FORCE_SOFTWARE_GFX`, `SPARK_SKIP_WINE`, and `SPARK_RHI_BACKEND` are all present in `tools/wine-run.sh`; `IsRunningUnderGvisor()` is implemented in `Utils/WineDetection.{h,cpp}` and consumed by `Graphics/RHI/RHIFactory.cpp`.
  - **One item still open:** `SPARK_WINE_VERSION` is not present in `tools/wine-run.sh` (script still auto-selects the Wine binary).

## Related Pages

- [Wine No-JobSystem Breakthrough](Wine-No-JobSystem-Breakthrough.md) — the tier-1-under-gVisor reliability fix
- [GPU/CPU Separation Plan](GPU-CPU-Separation-Plan.md) — RHI backend parity and NullRHIDevice
- [SparkBuild In-Tree](SparkBuild-In-Tree.md) — sibling developer tooling
