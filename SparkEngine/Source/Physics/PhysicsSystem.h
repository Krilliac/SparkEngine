/**
 * @file PhysicsSystem.h
 * @brief PhysicsSystem class and umbrella include for the physics subsystem
 * @author Spark Engine Team
 * @date 2025
 *
 * This is the umbrella header for the physics subsystem. It includes:
 * - PhysicsTypes.h — Enums, descriptors, collision types, materials
 * - PhysicsBody.h  — PhysicsBody and PhysicsConstraint wrapper classes
 * - PhysicsSystem  — Central manager for the Bullet Physics dynamics world (below)
 *
 * @note PhysicsSystem is **not thread-safe**. Call all public methods from the
 *       main game thread. The physics simulation itself runs on the calling thread
 *       inside Update().
 */

#pragma once

// Subsystem headers
#include "PhysicsBody.h"

#include "../Core/Platform.h"

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
class btCollisionShape;
class btMotionState;
class btGhostPairCallback;
class btTriangleMesh;

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
    // Interpolation
    // =========================================================================

    /**
     * @brief Get the current interpolation alpha for rendering between physics steps.
     *
     * When interpolation is enabled, this value represents how far between the
     * previous and current physics state the renderer should display. Use this
     * with PhysicsBody::GetInterpolatedPosition() / GetInterpolatedTransform().
     *
     * @return  Interpolation factor in [0, 1].
     */
    float GetInterpolationAlpha() const { return m_interpolationAlpha; }

    /**
     * @brief Enable or disable physics state interpolation.
     *
     * When enabled, Update() uses a fixed-timestep accumulator pattern and
     * stores previous/current states for smooth rendering. When disabled,
     * Update() passes deltaTime directly to Bullet (legacy behavior).
     *
     * @param enabled  true to enable interpolation, false for legacy behavior.
     */
    void SetInterpolationEnabled(bool enabled) { m_interpolationEnabled = enabled; }

    /**
     * @brief Check whether physics interpolation is enabled.
     * @return  true if interpolation is active.
     */
    bool IsInterpolationEnabled() const { return m_interpolationEnabled; }

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
    std::shared_ptr<PhysicsConstraint> CreateHingeConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                             std::shared_ptr<PhysicsBody> bodyB, const XMFLOAT3& pivotA,
                                                             const XMFLOAT3& pivotB, const XMFLOAT3& axisA,
                                                             const XMFLOAT3& axisB);

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
    std::shared_ptr<PhysicsConstraint> CreateSliderConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                              std::shared_ptr<PhysicsBody> bodyB,
                                                              const XMMATRIX& frameA, const XMMATRIX& frameB);

    /**
     * @brief Create a point-to-point (ball socket) constraint between two bodies.
     *
     * A point-to-point constraint anchors a point in bodyA to a point in bodyB,
     * allowing free rotation around the anchor. Like a ball-and-socket joint.
     *
     * @param bodyA   First body. Must not be nullptr.
     * @param bodyB   Second body. Pass nullptr to anchor bodyA to a world-space point.
     * @param pivotA  Pivot point in bodyA's local space.
     * @param pivotB  Pivot point in bodyB's local space (or world space if bodyB is nullptr).
     * @return        Shared pointer to the new PhysicsConstraint.
     */
    std::shared_ptr<PhysicsConstraint> CreatePoint2PointConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                   std::shared_ptr<PhysicsBody> bodyB,
                                                                   const XMFLOAT3& pivotA, const XMFLOAT3& pivotB);

    /**
     * @brief Create a cone-twist constraint between two bodies.
     *
     * A cone-twist constraint allows rotation within a cone-shaped range of motion
     * and a twist limit around the main axis. Suitable for ragdoll shoulder/hip joints.
     *
     * @param bodyA      First body. Must not be nullptr.
     * @param bodyB      Second body.
     * @param frameA     Constraint frame in bodyA's local space.
     * @param frameB     Constraint frame in bodyB's local space.
     * @param swingSpan1 Maximum swing angle on the first axis (radians).
     * @param swingSpan2 Maximum swing angle on the second axis (radians).
     * @param twistSpan  Maximum twist angle around the main axis (radians).
     * @return           Shared pointer to the new PhysicsConstraint.
     */
    std::shared_ptr<PhysicsConstraint> CreateConeTwistConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                                 std::shared_ptr<PhysicsBody> bodyB,
                                                                 const XMMATRIX& frameA, const XMMATRIX& frameB,
                                                                 float swingSpan1 = 0.5f, float swingSpan2 = 0.5f,
                                                                 float twistSpan = 0.5f);

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
    std::shared_ptr<PhysicsConstraint> CreateFixedConstraint(std::shared_ptr<PhysicsBody> bodyA,
                                                             std::shared_ptr<PhysicsBody> bodyB, const XMMATRIX& frameA,
                                                             const XMMATRIX& frameB);

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

    /**
     * @brief Perform a convex sphere sweep (shape cast) from one position to another.
     *
     * Uses Bullet's `btCollisionWorld::convexSweepTest` with a sphere shape to detect
     * the first body the swept shape contacts along the path. This is more accurate than
     * a ray for testing movement of objects with volume (e.g., player capsule movement,
     * projectile paths for fat projectiles).
     *
     * @param radius       Radius of the swept sphere (metres).
     * @param from         Start position of the sweep (world space).
     * @param to           End position of the sweep (world space).
     * @return             RaycastHit with `hasHit = false` if no collision along the path.
     */
    RaycastHit SphereCast(float radius, const XMFLOAT3& from, const XMFLOAT3& to);

    /**
     * @brief Perform a convex box sweep (shape cast) from one position to another.
     *
     * Sweeps a box shape from `from` to `to` and returns the first hit. Useful for
     * testing whether a box-shaped object can move between two positions without
     * colliding with obstacles.
     *
     * @param halfExtents  Half-extents of the swept box (metres).
     * @param from         Start position of the sweep (world space).
     * @param to           End position of the sweep (world space).
     * @return             RaycastHit describing the first contact, or hasHit=false if clear.
     */
    RaycastHit BoxCast(const XMFLOAT3& halfExtents, const XMFLOAT3& from, const XMFLOAT3& to);

    /**
     * @brief Perform a convex capsule sweep (shape cast) from one position to another.
     *
     * Sweeps a capsule shape from `from` to `to`. Commonly used for character controller
     * movement queries to detect whether the player capsule would collide with geometry.
     *
     * @param radius  Capsule hemisphere radius (metres).
     * @param height  Capsule cylindrical section height (metres).
     * @param from    Start position of the sweep (world space).
     * @param to      End position of the sweep (world space).
     * @return        RaycastHit describing the first contact, or hasHit=false if clear.
     */
    RaycastHit CapsuleCast(float radius, float height, const XMFLOAT3& from, const XMFLOAT3& to);

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
    void SetTriggerCallback(std::function<void(PhysicsBody*, PhysicsBody*, bool)> callback)
    {
        m_triggerCallback = callback;
    }

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
    std::string Console_Raycast(float originX, float originY, float originZ, float dirX, float dirY, float dirZ,
                                float maxDistance);

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

    /** @brief Ghost pair callback for overlap tests (owned; must be deleted in Shutdown). */
    btGhostPairCallback* m_ghostPairCallback = nullptr;

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

    /** @brief Triangle mesh data used by btBvhTriangleMeshShape (owned here, freed in Shutdown). */
    std::vector<btTriangleMesh*> m_triangleMeshes;

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
    // Interpolation
    // =========================================================================

    /** @brief Accumulated time for fixed-timestep interpolation. */
    float m_accumulator = 0.0f;

    /** @brief Interpolation alpha between previous and current physics state [0,1]. */
    float m_interpolationAlpha = 1.0f;

    /** @brief When true, physics bodies support interpolated transforms for rendering. */
    bool m_interpolationEnabled = true;

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

    /**
     * @brief Set of active trigger overlap pairs from the previous frame.
     *
     * Used by ProcessCollisions() to detect trigger exit events: pairs present
     * in the previous frame but absent in the current frame have exited.
     * Each pair is stored as (min_ptr, max_ptr) to ensure canonical ordering.
     */
    std::vector<std::pair<PhysicsBody*, PhysicsBody*>> m_activeTriggerPairs;

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
#if SPARK_BULLET_PHYSICS_AVAILABLE
    btCollisionShape* CreateCollisionShape(const CollisionShapeDesc& desc);
    btCollisionShape* CreateBoxShape(const XMFLOAT3& dimensions);
    btCollisionShape* CreateSphereShape(float radius);
    btCollisionShape* CreateCapsuleShape(float radius, float height);
    btCollisionShape* CreateCylinderShape(float radius, float height);
    btCollisionShape* CreateConeShape(float radius, float height);
    btCollisionShape* CreateMeshShape(const std::vector<XMFLOAT3>& vertices, const std::vector<uint32_t>& indices);
    btCollisionShape* CreateConvexHullShape(const std::vector<XMFLOAT3>& vertices);
    class btVector3 ToBullet(const XMFLOAT3& vec) const;
    XMFLOAT3 FromBullet(const class btVector3& vec) const;
    class btQuaternion ToBulletQuaternion(const XMFLOAT3& euler) const;
    XMFLOAT3 FromBullet(const class btQuaternion& quat) const;
#endif // SPARK_BULLET_PHYSICS_AVAILABLE

    void UpdateMetrics();
    void ProcessCollisions();

    /// @brief Walk all contact manifolds and fire collision/trigger-enter callbacks.
    void DispatchCollisionCallbacks(std::vector<std::pair<PhysicsBody*, PhysicsBody*>>& outTriggerPairs);

    /// @brief Detect trigger pairs that exited since last frame and fire exit callbacks.
    void UpdateTriggerExitEvents(const std::vector<std::pair<PhysicsBody*, PhysicsBody*>>& currentTriggerPairs);

    size_t HashShape(const CollisionShapeDesc& desc) const;
};

// Utility functions are declared in PhysicsTypes.h (included above).
