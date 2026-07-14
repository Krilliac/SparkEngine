/**
 * @file PhysicsSystem.h
 * @brief PhysicsSystem class and umbrella include for the physics subsystem
 * @author Spark Engine Team
 * @date 2025
 *
 * This is the umbrella header for the physics subsystem. It includes:
 * - PhysicsTypes.h — Enums, descriptors, collision types, materials
 * - PhysicsBody.h  — PhysicsBody and PhysicsConstraint wrapper classes
 * - PhysicsSystem  — Central manager for the Jolt Physics world (below)
 *
 * @note PhysicsSystem is **not thread-safe**. Call all public methods from the
 *       main game thread. The physics simulation itself runs on the calling thread
 *       inside Update().
 */

#pragma once

// Subsystem headers
#include "PhysicsBody.h"
#include "CharacterController.h"
#include "VehiclePhysics.h"
#include "RagdollSystem.h"
#include "SoftBodyPhysics.h"

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace Spark
{
    class EventBus;
}
#include <functional>
#include <mutex>

// Forward declarations for Jolt Physics
namespace JPH
{
    class PhysicsSystem;
    class TempAllocator;
    class JobSystem;
    class BroadPhaseLayerInterface;
    class ObjectVsBroadPhaseLayerFilter;
    class ObjectLayerPairFilter;
    class Shape;
    class ContactListener;
    class BodyActivationListener;
    class GroupFilterTable;
    class MutableCompoundShape;
} // namespace JPH

class PhysicsDebugRenderer;

/**
 * @class PhysicsSystem
 * @brief Central manager for the Jolt Physics world.
 *
 * PhysicsSystem owns and drives the Jolt `JPH::PhysicsSystem`. It handles
 * body and constraint lifecycle, advances the simulation each frame, routes
 * collision/trigger callbacks, and exposes spatial query APIs (raycast, overlap).
 *
 * ### Lifecycle
 * 1. Call `Initialize()` once at engine startup (creates the Jolt world).
 * 2. Call `Update(deltaTime)` each frame (advances physics by `deltaTime`).
 * 3. Call `Shutdown()` before the engine exits (destroys all bodies and the world).
 *
 * ### Coordinate system
 * All public methods accept and return DirectX Math types (XMFLOAT3, XMMATRIX).
 * Conversion to/from Jolt's `JPH::Vec3`/`JPH::RMat44` is handled internally.
 *
 * ### Gravity
 * Default gravity is `{0, -9.81, 0}` m/s² (Earth). Change it via `SetGravity()`.
 * A zero-gravity world is useful for space or underwater environments.
 *
 * ### Stepping contract (module-driven)
 * Physics stepping is **module-driven**: the engine core never advances the
 * simulation on its own. Exactly ONE owner (one game module, or the exe) may
 * advance the world per frame, by calling either `Update(deltaTime)` OR
 * `StepFixed(stepCount, alpha)` — never both, and never from two modules in
 * the same process. Double-stepping runs the simulation at 2x speed and breaks
 * determinism. As of 2026-07, the sole caller is SparkGameFPS
 * (`Game.cpp` -> `physics->Update(dt)`); a module that wants deterministic
 * fixed ticks instead should be the sole owner and drive `StepFixed()` from
 * `Spark::FixedTimestepAccumulator` (Core/FixedTimestepAccumulator.h):
 * @code
 *   auto& acc = Spark::FixedTimestepAccumulator::GetInstance();
 *   acc.Advance(frameDt); // once per frame, by the frame owner
 *   physics->StepFixed(acc.GetFixedStepCount(), acc.GetInterpolationAlpha());
 * @endcode
 */
// Thread safety: Main thread only. All physics simulation, raycasting,
// and collision shape operations must be called from the main thread.
class PhysicsSystem
{
  public:
    /**
     * @brief Physics system metrics
     */
    struct PhysicsMetrics
    {
        uint32_t activeRigidBodies; ///< Number of active rigid bodies
        uint32_t totalRigidBodies;  ///< Total number of rigid bodies
        uint32_t activeConstraints; ///< Number of active constraints
        uint32_t collisionPairs;    ///< Active collision pairs
        uint32_t broadphaseProxies; ///< Broadphase proxy count
        float simulationTime;       ///< Physics simulation time (ms)
        float collisionTime;        ///< Collision detection time (ms)
        uint32_t substeps;          ///< Number of substeps per frame
        float timeStep;             ///< Physics time step
        bool debugDrawEnabled;      ///< Debug drawing enabled
        uint32_t raycastCount;      ///< Raycasts performed this frame
    };

    PhysicsSystem();
    ~PhysicsSystem();

    /**
     * @brief Initialize the Jolt physics world and all supporting structures.
     * @return  `S_OK` on success; an `HRESULT` failure code on error.
     */
    HRESULT Initialize();

    /**
     * @brief Destroy all physics bodies, constraints, and the physics world.
     */
    void Shutdown();

    /**
     * @brief Advance the physics simulation by `deltaTime` seconds.
     *
     * Variable-rate entry point: internally accumulates `deltaTime` into fixed
     * ticks of `GetTimeStep()` when interpolation is enabled. See the class-level
     * "Stepping contract" — only ONE owner per process may call Update()/StepFixed()
     * per frame; a second caller double-steps the world.
     *
     * @param deltaTime  Time elapsed since the last frame in seconds.
     */
    void Update(float deltaTime);

    /**
     * @brief Advance the simulation by an exact number of fixed ticks (deterministic).
     *
     * Runs `stepCount` ticks of `GetTimeStep()` seconds each — no internal
     * accumulator, so identical tick sequences produce identical results
     * (combine with SetDeterministicSimulation(true) for lockstep networking).
     * Intended to be driven from `Spark::FixedTimestepAccumulator`:
     * @code
     *   physics->StepFixed(acc.GetFixedStepCount(), acc.GetInterpolationAlpha());
     * @endcode
     *
     * Mutually exclusive with Update(): the single stepping owner calls one or
     * the other each frame, never both (see class-level "Stepping contract").
     * Calling with `stepCount == 0` is valid — it only refreshes the
     * interpolation alpha, so call it every frame even on frames with no tick.
     *
     * @param stepCount           Fixed ticks to run this frame (clamped to GetMaxSubsteps()).
     * @param interpolationAlpha  Render interpolation alpha in [0, 1] (accumulator remainder / timestep).
     * @return Number of ticks actually simulated (0 when paused or Jolt is unavailable).
     */
    uint32_t StepFixed(uint32_t stepCount, float interpolationAlpha = 1.0f);

    /** @brief Optimize the Jolt broadphase after bulk static-body creation (level load). No-op without Jolt. */
    void OptimizeBroadPhase();

    /**
     * @brief Register Jolt's per-image runtime (allocator, Factory, dispatch tables).
     *
     * Every module DLL statically links its own Jolt copy with its own globals.
     * Called automatically for the calling image by the SparkModuleInjectEngineContext
     * hook (SDK ModuleDllMain.h) and by Initialize(); idempotent and cheap.
     * Without it, the first module-side shape creation or collision query
     * null-calls through the module image's unregistered allocator/dispatch tables.
     */
    static void EnsureImageRuntime();

    // =========================================================================
    // World settings
    // =========================================================================

    void SetGravity(const XMFLOAT3& gravity);
    XMFLOAT3 GetGravity() const;
    void SetTimeStep(float timeStep) { m_timeStep = timeStep; }
    float GetTimeStep() const { return m_timeStep; }
    void SetMaxSubsteps(int maxSubsteps) { m_maxSubsteps = maxSubsteps; }
    int GetMaxSubsteps() const { return m_maxSubsteps; }

    /**
     * @brief Enable deterministic simulation mode.
     *
     * When enabled, Jolt processes bodies and constraints in a deterministic order
     * so that identical inputs produce bit-identical outputs across runs and machines.
     * Required for lockstep networking and replay systems.
     *
     * Performance impact: ~10-15% slower due to sorting overhead.
     *
     * @param enabled  true to enable deterministic mode.
     */
    void SetDeterministicSimulation(bool enabled);

    /** @brief Check if deterministic simulation is enabled. */
    bool IsDeterministicSimulation() const { return m_deterministicMode; }

    // =========================================================================
    // Interpolation
    // =========================================================================

    float GetInterpolationAlpha() const { return m_interpolationAlpha; }
    void SetInterpolationEnabled(bool enabled) { m_interpolationEnabled = enabled; }
    bool IsInterpolationEnabled() const { return m_interpolationEnabled; }

    // =========================================================================
    // Body management
    // =========================================================================

    std::shared_ptr<PhysicsBody> CreateBody(const PhysicsBodyDesc& desc);
    void RemoveBody(std::shared_ptr<PhysicsBody> body);
    void RemoveAllBodies();
    const std::vector<std::shared_ptr<PhysicsBody>>& GetBodies() const { return m_bodies; }

    // =========================================================================
    // Constraint management
    // =========================================================================

    std::shared_ptr<PhysicsConstraint> CreateHingeConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                             std::shared_ptr<PhysicsBody> bodyB, const XMFLOAT3& pivotA,
                                                             const XMFLOAT3& pivotB, const XMFLOAT3& axisA,
                                                             const XMFLOAT3& axisB);

    std::shared_ptr<PhysicsConstraint> CreateSliderConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                              std::shared_ptr<PhysicsBody> bodyB,
                                                              const XMMATRIX& frameA, const XMMATRIX& frameB);

    std::shared_ptr<PhysicsConstraint> CreatePoint2PointConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                   std::shared_ptr<PhysicsBody> bodyB,
                                                                   const XMFLOAT3& pivotA, const XMFLOAT3& pivotB);

    std::shared_ptr<PhysicsConstraint> CreateConeTwistConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                 std::shared_ptr<PhysicsBody> bodyB,
                                                                 const XMMATRIX& frameA, const XMMATRIX& frameB,
                                                                 float swingSpan1 = 0.5f, float swingSpan2 = 0.5f,
                                                                 float twistSpan = 0.5f);

    std::shared_ptr<PhysicsConstraint> CreateFixedConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                             std::shared_ptr<PhysicsBody> bodyB, const XMMATRIX& frameA,
                                                             const XMMATRIX& frameB);

    /** @brief Create a distance constraint maintaining distance between two points. */
    std::shared_ptr<PhysicsConstraint> CreateDistanceConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                std::shared_ptr<PhysicsBody> bodyB,
                                                                const XMFLOAT3& pivotA, const XMFLOAT3& pivotB,
                                                                float minDistance = -1.0f, float maxDistance = -1.0f);

    /** @brief Create a cone constraint limiting rotation to a cone region. */
    std::shared_ptr<PhysicsConstraint> CreateConeConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                            std::shared_ptr<PhysicsBody> bodyB, const XMFLOAT3& pivot,
                                                            const XMFLOAT3& twistAxis, float halfConeAngle = 0.5f);

    /** @brief Create a 6-DOF constraint with per-axis freedom control. */
    std::shared_ptr<PhysicsConstraint> CreateSixDOFConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                              std::shared_ptr<PhysicsBody> bodyB,
                                                              const XMMATRIX& frameA, const XMMATRIX& frameB);

    /** @brief Create a pulley constraint (rope/cable between two bodies through fixed points). */
    std::shared_ptr<PhysicsConstraint> CreatePulleyConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                              std::shared_ptr<PhysicsBody> bodyB,
                                                              const XMFLOAT3& fixedPointA, const XMFLOAT3& fixedPointB,
                                                              const XMFLOAT3& bodyPointA, const XMFLOAT3& bodyPointB,
                                                              float ratio = 1.0f);

    /**
     * @brief Create a gear constraint coupling rotation of two hinge constraints.
     * @param bodyA     First body (must have a hinge constraint).
     * @param bodyB     Second body (must have a hinge constraint).
     * @param hingeA    The hinge constraint on bodyA.
     * @param hingeB    The hinge constraint on bodyB.
     * @param ratio     Gear ratio (bodyB rotates ratio times faster than bodyA).
     */
    std::shared_ptr<PhysicsConstraint> CreateGearConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                            std::shared_ptr<PhysicsBody> bodyB,
                                                            std::shared_ptr<PhysicsConstraint> hingeA,
                                                            std::shared_ptr<PhysicsConstraint> hingeB,
                                                            float ratio = 1.0f);

    /**
     * @brief Create a rack-and-pinion constraint coupling hinge rotation to slider translation.
     * @param bodyA     Rack body (with slider constraint).
     * @param bodyB     Pinion body (with hinge constraint).
     * @param slider    The slider constraint on bodyA.
     * @param hinge     The hinge constraint on bodyB.
     * @param ratio     Ratio of slider movement per hinge rotation (m/rad).
     */
    std::shared_ptr<PhysicsConstraint> CreateRackAndPinionConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                     std::shared_ptr<PhysicsBody> bodyB,
                                                                     std::shared_ptr<PhysicsConstraint> slider,
                                                                     std::shared_ptr<PhysicsConstraint> hinge,
                                                                     float ratio = 1.0f);

    /**
     * @brief Create a path constraint — body follows a spline path.
     * @param body      Body to constrain to the path.
     * @param pathPoints Control points for a Hermite spline path (world space).
     * @param pathTangents Tangent vectors at each control point.
     */
    std::shared_ptr<PhysicsConstraint> CreatePathConstraint(std::shared_ptr<PhysicsBody> body,
                                                            const std::vector<XMFLOAT3>& pathPoints,
                                                            const std::vector<XMFLOAT3>& pathTangents);

    void RemoveConstraint(std::shared_ptr<PhysicsConstraint> constraint);

    // =========================================================================
    // Constraint motors
    // =========================================================================

    /**
     * @brief Set a velocity motor on a hinge constraint.
     * @param constraint   Hinge constraint to motorize.
     * @param targetVelocity Target angular velocity (rad/s).
     * @param maxTorque    Maximum torque the motor can apply (N*m).
     */
    void SetHingeMotorVelocity(std::shared_ptr<PhysicsConstraint> constraint, float targetVelocity, float maxTorque);

    /**
     * @brief Set a position (servo) motor on a hinge constraint.
     * @param constraint   Hinge constraint to motorize.
     * @param targetAngle  Target angle (radians).
     * @param maxTorque    Maximum torque the motor can apply (N*m).
     */
    void SetHingeMotorPosition(std::shared_ptr<PhysicsConstraint> constraint, float targetAngle, float maxTorque);

    /**
     * @brief Set a velocity motor on a slider constraint.
     * @param constraint       Slider constraint to motorize.
     * @param targetVelocity   Target linear velocity (m/s).
     * @param maxForce         Maximum force the motor can apply (N).
     */
    void SetSliderMotorVelocity(std::shared_ptr<PhysicsConstraint> constraint, float targetVelocity, float maxForce);

    /**
     * @brief Set a position (servo) motor on a slider constraint.
     * @param constraint       Slider constraint to motorize.
     * @param targetPosition   Target position along slider axis (m).
     * @param maxForce         Maximum force the motor can apply (N).
     */
    void SetSliderMotorPosition(std::shared_ptr<PhysicsConstraint> constraint, float targetPosition, float maxForce);

    /** @brief Disable any active motor on a constraint. */
    void DisableConstraintMotor(std::shared_ptr<PhysicsConstraint> constraint);

    // =========================================================================
    // Spatial queries
    // =========================================================================
    //
    // Hit-result contract (all Raycast*/SphereCast/BoxCast/CapsuleCast variants):
    //   - `RaycastHit::point`    = contact point ON the hit surface (world space)
    //   - `RaycastHit::normal`   = outward unit surface normal at the contact
    //   - `RaycastHit::distance` = swept distance from `origin`/`from` (for shape
    //     casts this is how far the shape's CENTER travelled, not point-to-point)
    //   - `RaycastHit::body`     = PhysicsBody wrapper, or nullptr for raw Jolt bodies
    // Shape casts sweep the shape's center along `from`->`to`. Prefer the
    // *Filtered overloads below for gameplay traces so triggers/debris don't
    // block hits. All queries are main-thread only (see class-level note).
    // =========================================================================

    RaycastHit Raycast(const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance = 1000.0f);
    std::vector<RaycastHit> RaycastAll(const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance = 1000.0f);
    bool SphereOverlap(const XMFLOAT3& center, float radius, std::vector<PhysicsBody*>& results);
    bool BoxOverlap(const XMFLOAT3& center, const XMFLOAT3& halfExtents, std::vector<PhysicsBody*>& results);
    RaycastHit SphereCast(float radius, const XMFLOAT3& from, const XMFLOAT3& to);
    RaycastHit BoxCast(const XMFLOAT3& halfExtents, const XMFLOAT3& from, const XMFLOAT3& to);
    RaycastHit CapsuleCast(float radius, float height, const XMFLOAT3& from, const XMFLOAT3& to);

    // -------------------------------------------------------------------------
    // Filtered spatial queries (gameplay-facing)
    //
    // `layerMask` is tested against each candidate body's collision group
    // (`(body->GetCollisionGroup() & layerMask) != 0`) — use the named bits and
    // presets in CollisionLayers (PhysicsTypes.h), e.g. CollisionLayers::HitscanMask.
    // `ignoreBody` excludes one body (typically the query instigator: the
    // shooter's own hull, the sweeping vehicle itself). Bodies that have no
    // PhysicsBody wrapper (raw Jolt-internal bodies) always pass the layer
    // filter, so world geometry is never silently skipped.
    // -------------------------------------------------------------------------

    /** @brief Closest-hit raycast restricted to `layerMask`, optionally ignoring one body. */
    RaycastHit RaycastFiltered(const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance, uint16_t layerMask,
                               const PhysicsBody* ignoreBody = nullptr);

    /** @brief All-hits raycast (sorted near-to-far) restricted to `layerMask`. */
    std::vector<RaycastHit> RaycastAllFiltered(const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance,
                                               uint16_t layerMask, const PhysicsBody* ignoreBody = nullptr);

    /** @brief Sphere sweep from `from` to `to` restricted to `layerMask`. */
    RaycastHit SphereCastFiltered(float radius, const XMFLOAT3& from, const XMFLOAT3& to, uint16_t layerMask,
                                  const PhysicsBody* ignoreBody = nullptr);

    /** @brief Box sweep from `from` to `to` restricted to `layerMask`. */
    RaycastHit BoxCastFiltered(const XMFLOAT3& halfExtents, const XMFLOAT3& from, const XMFLOAT3& to,
                               uint16_t layerMask, const PhysicsBody* ignoreBody = nullptr);

    /** @brief Capsule sweep from `from` to `to` restricted to `layerMask`. */
    RaycastHit CapsuleCastFiltered(float radius, float height, const XMFLOAT3& from, const XMFLOAT3& to,
                                   uint16_t layerMask, const PhysicsBody* ignoreBody = nullptr);

    /** @brief Sphere overlap restricted to `layerMask`. Returns true if any body matched. */
    bool SphereOverlapFiltered(const XMFLOAT3& center, float radius, uint16_t layerMask,
                               std::vector<PhysicsBody*>& results, const PhysicsBody* ignoreBody = nullptr);

    /** @brief Box overlap restricted to `layerMask`. Returns true if any body matched. */
    bool BoxOverlapFiltered(const XMFLOAT3& center, const XMFLOAT3& halfExtents, uint16_t layerMask,
                            std::vector<PhysicsBody*>& results, const PhysicsBody* ignoreBody = nullptr);

    // =========================================================================
    // Collision callbacks
    // =========================================================================

    void SetCollisionCallback(std::function<void(const ContactInfo&)> callback) { m_collisionCallback = callback; }
    void SetTriggerCallback(std::function<void(PhysicsBody*, PhysicsBody*, bool)> callback)
    {
        m_triggerCallback = callback;
    }
    void SetEventBus(Spark::EventBus* bus) { m_eventBus = bus; }

    // =========================================================================
    // Debug rendering
    // =========================================================================

    void EnableDebugDraw(bool enabled) { m_debugDrawEnabled = enabled; }
    bool IsDebugDrawEnabled() const { return m_debugDrawEnabled; }
    void SetDebugDrawMode(int mode);
    void RenderDebug();

    // =========================================================================
    // Material management
    // =========================================================================

    void RegisterMaterial(const std::string& name, const PhysicsMaterial& material);
    const PhysicsMaterial* GetMaterial(const std::string& name) const;
    void SetDefaultMaterial(const PhysicsMaterial& material) { m_defaultMaterial = material; }

    // =========================================================================
    // Metrics
    // =========================================================================

    PhysicsMetrics GetMetrics() const;

    // =========================================================================
    // Console integration methods
    // =========================================================================

    PhysicsMetrics Console_GetMetrics() const;
    std::string Console_ListBodies() const;
    std::string Console_GetBodyInfo(const std::string& name) const;
    bool Console_CreateBody(const std::string& name, const std::string& type, float x, float y, float z);
    bool Console_RemoveBody(const std::string& name);
    void Console_SetGravity(float x, float y, float z);
    void Console_SetBodyProperty(const std::string& name, const std::string& property, float value);
    void Console_ApplyForce(const std::string& name, float x, float y, float z);
    void Console_ApplyImpulse(const std::string& name, float x, float y, float z);
    void Console_EnableDebugDraw(bool enabled);
    void Console_PausePhysics(bool paused);
    void Console_SetTimeStep(float timeStep);
    std::string Console_Raycast(float originX, float originY, float originZ, float dirX, float dirY, float dirZ,
                                float maxDistance);
    void Console_Reset();

    // =========================================================================
    // Collision layer filtering
    // =========================================================================

    /**
     * @brief Test whether two bodies should collide based on group/mask bitmasks.
     *
     * Implements symmetric filtering: A collides with B only if
     * `(A.group & B.mask) != 0 && (B.group & A.mask) != 0`.
     *
     * @param a First body descriptor.
     * @param b Second body descriptor.
     * @return true if the two bodies should collide.
     */
    static bool ShouldCollide(const PhysicsBodyDesc& a, const PhysicsBodyDesc& b)
    {
        return (a.collisionGroup & b.collisionMask) != 0 && (b.collisionGroup & a.collisionMask) != 0;
    }

    /** @brief Get the Jolt physics system (for internal use by PhysicsBody). */
    JPH::PhysicsSystem* GetJoltSystem() const { return m_joltSystem.get(); }

    /** @brief Get the temp allocator (used by CharacterController). */
    JPH::TempAllocator* GetTempAllocator() const { return m_tempAllocator.get(); }

    // =========================================================================
    // Character controller
    // =========================================================================

    /**
     * @brief Create a character controller for player/NPC movement.
     *
     * Uses Jolt's CharacterVirtual for precise collision response without
     * physics simulation forces. Supports walking, jumping, slopes, stairs,
     * and moving platform detection.
     *
     * @param desc  Character controller configuration.
     * @return      Unique pointer to the new CharacterController.
     */
    std::unique_ptr<class CharacterController> CreateCharacterController(const struct CharacterControllerDesc& desc);

    /**
     * @brief Create a vehicle with engine, transmission, and suspension.
     * @param body  The rigid body to attach the vehicle constraint to.
     * @param desc  Vehicle configuration (wheels, engine, transmission).
     * @return      Unique pointer to the new VehiclePhysics.
     */
    std::unique_ptr<VehiclePhysics> CreateVehicle(std::shared_ptr<PhysicsBody> body, const VehicleDesc& desc);

    /**
     * @brief Create a ragdoll from a skeleton descriptor.
     * @param desc  Ragdoll part definitions with joints.
     * @return      Unique pointer to the new Ragdoll.
     */
    std::unique_ptr<Ragdoll> CreateRagdoll(const RagdollDesc& desc);

    /**
     * @brief Create a soft body for cloth, flags, or deformable objects.
     * @param desc  Soft body vertex, edge, and face definitions.
     * @return      Unique pointer to the new SoftBody.
     */
    std::unique_ptr<SoftBody> CreateSoftBody(const SoftBodyDesc& desc);

    // =========================================================================
    // Collision group filtering (GroupFilterTable)
    // =========================================================================

    /**
     * @brief Create a GroupFilterTable for sub-group-based collision filtering.
     * @param numSubGroups  Number of sub-groups in the table.
     * @return A filter table ID to assign to bodies via CollisionGroupDesc.
     */
    uint32_t CreateGroupFilterTable(uint32_t numSubGroups);

    /**
     * @brief Disable collision between two sub-groups in a filter table.
     * @param filterID   Filter table ID returned by CreateGroupFilterTable().
     * @param subGroup1  First sub-group index.
     * @param subGroup2  Second sub-group index.
     */
    void DisableGroupCollision(uint32_t filterID, uint32_t subGroup1, uint32_t subGroup2);

    /**
     * @brief Enable collision between two sub-groups in a filter table.
     * @param filterID   Filter table ID returned by CreateGroupFilterTable().
     * @param subGroup1  First sub-group index.
     * @param subGroup2  Second sub-group index.
     */
    void EnableGroupCollision(uint32_t filterID, uint32_t subGroup1, uint32_t subGroup2);

    // =========================================================================
    // Mutable compound shape (runtime sub-shape modification)
    // =========================================================================

    /**
     * @brief Create a body with a MutableCompoundShape that supports runtime modifications.
     *
     * Unlike a static compound shape, sub-shapes can be added, removed, or repositioned
     * at runtime — useful for destructible objects, modular vehicles, or building systems.
     *
     * @param desc      Body descriptor (shape field is ignored; sub-shapes come from subShapes).
     * @param subShapes Initial sub-shapes to include.
     * @return The new physics body.
     */
    std::shared_ptr<PhysicsBody> CreateMutableCompoundBody(const PhysicsBodyDesc& desc,
                                                           const std::vector<MutableSubShapeDesc>& subShapes);

    /**
     * @brief Add a sub-shape to a MutableCompoundShape body at runtime.
     * @param body       Body with a MutableCompoundShape.
     * @param subShape   Sub-shape to add.
     * @return Index of the newly added sub-shape, or UINT32_MAX on failure.
     */
    uint32_t AddSubShape(std::shared_ptr<PhysicsBody> body, const MutableSubShapeDesc& subShape);

    /**
     * @brief Remove a sub-shape from a MutableCompoundShape body by index.
     * @param body   Body with a MutableCompoundShape.
     * @param index  Sub-shape index to remove.
     */
    void RemoveSubShape(std::shared_ptr<PhysicsBody> body, uint32_t index);

    // =========================================================================
    // Offset center of mass
    // =========================================================================

    /**
     * @brief Wrap a body's shape with an offset center of mass.
     *
     * Shifts the center of mass without changing collision geometry — useful
     * for making top-heavy objects more stable (e.g. boats, vehicles).
     *
     * @param body    Body to modify.
     * @param offset  Center-of-mass offset in local space.
     */
    void SetCenterOfMassOffset(std::shared_ptr<PhysicsBody> body, const XMFLOAT3& offset);

    // =========================================================================
    // Surface velocity
    // =========================================================================

    /**
     * @brief Set a surface velocity on a body (for conveyor belts, moving walkways).
     *
     * The surface velocity is applied to all contact points with this body,
     * causing objects on top to move along the surface.
     *
     * @param body     The body to set surface velocity on.
     * @param velocity Surface velocity vector in world space (m/s).
     */
    void SetSurfaceVelocity(std::shared_ptr<PhysicsBody> body, const XMFLOAT3& velocity);

    // =========================================================================
    // Buoyancy
    // =========================================================================

    /**
     * @brief Apply buoyancy force to a body based on a water plane.
     *
     * Calculates the submerged volume of the body's shape below the water surface
     * and applies an upward buoyancy force proportional to the displaced water.
     *
     * @param body          Body to apply buoyancy to.
     * @param waterSurface  Point on the water surface (world space).
     * @param waterNormal   Normal of the water surface (typically {0, 1, 0}).
     * @param waterDensity  Density of the water (kg/m³, default 1000 for freshwater).
     * @param linearDrag    Linear drag coefficient in water.
     * @param angularDrag   Angular drag coefficient in water.
     */
    void ApplyBuoyancy(std::shared_ptr<PhysicsBody> body, const XMFLOAT3& waterSurface,
                       const XMFLOAT3& waterNormal = {0, 1, 0}, float waterDensity = 1000.0f, float linearDrag = 0.3f,
                       float angularDrag = 0.05f);

    // =========================================================================
    // State serialization
    // =========================================================================

    /**
     * @brief Save the current physics state to a binary buffer.
     *
     * Captures positions, rotations, velocities, and activation state
     * of all bodies. Useful for save games and network state snapshots.
     *
     * @param outBuffer  Output buffer filled with serialized physics state.
     * @return true on success.
     */
    bool SaveState(std::vector<uint8_t>& outBuffer) const;

    /**
     * @brief Restore physics state from a previously saved binary buffer.
     *
     * @param buffer  Buffer containing serialized physics state.
     * @return true on success.
     */
    bool LoadState(const std::vector<uint8_t>& buffer);

  private:
    // =========================================================================
    // Jolt Physics world objects
    // =========================================================================

    std::unique_ptr<JPH::PhysicsSystem> m_joltSystem;
    std::unique_ptr<JPH::TempAllocator> m_tempAllocator;
    std::unique_ptr<JPH::JobSystem> m_jobSystem;
    std::unique_ptr<JPH::BroadPhaseLayerInterface> m_broadPhaseLayerInterface;
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilter> m_objectVsBroadphaseFilter;
    std::unique_ptr<JPH::ObjectLayerPairFilter> m_objectLayerPairFilter;
    std::unique_ptr<JPH::ContactListener> m_contactListener;
    std::unique_ptr<JPH::BodyActivationListener> m_bodyActivationListener;

    // =========================================================================
    // Body and constraint registries
    // =========================================================================

    std::vector<std::shared_ptr<PhysicsBody>> m_bodies;
    std::vector<std::shared_ptr<PhysicsConstraint>> m_constraints;
    std::unordered_map<std::string, std::shared_ptr<PhysicsBody>> m_namedBodies;

    /** @brief Maps Jolt BodyID raw values → PhysicsBody for fast collision lookups. */
    std::unordered_map<uint32_t, PhysicsBody*> m_bodyIDMap;

    // =========================================================================
    // Shape cache
    // =========================================================================

    // Ownership: void* stores heap-allocated JPH::ShapeRefC* (a typedef for
    // RefConst<Shape>). void* is used because ShapeRefC cannot be forward-declared
    // without pulling the entire Jolt header tree into this header. The cache owns
    // the allocations and deletes them in Shutdown(). This is exception-safe in
    // practice: Shutdown() is always called, and no code path between allocation
    // and Shutdown() can throw (Jolt API is noexcept).
    std::unordered_map<size_t, void*> m_shapeCache;

    // =========================================================================
    // Material presets
    // =========================================================================

    std::unordered_map<std::string, PhysicsMaterial> m_materials;
    PhysicsMaterial m_defaultMaterial;

    // =========================================================================
    // Group filter tables
    // =========================================================================

    // Ownership: same pattern as m_shapeCache — void* stores heap-allocated
    // JPH::Ref<GroupFilterTable>*. Deleted in Shutdown(). See shape cache comment.
    std::vector<void*> m_groupFilterTables;

    // =========================================================================
    // Surface velocity map (bodyID -> velocity, read by contact listener)
    // =========================================================================

    std::unordered_map<uint32_t, XMFLOAT3> m_surfaceVelocities;
    std::mutex m_surfaceVelocityMutex;

    // =========================================================================
    // Simulation settings
    // =========================================================================

    float m_timeStep = 1.0f / 60.0f;
    int m_maxSubsteps = 10;
    bool m_paused = false;
    bool m_deterministicMode = false;

    /** @brief One-shot WARN latch for RemoveBody-after-teardown (W10 exit AV guard). */
    bool m_warnedRemoveAfterTeardown = false;

    // =========================================================================
    // Interpolation
    // =========================================================================

    float m_accumulator = 0.0f;
    float m_interpolationAlpha = 1.0f;
    bool m_interpolationEnabled = true;

    // =========================================================================
    // Debug rendering
    // =========================================================================

    bool m_debugDrawEnabled = false;

    // =========================================================================
    // Callbacks
    // =========================================================================

    std::function<void(const ContactInfo&)> m_collisionCallback;
    std::function<void(PhysicsBody*, PhysicsBody*, bool)> m_triggerCallback;
    Spark::EventBus* m_eventBus = nullptr;
    std::vector<std::pair<PhysicsBody*, PhysicsBody*>> m_activeTriggerPairs;

    // =========================================================================
    // Metrics
    // =========================================================================

    mutable std::mutex m_metricsMutex;
    PhysicsMetrics m_metrics;

    // =========================================================================
    // Private helpers
    // =========================================================================

#if SPARK_JOLT_PHYSICS_AVAILABLE
    void* CreateCollisionShape(const CollisionShapeDesc& desc);
    void* CreateBoxShape(const XMFLOAT3& dimensions);
    void* CreateSphereShape(float radius);
    void* CreateCapsuleShape(float radius, float height);
    void* CreateCylinderShape(float radius, float height);
    void* CreateConeShape(float radius, float height);
    void* CreateMeshShape(const std::vector<XMFLOAT3>& vertices, const std::vector<uint32_t>& indices);
    void* CreateConvexHullShape(const std::vector<XMFLOAT3>& vertices);
#endif // SPARK_JOLT_PHYSICS_AVAILABLE

    void UpdateMetrics();
    void ProcessCollisions();
    void DispatchCollisionCallbacks(std::vector<std::pair<PhysicsBody*, PhysicsBody*>>& outTriggerPairs);
    void UpdateTriggerExitEvents(const std::vector<std::pair<PhysicsBody*, PhysicsBody*>>& currentTriggerPairs);
    size_t HashShape(const CollisionShapeDesc& desc) const;

    /** @brief Look up a PhysicsBody wrapper by its Jolt BodyID raw value. */
    PhysicsBody* FindBodyByJoltID(uint32_t joltBodyID) const;

    friend class SparkContactListener;
};

// Utility functions are declared in PhysicsTypes.h (included above).
