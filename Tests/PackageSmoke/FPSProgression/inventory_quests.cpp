/** @file inventory_quests.cpp
 * @brief Exercise production FPS inventory and quest mutations without private engine headers.
 */
#include "Game/InventorySystem.h"
#include "Game/QuestSystem.h"

#include <iostream>

int main()
{
    int failures = 0;
    auto check = [&failures](bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            ++failures;
        }
    };

    Spark::ItemRegistry items;
    Spark::ItemDef item;
    item.id = 1;
    item.maxStackSize = 3;
    item.weight = 2.0f;
    items.RegisterItem(item);
    Spark::InventoryComponent inventory;
    inventory.maxSlots = 2;
    inventory.maxWeight = 20.0f;
    check(Spark::InventoryOps::AddItem(inventory, items, 1, 2) == 2, "Initial item addition");
    check(Spark::InventoryOps::AddItem(inventory, items, 1, 8) == 4 && inventory.slots.size() == 2 &&
              inventory.slots[0].count == 3 && inventory.slots[1].count == 3,
          "Fill existing stack before allocating slots, limited by slot capacity");
    check(Spark::InventoryOps::GetTotalWeight(inventory, items) == 12.0f, "Total inventory weight");
    check(Spark::InventoryOps::AddItem(inventory, items, 1) == 0, "Full inventory rejects addition");
    check(Spark::InventoryOps::RemoveItem(inventory, 1, 4) == 4 && inventory.slots.size() == 1 &&
              Spark::InventoryOps::CountItem(inventory, 1) == 2,
          "Removal consumes stacks and erases empty slots");
    inventory.maxWeight = 7.0f;
    check(Spark::InventoryOps::AddItem(inventory, items, 1, 3) == 1 &&
              Spark::InventoryOps::GetTotalWeight(inventory, items) == 6.0f,
          "Weight capacity truncates addition to whole items");
    check(Spark::InventoryOps::AddItem(inventory, items, 999) == 0 &&
              Spark::InventoryOps::AddItem(inventory, items, 1, 0) == 0 &&
              Spark::InventoryOps::AddItem(inventory, items, 1, -1) == 0,
          "Unknown items and nonpositive additions do not mutate inventory");

    Spark::QuestRegistry quests;
    Spark::QuestDef quest;
    quest.id = 1;
    quest.name = "Arena";
    quest.isRepeatable = true;
    quest.objectives.push_back({Spark::ObjectiveType::Kill, "Required kills", 0, 2, false});
    quest.objectives.push_back({Spark::ObjectiveType::Collect, "Optional loot", 0, 3, true});
    quests.RegisterQuest(quest);
    Spark::QuestJournalComponent journal;
    check(Spark::QuestOps::StartQuest(journal, quests, 1), "Start quest");
    check(!Spark::QuestOps::StartQuest(journal, quests, 1), "Duplicate active quest rejected");
    check(!Spark::QuestOps::UpdateObjective(journal, quests, 1, -1) &&
              !Spark::QuestOps::UpdateObjective(journal, quests, 1, 2) &&
              !Spark::QuestOps::UpdateObjective(journal, quests, 999, 0),
          "Invalid objectives and unknown quests rejected");
    check(Spark::QuestOps::UpdateObjective(journal, quests, 1, 0) && !Spark::QuestOps::IsQuestCompleted(journal, 1),
          "Partial objective progress stays active");
    check(Spark::QuestOps::UpdateObjective(journal, quests, 1, 0, 10) &&
              Spark::QuestOps::IsQuestCompleted(journal, 1) &&
              journal.activeQuests[0].objectiveProgress[0].currentCount == 2 &&
              !journal.activeQuests[0].objectiveProgress[1].completed,
          "Completion clamps progress and does not require optional objectives");
    check(!Spark::QuestOps::UpdateObjective(journal, quests, 1, 0), "Completed quest rejects repeated progress");
    check(!Spark::QuestOps::StartQuest(journal, quests, 1), "Finished entry blocks restart until cleanup");
    Spark::QuestOps::CleanupFinished(journal);
    check(Spark::QuestOps::StartQuest(journal, quests, 1), "Repeatable quest restarts after cleanup");
    check(Spark::QuestOps::UpdateObjective(journal, quests, 1, 0) &&
              journal.activeQuests[0].status == Spark::QuestStatus::Active,
          "Repeat progress stays active despite historical completed ID");
    check(Spark::QuestOps::UpdateObjective(journal, quests, 1, 0) &&
              journal.activeQuests[0].status == Spark::QuestStatus::Completed,
          "Repeatable quest completes again");
    Spark::QuestOps::CleanupFinished(journal);
    quest.isRepeatable = false;
    quests.RegisterQuest(quest);
    check(!Spark::QuestOps::StartQuest(journal, quests, 1), "Nonrepeatable completed quest cannot restart");
    return failures == 0 ? 0 : 1;
}
