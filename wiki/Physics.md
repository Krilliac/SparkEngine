# Physics

SparkEngine integrates **Jolt Physics** for rigid body dynamics, collision detection, raycasting, constraints, cloth simulation, and soft body physics. The `PhysicsSystem` wraps Jolt with a DirectX Math-native API, transparently converting between `XMFLOAT3` / `XMMATRIX` and Jolt's native types.

**Source:** `SparkEngine/Source/Physics/`

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        PhysicsSystem                            │
│  World manager: lifecycle, simulation step, queries, callbacks  │
│                                                                 │
│  ┌──────────────┐  ┌───────────────────┐  ┌─────────────────┐  │
│  │  PhysicsBody │  │ PhysicsConstraint │  │ ClothSimulation │  │
│  │  (btRigidBody│  │ (btTypedConstraint│  │ (PBD solver)    │  │
│  │   wrapper)   │  │   wrapper)        │  │                 │  │
│  └──────────────┘  └───────────────────┘  └─────────────────┘  │
├─────────────────────────────────────────────────────────────────┤
│                    CollisionSystem                              │
│  Static utility: primitive tests, raycasts, sweep tests,       │
│  contact manifolds, vector math helpers                        │
└─────────────────────────────────────────────────────────────────┘
         │                    │                    │
         ▼                    ▼                    ▼
┌─────────────────┐  ┌──────────────┐  ┌────────────────────┐
│ btDynamicsWorld │  │ btBroadphase │  │ btConstraintSolver │
│ (Jolt core)   │  │ (DBVT)       │  │ (Sequential Imp.)  │
└─────────────────┘  └──────────────┘  └────────────────────┘
```

### Source Files

| File | Responsibility |
|------|---------------|
| `PhysicsSystem.h` | World manager: lifecycle, simulation step, queries, callbacks, console commands |
| `PhysicsTypes.h` | Lightweight type-only header (enums, structs, descriptors) -- no Jolt dependency |
| `CollisionSystem.h` | Static collision detection utilities: primitive tests, raycasts, sweep tests, manifolds |
| `ClothSimulation.h` | Position-based dynamics cloth and soft body simulation |

### Class Overview

| Class | Responsibility |
|-------|---------------|
| `PhysicsSystem` | World manager: lifecycle, simulation step, queries, callbacks |
| `PhysicsBody` | Wrapper around `btRigidBody` with DirectX Math transform API |
| `PhysicsConstraint` | Wrapper around `btTypedConstraint` (hinge, slider, fixed, etc.) |
| `CollisionSystem` | Static class with primitive collision tests, raycasts, sweep tests, and vector math |
| `ClothSimulation` | Manages all cloth and soft body instances via position-based dynamics |

---

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

---

## Body Types

```cpp
enum class PhysicsBodyType {
    Static,      // Immovable geometry (walls, floors, terrain)
    Kinematic,   // Animated by game logic (moving platforms, doors, elevators)
    Dynamic      // Fully physics-simulated (crates, projectiles, ragdolls)
};
```

**Behavioral differences:**

| Property | Static | Kinematic | Dynamic |
|----------|--------|-----------|---------|
| Responds to forces | No | No | Yes |
| Moves via code | No | Yes (SetTransform) | Via forces/impulses |
| Collides with Dynamic | Yes | Yes | Yes |
| Has mass | 0 (infinite) | 0 (infinite) | > 0 |
| Affected by gravity | No | No | Yes |
| Use case | Level geometry | Platforms, doors | Crates, projectiles |

---

## Collision Shapes

```cpp
enum class CollisionShapeType {
    Box,          // Axis-aligned box (half-extents in dimensions)
    Sphere,       // Sphere (uses radius field)
    Capsule,      // Capsule -- character controllers (uses radius + height)
    Cylinder,     // Cylinder (uses radius + height)
    Cone,         // Cone (uses radius + height)
    Mesh,         // Triangle mesh (static geometry only -- not for dynamic bodies)
    ConvexHull,   // Convex hull (dynamic convex objects -- from vertex list)
    Heightfield,  // Heightfield terrain (from height data array)
    Compound      // Multiple shapes combined into one body
};
```

### CollisionShapeDesc

```cpp
struct CollisionShapeDesc {
    CollisionShapeType type = CollisionShapeType::Box;
    XMFLOAT3 dimensions    = {1.0f, 1.0f, 1.0f};  // Half-extents for Box
    float radius           = 0.5f;                   // Radius for Sphere/Capsule/Cylinder/Cone
    float height           = 1.0f;                   // Height for Capsule/Cylinder/Cone
    std::string meshPath;                             // File path for Mesh shapes
    std::vector<XMFLOAT3> vertices;                  // Vertices for ConvexHull
    std::vector<uint32_t> indices;                    // Indices for Mesh
    XMFLOAT3 localOffset   = {0, 0, 0};             // Local-space offset from body center
    XMFLOAT3 localRotation = {0, 0, 0};             // Local-space rotation (Euler degrees)
};
```

### Shape Selection Guide

| Shape | Performance | Accuracy | When to use |
|-------|------------|----------|-------------|
| Box | Fastest | Low | Crates, walls, simple props |
| Sphere | Fastest | Low | Projectiles, loose items |
| Capsule | Fast | Medium | Character controllers, humanoids |
| Cylinder | Fast | Medium | Barrels, pillars |
| ConvexHull | Medium | High | Complex dynamic objects (< 256 vertices) |
| Mesh | Slow | Exact | Static-only level geometry |
| Heightfield | Medium | High | Large terrain surfaces |
| Compound | Varies | High | Complex shapes from simple primitives |

---

## PhysicsBodyDesc

The full body creation descriptor:

```cpp
struct PhysicsBodyDesc {
    PhysicsBodyType type     = PhysicsBodyType::Dynamic;
    XMFLOAT3 position        = {0, 0, 0};
    XMFLOAT3 rotation        = {0, 0, 0};       // Euler angles (degrees)
    XMFLOAT3 linearVelocity  = {0, 0, 0};       // Initial linear velocity
    XMFLOAT3 angularVelocity = {0, 0, 0};       // Initial angular velocity
    float mass               = 1.0f;             // Mass in kg (0 = auto from density)
    PhysicsMaterial material;                     // Surface properties
    CollisionShapeDesc shape;                     // Collision shape configuration
    bool isTrigger           = false;             // Trigger volume (no collision response)
    bool isKinematic         = false;             // Override to kinematic mode
    std::string name;                             // Debug name for console/editor
    void* userData           = nullptr;           // User-defined data pointer
};
```

---

## Physics Materials

Named material presets control friction, restitution, and damping:

```cpp
struct PhysicsMaterial {
    float friction       = 0.5f;   // Coulomb friction [0, inf)
    float restitution    = 0.1f;   // Bounciness [0, 1]
    float linearDamping  = 0.1f;   // Translational drag [0, 1]
    float angularDamping = 0.1f;   // Rotational drag [0, 1]
    float density        = 1.0f;   // kg/m^3 (for auto-mass)
    bool isStatic        = false;  // Hint flag for static geometry
    std::string name;              // Material name for registry lookup
};
```

### Material Tuning Guide

| Material | Friction | Restitution | Density (kg/m^3) | Notes |
|----------|----------|-------------|-------------------|-------|
| Ice | 0.03 | 0.05 | 917 | Very slippery, minimal bounce |
| Concrete | 0.60 | 0.00 | 2400 | Level geometry default |
| Rubber | 0.90 | 0.80 | 1100 | High grip, very bouncy |
| Wood | 0.40 | 0.20 | 600 | Moderate friction |
| Metal | 0.30 | 0.30 | 7800 | Low friction, moderate bounce |
| Glass | 0.20 | 0.10 | 2500 | Smooth surface |

### Friction/Restitution Mixing

Jolt combines materials from two contacting bodies:
- **Friction**: `sqrt(frictionA * frictionB)` (geometric mean)
- **Restitution**: `max(restitutionA, restitutionB)` (maximum)

### Registering Materials

```cpp
PhysicsMaterial mat;
mat.friction    = 0.5f;
mat.restitution = 0.3f;
mat.linearDamping  = 0.1f;
mat.angularDamping = 0.1f;

physics.RegisterMaterial("Metal", mat);

// Later, retrieve by name:
auto metalMat = physics.GetMaterial("Metal");
```

---

## Constraints

```cpp
enum class ConstraintType {
    Point2Point,  // Ball-and-socket joint (3 translational DOF removed)
    Hinge,        // Hinge joint (doors, wheels -- 1 rotational DOF)
    Slider,       // Linear slider (pistons, rails -- 1 translational DOF)
    ConeTwist,    // Cone-twist joint (ragdoll limbs -- limited rotation)
    Generic6DOF,  // 6-DOF generic constraint (fully configurable limits)
    Fixed         // Rigid connection (0 DOF -- welds two bodies)
};
```

### Constraint Use Cases

| Type | Typical Use | Example |
|------|------------|---------|
| Point2Point | Rope endpoints, chains | Hanging lanterns |
| Hinge | Rotating objects with axis | Doors, wheels, levers |
| Slider | Linear movement along axis | Pistons, elevators, drawers |
| ConeTwist | Limited angular freedom | Ragdoll shoulders, hips |
| Generic6DOF | Full configurability | Vehicle suspensions |
| Fixed | Welding bodies together | Compound wreckage pieces |

---

## Raycasting

```cpp
// Single-hit raycast (returns closest hit)
RaycastHit hit;
if (physics.Raycast(origin, direction, maxDistance, hit)) {
    // hit.point     -- world position of impact
    // hit.normal    -- surface normal at impact
    // hit.distance  -- distance from origin
    // hit.body      -- PhysicsBody* that was hit
    // hit.userData   -- user data pointer from the body
}

// Multi-hit raycast (returns all intersections)
std::vector<RaycastHit> hits;
physics.RaycastAll(origin, direction, maxDistance, hits);
```

### RaycastHit Structure

```cpp
struct RaycastHit {
    bool hasHit          = false;
    XMFLOAT3 point       = {0, 0, 0};    // World-space hit position
    XMFLOAT3 normal      = {0, 1, 0};    // Surface normal at hit
    float distance       = 0.0f;          // Distance from ray origin
    PhysicsBody* body    = nullptr;       // Body that was hit
    void* userData       = nullptr;       // User data from the body
};
```

---

## Overlap Queries

```cpp
// Find all bodies within a sphere
std::vector<PhysicsBody*> bodies;
physics.SphereOverlap(center, radius, bodies);

// Find all bodies within a box
physics.BoxOverlap(center, halfExtents, bodies);
```

---

## Collision Callbacks

### ContactInfo Structure

```cpp
struct ContactInfo {
    PhysicsBody* bodyA      = nullptr;
    PhysicsBody* bodyB      = nullptr;
    XMFLOAT3 contactPoint   = {0, 0, 0};  // World-space contact position
    XMFLOAT3 contactNormal  = {0, 1, 0};  // Normal pointing from A to B
    float penetrationDepth  = 0.0f;        // How deep objects interpenetrate
    float appliedImpulse    = 0.0f;        // Impulse applied to resolve collision
};
```

### Registering Callbacks

```cpp
// Collision enter/exit callbacks
physics.SetCollisionCallback([](PhysicsBody* a, PhysicsBody* b, const ContactInfo& info) {
    if (info.appliedImpulse > 50.0f) {
        // Strong impact -- play crash sound, spawn particles
    }
});

// Trigger enter/exit callbacks (for trigger volumes)
physics.SetTriggerCallback([](PhysicsBody* trigger, PhysicsBody* other, bool entered) {
    if (entered) {
        // Object entered trigger zone
    } else {
        // Object exited trigger zone
    }
});
```

---

## CollisionSystem (Static Utility Class)

The `CollisionSystem` class provides CPU-side collision detection independent of Jolt Physics. All methods are `static` and thread-safe.

### Primitive Shapes

```cpp
struct BoundingBox {
    XMFLOAT3 Min;   // Minimum corner
    XMFLOAT3 Max;   // Maximum corner
    XMFLOAT3 GetCenter() const;
    XMFLOAT3 GetExtents() const;
    void Transform(const XMMATRIX& transform);
};

struct BoundingSphere {
    XMFLOAT3 Center;
    float Radius;
    void Transform(const XMMATRIX& transform);
};

struct Ray {
    XMFLOAT3 Origin;
    XMFLOAT3 Direction;   // Should be normalized
    XMFLOAT3 GetPoint(float t) const;
};
```

### ContactManifold

```cpp
struct ContactManifold {
    XMFLOAT3 ContactPoints[4];   // Up to 4 contact points
    XMFLOAT3 Normal;             // Surface normal at collision
    float PenetrationDepth;      // Interpenetration depth
    int ContactCount;            // Number of valid contact points (0-4)
};
```

### Collision Tests

| Method | Description |
|--------|-------------|
| `SphereVsSphere(a, b)` | Boolean sphere-sphere overlap |
| `SphereVsSphere(a, b, manifold)` | Sphere-sphere with contact manifold |
| `SphereVsBox(sphere, box)` | Boolean sphere-box overlap |
| `SphereVsBox(sphere, box, manifold)` | Sphere-box with contact manifold |
| `BoxVsBox(a, b)` | Boolean AABB-AABB overlap |
| `BoxVsBox(a, b, manifold)` | AABB-AABB with contact manifold (SAT) |

### Raycast Tests

| Method | Description |
|--------|-------------|
| `RayVsSphere(ray, sphere)` | Ray-sphere intersection |
| `RayVsBox(ray, box)` | Ray-AABB intersection |
| `RayVsPlane(ray, point, normal)` | Ray-plane intersection |
| `RayVsTriangle(ray, v0, v1, v2)` | Ray-triangle intersection |

### Sweep (Shape-Cast) Tests

| Method | Description |
|--------|-------------|
| `SweepSphereVsSphere(moving, velocity, target, hitTime)` | Moving sphere vs stationary sphere |
| `SweepSphereVsBox(moving, velocity, target, hitTime)` | Moving sphere vs stationary AABB |
| `SweepBoxVsBox(moving, velocity, target, hitTime)` | Moving AABB vs stationary AABB (Minkowski sum) |

All sweep tests return a `CollisionResult` and write the parametric hit time `[0, 1]` to `hitTime`.

### Utility Functions

| Method | Description |
|--------|-------------|
| `ClosestPointOnBox(point, box)` | Closest point on AABB surface |
| `ClosestPointOnSphere(point, sphere)` | Closest point on sphere surface |
| `DistancePointToPlane(point, planePoint, normal)` | Signed distance to plane |
| `PointInSphere(point, sphere)` | Point containment test |
| `PointInBox(point, box)` | Point containment test |
| `Vector3Length / LengthSquared / Normalize` | Vector math utilities |
| `Vector3Dot / Cross / Reflect / Lerp` | Vector operations |

---

## Cloth Simulation

The `ClothSimulation` class (namespace `Spark::Physics`) provides position-based dynamics (PBD) for deformable surfaces.

### Features

- Distance constraints (structural, shear, bending)
- Pin constraints (attach cloth to static/kinematic points)
- Wind and gravity forces
- Collision with spheres, capsules, and planes
- Configurable solver iteration count for quality/performance trade-off
- Self-collision (optional, expensive)

### ClothDescriptor

```cpp
struct ClothDescriptor {
    int width            = 10;      // Particles in X direction
    int height           = 10;      // Particles in Y direction
    float spacing        = 0.1f;    // Distance between adjacent particles
    float mass           = 1.0f;    // Total cloth mass
    float stiffness      = 0.9f;    // Constraint stiffness [0, 1]
    float bendStiffness  = 0.5f;    // Bending stiffness [0, 1]
    float damping        = 0.01f;   // Velocity damping
    XMFLOAT3 origin      = {0,0,0}; // World-space origin
    int solverIterations = 4;       // Constraint solver iterations per frame
};
```

### ClothCollider Types

```cpp
struct ClothCollider {
    enum class Type { Sphere, Capsule, Plane };
    Type type          = Type::Sphere;
    XMFLOAT3 position  = {0, 0, 0};
    XMFLOAT3 normal    = {0, 1, 0};   // Normal for Plane, axis for Capsule
    float radius       = 0.5f;
    float height       = 1.0f;         // Height for Capsule
};
```

### Usage Example

```cpp
ClothSimulation cloth;
ClothDescriptor desc;
desc.width = 20; desc.height = 20;
desc.spacing = 0.05f;
desc.mass = 0.5f;
desc.stiffness = 0.9f;
desc.damping = 0.01f;

auto id = cloth.CreateCloth(desc);
cloth.PinParticle(id, 0, {0, 2, 0});       // Pin top-left corner
cloth.PinParticle(id, 19, {0.95f, 2, 0});  // Pin top-right corner
cloth.SetWind(id, {1.0f, 0.0f, 0.5f});

// Add a sphere collider for the cloth to drape over
ClothCollider sphere;
sphere.type = ClothCollider::Type::Sphere;
sphere.position = {0.5f, 1.0f, 0.0f};
sphere.radius = 0.3f;
cloth.AddCollider(id, sphere);

// Per frame:
cloth.Update(deltaTime);
const auto& particles = cloth.GetParticles(id);
// Use particle positions for mesh rendering
```

### ClothSimulation API

| Method | Description |
|--------|-------------|
| `CreateCloth(desc)` | Create a new cloth instance, returns handle ID |
| `DestroyCloth(id)` | Destroy a cloth instance |
| `PinParticle(id, index, pos)` | Pin a particle to a fixed world position |
| `UnpinParticle(id, index)` | Release a pinned particle |
| `SetWind(id, wind)` | Set wind force direction and strength |
| `AddCollider(id, collider)` | Add a collision primitive for cloth interaction |
| `Update(deltaTime)` | Advance all cloth simulations |
| `GetParticles(id)` | Get particle positions for rendering |
| `GetClothDimensions(id, w, h)` | Get grid dimensions |
| `GetInstanceCount()` | Number of active cloth instances |
| `Console_GetStatus()` | Status string for console integration |

---

## ECS Integration

Use `RigidBodyComponent` and `ColliderComponent` on entities:

```cpp
auto& rb = world.AddComponent<RigidBodyComponent>(entity);
rb.mass        = 10.0f;
rb.type        = RigidBodyComponent::Type::Dynamic;
rb.friction    = 0.5f;
rb.restitution = 0.3f;

auto& col = world.AddComponent<ColliderComponent>(entity);
col.shapeType  = CollisionShapeType::Box;
col.dimensions = {1.0f, 1.0f, 1.0f};
col.isTrigger  = false;
```

The `PhysicsUpdateSystem` runs in the ECS execution pipeline and:
1. Reads `RigidBodyComponent` + `ColliderComponent` to create/update Jolt bodies
2. Steps the physics simulation via `PhysicsSystem::Update()`
3. Writes updated transforms back to the [ECS](Entity-Component-System) `TransformComponent`

### ECS Execution Order

```
Physics --> Animation --> AI --> Audio --> Lifecycle --> Render
  ^
  |
  PhysicsUpdateSystem runs here (first in the pipeline)
```

---

## Internal Implementation

### Jolt Physics Integration

PhysicsSystem manages these Jolt objects internally:

| Jolt Object | Purpose |
|---------------|---------|
| `btDiscreteDynamicsWorld` | The simulation world |
| `btDefaultCollisionConfiguration` | Collision algorithm configuration |
| `btCollisionDispatcher` | Dispatches narrow-phase collision tests |
| `btDbvtBroadphase` | Dynamic AABB tree for broad-phase culling |
| `btSequentialImpulseConstraintSolver` | Sequential impulse constraint solver |

### Type Conversion

All public APIs use DirectX Math types. Internally, conversions happen at the boundary:

```
XMFLOAT3  <-->  btVector3     (position, velocity, forces)
XMMATRIX  <-->  btTransform   (body transforms)
XMFLOAT4  <-->  btQuaternion  (rotations)
```

### Simulation Step

`PhysicsSystem::Update(deltaTime)` calls `btDiscreteDynamicsWorld::stepSimulation()` with:
- Fixed time step: 1/60 second (16.67 ms)
- Max sub-steps: 10 (prevents spiral of death at low framerates)
- Internal interpolation between fixed steps for smooth rendering

---

## Debug Drawing

Enable the physics debug draw overlay to visualize collision shapes, constraints, and contact points:

```
physics_debug on       # Enable debug draw
physics_debug off      # Disable debug draw
```

The debug drawer renders:
- Wireframe collision shapes (color-coded by body type)
- Constraint frames and limits
- Contact points and normals
- AABB broadphase volumes (optional)

---

## Console Commands

```
physics_info                # Show physics world statistics (body count, constraint count, etc.)
physics_gravity <x y z>     # Set gravity vector (default: 0 -9.81 0)
physics_debug <on|off>      # Toggle debug visualization overlay
physics_pause               # Pause physics simulation (bodies freeze)
physics_step                # Single-step physics by one fixed timestep
physics_material <name>     # Show properties of a named material
```

---

## Error Handling

- `PhysicsSystem::Initialize()` returns `HRESULT`. Check with `FAILED()` macro.
- `CreateBody()` returns `nullptr` if the body descriptor is invalid (e.g., mesh shape with empty vertex list).
- `Raycast()` returns `false` and leaves `RaycastHit` unmodified when no hit occurs.
- Constraint creation validates that both body pointers are non-null; returns `nullptr` otherwise.
- Cloth operations with invalid IDs are no-ops (return empty particle lists).

---

## Performance Considerations

- **Broad-phase**: DBVT (dynamic bounding volume tree) provides O(n log n) culling.
- **Mesh shapes**: Use only for static geometry. For dynamic objects, prefer ConvexHull.
- **Compound shapes**: More efficient than mesh for dynamic objects with complex geometry.
- **Solver iterations**: Default is 10. Increase for more accurate stacking; decrease for better performance.
- **Cloth iterations**: `solverIterations` in `ClothDescriptor` controls stiffness vs. performance (4 = fast, 16 = stiff).
- **Body sleeping**: Jolt automatically deactivates bodies at rest. Reactivation is automatic on collision or force application.
- **Raycast performance**: Single raycast is O(log n) via broadphase. Batch raycasts when possible.

---

## Thread Safety

PhysicsSystem is **not thread-safe**. Call all public methods from the main game thread. The physics simulation runs on the calling thread inside `Update()`.

CollisionSystem methods are all `static` and **thread-safe** (no shared mutable state). They can be called from any thread for CPU-side collision queries.

ClothSimulation should only be called from the main thread, similar to PhysicsSystem.

---

## Troubleshooting

### Bodies fall through the floor
- Ensure floor body type is `Static` (not `Dynamic` with mass 0)
- Check that collision shapes overlap correctly (use `physics_debug on`)
- Increase solver iterations if stacking objects tunnel through each other

### Bodies jitter or vibrate
- Reduce `restitution` values (high restitution causes energy gain)
- Increase `linearDamping` and `angularDamping` slightly
- Ensure mass ratios between contacting bodies are not extreme (< 100:1)

### Raycast misses visible geometry
- Verify the object has a collision shape attached (visual mesh is not physics mesh)
- Check that the ray direction is normalized
- Ensure `maxDistance` is large enough to reach the target

### Cloth stretches excessively
- Increase `stiffness` toward 1.0
- Increase `solverIterations` (8-16 for stiff cloth)
- Reduce `mass` relative to `spacing`

---

## Solver Tuning

Jolt Physics uses an iterative constraint solver. Tuning solver parameters affects the trade-off between simulation accuracy, stability, and performance.

### Solver Iteration Counts

The constraint solver runs multiple iterations per physics step. More iterations produce more accurate results but cost more CPU time.

| Setting | Default | Effect |
|---------|---------|--------|
| Velocity iterations | 10 | Accuracy of velocity-level constraints (contacts, joints) |
| Position iterations | 2 | Accuracy of position correction (penetration resolution) |

**Recommended settings by scene complexity:**

| Scene Type | Velocity Iters | Position Iters | Notes |
|------------|---------------|----------------|-------|
| Simple (few objects) | 6 | 1 | Fastest, acceptable for most games |
| Standard (< 500 bodies) | 10 | 2 | Good balance for typical FPS/RPG |
| Complex stacking | 15-20 | 4 | Required for tall stacks of objects |
| Vehicle physics | 12-15 | 3 | Wheel contacts need extra precision |
| Ragdoll chains | 15 | 3 | Long constraint chains need more iterations |

### Common Instability Causes

1. **Too-thin colliders** — Colliders thinner than the physics step distance cause tunneling. Minimum recommended thickness: 5 cm for dynamic bodies at default timestep.

2. **Extreme mass ratios** — A 0.1 kg box on a 10,000 kg platform creates a 100,000:1 ratio. Keep mass ratios below 100:1 for stable contact resolution. Use kinematic bodies for immovable objects instead of very heavy dynamic bodies.

3. **Long constraint chains** — A chain of 10+ hinged bodies requires more solver iterations to propagate forces. Increase velocity iterations to 15-20, or break long chains into sub-chains.

4. **High restitution** — Values above 0.8 can cause energy gain in stacked scenarios. Use restitution < 0.5 for gameplay objects.

5. **Missing damping** — Zero damping allows objects to oscillate indefinitely. Use `linearDamping = 0.05-0.1` and `angularDamping = 0.05-0.1` as baseline.

### Debug Visualization

Use the physics debug overlay to diagnose instability:

```
physics_debug on           # Enable wireframe visualization
physics_info               # Show body count, active count, constraints
physics_pause              # Freeze simulation to inspect state
physics_step               # Advance one frame at a time
```

Color coding in debug draw:
- **Green** — Active dynamic bodies
- **Gray** — Sleeping bodies
- **Blue** — Kinematic bodies
- **Red** — Contact points (with normal arrows)
- **Yellow** — Constraint frames

### Performance vs Accuracy

| Tunable | Performance Impact | Accuracy Impact |
|---------|-------------------|----------------|
| Velocity iterations | Linear | Diminishing returns above 15 |
| Position iterations | Linear | Diminishing returns above 4 |
| Fixed timestep (smaller) | Quadratic (more sub-steps) | Better tunneling prevention |
| Max sub-steps | Linear (caps spiral-of-death) | Lower = more time debt |
| Body sleeping threshold | Saves CPU for resting objects | May cause pop-in artifacts |

### Timestep Configuration

```cpp
// In PhysicsSystem — configurable via console
float m_timeStep = 1.0f / 60.0f;  // Fixed timestep (default: 60 Hz)
int m_maxSubSteps = 10;            // Max sub-steps per frame

// For high-precision scenarios (e.g., small fast projectiles):
physics.SetTimeStep(1.0f / 120.0f);  // 120 Hz physics
physics.SetMaxSubSteps(20);
```

---

## See Also

- [Entity Component System](Entity-Component-System) -- RigidBody and Collider components
- [Rendering and Graphics](Rendering-and-Graphics) -- Debug draw overlay
- [Gameplay Systems](Gameplay-Systems) -- Player controller, weapons, and vehicles
- [Terrain and Procedural Generation](Terrain-and-Procedural-Generation) -- Heightfield collision shapes
- [Animation](Animation) -- Physics-driven animation and ragdolls
- [Day Night Cycle and Weather](Day-Night-Cycle-and-Weather) -- Environmental physics interactions
