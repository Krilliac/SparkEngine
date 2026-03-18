/**
 * @file EngineSetup.h
 * @brief Wires architecture components into the engine lifecycle
 *
 * Provides helper functions that register all engine subsystems with
 * EngineContext using dependency metadata, and create the EngineBootstrap
 * descriptor list for ordered initialization.
 *
 * Usage:
 * @code
 *   auto* ctx = EngineContext::Get();
 *   Spark::EngineSetup::RegisterCoreSubsystems(*ctx);
 *   ctx->InitializeAll();
 * @endcode
 */

#pragma once

#include "EngineContext.h"
#include "EngineBootstrap.h"
#include "../Engine/ECS/Systems/PhaseSystemManager.h"
#include "../Engine/ECS/Systems/TerrainSystem.h"
#include "../Utils/JobSystem.h"

// Forward declarations
class GraphicsEngine;
class PhysicsSystem;
class AudioEngine;
class InputManager;
class Timer;
class SceneManager;
class AngelScriptEngine;

namespace Spark
{
    namespace Animation
    {
        class AnimationSystem;
    }
    namespace AI
    {
        class AISystem;
    }
    namespace UI
    {
        class UISystem;
    }
    class SaveSystem;
    class CoroutineScheduler;
    class NetworkManager;
    class WeatherSystem;
    class DialogueSystem;
    class ModSystem;
} // namespace Spark

namespace Spark::EngineSetup
{

    /**
     * @brief Register all core engine subsystems with EngineContext dependency metadata.
     *
     * Sets up the dependency graph so InitializeAll() initializes them in the right order:
     *   Timer -> EventBus -> Input -> Graphics -> Physics -> Audio -> Animation -> AI
     *
     * Call this after creating the EngineContext and setting all subsystem pointers.
     */
    inline void RegisterCoreSubsystems(EngineContext& ctx)
    {
        // Timer has no dependencies
        if (auto* timer = ctx.GetTimer())
        {
            ctx.RegisterSubsystem<Timer>(timer, DependsOn<>{});
        }

        // EventBus depends on nothing
        if (auto* eventBus = ctx.GetEventBus())
        {
            ctx.RegisterSubsystem<Spark::EventBus>(eventBus, DependsOn<>{});
        }

        // Input depends on Timer
        if (auto* input = ctx.GetInput())
        {
            ctx.RegisterSubsystem<InputManager>(input, DependsOn<Timer>{});
        }

        // Graphics depends on Timer
        if (auto* graphics = ctx.GetGraphics())
        {
            ctx.RegisterSubsystem<GraphicsEngine>(graphics, DependsOn<Timer>{});
        }

        // Physics depends on Timer
        if (auto* physics = ctx.GetPhysics())
        {
            ctx.RegisterSubsystem<PhysicsSystem>(physics, DependsOn<Timer>{});
        }

        // Audio depends on Timer and Graphics (for 3D positioning)
        if (auto* audio = ctx.GetAudio())
        {
            ctx.RegisterSubsystem<AudioEngine>(audio, DependsOn<Timer, GraphicsEngine>{});
        }

        // Animation depends on Timer
        if (auto* anim = ctx.GetAnimation())
        {
            ctx.RegisterSubsystem<Spark::Animation::AnimationSystem>(anim, DependsOn<Timer>{});
        }

        // AI depends on Timer and Physics (for navmesh queries)
        if (auto* ai = ctx.GetAI())
        {
            ctx.RegisterSubsystem<Spark::AI::AISystem>(ai, DependsOn<Timer, PhysicsSystem>{});
        }

        // SaveSystem depends on nothing
        if (auto* save = ctx.GetSaveSystem())
        {
            ctx.RegisterSubsystem<Spark::SaveSystem>(save, DependsOn<>{});
        }

        // CoroutineScheduler depends on Timer
        if (auto* coroutines = ctx.GetCoroutineScheduler())
        {
            ctx.RegisterSubsystem<Spark::CoroutineScheduler>(coroutines, DependsOn<Timer>{});
        }

        // SceneManager depends on Graphics and Physics
        if (auto* scene = ctx.GetSceneManager())
        {
            ctx.RegisterSubsystem<SceneManager>(scene, DependsOn<GraphicsEngine, PhysicsSystem>{});
        }

        // Scripting depends on Timer and EventBus
        if (auto* script = ctx.GetScriptEngine())
        {
            ctx.RegisterSubsystem<AngelScriptEngine>(script, DependsOn<Timer, Spark::EventBus>{});
        }

        // Networking depends on Timer (only registered when ENABLE_NETWORKING=ON)
        if (auto* network = ctx.GetNetwork())
        {
            ctx.RegisterSubsystem<Spark::NetworkManager>(network, DependsOn<Timer>{});
        }

        // WeatherSystem depends on Timer (weather transitions are time-based)
        if (auto* weather = ctx.GetSystem<Spark::WeatherSystem>())
        {
            ctx.RegisterSubsystem<Spark::WeatherSystem>(weather, DependsOn<Timer>{});
        }

        // UISystem depends on Timer and EventBus
        if (auto* ui = ctx.GetSystem<Spark::UI::UISystem>())
        {
            ctx.RegisterSubsystem<Spark::UI::UISystem>(ui, DependsOn<Timer, Spark::EventBus>{});
        }

        // DialogueSystem depends on Timer and EventBus (fires events, tracks timing)
        if (auto* dialogue = ctx.GetSystem<Spark::DialogueSystem>())
        {
            ctx.RegisterSubsystem<Spark::DialogueSystem>(dialogue, DependsOn<Timer, Spark::EventBus>{});
        }

        // ModSystem depends on nothing (scans filesystem)
        if (auto* mods = ctx.GetSystem<Spark::ModSystem>())
        {
            ctx.RegisterSubsystem<Spark::ModSystem>(mods, DependsOn<>{});
        }
    }

    /**
     * @brief Create a PhaseSystemManager with all standard systems registered in their correct phases.
     *
     * This replaces flat SystemManager usage with deterministic phase ordering:
     *   PrePhysics -> Physics -> PostPhysics -> Animation -> AI -> Audio -> Gameplay -> PreRender -> Render -> PostRender
     *
     * @param ctx  EngineContext providing subsystem pointers.
     * @return Configured PhaseSystemManager ready for UpdateAll().
     */
    inline Spark::ECS::PhaseSystemManager CreatePhaseSystemManager(EngineContext& ctx)
    {
        using namespace Spark::ECS;
        PhaseSystemManager mgr;

        // Physics phase
        if (auto* physics = ctx.GetPhysics())
        {
            mgr.AddSystem<PhysicsUpdateSystem>(Phase::Physics, physics);
        }

        // Animation phase
        mgr.AddSystem<AnimationUpdateSystem>(Phase::Animation);

        // Spline followers — after animation, before AI (so spline-following entities
        // have updated positions before AI reads them)
        mgr.AddSystem<SplineFollowerSystem>(Phase::PostPhysics);

        // AI phase
        mgr.AddSystem<AIUpdateSystem>(Phase::AI);

        // Audio phase
        if (auto* audio = ctx.GetAudio())
        {
            mgr.AddSystem<AudioUpdateSystem>(Phase::Audio, audio);
        }

        // Gameplay phase — lifecycle, ability cooldowns, projectiles
        mgr.AddSystem<LifecycleSystem>(Phase::Gameplay);
        mgr.AddSystem<AbilityUpdateSystem>(Phase::Gameplay);
        mgr.AddSystem<ProjectileSystem>(Phase::Gameplay);

        // Pre-render phase — particles, decals, terrain LOD
        mgr.AddSystem<ParticleUpdateSystem>(Phase::PreRender);
        mgr.AddSystem<DecalSystem>(Phase::PreRender);
        mgr.AddSystem<TerrainSystem>(Phase::PreRender);

        // Render phase
        if (auto* graphics = ctx.GetGraphics())
        {
            mgr.AddSystem<RenderSystem>(Phase::Render, graphics);
        }

        return mgr;
    }

    /**
     * @brief Initialize the JobSystem with optimal thread count.
     *
     * Should be called early in engine startup, before any system that uses
     * parallel processing (AI, perception, parallel system executor).
     *
     * @param numThreads  Number of worker threads (0 = hardware_concurrency - 1).
     */
    inline void InitializeJobSystem(uint32_t numThreads = 0)
    {
        auto& jobSystem = Spark::JobSystem::Get();
        if (!jobSystem.IsInitialized())
        {
            jobSystem.Initialize(numThreads);
        }
    }

    /**
     * @brief Create an EngineBootstrap with standard subsystem descriptors.
     *
     * The bootstrap provides an alternative to EngineContext::InitializeAll() with
     * string-named subsystems and richer error reporting.
     *
     * @return Configured EngineBootstrap (call Initialize() to run).
     */
    inline Spark::EngineBootstrap CreateBootstrap()
    {
        Spark::EngineBootstrap bootstrap;

        bootstrap.Register({"JobSystem",
                            []()
                            {
                                auto& js = Spark::JobSystem::Get();
                                if (!js.IsInitialized())
                                    js.Initialize(0);
                                return true;
                            },
                            []()
                            {
                                auto& js = Spark::JobSystem::Get();
                                js.Shutdown();
                            },
                            {}});

        bootstrap.Register({"Timer", []() { return true; }, []() {}, {}});
        bootstrap.Register({"EventBus", []() { return true; }, []() {}, {}});
        bootstrap.Register({"Input", []() { return true; }, []() {}, {"Timer"}});
        bootstrap.Register({"Graphics", []() { return true; }, []() {}, {"Timer"}});
        bootstrap.Register({"Physics", []() { return true; }, []() {}, {"Timer"}});
        bootstrap.Register({"Audio", []() { return true; }, []() {}, {"Timer", "Graphics"}});
        bootstrap.Register({"Animation", []() { return true; }, []() {}, {"Timer"}});
        bootstrap.Register({"AI", []() { return true; }, []() {}, {"Timer", "Physics", "JobSystem"}});
        bootstrap.Register({"SaveSystem", []() { return true; }, []() {}, {}});
        bootstrap.Register({"CoroutineScheduler", []() { return true; }, []() {}, {"Timer"}});
        bootstrap.Register({"SceneManager", []() { return true; }, []() {}, {"Graphics", "Physics"}});
        bootstrap.Register({"Scripting", []() { return true; }, []() {}, {"Timer", "EventBus"}});

        return bootstrap;
    }

} // namespace Spark::EngineSetup
