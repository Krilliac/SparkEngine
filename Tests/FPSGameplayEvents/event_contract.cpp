/** @file event_contract.cpp
 * @brief Behavioral contracts for the production public event APIs, with no engine include path.
 */
#include <Spark/EventTypes.h>

#include <iostream>
#include <stdexcept>
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
    std::vector<uint32_t> received;
    auto subscription = bus.Subscribe<Spark::EntityCreatedEvent>([&received](const Spark::EntityCreatedEvent& event)
                                                                 { received.push_back(event.entityId); });
    Spark::QueuedEventBus queue;
    queue.QueueEvent(Spark::EntityCreatedEvent{1}, Spark::EventPriority::Low);
    queue.QueueEvent(Spark::EntityCreatedEvent{2});
    queue.QueueEvent(Spark::EntityCreatedEvent{3}, Spark::EventPriority::Critical);
    queue.QueueEvent(Spark::EntityCreatedEvent{4});
    check(received.empty() && queue.GetPendingCount() == 4, "Deferred events must wait for dispatch");
    queue.DispatchAll(bus);
    check(received == std::vector<uint32_t>({3, 2, 4, 1}), "Dispatch preserves priority and FIFO within a priority");
    check(queue.GetPendingCount() == 0, "Dispatch drains the pending queue");

    received.clear();
    queue.QueueEvent(Spark::EntityCreatedEvent{10001}, Spark::EventPriority::Low);
    for (uint32_t id = 0; id < 10000; ++id)
        queue.QueueEvent(Spark::EntityCreatedEvent{id}, Spark::EventPriority::Critical);
    check(queue.GetPendingCount() == 10000 && queue.GetPendingCount(Spark::EventPriority::Low) == 0 &&
              queue.GetDroppedEventCount() == 1,
          "Overflow evicts the oldest lowest-priority event at the production bound");
    queue.DispatchAll(bus);
    check(received.size() == 10000 && received.front() == 0 && received.back() == 9999 &&
              queue.GetDroppedEventCount() == 0,
          "Overflow retains ordered critical events and dispatch resets the dropped count");

    received.clear();
    auto enqueueDuringDispatch = bus.Subscribe<Spark::EntityCreatedEvent>(
        [&queue](const Spark::EntityCreatedEvent&) { queue.QueueEvent(Spark::EntityCreatedEvent{9}); });
    queue.QueueEvent(Spark::EntityCreatedEvent{8});
    queue.DispatchAll(bus);
    check(received == std::vector<uint32_t>({8}) && queue.GetPendingCount() == 1,
          "An event queued by a callback waits until the next dispatch");
    enqueueDuringDispatch.Unsubscribe();
    queue.Clear();
    check(queue.GetPendingCount() == 0, "Clear discards queued callbacks without dispatch");

    int afterThrow = 0;
    auto throwing = bus.Subscribe<Spark::FrameBeginEvent>([](const Spark::FrameBeginEvent&)
                                                          { throw std::runtime_error("expected callback failure"); });
    auto later = bus.Subscribe<Spark::FrameBeginEvent>([&afterThrow](const Spark::FrameBeginEvent&) { ++afterThrow; });
    bus.Publish(Spark::FrameBeginEvent{});
    bus.Publish(Spark::FrameBeginEvent{});
    check(afterThrow == 2, "Throwing subscribers cannot suppress later handlers or future publication");

    subscription.Unsubscribe();
    bus.Publish(Spark::EntityCreatedEvent{99});
    check(received == std::vector<uint32_t>({8}) && bus.SubscriberCount<Spark::EntityCreatedEvent>() == 0,
          "Explicit unsubscribe removes only the owned subscription");
    Spark::SubscriptionHandle survivor;
    {
        Spark::EventBus shortLivedBus;
        survivor = shortLivedBus.Subscribe<Spark::EngineStartEvent>([](const Spark::EngineStartEvent&) {});
        Spark::SubscriptionHandle moved = std::move(survivor);
        check(!survivor.IsActive() && moved.IsActive(), "Moving a handle transfers subscription ownership");
        survivor = std::move(moved);
    }
    survivor.Unsubscribe();
    check(!survivor.IsActive(), "A handle may be released after its bus is destroyed");
    return failures == 0 ? 0 : 1;
}
