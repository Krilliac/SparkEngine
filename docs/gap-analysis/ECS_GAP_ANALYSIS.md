# SparkEngine ECS (Entity Component System) — Gap Analysis

> **Scope**: `SparkEngine/Source/Engine/ECS/` (Components, Systems, World)
> **Date**: 2026-03-09
> **Methodology**: Static source-code audit of all `.h`/`.cpp` files under `Engine/ECS/`.
> Each gap is assigned a severity: **Critical**, **Major**, **Moderate**, **Minor**.

---

## Context

The ECS subsystem is built on EnTT and provides:
- **World**: Thin wrapper around `entt::registry` with typed entity/component CRUD
- **Components**: 7 component header files organized by domain (Core, Physics, Audio, Light, Animation, AI, Gameplay)
- **Systems**: 6 systems (Render, PhysicsUpdate, AudioUpdate, Lifecycle, AnimationUpdate, AIUpdate) with a `SystemManager` for ordered execution
- **ISystem interface**: Base class with `Update()`, `GetName()`, `IsEnabled()`, `SetEnabled()`

The architecture is clean and follows ECS best practices. Systems communicate through shared components, not direct calls. However, several gaps exist in the integration layer and missing standard functionality.

---

## Critical Gaps

### GAP-ECS01 — RenderSystem Does Not Issue Draw Calls

**Files**:
- `Engine/ECS/Systems/ECSystems.cpp` (lines 19-52)

**Impact**: The `RenderSystem::Update()` iterates entities with `Transform` + `MeshRenderer`, builds world matrices, and increments a counter — but does NOT actually call `GraphicsEngine::DrawMesh()` or submit any GPU work. The world matrix is computed and stored in `renderer.cachedWorldMatrix` but nothing is rendered.

**Evidence**:
```cpp
void RenderSystem::Update(World& world, float deltaTime)
{
    m_renderedCount = 0;
    // ... iterate entities, build world matrix ...
    XMStoreFloat4x4(&renderer.cachedWorldMatrix, worldMtx);
    renderer.worldMatrixDirty = false;
    m_renderedCount++;
    // No draw call to m_graphics
}
```

**What is needed**: After computing the world matrix, call `m_graphics->DrawMesh(renderer.meshName, renderer.materialName, worldMtx)` or equivalent. This is the critical bridge between ECS data and GPU rendering.

---

### GAP-ECS02 — No Transform Hierarchy (Parent-Child)

**Files**: `Engine/ECS/Components/CoreComponents.h`

**Impact**: `Transform` contains `position`, `rotation`, `scale` in world space but has no `parentEntity` or local/world transform distinction. Without parent-child relationships, attaching a weapon to a hand bone, a hat to a head, or a turret to a vehicle is impossible through the ECS.

**What is needed**: Add `EntityID parentEntity = entt::null` and `XMFLOAT3 localPosition/localRotation/localScale` to Transform. Implement a `TransformSystem` that propagates parent transforms to children in hierarchical order.

---

## Major Gaps

### GAP-ECS03 — No Component Serialization

**Files**: `Engine/ECS/Components/` (all 7 headers)

**Impact**: No component has a `Serialize()` / `Deserialize()` method or any serialization support. Scene saving, network replication, and prefab instancing all require the ability to serialize component data to/from binary or JSON.

**What is needed**: Implement serialization for all component types. Options:
- Reflection-based (register field names, types, offsets)
- Manual `Serialize(NetBuffer&)` / `Deserialize(NetBuffer&)` per component
- Integration with nlohmann/json for editor serialization

---

### GAP-ECS04 — No Entity Prefab / Template System

**Files**: `Engine/ECS/`

**Impact**: No way to define an entity archetype (e.g., "Enemy Soldier" = Transform + MeshRenderer + RigidBody + AIComponent + HealthComponent) and instantiate copies. Each entity must be manually assembled in code.

**What is needed**: A `Prefab` class that stores a component configuration and can instantiate new entities with those components pre-configured.

---

### GAP-ECS05 — No Entity Tagging / Layer System

**Files**: `Engine/ECS/Components/GameplayComponents.h`

**Impact**: `TagComponent` exists with a single string tag, but there is no bitmask-based layer system for efficient filtering. Physics collision layers, rendering layers, and gameplay query layers all need fast bitmask operations.

**What is needed**: Add a `LayerComponent` with a `uint32_t layerMask`. Provide layer name→bit mapping. Use for physics collision filtering, render layer culling, and gameplay queries ("find all entities on the Enemy layer").

---

### GAP-ECS06 — SystemManager Has No System Dependency Resolution

**Files**: `Engine/ECS/Systems/ECSystems.h` (SystemManager)

**Impact**: Systems execute in insertion order. There is no explicit dependency declaration or automatic topological sort. If systems are added in the wrong order (e.g., Render before Physics), behavior is incorrect. The correctness relies entirely on the caller adding systems in the right sequence.

**What is needed**: Either:
- Add `GetDependencies()` to `ISystem` and topologically sort in `AddSystem()`
- Or clearly document the required order and assert it at registration time

---

## Moderate Gaps

### GAP-ECS07 — No Component Change Detection

**Files**: `Engine/ECS/`

**Impact**: Systems iterate all entities with matching components every frame, even if nothing changed. For large scenes with mostly static entities, this wastes CPU time. EnTT provides `on_construct`, `on_update`, `on_destroy` signals that are not utilized.

**What is needed**: Use EnTT's observer/signal system for change-driven updates where appropriate (e.g., only recompute world matrix when Transform changes).

---

### GAP-ECS08 — World Has No Entity Query Caching

**Files**: `Engine/ECS/Components.h` (World class)

**Impact**: `GetEntitiesWith<Components...>()` creates a new view each call. While EnTT views are lightweight, there is no mechanism to cache frequently-used queries or use groups for performance-critical paths.

**What is needed**: Consider using EnTT groups for the most common component combinations (Transform + MeshRenderer, Transform + RigidBody) to get O(1) iteration with contiguous memory.

---

### GAP-ECS09 — No Entity Enable/Disable Beyond ActiveComponent

**Files**: `Engine/ECS/Components/GameplayComponents.h`

**Impact**: `ActiveComponent::active` is checked individually by each system. If a system forgets to check it, inactive entities are still processed. There is no global "disabled entity" mechanism.

**What is needed**: Either:
- Implement entity disable at the World level (move to a "disabled" registry or add a global excluded tag)
- Or centralize the active check in `SystemManager::UpdateAll()` by pre-filtering the view

---

### GAP-ECS10 — No Deferred Entity Destruction

**Files**: `Engine/ECS/Components.h` (World class)

**Impact**: `World::DestroyEntity()` calls `m_registry.destroy()` immediately. If called during system iteration, this can invalidate iterators. Systems must be careful not to destroy entities while iterating.

**What is needed**: Add `World::QueueDestroyEntity()` and `World::FlushDestroyQueue()`. Call flush between system updates in `SystemManager::UpdateAll()`.

---

## Minor Gaps

### GAP-ECS11 — No Entity Event System

**Impact**: No component-level events (OnAdd, OnRemove, OnEnable, OnDisable). Systems cannot react to component lifecycle changes without polling.

---

### GAP-ECS12 — MeshRenderer Uses String for Mesh/Material References

**Files**: `Engine/ECS/Components/CoreComponents.h`

**Impact**: `MeshRenderer::meshName` and `materialName` are `std::string`. Each frame, the RenderSystem must resolve these strings to actual GPU resources. Should use handles or IDs.

---

### GAP-ECS13 — No Multi-World Support

**Impact**: The engine appears to use a single World. For editor scenarios (editing world vs. play world) or level streaming, multiple World instances may be needed.

---

### GAP-ECS14 — SystemManager::GetSystem Uses Linear Search

**Files**: `Engine/ECS/Systems/ECSystems.h` (line 614)

**Impact**: `GetSystem(string_view name)` does a linear search through all systems. With only ~6 systems this is fine, but it could use a map for O(1) lookup.

---

## Summary Table

| ID | Severity | Gap | Impact |
|---|---|---|---|
| GAP-ECS01 | Critical | RenderSystem doesn't draw | Nothing renders via ECS |
| GAP-ECS02 | Critical | No transform hierarchy | No parent-child entities |
| GAP-ECS03 | Major | No component serialization | No save/load/replication |
| GAP-ECS04 | Major | No prefab system | Manual entity assembly |
| GAP-ECS05 | Major | No layer system | No efficient filtering |
| GAP-ECS06 | Major | No system dependency resolution | Order-dependent correctness |
| GAP-ECS07 | Moderate | No change detection | Wasted CPU on static entities |
| GAP-ECS08 | Moderate | No query caching | Suboptimal iteration |
| GAP-ECS09 | Moderate | No global entity disable | Inconsistent active checks |
| GAP-ECS10 | Moderate | No deferred destruction | Iterator invalidation risk |
| GAP-ECS11 | Minor | No entity events | Polling-only awareness |
| GAP-ECS12 | Minor | String-based asset references | Repeated lookups |
| GAP-ECS13 | Minor | No multi-world | Editor limitation |
| GAP-ECS14 | Minor | Linear system lookup | Negligible performance |

---

## Recommended Priority Order

1. **GAP-ECS01** — RenderSystem draw calls (unblocks visual output)
2. **GAP-ECS02** — Transform hierarchy (unblocks entity attachment)
3. **GAP-ECS03** — Component serialization (unblocks save/load)
4. **GAP-ECS10** — Deferred entity destruction (prevents crashes)
5. **GAP-ECS04** — Prefab system (workflow improvement)
6. **GAP-ECS05** — Layer system (filtering and collision)
7. **GAP-ECS07** — Change detection (performance)
8. Everything else
