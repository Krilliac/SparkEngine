/**
 * @file WeaponManager.h
 * @brief Central weapon management system for FPS gameplay
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a complete weapon system for first-person shooters including:
 * - Weapon inventory per entity (carry, switch, drop)
 * - Ammo tracking with reserve pools
 * - Fire modes (Auto, Burst, Semi-auto)
 * - Reload state machine
 * - Recoil patterns and spread
 * - ADS (aim-down-sights) FOV transitions
 * - Weapon switching with equip/holster timing
 *
 * Designed as an ECS component + system pair that integrates with
 * ProjectileComponent for actual projectile spawning.
 */

#pragma once

#include "../../Core/Platform.h"
#include "../../Utils/Delegate.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Spark::Gameplay
{

    // ============================================================================
    // Weapon Enums
    // ============================================================================

    enum class FireMode : uint8_t
    {
        SemiAuto, ///< One shot per trigger pull
        Burst,    ///< Fixed burst (e.g., 3-round burst)
        FullAuto  ///< Continuous fire while trigger held
    };

    enum class WeaponState : uint8_t
    {
        Idle,      ///< Ready to fire
        Firing,    ///< Currently firing (cooldown active)
        Reloading, ///< Magazine swap in progress
        Switching, ///< Equip/holster transition
        Empty,     ///< Magazine empty, needs reload
        Disabled   ///< Cannot fire (ability disabled, etc.)
    };

    enum class WeaponSlot : uint8_t
    {
        Primary = 0,
        Secondary = 1,
        Melee = 2,
        Grenade = 3,
        MaxSlots = 4
    };

    // ============================================================================
    // Recoil Pattern
    // ============================================================================

    /**
     * @brief Defines how a weapon's aim deviates during sustained fire
     */
    struct RecoilPattern
    {
        float verticalPerShot = 0.5f;     ///< Degrees of upward kick per shot
        float horizontalPerShot = 0.1f;   ///< Max degrees of horizontal deviation
        float recoverySpeed = 5.0f;       ///< Degrees per second of recovery toward center
        float maxVertical = 10.0f;        ///< Maximum accumulated vertical recoil
        float maxHorizontal = 3.0f;       ///< Maximum accumulated horizontal recoil
        float firstShotMultiplier = 1.5f; ///< Multiplier for the first shot in a burst
    };

    /**
     * @brief Spread configuration for bullet deviation
     */
    struct SpreadConfig
    {
        float baseSpread = 1.0f;      ///< Base spread in degrees when standing still
        float moveSpread = 3.0f;      ///< Additional spread while moving
        float crouchReduction = 0.5f; ///< Multiplier when crouching (0.5 = 50% less spread)
        float adsReduction = 0.3f;    ///< Multiplier when aiming down sights
        float sprintPenalty = 2.0f;   ///< Additional spread from sprinting
        float maxSpread = 15.0f;      ///< Absolute maximum spread
    };

    // ============================================================================
    // Weapon Definition (data-driven, shared across instances)
    // ============================================================================

    /**
     * @brief Complete definition of a weapon type
     *
     * This is the "template" data for a weapon. Each weapon instance (in an
     * entity's inventory) references a WeaponDefinition by ID and tracks its
     * own runtime state (ammo count, recoil accumulation, etc.).
     */
    struct WeaponDefinition
    {
        uint32_t definitionID = 0; ///< Unique ID for this weapon type
        std::string name;          ///< Display name ("AK-47", "Desert Eagle")
        WeaponSlot slot = WeaponSlot::Primary;

        // Damage
        float baseDamage = 25.0f;        ///< Damage per projectile
        float headshotMultiplier = 2.0f; ///< Headshot damage multiplier
        float armorPenetration = 0.0f;   ///< 0.0 = no penetration, 1.0 = full

        // Fire
        FireMode fireMode = FireMode::FullAuto;
        float fireRate = 600.0f;       ///< Rounds per minute
        int burstCount = 3;            ///< Rounds per burst (only used in Burst mode)
        float muzzleVelocity = 900.0f; ///< Projectile speed (m/s), 0 = hitscan
        bool isHitscan = true;         ///< true = raycast, false = ballistic projectile

        // Ammo
        int magazineSize = 30;           ///< Rounds per magazine
        int maxReserveAmmo = 120;        ///< Maximum reserve ammo
        float reloadTime = 2.5f;         ///< Seconds to reload full magazine
        float tacticalReloadTime = 2.0f; ///< Seconds for reload with round chambered

        // Handling
        float equipTime = 0.5f;           ///< Seconds to draw/equip the weapon
        float holsterTime = 0.3f;         ///< Seconds to put away
        float adsTime = 0.2f;             ///< Seconds to enter aim-down-sights
        float adsFOV = 50.0f;             ///< Field of view when ADS (degrees)
        float moveSpeedMultiplier = 1.0f; ///< Movement speed penalty while equipped

        // Recoil & Spread
        RecoilPattern recoil;
        SpreadConfig spread;

        // Helpers
        float GetShotInterval() const { return fireRate > 0.0f ? 60.0f / fireRate : 1.0f; }

        float GetDPS() const { return baseDamage * (fireRate / 60.0f); }
    };

    // ============================================================================
    // Weapon Instance (per-entity runtime state)
    // ============================================================================

    /**
     * @brief Runtime state for a single weapon in an entity's inventory
     */
    struct WeaponInstance
    {
        uint32_t definitionID = 0; ///< References a WeaponDefinition
        int currentAmmo = 0;       ///< Rounds in current magazine
        int reserveAmmo = 0;       ///< Reserve ammunition
        WeaponState state = WeaponState::Idle;
        float stateTimer = 0.0f; ///< Countdown for current state transition

        // Recoil accumulation
        float accumulatedVerticalRecoil = 0.0f;
        float accumulatedHorizontalRecoil = 0.0f;

        // ADS state
        bool isADS = false;
        float adsBlend = 0.0f; ///< 0.0 = hip, 1.0 = fully ADS

        // Fire control
        bool triggerHeld = false;
        int burstShotsRemaining = 0; ///< Remaining shots in current burst
        float fireCooldown = 0.0f;   ///< Time until next shot is allowed

        bool HasAmmo() const { return currentAmmo > 0; }
        bool CanReload(int magazineSize) const { return currentAmmo < magazineSize && reserveAmmo > 0; }
    };

    // ============================================================================
    // Weapon Inventory Component (attach to player entities)
    // ============================================================================

    /**
     * @brief ECS component that gives an entity a weapon inventory
     *
     * Attach this to any entity that can carry and use weapons.
     * The WeaponSystem processes entities with this component.
     */
    struct WeaponInventoryComponent
    {
        static constexpr size_t MAX_WEAPONS = static_cast<size_t>(WeaponSlot::MaxSlots);

        std::array<WeaponInstance, MAX_WEAPONS> weapons{};
        WeaponSlot activeSlot = WeaponSlot::Primary;
        WeaponSlot pendingSlot = WeaponSlot::Primary; ///< Slot being switched to

        // Input state (set by input system, consumed by weapon system)
        bool inputFire = false;
        bool inputReload = false;
        bool inputADS = false;
        int inputSwitchSlot = -1; ///< -1 = no switch, 0-3 = slot index

        WeaponInstance& GetActiveWeapon() { return weapons[static_cast<size_t>(activeSlot)]; }
        const WeaponInstance& GetActiveWeapon() const { return weapons[static_cast<size_t>(activeSlot)]; }

        bool HasWeaponInSlot(WeaponSlot slot) const { return weapons[static_cast<size_t>(slot)].definitionID != 0; }
    };

    // ============================================================================
    // Weapon Registry (stores all weapon definitions)
    // ============================================================================

    /**
     * @brief Central registry for all weapon definitions
     *
     * Game code registers weapon definitions at startup. The WeaponSystem
     * looks up definitions by ID when processing weapon behavior.
     */
    class WeaponRegistry
    {
      public:
        [[deprecated("Use EngineContext::Get()->GetSystem<WeaponRegistry>() instead")]]
        static WeaponRegistry& GetInstance();

        /// @brief Register a weapon definition and return its ID
        uint32_t RegisterWeapon(WeaponDefinition def);

        /// @brief Look up a weapon definition by ID
        const WeaponDefinition* GetWeapon(uint32_t id) const;

        /// @brief Look up a weapon definition by name
        const WeaponDefinition* GetWeaponByName(const std::string& name) const;

        /// @brief Get all registered weapons
        const std::vector<WeaponDefinition>& GetAllWeapons() const { return m_weapons; }

        /// @brief Register built-in default weapons (pistol, rifle, shotgun, etc.)
        void RegisterDefaults();

      private:
        WeaponRegistry() = default;
        std::vector<WeaponDefinition> m_weapons;
        uint32_t m_nextID = 1;
    };

    // ============================================================================
    // Fire Event (output from weapon system, consumed by projectile spawner)
    // ============================================================================

    /**
     * @brief Emitted when a weapon fires successfully
     */
    struct WeaponFireEvent
    {
        uint32_t ownerEntity = 0; ///< Entity that fired
        uint32_t weaponDefID = 0; ///< Weapon definition
        float damage = 0.0f;      ///< Actual damage (with modifiers)
        float spreadAngle = 0.0f; ///< Computed spread in degrees
        bool isHitscan = true;    ///< Raycast or ballistic
        float muzzleVelocity = 0.0f;
    };

    /**
     * @brief Callback for weapon fire events
     */
    using FireCallback = std::function<void(const WeaponFireEvent&)>;

    // ============================================================================
    // WeaponSystem (ECS system that processes WeaponInventoryComponent)
    // ============================================================================

    /**
     * @brief Processes all entities with WeaponInventoryComponent
     *
     * Handles fire timing, reload state machine, weapon switching,
     * recoil accumulation/recovery, ADS transitions, and spread calculation.
     * Emits WeaponFireEvent when a shot is fired.
     */
    class WeaponSystem
    {
      public:
        WeaponSystem();

        /// @brief Update all weapon components for this frame
        void Update(float deltaTime);

        /// @brief Register a callback for fire events
        Spark::Delegate<const WeaponFireEvent&>::HandlerID OnFire(FireCallback callback)
        {
            return (m_fireCallbacks += std::move(callback));
        }

      private:
        void ProcessWeapon(WeaponInventoryComponent& inv, float deltaTime);
        void HandleFiring(WeaponInstance& weapon, const WeaponDefinition& def, WeaponInventoryComponent& inv);
        void HandleReload(WeaponInstance& weapon, const WeaponDefinition& def, float deltaTime);
        void HandleSwitching(WeaponInventoryComponent& inv, float deltaTime);
        void UpdateRecoilRecovery(WeaponInstance& weapon, const WeaponDefinition& def, float deltaTime);
        void UpdateADS(WeaponInstance& weapon, const WeaponDefinition& def, float deltaTime, bool adsInput);

        float CalculateSpread(const WeaponInstance& weapon, const WeaponDefinition& def, bool isMoving,
                              bool isCrouching, bool isSprinting) const;

        void EmitFireEvent(const WeaponFireEvent& event);

        Spark::Delegate<const WeaponFireEvent&> m_fireCallbacks;
    };

} // namespace Spark::Gameplay
