/**
 * @file Main.cpp
 * @brief SparkGameOpenWorld DLL - IModule implementation and exports
 *
 * Implements the SparkGameOpenWorldModule class and exports the CreateModule/
 * DestroyModule factory functions for the engine's ModuleManager.
 */

#include "SparkGameOpenWorld.h"
#include "OWEngineSystems.h"
#include "World/OWWorldSetup.h"
#include "Player/OWPlayerSystem.h"
#include "Exploration/OWExplorationSystem.h"
#include "Wildlife/OWWildlifeSystem.h"
#include "Settlement/OWSettlementSystem.h"
#include "Gathering/OWGatheringSystem.h"
#include "Events/OWDynamicEventSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#include "Utils/InvalidStateDetector.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/GameplayComponents.h"
#include "Engine/ECS/Components/AIComponents.h"

#include <Spark/ModuleDllMain.h>

// =============================================================================
// Module exports
// =============================================================================

SPARK_IMPLEMENT_MODULE(SparkGameOpenWorldModule)

// =============================================================================
// SparkGameOpenWorldModule implementation
// =============================================================================

SparkGameOpenWorldModule::SparkGameOpenWorldModule() = default;

SparkGameOpenWorldModule::~SparkGameOpenWorldModule()
{
    if (m_initialized)
        OnUnload();
}

Spark::ModuleInfo SparkGameOpenWorldModule::GetModuleInfo() const
{
    Spark::ModuleInfo info{};
    info.name = "Spark Open World - Exploration Showcase";
    info.version = "1.0.0";
    info.sdkVersion = SPARK_SDK_VERSION;
    info.loadOrder = 1008;
    return info;
}

bool SparkGameOpenWorldModule::OnLoad(Spark::IEngineContext* context)
{
    if (!context)
        return false;

    m_context = context;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[OpenWorld] Loading Spark Open World module...");
    SPARK_LOG_INFO(Spark::LogCategory::Game, "Open World module loading - initializing 8 subsystems");

    // 1. World setup (regions, roads, streaming, origin rebasing)
    m_worldSetup = std::make_unique<OpenWorld::OWWorldSetup>();
    if (!m_worldSetup->Initialize(context))
    {
        console.LogError("[OpenWorld] Failed to initialize world setup");
        return false;
    }

    // 2. Player system (survival, stamina, fast travel, compass)
    m_playerSystem = std::make_unique<OpenWorld::OWPlayerSystem>();
    if (!m_playerSystem->Initialize(context))
    {
        console.LogError("[OpenWorld] Failed to initialize player system");
        return false;
    }

    // 3. Exploration system (POIs, discovery, map fog)
    m_explorationSystem = std::make_unique<OpenWorld::OWExplorationSystem>();
    if (!m_explorationSystem->Initialize(context))
    {
        console.LogError("[OpenWorld] Failed to initialize exploration system");
        return false;
    }
    m_explorationSystem->SetDiscoveryCallback(
        [this](const OpenWorld::PointOfInterest& poi)
        {
            if (poi.type != OpenWorld::POIType::FastTravel || !m_playerSystem)
                return;
            m_playerSystem->UnlockFastTravel({poi.poiId, poi.name, poi.x, poi.y, poi.z, poi.regionId});
        });

    // 4. Wildlife system (species, herds, predator-prey, taming)
    m_wildlifeSystem = std::make_unique<OpenWorld::OWWildlifeSystem>();
    if (!m_wildlifeSystem->Initialize(context))
    {
        console.LogError("[OpenWorld] Failed to initialize wildlife system");
        return false;
    }

    // 5. Settlement system (NPC villages, player camps)
    m_settlementSystem = std::make_unique<OpenWorld::OWSettlementSystem>();
    if (!m_settlementSystem->Initialize(context))
    {
        console.LogError("[OpenWorld] Failed to initialize settlement system");
        return false;
    }

    // 6. Gathering system (resource nodes, harvesting, crafting)
    m_gatheringSystem = std::make_unique<OpenWorld::OWGatheringSystem>();
    if (!m_gatheringSystem->Initialize(context))
    {
        console.LogError("[OpenWorld] Failed to initialize gathering system");
        return false;
    }

    // 7. Dynamic event system (world events, random encounters)
    m_eventSystem = std::make_unique<OpenWorld::OWDynamicEventSystem>();
    if (!m_eventSystem->Initialize(context))
    {
        console.LogError("[OpenWorld] Failed to initialize dynamic event system");
        return false;
    }

    // 8. Engine systems integration (save, AI, cinematic, weather, music, events)
    m_engineSystems = std::make_unique<OpenWorld::OWEngineSystems>();
    if (!m_engineSystems->Initialize(context))
    {
        console.LogWarning("[OpenWorld] Engine systems integration partially failed (non-fatal)");
    }

    RegisterConsoleCommands();

    // Register OpenWorld-specific state validation rules
    auto& stateDetector = Spark::InvalidStateDetector::GetInstance();

    stateDetector.AddRule(
        {"OpenWorld.DeadWildlife", "OpenWorld", Spark::StateViolationSeverity::Warning, true,
         [](World& w, std::vector<Spark::StateViolation>& out)
         {
             for (auto entity : w.GetEntitiesWith<HealthComponent, AIComponent>())
             {
                 auto* h = w.GetComponent<HealthComponent>(entity);
                 auto* ai = w.GetComponent<AIComponent>(entity);
                 if (h && ai && h->isDead &&
                     (ai->state == AIComponent::State::Patrolling || ai->state == AIComponent::State::Alert))
                 {
                     out.push_back({"OpenWorld.DeadWildlife", static_cast<uint32_t>(entity),
                                    "Dead wildlife AI still patrolling/alert", Spark::StateViolationSeverity::Warning});
                 }
             }
         }});

    stateDetector.AddRule({"OpenWorld.MaxHealthZero", "OpenWorld", Spark::StateViolationSeverity::Error, true,
                           [](World& w, std::vector<Spark::StateViolation>& out)
                           {
                               for (auto entity : w.GetEntitiesWith<HealthComponent>())
                               {
                                   auto* h = w.GetComponent<HealthComponent>(entity);
                                   if (h && h->maxHealth <= 0.0f)
                                   {
                                       out.push_back({"OpenWorld.MaxHealthZero", static_cast<uint32_t>(entity),
                                                      "maxHealth=" + std::to_string(h->maxHealth) + " is not positive",
                                                      Spark::StateViolationSeverity::Error});
                                   }
                               }
                           }});

    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "Open World module loaded successfully - 8 subsystems active");
    console.LogInfo("[OpenWorld] Module loaded successfully (8 subsystems)");
    console.LogInfo("[OpenWorld] Regions: " + std::to_string(m_worldSetup->GetRegionCount()) +
                    " | POIs: " + std::to_string(m_explorationSystem->GetPOICount()) +
                    " | Species: " + std::to_string(m_wildlifeSystem->GetSpeciesCount()) +
                    " | Settlements: " + std::to_string(m_settlementSystem->GetSettlementCount()) +
                    " | Nodes: " + std::to_string(m_gatheringSystem->GetNodeCount()) +
                    " | Recipes: " + std::to_string(m_gatheringSystem->GetRecipeCount()) +
                    " | Events: " + std::to_string(m_eventSystem->GetTemplateCount()));
    return true;
}

void SparkGameOpenWorldModule::OnUnload()
{
    if (!m_initialized)
        return;

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("[OpenWorld] Unloading Spark Open World module...");
    SPARK_LOG_INFO(Spark::LogCategory::Game, "Open World module shutting down");

    // Shutdown in reverse initialization order
    if (m_engineSystems)
    {
        m_engineSystems->Shutdown();
        m_engineSystems.reset();
    }
    if (m_eventSystem)
    {
        m_eventSystem->Shutdown();
        m_eventSystem.reset();
    }
    if (m_gatheringSystem)
    {
        m_gatheringSystem->Shutdown();
        m_gatheringSystem.reset();
    }
    if (m_settlementSystem)
    {
        m_settlementSystem->Shutdown();
        m_settlementSystem.reset();
    }
    if (m_wildlifeSystem)
    {
        m_wildlifeSystem->Shutdown();
        m_wildlifeSystem.reset();
    }
    if (m_explorationSystem)
    {
        m_explorationSystem->Shutdown();
        m_explorationSystem.reset();
    }
    if (m_playerSystem)
    {
        m_playerSystem->Shutdown();
        m_playerSystem.reset();
    }
    if (m_worldSetup)
    {
        m_worldSetup->Shutdown();
        m_worldSetup.reset();
    }

    m_context = nullptr;
    m_initialized = false;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "Open World module unloaded");
    console.LogInfo("[OpenWorld] Module unloaded");
}

void SparkGameOpenWorldModule::OnUpdate(float deltaTime)
{
    if (!m_initialized || m_paused)
        return;

    m_worldSetup->Update(deltaTime);
    m_playerSystem->Update(deltaTime);
    const auto& position = m_playerSystem->GetWorldState();
    const OpenWorld::BiomeRegion* region =
        m_worldSetup->GetRegionAtPosition(position.posX, position.posY, position.posZ);
    m_playerSystem->SetCurrentRegion(region ? region->regionId : 0);
    const auto& playerState = m_playerSystem->GetWorldState();
    m_explorationSystem->Update(deltaTime, playerState.posX, playerState.posY, playerState.posZ);
    m_wildlifeSystem->Update(deltaTime, playerState.posX, playerState.posZ, playerState.currentRegionId);
    m_settlementSystem->Update(deltaTime);
    m_gatheringSystem->Update(deltaTime);
    m_eventSystem->Update(deltaTime, playerState.posX, playerState.posZ, playerState.currentRegionId);
    m_engineSystems->Update(deltaTime);
}

void SparkGameOpenWorldModule::OnFixedUpdate(float fixedDeltaTime)
{
    if (!m_initialized || m_paused)
        return;

    m_playerSystem->FixedUpdate(fixedDeltaTime);
}

void SparkGameOpenWorldModule::OnRender()
{
    if (!m_initialized)
        return;
}

void SparkGameOpenWorldModule::OnResize(int width, int height)
{
    (void)width;
    (void)height;
}

void SparkGameOpenWorldModule::OnPause()
{
    m_paused = true;
}

void SparkGameOpenWorldModule::OnResume()
{
    m_paused = false;
}

void SparkGameOpenWorldModule::OnImGui()
{
    if (!m_initialized)
        return;

    m_worldSetup->RenderDebugUI();
    m_playerSystem->RenderDebugUI();
    m_explorationSystem->RenderDebugUI();
    m_wildlifeSystem->RenderDebugUI();
    m_settlementSystem->RenderDebugUI();
    m_gatheringSystem->RenderDebugUI();
    m_eventSystem->RenderDebugUI();
    m_engineSystems->RenderDebugUI();
}

void SparkGameOpenWorldModule::RegisterConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();

    // --- Status ---
    console.RegisterCommand(
        "ow_status",
        [this](const std::vector<std::string>&) -> std::string
        {
            std::string s = "=== Spark Open World Status ===\n";
            s += "Regions: " + std::to_string(m_worldSetup->GetRegionCount()) + "\n";
            s += "POIs: " + std::to_string(m_explorationSystem->GetDiscoveredCount()) + "/" +
                 std::to_string(m_explorationSystem->GetPOICount()) + "\n";
            s += "Wildlife: " + std::to_string(m_wildlifeSystem->GetActiveAnimalCount()) + " active, " +
                 std::to_string(m_wildlifeSystem->GetTamedCount()) + " tamed\n";
            s += "Settlements: " + std::to_string(m_settlementSystem->GetSettlementCount()) +
                 " | Camps: " + std::to_string(m_settlementSystem->GetCampCount()) + "\n";
            s += "Nodes: " + std::to_string(m_gatheringSystem->GetNodeCount()) +
                 " | Recipes: " + std::to_string(m_gatheringSystem->GetRecipeCount()) + "\n";
            s += "Active Events: " + std::to_string(m_eventSystem->GetActiveEventCount()) + "\n";
            s += m_worldSetup->GetWorldStatusString() + "\n";
            return s;
        },
        "Show open world module status", "OpenWorld");

    // --- World ---
    console.RegisterCommand(
        "ow_regions", [this](const std::vector<std::string>&) -> std::string
        { return m_worldSetup->GetRegionListString(); }, "List world regions", "OpenWorld");

    // --- Player ---
    console.RegisterCommand(
        "ow_player", [this](const std::vector<std::string>&) -> std::string
        { return m_playerSystem->GetStatusString(); }, "Show player status", "OpenWorld");

    console.RegisterCommand(
        "ow_eat",
        [this](const std::vector<std::string>&) -> std::string
        {
            m_playerSystem->Eat(30.0f);
            return "Ate food (+30 hunger)";
        },
        "Eat food", "OpenWorld");

    console.RegisterCommand(
        "ow_drink",
        [this](const std::vector<std::string>&) -> std::string
        {
            m_playerSystem->Drink(40.0f);
            return "Drank water (+40 thirst)";
        },
        "Drink water", "OpenWorld");

    console.RegisterCommand(
        "ow_teleport",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 3)
                return "Usage: ow_teleport <x> <y> <z>";
            try
            {
                float x = std::stof(args[0]);
                float y = std::stof(args[1]);
                float z = std::stof(args[2]);
                m_playerSystem->SetPosition(x, y, z);
                return "Teleported to (" + args[0] + ", " + args[1] + ", " + args[2] + ")";
            }
            catch (...)
            {
                return "Invalid coordinates";
            }
        },
        "Teleport player", "OpenWorld", "ow_teleport <x> <y> <z>");

    console.RegisterCommand(
        "ow_fast_travel",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
            {
                std::string s = "Fast Travel Points:\n";
                for (const auto& pt : m_playerSystem->GetFastTravelPoints())
                    s += "  [" + std::to_string(pt.pointId) + "] " + pt.name + "\n";
                return s + "Usage: ow_fast_travel <point_id>";
            }
            try
            {
                uint32_t id = static_cast<uint32_t>(std::stoi(args[0]));
                return m_playerSystem->FastTravelTo(id) ? "Fast traveled!" : "Invalid point";
            }
            catch (...)
            {
                return "Invalid point ID";
            }
        },
        "Fast travel to a discovered point", "OpenWorld");

    // --- Exploration ---
    console.RegisterCommand(
        "ow_explore", [this](const std::vector<std::string>&) -> std::string
        { return m_explorationSystem->GetExplorationString(); }, "Show exploration progress", "OpenWorld");

    console.RegisterCommand(
        "ow_pois", [this](const std::vector<std::string>&) -> std::string
        { return m_explorationSystem->GetPOIListString(); }, "List all points of interest", "OpenWorld");

    // --- Wildlife ---
    console.RegisterCommand(
        "ow_wildlife", [this](const std::vector<std::string>&) -> std::string
        { return m_wildlifeSystem->GetWildlifeString(); }, "Show wildlife summary", "OpenWorld");

    console.RegisterCommand(
        "ow_species", [this](const std::vector<std::string>&) -> std::string
        { return m_wildlifeSystem->GetSpeciesListString(); }, "List animal species", "OpenWorld");

    console.RegisterCommand(
        "ow_tame",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: ow_tame <animal_id>";
            try
            {
                uint32_t id = static_cast<uint32_t>(std::stoi(args[0]));
                return m_wildlifeSystem->TameAnimal(id) ? "Animal tamed!"
                                                        : "Cannot tame (invalid, dead, or untameable)";
            }
            catch (...)
            {
                return "Invalid animal ID";
            }
        },
        "Tame a wildlife animal", "OpenWorld");

    // --- Settlements ---
    console.RegisterCommand(
        "ow_settlements", [this](const std::vector<std::string>&) -> std::string
        { return m_settlementSystem->GetSettlementListString(); }, "List settlements", "OpenWorld");

    console.RegisterCommand(
        "ow_camps", [this](const std::vector<std::string>&) -> std::string
        { return m_settlementSystem->GetCampListString(); }, "List player camps", "OpenWorld");

    console.RegisterCommand(
        "ow_place_camp",
        [this](const std::vector<std::string>& args) -> std::string
        {
            std::string name = args.empty() ? "Camp" : args[0];
            const auto& ps = m_playerSystem->GetWorldState();
            uint32_t id = m_settlementSystem->PlaceCamp(name, ps.posX, ps.posY, ps.posZ, ps.currentRegionId);
            return "Camp '" + name + "' placed (id=" + std::to_string(id) + ")";
        },
        "Place a camp at current position", "OpenWorld");

    console.RegisterCommand(
        "ow_upgrade_camp",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: ow_upgrade_camp <camp_id>";
            try
            {
                uint32_t id = static_cast<uint32_t>(std::stoi(args[0]));
                return m_settlementSystem->UpgradeCamp(id) ? "Camp upgraded!" : "Cannot upgrade (max tier or invalid)";
            }
            catch (...)
            {
                return "Invalid camp ID";
            }
        },
        "Upgrade a player camp", "OpenWorld");

    // --- Gathering / Crafting ---
    console.RegisterCommand(
        "ow_nodes", [this](const std::vector<std::string>&) -> std::string
        { return m_gatheringSystem->GetNodeListString(); }, "List resource nodes", "OpenWorld");

    console.RegisterCommand(
        "ow_inventory", [this](const std::vector<std::string>&) -> std::string
        { return m_gatheringSystem->GetInventoryString(); }, "Show resource inventory", "OpenWorld");

    console.RegisterCommand(
        "ow_recipes", [this](const std::vector<std::string>&) -> std::string
        { return m_gatheringSystem->GetRecipeListString(); }, "List crafting recipes", "OpenWorld");

    console.RegisterCommand(
        "ow_harvest",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: ow_harvest <node_id>";
            try
            {
                uint32_t id = static_cast<uint32_t>(std::stoi(args[0]));
                uint32_t amount = m_gatheringSystem->HarvestNode(id);
                return amount > 0 ? "Harvested " + std::to_string(amount) + " resources" : "Node depleted or invalid";
            }
            catch (...)
            {
                return "Invalid node ID";
            }
        },
        "Harvest a resource node", "OpenWorld");

    console.RegisterCommand(
        "ow_craft",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: ow_craft <recipe_id>";
            try
            {
                uint32_t id = static_cast<uint32_t>(std::stoi(args[0]));
                return m_gatheringSystem->Craft(id) ? "Crafted!" : "Cannot craft (missing resources or invalid)";
            }
            catch (...)
            {
                return "Invalid recipe ID";
            }
        },
        "Craft an item", "OpenWorld");

    // --- Events ---
    console.RegisterCommand(
        "ow_events", [this](const std::vector<std::string>&) -> std::string
        { return m_eventSystem->GetActiveEventsString(); }, "Show active world events", "OpenWorld");

    console.RegisterCommand(
        "ow_event_types", [this](const std::vector<std::string>&) -> std::string
        { return m_eventSystem->GetEventListString(); }, "List event types", "OpenWorld");

    console.RegisterCommand(
        "ow_trigger_event",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: ow_trigger_event <template_id>";
            try
            {
                uint32_t tmplId = static_cast<uint32_t>(std::stoi(args[0]));
                const auto& ps = m_playerSystem->GetWorldState();
                uint32_t evtId = m_eventSystem->TriggerEvent(tmplId, ps.currentRegionId, ps.posX, ps.posZ);
                return evtId > 0 ? "Event triggered (id=" + std::to_string(evtId) + ")" : "Failed to trigger";
            }
            catch (...)
            {
                return "Invalid template ID";
            }
        },
        "Trigger a world event", "OpenWorld");

    // --- Engine integration ---
    console.RegisterCommand(
        "ow_save",
        [this](const std::vector<std::string>& args) -> std::string
        {
            std::string slot = args.empty() ? "slot1" : args[0];
            return m_engineSystems->SaveGame(slot);
        },
        "Save game", "OpenWorld");

    console.RegisterCommand(
        "ow_load",
        [this](const std::vector<std::string>& args) -> std::string
        {
            std::string slot = args.empty() ? "slot1" : args[0];
            return m_engineSystems->LoadGame(slot);
        },
        "Load game", "OpenWorld");

    console.RegisterCommand(
        "ow_weather",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: ow_weather <clear|cloudy|rain|snow|fog|storm>";
            return m_engineSystems->SetWeather(args[0]);
        },
        "Set weather", "OpenWorld");

    console.RegisterCommand(
        "ow_time",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "Usage: ow_time <hour> (0-24)";
            try
            {
                float hour = std::stof(args[0]);
                return m_engineSystems->SetTime(hour);
            }
            catch (...)
            {
                return "Invalid hour";
            }
        },
        "Set time of day", "OpenWorld");
}
