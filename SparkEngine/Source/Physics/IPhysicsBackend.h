/**
 * @file IPhysicsBackend.h
 * @brief Abstract physics backend interface for swappable physics engines
 *
 * Defines the interface that physics implementations (Bullet, Jolt, etc.) must
 * implement. This abstraction enables future migration to Jolt Physics
 * (ezEngine-inspired) without changing engine code that uses physics.
 *
 * Currently, PhysicsSystem implements this interface directly using Bullet Physics.
 * A future JoltPhysicsBackend could implement the same interface.
 *
 * @note This is a forward-looking abstraction. The current engine still uses
 *       PhysicsSystem directly in most places. Migration would involve:
 *       1. PhysicsSystem inherits IPhysicsBackend
 *       2. Engine code uses IPhysicsBackend* from EngineContext
 *       3. New backends implement IPhysicsBackend
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <cstdint>
#include <functional>
#include <string>

namespace Spark::Physics
{

    /// Collision shape types supported by all backends.
    enum class ShapeType
    {
        Box,
        Sphere,
        Capsule,
        ConvexHull,
        TriangleMesh,
        Heightfield,
        Compound
    };

    /// Rigid body motion type.
    enum class MotionType
    {
        Static,    ///< Never moves (walls, terrain)
        Kinematic, ///< Moves via code, not forces (elevators, moving platforms)
        Dynamic    ///< Affected by forces and gravity
    };

    /// Contact/collision event data.
    struct ContactEvent
    {
        uint32_t bodyA = 0;
        uint32_t bodyB = 0;
        DirectX::XMFLOAT3 contactPoint{0, 0, 0};
        DirectX::XMFLOAT3 contactNormal{0, 0, 0};
        float impulse = 0.0f;
    };

    /// Raycast hit result.
    struct RaycastHit
    {
        bool hit = false;
        uint32_t bodyID = 0;
        DirectX::XMFLOAT3 position{0, 0, 0};
        DirectX::XMFLOAT3 normal{0, 0, 0};
        float distance = 0.0f;
    };

    using ContactCallback = std::function<void(const ContactEvent&)>;

    /**
     * @brief Abstract interface for physics engine backends.
     *
     * All physics operations go through this interface. Implementations
     * manage their own internal state (dynamics world, broadphase, etc.)
     * and translate between the engine's DirectXMath types and the
     * backend's native types.
     */
    class IPhysicsBackend
    {
      public:
        virtual ~IPhysicsBackend() = default;

        // ================================================================
        // Lifecycle
        // ================================================================

        virtual bool Initialize() = 0;
        virtual void Shutdown() = 0;
        virtual void StepSimulation(float deltaTime) = 0;

        // ================================================================
        // Rigid Body Management
        // ================================================================

        /// Create a rigid body and return its unique ID.
        virtual uint32_t CreateBody(ShapeType shape, const DirectX::XMFLOAT3& halfExtents, MotionType motionType,
                                    const DirectX::XMFLOAT3& position, float mass) = 0;

        /// Destroy a rigid body by ID.
        virtual void DestroyBody(uint32_t bodyID) = 0;

        /// Set the world transform of a body.
        virtual void SetTransform(uint32_t bodyID, const DirectX::XMFLOAT3& position,
                                  const DirectX::XMFLOAT4& rotation) = 0;

        /// Get the world transform of a body.
        virtual void GetTransform(uint32_t bodyID, DirectX::XMFLOAT3& outPosition,
                                  DirectX::XMFLOAT4& outRotation) const = 0;

        /// Apply a force to a body (accumulated over the timestep).
        virtual void ApplyForce(uint32_t bodyID, const DirectX::XMFLOAT3& force) = 0;

        /// Apply an impulse to a body (instantaneous velocity change).
        virtual void ApplyImpulse(uint32_t bodyID, const DirectX::XMFLOAT3& impulse) = 0;

        /// Set the linear velocity of a body.
        virtual void SetLinearVelocity(uint32_t bodyID, const DirectX::XMFLOAT3& velocity) = 0;

        /// Get the linear velocity of a body.
        virtual DirectX::XMFLOAT3 GetLinearVelocity(uint32_t bodyID) const = 0;

        // ================================================================
        // Queries
        // ================================================================

        /// Cast a ray and return the first hit.
        virtual RaycastHit Raycast(const DirectX::XMFLOAT3& origin, const DirectX::XMFLOAT3& direction,
                                   float maxDistance) const = 0;

        // ================================================================
        // Callbacks
        // ================================================================

        /// Set a callback for contact events.
        virtual void SetContactCallback(ContactCallback callback) = 0;

        // ================================================================
        // Configuration
        // ================================================================

        /// Set the gravity vector.
        virtual void SetGravity(const DirectX::XMFLOAT3& gravity) = 0;

        /// Get the backend name (e.g., "Bullet", "Jolt").
        virtual const char* GetBackendName() const = 0;

        /// Get debug statistics string.
        virtual std::string GetDebugStats() const = 0;
    };

} // namespace Spark::Physics
