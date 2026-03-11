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
| `Script` | `scriptFile`, `className`, `moduleName` | [AngelScript](Scripting-with-AngelScript) binding |

### [Physics](Physics) Components

| Component | Fields | Description |
|-----------|--------|-------------|
| `RigidBodyComponent` | `mass`, `type` (Static/Kinematic/Dynamic), `linearDamping`, `angularDamping`, `friction`, `restitution`, `physicsBodyHandle` | Rigid body properties |
| `ColliderComponent` | `shapeType` (Box/Sphere/Capsule/...), `dimensions`, `offset`, `isTrigger` | Collision shape |

### [Audio](Audio) Components

| Component | Fields | Description |
|-----------|--------|-------------|
| `AudioSourceComponent` | `soundFile`, `volume`, `pitch`, `loop`, `is3D`, `minDistance`, `maxDistance`, `audioSourceHandle` | Audio source |

### Lighting

| Component | Fields | Description |
|-----------|--------|-------------|
| `LightComponent` | `type` (Point/Directional/Spot), `color`, `intensity`, `range`, `spotAngle`, `castShadows` | Light source |

### [Animation](Animation)

| Component | Fields | Description |
|-----------|--------|-------------|
| `AnimationController` | `currentAnimation`, `playbackSpeed`, `isPlaying`, `loop`, `blendFactor` | Animation state |

### Particles

| Component | Fields | Description |
|-----------|--------|-------------|
| `ParticleEmitterComponent` | `maxParticles`, `emissionRate`, `lifetime`, `startSize`, `endSize`, `startColor`, `endColor`, `velocity`, `gravity` | Particle emitter |

### [AI and Navigation](AI-and-Navigation)

| Component | Fields | Description |
|-----------|--------|-------------|
| `AIComponent` | `behaviorTreeName`, `detectionRadius`, `attackRange`, `moveSpeed`, `state` | AI behavior |

### [Networking](Networking)

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
| `PhysicsUpdateSystem` | Syncs [Physics](Physics) body transforms with ECS transforms |
| `AnimationUpdateSystem` | Evaluates [Animation](Animation) state machines |
| `AudioUpdateSystem` | Updates 3D [Audio](Audio) source positions |
| `NetworkUpdateSystem` | Syncs [networked](Networking) entity state |

## Performance Tips

- Prefer `GetEntitiesWith<A, B>()` views over per-entity queries in hot paths
- Components use SoA layout for cache-friendly iteration of a single component type
- Destroying an entity invalidates iterators — defer destruction until after the iteration loop
- Use `ActiveComponent` to logically disable entities without destroying them

## Thread Safety

The EnTT registry is **not thread-safe**. All World operations must be performed from the main thread unless external synchronization is provided.

---

## See Also

- [Rendering and Graphics](Rendering-and-Graphics) — MeshRenderer and lighting components
- [Audio](Audio) — AudioSourceComponent for spatial audio
- [AI and Navigation](AI-and-Navigation) — AIComponent for behavior trees
- [Event System](Event-System) — Publish/subscribe event bus
- [Physics](Physics) — RigidBody and Collider components
- [Animation](Animation) — Animation controller component
- [Scripting with AngelScript](Scripting-with-AngelScript) — Attach scripts to entities
- [Scene Management](Scene-Management) — Scene hierarchy and serialization

## Component Inventory

<!-- AUTO:component_list -->
| Component | Header |
|-----------|--------|
| `AIComponent` | `SparkEngine/Source/Engine/ECS/Components/AIComponents.h` |
| `AbilityComponent` | `SparkEngine/Source/Engine/ECS/Components/GameplayComponents.h` |
| `ActiveComponent` | `SparkEngine/Source/Engine/ECS/Components/GameplayComponents.h` |
| `AnimationController` | `SparkEngine/Source/Engine/ECS/Components/AnimationComponents.h` |
| `AudioSourceComponent` | `SparkEngine/Source/Engine/ECS/Components/AudioComponents.h` |
| `Camera2D` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `Camera` | `SparkEngine/Source/Engine/ECS/Components/CoreComponents.h` |
| `Collider2D` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `ColliderComponent` | `SparkEngine/Source/Engine/ECS/Components/PhysicsComponents.h` |
| `Config` | `SparkEngine/Source/Engine/ECS/Components/AIComponents.h` |
| `HealthComponent` | `SparkEngine/Source/Engine/ECS/Components/GameplayComponents.h` |
| `InventoryTag` | `SparkEngine/Source/Engine/ECS/Components/GameplayComponents.h` |
| `LightComponent` | `SparkEngine/Source/Engine/ECS/Components/LightComponents.h` |
| `MeshRenderer` | `SparkEngine/Source/Engine/ECS/Components/CoreComponents.h` |
| `NameComponent` | `SparkEngine/Source/Engine/ECS/Components/CoreComponents.h` |
| `NetworkIdentity` | `SparkEngine/Source/Engine/ECS/Components/NetworkComponents.h` |
| `NineSliceSprite` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `ParallaxBackground` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `ParallaxLayer` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `ParticleEmitterComponent` | `SparkEngine/Source/Engine/ECS/Components/AnimationComponents.h` |
| `PixelPerfectComponent` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `QuestTrackerTag` | `SparkEngine/Source/Engine/ECS/Components/GameplayComponents.h` |
| `RigidBody2D` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `RigidBodyComponent` | `SparkEngine/Source/Engine/ECS/Components/PhysicsComponents.h` |
| `Script` | `SparkEngine/Source/Engine/ECS/Components/CoreComponents.h` |
| `SpriteAnimationClip` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `SpriteAnimationFrame` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `SpriteAnimator` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `SpriteRenderer` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `TagComponent` | `SparkEngine/Source/Engine/ECS/Components/GameplayComponents.h` |
| `TilemapComponent` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `TilesetInfo` | `SparkEngine/Source/Engine/ECS/Components/Sprite2DComponents.h` |
| `Transform` | `SparkEngine/Source/Engine/ECS/Components/CoreComponents.h` |
| `WeatherComponent` | `SparkEngine/Source/Engine/ECS/Components/GameplayComponents.h` |
<!-- /AUTO:component_list -->

## System Inventory

<!-- AUTO:system_list -->
| System | Header |
|--------|--------|
| `AISystem` | `SparkEngine/Source/Engine/AI/AISystem.h` |
| `AIUpdateSystem` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` |
| `AchievementSystem` | `SparkEngine/Source/Engine/Stats/AchievementSystem.h` |
| `AnimationUpdateSystem` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` |
| `AudioUpdateSystem` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` |
| `Camera2DFollowSystem` | `SparkEngine/Source/Engine/ECS/Systems/Systems2D.h` |
| `CollisionSystem` | `SparkEngine/Source/Physics/CollisionSystem.h` |
| `DecalSystem` | `SparkEngine/Source/Graphics/DecalSystem.h` |
| `DestructionSystem` | `SparkEngine/Source/Engine/Destruction/DestructionSystem.h` |
| `DialogueSystem` | `SparkEngine/Source/Engine/Dialogue/DialogueSystem.h` |
| `FogSystem` | `SparkEngine/Source/Graphics/FogSystem.h` |
| `GPUParticleSystem` | `SparkEngine/Source/Graphics/GPUParticleSystem.h` |
| `ISystem` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` |
| `JobSystem` | `SparkEngine/Source/Utils/JobSystem.h` |
| `LifecycleSystem` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` |
| `LightChangeReactiveSystem` | `SparkEngine/Source/Engine/ECS/ReactiveSystem.h` |
| `LightingSystem` | `SparkEngine/Source/Graphics/LightingSystem.h` |
| `LocalizationSystem` | `SparkEngine/Source/Engine/Localization/LocalizationSystem.h` |
| `MaterialChangeReactiveSystem` | `SparkEngine/Source/Engine/ECS/ReactiveSystem.h` |
| `MaterialSystem` | `SparkEngine/Source/Graphics/MaterialSystem.h` |
| `ModSystem` | `SparkEngine/Source/Engine/Modding/ModSystem.h` |
| `ParallaxSystem` | `SparkEngine/Source/Engine/ECS/Systems/Systems2D.h` |
| `ParticleSystem` | `SparkEngine/Source/Graphics/GPUParticleSystem.h` |
| `ParticleSystem` | `SparkEngine/Source/Graphics/ParticleSystem.h` |
| `Physics2DUpdateSystem` | `SparkEngine/Source/Engine/ECS/Systems/Systems2D.h` |
| `PhysicsSystem` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` |
| `PhysicsSystem` | `SparkEngine/Source/Physics/PhysicsSystem.h` |
| `PhysicsUpdateSystem` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` |
| `PostProcessingSystem` | `SparkEngine/Source/Graphics/PostProcessingSystem.h` |
| `RagdollSystem` | `SparkEngine/Source/Engine/Animation/RagdollSystem.h` |
| `RenderSystem` | `SparkEngine/Source/Engine/ECS/Systems/ECSystems.h` |
| `ReplaySystem` | `SparkEngine/Source/Engine/Replay/ReplaySystem.h` |
| `SaveSystem` | `SparkEngine/Source/Engine/SaveSystem/SaveSystem.h` |
| `Sprite2DRenderSystem` | `SparkEngine/Source/Engine/ECS/Systems/Systems2D.h` |
| `SpriteAnimatorSystem` | `SparkEngine/Source/Engine/ECS/Systems/Systems2D.h` |
| `TessellationSystem` | `SparkEngine/Source/Graphics/TessellationSystem.h` |
| `TextureSystem` | `SparkEngine/Source/Graphics/TextureSystem.h` |
| `TilemapRenderSystem` | `SparkEngine/Source/Engine/ECS/Systems/Systems2D.h` |
| `UISystem` | `SparkEngine/Source/Engine/UI/UISystem.h` |
| `UpscalingSystem` | `SparkEngine/Source/Graphics/UpscalingSystem.h` |
| `VRSystem` | `SparkEngine/Source/Engine/VR/VRSystem.h` |
| `VisualScriptSystem` | `SparkEngine/Source/Engine/Scripting/VisualScriptSystem.h` |
| `WaterSystem` | `SparkEngine/Source/Graphics/WaterSystem.h` |
| `WeatherSystem` | `SparkEngine/Source/Graphics/WeatherSystem.h` |
<!-- /AUTO:system_list -->
