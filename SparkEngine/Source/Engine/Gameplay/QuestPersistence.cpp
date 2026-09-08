/**
 * @file QuestPersistence.cpp
 * @brief Capture, validate, and restore per-entity quest save state
 */

#include "QuestSystem.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace Spark::Gameplay
{

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

} // namespace Spark::Gameplay
