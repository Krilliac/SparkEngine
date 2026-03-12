# Destruction System

SparkEngine provides a destructible environment system for FPS gameplay. Objects can take damage, show progressive damage stages, and fracture into physics-simulated debris when destroyed.

**Source:** `SparkEngine/Source/Engine/Destruction/DestructionSystem.h`

## Overview

| Class | Responsibility |
|-------|---------------|
| `DestructionSystem` | Manages patterns, applies damage, spawns/cleans debris |
| `FracturePattern` | Defines how an object breaks apart (debris pieces, sounds, particles) |
| `FracturePiece` | A single debris piece with mesh, mass, and scatter force |
| `DestructibleComponent` | ECS component: health, damage stages, resistance |
| `DestructionEvent` | Event data for destruction callbacks |

## Quick Start

```cpp
DestructionSystem destruction;
destruction.Initialize();

// Register a fracture pattern
FracturePattern crate;
crate.AddPiece(FracturePiece{"top", "meshTop", {0, 0.5f, 0}, 2.0f});
crate.AddPiece(FracturePiece{"side", "meshSide", {0.5f, 0, 0}, 1.5f});
crate.SetDestructionSound("crate_break");
crate.SetParticleEffect("wood_splinters");
destruction.RegisterPattern("wooden_crate", crate);

// Attach to entity via ECS
auto& comp = world.AddComponent<DestructibleComponent>(entity);
comp.health = 50.0f;
comp.maxHealth = 50.0f;
comp.patternName = "wooden_crate";

// Apply damage
destruction.ApplyDamage(entity, 30.0f, hitPoint, hitDirection);
```

## DestructibleComponent

```cpp
struct DestructibleComponent {
    float health = 100.0f;
    float maxHealth = 100.0f;
    std::string patternName;       // Fracture pattern to use
    float damageThreshold = 0.0f;  // Minimum damage to register
    float damageMultiplier = 1.0f; // Incoming damage multiplier
    int damageStage = 0;           // Current visual damage stage
    int maxDamageStages = 3;       // Stages before destruction
};
```

## Damage and Destruction

```cpp
// Gradual damage
destruction.ApplyDamage(entityId, 25.0f, hitPoint, hitDir);

// Force-destroy regardless of health
destruction.ForceDestroy(entityId, 15.0f);

// Listen for destruction events
destruction.OnDestruction([](const DestructionEvent& event) {
    SpawnExplosionEffect(event.position);
});
```

## Debris Management

```cpp
destruction.SetMaxDebris(500);              // Cap active debris count
destruction.SetDebrisLifetimeMultiplier(0.5f); // Faster cleanup
size_t active = destruction.GetActiveDebrisCount();
```

## Multi-Stage Damage Example

```cpp
// Set up a wall with 3 visual damage stages before full destruction
auto& wall = world.AddComponent<DestructibleComponent>(wallEntity);
wall.health          = 200.0f;
wall.maxHealth       = 200.0f;
wall.patternName     = "brick_wall";
wall.damageThreshold = 5.0f;    // Ignore damage below 5
wall.damageMultiplier = 1.0f;
wall.maxDamageStages = 3;       // Cracks → chunks → collapse

// Register a fracture pattern for the wall
FracturePattern brickWall;
brickWall.AddPiece(FracturePiece{"chunk1", "wall_chunk_large", {-0.5f, 0, 0}, 5.0f, 8.0f, 3.0f});
brickWall.AddPiece(FracturePiece{"chunk2", "wall_chunk_large", { 0.5f, 0, 0}, 5.0f, 8.0f, 3.0f});
brickWall.AddPiece(FracturePiece{"brick1", "loose_brick",      {0, 0.5f, 0},  0.5f, 5.0f, 6.0f});
brickWall.AddPiece(FracturePiece{"brick2", "loose_brick",      {0, -0.3f, 0}, 0.5f, 5.0f, 6.0f});
brickWall.AddPiece(FracturePiece{"dust",   "dust_cloud",       {0, 0, 0},     0.1f, 3.0f, 1.0f});
brickWall.SetDestructionSound("wall_collapse");
brickWall.SetParticleEffect("brick_dust");
destruction.RegisterPattern("brick_wall", brickWall);

// React to destruction events
destruction.OnDestruction([&](const DestructionEvent& event) {
    // Spawn visual effects at the destruction site
    particleSystem.Emit(event.position, event.patternName + "_particles");
    audio.PlaySound3D("debris_impact", event.position);

    // Publish to event bus for other systems
    bus.Publish(EntityDestroyedEvent{ event.entityId });
});
```

## Integration with Weapons

```cpp
// When a bullet hits a destructible object
bus.Subscribe<CollisionEvent>([&](const CollisionEvent& e) {
    if (world.HasComponent<DestructibleComponent>(e.entityB)) {
        XMFLOAT3 hitPoint = GetCollisionPoint(e);
        XMFLOAT3 hitDir   = GetImpactDirection(e);
        destruction.ApplyDamage(e.entityB, e.impactForce, hitPoint, hitDir);
    }
});
```

## Console Commands

```
destruction_status    # Show system status and debris count
```

---

## See Also

- [Physics](Physics) — Debris uses rigid body simulation
- [Entity Component System](Entity-Component-System) — DestructibleComponent
- [Gameplay Systems](Gameplay-Systems) — Weapons dealing damage to destructibles
- [Audio](Audio) — Destruction sound effects
