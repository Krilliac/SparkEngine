/**
 * @file ECSIntegration.h
 * @brief Aggregates and cross-links all ECS architecture extensions
 *
 * This header consolidates the architecture improvements into a single
 * include for consumers that want the full ECS feature set:
 *
 * - PhaseSystemManager (R2.3): Deterministic phase-ordered execution
 * - FixedTimestepPhysics (R2.2): Accumulator-based fixed-step physics
 * - ReactiveSystem (R2.4): EnTT signal-based change-only processing
 * - ParallelSystemExecutor (R2.1): Automatic parallel system execution
 *
 * @see ECSystems.h for the base system definitions
 */

#pragma once

// Base ECS
#include "Systems/ECSystems.h"

// Architecture extensions
#include "Systems/PhaseSystemManager.h"
#include "Systems/FixedTimestepPhysics.h"
#include "Systems/ParallelSystemExecutor.h"
#include "ReactiveSystem.h"

// Forward declarations
class GraphicsEngine;
class PhysicsSystem;
class AudioEngine;

namespace Spark::ECS
{

    /**
     * @brief Factory that creates a fully-configured PhaseSystemManager for a game.
     *
     * Registers all standard engine systems in their correct phases and optionally
     * wraps the physics phase with a FixedTimestepPhysicsSystem for deterministic
     * simulation.
     *
     * @param graphics   Non-owning pointer to GraphicsEngine (may be nullptr for headless).
     * @param physics    Non-owning pointer to PhysicsSystem (may be nullptr to skip physics).
     * @param audio      Non-owning pointer to AudioEngine (may be nullptr for headless).
     * @param fixedTimestep  Physics timestep in seconds (default 1/60).
     * @return Configured PhaseSystemManager.
     */
    inline PhaseSystemManager CreateGameSystems(GraphicsEngine* graphics, PhysicsSystem* physics, AudioEngine* audio,
                                                float fixedTimestep = 1.0f / 60.0f)
    {
        PhaseSystemManager mgr;

        // Physics phase: fixed-timestep system wraps the physics simulation
        if (physics)
        {
            mgr.AddSystem<PhysicsUpdateSystem>(Phase::Physics, physics, fixedTimestep);
        }

        // Animation phase
        mgr.AddSystem<AnimationUpdateSystem>(Phase::Animation);

        // AI phase
        mgr.AddSystem<AIUpdateSystem>(Phase::AI);

        // Audio phase
        if (audio)
        {
            mgr.AddSystem<AudioUpdateSystem>(Phase::Audio, audio);
        }

        // Gameplay phase: lifecycle management (health, death, active state)
        mgr.AddSystem<LifecycleSystem>(Phase::Gameplay);

        // Render phase
        if (graphics)
        {
            mgr.AddSystem<RenderSystem>(Phase::Render, graphics);
        }

        return mgr;
    }

    /**
     * @brief Set up reactive systems on a World for automatic dirty-tracking.
     *
     * Connects EnTT signals to reactive systems so that changes to specific
     * components are tracked without polling. Call once after World creation.
     *
     * @param world  The ECS World to attach reactive signals to.
     * @return A vector of reactive systems that should be updated each frame.
     */
    inline std::vector<std::unique_ptr<ISystem>> CreateReactiveSystems(World& world)
    {
        std::vector<std::unique_ptr<ISystem>> systems;

        // Material change tracking — only re-upload material data when MeshRenderer changes
        auto materialReactive = std::make_unique<MaterialChangeReactiveSystem>();
        materialReactive->ConnectTo(world);
        systems.push_back(std::move(materialReactive));

        // Light change tracking — only rebuild shadow maps when lights change
        auto lightReactive = std::make_unique<LightChangeReactiveSystem>();
        lightReactive->ConnectTo(world);
        systems.push_back(std::move(lightReactive));

        return systems;
    }

    /**
     * @brief Configure a ParallelSystemExecutor with default system access declarations.
     *
     * Analyzes which components each system type reads/writes and builds
     * non-conflicting batches for parallel execution.
     *
     * @param executor  The executor to configure.
     * @param mgr       PhaseSystemManager containing registered systems.
     */
    inline void ConfigureParallelExecution(ParallelSystemExecutor& executor, const PhaseSystemManager& mgr)
    {
        // Physics: writes Transform, reads RigidBody (exclusive)
        executor.DeclareAccess("PhysicsUpdateSystem", {"Transform"}, {"RigidBodyComponent"});

        // Animation: writes AnimationController (exclusive)
        executor.DeclareAccess("AnimationUpdateSystem", {"AnimationController"}, {});

        // AI: reads Transform, writes AIComponent (exclusive)
        executor.DeclareAccess("AIUpdateSystem", {"AIComponent"}, {"Transform", "HealthComponent"});

        // Audio: reads Transform, AudioSourceComponent (read-only)
        executor.DeclareAccess("AudioUpdateSystem", {}, {"Transform", "AudioSourceComponent"});

        // Lifecycle: reads/writes HealthComponent, ActiveComponent
        executor.DeclareAccess("LifecycleSystem", {"HealthComponent", "ActiveComponent"}, {});

        // Render: reads Transform, MeshRenderer (read-only)
        executor.DeclareAccess("RenderSystem", {}, {"Transform", "MeshRenderer"});
    }

} // namespace Spark::ECS
