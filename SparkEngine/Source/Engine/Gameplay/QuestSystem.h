/**
 * @file QuestSystem.h
 * @brief Per-entity quest tracking with objectives, prerequisites, and rewards
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a reusable quest system for any game genre:
 * - Centralized quest definition registry (QuestDefinition)
 * - Per-entity quest state tracking (active, completed, failed)
 * - Objective progress reporting by type (kill, collect, talk, etc.)
 * - Prerequisite quest chains
 * - Item and XP rewards on completion
 * - Console integration for debugging
 *
 * ## Usage
 * @code
 *   auto& qs = QuestSystem::GetInstance();
 *   qs.Initialize();
 *
 *   QuestDefinition quest;
 *   quest.questId = 1;
 *   quest.name = "Rat Problem";
 *   quest.objectives.push_back({"Kill 5 rats", QuestObjective::Type::Kill, 100, 5});
 *   quest.xpReward = 50;
 *   qs.RegisterQuest(quest);
 *
 *   qs.StartQuest(playerEntity, 1);
 *   qs.ReportProgress(playerEntity, QuestObjective::Type::Kill, 100, 1);
 * @endcode
 */

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Spark::Gameplay
{

    // ============================================================================
    // Quest Objective
    // ============================================================================

    struct QuestObjective
    {
        std::string description;

        enum class Type : uint8_t
        {
            Kill,
            Collect,
            Talk,
            Reach,
            Interact,
            Custom
        };

        Type type = Type::Custom;
        uint32_t targetId = 0;      ///< Entity, item, or location ID to track
        uint32_t requiredCount = 1; ///< How many are needed to complete this objective
        uint32_t currentCount = 0;  ///< Current progress (runtime, per-entity copy)

        /// @brief Returns true if currentCount >= requiredCount
        [[nodiscard]] bool IsComplete() const { return currentCount >= requiredCount; }
    };

    // ============================================================================
    // Quest Definition (static registry data)
    // ============================================================================

    struct QuestDefinition
    {
        uint32_t questId = 0;
        std::string name;
        std::string description;
        std::vector<QuestObjective> objectives;

        uint32_t requiredLevel = 1;
        uint32_t xpReward = 0;
        std::vector<std::pair<uint32_t, uint32_t>> itemRewards; ///< (itemId, count) pairs

        uint32_t prerequisiteQuestId = 0; ///< 0 = no prerequisite
    };

    // ============================================================================
    // Quest State
    // ============================================================================

    enum class QuestState : uint8_t
    {
        NotStarted,
        Active,
        Completed,
        Failed
    };

    /** @brief Serializable per-quest runtime state for one entity. */
    struct QuestProgressSnapshot
    {
        uint32_t questId = 0;
        QuestState state = QuestState::NotStarted;
        std::vector<uint32_t> objectiveCounts;
    };

    // ============================================================================
    // Quest System
    // ============================================================================

    /**
     * @brief Manages quest definitions and per-entity quest progress tracking.
     *
     * Singleton. Call Initialize() at engine startup, Shutdown() on teardown.
     * Quests are registered globally; progress is tracked per entity ID.
     */
    class QuestSystem
    {
      public:
        /**
         * @brief Strategy interface for module-specific quest policy decisions.
         *
         * Game modules can install a policy object to enforce custom rules
         * (minimum level, faction gating, reputation checks, side effects) while
         * still using the engine's shared quest tracking implementation.
         */
        class IQuestPolicy
        {
          public:
            virtual ~IQuestPolicy() = default;

            /// @brief Whether this entity is allowed to start this quest.
            [[nodiscard]] virtual bool CanStartQuest(uint32_t entityId, const QuestDefinition& questDef) const = 0;

            /// @brief Invoked after quest start has been accepted and persisted.
            virtual void OnQuestStarted(uint32_t entityId, const QuestDefinition& questDef) = 0;

            /// @brief Invoked when objective progress is applied.
            virtual void OnObjectiveProgress(uint32_t entityId, uint32_t questId, const QuestObjective& objective) = 0;

            /// @brief Invoked after quest completion is committed.
            virtual void OnQuestCompleted(uint32_t entityId, const QuestDefinition& questDef) = 0;
        };

        static QuestSystem& GetInstance();

        /// @brief Initialize the quest system, clearing all state
        void Initialize();

        /// @brief Shut down the quest system
        void Shutdown();

        // -- Quest Registry --

        /// @brief Register a new quest definition
        void RegisterQuest(const QuestDefinition& def);

        /// @brief Look up a quest definition by ID
        /// @return Pointer to the definition, or nullptr if not found
        [[nodiscard]] const QuestDefinition* GetQuestDef(uint32_t questId) const;

        // -- Per-Entity Quest Tracking --

        /// @brief Start a quest for an entity (checks prerequisites)
        /// @return true if the quest was started
        bool StartQuest(uint32_t entityId, uint32_t questId);

        /// @brief Abandon an active quest
        /// @return true if the quest was active and is now abandoned
        bool AbandonQuest(uint32_t entityId, uint32_t questId);

        /// @brief Get the current state of a quest for an entity
        [[nodiscard]] QuestState GetQuestState(uint32_t entityId, uint32_t questId) const;

        // -- Progress --

        /// @brief Report objective progress matching a type and target
        /// @param entityId The entity making progress
        /// @param type The objective type (Kill, Collect, etc.)
        /// @param targetId The target being progressed (entity/item/location ID)
        /// @param count How much progress to add
        void ReportProgress(uint32_t entityId, QuestObjective::Type type, uint32_t targetId, uint32_t count = 1);

        /// @brief Check if all objectives for a quest are complete
        [[nodiscard]] bool IsQuestComplete(uint32_t entityId, uint32_t questId) const;

        /// @brief Mark a quest as completed (call after verifying IsQuestComplete)
        void CompleteQuest(uint32_t entityId, uint32_t questId);

        // -- Queries --

        /// @brief Get all active quest IDs for an entity
        [[nodiscard]] std::vector<uint32_t> GetActiveQuests(uint32_t entityId) const;

        /// @brief Get all completed quest IDs for an entity
        [[nodiscard]] std::vector<uint32_t> GetCompletedQuests(uint32_t entityId) const;

        /// @brief Capture all quest state for one entity without invoking policy callbacks.
        [[nodiscard]] std::vector<QuestProgressSnapshot> CaptureEntityState(uint32_t entityId) const;

        /// @brief Validate a snapshot against registered definitions without changing live state.
        [[nodiscard]] bool ValidateEntityState(const std::vector<QuestProgressSnapshot>& snapshot) const;

        /// @brief Atomically replace one entity's quest state after validating registered definitions.
        bool RestoreEntityState(uint32_t entityId, const std::vector<QuestProgressSnapshot>& snapshot);

        /// @brief Remove all quest state owned by an entity that is being destroyed or replaced.
        void ClearEntityState(uint32_t entityId);

        // -- Console --

        /// @brief Get a human-readable status string for debugging
        [[nodiscard]] std::string Console_GetStatus() const;

        /// @brief Install (or clear) a module-provided quest policy strategy.
        /// @param policy Non-owning pointer. Caller must keep object alive while installed.
        void SetPolicy(IQuestPolicy* policy);

        /// @brief Current installed policy, if any.
        [[nodiscard]] IQuestPolicy* GetPolicy() const { return m_policy; }

      private:
        QuestSystem() = default;

        /// @brief Per-entity quest instance with mutable objective progress
        struct ActiveQuestData
        {
            uint32_t questId = 0;
            QuestState state = QuestState::NotStarted;
            std::vector<QuestObjective> objectives; ///< Copy with runtime currentCount
        };

        // Quest definition registry
        std::unordered_map<uint32_t, QuestDefinition> m_questDefs;

        // Per-entity quest state: entityId -> (questId -> ActiveQuestData)
        std::unordered_map<uint32_t, std::unordered_map<uint32_t, ActiveQuestData>> m_entityQuests;

        // Optional non-owning policy strategy for module specialization.
        IQuestPolicy* m_policy = nullptr;
    };

} // namespace Spark::Gameplay
