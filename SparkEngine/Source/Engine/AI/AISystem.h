/**
 * @file AISystem.h
 * @brief ECS system for AI agent processing and coordination
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * This file defines the `AISystem` — the ECS system responsible for driving all
 * AI-controlled entities in the game. It integrates three subsystems:
 *
 * - **Behavior Trees** (see BehaviorTree.h): define *what* agents decide to do.
 * - **NavMesh** (see NavMesh.h): defines *where* agents can go and how to get there.
 * - **ECS** (see ECSystems.h / Components.h): provides per-entity state via `AIComponent`.
 *
 * ## Architecture
 *
 * ```
 * AISystem
 *   ├── m_behaviorTemplates  (shared BehaviorTree blueprints registered at startup)
 *   ├── m_behaviorInstances  (per-agent cloned instances, created lazily)
 *   └── Update(World, dt)
 *         ├── UpdatePerception()  → writes Blackboard entries
 *         ├── UpdateBehavior()    → ticks BehaviorTree, updates AIComponent::state
 *         └── UpdateMovement()   → advances agent along currentPath, writes Transform
 * ```
 *
 * ## Behavior tree registration
 * Templates are registered once at startup via `RegisterBehavior()`. When an agent's
 * `AIComponent::behaviorTreeName` is first encountered, `CreateBehaviorInstance()`
 * clones the template and assigns the opaque handle to `AIComponent::behaviorTreeHandle`.
 *
 * @code
 *   // At level load / system initialization:
 *   aiSystem.RegisterBehavior("PatrolGuard",
 *       Spark::AI::FPSBehaviors::CreateGuardBehavior({0,0,0}, 15.0f));
 *
 *   // On entity spawn:
 *   auto& ai = world.AddComponent<AIComponent>(guardEntity);
 *   ai.behaviorTreeName = "PatrolGuard";
 *   ai.config.detectionRange = 25.0f;
 *
 *   // Per frame:
 *   aiSystem.Update(world, dt);
 * @endcode
 *
 * ## Console integration
 * `Console_ListAgents()` and `Console_GetAgentInfo()` allow the debug console and
 * editor to inspect the live state of all AI agents at runtime.
 *
 * @note `AISystem` inherits from `Spark::ECS::ISystem` and is registered with the
 *       `SystemManager` like any other ECS system. The update call is automatically
 *       driven by `SystemManager::UpdateAll()`.
 */

#pragma once

#include "../ECS/Systems/ECSystems.h"
#include "BehaviorTree.h"
#include "NavMesh.h"
#include <unordered_map>
#include <memory>

namespace Spark::AI
{

    /**
 * @brief Per-entity AI component that drives decision-making and navigation.
 *
 * Entities with an AIComponent participate in the `AISystem` update loop each frame.
 * The component carries:
 * - A reference to the behavior tree template to use (`behaviorTreeName`).
 * - Opaque handles to the runtime behavior tree and NavMesh query (managed by AISystem).
 * - Perception state (target entity, last known position, alert timers).
 * - Pathfinding state (current path waypoints, progress index).
 * - Per-agent configuration (ranges, speed, accuracy).
 *
 * ### Opaque handles
 * `behaviorTreeHandle` and `navQueryHandle` are type-safe opaque handles managed
 * exclusively by the `AISystem`. Do not read or write these from external code.
 *
 * ### State machine
 * The `state` enum communicates the agent's high-level intent to other systems
 * (HUD, audio, game mode). Behavior tree action nodes write to it; external code
 * may read it but should avoid writing to it directly to prevent conflicts with
 * the behavior tree's decisions.
 */
    struct AIComponent
    {
        /**
     * @brief Name of the behavior tree template to use.
     *
     * Must match a name registered via `AISystem::RegisterBehavior()` before
     * the first `Update()` call that encounters this entity. On first use the
     * AISystem clones the template and stores the instance in `behaviorTreeHandle`.
     */
        std::string behaviorTreeName;

        /**
     * @brief Per-agent configuration parameters (ranges, speed, accuracy).
     *
     * See `AIAgentConfig` in BehaviorTree.h for field descriptions.
     * Adjust before spawning the agent to control difficulty.
     */
        AIAgentConfig config;

        /**
     * @brief Type-safe handle to this agent's private BehaviorTree instance.
     *
     * Created by `AISystem::CreateBehaviorInstance()` from the named template.
     * Managed exclusively by AISystem; do not dereference from external code.
     */
        Spark::BehaviorTreeHandle behaviorTreeHandle;

        /**
     * @brief Type-safe handle to this agent's NavMeshQuery for pathfinding.
     *
     * Created by the AISystem from the active NavMesh at agent initialization.
     * Managed exclusively by AISystem; do not dereference from external code.
     */
        Spark::NavQueryHandle navQueryHandle;

        // ---- Perception & state ------------------------------------------------

        /**
     * @brief High-level AI state broadcasted to other game systems.
     *
     * Behavior tree nodes write to this field to indicate what the agent is doing.
     * Other systems (audio, HUD, game mode) may read it to react accordingly.
     * For example, the audio system plays footstep sounds only when state != Dead.
     */
        enum class State
        {
            Idle,
            Patrolling,
            Alert,
            Combat,
            Fleeing,
            Dead
        };

        /** @brief Current high-level AI state. Default: Idle. */
        State state = State::Idle;

        /**
     * @brief EntityID of the current primary threat/target.
     *
     * Set by the perception system when the player or another threat is detected.
     * `entt::null` indicates no active target. Behavior tree conditions read this
     * to decide between attack, patrol, and idle branches.
     */
        EntityID targetEntity = entt::null;

        /**
     * @brief Last confirmed world-space position of the primary target.
     *
     * Updated each frame the target is directly visible. When line-of-sight is
     * lost the agent navigates to this position to "search" before giving up.
     */
        XMFLOAT3 lastKnownTargetPos{0, 0, 0};

        /**
     * @brief Time in seconds since the agent last had line-of-sight to the target.
     *
     * The behavior tree uses this to transition between Alert (searching) and
     * Idle (gave up) states after sufficient time has elapsed.
     */
        float timeSinceLastSeen = 0.0f;

        /**
     * @brief Accumulated time in the Alert state (seconds).
     *
     * Enables transitions like "Alert for > 5 seconds → enter Combat" even if
     * the target is briefly not visible.
     */
        float alertTimer = 0.0f;

        // ---- Pathfinding -------------------------------------------------------

        /**
     * @brief World-space waypoints produced by the most recent NavMesh path query.
     *
     * Updated by the AISystem when `AIComponent::state` changes to a movement
     * state or the target moves significantly. The movement subsystem advances
     * `currentPathIndex` as each waypoint is reached.
     */
        std::vector<XMFLOAT3> currentPath;

        /**
     * @brief Index into `currentPath` pointing to the current waypoint target.
     *
     * Incremented by the movement subsystem each time the agent comes within
     * stopping distance of the waypoint at `currentPath[currentPathIndex]`.
     */
        size_t currentPathIndex = 0;

        /**
     * @brief Immediate movement target (may differ from `currentPath` waypoints).
     *
     * Set by steering behaviors when the agent needs to move directly to a point
     * without full path recalculation (e.g. strafing, short-range approach).
     */
        XMFLOAT3 moveTarget{0, 0, 0};
    };

    // =============================================================================
    // AISystem
    // =============================================================================

    /**
 * @class AISystem
 * @brief ECS system that drives all AI agents each frame.
 *
 * AISystem is a concrete `Spark::ECS::ISystem` that processes every entity
 * carrying an `AIComponent`. It is registered with the game's `SystemManager`
 * and updated automatically each frame via `SystemManager::UpdateAll()`.
 *
 * ### Responsibilities
 * - **Behavior tree lifecycle**: clones templates on demand, ticks instances.
 * - **Perception**: determines which targets each agent can see or hear.
 * - **Pathfinding**: queries NavMesh when agents need new routes.
 * - **Movement**: steers agents towards waypoints and updates Transform.
 *
 * ### Behavior tree templates
 * Templates are shared blueprints that define the logic structure for an NPC type.
 * Register them at startup with `RegisterBehavior()`. Each agent gets its own
 * private clone (instance) created by `CreateBehaviorInstance()` so that agent-
 * specific blackboard state does not leak between agents.
 *
 * @code
 *   AISystem aiSys;
 *   aiSys.RegisterBehavior("Grunt",
 *       Spark::AI::FPSBehaviors::CreateCombatBehavior(gruntConfig));
 *   aiSys.RegisterBehavior("Scout",
 *       Spark::AI::FPSBehaviors::CreatePatrolBehavior(patrolPoints));
 * @endcode
 *
 * ### Integration with physics
 * Movement is applied directly to the entity's `Transform` component. If the
 * entity also has a `RigidBodyComponent` of type `Kinematic`, the PhysicsUpdateSystem
 * will forward the new Transform to the Bullet rigid body, enabling physics-based
 * collision avoidance.
 *
 * ### Performance considerations
 * - For large levels (100+ agents) consider spreading AI updates over multiple
 *   frames by enabling only a subset of agents per frame via `ActiveComponent`.
 * - Perception raycasts can be expensive; throttle them by only querying every
 *   N frames using the agent's `alertTimer` as a cooldown.
 *
 * @note This class does NOT inherit from the `AIUpdateSystem` stub in ECSystems.h.
 *       It is a standalone, fully-featured system used directly by the game.
 */
    class AISystem : public Spark::ECS::ISystem
    {
      public:
        /**
     * @brief Default constructor.
     *
     * Initializes an empty system with no registered behaviors. Call
     * `RegisterBehavior()` before using the system.
     */
        AISystem();

        /**
     * @brief Virtual destructor ensures proper cleanup of behavior instances.
     */
        ~AISystem() override = default;

        /**
     * @brief Process all AI agents for one simulation frame.
     *
     * For each entity with an `AIComponent`:
     * 1. Lazily initializes the behavior tree instance if not yet created.
     * 2. Calls `UpdatePerception()` to refresh target visibility and alertness.
     * 3. Calls `UpdateBehavior()` to tick the behavior tree and update `AIComponent::state`.
     * 4. Calls `UpdateMovement()` to steer the agent towards its current waypoint.
     *
     * Agents with `AIComponent::state == State::Dead` are skipped entirely.
     *
     * @param world      The ECS World containing AIComponent and Transform entities.
     * @param deltaTime  Frame time in seconds for speed-independent AI updates.
     */
        void Update(World& world, float deltaTime) override;

        const char* GetName() const override { return "AISystem"; }

        /**
     * @brief Register a behavior tree template for agent use.
     *
     * The template is stored by `name` and cloned for each agent that specifies
     * this name in `AIComponent::behaviorTreeName`. Registering the same name
     * again replaces the existing template.
     *
     * @param name  Unique identifier for this behavior (e.g. "Grunt", "Scout", "Boss").
     * @param tree  Unique pointer to the BehaviorTree template. Ownership is transferred.
     *
     * @code
     *   aiSystem.RegisterBehavior("PatrolGuard",
     *       FPSBehaviors::CreateGuardBehavior({10,0,5}, 20.0f));
     * @endcode
     */
        void RegisterBehavior(const std::string& name, std::unique_ptr<BehaviorTree> tree);

        /**
     * @brief Clone a registered template into a new per-agent instance.
     *
     * Called by the AISystem internally when an agent with a previously unseen
     * `behaviorTreeName` is first encountered. The returned instance is owned
     * by `m_behaviorInstances` and its address is stored in
     * `AIComponent::behaviorTreeHandle`.
     *
     * @param templateName  Name of a previously registered behavior template.
     * @return              Non-owning pointer to the new instance, or `nullptr` if
     *                      no template with that name is registered.
     */
        BehaviorTree* CreateBehaviorInstance(const std::string& templateName);

        // =========================================================================
        // Console integration
        // =========================================================================

        /**
     * @brief Return a formatted list of all active AI agents (console integration).
     *
     * Lists each entity with an AIComponent, showing its current state, behavior
     * tree name, and distance to its target.
     *
     * @param world  The ECS World to query for AIComponent entities.
     * @return       Newline-separated string listing all active agents.
     */
        std::string Console_ListAgents(World& world) const;

        /**
     * @brief Return detailed information about a specific AI agent (console integration).
     *
     * Outputs the agent's full state: AIComponent::state, blackboard entries,
     * current path length, target entity, and behavior tree node trace.
     *
     * @param world   The ECS World to query.
     * @param entity  EntityID of the agent to inspect.
     * @return        Multi-line formatted string with agent debug information.
     */
        std::string Console_GetAgentInfo(World& world, EntityID entity) const;

      private:
        /**
     * @brief Refresh an agent's perception data (line-of-sight, hearing).
     *
     * Reads the world to determine which entities the agent can detect. Updates
     * `AIComponent::targetEntity`, `lastKnownTargetPos`, `timeSinceLastSeen`, and
     * `alertTimer`. Also writes corresponding entries to the agent's behavior tree
     * Blackboard so that condition nodes can evaluate them.
     *
     * @param world      The ECS World for querying potential target entities.
     * @param ai         The AIComponent to update.
     * @param transform  The agent's current world transform (determines origin for raycasts).
     * @param deltaTime  Frame time for advancing timers.
     */
        void UpdatePerception(World& world, EntityID selfEntity, AIComponent& ai, const Transform& transform,
                              float deltaTime);

        /**
     * @brief Advance the agent's movement along its current path.
     *
     * Computes the direction to the next waypoint in `AIComponent::currentPath`,
     * scales it by `AIComponent::config.moveSpeed * deltaTime`, and applies the
     * result. When the entity has a `RigidBodyComponent`, the desired velocity is
     * written to `RigidBodyComponent::linearVelocity` so that the physics system
     * resolves the final position with collision. Otherwise, the transform is
     * updated directly.
     *
     * If the path becomes stale (target moved significantly) a new path query is
     * issued via the agent's `navQueryHandle`.
     *
     * @param world      The ECS World (used for physics component lookup).
     * @param entity     The entity ID for component queries.
     * @param ai         The AIComponent with current path and configuration.
     * @param transform  The agent's transform; position is modified directly when
     *                   no RigidBodyComponent is present.
     * @param deltaTime  Frame time for speed-independent movement.
     */
        void UpdateMovement(World& world, EntityID entity, AIComponent& ai, Transform& transform, float deltaTime);

        /**
     * @brief Tick the agent's behavior tree and update high-level state.
     *
     * Calls `BehaviorTree::Tick(deltaTime)` on the instance referenced by
     * `ai.behaviorTreeHandle`. The tree's action nodes may:
     * - Write to `ai.state` to signal intent to other systems.
     * - Write to `ai.moveTarget` to set a movement destination.
     * - Trigger attack/retreat/cover actions via the Blackboard.
     *
     * @param ai         The AIComponent whose behavior tree is ticked.
     * @param deltaTime  Frame time forwarded to the behavior tree.
     */
        void UpdateBehavior(AIComponent& ai, float deltaTime);

        /**
     * @brief Named behavior tree templates shared across all agents of a given type.
     *
     * Keyed by the name registered via `RegisterBehavior()`. Templates are never
     * ticked directly; clones are created via `CreateBehaviorInstance()`.
     */
        std::unordered_map<std::string, std::unique_ptr<BehaviorTree>> m_behaviorTemplates;

        /**
     * @brief Per-agent behavior tree instances cloned from templates.
     *
     * Owns all agent instances. Each agent's `AIComponent::behaviorTreeHandle`
     * points to an element in this vector. Instances are added on demand and
     * never removed (destroyed with the system).
     */
        std::vector<std::unique_ptr<BehaviorTree>> m_behaviorInstances;
    };

} // namespace Spark::AI
