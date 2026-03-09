# Gameplay Systems

Context: `#prompt:copilot-instructions` for project overview. Console commands: see `console-scripting` prompt.

## Game Module Architecture

Game logic in `SparkGame/` (DLL/SO), loaded at runtime via `IGameModule`. Engine calls `Initialize`, `Update(dt)`, `Render`, `Shutdown`. DLL exports: `CreateGameModule()` / `DestroyGameModule()`.

### SparkGame Structure

```
SparkGame/Source/
  Game/          ← Player, weapons, HUD, terrain, inventory, quests, day-night, weather
  Projectiles/   ← Bullet, Rocket, Grenade with ProjectilePool
  Console/       ← Game-specific console commands
  Enums/         ← Game enumerations
```

## Player & Camera

`SparkEngineCamera` (`Camera/SparkEngineCamera.h`) — FPS, third-person, debug fly modes. Physics-based movement with jump/crouch.

## Weapon & Projectile System

`ProjectilePool` — zero-allocation object pool.

| Class | Behavior |
|-------|----------|
| `Bullet` | Hitscan or fast physics projectile |
| `Rocket` | Arced trajectory, area-of-effect explosion |
| `Grenade` | Physics-simulated, timed or impact detonation |

## AI System (`Engine/AI/AISystem.h`)

| File | System |
|------|--------|
| `BehaviorTree.h` | Composite (sequence/selector/parallel), decorator, action nodes |
| `NavMesh.h` | Binary `.snav` format, A* pathfinding, obstacle avoidance |
| `PerceptionSystem.h` | Vision cones, hearing ranges, memory with decay |
| `SteeringBehaviors.h` | Seek, flee, pursue, evade, wander, flocking |

## Animation System (`Engine/Animation/AnimationSystem.h`)

- Skeletal animation with bone hierarchies (FBX, glTF via Assimp)
- Keyframe clips, state machines with blend transitions
- Multi-layer blending: override, additive, per-bone masks
- IK: two-bone, look-at, FABRIK chain solver
- Root motion extraction and application
- ECS: `AnimationController` component, `AnimationUpdateSystem` runs after physics

## Procedural Generation (`Engine/Procedural/`)

- **Noise**: Perlin, Simplex, Worley, FBM, ridged multifractal, domain warping
- **Terrain**: Heightmap with thermal/hydraulic erosion
- **Meshes**: Plane, box, sphere, cylinder, cone, torus, terrain, rock, tree
- **Placement**: Rule-based scattering (slope, height, density)
- **WFC**: Wave Function Collapse for room/dungeon layouts

## Save System (`Engine/SaveSystem/`)

ECS-aware JSON serialization with miniz compression. Multiple slots, quicksave/quickload, rotating autosaves. Per-component serializer registry for custom types.

## Other Systems

| System | Description |
|--------|-------------|
| HUD | Crosshair, health bar, kill feed, minimap, compass |
| Inventory | Item management, pickup/drop |
| Quests | Tracking, objectives, progression |
| Day/Night | Dynamic time cycle, sun position |
| Weather | Rain, snow, fog, wind |
| Vehicles | Drivable mechanics |
| Interactive | Doors, buttons, levers |
