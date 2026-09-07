/**
 * @file WeaponTypes.h
 * @brief Shared weapon categories used by engine, editor, and game modules.
 *
 * Namespace, underlying type, and numeric values preserve the existing game
 * module ABI. COUNT is the legacy sentinel 103, not a contiguous category count.
 */

#pragma once

namespace SparkEditor
{
    /**
 * @brief Weapon types supported by the game system
 *
 * Comprehensive list of weapon categories with distinct characteristics
 * and behaviors. Each weapon type has associated default statistics
 * defined in WeaponStats.h.
 */
    enum class WeaponType
    {
        PISTOL = 0,           ///< Semi-automatic pistol with moderate damage and high accuracy
        RIFLE = 1,            ///< Battle rifle with high damage and moderate accuracy
        SHOTGUN = 2,          ///< Close-range weapon with high damage but low accuracy
        ROCKET_LAUNCHER = 3,  ///< Explosive weapon with very high damage but slow fire rate
        GRENADE_LAUNCHER = 4, ///< Area-of-effect weapon with explosive projectiles
        SNIPER_RIFLE = 5,     ///< Long-range precision weapon with very high damage
        SUBMACHINE_GUN = 6,   ///< High rate of fire weapon with moderate damage
        ASSAULT_RIFLE = 7,    ///< Versatile automatic weapon balanced for combat
        MACHINE_GUN = 8,      ///< Heavy automatic weapon with sustained fire capability
        FLAMETHROWER = 9,     ///< Close-range continuous damage weapon
        PLASMA_RIFLE = 10,    ///< Energy weapon with unique projectile properties
        LASER_CANNON = 11,    ///< Instantaneous hit-scan energy weapon
        RAILGUN = 12,         ///< Electromagnetic projectile weapon with piercing capability
        MINIGUN = 13,         ///< Rotary cannon with extremely high rate of fire
        CROSSBOW = 14,        ///< Projectile weapon with special bolt types
        BOW = 15,             ///< Traditional ranged weapon with arrow projectiles
        THROWING_KNIFE = 16,  ///< Thrown melee weapon
        MELEE_WEAPON = 17,    ///< Close combat weapons (sword, axe, etc.)

        // Special/Utility weapons
        GRAPPLING_HOOK = 50, ///< Utility weapon for traversal
        SCANNER = 51,        ///< Detection and analysis tool
        REPAIR_TOOL = 52,    ///< Engineering/repair equipment
        MEDICAL_TOOL = 53,   ///< Healing and medical equipment

        // Custom/Modded weapons
        CUSTOM_1 = 100, ///< Custom weapon slot 1
        CUSTOM_2 = 101, ///< Custom weapon slot 2
        CUSTOM_3 = 102, ///< Custom weapon slot 3

        COUNT ///< Total number of weapon types (keep last)
    };

} // namespace SparkEditor
