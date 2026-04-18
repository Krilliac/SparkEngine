# Particle System

SparkEngine includes a CPU-side particle system for visual effects such as explosions, trails, muzzle flashes, sparks, smoke, and environmental particles. Particles are simulated on the CPU and rendered via D3D11 billboard quads with configurable blend modes, color gradients, and size curves. For large-scale GPU-accelerated particles, see [GPU Particles](GPU-Particles.md).

**Source:** `SparkEngine/Source/Graphics/ParticleSystem.h`
**Namespace:** Global
**Editor:** `SparkEditor/Source/Panels/ParticleEditorPanel.h`

---

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Emitter Shapes](#emitter-shapes)
- [Blend Modes](#blend-modes)
- [Emitter Configuration](#emitter-configuration)
  - [Emission](#emission)
  - [Particle Properties](#particle-properties)
  - [Color and Size Over Lifetime](#color-and-size-over-lifetime)
  - [Physics](#physics)
  - [Sub-Emitters](#sub-emitters)
- [Preset Effects](#preset-effects)
- [Code Example](#code-example)
- [Console Commands](#console-commands)
- [Source Files](#source-files)
- [See Also](#see-also)

---

## Overview

The CPU particle system manages a pool of `ParticleEmitter` instances, each containing a fixed-size particle buffer. Every frame the `ParticleSystem` manager updates all active emitters (spawning, simulating, and culling particles) and then renders them as camera-facing billboard quads via a dynamic vertex buffer.

```
┌─────────────────────────────────────────────────────────┐
│                      Game Loop                          │
│         ParticleSystem::Update(dt)                      │
│         ParticleSystem::Render(view, proj)              │
└─────────────────────────┬───────────────────────────────┘
                          │
              ┌───────────▼───────────┐
              │    ParticleSystem     │
              │  CreateEmitter()      │
              │  SpawnExplosion()     │
              │  SpawnMuzzleFlash()   │
              │  Console_* methods    │
              └───────────┬───────────┘
                          │  owns 0..N
              ┌───────────▼───────────┐
              │   ParticleEmitter     │
              │  EmitParticle()       │
              │  UpdateParticle()     │
              │  Play / Stop / Burst  │
              │  D3D11 vertex buffer  │
              └───────────────────────┘
```

- **Simulation space:** World space (particles detach from emitter) or Local space (particles follow emitter)
- **Rendering:** Point-sprite billboards uploaded to a dynamic `ID3D11Buffer` each frame
- **Lifetime:** Each `Particle` tracks `age`, `lifetime`, and `maxLifetime`; dead particles are recycled

---

## Architecture

The system is composed of three layers:

| Layer | Class/Struct | Responsibility |
|-------|-------------|----------------|
| Manager | `ParticleSystem` | Owns all emitters, provides preset effects and console integration |
| Emitter | `ParticleEmitter` | Simulates and renders a pool of particles from a `ParticleEmitterDesc` |
| Data | `Particle`, `ParticleVertex`, `ParticleEmitterDesc` | Per-particle state and emitter configuration |

`ParticleSystem::Initialize()` takes an `ID3D11Device*` and `ID3D11DeviceContext*`. Each `ParticleEmitter` creates its own vertex buffer via `ParticleEmitter::Initialize()`.

---

## Emitter Shapes

The `EmitterShape` enum controls where particles spawn relative to the emitter origin:

| Shape | Description | Relevant Parameters |
|-------|-------------|-------------------|
| `Point` | All particles spawn at a single point | None |
| `Sphere` | Particles spawn on or within a sphere | `shapeRadius` |
| `Cone` | Particles spawn within a cone | `shapeRadius`, `coneAngle` (degrees) |
| `Box` | Particles spawn within an axis-aligned box | `shapeExtents` (half-extents) |
| `Circle` | Particles spawn on or within a circle (2D, XZ plane) | `shapeRadius` |

The spawn position is computed by `GetRandomSpawnPosition()` and the initial velocity direction by `GetRandomSpawnDirection()`, both respecting the chosen shape.

---

## Blend Modes

The `ParticleBlendMode` enum selects the GPU blend state used when rendering particles:

| Mode | Typical Use |
|------|-------------|
| `Additive` | Fire, sparks, glow, energy effects. Adds particle color to the framebuffer. |
| `AlphaBlend` | Smoke, dust, clouds. Standard source-over-destination alpha blending. |
| `Multiply` | Shadow overlays, darkening effects. Multiplies framebuffer by particle color. |
| `Premultiplied` | UI particles, pre-multiplied alpha textures. Assumes RGB is pre-multiplied by alpha. |

---

## Emitter Configuration

All emitter parameters are specified via the `ParticleEmitterDesc` struct.

### Emission

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `emissionRate` | `float` | 10.0 | Particles spawned per second |
| `maxParticles` | `int` | 1000 | Maximum live particles in the pool |
| `burstCount` | `int` | 0 | Particles emitted in a single burst |
| `burstInterval` | `float` | 0.0 | Seconds between bursts (0 = one-shot) |
| `loop` | `bool` | true | Restart after duration expires |
| `playOnAwake` | `bool` | true | Begin playing immediately on creation |
| `prewarm` | `bool` | false | Simulate one full cycle on first frame |
| `duration` | `float` | 0.0 | Total effect duration (0 = infinite when looping) |

### Particle Properties

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `lifetime` | `FloatRange` | 1.0 -- 2.0 | Random lifetime per particle (seconds) |
| `startSpeed` | `FloatRange` | 1.0 -- 5.0 | Random initial speed |
| `startSize` | `FloatRange` | 0.1 -- 0.5 | Random initial billboard size |
| `startRotation` | `FloatRange` | 0.0 -- 6.28 | Random initial rotation (radians) |
| `rotationSpeed` | `FloatRange` | 0.0 -- 0.0 | Angular velocity (radians/sec) |
| `texturePath` | `std::string` | (empty) | Optional billboard texture path |

`FloatRange` stores a `min` and `max`; each particle samples a random value within the range at spawn time.

### Color and Size Over Lifetime

- **`colorOverLife`** -- A `std::vector<ColorKey>` defining an RGBA gradient. Each key has a normalized time `[0..1]` and an `XMFLOAT4` color. Default: white at t=0, transparent white at t=1 (fade out).
- **`sizeOverLife`** -- A `std::vector<std::pair<float, float>>` mapping normalized time to a size multiplier. Default: 1.0 at t=0, 0.0 at t=1 (shrink to nothing).

Both curves are evaluated per-particle each frame via `SampleColorGradient()` and `SampleSizeCurve()`.

### Physics

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `gravity` | `XMFLOAT3` | (0, -9.81, 0) | Gravity vector |
| `gravityMultiplier` | `float` | 0.0 | Scale factor for gravity (0 = no gravity) |
| `drag` | `float` | 0.0 | Velocity damping per second |

### Sub-Emitters

| Field | Type | Description |
|-------|------|-------------|
| `onDeathEmitter` | `std::string` | Emitter name to trigger when a particle dies |
| `onCollisionEmitter` | `std::string` | Emitter name to trigger on particle collision |

Sub-emitters reference other emitters by name within the same `ParticleSystem`.

---

## Preset Effects

`ParticleSystem` provides convenience methods that create pre-configured emitters:

| Method | Description |
|--------|-------------|
| `SpawnExplosion(position, radius)` | Short burst of additive particles expanding outward |
| `SpawnMuzzleFlash(position, direction)` | Cone-shaped flash along a direction |
| `SpawnSparks(position, normal, count)` | Directional sparks bouncing off a surface |
| `SpawnSmoke(position, duration)` | Alpha-blended rising smoke |
| `SpawnTrail(startPos)` | Continuous trail emitter attached to a moving object |

All presets return a `ParticleEmitter*` that can be further customized.

---

## Code Example

### Lifecycle and a custom fire emitter

```cpp
#include "Graphics/ParticleSystem.h"

ParticleSystem particles;
particles.Initialize(device, context);  // D3D11 device + immediate context

ParticleEmitterDesc desc;
desc.name              = "campfire";
desc.emissionRate      = 50.0f;   // particles/sec
desc.maxParticles      = 500;
desc.shape             = EmitterShape::Cone;
desc.coneAngle         = 15.0f;
desc.shapeRadius       = 0.15f;
desc.lifetime          = {0.5f, 1.5f};
desc.startSpeed        = {2.0f, 4.0f};
desc.startSize         = {0.05f, 0.2f};
desc.blendMode         = ParticleBlendMode::Additive;
desc.space             = ParticleSpace::World;
desc.gravity           = {0.0f, 1.0f, 0.0f}; // rises
desc.gravityMultiplier = 0.6f;
desc.drag              = 0.4f;
desc.colorOverLife = {
    {0.0f, {1.0f, 0.8f, 0.2f, 1.0f}},  // bright yellow
    {0.5f, {1.0f, 0.3f, 0.0f, 0.8f}},  // orange
    {1.0f, {0.3f, 0.0f, 0.0f, 0.0f}}   // fade to dark red
};
desc.sizeOverLife = {{0.0f, 0.4f}, {0.3f, 1.0f}, {1.0f, 0.0f}};

ParticleEmitter* fire = particles.CreateEmitter(desc);
fire->SetPosition({10.0f, 0.0f, 5.0f});
fire->Play();
```

### One-shot bursts and sub-emitters

`Burst()` emits `burstCount` particles immediately without advancing the
emission accumulator — useful for muzzle flashes, impacts, and anything
triggered from gameplay code rather than a rate. `onDeathEmitter` chains
emitters together so a dying particle spawns a follow-up effect:

```cpp
// Smoke trail that leaves a spark when each particle expires:
ParticleEmitterDesc spark;
spark.name           = "spark";
spark.emissionRate   = 0.0f;           // burst-driven, no rate
spark.burstCount     = 3;
spark.maxParticles   = 64;
spark.lifetime       = {0.1f, 0.3f};
spark.startSpeed     = {0.5f, 1.5f};
spark.blendMode      = ParticleBlendMode::Additive;
particles.CreateEmitter(spark);

ParticleEmitterDesc trail;
trail.name            = "rocket_trail";
trail.emissionRate    = 100.0f;
trail.shape           = EmitterShape::Point;
trail.lifetime        = {0.4f, 0.8f};
trail.startSize       = {0.05f, 0.1f};
trail.blendMode       = ParticleBlendMode::AlphaBlend;
trail.onDeathEmitter  = "spark";       // chain
particles.CreateEmitter(trail);

if (auto* e = particles.GetEmitter("rocket_trail"))
{
    e->SetPosition(missile.position);
    e->Play();
}
```

### Preset helpers

Convenience spawners create a pre-configured emitter, attach it to the
manager, and return the handle so you can further customise it:

```cpp
// Instant FX at world coordinates:
particles.SpawnExplosion({20, 1, 0}, /*radius=*/5.0f);
particles.SpawnMuzzleFlash(gun.muzzleWorld, gun.forwardDir);
particles.SpawnSparks(hit.point, hit.normal, /*count=*/30);
if (auto* smoke = particles.SpawnSmoke({0, 0, 0}, /*duration=*/4.0f))
    smoke->SetPosition(grenade.position);
```

### Per-frame update + render

```cpp
// Once per frame, after transforms have been committed:
particles.Update(deltaTime);
particles.Render(viewMatrix, projectionMatrix);
```

### Runtime queries and cleanup

```cpp
const int live    = particles.GetTotalActiveParticles();
const int emitters = particles.GetEmitterCount();

// Dump an at-a-glance inventory to the console:
Logger::Info("{}", particles.Console_ListEmitters());

// Stop or destroy:
if (auto* e = particles.GetEmitter("campfire"))
    e->Stop();

particles.DestroyEmitter("rocket_trail");
particles.DestroyAllEmitters();
particles.Shutdown();
```

---

## Console Commands

`ParticleSystem` exposes three `Console_*` methods for runtime debugging:

| Method | Description |
|--------|-------------|
| `Console_ListEmitters()` | Returns a string listing all active emitters with particle counts |
| `Console_GetEmitterInfo(name)` | Returns detailed info for a named emitter |
| `Console_SpawnEffect(type, x, y, z)` | Spawns a preset effect at world coordinates |

Metrics are also available via `GetTotalActiveParticles()` and `GetEmitterCount()`.

---

## Source Files

| File | Description |
|------|-------------|
| `SparkEngine/Source/Graphics/ParticleSystem.h` | `ParticleSystem`, `ParticleEmitter`, `ParticleEmitterDesc`, enums, and data structs |
| `SparkEditor/Source/Panels/ParticleEditorPanel.h` | Editor panel for authoring and previewing particle emitters |
| `SparkEditor/Source/Panels/ParticleEditorPanel.cpp` | Editor panel implementation |

---

## See Also

- [GPU Particles](GPU-Particles.md) -- GPU-accelerated particle system using compute shaders (up to 1M particles)
- [Rendering and Graphics](../subsystems/Rendering-and-Graphics.md) -- Graphics engine that hosts the particle system
- [Day Night Cycle and Weather](../gameplay-tools/Day-Night-Cycle-and-Weather.md) -- Weather effects that can drive environmental particles
- [SparkEditor](../gameplay-tools/SparkEditor.md) -- Editor panels including the Particle Editor
- [Destruction System](../subsystems/Destruction-System.md) -- Debris effects that may spawn particle emitters
