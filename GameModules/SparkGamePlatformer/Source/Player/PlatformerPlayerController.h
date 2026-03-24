/**
 * @file PlatformerPlayerController.h
 * @brief 3D platformer player movement, jumping, and ability state machine
 * @author Spark Engine Team
 * @date 2026
 *
 * Implements a full-featured platformer player controller:
 * - Physics-based movement with acceleration/deceleration
 * - State machine: idle, running, jumping, double jump, wall slide/jump, dash, ground pound
 * - Coyote time (grace period after leaving a ledge)
 * - Jump buffering (queue jump input before landing)
 * - Variable jump height (hold for higher, tap for lower)
 * - Ability unlock system (abilities start locked, unlocked by AbilityOrbs)
 * - Lives and respawn at last checkpoint
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/PlatformerEnums.h"
#include <cstdint>
#include <string>

namespace Platformer
{

    class PlatformerCheckpointSystem;

    /// @brief Player position as a simple 3D vector
    struct PlayerPosition
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    /// @brief Player velocity for physics simulation
    struct PlayerVelocity
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    /// @brief Tracks which abilities the player has unlocked
    struct AbilityFlags
    {
        bool doubleJump = false;
        bool wallJump = false;
        bool dash = false;
        bool groundPound = false;
        bool climb = false;
    };

    /**
     * @brief Controls player movement, physics, and state transitions
     *
     * The controller uses a state machine for animation integration. Each state
     * transition notifies the engine's animation system so the correct animation
     * plays. Physics is applied in FixedUpdate for deterministic behavior, while
     * visual interpolation happens in Update.
     */
    class PlatformerPlayerController
    {
      public:
        PlatformerPlayerController() = default;
        ~PlatformerPlayerController() = default;

        bool Initialize(Spark::IEngineContext* context, PlatformerCheckpointSystem* checkpoints);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Render();
        void Shutdown();

        void RenderDebugUI();

        /// @brief Get current player world position
        PlayerPosition GetPlayerPosition() const { return m_position; }

        /// @brief Get current state as human-readable string
        std::string GetStateString() const;

        /// @brief Get full player status for console output
        std::string GetPlayerStatusString() const;

        /// @brief Current number of lives remaining
        int GetLives() const { return m_lives; }

        /// @brief Respawn at last activated checkpoint
        void Respawn();

        /// @brief Apply damage to the player (from hazards)
        void TakeDamage(int amount);

        /// @brief Unlock a movement ability
        void UnlockAbility(PowerUpType type);

        /// @brief Apply a temporary power-up effect
        void ApplyPowerUp(PowerUpType type, float duration);

      private:
        void ProcessInput(float deltaTime);
        void ApplyGravity(float fixedDeltaTime);
        void ApplyMovement(float fixedDeltaTime);
        void UpdateState();
        void CheckGrounded();
        void HandleJump();
        void HandleWallSlide(float fixedDeltaTime);
        void HandleDash();
        void HandleGroundPound();
        void UpdateCoyoteTime(float fixedDeltaTime);
        void UpdateJumpBuffer(float fixedDeltaTime);
        void UpdateInvincibility(float deltaTime);
        void UpdatePowerUps(float deltaTime);
        void TransitionState(PlayerState newState);

        Spark::IEngineContext* m_context{nullptr};
        PlatformerCheckpointSystem* m_checkpoints{nullptr};

        // Position and physics
        PlayerPosition m_position{};
        PlayerVelocity m_velocity{};
        PlayerState m_state{PlayerState::Idle};
        float m_facingDirection{1.0f}; // 1.0 = right, -1.0 = left

        // Movement tuning
        float m_moveSpeed{8.0f};
        float m_runMultiplier{1.5f};
        float m_acceleration{40.0f};
        float m_deceleration{30.0f};
        float m_airAcceleration{20.0f};

        // Jump tuning
        float m_jumpForce{12.0f};
        float m_doubleJumpForce{10.0f};
        float m_wallJumpForceX{8.0f};
        float m_wallJumpForceY{11.0f};
        float m_gravity{-30.0f};
        float m_maxFallSpeed{-20.0f};
        float m_jumpCutMultiplier{0.4f}; // Applied when jump button released early

        // Coyote time and jump buffer
        float m_coyoteTime{0.12f};
        float m_coyoteTimer{0.0f};
        float m_jumpBufferTime{0.1f};
        float m_jumpBufferTimer{0.0f};

        // Dash tuning
        float m_dashSpeed{20.0f};
        float m_dashDuration{0.15f};
        float m_dashTimer{0.0f};
        float m_dashCooldown{0.5f};
        float m_dashCooldownTimer{0.0f};

        // Wall slide tuning
        float m_wallSlideSpeed{-2.0f};

        // Ground pound
        float m_groundPoundSpeed{-25.0f};

        // State tracking
        bool m_grounded{false};
        bool m_wasGrounded{false};
        bool m_touchingWall{false};
        bool m_hasDoubleJumped{false};
        bool m_hasDashed{false};
        bool m_jumpHeld{false};
        bool m_jumpRequested{false};

        // Abilities (locked by default, unlocked via AbilityOrbs)
        AbilityFlags m_abilities{};

        // Lives and invincibility
        int m_lives{3};
        static constexpr int MAX_LIVES = 9;
        float m_invincibilityDuration{1.5f};
        float m_invincibilityTimer{0.0f};
        bool m_invincible{false};

        // Active power-ups
        float m_speedBoostTimer{0.0f};
        float m_magnetTimer{0.0f};
        float m_shieldTimer{0.0f};
        float m_ghostTimer{0.0f};
        bool m_hasShield{false};

        bool m_initialized{false};
    };

} // namespace Platformer
