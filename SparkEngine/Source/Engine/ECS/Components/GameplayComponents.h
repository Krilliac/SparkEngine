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
#include <unordered_set>
#include <algorithm>

// =============================================================================
// TagComponent
// =============================================================================

struct TagComponent
{
    std::unordered_set<std::string> tags;

    bool HasTag(const std::string& tag) const { return tags.count(tag) > 0; }

    void AddTag(const std::string& tag) { tags.insert(tag); }

    void RemoveTag(const std::string& tag) { tags.erase(tag); }
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
        if (isDead)
            return;
        health = (std::max)(health - amount, 0.0f);
        isDead = (health <= 0.0f);
    }

    void Heal(float amount)
    {
        if (isDead)
            return; // Dead entities cannot be healed; use Revive() instead
        health = (std::min)(health + amount, maxHealth);
    }

    void Revive(float healthAmount)
    {
        health = (std::min)(healthAmount, maxHealth);
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
