/**
 * @file BehaviorTree.h
 * @brief Behavior tree framework for NPC AI decision-making
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a flexible behavior tree system with:
 * - Composite nodes: Sequence, Selector, Parallel
 * - Decorator nodes: Inverter, Repeater, Succeeder, UntilFail
 * - Leaf nodes: Action, Condition
 * - Blackboard for shared AI state
 * - Pre-built FPS combat behaviors
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

using namespace DirectX;

namespace Spark::AI {

// ============================================================================
// Node Status
// ============================================================================

enum class NodeStatus {
    Running,    ///< Node is still executing
    Success,    ///< Node completed successfully
    Failure     ///< Node failed
};

// ============================================================================
// Blackboard — shared AI state
// ============================================================================

class Blackboard {
public:
    using Value = std::variant<bool, int, float, std::string, XMFLOAT3>;

    void Set(const std::string& key, const Value& value) { m_data[key] = value; }

    template<typename T>
    T Get(const std::string& key, const T& defaultValue = T{}) const {
        auto it = m_data.find(key);
        if (it != m_data.end()) {
            try { return std::get<T>(it->second); }
            catch (...) { return defaultValue; }
        }
        return defaultValue;
    }

    bool Has(const std::string& key) const { return m_data.find(key) != m_data.end(); }
    void Remove(const std::string& key) { m_data.erase(key); }
    void Clear() { m_data.clear(); }

private:
    std::unordered_map<std::string, Value> m_data;
};

// ============================================================================
// Base Node
// ============================================================================

class BTNode {
public:
    virtual ~BTNode() = default;
    virtual NodeStatus Tick(float deltaTime, Blackboard& blackboard) = 0;
    virtual void Reset() { m_status = NodeStatus::Failure; }
    virtual const char* GetName() const { return "BTNode"; }

    NodeStatus GetStatus() const { return m_status; }

protected:
    NodeStatus m_status = NodeStatus::Failure;
};

// ============================================================================
// Composite Nodes
// ============================================================================

/// Runs children in order; fails on first failure
class SequenceNode : public BTNode {
public:
    explicit SequenceNode(const std::string& name = "Sequence") : m_name(name) {}

    void AddChild(std::unique_ptr<BTNode> child) { m_children.push_back(std::move(child)); }

    NodeStatus Tick(float deltaTime, Blackboard& blackboard) override {
        for (size_t i = m_currentChild; i < m_children.size(); ++i) {
            NodeStatus status = m_children[i]->Tick(deltaTime, blackboard);
            if (status == NodeStatus::Running) {
                m_currentChild = i;
                return m_status = NodeStatus::Running;
            }
            if (status == NodeStatus::Failure) {
                m_currentChild = 0;
                return m_status = NodeStatus::Failure;
            }
        }
        m_currentChild = 0;
        return m_status = NodeStatus::Success;
    }

    void Reset() override {
        BTNode::Reset();
        m_currentChild = 0;
        for (auto& child : m_children) child->Reset();
    }

    const char* GetName() const override { return m_name.c_str(); }

private:
    std::string m_name;
    std::vector<std::unique_ptr<BTNode>> m_children;
    size_t m_currentChild = 0;
};

/// Runs children in order; succeeds on first success
class SelectorNode : public BTNode {
public:
    explicit SelectorNode(const std::string& name = "Selector") : m_name(name) {}

    void AddChild(std::unique_ptr<BTNode> child) { m_children.push_back(std::move(child)); }

    NodeStatus Tick(float deltaTime, Blackboard& blackboard) override {
        for (size_t i = m_currentChild; i < m_children.size(); ++i) {
            NodeStatus status = m_children[i]->Tick(deltaTime, blackboard);
            if (status == NodeStatus::Running) {
                m_currentChild = i;
                return m_status = NodeStatus::Running;
            }
            if (status == NodeStatus::Success) {
                m_currentChild = 0;
                return m_status = NodeStatus::Success;
            }
        }
        m_currentChild = 0;
        return m_status = NodeStatus::Failure;
    }

    void Reset() override {
        BTNode::Reset();
        m_currentChild = 0;
        for (auto& child : m_children) child->Reset();
    }

    const char* GetName() const override { return m_name.c_str(); }

private:
    std::string m_name;
    std::vector<std::unique_ptr<BTNode>> m_children;
    size_t m_currentChild = 0;
};

/// Runs all children simultaneously
class ParallelNode : public BTNode {
public:
    enum class Policy { RequireOne, RequireAll };

    explicit ParallelNode(Policy successPolicy = Policy::RequireOne,
                          const std::string& name = "Parallel")
        : m_successPolicy(successPolicy), m_name(name) {}

    void AddChild(std::unique_ptr<BTNode> child) { m_children.push_back(std::move(child)); }

    NodeStatus Tick(float deltaTime, Blackboard& blackboard) override {
        int successCount = 0, failureCount = 0;

        for (auto& child : m_children) {
            NodeStatus status = child->Tick(deltaTime, blackboard);
            if (status == NodeStatus::Success) successCount++;
            else if (status == NodeStatus::Failure) failureCount++;
        }

        if (m_successPolicy == Policy::RequireAll) {
            if (successCount == static_cast<int>(m_children.size()))
                return m_status = NodeStatus::Success;
            if (failureCount > 0) return m_status = NodeStatus::Failure;
        } else {
            if (successCount > 0) return m_status = NodeStatus::Success;
            if (failureCount == static_cast<int>(m_children.size()))
                return m_status = NodeStatus::Failure;
        }
        return m_status = NodeStatus::Running;
    }

    void Reset() override {
        BTNode::Reset();
        for (auto& child : m_children) child->Reset();
    }

    const char* GetName() const override { return m_name.c_str(); }

private:
    Policy m_successPolicy;
    std::string m_name;
    std::vector<std::unique_ptr<BTNode>> m_children;
};

// ============================================================================
// Decorator Nodes
// ============================================================================

/// Inverts child result
class InverterNode : public BTNode {
public:
    explicit InverterNode(std::unique_ptr<BTNode> child) : m_child(std::move(child)) {}

    NodeStatus Tick(float deltaTime, Blackboard& blackboard) override {
        NodeStatus status = m_child->Tick(deltaTime, blackboard);
        if (status == NodeStatus::Success) return m_status = NodeStatus::Failure;
        if (status == NodeStatus::Failure) return m_status = NodeStatus::Success;
        return m_status = NodeStatus::Running;
    }

    const char* GetName() const override { return "Inverter"; }

private:
    std::unique_ptr<BTNode> m_child;
};

/// Repeats child N times
class RepeaterNode : public BTNode {
public:
    RepeaterNode(std::unique_ptr<BTNode> child, int repeatCount = -1)
        : m_child(std::move(child)), m_repeatCount(repeatCount) {}

    NodeStatus Tick(float deltaTime, Blackboard& blackboard) override {
        NodeStatus status = m_child->Tick(deltaTime, blackboard);
        if (status == NodeStatus::Running) return m_status = NodeStatus::Running;

        m_currentCount++;
        if (m_repeatCount > 0 && m_currentCount >= m_repeatCount)
            return m_status = NodeStatus::Success;

        m_child->Reset();
        return m_status = NodeStatus::Running;
    }

    void Reset() override {
        BTNode::Reset();
        m_currentCount = 0;
        m_child->Reset();
    }

    const char* GetName() const override { return "Repeater"; }

private:
    std::unique_ptr<BTNode> m_child;
    int m_repeatCount;
    int m_currentCount = 0;
};

// ============================================================================
// Leaf Nodes
// ============================================================================

/// Executes a custom action function
class ActionNode : public BTNode {
public:
    using ActionFunc = std::function<NodeStatus(float, Blackboard&)>;

    ActionNode(const std::string& name, ActionFunc action)
        : m_name(name), m_action(std::move(action)) {}

    NodeStatus Tick(float deltaTime, Blackboard& blackboard) override {
        return m_status = m_action(deltaTime, blackboard);
    }

    const char* GetName() const override { return m_name.c_str(); }

private:
    std::string m_name;
    ActionFunc m_action;
};

/// Checks a boolean condition
class ConditionNode : public BTNode {
public:
    using ConditionFunc = std::function<bool(const Blackboard&)>;

    ConditionNode(const std::string& name, ConditionFunc condition)
        : m_name(name), m_condition(std::move(condition)) {}

    NodeStatus Tick(float deltaTime, Blackboard& blackboard) override {
        return m_status = m_condition(blackboard) ? NodeStatus::Success : NodeStatus::Failure;
    }

    const char* GetName() const override { return m_name.c_str(); }

private:
    std::string m_name;
    ConditionFunc m_condition;
};

/// Waits for a specified duration
class WaitNode : public BTNode {
public:
    explicit WaitNode(float duration) : m_duration(duration) {}

    NodeStatus Tick(float deltaTime, Blackboard& blackboard) override {
        m_elapsed += deltaTime;
        if (m_elapsed >= m_duration) {
            m_elapsed = 0.0f;
            return m_status = NodeStatus::Success;
        }
        return m_status = NodeStatus::Running;
    }

    void Reset() override { BTNode::Reset(); m_elapsed = 0.0f; }
    const char* GetName() const override { return "Wait"; }

private:
    float m_duration;
    float m_elapsed = 0.0f;
};

// ============================================================================
// Behavior Tree
// ============================================================================

class BehaviorTree {
public:
    explicit BehaviorTree(const std::string& name = "BehaviorTree")
        : m_name(name) {}

    void SetRoot(std::unique_ptr<BTNode> root) { m_root = std::move(root); }
    BTNode* GetRoot() const { return m_root.get(); }

    NodeStatus Tick(float deltaTime) {
        if (!m_root) return NodeStatus::Failure;
        return m_root->Tick(deltaTime, m_blackboard);
    }

    void Reset() {
        if (m_root) m_root->Reset();
    }

    Blackboard& GetBlackboard() { return m_blackboard; }
    const Blackboard& GetBlackboard() const { return m_blackboard; }
    const std::string& GetName() const { return m_name; }

private:
    std::string m_name;
    std::unique_ptr<BTNode> m_root;
    Blackboard m_blackboard;
};

// ============================================================================
// AI Agent Component Data
// ============================================================================

struct AIAgentConfig {
    float detectionRange = 30.0f;
    float attackRange = 15.0f;
    float meleeRange = 2.0f;
    float moveSpeed = 5.0f;
    float turnSpeed = 180.0f;         ///< Degrees per second
    float accuracy = 0.7f;            ///< 0-1 shooting accuracy
    float reactionTime = 0.3f;        ///< Seconds before responding
    float coverSearchRadius = 20.0f;
    bool canStrafe = true;
    bool canSprint = true;
    bool canUseCover = true;
};

// ============================================================================
// Pre-built FPS AI Behaviors
// ============================================================================

namespace FPSBehaviors {

/// Create a patrol behavior tree
std::unique_ptr<BehaviorTree> CreatePatrolBehavior(const std::vector<XMFLOAT3>& patrolPoints);

/// Create a combat behavior tree
std::unique_ptr<BehaviorTree> CreateCombatBehavior(const AIAgentConfig& config);

/// Create a guard/sentry behavior tree
std::unique_ptr<BehaviorTree> CreateGuardBehavior(const XMFLOAT3& guardPosition, float guardRadius);

/// Create a flee behavior tree
std::unique_ptr<BehaviorTree> CreateFleeBehavior(float fleeDistance = 30.0f);

} // namespace FPSBehaviors

} // namespace Spark::AI
