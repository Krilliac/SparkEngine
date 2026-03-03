/**
 * @file PhysicsSystem.h
 * @brief Complete physics integration system for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 * 
 * This class provides a comprehensive physics system supporting collision
 * detection, dynamics simulation, constraints, and advanced physics features
 * for AAA-quality gameplay mechanics.
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

using namespace DirectX;

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
 * @brief Physics material properties
 */
struct PhysicsMaterial
{
    float friction = 0.5f;                ///< Surface friction coefficient
    float restitution = 0.1f;             ///< Bounce/elasticity coefficient
    float linearDamping = 0.1f;           ///< Linear velocity damping
    float angularDamping = 0.1f;          ///< Angular velocity damping
    float density = 1.0f;                 ///< Material density
    bool isStatic = false;                ///< Static material flag
    std::string name;                     ///< Material name
};

/**
 * @brief Collision shape definition
 */
struct CollisionShapeDesc
{
    CollisionShapeType type = CollisionShapeType::Box;
    XMFLOAT3 dimensions = {1.0f, 1.0f, 1.0f}; ///< Shape dimensions
    float radius = 0.5f;                  ///< Radius for sphere/capsule/cylinder
    float height = 1.0f;                  ///< Height for capsule/cylinder/cone
    std::string meshPath;                 ///< Mesh file path for mesh shapes
    std::vector<XMFLOAT3> vertices;       ///< Vertices for convex hull
    std::vector<uint32_t> indices;        ///< Indices for mesh shape
    XMFLOAT3 localOffset = {0, 0, 0};     ///< Local offset from body center
    XMFLOAT3 localRotation = {0, 0, 0};   ///< Local rotation
};

/**
 * @brief Physics body definition
 */
struct PhysicsBodyDesc
{
    PhysicsBodyType type = PhysicsBodyType::Dynamic;
    XMFLOAT3 position = {0, 0, 0};        ///< Initial position
    XMFLOAT3 rotation = {0, 0, 0};        ///< Initial rotation (Euler angles)
    XMFLOAT3 linearVelocity = {0, 0, 0};  ///< Initial linear velocity
    XMFLOAT3 angularVelocity = {0, 0, 0}; ///< Initial angular velocity
    float mass = 1.0f;                    ///< Body mass (0 for static)
    PhysicsMaterial material;             ///< Physics material
    CollisionShapeDesc shape;             ///< Collision shape
    bool isTrigger = false;               ///< Trigger volume flag
    bool isKinematic = false;             ///< Kinematic body flag
    std::string name;                     ///< Body name for debugging
    void* userData = nullptr;             ///< User data pointer
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
 * @brief Physics body wrapper
 */
class PhysicsBody
{
public:
    PhysicsBody(const PhysicsBodyDesc& desc, btRigidBody* bulletBody);
    ~PhysicsBody();

    // Transform
    XMFLOAT3 GetPosition() const;
    void SetPosition(const XMFLOAT3& position);
    XMFLOAT3 GetRotation() const;
    void SetRotation(const XMFLOAT3& rotation);
    XMMATRIX GetTransform() const;
    void SetTransform(const XMMATRIX& transform);

    // Velocity
    XMFLOAT3 GetLinearVelocity() const;
    void SetLinearVelocity(const XMFLOAT3& velocity);
    XMFLOAT3 GetAngularVelocity() const;
    void SetAngularVelocity(const XMFLOAT3& velocity);

    // Forces
    void ApplyForce(const XMFLOAT3& force, const XMFLOAT3& relativePos = {0, 0, 0});
    void ApplyImpulse(const XMFLOAT3& impulse, const XMFLOAT3& relativePos = {0, 0, 0});
    void ApplyTorque(const XMFLOAT3& torque);
    void ApplyTorqueImpulse(const XMFLOAT3& torque);

    // Properties
    float GetMass() const;
    void SetMass(float mass);
    PhysicsBodyType GetType() const { return m_desc.type; }
    const PhysicsMaterial& GetMaterial() const { return m_desc.material; }
    void SetMaterial(const PhysicsMaterial& material);

    // State
    void SetActive(bool active);
    bool IsActive() const;
    void SetKinematic(bool kinematic);
    bool IsKinematic() const;
    void SetTrigger(bool trigger);
    bool IsTrigger() const;

    // Collision filtering
    void SetCollisionGroup(uint16_t group);
    void SetCollisionMask(uint16_t mask);
    uint16_t GetCollisionGroup() const { return m_collisionGroup; }
    uint16_t GetCollisionMask() const { return m_collisionMask; }

    // User data
    void SetUserData(void* data) { m_desc.userData = data; }
    void* GetUserData() const { return m_desc.userData; }
    const std::string& GetName() const { return m_desc.name; }

    // Internal
    btRigidBody* GetBulletBody() const { return m_bulletBody; }
    const PhysicsBodyDesc& GetDesc() const { return m_desc; }

    // Console integration
    std::string GetInfo() const;
    void Console_SetProperty(const std::string& property, float value);
    void Console_ApplyForce(float x, float y, float z);

private:
    PhysicsBodyDesc m_desc;
    btRigidBody* m_bulletBody;
    uint16_t m_collisionGroup = 1;
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
 * @brief Physics world manager
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
     * @brief Initialize the physics system
     */
    HRESULT Initialize();

    /**
     * @brief Shutdown the physics system
     */
    void Shutdown();

    /**
     * @brief Update physics simulation
     */
    void Update(float deltaTime);

    // World settings
    void SetGravity(const XMFLOAT3& gravity);
    XMFLOAT3 GetGravity() const;
    void SetTimeStep(float timeStep) { m_timeStep = timeStep; }
    float GetTimeStep() const { return m_timeStep; }
    void SetMaxSubsteps(int maxSubsteps) { m_maxSubsteps = maxSubsteps; }
    int GetMaxSubsteps() const { return m_maxSubsteps; }

    // Body management
    std::shared_ptr<PhysicsBody> CreateBody(const PhysicsBodyDesc& desc);
    void RemoveBody(std::shared_ptr<PhysicsBody> body);
    void RemoveAllBodies();
    const std::vector<std::shared_ptr<PhysicsBody>>& GetBodies() const { return m_bodies; }

    // Constraint management
    std::shared_ptr<PhysicsConstraint> CreateHingeConstraint(
        std::shared_ptr<PhysicsBody> bodyA, std::shared_ptr<PhysicsBody> bodyB,
        const XMFLOAT3& pivotA, const XMFLOAT3& pivotB,
        const XMFLOAT3& axisA, const XMFLOAT3& axisB);
    std::shared_ptr<PhysicsConstraint> CreateSliderConstraint(
        std::shared_ptr<PhysicsBody> bodyA, std::shared_ptr<PhysicsBody> bodyB,
        const XMMATRIX& frameA, const XMMATRIX& frameB);
    std::shared_ptr<PhysicsConstraint> CreateFixedConstraint(
        std::shared_ptr<PhysicsBody> bodyA, std::shared_ptr<PhysicsBody> bodyB,
        const XMMATRIX& frameA, const XMMATRIX& frameB);
    void RemoveConstraint(std::shared_ptr<PhysicsConstraint> constraint);

    // Collision detection
    RaycastHit Raycast(const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance = 1000.0f);
    std::vector<RaycastHit> RaycastAll(const XMFLOAT3& origin, const XMFLOAT3& direction, float maxDistance = 1000.0f);
    bool SphereOverlap(const XMFLOAT3& center, float radius, std::vector<PhysicsBody*>& results);
    bool BoxOverlap(const XMFLOAT3& center, const XMFLOAT3& halfExtents, std::vector<PhysicsBody*>& results);

    // Collision callbacks
    void SetCollisionCallback(std::function<void(const ContactInfo&)> callback) { m_collisionCallback = callback; }
    void SetTriggerCallback(std::function<void(PhysicsBody*, PhysicsBody*, bool)> callback) { m_triggerCallback = callback; }

    // Debug rendering
    void EnableDebugDraw(bool enabled) { m_debugDrawEnabled = enabled; }
    bool IsDebugDrawEnabled() const { return m_debugDrawEnabled; }
    void SetDebugDrawMode(int mode);
    void RenderDebug();

    // Material management
    void RegisterMaterial(const std::string& name, const PhysicsMaterial& material);
    const PhysicsMaterial* GetMaterial(const std::string& name) const;
    void SetDefaultMaterial(const PhysicsMaterial& material) { m_defaultMaterial = material; }

    // Metrics
    PhysicsMetrics GetMetrics() const;

    // ========================================================================
    // CONSOLE INTEGRATION METHODS
    // ========================================================================

    /**
     * @brief Get physics system metrics
     */
    PhysicsMetrics Console_GetMetrics() const;

    /**
     * @brief List all physics bodies
     */
    std::string Console_ListBodies() const;

    /**
     * @brief Get physics body information
     */
    std::string Console_GetBodyInfo(const std::string& name) const;

    /**
     * @brief Create physics body via console
     */
    bool Console_CreateBody(const std::string& name, const std::string& type, float x, float y, float z);

    /**
     * @brief Remove physics body via console
     */
    bool Console_RemoveBody(const std::string& name);

    /**
     * @brief Set gravity
     */
    void Console_SetGravity(float x, float y, float z);

    /**
     * @brief Set body property
     */
    void Console_SetBodyProperty(const std::string& name, const std::string& property, float value);

    /**
     * @brief Apply force to body
     */
    void Console_ApplyForce(const std::string& name, float x, float y, float z);

    /**
     * @brief Apply impulse to body
     */
    void Console_ApplyImpulse(const std::string& name, float x, float y, float z);

    /**
     * @brief Enable/disable debug rendering
     */
    void Console_EnableDebugDraw(bool enabled);

    /**
     * @brief Pause/unpause physics simulation
     */
    void Console_PausePhysics(bool paused);

    /**
     * @brief Set physics time step
     */
    void Console_SetTimeStep(float timeStep);

    /**
     * @brief Perform raycast
     */
    std::string Console_Raycast(float originX, float originY, float originZ, 
                               float dirX, float dirY, float dirZ, float maxDistance);

    /**
     * @brief Reset physics world
     */
    void Console_Reset();

private:
    // Bullet Physics world
    btDiscreteDynamicsWorld* m_dynamicsWorld;
    btDefaultCollisionConfiguration* m_collisionConfig;
    btCollisionDispatcher* m_dispatcher;
    btDbvtBroadphase* m_broadphase;
    btSequentialImpulseConstraintSolver* m_solver;

    // Bodies and constraints
    std::vector<std::shared_ptr<PhysicsBody>> m_bodies;
    std::vector<std::shared_ptr<PhysicsConstraint>> m_constraints;
    std::unordered_map<std::string, std::shared_ptr<PhysicsBody>> m_namedBodies;

    // Collision shapes cache
    std::unordered_map<size_t, btCollisionShape*> m_shapeCache;

    // Materials
    std::unordered_map<std::string, PhysicsMaterial> m_materials;
    PhysicsMaterial m_defaultMaterial;

    // Simulation settings
    float m_timeStep = 1.0f / 60.0f;
    int m_maxSubsteps = 10;
    bool m_paused = false;

    // Debug rendering
    bool m_debugDrawEnabled = false;
    class DebugDrawer* m_debugDrawer;

    // Callbacks
    std::function<void(const ContactInfo&)> m_collisionCallback;
    std::function<void(PhysicsBody*, PhysicsBody*, bool)> m_triggerCallback;

    // Metrics
    mutable std::mutex m_metricsMutex;
    PhysicsMetrics m_metrics;

    // Helper methods
    btCollisionShape* CreateCollisionShape(const CollisionShapeDesc& desc);
    btCollisionShape* CreateBoxShape(const XMFLOAT3& dimensions);
    btCollisionShape* CreateSphereShape(float radius);
    btCollisionShape* CreateCapsuleShape(float radius, float height);
    btCollisionShape* CreateMeshShape(const std::vector<XMFLOAT3>& vertices, const std::vector<uint32_t>& indices);
    btCollisionShape* CreateConvexHullShape(const std::vector<XMFLOAT3>& vertices);

    void UpdateMetrics();
    void ProcessCollisions();
    size_t HashShape(const CollisionShapeDesc& desc);
    
    // Bullet conversion helpers
    class btVector3 ToBullet(const XMFLOAT3& vec) const;
    XMFLOAT3 FromBullet(const class btVector3& vec) const;
    class btQuaternion ToBulletQuaternion(const XMFLOAT3& euler) const;
    XMFLOAT3 FromBullet(const class btQuaternion& quat) const;
};

// Utility functions
std::string PhysicsBodyTypeToString(PhysicsBodyType type);
PhysicsBodyType StringToPhysicsBodyType(const std::string& str);
std::string CollisionShapeTypeToString(CollisionShapeType type);
CollisionShapeType StringToCollisionShapeType(const std::string& str);
std::string ConstraintTypeToString(ConstraintType type);
ConstraintType StringToConstraintType(const std::string& str);