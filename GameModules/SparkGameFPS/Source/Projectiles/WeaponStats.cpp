/**
 * @file WeaponStats.cpp
 * @brief Implementation of weapon statistics and utility functions
 * @author Spark Engine Team
 * @date 2025
 */

#include "WeaponStats.h"
#include <unordered_map>

// Use the SparkEditor namespace for enum types
using SparkEditor::WeaponType;

WeaponStats GetDefaultWeaponStats(WeaponType type)
{
    switch (type)
    {
    case WeaponType::PISTOL:
        return WeaponStats(WeaponType::PISTOL,
                           25.0f,  // Damage
                           450.0f, // Fire rate (RPM)
                           15,     // Magazine size
                           2.0f,   // Reload time
                           350.0f, // Muzzle velocity
                           0.85f   // Accuracy
        );

    case WeaponType::RIFLE:
        return WeaponStats(WeaponType::RIFLE,
                           35.0f,  // Damage
                           600.0f, // Fire rate (RPM)
                           30,     // Magazine size
                           2.5f,   // Reload time
                           800.0f, // Muzzle velocity
                           0.75f   // Accuracy
        );

    case WeaponType::SHOTGUN:
        return WeaponStats(WeaponType::SHOTGUN,
                           80.0f,  // Damage (per pellet)
                           120.0f, // Fire rate (RPM)
                           8,      // Magazine size
                           3.0f,   // Reload time
                           400.0f, // Muzzle velocity
                           0.45f   // Accuracy (spread pattern)
        );

    case WeaponType::ROCKET_LAUNCHER:
        return WeaponStats(WeaponType::ROCKET_LAUNCHER,
                           200.0f, // Damage
                           60.0f,  // Fire rate (RPM)
                           4,      // Magazine size
                           4.0f,   // Reload time
                           300.0f, // Muzzle velocity
                           0.95f   // Accuracy
        );

    case WeaponType::GRENADE_LAUNCHER:
        return WeaponStats(WeaponType::GRENADE_LAUNCHER,
                           150.0f, // Damage
                           90.0f,  // Fire rate (RPM)
                           6,      // Magazine size
                           3.5f,   // Reload time
                           250.0f, // Muzzle velocity
                           0.70f   // Accuracy
        );

    case WeaponType::SNIPER_RIFLE:
        return WeaponStats(WeaponType::SNIPER_RIFLE,
                           120.0f,  // Damage
                           60.0f,   // Fire rate (RPM)
                           5,       // Magazine size
                           3.5f,    // Reload time
                           1200.0f, // Muzzle velocity
                           0.98f    // Accuracy
        );

    case WeaponType::SUBMACHINE_GUN:
        return WeaponStats(WeaponType::SUBMACHINE_GUN,
                           18.0f,  // Damage
                           900.0f, // Fire rate (RPM)
                           40,     // Magazine size
                           2.2f,   // Reload time
                           300.0f, // Muzzle velocity
                           0.60f   // Accuracy
        );

    case WeaponType::ASSAULT_RIFLE:
        return WeaponStats(WeaponType::ASSAULT_RIFLE,
                           30.0f,  // Damage
                           700.0f, // Fire rate (RPM)
                           30,     // Magazine size
                           2.8f,   // Reload time
                           750.0f, // Muzzle velocity
                           0.70f   // Accuracy
        );

    case WeaponType::MACHINE_GUN:
        return WeaponStats(WeaponType::MACHINE_GUN,
                           40.0f,  // Damage
                           800.0f, // Fire rate (RPM)
                           100,    // Magazine size
                           5.0f,   // Reload time
                           850.0f, // Muzzle velocity
                           0.65f   // Accuracy
        );

    case WeaponType::FLAMETHROWER:
        return WeaponStats(WeaponType::FLAMETHROWER,
                           15.0f,   // Damage per tick
                           1200.0f, // Fire rate (continuous)
                           200,     // Fuel capacity
                           4.0f,    // Refuel time
                           50.0f,   // Stream velocity
                           0.80f    // Accuracy (stream control)
        );

    case WeaponType::PLASMA_RIFLE:
        return WeaponStats(WeaponType::PLASMA_RIFLE,
                           45.0f,  // Damage
                           300.0f, // Fire rate (RPM)
                           20,     // Energy cell capacity
                           3.0f,   // Recharge time
                           600.0f, // Projectile velocity
                           0.88f   // Accuracy
        );

    case WeaponType::LASER_CANNON:
        return WeaponStats(WeaponType::LASER_CANNON,
                           60.0f,  // Damage
                           180.0f, // Fire rate (RPM)
                           12,     // Energy capacity
                           4.0f,   // Recharge time
                           0.0f,   // Instant hit
                           0.95f   // Accuracy
        );

    case WeaponType::RAILGUN:
        return WeaponStats(WeaponType::RAILGUN,
                           180.0f,  // Damage
                           30.0f,   // Fire rate (RPM)
                           3,       // Magazine size
                           5.0f,    // Reload time
                           2000.0f, // Muzzle velocity
                           0.99f    // Accuracy
        );

    case WeaponType::MINIGUN:
        return WeaponStats(WeaponType::MINIGUN,
                           25.0f,   // Damage
                           3000.0f, // Fire rate (RPM)
                           500,     // Magazine size
                           8.0f,    // Reload time
                           600.0f,  // Muzzle velocity
                           0.50f    // Accuracy (spray pattern)
        );

    case WeaponType::CROSSBOW:
        return WeaponStats(WeaponType::CROSSBOW,
                           85.0f,  // Damage
                           45.0f,  // Fire rate (RPM)
                           1,      // Single shot
                           3.0f,   // Reload time
                           400.0f, // Projectile velocity
                           0.90f   // Accuracy
        );

    case WeaponType::BOW:
        return WeaponStats(WeaponType::BOW,
                           50.0f,  // Damage
                           120.0f, // Fire rate (RPM)
                           1,      // Single shot
                           1.5f,   // Nock time
                           300.0f, // Arrow velocity
                           0.85f   // Accuracy
        );

    case WeaponType::THROWING_KNIFE:
        return WeaponStats(WeaponType::THROWING_KNIFE,
                           40.0f,  // Damage
                           180.0f, // Throw rate
                           6,      // Carried knives
                           2.0f,   // Restock time
                           250.0f, // Throw velocity
                           0.75f   // Accuracy
        );

    case WeaponType::MELEE_WEAPON:
        return WeaponStats(WeaponType::MELEE_WEAPON,
                           60.0f,  // Damage
                           120.0f, // Attack rate
                           0,      // No ammo
                           0.0f,   // No reload
                           0.0f,   // No projectile
                           0.95f   // Accuracy (hit chance)
        );

    default:
        // Return pistol as fallback
        return GetDefaultWeaponStats(WeaponType::PISTOL);
    }
}

WeaponStats CreateWeaponStats(WeaponType type, float damage, float fireRate, int magSize, float reloadTime,
                              float velocity, float accuracy)
{
    return WeaponStats(type, damage, fireRate, magSize, reloadTime, velocity, accuracy);
}

const char* WeaponTypeToString(WeaponType type)
{
    static const std::unordered_map<WeaponType, const char*> weaponNames = {
        {WeaponType::PISTOL, "Pistol"},
        {WeaponType::RIFLE, "Rifle"},
        {WeaponType::SHOTGUN, "Shotgun"},
        {WeaponType::ROCKET_LAUNCHER, "Rocket Launcher"},
        {WeaponType::GRENADE_LAUNCHER, "Grenade Launcher"},
        {WeaponType::SNIPER_RIFLE, "Sniper Rifle"},
        {WeaponType::SUBMACHINE_GUN, "Submachine Gun"},
        {WeaponType::ASSAULT_RIFLE, "Assault Rifle"},
        {WeaponType::MACHINE_GUN, "Machine Gun"},
        {WeaponType::FLAMETHROWER, "Flamethrower"},
        {WeaponType::PLASMA_RIFLE, "Plasma Rifle"},
        {WeaponType::LASER_CANNON, "Laser Cannon"},
        {WeaponType::RAILGUN, "Railgun"},
        {WeaponType::MINIGUN, "Minigun"},
        {WeaponType::CROSSBOW, "Crossbow"},
        {WeaponType::BOW, "Bow"},
        {WeaponType::THROWING_KNIFE, "Throwing Knife"},
        {WeaponType::MELEE_WEAPON, "Melee Weapon"},
        {WeaponType::GRAPPLING_HOOK, "Grappling Hook"},
        {WeaponType::SCANNER, "Scanner"},
        {WeaponType::REPAIR_TOOL, "Repair Tool"},
        {WeaponType::MEDICAL_TOOL, "Medical Tool"},
        {WeaponType::CUSTOM_1, "Custom Weapon 1"},
        {WeaponType::CUSTOM_2, "Custom Weapon 2"},
        {WeaponType::CUSTOM_3, "Custom Weapon 3"}};

    auto it = weaponNames.find(type);
    return (it != weaponNames.end()) ? it->second : "Unknown";
}
