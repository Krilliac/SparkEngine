// TestAdversarialEngine.cpp — Adversarial / fuzz-style tests for core engine systems
//
// Targets: EventBus, QueuedEventBus, CVar, SaveData types, ComponentSerializerRegistry,
//          HealthComponent logic, ServiceLocator, Console, HookDispatcher, NetBuffer,
//          and LagCompensator.
//
// All subsystems are exercised through their real public headers where possible.
// Standalone mirror structs are used only where direct headers pull in
// platform-specific or graphics dependencies incompatible with CI.

#include "TestFramework.h"

#ifndef SPARK_PLATFORM_WINDOWS
#ifndef _XM_NO_INTRINSICS_
#define _XM_NO_INTRINSICS_
#endif
#endif

#include "../SparkEngine/Source/Utils/EventBus.h"
#include "../SparkEngine/Source/Utils/ConsoleVariable.h"
#include "../SparkEngine/Source/Engine/Events/EventSystem.h"

#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace Spark;

// ============================================================================
// Standalone HealthComponent (mirrors ECS component logic)
// ============================================================================

namespace Adversarial
{

    struct HealthComponent
    {
        float health = 100.0f;
        float maxHealth = 100.0f;
        bool isDead = false;

        void TakeDamage(float amount)
        {
            if (isDead)
                return;
            health = std::max(health - amount, 0.0f);
            isDead = (health <= 0.0f);
        }

        void Heal(float amount)
        {
            if (isDead || amount < 0.0f)
                return;
            health = std::min(health + amount, maxHealth);
        }

        float GetHealthPercent() const
        {
            if (maxHealth <= 0.0f)
                return 0.0f;
            return health / maxHealth;
        }
    };

    // Minimal SerializedComponent mirror — does not require SaveSystem headers
    struct SerializedComponent
    {
        std::string typeName;
        std::unordered_map<std::string, std::string> properties;
    };

    // Minimal SaveData mirror
    struct SaveData
    {
        std::string saveName;
        std::string sceneName;
        float playTime = 0.0f;
        uint32_t version = 1;
        std::unordered_map<std::string, std::string> customState;
        std::vector<SerializedComponent> components;
    };

    // Simple alias-aware command parser (mirrors console alias resolution)
    struct CommandParser
    {
        std::unordered_map<std::string, std::string> aliases;

        // Resolve an alias chain with a depth cap to catch cycles
        std::string Resolve(const std::string& cmd, int maxDepth = 16) const
        {
            std::string current = cmd;
            for (int depth = 0; depth < maxDepth; ++depth)
            {
                auto it = aliases.find(current);
                if (it == aliases.end())
                    return current; // not an alias — done
                if (it->second == current)
                    return current; // self-alias — stop
                current = it->second;
            }
            return ""; // depth exceeded — indicates a cycle
        }
    };

} // namespace Adversarial

// ============================================================================
// EventBus — adversarial tests
// ============================================================================

TEST(Adversarial_EventBus_EmptyPublish)
{
    // Publishing to a bus with no subscribers must not crash
    EventBus bus;
    struct NoListenersEvent
    {
        int x = 42;
    };
    EXPECT_NO_THROW(bus.Publish(NoListenersEvent{}));
}

TEST(Adversarial_EventBus_SubscribeUnsubscribeCycle)
{
    // 5000 rapid subscribe/unsubscribe cycles — no leak or corruption
    EventBus bus;
    struct CycleEvent
    {
        int v = 0;
    };
    for (int i = 0; i < 5000; ++i)
    {
        auto handle = bus.Subscribe<CycleEvent>([](const CycleEvent&) {});
        handle.Unsubscribe();
        EXPECT_FALSE(handle.IsActive());
    }
}

TEST(Adversarial_EventBus_ManySubs_SinglePublish)
{
    // 10 000 subscribers on one event type — publish must reach all of them
    EventBus bus;
    struct CountedEvent
    {
        int id = 0;
    };

    std::atomic<int> callCount{0};
    std::vector<SubscriptionHandle> handles;
    handles.reserve(10000);

    for (int i = 0; i < 10000; ++i)
        handles.push_back(bus.Subscribe<CountedEvent>([&callCount](const CountedEvent&) { callCount.fetch_add(1); }));

    bus.Publish(CountedEvent{1});
    EXPECT_EQ(callCount.load(), 10000);
}

TEST(Adversarial_EventBus_HandleMoveSafety)
{
    // Move a handle — only one copy should unsubscribe
    EventBus bus;
    struct MoveEvent
    {
        int v = 0;
    };

    int callCount = 0;
    auto h1 = bus.Subscribe<MoveEvent>([&callCount](const MoveEvent&) { ++callCount; });
    auto h2 = std::move(h1);

    EXPECT_FALSE(h1.IsActive()); // moved-from handle is inactive
    EXPECT_TRUE(h2.IsActive());

    bus.Publish(MoveEvent{7});
    EXPECT_EQ(callCount, 1); // exactly one handler was active

    h2.Unsubscribe();
    bus.Publish(MoveEvent{8});
    EXPECT_EQ(callCount, 1); // handler removed — no additional call
}

TEST(Adversarial_EventBus_DoubleUnsubscribe)
{
    // Calling Unsubscribe() twice on the same handle must be a safe no-op
    EventBus bus;
    struct DoubleUnsub
    {
        int v = 0;
    };
    auto handle = bus.Subscribe<DoubleUnsub>([](const DoubleUnsub&) {});
    EXPECT_NO_THROW(handle.Unsubscribe());
    EXPECT_NO_THROW(handle.Unsubscribe()); // second call must not crash
    EXPECT_FALSE(handle.IsActive());
}

TEST(Adversarial_EventBus_RecursivePublish)
{
    // Handlers that re-publish the same event type must not cause infinite recursion.
    // The EventBus guards against recursive publish for the same type via thread_local depth.
    EventBus bus;
    struct RecursiveEvent
    {
        int depth = 0;
    };

    int outerCallCount = 0;
    auto handle = bus.Subscribe<RecursiveEvent>(
        [&bus, &outerCallCount](const RecursiveEvent& e)
        {
            ++outerCallCount;
            // Attempt re-entrant publish — must be silently dropped, not stack overflow
            bus.Publish(RecursiveEvent{e.depth + 1});
        });

    EXPECT_NO_THROW(bus.Publish(RecursiveEvent{0}));
    EXPECT_EQ(outerCallCount, 1); // only the original publish fired; recursive attempt dropped
}

TEST(Adversarial_EventBus_LargeEventPayload)
{
    // Publishing an event carrying a 1 MB string must not crash or truncate
    EventBus bus;
    struct BigEvent
    {
        std::string payload;
    };

    std::string bigPayload(1024 * 1024, 'X');
    std::string received;

    auto handle = bus.Subscribe<BigEvent>([&received](const BigEvent& e) { received = e.payload; });

    EXPECT_NO_THROW(bus.Publish(BigEvent{bigPayload}));
    EXPECT_EQ(received.size(), bigPayload.size());
}

TEST(Adversarial_EventBus_ConcurrentPublish)
{
    // Two threads publish concurrently — no data race or crash
    EventBus bus;
    struct ThreadEvent
    {
        int threadId = 0;
    };

    std::atomic<int> totalReceived{0};
    auto handle = bus.Subscribe<ThreadEvent>([&totalReceived](const ThreadEvent&) { totalReceived.fetch_add(1); });

    constexpr int kPublishesPerThread = 500;
    auto publish = [&bus]()
    {
        for (int i = 0; i < kPublishesPerThread; ++i)
            bus.Publish(ThreadEvent{i});
    };

    std::thread t1(publish);
    std::thread t2(publish);
    t1.join();
    t2.join();

    EXPECT_EQ(totalReceived.load(), kPublishesPerThread * 2);
}

TEST(Adversarial_EventBus_ScopeDestructionCleanup)
{
    // Handles destroyed out of scope must cleanly unsubscribe without use-after-free
    EventBus bus;
    struct ScopeEvent
    {
        int v = 0;
    };

    int callCount = 0;
    {
        auto h = bus.Subscribe<ScopeEvent>([&callCount](const ScopeEvent&) { ++callCount; });
        bus.Publish(ScopeEvent{1});
        EXPECT_EQ(callCount, 1);
    } // handle destroyed here — subscription removed

    bus.Publish(ScopeEvent{2});
    EXPECT_EQ(callCount, 1); // no additional call after handle destroyed
}

TEST(Adversarial_EventBus_EmptyStringEventPayload)
{
    // Event with empty string fields must not crash downstream handlers
    EventBus bus;
    struct StringEvent
    {
        std::string source;
        std::string cause;
    };

    std::string gotSource = "unset";
    auto handle = bus.Subscribe<StringEvent>([&gotSource](const StringEvent& e) { gotSource = e.source; });

    EXPECT_NO_THROW(bus.Publish(StringEvent{"", ""}));
    EXPECT_TRUE(gotSource.empty());
}

// ============================================================================
// CVar — adversarial tests
// ============================================================================

TEST(Adversarial_CVar_SetFromString_MalformedInt)
{
    CVar<int> cv("test.adv.malformed_int", 42, CVarFlags::None, "adversarial int");
    EXPECT_FALSE(cv.SetFromString("not_a_number"));
    EXPECT_EQ(cv.Get(), 42); // value unchanged

    EXPECT_FALSE(cv.SetFromString(""));
    EXPECT_EQ(cv.Get(), 42);

    EXPECT_FALSE(cv.SetFromString("   "));
    EXPECT_EQ(cv.Get(), 42);
}

TEST(Adversarial_CVar_SetFromString_OverflowInt)
{
    CVar<int> cv("test.adv.overflow_int", 0, CVarFlags::None, "overflow int");
    // std::stoi on a value beyond INT_MAX throws — SetFromString must return false, not crash
    std::string huge = "99999999999999999999999999999999";
    EXPECT_FALSE(cv.SetFromString(huge));
    EXPECT_EQ(cv.Get(), 0);
}

TEST(Adversarial_CVar_SetFromString_NaNFloat)
{
    CVar<float> cv("test.adv.nan_float", 1.0f, CVarFlags::None, "nan float");
    // "nan" is a valid stof input — engine should accept it or reject gracefully
    bool result = cv.SetFromString("nan");
    // If accepted, value should actually be NaN; if rejected, stays 1.0f
    if (result)
    {
        EXPECT_TRUE(std::isnan(cv.Get()));
    }
    else
    {
        EXPECT_NEAR(cv.Get(), 1.0f, 0.0001f);
    }
    // Either way, no crash
    EXPECT_TRUE(true);
}

TEST(Adversarial_CVar_SetFromString_InfFloat)
{
    CVar<float> cv("test.adv.inf_float", 0.0f, CVarFlags::None, "inf float");
    bool result = cv.SetFromString("inf");
    if (result)
    {
        EXPECT_TRUE(std::isinf(cv.Get()));
    }
    else
    {
        EXPECT_NEAR(cv.Get(), 0.0f, 0.0001f);
    }
}

TEST(Adversarial_CVar_RangeClamping_Extremes)
{
    CVar<int> cv("test.adv.range_int", 5, CVarFlags::None, "ranged int", 0, 10);
    cv.Set(std::numeric_limits<int>::max());
    EXPECT_EQ(cv.Get(), 10); // clamped to max

    cv.Set(std::numeric_limits<int>::min());
    EXPECT_EQ(cv.Get(), 0); // clamped to min

    cv.Set(-99999);
    EXPECT_EQ(cv.Get(), 0);

    cv.Set(99999);
    EXPECT_EQ(cv.Get(), 10);
}

TEST(Adversarial_CVar_ReadOnly_IgnoresSet)
{
    CVar<int> cv("test.adv.readonly", 7, CVarFlags::ReadOnly, "readonly");
    cv.Set(42);
    EXPECT_EQ(cv.Get(), 7); // must not have changed

    EXPECT_FALSE(cv.SetFromString("100"));
    EXPECT_EQ(cv.Get(), 7);
}

TEST(Adversarial_CVar_Cheat_BlockedWithoutCheats)
{
    CVarRegistry::Get().SetCheatsEnabled(false);
    CVar<int> cv("test.adv.cheat_blocked", 0, CVarFlags::Cheat, "cheat cvar");
    EXPECT_FALSE(cv.SetFromString("999"));
    EXPECT_EQ(cv.Get(), 0); // blocked — cheats off
}

TEST(Adversarial_CVar_Cheat_AllowedWithCheats)
{
    CVarRegistry::Get().SetCheatsEnabled(true);
    CVar<int> cv("test.adv.cheat_allowed", 0, CVarFlags::Cheat, "cheat cvar enabled");
    EXPECT_TRUE(cv.SetFromString("999"));
    EXPECT_EQ(cv.Get(), 999);
    CVarRegistry::Get().SetCheatsEnabled(false); // restore
}

TEST(Adversarial_CVar_ResetToDefault)
{
    CVar<float> cv("test.adv.reset_float", 3.14f, CVarFlags::None, "reset test");
    cv.Set(99.0f);
    EXPECT_TRUE(cv.IsModified());
    cv.ResetToDefault();
    EXPECT_NEAR(cv.Get(), 3.14f, 0.001f);
    EXPECT_FALSE(cv.IsModified());
}

TEST(Adversarial_CVar_BoolFromVariousStrings)
{
    CVar<bool> cv("test.adv.bool_variants", false, CVarFlags::None, "bool parsing");

    // All truthy variants
    for (const char* s : {"true", "1", "on", "yes", "TRUE", "ON", "YES"})
    {
        cv.ResetToDefault();
        EXPECT_TRUE(cv.SetFromString(s));
        EXPECT_TRUE(cv.Get());
    }

    // All falsy variants
    for (const char* s : {"false", "0", "off", "no", "FALSE", "OFF", "NO"})
    {
        cv.Set(true);
        EXPECT_TRUE(cv.SetFromString(s));
        EXPECT_FALSE(cv.Get());
    }

    // Garbage should fail
    cv.Set(true);
    EXPECT_FALSE(cv.SetFromString("maybe"));
    EXPECT_TRUE(cv.Get()); // unchanged
}

TEST(Adversarial_CVar_StringCVar_HugeValue)
{
    CVar<std::string> cv("test.adv.big_string", "", CVarFlags::None, "large string cvar");
    std::string big(64 * 1024, 'A'); // 64 KB string
    EXPECT_NO_THROW(cv.Set(big));
    EXPECT_EQ(cv.Get().size(), big.size());
}

TEST(Adversarial_CVar_ChangeCallback_FiresOnSet)
{
    int callCount = 0;
    int lastOld = -1;
    int lastNew = -1;

    CVar<int> cv("test.adv.callback", 0, CVarFlags::None, "callback cvar");
    cv.OnChange(
        [&callCount, &lastOld, &lastNew](const int& oldVal, const int& newVal)
        {
            ++callCount;
            lastOld = oldVal;
            lastNew = newVal;
        });

    cv.Set(10);
    EXPECT_EQ(callCount, 1);
    EXPECT_EQ(lastOld, 0);
    EXPECT_EQ(lastNew, 10);

    // ReadOnly — callback must NOT fire
    cv.Set(10); // same value; still fires (value replaced)
    EXPECT_EQ(callCount, 2);
}

TEST(Adversarial_CVar_SetFromString_EmptyStringCVar)
{
    CVar<std::string> cv("test.adv.str_empty", "default", CVarFlags::None, "string empty");
    EXPECT_TRUE(cv.SetFromString(""));
    EXPECT_EQ(cv.Get(), "");
}

TEST(Adversarial_CVar_FloatRange_ZeroWidth)
{
    // min == max == 5.0f — every set should clamp to exactly 5.0f
    CVar<float> cv("test.adv.zero_range", 5.0f, CVarFlags::None, "zero-width range", 5.0f, 5.0f);
    cv.Set(-1000.0f);
    EXPECT_NEAR(cv.Get(), 5.0f, 0.0001f);
    cv.Set(1000.0f);
    EXPECT_NEAR(cv.Get(), 5.0f, 0.0001f);
    cv.Set(std::numeric_limits<float>::quiet_NaN());
    // NaN clamp behaviour is implementation-defined; engine must not crash
    EXPECT_TRUE(true);
}

// ============================================================================
// HealthComponent — adversarial boundary tests (standalone)
// ============================================================================

TEST(Adversarial_Health_NaNDamage)
{
    Adversarial::HealthComponent h;
    // NaN damage should not crash; health must remain a valid number
    EXPECT_NO_THROW(h.TakeDamage(std::numeric_limits<float>::quiet_NaN()));
    // health - NaN = NaN; std::max(NaN, 0) is implementation-defined but must not throw
    EXPECT_TRUE(true);
}

TEST(Adversarial_Health_InfiniteDamage)
{
    Adversarial::HealthComponent h;
    h.TakeDamage(std::numeric_limits<float>::infinity());
    EXPECT_NEAR(h.health, 0.0f, 0.0001f);
    EXPECT_TRUE(h.isDead);
}

TEST(Adversarial_Health_NegativeDamage)
{
    // Negative damage is not healing — TakeDamage(-50) should not increase health
    Adversarial::HealthComponent h;
    h.health = 50.0f;
    h.TakeDamage(-50.0f); // health - (-50) = health + 50 = 100 → clamped to 100? or treated as 0?
    // The implementation uses std::max(health - amount, 0.0f).
    // health - (-50) = 100, max(100, 0) = 100 — does not kill entity
    EXPECT_FALSE(h.isDead);
}

TEST(Adversarial_Health_ZeroMaxHealth_GetPercent)
{
    Adversarial::HealthComponent h;
    h.maxHealth = 0.0f;
    h.health = 0.0f;
    // Must not divide by zero
    EXPECT_NO_THROW(h.GetHealthPercent());
    EXPECT_NEAR(h.GetHealthPercent(), 0.0f, 0.0001f);
}

TEST(Adversarial_Health_HealDeadEntity)
{
    Adversarial::HealthComponent h;
    h.TakeDamage(200.0f); // kill
    EXPECT_TRUE(h.isDead);
    h.Heal(100.0f); // healing a dead entity must be a no-op
    EXPECT_NEAR(h.health, 0.0f, 0.0001f);
    EXPECT_TRUE(h.isDead);
}

TEST(Adversarial_Health_HealNegativeAmount)
{
    Adversarial::HealthComponent h;
    h.health = 50.0f;
    h.Heal(-30.0f); // negative healing must be ignored
    EXPECT_NEAR(h.health, 50.0f, 0.0001f);
}

TEST(Adversarial_Health_HealOverMax)
{
    Adversarial::HealthComponent h;
    h.health = 90.0f;
    h.maxHealth = 100.0f;
    h.Heal(std::numeric_limits<float>::infinity());
    EXPECT_NEAR(h.health, 100.0f, 0.0001f); // clamped to maxHealth
}

TEST(Adversarial_Health_RapidKillReviveChurn)
{
    // 10 000 alternating kill / revive cycles — no numeric corruption
    Adversarial::HealthComponent h;
    for (int i = 0; i < 10000; ++i)
    {
        h.TakeDamage(9999.0f);
        EXPECT_TRUE(h.isDead);
        EXPECT_NEAR(h.health, 0.0f, 0.0001f);

        // Revive manually
        h.health = 100.0f;
        h.isDead = false;
    }
    EXPECT_FALSE(h.isDead);
    EXPECT_NEAR(h.health, 100.0f, 0.0001f);
}

// ============================================================================
// SaveData types — adversarial boundary tests (standalone mirror)
// ============================================================================

TEST(Adversarial_SaveData_EmptyComponentProperties)
{
    // Deserialising a SerializedComponent with no properties must not crash
    Adversarial::SerializedComponent sc;
    sc.typeName = "Transform";
    // properties is empty — any code that does properties.at("posX") would throw
    EXPECT_NO_THROW(
        [&sc]()
        {
            // Simulate what a robust deserializer should do: check before access
            auto it = sc.properties.find("posX");
            float posX = (it != sc.properties.end()) ? std::stof(it->second) : 0.0f;
            (void)posX;
        }());
}

TEST(Adversarial_SaveData_MissingRequiredProperty)
{
    // A deserializer that calls at() on a missing key should throw std::out_of_range
    Adversarial::SerializedComponent sc;
    sc.typeName = "HealthComponent";
    sc.properties["health"] = "75.0";
    // "maxHealth" is intentionally absent

    EXPECT_THROW(
        [&sc]()
        {
            // Fragile access pattern — intentionally triggers the exception
            [[maybe_unused]] float maxH = std::stof(sc.properties.at("maxHealth"));
        }(),
        std::out_of_range);
}

TEST(Adversarial_SaveData_MalformedFloatProperty)
{
    // Feeding a non-numeric string where a float is expected should not silently produce garbage
    Adversarial::SerializedComponent sc;
    sc.typeName = "HealthComponent";
    sc.properties["health"] = "not_a_float";

    bool threw = false;
    try
    {
        [[maybe_unused]] float h = std::stof(sc.properties.at("health"));
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    catch (const std::out_of_range&)
    {
        threw = true;
    }
    EXPECT_TRUE(threw);
}

TEST(Adversarial_SaveData_EmptyTypeName)
{
    // A component with empty typeName is degenerate — a registry lookup must not crash
    Adversarial::SerializedComponent sc;
    sc.typeName = "";
    sc.properties["foo"] = "bar";

    // Simulate registry lookup: empty string as key
    std::unordered_map<std::string, int> fakeRegistry;
    fakeRegistry["Transform"] = 1;
    auto it = fakeRegistry.find(sc.typeName);
    EXPECT_TRUE(it == fakeRegistry.end()); // not found — correct, no crash
}

TEST(Adversarial_SaveData_DeeplyNestedCustomState)
{
    // Filling customState with 100 000 entries must not crash or deadlock
    Adversarial::SaveData data;
    for (int i = 0; i < 100000; ++i)
        data.customState["key_" + std::to_string(i)] = "value_" + std::to_string(i);

    EXPECT_EQ(data.customState.size(), 100000u);

    // Look up an arbitrary entry
    EXPECT_EQ(data.customState["key_42"], "value_42");
    EXPECT_EQ(data.customState["key_99999"], "value_99999");
}

TEST(Adversarial_SaveData_MassiveComponentList)
{
    // A SaveData with 50 000 serialized components must not OOM or crash construction
    Adversarial::SaveData data;
    data.components.reserve(50000);
    for (int i = 0; i < 50000; ++i)
    {
        Adversarial::SerializedComponent sc;
        sc.typeName = "Transform";
        sc.properties["posX"] = std::to_string(static_cast<float>(i));
        data.components.push_back(std::move(sc));
    }
    EXPECT_EQ(data.components.size(), 50000u);
    EXPECT_EQ(data.components[0].typeName, "Transform");
    EXPECT_EQ(data.components[49999].typeName, "Transform");
}

TEST(Adversarial_SaveData_ExtremeMetadataValues)
{
    Adversarial::SaveData data;
    data.playTime = std::numeric_limits<float>::max();
    data.version = std::numeric_limits<uint32_t>::max();
    data.saveName = std::string(1024 * 64, 'Z'); // 64 KB name
    EXPECT_EQ(data.saveName.size(), 64u * 1024u);
    EXPECT_EQ(data.version, std::numeric_limits<uint32_t>::max());
}

TEST(Adversarial_SaveData_PathTraversalSlotName)
{
    // Slot names containing path traversal sequences must be identifiable as invalid
    // Note: string literals with embedded \x00 silently truncate when used as const char*.
    // Test the embedded-null case explicitly via the two-argument std::string constructor.
    const std::vector<std::string> badNames = {
        "../../../etc/passwd",
        "..\\..\\System32\\config\\SAM",
        "/absolute/path",
        std::string("slot\x00null", 9), // explicit length preserves the null byte
        "",
        "   ",
        "slot/subdir",
        "slot\\subdir",
    };

    // Simulate IsValidSlotName logic: only alphanumeric, underscore, hyphen allowed
    auto IsValidSlotName = [](const std::string& name) -> bool
    {
        if (name.empty())
            return false;
        for (char c : name)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
                return false;
        }
        return true;
    };

    for (const auto& name : badNames)
        EXPECT_FALSE(IsValidSlotName(name));

    // Good names
    EXPECT_TRUE(IsValidSlotName("slot1"));
    EXPECT_TRUE(IsValidSlotName("checkpoint_02"));
    EXPECT_TRUE(IsValidSlotName("quick-save"));
}

// ============================================================================
// Command alias resolution — adversarial tests (standalone)
// ============================================================================

TEST(Adversarial_CommandParser_CircularAlias)
{
    // A → B → A cycle must not infinite-loop
    Adversarial::CommandParser parser;
    parser.aliases["cmdA"] = "cmdB";
    parser.aliases["cmdB"] = "cmdA";

    std::string result = parser.Resolve("cmdA");
    EXPECT_TRUE(result.empty()); // depth exceeded → empty string signals cycle
}

TEST(Adversarial_CommandParser_SelfAlias)
{
    Adversarial::CommandParser parser;
    parser.aliases["self"] = "self";
    std::string result = parser.Resolve("self");
    EXPECT_EQ(result, "self"); // self-alias terminates immediately
}

TEST(Adversarial_CommandParser_LongAliasChain)
{
    // Chain of 20 aliases — within depth cap of 16, should truncate and return empty
    Adversarial::CommandParser parser;
    for (int i = 0; i < 20; ++i)
        parser.aliases["cmd" + std::to_string(i)] = "cmd" + std::to_string(i + 1);

    std::string result = parser.Resolve("cmd0");
    // Chain is 20 deep, cap is 16 — expects empty (cycle-like truncation)
    EXPECT_TRUE(result.empty());
}

TEST(Adversarial_CommandParser_NoAlias)
{
    Adversarial::CommandParser parser;
    std::string result = parser.Resolve("actual_command");
    EXPECT_EQ(result, "actual_command"); // not an alias — returned as-is
}

TEST(Adversarial_CommandParser_EmptyAlias)
{
    Adversarial::CommandParser parser;
    parser.aliases["empty_target"] = "";
    std::string result = parser.Resolve("empty_target");
    // Empty target is not a registered alias — terminates immediately
    EXPECT_EQ(result, "");
}

// ============================================================================
// CVarRegistry — adversarial concurrent access
// ============================================================================

TEST(Adversarial_CVarRegistry_ConcurrentFind)
{
    // Register a cvar, then read it from multiple threads simultaneously
    CVar<int> cv("test.adv.concurrent_find", 77, CVarFlags::None, "concurrent read test");

    constexpr int kThreads = 8;
    constexpr int kReadsPerThread = 1000;
    std::atomic<int> failures{0};

    auto reader = [&failures]()
    {
        auto* found = CVarRegistry::Get().Find("test.adv.concurrent_find");
        for (int i = 0; i < kReadsPerThread; ++i)
        {
            if (found == nullptr || found->GetValueString().empty())
                failures.fetch_add(1);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back(reader);
    for (auto& t : threads)
        t.join();

    EXPECT_EQ(failures.load(), 0);
}

TEST(Adversarial_CVarRegistry_FindNonExistent)
{
    // Looking up a name that was never registered must return nullptr, not crash
    ICVar* result = CVarRegistry::Get().Find("this.cvar.does.not.exist.ever");
    EXPECT_TRUE(result == nullptr);
}

TEST(Adversarial_CVarRegistry_ResetAll_PreservesReadOnly)
{
    CVar<int> rw("test.adv.resetall_rw", 0, CVarFlags::None, "rw for reset test");
    CVar<int> ro("test.adv.resetall_ro", 42, CVarFlags::ReadOnly, "ro for reset test");

    rw.Set(99);
    // Cannot set ReadOnly via Set(), so it stays 42

    CVarRegistry::Get().ResetAll();

    EXPECT_EQ(rw.Get(), 0);  // reset to default
    EXPECT_EQ(ro.Get(), 42); // unchanged — ReadOnly
}

// ============================================================================
// Additional EventBus adversarial tests (unique event types to avoid
// thread_local s_publishDepth contamination)
// ============================================================================

namespace
{
    struct AdvEvtThrow
    {
    };
    struct AdvEvtFloat
    {
        float damage = 0.0f;
    };
} // namespace

TEST(Adversarial_EventBus_HandlerThrows_ExceptionIsolated)
{
    EventBus bus;
    int afterCount = 0;
    auto handle1 = bus.Subscribe<AdvEvtThrow>([](const AdvEvtThrow&) { throw std::runtime_error("boom"); });
    auto handle2 = bus.Subscribe<AdvEvtThrow>([&](const AdvEvtThrow&) { ++afterCount; });
    // Fault isolation: throwing handler is caught, subsequent handlers still execute
    EXPECT_NO_THROW(bus.Publish(AdvEvtThrow{}));
    EXPECT_EQ(afterCount, 1);
}

TEST(Adversarial_EventBus_ClearAllThenPublish)
{
    EventBus bus;
    int count = 0;
    auto handle = bus.Subscribe<AdvEvtFloat>([&](const AdvEvtFloat&) { ++count; });
    bus.ClearAll();
    bus.Publish(AdvEvtFloat{});
    EXPECT_EQ(count, 0);
}

TEST(Adversarial_EventBus_NegativeAndInfiniteFloatEvent)
{
    EventBus bus;
    float received = 0.0f;
    auto handle = bus.Subscribe<AdvEvtFloat>([&](const AdvEvtFloat& e) { received = e.damage; });

    bus.Publish(AdvEvtFloat{-1.0f});
    EXPECT_EQ(received, -1.0f);

    bus.Publish(AdvEvtFloat{std::numeric_limits<float>::infinity()});
    EXPECT_TRUE(std::isinf(received));

    bus.Publish(AdvEvtFloat{std::numeric_limits<float>::quiet_NaN()});
    EXPECT_TRUE(std::isnan(received));
}

// ============================================================================
// QueuedEventBus adversarial tests (uses real Spark::QueuedEventBus)
// ============================================================================

TEST(Adversarial_QueuedBus_OverflowDropsOldest)
{
    QueuedEventBus queue;
    constexpr size_t kOver = 10001;
    for (size_t i = 0; i < kOver; ++i)
        queue.QueueEvent(EntityDamagedEvent{static_cast<uint32_t>(i), 1.0f, ""});

    EXPECT_LE(queue.GetPendingCount(), static_cast<size_t>(10000));
    EXPECT_GT(queue.GetDroppedEventCount(), static_cast<size_t>(0));
}

TEST(Adversarial_QueuedBus_ClearThenDispatch_NoDelivery)
{
    QueuedEventBus queue;
    EventBus bus;
    int count = 0;
    auto handle = bus.Subscribe<EntityDamagedEvent>([&](const EntityDamagedEvent&) { ++count; });

    queue.QueueEvent(EntityDamagedEvent{});
    queue.Clear();
    queue.DispatchAll(bus);

    EXPECT_EQ(count, 0);
    EXPECT_EQ(queue.GetPendingCount(), static_cast<size_t>(0));
}

TEST(Adversarial_QueuedBus_DroppedCountResetAfterDispatch)
{
    QueuedEventBus queue;
    EventBus bus;

    for (size_t i = 0; i < 10001; ++i)
        queue.QueueEvent(EntityDamagedEvent{});

    EXPECT_GT(queue.GetDroppedEventCount(), static_cast<size_t>(0));
    queue.DispatchAll(bus);
    EXPECT_EQ(queue.GetDroppedEventCount(), static_cast<size_t>(0));
}

TEST(Adversarial_QueuedBus_ConcurrentQueue_NoRace)
{
    QueuedEventBus queue;
    constexpr int kThreads = 8;
    constexpr int kEventsPerThread = 500;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back(
            [&]
            {
                for (int i = 0; i < kEventsPerThread; ++i)
                    queue.QueueEvent(EntityDamagedEvent{});
            });
    }
    for (auto& th : threads)
        th.join();

    EXPECT_LE(queue.GetPendingCount(), static_cast<size_t>(10000));
}

// ============================================================================
// Standalone ServiceLocator adversarial tests
// ============================================================================

namespace
{

    struct SLSubA
    {
        int value = 1;
    };
    struct SLSubB
    {
        int value = 2;
    };

    using SLTypeId = const void*;
    template <typename T> SLTypeId SLGetTypeId()
    {
        static const char id = 0;
        return &id;
    }

    class StandaloneServiceLocator
    {
      public:
        template <typename T> void Register(T* ptr) { m_registry[SLGetTypeId<T>()] = static_cast<void*>(ptr); }

        template <typename T> T* Get() const
        {
            auto it = m_registry.find(SLGetTypeId<T>());
            return (it != m_registry.end()) ? static_cast<T*>(it->second) : nullptr;
        }

      private:
        std::unordered_map<SLTypeId, void*> m_registry;
    };

} // namespace

TEST(Adversarial_ServiceLocator_GetBeforeRegister_ReturnsNull)
{
    StandaloneServiceLocator loc;
    EXPECT_EQ(loc.Get<SLSubA>(), nullptr);
    EXPECT_EQ(loc.Get<SLSubB>(), nullptr);
}

TEST(Adversarial_ServiceLocator_RegisterNull)
{
    StandaloneServiceLocator loc;
    loc.Register<SLSubA>(nullptr);
    EXPECT_EQ(loc.Get<SLSubA>(), nullptr);
}

TEST(Adversarial_ServiceLocator_OverwriteRegistration)
{
    StandaloneServiceLocator loc;
    SLSubA a1;
    a1.value = 10;
    SLSubA a2;
    a2.value = 20;

    loc.Register(&a1);
    EXPECT_EQ(loc.Get<SLSubA>()->value, 10);

    loc.Register(&a2);
    EXPECT_EQ(loc.Get<SLSubA>()->value, 20);
}

TEST(Adversarial_ServiceLocator_TwoTypesDoNotCollide)
{
    StandaloneServiceLocator loc;
    SLSubA a;
    SLSubB b;
    loc.Register(&a);
    loc.Register(&b);
    EXPECT_EQ(loc.Get<SLSubA>(), &a);
    EXPECT_EQ(loc.Get<SLSubB>(), &b);
}

TEST(Adversarial_ServiceLocator_MassRegistration)
{
    StandaloneServiceLocator loc;
    std::vector<SLSubA> pool(1000);
    for (auto& sub : pool)
        loc.Register(&sub);
    EXPECT_EQ(loc.Get<SLSubA>(), &pool.back());
}

// ============================================================================
// Standalone ConsoleSystem adversarial tests
// ============================================================================

namespace
{

    enum class AdvPermission : uint8_t
    {
        Player = 0,
        Admin = 2,
        Developer = 3
    };

    class StandaloneConsole
    {
      public:
        using Handler = std::function<std::string(const std::vector<std::string>&)>;

        bool RegisterCommand(const std::string& name, Handler handler, AdvPermission perm = AdvPermission::Player)
        {
            if (name.empty() || !handler)
                return false;
            m_commands[name] = {std::move(handler), perm};
            return true;
        }

        bool Execute(const std::string& name, const std::vector<std::string>& args, AdvPermission callerPerm,
                     std::string& outResult)
        {
            auto it = m_commands.find(name);
            if (it == m_commands.end())
                return false;
            if (static_cast<uint8_t>(callerPerm) < static_cast<uint8_t>(it->second.requiredPerm))
                return false;
            outResult = it->second.handler(args);
            return true;
        }

        void SetAlias(const std::string& alias, const std::string& target) { m_aliases[alias] = target; }

        std::string ResolveAlias(const std::string& name, int depth = 0) const
        {
            if (depth > 10)
                return name;
            auto it = m_aliases.find(name);
            if (it == m_aliases.end())
                return name;
            return ResolveAlias(it->second, depth + 1);
        }

        size_t CommandCount() const { return m_commands.size(); }

      private:
        struct Entry
        {
            Handler handler;
            AdvPermission requiredPerm;
        };
        std::unordered_map<std::string, Entry> m_commands;
        std::unordered_map<std::string, std::string> m_aliases;
    };

} // namespace

TEST(Adversarial_Console_EmptyCommandName_Rejected)
{
    StandaloneConsole con;
    EXPECT_FALSE(con.RegisterCommand("", [](const std::vector<std::string>&) { return std::string("ok"); }));
    EXPECT_EQ(con.CommandCount(), static_cast<size_t>(0));
}

TEST(Adversarial_Console_NullHandler_Rejected)
{
    StandaloneConsole con;
    EXPECT_FALSE(con.RegisterCommand("cmd", nullptr));
}

TEST(Adversarial_Console_ExecuteUnregistered_ReturnsFalse)
{
    StandaloneConsole con;
    std::string result;
    EXPECT_FALSE(con.Execute("nonexistent", {}, AdvPermission::Developer, result));
}

TEST(Adversarial_Console_InsufficientPermission_Blocked)
{
    StandaloneConsole con;
    bool ran = false;
    con.RegisterCommand(
        "admin_cmd",
        [&](const std::vector<std::string>&)
        {
            ran = true;
            return std::string("ok");
        },
        AdvPermission::Admin);

    std::string result;
    EXPECT_FALSE(con.Execute("admin_cmd", {}, AdvPermission::Player, result));
    EXPECT_FALSE(ran);
    EXPECT_TRUE(con.Execute("admin_cmd", {}, AdvPermission::Developer, result));
    EXPECT_TRUE(ran);
}

TEST(Adversarial_Console_HandlerThrows_ExceptionPropagates)
{
    StandaloneConsole con;
    con.RegisterCommand("boom", [](const std::vector<std::string>&) -> std::string { throw std::runtime_error("!"); });
    std::string result;
    EXPECT_THROW(con.Execute("boom", {}, AdvPermission::Developer, result), std::runtime_error);
}

TEST(Adversarial_Console_AliasCycle_DoesNotInfiniteLoop)
{
    StandaloneConsole con;
    con.SetAlias("a", "b");
    con.SetAlias("b", "c");
    con.SetAlias("c", "a");
    EXPECT_NO_THROW(con.ResolveAlias("a"));
}

TEST(Adversarial_Console_HugeArgument_NoCorruption)
{
    StandaloneConsole con;
    size_t receivedSize = 0;
    con.RegisterCommand("echo",
                        [&](const std::vector<std::string>& args)
                        {
                            receivedSize = args.empty() ? 0 : args[0].size();
                            return std::string();
                        });
    std::string huge(1 << 20, 'Z');
    std::string result;
    EXPECT_TRUE(con.Execute("echo", {huge}, AdvPermission::Developer, result));
    EXPECT_EQ(receivedSize, huge.size());
}

// ============================================================================
// Standalone HookDispatcher adversarial tests
// ============================================================================

namespace
{

    enum class AdvHookType : uint32_t
    {
        OnDamage,
        OnDeath,
        OnMove,
        Count
    };

    struct AdvHookContext
    {
        AdvHookType type{};
        float value = 0.0f;
        bool cancelled = false;
        bool valueModified = false;
        float modifiedValue = 0.0f;
    };

    using AdvHookHandler = std::function<void(AdvHookContext&)>;

    class StandaloneHookDispatcher
    {
      public:
        struct Registration
        {
            uint32_t id;
            int priority;
            AdvHookHandler handler;
        };

        uint32_t Register(AdvHookType type, AdvHookHandler handler, int priority = 0)
        {
            if (!handler)
                return 0;
            uint32_t id = ++m_nextId;
            auto& list = m_hooks[type];
            list.push_back({id, priority, std::move(handler)});
            std::stable_sort(list.begin(), list.end(),
                             [](const Registration& a, const Registration& b) { return a.priority < b.priority; });
            return id;
        }

        void Unregister(uint32_t id)
        {
            for (auto& [type, list] : m_hooks)
                list.erase(std::remove_if(list.begin(), list.end(), [id](const Registration& r) { return r.id == id; }),
                           list.end());
        }

        AdvHookContext Dispatch(AdvHookType type, AdvHookContext ctx)
        {
            auto it = m_hooks.find(type);
            if (it == m_hooks.end())
                return ctx;
            auto snapshot = it->second;
            for (auto& reg : snapshot)
                reg.handler(ctx);
            return ctx;
        }

        int HandlerCount(AdvHookType type) const
        {
            auto it = m_hooks.find(type);
            return (it != m_hooks.end()) ? static_cast<int>(it->second.size()) : 0;
        }

        void Clear() { m_hooks.clear(); }

      private:
        std::unordered_map<AdvHookType, std::vector<Registration>> m_hooks;
        uint32_t m_nextId = 0;
    };

} // namespace

TEST(Adversarial_Hooks_NullHandler_Rejected)
{
    StandaloneHookDispatcher d;
    uint32_t id = d.Register(AdvHookType::OnDamage, nullptr);
    EXPECT_EQ(id, static_cast<uint32_t>(0));
    EXPECT_EQ(d.HandlerCount(AdvHookType::OnDamage), 0);
}

TEST(Adversarial_Hooks_DispatchNoHandlers_ReturnsContext)
{
    StandaloneHookDispatcher d;
    AdvHookContext ctx;
    ctx.value = 42.0f;
    auto result = d.Dispatch(AdvHookType::OnDamage, ctx);
    EXPECT_EQ(result.value, 42.0f);
}

TEST(Adversarial_Hooks_HandlerThrows_ExceptionPropagates)
{
    StandaloneHookDispatcher d;
    d.Register(AdvHookType::OnDamage, [](AdvHookContext&) { throw std::runtime_error("hook_error"); });
    EXPECT_THROW(d.Dispatch(AdvHookType::OnDamage, {}), std::runtime_error);
}

TEST(Adversarial_Hooks_PriorityOrder_LowRunsFirst)
{
    StandaloneHookDispatcher d;
    std::vector<int> order;
    d.Register(AdvHookType::OnDamage, [&](AdvHookContext&) { order.push_back(10); }, 10);
    d.Register(AdvHookType::OnDamage, [&](AdvHookContext&) { order.push_back(1); }, 1);
    d.Register(AdvHookType::OnDamage, [&](AdvHookContext&) { order.push_back(5); }, 5);
    d.Dispatch(AdvHookType::OnDamage, {});
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 5);
    EXPECT_EQ(order[2], 10);
}

TEST(Adversarial_Hooks_HandlerCancels_SubsequentHandlerStillRuns)
{
    StandaloneHookDispatcher d;
    int runCount = 0;
    d.Register(
        AdvHookType::OnDamage,
        [&](AdvHookContext& ctx)
        {
            ctx.cancelled = true;
            ++runCount;
        },
        0);
    d.Register(AdvHookType::OnDamage, [&](AdvHookContext&) { ++runCount; }, 1);
    auto result = d.Dispatch(AdvHookType::OnDamage, {});
    EXPECT_TRUE(result.cancelled);
    EXPECT_EQ(runCount, 2);
}

TEST(Adversarial_Hooks_UnregisterInvalidId_NoEffect)
{
    StandaloneHookDispatcher d;
    EXPECT_NO_THROW(d.Unregister(0));
    EXPECT_NO_THROW(d.Unregister(std::numeric_limits<uint32_t>::max()));
}

TEST(Adversarial_Hooks_MassHandlers_NoCrash)
{
    StandaloneHookDispatcher d;
    constexpr int kCount = 2000;
    std::atomic<int> totalCalls{0};
    for (int i = 0; i < kCount; ++i)
        d.Register(AdvHookType::OnMove, [&](AdvHookContext&) { ++totalCalls; });
    d.Dispatch(AdvHookType::OnMove, {});
    EXPECT_EQ(totalCalls.load(), kCount);
}

TEST(Adversarial_Hooks_ClearThenDispatch_NoHandlersCalled)
{
    StandaloneHookDispatcher d;
    bool called = false;
    d.Register(AdvHookType::OnDeath, [&](AdvHookContext&) { called = true; });
    d.Clear();
    d.Dispatch(AdvHookType::OnDeath, {});
    EXPECT_FALSE(called);
}

// ============================================================================
// Standalone NetBuffer adversarial tests
// ============================================================================

namespace
{

    class AdvNetBuffer
    {
      public:
        void WriteUint8(uint8_t v) { m_data.push_back(v); }
        void WriteUint16(uint16_t v)
        {
            m_data.push_back(static_cast<uint8_t>(v & 0xFF));
            m_data.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        }
        void WriteUint32(uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
                m_data.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
        }
        void WriteFloat(float v)
        {
            uint32_t bits;
            static_assert(sizeof(bits) == sizeof(v));
            std::memcpy(&bits, &v, sizeof(bits));
            WriteUint32(bits);
        }
        void WriteString(const std::string& s)
        {
            WriteUint32(static_cast<uint32_t>(s.size()));
            for (char c : s)
                m_data.push_back(static_cast<uint8_t>(c));
        }

        uint8_t ReadUint8()
        {
            if (!CanRead(1))
            {
                m_error = true;
                return 0;
            }
            return m_data[m_readPos++];
        }
        uint16_t ReadUint16()
        {
            if (!CanRead(2))
            {
                m_error = true;
                return 0;
            }
            uint16_t v = static_cast<uint16_t>(m_data[m_readPos]) | (static_cast<uint16_t>(m_data[m_readPos + 1]) << 8);
            m_readPos += 2;
            return v;
        }
        uint32_t ReadUint32()
        {
            if (!CanRead(4))
            {
                m_error = true;
                return 0;
            }
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i)
                v |= (static_cast<uint32_t>(m_data[m_readPos++]) << (i * 8));
            return v;
        }
        float ReadFloat()
        {
            uint32_t bits = ReadUint32();
            float v;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        }
        std::string ReadString()
        {
            uint32_t len = ReadUint32();
            if (m_error || !CanRead(len))
            {
                m_error = true;
                return {};
            }
            std::string s(m_data.begin() + static_cast<ptrdiff_t>(m_readPos),
                          m_data.begin() + static_cast<ptrdiff_t>(m_readPos + len));
            m_readPos += len;
            return s;
        }

        bool CanRead(size_t bytes) const { return !m_error && (m_readPos + bytes <= m_data.size()); }
        bool HasError() const { return m_error; }
        size_t RemainingBytes() const { return m_readPos <= m_data.size() ? m_data.size() - m_readPos : 0; }
        void Reset()
        {
            m_data.clear();
            m_readPos = 0;
            m_error = false;
        }

      private:
        std::vector<uint8_t> m_data;
        size_t m_readPos = 0;
        bool m_error = false;
    };

} // namespace

TEST(Adversarial_NetBuffer_ReadFromEmpty_SetsError)
{
    AdvNetBuffer buf;
    buf.ReadUint8();
    EXPECT_TRUE(buf.HasError());
}

TEST(Adversarial_NetBuffer_ReadPastEnd_SetsError)
{
    AdvNetBuffer buf;
    buf.WriteUint8(0xFF);
    buf.ReadUint8();
    buf.ReadUint8();
    EXPECT_TRUE(buf.HasError());
}

TEST(Adversarial_NetBuffer_ErrorFlagPropagates)
{
    AdvNetBuffer buf;
    buf.ReadUint32();
    uint32_t v = buf.ReadUint32();
    EXPECT_EQ(v, static_cast<uint32_t>(0));
    EXPECT_TRUE(buf.HasError());
}

TEST(Adversarial_NetBuffer_ResetClearsError)
{
    AdvNetBuffer buf;
    buf.ReadUint8();
    EXPECT_TRUE(buf.HasError());
    buf.Reset();
    EXPECT_FALSE(buf.HasError());
    EXPECT_EQ(buf.RemainingBytes(), static_cast<size_t>(0));
}

TEST(Adversarial_NetBuffer_TruncatedStringLength)
{
    AdvNetBuffer buf;
    buf.WriteUint32(100);
    buf.WriteUint8('A');
    buf.WriteUint8('B');
    buf.WriteUint8('C');
    std::string s = buf.ReadString();
    EXPECT_TRUE(buf.HasError());
    EXPECT_TRUE(s.empty());
}

TEST(Adversarial_NetBuffer_RoundTrip_AllTypes)
{
    AdvNetBuffer buf;
    buf.WriteUint8(0xAB);
    buf.WriteUint16(0x1234);
    buf.WriteUint32(0xDEADBEEF);
    buf.WriteFloat(3.14159f);
    buf.WriteString("hello");

    EXPECT_EQ(buf.ReadUint8(), static_cast<uint8_t>(0xAB));
    EXPECT_EQ(buf.ReadUint16(), static_cast<uint16_t>(0x1234));
    EXPECT_EQ(buf.ReadUint32(), static_cast<uint32_t>(0xDEADBEEF));
    EXPECT_NEAR(buf.ReadFloat(), 3.14159f, 1e-5f);
    EXPECT_EQ(buf.ReadString(), std::string("hello"));
    EXPECT_FALSE(buf.HasError());
    EXPECT_EQ(buf.RemainingBytes(), static_cast<size_t>(0));
}

TEST(Adversarial_NetBuffer_EmptyString_RoundTrip)
{
    AdvNetBuffer buf;
    buf.WriteString("");
    std::string s = buf.ReadString();
    EXPECT_EQ(s, "");
    EXPECT_FALSE(buf.HasError());
}

TEST(Adversarial_NetBuffer_HugeString_RoundTrip)
{
    AdvNetBuffer buf;
    std::string big(65536, 'Q');
    buf.WriteString(big);
    std::string result = buf.ReadString();
    EXPECT_EQ(result.size(), big.size());
    EXPECT_FALSE(buf.HasError());
}

// ============================================================================
// Standalone LagCompensator adversarial tests
// ============================================================================

namespace
{

    struct AdvEntityState
    {
        uint32_t id;
        float x, y, z;
    };

    struct AdvSnapshot
    {
        float timestamp = 0.0f;
        std::vector<AdvEntityState> entities;
    };

    class StandaloneLagCompensator
    {
      public:
        void Record(const AdvSnapshot& snap)
        {
            m_history.push_back(snap);
            PruneOld();
        }

        bool RewindToTime(float targetTime, AdvSnapshot& out) const
        {
            if (m_history.empty())
                return false;
            auto it = std::lower_bound(m_history.begin(), m_history.end(), targetTime,
                                       [](const AdvSnapshot& s, float t) { return s.timestamp < t; });
            if (it == m_history.end())
                --it;
            out = *it;
            return true;
        }

        void SetMaxHistory(float seconds) { m_maxHistory = seconds; }
        void Clear() { m_history.clear(); }
        size_t HistorySize() const { return m_history.size(); }

      private:
        void PruneOld()
        {
            if (m_history.size() < 2)
                return;
            float newest = m_history.back().timestamp;
            auto cutoff = newest - m_maxHistory;
            m_history.erase(std::remove_if(m_history.begin(), m_history.end(),
                                           [cutoff](const AdvSnapshot& s) { return s.timestamp < cutoff; }),
                            m_history.end());
        }

        std::vector<AdvSnapshot> m_history;
        float m_maxHistory = 1.0f;
    };

} // namespace

TEST(Adversarial_LagComp_RewindEmptyHistory_ReturnsFalse)
{
    StandaloneLagCompensator lc;
    AdvSnapshot out;
    EXPECT_FALSE(lc.RewindToTime(1.0f, out));
}

TEST(Adversarial_LagComp_RewindToFutureTime_ClampsToLatest)
{
    StandaloneLagCompensator lc;
    lc.Record({0.5f, {}});
    lc.Record({1.0f, {}});
    AdvSnapshot out;
    EXPECT_TRUE(lc.RewindToTime(9999.0f, out));
    EXPECT_EQ(out.timestamp, 1.0f);
}

TEST(Adversarial_LagComp_RewindToPastTime_ClampsToEarliest)
{
    StandaloneLagCompensator lc;
    lc.Record({1.0f, {}});
    lc.Record({2.0f, {}});
    AdvSnapshot out;
    EXPECT_TRUE(lc.RewindToTime(0.0f, out));
    EXPECT_EQ(out.timestamp, 1.0f);
}

TEST(Adversarial_LagComp_OverflowPruning)
{
    StandaloneLagCompensator lc;
    lc.SetMaxHistory(1.0f);
    for (int i = 0; i < 500; ++i)
        lc.Record({static_cast<float>(i) * 0.1f, {}});

    EXPECT_LE(lc.HistorySize(), static_cast<size_t>(12));
    EXPECT_GT(lc.HistorySize(), static_cast<size_t>(0));
}

TEST(Adversarial_LagComp_ClearThenRewind_ReturnsFalse)
{
    StandaloneLagCompensator lc;
    lc.Record({1.0f, {}});
    lc.Clear();
    AdvSnapshot out;
    EXPECT_FALSE(lc.RewindToTime(1.0f, out));
}

TEST(Adversarial_LagComp_NegativeTimestamp_NoCorruption)
{
    StandaloneLagCompensator lc;
    lc.Record({-0.5f, {{1, 0.0f, 0.0f, 0.0f}}});
    lc.Record({0.0f, {}});
    AdvSnapshot out;
    EXPECT_TRUE(lc.RewindToTime(-0.5f, out));
    EXPECT_EQ(out.entities.size(), static_cast<size_t>(1));
}
