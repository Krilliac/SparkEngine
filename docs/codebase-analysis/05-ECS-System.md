# 05 — Entity-Component-System (ECS)

**Location:** `SparkEngine/Source/Engine/ECS/`

Built on **EnTT**, the ECS provides 70+ component types and 11 core systems. Entities are lightweight IDs; components are pure data structs; systems contain all logic.

---

## Architecture

```
ECS/
├── Components/
│   ├── CoreComponents.h          — Transform, MeshRenderer, Camera, Script, NameComponent
│   ├── PhysicsComponents.h       — RigidBodyComponent, ColliderComponent
│   ├── AnimationComponents.h     — AnimationController, ParticleEmitterComponent
│   ├── AIComponents.h            — AIComponent (state, perception, pathfinding)
│   ├── AudioComponents.h         — AudioSourceComponent
│   ├── LightComponents.h         — LightComponent
│   ├── NetworkComponents.h       — NetworkIdentity
│   ├── GameplayComponents.h      — HealthComponent, TagComponent, ActiveComponent
│   ├── SpriteComponents.h        — SpriteRenderer, SpriteAnimator, Camera2D, Tilemap
│   ├── FPSComponents.h           — DecalComponent, ProjectileComponent, InteractionComponent
│   ├── SplineComponents.h        — SplineComponent, SplineFollowerComponent
│   ├── VolumeComponents.h        — TriggerVolume, PostProcessVolume, ReflectionProbe, etc.
│   └── Components.h              — Umbrella header (includes all above)
├── Systems/
│   └── ECSystems.h               — All 11 system implementations
└── World/                        — EnTT registry wrapper (if present)
```

---

## Core Components

### Transform

```cpp
struct Transform {
    XMFLOAT3 position{0, 0, 0};
    XMFLOAT3 rotation{0, 0, 0};    // Euler angles in degrees
    XMFLOAT3 scale{1, 1, 1};
    int32_t parentEntity = -1;       // -1 = root (no parent)
    bool dirty = true;               // Needs world matrix recomputation
};
```

### MeshRenderer

```cpp
struct MeshRenderer {
    std::string meshPath;
    std::string materialPath;
    bool visible = true;
    bool castShadows = true;
    bool receiveShadows = true;
    int renderOrder = 0;
};
```

### Camera

```cpp
struct Camera {
    float fov = 60.0f;          // Field of view (degrees)
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    bool isMainCamera = false;
};
```

### NameComponent

```cpp
struct NameComponent {
    std::string name;
};
```

### Script

```cpp
struct Script {
    std::string scriptPath;
    std::string className;
    std::string moduleName;
};
```

---

## Physics Components

### RigidBodyComponent

```cpp
struct RigidBodyComponent {
    PhysicsBodyType type = PhysicsBodyType::Dynamic;  // Static, Kinematic, Dynamic
    float mass = 1.0f;
    float friction = 0.5f;
    float restitution = 0.3f;
    float linearDamping = 0.01f;
    float angularDamping = 0.05f;
    XMFLOAT3 linearVelocity{0, 0, 0};
    XMFLOAT3 angularVelocity{0, 0, 0};
    PhysicsHandle physicsHandle;  // Opaque handle to Jolt body
};
```

### ColliderComponent

```cpp
struct ColliderComponent {
    CollisionShapeType type = CollisionShapeType::Box;
    float radius = 0.5f;           // Sphere, Capsule, Cylinder
    float height = 1.0f;           // Capsule, Cylinder
    XMFLOAT3 halfExtents{0.5f, 0.5f, 0.5f};  // Box
    XMFLOAT3 offset{0, 0, 0};     // Local offset from entity center
    bool isTrigger = false;
};
```

---

## AI Component

```cpp
struct AIComponent {
    enum class State { Idle, Patrolling, Alert, Combat, Fleeing, Dead };
    State state = State::Idle;

    std::string behaviorTreeName;
    BehaviorTreeHandle behaviorTreeHandle;
    NavQueryHandle navQueryHandle;

    // Perception
    EntityID targetEntity = 0;
    XMFLOAT3 lastKnownTargetPos{0, 0, 0};
    float timeSinceLastSeen = 0.0f;
    float alertTimer = 0.0f;
    Cooldown attackCooldown;
    Cooldown perceptionCooldown;

    // Pathfinding
    std::vector<XMFLOAT3> currentPath;
    size_t currentPathIndex = 0;

    // Configuration
    float detectionRange = 30.0f;
    float attackRange = 15.0f;
    float moveSpeed = 5.0f;
    float reactionTime = 0.5f;
    float accuracy = 0.7f;
};
```

---

## Animation & Particle Components

```cpp
struct AnimationController {
    std::string currentAnimation;
    float playbackSpeed = 1.0f;
    float currentTime = 0.0f;
    bool loop = true;
    float duration = 0.0f;
    float normalizedTime = 0.0f;
    AnimInstanceHandle animInstanceHandle;
};

struct ParticleEmitterComponent {
    std::string effectName;
    float emissionRate = 100.0f;
    float lifetime = 5.0f;
    XMFLOAT4 startColor{1, 1, 1, 1};
    float startSize = 1.0f;
    float startSpeed = 5.0f;
    ParticleHandle emitterHandle;
};
```

---

## Additional Component Categories

### Volume Components
`TriggerVolume`, `PostProcessVolume`, `ReflectionProbe`, `LightProbe`, `NavObstacle`, `WaterPlane`, `FogVolume`, `LODGroup`, `SpawnPoint`, `AudioReverbZone`, `WindZone`, `PhysicsJoint`, `Occluder`, `CoverPoint`, `TacticalPoint`, `Destructible`, `CinematicTrigger`, `DialogueTrigger`, `AreaBoundary`, `Billboard`

### Advanced Components
`AudioListener`, `CharacterController`, `NavRegion`, `NavLink`, `Skybox`, `ConstantForce`, `ForceRegion`, `Ragdoll`, `SoftBody`, `Vehicle`, `BuoyancyVolume`, `SpringArm`, `LineRenderer`, `TrailRenderer`, `Text3D`, `FoliageVolume`

### 2D/Sprite Components
`SpriteRenderer`, `SpriteAnimator`, `Camera2D`, `TilemapComponent`

---

## Systems — Execution Order

**Physics → Animation → AI → Audio → Lifecycle → Render**

### ISystem Interface

```cpp
class ISystem {
public:
    virtual ~ISystem() = default;
    virtual void Update(entt::registry& world, float deltaTime) = 0;
    virtual std::string GetName() const = 0;
    virtual bool IsEnabled() const { return m_enabled; }
    virtual void SetEnabled(bool enabled) { m_enabled = enabled; }
};
```

### SystemManager

```cpp
class SystemManager {
    std::vector<std::unique_ptr<ISystem>> m_systems;  // insertion order preserved
public:
    template <typename T, typename... Args>
    T* AddSystem(Args&&... args);

    void UpdateAll(entt::registry& world, float deltaTime);
    ISystem* GetSystem(const std::string& name);
};
```

### 1. PhysicsUpdateSystem

Syncs ECS Transform ↔ Jolt Physics rigid bodies:

```cpp
void Update(entt::registry& world, float dt) override {
    // Pre-simulate: write Kinematic positions to physics
    auto kinematicView = world.view<Transform, RigidBodyComponent>();
    for (auto entity : kinematicView) {
        auto& [transform, rb] = kinematicView.get(entity);
        if (rb.type == PhysicsBodyType::Kinematic)
            physics->SetBodyPosition(rb.physicsHandle, transform.position);
    }

    // Post-simulate: read Dynamic positions back to Transform
    auto dynamicView = world.view<Transform, RigidBodyComponent>();
    for (auto entity : dynamicView) {
        auto& [transform, rb] = dynamicView.get(entity);
        if (rb.type == PhysicsBodyType::Dynamic)
            transform.position = physics->GetBodyPosition(rb.physicsHandle);
    }
}
```

### 2. AnimationUpdateSystem

Evaluates skeletal animation and uploads bone matrices to GPU:

- Advances clip playback time
- Evaluates state machine transitions
- Blends animation layers
- Solves IK chains
- Uploads `blendResult.finalTransforms` to GPU constant buffer

### 3. AIUpdateSystem

Runs perception, behavior trees, and pathfinding:

- Refreshes target visibility and timers
- Ticks behavior tree instances
- Updates path following via Transform

### 4. AudioUpdateSystem

Syncs 3D audio source positions from Transform:

```cpp
auto view = world.view<AudioSourceComponent, Transform>();
for (auto entity : view) {
    auto& [audio, transform] = view.get(entity);
    if (audio.is3D)
        audioEngine->SetSourcePosition(audio.handle, transform.position);
}
```

### 5. LifecycleSystem

Monitors HealthComponent for death events, respects ActiveComponent state.

### 6. RenderSystem

Submits draw calls to GraphicsEngine:

```cpp
auto view = world.view<MeshRenderer, Transform>();
for (auto entity : view) {
    auto& [mesh, transform] = view.get(entity);
    if (mesh.visible)
        graphics->SubmitMeshForRendering({mesh.meshPath, ComputeWorldMatrix(transform)});
}
```

### 7. ParticleUpdateSystem

Spawns particles, advances positions/velocities/sizes/colors, removes dead particles.

### 8. DecalSystem

Decrements DecalComponent lifetime, computes fade-out, marks expired decals for destruction.

### 9. ProjectileSystem

Advances ProjectileComponent movement, applies gravity, marks expired projectiles.

### 10. SplineFollowerSystem

Advances entities along SplineComponent paths, evaluates position/rotation.

### 11. AbilityUpdateSystem

Ticks AbilityComponent cooldowns each frame.
