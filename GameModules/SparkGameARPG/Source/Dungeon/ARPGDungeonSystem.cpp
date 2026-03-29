/**
 * @file ARPGDungeonSystem.cpp
 * @brief Procedural dungeon floor generation and tier difficulty scaling
 */

#include "ARPGDungeonSystem.h"
#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <random>
#include <sstream>
#include "Utils/LogMacros.h"

namespace ARPG
{

    static std::mt19937& GetDungeonRNG()
    {
        static thread_local std::mt19937 rng{std::random_device{}()};
        return rng;
    }

    bool ARPGDungeonSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;
        RegisterTierConfigs();

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[ARPG] Dungeon system initialized (%s tiers)",
                       std::to_string(m_tierConfigs.size()).c_str());
        return true;
    }

    void ARPGDungeonSystem::Shutdown()
    {
        m_floors.clear();
        m_currentFloorIndex = -1;
    }

    void ARPGDungeonSystem::Update([[maybe_unused]] float deltaTime)
    {
        // Future: dungeon event timers, trap triggers, ambient spawning
    }

    void ARPGDungeonSystem::RegisterTierConfigs()
    {
        m_tierConfigs.clear();

        DungeonTierConfig normal;
        normal.tier = ARPGDungeonTier::Normal;
        normal.name = "Normal";
        normal.hpMultiplier = 1.0f;
        normal.damageMultiplier = 1.0f;
        normal.xpMultiplier = 1.0f;
        normal.lootBonusChance = 0.0f;
        normal.minMonsterLevel = 1;
        normal.maxMonsterLevel = 30;
        m_tierConfigs.push_back(normal);

        DungeonTierConfig nightmare;
        nightmare.tier = ARPGDungeonTier::Nightmare;
        nightmare.name = "Nightmare";
        nightmare.hpMultiplier = 2.0f;
        nightmare.damageMultiplier = 1.8f;
        nightmare.xpMultiplier = 2.0f;
        nightmare.lootBonusChance = 10.0f;
        nightmare.minMonsterLevel = 25;
        nightmare.maxMonsterLevel = 50;
        m_tierConfigs.push_back(nightmare);

        DungeonTierConfig hell;
        hell.tier = ARPGDungeonTier::Hell;
        hell.name = "Hell";
        hell.hpMultiplier = 4.0f;
        hell.damageMultiplier = 3.5f;
        hell.xpMultiplier = 3.0f;
        hell.lootBonusChance = 25.0f;
        hell.minMonsterLevel = 45;
        hell.maxMonsterLevel = 65;
        m_tierConfigs.push_back(hell);

        DungeonTierConfig inferno;
        inferno.tier = ARPGDungeonTier::Inferno;
        inferno.name = "Inferno";
        inferno.hpMultiplier = 8.0f;
        inferno.damageMultiplier = 6.0f;
        inferno.xpMultiplier = 5.0f;
        inferno.lootBonusChance = 50.0f;
        inferno.minMonsterLevel = 60;
        inferno.maxMonsterLevel = 70;
        m_tierConfigs.push_back(inferno);
    }

    const DungeonTierConfig* ARPGDungeonSystem::GetTierConfig(ARPGDungeonTier tier) const
    {
        for (const auto& config : m_tierConfigs)
        {
            if (config.tier == tier)
                return &config;
        }
        return nullptr;
    }

    DungeonLevel ARPGDungeonSystem::GenerateFloor(ARPGDungeonTier tier, int floorNumber)
    {
        const auto* tierConfig = GetTierConfig(tier);

        DungeonLevel level;
        level.tier = tier;
        level.floorNumber = floorNumber;

        // Deterministic seed from tier + floor for reproducible layouts
        level.layoutSeed = static_cast<uint32_t>(tier) * 10000 + static_cast<uint32_t>(floorNumber);

        // Monster density increases with depth
        level.monsterDensity = 1.0f + static_cast<float>(floorNumber) * 0.1f;

        // Elite pack chance increases with floor depth
        float eliteChance = BASE_ELITE_CHANCE + ELITE_CHANCE_PER_FLOOR * static_cast<float>(floorNumber);
        std::uniform_real_distribution<float> dist(0.0f, 1.0f);
        level.hasElitePack = (dist(GetDungeonRNG()) < eliteChance);

        // Boss every BOSS_FLOOR_INTERVAL floors
        level.hasBoss = (floorNumber > 0 && floorNumber % BOSS_FLOOR_INTERVAL == 0);

        // Monster level scales with floor and tier
        if (tierConfig)
        {
            int levelRange = tierConfig->maxMonsterLevel - tierConfig->minMonsterLevel;
            float floorProgress = static_cast<float>(floorNumber) / 20.0f; // ~20 floors per tier
            floorProgress = std::min(floorProgress, 1.0f);
            level.monsterLevel =
                tierConfig->minMonsterLevel + static_cast<int>(static_cast<float>(levelRange) * floorProgress);
        }
        else
        {
            level.monsterLevel = floorNumber;
        }

        return level;
    }

    const DungeonLevel* ARPGDungeonSystem::GetCurrentFloor() const
    {
        if (m_currentFloorIndex < 0 || m_currentFloorIndex >= static_cast<int>(m_floors.size()))
            return nullptr;
        return &m_floors[static_cast<size_t>(m_currentFloorIndex)];
    }

    int ARPGDungeonSystem::GetCurrentFloorNumber() const
    {
        const auto* floor = GetCurrentFloor();
        return floor ? floor->floorNumber : 0;
    }

    void ARPGDungeonSystem::SetDungeonTier(ARPGDungeonTier tier)
    {
        m_currentTier = tier;
        m_floors.clear();
        m_currentFloorIndex = -1;

        const auto* config = GetTierConfig(tier);
        std::string tierName = config ? config->name : "Unknown";
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[ARPG] Dungeon tier set to: %s", tierName.c_str());
    }

    void ARPGDungeonSystem::DescendToNextFloor()
    {
        int nextFloor = (m_currentFloorIndex >= 0) ? GetCurrentFloorNumber() + 1 : 1;
        DungeonLevel level = GenerateFloor(m_currentTier, nextFloor);
        m_floors.push_back(level);
        m_currentFloorIndex = static_cast<int>(m_floors.size()) - 1;

        std::string msg = "[ARPG] Descended to floor " + std::to_string(nextFloor);
        msg += " (MonLv:" + std::to_string(level.monsterLevel);
        msg += " Density:" + std::to_string(static_cast<int>(level.monsterDensity * 100.0f)) + "%";
        if (level.hasElitePack)
            msg += " ELITE";
        if (level.hasBoss)
            msg += " BOSS";
        msg += ")";
        SPARK_LOG_INFO(Spark::LogCategory::Game, "%s", msg.c_str());
    }

    std::string ARPGDungeonSystem::GetDungeonStatusString() const
    {
        std::ostringstream ss;
        ss << "=== ARPG Dungeon System ===\n";

        ss << "Difficulty tiers:\n";
        for (const auto& config : m_tierConfigs)
        {
            ss << "  " << config.name << " - HP x" << config.hpMultiplier << " DMG x" << config.damageMultiplier
               << " XP x" << config.xpMultiplier << " Lv" << config.minMonsterLevel << "-" << config.maxMonsterLevel
               << "\n";
        }

        const auto* floor = GetCurrentFloor();
        if (floor)
        {
            ss << "\nCurrent: Floor " << floor->floorNumber << " | MonLv:" << floor->monsterLevel
               << " | Density:" << static_cast<int>(floor->monsterDensity * 100.0f) << "%";
            if (floor->hasElitePack)
                ss << " | ELITE PACK";
            if (floor->hasBoss)
                ss << " | BOSS";
            ss << "\n";
        }
        else
        {
            ss << "\nNot currently in a dungeon\n";
        }

        ss << "Total floors explored: " << m_floors.size() << "\n";
        return ss.str();
    }

    void ARPGDungeonSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("ARPG Dungeon System"))
        {
            ImGui::Text("Tiers: %zu | Floors explored: %zu", m_tierConfigs.size(), m_floors.size());

            const auto* floor = GetCurrentFloor();
            if (floor)
            {
                ImGui::Text("Current: Floor %d | MonLv %d | Density %.0f%%", floor->floorNumber, floor->monsterLevel,
                            floor->monsterDensity * 100.0f);
                if (floor->hasElitePack)
                    ImGui::TextColored({1, 0.5f, 0, 1}, "ELITE PACK present");
                if (floor->hasBoss)
                    ImGui::TextColored({1, 0, 0, 1}, "BOSS present");
            }
            else
            {
                ImGui::Text("Not in dungeon");
            }
            ImGui::TreePop();
        }
#endif
    }

} // namespace ARPG
