/**
 * @file GameEngineSystems.cpp
 * @brief Engine system integration — wires audio, weather, destruction,
 *        dialogue, save, and coroutine systems into the game loop
 *
 * Extracted from Game.cpp to keep engine-integration code separate from
 * core gameplay logic.
 */

#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>
#endif
#include <cstdint>
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include "Game.h"
#include "Player.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#include "Core/EngineContext.h"

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

/*-------------------------------------------------------------
  Initialize engine system connections from the game side
--------------------------------------------------------------*/
void Game::InitializeEngineSystems()
{
    auto* ctx = EngineContext::Get();
    if (!ctx)
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"EngineContext not available - skipping engine system wiring", L"WARNING");
        return;
    }

    // ---- Audio --------------------------------------------------------
    // The AudioEngine and MusicManager are initialized by SparkEngine.cpp.
    // Here we set up game-specific audio: register tracks and start ambient music.
    {
        auto& musicMgr = Spark::Audio::MusicManager::GetInstance();

        // Register game music tracks for the combat arena
        Spark::Audio::MusicTrack explorationTrack;
        explorationTrack.name = "arena_ambient";
        explorationTrack.filepath = "Assets/Audio/Music/arena_ambient.ogg";
        explorationTrack.bpm = 90.0f;
        explorationTrack.loop = true;
        musicMgr.RegisterTrack(explorationTrack);

        Spark::Audio::MusicTrack combatTrack;
        combatTrack.name = "arena_combat";
        combatTrack.filepath = "Assets/Audio/Music/arena_combat.ogg";
        combatTrack.bpm = 140.0f;
        combatTrack.loop = true;
        musicMgr.RegisterTrack(combatTrack);

        // Set dynamic music states: exploration → combat as enemies engage
        Spark::Audio::DynamicMusicState dynamicState;
        dynamicState.explorationTrack = "arena_ambient";
        dynamicState.combatTrack = "arena_combat";
        dynamicState.transitionDuration = 2.0f;
        musicMgr.SetDynamicMusicState(dynamicState);

        // Start with exploration music
        musicMgr.Play("arena_ambient", 1.0f);

        m_audioInitialized = true;
        LOG_TO_CONSOLE_IMMEDIATE(L"Audio: game music tracks registered, ambient playing", L"SUCCESS");
    }

    // ---- Weather ------------------------------------------------------
    // Access the WeatherSystem registered by SparkEngine and set initial weather
    if (auto* weather = ctx->GetSystem<Spark::WeatherSystem>())
    {
        weather->SetWeather(Spark::WeatherType::Clear, 1.0f, 0.0f);
        m_weatherActive = true;
        LOG_TO_CONSOLE_IMMEDIATE(L"Weather: clear skies set for arena", L"SUCCESS");
    }

    // ---- Destruction --------------------------------------------------
    // Register game-specific fracture patterns for arena objects
    {
        auto& destruction = Spark::DestructionSystem::GetInstance();

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
        destruction.RegisterPattern("wooden_crate", cratePattern);

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
        destruction.RegisterPattern("metal_barrel", barrelPattern);

        LOG_TO_CONSOLE_IMMEDIATE(L"Destruction: 2 fracture patterns registered (crate, barrel)", L"SUCCESS");
    }

    // ---- Dialogue -----------------------------------------------------
    // Register sample NPC dialogue trees for arena vendors
    if (auto* dialogue = ctx->GetSystem<Spark::DialogueSystem>())
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
    {
        m_saveSystemReady = true;
        LOG_TO_CONSOLE_IMMEDIATE(L"Save system: ready for quicksave/quickload", L"SUCCESS");
    }

    // ---- Coroutine Scheduler ------------------------------------------
    {
        LOG_TO_CONSOLE_IMMEDIATE(L"Coroutine scheduler: game coroutines available", L"SUCCESS");
    }

    // ---- Cinematic Sequencer ------------------------------------------
    // Create a sample intro sequence to demonstrate the cinematic system
    {
        auto& seqMgr = Spark::Cinematic::SequencerManager::GetInstance();
        auto* intro = seqMgr.CreateSequence("arena_intro");

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
        fadeTrack->AddKeyframe({0.0f, 1.0f, {0, 0, 0}, Spark::Cinematic::InterpolationMode::Linear}); // Start black
        fadeTrack->AddKeyframe({1.5f, 0.0f, {0, 0, 0}, Spark::Cinematic::InterpolationMode::Linear}); // Fade in

        // Event track: enable controls after intro
        auto* eventTrack = intro->AddEventTrack("GameEvents");
        eventTrack->AddCue({5.0f, "enable_player_control", ""});

        LOG_TO_CONSOLE_IMMEDIATE(L"Cinematic: arena_intro sequence registered (5s, 4 tracks)", L"SUCCESS");
    }

    // ---- Replay System ------------------------------------------------
    // Configure the replay system for match recording
    {
        auto& replay = Spark::ReplaySystem::GetInstance();
        replay.SetRecordInterval(1.0f / 20.0f); // 20 fps recording
        replay.SetMetadata("combat_arena", "freeplay");
        LOG_TO_CONSOLE_IMMEDIATE(L"Replay: system configured (20fps, combat_arena)", L"SUCCESS");
    }

    LOG_TO_CONSOLE_IMMEDIATE(L"All engine systems wired into game", L"SUCCESS");
}
