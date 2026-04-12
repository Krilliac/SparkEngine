# GPU/CPU Separation — Future Session Plan

## Context

SparkEngine has a D3D11-primary rendering backend with Linux support via Platform.h
stubs. Prior sessions split 7 files along CPU/GPU boundaries (FoliageRenderer,
FoliageImpostorBaker, PostProcessingPipeline, TextureSystem, UpscalingSystem,
EditorWindowManager, PostProcessingPipeline.h). This plan covers the remaining
work to complete full CPU/GPU separation across the engine.

## Current State

### Completed Splits (7 files, 5 GPU extraction files)
| File | CPU | GPU |
|------|-----|-----|
| FoliageRenderer.cpp | 463 | FoliageRendererGPU.cpp (472) |
| FoliageImpostorBaker.cpp | 176 | FoliageImpostorBakerGPU.cpp (489) |
| PostProcessingPipeline.cpp | 475 | PostProcessingPipelineGPU.cpp (1089) |
| TextureSystem.cpp | 500 | TextureSystemGPU.cpp (748) |
| UpscalingSystem.cpp | 560 | UpscalingSystemGPU.cpp (462) |

### Already GPU-Only (no split needed)
- LightingSystem.cpp, Shader.cpp, ShaderCompilation.cpp, Mesh.cpp
- DecalSystem.cpp, RenderTarget.cpp, RenderTargetManager.cpp
- AssetPipeline.cpp, MaterialConsoleOps.cpp

### Already CPU-Only (no split needed)
- SparkEngine.cpp (1957 lines — pure CPU bootstrap)
- MemoryIntegrity.cpp (593 lines — OS API guards, not GPU)
- AngelScriptEngine.cpp (1136 lines — scripting, CPU-only)

---

## Phase 1: Complete Deferred Splits (4 files)

These files use a **nested dual-guard pattern** that broke mechanical extraction:
```cpp
#ifdef SPARK_PLATFORM_WINDOWS
// ... Windows D3D11 implementation ...
#else
// ... Linux CPU stub implementation ...
#endif
```

The issue: both Windows and Linux implementations exist in the same file,
but the includes and method bodies differ by platform.

### Approach: Three-Way Split

Instead of CPU + GPU, use **CPU + D3D11 + LinuxStub**:

| File | Lines | Strategy |
|------|-------|----------|
| **AssetTypes.cpp** (1100) | Win: 537, Linux: 563 | Extract Win block → `AssetTypesD3D11.cpp`, extract Linux block → `AssetTypesLinux.cpp`, leave shared helpers in original |
| **MaterialSystem.cpp** (869) | Win: 405, Linux: 135 | Extract Win GPU methods → `MaterialSystemGPU.cpp` (bind, texture load, samplers). Linux stubs are small enough to keep inline. |
| **PBRMaterialLighting.cpp** (838) | Win: 538, Linux: 45 | Extract Win lighting pass → `PBRMaterialLightingGPU.cpp`. Linux stubs trivial. |
| **GPUParticleSystem.cpp** (528) | Win: 377, Linux: 0 | Mostly GPU-only; add Linux stubs rather than splitting. |

### Implementation Steps
1. Read each file, identify the outer `#ifdef`/`#else`/`#endif` structure
2. For AssetTypes: create three files (shared CPU + D3D11 impl + Linux impl)
3. For the others: extract the `#ifdef SPARK_PLATFORM_WINDOWS` block to `*GPU.cpp`
4. Ensure Linux stubs remain in the original or are complete in a `*Linux.cpp`
5. Build on Linux GCC, verify all tests pass
6. Build on MinGW (if available) to verify Windows path compiles

---

## Phase 2: GPU→CPU Portability (Move CPU Logic Out of Windows Guards)

53 Graphics headers use `#ifdef SPARK_PLATFORM_WINDOWS`. Most guards are
correct (protecting D3D11 types), but some guard CPU-portable logic that
should run on all platforms.

### Candidates to Unguard

Check each for CPU algorithms trapped behind Windows guards:

| Header | What's Guarded | Portable? |
|--------|---------------|-----------|
| FoliageRenderer.h | `UploadToSceneBuffer`, render pass members | D3D11 types — correct |
| BVHAccelerator.h | Nothing significant | Already portable |
| PostProcessingPipeline.h | `RenderTargetPool`, `GPUDebugMarkers`, shader ComPtrs | D3D11 types — correct |
| DynamicQualityTypes.h | Enums only | **Already portable** — can remove guard |
| TemporalEffectsTypes.h | Enums only | **Already portable** — can remove guard |
| DrawSortKey.h | Sort key computation | **Likely portable** — check for D3D11 deps |

### Implementation Steps
1. Grep all `.h` files for `#ifdef SPARK_PLATFORM_WINDOWS`
2. For each, check if the guarded code uses D3D11 types
3. If NOT (pure CPU structs/enums/algorithms): remove the guard
4. Build on Linux to verify no D3D11 types leak through

---

## Phase 3: CPU→GPU Wiring (Connect CPU Systems to GPU Pipeline)

Six systems were documented as "lifecycle-only" (initialized but never produce
GPU output). Completing their wiring requires actual GPU work:

| System | Gap | Effort |
|--------|-----|--------|
| **VCTSystem** | TraceDiffuse/TraceSpecular never called | Large — needs deferred render pass + compute shaders |
| **GTAOEffect** | ComputeGTAO never dispatched | Medium — needs compute shader dispatch in post-process |
| **BVHAccelerator** | FrustumQuery never called from scene renderer | Medium — wire into SceneRenderer's cull pass |
| **SoftwareDenoiser** | Denoise never called | Small — wire after RT accumulation (requires DXR path) |
| **NoiseGraph** | Evaluate never called | Small — wire into terrain/procedural consumer |
| **ShaderVariantSystem** | RequestVariant never called | Medium — wire into MaterialSystem bind path |

### Priority Order
1. BVHAccelerator → SceneRenderer (immediate perf benefit for large scenes)
2. ShaderVariantSystem → MaterialSystem (unlocks material keyword permutations)
3. GTAOEffect → PostProcessingPipeline (visual quality improvement)
4. VCTSystem (deferred — needs full compute pipeline)

---

## Phase 4: RHI Backend Parity

Currently D3D11 is the only functional backend. The RHI abstraction exists
but backends are stubs. This phase ensures the CPU/GPU split supports
multiple backends:

1. **NullRHIDevice** — already functional (Phase Y/Z)
2. **OpenGLDevice** — has scaffold, needs real GL calls for the split GPU files
3. **VulkanDevice** — has scaffold, needs Vulkan command buffer integration
4. **D3D12Device** — has scaffold, DXR partially done
5. **MetalDevice** — macOS only, stub

### What the Split Enables
The `*GPU.cpp` files are all `#ifdef SPARK_PLATFORM_WINDOWS` / D3D11. To add
a second backend:
1. Create `*Vulkan.cpp` or `*OpenGL.cpp` alongside the `*GPU.cpp`
2. The CPU `.cpp` stays unchanged — it calls through the RHI interface
3. CMake selects which backend `.cpp` to compile based on `ENABLE_VULKAN` etc.

---

## Verification

After each phase:
1. `cmake --preset linux-gcc-release && cmake --build build --config Release`
2. `cd build && ctest --output-on-failure`
3. `tools/validate-all.sh --warn-only` (10/10 expected)
4. `clang-format -i` on modified files
5. Commit + push

---

## Summary

| Phase | Scope | Effort | Impact |
|-------|-------|--------|--------|
| 1 | 4 deferred file splits | 1-2 sessions | Completes CPU/GPU separation |
| 2 | Unguard portable headers | 1 session | More code runs on Linux |
| 3 | Wire 6 lifecycle-only systems | 2-3 sessions | Actual GPU features work |
| 4 | Multi-backend support | 3-5 sessions | Vulkan/OpenGL rendering |
