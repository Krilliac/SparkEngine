# Engine next-steps — Phase U (2026-04-11)

**Type:** Observation
**Status:** Active — landed
**Scope:** Activate `Spark::Graphics::ShaderHotReload` — the process-wide
shader file watcher from `Graphics/ShaderHotReload.h`. First phase of
Theme 3A (Shader hot-reload surface) from the Phase U+ plan.

---

## Summary

Wired the `Spark::Graphics::ShaderHotReload` singleton into the `Shader`
class lifecycle and the per-frame pump on both the Windows and Linux
branches. Prior to this phase the class had zero production call sites —
the header-only singleton existed in `Graphics/ShaderHotReload.h` but
every path through `Shader::Initialize`, `LoadFromFile`, and
`GraphicsEngine::BeginFrame` ignored it and fell through to the legacy
per-Shader Win32 file watcher. After this phase each `Shader::Initialize`
call lazily initialises the process-wide singleton, each
`LoadVertexShader`/`LoadPixelShader`/`LoadFromFile` registers its parent
directory with the singleton, and each frame `HotReloadShaders()` pumps
`ShaderHotReload::Update(dt)` so source `.hlsl` changes on disk are
picked up by the shared file watcher on both platforms.

## Files touched (code)

- `SparkEngine/Source/Graphics/Shader.cpp`
  - Added `#include "ShaderHotReload.h"` + `#include <filesystem>` on
    both Windows and Linux include blocks.
  - `Shader::Initialize` (both branches): lazy
    `ShaderHotReload::GetInstance().Initialize(path)` on the first
    existing search path, fallback to `"."`; subsequent Shader instances
    call `AddWatchDirectory(path)` for each existing search path so the
    singleton merges all active watch roots.

- `SparkEngine/Source/Graphics/ShaderCompilation.cpp`
  - Added `#include "ShaderHotReload.h"` + `#include <filesystem>` on
    both Windows and Linux include blocks.
  - `LoadVertexShader`, `LoadPixelShader`, `LoadFromFile` (all four
    sites across both branches): on successful load, registers
    `std::filesystem::path(path).parent_path()` with
    `ShaderHotReload::AddWatchDirectory` so future file mtime changes
    in that directory are picked up by the polling loop.

- `SparkEngine/Source/Graphics/ShaderHotReload.cpp`
  - Added `#include "ShaderHotReload.h"` on both Windows and Linux
    branches.
  - `Shader::HotReloadShaders` (both branches): prepended
    `Spark::Graphics::ShaderHotReload::GetInstance().Update(0.016f)` so
    every call to the legacy per-Shader reloader also ticks the
    singleton. The singleton owns its own poll-interval gating (default
    `0.5 s`) so passing a nominal delta each frame is cheap — the
    singleton only touches the filesystem on the configured interval.

- `SparkEngine/Source/Graphics/GraphicsEngine.cpp`
  - Added `#include "ShaderHotReload.h"` + `#include "Shader.h"` to the
    Linux branch.
  - Linux `GraphicsEngine::BeginFrame`: added a call to
    `m_shader->HotReloadShaders()` after `m_vramBudgetMonitor->Update()`.
    The Windows branch already pumped `HotReloadShaders` from
    `BeginFrame`; adding the Linux call means both platforms tick the
    singleton each frame.

## Files touched (tests)

- `Tests/TestShaderHotReloadPhaseU.cpp` (new) — 9 tests against the real
  `Spark::Graphics::ShaderHotReload` singleton covering behaviour relied
  on by the Phase U wire-up:
  1. `UpdateBelowIntervalDoesNotPoll` — sub-poll-interval ticks
     accumulate without triggering `CheckForChanges`.
  2. `UpdateOverIntervalPollsOnce` — a single over-interval tick polls
     but does not fire callbacks when no file has changed.
  3. `InitializeIsIdempotent` — calling `Initialize` twice on the same
     directory is a no-op (matches the wire-up's check-and-initialise
     pattern).
  4. `AddWatchDirectoryMergesShaders` — `Initialize(dirA)` +
     `AddWatchDirectory(dirB)` produces a merged watched-file set
     covering both roots (matches the wire-up's search-path walk).
  5. `AddWatchDirectoryNonexistentIsSafe` — passing a missing path to
     `AddWatchDirectory` does not throw and does not grow the registry.
  6. `RecursiveDirectoryTraversal` — a tree of subdirectories under the
     watch root is walked recursively and every `.hlsl` file discovered.
  7. `MtimeBumpTriggersReload` — rewriting a file after the FS mtime
     granularity window + one over-interval `Update` fires a callback
     with the expected `shaderName`.
  8. `InitialiseThenShutdownDisables` — `Shutdown` clears
     `IsWatching`/`IsEnabled`/`GetWatchedShaderCount`.
  9. `UpdateAfterShutdownIsNoOp` — `Update(dt)` on a shut-down singleton
     is a silent no-op (the wire-up pumps the singleton every frame
     regardless of Shader instance lifecycle).

- `Tests/CMakeLists.txt` — added `TestShaderHotReloadPhaseU.cpp` to the
  `SparkTests` source list.

Every test starts with `ResetSingleton()` (which calls `Shutdown`) to
flush any residual callbacks from earlier test files. The existing
`TestShaderHotReload.cpp` and `TestShaderHotReloadCompilation.cpp`
already call `Shutdown` at the end of each test, so the cross-file
interference surface is small — the `ResetSingleton` helper is an
explicit safety net.

## Activation verification

After Phase U:

- `Spark::Graphics::ShaderHotReload::GetInstance()` has three production
  call sites: `Shader::Initialize`, `Shader::LoadFromFile` /
  `LoadVertexShader` / `LoadPixelShader`, and `Shader::HotReloadShaders`
  (via the pump). The `GraphicsEngine` Linux branch now also drives
  `m_shader->HotReloadShaders()` from `BeginFrame`, mirroring the
  Windows path that was already present.
- The orphan count for Theme 3A shader hot-reload triplet drops from 3
  to 2 — `ShaderDiskCache` and `ShaderCrossCompiler` remain for Phases V
  and W.
- Existing `TestShaderHotReload.cpp` / `TestShaderHotReloadCompilation.cpp`
  tests still pass — the singleton's semantics are unchanged; Phase U
  only adds new call sites.

## Playbook notes for future phases

1. **Singleton lifecycle is shared across Shader instances.** The
   Phase U wire-up uses `if (!hotReload.IsWatching())` to guard the
   first Initialize — subsequent Shader instances skip re-init and
   instead append their search paths via `AddWatchDirectory`. This
   pattern works because `ShaderHotReload::Initialize` clears watched
   directories but `AddWatchDirectory` is additive. Phase V
   (`ShaderDiskCache`) is likely to follow a different pattern because
   it's per-Shader state, not process-wide.

2. **`Shader::Shutdown` does NOT tear down the singleton.** The
   singleton outlives any individual Shader instance — a teardown here
   would break other live Shader objects using the same watcher. The
   singleton is a permanent singleton, matching the
   `SimpleConsole::GetInstance` convention.

3. **FS mtime granularity matters for mtime tests.** ext4 has 1 s
   resolution; the Phase U `MtimeBumpTriggersReload` test sleeps
   `1100 ms` before rewriting so the mtime tick advances. Use this
   same pattern in Phase V / W if any disk cache test depends on a
   fresh mtime.

4. **Parent directory is the unit of watch registration.** Singleton
   watches directories, not individual files. The wire-up calls
   `AddWatchDirectory(parent_path)` rather than `AddShader(file)` —
   this is a deliberate choice so sibling shaders are picked up for
   free without the caller knowing about them.

5. **Pump delta is nominal.** `Update(0.016f)` in the HotReloadShaders
   pump is a nominal 16 ms (60 FPS). Over-estimating is fine because
   the singleton internally gates on its own poll interval (default
   0.5 s), so a 200 ms frame will still only poll once per interval
   boundary.

6. **Update(dt) on a disabled singleton is a silent no-op.** Verified
   by `ShaderHotReloadPhaseU_UpdateAfterShutdownIsNoOp`. This matters
   because the `GraphicsEngine::BeginFrame` pump unconditionally calls
   `HotReloadShaders()` even before a Shader instance has loaded any
   files. The singleton's `if (!m_enabled) return;` early-out keeps
   this path cheap.

## Cross-references

- Plan: [engine-next-steps-phase-u-plan-2026-04-11.md](engine-next-steps-phase-u-plan-2026-04-11.md)
- Previous phase: [engine-next-steps-phase-t-2026-04-11.md](engine-next-steps-phase-t-2026-04-11.md)
- Next phases: Phase V (`ShaderDiskCache`), Phase W (`ShaderCrossCompiler`)
