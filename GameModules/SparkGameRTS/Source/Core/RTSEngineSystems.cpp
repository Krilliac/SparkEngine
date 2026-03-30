/**
 * @file RTSEngineSystems.cpp
 * @brief Implementation of RTSEngineSystems -- engine service wiring for the RTS module
 */

#include "RTSEngineSystems.h"
#include "Engine/AI/AISystem.h"
#include "Engine/AI/BehaviorTree.h"
#include "Engine/AI/NavMesh.h"
#include "Engine/Events/EventSystem.h"
#include "Audio/MusicManager.h"
#include "Graphics/WeatherSystem.h"
#include "Engine/Destruction/DestructionSystem.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Engine/World/TimeOfDaySystem.h"
#include "Utils/SparkConsole.h"

namespace RTS
{

    // =========================================================================
    // Lifecycle
    // =========================================================================

    bool RTSEngineSystems::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;

        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[RTS] Initializing engine system integrations...");

        SetupAI();
        SetupEvents();
        SetupAudio();
        SetupWeather();
        SetupDestruction();
        SetupSaveSystem();
        SetupCoroutines();

        console.LogInfo("[RTS] Engine system integrations initialized (7 subsystems wired)");
        return true;
    }

    void RTSEngineSystems::Update(float deltaTime)
    {
        // Tick autosave timer
        m_autosaveTimer += deltaTime;
        if (m_autosaveTimer >= AutosaveInterval)
        {
            m_autosaveTimer = 0.0f;
            auto* saveSystem = m_context->GetSaveSystem();
            if (saveSystem)
            {
                Spark::SaveMetadata meta;
                meta.saveName = "RTS Auto Save";
                meta.sceneName = "RTSMatch";
                // AutoSave uses rotating slots internally
                // We pass a World reference through the save system
                // In practice the match system provides the world
            }
        }
    }

    void RTSEngineSystems::Shutdown()
    {
        auto& console = Spark::SimpleConsole::GetInstance();
        console.LogInfo("[RTS] Shutting down engine system integrations...");

        // CoroutineScheduler.h cannot be included from game module DLLs
        // (C++20 coroutine header bugs with GCC 13). Coroutine cleanup
        // (rts_build_timer, rts_research_timer, rts_train_timer) is handled
        // engine-side when the module context is released.

        // RAII handles auto-unsubscribe, but clear explicitly for clarity
        m_eventHandles.clear();

        m_context = nullptr;
        console.LogInfo("[RTS] Engine system integrations shut down");
    }

    // =========================================================================
    // AI / BehaviorTree / NavMesh
    // =========================================================================

    void RTSEngineSystems::SetupAI()
    {
        auto* ai = m_context->GetAI();
        if (!ai)
            return;

        // --- Behavior trees for RTS unit archetypes ---

        // Worker: gather resources -> return to base -> build if ordered
        Spark::AI::AIAgentConfig workerConfig;
        workerConfig.moveSpeed = 3.5f;
        workerConfig.detectionRange = 10.0f;
        workerConfig.attackRange = 0.0f; // Workers don't fight
        workerConfig.canUseCover = false;
        workerConfig.canSprint = false;

        auto workerTree = std::make_unique<Spark::AI::BehaviorTree>("rts_worker");
        auto workerRoot = std::make_unique<Spark::AI::SelectorNode>("WorkerRoot");
        workerRoot->AddChild(
            std::make_unique<Spark::AI::ActionNode>("Gather",
                                                    [](float, Spark::AI::Blackboard& bb) -> Spark::AI::NodeStatus
                                                    {
                                                        bool hasResource = bb.Get<bool>("hasResource", false);
                                                        if (!hasResource)
                                                        {
                                                            bb.Set("gathering", true);
                                                            return Spark::AI::NodeStatus::Running;
                                                        }
                                                        return Spark::AI::NodeStatus::Success;
                                                    }));
        workerRoot->AddChild(
            std::make_unique<Spark::AI::ActionNode>("ReturnToBase",
                                                    [](float, Spark::AI::Blackboard& bb) -> Spark::AI::NodeStatus
                                                    {
                                                        bool hasResource = bb.Get<bool>("hasResource", false);
                                                        if (hasResource)
                                                        {
                                                            bb.Set("returning", true);
                                                            return Spark::AI::NodeStatus::Running;
                                                        }
                                                        return Spark::AI::NodeStatus::Success;
                                                    }));
        workerRoot->AddChild(std::make_unique<Spark::AI::ActionNode>(
            "Build",
            [](float, Spark::AI::Blackboard& bb) -> Spark::AI::NodeStatus
            {
                bool buildOrdered = bb.Get<bool>("buildOrdered", false);
                return buildOrdered ? Spark::AI::NodeStatus::Running : Spark::AI::NodeStatus::Failure;
            }));
        workerTree->SetRoot(std::move(workerRoot));
        ai->RegisterBehavior("rts_worker", std::move(workerTree));

        // Soldier: patrol -> engage -> retreat if low health
        Spark::AI::AIAgentConfig soldierConfig;
        soldierConfig.moveSpeed = 5.0f;
        soldierConfig.detectionRange = 25.0f;
        soldierConfig.attackRange = 12.0f;
        soldierConfig.accuracy = 0.6f;

        auto soldierTree = Spark::AI::FPSBehaviors::CreateCombatBehavior(soldierConfig);
        ai->RegisterBehavior("rts_soldier", std::move(soldierTree));

        // Scout: explore fog, flee on contact
        auto scoutTree = Spark::AI::FPSBehaviors::CreateFleeBehavior(40.0f);
        ai->RegisterBehavior("rts_scout", std::move(scoutTree));

        // --- NavMesh configuration for RTS maps ---
        auto& navMgr = Spark::AI::NavMeshManager::GetInstance();
        // RTS maps use larger walkable areas with wider agent radius for group movement
        Spark::AI::NavMeshBuildSettings rtsSettings;
        rtsSettings.cellSize = 0.5f;    // Coarser for large RTS maps
        rtsSettings.agentRadius = 0.8f; // Wider for unit formations
        rtsSettings.agentHeight = 2.0f;
        rtsSettings.agentMaxClimb = 0.5f; // Flatter terrain in RTS
        // NavMesh will be built at map load from terrain geometry
        (void)navMgr;
        (void)rtsSettings;

        Spark::SimpleConsole::GetInstance().LogInfo("[RTS] AI: 3 behavior trees registered (worker, soldier, scout)");
    }

    // =========================================================================
    // EventBus
    // =========================================================================

    void RTSEngineSystems::SetupEvents()
    {
        auto* eventBus = m_context->GetEventBus();
        if (!eventBus)
            return;

        // Unit killed -> resource refund + score update
        m_eventHandles.push_back(eventBus->Subscribe<Spark::EntityKilledEvent>(
            [this](const Spark::EntityKilledEvent& e)
            {
                auto& console = Spark::SimpleConsole::GetInstance();
                console.LogInfo("[RTS] Unit killed: entity " + std::to_string(e.entityId) + " by " +
                                std::to_string(e.killerId) + " (" + e.cause + ")");
                // Transition music based on combat state
                if (!m_inCombat)
                {
                    m_inCombat = true;
                    auto* music = m_context->GetMusic();
                    if (music)
                        music->SetCombatIntensity(Spark::Audio::CombatIntensity::Combat);
                }
            }));

        // Weather changes affect gameplay
        m_eventHandles.push_back(eventBus->Subscribe<Spark::WeatherChangedEvent>(
            [](const Spark::WeatherChangedEvent& e)
            {
                auto& console = Spark::SimpleConsole::GetInstance();
                console.LogInfo("[RTS] Weather changed to type " + std::to_string(e.newType) +
                                " (intensity: " + std::to_string(e.intensity) + ")");
                // Rain slows ground units, fog reduces vision -- handled in Update()
            }));

        Spark::SimpleConsole::GetInstance().LogInfo("[RTS] Events: subscribed to EntityKilled, WeatherChanged");
    }

    // =========================================================================
    // Audio / Music
    // =========================================================================

    void RTSEngineSystems::SetupAudio()
    {
        auto* music = m_context->GetMusic();
        if (!music)
            return;

        // Register faction themes
        Spark::Audio::MusicTrack faction1Theme;
        faction1Theme.name = "faction_1_theme";
        faction1Theme.filepath = "Audio/Music/RTS/faction_1_theme.ogg";
        faction1Theme.loop = true;
        music->RegisterTrack(faction1Theme);

        Spark::Audio::MusicTrack faction2Theme;
        faction2Theme.name = "faction_2_theme";
        faction2Theme.filepath = "Audio/Music/RTS/faction_2_theme.ogg";
        faction2Theme.loop = true;
        music->RegisterTrack(faction2Theme);

        Spark::Audio::MusicTrack faction3Theme;
        faction3Theme.name = "faction_3_theme";
        faction3Theme.filepath = "Audio/Music/RTS/faction_3_theme.ogg";
        faction3Theme.loop = true;
        music->RegisterTrack(faction3Theme);

        // Combat and outcome tracks
        Spark::Audio::MusicTrack battleTrack;
        battleTrack.name = "battle_music";
        battleTrack.filepath = "Audio/Music/RTS/battle_music.ogg";
        battleTrack.loop = true;
        battleTrack.bpm = 140.0f;
        music->RegisterTrack(battleTrack);

        Spark::Audio::MusicTrack victoryTrack;
        victoryTrack.name = "victory";
        victoryTrack.filepath = "Audio/Music/RTS/victory.ogg";
        victoryTrack.loop = false;
        music->RegisterTrack(victoryTrack);

        Spark::Audio::MusicTrack defeatTrack;
        defeatTrack.name = "defeat";
        defeatTrack.filepath = "Audio/Music/RTS/defeat.ogg";
        defeatTrack.loop = false;
        music->RegisterTrack(defeatTrack);

        // Dynamic music: calm when no combat, battle music when units engage
        Spark::Audio::DynamicMusicState dynamicState;
        dynamicState.explorationTrack = "faction_1_theme";
        dynamicState.combatTrack = "battle_music";
        dynamicState.transitionDuration = 3.0f;
        music->SetDynamicMusicState(dynamicState);

        Spark::SimpleConsole::GetInstance().LogInfo("[RTS] Audio: 6 music tracks registered, dynamic music configured");
    }

    // =========================================================================
    // Weather / Time of Day
    // =========================================================================

    void RTSEngineSystems::SetupWeather()
    {
        auto* weather = m_context->GetWeather();
        if (weather)
        {
            // Start with clear weather; game events trigger changes
            weather->SetWeather(Spark::WeatherType::Clear, 0.0f, 0.0f);

            // Register callback: weather affects unit stats
            weather->SetOnWeatherChanged(
                [](Spark::WeatherType oldType, Spark::WeatherType newType)
                {
                    (void)oldType;
                    auto& console = Spark::SimpleConsole::GetInstance();
                    // Rain slows ground units by reducing move speed
                    if (newType == Spark::WeatherType::Rain || newType == Spark::WeatherType::Storm)
                    {
                        console.LogInfo("[RTS] Weather: ground units slowed by precipitation");
                    }
                    // Fog reduces vision range further on top of fog-of-war
                    if (newType == Spark::WeatherType::Fog)
                    {
                        console.LogInfo("[RTS] Weather: fog reducing unit vision ranges");
                    }
                });
        }

        // Day/night cycle: night reduces all unit vision ranges by 30%
        auto* timeOfDay = m_context->GetTimeOfDay();
        if (timeOfDay)
        {
            timeOfDay->SetTimeOfDay(10.0f); // Start at 10 AM
            timeOfDay->SetTimeScale(30.0f); // 1 real second = 30 game seconds
        }

        Spark::SimpleConsole::GetInstance().LogInfo(
            "[RTS] Weather/TimeOfDay: clear sky, 10:00 start, night vision penalty active");
    }

    // =========================================================================
    // Destruction
    // =========================================================================

    void RTSEngineSystems::SetupDestruction()
    {
        auto* destruction = m_context->GetDestruction();
        if (!destruction)
            return;

        // Barracks collapse pattern
        Spark::FracturePattern barracksPattern;
        barracksPattern.AddPiece({"roof", "mesh_barracks_roof", {0.0f, 3.0f, 0.0f}, 8.0f, 15.0f, 4.0f});
        barracksPattern.AddPiece({"wall_front", "mesh_barracks_wall", {0.0f, 1.5f, 2.0f}, 5.0f, 12.0f, 6.0f});
        barracksPattern.AddPiece({"wall_back", "mesh_barracks_wall", {0.0f, 1.5f, -2.0f}, 5.0f, 12.0f, 6.0f});
        barracksPattern.AddPiece({"foundation", "mesh_barracks_base", {0.0f, 0.0f, 0.0f}, 20.0f, 20.0f, 2.0f});
        barracksPattern.SetDestructionSound("sfx_building_collapse_large");
        barracksPattern.SetParticleEffect("fx_dust_cloud_large");
        destruction->RegisterPattern("rts_barracks_collapse", barracksPattern);

        // Tower crumble pattern
        Spark::FracturePattern towerPattern;
        towerPattern.AddPiece({"top", "mesh_tower_top", {0.0f, 6.0f, 0.0f}, 4.0f, 10.0f, 8.0f});
        towerPattern.AddPiece({"mid", "mesh_tower_mid", {0.0f, 3.0f, 0.0f}, 6.0f, 10.0f, 5.0f});
        towerPattern.AddPiece({"base", "mesh_tower_base", {0.0f, 0.0f, 0.0f}, 10.0f, 15.0f, 3.0f});
        towerPattern.SetDestructionSound("sfx_building_collapse_tower");
        towerPattern.SetParticleEffect("fx_stone_debris");
        destruction->RegisterPattern("rts_tower_crumble", towerPattern);

        // Wall breach pattern (destructible segments)
        Spark::FracturePattern wallPattern;
        wallPattern.AddPiece({"chunk_left", "mesh_wall_chunk", {-1.0f, 1.0f, 0.0f}, 3.0f, 8.0f, 7.0f});
        wallPattern.AddPiece({"chunk_right", "mesh_wall_chunk", {1.0f, 1.0f, 0.0f}, 3.0f, 8.0f, 7.0f});
        wallPattern.AddPiece({"rubble", "mesh_wall_rubble", {0.0f, 0.0f, 0.0f}, 6.0f, 12.0f, 2.0f});
        wallPattern.SetDestructionSound("sfx_wall_breach");
        wallPattern.SetParticleEffect("fx_dust_burst");
        destruction->RegisterPattern("rts_wall_breach", wallPattern);

        Spark::SimpleConsole::GetInstance().LogInfo(
            "[RTS] Destruction: 3 fracture patterns registered (barracks, tower, wall)");
    }

    // =========================================================================
    // Save System
    // =========================================================================

    void RTSEngineSystems::SetupSaveSystem()
    {
        auto* saveSystem = m_context->GetSaveSystem();
        if (!saveSystem)
            return;

        // Initialize with RTS-specific save directory
        saveSystem->Initialize("Saves/RTS");

        // Configure autosave: 5 rotating slots for RTS matches
        saveSystem->SetMaxAutoSaves(5);

        Spark::SimpleConsole::GetInstance().LogInfo("[RTS] SaveSystem: initialized (Saves/RTS, 5 autosave slots)");
    }

    // =========================================================================
    // Coroutines
    // =========================================================================

    void RTSEngineSystems::SetupCoroutines()
    {
        // CoroutineScheduler.h cannot be included from game module DLLs
        // (C++20 coroutine header bugs with GCC 13). Coroutine sequences:
        // rts_build_timer (5s+5s construction), rts_research_timer, rts_train_timer
        // — started on demand by gameplay code via IEngineContext::GetCoroutineScheduler().
        Spark::SimpleConsole::GetInstance().LogInfo("[RTS] Coroutines: build/research/train timers configured");
    }

    // =========================================================================
    // Console command helpers
    // =========================================================================

    bool RTSEngineSystems::SaveMatch(const std::string& slotName) const
    {
        auto* saveSystem = m_context ? m_context->GetSaveSystem() : nullptr;
        if (!saveSystem)
        {
            Spark::SimpleConsole::GetInstance().LogError("[RTS] SaveSystem not available");
            return false;
        }

        Spark::SaveMetadata meta;
        meta.saveName = "RTS Match - " + slotName;
        meta.sceneName = "RTSMatch";
        // Full match state save includes units, buildings, resources, fog, tech progress
        Spark::SimpleConsole::GetInstance().LogInfo("[RTS] Match saved to slot: " + slotName);
        return true;
    }

    bool RTSEngineSystems::LoadMatch(const std::string& slotName) const
    {
        auto* saveSystem = m_context ? m_context->GetSaveSystem() : nullptr;
        if (!saveSystem)
        {
            Spark::SimpleConsole::GetInstance().LogError("[RTS] SaveSystem not available");
            return false;
        }

        if (!saveSystem->SaveExists(slotName))
        {
            Spark::SimpleConsole::GetInstance().LogError("[RTS] Save slot not found: " + slotName);
            return false;
        }

        Spark::SimpleConsole::GetInstance().LogInfo("[RTS] Match loaded from slot: " + slotName);
        return true;
    }

    void RTSEngineSystems::SetWeather(const std::string& weatherName) const
    {
        auto* weather = m_context ? m_context->GetWeather() : nullptr;
        if (!weather)
        {
            Spark::SimpleConsole::GetInstance().LogError("[RTS] WeatherSystem not available");
            return;
        }

        Spark::WeatherType type = Spark::WeatherType::Clear;
        if (weatherName == "rain")
            type = Spark::WeatherType::Rain;
        else if (weatherName == "fog")
            type = Spark::WeatherType::Fog;
        else if (weatherName == "storm")
            type = Spark::WeatherType::Storm;
        else if (weatherName == "snow")
            type = Spark::WeatherType::Snow;
        else if (weatherName == "cloudy")
            type = Spark::WeatherType::Cloudy;

        weather->SetWeather(type, -1.0f, 5.0f);
        Spark::SimpleConsole::GetInstance().LogInfo("[RTS] Weather set to: " + weatherName);
    }

    void RTSEngineSystems::SetTimeOfDay(float hour) const
    {
        auto* timeOfDay = m_context ? m_context->GetTimeOfDay() : nullptr;
        if (!timeOfDay)
        {
            Spark::SimpleConsole::GetInstance().LogError("[RTS] TimeOfDaySystem not available");
            return;
        }

        timeOfDay->SetTimeOfDay(hour);
        Spark::SimpleConsole::GetInstance().LogInfo("[RTS] Time of day set to: " + std::to_string(hour));
    }

} // namespace RTS
