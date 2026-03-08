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
#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <any>
#include <variant>


namespace Spark::AI
{

    // =============================================================================
    // Node Status
    // =============================================================================

    /**
 * @brief Result returned by a behavior tree node after each tick.
 *
 * The three-valued status drives the composite node policies:
 * - **Running** is propagated up the tree, pausing evaluation until the next tick.
 * - **Success** / **Failure** trigger policy-specific logic in composite parents.
 */
    enum class NodeStatus
    {
        Running, ///< The node is executing asynchronously (will be ticked again next frame).
        Success, ///< The node completed its goal successfully.
        Failure  ///< The node failed (condition not met, action could not execute, etc.).
    };

    // =============================================================================
    // Blackboard
    // =============================================================================

    /**
 * @class Blackboard
 * @brief Shared typed key-value store for inter-node communication.
 *
 * The Blackboard is the "working memory" of an AI agent. Perception systems write
 * observations into it (e.g. "enemyVisible = true", "lastSeenPos = {x, y, z}") and
 * behavior tree nodes read from it to make decisions.
 *
 * ### Supported value types
 * Values are stored as `std::variant<bool, int, float, std::string, XMFLOAT3>`.
 * Use the appropriate C++ type when calling `Get<T>()` — a type mismatch returns
 * `defaultValue` rather than throwing.
 *
 * ### Ownership
 * The Blackboard is owned by the BehaviorTree and shared by all nodes via reference.
 * External systems that need to write perception data should obtain a reference to
 * the tree's blackboard via `BehaviorTree::GetBlackboard()`.
 *
 * @code
 *   Blackboard& bb = tree.GetBlackboard();
 *   bb.Set("enemyVisible", true);
 *   bb.Set("enemyPosition", XMFLOAT3{10, 0, -5});
 *
 *   bool visible = bb.Get<bool>("enemyVisible", false);
 *   XMFLOAT3 pos = bb.Get<XMFLOAT3>("enemyPosition");
 * @endcode
 */
    class Blackboard
    {
      public:
        /**
     * @brief Variant type holding all supported blackboard value types.
     *
     * Supported types: `bool`, `int`, `float`, `std::string`, `DirectX::XMFLOAT3`.
     * Use `std::get<T>()` or the `Get<T>()` helper to retrieve typed values.
     */
        using Value = std::variant<bool, int, float, std::string, XMFLOAT3>;

        /**
     * @brief Write (or overwrite) a key-value pair.
     *
     * Overwrites any existing value for `key` regardless of type. There is no
     * type-safety check on overwrite; callers are responsible for consistent usage.
     *
     * @param key    Unique string key (case-sensitive).
     * @param value  Value to store; must be one of the supported variant types.
     */
        void Set(const std::string& key, const Value& value) { m_data[key] = value; }

        /**
     * @brief Read a typed value, returning a default if the key is absent or has the wrong type.
     *
     * Uses `std::get<T>()` internally; a `std::bad_variant_access` exception is caught
     * and silently returns `defaultValue` so callers do not need to guard against type mismatches.
     *
     * @tparam T           The expected C++ type of the stored value.
     * @param  key          Key to look up.
     * @param  defaultValue Value returned when the key is missing or the type doesn't match.
     * @return              The stored value of type T, or `defaultValue`.
     */
        template <typename T> T Get(const std::string& key, const T& defaultValue = T{}) const
        {
            auto it = m_data.find(key);
            if (it != m_data.end())
            {
                try
                {
                    return std::get<T>(it->second);
                }
                catch (...)
                {
                    return defaultValue;
                }
            }
            return defaultValue;
        }

        /**
     * @brief Check whether a key exists in the blackboard.
     * @param key  Key to test (case-sensitive).
     * @return     `true` if the key is present (regardless of type).
     */
        bool Has(const std::string& key) const { return m_data.find(key) != m_data.end(); }

        /**
     * @brief Remove a key and its associated value.
     *
     * A no-op if the key does not exist.
     *
     * @param key  Key to remove.
     */
        void Remove(const std::string& key) { m_data.erase(key); }

        /**
     * @brief Remove all key-value pairs from the blackboard.
     *
     * Typically called when an agent is respawned or its behavior tree is reset to
     * ensure stale perception data is cleared.
     */
        void Clear() { m_data.clear(); }

      private:
        /**
     * @brief Underlying storage for all blackboard entries.
     *
     * Implemented as a hash map for O(1) average-case lookups by key.
     */
        std::unordered_map<std::string, Value> m_data;
    };

    // =============================================================================
    // Base Node
    // =============================================================================

    /**
 * @class BTNode
 * @brief Abstract base class for all behavior tree nodes.
 *
 * Every node in the tree — whether composite, decorator, or leaf — inherits from
 * BTNode and overrides `Tick()` to implement its specific logic.
 *
 * ### Implementing a custom node
 * @code
 *   class MyActionNode : public BTNode {
 *   public:
 *       NodeStatus Tick(float deltaTime, Blackboard& blackboard) override {
 *           // ... do work ...
 *           return m_status = NodeStatus::Success;
 *       }
 *       const char* GetName() const override { return "MyAction"; }
 *   };
 * @endcode
 *
 * @note Always write the return value into `m_status` before returning so that
 *       `GetStatus()` reflects the most recent result even between ticks.
 */
    class BTNode
    {
      public:
        virtual ~BTNode() = default;

        /**
     * @brief Evaluate this node for one simulation tick.
     *
     * The implementation should perform the node's work and return the current status.
     * If the work is ongoing it must return `NodeStatus::Running` so the parent knows
     * to tick it again next frame.
     *
     * @param deltaTime  Time elapsed since the last tick (seconds). Use for timers.
     * @param blackboard Reference to the tree's shared blackboard for reading/writing AI state.
     * @return           The node's current status after this tick.
     */
        virtual NodeStatus Tick(float deltaTime, Blackboard& blackboard) = 0;

        /**
     * @brief Reset the node to its initial state.
     *
     * Called by parent composite nodes when the tree is restarted or when a branch
     * needs to be re-evaluated from scratch. Override in subclasses to reset any
     * accumulated state (e.g. timers, child indices).
     */
        virtual void Reset() { m_status = NodeStatus::Failure; }

        /**
     * @brief Return a debug-friendly name for this node.
     *
     * Used by the SparkEditor AI debugger to display the tree structure and
     * highlight the currently executing nodes.
     *
     * @return C-string name of the node.
     */
        virtual const char* GetName() const { return "BTNode"; }

        /**
     * @brief Retrieve the status produced during the most recent Tick() call.
     *
     * Useful for parent nodes and external debuggers that need to inspect node
     * status without re-ticking.
     *
     * @return The last recorded NodeStatus.
     */
        NodeStatus GetStatus() const { return m_status; }

      protected:
        /** @brief Cached status from the most recent Tick() call. Initialized to Failure. */
        NodeStatus m_status = NodeStatus::Failure;
    };

    // =============================================================================
    // Composite Nodes
    // =============================================================================

    /**
 * @class SequenceNode
 * @brief Composite node that runs children left-to-right, stopping on first failure.
 *
 * A Sequence node is analogous to a logical AND:
 * - Ticks children in order.
 * - Returns **Failure** immediately if any child fails.
 * - Returns **Running** if a child is still running (resumes from that child next tick).
 * - Returns **Success** only when **all** children succeed.
 *
 * ### Typical use cases
 * - "Move to cover AND crouch AND wait 2 seconds" — all steps must complete.
 * - Precondition + action pairs: "CanSeeEnemy → Attack".
 *
 * @code
 *   auto seq = std::make_unique<SequenceNode>("PatrolStep");
 *   seq->AddChild(std::make_unique<ConditionNode>("HasWaypoint", hasWaypointFn));
 *   seq->AddChild(std::make_unique<ActionNode>("MoveToWaypoint", moveFn));
 *   seq->AddChild(std::make_unique<WaitNode>(1.0f));  // pause at waypoint
 * @endcode
 */
    class SequenceNode : public BTNode
    {
      public:
        /**
     * @brief Construct a SequenceNode with an optional display name.
     * @param name  Human-readable name shown in the editor debugger. Default: "Sequence".
     */
        explicit SequenceNode(const std::string& name = "Sequence") : m_name(name) {}

        /**
     * @brief Append a child node to the end of the sequence.
     *
     * Children are ticked in the order they are added. Transfer ownership via
     * `std::move()` when calling.
     *
     * @param child  Unique pointer to the child node to add.
     */
        void AddChild(std::unique_ptr<BTNode> child) { m_children.push_back(std::move(child)); }

        /**
     * @brief Tick children in order; fail fast on first failure.
     *
     * Maintains `m_currentChild` so that a Running child is resumed on the next tick
     * rather than restarting from child 0.
     *
     * @param deltaTime  Frame delta time (seconds).
     * @param blackboard AI working memory.
     * @return           Running (waiting on a child), Failure (child failed), or
     *                   Success (all children succeeded).
     */
        NodeStatus Tick(float deltaTime, Blackboard& blackboard) override
        {
            for (size_t i = m_currentChild; i < m_children.size(); ++i)
            {
                NodeStatus status = m_children[i]->Tick(deltaTime, blackboard);
                if (status == NodeStatus::Running)
                {
                    m_currentChild = i;
                    return m_status = NodeStatus::Running;
                }
                if (status == NodeStatus::Failure)
                {
                    m_currentChild = 0;
                    return m_status = NodeStatus::Failure;
                }
            }
            m_currentChild = 0;
            return m_status = NodeStatus::Success;
        }

        /**
     * @brief Reset the sequence and all its children to initial state.
     *
     * Resets `m_currentChild` to 0 so the sequence restarts from the beginning.
     */
        void Reset() override
        {
            BTNode::Reset();
            m_currentChild = 0;
            for (auto& child : m_children)
                child->Reset();
        }

        const char* GetName() const override { return m_name.c_str(); }

      private:
        std::string m_name;
        std::vector<std::unique_ptr<BTNode>> m_children;
        /** @brief Index of the currently executing child; preserved across ticks for Running nodes. */
        size_t m_currentChild = 0;
    };

    /**
 * @class SelectorNode
 * @brief Composite node that runs children left-to-right, stopping on first success.
 *
 * A Selector is analogous to a logical OR (priority selector):
 * - Ticks children in order (higher priority first).
 * - Returns **Success** immediately if any child succeeds.
 * - Returns **Running** if a child is still running (resumes from that child next tick).
 * - Returns **Failure** only when **all** children fail.
 *
 * ### Typical use cases
 * - Fallback chains: "TryAttack → TakeCover → Flee → Idle" (first available action wins).
 * - Priority overrides: highest-priority branch wins if its conditions are met.
 *
 * @code
 *   auto selector = std::make_unique<SelectorNode>("CombatSelector");
 *   selector->AddChild(std::make_unique<SequenceNode>("AttackIfInRange"));
 *   selector->AddChild(std::make_unique<SequenceNode>("ChaseTarget"));
 *   selector->AddChild(std::make_unique<ActionNode>("Idle", idleFn));
 * @endcode
 */
    class SelectorNode : public BTNode
    {
      public:
        /**
     * @brief Construct a SelectorNode with an optional display name.
     * @param name  Human-readable name. Default: "Selector".
     */
        explicit SelectorNode(const std::string& name = "Selector") : m_name(name) {}

        /**
     * @brief Append a child at the lowest priority position.
     * @param child  Unique pointer to the child node.
     */
        void AddChild(std::unique_ptr<BTNode> child) { m_children.push_back(std::move(child)); }

        /**
     * @brief Tick children in order; succeed fast on first success.
     *
     * @param deltaTime  Frame delta time (seconds).
     * @param blackboard AI working memory.
     * @return           Running, Success (first child succeeded), or Failure (all children failed).
     */
        NodeStatus Tick(float deltaTime, Blackboard& blackboard) override
        {
            for (size_t i = m_currentChild; i < m_children.size(); ++i)
            {
                NodeStatus status = m_children[i]->Tick(deltaTime, blackboard);
                if (status == NodeStatus::Running)
                {
                    m_currentChild = i;
                    return m_status = NodeStatus::Running;
                }
                if (status == NodeStatus::Success)
                {
                    m_currentChild = 0;
                    return m_status = NodeStatus::Success;
                }
            }
            m_currentChild = 0;
            return m_status = NodeStatus::Failure;
        }

        /**
     * @brief Reset the selector and all its children.
     */
        void Reset() override
        {
            BTNode::Reset();
            m_currentChild = 0;
            for (auto& child : m_children)
                child->Reset();
        }

        const char* GetName() const override { return m_name.c_str(); }

      private:
        std::string m_name;
        std::vector<std::unique_ptr<BTNode>> m_children;
        /** @brief Index of the currently executing child. */
        size_t m_currentChild = 0;
    };

    /**
 * @class ParallelNode
 * @brief Composite node that ticks **all** children simultaneously every frame.
 *
 * Unlike Sequence and Selector, a Parallel node ticks every child regardless of
 * their individual success or failure. The overall result is determined by a
 * configurable success policy.
 *
 * ### Success policies
 * - **RequireOne** – Succeeds as soon as any child succeeds; fails if all fail.
 * - **RequireAll** – Succeeds only when all children succeed; fails if any fail.
 *
 * ### Typical use cases
 * - Running multiple concurrent goals: "Patrol AND ScanForEnemies simultaneously".
 * - Interrupt-by-condition: "Parallel(MoveTo, Guard IsHealthLow → Flee)".
 *
 * @note Running a Parallel node is more expensive than Sequence/Selector because
 *       every child is ticked regardless of status. Prefer the latter when possible.
 */
    class ParallelNode : public BTNode
    {
      public:
        /**
     * @brief Success/failure evaluation policy for the Parallel node.
     */
        enum class Policy
        {
            RequireOne, ///< Succeed when at least one child succeeds.
            RequireAll  ///< Succeed only when every child succeeds.
        };

        /**
     * @brief Construct a ParallelNode with the given success policy and name.
     * @param successPolicy  Determines when the node reports Success.
     * @param name           Human-readable name. Default: "Parallel".
     */
        explicit ParallelNode(Policy successPolicy = Policy::RequireOne, const std::string& name = "Parallel")
            : m_successPolicy(successPolicy), m_name(name)
        {
        }

        /**
     * @brief Append a child to the parallel group.
     * @param child  Unique pointer to the child node.
     */
        void AddChild(std::unique_ptr<BTNode> child) { m_children.push_back(std::move(child)); }

        /**
     * @brief Tick all children; evaluate overall status via the configured policy.
     *
     * All children are ticked unconditionally. Success/failure counts are tallied
     * and the overall result is derived from `m_successPolicy`.
     *
     * @param deltaTime  Frame delta time (seconds).
     * @param blackboard AI working memory.
     * @return           Running, Success, or Failure as determined by the policy.
     */
        NodeStatus Tick(float deltaTime, Blackboard& blackboard) override
        {
            int successCount = 0, failureCount = 0;

            for (auto& child : m_children)
            {
                NodeStatus status = child->Tick(deltaTime, blackboard);
                if (status == NodeStatus::Success)
                    successCount++;
                else if (status == NodeStatus::Failure)
                    failureCount++;
            }

            if (m_successPolicy == Policy::RequireAll)
            {
                if (successCount == static_cast<int>(m_children.size()))
                    return m_status = NodeStatus::Success;
                if (failureCount > 0)
                    return m_status = NodeStatus::Failure;
            }
            else
            {
                if (successCount > 0)
                    return m_status = NodeStatus::Success;
                if (failureCount == static_cast<int>(m_children.size()))
                    return m_status = NodeStatus::Failure;
            }
            return m_status = NodeStatus::Running;
        }

        /**
     * @brief Reset all children in the parallel group.
     */
        void Reset() override
        {
            BTNode::Reset();
            for (auto& child : m_children)
                child->Reset();
        }

        const char* GetName() const override { return m_name.c_str(); }

      private:
        Policy m_successPolicy;
        std::string m_name;
        std::vector<std::unique_ptr<BTNode>> m_children;
    };

    // =============================================================================
    // Decorator Nodes
    // =============================================================================

    /**
 * @class InverterNode
 * @brief Decorator that negates its child's Success/Failure result.
 *
 * Passes `Running` through unchanged. Converts `Success` to `Failure` and
 * `Failure` to `Success`. Useful for building "NOT" conditions:
 * e.g. "Succeed if player is NOT in sight".
 *
 * @code
 *   // Succeed when the player is NOT visible
 *   auto notVisible = std::make_unique<InverterNode>(
 *       std::make_unique<ConditionNode>("IsPlayerVisible", isVisibleFn));
 * @endcode
 */
    class InverterNode : public BTNode
    {
      public:
        /**
     * @brief Construct the inverter wrapping the given child node.
     * @param child  The single child whose result will be negated.
     */
        explicit InverterNode(std::unique_ptr<BTNode> child) : m_child(std::move(child)) {}

        /**
     * @brief Tick the child and invert its Success/Failure result.
     * @param deltaTime  Frame delta time (seconds).
     * @param blackboard AI working memory.
     * @return           Inverted result (Running unchanged, Success↔Failure).
     */
        NodeStatus Tick(float deltaTime, Blackboard& blackboard) override
        {
            NodeStatus status = m_child->Tick(deltaTime, blackboard);
            if (status == NodeStatus::Success)
                return m_status = NodeStatus::Failure;
            if (status == NodeStatus::Failure)
                return m_status = NodeStatus::Success;
            return m_status = NodeStatus::Running;
        }

        const char* GetName() const override { return "Inverter"; }

      private:
        /** @brief The single child node whose result is inverted. */
        std::unique_ptr<BTNode> m_child;
    };

    /**
 * @class RepeaterNode
 * @brief Decorator that re-ticks its child a fixed number of times (or infinitely).
 *
 * After each non-Running result from the child, the child is reset and re-ticked
 * on the next frame. When `repeatCount > 0`, the Repeater returns Success after
 * that many completions. When `repeatCount <= 0` (default: -1) the child repeats
 * indefinitely, which is useful for looping patrol behaviors.
 *
 * @note An infinite repeater never returns Success or Failure; it always returns
 *       Running. Wrap it in a Selector or use an InterruptCondition to escape.
 *
 * @code
 *   // Repeat a patrol loop forever
 *   auto loopPatrol = std::make_unique<RepeaterNode>(
 *       std::move(patrolSequence), -1);
 * @endcode
 */
    class RepeaterNode : public BTNode
    {
      public:
        /**
     * @brief Construct a RepeaterNode.
     * @param child        The child to repeat.
     * @param repeatCount  Number of repetitions. Negative value = infinite. Default: -1.
     */
        RepeaterNode(std::unique_ptr<BTNode> child, int repeatCount = -1)
            : m_child(std::move(child)), m_repeatCount(repeatCount)
        {
        }

        /**
     * @brief Tick the child; reset and loop when it completes.
     *
     * @param deltaTime  Frame delta time (seconds).
     * @param blackboard AI working memory.
     * @return           Running while repeating, Success after `repeatCount` completions.
     */
        NodeStatus Tick(float deltaTime, Blackboard& blackboard) override
        {
            NodeStatus status = m_child->Tick(deltaTime, blackboard);
            if (status == NodeStatus::Running)
                return m_status = NodeStatus::Running;

            m_currentCount++;
            if (m_repeatCount > 0 && m_currentCount >= m_repeatCount)
                return m_status = NodeStatus::Success;

            m_child->Reset();
            return m_status = NodeStatus::Running;
        }

        /**
     * @brief Reset the repeater and its child, clearing the repeat counter.
     */
        void Reset() override
        {
            BTNode::Reset();
            m_currentCount = 0;
            m_child->Reset();
        }

        const char* GetName() const override { return "Repeater"; }

      private:
        std::unique_ptr<BTNode> m_child;
        /** @brief Target number of repetitions (-1 = infinite). */
        int m_repeatCount;
        /** @brief Number of completions so far in the current execution. */
        int m_currentCount = 0;
    };

    // =============================================================================
    // Leaf Nodes
    // =============================================================================

    /**
 * @class ActionNode
 * @brief Leaf node that executes a custom user-supplied function each tick.
 *
 * ActionNode is the primary way to integrate game logic into a behavior tree.
 * The action function receives `deltaTime` and a reference to the Blackboard, and
 * returns a NodeStatus to signal whether the action is still ongoing, succeeded, or failed.
 *
 * ### Writing action functions
 * @code
 *   auto moveToTarget = std::make_unique<ActionNode>("MoveToTarget",
 *       [this](float dt, Blackboard& bb) -> NodeStatus
 *       {
 *           XMFLOAT3 target = bb.Get<XMFLOAT3>("targetPos");
 *           float dist = MathUtils::Distance(m_agent->GetPosition(), target);
 *           if (dist < 0.5f) return NodeStatus::Success;
 *           m_agent->MoveTowards(target, dt);
 *           return NodeStatus::Running;
 *       });
 * @endcode
 *
 * @note Prefer short, single-responsibility action functions. Complex multi-step
 *       processes should be modeled as a SequenceNode of simpler ActionNodes.
 */
    class ActionNode : public BTNode
    {
      public:
        /**
     * @brief Function signature for the action callback.
     *
     * @param deltaTime  Frame time in seconds.
     * @param blackboard Reference to the tree's shared blackboard.
     * @return           NodeStatus indicating ongoing, success, or failure.
     */
        using ActionFunc = std::function<NodeStatus(float, Blackboard&)>;

        /**
     * @brief Construct an ActionNode with a name and callback.
     * @param name    Human-readable identifier shown in the editor debugger.
     * @param action  The function executed each tick.
     */
        ActionNode(const std::string& name, ActionFunc action) : m_name(name), m_action(std::move(action)) {}

        /**
     * @brief Invoke the action function and return its result.
     * @param deltaTime  Frame delta time (seconds).
     * @param blackboard AI working memory.
     * @return           The status returned by the action function.
     */
        NodeStatus Tick(float deltaTime, Blackboard& blackboard) override
        {
            return m_status = m_action(deltaTime, blackboard);
        }

        const char* GetName() const override { return m_name.c_str(); }

      private:
        std::string m_name;
        ActionFunc m_action;
    };

    /**
 * @class ConditionNode
 * @brief Leaf node that tests a boolean predicate against the Blackboard.
 *
 * Returns `NodeStatus::Success` if the condition function returns `true`, and
 * `NodeStatus::Failure` otherwise. Never returns Running since the check is
 * instantaneous.
 *
 * Conditions are typically used as the first child of a SequenceNode (a guard),
 * or as a child of an InverterNode to build negated guards.
 *
 * @code
 *   // "Is the enemy closer than 10 metres?"
 *   auto isInRange = std::make_unique<ConditionNode>("IsInAttackRange",
 *       [this](const Blackboard& bb) {
 *           float dist = bb.Get<float>("distToTarget", FLT_MAX);
 *           return dist < 10.0f;
 *       });
 * @endcode
 */
    class ConditionNode : public BTNode
    {
      public:
        /**
     * @brief Function signature for the condition predicate.
     *
     * @param blackboard Const reference to the tree's blackboard.
     * @return           `true` → Success; `false` → Failure.
     */
        using ConditionFunc = std::function<bool(const Blackboard&)>;

        /**
     * @brief Construct a ConditionNode with a name and predicate.
     * @param name       Human-readable identifier.
     * @param condition  Predicate function evaluated each tick.
     */
        ConditionNode(const std::string& name, ConditionFunc condition)
            : m_name(name), m_condition(std::move(condition))
        {
        }

        /**
     * @brief Evaluate the condition and return Success or Failure.
     *
     * @param deltaTime  Unused for conditions (included for interface consistency).
     * @param blackboard AI working memory read by the condition function.
     * @return           Success if condition returns true, Failure otherwise.
     */
        NodeStatus Tick(float deltaTime, Blackboard& blackboard) override
        {
            return m_status = m_condition(blackboard) ? NodeStatus::Success : NodeStatus::Failure;
        }

        const char* GetName() const override { return m_name.c_str(); }

      private:
        std::string m_name;
        ConditionFunc m_condition;
    };

    /**
 * @class WaitNode
 * @brief Leaf node that pauses execution for a specified duration.
 *
 * Returns `NodeStatus::Running` until the accumulated elapsed time reaches
 * `duration`, then returns `NodeStatus::Success` and resets the timer for
 * subsequent calls.
 *
 * Commonly used to add delays to patrol sequences (pause at waypoints) or
 * to implement attack cooldowns within a behavior branch.
 *
 * @code
 *   // Pause at a waypoint for 3 seconds before continuing the patrol
 *   auto pause = std::make_unique<WaitNode>(3.0f);
 * @endcode
 */
    class WaitNode : public BTNode
    {
      public:
        /**
     * @brief Construct a WaitNode.
     * @param duration  How many seconds to wait before returning Success.
     */
        explicit WaitNode(float duration) : m_duration(duration) {}

        /**
     * @brief Accumulate elapsed time; return Success when the duration is reached.
     * @param deltaTime  Time since last tick (seconds).
     * @param blackboard AI working memory (unused by WaitNode but required by interface).
     * @return           Running while waiting, Success when duration elapsed.
     */
        NodeStatus Tick(float deltaTime, Blackboard& blackboard) override
        {
            m_elapsed += deltaTime;
            if (m_elapsed >= m_duration)
            {
                m_elapsed = 0.0f;
                return m_status = NodeStatus::Success;
            }
            return m_status = NodeStatus::Running;
        }

        /**
     * @brief Reset the elapsed timer to zero.
     */
        void Reset() override
        {
            BTNode::Reset();
            m_elapsed = 0.0f;
        }
        const char* GetName() const override { return "Wait"; }

      private:
        /** @brief Target wait duration in seconds. */
        float m_duration;
        /** @brief Accumulated elapsed time since last reset (seconds). */
        float m_elapsed = 0.0f;
    };

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
