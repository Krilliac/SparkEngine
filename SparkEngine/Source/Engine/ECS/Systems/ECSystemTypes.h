/**
 * @file ECSystemTypes.h
 * @brief Base types and interfaces shared by all ECS systems.
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * This header defines the foundational types for the ECS system layer:
 * - `ISystem` – the abstract base interface that every ECS system implements.
 *
 * Extracted from ECSystems.h so that downstream headers that only need the base
 * interface (e.g. Systems2D.h, TerrainSystem.h, ParallelSystemExecutor.h) can
 * include this lightweight header instead of pulling in all concrete system
 * definitions.
 *
 * ## ECS design principle
 * ```
 * Entity  = unique ID (a number)
 * Component = plain data struct  (Transform, RigidBodyComponent, etc.)
 * System  = logic class that iterates entities with specific components
 * ```
 *
 * By separating data from logic:
 * - Components are cache-friendly (SoA layout via EnTT).
 * - Systems can be enabled/disabled without modifying entity data.
 * - New behavior is added by creating a new system, not modifying existing objects.
 */

#pragma once

// Forward declaration — World is defined in Components.h at global scope.
class World;

namespace Spark::ECS
{

    // =============================================================================
    // Base System Interface
    // =============================================================================

    /**
 * @class ISystem
 * @brief Abstract base interface for all ECS systems.
 *
 * Every system that participates in the ECS update loop must inherit from ISystem
 * and implement:
 * - `Update(World&, float)` – the per-frame logic.
 * - `GetName()` – a unique debug name for logging and the editor's system panel.
 *
 * Systems may be enabled or disabled at runtime via `SetEnabled()`. Disabled
 * systems are skipped by `SystemManager::UpdateAll()`.
 *
 * ### Custom system example
 * @code
 *   class MyGravitySystem : public ISystem {
 *   public:
 *       void Update(World& world, float dt) override {
 *           for (auto [e, rb, tf] : world.GetEntitiesWith<RigidBodyComponent, Transform>().each()) {
 *               if (rb.type == RigidBodyComponent::Type::Dynamic)
 *                   rb.linearVelocity.y -= 9.81f * dt;
 *           }
 *       }
 *       const char* GetName() const override { return "MyGravitySystem"; }
 *   };
 * @endcode
 */
    class ISystem
    {
      public:
        /** @brief Virtual destructor ensures correct cleanup in derived classes. */
        virtual ~ISystem() = default;

        /**
     * @brief Execute the system's per-frame logic against the entity world.
     *
     * Called once per frame by `SystemManager::UpdateAll()` (unless disabled).
     * Implementations should query the world for relevant component combinations and
     * apply logic without holding references across frames.
     *
     * @param world      The ECS World containing all entities and components.
     * @param deltaTime  Time elapsed since the previous frame (seconds). Used for
     *                   frame-rate independent simulation and animation.
     */
        virtual void Update(World& world, float deltaTime) = 0;

        /**
     * @brief Return a stable debug name for this system.
     *
     * Used by the SparkEditor systems panel, profiler, and debug logs. Should be
     * a short, unique, human-readable string (e.g. "RenderSystem", "AIUpdateSystem").
     *
     * @return  C-string name of the system.
     */
        virtual const char* GetName() const = 0;

        /**
     * @brief Query whether this system is currently active.
     *
     * Disabled systems are not updated by `SystemManager::UpdateAll()`.
     *
     * @return  `true` if the system will be updated this frame.
     */
        virtual bool IsEnabled() const { return m_enabled; }

        /**
     * @brief Enable or disable this system.
     *
     * Disabled systems retain their internal state; they can be re-enabled at any
     * time to resume operation. Use this to pause expensive systems (e.g. AIUpdateSystem)
     * during loading screens or cinematic sequences.
     *
     * @param enabled  `true` to activate, `false` to deactivate.
     */
        virtual void SetEnabled(bool enabled) { m_enabled = enabled; }

      protected:
        /** @brief Whether this system participates in each `UpdateAll()` call. Default: true. */
        bool m_enabled = true;
    };

} // namespace Spark::ECS
