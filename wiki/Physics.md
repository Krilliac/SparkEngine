# Physics

SparkEngine integrates **Bullet Physics 3** for rigid body dynamics, collision detection, raycasting, and constraints. The `PhysicsSystem` wraps Bullet with a DirectX Math-native API.

**Source:** `SparkEngine/Source/Physics/PhysicsSystem.h`

## Overview

| Class | Responsibility |
|-------|---------------|
| `PhysicsSystem` | World manager: lifecycle, simulation step, queries, callbacks |
| `PhysicsBody` | Wrapper around `btRigidBody` with DirectX Math transform API |
| `PhysicsConstraint` | Wrapper around `btTypedConstraint` |

All `XMFLOAT3` / `XMMATRIX` types are transparently converted to and from Bullet's `btVector3` / `btTransform`.

## Quick Start

```cpp
PhysicsSystem physics;
if (FAILED(physics.Initialize())) return;

// Create a dynamic box
PhysicsBodyDesc desc;
desc.type             = PhysicsBodyType::Dynamic;
desc.position         = {0, 5, 0};
desc.mass             = 10.0f;
desc.shape.type       = CollisionShapeType::Box;
desc.shape.dimensions = {1, 1, 1};
auto box = physics.CreateBody(desc);

// Game loop
while (running) {
    physics.Update(deltaTime);
    renderTransform = box->GetTransform();
}
```

## Body Types

```cpp
enum class PhysicsBodyType {
    Static,      // Immovable (walls, floors)
    Kinematic,   // Animated by game logic (platforms, doors)
    Dynamic      // Fully physics-simulated (crates, projectiles)
};
```

## Collision Shapes

```cpp
enum class CollisionShapeType {
    Box,          // Axis-aligned box
    Sphere,       // Sphere
    Capsule,      // Capsule (character controllers)
    Cylinder,     // Cylinder
    Cone,         // Cone
    Mesh,         // Triangle mesh (static geometry only)
    ConvexHull,   // Convex hull (dynamic convex objects)
    Heightfield,  // Heightfield terrain
    Compound      // Multiple shapes combined
};
```

## Constraints

```cpp
enum class ConstraintType {
    Point2Point,  // Ball-and-socket joint
    Hinge,        // Hinge (doors, wheels)
    Slider,       // Linear slider
    ConeTwist,    // Cone-twist (ragdoll limbs)
    Fixed,        // Rigid connection
    Generic       // 6-DOF generic constraint
};
```

## Raycasting

```cpp
// Single-hit raycast
RaycastHit hit;
if (physics.Raycast(origin, direction, maxDistance, hit)) {
    // hit.point, hit.normal, hit.distance, hit.body
}

// Multi-hit raycast
std::vector<RaycastHit> hits;
physics.RaycastAll(origin, direction, maxDistance, hits);
```

## Overlap Queries

```cpp
// Find all bodies within a sphere
std::vector<PhysicsBody*> bodies;
physics.SphereOverlap(center, radius, bodies);

// Find all bodies within a box
physics.BoxOverlap(center, halfExtents, bodies);
```

## Physics Materials

Named material presets with friction and restitution:

```cpp
PhysicsMaterial mat;
mat.friction    = 0.5f;
mat.restitution = 0.3f;
mat.linearDamping  = 0.1f;
mat.angularDamping = 0.1f;

physics.RegisterMaterial("Metal", mat);
```

## Collision Callbacks

```cpp
// Collision enter/exit callbacks
physics.SetCollisionCallback([](PhysicsBody* a, PhysicsBody* b, const ContactInfo& info) {
    // Handle collision
});

// Trigger enter/exit callbacks
physics.SetTriggerCallback([](PhysicsBody* trigger, PhysicsBody* other, bool entered) {
    // Handle trigger
});
```

## ECS Integration

Use `RigidBodyComponent` and `ColliderComponent` on entities:

```cpp
auto& rb = world.AddComponent<RigidBodyComponent>(entity);
rb.mass = 10.0f;
rb.type = RigidBodyComponent::Type::Dynamic;
rb.friction = 0.5f;
rb.restitution = 0.3f;

auto& col = world.AddComponent<ColliderComponent>(entity);
col.shapeType  = CollisionShapeType::Box;
col.dimensions = {1.0f, 1.0f, 1.0f};
col.isTrigger  = false;
```

The `PhysicsUpdateSystem` syncs physics body transforms with ECS `Transform` components each frame.

## Debug Drawing

Enable the Bullet debug draw overlay to visualize collision shapes, constraints, and contact points:

```
physics_debug on       # Enable debug draw
physics_debug off      # Disable debug draw
```

## Console Commands

```
physics_info           # Show physics world statistics
physics_gravity <x y z> # Set gravity vector
physics_debug <on|off> # Toggle debug visualization
physics_pause          # Pause physics simulation
physics_step           # Single-step physics
physics_material <name> # Show material properties
```

## Thread Safety

PhysicsSystem is **not thread-safe**. Call all public methods from the main game thread. The physics simulation runs on the calling thread inside `Update()`.

## See Also

- [[Entity Component System]] — RigidBody and Collider components
- [[Rendering and Graphics]] — Debug draw overlay
