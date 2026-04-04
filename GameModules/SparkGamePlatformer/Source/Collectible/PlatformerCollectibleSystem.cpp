/**
 * @file PlatformerCollectibleSystem.cpp
 * @brief Collectible placement, collection detection, and tracking
 */

#include "PlatformerCollectibleSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <cmath>

namespace Platformer
{

    bool PlatformerCollectibleSystem::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;

        BuildDemoCollectibles();

        m_initialized = true;

        auto& console = Spark::SimpleConsole::GetInstance();
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Platformer collectible system initialized with %zu collectibles",
                       m_collectibles.size());
        console.LogInfo("[Platformer Collectible] System initialized with " + std::to_string(m_collectibles.size()) +
                        " collectibles");
        return true;
    }

    void PlatformerCollectibleSystem::BuildDemoCollectibles()
    {
        // Level 0 (Green Hills) collectibles
        // Coin trail along the main path
        SpawnCollectibleLine(5.0f, 2.0f, 0.0f, 8, 2.0f, CollectibleType::Coin);

        // Gems on higher platforms
        CollectibleInstance gem{};
        gem.id = m_nextId++;
        gem.type = CollectibleType::Gem;
        gem.posX = 55.0f;
        gem.posY = 14.0f;
        gem.posZ = 0.0f;
        gem.value = 5;
        m_collectibles.push_back(gem);

        gem.id = m_nextId++;
        gem.posX = 72.0f;
        gem.posY = 14.0f;
        m_collectibles.push_back(gem);

        // Stars (3 per level, progressively harder to reach)
        CollectibleInstance star{};
        star.type = CollectibleType::Star;
        star.value = 1;

        star.id = m_nextId++;
        star.posX = 30.0f;
        star.posY = 8.0f;
        star.posZ = 0.0f;
        m_collectibles.push_back(star);

        star.id = m_nextId++;
        star.posX = 60.0f;
        star.posY = 16.0f;
        star.posZ = 0.0f;
        m_collectibles.push_back(star);

        star.id = m_nextId++;
        star.posX = 90.0f;
        star.posY = 20.0f;
        star.posZ = 0.0f;
        m_collectibles.push_back(star);

        // Ability orb: unlocks double jump
        CollectibleInstance abilityOrb{};
        abilityOrb.id = m_nextId++;
        abilityOrb.type = CollectibleType::AbilityOrb;
        abilityOrb.posX = 20.0f;
        abilityOrb.posY = 6.0f;
        abilityOrb.posZ = 0.0f;
        abilityOrb.abilityType = PowerUpType::DoubleJump;
        abilityOrb.value = 0;
        m_collectibles.push_back(abilityOrb);

        // Key for a locked gate
        CollectibleInstance key{};
        key.id = m_nextId++;
        key.type = CollectibleType::Key;
        key.posX = 48.0f;
        key.posY = 5.0f;
        key.posZ = 0.0f;
        key.value = 1;
        m_collectibles.push_back(key);

        // Extra life in the secret area
        CollectibleInstance extraLife{};
        extraLife.id = m_nextId++;
        extraLife.type = CollectibleType::ExtraLife;
        extraLife.posX = 85.0f;
        extraLife.posY = 25.0f;
        extraLife.posZ = 5.0f; // Off the main path
        extraLife.value = 1;
        m_collectibles.push_back(extraLife);
    }

    void PlatformerCollectibleSystem::SpawnCollectibleLine(float startX, float y, float z, int count, float spacing,
                                                           CollectibleType type)
    {
        for (int i = 0; i < count; ++i)
        {
            CollectibleInstance item{};
            item.id = m_nextId++;
            item.type = type;
            item.posX = startX + static_cast<float>(i) * spacing;
            item.posY = y;
            item.posZ = z;
            item.value = (type == CollectibleType::Coin) ? 1 : 5;
            m_collectibles.push_back(item);
        }
    }

    void PlatformerCollectibleSystem::CheckCollection(float playerX, float playerY, float playerZ, bool magnetActive)
    {
        float collectionMultiplier = magnetActive ? m_magnetRadius : 1.0f;

        for (auto& item : m_collectibles)
        {
            if (item.collected)
                continue;

            float dx = item.posX - playerX;
            float dy = item.posY - playerY;
            float dz = item.posZ - playerZ;
            float distSq = dx * dx + dy * dy + dz * dz;
            float radius = item.collectionRadius * collectionMultiplier;

            if (distSq <= radius * radius)
            {
                item.collected = true;

                switch (item.type)
                {
                case CollectibleType::Coin:
                    m_coinsCollected += item.value;
                    SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Platformer coin collected (total: %d)",
                                    m_coinsCollected);
                    break;
                case CollectibleType::Gem:
                    m_gemsCollected += item.value;
                    break;
                case CollectibleType::Star:
                    m_starsCollected += item.value;
                    break;
                case CollectibleType::Key:
                    m_keysCollected += item.value;
                    break;
                case CollectibleType::HealthPickup:
                case CollectibleType::AbilityOrb:
                case CollectibleType::ExtraLife:
                    // These are handled by the player controller via callbacks
                    break;
                default:
                    break;
                }
            }
        }
    }

    void PlatformerCollectibleSystem::ResetLevel(uint32_t levelIndex)
    {
        (void)levelIndex;

        // In a full implementation, this would reset only collectibles belonging
        // to the specified level. For the demo, reset all.
        for (auto& item : m_collectibles)
            item.collected = false;
    }

    LevelCollectionStats PlatformerCollectibleSystem::GetCurrentLevelStats() const
    {
        LevelCollectionStats stats{};
        for (const auto& item : m_collectibles)
        {
            switch (item.type)
            {
            case CollectibleType::Coin:
                stats.totalCoins++;
                if (item.collected)
                    stats.collectedCoins++;
                break;
            case CollectibleType::Gem:
                stats.totalGems++;
                if (item.collected)
                    stats.collectedGems++;
                break;
            case CollectibleType::Star:
                stats.totalStars++;
                if (item.collected)
                    stats.collectedStars++;
                break;
            case CollectibleType::Key:
                stats.totalKeys++;
                if (item.collected)
                    stats.collectedKeys++;
                break;
            default:
                break;
            }
        }
        return stats;
    }

    void PlatformerCollectibleSystem::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        // Update bobbing/spinning animation timer
        m_animationTimer += deltaTime;
    }

    void PlatformerCollectibleSystem::Render()
    {
        if (!m_initialized)
            return;

        // In a full implementation, this would render each uncollected item:
        // - Coins: gold disk with spin animation
        // - Gems: colored crystal with glow
        // - Stars: golden star with pulse
        // - Keys: key model with bob animation
        // - Ability orbs: glowing sphere with particle trail
        // All items use sin(m_animationTimer * m_bobFrequency) * m_bobAmplitude
        // for vertical bobbing and m_spinSpeed for Y-axis rotation.
    }

    void PlatformerCollectibleSystem::Shutdown()
    {
        m_collectibles.clear();
        m_initialized = false;
    }

    std::string PlatformerCollectibleSystem::GetCollectionString() const
    {
        auto stats = GetCurrentLevelStats();
        std::string result = "=== Platformer Collectibles ===\n";
        result += "Coins: " + std::to_string(stats.collectedCoins) + "/" + std::to_string(stats.totalCoins) + "\n";
        result += "Gems: " + std::to_string(stats.collectedGems) + "/" + std::to_string(stats.totalGems) + "\n";
        result += "Stars: " + std::to_string(stats.collectedStars) + "/" + std::to_string(stats.totalStars) + "\n";
        result += "Keys: " + std::to_string(stats.collectedKeys) + "/" + std::to_string(stats.totalKeys) + "\n";
        result += "Total coins (all levels): " + std::to_string(m_coinsCollected) + "\n";
        result += "Total stars (all levels): " + std::to_string(m_starsCollected) + "\n";
        return result;
    }

    void PlatformerCollectibleSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Platformer Collectibles"))
            return;

        auto stats = GetCurrentLevelStats();
        ImGui::Text("Coins: %d / %d", stats.collectedCoins, stats.totalCoins);
        ImGui::Text("Gems: %d / %d", stats.collectedGems, stats.totalGems);
        ImGui::Text("Stars: %d / %d", stats.collectedStars, stats.totalStars);
        ImGui::Text("Keys: %d / %d", stats.collectedKeys, stats.totalKeys);
        ImGui::Separator();
        ImGui::Text("Global Coins: %d", m_coinsCollected);
        ImGui::Text("Global Stars: %d", m_starsCollected);
#endif
    }

} // namespace Platformer
