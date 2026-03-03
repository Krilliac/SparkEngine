// TestAIBehaviorTree.cpp - Tests for AI behavior tree logic
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <memory>
#include <any>

namespace TestAI {

enum class NodeStatus { Running, Success, Failure };

class Blackboard {
public:
    template<typename T>
    void Set(const std::string& key, const T& value) {
        m_data[key] = value;
    }

    template<typename T>
    T Get(const std::string& key, const T& defaultValue = T{}) const {
        auto it = m_data.find(key);
        if (it == m_data.end()) return defaultValue;
        try { return std::any_cast<T>(it->second); }
        catch (...) { return defaultValue; }
    }

    bool Has(const std::string& key) const {
        return m_data.count(key) > 0;
    }

    void Clear() { m_data.clear(); }

private:
    std::unordered_map<std::string, std::any> m_data;
};

class BTNode {
public:
    virtual ~BTNode() = default;
    virtual NodeStatus Tick(float dt, Blackboard& bb) = 0;
};

class SequenceNode : public BTNode {
public:
    void AddChild(std::unique_ptr<BTNode> child) { m_children.push_back(std::move(child)); }

    NodeStatus Tick(float dt, Blackboard& bb) override {
        for (auto& child : m_children) {
            NodeStatus status = child->Tick(dt, bb);
            if (status != NodeStatus::Success) return status;
        }
        return NodeStatus::Success;
    }
private:
    std::vector<std::unique_ptr<BTNode>> m_children;
};

class SelectorNode : public BTNode {
public:
    void AddChild(std::unique_ptr<BTNode> child) { m_children.push_back(std::move(child)); }

    NodeStatus Tick(float dt, Blackboard& bb) override {
        for (auto& child : m_children) {
            NodeStatus status = child->Tick(dt, bb);
            if (status != NodeStatus::Failure) return status;
        }
        return NodeStatus::Failure;
    }
private:
    std::vector<std::unique_ptr<BTNode>> m_children;
};

class ActionNode : public BTNode {
public:
    explicit ActionNode(std::function<NodeStatus(float, Blackboard&)> fn) : m_fn(fn) {}
    NodeStatus Tick(float dt, Blackboard& bb) override { return m_fn(dt, bb); }
private:
    std::function<NodeStatus(float, Blackboard&)> m_fn;
};

class ConditionNode : public BTNode {
public:
    explicit ConditionNode(std::function<bool(const Blackboard&)> fn) : m_fn(fn) {}
    NodeStatus Tick(float dt, Blackboard& bb) override {
        return m_fn(bb) ? NodeStatus::Success : NodeStatus::Failure;
    }
private:
    std::function<bool(const Blackboard&)> m_fn;
};

class InverterNode : public BTNode {
public:
    explicit InverterNode(std::unique_ptr<BTNode> child) : m_child(std::move(child)) {}
    NodeStatus Tick(float dt, Blackboard& bb) override {
        auto status = m_child->Tick(dt, bb);
        if (status == NodeStatus::Success) return NodeStatus::Failure;
        if (status == NodeStatus::Failure) return NodeStatus::Success;
        return NodeStatus::Running;
    }
private:
    std::unique_ptr<BTNode> m_child;
};

} // namespace TestAI

TEST(Blackboard_SetAndGet)
{
    TestAI::Blackboard bb;
    bb.Set("health", 100.0f);
    bb.Set("name", std::string("TestAgent"));
    bb.Set("alive", true);

    EXPECT_NEAR(bb.Get<float>("health"), 100.0f, 0.001f);
    EXPECT_EQ(bb.Get<std::string>("name"), std::string("TestAgent"));
    EXPECT_TRUE(bb.Get<bool>("alive"));
}

TEST(Blackboard_DefaultValues)
{
    TestAI::Blackboard bb;
    EXPECT_NEAR(bb.Get<float>("missing", 5.0f), 5.0f, 0.001f);
    EXPECT_FALSE(bb.Get<bool>("missing", false));
}

TEST(Blackboard_Has)
{
    TestAI::Blackboard bb;
    bb.Set("key", 42);
    EXPECT_TRUE(bb.Has("key"));
    EXPECT_FALSE(bb.Has("missing"));
}

TEST(Blackboard_Overwrite)
{
    TestAI::Blackboard bb;
    bb.Set("val", 10);
    bb.Set("val", 20);
    EXPECT_EQ(bb.Get<int>("val"), 20);
}

TEST(ActionNode_ReturnsSuccess)
{
    TestAI::Blackboard bb;
    TestAI::ActionNode node([](float, TestAI::Blackboard&) {
        return TestAI::NodeStatus::Success;
    });
    EXPECT_TRUE(node.Tick(0.016f, bb) == TestAI::NodeStatus::Success);
}

TEST(ActionNode_ReturnsFailure)
{
    TestAI::Blackboard bb;
    TestAI::ActionNode node([](float, TestAI::Blackboard&) {
        return TestAI::NodeStatus::Failure;
    });
    EXPECT_TRUE(node.Tick(0.016f, bb) == TestAI::NodeStatus::Failure);
}

TEST(ConditionNode_TrueCondition)
{
    TestAI::Blackboard bb;
    bb.Set("hasTarget", true);
    TestAI::ConditionNode node([](const TestAI::Blackboard& b) {
        return b.Get<bool>("hasTarget", false);
    });
    EXPECT_TRUE(node.Tick(0.016f, bb) == TestAI::NodeStatus::Success);
}

TEST(ConditionNode_FalseCondition)
{
    TestAI::Blackboard bb;
    bb.Set("hasTarget", false);
    TestAI::ConditionNode node([](const TestAI::Blackboard& b) {
        return b.Get<bool>("hasTarget", false);
    });
    EXPECT_TRUE(node.Tick(0.016f, bb) == TestAI::NodeStatus::Failure);
}

TEST(SequenceNode_AllSucceed)
{
    TestAI::Blackboard bb;
    TestAI::SequenceNode seq;
    seq.AddChild(std::make_unique<TestAI::ActionNode>(
        [](float, TestAI::Blackboard&) { return TestAI::NodeStatus::Success; }));
    seq.AddChild(std::make_unique<TestAI::ActionNode>(
        [](float, TestAI::Blackboard&) { return TestAI::NodeStatus::Success; }));
    EXPECT_TRUE(seq.Tick(0.016f, bb) == TestAI::NodeStatus::Success);
}

TEST(SequenceNode_OneFails)
{
    TestAI::Blackboard bb;
    TestAI::SequenceNode seq;
    seq.AddChild(std::make_unique<TestAI::ActionNode>(
        [](float, TestAI::Blackboard&) { return TestAI::NodeStatus::Success; }));
    seq.AddChild(std::make_unique<TestAI::ActionNode>(
        [](float, TestAI::Blackboard&) { return TestAI::NodeStatus::Failure; }));
    seq.AddChild(std::make_unique<TestAI::ActionNode>(
        [](float, TestAI::Blackboard&) { return TestAI::NodeStatus::Success; }));
    EXPECT_TRUE(seq.Tick(0.016f, bb) == TestAI::NodeStatus::Failure);
}

TEST(SelectorNode_FirstSucceeds)
{
    TestAI::Blackboard bb;
    TestAI::SelectorNode sel;
    sel.AddChild(std::make_unique<TestAI::ActionNode>(
        [](float, TestAI::Blackboard&) { return TestAI::NodeStatus::Success; }));
    sel.AddChild(std::make_unique<TestAI::ActionNode>(
        [](float, TestAI::Blackboard&) { return TestAI::NodeStatus::Failure; }));
    EXPECT_TRUE(sel.Tick(0.016f, bb) == TestAI::NodeStatus::Success);
}

TEST(SelectorNode_AllFail)
{
    TestAI::Blackboard bb;
    TestAI::SelectorNode sel;
    sel.AddChild(std::make_unique<TestAI::ActionNode>(
        [](float, TestAI::Blackboard&) { return TestAI::NodeStatus::Failure; }));
    sel.AddChild(std::make_unique<TestAI::ActionNode>(
        [](float, TestAI::Blackboard&) { return TestAI::NodeStatus::Failure; }));
    EXPECT_TRUE(sel.Tick(0.016f, bb) == TestAI::NodeStatus::Failure);
}

TEST(SelectorNode_SecondSucceeds)
{
    TestAI::Blackboard bb;
    TestAI::SelectorNode sel;
    sel.AddChild(std::make_unique<TestAI::ActionNode>(
        [](float, TestAI::Blackboard&) { return TestAI::NodeStatus::Failure; }));
    sel.AddChild(std::make_unique<TestAI::ActionNode>(
        [](float, TestAI::Blackboard&) { return TestAI::NodeStatus::Success; }));
    EXPECT_TRUE(sel.Tick(0.016f, bb) == TestAI::NodeStatus::Success);
}

TEST(InverterNode_InvertsSuccess)
{
    TestAI::Blackboard bb;
    auto inverter = std::make_unique<TestAI::InverterNode>(
        std::make_unique<TestAI::ActionNode>(
            [](float, TestAI::Blackboard&) { return TestAI::NodeStatus::Success; }));
    EXPECT_TRUE(inverter->Tick(0.016f, bb) == TestAI::NodeStatus::Failure);
}

TEST(InverterNode_InvertsFailure)
{
    TestAI::Blackboard bb;
    auto inverter = std::make_unique<TestAI::InverterNode>(
        std::make_unique<TestAI::ActionNode>(
            [](float, TestAI::Blackboard&) { return TestAI::NodeStatus::Failure; }));
    EXPECT_TRUE(inverter->Tick(0.016f, bb) == TestAI::NodeStatus::Success);
}

TEST(InverterNode_KeepsRunning)
{
    TestAI::Blackboard bb;
    auto inverter = std::make_unique<TestAI::InverterNode>(
        std::make_unique<TestAI::ActionNode>(
            [](float, TestAI::Blackboard&) { return TestAI::NodeStatus::Running; }));
    EXPECT_TRUE(inverter->Tick(0.016f, bb) == TestAI::NodeStatus::Running);
}
