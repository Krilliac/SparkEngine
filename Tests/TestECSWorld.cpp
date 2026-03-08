// TestECSWorld.cpp - Tests for the ECS World and Components
// Note: Requires EnTT header to be available

#include "TestFramework.h"

// Minimal test stubs for when EnTT is not available during CI
// These test the component data structures independently

#include <string>
#include <vector>
#include <algorithm>

// Standalone HealthComponent test (doesn't need EnTT)
struct TestHealthComponent
{
    float health = 100.0f;
    float maxHealth = 100.0f;
    bool isDead = false;

    void TakeDamage(float amount)
    {
        health = (std::max)(health - amount, 0.0f);
        isDead = (health <= 0.0f);
    }

    void Heal(float amount)
    {
        health = (std::min)(health + amount, maxHealth);
        isDead = false;
    }
};

TEST(HealthComponent_InitialState)
{
    TestHealthComponent hp;
    EXPECT_NEAR(hp.health, 100.0f, 0.001f);
    EXPECT_NEAR(hp.maxHealth, 100.0f, 0.001f);
    EXPECT_FALSE(hp.isDead);
}

TEST(HealthComponent_TakeDamage)
{
    TestHealthComponent hp;
    hp.TakeDamage(30.0f);
    EXPECT_NEAR(hp.health, 70.0f, 0.001f);
    EXPECT_FALSE(hp.isDead);
}

TEST(HealthComponent_TakeLethalDamage)
{
    TestHealthComponent hp;
    hp.TakeDamage(150.0f);
    EXPECT_NEAR(hp.health, 0.0f, 0.001f);
    EXPECT_TRUE(hp.isDead);
}

TEST(HealthComponent_Heal)
{
    TestHealthComponent hp;
    hp.TakeDamage(50.0f);
    hp.Heal(20.0f);
    EXPECT_NEAR(hp.health, 70.0f, 0.001f);
}

TEST(HealthComponent_HealCannotExceedMax)
{
    TestHealthComponent hp;
    hp.TakeDamage(10.0f);
    hp.Heal(50.0f);
    EXPECT_NEAR(hp.health, 100.0f, 0.001f);
}

TEST(HealthComponent_ReviveFromDead)
{
    TestHealthComponent hp;
    hp.TakeDamage(100.0f);
    EXPECT_TRUE(hp.isDead);
    hp.Heal(50.0f);
    EXPECT_FALSE(hp.isDead);
    EXPECT_NEAR(hp.health, 50.0f, 0.001f);
}

// Tag component tests
struct TestTagComponent
{
    std::vector<std::string> tags;

    bool HasTag(const std::string& tag) const
    {
        for (const auto& t : tags)
            if (t == tag)
                return true;
        return false;
    }

    void AddTag(const std::string& tag)
    {
        if (!HasTag(tag))
            tags.push_back(tag);
    }
};

TEST(TagComponent_AddAndCheck)
{
    TestTagComponent tags;
    tags.AddTag("enemy");
    tags.AddTag("boss");
    EXPECT_TRUE(tags.HasTag("enemy"));
    EXPECT_TRUE(tags.HasTag("boss"));
    EXPECT_FALSE(tags.HasTag("player"));
}

TEST(TagComponent_NoDuplicates)
{
    TestTagComponent tags;
    tags.AddTag("test");
    tags.AddTag("test");
    EXPECT_EQ(tags.tags.size(), 1u);
}
