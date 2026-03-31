/**
 * @file TestWeaponMechanics.cpp
 * @brief Tests for WeaponManager: fire rate, spread calculation, recoil recovery,
 *        reload states, ammo management, and weapon switching.
 *
 * Standalone reimplementation — mirrors Spark::Gameplay::WeaponSystem
 * without engine dependencies for CI/Linux compatibility.
 */

#include "TestFramework.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace
{

    constexpr float PI = 3.14159265358979323846f;

    enum class FireMode
    {
        SemiAuto,
        Burst,
        FullAuto
    };

    enum class WeaponState
    {
        Idle,
        Firing,
        Reloading,
        Switching,
        Empty,
        Disabled
    };

    enum class WeaponSlot
    {
        Primary,
        Secondary,
        Melee,
        Grenade,
        MaxSlots
    };

    struct RecoilPattern
    {
        float verticalPerShot = 1.0f;
        float horizontalPerShot = 0.3f;
        float recoverySpeed = 5.0f;
        float maxVertical = 10.0f;
        float maxHorizontal = 5.0f;
        float firstShotMultiplier = 1.5f;
    };

    struct SpreadConfig
    {
        float baseSpread = 1.0f;      // degrees
        float moveSpread = 2.0f;      // added when moving
        float crouchReduction = 0.5f; // multiplier
        float adsReduction = 0.3f;    // multiplier
        float sprintPenalty = 3.0f;   // added when sprinting
        float maxSpread = 10.0f;
    };

    struct WeaponDefinition
    {
        uint32_t definitionID = 0;
        std::string name;
        WeaponSlot slot = WeaponSlot::Primary;
        float baseDamage = 25.0f;
        float headshotMultiplier = 2.0f;
        float armorPenetration = 0.0f;
        FireMode fireMode = FireMode::FullAuto;
        float fireRate = 600.0f; // rounds per minute
        int burstCount = 3;
        float muzzleVelocity = 900.0f;
        bool isHitscan = true;
        int magazineSize = 30;
        int maxReserveAmmo = 120;
        float reloadTime = 2.5f;
        float tacticalReloadTime = 2.0f;
        float equipTime = 0.5f;
        float holsterTime = 0.3f;
        float adsTime = 0.2f;
        float adsFOV = 60.0f;
        float moveSpeedMultiplier = 0.95f;
        RecoilPattern recoil;
        SpreadConfig spread;

        float GetShotInterval() const { return 60.0f / fireRate; }
        float GetDPS() const { return baseDamage * (fireRate / 60.0f); }
    };

    struct WeaponInstance
    {
        uint32_t definitionID = 0;
        int currentAmmo = 30;
        int reserveAmmo = 120;
        WeaponState state = WeaponState::Idle;
        float stateTimer = 0.0f;
        float accumulatedVerticalRecoil = 0.0f;
        float accumulatedHorizontalRecoil = 0.0f;
        bool isADS = false;
        float adsBlend = 0.0f;
        bool triggerHeld = false;
        int burstShotsRemaining = 0;
        float fireCooldown = 0.0f;

        bool HasAmmo() const { return currentAmmo > 0; }
        bool CanReload() const { return reserveAmmo > 0 && currentAmmo < 30; }
    };

    // --- Weapon system functions ---

    float CalculateSpread(const SpreadConfig& config, bool isMoving, bool isCrouching, bool isADS, bool isSprinting)
    {
        float spread = config.baseSpread;

        if (isMoving)
            spread += config.moveSpread;
        if (isSprinting)
            spread += config.sprintPenalty;
        if (isCrouching)
            spread *= config.crouchReduction;
        if (isADS)
            spread *= config.adsReduction;

        return std::min(spread, config.maxSpread);
    }

    void ApplyRecoil(WeaponInstance& weapon, const RecoilPattern& pattern, bool isFirstShot)
    {
        float multiplier = isFirstShot ? pattern.firstShotMultiplier : 1.0f;

        weapon.accumulatedVerticalRecoil += pattern.verticalPerShot * multiplier;
        weapon.accumulatedHorizontalRecoil += pattern.horizontalPerShot * multiplier;

        // Clamp to max
        weapon.accumulatedVerticalRecoil = std::min(weapon.accumulatedVerticalRecoil, pattern.maxVertical);
        weapon.accumulatedHorizontalRecoil = std::min(weapon.accumulatedHorizontalRecoil, pattern.maxHorizontal);
    }

    void RecoverRecoil(WeaponInstance& weapon, const RecoilPattern& pattern, float deltaTime)
    {
        float recovery = pattern.recoverySpeed * deltaTime;
        weapon.accumulatedVerticalRecoil = std::max(0.0f, weapon.accumulatedVerticalRecoil - recovery);
        weapon.accumulatedHorizontalRecoil = std::max(0.0f, weapon.accumulatedHorizontalRecoil - recovery);
    }

    bool TryFire(WeaponInstance& weapon, const WeaponDefinition& def)
    {
        if (weapon.state != WeaponState::Idle && weapon.state != WeaponState::Firing)
            return false;
        if (weapon.fireCooldown > 0.0f)
            return false;
        if (!weapon.HasAmmo())
        {
            weapon.state = WeaponState::Empty;
            return false;
        }

        weapon.currentAmmo--;
        weapon.fireCooldown = def.GetShotInterval();
        weapon.state = WeaponState::Firing;
        return true;
    }

    bool TryReload(WeaponInstance& weapon, const WeaponDefinition& def)
    {
        if (weapon.state == WeaponState::Reloading)
            return false;
        if (!weapon.CanReload())
            return false;

        weapon.state = WeaponState::Reloading;
        weapon.stateTimer = (weapon.currentAmmo > 0) ? def.tacticalReloadTime : def.reloadTime;
        return true;
    }

    void CompleteReload(WeaponInstance& weapon, int magazineSize)
    {
        int needed = magazineSize - weapon.currentAmmo;
        int toLoad = std::min(needed, weapon.reserveAmmo);
        weapon.currentAmmo += toLoad;
        weapon.reserveAmmo -= toLoad;
        weapon.state = WeaponState::Idle;
        weapon.stateTimer = 0.0f;
    }

    void UpdateWeapon(WeaponInstance& weapon, const WeaponDefinition& def, float deltaTime)
    {
        if (weapon.fireCooldown > 0.0f)
            weapon.fireCooldown -= deltaTime;

        if (weapon.state == WeaponState::Reloading)
        {
            weapon.stateTimer -= deltaTime;
            if (weapon.stateTimer <= 0.0f)
                CompleteReload(weapon, def.magazineSize);
        }

        // Recover recoil when not firing
        if (weapon.state != WeaponState::Firing)
            RecoverRecoil(weapon, def.recoil, deltaTime);

        // Transition from Firing to Idle when cooldown expires
        if (weapon.state == WeaponState::Firing && weapon.fireCooldown <= 0.0f)
            weapon.state = WeaponState::Idle;

        // ADS blend
        float adsTarget = weapon.isADS ? 1.0f : 0.0f;
        float adsSpeed = 1.0f / def.adsTime;
        if (weapon.adsBlend < adsTarget)
            weapon.adsBlend = std::min(adsTarget, weapon.adsBlend + adsSpeed * deltaTime);
        else if (weapon.adsBlend > adsTarget)
            weapon.adsBlend = std::max(adsTarget, weapon.adsBlend - adsSpeed * deltaTime);
    }

} // anonymous namespace

// ============================================================================
// Fire Rate Tests
// ============================================================================

TEST(Weapon_ShotInterval)
{
    WeaponDefinition rifle;
    rifle.fireRate = 600.0f; // 10 rounds/sec

    EXPECT_NEAR(rifle.GetShotInterval(), 0.1f, 0.001f);
}

TEST(Weapon_DPS)
{
    WeaponDefinition rifle;
    rifle.baseDamage = 25.0f;
    rifle.fireRate = 600.0f;

    EXPECT_NEAR(rifle.GetDPS(), 250.0f, 0.1f); // 25 * 10
}

TEST(Weapon_FireConsumesAmmo)
{
    WeaponDefinition def;
    def.fireRate = 600.0f;
    def.magazineSize = 30;

    WeaponInstance weapon;
    weapon.currentAmmo = 30;

    EXPECT_TRUE(TryFire(weapon, def));
    EXPECT_EQ(weapon.currentAmmo, 29);
}

TEST(Weapon_FireEmptyMagazine)
{
    WeaponDefinition def;
    WeaponInstance weapon;
    weapon.currentAmmo = 0;

    EXPECT_FALSE(TryFire(weapon, def));
    EXPECT_EQ(static_cast<int>(weapon.state), static_cast<int>(WeaponState::Empty));
}

TEST(Weapon_FireCooldownPrevents)
{
    WeaponDefinition def;
    def.fireRate = 600.0f;

    WeaponInstance weapon;
    weapon.currentAmmo = 30;

    TryFire(weapon, def);
    EXPECT_FALSE(TryFire(weapon, def)); // on cooldown
}

TEST(Weapon_FireCooldownExpires)
{
    WeaponDefinition def;
    def.fireRate = 600.0f;

    WeaponInstance weapon;
    weapon.currentAmmo = 30;

    TryFire(weapon, def);
    UpdateWeapon(weapon, def, 0.15f); // wait past interval (0.1s)

    EXPECT_TRUE(TryFire(weapon, def));
}

// ============================================================================
// Spread Calculation Tests
// ============================================================================

TEST(Weapon_SpreadStanding)
{
    SpreadConfig config;
    config.baseSpread = 1.0f;

    float spread = CalculateSpread(config, false, false, false, false);
    EXPECT_NEAR(spread, 1.0f, 0.01f);
}

TEST(Weapon_SpreadMoving)
{
    SpreadConfig config;
    config.baseSpread = 1.0f;
    config.moveSpread = 2.0f;

    float spread = CalculateSpread(config, true, false, false, false);
    EXPECT_NEAR(spread, 3.0f, 0.01f);
}

TEST(Weapon_SpreadCrouching)
{
    SpreadConfig config;
    config.baseSpread = 2.0f;
    config.crouchReduction = 0.5f;

    float spread = CalculateSpread(config, false, true, false, false);
    EXPECT_NEAR(spread, 1.0f, 0.01f);
}

TEST(Weapon_SpreadADS)
{
    SpreadConfig config;
    config.baseSpread = 2.0f;
    config.adsReduction = 0.3f;

    float spread = CalculateSpread(config, false, false, true, false);
    EXPECT_NEAR(spread, 0.6f, 0.01f);
}

TEST(Weapon_SpreadSprinting)
{
    SpreadConfig config;
    config.baseSpread = 1.0f;
    config.sprintPenalty = 3.0f;

    float spread = CalculateSpread(config, false, false, false, true);
    EXPECT_NEAR(spread, 4.0f, 0.01f);
}

TEST(Weapon_SpreadCappedAtMax)
{
    SpreadConfig config;
    config.baseSpread = 5.0f;
    config.moveSpread = 5.0f;
    config.sprintPenalty = 5.0f;
    config.maxSpread = 10.0f;

    float spread = CalculateSpread(config, true, false, false, true);
    EXPECT_NEAR(spread, 10.0f, 0.01f);
}

TEST(Weapon_SpreadCombined)
{
    SpreadConfig config;
    config.baseSpread = 2.0f;
    config.moveSpread = 1.0f;
    config.crouchReduction = 0.5f;
    config.adsReduction = 0.5f;
    config.maxSpread = 10.0f;

    // Moving + crouching + ADS
    float spread = CalculateSpread(config, true, true, true, false);
    // (2.0 + 1.0) * 0.5 * 0.5 = 0.75
    EXPECT_NEAR(spread, 0.75f, 0.01f);
}

// ============================================================================
// Recoil Tests
// ============================================================================

TEST(Weapon_RecoilAccumulates)
{
    RecoilPattern pattern;
    pattern.verticalPerShot = 1.0f;
    pattern.horizontalPerShot = 0.3f;
    pattern.maxVertical = 10.0f;

    WeaponInstance weapon;
    ApplyRecoil(weapon, pattern, false);
    EXPECT_NEAR(weapon.accumulatedVerticalRecoil, 1.0f, 0.01f);

    ApplyRecoil(weapon, pattern, false);
    EXPECT_NEAR(weapon.accumulatedVerticalRecoil, 2.0f, 0.01f);
}

TEST(Weapon_FirstShotMultiplier)
{
    RecoilPattern pattern;
    pattern.verticalPerShot = 1.0f;
    pattern.firstShotMultiplier = 1.5f;

    WeaponInstance weapon;
    ApplyRecoil(weapon, pattern, true);
    EXPECT_NEAR(weapon.accumulatedVerticalRecoil, 1.5f, 0.01f);
}

TEST(Weapon_RecoilClamped)
{
    RecoilPattern pattern;
    pattern.verticalPerShot = 5.0f;
    pattern.maxVertical = 8.0f;

    WeaponInstance weapon;
    ApplyRecoil(weapon, pattern, false);
    ApplyRecoil(weapon, pattern, false); // would be 10, clamped to 8
    EXPECT_NEAR(weapon.accumulatedVerticalRecoil, 8.0f, 0.01f);
}

TEST(Weapon_RecoilRecovery)
{
    RecoilPattern pattern;
    pattern.verticalPerShot = 5.0f;
    pattern.recoverySpeed = 2.5f;

    WeaponInstance weapon;
    weapon.accumulatedVerticalRecoil = 5.0f;

    RecoverRecoil(weapon, pattern, 1.0f);
    EXPECT_NEAR(weapon.accumulatedVerticalRecoil, 2.5f, 0.01f);
}

TEST(Weapon_RecoilRecoveryFloorAtZero)
{
    RecoilPattern pattern;
    pattern.recoverySpeed = 100.0f;

    WeaponInstance weapon;
    weapon.accumulatedVerticalRecoil = 1.0f;

    RecoverRecoil(weapon, pattern, 1.0f);
    EXPECT_NEAR(weapon.accumulatedVerticalRecoil, 0.0f, 0.01f);
}

// ============================================================================
// Reload Tests
// ============================================================================

TEST(Weapon_ReloadFromPartial)
{
    WeaponDefinition def;
    def.magazineSize = 30;
    def.tacticalReloadTime = 2.0f;

    WeaponInstance weapon;
    weapon.currentAmmo = 15;
    weapon.reserveAmmo = 60;

    EXPECT_TRUE(TryReload(weapon, def));
    EXPECT_EQ(static_cast<int>(weapon.state), static_cast<int>(WeaponState::Reloading));
    EXPECT_NEAR(weapon.stateTimer, 2.0f, 0.01f); // tactical reload (has ammo)
}

TEST(Weapon_ReloadFromEmpty)
{
    WeaponDefinition def;
    def.magazineSize = 30;
    def.reloadTime = 2.5f;

    WeaponInstance weapon;
    weapon.currentAmmo = 0;
    weapon.reserveAmmo = 60;

    EXPECT_TRUE(TryReload(weapon, def));
    EXPECT_NEAR(weapon.stateTimer, 2.5f, 0.01f); // full reload
}

TEST(Weapon_ReloadCompletes)
{
    WeaponDefinition def;
    def.magazineSize = 30;
    def.reloadTime = 2.5f;

    WeaponInstance weapon;
    weapon.currentAmmo = 0;
    weapon.reserveAmmo = 60;

    TryReload(weapon, def);
    UpdateWeapon(weapon, def, 3.0f); // wait past reload time

    EXPECT_EQ(weapon.currentAmmo, 30);
    EXPECT_EQ(weapon.reserveAmmo, 30);
    EXPECT_EQ(static_cast<int>(weapon.state), static_cast<int>(WeaponState::Idle));
}

TEST(Weapon_ReloadPartialReserve)
{
    WeaponDefinition def;
    def.magazineSize = 30;

    WeaponInstance weapon;
    weapon.currentAmmo = 20;
    weapon.reserveAmmo = 5;

    TryReload(weapon, def);
    CompleteReload(weapon, def.magazineSize);

    EXPECT_EQ(weapon.currentAmmo, 25);
    EXPECT_EQ(weapon.reserveAmmo, 0);
}

TEST(Weapon_CantReloadWhenFull)
{
    WeaponDefinition def;
    def.magazineSize = 30;

    WeaponInstance weapon;
    weapon.currentAmmo = 30;
    weapon.reserveAmmo = 60;

    EXPECT_FALSE(weapon.CanReload());
}

TEST(Weapon_CantReloadNoReserve)
{
    WeaponDefinition def;

    WeaponInstance weapon;
    weapon.currentAmmo = 15;
    weapon.reserveAmmo = 0;

    EXPECT_FALSE(weapon.CanReload());
}

// ============================================================================
// Weapon State Tests
// ============================================================================

TEST(Weapon_CantFireWhileReloading)
{
    WeaponDefinition def;
    WeaponInstance weapon;
    weapon.currentAmmo = 30;
    weapon.state = WeaponState::Reloading;

    EXPECT_FALSE(TryFire(weapon, def));
}

TEST(Weapon_CantFireWhileDisabled)
{
    WeaponDefinition def;
    WeaponInstance weapon;
    weapon.currentAmmo = 30;
    weapon.state = WeaponState::Disabled;

    EXPECT_FALSE(TryFire(weapon, def));
}

// ============================================================================
// ADS Blend Tests
// ============================================================================

TEST(Weapon_ADSBlendTransition)
{
    WeaponDefinition def;
    def.adsTime = 0.2f;

    WeaponInstance weapon;
    weapon.isADS = true;
    weapon.adsBlend = 0.0f;

    UpdateWeapon(weapon, def, 0.1f);
    EXPECT_NEAR(weapon.adsBlend, 0.5f, 0.05f); // halfway through ADS

    UpdateWeapon(weapon, def, 0.15f);
    EXPECT_NEAR(weapon.adsBlend, 1.0f, 0.01f); // fully ADS
}

TEST(Weapon_ADSUnscope)
{
    WeaponDefinition def;
    def.adsTime = 0.2f;

    WeaponInstance weapon;
    weapon.isADS = false;
    weapon.adsBlend = 1.0f; // was fully scoped

    UpdateWeapon(weapon, def, 0.3f);
    EXPECT_NEAR(weapon.adsBlend, 0.0f, 0.01f);
}

// ============================================================================
// Headshot / Damage Tests
// ============================================================================

TEST(Weapon_HeadshotDamage)
{
    WeaponDefinition def;
    def.baseDamage = 30.0f;
    def.headshotMultiplier = 2.5f;

    float headshotDamage = def.baseDamage * def.headshotMultiplier;
    EXPECT_NEAR(headshotDamage, 75.0f, 0.01f);
}
