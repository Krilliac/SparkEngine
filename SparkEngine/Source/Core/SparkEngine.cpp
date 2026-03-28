/**
 * @file SparkEngine.cpp
 * @brief SparkEngine executable entry point - loads game modules dynamically
 *
 * SparkEngine is the runtime host. It creates the window, initializes engine
 * systems (graphics, input, timer), then loads game modules via ModuleManager.
 * Modules implement IModule (or the legacy IGameModule) and provide all
 * game-specific logic.
 *
 * Architecture: Engine (exe) -> loads -> Module DLLs (via ModuleManager)
 * Similar to Unreal Engine's module loading or Unity's player runtime.
 */

#include "Platform.h"

// On Windows, framework.h must come before any header that uses Win32 types
// (HINSTANCE, HMODULE, HWND, etc.) because it pulls in <windows.h>.
#ifdef SPARK_PLATFORM_WINDOWS
#include "framework.h"
#endif

// ============================================================================
// Common includes (shared between all platforms)
// ============================================================================
#include "SparkEngine.h"
#include "ModuleManager.h"
#include "EngineContext.h"
#include "EngineSettings.h"
#include "EngineConsoleCommands.h"
#include "Engine/Events/EventSystem.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Engine/Coroutine/CoroutineScheduler.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/GraphicsConsoleCommands.h"
#include "Input/InputManager.h"
#include "Audio/AudioEngine.h"
#include "Utils/Timer.h"
#include "Utils/SparkConsole.h"
#include "Utils/ConsoleProcessManager.h"
#include "EngineSetup.h"
#include "AssetIntegration.h"
#include "Graphics/WeatherSystem.h"
#include "Engine/World/TimeOfDaySystem.h"
#include "Engine/UI/UISystem.h"
#include "Engine/Dialogue/DialogueSystem.h"
#include "Engine/Modding/ModSystem.h"
#include "ModuleHotReload.h"
#include "Utils/ChromeTracing.h"
#include "Utils/MemoryDebugger.h"
#include "Utils/MemoryMonitor.h"
#include "Utils/FrameInspector.h"
#include "Utils/Tween.h"
#include "Utils/DebugDraw.h"
#include "Utils/DebugHookManager.h"
#include "Utils/DebugOverlay.h"
#include "Utils/FileLogger.h"
#include "Utils/Logger.h"
#include "Utils/JobSystem.h"
#include "Graphics/DecalSystem.h"
#include "Graphics/MeshLOD.h"
#include "Audio/MusicManager.h"
#include "Engine/Gameplay/WeaponManager.h"
#include "Engine/Gameplay/AbilitySystem.h"
#include "Engine/Gameplay/ConditionSystem.h"
#include "Engine/Gameplay/InstanceManager.h"
#include "Engine/AI/MovementSystem.h"
#include "Engine/Destruction/DestructionSystem.h"
#include "Engine/ECS/Systems/TerrainSystem.h"
#include "Graphics/TerrainRenderer.h"
#include "Engine/Networking/ClientPrediction.h"
#include "Engine/Networking/ConnectionScopeFilter.h"
#include "Engine/Streaming/SeamlessAreaManager.h"
#include "Engine/ECS/Systems/ParallelSystemExecutor.h"
#include "Engine/AI/TacticalPointSystem.h"
#include "Engine/AI/CoverSystem.h"
#include "Engine/AI/FormationSystem.h"
#include "Engine/AI/GroupAI.h"
#include "Engine/AI/CollisionAvoidance.h"
#include "Engine/Gameplay/MaterialEffects.h"
#include "Engine/Dialogue/DynamicResponseSystem.h"
#include "Engine/ECS/EntityArchetype.h"
#include "Engine/ECS/EntityArchetypeLoader.h"
#include "Engine/Gameplay/WeatherGameplayIntegration.h"
#include "Graphics/MaterialLoader.h"
#include "Engine/World/ProximityTriggerSystem.h"
#include "Graphics/SkyAtmosphere.h"
#include "Graphics/WaterRenderer.h"
#include "Graphics/OcclusionCulling.h"
// Serialization, rendering, and utility systems
#include "Engine/SaveSystem/FreezeSystem.h"
#include "Graphics/RenderCommandRing.h"
#include "Graphics/ConstantBufferDiff.h"
#include "Graphics/DirtyRectTracker.h"
#include "Utils/MultiISA.h"
#include "Utils/GPUPerfCounters.h"
#include "Utils/LockFreeRingAllocator.h"
#include "SceneManager/SceneConfigDatabase.h"
// Cinematic, replay, VR, mobile, ragdoll systems
#include "Engine/Cinematic/Sequencer.h"
#include "Engine/Replay/ReplaySystem.h"
#include "Engine/VR/VRSystem.h"
#include "Engine/Animation/RagdollSystem.h"
#include "Engine/Mobile/MobilePlatform.h"

// Extended engine systems
#include "FaultIsolation.h"
#include "FixedTimestepAccumulator.h"
#include "PluginRegistry.h"
#include "ResourceVersionTracker.h"
#include "Utils/ProfileProperties.h"
#include "Engine/Networking/DeltaSnapshotManager.h"
#include "Engine/Networking/InstabilitySimulator.h"
#ifdef ENABLE_NETWORKING
#include "Engine/Networking/AreaServer.h"
#include "Engine/Networking/WorldServer.h"
#include "Engine/Networking/DedicatedServer.h"
#endif
#include "Engine/Tween/TweenSystem.h"
#include "Engine/Modding/VirtualFileSystem.h"
#include "Engine/UI/UIFactory.h"
#include "Engine/Scripting/AngelScriptEngine.h"
#include "Engine/Scripting/LuaScriptEngine.h"
#include "Engine/Animation/BlendSpace.h"
#include "Utils/Profiler.h"
#include "Graphics/RHI/DXRSupport.h"
#include "Graphics/RHI/NullRHIDevice.h"
#include "Graphics/RHI/RHIValidationLayer.h"
#include "Graphics/ClusteredLightCulling.h"
#include "Graphics/LightProbeSystem.h"
#include "Graphics/ClipmapTerrain.h"
#include "Graphics/VirtualTexture.h"
#include "Engine/ECS/Components/LightComponents.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Graphics/MaterialPropertyHandle.h"
#include "Graphics/RHI/PipelineStateCache.h"
#include "Graphics/RenderGraph/TransientResourcePool.h"
#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
#include "Physics/PhysicsSystem.h"
#endif

#include <atomic>
#include <memory>
#include <sstream>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <thread>

// Platform-specific includes
#ifdef SPARK_PLATFORM_WINDOWS
#include "Utils/Assert.h"
#include "Utils/SparkError.h"
#include "Utils/Validate.h"
#include "Utils/DeltaSmoother.h"
#include "Utils/CrashHandler.h"
#include "Utils/D3DUtils.h"
#include "Utils/LocalFileCache.h"
#else
#include <csignal>
#include <cstring>
#ifdef SPARK_SDL2_AVAILABLE
#include <SDL.h>
#endif
#endif // SPARK_PLATFORM_WINDOWS

// ============================================================================
// Common globals (shared between all platforms)
// ============================================================================
std::unique_ptr<GraphicsEngine> g_graphics;
std::unique_ptr<InputManager> g_input;
std::unique_ptr<Timer> g_timer;
std::unique_ptr<Spark::EventBus> g_eventBus;
std::unique_ptr<ModuleManager> g_moduleManager;
std::unique_ptr<AudioEngine> g_audioEngine;
std::unique_ptr<Spark::ModuleHotReloadManager> g_moduleHotReload;
#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
std::unique_ptr<PhysicsSystem> g_physicsOwned;
#endif

// ============================================================================
// Common helper functions (shared across all startup paths)
// ============================================================================

static void LogMissingModuleWarnings()
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

static void InitDebugSystems()
{
    // Initialize the debug hook manager first so subsequent inits can be observed
    Spark::DebugHookManager::GetInstance().SetEnabled(true);
    SPARK_DEBUG_HOOK(EnginePreInit, 0, 0.0f);

    // Initialize the unified Logger with a stderr sink so SPARK_LOG_* output is visible
    auto& logger = Spark::Logger::Get();
    logger.Initialize(/*enableAsync=*/false);
    logger.AddSink(std::make_unique<Spark::StderrSink>());

    Spark::FileLogger::GetInstance().Initialize("Logs");
    Spark::ChromeTracing::GetInstance().Start();
#ifndef NDEBUG
    Spark::MemoryDebugger::GetInstance().SetEnabled(true);
#endif
    Spark::DebugOverlay::GetInstance().SetEnabled(true);
    Spark::MemoryMonitor::GetInstance().Initialize();
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
// Gameplay system lifecycle (TC-inspired systems from this session)
// ============================================================================

static void InitCoreGameplaySystems(EngineContext* ctx)
{
    SPARK_DEBUG_HOOK_SYSTEM(SystemPreInit, "CoreGameplaySystems", 0.0);
    auto* eventBus = ctx->GetEventBus();

    Spark::Gameplay::ConditionSystem::GetInstance().Initialize();
    Spark::Gameplay::AbilitySystem::GetInstance().Initialize(eventBus);
    Spark::Gameplay::InstanceManager::GetInstance().Initialize();
    Spark::AI::MovementSystem::GetInstance().Initialize();
    Spark::Audio::MusicManager::GetInstance().Initialize();

    auto& destruction = Spark::DestructionSystem::GetInstance();
    destruction.Initialize();
    if (auto* world = ctx->GetWorld())
    {
        destruction.SetWorld(world);
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

static void InitRenderingAndUtilitySystems()
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
    Spark::UI::UIFactory::GetInstance().Initialize();
    Spark::Graphics::ClusteredLightCulling::GetInstance().Initialize();
    Spark::Graphics::LightProbeSystem::GetInstance().Initialize();
    Spark::Graphics::PipelineStateCache::GetInstance().Initialize();
    Spark::Graphics::TransientResourcePool::GetInstance().Initialize();
    Spark::Graphics::ClipmapTerrain::GetInstance().Initialize();
    Spark::Graphics::VirtualTextureManager::GetInstance().Initialize();
    Spark::PluginRegistry::InitializeAll();
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

#ifdef ENABLE_DXR
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

static void InitGameplaySystems()
{
    auto* ctx = EngineContext::Get();
    if (!ctx)
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "EngineContext is null — all gameplay systems skipped");
        return;
    }

    InitCoreGameplaySystems(ctx);
    InitAIAndWorldSystems(ctx->GetEventBus());
    InitRenderingAndUtilitySystems();
    InitScriptingAndPlatformSystems(ctx);
}

/**
 * @brief Per-frame update for all gameplay subsystems.
 *
 * Called after module Update (which runs the ECS system pipeline: Physics ->
 * Animation -> AI -> Audio -> Lifecycle -> Render) so gameplay systems see
 * the latest ECS state. Non-ECS systems update first (Weather, Dialogue, UI),
 * then ECS-dependent systems (AbilitySystem, InstanceManager, etc.).
 */
static void UpdateNonECSSystems(EngineContext* ctx, float dt)
{
    SPARK_GUARDED_UPDATE("Weather", "Core", {
        SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "Weather", 0.0);
        if (auto* weather = ctx->GetWeather())
            weather->Update(dt);
        SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "Weather", 0.0);
    });

    SPARK_GUARDED_UPDATE("TimeOfDay", "Core", {
        SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "TimeOfDay", 0.0);
        Spark::TimeOfDaySystem::GetInstance().Update(dt);
        SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "TimeOfDay", 0.0);
    });

    SPARK_GUARDED_UPDATE("WeatherGameplay", "Core", {
        auto* weather = ctx->GetWeather();
        auto* physics = ctx->GetPhysics();
        if (weather && physics)
        {
            Spark::Gameplay::WeatherGameplayIntegration::GetInstance().Update(dt, weather, physics);
        }
    });

    SPARK_GUARDED_UPDATE("Dialogue", "Core", {
        SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "Dialogue", 0.0);
        if (auto* dialogue = ctx->GetDialogue())
            dialogue->Update(dt);
        SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "Dialogue", 0.0);
    });

    SPARK_GUARDED_UPDATE("UI", "Core", {
        SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "UI", 0.0);
        if (auto* ui = ctx->GetUI())
            ui->Update(dt);
        SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "UI", 0.0);
    });
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

        if (g_graphics)
        {
            XMFLOAT4X4 viewMat, projMat;
            XMStoreFloat4x4(&viewMat, g_graphics->GetFrameViewMatrix());
            XMStoreFloat4x4(&projMat, g_graphics->GetFrameProjectionMatrix());
            clustering.Update(viewMat, projMat, g_graphics->GetNearPlane(), g_graphics->GetFarPlane());
        }
    });
}

static void UpdateExtendedSystems(EngineContext* ctx, float dt)
{
    SPARK_GUARDED_UPDATE("SeamlessArea", "Core", { Spark::Streaming::SeamlessAreaManager::GetInstance().Update(dt); });

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

    SPARK_GUARDED_UPDATE("ECS_Executor", "Core", { Spark::ECS::StageBasedExecutor::GetInstance().ExecuteAll(dt); });
}

static uint64_t g_frameCounter = 0;

static void UpdateGameplaySystems(float dt)
{
    auto* ctx = EngineContext::Get();
    if (!ctx)
        return;

    ++g_frameCounter;
    auto& debugHooks = Spark::DebugHookManager::GetInstance();
    debugHooks.SetFrameNumber(g_frameCounter);
    debugHooks.SetDeltaTime(dt);

    Profiler::GetInstance().BeginFrame();

    SPARK_DEBUG_HOOK(FrameBegin, g_frameCounter, dt);

    if (auto* bus = ctx->GetEventBus())
    {
        bus->Publish(Spark::FrameBeginEvent{dt});
    }

    UpdateNonECSSystems(ctx, dt);

    auto* world = ctx->GetWorld();
    if (!world)
        return;

    UpdateECSDependentSystems(world, dt);

    Spark::Graphics::ConstantBufferDiffManager::GetInstance().BeginFrame();
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

static void ShutdownGameplaySystems()
{
    if (auto* ctx = EngineContext::Get())
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
    Spark::Gameplay::InstanceManager::GetInstance().Shutdown();
    Spark::Gameplay::AbilitySystem::GetInstance().Shutdown();
    Spark::Gameplay::ConditionSystem::GetInstance().Shutdown();

    if (auto* as = AngelScriptEngine::GetInstance())
    {
        as->Shutdown();
    }
    Spark::Scripting::LuaScriptEngine::GetInstance().Shutdown();

    Profiler::GetInstance().Shutdown();

#ifdef ENABLE_DXR
    Spark::Graphics::DXRManager::GetInstance().Shutdown();
#endif

#ifndef NDEBUG
    Spark::Graphics::RHIValidationLayer::GetInstance().Shutdown();
#endif
}

static void UpdateDebugSystems(float dt)
{
    SPARK_GUARDED_UPDATE("TweenManager", "Debug", { Spark::TweenManager::GetInstance().Update(dt); });
    Spark::DebugDrawManager::GetInstance().Flush(dt);
    SPARK_GUARDED_UPDATE("DebugOverlay", "Debug", { Spark::DebugOverlay::GetInstance().Update(dt); });
    SPARK_GUARDED_UPDATE("MemoryMonitor", "Debug", { Spark::MemoryMonitor::GetInstance().Update(dt); });
    Spark::FrameInspector::GetInstance().OnFrameEnd();

    // Update decal fading
    SPARK_GUARDED_UPDATE("DecalSystem", "Debug", { Spark::Graphics::DecalSystem::GetInstance().Update(dt); });
}

static void ShutdownDebugSystems()
{
    SPARK_DEBUG_HOOK_SYSTEM(SystemPreShutdown, "DebugSystems", 0.0);

    Spark::Graphics::DecalSystem::GetInstance().Shutdown();

    Spark::TweenManager::GetInstance().KillAll();
    Spark::DebugDrawManager::GetInstance().Clear();
    Spark::MemoryMonitor::GetInstance().Shutdown();
#ifndef NDEBUG
    Spark::MemoryDebugger::GetInstance().PrintLeakReport();
#endif
    Spark::ChromeTracing::GetInstance().SaveToFile("spark_trace.json");
    Spark::ChromeTracing::GetInstance().Stop();
    Spark::FileLogger::GetInstance().Shutdown();

    SPARK_DEBUG_HOOK_SYSTEM(SystemPostShutdown, "DebugSystems", 0.0);
}

static void InitPhysics()
{
#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
    ASSERT_NOT_NULL(EngineContext::Get());
    g_physicsOwned = std::make_unique<PhysicsSystem>();
    EngineContext::Get()->SetPhysics(g_physicsOwned.get());
    if (g_graphics)
        g_graphics->SetPhysicsSystem(g_physicsOwned.get());
    if (g_eventBus)
        g_physicsOwned->SetEventBus(g_eventBus.get());
#endif
}

/**
 * @brief Initialize the console subsystem and all dependent debug/gameplay systems.
 *
 * Must be called after EngineContext is set up (for EventBus, Physics, etc.)
 * but before module loading (so modules can register console commands).
 * ConsoleProcessManager launches the SparkConsole.exe subprocess and owns the
 * stdin/stdout pipe used for command I/O.
 */
static void InitConsole()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    console.Initialize();
    console.LogSuccess("Spark Engine runtime initialized");

    if (!Spark::ConsoleProcessManager::GetInstance().Initialize())
    {
        console.LogWarning("ConsoleProcessManager failed to initialize — SparkConsole subprocess unavailable");
    }
    InitDebugSystems();
    InitGameplaySystems();

    // Publish EngineStartEvent — all systems initialized
    if (g_eventBus)
    {
        g_eventBus->Publish(Spark::EngineStartEvent{});
    }

    SPARK_DEBUG_HOOK(EnginePostInit, 0, 0.0f);
}

static void ShutdownPhysics()
{
#ifdef SPARK_BULLET_PHYSICS_AVAILABLE
    if (g_physicsOwned)
    {
        g_physicsOwned->Shutdown();
        g_physicsOwned.reset();
    }
#endif
}

/**
 * @brief Common engine shutdown sequence shared by all startup paths.
 *
 * Shuts down gameplay/debug systems, console, modules, audio, physics,
 * and engine context in the correct order.
 */
static void ShutdownEngine()
{
    SPARK_DEBUG_HOOK(EnginePreShutdown, g_frameCounter, 0.0f);

    // Publish EngineShutdownEvent before tearing down systems
    if (g_eventBus)
    {
        g_eventBus->Publish(Spark::EngineShutdownEvent{});
    }

    ShutdownGameplaySystems();
    ShutdownDebugSystems();
    Spark::ConsoleProcessManager::GetInstance().Shutdown();

    if (g_moduleManager)
    {
        g_moduleManager->ShutdownAll();
        g_moduleManager->UnloadAll();
        g_moduleManager.reset();
    }

    g_audioEngine.reset();
    ShutdownPhysics();

    // Shut down the job system after all subsystems that submit jobs
    Spark::JobSystem::Get().Shutdown();

    EngineContext::ResetOwned();
    g_eventBus.reset();
    g_input.reset();
    g_graphics.reset();
    g_timer.reset();

    Spark::SimpleConsole::GetInstance().Shutdown();

    SPARK_DEBUG_HOOK(EnginePostShutdown, g_frameCounter, 0.0f);
    Spark::DebugHookManager::GetInstance().Clear();
}

// ============================================================================
// Windows platform
// ============================================================================
#ifdef SPARK_PLATFORM_WINDOWS

// Windows-specific globals
constexpr int MAX_LOADSTRING = 100;
HINSTANCE g_hInst;
WCHAR g_szTitle[MAX_LOADSTRING];
WCHAR g_szClass[MAX_LOADSTRING];
std::unique_ptr<Spark::LocalFileCache> g_fileCache;
std::unique_ptr<Spark::WeatherSystem> g_weatherSystem;
std::unique_ptr<Spark::UI::UISystem> g_uiSystem;
std::unique_ptr<Spark::DialogueSystem> g_dialogueSystem;
std::unique_ptr<Spark::ModSystem> g_modSystem;
static Spark::DeltaSmoother g_deltaSmoother(10);

#ifdef SPARK_HEADLESS_SUPPORT
// g_headlessMode is defined in EngineContext.cpp (SparkEngineLib)
static std::atomic<bool> g_shutdownRequested{false};

/**
 * @brief Parse command line for -headless or -dedicated flags
 */
static bool ParseHeadlessFlag(LPWSTR cmdLine)
{
    std::wstring cmd(cmdLine);
    return cmd.find(L"-headless") != std::wstring::npos || cmd.find(L"-dedicated") != std::wstring::npos;
}

/**
 * @brief Console Ctrl handler for graceful headless shutdown (Windows)
 */
static BOOL WINAPI HeadlessCtrlHandler(DWORD ctrlType)
{
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT || ctrlType == CTRL_BREAK_EVENT)
    {
        g_shutdownRequested = true;
        return TRUE;
    }
    return FALSE;
}
#endif // SPARK_HEADLESS_SUPPORT

// Win32 forward declarations
ATOM MyRegisterClass(HINSTANCE);
BOOL InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

// Graphics console commands are registered via GraphicsConsoleCommands.cpp

/**
 * @brief Get the executable directory
 */
static std::filesystem::path GetExecutableDirectory()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    return std::filesystem::path(exePath).parent_path();
}

/**
 * @brief Find a specific game module DLL from command line
 *
 * Checks for -game <path> on the command line.
 * Returns empty string if not specified.
 */
static std::string FindGameModuleFromCmdLine(LPWSTR cmdLine)
{
    std::wstring cmd(cmdLine);
    size_t pos = cmd.find(L"-game ");
    if (pos != std::wstring::npos)
    {
        size_t start = pos + 6;
        size_t end = cmd.find(L' ', start);
        std::wstring wpath = cmd.substr(start, end - start);
        std::string path(wpath.begin(), wpath.end());
        if (std::filesystem::exists(path))
            return path;
    }
    return "";
}

/**
 * @brief Find the module manifest or fall back to directory scan
 *
 * Loading priority:
 * 1. Command line: -game <path> (loads single module)
 * 2. spark.modules.json manifest next to the engine exe
 * 3. Directory scan for *Game*.dll / *Module*.dll
 */
static bool LoadGameModules(ModuleManager& manager, LPWSTR cmdLine)
{
    auto exeDir = GetExecutableDirectory();

    // 1. Check command line for specific module
    std::string cmdLineModule = FindGameModuleFromCmdLine(cmdLine);
    if (!cmdLineModule.empty())
        return manager.LoadModule(cmdLineModule);

    // 2. Check for module manifest
    auto manifestPath = exeDir / "spark.modules.json";
    if (std::filesystem::exists(manifestPath))
        return manager.LoadModulesFromManifest(manifestPath.string());

    // 3. Fall back to directory scan
    return manager.LoadModulesFromDirectory(exeDir.string());
}

// ===================================================================================
//                       Windows extracted helpers
// ===================================================================================

/**
 * @brief Configure and install the crash handler with GitHub upload support.
 */
static void SetupCrashHandler()
{
    CrashConfig crashCfg{};
    crashCfg.dumpPrefix = L"SparkCrash";
    crashCfg.uploadURL = "";
    crashCfg.captureScreenshot = true;
    crashCfg.captureSystemInfo = true;
    crashCfg.captureAllThreads = true;
    crashCfg.zipBeforeUpload = true;
    crashCfg.triggerCrashOnAssert = false;

    // GitHub Issue upload — reads token from SPARK_GITHUB_TOKEN env var
    const char* ghRepo = std::getenv("SPARK_GITHUB_REPO");
    const char* ghToken = std::getenv("SPARK_GITHUB_TOKEN");
    if (ghRepo && ghToken)
    {
        crashCfg.githubRepo = ghRepo;
        crashCfg.githubToken = ghToken;
    }

    InstallCrashHandler(crashCfg);
}

#ifdef SPARK_HEADLESS_SUPPORT
/**
 * @brief Run the engine in headless/dedicated server mode (Windows).
 *
 * Allocates a console, initializes server-only subsystems, runs a fixed 60 Hz
 * tick loop, and shuts down cleanly on Ctrl+C.
 */
static int RunHeadlessWindows(LPWSTR lpCmdLine)
{
    // Allocate a console for stdout/stderr output
    AllocConsole();
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);
    freopen_s(&fp, "CONIN$", "r", stdin);

    // Install Ctrl+C handler for graceful shutdown
    SetConsoleCtrlHandler(HeadlessCtrlHandler, TRUE);

    Spark::SimpleConsole::GetInstance().LogInfo("=== Spark Engine (Headless/Dedicated Server) ===");

    // Initialize only the subsystems needed for headless operation
    g_timer = std::make_unique<Timer>();
    g_eventBus = std::make_unique<Spark::EventBus>();
    EngineContext::SetOwned(std::make_unique<EngineContext>(nullptr, nullptr, g_timer.get(), g_eventBus.get()));

    // File cache
    g_fileCache = std::make_unique<Spark::LocalFileCache>();
    EngineContext::Get()->SetFileCache(g_fileCache.get());

    InitPhysics();

    // Register core subsystems with dependency metadata
    Spark::EngineSetup::RegisterCoreSubsystems(*EngineContext::Get());
    Spark::EngineSetup::InitializeJobSystem();

    // Module loading
    g_moduleManager = std::make_unique<ModuleManager>();

    InitConsole();
    auto& console = Spark::SimpleConsole::GetInstance();

    if (LoadGameModules(*g_moduleManager, lpCmdLine))
    {
        g_moduleManager->InitializeAll(EngineContext::Get());
        console.LogSuccess("Loaded " + std::to_string(g_moduleManager->GetModuleCount()) + " module(s)");
    }
    else
    {
        console.LogWarning("No game modules found. Running engine-only headless mode.");
    }

    // Module hot-reload watcher
    g_moduleHotReload = std::make_unique<Spark::ModuleHotReloadManager>();
    g_moduleHotReload->Initialize(g_moduleManager.get(), EngineContext::Get());
    g_moduleHotReload->WatchAllLoadedModules();
    g_moduleHotReload->Start();

    // SaveSystem
    if (!Spark::SaveSystem::GetInstance().Initialize("Saves"))
    {
        Spark::SimpleConsole::GetInstance().LogWarning("SaveSystem initialization failed — save/load unavailable");
    }
    EngineContext::Get()->SetSaveSystem(&Spark::SaveSystem::GetInstance());
    console.LogInfo("SaveSystem initialized");

    // CoroutineScheduler
    EngineContext::Get()->SetCoroutineScheduler(&Spark::CoroutineScheduler::GetInstance());

    // Register console commands
    if (g_graphics)
        Spark::Graphics::RegisterGraphicsConsoleCommands(*g_graphics);
    Spark::RegisterEngineConsoleCommands(g_moduleManager.get(), g_audioEngine.get(), g_moduleHotReload.get());
    Spark::SubsystemFaultIsolator::GetInstance().RegisterConsoleCommands();

    // Fixed 60 Hz server loop
    constexpr auto TICK_INTERVAL = std::chrono::microseconds(16667);
    console.LogInfo("Starting headless server loop (60 Hz)...");
    console.LogInfo("Press Ctrl+C or type 'quit' to stop.");

    while (!g_shutdownRequested)
    {
        auto tickStart = std::chrono::steady_clock::now();

        float dt = g_timer ? g_timer->GetDeltaTime() : (1.0f / 60.0f);

        Spark::FixedTimestepAccumulator::GetInstance().Advance(dt);

        SPARK_GUARDED_UPDATE("Modules", "Core", {
            if (g_moduleManager && g_moduleManager->HasModules())
                g_moduleManager->UpdateAll(dt);
        });

        if (g_moduleHotReload)
            g_moduleHotReload->PollChanges();

        UpdateGameplaySystems(dt);
        UpdateDebugSystems(dt);
        SPARK_GUARDED_UPDATE("Console", "Core", {
            Spark::ConsoleProcessManager::GetInstance().ProcessCommands();
            console.Update();
        });

        auto elapsed = std::chrono::steady_clock::now() - tickStart;
        if (elapsed < TICK_INTERVAL)
            std::this_thread::sleep_for(TICK_INTERVAL - elapsed);
    }

    // Shutdown
    g_moduleHotReload.reset();
    console.LogInfo("Headless server shutting down...");
    g_fileCache.reset();
    ShutdownEngine();

    FreeConsole();
    return 0;
}
#endif // SPARK_HEADLESS_SUPPORT

/**
 * @brief Initialize windowed-mode subsystems: engine context, physics, modules,
 *        audio, save system, console commands, and debug/gameplay systems.
 *
 * Called after the Win32 window has been created and InitInstance() succeeded.
 */
static void InitEngineContext()
{
    g_eventBus = std::make_unique<Spark::EventBus>();
    EngineContext::SetOwned(
        std::make_unique<EngineContext>(g_graphics.get(), g_input.get(), g_timer.get(), g_eventBus.get()));

    g_fileCache = std::make_unique<Spark::LocalFileCache>();
    EngineContext::Get()->SetFileCache(g_fileCache.get());

    InitPhysics();

    Spark::EngineSetup::RegisterCoreSubsystems(*EngineContext::Get());
    Spark::EngineSetup::InitializeJobSystem();

    EngineContext::Get()->SetSaveSystem(&Spark::SaveSystem::GetInstance());
    EngineContext::Get()->SetCoroutineScheduler(&Spark::CoroutineScheduler::GetInstance());

    static Spark::AssetRegistry g_assetRegistry;
    EngineContext::Get()->SetAssetRegistry(&g_assetRegistry);

    if (g_graphics && g_graphics->GetAssetPipeline())
    {
        EngineContext::Get()->SetAssetPipeline(g_graphics->GetAssetPipeline());
    }
}

static void InitGameplaySubsystems()
{
    g_weatherSystem = std::make_unique<Spark::WeatherSystem>();
    EngineContext::Get()->SetWeather(g_weatherSystem.get());
    EngineContext::Get()->SetTimeOfDay(&Spark::TimeOfDaySystem::GetInstance());

    g_uiSystem = std::make_unique<Spark::UI::UISystem>();
    EngineContext::Get()->SetUI(g_uiSystem.get());

    g_dialogueSystem = std::make_unique<Spark::DialogueSystem>();
    EngineContext::Get()->SetDialogue(g_dialogueSystem.get());

    g_modSystem = std::make_unique<Spark::ModSystem>();
    EngineContext::Get()->SetModSystem(g_modSystem.get());
}

static void LoadAndInitModules(LPWSTR lpCmdLine)
{
    g_moduleManager = std::make_unique<ModuleManager>();
    auto& console = Spark::SimpleConsole::GetInstance();

    if (LoadGameModules(*g_moduleManager, lpCmdLine))
    {
        g_moduleManager->InitializeAll(EngineContext::Get());

        auto* primary = g_moduleManager->GetPrimaryModule();
        if (primary)
        {
            auto info = primary->GetModuleInfo();
            std::wstring title = L"Spark Engine - ";
            std::string modName(info.name);
            title.append(modName.begin(), modName.end());
            HWND hWnd = FindWindowW(g_szClass, g_szTitle);
            if (hWnd)
                SetWindowTextW(hWnd, title.c_str());
        }

        console.LogSuccess("Loaded " + std::to_string(g_moduleManager->GetModuleCount()) + " module(s)");
    }
    else
    {
        console.LogWarning("No game modules found. Running engine-only mode.");
        console.LogInfo("Place a game DLL (e.g. SparkGame.dll) next to the engine executable,");
        console.LogInfo("use -game <path> on the command line,");
        console.LogInfo("or create a spark.modules.json manifest.");
    }

    g_moduleHotReload = std::make_unique<Spark::ModuleHotReloadManager>();
    g_moduleHotReload->Initialize(g_moduleManager.get(), EngineContext::Get());
    g_moduleHotReload->WatchAllLoadedModules();
    g_moduleHotReload->Start();
}

static void InitializeWindowedSubsystems(HINSTANCE hInstance, LPWSTR lpCmdLine)
{
    InitEngineContext();
    InitGameplaySubsystems();

    auto& console = Spark::SimpleConsole::GetInstance();

    if (!Spark::SaveSystem::GetInstance().Initialize("Saves"))
    {
        console.LogWarning("SaveSystem initialization failed — save/load unavailable");
    }
    console.LogInfo("SaveSystem initialized");

    // Initialize AudioEngine before modules so modules can use EngineContext::Get()->GetAudio()
    g_audioEngine = std::make_unique<AudioEngine>();
    if (SUCCEEDED(g_audioEngine->Initialize(32)))
    {
        console.LogInfo("AudioEngine initialized (32 sources)");
        EngineContext::Get()->SetAudio(g_audioEngine.get());
    }
    else
    {
        console.LogWarning("AudioEngine initialization failed - audio commands will be unavailable");
        g_audioEngine.reset();
    }

    LoadAndInitModules(lpCmdLine);

    if (g_graphics)
        Spark::Graphics::RegisterGraphicsConsoleCommands(*g_graphics);
    Spark::RegisterEngineConsoleCommands(g_moduleManager.get(), g_audioEngine.get(), g_moduleHotReload.get());
    Spark::SubsystemFaultIsolator::GetInstance().RegisterConsoleCommands();
    EngineSettings::GetInstance().RegisterConsoleCommands();

    LogMissingModuleWarnings();

    if (g_weatherSystem && g_eventBus)
    {
        g_weatherSystem->SetEventBus(g_eventBus.get());
    }

    InitConsole();
}

/**
 * @brief Run the Win32 message pump and per-frame engine tick loop.
 *
 * Returns the wParam from the WM_QUIT message for use as the process exit code.
 */
static int RunWindowedMainLoop(HINSTANCE hInstance)
{
    HACCEL accel = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_SparkEngine));
    MSG msg = {};
    ASSERT(g_timer);

    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("Starting main engine loop...");

    // Win32 message pump: PeekMessage with PM_REMOVE gives us non-blocking
    // message processing — the engine ticks in the else branch whenever
    // there are no pending OS messages (resize, input, focus, etc.).
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (!TranslateAccelerator(msg.hwnd, accel, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
        else
        {
            // Smooth delta time over the last N frames to prevent physics/animation
            // jitter caused by single-frame spikes (e.g. shader compilation stalls,
            // OS scheduling delays). Raw dt is preserved for profiling accuracy.
            float rawDt = g_timer ? g_timer->GetDeltaTime() : 0.016f;
            float dt = g_deltaSmoother.Smooth(rawDt);

            // Advance the global fixed-timestep accumulator so all systems can
            // query GetFixedStepCount() for deterministic fixed-rate updates.
            Spark::FixedTimestepAccumulator::GetInstance().Advance(rawDt);

            SPARK_GUARDED_UPDATE("Input", "Core", {
                if (g_input)
                    g_input->Update();
            });

            if (g_moduleManager && g_moduleManager->HasModules())
            {
                SPARK_GUARDED_UPDATE("Modules", "Core", {
                    g_moduleManager->UpdateAll(dt);
                    g_moduleManager->RenderAll();
                });
            }
            else if (g_graphics)
            {
                // Engine-only mode: just clear and present
                g_graphics->BeginFrame();
                g_graphics->EndFrame();
            }

            if (g_moduleHotReload)
                g_moduleHotReload->PollChanges();

            UpdateGameplaySystems(dt);
            UpdateDebugSystems(dt);
            SPARK_GUARDED_UPDATE("Console", "Core", {
                Spark::ConsoleProcessManager::GetInstance().ProcessCommands();
                console.Update();
            });
        }
    }

    // Shutdown
    g_moduleHotReload.reset();
    console.LogInfo("Shutting down...");
    g_fileCache.reset();
    ShutdownEngine();

    return static_cast<int>(msg.wParam);
}

// ===================================================================================
//                                    wWinMain
// ===================================================================================
int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Core);
    ASSERT(hInstance != nullptr);

    SetupCrashHandler();

#ifdef SPARK_HEADLESS_SUPPORT
    g_headlessMode = ParseHeadlessFlag(lpCmdLine);
    if (g_headlessMode)
        return RunHeadlessWindows(lpCmdLine);
#endif

    // Register window class and title
    ASSERT(MAX_LOADSTRING <= _countof(g_szClass) && MAX_LOADSTRING <= _countof(g_szTitle));
    wcscpy_s(g_szClass, MAX_LOADSTRING, L"SparkEngineWindowClass");
    wcscpy_s(g_szTitle, MAX_LOADSTRING, L"Spark Engine");

    ATOM cls = MyRegisterClass(hInstance);
    ASSERT_MSG(cls != 0, "MyRegisterClass failed");
    if (cls == 0)
    {
        MessageBoxW(nullptr, L"RegisterClassExW failed", L"Fatal Error", MB_ICONERROR);
        return -1;
    }

    // Create window and init graphics/input/timer
    if (!InitInstance(hInstance, nCmdShow))
        return -1;

    // Initialize all engine subsystems, load modules, register commands
    InitializeWindowedSubsystems(hInstance, lpCmdLine);

    // Run the message pump + tick loop until WM_QUIT
    return RunWindowedMainLoop(hInstance);
}

// ===================================================================================
//                       Win32 boilerplate
// ===================================================================================
ATOM MyRegisterClass(HINSTANCE hInst)
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_SparkEngine));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszMenuName = MAKEINTRESOURCEW(IDC_SparkEngine);
    wc.lpszClassName = g_szClass;
    wc.hIconSm = LoadIcon(hInst, MAKEINTRESOURCE(IDI_SMALL));

    ATOM result = RegisterClassExW(&wc);
    ASSERT_MSG(result != 0, "RegisterClassExW returned zero");
    return result;
}

BOOL InitInstance(HINSTANCE hInst, int nCmdShow)
{
    ASSERT(hInst != nullptr);
    g_hInst = hInst;

    // Load engine settings from INI (before window creation so we can use the dimensions)
    auto& settings = EngineSettings::GetInstance();
    settings.Load();

    int winW = settings.Graphics().windowWidth;
    int winH = settings.Graphics().windowHeight;

    HWND hWnd = CreateWindowW(g_szClass, g_szTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, winW, winH, nullptr, nullptr,
                              hInst, nullptr);

    if (!hWnd)
    {
        DWORD err = GetLastError();
        wchar_t buf[256];
        swprintf_s(buf, L"CreateWindowW failed (0x%08X)", static_cast<unsigned>(err));
        MessageBoxW(nullptr, buf, L"Fatal Error", MB_ICONERROR);
        return FALSE;
    }

    g_timer = std::make_unique<Timer>();
    ASSERT(g_timer);

    g_graphics = std::make_unique<GraphicsEngine>();
    ASSERT(g_graphics);
    HRESULT hr = g_graphics->Initialize(hWnd);
    if (FAILED(hr))
    {
        wchar_t buf[256];
        swprintf_s(buf, L"Graphics initialization failed (HR=0x%08X)", static_cast<unsigned>(hr));
        MessageBoxW(hWnd, buf, L"Fatal Error", MB_ICONERROR);
        return FALSE;
    }

    // Apply VSync setting from INI
    g_graphics->Console_SetVSync(settings.Graphics().vsync);

    g_input = std::make_unique<InputManager>();
    ASSERT(g_input);
    g_input->Initialize(hWnd);

    // Apply input settings from INI
    g_input->Console_SetMouseSensitivity(settings.Controls().mouseSensitivity);
    g_input->Console_SetInvertMouseY(settings.Controls().invertMouseY);
    g_input->Console_SetMouseDeadZone(settings.Controls().mouseDeadZone);
    g_input->Console_SetRawMouseInput(settings.Controls().rawMouseInput);
    g_input->Console_SetMouseAcceleration(settings.Controls().mouseAcceleration);

    // Console init is handled by InitConsole() in InitializeWindowedSubsystems()
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);

    return TRUE;
}

// ===================================================================================
//                          Window procedure
// ===================================================================================
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        if (g_input)
            g_input->HandleMessage(msg, wParam, lParam);
        break;

    case WM_SIZE:
        if (g_graphics)
            g_graphics->OnResize(LOWORD(lParam), HIWORD(lParam));
        if (g_moduleManager)
            g_moduleManager->ResizeAll(LOWORD(lParam), HIWORD(lParam));
        if (g_eventBus)
            g_eventBus->Publish(Spark::WindowResizeEvent{LOWORD(lParam), HIWORD(lParam)});
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

INT_PTR CALLBACK About(HWND hDlg, UINT msg, WPARAM wParam, LPARAM)
{
    if (msg == WM_COMMAND && (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL))
    {
        EndDialog(hDlg, LOWORD(wParam));
        return TRUE;
    }
    return FALSE;
}


#endif // SPARK_PLATFORM_WINDOWS

// ============================================================================
// Linux platform
// ============================================================================
#ifndef SPARK_PLATFORM_WINDOWS

static std::atomic<bool> g_shutdownRequested{false};

static void SignalHandler(int)
{
    g_shutdownRequested.store(true, std::memory_order_relaxed);
}

static bool ParseFlag(int argc, char* argv[], const char* flag)
{
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], flag) == 0)
            return true;
    }
    return false;
}

static std::filesystem::path GetExecutableDirectoryLinux()
{
    // /proc/self/exe is the canonical way on Linux
    std::error_code ec;
    auto exePath = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec)
        return exePath.parent_path();
    return std::filesystem::current_path();
}

static std::string FindGameModuleFromArgs(int argc, char* argv[])
{
    for (int i = 1; i < argc - 1; ++i)
    {
        if (strcmp(argv[i], "-game") == 0)
        {
            std::string path = argv[i + 1];
            if (std::filesystem::exists(path))
                return path;
        }
    }
    return "";
}

static bool LoadGameModulesLinux(ModuleManager& manager, int argc, char* argv[])
{
    auto exeDir = GetExecutableDirectoryLinux();

    // 1. Check command line for specific module
    std::string cmdLineModule = FindGameModuleFromArgs(argc, argv);
    if (!cmdLineModule.empty())
        return manager.LoadModule(cmdLineModule);

    // 2. Check for module manifest
    auto manifestPath = exeDir / "spark.modules.json";
    if (std::filesystem::exists(manifestPath))
        return manager.LoadModulesFromManifest(manifestPath.string());

    // 3. Fall back to directory scan
    return manager.LoadModulesFromDirectory(exeDir.string());
}


// ===================================================================================
//                    Linux extracted helpers
// ===================================================================================

/**
 * @brief Common per-frame tick logic shared by SDL2 windowed and no-SDL2 fallback modes.
 *
 * Updates input, modules, gameplay/debug systems, and console processing.
 */
static void TickFrame(float dt)
{
    // Advance the global fixed-timestep accumulator for deterministic fixed-rate updates.
    Spark::FixedTimestepAccumulator::GetInstance().Advance(dt);

    SPARK_GUARDED_UPDATE("Input", "Core", {
        if (g_input)
            g_input->Update();
    });

    if (g_moduleManager && g_moduleManager->HasModules())
    {
        SPARK_GUARDED_UPDATE("Modules", "Core", {
            g_moduleManager->UpdateAll(dt);
            g_moduleManager->RenderAll();
        });
    }
    else if (g_graphics)
    {
        g_graphics->BeginFrame();
        g_graphics->EndFrame();
    }

    if (g_moduleHotReload)
        g_moduleHotReload->PollChanges();

    UpdateGameplaySystems(dt);
    UpdateDebugSystems(dt);
    SPARK_GUARDED_UPDATE("Console", "Core", {
        Spark::ConsoleProcessManager::GetInstance().ProcessCommands();
        Spark::SimpleConsole::GetInstance().Update();
    });
}

/**
 * @brief Register gameplay subsystems (Weather, UI, Dialogue, Modding) with EngineContext.
 *
 * Uses function-local statics so these objects live for the process lifetime
 * without polluting the global namespace.
 */
static void RegisterGameplaySubsystems()
{
    static Spark::WeatherSystem s_weatherSystem;
    EngineContext::Get()->SetWeather(&s_weatherSystem);

    // TimeOfDay — singleton, registered with context for game-module access
    EngineContext::Get()->SetTimeOfDay(&Spark::TimeOfDaySystem::GetInstance());

    // Wire WeatherSystem to EventBus for WeatherChangedEvent publishing
    if (g_eventBus)
    {
        s_weatherSystem.SetEventBus(g_eventBus.get());
    }

    static Spark::UI::UISystem s_uiSystem;
    EngineContext::Get()->SetUI(&s_uiSystem);

    static Spark::DialogueSystem s_dialogueSystem;
    EngineContext::Get()->SetDialogue(&s_dialogueSystem);

    static Spark::ModSystem s_modSystem;
    EngineContext::Get()->SetModSystem(&s_modSystem);
}

/**
 * @brief Initialize engine core subsystems common to all Linux startup paths.
 *
 * Creates EngineContext, physics, core subsystem registration, save system,
 * coroutine scheduler, and gameplay subsystem registration.
 *
 * @param registerGameplay If true, registers Weather/UI/Dialogue/Modding and AssetRegistry.
 */
static void InitLinuxCoreSubsystems(bool registerGameplay)
{
    EngineContext::SetOwned(
        std::make_unique<EngineContext>(g_graphics.get(), g_input.get(), g_timer.get(), g_eventBus.get()));

    InitPhysics();

    Spark::EngineSetup::RegisterCoreSubsystems(*EngineContext::Get());
    Spark::EngineSetup::InitializeJobSystem();

    if (!Spark::SaveSystem::GetInstance().Initialize("Saves"))
    {
        Spark::SimpleConsole::GetInstance().LogWarning("SaveSystem initialization failed — save/load unavailable");
    }
    EngineContext::Get()->SetSaveSystem(&Spark::SaveSystem::GetInstance());
    EngineContext::Get()->SetCoroutineScheduler(&Spark::CoroutineScheduler::GetInstance());

    // AssetPipeline (owned by GraphicsEngine, exposed via EngineContext for SDK access)
    if (g_graphics && g_graphics->GetAssetPipeline())
    {
        EngineContext::Get()->SetAssetPipeline(g_graphics->GetAssetPipeline());
    }

    if (registerGameplay)
    {
        static Spark::AssetRegistry s_assetRegistry;
        EngineContext::Get()->SetAssetRegistry(&s_assetRegistry);
        RegisterGameplaySubsystems();
    }
}

/**
 * @brief Load game modules, initialize hot-reload watcher, and register console commands.
 *
 * Shared by all Linux startup paths. Handles module loading via LoadGameModulesLinux,
 * hot-reload setup, audio engine init (if windowed), and console command registration.
 *
 * @param argc Argument count from main().
 * @param argv Argument values from main().
 * @param initAudio If true, creates and initializes AudioEngine.
 */
static void InitLinuxModulesAndCommands(int argc, char* argv[], bool initAudio)
{
    auto& console = Spark::SimpleConsole::GetInstance();

    // Initialize AudioEngine before modules so modules can use EngineContext::Get()->GetAudio()
    if (initAudio)
    {
        g_audioEngine = std::make_unique<AudioEngine>();
        if (SUCCEEDED(g_audioEngine->Initialize(32)))
        {
            console.LogInfo("AudioEngine initialized (32 sources)");
            EngineContext::Get()->SetAudio(g_audioEngine.get());
        }
        else
        {
            console.LogWarning("AudioEngine initialization failed");
            g_audioEngine.reset();
        }
    }

    g_moduleManager = std::make_unique<ModuleManager>();

    if (LoadGameModulesLinux(*g_moduleManager, argc, argv))
    {
        g_moduleManager->InitializeAll(EngineContext::Get());
        console.LogSuccess("Loaded " + std::to_string(g_moduleManager->GetModuleCount()) + " module(s)");
    }
    else
    {
        console.LogWarning("No game modules found. Running engine-only mode.");
    }

    // Module hot-reload watcher
    g_moduleHotReload = std::make_unique<Spark::ModuleHotReloadManager>();
    g_moduleHotReload->Initialize(g_moduleManager.get(), EngineContext::Get());
    g_moduleHotReload->WatchAllLoadedModules();
    g_moduleHotReload->Start();

    // Console commands
    if (g_graphics)
        Spark::Graphics::RegisterGraphicsConsoleCommands(*g_graphics);
    Spark::RegisterEngineConsoleCommands(g_moduleManager.get(), g_audioEngine.get(), g_moduleHotReload.get());
    Spark::SubsystemFaultIsolator::GetInstance().RegisterConsoleCommands();

    LogMissingModuleWarnings();
}

/**
 * @brief Common shutdown sequence for all Linux startup paths.
 */
static void ShutdownLinux()
{
    g_moduleHotReload.reset();
    Spark::SimpleConsole::GetInstance().LogInfo("Shutting down...");
    ShutdownEngine();
}

#ifdef SPARK_HEADLESS_SUPPORT
/**
 * @brief Run the engine in headless/dedicated server mode (Linux).
 *
 * Initializes server-only subsystems (no graphics, no audio), runs a fixed 60 Hz
 * tick loop, and shuts down cleanly on SIGINT/SIGTERM.
 */
static int RunHeadlessLinux(int argc, char* argv[])
{
    Spark::SimpleConsole::GetInstance().LogInfo("=== Spark Engine (Headless/Dedicated Server - Linux) ===");

    g_eventBus = std::make_unique<Spark::EventBus>();
    g_timer = std::make_unique<Timer>();

    // Headless: no gameplay subsystems (no Weather/UI/Dialogue/Modding)
    InitLinuxCoreSubsystems(/*registerGameplay=*/false);

    InitConsole();

    InitLinuxModulesAndCommands(argc, argv, /*initAudio=*/false);

    // Fixed 60 Hz server loop
    constexpr auto TICK_INTERVAL = std::chrono::microseconds(16667);
    auto& console = Spark::SimpleConsole::GetInstance();
    console.LogInfo("Starting headless server loop (60 Hz)...");
    console.LogInfo("Press Ctrl+C to stop.");

    while (!g_shutdownRequested)
    {
        auto tickStart = std::chrono::steady_clock::now();
        float dt = g_timer ? g_timer->GetDeltaTime() : (1.0f / 60.0f);

        Spark::FixedTimestepAccumulator::GetInstance().Advance(dt);

        SPARK_GUARDED_UPDATE("Modules", "Core", {
            if (g_moduleManager && g_moduleManager->HasModules())
                g_moduleManager->UpdateAll(dt);
        });

        if (g_moduleHotReload)
            g_moduleHotReload->PollChanges();

        UpdateGameplaySystems(dt);
        UpdateDebugSystems(dt);
        SPARK_GUARDED_UPDATE("Console", "Core", {
            Spark::ConsoleProcessManager::GetInstance().ProcessCommands();
            console.Update();
        });

        auto elapsed = std::chrono::steady_clock::now() - tickStart;
        if (elapsed < TICK_INTERVAL)
            std::this_thread::sleep_for(TICK_INTERVAL - elapsed);
    }

    ShutdownLinux();
    Spark::SimpleConsole::GetInstance().LogInfo("Headless server shut down cleanly.");
    return 0;
}
#endif // SPARK_HEADLESS_SUPPORT

#ifdef SPARK_SDL2_AVAILABLE
/**
 * @brief Translate an SDL key symbol to a Win32 virtual key code.
 *
 * InputManager uses WM_KEYDOWN/WM_KEYUP style messages internally,
 * so SDL key events must be translated to VK_* codes for consistent handling.
 *
 * @return The corresponding VK_* code, or 0 if the key is not mapped.
 */
static int TranslateSDLKeyToVK(SDL_Keycode sym)
{
    // Alphabetic keys
    if (sym >= SDLK_a && sym <= SDLK_z)
        return 'A' + (sym - SDLK_a);

    // Numeric keys
    if (sym >= SDLK_0 && sym <= SDLK_9)
        return '0' + (sym - SDLK_0);

    // Function keys
    if (sym >= SDLK_F1 && sym <= SDLK_F12)
        return VK_F1 + (sym - SDLK_F1);

    // Named keys
    switch (sym)
    {
    case SDLK_SPACE:
        return VK_SPACE;
    case SDLK_ESCAPE:
        return VK_ESCAPE;
    case SDLK_RETURN:
        return VK_RETURN;
    case SDLK_TAB:
        return VK_TAB;
    case SDLK_BACKSPACE:
        return VK_BACK;
    case SDLK_UP:
        return VK_UP;
    case SDLK_DOWN:
        return VK_DOWN;
    case SDLK_LEFT:
        return VK_LEFT;
    case SDLK_RIGHT:
        return VK_RIGHT;
    case SDLK_LSHIFT:
        return VK_LSHIFT;
    case SDLK_RSHIFT:
        return VK_RSHIFT;
    case SDLK_LCTRL:
        return VK_LCONTROL;
    case SDLK_RCTRL:
        return VK_RCONTROL;
    case SDLK_LALT:
        return VK_LMENU;
    case SDLK_RALT:
        return VK_RMENU;
    case SDLK_DELETE:
        return VK_DELETE;
    default:
        return 0;
    }
}

/**
 * @brief Dispatch a single SDL event to the appropriate engine subsystem.
 *
 * Handles window close/resize, keyboard, and mouse events by translating
 * them into the InputManager's message format.
 *
 * @param event The SDL event to process.
 * @return false if the application should quit, true otherwise.
 */
static bool HandleSDLEvent(const SDL_Event& event)
{
    switch (event.type)
    {
    case SDL_QUIT:
        return false;

    case SDL_WINDOWEVENT:
        if (event.window.event == SDL_WINDOWEVENT_CLOSE)
            return false;
        if (event.window.event == SDL_WINDOWEVENT_RESIZED)
        {
            int w = event.window.data1;
            int h = event.window.data2;
            if (g_graphics)
                g_graphics->OnResize(w, h);
            if (g_moduleManager)
                g_moduleManager->ResizeAll(w, h);
            if (g_eventBus)
                g_eventBus->Publish(Spark::WindowResizeEvent{static_cast<uint32_t>(w), static_cast<uint32_t>(h)});
        }
        break;

    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (g_input)
        {
            UINT msg = (event.type == SDL_KEYDOWN) ? WM_KEYDOWN : WM_KEYUP;
            int vk = TranslateSDLKeyToVK(event.key.keysym.sym);
            if (vk != 0)
                g_input->HandleMessage(msg, static_cast<WPARAM>(vk), 0);
        }
        break;

    case SDL_MOUSEMOTION:
        if (g_input)
            g_input->HandleMessage(WM_MOUSEMOVE, 0,
                                   static_cast<LPARAM>((event.motion.y << 16) | (event.motion.x & 0xFFFF)));
        break;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (g_input)
        {
            UINT msg = 0;
            if (event.button.button == SDL_BUTTON_LEFT)
                msg = (event.type == SDL_MOUSEBUTTONDOWN) ? WM_LBUTTONDOWN : WM_LBUTTONUP;
            else if (event.button.button == SDL_BUTTON_RIGHT)
                msg = (event.type == SDL_MOUSEBUTTONDOWN) ? WM_RBUTTONDOWN : WM_RBUTTONUP;
            else if (event.button.button == SDL_BUTTON_MIDDLE)
                msg = (event.type == SDL_MOUSEBUTTONDOWN) ? WM_MBUTTONDOWN : WM_MBUTTONUP;
            if (msg)
                g_input->HandleMessage(msg, 0, 0);
        }
        break;
    }

    return true;
}

/**
 * @brief Initialize SDL2 windowed-mode subsystems: window, graphics, input,
 *        engine context, modules, audio, and console commands.
 *
 * @param window The SDL2 window (already created by the caller).
 * @param argc Argument count from main().
 * @param argv Argument values from main().
 */
static void InitializeSDL2Subsystems(SDL_Window* window, int argc, char* argv[])
{
    auto& settings = EngineSettings::GetInstance();

    // Core engine objects
    g_timer = std::make_unique<Timer>();
    g_eventBus = std::make_unique<Spark::EventBus>();
    g_input = std::make_unique<InputManager>();
    g_input->Initialize(static_cast<HWND>(window));
    g_graphics = std::make_unique<GraphicsEngine>();

    HRESULT hr = g_graphics->Initialize(static_cast<Spark::NativeWindowHandle>(window));
    auto& console = Spark::SimpleConsole::GetInstance();
    if (SUCCEEDED(hr))
        console.LogInfo("Graphics engine initialized (RHI backend).");
    else
        console.LogWarning("Graphics engine initialization deferred (headless fallback).");

    // Engine context, physics, core subsystems, gameplay subsystems
    InitLinuxCoreSubsystems(/*registerGameplay=*/true);

    // Modules, audio, console commands
    InitLinuxModulesAndCommands(argc, argv, /*initAudio=*/true);

    // Update window title with primary module name
    if (g_moduleManager)
    {
        auto* primary = g_moduleManager->GetPrimaryModule();
        if (primary)
        {
            auto info = primary->GetModuleInfo();
            std::string title = std::string("Spark Engine - ") + info.name;
            SDL_SetWindowTitle(window, title.c_str());
        }
    }

    settings.RegisterConsoleCommands();

    // Initialize console, debug, and gameplay systems in one call
    // (also publishes EngineStartEvent when complete)
    InitConsole();
}

/**
 * @brief Run the SDL2 event pump and per-frame engine tick loop.
 *
 * Processes SDL events via HandleSDLEvent(), then calls TickFrame() for
 * the engine update. Returns when the window is closed or SIGINT is received.
 */
static void RunSDL2MainLoop()
{
    Spark::SimpleConsole::GetInstance().LogInfo("Starting main engine loop (SDL2)...");

    while (!g_shutdownRequested)
    {
        SDL_Event event;
        bool running = true;

        while (SDL_PollEvent(&event))
        {
            if (!HandleSDLEvent(event))
            {
                running = false;
                break;
            }
        }

        if (!running)
            break;

        float dt = g_timer ? g_timer->GetDeltaTime() : 0.016f;
        TickFrame(dt);
    }
}

/**
 * @brief Run the engine in SDL2 windowed mode (Linux).
 *
 * Creates an SDL2 window, initializes all engine subsystems, runs the
 * main loop, and cleans up SDL resources on exit.
 */
static int RunSDL2Windowed(int argc, char* argv[])
{
    Spark::SimpleConsole::GetInstance().LogInfo("=== Spark Engine (Linux Build) ===");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
    {
        Spark::SimpleConsole::GetInstance().LogError(std::string("SDL_Init failed: ") + SDL_GetError());
        return -1;
    }

    auto& settings = EngineSettings::GetInstance();
    settings.Load();

    int winW = settings.Graphics().windowWidth;
    int winH = settings.Graphics().windowHeight;

    // Set OpenGL attributes before window creation (required for Mesa/llvmpipe)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    Uint32 windowFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (settings.Graphics().fullscreen)
        windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    windowFlags |= SDL_WINDOW_OPENGL;

    SDL_Window* window =
        SDL_CreateWindow("Spark Engine", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winW, winH, windowFlags);
    if (!window)
    {
        Spark::SimpleConsole::GetInstance().LogError(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        SDL_Quit();
        return -1;
    }

    // Create SDL GL context and make it current before engine graphics init.
    // This ensures Mesa llvmpipe and other software renderers work correctly —
    // the GraphicsEngine can then share or skip its own bootstrap context.
    SDL_GLContext glContext = SDL_GL_CreateContext(window);
    if (!glContext)
    {
        Spark::SimpleConsole::GetInstance().LogWarning(std::string("SDL_GL_CreateContext failed: ") + SDL_GetError() +
                                                       " — engine will try headless fallback");
    }
    else
    {
        SDL_GL_MakeCurrent(window, glContext);
        SDL_GL_SetSwapInterval(1);
        Spark::SimpleConsole::GetInstance().LogInfo("SDL2 OpenGL context created successfully");
    }

    InitializeSDL2Subsystems(window, argc, argv);
    RunSDL2MainLoop();

    ShutdownLinux();
    if (glContext)
        SDL_GL_DeleteContext(glContext);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
#endif // SPARK_SDL2_AVAILABLE

#ifndef SPARK_SDL2_AVAILABLE
/**
 * @brief Run the engine without SDL2 (no-window fallback).
 *
 * Initializes engine subsystems in headless-like mode, processes a few ticks
 * to validate initialization, then exits. Used when SDL2 is not available
 * and the engine was not explicitly started in headless mode.
 */
static int RunNoSDL2Fallback(int argc, char* argv[])
{
    auto& noSdlConsole = Spark::SimpleConsole::GetInstance();
    noSdlConsole.LogInfo("=== Spark Engine (Linux Build) ===");
    noSdlConsole.LogWarning("SDL2 not available. Running without a window.");
    noSdlConsole.LogWarning("Install SDL2 and rebuild with -DENABLE_SDL2=ON for windowed mode.");

    g_eventBus = std::make_unique<Spark::EventBus>();
    g_timer = std::make_unique<Timer>();
    g_input = std::make_unique<InputManager>();
    g_graphics = std::make_unique<GraphicsEngine>();

    HRESULT hr = g_graphics->Initialize(nullptr);
    if (FAILED(hr))
        noSdlConsole.LogWarning("Graphics initialization failed (fallback mode).");

    // Engine context, physics, core subsystems, gameplay subsystems
    InitLinuxCoreSubsystems(/*registerGameplay=*/true);

    InitConsole();
    Spark::SimpleConsole::GetInstance().LogWarning("No SDL2 - engine will exit after initialization.");

    InitLinuxModulesAndCommands(argc, argv, /*initAudio=*/false);

    // Minimal loop — process a few ticks to validate initialization, then exit
    for (int frame = 0; frame < 10 && !g_shutdownRequested; ++frame)
    {
        float dt = g_timer ? g_timer->GetDeltaTime() : 0.016f;
        TickFrame(dt);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    ShutdownLinux();
    return 0;
}
#endif // !SPARK_SDL2_AVAILABLE

// ===================================================================================
//                                    main
// ===================================================================================

int main(int argc, char* argv[])
{
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

#ifdef SPARK_HEADLESS_SUPPORT
    bool headless = ParseFlag(argc, argv, "-headless") || ParseFlag(argc, argv, "-dedicated");
    g_headlessMode = headless;
    if (headless)
        return RunHeadlessLinux(argc, argv);
#endif

#ifdef SPARK_SDL2_AVAILABLE
    int result = RunSDL2Windowed(argc, argv);
#else
    int result = RunNoSDL2Fallback(argc, argv);
#endif

    Spark::SimpleConsole::GetInstance().LogInfo("Spark Engine shut down cleanly.");
    return result;
}
#endif // !SPARK_PLATFORM_WINDOWS
