/**
 * @file ClassSystem.cpp
 * @brief Implementation of the FPS class system
 * @author Spark Engine Team
 * @date 2025
 */

#include "ClassSystem.h"
#include "Utils/Validate.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;
namespace Spark
{

    // ============================================================================
    // AbilityState Implementation
    // ============================================================================

    void AbilityState::Update(float dt)
    {
        if (isActive)
        {
            durationRemaining -= dt;
            if (durationRemaining <= 0.0f)
            {
                Deactivate();
            }
        }

        if (cooldownRemaining > 0.0f)
        {
            cooldownRemaining -= dt;
            if (cooldownRemaining <= 0.0f)
            {
                cooldownRemaining = 0.0f;
                isReady = true;
            }
        }
    }

    bool AbilityState::Activate(float currentEnergy)
    {
        if (!isReady || isActive || currentEnergy < energyCost)
            return false;

        isActive = true;
        isReady = false;
        durationRemaining = durationMax;
        return true;
    }

    void AbilityState::Deactivate()
    {
        isActive = false;
        durationRemaining = 0.0f;
        cooldownRemaining = cooldownMax;
    }

    void AbilityState::Reset()
    {
        isActive = false;
        isReady = true;
        cooldownRemaining = 0.0f;
        durationRemaining = 0.0f;
    }

    float AbilityState::GetCooldownProgress() const
    {
        if (cooldownMax <= 0.0f)
            return 1.0f;
        return 1.0f - (cooldownRemaining / cooldownMax);
    }

    // ============================================================================
    // Deployable Implementation
    // ============================================================================

    void Deployable::Update(float dt)
    {
        if (!isActive)
            return;
        timer += dt;
        if (IsDead())
        {
            isActive = false;
        }
    }

    // ============================================================================
    // ClassSystem Implementation
    // ============================================================================

    ClassSystem::ClassSystem()
    {
        m_defaultDef.name = "Unknown";
        m_defaultDef.description = "Invalid class";
    }

    bool ClassSystem::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Initializing ClassSystem with 6 classes");
        InitScout();
        InitMedic();
        InitEngineer();
        InitRecon();
        InitVanguard();
        InitTitan();
        return true;
    }

    void ClassSystem::Update(float dt)
    {
        // Update ability cooldowns for all classes
        for (auto& [key, classDef] : m_classes)
        {
            classDef.primaryAbility.Update(dt);
            classDef.secondaryAbility.Update(dt);
        }

        // Update deployables
        for (auto& dep : m_deployables)
        {
            dep.Update(dt);
        }

        // Remove dead deployables
        m_deployables.erase(
            std::remove_if(m_deployables.begin(), m_deployables.end(), [](const Deployable& d) { return !d.isActive; }),
            m_deployables.end());
    }

    // ============================================================================
    // Class Definitions - Spark Engine Showcase Archetypes
    // ============================================================================

    void ClassSystem::InitScout()
    {
        ClassDefinition def;
        def.classType = PlayerClass::SCOUT;
        def.name = "Scout";
        def.description = "Fast and agile class with jetpack for vertical mobility. "
                          "Excels at flanking and ambush tactics.";

        // Stats - fast and mobile, moderate health
        def.baseHealth = 100.0f;
        def.baseArmor = 25.0f;
        def.baseShield = 50.0f;
        def.shieldRechargeRate = 12.0f;
        def.shieldRechargeDelay = 5.0f;
        def.baseSpeed = 6.5f; // Fastest infantry
        def.sprintMultiplier = 2.2f;
        def.crouchMultiplier = 0.5f;
        def.baseJumpHeight = 3.5f; // Higher jump
        def.baseStamina = 120.0f;
        def.staminaRegenRate = 60.0f;

        def.adsSpeedMultiplier = 0.6f;
        def.strafeSpeedMultiplier = 0.9f;

        def.damageResistance = 0.0f;
        def.explosiveResistance = -0.1f; // Slightly weak to explosives

        // Loadout: Assault Rifle primary, SMG secondary, Pistol sidearm
        def.loadout.primary = LoadoutSlot(WeaponType::ASSAULT_RIFLE, 4);
        def.loadout.secondary = LoadoutSlot(WeaponType::SUBMACHINE_GUN, 3);
        def.loadout.sidearm = LoadoutSlot(WeaponType::PISTOL, 2);

        // Abilities: Jetpack + Sprint Boost
        def.primaryAbility.type = ClassAbility::JETPACK;
        def.primaryAbility.cooldownMax = 0.5f; // Short cooldown, fuel-based
        def.primaryAbility.durationMax = 4.0f; // 4 seconds of flight
        def.primaryAbility.energyCost = 10.0f;

        def.secondaryAbility.type = ClassAbility::SPRINT_BOOST;
        def.secondaryAbility.cooldownMax = 15.0f;
        def.secondaryAbility.durationMax = 5.0f;
        def.secondaryAbility.energyCost = 30.0f;

        // Passives
        def.passives.emplace_back("Agile", "25% faster fall recovery", 0.25f);
        def.passives.emplace_back("Lightweight", "Reduced fall damage", 0.5f);

        // Allowed weapons
        def.allowedWeapons = {WeaponType::ASSAULT_RIFLE, WeaponType::SUBMACHINE_GUN, WeaponType::PISTOL,
                              WeaponType::SHOTGUN,       WeaponType::THROWING_KNIFE, WeaponType::MELEE_WEAPON};

        m_classes[static_cast<int>(PlayerClass::SCOUT)] = def;
    }

    void ClassSystem::InitMedic()
    {
        ClassDefinition def;
        def.classType = PlayerClass::MEDIC;
        def.name = "Medic";
        def.description = "Support class that heals and revives allies. "
                          "Well-armed with assault rifles for frontline combat.";

        // Stats - balanced, good sustain
        def.baseHealth = 100.0f;
        def.baseArmor = 50.0f;
        def.baseShield = 50.0f;
        def.shieldRechargeRate = 15.0f; // Faster shield regen
        def.shieldRechargeDelay = 4.0f;
        def.baseSpeed = 5.5f;
        def.sprintMultiplier = 2.0f;
        def.crouchMultiplier = 0.5f;
        def.baseJumpHeight = 3.0f;
        def.baseStamina = 100.0f;
        def.staminaRegenRate = 55.0f;

        def.adsSpeedMultiplier = 0.5f;
        def.strafeSpeedMultiplier = 0.8f;

        def.damageResistance = 0.05f; // Slight damage resistance
        def.explosiveResistance = 0.0f;

        // Loadout: AR primary, Pistol secondary, Medical tool
        def.loadout.primary = LoadoutSlot(WeaponType::ASSAULT_RIFLE, 5);
        def.loadout.secondary = LoadoutSlot(WeaponType::PISTOL, 3);
        def.loadout.sidearm = LoadoutSlot(WeaponType::PISTOL, 2);
        def.loadout.tool = LoadoutSlot(WeaponType::MEDICAL_TOOL, 0);

        // Abilities: Heal Aura + Revive
        def.primaryAbility.type = ClassAbility::HEAL_AURA;
        def.primaryAbility.cooldownMax = 20.0f;
        def.primaryAbility.durationMax = 8.0f; // 8 second healing pulse
        def.primaryAbility.energyCost = 35.0f;

        def.secondaryAbility.type = ClassAbility::REVIVE;
        def.secondaryAbility.cooldownMax = 30.0f;
        def.secondaryAbility.durationMax = 2.0f; // 2 second channel
        def.secondaryAbility.energyCost = 50.0f;

        // Passives
        def.passives.emplace_back("Combat Surgeon", "Passive health regen: 2 HP/s", 2.0f);
        def.passives.emplace_back("Triage", "Nearby allies heal 50% faster", 0.5f);

        // Allowed weapons
        def.allowedWeapons = {WeaponType::ASSAULT_RIFLE,  WeaponType::RIFLE,        WeaponType::PISTOL,
                              WeaponType::SUBMACHINE_GUN, WeaponType::MEDICAL_TOOL, WeaponType::MELEE_WEAPON};

        m_classes[static_cast<int>(PlayerClass::MEDIC)] = def;
    }

    void ClassSystem::InitEngineer()
    {
        ClassDefinition def;
        def.classType = PlayerClass::ENGINEER;
        def.name = "Engineer";
        def.description = "Builder and repair specialist. Deploys turrets and barriers, "
                          "excels at area denial and vehicle support.";

        // Stats - moderate all-around
        def.baseHealth = 100.0f;
        def.baseArmor = 50.0f;
        def.baseShield = 50.0f;
        def.shieldRechargeRate = 10.0f;
        def.shieldRechargeDelay = 6.0f;
        def.baseSpeed = 5.0f;
        def.sprintMultiplier = 1.8f;
        def.crouchMultiplier = 0.5f;
        def.baseJumpHeight = 2.8f;
        def.baseStamina = 100.0f;
        def.staminaRegenRate = 50.0f;

        def.adsSpeedMultiplier = 0.5f;
        def.strafeSpeedMultiplier = 0.8f;

        def.damageResistance = 0.05f;
        def.explosiveResistance = 0.15f; // Good explosive resistance

        // Loadout: SMG/Shotgun primary, Pistol secondary, Repair tool
        def.loadout.primary = LoadoutSlot(WeaponType::SUBMACHINE_GUN, 4);
        def.loadout.secondary = LoadoutSlot(WeaponType::SHOTGUN, 3);
        def.loadout.sidearm = LoadoutSlot(WeaponType::PISTOL, 2);
        def.loadout.tool = LoadoutSlot(WeaponType::REPAIR_TOOL, 0);

        // Abilities: Deploy Turret + Deploy Barrier
        def.primaryAbility.type = ClassAbility::DEPLOY_TURRET;
        def.primaryAbility.cooldownMax = 45.0f;
        def.primaryAbility.durationMax = 0.5f; // Instant placement
        def.primaryAbility.energyCost = 40.0f;

        def.secondaryAbility.type = ClassAbility::DEPLOY_BARRIER;
        def.secondaryAbility.cooldownMax = 20.0f;
        def.secondaryAbility.durationMax = 0.5f;
        def.secondaryAbility.energyCost = 20.0f;

        // Passives
        def.passives.emplace_back("Flak Armor", "15% less explosive damage", 0.15f);
        def.passives.emplace_back("Resourceful", "Ammo packs restore faster", 0.25f);

        // Allowed weapons
        def.allowedWeapons = {WeaponType::SUBMACHINE_GUN, WeaponType::SHOTGUN,     WeaponType::PISTOL,
                              WeaponType::ASSAULT_RIFLE,  WeaponType::REPAIR_TOOL, WeaponType::MELEE_WEAPON};

        m_classes[static_cast<int>(PlayerClass::ENGINEER)] = def;
    }

    void ClassSystem::InitRecon()
    {
        ClassDefinition def;
        def.classType = PlayerClass::RECON;
        def.name = "Recon";
        def.description = "Stealth reconnaissance class with cloaking technology. "
                          "Deadly at range with sniper rifles, fragile up close.";

        // Stats - low health, high mobility, stealth-focused
        def.baseHealth = 80.0f; // Lowest health
        def.baseArmor = 0.0f;   // No armor
        def.baseShield = 40.0f;
        def.shieldRechargeRate = 8.0f;
        def.shieldRechargeDelay = 8.0f;
        def.baseSpeed = 6.0f;
        def.sprintMultiplier = 2.1f;
        def.crouchMultiplier = 0.4f; // Quieter crouch movement
        def.baseJumpHeight = 3.0f;
        def.baseStamina = 110.0f;
        def.staminaRegenRate = 55.0f;

        def.adsSpeedMultiplier = 0.3f; // Slow while scoped
        def.strafeSpeedMultiplier = 0.7f;

        def.damageResistance = -0.1f; // Takes MORE damage
        def.explosiveResistance = -0.15f;
        def.headshotMultiplier = 2.5f; // Bonus headshot damage dealt

        // Loadout: Sniper primary, SMG secondary, Pistol sidearm, Scanner tool
        def.loadout.primary = LoadoutSlot(WeaponType::SNIPER_RIFLE, 3);
        def.loadout.secondary = LoadoutSlot(WeaponType::SUBMACHINE_GUN, 3);
        def.loadout.sidearm = LoadoutSlot(WeaponType::PISTOL, 2);
        def.loadout.tool = LoadoutSlot(WeaponType::SCANNER, 0);

        // Abilities: Cloak + Recon Dart
        def.primaryAbility.type = ClassAbility::CLOAK;
        def.primaryAbility.cooldownMax = 8.0f;
        def.primaryAbility.durationMax = 12.0f; // 12 seconds of cloak
        def.primaryAbility.energyCost = 20.0f;

        def.secondaryAbility.type = ClassAbility::SENSOR_PULSE;
        def.secondaryAbility.cooldownMax = 25.0f;
        def.secondaryAbility.durationMax = 15.0f; // 15 second scan duration
        def.secondaryAbility.energyCost = 15.0f;

        // Passives
        def.passives.emplace_back("Silent Movement", "Reduced footstep audio", 0.6f);
        def.passives.emplace_back("Marksman", "10% bonus headshot damage", 0.1f);

        // Allowed weapons
        def.allowedWeapons = {WeaponType::SNIPER_RIFLE, WeaponType::SUBMACHINE_GUN, WeaponType::PISTOL,
                              WeaponType::CROSSBOW,     WeaponType::THROWING_KNIFE, WeaponType::SCANNER,
                              WeaponType::MELEE_WEAPON};

        m_classes[static_cast<int>(PlayerClass::RECON)] = def;
    }

    void ClassSystem::InitVanguard()
    {
        ClassDefinition def;
        def.classType = PlayerClass::VANGUARD;
        def.name = "Vanguard";
        def.description = "Frontline tank class with energy shield and heavy weapons. "
                          "Dominates direct combat but trades mobility for power.";

        // Stats - tanky, slow, high firepower
        def.baseHealth = 100.0f;
        def.baseArmor = 75.0f;  // High armor
        def.baseShield = 75.0f; // Extra shield
        def.shieldRechargeRate = 8.0f;
        def.shieldRechargeDelay = 7.0f;
        def.baseSpeed = 4.5f; // Slowest infantry
        def.sprintMultiplier = 1.7f;
        def.crouchMultiplier = 0.4f;
        def.baseJumpHeight = 2.5f; // Lower jump
        def.baseStamina = 80.0f;   // Less stamina
        def.staminaRegenRate = 35.0f;

        def.adsSpeedMultiplier = 0.4f;
        def.strafeSpeedMultiplier = 0.65f;

        def.damageResistance = 0.15f; // 15% damage resistance
        def.explosiveResistance = 0.10f;

        // Loadout: LMG primary, Rocket secondary, Pistol sidearm
        def.loadout.primary = LoadoutSlot(WeaponType::MACHINE_GUN, 3);
        def.loadout.secondary = LoadoutSlot(WeaponType::ROCKET_LAUNCHER, 2);
        def.loadout.sidearm = LoadoutSlot(WeaponType::PISTOL, 2);

        // Abilities: Energy Shield + Rocket Barrage
        def.primaryAbility.type = ClassAbility::ENERGY_SHIELD;
        def.primaryAbility.cooldownMax = 15.0f;
        def.primaryAbility.durationMax = 10.0f; // 10 second energy shield
        def.primaryAbility.energyCost = 30.0f;

        def.secondaryAbility.type = ClassAbility::ROCKET_BARRAGE;
        def.secondaryAbility.cooldownMax = 60.0f;
        def.secondaryAbility.durationMax = 3.0f;
        def.secondaryAbility.energyCost = 60.0f;

        // Passives
        def.passives.emplace_back("Juggernaut", "15% damage resistance", 0.15f);
        def.passives.emplace_back("Heavy Lifting", "No movement penalty with LMGs", 0.0f);

        // Allowed weapons
        def.allowedWeapons = {WeaponType::MACHINE_GUN,   WeaponType::ROCKET_LAUNCHER, WeaponType::PISTOL,
                              WeaponType::ASSAULT_RIFLE, WeaponType::MINIGUN,         WeaponType::SHOTGUN,
                              WeaponType::MELEE_WEAPON};

        m_classes[static_cast<int>(PlayerClass::VANGUARD)] = def;
    }

    void ClassSystem::InitTitan()
    {
        ClassDefinition def;
        def.classType = PlayerClass::TITAN;
        def.name = "Titan";
        def.description = "Mechanized assault exosuit with extreme armor and dual weapons. "
                          "Walking fortress that dominates infantry combat.";

        // Stats - extremely tanky, very slow
        def.baseHealth = 200.0f;     // Double health
        def.baseArmor = 200.0f;      // Double armor
        def.baseShield = 0.0f;       // No rechargeable shield
        def.baseSpeed = 3.5f;        // Very slow
        def.sprintMultiplier = 1.3f; // Can barely sprint
        def.crouchMultiplier = 0.3f;
        def.baseJumpHeight = 1.5f; // Heavy, low jump
        def.baseStamina = 60.0f;
        def.staminaRegenRate = 25.0f;
        def.canSprint = false; // Cannot sprint normally

        def.adsSpeedMultiplier = 0.3f;
        def.strafeSpeedMultiplier = 0.5f;

        def.damageResistance = 0.30f; // 30% damage resistance
        def.explosiveResistance = 0.20f;
        def.headshotMultiplier = 1.5f; // Reduced headshot vulnerability

        // Loadout: Dual heavy weapons
        def.loadout.primary = LoadoutSlot(WeaponType::MINIGUN, 3);
        def.loadout.secondary = LoadoutSlot(WeaponType::GRENADE_LAUNCHER, 2);

        // Abilities: Lockdown + Emergency Repair
        def.primaryAbility.type = ClassAbility::LOCKDOWN;
        def.primaryAbility.cooldownMax = 30.0f;
        def.primaryAbility.durationMax = 15.0f; // 15 seconds locked in place
        def.primaryAbility.energyCost = 25.0f;

        def.secondaryAbility.type = ClassAbility::EMERGENCY_REPAIR;
        def.secondaryAbility.cooldownMax = 45.0f;
        def.secondaryAbility.durationMax = 3.0f; // 3 second repair burst
        def.secondaryAbility.energyCost = 50.0f;

        // Passives
        def.passives.emplace_back("Armored Plating", "30% damage resistance", 0.30f);
        def.passives.emplace_back("Intimidating", "Enemies within 5m have reduced accuracy", 0.1f);

        // Allowed weapons - heavy weapons only
        def.allowedWeapons = {WeaponType::MINIGUN,         WeaponType::MACHINE_GUN,  WeaponType::GRENADE_LAUNCHER,
                              WeaponType::ROCKET_LAUNCHER, WeaponType::FLAMETHROWER, WeaponType::PLASMA_RIFLE,
                              WeaponType::MELEE_WEAPON};

        m_classes[static_cast<int>(PlayerClass::TITAN)] = def;
    }

    // ============================================================================
    // Class Query Methods
    // ============================================================================

    const ClassDefinition& ClassSystem::GetClassDefinition(PlayerClass classType) const
    {
        auto it = m_classes.find(static_cast<int>(classType));
        if (it != m_classes.end())
            return it->second;
        return m_defaultDef;
    }

    ClassDefinition& ClassSystem::GetClassDefinitionMut(PlayerClass classType)
    {
        auto it = m_classes.find(static_cast<int>(classType));
        if (it != m_classes.end())
            return it->second;
        return m_defaultDef;
    }

    bool ClassSystem::IsWeaponAllowed(PlayerClass classType, WeaponType weaponType) const
    {
        const auto& def = GetClassDefinition(classType);
        for (auto w : def.allowedWeapons)
        {
            if (w == weaponType)
                return true;
        }
        return false;
    }

    ClassLoadout ClassSystem::GetDefaultLoadout(PlayerClass classType) const
    {
        return GetClassDefinition(classType).loadout;
    }

    // ============================================================================
    // Ability System
    // ============================================================================

    bool ClassSystem::ActivatePrimaryAbility(PlayerClass classType, float currentEnergy)
    {
        auto it = m_classes.find(static_cast<int>(classType));
        if (it == m_classes.end())
            return false;
        return it->second.primaryAbility.Activate(currentEnergy);
    }

    bool ClassSystem::ActivateSecondaryAbility(PlayerClass classType, float currentEnergy)
    {
        auto it = m_classes.find(static_cast<int>(classType));
        if (it == m_classes.end())
            return false;
        return it->second.secondaryAbility.Activate(currentEnergy);
    }

    void ClassSystem::DeactivatePrimaryAbility(PlayerClass classType)
    {
        auto it = m_classes.find(static_cast<int>(classType));
        if (it != m_classes.end())
        {
            it->second.primaryAbility.Deactivate();
        }
    }

    const AbilityState& ClassSystem::GetPrimaryAbilityState(PlayerClass classType) const
    {
        auto it = m_classes.find(static_cast<int>(classType));
        if (it != m_classes.end())
            return it->second.primaryAbility;
        return m_defaultAbility;
    }

    const AbilityState& ClassSystem::GetSecondaryAbilityState(PlayerClass classType) const
    {
        auto it = m_classes.find(static_cast<int>(classType));
        if (it != m_classes.end())
            return it->second.secondaryAbility;
        return m_defaultAbility;
    }

    // ============================================================================
    // Deployables
    // ============================================================================

    int ClassSystem::PlaceDeployable(Deployable::Type type, const DirectX::XMFLOAT3& position, int ownerID)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_WARN_IF(Spark::LogCategory::Game, static_cast<int>(m_deployables.size()) >= MAX_DEPLOYABLES - 1,
                      "Deployable count near maximum capacity");
        if (m_deployables.size() >= static_cast<size_t>(MAX_DEPLOYABLES))
            return -1;

        Deployable dep;
        dep.type = type;
        dep.position = position;
        dep.ownerID = ownerID;
        dep.isActive = true;

        switch (type)
        {
        case Deployable::Type::Turret:
            dep.health = 500.0f;
            dep.maxHealth = 500.0f;
            dep.lifetime = 120.0f;
            break;
        case Deployable::Type::Barrier:
            dep.health = 1000.0f;
            dep.maxHealth = 1000.0f;
            dep.lifetime = 90.0f;
            break;
        case Deployable::Type::AmmoPack:
            dep.health = 100.0f;
            dep.maxHealth = 100.0f;
            dep.lifetime = 60.0f;
            break;
        case Deployable::Type::MedKit:
            dep.health = 100.0f;
            dep.maxHealth = 100.0f;
            dep.lifetime = 60.0f;
            break;
        case Deployable::Type::SpawnBeacon:
            dep.health = 200.0f;
            dep.maxHealth = 200.0f;
            dep.lifetime = 180.0f;
            break;
        case Deployable::Type::MotionSensor:
            dep.health = 50.0f;
            dep.maxHealth = 50.0f;
            dep.lifetime = 45.0f;
            break;
        }

        m_deployables.push_back(dep);
        return static_cast<int>(m_deployables.size()) - 1;
    }

    void ClassSystem::RemoveDeployables(int ownerID)
    {
        m_deployables.erase(std::remove_if(m_deployables.begin(), m_deployables.end(),
                                           [ownerID](const Deployable& d) { return d.ownerID == ownerID; }),
                            m_deployables.end());
    }

    // ============================================================================
    // Utility - Static String Methods
    // ============================================================================

    const char* ClassSystem::GetClassName(PlayerClass classType)
    {
        switch (classType)
        {
        case PlayerClass::SCOUT:
            return "Scout";
        case PlayerClass::MEDIC:
            return "Medic";
        case PlayerClass::ENGINEER:
            return "Engineer";
        case PlayerClass::RECON:
            return "Recon";
        case PlayerClass::VANGUARD:
            return "Vanguard";
        case PlayerClass::TITAN:
            return "Titan";
        default:
            return "Unknown";
        }
    }

    const char* ClassSystem::GetClassDescription(PlayerClass classType)
    {
        switch (classType)
        {
        case PlayerClass::SCOUT:
            return "Fast and agile with jetpack. Flanking specialist.";
        case PlayerClass::MEDIC:
            return "Heals and revives allies. Frontline support.";
        case PlayerClass::ENGINEER:
            return "Deploys turrets and barriers. Area denial expert.";
        case PlayerClass::RECON:
            return "Stealth recon with sniper rifles. Intel gatherer.";
        case PlayerClass::VANGUARD:
            return "Tanky frontline with LMGs and energy shield.";
        case PlayerClass::TITAN:
            return "Mechanized exosuit. Walking fortress.";
        default:
            return "Unknown class.";
        }
    }

    const char* ClassSystem::GetAbilityName(ClassAbility ability)
    {
        switch (ability)
        {
        case ClassAbility::JETPACK:
            return "Jetpack";
        case ClassAbility::SPRINT_BOOST:
            return "Sprint Boost";
        case ClassAbility::HEAL_AURA:
            return "Healing Aura";
        case ClassAbility::REVIVE:
            return "Revive";
        case ClassAbility::DEPLOY_TURRET:
            return "Deploy Turret";
        case ClassAbility::DEPLOY_BARRIER:
            return "Deploy Barrier";
        case ClassAbility::CLOAK:
            return "Cloak";
        case ClassAbility::SENSOR_PULSE:
            return "Sensor Pulse";
        case ClassAbility::ENERGY_SHIELD:
            return "Energy Shield";
        case ClassAbility::ROCKET_BARRAGE:
            return "Rocket Barrage";
        case ClassAbility::LOCKDOWN:
            return "Lockdown";
        case ClassAbility::EMERGENCY_REPAIR:
            return "Emergency Repair";
        default:
            return "None";
        }
    }

} // namespace Spark
