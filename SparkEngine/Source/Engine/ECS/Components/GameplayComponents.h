/**
 * @file GameplayComponents.h
 * @brief ECS gameplay components: Tags, Health, Active, Weather, Inventory, Quests
 *
 * Split from the monolithic Components.h for faster compilation and
 * clearer separation of concerns.
 */

#pragma once
#include <string>
#include <vector>
#include <algorithm>

// =============================================================================
// TagComponent
// =============================================================================

struct TagComponent
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

// =============================================================================
// ActiveComponent
// =============================================================================

struct ActiveComponent
{
    bool active = true;
};

// =============================================================================
// HealthComponent
// =============================================================================

struct HealthComponent
{
    float health = 100.0f;
    float maxHealth = 100.0f;
    bool isDead = false;
    bool deathProcessed = false;

    void TakeDamage(float amount)
    {
        health = (std::max)(health - amount, 0.0f);
        isDead = (health <= 0.0f);
    }

    void Heal(float amount)
    {
        health = (std::min)(health + amount, maxHealth);
        isDead = false;
        deathProcessed = false;
    }
};

// =============================================================================
// WeatherComponent
// =============================================================================

struct WeatherComponent
{
    int weatherType = 0;
    float intensity = 0.0f;
    float windX = 1.0f, windY = 0.0f, windZ = 0.0f;
    float windSpeed = 0.0f;
    float transitionTime = 3.0f;
    bool enabled = true;
};

// =============================================================================
// InventoryTag
// =============================================================================

struct InventoryTag
{
    int maxSlots = 20;
    float maxWeight = 100.0f;
    int currency = 0;
    bool hasInventory = true;
};

// =============================================================================
// QuestTrackerTag
// =============================================================================

struct QuestTrackerTag
{
    int activeQuestCount = 0;
    int completedQuestCount = 0;
    bool questLogOpen = false;
};
