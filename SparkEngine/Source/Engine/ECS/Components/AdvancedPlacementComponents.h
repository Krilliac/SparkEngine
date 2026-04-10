/**
 * @file AdvancedPlacementComponents.h
 * @brief ECS components for advanced editor-placeable objects
 * @author Spark Engine Team
 * @date 2026
 *
 * Components for physics, rendering, navigation, and gameplay objects
 * commonly found in game engines (Unity, Unreal, Godot, O3DE, Flax).
 * Each bridges to an existing engine system or a newly created one.
 */

#pragma once
#include "../../../Core/Platform.h"
#include "../../../Utils/Assert.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// =============================================================================
// AudioListenerComponent — Spatial audio receiver position
// =============================================================================

/**
 * @brief Marks an entity as the audio listener for spatial sound.
 *
 * Bridges to AudioMixer. Only one listener should be active at a time.
 * The audio system uses the entity's Transform for 3D sound calculations.
 */
struct AudioListenerComponent
{
    bool isActive = true;     ///< Whether this is the current active listener.
    float volumeScale = 1.0f; ///< Global volume multiplier for this listener.
};

// =============================================================================
// CharacterControllerComponent — Jolt character movement
// =============================================================================

/**
 * @brief Character controller for walking, sliding, and stepping.
 *
 * Bridges to CharacterController.h (Jolt CharacterVirtual wrapper).
 * Handles ground detection, slope limits, step heights, and gravity.
 */
struct CharacterControllerComponent
{
    float height = 1.8f;              ///< Capsule height (meters).
    float radius = 0.3f;              ///< Capsule radius (meters).
    float stepHeight = 0.35f;         ///< Max step height for auto-stepping.
    float slopeLimit = 50.0f;         ///< Max walkable slope angle (degrees).
    float skinWidth = 0.08f;          ///< Collision skin width.
    float gravity = -9.81f;           ///< Gravity acceleration.
    float moveSpeed = 5.0f;           ///< Default movement speed (m/s).
    float jumpForce = 5.0f;           ///< Jump impulse strength.
    int groundState = 0;              ///< Runtime: 0=OnGround, 1=Steep, 2=NotSupported, 3=InAir.
    uint32_t runtimeControllerId = 0; ///< Runtime handle.

    bool Validate() const
    {
        ASSERT_MSG(height > 0.0f, "CharacterController height must be positive");
        ASSERT_MSG(radius > 0.0f, "CharacterController radius must be positive");
        return height > 0.0f && radius > 0.0f;
    }
};

// =============================================================================
// NavRegionComponent — NavMesh baking region bounds
// =============================================================================

/**
 * @brief Defines an AABB region where NavMesh should be generated.
 *
 * Bridges to NavMeshBuilder. Multiple regions can exist; the builder
 * generates navigation mesh within each region's bounds.
 */
struct NavRegionComponent
{
    DirectX::XMFLOAT3 halfExtents{25.0f, 10.0f, 25.0f}; ///< Region half-extents.
    float agentRadius = 0.3f;                           ///< Agent radius for NavMesh generation.
    float agentHeight = 1.8f;                           ///< Agent height for NavMesh generation.
    float maxSlope = 45.0f;                             ///< Max walkable slope (degrees).
    float cellSize = 0.3f;                              ///< NavMesh cell size (smaller = more detail).
    bool autoRebuild = true;                            ///< Rebuild NavMesh when geometry changes.
};

// =============================================================================
// NavLinkComponent — Off-mesh navigation link
// =============================================================================

/**
 * @brief Defines an off-mesh link between two NavMesh positions.
 *
 * Bridges to NavMeshLinkSystem. Allows AI agents to traverse gaps,
 * climb ladders, or teleport between disconnected NavMesh regions.
 */
struct NavLinkComponent
{
    enum class TraversalType : uint8_t
    {
        Walk,
        Jump,
        Drop,
        Climb,
        Teleport
    };

    DirectX::XMFLOAT3 endOffset{0.0f, 0.0f, 5.0f}; ///< End position relative to entity.
    float radius = 0.5f;                           ///< Agent radius tolerance.
    TraversalType traversalType = TraversalType::Walk;
    float traversalCost = 1.0f; ///< Pathfinding cost multiplier.
    bool bidirectional = true;  ///< Traversable in both directions.
    bool enabled = true;        ///< Active state.
    std::string animationName;  ///< Animation during traversal.
    uint32_t runtimeLinkId = 0; ///< ID from NavMeshLinkSystem.
};

// =============================================================================
// SkyboxComponent — Sky/atmosphere rendering
// =============================================================================

/**
 * @brief Configures sky rendering for the scene.
 *
 * Bridges to SkyAtmosphereSystem. Supports procedural Preetham sky,
 * cubemap skybox, gradient sky, or solid color.
 */
struct SkyboxComponent
{
    enum class SkyMode : uint8_t
    {
        Procedural, ///< Preetham/Perez analytical sky model.
        Cubemap,    ///< Six-face cubemap texture.
        Gradient,   ///< Two-color gradient.
        SolidColor  ///< Single color.
    };

    SkyMode mode = SkyMode::Procedural;
    std::string cubemapPath;                               ///< Cubemap texture path (Cubemap mode).
    DirectX::XMFLOAT4 topColor{0.2f, 0.4f, 0.8f, 1.0f};    ///< Top/zenith color.
    DirectX::XMFLOAT4 bottomColor{0.8f, 0.9f, 1.0f, 1.0f}; ///< Bottom/horizon color.
    float turbidity = 2.0f;                                ///< Atmospheric turbidity (Procedural).
    float sunSize = 1.0f;                                  ///< Sun disc size multiplier.
    float exposure = 1.0f;                                 ///< Sky exposure.
    float rotation = 0.0f;                                 ///< Cubemap Y-axis rotation (degrees).
};

// =============================================================================
// ConstantForceComponent — Persistent force on a rigidbody
// =============================================================================

/**
 * @brief Applies a constant force and/or torque to a rigidbody every frame.
 *
 * Useful for rockets, wind-driven objects, conveyor belts, and floating
 * objects. The force is applied in world or local space.
 */
struct ConstantForceComponent
{
    DirectX::XMFLOAT3 force{0.0f, 0.0f, 0.0f};  ///< Force vector (Newtons).
    DirectX::XMFLOAT3 torque{0.0f, 0.0f, 0.0f}; ///< Torque vector (Newton-meters).
    bool relativeForce = false;                 ///< Apply force in local space.
    bool relativeTorque = false;                ///< Apply torque in local space.
    bool enabled = true;                        ///< Active state.
};

// =============================================================================
// ForceRegionComponent — Physics volume applying forces to bodies
// =============================================================================

/**
 * @brief A volume that applies physics forces to bodies inside it.
 *
 * Supports directional force (conveyor), point force (explosion/attraction),
 * and buoyancy. Bodies within the AABB bounds receive forces each frame.
 */
struct ForceRegionComponent
{
    enum class ForceType : uint8_t
    {
        Directional, ///< Constant direction (conveyor belt, wind tunnel).
        Point,       ///< Radial from center (explosion, black hole).
        Buoyancy     ///< Upward buoyancy force opposing gravity.
    };

    ForceType forceType = ForceType::Directional;
    DirectX::XMFLOAT3 halfExtents{5.0f, 5.0f, 5.0f};    ///< Volume bounds.
    DirectX::XMFLOAT3 forceDirection{0.0f, 1.0f, 0.0f}; ///< Force direction (Directional).
    float forceMagnitude = 10.0f;                       ///< Force strength.
    float damping = 0.0f;                               ///< Linear damping inside volume.
    bool enabled = true;                                ///< Active state.

    bool Validate() const
    {
        ASSERT_MSG(forceMagnitude >= 0.0f, "ForceRegion magnitude must be non-negative");
        return forceMagnitude >= 0.0f;
    }
};

// =============================================================================
// RagdollComponent — Ragdoll physics on skeletal mesh
// =============================================================================

/**
 * @brief Enables ragdoll physics on a skeletal mesh entity.
 *
 * Bridges to RagdollSystem. Supports three modes: fully animated,
 * blended (partial physics), and full ragdoll (all physics).
 */
struct RagdollComponent
{
    enum class Mode : uint8_t
    {
        Animated, ///< Animation drives all bones.
        Blended,  ///< Partial physics (e.g. dead limb + animated torso).
        Physics   ///< Full ragdoll, all bones physics-driven.
    };

    Mode mode = Mode::Animated;
    std::string definitionName;  ///< Ragdoll definition asset name.
    float blendWeight = 0.5f;    ///< Blend factor between animation and physics [0,1].
    float jointStiffness = 1.0f; ///< Joint constraint stiffness.
    float linearDamping = 0.1f;  ///< Linear velocity damping.
    float angularDamping = 0.5f; ///< Angular velocity damping.
    bool selfCollision = false;  ///< Bones collide with each other.
    uint32_t runtimeId = 0;      ///< Runtime ragdoll instance handle.

    bool Validate() const
    {
        ASSERT_MSG(blendWeight >= 0.0f && blendWeight <= 1.0f, "Ragdoll blendWeight must be in [0,1]");
        return blendWeight >= 0.0f && blendWeight <= 1.0f;
    }
};

// =============================================================================
// SoftBodyComponent — Cloth/soft body simulation
// =============================================================================

/**
 * @brief Enables soft body (cloth) simulation on an entity's mesh.
 *
 * Bridges to SoftBodyPhysics (XPBD solver). Used for capes, flags,
 * banners, curtains, and deformable objects.
 */
struct SoftBodyComponent
{
    float mass = 1.0f;          ///< Total soft body mass (kg).
    float stiffness = 0.8f;     ///< Constraint stiffness [0,1].
    float damping = 0.02f;      ///< Velocity damping.
    float windInfluence = 1.0f; ///< Wind force multiplier.
    float gravityScale = 1.0f;  ///< Gravity multiplier.
    int solverIterations = 4;   ///< XPBD solver iterations per frame.
    bool selfCollision = false; ///< Vertices collide with each other.
    bool twoSided = false;      ///< Render both faces of the mesh.
    uint32_t runtimeId = 0;     ///< Runtime soft body handle.

    bool Validate() const
    {
        ASSERT_MSG(mass > 0.0f, "SoftBody mass must be positive");
        ASSERT_MSG(solverIterations >= 1, "SoftBody solverIterations must be >= 1");
        return mass > 0.0f && solverIterations >= 1;
    }
};

// =============================================================================
// VehicleComponent — Vehicle physics (wheeled/tracked/motorcycle)
// =============================================================================

/**
 * @brief Configures vehicle physics simulation on an entity.
 *
 * Bridges to VehiclePhysics.h (Jolt VehicleConstraint wrapper).
 * Supports wheeled, tracked, and motorcycle vehicle types.
 */
struct VehicleComponent
{
    enum class VehicleType : uint8_t
    {
        Wheeled,
        Tracked,
        Motorcycle
    };

    VehicleType vehicleType = VehicleType::Wheeled;
    int wheelCount = 4;                   ///< Number of wheels.
    float mass = 1500.0f;                 ///< Vehicle mass (kg).
    float maxEngineTorque = 500.0f;       ///< Peak engine torque (Nm).
    float maxSteerAngle = 35.0f;          ///< Maximum steering angle (degrees).
    float maxBrakeForce = 5000.0f;        ///< Maximum brake force (N).
    float suspensionLength = 0.3f;        ///< Suspension travel distance (meters).
    float suspensionStiffness = 30000.0f; ///< Suspension spring stiffness.
    float suspensionDamping = 4000.0f;    ///< Suspension damping.
    int gearCount = 6;                    ///< Number of forward gears.
    bool antiRollBar = true;              ///< Enable anti-roll bar stabilization.
    uint32_t runtimeId = 0;               ///< Runtime vehicle handle.

    bool Validate() const
    {
        ASSERT_MSG(mass > 0.0f, "Vehicle mass must be positive");
        ASSERT_MSG(wheelCount > 0, "Vehicle wheelCount must be positive");
        return mass > 0.0f && wheelCount > 0;
    }
};

// =============================================================================
// BuoyancyVolumeComponent — Water buoyancy region
// =============================================================================

/**
 * @brief Defines a volume where rigid bodies experience buoyancy.
 *
 * Bridges to PhysicsSystem::ApplyBuoyancy(). Bodies overlapping
 * the volume receive upward force opposing gravity.
 */
struct BuoyancyVolumeComponent
{
    DirectX::XMFLOAT3 halfExtents{10.0f, 5.0f, 10.0f}; ///< Volume bounds.
    float waterDensity = 1000.0f;                      ///< Water density (kg/m^3).
    float linearDrag = 0.3f;                           ///< Linear drag on submerged bodies.
    float angularDrag = 0.05f;                         ///< Angular drag on submerged bodies.
    float flowSpeed = 0.0f;                            ///< Water current speed (m/s).
    DirectX::XMFLOAT3 flowDirection{1.0f, 0.0f, 0.0f}; ///< Current direction.
    bool enabled = true;                               ///< Active state.
};

// =============================================================================
// SpringArmComponent — Camera collision avoidance
// =============================================================================

/**
 * @brief Extends a spring arm from an entity to prevent camera clipping.
 *
 * Bridges to SpringArm.h. Performs a sphere cast along the arm direction
 * and shortens the arm if geometry is hit, keeping the camera visible.
 */
struct SpringArmComponent
{
    float targetLength = 5.0f;   ///< Desired arm length (meters).
    float probeRadius = 0.2f;    ///< Sphere cast radius.
    float smoothSpeed = 10.0f;   ///< Interpolation speed.
    float minLength = 0.5f;      ///< Minimum arm length.
    bool doCollisionTest = true; ///< Enable collision testing.
    float currentLength = 5.0f;  ///< Runtime: current arm length.
    bool isColliding = false;    ///< Runtime: hitting geometry.

    bool Validate() const
    {
        ASSERT_MSG(targetLength > 0.0f, "SpringArm targetLength must be positive");
        ASSERT_MSG(minLength > 0.0f, "SpringArm minLength must be positive");
        return targetLength > 0.0f && minLength > 0.0f;
    }
};

// =============================================================================
// LineRendererComponent — World-space polyline rendering
// =============================================================================

/**
 * @brief Renders a polyline from world-space points as a camera-facing strip.
 *
 * Bridges to LineTrailRenderer.h. Used for laser beams, ropes, connections,
 * selection indicators, and other line-based visuals.
 */
struct LineRendererComponent
{
    std::vector<DirectX::XMFLOAT3> points;    ///< Polyline control points.
    float startWidth = 0.1f;                  ///< Width at the first point.
    float endWidth = 0.1f;                    ///< Width at the last point.
    DirectX::XMFLOAT4 startColor{1, 1, 1, 1}; ///< Color at start.
    DirectX::XMFLOAT4 endColor{1, 1, 1, 1};   ///< Color at end.
    std::string materialPath;                 ///< Material/texture.
    bool useWorldSpace = true;                ///< Points in world or local space.
    bool loop = false;                        ///< Connect last to first.
    int sortingLayer = 0;                     ///< Render sort order.
};

// =============================================================================
// TrailRendererComponent — Trail effect behind moving objects
// =============================================================================

/**
 * @brief Renders a fading trail behind a moving entity.
 *
 * Bridges to LineTrailRenderer.h. Used for sword swipes, projectile trails,
 * vehicle skid marks, and character movement effects.
 */
struct TrailRendererComponent
{
    float lifetime = 2.0f;                    ///< Trail segment persistence (seconds).
    float minVertexDistance = 0.1f;           ///< Min distance to add a trail point.
    float startWidth = 0.5f;                  ///< Width at newest point.
    float endWidth = 0.0f;                    ///< Width at oldest point.
    DirectX::XMFLOAT4 startColor{1, 1, 1, 1}; ///< Color at newest.
    DirectX::XMFLOAT4 endColor{1, 1, 1, 0};   ///< Color at oldest (fades).
    std::string materialPath;                 ///< Material/texture.
    bool emitting = true;                     ///< Add new points.
    int sortingLayer = 0;                     ///< Render sort order.
};

// =============================================================================
// Text3DComponent — World-space 3D text rendering
// =============================================================================

/**
 * @brief Renders text in 3D world space.
 *
 * Bridges to MSDFTextRenderer. Used for NPC name plates, signage,
 * damage numbers, and in-world labels.
 */
struct Text3DComponent
{
    std::string text;                                ///< Display text.
    std::string fontPath;                            ///< Font asset path.
    float fontSize = 1.0f;                           ///< Text size in world units.
    DirectX::XMFLOAT4 color{1.0f, 1.0f, 1.0f, 1.0f}; ///< Text color.
    bool faceCamera = true;                          ///< Billboard the text toward camera.
    bool castShadows = false;                        ///< Text casts shadows.

    enum class Alignment : uint8_t
    {
        Left,
        Center,
        Right
    };

    Alignment alignment = Alignment::Center;
    float maxWidth = 0.0f; ///< Max width before wrapping (0=no wrap).
    int sortingLayer = 0;  ///< Render sort order.
};

// =============================================================================
// FoliageVolumeComponent — Vegetation scatter region
// =============================================================================

/**
 * @brief Defines a region where vegetation is procedurally scattered.
 *
 * Bridges to FoliageSystem. Species configuration determines what
 * meshes are instanced within the volume bounds.
 */
struct FoliageVolumeComponent
{
    DirectX::XMFLOAT3 halfExtents{50.0f, 50.0f, 50.0f}; ///< Volume half-extents.
    int seed = 0;                                       ///< Random seed for reproducibility.
    float densityScale = 1.0f;                          ///< Global density multiplier.
    float minSlopeAngle = 0.0f;                         ///< Min terrain slope for placement.
    float maxSlopeAngle = 45.0f;                        ///< Max terrain slope for placement.
    float minAltitude = -1000.0f;                       ///< Min placement altitude.
    float maxAltitude = 1000.0f;                        ///< Max placement altitude.
    bool alignToSurface = true;                         ///< Align instances to terrain normal.
    bool castShadows = true;                            ///< Instances cast shadows.
    float cullDistance = 100.0f;                        ///< Distance to cull instances.
    bool enabled = true;                                ///< Active state.
    uint32_t runtimeVolumeId = 0;                       ///< Runtime handle from FoliageManager.
    std::vector<std::string> speciesNames; ///< Species the volume scatters (lookup by name in FoliageManager).
};
