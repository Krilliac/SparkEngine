/**
 * @file TestDestructionSystem.cpp
 * @brief Tests for Spark::DestructionSystem and DestructibleComponent
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Engine/Destruction/DestructionSystem.h"

TEST(Destructible_ApplyDamage)
{
    Spark::DestructibleComponent comp;
    comp.health = 100.0f;
    comp.maxHealth = 100.0f;

    EXPECT_FALSE(comp.ApplyDamage(30.0f));
    EXPECT_NEAR(comp.health, 70.0f, 0.01f);
    EXPECT_FALSE(comp.isDestroyed);

    EXPECT_TRUE(comp.ApplyDamage(80.0f));
    EXPECT_TRUE(comp.isDestroyed);
    EXPECT_NEAR(comp.health, 0.0f, 0.01f);
}

TEST(Destructible_DamageThreshold)
{
    Spark::DestructibleComponent comp;
    comp.health = 100.0f;
    comp.damageThreshold = 10.0f;

    EXPECT_FALSE(comp.ApplyDamage(5.0f));
    EXPECT_NEAR(comp.health, 100.0f, 0.01f); // Below threshold, no damage

    EXPECT_FALSE(comp.ApplyDamage(15.0f));
    EXPECT_NEAR(comp.health, 85.0f, 0.01f); // Above threshold
}

TEST(Destructible_DamageStages)
{
    Spark::DestructibleComponent comp;
    comp.health = 100.0f;
    comp.maxHealth = 100.0f;
    comp.maxDamageStages = 3;

    comp.ApplyDamage(50.0f);
    EXPECT_EQ(comp.damageStage, 1); // 50% health = stage 1 of 3
}

TEST(Destruction_RegisterPattern)
{
    auto& sys = Spark::DestructionSystem::GetInstance();
    Spark::FracturePattern pattern;
    pattern.AddPiece({"piece1", "mesh1", {0, 0, 0}, 1.0f, 10.0f, 5.0f});
    pattern.AddPiece({"piece2", "mesh2", {1, 0, 0}, 1.0f, 10.0f, 5.0f});
    pattern.SetDestructionSound("break_sfx");

    sys.RegisterPattern("test_pattern", pattern);

    const auto* p = sys.GetPattern("test_pattern");
    EXPECT_TRUE(p != nullptr);
    EXPECT_EQ(p->GetPieces().size(), static_cast<size_t>(2));
    EXPECT_EQ(p->GetDestructionSound(), std::string("break_sfx"));
}

TEST(Destruction_DebrisCleanup)
{
    auto& sys = Spark::DestructionSystem::GetInstance();
    sys.Initialize();
    sys.SetMaxDebris(100);

    DirectX::XMFLOAT3 pos{0, 0, 0};
    DirectX::XMFLOAT3 dir{0, 1, 0};
    sys.ApplyDamage(1, 100.0f, pos, dir);

    EXPECT_TRUE(sys.GetActiveDebrisCount() > static_cast<size_t>(0));

    // Simulate time passing to expire debris
    for (int i = 0; i < 600; ++i)
    {
        sys.Update(1.0f / 60.0f);
    }
    // After 10 seconds, debris should be cleaned up
    EXPECT_EQ(sys.GetActiveDebrisCount(), static_cast<size_t>(0));
}
