/**
 * @file TestWeaponMechanicsReal.cpp
 * @brief Production-source tests for the shipped FPS weapon model and WeaponSystem
 *
 * Exercises Spark::Gameplay's shipped types (Engine/Gameplay/WeaponManager.cpp,
 * part of SparkEngineLib): the WeaponDefinition / WeaponInstance data model, the
 * WeaponInventoryComponent, WeaponRegistry lookup, and WeaponSystem::Update's
 * fire / cooldown / reload / recoil / spread / ADS / switching state machine,
 * driven through a real World published by an injected EngineContext.
 * (RDY-010 retired the standalone TestWeaponMechanics.cpp mirror these replaced.)
 */

#include "TestFramework.h"
#include "Core/EngineContext.h"
#include "Engine/ECS/Components.h"
#include "Engine/Gameplay/WeaponManager.h"

#include <cstddef>
#include <string>
#include <vector>

using namespace Spark::Gameplay;

namespace
{
    // RegisterWeapon appends to a process-wide registry that is never cleared,
    // so every test here uses a name unique to this file to stay independent of
    // execution order and of any other test that registers weapons.
    WeaponDefinition MakeDefinition(const std::string& name)
    {
        WeaponDefinition def;
        def.name = name;
        def.slot = WeaponSlot::Primary;
        def.baseDamage = 20.0f;
        def.fireRate = 600.0f;
        def.magazineSize = 30;
        return def;
    }

    /// One armed entity in a real World that WeaponSystem::Update reaches through EngineContext.
    struct WeaponRig
    {
        EngineContext context;
        World world;
        WeaponSystem system;
        WeaponInventoryComponent* inventory = nullptr;
        std::vector<WeaponFireEvent> fired;

        explicit WeaponRig(const WeaponDefinition& definition, int currentAmmo = 30, int reserveAmmo = 90)
        {
            context.SetWorld(&world);
            EngineContext::SetInjected(&context);
            const EntityID entity = world.CreateEntity("WeaponMechanicsReal_Shooter");
            inventory = &world.AddComponent<WeaponInventoryComponent>(entity);
            WeaponInstance& weapon = inventory->weapons[static_cast<std::size_t>(definition.slot)];
            weapon.definitionID = WeaponRegistry::GetInstance().RegisterWeapon(definition);
            weapon.currentAmmo = currentAmmo;
            weapon.reserveAmmo = reserveAmmo;
            inventory->activeSlot = definition.slot;
            inventory->pendingSlot = definition.slot;
            system.OnFire([this](const WeaponFireEvent& event) { fired.push_back(event); });
        }

        ~WeaponRig() { EngineContext::SetInjected(nullptr); }

        WeaponRig(const WeaponRig&) = delete;
        WeaponRig& operator=(const WeaponRig&) = delete;

        WeaponInstance& Active() { return inventory->GetActiveWeapon(); }
    };
} // namespace

TEST(WeaponMechanicsReal_ShotIntervalFromFireRate)
{
    WeaponDefinition def;
    def.fireRate = 600.0f;
    // 600 RPM = 10 rounds per second = 100ms between shots.
    EXPECT_NEAR(def.GetShotInterval(), 0.1f, 0.0001f);

    def.fireRate = 120.0f;
    EXPECT_NEAR(def.GetShotInterval(), 0.5f, 0.0001f);
}

TEST(WeaponMechanicsReal_ShotIntervalGuardsZeroFireRate)
{
    WeaponDefinition def;
    def.fireRate = 0.0f;
    // Must not divide by zero; the shipped guard returns a 1s interval.
    EXPECT_NEAR(def.GetShotInterval(), 1.0f, 0.0001f);
}

TEST(WeaponMechanicsReal_DPSCombinesDamageAndFireRate)
{
    WeaponDefinition def;
    def.baseDamage = 25.0f;
    def.fireRate = 600.0f;
    EXPECT_NEAR(def.GetDPS(), 250.0f, 0.001f);
}

TEST(WeaponMechanicsReal_InstanceAmmoPredicates)
{
    WeaponInstance weapon;
    weapon.currentAmmo = 0;
    EXPECT_FALSE(weapon.HasAmmo());
    weapon.currentAmmo = 1;
    EXPECT_TRUE(weapon.HasAmmo());
}

TEST(WeaponMechanicsReal_CanReloadNeedsRoomAndReserve)
{
    WeaponInstance weapon;
    weapon.currentAmmo = 30;
    weapon.reserveAmmo = 90;
    // Full magazine: nothing to reload even with reserve available.
    EXPECT_FALSE(weapon.CanReload(30));

    weapon.currentAmmo = 12;
    EXPECT_TRUE(weapon.CanReload(30));

    weapon.reserveAmmo = 0;
    // Room in the magazine but no reserve to draw from.
    EXPECT_FALSE(weapon.CanReload(30));
}

TEST(WeaponMechanicsReal_InventoryHasOneSlotPerWeaponSlotEnum)
{
    EXPECT_EQ(WeaponInventoryComponent::MAX_WEAPONS, static_cast<size_t>(WeaponSlot::MaxSlots));
    EXPECT_EQ(WeaponInventoryComponent::MAX_WEAPONS, static_cast<size_t>(4));
}

TEST(WeaponMechanicsReal_ActiveWeaponFollowsActiveSlot)
{
    WeaponInventoryComponent inv;
    inv.weapons[static_cast<size_t>(WeaponSlot::Primary)].currentAmmo = 30;
    inv.weapons[static_cast<size_t>(WeaponSlot::Secondary)].currentAmmo = 7;

    inv.activeSlot = WeaponSlot::Primary;
    EXPECT_EQ(inv.GetActiveWeapon().currentAmmo, 30);

    inv.activeSlot = WeaponSlot::Secondary;
    EXPECT_EQ(inv.GetActiveWeapon().currentAmmo, 7);
}

TEST(WeaponMechanicsReal_HasWeaponInSlotTracksDefinitionID)
{
    WeaponInventoryComponent inv;
    EXPECT_FALSE(inv.HasWeaponInSlot(WeaponSlot::Melee));
    inv.weapons[static_cast<size_t>(WeaponSlot::Melee)].definitionID = 42;
    EXPECT_TRUE(inv.HasWeaponInSlot(WeaponSlot::Melee));
}

TEST(WeaponMechanicsReal_RegistryIsASingleInstance)
{
    auto& a = WeaponRegistry::GetInstance();
    auto& b = WeaponRegistry::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(WeaponMechanicsReal_RegistryAssignsUniqueNonZeroIDs)
{
    auto& registry = WeaponRegistry::GetInstance();
    const uint32_t first = registry.RegisterWeapon(MakeDefinition("WeaponMechanicsReal_Alpha"));
    const uint32_t second = registry.RegisterWeapon(MakeDefinition("WeaponMechanicsReal_Beta"));

    // definitionID 0 is the "no weapon" sentinel used by HasWeaponInSlot.
    EXPECT_NE(first, 0u);
    EXPECT_NE(second, 0u);
    EXPECT_NE(first, second);
}

TEST(WeaponMechanicsReal_RegistryLookupByIDAndName)
{
    auto& registry = WeaponRegistry::GetInstance();
    const uint32_t id = registry.RegisterWeapon(MakeDefinition("WeaponMechanicsReal_Gamma"));

    const WeaponDefinition* byID = registry.GetWeapon(id);
    ASSERT_TRUE(byID != nullptr);
    EXPECT_TRUE(byID->name == "WeaponMechanicsReal_Gamma");
    EXPECT_EQ(byID->definitionID, id);
    EXPECT_EQ(byID->magazineSize, 30);

    const WeaponDefinition* byName = registry.GetWeaponByName("WeaponMechanicsReal_Gamma");
    ASSERT_TRUE(byName != nullptr);
    EXPECT_EQ(byName->definitionID, id);
}

TEST(WeaponMechanicsReal_RegistryMissesReturnNull)
{
    auto& registry = WeaponRegistry::GetInstance();
    // 0 is the sentinel and is never handed out by RegisterWeapon.
    EXPECT_TRUE(registry.GetWeapon(0) == nullptr);
    EXPECT_TRUE(registry.GetWeaponByName("WeaponMechanicsReal_NeverRegistered") == nullptr);
}

TEST(WeaponMechanicsReal_FireCallbackRegistrationYieldsDistinctHandles)
{
    using HandlerID = Spark::Delegate<const WeaponFireEvent&>::HandlerID;

    WeaponSystem system;
    const HandlerID first = system.OnFire([](const WeaponFireEvent&) {});
    const HandlerID second = system.OnFire([](const WeaponFireEvent&) {});

    // Delegate hands out 1-based handler IDs; 0 would mean "not registered",
    // and two subscribers must never collide on the same removable handle.
    EXPECT_NE(first, static_cast<HandlerID>(0));
    EXPECT_NE(second, static_cast<HandlerID>(0));
    EXPECT_NE(first, second);
}

TEST(WeaponMechanicsReal_FireConsumesAmmoAndEmitsEvent)
{
    WeaponDefinition def = MakeDefinition("WeaponMechanicsReal_FireEvent");
    def.isHitscan = false;
    def.muzzleVelocity = 450.0f;
    WeaponRig rig(def);
    rig.inventory->inputFire = true;

    rig.system.Update(0.0f);

    EXPECT_EQ(rig.Active().currentAmmo, 29);
    EXPECT_TRUE(rig.Active().state == WeaponState::Firing);
    EXPECT_NEAR(rig.Active().fireCooldown, 0.1f, 0.0001f);
    ASSERT_EQ(rig.fired.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(rig.fired[0].weaponDefID, rig.Active().definitionID);
    EXPECT_NEAR(rig.fired[0].damage, 20.0f, 0.0001f);
    EXPECT_FALSE(rig.fired[0].isHitscan);
    EXPECT_NEAR(rig.fired[0].muzzleVelocity, 450.0f, 0.0001f);
}

TEST(WeaponMechanicsReal_FireCooldownHoldsUntilShotIntervalElapses)
{
    WeaponRig rig(MakeDefinition("WeaponMechanicsReal_Cooldown"));
    rig.inventory->inputFire = true; // full-auto: the trigger stays held

    rig.system.Update(0.0f);
    rig.system.Update(0.05f); // half of the 0.1s interval
    EXPECT_EQ(rig.Active().currentAmmo, 29);
    EXPECT_EQ(rig.fired.size(), static_cast<std::size_t>(1));

    rig.system.Update(0.06f); // interval elapsed: back to Idle, and it fires again this frame
    EXPECT_EQ(rig.Active().currentAmmo, 28);
    EXPECT_EQ(rig.fired.size(), static_cast<std::size_t>(2));
}

TEST(WeaponMechanicsReal_EmptyMagazineAutoReloadsWithFullReloadTime)
{
    WeaponRig rig(MakeDefinition("WeaponMechanicsReal_EmptyReload"), 0, 60);
    rig.inventory->inputFire = true;

    rig.system.Update(0.0f);
    EXPECT_TRUE(rig.Active().state == WeaponState::Empty);
    EXPECT_TRUE(rig.fired.empty());

    rig.inventory->inputFire = false;
    rig.system.Update(0.0f);
    EXPECT_TRUE(rig.Active().state == WeaponState::Reloading);
    EXPECT_NEAR(rig.Active().stateTimer, 2.5f, 0.0001f); // reloadTime: nothing chambered

    rig.system.Update(2.4f);
    EXPECT_TRUE(rig.Active().state == WeaponState::Reloading);
    rig.system.Update(0.2f);
    EXPECT_TRUE(rig.Active().state == WeaponState::Idle);
    EXPECT_EQ(rig.Active().currentAmmo, 30);
    EXPECT_EQ(rig.Active().reserveAmmo, 30);
}

TEST(WeaponMechanicsReal_ManualReloadIsTacticalAndDrawsOnlyWhatReserveHolds)
{
    WeaponRig rig(MakeDefinition("WeaponMechanicsReal_TacticalReload"), 20, 5);
    rig.inventory->inputReload = true;

    rig.system.Update(0.0f);
    EXPECT_TRUE(rig.Active().state == WeaponState::Reloading);
    EXPECT_NEAR(rig.Active().stateTimer, 2.0f, 0.0001f); // tacticalReloadTime: a round is chambered
    EXPECT_FALSE(rig.inventory->inputReload);

    rig.system.Update(2.1f);
    EXPECT_EQ(rig.Active().currentAmmo, 25);
    EXPECT_EQ(rig.Active().reserveAmmo, 0);
}

TEST(WeaponMechanicsReal_NoShotWhileReloadingOrDisabled)
{
    WeaponRig rig(MakeDefinition("WeaponMechanicsReal_Blocked"), 15, 60);
    rig.inventory->inputFire = true;

    rig.Active().state = WeaponState::Disabled;
    rig.system.Update(0.0f);
    EXPECT_EQ(rig.Active().currentAmmo, 15);

    rig.Active().state = WeaponState::Reloading;
    rig.Active().stateTimer = 1.0f;
    rig.system.Update(0.5f);
    EXPECT_EQ(rig.Active().currentAmmo, 15);
    EXPECT_TRUE(rig.fired.empty());
}

TEST(WeaponMechanicsReal_RecoilFirstShotMultiplierClampAndRecovery)
{
    WeaponDefinition def = MakeDefinition("WeaponMechanicsReal_Recoil");
    def.recoil.verticalPerShot = 1.0f;
    def.recoil.firstShotMultiplier = 1.5f;
    def.recoil.maxVertical = 2.0f;
    def.recoil.recoverySpeed = 2.5f;
    WeaponRig rig(def);
    rig.inventory->inputFire = true;

    rig.system.Update(0.0f);
    EXPECT_NEAR(rig.Active().accumulatedVerticalRecoil, 1.5f, 0.0001f); // the first shot kicks 1.5x

    rig.Active().fireCooldown = 0.0f;
    rig.Active().state = WeaponState::Idle;
    rig.system.Update(0.0f);
    EXPECT_NEAR(rig.Active().accumulatedVerticalRecoil, 2.0f, 0.0001f); // 2.5 clamped to maxVertical

    rig.inventory->inputFire = false;
    rig.system.Update(0.4f); // recovers at 2.5 degrees per second
    EXPECT_NEAR(rig.Active().accumulatedVerticalRecoil, 1.0f, 0.0001f);
    rig.system.Update(10.0f);
    EXPECT_NEAR(rig.Active().accumulatedVerticalRecoil, 0.0f, 0.0001f); // floors at zero
}

TEST(WeaponMechanicsReal_FireEventSpreadAppliesADSRecoilAndCap)
{
    WeaponDefinition def = MakeDefinition("WeaponMechanicsReal_Spread");
    def.spread.baseSpread = 2.0f;
    def.spread.adsReduction = 0.5f;
    def.spread.maxSpread = 10.0f;
    def.recoil.verticalPerShot = 1.0f;
    def.recoil.firstShotMultiplier = 1.0f;
    {
        WeaponRig rig(def);
        rig.inventory->inputFire = true;
        rig.system.Update(0.0f);
        ASSERT_EQ(rig.fired.size(), static_cast<std::size_t>(1));
        EXPECT_NEAR(rig.fired[0].spreadAngle, 2.2f, 0.0001f); // base + 0.2 * accumulated recoil
    }
    {
        WeaponDefinition aimed = def;
        aimed.name = "WeaponMechanicsReal_SpreadADS";
        WeaponRig rig(aimed);
        rig.Active().isADS = true;
        rig.inventory->inputADS = true;
        rig.inventory->inputFire = true;
        rig.system.Update(0.0f);
        ASSERT_EQ(rig.fired.size(), static_cast<std::size_t>(1));
        EXPECT_NEAR(rig.fired[0].spreadAngle, 1.2f, 0.0001f); // 2.0 * 0.5 + 0.2
    }
    {
        WeaponDefinition capped = def;
        capped.name = "WeaponMechanicsReal_SpreadCap";
        capped.spread.maxSpread = 1.5f;
        WeaponRig rig(capped);
        rig.inventory->inputFire = true;
        rig.system.Update(0.0f);
        ASSERT_EQ(rig.fired.size(), static_cast<std::size_t>(1));
        EXPECT_NEAR(rig.fired[0].spreadAngle, 1.5f, 0.0001f);
    }
}

TEST(WeaponMechanicsReal_ADSBlendFollowsAdsTime)
{
    WeaponDefinition def = MakeDefinition("WeaponMechanicsReal_ADS");
    def.adsTime = 0.2f;
    WeaponRig rig(def);

    rig.inventory->inputADS = true;
    rig.system.Update(0.1f);
    EXPECT_NEAR(rig.Active().adsBlend, 0.5f, 0.0001f);
    EXPECT_FALSE(rig.Active().isADS);
    rig.system.Update(0.15f);
    EXPECT_NEAR(rig.Active().adsBlend, 1.0f, 0.0001f);
    EXPECT_TRUE(rig.Active().isADS);

    rig.inventory->inputADS = false;
    rig.system.Update(0.3f);
    EXPECT_NEAR(rig.Active().adsBlend, 0.0f, 0.0001f);
    EXPECT_FALSE(rig.Active().isADS);
}

TEST(WeaponMechanicsReal_SwitchHolstersThenEquipsTheTargetSlot)
{
    WeaponDefinition primary = MakeDefinition("WeaponMechanicsReal_SwitchPrimary");
    primary.holsterTime = 0.3f;
    WeaponRig rig(primary);
    WeaponDefinition secondary = MakeDefinition("WeaponMechanicsReal_SwitchSecondary");
    secondary.slot = WeaponSlot::Secondary;
    secondary.equipTime = 0.5f;
    WeaponInstance& sidearm = rig.inventory->weapons[static_cast<std::size_t>(WeaponSlot::Secondary)];
    sidearm.definitionID = WeaponRegistry::GetInstance().RegisterWeapon(secondary);
    sidearm.currentAmmo = 12;

    rig.inventory->inputSwitchSlot = static_cast<int>(WeaponSlot::Secondary);
    rig.system.Update(0.0f);
    EXPECT_TRUE(rig.Active().state == WeaponState::Switching);
    EXPECT_NEAR(rig.Active().stateTimer, 0.3f, 0.0001f);
    EXPECT_EQ(rig.inventory->inputSwitchSlot, -1);

    rig.system.Update(0.3f); // holster done: the secondary starts equipping
    EXPECT_TRUE(rig.inventory->activeSlot == WeaponSlot::Secondary);
    EXPECT_TRUE(rig.Active().state == WeaponState::Switching);
    EXPECT_NEAR(rig.Active().stateTimer, 0.5f, 0.0001f);

    rig.system.Update(0.5f);
    EXPECT_TRUE(rig.Active().state == WeaponState::Idle);
    EXPECT_EQ(rig.Active().currentAmmo, 12);
}
