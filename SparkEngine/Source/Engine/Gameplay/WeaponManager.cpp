/**
 * @file WeaponManager.cpp
 * @brief Weapon system implementation - fire control, reload, recoil, ADS
 */

#include "WeaponManager.h"
#include "../../Core/EngineContext.h"
#include "../../Utils/Validate.h"
#include "../ECS/Components.h"

#include <algorithm>
#include <cmath>

namespace Spark::Gameplay
{

    // ============================================================================
    // WeaponRegistry
    // ============================================================================

    WeaponRegistry& WeaponRegistry::GetInstance()
    {
        static WeaponRegistry instance;
        return instance;
    }

    uint32_t WeaponRegistry::RegisterWeapon(WeaponDefinition def)
    {
        def.definitionID = m_nextID++;
        m_weapons.push_back(std::move(def));
        return m_weapons.back().definitionID;
    }

    const WeaponDefinition* WeaponRegistry::GetWeapon(uint32_t id) const
    {
        for (const auto& w : m_weapons)
        {
            if (w.definitionID == id)
                return &w;
        }
        return nullptr;
    }

    const WeaponDefinition* WeaponRegistry::GetWeaponByName(const std::string& name) const
    {
        for (const auto& w : m_weapons)
        {
            if (w.name == name)
                return &w;
        }
        return nullptr;
    }

    void WeaponRegistry::RegisterDefaults()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Registering default weapon definitions");
        // Pistol
        {
            WeaponDefinition def;
            def.name = "Pistol";
            def.slot = WeaponSlot::Secondary;
            def.baseDamage = 25.0f;
            def.fireMode = FireMode::SemiAuto;
            def.fireRate = 400.0f;
            def.isHitscan = true;
            def.magazineSize = 15;
            def.maxReserveAmmo = 60;
            def.reloadTime = 1.8f;
            def.tacticalReloadTime = 1.5f;
            def.equipTime = 0.3f;
            def.adsFOV = 55.0f;
            def.recoil.verticalPerShot = 1.0f;
            def.spread.baseSpread = 1.5f;
            RegisterWeapon(std::move(def));
        }

        // Assault Rifle
        {
            WeaponDefinition def;
            def.name = "Assault Rifle";
            def.slot = WeaponSlot::Primary;
            def.baseDamage = 30.0f;
            def.fireMode = FireMode::FullAuto;
            def.fireRate = 700.0f;
            def.isHitscan = true;
            def.magazineSize = 30;
            def.maxReserveAmmo = 120;
            def.reloadTime = 2.5f;
            def.tacticalReloadTime = 2.0f;
            def.recoil.verticalPerShot = 0.6f;
            def.recoil.horizontalPerShot = 0.15f;
            def.spread.baseSpread = 1.0f;
            def.moveSpeedMultiplier = 0.9f;
            RegisterWeapon(std::move(def));
        }

        // Shotgun
        {
            WeaponDefinition def;
            def.name = "Shotgun";
            def.slot = WeaponSlot::Primary;
            def.baseDamage = 15.0f; // Per pellet, fires ~8 pellets
            def.fireMode = FireMode::SemiAuto;
            def.fireRate = 80.0f;
            def.isHitscan = true;
            def.magazineSize = 8;
            def.maxReserveAmmo = 32;
            def.reloadTime = 4.0f;
            def.tacticalReloadTime = 3.5f;
            def.equipTime = 0.6f;
            def.recoil.verticalPerShot = 3.0f;
            def.spread.baseSpread = 5.0f;
            def.moveSpeedMultiplier = 0.85f;
            RegisterWeapon(std::move(def));
        }

        // Sniper Rifle
        {
            WeaponDefinition def;
            def.name = "Sniper Rifle";
            def.slot = WeaponSlot::Primary;
            def.baseDamage = 100.0f;
            def.headshotMultiplier = 3.0f;
            def.armorPenetration = 0.8f;
            def.fireMode = FireMode::SemiAuto;
            def.fireRate = 40.0f;
            def.isHitscan = true;
            def.magazineSize = 5;
            def.maxReserveAmmo = 25;
            def.reloadTime = 3.0f;
            def.adsFOV = 20.0f;
            def.adsTime = 0.35f;
            def.recoil.verticalPerShot = 5.0f;
            def.spread.baseSpread = 0.5f;
            def.spread.adsReduction = 0.05f;
            def.moveSpeedMultiplier = 0.8f;
            RegisterWeapon(std::move(def));
        }

        // SMG
        {
            WeaponDefinition def;
            def.name = "SMG";
            def.slot = WeaponSlot::Primary;
            def.baseDamage = 20.0f;
            def.fireMode = FireMode::FullAuto;
            def.fireRate = 900.0f;
            def.isHitscan = true;
            def.magazineSize = 25;
            def.maxReserveAmmo = 100;
            def.reloadTime = 2.0f;
            def.tacticalReloadTime = 1.7f;
            def.equipTime = 0.25f;
            def.recoil.verticalPerShot = 0.3f;
            def.recoil.horizontalPerShot = 0.2f;
            def.spread.baseSpread = 2.0f;
            def.moveSpeedMultiplier = 0.95f;
            RegisterWeapon(std::move(def));
        }

        // Knife (melee)
        {
            WeaponDefinition def;
            def.name = "Knife";
            def.slot = WeaponSlot::Melee;
            def.baseDamage = 50.0f;
            def.fireMode = FireMode::SemiAuto;
            def.fireRate = 60.0f;
            def.isHitscan = true;
            def.muzzleVelocity = 0.0f;
            def.magazineSize = 1;
            def.maxReserveAmmo = 0;
            def.reloadTime = 0.0f;
            def.equipTime = 0.2f;
            def.adsFOV = 90.0f;
            def.recoil = {};
            def.spread.baseSpread = 0.0f;
            def.moveSpeedMultiplier = 1.0f;
            RegisterWeapon(std::move(def));
        }

        // Frag Grenade
        {
            WeaponDefinition def;
            def.name = "Frag Grenade";
            def.slot = WeaponSlot::Grenade;
            def.baseDamage = 150.0f;
            def.fireMode = FireMode::SemiAuto;
            def.fireRate = 30.0f;
            def.isHitscan = false;
            def.muzzleVelocity = 20.0f;
            def.magazineSize = 1;
            def.maxReserveAmmo = 3;
            def.reloadTime = 0.5f;
            def.equipTime = 0.4f;
            RegisterWeapon(std::move(def));
        }
    }

    // ============================================================================
    // WeaponSystem
    // ============================================================================

    WeaponSystem::WeaponSystem() = default;

    void WeaponSystem::Update(float deltaTime)
    {
        auto* ctx = ::EngineContext::Get();
        if (!ctx)
            return;

        auto* world = ctx->GetWorld();
        if (!world)
            return;

        auto view = world->GetEntitiesWith<WeaponInventoryComponent>();
        for (auto [entity, inv] : view.each())
        {
            ProcessWeapon(inv, deltaTime);
        }
    }

    void WeaponSystem::ProcessWeapon(WeaponInventoryComponent& inv, float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        auto& weapon = inv.GetActiveWeapon();
        if (weapon.definitionID == 0)
            return;

        const auto* def = WeaponRegistry::GetInstance().GetWeapon(weapon.definitionID);
        if (!def)
            return;

        // Handle weapon switching
        if (inv.inputSwitchSlot >= 0 && inv.inputSwitchSlot < static_cast<int>(WeaponInventoryComponent::MAX_WEAPONS))
        {
            auto targetSlot = static_cast<WeaponSlot>(inv.inputSwitchSlot);
            if (targetSlot != inv.activeSlot && inv.HasWeaponInSlot(targetSlot) &&
                weapon.state != WeaponState::Switching)
            {
                weapon.state = WeaponState::Switching;
                weapon.stateTimer = def->holsterTime;
                inv.pendingSlot = targetSlot;
                SPARK_LOG_INFO(Spark::LogCategory::Core, "Weapon switch initiated from slot %d to slot %d",
                               static_cast<int>(inv.activeSlot), inv.inputSwitchSlot);
            }
            inv.inputSwitchSlot = -1;
        }

        // Handle switching state
        if (weapon.state == WeaponState::Switching)
        {
            HandleSwitching(inv, deltaTime);
            return;
        }

        // Handle reloading
        if (weapon.state == WeaponState::Reloading)
        {
            HandleReload(weapon, *def, deltaTime);
            return;
        }

        // Auto-reload on empty
        if (weapon.state == WeaponState::Empty && weapon.CanReload(def->magazineSize))
        {
            weapon.state = WeaponState::Reloading;
            weapon.stateTimer = weapon.currentAmmo > 0 ? def->tacticalReloadTime : def->reloadTime;
            return;
        }

        // Manual reload
        if (inv.inputReload && weapon.state == WeaponState::Idle && weapon.CanReload(def->magazineSize))
        {
            weapon.state = WeaponState::Reloading;
            weapon.stateTimer = weapon.currentAmmo > 0 ? def->tacticalReloadTime : def->reloadTime;
            inv.inputReload = false;
            return;
        }

        // Fire cooldown
        if (weapon.fireCooldown > 0.0f)
        {
            weapon.fireCooldown -= deltaTime;
            if (weapon.fireCooldown <= 0.0f)
            {
                weapon.fireCooldown = 0.0f;
                if (weapon.state == WeaponState::Firing)
                    weapon.state = WeaponState::Idle;
            }
        }

        // Firing logic
        if (inv.inputFire && weapon.state == WeaponState::Idle && weapon.fireCooldown <= 0.0f)
        {
            if (weapon.HasAmmo())
            {
                HandleFiring(weapon, *def, inv);
            }
            else
            {
                weapon.state = WeaponState::Empty;
                SPARK_LOG_INFO(Spark::LogCategory::Core, "Weapon ammo depleted — switching to Empty state");
            }
        }

        // For semi-auto, clear trigger on release
        if (!inv.inputFire && def->fireMode == FireMode::SemiAuto)
        {
            weapon.triggerHeld = false;
        }

        // Recoil recovery
        UpdateRecoilRecovery(weapon, *def, deltaTime);

        // ADS transitions
        UpdateADS(weapon, *def, deltaTime, inv.inputADS);
    }

    void WeaponSystem::HandleFiring(WeaponInstance& weapon, const WeaponDefinition& def, WeaponInventoryComponent& inv)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Game);
        // Semi-auto: prevent firing while trigger is held from previous shot
        if (def.fireMode == FireMode::SemiAuto && weapon.triggerHeld)
            return;

        weapon.currentAmmo--;
        weapon.state = WeaponState::Firing;
        weapon.fireCooldown = def.GetShotInterval();
        weapon.triggerHeld = true;
        SPARK_LOG_DEBUG(Spark::LogCategory::Core, "Weapon '%s' fired — ammo: %d/%d", def.name.c_str(),
                        weapon.currentAmmo, weapon.reserveAmmo);

        // Apply recoil
        float recoilMultiplier = (weapon.accumulatedVerticalRecoil == 0.0f) ? def.recoil.firstShotMultiplier : 1.0f;
        weapon.accumulatedVerticalRecoil += def.recoil.verticalPerShot * recoilMultiplier;
        weapon.accumulatedHorizontalRecoil += def.recoil.horizontalPerShot;

        weapon.accumulatedVerticalRecoil = std::min(weapon.accumulatedVerticalRecoil, def.recoil.maxVertical);
        weapon.accumulatedHorizontalRecoil = std::min(weapon.accumulatedHorizontalRecoil, def.recoil.maxHorizontal);

        // Calculate spread
        float spreadAngle = CalculateSpread(weapon, def, false, false, false);

        // Emit fire event
        WeaponFireEvent event;
        event.ownerEntity = 0; // Set by ECS integration
        event.weaponDefID = def.definitionID;
        event.damage = def.baseDamage;
        event.spreadAngle = spreadAngle;
        event.isHitscan = def.isHitscan;
        event.muzzleVelocity = def.muzzleVelocity;
        EmitFireEvent(event);

        // Handle burst mode
        if (def.fireMode == FireMode::Burst)
        {
            if (weapon.burstShotsRemaining <= 0)
                weapon.burstShotsRemaining = def.burstCount - 1; // Already fired one
            else
                weapon.burstShotsRemaining--;

            if (weapon.burstShotsRemaining <= 0)
                weapon.triggerHeld = true; // Prevent re-fire until release
        }

        // Clear input for non-auto modes
        if (def.fireMode != FireMode::FullAuto)
            inv.inputFire = false;
    }

    void WeaponSystem::HandleReload(WeaponInstance& weapon, const WeaponDefinition& def, float deltaTime)
    {
        weapon.stateTimer -= deltaTime;
        if (weapon.stateTimer <= 0.0f)
        {
            int needed = def.magazineSize - weapon.currentAmmo;
            int available = std::min(needed, weapon.reserveAmmo);
            weapon.currentAmmo += available;
            weapon.reserveAmmo -= available;
            weapon.state = WeaponState::Idle;
            weapon.stateTimer = 0.0f;
            SPARK_LOG_INFO(Spark::LogCategory::Core, "Reload complete — ammo: %d, reserve: %d", weapon.currentAmmo,
                           weapon.reserveAmmo);
        }
    }

    void WeaponSystem::HandleSwitching(WeaponInventoryComponent& inv, float deltaTime)
    {
        auto& weapon = inv.GetActiveWeapon();
        weapon.stateTimer -= deltaTime;

        if (weapon.stateTimer <= 0.0f)
        {
            // Complete the switch
            weapon.state = WeaponState::Idle;
            weapon.stateTimer = 0.0f;
            inv.activeSlot = inv.pendingSlot;

            // Start equip timer on new weapon
            auto& newWeapon = inv.GetActiveWeapon();
            const auto* newDef = WeaponRegistry::GetInstance().GetWeapon(newWeapon.definitionID);
            if (newDef)
            {
                newWeapon.state = WeaponState::Switching;
                newWeapon.stateTimer = newDef->equipTime;
            }
        }
    }

    void WeaponSystem::UpdateRecoilRecovery(WeaponInstance& weapon, const WeaponDefinition& def, float deltaTime)
    {
        if (weapon.accumulatedVerticalRecoil > 0.0f)
        {
            weapon.accumulatedVerticalRecoil -= def.recoil.recoverySpeed * deltaTime;
            weapon.accumulatedVerticalRecoil = std::max(weapon.accumulatedVerticalRecoil, 0.0f);
        }
        if (weapon.accumulatedHorizontalRecoil > 0.0f)
        {
            weapon.accumulatedHorizontalRecoil -= def.recoil.recoverySpeed * deltaTime;
            weapon.accumulatedHorizontalRecoil = std::max(weapon.accumulatedHorizontalRecoil, 0.0f);
        }
    }

    void WeaponSystem::UpdateADS(WeaponInstance& weapon, const WeaponDefinition& def, float deltaTime, bool adsInput)
    {
        float adsSpeed = def.adsTime > 0.0f ? 1.0f / def.adsTime : 10.0f;

        if (adsInput && !weapon.isADS)
        {
            weapon.adsBlend += adsSpeed * deltaTime;
            if (weapon.adsBlend >= 1.0f)
            {
                weapon.adsBlend = 1.0f;
                weapon.isADS = true;
            }
        }
        else if (!adsInput && (weapon.isADS || weapon.adsBlend > 0.0f))
        {
            weapon.adsBlend -= adsSpeed * deltaTime;
            if (weapon.adsBlend <= 0.0f)
            {
                weapon.adsBlend = 0.0f;
                weapon.isADS = false;
            }
        }
    }

    float WeaponSystem::CalculateSpread(const WeaponInstance& weapon, const WeaponDefinition& def, bool isMoving,
                                        bool isCrouching, bool isSprinting) const
    {
        float spread = def.spread.baseSpread;

        if (isMoving)
            spread += def.spread.moveSpread;
        if (isSprinting)
            spread += def.spread.sprintPenalty;
        if (isCrouching)
            spread *= def.spread.crouchReduction;
        if (weapon.isADS || weapon.adsBlend > 0.5f)
            spread *= def.spread.adsReduction;

        // Recoil adds to spread
        spread += weapon.accumulatedVerticalRecoil * 0.2f;

        return std::min(spread, def.spread.maxSpread);
    }

    void WeaponSystem::EmitFireEvent(const WeaponFireEvent& event)
    {
        m_fireCallbacks.Broadcast(event);
    }

} // namespace Spark::Gameplay
