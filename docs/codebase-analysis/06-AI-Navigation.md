# 06 — AI & Navigation

**Location:** `SparkEngine/Source/Engine/AI/`

Three-layer architecture: data (NavMeshData), query (NavMeshQuery), and management (AISystem). Features behavior trees, NavMesh pathfinding, perception, steering behaviors, cover systems, formations, and tactical points.

---

## File Structure

```
AI/
├── AISystem.h/cpp            — ECS system for AI agents
├── BehaviorTree.h            — BT container, lifecycle
├── BehaviorTreeTypes.h       — NodeStatus, Blackboard, BTNode base
├── BehaviorTreeNodes.h       — Concrete node implementations
├── NavMesh.h                 — Navigation mesh manager & query
├── NavMeshTypes.h            — NavMesh data structures
├── AIIntegration.h           — ECS integration helpers
├── AIBudgetLimiter.h         — Performance throttling
├── AIDebugRenderer.h         — Debug visualization
├── AIDirector.h              — Global AI coordination
├── PerceptionSystem.h        — Line-of-sight, hearing
├── ParallelPerception.h      — Multithreaded perception
├── MovementSystem.h          — Steering behaviors
├── SteeringBehaviors.h       — Flocking, pursuit, evasion
├── CoverSystem.h             — Cover point management
├── FormationSystem.h         — Group movement formations
├── GroupAI.h                 — Squad/swarm logic
├── EnvironmentQuery.h        — Spatial queries (EQS)
├── TacticalPointSystem.h     — Strategic position evaluation
├── NavMeshLink.h             — Jump/drop links
├── NavMeshObstacles.h        — Dynamic obstacle carving
├── CollisionAvoidance.h      — Reactive avoidance
└── RecastDetourBackend.h     — Recast/Detour integration
```

---

## Behavior Tree Framework

### Node Status

```cpp
enum class NodeStatus {
    Running,   // Still executing (pauses parent)
    Success,   // Task completed
    Failure    // Task failed
};
```

### Blackboard — Typed Key-Value Store

```cpp
// Value = variant<bool, int, float, std::string, XMFLOAT3>
Blackboard bb;
bb.Set("targetPos", XMFLOAT3{10, 0, 5});
bb.Set("hasAmmo", true);
bb.Set("health", 75.0f);

XMFLOAT3 target = bb.Get<XMFLOAT3>("targetPos", {0,0,0});
bool ammo = bb.Get<bool>("hasAmmo", false);
```

### Node Types

| Category | Nodes | Behavior |
|----------|-------|----------|
| **Composite** | `SequenceNode` | All children must succeed (AND) |
| | `SelectorNode` | First successful child wins (OR) |
| | `ParallelNode` | Tick all, policy on completion |
| **Decorator** | `InverterNode` | Flip Success/Failure |
| | `RepeaterNode` | Loop N times |
| **Leaf (Action)** | `ActionNode` | User-defined callback |
| | `WaitNode` | Delay for duration |
| **Leaf (Condition)** | `ConditionNode` | Return Success/Failure from callback |

### Building Trees

```cpp
// Manual construction
auto root = std::make_unique<SelectorNode>("Root");
auto patrol = std::make_unique<SequenceNode>("Patrol");
patrol->AddChild(std::make_unique<ConditionNode>("NoTarget",
    [](Blackboard& bb) { return !bb.Has("targetEntity"); }));
patrol->AddChild(std::make_unique<ActionNode>("WalkPath",
    [](float dt, Blackboard& bb) { /* follow waypoints */ return NodeStatus::Running; }));
root->AddChild(std::move(patrol));
```

### Pre-built FPS Behaviors (Factory Functions)

```cpp
auto patrolTree = CreatePatrolBehavior(patrolPoints);   // Walk waypoints
auto combatTree = CreateCombatBehavior(config);          // Seek, cover, fire, retreat
auto guardTree  = CreateGuardBehavior(position, radius); // Stand post, alert on intrusion
auto fleeTree   = CreateFleeBehavior(fleeDistance);      // Run away when health low
```

### BehaviorTree Container

```cpp
BehaviorTree tree;
tree.SetRoot(std::move(rootNode));
tree.Tick(deltaTime);   // Recursive evaluation
tree.Reset();            // Clear node state (NOT blackboard)

Blackboard& bb = tree.GetBlackboard();
bb.Set("playerPos", playerPosition);  // Perception writes here
```

---

## NavMesh — Navigation

### Three Layers

**Layer 1: Data (NavMeshData)**
- Vertices, triangles, adjacency graph
- Binary `.snav` file format
- Managed by NavMeshManager singleton

**Layer 2: Query (NavMeshQuery)**
- Per-agent read-only interface to NavMeshData
- Path caching: 64-entry LRU, 5-second expiry (Redot-inspired)

**Layer 3: Management (NavMeshManager)**
- Singleton registry of named NavMeshData
- Thread-safe mutations from main thread only

### NavMeshQuery API

```cpp
auto query = NavMeshManager::GetInstance().CreateQuery("main_navmesh");

// Pathfinding (A*)
std::vector<XMFLOAT3> path;
bool found = query->FindPath(startPos, endPos, path);

// Spatial queries
XMFLOAT3 nearest = query->FindNearestPoint(worldPos);
bool onMesh = query->IsPointOnNavMesh(worldPos);

// Line-of-sight
bool canSee = query->Raycast(fromPos, toPos);

// Random sampling (for patrol points)
XMFLOAT3 randomPt = query->GetRandomPoint();
XMFLOAT3 nearbyPt = query->GetRandomPointInCircle(center, radius);
```

### NavMesh Building

```cpp
NavMeshManager::GetInstance().LoadNavMesh("main", "Data/NavMesh/level1.snav");

// Or build procedurally
NavMeshBuilder builder;
builder.Build(vertices, indices, settings);        // Voxel-based from triangle soup
builder.BuildFromHeightfield(heightfield, settings); // Terrain-optimized
```

Build pipeline: Voxelization → Erosion → Contour extraction → Triangulation. Uses Recast/Detour backend with fallback grid builder.

---

## AISystem — ECS Integration

**File:** `SparkEngine/Source/Engine/AI/AISystem.h`

```cpp
class AISystem : public Spark::ECS::ISystem {
public:
    void RegisterBehavior(const std::string& name, std::unique_ptr<BehaviorTree> tree);
    BehaviorTree* CreateBehaviorInstance(const std::string& name);

    void Update(entt::registry& world, float deltaTime) override;

    // Console
    std::string Console_ListAgents(entt::registry& world) const;
    std::string Console_GetAgentInfo(entt::registry& world, entt::entity entity) const;

private:
    void UpdatePerception();  // Refresh targets, visibility, timers
    void UpdateBehavior();    // Tick behavior trees
    void UpdateMovement();    // Steer toward waypoints
};
```

### Per-Frame Pipeline

```
AISystem::Update()
├── UpdatePerception()   → writes to Blackboard (target, visibility, etc.)
├── UpdateBehavior()     → BehaviorTree::Tick() → reads Blackboard, makes decisions
└── UpdateMovement()     → writes to Transform (position, rotation)
```

### Lazy Instantiation

Behavior templates registered at startup. Per-agent instances cloned on first encounter:

```cpp
aiSystem.RegisterBehavior("guard", CreateGuardBehavior({0,0,0}, 20.0f));
// On first AI tick for entity with behaviorTreeName="guard":
// → Clone template → create instance → begin ticking
```

---

## Perception System

**File:** `SparkEngine/Source/Engine/AI/PerceptionSystem.h`

Line-of-sight and hearing detection:

- **Visual**: Raycast from AI eye to target, check angle against FOV cone
- **Audio**: Distance check against hearing range, louder sources detected farther
- **Cooldowns**: Perception checks throttled to avoid per-frame expense
- **Parallel**: `ParallelPerception.h` provides multithreaded perception for many agents

---

## Steering Behaviors

**File:** `SparkEngine/Source/Engine/AI/SteeringBehaviors.h`

Classic Craig Reynolds steering:

| Behavior | Description |
|----------|-------------|
| Seek | Move toward target |
| Flee | Move away from target |
| Arrive | Seek with deceleration near target |
| Pursue | Predict target future position, seek there |
| Evade | Predict pursuer position, flee from there |
| Wander | Random meandering (circle-based) |
| Separation | Avoid crowding neighbors |
| Alignment | Match neighbor heading |
| Cohesion | Move toward neighbor center |
| ObstacleAvoidance | Raycast-based avoidance |

Combined via weighted blending:

```cpp
XMFLOAT3 steeringForce = SteeringBehaviors::Calculate(agent, world);
// Applies weighted sum of active behaviors
```

---

## Cover System

**File:** `SparkEngine/Source/Engine/AI/CoverSystem.h`

Evaluates cover positions for tactical AI:

- Cover point registration (static/dynamic)
- Scoring based on: distance to AI, distance to threat, angle to threat, height
- Occupation tracking (prevent multiple AI claiming same cover)
- Line-of-fire checks via raycasting

---

## Formation System

**File:** `SparkEngine/Source/Engine/AI/FormationSystem.h`

Group movement formations:

| Formation | Description |
|-----------|-------------|
| Line | Side-by-side |
| Column | Single file |
| Wedge | V-shape |
| Circle | Surround point |
| Custom | User-defined offsets |

---

## Group AI

**File:** `SparkEngine/Source/Engine/AI/GroupAI.h`

Squad and swarm coordination:

- Squad leader designation
- Shared blackboard for group state
- Flanking coordination
- Suppressive fire coordination
- Retreat/regroup commands

---

## Environment Query System (EQS)

**File:** `SparkEngine/Source/Engine/AI/EnvironmentQuery.h`

Spatial queries for AI decision-making:

- Generate test points (grid, ring, cone, navmesh sampling)
- Score points with weighted criteria (distance, visibility, cover, elevation)
- Return best position for actions (attack, hide, patrol, flank)

---

## Tactical Point System

**File:** `SparkEngine/Source/Engine/AI/TacticalPointSystem.h`

Strategic position evaluation:

- Pre-placed tactical points (sniper nests, chokepoints, objectives)
- Dynamic evaluation based on game state
- Priority scoring for AI decision trees

---

## AI Budget Limiter

**File:** `SparkEngine/Source/Engine/AI/AIBudgetLimiter.h`

Performance throttling to maintain framerate:

- Configurable per-frame AI time budget
- Priority-based agent scheduling
- Distant/invisible agents updated less frequently
- Graceful degradation under load

---

## Collision Avoidance

**File:** `SparkEngine/Source/Engine/AI/CollisionAvoidance.h`

Reactive avoidance for agent navigation:

- Velocity-based obstacle prediction
- ORCA (Optimal Reciprocal Collision Avoidance) style
- Integration with NavMesh path following

---

## Design Patterns

1. **Opaque Handles** — `BehaviorTreeHandle`, `NavQueryHandle` prevent external code from destabilizing internals
2. **Blackboard** — Bridges perception (writes) and decision-making (reads)
3. **Lazy Instantiation** — Templates registered once, instances cloned per-agent
4. **Path Caching** — 64-entry LRU reduces A* overhead (Redot-inspired)
5. **Cooldowns** — Throttle expensive checks (perception, cover search)
6. **Console Integration** — All systems expose `Console_ListX()` / `Console_GetXInfo()`
