# Cloth Simulation

SparkEngine provides a position-based dynamics (PBD) cloth simulator for flags, capes, curtains, and other deformable surfaces. The system supports distance constraints, pin constraints, wind forces, and collision with simple shapes.

**Source:** `SparkEngine/Source/Physics/ClothSimulation.h`

## Overview

| Class | Responsibility |
|-------|---------------|
| `ClothSimulation` | Manages all cloth instances, runs PBD solver |
| `ClothDescriptor` | Configuration for creating a cloth (grid size, stiffness, damping) |
| `ClothInstance` | Runtime state: particles, constraints, colliders |
| `ClothParticle` | Single particle with position, velocity, inverse mass |
| `ClothConstraint` | Distance constraint between two particles |
| `ClothCollider` | Simple collider (sphere, capsule, plane) for cloth interaction |

## Quick Start

```cpp
ClothSimulation cloth;

ClothDescriptor desc;
desc.width = 10;
desc.height = 10;
desc.spacing = 0.1f;
desc.mass = 0.5f;
desc.stiffness = 0.9f;
desc.bendStiffness = 0.5f;
desc.damping = 0.01f;
desc.solverIterations = 4;

auto id = cloth.CreateCloth(desc);
cloth.PinParticle(id, 0, {0, 2, 0});     // Pin top-left corner
cloth.PinParticle(id, 9, {0.9f, 2, 0});  // Pin top-right corner
cloth.SetWind(id, {1.0f, 0, 0.5f});

// Per frame:
cloth.Update(deltaTime);
const auto& particles = cloth.GetParticles(id);
// Upload particle positions to mesh for rendering
```

## Cloth Descriptor

| Field | Default | Description |
|-------|---------|-------------|
| `width` | 10 | Particles in X direction |
| `height` | 10 | Particles in Y direction |
| `spacing` | 0.1 | Distance between adjacent particles |
| `mass` | 1.0 | Total cloth mass |
| `stiffness` | 0.9 | Structural constraint stiffness (0-1) |
| `bendStiffness` | 0.5 | Bending constraint stiffness (0-1) |
| `damping` | 0.01 | Velocity damping |
| `solverIterations` | 4 | Constraint solver iterations per frame |

## Colliders

Add simple colliders to prevent cloth from passing through objects:

```cpp
ClothCollider sphere;
sphere.type = ClothCollider::Type::Sphere;
sphere.position = {0, 1, 0};
sphere.radius = 0.3f;
cloth.AddCollider(id, sphere);

ClothCollider ground;
ground.type = ClothCollider::Type::Plane;
ground.position = {0, 0, 0};
ground.normal = {0, 1, 0};
cloth.AddCollider(id, ground);
```

## Pin and Unpin

```cpp
cloth.PinParticle(id, particleIndex, worldPosition);
cloth.UnpinParticle(id, particleIndex);
```

## Performance Notes

- Increase `solverIterations` for stiffer, more stable cloth (at higher CPU cost)
- Reduce grid resolution (`width` x `height`) for distant cloth
- Self-collision is optional and expensive

## Console Commands

```
cloth_status    # Show active cloth instances and particle counts
```

---

## See Also

- [Physics](Physics) — Rigid body simulation
- [Animation](Animation) — Character capes and cloth attachments
- [Rendering and Graphics](Rendering-and-Graphics) — Rendering deformable meshes
