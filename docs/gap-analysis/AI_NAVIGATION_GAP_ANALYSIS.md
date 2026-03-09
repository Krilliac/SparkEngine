# SparkEngine AI & Navigation — Gap Analysis

> **Scope**: `SparkEngine/Source/Engine/AI/` (AISystem, BehaviorTree, NavMesh, PerceptionSystem, SteeringBehaviors)
> **Date**: 2026-03-09
> **Methodology**: Static source-code audit of all `.h`/`.cpp` files under `Engine/AI/`.
> Each gap is assigned a severity: **Critical**, **Major**, **Moderate**, **Minor**.

---

## Context

The AI subsystem is relatively well-designed with three main layers:
- **BehaviorTree**: Complete node framework (Sequence, Selector, Parallel, Inverter, Repeater, Action, Condition, Wait) with Blackboard key-value store. Has 7 stub patterns in the header, mostly in advanced node types.
- **NavMesh**: Working A* pathfinding over triangle adjacency graphs, point-to-triangle projection, path smoothing via string-pulling, and a NavMeshBuilder. The `.cpp` has 11 stub patterns, mostly in the builder.
- **PerceptionSystem**: Header-only inline implementation with vision cone, hearing radius, and confidence-decay memory. Has 6 stub patterns.
- **AISystem**: Orchestrates all three, with per-agent behavior tree instances and NavMeshQuery handles.

---

## Critical Gaps

### GAP-AI01 — NavMeshBuilder Is Largely Stubbed

**Files**:
- `Engine/AI/NavMesh.h` (NavMeshBuilder class)
- `Engine/AI/NavMesh.cpp` (11 stub patterns, concentrated in builder)

**Impact**: While `NavMeshQuery::FindPath()` has a real A* implementation, the `NavMeshBuilder::Build()` that generates NavMesh data from level geometry is incomplete. Without this, nav meshes must be hand-authored or loaded from pre-built `.snav` files with no tooling to create them.

**Evidence**: The builder needs to:
1. Voxelize the scene geometry
2. Filter walkable voxels by slope and height
3. Build a triangle mesh from the walkable region
4. Compute triangle adjacency for pathfinding

Steps 1-4 require integration with Recast/Detour or a custom implementation. The current builder likely returns basic/placeholder data.

**What is needed**: Integrate Recast (industry standard) for nav mesh generation, or implement a minimal voxel-based builder. Expose bake parameters (agent radius, step height, slope angle) in the editor.

---

### GAP-AI02 — No Dynamic NavMesh Obstacle Support

**Files**: `Engine/AI/NavMesh.h`

**Impact**: The NavMesh is static once built. There is no support for dynamic obstacles (doors opening/closing, destructible walls, moving platforms) that modify the navigation graph at runtime.

**What is needed**: Implement nav mesh tile-based updates or Detour's dynamic obstacle avoidance. At minimum, support marking/unmarking rectangular regions as blocked.

---

## Major Gaps

### GAP-AI03 — No Local Obstacle Avoidance (RVO / ORCA)

**Files**: `Engine/AI/SteeringBehaviors.h`

**Impact**: SteeringBehaviors provides basic steering (seek, flee, arrive, wander, pursuit, evasion) but has no local obstacle avoidance (RVO/ORCA). When multiple AI agents follow paths simultaneously, they will overlap and clip through each other rather than flowing around one another.

**What is needed**: Implement Reciprocal Velocity Obstacles (RVO2) or ORCA for local agent avoidance. Alternatively, integrate RVO2 library.

---

### GAP-AI04 — No Squad/Group AI

**Files**: `Engine/AI/AISystem.h`

**Impact**: Each AI agent operates independently. For an FPS game, group behaviors (flanking, suppressive fire, coordinated assault, covering fire) are essential for believable combat AI.

**What is needed**: A `SquadManager` that groups agents and coordinates their behavior tree blackboards (e.g., assigned roles like "flanker", "suppressor", "point man").

---

### GAP-AI05 — PerceptionSystem Has No Line-of-Sight Raycasts

**File**: `Engine/AI/PerceptionSystem.h`

**Impact**: The vision cone check (`IsInVisionCone`) tests angle and distance but does NOT perform a physics raycast to verify line of sight. An agent can "see" through walls.

**Evidence**: The function is a pure geometric test with no PhysicsSystem dependency:
```cpp
inline bool IsInVisionCone(const XMFLOAT3& eye, const XMFLOAT3& forward,
                           const XMFLOAT3& target, float fovDegrees, float maxRange)
```

**What is needed**: Add an optional `PhysicsSystem*` parameter to perform a raycast from eye to target, rejecting detections blocked by geometry.

---

### GAP-AI06 — BehaviorTree Has No Serialization

**File**: `Engine/AI/BehaviorTree.h`

**Impact**: Behavior trees can only be created in C++ code. There is no JSON/XML serialization format, meaning designers cannot author or tweak AI behavior without recompiling.

**What is needed**: Implement a JSON-based behavior tree format that can be loaded at runtime. Support hot-reload for rapid iteration.

---

## Moderate Gaps

### GAP-AI07 — No Utility AI or GOAP Alternative

**Files**: `Engine/AI/`

**Impact**: Only behavior trees are supported for AI decision-making. Utility AI (score-based action selection) and GOAP (Goal-Oriented Action Planning) are alternatives that can produce more emergent behavior. While behavior trees are the right default, a scoring system for action selection would complement them.

**What is needed**: Consider adding a `UtilityAI` evaluator that can be used as a behavior tree leaf node, scoring available actions based on context.

---

### GAP-AI08 — No AI Debug Visualization

**Files**: `Engine/AI/AISystem.h`

**Impact**: No debug rendering for AI: no path visualization, no behavior tree state overlay, no perception cone display, no navmesh wireframe. Debugging AI behavior requires reading log output or code inspection.

**What is needed**: Implement debug draw for:
- NavMesh wireframe (triangles, adjacency)
- Current path (line strip with waypoints)
- Perception cones (vision frustum, hearing radius)
- Behavior tree state (current active node name above agent)
- Agent state text overlay (Idle/Patrol/Combat/Dead)

---

### GAP-AI09 — NavMesh Has No Area Costs

**File**: `Engine/AI/NavMesh.h`

**Impact**: All triangles have equal traversal cost. There is no support for area types (road = cheap, swamp = expensive, water = very expensive) that would make pathfinding terrain-aware.

**What is needed**: Add an `areaCost` field to `NavTriangle` and factor it into the A* heuristic.

---

### GAP-AI10 — No Cover Point System

**Files**: `Engine/AI/`

**Impact**: FPS AI requires cover-seeking behavior. There is no cover point detection, evaluation, or reservation system.

**What is needed**: A `CoverSystem` that:
- Detects cover points via raycasts from navmesh points
- Evaluates cover quality (protection angle, distance to threat)
- Supports reservation (one agent per cover point)
- Integrates with behavior trees via blackboard keys

---

## Minor Gaps

### GAP-AI11 — No Environmental Queries (EQS equivalent)

**Impact**: No system for spatial queries like "find the best sniper position within 50m that has line of sight to the objective." Agents can only path to explicit points.

---

### GAP-AI12 — SteeringBehaviors Not Integrated With NavMesh

**Impact**: Steering behaviors operate in open space with no awareness of the navmesh boundaries. An agent following a steering force can leave the walkable area.

---

### GAP-AI13 — AISystem Has No Performance Budget

**Impact**: All agents are ticked every frame regardless of distance to the player. No LOD system for AI (reduced tick rate for distant agents).

---

## Summary Table

| ID | Severity | Gap | Impact |
|---|---|---|---|
| GAP-AI01 | Critical | NavMeshBuilder stubbed | Cannot generate nav meshes |
| GAP-AI02 | Critical | No dynamic obstacles | Static navigation only |
| GAP-AI03 | Major | No local avoidance (RVO) | Agents clip through each other |
| GAP-AI04 | Major | No squad/group AI | No coordinated combat |
| GAP-AI05 | Major | No LOS raycasts in perception | AI sees through walls |
| GAP-AI06 | Major | No BT serialization | C++-only AI authoring |
| GAP-AI07 | Moderate | No utility AI / GOAP | Limited decision framework |
| GAP-AI08 | Moderate | No AI debug visualization | Hard to debug AI |
| GAP-AI09 | Moderate | No navmesh area costs | Uniform traversal cost |
| GAP-AI10 | Moderate | No cover point system | No cover-seeking AI |
| GAP-AI11 | Minor | No environmental queries | Limited spatial reasoning |
| GAP-AI12 | Minor | Steering not navmesh-aware | Agents can leave walkable area |
| GAP-AI13 | Minor | No AI LOD / performance budget | All agents tick every frame |

---

## Recommended Priority Order

1. **GAP-AI01** — NavMeshBuilder (unblocks level navigation)
2. **GAP-AI05** — LOS raycasts in perception (FPS-critical)
3. **GAP-AI10** — Cover point system (FPS-critical)
4. **GAP-AI03** — Local avoidance / RVO
5. **GAP-AI06** — BT serialization (designer workflow)
6. **GAP-AI04** — Squad AI (combat quality)
7. **GAP-AI08** — Debug visualization
8. Everything else
