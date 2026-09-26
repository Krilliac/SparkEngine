/**
 * @file PlatformerCollectibleSystem.cpp
 * @brief Collectible placement, collection detection, and tracking
 */

#include "PlatformerCollectibleSystem.h"
#include "Engine/ECS/Components.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>

namespace Platformer
{
    namespace
    {
        constexpr float CoinMeshHalfHeight = 0.25f;
    } // namespace

    bool PlatformerCollectibleSystem::Initialize(Spark::IEngineContext* context)
    {
        // The context is only stored; collectible placement and collection are self-contained, so a null
        // context (the level-flow tests) is valid.
        m_context = context;

        BuildDemoCollectibles();
        PlaceCoinMeshes();

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
        SpawnCollectibleLine(5.0f, 1.0f, 0.0f, 8, 2.0f, CollectibleType::Coin);

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
        abilityOrb.posY = 1.0f;
        abilityOrb.posZ = 0.0f;
        abilityOrb.abilityType = PowerUpType::DoubleJump;
        abilityOrb.value = 0;
        m_collectibles.push_back(abilityOrb);

        abilityOrb.id = m_nextId++;
        abilityOrb.posX = 40.0f;
        abilityOrb.abilityType = PowerUpType::Dash;
        m_collectibles.push_back(abilityOrb);

        abilityOrb.id = m_nextId++;
        abilityOrb.posX = 70.0f;
        abilityOrb.abilityType = PowerUpType::GroundPound;
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

    std::vector<CollectedPickup> PlatformerCollectibleSystem::CheckCollection(float playerX, float playerY,
                                                                              float playerZ, bool magnetActive)
    {
        std::vector<CollectedPickup> collected;
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
                collected.push_back({item.type, item.abilityType, item.value, item.powerUpDuration});

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

        return collected;
    }

    bool PlatformerCollectibleSystem::HasCollectible(uint32_t id) const
    {
        return std::ranges::any_of(m_collectibles, [id](const CollectibleInstance& item) { return item.id == id; });
    }

    CollectionProgress PlatformerCollectibleSystem::CaptureProgress() const
    {
        CollectionProgress progress;
        for (const auto& item : m_collectibles)
        {
            if (item.collected)
                progress.collectedIds.push_back(item.id);
        }
        std::ranges::sort(progress.collectedIds);
        progress.coins = m_coinsCollected;
        progress.gems = m_gemsCollected;
        progress.stars = m_starsCollected;
        progress.keys = m_keysCollected;
        return progress;
    }

    bool PlatformerCollectibleSystem::RestoreProgress(const CollectionProgress& progress)
    {
        if (progress.coins < 0 || progress.gems < 0 || progress.stars < 0 || progress.keys < 0)
            return false;
        if (!std::ranges::all_of(progress.collectedIds, [this](uint32_t id) { return HasCollectible(id); }))
            return false;

        for (auto& item : m_collectibles)
            item.collected = std::ranges::find(progress.collectedIds, item.id) != progress.collectedIds.end();
        m_coinsCollected = progress.coins;
        m_gemsCollected = progress.gems;
        m_starsCollected = progress.stars;
        m_keysCollected = progress.keys;
        return true;
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

    void PlatformerCollectibleSystem::PlaceCoinMeshes()
    {
        auto* world = m_context ? m_context->GetWorld() : nullptr;
        if (!world)
            return; // No engine world (the level-flow tests): collectibles work without meshes.

        // coin (tools/blender/author_platformer_kit.py) is a 0.5 m disc, pivot at its bottom edge, face towards +Z.
        // Collectible positions are item centres, so the mesh is lowered by half its height.
        m_coinEntities.assign(m_collectibles.size(), 0);
        for (size_t index = 0; index < m_collectibles.size(); ++index)
        {
            const CollectibleInstance& item = m_collectibles[index];
            if (item.type != CollectibleType::Coin)
                continue;

            EntityID entity = world->CreateEntity("Platformer_Coin");
            world->AddComponent<Transform>(
                entity,
                Transform{{item.posX, item.posY - CoinMeshHalfHeight, item.posZ}, {0.0f, 180.0f, 0.0f}, {1, 1, 1}});
            world->AddComponent<MeshRenderer>(entity).meshPath = "Assets/Models/Platformer/Kit/coin.obj";
            m_coinEntities[index] = static_cast<uint32_t>(entity);
        }
    }

    void PlatformerCollectibleSystem::RemoveCoinMeshes()
    {
        auto* world = m_context ? m_context->GetWorld() : nullptr;
        if (world)
        {
            for (uint32_t entityId : m_coinEntities)
            {
                const auto entity = static_cast<EntityID>(entityId);
                if (entityId != 0 && world->GetRegistry().valid(entity))
                    world->DestroyEntity(entity);
            }
        }
        m_coinEntities.clear();
    }

    void PlatformerCollectibleSystem::Render()
    {
        if (!m_initialized)
            return;

        // Coins are drawn by the engine's RenderSystem from the kit meshes PlaceCoinMeshes adds to the world; here
        // they spin, bob and disappear once collected.
        auto* world = m_context ? m_context->GetWorld() : nullptr;
        if (world && !m_coinEntities.empty())
        {
            const float bob = std::sin(m_animationTimer * m_bobFrequency) * m_bobAmplitude;
            const float spin = std::fmod(180.0f + m_animationTimer * m_spinSpeed, 360.0f);
            const size_t count = std::min(m_coinEntities.size(), m_collectibles.size());
            for (size_t index = 0; index < count; ++index)
            {
                const auto entity = static_cast<EntityID>(m_coinEntities[index]);
                if (m_coinEntities[index] == 0 || !world->GetRegistry().valid(entity))
                    continue;
                auto* transform = world->GetComponent<Transform>(entity);
                auto* renderer = world->GetComponent<MeshRenderer>(entity);
                if (!transform || !renderer)
                    continue;
                const CollectibleInstance& item = m_collectibles[index];
                transform->position.y = item.posY - CoinMeshHalfHeight + bob;
                transform->rotation.y = spin;
                renderer->visible = !item.collected;
                renderer->worldMatrixDirty = true;
            }
        }

        // In a full implementation, this would also render each uncollected item:
        // - Gems: colored crystal with glow
        // - Stars: golden star with pulse
        // - Keys: key model with bob animation
        // - Ability orbs: glowing sphere with particle trail
        // All items use sin(m_animationTimer * m_bobFrequency) * m_bobAmplitude
        // for vertical bobbing and m_spinSpeed for Y-axis rotation.
    }

    void PlatformerCollectibleSystem::Shutdown()
    {
        RemoveCoinMeshes();
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
