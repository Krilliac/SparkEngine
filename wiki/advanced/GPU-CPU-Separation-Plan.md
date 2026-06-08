# GPU/CPU Separation Plan

> **Audience:** Programmers
>
> **Thread Context:** Build-time / source-organization concern, not runtime. The split governs which translation units compile on which platform.
>
> **Platform/Backend Scope:** Windows (D3D11) vs. Linux/headless. The goal is that pure-CPU logic compiles everywhere while GPU-specific (D3D11) code is isolated into per-platform translation units.

## Overview

SparkEngine has a D3D11-primary rendering backend with Linux support via `Core/Platform.h` stubs. To keep CPU-portable code compiling on Linux/headless builds, GPU-heavy source files were split along the CPU/GPU boundary: a shared CPU `.cpp` plus platform-specific implementation files. This plan tracked the remaining work to complete that separation across the engine.

A naming note: the original plan proposed `*GPU.cpp` suffixes for the extracted GPU code. The implementation instead settled on **per-platform suffixes** — `*Windows.cpp` (D3D11) and `*Linux.cpp` (CPU stub) — which more directly expresses the three-way split the plan called for.

## Current Status (as of 2026-06-08)

**Overall: Substantially Completed (Phases 1-2), Partial (Phase 3), Not Started (Phase 4).**

| Phase | Scope | Status | Evidence |
|-------|-------|--------|----------|
| 1 | Complete the 4 deferred file splits | **Completed** | `AssetTypesWindows.cpp` + `AssetTypesLinux.cpp`, `MaterialSystemWindows.cpp` + `MaterialSystemLinux.cpp`, `PBRMaterialLightingWindows.cpp` + `PBRMaterialLightingLinux.cpp`, `GPUParticleSystemWindows.cpp` + `GPUParticleSystemLinux.cpp` all exist under `SparkEngine/Source/Graphics/` |
| 2 | Unguard CPU-portable headers | **Likely Completed / N/A** | Earlier splits (Foliage, PostProcessing, TextureSystem, UpscalingSystem) now ship as `*Windows.cpp` companions; portable headers compile on Linux. Not separately re-audited line-by-line. |
| 3 | Wire 6 lifecycle-only systems to the GPU pipeline | **Partial** | `BVHAccelerator::FrustumQuery` is now called from `SceneRenderer.cpp:132`. GTAO, VCT (`TraceDiffuse`/`TraceSpecular`), `ShaderVariantSystem::RequestVariant`, and the denoiser remain documented as not called from any render pass (see their header comments). |
| 4 | RHI backend parity (Vulkan/OpenGL/D3D12/Metal) | **Not Started** | D3D11 remains the only fully functional backend; others are scaffolds. `NullRHIDevice` works as the headless fallback. |

The earlier-completed splits described in the original "Current State" section (FoliageRenderer, FoliageImpostorBaker, PostProcessingPipeline, TextureSystem, UpscalingSystem) are confirmed present, now as `*Windows.cpp` companion files rather than `*GPU.cpp`.

## Phase 1 — Deferred Splits (Completed)

The four files used a nested dual-guard pattern (`#ifdef SPARK_PLATFORM_WINDOWS … #else … #endif`) where both the Windows D3D11 implementation and the Linux CPU stub lived in one file with diverging includes. The fix was a three-way split: shared CPU helpers in the original `.cpp`, the Windows block extracted to `*Windows.cpp`, the Linux block to `*Linux.cpp`.

| File | Status |
|------|--------|
| `AssetTypes.cpp` → `AssetTypesWindows.cpp` / `AssetTypesLinux.cpp` | Done |
| `MaterialSystem.cpp` → `MaterialSystemWindows.cpp` / `MaterialSystemLinux.cpp` | Done |
| `PBRMaterialLighting.cpp` → `PBRMaterialLightingWindows.cpp` / `PBRMaterialLightingLinux.cpp` | Done |
| `GPUParticleSystem.cpp` → `GPUParticleSystemWindows.cpp` / `GPUParticleSystemLinux.cpp` | Done |

## Phase 2 — GPU→CPU Portability (Unguard Portable Headers)

The plan flagged headers whose `#ifdef SPARK_PLATFORM_WINDOWS` guards were wrapping pure-CPU logic (enums, sort keys) that should compile everywhere — e.g. `DynamicQualityTypes.h`, `TemporalEffectsTypes.h`, `DrawSortKey.h`. Correct guards (D3D11 ComPtrs in `FoliageRenderer.h`, `PostProcessingPipeline.h`) were to stay. With the Phase 1 splits landed and Linux builds passing, this work is effectively folded in; a fresh line-by-line audit was not separately confirmed.

## Phase 3 — CPU→GPU Wiring (Partial)

Six systems were "lifecycle-only" — initialized but never producing GPU output:

| System | Original Gap | Status (2026-06-08) |
|--------|--------------|---------------------|
| **BVHAccelerator** | `FrustumQuery` never called | **Wired** — called from `SceneRenderer.cpp` cull pass |
| **GTAOEffect** | `ComputeGTAO` never dispatched | Not wired (header still notes GPU path uncalled) |
| **VCTSystem** | `TraceDiffuse`/`TraceSpecular` never called | Not wired (`VoxelConeTracing.h` notes uncalled) |
| **ShaderVariantSystem** | `RequestVariant` never called | Not wired (`ShaderVariantSystem.h` notes uncalled) |
| **SoftwareDenoiser** | `Denoise` never called | Not wired (`DenoiserInterface.h` notes uncalled) |
| **NoiseGraph** | `Evaluate` never called | Not separately confirmed |

The original priority order put BVHAccelerator → SceneRenderer first; that is the one that landed.

## Phase 4 — RHI Backend Parity (Not Started)

D3D11 remains the only functional backend. The RHI abstraction exists but D3D12/Vulkan/Metal/OpenGL are scaffolds; `NullRHIDevice` is the working headless fallback. The per-platform split is the enabler: a second backend would add `*Vulkan.cpp`/`*OpenGL.cpp` alongside the existing `*Windows.cpp`, with the shared CPU `.cpp` unchanged and CMake selecting the backend via `ENABLE_VULKAN` etc.

## Verification (per phase)

1. `cmake --preset linux-gcc-release && cmake --build build --config Release`
2. `cd build && ctest --output-on-failure`
3. `tools/validate-all.sh --warn-only`
4. `clang-format -i` on modified files

## Source & Freshness

- **Original entry date:** 2026-04-12 (`gpu-cpu-separation-plan-2026-04-12.md`, type: Plan)
- **Verified against codebase 2026-06-08.**
- Status bullets:
  - **Phase 1 Completed** — all four deferred files now have `*Windows.cpp`/`*Linux.cpp` companions (naming differs from the plan's `*GPU.cpp` proposal).
  - **Phase 3 Partial** — only `BVHAccelerator::FrustumQuery` is wired in (`SceneRenderer.cpp:132`); GTAO/VCT/ShaderVariant/Denoiser headers still self-document as uncalled.
  - **Phase 4 Not Started** — D3D11-only; other RHI backends remain scaffolds, `NullRHIDevice` is the headless fallback.

## Related Pages

- [Jolt Physics Integration](Jolt-Physics-Integration.md)
- [Wine Role and Fallback Tiers](Wine-Role-and-Fallback-Tiers.md) — NullRHIDevice / software-render tiers
