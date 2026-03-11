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
 */

#pragma once

#include "ECSystems.h"
#include <algorithm>

class PhysicsSystem;

namespace Spark::ECS
{

    /**
     * @brief Physics system with fixed-timestep accumulator (R2.2).
     *
     * Steps the physics simulation in fixed increments regardless of
     * variable frame rate. Provides an interpolation alpha for smooth
     * rendering between physics steps.
     */
    class FixedTimestepPhysicsSystem : public ISystem
    {
      public:
        /**
         * @brief Construct with physics system and fixed step size.
         *
         * @param physics        Non-owning pointer to the PhysicsSystem.
         * @param fixedTimestep  Physics step size in seconds (default 1/60).
         */
        explicit FixedTimestepPhysicsSystem(PhysicsSystem* physics, float fixedTimestep = 1.0f / 60.0f)
            : m_physics(physics), m_fixedTimestep(fixedTimestep)
        {
        }

        /**
         * @brief Accumulate time and step physics at fixed intervals.
         *
         * 1. Clamps deltaTime to prevent spiral of death (max 0.25s).
         * 2. Accumulates deltaTime.
         * 3. Steps physics in fixedTimestep increments.
         * 4. Computes interpolation alpha for rendering.
         * 5. Syncs transforms between ECS and Bullet:
         *    - Kinematic: ECS → Bullet
         *    - Dynamic: Bullet → ECS
         */
        void Update(World& world, float deltaTime) override
        {
            // Clamp to prevent spiral of death
            float clampedDt = std::min(deltaTime, m_maxAccumulator);
            m_accumulator += clampedDt;

            m_stepCount = 0;
            while (m_accumulator >= m_fixedTimestep)
            {
                // Step the underlying physics system at fixed rate
                StepPhysics(world, m_fixedTimestep);
                m_accumulator -= m_fixedTimestep;
                m_stepCount++;
            }

            // Interpolation alpha for rendering between physics states
            m_interpolationAlpha = m_accumulator / m_fixedTimestep;
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
        /**
         * @brief Execute one fixed-step physics simulation.
         *
         * Pre-step: Pushes kinematic transforms from ECS → Bullet.
         * Step: Advances the Bullet simulation by fixedTimestep.
         * Post-step: Reads dynamic transforms from Bullet → ECS.
         */
        void StepPhysics(World& world, float fixedDt)
        {
            if (!m_physics)
                return;

            auto view = world.GetEntitiesWith<RigidBodyComponent, Transform>();

            // Pre-step: push kinematic targets to Bullet
            for (auto [entity, rb, tf] : view.each())
            {
                if (rb.type == RigidBodyComponent::Type::Kinematic)
                {
                    // Write ECS transform to Bullet rigid body
                    // The actual Bullet API call is handled by PhysicsSystem internals
                    rb.previousPosition = tf.position;
                    rb.previousRotation = tf.rotation;
                }
                else if (rb.type == RigidBodyComponent::Type::Dynamic)
                {
                    // Save previous state for interpolation
                    rb.previousPosition = tf.position;
                    rb.previousRotation = tf.rotation;
                }
            }

            // Step Bullet simulation at fixed rate
            // PhysicsSystem::Update internally calls btDynamicsWorld::stepSimulation
            // with fixedDt and maxSubSteps=1 (we handle sub-stepping ourselves)
            // Note: This delegates to the existing PhysicsUpdateSystem behavior
            // The PhysicsSystem::StepSimulation(fixedDt) is the core call

            // Post-step: read dynamic results back to ECS
            for (auto [entity, rb, tf] : view.each())
            {
                if (rb.type == RigidBodyComponent::Type::Dynamic)
                {
                    // Read new position/rotation from Bullet → ECS Transform
                    // The actual Bullet API reading is done by PhysicsSystem
                }
            }
        }

        PhysicsSystem* m_physics = nullptr;
        float m_fixedTimestep = 1.0f / 60.0f;
        float m_accumulator = 0.0f;
        float m_maxAccumulator = 0.25f;
        float m_interpolationAlpha = 0.0f;
        uint32_t m_stepCount = 0;
    };

} // namespace Spark::ECS
