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
#include <DirectXMath.h>
#endif

#include "ClassSystem.h"
#include "Projectiles/WeaponStats.h"

using SparkEditor::WeaponType;

/**
 * @brief Comprehensive player state for console display and serialization
 */
struct PlayerState
{
    float health, maxHealth, armor, maxArmor, stamina, maxStamina;
    float shield, maxShield, energy, maxEnergy;
    DirectX::XMFLOAT3 position, velocity;
    WeaponType currentWeapon;
    int currentAmmo, maxAmmo;
    bool isAlive, isGrounded, isReloading, isRunning, isCrouching;
    bool godMode, noclip, infiniteAmmo;
    float fireTimer, reloadTimer;
    float speed, jumpHeight;
    PlayerClass playerClass;
    int activeLoadoutSlot;
    bool primaryAbilityActive, secondaryAbilityActive;
};
