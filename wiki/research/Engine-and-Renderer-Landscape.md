# Engine & Renderer Landscape

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (reference)
>
> **Platform/Backend Scope:** Cross-engine reference

## Overview

This page surveys the engines, frameworks, and architectural patterns most relevant to SparkEngine's evolution from an FPS-focused engine into a general-purpose (FPS/RPG/MMO/open-world) C++23 engine. It covers three research targets — ECS design philosophies, large-world/MMO networking architecture, and notable open-source C++ engines worth studying — and distills each into concrete, prioritized recommendations.

Crucially, every recommendation below was **verified against SparkEngine's actual source tree**, not proposed in the abstract. A surprising amount of the "obvious" advice turned out to already be implemented (change detection, parallel system scheduling, AOI interest management, delta replication, immutable state blocks). Those are called out explicitly so we don't rebuild what exists. The recommendations that survive verification are the real gaps — and they cluster tightly around the open-world/MMO roadmap.

SparkEngine's current relevant stack: **EnTT** ECS, full **RHI abstraction** (D3D11 primary; D3D12/Vulkan/Metal/OpenGL experimental), a **render graph**, **Jolt** physics, **AngelScript** scripting, a **Dear ImGui** editor, and a **UDP AreaServer/WorldServer** networking stack with floating-origin rebasing and seamless area streaming.

## How to read this

- **Comparative landscape** is a quick orientation table — where each reference engine sits on the axes that matter to SparkEngine.
- **Per-target notes** give the substance of each research area: what the field has converged on, the notable techniques, and sources.
- **Prioritized recommendations** are the actionable output. Each carries a verdict and an effort estimate:
  - **strong** — verified gap, high value, low risk. Do these.
  - **plausible** — real gap or partial gap, worth doing, but higher effort, more speculative, or dependent on a strong item landing first.
  - **Considered and skipped** — already implemented in SparkEngine or duplicative. Listed briefly so the reasoning is on record.
- Effort is **low / medium / high** relative to the existing codebase (a "low" item usually means the supporting machinery already ships and only wiring or a small addition remains).

## Comparative landscape

| Engine / Framework | Language | RHI / Render approach | ECS / Data model | Scripting | Standout feature for SparkEngine |
|---|---|---|---|---|---|
| **SparkEngine** | C++23 | Custom RHI (D3D11 primary; D3D12/VK/Metal/GL experimental) + render graph + NullRHI fallback | EnTT (sparse-set) | AngelScript (hot-reload) | The baseline being evaluated |
| **EnTT** (library) | C++17 | n/a (ECS library) | Sparse-set; opt-in owning groups | n/a | Owning groups for perfect-SoA hot queries ([skypjack](https://skypjack.github.io/2019-08-20-ecs-baf-part-4-insights/)) |
| **flecs** (library) | C/C++ | n/a (ECS library) | Archetype/table; first-class relationships | n/a | Entity relationships (ChildOf/IsA), wildcard/transitive queries ([flecs](https://www.flecs.dev/flecs/md_docs_2Relationships.html)) |
| **Bevy** | Rust | wgpu | Hybrid (Table + SparseSet); change-detection ticks | Rust | Per-component Added/Changed change detection ([Bevy deepwiki](https://deepwiki.com/bevyengine/bevy/2.7-archetypes-and-storage)) |
| **Unity DOTS** | C# (Burst/LLVM) | SRP | Archetype chunks (16KB) | C# | Auto job scheduling from read/write access decls ([Unity docs](https://docs.unity3d.com/Packages/com.unity.entities@0.50/manual/chunk_iteration_job.html)) |
| **Wicked Engine** | C++17 | Multi-backend RHI (D3D11/D3D12/VK/Metal), bindless | Dense-array ECS | Lua | Bindless descriptors + offline shader dump ([Wicked](https://github.com/turanszkij/WickedEngine)) |
| **Hazel** | C++ | NVRHI (D3D11/D3D12/VK) | EnTT | C# | Adopted NVRHI auto barrier-tracking over hand-rolled RHI ([NVRHI](https://github.com/NVIDIA-RTX/NVRHI)) |
| **Ogre-Next** | C++ | D3D11/VK/Metal | n/a (scene graph) | n/a | HLMS shader permutations + macro/blend/sampler blocks ([Ogre HLMS](https://ogrecave.github.io/ogre-next/api/2.3/hlms.html)) |
| **Stride** | C# | D3D11/12/VK/GL | Entity-component | C# | Data-driven, editable node-graph Graphics Compositor ([Stride](https://www.stride3d.net/features/)) |
| **SpatialOS** | (platform) | n/a | Entity-component, worker/authority | n/a | Authority vs interest separation; handover state machine ([Improbable](https://networking.docs.improbable.io/welcome/spatialos-concepts/authority-and-interest/)) |
| **HeroEngine** | (platform) | n/a | GOM | HSL | AreaServer/WorldServer + ID Server + spatial awareness ([HE wiki](http://hewiki.heroengine.com/wiki/Area_Server)) |
| **Star Citizen** | C++ | proprietary | proprietary | n/a | Replication Layer decoupling + dual-server border replication ([SC tools](https://starcitizen.tools/Replication_layer)) |

## Per-target notes

### ECS design: EnTT vs flecs vs Bevy vs Unity DOTS

The four leading ECS designs split into two storage philosophies. **EnTT** (which SparkEngine already uses) is **sparse-set** based: each component lives in its own packed dense array indexed via a sparse entity map, giving O(1) add/remove and zero archetype fragmentation, but multi-component iteration must intersect pools unless you opt into "groups." **flecs, Unity DOTS, and Bevy** are **archetype/table** based: entities are grouped into chunks/tables by exact component set, giving cache-perfect columnar iteration and easy parallelization at the cost of expensive structural changes (entity relocation) and table fragmentation.

The most transferable ideas for SparkEngine are **not** "switch storage model" — they are specific ergonomics layered on top of the storage you already have.

**Notable techniques:**

- **Sparse-set storage (EnTT):** per-component dense array + sparse entity-index map = O(1) insert/remove, no relocation; iterating N components requires choosing a lead pool and membership-testing the rest.
- **EnTT owning groups:** `group<A,B>()` physically reorders the owned pools so matching entities are contiguous and index-aligned — "perfect SoA," branchless iteration, zero indirection. Tradeoff: a pool can only be fully owned by one group.
- **Archetype/table storage (flecs/DOTS/Bevy):** entities with identical component sets share a column-oriented table/chunk; query matching is cached so steady-state query overhead trends to zero.
- **Unity DOTS chunks:** fixed 16KB `ArchetypeChunk` blocks, per-chunk presence checks done once per chunk; `IJobChunk`/`IJobEntity` + Burst compile system bodies to native SIMD.
- **DOTS auto job scheduling:** the scheduler builds a dependency graph from declared read/write access — read-only components run concurrently; writes force ordering.
- **flecs relationships:** first-class `(Relation, Target)` pairs (ChildOf, IsA, Likes) indexed by the ECS, queryable with wildcards `(Likes,*)` and transitively — faster than a component holding an entity handle.
- **Bevy change detection:** each component column records last-changed/last-added ticks; `Added<T>`/`Changed<T>` filters compare against the system's last-run tick — reactive systems without events.
- **Archetype fragmentation hazard:** deep hierarchies or many unique component combos create many tiny tables, collapsing throughput — the main reason sparse-set engines avoid encoding hierarchy as archetypes.

**Sources:** [ecs-faq](https://github.com/SanderMertens/ecs-faq) · [flecs relationships](https://www.flecs.dev/flecs/md_docs_2Relationships.html) · [flecs queries](https://www.flecs.dev/flecs/md_docs_2Queries.html) · [Bevy archetypes/storage](https://deepwiki.com/bevyengine/bevy/2.7-archetypes-and-storage) · [skypjack: ECS back-and-forth pt.4](https://skypjack.github.io/2019-08-20-ecs-baf-part-4-insights/) · [skypjack: EnTT tips](https://skypjack.github.io/2019-04-12-entt-tips-and-tricks-part-1/) · [Unity DOTS chunk iteration](https://docs.unity3d.com/Packages/com.unity.entities@0.50/manual/chunk_iteration_job.html) · [ecs_benchmark](https://github.com/abeimler/ecs_benchmark)

### Large-world / MMO networking: replication, interest management, server meshing

Modern large-world/MMO networking has converged on three primitives: (1) **decoupling entity state/replication from the simulation servers**, (2) **interest management (AOI)** so each client/server only sees nearby entities, and (3) **server meshing** — distributing authority across processes with seamless handover. HeroEngine pioneered the AreaServer/WorldServer model SparkEngine already cites; SpatialOS generalized it into a worker/authority/interest entity-component model; Star Citizen's Replication Layer + Persistent Entity Streaming is the production-grade example of state-server decoupling with client-survivable crash recovery.

SparkEngine's networking stack is **far more built-out than a generic review would assume** — AOI and per-client delta replication already exist by name (see `ConnectionScope.h`, `ConnectionScopeFilter.h`, `DeltaSnapshotManager.h`, `EntityReplicator.h`, `ReplicationFields.h`). The real gaps are in **authority handover** and **distributed ID allocation**.

**Notable techniques:**

- **Authority vs interest separation (SpatialOS):** write-access "authority" over an entity-component is distinct from "interest" (read/query). One worker simulates; many can read. Maps cleanly onto EnTT components.
- **Per-component authority:** authority granted at component granularity — position owned by a physics worker, inventory by another — enabling fine-grained load balancing.
- **Authority handover state machine (SpatialOS):** transitions `NotAuthoritative -> Authoritative -> AuthorityLossImminent -> NotAuthoritative`, with an `AuthorityLossImminent` callback letting the losing server flush critical updates before handover. Prevents dropped state across boundaries.
- **Replication Layer decoupling (Star Citizen):** entity state lives in a process separate from the Dedicated Game Server; if the DGS crashes the client stays connected and resumes at pre-crash state.
- **Dual-server border replication (Star Citizen):** at a region boundary the entity is handed to *both* servers — one computes, one receives — and authority swaps as the entity crosses, with no visible pop.
- **Spatial Awareness System (HeroEngine):** server-side per-entity awareness range generates add/remove awareness events that drive replication, culling distant entities.
- **AreaServer = self-contained GOM microcosm (HeroEngine):** each area server runs the full object model for a ~200–500m seamless area; WorldServer coordinates plus support services (Post Office Router, ID Server).
- **AOI spatial structures:** grid/chunk cells, spatial hashing (Mirror tracks 100k+ entities via 2D-grid hash), quadtree/octree. Grid/spatial-hash is the pragmatic default for uniform density.
- **Distributed ID allocation:** batches of IDs handed to each process (HeroEngine ID Server) so any node can spawn without a central round-trip.

**Sources:** [SpatialOS authority & interest](https://networking.docs.improbable.io/welcome/spatialos-concepts/authority-and-interest/) · [SpatialOS handover](https://documentation.improbable.io/spatialos-overview/docs/handing-over-write-access-authority) · [SC Replication Layer](https://starcitizen.tools/Replication_layer) · [SC PES forum writeup](https://forum.level1techs.com/t/star-citizen-persistent-entity-streaming-and-the-replication-layer/202872) · [HE Area Server](http://hewiki.heroengine.com/wiki/Area_Server) · [HE Spatial Awareness](http://hewiki.heroengine.com/wiki/Spatial_Awareness_System) · [HE World Server](http://wiki.heroengine.com/wiki/World_server) · [Mirror spatial hashing](https://mirror-networking.gitbook.io/docs/manual/interest-management/spatial-hashing) · [Interest management in MOGs](https://www.dynetisgames.com/2017/04/05/interest-management-mog/)

### Notable open-source C++ engines to study

Four mature engines, each with a distinct lesson:

- **Wicked Engine** (MIT, C++17) — the closest analog and most directly instructive: multi-backend RHI (D3D11/D3D12/Vulkan/Metal) with a **bindless descriptor model**, dense-array ECS, job system, and an **offline shader-permutation compiler** that embeds precompiled blobs into a generated header.
- **Hazel** (Cherno) — abandoned its hand-rolled Vulkan-only abstraction in 2025 and adopted NVIDIA's **NVRHI** (D3D11/D3D12/Vulkan with **automatic state tracking**). A validating data point for anyone maintaining a custom RHI.
- **Ogre-Next** (OGRE 2.x) — best-documented design for **shader-permutation generation (HLMS templates)** and **render-state batching (macro/blend/sampler blocks)**.
- **Stride** (C#/Xenko lineage) — a **data-driven, node-graph Graphics Compositor** that makes the render pipeline user-editable.

**Notable techniques:**

- **Wicked `wi::graphics`:** one `GraphicsDevice` interface spanning D3D11/D3D12/Vulkan/Metal — proof a single RHI can span legacy and modern APIs, exactly SparkEngine's shape.
- **Wicked bindless descriptor arrays:** 500,000+ resources bound without per-draw descriptor updates; buffer index maps directly to shader resource index — the foundation for GPU-driven rendering.
- **Wicked offline shader compiler:** ~428 shader types compiled into an embedded `wiShaderDump.h` with precompiled per-API blobs — ships with zero runtime-compile dependency.
- **NVRHI two-tier binding:** binding layouts (templates) + immutable binding sets (instances) allocated once at creation; lightweight and validatable at draw time.
- **NVRHI automatic resource-state tracking:** per-command-list barrier inference from declared initial/final states, with `setEnableAutomaticBarriers` to opt out on hot paths.
- **Ogre-Next HLMS:** hand-written HLSL/GLSL templates + custom preprocessor (`@property`, `@piece`/`@insertpiece`, `@foreach`) generate permutations on demand instead of precompiling all combinations.
- **Ogre-Next macroblocks/blendblocks/samplerblocks:** immutable, shared, ID'd render-state groups; sort/batch by block ID to minimize state changes.
- **Stride Graphics Compositor:** data-driven node-graph render pipeline (`RootRenderFeature` + `RenderStages`) editable visually, decoupling pipeline structure from engine code.

**Sources:** [Wicked Engine](https://github.com/turanszkij/WickedEngine) · [Wicked deepwiki](https://deepwiki.com/turanszkij/WickedEngine) · [Hazel](https://github.com/TheCherno/Hazel) · [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) · [NVRHI blog](https://developer.nvidia.com/blog/writing-portable-rendering-code-with-nvrhi/) · [Ogre-Next HLMS](https://ogrecave.github.io/ogre-next/api/2.3/hlms.html) · [Stride features](https://www.stride3d.net/features/) · [Stride custom render feature](https://github.com/tebjan/Stride.CustomRootRenderFeature)

## Prioritized recommendations for SparkEngine

### Strong (verified gaps — high value, low risk)

**1. EnTT owning groups for the hottest steady-state queries** — *effort: low*
Adopt EnTT owning `group<>` (instead of plain views) for render submission, transform propagation, and physics-sync-to-Jolt. Verified absent: grep across `ECSystems.cpp` and the ECS tree found **zero** `.group<>`/owning usage — all queries are plain views. EnTT already ships, so this is a pure usage change buying archetype-class contiguous SoA iteration exactly where the frame is spent (Render is the last ECS stage). Honors the one-group-per-owned-pool constraint. *Files:* `SparkEngine/Source/Engine/ECS/Systems/ECSystems.cpp`.

**2. Registry-wide deferred structural-change command buffer** — *effort: medium*
Add an `EntityCommandBuffer`-style facility for create/destroy/add/remove during iteration, flushed at stage boundaries (matching flecs deferred / Bevy `Commands`). Verified absent as a general facility — the "deferred queue" references in `ECSystems.h` are *per-system* action queues (e.g. `m_deadEntities`), not a shared structural-edit buffer. This is the **missing safety prerequisite** that makes the already-built `ParallelSystemExecutor` safe for gameplay systems that mutate structure mid-iteration. *Files:* `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h`, `ParallelSystemExecutor.h`.

**3. Authority-handover protocol at AreaServer boundaries** — *effort: high*
Implement an `AuthorityLossImminent` phase + flush callback + gaining-server ack + dual-server border-band streaming. Highest-value MMO-correctness item: `AreaServer.cpp` today does a blunt handover — `CheckEntityBoundaries()` serializes a full snapshot, pushes to `m_pendingMigrations`, and immediately *erases* the entity (~line 557), with a hardcoded placeholder target area (`m_config.areaId + 1`, ~line 536). No losing-server flush, no ack, no overlap band — exactly the dropped-update/entity-pop failure mode SpatialOS/Star Citizen solve. The seam exists; the protocol is a stub. *Files:* `SparkEngine/Source/Engine/Networking/AreaServer.cpp`.

**4. Distributed/batched entity-ID allocator on WorldServer** — *effort: low*
Implement HeroEngine ID-Server-style reserved ID blocks per AreaServer for collision-free local spawning. Today ID allocation is a single per-node atomic (`NetworkManager.h:662`, `std::atomic<uint32_t> m_nextNetworkID{1}`); networkIDs are merely preserved across migration, not centrally minted — the moment two AreaServers spawn concurrently, IDs collide. WorldServer already has the central role and an `AllocateAreaID()` precedent, so handing out reserved blocks is small, self-contained, and a hard prerequisite for distributed spawning. *Files:* `SparkEngine/Source/Engine/Networking/NetworkManager.h`, `WorldServer.h`.

**5. NVRHI-style automatic resource-state/barrier tracking (D3D12/Vulkan, opt-out)** — *effort: high*
Declare initial/final states at command-list boundaries and infer transitions internally, with a `setEnableAutomaticBarriers`-style opt-out. Verified gap: `D3D12CommandList.cpp` only has a manual `TransitionBarrier()`/`SetCurrentState()` per-resource model (callers issue transitions by hand); no boundary inference, no toggle. D3D11 (primary) hides barriers, so this de-risks exactly the experimental backends where RHI bugs concentrate — the strongest correctness win on the render side. *Files:* `SparkEngine/Source/Graphics/RHI/D3D12/D3D12CommandList.cpp`.

**6. Bindless descriptor model on D3D12/Vulkan** — *effort: high*
Move to a large descriptor array with index-into-array in shaders, following Wicked's `wi::graphics`, as the foundation for GPU-driven culling/draw. Verified gap aligned with the open-world/MMO roadmap: `D3D12Device` only *detects* `bindlessResourceSupport` as a capability flag (`ResourceBindingTier >= 3`) — there is no actual bindless binding path in the command list (no descriptor-array binding, no `GetBindlessIndex`). Indirect-draw plumbing (`DrawInstancedIndirect`/`DispatchIndirect`) and a `GPUSceneBuffer` already exist, so bindless is the missing prerequisite for true GPU-driven rendering. Correctly staged **after** barrier work (#5). *Files:* `SparkEngine/Source/Graphics/RHI/D3D12/D3D12Device.cpp`, `GPUSceneBuffer.h`.

### Plausible (real or partial gaps — worth doing, higher effort or dependent)

**7. flecs-style first-class entity relationships (ChildOf/IsA) with a maintained index** — *effort: high*
For hierarchy, prefab inheritance, and area-ownership, enabling wildcard/transitive queries. Genuinely missing: the only hierarchy mechanism is raw `EntityID parent` + `std::vector<EntityID> children` on `Transform` (`CoreComponents.h`) — the ad-hoc handle pattern this argues against — and grep found no IsA/relationship/wildcard support. Fits the RPG/MMO/open-world direction. Downgraded from strong because it is high effort, partly overlaps `RuntimePrefab.h` (data-driven template cloning, though not live IsA inheritance), and a full relationship index is a large new subsystem. **Scope it to scene-graph + area-ownership first**, not a general relationship engine. *Files:* `SparkEngine/Source/Engine/ECS/Components/CoreComponents.h`, `RuntimePrefab.h`.

**8. Per-component authority tag (Authoritative / Replicated / NotPresent)** — *effort: low*
Build snapshots partly from authority tags. The *interest* half is fully present (`ReplicationFields.h` has `FieldVisibility` Public/Private/Party/Spectator + `GetDirtyMaskForVisibility`); the *authority* half is genuinely absent — authority is implicitly "the simulating node writes." A lightweight authority tag is the actual prerequisite for real server meshing — **scope it tightly to the authority dimension only** (the visibility dimension already ships). Direct enabler for #3. *Files:* `SparkEngine/Source/Engine/Networking/ReplicationFields.h`.

**9. Replication-layer split: authoritative entity store separate from AreaServer simulation** — *effort: high*
For crash recovery, à la Star Citizen. Not present today: `WorldServer.h` coordinates sessions/areas/load-balancing but owns **no** authoritative entity store — if an AreaServer dies, its `m_trackedEntities` are lost. Would integrate with the existing Persistence subsystem. **Start as a design doc + thin in-process store**; value is gated on #8 (authority) and #3 (handover) landing first. *Files:* `SparkEngine/Source/Engine/Networking/WorldServer.h`.

**10. Data-driven, editor-editable render graph (node panel)** — *effort: medium*
Expose pass composition as an ImGui node graph (Stride Graphics Compositor). Verified gap: `RenderGraph.h` is built entirely from C++ `AddPass()` lambdas and only exports DOT/debug text — no editor panel. Would let game modules customize pipelines per genre without recompiling. Builds on the existing graph + collaborative editor. *Files:* `SparkEngine/Source/Graphics/RenderGraph.h`.

**11. Offline shader-dump embedded header** — *effort: medium*
Add a build step embedding precompiled per-backend blobs into a generated header (Wicked `wiShaderDump.h`) so release builds need no runtime shader compiler. Partially covered: `SparkShaderCompiler` compiles offline and `ShaderDiskCache` persists blobs, but neither emits an embedded C++ header to drop the runtime compiler/file-IO dependency. Self-contained codegen; incremental over the disk cache (which already removes most recompilation cost). *Files:* `SparkShaderCompiler/`, `SparkEngine/Source/Graphics/ShaderDiskCache.h`.

**12. Record the sparse-set storage decision in `wiki/advanced/Codebase-Observations.md`** — *effort: low*
Document the decision to **keep sparse-set storage and not migrate to an archetype model**. No ECS storage-model decision doc exists today; recording it fits the repo's persistence practice and forecloses a costly future rewrite proposal. The fragmentation/relocation rationale is sound for SparkEngine's dynamic-composition, streaming-heavy workloads, and groups (#1) + reactive systems already recover most of the iteration win. Documentation-only.

### Considered and skipped (already implemented or duplicative)

- **Per-component change detection (Added/Changed)** — already built: `SparkEngine/Source/Engine/ECS/ReactiveSystem.h` (templated `ReactiveSystem<Component>` on `on_construct`/`on_update`/`on_destroy` with a change queue; concrete `MaterialChangeReactiveSystem`/`LightChangeReactiveSystem`; wired via `ECSIntegration.h`). Net-new value only in extending to networking delta replication / collaborative editor.
- **Per-system read/write access auto-scheduling (DOTS-style)** — already built: `SparkEngine/Source/Engine/ECS/Systems/ParallelSystemExecutor.h` (`SystemAccessDecl`, `DeclareReads`/`DeclareWrites`, `BuildSchedule()` conflict batching, `JobSystem` dispatch, `StageBasedExecutor` honoring Physics→Animation→AI→Audio→Lifecycle→Render). Remaining work is wiring real per-system declarations, not building the mechanism.
- **Explicit interest-management / AOI layer** — already implemented twice: `ConnectionScope.h` (`EvaluateScope()` enter/exit/inScope, `priorityBias`, `alwaysRelevant`) and `ConnectionScopeFilter.h` (area+radius+team+visibility), wired into `WorldServer::UpdatePlayerPosition` (interestRadius 500.0f). Only marginal delta is a spatial-hash acceleration structure — a perf optimization of an existing system.
- **Delta/snapshot replication with per-client baselines + priority** — already implemented: `DeltaSnapshotManager.h` (per-connection `ackedEntityFields`, `lastAckedSequence`, FNV-1a hashing, `BuildDeltaPacket`) + `EntityReplicator.h` (bitmask dirty tracking, `WriteCreate/UpdatePacket`, `GatherDirtyEntities`/`FlushDirty`) + `ConnectionScope` priority bias.
- **Immutable ID'd render-state blocks + sort-by-block (Ogre macroblocks)** — already present under other names: `PipelineStateCache.h` (immutable flyweight, shared_ptr dedup, encoded blend/depth/raster as uint32, pointer-compare sorting), `DrawSortKey.h` (64-bit radix-sorted key), `RHIPipelineTypes.h` (immutable RHI descs).
- **Template/HLMS shader-permutation system** — core machinery exists: `ShaderVariantSystem.h` (MultiCompile/ShaderFeature/DynamicBranch keywords, 64-bit variant keys, dead-variant stripping, warmup) + `ShaderDiskCache.h` (content-hash-keyed blob cache). Only the generative "code pieces" assembly would be net-new — marginal.
- **Study Wicked's job system / parallel ECS** — already built, not just study-worthy: `Utils/JobSystem.h` (`Submit`/`ParallelFor`) + `ParallelSystemExecutor.h` (read/write-set batched parallel dispatch), plus `ParallelPerception` and multithreaded Jolt.

## Sources

**ECS:** [ecs-faq](https://github.com/SanderMertens/ecs-faq) · [flecs FAQ](https://www.flecs.dev/flecs/md_docs_2FAQ.html) · [flecs relationships](https://www.flecs.dev/flecs/md_docs_2Relationships.html) · [flecs queries](https://www.flecs.dev/flecs/md_docs_2Queries.html) · [Bevy archetypes/storage](https://deepwiki.com/bevyengine/bevy/2.7-archetypes-and-storage) · [Bevy components/storage](https://deepwiki.com/bevyengine/bevy/2.2-components-and-storage) · [Bevy issue #17564](https://github.com/bevyengine/bevy/issues/17564) · [skypjack: ECS back-and-forth pt.4](https://skypjack.github.io/2019-08-20-ecs-baf-part-4-insights/) · [skypjack: EnTT tips pt.1](https://skypjack.github.io/2019-04-12-entt-tips-and-tricks-part-1/) · [EnTT meta containers](https://deepwiki.com/skypjack/entt/3.3-meta-containers) · [Unity DOTS chunk iteration](https://docs.unity3d.com/Packages/com.unity.entities@0.50/manual/chunk_iteration_job.html) · [What's next after Burst](https://lucasmeijer.com/posts/whats_next_after_burst/) · [ecs_benchmark](https://github.com/abeimler/ecs_benchmark)

**MMO networking:** [SpatialOS authority & interest](https://networking.docs.improbable.io/welcome/spatialos-concepts/authority-and-interest/) · [SpatialOS handover](https://documentation.improbable.io/spatialos-overview/docs/handing-over-write-access-authority) · [SpatialOS access](https://docs.improbable.io/reference/13.6/shared/design/understanding-access) · [SpatialOS load balancing](https://docs.improbable.io/reference/13.7/shared/concepts/workers-load-balancing) · [Dynamic interest management](https://www.improbable.io/blog/thinking-spatially-using-dynamic-interest-management-to-create-revolutionary-game-designs/) · [SC Replication Layer](https://starcitizen.tools/Replication_layer) · [SC Comm-Link 18397](https://star-citizen.wiki/Comm-Link:18397/en) · [SC PES forum writeup](https://forum.level1techs.com/t/star-citizen-persistent-entity-streaming-and-the-replication-layer/202872) · [HE Area Server](http://hewiki.heroengine.com/wiki/Area_Server) · [HE Seamless world](http://hewiki.heroengine.com/wiki/Seamless_world) · [HE Spatial Awareness](http://hewiki.heroengine.com/wiki/Spatial_Awareness_System) · [HE World Server](http://wiki.heroengine.com/wiki/World_server) · [Interest management in MOGs](https://www.dynetisgames.com/2017/04/05/interest-management-mog/) · [Mirror spatial hashing](https://mirror-networking.gitbook.io/docs/manual/interest-management/spatial-hashing) · [MMO AOI algorithm](https://dev.to/aceld/11-mmo-online-game-aoi-algorithm-l7d) · [Spatial partition pattern](https://gameprogrammingpatterns.com/spatial-partition.html)

**Open-source C++ engines:** [Wicked Engine](https://github.com/turanszkij/WickedEngine) · [Wicked deepwiki](https://deepwiki.com/turanszkij/WickedEngine) · [Wicked site](https://wickedengine.net/) · [Hazel](https://github.com/TheCherno/Hazel) · [Hazel site](https://hazelengine.com/) · [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) · [NVRHI blog](https://developer.nvidia.com/blog/writing-portable-rendering-code-with-nvrhi/) · [Hazel new renderer](https://app.daily.dev/posts/hazel-s-new-renderer-is-done--ycfykgzsy) · [Ogre-Next HLMS](https://ogrecave.github.io/ogre-next/api/2.3/hlms.html) · [Ogre-Next intro](https://deepwiki.com/OGRECave/ogre-next/1-introduction-to-ogre-next) · [Stride features](https://www.stride3d.net/features/) · [Stride (Wikipedia)](https://en.wikipedia.org/wiki/Stride_(game_engine)) · [Stride repo](https://github.com/stride3d/stride) · [Stride custom render feature](https://github.com/tebjan/Stride.CustomRootRenderFeature)

## Source & Freshness

- **Method:** Deep multi-agent research workflow (2026-06-08). Recommendations were verified against the live SparkEngine source tree, not proposed abstractly.
- **Coverage limitation:** The workflow launched 20 research targets; **17 stalled on web access** in the research sandbox (network unavailable) and only 3 returned — ECS design, large-world/MMO networking, and open-source C++ engines. These three are the most roadmap-relevant. The rendering/technique targets that could not be web-researched here (Nanite-style virtual geometry, realtime GI, temporal upscaling, GPU-driven rendering, render-graph patterns, WebGPU/Slang) are already covered in depth by the migrated analyses below.
- **See also:** [ThorVG-Unity-Graphics-Analysis](ThorVG-Unity-Graphics-Analysis.md), [Advanced-Techniques-Catalog](Advanced-Techniques-Catalog.md), [Five-Engine-Analysis](Five-Engine-Analysis.md), [Eleven-Engine-Analysis](Eleven-Engine-Analysis.md).
