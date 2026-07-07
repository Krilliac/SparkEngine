/**
 * @file ConditionSystem.cpp
 * @brief Implementation of the universal condition evaluation system
 */

#include "ConditionSystem.h"
#include "../ECS/Components.h"
#include "../ECS/Components/GameplayComponents.h"
#include "../ECS/Components/AIComponents.h"
#include "../../Graphics/WeatherSystem.h"
#include "../../Utils/Validate.h"
#include "AbilitySystem.h"
#include "../../Core/EngineContext.h"

#include <cmath>

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
        SPARK_LOG_INFO(Spark::LogCategory::Core, "ConditionSystem initialized");
    }

    void ConditionSystem::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "ConditionSystem shutting down (%zu evaluators, %zu variables)",
                       m_customEvaluators.size(), m_variables.size());
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
        SPARK_LOG_INFO(Spark::LogCategory::Core, "ConditionSystem: registered custom evaluator '%s'", id.c_str());
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
        auto entityId = static_cast<entt::entity>(entity);

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

        // ---- Entity state conditions ----
        case ConditionType::IsAlive:
        {
            auto* health = world.GetComponent<HealthComponent>(entityId);
            return health ? !health->isDead : true;
        }

        case ConditionType::IsDead:
        {
            auto* health = world.GetComponent<HealthComponent>(entityId);
            return health ? health->isDead : false;
        }

        case ConditionType::HealthAbove:
        {
            auto* health = world.GetComponent<HealthComponent>(entityId);
            if (!health || health->maxHealth <= 0.0f)
                return false;
            auto* threshold = std::get_if<double>(&p1);
            auto* thresholdInt = std::get_if<int64_t>(&p1);
            float pct = 0.0f;
            if (threshold)
                pct = static_cast<float>(*threshold);
            else if (thresholdInt)
                pct = static_cast<float>(*thresholdInt) / 100.0f;
            return (health->health / health->maxHealth) > pct;
        }

        case ConditionType::HealthBelow:
        {
            auto* health = world.GetComponent<HealthComponent>(entityId);
            if (!health || health->maxHealth <= 0.0f)
                return true;
            auto* threshold = std::get_if<double>(&p1);
            auto* thresholdInt = std::get_if<int64_t>(&p1);
            float pct = 1.0f;
            if (threshold)
                pct = static_cast<float>(*threshold);
            else if (thresholdInt)
                pct = static_cast<float>(*thresholdInt) / 100.0f;
            return (health->health / health->maxHealth) < pct;
        }

        case ConditionType::HasComponent:
        {
            // Check by component name string against known component types
            auto* compName = std::get_if<std::string>(&p1);
            if (!compName)
                return false;
            if (*compName == "Transform")
                return world.HasComponent<Transform>(entityId);
            if (*compName == "MeshRenderer")
                return world.HasComponent<MeshRenderer>(entityId);
            if (*compName == "HealthComponent")
                return world.HasComponent<HealthComponent>(entityId);
            if (*compName == "AIComponent")
                return world.HasComponent<AIComponent>(entityId);
            if (*compName == "TagComponent")
                return world.HasComponent<TagComponent>(entityId);
            if (*compName == "AbilityComponent")
                return world.HasComponent<AbilityComponent>(entityId);
            if (*compName == "InventoryTag")
                return world.HasComponent<InventoryTag>(entityId);
            if (*compName == "QuestTrackerTag")
                return world.HasComponent<QuestTrackerTag>(entityId);
            if (*compName == "RigidBodyComponent")
                return world.HasComponent<RigidBodyComponent>(entityId);
            if (*compName == "AnimationController")
                return world.HasComponent<AnimationController>(entityId);
            return false;
        }

        case ConditionType::HasTag:
        {
            auto* tagName = std::get_if<std::string>(&p1);
            if (!tagName)
                return false;
            auto* tagComp = world.GetComponent<TagComponent>(entityId);
            return tagComp ? tagComp->HasTag(*tagName) : false;
        }

        case ConditionType::HasAura:
        {
            auto* auraId = std::get_if<uint32_t>(&p1);
            auto* auraIdInt = std::get_if<int64_t>(&p1);
            uint32_t id = 0;
            if (auraId)
                id = *auraId;
            else if (auraIdInt)
                id = static_cast<uint32_t>(*auraIdInt);
            else
                return false;
            return AbilitySystem::GetInstance().HasAura(entity, id);
        }

        // ---- Spatial conditions ----
        case ConditionType::InArea:
        {
            // Check via world variable "entity_area_<entityId>" or TagComponent "area:<areaId>"
            auto* areaId = std::get_if<uint32_t>(&p1);
            auto* areaIdInt = std::get_if<int64_t>(&p1);
            uint32_t targetArea = 0;
            if (areaId)
                targetArea = *areaId;
            else if (areaIdInt)
                targetArea = static_cast<uint32_t>(*areaIdInt);
            else
                return false;
            auto* tagComp = world.GetComponent<TagComponent>(entityId);
            if (tagComp)
            {
                return tagComp->HasTag("area:" + std::to_string(targetArea));
            }
            return false;
        }

        case ConditionType::NearEntity:
        {
            auto* targetIdParam = std::get_if<uint32_t>(&p1);
            auto* targetIdInt = std::get_if<int64_t>(&p1);
            auto* radiusParam = std::get_if<double>(&p2);
            auto* radiusInt = std::get_if<int64_t>(&p2);

            uint32_t targetId = 0;
            if (targetIdParam)
                targetId = *targetIdParam;
            else if (targetIdInt)
                targetId = static_cast<uint32_t>(*targetIdInt);
            else
                return false;

            float radius = 10.0f;
            if (radiusParam)
                radius = static_cast<float>(*radiusParam);
            else if (radiusInt)
                radius = static_cast<float>(*radiusInt);

            auto* myTransform = world.GetComponent<Transform>(entityId);
            auto* otherTransform = world.GetComponent<Transform>(static_cast<entt::entity>(targetId));
            if (!myTransform || !otherTransform)
                return false;

            float dx = myTransform->position.x - otherTransform->position.x;
            float dy = myTransform->position.y - otherTransform->position.y;
            float dz = myTransform->position.z - otherTransform->position.z;
            return (dx * dx + dy * dy + dz * dz) <= (radius * radius);
        }

        case ConditionType::InCell:
        {
            auto* cellX = std::get_if<int64_t>(&p1);
            auto* cellZ = std::get_if<int64_t>(&p2);
            if (!cellX || !cellZ)
                return false;

            auto* transform = world.GetComponent<Transform>(entityId);
            if (!transform)
                return false;

            // Default cell size of 64 world units (matches SpatialGrid default)
            constexpr float kCellSize = 64.0f;
            int entityCellX = static_cast<int>(std::floor(transform->position.x / kCellSize));
            int entityCellZ = static_cast<int>(std::floor(transform->position.z / kCellSize));
            return entityCellX == static_cast<int>(*cellX) && entityCellZ == static_cast<int>(*cellZ);
        }

        // ---- Gameplay conditions ----
        case ConditionType::HasItem:
        {
            // Check if entity has inventory and use world variable for item counts
            auto* itemId = std::get_if<uint32_t>(&p1);
            auto* itemIdInt = std::get_if<int64_t>(&p1);
            if (!itemId && !itemIdInt)
                return false;

            if (!world.HasComponent<InventoryTag>(entityId))
                return false;

            uint32_t id = itemId ? *itemId : static_cast<uint32_t>(*itemIdInt);
            std::string varKey = "inv_" + std::to_string(entity) + "_item_" + std::to_string(id);
            int64_t count = GetVariable(varKey);

            // If param2 specifies a required count, check against it
            auto* reqCount = std::get_if<int64_t>(&p2);
            return count >= (reqCount ? *reqCount : 1);
        }

        case ConditionType::QuestComplete:
        {
            auto* questId = std::get_if<uint32_t>(&p1);
            auto* questIdInt = std::get_if<int64_t>(&p1);
            uint32_t id = questId ? *questId : (questIdInt ? static_cast<uint32_t>(*questIdInt) : 0);
            return GetFlag("quest_" + std::to_string(id) + "_complete");
        }

        case ConditionType::QuestActive:
        {
            auto* questId = std::get_if<uint32_t>(&p1);
            auto* questIdInt = std::get_if<int64_t>(&p1);
            uint32_t id = questId ? *questId : (questIdInt ? static_cast<uint32_t>(*questIdInt) : 0);
            return GetFlag("quest_" + std::to_string(id) + "_active");
        }

        case ConditionType::LevelAbove:
        {
            auto* level = std::get_if<int64_t>(&p1);
            if (!level)
                return false;
            std::string varKey = "entity_level_" + std::to_string(entity);
            return GetVariable(varKey) > *level;
        }

        case ConditionType::LevelBelow:
        {
            auto* level = std::get_if<int64_t>(&p1);
            if (!level)
                return false;
            std::string varKey = "entity_level_" + std::to_string(entity);
            return GetVariable(varKey) < *level;
        }

        case ConditionType::HasAbility:
        {
            auto* abilityId = std::get_if<uint32_t>(&p1);
            auto* abilityIdInt = std::get_if<int64_t>(&p1);
            uint32_t id = 0;
            if (abilityId)
                id = *abilityId;
            else if (abilityIdInt)
                id = static_cast<uint32_t>(*abilityIdInt);
            else
                return false;

            // AbilityFlags is a 32-bit flag set; shifting 1u by >= 32 is undefined
            // behaviour. A data-defined ability id out of range simply has no bit.
            if (id >= 32u)
            {
                return false;
            }

            auto* abilityComp = world.GetComponent<AbilityComponent>(entityId);
            if (abilityComp)
            {
                // Check if the ability flag bit is set
                auto flag = static_cast<AbilityFlags>(1u << id);
                return abilityComp->abilities.Has(flag);
            }
            return false;
        }

        // ---- Class/faction via tags ----
        case ConditionType::IsClass:
        {
            auto* classId = std::get_if<uint32_t>(&p1);
            auto* classIdInt = std::get_if<int64_t>(&p1);
            auto* className = std::get_if<std::string>(&p1);
            auto* tagComp = world.GetComponent<TagComponent>(entityId);
            if (!tagComp)
                return false;
            if (className)
                return tagComp->HasTag("class:" + *className);
            uint32_t id = classId ? *classId : (classIdInt ? static_cast<uint32_t>(*classIdInt) : 0);
            return tagComp->HasTag("class:" + std::to_string(id));
        }

        case ConditionType::IsFaction:
        {
            auto* factionId = std::get_if<uint32_t>(&p1);
            auto* factionIdInt = std::get_if<int64_t>(&p1);
            auto* factionName = std::get_if<std::string>(&p1);
            auto* tagComp = world.GetComponent<TagComponent>(entityId);
            if (!tagComp)
                return false;
            if (factionName)
                return tagComp->HasTag("faction:" + *factionName);
            uint32_t id = factionId ? *factionId : (factionIdInt ? static_cast<uint32_t>(*factionIdInt) : 0);
            return tagComp->HasTag("faction:" + std::to_string(id));
        }

        // ---- World state conditions ----
        case ConditionType::TimeOfDayBetween:
        {
            auto* startHour = std::get_if<double>(&p1);
            auto* endHour = std::get_if<double>(&p2);
            auto* startInt = std::get_if<int64_t>(&p1);
            auto* endInt = std::get_if<int64_t>(&p2);

            float start =
                startHour ? static_cast<float>(*startHour) : (startInt ? static_cast<float>(*startInt) : 0.0f);
            float end = endHour ? static_cast<float>(*endHour) : (endInt ? static_cast<float>(*endInt) : 24.0f);

            // Read current time of day from world variable (0-24 hours)
            float currentHour = static_cast<float>(GetVariable("time_of_day")) / 100.0f;

            if (start <= end)
                return currentHour >= start && currentHour <= end;
            // Wrap-around (e.g., 22:00 to 06:00)
            return currentHour >= start || currentHour <= end;
        }

        case ConditionType::WeatherIs:
        {
            auto* weatherType = std::get_if<int64_t>(&p1);
            auto* weatherTypeUint = std::get_if<uint32_t>(&p1);
            int targetType = 0;
            if (weatherType)
                targetType = static_cast<int>(*weatherType);
            else if (weatherTypeUint)
                targetType = static_cast<int>(*weatherTypeUint);
            else
                return false;

            // Query the WeatherSystem for current weather state
            auto* ctx = EngineContext::Get();
            if (ctx)
            {
                auto* weatherSys = ctx->GetWeather();
                if (weatherSys)
                {
                    return static_cast<int>(weatherSys->GetCurrentState().type) == targetType;
                }
            }
            return false;
        }

        case ConditionType::Count:
            return false;
        }

        return false;
    }

} // namespace Spark::Gameplay
