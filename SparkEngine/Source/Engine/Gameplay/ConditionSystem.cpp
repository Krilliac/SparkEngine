/**
 * @file ConditionSystem.cpp
 * @brief Implementation of the universal condition evaluation system
 */

#include "ConditionSystem.h"
#include "../ECS/Components/GameplayComponents.h"

namespace Spark::Gameplay
{

    // ============================================================================
    // Singleton
    // ============================================================================

    ConditionSystem& ConditionSystem::GetInstance()
    {
        static ConditionSystem instance;
        return instance;
    }

    // ============================================================================
    // Lifecycle
    // ============================================================================

    void ConditionSystem::Initialize()
    {
        m_customEvaluators.clear();
        m_variables.clear();
        m_flags.clear();
    }

    void ConditionSystem::Shutdown()
    {
        m_customEvaluators.clear();
        m_variables.clear();
        m_flags.clear();
    }

    // ============================================================================
    // Public evaluation API
    // ============================================================================

    bool ConditionSystem::Evaluate(const ConditionSet& conditions, EntityID entity, World& world) const
    {
        if (conditions.IsEmpty())
        {
            return true;
        }

        // All groups must pass (AND between groups)
        for (const auto& group : conditions.groups)
        {
            if (!EvaluateGroup(group, entity, world))
            {
                return false;
            }
        }
        return true;
    }

    bool ConditionSystem::EvaluateCondition(const Condition& condition, EntityID entity, World& world) const
    {
        bool result = EvaluateSingle(condition.type, condition.param1, condition.param2, entity, world);
        return condition.negated ? !result : result;
    }

    // ============================================================================
    // Custom evaluators
    // ============================================================================

    void ConditionSystem::RegisterCustomEvaluator(const std::string& id, CustomConditionEvaluator evaluator)
    {
        m_customEvaluators[id] = std::move(evaluator);
    }

    // ============================================================================
    // World variables
    // ============================================================================

    void ConditionSystem::SetVariable(const std::string& name, int64_t value)
    {
        m_variables[name] = value;
    }

    int64_t ConditionSystem::GetVariable(const std::string& name) const
    {
        auto it = m_variables.find(name);
        return (it != m_variables.end()) ? it->second : 0;
    }

    void ConditionSystem::SetFlag(const std::string& name, bool value)
    {
        m_flags[name] = value;
    }

    bool ConditionSystem::GetFlag(const std::string& name) const
    {
        auto it = m_flags.find(name);
        return (it != m_flags.end()) ? it->second : false;
    }

    // ============================================================================
    // Private evaluation helpers
    // ============================================================================

    bool ConditionSystem::EvaluateGroup(const ConditionGroup& group, EntityID entity, World& world) const
    {
        if (group.conditions.empty())
        {
            return true;
        }

        if (group.logic == ConditionGroupLogic::And)
        {
            for (const auto& cond : group.conditions)
            {
                if (!EvaluateCondition(cond, entity, world))
                {
                    return false;
                }
            }
            return true;
        }
        else // Or
        {
            for (const auto& cond : group.conditions)
            {
                if (EvaluateCondition(cond, entity, world))
                {
                    return true;
                }
            }
            return false;
        }
    }

    bool ConditionSystem::EvaluateSingle(ConditionType type, const ConditionParam& p1, const ConditionParam& p2,
                                         EntityID entity, World& world) const
    {
        // Suppress unused parameter warnings for entity/world — many condition
        // types are placeholders that will use them once fully integrated.
        (void)entity;
        (void)world;

        switch (type)
        {
        // ---- Meta conditions ----
        case ConditionType::AlwaysTrue:
            return true;

        case ConditionType::AlwaysFalse:
            return false;

        // ---- Flag/Variable conditions ----
        case ConditionType::FlagSet:
        {
            if (auto* str = std::get_if<std::string>(&p1))
            {
                return GetFlag(*str);
            }
            return false;
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

        // ---- Custom evaluator ----
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
                    return it->second(entity, cond);
                }
            }
            return false;
        }

        // ---- Placeholder conditions (return true until integrated) ----
        // These will be implemented as subsystems are wired in.
        case ConditionType::IsAlive:
        case ConditionType::IsDead:
        case ConditionType::HealthAbove:
        case ConditionType::HealthBelow:
        case ConditionType::HasComponent:
        case ConditionType::HasTag:
        case ConditionType::HasAura:
        case ConditionType::InArea:
        case ConditionType::NearEntity:
        case ConditionType::InCell:
        case ConditionType::HasItem:
        case ConditionType::QuestComplete:
        case ConditionType::QuestActive:
        case ConditionType::LevelAbove:
        case ConditionType::LevelBelow:
        case ConditionType::HasAbility:
        case ConditionType::IsClass:
        case ConditionType::IsFaction:
        case ConditionType::TimeOfDayBetween:
        case ConditionType::WeatherIs:
            return true;

        case ConditionType::Count:
            return false;
        }

        return false;
    }

} // namespace Spark::Gameplay
