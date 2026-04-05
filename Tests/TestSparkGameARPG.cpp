// TestSparkGameARPG.cpp - Tests for ARPG game module mechanics
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <cstdint>

namespace TestARPG
{

    enum class Rarity
    {
        Common,
        Uncommon,
        Rare,
        Epic,
        Legendary
    };

    struct LootTableEntry
    {
        std::string name;
        Rarity rarity;
        int weight;
    };

    // Simple deterministic LCG for reproducible tests
    class DeterministicRNG
    {
      public:
        explicit DeterministicRNG(uint32_t seed) : m_state(seed) {}

        uint32_t Next()
        {
            m_state = m_state * 1103515245u + 12345u;
            return (m_state >> 16) & 0x7FFF;
        }

        int Range(int min, int max) { return min + static_cast<int>(Next() % static_cast<uint32_t>(max - min + 1)); }

        float Float01() { return static_cast<float>(Next()) / 32767.0f; }

      private:
        uint32_t m_state;
    };

    static Rarity RollLoot(const std::vector<LootTableEntry>& table, DeterministicRNG& rng)
    {
        int totalWeight = 0;
        for (const auto& entry : table)
        {
            totalWeight += entry.weight;
        }

        int roll = rng.Range(0, totalWeight - 1);
        int cumulative = 0;
        for (const auto& entry : table)
        {
            cumulative += entry.weight;
            if (roll < cumulative)
            {
                return entry.rarity;
            }
        }
        return table.back().rarity;
    }

    struct Skill
    {
        std::string name;
        float cooldownDuration = 5.0f;
        float cooldownRemaining = 0.0f;

        bool IsReady() const { return cooldownRemaining <= 0.0f; }

        void Use()
        {
            if (IsReady())
            {
                cooldownRemaining = cooldownDuration;
            }
        }

        void Update(float dt) { cooldownRemaining = std::max(0.0f, cooldownRemaining - dt); }
    };

    struct DamageOverTime
    {
        float damagePerTick = 10.0f;
        float tickInterval = 1.0f;
        float duration = 5.0f;
        float elapsed = 0.0f;
        float timeSinceLastTick = 0.0f;
        float totalDamageDealt = 0.0f;

        bool IsActive() const { return elapsed < duration; }

        void Update(float dt)
        {
            if (!IsActive())
            {
                return;
            }

            elapsed += dt;
            timeSinceLastTick += dt;

            while (timeSinceLastTick >= tickInterval && elapsed <= duration + tickInterval)
            {
                totalDamageDealt += damagePerTick;
                timeSinceLastTick -= tickInterval;
            }
        }
    };

    static float ClampResistance(float resistance, float maxCap = 0.75f)
    {
        return std::min(resistance, maxCap);
    }

} // namespace TestARPG

TEST(ARPG_LootTableRolls)
{
    std::vector<TestARPG::LootTableEntry> table = {
        {"Common Sword", TestARPG::Rarity::Common, 60},      {"Uncommon Sword", TestARPG::Rarity::Uncommon, 25},
        {"Rare Sword", TestARPG::Rarity::Rare, 10},          {"Epic Sword", TestARPG::Rarity::Epic, 4},
        {"Legendary Sword", TestARPG::Rarity::Legendary, 1},
    };

    TestARPG::DeterministicRNG rng(42);

    int commonCount = 0;
    int legendaryCount = 0;
    const int totalRolls = 1000;

    for (int i = 0; i < totalRolls; i++)
    {
        auto rarity = TestARPG::RollLoot(table, rng);
        if (rarity == TestARPG::Rarity::Common)
        {
            commonCount++;
        }
        if (rarity == TestARPG::Rarity::Legendary)
        {
            legendaryCount++;
        }
    }

    // Common should appear most often (60% weight)
    EXPECT_GT(commonCount, 400);
    // Legendary should be very rare (1% weight)
    EXPECT_LT(legendaryCount, 50);
    // Common should outnumber legendary
    EXPECT_GT(commonCount, legendaryCount);
}

TEST(ARPG_SkillCooldowns)
{
    TestARPG::Skill fireball;
    fireball.name = "Fireball";
    fireball.cooldownDuration = 3.0f;

    EXPECT_TRUE(fireball.IsReady());

    fireball.Use();
    EXPECT_FALSE(fireball.IsReady());
    EXPECT_NEAR(fireball.cooldownRemaining, 3.0f, 0.001f);

    // Advance 2 seconds: still on cooldown
    fireball.Update(2.0f);
    EXPECT_FALSE(fireball.IsReady());
    EXPECT_NEAR(fireball.cooldownRemaining, 1.0f, 0.001f);

    // Advance 1 more second: should be ready
    fireball.Update(1.0f);
    EXPECT_TRUE(fireball.IsReady());
    EXPECT_NEAR(fireball.cooldownRemaining, 0.0f, 0.001f);
}

TEST(ARPG_ProcChances)
{
    TestARPG::DeterministicRNG rng(12345);

    float procChance = 0.25f;
    int procCount = 0;
    const int trials = 1000;

    for (int i = 0; i < trials; i++)
    {
        if (rng.Float01() < procChance)
        {
            procCount++;
        }
    }

    // With 25% chance over 1000 trials, expect roughly 250 procs
    // Allow wide margin for deterministic RNG distribution
    EXPECT_GT(procCount, 100);
    EXPECT_LT(procCount, 500);
}

TEST(ARPG_DotDamage)
{
    TestARPG::DamageOverTime dot;
    dot.damagePerTick = 15.0f;
    dot.tickInterval = 1.0f;
    dot.duration = 5.0f;

    EXPECT_TRUE(dot.IsActive());
    EXPECT_NEAR(dot.totalDamageDealt, 0.0f, 0.001f);

    // Tick at 1 second
    dot.Update(1.0f);
    EXPECT_NEAR(dot.totalDamageDealt, 15.0f, 0.001f);

    // Tick at 2 seconds
    dot.Update(1.0f);
    EXPECT_NEAR(dot.totalDamageDealt, 30.0f, 0.001f);

    // Tick at 3, 4, 5 seconds
    dot.Update(3.0f);
    EXPECT_NEAR(dot.totalDamageDealt, 75.0f, 0.001f);

    // DoT has expired
    EXPECT_FALSE(dot.IsActive());
}

TEST(ARPG_MaxResistanceCap)
{
    // Below cap: unchanged
    EXPECT_NEAR(TestARPG::ClampResistance(0.5f), 0.5f, 0.001f);

    // At cap: unchanged
    EXPECT_NEAR(TestARPG::ClampResistance(0.75f), 0.75f, 0.001f);

    // Above cap: clamped to 0.75
    EXPECT_NEAR(TestARPG::ClampResistance(0.90f), 0.75f, 0.001f);
    EXPECT_NEAR(TestARPG::ClampResistance(1.0f), 0.75f, 0.001f);

    // Zero: unchanged
    EXPECT_NEAR(TestARPG::ClampResistance(0.0f), 0.0f, 0.001f);

    // Negative: unchanged (below cap)
    EXPECT_NEAR(TestARPG::ClampResistance(-0.1f), -0.1f, 0.001f);
}
