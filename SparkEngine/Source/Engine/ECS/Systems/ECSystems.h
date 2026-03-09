/**
 * @file ECSystems.h
 * @brief ECS system definitions that process component data each frame
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * This file implements the "Systems" layer of the Entity Component System pattern.
 * Systems hold the **logic**; they query specific component combinations from the
 * World and update them. Components (see Components.h) are pure data.
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
 *
 * ## System execution order
 * The `SystemManager::UpdateAll()` processes systems in the order they were added.
 * The recommended order for correctness is:
 *
 * 1. **PhysicsUpdateSystem** – Runs physics simulation step, writes results to Transform.
 * 2. **AnimationUpdateSystem** – Evaluates skeleton animation, produces bone matrices.
 * 3. **AIUpdateSystem** – Reads Transform, runs behavior trees, writes velocity/target.
 * 4. **AudioUpdateSystem** – Reads Transform, updates 3D audio source positions.
 * 5. **LifecycleSystem** – Processes health, death callbacks, active/inactive entities.
 * 6. **RenderSystem** – Reads Transform + MeshRenderer, submits draw calls to GPU.
 *
 * ## Usage
 * @code
 *   SystemManager sysManager;
 *
 *   // Register systems in execution order
 *   sysManager.AddSystem<PhysicsUpdateSystem>(physicsSystem);
 *   sysManager.AddSystem<AnimationUpdateSystem>();
 *   sysManager.AddSystem<AIUpdateSystem>();
 *   sysManager.AddSystem<AudioUpdateSystem>(audioEngine);
 *   sysManager.AddSystem<LifecycleSystem>();
 *   sysManager.AddSystem<RenderSystem>(graphicsEngine);
 *
 *   // Per-frame update
 *   sysManager.UpdateAll(world, deltaTime);
 * @endcode
 *
 * @note All systems operate on the same World instance. Systems communicate
 *       indirectly through shared components, never by calling each other directly.
 */

#pragma once

#include "../Components.h"
#include <functional>
#include <vector>
#include <string>
#include <string_view>

// Forward declarations for engine subsystems that systems depend on.
// Full headers are included in the .cpp implementations.
class GraphicsEngine; ///< Rendering backend (DirectX 11)
class PhysicsSystem;  ///< Bullet Physics simulation world
class AudioEngine;    ///< XAudio2 audio backend

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

    // =============================================================================
    // Render System
    // =============================================================================

    /**
 * @class RenderSystem
 * @brief Submits renderable entities (MeshRenderer + Transform) to the GraphicsEngine.
 *
 * The RenderSystem queries all entities that have both a `MeshRenderer` and a `Transform`
 * component. For each such entity it builds a world matrix from the Transform and issues
 * a draw call to the `GraphicsEngine`.
 *
 * ### Culling
 * The system respects `MeshRenderer::visible = false` — invisible entities are skipped
 * without removing their components. This allows objects to be toggled without any
 * allocation overhead.
 *
 * ### Shadow pass
 * `MeshRenderer::castShadows` is forwarded to the graphics engine's shadow map render
 * pass. The shadow pass is a separate draw call issued by the engine before the main pass.
 *
 * ### Dependency
 * Must be initialized with a valid `GraphicsEngine*`. The graphics engine must have
 * already completed `BeginFrame()` before this system's `Update()` is called.
 *
 * @note This system reads Transform and MeshRenderer but does NOT write to them.
 *       It is safe to run after physics and animation systems have updated transforms.
 */
    class RenderSystem : public ISystem
    {
      public:
        /**
     * @brief Construct the RenderSystem with a reference to the graphics engine.
     *
     * @param graphics  Non-owning pointer to the active GraphicsEngine. Must not be null.
     *                  The engine must remain alive for the lifetime of this system.
     */
        RenderSystem(GraphicsEngine* graphics) : m_graphics(graphics) {}

        /**
     * @brief Iterate all (MeshRenderer, Transform) entities and submit draw calls.
     *
     * For each visible entity:
     * 1. Reads the Transform to build a world matrix.
     * 2. Resolves the mesh and material via the AssetPipeline.
     * 3. Sets per-object constant buffer data.
     * 4. Issues `GraphicsEngine::DrawMesh()` for the main render pass.
     * 5. Issues a separate shadow-map draw if `castShadows == true`.
     *
     * @param world      The ECS World to query.
     * @param deltaTime  Unused by the render system but required by the interface.
     */
        void Update(World& world, float deltaTime) override;

        const char* GetName() const override { return "RenderSystem"; }

        /**
     * @brief Return the number of entities submitted for rendering in the last frame.
     *
     * Useful for monitoring scene complexity and render budget. A high count may
     * indicate that culling or LOD systems should be enabled.
     *
     * @return  Count of entities rendered (visible + having a mesh) in the last Update() call.
     */
        int GetRenderedEntityCount() const { return m_renderedCount; }

      private:
        /** @brief Non-owning reference to the graphics engine used to issue draw calls. */
        GraphicsEngine* m_graphics;

        /** @brief Entities submitted for rendering in the most recent Update() call. */
        int m_renderedCount = 0;
    };

    // =============================================================================
    // Physics Update System
    // =============================================================================

    /**
 * @class PhysicsUpdateSystem
 * @brief Synchronizes ECS Transform components with Bullet Physics rigid body simulation.
 *
 * This system bridges the ECS world and the PhysicsSystem. It operates in two phases:
 *
 * **Phase 1 – Pre-simulate write-back:**
 * For Kinematic rigid bodies, reads the ECS Transform (which may have been modified
 * by animation or script) and writes the new position/rotation into the Bullet
 * rigid body so it influences other Dynamic bodies.
 *
 * **Phase 2 – Post-simulate read-back:**
 * After `PhysicsSystem::Update()` runs the simulation step, reads the new
 * position/rotation from each Dynamic rigid body back into the ECS Transform
 * component so that downstream systems (RenderSystem, AudioUpdateSystem) see
 * the correct positions.
 *
 * ### Execution order
 * Must run **before** RenderSystem and AudioUpdateSystem so they see up-to-date
 * transform data. Should run **after** any script or animation systems that may
 * set a Kinematic body's target position.
 *
 * @note Static bodies (PhysicsBodyType::Static) are never written back to Transform
 *       since they never move. Their Transform is only written once at creation time.
 */
    class PhysicsUpdateSystem : public ISystem
    {
      public:
        /**
     * @brief Construct with a reference to the physics simulation.
     *
     * @param physics  Non-owning pointer to the PhysicsSystem. Must not be null.
     *                 The physics system must remain alive for the lifetime of this object.
     */
        PhysicsUpdateSystem(PhysicsSystem* physics) : m_physics(physics) {}

        /**
     * @brief Sync transforms between the ECS world and the physics simulation.
     *
     * - Kinematic entities: writes ECS Transform → Bullet rigid body.
     * - Dynamic entities: reads Bullet rigid body → ECS Transform.
     * - Static entities: no-op.
     *
     * @param world      The ECS World containing RigidBodyComponent and Transform.
     * @param deltaTime  Physics time step (seconds), forwarded to PhysicsSystem::Update().
     */
        void Update(World& world, float deltaTime) override;

        const char* GetName() const override { return "PhysicsUpdateSystem"; }

      private:
        /** @brief Non-owning pointer to the Bullet Physics simulation world. */
        PhysicsSystem* m_physics;
    };

    // =============================================================================
    // Audio Update System
    // =============================================================================

    /**
 * @class AudioUpdateSystem
 * @brief Updates 3D audio source positions from entity Transform components.
 *
 * The AudioUpdateSystem synchronizes the spatial position and velocity of
 * AudioSourceComponent audio sources with the world positions of their entities.
 * This allows sounds attached to moving enemies, vehicles, or projectiles to
 * automatically follow the entity in 3D space.
 *
 * ### Per-frame behavior
 * For each entity with both `AudioSourceComponent` and `Transform`:
 * 1. Reads the entity's world-space position from Transform.
 * 2. Computes per-frame velocity from position delta for Doppler effects.
 * 3. Writes position and velocity to the underlying `AudioSource` via the AudioEngine.
 *
 * ### Listener position
 * The 3D audio listener (typically the camera / player head) must be updated
 * separately via `AudioEngine::Console_SetListenerPosition()` or by the Player
 * system each frame.
 *
 * @note Only entities with `AudioSourceComponent::is3D == true` benefit from this
 *       system. 2D sources are unaffected by position changes.
 */
    class AudioUpdateSystem : public ISystem
    {
      public:
        /**
     * @brief Construct with a reference to the audio engine.
     *
     * @param audio  Non-owning pointer to the AudioEngine. Must not be null.
     */
        AudioUpdateSystem(AudioEngine* audio) : m_audio(audio) {}

        /**
     * @brief Sync 3D audio source positions from entity transforms.
     *
     * Iterates all entities with AudioSourceComponent + Transform.
     * Writes world position and computed velocity to the AudioEngine.
     *
     * @param world      The ECS World to query.
     * @param deltaTime  Frame time used to compute position-delta velocity (seconds).
     */
        void Update(World& world, float deltaTime) override;

        const char* GetName() const override { return "AudioUpdateSystem"; }

      private:
        /** @brief Non-owning pointer to the XAudio2-based audio engine. */
        AudioEngine* m_audio;
    };

    // =============================================================================
    // Lifecycle System
    // =============================================================================

    /**
 * @class LifecycleSystem
 * @brief Manages entity activation state and death events.
 *
 * The LifecycleSystem monitors `ActiveComponent` and `HealthComponent` each frame:
 *
 * - **Inactive entities** – entities with `ActiveComponent::active == false` are
 *   skipped by most other systems, achieving a cheap "disabled" state without
 *   removing components. The LifecycleSystem itself still visits them to detect
 *   re-activation.
 *
 * - **Death detection** – entities with `HealthComponent::isDead == true` trigger
 *   the registered death callback (`m_onDeath`). The callback receives the EntityID
 *   so the game can handle loot drops, score, sound effects, and eventual entity
 *   destruction.
 *
 * ### Death callback
 * Register a callback before any enemies can die:
 * @code
 *   lifecycleSys->SetDeathCallback([&](EntityID id) {
 *       // Drop loot, play death sound, award score...
 *       world.DestroyEntity(id);
 *   });
 * @endcode
 *
 * @note The callback is invoked **once** per entity per death event. The entity
 *       is NOT automatically destroyed; the callback is responsible for that.
 *       To prevent the callback firing again, either destroy the entity or clear
 *       the `HealthComponent::isDead` flag.
 */
    class LifecycleSystem : public ISystem
    {
      public:
        /**
     * @brief Callback signature invoked when an entity's health reaches zero.
     *
     * @param entityID  The EntityID of the entity that died.
     */
        using DeathCallback = std::function<void(EntityID)>;

        /**
     * @brief Scan all entities for death and activation state changes.
     *
     * - Fires `m_onDeath` for each entity with `HealthComponent::isDead == true`.
     * - (Future) handles re-activation of entities that transition to `active == true`.
     *
     * @param world      The ECS World to query.
     * @param deltaTime  Frame time (seconds). May be used for deferred actions.
     */
        void Update(World& world, float deltaTime) override;

        const char* GetName() const override { return "LifecycleSystem"; }

        /**
     * @brief Register the callback invoked when an entity's HealthComponent marks it dead.
     *
     * Only one callback can be registered; subsequent calls overwrite the previous one.
     * Pass an empty `std::function` to remove the callback.
     *
     * @param cb  Callback function receiving the EntityID of the dead entity.
     */
        void SetDeathCallback(DeathCallback cb) { m_onDeath = cb; }

      private:
        /**
     * @brief Callback invoked when `HealthComponent::isDead` is detected.
     *
     * Set via `SetDeathCallback()`. May be empty (no-op) if not registered.
     */
        DeathCallback m_onDeath;
    };

    // =============================================================================
    // Animation Update System
    // =============================================================================

    /**
 * @class AnimationUpdateSystem
 * @brief Evaluates skeletal animation for all entities with AnimationController.
 *
 * Each frame the AnimationUpdateSystem:
 * 1. Locates or creates an `AnimationInstance` for each entity with an
 *    `AnimationController` component.
 * 2. Advances the state machine and blend layer playback times.
 * 3. Calls `AnimationEvaluator::SampleClip()` and `BlendTransforms()` to
 *    produce local bone transforms.
 * 4. Calls `AnimationEvaluator::ComputeSkinningMatrices()` to produce the
 *    final GPU-ready bone matrices.
 * 5. Solves any enabled IK chains via the IK evaluators.
 * 6. Uploads the final bone matrix array to the per-entity GPU constant buffer.
 *
 * ### Execution order
 * Should run BEFORE RenderSystem so that the GPU buffer is filled before the
 * render pass reads from it. May run AFTER PhysicsUpdateSystem if root motion
 * is used to feed position data back into physics.
 *
 * @note AnimationUpdateSystem stores per-entity `AnimationInstance` objects
 *       internally (keyed by EntityID). These are lazily created on first
 *       encounter and destroyed when the entity or component is removed.
 */
    class AnimationUpdateSystem : public ISystem
    {
      public:
        /**
     * @brief Advance animation playback for all animated entities.
     *
     * Queries all entities with AnimationController, updates state machines,
     * evaluates clips, blends layers, solves IK, and uploads bone matrices to GPU.
     *
     * @param world      The ECS World to query.
     * @param deltaTime  Frame time in seconds used to advance animation playback.
     */
        void Update(World& world, float deltaTime) override;

        const char* GetName() const override { return "AnimationUpdateSystem"; }
    };

    // =============================================================================
    // AI Update System
    // =============================================================================

    /**
 * @class AIUpdateSystem
 * @brief Processes AI agent perception, behavior trees, and pathfinding each frame.
 *
 * The AIUpdateSystem iterates all entities with an `AIComponent` and performs:
 *
 * 1. **Perception update** – Updates line-of-sight and hearing checks.
 *    Writes observation data to the agent's behavior tree Blackboard
 *    (e.g. `"enemyVisible"`, `"targetPos"`, `"distToTarget"`).
 *
 * 2. **Behavior tree tick** – Calls `BehaviorTree::Tick(deltaTime)` on each
 *    agent's behavior tree instance. Behavior nodes read/write the Blackboard
 *    and update `AIComponent::state`.
 *
 * 3. **Pathfinding** – If the behavior tree requests a new path (via Blackboard
 *    key `"newPathRequested"`), queries the NavMeshQuery to compute a path and
 *    stores it in `AIComponent::currentPath`.
 *
 * 4. **Movement** – Advances the agent along `AIComponent::currentPath` by
 *    modifying its Transform at `AIComponent::config.moveSpeed` m/s.
 *    Integrates with the physics system for collision-aware movement when
 *    a RigidBodyComponent is also present.
 *
 * ### Execution order
 * Should run AFTER PhysicsUpdateSystem (so it sees up-to-date positions) and
 * BEFORE RenderSystem and AudioUpdateSystem (so they see the correct positions
 * after AI movement).
 *
 * @note The AIUpdateSystem respects `AIComponent::state == State::Dead` — dead
 *       agents are skipped entirely, avoiding unnecessary behavior tree ticks.
 */
    class AIUpdateSystem : public ISystem
    {
      public:
        /**
     * @brief Process all AI agents for one simulation frame.
     *
     * Iterates entities with AIComponent, runs perception, ticks behavior trees,
     * queries pathfinding, and applies movement.
     *
     * @param world      The ECS World to query.
     * @param deltaTime  Frame time in seconds for speed-independent AI updates.
     */
        void Update(World& world, float deltaTime) override;

        const char* GetName() const override { return "AIUpdateSystem"; }
    };

    // =============================================================================
    // System Manager
    // =============================================================================

    /**
 * @class SystemManager
 * @brief Owns, orders, and updates all ECS systems.
 *
 * SystemManager is the single point of control for the ECS update loop. Systems
 * are added via `AddSystem<T>()` and stored in a `std::vector<unique_ptr<ISystem>>`
 * preserving insertion order. `UpdateAll()` calls each enabled system in order
 * each frame.
 *
 * ### Ownership
 * The SystemManager exclusively owns all systems via `unique_ptr`. Systems are
 * destroyed when the SystemManager is destroyed.
 *
 * ### Thread safety
 * `AddSystem` and `UpdateAll` must be called from the same thread. Individual
 * system `Update()` implementations may launch background tasks but must not
 * call SystemManager methods from those tasks.
 *
 * @code
 *   SystemManager mgr;
 *   mgr.AddSystem<PhysicsUpdateSystem>(physicsPtr);
 *   mgr.AddSystem<RenderSystem>(graphicsPtr);
 *
 *   // Per-frame in the game loop:
 *   mgr.UpdateAll(world, deltaTime);
 *
 *   // Disable AI during cutscenes:
 *   mgr.GetSystem("AIUpdateSystem")->SetEnabled(false);
 * @endcode
 */
    class SystemManager
    {
      public:
        SystemManager() = default;
        ~SystemManager() = default;

        /**
     * @brief Construct and register a system of type T.
     *
     * Systems are executed in the order they are added. Constructor arguments for
     * T are forwarded via perfect forwarding.
     *
     * @tparam T     System type to create (must inherit from ISystem).
     * @tparam Args  Constructor argument types, deduced automatically.
     * @param  args  Arguments forwarded to T's constructor.
     * @return       Non-owning pointer to the newly created system (owned by this manager).
     *
     * @code
     *   auto* render = mgr.AddSystem<RenderSystem>(graphicsEngine);
     *   render->SetEnabled(false);  // disable initially
     * @endcode
     */
        template <typename T, typename... Args> T* AddSystem(Args&&... args)
        {
            auto system = std::make_unique<T>(std::forward<Args>(args)...);
            T* ptr = system.get();
            m_systems.push_back(std::move(system));
            return ptr;
        }

        /**
     * @brief Update all enabled systems in registration order.
     *
     * Iterates the internal system list and calls `Update(world, deltaTime)` on
     * each system that returns `IsEnabled() == true`. Disabled systems are skipped
     * entirely (their `Update()` is never called).
     *
     * @param world      The ECS World shared by all systems.
     * @param deltaTime  Frame time in seconds passed to every system.
     */
        void UpdateAll(World& world, float deltaTime)
        {
            for (auto& system : m_systems)
            {
                if (system->IsEnabled())
                    system->Update(world, deltaTime);
            }
        }

        /**
     * @brief Find a system by its name.
     *
     * Performs a linear search comparing `GetName()` to the supplied string.
     * Used for runtime configuration (enable/disable, console access).
     *
     * @param name  The exact string returned by the system's `GetName()` method.
     * @return      Non-owning pointer to the matching system, or `nullptr` if not found.
     *
     * @code
     *   auto* ai = mgr.GetSystem("AIUpdateSystem");
     *   if (ai) ai->SetEnabled(false);  // pause AI during cutscene
     * @endcode
     */
        ISystem* GetSystem(std::string_view name)
        {
            for (auto& system : m_systems)
            {
                if (name == system->GetName())
                    return system.get();
            }
            return nullptr;
        }

        const ISystem* GetSystem(std::string_view name) const
        {
            for (const auto& system : m_systems)
            {
                if (name == system->GetName())
                    return system.get();
            }
            return nullptr;
        }

        /**
     * @brief Return the total number of registered systems (enabled and disabled).
     * @return  Count of systems registered with this manager.
     */
        size_t GetSystemCount() const { return m_systems.size(); }

        /**
     * @brief List all registered systems and their enabled state (console integration).
     *
     * @return  Newline-separated string of "SystemName: enabled/disabled" entries.
     */
        std::string Console_ListSystems() const;

      private:
        /**
     * @brief Ordered collection of all registered systems.
     *
     * Unique ownership via `unique_ptr`; systems are processed in insertion order.
     */
        std::vector<std::unique_ptr<ISystem>> m_systems;
    };

} // namespace Spark::ECS
