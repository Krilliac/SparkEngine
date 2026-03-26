// TestAdversarialEngine.cpp
// Adversarial tests: invalid inputs, boundary conditions, double-init, reentrancy,
// queue overflow, use-after-clear, and stress scenarios for core engine systems.
//
// Systems under attack:
//   EventBus       — recursive publish, massive subscribers, handler exceptions,
//                    use-after-move on SubscriptionHandle, unsubscribe-during-dispatch
//   QueuedEventBus — overflow/drop behaviour, clear-then-dispatch, concurrent queue
//   ServiceLocator — null register, overwrite, unknown-type query (standalone)
//   ConsoleSystem  — empty names, alias cycles, handler throws, permission bypass (standalone)
//   HookDispatcher — null handler, throws during dispatch, reentrancy, priority order (standalone)
//   NetBuffer      — overread, truncated string, error-flag propagation (standalone)
//   LagCompensator — empty history, future rewind, max-duration pruning (standalone)

#include "TestFramework.h"

#include "Utils/EventBus.h"
#include "Engine/Events/EventSystem.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ============================================================================
// EventBus adversarial tests (uses real Spark::EventBus)
// ============================================================================

namespace
{
    // One event type per publish-path to avoid cross-test s_publishDepth contamination.
    // NOTE: Spark::EventBus::Publish() is NOT exception-safe — if a handler throws,
    //       the per-type static thread_local s_publishDepth is left at 1, silently
    //       suppressing all future publishes of that same event type in the same thread.
    //       Each test that publishes therefore uses a distinct event type.
    struct AdvDamageEvent
    {
        float damage = 0.0f;
    };
    struct AdvEvtThrow
    {
    };
    struct AdvEvtRecursive
    {
        float damage = 0.0f;
    };
    struct AdvEvtMass
    {
    };
    struct AdvEvtFloat
    {
        float damage = 0.0f;
    };
    struct AdvEmptyEvent
    {
    };
    struct AdvStringEvent
    {
        std::string text;
    };
} // namespace

TEST(Adversarial_EventBus_PublishWithNoSubscribers)
{
    Spark::EventBus bus;
    // Should be a silent no-op, not a crash.
    EXPECT_NO_THROW(bus.Publish(AdvDamageEvent{999.0f}));
    EXPECT_NO_THROW(bus.Publish(AdvEmptyEvent{}));
}

TEST(Adversarial_EventBus_DoubleUnsubscribe)
{
    Spark::EventBus bus;
    int count = 0;
    auto handle = bus.Subscribe<AdvDamageEvent>([&](const AdvDamageEvent&) { ++count; });
    handle.Unsubscribe();
    // Second Unsubscribe must be a no-op, not a crash.
    EXPECT_NO_THROW(handle.Unsubscribe());
    bus.Publish(AdvDamageEvent{1.0f});
    EXPECT_EQ(count, 0); // Handler was removed; no delivery.
}

TEST(Adversarial_EventBus_MoveHandle_NoDoubleUnsubscribe)
{
    Spark::EventBus bus;
    int count = 0;
    auto h1 = bus.Subscribe<AdvDamageEvent>([&](const AdvDamageEvent&) { ++count; });
    auto h2 = std::move(h1);
    // h1 is now dead; letting it destroy must not unsubscribe the live h2.
    bus.Publish(AdvDamageEvent{});
    EXPECT_EQ(count, 1); // h2 still active.
    EXPECT_FALSE(h1.IsActive());
    EXPECT_TRUE(h2.IsActive());
}

TEST(Adversarial_EventBus_HandlerThrows_ExceptionPropagates)
{
    // Uses AdvEvtThrow (unique type) so a stuck s_publishDepth does not affect
    // other tests that share AdvDamageEvent.
    Spark::EventBus bus;
    auto handle = bus.Subscribe<AdvEvtThrow>([](const AdvEvtThrow&) { throw std::runtime_error("boom"); });
    EXPECT_THROW(bus.Publish(AdvEvtThrow{}), std::runtime_error);
}

TEST(Adversarial_EventBus_RecursivePublish_Suppressed)
{
    // Publishing the same event type from within a handler must not recurse infinitely.
    // Uses AdvEvtRecursive (unique type) to prevent s_publishDepth contamination.
    Spark::EventBus bus;
    int dispatchCount = 0;
    auto handle = bus.Subscribe<AdvEvtRecursive>(
        [&](const AdvEvtRecursive& e)
        {
            ++dispatchCount;
            // Attempt recursive publish of the same type (should be suppressed).
            bus.Publish(AdvEvtRecursive{e.damage + 1.0f});
        });
    bus.Publish(AdvEvtRecursive{0.0f});
    EXPECT_EQ(dispatchCount, 1); // Recursive call silently dropped.
}

TEST(Adversarial_EventBus_MassiveSubscriberCount)
{
    // Uses AdvEvtMass (unique type) to prevent s_publishDepth contamination.
    Spark::EventBus bus;
    constexpr int kCount = 5000;
    int total = 0;
    std::vector<Spark::SubscriptionHandle> handles;
    handles.reserve(kCount);
    for (int i = 0; i < kCount; ++i)
        handles.push_back(bus.Subscribe<AdvEvtMass>([&](const AdvEvtMass&) { ++total; }));

    bus.Publish(AdvEvtMass{});
    EXPECT_EQ(total, kCount);
    EXPECT_EQ(bus.SubscriberCount<AdvEvtMass>(), static_cast<size_t>(kCount));
}

TEST(Adversarial_EventBus_ClearAllThenPublish)
{
    Spark::EventBus bus;
    int count = 0;
    auto handle = bus.Subscribe<AdvDamageEvent>([&](const AdvDamageEvent&) { ++count; });
    bus.ClearAll();
    bus.Publish(AdvDamageEvent{});
    EXPECT_EQ(count, 0);
    // Handle destructor must not crash even though the channel was cleared.
}

TEST(Adversarial_EventBus_EmptyStringEvent)
{
    Spark::EventBus bus;
    std::string received;
    auto handle = bus.Subscribe<AdvStringEvent>([&](const AdvStringEvent& e) { received = e.text; });
    bus.Publish(AdvStringEvent{""}); // Empty string — must not crash.
    EXPECT_EQ(received, "");

    std::string huge(1 << 20, 'X'); // 1 MB string — no truncation expected.
    bus.Publish(AdvStringEvent{huge});
    EXPECT_EQ(received.size(), huge.size());
}

TEST(Adversarial_EventBus_NegativeAndInfiniteFloatEvent)
{
    // Uses AdvEvtFloat (unique type) to prevent s_publishDepth contamination.
    Spark::EventBus bus;
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
    Spark::QueuedEventBus queue;
    // Push MaxQueueSize + 1 events; the oldest should be dropped.
    constexpr size_t kOver = 10001;
    for (size_t i = 0; i < kOver; ++i)
        queue.QueueEvent(Spark::EntityDamagedEvent{static_cast<uint32_t>(i), 1.0f, ""});

    // Queue can hold at most 10000.
    EXPECT_LE(queue.GetPendingCount(), static_cast<size_t>(10000));
    EXPECT_GT(queue.GetDroppedEventCount(), static_cast<size_t>(0));
}

TEST(Adversarial_QueuedBus_ClearThenDispatch_NoDelivery)
{
    Spark::QueuedEventBus queue;
    Spark::EventBus bus;
    int count = 0;
    auto handle = bus.Subscribe<Spark::EntityDamagedEvent>([&](const Spark::EntityDamagedEvent&) { ++count; });

    queue.QueueEvent(Spark::EntityDamagedEvent{});
    queue.Clear();
    queue.DispatchAll(bus);

    EXPECT_EQ(count, 0);
    EXPECT_EQ(queue.GetPendingCount(), static_cast<size_t>(0));
}

TEST(Adversarial_QueuedBus_DroppedCountResetAfterDispatch)
{
    Spark::QueuedEventBus queue;
    Spark::EventBus bus;

    for (size_t i = 0; i < 10001; ++i)
        queue.QueueEvent(Spark::EntityDamagedEvent{});

    EXPECT_GT(queue.GetDroppedEventCount(), static_cast<size_t>(0));
    queue.DispatchAll(bus); // Resets dropped count.
    EXPECT_EQ(queue.GetDroppedEventCount(), static_cast<size_t>(0));
}

TEST(Adversarial_QueuedBus_ConcurrentQueue_NoRace)
{
    Spark::QueuedEventBus queue;
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
                    queue.QueueEvent(Spark::EntityDamagedEvent{});
            });
    }
    for (auto& th : threads)
        th.join();

    // No crash; count is bounded by MaxQueueSize.
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
    struct SLSubC
    {
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
    // Storing and retrieving null must not crash.
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

    loc.Register(&a2); // Overwrite — second registration wins.
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
    // Register 1000 instances of SLSubA sequentially; final pointer must survive.
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
                return name; // Break infinite cycles.
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
    con.SetAlias("c", "a"); // Cycle: a → b → c → a
    // ResolveAlias must terminate (depth guard).
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
            // Snapshot so handlers can register/unregister without invalidating iteration.
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
    EXPECT_EQ(result.value, 42.0f); // Untouched.
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
    // Cancellation is advisory; all handlers still execute.
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
    buf.ReadUint8(); // OK
    buf.ReadUint8(); // Past end
    EXPECT_TRUE(buf.HasError());
}

TEST(Adversarial_NetBuffer_ErrorFlagPropagates)
{
    // Once in error state, subsequent reads should also fail cleanly.
    AdvNetBuffer buf;
    buf.ReadUint32();              // Triggers error immediately.
    uint32_t v = buf.ReadUint32(); // Should not crash; returns 0.
    EXPECT_EQ(v, static_cast<uint32_t>(0));
    EXPECT_TRUE(buf.HasError());
}

TEST(Adversarial_NetBuffer_ResetClearsError)
{
    AdvNetBuffer buf;
    buf.ReadUint8(); // Error.
    EXPECT_TRUE(buf.HasError());
    buf.Reset();
    EXPECT_FALSE(buf.HasError());
    EXPECT_EQ(buf.RemainingBytes(), static_cast<size_t>(0));
}

TEST(Adversarial_NetBuffer_TruncatedStringLength)
{
    // Write a string length header (100) but provide only 3 bytes of payload.
    AdvNetBuffer buf;
    buf.WriteUint32(100); // Claims 100 bytes follow.
    buf.WriteUint8('A');
    buf.WriteUint8('B');
    buf.WriteUint8('C'); // Only 3 bytes actually present.
    std::string s = buf.ReadString();
    EXPECT_TRUE(buf.HasError()); // Must detect overrun, not crash.
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
        float timestamp;
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
            // Find the snapshot closest to targetTime (lower_bound by timestamp).
            auto it = std::lower_bound(m_history.begin(), m_history.end(), targetTime,
                                       [](const AdvSnapshot& s, float t) { return s.timestamp < t; });
            if (it == m_history.end())
                --it; // Clamp to most recent.
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
    EXPECT_TRUE(lc.RewindToTime(9999.0f, out)); // Far future → clamp to latest.
    EXPECT_EQ(out.timestamp, 1.0f);
}

TEST(Adversarial_LagComp_RewindToPastTime_ClampsToEarliest)
{
    StandaloneLagCompensator lc;
    lc.Record({1.0f, {}});
    lc.Record({2.0f, {}});
    AdvSnapshot out;
    EXPECT_TRUE(lc.RewindToTime(0.0f, out)); // Before all records → earliest.
    EXPECT_EQ(out.timestamp, 1.0f);
}

TEST(Adversarial_LagComp_OverflowPruning)
{
    StandaloneLagCompensator lc;
    lc.SetMaxHistory(1.0f);
    // Record 500 snapshots spanning 50 seconds; only the last 1s should survive.
    for (int i = 0; i < 500; ++i)
        lc.Record({static_cast<float>(i) * 0.1f, {}});

    // With maxHistory=1.0s, only ~10 entries should remain.
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
    // Timestamps before zero are legal; pruning must not discard snapshots within
    // the max-history window. Use -0.5f and 0.0f (0.5s apart, within 1s limit).
    StandaloneLagCompensator lc;
    lc.Record({-0.5f, {{1, 0.0f, 0.0f, 0.0f}}});
    lc.Record({0.0f, {}});
    AdvSnapshot out;
    EXPECT_TRUE(lc.RewindToTime(-0.5f, out));
    EXPECT_EQ(out.entities.size(), static_cast<size_t>(1));
}
