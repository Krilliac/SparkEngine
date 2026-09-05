/**
 * @file QuestSystem.cpp
 * @brief Implementation of the per-entity quest tracking system
 */

#include "QuestSystem.h"
#include "InventorySystem.h"
#include "../../Utils/LogMacros.h"
#include "../../Utils/SparkConsole.h"
#include "../Events/EventSystem.h"

#include <algorithm>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Spark::Gameplay
{

    // ============================================================================
    // Singleton
    // ============================================================================

    QuestSystem& QuestSystem::GetInstance()
    {
        static QuestSystem instance;
        return instance;
    }

    // ============================================================================
    // Lifecycle
    // ============================================================================

    void QuestSystem::Initialize()
    {
        m_questDefs.clear();
        m_entityQuests.clear();

        Spark::SimpleConsole::GetInstance().LogInfo("[QuestSystem] Initialized");
        SPARK_LOG_INFO(Spark::LogCategory::Game, "QuestSystem initialized");
    }

    void QuestSystem::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Game, "QuestSystem shutting down (%zu quest defs, %zu tracked entities)",
                       m_questDefs.size(), m_entityQuests.size());

        m_questDefs.clear();
        m_entityQuests.clear();
    }

    // ============================================================================
    // Quest Registry
    // ============================================================================

    void QuestSystem::RegisterQuest(const QuestDefinition& def)
    {
        if (def.questId == 0)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "QuestSystem: Cannot register quest with ID 0");
            return;
        }

        m_questDefs[def.questId] = def;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "QuestSystem: Registered quest '%s' (id=%u, %zu objectives)",
                       def.name.c_str(), def.questId, def.objectives.size());
    }

    const QuestDefinition* QuestSystem::GetQuestDef(uint32_t questId) const
    {
        auto it = m_questDefs.find(questId);
        return (it != m_questDefs.end()) ? &it->second : nullptr;
    }

    // ============================================================================
    // Per-Entity Quest Tracking
    // ============================================================================

    bool QuestSystem::StartQuest(uint32_t entityId, uint32_t questId)
    {
        const QuestDefinition* def = GetQuestDef(questId);
        if (!def)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "QuestSystem: Unknown quest ID %u", questId);
            return false;
        }

        // Check prerequisite
        if (def->prerequisiteQuestId != 0)
        {
            QuestState prereqState = GetQuestState(entityId, def->prerequisiteQuestId);
            if (prereqState != QuestState::Completed)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Game,
                               "QuestSystem: Entity %u cannot start quest %u — prerequisite %u not completed", entityId,
                               questId, def->prerequisiteQuestId);
                return false;
            }
        }

        if (m_policy && !m_policy->CanStartQuest(entityId, *def))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "QuestSystem: Entity %u blocked by quest policy when starting quest %u", entityId, questId);
            return false;
        }

        auto& entityQuests = m_entityQuests[entityId];

        // Check if already started or completed
        auto it = entityQuests.find(questId);
        if (it != entityQuests.end())
        {
            if (it->second.state == QuestState::Active)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Game, "QuestSystem: Entity %u already has quest %u active", entityId,
                               questId);
                return false;
            }
            if (it->second.state == QuestState::Completed)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Game, "QuestSystem: Entity %u already completed quest %u", entityId,
                               questId);
                return false;
            }
        }

        // Create active quest data with a copy of objectives (zeroed progress)
        ActiveQuestData questData;
        questData.questId = questId;
        questData.state = QuestState::Active;
        questData.objectives = def->objectives;
        for (auto& obj : questData.objectives)
        {
            obj.currentCount = 0;
        }

        entityQuests[questId] = std::move(questData);
        if (m_policy)
        {
            m_policy->OnQuestStarted(entityId, *def);
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "QuestSystem: Entity %u started quest '%s' (id=%u)", entityId,
                       def->name.c_str(), questId);
        return true;
    }

    bool QuestSystem::AbandonQuest(uint32_t entityId, uint32_t questId)
    {
        auto entityIt = m_entityQuests.find(entityId);
        if (entityIt == m_entityQuests.end())
        {
            return false;
        }

        auto questIt = entityIt->second.find(questId);
        if (questIt == entityIt->second.end() || questIt->second.state != QuestState::Active)
        {
            return false;
        }

        questIt->second.state = QuestState::Failed;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "QuestSystem: Entity %u abandoned quest %u", entityId, questId);
        return true;
    }

    QuestState QuestSystem::GetQuestState(uint32_t entityId, uint32_t questId) const
    {
        auto entityIt = m_entityQuests.find(entityId);
        if (entityIt == m_entityQuests.end())
        {
            return QuestState::NotStarted;
        }

        auto questIt = entityIt->second.find(questId);
        if (questIt == entityIt->second.end())
        {
            return QuestState::NotStarted;
        }

        return questIt->second.state;
    }

    // ============================================================================
    // Progress
    // ============================================================================

    void QuestSystem::ReportProgress(uint32_t entityId, QuestObjective::Type type, uint32_t targetId, uint32_t count)
    {
        auto entityIt = m_entityQuests.find(entityId);
        if (entityIt == m_entityQuests.end())
        {
            return;
        }

        for (auto& [questId, questData] : entityIt->second)
        {
            if (questData.state != QuestState::Active)
            {
                continue;
            }

            bool anyUpdated = false;
            for (auto& obj : questData.objectives)
            {
                if (obj.type == type && obj.targetId == targetId && !obj.IsComplete())
                {
                    uint32_t oldCount = obj.currentCount;
                    obj.currentCount = std::min(obj.currentCount + count, obj.requiredCount);

                    if (obj.currentCount != oldCount)
                    {
                        anyUpdated = true;
                        if (m_policy)
                        {
                            m_policy->OnObjectiveProgress(entityId, questId, obj);
                        }
                        SPARK_LOG_INFO(Spark::LogCategory::Game,
                                       "QuestSystem: Entity %u quest %u objective progress: %u/%u", entityId, questId,
                                       obj.currentCount, obj.requiredCount);
                    }
                }
            }

            // Auto-check completion when objectives update
            if (anyUpdated)
            {
                bool allComplete = true;
                for (const auto& obj : questData.objectives)
                {
                    if (!obj.IsComplete())
                    {
                        allComplete = false;
                        break;
                    }
                }

                if (allComplete)
                {
                    SPARK_LOG_INFO(Spark::LogCategory::Game,
                                   "QuestSystem: All objectives complete for entity %u quest %u — ready for turn-in",
                                   entityId, questId);
                }
            }
        }
    }

    bool QuestSystem::IsQuestComplete(uint32_t entityId, uint32_t questId) const
    {
        auto entityIt = m_entityQuests.find(entityId);
        if (entityIt == m_entityQuests.end())
        {
            return false;
        }

        auto questIt = entityIt->second.find(questId);
        if (questIt == entityIt->second.end() || questIt->second.state != QuestState::Active)
        {
            return false;
        }

        for (const auto& obj : questIt->second.objectives)
        {
            if (!obj.IsComplete())
            {
                return false;
            }
        }

        return true;
    }

    bool QuestSystem::CompleteQuest(uint32_t entityId, uint32_t questId)
    {
        auto entityIt = m_entityQuests.find(entityId);
        if (entityIt == m_entityQuests.end())
        {
            return false;
        }

        auto questIt = entityIt->second.find(questId);
        if (questIt == entityIt->second.end() || questIt->second.state != QuestState::Active)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "QuestSystem: Cannot complete quest %u for entity %u — not active",
                           questId, entityId);
            return false;
        }

        const QuestDefinition* def = GetQuestDef(questId);

        // Deliver the declared item rewards BEFORE the completion is committed. A quest
        // can only be completed once, so a reward granted after the state flip is
        // destroyed outright when the inventory is full — the quest can never be
        // re-completed to collect it. Paying first lets a refused delivery leave the
        // quest Active and retryable once the player frees a slot.
        // XP stays policy-owned: the engine has no XP sink of its own.
        if (def && !m_policy && !def->itemRewards.empty())
        {
            auto& inventory = InventorySystem::GetInstance();
            std::vector<std::pair<uint32_t, uint32_t>> granted;
            granted.reserve(def->itemRewards.size());

            for (const auto& [itemId, count] : def->itemRewards)
            {
                if (inventory.AddItem(entityId, itemId, count))
                {
                    granted.emplace_back(itemId, count);
                    continue;
                }

                // Undo the partial delivery so a refused completion never leaves a
                // half-paid reward behind, then report the refusal to the caller.
                for (const auto& [grantedItem, grantedCount] : granted)
                {
                    inventory.RemoveItem(entityId, grantedItem, grantedCount);
                }

                SPARK_LOG_WARN(Spark::LogCategory::Game,
                               "QuestSystem: Entity %u cannot receive reward item %u x%u from quest %u — quest stays "
                               "active so the reward is not lost",
                               entityId, itemId, count, questId);
                return false;
            }
        }

        questIt->second.state = QuestState::Completed;

        if (def)
        {
            if (m_policy)
            {
                m_policy->OnQuestCompleted(entityId, *def);
            }

            SPARK_LOG_INFO(Spark::LogCategory::Game,
                           "QuestSystem: Entity %u completed quest '%s' (id=%u, xp=%u, %zu item rewards)", entityId,
                           def->name.c_str(), questId, def->xpReward, def->itemRewards.size());

            // Publish after the completion is committed so subscribers (for example the
            // editor's OnQuestComplete event response trigger) observe the final state.
            Spark::EventBus::Global().Publish<Spark::QuestCompletedEvent>({entityId, questId, def->name});
        }

        return true;
    }

    // ============================================================================
    // Queries
    // ============================================================================

    std::vector<uint32_t> QuestSystem::GetActiveQuests(uint32_t entityId) const
    {
        std::vector<uint32_t> result;
        auto entityIt = m_entityQuests.find(entityId);
        if (entityIt == m_entityQuests.end())
        {
            return result;
        }

        for (const auto& [questId, questData] : entityIt->second)
        {
            if (questData.state == QuestState::Active)
            {
                result.push_back(questId);
            }
        }

        return result;
    }

    std::vector<uint32_t> QuestSystem::GetCompletedQuests(uint32_t entityId) const
    {
        std::vector<uint32_t> result;
        auto entityIt = m_entityQuests.find(entityId);
        if (entityIt == m_entityQuests.end())
        {
            return result;
        }

        for (const auto& [questId, questData] : entityIt->second)
        {
            if (questData.state == QuestState::Completed)
            {
                result.push_back(questId);
            }
        }

        return result;
    }

    std::vector<QuestProgressSnapshot> QuestSystem::CaptureEntityState(uint32_t entityId) const
    {
        std::vector<QuestProgressSnapshot> result;
        const auto entityIt = m_entityQuests.find(entityId);
        if (entityIt == m_entityQuests.end())
            return result;

        result.reserve(entityIt->second.size());
        for (const auto& [questId, questData] : entityIt->second)
        {
            QuestProgressSnapshot snapshot;
            snapshot.questId = questId;
            snapshot.state = questData.state;
            snapshot.objectiveCounts.reserve(questData.objectives.size());
            for (const QuestObjective& objective : questData.objectives)
                snapshot.objectiveCounts.push_back(objective.currentCount);
            result.push_back(std::move(snapshot));
        }
        std::sort(result.begin(), result.end(), [](const QuestProgressSnapshot& lhs, const QuestProgressSnapshot& rhs)
                  { return lhs.questId < rhs.questId; });
        return result;
    }

    bool QuestSystem::ValidateEntityState(const std::vector<QuestProgressSnapshot>& snapshot) const
    {
        std::unordered_set<uint32_t> questIds;
        questIds.reserve(snapshot.size());
        for (const QuestProgressSnapshot& savedQuest : snapshot)
        {
            const QuestDefinition* definition = GetQuestDef(savedQuest.questId);
            if (!definition || savedQuest.questId == 0 || savedQuest.state == QuestState::NotStarted ||
                savedQuest.state > QuestState::Failed ||
                savedQuest.objectiveCounts.size() != definition->objectives.size() ||
                !questIds.insert(savedQuest.questId).second)
                return false;

            bool allObjectivesComplete = true;
            for (size_t index = 0; index < definition->objectives.size(); ++index)
            {
                const uint32_t requiredCount = definition->objectives[index].requiredCount;
                if (savedQuest.objectiveCounts[index] > requiredCount)
                    return false;
                allObjectivesComplete &= savedQuest.objectiveCounts[index] >= requiredCount;
            }

            if (savedQuest.state == QuestState::Completed && !allObjectivesComplete)
                return false;
        }
        return true;
    }

    bool QuestSystem::RestoreEntityState(uint32_t entityId, const std::vector<QuestProgressSnapshot>& snapshot)
    {
        if (!ValidateEntityState(snapshot))
            return false;

        std::unordered_map<uint32_t, ActiveQuestData> restored;
        restored.reserve(snapshot.size());
        for (const QuestProgressSnapshot& savedQuest : snapshot)
        {
            const QuestDefinition* definition = GetQuestDef(savedQuest.questId);
            ActiveQuestData questData;
            questData.questId = savedQuest.questId;
            questData.state = savedQuest.state;
            questData.objectives = definition->objectives;
            for (size_t index = 0; index < questData.objectives.size(); ++index)
                questData.objectives[index].currentCount = savedQuest.objectiveCounts[index];
            restored.emplace(savedQuest.questId, std::move(questData));
        }

        if (restored.empty())
            m_entityQuests.erase(entityId);
        else
            m_entityQuests[entityId] = std::move(restored);
        return true;
    }

    void QuestSystem::ClearEntityState(uint32_t entityId)
    {
        m_entityQuests.erase(entityId);
    }

    // ============================================================================
    // Console
    // ============================================================================

    std::string QuestSystem::Console_GetStatus() const
    {
        std::string s = "Quest System:\n";
        s += "  Registered quests: " + std::to_string(m_questDefs.size()) + "\n";
        s += "  Tracked entities: " + std::to_string(m_entityQuests.size()) + "\n";

        for (const auto& [entityId, quests] : m_entityQuests)
        {
            uint32_t active = 0;
            uint32_t completed = 0;
            for (const auto& [questId, questData] : quests)
            {
                if (questData.state == QuestState::Active)
                {
                    ++active;
                }
                else if (questData.state == QuestState::Completed)
                {
                    ++completed;
                }
            }
            s += "  Entity " + std::to_string(entityId) + ": " + std::to_string(active) + " active, " +
                 std::to_string(completed) + " completed\n";
        }

        return s;
    }

    void QuestSystem::SetPolicy(IQuestPolicy* policy)
    {
        m_policy = policy;
    }

} // namespace Spark::Gameplay
