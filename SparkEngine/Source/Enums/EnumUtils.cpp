/**
 * @file EnumUtils.cpp
 * @brief Implementation of enum utility functions
 * @author Spark Engine Team
 * @date 2025
 */

#include "EnumUtils.h"
#include "Enums/GameSystemEnums.h"
#include <algorithm>
#include <unordered_map>
#include <cctype>

namespace SparkEditor {

// WeaponType specialization
template<>
std::string EnumUtils<WeaponType>::ToString(WeaponType value) {
    static const std::unordered_map<WeaponType, std::string> weaponNames = {
        { WeaponType::PISTOL, "Pistol" },
        { WeaponType::RIFLE, "Rifle" },
        { WeaponType::SHOTGUN, "Shotgun" },
        { WeaponType::ROCKET_LAUNCHER, "Rocket Launcher" },
        { WeaponType::GRENADE_LAUNCHER, "Grenade Launcher" },
        { WeaponType::SNIPER_RIFLE, "Sniper Rifle" },
        { WeaponType::SUBMACHINE_GUN, "Submachine Gun" },
        { WeaponType::ASSAULT_RIFLE, "Assault Rifle" },
        { WeaponType::MACHINE_GUN, "Machine Gun" },
        { WeaponType::FLAMETHROWER, "Flamethrower" },
        { WeaponType::PLASMA_RIFLE, "Plasma Rifle" },
        { WeaponType::LASER_CANNON, "Laser Cannon" },
        { WeaponType::RAILGUN, "Railgun" },
        { WeaponType::MINIGUN, "Minigun" },
        { WeaponType::CROSSBOW, "Crossbow" },
        { WeaponType::BOW, "Bow" },
        { WeaponType::THROWING_KNIFE, "Throwing Knife" },
        { WeaponType::MELEE_WEAPON, "Melee Weapon" },
        { WeaponType::GRAPPLING_HOOK, "Grappling Hook" },
        { WeaponType::SCANNER, "Scanner" },
        { WeaponType::REPAIR_TOOL, "Repair Tool" },
        { WeaponType::MEDICAL_TOOL, "Medical Tool" },
        { WeaponType::CUSTOM_1, "Custom Weapon 1" },
        { WeaponType::CUSTOM_2, "Custom Weapon 2" },
        { WeaponType::CUSTOM_3, "Custom Weapon 3" }
    };

    auto it = weaponNames.find(value);
    return (it != weaponNames.end()) ? it->second : "Unknown Weapon";
}

template<>
WeaponType EnumUtils<WeaponType>::FromString(const std::string& str) {
    static const std::unordered_map<std::string, WeaponType> nameToType = {
        { "pistol", WeaponType::PISTOL },
        { "rifle", WeaponType::RIFLE },
        { "shotgun", WeaponType::SHOTGUN },
        { "rocket launcher", WeaponType::ROCKET_LAUNCHER },
        { "rocket", WeaponType::ROCKET_LAUNCHER },
        { "grenade launcher", WeaponType::GRENADE_LAUNCHER },
        { "grenade", WeaponType::GRENADE_LAUNCHER },
        { "sniper rifle", WeaponType::SNIPER_RIFLE },
        { "sniper", WeaponType::SNIPER_RIFLE },
        { "submachine gun", WeaponType::SUBMACHINE_GUN },
        { "smg", WeaponType::SUBMACHINE_GUN },
        { "assault rifle", WeaponType::ASSAULT_RIFLE },
        { "assault", WeaponType::ASSAULT_RIFLE },
        { "machine gun", WeaponType::MACHINE_GUN },
        { "mg", WeaponType::MACHINE_GUN },
        { "flamethrower", WeaponType::FLAMETHROWER },
        { "flame", WeaponType::FLAMETHROWER },
        { "plasma rifle", WeaponType::PLASMA_RIFLE },
        { "plasma", WeaponType::PLASMA_RIFLE },
        { "laser cannon", WeaponType::LASER_CANNON },
        { "laser", WeaponType::LASER_CANNON },
        { "railgun", WeaponType::RAILGUN },
        { "rail", WeaponType::RAILGUN },
        { "minigun", WeaponType::MINIGUN },
        { "crossbow", WeaponType::CROSSBOW },
        { "bow", WeaponType::BOW },
        { "throwing knife", WeaponType::THROWING_KNIFE },
        { "knife", WeaponType::THROWING_KNIFE },
        { "melee weapon", WeaponType::MELEE_WEAPON },
        { "melee", WeaponType::MELEE_WEAPON },
        { "grappling hook", WeaponType::GRAPPLING_HOOK },
        { "grapple", WeaponType::GRAPPLING_HOOK },
        { "scanner", WeaponType::SCANNER },
        { "repair tool", WeaponType::REPAIR_TOOL },
        { "repair", WeaponType::REPAIR_TOOL },
        { "medical tool", WeaponType::MEDICAL_TOOL },
        { "medical", WeaponType::MEDICAL_TOOL }
    };

    // Convert to lowercase for case-insensitive matching
    std::string lowerStr = str;
    std::transform(lowerStr.begin(), lowerStr.end(), lowerStr.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    auto it = nameToType.find(lowerStr);
    return (it != nameToType.end()) ? it->second : WeaponType::PISTOL;
}

template<>
std::vector<WeaponType> EnumUtils<WeaponType>::GetAllValues() {
    return {
        WeaponType::PISTOL,
        WeaponType::RIFLE,
        WeaponType::SHOTGUN,
        WeaponType::ROCKET_LAUNCHER,
        WeaponType::GRENADE_LAUNCHER,
        WeaponType::SNIPER_RIFLE,
        WeaponType::SUBMACHINE_GUN,
        WeaponType::ASSAULT_RIFLE,
        WeaponType::MACHINE_GUN,
        WeaponType::FLAMETHROWER,
        WeaponType::PLASMA_RIFLE,
        WeaponType::LASER_CANNON,
        WeaponType::RAILGUN,
        WeaponType::MINIGUN,
        WeaponType::CROSSBOW,
        WeaponType::BOW,
        WeaponType::THROWING_KNIFE,
        WeaponType::MELEE_WEAPON,
        WeaponType::GRAPPLING_HOOK,
        WeaponType::SCANNER,
        WeaponType::REPAIR_TOOL,
        WeaponType::MEDICAL_TOOL,
        WeaponType::CUSTOM_1,
        WeaponType::CUSTOM_2,
        WeaponType::CUSTOM_3
    };
}

// MovementState specialization
template<>
std::string EnumUtils<MovementState>::ToString(MovementState value) {
    static const std::unordered_map<MovementState, std::string> stateNames = {
        { MovementState::IDLE, "Idle" },
        { MovementState::WALKING, "Walking" },
        { MovementState::RUNNING, "Running" },
        { MovementState::SPRINTING, "Sprinting" },
        { MovementState::CROUCHING, "Crouching" },
        { MovementState::CRAWLING, "Crawling" },
        { MovementState::JUMPING, "Jumping" },
        { MovementState::FALLING, "Falling" },
        { MovementState::CLIMBING, "Climbing" },
        { MovementState::SWIMMING, "Swimming" },
        { MovementState::SLIDING, "Sliding" },
        { MovementState::WALL_RUNNING, "Wall Running" },
        { MovementState::GRAPPLING, "Grappling" },
        { MovementState::STUNNED, "Stunned" },
        { MovementState::KNOCKED_DOWN, "Knocked Down" },
        { MovementState::DEAD, "Dead" }
    };

    auto it = stateNames.find(value);
    return (it != stateNames.end()) ? it->second : "Unknown State";
}

template<>
MovementState EnumUtils<MovementState>::FromString(const std::string& str) {
    static const std::unordered_map<std::string, MovementState> nameToState = {
        { "idle", MovementState::IDLE },
        { "walking", MovementState::WALKING },
        { "running", MovementState::RUNNING },
        { "sprinting", MovementState::SPRINTING },
        { "crouching", MovementState::CROUCHING },
        { "crawling", MovementState::CRAWLING },
        { "jumping", MovementState::JUMPING },
        { "falling", MovementState::FALLING },
        { "climbing", MovementState::CLIMBING },
        { "swimming", MovementState::SWIMMING },
        { "sliding", MovementState::SLIDING },
        { "wall running", MovementState::WALL_RUNNING },
        { "grappling", MovementState::GRAPPLING },
        { "stunned", MovementState::STUNNED },
        { "knocked down", MovementState::KNOCKED_DOWN },
        { "dead", MovementState::DEAD }
    };

    std::string lowerStr = str;
    std::transform(lowerStr.begin(), lowerStr.end(), lowerStr.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    auto it = nameToState.find(lowerStr);
    return (it != nameToState.end()) ? it->second : MovementState::IDLE;
}

template<>
std::vector<MovementState> EnumUtils<MovementState>::GetAllValues() {
    return {
        MovementState::IDLE,
        MovementState::WALKING,
        MovementState::RUNNING,
        MovementState::SPRINTING,
        MovementState::CROUCHING,
        MovementState::CRAWLING,
        MovementState::JUMPING,
        MovementState::FALLING,
        MovementState::CLIMBING,
        MovementState::SWIMMING,
        MovementState::SLIDING,
        MovementState::WALL_RUNNING,
        MovementState::GRAPPLING,
        MovementState::STUNNED,
        MovementState::KNOCKED_DOWN,
        MovementState::DEAD
    };
}

// HealthState specialization
template<>
std::string EnumUtils<HealthState>::ToString(HealthState value) {
    static const std::unordered_map<HealthState, std::string> stateNames = {
        { HealthState::HEALTHY, "Healthy" },
        { HealthState::INJURED, "Injured" },
        { HealthState::CRITICAL, "Critical" },
        { HealthState::BLEEDING, "Bleeding" },
        { HealthState::POISONED, "Poisoned" },
        { HealthState::BURNING, "Burning" },
        { HealthState::FROZEN, "Frozen" },
        { HealthState::ELECTRIFIED, "Electrified" },
        { HealthState::DEAD, "Dead" },
        { HealthState::INVULNERABLE, "Invulnerable" },
        { HealthState::REGENERATING, "Regenerating" }
    };

    auto it = stateNames.find(value);
    return (it != stateNames.end()) ? it->second : "Unknown Health State";
}

template<>
HealthState EnumUtils<HealthState>::FromString(const std::string& str) {
    static const std::unordered_map<std::string, HealthState> nameToState = {
        { "healthy", HealthState::HEALTHY },
        { "injured", HealthState::INJURED },
        { "critical", HealthState::CRITICAL },
        { "bleeding", HealthState::BLEEDING },
        { "poisoned", HealthState::POISONED },
        { "burning", HealthState::BURNING },
        { "frozen", HealthState::FROZEN },
        { "electrified", HealthState::ELECTRIFIED },
        { "dead", HealthState::DEAD },
        { "invulnerable", HealthState::INVULNERABLE },
        { "regenerating", HealthState::REGENERATING }
    };

    std::string lowerStr = str;
    std::transform(lowerStr.begin(), lowerStr.end(), lowerStr.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    auto it = nameToState.find(lowerStr);
    return (it != nameToState.end()) ? it->second : HealthState::HEALTHY;
}

template<>
std::vector<HealthState> EnumUtils<HealthState>::GetAllValues() {
    return {
        HealthState::HEALTHY,
        HealthState::INJURED,
        HealthState::CRITICAL,
        HealthState::BLEEDING,
        HealthState::POISONED,
        HealthState::BURNING,
        HealthState::FROZEN,
        HealthState::ELECTRIFIED,
        HealthState::DEAD,
        HealthState::INVULNERABLE,
        HealthState::REGENERATING
    };
}

// DamageType specialization
template<>
std::string EnumUtils<DamageType>::ToString(DamageType value) {
    static const std::unordered_map<DamageType, std::string> typeNames = {
        { DamageType::PHYSICAL, "Physical" },
        { DamageType::FIRE, "Fire" },
        { DamageType::ICE, "Ice" },
        { DamageType::ELECTRIC, "Electric" },
        { DamageType::POISON, "Poison" },
        { DamageType::EXPLOSIVE, "Explosive" },
        { DamageType::ENERGY, "Energy" },
        { DamageType::PSYCHIC, "Psychic" },
        { DamageType::HOLY, "Holy" },
        { DamageType::DARK, "Dark" },
        { DamageType::SONIC, "Sonic" },
        { DamageType::RADIATION, "Radiation" },
        { DamageType::PIERCING, "Piercing" },
        { DamageType::SLASHING, "Slashing" },
        { DamageType::BLUDGEONING, "Bludgeoning" },
        { DamageType::TRUE_DAMAGE, "True Damage" }
    };

    auto it = typeNames.find(value);
    return (it != typeNames.end()) ? it->second : "Unknown Damage Type";
}

template<>
DamageType EnumUtils<DamageType>::FromString(const std::string& str) {
    static const std::unordered_map<std::string, DamageType> nameToType = {
        { "physical", DamageType::PHYSICAL },
        { "fire", DamageType::FIRE },
        { "ice", DamageType::ICE },
        { "electric", DamageType::ELECTRIC },
        { "poison", DamageType::POISON },
        { "explosive", DamageType::EXPLOSIVE },
        { "energy", DamageType::ENERGY },
        { "psychic", DamageType::PSYCHIC },
        { "holy", DamageType::HOLY },
        { "dark", DamageType::DARK },
        { "sonic", DamageType::SONIC },
        { "radiation", DamageType::RADIATION },
        { "piercing", DamageType::PIERCING },
        { "slashing", DamageType::SLASHING },
        { "bludgeoning", DamageType::BLUDGEONING },
        { "true damage", DamageType::TRUE_DAMAGE },
        { "true", DamageType::TRUE_DAMAGE }
    };

    std::string lowerStr = str;
    std::transform(lowerStr.begin(), lowerStr.end(), lowerStr.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    auto it = nameToType.find(lowerStr);
    return (it != nameToType.end()) ? it->second : DamageType::PHYSICAL;
}

template<>
std::vector<DamageType> EnumUtils<DamageType>::GetAllValues() {
    return {
        DamageType::PHYSICAL,
        DamageType::FIRE,
        DamageType::ICE,
        DamageType::ELECTRIC,
        DamageType::POISON,
        DamageType::EXPLOSIVE,
        DamageType::ENERGY,
        DamageType::PSYCHIC,
        DamageType::HOLY,
        DamageType::DARK,
        DamageType::SONIC,
        DamageType::RADIATION,
        DamageType::PIERCING,
        DamageType::SLASHING,
        DamageType::BLUDGEONING,
        DamageType::TRUE_DAMAGE
    };
}

} // namespace SparkEditor

// ============================================================================
// SparkEngine namespace enum specializations
// EnumUtils lives in SparkEditor, but we provide specializations for
// SparkEngine enums so that code including AllEnums.h + EnumUtils.h can use
// EnumUtils<GraphicsAPI>, EnumUtils<InputAction>, etc.
// ============================================================================

#include "GraphicsEnums.h"
#include "InputEnums.h"

namespace SparkEditor {

// --- GraphicsAPI ---
template<>
std::string EnumUtils<SparkEngine::GraphicsAPI>::ToString(SparkEngine::GraphicsAPI value) {
    using SparkEngine::GraphicsAPI;
    static const std::unordered_map<GraphicsAPI, std::string> names = {
        { GraphicsAPI::DIRECTX_11, "DirectX 11" },
        { GraphicsAPI::DIRECTX_12, "DirectX 12" },
        { GraphicsAPI::OPENGL, "OpenGL" },
        { GraphicsAPI::VULKAN, "Vulkan" },
        { GraphicsAPI::METAL, "Metal" }
    };
    auto it = names.find(value);
    return (it != names.end()) ? it->second : "Unknown Graphics API";
}

template<>
SparkEngine::GraphicsAPI EnumUtils<SparkEngine::GraphicsAPI>::FromString(const std::string& str) {
    using SparkEngine::GraphicsAPI;
    static const std::unordered_map<std::string, GraphicsAPI> nameToType = {
        { "directx 11", GraphicsAPI::DIRECTX_11 },
        { "dx11", GraphicsAPI::DIRECTX_11 },
        { "directx 12", GraphicsAPI::DIRECTX_12 },
        { "dx12", GraphicsAPI::DIRECTX_12 },
        { "opengl", GraphicsAPI::OPENGL },
        { "gl", GraphicsAPI::OPENGL },
        { "vulkan", GraphicsAPI::VULKAN },
        { "vk", GraphicsAPI::VULKAN },
        { "metal", GraphicsAPI::METAL }
    };
    std::string lowerStr = str;
    std::transform(lowerStr.begin(), lowerStr.end(), lowerStr.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto it = nameToType.find(lowerStr);
    return (it != nameToType.end()) ? it->second : GraphicsAPI::DIRECTX_11;
}

template<>
std::vector<SparkEngine::GraphicsAPI> EnumUtils<SparkEngine::GraphicsAPI>::GetAllValues() {
    using SparkEngine::GraphicsAPI;
    return {
        GraphicsAPI::DIRECTX_11,
        GraphicsAPI::DIRECTX_12,
        GraphicsAPI::OPENGL,
        GraphicsAPI::VULKAN,
        GraphicsAPI::METAL
    };
}

// --- InputAction ---
template<>
std::string EnumUtils<SparkEngine::InputAction>::ToString(SparkEngine::InputAction value) {
    using SparkEngine::InputAction;
    static const std::unordered_map<InputAction, std::string> names = {
        { InputAction::PRESSED, "Pressed" },
        { InputAction::RELEASED, "Released" },
        { InputAction::HELD, "Held" },
        { InputAction::REPEATED, "Repeated" }
    };
    auto it = names.find(value);
    return (it != names.end()) ? it->second : "Unknown Input Action";
}

template<>
SparkEngine::InputAction EnumUtils<SparkEngine::InputAction>::FromString(const std::string& str) {
    using SparkEngine::InputAction;
    static const std::unordered_map<std::string, InputAction> nameToType = {
        { "pressed", InputAction::PRESSED },
        { "released", InputAction::RELEASED },
        { "held", InputAction::HELD },
        { "repeated", InputAction::REPEATED }
    };
    std::string lowerStr = str;
    std::transform(lowerStr.begin(), lowerStr.end(), lowerStr.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto it = nameToType.find(lowerStr);
    return (it != nameToType.end()) ? it->second : InputAction::PRESSED;
}

template<>
std::vector<SparkEngine::InputAction> EnumUtils<SparkEngine::InputAction>::GetAllValues() {
    using SparkEngine::InputAction;
    return {
        InputAction::PRESSED,
        InputAction::RELEASED,
        InputAction::HELD,
        InputAction::REPEATED
    };
}

} // namespace SparkEditor