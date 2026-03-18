/**
 * @file ConditionSystem.h
 * @brief Universal data-driven condition evaluation (TC ConditionMgr-inspired)
 *
 * Single evaluation engine used by quests, dialogue, AI, loot tables, shops.
 * Conditions are composable with AND/OR/NOT logic.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

class World; // Forward-declare the ECS World (defined in Components.h)

namespace Spark::Gameplay
{

    using EntityID = uint32_t;

    enum class ConditionType : uint16_t
    {
        // Entity state
        IsAlive,
        IsDead,
        HealthAbove, // param: float threshold (0-1 percentage)
        HealthBelow,
        HasComponent, // param: string componentName
        HasTag,       // param: string tagName
        HasAura,      // param: uint32 auraId

        // Spatial
        InArea,     // param: uint32 areaId
        NearEntity, // param: entityId, float radius
        InCell,     // param: int32 cellX, int32 cellZ

        // Gameplay
        HasItem,       // param: uint32 itemId, int count
        QuestComplete, // param: uint32 questId
        QuestActive,   // param: uint32 questId
        LevelAbove,    // param: int level
        LevelBelow,
        HasAbility, // param: uint32 abilityId

        // Class/faction
        IsClass,   // param: uint32 classId
        IsFaction, // param: uint32 factionId

        // World state
        TimeOfDayBetween, // param: float startHour, float endHour
        WeatherIs,        // param: int weatherType

        // Generic
        FlagSet,        // param: string flagName
        VariableEquals, // param: string varName, int value
        VariableGreaterThan,
        VariableLessThan,

        // Meta
        AlwaysTrue,
        AlwaysFalse,

        Custom, // param: string customId - evaluated via registered callback

        Count
    };

    using ConditionParam = std::variant<std::monostate, int64_t, double, std::string, uint32_t>;

    struct Condition
    {
        ConditionType type = ConditionType::AlwaysTrue;
        ConditionParam param1;
        ConditionParam param2;
        bool negated = false; // NOT this condition
    };

    enum class ConditionGroupLogic : uint8_t
    {
        And, // All conditions in group must be true
        Or   // Any condition in group must be true
    };

    struct ConditionGroup
    {
        ConditionGroupLogic logic = ConditionGroupLogic::And;
        std::vector<Condition> conditions;
    };

    // Multiple groups are combined with AND by default
    // (Group1 AND Group2 AND ...) where each group uses its own logic
    struct ConditionSet
    {
        std::vector<ConditionGroup> groups;

        bool IsEmpty() const { return groups.empty(); }
    };

    using CustomConditionEvaluator = std::function<bool(EntityID, const Condition&)>;

    /**
 * @brief Universal condition evaluator used by all gameplay systems
 *
 * Provides a data-driven condition evaluation engine. Conditions are grouped
 * with AND/OR logic and can be negated. Custom evaluators allow extension
 * without modifying the core system. World variables and flags provide
 * global state that conditions can query.
 */
    class ConditionSystem
    {
      public:
        static ConditionSystem& GetInstance();

        void Initialize();
        void Shutdown();

        /**
     * @brief Evaluate a full condition set for an entity
     * @param conditions The set of condition groups to evaluate
     * @param entity The entity to evaluate conditions against
     * @param world The ECS world for component lookups
     * @return true if all groups pass (AND between groups)
     */
        bool Evaluate(const ConditionSet& conditions, EntityID entity, World& world) const;

        /**
     * @brief Evaluate a single condition for an entity
     * @param condition The condition to evaluate
     * @param entity The entity to evaluate against
     * @param world The ECS world for component lookups
     * @return true if the condition passes (including negation)
     */
        bool EvaluateCondition(const Condition& condition, EntityID entity, World& world) const;

        /**
     * @brief Register a custom condition evaluator
     * @param id The custom condition identifier (matched against param1 string)
     * @param evaluator The callback that evaluates this custom condition
     */
        void RegisterCustomEvaluator(const std::string& id, CustomConditionEvaluator evaluator);

        /**
     * @brief Set a world variable (for VariableEquals/GreaterThan/LessThan conditions)
     */
        void SetVariable(const std::string& name, int64_t value);
        int64_t GetVariable(const std::string& name) const;

        /**
     * @brief Set a world flag (for FlagSet condition)
     */
        void SetFlag(const std::string& name, bool value);
        bool GetFlag(const std::string& name) const;

      private:
        ConditionSystem() = default;

        bool EvaluateGroup(const ConditionGroup& group, EntityID entity, World& world) const;
        bool EvaluateSingle(ConditionType type, const ConditionParam& p1, const ConditionParam& p2, EntityID entity,
                            World& world) const;

        std::unordered_map<std::string, CustomConditionEvaluator> m_customEvaluators;
        std::unordered_map<std::string, int64_t> m_variables;
        std::unordered_map<std::string, bool> m_flags;
    };

} // namespace Spark::Gameplay
