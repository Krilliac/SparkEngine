# Entity Component System

SparkEngine uses the **EnTT** library for its Entity Component System (ECS). Components are pure POD structs holding state only -- behavior is implemented by Systems that query and mutate components each frame.

**Source:** `SparkEngine/Source/Engine/ECS/Components.h`, `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h`

## Core Concepts

```
┌─────────────────────────────────────────────────────────────────┐
│                       ECS Architecture                          │
│                                                                 │
│  Entity = unique ID (a number, backed by entt::entity)          │
│  Component = plain data struct (Transform, HealthComponent...)  │
│  System = logic class that iterates entities with components    │
│  World = wrapper around EnTT registry managing it all           │
│                                                                 │
│  ┌────────┐  has  ┌────────────┐  processed by  ┌──────────┐  │
│  │ Entity │──────│ Components │───────────────│  Systems  │  │
│  │  (ID)  │      │  (Data)    │               │  (Logic)  │  │
│  └────────┘      └────────────┘               └──────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

By separating data from logic:
- Components are cache-friendly (SoA layout via EnTT)
- Systems can be enabled/disabled without modifying entity data
- New behavior is added by creating a new system, not modifying existing objects

## The World Class

`World` is the central hub for all ECS operations. It wraps the EnTT registry and provides a clean API for entity and component management.

### Entity Lifecycle

```cpp
World world;

// Create an entity with a name
EntityID player = world.CreateEntity("Player");

// Add components
auto& tf = world.AddComponent<Transform>(player);
tf.position = {0.0f, 1.0f, 0.0f};

auto& hp = world.AddComponent<HealthComponent>(player);
hp.maxHealth = 200.0f;
hp.health    = 200.0f;

// Query components
if (world.HasComponent<Transform>(player)) {
    auto& transform = world.GetComponent<Transform>(player);
}

// Remove a component
world.RemoveComponent<HealthComponent>(player);

// Destroy an entity (removes all components)
world.DestroyEntity(player);
```

### World API Reference

| Method | Description |
|--------|-------------|
| `CreateEntity(name)` | Create a new entity with a `NameComponent` |
| `DestroyEntity(id)` | Destroy entity and all attached components |
| `AddComponent<T>(id, args...)` | Attach a component to an entity |
| `GetComponent<T>(id)` | Get a mutable reference to a component |
| `HasComponent<T>(id)` | Check whether an entity has a component |
| `RemoveComponent<T>(id)` | Remove a component from an entity |
| `GetEntitiesWith<A, B, ...>()` | Get a view of all entities matching the component set |

## Iterating Entities

Use `GetEntitiesWith<>()` to iterate all entities matching a component set:

```cpp
// Iterate all entities with both Transform and HealthComponent
for (auto [entity, tf, hp] : world.GetEntitiesWith<Transform, HealthComponent>().each())
{
    hp.health -= 1.0f * deltaTime;
    if (hp.health <= 0.0f) {
        hp.isDead = true;
    }
}
```

This is cache-friendly because EnTT stores components in Structure-of-Arrays (SoA) layout.

### Multi-component Queries

```cpp
// Entities with Transform + MeshRenderer + LightComponent
for (auto [e, tf, mesh, light] :
     world.GetEntitiesWith<Transform, MeshRenderer, LightComponent>().each())
{
    // Update light position from transform
    light.position = tf.position;
}

// Entities with AIComponent + Transform (for NPC movement)
for (auto [e, ai, tf] : world.GetEntitiesWith<AIComponent, Transform>().each())
{
    if (ai.state != AIComponent::State::Dead)
        UpdateAIMovement(ai, tf, deltaTime);
}
```

## Component Reference

### Core Components

**Source:** `SparkEngine/Source/Engine/ECS/Components/CoreComponents.h`

#### EntityID

```cpp
using EntityID = entt::entity;
```

Lightweight handle type backed by `entt::entity`. Used as the unique identifier for every game object.

#### NameComponent

```cpp
struct NameComponent
{
    std::string name;  // Human-readable entity name
};
```

#### Transform

```cpp
struct Transform
{
    XMFLOAT3 position = {0.0f, 0.0f, 0.0f};  // World-space position
    XMFLOAT3 rotation = {0.0f, 0.0f, 0.0f};  // Euler rotation (degrees)
    XMFLOAT3 scale    = {1.0f, 1.0f, 1.0f};  // Scale multiplier

    XMMATRIX GetLocalMatrix() const;   // Compose SRT matrix
    XMMATRIX GetWorldMatrix() const;   // Local matrix (no parent chain in flat ECS)
};
```

#### MeshRenderer

```cpp
struct MeshRenderer
{
    int  meshIndex      = -1;     // Index into mesh asset table
    int  materialIndex  = -1;     // Index into material table
    bool visible        = true;   // Skip rendering if false
    bool castShadows    = true;   // Include in shadow map pass
    bool receiveShadows = true;   // Apply shadow map sampling
};
```

#### Camera

```cpp
struct Camera
{
    float fov       = 70.0f;    // Field of view (degrees)
    float nearPlane = 0.1f;     // Near clipping plane
    float farPlane  = 1000.0f;  // Far clipping plane
    bool  isActive  = false;    // Only one camera active at a time
};
```

#### Script

```cpp
struct Script
{
    std::string scriptFile;   // Path to AngelScript file
    std::string className;    // Class name in the script
    std::string moduleName;   // AngelScript module name
};
```

### Physics Components

**Source:** `SparkEngine/Source/Engine/ECS/Components/PhysicsComponents.h`

| Component | Key Fields | Description |
|-----------|------------|-------------|
| `RigidBodyComponent` | `mass`, `type` (Static/Kinematic/Dynamic), `linearDamping`, `angularDamping`, `friction`, `restitution`, `physicsBodyHandle` | Rigid body properties for Jolt Physics |
| `ColliderComponent` | `shapeType` (Box/Sphere/Capsule/Mesh/ConvexHull), `dimensions`, `offset`, `isTrigger` | Collision shape definition |

### Audio Components

**Source:** `SparkEngine/Source/Engine/ECS/Components/AudioComponents.h`

| Component | Key Fields | Description |
|-----------|------------|-------------|
| `AudioSourceComponent` | `soundFile`, `volume`, `pitch`, `loop`, `is3D`, `minDistance`, `maxDistance`, `audioSourceHandle` | Spatial or 2D audio source |

### Lighting Components

**Source:** `SparkEngine/Source/Engine/ECS/Components/LightComponents.h`

| Component | Key Fields | Description |
|-----------|------------|-------------|
| `LightComponent` | `type` (Point/Directional/Spot), `color`, `intensity`, `range`, `spotAngle`, `castShadows` | Light source parameters |

### Animation Components

**Source:** `SparkEngine/Source/Engine/ECS/Components/AnimationComponents.h`

| Component | Key Fields | Description |
|-----------|------------|-------------|
| `AnimationController` | `currentAnimation`, `playbackSpeed`, `isPlaying`, `loop`, `blendFactor` | Controls animation playback |
| `ParticleEmitterComponent` | `maxParticles`, `emissionRate`, `lifetime`, `startSize`, `endSize`, `startColor`, `endColor`, `velocity`, `gravity` | Particle emission parameters |

### Gameplay Components

**Source:** `SparkEngine/Source/Engine/ECS/Components/GameplayComponents.h`

| Component | Key Fields | Description |
|-----------|------------|-------------|
| `HealthComponent` | `health`, `maxHealth`, `isDead`, `isInvulnerable` | Damageable entity health |
| `TagComponent` | `tag` (string) | Arbitrary tag for filtering |
| `ActiveComponent` | `isActive` (bool) | Enable/disable entity without destroying |
| `AbilityComponent` | -- | Player ability tracking |
| `WeatherComponent` | -- | Weather zone marker |
| `InventoryTag` | -- | Marks entities with inventory |
| `QuestTrackerTag` | -- | Marks quest-tracking entities |

### FPS Components

**Source:** `SparkEngine/Source/Engine/ECS/Components/FPSComponents.h`

| Component | Key Fields | Description |
|-----------|------------|-------------|
| `DecalComponent` | `texturePath`, `category`, `size`, `color`, `lifetime`, `fadeOutDuration`, `receiveLighting`, `sortOrder` | Projected decal texture |
| `ProjectileComponent` | `direction`, `speed`, `gravityScale`, `damage`, `explosionRadius`, `maxRange`, `maxLifetime`, `movementType`, `impactBehavior` | In-flight projectile state |
| `InteractionComponent` | `type`, `displayName`, `actionVerb`, `interactionRadius`, `holdDuration`, `cooldownDuration`, `usesRemaining` | Player-interactable object |

### AI Components

**Source:** `SparkEngine/Source/Engine/ECS/Components/AIComponents.h`

| Component | Key Fields | Description |
|-----------|------------|-------------|
| `AIComponent` | `behaviorTreeName`, `detectionRadius`, `attackRange`, `moveSpeed`, `state` | AI agent behavior |
| `Config` | -- | AI configuration data |

### Network Components

**Source:** `SparkEngine/Source/Engine/ECS/Components/NetworkComponents.h`

| Component | Key Fields | Description |
|-----------|------------|-------------|
| `NetworkIdentity` | `networkId`, `ownerId`, `isLocalPlayer`, `isServerAuthority` | Network replication identity |

### 2D / Sprite Components

**Source:** `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h`

| Component | Description |
|-----------|-------------|
| `SpriteRenderer` | 2D sprite rendering |
| `SpriteAnimator` | Sprite animation controller |
| `SpriteAnimationClip` | Sprite animation clip data |
| `Camera2D` | Orthographic 2D camera |
| `RigidBody2D` | 2D physics body |
| `Collider2D` | 2D collision shape |
| `TilemapComponent` | Tile-based map rendering |
| `ParallaxBackground` / `ParallaxLayer` | Multi-layer parallax scrolling |
| `NineSliceSprite` | Nine-slice scalable sprite |
| `PixelPerfectComponent` | Snap to pixel grid |

## Built-in Systems

### ISystem Base Interface

All ECS systems inherit from `ISystem`:

```cpp
class ISystem
{
public:
    virtual ~ISystem() = default;
    virtual void Update(World& world, float deltaTime) = 0;
    virtual const char* GetName() const = 0;
    virtual bool IsEnabled() const { return m_enabled; }
    virtual void SetEnabled(bool enabled) { m_enabled = enabled; }
protected:
    bool m_enabled = true;
};
```

### System Execution Order

Systems are processed by `SystemManager::UpdateAll()` in registration order. The recommended order:

```
1. PhysicsUpdateSystem    — Step Jolt Physics simulation, sync transforms
2. AnimationUpdateSystem  — Evaluate state machines, blend poses, solve IK
3. AIUpdateSystem         — Perception, behavior trees, pathfinding, movement
4. AudioUpdateSystem      — Update 3D audio source positions from transforms
5. ParticleUpdateSystem   — Spawn, simulate, and cull particles
6. LifecycleSystem        — Process death events, active/inactive toggling
7. DecalSystem            — Manage decal lifetimes and fade-out
8. ProjectileSystem       — Advance projectile positions, check expiration
9. RenderSystem           — Submit draw calls to the GPU
```

### Core System Reference

| System | Constructor | Description |
|--------|-------------|-------------|
| `RenderSystem` | `RenderSystem(GraphicsEngine*)` | Iterates (MeshRenderer + Transform) entities and submits draw calls |
| `PhysicsUpdateSystem` | `PhysicsUpdateSystem(PhysicsSystem*, float fixedTimestep)` | Fixed-timestep physics sync with interpolation |
| `AnimationUpdateSystem` | `AnimationUpdateSystem()` | Evaluates animation state machines, blends layers, solves IK |
| `AIUpdateSystem` | `AIUpdateSystem()` | Ticks behavior trees, updates perception and pathfinding |
| `AudioUpdateSystem` | `AudioUpdateSystem(AudioEngine*)` | Syncs 3D audio positions from transforms |
| `LifecycleSystem` | `LifecycleSystem()` | Monitors health/death, entity activation state |
| `ParticleUpdateSystem` | `ParticleUpdateSystem()` | Advances particle simulation |
| `DecalSystem` | `DecalSystem()` | Manages decal lifetimes and fade-out |
| `ProjectileSystem` | `ProjectileSystem()` | Advances projectile movement and expiration |

### PhysicsUpdateSystem Details

The physics system uses a fixed-timestep accumulator pattern:

```cpp
PhysicsUpdateSystem(PhysicsSystem* physics, float fixedTimestep = 1.0f / 60.0f);

float GetInterpolationAlpha() const;  // For smooth rendering between physics steps
float GetFixedTimestep() const;       // Current fixed timestep value
```

It operates in two phases:
1. **Pre-simulate** -- Write kinematic body positions from ECS Transform to Jolt
2. **Post-simulate** -- Read dynamic body positions from Jolt back to ECS Transform

### LifecycleSystem Callbacks

```cpp
auto* lifecycle = mgr.AddSystem<LifecycleSystem>();
lifecycle->SetDeathCallback([&](EntityID id) {
    // Drop loot, play death sound, award score
    SpawnLoot(id);
    audio.PlaySound("death");
    world.DestroyEntity(id);
});
```

The callback fires once per entity when `HealthComponent::isDead == true`. The entity is NOT automatically destroyed; the callback is responsible for that.

### DecalSystem and ProjectileSystem Callbacks

Both systems support expiration callbacks:

```cpp
auto* decals = mgr.AddSystem<DecalSystem>();
decals->SetExpiredCallback([&](EntityID id) {
    world.DestroyEntity(id);
});

auto* projectiles = mgr.AddSystem<ProjectileSystem>();
projectiles->SetExpiredCallback([&](EntityID id) {
    SpawnImpactEffect(id);
    world.DestroyEntity(id);
});
```

## SystemManager

The `SystemManager` owns, orders, and updates all ECS systems:

```cpp
SystemManager mgr;

// Register systems in execution order
auto* physics = mgr.AddSystem<PhysicsUpdateSystem>(physicsPtr, 1.0f / 60.0f);
auto* anim    = mgr.AddSystem<AnimationUpdateSystem>();
auto* ai      = mgr.AddSystem<AIUpdateSystem>();
auto* audio   = mgr.AddSystem<AudioUpdateSystem>(audioPtr);
auto* render  = mgr.AddSystem<RenderSystem>(graphicsPtr);

// Per-frame update
mgr.UpdateAll(world, deltaTime);

// Disable AI during cutscenes
mgr.GetSystem("AIUpdateSystem")->SetEnabled(false);
```

### SystemManager API

| Method | Description |
|--------|-------------|
| `AddSystem<T>(args...)` | Create and register a system; returns non-owning `T*` |
| `UpdateAll(world, dt)` | Call `Update()` on all enabled systems in order |
| `GetSystem(name)` | Find system by `GetName()` string (linear search) |
| `GetSystemCount()` | Total registered systems (enabled + disabled) |
| `Console_ListSystems()` | Formatted list for debug console |

### Writing a Custom System

```cpp
class MyGravitySystem : public Spark::ECS::ISystem
{
public:
    void Update(World& world, float dt) override
    {
        for (auto [e, rb, tf] :
             world.GetEntitiesWith<RigidBodyComponent, Transform>().each())
        {
            if (rb.type == RigidBodyComponent::Type::Dynamic)
                tf.position.y -= 9.81f * dt * dt * 0.5f;
        }
    }
    const char* GetName() const override { return "MyGravitySystem"; }
};

// Register it
mgr.AddSystem<MyGravitySystem>();
```

## Performance Tips

- Prefer `GetEntitiesWith<A, B>()` views over per-entity queries in hot paths
- Components use SoA layout for cache-friendly iteration of a single component type
- Destroying an entity invalidates iterators -- defer destruction until after the iteration loop
- Use `ActiveComponent` to logically disable entities without destroying them
- Use `SetEnabled(false)` on systems to skip expensive updates during loading or cutscenes
- Keep component structs small; use IDs to reference large data stored elsewhere

## Thread Safety

The EnTT registry is **not thread-safe**. All World operations must be performed from the main thread unless external synchronization is provided. Individual system `Update()` implementations may launch background tasks but must synchronize before modifying the World.

| Operation | Thread Safety |
|-----------|--------------|
| `World::CreateEntity()` | Main thread only |
| `World::DestroyEntity()` | Main thread only |
| `World::AddComponent()` | Main thread only |
| `World::GetComponent()` | Main thread only (read) |
| `GetEntitiesWith<>()` | Main thread only |
| `SystemManager::UpdateAll()` | Main thread only |
| `ISystem::SetEnabled()` | Main thread only |

---

## See Also

- [Rendering and Graphics](Rendering-and-Graphics) -- MeshRenderer and lighting components
- [Audio](Audio) -- AudioSourceComponent for spatial audio
- [AI and Navigation](AI-and-Navigation) -- AIComponent for behavior trees
- [Event System](Event-System) -- Publish/subscribe event bus
- [Physics](Physics) -- RigidBody and Collider components
- [Animation](Animation) -- Animation controller component
- [Scripting with AngelScript](Scripting-with-AngelScript) -- Attach scripts to entities
- [Scene Management](Scene-Management) -- Scene hierarchy and serialization
- [Gameplay Systems](Gameplay-Systems) -- Weapon, projectile, and interaction components

## Component Inventory

<!-- AUTO:component_list -->
| Component | Header |
|-----------|--------|
| `AIComponent` | `SparkEngine/Source/Engine/ECS/Components/AIComponents.h` |
| `AbilityComponent` | `SparkEngine/Source/Engine/ECS/Components/GameplayComponents.h` |
| `ActiveComponent` | `SparkEngine/Source/Engine/ECS/Components/GameplayComponents.h` |
| `AnimationController` | `SparkEngine/Source/Engine/ECS/Components/AnimationComponents.h` |
| `AreaBoundaryComponent` | `SparkEngine/Source/Engine/ECS/Components/PlacementComponents.h` |
| `AudioListenerComponent` | `SparkEngine/Source/Engine/ECS/Components/AdvancedPlacementComponents.h` |
| `AudioReverbZoneComponent` | `SparkEngine/Source/Engine/ECS/Components/VolumeComponents.h` |
| `AudioSourceComponent` | `SparkEngine/Source/Engine/ECS/Components/AudioComponents.h` |
| `BillboardComponent` | `SparkEngine/Source/Engine/ECS/Components/PlacementComponents.h` |
| `BuoyancyVolumeComponent` | `SparkEngine/Source/Engine/ECS/Components/AdvancedPlacementComponents.h` |
| `Camera2D` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `CameraDrawMaskComponent` | `SparkEngine/Source/Engine/ECS/Components/VisibilityComponents.h` |
| `Camera` | `SparkEngine/Source/Engine/ECS/Components/CoreComponents.h` |
| `CharacterControllerComponent` | `SparkEngine/Source/Engine/ECS/Components/AdvancedPlacementComponents.h` |
| `CinematicTriggerComponent` | `SparkEngine/Source/Engine/ECS/Components/PlacementComponents.h` |
| `Collider2D` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `ColliderComponent` | `SparkEngine/Source/Engine/ECS/Components/PhysicsComponents.h` |
| `CollisionMaskComponent` | `SparkEngine/Source/Engine/ECS/Components/CollisionMaskComponents.h` |
| `Config` | `SparkEngine/Source/Engine/ECS/Components/AIComponents.h` |
| `ConstantForceComponent` | `SparkEngine/Source/Engine/ECS/Components/AdvancedPlacementComponents.h` |
| `CoverPointComponent` | `SparkEngine/Source/Engine/ECS/Components/PlacementComponents.h` |
| `DecalComponent` | `SparkEngine/Source/Engine/ECS/Components/FPSComponents.h` |
| `DestructibleComponent` | `SparkEngine/Source/Engine/ECS/Components/PlacementComponents.h` |
| `DialogueTriggerComponent` | `SparkEngine/Source/Engine/ECS/Components/PlacementComponents.h` |
| `FogVolumeComponent` | `SparkEngine/Source/Engine/ECS/Components/VolumeComponents.h` |
| `FoliageVolumeComponent` | `SparkEngine/Source/Engine/ECS/Components/AdvancedPlacementComponents.h` |
| `ForceRegionComponent` | `SparkEngine/Source/Engine/ECS/Components/AdvancedPlacementComponents.h` |
| `HealthComponent` | `SparkEngine/Source/Engine/ECS/Components/GameplayComponents.h` |
| `InteractionComponent` | `SparkEngine/Source/Engine/ECS/Components/FPSComponents.h` |
| `InventoryTag` | `SparkEngine/Source/Engine/ECS/Components/GameplayComponents.h` |
| `LODGroupComponent` | `SparkEngine/Source/Engine/ECS/Components/VolumeComponents.h` |
| `LightComponent` | `SparkEngine/Source/Engine/ECS/Components/LightComponents.h` |
| `LightProbeComponent` | `SparkEngine/Source/Engine/ECS/Components/VolumeComponents.h` |
| `LineRendererComponent` | `SparkEngine/Source/Engine/ECS/Components/AdvancedPlacementComponents.h` |
| `MeshRenderer` | `SparkEngine/Source/Engine/ECS/Components/CoreComponents.h` |
| `NameComponent` | `SparkEngine/Source/Engine/ECS/Components/CoreComponents.h` |
| `NavLinkComponent` | `SparkEngine/Source/Engine/ECS/Components/AdvancedPlacementComponents.h` |
| `NavObstacleComponent` | `SparkEngine/Source/Engine/ECS/Components/VolumeComponents.h` |
| `NavRegionComponent` | `SparkEngine/Source/Engine/ECS/Components/AdvancedPlacementComponents.h` |
| `NetworkIdentity` | `SparkEngine/Source/Engine/ECS/Components/NetworkComponents.h` |
| `NineSliceSprite` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `OccluderComponent` | `SparkEngine/Source/Engine/ECS/Components/PlacementComponents.h` |
| `ParallaxBackground` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `ParallaxLayer` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `ParticleEmitterComponent` | `SparkEngine/Source/Engine/ECS/Components/AnimationComponents.h` |
| `PhysicsJointComponent` | `SparkEngine/Source/Engine/ECS/Components/PlacementComponents.h` |
| `PixelPerfectComponent` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `PostProcessVolumeComponent` | `SparkEngine/Source/Engine/ECS/Components/VolumeComponents.h` |
| `ProjectileComponent` | `SparkEngine/Source/Engine/ECS/Components/FPSComponents.h` |
| `QuestTrackerTag` | `SparkEngine/Source/Engine/ECS/Components/GameplayComponents.h` |
| `RagdollComponent` | `SparkEngine/Source/Engine/ECS/Components/AdvancedPlacementComponents.h` |
| `ReflectionProbeComponent` | `SparkEngine/Source/Engine/ECS/Components/VolumeComponents.h` |
| `RigidBody2D` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `RigidBodyComponent` | `SparkEngine/Source/Engine/ECS/Components/PhysicsComponents.h` |
| `Script` | `SparkEngine/Source/Engine/ECS/Components/CoreComponents.h` |
| `SkyboxComponent` | `SparkEngine/Source/Engine/ECS/Components/AdvancedPlacementComponents.h` |
| `SoftBodyComponent` | `SparkEngine/Source/Engine/ECS/Components/AdvancedPlacementComponents.h` |
| `SpawnPointComponent` | `SparkEngine/Source/Engine/ECS/Components/VolumeComponents.h` |
| `SplineComponent` | `SparkEngine/Source/Engine/ECS/Components/SplineComponents.h` |
| `SplineFollowerComponent` | `SparkEngine/Source/Engine/ECS/Components/SplineComponents.h` |
| `SpringArmComponent` | `SparkEngine/Source/Engine/ECS/Components/AdvancedPlacementComponents.h` |
| `SpriteAnimationClip` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `SpriteAnimationFrame` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `SpriteAnimator` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `SpriteRenderer` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `TacticalPointComponent` | `SparkEngine/Source/Engine/ECS/Components/PlacementComponents.h` |
| `TagComponent` | `SparkEngine/Source/Engine/ECS/Components/GameplayComponents.h` |
| `TerrainComponent` | `SparkEngine/Source/Engine/ECS/Components/TerrainComponents.h` |
| `Text3DComponent` | `SparkEngine/Source/Engine/ECS/Components/AdvancedPlacementComponents.h` |
| `TilemapComponent` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `TilesetInfo` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `TrailRendererComponent` | `SparkEngine/Source/Engine/ECS/Components/AdvancedPlacementComponents.h` |
| `Transform` | `SparkEngine/Source/Engine/ECS/Components/CoreComponents.h` |
| `TriggerVolumeComponent` | `SparkEngine/Source/Engine/ECS/Components/VolumeComponents.h` |
| `VehicleComponent` | `SparkEngine/Source/Engine/ECS/Components/AdvancedPlacementComponents.h` |
| `VisibilityMaskComponent` | `SparkEngine/Source/Engine/ECS/Components/VisibilityComponents.h` |
| `WaterPlaneComponent` | `SparkEngine/Source/Engine/ECS/Components/VolumeComponents.h` |
| `WeatherComponent` | `SparkEngine/Source/Engine/ECS/Components/GameplayComponents.h` |
| `WindZoneComponent` | `SparkEngine/Source/Engine/ECS/Components/PlacementComponents.h` |
<!-- /AUTO:component_list -->

## System Inventory

<!-- AUTO:system_list -->
| System | Header |
|--------|--------|
| `AISystem` | `SparkEngine/Source/Engine/AI/AISystem.h` |
| `AIUpdateSystem` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` |
| `AbilitySystem` | `SparkEngine/Source/Engine/Gameplay/AbilitySystem.h` |
| `AbilityUpdateSystem` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` |
| `AnimationUpdateSystem` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` |
| `AudioUpdateSystem` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` |
| `Camera2DFollowSystem` | `SparkEngine/Source/Engine/ECS/Systems/Systems2D.h` |
| `CollisionSystem` | `SparkEngine/Source/Physics/CollisionSystem.h` |
| `ConditionSystem` | `SparkEngine/Source/Engine/Gameplay/ConditionSystem.h` |
| `CoverSystem` | `SparkEngine/Source/Engine/AI/CoverSystem.h` |
| `DDGIProbeSystem` | `SparkEngine/Source/Graphics/DDGIProbeSystem.h` |
| `DecalSystem` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` |
| `DecalSystem` | `SparkEngine/Source/Graphics/DecalSystem.h` |
| `DestructionSystem` | `SparkEngine/Source/Engine/Destruction/DestructionSystem.h` |
| `DialogueSystem` | `SparkEngine/Source/Engine/Dialogue/DialogueSystem.h` |
| `DynamicResponseSystem` | `SparkEngine/Source/Engine/Dialogue/DynamicResponseSystem.h` |
| `FogSystem` | `SparkEngine/Source/Graphics/FogSystem.h` |
| `FormationSystem` | `SparkEngine/Source/Engine/AI/FormationSystem.h` |
| `FreezeSystem` | `SparkEngine/Source/Engine/SaveSystem/FreezeSystem.h` |
| `GPUParticleSystem` | `SparkEngine/Source/Graphics/GPUParticleSystem.h` |
| `IAnimationSystem` | `SparkEngine/Source/Engine/Animation/IAnimationSystem.h` |
| `ISaveSystem` | `SparkEngine/Source/Engine/SaveSystem/ISaveSystem.h` |
| `IUISystem` | `SparkEngine/Source/Engine/UI/IUISystem.h` |
| `JobSystem` | `SparkEngine/Source/Physics/PhysicsSystem.h` |
| `JobSystem` | `SparkEngine/Source/Utils/JobSystem.h` |
| `LifecycleSystem` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` |
| `LightChangeReactiveSystem` | `SparkEngine/Source/Engine/ECS/ReactiveSystem.h` |
| `LightProbeSystem` | `SparkEngine/Source/Graphics/LightProbeSystem.h` |
| `LightingSystem` | `SparkEngine/Source/Graphics/LightingSystem.h` |
| `LocalizationSystem` | `SparkEngine/Source/Engine/Localization/LocalizationSystem.h` |
| `MaterialChangeReactiveSystem` | `SparkEngine/Source/Engine/ECS/ReactiveSystem.h` |
| `MaterialSystem` | `SparkEngine/Source/Graphics/MaterialSystem.h` |
| `ModSystem` | `SparkEngine/Source/Engine/Modding/ModSystem.h` |
| `MovementSystem` | `SparkEngine/Source/Engine/AI/MovementSystem.h` |
| `ParallaxSystem` | `SparkEngine/Source/Engine/ECS/Systems/Systems2D.h` |
| `ParticleSystem` | `SparkEngine/Source/Graphics/ParticleSystem.h` |
| `ParticleUpdateSystem` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` |
| `Physics2DUpdateSystem` | `SparkEngine/Source/Engine/ECS/Systems/Systems2D.h` |
| `PhysicsSystem` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` |
| `PhysicsSystem` | `SparkEngine/Source/Physics/PhysicsSystem.h` |
| `PhysicsSystem` | `SparkEngine/Source/Physics/PhysicsSystem.h` |
| `PhysicsSystem` | `SparkEngine/Source/Physics/RagdollSystem.h` |
| `PhysicsUpdateSystem` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` |
| `ProbeSystem` | `SparkEngine/Source/Graphics/HybridRT/ProbeSystem.h` |
| `ProjectileSystem` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` |
| `ProximityTriggerSystem` | `SparkEngine/Source/Engine/World/ProximityTriggerSystem.h` |
| `RTHandleSystem` | `SparkEngine/Source/Graphics/RTHandleSystem.h` |
| `RagdollSystem` | `SparkEngine/Source/Engine/Animation/RagdollSystem.h` |
| `RenderSystem` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` |
| `ReplaySystem` | `SparkEngine/Source/Engine/Replay/ReplaySystem.h` |
| `SaveSystem` | `SparkEngine/Source/Engine/SaveSystem/SaveSystem.h` |
| `ShaderVariantSystem` | `SparkEngine/Source/Graphics/ShaderVariantSystem.h` |
| `SplineFollowerSystem` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` |
| `Sprite2DRenderSystem` | `SparkEngine/Source/Engine/ECS/Systems/Systems2D.h` |
| `SpriteAnimatorSystem` | `SparkEngine/Source/Engine/ECS/Systems/Systems2D.h` |
| `TacticalPointSystem` | `SparkEngine/Source/Engine/AI/TacticalPointSystem.h` |
| `TerrainSystem` | `SparkEngine/Source/Engine/ECS/Systems/TerrainSystem.h` |
| `TextureSystem` | `SparkEngine/Source/Graphics/TextureSystem.h` |
| `TilemapRenderSystem` | `SparkEngine/Source/Engine/ECS/Systems/Systems2D.h` |
| `TimeOfDaySystem` | `SparkEngine/Source/Engine/World/TimeOfDaySystem.h` |
| `TweenSystem` | `SparkEngine/Source/Engine/Tween/TweenSystem.h` |
| `UISystem` | `SparkEngine/Source/Engine/UI/UISystem.h` |
| `UpscalingSystem` | `SparkEngine/Source/Graphics/UpscalingSystem.h` |
| `VRSystem` | `SparkEngine/Source/Engine/VR/VRSystem.h` |
| `VirtualFileSystem` | `SparkEngine/Source/Engine/Modding/VirtualFileSystem.h` |
| `WeatherSystem` | `SparkEngine/Source/Graphics/WeatherSystem.h` |
| `WorldOriginSystem` | `SparkEngine/Source/Engine/World/WorldOriginSystem.h` |
<!-- /AUTO:system_list -->
