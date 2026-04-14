# Wine's Role in SparkEngine + Layered Fallback Tiers

**Last updated:** 2026-04-14
**Type:** Decision + Observation
**Status:** Active

## TL;DR — what Wine is *for*

Wine in SparkEngine is a **live execution path**, not a CI smoke test.
The intent is that a developer without a GPU — or inside a sandboxed
environment that has no GPU passthrough — can:

1. Cross-compile the engine on Linux with MinGW-w64
2. Run it under Wine
3. See real, frame-by-frame rendering output via Lavapipe (CPU Vulkan)
   or llvmpipe (CPU OpenGL)

This worked end-to-end before the gVisor regression documented in
`wine-gvisor-incompatibility.md`. The `build-linux-mingw-wine` CI job
exists to *protect* this path, not to define it. Decisions about Wine
version, DXVK pairing, fork choice, or escape hatches should optimize
for **live developer experience first, CI second**.

A future session that asks "should we swap upstream Wine for
ValveSoftware/wine?" or "should we drop the Wine job, it's flaky?"
should re-read this entry first. Both answers are no, and the reasons
are below.

## Why upstream Wine, not ValveSoftware/wine

`ValveSoftware/wine` is the base of Proton. It's optimized for
*shipping games to players via Steam*, not for headless / sandboxed /
software-rendered developer use. Specifically:

- **Valve carries upstream `dlls/ntdll/unix/*` verbatim.** That's where
  every bug we currently care about lives. Same syscalls, same gVisor
  failure modes, no benefit from the swap.
- **Valve's value-add is in graphics/audio/input shims** (fsync, DXVK
  pairing, Wayland fixes, anticheat shims, per-title hacks). None of
  those help a CPU-rasterized headless engine binary.
- **Proton is a moving target.** Releases follow Steam, not a tagged
  upstream. CI reproducibility tanks if we rebase on it.
- **Patches we carry today** (wine-mirror/wine#61, #62, #63) are written
  against tagged upstream Wine source. Carrying them forward against a
  fork that rebases unpredictably multiplies maintenance cost.

The only legitimate reason to add `ValveSoftware/wine` (or rather, real
Proton) would be a **separate** Steam Deck / Proton compatibility tier:
a CI job that downloads a pinned Proton release and runs one of the
`GameModules/SparkGame*` DLLs through it on a real Linux host (not
gVisor — Proton hates sandboxed syscalls). That's a future enhancement,
not a base swap.

## The fallback ladder

The Wine + software-rendering stack has **multiple legitimate paths**.
Each lower tier is slower but uses a different code path in Wine,
DXVK, Mesa, and the kernel — so when one tier breaks, the next tier
often still works. Always preserve all rungs. Never delete a slow tier
"because the fast one works."

| Tier | Path | When it's used | Speed |
|------|------|----------------|-------|
| **0 — Native** | Real Windows + real GPU | Primary dev/ship target | ~1× |
| **1 — Wine + DXVK + Lavapipe** | MinGW PE → Wine → DXVK → Vulkan → Lavapipe (CPU) | Default Linux developer path; what `tools/wine-run.sh` picks first | ~10–50× slower than tier 0 |
| **2 — Wine + WineD3D + llvmpipe** | MinGW PE → Wine → WineD3D → OpenGL → llvmpipe (CPU) | Tier 1 unavailable / Lavapipe broken / DXVK trips a gVisor syscall | ~100–500× slower |
| **3 — Wine + NullRHIDevice** | MinGW PE → Wine → NullRHI (no graphics calls) | Logic-only smoke; tier 2 also broken | Effectively wall-clock |
| **4 — Native Linux + NullRHIDevice** | GCC ELF → NullRHI (no Wine at all) | Wine itself is broken in the environment (e.g. current gVisor regression) | Fastest, but doesn't exercise the PE / Win32 path |

`tools/wine-run.sh` already auto-detects DXVK, VKD3D-Proton, and
Lavapipe. The fallback selection logic should be:

```
if DXVK present and Lavapipe importable and ! gvisor_known_bad:
    use tier 1
elif WineD3D + llvmpipe importable:
    use tier 2
elif Wine itself runs:
    use tier 3 (-rhi=null)
else:
    fall through to tier 4 (skip Wine, run native Linux build of tests)
```

The current script does some of this but does not yet cleanly express
the tier-3/tier-4 fallthrough. That is a worthwhile follow-up — see
*Escape hatches needed* below.

## Escape hatches by layer

Each layer in the stack needs an opt-out so a developer can isolate
which layer is failing without rebuilding the world. **Existing**
hatches are listed first; **needed** ones are flagged.

### Layer: Wine itself

| Hatch | Purpose | Status |
|-------|---------|--------|
| `WINEDEBUG=+seh,+virtual` | Verbose Wine signal/memory trace | Built-in |
| `WINEPRELOADRESERVE=` | Disable Wine's address-space preloader | Built-in (does not help under gVisor) |
| `SPARK_WINE_PROBE=/path/to/probe.exe` | Pre-flight a known-good PE binary so we fail fast instead of spinning in trap loop | Landed in `tools/wine-run.sh` |
| `SPARK_WINE_GVISOR_SHIM=1` | LD_PRELOAD `tools/gvisor-wine-shim.so` to patch `REG_TRAPNO` | Landed |
| `SPARK_WINE_VERSION=/path/to/wine64` | Pick a specific Wine binary (not the Ubuntu wrapper) | **Needed** — script currently auto-selects |
| `SPARK_SKIP_WINE=1` | Skip Wine entirely, fall through to tier 4 native Linux build | **Needed** |

### Layer: D3D11 → Vulkan/OpenGL translator

| Hatch | Purpose | Status |
|-------|---------|--------|
| `WINEDLLOVERRIDES=d3d11=n,b` | Force WineD3D over DXVK (tier 1 → tier 2 downgrade) | Built-in |
| `WINEDLLOVERRIDES=d3d11,dxgi=n` | Force native (use the engine's own d3d11 stub if any) | Built-in |
| `DXVK_HUD=fps,memory` | Diagnose whether DXVK is actually executing | Built-in |
| `SPARK_WINE_BACKEND={dxvk\|wined3d\|null}` | Single env var that picks the tier (1/2/3) at script level | **Needed** |
| `DXVK_LOG_LEVEL=info` | DXVK self-diagnostics | Built-in |

### Layer: Vulkan / OpenGL ICD

| Hatch | Purpose | Status |
|-------|---------|--------|
| `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json` | Force Lavapipe specifically (skip any GPU ICD that snuck in) | Built-in |
| `LIBGL_ALWAYS_SOFTWARE=1` | Force llvmpipe in the OpenGL path | Built-in |
| `MESA_LOADER_DRIVER_OVERRIDE=llvmpipe` | Same, more targeted | Built-in |
| `GALLIUM_DRIVER=llvmpipe` | Same, lower-level | Built-in |
| `SPARK_FORCE_SOFTWARE_GFX=1` | One env var that sets all three above + `WINEDLLOVERRIDES` for WineD3D | **Needed** |

### Layer: SparkEngine RHI

| Hatch | Purpose | Status |
|-------|---------|--------|
| `-rhi=null` command-line arg | Force `NullRHIDevice` from the start (skip D3D11 init entirely) | Built-in |
| `-rhi=opengl` | Use OpenGL backend (so DXVK is bypassed even on tier 1) | Built-in |
| `-headless` | Skip window creation entirely | Built-in |
| `-test-frames N` | Render N frames then exit cleanly | Built-in |
| `SPARK_RHI_BACKEND=null\|opengl\|d3d11\|vulkan` | Env-var equivalent of `-rhi=` for cases where the launcher controls argv | **Needed** |

### Layer: SparkEngine itself

| Hatch | Purpose | Status |
|-------|---------|--------|
| `EngineContext::Get()->IsRunningUnderWine()` | Runtime branch for Wine-specific quirks | Landed (`Utils/WineDetection.h`) |
| `Spark::LogWineEnvironmentIfApplicable()` | Banner: `Running under Wine 9.0` | Landed |
| `IsRunningUnderGvisor()` runtime check | Detect gVisor at startup so we can pick a safer tier automatically | **Needed** — could probe `/proc/1/comm == "runsc"` or look for the `runsc` binary in `/proc/self/maps` |

## Why the fallback ladder matters more than fixing tier 1

The instinct on the gVisor regression is "fix tier 1 so everything goes
back to normal." That's correct as the long-term goal — patches #61,
#62, #63 do exactly that.

But the deeper lesson is: **even when tier 1 is healthy, tier 2 and
tier 3 must remain working code paths.** Reasons:

- **Mesa version drift.** Lavapipe correctness has regressed twice in
  the last two years on edge cases (timeline semaphores, sparse
  bindings). When that happens, tier 2 (llvmpipe) keeps the engine
  visible while you wait for the next Mesa point release.
- **DXVK version drift.** DXVK occasionally bumps its minimum Vulkan
  driver version. Older distros stop being valid tier-1 hosts; tier 2
  picks up the slack.
- **Sandbox proliferation.** gVisor is one example. There will be
  others (Firecracker, runD, future Claude harness sandboxes). Each
  may break a different layer. A working ladder means "swap to the
  next rung" instead of "the engine doesn't run on this machine."
- **Test signal granularity.** When tier 1 fails and tier 2 succeeds,
  you immediately know the bug is in DXVK or Lavapipe, not in Wine or
  SparkEngine. Throwing away tier 2 throws away that signal.

**Rule of thumb:** if a session is tempted to delete a "slower" path
because the faster one works, stop and add it as an escape hatch
instead.

## Cross-references

- `knowledge/mingw-wine-cross-compilation.md` — the original pattern
  entry; documents the build presets, perf table, and software-render
  fallback matrix
- `knowledge/wine-gvisor-incompatibility.md` — the gVisor regression
  itself: trap_no fix, virtual_setup_exception fix, LD_PRELOAD shim,
  `wine-run.sh` rewrite
- `knowledge/wine-gvisor-root-cause-found-2026-04-14.md` — root-cause
  walkthrough and upstream PR status (wine-mirror/wine#61, #62, #63)
- `knowledge/wine-patched-build-results-2026-04-14.md` — empirical
  results from running the patched Wine build
- `knowledge/live-editor-testing.md` — adjacent pattern for the editor
  side (Xvfb + Mesa llvmpipe)
- `docs/wine-upstream/README.md` — the upstream patch tracking notes
- `tools/wine-run.sh` — current dispatcher (DXVK/VKD3D/Lavapipe
  auto-detect, gVisor shim opt-in)
- `tools/gvisor-wine-shim.c` — the LD_PRELOAD trampoline for tier 1
  under gVisor
- `Utils/WineDetection.h|.cpp` — runtime Wine detection used by the
  engine startup banner

## Action items captured from this session

1. Add `SPARK_WINE_BACKEND={dxvk|wined3d|null}` to `tools/wine-run.sh`
   so a developer can force tier 1/2/3 with one variable.
2. Add `SPARK_FORCE_SOFTWARE_GFX=1` umbrella variable that sets
   `LIBGL_ALWAYS_SOFTWARE`, `GALLIUM_DRIVER=llvmpipe`,
   `VK_ICD_FILENAMES=lvp_icd…`, and the WineD3D `WINEDLLOVERRIDES` in
   one shot.
3. Add `SPARK_SKIP_WINE=1` short-circuit so the launcher transparently
   falls through to a native Linux build of the same target when Wine
   itself is broken in the environment.
4. Add `IsRunningUnderGvisor()` next to `IsRunningUnderWine()` in
   `Utils/WineDetection.{h,cpp}` and have the engine startup banner
   include it.
5. Add `SPARK_RHI_BACKEND` env-var fallback to the command-line
   `-rhi=` parsing so launchers without argv control can still pick
   the backend.

None of these are urgent — they are quality-of-life improvements that
each make a future regression in this stack diagnose-in-minutes
instead of diagnose-in-hours. Any session touching Wine, RHI selection,
or sandboxed execution should pick one off the list opportunistically.
