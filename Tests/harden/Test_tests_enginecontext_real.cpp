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

#include <string>
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
} // namespace

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
        &a, DependsOn<SysB>{}, [&]() { initLog.push_back("A"); return true; },
        [&]() { shutLog.push_back("A"); });
    ctx.RegisterSubsystem<SysB>(
        &b, DependsOn<SysC>{}, [&]() { initLog.push_back("B"); return true; },
        [&]() { shutLog.push_back("B"); });
    ctx.RegisterSubsystem<SysC>(
        &c, DependsOn<>{}, [&]() { initLog.push_back("C"); return true; },
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
    SysA a;
    SysB b;

    // A depends on B and B depends on A — a cycle; InitializeAll must fail.
    ctx.RegisterSubsystem<SysA>(&a, DependsOn<SysB>{}, []() { return true; }, nullptr);
    ctx.RegisterSubsystem<SysB>(&b, DependsOn<SysA>{}, []() { return true; }, nullptr);

    EXPECT_FALSE(ctx.InitializeAll());
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
        &a1, DependsOn<>{}, [&]() { initLog.push_back("A1"); return true; }, nullptr);
    ctx.RegisterSubsystem<SysA>(
        &a2, DependsOn<>{}, [&]() { initLog.push_back("A2"); return true; }, nullptr);

    EXPECT_TRUE(ctx.InitializeAll());
    EXPECT_EQ(initLog.size(), static_cast<size_t>(1));
    if (initLog.size() == 1)
        EXPECT_EQ(initLog[0], std::string("A2"));
}
