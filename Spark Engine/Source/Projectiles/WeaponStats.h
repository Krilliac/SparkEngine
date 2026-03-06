/**
 * @file WeaponStats.h
 * @brief Weapon configuration and statistics system
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file defines weapon statistics structures and default configurations
 * for the weapon system. It provides a data-driven approach to weapon balancing
 * and configuration management.
 */

#pragma once

#include "../Core/framework.h"
#include "../Enums/GameSystemEnums.h"
#include "../Utils/Assert.h"

/**
 * @brief Weapon statistics and configuration structure
 * 
 * Contains all the parameters that define a weapon's behavior including
 * damage, fire rate, ammunition, and ballistic properties.
 */
struct WeaponStats
{
    SparkEditor::WeaponType Type;         ///< Weapon category/type
    float      Damage;       ///< Base damage per shot/projectile
    float      FireRate;     ///< Rate of fire in rounds per minute
    int        MagazineSize; ///< Number of rounds per magazine/clip
    float      ReloadTime;   ///< Time required to reload in seconds
    float      MuzzleVelocity; ///< Initial projectile speed in units per second
    float      Accuracy;     ///< Accuracy factor (0.0 = completely inaccurate, 1.0 = perfect accuracy)

    /**
     * @brief Default constructor with safe initial values
     */
    WeaponStats()
        : Type(SparkEditor::WeaponType::PISTOL)
        , Damage(10.0f)
        , FireRate(600.0f)
        , MagazineSize(15)
        , ReloadTime(2.0f)
        , MuzzleVelocity(300.0f)
        , Accuracy(0.85f)
    {
    }

    /**
     * @brief Parameterized constructor
     * 
     * @param type Weapon type
     * @param damage Base damage value
     * @param fireRate Rate of fire in RPM
     * @param magSize Magazine capacity
     * @param reloadTime Reload duration in seconds
     * @param velocity Muzzle velocity
     * @param accuracy Accuracy factor (0.0-1.0)
     */
    WeaponStats(SparkEditor::WeaponType type, float damage, float fireRate, int magSize,
               float reloadTime, float velocity, float accuracy)
        : Type(type)
        , Damage(damage)
        , FireRate(fireRate)
        , MagazineSize(magSize)
        , ReloadTime(reloadTime)
        , MuzzleVelocity(velocity)
        , Accuracy(accuracy)
    {
        ASSERT_MSG(damage >= 0.0f, "Weapon damage must be non-negative");
        ASSERT_MSG(fireRate >= 0.0f, "Fire rate must be non-negative");
        ASSERT_MSG(magSize >= 0, "Magazine size must be non-negative");
        ASSERT_MSG(reloadTime >= 0.0f, "Reload time must be non-negative");
        ASSERT_MSG(velocity >= 0.0f, "Muzzle velocity must be non-negative");
        ASSERT_MSG(accuracy >= 0.0f && accuracy <= 1.0f, "Accuracy must be between 0.0 and 1.0");
    }

    /**
     * @brief Calculate time between shots based on fire rate
     * @return Time between shots in seconds
     */
    float GetShotInterval() const
    {
        if (FireRate <= 0.0f) return 1.0f; // Safety fallback
        return 60.0f / FireRate; // Convert RPM to seconds per shot
    }

    /**
     * @brief Get effective range based on muzzle velocity and accuracy
     * @return Effective range in game units
     */
    float GetEffectiveRange() const
    {
        return MuzzleVelocity * Accuracy * 0.1f; // Simple formula
    }

    /**
     * @brief Calculate damage per second (DPS)
     * @return Theoretical maximum damage per second
     */
    float GetDPS() const
    {
        return Damage * (FireRate / 60.0f);
    }

    /**
     * @brief Validate weapon statistics for logical consistency
     * @return true if stats are valid, false otherwise
     */
    bool IsValid() const
    {
        return Damage >= 0.0f &&
               FireRate >= 0.0f &&
               MagazineSize >= 0 &&
               ReloadTime >= 0.0f &&
               MuzzleVelocity >= 0.0f &&
               Accuracy >= 0.0f && Accuracy <= 1.0f;
    }
};

 /**
 * @brief Get default weapon statistics for a given weapon type
 * 
 * Provides balanced default configurations for each weapon type.
 * These values can be used as starting points for weapon balancing.
 * 
 * @param type The weapon type to get defaults for
 * @return WeaponStats structure with default values for the specified type
 */
WeaponStats GetDefaultWeaponStats(SparkEditor::WeaponType type);

/**
 * @brief Create a weapon stats configuration from parameters
 * 
 * Helper function to create weapon stats with validation.
 * 
 * @param type Weapon type
 * @param damage Base damage value
 * @param fireRate Rate of fire in RPM
 * @param magSize Magazine capacity
 * @param reloadTime Reload duration in seconds
 * @param velocity Muzzle velocity
 * @param accuracy Accuracy factor (0.0-1.0)
 * @return WeaponStats structure with specified values
 */
WeaponStats CreateWeaponStats(SparkEditor::WeaponType type, float damage, float fireRate,
                             int magSize, float reloadTime, float velocity, float accuracy);

/**
 * @brief Apply weapon modifications to base stats
 * 
 * Allows for weapon upgrades, attachments, or temporary modifications
 * to be applied to base weapon statistics.
 * 
 * @param baseStats Base weapon statistics
 * @param damageMultiplier Damage modification factor
 * @param fireRateMultiplier Fire rate modification factor
 * @param accuracyMultiplier Accuracy modification factor
 * @param reloadTimeMultiplier Reload time modification factor (lower is better)
 * @return Modified weapon statistics
 */
WeaponStats ApplyWeaponModifications(const WeaponStats& baseStats,
                                   float damageMultiplier = 1.0f,
                                   float fireRateMultiplier = 1.0f,
                                   float accuracyMultiplier = 1.0f,
                                   float reloadTimeMultiplier = 1.0f);

/**
 * @brief Convert weapon type to string representation
 * 
 * @param type Weapon type to convert
 * @return String name of the weapon type
 */
const char* WeaponTypeToString(SparkEditor::WeaponType type);

/**
 * @brief Convert string to weapon type
 * 
 * @param str String representation of weapon type
 * @return Corresponding weapon type, or WeaponType::PISTOL if not found
 */
SparkEditor::WeaponType StringToWeaponType(const char* str);
