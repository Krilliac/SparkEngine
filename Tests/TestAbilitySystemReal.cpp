/**
 * @file TestAbilitySystemReal.cpp
 * @brief Real-class tests for Spark::Gameplay::AbilitySystem (registration/query API)
 */

#include "TestFramework.h"
#include "Engine/Gameplay/AbilitySystem.h"

namespace
{
    void ResetAbilitySystem()
    {
        auto& sys = Spark::Gameplay::AbilitySystem::GetInstance();
        sys.Shutdown();
        sys.Initialize(nullptr); // no event bus needed for registration tests
    }
} // namespace

TEST(AbilitySystemReal_SingletonStable)
{
    auto& a = Spark::Gameplay::AbilitySystem::GetInstance();
    auto& b = Spark::Gameplay::AbilitySystem::GetInstance();
    EXPECT_TRUE(&a == &b);
}

TEST(AbilitySystemReal_RegisterAbility)
{
    ResetAbilitySystem();
    auto& sys = Spark::Gameplay::AbilitySystem::GetInstance();

    Spark::Gameplay::AbilityDefinition def;
    def.id = 1;
    def.name = "Fireball";
    def.cooldown = 2.0f;
    def.castTime = 1.5f;
    def.range = 30.0f;
    sys.RegisterAbility(def);

    EXPECT_EQ(sys.GetRegisteredAbilityCount(), 1);
    auto* retrieved = sys.GetAbilityDef(1);
    ASSERT_TRUE(retrieved != nullptr);
    EXPECT_EQ(retrieved->name, std::string("Fireball"));
    EXPECT_NEAR(retrieved->cooldown, 2.0f, 0.01f);
}

TEST(AbilitySystemReal_RegisterAbilityIdZeroRejected)
{
    ResetAbilitySystem();
    auto& sys = Spark::Gameplay::AbilitySystem::GetInstance();

    Spark::Gameplay::AbilityDefinition def;
    def.id = 0;
    def.name = "Invalid";
    sys.RegisterAbility(def);

    EXPECT_EQ(sys.GetRegisteredAbilityCount(), 0);
    EXPECT_TRUE(sys.GetAbilityDef(0) == nullptr);
}

TEST(AbilitySystemReal_RegisterAura)
{
    ResetAbilitySystem();
    auto& sys = Spark::Gameplay::AbilitySystem::GetInstance();

    Spark::Gameplay::AuraDefinition aura;
    aura.id = 10;
    aura.name = "Burning";
    aura.duration = 8.0f;
    aura.tickInterval = 2.0f;
    aura.valuePerTick = 15.0f;
    sys.RegisterAura(aura);

    EXPECT_EQ(sys.GetRegisteredAuraCount(), 1);
    auto* retrieved = sys.GetAuraDef(10);
    ASSERT_TRUE(retrieved != nullptr);
    EXPECT_EQ(retrieved->name, std::string("Burning"));
    EXPECT_NEAR(retrieved->duration, 8.0f, 0.01f);
}

TEST(AbilitySystemReal_RegisterAuraIdZeroRejected)
{
    ResetAbilitySystem();
    auto& sys = Spark::Gameplay::AbilitySystem::GetInstance();

    Spark::Gameplay::AuraDefinition aura;
    aura.id = 0;
    sys.RegisterAura(aura);

    EXPECT_EQ(sys.GetRegisteredAuraCount(), 0);
}

TEST(AbilitySystemReal_GetMissingReturnsNull)
{
    ResetAbilitySystem();
    auto& sys = Spark::Gameplay::AbilitySystem::GetInstance();

    EXPECT_TRUE(sys.GetAbilityDef(999) == nullptr);
    EXPECT_TRUE(sys.GetAuraDef(999) == nullptr);
}

TEST(AbilitySystemReal_MultipleAbilities)
{
    ResetAbilitySystem();
    auto& sys = Spark::Gameplay::AbilitySystem::GetInstance();

    for (uint32_t i = 1; i <= 5; ++i)
    {
        Spark::Gameplay::AbilityDefinition def;
        def.id = i;
        def.name = "Ability" + std::to_string(i);
        sys.RegisterAbility(def);
    }

    EXPECT_EQ(sys.GetRegisteredAbilityCount(), 5);
    EXPECT_TRUE(sys.GetAbilityDef(3) != nullptr);
    EXPECT_EQ(sys.GetAbilityDef(3)->name, std::string("Ability3"));
}

TEST(AbilitySystemReal_IsCastingWithoutCast)
{
    ResetAbilitySystem();
    auto& sys = Spark::Gameplay::AbilitySystem::GetInstance();
    // No active casts, entity 1 should not be casting
    EXPECT_FALSE(sys.IsCasting(1));
}

TEST(AbilitySystemReal_ShutdownClearsState)
{
    ResetAbilitySystem();
    auto& sys = Spark::Gameplay::AbilitySystem::GetInstance();

    Spark::Gameplay::AbilityDefinition def;
    def.id = 1;
    def.name = "Test";
    sys.RegisterAbility(def);
    EXPECT_EQ(sys.GetRegisteredAbilityCount(), 1);

    sys.Shutdown();
    // After shutdown, registered abilities should still be in the map
    // (Shutdown clears casts/auras/cooldowns but not definitions)
    // Re-initialize to fully clear
    sys.Initialize(nullptr);
    EXPECT_EQ(sys.GetRegisteredAbilityCount(), 0);
}
