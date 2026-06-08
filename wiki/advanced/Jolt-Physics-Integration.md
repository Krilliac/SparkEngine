# Jolt Physics Integration

> **Audience:** Programmers | Mixed
>
> **Thread Context:** Physics runs on the main thread by default; Jolt's `JobSystemThreadPool` dispatches the broad-phase/narrow-phase work across worker threads. The contact listener and body-activation listener are invoked on Jolt's internal threads, so pending-contact and surface-velocity state are mutex-protected.
>
> **Platform/Backend Scope:** Cross-platform. Jolt is the sole physics backend. Builds without Jolt fall back to `PhysicsSystemStub.cpp`.

## Overview

SparkEngine uses [Jolt Physics](https://github.com/jrouwe/JoltPhysics) for 3D rigid-body, character, vehicle, ragdoll, and soft-body simulation. The engine migrated from Bullet Physics to Jolt across roughly seven commits (~6,500 lines). The integration is wrapped behind the engine's own `PhysicsSystem` API rather than exposing Jolt types directly, and a no-Jolt stub (`PhysicsSystemStub.cpp`) keeps the engine buildable without the dependency.

All physics source lives under `SparkEngine/Source/Physics/`.

## What's Implemented

### Core

`PhysicsSystem.h/.cpp`, `PhysicsSystemQueries.cpp`, `PhysicsShapeFactory.cpp`, `PhysicsSpatialQueries.cpp`, `PhysicsConstraints.cpp`, `PhysicsMotorControl.cpp`:

- Full Jolt `PhysicsSystem` lifecycle (Init / Update / Shutdown)
- Multi-threaded job system (`JobSystemThreadPool`)
- Broad-phase layers (NON_MOVING, MOVING, TRIGGER)
- Contact listener with surface-velocity support (conveyor belts)
- Body-activation listener
- Shape cache (hash-based deduplication)
- Fixed-timestep accumulator with interpolation
- Deterministic mode (`PhysicsSettings::mDeterministicSimulation` toggle)
- Binary state serialization (body positions/velocities save/load)

### Shapes

15 types in the `CollisionShapeType` enum: Box, Sphere, Capsule, Cylinder, Cone (convex hull), Mesh, ConvexHull, Heightfield, Compound (static), TaperedCapsule, TaperedCylinder, Plane, Scaled, OffsetCenterOfMass, MutableCompound, Empty.

### Constraints

12 types plus motors: Point2Point, Hinge, Slider, ConeTwist (SwingTwist), 6DOF, Fixed, Distance, Cone, Gear, RackAndPinion, Pulley, Path (Hermite spline). Motors: Hinge velocity/position, Slider velocity/position, Disable.

### Subsystems

- **CharacterController** (`CharacterController.h/.cpp`) — Jolt `CharacterVirtual` (slopes, stairs, ground state, moving platforms)
- **VehiclePhysics** (`VehiclePhysics.h/.cpp`) — wheeled, tracked, motorcycle with engine/transmission/differential
- **RagdollSystem** (`RagdollSystem.h/.cpp`) — 3 modes (Physics, Kinematic, Blended), per-part constraints
- **SoftBodyPhysics** (`SoftBodyPhysics.h/.cpp`) — XPBD cloth/deformable, skinned constraints (cloth-to-skeleton), wind
- **ClothSimulation** (`ClothSimulation.h/.cpp`) — standalone cloth, separate from Jolt soft body
- **GroupFilterTable** — sub-group collision pairs
- **MutableCompoundShape** — runtime add/remove of sub-shapes
- **Buoyancy** — `ApplyBuoyancyImpulse` with a water plane
- **Debug renderer** (`PhysicsDebugRenderer.h/.cpp`) — debug-draw data collector

### Build

- `SPARK_DOUBLE_PRECISION_PHYSICS` CMake option (OFF by default; `CMakeLists.txt:140`) maps to `JPH_DOUBLE_PRECISION` for large worlds
- `PhysicsSystemStub.cpp` provides a no-Jolt fallback; all constraint stubs return a `shared_ptr` with a null Jolt backend, so calls are safe
- All enum-to-string conversions cover every enum value

## ECS Integration

- `PhysicsComponents.h` provides `RigidBodyComponent` and `ColliderComponent`
- The physics update system in the ECS syncs `Transform` ↔ physics
- `PhysicsHandle` is an opaque handle linking ECS entities to `PhysicsBody` instances
- Editor panels: `Physics2DPanel` and `Physics3DPanel` both exist under `SparkEditor/Source/Panels/`

## Implementation Notes

- The `g_physicsSystem` accessor in `PhysicsSystemQueries.cpp` lets `PhysicsBody` methods call back into the system (e.g. `SetPosition` routes through `BodyInterface`).
- The contact listener runs on Jolt's internal threads — it uses a mutex for pending contacts.
- The surface-velocity map is also mutex-protected for contact-listener thread safety.
- `IPhysicsBackend.h` is a forward-looking abstraction. `PhysicsSystem` does **not** currently inherit it — engine code still uses `PhysicsSystem` directly. (See the deferred Phase 8C item in [Reflection & Polymorphism Refactoring Plan](Reflection-Polymorphism-Refactoring-Plan.md).)

## Remaining / Future Work

| Feature | Priority | Notes |
|---------|----------|-------|
| Full `JPH::DebugRenderer` subclass | Medium | Current data collector works; full impl needs graphics-engine wiring |
| Per-triangle `PhysicsMaterial` | Low | Jolt supports it on `MeshShape` but we don't expose it |
| Hair simulation (GPU strands) | Low | 3 Jolt files, very specialized |
| Jolt native `StateRecorder` | Low | Our binary format covers the common case |
| Constraint breaking | Low | `SetBreakingThreshold` stubbed; Jolt has no direct API, needs force monitoring |
| `IPhysicsBackend` inheritance | Low | Reflection plan Phase 8C — deferred (ID-vs-pointer API mismatch) |

## Source & Freshness

- **Original entry date:** 2026-03-22 (`jolt-physics-integration.md`, type: Observation)
- **Verified against codebase 2026-06-08.**
- Status bullets:
  - **Still accurate.** All listed core systems, shapes (15), constraints (12 + motors), and subsystems are present under `SparkEngine/Source/Physics/`.
  - Jolt is referenced ~45 times in `PhysicsSystem.cpp`; `PhysicsSystemStub.cpp` confirms the no-Jolt fallback.
  - `SPARK_DOUBLE_PRECISION_PHYSICS` confirmed at `CMakeLists.txt:140` (OFF by default).
  - Both `Physics2DPanel` and `Physics3DPanel` confirmed present in the editor.
  - The original file's "future work" items remain open; `IPhysicsBackend` is present but `PhysicsSystem` still does not inherit it (confirmed in `IPhysicsBackend.h` header doc).

## Related Pages

- [Reflection & Polymorphism Refactoring Plan](Reflection-Polymorphism-Refactoring-Plan.md) — Phase 8C (physics backend integration)
- [GPU/CPU Separation Plan](GPU-CPU-Separation-Plan.md)
