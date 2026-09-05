/**
 * @file TestWeaponMechanicsReal.cpp
 * @brief Production-source companion for TestWeaponMechanics.cpp
 *
 * TestWeaponMechanics.cpp is a standalone reimplementation of the weapon model;
 * it cannot detect a regression in the shipped code. This file includes the real
 * header and exercises Spark::Gameplay's shipped types: WeaponDefinition,
 * WeaponInstance, WeaponInventoryComponent and WeaponRegistry
 * (Engine/Gameplay/WeaponManager.cpp, already part of SparkEngineLib).
 *
 * WeaponSystem::Update walks the ECS world from EngineContext, so its per-entity
 * state machine is covered by the engine-level integration tests rather than
 * here; what this file pins down is the shipped data model and registry lookup.
 */

#include "TestFramework.h"
#include "Engine/Gameplay/WeaponManager.h"

#include <string>

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
