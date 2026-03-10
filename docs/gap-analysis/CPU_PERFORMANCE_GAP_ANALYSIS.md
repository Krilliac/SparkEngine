# SparkEngine CPU Performance — Gap Analysis

> **Scope**: Engine-wide per-frame hot paths — ECS systems, AI, physics, networking, animation, rendering
> **Date**: 2026-03-10
> **Methodology**: Static source-code audit of update loops, per-frame methods, and algorithmic complexity.
> Each gap is assigned a severity: **Critical**, **Major**, **Moderate**, **Minor**.

---

## Context

SparkEngine's ECS execution order is Physics -> Animation -> AI -> Audio -> Lifecycle -> Render. Each system runs every frame on the main thread. The engine has a `Profiler` with scoped timers and a `PerformanceStats` tracker. Some subsystems are well-optimized (animation keyframes use binary search), but several hot paths contain O(n^2) algorithms, linear scans over large collections, and per-frame allocations that will cause frame drops as scene complexity grows.

---

## Critical Gaps

### GAP-CPU01 — O(n^2) Entity State Interpolation in Lag Compensation

**Files**:
- `Engine/Networking/NetworkManager.cpp` (lines 167–195)

**Impact**: `LagCompensator::RewindToTime()` uses nested loops to match entities across two snapshots by `networkID`. For each entity in the "before" snapshot, it scans all entities in the "after" snapshot. With 100 networked entities, this is 10,000 comparisons per lag-compensated query. In a multiplayer FPS with server-side hit detection, this runs for every projectile/hitscan check.

**Evidence**:
```cpp
for (const auto& entityBefore : before->entities)
{
    for (const auto& entityAfter : after->entities)
    {
        if (entityBefore.networkID == entityAfter.networkID)
        {
            // ... interpolate ...
            outSnapshot.entities.push_back(interpolated);
            break;
        }
    }
}
```

**What is needed**: Build an `std::unordered_map<uint32_t, const EntityState*>` for the "after" snapshot before the outer loop. Reduces complexity to O(n) with O(1) lookups.

---

### GAP-CPU02 — Linear Scan of All NavMesh Triangles for Nearest Point

**Files**:
- `Engine/AI/NavMesh.cpp` (lines 23–52)

**Impact**: `NavMeshQuery::FindNearestPoint()` iterates every triangle in the navmesh, computing point-to-triangle projection and distance for each. With a 1000-triangle navmesh and 20 AI agents querying per frame, this is 20,000 projection calculations per frame. This is the core spatial query used by all AI navigation.

**Evidence**:
```cpp
for (uint32_t i = 0; i < static_cast<uint32_t>(m_navMesh->triangles.size()); ++i)
{
    XMFLOAT3 projected = ProjectPointToTriangle(position, i);
    float distSq = dx * dx + dy * dy + dz * dz;
    if (distSq < bestDistSq) { ... }
}
```

**What is needed**: Build a spatial index (grid or BVH) over navmesh triangles. Only test triangles whose bounding cells overlap the query sphere. Reduces per-query cost from O(T) to O(log T) or O(1) amortized.

---

### GAP-CPU03 — O(n*m) AI Perception Without Spatial Partitioning

**Files**:
- `Engine/AI/AISystem.cpp` (lines 65–109)

**Impact**: Each AI agent's perception system checks distance to all potential targets every frame. With N agents and M targets, this is O(N*M) distance computations. In a scene with 50 enemies and 20 targets, that is 1,000 distance checks per frame. Combined with the navmesh scans (GAP-CPU02), the AI system dominates frame time.

**Evidence**:
```cpp
if (distSq < ai.config.detectionRange * ai.config.detectionRange)
{
    ai.lastKnownTargetPos = targetTransform->position;
}
```

No spatial partitioning limits the search radius. Every target is tested regardless of proximity.

**What is needed**: Use a spatial grid or quadtree to bucket entities by position. For each agent, only check targets in nearby cells (cells within `detectionRange`). The engine's existing Octree in `Utils/` could be adapted for this purpose.

---

### GAP-CPU04 — Per-Pathfinding Vector Allocations

**Files**:
- `Engine/AI/NavMesh.cpp` (lines 83–88)

**Impact**: Every A* pathfinding query allocates three full-sized vectors proportional to the total navmesh triangle count:

```cpp
std::vector<float> gCosts(triCount, 1e30f);
std::vector<uint32_t> parents(triCount, UINT32_MAX);
std::vector<bool> closed(triCount, false);
```

With a 2000-triangle navmesh, each query allocates ~24KB (8000 floats + 8000 uint32s + 2000 bools). If 10 agents request paths in a single frame, that is 240KB of heap allocation and deallocation per frame, causing allocator fragmentation and cache thrashing.

**What is needed**: Pool these vectors per-agent or per-thread. Allocate once at initialization and clear/reuse each query. Alternatively, use a sparse `std::unordered_map<uint32_t, AStarNodeData>` for large navmeshes where paths traverse a small fraction of total triangles.

---

## Major Gaps

### GAP-CPU05 — Linear Snapshot Search in Lag Compensator

**Files**:
- `Engine/Networking/NetworkManager.cpp` (lines 142–152)

**Impact**: The lag compensator scans all history snapshots linearly to find the two snapshots bracketing a target timestamp. With 300 snapshots (5 seconds at 60fps), this is 300 iterations per lag-compensated query.

**Evidence**:
```cpp
for (size_t i = 0; i < m_history.size(); ++i)
{
    if (m_history[i].timestamp <= targetTime) before = &m_history[i];
    if (m_history[i].timestamp >= targetTime && !after) after = &m_history[i];
}
```

The history is chronologically ordered, making binary search trivial.

**What is needed**: Replace with `std::lower_bound()` on the timestamp field. Complexity drops from O(n) to O(log n). With 300 snapshots, this is ~9 comparisons instead of 300.

---

### GAP-CPU06 — vector::erase() for Physics Body Removal

**Files**:
- `Physics/PhysicsSystem.cpp` (lines 944–949)

**Impact**: Removing a physics body performs a linear search followed by `vector::erase()`, which shifts all subsequent elements:

```cpp
auto it = std::find(m_bodies.begin(), m_bodies.end(), body);
if (it != m_bodies.end())
    m_bodies.erase(it);  // O(n) shift
```

With 200 physics bodies and frequent create/destroy cycles (bullets, debris, pickups), this is O(n) per removal with O(n) memory moves.

**What is needed**: Use swap-and-pop removal (`std::swap(*it, m_bodies.back()); m_bodies.pop_back()`) for O(1) removal when order doesn't matter. For order-preserving removal, use a deferred deletion list processed at end-of-frame.

---

### GAP-CPU07 — shared_ptr Overhead for Physics Bodies

**Files**:
- `Physics/PhysicsSystem.cpp` (lines 841–924)

**Impact**: Physics bodies are stored as `std::shared_ptr<PhysicsBody>` in `m_bodies`. Every access, copy, and removal involves atomic reference count operations. With 200+ dynamic bodies accessed every physics step, the atomic increments/decrements add measurable overhead — atomic operations prevent instruction reordering and force cache line synchronization.

**What is needed**: Replace with `std::unique_ptr<PhysicsBody>` since ownership is exclusive to `PhysicsSystem`. Use raw non-owning pointers for external references (entity components pointing to their physics body). This eliminates all atomic overhead.

---

## Moderate Gaps

### GAP-CPU08 — Linear System Name Lookup in ECS

**Files**:
- `Engine/ECS/Systems/ECSystems.h` (lines 614–632)

**Impact**: `SystemManager::GetSystem(string_view name)` performs a linear string comparison over all registered systems:

```cpp
for (auto& system : m_systems)
{
    if (name == system->GetName())
        return system.get();
}
```

With 6–10 systems this is negligible, but if called multiple times per frame for runtime enable/disable logic, string comparisons add up.

**What is needed**: Cache an `std::unordered_map<std::string, ISystem*>` at registration time for O(1) lookups.

---

### GAP-CPU09 — Profiler Overhead If Accidentally Enabled in Shipping

**Files**:
- `Utils/Profiler.h` (lines 279–298)

**Impact**: The profiler is conditionally compiled behind `PROFILING_ENABLED`. If this macro is accidentally defined in shipping builds, `ScopedProfileTimer` objects call `std::chrono::high_resolution_clock::now()` on every scope entry and exit. Each call is 20–50ns, and with hundreds of profiled scopes per frame, this adds 10–50 microseconds of overhead.

**Evidence**:
```cpp
#ifdef PROFILING_ENABLED
#define PROFILE_SCOPE(name) ScopedProfileTimer _profile_##__LINE__(name)
#else
#define PROFILE_SCOPE(name)
#endif
```

**What is needed**: Add a `static_assert` or CMake-level guard that prevents `PROFILING_ENABLED` from being set in Release/Shipping configurations. Alternatively, use a runtime flag with near-zero overhead when disabled (function pointer swap to no-op).

---

### GAP-CPU10 — Input Event Log Uses Front-Erase on Vector

**Files**:
- `Input/InputManager.cpp` (lines 744–748)

**Impact**: `LogInputEvent()` erases from the front of `m_recentInputEvents` when it exceeds 100 entries:

```cpp
if (m_recentInputEvents.size() >= 100)
    m_recentInputEvents.erase(m_recentInputEvents.begin());
```

Front-erase on `std::vector` is O(n), shifting all elements. With input events logged every frame, this is 100 element shifts per frame.

**What is needed**: Replace with `std::deque` (O(1) front removal), a ring buffer (the engine already has `Utils/RingBuffer`), or a circular index over a fixed-size array.

---

### GAP-CPU11 — Animation State Machine Transition Lookup

**Files**:
- `Engine/Animation/AnimationSystem.cpp` (lines 127–130)

**Impact**: Animation transitions are stored in a flat `std::vector`. When evaluating transitions each frame, all transitions must be checked against the current state. With 10 animated characters and 20 transitions per state machine, this is 200 linear scans per frame.

**What is needed**: Index transitions by source state in an `std::unordered_map<AnimationStateID, std::vector<Transition>>`. Only check transitions leaving the current state, not all transitions.

---

## Minor Gaps

### GAP-CPU12 — AI Path Vector Clearing Causes Allocator Churn

**Files**:
- `Engine/AI/AISystem.cpp` (lines 144–146)

**Impact**: When an AI agent completes its path, `ai.currentPath.clear()` deallocates the vector's storage. The next pathfinding request allocates a new vector. Over thousands of path completions, this causes heap fragmentation.

**What is needed**: Use `ai.currentPath.resize(0)` to keep the allocation, or swap with a pre-allocated path buffer.

---

### GAP-CPU13 — String Concatenation in Console Logging

**Files**:
- `Input/InputManager.cpp` (lines 319–350)

**Impact**: Console log messages use `std::string` concatenation (`"text" + std::string(value) + "more text"`), creating temporary strings. These are in console command handlers (not per-frame), so the impact is low, but it violates the C++20 coding standard which recommends `std::format`.

**What is needed**: Replace string concatenation with `std::format()` for consistency with C++20 coding standards. Low priority.

---
