/**
 * @file PlatformerPlayerController.cpp
 * @brief 3D platformer player controller with physics, state machine, and abilities
 */

#include "PlatformerPlayerController.h"
#include "Checkpoint/PlatformerCheckpointSystem.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>

namespace Platformer
{

    bool PlatformerPlayerController::Initialize(Spark::IEngineContext* context, PlatformerCheckpointSystem* checkpoints)
    {
        if (!context)
            return false;

        m_context = context;
        m_checkpoints = checkpoints;

        // Start at origin; the level system will set the proper spawn point
        m_position = {0.0f, 5.0f, 0.0f};
        m_velocity = {0.0f, 0.0f, 0.0f};
        m_state = PlayerState::Falling;
        m_lives = 3;

        m_initialized = true;

        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[Platformer Player] Player controller initialized");
        return true;
    }

    void PlatformerPlayerController::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        ProcessInput(deltaTime);
        UpdateInvincibility(deltaTime);
        UpdatePowerUps(deltaTime);
    }

    void PlatformerPlayerController::FixedUpdate(float fixedDeltaTime)
    {
        if (!m_initialized)
            return;

        if (m_state == PlayerState::Dead)
            return;

        // Save previous grounded state for coyote time
        m_wasGrounded = m_grounded;

        CheckGrounded();
        UpdateCoyoteTime(fixedDeltaTime);
        UpdateJumpBuffer(fixedDeltaTime);

        HandleJump();
        HandleWallSlide(fixedDeltaTime);
        HandleDash();
        HandleGroundPound();

        ApplyGravity(fixedDeltaTime);
        ApplyMovement(fixedDeltaTime);
        UpdateState();
    }

    void PlatformerPlayerController::ProcessInput(float deltaTime)
    {
        (void)deltaTime;

        // In a real implementation, input would come from the engine's InputManager.
        // Here we document the intended input mappings:
        // - Left stick / WASD: horizontal movement
        // - A / Space: jump (hold for variable height)
        // - X / Shift: dash
        // - B / Ctrl: ground pound (in air)
        // - Y / E: interact (checkpoints, doors)
        // - LT / Run: run modifier
    }

    void PlatformerPlayerController::ApplyGravity(float fixedDeltaTime)
    {
        if (m_state == PlayerState::Dashing)
            return;

        // Wall slide has reduced gravity
        if (m_state == PlayerState::WallSliding)
        {
            m_velocity.y = std::max(m_velocity.y + m_gravity * 0.1f * fixedDeltaTime, m_wallSlideSpeed);
            return;
        }

        // Ground pound overrides vertical velocity
        if (m_state == PlayerState::GroundPounding)
        {
            m_velocity.y = m_groundPoundSpeed;
            return;
        }

        // Standard gravity with terminal velocity
        m_velocity.y += m_gravity * fixedDeltaTime;
        m_velocity.y = std::max(m_velocity.y, m_maxFallSpeed);
    }

    void PlatformerPlayerController::ApplyMovement(float fixedDeltaTime)
    {
        // Dash overrides normal movement
        if (m_state == PlayerState::Dashing)
        {
            m_position.x += m_facingDirection * m_dashSpeed * fixedDeltaTime;
            return;
        }

        // Ground pound locks horizontal movement
        if (m_state == PlayerState::GroundPounding)
        {
            m_position.y += m_velocity.y * fixedDeltaTime;
            return;
        }

        // Apply speed boost if active
        float currentMoveSpeed = m_moveSpeed;
        if (m_speedBoostTimer > 0.0f)
            currentMoveSpeed *= 1.5f;

        // Horizontal deceleration when no input
        float accel = m_grounded ? m_deceleration : m_airAcceleration;
        if (std::abs(m_velocity.x) > 0.1f)
        {
            float sign = (m_velocity.x > 0.0f) ? 1.0f : -1.0f;
            m_velocity.x -= sign * accel * fixedDeltaTime;

            // Prevent overshoot past zero
            if ((sign > 0.0f && m_velocity.x < 0.0f) || (sign < 0.0f && m_velocity.x > 0.0f))
                m_velocity.x = 0.0f;
        }

        // Clamp horizontal speed
        m_velocity.x = std::clamp(m_velocity.x, -currentMoveSpeed, currentMoveSpeed);

        // Apply velocity to position
        m_position.x += m_velocity.x * fixedDeltaTime;
        m_position.y += m_velocity.y * fixedDeltaTime;
        m_position.z += m_velocity.z * fixedDeltaTime;

        // Simple ground plane collision (y = 0)
        if (m_position.y <= 0.0f)
        {
            m_position.y = 0.0f;
            m_velocity.y = 0.0f;
            m_grounded = true;
        }
    }

    void PlatformerPlayerController::CheckGrounded()
    {
        // In a full implementation, this raycasts downward using the physics engine.
        // For the demo, we use a simple ground plane check at y = 0.
        m_grounded = (m_position.y <= 0.01f && m_velocity.y <= 0.0f);

        // Reset jump/dash when landing
        if (m_grounded && !m_wasGrounded)
        {
            m_hasDoubleJumped = false;
            m_hasDashed = false;
        }
    }

    void PlatformerPlayerController::UpdateCoyoteTime(float fixedDeltaTime)
    {
        // Start coyote timer when leaving ground without jumping
        if (m_wasGrounded && !m_grounded && m_velocity.y <= 0.0f)
        {
            m_coyoteTimer = m_coyoteTime;
        }

        if (m_coyoteTimer > 0.0f)
            m_coyoteTimer -= fixedDeltaTime;
    }

    void PlatformerPlayerController::UpdateJumpBuffer(float fixedDeltaTime)
    {
        if (m_jumpRequested)
        {
            m_jumpBufferTimer = m_jumpBufferTime;
            m_jumpRequested = false;
        }

        if (m_jumpBufferTimer > 0.0f)
            m_jumpBufferTimer -= fixedDeltaTime;
    }

    void PlatformerPlayerController::HandleJump()
    {
        bool canJump = m_grounded || m_coyoteTimer > 0.0f;
        bool bufferedJump = m_jumpBufferTimer > 0.0f;

        // Standard jump (or buffered jump on landing)
        if (bufferedJump && canJump)
        {
            m_velocity.y = m_jumpForce;
            m_coyoteTimer = 0.0f;
            m_jumpBufferTimer = 0.0f;
            m_grounded = false;
            TransitionState(PlayerState::Jumping);
            return;
        }

        // Double jump (requires ability unlock)
        if (bufferedJump && !m_grounded && !m_hasDoubleJumped && m_abilities.doubleJump)
        {
            m_velocity.y = m_doubleJumpForce;
            m_hasDoubleJumped = true;
            m_jumpBufferTimer = 0.0f;
            TransitionState(PlayerState::DoubleJumping);
            return;
        }

        // Variable jump height: cut velocity when button released early
        if (!m_jumpHeld && m_velocity.y > 0.0f &&
            (m_state == PlayerState::Jumping || m_state == PlayerState::DoubleJumping))
        {
            m_velocity.y *= m_jumpCutMultiplier;
        }
    }

    void PlatformerPlayerController::HandleWallSlide(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;

        if (!m_abilities.wallJump)
            return;

        // Wall slide: touching wall + falling + holding toward wall
        if (m_touchingWall && !m_grounded && m_velocity.y < 0.0f)
        {
            TransitionState(PlayerState::WallSliding);

            // Wall jump: press jump while wall sliding
            if (m_jumpBufferTimer > 0.0f)
            {
                m_velocity.x = -m_facingDirection * m_wallJumpForceX;
                m_velocity.y = m_wallJumpForceY;
                m_facingDirection = -m_facingDirection;
                m_jumpBufferTimer = 0.0f;
                m_hasDoubleJumped = false; // Reset double jump after wall jump
                TransitionState(PlayerState::WallJumping);
            }
        }
    }

    void PlatformerPlayerController::HandleDash()
    {
        if (!m_abilities.dash)
            return;

        // Active dash
        if (m_state == PlayerState::Dashing)
        {
            m_dashTimer -= 1.0f / 60.0f; // Approximate fixed timestep
            if (m_dashTimer <= 0.0f)
            {
                m_velocity.x = m_facingDirection * m_moveSpeed * 0.5f;
                m_velocity.y = 0.0f;
                TransitionState(m_grounded ? PlayerState::Running : PlayerState::Falling);
            }
            return;
        }

        // Dash cooldown
        if (m_dashCooldownTimer > 0.0f)
            m_dashCooldownTimer -= 1.0f / 60.0f;
    }

    void PlatformerPlayerController::HandleGroundPound()
    {
        if (!m_abilities.groundPound)
            return;

        if (m_state == PlayerState::GroundPounding && m_grounded)
        {
            // Ground pound landed — could trigger shockwave/effects
            TransitionState(PlayerState::Idle);
        }
    }

    void PlatformerPlayerController::UpdateState()
    {
        // Skip state updates for action states controlled by their handlers
        if (m_state == PlayerState::Dashing || m_state == PlayerState::GroundPounding ||
            m_state == PlayerState::WallSliding || m_state == PlayerState::WallJumping || m_state == PlayerState::Dead)
        {
            return;
        }

        if (m_grounded)
        {
            if (std::abs(m_velocity.x) > 0.5f)
                TransitionState(PlayerState::Running);
            else
                TransitionState(PlayerState::Idle);
        }
        else
        {
            if (m_velocity.y > 0.0f)
            {
                if (m_hasDoubleJumped)
                    TransitionState(PlayerState::DoubleJumping);
                else
                    TransitionState(PlayerState::Jumping);
            }
            else
            {
                TransitionState(PlayerState::Falling);
            }
        }
    }

    void PlatformerPlayerController::TransitionState(PlayerState newState)
    {
        if (m_state == newState)
            return;

        m_state = newState;

        // In a full implementation, this would notify the animation system:
        // m_context->GetAnimation()->SetState("player", GetStateString());
    }

    void PlatformerPlayerController::UpdateInvincibility(float deltaTime)
    {
        if (!m_invincible)
            return;

        m_invincibilityTimer -= deltaTime;
        if (m_invincibilityTimer <= 0.0f)
        {
            m_invincible = false;
            m_invincibilityTimer = 0.0f;
        }
    }

    void PlatformerPlayerController::UpdatePowerUps(float deltaTime)
    {
        if (m_speedBoostTimer > 0.0f)
            m_speedBoostTimer -= deltaTime;
        if (m_magnetTimer > 0.0f)
            m_magnetTimer -= deltaTime;
        if (m_shieldTimer > 0.0f)
        {
            m_shieldTimer -= deltaTime;
            if (m_shieldTimer <= 0.0f)
                m_hasShield = false;
        }
        if (m_ghostTimer > 0.0f)
            m_ghostTimer -= deltaTime;
    }

    void PlatformerPlayerController::TakeDamage(int amount)
    {
        if (m_invincible || m_state == PlayerState::Dead)
            return;

        // Ghost mode ignores damage
        if (m_ghostTimer > 0.0f)
            return;

        // Shield absorbs hit
        if (m_hasShield)
        {
            m_hasShield = false;
            m_shieldTimer = 0.0f;
            m_invincible = true;
            m_invincibilityTimer = m_invincibilityDuration * 0.5f;
            return;
        }

        m_lives -= amount;
        if (m_lives <= 0)
        {
            m_lives = 0;
            TransitionState(PlayerState::Dead);
            return;
        }

        // Grant invincibility frames
        m_invincible = true;
        m_invincibilityTimer = m_invincibilityDuration;

        // Knockback: small upward bounce
        m_velocity.y = m_jumpForce * 0.5f;
        m_grounded = false;
    }

    void PlatformerPlayerController::Respawn()
    {
        if (m_checkpoints)
        {
            auto spawnPos = m_checkpoints->GetLastCheckpointPosition();
            m_position = {spawnPos.x, spawnPos.y + 1.0f, spawnPos.z};
        }
        else
        {
            m_position = {0.0f, 5.0f, 0.0f};
        }

        m_velocity = {0.0f, 0.0f, 0.0f};
        m_state = PlayerState::Falling;
        m_invincible = true;
        m_invincibilityTimer = m_invincibilityDuration;
        m_hasDoubleJumped = false;
        m_hasDashed = false;

        if (m_lives <= 0)
            m_lives = 3; // Restart with default lives on game over
    }

    void PlatformerPlayerController::UnlockAbility(PowerUpType type)
    {
        auto& console = Spark::SimpleConsole::GetInstance();

        switch (type)
        {
        case PowerUpType::DoubleJump:
            m_abilities.doubleJump = true;
            console.LogInfo("[Platformer Player] Unlocked: Double Jump");
            break;
        case PowerUpType::SpeedBoost:
            // Speed boost is a temporary power-up, not a permanent unlock
            break;
        case PowerUpType::Magnet:
            // Magnet is a temporary power-up
            break;
        case PowerUpType::Shield:
            // Shield is a temporary power-up
            break;
        case PowerUpType::GhostMode:
            // Ghost is a temporary power-up
            break;
        }
    }

    void PlatformerPlayerController::ApplyPowerUp(PowerUpType type, float duration)
    {
        switch (type)
        {
        case PowerUpType::SpeedBoost:
            m_speedBoostTimer = duration;
            break;
        case PowerUpType::Magnet:
            m_magnetTimer = duration;
            break;
        case PowerUpType::Shield:
            m_hasShield = true;
            m_shieldTimer = duration;
            break;
        case PowerUpType::GhostMode:
            m_ghostTimer = duration;
            break;
        default:
            break;
        }
    }

    void PlatformerPlayerController::Render()
    {
        if (!m_initialized)
            return;

        // In a full implementation, this would render the player model,
        // ability VFX (dash trail, shield bubble, ghost transparency),
        // and invincibility flash effect via the GraphicsEngine.
    }

    void PlatformerPlayerController::Shutdown()
    {
        m_initialized = false;
    }

    std::string PlatformerPlayerController::GetStateString() const
    {
        switch (m_state)
        {
        case PlayerState::Idle:
            return "Idle";
        case PlayerState::Running:
            return "Running";
        case PlayerState::Jumping:
            return "Jumping";
        case PlayerState::DoubleJumping:
            return "DoubleJumping";
        case PlayerState::WallSliding:
            return "WallSliding";
        case PlayerState::WallJumping:
            return "WallJumping";
        case PlayerState::Dashing:
            return "Dashing";
        case PlayerState::Falling:
            return "Falling";
        case PlayerState::GroundPounding:
            return "GroundPounding";
        case PlayerState::Climbing:
            return "Climbing";
        case PlayerState::Dead:
            return "Dead";
        default:
            return "Unknown";
        }
    }

    std::string PlatformerPlayerController::GetPlayerStatusString() const
    {
        std::string status = "=== Platformer Player ===\n";
        status += "State: " + GetStateString() + "\n";
        status += "Position: (" + std::to_string(m_position.x) + ", " + std::to_string(m_position.y) + ", " +
                  std::to_string(m_position.z) + ")\n";
        status += "Velocity: (" + std::to_string(m_velocity.x) + ", " + std::to_string(m_velocity.y) + ", " +
                  std::to_string(m_velocity.z) + ")\n";
        status += "Lives: " + std::to_string(m_lives) + "/" + std::to_string(MAX_LIVES) + "\n";
        status += "Grounded: " + std::string(m_grounded ? "Yes" : "No") + "\n";
        status += "Invincible: " + std::string(m_invincible ? "Yes" : "No") + "\n";

        status += "Abilities:";
        if (m_abilities.doubleJump)
            status += " DoubleJump";
        if (m_abilities.wallJump)
            status += " WallJump";
        if (m_abilities.dash)
            status += " Dash";
        if (m_abilities.groundPound)
            status += " GroundPound";
        if (m_abilities.climb)
            status += " Climb";
        if (!m_abilities.doubleJump && !m_abilities.wallJump && !m_abilities.dash && !m_abilities.groundPound &&
            !m_abilities.climb)
            status += " None";
        status += "\n";

        return status;
    }

    void PlatformerPlayerController::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Platformer Player"))
            return;

        ImGui::Text("State: %s", GetStateString().c_str());
        ImGui::Text("Position: (%.2f, %.2f, %.2f)", m_position.x, m_position.y, m_position.z);
        ImGui::Text("Velocity: (%.2f, %.2f, %.2f)", m_velocity.x, m_velocity.y, m_velocity.z);
        ImGui::Text("Lives: %d / %d", m_lives, MAX_LIVES);
        ImGui::Text("Grounded: %s", m_grounded ? "Yes" : "No");
        ImGui::Text("Coyote Timer: %.3f", m_coyoteTimer);
        ImGui::Text("Jump Buffer: %.3f", m_jumpBufferTimer);
        ImGui::Separator();

        ImGui::Text("Abilities:");
        ImGui::Text("  Double Jump: %s", m_abilities.doubleJump ? "Unlocked" : "Locked");
        ImGui::Text("  Wall Jump: %s", m_abilities.wallJump ? "Unlocked" : "Locked");
        ImGui::Text("  Dash: %s", m_abilities.dash ? "Unlocked" : "Locked");
        ImGui::Text("  Ground Pound: %s", m_abilities.groundPound ? "Unlocked" : "Locked");
        ImGui::Text("  Climb: %s", m_abilities.climb ? "Unlocked" : "Locked");
        ImGui::Separator();

        if (m_invincible)
            ImGui::TextColored(ImVec4(1, 1, 0, 1), "INVINCIBLE (%.1fs)", m_invincibilityTimer);
        if (m_speedBoostTimer > 0.0f)
            ImGui::TextColored(ImVec4(0, 1, 1, 1), "Speed Boost (%.1fs)", m_speedBoostTimer);
        if (m_magnetTimer > 0.0f)
            ImGui::TextColored(ImVec4(1, 0, 1, 1), "Magnet (%.1fs)", m_magnetTimer);
        if (m_hasShield)
            ImGui::TextColored(ImVec4(0, 0.5f, 1, 1), "Shield (%.1fs)", m_shieldTimer);
        if (m_ghostTimer > 0.0f)
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1), "Ghost Mode (%.1fs)", m_ghostTimer);
#endif
    }

} // namespace Platformer
