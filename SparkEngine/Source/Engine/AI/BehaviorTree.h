/**
 * @file BehaviorTree.h
 * @brief Behavior tree framework for NPC AI decision-making
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * This file implements a complete, lightweight behavior tree (BT) system for
 * authoring NPC decision logic. Behavior trees are a well-established technique
 * in game AI: they express complex decision-making as a tree of simple, reusable
 * nodes that are evaluated (ticked) once per AI update.
 *
 * ## Core concepts
 *
 * ### Node status
 * Every node returns one of three statuses after being ticked:
 * - **Running** – the node is still doing work (e.g. waiting, moving towards a target).
 * - **Success** – the node completed its task successfully.
 * - **Failure** – the node could not complete its task (condition not met, path blocked, etc.).
 *
 * ### Tree structure
 * Nodes are organized as a tree with a single root. Each frame the root is ticked;
 * composite nodes decide which children to tick based on their policy.
 *
 * | Node type        | Subclasses                              | Description                              |
 * |------------------|-----------------------------------------|------------------------------------------|
 * | Composite        | SequenceNode, SelectorNode, ParallelNode| Multiple children, evaluated by policy   |
 * | Decorator        | InverterNode, RepeaterNode              | Single child, modifies its result/timing |
 * | Leaf (action)    | ActionNode, WaitNode                    | Perform actual work                      |
 * | Leaf (condition) | ConditionNode                           | Test state, return Success or Failure    |
 *
 * ### Blackboard
 * The Blackboard is a typed key-value store shared by all nodes in a tree. It acts
 * as the agent's working memory, holding perception data, navigation state, and
 * inter-node communication variables.
 *
 * ## Usage example
 * @code
 *   // Build a simple guard behavior: idle at post, then patrol on alarm
 *   auto root = std::make_unique<SelectorNode>("Root");
 *
 *   // Branch 1: attack if enemy seen
 *   auto combatSeq = std::make_unique<SequenceNode>("CombatSeq");
 *   combatSeq->AddChild(std::make_unique<ConditionNode>("EnemyVisible",
 *       [](const Blackboard& bb) { return bb.Get<bool>("enemyVisible", false); }));
 *   combatSeq->AddChild(std::make_unique<ActionNode>("Attack",
 *       [](float dt, Blackboard& bb) -> NodeStatus { ... }));
 *   root->AddChild(std::move(combatSeq));
 *
 *   // Branch 2: patrol
 *   root->AddChild(std::make_unique<ActionNode>("Patrol", patrolFn));
 *
 *   auto tree = std::make_unique<BehaviorTree>("GuardBehavior");
 *   tree->SetRoot(std::move(root));
 *
 *   // Each AI update
 *   tree->Tick(deltaTime);
 * @endcode
 *
 * @note All node classes use value-semantic ownership via `std::unique_ptr`; there
 *       are no raw-pointer ownership relationships in this API.
 */

#pragma once

// Subsystem headers
#include "BehaviorTreeTypes.h"
#include "BehaviorTreeNodes.h"

#include <memory>
#include <string>
#include <vector>

namespace Spark::AI
{


    // =============================================================================
    // Behavior Tree
    // =============================================================================

    /**
 * @class BehaviorTree
 * @brief Top-level container for an AI behavior tree.
 *
 * BehaviorTree owns the root node and the Blackboard. Each AI agent has its own
 * BehaviorTree instance (cloned from a shared template by the AISystem) so that
 * agent-specific state stored in the Blackboard does not bleed between agents.
 *
 * ### Lifecycle
 * 1. Build the tree by calling `SetRoot()` with the constructed node hierarchy.
 * 2. Write initial perception data to the Blackboard via `GetBlackboard()`.
 * 3. Call `Tick(deltaTime)` once per AI update frame.
 * 4. Optionally call `Reset()` to restart the tree from scratch.
 *
 * @note BehaviorTree objects are not thread-safe. Call `Tick()` from the main
 *       game thread or ensure external synchronization if called from a job.
 */
    class BehaviorTree
    {
      public:
        /**
     * @brief Construct a BehaviorTree with the given display name.
     * @param name  Name used for editor display and debug logging. Default: "BehaviorTree".
     */
        explicit BehaviorTree(const std::string& name = "BehaviorTree") : m_name(name) {}

        /**
     * @brief Assign the root node of the tree.
     *
     * Replaces any previously set root. The tree takes ownership of the node via
     * `std::unique_ptr`.
     *
     * @param root  Root node of the behavior tree hierarchy.
     */
        void SetRoot(std::unique_ptr<BTNode> root) { m_root = std::move(root); }

        /**
     * @brief Get a non-owning pointer to the root node.
     * @return  Pointer to the root node, or `nullptr` if no root has been set.
     */
        BTNode* GetRoot() const { return m_root.get(); }

        /**
     * @brief Evaluate the tree for one AI update tick.
     *
     * Recursively ticks the root node with the tree's own Blackboard. If no root
     * has been set this returns `NodeStatus::Failure` immediately.
     *
     * @param deltaTime  Time elapsed since the last tick (seconds).
     * @return           The root node's resulting NodeStatus.
     */
        NodeStatus Tick(float deltaTime)
        {
            if (!m_root)
                return NodeStatus::Failure;
            return m_root->Tick(deltaTime, m_blackboard);
        }

        /**
     * @brief Reset the entire tree to its initial state.
     *
     * Recursively resets all nodes. Does NOT clear the Blackboard — call
     * `GetBlackboard().Clear()` separately if a full agent reset is needed.
     */
        void Reset()
        {
            if (m_root)
                m_root->Reset();
        }

        /**
     * @brief Get a mutable reference to the tree's shared Blackboard.
     *
     * External perception systems (e.g. PerceptionSystem) write observation data
     * here each frame before the tree is ticked.
     *
     * @return  Mutable reference to the Blackboard.
     */
        Blackboard& GetBlackboard() { return m_blackboard; }

        /**
     * @brief Get a const reference to the tree's shared Blackboard.
     * @return  Const reference to the Blackboard for read-only access.
     */
        const Blackboard& GetBlackboard() const { return m_blackboard; }

        /**
     * @brief Return the display name of this behavior tree.
     * @return  Name string as provided at construction.
     */
        const std::string& GetName() const { return m_name; }

      private:
        /** @brief Human-readable identifier for editor display. */
        std::string m_name;
        /** @brief Root node of the tree hierarchy; owns the entire node graph. */
        std::unique_ptr<BTNode> m_root;
        /** @brief Per-agent working memory shared by all nodes in this tree instance. */
        Blackboard m_blackboard;
    };

    // =============================================================================
    // AI Agent Configuration
    // =============================================================================

    /**
 * @brief Per-agent configuration parameters for perception and combat behavior.
 *
 * AIAgentConfig is attached to the AIComponent (see Components.h) and read by the
 * AISystem and behavior tree action nodes to determine sensing range, movement
 * speed, and combat capabilities.
 *
 * Adjust these values to control individual agent difficulty:
 * - Increase `detectionRange` for enemies with enhanced sensors.
 * - Reduce `accuracy` for easy/damaged enemies.
 * - Lower `reactionTime` for elite enemies that respond faster.
 */
    struct AIAgentConfig
    {
        /** @brief Radius (metres) at which the agent can detect the player. Default: 30 m. */
        float detectionRange = 30.0f;

        /** @brief Radius (metres) at which the agent opens fire on a target. Default: 15 m. */
        float attackRange = 15.0f;

        /** @brief Radius (metres) at which the agent switches to melee combat. Default: 2 m. */
        float meleeRange = 2.0f;

        /** @brief Pathfinding movement speed (metres/second). Default: 5 m/s. */
        float moveSpeed = 5.0f;

        /**
     * @brief Maximum rotation speed when turning to face a target (degrees/second).
     *
     * Lower values make agents turn sluggishly; higher values make them snap to
     * face the player instantly. Default: 180°/s.
     */
        float turnSpeed = 180.0f;

        /**
     * @brief Shot accuracy in [0, 1]. 1.0 = perfect; 0.0 = completely random.
     *
     * Applied as an angular spread to outgoing projectile directions. Combined
     * with range to produce a realistic accuracy falloff. Default: 0.7.
     */
        float accuracy = 0.7f;

        /**
     * @brief Delay (seconds) between first perceiving a threat and beginning to react.
     *
     * Models human/biological reaction latency. Reduce for elite enemies;
     * increase for slow/surprised enemies. Default: 0.3 s.
     */
        float reactionTime = 0.3f;

        /** @brief Radius (metres) to search for cover positions when threatened. Default: 20 m. */
        float coverSearchRadius = 20.0f;

        /** @brief Allow the agent to strafe (move laterally while facing the target). */
        bool canStrafe = true;

        /** @brief Allow the agent to sprint towards/away from the target. */
        bool canSprint = true;

        /** @brief Allow the agent to seek and use cover objects in the environment. */
        bool canUseCover = true;
    };

    // =============================================================================
    // Pre-built FPS AI Behaviors
    // =============================================================================

    /**
 * @namespace FPSBehaviors
 * @brief Factory functions that construct ready-to-use FPS combat behavior trees.
 *
 * These factories cover the most common NPC archetypes in a first-person shooter.
 * Use them as starting points and customize by modifying the returned tree's
 * nodes or by building custom trees from scratch using the node classes above.
 *
 * All returned trees are pre-wired with standard Blackboard keys:
 * - `"enemyVisible"` (bool)  – set by the PerceptionSystem.
 * - `"targetPos"`   (XMFLOAT3) – last known position of the target.
 * - `"distToTarget"` (float)  – distance to the current target.
 */
    namespace FPSBehaviors
    {

        /**
 * @brief Create a patrol behavior tree that walks between waypoints.
 *
 * The agent moves through `patrolPoints` in sequence, waiting briefly at each point
 * before continuing. If the patrol list is empty the agent idles in place.
 *
 * @param patrolPoints  Ordered list of world-space waypoints to patrol between.
 * @return              Unique pointer to the constructed BehaviorTree.
 */
        std::unique_ptr<BehaviorTree> CreatePatrolBehavior(const std::vector<XMFLOAT3>& patrolPoints);

        /**
 * @brief Create a full combat behavior tree with seek, cover, fire, and retreat logic.
 *
 * The returned tree handles: target acquisition, advancing to attack range, seeking
 * cover when low health, firing, and retreating when overwhelmed. Behavior is tuned
 * by the supplied `config`.
 *
 * @param config  Per-agent configuration controlling ranges, speed, and accuracy.
 * @return        Unique pointer to the constructed BehaviorTree.
 */
        std::unique_ptr<BehaviorTree> CreateCombatBehavior(const AIAgentConfig& config);

        /**
 * @brief Create a guard/sentry behavior tree that alerts on intrusion.
 *
 * The agent stands at `guardPosition` and periodically scans within `guardRadius`.
 * On detecting the player it transitions to an alert/combat state.
 *
 * @param guardPosition  World-space position the guard returns to after patrol.
 * @param guardRadius    Radius (metres) within which the guard watches for threats.
 * @return               Unique pointer to the constructed BehaviorTree.
 */
        std::unique_ptr<BehaviorTree> CreateGuardBehavior(const XMFLOAT3& guardPosition, float guardRadius);

        /**
 * @brief Create a flee behavior tree for injured or non-combatant agents.
 *
 * The agent runs directly away from the last known threat position until it has
 * moved at least `fleeDistance` metres away, then idles.
 *
 * @param fleeDistance  Minimum distance (metres) to run before stopping. Default: 30 m.
 * @return              Unique pointer to the constructed BehaviorTree.
 */
        std::unique_ptr<BehaviorTree> CreateFleeBehavior(float fleeDistance = 30.0f);

    } // namespace FPSBehaviors

} // namespace Spark::AI
