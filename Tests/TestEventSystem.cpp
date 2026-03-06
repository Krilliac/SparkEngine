// TestEventSystem.cpp - Tests for type-safe publish/subscribe event bus
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <functional>
#include <unordered_map>
#include <vector>
#include <typeindex>
#include <mutex>
#include <cstdint>
#include <algorithm>
#include <string>

namespace TestEvents {

using SubscriptionID = uint64_t;

class EventBus {
public:
    template<typename T>
    SubscriptionID Subscribe(std::function<void(const T&)> callback) {
        std::lock_guard<std::mutex> lock(m_mutex);
        SubscriptionID id = m_nextId++;
        auto& subs = m_subscribers[std::type_index(typeid(T))];
        subs.push_back({
            id,
            [cb = std::move(callback)](const void* data) {
                cb(*static_cast<const T*>(data));
            }
        });
        return id;
    }

    template<typename T>
    bool Unsubscribe(SubscriptionID id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_subscribers.find(std::type_index(typeid(T)));
        if (it == m_subscribers.end()) return false;
        auto& subs = it->second;
        auto sub = std::remove_if(subs.begin(), subs.end(),
            [id](const Subscription& s) { return s.id == id; });
        if (sub == subs.end()) return false;
        subs.erase(sub, subs.end());
        return true;
    }

    template<typename T>
    void Publish(const T& event) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_subscribers.find(std::type_index(typeid(T)));
        if (it == m_subscribers.end()) return;
        auto subs = it->second;
        for (const auto& sub : subs) {
            sub.callback(&event);
        }
    }

    template<typename T>
    size_t GetSubscriberCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_subscribers.find(std::type_index(typeid(T)));
        if (it == m_subscribers.end()) return 0;
        return it->second.size();
    }

    template<typename T>
    void ClearSubscriptions() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_subscribers.erase(std::type_index(typeid(T)));
    }

    void ClearAll() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_subscribers.clear();
    }

private:
    struct Subscription {
        SubscriptionID id;
        std::function<void(const void*)> callback;
    };

    mutable std::mutex m_mutex;
    std::unordered_map<std::type_index, std::vector<Subscription>> m_subscribers;
    SubscriptionID m_nextId = 1;
};

// Test event types
struct DamageEvent {
    uint32_t entityId;
    float damage;
};

struct HealEvent {
    uint32_t entityId;
    float amount;
};

struct ChatEvent {
    std::string message;
};

} // namespace TestEvents

// =============================================================================
// Tests
// =============================================================================

TEST(EventBus_SubscribeAndPublish) {
    TestEvents::EventBus bus;
    float receivedDamage = 0.0f;
    uint32_t receivedEntity = 0;

    bus.Subscribe<TestEvents::DamageEvent>([&](const TestEvents::DamageEvent& e) {
        receivedDamage = e.damage;
        receivedEntity = e.entityId;
    });

    bus.Publish(TestEvents::DamageEvent{42, 25.0f});

    EXPECT_NEAR(receivedDamage, 25.0f, 0.001f);
    EXPECT_EQ(receivedEntity, (uint32_t)42);
}

TEST(EventBus_MultipleSubscribers) {
    TestEvents::EventBus bus;
    int callCount = 0;

    bus.Subscribe<TestEvents::DamageEvent>([&](const TestEvents::DamageEvent&) { callCount++; });
    bus.Subscribe<TestEvents::DamageEvent>([&](const TestEvents::DamageEvent&) { callCount++; });
    bus.Subscribe<TestEvents::DamageEvent>([&](const TestEvents::DamageEvent&) { callCount++; });

    bus.Publish(TestEvents::DamageEvent{1, 10.0f});
    EXPECT_EQ(callCount, 3);
}

TEST(EventBus_DifferentEventTypes) {
    TestEvents::EventBus bus;
    bool gotDamage = false;
    bool gotHeal = false;

    bus.Subscribe<TestEvents::DamageEvent>([&](const TestEvents::DamageEvent&) { gotDamage = true; });
    bus.Subscribe<TestEvents::HealEvent>([&](const TestEvents::HealEvent&) { gotHeal = true; });

    bus.Publish(TestEvents::DamageEvent{1, 10.0f});
    EXPECT_TRUE(gotDamage);
    EXPECT_FALSE(gotHeal);

    bus.Publish(TestEvents::HealEvent{1, 20.0f});
    EXPECT_TRUE(gotHeal);
}

TEST(EventBus_Unsubscribe) {
    TestEvents::EventBus bus;
    int callCount = 0;

    auto id = bus.Subscribe<TestEvents::DamageEvent>(
        [&](const TestEvents::DamageEvent&) { callCount++; });

    bus.Publish(TestEvents::DamageEvent{1, 10.0f});
    EXPECT_EQ(callCount, 1);

    bool removed = bus.Unsubscribe<TestEvents::DamageEvent>(id);
    EXPECT_TRUE(removed);

    bus.Publish(TestEvents::DamageEvent{1, 10.0f});
    EXPECT_EQ(callCount, 1); // Should not increment
}

TEST(EventBus_UnsubscribeInvalidId) {
    TestEvents::EventBus bus;
    bool removed = bus.Unsubscribe<TestEvents::DamageEvent>(999);
    EXPECT_FALSE(removed);
}

TEST(EventBus_SubscriberCount) {
    TestEvents::EventBus bus;
    EXPECT_EQ(bus.GetSubscriberCount<TestEvents::DamageEvent>(), (size_t)0);

    bus.Subscribe<TestEvents::DamageEvent>([](const TestEvents::DamageEvent&) {});
    bus.Subscribe<TestEvents::DamageEvent>([](const TestEvents::DamageEvent&) {});
    EXPECT_EQ(bus.GetSubscriberCount<TestEvents::DamageEvent>(), (size_t)2);
    EXPECT_EQ(bus.GetSubscriberCount<TestEvents::HealEvent>(), (size_t)0);
}

TEST(EventBus_ClearSubscriptions) {
    TestEvents::EventBus bus;
    int count = 0;

    bus.Subscribe<TestEvents::DamageEvent>([&](const TestEvents::DamageEvent&) { count++; });
    bus.Subscribe<TestEvents::DamageEvent>([&](const TestEvents::DamageEvent&) { count++; });

    bus.ClearSubscriptions<TestEvents::DamageEvent>();
    bus.Publish(TestEvents::DamageEvent{1, 10.0f});
    EXPECT_EQ(count, 0);
}

TEST(EventBus_ClearAll) {
    TestEvents::EventBus bus;
    int count = 0;

    bus.Subscribe<TestEvents::DamageEvent>([&](const TestEvents::DamageEvent&) { count++; });
    bus.Subscribe<TestEvents::HealEvent>([&](const TestEvents::HealEvent&) { count++; });

    bus.ClearAll();
    bus.Publish(TestEvents::DamageEvent{1, 10.0f});
    bus.Publish(TestEvents::HealEvent{1, 10.0f});
    EXPECT_EQ(count, 0);
}

TEST(EventBus_StringEventData) {
    TestEvents::EventBus bus;
    std::string receivedMsg;

    bus.Subscribe<TestEvents::ChatEvent>([&](const TestEvents::ChatEvent& e) {
        receivedMsg = e.message;
    });

    bus.Publish(TestEvents::ChatEvent{"Hello, world!"});
    EXPECT_EQ(receivedMsg, std::string("Hello, world!"));
}

TEST(EventBus_NoSubscribersPublish) {
    TestEvents::EventBus bus;
    // Publishing with no subscribers should not crash
    EXPECT_NO_THROW(bus.Publish(TestEvents::DamageEvent{1, 10.0f}));
}
