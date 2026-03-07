/**
 * @file PhysicsSystem.h
 * @brief Complete physics integration system for Spark Engine.
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * PhysicsSystem wraps the **Bullet Physics 3** dynamics library, exposing a
 * DirectX Math-native API that integrates cleanly with the rest of Spark Engine.
 * All DirectX `XMFLOAT3` / `XMMATRIX` types are transparently converted to and
 * from Bullet's `btVector3` / `btTransform` internally.
 *
 * ## Architecture overview
 *
 * | Class              | Responsibility                                                    |
 * |--------------------|-------------------------------------------------------------------|
 * | PhysicsBody        | Wrapper around a `btRigidBody` with a DirectX Math transform API |
 * | PhysicsConstraint  | Wrapper around a `btTypedConstraint` (hinge, slider, fixed, …)   |
 * | PhysicsSystem      | World manager: lifecycle, simulation step, queries, callbacks     |
 *
 * ## Supported features
 *
 * - **Body types**: Static (immovable), Kinematic (animated by game logic),
 *   Dynamic (fully simulated).
 * - **Collision shapes**: Box, Sphere, Capsule, Cylinder, Cone, Mesh,
 *   ConvexHull, Heightfield, Compound.
 * - **Constraints**: Hinge, Slider, Fixed (more via `btTypedConstraint` directly).
 * - **Queries**: Raycast (single/all), SphereOverlap, BoxOverlap.
 * - **Callbacks**: Collision and trigger enter/exit events via `std::function`.
 * - **Materials**: Named PhysicsMaterial presets (friction, restitution, damping).
 * - **Debug rendering**: Optional Bullet debug-draw overlay via a custom DebugDrawer.
 * - **Console integration**: 12+ runtime commands for inspecting/modifying physics.
 *
 * ## Typical usage
 * @code
 *   PhysicsSystem physics;
 *   if (FAILED(physics.Initialize())) return;
 *
 *   // Create a dynamic box
 *   PhysicsBodyDesc desc;
 *   desc.type             = PhysicsBodyType::Dynamic;
 *   desc.position         = {0, 5, 0};
 *   desc.mass             = 10.0f;
 *   desc.shape.type       = CollisionShapeType::Box;
 *   desc.shape.dimensions = {1, 1, 1};
 *   auto box = physics.CreateBody(desc);
 *
 *   // Game loop
 *   while (running) {
 *       physics.Update(deltaTime);
 *       renderTransform = box->GetTransform();
 *   }
 * @endcode
 *
 * @note PhysicsSystem is **not thread-safe**. Call all public methods from the
 *       main game thread. The physics simulation itself runs on the calling thread
 *       inside Update().
 */

#pragma once
#include "../Core/Platform.h"

#include "Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <functional>
#include <mutex>


// Forward declarations for Bullet Physics
class btDiscreteDynamicsWorld;
class btDefaultCollisionConfiguration;
class btCollisionDispatcher;
class btDbvtBroadphase;
class btSequentialImpulseConstraintSolver;
class btRigidBody;
class btCollisionShape;
class btMotionState;
class btTypedConstraint;

/**
 * @brief Physics body types
 */
enum class PhysicsBodyType
{
    Static,        ///< Static body (immovable)
    Kinematic,     ///< Kinematic body (user-controlled movement)
    Dynamic        ///< Dynamic body (physics-controlled)
};

/**
 * @brief Collision shapes
 */
enum class CollisionShapeType
{
    Box,           ///< Box collision shape
    Sphere,        ///< Sphere collision shape
    Capsule,       ///< Capsule collision shape
    Cylinder,      ///< Cylinder collision shape
    Cone,          ///< Cone collision shape
    Mesh,          ///< Triangle mesh shape
    ConvexHull,    ///< Convex hull shape
    Heightfield,   ///< Heightfield terrain shape
    Compound       ///< Compound shape (multiple shapes)
};

/**
 * @brief Physics constraint types
 */
enum class ConstraintType
{
    Point2Point,   ///< Point-to-point constraint
    Hinge,         ///< Hinge constraint
    Slider,        ///< Slider constraint
    ConeTwist,     ///< Cone-twist constraint
    Generic6DOF,   ///< 6-DOF constraint
    Fixed          ///< Fixed constraint
};

/**
 * @brief Surface and physical properties assigned to a physics body.
 *
 * PhysicsMaterial controls how a rigid body interacts with other objects during
 * contact: how much it slides (friction), how much it bounces (restitution), and
 * how quickly it slows down (linear/angular damping).
 *
 * Materials can be registered by name with PhysicsSystem::RegisterMaterial() and
 * then looked up by name, making it easy to share presets (e.g. "Ice", "Rubber",
 * "Wood") across many bodies without duplicating values.
 *
 * ### Tuning guide
 * - **friction**: 0.0 = frictionless ice; 1.0 = high-grip rubber. Values above 1
 *   are valid for extra-grippy surfaces. Combined with the other body's friction via
 *   Bullet's default `sqrt(frA * frB)` mixing.
 * - **restitution**: 0.0 = no bounce (clay); 1.0 = perfectly elastic. Values > 1
 *   add energy (usually undesirable). Combined via `max(restA, restB)` mixing.
 * - **linearDamping / angularDamping**: [0, 1] range; simulates air resistance.
 *   Keep values small (≤ 0.3) for realistic motion; higher values feel floaty.
 * - **density**: informational only (mass is computed from shape volume × density
 *   when `PhysicsBodyDesc::mass == 0`).
 */
struct PhysicsMaterial
{
    /**
     * @brief Coulomb friction coefficient controlling sliding resistance.
     *
     * Range: [0, ∞). Typical values: ice ≈ 0.03, concrete ≈ 0.6, rubber ≈ 0.9.
     * Combined with the contact partner's friction via `sqrt(frA × frB)`.
     */
    float friction = 0.5f;

    /**
     * @brief Coefficient of restitution controlling bounciness.
     *
     * Range: [0, 1]. 0 = perfectly inelastic (no bounce); 1 = perfectly elastic.
     * Combined with the contact partner's restitution via `max(restA, restB)`.
     */
    float restitution = 0.1f;

    /**
     * @brief Linear velocity damping applied each simulation step.
     *
     * Range: [0, 1]. Models air/fluid drag on the body's translational motion.
     * 0 = no damping; 0.1 = light air resistance; 0.5 = heavy drag.
     */
    float linearDamping = 0.1f;

    /**
     * @brief Angular velocity damping applied each simulation step.
     *
     * Range: [0, 1]. Models rotational drag (air resistance to spinning).
     * Keep similar to linearDamping for natural-looking objects.
     */
    float angularDamping = 0.1f;

    /**
     * @brief Material density in kg/m³ (used for automatic mass computation).
     *
     * When `PhysicsBodyDesc::mass == 0`, the body's mass is estimated as
     * `volume × density`. Ignored if an explicit non-zero mass is provided.
     * Water ≈ 1000, steel ≈ 7800, wood ≈ 600.
     */
    float density = 1.0f;

    /**
     * @brief Hint flag indicating this material is intended for static bodies.
     *
     * Informational only; the actual body type is controlled by `PhysicsBodyDesc::type`.
     * Static materials are typically registered with `friction = 0.6` and
     * `restitution = 0.0` for level geometry.
     */
    bool isStatic = false;

    /**
     * @brief Human-readable identifier for this material preset.
     *
     * Used as the key when registering with PhysicsSystem::RegisterMaterial().
     * Leave empty for anonymous (one-off) materials.
     */
    std::string name;
};

/**
 * @brief Descriptor that fully specifies the collision geometry for a physics body.
 *
 * PhysicsSystem::CreateBody() reads this struct to construct the appropriate Bullet
 * `btCollisionShape`. Once a shape is created it may be cached internally (keyed by
 * a hash of the descriptor) so that many bodies sharing the same geometry reuse a
 * single `btCollisionShape` instance, reducing memory usage.
 *
 * ### Which fields apply to which shape types
 *
 * | `type`         | Primary fields used                        |
 * |----------------|--------------------------------------------|
 * | Box            | `dimensions`                               |
 * | Sphere         | `radius`                                   |
 * | Capsule        | `radius`, `height`                         |
 * | Cylinder       | `radius`, `height`                         |
 * | Cone           | `radius`, `height`                         |
 * | Mesh           | `meshPath` OR `vertices` + `indices`       |
 * | ConvexHull     | `vertices`                                 |
 * | Heightfield    | `dimensions.x` (width), `dimensions.z` (depth), `dimensions.y` (max height) |
 * | Compound       | (constructed externally; pass sub-shapes separately) |
 *
 * @note TriangleMesh shapes (`Mesh`) do not respond to dynamic physics forces — use
 *       them for static world geometry. For dynamic objects, prefer ConvexHull.
 */
struct CollisionShapeDesc
{
    /**
     * @brief The geometric primitive to use for collision.
     *
     * Determines which fields below are relevant. Defaults to Box.
     */
    CollisionShapeType type = CollisionShapeType::Box;

    /**
     * @brief Half-extents (width, height, depth) for Box shapes.
     *
     * Each component is a half-extent, so `{0.5, 1.0, 0.5}` creates a 1×2×1 m box.
     * Also used as width/depth/maxHeight for Heightfield shapes.
     */
    XMFLOAT3 dimensions = {1.0f, 1.0f, 1.0f};

    /**
     * @brief Radius for Sphere, Capsule, Cylinder, and Cone shapes (metres).
     *
     * For Capsule/Cylinder/Cone this is the radius of the cross-section circle.
     * Ignored by Box, Mesh, ConvexHull, and Heightfield shapes.
     */
    float radius = 0.5f;

    /**
     * @brief Total height for Capsule, Cylinder, and Cone shapes (metres).
     *
     * For Capsule, the cylindrical section height (excluding the two hemisphere caps)
     * equals `height`. The total capsule height is `height + 2 × radius`.
     * Ignored by Box, Sphere, Mesh, ConvexHull, and Heightfield shapes.
     */
    float height = 1.0f;

    /**
     * @brief File path to an OBJ/FBX mesh for TriangleMesh collision shapes.
     *
     * Used only when `type == CollisionShapeType::Mesh`. The mesh is loaded via
     * Assimp, converted to a `btBvhTriangleMeshShape`, and cached.
     * Leave empty if providing vertices/indices directly.
     */
    std::string meshPath;

    /**
     * @brief Vertex positions for ConvexHull and inline TriangleMesh shapes.
     *
     * - For **ConvexHull**: provide all hull vertices (duplicates are handled by Bullet).
     * - For **Mesh** (inline): paired with `indices` to define the triangle list.
     *   This is useful for procedurally-generated geometry or when the mesh data is
     *   already in memory.
     */
    std::vector<XMFLOAT3> vertices;

    /**
     * @brief Triangle index list for inline TriangleMesh shapes.
     *
     * Used only when `type == CollisionShapeType::Mesh` and `meshPath` is empty.
     * Indices must form a triangle list (groups of 3), each referencing a position
     * in `vertices`. Must not be empty if `vertices` is provided for a Mesh shape.
     */
    std::vector<uint32_t> indices;

    /**
     * @brief Local translation of the shape relative to the body's center of mass.
     *
     * Use this to offset the collision shape from the body origin (e.g. to match a
     * mesh whose pivot is not at its centre). Applied before `localRotation`.
     */
    XMFLOAT3 localOffset = {0, 0, 0};

    /**
     * @brief Local Euler rotation (degrees) of the shape relative to the body.
     *
     * Useful for aligning a cylinder or capsule whose axis does not match the
     * default (Y-up). Applied after `localOffset`.
     */
    XMFLOAT3 localRotation = {0, 0, 0};
};

/**
 * @brief Complete descriptor for creating a PhysicsBody.
 *
 * Pass a populated PhysicsBodyDesc to PhysicsSystem::CreateBody() to spawn a
 * rigid body into the simulation. All fields have sensible defaults, so you
 * only need to override the ones relevant to your specific body.
 *
 * ### Quick-start examples
 * @code
 *   // Static floor plane
 *   PhysicsBodyDesc floor;
 *   floor.type             = PhysicsBodyType::Static;
 *   floor.mass             = 0.0f;   // mass must be 0 for static bodies
 *   floor.shape.type       = CollisionShapeType::Box;
 *   floor.shape.dimensions = {50, 0.5f, 50};
 *   floor.name             = "Floor";
 *   auto floorBody = physics.CreateBody(floor);
 *
 *   // Dynamic grenade
 *   PhysicsBodyDesc grenade;
 *   grenade.type             = PhysicsBodyType::Dynamic;
 *   grenade.position         = playerPos;
 *   grenade.linearVelocity   = throwDirection * 12.0f;
 *   grenade.mass             = 0.4f;
 *   grenade.shape.type       = CollisionShapeType::Sphere;
 *   grenade.shape.radius     = 0.08f;
 *   grenade.material.restitution = 0.3f;  // slight bounce
 *   auto grenadeBody = physics.CreateBody(grenade);
 * @endcode
 *
 * @note If both `isTrigger` and a collision shape are set, the shape is used for
 *       overlap testing but generates no contact forces. Trigger bodies do not
 *       collide with anything; they only fire the trigger callback.
 */
struct PhysicsBodyDesc
{
    /**
     * @brief Simulation mode for this body.
     *
     * - Static: mass must be 0; never moved by the simulation; optimal for level geometry.
     * - Kinematic: moved explicitly via SetPosition()/SetTransform(); other bodies react to it.
     * - Dynamic: fully simulated; responds to forces, gravity, and contacts.
     */
    PhysicsBodyType type = PhysicsBodyType::Dynamic;

    /**
     * @brief Initial world-space position of the body's center of mass (metres).
     *
     * Applied at creation time. Ignored after the body is added to the world;
     * use PhysicsBody::SetPosition() to move it later.
     */
    XMFLOAT3 position = {0, 0, 0};

    /**
     * @brief Initial rotation expressed as Euler angles in degrees (X=pitch, Y=yaw, Z=roll).
     *
     * Converted internally to a quaternion before being passed to Bullet. Using
     * Euler angles avoids gimbal lock issues at the API boundary; prefer setting
     * this to zero and calling PhysicsBody::SetTransform() if you already have a matrix.
     */
    XMFLOAT3 rotation = {0, 0, 0};

    /**
     * @brief Initial linear velocity applied to the body at creation (metres/second).
     *
     * Non-zero values are useful for thrown objects (grenades, arrows). For Static
     * and Kinematic bodies, this field is ignored.
     */
    XMFLOAT3 linearVelocity = {0, 0, 0};

    /**
     * @brief Initial angular velocity applied to the body at creation (radians/second).
     *
     * Non-zero values give thrown objects an initial spin. For Static and Kinematic
     * bodies, this field is ignored.
     */
    XMFLOAT3 angularVelocity = {0, 0, 0};

    /**
     * @brief Mass of the body in kilograms.
     *
     * - 0.0 → the body behaves as Static regardless of the `type` field. Bullet
     *          treats zero-mass bodies as immovable.
     * - > 0  → must match `type == Dynamic` for full simulation.
     *
     * Typical values: player capsule ≈ 80 kg, grenade ≈ 0.4 kg, car ≈ 1500 kg.
     */
    float mass = 1.0f;

    /**
     * @brief Surface and physical material properties (friction, restitution, damping).
     *
     * Use PhysicsSystem::GetMaterial("name") to retrieve a pre-registered material
     * preset, or fill in a local PhysicsMaterial with custom values.
     */
    PhysicsMaterial material;

    /**
     * @brief Geometry used for collision detection and contact generation.
     *
     * Fully describes the shape type and its dimensions/data. See CollisionShapeDesc
     * for per-shape-type field details.
     */
    CollisionShapeDesc shape;

    /**
     * @brief When true, the body acts as a trigger volume rather than a solid collider.
     *
     * Trigger bodies generate OnTriggerEnter/OnTriggerExit callbacks (via
     * PhysicsSystem::SetTriggerCallback()) but apply no contact forces. Use them
     * for damage zones, checkpoint regions, and area-of-effect volumes.
     */
    bool isTrigger = false;

    /**
     * @brief Convenience flag to set `type = Kinematic` when true.
     *
     * Equivalent to setting `type = PhysicsBodyType::Kinematic`. Provided for
     * readability in code that configures bodies through UI or scripting.
     */
    bool isKinematic = false;

    /**
     * @brief Human-readable identifier for debugging and console queries.
     *
     * Named bodies can be retrieved by name via PhysicsSystem::Console_GetBodyInfo().
     * Leave empty for anonymous (un-named) bodies.
     */
    std::string name;

    /**
     * @brief Opaque pointer to caller-owned data associated with this body.
     *
     * Stored without any ownership semantics (no delete is ever called). Typically
     * used to link the PhysicsBody back to a game object or ECS entity:
     * @code
     *   desc.userData = static_cast<void*>(gameObject);
     * @endcode
     * Accessible later via PhysicsBody::GetUserData() in collision callbacks.
     */
    void* userData = nullptr;
};

/**
 * @brief Raycast hit information
 */
struct RaycastHit
{
    bool hasHit = false;                  ///< Whether ray hit something
    XMFLOAT3 point = {0, 0, 0};          ///< Hit point in world space
    XMFLOAT3 normal = {0, 1, 0};         ///< Hit surface normal
    float distance = 0.0f;               ///< Distance from ray origin
    class PhysicsBody* body = nullptr;    ///< Hit physics body
    void* userData = nullptr;             ///< User data from hit body
};

/**
 * @brief Collision contact information
 */
struct ContactInfo
{
    class PhysicsBody* bodyA = nullptr;   ///< First colliding body
    class PhysicsBody* bodyB = nullptr;   ///< Second colliding body
    XMFLOAT3 contactPoint = {0, 0, 0};   ///< Contact point
    XMFLOAT3 contactNormal = {0, 1, 0};  ///< Contact normal
    float penetrationDepth = 0.0f;       ///< Penetration depth
    float appliedImpulse = 0.0f;         ///< Applied impulse
};

/**
 * @class PhysicsBody
 * @brief DirectX Math-native wrapper around a Bullet `btRigidBody`.
 *
 * PhysicsBody is the primary handle callers use to interact with a single
 * rigid body in the simulation. It abstracts away Bullet's coordinate system
 * and type conversions, presenting all transforms and velocities as DirectX
 * `XMFLOAT3` / `XMMATRIX` values.
 *
 * ### Ownership
 * PhysicsBody instances are created and owned by PhysicsSystem via `shared_ptr`.
 * Callers receive a `shared_ptr<PhysicsBody>` from CreateBody(); keeping it alive
 * keeps the body active. Dropping the last reference triggers cleanup, but for
 * deterministic removal call `PhysicsSystem::RemoveBody()` explicitly.
 *
 * ### Simulation loop integration
 * After each PhysicsSystem::Update() call, Dynamic bodies have new positions and
 * rotations. Sync your rendered transform by calling GetTransform() once per frame:
 * @code
 *   physics.Update(dt);
 *   XMMATRIX world = body->GetTransform();
 *   renderable.SetWorldMatrix(world);
 * @endcode
 *
 * ### Kinematic bodies
 * For animated/scripted objects that should push other bodies around, set
 * `type = Kinematic`. Drive them by calling SetPosition() or SetTransform()
 * each frame before Physics::Update().
 */
class PhysicsBody
{
public:
    /**
     * @brief Construct a PhysicsBody wrapping the given Bullet rigid body.
     *
     * Called internally by PhysicsSystem::CreateBody(). Do not construct manually.
     *
     * @param desc        Descriptor used to create this body (stored for later queries).
     * @param bulletBody  Bullet rigid body pointer (owned by the Bullet dynamics world;
     *                    must outlive this PhysicsBody wrapper).
     */
    PhysicsBody(const PhysicsBodyDesc& desc, btRigidBody* bulletBody);

    /**
     * @brief Destructor. Does not remove the body from the Bullet world.
     *
     * Call PhysicsSystem::RemoveBody() before the last shared_ptr reference is
     * dropped to ensure the body is properly unregistered from the dynamics world.
     */
    ~PhysicsBody();

    // =========================================================================
    // Transform
    // =========================================================================

    /**
     * @brief Get the current world-space position of the body's center of mass.
     *
     * For Dynamic bodies this reflects the position after the most recent
     * PhysicsSystem::Update() call. For Static bodies the position never changes.
     *
     * @return  World-space position in metres.
     */
    XMFLOAT3 GetPosition() const;

    /**
     * @brief Teleport the body to a new world-space position.
     *
     * For Dynamic bodies this should be used sparingly (e.g. level teleporters)
     * as abrupt position changes can cause tunnelling and interpenetration.
     * For Kinematic bodies, call this every frame to drive the animation.
     *
     * @param position  New world-space position (metres).
     */
    void SetPosition(const XMFLOAT3& position);

    /**
     * @brief Get the current world-space rotation as Euler angles (degrees).
     *
     * The rotation is extracted from the body's internal quaternion and converted
     * to (pitch, yaw, roll) in degrees. Prefer GetTransform() if you need the full
     * 4×4 matrix to avoid gimbal-lock issues.
     *
     * @return  Euler angles in degrees (X=pitch, Y=yaw, Z=roll).
     */
    XMFLOAT3 GetRotation() const;

    /**
     * @brief Set the world-space rotation from Euler angles (degrees).
     *
     * Converts (pitch, yaw, roll) to a quaternion internally. Useful for scripted
     * or editor-driven orientation changes.
     *
     * @param rotation  Euler angles in degrees (X=pitch, Y=yaw, Z=roll).
     */
    void SetRotation(const XMFLOAT3& rotation);

    /**
     * @brief Get the body's full world-space transform as a 4×4 matrix.
     *
     * Returns a combined rotation + translation matrix suitable for use as a
     * world matrix in a DirectX rendering pipeline. The matrix is column-major
     * (consistent with DirectX Math conventions).
     *
     * @return  World-space transform matrix (rotation + translation; no scale).
     */
    XMMATRIX GetTransform() const;

    /**
     * @brief Set the body's full world-space transform from a 4×4 matrix.
     *
     * Decomposes the matrix into a translation and rotation, discarding any scale
     * component (Bullet does not support per-body scaling). For Kinematic bodies,
     * call this each frame to match the animated position/orientation.
     *
     * @param transform  World-space transform matrix. Scale is ignored.
     */
    void SetTransform(const XMMATRIX& transform);

    // =========================================================================
    // Velocity
    // =========================================================================

    /**
     * @brief Get the body's current linear (translational) velocity.
     *
     * Only meaningful for Dynamic bodies; Static and Kinematic bodies always
     * report zero or the velocity explicitly set via SetLinearVelocity().
     *
     * @return  Linear velocity in metres per second.
     */
    XMFLOAT3 GetLinearVelocity() const;

    /**
     * @brief Directly set the body's linear velocity, overriding any accumulated forces.
     *
     * Useful for snapping a body to a specific speed (e.g. setting a projectile's
     * initial velocity). For continuous thrust, prefer ApplyForce() each frame.
     *
     * @param velocity  New linear velocity in metres per second.
     */
    void SetLinearVelocity(const XMFLOAT3& velocity);

    /**
     * @brief Get the body's current angular (rotational) velocity.
     *
     * Returned in radians per second around each world axis.
     *
     * @return  Angular velocity (radians/second) around X, Y, Z world axes.
     */
    XMFLOAT3 GetAngularVelocity() const;

    /**
     * @brief Directly set the body's angular velocity.
     *
     * Overwrites any spin accumulated from torques or collisions. Useful for
     * controlling a thrown object's spin speed.
     *
     * @param velocity  New angular velocity in radians per second.
     */
    void SetAngularVelocity(const XMFLOAT3& velocity);

    // =========================================================================
    // Forces and impulses
    // =========================================================================

    /**
     * @brief Apply a continuous force to the body (accumulates over the time step).
     *
     * Forces are integrated over the physics time step: `Δvelocity = force / mass × Δt`.
     * Call this every frame for sustained effects like jet thrust or wind.
     * The accumulated force is cleared automatically after each simulation step.
     *
     * @param force        Force vector in Newtons (world space).
     * @param relativePos  Local-space offset from the center of mass where the force
     *                     is applied. Non-zero values generate torque in addition to
     *                     linear force. Defaults to {0,0,0} (center of mass).
     */
    void ApplyForce(const XMFLOAT3& force, const XMFLOAT3& relativePos = {0, 0, 0});

    /**
     * @brief Apply an instantaneous impulse to the body.
     *
     * An impulse produces an immediate velocity change: `Δvelocity = impulse / mass`.
     * Use this for single-frame effects like explosions, jump boosts, or hit reactions.
     *
     * @param impulse      Impulse vector in Newton-seconds (N·s) (world space).
     * @param relativePos  Local-space application offset. Non-zero values generate an
     *                     angular impulse in addition to the linear one.
     *                     Defaults to {0,0,0} (center of mass).
     */
    void ApplyImpulse(const XMFLOAT3& impulse, const XMFLOAT3& relativePos = {0, 0, 0});

    /**
     * @brief Apply a continuous torque to the body (accumulates over the time step).
     *
     * Causes angular acceleration: `Δω = torque / inertia × Δt`. Like ApplyForce(),
     * the accumulated torque is reset after each simulation step.
     *
     * @param torque  Torque vector in Newton-metres (world space). The direction of
     *                the vector is the axis of rotation; the magnitude is the strength.
     */
    void ApplyTorque(const XMFLOAT3& torque);

    /**
     * @brief Apply an instantaneous angular impulse to the body.
     *
     * Produces an immediate angular velocity change. Equivalent to a rotational
     * version of ApplyImpulse(). Useful for imparting spin on impact.
     *
     * @param torque  Angular impulse in Newton-metre-seconds (N·m·s) (world space).
     */
    void ApplyTorqueImpulse(const XMFLOAT3& torque);

    // =========================================================================
    // Properties
    // =========================================================================

    /**
     * @brief Get the body's mass in kilograms.
     *
     * Returns the effective mass used by the simulation. For Static bodies this
     * is always 0 regardless of what was specified in the descriptor.
     *
     * @return  Mass in kilograms (0 for Static bodies).
     */
    float GetMass() const;

    /**
     * @brief Change the body's mass at runtime.
     *
     * Causes Bullet to recompute inertia tensors. Valid for Dynamic bodies; setting
     * mass to 0 effectively converts the body to Static.
     *
     * @param mass  New mass in kilograms. Must be ≥ 0.
     */
    void SetMass(float mass);

    /**
     * @brief Get the body type (Static, Kinematic, or Dynamic).
     * @return  PhysicsBodyType enum value as set in the creation descriptor.
     */
    PhysicsBodyType GetType() const { return m_desc.type; }

    /**
     * @brief Get the current physics material (const reference).
     * @return  Const reference to the body's PhysicsMaterial.
     */
    const PhysicsMaterial& GetMaterial() const { return m_desc.material; }

    /**
     * @brief Replace the body's physics material at runtime.
     *
     * Updates friction, restitution, and damping on the underlying `btRigidBody`.
     * Takes effect on the next simulation step.
     *
     * @param material  New material properties to apply.
     */
    void SetMaterial(const PhysicsMaterial& material);

    // =========================================================================
    // State
    // =========================================================================

    /**
     * @brief Activate or deactivate the body in the simulation.
     *
     * Inactive bodies are excluded from collision detection and dynamics, making
     * deactivation useful for pooled objects. Reactivate with `SetActive(true)` before
     * reusing.
     *
     * @param active  `true` to activate; `false` to deactivate (sleep).
     */
    void SetActive(bool active);

    /**
     * @brief Check whether the body is currently active in the simulation.
     * @return  `true` if the body is participating in the physics step; `false` if sleeping.
     */
    bool IsActive() const;

    /**
     * @brief Switch the body between Dynamic and Kinematic modes at runtime.
     *
     * Setting `kinematic = true` reconfigures the body as Kinematic: it will no
     * longer respond to forces or gravity, but other Dynamic bodies will still
     * respond to contacts with it.
     *
     * @param kinematic  `true` to make the body Kinematic; `false` to make it Dynamic.
     */
    void SetKinematic(bool kinematic);

    /**
     * @brief Check whether the body is currently Kinematic.
     * @return  `true` if Kinematic; `false` if Dynamic or Static.
     */
    bool IsKinematic() const;

    /**
     * @brief Enable or disable trigger mode at runtime.
     *
     * A trigger body generates overlap callbacks (via PhysicsSystem::SetTriggerCallback())
     * but applies no contact forces. Switching from solid to trigger (or vice versa)
     * modifies the body's collision flags on the Bullet rigid body.
     *
     * @param trigger  `true` to enable trigger mode; `false` for solid collision.
     */
    void SetTrigger(bool trigger);

    /**
     * @brief Check whether trigger mode is enabled.
     * @return  `true` if this body is a trigger volume; `false` if it is a solid collider.
     */
    bool IsTrigger() const;

    // =========================================================================
    // Collision filtering
    // =========================================================================

    /**
     * @brief Set the collision group bitmask for this body.
     *
     * The body only collides with another body when:
     *   `(this->group & other->mask) != 0 && (other->group & this->mask) != 0`.
     *
     * Default group: 1 (bit 0). Group 0 is reserved (no collisions at all).
     *
     * @param group  Bitmask identifying which "team" this body belongs to.
     */
    void SetCollisionGroup(uint16_t group);

    /**
     * @brief Set the collision mask for this body.
     *
     * The mask defines which groups this body will respond to. Bits set to 1
     * mean "collide with bodies in that group".
     *
     * Default mask: 0xFFFF (collide with everything).
     *
     * @param mask  Bitmask of groups this body will collide with.
     */
    void SetCollisionMask(uint16_t mask);

    /**
     * @brief Get the current collision group bitmask.
     * @return  16-bit collision group mask.
     */
    uint16_t GetCollisionGroup() const { return m_collisionGroup; }

    /**
     * @brief Get the current collision filter mask.
     * @return  16-bit collision filter mask.
     */
    uint16_t GetCollisionMask() const { return m_collisionMask; }

    // =========================================================================
    // User data
    // =========================================================================

    /**
     * @brief Attach a caller-owned data pointer to this body.
     *
     * The pointer is stored without any ownership semantics — the body never deletes
     * it. Typically used to link the body back to a game object or ECS entity so that
     * collision callbacks can identify and manipulate the owning object.
     *
     * @param data  Opaque pointer to caller-managed data. May be `nullptr`.
     */
    void SetUserData(void* data) { m_desc.userData = data; }

    /**
     * @brief Retrieve the previously set user data pointer.
     * @return  Pointer set by SetUserData(), or `nullptr` if none was set.
     */
    void* GetUserData() const { return m_desc.userData; }

    /**
     * @brief Get the human-readable debug name of this body.
     * @return  Name string as provided in PhysicsBodyDesc::name at creation time.
     */
    const std::string& GetName() const { return m_desc.name; }

    // =========================================================================
    // Internal / low-level access
    // =========================================================================

    /**
     * @brief Get the underlying Bullet rigid body pointer.
     *
     * Use this for advanced Bullet operations not exposed by the PhysicsBody API.
     * Handle with care — modifications to the Bullet body can desynchronize the
     * PhysicsBody's cached state.
     *
     * @return  Non-owning pointer to the underlying `btRigidBody`.
     */
    btRigidBody* GetBulletBody() const { return m_bulletBody; }

    /**
     * @brief Get the creation descriptor for this body.
     * @return  Const reference to the PhysicsBodyDesc used to create this body.
     */
    const PhysicsBodyDesc& GetDesc() const { return m_desc; }

    // =========================================================================
    // Console integration
    // =========================================================================

    /**
     * @brief Return a formatted summary string for the debug console.
     *
     * Includes name, type, position, velocity, mass, material properties, and
     * active/trigger flags. Bound to the console command `physics body info <name>`.
     *
     * @return  Human-readable multi-line info string.
     */
    std::string GetInfo() const;

    /**
     * @brief Set a named floating-point property via the debug console.
     *
     * Accepted property names: `"mass"`, `"friction"`, `"restitution"`,
     * `"linearDamping"`, `"angularDamping"`.
     *
     * @param property  Property name (case-sensitive).
     * @param value     New value to assign.
     */
    void Console_SetProperty(const std::string& property, float value);

    /**
     * @brief Apply a force impulse via the debug console.
     *
     * Equivalent to `ApplyImpulse({x, y, z})` — useful for testing physics
     * interactions at runtime without modifying source code.
     *
     * @param x  Force X component (world space, N·s).
     * @param y  Force Y component (world space, N·s).
     * @param z  Force Z component (world space, N·s).
     */
    void Console_ApplyForce(float x, float y, float z);

private:
    /** @brief Descriptor capturing all creation-time parameters for this body. */
    PhysicsBodyDesc m_desc;

    /** @brief Underlying Bullet rigid body; owned and destroyed by the Bullet dynamics world. */
    btRigidBody* m_bulletBody;

    /**
     * @brief Collision group bitmask for Bullet broadphase filtering.
     *
     * Default: 1 (bit 0 set — standard "world objects" group).
     */
    uint16_t m_collisionGroup = 1;

    /**
     * @brief Collision filter mask — defines which groups this body collides with.
     *
     * Default: 0xFFFF (all bits set — collide with every group).
     */
    uint16_t m_collisionMask = 0xFFFF;
};

/**
 * @brief Physics constraint wrapper
 */
class PhysicsConstraint
{
public:
    PhysicsConstraint(ConstraintType type, btTypedConstraint* bulletConstraint);
    ~PhysicsConstraint();

    ConstraintType GetType() const { return m_type; }
    void SetEnabled(bool enabled);
    bool IsEnabled() const;
    void SetBreakingThreshold(float threshold);
    float GetBreakingThreshold() const;

    btTypedConstraint* GetBulletConstraint() const { return m_bulletConstraint; }

private:
    ConstraintType m_type;
    btTypedConstraint* m_bulletConstraint;
};

/**
 * @class PhysicsSystem
 * @brief Central manager for the Bullet Physics dynamics world.
 *
 * PhysicsSystem owns and drives the Bullet `btDiscreteDynamicsWorld`. It handles
 * body and constraint lifecycle, advances the simulation each frame, routes
 * collision/trigger callbacks, and exposes spatial query APIs (raycast, overlap).
 *
 * ### Lifecycle
 * 1. Call `Initialize()` once at engine startup (creates the Bullet world).
 * 2. Call `Update(deltaTime)` each frame (advances physics by `deltaTime`).
 * 3. Call `Shutdown()` before the engine exits (destroys all bodies and the world).
 *
 * ### Coordinate system
 * All public methods accept and return DirectX Math types (XMFLOAT3, XMMATRIX).
 * Conversion to/from Bullet's `btVector3`/`btTransform` is handled internally.
 *
 * ### Gravity
 * Default gravity is `{0, -9.81, 0}` m/s² (Earth). Change it via `SetGravity()`.
 * A zero-gravity world is useful for space or underwater environments.
 */
class PhysicsSystem
{
public:
    /**
     * @brief Physics system metrics
     */
    struct PhysicsMetrics
    {
        uint32_t activeRigidBodies;       ///< Number of active rigid bodies
        uint32_t totalRigidBodies;        ///< Total number of rigid bodies
        uint32_t activeConstraints;       ///< Number of active constraints
        uint32_t collisionPairs;          ///< Active collision pairs
        uint32_t broadphaseProxies;       ///< Broadphase proxy count
        float simulationTime;             ///< Physics simulation time (ms)
        float collisionTime;              ///< Collision detection time (ms)
        uint32_t substeps;                ///< Number of substeps per frame
        float timeStep;                   ///< Physics time step
        bool debugDrawEnabled;            ///< Debug drawing enabled
        uint32_t raycastCount;            ///< Raycasts performed this frame
    };

    PhysicsSystem();
    ~PhysicsSystem();

    /**
     * @brief Initialize the Bullet physics world and all supporting structures.
     *
     * Creates the btDefaultCollisionConfiguration, btCollisionDispatcher,
     * btDbvtBroadphase, btSequentialImpulseConstraintSolver, and
     * btDiscreteDynamicsWorld. Sets default gravity to `{0, -9.81, 0}`.
     *
     * @return  `S_OK` on success; an `HRESULT` failure code if Bullet structures
     *          could not be allocated (out-of-memory conditions).
     */
    HRESULT Initialize();

    /**
     * @brief Destroy all physics bodies, constraints, and the dynamics world.
     *
     * Removes all bodies and constraints from the Bullet world, deletes all Bullet
     * objects, and releases internal state. Safe to call multiple times. After
     * Shutdown() the system must be re-initialized via Initialize() before use.
     */
    void Shutdown();

    /**
     * @brief Advance the physics simulation by `deltaTime` seconds.
     *
     * Internally calls `btDiscreteDynamicsWorld::stepSimulation(deltaTime,
     * maxSubsteps, timeStep)`. After the step, collision and trigger callbacks
     * are fired for all new/continuing/ended contact pairs.
     *
     * @param deltaTime  Time elapsed since the last frame in seconds. Values above
     *                   `maxSubsteps × timeStep` are clamped to prevent the
     *                   simulation from spiralling (Bullet handles this internally).
     */
    void Update(float deltaTime);

    // =========================================================================
    // World settings
    // =========================================================================

    /**
     * @brief Set the global gravity acceleration vector.
     *
     * Applied to all Dynamic bodies each simulation step. Default: `{0, -9.81, 0}`.
     * Set to `{0, 0, 0}` for zero-gravity (space) environments.
     *
     * @param gravity  Gravity acceleration in metres per second squared (world space).
     */
    void SetGravity(const XMFLOAT3& gravity);

    /**
     * @brief Get the current global gravity acceleration vector.
     * @return  Current gravity in m/s² (world space).
     */
    XMFLOAT3 GetGravity() const;

    /**
     * @brief Set the fixed internal physics time step.
     *
     * Smaller values improve simulation accuracy but increase CPU cost. Default: 1/60 s.
     * Must be > 0.
     *
     * @param timeStep  Fixed step duration in seconds. Typical range: 1/120 to 1/30.
     */
    void SetTimeStep(float timeStep) { m_timeStep = timeStep; }

    /**
     * @brief Get the current fixed physics time step.
     * @return  Step duration in seconds.
     */
    float GetTimeStep() const { return m_timeStep; }

    /**
     * @brief Set the maximum number of sub-steps per Update() call.
     *
     * If `deltaTime > maxSubsteps × timeStep` no additional sub-steps are run,
     * which prevents the simulation from "blowing up" on long frames. Default: 10.
     *
     * @param maxSubsteps  Maximum sub-step count per frame. Must be ≥ 1.
     */
    void SetMaxSubsteps(int maxSubsteps) { m_maxSubsteps = maxSubsteps; }

    /**
     * @brief Get the current maximum sub-step count.
     * @return  Maximum sub-steps per frame.
     */
    int GetMaxSubsteps() const { return m_maxSubsteps; }

    // =========================================================================
    // Body management
    // =========================================================================

    /**
     * @brief Create and register a physics body in the dynamics world.
     *
     * Constructs a `btCollisionShape` (or retrieves a cached one), creates a
     * `btRigidBody` with the specified mass and material, adds it to the Bullet
     * world, and returns a `shared_ptr<PhysicsBody>` wrapper.
     *
     * @param desc  Full description of the body to create (type, shape, material, etc.).
     * @return      Shared pointer to the new PhysicsBody, or nullptr on failure.
     */
    std::shared_ptr<PhysicsBody> CreateBody(const PhysicsBodyDesc& desc);

    /**
     * @brief Remove a body from the dynamics world and release its resources.
     *
     * Removes the `btRigidBody` from the Bullet world and erases the body from
     * internal tracking. The `shared_ptr` passed in is the last reference counted
     * by the system; after this call only caller-held references keep it alive.
     *
     * @param body  Body to remove. No-op if nullptr or not managed by this system.
     */
    void RemoveBody(std::shared_ptr<PhysicsBody> body);

    /**
     * @brief Remove all registered bodies from the dynamics world.
     *
     * Equivalent to calling RemoveBody() for every body. Safe to call before
     * loading a new level or resetting the game state.
     */
    void RemoveAllBodies();

    /**
     * @brief Get a read-only reference to the list of all registered bodies.
     *
     * Iterating over this list is useful for batch operations (e.g. applying wind
     * force to all Dynamic bodies). Do not add or remove bodies while iterating.
     *
     * @return  Const reference to the internal bodies vector.
     */
    const std::vector<std::shared_ptr<PhysicsBody>>& GetBodies() const { return m_bodies; }

    // =========================================================================
    // Constraint management
    // =========================================================================

    /**
     * @brief Create a hinge constraint between two bodies (or a body and the world).
     *
     * A hinge allows rotation around a single axis (like a door hinge or wheel).
     * The pivot points are in local body space.
     *
     * @param bodyA   First body. Must not be nullptr.
     * @param bodyB   Second body. Pass nullptr or a Static body to fix bodyA to world.
     * @param pivotA  Pivot point in bodyA's local space.
     * @param pivotB  Pivot point in bodyB's local space.
     * @param axisA   Hinge axis in bodyA's local space (normalized).
     * @param axisB   Hinge axis in bodyB's local space (normalized).
     * @return        Shared pointer to the new PhysicsConstraint.
     */
    std::shared_ptr<PhysicsConstraint> CreateHingeConstraint(
        std::shared_ptr<PhysicsBody> bodyA, std::shared_ptr<PhysicsBody> bodyB,
        const XMFLOAT3& pivotA, const XMFLOAT3& pivotB,
        const XMFLOAT3& axisA, const XMFLOAT3& axisB);

    /**
     * @brief Create a slider constraint allowing linear motion along a single axis.
     *
     * The constraint frames define the slide axis in each body's local space.
     * Useful for elevator platforms, hydraulic pistons, and sliding doors.
     *
     * @param bodyA   First body.
     * @param bodyB   Second body.
     * @param frameA  Constraint frame in bodyA's local space (rotation defines slide axis).
     * @param frameB  Constraint frame in bodyB's local space.
     * @return        Shared pointer to the new PhysicsConstraint.
     */
    std::shared_ptr<PhysicsConstraint> CreateSliderConstraint(
        std::shared_ptr<PhysicsBody> bodyA, std::shared_ptr<PhysicsBody> bodyB,
        const XMMATRIX& frameA, const XMMATRIX& frameB);

    /**
     * @brief Create a fixed (weld) constraint that locks two bodies together.
     *
     * Both translation and rotation between the two bodies are fully constrained.
     * Use to attach accessories to a vehicle or weld broken prop pieces.
     *
     * @param bodyA   First body.
     * @param bodyB   Second body.
     * @param frameA  Attachment frame in bodyA's local space.
     * @param frameB  Attachment frame in bodyB's local space.
     * @return        Shared pointer to the new PhysicsConstraint.
     */
    std::shared_ptr<PhysicsConstraint> CreateFixedConstraint(
        std::shared_ptr<PhysicsBody> bodyA, std::shared_ptr<PhysicsBody> bodyB,
        const XMMATRIX& frameA, const XMMATRIX& frameB);

    /**
     * @brief Remove a constraint from the dynamics world.
     *
     * @param constraint  Constraint to remove. No-op if nullptr or not registered.
     */
    void RemoveConstraint(std::shared_ptr<PhysicsConstraint> constraint);

    // =========================================================================
    // Spatial queries
    // =========================================================================

    /**
     * @brief Cast a ray and return the first hit body.
     *
     * Uses Bullet's `btCollisionWorld::ClosestRayResultCallback`. Returns the closest
     * body whose bounding volume intersects the ray segment.
     *
     * @param origin       Ray start position (world space).
     * @param direction    Ray direction (world space, **need not** be normalized;
     *                     it is normalized internally).
     * @param maxDistance  Maximum ray length in metres. Default: 1000 m.
     * @return             RaycastHit with `hasHit = false` if nothing was hit.
     */
    RaycastHit Raycast(const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance = 1000.0f);

    /**
     * @brief Cast a ray and return **all** hit bodies sorted by distance.
     *
     * Uses Bullet's `btCollisionWorld::AllHitsRayResultCallback`. Returned hits are
     * sorted ascending by `distance` (closest first).
     *
     * @param origin       Ray start position (world space).
     * @param direction    Ray direction (world space).
     * @param maxDistance  Maximum ray length in metres. Default: 1000 m.
     * @return             Vector of RaycastHit; empty if nothing was hit.
     */
    std::vector<RaycastHit> RaycastAll(const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance = 1000.0f);

    /**
     * @brief Find all bodies overlapping a sphere.
     *
     * Uses a Bullet ghost object sphere query to find all bodies whose AABB
     * overlaps the test sphere. Does not perform exact shape tests.
     *
     * @param center   Center of the test sphere (world space).
     * @param radius   Radius of the test sphere (metres).
     * @param results  Output vector populated with non-owning pointers to overlapping bodies.
     * @return         `true` if at least one body was found; `false` if the query returned nothing.
     */
    bool SphereOverlap(const XMFLOAT3& center, float radius, std::vector<PhysicsBody*>& results);

    /**
     * @brief Find all bodies overlapping an axis-aligned box.
     *
     * Queries the Bullet broadphase for bodies whose AABBs overlap the test box.
     *
     * @param center       Center of the test box (world space).
     * @param halfExtents  Half-extents of the box on each axis (metres).
     * @param results      Output vector populated with non-owning pointers to overlapping bodies.
     * @return             `true` if at least one body was found; `false` otherwise.
     */
    bool BoxOverlap(const XMFLOAT3& center, const XMFLOAT3& halfExtents, std::vector<PhysicsBody*>& results);

    // =========================================================================
    // Collision callbacks
    // =========================================================================

    /**
     * @brief Register a callback invoked for every new contact point each frame.
     *
     * The callback is called with a ContactInfo struct describing the two bodies,
     * the contact point, normal, penetration depth, and applied impulse.
     * Only one collision callback can be active at a time; calling this overwrites
     * the previous registration.
     *
     * @param callback  Function accepting a const ContactInfo reference. Called from
     *                  within Update() — do not modify the physics world from inside
     *                  the callback.
     */
    void SetCollisionCallback(std::function<void(const ContactInfo&)> callback) { m_collisionCallback = callback; }

    /**
     * @brief Register a callback invoked when trigger volumes are entered or exited.
     *
     * The callback signature is `void(PhysicsBody* trigger, PhysicsBody* other, bool entered)`:
     * - `trigger`: the trigger-mode body.
     * - `other`: the overlapping body.
     * - `entered`: `true` when the overlap begins; `false` when it ends.
     *
     * @param callback  Trigger event handler. Called from within Update().
     */
    void SetTriggerCallback(std::function<void(PhysicsBody*, PhysicsBody*, bool)> callback) { m_triggerCallback = callback; }

    // =========================================================================
    // Debug rendering
    // =========================================================================

    /**
     * @brief Enable or disable the Bullet physics debug overlay.
     *
     * When enabled, collision shapes, contact points, and constraint frames are
     * drawn each frame via the engine's line renderer. Useful for diagnosing
     * physics setup issues. Has no effect in release builds if the DebugDrawer
     * is compiled out.
     *
     * @param enabled  `true` to draw debug geometry; `false` to hide it.
     */
    void EnableDebugDraw(bool enabled) { m_debugDrawEnabled = enabled; }

    /**
     * @brief Check whether debug drawing is currently enabled.
     * @return  `true` if the debug overlay is active.
     */
    bool IsDebugDrawEnabled() const { return m_debugDrawEnabled; }

    /**
     * @brief Set the debug draw mode bitmask for the Bullet DebugDrawer.
     *
     * Mode bits correspond to `btIDebugDraw::DebugDrawModes`:
     * - `1` = draw wireframe shapes
     * - `2` = draw AABB boxes
     * - `4` = draw contact normals
     * - etc.
     * Pass `0` to hide all debug geometry without disabling the drawer.
     *
     * @param mode  Bitmask of btIDebugDraw::DebugDrawModes values.
     */
    void SetDebugDrawMode(int mode);

    /**
     * @brief Issue all queued Bullet debug draw calls for the current frame.
     *
     * Should be called once per frame after Update() and before the scene is
     * rendered, so debug lines are drawn on top of world geometry. No-op if
     * debug drawing is disabled or the DebugDrawer is not initialized.
     */
    void RenderDebug();

    // =========================================================================
    // Material management
    // =========================================================================

    /**
     * @brief Register a named physics material preset.
     *
     * Presets can be retrieved by name and assigned to PhysicsBodyDesc::material
     * before creating a body. Common practice is to register all presets during
     * level load (e.g. "Concrete", "Metal", "Wood", "Ice").
     *
     * @param name      Unique material identifier (case-sensitive).
     * @param material  Material properties to associate with this name.
     */
    void RegisterMaterial(const std::string& name, const PhysicsMaterial& material);

    /**
     * @brief Look up a registered material preset by name.
     *
     * @param name  Material name as registered via RegisterMaterial().
     * @return      Const pointer to the PhysicsMaterial, or `nullptr` if not found.
     */
    const PhysicsMaterial* GetMaterial(const std::string& name) const;

    /**
     * @brief Set the material applied to bodies that do not specify one.
     *
     * @param material  Default material properties (friction, restitution, damping).
     */
    void SetDefaultMaterial(const PhysicsMaterial& material) { m_defaultMaterial = material; }

    // =========================================================================
    // Metrics
    // =========================================================================

    /**
     * @brief Retrieve performance and state metrics from the last simulation step.
     *
     * Metrics include body counts, simulation time, collision pairs, and raycast
     * statistics. Safe to call at any point; returns a snapshot with thread-safety
     * via an internal mutex.
     *
     * @return  PhysicsMetrics struct populated with data from the most recent Update().
     */
    PhysicsMetrics GetMetrics() const;

    // =========================================================================
    // Console integration methods
    // =========================================================================

    /**
     * @brief Retrieve a snapshot of physics metrics for the debug console.
     *
     * Returns the same data as GetMetrics() in a console-friendly form.
     * Bound to the console command `physics metrics`.
     *
     * @return  PhysicsMetrics populated with data from the last Update().
     */
    PhysicsMetrics Console_GetMetrics() const;

    /**
     * @brief Return a formatted list of all registered physics bodies.
     *
     * Each entry includes the body's name, type, position, and active state.
     * Bound to the console command `physics list`.
     *
     * @return  Human-readable multi-line body listing.
     */
    std::string Console_ListBodies() const;

    /**
     * @brief Return detailed information about a named body for the console.
     *
     * Delegates to PhysicsBody::GetInfo() for the body with the given name.
     * Bound to the console command `physics body info <name>`.
     *
     * @param name  Name of the body to inspect (as set in PhysicsBodyDesc::name).
     * @return      Formatted info string, or an error message if the body is not found.
     */
    std::string Console_GetBodyInfo(const std::string& name) const;

    /**
     * @brief Spawn a dynamic physics body from the debug console.
     *
     * Creates a simple box-shaped Dynamic body at the given world position.
     * Bound to the console command `physics create <name> <type> <x> <y> <z>`.
     *
     * @param name  Debug name for the new body.
     * @param type  Body type string: `"static"`, `"kinematic"`, or `"dynamic"`.
     * @param x     Initial X position (metres).
     * @param y     Initial Y position (metres).
     * @param z     Initial Z position (metres).
     * @return      `true` if the body was created successfully; `false` on invalid type string.
     */
    bool Console_CreateBody(const std::string& name, const std::string& type, float x, float y, float z);

    /**
     * @brief Remove a named body from the simulation via the debug console.
     *
     * Bound to the console command `physics remove <name>`.
     *
     * @param name  Name of the body to remove.
     * @return      `true` if the body was found and removed; `false` if not found.
     */
    bool Console_RemoveBody(const std::string& name);

    /**
     * @brief Set the world gravity vector via the debug console.
     *
     * Bound to the console command `physics gravity <x> <y> <z>`.
     *
     * @param x  Gravity X component (m/s²).
     * @param y  Gravity Y component (m/s²). Typically -9.81 for Earth.
     * @param z  Gravity Z component (m/s²).
     */
    void Console_SetGravity(float x, float y, float z);

    /**
     * @brief Set a floating-point property on a named body via the console.
     *
     * Delegates to PhysicsBody::Console_SetProperty(). Accepted properties:
     * `"mass"`, `"friction"`, `"restitution"`, `"linearDamping"`, `"angularDamping"`.
     * Bound to the console command `physics set <name> <property> <value>`.
     *
     * @param name      Name of the target body.
     * @param property  Property name (case-sensitive).
     * @param value     New value.
     */
    void Console_SetBodyProperty(const std::string& name, const std::string& property, float value);

    /**
     * @brief Apply a continuous force to a named body via the console.
     *
     * Calls PhysicsBody::ApplyForce(). The force is applied to the body's center
     * of mass for a single frame.
     * Bound to the console command `physics force <name> <x> <y> <z>`.
     *
     * @param name  Name of the target body.
     * @param x     Force X component (N).
     * @param y     Force Y component (N).
     * @param z     Force Z component (N).
     */
    void Console_ApplyForce(const std::string& name, float x, float y, float z);

    /**
     * @brief Apply an instantaneous impulse to a named body via the console.
     *
     * Calls PhysicsBody::ApplyImpulse(). Useful for testing collision responses.
     * Bound to the console command `physics impulse <name> <x> <y> <z>`.
     *
     * @param name  Name of the target body.
     * @param x     Impulse X component (N·s).
     * @param y     Impulse Y component (N·s).
     * @param z     Impulse Z component (N·s).
     */
    void Console_ApplyImpulse(const std::string& name, float x, float y, float z);

    /**
     * @brief Toggle the Bullet physics debug overlay via the console.
     *
     * Equivalent to calling EnableDebugDraw(). Bound to the console command
     * `physics debug <on|off>`.
     *
     * @param enabled  `true` to show debug shapes; `false` to hide them.
     */
    void Console_EnableDebugDraw(bool enabled);

    /**
     * @brief Pause or resume the physics simulation via the console.
     *
     * When paused, Update() is a no-op. Bound to the console command
     * `physics pause <on|off>`.
     *
     * @param paused  `true` to pause the simulation; `false` to resume it.
     */
    void Console_PausePhysics(bool paused);

    /**
     * @brief Change the fixed simulation time step via the console.
     *
     * Equivalent to SetTimeStep(). Takes effect on the next Update() call.
     * Bound to the console command `physics timestep <seconds>`.
     *
     * @param timeStep  New time step in seconds. Typical range: 1/120 to 1/30.
     */
    void Console_SetTimeStep(float timeStep);

    /**
     * @brief Perform a raycast and format the result for the debug console.
     *
     * Calls Raycast() and returns a human-readable summary of the hit (body name,
     * distance, hit point, surface normal). Bound to the console command
     * `physics raycast <ox> <oy> <oz> <dx> <dy> <dz> <maxDist>`.
     *
     * @param originX    Ray origin X (metres).
     * @param originY    Ray origin Y (metres).
     * @param originZ    Ray origin Z (metres).
     * @param dirX       Ray direction X.
     * @param dirY       Ray direction Y.
     * @param dirZ       Ray direction Z.
     * @param maxDistance  Maximum ray length (metres).
     * @return            Formatted hit-result string, or "No hit" if nothing was intersected.
     */
    std::string Console_Raycast(float originX, float originY, float originZ,
                               float dirX, float dirY, float dirZ, float maxDistance);

    /**
     * @brief Remove all bodies and reset the physics world to its initial state.
     *
     * Calls RemoveAllBodies() and resets metrics. Bound to the console command
     * `physics reset`. Useful for level transitions or hot-reloads.
     */
    void Console_Reset();

private:
    // =========================================================================
    // Bullet Physics world objects
    // =========================================================================

    /** @brief Main Bullet dynamics world. Owns all rigid bodies and drives simulation. */
    btDiscreteDynamicsWorld* m_dynamicsWorld;

    /** @brief Bullet collision configuration (algorithm selection, memory pools). */
    btDefaultCollisionConfiguration* m_collisionConfig;

    /** @brief Bullet near-phase collision dispatcher (routes shape-pair tests). */
    btCollisionDispatcher* m_dispatcher;

    /** @brief Bullet DBVT broadphase: fast AABB tree for coarse collision culling. */
    btDbvtBroadphase* m_broadphase;

    /** @brief Bullet constraint/contact solver (Sequential Impulse method). */
    btSequentialImpulseConstraintSolver* m_solver;

    // =========================================================================
    // Body and constraint registries
    // =========================================================================

    /** @brief All registered PhysicsBody wrappers, including Static and Trigger bodies. */
    std::vector<std::shared_ptr<PhysicsBody>> m_bodies;

    /** @brief All registered constraint wrappers. */
    std::vector<std::shared_ptr<PhysicsConstraint>> m_constraints;

    /**
     * @brief Secondary index mapping body names → PhysicsBody for fast console lookups.
     *
     * Populated from `PhysicsBodyDesc::name` at CreateBody() time. Bodies without
     * a name are not inserted here.
     */
    std::unordered_map<std::string, std::shared_ptr<PhysicsBody>> m_namedBodies;

    // =========================================================================
    // Collision shape cache
    // =========================================================================

    /**
     * @brief Cache mapping CollisionShapeDesc hashes → Bullet collision shapes.
     *
     * Allows multiple bodies with identical shape descriptors to share a single
     * `btCollisionShape` instance, reducing memory overhead for scenes with many
     * identical objects (e.g. 500 boxes of the same size).
     */
    std::unordered_map<size_t, btCollisionShape*> m_shapeCache;

    // =========================================================================
    // Material presets
    // =========================================================================

    /** @brief Named material preset registry (populated via RegisterMaterial()). */
    std::unordered_map<std::string, PhysicsMaterial> m_materials;

    /**
     * @brief Default material applied to bodies that do not specify a custom material.
     *
     * Initial values: friction = 0.5, restitution = 0.1, damping = 0.1.
     */
    PhysicsMaterial m_defaultMaterial;

    // =========================================================================
    // Simulation settings
    // =========================================================================

    /**
     * @brief Fixed integration time step passed to `btDiscreteDynamicsWorld::stepSimulation`.
     *
     * Default: 1/60 s. Smaller values increase accuracy but cost more CPU.
     */
    float m_timeStep = 1.0f / 60.0f;

    /**
     * @brief Maximum sub-steps per Update() call.
     *
     * Prevents the simulation from running more steps than this when `deltaTime`
     * is large (e.g. a frame took 500 ms). Default: 10.
     */
    int m_maxSubsteps = 10;

    /**
     * @brief When true, Update() skips the Bullet stepSimulation call.
     *
     * Set via Console_PausePhysics(). Bodies remain frozen in place; no callbacks fire.
     */
    bool m_paused = false;

    // =========================================================================
    // Debug rendering
    // =========================================================================

    /** @brief When true, RenderDebug() issues Bullet draw calls each frame. */
    bool m_debugDrawEnabled = false;

    /** @brief Custom DebugDrawer adapter that routes Bullet line draws to Spark's renderer. */
    class DebugDrawer* m_debugDrawer;

    // =========================================================================
    // Callbacks
    // =========================================================================

    /**
     * @brief User callback invoked once per contact pair per simulation step.
     *
     * Null by default. Set via SetCollisionCallback(). Called from ProcessCollisions().
     */
    std::function<void(const ContactInfo&)> m_collisionCallback;

    /**
     * @brief User callback invoked when trigger overlap state changes.
     *
     * Null by default. Set via SetTriggerCallback(). Third `bool` parameter is
     * `true` on enter, `false` on exit.
     */
    std::function<void(PhysicsBody*, PhysicsBody*, bool)> m_triggerCallback;

    // =========================================================================
    // Metrics
    // =========================================================================

    /**
     * @brief Mutex protecting m_metrics from concurrent read/write.
     *
     * Marked mutable to allow const GetMetrics() to lock it.
     */
    mutable std::mutex m_metricsMutex;

    /** @brief Snapshot of per-frame physics statistics, updated by UpdateMetrics(). */
    PhysicsMetrics m_metrics;

    // =========================================================================
    // Private helpers
    // =========================================================================

    /**
     * @brief Dispatch to the appropriate shape-creation helper based on `desc.type`.
     *
     * Returns a cached shape if one with the same hash already exists; otherwise
     * creates a new one, caches it, and returns it.
     *
     * @param desc  Shape descriptor to create.
     * @return      Non-owning pointer to the Bullet collision shape (owned by m_shapeCache).
     */
    btCollisionShape* CreateCollisionShape(const CollisionShapeDesc& desc);

    /**
     * @brief Create a `btBoxShape` from half-extent dimensions.
     * @param dimensions  Half-extents on each axis (metres).
     * @return            Newly allocated `btBoxShape`.
     */
    btCollisionShape* CreateBoxShape(const XMFLOAT3& dimensions);

    /**
     * @brief Create a `btSphereShape` with the given radius.
     * @param radius  Sphere radius (metres).
     * @return        Newly allocated `btSphereShape`.
     */
    btCollisionShape* CreateSphereShape(float radius);

    /**
     * @brief Create a `btCapsuleShape` with the given radius and cylindrical height.
     * @param radius  Radius of the end-cap hemispheres (metres).
     * @param height  Height of the cylindrical section, excluding end-caps (metres).
     * @return        Newly allocated `btCapsuleShape`.
     */
    btCollisionShape* CreateCapsuleShape(float radius, float height);

    /**
     * @brief Create a `btBvhTriangleMeshShape` from raw vertex/index data.
     *
     * Triangle mesh shapes are static-only; they do not generate responses when
     * used on Dynamic bodies. Ideal for level geometry.
     *
     * @param vertices  Array of vertex positions.
     * @param indices   Triangle index list (groups of 3).
     * @return          Newly allocated `btBvhTriangleMeshShape`.
     */
    btCollisionShape* CreateMeshShape(const std::vector<XMFLOAT3>& vertices, const std::vector<uint32_t>& indices);

    /**
     * @brief Create a `btConvexHullShape` from raw vertex data.
     *
     * Suitable for Dynamic bodies that need approximate mesh-like collision.
     * Bullet internally computes the convex hull.
     *
     * @param vertices  Input point cloud (duplicates are acceptable).
     * @return          Newly allocated `btConvexHullShape`.
     */
    btCollisionShape* CreateConvexHullShape(const std::vector<XMFLOAT3>& vertices);

    /**
     * @brief Refresh m_metrics from the current Bullet world state.
     *
     * Called at the end of each Update() to capture body counts, timing, and
     * raycast statistics for the just-completed simulation step.
     */
    void UpdateMetrics();

    /**
     * @brief Iterate Bullet's contact manifolds and fire collision/trigger callbacks.
     *
     * Called from Update() after stepSimulation(). For each manifold pair, checks
     * whether either body is a trigger and dispatches the appropriate callback.
     */
    void ProcessCollisions();

    /**
     * @brief Compute a hash for a CollisionShapeDesc to use as a cache key.
     *
     * The hash covers all fields that affect the shape geometry (type, dimensions,
     * radius, height). Vertex/index data for Mesh shapes is hashed by content.
     *
     * @param desc  Shape descriptor to hash.
     * @return      64-bit hash value.
     */
    size_t HashShape(const CollisionShapeDesc& desc);

    /**
     * @brief Convert an XMFLOAT3 to a Bullet btVector3.
     * @param vec  Source DirectX Math vector.
     * @return     Equivalent Bullet vector.
     */
    class btVector3 ToBullet(const XMFLOAT3& vec) const;

    /**
     * @brief Convert a Bullet btVector3 to an XMFLOAT3.
     * @param vec  Source Bullet vector.
     * @return     Equivalent DirectX Math float3.
     */
    XMFLOAT3 FromBullet(const class btVector3& vec) const;

    /**
     * @brief Convert Euler angles (degrees) to a Bullet quaternion.
     * @param euler  Rotation in degrees (X=pitch, Y=yaw, Z=roll).
     * @return       Equivalent Bullet quaternion.
     */
    class btQuaternion ToBulletQuaternion(const XMFLOAT3& euler) const;

    /**
     * @brief Convert a Bullet quaternion to Euler angles in degrees.
     * @param quat  Source Bullet quaternion.
     * @return      Rotation in degrees (X=pitch, Y=yaw, Z=roll).
     */
    XMFLOAT3 FromBullet(const class btQuaternion& quat) const;
};

// Utility functions
std::string PhysicsBodyTypeToString(PhysicsBodyType type);
PhysicsBodyType StringToPhysicsBodyType(const std::string& str);
std::string CollisionShapeTypeToString(CollisionShapeType type);
CollisionShapeType StringToCollisionShapeType(const std::string& str);
std::string ConstraintTypeToString(ConstraintType type);
ConstraintType StringToConstraintType(const std::string& str);
