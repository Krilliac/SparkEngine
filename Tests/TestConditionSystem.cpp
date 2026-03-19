// TestConditionSystem.cpp - Tests for stateless condition evaluation system
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace TestCondition
{

    using EntityID = uint32_t;

    enum class ConditionType : uint16_t
    {
        IsAlive,
        IsDead,
        HealthAbove,
        HealthBelow,
        HasAura,
        LevelAbove,
        LevelBelow,
        FlagSet,
        VariableEquals,
        VariableGreaterThan,
        VariableLessThan,
        AlwaysTrue,
        AlwaysFalse,
        Custom,
        Count
    };

    using ConditionParam = std::variant<std::monostate, int64_t, double, std::string, uint32_t>;

    struct Condition
    {
        ConditionType type = ConditionType::AlwaysTrue;
        ConditionParam param1;
        ConditionParam param2;
        bool negated = false;
    };

    enum class ConditionGroupLogic : uint8_t
    {
        And,
        Or
    };

    struct ConditionGroup
    {
        ConditionGroupLogic logic = ConditionGroupLogic::And;
        std::vector<Condition> conditions;
    };

    struct ConditionSet
    {
        std::vector<ConditionGroup> groups;

        bool IsEmpty() const { return groups.empty(); }
    };

    using CustomConditionEvaluator = std::function<bool(EntityID, const Condition&)>;

    class ConditionSystem
    {
      public:
        void Initialize()
        {
            m_customEvaluators.clear();
            m_variables.clear();
            m_flags.clear();
        }

        void Shutdown()
        {
            m_customEvaluators.clear();
            m_variables.clear();
            m_flags.clear();
        }

        bool Evaluate(const ConditionSet& conditions, EntityID entity) const
        {
            if (conditions.IsEmpty())
            {
                return true;
            }

            for (const auto& group : conditions.groups)
            {
                if (!EvaluateGroup(group, entity))
                {
                    return false;
                }
            }
            return true;
        }

        bool EvaluateCondition(const Condition& condition, EntityID entity) const
        {
            bool result = EvaluateSingle(condition.type, condition.param1, condition.param2, entity);
            return condition.negated ? !result : result;
        }

        void RegisterCustomEvaluator(const std::string& id, CustomConditionEvaluator evaluator)
        {
            m_customEvaluators[id] = std::move(evaluator);
        }

        void SetVariable(const std::string& name, int64_t value) { m_variables[name] = value; }

        int64_t GetVariable(const std::string& name) const
        {
            auto it = m_variables.find(name);
            return (it != m_variables.end()) ? it->second : 0;
        }

        void SetFlag(const std::string& name, bool value) { m_flags[name] = value; }

        bool GetFlag(const std::string& name) const
        {
            auto it = m_flags.find(name);
            return (it != m_flags.end()) ? it->second : false;
        }

      private:
        bool EvaluateGroup(const ConditionGroup& group, EntityID entity) const
        {
            if (group.conditions.empty())
            {
                return true;
            }

            if (group.logic == ConditionGroupLogic::And)
            {
                for (const auto& cond : group.conditions)
                {
                    if (!EvaluateCondition(cond, entity))
                    {
                        return false;
                    }
                }
                return true;
            }
            else
            {
                for (const auto& cond : group.conditions)
                {
                    if (EvaluateCondition(cond, entity))
                    {
                        return true;
                    }
                }
                return false;
            }
        }

        bool EvaluateSingle(ConditionType type, const ConditionParam& p1, const ConditionParam& p2,
                            EntityID /*entity*/) const
        {
            switch (type)
            {
            case ConditionType::AlwaysTrue:
                return true;
            case ConditionType::AlwaysFalse:
                return false;

            case ConditionType::FlagSet:
            {
                auto* str = std::get_if<std::string>(&p1);
                return str ? GetFlag(*str) : false;
            }

            case ConditionType::VariableEquals:
            {
                auto* varName = std::get_if<std::string>(&p1);
                auto* value = std::get_if<int64_t>(&p2);
                if (varName && value)
                {
                    return GetVariable(*varName) == *value;
                }
                return false;
            }

            case ConditionType::VariableGreaterThan:
            {
                auto* varName = std::get_if<std::string>(&p1);
                auto* value = std::get_if<int64_t>(&p2);
                if (varName && value)
                {
                    return GetVariable(*varName) > *value;
                }
                return false;
            }

            case ConditionType::VariableLessThan:
            {
                auto* varName = std::get_if<std::string>(&p1);
                auto* value = std::get_if<int64_t>(&p2);
                if (varName && value)
                {
                    return GetVariable(*varName) < *value;
                }
                return false;
            }

            case ConditionType::Custom:
            {
                auto* customId = std::get_if<std::string>(&p1);
                if (customId)
                {
                    auto it = m_customEvaluators.find(*customId);
                    if (it != m_customEvaluators.end())
                    {
                        Condition cond;
                        cond.type = type;
                        cond.param1 = p1;
                        cond.param2 = p2;
                        return it->second(0, cond);
                    }
                }
                return false;
            }

            default:
                return false;
            }
        }

        std::unordered_map<std::string, CustomConditionEvaluator> m_customEvaluators;
        std::unordered_map<std::string, int64_t> m_variables;
        std::unordered_map<std::string, bool> m_flags;
    };

} // namespace TestCondition

// ============================================================================
// Tests
// ============================================================================

TEST(ConditionSystem_AlwaysTrueAndAlwaysFalse)
{
    TestCondition::ConditionSystem sys;
    sys.Initialize();

    TestCondition::Condition alwaysTrue;
    alwaysTrue.type = TestCondition::ConditionType::AlwaysTrue;

    TestCondition::Condition alwaysFalse;
    alwaysFalse.type = TestCondition::ConditionType::AlwaysFalse;

    EXPECT_TRUE(sys.EvaluateCondition(alwaysTrue, 1));
    EXPECT_FALSE(sys.EvaluateCondition(alwaysFalse, 1));

    sys.Shutdown();
}

TEST(ConditionSystem_NegatedCondition)
{
    TestCondition::ConditionSystem sys;
    sys.Initialize();

    TestCondition::Condition negatedTrue;
    negatedTrue.type = TestCondition::ConditionType::AlwaysTrue;
    negatedTrue.negated = true;

    TestCondition::Condition negatedFalse;
    negatedFalse.type = TestCondition::ConditionType::AlwaysFalse;
    negatedFalse.negated = true;

    EXPECT_FALSE(sys.EvaluateCondition(negatedTrue, 1));
    EXPECT_TRUE(sys.EvaluateCondition(negatedFalse, 1));

    sys.Shutdown();
}

TEST(ConditionSystem_AndGroupAllMustPass)
{
    TestCondition::ConditionSystem sys;
    sys.Initialize();

    TestCondition::ConditionGroup andGroup;
    andGroup.logic = TestCondition::ConditionGroupLogic::And;
    andGroup.conditions.push_back({TestCondition::ConditionType::AlwaysTrue, {}, {}, false});
    andGroup.conditions.push_back({TestCondition::ConditionType::AlwaysTrue, {}, {}, false});

    TestCondition::ConditionSet set;
    set.groups.push_back(andGroup);
    EXPECT_TRUE(sys.Evaluate(set, 1));

    // Add a failing condition — entire group should fail
    andGroup.conditions.push_back({TestCondition::ConditionType::AlwaysFalse, {}, {}, false});
    set.groups.clear();
    set.groups.push_back(andGroup);
    EXPECT_FALSE(sys.Evaluate(set, 1));

    sys.Shutdown();
}

TEST(ConditionSystem_OrGroupAnyCanPass)
{
    TestCondition::ConditionSystem sys;
    sys.Initialize();

    TestCondition::ConditionGroup orGroup;
    orGroup.logic = TestCondition::ConditionGroupLogic::Or;
    orGroup.conditions.push_back({TestCondition::ConditionType::AlwaysFalse, {}, {}, false});
    orGroup.conditions.push_back({TestCondition::ConditionType::AlwaysTrue, {}, {}, false});

    TestCondition::ConditionSet set;
    set.groups.push_back(orGroup);
    EXPECT_TRUE(sys.Evaluate(set, 1));

    // All false — OR group should fail
    TestCondition::ConditionGroup allFalse;
    allFalse.logic = TestCondition::ConditionGroupLogic::Or;
    allFalse.conditions.push_back({TestCondition::ConditionType::AlwaysFalse, {}, {}, false});
    allFalse.conditions.push_back({TestCondition::ConditionType::AlwaysFalse, {}, {}, false});

    set.groups.clear();
    set.groups.push_back(allFalse);
    EXPECT_FALSE(sys.Evaluate(set, 1));

    sys.Shutdown();
}

TEST(ConditionSystem_VariableSetAndGet)
{
    TestCondition::ConditionSystem sys;
    sys.Initialize();

    EXPECT_EQ(sys.GetVariable("score"), int64_t(0));

    sys.SetVariable("score", 42);
    EXPECT_EQ(sys.GetVariable("score"), int64_t(42));

    sys.SetVariable("score", -10);
    EXPECT_EQ(sys.GetVariable("score"), int64_t(-10));

    // Unset variable returns 0
    EXPECT_EQ(sys.GetVariable("nonexistent"), int64_t(0));

    sys.Shutdown();
}

TEST(ConditionSystem_VariableEqualsEvaluation)
{
    TestCondition::ConditionSystem sys;
    sys.Initialize();

    sys.SetVariable("quest_step", 3);

    TestCondition::Condition cond;
    cond.type = TestCondition::ConditionType::VariableEquals;
    cond.param1 = std::string("quest_step");
    cond.param2 = int64_t(3);
    EXPECT_TRUE(sys.EvaluateCondition(cond, 1));

    cond.param2 = int64_t(5);
    EXPECT_FALSE(sys.EvaluateCondition(cond, 1));

    sys.Shutdown();
}

TEST(ConditionSystem_VariableGreaterThanAndLessThan)
{
    TestCondition::ConditionSystem sys;
    sys.Initialize();

    sys.SetVariable("power", 50);

    TestCondition::Condition gt;
    gt.type = TestCondition::ConditionType::VariableGreaterThan;
    gt.param1 = std::string("power");
    gt.param2 = int64_t(25);
    EXPECT_TRUE(sys.EvaluateCondition(gt, 1));

    gt.param2 = int64_t(50);
    EXPECT_FALSE(sys.EvaluateCondition(gt, 1));

    gt.param2 = int64_t(75);
    EXPECT_FALSE(sys.EvaluateCondition(gt, 1));

    TestCondition::Condition lt;
    lt.type = TestCondition::ConditionType::VariableLessThan;
    lt.param1 = std::string("power");
    lt.param2 = int64_t(75);
    EXPECT_TRUE(sys.EvaluateCondition(lt, 1));

    lt.param2 = int64_t(50);
    EXPECT_FALSE(sys.EvaluateCondition(lt, 1));

    lt.param2 = int64_t(25);
    EXPECT_FALSE(sys.EvaluateCondition(lt, 1));

    sys.Shutdown();
}

TEST(ConditionSystem_FlagSetAndGet)
{
    TestCondition::ConditionSystem sys;
    sys.Initialize();

    EXPECT_FALSE(sys.GetFlag("door_unlocked"));

    sys.SetFlag("door_unlocked", true);
    EXPECT_TRUE(sys.GetFlag("door_unlocked"));

    sys.SetFlag("door_unlocked", false);
    EXPECT_FALSE(sys.GetFlag("door_unlocked"));

    // Unset flag returns false
    EXPECT_FALSE(sys.GetFlag("nonexistent_flag"));

    sys.Shutdown();
}

TEST(ConditionSystem_FlagSetConditionEvaluation)
{
    TestCondition::ConditionSystem sys;
    sys.Initialize();

    TestCondition::Condition cond;
    cond.type = TestCondition::ConditionType::FlagSet;
    cond.param1 = std::string("boss_defeated");

    EXPECT_FALSE(sys.EvaluateCondition(cond, 1));

    sys.SetFlag("boss_defeated", true);
    EXPECT_TRUE(sys.EvaluateCondition(cond, 1));

    // Negated flag check
    cond.negated = true;
    EXPECT_FALSE(sys.EvaluateCondition(cond, 1));

    sys.Shutdown();
}

TEST(ConditionSystem_CustomEvaluatorRegistration)
{
    TestCondition::ConditionSystem sys;
    sys.Initialize();

    bool customCalled = false;
    sys.RegisterCustomEvaluator(
        "is_premium",
        [&customCalled](TestCondition::EntityID /*entity*/, const TestCondition::Condition& /*cond*/) -> bool
        {
            customCalled = true;
            return true;
        });

    TestCondition::Condition cond;
    cond.type = TestCondition::ConditionType::Custom;
    cond.param1 = std::string("is_premium");

    EXPECT_TRUE(sys.EvaluateCondition(cond, 1));
    EXPECT_TRUE(customCalled);

    // Unregistered custom evaluator returns false
    TestCondition::Condition unknown;
    unknown.type = TestCondition::ConditionType::Custom;
    unknown.param1 = std::string("unknown_evaluator");
    EXPECT_FALSE(sys.EvaluateCondition(unknown, 1));

    sys.Shutdown();
}

TEST(ConditionSystem_EmptyConditionSetEvaluatesToTrue)
{
    TestCondition::ConditionSystem sys;
    sys.Initialize();

    TestCondition::ConditionSet emptySet;
    EXPECT_TRUE(emptySet.IsEmpty());
    EXPECT_TRUE(sys.Evaluate(emptySet, 1));

    sys.Shutdown();
}

TEST(ConditionSystem_MixedConditionGroups)
{
    TestCondition::ConditionSystem sys;
    sys.Initialize();

    sys.SetFlag("has_key", true);
    sys.SetVariable("gold", 100);

    // Group 1 (AND): flag "has_key" is set AND variable "gold" > 50
    TestCondition::ConditionGroup andGroup;
    andGroup.logic = TestCondition::ConditionGroupLogic::And;

    TestCondition::Condition flagCond;
    flagCond.type = TestCondition::ConditionType::FlagSet;
    flagCond.param1 = std::string("has_key");
    andGroup.conditions.push_back(flagCond);

    TestCondition::Condition goldCond;
    goldCond.type = TestCondition::ConditionType::VariableGreaterThan;
    goldCond.param1 = std::string("gold");
    goldCond.param2 = int64_t(50);
    andGroup.conditions.push_back(goldCond);

    // Group 2 (OR): AlwaysFalse OR variable "gold" == 100
    TestCondition::ConditionGroup orGroup;
    orGroup.logic = TestCondition::ConditionGroupLogic::Or;
    orGroup.conditions.push_back({TestCondition::ConditionType::AlwaysFalse, {}, {}, false});

    TestCondition::Condition goldEq;
    goldEq.type = TestCondition::ConditionType::VariableEquals;
    goldEq.param1 = std::string("gold");
    goldEq.param2 = int64_t(100);
    orGroup.conditions.push_back(goldEq);

    // Both groups must pass (AND between groups)
    TestCondition::ConditionSet set;
    set.groups.push_back(andGroup);
    set.groups.push_back(orGroup);
    EXPECT_TRUE(sys.Evaluate(set, 1));

    // Remove the flag — AND group fails, entire set fails
    sys.SetFlag("has_key", false);
    EXPECT_FALSE(sys.Evaluate(set, 1));

    sys.Shutdown();
}
