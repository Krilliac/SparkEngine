// TestECSStress.cpp - Stress tests for ECS component data structures
// Uses standalone structs matching TestECSWorld.cpp patterns

#include "TestFramework.h"
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

// ============================================================================
// Standalone HealthComponent (mirrors TestECSWorld.cpp)
// ============================================================================

struct StressHealthComponent
{
    float health = 100.0f;
    float maxHealth = 100.0f;
    bool isDead = false;
    bool deathProcessed = false;

    void TakeDamage(float amount)
    {
        if (isDead)
            return;
        health = (std::max)(health - amount, 0.0f);
        isDead = (health <= 0.0f);
    }

    void Heal(float amount)
    {
        if (isDead)
            return; // Dead entities cannot be healed; use Revive() instead
        if (amount < 0.0f)
            return; // Negative healing is not allowed
        health = (std::min)(health + amount, maxHealth);
    }

    void Revive(float healthAmount)
    {
        health = (std::min)(healthAmount, maxHealth);
        isDead = false;
        deathProcessed = false;
    }

    float GetHealthPercent() const
    {
        if (maxHealth <= 0.0f)
            return 0.0f;
        return health / maxHealth;
    }
};

// ============================================================================
// Standalone TagComponent (mirrors TestECSWorld.cpp)
// ============================================================================

struct StressTagComponent
{
    std::unordered_set<std::string> tags;

    bool HasTag(const std::string& tag) const { return tags.count(tag) > 0; }

    void AddTag(const std::string& tag) { tags.insert(tag); }

    void RemoveTag(const std::string& tag) { tags.erase(tag); }
};

// ============================================================================
// Minimal entity stub for stress testing
// ============================================================================

struct StressEntity
{
    uint32_t id = 0;
    bool alive = true;
    StressHealthComponent health;
    StressTagComponent tags;
};

// ============================================================================
// Tests
// ============================================================================

TEST(ECSStress_MassiveEntityCreation)
{
    // Create 50000 entities and verify no crash or data corruption
    std::vector<StressEntity> entities;
    entities.reserve(50000);

    for (uint32_t i = 0; i < 50000; ++i)
    {
        StressEntity e;
        e.id = i;
        e.health.health = 100.0f;
        e.health.maxHealth = 100.0f;
        entities.push_back(e);
    }

    EXPECT_EQ(static_cast<int>(entities.size()), 50000);

    // Verify first and last entity are valid
    EXPECT_EQ(static_cast<int>(entities[0].id), 0);
    EXPECT_EQ(static_cast<int>(entities[49999].id), 49999);
    EXPECT_NEAR(entities[49999].health.health, 100.0f, 0.001f);
}

TEST(ECSStress_RapidEntityChurn)
{
    // Create and destroy 10000 entities in alternating fashion
    std::vector<StressEntity> entities;

    for (int i = 0; i < 10000; ++i)
    {
        StressEntity e;
        e.id = static_cast<uint32_t>(i);
        e.health.health = 50.0f;
        entities.push_back(e);

        // Immediately destroy it
        entities.back().alive = false;
        entities.pop_back();
    }

    // After alternating create/destroy, vector should be empty
    EXPECT_EQ(static_cast<int>(entities.size()), 0);

    // Create some and keep them to verify stability after churn
    for (int i = 0; i < 100; ++i)
    {
        StressEntity e;
        e.id = static_cast<uint32_t>(i);
        entities.push_back(e);
    }
    EXPECT_EQ(static_cast<int>(entities.size()), 100);
}

TEST(ECSStress_ComponentSpam)
{
    // Add and remove the same component data 1000 times
    StressEntity entity;
    entity.id = 1;

    for (int i = 0; i < 1000; ++i)
    {
        entity.tags.AddTag("spamTag");
        EXPECT_TRUE(entity.tags.HasTag("spamTag"));
        entity.tags.RemoveTag("spamTag");
        EXPECT_FALSE(entity.tags.HasTag("spamTag"));
    }

    // Entity should be in a clean state after all the churn
    EXPECT_EQ(static_cast<int>(entity.tags.tags.size()), 0);
}

TEST(ECSStress_HealthOverflow)
{
    // Deal INT_MAX damage — health should clamp to 0, not go negative
    StressHealthComponent hp;
    hp.health = 100.0f;
    hp.maxHealth = 100.0f;

    float massiveDamage = static_cast<float>(std::numeric_limits<int>::max());
    hp.TakeDamage(massiveDamage);

    EXPECT_GE(hp.health, 0.0f);
    EXPECT_NEAR(hp.health, 0.0f, 0.001f);
    EXPECT_TRUE(hp.isDead);
}

TEST(ECSStress_HealthNegativeHeal)
{
    // Heal with -10 — should be rejected, health unchanged
    StressHealthComponent hp;
    hp.health = 50.0f;
    hp.maxHealth = 100.0f;

    hp.Heal(-10.0f);

    EXPECT_NEAR(hp.health, 50.0f, 0.001f);
    EXPECT_FALSE(hp.isDead);
}

TEST(ECSStress_HealthZeroMaxHP)
{
    // maxHP=0 — verify no division by zero in GetHealthPercent
    StressHealthComponent hp;
    hp.health = 0.0f;
    hp.maxHealth = 0.0f;
    hp.isDead = true;

    float percent = hp.GetHealthPercent();
    EXPECT_NEAR(percent, 0.0f, 0.001f);

    // Revive with maxHP=0 should clamp health to 0
    hp.Revive(50.0f);
    EXPECT_NEAR(hp.health, 0.0f, 0.001f);
}

TEST(ECSStress_DeadEntityOperations)
{
    // Operations on a dead entity should be no-ops (except Revive)
    StressHealthComponent hp;
    hp.health = 100.0f;
    hp.maxHealth = 100.0f;

    hp.TakeDamage(200.0f);
    EXPECT_TRUE(hp.isDead);
    EXPECT_NEAR(hp.health, 0.0f, 0.001f);

    // Further damage on a dead entity is a no-op
    hp.TakeDamage(50.0f);
    EXPECT_NEAR(hp.health, 0.0f, 0.001f);

    // Healing on a dead entity is a no-op
    hp.Heal(50.0f);
    EXPECT_NEAR(hp.health, 0.0f, 0.001f);
    EXPECT_TRUE(hp.isDead);

    // Only Revive should bring it back
    hp.Revive(75.0f);
    EXPECT_FALSE(hp.isDead);
    EXPECT_NEAR(hp.health, 75.0f, 0.001f);
}

TEST(ECSStress_TagComponentMassiveTags)
{
    // Add 1000 unique tags and verify lookup
    StressTagComponent tags;

    for (int i = 0; i < 1000; ++i)
    {
        tags.AddTag("tag_" + std::to_string(i));
    }

    EXPECT_EQ(static_cast<int>(tags.tags.size()), 1000);

    EXPECT_TRUE(tags.HasTag("tag_0"));
    EXPECT_TRUE(tags.HasTag("tag_500"));
    EXPECT_TRUE(tags.HasTag("tag_999"));
    EXPECT_FALSE(tags.HasTag("tag_1000"));
}

TEST(ECSStress_TagComponentEmptyString)
{
    // Empty string tag should work without crashing
    StressTagComponent tags;

    tags.AddTag("");
    EXPECT_TRUE(tags.HasTag(""));
    EXPECT_EQ(static_cast<int>(tags.tags.size()), 1);

    tags.RemoveTag("");
    EXPECT_FALSE(tags.HasTag(""));
    EXPECT_EQ(static_cast<int>(tags.tags.size()), 0);
}

TEST(ECSStress_TagComponentDuplicateAdd)
{
    // Add the same tag 100 times — unordered_set ensures uniqueness
    StressTagComponent tags;

    for (int i = 0; i < 100; ++i)
    {
        tags.AddTag("duplicate");
    }

    EXPECT_EQ(static_cast<int>(tags.tags.size()), 1);
    EXPECT_TRUE(tags.HasTag("duplicate"));

    // Single removal should clear it
    tags.RemoveTag("duplicate");
    EXPECT_FALSE(tags.HasTag("duplicate"));
    EXPECT_EQ(static_cast<int>(tags.tags.size()), 0);
}
