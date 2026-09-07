/**
 * @file WeaponStatsOperations.cpp
 * @brief Weapon operations retaining engine diagnostics outside the SDK-only configuration source.
 */

#include "WeaponStats.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

using SparkEditor::WeaponType;

WeaponStats ApplyWeaponModifications(const WeaponStats& baseStats, float damageMultiplier, float fireRateMultiplier,
                                     float accuracyMultiplier, float reloadTimeMultiplier)
{
    SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Applying weapon mods: dmg=%.2fx, fire=%.2fx, acc=%.2fx, reload=%.2fx",
                    damageMultiplier, fireRateMultiplier, accuracyMultiplier, reloadTimeMultiplier);
    WeaponStats modified = baseStats;

    modified.Damage *= damageMultiplier;
    modified.FireRate *= fireRateMultiplier;
    modified.Accuracy = std::min(1.0f, modified.Accuracy * accuracyMultiplier);
    modified.ReloadTime *= reloadTimeMultiplier;

    return modified;
}

WeaponType StringToWeaponType(const char* str)
{
    static const std::unordered_map<std::string, WeaponType> nameToType = {
        {"Pistol", WeaponType::PISTOL},
        {"Rifle", WeaponType::RIFLE},
        {"Shotgun", WeaponType::SHOTGUN},
        {"Rocket Launcher", WeaponType::ROCKET_LAUNCHER},
        {"Grenade Launcher", WeaponType::GRENADE_LAUNCHER},
        {"Sniper Rifle", WeaponType::SNIPER_RIFLE},
        {"Submachine Gun", WeaponType::SUBMACHINE_GUN},
        {"Assault Rifle", WeaponType::ASSAULT_RIFLE},
        {"Machine Gun", WeaponType::MACHINE_GUN},
        {"Flamethrower", WeaponType::FLAMETHROWER},
        {"Plasma Rifle", WeaponType::PLASMA_RIFLE},
        {"Laser Cannon", WeaponType::LASER_CANNON},
        {"Railgun", WeaponType::RAILGUN},
        {"Minigun", WeaponType::MINIGUN},
        {"Crossbow", WeaponType::CROSSBOW},
        {"Bow", WeaponType::BOW},
        {"Throwing Knife", WeaponType::THROWING_KNIFE},
        {"Melee Weapon", WeaponType::MELEE_WEAPON},
        {"Grappling Hook", WeaponType::GRAPPLING_HOOK},
        {"Scanner", WeaponType::SCANNER},
        {"Repair Tool", WeaponType::REPAIR_TOOL},
        {"Medical Tool", WeaponType::MEDICAL_TOOL}};

    // Convert to string and try exact match first
    std::string input(str);
    auto it = nameToType.find(input);
    if (it != nameToType.end())
    {
        return it->second;
    }

    // Try case-insensitive match
    std::string lowerInput = input;
    std::transform(lowerInput.begin(), lowerInput.end(), lowerInput.begin(), ::tolower);

    if (lowerInput == "pistol")
        return WeaponType::PISTOL;
    if (lowerInput == "rifle")
        return WeaponType::RIFLE;
    if (lowerInput == "shotgun")
        return WeaponType::SHOTGUN;
    if (lowerInput == "rocket" || lowerInput == "rocket launcher")
        return WeaponType::ROCKET_LAUNCHER;
    if (lowerInput == "grenade" || lowerInput == "grenade launcher")
        return WeaponType::GRENADE_LAUNCHER;
    if (lowerInput == "sniper" || lowerInput == "sniper rifle")
        return WeaponType::SNIPER_RIFLE;
    if (lowerInput == "smg" || lowerInput == "submachine gun")
        return WeaponType::SUBMACHINE_GUN;
    if (lowerInput == "assault" || lowerInput == "assault rifle")
        return WeaponType::ASSAULT_RIFLE;
    if (lowerInput == "machine gun" || lowerInput == "mg")
        return WeaponType::MACHINE_GUN;
    if (lowerInput == "flamethrower" || lowerInput == "flame")
        return WeaponType::FLAMETHROWER;
    if (lowerInput == "plasma" || lowerInput == "plasma rifle")
        return WeaponType::PLASMA_RIFLE;
    if (lowerInput == "laser" || lowerInput == "laser cannon")
        return WeaponType::LASER_CANNON;
    if (lowerInput == "railgun" || lowerInput == "rail")
        return WeaponType::RAILGUN;
    if (lowerInput == "minigun")
        return WeaponType::MINIGUN;
    if (lowerInput == "crossbow")
        return WeaponType::CROSSBOW;
    if (lowerInput == "bow")
        return WeaponType::BOW;
    if (lowerInput == "knife" || lowerInput == "throwing knife")
        return WeaponType::THROWING_KNIFE;
    if (lowerInput == "melee" || lowerInput == "melee weapon")
        return WeaponType::MELEE_WEAPON;

    // Default fallback
    SPARK_LOG_WARN(Spark::LogCategory::Game, "Unknown weapon type string: %s, defaulting to Pistol", str);
    return WeaponType::PISTOL;
}
