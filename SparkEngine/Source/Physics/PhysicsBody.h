/**
 * @file PhysicsBody.h
 * @brief PhysicsBody and PhysicsConstraint wrapper classes
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains the DirectX Math-native wrappers around Jolt Physics bodies
 * and constraints. These classes are the primary handles callers use to interact
 * with individual physics objects.
 *
 * @see PhysicsTypes.h, PhysicsSystem.h
 */

#pragma once

#include "../Core/Platform.h"
#include "PhysicsTypes.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <cstdint>
#include <string>

// Forward declarations for Jolt Physics
namespace JPH
{
    class Body;
    class BodyID;
    class Constraint;
} // namespace JPH

/**
 * @class PhysicsBody
 * @brief DirectX Math-native wrapper around a Jolt `JPH::Body`.
 *
 * PhysicsBody is the primary handle callers use to interact with a single
 * rigid body in the simulation. It abstracts away Jolt's coordinate system
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
    PhysicsBody(const PhysicsBodyDesc& desc, uint32_t joltBodyID);
    ~PhysicsBody();

    // =========================================================================
    // Transform
    // =========================================================================

    XMFLOAT3 GetPosition() const;
    void SetPosition(const XMFLOAT3& position);
    XMFLOAT3 GetRotation() const;
    void SetRotation(const XMFLOAT3& rotation);
    XMMATRIX GetTransform() const;
    void SetTransform(const XMMATRIX& transform);

    // =========================================================================
    // Velocity
    // =========================================================================

    XMFLOAT3 GetLinearVelocity() const;
    void SetLinearVelocity(const XMFLOAT3& velocity);
    XMFLOAT3 GetAngularVelocity() const;
    void SetAngularVelocity(const XMFLOAT3& velocity);

    // =========================================================================
    // Forces and impulses
    // =========================================================================

    void ApplyForce(const XMFLOAT3& force, const XMFLOAT3& relativePos = {0, 0, 0});
    void ApplyImpulse(const XMFLOAT3& impulse, const XMFLOAT3& relativePos = {0, 0, 0});
    void ApplyTorque(const XMFLOAT3& torque);
    void ApplyTorqueImpulse(const XMFLOAT3& torque);

    // =========================================================================
    // Properties
    // =========================================================================

    float GetMass() const;
    void SetMass(float mass);
    PhysicsBodyType GetType() const { return m_desc.type; }
    const PhysicsMaterial& GetMaterial() const { return m_desc.material; }
    void SetMaterial(const PhysicsMaterial& material);

    // =========================================================================
    // State
    // =========================================================================

    void SetActive(bool active);
    bool IsActive() const;
    void SetKinematic(bool kinematic);
    bool IsKinematic() const;
    void SetTrigger(bool trigger);
    bool IsTrigger() const;

    // =========================================================================
    // Collision filtering
    // =========================================================================

    void SetCollisionGroup(uint16_t group);
    void SetCollisionMask(uint16_t mask);
    uint16_t GetCollisionGroup() const { return m_collisionGroup; }
    uint16_t GetCollisionMask() const { return m_collisionMask; }

    // =========================================================================
    // User data
    // =========================================================================

    void SetUserData(void* data) { m_desc.userData = data; }
    void* GetUserData() const { return m_desc.userData; }
    const std::string& GetName() const { return m_desc.name; }

    /** @brief Get the ECS entity ID that owns this body (0 = no entity). */
    uint32_t GetEntityID() const { return m_desc.entityId; }

    /** @brief Set the ECS entity ID that owns this body. */
    void SetEntityID(uint32_t id) { m_desc.entityId = id; }

    // =========================================================================
    // Internal / low-level access
    // =========================================================================

    /** @brief Get the Jolt BodyID (index into the Jolt physics system). */
    uint32_t GetJoltBodyID() const { return m_joltBodyID; }
    const PhysicsBodyDesc& GetDesc() const { return m_desc; }

    // =========================================================================
    // Console integration
    // =========================================================================

    std::string GetInfo() const;
    void Console_SetProperty(const std::string& property, float value);
    void Console_ApplyForce(float x, float y, float z);

    // =========================================================================
    // Interpolation
    // =========================================================================

    XMFLOAT3 GetInterpolatedPosition(float alpha) const;
    XMMATRIX GetInterpolatedTransform(float alpha) const;
    void StoreCurrentState();
    void UpdateCurrentState();

  private:
    /// @brief Push m_collisionGroup/m_collisionMask to Jolt's broadphase.
    void ApplyCollisionFilter();

    PhysicsBodyDesc m_desc;
    uint32_t m_joltBodyID; ///< Jolt BodyID raw value (BodyID::GetIndexAndSequenceNumber())
    uint16_t m_collisionGroup = 1;
    uint16_t m_collisionMask = 0xFFFF;

    // Interpolation state
    XMFLOAT3 m_previousPosition{0, 0, 0};
    XMFLOAT4 m_previousRotation{0, 0, 0, 1}; ///< Previous quaternion
    XMFLOAT3 m_currentPosition{0, 0, 0};
    XMFLOAT4 m_currentRotation{0, 0, 0, 1}; ///< Current quaternion
};

/**
 * @brief Physics constraint wrapper
 */
class PhysicsConstraint
{
  public:
    PhysicsConstraint(ConstraintType type, JPH::Constraint* joltConstraint);
    ~PhysicsConstraint();

    ConstraintType GetType() const { return m_type; }
    void SetEnabled(bool enabled);
    bool IsEnabled() const;
    void SetBreakingThreshold(float threshold);
    float GetBreakingThreshold() const;

    JPH::Constraint* GetJoltConstraint() const { return m_joltConstraint; }

  private:
    ConstraintType m_type;
    JPH::Constraint* m_joltConstraint;
    float m_breakingThreshold = 0.0f; ///< Force threshold for auto-breaking (0 = unbreakable)
};
