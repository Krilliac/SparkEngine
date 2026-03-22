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

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
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
} // namespace JPH

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
     * @param deltaTime  Time elapsed since the last frame in seconds.
     */
    void Update(float deltaTime);

    // =========================================================================
    // World settings
    // =========================================================================

    void SetGravity(const XMFLOAT3& gravity);
    XMFLOAT3 GetGravity() const;
    void SetTimeStep(float timeStep) { m_timeStep = timeStep; }
    float GetTimeStep() const { return m_timeStep; }
    void SetMaxSubsteps(int maxSubsteps) { m_maxSubsteps = maxSubsteps; }
    int GetMaxSubsteps() const { return m_maxSubsteps; }

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

    void RemoveConstraint(std::shared_ptr<PhysicsConstraint> constraint);

    // =========================================================================
    // Spatial queries
    // =========================================================================

    RaycastHit Raycast(const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance = 1000.0f);
    std::vector<RaycastHit> RaycastAll(const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance = 1000.0f);
    bool SphereOverlap(const XMFLOAT3& center, float radius, std::vector<PhysicsBody*>& results);
    bool BoxOverlap(const XMFLOAT3& center, const XMFLOAT3& halfExtents, std::vector<PhysicsBody*>& results);
    RaycastHit SphereCast(float radius, const XMFLOAT3& from, const XMFLOAT3& to);
    RaycastHit BoxCast(const XMFLOAT3& halfExtents, const XMFLOAT3& from, const XMFLOAT3& to);
    RaycastHit CapsuleCast(float radius, float height, const XMFLOAT3& from, const XMFLOAT3& to);

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

    std::unordered_map<size_t, void*> m_shapeCache;

    // =========================================================================
    // Material presets
    // =========================================================================

    std::unordered_map<std::string, PhysicsMaterial> m_materials;
    PhysicsMaterial m_defaultMaterial;

    // =========================================================================
    // Simulation settings
    // =========================================================================

    float m_timeStep = 1.0f / 60.0f;
    int m_maxSubsteps = 10;
    bool m_paused = false;

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
};

// Utility functions are declared in PhysicsTypes.h (included above).
