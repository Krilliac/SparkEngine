/**
 * @file ConstraintComponent.h
 * @brief ECS component for physics constraints (joints) between rigid bodies
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Provides an ECS-friendly component for creating physics constraints between
 * entities with RigidBodyComponent. Supports hinge, slider, fixed, spring,
 * cone-twist, and generic 6-DOF joints via the existing PhysicsSystem API.
 *
 * Breakable constraints allow destructible environments — when the applied
 * force exceeds `breakForce`, the constraint is automatically removed and
 * an event is fired via the EventBus.
 */

#pragma once

#include "../../../Core/Platform.h"
#include "../../../Utils/OpaqueHandle.h"
#include <string>

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

// =============================================================================
// ConstraintComponent
// =============================================================================

/**
 * @brief ECS component for physics joints between two rigid body entities.
 *
 * Attach this component to the "parent" entity. Set `targetEntity` to the
 * other entity involved in the constraint. The PhysicsUpdateSystem reads
 * these components and creates/destroys Bullet constraints as needed.
 */
struct ConstraintComponent
{
    /**
     * @brief Supported constraint (joint) types.
     */
    enum class Type
    {
        Hinge,      ///< Rotation around a single axis (doors, wheels)
        Slider,     ///< Translation along a single axis (pistons, drawbridges)
        Fixed,      ///< No relative motion (welded together)
        Spring,     ///< Damped spring with configurable stiffness
        ConeTwist,  ///< Limited rotation cone (ragdoll shoulders, ball joints)
        Generic6DOF ///< Fully configurable 6 degrees of freedom
    };

    Type type = Type::Fixed;

    /** @brief Entity ID of the other body in the constraint. */
    uint32_t targetEntity = 0;

    /** @brief Local-space pivot point on this entity. */
    DirectX::XMFLOAT3 pivotA{0.0f, 0.0f, 0.0f};

    /** @brief Local-space pivot point on the target entity. */
    DirectX::XMFLOAT3 pivotB{0.0f, 0.0f, 0.0f};

    /** @brief Constraint axis (interpretation depends on Type). */
    DirectX::XMFLOAT3 axis{0.0f, 1.0f, 0.0f};

    // --- Hinge / ConeTwist limits ---

    /** @brief Lower angular limit in radians (hinge/cone-twist). */
    float lowerLimit = -3.14159f;

    /** @brief Upper angular limit in radians (hinge/cone-twist). */
    float upperLimit = 3.14159f;

    // --- Spring parameters ---

    /** @brief Spring stiffness coefficient (spring type only). */
    float stiffness = 100.0f;

    /** @brief Spring damping coefficient (spring type only). */
    float damping = 1.0f;

    /** @brief Spring rest length (spring type only). */
    float restLength = 0.0f;

    // --- Breakable constraint ---

    /** @brief Maximum force before constraint breaks. 0 = unbreakable. */
    float breakForce = 0.0f;

    /** @brief Maximum torque before constraint breaks. 0 = unbreakable. */
    float breakTorque = 0.0f;

    /** @brief Set to true by the system when the constraint has broken. */
    bool isBroken = false;

    // --- Motor (hinge/slider) ---

    /** @brief Enable motor-driven constraint. */
    bool enableMotor = false;

    /** @brief Target velocity for the motor (rad/s or m/s). */
    float motorTargetVelocity = 0.0f;

    /** @brief Maximum impulse the motor can apply per step. */
    float motorMaxImpulse = 10.0f;

    // --- Runtime handle (managed by PhysicsUpdateSystem) ---

    /** @brief Opaque handle to the Bullet constraint instance. */
    Spark::PhysicsHandle constraintHandle;

    /** @brief Whether the constraint has been created in the physics world. */
    bool initialized = false;
};
