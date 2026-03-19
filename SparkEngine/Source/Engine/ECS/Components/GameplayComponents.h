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

/**
 * @brief Arbitrary string tags for entity classification and querying.
 *
 * Game code can filter entities by tag (e.g. "enemy", "pickup", "destructible")
 * without needing a dedicated component for each category.
 */
struct TagComponent
{
    std::unordered_set<std::string> tags; ///< Set of tag strings (case-sensitive).

    bool HasTag(const std::string& tag) const { return tags.count(tag) > 0; }

    void AddTag(const std::string& tag) { tags.insert(tag); }

    void RemoveTag(const std::string& tag) { tags.erase(tag); }
};

// =============================================================================
// ActiveComponent
// =============================================================================

/**
 * @brief Master enable/disable toggle for an entity.
 *
 * When `active` is false, most systems (Render, Audio, AI, Physics) skip
 * this entity. Useful for object pooling — deactivate instead of destroying.
 */
struct ActiveComponent
{
    bool active = true; ///< When false, the entity is treated as dormant by all systems.
};

// =============================================================================
// HealthComponent
// =============================================================================

/**
 * @brief Tracks entity health, death state, and provides damage/heal/revive operations.
 *
 * The LifecycleSystem watches for `isDead` transitions and fires death
 * callbacks. Once `deathProcessed` is set, the entity won't trigger
 * the callback again (prevents double-processing).
 */
struct HealthComponent
{
    float health = 100.0f;       ///< Current health points.
    float maxHealth = 100.0f;    ///< Maximum health cap (Heal cannot exceed this).
    bool isDead = false;         ///< Set to true when health reaches zero.
    bool deathProcessed = false; ///< Set by LifecycleSystem after firing the death callback.

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
    int weatherType = 0;    ///< Weather preset index (0 = clear, 1 = rain, 2 = snow, etc.).
    float intensity = 0.0f; ///< Effect intensity [0, 1]; controls particle density and sound volume.
    float windX = 1.0f, windY = 0.0f, windZ = 0.0f; ///< Wind direction vector (not normalized).
    float windSpeed = 0.0f;                         ///< Wind speed in m/s; affects particle drift and vegetation sway.
    float transitionTime = 3.0f; ///< Blend duration when entering/leaving this weather zone (seconds).
    bool enabled = true;         ///< Runtime toggle for this weather zone.
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
    int maxSlots = 20;        ///< Maximum number of item slots in this inventory.
    float maxWeight = 100.0f; ///< Maximum carry weight (kg); excess items are rejected.
    int currency = 0;         ///< Currency balance (gold/credits) carried by this entity.
    bool hasInventory = true; ///< False to temporarily disable inventory access (e.g. during cutscenes).
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
    int activeQuestCount = 0;    ///< Number of in-progress quests tracked by this entity.
    int completedQuestCount = 0; ///< Lifetime count of completed quests (for statistics/achievements).
    bool questLogOpen = false;   ///< Whether the quest log UI is currently visible for this entity.
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
