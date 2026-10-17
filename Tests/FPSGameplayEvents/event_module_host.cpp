/** @file event_module_host.cpp
 * @brief Host-owned bus dispatch, production adapter publication and real DLL unload/reload.
 */
#include <Spark/EventTypes.h>

#include <filesystem>
#include <iostream>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{
    class ModuleImage
    {
      public:
        explicit ModuleImage(const std::filesystem::path& path)
        {
#if defined(_WIN32)
            m_handle = LoadLibraryW(path.c_str());
#else
            m_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        }

        ~ModuleImage() { Close(); }
        ModuleImage(const ModuleImage&) = delete;
        ModuleImage& operator=(const ModuleImage&) = delete;

        template <typename Function> Function Get(const char* name) const
        {
            if (!m_handle)
                return nullptr;
#if defined(_WIN32)
            return reinterpret_cast<Function>(GetProcAddress(m_handle, name));
#else
            return reinterpret_cast<Function>(dlsym(m_handle, name));
#endif
        }

        bool Close()
        {
            if (!m_handle)
                return true;
#if defined(_WIN32)
            const bool closed = FreeLibrary(m_handle) != 0;
#else
            const bool closed = dlclose(m_handle) == 0;
#endif
            if (closed)
                m_handle = nullptr;
            return closed;
        }

      private:
#if defined(_WIN32)
        HMODULE m_handle = nullptr;
#else
        void* m_handle = nullptr;
#endif
    };
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "Expected the event module image path\n";
        return 1;
    }
    int failures = 0;
    auto check = [&failures](bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << message << '\n';
            ++failures;
        }
    };

    Spark::EventBus hostBus;
    Spark::EventBus unrelatedBus;
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        ModuleImage image(std::filesystem::absolute(argv[1]));
        const auto load = image.Get<void* (*)(Spark::EventBus*, int*)>("FPSLoadEvents");
        const auto loadCount = image.Get<int (*)()>("FPSLoadCount");
        const auto boundBus = image.Get<Spark::EventBus* (*)(void*)>("FPSBoundEventBus");
        const auto runAdapters = image.Get<bool (*)(void*)>("FPSRunEventAdapters");
        const auto queueEvent = image.Get<void (*)(Spark::QueuedEventBus*)>("FPSQueueModuleEvent");
        const auto unload = image.Get<void (*)(void*)>("FPSUnloadEvents");
        if (!load || !loadCount || !boundBus || !runAdapters || !queueEvent || !unload)
        {
            std::cerr << "Failed to load the event module and its required exports\n";
            return 1;
        }

        int observedDamage = 0;
        void* instance = load(&hostBus, &observedDamage);
        if (!instance)
        {
            std::cerr << "Module failed to bind the host bus\n";
            return 1;
        }
        check(loadCount() == 1, "Reload must start with fresh module static state after actual image unload");
        check(boundBus(instance) == &hostBus, "Module must retain the injected host-owned bus identity");
        check(hostBus.SubscriberCount<Spark::EntityDamagedEvent>() == 1, "Host must observe the module subscription");
        hostBus.Publish(Spark::EntityDamagedEvent{42, 5.0f, "host"});
        unrelatedBus.Publish(Spark::EntityDamagedEvent{42, 90.0f, "other bus"});
        check(observedDamage == 5, "Only the injected host bus must synchronously dispatch to module code");

        Spark::QueuedEventBus queue;
        queue.QueueEvent(Spark::EntityDamagedEvent{42, 7.0f, "deferred host"});
        check(observedDamage == 5, "Host deferred publication must wait for DispatchAll");
        queue.DispatchAll(hostBus);
        check(observedDamage == 12, "Deferred host events must dispatch across the module boundary");

        std::vector<Spark::ItemPickedUpEvent> pickups;
        std::vector<Spark::QuestCompletedEvent> completions;
        auto pickup = hostBus.Subscribe<Spark::ItemPickedUpEvent>([&pickups](const Spark::ItemPickedUpEvent& event)
                                                                  { pickups.push_back(event); });
        auto quest = hostBus.Subscribe<Spark::QuestCompletedEvent>(
            [&completions](const Spark::QuestCompletedEvent& event) { completions.push_back(event); });
        check(runAdapters(instance), "Production FPS mutations must succeed inside the module image");
        check(pickups.size() == 1 && pickups[0].entityId == 42 && pickups[0].itemDefId == 4 && pickups[0].count == 3,
              "Production FPS inventory adapter must publish its exact payload back to the host");
        check(completions.size() == 1 && completions[0].entityId == 42 && completions[0].questId == 8 &&
                  completions[0].questName == "Public events arena",
              "Production FPS quest adapter must publish its string payload back to the host");
        queueEvent(&queue);
        check(queue.GetPendingCount() == 1 && completions.size() == 1,
              "Module callbacks must remain queued in the host");
        queue.DispatchAll(hostBus);
        check(completions.size() == 2 && completions.back().questId == 9 &&
                  completions.back().questName == "Deferred module event",
              "Host must dispatch and destroy module-created deferred callbacks while mapped");

        queueEvent(&queue);
        // The module owns its subscriptions. The host owns queues and channels;
        // their erased destructors may live in the image, even after unsubscribe.
        unload(instance);
        check(hostBus.SubscriberCount<Spark::EntityDamagedEvent>() == 0,
              "Module shutdown must unsubscribe before image unload");
        hostBus.Publish(Spark::EntityDamagedEvent{42, 100.0f, "after shutdown"});
        check(observedDamage == 12, "A shut-down module must receive no more host events");
        queue.Clear();
        pickup.Unsubscribe();
        quest.Unsubscribe();
        hostBus.ClearAll();
        check(image.Close(), "The operating system must successfully unload the module image");

        hostBus.Publish(Spark::EntityDamagedEvent{42, 100.0f, "after unload"});
        queue.DispatchAll(hostBus);
        check(observedDamage == 12 && queue.GetPendingCount() == 0,
              "Host bus and queue must remain usable without callbacks into the unloaded image");
    }
    return failures == 0 ? 0 : 1;
}
