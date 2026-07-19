/**
 * @file FixedTimestepPhysics.h
 * @brief Fixed-timestep physics system with accumulator pattern (R2.2 — Architecture Analysis)
 *
 * Wraps PhysicsUpdateSystem with a fixed-timestep accumulator to ensure
 * deterministic physics simulation independent of frame rate.
 *
 * ## How it works
 * ```
 * deltaTime is accumulated each frame.
 * While accumulator >= fixedTimestep:
 *     Step physics by fixedTimestep
 *     accumulator -= fixedTimestep
 * interpolationAlpha = accumulator / fixedTimestep
 * ```
 *
 * ## Usage
 * @code
 *   // Replace:
 *   //   mgr.AddSystem<PhysicsUpdateSystem>(physicsPtr);
 *   // With:
 *   mgr.AddSystem<FixedTimestepPhysicsSystem>(physicsPtr, 1.0f/60.0f);
 *
 *   // For rendering interpolation:
 *   float alpha = fixedPhys->GetInterpolationAlpha();
 * @endcode
 *
 * @warning This system advances the simulation via PhysicsSystem::StepFixed() and
 *          therefore becomes the process's single stepping owner. Per the
 *          "Stepping contract" in Physics/PhysicsSystem.h, the host game module
 *          or exe must NOT also call PhysicsSystem::Update()/StepFixed() — doing
 *          so double-steps the world and breaks determinism.
 */

#pragma once

#include "ECSystems.h"
#include "../../../Physics/PhysicsSystem.h"
#include <algorithm>

namespace Spark::ECS
{

    /**
     * @brief Physics system with fixed-timestep accumulator (R2.2).
     *
     * Steps the physics simulation in fixed increments regardless of
     * variable frame rate. Provides an interpolation alpha for smooth
     * rendering between physics steps. Body auto-creation and the
     * kinematic ECS→Jolt / dynamic Jolt→ECS transform sync are delegated
     * to an internal PhysicsUpdateSystem, so this is a drop-in replacement
     * for it (see the stepping-ownership warning in the file header).
     */
    class FixedTimestepPhysicsSystem : public ISystem
    {
      public:
        /**
         * @brief Construct with physics system and fixed step size.
         *
         * @param physics        Non-owning pointer to the PhysicsSystem. Must not be null.
         * @param fixedTimestep  Physics step size in seconds (default 1/60).
         */
        explicit FixedTimestepPhysicsSystem(Spark::NonNull<PhysicsSystem*> physics, float fixedTimestep = 1.0f / 60.0f)
            : m_physics(physics), m_syncSystem(physics, fixedTimestep), m_fixedTimestep(fixedTimestep)
        {
            SPARK_EXPECTS(fixedTimestep > 0.0f);
        }

        /**
         * @brief Accumulate time and step physics at fixed intervals.
         *
         * 1. Clamps deltaTime to prevent spiral of death (max 0.25s).
         * 2. Accumulates deltaTime and converts it to whole fixed ticks.
         * 3. Advances the simulation via PhysicsSystem::StepFixed().
         * 4. Computes interpolation alpha for rendering.
         * 5. Syncs transforms between ECS and Jolt (via PhysicsUpdateSystem):
         *    - Auto-creates missing physics bodies
         *    - Kinematic: ECS → Jolt
         *    - Dynamic: Jolt → ECS
         */
        void Update(World& world, float deltaTime) override
        {
            // Clamp to prevent spiral of death
            float clampedDt = std::min(deltaTime, m_maxAccumulator);
            m_accumulator += clampedDt;

            m_stepCount = 0;
            while (m_accumulator >= m_fixedTimestep)
            {
                m_accumulator -= m_fixedTimestep;
                m_stepCount++;
            }

            // Interpolation alpha for rendering between physics states
            m_interpolationAlpha = m_accumulator / m_fixedTimestep;

            // Advance the Jolt simulation by the elapsed whole ticks. StepFixed
            // runs ticks of GetTimeStep() seconds, so keep it in sync with our
            // step size (SetFixedTimestep may have changed it). stepCount == 0
            // is valid — it only refreshes the physics-side interpolation alpha.
            m_physics->SetTimeStep(m_fixedTimestep);
            m_physics->StepFixed(m_stepCount, m_interpolationAlpha);

            // Post-step: body auto-creation, kinematic ECS → Jolt push, and
            // dynamic Jolt → ECS read-back are identical to the variable-rate
            // path, so delegate to PhysicsUpdateSystem instead of duplicating it.
            // Kinematic targets pushed here take effect on the next tick.
            m_syncSystem.Update(world, clampedDt);
        }

        const char* GetName() const override { return "FixedTimestepPhysicsSystem"; }

        /** @brief Interpolation factor [0..1) for rendering between physics steps. */
        float GetInterpolationAlpha() const { return m_interpolationAlpha; }

        /** @brief The fixed timestep size in seconds. */
        float GetFixedTimestep() const { return m_fixedTimestep; }

        /** @brief Set a new fixed timestep (e.g. for slow-motion). */
        void SetFixedTimestep(float timestep) { m_fixedTimestep = timestep; }

        /** @brief Number of physics steps taken in the last Update(). */
        uint32_t GetLastStepCount() const { return m_stepCount; }

        /** @brief Get the raw physics system pointer. */
        PhysicsSystem* GetPhysicsSystem() const { return m_physics; }

      private:
        PhysicsSystem* m_physics;         ///< Non-owning; never null (NonNull-checked at construction).
        PhysicsUpdateSystem m_syncSystem; ///< Delegate for body creation and ECS↔Jolt transform sync.
        float m_fixedTimestep = 1.0f / 60.0f;
        float m_accumulator = 0.0f;
        float m_maxAccumulator = 0.25f;
        float m_interpolationAlpha = 0.0f;
        uint32_t m_stepCount = 0;
    };

} // namespace Spark::ECS
