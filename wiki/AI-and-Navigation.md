# AI and Navigation

SparkEngine provides a complete AI framework with behavior trees, NavMesh pathfinding, a perception system, and steering behaviors.

**Source:** `SparkEngine/Source/Engine/AI/`

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

- **SequenceNode** — Runs children left-to-right. Fails if any child fails. Succeeds when all children succeed.
- **SelectorNode** — Runs children left-to-right. Succeeds if any child succeeds. Fails when all children fail.
- **ParallelNode** — Runs all children simultaneously. Configurable success/failure policies.

### Decorator Nodes

- **InverterNode** — Inverts child result (Success ↔ Failure)
- **RepeaterNode** — Repeats child a specified number of times

### Blackboard

The `Blackboard` is a typed key-value store shared by all nodes in a tree, acting as the agent's working memory:

```cpp
Blackboard bb;
bb.Set("enemyVisible", true);
bb.Set("targetPosition", XMFLOAT3{10, 0, 5});

bool visible = bb.Get<bool>("enemyVisible", false);  // default if not found
```

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

## NavMesh Pathfinding

`NavMesh` provides A* pathfinding on navigation meshes stored in binary `.snav` format.

```cpp
NavMesh navmesh;
navmesh.LoadFromFile("Assets/NavMeshes/Level01.snav");

// Find a path
std::vector<XMFLOAT3> path;
if (navmesh.FindPath(startPos, endPos, path)) {
    // Follow path waypoints
    for (const auto& waypoint : path) {
        // Move toward waypoint
    }
}
```

## Perception System

`PerceptionSystem` gives NPCs awareness of their environment through configurable senses:

### Vision

```cpp
PerceptionConfig config;
config.visionRange = 30.0f;    // Maximum sight distance
config.visionAngle = 90.0f;    // Field of view (degrees)
config.visionHeight = 2.0f;    // Eye height
```

### Hearing

```cpp
config.hearingRange = 15.0f;   // Maximum hearing distance
config.hearingThreshold = 0.1f; // Minimum sound level to detect
```

### Memory

The perception system maintains memory of previously detected entities, with configurable memory duration.

## Steering Behaviors

`SteeringBehaviors` provides movement algorithms for NPC locomotion:

| Behavior | Description |
|----------|-------------|
| **Seek** | Move directly toward a target position |
| **Flee** | Move directly away from a target position |
| **Pursue** | Predict target's future position and intercept |
| **Evade** | Predict target's future position and avoid |
| **Wander** | Random wandering with configurable parameters |
| **Flocking** | Group behavior (separation, alignment, cohesion) |

```cpp
SteeringBehaviors steering;

// Calculate steering force
XMFLOAT3 force = steering.Seek(currentPos, targetPos, currentVelocity, maxSpeed);

// Combine multiple behaviors
XMFLOAT3 combined = steering.Seek(...) * 1.0f
                   + steering.Separation(...) * 1.5f
                   + steering.Alignment(...) * 1.0f;
```

## ECS Integration

Use `AIComponent` on entities (see [[Entity Component System]]):

```cpp
auto& ai = world.AddComponent<AIComponent>(entity);
ai.behaviorTreeName = "GuardBehavior";
ai.detectionRadius  = 20.0f;
ai.attackRange      = 5.0f;
ai.moveSpeed        = 3.0f;
ai.state            = AIState::Idle;
```

The `AISystem` ticks behavior trees and updates steering for all entities with `AIComponent` each frame.

## Node Ownership

All node classes use value-semantic ownership via `std::unique_ptr`. There are no raw-pointer ownership relationships in the behavior tree API.

---

## See Also

- [[Entity Component System]] — AIComponent
- [[Gameplay Systems]] — NPC and combat systems
- [[Animation]] — NPC animation state machines
- [[Physics]] — Steering and collision avoidance
- [[Event System]] — AI-related event publishing
- [[Terrain and Procedural Generation]] — NavMesh generation on terrain
- [[Scripting with AngelScript]] — Scripting AI behaviors
