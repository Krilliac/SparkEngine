# AI and Navigation

SparkEngine provides a complete AI framework with behavior trees, NavMesh pathfinding, a perception system, steering behaviors, an environment query system (EQS), an AI Director for dynamic difficulty, and budget-limited processing for large agent counts.

**Source:** `SparkEngine/Source/Engine/AI/`

## Architecture

```
AIIntegratedSystem (top-level ECS system)
  |
  +-- ParallelPerceptionSystem    (R4.1/R4.2: Octree-accelerated, multi-threaded)
  |     +-- SpatialPerceptionIndex (Octree spatial partition)
  |     +-- PerceptionComponent    (per-entity sight/hearing/memory)
  |
  +-- AIBudgetLimiter             (R4.3: frame-budget with priority queue)
  |
  +-- NavMeshObstacleManager      (R4.4: dynamic obstacle carving)
  |
  +-- AISystem (core)
  |     +-- BehaviorTree templates (shared blueprints)
  |     +-- BehaviorTree instances (per-agent clones)
  |     +-- Update loop:
  |           1. UpdatePerception()  -> writes Blackboard entries
  |           2. UpdateBehavior()    -> ticks BehaviorTree
  |           3. UpdateMovement()    -> advances along NavMesh path
  |
  +-- AIDirector                  (dynamic difficulty adjustment)
  |
  +-- EnvironmentQuery (EQS)      (tactical position evaluation)
```

All classes reside in the `Spark::AI` namespace.

## Behavior Trees

The behavior tree framework in `BehaviorTree.h` enables complex NPC decision-making through a tree of reusable nodes.

### Node Status

Every node returns one of three statuses after being ticked:

```cpp
enum class NodeStatus {
    Running,   // Still working (ticked again next frame)
    Success,   // Completed successfully
    Failure    // Could not complete
};
```

### Node Types

| Type | Subclasses | Description |
|------|-----------|-------------|
| **Composite** | `SequenceNode`, `SelectorNode`, `ParallelNode` | Multiple children, evaluated by policy |
| **Decorator** | `InverterNode`, `RepeaterNode` | Single child, modifies result or timing |
| **Leaf (Action)** | `ActionNode`, `WaitNode` | Performs actual work |
| **Leaf (Condition)** | `ConditionNode` | Tests state, returns Success or Failure |

### Composite Nodes

- **SequenceNode** -- Runs children left-to-right. Fails if any child fails. Succeeds when all children succeed.
- **SelectorNode** -- Runs children left-to-right. Succeeds if any child succeeds. Fails when all children fail.
- **ParallelNode** -- Runs all children simultaneously. Configurable success/failure policies.

### Decorator Nodes

- **InverterNode** -- Inverts child result (Success <-> Failure). Running is passed through unchanged.
- **RepeaterNode** -- Repeats child a specified number of times. Returns Running until all repetitions complete.

### Blackboard

The `Blackboard` is a typed key-value store shared by all nodes in a tree, acting as the agent's working memory:

```cpp
Blackboard bb;
bb.Set("enemyVisible", true);
bb.Set("targetPosition", XMFLOAT3{10, 0, 5});
bb.Set("health", 75.0f);
bb.Set("alertLevel", 2);

bool visible = bb.Get<bool>("enemyVisible", false);      // default if not found
XMFLOAT3 pos = bb.Get<XMFLOAT3>("targetPosition", {});
```

The Blackboard supports any type via `std::any`. Common entries written by the AISystem:

| Key | Type | Written By | Description |
|-----|------|------------|-------------|
| `enemyVisible` | `bool` | UpdatePerception | Whether the target is in line-of-sight |
| `targetPosition` | `XMFLOAT3` | UpdatePerception | Last known target world position |
| `targetDistance` | `float` | UpdatePerception | Distance to target |
| `alertLevel` | `int` | UpdatePerception | 0=Idle, 1=Suspicious, 2=Alert, 3=Combat |
| `hasPath` | `bool` | UpdateMovement | Whether a valid NavMesh path exists |
| `pathLength` | `float` | UpdateMovement | Total path length in metres |

### Building a Behavior Tree

```cpp
// Guard behavior: attack if enemy seen, otherwise patrol
auto root = std::make_unique<SelectorNode>("Root");

// Branch 1: attack sequence
auto combatSeq = std::make_unique<SequenceNode>("CombatSeq");
combatSeq->AddChild(std::make_unique<ConditionNode>("EnemyVisible",
    [](const Blackboard& bb) { return bb.Get<bool>("enemyVisible", false); }));
combatSeq->AddChild(std::make_unique<ActionNode>("Attack",
    [](float dt, Blackboard& bb) -> NodeStatus {
        // Attack logic here
        return NodeStatus::Success;
    }));
root->AddChild(std::move(combatSeq));

// Branch 2: patrol fallback
root->AddChild(std::make_unique<ActionNode>("Patrol", patrolFunction));

// Create and tick the tree
auto tree = std::make_unique<BehaviorTree>("GuardBehavior");
tree->SetRoot(std::move(root));

// Each AI update:
tree->Tick(deltaTime);
```

## AISystem

The `AISystem` (`AISystem.h`) is the ECS system that drives all AI agents each frame. It integrates behavior trees, NavMesh, and perception.

### AIComponent

```cpp
struct AIComponent
{
    std::string behaviorTreeName;           // Name of the registered behavior template
    AIAgentConfig config;                   // Per-agent tuning (ranges, speed, accuracy)

    // Opaque handles (managed by AISystem -- do not touch)
    Spark::BehaviorTreeHandle behaviorTreeHandle;
    Spark::NavQueryHandle navQueryHandle;

    // State
    enum class State { Idle, Patrolling, Alert, Combat, Fleeing, Dead };
    State state = State::Idle;

    // Perception
    EntityID targetEntity = entt::null;
    XMFLOAT3 lastKnownTargetPos{0, 0, 0};
    float timeSinceLastSeen = 0.0f;
    float alertTimer = 0.0f;

    // Pathfinding
    std::vector<XMFLOAT3> currentPath;
    size_t currentPathIndex = 0;
    XMFLOAT3 moveTarget{0, 0, 0};
};
```

### AISystem Class Reference

```cpp
class AISystem : public Spark::ECS::ISystem
{
public:
    AISystem();
    ~AISystem() override = default;

    void Update(World& world, float deltaTime) override;
    const char* GetName() const override;   // Returns "AISystem"

    void RegisterBehavior(const std::string& name, std::unique_ptr<BehaviorTree> tree);
    BehaviorTree* CreateBehaviorInstance(const std::string& templateName);

    // Console integration
    std::string Console_ListAgents(World& world) const;
    std::string Console_GetAgentInfo(World& world, EntityID entity) const;
};
```

### Update Loop (per agent, per frame)

1. **Lazy initialization**: Clone behavior tree template if `behaviorTreeHandle` is null.
2. **UpdatePerception**: Determine visible targets, update `targetEntity`, `lastKnownTargetPos`, `timeSinceLastSeen`, `alertTimer`. Write Blackboard entries.
3. **UpdateBehavior**: Tick the behavior tree. Action nodes update `AIComponent::state` and `moveTarget`.
4. **UpdateMovement**: Advance agent along `currentPath`, apply steering, write to `Transform`. If path is stale, issue a new NavMesh query.

Agents with `state == State::Dead` are skipped entirely.

## NavMesh Pathfinding

### Data Model

The NavMesh subsystem has three layers:

| Layer | Classes | Purpose |
|-------|---------|---------|
| Data | `NavMeshData`, `NavVertex`, `NavTriangle` | Raw triangle mesh representing walkable surface |
| Query | `NavMeshQuery` | Per-agent read-only pathfinding and spatial queries |
| Management | `NavMeshBuilder`, `NavMeshManager` | Offline baking and runtime registry |

### NavMeshData

```cpp
struct NavVertex  { XMFLOAT3 position; };

struct NavTriangle
{
    uint32_t indices[3];              // Vertex indices
    uint32_t neighborTriangles[3];    // Adjacent triangle indices (UINT32_MAX = boundary)
    XMFLOAT3 centroid;               // Pre-computed centroid
    XMFLOAT3 normal;                 // Surface normal
    float area;                       // Surface area (m^2)
    uint16_t flags;                   // Surface type bitmask
    std::vector<uint32_t> adjacency;  // Dynamic adjacency (off-mesh connections)
};

struct NavMeshData
{
    std::vector<NavVertex> vertices;
    std::vector<NavTriangle> triangles;
    XMFLOAT3 boundsMin, boundsMax;

    // Build parameters (embedded for reference)
    float cellSize = 0.3f;
    float cellHeight = 0.2f;
    float agentHeight = 2.0f;
    float agentRadius = 0.6f;
    float agentMaxClimb = 0.9f;
    float agentMaxSlope = 45.0f;
};
```

### Triangle Flags

| Bit | Meaning |
|-----|---------|
| 0 | Walkable surface |
| 1 | Water surface (slower movement) |
| 2 | Hazard zone (avoided unless no other path) |

### NavMeshQuery

```cpp
class NavMeshQuery
{
public:
    explicit NavMeshQuery(const NavMeshData* navMesh);

    NavMeshHit FindNearestPoint(const XMFLOAT3& position, float searchRadius = 10.0f) const;
    PathResult FindPath(const PathRequest& request) const;
    NavMeshHit Raycast(const XMFLOAT3& start, const XMFLOAT3& end) const;
    bool IsPointOnNavMesh(const XMFLOAT3& point, float tolerance = 0.5f) const;
    XMFLOAT3 GetRandomPoint() const;
    XMFLOAT3 GetRandomPointInCircle(const XMFLOAT3& center, float radius) const;
};
```

### PathRequest and PathResult

```cpp
struct PathRequest
{
    XMFLOAT3 start;
    XMFLOAT3 end;
    float agentRadius = 0.6f;
    uint16_t includeFlags = 0xFFFF;   // All flags allowed
    uint16_t excludeFlags = 0x0000;   // No exclusions
};

struct PathResult
{
    bool found = false;
    std::vector<PathPoint> path;      // Ordered waypoints from start to goal
    float totalCost = 0.0f;           // Approximate path length in metres
};

struct PathPoint
{
    XMFLOAT3 position;
    uint32_t triangleIndex;
};
```

### NavMeshBuilder

```cpp
class NavMeshBuilder
{
public:
    static std::unique_ptr<NavMeshData> Build(
        const std::vector<XMFLOAT3>& vertices,
        const std::vector<uint32_t>& indices,
        const NavMeshBuildSettings& settings);

    static std::unique_ptr<NavMeshData> BuildFromHeightfield(
        const float* heightData, int width, int height,
        const XMFLOAT3& origin, float cellSize,
        const NavMeshBuildSettings& settings);
};
```

### NavMeshBuildSettings

```cpp
struct NavMeshBuildSettings
{
    float cellSize = 0.3f;           float cellHeight = 0.2f;
    float agentHeight = 2.0f;        float agentRadius = 0.6f;
    float agentMaxClimb = 0.9f;      float agentMaxSlope = 45.0f;
    float regionMinSize = 8.0f;      float regionMergeSize = 20.0f;
    float edgeMaxLen = 12.0f;        float edgeMaxError = 1.3f;
    int   vertsPerPoly = 6;
    float detailSampleDist = 6.0f;   float detailSampleMaxError = 1.0f;
};
```

### NavMeshManager

```cpp
class NavMeshManager
{
public:
    static NavMeshManager& GetInstance();

    bool LoadNavMesh(const std::string& name, const std::string& filepath);
    bool BuildNavMesh(const std::string& name, const std::vector<XMFLOAT3>& vertices,
                      const std::vector<uint32_t>& indices, const NavMeshBuildSettings& settings);
    const NavMeshData* GetNavMesh(const std::string& name) const;
    std::unique_ptr<NavMeshQuery> CreateQuery(const std::string& name) const;
    void RemoveNavMesh(const std::string& name);
    void Clear();

    // Console
    std::string Console_ListNavMeshes() const;
    std::string Console_GetNavMeshInfo(const std::string& name) const;
};
```

### Dynamic Obstacles (NavMeshObstacles.h)

The `NavMeshObstacleManager` tracks axis-aligned box and cylinder obstacles that carve into the NavMesh at runtime:

```cpp
enum class ObstacleShape : uint8_t { Box, Cylinder };

struct ObstacleDesc
{
    ObstacleShape shape = ObstacleShape::Box;
    XMFLOAT3 position{0, 0, 0};
    XMFLOAT3 halfExtents{1, 1, 1};   // For Box
    float radius = 1.0f;              // For Cylinder
    float height = 2.0f;              // For Cylinder
    float margin = 0.0f;              // Extra buffer around the obstacle
};

class NavMeshObstacleManager
{
public:
    static NavMeshObstacleManager& GetInstance();
    void SetNavMesh(NavMeshData* navMesh);

    ObstacleHandle AddObstacle(const ObstacleDesc& desc);
    bool UpdateObstacle(ObstacleHandle handle, const ObstacleDesc& desc);
    bool RemoveObstacle(ObstacleHandle handle);

    bool MarkObstacleDirty(ObstacleHandle handle, const ObstacleDesc& desc);
    void ApplyDirtyObstacles();      // Batch re-carve for moving obstacles

    bool IsTriangleCarved(uint32_t triangleIndex) const;
    size_t GetObstacleCount() const;
    void Clear();
};
```

## Perception System

`PerceptionSystem.h` gives NPCs awareness of their environment through configurable senses.

### PerceptionComponent

```cpp
struct PerceptionComponent
{
    // Sight
    float sightRange = 30.0f;        // Maximum sight distance
    float sightFOV = 120.0f;         // Field of view (degrees, full cone)

    // Hearing
    float hearingRange = 20.0f;      // Maximum hearing distance

    // Memory
    float memoryDecayRate = 0.1f;    // Confidence loss per second
    float minConfidence = 0.05f;     // Prune threshold

    std::unordered_map<PerceptionEntityID, PerceptionMemory> memories;

    // Convenience methods
    void Remember(PerceptionEntityID id, const XMFLOAT3& pos, float time);
    void DecayAndPrune(float currentTime);
    const PerceptionMemory* GetMemory(PerceptionEntityID id) const;
    PerceptionEntityID GetMostConfidentTarget() const;
    void ClearMemories();
};
```

### PerceptionMemory

```cpp
struct PerceptionMemory
{
    XMFLOAT3 lastSeenPosition{0, 0, 0};
    float lastSeenTime = 0.0f;
    float confidence = 0.0f;          // 0..1, decays over time

    void Update(const XMFLOAT3& position, float currentTime);
    void Decay(float currentTime, float decayRate);
    bool IsValid(float minConfidence = 0.05f) const;
};
```

### Perception Functions

```cpp
namespace Perception
{
    bool CanSee(const XMFLOAT3& observerPos, const XMFLOAT3& observerForward,
                const XMFLOAT3& targetPos, float fovDegrees, float maxRange);

    bool CanHear(const XMFLOAT3& listenerPos, const XMFLOAT3& soundPos, float soundRadius);

    bool CanHearWithLoudness(const XMFLOAT3& listenerPos, const XMFLOAT3& soundPos,
                             float soundRadius, float& loudness);

    void UpdatePerception(PerceptionComponent& component, const XMFLOAT3& observerPos,
                          const XMFLOAT3& observerForward, float currentTime,
                          const std::vector<PerceptionEntityID>& targetIds,
                          const std::vector<XMFLOAT3>& targetPositions,
                          const std::vector<XMFLOAT3>& soundPositions = {},
                          const std::vector<float>& soundRadii = {},
                          const std::vector<PerceptionEntityID>& soundSourceIds = {});
}
```

### Octree-Accelerated Perception (Windows)

On Windows, `SpatialPerceptionIndex` uses an Octree for O(log N) broad-phase queries instead of O(N) brute-force:

```cpp
class SpatialPerceptionIndex
{
public:
    SpatialPerceptionIndex(const XMFLOAT3& worldMin, const XMFLOAT3& worldMax);
    void Rebuild(const std::vector<PerceptionEntityID>& ids,
                 const std::vector<XMFLOAT3>& positions, float halfExtent = 0.5f);
    std::vector<PerceptionEntityID> QuerySphere(const XMFLOAT3& center, float radius) const;
    bool GetPosition(PerceptionEntityID id, XMFLOAT3& outPos) const;
};
```

### Parallel Perception (ParallelPerception.h)

The `ParallelPerceptionSystem` dispatches perception queries in parallel via `JobSystem::ParallelFor`:

```cpp
class ParallelPerceptionSystem
{
public:
    void Initialize(const XMFLOAT3& worldMin, const XMFLOAT3& worldMax, int octreeMaxDepth = 6);
    void RebuildSpatialIndex(World& world);
    void AddActiveSound(PerceptionEntityID sourceId, const XMFLOAT3& position, float radius);
    void UpdateAllAgents(World& world, float currentTime);
    void SetMinBatchSize(int batchSize);    // Default: 4
    uint32_t GetLastAgentCount() const;
    size_t GetSpatialEntityCount() const;
};
```

Workflow each frame:
1. `RebuildSpatialIndex()` -- main thread inserts all perceivable entities into the Octree.
2. `GatherAgentData()` -- main thread snapshots per-agent state into `AgentPerceptionJob` structs.
3. `ParallelFor` -- worker threads run sight/hearing checks per agent using Octree queries.
4. `WriteBackResults()` -- main thread copies updated perception data back to ECS components.

## Steering Behaviors

`SteeringBehaviors.h` provides movement algorithms for NPC locomotion. All functions are header-only inline implementations:

| Behavior | Signature | Description |
|----------|-----------|-------------|
| **Seek** | `Seek(position, target, maxSpeed)` | Move directly toward a target position |
| **Flee** | `Flee(position, threat, maxSpeed)` | Move directly away from a threat position |
| **Arrive** | `Arrive(position, target, maxSpeed, slowRadius)` | Seek with smooth deceleration |
| **Pursue** | `Pursue(position, targetPos, targetVelocity, maxSpeed)` | Predict target's future position and intercept |
| **Evade** | `Evade(position, threatPos, threatVelocity, maxSpeed)` | Predict threat's future position and avoid |
| **Wander** | `Wander(position, velocity, wanderRadius, wanderDistance, wanderJitter, rng)` | Random smooth exploration |
| **ObstacleAvoidance** | `ObstacleAvoidance(position, velocity, obstacles, avoidRadius)` | Steer around sphere obstacles |
| **Separation** | `Separation(position, neighbors, separationRadius)` | Repel from nearby neighbors |
| **Alignment** | `Alignment(velocity, neighborVelocities)` | Match average neighbor heading |
| **Cohesion** | `Cohesion(position, neighborPositions)` | Steer toward flock centroid |

### Combining Behaviors

```cpp
XMFLOAT3 force = SteeringBehaviors::Seek(pos, targetPos, maxSpeed);

// Weighted combination for flocking
XMFLOAT3 combined;
combined.x = SteeringBehaviors::Seek(pos, targetPos, maxSpeed).x * 1.0f
           + SteeringBehaviors::Separation(pos, neighbors, 3.0f).x * 1.5f
           + SteeringBehaviors::Alignment(vel, neighborVels).x * 1.0f
           + SteeringBehaviors::Cohesion(pos, neighborPos).x * 0.8f;
// ... same for y and z
```

### Obstacle Struct

```cpp
struct Obstacle
{
    XMFLOAT3 position;
    float radius = 1.0f;
};
```

## Environment Query System (EQS)

`EnvironmentQuery.h` provides a generator/test/scorer pipeline for evaluating tactical positions:

### Pipeline

```
Generator -> Test (filter) -> Scorer (rank) -> Best Position
```

### Generators

| Class | Description |
|-------|-------------|
| `GridGenerator(center, radius, spacing)` | Grid of points within a circle |
| `RingGenerator(center, innerR, outerR, numPoints)` | Points in a ring (donut) shape |
| `ConeGenerator(origin, direction, distance, halfAngle, numPoints)` | Points in a directional arc |

### Tests (Filters)

| Class | Description |
|-------|-------------|
| `DistanceTest(reference, minDist, maxDist)` | Keep points within distance range |
| `CoverTest(threatPos, agentPos, coverRadius, wantCover)` | Keep points in/out of cover |
| `PredicateTest(name, predicate)` | Custom boolean filter |

### Scorers

| Class | Description |
|-------|-------------|
| `DistanceScorer(reference, maxDistance, preferCloser)` | Score by distance |
| `DirectionScorer(origin, preferredDir)` | Score by alignment with direction |
| `PredicateScorer(name, weight, scoreFn)` | Custom scoring function |

### Usage Example

```cpp
EQSQuery query;
query.AddGenerator(std::make_unique<GridGenerator>(agentPos, 15.0f, 2.0f));
query.AddTest(std::make_unique<CoverTest>(threatPos, agentPos, 3.0f, true));
query.AddTest(std::make_unique<DistanceTest>(agentPos, 5.0f, 15.0f));
query.AddScorer(std::make_unique<DistanceScorer>(goalPos, 20.0f, true));

EQSResult result = query.Execute();
if (result.HasValidItem()) {
    EQSItem best = result.GetBestItem();
    agent.MoveTo(best.position);
}

// Get top 3 candidates
auto topItems = result.GetTopItems(3);
```

### EQSResult

```cpp
struct EQSResult
{
    std::vector<EQSItem> items;    // All items (including filtered)
    int totalGenerated = 0;
    int totalFiltered = 0;         // Items that passed all tests
    float queryTimeMs = 0.0f;

    bool HasValidItem() const;
    EQSItem GetBestItem() const;
    std::vector<EQSItem> GetTopItems(int count) const;
};
```

## AI Director

The `AIDirector` (`AIDirector.h`) monitors player performance and dynamically adjusts difficulty to maintain optimal tension:

### Phase Cycle

```
BuildUp (60s) -> Peak (20s) -> Relax (30s) -> BuildUp -> ...
```

```cpp
enum class DirectorPhase { BuildUp, Peak, Relax, Transition };
```

### Player Metrics

```cpp
struct PlayerPerformanceMetrics
{
    float healthPercent = 1.0f;
    float ammoPercent = 1.0f;
    float accuracy = 0.5f;
    int killsRecent = 0;
    int deathsRecent = 0;
    float damageDealtRecent = 0;
    float damageTakenRecent = 0;
    float timeSinceLastCombat = 0;
};
```

### Difficulty Parameters

```cpp
struct DifficultyParameters
{
    float enemySpawnRate = 1.0f;
    float enemyHealthMultiplier = 1.0f;
    float enemyDamageMultiplier = 1.0f;
    float enemyAccuracyMultiplier = 1.0f;
    float resourceSpawnRate = 1.0f;
    int   maxConcurrentEnemies = 10;
    float specialEnemyChance = 0.1f;
};
```

### Usage

```cpp
AIDirector director;
director.SetDesiredIntensityRange(0.3f, 0.7f);
director.SetBuildUpDuration(60.0f);
director.SetPeakDuration(20.0f);
director.SetRelaxDuration(30.0f);

// Per frame:
director.Update(world, deltaTime);
float spawnRate = director.GetCurrentSpawnRate();
float dmgMult   = director.GetDamageMultiplier();
int maxEnemies  = director.GetMaxConcurrentEnemies();
```

## AI Budget Limiter

The `AIBudgetLimiter` (`AIBudgetLimiter.h`) limits AI processing time per frame to a configurable budget (default 4ms):

```cpp
class AIBudgetLimiter
{
public:
    void SetBudgetMs(float budgetMs);            // Default: 4.0
    void SetMaxStaleFrames(uint32_t maxFrames);  // Default: 5
    void SetFarDistanceThreshold(float distance); // Default: 100.0
    void SetStarvationBonus(float bonus);         // Default: 10.0

    void BeginFrame(const XMFLOAT3& playerPosition, World& world);
    bool HasBudgetRemaining() const;
    std::optional<EntityID> GetNextAgent();
    void MarkAgentProcessed(EntityID entityId);
    void EndFrame();

    const BudgetFrameStats& GetLastFrameStats() const;
};
```

### Priority Scoring

```
priority = 1.0 / (1.0 + distanceSq / farThresholdSq) + starvationBonus * staleFrames
```

Agents close to the player and/or starved for updates get higher priority. Agents exceeding `maxStaleFrames` are force-updated regardless of budget.

### BudgetFrameStats

```cpp
struct BudgetFrameStats
{
    uint32_t agentsProcessed = 0;
    uint32_t agentsDeferred = 0;
    uint32_t agentsForceUpdated = 0;
    float timeConsumedMs = 0.0f;
    float budgetMs = 0.0f;
    float BudgetUtilization() const;   // Percentage (0..100+)
};
```

## Integrated AI System

The `AIIntegratedSystem` (`AIIntegration.h`) combines all subsystems into a single ECS system:

```cpp
struct AIIntegrationConfig
{
    XMFLOAT3 worldMin{-500, -500, -500};
    XMFLOAT3 worldMax{500, 500, 500};
    int octreeMaxDepth = 6;
    float budgetMs = 4.0f;
    uint32_t maxStaleFrames = 5;
    bool enableParallelPerception = true;
    bool enableBudgetLimiter = true;
    bool enableNavMeshObstacles = true;
    int minPerceptionBatchSize = 4;
};

class AIIntegratedSystem : public Spark::ECS::ISystem
{
public:
    void Initialize(const AIIntegrationConfig& config = {});
    void Shutdown();
    void Update(World& world, float deltaTime) override;

    AISystem& GetAISystem();
    ParallelPerceptionSystem* GetParallelPerception();
    AIBudgetLimiter* GetBudgetLimiter();
    NavMeshObstacleManager* GetObstacleManager();

    void SetPlayerPosition(const XMFLOAT3& pos);
    std::string Console_GetStatus() const;
};
```

### Frame Execution Flow

```
AIIntegratedSystem::Update(world, dt)
  1. parallelPerception.RebuildSpatialIndex(world)
  2. parallelPerception.UpdateAllAgents(world, time)
  3. obstacleManager.Update()
  4. aiSystem.Update(world, dt)    // behavior + movement
```

## ECS Integration

Use `AIComponent` on entities (see [Entity Component System](Entity-Component-System)):

```cpp
auto& ai = world.AddComponent<AIComponent>(entity);
ai.behaviorTreeName = "GuardBehavior";
ai.config.detectionRange = 20.0f;
ai.config.attackRange = 5.0f;
ai.config.moveSpeed = 3.0f;
ai.state = AIComponent::State::Idle;
```

The `AISystem` ticks behavior trees and updates steering for all entities with `AIComponent` each frame.

## Node Ownership

All node classes use value-semantic ownership via `std::unique_ptr`. There are no raw-pointer ownership relationships in the behavior tree API.

## Performance Considerations

- **Budget limiter**: For large agent counts (100+), enable `AIBudgetLimiter` with a 4ms budget to avoid frame drops. Nearby agents are prioritized.
- **Parallel perception**: Octree-accelerated perception reduces per-agent queries from O(N) to O(log N). Multi-threaded dispatch via JobSystem.
- **NavMesh queries**: Each agent holds its own `NavMeshQuery`; queries are read-only and safe to parallelize against shared `NavMeshData`.
- **Obstacle batching**: For moving obstacles, use `MarkObstacleDirty()` + `ApplyDirtyObstacles()` instead of `UpdateObstacle()` per-obstacle.
- **EQS query time**: Grid generators produce O(R^2/S^2) points where R=radius, S=spacing. Keep generator counts under 500 for real-time use.

## Thread Safety

| Component | Thread Safety |
|-----------|--------------|
| `AISystem` | Main thread only |
| `NavMeshData` | Read-only safe for concurrent `NavMeshQuery` access |
| `NavMeshManager` mutations | Main thread only |
| `NavMeshManager` reads | Concurrent safe after mutations complete |
| `NavMeshObstacleManager` | Main thread only |
| `ParallelPerceptionSystem` | Main thread for `Rebuild`/`WriteBack`; worker threads for perception queries |
| `AIBudgetLimiter` | Main thread only |
| `BehaviorTree` instances | Not thread-safe (one instance per agent, ticked on main thread) |
| `Blackboard` | Not thread-safe (one per agent) |

## Console Commands

| Command | Description |
|---------|-------------|
| `ai_list_agents` | List all active AI agents with state |
| `ai_agent_info <entityId>` | Detailed info for a specific agent |
| `ai_status` | Integrated AI system status |
| `nav_list` | List all registered NavMeshes |
| `nav_info <name>` | NavMesh details (vertices, triangles, bounds) |
| `nav_obstacles` | List active dynamic obstacles |
| `director_status` | AI Director phase, intensity, metrics |
| `director_force_phase <phase>` | Force a specific director phase |
| `director_set_intensity <value>` | Override intensity manually |

## Troubleshooting

### Agent does not move

1. Verify `AIComponent::behaviorTreeName` matches a registered template.
2. Check that `NavMeshManager` has a loaded NavMesh for the current level.
3. Ensure the agent's position is on or near the NavMesh surface.
4. Confirm `AISystem::Update()` is being called each frame.

### Agent ignores the player

1. Check `PerceptionComponent::sightRange` and `sightFOV` values.
2. Verify the player entity is in the perceivable entities list.
3. Inspect the Blackboard (`ai_agent_info`) for `enemyVisible` and `targetDistance`.

### NavMesh path not found

1. Verify start and end positions are within the NavMesh bounds.
2. Check `PathRequest::includeFlags` and `excludeFlags` are not excluding walkable triangles.
3. Ensure there are no disconnected NavMesh islands between start and goal.
4. Try increasing `PathRequest::agentRadius` tolerance.

### Budget limiter starving agents

1. Increase `SetBudgetMs()` if frame time allows.
2. Decrease `SetMaxStaleFrames()` to force-update sooner.
3. Increase `SetStarvationBonus()` to prioritize starved agents more aggressively.

---

## See Also

- [Entity Component System](Entity-Component-System) -- AIComponent
- [Gameplay Systems](Gameplay-Systems) -- NPC and combat systems
- [Animation](Animation) -- NPC animation state machines
- [Physics](Physics) -- Steering and collision avoidance
- [Event System](Event-System) -- AI-related event publishing
- [Terrain and Procedural Generation](Terrain-and-Procedural-Generation) -- NavMesh generation on terrain
- [Scripting with AngelScript](Scripting-with-AngelScript) -- Scripting AI behaviors
