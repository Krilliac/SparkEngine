/**
 * @file PhysicsTypes.h
 * @brief Physics type definitions, enums, descriptors, and utility functions.
 * @author Spark Engine Team
 * @date 2025
 *
 * Extracted from PhysicsSystem.h so that code needing only physics types
 * (e.g. ECS components, editor panels) can include this lightweight header
 * without pulling in the full PhysicsSystem class and its Jolt dependencies.
 */

#pragma once
#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <cstdint>

using DirectX::XMFLOAT3;

// =============================================================================
// Enums
// =============================================================================

/**
 * @brief Physics body types
 */
enum class PhysicsBodyType
{
    Static,    ///< Static body (immovable)
    Kinematic, ///< Kinematic body (user-controlled movement)
    Dynamic    ///< Dynamic body (physics-controlled)
};

/**
 * @brief Collision shapes
 */
enum class CollisionShapeType
{
    Box,             ///< Box collision shape
    Sphere,          ///< Sphere collision shape
    Capsule,         ///< Capsule collision shape
    Cylinder,        ///< Cylinder collision shape
    Cone,            ///< Cone collision shape (approximated as convex hull in Jolt)
    Mesh,            ///< Triangle mesh shape (static geometry only)
    ConvexHull,      ///< Convex hull shape
    Heightfield,     ///< Heightfield terrain shape (optimized grid)
    Compound,        ///< Compound shape (multiple sub-shapes)
    TaperedCapsule,  ///< Capsule with different top/bottom radii
    TaperedCylinder, ///< Cylinder with different top/bottom radii
    Plane,           ///< Infinite plane shape
    Scaled,          ///< Scaled wrapper around another shape
    Empty            ///< Empty shape (no collision geometry)
};

/**
 * @brief Motion quality — controls how precisely collisions are detected.
 *
 * Discrete is cheapest but fast-moving bodies can tunnel through thin geometry.
 * LinearCast prevents tunneling by sweeping the shape from start to end position.
 */
enum class MotionQuality
{
    Discrete,  ///< Standard discrete collision detection (cheapest)
    LinearCast ///< CCD via linear cast — prevents tunneling for fast objects (bullets, projectiles)
};

/**
 * @brief Allowed degrees of freedom for a physics body.
 *
 * Bitfield controlling which translation/rotation axes are active.
 * Use to constrain bodies to 2D planes, lock rotation axes, etc.
 */
enum class AllowedDOFs : uint8_t
{
    All = 0b111111,          ///< All translation and rotation (default 3D)
    TranslationX = 0b000001, ///< Allow X translation
    TranslationY = 0b000010, ///< Allow Y translation
    TranslationZ = 0b000100, ///< Allow Z translation
    RotationX = 0b001000,    ///< Allow X rotation
    RotationY = 0b010000,    ///< Allow Y rotation
    RotationZ = 0b100000,    ///< Allow Z rotation
    Plane2D = 0b100011,      ///< 2D mode: translate X/Y, rotate Z only
};

/**
 * @brief Physics constraint types
 */
enum class ConstraintType
{
    Point2Point,   ///< Point-to-point (ball-socket) constraint
    Hinge,         ///< Hinge constraint (single rotation axis)
    Slider,        ///< Slider constraint (linear motion along axis)
    ConeTwist,     ///< Cone-twist / swing-twist constraint (ragdoll joints)
    Generic6DOF,   ///< 6-DOF constraint (per-axis freedom control)
    Fixed,         ///< Fixed (weld) constraint — locks two bodies together
    Distance,      ///< Distance constraint — maintains distance between anchor points
    Cone,          ///< Cone constraint — limits rotation to a cone
    Gear,          ///< Gear constraint — couples rotation of two hinge constraints
    RackAndPinion, ///< Rack-and-pinion — couples rotation to linear motion
    Pulley,        ///< Pulley constraint — rope/cable between two bodies
    Path           ///< Path constraint — constrains body to follow a path
};

// =============================================================================
// Structs
// =============================================================================

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
 *   the physics engine's default `sqrt(frA * frB)` mixing.
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
 * PhysicsSystem::CreateBody() reads this struct to construct the appropriate Jolt
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
     * - For **ConvexHull**: provide all hull vertices (duplicates are handled by Jolt).
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

    /**
     * @brief Secondary radius for TaperedCapsule / TaperedCylinder shapes.
     *
     * For TaperedCapsule: `radius` is the top radius, `topRadius` is the bottom radius.
     * For TaperedCylinder: `radius` is the top radius, `topRadius` is the bottom radius.
     * Ignored by other shape types.
     */
    float topRadius = 0.5f;

    /**
     * @brief Heightfield data for terrain collision shapes.
     *
     * A flat array of height values in row-major order. Width × depth samples.
     * Used only when `type == CollisionShapeType::Heightfield`.
     */
    std::vector<float> heightfieldData;

    /**
     * @brief Number of samples along each axis for heightfield shapes.
     *
     * The heightfield is `heightfieldSamples × heightfieldSamples` (square grid).
     * Must be a power of 2 + 1 for optimal Jolt performance.
     */
    uint32_t heightfieldSamples = 0;

    /**
     * @brief Scale applied to heightfield height values.
     *
     * Multiplies each raw height value. Use with dimensions for XZ extent.
     */
    float heightfieldScale = 1.0f;

    /**
     * @brief Uniform scale factor for Scaled shape wrapper.
     *
     * Applied uniformly when `type == CollisionShapeType::Scaled`.
     */
    XMFLOAT3 scale = {1.0f, 1.0f, 1.0f};

    /**
     * @brief Plane normal for Plane shape.
     */
    XMFLOAT3 planeNormal = {0, 1, 0};

    /**
     * @brief Plane distance from origin for Plane shape.
     */
    float planeDistance = 0.0f;
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
     * Converted internally to a quaternion before being passed to Jolt. Using
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
     * - 0.0 -> the body behaves as Static regardless of the `type` field. Jolt
     *          treats zero-mass bodies as immovable.
     * - > 0  -> must match `type == Dynamic` for full simulation.
     *
     * Typical values: player capsule ~ 80 kg, grenade ~ 0.4 kg, car ~ 1500 kg.
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
     * @brief Collision filter group bitmask (what this body IS).
     *
     * Used by the broadphase to filter collision pairs. Body A collides
     * with body B only if `(A.group & B.mask) != 0 && (B.group & A.mask) != 0`.
     * Default: 1 (default group). Set to 0 to disable all collisions.
     */
    uint16_t collisionGroup = 1;

    /**
     * @brief Collision filter mask bitmask (what this body DETECTS).
     *
     * Bodies whose group bits overlap this mask will be considered for collision.
     * Default: 0xFFFF (collide with everything).
     */
    uint16_t collisionMask = 0xFFFF;

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

    /**
     * @brief ECS entity ID that owns this physics body.
     *
     * Set automatically by the ECS physics integration when a RigidBodyComponent
     * is created. Used by PhysicsSystem to populate EventBus collision/trigger
     * events with correct entity IDs.
     *
     * Zero means "no entity" (standalone body not tied to the ECS).
     */
    uint32_t entityId = 0;

    /**
     * @brief Motion quality controlling collision detection precision.
     *
     * Use LinearCast for fast-moving objects (bullets, projectiles) to prevent
     * tunneling through thin geometry. Default: Discrete (cheapest).
     */
    MotionQuality motionQuality = MotionQuality::Discrete;

    /**
     * @brief Allowed degrees of freedom for this body.
     *
     * Restrict which axes can translate/rotate. Use Plane2D for side-scrollers.
     * Default: All (full 3D freedom).
     */
    AllowedDOFs allowedDOFs = AllowedDOFs::All;

    /**
     * @brief Per-body gravity multiplier.
     *
     * 1.0 = normal gravity, 0.0 = no gravity (floating), 2.0 = double gravity.
     * Useful for space environments, underwater areas, or moon-like gravity zones.
     */
    float gravityFactor = 1.0f;

    /**
     * @brief Maximum linear velocity (m/s). Jolt clamps to prevent instability.
     *
     * Default: 500 m/s. Reduce for controlled simulations; increase for extreme speeds.
     */
    float maxLinearVelocity = 500.0f;

    /**
     * @brief Maximum angular velocity (rad/s). Jolt clamps to prevent instability.
     *
     * Default: 47.12 (≈ 15π rad/s ≈ 7.5 revolutions/second).
     */
    float maxAngularVelocity = 47.12389f;
};

// =============================================================================
// Motor and spring settings
// =============================================================================

/**
 * @brief Motor state for constraint motors.
 */
enum class MotorState
{
    Off,      ///< Motor disabled
    Velocity, ///< Motor drives to target velocity
    Position  ///< Motor drives to target position (servo)
};

/**
 * @brief Spring configuration for constraints with spring behavior.
 *
 * Springs can be configured with either frequency+damping (intuitive) or
 * stiffness+damping (precise control). Frequency mode: frequency in Hz,
 * damping ratio 0-1 (1 = critically damped). Stiffness mode: N/m or N*m.
 */
struct PhysicsSpringSettings
{
    bool useFrequencyMode = true; ///< true = frequency+damping, false = stiffness+damping
    float frequency = 1.0f;       ///< Spring frequency in Hz (frequency mode)
    float damping = 0.5f;         ///< Damping ratio [0-1] (frequency) or absolute damping (stiffness mode)
    float stiffness = 100.0f;     ///< Spring stiffness N/m (stiffness mode only)
};

/**
 * @brief Motor configuration for motorized constraints.
 */
struct PhysicsMotorSettings
{
    PhysicsSpringSettings spring;  ///< Spring settings for the motor
    float minForceLimit = -1e10f;  ///< Minimum force the motor can apply (N)
    float maxForceLimit = 1e10f;   ///< Maximum force the motor can apply (N)
    float minTorqueLimit = -1e10f; ///< Minimum torque the motor can apply (N*m)
    float maxTorqueLimit = 1e10f;  ///< Maximum torque the motor can apply (N*m)
};

// =============================================================================
// Vehicle types
// =============================================================================

/**
 * @brief Vehicle type enumeration.
 */
enum class PhysicsVehicleType
{
    Wheeled,   ///< Standard wheeled vehicle (car, truck)
    Tracked,   ///< Tracked vehicle (tank, bulldozer)
    Motorcycle ///< Two-wheeled motorcycle with lean stabilization
};

/**
 * @brief Wheel configuration for vehicle physics.
 */
struct VehicleWheelDesc
{
    XMFLOAT3 position = {0, 0, 0};       ///< Wheel attachment position relative to vehicle body
    XMFLOAT3 suspensionDir = {0, -1, 0}; ///< Suspension direction (typically down)
    XMFLOAT3 steeringAxis = {0, 1, 0};   ///< Axis around which the wheel steers
    XMFLOAT3 wheelForward = {0, 0, 1};   ///< Forward direction of the wheel
    XMFLOAT3 wheelUp = {0, 1, 0};        ///< Up direction of the wheel
    float radius = 0.3f;                 ///< Wheel radius (metres)
    float width = 0.1f;                  ///< Wheel width (metres)
    float suspensionMinLength = 0.3f;    ///< Minimum suspension spring length
    float suspensionMaxLength = 0.5f;    ///< Maximum suspension spring length
    float suspensionFrequency = 1.5f;    ///< Suspension spring frequency (Hz)
    float suspensionDamping = 0.5f;      ///< Suspension damping ratio [0-1]
    float maxSteerAngle = 0.0f;          ///< Maximum steering angle (radians, 0 = non-steering)
    float maxBrakeTorque = 1500.0f;      ///< Maximum braking torque (N*m)
    float maxHandBrakeTorque = 4000.0f;  ///< Handbrake torque (N*m)
    float lateralFriction = 1.0f;        ///< Lateral (side) friction multiplier
    float longitudinalFriction = 1.0f;   ///< Longitudinal (forward/backward) friction multiplier
};

/**
 * @brief Vehicle configuration descriptor.
 */
struct VehicleDesc
{
    PhysicsVehicleType type = PhysicsVehicleType::Wheeled;
    std::vector<VehicleWheelDesc> wheels;

    // Engine
    float maxEngineTorque = 500.0f; ///< Maximum engine torque (N*m)
    float minRPM = 1000.0f;         ///< Engine idle RPM
    float maxRPM = 6000.0f;         ///< Engine redline RPM

    // Transmission
    std::vector<float> gearRatios = {2.66f, 1.78f, 1.30f, 1.0f, 0.74f}; ///< Forward gear ratios
    float reverseGearRatio = -2.90f;                                    ///< Reverse gear ratio
    float clutchStrength = 10.0f;                                       ///< Clutch strength

    // Differentials
    float differentialRatio = 3.42f; ///< Final drive ratio

    // Anti-roll bars
    float antiRollBarStiffness = 1000.0f; ///< Anti-roll bar stiffness (N/m, 0 = disabled)

    // Motorcycle-specific
    float leanSpringConstant = 5000.0f; ///< Lean stabilization spring (motorcycle only)
    float leanSpringDamping = 1000.0f;  ///< Lean stabilization damping (motorcycle only)
};

// =============================================================================
// Ragdoll types
// =============================================================================

/**
 * @brief Ragdoll limb/part descriptor.
 */
struct RagdollPartDesc
{
    std::string name;                                          ///< Bone/joint name (e.g., "LeftUpperArm")
    int parentIndex = -1;                                      ///< Index of parent part (-1 = root)
    PhysicsBodyDesc bodyDesc;                                  ///< Body configuration for this limb
    ConstraintType constraintType = ConstraintType::ConeTwist; ///< Joint type connecting to parent
    XMFLOAT3 constraintPivot = {0, 0, 0};                      ///< Joint pivot in parent's local space
    XMFLOAT3 constraintAxis = {0, 1, 0};                       ///< Primary joint axis
    float swingLimit1 = 0.5f;                                  ///< Swing limit 1 (radians)
    float swingLimit2 = 0.5f;                                  ///< Swing limit 2 (radians)
    float twistLimit = 0.3f;                                   ///< Twist limit (radians)
};

/**
 * @brief Ragdoll descriptor — a collection of connected parts.
 */
struct RagdollDesc
{
    std::vector<RagdollPartDesc> parts;
};

/**
 * @brief Raycast hit information
 */
struct RaycastHit
{
    bool hasHit = false;               ///< Whether ray hit something
    XMFLOAT3 point = {0, 0, 0};        ///< Hit point in world space
    XMFLOAT3 normal = {0, 1, 0};       ///< Hit surface normal
    float distance = 0.0f;             ///< Distance from ray origin
    class PhysicsBody* body = nullptr; ///< Hit physics body
    void* userData = nullptr;          ///< User data from hit body (legacy, prefer entityId)
    uint32_t entityId = 0;             ///< ECS entity ID of the hit body (0 = no entity)
};

/**
 * @brief Collision contact information
 */
struct ContactInfo
{
    class PhysicsBody* bodyA = nullptr; ///< First colliding body
    class PhysicsBody* bodyB = nullptr; ///< Second colliding body
    XMFLOAT3 contactPoint = {0, 0, 0};  ///< Contact point
    XMFLOAT3 contactNormal = {0, 1, 0}; ///< Contact normal
    float penetrationDepth = 0.0f;      ///< Penetration depth
    float appliedImpulse = 0.0f;        ///< Applied impulse
    uint32_t entityIdA = 0;             ///< ECS entity ID of bodyA (0 = no entity)
    uint32_t entityIdB = 0;             ///< ECS entity ID of bodyB (0 = no entity)
};

// =============================================================================
// Utility functions
// =============================================================================

std::string PhysicsBodyTypeToString(PhysicsBodyType type);
PhysicsBodyType StringToPhysicsBodyType(const std::string& str);
std::string CollisionShapeTypeToString(CollisionShapeType type);
CollisionShapeType StringToCollisionShapeType(const std::string& str);
std::string ConstraintTypeToString(ConstraintType type);
ConstraintType StringToConstraintType(const std::string& str);
