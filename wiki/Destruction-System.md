# Destruction System

SparkEngine provides a destructible environment system for FPS gameplay. Objects can take damage, show progressive damage stages, and fracture into physics-simulated debris when destroyed.

**Source:** `SparkEngine/Source/Engine/Destruction/DestructionSystem.h`

## Architecture

The destruction system is composed of five core types that work together to manage destructible objects from pattern definition through damage application to debris cleanup:

```
┌─────────────────────────────────────────────────────────────────┐
│                        Game Code                                 │
│     (registers patterns, applies damage, listens for events)     │
├─────────────────────────────────────────────────────────────────┤
│                     DestructionSystem                             │
│  ┌──────────────┬─────────────────┬───────────────────────────┐ │
│  │ Pattern      │ Damage          │ Debris                    │ │
│  │ Registry     │ Processing      │ Management                │ │
│  │              │                 │                           │ │
│  │ RegisterPattern()  ApplyDamage()     Update() (cleanup)    │ │
│  │ GetPattern()       ForceDestroy()    SetMaxDebris()        │ │
│  └──────────────┴─────────────────┴───────────────────────────┘ │
├─────────────────────────────────────────────────────────────────┤
│  FracturePattern        DestructibleComponent    DestructionEvent│
│  (piece definitions,    (ECS component: health,  (callback data: │
│   sound, particles)      damage stages, flags)    position, force)│
├─────────────────────────────────────────────────────────────────┤
│            Physics System            Audio System                 │
│        (rigid body debris)       (destruction sounds)             │
└─────────────────────────────────────────────────────────────────┘
```

### Class Overview

| Class | Responsibility |
|-------|---------------|
| `DestructionSystem` | Singleton manager: registers patterns, applies damage, spawns and cleans debris |
| `FracturePattern` | Defines how an object breaks apart: debris pieces, sound effect, particle effect |
| `FracturePiece` | A single debris piece with mesh name, mass, offset, lifetime, and scatter force |
| `DestructibleComponent` | ECS component attached to entities: health, damage stages, resistance, destroyed flag |
| `DestructionEvent` | Event payload passed to destruction callbacks with position, impact direction, force |
| `DebrisInstance` | Internal tracking struct for active debris lifetime management |

### Namespace

All destruction types reside in `namespace Spark`.

## FracturePiece

A single debris piece created when an object is destroyed:

```cpp
struct FracturePiece
{
    std::string name;                          // Piece identifier (e.g. "top", "chunk1")
    std::string meshName;                      // Mesh asset name for this debris
    DirectX::XMFLOAT3 localOffset{0, 0, 0};   // Offset from parent center
    float mass = 1.0f;                         // Physics mass (kg)
    float lifetime = 10.0f;                    // Seconds before debris is cleaned up
    float scatterForce = 5.0f;                 // Force applied at fracture time
};
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `name` | `std::string` | `""` | Unique identifier within the pattern |
| `meshName` | `std::string` | `""` | Asset name for the debris mesh (loaded via [Asset Pipeline](Asset-Pipeline)) |
| `localOffset` | `XMFLOAT3` | `{0,0,0}` | Position offset from parent object center |
| `mass` | `float` | `1.0` | Physics mass in kg; heavier pieces fly shorter distances |
| `lifetime` | `float` | `10.0` | Seconds the debris stays in the world before auto-removal |
| `scatterForce` | `float` | `5.0` | Impulse magnitude applied in the hit direction at spawn |

## FracturePattern

Defines how an object breaks apart -- a collection of debris pieces with associated effects:

```cpp
class FracturePattern
{
public:
    void AddPiece(const FracturePiece& piece);
    const std::vector<FracturePiece>& GetPieces() const;

    void SetDestructionSound(const std::string& sound);
    const std::string& GetDestructionSound() const;

    void SetParticleEffect(const std::string& effect);
    const std::string& GetParticleEffect() const;

private:
    std::vector<FracturePiece> m_pieces;
    std::string m_destructionSound;
    std::string m_particleEffect;
};
```

| Method | Description |
|--------|-------------|
| `AddPiece(piece)` | Append a debris piece to the pattern |
| `GetPieces()` | Get the read-only vector of all pieces |
| `SetDestructionSound(name)` | Set the [audio](Audio) asset played on destruction |
| `GetDestructionSound()` | Get the destruction sound name |
| `SetParticleEffect(name)` | Set the particle effect spawned on destruction |
| `GetParticleEffect()` | Get the particle effect name |

### Creating Fracture Patterns

```cpp
// Simple wooden crate: 2 pieces
FracturePattern crate;
crate.AddPiece(FracturePiece{"top",   "meshTop",  {0, 0.5f, 0}, 2.0f, 10.0f, 5.0f});
crate.AddPiece(FracturePiece{"side",  "meshSide", {0.5f, 0, 0}, 1.5f, 10.0f, 5.0f});
crate.SetDestructionSound("crate_break");
crate.SetParticleEffect("wood_splinters");
destruction.RegisterPattern("wooden_crate", crate);

// Complex brick wall: 5 pieces with varying mass and scatter
FracturePattern brickWall;
brickWall.AddPiece(FracturePiece{"chunk1", "wall_chunk_large", {-0.5f, 0, 0}, 5.0f, 8.0f, 3.0f});
brickWall.AddPiece(FracturePiece{"chunk2", "wall_chunk_large", { 0.5f, 0, 0}, 5.0f, 8.0f, 3.0f});
brickWall.AddPiece(FracturePiece{"brick1", "loose_brick",      {0, 0.5f, 0},  0.5f, 5.0f, 6.0f});
brickWall.AddPiece(FracturePiece{"brick2", "loose_brick",      {0, -0.3f, 0}, 0.5f, 5.0f, 6.0f});
brickWall.AddPiece(FracturePiece{"dust",   "dust_cloud",       {0, 0, 0},     0.1f, 3.0f, 1.0f});
brickWall.SetDestructionSound("wall_collapse");
brickWall.SetParticleEffect("brick_dust");
destruction.RegisterPattern("brick_wall", brickWall);
```

### Pattern Design Guidelines

| Object Type | Recommended Pieces | Mass Range | Scatter Force | Lifetime |
|------------|-------------------|------------|---------------|----------|
| Wooden crate | 2-4 | 0.5 - 3.0 kg | 4.0 - 8.0 | 8 - 12s |
| Brick wall | 4-8 | 0.5 - 10.0 kg | 2.0 - 6.0 | 5 - 10s |
| Glass window | 3-6 | 0.05 - 0.2 kg | 3.0 - 10.0 | 4 - 8s |
| Metal barrel | 2-3 | 2.0 - 5.0 kg | 5.0 - 12.0 | 10 - 15s |
| Concrete pillar | 5-10 | 5.0 - 20.0 kg | 1.0 - 4.0 | 8 - 15s |

## DestructibleComponent

ECS component attached to entities that can be damaged and destroyed:

```cpp
struct DestructibleComponent
{
    float health = 100.0f;                // Current health
    float maxHealth = 100.0f;             // Maximum health
    std::string patternName;              // Fracture pattern to use
    bool isDestroyed = false;             // Whether the object has been destroyed
    bool destructionProcessed = false;    // Whether debris has been spawned

    // Damage resistance
    float damageThreshold = 0.0f;         // Minimum damage to register (ignore scratches)
    float damageMultiplier = 1.0f;        // Multiplier for incoming damage

    // Partial damage states
    int damageStage = 0;                  // Current visual damage stage (0 = pristine)
    int maxDamageStages = 3;              // Number of stages before destruction

    // Method
    bool ApplyDamage(float amount);       // Returns true if destroyed by this damage
};
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `health` | `float` | `100.0` | Current health points |
| `maxHealth` | `float` | `100.0` | Maximum health points |
| `patternName` | `std::string` | `""` | Name of the registered `FracturePattern` |
| `isDestroyed` | `bool` | `false` | Set to true when health reaches zero |
| `destructionProcessed` | `bool` | `false` | Set to true after debris has been spawned |
| `damageThreshold` | `float` | `0.0` | Damage below this value is ignored |
| `damageMultiplier` | `float` | `1.0` | Incoming damage is multiplied by this factor |
| `damageStage` | `int` | `0` | Current visual damage stage (0 = pristine) |
| `maxDamageStages` | `int` | `3` | Total number of visual stages before full destruction |

### Damage Stage Calculation

The `ApplyDamage` method automatically calculates the damage stage based on remaining health:

```
healthPercent = clamp(health / maxHealth, 0.0, 1.0)
damageStage   = clamp(int((1.0 - healthPercent) * maxDamageStages), 0, maxDamageStages)
```

For a wall with `maxHealth = 200` and `maxDamageStages = 3`:

| Health Range | healthPercent | damageStage | Visual State |
|-------------|---------------|-------------|--------------|
| 200 - 134 | 1.00 - 0.67 | 0 | Pristine |
| 133 - 67 | 0.66 - 0.34 | 1 | Cracks visible |
| 66 - 1 | 0.33 - 0.01 | 2 | Chunks missing |
| 0 | 0.00 | 3 | Fully destroyed |

### Damage Resistance

The `damageThreshold` and `damageMultiplier` fields allow tuning how objects respond to different damage amounts:

```cpp
// Armored wall: resists small arms fire, takes double from explosives
auto& wall = world.AddComponent<DestructibleComponent>(entity);
wall.health = 500.0f;
wall.maxHealth = 500.0f;
wall.damageThreshold = 15.0f;    // Ignore pistol shots (< 15 damage)
wall.damageMultiplier = 0.5f;    // Take half damage from bullets
wall.patternName = "armored_wall";

// Glass window: fragile, shatters easily
auto& glass = world.AddComponent<DestructibleComponent>(entity);
glass.health = 10.0f;
glass.maxHealth = 10.0f;
glass.damageThreshold = 0.0f;    // Any damage registers
glass.damageMultiplier = 2.0f;   // Double damage from everything
glass.maxDamageStages = 1;       // No intermediate stages, just shatters
glass.patternName = "glass_window";
```

## DestructionEvent

Event data passed to destruction callbacks:

```cpp
struct DestructionEvent
{
    uint32_t entityId = 0;                    // Entity that was destroyed
    DirectX::XMFLOAT3 position{0, 0, 0};     // World position of destruction
    DirectX::XMFLOAT3 impactDir{0, 0, 0};    // Direction of the destroying hit
    float impactForce = 0.0f;                 // Force of the destroying hit
    std::string patternName;                  // Fracture pattern used
};
```

## DestructionSystem

The main system class that manages patterns, applies damage, and handles debris:

### Constructor and Initialization

```cpp
DestructionSystem();
void Initialize();
void Update(float deltaTime);  // Clean up expired debris
```

### Pattern Management

```cpp
void RegisterPattern(const std::string& name, const FracturePattern& pattern);
const FracturePattern* GetPattern(const std::string& name) const;
```

`RegisterPattern` stores the pattern in an `std::unordered_map<std::string, FracturePattern>`. `GetPattern` returns `nullptr` if the name is not found.

### Damage Application

```cpp
void ApplyDamage(uint32_t entityId, float damage,
                 const DirectX::XMFLOAT3& hitPoint,
                 const DirectX::XMFLOAT3& hitDir);

void ForceDestroy(uint32_t entityId, float force = 10.0f);
```

| Method | Description |
|--------|-------------|
| `ApplyDamage` | Apply damage respecting threshold and multiplier. If health reaches zero, triggers destruction. |
| `ForceDestroy` | Immediately destroy regardless of health. The `force` parameter controls debris scatter intensity. |

### Debris Management

```cpp
size_t GetActiveDebrisCount() const;
void SetMaxDebris(size_t max);                    // Default: 500
void SetDebrisLifetimeMultiplier(float mult);     // Default: 1.0
```

The system tracks all active debris via `std::vector<DebrisInstance>`:

```cpp
struct DebrisInstance  // (private)
{
    uint32_t entityId = 0;
    float remainingLifetime = 10.0f;
};
```

### Callbacks

```cpp
void OnDestruction(std::function<void(const DestructionEvent&)> callback);
```

Multiple callbacks can be registered. All are invoked when any destructible object is destroyed.

### Console Integration

```cpp
std::string Console_GetStatus() const;
```

## Internal Implementation

### Damage Flow

```
ApplyDamage(entityId, damage, hitPoint, hitDir)
  │
  ├── Lookup DestructibleComponent on entity
  │     └── If not found, return silently
  │
  ├── component.ApplyDamage(damage)
  │     ├── Apply damageMultiplier
  │     ├── Check against damageThreshold
  │     ├── Subtract from health
  │     ├── Update damageStage
  │     └── Return true if health <= 0
  │
  ├── If destroyed:
  │     ├── Lookup FracturePattern by patternName
  │     ├── For each FracturePiece in pattern:
  │     │     ├── Spawn debris entity with mesh
  │     │     ├── Apply rigid body with mass
  │     │     ├── Apply impulse = hitDir * scatterForce
  │     │     └── Track as DebrisInstance
  │     │
  │     ├── Play destruction sound (if set)
  │     ├── Spawn particle effect (if set)
  │     ├── Set destructionProcessed = true
  │     ├── Increment m_totalDestructions
  │     │
  │     └── Fire DestructionEvent to all callbacks
  │
  └── If not destroyed:
        └── Update visual damage stage (game code can switch meshes/materials)
```

### Debris Cleanup Flow

```
Update(deltaTime)
  │
  ├── For each DebrisInstance in m_debris:
  │     ├── remainingLifetime -= deltaTime * m_debrisLifetimeMultiplier
  │     ├── If remainingLifetime <= 0:
  │     │     ├── Remove debris entity from world
  │     │     ├── Decrement m_activeDebrisCount
  │     │     └── Remove from vector
  │     └── If m_activeDebrisCount > m_maxDebris:
  │           └── Remove oldest debris to stay under limit
  │
  └── Return
```

### Debris Cap Enforcement

When `m_activeDebrisCount` exceeds `m_maxDebris`, the system removes the oldest debris first (FIFO order). This prevents performance degradation during heavy destruction sequences.

## Quick Start

```cpp
DestructionSystem destruction;
destruction.Initialize();

// Register fracture patterns
FracturePattern crate;
crate.AddPiece(FracturePiece{"top",  "meshTop",  {0, 0.5f, 0}, 2.0f});
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

// Force-destroy regardless of health
destruction.ForceDestroy(entity, 15.0f);
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
wall.maxDamageStages = 3;       // Cracks -> chunks -> collapse

// After applying damage, check damage stage to swap visuals
destruction.ApplyDamage(wallEntity, 60.0f, hitPoint, hitDir);
auto& wallComp = world.GetComponent<DestructibleComponent>(wallEntity);
switch (wallComp.damageStage)
{
    case 0: SetMesh(wallEntity, "wall_pristine"); break;
    case 1: SetMesh(wallEntity, "wall_cracked"); break;
    case 2: SetMesh(wallEntity, "wall_broken"); break;
    case 3: /* Fully destroyed, debris spawned */ break;
}
```

## Destruction Event Handling

```cpp
// Listen for destruction events
destruction.OnDestruction([&](const DestructionEvent& event) {
    // Spawn visual effects at the destruction site
    particleSystem.Emit(event.position, event.patternName + "_particles");
    audio.PlaySound3D("debris_impact", event.position);

    // Publish to event bus for other systems
    bus.Publish(EntityDestroyedEvent{ event.entityId });

    // Update score if player destroyed it
    if (IsPlayerDamageSource(event))
    {
        AddScore(event.entityId, 10);
    }
});
```

## Integration with Weapons

```cpp
// When a bullet hits a destructible object
bus.Subscribe<CollisionEvent>([&](const CollisionEvent& e) {
    if (world.HasComponent<DestructibleComponent>(e.entityB))
    {
        XMFLOAT3 hitPoint = GetCollisionPoint(e);
        XMFLOAT3 hitDir   = GetImpactDirection(e);
        destruction.ApplyDamage(e.entityB, e.impactForce, hitPoint, hitDir);
    }
});
```

## Integration with Explosion System

```cpp
// Radial damage from an explosion
void ApplyExplosionDamage(const XMFLOAT3& center, float radius, float maxDamage)
{
    auto entities = QueryEntitiesInRadius<DestructibleComponent>(center, radius);
    for (auto entity : entities)
    {
        float dist = Distance(center, GetPosition(entity));
        float falloff = 1.0f - (dist / radius);           // Linear falloff
        float damage = maxDamage * std::max(0.0f, falloff);
        XMFLOAT3 dir = Normalize(GetPosition(entity) - center);
        destruction.ApplyDamage(entity, damage, GetPosition(entity), dir);
    }
}

// Grenade explosion
ApplyExplosionDamage(grenadePosition, 10.0f, 150.0f);
```

## Debris Management

### Configuring Debris Limits

```cpp
// Performance: limit total active debris
destruction.SetMaxDebris(500);                    // Cap at 500 pieces (default)
destruction.SetDebrisLifetimeMultiplier(0.5f);    // Half lifetime = faster cleanup

// Query current state
size_t active = destruction.GetActiveDebrisCount();
```

### Debris Lifetime Multiplier

The `m_debrisLifetimeMultiplier` scales the countdown rate for all debris:

| Multiplier | Effect | Use Case |
|-----------|--------|----------|
| `0.25` | 4x faster cleanup | Low-end hardware, mobile |
| `0.5` | 2x faster cleanup | Standard settings |
| `1.0` | Normal lifetime | Default |
| `2.0` | 2x longer lifetime | High-end hardware, cinematic |

## Error Handling

| Scenario | Behavior |
|----------|----------|
| `ApplyDamage` on non-existent entity | Silently returns (no crash) |
| `ApplyDamage` on already-destroyed entity | `ApplyDamage` returns false, no action |
| Unknown `patternName` on component | `GetPattern` returns `nullptr`; destruction occurs without debris |
| `ForceDestroy` on entity without component | Silently returns |
| Damage below `damageThreshold` | Ignored, no health reduction |
| `maxHealth` is zero | Division-by-zero guard: `healthPercent` defaults to `0.0f` |
| Debris count exceeds `m_maxDebris` | Oldest debris removed first (FIFO) |

## Performance Considerations

| Parameter | Default | Description |
|-----------|---------|-------------|
| Max debris | 500 | `m_maxDebris` -- hard cap on active debris entities |
| Debris lifetime multiplier | 1.0 | `m_debrisLifetimeMultiplier` -- scales all lifetimes |
| Pattern lookup | O(1) | `std::unordered_map` keyed by pattern name |
| Debris storage | `std::vector` | Linear scan for cleanup; adequate for typical counts |
| Callback storage | `std::vector` | All callbacks called sequentially on destruction |

### Performance Tips

1. **Limit piece count per pattern.** Each piece spawns a physics rigid body. Keep patterns under 8-10 pieces.
2. **Use short lifetimes for small debris.** Dust clouds and tiny fragments should use 2-4 second lifetimes.
3. **Reduce debris cap on low-end hardware.** `SetMaxDebris(200)` for budget GPUs.
4. **Use `SetDebrisLifetimeMultiplier(0.5f)`** to clean up debris faster during intense combat.
5. **Batch destruction queries.** If many objects might be destroyed in one frame (explosion), the system handles it but consider spreading across frames.

## Thread Safety

The `DestructionSystem` is **not thread-safe**. All methods must be called from the main game thread. The system participates in the [ECS execution order](Entity-Component-System): Physics -> Animation -> AI -> Audio -> Lifecycle -> Render. Destruction processing happens during the Lifecycle phase.

Internal state that is not protected by mutexes:

| Member | Type | Access Pattern |
|--------|------|---------------|
| `m_patterns` | `unordered_map` | Write during init, read during gameplay |
| `m_debris` | `vector` | Read/write every frame in `Update()` |
| `m_destructionCallbacks` | `vector` | Write during init, invoked during gameplay |
| `m_activeDebrisCount` | `size_t` | Atomic-like but not `std::atomic`; main thread only |

## Console Commands

```
destruction_status    # Show system status: patterns registered, active debris,
                      # total destructions, debris cap, lifetime multiplier
```

Example output:

```
=== Destruction System ===
  Patterns registered:  12
  Active debris:        147 / 500
  Total destructions:   83
  Lifetime multiplier:  1.0x
```

## Troubleshooting

| Symptom | Possible Cause | Solution |
|---------|---------------|----------|
| Object takes no damage | `damageThreshold` too high | Lower the threshold or increase weapon damage |
| Object never visually changes | `damageStage` not checked in render code | Switch mesh/material based on `damageStage` value |
| No debris spawned | `patternName` does not match registered pattern | Verify pattern name matches `RegisterPattern()` call |
| Debris floats in air | Physics not applied to spawned debris | Ensure [Physics](Physics) system processes debris rigid bodies |
| Too many debris, FPS drops | `m_maxDebris` too high for hardware | Reduce via `SetMaxDebris()` or increase lifetime multiplier |
| Destruction sound not playing | Sound asset not loaded | Check [Audio](Audio) system; verify sound asset name |
| Callback not firing | Callback registered after destruction | Register callbacks during initialization, before gameplay |
| `ApplyDamage` has no effect | Entity already destroyed (`isDestroyed == true`) | Check `isDestroyed` before applying; use `ForceDestroy` if needed |
| `maxHealth` zero causes NaN | Division by zero in stage calc | Fixed: code guards with `(maxHealth > 0.0f)` check |

---

## See Also

- [Physics](Physics) -- Debris uses rigid body simulation
- [Entity Component System](Entity-Component-System) -- DestructibleComponent
- [Gameplay Systems](Gameplay-Systems) -- Weapons dealing damage to destructibles
- [Audio](Audio) -- Destruction sound effects
- [Rendering and Graphics](Rendering-and-Graphics) -- Damage stage visual switching
- [Event System](Event-System) -- Publishing destruction events
- [Asset Pipeline](Asset-Pipeline) -- Loading debris mesh assets
