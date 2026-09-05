/**
 * @file GameplaySystemLifecycle.cpp
 * @brief Initialization, update, and shutdown for all gameplay and debug subsystems
 *
 * Extracted from SparkEngine.cpp. Contains the lifecycle management for 40+
 * gameplay, rendering-utility, AI, scripting, and debug subsystems.
 */

#include "Core/Lifecycle/GameplayLifecycleShared.h"
#include "Core/Platform.h"
#include "Engine/ECS/Components.h"
#include "Core/EngineContext.h"
#include "Core/EngineSetup.h"
#include "Core/EngineSettings.h"
#include "Core/EngineRuntime.h"
#include "Core/RuntimePackage.h"
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
#include "Utils/BenchmarkFramework.h"
#include "Utils/AssetStallDetector.h"
#include "Core/AssetValidator.h"
#include "Utils/NetworkHealthMonitor.h"
#include "Utils/GPUResourceLeakDetector.h"
#include "Utils/FrameInspector.h"
#include "Utils/Tween.h"
#include "Utils/DebugDraw.h"
#include "Utils/DebugHookManager.h"
#include "Utils/DebugOverlay.h"
#include "Utils/Logger.h"
#include "Utils/ConsoleSink.h"
#include "Utils/Profiler.h"
#include "Utils/ProfileProperties.h"
#include "Graphics/DecalSystem.h"
#include "Audio/AudioEngine.h"
#include "Audio/MusicManager.h"
#include "Audio/AudioMixer.h"
#include "Camera/SparkEngineCamera.h"
#include "Engine/Gameplay/WeaponManager.h"
#include "Input/InputManager.h"
#include "Engine/Gameplay/AbilitySystem.h"
#include "Engine/Gameplay/ConditionSystem.h"
#include "Engine/Gameplay/InstanceManager.h"
#include "Engine/Gameplay/InventorySystem.h"
#include "Engine/Gameplay/QuestSystem.h"
#include "Engine/AI/MovementSystem.h"
#include "Engine/Destruction/DestructionSystem.h"
#include "Engine/AI/TacticalPointSystem.h"
#include "Engine/AI/CoverSystem.h"
#include "Engine/AI/FormationSystem.h"
#include "Engine/AI/GroupAI.h"
#include "Engine/AI/CollisionAvoidance.h"
#include "Engine/AI/AIIntegration.h"
#include "Engine/Animation/AnimationSystem.h"
#include "Engine/Localization/LocalizationSystem.h"
#include "Engine/Gameplay/MaterialEffects.h"
#include "Engine/Gameplay/AchievementSystem.h"
#include "Engine/Accessibility/AccessibilitySystem.h"
#include "Engine/Dialogue/DynamicResponseSystem.h"
#include "Engine/ECS/EntityArchetype.h"
#include "Engine/Gameplay/WeatherGameplayIntegration.h"
#include "Graphics/MaterialLoader.h"
#include "Engine/World/ProximityTriggerSystem.h"
#include "Graphics/SkyAtmosphere.h"
#include "Graphics/VolumetricClouds.h"
#include "Graphics/WaterRenderer.h"
#include "Engine/SaveSystem/FreezeSystem.h"
#include "Engine/Editor/CoreComponentSerializers.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Graphics/RenderCommandRing.h"
#include "Graphics/ConstantBufferDiff.h"
#include "Graphics/GPUProfiler.h"
#include "Utils/MultiISA.h"
#include "Utils/GPUPerfCounters.h"
#include "SceneManager/SceneConfigDatabase.h"
#include "Engine/Cinematic/Sequencer.h"
#include "Engine/Replay/ReplaySystem.h"
#include "Engine/ECS/Components/GameplayComponents.h"
#include "Engine/ECS/Components/PhysicsComponents.h"
#include "Engine/ECS/Components/VolumeComponents.h"
#include "Engine/VR/VRSystem.h"
#include "Core/FaultIsolation.h"
#include "Core/FixedTimestepAccumulator.h"
#include "Core/PluginRegistry.h"
#include "Core/ResourceVersionTracker.h"
#include "Engine/AI/NavMeshLink.h"
#include "Engine/Animation/AnimNotify.h"
#include "Engine/ECS/RuntimePrefab.h"
#include "Engine/Gameplay/GameplaySystemExtension.h"
#include "Engine/Scripting/ScriptHookManager.h"
// Phase EE Theme 3D: three more SparkEngine singleton orphans
// surfaced by a deep parallel sweep. All three are low-risk pure-CPU
// utilities with default constructors and no platform guards.
#include "Engine/Gameplay/EventResponseSystem.h"
#include "Engine/ECS/EntityPresetManager.h"
#include "Core/AssetMigration.h"
// Phase FF Theme 3D: two diagnostics/analytics singletons. Both are
// Utils-folder pure-CPU classes with zero external wire-up.
#include "Utils/Telemetry.h"
#include "Utils/CacheDebugger.h"
// Phase GG Theme 3D: four more orphans — static random-number init,
// CPU section profiler, mesh LOD generator, and GPU texture
// compressor. All pure-CPU utilities with clean public APIs.
#include "Utils/MathUtilsExtended.h"
#include "Utils/CpuDebugger.h"
#include "Graphics/LODGenerator.h"
#include "Graphics/TextureCompressor.h"
// Phase HH Theme 3D: Networking data registry singleton. Servers
// register immutable datablocks at startup (weapon stats, vehicle
// configs, item templates) which clients then reference by ID,
// eliminating per-tick replication for shared configuration data.
#include "Engine/Networking/DatablockRegistry.h"
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
#include "Engine/LevelDesign/CSGSystem.h"
#include "Engine/Text/FontSystem.h"
#include "Input/InputActionSystem.h"
#include "Engine/Build/GamePackager.h"
#include "Engine/OnlineServices/OnlineServices.h"
#include "Engine/DataTable/DataTableSystem.h"
#include "Engine/Rendering/MovieRenderPipeline.h"
#include "Engine/RemoteDebug/RemoteDebugSystem.h"
#include "Engine/Crafting/LootAndCraftingSystem.h"
#include "Utils/FileWatcher/FileWatcher.h"
#include "Utils/TimerManager.h"
#include "Utils/InGameConsole.h"
#include "Engine/Modding/VirtualFileSystem.h"
#include "Engine/Modding/ArchiveResourceProvider.h"
#include "Engine/UI/UIFactory.h"
#include "Engine/Scripting/AngelScriptEngine.h"
#include "Engine/Animation/BlendSpace.h"
#include "Graphics/RHI/DXRSupport.h"
#include "Graphics/RHI/NullRHIDevice.h"
#include "Graphics/RHI/RHIValidationLayer.h"
#include "Graphics/ClusteredLightCulling.h"
#include "Graphics/ClipmapTerrain.h"
#include "Graphics/FoliageRenderer.h"
#include "Graphics/FoliageSystem.h"
#include "Graphics/MaterialPropertyHandle.h"
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
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Spark::Core::Lifecycle
{

    // ============================================================================
    // Runtime path anchoring
    // ============================================================================

    /// Per-user writable location for @p relative (Logs/, spark_trace.json, ...).
    /// Falls back to the CWD-relative form when the platform supplies no per-user
    /// directory — or when the per-user directory has no narrow spelling the
    /// engine's std::string file APIs can reopen (a non-ASCII Windows profile
    /// outside a UTF-8 code page) — so those runs keep their previous behaviour
    /// instead of writing to a '?'-mangled path that cannot be created.
    static std::filesystem::path ResolveUserDataPath(std::string_view relative)
    {
        const std::filesystem::path root = Spark::UserPaths::NarrowSafeDirectory(Spark::UserPaths::GetUserDataDir());
        return root.empty() ? std::filesystem::path(relative) : root / relative;
    }

    std::string InstallEngineLogSinksImpl()
    {
        auto& runtime = GetEngineRuntime();
        auto& logger = Spark::Logger::Get();
        if (runtime.logSinksInstalled)
        {
            // The latch alone is not proof: the Logger is a process-wide
            // singleton that anything (an editor panel, a tool, a test) can
            // ClearSinks() or Shutdown() behind this flag. Trusting the latch
            // then hands out a log path nothing is writing to, so re-check the
            // Logger itself and reinstall when it has been torn down.
            if (logger.IsInitialized() && logger.GetSinkCount() > 0)
                return runtime.engineLogPath;
            runtime.logSinksInstalled = false;
            runtime.engineLogPath.clear();
        }

        // Sinks without an initialized Logger receive nothing: Log() drops every
        // message until Initialize() has run. The platform entry points call
        // Initialize first; make the install self-sufficient for every other
        // caller (Initialize is idempotent, whichever call runs first picks the
        // mode, and the lifecycle uses synchronous logging).
        if (!logger.IsInitialized())
            logger.Initialize(/*enableAsync=*/false);

        // Logger::InstallDefaultSinks replaces every sink and opens a fresh
        // timestamped file, so it must run exactly once per process: a second
        // call within the same second would truncate the file the first one
        // opened. The early-init entry points and InitializeDebugSystemsImpl all
        // route through here; whichever runs first wins.
        Spark::Logger::SinkSetup setup;
        setup.file.directory = ResolveUserDataPath("Logs").string() + "/";
        setup.file.prefix = "SparkEngine";
        // The Logger is a leaf that standalone tools compile without the console
        // subsystem, so the SimpleConsole bridge (which feeds SparkConsole.exe) is
        // supplied here, where SparkConsole.cpp is always linked.
        runtime.engineLogPath = logger.InstallDefaultSinks(setup, std::make_unique<Spark::ConsoleSink>());

        // InstallDefaultSinks returns an empty path when the FileSink could not
        // open its file, and drops the sink. Latching "installed" on that result
        // would make the absence of the log file — the artifact the release gates
        // read — permanent and silent, so leave the flag clear: the next entry
        // point retries, and the failure is stated once here.
        runtime.logSinksInstalled = !runtime.engineLogPath.empty();
        if (!runtime.logSinksInstalled)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core,
                           "Engine file logging unavailable: could not open a log file in '%s'",
                           setup.file.directory.c_str());
        }
        return runtime.engineLogPath;
    }

    // ============================================================================
    // SparkPak auto-mount — scan for .spk files beside the executable
    // ============================================================================

    static void MountSparkPakArchives()
    {
        auto& vfs = Spark::VirtualFileSystem::GetInstance();
        auto& console = Spark::SimpleConsole::GetInstance();

        // Read-only content ships beside the executable. A -game/-manifest launch
        // from another directory must still find it, so anchor at the executable
        // first and only fall back to the CWD form for development trees.
        std::error_code ec;
        std::filesystem::path dataDir;
        const std::filesystem::path exeDir = Spark::RuntimePackage::GetExecutableDirectory();
        if (!exeDir.empty() && std::filesystem::exists(exeDir / "Data", ec))
            dataDir = exeDir / "Data";
        else
            dataDir = std::filesystem::current_path(ec) / "Data";
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

    void LogMissingModuleWarningsImpl()
    {
        auto& console = Spark::SimpleConsole::GetInstance();
        int missingCount = 0;

#ifndef SPARK_JOLT_PHYSICS_AVAILABLE
        console.LogWarning("[MISSING MODULE] Jolt Physics — rigid body simulation, collision detection, and "
                           "raycasting are DISABLED.");
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

    void InitializeDebugSystemsImpl()
    {
        // Initialize the debug hook manager first so subsequent inits can be observed
        Spark::DebugHookManager::GetInstance().SetEnabled(true);
        SPARK_DEBUG_HOOK(EnginePreInit, 0, 0.0f);

        // Initialize the unified Logger and install the engine's standard sink set
        // (stderr + rotating per-user log file + SparkConsole bridge). Platform entry
        // points may already have done both early so startup logs are captured;
        // Initialize is idempotent and InstallEngineLogSinksImpl installs once.
        auto& logger = Spark::Logger::Get();
        logger.Initialize(/*enableAsync=*/false);
        InstallEngineLogSinksImpl();

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

        Spark::ChromeTracing::GetInstance().Start();
#ifndef NDEBUG
        Spark::MemoryDebugger::GetInstance().SetEnabled(true);
#endif
        Spark::DebugOverlay::GetInstance().SetEnabled(true);
        Spark::MemoryMonitor::GetInstance().Initialize();
        Spark::HitchDetector::GetInstance().Initialize();
        Spark::BenchmarkFramework::GetInstance().Initialize();
        Spark::BenchmarkFramework::GetInstance().RegisterBuiltinScenarios();
        Spark::AssetStallDetector::GetInstance().Initialize();
        Spark::AssetValidator::GetInstance().Initialize();
        Spark::NetworkHealthMonitor::GetInstance().Initialize();
        Spark::GPUResourceLeakDetector::GetInstance().Initialize();
        Spark::Security::MemoryIntegritySystem::GetInstance().Initialize();
        Spark::InvalidStateDetector::GetInstance().Initialize();
        // Publish the host detector through the context: game-module DLLs link
        // SparkEngineLib statically, so their GetInstance() is a DLL-local copy
        // the host never ticks.
        if (auto* ctx = EngineContext::Get())
        {
            ctx->SetInvalidStateDetector(&Spark::InvalidStateDetector::GetInstance());
        }

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

        // Each engine-lifetime singleton is registered on the context right after
        // its Initialize() so IEngineContext getters hand modules the host instance
        // (a module DLL's own GetInstance() is a DLL-local copy).
        Spark::Gameplay::ConditionSystem::GetInstance().Initialize();
        ctx->SetConditions(&Spark::Gameplay::ConditionSystem::GetInstance());
        Spark::Gameplay::AbilitySystem::GetInstance().Initialize(eventBus);
        ctx->SetAbilities(&Spark::Gameplay::AbilitySystem::GetInstance());
        Spark::Gameplay::InstanceManager::GetInstance().Initialize();
        ctx->SetInstances(&Spark::Gameplay::InstanceManager::GetInstance());
        Spark::Gameplay::InventorySystem::GetInstance().Initialize();
        Spark::Gameplay::QuestSystem::GetInstance().Initialize();
        Spark::Gameplay::AchievementSystem::GetInstance().Initialize();
        Spark::Accessibility::AccessibilitySystem::GetInstance().Initialize();
        Spark::AI::MovementSystem::GetInstance().Initialize();
        Spark::Audio::MusicManager::GetInstance().Initialize();
        ctx->SetMusic(&Spark::Audio::MusicManager::GetInstance());

        // Mixer: bus volumes reach the voices only once AudioEngine knows the mixer,
        // and occlusion traces need the physics system.
        auto& mixer = Spark::Audio::AudioMixer::GetInstance();
        mixer.Initialize();
        mixer.SetPhysics(ctx->GetPhysics());
        if (auto* audio = ctx->GetAudio())
        {
            audio->SetMixer(&mixer);
        }

        // WeaponSystem is engine-owned (EngineRuntime) so GetWeapons() hands modules
        // the instance the frame loop actually ticks.
        auto& runtime = GetEngineRuntime();
        if (!runtime.weaponSystem)
        {
            runtime.weaponSystem = std::make_unique<Spark::Gameplay::WeaponSystem>();
        }
        ctx->SetWeapons(runtime.weaponSystem.get());

        auto& destruction = Spark::DestructionSystem::GetInstance();
        destruction.Initialize();
        ctx->SetDestruction(&destruction);
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

    static void InitAIAndWorldSystems(EngineContext* ctx, Spark::EventBus* eventBus)
    {
        SPARK_DEBUG_HOOK_SYSTEM(SystemPreInit, "AIAndWorldSystems", 0.0);
        Spark::AI::TacticalPointSystem::GetInstance().Initialize();
        Spark::AI::CoverSystem::GetInstance().Initialize();
        Spark::AI::FormationSystem::GetInstance().Initialize();
        Spark::AI::GroupAISystem::GetInstance().Initialize();
        Spark::AI::CollisionAvoidanceSystem::GetInstance().Initialize();
        // AIIntegratedSystem owns ParallelPerceptionSystem + NavMeshObstacleManager
        // and (optionally) the heavy AISystem pipeline. Default config keeps
        // runCoreAISystem=false so AIComponent behavior trees are ticked exactly
        // once per frame — by Spark::ECS::AIUpdateSystem, which runs in the
        // lifecycle-owned PhaseSystemManager (see InitializeEcsPhaseSystemsImpl).
        Spark::AI::AIIntegratedSystem::GetInstance().Initialize();
        ctx->SetAI(&Spark::AI::AIIntegratedSystem::GetInstance().GetAISystem());
        ctx->SetAnimation(&Spark::Animation::AnimationManager::GetInstance());
        Spark::Gameplay::MaterialEffectSystem::GetInstance().Initialize();
        Spark::Dialogue::DynamicResponseSystem::GetInstance().Initialize();
        Spark::ECS::EntityArchetypeSystem::GetInstance().Initialize();
        Spark::Gameplay::WeatherGameplayIntegration::GetInstance().Initialize(eventBus);
        Spark::World::ProximityTriggerSystem::GetInstance().Initialize();
        Spark::Graphics::SkyAtmosphereSystem::GetInstance().Initialize();
        Spark::Graphics::VolumetricCloudSystem::GetInstance().Initialize();
        Spark::Graphics::WaterRenderer::GetInstance().Initialize();
        // OcclusionCullingSystem is not initialized here: nothing submits occluders
        // or queries it, so a lifecycle Initialize/Shutdown pair would only report a
        // system as active that never runs.
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
        ctx->SetTween(&Spark::TweenSystem::GetInstance());
        Spark::VirtualFileSystem::GetInstance().Initialize();
        ctx->SetVFS(&Spark::VirtualFileSystem::GetInstance());
        MountSparkPakArchives();
        Spark::UI::UIFactory::GetInstance().Initialize();
        Spark::Graphics::ClusteredLightCulling::GetInstance().Initialize();
        // LightProbeSystem, RHI PipelineStateCache, TransientResourcePool and
        // VirtualTextureManager are deliberately not initialized here: no renderer
        // feeds or consults them (the D3D11 path uses its own D3D11PipelineStateCache),
        // so lifecycle-wiring them would only advertise dead systems as live.
        Spark::Graphics::ClipmapTerrain::GetInstance().Initialize();
        Spark::Graphics::FoliageManager::GetInstance().Initialize();
        // FoliageRenderer::Initialize with a nullptr loader. The renderer
        // will attempt to self-install an AssetPipeline-backed loader via
        // EngineContext immediately, and again on the first Collect call
        // if AssetPipeline was not yet registered at init time. We also
        // explicitly attempt to install it here with whatever AssetPipeline
        // the context has *right now* — whichever path resolves first wins.
        Spark::Graphics::FoliageRenderer::GetInstance().Initialize(nullptr, 50.0f);
        if (auto* pipeline = ctx->GetAssetPipeline())
        {
            Spark::Graphics::FoliageRenderer::GetInstance().InstallAssetPipelineLoader(pipeline);
        }
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

        // Module hot reload is owned by EngineRuntime::moduleHotReload
        // (Spark::ModuleHotReloadManager) and polled by the platform frame loops;
        // the Engine/HotReload/ModuleHotReload singleton had no registered modules
        // and is no longer part of the lifecycle.
        Spark::LevelDesign::CSGSystem::GetInstance().Initialize();
        Spark::Text::FontSystem::GetInstance().Initialize();
        Spark::Input::InputActionSystem::GetInstance().Initialize();
        // Actions can only trigger once the system can read keys: bind the providers
        // to the engine-owned InputManager (absent in headless mode).
        if (auto* input = ctx->GetInput())
        {
            Spark::Input::InputActionSystem::GetInstance().SetKeyStateProviders(
                [input](int key) { return input->IsKeyDown(key); },
                [input](int key) { return input->WasKeyPressed(key); },
                [input](int key) { return input->WasKeyReleased(key); });
        }
        Spark::Build::GamePackager::GetInstance().Initialize();
        Spark::OnlineServices::OnlineServiceManager::GetInstance().Initialize();
        Spark::Data::DataTableRegistry::GetInstance().Initialize();

        // Off-mesh navigation links (jump, climb, teleport) — pathfinding
        // consults this registry for traversal between disconnected nav
        // regions. `FoliageVolumeComponent`-style runtime bookkeeping is
        // already wired in `AdvancedPlacementComponents.h`; the singleton
        // just needs to be alive so links can be registered.
        Spark::AI::NavMeshLinkSystem::GetInstance().Initialize();

        // Runtime prefab registry — data-driven entity templates with
        // component descriptors, spawning, and binary (de)serialization.
        // Touch the singleton so later RegisterPrefab calls find it in a
        // constructed state.
        (void)Spark::ECS::PrefabRegistry::GetInstance();

        // Extension point registry for genre-specific quest / dialogue
        // behavior. Game modules (RPG, MMO, etc.) register concrete
        // implementations here; the engine QuestSystem / DialogueSystem
        // delegates to matching extensions at runtime.
        (void)Spark::Gameplay::GameplayExtensionRegistry::GetInstance();

        // Phase BB Theme 3D: Script hook dispatcher for gameplay events.
        // Touch the singleton so scripts that call RegisterHook during
        // module load find it in a constructed state. Hot-reloading a
        // script calls UnregisterAllForScript on the same singleton, so
        // having it alive from engine startup is the correct lifetime.
        (void)Spark::Scripting::ScriptHookManager::GetInstance();

        // DynamicQualityScaler, GPUStallProfiler, AsyncComputeScheduler and
        // AIDebugRenderer are not lifecycle-initialized: none of them has a
        // per-frame producer or consumer in production (RecordFrameTime,
        // BeginCPUWork, Submit/Flush and Update have zero callers), so an
        // Initialize/Shutdown pair here would present unwired utilities as live.

        // Phase EE Theme 3D: Event response rule engine — data-driven
        // "When/If/Then" gameplay logic for no-code designers. Rules
        // are attached via AddRule / LoadFromJson; Initialize
        // subscribes to the event bus. Update(dt) is pumped from the
        // gameplay update phase when rules need per-frame timing
        // (OnTimer triggers).
        Spark::Gameplay::EventResponseSystem::GetInstance().Initialize();

        // Phase EE Theme 3D: Entity preset registry — pre-configured
        // entity templates for no-code entity spawning. Initialize
        // registers built-in presets; editor panels query it for the
        // spawn menu.
        Spark::ECS::EntityPresetManager::GetInstance().Initialize();

        // Phase EE Theme 3D: Asset migration registry — manages
        // versioned migration steps for asset schema upgrades. Real
        // migration steps are registered by asset loaders before
        // Execute(asset, version) is called. Initialize clears the
        // previous state so the registry can be re-populated.
        Spark::AssetMigrationRegistry::GetInstance().Initialize();

        // Phase FF Theme 3D: Telemetry system — event recording,
        // batching, and backend dispatch. Initialized with
        // consent=false + enabled=false by default so nothing is
        // sent unless a game opts in (privacy-first). Games can
        // re-initialize with a concrete TelemetryConfig when the
        // user accepts data collection.
        if (!ctx->GetTelemetryService())
        {
            Spark::TelemetryConfig telemetryCfg;
            telemetryCfg.enabled = false;
            telemetryCfg.consentGiven = false;
            auto& telemetry = Spark::TelemetrySystem::GetInstance();
            telemetry.Initialize(telemetryCfg);
            ctx->SetTelemetryService(&telemetry);
        }

        // Phase FF Theme 3D: Cache performance debugger — tracks
        // hit/miss rates for engine caches. Lazy-initialised on
        // first GetInstance() access; enabled by default. The touch
        // here guarantees it exists for any cache that registers
        // during engine startup.
        (void)Spark::CacheDebugger::GetInstance();

        // Phase GG Theme 3D: Seed the random number generator for
        // all MathUtilsExtended::Random* functions. This is the
        // once-per-process initialization the class docs recommend.
        MathUtilsExtended::InitializeRandom();

        // Phase GG Theme 3D: CPU section profiler (begin/end
        // timed sections, aggregates statistics). Lazy singleton
        // enabled by default — the touch makes it reachable from
        // any profiling code before the first section timer fires.
        (void)Spark::CpuDebugger::GetInstance();

        // Phase GG Theme 3D: Mesh LOD generator (Quadric Error
        // Metric simplification). Lazy utility singleton — asset
        // cookers and runtime mesh optimizers call Generate() /
        // Simplify() on demand. Touch so it's reachable from any
        // mesh-loading code path.
        (void)Spark::Graphics::LODGenerator::GetInstance();

        // Phase GG Theme 3D: GPU texture compressor (BC1/BC7/ASTC).
        // Lazy utility singleton — texture loaders call
        // Compress() / SaveCompressed() on demand. Touch so the
        // instance is constructed before any asset cooker needs it.
        (void)Spark::Graphics::TextureCompressor::GetInstance();

        // Phase HH Theme 3D: Datablock registry — Torque3D-inspired
        // immutable shared-data table (sent once at connect time).
        // Servers register datablocks during startup; clients
        // deserialize on connect. Touch so the singleton exists
        // before any network handshake fires.
        (void)Spark::Net::DatablockRegistry::Get();

        Spark::Rendering::MovieRenderPipeline::GetInstance().Initialize();
        Spark::RemoteDebug::RemoteDebugSystem::GetInstance().Initialize();
        // HLODSystem: no cluster registration or per-frame Update exists in
        // production, so it is not lifecycle-initialized.
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
                ctx->SetScriptEngine(&s_angelScript);
                AngelScriptEngine::BindWorld(ctx->GetWorld());
                SPARK_LOG_INFO(Spark::LogCategory::Core, "AngelScriptEngine initialized");
            }
            else
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "AngelScriptEngine init failed — scripts disabled");
            }
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
        ctx->SetAreaStreaming(&Spark::Streaming::SeamlessAreaManager::GetInstance());
        ctx->SetLocalization(&Spark::LocalizationSystem::Get());

#ifdef ENABLE_NETWORKING
        SPARK_LOG_INFO(Spark::LogCategory::Core,
                       "Networking enabled — AreaServer, WorldServer, and DedicatedServer are available");
#endif

        ctx->SetCinematic(&Spark::Cinematic::SequencerManager::GetInstance());
        ctx->SetReplay(&Spark::ReplaySystem::GetInstance());

        // RagdollSystem and MobilePlatform are not constructed by the engine:
        // nothing owns or ticks them (wire-in-or-delete rule). Deleting the
        // classes themselves is tracked with their owners.

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

        SPARK_DEBUG_HOOK_SYSTEM(SystemPostInit, "ScriptingAndPlatform", 0.0);
    }

    void InitializeNetworkingSystemsImpl()
    {
        auto* ctx = EngineContext::Get();
        if (!ctx)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "EngineContext is null — networking init skipped");
            return;
        }

        InitNetworkingLifecycle(ctx);
    }

    Spark::ECS::PhaseSystemManager& GetPhaseSystemManagerImpl()
    {
        static Spark::ECS::PhaseSystemManager s_phaseSystems;
        return s_phaseSystems;
    }

    void InitializeEcsPhaseSystemsImpl()
    {
        auto* ctx = EngineContext::Get();
        if (!ctx)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::ECS, "EngineContext is null — ECS phase systems not registered");
            return;
        }

        // Rebuild from scratch (move-assign) so a repeated init — editor
        // restart, tests — replaces the previous set instead of accumulating
        // duplicate systems that would double-tick component data.
        GetPhaseSystemManagerImpl() = Spark::EngineSetup::CreatePhaseSystemManager(*ctx);
        SPARK_LOG_INFO(Spark::LogCategory::ECS, "ECS phase pipeline registered: %zu systems",
                       GetPhaseSystemManagerImpl().GetSystemCount());
    }

    void InitializeGameplaySystemsImpl()
    {
        auto* ctx = EngineContext::Get();
        if (!ctx)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "EngineContext is null — all gameplay systems skipped");
            return;
        }

        InitCoreGameplaySystems(ctx);

        // Register snapshot serializers for play-in-editor mode and publish the
        // host registry so module DLLs register into the one SaveSystem consults.
        Spark::Editor::RegisterCoreComponentSerializers();
        ctx->SetComponentSerializers(&Spark::ComponentSerializerRegistry::GetInstance());

        if (auto* eventBus = ctx->GetEventBus())
        {
            InitAIAndWorldSystems(ctx, eventBus);
        }
        else
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "EventBus is null — AI/World systems skipped");
        }
        InitRenderingAndUtilitySystems(ctx);
        InitScriptingAndPlatformSystems(ctx);

        // Canonical ECS phase pipeline (architecture contract, Invariant 3).
        // Registered last so the subsystem pointers CreatePhaseSystemManager
        // reads from the context (physics, audio, graphics) are all set.
        // UpdateGameplaySystemsImpl pumps UpdateAll on this manager each frame.
        InitializeEcsPhaseSystemsImpl();
    }

    // ============================================================================
    // Per-frame update
    // ============================================================================

    // The Update stage declares MainThread affinity, and none of these systems
    // is thread-safe (they publish EventBus events and are read by module
    // OnUpdate on the same frame), so they run serially on the main thread.
    static void UpdateNonECSSystems(EngineContext* ctx, float dt)
    {
        SPARK_GUARDED_UPDATE("Weather", "Core", {
            if (auto* weather = ctx->GetWeather())
                weather->Update(dt);
        });

        SPARK_GUARDED_UPDATE("TimeOfDay", "Core", { Spark::TimeOfDaySystem::GetInstance().Update(dt); });

        // WeatherGameplay depends on Weather — must run after Weather completes
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

    // ------------------------------------------------------------------------
    // TriggerVolumeComponent -> ProximityTriggerSystem bridge
    // ------------------------------------------------------------------------

    /// What the bridge created for one authored volume. ProximityTriggerSystem
    /// has no owner concept and no in-place update, so this is the only record of
    /// which triggers belong to a TriggerVolumeComponent and of the authored state
    /// they were built from.
    struct BridgedTriggerVolume
    {
        uint32_t triggerID = 0;
        TriggerVolumeComponent::Shape shape = TriggerVolumeComponent::Shape::Sphere;
        XMFLOAT3 center{0.0f, 0.0f, 0.0f};
        XMFLOAT3 halfExtents{0.0f, 0.0f, 0.0f};
        float radius = 0.0f;
        bool enabled = true;
        /// Set from the enter callback so the next tick can write the disable back
        /// into the component, which would otherwise keep claiming to be enabled.
        bool oneShotFired = false;
    };

    /// Bridge-owned triggers keyed by the volume entity. The key is the whole entt
    /// entity value, so a recycled index (which carries a new version) is a
    /// different key rather than a collision.
    static std::unordered_map<uint32_t, BridgedTriggerVolume>& BridgedTriggerVolumes()
    {
        static std::unordered_map<uint32_t, BridgedTriggerVolume> s_bridgedTriggerVolumes;
        return s_bridgedTriggerVolumes;
    }

    /// Whether @p tracked was built from the volume's current authored state.
    static bool MatchesAuthoredVolume(const BridgedTriggerVolume& tracked, const TriggerVolumeComponent& volume,
                                      const XMFLOAT3& position)
    {
        if (tracked.shape != volume.shape)
            return false;
        if (tracked.center.x != position.x || tracked.center.y != position.y || tracked.center.z != position.z)
            return false;
        if (volume.shape == TriggerVolumeComponent::Shape::AABB)
        {
            return tracked.halfExtents.x == volume.halfExtents.x && tracked.halfExtents.y == volume.halfExtents.y &&
                   tracked.halfExtents.z == volume.halfExtents.z;
        }
        return tracked.radius == volume.radius;
    }

    /// Drop every trigger this bridge created. Called at gameplay shutdown so the
    /// singleton does not carry triggers into the next initialization.
    static void RemoveBridgedTriggerVolumes()
    {
        auto& triggers = Spark::World::ProximityTriggerSystem::GetInstance();
        for (const auto& [entity, tracked] : BridgedTriggerVolumes())
            triggers.RemoveTrigger(tracked.triggerID);
        BridgedTriggerVolumes().clear();
    }

    static void UpdateProximityTriggers(EngineContext* ctx, ::World& world)
    {
        auto& triggers = Spark::World::ProximityTriggerSystem::GetInstance();
        auto& bridged = BridgedTriggerVolumes();
        auto& reg = world.GetRegistry();
        Spark::EventBus* bus = ctx->GetEventBus();

        // Retire triggers whose volume is gone — entity destroyed, component
        // removed, level unloaded. Nothing else removes them, so without this the
        // singleton accumulates triggers that keep publishing enter/exit events
        // naming an entity id entt has since handed to something unrelated.
        std::erase_if(bridged,
                      [&reg, &triggers](const std::pair<const uint32_t, BridgedTriggerVolume>& tracked)
                      {
                          const auto entity = static_cast<entt::entity>(tracked.first);
                          const TriggerVolumeComponent* volume =
                              reg.valid(entity) ? reg.try_get<TriggerVolumeComponent>(entity) : nullptr;
                          if (volume && volume->runtimeTriggerID == tracked.second.triggerID)
                              return false;
                          triggers.RemoveTrigger(tracked.second.triggerID);
                          return true;
                      });

        // Create the runtime trigger for every authored volume that has none yet,
        // and reconcile the ones that already exist: a volume that moved, was
        // resized or was toggled at runtime must not keep testing against the state
        // it was authored with. The callbacks publish TriggerEnter/ExitEvent on the
        // engine EventBus and fire the authored script event names;
        // ProximityTriggerSystem defers dispatch until its scan is complete, so a
        // one-shot volume may disable itself from inside onEnter.
        auto volumeView = reg.view<TriggerVolumeComponent, Transform>();
        for (auto entity : volumeView)
        {
            auto& volume = volumeView.get<TriggerVolumeComponent>(entity);
            const auto& transform = volumeView.get<Transform>(entity);
            const uint32_t volumeEntity = static_cast<uint32_t>(entity);

            auto tracked = bridged.find(volumeEntity);
            if (tracked != bridged.end() && (volume.runtimeTriggerID != tracked->second.triggerID ||
                                             !MatchesAuthoredVolume(tracked->second, volume, transform.position)))
            {
                // The system exposes no way to move or resize a trigger in place,
                // so rebuild it. Occupancy restarts with it: an entity standing
                // inside a volume that moves is reported as entering the new one.
                triggers.RemoveTrigger(tracked->second.triggerID);
                bridged.erase(tracked);
                tracked = bridged.end();
            }

            if (tracked == bridged.end())
            {
                const bool oneShot = volume.oneShot;
                const std::string onEnterEvent = volume.onEnterEvent;
                const std::string onExitEvent = volume.onExitEvent;
                auto onEnter = [bus, volumeEntity, oneShot, onEnterEvent](uint32_t triggerID, uint32_t entityID)
                {
                    if (bus)
                        bus->Publish(Spark::TriggerEnterEvent{entityID, volumeEntity});
                    // The authored script event: without this the inspector field
                    // is a knob the editor advertises and nothing reads.
                    if (!onEnterEvent.empty())
                        Spark::Gameplay::EventResponseSystem::GetInstance().FireCustomEvent(onEnterEvent, entityID);
                    if (oneShot)
                    {
                        Spark::World::ProximityTriggerSystem::GetInstance().EnableTrigger(triggerID, false);
                        auto& volumes = BridgedTriggerVolumes();
                        if (auto fired = volumes.find(volumeEntity); fired != volumes.end())
                        {
                            fired->second.enabled = false;
                            fired->second.oneShotFired = true;
                        }
                    }
                };
                auto onExit = [bus, volumeEntity, onExitEvent](uint32_t /*triggerID*/, uint32_t entityID)
                {
                    if (bus)
                        bus->Publish(Spark::TriggerExitEvent{entityID, volumeEntity});
                    if (!onExitEvent.empty())
                        Spark::Gameplay::EventResponseSystem::GetInstance().FireCustomEvent(onExitEvent, entityID);
                };
                const uint32_t triggerID =
                    (volume.shape == TriggerVolumeComponent::Shape::AABB)
                        ? triggers.CreateAABBTrigger(transform.position, volume.halfExtents, std::move(onEnter),
                                                     std::move(onExit))
                        : triggers.CreateSphereTrigger(transform.position, volume.radius, std::move(onEnter),
                                                       std::move(onExit));
                volume.runtimeTriggerID = triggerID;

                BridgedTriggerVolume record;
                record.triggerID = triggerID;
                record.shape = volume.shape;
                record.center = transform.position;
                record.halfExtents = volume.halfExtents;
                record.radius = volume.radius;
                record.enabled = volume.enabled;
                if (!volume.enabled)
                    triggers.EnableTrigger(triggerID, false);
                bridged.emplace(volumeEntity, record);
            }
            else if (tracked->second.oneShotFired)
            {
                // A fired one-shot disabled its trigger from inside the callback;
                // make the component say so instead of advertising enabled = true.
                volume.enabled = false;
                tracked->second.oneShotFired = false;
            }
            else if (tracked->second.enabled != volume.enabled)
            {
                triggers.EnableTrigger(tracked->second.triggerID, volume.enabled);
                tracked->second.enabled = volume.enabled;
            }
        }

        if (triggers.GetTotalTriggerCount() == 0)
            return;

        // Every positioned entity that is not itself a trigger volume is a candidate.
        // The buffer is reused across frames: this scan runs on the main thread
        // every frame and a fresh allocation each time is pure overhead.
        static std::vector<Spark::World::EntityPosition> positions;
        positions.clear();
        auto view = reg.view<Transform>();
        positions.reserve(static_cast<size_t>(view.size()));
        for (auto entity : view)
        {
            if (reg.any_of<TriggerVolumeComponent>(entity))
                continue;
            positions.push_back({static_cast<uint32_t>(entity), view.get<Transform>(entity).position});
        }
        triggers.Update(positions);
    }

    // ------------------------------------------------------------------------
    // Replay capture: the engine-side RecordFrame producer
    // ------------------------------------------------------------------------

    static void CaptureReplayFrame(::World& world, float dt)
    {
        auto& replay = Spark::ReplaySystem::GetInstance();
        if (!replay.IsRecording())
            return;

        // Reused scratch: this scan walks every Transform entity on the main thread
        // for every recorded frame.
        static std::vector<Spark::ReplayEntityState> states;
        states.clear();
        auto& reg = world.GetRegistry();
        auto view = reg.view<Transform>();
        states.reserve(static_cast<size_t>(view.size()));
        for (auto entity : view)
        {
            const auto& transform = view.get<Transform>(entity);
            Spark::ReplayEntityState state;
            state.entityId = static_cast<uint32_t>(entity);
            state.position = transform.position;
            // Transform::rotation is Euler degrees (pitch, yaw, roll).
            const XMVECTOR q = XMQuaternionRotationRollPitchYaw(XMConvertToRadians(transform.rotation.x),
                                                                 XMConvertToRadians(transform.rotation.y),
                                                                 XMConvertToRadians(transform.rotation.z));
            XMStoreFloat4(&state.rotation, q);
            if (const auto* rb = reg.try_get<RigidBodyComponent>(entity))
                state.velocity = rb->linearVelocity;
            if (const auto* health = reg.try_get<HealthComponent>(entity))
                state.health = health->health;
            state.flags = Spark::ReplayFlags::Alive;
            states.push_back(state);
        }
        // ReplaySystem owns the capture clock (StartRecording resets it) and
        // applies the configured record interval itself; this producer only
        // supplies the tick's delta and the entity states.
        replay.RecordFrameTick(states, dt);
    }

    static void UpdateECSDependentSystems(EngineContext* ctx, ::World* world, float dt)
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

        // AIIntegratedSystem owns the parallel perception spatial index and
        // (optionally) the heavy AISystem pipeline. Default config skips the
        // inner AISystem so behavior trees aren't double-ticked alongside the
        // ECS AIUpdateSystem, which runs in the phase pipeline pumped at the
        // end of UpdateGameplaySystemsImpl.
        SPARK_GUARDED_UPDATE("AIIntegrated", "Core", {
            SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "AIIntegrated", 0.0);
            Spark::AI::AIIntegratedSystem::GetInstance().Update(*world, dt);
            SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "AIIntegrated", 0.0);
        });

        SPARK_GUARDED_UPDATE("Coroutine", "Core", { Spark::CoroutineScheduler::GetInstance().Update(dt); });

        SPARK_GUARDED_UPDATE("MusicManager", "Core", { Spark::Audio::MusicManager::GetInstance().Update(dt); });

        // 3D listener follows the active camera; the mixer's occlusion/reverb
        // queries use the same listener position instead of a fixed origin.
        SPARK_GUARDED_UPDATE("AudioListener", "Core", {
            auto* audio = ctx->GetAudio();
            auto* camera = ctx->GetCamera();
            if (audio && camera)
            {
                audio->SetListenerFromCamera(camera->GetPosition(), camera->GetForward(), {0.0f, 1.0f, 0.0f}, dt);
            }
        });
        SPARK_GUARDED_UPDATE("AudioMixer", "Core", {
            auto* audio = ctx->GetAudio();
            Spark::Audio::AudioMixer::GetInstance().Update(
                audio ? audio->GetListenerPosition() : DirectX::XMFLOAT3{0.0f, 0.0f, 0.0f}, dt);
        });
        SPARK_GUARDED_UPDATE("Accessibility", "Core",
                             { Spark::Accessibility::AccessibilitySystem::GetInstance().Update(dt); });

        SPARK_GUARDED_UPDATE("WeaponSystem", "Core", {
            if (auto* weapons = ctx->GetWeapons())
                weapons->Update(dt);
        });

        SPARK_GUARDED_UPDATE("ProximityTriggers", "Core", { UpdateProximityTriggers(ctx, *world); });

        SPARK_GUARDED_UPDATE("ReplayCapture", "Core", { CaptureReplayFrame(*world, dt); });

        SPARK_GUARDED_UPDATE("Destruction", "Core", {
            SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "Destruction", 0.0);
            auto& destruction = Spark::DestructionSystem::GetInstance();
            destruction.SetWorld(world);
            destruction.Update(dt);
            SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "Destruction", 0.0);
        });

        // TerrainSystem is ticked by the ECS phase pipeline (Phase::PreRender,
        // see CreatePhaseSystemManager) — no ad-hoc tick here, or terrain
        // streaming/LOD state would advance twice per frame.

        SPARK_GUARDED_UPDATE("Foliage", "Core", {
            auto& foliage = Spark::Graphics::FoliageManager::GetInstance();
            foliage.UpdateFromECS(*world, dt);
            XMFLOAT3 camPos{0.0f, 0.0f, 0.0f};
            if (const auto* ctx = EngineContext::Get())
            {
                if (const auto* graphics = ctx->GetGraphics())
                {
                    camPos = graphics->GetFrameCameraPosition();
                }
            }
            foliage.Update(dt, camPos);

            // Rebuild the render-side batch after the manager's visibility
            // has been refreshed so the next frame can push instances into
            // the GPU scene buffer on Windows builds.
            Spark::Graphics::FoliageRenderer::GetInstance().CollectFromFoliageManager(dt);
        });

        SPARK_GUARDED_UPDATE("AI_Tactical", "Core", {
            SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "AI_Tactical", 0.0);
            Spark::AI::FormationSystem::GetInstance().Update(dt);
            Spark::AI::GroupAISystem::GetInstance().Update(dt);
            SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "AI_Tactical", 0.0);
        });

        SPARK_GUARDED_UPDATE("DynamicResponse", "Core",
                             { Spark::Dialogue::DynamicResponseSystem::GetInstance().Update(dt); });

        SPARK_GUARDED_UPDATE("SkyAtmosphere", "Core",
                             { Spark::Graphics::SkyAtmosphereSystem::GetInstance().Update(dt); });
        SPARK_GUARDED_UPDATE("VolumetricClouds", "Core",
                             { Spark::Graphics::VolumetricCloudSystem::GetInstance().Update(dt); });

        SPARK_GUARDED_UPDATE("WaterRenderer", "Core", { Spark::Graphics::WaterRenderer::GetInstance().Update(dt); });
    }

    static void UpdateClusteredLighting(::World* world)
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

    // Main thread only: TweenSystem runs user callbacks and has no
    // synchronization, and the Update stage declares MainThread affinity.
    static void UpdateExtendedSystems(EngineContext* ctx, float dt)
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
        Spark::Cinematic::SequencerManager::GetInstance().DispatchPendingAudioCues();
        SPARK_GUARDED_UPDATE("Replay", "Core", { Spark::ReplaySystem::GetInstance().UpdatePlayback(dt); });
        SPARK_GUARDED_UPDATE("VR", "Core", {
            if (auto* vr = ctx->GetVR())
                vr->UpdateTracking();
        });

        SPARK_GUARDED_UPDATE("GameplayDebugger", "Core", { Spark::Utils::GameplayDebugger::GetInstance().Update(dt); });
        SPARK_GUARDED_UPDATE("VideoPlayer", "Core", { Spark::Cinematic::VideoPlayer::GetInstance().Update(dt); });
    }

    static uint64_t g_frameCounter = 0;

    uint64_t GetGameplayFrameCountImpl()
    {
        return g_frameCounter;
    }

    void UpdateGameplaySystemsImpl(float dt)
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

        SPARK_GUARDED_UPDATE("InputActions", "Core", { Spark::Input::InputActionSystem::GetInstance().Update(); });
        SPARK_GUARDED_UPDATE("OnlineServices", "Core",
                             { Spark::OnlineServices::OnlineServiceManager::GetInstance().Update(dt); });
        SPARK_GUARDED_UPDATE("MovieRender", "Core",
                             { Spark::Rendering::MovieRenderPipeline::GetInstance().Update(dt); });
        SPARK_GUARDED_UPDATE("RemoteDebug", "Core",
                             { Spark::RemoteDebug::RemoteDebugSystem::GetInstance().Update(dt); });
        SPARK_GUARDED_UPDATE("Crafting", "Core", { Spark::Gameplay::CraftingSystem::GetInstance().Update(dt); });
        SPARK_GUARDED_UPDATE("FileWatcher", "Core", { Spark::Utils::FileWatcher::GetInstance().Update(dt); });
        SPARK_GUARDED_UPDATE("TimerManager", "Core", { Spark::TimerManager::GetInstance().Update(dt); });
        SPARK_GUARDED_UPDATE("EventResponse", "Core",
                             { Spark::Gameplay::EventResponseSystem::GetInstance().Update(dt); });
        SPARK_GUARDED_UPDATE("InGameConsole", "Core", { Spark::InGameConsole::GetInstance().Update(dt); });
        if (auto* telemetry = ctx->GetTelemetryService())
            SPARK_GUARDED_UPDATE("Telemetry", "Core", { telemetry->Update(dt); });

        UpdateNonECSSystems(ctx, dt);

        auto* world = ctx->GetWorld();
        if (!world)
            return;

        UpdateECSDependentSystems(ctx, world, dt);

        Spark::Graphics::ConstantBufferDiffManager::GetInstance().BeginFrame();
        Spark::Graphics::GPUProfiler::GetInstance().EndFrame();
        Spark::Graphics::GPUPerfCounters::GetInstance().EndFrame();

        UpdateClusteredLighting(world);
        UpdateExtendedSystems(ctx, dt);

        // Canonical ECS phase pipeline: Physics -> Animation -> AI -> Audio ->
        // Gameplay -> PreRender -> Render, serially in Phase enum order
        // (architecture contract, Invariant 3). This is the ONLY tick site for
        // the Spark::ECS phase systems — Tests/harden/Test_lifecycle_ecs_phase_wiring.cpp
        // guards the registration; do not remove this pump without replacing it.
        SPARK_GUARDED_UPDATE("ECS_Phases", "Core", { GetPhaseSystemManagerImpl().UpdateAll(*world, dt); });

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
        Spark::Graphics::WaterRenderer::GetInstance().Shutdown();
        Spark::Graphics::SkyAtmosphereSystem::GetInstance().Shutdown();
        Spark::Graphics::VolumetricCloudSystem::GetInstance().Shutdown();
        Spark::World::ProximityTriggerSystem::GetInstance().Shutdown();
        Spark::Gameplay::WeatherGameplayIntegration::GetInstance().Shutdown();
        Spark::Graphics::MaterialLoader::GetInstance().Shutdown();
        Spark::ECS::EntityArchetypeSystem::GetInstance().Shutdown();
        Spark::Dialogue::DynamicResponseSystem::GetInstance().Shutdown();
        Spark::Gameplay::MaterialEffectSystem::GetInstance().Shutdown();
        Spark::AI::AIIntegratedSystem::GetInstance().Shutdown();
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
        Spark::Graphics::FoliageRenderer::GetInstance().Shutdown();
        Spark::Graphics::FoliageManager::GetInstance().Shutdown();
        Spark::Graphics::ClipmapTerrain::GetInstance().Shutdown();
        Spark::Graphics::MaterialPropertyRegistry::GetInstance().Shutdown();
        Spark::Graphics::ClusteredLightCulling::GetInstance().Shutdown();
        Spark::UI::UIFactory::GetInstance().Shutdown();
        Spark::VirtualFileSystem::GetInstance().Shutdown();
        Spark::TweenSystem::GetInstance().Shutdown();
        Spark::Net::InstabilitySimulator::GetInstance().Shutdown();
        Spark::Net::DeltaSnapshotManager::GetInstance().Shutdown();
        Spark::ResourceVersionTracker::GetInstance().Shutdown();
        Spark::FixedTimestepAccumulator::GetInstance().Shutdown();
    }

    void ShutdownGameplaySystemsImpl()
    {
        auto* ctx = EngineContext::Get();
        if (ctx)
        {
            if (auto* vr = ctx->GetVR())
                vr->Shutdown();
        }

        // Drop the ECS phase systems first: they hold non-owning pointers to
        // physics/audio/graphics, which the platform layer tears down next.
        GetPhaseSystemManagerImpl() = Spark::ECS::PhaseSystemManager{};

        // Unregister the engine-lifetime services published at init BEFORE the
        // singletons behind them are torn down: between the Shutdown* calls and
        // this block, GetVFS()/GetAI()/GetAreaStreaming()/GetTween() would hand a
        // module or console command a shut-down singleton — exactly what this
        // block exists to prevent. The list is the inverse of the init path.
        if (ctx)
        {
            if (auto* audio = ctx->GetAudio())
                audio->SetMixer(nullptr);
            ctx->SetConditions(nullptr);
            ctx->SetAbilities(nullptr);
            ctx->SetInstances(nullptr);
            ctx->SetMusic(nullptr);
            ctx->SetDestruction(nullptr);
            ctx->SetWeapons(nullptr);
            ctx->SetAI(nullptr);
            ctx->SetAnimation(nullptr);
            ctx->SetTween(nullptr);
            ctx->SetVFS(nullptr);
            ctx->SetAreaStreaming(nullptr);
            ctx->SetLocalization(nullptr);
            ctx->SetCinematic(nullptr);
            ctx->SetReplay(nullptr);
            ctx->SetComponentSerializers(nullptr);
        }

        Spark::Streaming::SeamlessAreaManager::GetInstance().Shutdown();
        Spark::Net::ConnectionScopeFilter::GetInstance().Shutdown();

        // Retire the triggers the TriggerVolumeComponent bridge created before the
        // system that owns them goes away, so a later initialization does not
        // inherit them.
        RemoveBridgedTriggerVolumes();

        ShutdownAIAndWorldSystems();
        ShutdownRenderingAndUtilitySystems();

        GetEngineRuntime().weaponSystem.reset();
        Spark::Audio::AudioMixer::GetInstance().SetPhysics(nullptr);
        Spark::Audio::AudioMixer::GetInstance().Shutdown();

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
        Spark::LevelDesign::CSGSystem::GetInstance().Shutdown();
        Spark::Text::FontSystem::GetInstance().Shutdown();
        // Drop the InputManager-bound providers before the platform layer frees it.
        Spark::Input::InputActionSystem::GetInstance().SetKeyStateProviders({}, {}, {});
        Spark::Input::InputActionSystem::GetInstance().Shutdown();
        Spark::Build::GamePackager::GetInstance().Shutdown();
        Spark::InGameConsole::GetInstance().Shutdown();
        Spark::TimerManager::GetInstance().Shutdown();
        Spark::Utils::FileWatcher::GetInstance().Shutdown();
        Spark::Gameplay::CraftingSystem::GetInstance().Shutdown();
        Spark::Gameplay::LootTableManager::GetInstance().Shutdown();
        Spark::RemoteDebug::RemoteDebugSystem::GetInstance().Shutdown();
        Spark::Rendering::MovieRenderPipeline::GetInstance().Shutdown();
        Spark::Data::DataTableRegistry::GetInstance().Shutdown();

        // Engine-orphan singletons wired in this branch — teardown
        // mirrors the startup order so dependents are released first.
        // Phase HH Theme 3D additions:
        Spark::Net::DatablockRegistry::Get().Clear();
        // Phase FF Theme 3D additions:
        Spark::CacheDebugger::GetInstance().Reset();
        if (ctx && ctx->GetTelemetryService())
        {
            ctx->GetTelemetryService()->Shutdown();
            ctx->SetTelemetryService(nullptr);
        }
        else
        {
            auto& telemetry = Spark::TelemetrySystem::GetInstance();
            if (telemetry.IsInitialized())
                telemetry.Shutdown();
        }
        // Phase EE Theme 3D additions (released first — they hold no
        // GPU handles so the order relative to render-loop teardown
        // does not matter). EntityPresetManager has no explicit
        // Shutdown API because it owns only pure in-memory data;
        // the process-exit destructor handles cleanup.
        Spark::AssetMigrationRegistry::GetInstance().Shutdown();
        Spark::Gameplay::EventResponseSystem::GetInstance().Shutdown();
        Spark::Scripting::ScriptHookManager::GetInstance().Clear();
        Spark::Gameplay::GameplayExtensionRegistry::GetInstance().Clear();
        Spark::AI::NavMeshLinkSystem::GetInstance().Shutdown();

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

        Profiler::GetInstance().Shutdown();

#ifdef SPARK_HARDWARE_RT
        Spark::Graphics::DXRManager::GetInstance().Shutdown();
#endif

#ifndef NDEBUG
        Spark::Graphics::RHIValidationLayer::GetInstance().Shutdown();
#endif
    }

    void UpdateDebugSystemsImpl(float dt)
    {
        SPARK_GUARDED_UPDATE("TweenManager", "Debug", { Spark::TweenManager::GetInstance().Update(dt); });
        Spark::DebugDrawManager::GetInstance().Flush(dt);
        SPARK_GUARDED_UPDATE("DebugOverlay", "Debug", { Spark::DebugOverlay::GetInstance().Update(dt); });
        SPARK_GUARDED_UPDATE("MemoryMonitor", "Debug", { Spark::MemoryMonitor::GetInstance().Update(dt); });
        SPARK_GUARDED_UPDATE("HitchDetector", "Debug", { Spark::HitchDetector::GetInstance().Update(dt); });
        SPARK_GUARDED_UPDATE("AssetStallDetector", "Debug", { Spark::AssetStallDetector::GetInstance().Update(dt); });
        SPARK_GUARDED_UPDATE("NetworkHealthMonitor", "Debug",
                             { Spark::NetworkHealthMonitor::GetInstance().Update(dt); });
        SPARK_GUARDED_UPDATE("GPUResourceLeakDetector", "Debug",
                             { Spark::GPUResourceLeakDetector::GetInstance().Update(dt); });
        SPARK_GUARDED_UPDATE("MemoryIntegrity", "Security",
                             { Spark::Security::MemoryIntegritySystem::GetInstance().Update(dt); });
        SPARK_GUARDED_UPDATE("InvalidStateDetector", "Debug",
                             { Spark::InvalidStateDetector::GetInstance().Update(dt); });
        Spark::FrameInspector::GetInstance().OnFrameEnd();

        // Update decal fading
        SPARK_GUARDED_UPDATE("DecalSystem", "Debug", { Spark::Graphics::DecalSystem::GetInstance().Update(dt); });
    }

    void ShutdownDebugSystemsImpl()
    {
        SPARK_DEBUG_HOOK_SYSTEM(SystemPreShutdown, "DebugSystems", 0.0);

        Spark::Graphics::DecalSystem::GetInstance().Shutdown();

        Spark::TweenManager::GetInstance().KillAll();
        Spark::DebugDrawManager::GetInstance().Clear();
        Spark::MemoryMonitor::GetInstance().Shutdown();
        Spark::HitchDetector::GetInstance().Shutdown();
        Spark::BenchmarkFramework::GetInstance().Shutdown();
        Spark::AssetStallDetector::GetInstance().Shutdown();
        Spark::AssetValidator::GetInstance().Shutdown();
        Spark::NetworkHealthMonitor::GetInstance().Shutdown();
        Spark::GPUResourceLeakDetector::GetInstance().Shutdown();
        Spark::Security::MemoryIntegritySystem::GetInstance().Shutdown();
        if (auto* ctx = EngineContext::Get())
        {
            ctx->SetInvalidStateDetector(nullptr);
        }
        Spark::InvalidStateDetector::GetInstance().Shutdown();
#ifndef NDEBUG
        Spark::MemoryDebugger::GetInstance().PrintLeakReport();
#endif
        Spark::ChromeTracing::GetInstance().SaveToFile(ResolveUserDataPath("spark_trace.json").string());
        Spark::ChromeTracing::GetInstance().Stop();

        SPARK_DEBUG_HOOK_SYSTEM(SystemPostShutdown, "DebugSystems", 0.0);
    }

} // namespace Spark::Core::Lifecycle
