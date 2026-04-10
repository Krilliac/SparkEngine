# Engine next-steps — Phase D (2026-04-10)

**Status:** Active. Three follow-ups from the Phase C "remaining work"
list — DXR `.cso` build step, Foliage lifecycle bake, DXR test
coverage. The two SDK-vendoring items (OpenXR for VRSystem, Steamworks
for SteamTransport) are still deferred.

## Items closed

### D1 — DXR build-step shader compilation

**Problem:** Phase C added `LoadDXILBlob` to read pre-compiled `.cso`
files for DXR but the build system never produced them. Trace methods
were always no-ops because `BuildRTPSOs` couldn't load the bytecode.

**Investigation:** Two related gaps in the existing CMake.

1. The shader copy logic at `CMakeLists.txt:1293` only globs
   `SparkEngine/Shaders/HLSL/*.hlsl` (25 files). The actual DXR HLSL
   files live under `Shaders/HLSL/RayTracing/` (top-level), along with
   ~91 other shaders (compute, mesh, etc.) — none are copied to the
   runtime directory at all.

2. The runtime DXR code expected `.cso` blobs but no compile step
   existed. The engine's existing shader compile path uses
   `D3DCompile` (SM 5.0/5.1), which can't produce DXR libraries —
   those need DXC's `lib_6_3` profile.

**Fixes (`CMakeLists.txt`):**

- Added a `copy_directory` POST_BUILD command for the entire
  `Shaders/HLSL/` tree, mirroring it under
  `$<TARGET_FILE_DIR:SparkEngine>/Shaders/HLSL/`. This makes the
  ~91 previously-orphaned shaders reachable at runtime, including
  the DXR shaders and the foliage impostor bake shader from Phase C.

- Added a DXC build step gated on `WIN32 AND ENABLE_DXR AND NOT MINGW`
  (matching the existing `SPARK_HARDWARE_RT` gate at
  `CMakeLists.txt:1511`):
  - `find_program(DXC_EXECUTABLE NAMES dxc dxc.exe HINTS ...)` looks
    in the Windows SDK bin path, the legacy SDK path, and `VULKAN_SDK`
    (which ships its own DXC).
  - When `dxc` is found, registers a per-file `add_custom_command`
    POST_BUILD that runs `dxc -T lib_6_3 -Fo <output>.cso <input>.hlsl`
    for each of `DXR{Reflections,Shadows,AO,GI}.hlsl`. The output
    lands in `$<TARGET_FILE_DIR:SparkEngine>/Shaders/HLSL/RayTracing/`
    where `LoadDXILBlob` looks for it.
  - When `dxc` is missing, prints a `STATUS` message explaining how
    to install it. The build still succeeds — the DXR runtime gracefully
    handles missing `.cso` files by skipping the PSO build with a
    warning.
  - `lib_6_3` library compilation discovers all `[shader("...")]`-tagged
    functions automatically, so no `-E entry` flag is needed.

**Linux native:** the entire DXC block is wrapped in
`if(WIN32 AND NOT MINGW)` so Linux configure/build is unaffected. The
top-level shader copy still happens on Linux because some compute /
mesh shaders are usable via the Vulkan path (future work).

### D2 — Foliage impostor lifecycle bake

**Problem:** Phase C added `FoliageImpostorAtlas` (D3D11 baker for the
runtime impostor texture) but documented the lifecycle wiring as
"intentionally not included — needs its own session for AssetPipeline
mesh fetch + bbox compute + tint plumbing".

**Fixes:**

1. **`FoliageManager` API extension** (FoliageSystem.h/.cpp):
   New `GetSpeciesByGlobalIndex(uint32_t)` const accessor. The
   existing API only had `FindSpecies(name)` and `GetSpeciesCount()`,
   so iterating the registry by index meant building a name list
   externally. The new accessor matches the existing slot-based
   `GetSpeciesGlobalIndex(name)` lookup.

2. **`FoliageImpostorAtlas::BakeAllRegisteredSpecies(...)`**
   (FoliageImpostorBaker.h/.cpp):
   New method that walks `FoliageManager`, calls
   `ComputeAtlasLayout()` for the full species count, re-`Initialize`s
   the atlas at the resulting size (Initialize is idempotent — it
   releases old resources first), then iterates species and:
   - fetches each mesh through a user-supplied loader callback
   - reads `MeshAssetData::boundingBoxMin/Max` from the loaded mesh
   - calls `BakeSlot` with a default foliage-green tint
   - skips species whose mesh load returns null (will be retried on
     the next bake)
   - returns the count of successfully baked species

   The mesh loader is passed in as a `std::function` so this method
   stays testable without an `AssetPipeline` dependency, matching the
   existing `FoliageRenderer::FoliageMeshLoader` shape.

3. **`FoliageRenderer::BakeImpostorAtlasIfNeeded(device, context)`**
   (FoliageRenderer.h/.cpp):
   New Windows-only public method on the renderer. Holds an internal
   `FoliageImpostorAtlas m_impostorAtlas` singleton plus an
   `m_lastBakedSpeciesCount` watermark. Re-bakes when:
   - the atlas hasn't been initialised yet, OR
   - the species count has grown since the previous bake

   This handles late species registration: a game module that calls
   `FoliageManager::RegisterSpecies` after engine startup will have
   its species picked up on the next frame's `CollectFromFoliageManager`
   call without any explicit "rebuild atlas" plumbing.

4. **Lazy bake hook in `CollectFromFoliageManager`**
   (FoliageRenderer.cpp):
   At the end of the per-frame collect, on Windows builds, looks up
   `EngineContext::Get()->GetGraphics()` and (if non-null) calls
   `BakeImpostorAtlasIfNeeded(graphics->GetDevice(),
   graphics->GetContext())`. The test path is unaffected — there's
   no GraphicsEngine in the unit tests, so the call short-circuits.

5. **`FoliageRenderer::GetImpostorAtlas()`** read-only accessor for
   downstream code that wants to bind the SRV.

**What's still missing for full impostor rendering:**
The atlas is now built, populated, and accessible — but the foliage
shader pair (`FoliagePS.hlsl`/`FoliageVS.hlsl`) doesn't yet bind the
atlas SRV or sample it for impostor-LOD instances. That's its own
session: it touches the foliage VS to handle billboard orientation
for impostors, the PS to sample the atlas, and the engine code that
binds resources before drawing the foliage batch.

### D3 — DXR test coverage

**Problem:** Phase C noted "no `Tests/` files exist for DXR
specifically — the D3D12 device stack is not reachable from the
native Linux test runner". This phase adds platform-agnostic coverage
for the parts of `DXRManager` that don't depend on a real device.

**New file `Tests/TestDXRSupport.cpp`** (~200 lines, 13 test cases):

| Test | What it covers |
|------|---------------|
| `DXR_RTFeature_OrOperator` | bitwise OR + HasFeature |
| `DXR_RTFeature_AndOperator` | bitwise AND + HasFeature |
| `DXR_RTFeature_NoneIsFalsy` | unary `!` + None state |
| `DXR_RTFeature_AllIncludesEverything` | All flag covers all features |
| `DXR_Manager_NotAvailableInHeadless` | IsAvailable() == false on test runner |
| `DXR_Manager_BackendDisabledWhenUnavailable` | GetBackend() == Disabled |
| `DXR_Manager_InitializeWithNullDeviceFails` | null device → fails cleanly |
| `DXR_Manager_TraceMethodsNoOpWhenUninitialized` | All 4 Trace*() are crash-safe |
| `DXR_Manager_SettingsRoundTrip` | SetSettings/GetSettings preserves data |
| `DXR_Manager_ConsoleEnableFeature` | Console_EnableFeature toggle + unknown name |
| `DXR_Manager_ConsoleSetQualityPresets` | low/medium/high/ultra renderScale |
| `DXR_Manager_StatsZeroWhenUninitialized` | GetStats() returns zeros |
| `DXR_Manager_ConsoleStatusFormat` | Console_GetStatus() format invariants |

13 tests, 47 assertions, all passing. Registered in
`Tests/CMakeLists.txt` next to `TestFoliageImpostorBaker.cpp`.

**Why these particular tests?**
The DXR full pipeline only runs on Windows MSVC with a DXR-capable
GPU and pre-compiled `.cso` shader blobs. None of those are reachable
in CI, so the tests deliberately cover only the public API contract
that the rest of the engine depends on regardless of platform — the
parts that would silently break if someone changed the enum operators,
the `Trace*()` guards, or the settings round-trip.

The tests run on Linux native and don't need any platform-specific
gating — `DXRManager::GetInstance()` is always callable and always
returns a non-initialised manager on a non-Windows host.

## Files touched

```
CMakeLists.txt                                              (D1)
SparkEngine/Source/Graphics/FoliageSystem.h                 (D2)
SparkEngine/Source/Graphics/FoliageSystem.cpp               (D2)
SparkEngine/Source/Graphics/FoliageImpostorBaker.h          (D2)
SparkEngine/Source/Graphics/FoliageImpostorBaker.cpp        (D2)
SparkEngine/Source/Graphics/FoliageRenderer.h               (D2)
SparkEngine/Source/Graphics/FoliageRenderer.cpp             (D2)
Tests/CMakeLists.txt                                        (D3)
Tests/TestDXRSupport.cpp                                    (D3) (new)
.claude/knowledge/engine-next-steps-phase-d-2026-04-10.md   (new)
.claude/index.md
```

## Build status

- `cmake --preset linux-gcc-release` — clean
- `cmake --build build/linux-gcc-release --config Release` — builds
  SparkEngineLib + SparkEngine + SparkTests + 10 game modules clean.
  Only pre-existing ODR warnings (DebugHookManager).
- `ctest` — 1/1 test executable, **4344 passed, 0 failed, 1 warned**
  (the LoadTest stability warning from Phase B).
- `clang-format` — clean on all 9 changed code files plus new test.
- 13 new DXR tests, 47 new assertions.

## What still needs follow-ups

| Item | Why deferred |
|------|--------------|
| OpenXR (`VRSystem`) | SDK vendoring (skipped per user) |
| Steamworks (`SteamTransport`) | SDK vendoring (skipped per user) |
| Foliage impostor render binding | Foliage VS/PS need to sample the atlas SRV for impostor-LOD instances. Touches FoliagePS.hlsl, the foliage constant buffer slots, and the resource-binding step before the foliage draw. |
| DXR runtime tests on Windows | Needs CI job that runs SparkTests on a DXR-capable Windows host with pre-compiled `.cso` blobs present, OR a D3D12 mock layer. |
| Top-level Shaders/HLSL/ shader compile | The 91 non-DXR shaders are now copied to the runtime dir but most aren't compiled to `.cso`. Each subsystem owns its own compile step (or runtime D3DCompile call). Rolling them all into a unified offline pipeline is its own engineering session. |
