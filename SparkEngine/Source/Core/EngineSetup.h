/**
 * @file EngineSetup.h
 * @brief Engine startup helpers: ECS phase wiring, JobSystem start, bootstrap descriptors
 *
 * Subsystem ownership and init/shutdown order are not decided here:
 * EngineRuntime owns every engine-lifetime subsystem and
 * LifecycleCompositionRoot orders startup and teardown (OD-01).
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

namespace Spark::EngineSetup
{

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
     * Descriptors are string-named subsystems with declared dependencies; the
     * bootstrap orders them and reports which one failed.
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
