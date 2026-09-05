/**
 * @file GameEngineSystems.cpp
 * @brief Engine system integration — wires audio, weather, destruction,
 *        dialogue, save, and coroutine systems into the game loop
 *
 * Uses IEngineContext (SDK v2) for subsystem access instead of singletons.
 * Extracted from Game.cpp to keep engine-integration code separate from
 * core gameplay logic.
 */

#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif
#include <cstdint>
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif

#include "Game.h"
#include "Player.h"
#include "FPSAssetPaths.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

// Engine systems
#include "Audio/AudioEngine.h"
#include "Audio/MusicManager.h"
#include "Graphics/WeatherSystem.h"
#include "Engine/Destruction/DestructionSystem.h"
#include "Engine/Dialogue/DialogueSystem.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Engine/Coroutine/CoroutineScheduler.h"
#include "Engine/Cinematic/Sequencer.h"
#include "Engine/Replay/ReplaySystem.h"

using namespace DirectX;

void Game::SetEngineContext(Spark::IEngineContext* context)
{
    if (m_engineContext == context && (!context || m_engineSystemsInitialized))
    {
        return;
    }

    m_engineContext = context;
    if (m_engineContext && !m_engineSystemsInitialized)
    {
        InitializeEngineSystems();
    }
}

/*-------------------------------------------------------------
  Initialize engine system connections from the game side
--------------------------------------------------------------*/
void Game::InitializeEngineSystems()
{
    if (m_engineSystemsInitialized)
    {
        return;
    }

    SPARK_LOG_INFO(Spark::LogCategory::Game, "Initializing engine system connections");
    if (!m_engineContext)
    {
        SPARK_LOG_WARN(Spark::LogCategory::Game, "EngineContext not available — skipping engine system wiring");
        LOG_TO_CONSOLE_IMMEDIATE(L"EngineContext not available - skipping engine system wiring", L"WARNING");
        return;
    }

    // ---- Audio --------------------------------------------------------
    if (auto* music = m_engineContext->GetMusic())
    {
        // Register game music tracks for the combat arena
        Spark::Audio::MusicTrack explorationTrack;
        explorationTrack.name = "arena_ambient";
        explorationTrack.filepath = Spark::FPSAssets::ResolveUtf8("Audio/ambient_wind.wav");
        explorationTrack.bpm = 90.0f;
        explorationTrack.loop = true;
        music->RegisterTrack(explorationTrack);

        Spark::Audio::MusicTrack combatTrack;
        combatTrack.name = "arena_combat";
        combatTrack.filepath = Spark::FPSAssets::ResolveUtf8("Audio/music_combat.wav");
        combatTrack.bpm = 140.0f;
        combatTrack.loop = true;
        music->RegisterTrack(combatTrack);

        // Set dynamic music states: exploration → combat as enemies engage
        Spark::Audio::DynamicMusicState dynamicState;
        dynamicState.explorationTrack = "arena_ambient";
        dynamicState.combatTrack = "arena_combat";
        dynamicState.transitionDuration = 2.0f;
        music->SetDynamicMusicState(dynamicState);

        // Start with exploration music
        music->Play("arena_ambient", 1.0f);

        m_audioInitialized = true;
        LOG_TO_CONSOLE_IMMEDIATE(L"Audio: game music tracks registered, ambient playing", L"SUCCESS");
    }

    // ---- Weather ------------------------------------------------------
    if (auto* weather = m_engineContext->GetWeather())
    {
        weather->SetWeather(Spark::WeatherType::Clear, 1.0f, 0.0f);
        m_weatherActive = true;
        LOG_TO_CONSOLE_IMMEDIATE(L"Weather: clear skies set for arena", L"SUCCESS");
    }

    // ---- Destruction --------------------------------------------------
    if (auto* destruction = m_engineContext->GetDestruction())
    {
        Spark::FracturePattern cratePattern;
        Spark::FracturePiece topPlank;
        topPlank.name = "top_plank";
        topPlank.meshName = "crate_top";
        topPlank.localOffset = {0.0f, 0.5f, 0.0f};
        topPlank.mass = 1.0f;
        topPlank.lifetime = 4.0f;
        topPlank.scatterForce = 8.0f;
        cratePattern.AddPiece(topPlank);

        Spark::FracturePiece side1;
        side1.name = "side_1";
        side1.meshName = "crate_side";
        side1.localOffset = {0.5f, 0.0f, 0.0f};
        side1.mass = 0.8f;
        side1.lifetime = 4.0f;
        side1.scatterForce = 6.0f;
        cratePattern.AddPiece(side1);

        cratePattern.SetDestructionSound("sfx_wood_break");
        cratePattern.SetParticleEffect("vfx_splinters");
        destruction->RegisterPattern("wooden_crate", cratePattern);

        Spark::FracturePattern barrelPattern;
        Spark::FracturePiece barrelTop;
        barrelTop.name = "barrel_top";
        barrelTop.meshName = "barrel_lid";
        barrelTop.localOffset = {0.0f, 0.6f, 0.0f};
        barrelTop.mass = 0.5f;
        barrelTop.lifetime = 3.0f;
        barrelTop.scatterForce = 12.0f;
        barrelPattern.AddPiece(barrelTop);

        Spark::FracturePiece barrelBody;
        barrelBody.name = "barrel_body";
        barrelBody.meshName = "barrel_shell";
        barrelBody.localOffset = {0.0f, 0.0f, 0.0f};
        barrelBody.mass = 2.0f;
        barrelBody.lifetime = 5.0f;
        barrelBody.scatterForce = 5.0f;
        barrelPattern.AddPiece(barrelBody);

        barrelPattern.SetDestructionSound("sfx_metal_break");
        barrelPattern.SetParticleEffect("vfx_sparks");
        destruction->RegisterPattern("metal_barrel", barrelPattern);

        LOG_TO_CONSOLE_IMMEDIATE(L"Destruction: 2 fracture patterns registered (crate, barrel)", L"SUCCESS");
    }

    // ---- Dialogue -----------------------------------------------------
    if (auto* dialogue = m_engineContext->GetDialogue())
    {
        auto vendorTree = std::make_unique<Spark::DialogueTree>();
        vendorTree->SetId("arena_vendor");
        vendorTree->SetStartNodeId("greeting");

        Spark::DialogueNode greeting;
        greeting.id = "greeting";
        greeting.type = Spark::DialogueNodeType::Choice;
        greeting.speakerName = "Vendor";
        greeting.text = "Welcome to the arena! Need supplies?";

        Spark::DialogueChoice healthChoice;
        healthChoice.text = "Buy health potions";
        healthChoice.nextNodeId = "health_reply";
        greeting.choices.push_back(healthChoice);

        Spark::DialogueChoice ammoChoice;
        ammoChoice.text = "Buy ammo";
        ammoChoice.nextNodeId = "ammo_reply";
        greeting.choices.push_back(ammoChoice);

        Spark::DialogueChoice noChoice;
        noChoice.text = "No thanks";
        noChoice.nextNodeId = "";
        greeting.choices.push_back(noChoice);

        vendorTree->AddNode(greeting);

        Spark::DialogueNode healthReply;
        healthReply.id = "health_reply";
        healthReply.type = Spark::DialogueNodeType::Text;
        healthReply.speakerName = "Vendor";
        healthReply.text = "Here you go - stay alive out there!";
        healthReply.eventName = "give_health_potion";
        vendorTree->AddNode(healthReply);

        Spark::DialogueNode ammoReply;
        ammoReply.id = "ammo_reply";
        ammoReply.type = Spark::DialogueNodeType::Text;
        ammoReply.speakerName = "Vendor";
        ammoReply.text = "Locked and loaded. Good hunting!";
        ammoReply.eventName = "give_ammo_pack";
        vendorTree->AddNode(ammoReply);

        dialogue->RegisterTree("arena_vendor", std::move(vendorTree));
        LOG_TO_CONSOLE_IMMEDIATE(L"Dialogue: arena vendor dialogue tree registered", L"SUCCESS");
    }

    // ---- Save System --------------------------------------------------
    // Quicksave needs both a SaveSystem service and a World to serialise; claim
    // readiness only when the engine context actually supplies them.
    m_saveSystemReady = (m_engineContext->GetSaveSystem() != nullptr) && (m_engineContext->GetWorld() != nullptr);
    if (m_saveSystemReady)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Save system: ready for quicksave/quickload", L"SUCCESS");
    }
    else
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Save system: unavailable - quicksave/quickload disabled", L"WARNING");
    }

    // ---- Coroutine Scheduler ------------------------------------------
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Coroutine scheduler: game coroutines available", L"SUCCESS");
    }

    // ---- Cinematic Sequencer ------------------------------------------
    if (auto* cinematic = m_engineContext->GetCinematic())
    {
        auto* intro = cinematic->CreateSequence("arena_intro");

        // Camera track: dolly from above to player start position
        auto* camTrack = intro->AddCameraTrack("MainCamera");
        camTrack->AddKeyframe({0.0f,
                               {0.0f, 20.0f, -30.0f},
                               {0.0f, 0.0f, 0.0f},
                               60.0f,
                               0.0f,
                               Spark::Cinematic::InterpolationMode::CubicBezier});
        camTrack->AddKeyframe({3.0f,
                               {0.0f, 5.0f, -10.0f},
                               {0.0f, 2.0f, 0.0f},
                               55.0f,
                               0.0f,
                               Spark::Cinematic::InterpolationMode::CubicBezier});
        camTrack->AddKeyframe(
            {5.0f, {0.0f, 2.0f, -5.0f}, {0.0f, 1.0f, 5.0f}, 60.0f, 0.0f, Spark::Cinematic::InterpolationMode::Linear});

        // Subtitle track
        auto* subTrack = intro->AddSubtitleTrack("Narration");
        subTrack->AddSubtitle({0.5f, 3.0f, "Welcome to the Spark Arena.", "Announcer", {1, 1, 1, 1}});
        subTrack->AddSubtitle({3.5f, 5.5f, "Prepare for combat.", "Announcer", {1, 0.8f, 0.2f, 1}});

        // Fade track: fade from black
        auto* fadeTrack = intro->AddFadeTrack("ScreenFade");
        fadeTrack->AddKeyframe({0.0f, 1.0f, {0, 0, 0}, Spark::Cinematic::InterpolationMode::Linear});
        fadeTrack->AddKeyframe({1.5f, 0.0f, {0, 0, 0}, Spark::Cinematic::InterpolationMode::Linear});

        // Event track: enable controls after intro
        auto* eventTrack = intro->AddEventTrack("GameEvents");
        eventTrack->AddCue({5.0f, "enable_player_control", ""});

        LOG_TO_CONSOLE_IMMEDIATE(L"Cinematic: arena_intro sequence registered (5s, 4 tracks)", L"SUCCESS");
    }

    // ---- Replay System ------------------------------------------------
    if (auto* replay = m_engineContext->GetReplay())
    {
        replay->SetRecordInterval(1.0f / 20.0f); // 20 fps recording
        replay->SetMetadata("combat_arena", "freeplay");
        LOG_TO_CONSOLE_IMMEDIATE(L"Replay: system configured (20fps, combat_arena)", L"SUCCESS");
    }

    m_engineSystemsInitialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game, "All engine systems wired into game");
    LOG_TO_CONSOLE_IMMEDIATE(L"All engine systems wired into game", L"SUCCESS");
}

// ============================================================================
// LOCAL PROFILE PERSISTENCE
// ============================================================================

Spark::FPSLocalProfile Game::CaptureLocalProfile() const
{
    Spark::FPSLocalProfile profile;
    profile.playTimeSeconds = m_playTime;

    if (m_progression)
    {
        profile.progressionLevel = m_progression->GetLevel();
        profile.progressionXP = m_progression->GetCurrentXP();
    }
    if (m_player)
    {
        profile.playerClass = static_cast<int>(m_player->GetClass());
        profile.weapon = static_cast<int>(m_player->GetCurrentWeaponType());
        profile.health = m_player->GetHealth();
        profile.armor = m_player->GetArmor();
    }
    if (m_gameMode)
    {
        if (const auto* score = m_gameMode->GetPlayerScore("Player1"))
        {
            profile.kills = score->kills;
            profile.deaths = score->deaths;
            profile.score = score->totalScore;
        }
    }
    return profile;
}

void Game::ApplyLocalProfile(const Spark::FPSLocalProfile& profile)
{
    m_playTime = profile.playTimeSeconds;

    if (m_progression)
        m_progression->RestoreProgress(profile.progressionXP);

    if (m_player)
    {
        m_player->SetClass(static_cast<PlayerClass>(profile.playerClass), m_classSystem.get());
        m_player->Console_ChangeWeapon(static_cast<WeaponType>(profile.weapon));
        m_player->Console_SetHealth(profile.health);
        m_player->Console_SetArmor(profile.armor);
        m_player->SetActive(profile.health > 0.0f);
    }
    if (m_hudSystem)
        m_hudSystem->SetCurrentClass(static_cast<PlayerClass>(profile.playerClass));

    // The scoreboard lives outside the ECS, so a loaded save has to put it back
    // explicitly. Capturing kills/deaths/score and then not restoring them is what made
    // a quickload silently reset the match score.
    if (m_gameMode)
        m_gameMode->RestorePlayerScore("Player1", profile.kills, profile.deaths, profile.score);

    // A profile captured while dead restores an inactive player. Player::Update()
    // early-returns while dead and only a PlayerRespawnEvent revives it, so the respawn
    // countdown has to be re-armed here or the restored session can never progress.
    if (m_respawnSystem && profile.health <= 0.0f && !m_respawnSystem->IsWaitingForRespawn())
    {
        m_respawnSystem->ArmRespawn();
        LOG_TO_CONSOLE_IMMEDIATE(L"Restored profile was captured while dead - respawn countdown re-armed", L"WARNING");
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"Local profile applied from save", L"SUCCESS");
}

bool Game::QuickSaveProfile(std::string& outMessage)
{
    if (!m_engineContext)
    {
        outMessage = "Save system unavailable: no engine context";
        return false;
    }

    auto* saveSystem = m_engineContext->GetSaveSystem();
    World* world = m_engineContext->GetWorld();
    if (!saveSystem || !world)
    {
        outMessage = "Save system unavailable: engine exposes no save system or world";
        return false;
    }

    Spark::SaveMetadata metadata;
    metadata.saveName = "Quick Save";
    metadata.sceneName = "combat_arena";
    metadata.playTime = m_playTime;
    if (m_player)
    {
        metadata.playerHealth = m_player->GetHealth();
        metadata.playerArmor = m_player->GetArmor();
        metadata.playerPosition = m_player->GetPosition();
    }

    const Spark::FPSLocalProfile profile = CaptureLocalProfile();
    metadata.playerKills = profile.kills;
    metadata.playerDeaths = profile.deaths;

    std::unordered_map<std::string, std::string> customState;
    profile.WriteTo(customState);

    if (!saveSystem->Save(kQuickSaveSlot, *world, metadata, customState))
    {
        outMessage = std::string("Quick save FAILED to write slot '") + kQuickSaveSlot + "'";
        return false;
    }

    outMessage = std::string("Quick save written to slot '") + kQuickSaveSlot + "'";
    return true;
}

bool Game::QuickLoadProfile(std::string& outMessage)
{
    if (!m_engineContext)
    {
        outMessage = "Save system unavailable: no engine context";
        return false;
    }

    auto* saveSystem = m_engineContext->GetSaveSystem();
    World* world = m_engineContext->GetWorld();
    if (!saveSystem || !world)
    {
        outMessage = "Save system unavailable: engine exposes no save system or world";
        return false;
    }
    if (!saveSystem->SaveExists(kQuickSaveSlot))
    {
        outMessage = std::string("No quicksave found in slot '") + kQuickSaveSlot + "'";
        return false;
    }

    std::unordered_map<std::string, std::string> customState;
    if (!saveSystem->Load(kQuickSaveSlot, *world, customState))
    {
        outMessage = std::string("Quick load FAILED for slot '") + kQuickSaveSlot + "'";
        return false;
    }

    Spark::FPSLocalProfile profile;
    std::string profileError;
    if (!profile.ReadFrom(customState, profileError))
    {
        outMessage = "Quick load restored the world but the local profile was rejected: " + profileError;
        return false;
    }

    ApplyLocalProfile(profile);

    // Report the level the session is actually at: ApplyLocalProfile re-derives it from
    // XP through ProgressionSystem, which can disagree with the level stored in the file
    // (older save, changed XP curve, level cap).
    const int restoredLevel = m_progression ? m_progression->GetLevel() : profile.progressionLevel;
    outMessage = "Quick load restored level " + std::to_string(restoredLevel) + " (" +
                 std::to_string(profile.progressionXP) + " XP)";
    return true;
}
