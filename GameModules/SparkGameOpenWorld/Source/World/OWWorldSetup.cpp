/**
 * @file OWWorldSetup.cpp
 * @brief Open world biome regions, road network, and streaming registration
 */

#include "OWWorldSetup.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#include "Engine/Streaming/SceneManifest.h"
#include "Engine/Streaming/SeamlessAreaManager.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <cmath>
#include <sstream>

namespace OpenWorld
{

    bool OWWorldSetup::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;

        DefineBiomeRegions();
        DefineRoadNetwork();
        RegisterAreasWithStreaming();
        ConfigureOriginRebasing();

        m_initialized = true;
        return true;
    }

    void OWWorldSetup::DefineBiomeRegions()
    {
        m_regions.clear();

        // Region 1: Emerald Meadows — central grasslands, starting area
        {
            BiomeRegion r{};
            r.regionId = 1;
            r.name = "Emerald Meadows";
            r.description = "Rolling grasslands dotted with wildflowers and gentle streams";
            r.biome = Biome::Grasslands;
            r.boundsMinX = -2000.0f;
            r.boundsMinY = -10.0f;
            r.boundsMinZ = -2000.0f;
            r.boundsMaxX = 2000.0f;
            r.boundsMaxY = 150.0f;
            r.boundsMaxZ = 2000.0f;
            r.baseTemperature = 20.0f;
            r.elevationMin = 0.0f;
            r.elevationMax = 80.0f;
            r.dangerLevel = 1;
            r.abundantResources = {ResourceType::Herbs, ResourceType::Fiber, ResourceType::Water};
            r.nativeWildlife = {AnimalType::Deer, AnimalType::Rabbit, AnimalType::Horse, AnimalType::Fox};
            r.connectedRegions = {2, 3, 5, 6};
            m_regions.push_back(r);
        }

        // Region 2: Ironwood Forest — dense woodland, moderate danger
        {
            BiomeRegion r{};
            r.regionId = 2;
            r.name = "Ironwood Forest";
            r.description = "An ancient forest with towering ironwood trees and dappled sunlight";
            r.biome = Biome::Forest;
            r.boundsMinX = 2000.0f;
            r.boundsMinY = -20.0f;
            r.boundsMinZ = -3000.0f;
            r.boundsMaxX = 6000.0f;
            r.boundsMaxY = 300.0f;
            r.boundsMaxZ = 3000.0f;
            r.baseTemperature = 15.0f;
            r.elevationMin = 10.0f;
            r.elevationMax = 200.0f;
            r.dangerLevel = 3;
            r.abundantResources = {ResourceType::Wood, ResourceType::Herbs, ResourceType::Fiber};
            r.nativeWildlife = {AnimalType::Wolf, AnimalType::Bear, AnimalType::Deer, AnimalType::Boar,
                                AnimalType::Fox};
            r.connectedRegions = {1, 3, 4};
            m_regions.push_back(r);
        }

        // Region 3: Stormcrest Mountains — high altitude, harsh
        {
            BiomeRegion r{};
            r.regionId = 3;
            r.name = "Stormcrest Mountains";
            r.description = "Jagged peaks shrouded in mist, where only the hardiest survive";
            r.biome = Biome::Mountains;
            r.boundsMinX = -1000.0f;
            r.boundsMinY = 100.0f;
            r.boundsMinZ = 2000.0f;
            r.boundsMaxX = 4000.0f;
            r.boundsMaxY = 800.0f;
            r.boundsMaxZ = 7000.0f;
            r.baseTemperature = 2.0f;
            r.elevationMin = 200.0f;
            r.elevationMax = 800.0f;
            r.dangerLevel = 6;
            r.abundantResources = {ResourceType::Stone, ResourceType::Iron, ResourceType::Crystal};
            r.nativeWildlife = {AnimalType::Eagle, AnimalType::MountainLion, AnimalType::Elk};
            r.connectedRegions = {1, 2, 8};
            m_regions.push_back(r);
        }

        // Region 4: Ashwind Desert — arid expanse, extreme heat
        {
            BiomeRegion r{};
            r.regionId = 4;
            r.name = "Ashwind Desert";
            r.description = "A scorching desert of red sand dunes and ancient ruins half-buried in ash";
            r.biome = Biome::Desert;
            r.boundsMinX = 4000.0f;
            r.boundsMinY = -50.0f;
            r.boundsMinZ = -5000.0f;
            r.boundsMaxX = 10000.0f;
            r.boundsMaxY = 200.0f;
            r.boundsMaxZ = 1000.0f;
            r.baseTemperature = 42.0f;
            r.elevationMin = -20.0f;
            r.elevationMax = 150.0f;
            r.dangerLevel = 5;
            r.abundantResources = {ResourceType::Stone, ResourceType::Gold, ResourceType::Clay};
            r.nativeWildlife = {AnimalType::Snake, AnimalType::Eagle};
            r.connectedRegions = {2, 7};
            m_regions.push_back(r);
        }

        // Region 5: Frosthollow Tundra — frozen north, extreme cold
        {
            BiomeRegion r{};
            r.regionId = 5;
            r.name = "Frosthollow Tundra";
            r.description = "An endless frozen plain where blizzards rage and mammoth bones lie exposed";
            r.biome = Biome::Tundra;
            r.boundsMinX = -6000.0f;
            r.boundsMinY = -10.0f;
            r.boundsMinZ = 2000.0f;
            r.boundsMaxX = -1000.0f;
            r.boundsMaxY = 200.0f;
            r.boundsMaxZ = 8000.0f;
            r.baseTemperature = -15.0f;
            r.elevationMin = 0.0f;
            r.elevationMax = 120.0f;
            r.dangerLevel = 7;
            r.abundantResources = {ResourceType::Hide, ResourceType::Stone, ResourceType::Water};
            r.nativeWildlife = {AnimalType::Bison, AnimalType::Wolf, AnimalType::Elk, AnimalType::Bear};
            r.connectedRegions = {1, 8};
            m_regions.push_back(r);
        }

        // Region 6: Mistveil Swamp — wetlands, poison, disease
        {
            BiomeRegion r{};
            r.regionId = 6;
            r.name = "Mistveil Swamp";
            r.description = "Fetid marshlands where visibility is poor and the ground cannot be trusted";
            r.biome = Biome::Swamp;
            r.boundsMinX = -5000.0f;
            r.boundsMinY = -30.0f;
            r.boundsMinZ = -4000.0f;
            r.boundsMaxX = -2000.0f;
            r.boundsMaxY = 50.0f;
            r.boundsMaxZ = 2000.0f;
            r.baseTemperature = 25.0f;
            r.elevationMin = -20.0f;
            r.elevationMax = 30.0f;
            r.dangerLevel = 4;
            r.abundantResources = {ResourceType::Herbs, ResourceType::Clay, ResourceType::Fiber};
            r.nativeWildlife = {AnimalType::Snake, AnimalType::Boar, AnimalType::Fox};
            r.connectedRegions = {1, 7};
            m_regions.push_back(r);
        }

        // Region 7: Sunbreak Coast — temperate coastline, trade hub
        {
            BiomeRegion r{};
            r.regionId = 7;
            r.name = "Sunbreak Coast";
            r.description = "A warm coastline with white cliffs, sandy beaches, and a bustling port";
            r.biome = Biome::Coast;
            r.boundsMinX = -4000.0f;
            r.boundsMinY = -5.0f;
            r.boundsMinZ = -8000.0f;
            r.boundsMaxX = 4000.0f;
            r.boundsMaxY = 80.0f;
            r.boundsMaxZ = -4000.0f;
            r.baseTemperature = 24.0f;
            r.elevationMin = 0.0f;
            r.elevationMax = 60.0f;
            r.dangerLevel = 2;
            r.abundantResources = {ResourceType::Water, ResourceType::Fiber, ResourceType::Gold};
            r.nativeWildlife = {AnimalType::Eagle, AnimalType::Deer, AnimalType::Rabbit};
            r.connectedRegions = {4, 6};
            m_regions.push_back(r);
        }

        // Region 8: Cinderforge Caldera — volcanic, endgame
        {
            BiomeRegion r{};
            r.regionId = 8;
            r.name = "Cinderforge Caldera";
            r.description = "A volcanic hellscape of lava flows, obsidian spires, and toxic vents";
            r.biome = Biome::Volcanic;
            r.boundsMinX = -2000.0f;
            r.boundsMinY = 0.0f;
            r.boundsMinZ = 7000.0f;
            r.boundsMaxX = 3000.0f;
            r.boundsMaxY = 600.0f;
            r.boundsMaxZ = 12000.0f;
            r.baseTemperature = 55.0f;
            r.elevationMin = 50.0f;
            r.elevationMax = 600.0f;
            r.dangerLevel = 10;
            r.abundantResources = {ResourceType::Iron, ResourceType::Crystal, ResourceType::Stone};
            r.nativeWildlife = {}; // Too hostile for wildlife
            r.connectedRegions = {3, 5};
            m_regions.push_back(r);
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "Open world defined %zu biome regions", m_regions.size());
        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Defined " + std::to_string(m_regions.size()) +
                                                    " biome regions");
    }

    void OWWorldSetup::DefineRoadNetwork()
    {
        m_roads.clear();

        auto addRoad = [&](uint32_t id, const std::string& name, uint32_t from, uint32_t to, float safety, bool main)
        { m_roads.push_back({id, name, from, to, safety, main}); };

        addRoad(1, "Meadow Trail", 1, 2, 0.8f, true);
        addRoad(2, "Mountain Pass", 1, 3, 0.5f, true);
        addRoad(3, "Frostbound Road", 1, 5, 0.4f, false);
        addRoad(4, "Marshway", 1, 6, 0.3f, false);
        addRoad(5, "Timber Road", 2, 3, 0.6f, true);
        addRoad(6, "Scorched Path", 2, 4, 0.3f, false);
        addRoad(7, "Coastal Highway", 4, 7, 0.7f, true);
        addRoad(8, "Bogwalk", 6, 7, 0.4f, false);
        addRoad(9, "Summit Trail", 3, 8, 0.2f, false);
        addRoad(10, "Frozen Ridge", 5, 8, 0.1f, false);

        SPARK_LOG_INFO(Spark::LogCategory::Game, "Open world defined %zu roads", m_roads.size());
    }

    void OWWorldSetup::RegisterAreasWithStreaming()
    {
        // A module DLL statically links SparkEngineLib, so calling GetInstance()
        // here creates a manager owned by this DLL rather than using the host's
        // lifecycle-managed service. Resolve the host instance through the ABI;
        // retain the fallback only for standalone/unit-test contexts.
        auto* streamingMgr = m_context ? m_context->GetAreaStreaming() : nullptr;
        if (!streamingMgr)
            streamingMgr = &Spark::Streaming::SeamlessAreaManager::GetInstance();

        for (const auto& region : m_regions)
        {
            Spark::Streaming::AreaDefinition def{};
            def.areaId = region.regionId;
            def.name = region.name;
            def.boundsMin = {region.boundsMinX, region.boundsMinY, region.boundsMinZ};
            def.boundsMax = {region.boundsMaxX, region.boundsMaxY, region.boundsMaxZ};
            def.scenePath = "Assets/Scenes/OpenWorld/" + region.name + ".scene";
            def.priority = (region.dangerLevel <= 2) ? 2 : 1;

            // Build a scene manifest for this biome's assets
            Spark::Streaming::SceneManifest manifest;
            manifest.name = region.name;
            std::string basePath = "Assets/OpenWorld/" + region.name + "/";
            manifest.meshPaths.push_back(basePath + "terrain.mesh");
            manifest.meshPaths.push_back(basePath + "props.mesh");
            manifest.texturePaths.push_back(basePath + "terrain_albedo.dds");
            manifest.texturePaths.push_back(basePath + "terrain_normal.dds");
            manifest.audioPaths.push_back(basePath + "ambience.wav");

            streamingMgr->RegisterArea(def, std::move(manifest));
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "Open world areas registered with SeamlessAreaManager");
        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Registered areas with SeamlessAreaManager");
    }

    void OWWorldSetup::ConfigureOriginRebasing()
    {
        // Open world spans ~20km — rebasing is critical for precision
        m_originSystem.SetRebasingThreshold(4000.0f);
        m_originSystem.SetEnabled(true);

        SPARK_LOG_INFO(Spark::LogCategory::Game, "Open world origin rebasing enabled (threshold: 4000m)");
        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Origin rebasing enabled (threshold: 4000m)");
    }

    void OWWorldSetup::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        m_worldTime += deltaTime;

        // The host service is pumped once by the shared gameplay lifecycle.
        // Only standalone/test use of the per-image fallback owns its tick.
        if (!m_context || !m_context->GetAreaStreaming())
            Spark::Streaming::SeamlessAreaManager::GetInstance().Update(deltaTime);
    }

    void OWWorldSetup::Shutdown()
    {
        if (!m_initialized)
            return;

        auto* streamingMgr = m_context ? m_context->GetAreaStreaming() : nullptr;
        if (!streamingMgr)
            streamingMgr = &Spark::Streaming::SeamlessAreaManager::GetInstance();
        for (const auto& region : m_regions)
            streamingMgr->UnregisterArea(region.regionId);

        m_regions.clear();
        m_roads.clear();
        m_context = nullptr;
        m_initialized = false;
    }

    const BiomeRegion* OWWorldSetup::GetRegion(uint32_t regionId) const
    {
        for (const auto& r : m_regions)
        {
            if (r.regionId == regionId)
                return &r;
        }
        return nullptr;
    }

    const BiomeRegion* OWWorldSetup::GetRegionAtPosition(float x, float z) const
    {
        for (const auto& r : m_regions)
        {
            if (x >= r.boundsMinX && x <= r.boundsMaxX && z >= r.boundsMinZ && z <= r.boundsMaxZ)
                return &r;
        }
        return nullptr;
    }

    const BiomeRegion* OWWorldSetup::GetRegionAtPosition(float x, float y, float z) const
    {
        for (const auto& r : m_regions)
        {
            if (x >= r.boundsMinX && x <= r.boundsMaxX && y >= r.boundsMinY && y <= r.boundsMaxY && z >= r.boundsMinZ &&
                z <= r.boundsMaxZ)
                return &r;
        }
        return nullptr;
    }

    std::string OWWorldSetup::GetRegionListString() const
    {
        std::ostringstream ss;
        ss << "=== Open World Regions ===\n";
        for (const auto& r : m_regions)
        {
            ss << "[" << r.regionId << "] " << r.name << " (" << static_cast<int>(r.biome) << ")";
            ss << " Danger:" << r.dangerLevel << " Temp:" << r.baseTemperature << "C\n";
            ss << "    " << r.description << "\n";
            ss << "    Wildlife: " << r.nativeWildlife.size();
            ss << " | Resources: " << r.abundantResources.size();
            ss << " | Connections: ";
            for (size_t i = 0; i < r.connectedRegions.size(); ++i)
            {
                if (i > 0)
                    ss << ", ";
                const auto* connected = GetRegion(r.connectedRegions[i]);
                ss << (connected ? connected->name : "?");
            }
            ss << "\n";
        }
        return ss.str();
    }

    std::string OWWorldSetup::GetWorldStatusString() const
    {
        std::ostringstream ss;
        ss << "World Time: " << static_cast<int>(m_worldTime) << "s";
        ss << " | Regions: " << m_regions.size();
        ss << " | Roads: " << m_roads.size();
        return ss.str();
    }

    void OWWorldSetup::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Open World Map"))
            return;

        ImGui::Text("World Time: %.1f s", m_worldTime);
        ImGui::Text("Regions: %zu | Roads: %zu", m_regions.size(), m_roads.size());
        ImGui::Separator();

        for (const auto& r : m_regions)
        {
            ImGui::PushID(static_cast<int>(r.regionId));
            if (ImGui::TreeNode(r.name.c_str()))
            {
                ImGui::Text("Biome: %d | Danger: %d | Temp: %.0fC", static_cast<int>(r.biome), r.dangerLevel,
                            r.baseTemperature);
                ImGui::TextWrapped("%s", r.description.c_str());
                ImGui::Text("Bounds: (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)", r.boundsMinX, r.boundsMinY, r.boundsMinZ,
                            r.boundsMaxX, r.boundsMaxY, r.boundsMaxZ);
                ImGui::Text("Wildlife: %zu species | Resources: %zu types", r.nativeWildlife.size(),
                            r.abundantResources.size());
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
#endif
    }

} // namespace OpenWorld
