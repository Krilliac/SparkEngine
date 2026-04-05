// TestSparkGameRPG.cpp - Tests for RPG game module mechanics
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <string>
#include <cmath>

namespace TestRPG
{

    struct Stats
    {
        int strength = 10;
        int defense = 5;
        int intelligence = 8;
        int vitality = 10;

        int MaxHP() const { return 100 + vitality * 10; }
    };

    struct EquipmentBonus
    {
        int strength = 0;
        int defense = 0;
        int intelligence = 0;
        int vitality = 0;
    };

    struct ResistanceProfile
    {
        float physical = 0.0f;
        float fire = 0.0f;
        float ice = 0.0f;
    };

    struct Character
    {
        std::string name;
        int level = 1;
        int xp = 0;
        int statPoints = 0;
        Stats baseStats;
        EquipmentBonus equipment;

        Stats EffectiveStats() const
        {
            Stats effective;
            effective.strength = baseStats.strength + equipment.strength;
            effective.defense = baseStats.defense + equipment.defense;
            effective.intelligence = baseStats.intelligence + equipment.intelligence;
            effective.vitality = baseStats.vitality + equipment.vitality;
            return effective;
        }

        int XPForNextLevel() const { return level * 100; }

        void AddXP(int amount)
        {
            xp += amount;
            while (xp >= XPForNextLevel())
            {
                xp -= XPForNextLevel();
                level++;
                statPoints += 3;
            }
        }
    };

    static int CalculateDamage(int attackerStrength, int defenderDefense, bool isCritical = false)
    {
        int baseDamage = attackerStrength * 2 - defenderDefense;
        if (baseDamage < 1)
        {
            baseDamage = 1;
        }
        if (isCritical)
        {
            baseDamage = static_cast<int>(baseDamage * 1.5f);
        }
        return baseDamage;
    }

    static int ApplyResistance(int damage, float resistance)
    {
        float reduced = damage * (1.0f - resistance);
        return std::max(1, static_cast<int>(reduced));
    }

} // namespace TestRPG

TEST(RPG_DamageCalculation)
{
    // Attacker with 20 strength vs defender with 10 defense
    int damage = TestRPG::CalculateDamage(20, 10);
    // baseDamage = 20*2 - 10 = 30
    EXPECT_EQ(damage, 30);

    // Low strength vs high defense: floor at 1
    int minDamage = TestRPG::CalculateDamage(3, 50);
    EXPECT_EQ(minDamage, 1);

    // Equal stats
    int equalDamage = TestRPG::CalculateDamage(10, 10);
    // baseDamage = 10*2 - 10 = 10
    EXPECT_EQ(equalDamage, 10);
}

TEST(RPG_StatCalculation)
{
    TestRPG::Character hero;
    hero.name = "TestHero";
    hero.baseStats.strength = 15;
    hero.baseStats.defense = 10;
    hero.baseStats.intelligence = 12;
    hero.baseStats.vitality = 14;

    hero.equipment.strength = 5;
    hero.equipment.defense = 8;
    hero.equipment.vitality = 3;

    auto effective = hero.EffectiveStats();
    EXPECT_EQ(effective.strength, 20);
    EXPECT_EQ(effective.defense, 18);
    EXPECT_EQ(effective.intelligence, 12);
    EXPECT_EQ(effective.vitality, 17);
    EXPECT_EQ(effective.MaxHP(), 100 + 17 * 10);
}

TEST(RPG_LevelUpProgression)
{
    TestRPG::Character hero;
    hero.name = "TestHero";
    EXPECT_EQ(hero.level, 1);
    EXPECT_EQ(hero.statPoints, 0);

    // Level 1 requires 100 XP
    hero.AddXP(50);
    EXPECT_EQ(hero.level, 1);
    EXPECT_EQ(hero.xp, 50);
    EXPECT_EQ(hero.statPoints, 0);

    // Add enough to level up
    hero.AddXP(50);
    EXPECT_EQ(hero.level, 2);
    EXPECT_EQ(hero.xp, 0);
    EXPECT_EQ(hero.statPoints, 3);

    // Add enough for two more levels at once
    // Level 2 requires 200, level 3 requires 300 => need 500 total
    hero.AddXP(500);
    EXPECT_EQ(hero.level, 4);
    EXPECT_EQ(hero.statPoints, 9); // 3 per level * 3 level-ups total
}

TEST(RPG_CriticalHit)
{
    // Normal hit: 25*2 - 10 = 40
    int normalDamage = TestRPG::CalculateDamage(25, 10, false);
    EXPECT_EQ(normalDamage, 40);

    // Critical hit: 40 * 1.5 = 60
    int critDamage = TestRPG::CalculateDamage(25, 10, true);
    EXPECT_EQ(critDamage, 60);

    // Critical on minimum damage: 1 * 1.5 = 1 (int truncation)
    int minCrit = TestRPG::CalculateDamage(3, 50, true);
    EXPECT_EQ(minCrit, 1);
}

TEST(RPG_ResistanceReduction)
{
    // 50% physical resistance on 100 damage
    int reduced = TestRPG::ApplyResistance(100, 0.5f);
    EXPECT_EQ(reduced, 50);

    // 0% resistance: full damage
    int full = TestRPG::ApplyResistance(100, 0.0f);
    EXPECT_EQ(full, 100);

    // 90% resistance on 100 damage
    int heavy = TestRPG::ApplyResistance(100, 0.9f);
    EXPECT_EQ(heavy, 10);

    // 100% resistance: floor at 1
    int immune = TestRPG::ApplyResistance(100, 1.0f);
    EXPECT_EQ(immune, 1);
}
