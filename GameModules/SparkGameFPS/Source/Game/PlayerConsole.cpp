#include "Core/Platform.h"
#include "Core/FPSLog.h"
#include "Engine/Security/MemoryIntegrity.h"
#include "Player.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS

#include <algorithm>
#include <string>

using namespace DirectX;

// ============================================================================
// CONSOLE INTEGRATION IMPLEMENTATIONS
// ============================================================================

void Player::Console_SetHealth(float health)
{
    FPS_LOG_DEBUG("Console: setting player health to {:.1f}", health);
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_health = std::max(0.0f, std::min(m_maxHealth, health));
    if (m_health <= 0.0f)
    {
        SetActive(false);
    }
    else if (!IsActive())
    {
        SetActive(true);
    }
    NotifyStateChange();
    FPS_CONSOLE("Player health set to " + std::to_string(m_health) + " via console", "SUCCESS");
}

void Player::Console_SetArmor(float armor)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_armor = std::max(0.0f, std::min(m_maxArmor, armor));
    NotifyStateChange();
    FPS_CONSOLE("Player armor set to " + std::to_string(m_armor) + " via console", "SUCCESS");
}

void Player::Console_SetMaxHealth(float maxHealth)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (maxHealth > 0.0f && maxHealth <= 9999.0f)
    {
        m_maxHealth = maxHealth;
        if (m_health > m_maxHealth)
        {
            m_health = m_maxHealth;
        }
        NotifyStateChange();
        FPS_CONSOLE("Player max health set to " + std::to_string(m_maxHealth) + " via console", "SUCCESS");
    }
    else
    {
        FPS_CONSOLE("Invalid max health value. Must be between 1 and 9999", "ERROR");
    }
}

void Player::Console_SetSpeed(float speed)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    // Speed validation — bypassing allows speed hacking
    SPARK_BRANCH_GUARD_BEGIN("fps_speed_validation")
    if (speed > 0.0f && speed <= 100.0f)
    {
        m_speed = speed;
        NotifyStateChange();
        FPS_CONSOLE("Player speed set to " + std::to_string(m_speed) + " via console", "SUCCESS");
    }
    else
    {
        FPS_CONSOLE("Invalid speed value. Must be between 0.1 and 100", "ERROR");
    }
    SPARK_BRANCH_GUARD_END("fps_speed_validation")
}

void Player::Console_SetJumpHeight(float height)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    // Jump validation — bypassing allows infinite vertical flight
    SPARK_BRANCH_GUARD_BEGIN("fps_jump_validation")
    if (height > 0.0f && height <= 50.0f)
    {
        m_jumpHeight = height;
        NotifyStateChange();
        FPS_CONSOLE("Player jump height set to " + std::to_string(m_jumpHeight) + " via console", "SUCCESS");
    }
    else
    {
        FPS_CONSOLE("Invalid jump height. Must be between 0.1 and 50", "ERROR");
    }
    SPARK_BRANCH_GUARD_END("fps_jump_validation")
}

void Player::Console_SetPosition(float x, float y, float z)
{
    FPS_LOG_INFO("Console: teleporting player to ({:.1f}, {:.1f}, {:.1f})", x, y, z);
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    DirectX::XMFLOAT3 newPos = {x, y, z};
    SetPosition(newPos);

    if (m_camera)
    {
        m_camera->SetPosition(newPos);
    }

    m_velocity = {0.0f, 0.0f, 0.0f};

    NotifyStateChange();
    FPS_CONSOLE("Player teleported to (" + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) +
                    ") via console",
                "SUCCESS");
}

void Player::Console_SetGodMode(bool enabled)
{
    FPS_LOG_INFO("Console: god mode {}", enabled ? "enabled" : "disabled");
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_godModeEnabled = enabled;
    NotifyStateChange();
    FPS_CONSOLE("Player god mode " + std::string(enabled ? "enabled" : "disabled") + " via console", "SUCCESS");
}

void Player::Console_SetNoclip(bool enabled)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_noclipEnabled = enabled;
    NotifyStateChange();
    FPS_CONSOLE("Player noclip " + std::string(enabled ? "enabled" : "disabled") + " via console", "SUCCESS");
}

void Player::Console_SetInfiniteAmmo(bool enabled)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_infiniteAmmoEnabled = enabled;
    NotifyStateChange();
    FPS_CONSOLE("Player infinite ammo " + std::string(enabled ? "enabled" : "disabled") + " via console", "SUCCESS");
}

void Player::Console_GiveAmmo(int amount)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (amount > 0 && amount <= 9999)
    {
        m_currentAmmo = std::min(m_currentWeapon.MagazineSize, m_currentAmmo + amount);
        NotifyStateChange();
        FPS_CONSOLE("Player given " + std::to_string(amount) + " ammo. Current: " + std::to_string(m_currentAmmo) +
                        " via console",
                    "SUCCESS");
    }
    else
    {
        FPS_CONSOLE("Invalid ammo amount. Must be between 1 and 9999", "ERROR");
    }
}

void Player::Console_ChangeWeapon(WeaponType weaponType)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    ChangeWeapon(weaponType);
    NotifyStateChange();
    FPS_CONSOLE("Player weapon changed via console", "SUCCESS");
}

PlayerState Player::Console_GetState() const
{
    return GetStateThreadSafe();
}

void Player::Console_RegisterStateCallback(std::function<void(const PlayerState&)> callback)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_stateCallback = callback;
    FPS_CONSOLE("Player state callback registered", "INFO");
}

void Player::Console_ApplyPhysicsSettings(float gravity, float friction)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (gravity >= -100.0f && gravity <= 100.0f)
    {
        m_gravityForce = gravity;
    }
    if (friction >= 0.0f && friction <= 1.0f)
    {
        m_frictionCoeff = friction;
    }
    NotifyStateChange();
    FPS_CONSOLE("Player physics settings updated - Gravity: " + std::to_string(m_gravityForce) +
                    ", Friction: " + std::to_string(m_frictionCoeff) + " via console",
                "SUCCESS");
}

void Player::NotifyStateChange()
{
    if (m_stateCallback)
    {
        m_stateCallback(GetStateThreadSafe());
    }
}

PlayerState Player::GetStateThreadSafe() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    PlayerState state;
    state.health = m_health;
    state.maxHealth = m_maxHealth;
    state.armor = m_armor;
    state.maxArmor = m_maxArmor;
    state.stamina = m_stamina;
    state.maxStamina = m_maxStamina;
    state.shield = m_shield;
    state.maxShield = m_maxShield;
    state.energy = m_energy;
    state.maxEnergy = m_maxEnergy;
    state.position = GetPosition();
    state.velocity = m_velocity;
    state.currentWeapon = m_currentWeapon.Type;
    state.currentAmmo = m_currentAmmo;
    state.maxAmmo = m_currentWeapon.MagazineSize;
    state.isAlive = IsAlive();
    state.isGrounded = m_isGrounded;
    state.isReloading = m_isReloading;
    state.isRunning = m_isRunning;
    state.isCrouching = m_isCrouching;
    state.godMode = m_godModeEnabled;
    state.noclip = m_noclipEnabled;
    state.infiniteAmmo = m_infiniteAmmoEnabled;
    state.fireTimer = m_fireTimer;
    state.reloadTimer = m_reloadTimer;
    state.speed = m_speed;
    state.jumpHeight = m_jumpHeight;
    state.playerClass = m_playerClass;
    state.activeLoadoutSlot = m_activeLoadoutSlot;
    state.primaryAbilityActive = m_primaryAbility.isActive;
    state.secondaryAbilityActive = m_secondaryAbility.isActive;
    return state;
}
