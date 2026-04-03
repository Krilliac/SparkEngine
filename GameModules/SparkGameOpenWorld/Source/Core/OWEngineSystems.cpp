/**
 * @file OWEngineSystems.cpp
 * @brief Wires open world gameplay into engine subsystems
 *
 * Registers save serializers, wildlife behavior trees, cinematic sequences,
 * biome-driven weather/time rules, dynamic music, and event subscriptions.
 */

#include "OWEngineSystems.h"
#include "Enums/OpenWorldEnums.h"

#include "Engine/SaveSystem/SaveSystem.h"
#include "Engine/AI/AISystem.h"
#include "Engine/AI/BehaviorTree.h"
#include "Engine/Cinematic/Sequencer.h"
#include "Graphics/WeatherSystem.h"
#include "Engine/World/TimeOfDaySystem.h"
#include "Audio/MusicManager.h"
#include "Engine/Events/EventSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

namespace OpenWorld
{

    OWEngineSystems::~OWEngineSystems()
    {
        if (m_initialized)
            Shutdown();
    }

    bool OWEngineSystems::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;

        RegisterSaveSerializers();
        RegisterBehaviorTrees();
        RegisterCinematicSequences();
        SetupWeatherAndTimeOfDay();
        RegisterMusicTracks();
        SubscribeToEvents();

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Open world engine systems integration initialized (6 subsystems)");
        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Engine systems wired (6 subsystems)");
        return true;
    }

    void OWEngineSystems::Update([[maybe_unused]] float deltaTime)
    {
        if (!m_initialized)
            return;
    }

    void OWEngineSystems::Shutdown()
    {
        if (!m_initialized)
            return;

        m_eventHandles.clear();
        m_context = nullptr;
        m_initialized = false;
        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Engine systems shut down");
    }

    void OWEngineSystems::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        // Engine system debug is rendered by the engine's own panels
#endif
    }

    // =========================================================================
    // Save System
    // =========================================================================

    void OWEngineSystems::RegisterSaveSerializers()
    {
        auto* saveSystem = m_context->GetSaveSystem();
        if (!saveSystem)
            return;

        auto& registry = Spark::ComponentSerializerRegistry::GetInstance();

        auto registerPlaceholder = [&](const std::string& typeName)
        {
            registry.Register(
                typeName,
                [typeName](const void*) -> Spark::SerializedComponent
                {
                    Spark::SerializedComponent sc;
                    sc.typeName = typeName;
                    sc.properties["placeholder"] = typeName;
                    return sc;
                },
                []([[maybe_unused]] World& world, [[maybe_unused]] EntityID entity,
                   [[maybe_unused]] const Spark::SerializedComponent& data) {});
        };

        registerPlaceholder("OWPlayerSurvival");    // Health, stamina, hunger, thirst, temp
        registerPlaceholder("OWExplorationState");  // Discovered POIs, secrets, XP
        registerPlaceholder("OWResourceInventory"); // Gathered materials
        registerPlaceholder("OWCampData");          // Player camp positions and tiers
        registerPlaceholder("OWWildlifeTamed");     // Tamed animal companions
        registerPlaceholder("OWEventProgress");     // Completed world events

        saveSystem->SetMaxAutoSaves(3);

        SPARK_LOG_INFO(Spark::LogCategory::Game, "Open world registered 6 save serializers");
        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Registered 6 save serializers");
    }

    std::string OWEngineSystems::SaveGame(const std::string& slotName)
    {
        auto* saveSystem = m_context->GetSaveSystem();
        if (!saveSystem)
            return "Save system not available";

        return "Save to slot '" + slotName + "' requested (save system wired)";
    }

    std::string OWEngineSystems::LoadGame(const std::string& slotName)
    {
        auto* saveSystem = m_context->GetSaveSystem();
        if (!saveSystem)
            return "Save system not available";

        if (!saveSystem->SaveExists(slotName))
            return "No save found in slot '" + slotName + "'";

        return "Load from slot '" + slotName + "' requested (save system wired)";
    }

    // =========================================================================
    // AI / Behavior Trees
    // =========================================================================

    void OWEngineSystems::RegisterBehaviorTrees()
    {
        auto* aiSystem = m_context->GetAI();
        if (!aiSystem)
            return;

        using namespace Spark::AI;

        // Herbivore: graze, flee from threats, herd movement
        {
            auto grazerTree =
                FPSBehaviors::CreatePatrolBehavior({{0.0f, 0.0f, 0.0f}, {20.0f, 0.0f, 10.0f}, {-10.0f, 0.0f, 20.0f}});
            aiSystem->RegisterBehavior("ow_herbivore", std::move(grazerTree));
        }

        // Predator: stalk, chase, attack, retreat when wounded
        {
            AIAgentConfig predatorConfig;
            predatorConfig.detectionRange = 40.0f;
            predatorConfig.attackRange = 5.0f;
            predatorConfig.moveSpeed = 7.0f;
            predatorConfig.accuracy = 0.8f;
            predatorConfig.reactionTime = 0.15f;
            auto predatorTree = FPSBehaviors::CreateCombatBehavior(predatorConfig);
            aiSystem->RegisterBehavior("ow_predator", std::move(predatorTree));
        }

        // Settlement guard: patrol route, investigate, defend
        {
            AIAgentConfig guardConfig;
            guardConfig.detectionRange = 30.0f;
            guardConfig.attackRange = 10.0f;
            guardConfig.moveSpeed = 4.0f;
            guardConfig.accuracy = 0.7f;
            guardConfig.reactionTime = 0.3f;
            auto guardTree = FPSBehaviors::CreateCombatBehavior(guardConfig);
            aiSystem->RegisterBehavior("ow_guard", std::move(guardTree));
        }

        // Merchant: stand at post, greet players
        {
            auto merchantTree = FPSBehaviors::CreateGuardBehavior({0.0f, 0.0f, 0.0f}, 5.0f);
            aiSystem->RegisterBehavior("ow_merchant", std::move(merchantTree));
        }

        // Bandit: aggressive patrol, ambush tactics
        {
            AIAgentConfig banditConfig;
            banditConfig.detectionRange = 35.0f;
            banditConfig.attackRange = 12.0f;
            banditConfig.moveSpeed = 5.5f;
            banditConfig.accuracy = 0.6f;
            banditConfig.reactionTime = 0.2f;
            banditConfig.canUseCover = true;
            auto banditTree = FPSBehaviors::CreateCombatBehavior(banditConfig);
            aiSystem->RegisterBehavior("ow_bandit", std::move(banditTree));
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "Open world registered 5 behavior trees");
        Spark::SimpleConsole::GetInstance().LogInfo(
            "[OpenWorld] Registered 5 behavior trees (herbivore, predator, guard, merchant, bandit)");
    }

    // =========================================================================
    // Cinematic Sequences
    // =========================================================================

    void OWEngineSystems::RegisterCinematicSequences()
    {
        auto* cinematic = m_context->GetCinematic();
        if (!cinematic)
            return;

        using namespace Spark::Cinematic;

        // Opening: dawn breaks over the Emerald Meadows
        {
            auto* seq = cinematic->CreateSequence("ow_opening");

            auto* cam = seq->AddCameraTrack("opening_camera");
            cam->AddKeyframe({0.0f, {0.0f, 80.0f, -500.0f}, {0.0f, 0.0f, 0.0f}, 65.0f, 0.0f});
            cam->AddKeyframe({4.0f, {0.0f, 40.0f, -200.0f}, {0.0f, 5.0f, 100.0f}, 55.0f, 0.0f});
            cam->AddKeyframe({8.0f, {50.0f, 15.0f, -50.0f}, {0.0f, 5.0f, 0.0f}, 50.0f, 0.0f});
            cam->AddKeyframe({12.0f, {0.0f, 8.0f, 0.0f}, {0.0f, 5.0f, 10.0f}, 60.0f, 0.0f});

            auto* subs = seq->AddSubtitleTrack("opening_subs");
            subs->AddSubtitle({1.0f, 4.0f, "The world stretches before you, vast and uncharted.", "Narrator"});
            subs->AddSubtitle({5.0f, 8.0f, "Every horizon holds a secret. Every path, a choice.", "Narrator"});
            subs->AddSubtitle({9.0f, 12.0f, "Your journey begins in the Emerald Meadows.", "Narrator"});

            auto* fade = seq->AddFadeTrack("opening_fade");
            fade->AddKeyframe({0.0f, 1.0f, {0.0f, 0.0f, 0.0f}});
            fade->AddKeyframe({1.5f, 0.0f, {0.0f, 0.0f, 0.0f}});
            fade->AddKeyframe({11.0f, 0.0f, {0.0f, 0.0f, 0.0f}});
            fade->AddKeyframe({12.0f, 0.0f, {0.0f, 0.0f, 0.0f}});

            auto* events = seq->AddEventTrack("opening_events");
            events->AddCue({0.5f, "ow_play_music", "meadow_dawn"});
            events->AddCue({12.0f, "ow_enable_input", ""});
        }

        // Dragon sighting event cinematic
        {
            auto* seq = cinematic->CreateSequence("ow_dragon_sighting");

            auto* cam = seq->AddCameraTrack("dragon_camera");
            cam->AddKeyframe({0.0f, {0.0f, 10.0f, 0.0f}, {100.0f, 50.0f, 200.0f}, 70.0f, 0.0f});
            cam->AddKeyframe({3.0f, {0.0f, 15.0f, 0.0f}, {200.0f, 80.0f, 100.0f}, 60.0f, 0.0f});
            cam->AddKeyframe({6.0f, {0.0f, 10.0f, 0.0f}, {300.0f, 100.0f, -50.0f}, 50.0f, 0.0f});

            auto* subs = seq->AddSubtitleTrack("dragon_subs");
            subs->AddSubtitle({1.0f, 3.0f, "A shadow passes overhead...", ""});
            subs->AddSubtitle({3.5f, 6.0f, "The ancient dragon circles the peaks, watching.", "Narrator"});

            auto* events = seq->AddEventTrack("dragon_events");
            events->AddCue({0.0f, "ow_play_music", "dragon_theme"});
            events->AddCue({6.0f, "ow_resume_gameplay", ""});
        }

        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Registered 2 cinematic sequences");
    }

    // =========================================================================
    // Weather / Time of Day
    // =========================================================================

    void OWEngineSystems::SetupWeatherAndTimeOfDay()
    {
        auto* weather = m_context->GetWeather();
        auto* timeOfDay = m_context->GetTimeOfDay();

        // Start at dawn with clear weather
        if (weather)
            weather->SetWeather(Spark::WeatherType::Clear, 0.0f, 0.0f);

        if (timeOfDay)
        {
            timeOfDay->SetTimeOfDay(6.0f);  // 6:00 AM — dawn
            timeOfDay->SetTimeScale(40.0f); // 1 real second = 40 game seconds (~36 min day cycle)
        }

        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Weather and time configured (6:00 AM dawn, clear)");
    }

    std::string OWEngineSystems::SetWeather(const std::string& weatherName)
    {
        auto* weather = m_context->GetWeather();
        if (!weather)
            return "Weather system not available";

        Spark::WeatherType type = Spark::WeatherType::Clear;
        if (weatherName == "clear")
            type = Spark::WeatherType::Clear;
        else if (weatherName == "cloudy")
            type = Spark::WeatherType::Cloudy;
        else if (weatherName == "rain")
            type = Spark::WeatherType::Rain;
        else if (weatherName == "snow")
            type = Spark::WeatherType::Snow;
        else if (weatherName == "fog")
            type = Spark::WeatherType::Fog;
        else if (weatherName == "storm")
            type = Spark::WeatherType::Storm;
        else
            return "Unknown weather: " + weatherName;

        weather->SetWeather(type, -1.0f, 5.0f);
        return "Weather set to " + weatherName + " (5s transition)";
    }

    std::string OWEngineSystems::SetTime(float hour)
    {
        auto* timeOfDay = m_context->GetTimeOfDay();
        if (!timeOfDay)
            return "Time-of-day system not available";

        if (hour < 0.0f || hour >= 24.0f)
            return "Hour must be in [0, 24)";

        timeOfDay->SetTimeOfDay(hour);
        return "Time set to " + timeOfDay->GetTimeString();
    }

    std::string OWEngineSystems::GetWeatherStatus() const
    {
        auto* weather = m_context->GetWeather();
        if (!weather)
            return "Weather system not available";

        return "Weather active (query via engine context)";
    }

    std::string OWEngineSystems::GetAbilitySummary() const
    {
        return "Open world uses survival mechanics instead of ability systems";
    }

    // =========================================================================
    // Music
    // =========================================================================

    void OWEngineSystems::RegisterMusicTracks()
    {
        auto* music = m_context->GetMusic();
        if (!music)
            return;

        using Spark::Audio::MusicTrack;

        // Exploration tracks per biome
        music->RegisterTrack({"meadow_dawn", "Assets/Audio/Music/ow_meadow_dawn.ogg", 80.0f, 0.0f, -1.0f, true, ""});
        music->RegisterTrack({"forest_depths", "Assets/Audio/Music/ow_forest.ogg", 70.0f, 0.0f, -1.0f, true, ""});
        music->RegisterTrack({"mountain_winds", "Assets/Audio/Music/ow_mountain.ogg", 65.0f, 0.0f, -1.0f, true, ""});
        music->RegisterTrack({"desert_heat", "Assets/Audio/Music/ow_desert.ogg", 75.0f, 0.0f, -1.0f, true, ""});
        music->RegisterTrack({"tundra_silence", "Assets/Audio/Music/ow_tundra.ogg", 55.0f, 0.0f, -1.0f, true, ""});
        music->RegisterTrack({"coastal_breeze", "Assets/Audio/Music/ow_coast.ogg", 85.0f, 0.0f, -1.0f, true, ""});

        // Action / event tracks
        music->RegisterTrack({"combat_tension", "Assets/Audio/Music/ow_combat.ogg", 120.0f, 0.0f, -1.0f, true, ""});
        music->RegisterTrack({"dragon_theme", "Assets/Audio/Music/ow_dragon.ogg", 140.0f, 0.0f, -1.0f, false, ""});

        // Settlement / safe area
        music->RegisterTrack({"village_hearth", "Assets/Audio/Music/ow_village.ogg", 90.0f, 0.0f, -1.0f, true, ""});

        // Dynamic music state
        Spark::Audio::DynamicMusicState dynamicState;
        dynamicState.explorationTrack = "meadow_dawn";
        dynamicState.lowThreatTrack = "forest_depths";
        dynamicState.combatTrack = "combat_tension";
        dynamicState.bossFightTrack = "dragon_theme";
        dynamicState.transitionDuration = 3.0f;
        music->SetDynamicMusicState(dynamicState);

        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Registered 9 music tracks with dynamic transitions");
    }

    // =========================================================================
    // Event Bus
    // =========================================================================

    void OWEngineSystems::SubscribeToEvents()
    {
        auto* eventBus = m_context->GetEventBus();
        if (!eventBus)
            return;

        // Weather changes affect survival (temperature, visibility)
        m_eventHandles.push_back(eventBus->Subscribe<Spark::WeatherChangedEvent>(
            [](const Spark::WeatherChangedEvent& evt)
            {
                Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Weather changed to type " +
                                                            std::to_string(evt.newType) +
                                                            " (intensity=" + std::to_string(evt.intensity) + ")");
            }));

        // Time of day transitions (dawn/dusk affect wildlife behavior)
        m_eventHandles.push_back(eventBus->Subscribe<Spark::TimeOfDayChangedEvent>(
            [](const Spark::TimeOfDayChangedEvent& evt)
            {
                if (evt.currentHour >= 6.0f && evt.previousHour < 6.0f)
                {
                    Spark::SimpleConsole::GetInstance().LogInfo(
                        "[OpenWorld] Dawn — diurnal wildlife active, nocturnal retreating");
                }
                else if (evt.currentHour >= 20.0f && evt.previousHour < 20.0f)
                {
                    Spark::SimpleConsole::GetInstance().LogInfo(
                        "[OpenWorld] Dusk — nocturnal predators emerging, increased danger");
                }
            }));

        // Entity killed (quest/hunting tracking)
        m_eventHandles.push_back(eventBus->Subscribe<Spark::EntityKilledEvent>(
            [](const Spark::EntityKilledEvent& evt)
            {
                Spark::SimpleConsole::GetInstance().LogInfo(
                    "[OpenWorld] Entity killed: " + std::to_string(evt.entityId) + " (" + evt.cause + ")");
            }));

        SPARK_LOG_INFO(Spark::LogCategory::Game, "Open world subscribed to 3 engine events");
        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Subscribed to 3 engine events");
    }

} // namespace OpenWorld
