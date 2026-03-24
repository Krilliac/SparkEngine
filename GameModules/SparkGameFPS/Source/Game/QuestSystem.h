/**
 * @file QuestSystem.h
 * @brief Objective-based quest tracking system with progress and rewards
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides quest definitions, objective tracking, and a quest journal
 * as an ECS component. Supports kill, collect, reach, interact, and
 * survive objective types with automatic progress tracking.
 *
 * ## Usage
 * @code
 *   Spark::QuestRegistry quests;
 *   Spark::QuestDef quest;
 *   quest.id = 1;
 *   quest.name = "Clear the Arena";
 *   quest.objectives.push_back({Spark::ObjectiveType::Kill, "Defeat 5 enemies", 0, 5});
 *   quests.RegisterQuest(quest);
 *
 *   auto& journal = world.AddComponent<Spark::QuestJournalComponent>(entity);
 *   Spark::QuestOps::StartQuest(journal, quests, 1);
 *   Spark::QuestOps::UpdateObjective(journal, 1, 0, 1); // killed one enemy
 * @endcode
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <algorithm>
#include <cstdint>

#include "Engine/Events/EventSystem.h"

namespace Spark
{

    // =============================================================================
    // Quest Enums
    // =============================================================================

    /**
 * @brief Status of a quest in the player's journal
 */
    enum class QuestStatus
    {
        NotStarted, ///< Quest exists but not yet accepted
        Active,     ///< Quest is in progress
        Completed,  ///< All objectives met, rewards given
        Failed      ///< Quest failed (timer expired, NPC died, etc.)
    };

    /**
 * @brief Types of quest objectives
 */
    enum class ObjectiveType
    {
        Kill,     ///< Kill N enemies of a type
        Collect,  ///< Collect N items
        Reach,    ///< Reach a location (within radius)
        Interact, ///< Interact with a specific object/NPC
        Survive,  ///< Survive for N seconds
        Escort,   ///< Keep an NPC alive while moving
        Custom    ///< Scripted objective with manual completion
    };

    // =============================================================================
    // Quest Objective
    // =============================================================================

    /**
 * @brief A single objective within a quest
 */
    struct QuestObjective
    {
        ObjectiveType type = ObjectiveType::Kill;
        std::string description; ///< Human-readable objective text
        uint32_t targetId = 0;   ///< Target entity/item/location ID
        int requiredCount = 1;   ///< How many times to complete
        bool optional = false;   ///< Optional objectives don't block completion
    };

    /**
 * @brief Tracks runtime progress for a single objective
 */
    struct ObjectiveProgress
    {
        int currentCount = 0;   ///< Current progress toward requiredCount
        bool completed = false; ///< Whether this objective is done
    };

    // =============================================================================
    // Quest Reward
    // =============================================================================

    /**
 * @brief Reward given upon quest completion
 */
    struct QuestReward
    {
        int experiencePoints = 0;                    ///< XP reward
        int currency = 0;                            ///< Gold/currency reward
        std::vector<std::pair<uint32_t, int>> items; ///< (itemDefId, count) pairs
    };

    // =============================================================================
    // Quest Definition
    // =============================================================================

    /**
 * @brief Immutable definition of a quest (shared data)
 */
    struct QuestDef
    {
        uint32_t id = 0;         ///< Unique quest identifier
        std::string name;        ///< Display name
        std::string description; ///< Quest description text
        std::vector<QuestObjective> objectives;
        QuestReward reward;

        // Prerequisites
        std::vector<uint32_t> requiredQuests; ///< Quest IDs that must be completed first
        int requiredLevel = 0;                ///< Minimum level to accept

        // Metadata
        bool isMainQuest = false;  ///< Main story vs side quest
        bool isRepeatable = false; ///< Can be completed multiple times
        float timeLimit = 0.0f;    ///< Time limit in seconds (0 = no limit)
    };

    // =============================================================================
    // Active Quest (runtime state)
    // =============================================================================

    /**
 * @brief Runtime state for an active quest
 */
    struct ActiveQuest
    {
        uint32_t questId = 0;
        QuestStatus status = QuestStatus::Active;
        std::vector<ObjectiveProgress> objectiveProgress;
        float elapsedTime = 0.0f; ///< Time spent on this quest
    };

    // =============================================================================
    // Quest Journal Component (ECS)
    // =============================================================================

    /**
 * @brief ECS component tracking an entity's quest state
 *
 * Pure data struct. Use QuestOps for logic operations.
 */
    struct QuestJournalComponent
    {
        std::vector<ActiveQuest> activeQuests;
        std::unordered_set<uint32_t> completedQuestIds;
        std::unordered_set<uint32_t> failedQuestIds;
    };

    // =============================================================================
    // Quest Registry
    // =============================================================================

    /**
 * @class QuestRegistry
 * @brief Global registry of all quest definitions
 */
    class QuestRegistry
    {
      public:
        void RegisterQuest(const QuestDef& def) { m_quests[def.id] = def; }

        const QuestDef* GetQuest(uint32_t id) const
        {
            auto it = m_quests.find(id);
            return (it != m_quests.end()) ? &it->second : nullptr;
        }

        bool HasQuest(uint32_t id) const { return m_quests.count(id) > 0; }

        const std::unordered_map<uint32_t, QuestDef>& GetAllQuests() const { return m_quests; }

        size_t GetQuestCount() const { return m_quests.size(); }

      private:
        std::unordered_map<uint32_t, QuestDef> m_quests;
    };

    // =============================================================================
    // Quest Operations
    // =============================================================================

    /**
 * @brief Stateless operations for manipulating quest journal data
 */
    namespace QuestOps
    {

        /**
     * @brief Start a quest for an entity
     *
     * @return true if the quest was successfully started
     */
        inline bool StartQuest(QuestJournalComponent& journal, const QuestRegistry& registry, uint32_t questId)
        {
            const QuestDef* def = registry.GetQuest(questId);
            if (!def)
                return false;

            // Check if already active or completed (unless repeatable)
            if (!def->isRepeatable && journal.completedQuestIds.count(questId))
                return false;

            for (const auto& aq : journal.activeQuests)
            {
                if (aq.questId == questId)
                    return false; // already active
            }

            // Check prerequisites
            for (uint32_t reqId : def->requiredQuests)
            {
                if (!journal.completedQuestIds.count(reqId))
                    return false;
            }

            ActiveQuest aq;
            aq.questId = questId;
            aq.status = QuestStatus::Active;
            aq.objectiveProgress.resize(def->objectives.size());
            journal.activeQuests.push_back(std::move(aq));
            return true;
        }

        /**
     * @brief Update progress on a specific objective
     *
     * @param questId        Quest to update
     * @param objectiveIndex Index of the objective within the quest
     * @param increment      How much to increment progress
     * @param eventBus       Optional EventBus to publish QuestCompletedEvent
     * @param entityId       Entity ID for the event (only used if eventBus is set)
     * @return               true if the objective was updated
     */
        inline bool UpdateObjective(QuestJournalComponent& journal, const QuestRegistry& registry, uint32_t questId,
                                    int objectiveIndex, int increment = 1, EventBus* eventBus = nullptr,
                                    uint32_t entityId = 0)
        {
            const QuestDef* def = registry.GetQuest(questId);
            if (!def)
                return false;

            for (auto& aq : journal.activeQuests)
            {
                if (aq.questId == questId && aq.status == QuestStatus::Active)
                {
                    if (objectiveIndex < 0 || objectiveIndex >= static_cast<int>(aq.objectiveProgress.size()))
                        return false;

                    auto& prog = aq.objectiveProgress[objectiveIndex];
                    if (prog.completed)
                        return false;

                    prog.currentCount += increment;
                    if (prog.currentCount >= def->objectives[objectiveIndex].requiredCount)
                    {
                        prog.currentCount = def->objectives[objectiveIndex].requiredCount;
                        prog.completed = true;
                    }

                    // Check if all required objectives are complete
                    bool allDone = true;
                    for (int i = 0; i < static_cast<int>(def->objectives.size()); ++i)
                    {
                        if (!def->objectives[i].optional && !aq.objectiveProgress[i].completed)
                        {
                            allDone = false;
                            break;
                        }
                    }
                    if (allDone)
                    {
                        aq.status = QuestStatus::Completed;
                        journal.completedQuestIds.insert(questId);

                        // Publish quest completion event
                        if (eventBus)
                        {
                            QuestCompletedEvent evt;
                            evt.entityId = entityId;
                            evt.questId = questId;
                            evt.questName = def->name;
                            eventBus->Publish(evt);
                        }
                    }

                    return true;
                }
            }
            return false;
        }

        /**
     * @brief Fail a quest
     */
        inline bool FailQuest(QuestJournalComponent& journal, uint32_t questId)
        {
            for (auto& aq : journal.activeQuests)
            {
                if (aq.questId == questId && aq.status == QuestStatus::Active)
                {
                    aq.status = QuestStatus::Failed;
                    journal.failedQuestIds.insert(questId);
                    return true;
                }
            }
            return false;
        }

        /**
     * @brief Get the active quest state for a given quest ID
     * @return Pointer to the active quest, or nullptr if not active
     */
        inline const ActiveQuest* GetActiveQuest(const QuestJournalComponent& journal, uint32_t questId)
        {
            for (const auto& aq : journal.activeQuests)
            {
                if (aq.questId == questId)
                    return &aq;
            }
            return nullptr;
        }

        /**
     * @brief Check if a quest is completed
     */
        inline bool IsQuestCompleted(const QuestJournalComponent& journal, uint32_t questId)
        {
            return journal.completedQuestIds.count(questId) > 0;
        }

        /**
     * @brief Get the number of active quests
     */
        inline int GetActiveQuestCount(const QuestJournalComponent& journal)
        {
            int count = 0;
            for (const auto& aq : journal.activeQuests)
            {
                if (aq.status == QuestStatus::Active)
                    ++count;
            }
            return count;
        }

        /**
     * @brief Update elapsed time for all active quests and fail timed-out ones
     *
     * @param dt Delta time in seconds
     */
        inline void UpdateTimers(QuestJournalComponent& journal, const QuestRegistry& registry, float dt)
        {
            for (auto& aq : journal.activeQuests)
            {
                if (aq.status != QuestStatus::Active)
                    continue;
                aq.elapsedTime += dt;

                const QuestDef* def = registry.GetQuest(aq.questId);
                if (def && def->timeLimit > 0.0f && aq.elapsedTime >= def->timeLimit)
                {
                    aq.status = QuestStatus::Failed;
                    journal.failedQuestIds.insert(aq.questId);
                }
            }
        }

        /**
     * @brief Remove completed and failed quests from the active list
     */
        inline void CleanupFinished(QuestJournalComponent& journal)
        {
            journal.activeQuests.erase(
                std::remove_if(journal.activeQuests.begin(), journal.activeQuests.end(), [](const ActiveQuest& aq)
                               { return aq.status == QuestStatus::Completed || aq.status == QuestStatus::Failed; }),
                journal.activeQuests.end());
        }

    } // namespace QuestOps

} // namespace Spark
