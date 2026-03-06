/**
 * @file ClassSystem.h
 * @brief FPS class system inspired by PlanetSide 2 and Battlefield
 * @author Spark Engine Team
 * @date 2025
 *
 * Implements a full class-based loadout and ability system with six distinct
 * classes: Light Assault, Combat Medic, Engineer, Infiltrator, Heavy Assault,
 * and MAX Suit. Each class has unique stats, weapon loadouts, passive traits,
 * and active abilities.
 */

#pragma once
#include "../Core/Platform.h"

#include "Enums/GameSystemEnums.h"
#include "Projectiles/WeaponStats.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

using SparkEditor::PlayerClass;
using SparkEditor::ClassAbility;
using SparkEditor::WeaponType;

namespace Spark {

/**
 * @brief Weapon loadout slot definition
 */
struct LoadoutSlot {
    WeaponType weapon = WeaponType::PISTOL;
    int reserveAmmo = 0;   ///< Extra magazines worth of ammo

    LoadoutSlot() = default;
    LoadoutSlot(WeaponType w, int reserve) : weapon(w), reserveAmmo(reserve) {}
};

/**
 * @brief Full class loadout (weapons + equipment)
 */
struct ClassLoadout {
    LoadoutSlot primary;
    LoadoutSlot secondary;
    LoadoutSlot sidearm;
    LoadoutSlot tool;       ///< Class-specific tool (med tool, repair tool, etc.)

    ClassLoadout() = default;
};

/**
 * @brief Ability runtime state
 */
struct AbilityState {
    ClassAbility type = ClassAbility::NONE;
    float cooldownMax = 10.0f;      ///< Max cooldown in seconds
    float cooldownRemaining = 0.0f; ///< Current cooldown remaining
    float durationMax = 5.0f;       ///< Max active duration
    float durationRemaining = 0.0f; ///< Current active time remaining
    float energyCost = 25.0f;       ///< Energy/resource cost to activate
    bool isActive = false;          ///< Currently active
    bool isReady = true;            ///< Off cooldown and ready to use

    void Update(float dt);
    bool Activate(float currentEnergy);
    void Deactivate();
    void Reset();
    float GetCooldownProgress() const;
};

/**
 * @brief Passive trait that modifies class behavior
 */
struct PassiveTrait {
    std::string name;
    std::string description;
    float value = 0.0f;    ///< Modifier value (multiplier or flat)

    PassiveTrait() = default;
    PassiveTrait(const std::string& n, const std::string& d, float v)
        : name(n), description(d), value(v) {}
};

/**
 * @brief Complete class definition with stats, loadout, abilities, and traits
 */
struct ClassDefinition {
    PlayerClass classType = PlayerClass::LIGHT_ASSAULT;
    std::string name;
    std::string description;

    // Base stats
    float baseHealth = 100.0f;
    float baseArmor = 0.0f;
    float baseShield = 0.0f;        ///< Rechargeable shield (like PS2)
    float shieldRechargeRate = 10.0f;
    float shieldRechargeDelay = 6.0f;
    float baseSpeed = 5.0f;
    float sprintMultiplier = 2.0f;
    float crouchMultiplier = 0.5f;
    float baseJumpHeight = 3.0f;
    float baseStamina = 100.0f;
    float staminaRegenRate = 50.0f;

    // Movement modifiers
    float adsSpeedMultiplier = 0.5f; ///< Speed while aiming down sights
    float strafeSpeedMultiplier = 0.8f;
    bool canSprint = true;

    // Damage modifiers
    float damageResistance = 0.0f;      ///< % damage reduction
    float headshotMultiplier = 2.0f;
    float explosiveResistance = 0.0f;

    // Loadout
    ClassLoadout loadout;

    // Abilities
    AbilityState primaryAbility;
    AbilityState secondaryAbility;

    // Passive traits
    std::vector<PassiveTrait> passives;

    // Allowed weapon types for this class
    std::vector<WeaponType> allowedWeapons;
};

/**
 * @brief Deployable object placed by Engineer or other classes
 */
struct Deployable {
    enum class Type {
        Turret,
        Barrier,
        AmmoPack,
        MedKit,
        SpawnBeacon,
        MotionSensor
    };

    Type type;
    DirectX::XMFLOAT3 position{};
    float health = 100.0f;
    float maxHealth = 100.0f;
    float lifetime = 60.0f;     ///< Seconds until auto-destroy
    float timer = 0.0f;
    bool isActive = true;
    int ownerID = -1;

    void Update(float dt);
    bool IsDead() const { return health <= 0.0f || timer >= lifetime; }
};

/**
 * @brief Main class system manager
 *
 * Handles class definitions, loadout management, ability processing,
 * and class switching. Provides factory methods for creating class
 * configurations and handles class-specific game logic.
 */
class ClassSystem {
public:
    ClassSystem();
    ~ClassSystem() = default;

    /**
     * @brief Initialize the class system with default class definitions
     * @return true on success
     */
    bool Initialize();

    /**
     * @brief Update ability cooldowns and active effects
     * @param dt Delta time
     */
    void Update(float dt);

    // === Class Definitions ===

    /**
     * @brief Get the definition for a specific class
     * @param classType The class to retrieve
     * @return Const reference to the class definition
     */
    const ClassDefinition& GetClassDefinition(PlayerClass classType) const;

    /**
     * @brief Get a mutable reference for customization
     */
    ClassDefinition& GetClassDefinitionMut(PlayerClass classType);

    /**
     * @brief Get all class definitions
     */
    const std::unordered_map<int, ClassDefinition>& GetAllClasses() const { return m_classes; }

    /**
     * @brief Check if a weapon type is allowed for a class
     */
    bool IsWeaponAllowed(PlayerClass classType, WeaponType weaponType) const;

    /**
     * @brief Get the default loadout for a class
     */
    ClassLoadout GetDefaultLoadout(PlayerClass classType) const;

    // === Ability System ===

    /**
     * @brief Activate primary ability for a class
     * @param classType The class activating the ability
     * @param currentEnergy Current energy/resource pool
     * @return true if ability activated successfully
     */
    bool ActivatePrimaryAbility(PlayerClass classType, float currentEnergy);

    /**
     * @brief Activate secondary ability for a class
     */
    bool ActivateSecondaryAbility(PlayerClass classType, float currentEnergy);

    /**
     * @brief Deactivate primary ability
     */
    void DeactivatePrimaryAbility(PlayerClass classType);

    /**
     * @brief Get ability state for a class
     */
    const AbilityState& GetPrimaryAbilityState(PlayerClass classType) const;
    const AbilityState& GetSecondaryAbilityState(PlayerClass classType) const;

    // === Deployables ===

    /**
     * @brief Place a deployable object in the world
     */
    int PlaceDeployable(Deployable::Type type, const DirectX::XMFLOAT3& position, int ownerID);

    /**
     * @brief Get all active deployables
     */
    const std::vector<Deployable>& GetDeployables() const { return m_deployables; }

    /**
     * @brief Remove deployables for a specific owner
     */
    void RemoveDeployables(int ownerID);

    // === Utility ===

    /**
     * @brief Get display name for a class
     */
    static const char* GetClassName(PlayerClass classType);

    /**
     * @brief Get short description for a class
     */
    static const char* GetClassDescription(PlayerClass classType);

    /**
     * @brief Get ability name
     */
    static const char* GetAbilityName(ClassAbility ability);

    /**
     * @brief Get number of classes available
     */
    static int GetClassCount() { return static_cast<int>(PlayerClass::COUNT); }

private:
    void InitLightAssault();
    void InitCombatMedic();
    void InitEngineer();
    void InitInfiltrator();
    void InitHeavyAssault();
    void InitMAXSuit();

    std::unordered_map<int, ClassDefinition> m_classes;
    std::vector<Deployable> m_deployables;
    static constexpr int MAX_DEPLOYABLES = 50;

    // Default definition returned for invalid lookups
    ClassDefinition m_defaultDef;
    AbilityState m_defaultAbility;
};

} // namespace Spark
