# PCSX2 Architecture Analysis for SparkEngine

**Last updated:** 2026-03-20
**Type:** Decision
**Status:** Resolved

## Description

Analysis of the PCSX2 PS2 emulator (github.com/PCSX2/pcsx2) for architectural patterns, performance techniques, and systems worth adopting in SparkEngine. PCSX2 is a mature C++ project with battle-tested solutions for graphics abstraction, memory management, threading, and state serialization.

## Context

SparkEngine already has: RHI abstraction (D3D11/D3D12/Vulkan/Metal/OpenGL), PipelineStateCache with FNV-1a hashing, ConstantBufferRing, DescriptorHeapAllocator, GPUCapabilities (RHIDeviceCapabilities), FrameAllocator, LRU asset cache, JobSystem, SaveSystem with ECS-aware persistence, Profiler, ReplaySystem, and AudioMixer. Previous analyses adopted systems from TrinityCore (10), CryEngine (16), and five other engines (20 recommendations).

## Methods Tried

1. WebFetch of PCSX2 repository structure and 30+ key header files → WORKED
2. WebSearch for PCSX2 architecture documentation → WORKED
3. Cross-reference against SparkEngine codebase via file search → WORKED

## Recommendations (12 total, filtered for what SparkEngine doesn't already have)

### HIGH Priority (5)

#### 1. Save State Tag Validation (FreezeTag Pattern)
- **PCSX2**: `SaveStateBase::FreezeTag()` inserts named checkpoint markers between subsystem sections during serialization. On deserialization, tags are validated to detect misalignment. `FreezeLegacy<T>()` handles struct evolution by reading truncated data when fields are added.
- **SparkEngine status**: MISSING — SaveSystem has no tag validation or legacy format support
- **Recommendation**: Add tag markers between subsystem sections in save files. Implement a `FreezeLegacy` equivalent for backward-compatible struct evolution when save formats change between updates.
- **Effort**: Low — extends existing SaveSystem serialization

#### 2. Null RHI Backend for Headless Testing
- **PCSX2**: `Null` renderer implements `GSDevice` interface with no-ops. Used by `pcsx2-gsrunner` for automated GS dump validation without GPU.
- **SparkEngine status**: MISSING — all backends require real hardware
- **Recommendation**: Create NullRHIDevice implementing RHIDevice interface with no-op methods. Enable render graph construction, draw call submission, and shader parameter binding tests in CI without a GPU.
- **Effort**: Medium — must implement full RHI interface but all methods are trivial

#### 3. Render Thread Command Ring Buffer (MTGS Pattern)
- **PCSX2**: MTGS uses an 8MB ring buffer to decouple CPU thread from graphics thread. Commands are typed enums, ring uses power-of-2 sizing with bitmask wrapping. `RunOnGSThread()` enables arbitrary lambdas on the render thread.
- **SparkEngine status**: PARTIAL — D3D12 has command list recording but no explicit render command queue abstraction; submission is mutex-serialized
- **Recommendation**: Abstract render command submission into a typed ring buffer between game thread and render thread. Add a `RunOnRenderThread()` mechanism for one-off operations (resource creation, debug visualization).
- **Effort**: High — architectural change to threading model

#### 4. Multi-ISA Function Dispatch for Hot Paths
- **PCSX2**: `MultiISA.h` compiles hot functions three times (SSE4, AVX, AVX2) into separate namespaces. At startup, `g_cpu.vectorISA` selects optimal path via `MULTI_ISA_SELECT(fn)`. Zero runtime branching in inner loops.
- **SparkEngine status**: MISSING — uses DirectXMath only, no per-CPU dispatch
- **Recommendation**: Apply to CPU-bound hot paths: physics broadphase, frustum culling, particle updates, audio mixing. Requires CMake integration to compile source files with different `-march` flags.
- **Effort**: Medium — needs build system changes + identifying hot paths

#### 5. Per-Subsystem Freeze Architecture for Save System
- **PCSX2**: Each hardware module owns a `freeze()` function. Master `FreezeInternals()` calls them in sequence. Each subsystem evolves its save format independently.
- **SparkEngine status**: PARTIAL — SaveSystem serializes ECS components via ComponentSerializerRegistry, but non-ECS subsystems (audio state, physics config, AI state) don't participate
- **Recommendation**: Register every subsystem with the save system via a `IFreezable` interface. Each implements `Freeze()`/`Thaw()`. Orchestrator inserts validation tags between sections.
- **Effort**: Low — interface + registration pattern

### MEDIUM Priority (5)

#### 6. Constant Buffer Content Diff
- **PCSX2**: D3D11 backend caches last-uploaded CB state (`m_vs_cb_cache`, `m_ps_cb_cache`). memcmp before `UpdateSubresource()`, skip if unchanged.
- **SparkEngine status**: EXISTS (dirty flags for state categories) but no per-CB content diff
- **Recommendation**: Add memcmp of constant buffer contents before upload. Most per-object CBs are identical across frames for static objects.
- **Effort**: Low

#### 7. Dirty Rectangle Tracking for Texture Updates
- **PCSX2**: `GSDirtyRect`/`GSDirtyRectList` track modified sub-regions of render targets. Only dirty rectangles are re-uploaded.
- **SparkEngine status**: MISSING for textures (dirty flags exist for pipeline state only)
- **Recommendation**: Apply to terrain heightmap/splatmap edits, UI atlases, and decal updates. Upload only modified sub-rectangles.
- **Effort**: Low-Medium

#### 8. GSRingHeap: Lock-Free Ring Allocator
- **PCSX2**: Single-writer/multi-reader ring allocator with `memory_order_relaxed` atomics. Allocations include size prefix for O(1) dealloc. `SharedPtr`/`UniquePtr` wrappers.
- **SparkEngine status**: EXISTS (FrameAllocator for per-frame temp data) but no lock-free variant for multi-threaded use
- **Recommendation**: Add a lock-free ring allocator for render command data produced by game thread, consumed by render thread. The existing FrameAllocator is single-threaded.
- **Effort**: Medium

#### 9. Per-Scene Configuration Database
- **PCSX2**: `GameDatabase` stores per-game overrides (rendering fixes, CPU modes, speed hacks) keyed by serial. Applied at load time.
- **SparkEngine status**: PARTIAL — SceneMetadata exists with per-node properties, but no centralized override database
- **Recommendation**: Create a scene config database mapping scene IDs to render/physics/audio overrides. Different levels can specify shadow quality, draw distance, physics sub-steps, reverb presets.
- **Effort**: Low

#### 10. GPU Performance Counter Categories
- **PCSX2**: `GSPerfMon` tracks categorized counters: draw calls, primitives, readbacks, fillrate, render passes, barriers, sync points. Frame-based aggregation with delta monitoring.
- **SparkEngine status**: EXISTS (Profiler with ProfileCategory enum) but limited GPU-specific counters
- **Recommendation**: Add GPU counter categories to existing profiler: draw calls, state changes, texture uploads, barrier count, render pass count. Display in editor performance panel.
- **Effort**: Low

### LOW Priority (2)

#### 11. WorkSema: Spin-Before-Sleep Synchronization
- **PCSX2**: `WorkSema` spins briefly before kernel sleep transition. Manages worker states (spinning/sleeping/running) via atomics. Reduces synchronization overhead for high-frequency wake/sleep.
- **SparkEngine status**: MISSING — uses std::condition_variable
- **Recommendation**: Replace condition variables in job system and coroutine scheduler hot paths with spin-before-sleep semaphores.
- **Effort**: Medium — subtle threading primitive

#### 12. Aligned Heap Arrays with SIMD Alignment
- **PCSX2**: `HeapArray.h` provides `FixedHeapArray<T, SIZE, ALIGNMENT>` and `DynamicHeapArray<T, ALIGNMENT>` with platform-specific aligned allocation, move semantics, and trivially_copyable constraints.
- **Recommendation**: Use for SIMD-processed particle buffers, physics collision data, and audio sample buffers. Cleaner than manual aligned allocation.
- **Effort**: Low — header-only utility

## Systems Already Covered by SparkEngine

These PCSX2 features were analyzed but SparkEngine already has equivalent or better implementations:

| PCSX2 System | SparkEngine Equivalent | Status |
|-------------|----------------------|--------|
| Selector-based PSO caching | PipelineStateCache (FNV-1a hash + dirty flags) | EXISTS |
| Texture pool with age-based recycling | RenderTargetPool (age-based GC at 60 idle frames) | EXISTS |
| Stream buffer ring pattern | ConstantBufferRing (bump-allocate + discard) | EXISTS |
| D3D12 descriptor heap management | DescriptorHeapAllocator (free-list + mutex) | EXISTS |
| GPU capability detection | RHIDeviceCapabilities struct | EXISTS |
| LRU cache | AssetCache::EvictLRU() + TextureSystem LRU | EXISTS |
| ADSR audio envelopes | XAudio2 handles natively | DELEGATED |
| Reverb processing | AudioMixer with reverb zones + presets | EXISTS |
| Input recording/replay | ReplaySystem (entity-state based) | EXISTS |
| Controller abstraction | InputManager + GamepadInput | EXISTS |

## Summary

12 recommendations total: 5 HIGH, 5 MEDIUM, 2 LOW. Key gaps are in save system robustness (tag validation, per-subsystem freeze), headless testing (null RHI), render thread decoupling (command ring buffer), and CPU optimization (multi-ISA dispatch). Most HIGH items are LOW-MEDIUM effort.

## Notes

- PCSX2 is an emulator, not a game engine. Many of its core patterns (JIT recompilation, cycle-accurate timing, virtual memory emulation) are not applicable.
- The patterns adopted here are from PCSX2's **common utilities and graphics abstraction layers**, which face similar engineering challenges to game engines.
- The save system improvements (items 1 + 5) should be implemented together as they're complementary.
- The null RHI backend (item 2) would immediately improve CI coverage by enabling render graph tests on Linux CI runners.
