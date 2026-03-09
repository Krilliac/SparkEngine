# Gameplay Systems

Context: `#prompt:copilot-instructions` for project overview.

## Game Module Architecture

Game logic lives in `SparkGame/` (compiled as DLL/SO), loaded at runtime by `GameModuleLoader` via the `IGameModule` interface. The engine calls `Initialize`, `Update(dt)`, `Render`, and `Shutdown`.

### SparkGame Structure

```
SparkGame/Source/
  Game/          ← Player, weapons, HUD, terrain, inventory, quests, day-night, weather
  Projectiles/   ← Bullet, Rocket, Grenade with ProjectilePool
  Console/       ← Game-specific console commands
  Enums/         ← Game enumerations
```

## Player & Camera

- `SparkEngineCamera` (`Camera/SparkEngineCamera.h`) — FPS, third-person, debug fly modes
- First-person controller with physics-based movement, jump, crouch
- Console: `player_speed`, `noclip`, `god_mode`, `teleport <x> <y> <z>`, `camera_mode <fps|third|debug>`

## Weapon & Projectile System

`ProjectilePool` — zero-allocation object pool for high-performance projectile management.

| Class | Behavior |
|-------|----------|
| `Bullet` | Hitscan or fast physics projectile |
| `Rocket` | Arced trajectory, area-of-effect explosion |
| `Grenade` | Physics-simulated with timed or impact detonation |

Console: `spawn_projectile <type>`, `weapon_stats`, `pool_stats`, `explosion_test`, `ballistics_test`

## AI System

`AISystem` (`Engine/AI/AISystem.h`) — orchestrates all AI subsystems.

| File | System |
|------|--------|
| `BehaviorTree.h` | Composite (sequence/selector/parallel), decorator, action nodes |
| `NavMesh.h` | Binary `.snav` format, A* pathfinding, obstacle avoidance |
| `PerceptionSystem.h` | Vision cones, hearing ranges, memory with decay |
| `SteeringBehaviors.h` | Seek, flee, pursue, evade, wander, flocking (separation/alignment/cohesion) |

Console: `ai_debug`, `navmesh_rebuild`, `ai_perception_debug`

## Animation System

`AnimationSystem` (`Engine/Animation/AnimationSystem.h`)

- Skeletal animation with bone hierarchies (FBX, glTF import via Assimp)
- Keyframe clips, animation state machines with blend transitions
- Multi-layer blending: override, additive, layered with per-bone masks
- IK: two-bone, look-at, FABRIK chain solver
- Root motion extraction and application

### ECS Integration

`AnimationController` component stores active state machine. `AnimationUpdateSystem` evaluates each frame after physics.

## Procedural Generation (`Engine/Procedural/`)

- **Noise**: Perlin, Simplex, Worley, FBM, ridged multifractal, domain warping
- **Terrain**: Heightmap generation with thermal/hydraulic erosion
- **Meshes**: Plane, box, sphere, cylinder, cone, torus, terrain, rock, tree
- **Placement**: Rule-based object scattering (slope, height, density constraints)
- **WFC**: Wave Function Collapse for room/dungeon layout generation

## Save System (`Engine/SaveSystem/`)

- ECS-aware serialization: iterates entities, serializes registered components to JSON
- Miniz compression for save files
- Multiple save slots, quicksave/quickload, rotating autosaves
- Per-component serializer registry — register custom serialization for new components

## Other Gameplay Systems

| System | Location | Description |
|--------|----------|-------------|
| HUD | `SparkGame/Source/Game/` | Crosshair, health bar, kill feed, minimap, compass |
| Inventory | `SparkGame/Source/Game/` | Item management, pickup/drop |
| Quests | `SparkGame/Source/Game/` | Quest tracking, objectives, progression |
| Day/Night | `SparkGame/Source/Game/` | Dynamic time cycle, sun position |
| Weather | `SparkGame/Source/Game/` | Rain, snow, fog, wind effects |
| Vehicles | `SparkGame/Source/Game/` | Drivable vehicle mechanics |
| Interactive | `SparkGame/Source/Game/` | Doors, buttons, levers |
