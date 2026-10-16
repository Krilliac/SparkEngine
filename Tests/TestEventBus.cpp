// TestEventBus.cpp - Tests for Spark::EventBus and SubscriptionHandle (public Spark/EventBus.h)
// Note: test names use "UtilEventBus_" prefix to avoid collisions with TestEventSystem.cpp
#include "TestFramework.h"
#include <Spark/EventBus.h>

#include <string>
#include <typeinfo>
#include <vector>

// Event types used in tests
struct EBCounterEvent
{
    int value = 0;
};

struct EBStringEvent
{
    std::string message;
};

struct EBEmptyEvent
{
};

// ============================================================================
// Basic publish/subscribe
// ============================================================================

TEST(UtilEventBus_Subscribe_And_Publish)
{
    Spark::EventBus bus;
    int received = 0;

    auto handle = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent& e) { received += e.value; });
    bus.Publish<EBCounterEvent>({5});

    EXPECT_EQ(received, 5);
}

TEST(UtilEventBus_MultipleSubscribers)
{
    Spark::EventBus bus;
    int a = 0, b = 0;

    auto h1 = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent& e) { a += e.value; });
    auto h2 = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent& e) { b += e.value; });
    bus.Publish<EBCounterEvent>({3});

    EXPECT_EQ(a, 3);
    EXPECT_EQ(b, 3);
}

TEST(UtilEventBus_MultiplePublish)
{
    Spark::EventBus bus;
    int total = 0;

    auto handle = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent& e) { total += e.value; });
    bus.Publish<EBCounterEvent>({1});
    bus.Publish<EBCounterEvent>({2});
    bus.Publish<EBCounterEvent>({3});

    EXPECT_EQ(total, 6);
}

TEST(UtilEventBus_DifferentEventTypes)
{
    Spark::EventBus bus;
    int counterReceived = 0;
    std::string stringReceived;

    auto h1 = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent& e) { counterReceived = e.value; });
    auto h2 = bus.Subscribe<EBStringEvent>([&](const EBStringEvent& e) { stringReceived = e.message; });

    bus.Publish<EBCounterEvent>({42});
    bus.Publish<EBStringEvent>({"hello"});

    EXPECT_EQ(counterReceived, 42);
    EXPECT_TRUE(stringReceived == "hello");
}

TEST(UtilEventBus_PublishWithNoSubscribers_NoCrash)
{
    Spark::EventBus bus;
    EXPECT_NO_THROW(bus.Publish<EBEmptyEvent>({}));
}

// ============================================================================
// SubscriptionHandle lifetime
// ============================================================================

TEST(UtilEventBus_HandleAutoUnsubscribes)
{
    Spark::EventBus bus;
    int received = 0;

    {
        auto handle = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent& e) { received += e.value; });
        bus.Publish<EBCounterEvent>({1});
        EXPECT_EQ(received, 1);
        // handle goes out of scope — unsubscribes automatically
    }

    bus.Publish<EBCounterEvent>({5});
    EXPECT_EQ(received, 1); // Should not receive after handle destroyed
}

TEST(UtilEventBus_ExplicitUnsubscribe)
{
    Spark::EventBus bus;
    int received = 0;

    auto handle = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent& e) { received += e.value; });
    bus.Publish<EBCounterEvent>({10});
    EXPECT_EQ(received, 10);

    handle.Unsubscribe();
    bus.Publish<EBCounterEvent>({99});
    EXPECT_EQ(received, 10); // No additional delivery
}

TEST(UtilEventBus_Handle_IsActive)
{
    Spark::EventBus bus;
    auto handle = bus.Subscribe<EBEmptyEvent>([](const EBEmptyEvent&) {});
    EXPECT_TRUE(handle.IsActive());
    handle.Unsubscribe();
    EXPECT_FALSE(handle.IsActive());
}

TEST(UtilEventBus_Handle_MultipleUnsubscribe_Safe)
{
    Spark::EventBus bus;
    auto handle = bus.Subscribe<EBEmptyEvent>([](const EBEmptyEvent&) {});
    EXPECT_NO_THROW(handle.Unsubscribe());
    EXPECT_NO_THROW(handle.Unsubscribe()); // Second call should be a no-op
}

TEST(UtilEventBus_Handle_MoveConstruct)
{
    Spark::EventBus bus;
    int received = 0;

    auto h1 = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent& e) { received += e.value; });
    auto h2 = std::move(h1);

    EXPECT_FALSE(h1.IsActive());
    EXPECT_TRUE(h2.IsActive());

    bus.Publish<EBCounterEvent>({7});
    EXPECT_EQ(received, 7);
}

TEST(UtilEventBus_Handle_MoveAssign)
{
    Spark::EventBus bus;
    int received = 0;

    auto h1 = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent& e) { received += e.value; });
    Spark::SubscriptionHandle h2;
    h2 = std::move(h1);

    EXPECT_FALSE(h1.IsActive());
    EXPECT_TRUE(h2.IsActive());

    bus.Publish<EBCounterEvent>({9});
    EXPECT_EQ(received, 9);
}

// ============================================================================
// SubscriberCount and ClearSubscribers
// ============================================================================

TEST(UtilEventBus_SubscriberCount)
{
    Spark::EventBus bus;
    EXPECT_EQ(bus.SubscriberCount<EBCounterEvent>(), 0u);

    auto h1 = bus.Subscribe<EBCounterEvent>([](const EBCounterEvent&) {});
    EXPECT_EQ(bus.SubscriberCount<EBCounterEvent>(), 1u);

    auto h2 = bus.Subscribe<EBCounterEvent>([](const EBCounterEvent&) {});
    EXPECT_EQ(bus.SubscriberCount<EBCounterEvent>(), 2u);

    h1.Unsubscribe();
    EXPECT_EQ(bus.SubscriberCount<EBCounterEvent>(), 1u);
}

TEST(UtilEventBus_ClearSubscribers)
{
    Spark::EventBus bus;
    int received = 0;

    auto handle = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent& e) { received += e.value; });
    bus.ClearSubscribers<EBCounterEvent>();

    bus.Publish<EBCounterEvent>({99});
    EXPECT_EQ(received, 0);
}

TEST(UtilEventBus_ClearAll)
{
    Spark::EventBus bus;
    int a = 0, b = 0;

    auto h1 = bus.Subscribe<EBCounterEvent>([&](const EBCounterEvent&) { ++a; });
    auto h2 = bus.Subscribe<EBStringEvent>([&](const EBStringEvent&) { ++b; });

    bus.ClearAll();

    bus.Publish<EBCounterEvent>({1});
    bus.Publish<EBStringEvent>({"x"});

    EXPECT_EQ(a, 0);
    EXPECT_EQ(b, 0);
}

// ============================================================================
// Global bus
// ============================================================================

TEST(UtilEventBus_Global_IsSingleton)
{
    auto& a = Spark::EventBus::Global();
    auto& b = Spark::EventBus::Global();
    EXPECT_TRUE(&a == &b);
}

// ============================================================================
// Cross-module channel identity (libc++ matches SDK event channels by name)
// ============================================================================

TEST(UtilEventBus_SharedTypeName_AcceptsExternalLinkageNames)
{
    using Spark::Detail::SharedTypeNameParser;
    EXPECT_TRUE(SharedTypeNameParser::IsShared("14EBCounterEvent"));
    EXPECT_TRUE(SharedTypeNameParser::IsShared("N5Spark15PlayerDiedEventE"));
    // An uppercase "IZ" inside an identifier is not a local-entity marker.
    EXPECT_TRUE(SharedTypeNameParser::IsShared("12HORIZONEvent"));
    EXPECT_TRUE(SharedTypeNameParser::IsShared("N6WIZARD9CastEventE"));
    EXPECT_TRUE(SharedTypeNameParser::IsShared("1WIiE"));
    EXPECT_TRUE(SharedTypeNameParser::IsShared("1WIJiPK3FooEE"));
    EXPECT_TRUE(SharedTypeNameParser::IsShared("1WILi5EE"));
    EXPECT_TRUE(SharedTypeNameParser::IsShared("1WILin5EE"));
    EXPECT_TRUE(SharedTypeNameParser::IsShared("1WINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEE"));
    // S_ back-references the already accepted N5Spark component.
    EXPECT_TRUE(SharedTypeNameParser::IsShared("N5Spark1WIS_EE"));
}

TEST(UtilEventBus_SharedTypeName_RefusesInternalLinkageNames)
{
    using Spark::Detail::SharedTypeNameParser;
    // Function-local types, alone and as template arguments in every position.
    EXPECT_FALSE(SharedTypeNameParser::IsShared("Z4mainE1L"));
    EXPECT_FALSE(SharedTypeNameParser::IsShared("1WIZ4mainE1LE"));
    EXPECT_FALSE(SharedTypeNameParser::IsShared("1WIJZ4mainE1LEE"));
    EXPECT_FALSE(SharedTypeNameParser::IsShared("1WIJiZ4mainE1LEE"));
    EXPECT_FALSE(SharedTypeNameParser::IsShared("1WIJPZ4mainE1LEE"));
    EXPECT_FALSE(SharedTypeNameParser::IsShared("1WIJKZ4mainE1LEE"));
    EXPECT_FALSE(SharedTypeNameParser::IsShared("1WIiPZ4mainE1LE"));
    // Anonymous namespaces, unnamed and lambda types, clang's $_N.
    EXPECT_FALSE(SharedTypeNameParser::IsShared("N12_GLOBAL__N_13FooE"));
    EXPECT_FALSE(SharedTypeNameParser::IsShared("1WIN12_GLOBAL__N_13FooEEE"));
    EXPECT_FALSE(SharedTypeNameParser::IsShared("N5SparkUt_E"));
    EXPECT_FALSE(SharedTypeNameParser::IsShared("N5SparkUlvE_E"));
    EXPECT_FALSE(SharedTypeNameParser::IsShared("3$_0"));
    EXPECT_FALSE(SharedTypeNameParser::IsShared("10._anon_195"));
    EXPECT_FALSE(SharedTypeNameParser::IsShared("1WI8._anon_3E"));
    // Malformed or truncated input.
    EXPECT_FALSE(SharedTypeNameParser::IsShared(""));
    EXPECT_FALSE(SharedTypeNameParser::IsShared("9Short"));
    EXPECT_FALSE(SharedTypeNameParser::IsShared("N5Spark"));
    EXPECT_FALSE(SharedTypeNameParser::IsShared("1WIi"));
    EXPECT_FALSE(SharedTypeNameParser::IsShared("05Event"));
}

namespace
{
    struct EBAnonymousEvent
    {
    };
} // namespace

template <typename... Ts> struct EBTemplateEvent
{
};

TEST(UtilEventBus_SharedTypeName_ClassifiesRealTypeNames)
{
#if defined(_MSC_VER)
    // MSVC and clang-cl both use the MSVC ABI's readable names ("struct X").
    SKIP_TEST("typeid().name() is not an Itanium mangled name under the MSVC ABI");
#else
    using Spark::Detail::SharedTypeNameParser;
    struct LocalEvent
    {
    };
    EXPECT_TRUE(SharedTypeNameParser::IsShared(typeid(EBCounterEvent).name()));
    EXPECT_TRUE(SharedTypeNameParser::IsShared(typeid(EBTemplateEvent<int, const EBCounterEvent*>).name()));
    EXPECT_FALSE(SharedTypeNameParser::IsShared(typeid(LocalEvent).name()));
    EXPECT_FALSE(SharedTypeNameParser::IsShared(typeid(EBAnonymousEvent).name()));
    EXPECT_FALSE(SharedTypeNameParser::IsShared(typeid(EBTemplateEvent<LocalEvent>).name()));
    EXPECT_FALSE(SharedTypeNameParser::IsShared(typeid(EBTemplateEvent<int, const LocalEvent*>).name()));
#endif
}
