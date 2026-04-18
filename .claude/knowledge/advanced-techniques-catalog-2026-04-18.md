# Advanced Techniques Catalog — SparkEngine (2026-04-18)

**Type:** Decision + Plan
**Status:** Active — execution roadmap
**Scope:** 34 advanced techniques across 7 subsystems, prioritized by effort/impact

---

## Context

User asked for advanced techniques the engine could benefit from. A three-agent sweep
(rendering, runtime/ECS/physics/AI, networking/streaming/scripting) catalogued what
already exists, what's missing vs. modern AAA engines, and where each new technique
would slot into existing code. This knowledge doc captures the full menu so future
sessions can pick and execute any item without repeating the research.

**Baseline strengths (already shipped):** render graph + transient aliasing, GPU-driven
culling + HiZ, mesh shaders + mesh clusters (Nanite building blocks), DDGI + Adaptive
Probe Volumes, TAA, FSR 1/2, neural inference substrate (inference + training),
SparkSR native temporal upsampler, Jolt physics, EnTT ECS with parallel dependency-aware
executor, snapshot netcode + CSP + lag compensation + AOI, DirectStorage,
WorldOriginSystem rebasing, AngelScript with hot-reload, async DB persistence,
coroutine scheduler, `AIBudgetLimiter` distance-prioritised tick scaling.

The gaps below are real AAA holes, not foundational ones.

---

## Rendering & GPU

### High-impact

1. **Bindless descriptors / buffer device addresses**
   Unblocks GPU-resident scene, cuts descriptor churn, prerequisite for most
   GPU-driven techniques. Hook: `Graphics/RHI/D3D12/`, `Graphics/RHI/Vulkan/`,
   `Graphics/MeshClusterSystem.h`.

2. **Virtual Shadow Maps (VSM)**
   Replaces cascade flicker; near-constant cost regardless of light count.
   Hook: extend `Graphics/CachedShadowAtlas.h` with feedback-buffer page table +
   indirection texture; wire a VSM pass into `Graphics/RenderGraph/`.

3. **Lumen-style multi-bounce radiance cache**
   Dynamic GI without prebake; complements DDGI/APV for far-field.
   Hook: `Graphics/Neural/NeuralRadianceCache.h` (already present) + SDF tracing
   via `Graphics/HybridRT/SDFSceneManager.h`; feedback loop through render graph
   between lighting pass and GI update pass.

4. **Variable Rate Shading (VRS)**
   5–15% free on fill-bound scenes. Hook: `Graphics/PostProcessingPipeline.h`
   (~SetDepthSRV line 305), D3D12 device shading-rate image; build rate map
   from existing motion vectors.

5. **Nanite-style virtualized geometry (finish the job)**
   Building blocks already present (meshlets, DAG LOD, visibility buffer);
   missing piece is streaming + pixel-perfect LOD feedback.
   Hook: `Graphics/MeshClusterSystem.h` + `Graphics/MeshShaderPipeline.h`.

### Secondary

6. **DLSS 3 / FSR 3 frame generation** — add optical-flow pass; reuse motion-vector path in `GBufferPassData`.
7. **Reactive-mask pipeline for FSR 2** — already stubbed in `UpscalingShaders.h`; wire transparent/particle passes to write it.
8. **D3D12 enhanced barriers & work graphs** — after bindless, split-barriers + hierarchical dispatch become natural.
9. **GPU skinning motion-vector output** — extend `Graphics/GPUSkinning.h` (SkinningCS.hlsl) to emit previous-frame positions; fixes skinned TAA/motion blur.
10. **Compute-driven vertex pulling** — required for fully GPU-resident mesh cluster path.

---

## ECS, Memory, Concurrency

11. **Chunked archetype SoA storage**
    Dense component arrays → SIMD-friendly iteration, big cache wins at 100k+ entities.
    Hook: new `ChunkedArchetype` alongside entity layout in `Engine/ECS/`.

12. **SIMD iteration wrapper**
    First targets: transform integration, physics velocity steps, perception
    distance² checks. Hook: new `Engine/ECS/Systems/SIMDIterator.h`, exposed via
    `ParallelSystemExecutor::DeclareVectorized()`.

13. **Per-subsystem frame allocators (TLSF / slab / arena)**
    Prevents AI/physics/render starving each other. Hook: new
    `Utils/SubsystemFrameAllocator.h`, layered on `Utils/FrameAllocator.h`.

14. **Fiber-based job system for long-running async**
    Asset streaming, save/load. Hook: extend `Utils/JobSystem.h` with optional fiber backend.

15. **Lock-free / priority-tiered event queue**
    Hook: `Engine/Events/EventSystem.h::QueuedEventBus`. Replace mutex with
    MoodyCamel or add priority tiers so critical events don't get dropped
    under queue pressure. **Started this session — see Implementation Log.**

---

## AI

16. **HTN + GOAP planners alongside Behavior Trees**
    Hook: new `Engine/AI/HTNPlanner.h`, `Engine/AI/GoalAI.h`; opt in per agent.

17. **RVO2 / velocity-obstacle crowd steering**
    Pairs with NavMesh. Hook: expand `Engine/AI/CollisionAvoidance.h`.

18. **Influence / threat maps**
    Hook: new `Engine/AI/InfluenceMap.h`; diffuse per frame, sample via existing Octree.

19. **LOD-based AI tick scaling** — **EFFECTIVELY DONE** via `Engine/AI/AIBudgetLimiter.h`
    (distance-prioritised queue + `m_maxStaleFrames` starvation bound). Wired in
    `Core/Lifecycle/GameplayLifecycleShared.cpp` via `AIIntegratedSystem` Init/Update/Shutdown.

20. **Neural behavior policies** — route small MLPs from AI decision nodes
    via `Graphics/Neural/NeuralInference.h`.

---

## Physics & Animation

21. **Soft-body / cloth / rope** — extend `Engine/Physics/JoltPhysicsInterface.h` with `ShapeType::SoftMesh`.
22. **Vehicle system** — Jolt vehicle controller + wheel raycasts on same interface.
23. **Motion matching** — `Engine/Animation/` pose database + match-cost solver.
24. **Deterministic fixed-step physics tick** — prerequisite for rollback netcode.

---

## Networking & Streaming

25. **Rollback netcode (GGPO-style)** — extend `ClientPrediction::Reconcile()` with full input-history replay; depends on #24.
26. **Network transform quantization** — 16-bit bbox positions, 48-bit smallest-three quats in `Engine/Networking/ReplicationFields.cpp`; reuse `Engine/Animation/AnimationCompression.h` primitives. Roughly halves transform bandwidth.
27. **Sub-tick input replication** — `Engine/Networking/SubTickInput.h` exists but isn't fed into snapshots.
28. **Virtual texturing with feedback streaming** — new `VirtualTexturePool`, integrate with `Engine/Streaming/DirectStorageLoader`.
29. **Background GDeflate/zstd decompression threads** — CPU fallback pool for non-Windows targets.
30. **LRU asset cache with memory budgets** — wrap `AreaAssetLoader::LoadedData`.

---

## Scripting, Persistence, Save

31. **AngelScript tier-2 JIT for hot paths** — LLVM IR codegen; profile first.
32. **SQLite WAL mode + crash recovery** in `Engine/Persistence/AsyncDatabase`.
33. **Per-component save versioning + migration callbacks** in `Engine/SaveSystem/`.
34. **Chunked streaming save format** — incremental autosave to SSD without hitches.

---

## Priority Ordering

- **Fastest wins (1–2 weeks):** #9 skinning motion vectors, #15 priority event queue, #26 transform quantization, #7 reactive mask, #30 LRU asset cache.
- **High-leverage medium (1–2 months):** #1 bindless, #2 VSM, #4 VRS, #11 chunked archetypes, #12 SIMD iter, #17 RVO2, #25 rollback (needs #24).
- **Flagship multi-month:** #3 Lumen-style GI, #5 finish Nanite, #6 frame gen, #28 virtual texturing.

---

## Pre-Existing Items Evaluated This Session

| Plan doc | Item | Verdict |
|---|---|---|
| `gpu-cpu-separation-plan-2026-04-12.md` Phase 1 | `AssetTypes.cpp` split (est. 1100 lines) | **DONE / obsolete** — file is now 55 lines |
| same Phase 1 | `GPUParticleSystem.cpp` split (est. 528 lines) | **DONE / obsolete** — file is now 15 lines |
| same Phase 1 | `MaterialSystem.cpp` split (est. 869 lines) | **Obsolete** — now 486 lines, under bloat threshold |
| same Phase 1 | `PBRMaterialLighting.cpp` split (est. 838 lines) | **Obsolete** — now 315 lines |
| same Phase 2 | Unguard `DynamicQualityTypes.h` | **DONE** — only the `<d3d11.h>` include is guarded; enums/structs are portable |
| same Phase 2 | Unguard `TemporalEffectsTypes.h` | **DONE** — no guards, fully portable |
| same Phase 2 | Unguard `DrawSortKey.h` | **DONE** — fully portable, namespaced |
| same Phase 3 | Wire GTAO / BVH / ShaderVariant / VCT / Noise / Denoiser | **DONE** via Phases I/L/O/Q/S/T (per index) |
| same Phase 4 | RHI parity for `RHIHandlePool`, `TransientBufferAllocator` | **DONE** via Phases X/Y/Z/AA |
| `reflection-polymorphism-refactoring-plan-2026-04-12.md` Phase 4 | EntityReplicator auto-driven from reflection | **Deferred-as-unnecessary** per plan ("existing `ReplicatedField<T>` is already well-designed") |
| Advanced Technique #19 | AI LOD-based tick scaling | **DONE** via `AIBudgetLimiter` + `AIIntegratedSystem` in `GameplayLifecycleShared.cpp` |

Net: most "pending" items from active plans are already shipped. The plans are kept
active because they capture design rationale, but their Phase-1/2/3/4 todos should no
longer be treated as open work.

---

## Implementation Log (this session)

### Technique #15 — Priority tiers on QueuedEventBus
Extends `Spark::QueuedEventBus` (`Engine/Events/EventSystem.h`) with three
priority tiers (Critical / Normal / Low), separate backing vectors, and
drop-oldest-of-Low-tier-first under MaxQueueSize pressure. Critical events
are never dropped and dispatch first. Backward-compatible: default tier is
`Normal`, existing `QueueEvent<T>(evt)` call sites retain identical semantics.

Tests: added to existing `Tests/TestEventSystem*.cpp` (or new file if no
matching fixture). Coverage: tier ordering on dispatch, Low-tier eviction when
over capacity, Critical immunity to eviction, tier-stats accessors.

---

## Verification

For each implemented item:

1. `cmake --preset linux-gcc-release && cmake --build build/linux-gcc-release --config Release`
2. `./build/linux-gcc-release/bin/SparkTests --gtest_filter=<Prefix>*`
3. Full `./build/linux-gcc-release/bin/SparkTests` — zero regressions
4. `clang-format -i` + `clang-format --dry-run --Werror` on touched files
5. `tools/validate-all.sh --warn-only` before commit
6. Commit on `claude/engine-optimization-ideas-ng6c9`, push with `-u origin`

---

## Next-session pickup

Pick any numbered item above. For each, the plan file
`/root/.claude/plans/give-me-some-advanced-ticklish-cook.md` has identical file-path
hooks. Techniques #7 (reactive mask), #9 (skinning motion vectors), #26 (transform
quantization), and #30 (LRU asset cache) are the best additional "fastest wins"
once #15 lands.
