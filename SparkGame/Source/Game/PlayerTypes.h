/**
 * @file PlayerTypes.h
 * @brief Type definitions used by the Player class
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains standalone data types extracted from Player.h to reduce header
 * coupling and allow other systems to reference player types without pulling
 * in the full Player class definition.
 */

#pragma once

#include "Core/Platform.h"
#include "ClassSystem.h"
#include "Projectiles/WeaponStats.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS

using SparkEditor::WeaponType;

/**
 * @brief Get comprehensive player state for console display
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
