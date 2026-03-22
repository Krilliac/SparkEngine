# Jolt Physics Integration

**Last updated:** 2026-03-22
**Type:** Observation
**Status:** Active

## Description

SparkEngine migrated from Bullet Physics to Jolt Physics across 7 commits (~6,500 lines).
This entry records the integration status and remaining work.

## What's Implemented

### Core (PhysicsSystem.h/cpp, PhysicsSystemQueries.cpp, PhysicsShapeFactory.cpp)
- Full Jolt PhysicsSystem lifecycle (Init/Update/Shutdown)
- Multi-threaded job system (JobSystemThreadPool)
- Broadphase layers (NON_MOVING, MOVING, TRIGGER)
- Contact listener with surface velocity support
- Body activation listener
- Shape cache (hash-based deduplication)
- Fixed timestep accumulator with interpolation

### Shapes (15 types in CollisionShapeType enum)
Box, Sphere, Capsule, Cylinder, Cone (convex hull), Mesh, ConvexHull, Heightfield,
Compound (static), TaperedCapsule, TaperedCylinder, Plane, Scaled, OffsetCenterOfMass,
MutableCompound, Empty

### Constraints (12 types + motors)
Point2Point, Hinge, Slider, ConeTwist (SwingTwist), 6DOF, Fixed, Distance, Cone,
Gear, RackAndPinion, Pulley, Path (Hermite spline)
Motors: Hinge velocity/position, Slider velocity/position, Disable

### Subsystems
- **CharacterController**: Jolt CharacterVirtual (slopes, stairs, ground state, moving platforms)
- **VehiclePhysics**: Wheeled, tracked, motorcycle with engine/transmission/differential
- **Ragdoll**: 3 modes (Physics, Kinematic, Blended), per-part constraints
- **SoftBody**: XPBD cloth/deformable, skinned constraints (cloth-to-skeleton), wind
- **GroupFilterTable**: Sub-group collision pairs
- **MutableCompoundShape**: Runtime add/remove sub-shapes
- **Buoyancy**: ApplyBuoyancyImpulse with water plane
- **State serialization**: Binary save/load of body positions/velocities
- **Surface velocity**: Contact listener integration for conveyor belts
- **Deterministic mode**: PhysicsSettings::mDeterministicSimulation toggle
- **Debug renderer**: Data collector (PhysicsDebugRenderer.h/cpp)

### Build
- `SPARK_DOUBLE_PRECISION_PHYSICS` CMake option (OFF by default)
- PhysicsSystemStub.cpp for builds without Jolt
- All string conversions cover all enum values

## What's Remaining (Future Work)

| Feature | Priority | Notes |
|---------|----------|-------|
| Full JPH::DebugRenderer subclass | Medium | Current data collector works; full impl needs graphics engine wiring |
| Per-triangle PhysicsMaterial | Low | Jolt supports it on MeshShape but we don't expose it |
| Hair simulation (GPU strands) | Low | 3 Jolt files, very specialized |
| Jolt native StateRecorder | Low | Our binary format covers the common case |
| Constraint breaking | Low | SetBreakingThreshold stubbed; Jolt has no direct API, needs force monitoring |

## Key Files

| File | Purpose | Lines |
|------|---------|-------|
| PhysicsSystem.h | API surface, forward declarations | ~550 |
| PhysicsSystem.cpp | Lifecycle, contact listener, layers | ~670 |
| PhysicsSystemQueries.cpp | Body/constraint creation, queries, serialization | ~1550 |
| PhysicsShapeFactory.cpp | Shape creation from descriptors | ~340 |
| PhysicsBodyImpl.cpp | PhysicsBody/PhysicsConstraint methods | ~510 |
| PhysicsTypes.h | Enums, descriptors, utility types | ~730 |
| PhysicsBody.h | Body/constraint wrapper classes | ~200 |
| CharacterController.h/cpp | CharacterVirtual wrapper | ~150/~250 |
| VehiclePhysics.h/cpp | Vehicle constraint wrapper | ~70/~200 |
| RagdollSystem.h/cpp | Ragdoll wrapper | ~80/~200 |
| SoftBodyPhysics.h/cpp | Soft body wrapper | ~180/~300 |
| PhysicsDebugRenderer.h/cpp | Debug draw data collector | ~90/~30 |
| PhysicsConsoleOps.cpp | Console commands, string conversions | ~330 |
| PhysicsSystemStub.cpp | No-Jolt fallback | ~680 |
| ClothSimulation.h/cpp | Standalone cloth (separate from Jolt soft body) | ~120/~240 |

## ECS Integration

- `PhysicsComponents.h` has RigidBodyComponent and ColliderComponent
- `ECSystems.cpp` has PhysicsUpdateSystem that syncs Transform ↔ physics
- PhysicsHandle is an opaque handle linking ECS entities to PhysicsBody instances
- Editor panels: Physics2DPanel exists; Physics3DPanel added (2026-03-22)

## Notes

- PhysicsSystemStub.cpp is intentionally stub-only (for builds without Jolt)
- All constraint stubs return shared_ptr with nullptr Jolt backend — safe to call
- The `g_physicsSystem` global in PhysicsSystemQueries.cpp is used by PhysicsBody
  methods that need to call back into the system (e.g., SetPosition routes through BodyInterface)
- Contact listener runs on Jolt's internal threads — uses mutex for pending contacts
- Surface velocity map also mutex-protected for contact listener thread safety
