/**
 * @file LootSystem.h
 * @brief Enemy loot drops and timed power-up buffs
 *
 * When enemies die they drop loot (items, ammo, health). Power-ups
 * grant temporary buffs (speed boost, damage boost, shield regen, etc.)
 * that expire after a configurable duration.
 */

#pragma once
#include "Core/Platform.h"
#include "Enums/GameSystemEnums.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif

#include <vector>
#include <string>
#include <functional>

class Player;

namespace Spark
{

    /**
     * @brief Types of power-up pickups
     */
    enum class PowerUpType
    {
        SpeedBoost,   ///< +50% move speed for duration
        DamageBoost,  ///< +50% weapon damage for duration
        ShieldRegen,  ///< Rapid shield regeneration
        InfiniteAmmo, ///< No ammo consumption
        DoubleXP,     ///< 2x XP gain
        Invisibility, ///< Reduced enemy detection range
        COUNT
    };

    /**
     * @brief An active power-up buff on the player
     */
    struct ActiveBuff
    {
        PowerUpType type;
        float duration;  ///< Total duration in seconds
        float remaining; ///< Time remaining
        float magnitude; ///< Effect strength (e.g., 1.5 for +50%)
    };

    /**
     * @brief A dropped item in the world waiting for pickup
     */
    struct WorldDrop
    {
        DirectX::XMFLOAT3 position;
        float lifetime;       ///< Seconds before despawn
        float bobTimer{0.0f}; ///< Animation timer for floating effect

        enum class DropType
        {
            HealthOrb,
            ArmorShard,
            AmmoPack,
            PowerUp,
            ItemDrop
        } type;

        PowerUpType powerUpType{PowerUpType::SpeedBoost}; ///< Only valid if type == PowerUp
        uint32_t itemDefId{0};                            ///< Only valid if type == ItemDrop
        float value{0.0f};                                ///< Health/armor amount or item count
        bool collected{false};
    };

    /**
     * @brief Loot table entry for enemy drops
     */
    struct LootEntry
    {
        WorldDrop::DropType type;
        float dropChance; ///< 0.0 to 1.0 probability
        float minValue;
        float maxValue;
        PowerUpType powerUpType{PowerUpType::SpeedBoost};
        uint32_t itemDefId{0};
    };

    /**
     * @brief Callbacks for loot events
     */
    struct LootCallbacks
    {
        std::function<void(PowerUpType type, float duration)> onPowerUpCollected;
        std::function<void(PowerUpType type)> onPowerUpExpired;
        std::function<void(const WorldDrop& drop)> onDropCollected;
    };

    /**
     * @brief Manages enemy loot drops and active power-up buffs
     */
    class LootSystem
    {
      public:
        LootSystem();
        ~LootSystem() = default;

        void Initialize();

        /**
         * @brief Update drops and active buffs
         * @param dt Delta time
         * @param player Player for proximity checks and buff application
         */
        void Update(float dt, Player* player);

        /**
         * @brief Spawn loot drops at an enemy death position
         * @param position Where the enemy died
         * @param enemyType Affects loot quality/quantity
         * @param isBossWave Boss enemies drop better loot
         */
        void SpawnEnemyLoot(const DirectX::XMFLOAT3& position, int enemyType, bool isBossWave = false);

        /**
         * @brief Spawn a specific power-up at a location
         */
        void SpawnPowerUp(PowerUpType type, const DirectX::XMFLOAT3& position, float duration = 15.0f);

        // === Buff Queries ===

        bool HasBuff(PowerUpType type) const;
        float GetBuffTimeRemaining(PowerUpType type) const;
        float GetBuffMagnitude(PowerUpType type) const;
        const std::vector<ActiveBuff>& GetActiveBuffs() const { return m_activeBuffs; }

        // === Configuration ===

        void SetPickupRadius(float radius) { m_pickupRadius = radius; }
        void SetDropLifetime(float seconds) { m_dropLifetime = seconds; }
        LootCallbacks& GetCallbacks() { return m_callbacks; }

        /**
         * @brief Get the power-up name for display
         */
        static const char* GetPowerUpName(PowerUpType type);

        // Console integration
        std::string Console_GetStatus() const;

      private:
        void ApplyBuff(PowerUpType type, float duration);
        void CollectDrop(WorldDrop& drop, Player* player);
        void BuildLootTable();

        std::vector<WorldDrop> m_worldDrops;
        std::vector<ActiveBuff> m_activeBuffs;
        std::vector<LootEntry> m_lootTable;
        std::vector<LootEntry> m_bossLootTable;

        float m_pickupRadius{2.5f};
        float m_dropLifetime{30.0f};
        LootCallbacks m_callbacks;

        static constexpr int MAX_WORLD_DROPS = 64;
    };

} // namespace Spark
