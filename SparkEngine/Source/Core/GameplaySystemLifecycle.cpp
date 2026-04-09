/**
 * @file GameplaySystemLifecycle.cpp
 * @brief Initialization, update, and shutdown for all gameplay and debug subsystems
 *
 * Extracted from SparkEngine.cpp. Contains the lifecycle management for 40+
 * gameplay, rendering-utility, AI, scripting, and debug subsystems.
 */

#include "GameplaySystemLifecycle.h"
#include "Platform.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Systems/ParallelSystemExecutor.h"
#include "EngineContext.h"
#include "EngineSettings.h"
#include "Engine/Events/EventSystem.h"
#include "Engine/Coroutine/CoroutineScheduler.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/WeatherSystem.h"
#include "Engine/World/TimeOfDaySystem.h"
#include "Engine/UI/UISystem.h"
#include "Engine/Dialogue/DialogueSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/ChromeTracing.h"
#include "Utils/MemoryDebugger.h"
#include "Utils/MemoryMonitor.h"
#include "Utils/HitchDetector.h"
#include "Utils/AssetStallDetector.h"
#include "Utils/NetworkHealthMonitor.h"
#include "Utils/GPUResourceLeakDetector.h"
#include "Utils/FrameInspector.h"
#include "Utils/Tween.h"
#include "Utils/DebugDraw.h"
#include "Utils/DebugHookManager.h"
#include "Utils/DebugOverlay.h"
#include "Utils/FileLogger.h"
#include "Utils/Logger.h"
#include "Utils/JobSystem.h"
#include "Utils/Profiler.h"
#include "Utils/ProfileProperties.h"
#include "Graphics/DecalSystem.h"
#include "Audio/MusicManager.h"
#include "Audio/AudioMixer.h"
#include "Engine/Gameplay/WeaponManager.h"
#include "Engine/Gameplay/AbilitySystem.h"
#include "Engine/Gameplay/ConditionSystem.h"
#include "Engine/Gameplay/InstanceManager.h"
#include "Engine/Gameplay/InventorySystem.h"
#include "Engine/Gameplay/QuestSystem.h"
#include "Engine/AI/MovementSystem.h"
#include "Engine/Destruction/DestructionSystem.h"
#include "Engine/ECS/Systems/TerrainSystem.h"
#include "Engine/AI/TacticalPointSystem.h"
#include "Engine/AI/CoverSystem.h"
#include "Engine/AI/FormationSystem.h"
#include "Engine/AI/GroupAI.h"
#include "Engine/AI/CollisionAvoidance.h"
#include "Engine/Gameplay/MaterialEffects.h"
#include "Engine/Gameplay/AchievementSystem.h"
#include "Engine/Accessibility/AccessibilitySystem.h"
#include "Engine/Dialogue/DynamicResponseSystem.h"
#include "Engine/ECS/EntityArchetype.h"
#include "Engine/Gameplay/WeatherGameplayIntegration.h"
#include "Graphics/MaterialLoader.h"
#include "Engine/World/ProximityTriggerSystem.h"
#include "Graphics/SkyAtmosphere.h"
#include "Graphics/WaterRenderer.h"
#include "Graphics/OcclusionCulling.h"
#include "Engine/SaveSystem/FreezeSystem.h"
#include "Engine/Editor/CoreComponentSerializers.h"
#include "Graphics/RenderCommandRing.h"
#include "Graphics/ConstantBufferDiff.h"
#include "Graphics/GPUProfiler.h"
#include "Utils/MultiISA.h"
#include "Utils/GPUPerfCounters.h"
#include "SceneManager/SceneConfigDatabase.h"
#include "Engine/Cinematic/Sequencer.h"
#include "Engine/Replay/ReplaySystem.h"
#include "Engine/VR/VRSystem.h"
#include "Engine/Animation/RagdollSystem.h"
#include "Engine/Mobile/MobilePlatform.h"
#include "FaultIsolation.h"
#include "FixedTimestepAccumulator.h"
#include "PluginRegistry.h"
#include "ResourceVersionTracker.h"
#include "Engine/Animation/AnimNotify.h"
#include "Engine/Gameplay/GameplayTags.h"
#include "Utils/GameplayDebugger.h"
#include "Graphics/ScreenCapture.h"
#include "Engine/Cinematic/VideoPlayer.h"
#include "Graphics/LightmapBaker.h"
#include "Engine/Procedural/ProceduralGenerator.h"
#include "Engine/Networking/DeltaSnapshotManager.h"
#include "Engine/Networking/NetworkManager.h"
#include "Engine/Networking/InstabilitySimulator.h"
#include "Engine/Security/MemoryIntegrity.h"
#include "Utils/InvalidStateDetector.h"
#include "Engine/Networking/ConnectionScopeFilter.h"
#include "Engine/Tween/TweenSystem.h"
#include "Engine/HotReload/ModuleHotReload.h"
#include "Engine/LevelDesign/CSGSystem.h"
#include "Engine/Text/FontSystem.h"
#include "Input/InputActionSystem.h"
#include "Engine/Build/GamePackager.h"
#include "Engine/OnlineServices/OnlineServices.h"
#include "Engine/DataTable/DataTableSystem.h"
#include "Engine/Rendering/MovieRenderPipeline.h"
#include "Engine/RemoteDebug/RemoteDebugSystem.h"
#include "Engine/HLOD/HLODSystem.h"
#include "Engine/Crafting/LootAndCraftingSystem.h"
#include "Utils/FileWatcher/FileWatcher.h"
#include "Utils/TimerManager.h"
#include "Utils/InGameConsole.h"
#include "Engine/Modding/VirtualFileSystem.h"
#include "Engine/Modding/ArchiveResourceProvider.h"
#include "Engine/UI/UIFactory.h"
#include "Engine/Scripting/AngelScriptEngine.h"
#include "Engine/Scripting/LuaScriptEngine.h"
#include "Engine/Animation/BlendSpace.h"
#include "Graphics/RHI/DXRSupport.h"
#include "Graphics/RHI/NullRHIDevice.h"
#include "Graphics/RHI/RHIValidationLayer.h"
#include "Graphics/ClusteredLightCulling.h"
#include "Graphics/LightProbeSystem.h"
#include "Graphics/ClipmapTerrain.h"
#include "Graphics/VirtualTexture.h"
#include "Graphics/MaterialPropertyHandle.h"
#include "Graphics/RHI/PipelineStateCache.h"
#include "Graphics/RenderGraph/TransientResourcePool.h"
#include "Engine/ECS/Components/LightComponents.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Engine/Streaming/SeamlessAreaManager.h"
#ifdef ENABLE_NETWORKING
#include "Engine/Networking/AreaServer.h"
#include "Engine/Networking/WorldServer.h"
#include "Engine/Networking/DedicatedServer.h"
#endif

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

// ============================================================================
// SparkPak auto-mount — scan for .spk files beside the executable
// ============================================================================

static void MountSparkPakArchives()
{
    auto& vfs = Spark::VirtualFileSystem::GetInstance();
    auto& console = Spark::SimpleConsole::GetInstance();

    // Search for .spk files in "Data/" subdirectory next to executable
    std::filesystem::path dataDir = std::filesystem::current_path() / "Data";
    std::error_code ec;
    if (!std::filesystem::exists(dataDir, ec))
        return;

    std::vector<std::filesystem::path> archives;
    for (const auto& entry : std::filesystem::directory_iterator(dataDir, ec))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".spk")
            archives.push_back(entry.path());
    }

    std::sort(archives.begin(), archives.end());

    int32_t priorityOffset = 0;
    for (const auto& archivePath : archives)
    {
        auto provider = std::make_unique<Spark::ArchiveResourceProvider>(archivePath.string());
        if (provider->IsValid())
        {
            auto name = archivePath.stem().string();
            vfs.Mount(name, std::move(provider), Spark::ENGINE_PRIORITY + priorityOffset);
            console.Log("[SparkPak] Mounted: " + archivePath.filename().string() + " (priority " +
                        std::to_string(Spark::ENGINE_PRIORITY + priorityOffset) + ")");
            ++priorityOffset;
        }
        else
        {
            console.LogWarning("[SparkPak] Failed to open: " + archivePath.filename().string());
        }
    }
}

// ============================================================================
// Missing module warnings
// ============================================================================

void LogMissingModuleWarnings()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    int missingCount = 0;

#ifndef SPARK_BULLET_PHYSICS_AVAILABLE
    console.LogWarning(
        "[MISSING MODULE] Bullet Physics — rigid body simulation, collision detection, and raycasting are DISABLED.");
    console.LogWarning(
        "                 Physics-dependent features (gravity, projectiles, triggers) will not function.");
    ++missingCount;
#endif

#ifndef SPARK_MINIZ_AVAILABLE
    console.LogWarning("[MISSING MODULE] miniz — crash dump compression and save file compression are DISABLED.");
    console.LogWarning("                 CrashHandler is using a stub. Save files will not be compressed.");
    ++missingCount;
#endif

#ifndef SPARK_SDL2_AVAILABLE
#ifndef SPARK_PLATFORM_WINDOWS
    console.LogWarning("[MISSING MODULE] SDL2 — cross-platform windowing and input are DISABLED.");
    console.LogWarning("                 Install libsdl2-dev and rebuild with -DENABLE_SDL2=ON for windowed mode.");
    ++missingCount;
#endif
#endif

    if (missingCount > 0)
    {
        console.LogWarning("------------------------------------------------------------");
        console.LogWarning(std::to_string(missingCount) + " module(s) missing. Expect degraded functionality.");
        console.LogWarning("Run: git submodule update --init --recursive");
        console.LogWarning("Then rebuild to restore full engine features.");
        console.LogWarning("See README.md 'Dependencies' section for details.");
        console.LogWarning("------------------------------------------------------------");
    }
}

// ============================================================================
// Debug system lifecycle
// ============================================================================

void InitDebugSystems()
{
    // Initialize the debug hook manager first so subsequent inits can be observed
    Spark::DebugHookManager::GetInstance().SetEnabled(true);
    SPARK_DEBUG_HOOK(EnginePreInit, 0, 0.0f);

    // Initialize the unified Logger with a stderr sink so SPARK_LOG_* output is visible
    auto& logger = Spark::Logger::Get();
    logger.Initialize(/*enableAsync=*/false);
    logger.AddSink(std::make_unique<Spark::StderrSink>());

    // Apply logging configuration from settings.ini [Logging] section
    {
        const auto& logCfg = EngineSettings::GetInstance().Logging();
        Spark::Logger::Config cfg;
        cfg.globalLevel = Spark::StringToLogLevel(logCfg.globalLevel, Spark::LogLevel::Info);
        cfg.stackTraceLevel = Spark::StringToLogLevel(logCfg.stackTraceLevel, Spark::LogLevel::Error);
        cfg.categoryMask = logCfg.categoryMask;

        // Map per-category string overrides to LogLevel values
        struct CatOverride
        {
            Spark::LogCategory cat;
            const std::string& levelStr;
        };
        const CatOverride overrides[] = {
            {Spark::LogCategory::Core, logCfg.coreLevel},
            {Spark::LogCategory::Graphics, logCfg.graphicsLevel},
            {Spark::LogCategory::Physics, logCfg.physicsLevel},
            {Spark::LogCategory::Audio, logCfg.audioLevel},
            {Spark::LogCategory::AI, logCfg.aiLevel},
            {Spark::LogCategory::Animation, logCfg.animationLevel},
            {Spark::LogCategory::ECS, logCfg.ecsLevel},
            {Spark::LogCategory::Network, logCfg.networkLevel},
            {Spark::LogCategory::Input, logCfg.inputLevel},
            {Spark::LogCategory::Scripting, logCfg.scriptingLevel},
            {Spark::LogCategory::Scene, logCfg.sceneLevel},
            {Spark::LogCategory::Save, logCfg.saveLevel},
            {Spark::LogCategory::Cinematic, logCfg.cinematicLevel},
            {Spark::LogCategory::Procedural, logCfg.proceduralLevel},
            {Spark::LogCategory::Editor, logCfg.editorLevel},
            {Spark::LogCategory::Game, logCfg.gameLevel},
        };
        for (const auto& [cat, str] : overrides)
        {
            if (!str.empty())
            {
                auto idx = static_cast<size_t>(cat);
                cfg.categoryLevels[idx] = Spark::StringToLogLevel(str, cfg.globalLevel);
            }
        }
        logger.ApplyConfig(cfg);
    }

    Spark::FileLogger::GetInstance().Initialize("Logs");
    Spark::ChromeTracing::GetInstance().Start();
#ifndef NDEBUG
    Spark::MemoryDebugger::GetInstance().SetEnabled(true);
#endif
    Spark::DebugOverlay::GetInstance().SetEnabled(true);
    Spark::MemoryMonitor::GetInstance().Initialize();
    Spark::HitchDetector::GetInstance().Initialize();
    Spark::AssetStallDetector::GetInstance().Initialize();
    Spark::NetworkHealthMonitor::GetInstance().Initialize();
    Spark::GPUResourceLeakDetector::GetInstance().Initialize();
    Spark::Security::MemoryIntegritySystem::GetInstance().Initialize();
    Spark::InvalidStateDetector::GetInstance().Initialize();

    // Register memory pressure response callbacks
    Spark::MemoryMonitor::GetInstance().RegisterPressureCallback(
        "Graphics",
        [](Spark::MemoryHealthStatus status)
        {
            if (status == Spark::MemoryHealthStatus::Critical)
            {
                if (auto* ctx = EngineContext::Get())
                {
                    if (auto* gfx = ctx->GetGraphics())
                        gfx->Console_ForceGarbageCollection();
                }
                SPARK_LOG_WARN(Spark::LogCategory::Graphics, "Memory pressure: forced graphics garbage collection");
            }
        });

    Spark::DebugDrawManager::GetInstance().SetEnabled(true);

    // Graphics utility singletons
    Spark::Graphics::DecalSystem::GetInstance().Initialize();
    // LODManager is a passive cache (no init/update needed; queries only)
    // NavMeshObstacleManager is a passive registry (SetNavMesh + Add/Remove at level load time)
    // NavMeshManager is a passive registry (Load/Build at level load time, queried by AISystem)

    // Register default weapon definitions
    Spark::Gameplay::WeaponRegistry::GetInstance().RegisterDefaults();
}

// ============================================================================
// Gameplay system lifecycle
// ============================================================================

static void InitNetworkingLifecycle(EngineContext* ctx)
{
    SPARK_DEBUG_HOOK_SYSTEM(SystemPreInit, "NetworkingLifecycle", 0.0);

    if (!ctx->GetNetworkService())
    {
        auto& networkSingleton = Spark::NetworkManager::GetInstance(); // DI_SHIM_OK: one-time compatibility bridge
        ctx->SetNetwork(&networkSingleton);
        ctx->SetNetworkService(&networkSingleton);
    }

    auto* network = ctx->GetNetworkService();
    if (network)
    {
        if (!network->IsInitialized() && !network->Initialize())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "Network service failed to initialize");
        }
    }

    SPARK_DEBUG_HOOK_SYSTEM(SystemPostInit, "NetworkingLifecycle", 0.0);
}

static void InitCoreGameplaySystems(EngineContext* ctx)
{
    SPARK_DEBUG_HOOK_SYSTEM(SystemPreInit, "CoreGameplaySystems", 0.0);
    auto* eventBus = ctx->GetEventBus();

    Spark::Gameplay::ConditionSystem::GetInstance().Initialize();
    Spark::Gameplay::AbilitySystem::GetInstance().Initialize(eventBus);
    Spark::Gameplay::InstanceManager::GetInstance().Initialize();
    Spark::Gameplay::InventorySystem::GetInstance().Initialize();
    Spark::Gameplay::QuestSystem::GetInstance().Initialize();
    Spark::Gameplay::AchievementSystem::GetInstance().Initialize();
    Spark::Accessibility::AccessibilitySystem::GetInstance().Initialize();
    Spark::AI::MovementSystem::GetInstance().Initialize();
    Spark::Audio::MusicManager::GetInstance().Initialize();
    Spark::Audio::AudioMixer::GetInstance().Initialize();

    auto& destruction = Spark::DestructionSystem::GetInstance();
    destruction.Initialize();
    if (auto* world = ctx->GetWorld())
    {
        destruction.SetWorld(world);
        Spark::InvalidStateDetector::GetInstance().SetWorld(world);
    }
    destruction.OnDestruction(
        [](const Spark::DestructionEvent& e)
        { Spark::Dialogue::DynamicResponseSystem::GetInstance().SendSignal("OnDestruction", e.entityId); });
    SPARK_DEBUG_HOOK_SYSTEM(SystemPostInit, "CoreGameplaySystems", 0.0);
}

static void InitAIAndWorldSystems(Spark::EventBus* eventBus)
{
    SPARK_DEBUG_HOOK_SYSTEM(SystemPreInit, "AIAndWorldSystems", 0.0);
    Spark::AI::TacticalPointSystem::GetInstance().Initialize();
    Spark::AI::CoverSystem::GetInstance().Initialize();
    Spark::AI::FormationSystem::GetInstance().Initialize();
    Spark::AI::GroupAISystem::GetInstance().Initialize();
    Spark::AI::CollisionAvoidanceSystem::GetInstance().Initialize();
    Spark::Gameplay::MaterialEffectSystem::GetInstance().Initialize();
    Spark::Dialogue::DynamicResponseSystem::GetInstance().Initialize();
    Spark::ECS::EntityArchetypeSystem::GetInstance().Initialize();
    Spark::Gameplay::WeatherGameplayIntegration::GetInstance().Initialize(eventBus);
    Spark::World::ProximityTriggerSystem::GetInstance().Initialize();
    Spark::Graphics::SkyAtmosphereSystem::GetInstance().Initialize();
    Spark::Graphics::WaterRenderer::GetInstance().Initialize();
    Spark::Graphics::OcclusionCullingSystem::GetInstance().Initialize();
    SPARK_DEBUG_HOOK_SYSTEM(SystemPostInit, "AIAndWorldSystems", 0.0);
}

static void InitRenderingAndUtilitySystems(EngineContext* ctx)
{
    SPARK_DEBUG_HOOK_SYSTEM(SystemPreInit, "RenderingAndUtility", 0.0);
    Spark::FreezeSystem::GetInstance().Initialize();
    Spark::Graphics::RenderCommandQueue::GetInstance().Initialize();
    Spark::Graphics::ConstantBufferDiffManager::GetInstance().Initialize();
    Spark::Graphics::GPUPerfCounters::GetInstance().Initialize();
    Spark::MultiISADispatch::GetInstance().Initialize();
    Spark::SceneConfigDatabase::GetInstance().Initialize();

    Spark::FixedTimestepAccumulator::GetInstance().Initialize();
    Spark::TweenSystem::GetInstance().Initialize();
    Spark::VirtualFileSystem::GetInstance().Initialize();
    MountSparkPakArchives();
    Spark::UI::UIFactory::GetInstance().Initialize();
    Spark::Graphics::ClusteredLightCulling::GetInstance().Initialize();
    Spark::Graphics::LightProbeSystem::GetInstance().Initialize();
    Spark::Graphics::PipelineStateCache::GetInstance().Initialize();
    Spark::Graphics::TransientResourcePool::GetInstance().Initialize();
    Spark::Graphics::ClipmapTerrain::GetInstance().Initialize();
    Spark::Graphics::VirtualTextureManager::GetInstance().Initialize();
    Spark::PluginRegistry::InitializeAll();

    Spark::Animation::AnimNotifyManager::GetInstance().Initialize();
    if (ctx->GetGameplayTagService())
    {
        ctx->GetGameplayTagService()->Initialize();
    }
    else
    {
        auto& gameplayTagRegistry = Spark::Gameplay::GameplayTagRegistry::GetInstance();
        ctx->SetGameplayTagService(&gameplayTagRegistry);
        gameplayTagRegistry.Initialize();
    }
    Spark::Utils::GameplayDebugger::GetInstance().Initialize();
    Spark::Graphics::ScreenCapture::GetInstance().Initialize();
    Spark::Cinematic::VideoPlayer::GetInstance().Initialize();
    Spark::Graphics::LightmapBaker::GetInstance().Initialize();
    Spark::Procedural::ProceduralGenerator::GetInstance().Initialize();
    Spark::Graphics::GPUProfiler::GetInstance().Initialize();

    Spark::HotReload::ModuleHotReload::GetInstance().Initialize();
    Spark::LevelDesign::CSGSystem::GetInstance().Initialize();
    Spark::Text::FontSystem::GetInstance().Initialize();
    Spark::Input::InputActionSystem::GetInstance().Initialize();
    Spark::Build::GamePackager::GetInstance().Initialize();
    Spark::OnlineServices::OnlineServiceManager::GetInstance().Initialize();
    Spark::Data::DataTableRegistry::GetInstance().Initialize();
    Spark::Rendering::MovieRenderPipeline::GetInstance().Initialize();
    Spark::RemoteDebug::RemoteDebugSystem::GetInstance().Initialize();
    Spark::HLOD::HLODSystem::GetInstance().Initialize();
    Spark::Gameplay::LootTableManager::GetInstance().Initialize();
    Spark::Gameplay::CraftingSystem::GetInstance().Initialize();
    Spark::Utils::FileWatcher::GetInstance().Initialize();
    Spark::TimerManager::GetInstance().Initialize();
    Spark::InGameConsole::GetInstance().Initialize();
    SPARK_DEBUG_HOOK_SYSTEM(SystemPostInit, "RenderingAndUtility", 0.0);
}

static void InitScriptingAndPlatformSystems(EngineContext* ctx)
{
    SPARK_DEBUG_HOOK_SYSTEM(SystemPreInit, "ScriptingAndPlatform", 0.0);
    {
        static AngelScriptEngine s_angelScript;
        if (s_angelScript.Initialize())
        {
            AngelScriptEngine::BindWorld(ctx->GetWorld());
            SPARK_LOG_INFO(Spark::LogCategory::Core, "AngelScriptEngine initialized");
        }
        else
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "AngelScriptEngine init failed — scripts disabled");
        }
    }

    if (Spark::Scripting::LuaScriptEngine::GetInstance().Initialize())
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "LuaScriptEngine initialized");
    }
    else
    {
        SPARK_LOG_WARN(Spark::LogCategory::Core, "LuaScriptEngine init failed — Lua scripts disabled");
    }

    (void)Spark::Animation::BlendSpaceManager::GetInstance();
    Profiler::GetInstance().SetEnabled(true);

#ifdef SPARK_HARDWARE_RT
    Spark::Graphics::DXRManager::GetInstance().Initialize(nullptr);
#endif

#ifndef NDEBUG
    Spark::Graphics::RHIValidationLayer::GetInstance().Initialize();
    (void)Spark::RHI::NullRHIDevice::GetInstance();
#endif

    Spark::Streaming::SeamlessAreaManager::GetInstance().Initialize();

#ifdef ENABLE_NETWORKING
    SPARK_LOG_INFO(Spark::LogCategory::Core,
                   "Networking enabled — AreaServer, WorldServer, and DedicatedServer are available");
#endif

    ctx->SetCinematic(&Spark::Cinematic::SequencerManager::GetInstance());
    ctx->SetReplay(&Spark::ReplaySystem::GetInstance());

    {
        static Spark::Animation::RagdollSystem s_ragdollSystem;
        s_ragdollSystem.Initialize();
    }

    {
        static Spark::VR::VRSystem s_vrSystem;
        if (s_vrSystem.Initialize())
        {
            ctx->SetVR(&s_vrSystem);
            SPARK_LOG_INFO(Spark::LogCategory::Core, "VRSystem initialized — VR hardware detected");
        }
        else
        {
            ctx->SetVR(&s_vrSystem);
        }
    }

    {
        static Spark::Mobile::MobilePlatform s_mobilePlatform;
        if (s_mobilePlatform.Initialize())
        {
            SPARK_LOG_INFO(Spark::LogCategory::Core, "MobilePlatform initialized");
        }
    }
    SPARK_DEBUG_HOOK_SYSTEM(SystemPostInit, "ScriptingAndPlatform", 0.0);
}

void InitGameplaySystems()
{
    auto* ctx = EngineContext::Get();
    if (!ctx)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "EngineContext is null — all gameplay systems skipped");
        return;
    }

    InitNetworkingLifecycle(ctx);
    InitCoreGameplaySystems(ctx);

    // Register snapshot serializers for play-in-editor mode
    Spark::Editor::RegisterCoreComponentSerializers();

    if (auto* eventBus = ctx->GetEventBus())
    {
        InitAIAndWorldSystems(eventBus);
    }
    else
    {
        SPARK_LOG_WARN(Spark::LogCategory::Core, "EventBus is null — AI/World systems skipped");
    }
    InitRenderingAndUtilitySystems(ctx);
    InitScriptingAndPlatformSystems(ctx);
}

// ============================================================================
// Per-frame update
// ============================================================================

static void UpdateNonECSSystems(EngineContext* ctx, float dt)
{
    auto& jobs = Spark::JobSystem::Get();
    bool canParallelize = jobs.IsInitialized() && jobs.GetWorkerCount() > 0;

    if (canParallelize)
    {
        // Batch 1: Weather, TimeOfDay, Dialogue, UI are independent — run in parallel
        auto futWeather = jobs.Submit(
            [ctx, dt]()
            {
                SPARK_GUARDED_UPDATE("Weather", "Core", {
                    if (auto* weather = ctx->GetWeather())
                        weather->Update(dt);
                });
            });

        auto futTimeOfDay = jobs.Submit(
            [dt]()
            { SPARK_GUARDED_UPDATE("TimeOfDay", "Core", { Spark::TimeOfDaySystem::GetInstance().Update(dt); }); });

        auto futDialogue = jobs.Submit(
            [ctx, dt]()
            {
                SPARK_GUARDED_UPDATE("Dialogue", "Core", {
                    if (auto* dialogue = ctx->GetDialogue())
                        dialogue->Update(dt);
                });
            });

        auto futUI = jobs.Submit(
            [ctx, dt]()
            {
                SPARK_GUARDED_UPDATE("UI", "Core", {
                    if (auto* ui = ctx->GetUI())
                        ui->Update(dt);
                });
            });

        // Wait for parallel batch to complete
        futWeather.get();
        futTimeOfDay.get();
        futDialogue.get();
        futUI.get();

        // WeatherGameplay depends on Weather — must run after Weather completes
        SPARK_GUARDED_UPDATE("WeatherGameplay", "Core", {
            auto* weather = ctx->GetWeather();
            auto* physics = ctx->GetPhysics();
            if (weather && physics)
            {
                Spark::Gameplay::WeatherGameplayIntegration::GetInstance().Update(dt, weather, physics);
            }
        });
    }
    else
    {
        // Fallback: sequential execution when JobSystem is unavailable
        SPARK_GUARDED_UPDATE("Weather", "Core", {
            if (auto* weather = ctx->GetWeather())
                weather->Update(dt);
        });

        SPARK_GUARDED_UPDATE("TimeOfDay", "Core", { Spark::TimeOfDaySystem::GetInstance().Update(dt); });

        SPARK_GUARDED_UPDATE("WeatherGameplay", "Core", {
            auto* weather = ctx->GetWeather();
            auto* physics = ctx->GetPhysics();
            if (weather && physics)
            {
                Spark::Gameplay::WeatherGameplayIntegration::GetInstance().Update(dt, weather, physics);
            }
        });

        SPARK_GUARDED_UPDATE("Dialogue", "Core", {
            if (auto* dialogue = ctx->GetDialogue())
                dialogue->Update(dt);
        });

        SPARK_GUARDED_UPDATE("UI", "Core", {
            if (auto* ui = ctx->GetUI())
                ui->Update(dt);
        });
    }
}

static void UpdateECSDependentSystems(World* world, float dt)
{
    SPARK_GUARDED_UPDATE("AbilitySystem", "Core", {
        SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "AbilitySystem", 0.0);
        Spark::Gameplay::AbilitySystem::GetInstance().Update(*world, dt);
        SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "AbilitySystem", 0.0);
    });

    SPARK_GUARDED_UPDATE("AI_Movement", "Core", {
        SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "AI_Movement", 0.0);
        Spark::Gameplay::InstanceManager::GetInstance().Update(dt);
        Spark::AI::MovementSystem::GetInstance().Update(*world, dt);
        SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "AI_Movement", 0.0);
    });

    SPARK_GUARDED_UPDATE("Coroutine", "Core", { Spark::CoroutineScheduler::GetInstance().Update(dt); });

    SPARK_GUARDED_UPDATE("MusicManager", "Core", { Spark::Audio::MusicManager::GetInstance().Update(dt); });

    SPARK_GUARDED_UPDATE("AudioMixer", "Core", { Spark::Audio::AudioMixer::GetInstance().Update({0, 0, 0}, dt); });
    SPARK_GUARDED_UPDATE("Accessibility", "Core",
                         { Spark::Accessibility::AccessibilitySystem::GetInstance().Update(dt); });

    SPARK_GUARDED_UPDATE("WeaponSystem", "Core", {
        static Spark::Gameplay::WeaponSystem s_weaponSystem;
        s_weaponSystem.Update(dt);
    });

    SPARK_GUARDED_UPDATE("Destruction", "Core", {
        SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "Destruction", 0.0);
        auto& destruction = Spark::DestructionSystem::GetInstance();
        destruction.SetWorld(world);
        destruction.Update(dt);
        SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "Destruction", 0.0);
    });

    SPARK_GUARDED_UPDATE("Terrain", "Core", {
        static Spark::ECS::TerrainSystem s_terrainSystem;
        s_terrainSystem.Update(*world, dt);
    });

    SPARK_GUARDED_UPDATE("AI_Tactical", "Core", {
        SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "AI_Tactical", 0.0);
        Spark::AI::FormationSystem::GetInstance().Update(dt);
        Spark::AI::GroupAISystem::GetInstance().Update(dt);
        SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "AI_Tactical", 0.0);
    });

    SPARK_GUARDED_UPDATE("DynamicResponse", "Core",
                         { Spark::Dialogue::DynamicResponseSystem::GetInstance().Update(dt); });

    SPARK_GUARDED_UPDATE("SkyAtmosphere", "Core", { Spark::Graphics::SkyAtmosphereSystem::GetInstance().Update(dt); });

    SPARK_GUARDED_UPDATE("WaterRenderer", "Core", { Spark::Graphics::WaterRenderer::GetInstance().Update(dt); });
}

static void UpdateClusteredLighting(World* world)
{
    SPARK_GUARDED_UPDATE("ClusteredLighting", "Core", {
        auto& clustering = Spark::Graphics::ClusteredLightCulling::GetInstance();
        clustering.ClearLights();

        auto& reg = world->GetRegistry();
        auto lightView = reg.view<LightComponent, Transform>();
        for (auto entity : lightView)
        {
            const auto& lc = lightView.get<LightComponent>(entity);
            const auto& xform = lightView.get<Transform>(entity);

            Spark::Graphics::LightData ld;
            ld.position = xform.position;
            ld.color = lc.color;
            ld.intensity = lc.intensity;
            ld.radius = lc.range;
            ld.type = (lc.type == LightComponent::Type::Point) ? 0u : 1u;
            clustering.AddLight(ld);
        }

        if (const auto* ctx = EngineContext::Get())
        {
            if (const auto* graphics = ctx->GetGraphics())
            {
                XMFLOAT4X4 viewMat, projMat;
                XMStoreFloat4x4(&viewMat, graphics->GetFrameViewMatrix());
                XMStoreFloat4x4(&projMat, graphics->GetFrameProjectionMatrix());
                clustering.Update(viewMat, projMat, graphics->GetNearPlane(), graphics->GetFarPlane());
            }
        }
    });
}

static void UpdateExtendedSystems(EngineContext* ctx, float dt)
{
    auto& jobs = Spark::JobSystem::Get();
    bool canParallelize = jobs.IsInitialized() && jobs.GetWorkerCount() > 0;

    if (canParallelize)
    {
        // Batch: SeamlessArea, Tween, Cinematic, Replay are independent singletons
        auto futSeamless = jobs.Submit(
            [dt]()
            {
                SPARK_GUARDED_UPDATE("SeamlessArea", "Core",
                                     { Spark::Streaming::SeamlessAreaManager::GetInstance().Update(dt); });
            });

        auto futTween = jobs.Submit(
            [dt]() { SPARK_GUARDED_UPDATE("Tween", "Core", { Spark::TweenSystem::GetInstance().Update(dt); }); });

        auto futCinematic = jobs.Submit(
            [dt]()
            {
                SPARK_GUARDED_UPDATE("Cinematic", "Core",
                                     { Spark::Cinematic::SequencerManager::GetInstance().Update(dt); });
            });

        auto futReplay = jobs.Submit(
            [dt]()
            { SPARK_GUARDED_UPDATE("Replay", "Core", { Spark::ReplaySystem::GetInstance().UpdatePlayback(dt); }); });

        // These run on the main thread while the batch is in flight
        SPARK_GUARDED_UPDATE("UIFactory", "Core", { Spark::UI::UIFactory::GetInstance().UpdateAllBindings(); });
        SPARK_GUARDED_UPDATE("Plugins", "Core", { Spark::PluginRegistry::UpdateAll(dt); });

#ifndef NDEBUG
        Spark::ProfileProperties::GetInstance().ResetFrameProperties();
#endif

        SPARK_GUARDED_UPDATE("VR", "Core", {
            if (auto* vr = ctx->GetVR())
                vr->UpdateTracking();
        });

        // Wait for parallel batch before ECS executor (which may depend on Tween/Cinematic results)
        futSeamless.get();
        futTween.get();
        futCinematic.get();
        futReplay.get();
    }
    else
    {
        SPARK_GUARDED_UPDATE("SeamlessArea", "Core",
                             { Spark::Streaming::SeamlessAreaManager::GetInstance().Update(dt); });
        SPARK_GUARDED_UPDATE("Tween", "Core", { Spark::TweenSystem::GetInstance().Update(dt); });
        SPARK_GUARDED_UPDATE("UIFactory", "Core", { Spark::UI::UIFactory::GetInstance().UpdateAllBindings(); });
        SPARK_GUARDED_UPDATE("Plugins", "Core", { Spark::PluginRegistry::UpdateAll(dt); });

#ifndef NDEBUG
        Spark::ProfileProperties::GetInstance().ResetFrameProperties();
#endif

        SPARK_GUARDED_UPDATE("Cinematic", "Core", { Spark::Cinematic::SequencerManager::GetInstance().Update(dt); });
        SPARK_GUARDED_UPDATE("Replay", "Core", { Spark::ReplaySystem::GetInstance().UpdatePlayback(dt); });
        SPARK_GUARDED_UPDATE("VR", "Core", {
            if (auto* vr = ctx->GetVR())
                vr->UpdateTracking();
        });
    }

    SPARK_GUARDED_UPDATE("GameplayDebugger", "Core", { Spark::Utils::GameplayDebugger::GetInstance().Update(dt); });
    SPARK_GUARDED_UPDATE("VideoPlayer", "Core", { Spark::Cinematic::VideoPlayer::GetInstance().Update(dt); });

    SPARK_GUARDED_UPDATE("ECS_Executor", "Core", { Spark::ECS::StageBasedExecutor::GetInstance().ExecuteAll(dt); });
}

static uint64_t g_frameCounter = 0;

uint64_t GetGameplayFrameCount()
{
    return g_frameCounter;
}

void UpdateGameplaySystems(float dt)
{
    auto* ctx = EngineContext::Get();
    if (!ctx)
        return;

    ++g_frameCounter;
    auto& debugHooks = Spark::DebugHookManager::GetInstance();
    debugHooks.SetFrameNumber(g_frameCounter);
    debugHooks.SetDeltaTime(dt);

    // Update fault isolation auto-recovery (re-enables subsystems after cooldown)
    static float s_engineTime = 0.0f;
    s_engineTime += dt;
    Spark::SubsystemFaultIsolator::GetInstance().Update(s_engineTime);

    Profiler::GetInstance().BeginFrame();
    Spark::Graphics::GPUProfiler::GetInstance().BeginFrame();

    SPARK_DEBUG_HOOK(FrameBegin, g_frameCounter, dt);

    if (auto* bus = ctx->GetEventBus())
    {
        bus->Publish(Spark::FrameBeginEvent{dt});
    }

    SPARK_GUARDED_UPDATE("ModuleHotReload", "Core", { Spark::HotReload::ModuleHotReload::GetInstance().Update(dt); });
    SPARK_GUARDED_UPDATE("InputActions", "Core", { Spark::Input::InputActionSystem::GetInstance().Update(); });
    SPARK_GUARDED_UPDATE("OnlineServices", "Core",
                         { Spark::OnlineServices::OnlineServiceManager::GetInstance().Update(dt); });
    SPARK_GUARDED_UPDATE("MovieRender", "Core", { Spark::Rendering::MovieRenderPipeline::GetInstance().Update(dt); });
    SPARK_GUARDED_UPDATE("RemoteDebug", "Core", { Spark::RemoteDebug::RemoteDebugSystem::GetInstance().Update(dt); });
    SPARK_GUARDED_UPDATE("Crafting", "Core", { Spark::Gameplay::CraftingSystem::GetInstance().Update(dt); });
    SPARK_GUARDED_UPDATE("FileWatcher", "Core", { Spark::Utils::FileWatcher::GetInstance().Update(dt); });
    SPARK_GUARDED_UPDATE("TimerManager", "Core", { Spark::TimerManager::GetInstance().Update(dt); });
    SPARK_GUARDED_UPDATE("InGameConsole", "Core", { Spark::InGameConsole::GetInstance().Update(dt); });

    UpdateNonECSSystems(ctx, dt);

    auto* world = ctx->GetWorld();
    if (!world)
        return;

    UpdateECSDependentSystems(world, dt);

    Spark::Graphics::ConstantBufferDiffManager::GetInstance().BeginFrame();
    Spark::Graphics::GPUProfiler::GetInstance().EndFrame();
    Spark::Graphics::GPUPerfCounters::GetInstance().EndFrame();

    UpdateClusteredLighting(world);
    UpdateExtendedSystems(ctx, dt);

    Profiler::GetInstance().EndFrame();

    SPARK_DEBUG_HOOK(FrameEnd, g_frameCounter, dt);

    if (auto* bus = ctx->GetEventBus())
    {
        bus->Publish(Spark::FrameEndEvent{dt});
    }
}

// ============================================================================
// Shutdown
// ============================================================================

static void ShutdownAIAndWorldSystems()
{
    Spark::Graphics::OcclusionCullingSystem::GetInstance().Shutdown();
    Spark::Graphics::WaterRenderer::GetInstance().Shutdown();
    Spark::Graphics::SkyAtmosphereSystem::GetInstance().Shutdown();
    Spark::World::ProximityTriggerSystem::GetInstance().Shutdown();
    Spark::Gameplay::WeatherGameplayIntegration::GetInstance().Shutdown();
    Spark::Graphics::MaterialLoader::GetInstance().Shutdown();
    Spark::ECS::EntityArchetypeSystem::GetInstance().Shutdown();
    Spark::Dialogue::DynamicResponseSystem::GetInstance().Shutdown();
    Spark::Gameplay::MaterialEffectSystem::GetInstance().Shutdown();
    Spark::AI::CollisionAvoidanceSystem::GetInstance().Shutdown();
    Spark::AI::GroupAISystem::GetInstance().Shutdown();
    Spark::AI::FormationSystem::GetInstance().Shutdown();
    Spark::AI::CoverSystem::GetInstance().Shutdown();
    Spark::AI::TacticalPointSystem::GetInstance().Shutdown();
}

static void ShutdownRenderingAndUtilitySystems()
{
    Spark::SceneConfigDatabase::GetInstance().Shutdown();
    Spark::Graphics::GPUPerfCounters::GetInstance().Shutdown();
    Spark::Graphics::ConstantBufferDiffManager::GetInstance().Shutdown();
    Spark::Graphics::RenderCommandQueue::GetInstance().Shutdown();
    Spark::FreezeSystem::GetInstance().Shutdown();

    Spark::PluginRegistry::ShutdownAll();
#ifndef NDEBUG
    Spark::ProfileProperties::GetInstance().Shutdown();
#endif
    Spark::Graphics::VirtualTextureManager::GetInstance().Shutdown();
    Spark::Graphics::ClipmapTerrain::GetInstance().Shutdown();
    Spark::Graphics::TransientResourcePool::GetInstance().Shutdown();
    Spark::Graphics::PipelineStateCache::GetInstance().Shutdown();
    Spark::Graphics::MaterialPropertyRegistry::GetInstance().Shutdown();
    Spark::Graphics::LightProbeSystem::GetInstance().Shutdown();
    Spark::Graphics::ClusteredLightCulling::GetInstance().Shutdown();
    Spark::UI::UIFactory::GetInstance().Shutdown();
    Spark::VirtualFileSystem::GetInstance().Shutdown();
    Spark::TweenSystem::GetInstance().Shutdown();
    Spark::Net::InstabilitySimulator::GetInstance().Shutdown();
    Spark::Net::DeltaSnapshotManager::GetInstance().Shutdown();
    Spark::ResourceVersionTracker::GetInstance().Shutdown();
    Spark::FixedTimestepAccumulator::GetInstance().Shutdown();
}

void ShutdownGameplaySystems()
{
    auto* ctx = EngineContext::Get();
    if (ctx)
    {
        if (auto* vr = ctx->GetVR())
            vr->Shutdown();
    }

    Spark::ECS::StageBasedExecutor::GetInstance().Shutdown();
    Spark::Streaming::SeamlessAreaManager::GetInstance().Shutdown();
    Spark::Net::ConnectionScopeFilter::GetInstance().Shutdown();

    ShutdownAIAndWorldSystems();
    ShutdownRenderingAndUtilitySystems();

    Spark::Audio::MusicManager::GetInstance().Shutdown();
    Spark::AI::MovementSystem::GetInstance().Shutdown();
    Spark::Gameplay::QuestSystem::GetInstance().Shutdown();
    Spark::Gameplay::InventorySystem::GetInstance().Shutdown();
    Spark::Gameplay::InstanceManager::GetInstance().Shutdown();
    Spark::Gameplay::AbilitySystem::GetInstance().Shutdown();
    Spark::Gameplay::ConditionSystem::GetInstance().Shutdown();
    Spark::Gameplay::AchievementSystem::GetInstance().Shutdown();
    Spark::Accessibility::AccessibilitySystem::GetInstance().Shutdown();

    Spark::Animation::AnimNotifyManager::GetInstance().Shutdown();
    if (ctx && ctx->GetGameplayTagService())
    {
        ctx->GetGameplayTagService()->Shutdown();
    }
    else
    {
        Spark::Gameplay::GameplayTagRegistry::GetInstance().Shutdown();
    }
    Spark::Utils::GameplayDebugger::GetInstance().Shutdown();
    Spark::Graphics::ScreenCapture::GetInstance().Shutdown();
    Spark::Cinematic::VideoPlayer::GetInstance().Shutdown();
    Spark::Graphics::LightmapBaker::GetInstance().Shutdown();
    Spark::Procedural::ProceduralGenerator::GetInstance().Shutdown();
    Spark::Graphics::GPUProfiler::GetInstance().Shutdown();
    Spark::HotReload::ModuleHotReload::GetInstance().Shutdown();
    Spark::LevelDesign::CSGSystem::GetInstance().Shutdown();
    Spark::Text::FontSystem::GetInstance().Shutdown();
    Spark::Input::InputActionSystem::GetInstance().Shutdown();
    Spark::Build::GamePackager::GetInstance().Shutdown();
    Spark::InGameConsole::GetInstance().Shutdown();
    Spark::TimerManager::GetInstance().Shutdown();
    Spark::Utils::FileWatcher::GetInstance().Shutdown();
    Spark::Gameplay::CraftingSystem::GetInstance().Shutdown();
    Spark::Gameplay::LootTableManager::GetInstance().Shutdown();
    Spark::HLOD::HLODSystem::GetInstance().Shutdown();
    Spark::RemoteDebug::RemoteDebugSystem::GetInstance().Shutdown();
    Spark::Rendering::MovieRenderPipeline::GetInstance().Shutdown();
    Spark::Data::DataTableRegistry::GetInstance().Shutdown();
    Spark::OnlineServices::OnlineServiceManager::GetInstance().Shutdown();

    if (ctx)
    {
        if (auto* network = ctx->GetNetworkService())
        {
            network->Shutdown();
        }
    }

    if (auto* as = AngelScriptEngine::GetInstance())
    {
        as->Shutdown();
    }
    Spark::Scripting::LuaScriptEngine::GetInstance().Shutdown();

    Profiler::GetInstance().Shutdown();

#ifdef SPARK_HARDWARE_RT
    Spark::Graphics::DXRManager::GetInstance().Shutdown();
#endif

#ifndef NDEBUG
    Spark::Graphics::RHIValidationLayer::GetInstance().Shutdown();
#endif
}

void UpdateDebugSystems(float dt)
{
    SPARK_GUARDED_UPDATE("TweenManager", "Debug", { Spark::TweenManager::GetInstance().Update(dt); });
    Spark::DebugDrawManager::GetInstance().Flush(dt);
    SPARK_GUARDED_UPDATE("DebugOverlay", "Debug", { Spark::DebugOverlay::GetInstance().Update(dt); });
    SPARK_GUARDED_UPDATE("MemoryMonitor", "Debug", { Spark::MemoryMonitor::GetInstance().Update(dt); });
    SPARK_GUARDED_UPDATE("HitchDetector", "Debug", { Spark::HitchDetector::GetInstance().Update(dt); });
    SPARK_GUARDED_UPDATE("AssetStallDetector", "Debug", { Spark::AssetStallDetector::GetInstance().Update(dt); });
    SPARK_GUARDED_UPDATE("NetworkHealthMonitor", "Debug", { Spark::NetworkHealthMonitor::GetInstance().Update(dt); });
    SPARK_GUARDED_UPDATE("GPUResourceLeakDetector", "Debug",
                         { Spark::GPUResourceLeakDetector::GetInstance().Update(dt); });
    SPARK_GUARDED_UPDATE("MemoryIntegrity", "Security",
                         { Spark::Security::MemoryIntegritySystem::GetInstance().Update(dt); });
    SPARK_GUARDED_UPDATE("InvalidStateDetector", "Debug", { Spark::InvalidStateDetector::GetInstance().Update(dt); });
    Spark::FrameInspector::GetInstance().OnFrameEnd();

    // Update decal fading
    SPARK_GUARDED_UPDATE("DecalSystem", "Debug", { Spark::Graphics::DecalSystem::GetInstance().Update(dt); });
}

void ShutdownDebugSystems()
{
    SPARK_DEBUG_HOOK_SYSTEM(SystemPreShutdown, "DebugSystems", 0.0);

    Spark::Graphics::DecalSystem::GetInstance().Shutdown();

    Spark::TweenManager::GetInstance().KillAll();
    Spark::DebugDrawManager::GetInstance().Clear();
    Spark::MemoryMonitor::GetInstance().Shutdown();
    Spark::HitchDetector::GetInstance().Shutdown();
    Spark::AssetStallDetector::GetInstance().Shutdown();
    Spark::NetworkHealthMonitor::GetInstance().Shutdown();
    Spark::GPUResourceLeakDetector::GetInstance().Shutdown();
    Spark::Security::MemoryIntegritySystem::GetInstance().Shutdown();
    Spark::InvalidStateDetector::GetInstance().Shutdown();
#ifndef NDEBUG
    Spark::MemoryDebugger::GetInstance().PrintLeakReport();
#endif
    Spark::ChromeTracing::GetInstance().SaveToFile("spark_trace.json");
    Spark::ChromeTracing::GetInstance().Stop();
    Spark::FileLogger::GetInstance().Shutdown();

    SPARK_DEBUG_HOOK_SYSTEM(SystemPostShutdown, "DebugSystems", 0.0);
}
