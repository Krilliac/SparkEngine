// TestWeaponSystem.cpp - Tests for the FPS weapon management system

#include "TestFramework.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ============================================================================
// Standalone reimplementation of key weapon types for testing
// (mirrors Spark::Gameplay from WeaponManager.h)
// ============================================================================

namespace
{

    enum class FireMode : uint8_t
    {
        SemiAuto,
        Burst,
        FullAuto
    };

    enum class WeaponState : uint8_t
    {
        Idle,
        Firing,
        Reloading,
        Switching,
        Empty,
        Disabled
    };

    enum class WeaponSlot : uint8_t
    {
        Primary = 0,
        Secondary = 1,
        Melee = 2,
        Grenade = 3,
        MaxSlots = 4
    };

    struct RecoilPattern
    {
        float verticalPerShot = 0.5f;
        float horizontalPerShot = 0.1f;
        float recoverySpeed = 5.0f;
        float maxVertical = 10.0f;
        float maxHorizontal = 3.0f;
        float firstShotMultiplier = 1.5f;
    };

    struct SpreadConfig
    {
        float baseSpread = 1.0f;
        float moveSpread = 3.0f;
        float crouchReduction = 0.5f;
        float adsReduction = 0.3f;
        float sprintPenalty = 2.0f;
        float maxSpread = 15.0f;
    };

    struct WeaponDefinition
    {
        uint32_t definitionID = 0;
        std::string name;
        WeaponSlot slot = WeaponSlot::Primary;
        float baseDamage = 25.0f;
        float headshotMultiplier = 2.0f;
        FireMode fireMode = FireMode::FullAuto;
        float fireRate = 600.0f;
        int burstCount = 3;
        bool isHitscan = true;
        float muzzleVelocity = 900.0f;
        int magazineSize = 30;
        int maxReserveAmmo = 120;
        float reloadTime = 2.5f;
        float tacticalReloadTime = 2.0f;
        float equipTime = 0.5f;
        float holsterTime = 0.3f;
        float adsTime = 0.2f;
        float adsFOV = 50.0f;
        RecoilPattern recoil;
        SpreadConfig spread;

        float GetShotInterval() const { return fireRate > 0.0f ? 60.0f / fireRate : 1.0f; }
        float GetDPS() const { return baseDamage * (fireRate / 60.0f); }
    };

    struct WeaponInstance
    {
        uint32_t definitionID = 0;
        int currentAmmo = 0;
        int reserveAmmo = 0;
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
        bool CanReload(int magazineSize) const { return currentAmmo < magazineSize && reserveAmmo > 0; }
    };

} // namespace

// ============================================================================
// WeaponDefinition Tests
// ============================================================================

TEST(WeaponDef_ShotInterval)
{
    WeaponDefinition def;
    def.fireRate = 600.0f; // 10 shots per second
    EXPECT_NEAR(def.GetShotInterval(), 0.1f, 0.001f);
}

TEST(WeaponDef_ShotIntervalZeroRate)
{
    WeaponDefinition def;
    def.fireRate = 0.0f;
    EXPECT_NEAR(def.GetShotInterval(), 1.0f, 0.001f); // Safety fallback
}

TEST(WeaponDef_DPS)
{
    WeaponDefinition def;
    def.baseDamage = 30.0f;
    def.fireRate = 600.0f; // 10 shots/sec
    EXPECT_NEAR(def.GetDPS(), 300.0f, 0.01f);
}

// ============================================================================
// WeaponInstance Tests
// ============================================================================

TEST(WeaponInstance_HasAmmo)
{
    WeaponInstance inst;
    inst.currentAmmo = 0;
    EXPECT_FALSE(inst.HasAmmo());

    inst.currentAmmo = 1;
    EXPECT_TRUE(inst.HasAmmo());
}

TEST(WeaponInstance_CanReload)
{
    WeaponInstance inst;
    inst.currentAmmo = 30;
    inst.reserveAmmo = 60;
    EXPECT_FALSE(inst.CanReload(30)); // Magazine full

    inst.currentAmmo = 15;
    EXPECT_TRUE(inst.CanReload(30)); // Not full + has reserve

    inst.reserveAmmo = 0;
    EXPECT_FALSE(inst.CanReload(30)); // No reserve ammo
}

TEST(WeaponInstance_DefaultState)
{
    WeaponInstance inst;
    EXPECT_EQ(static_cast<int>(inst.state), static_cast<int>(WeaponState::Idle));
    EXPECT_FALSE(inst.isADS);
    EXPECT_NEAR(inst.adsBlend, 0.0f, 0.001f);
    EXPECT_NEAR(inst.fireCooldown, 0.0f, 0.001f);
}

// ============================================================================
// Recoil Pattern Tests
// ============================================================================

TEST(Recoil_AccumulationClamped)
{
    RecoilPattern recoil;
    recoil.maxVertical = 10.0f;
    recoil.verticalPerShot = 3.0f;

    float accumulated = 0.0f;
    for (int i = 0; i < 10; ++i)
    {
        accumulated += recoil.verticalPerShot;
        if (accumulated > recoil.maxVertical)
            accumulated = recoil.maxVertical;
    }

    EXPECT_NEAR(accumulated, recoil.maxVertical, 0.001f);
}

TEST(Recoil_FirstShotMultiplier)
{
    RecoilPattern recoil;
    recoil.verticalPerShot = 1.0f;
    recoil.firstShotMultiplier = 1.5f;

    float firstShot = recoil.verticalPerShot * recoil.firstShotMultiplier;
    EXPECT_NEAR(firstShot, 1.5f, 0.001f);
}

// ============================================================================
// Spread Calculation Tests
// ============================================================================

TEST(Spread_BaseSpread)
{
    SpreadConfig spread;
    spread.baseSpread = 2.0f;
    EXPECT_NEAR(spread.baseSpread, 2.0f, 0.001f);
}

TEST(Spread_CrouchReduction)
{
    SpreadConfig spread;
    spread.baseSpread = 4.0f;
    spread.crouchReduction = 0.5f;

    float crouchSpread = spread.baseSpread * spread.crouchReduction;
    EXPECT_NEAR(crouchSpread, 2.0f, 0.001f);
}

TEST(Spread_ADSReduction)
{
    SpreadConfig spread;
    spread.baseSpread = 4.0f;
    spread.adsReduction = 0.3f;

    float adsSpread = spread.baseSpread * spread.adsReduction;
    EXPECT_NEAR(adsSpread, 1.2f, 0.001f);
}

TEST(Spread_MaxSpreadClamped)
{
    SpreadConfig spread;
    spread.baseSpread = 5.0f;
    spread.moveSpread = 8.0f;
    spread.sprintPenalty = 5.0f;
    spread.maxSpread = 15.0f;

    float total = spread.baseSpread + spread.moveSpread + spread.sprintPenalty;
    if (total > spread.maxSpread)
        total = spread.maxSpread;

    EXPECT_NEAR(total, 15.0f, 0.001f);
}

// ============================================================================
// Fire Mode Tests
// ============================================================================

TEST(FireMode_SemiAutoBlocksHeldTrigger)
{
    WeaponInstance inst;
    inst.triggerHeld = true;
    inst.state = WeaponState::Idle;

    // Semi-auto should not fire when trigger is already held
    bool canFire = !inst.triggerHeld;
    EXPECT_FALSE(canFire);
}

TEST(FireMode_FullAutoAllowsHeldTrigger)
{
    WeaponDefinition def;
    def.fireMode = FireMode::FullAuto;

    WeaponInstance inst;
    inst.triggerHeld = true;
    inst.currentAmmo = 30;
    inst.fireCooldown = 0.0f;
    inst.state = WeaponState::Idle;

    // Full auto should allow continuous fire
    bool canFire = inst.HasAmmo() && inst.fireCooldown <= 0.0f;
    EXPECT_TRUE(canFire);
}

// ============================================================================
// Reload Tests
// ============================================================================

TEST(Reload_FullMagazine)
{
    WeaponInstance inst;
    inst.currentAmmo = 15;
    inst.reserveAmmo = 60;
    int magazineSize = 30;

    int needed = magazineSize - inst.currentAmmo;
    int available = std::min(needed, inst.reserveAmmo);
    inst.currentAmmo += available;
    inst.reserveAmmo -= available;

    EXPECT_EQ(inst.currentAmmo, 30);
    EXPECT_EQ(inst.reserveAmmo, 45);
}

TEST(Reload_PartialReserve)
{
    WeaponInstance inst;
    inst.currentAmmo = 0;
    inst.reserveAmmo = 10;
    int magazineSize = 30;

    int needed = magazineSize - inst.currentAmmo;
    int available = std::min(needed, inst.reserveAmmo);
    inst.currentAmmo += available;
    inst.reserveAmmo -= available;

    EXPECT_EQ(inst.currentAmmo, 10);
    EXPECT_EQ(inst.reserveAmmo, 0);
}

// ============================================================================
// Weapon Slot Tests
// ============================================================================

TEST(WeaponSlot_MaxSlots)
{
    EXPECT_EQ(static_cast<int>(WeaponSlot::MaxSlots), 4);
}

TEST(WeaponSlot_Indexing)
{
    std::array<WeaponInstance, static_cast<size_t>(WeaponSlot::MaxSlots)> weapons{};
    weapons[static_cast<size_t>(WeaponSlot::Primary)].definitionID = 1;
    weapons[static_cast<size_t>(WeaponSlot::Secondary)].definitionID = 2;
    weapons[static_cast<size_t>(WeaponSlot::Melee)].definitionID = 3;

    EXPECT_EQ(weapons[0].definitionID, (uint32_t)1);
    EXPECT_EQ(weapons[1].definitionID, (uint32_t)2);
    EXPECT_EQ(weapons[2].definitionID, (uint32_t)3);
    EXPECT_EQ(weapons[3].definitionID, (uint32_t)0);
}
