# SparkEngine GPU-to-CPU Offloading — Gap Analysis

> **Scope**: `SparkEngine/Source/Graphics/`, `SparkEngine/Source/Core/`, `SparkEngine/Source/Engine/ECS/`
> **Date**: 2026-03-09
> **Methodology**: Static source-code audit of all rendering, resource management, and memory tracking systems, combined with industry research on GPU-to-CPU offloading techniques (unified memory, dynamic load balancing, hybrid rendering, VRAM budget management, WARP software rasterization).
> Each gap is assigned a severity: **Critical** (blocks offloading correctness), **Major** (significant missing feature), **Moderate** (partial implementation), **Minor** (polish / optimization).

---

## Context

**What is GPU-to-CPU offloading?** When a GPU's dedicated video memory (VRAM) is exhausted or its processing capacity becomes a bottleneck, work must be redistributed — either by moving data to system RAM (memory offloading), shifting rendering or compute tasks to the CPU (compute offloading), or dynamically reducing GPU workload through quality scaling. This is critical for:

- Running on hardware with limited VRAM (integrated GPUs, older discrete cards)
- Handling large open-world scenes that exceed VRAM budgets
- Maintaining stable frame rates under variable GPU load
- Graceful degradation instead of crashes when VRAM is exhausted

**Current SparkEngine state**: The engine has no GPU-to-CPU offloading capability. There is a basic VRAM usage counter (`Console_GetVRAMUsage`), a texture memory budget in `TextureSystem` (512 MB default), and quality presets in `GraphicsSettings` — but none of these systems are connected to form a reactive offloading pipeline. The engine will crash or produce undefined behavior if GPU memory is exhausted.

---

## Critical Gaps

### GAP-OFF01 — No VRAM Budget Monitoring or Pressure Detection System

**Files**:
- `Graphics/GraphicsEngine.h` (lines 200-203: `RenderStatistics` tracks `totalGPUMemory` but never queries actual hardware)
- `Graphics/GraphicsEngine.cpp` (lines 2479-2500: `Console_GetVRAMUsage()` sums tracked allocations only)
- `Graphics/RHI/RHITypes.h` (lines 481-482: `dedicatedVideoMemory` and `sharedSystemMemory` are read once at init)
- `Graphics/RHI/D3D11/D3D11Device.cpp` (line 540: adapter memory queried at creation, never refreshed)

**Impact**: The engine cannot detect when VRAM is running low. Without real-time VRAM pressure detection, no offloading, quality reduction, or resource eviction can be triggered proactively. The `Console_GetVRAMUsage()` method only sums internally tracked allocations (`m_textureMemoryUsage + m_bufferMemoryUsage`) — it does not query the DXGI adapter for actual current VRAM consumption, which includes driver overhead, implicit resources, and allocations from other processes.

**Evidence**:
```cpp
// GraphicsEngine.cpp:2479 — only sums internal tracking, not actual VRAM
size_t GraphicsEngine::Console_GetVRAMUsage() const
{
    LOG_TO_CONSOLE_IMMEDIATE(L"Retrieving VRAM usage via console", L"INFO");
    // Calculate total VRAM usage from tracked memory
    ...
}

// RHITypes.h:481 — static, never updated after init
uint64_t dedicatedVideoMemory = 0;
uint64_t sharedSystemMemory = 0;
```

**What is needed**:
1. **DXGI memory query**: Use `IDXGIAdapter3::QueryVideoMemoryInfo()` (available on DXGI 1.4+/Windows 10) to poll real-time VRAM usage per frame or at configurable intervals. This returns `CurrentUsage`, `Budget`, and `AvailableForReservation` for both local (dedicated VRAM) and non-local (shared system RAM) memory segments.
2. **Memory pressure thresholds**: Define configurable thresholds (e.g., 75% warning, 85% critical, 95% emergency) that trigger escalating offloading responses.
3. **Per-resource memory tracking**: Tag every GPU allocation (textures, buffers, render targets, constant buffers) with size and priority metadata so the system knows what can be evicted.
4. **Memory pressure event system**: Broadcast memory pressure events through `EngineContext` so all subsystems (TextureSystem, MaterialSystem, ParticleSystem, ShadowAtlas) can respond independently.

---

### GAP-OFF02 — No Resource Residency Management or Eviction Policy

**Files**:
- `Graphics/TextureSystem.h` (lines 235-238: `SetMemoryBudget`/`GetMemoryUsage`/`GarbageCollect` declared)
- `Graphics/TextureSystem.cpp` (lines 445-449: budget check triggers GC but with no priority or LRU logic)
- `Graphics/GraphicsEngine.h` (line 456: `Console_ForceGarbageCollection` is the only eviction trigger)

**Impact**: When the texture memory budget is exceeded, `TextureSystem::GarbageCollect()` is called, but there is no residency management system. The engine cannot: (a) track which resources are currently needed by the GPU vs. merely cached, (b) evict resources by priority or recency, (c) move resources between VRAM and system RAM, or (d) reload evicted resources on demand. Without this, the engine either keeps everything in VRAM (risking OOM) or unloads textures permanently (causing visual artifacts).

**Evidence**:
```cpp
// TextureSystem.cpp:441 — simplistic budget check, no priority eviction
void TextureSystem::Update(float deltaTime)
{
    UpdateMetrics();
    if (GetMemoryUsage() > m_memoryBudget)
    {
        GarbageCollect();  // No LRU, no priority, no partial eviction
    }
}
```

**What is needed**:
1. **Resource residency states**: Each GPU resource should have a residency state: `Resident` (in VRAM), `Evicted` (data in system RAM, GPU handle released), `Streaming` (transfer in progress), `Pinned` (never evict — e.g., render targets, G-buffers).
2. **LRU eviction with priority weighting**: Track last-used frame number per resource. When memory pressure occurs, evict lowest-priority, least-recently-used resources first. Priority should account for: screen coverage, distance to camera, resource type (render targets > shadow maps > scene textures > distant textures).
3. **System RAM staging pool**: Maintain a CPU-side staging pool where evicted texture/buffer data is retained in system RAM. This enables fast re-upload when a resource is needed again, avoiding disk I/O.
4. **Async re-upload pipeline**: When an evicted resource is needed, queue an async upload via `ID3D11DeviceContext::UpdateSubresource` or a staging buffer copy, using a priority queue to avoid upload storms.

---

### GAP-OFF03 — No Dynamic Quality Scaling in Response to GPU Load

**Files**:
- `Graphics/GraphicsEngine.h` (lines 72-80: `QualityPreset` enum exists but is static)
- `Graphics/GraphicsEngine.h` (lines 136-174: `GraphicsSettings` has `renderScale` but it is never auto-adjusted)
- `Graphics/UpscalingSystem.h` (11 stub returns — DLSS/FSR integration is entirely empty)
- `Graphics/MeshLOD.h` (2 stubs), `Graphics/MeshLOD.cpp` (2 stubs)

**Impact**: The engine has no mechanism to dynamically reduce rendering quality when the GPU is under pressure. `QualityPreset` and `renderScale` exist but are only changed manually by the user. There is no frame-time feedback loop that adjusts resolution, shadow quality, LOD bias, texture quality, or post-processing intensity in response to measured GPU performance. This means the engine either runs at the configured quality (potentially dropping frames) or requires manual user intervention.

**Evidence**:
```cpp
// GraphicsSettings — renderScale exists but is never auto-modified
float renderScale = 1.0f;

// UpscalingSystem.h — 11 stubs, no working upscaling
// MeshLOD — basic LOD with 2 stubs, no distance-bias override
```

**What is needed**:
1. **Dynamic Resolution Scaling (DRS)**: Implement a frame-time-based feedback controller that adjusts `renderScale` between a configured min (e.g., 0.5) and max (1.0). When GPU frame time exceeds target (e.g., 16.67ms for 60fps), reduce scale; when consistently below, increase. Use exponential smoothing to avoid oscillation.
2. **Quality tier cascade**: Define automatic quality reduction steps: (1) reduce shadow map resolution, (2) reduce texture mip bias, (3) disable optional post-processing (bloom, SSAO, motion blur), (4) reduce render scale, (5) reduce LOD bias to force simpler meshes, (6) reduce particle counts.
3. **GPU timing feedback**: Use the existing `ID3D11Query` timestamp queries (already declared in `GraphicsEngine.h` lines 579-585) to measure actual GPU frame time and drive the quality scaling decisions.

---

## Major Gaps

### GAP-OFF04 — No CPU-Side Software Rendering Fallback

**Files**:
- `Graphics/RHI/RHIFactory.cpp` (line 66: backend priority is D3D11 → Vulkan → OpenGL, no software path)
- No WARP adapter selection code exists anywhere in the codebase

**Impact**: DirectX 11 includes the Windows Advanced Rasterization Platform (WARP), a high-performance software rasterizer that runs on the CPU. It supports feature levels 9_1 through 11_1 and can serve as a fallback when no GPU is available or when GPU resources are critically exhausted. SparkEngine never attempts to create a WARP device, meaning there is no CPU rendering fallback for headless testing, CI/CD pipeline rendering, or extreme VRAM pressure scenarios.

**Evidence**:
```cpp
// RHIFactory.cpp:66 — no software/WARP backend in priority list
// Priority: D3D11 on Windows, Vulkan on Linux, OpenGL as fallback
```

No references to `D3D_DRIVER_TYPE_WARP` or `DXGI_ADAPTER_FLAG_SOFTWARE` exist in the codebase.

**What is needed**:
1. **WARP device creation path**: Add `D3D_DRIVER_TYPE_WARP` as a fallback in `CreateDevice()`. When hardware device creation fails, or when explicitly requested (e.g., `--software-renderer` CLI flag), create a WARP device.
2. **Hybrid device strategy**: In extreme VRAM pressure scenarios, the engine could create a secondary WARP device for specific low-priority workloads (shadow map rendering, reflection probes, thumbnail generation) while the primary hardware device handles the main scene.
3. **CI/headless rendering**: Enable WARP-based rendering for automated testing, screenshot tests, and CI/CD pipelines where no GPU is available.

---

### GAP-OFF05 — No Texture Mip-Level Streaming Based on VRAM Pressure

**Files**:
- `Graphics/TextureSystem.h` (lines 241-242: `EnableStreaming`/`IsStreamingEnabled` exist but are boolean toggles only)
- `Graphics/TextureSystem.cpp` (15 stub returns in streaming-related methods)
- `Graphics/TextureSystem.h` (line 271: `m_memoryBudget = 512 * 1024 * 1024`)

**Impact**: Modern engines (Unreal Engine, Unity, V-Ray GPU) manage VRAM pressure primarily through **mip-level streaming** — loading only the mip levels needed for the current view (based on screen-space UV derivatives and camera distance), and evicting higher-resolution mips when VRAM is scarce. SparkEngine declares texture streaming infrastructure (`StreamingRequest`, `StreamingThreadFunction`, `m_streamingQueue`) but the streaming logic is stubbed. Textures are loaded at full resolution or not at all — there is no partial-mip residency.

**Evidence**:
```cpp
// TextureSystem.h — streaming infrastructure declared but stubbed
struct StreamingRequest
{
    std::string filePath;
    TextureDesc desc;
    std::function<void(std::shared_ptr<Texture>)> callback;
    int priority = 0;
    bool urgent = false;
};

// 15 stub returns in TextureSystem.cpp for streaming methods
```

**What is needed**:
1. **Per-texture mip residency**: Track which mip levels are resident in VRAM vs. available on disk/system RAM. Use `ID3D11Texture2D` with `D3D11_RESOURCE_MISC_TILED` (if available) or manage mip chains manually via staging textures.
2. **View-dependent mip selection**: Each frame, compute the required mip level per texture based on screen-space coverage (distance to camera, UV density, render resolution). Only request mips that are actually needed.
3. **Budget-aware streaming**: When VRAM is under pressure, bias mip requests toward lower resolution (higher mip levels). When VRAM is abundant, stream in higher-resolution mips for nearby/important textures.
4. **Async mip upload**: Use a dedicated upload thread and staging buffers to transfer mip data to the GPU without stalling the render thread.

---

### GAP-OFF06 — No Compute Work Distribution Between GPU and CPU

**Files**:
- `Graphics/OcclusionCulling.h` (20 stubs — GPU-driven culling path is empty)
- `Graphics/GPUParticleSystem.h` (2 stubs — GPU particle compute is empty)
- `Graphics/ForwardPlusLightCulling.h` (18 stubs — light culling compute is empty)
- `Graphics/ScreenSpaceEffectsGPU.h` (13 stubs — GPU compute effects are empty)

**Impact**: The engine declares both CPU and GPU paths for several compute workloads (occlusion culling, particles, light culling, screen-space effects) but only CPU paths have any implementation, and the GPU compute paths are entirely stubbed. There is no mechanism to dynamically route compute work between CPU and GPU based on current load. When the GPU is saturated (e.g., complex scene with many lights), compute tasks like light culling and occlusion culling should be offloadable to the CPU — and vice versa, when the CPU is the bottleneck, these tasks should shift to the GPU.

**Evidence**:
- `OcclusionCulling.h:590`: Fallback comment "all visible" — no actual occlusion test
- `ForwardPlusLightCulling.h`: 18 stubs, compute shader paths return immediately
- `GPUParticleSystem.h:595`: Comment references "optional fallback" but CPU path is also minimal

**What is needed**:
1. **Dual-path compute architecture**: For each compute workload (culling, particles, light tiling), implement both a CPU path and a GPU compute shader path, with a runtime switch.
2. **Load-based routing**: Monitor GPU vs. CPU utilization each frame. When GPU compute queue time exceeds a threshold, route overflow tasks to CPU worker threads. When CPU is bottlenecked, prefer GPU compute.
3. **Work-item granularity**: Design compute tasks with a consistent work-item interface so they can be dispatched to either processor without changing the algorithm semantics. Use a task-graph abstraction.

---

### GAP-OFF07 — No Shared Memory / Unified Memory Architecture Support

**Files**:
- `Graphics/RHI/RHITypes.h` (line 482: `sharedSystemMemory` is stored but never used)
- `Graphics/RHI/D3D11/D3D11Device.cpp` (line 540: shared memory queried from adapter but not leveraged)

**Impact**: The `RHIDeviceCapabilities` struct captures `sharedSystemMemory` (the portion of system RAM accessible to the GPU via the PCI-E bus), but this value is never used for any decision. On integrated GPUs (Intel UHD, AMD APUs) and systems with shared memory architectures, this memory is a significant resource. On discrete GPUs, DXGI can allocate from shared system memory when dedicated VRAM is exhausted, but this is orders of magnitude slower. The engine makes no distinction between dedicated and shared memory, cannot leverage shared memory for staging/streaming, and does not warn when falling back to shared memory.

**Evidence**:
```cpp
// RHITypes.h:482 — stored, never read anywhere else
uint64_t sharedSystemMemory = 0;

// No code path reads sharedSystemMemory after device creation
```

**What is needed**:
1. **Integrated GPU detection**: At startup, detect whether the adapter is integrated (shared memory architecture) vs. discrete. On integrated GPUs, the memory budget should be a percentage of `sharedSystemMemory` rather than `dedicatedVideoMemory`.
2. **Shared memory tier**: Add a `SharedMemory` residency tier between `Resident` (VRAM) and `Evicted` (CPU-only). Resources in the shared tier are accessible by the GPU but at reduced bandwidth — suitable for infrequently accessed textures, LOD meshes, and staging buffers.
3. **PCI-E bandwidth awareness**: When resources are accessed from shared memory on discrete GPUs, the PCI-E bus becomes the bottleneck (~16 GB/s for PCIe 3.0 x16 vs. ~400+ GB/s VRAM bandwidth). The engine should track shared memory usage and log performance warnings.

---

## Moderate Gaps

### GAP-OFF08 — No Shadow Map Quality Reduction Under Pressure

**Files**:
- `Graphics/ShadowAtlas.h` (15 stubs)
- `Graphics/GraphicsSettings` (lines 151-153: fixed `shadowMapSize` and `cascadeCount`)

**Impact**: Shadow maps are often the largest consumers of VRAM after textures (a single 4096x4096 shadow cascade at 32-bit depth = 64 MB, and cascaded shadow maps multiply this). The engine has no mechanism to reduce shadow resolution, cascade count, or update frequency in response to VRAM pressure. The `ShadowAtlas` design includes a priority-based allocation concept, but it is entirely stubbed.

**What is needed**:
1. **Dynamic shadow resolution**: Reduce shadow map resolution by 50% when VRAM pressure exceeds the warning threshold. Halving each cascade from 2048 to 1024 reduces shadow VRAM by 75%.
2. **Cascade count reduction**: Drop from 4 cascades to 2 under extreme pressure, with a larger far-plane on the remaining cascades to maintain coverage.
3. **Shadow update frequency**: Skip shadow map updates for static lights when under pressure. Only update shadow maps for the highest-priority light (typically the directional sun).
4. **Shadow atlas pooling**: Implement the `ShadowAtlas` tile system so multiple lights share a single atlas texture, reducing total shadow VRAM allocation.

---

### GAP-OFF09 — No Render Target Downscaling or Format Reduction

**Files**:
- `Graphics/GraphicsEngine.h` (lines 534-541: G-buffer textures are fixed 4x RGBA)
- `Graphics/RenderTarget.h` / `Graphics/RenderTarget.cpp` (27 stubs)

**Impact**: G-buffer textures, HDR buffers, and post-processing intermediate targets consume significant VRAM (4 full-screen RGBA16F G-buffer targets at 1920x1080 = ~63 MB, scaling quadratically with resolution). There is no mechanism to: (a) downscale intermediate render targets when resolution changes, (b) use lower-precision formats under pressure (e.g., R11G11B10F instead of RGBA16F), or (c) share render targets between passes that don't overlap in time (aliasing).

**What is needed**:
1. **Render target aliasing**: Implement a render target pool where non-overlapping passes share the same memory. Shadow maps, SSAO buffers, and post-process scratch targets can alias the same physical memory.
2. **Format downgrade**: Under VRAM pressure, switch G-buffer normal format from RGBA16F to R10G10B10A2, and HDR buffer from RGBA16F to R11G11B10F.
3. **Resolution-linked scaling**: Render targets should automatically resize when `renderScale` changes, rather than remaining at full resolution.

---

### GAP-OFF10 — No CPU-Side Culling Offload for Large Scenes

**Files**:
- `Graphics/FrustumCulling.h` (1 stub — basic frustum culling only)
- `Graphics/OcclusionCulling.h` (20 stubs — GPU path entirely empty)
- No spatial acceleration structure (BVH, octree) exists

**Impact**: For large scenes (thousands of objects), culling is a significant per-frame cost. The engine has basic frustum culling but no spatial acceleration structure, and the GPU occlusion culling path is empty. Without a BVH or octree, frustum culling is O(n) per object. When the GPU is under heavy rendering load, it would be beneficial to perform aggressive CPU-side culling (frustum + software occlusion) to reduce the draw call count submitted to the GPU — effectively offloading the GPU by reducing its workload via CPU pre-processing.

**What is needed**:
1. **BVH/octree spatial structure**: Build a spatial acceleration structure for the scene on the CPU. Update it incrementally when objects move.
2. **CPU software occlusion culling**: Rasterize a low-resolution depth buffer on the CPU (using simplified occluder geometry) and test object bounding boxes against it. This is a proven technique (used in Frostbite, Unreal) that reduces GPU draw calls by 40-60% in complex scenes.
3. **Parallel CPU culling**: Distribute culling work across CPU worker threads using a job system. Each thread processes a spatial partition independently.

---

### GAP-OFF11 — No Frame Pacing or Latency Management for Offloaded Work

**Files**:
- `Graphics/GraphicsEngine.h` (lines 573-585: timing infrastructure exists but is passive)
- No frame budget controller exists

**Impact**: When work is offloaded from GPU to CPU (or vice versa), frame pacing becomes critical. CPU work added to the frame must complete before the GPU needs the results, or the GPU will stall. Conversely, GPU compute results needed by the CPU must be ready before the CPU's next frame logic. The engine has timestamp queries for measurement but no frame budget controller that allocates time slices to CPU vs. GPU work and ensures neither processor is waiting on the other.

**What is needed**:
1. **Frame budget allocator**: Define per-frame time budgets for CPU and GPU work. Example: at 60fps (16.67ms budget), allocate 8ms CPU, 12ms GPU, 2ms overhead. Adjust based on measured timings.
2. **Async compute overlap**: Submit GPU compute work (culling, particles, lighting) early in the frame so it overlaps with CPU game logic. Read back results later in the same frame.
3. **Triple-buffered resource updates**: Use triple buffering for resources that are written by CPU and read by GPU (constant buffers, instance data) to avoid CPU-GPU synchronization stalls.

---

## Minor Gaps

### GAP-OFF12 — No Profiler Integration for Offloading Decisions

**Files**:
- `Utils/Profiler.h` (exists but does not track CPU vs. GPU breakdown per subsystem)
- `Graphics/GraphicsEngine.h` (lines 173: `enableGPUTiming` exists but is off by default)

**Impact**: Without per-subsystem CPU/GPU timing breakdowns, the offloading system cannot make informed decisions about which work to move and where. The profiler exists but does not categorize work by offloadable subsystem.

**What is needed**:
1. **Subsystem timing tags**: Tag each render pass, compute dispatch, and CPU task with a subsystem identifier (shadows, culling, particles, lighting, post-processing). Record CPU and GPU time separately.
2. **Offloading telemetry**: Log when offloading decisions are made (what was moved, from where to where, time saved, time cost). Surface in the editor ImGui overlay.

---

### GAP-OFF13 — No Memory Defragmentation for GPU Resources

**Files**: No defragmentation logic exists in any Graphics/ file.

**Impact**: Over time, as GPU resources are allocated and freed, VRAM can become fragmented. While the D3D11 runtime and driver handle most VRAM defragmentation internally, the engine's own resource pools (render target cache, texture cache, staging buffers) can benefit from periodic compaction. Fragmented resource pools lead to allocation failures even when total free memory is sufficient.

**What is needed**:
1. **Resource pool compaction**: Periodically consolidate free blocks in CPU-managed resource pools (staging buffers, upload heaps).
2. **Allocation strategy**: Use a buddy allocator or slab allocator for staging buffers and constant buffer pools to minimize fragmentation.

---

### GAP-OFF14 — No User-Facing Configuration for Offloading Behavior

**Files**:
- `Graphics/GraphicsEngine.h` (lines 136-174: `GraphicsSettings` has no offloading options)
- No editor UI for memory/offloading configuration

**Impact**: Even once offloading is implemented, users need control over its behavior: VRAM budget overrides, minimum quality floors, offloading aggressiveness, and the ability to pin specific resources in VRAM. The `GraphicsSettings` struct and editor UI have no fields for this.

**What is needed**:
1. **Offloading settings struct**: Add fields to `GraphicsSettings`: `vramBudgetOverrideMB`, `enableDynamicResolution`, `minRenderScale`, `enableCPUFallback`, `offloadingAggressiveness` (0.0-1.0).
2. **Editor panel**: Add an ImGui panel showing real-time VRAM usage, residency states, offloading activity, and manual controls.
3. **Console commands**: Register commands like `gfx_vram_budget`, `gfx_offload_mode`, `gfx_min_quality` via `GraphicsConsoleCommands.cpp`.

---

## Summary Table

| ID | Severity | Subsystem | Impact |
|---|---|---|---|
| GAP-OFF01 | Critical | VRAM Monitoring | No real-time VRAM pressure detection |
| GAP-OFF02 | Critical | Resource Residency | No eviction policy, no VRAM↔RAM movement |
| GAP-OFF03 | Critical | Dynamic Quality | No auto-scaling of quality under GPU load |
| GAP-OFF04 | Major | Software Fallback | No WARP/CPU rendering path |
| GAP-OFF05 | Major | Texture Streaming | No mip-level streaming for VRAM pressure |
| GAP-OFF06 | Major | Compute Distribution | No CPU↔GPU compute work routing |
| GAP-OFF07 | Major | Shared Memory | Shared system memory detected but unused |
| GAP-OFF08 | Moderate | Shadow Quality | Fixed shadow resolution, no pressure response |
| GAP-OFF09 | Moderate | Render Targets | No RT aliasing, format downgrade, or auto-resize |
| GAP-OFF10 | Moderate | CPU Culling | No spatial acceleration or software occlusion |
| GAP-OFF11 | Moderate | Frame Pacing | No frame budget controller for CPU/GPU overlap |
| GAP-OFF12 | Minor | Profiling | No per-subsystem CPU/GPU timing for offload decisions |
| GAP-OFF13 | Minor | Defragmentation | No resource pool compaction |
| GAP-OFF14 | Minor | Configuration | No user-facing offloading settings or UI |

---

## Aggregate Statistics

| Metric | Value |
|---|---|
| Total gaps identified | 14 |
| Critical | 3 |
| Major | 4 |
| Moderate | 4 |
| Minor | 3 |
| Existing VRAM tracking code | ~30 lines (passive, no decision-making) |
| Existing memory budget code | TextureSystem only (512 MB default, no enforcement) |
| Existing quality presets | 5 (Low/Medium/High/Ultra/Custom — static only) |
| GPU compute paths declared | 4 (occlusion, particles, light culling, screen-space) |
| GPU compute paths functional | 0 |
| WARP/software rendering support | None |
| Dynamic resolution support | None |
| Mip-level streaming support | Declared, 15 stubs |

---

## Recommended Priority Order

1. **GAP-OFF01** — VRAM budget monitoring (foundation: everything else depends on knowing current memory state)
2. **GAP-OFF02** — Resource residency management (enables proactive eviction before OOM)
3. **GAP-OFF03** — Dynamic quality scaling (immediate user-facing improvement; reduces GPU load without code-level offloading)
4. **GAP-OFF05** — Texture mip streaming (textures are the #1 VRAM consumer; partial mip residency is the most effective VRAM reduction technique)
5. **GAP-OFF08 + GAP-OFF09** — Shadow and render target optimization (large fixed-cost VRAM consumers that can be reduced under pressure)
6. **GAP-OFF10** — CPU culling with spatial acceleration (reduces GPU draw call load; the most practical "offload" for immediate performance gains)
7. **GAP-OFF06** — Compute work distribution (requires both CPU and GPU paths to be functional; depends on GAP-G07/G08 from the Graphics gap analysis)
8. **GAP-OFF07** — Shared memory utilization (important for integrated GPU support)
9. **GAP-OFF11** — Frame pacing (needed once actual offloading is in place)
10. **GAP-OFF04** — WARP fallback (niche use case but valuable for CI/headless)
11. **GAP-OFF14** — User configuration (needed once features exist to configure)
12. **GAP-OFF12 + GAP-OFF13** — Profiling and defragmentation (optimization polish)

---

## Architectural Recommendation

The GPU-to-CPU offloading system should be implemented as a new top-level subsystem: `ResourceBudgetManager` (or `MemoryOrchestrator`), registered in `EngineContext` alongside the existing service locator pattern. This manager would:

1. **Poll** DXGI for real-time VRAM usage each frame via `IDXGIAdapter3::QueryVideoMemoryInfo()`
2. **Evaluate** memory pressure against configurable thresholds
3. **Broadcast** pressure events to all registered subsystems (TextureSystem, ShadowAtlas, RenderTarget pool, ParticleSystem)
4. **Coordinate** the quality cascade: resolution scaling → shadow reduction → texture mip eviction → post-process disabling → LOD bias → render target format downgrade
5. **Log** all offloading decisions for profiling and debugging

This centralized approach prevents subsystems from independently reacting to VRAM pressure in conflicting ways and enables a smooth, predictable degradation curve.

```
┌─────────────────────────────────────────────────────────┐
│                  ResourceBudgetManager                   │
│  ┌──────────┐  ┌──────────────┐  ┌──────────────────┐   │
│  │  DXGI    │  │  Pressure    │  │  Quality         │   │
│  │  Memory  │→ │  Evaluator   │→ │  Cascade         │   │
│  │  Query   │  │  (thresholds)│  │  Controller      │   │
│  └──────────┘  └──────────────┘  └────────┬─────────┘   │
│                                           │              │
│  ┌────────────────────────────────────────┼──────────┐   │
│  │           Event Broadcast              ▼          │   │
│  │  ┌──────────┬──────────┬──────────┬──────────┐    │   │
│  │  │ Texture  │ Shadow   │ Render   │ Post-    │    │   │
│  │  │ System   │ Atlas    │ Target   │ Process  │    │   │
│  │  │ (mip     │ (res     │ (format  │ (disable │    │   │
│  │  │  evict)  │  reduce) │  downgrade│ passes) │    │   │
│  │  └──────────┴──────────┴──────────┴──────────┘    │   │
│  └───────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

### Thread Safety Considerations

- DXGI memory queries must run on the thread that created the device (main thread)
- Resource eviction and re-upload should use the existing `TextureSystem` streaming threads
- Quality cascade changes must be applied on the main render thread
- Event broadcast should use a thread-safe observer pattern (consistent with existing `NetworkManager` queue mutex pattern)

### Dependencies on Existing Gap Analyses

| This Gap | Depends On | Reason |
|---|---|---|
| GAP-OFF05 (mip streaming) | GAP-G09 (TextureSystem streaming stubs) | Streaming infrastructure must be functional |
| GAP-OFF06 (compute distribution) | GAP-G07, GAP-G08 (screen-space & light culling stubs) | GPU compute paths must exist to distribute |
| GAP-OFF08 (shadow quality) | GAP-G05 (shadow system stubs) | Shadow rendering must work before it can be scaled |
| GAP-OFF09 (render targets) | GAP-G10 (render target stubs) | RT management must work before aliasing/downgrade |
| GAP-OFF10 (CPU culling) | GAP-G18 (frustum culling minimal) | Basic culling must work before advanced offloading |
