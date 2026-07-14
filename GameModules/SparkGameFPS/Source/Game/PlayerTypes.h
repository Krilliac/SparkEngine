/**
 * @file PlayerTypes.h
 * @brief Player-related type definitions and state structures
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains the PlayerState struct and any supporting types extracted from
 * Player.h to reduce header weight for code that only needs the data types.
 *
 * @see Player.h
 */

#pragma once

#include "Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif

#include "ClassSystem.h"
#include "Projectiles/WeaponStats.h"

using SparkEditor::WeaponType;

/**
 * @brief Comprehensive player state for console display and serialization.
 *
 * Captures a complete snapshot of the player's current condition.
 * Used by the HUD, console stat commands, and save/load serialization.
 */
struct PlayerState
{
    // Health and resources
    float health;     ///< Current health points.
    float maxHealth;  ///< Maximum health points.
    float armor;      ///< Current armor points.
    float maxArmor;   ///< Maximum armor points.
    float stamina;    ///< Current stamina (sprint resource).
    float maxStamina; ///< Maximum stamina.
    float shield;     ///< Current shield points (regenerating).
    float maxShield;  ///< Maximum shield capacity.
    float energy;     ///< Current ability energy.
    float maxEnergy;  ///< Maximum ability energy.

    // Transform
    DirectX::XMFLOAT3 position; ///< World-space position.
    DirectX::XMFLOAT3 velocity; ///< Current velocity vector.

    // Weapon
    WeaponType currentWeapon; ///< Currently equipped weapon type.
    int currentAmmo;          ///< Rounds remaining in the magazine.
    int maxAmmo;              ///< Magazine capacity.

    // Flags
    bool isAlive;     ///< Whether the player is currently alive.
    bool isGrounded;  ///< Whether the player is on the ground.
    bool isReloading; ///< Whether a reload is in progress.
    bool isRunning;   ///< Whether the player is sprinting.
    bool isCrouching; ///< Whether the player is crouching.

    // Cheats
    bool godMode;      ///< Invulnerability cheat active.
    bool noclip;       ///< No-clip (fly through walls) active.
    bool infiniteAmmo; ///< Infinite ammo cheat active.

    // Timers
    float fireTimer;   ///< Cooldown until next shot (seconds remaining).
    float reloadTimer; ///< Time remaining in reload animation.

    // Movement
    float speed;      ///< Current movement speed (m/s).
    float jumpHeight; ///< Jump height (meters).

    // Class
    PlayerClass playerClass;     ///< Active player class archetype.
    int activeLoadoutSlot;       ///< Currently selected loadout slot index.
    bool primaryAbilityActive;   ///< Whether the primary ability is currently active.
    bool secondaryAbilityActive; ///< Whether the secondary ability is currently active.
};
