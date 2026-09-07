/**
 * @file FPSGameplayEvents.cpp
 * @brief Engine event adapters for the SDK-independent FPS inventory and quest mutations.
 */

#include "InventorySystem.h"
#include "QuestSystem.h"
#include "Engine/Events/EventSystem.h"

namespace Spark
{
    int InventoryOps::AddItem(InventoryComponent& inv, const ItemRegistry& registry, uint32_t itemId, int count,
                              EventBus* eventBus, uint32_t entityId)
    {
        const int added = AddItem(inv, registry, itemId, count);
        if (added > 0 && eventBus)
        {
            ItemPickedUpEvent event;
            event.entityId = entityId;
            event.itemDefId = itemId;
            event.count = added;
            eventBus->Publish(event);
        }
        return added;
    }

    bool QuestOps::UpdateObjective(QuestJournalComponent& journal, const QuestRegistry& registry, uint32_t questId,
                                   int objectiveIndex, int increment, EventBus* eventBus, uint32_t entityId)
    {
        // Select the same active entry as the mutation, even if a restored journal
        // retains an earlier finished entry with this ID. Mutation does not resize this vector.
        const auto quest =
            std::find_if(journal.activeQuests.begin(), journal.activeQuests.end(), [questId](const ActiveQuest& entry)
                         { return entry.questId == questId && entry.status == QuestStatus::Active; });
        const bool updated = UpdateObjective(journal, registry, questId, objectiveIndex, increment);
        // The completed-ID history also contains earlier runs of repeatable quests.
        // Only the current entry's transition may publish this run's completion.
        if (updated && quest != journal.activeQuests.end() && quest->status == QuestStatus::Completed && eventBus)
        {
            QuestCompletedEvent event;
            event.entityId = entityId;
            event.questId = questId;
            event.questName = registry.GetQuest(questId)->name;
            eventBus->Publish(event);
        }
        return updated;
    }
} // namespace Spark
