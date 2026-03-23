# 04 — Physics System

**Location:** `SparkEngine/Source/Physics/`

Built on **Jolt Physics**, the physics subsystem provides rigid body simulation, constraints, raycasting, character controllers, vehicles, ragdolls, soft bodies, and cloth simulation. All APIs use DirectXMath types.

---

## PhysicsSystem — Central Manager

**File:** `SparkEngine/Source/Physics/PhysicsSystem.h`

Owns the Jolt physics world, manages body/constraint lifecycle, and provides spatial queries.

### Lifecycle

```cpp
PhysicsSystem physics;
physics.Initialize();           // Create Jolt world, allocators, job system
physics.Update(fixedDeltaTime); // Advance simulation (fixed timestep)
physics.Shutdown();             // Release all resources
```

### Body Creation

```cpp
PhysicsBodyDesc desc;
desc.type = PhysicsBodyType::Dynamic;
desc.position = {0.0f, 10.0f, 0.0f};
desc.shape.type = CollisionShapeType::Sphere;
desc.shape.radius = 0.5f;
desc.mass = 10.0f;
desc.material.friction = 0.5f;
desc.material.restitution = 0.3f;
desc.collisionGroup = 1;
desc.collisionMask = 0xFFFF;
desc.motionQuality = MotionQuality::Discrete;

auto body = physics.CreateBody(desc);
physics.RemoveBody(body);
```

### PhysicsBodyDesc — Full Descriptor

```cpp
struct PhysicsBodyDesc {
    PhysicsBodyType type = PhysicsBodyType::Dynamic;   // Static, Kinematic, Dynamic
    XMFLOAT3 position = {0, 0, 0};
    XMFLOAT3 rotation = {0, 0, 0};      // Euler degrees
    XMFLOAT3 linearVelocity = {0, 0, 0};
    XMFLOAT3 angularVelocity = {0, 0, 0};
    float mass = 1.0f;
    PhysicsMaterial material;
    CollisionShapeDesc shape;
    bool isTrigger = false;
    bool isKinematic = false;
    uint16_t collisionGroup = 1;
    uint16_t collisionMask = 0xFFFF;
    std::string name;
    void* userData = nullptr;
    uint32_t entityId = 0;
    MotionQuality motionQuality = MotionQuality::Discrete;
    AllowedDOFs allowedDOFs = AllowedDOFs::All;
    float gravityFactor = 1.0f;
    float maxLinearVelocity = 500.0f;
    float maxAngularVelocity = 47.12389f;
};
```

### Collision Shapes

`Box`, `Sphere`, `Capsule`, `Cylinder`, `Cone`, `Mesh`, `ConvexHull`, `Heightfield`, `Compound`, `TaperedCapsule`, `TaperedCylinder`, `Plane`, `Scaled`, `OffsetCenterOfMass`, `MutableCompound`, `Empty`

### PhysicsBody — Per-Body Wrapper

```cpp
// Transform
body->SetPosition({5, 0, 0});
body->SetTransform(worldMatrix);
XMFLOAT3 pos = body->GetPosition();
XMMATRIX transform = body->GetTransform();

// Velocity
body->SetLinearVelocity({10, 0, 0});
body->SetAngularVelocity({0, 3.14f, 0});

// Forces
body->ApplyForce({0, -100, 0});           // Continuous force
body->ApplyImpulse({0, 500, 0});          // Instantaneous
body->ApplyTorque({0, 50, 0});
body->ApplyTorqueImpulse({0, 10, 0});

// Properties
body->SetMass(20.0f);
body->SetActive(true);
body->SetKinematic(false);

// Entity linkage
body->SetEntityID(entityId);
uint32_t eid = body->GetEntityID();

// Interpolation (for rendering between physics steps)
body->StoreCurrentState();
XMFLOAT3 interpolated = body->GetInterpolatedPosition();
```

---

## Spatial Queries

### Raycasting

```cpp
// Single hit (nearest)
RaycastHit hit = physics.Raycast(origin, direction, 100.0f);
if (hit.hasHit) {
    XMFLOAT3 point = hit.point;
    XMFLOAT3 normal = hit.normal;
    float distance = hit.distance;
    PhysicsBody* body = hit.body;
    uint32_t entityId = hit.entityId;
}

// All hits
std::vector<RaycastHit> hits = physics.RaycastAll(origin, direction, 100.0f);
```

### Overlap Tests

```cpp
std::vector<PhysicsBody*> results;
physics.SphereOverlap({0, 5, 0}, 10.0f, results);
physics.BoxOverlap({0, 5, 0}, {5, 5, 5}, results);
```

### Shape Casts

```cpp
RaycastHit hit = physics.SphereCast(origin, radius, direction, maxDistance);
RaycastHit hit = physics.BoxCast(origin, halfExtents, direction, maxDistance);
RaycastHit hit = physics.CapsuleCast(origin, radius, height, direction, maxDistance);
```

---

## Constraints

10 constraint types connecting pairs of bodies:

```cpp
auto hinge = physics.CreateHingeConstraint(bodyA, bodyB, pivotA, pivotB, axisA, axisB);
auto slider = physics.CreateSliderConstraint(bodyA, bodyB, axisA, axisB);
auto fixed = physics.CreateFixedConstraint(bodyA, bodyB);
auto distance = physics.CreateDistanceConstraint(bodyA, bodyB, minDist, maxDist);
auto cone = physics.CreateConeConstraint(bodyA, bodyB, pivotA, pivotB, twistAxis, maxAngle);
auto sixdof = physics.CreateSixDOFConstraint(bodyA, bodyB, desc);
auto pulley = physics.CreatePulleyConstraint(bodyA, bodyB, anchorA, anchorB, ratio);
auto gear = physics.CreateGearConstraint(bodyA, bodyB, axisA, axisB, ratio);
auto rackPinion = physics.CreateRackAndPinionConstraint(bodyA, bodyB, desc);
auto path = physics.CreatePathConstraint(bodyA, pathPoints);

// Motor control
physics.SetHingeMotorVelocity(hinge, targetAngularVel, maxTorque);
```

---

## Collision Callbacks

```cpp
physics.SetCollisionCallback([](const ContactInfo& info) {
    PhysicsBody* a = info.bodyA;
    PhysicsBody* b = info.bodyB;
    XMFLOAT3 point = info.contactPoint;
    XMFLOAT3 normal = info.contactNormal;
    float depth = info.penetrationDepth;
    float impulse = info.appliedImpulse;
});

physics.SetTriggerCallback([](PhysicsBody* a, PhysicsBody* b, bool entered) {
    if (entered) { /* trigger enter */ }
    else         { /* trigger exit */ }
});
```

---

## CharacterController

**File:** `SparkEngine/Source/Physics/CharacterController.h`

Jolt `CharacterVirtual` wrapper for player/NPC movement:

```cpp
auto controller = physics.CreateCharacterController(desc);
controller->SetPosition(spawnPoint);
controller->Update(deltaTime);

bool grounded = controller->IsGrounded();
controller->SetLinearVelocity(moveDir * speed);
controller->Jump(jumpForce);
```

Features: ground detection, slope limiting, stair stepping, moving platform support.

---

## VehiclePhysics

**File:** `SparkEngine/Source/Physics/VehiclePhysics.h`

Jolt vehicle constraint wrapper:

```cpp
auto vehicle = physics.CreateVehicle(chassisBody, vehicleDesc);
vehicle->SetThrottle(1.0f);
vehicle->SetBrake(0.5f);
vehicle->SetSteering(-0.3f);

// Types: Wheeled, Tracked, Motorcycle
// Engine: torque curve, max RPM, idle RPM
// Transmission: gear ratios, shift timing, differentials
// Suspension: spring rate, damping, travel distance
```

---

## RagdollSystem

**File:** `SparkEngine/Source/Physics/RagdollSystem.h`

Physics-driven skeleton with three modes:

| Mode | Behavior |
|------|----------|
| Animated | Kinematic bodies follow animation poses |
| Blended | Motors drive bodies toward animated poses |
| Physics | Fully dynamic ragdoll |

```cpp
auto ragdoll = physics.CreateRagdoll(ragdollDesc);
ragdoll->SetMode(RagdollMode::Physics);   // On death
ragdoll->SetMode(RagdollMode::Blended);   // Getting up
ragdoll->ApplyImpulse(boneIndex, impulse);
```

---

## SoftBodyPhysics

**File:** `SparkEngine/Source/Physics/SoftBodyPhysics.h`

XPBD-based soft body simulation:

```cpp
auto softBody = physics.CreateSoftBody(desc);
// desc: vertices, edges, edge constraints, skeletal skinning (up to 4 bones/vertex)
// Features: wind forces, pin constraints, collision with rigid bodies
```

---

## ClothSimulation

**File:** `SparkEngine/Source/Physics/ClothSimulation.h`

Position-based dynamics cloth simulator:

```cpp
ClothSimulation cloth;
cloth.Initialize(gridWidth, gridHeight, particleSpacing);
cloth.PinVertex(0);                    // Pin top-left corner
cloth.PinVertex(gridWidth - 1);        // Pin top-right corner
cloth.SetWind({5.0f, 0.0f, 2.0f});
cloth.Update(deltaTime);

// Collision with geometry
cloth.AddSphereCollider(center, radius);
cloth.AddCapsuleCollider(start, end, radius);
cloth.AddPlaneCollider(normal, distance);
```

---

## State Serialization

```cpp
std::vector<uint8_t> buffer;
physics.SaveState(buffer);   // Serialize entire physics world
physics.LoadState(buffer);   // Restore from snapshot
```

---

## Console Integration

```cpp
PhysicsMetrics metrics = physics.Console_GetMetrics();
// metrics: bodyCount, activeBodyCount, constraintCount, stepTime, broadphaseTime
std::string bodies = physics.Console_ListBodies();
```

---

## Thread Safety

- **Main thread only**: All physics operations, raycasting, and shape operations
- Jolt Physics internally uses multithreaded job dispatch for the simulation step
- Physics bodies store `entityId` for ECS linkage
