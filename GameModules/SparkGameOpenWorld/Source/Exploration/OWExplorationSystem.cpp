/**
 * @file OWExplorationSystem.cpp
 * @brief POI discovery, map fog, and exploration progress tracking
 */

#include "OWExplorationSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <cmath>
#include <sstream>

namespace OpenWorld
{

    bool OWExplorationSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;

        DefinePointsOfInterest();

        // Initialize all POIs as undiscovered
        for (const auto& poi : m_pois)
        {
            m_progress[poi.poiId] = {poi.poiId, DiscoveryState::Undiscovered, false};
        }

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Exploration system initialized with %zu POIs", m_pois.size());
        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Exploration: " + std::to_string(m_pois.size()) +
                                                    " points of interest");
        return true;
    }

    void OWExplorationSystem::DefinePointsOfInterest()
    {
        m_pois.clear();
        uint32_t id = 1;

        // --- Emerald Meadows (Region 1) ---
        m_pois.push_back({id++, "Shepherd's Overlook", "A hilltop with a panoramic view of the meadows",
                          POIType::Viewpoint, 500.0f, 60.0f, -300.0f, 20.0f, 1, 100, false});
        m_pois.push_back({id++, "Old Stone Well", "An ancient well said to grant wishes", POIType::Shrine, -400.0f,
                          2.0f, 600.0f, 15.0f, 1, 50, true});
        m_pois.push_back({id++, "Windmill Ruins", "The remains of a once-great flour mill", POIType::Ruins, 800.0f,
                          10.0f, 100.0f, 25.0f, 1, 75, false});
        m_pois.push_back({id++, "Meadow Crossroads", "A major junction with a weathered signpost", POIType::FastTravel,
                          0.0f, 5.0f, 0.0f, 30.0f, 1, 25, false});

        // --- Ironwood Forest (Region 2) ---
        m_pois.push_back({id++, "Hollow Oak", "A massive hollowed-out oak tree, ancient and eerie", POIType::Landmark,
                          3000.0f, 15.0f, -500.0f, 20.0f, 2, 75, true});
        m_pois.push_back({id++, "Moonstone Cave", "A cave with glowing crystal formations", POIType::Cave, 4000.0f,
                          -10.0f, 1000.0f, 15.0f, 2, 150, true});
        m_pois.push_back({id++, "Ranger's Watchtower", "A tall watchtower offering forest views", POIType::Viewpoint,
                          3500.0f, 80.0f, 500.0f, 20.0f, 2, 100, false});
        m_pois.push_back({id++, "Wolf Den", "The lair of the forest wolf pack", POIType::Dungeon, 2500.0f, -5.0f,
                          -1500.0f, 25.0f, 2, 200, true});

        // --- Stormcrest Mountains (Region 3) ---
        m_pois.push_back({id++, "Eagle's Perch", "The highest reachable peak in the range", POIType::Viewpoint, 1000.0f,
                          700.0f, 5000.0f, 20.0f, 3, 200, false});
        m_pois.push_back({id++, "Dwarven Mine", "Abandoned mine shafts with rich ore veins", POIType::Dungeon, 500.0f,
                          250.0f, 3500.0f, 25.0f, 3, 250, true});
        m_pois.push_back({id++, "Frozen Shrine", "An ice-encrusted altar to a forgotten god", POIType::Shrine, -500.0f,
                          400.0f, 4500.0f, 15.0f, 3, 150, true});

        // --- Ashwind Desert (Region 4) ---
        m_pois.push_back({id++, "Sandstone Colossus", "A half-buried statue of immense proportions", POIType::Landmark,
                          7000.0f, 30.0f, -2000.0f, 30.0f, 4, 150, false});
        m_pois.push_back({id++, "Oasis of Respite", "A rare water source amid the dunes", POIType::FastTravel, 6000.0f,
                          5.0f, -1000.0f, 25.0f, 4, 100, false});
        m_pois.push_back({id++, "Buried Temple", "Ancient ruins emerging from shifting sands", POIType::Dungeon,
                          8000.0f, -20.0f, -3000.0f, 25.0f, 4, 300, true});
        m_pois.push_back({id++, "Scorpion Caves", "A network of tunnels beneath the desert", POIType::Cave, 5500.0f,
                          -30.0f, -2500.0f, 20.0f, 4, 175, true});

        // --- Frosthollow Tundra (Region 5) ---
        m_pois.push_back({id++, "Mammoth Graveyard", "A field of ancient bones jutting from the ice", POIType::Landmark,
                          -3500.0f, 10.0f, 5000.0f, 30.0f, 5, 150, true});
        m_pois.push_back({id++, "Ice Caverns", "Crystalline ice caves with aurora reflections", POIType::Cave, -4000.0f,
                          -15.0f, 4000.0f, 20.0f, 5, 200, true});
        m_pois.push_back({id++, "Frost Watchtower", "A crumbling tower on the tundra's edge", POIType::Viewpoint,
                          -2000.0f, 50.0f, 3000.0f, 20.0f, 5, 100, false});

        // --- Mistveil Swamp (Region 6) ---
        m_pois.push_back({id++, "Witch's Hut", "A stilted shack deep in the bog", POIType::Landmark, -3500.0f, 3.0f,
                          -1000.0f, 15.0f, 6, 100, true});
        m_pois.push_back({id++, "Sunken Ruins", "Crumbling walls half-swallowed by the marsh", POIType::Ruins, -4000.0f,
                          -5.0f, 500.0f, 25.0f, 6, 150, true});
        m_pois.push_back({id++, "Bog Shrine", "A moss-covered altar oozing with swamp water", POIType::Shrine, -3000.0f,
                          1.0f, -2000.0f, 15.0f, 6, 75, false});

        // --- Sunbreak Coast (Region 7) ---
        m_pois.push_back({id++, "Lighthouse Point", "A working lighthouse overlooking the sea", POIType::Viewpoint,
                          0.0f, 40.0f, -6000.0f, 20.0f, 7, 100, false});
        m_pois.push_back({id++, "Smuggler's Cove", "A hidden inlet used by coastal smugglers", POIType::Cave, -2000.0f,
                          2.0f, -5500.0f, 20.0f, 7, 175, true});
        m_pois.push_back({id++, "Port Market", "A bustling trading hub on the waterfront", POIType::FastTravel, 1000.0f,
                          3.0f, -5000.0f, 30.0f, 7, 50, false});
        m_pois.push_back({id++, "Shipwreck Beach", "The wreckage of an old galleon", POIType::TreasureCache, 2500.0f,
                          1.0f, -7000.0f, 25.0f, 7, 200, true});

        // --- Cinderforge Caldera (Region 8) ---
        m_pois.push_back({id++, "Obsidian Spire", "A towering natural obsidian formation", POIType::Landmark, 500.0f,
                          350.0f, 9000.0f, 30.0f, 8, 250, false});
        m_pois.push_back({id++, "Forge of the Ancients", "A volcanic forge of immense power", POIType::Dungeon, 0.0f,
                          200.0f, 10000.0f, 25.0f, 8, 500, true});
        m_pois.push_back({id++, "Lava Tunnels", "Cooled lava tubes leading deep underground", POIType::Cave, -500.0f,
                          150.0f, 8500.0f, 20.0f, 8, 300, true});
    }

    void OWExplorationSystem::Update(float deltaTime, float playerX, float playerY, float playerZ)
    {
        if (!m_initialized)
            return;

        (void)deltaTime;
        CheckDiscovery(playerX, playerY, playerZ);
    }

    void OWExplorationSystem::CheckDiscovery(float playerX, float playerY, float playerZ)
    {
        for (const auto& poi : m_pois)
        {
            auto& prog = m_progress[poi.poiId];
            if (prog.state >= DiscoveryState::Visited)
                continue;

            float dx = playerX - poi.x;
            float dy = playerY - poi.y;
            float dz = playerZ - poi.z;
            float distSq = dx * dx + dy * dy + dz * dz;
            float radiusSq = poi.discoveryRadius * poi.discoveryRadius;

            if (distSq <= radiusSq)
            {
                DiscoverPOI(poi.poiId);
            }
        }
    }

    void OWExplorationSystem::DiscoverPOI(uint32_t poiId)
    {
        auto it = m_progress.find(poiId);
        if (it == m_progress.end() || it->second.state >= DiscoveryState::Visited)
            return;

        it->second.state = DiscoveryState::Visited;

        // Find the POI for reward
        for (const auto& poi : m_pois)
        {
            if (poi.poiId == poiId)
            {
                m_totalXPEarned += poi.xpReward;
                Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Discovered: " + poi.name + " (+" +
                                                            std::to_string(poi.xpReward) + " XP)");
                break;
            }
        }
    }

    void OWExplorationSystem::RevealPOI(uint32_t poiId)
    {
        auto it = m_progress.find(poiId);
        if (it == m_progress.end() || it->second.state >= DiscoveryState::Revealed)
            return;

        it->second.state = DiscoveryState::Revealed;
    }

    void OWExplorationSystem::FindSecret(uint32_t poiId)
    {
        auto it = m_progress.find(poiId);
        if (it == m_progress.end())
            return;

        if (!it->second.secretFound)
        {
            it->second.secretFound = true;
            it->second.state = DiscoveryState::Completed;
            m_totalXPEarned += 50; // Bonus XP for secret
            Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Secret found at POI " + std::to_string(poiId) +
                                                        " (+50 XP)");
        }
    }

    size_t OWExplorationSystem::GetDiscoveredCount() const
    {
        size_t count = 0;
        for (const auto& [id, prog] : m_progress)
        {
            if (prog.state >= DiscoveryState::Visited)
                ++count;
        }
        return count;
    }

    float OWExplorationSystem::GetOverallCompletion() const
    {
        if (m_pois.empty())
            return 0.0f;

        size_t completed = 0;
        for (const auto& [id, prog] : m_progress)
        {
            if (prog.state == DiscoveryState::Completed)
                ++completed;
            else if (prog.state == DiscoveryState::Visited)
                ++completed; // Visited counts as partial completion for non-secret POIs
        }
        return static_cast<float>(completed) / static_cast<float>(m_pois.size()) * 100.0f;
    }

    RegionExploration OWExplorationSystem::GetRegionExploration(uint32_t regionId) const
    {
        RegionExploration result{};
        result.regionId = regionId;

        for (const auto& poi : m_pois)
        {
            if (poi.regionId != regionId)
                continue;

            result.totalPOIs++;
            auto it = m_progress.find(poi.poiId);
            if (it != m_progress.end())
            {
                if (it->second.state >= DiscoveryState::Visited)
                    result.discoveredPOIs++;
                if (it->second.state == DiscoveryState::Completed)
                    result.completedPOIs++;
            }
        }

        if (result.totalPOIs > 0)
            result.completionPercent =
                static_cast<float>(result.discoveredPOIs) / static_cast<float>(result.totalPOIs) * 100.0f;

        return result;
    }

    std::string OWExplorationSystem::GetExplorationString() const
    {
        std::ostringstream ss;
        ss << "=== Exploration Progress ===\n";
        ss << "Discovered: " << GetDiscoveredCount() << "/" << m_pois.size() << "\n";
        ss << "Completion: " << GetOverallCompletion() << "%\n";
        ss << "Total XP: " << m_totalXPEarned << "\n";
        return ss.str();
    }

    std::string OWExplorationSystem::GetPOIListString() const
    {
        std::ostringstream ss;
        ss << "=== Points of Interest ===\n";
        for (const auto& poi : m_pois)
        {
            auto it = m_progress.find(poi.poiId);
            std::string state = "???";
            if (it != m_progress.end())
            {
                switch (it->second.state)
                {
                case DiscoveryState::Undiscovered:
                    state = "Undiscovered";
                    break;
                case DiscoveryState::Revealed:
                    state = "Revealed";
                    break;
                case DiscoveryState::Visited:
                    state = "Visited";
                    break;
                case DiscoveryState::Completed:
                    state = "Completed";
                    break;
                default:
                    break;
                }
            }
            ss << "[" << poi.poiId << "] " << poi.name << " (" << state << ")\n";
        }
        return ss.str();
    }

    void OWExplorationSystem::Shutdown()
    {
        if (!m_initialized)
            return;

        m_pois.clear();
        m_progress.clear();
        m_initialized = false;
    }

    void OWExplorationSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Exploration"))
            return;

        ImGui::Text("POIs: %zu | Discovered: %zu | XP: %u", m_pois.size(), GetDiscoveredCount(), m_totalXPEarned);
        ImGui::Text("Overall Completion: %.1f%%", GetOverallCompletion());
        ImGui::Separator();

        for (const auto& poi : m_pois)
        {
            auto it = m_progress.find(poi.poiId);
            if (it == m_progress.end())
                continue;

            const char* stateStr = "???";
            switch (it->second.state)
            {
            case DiscoveryState::Undiscovered:
                stateStr = "Undiscovered";
                break;
            case DiscoveryState::Revealed:
                stateStr = "Revealed";
                break;
            case DiscoveryState::Visited:
                stateStr = "Visited";
                break;
            case DiscoveryState::Completed:
                stateStr = "Completed";
                break;
            default:
                break;
            }

            ImGui::Text("[%u] %s (%s)", poi.poiId, poi.name.c_str(), stateStr);
        }
#endif
    }

} // namespace OpenWorld
