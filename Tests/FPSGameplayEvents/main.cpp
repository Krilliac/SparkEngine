/** @file main.cpp
 * @brief Exercise production FPS event adapters with the real engine EventBus.
 */
#include "Game/InventorySystem.h"
#include "Game/QuestSystem.h"
#include "Engine/Events/EventSystem.h"

#include <iostream>
#include <vector>

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
    Spark::EventBus bus;
    std::vector<Spark::ItemPickedUpEvent> pickups;
    std::vector<Spark::QuestCompletedEvent> completions;
    auto pickupSubscription = bus.Subscribe<Spark::ItemPickedUpEvent>([&pickups](const Spark::ItemPickedUpEvent& event)
                                                                      { pickups.push_back(event); });
    auto questSubscription = bus.Subscribe<Spark::QuestCompletedEvent>(
        [&completions](const Spark::QuestCompletedEvent& event) { completions.push_back(event); });

    Spark::ItemRegistry items;
    Spark::ItemDef item;
    item.id = 4;
    item.maxStackSize = 3;
    item.weight = 2.0f;
    items.RegisterItem(item);
    Spark::InventoryComponent inventory;
    inventory.maxWeight = 5.0f;
    check(Spark::InventoryOps::AddItem(inventory, items, 4, 8, &bus, 42) == 2,
          "Adapter retains weight-limited mutation");
    check(pickups.size() == 1 && pickups[0].entityId == 42 && pickups[0].itemDefId == 4 && pickups[0].count == 2,
          "Pickup publishes once with actual added count and requested entity");
    check(Spark::InventoryOps::AddItem(inventory, items, 4, 1, &bus) == 0 &&
              Spark::InventoryOps::AddItem(inventory, items, 999, 1, &bus) == 0 &&
              Spark::InventoryOps::AddItem(inventory, items, 4, 0, &bus) == 0 && pickups.size() == 1,
          "Rejected additions publish nothing");
    inventory.maxWeight = 20.0f;
    check(Spark::InventoryOps::AddItem(inventory, items, 4, 2, nullptr) == 2 && pickups.size() == 1,
          "Null bus retains mutation and suppresses pickup");
    check(Spark::InventoryOps::AddItem(inventory, items, 4, 1, &bus) == 1 && pickups.size() == 2 &&
              pickups.back().entityId == 0 && Spark::InventoryOps::CountItem(inventory, 4) == 5,
          "Default entity remains zero and adapter does not mutate twice");

    Spark::QuestRegistry quests;
    Spark::QuestDef quest;
    quest.id = 8;
    quest.name = "Arena";
    quest.isRepeatable = true;
    quest.objectives.push_back({Spark::ObjectiveType::Kill, "Kills", 0, 2, false});
    quest.objectives.push_back({Spark::ObjectiveType::Collect, "Bonus", 0, 1, true});
    quests.RegisterQuest(quest);
    Spark::QuestJournalComponent journal;
    check(Spark::QuestOps::StartQuest(journal, quests, 8), "Start adapter quest");
    check(Spark::QuestOps::UpdateObjective(journal, quests, 8, 0, 1, &bus, 42) && completions.empty(),
          "Partial progress publishes nothing");
    check(Spark::QuestOps::UpdateObjective(journal, quests, 8, 0, 1, &bus, 42) && completions.size() == 1 &&
              completions[0].entityId == 42 && completions[0].questId == 8 && completions[0].questName == "Arena",
          "Required completion publishes once with exact payload despite unfinished optional objective");
    check(!Spark::QuestOps::UpdateObjective(journal, quests, 8, 0, 1, &bus, 42) &&
              !Spark::QuestOps::UpdateObjective(journal, quests, 999, 0, 1, &bus) && completions.size() == 1,
          "Repeated or unknown updates do not republish completion");
    Spark::QuestOps::CleanupFinished(journal);
    check(Spark::QuestOps::StartQuest(journal, quests, 8), "Restart repeatable quest");
    check(!Spark::QuestOps::UpdateObjective(journal, quests, 8, -1, 1, &bus) && completions.size() == 1,
          "Invalid objective does not publish");
    check(Spark::QuestOps::UpdateObjective(journal, quests, 8, 0, 1, &bus) && completions.size() == 1,
          "Historical completion must not publish during new partial progress");
    check(Spark::QuestOps::UpdateObjective(journal, quests, 8, 0, 1, &bus) && completions.size() == 2 &&
              completions.back().entityId == 0,
          "Repeatable quest publishes exactly once per completed run");
    Spark::QuestOps::CleanupFinished(journal);
    check(Spark::QuestOps::StartQuest(journal, quests, 8), "Restart for null-bus completion");
    check(Spark::QuestOps::UpdateObjective(journal, quests, 8, 0, 2, nullptr) && completions.size() == 2 &&
              journal.activeQuests[0].status == Spark::QuestStatus::Completed,
          "Null bus preserves quest completion without publication");
    // A restored POD journal may retain a finished entry before another active run.
    Spark::ActiveQuest resumed;
    resumed.questId = 8;
    resumed.objectiveProgress.resize(quest.objectives.size());
    journal.activeQuests.push_back(resumed);
    check(Spark::QuestOps::UpdateObjective(journal, quests, 8, 0, 1, &bus) && completions.size() == 2,
          "Earlier finished entry must not cause publication for an active run's partial progress");
    check(Spark::QuestOps::UpdateObjective(journal, quests, 8, 0, 1, &bus) && completions.size() == 3,
          "Completion follows the actual updated entry when the journal retains an earlier run");
    return failures == 0 ? 0 : 1;
}
