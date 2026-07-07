/**
 * @file BehaviorTreeNodes.h
 * @brief Concrete behavior tree node implementations
 * @author Spark Engine Team
 * @date 2025
 *
 * Contains all concrete node types: composites (Sequence, Selector, Parallel),
 * decorators (Inverter, Repeater), and leaves (Action, Condition, Wait).
 *
 * @see BehaviorTreeTypes.h, BehaviorTree.h
 */

#pragma once

#include "BehaviorTreeTypes.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Spark::AI
{

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
     */
    class SequenceNode : public BTNode
    {
      public:
        explicit SequenceNode(const std::string& name = "Sequence") : m_name(name) {}

        void AddChild(std::unique_ptr<BTNode> child) { m_children.push_back(std::move(child)); }

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

        void Reset() override
        {
            BTNode::Reset();
            m_currentChild = 0;
            for (auto& child : m_children)
                child->Reset();
        }

        const char* GetName() const override { return m_name.c_str(); }

        std::unique_ptr<BTNode> Clone() const override
        {
            auto cloned = std::make_unique<SequenceNode>(m_name);
            for (const auto& child : m_children)
            {
                if (child)
                    cloned->AddChild(child->Clone());
            }
            return cloned;
        }

      private:
        std::string m_name;
        std::vector<std::unique_ptr<BTNode>> m_children;
        size_t m_currentChild = 0;
    };

    /**
     * @class SelectorNode
     * @brief Composite node that runs children left-to-right, stopping on first success.
     *
     * A Selector is analogous to a logical OR (priority selector).
     */
    class SelectorNode : public BTNode
    {
      public:
        explicit SelectorNode(const std::string& name = "Selector") : m_name(name) {}

        void AddChild(std::unique_ptr<BTNode> child) { m_children.push_back(std::move(child)); }

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

        void Reset() override
        {
            BTNode::Reset();
            m_currentChild = 0;
            for (auto& child : m_children)
                child->Reset();
        }

        const char* GetName() const override { return m_name.c_str(); }

        std::unique_ptr<BTNode> Clone() const override
        {
            auto cloned = std::make_unique<SelectorNode>(m_name);
            for (const auto& child : m_children)
            {
                if (child)
                    cloned->AddChild(child->Clone());
            }
            return cloned;
        }

      private:
        std::string m_name;
        std::vector<std::unique_ptr<BTNode>> m_children;
        size_t m_currentChild = 0;
    };

    /**
     * @class ParallelNode
     * @brief Composite node that ticks **all** children simultaneously every frame.
     */
    class ParallelNode : public BTNode
    {
      public:
        enum class Policy
        {
            RequireOne, ///< Succeed when at least one child succeeds.
            RequireAll  ///< Succeed only when every child succeeds.
        };

        explicit ParallelNode(Policy successPolicy = Policy::RequireOne, const std::string& name = "Parallel")
            : m_successPolicy(successPolicy), m_name(name)
        {
        }

        void AddChild(std::unique_ptr<BTNode> child) { m_children.push_back(std::move(child)); }

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

        void Reset() override
        {
            BTNode::Reset();
            for (auto& child : m_children)
                child->Reset();
        }

        const char* GetName() const override { return m_name.c_str(); }

        std::unique_ptr<BTNode> Clone() const override
        {
            auto cloned = std::make_unique<ParallelNode>(m_successPolicy, m_name);
            for (const auto& child : m_children)
            {
                if (child)
                    cloned->AddChild(child->Clone());
            }
            return cloned;
        }

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
     */
    class InverterNode : public BTNode
    {
      public:
        explicit InverterNode(std::unique_ptr<BTNode> child) : m_child(std::move(child)) {}

        NodeStatus Tick(float deltaTime, Blackboard& blackboard) override
        {
            if (!m_child)
                return m_status = NodeStatus::Failure;

            NodeStatus status = m_child->Tick(deltaTime, blackboard);
            if (status == NodeStatus::Success)
                return m_status = NodeStatus::Failure;
            if (status == NodeStatus::Failure)
                return m_status = NodeStatus::Success;
            return m_status = NodeStatus::Running;
        }

        const char* GetName() const override { return "Inverter"; }

        std::unique_ptr<BTNode> Clone() const override
        {
            std::unique_ptr<BTNode> clonedChild = nullptr;
            if (m_child)
                clonedChild = m_child->Clone();
            return std::make_unique<InverterNode>(std::move(clonedChild));
        }

      private:
        std::unique_ptr<BTNode> m_child;
    };

    /**
     * @class RepeaterNode
     * @brief Decorator that re-ticks its child a fixed number of times (or infinitely).
     */
    class RepeaterNode : public BTNode
    {
      public:
        RepeaterNode(std::unique_ptr<BTNode> child, int repeatCount = -1)
            : m_child(std::move(child)), m_repeatCount(repeatCount)
        {
        }

        NodeStatus Tick(float deltaTime, Blackboard& blackboard) override
        {
            if (!m_child)
                return m_status = NodeStatus::Failure;

            NodeStatus status = m_child->Tick(deltaTime, blackboard);
            if (status == NodeStatus::Running)
                return m_status = NodeStatus::Running;

            m_currentCount++;
            if (m_repeatCount > 0 && m_currentCount >= m_repeatCount)
                return m_status = NodeStatus::Success;

            m_child->Reset();
            return m_status = NodeStatus::Running;
        }

        void Reset() override
        {
            BTNode::Reset();
            m_currentCount = 0;
            if (m_child)
                m_child->Reset();
        }

        const char* GetName() const override { return "Repeater"; }

        std::unique_ptr<BTNode> Clone() const override
        {
            std::unique_ptr<BTNode> clonedChild = nullptr;
            if (m_child)
                clonedChild = m_child->Clone();
            return std::make_unique<RepeaterNode>(std::move(clonedChild), m_repeatCount);
        }

      private:
        std::unique_ptr<BTNode> m_child;
        int m_repeatCount;
        int m_currentCount = 0;
    };

    // =============================================================================
    // Leaf Nodes
    // =============================================================================

    /**
     * @class ActionNode
     * @brief Leaf node that executes a custom user-supplied function each tick.
     */
    class ActionNode : public BTNode
    {
      public:
        using ActionFunc = std::function<NodeStatus(float, Blackboard&)>;

        ActionNode(const std::string& name, ActionFunc action) : m_name(name), m_action(std::move(action)) {}

        NodeStatus Tick(float deltaTime, Blackboard& blackboard) override
        {
            if (!m_action)
                return m_status = NodeStatus::Failure;
            return m_status = m_action(deltaTime, blackboard);
        }

        const char* GetName() const override { return m_name.c_str(); }

        std::unique_ptr<BTNode> Clone() const override { return std::make_unique<ActionNode>(m_name, m_action); }

      private:
        std::string m_name;
        ActionFunc m_action;
    };

    /**
     * @class ConditionNode
     * @brief Leaf node that tests a boolean predicate against the Blackboard.
     */
    class ConditionNode : public BTNode
    {
      public:
        using ConditionFunc = std::function<bool(const Blackboard&)>;

        ConditionNode(const std::string& name, ConditionFunc condition)
            : m_name(name), m_condition(std::move(condition))
        {
        }

        NodeStatus Tick(float deltaTime, Blackboard& blackboard) override
        {
            if (!m_condition)
                return m_status = NodeStatus::Failure;
            return m_status = m_condition(blackboard) ? NodeStatus::Success : NodeStatus::Failure;
        }

        const char* GetName() const override { return m_name.c_str(); }

        std::unique_ptr<BTNode> Clone() const override { return std::make_unique<ConditionNode>(m_name, m_condition); }

      private:
        std::string m_name;
        ConditionFunc m_condition;
    };

    /**
     * @class WaitNode
     * @brief Leaf node that pauses execution for a specified duration.
     */
    class WaitNode : public BTNode
    {
      public:
        explicit WaitNode(float duration) : m_duration(duration) {}

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

        void Reset() override
        {
            BTNode::Reset();
            m_elapsed = 0.0f;
        }
        const char* GetName() const override { return "Wait"; }

        std::unique_ptr<BTNode> Clone() const override { return std::make_unique<WaitNode>(m_duration); }

      private:
        float m_duration;
        float m_elapsed = 0.0f;
    };

} // namespace Spark::AI
