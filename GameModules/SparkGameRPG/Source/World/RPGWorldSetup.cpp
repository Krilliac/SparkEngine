/**
 * @file RPGWorldSetup.cpp
 * @brief RPG world areas, encounter tables, and streaming registration
 */

#include "RPGWorldSetup.h"
#include "Engine/Streaming/SeamlessAreaManager.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <sstream>
#include "Utils/LogMacros.h"

namespace RPG
{

    bool RPGWorldSetup::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;

        DefineWorldAreas();
        RegisterAreasWithStreaming();
        ConfigureOriginRebasing();

        m_initialized = true;
        return true;
    }

    void RPGWorldSetup::DefineWorldAreas()
    {
        m_areas.clear();

        // Area 1: Oakhollow Village — safe hub, merchants, quest givers
        RPGAreaInfo village{};
        village.areaId = 1;
        village.name = "Village";
        village.description = "Oakhollow, a peaceful hamlet nestled at the forest's edge";
        village.boundsMinX = -200.0f;
        village.boundsMinY = -10.0f;
        village.boundsMinZ = -200.0f;
        village.boundsMaxX = 200.0f;
        village.boundsMaxY = 100.0f;
        village.boundsMaxZ = 200.0f;
        village.suggestedLevel = 1;
        village.isSafeZone = true;
        village.connectedAreas = {2, 5}; // Forest, Swamp
        // No encounters in safe zone
        m_areas.push_back(village);

        // Area 2: Thornwood Forest — open-world exploration, wildlife and wolves
        RPGAreaInfo forest{};
        forest.areaId = 2;
        forest.name = "Forest";
        forest.description = "Thornwood Forest, dense and teeming with wildlife and lurking dangers";
        forest.boundsMinX = 200.0f;
        forest.boundsMinY = -50.0f;
        forest.boundsMinZ = -1000.0f;
        forest.boundsMaxX = 2000.0f;
        forest.boundsMaxY = 300.0f;
        forest.boundsMaxZ = 1000.0f;
        forest.suggestedLevel = 2;
        forest.isSafeZone = false;
        forest.connectedAreas = {1, 3, 4}; // Village, Dungeon, Castle
        forest.encounters = {
            {100, "Shadow Wolf", 1, 3, 10.0f}, {101, "Forest Spider", 2, 4, 7.0f},    {102, "Goblin Scout", 2, 5, 5.0f},
            {103, "Wild Boar", 1, 2, 8.0f},    {104, "Thornwood Treant", 4, 6, 2.0f},
        };
        m_areas.push_back(forest);

        // Area 3: Shadow Crypt — underground dungeon
        RPGAreaInfo dungeon{};
        dungeon.areaId = 3;
        dungeon.name = "Dungeon";
        dungeon.description = "The Shadow Crypt, an ancient burial site corrupted by dark magic";
        dungeon.boundsMinX = -150.0f;
        dungeon.boundsMinY = -200.0f;
        dungeon.boundsMinZ = -150.0f;
        dungeon.boundsMaxX = 150.0f;
        dungeon.boundsMaxY = 0.0f;
        dungeon.boundsMaxZ = 150.0f;
        dungeon.suggestedLevel = 5;
        dungeon.isSafeZone = false;
        dungeon.connectedAreas = {2}; // Forest
        dungeon.encounters = {
            {200, "Skeleton Warrior", 4, 6, 8.0f},    {201, "Phantom", 5, 7, 5.0f},
            {202, "Crypt Crawler", 4, 5, 7.0f},       {203, "Bone Golem", 6, 8, 3.0f},
            {204, "Necromancer Acolyte", 5, 7, 4.0f},
        };
        m_areas.push_back(dungeon);

        // Area 4: Thornwall Castle — endgame zone, undead siege
        RPGAreaInfo castle{};
        castle.areaId = 4;
        castle.name = "Castle";
        castle.description = "Thornwall Castle, once a bastion of the realm, now overrun by the undead";
        castle.boundsMinX = -300.0f;
        castle.boundsMinY = 0.0f;
        castle.boundsMinZ = 1000.0f;
        castle.boundsMaxX = 300.0f;
        castle.boundsMaxY = 200.0f;
        castle.boundsMaxZ = 1600.0f;
        castle.suggestedLevel = 8;
        castle.isSafeZone = false;
        castle.connectedAreas = {2}; // Forest
        castle.encounters = {
            {300, "Undead Soldier", 7, 10, 8.0f},
            {301, "Wraith Knight", 8, 11, 5.0f},
            {302, "Siege Abomination", 9, 12, 3.0f},
            {303, "Dark Archer", 7, 9, 6.0f},
        };
        m_areas.push_back(castle);

        // Area 5: Mirefen Swamp — poisonous wetlands
        RPGAreaInfo swamp{};
        swamp.areaId = 5;
        swamp.name = "Swamp";
        swamp.description = "Mirefen Swamp, a fetid marshland thick with fog and venomous creatures";
        swamp.boundsMinX = -500.0f;
        swamp.boundsMinY = -30.0f;
        swamp.boundsMinZ = -800.0f;
        swamp.boundsMaxX = -200.0f;
        swamp.boundsMaxY = 50.0f;
        swamp.boundsMaxZ = 200.0f;
        swamp.suggestedLevel = 4;
        swamp.isSafeZone = false;
        swamp.connectedAreas = {1}; // Village
        swamp.encounters = {
            {400, "Swamp Leech", 3, 5, 9.0f},
            {401, "Bog Troll", 5, 7, 4.0f},
            {402, "Venomous Serpent", 3, 6, 7.0f},
            {403, "Marsh Wisp", 4, 6, 5.0f},
        };
        m_areas.push_back(swamp);

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[RPG World] Defined %s world areas",
                       std::to_string(m_areas.size()).c_str());
    }

    void RPGWorldSetup::RegisterAreasWithStreaming()
    {
        auto& streamingMgr = Spark::Streaming::SeamlessAreaManager::GetInstance();

        for (const auto& area : m_areas)
        {
            Spark::Streaming::AreaDefinition def{};
            def.areaId = area.areaId;
            def.name = area.name;
            def.boundsMin = {area.boundsMinX, area.boundsMinY, area.boundsMinZ};
            def.boundsMax = {area.boundsMaxX, area.boundsMaxY, area.boundsMaxZ};
            def.scenePath = "Assets/Scenes/RPG/" + area.name + ".scene";
            def.priority = area.isSafeZone ? 2 : 1;

            streamingMgr.RegisterArea(def);
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[RPG World] Registered areas with SeamlessAreaManager");
    }

    void RPGWorldSetup::ConfigureOriginRebasing()
    {
        m_originSystem.SetRebasingThreshold(3000.0f);
        m_originSystem.SetEnabled(true);

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[RPG World] Origin rebasing enabled (threshold: 3000m)");
    }

    void RPGWorldSetup::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        m_worldTime += deltaTime;

        auto& streamingMgr = Spark::Streaming::SeamlessAreaManager::GetInstance();
        streamingMgr.Update(deltaTime);
    }

    void RPGWorldSetup::Shutdown()
    {
        if (!m_initialized)
            return;

        m_areas.clear();
        m_initialized = false;
    }

    const RPGAreaInfo* RPGWorldSetup::GetArea(uint32_t areaId) const
    {
        for (const auto& area : m_areas)
        {
            if (area.areaId == areaId)
                return &area;
        }
        return nullptr;
    }

    std::string RPGWorldSetup::GetAreaListString() const
    {
        std::ostringstream ss;
        ss << "=== RPG World Areas ===\n";
        for (const auto& area : m_areas)
        {
            ss << "[" << area.areaId << "] " << area.name << " (Lv" << area.suggestedLevel << ")";
            if (area.isSafeZone)
                ss << " [Safe]";
            ss << "\n    " << area.description << "\n";
            ss << "    Encounters: " << area.encounters.size();
            ss << " | Connections: ";
            for (size_t i = 0; i < area.connectedAreas.size(); ++i)
            {
                if (i > 0)
                    ss << ", ";
                const auto* connected = GetArea(area.connectedAreas[i]);
                ss << (connected ? connected->name : "?");
            }
            ss << "\n";
        }
        return ss.str();
    }

    void RPGWorldSetup::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("RPG World"))
            return;

        ImGui::Text("World Time: %.1f s", m_worldTime);
        ImGui::Text("Areas: %zu", m_areas.size());
        ImGui::Separator();

        for (const auto& area : m_areas)
        {
            ImGui::PushID(static_cast<int>(area.areaId));
            if (ImGui::TreeNode(area.name.c_str()))
            {
                ImGui::Text("ID: %u | Level: %d", area.areaId, area.suggestedLevel);
                ImGui::Text("Safe Zone: %s", area.isSafeZone ? "Yes" : "No");
                ImGui::TextWrapped("%s", area.description.c_str());
                ImGui::Text("Bounds: (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)", area.boundsMinX, area.boundsMinY,
                            area.boundsMinZ, area.boundsMaxX, area.boundsMaxY, area.boundsMaxZ);

                if (!area.encounters.empty() && ImGui::TreeNode("Encounters"))
                {
                    for (const auto& enc : area.encounters)
                    {
                        ImGui::Text("%s (Lv%d-%d, weight:%.1f)", enc.enemyName.c_str(), enc.minLevel, enc.maxLevel,
                                    enc.spawnWeight);
                    }
                    ImGui::TreePop();
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
#endif
    }

} // namespace RPG
