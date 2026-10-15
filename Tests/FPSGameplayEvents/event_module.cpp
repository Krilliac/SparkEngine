/** @file event_module.cpp
 * @brief Dynamically loaded public-event fixture containing the production FPS mutation adapters.
 */
#include "Game/InventorySystem.h"
#include "Game/QuestSystem.h"
#include <Spark/EventTypes.h>

#include <memory>

#if defined(_WIN32)
#define FPS_EVENT_PROBE_API extern "C" __declspec(dllexport)
#else
#define FPS_EVENT_PROBE_API extern "C" __attribute__((visibility("default")))
#endif

namespace
{
    int s_loadCount = 0;

    struct EventModule
    {
        Spark::EventBus* bus = nullptr;
        Spark::SubscriptionHandle damageSubscription;
    };
} // namespace

FPS_EVENT_PROBE_API void* FPSLoadEvents(Spark::EventBus* bus, int* observedDamage)
{
    if (!bus || !observedDamage)
        return nullptr;
    ++s_loadCount;
    auto module = std::make_unique<EventModule>();
    module->bus = bus;
    module->damageSubscription =
        bus->Subscribe<Spark::EntityDamagedEvent>([observedDamage](const Spark::EntityDamagedEvent& event)
                                                  { *observedDamage += static_cast<int>(event.damage); });
    return module.release();
}

FPS_EVENT_PROBE_API int FPSLoadCount()
{
    return s_loadCount;
}

FPS_EVENT_PROBE_API Spark::EventBus* FPSBoundEventBus(void* instance)
{
    return static_cast<EventModule*>(instance)->bus;
}

FPS_EVENT_PROBE_API bool FPSRunEventAdapters(void* instance)
{
    auto& module = *static_cast<EventModule*>(instance);
    Spark::ItemRegistry items;
    Spark::ItemDef item;
    item.id = 4;
    item.maxStackSize = 10;
    items.RegisterItem(item);
    Spark::InventoryComponent inventory;
    if (Spark::InventoryOps::AddItem(inventory, items, 4, 3, module.bus, 42) != 3)
        return false;

    Spark::QuestRegistry quests;
    Spark::QuestDef quest;
    quest.id = 8;
    quest.name = "Public events arena";
    quest.objectives.push_back({Spark::ObjectiveType::Kill, "Kills", 0, 2, false});
    quests.RegisterQuest(quest);
    Spark::QuestJournalComponent journal;
    return Spark::QuestOps::StartQuest(journal, quests, 8) &&
           Spark::QuestOps::UpdateObjective(journal, quests, 8, 0, 2, module.bus, 42);
}

FPS_EVENT_PROBE_API void FPSQueueModuleEvent(Spark::QueuedEventBus* queue)
{
    queue->QueueEvent(Spark::QuestCompletedEvent{42, 9, "Deferred module event"});
}

FPS_EVENT_PROBE_API void FPSUnloadEvents(void* instance)
{
    // Destroy the module-owned callback and unsubscribe function while mapped.
    const std::unique_ptr<EventModule> module(static_cast<EventModule*>(instance));
}
