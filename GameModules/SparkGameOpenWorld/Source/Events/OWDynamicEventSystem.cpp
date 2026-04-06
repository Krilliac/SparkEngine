/**
 * @file OWDynamicEventSystem.cpp
 * @brief Dynamic world events and random encounters
 */

#include "OWDynamicEventSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <cmath>
#include <sstream>

namespace OpenWorld
{

    bool OWDynamicEventSystem::Initialize(Spark::IEngineContext* context)
    {
        m_context = context;

        DefineEventTemplates();

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Dynamic event system initialized: %zu event types",
                       m_templates.size());
        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Events: " + std::to_string(m_templates.size()) +
                                                    " event types registered");
        return true;
    }

    void OWDynamicEventSystem::DefineEventTemplates()
    {
        m_templates.clear();

        m_templates.push_back({1,
                               "Bandit Raid",
                               "A group of bandits attacks a nearby settlement",
                               WorldEventType::BanditRaid,
                               120.0f,
                               300.0f,
                               2,
                               150,
                               {Biome::Grasslands, Biome::Forest, Biome::Coast}});

        m_templates.push_back({2,
                               "Wildlife Stampede",
                               "A herd of animals stampedes through the area",
                               WorldEventType::WildlifeStampede,
                               60.0f,
                               600.0f,
                               1,
                               75,
                               {Biome::Grasslands, Biome::Tundra}});

        m_templates.push_back({3,
                               "Merchant Caravan",
                               "A traveling merchant needs escort to the next settlement",
                               WorldEventType::MerchantCaravan,
                               180.0f,
                               400.0f,
                               1,
                               200,
                               {Biome::Grasslands, Biome::Forest, Biome::Desert, Biome::Coast}});

        m_templates.push_back({4,
                               "Severe Storm",
                               "A dangerous storm system rolls in, affecting visibility and travel",
                               WorldEventType::WeatherStorm,
                               90.0f,
                               500.0f,
                               3,
                               100,
                               {Biome::Mountains, Biome::Tundra, Biome::Coast, Biome::Swamp}});

        m_templates.push_back({5,
                               "Volcanic Eruption",
                               "The caldera rumbles and spews lava flows across the region",
                               WorldEventType::VolcanicEruption,
                               300.0f,
                               1800.0f,
                               8,
                               500,
                               {Biome::Volcanic}});

        m_templates.push_back({6,
                               "Dragon Sighting",
                               "A legendary dragon has been spotted soaring over the region",
                               WorldEventType::DragonSighting,
                               45.0f,
                               3600.0f,
                               5,
                               1000,
                               {Biome::Mountains, Biome::Volcanic}});

        m_templates.push_back({7,
                               "Harvest Festival",
                               "A settlement celebrates the harvest with games and trades",
                               WorldEventType::HarvestFestival,
                               240.0f,
                               1200.0f,
                               1,
                               50,
                               {Biome::Grasslands, Biome::Forest}});

        m_templates.push_back({8,
                               "Ancient Awakening",
                               "An ancient guardian stirs from slumber deep underground",
                               WorldEventType::AncientAwakening,
                               180.0f,
                               2400.0f,
                               7,
                               750,
                               {Biome::Desert, Biome::Mountains, Biome::Volcanic}});
    }

    void OWDynamicEventSystem::Update(float deltaTime, float playerX, float playerZ, uint32_t playerRegionId)
    {
        if (!m_initialized)
            return;

        // Tick cooldowns
        for (auto& [id, remaining] : m_cooldowns)
        {
            remaining = std::max(0.0f, remaining - deltaTime);
        }

        // Check for new random events periodically
        m_eventCheckTimer += deltaTime;
        if (m_eventCheckTimer >= 30.0f) // Check every 30 seconds
        {
            m_eventCheckTimer = 0.0f;
            CheckForNewEvents(playerX, playerZ, playerRegionId);
        }

        UpdateActiveEvents(deltaTime, playerX, playerZ);
    }

    // Intentional: playerX/playerZ reserved for future distance-based event spawning
    void OWDynamicEventSystem::CheckForNewEvents([[maybe_unused]] float playerX, [[maybe_unused]] float playerZ,
                                                 uint32_t playerRegionId)
    {
        // Simple random event spawning near the player's region
        for (const auto& tmpl : m_templates)
        {
            // Check cooldown
            auto cdIt = m_cooldowns.find(tmpl.templateId);
            if (cdIt != m_cooldowns.end() && cdIt->second > 0.0f)
                continue;

            // Limit concurrent events
            if (m_activeEvents.size() >= 3)
                break;

            // Simplified biome check: use a deterministic "random" based on world time
            bool biomeMatch = false;
            for (auto b : tmpl.validBiomes)
            {
                if (static_cast<uint32_t>(b) + 1 == playerRegionId)
                {
                    biomeMatch = true;
                    break;
                }
            }
            if (!biomeMatch)
                continue;

            // Pseudo-random spawn chance (deterministic for reproducibility)
            uint32_t hash = (m_nextEventId * 31 + playerRegionId * 17 + tmpl.templateId * 13) % 100;
            if (hash < 85) // 15% chance per eligible template per check
                continue;

            TriggerEvent(tmpl.templateId, playerRegionId, playerX + static_cast<float>(hash), playerZ);
            break; // One event per check cycle
        }
    }

    uint32_t OWDynamicEventSystem::TriggerEvent(uint32_t templateId, uint32_t regionId, float x, float z)
    {
        const WorldEventTemplate* tmpl = nullptr;
        for (const auto& t : m_templates)
        {
            if (t.templateId == templateId)
            {
                tmpl = &t;
                break;
            }
        }
        if (!tmpl)
            return 0;

        ActiveWorldEvent evt{};
        evt.eventId = m_nextEventId++;
        evt.templateId = templateId;
        evt.name = tmpl->name;
        evt.type = tmpl->type;
        evt.state = EventState::Approaching;
        evt.posX = x;
        evt.posZ = z;
        evt.regionId = regionId;
        evt.timeRemaining = tmpl->duration;
        evt.totalDuration = tmpl->duration;

        m_activeEvents[evt.eventId] = evt;
        m_cooldowns[templateId] = tmpl->cooldown;

        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] EVENT: " + tmpl->name + " in Region " +
                                                    std::to_string(regionId) + " (id=" + std::to_string(evt.eventId) +
                                                    ")");
        return evt.eventId;
    }

    bool OWDynamicEventSystem::JoinEvent(uint32_t eventId)
    {
        auto it = m_activeEvents.find(eventId);
        if (it == m_activeEvents.end() || it->second.state == EventState::Completed)
            return false;

        it->second.playerParticipating = true;
        it->second.state = EventState::Active;
        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Joined event: " + it->second.name);
        return true;
    }

    // Intentional: playerX/playerZ reserved for future proximity-based event updates
    void OWDynamicEventSystem::UpdateActiveEvents(float deltaTime, [[maybe_unused]] float playerX,
                                                  [[maybe_unused]] float playerZ)
    {
        std::vector<uint32_t> completed;

        for (auto& [id, evt] : m_activeEvents)
        {
            evt.timeRemaining -= deltaTime;

            // State transitions based on remaining time
            float progress = 1.0f - (evt.timeRemaining / evt.totalDuration);
            if (progress > 0.9f)
                evt.state = EventState::Resolving;
            else if (progress > 0.1f && evt.state == EventState::Approaching)
                evt.state = EventState::Active;

            if (evt.timeRemaining <= 0.0f)
                completed.push_back(id);
        }

        for (uint32_t id : completed)
            CompleteEvent(id);
    }

    void OWDynamicEventSystem::CompleteEvent(uint32_t eventId)
    {
        auto it = m_activeEvents.find(eventId);
        if (it == m_activeEvents.end())
            return;

        it->second.state = EventState::Completed;
        m_totalEventsCompleted++;

        std::string result = it->second.playerParticipating ? "PARTICIPATED" : "MISSED";
        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Event completed: " + it->second.name + " (" + result +
                                                    ")");

        m_activeEvents.erase(it);
    }

    std::string OWDynamicEventSystem::GetEventListString() const
    {
        std::ostringstream ss;
        ss << "=== World Event Types ===\n";
        for (const auto& t : m_templates)
        {
            ss << "[" << t.templateId << "] " << t.name << " (dur:" << t.duration << "s, cd:" << t.cooldown << "s)\n";
            ss << "    " << t.description << "\n";
        }
        return ss.str();
    }

    std::string OWDynamicEventSystem::GetActiveEventsString() const
    {
        std::ostringstream ss;
        ss << "=== Active Events ===\n";
        if (m_activeEvents.empty())
        {
            ss << "No active events\n";
            return ss.str();
        }
        for (const auto& [id, evt] : m_activeEvents)
        {
            const char* stateStr = "???";
            switch (evt.state)
            {
            case EventState::Approaching:
                stateStr = "Approaching";
                break;
            case EventState::Active:
                stateStr = "Active";
                break;
            case EventState::Resolving:
                stateStr = "Resolving";
                break;
            default:
                break;
            }
            ss << "[" << id << "] " << evt.name << " (" << stateStr << ") " << evt.timeRemaining << "s remaining";
            if (evt.playerParticipating)
                ss << " [JOINED]";
            ss << "\n";
        }
        ss << "Total completed: " << m_totalEventsCompleted << "\n";
        return ss.str();
    }

    void OWDynamicEventSystem::Shutdown()
    {
        if (!m_initialized)
            return;

        m_templates.clear();
        m_activeEvents.clear();
        m_cooldowns.clear();
        m_initialized = false;
    }

    void OWDynamicEventSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (!ImGui::CollapsingHeader("Dynamic Events"))
            return;

        ImGui::Text("Event Types: %zu | Active: %zu | Completed: %u", m_templates.size(), m_activeEvents.size(),
                    m_totalEventsCompleted);
        ImGui::Separator();

        for (const auto& [id, evt] : m_activeEvents)
        {
            float progress = 1.0f - (evt.timeRemaining / evt.totalDuration);
            ImGui::ProgressBar(progress, ImVec2(-1, 0), evt.name.c_str());
        }
#endif
    }

} // namespace OpenWorld
