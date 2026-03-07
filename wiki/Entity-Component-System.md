# Entity Component System

SparkEngine uses the **EnTT** library for its Entity Component System (ECS). Components are pure POD structs holding state only — behavior is implemented by Systems that query and mutate components each frame.

**Source:** `SparkEngine/Source/Engine/ECS/Components.h`, `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h`

## Core Concepts

- **Entity** — A lightweight handle (`EntityID`, backed by `entt::entity`) that identifies a game object
- **Component** — A plain data struct attached to an entity (no behavior)
- **System** — A function that iterates entities with specific components and applies logic
- **World** — A wrapper around the EnTT registry that manages entities and components

## The World Class

`World` is the central hub for all ECS operations:

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

// Destroy an entity
world.DestroyEntity(player);
```

## Iterating Entities

Use `GetEntitiesWith<>()` to iterate all entities matching a component set:

```cpp
// Iterate all entities with both Transform and HealthComponent
for (auto [entity, tf, hp] : world.GetEntitiesWith<Transform, HealthComponent>().each())
{
    hp.health -= 1.0f * deltaTime;
    if (hp.health <= 0.0f) {
        // Handle death
    }
}
```

This is cache-friendly because EnTT stores components in Structure-of-Arrays (SoA) layout.

## Component Reference

### Core Components

| Component | Fields | Description |
|-----------|--------|-------------|
| `NameComponent` | `name` (string) | Human-readable entity name |
| `Transform` | `position`, `rotation`, `scale` (XMFLOAT3) | 3D transform |
| `MeshRenderer` | `meshIndex`, `materialIndex`, `visible`, `castShadows`, `receiveShadows` | Mesh rendering |
| `Camera` | `fov`, `nearPlane`, `farPlane`, `isActive` | Camera parameters |
| `Script` | `scriptFile`, `className`, `moduleName` | AngelScript binding |

### Physics Components

| Component | Fields | Description |
|-----------|--------|-------------|
| `RigidBodyComponent` | `mass`, `type` (Static/Kinematic/Dynamic), `linearDamping`, `angularDamping`, `friction`, `restitution`, `physicsBodyHandle` | Rigid body properties |
| `ColliderComponent` | `shapeType` (Box/Sphere/Capsule/...), `dimensions`, `offset`, `isTrigger` | Collision shape |

### Audio Components

| Component | Fields | Description |
|-----------|--------|-------------|
| `AudioSourceComponent` | `soundFile`, `volume`, `pitch`, `loop`, `is3D`, `minDistance`, `maxDistance`, `audioSourceHandle` | Audio source |

### Lighting

| Component | Fields | Description |
|-----------|--------|-------------|
| `LightComponent` | `type` (Point/Directional/Spot), `color`, `intensity`, `range`, `spotAngle`, `castShadows` | Light source |

### Animation

| Component | Fields | Description |
|-----------|--------|-------------|
| `AnimationController` | `currentAnimation`, `playbackSpeed`, `isPlaying`, `loop`, `blendFactor` | Animation state |

### Particles

| Component | Fields | Description |
|-----------|--------|-------------|
| `ParticleEmitterComponent` | `maxParticles`, `emissionRate`, `lifetime`, `startSize`, `endSize`, `startColor`, `endColor`, `velocity`, `gravity` | Particle emitter |

### AI

| Component | Fields | Description |
|-----------|--------|-------------|
| `AIComponent` | `behaviorTreeName`, `detectionRadius`, `attackRange`, `moveSpeed`, `state` | AI behavior |

### Networking

| Component | Fields | Description |
|-----------|--------|-------------|
| `NetworkIdentity` | `networkId`, `ownerId`, `isLocalPlayer`, `isServerAuthority` | Network replication |

### Tags / Metadata

| Component | Fields | Description |
|-----------|--------|-------------|
| `HealthComponent` | `health`, `maxHealth`, `isDead`, `isInvulnerable` | Health tracking |
| `TagComponent` | `tag` (string) | Arbitrary tag |
| `ActiveComponent` | `isActive` (bool) | Enable/disable entity |

## Built-in Systems

Systems in `ECSystems.h` run each frame:

| System | Description |
|--------|-------------|
| `TransformUpdateSystem` | Updates world transforms from local transforms |
| `PhysicsUpdateSystem` | Syncs physics body transforms with ECS transforms |
| `AnimationUpdateSystem` | Evaluates animation state machines |
| `AudioUpdateSystem` | Updates 3D audio source positions |
| `NetworkUpdateSystem` | Syncs networked entity state |

## Performance Tips

- Prefer `GetEntitiesWith<A, B>()` views over per-entity queries in hot paths
- Components use SoA layout for cache-friendly iteration of a single component type
- Destroying an entity invalidates iterators — defer destruction until after the iteration loop
- Use `ActiveComponent` to logically disable entities without destroying them

## Thread Safety

The EnTT registry is **not thread-safe**. All World operations must be performed from the main thread unless external synchronization is provided.

## See Also

- [[Scripting with AngelScript]] — Attach scripts to entities
- [[Physics]] — Physics components in detail
- [[Animation]] — Animation controller component
