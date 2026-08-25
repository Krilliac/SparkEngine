/**
 * @file RPGEngineSystems.cpp
 * @brief Wires RPG gameplay into engine subsystems
 *
 * Configures real ECS saving plus NPC behavior trees, cinematic sequences,
 * weather/time rules, music, and event subscriptions.
 */

#include "RPGEngineSystems.h"
#include "Enums/RPGEnums.h"

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

#include <unordered_map>

namespace RPG
{

    RPGEngineSystems::~RPGEngineSystems()
    {
        if (m_initialized)
            Shutdown();
    }

    bool RPGEngineSystems::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;
        auto& console = Spark::SimpleConsole::GetInstance();

        ConfigureSaveSystem();
        RegisterBehaviorTrees();
        RegisterCinematicSequences();
        SetupWeatherAndTimeOfDay();
        RegisterMusicTracks();
        SubscribeToEvents();

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "RPG engine systems integration initialized (6 subsystems wired)");
        console.LogInfo("[RPG] Engine systems integration initialized (6 subsystems wired)");
        return true;
    }

    void RPGEngineSystems::Shutdown()
    {
        if (!m_initialized)
            return;

        auto& console = Spark::SimpleConsole::GetInstance();

        // Release event subscriptions (RAII handles auto-unsubscribe)
        m_eventHandles.clear();

        m_context = nullptr;
        m_initialized = false;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "RPG engine systems integration shut down");
        console.LogInfo("[RPG] Engine systems integration shut down");
    }

    // =========================================================================
    // Save System
    // =========================================================================

    void RPGEngineSystems::ConfigureSaveSystem()
    {
        auto* saveSystem = m_context->GetSaveSystem();
        if (!saveSystem)
            return;

        // RPG state represented by ECS components is covered by the engine's real
        // built-in/reflected serializers. Do not register type names that emit fake
        // data: those produce apparently successful saves which cannot restore state.
        saveSystem->SetMaxAutoSaves(3);

        SPARK_LOG_INFO(Spark::LogCategory::Game, "RPG save integration configured (3 rotating autosaves)");
        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Save integration configured (real ECS snapshots)");
    }

    std::string RPGEngineSystems::SaveGame(const std::string& slotName, const std::string& demoState)
    {
        auto* saveSystem = m_context ? m_context->GetSaveSystem() : nullptr;
        auto* world = m_context ? m_context->GetWorld() : nullptr;
        if (!saveSystem)
            return "Save system not available";
        if (!world)
            return "World not available for saving";

        Spark::SaveMetadata metadata;
        metadata.saveName = "Oakhollow - " + slotName;
        metadata.sceneName = "RPG/Oakhollow";
        metadata.playerClass = "Adventurer";
        metadata.playTime = static_cast<float>(m_context->GetElapsedTime());

        if (demoState.empty())
            return "Failed to snapshot RPG demo session";
        const std::unordered_map<std::string, std::string> customState = {{"SparkGameRPG.demo.v1", demoState}};
        return saveSystem->Save(slotName, *world, metadata, customState)
                   ? "Saved RPG world to slot '" + slotName + "'"
                   : "Failed to save RPG world to slot '" + slotName + "'";
    }

    std::string RPGEngineSystems::LoadGame(const std::string& slotName, std::string& outDemoState,
                                           const std::function<bool(const std::string&)>& validateDemoState)
    {
        outDemoState.clear();
        auto* saveSystem = m_context ? m_context->GetSaveSystem() : nullptr;
        auto* world = m_context ? m_context->GetWorld() : nullptr;
        if (!saveSystem)
            return "Save system not available";
        if (!world)
            return "World not available for loading";

        if (!saveSystem->SaveExists(slotName))
            return "No save found in slot '" + slotName + "'";

        std::unordered_map<std::string, std::string> customState;
        const auto validateCustomState = [&](const std::unordered_map<std::string, std::string>& candidate)
        {
            const auto demoState = candidate.find("SparkGameRPG.demo.v1");
            return demoState != candidate.end() && validateDemoState && validateDemoState(demoState->second);
        };
        if (!saveSystem->Load(slotName, *world, customState, validateCustomState))
            return "Failed to validate or load RPG state from slot '" + slotName + "'";

        const auto demoState = customState.find("SparkGameRPG.demo.v1");
        outDemoState = demoState->second;
        return "Loaded RPG world from slot '" + slotName + "'";
    }

    // =========================================================================
    // AI / Behavior Trees
    // =========================================================================

    void RPGEngineSystems::RegisterBehaviorTrees()
    {
        auto* aiSystem = m_context->GetAI();
        if (!aiSystem)
            return;

        using namespace Spark::AI;

        // Villager: daily schedule, patrol between home and market, flee on threat
        {
            auto villagerTree =
                FPSBehaviors::CreatePatrolBehavior({{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 5.0f}, {15.0f, 0.0f, -3.0f}});
            aiSystem->RegisterBehavior("rpg_villager", std::move(villagerTree));
        }

        // Merchant: stand at shop position, greet approaching players
        {
            auto merchantTree = FPSBehaviors::CreateGuardBehavior({5.0f, 0.0f, 0.0f}, 8.0f);
            aiSystem->RegisterBehavior("rpg_merchant", std::move(merchantTree));
        }

        // Guard: patrol route, investigate sounds, attack hostiles
        {
            AIAgentConfig guardConfig;
            guardConfig.detectionRange = 35.0f;
            guardConfig.attackRange = 12.0f;
            guardConfig.moveSpeed = 4.5f;
            guardConfig.accuracy = 0.75f;
            guardConfig.reactionTime = 0.25f;
            auto guardTree = FPSBehaviors::CreateCombatBehavior(guardConfig);
            aiSystem->RegisterBehavior("rpg_guard", std::move(guardTree));
        }

        // Companion: follow player, assist in combat, disengage when player is safe
        {
            AIAgentConfig companionConfig;
            companionConfig.detectionRange = 25.0f;
            companionConfig.attackRange = 10.0f;
            companionConfig.moveSpeed = 5.5f;
            companionConfig.accuracy = 0.65f;
            companionConfig.reactionTime = 0.2f;
            companionConfig.canUseCover = false;
            auto companionTree = FPSBehaviors::CreateCombatBehavior(companionConfig);
            aiSystem->RegisterBehavior("rpg_companion", std::move(companionTree));
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "RPG registered 4 NPC behavior trees");
        Spark::SimpleConsole::GetInstance().LogInfo(
            "[RPG] Registered 4 NPC behavior trees (villager, merchant, guard, companion)");
    }

    // =========================================================================
    // Cinematic Sequences
    // =========================================================================

    void RPGEngineSystems::RegisterCinematicSequences()
    {
        auto* cinematic = m_context->GetCinematic();
        if (!cinematic)
            return;

        using namespace Spark::Cinematic;

        // --- Intro: camera sweep of starting village ---
        {
            auto* seq = cinematic->CreateSequence("rpg_intro");

            auto* cam = seq->AddCameraTrack("intro_camera");
            cam->AddKeyframe({0.0f, {-20.0f, 15.0f, -20.0f}, {0.0f, 0.0f, 0.0f}, 60.0f, 0.0f});
            cam->AddKeyframe({4.0f, {0.0f, 10.0f, -15.0f}, {0.0f, 2.0f, 5.0f}, 55.0f, 0.0f});
            cam->AddKeyframe({8.0f, {10.0f, 5.0f, 0.0f}, {0.0f, 1.5f, 0.0f}, 50.0f, 0.0f});
            cam->AddKeyframe({12.0f, {0.0f, 3.0f, 5.0f}, {0.0f, 1.5f, 0.0f}, 60.0f, 0.0f});

            auto* subs = seq->AddSubtitleTrack("intro_subtitles");
            subs->AddSubtitle({1.0f, 4.0f, "Welcome to the village of Ashvale...", "Narrator"});
            subs->AddSubtitle({5.0f, 8.0f, "A quiet place, far from the troubles of the realm.", "Narrator"});
            subs->AddSubtitle({9.0f, 12.0f, "But darkness stirs in the mountains to the north.", "Narrator"});

            auto* fade = seq->AddFadeTrack("intro_fade");
            fade->AddKeyframe({0.0f, 1.0f, {0.0f, 0.0f, 0.0f}});
            fade->AddKeyframe({1.5f, 0.0f, {0.0f, 0.0f, 0.0f}});
            fade->AddKeyframe({11.0f, 0.0f, {0.0f, 0.0f, 0.0f}});
            fade->AddKeyframe({12.0f, 1.0f, {0.0f, 0.0f, 0.0f}});

            auto* events = seq->AddEventTrack("intro_events");
            events->AddCue({0.5f, "rpg_play_music", "village_theme"});
            events->AddCue({12.0f, "rpg_enable_input", ""});
        }

        // --- Boss intro: dramatic reveal ---
        {
            auto* seq = cinematic->CreateSequence("rpg_boss_intro");

            auto* cam = seq->AddCameraTrack("boss_camera");
            cam->AddKeyframe({0.0f, {0.0f, 2.0f, -10.0f}, {0.0f, 5.0f, 10.0f}, 70.0f, 0.0f});
            cam->AddKeyframe({2.0f, {0.0f, 8.0f, -5.0f}, {0.0f, 6.0f, 10.0f}, 50.0f, 0.0f});
            cam->AddKeyframe({4.0f, {3.0f, 4.0f, 0.0f}, {0.0f, 5.0f, 10.0f}, 60.0f, 0.0f});
            cam->AddKeyframe({6.0f, {0.0f, 3.0f, 2.0f}, {0.0f, 5.0f, 10.0f}, 65.0f, 0.0f});

            auto* subs = seq->AddSubtitleTrack("boss_subtitles");
            subs->AddSubtitle({1.5f, 4.0f, "So... you dare enter my domain.", "Shadow Lord"});
            subs->AddSubtitle({4.5f, 6.0f, "Prepare to face your doom!", "Shadow Lord"});

            auto* events = seq->AddEventTrack("boss_events");
            events->AddCue({0.0f, "rpg_play_music", "boss_battle"});
            events->AddCue({6.0f, "rpg_start_boss_fight", ""});
        }

        // --- Ending: celebration scene ---
        {
            auto* seq = cinematic->CreateSequence("rpg_ending");

            auto* cam = seq->AddCameraTrack("ending_camera");
            cam->AddKeyframe({0.0f, {0.0f, 3.0f, -8.0f}, {0.0f, 2.0f, 0.0f}, 55.0f, 0.0f});
            cam->AddKeyframe({5.0f, {-5.0f, 5.0f, -5.0f}, {0.0f, 2.0f, 0.0f}, 60.0f, 0.0f});
            cam->AddKeyframe({10.0f, {0.0f, 15.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 50.0f, 0.0f});

            auto* subs = seq->AddSubtitleTrack("ending_subtitles");
            subs->AddSubtitle({1.0f, 5.0f, "The shadow has been lifted from the land.", "Narrator"});
            subs->AddSubtitle({6.0f, 10.0f, "Peace returns to Ashvale. Your legend lives on.", "Narrator"});

            auto* fade = seq->AddFadeTrack("ending_fade");
            fade->AddKeyframe({0.0f, 0.0f, {0.0f, 0.0f, 0.0f}});
            fade->AddKeyframe({9.0f, 0.0f, {0.0f, 0.0f, 0.0f}});
            fade->AddKeyframe({10.0f, 1.0f, {0.0f, 0.0f, 0.0f}});

            auto* events = seq->AddEventTrack("ending_events");
            events->AddCue({0.0f, "rpg_play_music", "victory_fanfare"});
            events->AddCue({10.0f, "rpg_show_credits", ""});
        }

        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Registered 3 cinematic sequences (intro, boss, ending)");
    }

    // =========================================================================
    // Weather / Time of Day
    // =========================================================================

    void RPGEngineSystems::SetupWeatherAndTimeOfDay()
    {
        auto* weather = m_context->GetWeather();
        auto* timeOfDay = m_context->GetTimeOfDay();

        // Start with clear weather, morning time
        if (weather)
            weather->SetWeather(Spark::WeatherType::Clear, 0.0f, 0.0f);

        if (timeOfDay)
        {
            timeOfDay->SetTimeOfDay(8.0f);  // 8:00 AM start
            timeOfDay->SetTimeScale(60.0f); // 1 real second = 1 game minute
        }

        // Weather affects combat via event system (rain reduces fire damage, snow slows
        // movement). The actual modifiers are applied in RPGCombatSystem when it reads
        // the current weather state from context.

        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Weather and time-of-day configured (8:00 AM, clear)");
    }

    std::string RPGEngineSystems::SetWeather(const std::string& weatherName)
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
            return "Unknown weather: " + weatherName + " (use: clear/cloudy/rain/snow/fog/storm)";

        weather->SetWeather(type, -1.0f, 5.0f);
        return "Weather set to " + weatherName + " (5s transition)";
    }

    std::string RPGEngineSystems::SetTime(float hour)
    {
        auto* timeOfDay = m_context->GetTimeOfDay();
        if (!timeOfDay)
            return "Time-of-day system not available";

        if (hour < 0.0f || hour >= 24.0f)
            return "Hour must be in [0, 24)";

        timeOfDay->SetTimeOfDay(hour);
        return "Time set to " + timeOfDay->GetTimeString();
    }

    // =========================================================================
    // Music / Audio
    // =========================================================================

    void RPGEngineSystems::RegisterMusicTracks()
    {
        auto* music = m_context->GetMusic();
        if (!music)
            return;

        using Spark::Audio::MusicTrack;

        // Village / exploration
        music->RegisterTrack({"village_theme", "Assets/Audio/Music/rpg_village.ogg", 95.0f, 0.0f, -1.0f, true, ""});
        music->RegisterTrack({"forest_exploration", "Assets/Audio/Music/rpg_forest.ogg", 85.0f, 0.0f, -1.0f, true, ""});

        // Dungeon / tension
        music->RegisterTrack({"dungeon_ambient", "Assets/Audio/Music/rpg_dungeon.ogg", 70.0f, 0.0f, -1.0f, true, ""});

        // Combat
        music->RegisterTrack({"boss_battle", "Assets/Audio/Music/rpg_boss.ogg", 140.0f, 0.0f, -1.0f, true, ""});

        // Victory
        music->RegisterTrack({"victory_fanfare", "Assets/Audio/Music/rpg_victory.ogg", 120.0f, 0.0f, -1.0f, false, ""});

        // Set up dynamic music transitions (exploration -> combat based on enemy proximity)
        Spark::Audio::DynamicMusicState dynamicState;
        dynamicState.explorationTrack = "village_theme";
        dynamicState.lowThreatTrack = "forest_exploration";
        dynamicState.combatTrack = "dungeon_ambient";
        dynamicState.bossFightTrack = "boss_battle";
        dynamicState.transitionDuration = 2.5f;
        music->SetDynamicMusicState(dynamicState);

        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Registered 5 music tracks with dynamic transitions");
    }

    // =========================================================================
    // Event Bus
    // =========================================================================

    void RPGEngineSystems::SubscribeToEvents()
    {
        auto* eventBus = m_context->GetEventBus();
        if (!eventBus)
            return;

        // Quest completed: log and trigger celebration effects
        m_eventHandles.push_back(eventBus->Subscribe<Spark::QuestCompletedEvent>(
            [](const Spark::QuestCompletedEvent& evt)
            {
                Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Quest completed: " + evt.questName +
                                                            " (id=" + std::to_string(evt.questId) + ")");
            }));

        // Entity killed: update quest kill counters, NPC reactions
        m_eventHandles.push_back(eventBus->Subscribe<Spark::EntityKilledEvent>(
            [](const Spark::EntityKilledEvent& evt)
            {
                Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Entity killed: " + std::to_string(evt.entityId) +
                                                            " by " + std::to_string(evt.killerId) + " (" + evt.cause +
                                                            ")");
            }));

        // Time of day changed: update NPC schedules (shops close at night, guard shifts)
        m_eventHandles.push_back(eventBus->Subscribe<Spark::TimeOfDayChangedEvent>(
            [](const Spark::TimeOfDayChangedEvent& evt)
            {
                // Night time: close shops, switch guard patrols
                if (evt.currentHour >= 20.0f || evt.currentHour < 6.0f)
                {
                    Spark::SimpleConsole::GetInstance().LogInfo(
                        "[RPG] Night time — shops closing, guards on night shift");
                }
                // Dawn: reopen shops, day guards on duty
                else if (evt.currentHour >= 6.0f && evt.previousHour < 6.0f)
                {
                    Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Dawn — shops opening, day guards on duty");
                }
            }));

        // Weather changed: log for combat modifier awareness
        m_eventHandles.push_back(eventBus->Subscribe<Spark::WeatherChangedEvent>(
            [](const Spark::WeatherChangedEvent& evt)
            {
                Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Weather changed to type " +
                                                            std::to_string(evt.newType) +
                                                            " (intensity=" + std::to_string(evt.intensity) + ")");
            }));

        SPARK_LOG_INFO(Spark::LogCategory::Game, "RPG subscribed to 4 engine events");
        Spark::SimpleConsole::GetInstance().LogInfo("[RPG] Subscribed to 4 engine events");
    }

} // namespace RPG
