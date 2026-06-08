# Advanced Techniques Catalog

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (research/reference)
>
> **Platform/Backend Scope:** Cross-engine reference

## Overview

A catalog of **34 advanced techniques across 7 subsystems**, prioritized by effort/impact, produced from a three-agent sweep (rendering; runtime/ECS/physics/AI; networking/streaming/scripting). The sweep catalogued what already exists, what's missing vs. modern AAA engines, and where each new technique would slot into existing code, so any item can be picked up without repeating the research.

**Baseline strengths already shipped** (confirmed 2026-06-08): render graph + transient aliasing, GPU-driven culling + HiZ, mesh shaders + mesh clusters (Nanite building blocks), DDGI + Adaptive Probe Volumes, TAA, FSR 1/2, neural inference substrate, SparkSR temporal upsampler, Jolt physics, EnTT ECS with parallel dependency-aware executor, snapshot netcode + CSP + lag compensation + AOI, DirectStorage, WorldOriginSystem rebasing, AngelScript hot-reload, async DB persistence, coroutine scheduler, `AIBudgetLimiter` distance-prioritised tick scaling.

The gaps below are real AAA holes, not foundational ones. Each item carries a status marker as of the 2026-06-08 verification.

> **Status legend:** **Implemented** — concrete subsystem/file exists. **Partial** — core exists, refinement incomplete. **Open** — not yet built.

## Rendering & GPU

### High-impact
1. **Bindless descriptors / buffer device addresses** — **Partial.** Bindless plumbing exists in `RHI/D3D12/`, `RHI/Vulkan/`, `RHITypes.h`, and `HybridRT/HybridRTManager.h`; full GPU-resident scene path is in progress. Hooks: `Graphics/RHI/D3D12/`, `Graphics/RHI/Vulkan/`, `Graphics/MeshClusterSystem.h`.
2. **Virtual Shadow Maps (VSM)** — **Open.** `CachedShadowAtlas.h` exists but no feedback-buffer page table / indirection texture. Hook: extend `Graphics/CachedShadowAtlas.h`; wire a VSM pass into `Graphics/RenderGraph/`.
3. **Lumen-style multi-bounce radiance cache** — **Partial.** `Graphics/Neural/NeuralRadianceCache.h` is present and SDF tracing exists via `HybridRT/SDFSceneManager.h`; far-field multi-bounce feedback loop not fully wired.
4. **Variable Rate Shading (VRS)** — **Open.** VRS/shading-rate references exist in `GraphicsEngineTypes.h`, `RHITypes.h`, and the D3D12/Vulkan devices as capability flags, but no rate-map build from motion vectors. Hook: `Graphics/PostProcessingPipeline.h`, D3D12 shading-rate image.
5. **Nanite-style virtualized geometry (finish the job)** — **Partial.** Meshlets, DAG LOD, and visibility buffer present (`MeshClusterSystem.h`, `MeshShaderPipeline.{h,cpp}`); streaming + pixel-perfect LOD feedback is the remaining piece.

### Secondary
6. **DLSS 3 / FSR 3 frame generation** — **Open.** Reuse motion-vector path in `GBufferPassData`.
7. **Reactive-mask pipeline for FSR 2** — **Partial / Open.** Stubbed in `UpscalingShaders.h`; transparent/particle passes not yet writing it. *(Flagged in original as a "fastest win".)*
8. **D3D12 enhanced barriers & work graphs** — **Open.** Natural follow-on after bindless.
9. **GPU skinning motion-vector output** — **Open.** `Graphics/GPUSkinning.{h,cpp}` exists but does not emit previous-frame positions; skinned TAA/motion-blur fix still pending. *(Flagged as a "fastest win".)*
10. **Compute-driven vertex pulling** — **Open.** Required for fully GPU-resident mesh-cluster path.

## ECS, Memory, Concurrency
11. **Chunked archetype SoA storage** — **Open.** Hook: new `ChunkedArchetype` in `Engine/ECS/`.
12. **SIMD iteration wrapper** — **Open.** Hook: new `Engine/ECS/Systems/SIMDIterator.h` via `ParallelSystemExecutor::DeclareVectorized()`.
13. **Per-subsystem frame allocators (TLSF / slab / arena)** — **Open.** Hook: new `Utils/SubsystemFrameAllocator.h` over `Utils/FrameAllocator.h`.
14. **Fiber-based job system for long-running async** — **Open.** `Utils/JobSystem.h` exists (thread-pool); optional fiber backend not added.
15. **Lock-free / priority-tiered event queue** — **Implemented.** `Spark::QueuedEventBus` in `Engine/Events/EventSystem.h` now has three priority tiers (`EventPriority::Critical/Normal/Low`): Critical dispatches first and is never evicted before Normal/Low; Low is evicted first under `MaxQueueSize` pressure. Backward-compatible default tier is `Normal`. *(This was the item "started this session" in the original log — confirmed landed.)*

## AI
16. **HTN + GOAP planners alongside Behavior Trees** — **Open.** Behavior Trees exist (`Engine/AI/BehaviorTree*.h`); no `HTNPlanner.h` / `GoalAI.h`.
17. **RVO2 / velocity-obstacle crowd steering** — **Partial.** `Engine/AI/CollisionAvoidance.h` exists and references ORCA/velocity-obstacle concepts; confirm full RVO2 solver depth.
18. **Influence / threat maps** — **Open.** No `Engine/AI/InfluenceMap.h` found.
19. **LOD-based AI tick scaling** — **Implemented.** `Engine/AI/AIBudgetLimiter.{h,cpp}` (distance-prioritised queue + `m_maxStaleFrames` starvation bound), wired in `Core/Lifecycle/GameplayLifecycleShared.cpp` via `AIIntegratedSystem`.
20. **Neural behavior policies** — **Partial.** Neural substrate exists (`Graphics/Neural/NeuralInference.h`); routing small MLPs from AI decision nodes not wired.

## Physics & Animation
21. **Soft-body / cloth / rope** — **Implemented.** `Physics/SoftBodyPhysics.{h,cpp}` and `Physics/ClothSimulation.{h,cpp}` now exist (the original recommended extending the Jolt interface with `ShapeType::SoftMesh`).
22. **Vehicle system** — **Implemented.** `Physics/VehiclePhysics.{h,cpp}`.
23. **Motion matching** — **Open.** No pose-database / match-cost solver in `Engine/Animation/`.
24. **Deterministic fixed-step physics tick** — **Partial.** `Core/FixedTimestepAccumulator` provides the fixed-step loop; confirm physics determinism guarantees for rollback.

## Networking & Streaming
25. **Rollback netcode (GGPO-style)** — **Open.** No rollback path in `Engine/Networking/`; depends on #24.
26. **Network transform quantization** — **Implemented.** `Engine/Networking/NetQuantize.{h,cpp}` (16-bit bbox positions, smallest-three quats), roughly halving transform bandwidth. *(Flagged as a "fastest win" — done.)*
27. **Sub-tick input replication** — **Partial.** `Engine/Networking/SubTickInput.{h,cpp}` exists; confirm it's fed into snapshots.
28. **Virtual texturing with feedback streaming** — **Partial.** `Graphics/VirtualTexture.{h,cpp}` exists; integration with DirectStorage feedback streaming not confirmed.
29. **Background GDeflate/zstd decompression threads** — **Open / unverified.** CPU fallback pool for non-Windows targets.
30. **LRU asset cache with memory budgets** — **Open.** No LRU cache wrapping `AreaAssetLoader::LoadedData` found. *(Flagged as a "fastest win".)*

## Scripting, Persistence, Save
31. **AngelScript tier-2 JIT for hot paths** — **Open.** Profile first.
32. **SQLite WAL mode + crash recovery** — **Open / unverified.** `Engine/Persistence/AsyncDatabase.{h,cpp}` exists; WAL/crash-recovery not grep-confirmed.
33. **Per-component save versioning + migration callbacks** — **Partial.** `Engine/SaveSystem/` has version/migration references (`SaveSystem.{h,cpp}`, `SaveSystemTypes.h`); confirm per-component granularity.
34. **Chunked streaming save format** — **Open.** Incremental autosave-to-SSD without hitches.

## Priority Ordering (original guidance, with current state)

- **Fastest wins (1–2 weeks):** #9 skinning motion vectors *(open)*, #15 priority event queue *(done)*, #26 transform quantization *(done)*, #7 reactive mask *(partial)*, #30 LRU asset cache *(open)*.
- **High-leverage medium (1–2 months):** #1 bindless *(partial)*, #2 VSM *(open)*, #4 VRS *(open)*, #11 chunked archetypes *(open)*, #12 SIMD iter *(open)*, #17 RVO2 *(partial)*, #25 rollback *(open, needs #24)*.
- **Flagship multi-month:** #3 Lumen-style GI *(partial)*, #5 finish Nanite *(partial)*, #6 frame gen *(open)*, #28 virtual texturing *(partial)*.

## Pre-Existing Items Evaluated (from the original session)

The original session checked several active plan-doc items against the codebase and found most already shipped. Key verdicts (retained for design rationale):

| Item | Verdict (original) |
|---|---|
| `AssetTypes.cpp` split | DONE / obsolete (file now ~55 lines) |
| `GPUParticleSystem.cpp` split | DONE / obsolete (~15 lines) |
| `MaterialSystem.cpp` split | Obsolete — under bloat threshold |
| `PBRMaterialLighting.cpp` split | Obsolete — under threshold |
| Unguard `DynamicQualityTypes.h` / `TemporalEffectsTypes.h` / `DrawSortKey.h` | DONE — portable |
| Wire GTAO / BVH / ShaderVariant / VCT / Noise / Denoiser | DONE (Phases I/L/O/Q/S/T) |
| RHI parity for `RHIHandlePool`, `TransientBufferAllocator` | DONE (Phases X/Y/Z/AA) |
| EntityReplicator auto-driven from reflection | Deferred-as-unnecessary (`ReplicatedField<T>` already well-designed) |
| AI LOD-based tick scaling (#19) | DONE via `AIBudgetLimiter` + `AIIntegratedSystem` |

Net: the "pending" Phase-1/2/3/4 todos from those plans are effectively shipped; the plans survive only as design rationale.

## Remaining Open Work (post-freshening)

Highest-value still-open items: **#9 GPU skinning motion vectors**, **#30 LRU asset cache**, **#2 VSM**, **#4 VRS**, **#11/#12 chunked SoA + SIMD iteration**, **#16 HTN/GOAP**, **#18 influence maps**, **#23 motion matching**, **#25 rollback netcode**, **#31 AngelScript JIT**, **#34 chunked save format**. Confirmed done since authoring: **#15 priority event queue**, **#21 soft-body/cloth**, **#22 vehicle system**, **#26 transform quantization** (plus the pre-existing-items list above).

## Source & Freshness

- **Original analysis date:** 2026-04-18 (type: Decision + Plan; execution roadmap).
- **Verified against codebase 2026-06-08.**
- **Annotations / updates made:**
  - Added a per-technique status marker to all 34 items.
  - Confirmed newly **Implemented** since authoring: #15 priority-tiered event queue (verified `EventPriority` enum + tier eviction in `EventSystem.h`), #21 soft-body/cloth (`SoftBodyPhysics`, `ClothSimulation`), #22 vehicle system (`VehiclePhysics`), #26 transform quantization (`NetQuantize`).
  - Confirmed still **Open**: VSM, VRS, frame gen, GPU skinning motion vectors, chunked archetypes, SIMD iteration, fiber jobs, HTN/GOAP, influence maps, motion matching, rollback netcode, LRU asset cache, AngelScript JIT, chunked save format.
  - Marked **Partial** where a substrate exists but the technique is unfinished (bindless, radiance cache, Nanite finish, RVO2, virtual texturing, sub-tick input, save versioning).
  - Retained the "Pre-Existing Items Evaluated" verdicts and trimmed the session-specific Verification/Next-pickup blocks and external plan-file path references (stripped as AI-session frontmatter).

## Related Pages

- [Five Engine Analysis](Five-Engine-Analysis.md)
- [Eleven Engine Analysis](Eleven-Engine-Analysis.md)
- [ThorVG + Unity Graphics Analysis](ThorVG-Unity-Graphics-Analysis.md)
