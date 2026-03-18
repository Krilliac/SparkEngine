/**
 * @file BehaviorTreeTypes.h
 * @brief Core types for the behavior tree framework: NodeStatus, Blackboard, and BTNode base class
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * This header defines the foundational types used by the behavior tree system:
 * - **NodeStatus** — the three-valued result returned by every node after a tick.
 * - **Blackboard** — a typed key-value store for inter-node communication.
 * - **BTNode** — the abstract base class for all behavior tree nodes.
 *
 * Concrete node implementations (composites, decorators, leaf nodes) and the
 * BehaviorTree container class are defined in BehaviorTree.h.
 *
 * @see BehaviorTree.h
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
        bool Has(const std::string& key) const { return m_data.contains(key); }

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

} // namespace Spark::AI
