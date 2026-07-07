/**
 * @file JoltPhysicsInterface.h
 * @brief Abstract physics interface to support Jolt Physics migration
 * @author Spark Engine Team
 * @date 2026
 *
 * Defines backend-agnostic physics interfaces that decouple engine code
 * from a specific physics library (Bullet, Jolt, PhysX). The engine
 * programs against these interfaces, and a concrete backend provides
 * the implementation.
 *
 * Interfaces:
 * - IPhysicsShape: collision geometry (box, sphere, capsule, convex, mesh)
 * - IPhysicsBody: rigid body with transform, velocity, forces
 * - IPhysicsWorld: simulation stepping, body management, queries
 * - IContactListener: collision event callbacks
 *
 * Also provides a CollisionFilter with 16-layer enable/disable matrix
 * and PhysicsMaterial for surface properties.
 *
 * @see Engine/Physics/, BulletPhysics integration
 */

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Spark::Physics
{

    // =========================================================================
    // Forward Declarations
    // =========================================================================

    class IPhysicsShape;
    class IPhysicsBody;
    class IPhysicsWorld;

    // =========================================================================
    // Math Types (engine-agnostic)
    // =========================================================================

    struct PhysVec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;

        PhysVec3() = default;
        PhysVec3(float ax, float ay, float az) : x(ax), y(ay), z(az) {}

        PhysVec3 operator+(const PhysVec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
        PhysVec3 operator-(const PhysVec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
        PhysVec3 operator*(float s) const { return {x * s, y * s, z * s}; }

        float Dot(const PhysVec3& o) const { return x * o.x + y * o.y + z * o.z; }
        float LengthSq() const { return Dot(*this); }
    };

    struct PhysQuat
    {
        float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

        PhysQuat() = default;
        PhysQuat(float ax, float ay, float az, float aw) : x(ax), y(ay), z(az), w(aw) {}

        static PhysQuat Identity() { return {0.0f, 0.0f, 0.0f, 1.0f}; }
    };

    struct PhysTransform
    {
        PhysVec3 position;
        PhysQuat rotation;

        static PhysTransform Identity() { return {PhysVec3{0, 0, 0}, PhysQuat::Identity()}; }
    };

    // =========================================================================
    // Physics Material
    // =========================================================================

    struct PhysicsMaterial
    {
        float friction = 0.5f;
        float restitution = 0.3f;
        float density = 1000.0f; ///< kg/m^3
        std::string name;
    };

    // =========================================================================
    // Shape Types
    // =========================================================================

    enum class ShapeType : uint8_t
    {
        Box,
        Sphere,
        Capsule,
        ConvexHull,
        TriangleMesh
    };

    // =========================================================================
    // Collision Filter (16 layers)
    // =========================================================================

    static constexpr int COLLISION_LAYER_COUNT = 16;

    /**
     * @brief Layer-based collision filtering with a 16x16 enable/disable matrix
     *
     * Each body is assigned a layer [0, 15]. The matrix determines which
     * layer pairs can collide. Symmetric: setting (A, B) also sets (B, A).
     */
    class CollisionFilter
    {
      public:
        CollisionFilter()
        {
            // Default: all layers collide with all layers
            m_matrix.fill(0xFFFF);
        }

        /// @brief Enable collision between two layers
        void EnableCollision(uint8_t layerA, uint8_t layerB)
        {
            if (layerA >= COLLISION_LAYER_COUNT || layerB >= COLLISION_LAYER_COUNT)
                return;
            m_matrix[layerA] |= (1u << layerB);
            m_matrix[layerB] |= (1u << layerA);
        }

        /// @brief Disable collision between two layers
        void DisableCollision(uint8_t layerA, uint8_t layerB)
        {
            if (layerA >= COLLISION_LAYER_COUNT || layerB >= COLLISION_LAYER_COUNT)
                return;
            m_matrix[layerA] &= ~(1u << layerB);
            m_matrix[layerB] &= ~(1u << layerA);
        }

        /// @brief Check if two layers can collide
        bool CanCollide(uint8_t layerA, uint8_t layerB) const
        {
            if (layerA >= COLLISION_LAYER_COUNT || layerB >= COLLISION_LAYER_COUNT)
                return false;
            return (m_matrix[layerA] & (1u << layerB)) != 0;
        }

        /// @brief Disable all collisions for a layer
        void DisableAllForLayer(uint8_t layer)
        {
            if (layer >= COLLISION_LAYER_COUNT)
                return;
            for (int i = 0; i < COLLISION_LAYER_COUNT; ++i)
            {
                m_matrix[i] &= ~(1u << layer);
            }
            m_matrix[layer] = 0;
        }

      private:
        std::array<uint16_t, COLLISION_LAYER_COUNT> m_matrix;
    };

    // =========================================================================
    // Contact / Collision Callbacks
    // =========================================================================

    struct ContactPoint
    {
        PhysVec3 position; ///< Contact point in world space
        PhysVec3 normal;   ///< Contact normal (from body A to body B)
        float penetrationDepth = 0.0f;
        float impulse = 0.0f; ///< Applied impulse magnitude
    };

    /**
     * @brief Interface for receiving collision callbacks
     */
    class IContactListener
    {
      public:
        virtual ~IContactListener() = default;

        /// @brief Called when two bodies first begin overlapping
        virtual void OnContactBegin(IPhysicsBody* bodyA, IPhysicsBody* bodyB, const ContactPoint& contact) = 0;

        /// @brief Called each simulation step while bodies remain in contact
        virtual void OnContactPersist(IPhysicsBody* bodyA, IPhysicsBody* bodyB, const ContactPoint& contact) = 0;

        /// @brief Called when two bodies stop overlapping
        virtual void OnContactEnd(IPhysicsBody* bodyA, IPhysicsBody* bodyB) = 0;
    };

    // =========================================================================
    // Query Results
    // =========================================================================

    struct RayCastResult
    {
        bool hit = false;
        float distance = 0.0f;
        PhysVec3 hitPoint;
        PhysVec3 hitNormal;
        IPhysicsBody* body = nullptr;
    };

    struct SweepResult
    {
        bool hit = false;
        float fraction = 0.0f; ///< Fraction of sweep distance [0, 1]
        PhysVec3 hitPoint;
        PhysVec3 hitNormal;
        IPhysicsBody* body = nullptr;
    };

    // =========================================================================
    // IPhysicsShape
    // =========================================================================

    /**
     * @brief Abstract collision shape
     */
    class IPhysicsShape
    {
      public:
        virtual ~IPhysicsShape() = default;

        virtual ShapeType GetType() const = 0;
        virtual PhysVec3 GetLocalBoundsMin() const = 0;
        virtual PhysVec3 GetLocalBoundsMax() const = 0;
        virtual float GetVolume() const = 0;

        // NOTE: Shape construction is the concrete backend's responsibility. A
        // backend factory (see Source/Physics/PhysicsShapeFactory) creates shapes;
        // this header stays a pure interface. (Previously declared but never-defined
        // static CreateBox/Sphere/Capsule/ConvexHull/TriangleMesh entry points were
        // removed — they were a link-error trap with no implementation or caller.)
    };

    // =========================================================================
    // Body Settings
    // =========================================================================

    enum class BodyType : uint8_t
    {
        Static,
        Kinematic,
        Dynamic
    };

    struct BodySettings
    {
        PhysTransform transform = PhysTransform::Identity();
        BodyType type = BodyType::Dynamic;
        uint8_t collisionLayer = 0;
        float mass = 1.0f;
        float linearDamping = 0.05f;
        float angularDamping = 0.05f;
        PhysicsMaterial material;
        bool useDoublePrecision = false; ///< Enable for large-world support
        bool isSensor = false;           ///< Trigger volume (no physics response)
    };

    // =========================================================================
    // IPhysicsBody
    // =========================================================================

    /**
     * @brief Abstract rigid body with transform, velocity, and force application
     */
    class IPhysicsBody
    {
      public:
        virtual ~IPhysicsBody() = default;

        // Transform
        virtual void SetTransform(const PhysTransform& transform) = 0;
        virtual PhysTransform GetTransform() const = 0;
        virtual void SetPosition(const PhysVec3& position) = 0;
        virtual PhysVec3 GetPosition() const = 0;
        virtual void SetRotation(const PhysQuat& rotation) = 0;
        virtual PhysQuat GetRotation() const = 0;

        // Velocity
        virtual void SetLinearVelocity(const PhysVec3& velocity) = 0;
        virtual PhysVec3 GetLinearVelocity() const = 0;
        virtual void SetAngularVelocity(const PhysVec3& velocity) = 0;
        virtual PhysVec3 GetAngularVelocity() const = 0;

        // Forces
        virtual void ApplyForce(const PhysVec3& force) = 0;
        virtual void ApplyForceAtPoint(const PhysVec3& force, const PhysVec3& point) = 0;
        virtual void ApplyTorque(const PhysVec3& torque) = 0;
        virtual void ApplyImpulse(const PhysVec3& impulse) = 0;

        // Properties
        virtual void SetMass(float mass) = 0;
        virtual float GetMass() const = 0;
        virtual void SetMaterial(const PhysicsMaterial& material) = 0;
        virtual BodyType GetBodyType() const = 0;
        virtual uint8_t GetCollisionLayer() const = 0;
        virtual void SetCollisionLayer(uint8_t layer) = 0;

        // State
        virtual bool IsActive() const = 0;
        virtual void SetActive(bool active) = 0;
        virtual void SetUserData(void* data) = 0;
        virtual void* GetUserData() const = 0;
    };

    // =========================================================================
    // World Settings
    // =========================================================================

    struct WorldSettings
    {
        PhysVec3 gravity = {0.0f, -9.81f, 0.0f};
        float fixedTimestep = 1.0f / 60.0f;
        int maxSubSteps = 4;
        bool useDoublePrecision = false; ///< Large-world double-precision support
        int solverIterations = 10;
    };

    // =========================================================================
    // IPhysicsWorld
    // =========================================================================

    /**
     * @brief Abstract physics simulation world
     *
     * Manages body lifetime, simulation stepping, and spatial queries.
     * A concrete backend (Bullet, Jolt) implements this interface.
     */
    class IPhysicsWorld
    {
      public:
        virtual ~IPhysicsWorld() = default;

        // Lifecycle
        virtual bool Initialize(const WorldSettings& settings) = 0;
        virtual void Shutdown() = 0;

        // Simulation
        virtual void StepSimulation(float deltaTime) = 0;
        virtual void SetGravity(const PhysVec3& gravity) = 0;
        virtual PhysVec3 GetGravity() const = 0;

        // Body management
        virtual IPhysicsBody* CreateBody(const BodySettings& settings, IPhysicsShape* shape) = 0;
        virtual void DestroyBody(IPhysicsBody* body) = 0;
        virtual uint32_t GetBodyCount() const = 0;

        // Queries
        virtual RayCastResult RayCast(const PhysVec3& origin, const PhysVec3& direction, float maxDistance,
                                      uint16_t layerMask = 0xFFFF) const = 0;
        virtual SweepResult SweepTest(IPhysicsShape* shape, const PhysTransform& from, const PhysVec3& direction,
                                      float maxDistance, uint16_t layerMask = 0xFFFF) const = 0;

        // Collision filter
        virtual void SetCollisionFilter(const CollisionFilter& filter) = 0;
        virtual const CollisionFilter& GetCollisionFilter() const = 0;

        // Contact listener
        virtual void SetContactListener(IContactListener* listener) = 0;

        // NOTE: World construction is the concrete backend's responsibility (see
        // Source/Physics/PhysicsSystem). The previously declared but never-defined
        // static Create(backend) factory was removed — it had no implementation or
        // caller and would only produce a linker error if invoked.
    };

} // namespace Spark::Physics
