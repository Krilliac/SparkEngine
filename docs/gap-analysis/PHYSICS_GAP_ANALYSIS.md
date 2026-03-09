# SparkEngine Physics — Gap Analysis

> **Scope**: `SparkEngine/Source/Physics/` and physics-related ECS components
> **Date**: 2026-03-09
> **Methodology**: Static source-code audit of all `.h`/`.cpp` files under `Physics/` and related ECS integration.
> Each gap is assigned a severity: **Critical**, **Major**, **Moderate**, **Minor**.

---

## Context

The PhysicsSystem is one of the more complete subsystems in SparkEngine. It wraps Bullet Physics 3 with a DirectXMath-native API, supports multiple body types (Static, Kinematic, Dynamic), collision shapes (Box, Sphere, Capsule, Cylinder, Cone, Mesh, ConvexHull, Heightfield, Compound), constraints (Hinge, Slider, Fixed), raycasts, overlap queries, and debug rendering. The `.cpp` has real implementations backed by Bullet. However, several gaps remain.

---

## Critical Gaps

### GAP-P01 — No Character Controller

**Files**: `Physics/PhysicsSystem.h`, `Physics/PhysicsSystem.cpp`

**Impact**: For an FPS engine, character movement through the physics world is fundamental. Bullet Physics provides `btKinematicCharacterController` but it is not exposed. Players and AI agents must currently use Dynamic rigid bodies for movement, which produces floaty, imprecise FPS movement.

**Evidence**: No `CharacterController` class, no `btKinematicCharacterController` include, no step-over/slope-limit/ground-detection logic.

**What is needed**: Implement a `CharacterController` wrapping `btKinematicCharacterController` or a custom kinematic sweep-based controller with:
- Step height and slope limit
- Ground detection (isGrounded)
- Smooth movement with collision response
- Push-back from dynamic objects
- Integration with the ECS via a `CharacterControllerComponent`

---

### GAP-P02 — Global Physics Pointer Instead of Service Locator

**File**: `Physics/PhysicsSystem.cpp` (line 21)

**Evidence**:
```cpp
PhysicsSystem* g_physicsSystem = nullptr;
```

**Impact**: The physics system uses a raw global pointer, violating the project's own convention of using `EngineContext` service locator. This creates initialization order dependencies and makes testing difficult.

**What is needed**: Register `PhysicsSystem` with `EngineContext` and access it through the service locator pattern used elsewhere in the engine.

---

## Major Gaps

### GAP-P03 — CollisionSystem Has 8 Stub Patterns

**Files**:
- `Physics/CollisionSystem.h` (7 stub returns)
- `Physics/CollisionSystem.cpp` (1 stub return)

**Impact**: The CollisionSystem appears to be a higher-level collision event dispatch system separate from PhysicsSystem's raw collision callbacks. With 7+ stubs in the header, collision layer filtering, collision matrix configuration, and event-based collision dispatch are non-functional.

**What is needed**: Implement collision layers (bitmask-based), collision matrix (which layers interact), and per-pair collision callbacks with enter/stay/exit semantics.

---

### GAP-P04 — No Trigger Volume System

**Files**: `Physics/PhysicsSystem.h`

**Impact**: While PhysicsSystem declares trigger callbacks, there is no dedicated trigger volume abstraction. FPS games heavily rely on trigger zones (damage zones, pickup areas, objective zones, level transitions). Currently you must manually create ghost objects via Bullet.

**What is needed**: A `TriggerVolume` component/class with:
- Box, sphere, and capsule shapes
- OnEnter / OnStay / OnExit callbacks per entity
- Tag-based filtering (e.g., only "Player" entities trigger)
- Integration with the ECS

---

### GAP-P05 — No Vehicle Physics

**Files**: `Physics/PhysicsSystem.h`

**Impact**: Bullet Physics has `btRaycastVehicle` for vehicle simulation but it is not exposed. While not core to FPS, vehicle sections are common in modern FPS games.

**What is needed**: If vehicle gameplay is planned, wrap `btRaycastVehicle` with suspension, wheel, and engine parameters. Otherwise, document this as out-of-scope.

---

### GAP-P06 — Physics Debug Rendering Not Connected to Graphics Engine

**Files**: `Physics/PhysicsSystem.h` (DebugDrawer class)

**Impact**: A custom `DebugDrawer` inheriting from `btIDebugDraw` is declared, but it needs to be connected to the graphics engine's line-drawing system. Without this, physics debug visualization (wireframe shapes, contact points, constraint axes) is invisible.

**What is needed**: Implement `DebugDrawer::drawLine()` to batch line segments and submit them to `GraphicsEngine` as a debug overlay. Add console command `physics_debug_draw [0|1]`.

---

## Moderate Gaps

### GAP-P07 — No Physics Material Presets Beyond Code

**File**: `Physics/PhysicsSystem.h`

**Impact**: `PhysicsMaterial` structs (friction, restitution, damping, density) exist but there is no material library or asset loading. Materials must be created in code, not loaded from data files.

**What is needed**: A `PhysicsMaterialLibrary` that loads material presets from a JSON/config file and allows editor assignment.

---

### GAP-P08 — No Continuous Collision Detection Configuration

**File**: `Physics/PhysicsSystem.h`

**Impact**: Bullet supports CCD (continuous collision detection) for fast-moving objects like bullets and projectiles, but there is no API to enable it per-body. In an FPS engine, projectiles can tunnel through thin walls without CCD.

**What is needed**: Expose `btRigidBody::setCcdMotionThreshold()` and `setCcdSweptSphereRadius()` through the `PhysicsBody` API. Enable by default for projectile-tagged bodies.

---

### GAP-P09 — No Ragdoll System

**Files**: `Physics/PhysicsSystem.h`

**Impact**: No ragdoll setup utility exists. For FPS death animations, ragdolls are expected. Currently, constraints exist but there is no automated ragdoll generation from a skeleton.

**What is needed**: A `RagdollBuilder` that takes a `Skeleton` and creates a set of rigid bodies + hinge/cone constraints matching the bone hierarchy. Provide blend-to-ragdoll and ragdoll-to-animation transitions.

---

### GAP-P10 — PhysicsSystem Is Main-Thread Only With No Async Queries

**File**: `Physics/PhysicsSystem.h`

**Impact**: All physics queries (raycast, overlap) are synchronous and main-thread only. For AI pathfinding and perception, many raycasts are needed per frame which can bottleneck.

**What is needed**: Consider batch raycast API that can be parallelized, or at minimum document the threading constraints clearly in the API.

---

## Minor Gaps

### GAP-P11 — No Destructible Physics Objects

**Impact**: No breakable constraints or fracture system exists. Destructible environments are common in FPS games.

---

### GAP-P12 — Euler Angle Conversion May Gimbal Lock

**File**: `Physics/PhysicsSystem.cpp` (lines 40-52)

**Evidence**:
```cpp
btQuaternion PhysicsSystem::ToBulletQuaternion(const XMFLOAT3& euler) const
{
    btQuaternion quat;
    quat.setEulerZYX(euler.z, euler.y, euler.x);
    return quat;
}
```

**Impact**: Converting between Euler angles and quaternions at the physics boundary can introduce gimbal lock for objects with extreme rotations. The engine uses Euler angles in Transform components while Bullet uses quaternions internally.

**What is needed**: Consider using quaternion representation in Transform to avoid conversion artifacts, or document the Euler convention and gimbal lock limitations.

---

## Summary Table

| ID | Severity | Gap | Impact |
|---|---|---|---|
| GAP-P01 | Critical | No character controller | FPS movement |
| GAP-P02 | Critical | Global pointer pattern | Architecture |
| GAP-P03 | Major | CollisionSystem stubs | Collision layers |
| GAP-P04 | Major | No trigger volumes | Gameplay zones |
| GAP-P05 | Major | No vehicle physics | Vehicle gameplay |
| GAP-P06 | Major | Debug draw disconnected | Debug visualization |
| GAP-P07 | Moderate | No material presets from data | Workflow |
| GAP-P08 | Moderate | No CCD configuration | Projectile tunneling |
| GAP-P09 | Moderate | No ragdoll system | Death animations |
| GAP-P10 | Moderate | No async queries | Performance |
| GAP-P11 | Minor | No destructibles | Environmental gameplay |
| GAP-P12 | Minor | Euler↔quaternion conversion | Gimbal lock |

---

## Recommended Priority Order

1. **GAP-P01** — Character controller (essential for FPS)
2. **GAP-P02** — Replace global pointer with EngineContext
3. **GAP-P04** — Trigger volumes (essential for FPS gameplay)
4. **GAP-P03** — CollisionSystem layers and filtering
5. **GAP-P06** — Physics debug rendering
6. **GAP-P08** — CCD for projectiles
7. **GAP-P09** — Ragdoll system
8. Everything else
