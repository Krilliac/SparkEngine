/**
 * @file LootSystem.cpp
 * @brief Enemy loot drops and timed power-up buffs
 */

#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include "LootSystem.h"
#include "Player.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <sstream>

using namespace DirectX;

namespace Spark
{

    static float DistSq(const XMFLOAT3& a, const XMFLOAT3& b)
    {
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        float dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    }

    LootSystem::LootSystem() = default;

    void LootSystem::Initialize()
    {
        m_worldDrops.clear();
        m_activeBuffs.clear();
        BuildLootTable();
        LOG_TO_CONSOLE_IMMEDIATE(L"Loot system initialized", L"SUCCESS");
    }

    void LootSystem::BuildLootTable()
    {
        // Normal enemy drops
        m_lootTable = {
            {WorldDrop::DropType::HealthOrb, 0.4f, 15.0f, 30.0f, {}, 0},
            {WorldDrop::DropType::AmmoPack, 0.5f, 10.0f, 30.0f, {}, 0},
            {WorldDrop::DropType::ArmorShard, 0.2f, 10.0f, 25.0f, {}, 0},
            {WorldDrop::DropType::PowerUp, 0.05f, 0.0f, 0.0f, PowerUpType::SpeedBoost, 0},
            {WorldDrop::DropType::PowerUp, 0.05f, 0.0f, 0.0f, PowerUpType::DamageBoost, 0},
        };

        // Boss enemy drops — guaranteed and better quality
        m_bossLootTable = {
            {WorldDrop::DropType::HealthOrb, 1.0f, 50.0f, 100.0f, {}, 0},
            {WorldDrop::DropType::ArmorShard, 0.8f, 30.0f, 50.0f, {}, 0},
            {WorldDrop::DropType::AmmoPack, 1.0f, 30.0f, 60.0f, {}, 0},
            {WorldDrop::DropType::PowerUp, 0.4f, 0.0f, 0.0f, PowerUpType::DamageBoost, 0},
            {WorldDrop::DropType::PowerUp, 0.3f, 0.0f, 0.0f, PowerUpType::ShieldRegen, 0},
            {WorldDrop::DropType::PowerUp, 0.2f, 0.0f, 0.0f, PowerUpType::DoubleXP, 0},
        };
    }

    void LootSystem::Update(float dt, Player* player)
    {
        if (!player)
            return;

        XMFLOAT3 playerPos = player->GetPosition();
        float pickupRadiusSq = m_pickupRadius * m_pickupRadius;

        // Update world drops: check lifetime and proximity
        for (auto& drop : m_worldDrops)
        {
            if (drop.collected)
                continue;

            drop.lifetime -= dt;
            drop.bobTimer += dt;

            if (drop.lifetime <= 0.0f)
            {
                drop.collected = true; // Mark for removal
                continue;
            }

            // Check pickup proximity
            if (DistSq(playerPos, drop.position) <= pickupRadiusSq)
            {
                CollectDrop(drop, player);
            }
        }

        // Remove collected/expired drops
        m_worldDrops.erase(
            std::remove_if(m_worldDrops.begin(), m_worldDrops.end(), [](const WorldDrop& d) { return d.collected; }),
            m_worldDrops.end());

        // Update active buffs
        for (auto& buff : m_activeBuffs)
        {
            buff.remaining -= dt;
            if (buff.remaining <= 0.0f)
            {
                if (m_callbacks.onPowerUpExpired)
                    m_callbacks.onPowerUpExpired(buff.type);
            }
        }

        // Remove expired buffs
        m_activeBuffs.erase(std::remove_if(m_activeBuffs.begin(), m_activeBuffs.end(),
                                           [](const ActiveBuff& b) { return b.remaining <= 0.0f; }),
                            m_activeBuffs.end());
    }

    void LootSystem::SpawnEnemyLoot(const XMFLOAT3& position, int enemyType, bool isBossWave)
    {
        static std::mt19937 rng(std::random_device{}());
        std::uniform_real_distribution<float> chance(0.0f, 1.0f);
        std::uniform_real_distribution<float> offset(-1.5f, 1.5f);

        const auto& table = isBossWave ? m_bossLootTable : m_lootTable;

        for (const auto& entry : table)
        {
            if (chance(rng) > entry.dropChance)
                continue;

            if (static_cast<int>(m_worldDrops.size()) >= MAX_WORLD_DROPS)
                break;

            WorldDrop drop;
            drop.position = {position.x + offset(rng), position.y + 0.5f, position.z + offset(rng)};
            drop.lifetime = m_dropLifetime;
            drop.type = entry.type;
            drop.powerUpType = entry.powerUpType;
            drop.itemDefId = entry.itemDefId;

            if (entry.minValue != entry.maxValue)
            {
                std::uniform_real_distribution<float> val(entry.minValue, entry.maxValue);
                drop.value = val(rng);
            }
            else
            {
                drop.value = entry.minValue;
            }

            m_worldDrops.push_back(drop);
        }
    }

    void LootSystem::SpawnPowerUp(PowerUpType type, const XMFLOAT3& position, float duration)
    {
        if (static_cast<int>(m_worldDrops.size()) >= MAX_WORLD_DROPS)
            return;

        WorldDrop drop;
        drop.position = position;
        drop.lifetime = m_dropLifetime;
        drop.type = WorldDrop::DropType::PowerUp;
        drop.powerUpType = type;
        drop.value = duration;
        m_worldDrops.push_back(drop);
    }

    void LootSystem::CollectDrop(WorldDrop& drop, Player* player)
    {
        drop.collected = true;

        switch (drop.type)
        {
        case WorldDrop::DropType::HealthOrb:
            player->Heal(drop.value);
            break;

        case WorldDrop::DropType::ArmorShard:
            player->AddArmor(drop.value);
            break;

        case WorldDrop::DropType::AmmoPack:
            player->Console_GiveAmmo(static_cast<int>(drop.value));
            break;

        case WorldDrop::DropType::PowerUp:
        {
            float duration = (drop.value > 0.0f) ? drop.value : 15.0f;
            ApplyBuff(drop.powerUpType, duration);
            break;
        }

        case WorldDrop::DropType::ItemDrop:
            // Item drops are handled by the inventory system via EventBus
            break;
        }

        if (m_callbacks.onDropCollected)
            m_callbacks.onDropCollected(drop);
    }

    void LootSystem::ApplyBuff(PowerUpType type, float duration)
    {
        // Refresh if already active, don't stack
        for (auto& buff : m_activeBuffs)
        {
            if (buff.type == type)
            {
                buff.remaining = std::max(buff.remaining, duration);
                return;
            }
        }

        ActiveBuff buff;
        buff.type = type;
        buff.duration = duration;
        buff.remaining = duration;

        switch (type)
        {
        case PowerUpType::SpeedBoost:
            buff.magnitude = 1.5f;
            break;
        case PowerUpType::DamageBoost:
            buff.magnitude = 1.5f;
            break;
        case PowerUpType::ShieldRegen:
            buff.magnitude = 5.0f;
            break;
        case PowerUpType::InfiniteAmmo:
            buff.magnitude = 1.0f;
            break;
        case PowerUpType::DoubleXP:
            buff.magnitude = 2.0f;
            break;
        case PowerUpType::Invisibility:
            buff.magnitude = 0.5f;
            break;
        default:
            buff.magnitude = 1.0f;
            break;
        }

        m_activeBuffs.push_back(buff);

        if (m_callbacks.onPowerUpCollected)
            m_callbacks.onPowerUpCollected(type, duration);

        std::wstring msg = L"Power-up: ";
        msg += std::wstring(GetPowerUpName(type), GetPowerUpName(type) + strlen(GetPowerUpName(type)));
        msg += L" (" + std::to_wstring(static_cast<int>(duration)) + L"s)";
        LOG_TO_CONSOLE_IMMEDIATE(msg, L"SUCCESS");
    }

    bool LootSystem::HasBuff(PowerUpType type) const
    {
        for (const auto& buff : m_activeBuffs)
        {
            if (buff.type == type)
                return true;
        }
        return false;
    }

    float LootSystem::GetBuffTimeRemaining(PowerUpType type) const
    {
        for (const auto& buff : m_activeBuffs)
        {
            if (buff.type == type)
                return buff.remaining;
        }
        return 0.0f;
    }

    float LootSystem::GetBuffMagnitude(PowerUpType type) const
    {
        for (const auto& buff : m_activeBuffs)
        {
            if (buff.type == type)
                return buff.magnitude;
        }
        return 1.0f;
    }

    const char* LootSystem::GetPowerUpName(PowerUpType type)
    {
        switch (type)
        {
        case PowerUpType::SpeedBoost:
            return "Speed Boost";
        case PowerUpType::DamageBoost:
            return "Damage Boost";
        case PowerUpType::ShieldRegen:
            return "Shield Regen";
        case PowerUpType::InfiniteAmmo:
            return "Infinite Ammo";
        case PowerUpType::DoubleXP:
            return "Double XP";
        case PowerUpType::Invisibility:
            return "Invisibility";
        default:
            return "Unknown";
        }
    }

    std::string LootSystem::Console_GetStatus() const
    {
        std::stringstream ss;
        ss << "=== Loot System ===\n";
        ss << "World Drops: " << m_worldDrops.size() << "/" << MAX_WORLD_DROPS << "\n";
        ss << "Active Buffs: " << m_activeBuffs.size() << "\n";
        for (const auto& buff : m_activeBuffs)
        {
            ss << "  " << GetPowerUpName(buff.type) << " x" << buff.magnitude << " (" << buff.remaining << "s)\n";
        }
        return ss.str();
    }

} // namespace Spark
