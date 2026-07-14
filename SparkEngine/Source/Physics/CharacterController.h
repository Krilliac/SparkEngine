/**
 * @file CharacterController.h
 * @brief Jolt CharacterVirtual wrapper for game character movement
 * @author Spark Engine Team
 * @date 2025
 *
 * Wraps Jolt's CharacterVirtual to provide a game-ready character controller
 * with walking, jumping, slopes, stairs, moving platforms, and ground detection.
 * The CharacterVirtual approach is preferred over a kinematic body because it
 * provides precise collision response without being affected by physics forces.
 *
 * @see PhysicsSystem.h, PhysicsTypes.h
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS

#include <cstdint>
#include <memory>

using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4;

class PhysicsSystem;

namespace JPH
{
    class CharacterVirtual;
}

/**
 * @brief Ground state of the character controller.
 */
enum class CharacterGroundState
{
    OnGround,      ///< Character is standing on solid ground
    OnSteepGround, ///< Character is on ground steeper than max slope angle
    NotSupported,  ///< Character is touching something but not supported
    InAir          ///< Character is airborne (no ground contact)
};

/**
 * @brief Configuration for creating a CharacterController.
 */
struct CharacterControllerDesc
{
    float height = 1.75f;                   ///< Total capsule height (metres)
    float radius = 0.3f;                    ///< Capsule radius (metres)
    float mass = 80.0f;                     ///< Character mass (kg) — used to push objects
    float maxSlopeAngle = 0.78f;            ///< Maximum walkable slope angle (radians, ~45 degrees)
    float maxStrength = 100.0f;             ///< Maximum force to push other bodies (N)
    float characterPadding = 0.02f;         ///< Distance to keep from geometry (metres)
    float penetrationRecovery = 1.0f;       ///< Speed of penetration recovery [0-1]
    float predictiveContactDistance = 0.1f; ///< Look-ahead distance for predictive contacts
    XMFLOAT3 position = {0, 0, 0};          ///< Initial world position
    XMFLOAT4 rotation = {0, 0, 0, 1};       ///< Initial rotation (quaternion)
    XMFLOAT3 up = {0, 1, 0};                ///< Up direction for ground detection
};

/**
 * @class CharacterController
 * @brief Game-ready character controller using Jolt CharacterVirtual.
 *
 * Provides FPS/third-person character movement with:
 * - Gravity-aware walking on surfaces
 * - Slope limiting (slides off steep slopes)
 * - Stair stepping
 * - Moving platform support
 * - Jump with ground detection
 * - Smooth collision sliding
 *
 * ### Usage
 * @code
 *   CharacterControllerDesc desc;
 *   desc.height = 1.8f;
 *   desc.radius = 0.3f;
 *   desc.position = playerSpawn;
 *   auto controller = physics.CreateCharacterController(desc);
 *
 *   // Each frame:
 *   XMFLOAT3 moveDir = GetInputDirection();
 *   controller->SetLinearVelocity(moveDir * speed);
 *   controller->Update(deltaTime, gravity);
 *   playerTransform.position = controller->GetPosition();
 * @endcode
 */
class CharacterController
{
  public:
    CharacterController(PhysicsSystem* physicsSystem, const CharacterControllerDesc& desc);
    ~CharacterController();

    // Non-copyable
    CharacterController(const CharacterController&) = delete;
    CharacterController& operator=(const CharacterController&) = delete;

    /**
     * @brief Update the character controller position based on velocity and gravity.
     *
     * Call this once per frame after setting the desired velocity. The controller
     * will resolve collisions, slide along walls, and respect slope limits.
     *
     * @param deltaTime Time step in seconds.
     * @param gravity   Gravity vector to apply (world space, e.g. {0, -9.81, 0}).
     */
    void Update(float deltaTime, const XMFLOAT3& gravity);

    // =========================================================================
    // Position and velocity
    // =========================================================================

    XMFLOAT3 GetPosition() const;
    void SetPosition(const XMFLOAT3& position);
    XMFLOAT4 GetRotation() const;
    void SetRotation(const XMFLOAT4& rotation);
    XMFLOAT3 GetLinearVelocity() const;
    void SetLinearVelocity(const XMFLOAT3& velocity);

    // =========================================================================
    // Ground detection
    // =========================================================================

    /** @brief Get the current ground state (on ground, in air, steep slope). */
    CharacterGroundState GetGroundState() const;

    /** @brief Check if the character is standing on walkable ground. */
    bool IsOnGround() const;

    /** @brief Get the surface normal of the ground below the character. */
    XMFLOAT3 GetGroundNormal() const;

    /** @brief Get the velocity of the ground body (for moving platforms). */
    XMFLOAT3 GetGroundVelocity() const;

    // =========================================================================
    // Properties
    // =========================================================================

    /** @brief Get the up direction used for ground detection. */
    XMFLOAT3 GetUp() const;

    /** @brief Set the up direction (useful for gravity changes or planet walking). */
    void SetUp(const XMFLOAT3& up);

    /** @brief Get the character mass in kg. */
    float GetMass() const { return m_desc.mass; }

  private:
    /// @brief Attempt to create the Jolt character from stored desc. Returns true on success.
    bool TryInitialize();

    PhysicsSystem* m_physicsSystem; ///< Non-owning; lifetime tied to engine
    CharacterControllerDesc m_desc;
    std::unique_ptr<JPH::CharacterVirtual> m_joltCharacter; ///< RAII-owned Jolt character
    bool m_pendingInit = false; ///< True if construction was deferred due to null PhysicsSystem
};
