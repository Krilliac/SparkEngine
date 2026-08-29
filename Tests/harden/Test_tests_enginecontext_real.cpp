/**
 * @file Test_tests_enginecontext_real.cpp
 * @brief Real-class tests for the dependency-aware subsystem init in EngineContext.
 *
 * TestEngineContext.cpp exercises a standalone ServiceLocator reimplementation,
 * so the shipped EngineContext (its Kahn's-algorithm TopologicalSort and
 * RegisterSubsystem/InitializeAll/ShutdownAll flow) has no direct coverage.
 * These tests drive a local EngineContext instance (default-constructed, so it
 * never touches the process-wide singleton) and assert: deterministic
 * dependency-before-dependent init order, reverse-order shutdown, cycle
 * detection returning false, and replace-existing-entry semantics.
 */

#include "TestFramework.h"
#include "Core/EngineContext.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <initializer_list>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // Distinct, complete tag types used as fake subsystems. Each gets its own
    // GetTypeId<T>() marker, exactly like a real subsystem class.
    struct SysA
    {
    };
    struct SysB
    {
    };
    struct SysC
    {
    };
    struct SysD
    {
    };

    void ExpectLifecycleEvents(const std::vector<std::string>& actual, std::initializer_list<const char*> expected)
    {
        EXPECT_EQ(actual.size(), expected.size());
        size_t index = 0;
        for (const char* value : expected)
        {
            if (index < actual.size())
                EXPECT_EQ(actual[index], std::string(value));
            ++index;
        }
    }
} // namespace

TEST(EngineContextReal_RegisterSystemNullptrUnregisters)
{
    EngineContext ctx;
    SysA a;

    ctx.RegisterSystem<SysA>(&a);
    ASSERT_TRUE(ctx.GetSystem<SysA>() == &a);

    ctx.RegisterSystem<SysA>(nullptr);
    EXPECT_TRUE(ctx.GetSystem<SysA>() == nullptr);
}

TEST(EngineContextReal_DeterministicInitAndReverseShutdown)
{
    EngineContext ctx;
    std::vector<std::string> initLog;
    std::vector<std::string> shutLog;
    SysA a;
    SysB b;
    SysC c;

    // A depends on B, B depends on C — a linear chain with a unique topological
    // order: init C, B, A and shut down A, B, C.
    ctx.RegisterSubsystem<SysA>(
        &a, DependsOn<SysB>{},
        [&]()
        {
            initLog.push_back("A");
            return true;
        },
        [&]() { shutLog.push_back("A"); });
    ctx.RegisterSubsystem<SysB>(
        &b, DependsOn<SysC>{},
        [&]()
        {
            initLog.push_back("B");
            return true;
        },
        [&]() { shutLog.push_back("B"); });
    ctx.RegisterSubsystem<SysC>(
        &c, DependsOn<>{},
        [&]()
        {
            initLog.push_back("C");
            return true;
        },
        [&]() { shutLog.push_back("C"); });

    EXPECT_TRUE(ctx.InitializeAll());
    EXPECT_EQ(initLog.size(), static_cast<size_t>(3));
    if (initLog.size() == 3)
    {
        EXPECT_EQ(initLog[0], std::string("C"));
        EXPECT_EQ(initLog[1], std::string("B"));
        EXPECT_EQ(initLog[2], std::string("A"));
    }

    ctx.ShutdownAll();
    EXPECT_EQ(shutLog.size(), static_cast<size_t>(3));
    if (shutLog.size() == 3)
    {
        EXPECT_EQ(shutLog[0], std::string("A"));
        EXPECT_EQ(shutLog[1], std::string("B"));
        EXPECT_EQ(shutLog[2], std::string("C"));
    }
}

TEST(EngineContextReal_CycleDetectionReturnsFalse)
{
    EngineContext ctx;
    std::vector<std::string> events;
    SysA a;
    SysB b;

    // A depends on B and B depends on A — a cycle; InitializeAll must fail.
    ctx.RegisterSubsystem<SysA>(
        &a, DependsOn<SysB>{},
        [&]()
        {
            events.push_back("init A");
            return true;
        },
        nullptr);
    ctx.RegisterSubsystem<SysB>(
        &b, DependsOn<SysA>{},
        [&]()
        {
            events.push_back("init B");
            return true;
        },
        nullptr);

    EXPECT_FALSE(ctx.InitializeAll());
    EXPECT_TRUE(events.empty());
    EXPECT_TRUE(ctx.GetInitOrder().empty());

    // Cycle failure leaves the graph idle and repairable.
    ctx.RegisterSubsystem<SysB>(
        &b, DependsOn<>{},
        [&]()
        {
            events.push_back("init B");
            return true;
        },
        nullptr);
    EXPECT_TRUE(ctx.InitializeAll());
    ExpectLifecycleEvents(events, {"init B", "init A"});
    ctx.ShutdownAll();
    EXPECT_TRUE(ctx.GetInitOrder().empty());
}

TEST(EngineContextReal_ReplaceExistingEntry)
{
    EngineContext ctx;
    std::vector<std::string> initLog;
    SysA a1;
    SysA a2;

    // Registering the same type twice replaces the first entry — only the
    // second registration's init callback should fire, exactly once.
    ctx.RegisterSubsystem<SysA>(
        &a1, DependsOn<>{},
        [&]()
        {
            initLog.push_back("A1");
            return true;
        },
        nullptr);
    ctx.RegisterSubsystem<SysA>(
        &a2, DependsOn<>{},
        [&]()
        {
            initLog.push_back("A2");
            return true;
        },
        nullptr);

    EXPECT_TRUE(ctx.InitializeAll());
    EXPECT_EQ(initLog.size(), static_cast<size_t>(1));
    if (initLog.size() == 1)
        EXPECT_EQ(initLog[0], std::string("A2"));
}

TEST(PartialInit_EngineContextFalseFailureRollsBackAndRetries)
{
    EngineContext ctx;
    std::vector<std::string> events;
    bool failB = true;
    SysA a;
    SysB b;
    SysC c;

    ctx.RegisterSubsystem<SysA>(
        &a, DependsOn<>{},
        [&]()
        {
            events.push_back("init A");
            return true;
        },
        [&]() { events.push_back("shutdown A"); });
    ctx.RegisterSubsystem<SysB>(
        &b, DependsOn<SysA>{},
        [&]()
        {
            events.push_back("init B");
            return !failB;
        },
        [&]() { events.push_back("shutdown B"); });
    ctx.RegisterSubsystem<SysC>(
        &c, DependsOn<SysB>{},
        [&]()
        {
            events.push_back("init C");
            return true;
        },
        [&]() { events.push_back("shutdown C"); });

    EXPECT_FALSE(ctx.InitializeAll());
    ExpectLifecycleEvents(events, {"init A", "init B", "shutdown B", "shutdown A"});
    EXPECT_TRUE(ctx.GetInitOrder().empty());
    EXPECT_FALSE(ctx.HasLifecycleFailure());
    EXPECT_TRUE(ctx.GetSystem<SysA>() == &a);
    EXPECT_TRUE(ctx.GetSystem<SysB>() == &b);
    EXPECT_TRUE(ctx.GetSystem<SysC>() == &c);

    failB = false;
    events.clear();
    EXPECT_TRUE(ctx.InitializeAll());
    ExpectLifecycleEvents(events, {"init A", "init B", "init C"});
    const size_t initializedEventCount = events.size();
    EXPECT_TRUE(ctx.InitializeAll());
    EXPECT_EQ(events.size(), initializedEventCount);

    ctx.ShutdownAll();
    ExpectLifecycleEvents(events, {"init A", "init B", "init C", "shutdown C", "shutdown B", "shutdown A"});
    EXPECT_TRUE(ctx.GetInitOrder().empty());
    const size_t eventCount = events.size();
    ctx.ShutdownAll();
    EXPECT_EQ(events.size(), eventCount);
}

TEST(PartialInit_EngineContextInitExceptionRollsBackAndRetries)
{
    EngineContext ctx;
    std::vector<std::string> events;
    int throwMode = 0;
    SysA a;
    SysB b;
    SysC c;

    ctx.RegisterSubsystem<SysA>(
        &a, DependsOn<>{},
        [&]()
        {
            events.push_back("init A");
            return true;
        },
        [&]() { events.push_back("shutdown A"); });
    ctx.RegisterSubsystem<SysB>(
        &b, DependsOn<SysA>{},
        [&]()
        {
            events.push_back("init B");
            if (throwMode == 0)
                throw std::runtime_error("expected init failure");
            if (throwMode == 1)
                throw 7;
            return true;
        },
        [&]() { events.push_back("shutdown B"); });
    ctx.RegisterSubsystem<SysC>(
        &c, DependsOn<SysB>{},
        [&]()
        {
            events.push_back("init C");
            return true;
        },
        [&]() { events.push_back("shutdown C"); });

    EXPECT_FALSE(ctx.InitializeAll());
    ExpectLifecycleEvents(events, {"init A", "init B", "shutdown B", "shutdown A"});
    EXPECT_TRUE(ctx.GetInitOrder().empty());

    throwMode = 1;
    events.clear();
    EXPECT_FALSE(ctx.InitializeAll());
    ExpectLifecycleEvents(events, {"init A", "init B", "shutdown B", "shutdown A"});
    EXPECT_TRUE(ctx.GetInitOrder().empty());

    throwMode = 2;
    events.clear();
    EXPECT_TRUE(ctx.InitializeAll());
    ExpectLifecycleEvents(events, {"init A", "init B", "init C"});
    EXPECT_TRUE(ctx.GetSystem<SysA>() == &a);
    EXPECT_TRUE(ctx.GetSystem<SysB>() == &b);
    EXPECT_TRUE(ctx.GetSystem<SysC>() == &c);
    ctx.ShutdownAll();
    ExpectLifecycleEvents(events, {"init A", "init B", "init C", "shutdown C", "shutdown B", "shutdown A"});

    EngineContext quarantined;
    std::vector<std::string> quarantineEvents;
    SysA quarantineA;
    SysB quarantineB;
    quarantined.RegisterSubsystem<SysA>(
        &quarantineA, DependsOn<>{},
        [&]()
        {
            quarantineEvents.push_back("init A");
            return true;
        },
        [&]() { quarantineEvents.push_back("shutdown A"); });
    quarantined.RegisterSubsystem<SysB>(
        &quarantineB, DependsOn<SysA>{},
        [&]()
        {
            quarantineEvents.push_back("init B");
            return false;
        },
        [&]()
        {
            quarantineEvents.push_back("rollback B");
            throw std::runtime_error("expected rollback failure");
        });
    EXPECT_FALSE(quarantined.InitializeAll());
    ExpectLifecycleEvents(quarantineEvents, {"init A", "init B", "rollback B", "shutdown A"});
    EXPECT_TRUE(quarantined.HasLifecycleFailure());
    EXPECT_TRUE(quarantined.GetSystem<SysA>() == nullptr);
    EXPECT_TRUE(quarantined.GetSystem<SysB>() == nullptr);
    EXPECT_FALSE(quarantined.RegisterSystem<SysA>(nullptr));
    const size_t quarantinedEventCount = quarantineEvents.size();
    EXPECT_FALSE(quarantined.InitializeAll());
    EXPECT_EQ(quarantineEvents.size(), quarantinedEventCount);
}

TEST(PartialInit_EngineContextShutdownExceptionQuarantinesContext)
{
    EngineContext ctx;
    std::vector<std::string> events;
    bool throwFromBShutdown = true;
    SysA a;
    SysB b;
    SysC c;

    ctx.RegisterSubsystem<SysA>(
        &a, DependsOn<>{},
        [&]()
        {
            events.push_back("init A");
            return true;
        },
        [&]() { events.push_back("shutdown A"); });
    ctx.RegisterSubsystem<SysB>(
        &b, DependsOn<SysA>{},
        [&]()
        {
            events.push_back("init B");
            return true;
        },
        [&]()
        {
            events.push_back("shutdown B");
            if (throwFromBShutdown)
                throw std::runtime_error("expected shutdown failure");
        });
    ctx.RegisterSubsystem<SysC>(
        &c, DependsOn<SysB>{},
        [&]()
        {
            events.push_back("init C");
            return true;
        },
        [&]() { events.push_back("shutdown C"); });

    EXPECT_TRUE(ctx.InitializeAll());
    ctx.ShutdownAll();
    ExpectLifecycleEvents(events, {"init A", "init B", "init C", "shutdown C", "shutdown B", "shutdown A"});
    EXPECT_TRUE(ctx.GetInitOrder().empty());
    EXPECT_TRUE(ctx.HasLifecycleFailure());
    EXPECT_TRUE(ctx.GetSystem<SysA>() == nullptr);
    EXPECT_TRUE(ctx.GetSystem<SysB>() == nullptr);
    EXPECT_TRUE(ctx.GetSystem<SysC>() == nullptr);
    EXPECT_FALSE(ctx.RegisterSystem<SysB>(nullptr));

    const size_t firstShutdownEventCount = events.size();
    ctx.ShutdownAll();
    EXPECT_EQ(events.size(), firstShutdownEventCount);

    const size_t quarantinedEventCount = events.size();
    EXPECT_FALSE(ctx.InitializeAll());
    ctx.ShutdownAll();
    EXPECT_EQ(events.size(), quarantinedEventCount);
}

TEST(PartialInit_EngineContextRejectsLifecycleGraphMutation)
{
    EngineContext ctx;
    std::vector<std::string> events;
    SysA a;
    SysA replacementA;
    SysB originalB;
    SysB replacementB;
    SysD d;
    bool initWorkerReturned = false;
    bool initWorkerResult = true;
    bool shutdownWorkerReturned = false;

    ctx.RegisterSubsystem<SysA>(
        &a, DependsOn<>{},
        [&]()
        {
            events.push_back("init A");
            std::thread worker(
                [&]()
                {
                    initWorkerResult = ctx.InitializeAll();
                    initWorkerReturned = true;
                });
            worker.join();
            EXPECT_FALSE(ctx.InitializeAll());
            ctx.ShutdownAll();
            EXPECT_FALSE(ctx.RegisterSystem<SysB>(&replacementB));
            EXPECT_FALSE(ctx.RegisterSubsystem<SysB>(
                &replacementB, DependsOn<SysA>{},
                [&]()
                {
                    events.push_back("init replacement B");
                    return true;
                },
                [&]() { events.push_back("shutdown replacement B"); }));
            EXPECT_FALSE(ctx.RegisterSubsystem<SysD>(
                &d, DependsOn<SysB>{},
                [&]()
                {
                    events.push_back("init D");
                    return true;
                },
                [&]() { events.push_back("shutdown D"); }));
            return true;
        },
        [&]() { events.push_back("shutdown A"); });
    ctx.RegisterSubsystem<SysB>(
        &originalB, DependsOn<SysA>{},
        [&]()
        {
            events.push_back("init original B");
            return true;
        },
        [&]()
        {
            events.push_back("shutdown original B");
            std::thread worker(
                [&]()
                {
                    ctx.ShutdownAll();
                    shutdownWorkerReturned = true;
                });
            worker.join();
            EXPECT_FALSE(ctx.InitializeAll());
            ctx.ShutdownAll();
            EXPECT_FALSE(ctx.RegisterSystem<SysA>(&replacementA));
            EXPECT_FALSE(ctx.RegisterSubsystem<SysD>(
                &d, DependsOn<SysB>{},
                [&]()
                {
                    events.push_back("init D");
                    return true;
                },
                [&]() { events.push_back("shutdown D"); }));
        });

    EXPECT_TRUE(ctx.InitializeAll());
    EXPECT_TRUE(initWorkerReturned);
    EXPECT_FALSE(initWorkerResult);
    ExpectLifecycleEvents(events, {"init A", "init original B"});
    EXPECT_EQ(ctx.GetSubsystemCount(), static_cast<size_t>(2));
    EXPECT_TRUE(ctx.GetSystem<SysB>() == &originalB);
    EXPECT_TRUE(ctx.GetSystem<SysD>() == nullptr);
    ctx.ShutdownAll();
    EXPECT_TRUE(shutdownWorkerReturned);
    ExpectLifecycleEvents(events, {"init A", "init original B", "shutdown original B", "shutdown A"});
    EXPECT_TRUE(ctx.GetSystem<SysA>() == &a);

    // Once idle, both replacement and addition are accepted.
    EXPECT_TRUE(ctx.RegisterSubsystem<SysB>(
        &replacementB, DependsOn<SysA>{},
        [&]()
        {
            events.push_back("init replacement B");
            return true;
        },
        [&]() { events.push_back("shutdown replacement B"); }));
    EXPECT_TRUE(ctx.RegisterSubsystem<SysD>(
        &d, DependsOn<SysB>{},
        [&]()
        {
            events.push_back("init D");
            return true;
        },
        [&]() { events.push_back("shutdown D"); }));
    EXPECT_EQ(ctx.GetSubsystemCount(), static_cast<size_t>(3));
    EXPECT_TRUE(ctx.GetSystem<SysB>() == &replacementB);
    EXPECT_TRUE(ctx.GetSystem<SysD>() == &d);

    events.clear();
    EXPECT_TRUE(ctx.InitializeAll());
    ExpectLifecycleEvents(events, {"init A", "init replacement B", "init D"});
    ctx.ShutdownAll();
    ExpectLifecycleEvents(
        events, {"init A", "init replacement B", "init D", "shutdown D", "shutdown replacement B", "shutdown A"});

    // Concurrent lifecycle calls fail fast, while graph mutation is rejected
    // instead of racing the pointer snapshot held by InitializeAll().
    EngineContext concurrent;
    SysA concurrentA;
    SysD concurrentD;
    std::mutex gateMutex;
    std::condition_variable gateChanged;
    bool initEntered = false;
    bool releaseInit = false;
    std::atomic<int> initCalls{0};
    std::atomic<int> shutdownCalls{0};
    bool firstInitResult = false;
    bool secondInitResult = false;
    bool secondInitFinished = false;

    EXPECT_TRUE(concurrent.RegisterSubsystem<SysA>(
        &concurrentA, DependsOn<>{},
        [&]()
        {
            {
                std::lock_guard<std::mutex> lock(gateMutex);
                initEntered = true;
            }
            gateChanged.notify_all();
            std::unique_lock<std::mutex> lock(gateMutex);
            gateChanged.wait(lock, [&]() { return releaseInit; });
            ++initCalls;
            return true;
        },
        [&]() { ++shutdownCalls; }));

    std::thread firstInitializer([&]() { firstInitResult = concurrent.InitializeAll(); });
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        gateChanged.wait(lock, [&]() { return initEntered; });
    }
    EXPECT_FALSE(concurrent.RegisterSubsystem<SysD>(&concurrentD, DependsOn<SysA>{}));
    std::thread secondInitializer(
        [&]()
        {
            secondInitResult = concurrent.InitializeAll();
            {
                std::lock_guard<std::mutex> lock(gateMutex);
                secondInitFinished = true;
            }
            gateChanged.notify_all();
        });
    bool secondFinishedBeforeRelease = false;
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        secondFinishedBeforeRelease =
            gateChanged.wait_for(lock, std::chrono::seconds(5), [&]() { return secondInitFinished; });
        releaseInit = true;
    }
    gateChanged.notify_all();
    firstInitializer.join();
    secondInitializer.join();

    EXPECT_TRUE(firstInitResult);
    EXPECT_TRUE(secondFinishedBeforeRelease);
    EXPECT_FALSE(secondInitResult);
    EXPECT_EQ(initCalls.load(), 1);
    EXPECT_EQ(concurrent.GetSubsystemCount(), static_cast<size_t>(1));
    EXPECT_TRUE(concurrent.InitializeAll());

    std::thread firstShutdown([&]() { concurrent.ShutdownAll(); });
    std::thread secondShutdown([&]() { concurrent.ShutdownAll(); });
    firstShutdown.join();
    secondShutdown.join();
    EXPECT_EQ(shutdownCalls.load(), 1);
    EXPECT_FALSE(concurrent.HasLifecycleFailure());
}
