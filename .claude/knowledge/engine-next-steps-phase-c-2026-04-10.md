# Engine next-steps — Phase C (2026-04-10)

**Status:** Active. Three follow-ups from the Phase B "remaining work"
list (DXR finish, Foliage GPU bake, SelectionManager panel migration)
landed in this branch. Each was scoped down from "ship a complete
subsystem" to "make the code structurally correct and unblock the next
session" so the work would fit in one branch.

## Items closed

### C1 — DXR finish (RHI/DXRSupport.{h,cpp})

**Problem:** Phase B documented DXR as "~80% — needs shader binding
tables + DispatchRays". Investigation found a more accurate picture:
the DispatchRays call was already in `DispatchRT` and BuildShaderTables
existed, but the implementation had three latent bugs that would have
caused runtime faults if a user enabled DXR:

1. `BuildRTPSOs` used fictional export names (`ReflectionRayGen`,
   `ShadowRayGen`, etc.) that do not exist in the .hlsl files. The
   actual entry points are `RayGen`, `ClosestHit`, `Miss` — same names
   in all four files. Each PSO has its own DXIL library subobject so
   reusing the names is fine.
2. `BuildRTPSOs` had an empty `D3D12_DXIL_LIBRARY_DESC` — no shader
   bytecode pointer. CreateStateObject would fail.
3. `BuildShaderTables` allocated only one set of rayGen/miss/hitGroup
   tables and populated them with PSO 0's identifiers. PSOs 1-3 would
   dispatch shaders that didn't match their state object.

**Fixes (DXRSupport.cpp):**
- Added `LoadDXILBlob(path)` helper that reads pre-compiled `.cso`
  files from disk into a vector. Missing files log a warning and the
  matching PSO is skipped — the trace becomes a no-op rather than
  crashing.
- `BuildRTPSOs` now loads `Shaders/HLSL/RayTracing/DXR{Reflections,
  Shadows,AO,GI}.cso`, builds export descriptors using the real
  `RayGen`/`ClosestHit`/`Miss` names, and stores the blob in
  `DXRInternalState::dxilBlobs[i]` so the raw pointer in
  `D3D12_DXIL_LIBRARY_DESC` stays valid until CreateStateObject
  finishes. The Shadows shader has no closesthit (visibility-only),
  so its export list is rayGen+miss only.
- `BuildShaderTables` now builds **per-PSO** rayGen / miss /
  hitGroup tables (`rayGenTables[4]`, `missTables[4]`,
  `hitGroupTables[4]`) using `CreateUploadBufferWithData`. Each table
  is populated from its own PSO's `ID3D12StateObjectProperties`.
- New `EnsureOutputTexture(state, effectIndex)` lazily allocates a
  `R16G16B16A16_FLOAT` UAV at the current output dimensions, recreating
  on resize. Called from inside `DispatchRT`.
- New `UpdateFrameConstants(...)` writes a per-frame constant buffer
  with `InvViewProj`, camera position, light direction, max distance,
  bounces, samples, and roughness threshold — layout matches the
  `RTConstants` cbuffer in the four DXR HLSL files.
- The four `Trace*` methods now actually take and use their parameters
  (they were `/* */`-prefixed before) and call `UpdateFrameConstants`
  before invoking `DispatchRT`.
- `DispatchRT` was rewritten to use the per-PSO tables, lazily
  allocate the output texture, and bind the per-frame constant buffer
  at root parameter slot 2.

**Build dependency:** the runtime expects pre-compiled `.cso` blobs
next to the `.hlsl` files. Production CI must add a shader-compile
step over `Shaders/HLSL/RayTracing/`. Until that's wired, the trace
methods log a warning at PSO build time and become no-ops at runtime
— no crashes, no rendering artefacts, just zero RT effects.

**Updated `@warning` → `@note`:** the header doc block was rewritten
to reflect the new state. The remaining gap is the build-system
shader compile step plus a future MinGW-Wine-Lavapipe test path — no
core implementation work left in this file.

**Linux native build:** the entire DXR file body is under
`#ifdef SPARK_PLATFORM_WINDOWS`, so the Linux build still compiles
the .cpp but gets only the empty wrapper class members. CI's Windows
job is the only place this code executes.

### C2a — FoliageRenderer GPU upload wired

`FoliageRenderer::UploadToSceneBuffer` was already implemented but
nothing called it. Added a call from `GraphicsEngine::EndFrame()` just
before `m_gpuSceneBuffer.FlushToGPU()`:

```cpp
auto& foliage = Spark::Graphics::FoliageRenderer::GetInstance();
if (foliage.IsInitialized())
{
    const uint32_t startSlot = m_gpuSceneBuffer.GetActiveCount();
    const uint32_t written = foliage.UploadToSceneBuffer(m_gpuSceneBuffer, startSlot);
    ...
}
```

The renderer's CPU `CollectFromFoliageManager` step is wired into the
Foliage update at `GameplayLifecycleShared.cpp:753`, so by the time
`EndFrame` runs the latest batch is ready. The upload appends to the
GPU scene buffer after the static-mesh slots already populated by the
ECS draw submission, using `GetActiveCount()` as the start slot (the
existing GPUSceneBuffer API).

### C2b — FoliageImpostorAtlas GPU bake foundation

The Phase B `@warning` on `FoliageImpostorBaker.h` listed three
missing pieces: no compute shader, no render-target allocation, no
HLSL under `Shaders/HLSL/FoliageImpostor*`. All three are now in.

**New file: `Shaders/HLSL/FoliageImpostorBake.hlsl`** — minimal
vertex + pixel shader pair. The VS multiplies vertex position by a
constant-buffer ortho view-proj. The PS does half-lambert wrap on the
mesh normal and modulates by a per-species RGBA tint, so impostors
read correctly even when no diffuse texture is bound. Constant buffer
layout matches the C++ `ImpostorBakeCB` struct.

**New class: `Spark::Graphics::FoliageImpostorAtlas`** (Windows-only,
in the existing `FoliageImpostorBaker.h`/`.cpp`). API:

- `Initialize(device, width, height)` — allocates an `R8G8B8A8_UNORM`
  atlas texture, matching depth buffer, RTV, SRV, dynamic CB,
  rasterizer/depth/blend state, compiles `VSMain`/`PSMain` from
  `FoliageImpostorBake.hlsl`, and creates an input layout matching
  `MeshAssetData::Vertex` (96-byte stride: pos / normal / tangent /
  uv0 / uv1 / color / boneIndices / boneWeights).
- `BakeSlot(context, slot, mesh, bboxMin, bboxMax, tint)` — fits an
  orthographic projection around the mesh's bounding box (using the
  XZ diagonal as ortho width so any yaw rotation still encloses the
  mesh), iterates over `slot.angleSteps` yaw angles, sets a viewport
  the size of one cell at `(slot.atlasX + angle * slot.cellSize,
  slot.atlasY)`, builds the view-proj for that angle, updates the CB,
  clears only the depth buffer (so adjacent cells already in the atlas
  remain untouched), and calls `DrawIndexed`.
- `Shutdown()` releases everything.
- `GetSRV()` / `GetTexture()` for downstream consumption.

**Why is no automatic lifecycle wiring included?**
Walking the FoliageManager species registry to bake every species at
startup would need: an AssetPipeline mesh fetch per species, a
bounding-box compute, and a tint plumbed from the species record. Each
of those is a small-but-real piece of design work that needs to land
its own session — getting it wrong would cause atlas corruption or
unbounded GPU memory usage. The atlas is now **available** to
production code as a system that can be used; it is not yet
automatically populated.

**Updated `@warning` → `@note`:** the header now describes the
existing CPU helpers, the new `FoliageImpostorAtlas` class, and the
single remaining gap (lifecycle bake invocation).

### C5 — SelectionManager full panel migration

Phase B widened `SelectionManager::EntityId` from `uint32_t` to
`uint64_t` to unblock the migration. This phase actually makes the
panels use the singleton.

**HierarchyPanel** is now the **authoritative source** of selection.
It pushes its state into `SelectionManager` from
`NotifySelectionChanged()`:

```cpp
if (!m_syncingFromSelectionManager) {
    std::vector<EntityId> ids = ...;
    SelectionManager::GetInstance().SelectMultiple(ids);
}
```

The `m_syncingFromSelectionManager` re-entry guard breaks the loop
when an external `Select` triggers a callback that would call back
into the panel.

In `Initialize()`, HierarchyPanel also subscribes to incoming
SelectionManager changes:

```cpp
m_selectionMgrCallbackId = SelectionManager::GetInstance().OnSelectionChanged(
    [this](const SelectionChangedEvent& event) {
        if (m_syncingFromSelectionManager) return;
        // mirror event.current → m_selectedObjects
    });
```

Unregisters in `Shutdown()`.

**InspectorPanel** is now an **observer**. Its `Initialize()`
subscribes to SelectionManager and updates `m_inspectedObjectID` to
the back of the order vector (matching `GetPrimarySelection()`),
clearing it when the selection becomes empty. The `m_inspectedObjectID`
field stays — this is a one-way binding from selection authoritative
source to the panel's local state, so all the existing
`InspectorComponentRenderers_*` files that read `m_inspectedObjectID`
keep working without changes.

Unregisters its callback in `Shutdown()`.

**SceneViewPanel** turned out to have **no** selection state at all
(the Phase B audit was wrong on this one). No changes needed.

**Initialize order:** `EditorUI::Initialize()` calls
`SelectionManager::GetInstance().Initialize()` at line 207, then
`CreatePanels()` at line 256, which calls each panel's `Initialize()`
where the new callbacks are registered. The singleton is alive when
the panels register, and the panels are alive when external code
mutates the singleton.

**Editor build on Linux:** SparkEditor is skipped on Linux native
because Dear ImGui is not present in `ThirdParty/`. These panel
changes will be validated by the Windows MSVC CI job.

## What still needs follow-ups

| Item | Status |
|------|--------|
| DXR `.cso` build step | Needs build-system shader-compile pass for `Shaders/HLSL/RayTracing/`. Until then trace methods are no-ops with a logged warning. |
| Foliage impostor lifecycle bake | Manager needs to walk FoliageManager species registry → AssetPipeline mesh fetch → BakeSlot. Atlas SRV needs to be bound for sampling in the foliage render path. |
| OpenXR integration (VRSystem) | Phase B item — vendor `openxr_loader`, fill in `Initialize`/`UpdateTracking`, wire per-eye rendering. |
| Steamworks integration (SteamTransport) | Phase B item — vendor SDK, replace stubs. |
| DXR test coverage | No `Tests/` files exist for DXR — needs MinGW-Wine-Lavapipe path or D3D12 mock. |

## Files touched

```
SparkEngine/Source/Graphics/RHI/DXRSupport.h
SparkEngine/Source/Graphics/RHI/DXRSupport.cpp
SparkEngine/Source/Graphics/FoliageImpostorBaker.h
SparkEngine/Source/Graphics/FoliageImpostorBaker.cpp
SparkEngine/Source/Graphics/GraphicsEngine.cpp
Shaders/HLSL/FoliageImpostorBake.hlsl       (new)
SparkEditor/Source/Panels/HierarchyPanel.h
SparkEditor/Source/Panels/HierarchyPanel.cpp
SparkEditor/Source/Panels/InspectorPanel.h
SparkEditor/Source/Panels/InspectorPanel.cpp
SparkEditor/Source/Core/EditorUI.cpp
.claude/knowledge/engine-next-steps-phase-c-2026-04-10.md (new)
.claude/index.md
```

## Build status

- `cmake --preset linux-gcc-release` — clean
- `cmake --build build/linux-gcc-release --config Release` — builds
  SparkEngineLib + SparkEngine + SparkTests + 10 game modules clean.
  Only pre-existing ODR warnings (DebugHookManager) which were already
  present on the parent commit.
- `ctest` — 1/1 test executable passes (4566+ underlying assertions
  via TestMain). LoadTest_FullEngine_3000Frames remains in
  TestWarnings.h from Phase B.
- `clang-format` — clean on all 10 changed code files plus the new
  HLSL file.
- SparkEditor target is skipped on Linux (Dear ImGui not vendored)
  so the Inspector / Hierarchy changes will be validated only by the
  Windows MSVC CI job.
