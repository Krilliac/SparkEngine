/**
 * @file GameplayComponents.h
 * @brief ECS gameplay components: Tags, Health, Active, Weather, Inventory, Quests
 *
 * Split from the monolithic Components.h for faster compilation and
 * clearer separation of concerns.
 */

#pragma once
#include "../../../Utils/BitFlags.h"
#include "../../../Utils/Cooldown.h"
#include "../../../Utils/Assert.h"
#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

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
        ASSERT_MSG(amount >= 0.0f, "TakeDamage amount must be non-negative");
        if (isDead)
            return;
        health = (std::max)(health - amount, 0.0f);
        isDead = (health <= 0.0f);
    }

    void Heal(float amount)
    {
        ASSERT_MSG(amount >= 0.0f, "Heal amount must be non-negative");
        if (isDead)
            return; // Dead entities cannot be healed; use Revive() instead
        health = (std::min)(health + amount, maxHealth);
    }

    void Revive(float healthAmount)
    {
        ASSERT_MSG(healthAmount > 0.0f, "Revive healthAmount must be positive");
        health = (std::min)(healthAmount, maxHealth);
        isDead = false;
        deathProcessed = false;
    }

    /**
     * @brief Validate that health parameters are within sane ranges.
     * @return true if all parameters are valid.
     */
    bool Validate() const
    {
        ASSERT_MSG(maxHealth > 0.0f, "maxHealth must be positive");
        ASSERT_MSG(health >= 0.0f && health <= maxHealth, "health must be in [0, maxHealth]");
        return maxHealth > 0.0f && health >= 0.0f && health <= maxHealth;
    }
};

// =============================================================================
// WeatherComponent — marks entities as weather-affected zones
// =============================================================================

/**
 * @brief Per-entity weather zone data.
 *
 * Attach to trigger/zone entities to define localized weather areas.
 * The global Spark::WeatherSystem reads these when entities overlap
 * the player to blend weather transitions.
 *
 * @note Data-only component — game code queries via World::view<WeatherComponent>().
 */
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
// InventoryTag — marks entities that have an inventory
// =============================================================================

/**
 * @brief Tag component for entities that carry an inventory.
 *
 * Game code queries this via World::view<InventoryTag>() to find entities
 * with inventories (players, chests, NPCs). Inventory management logic
 * lives in game modules, not the engine.
 *
 * @note Data-only marker — no engine-level system processes it.
 */
struct InventoryTag
{
    int maxSlots = 20;
    float maxWeight = 100.0f;
    int currency = 0;
    bool hasInventory = true;
};

// =============================================================================
// QuestTrackerTag — marks entities that participate in quests
// =============================================================================

/**
 * @brief Tag component for entities with quest tracking.
 *
 * Game code queries this to find entities involved in quests.
 * Quest progression logic lives in game modules.
 *
 * @note Data-only marker — no engine-level system processes it.
 */
struct QuestTrackerTag
{
    int activeQuestCount = 0;
    int completedQuestCount = 0;
    bool questLogOpen = false;
};

// =============================================================================
// AbilityFlags — type-safe bitmask for entity capability tracking
// =============================================================================

enum class AbilityFlags : uint32_t
{
    None = 0,
    CanJump = 1 << 0,
    CanSprint = 1 << 1,
    CanCrouch = 1 << 2,
    CanShoot = 1 << 3,
    CanMelee = 1 << 4,
    CanInteract = 1 << 5,
    CanSwim = 1 << 6,
    CanClimb = 1 << 7,
    All = 0xFFFFFFFF
};
SPARK_ENABLE_BITMASK_OPERATORS(AbilityFlags)

// =============================================================================
// AbilityComponent — tracks which abilities an entity has and their cooldowns
// =============================================================================

struct AbilityComponent
{
    Spark::BitFlags<AbilityFlags> abilities{AbilityFlags::CanJump | AbilityFlags::CanShoot | AbilityFlags::CanInteract};

    /// Primary ability cooldown (e.g. fire rate)
    Spark::Cooldown primaryCooldown{0.0f, true};

    /// Secondary ability cooldown (e.g. grenade throw)
    Spark::Cooldown secondaryCooldown{0.0f, true};

    /// Dash/sprint cooldown
    Spark::Cooldown sprintCooldown{0.0f, true};

    void Update(float deltaTime)
    {
        primaryCooldown.Update(deltaTime);
        secondaryCooldown.Update(deltaTime);
        sprintCooldown.Update(deltaTime);
    }
};
