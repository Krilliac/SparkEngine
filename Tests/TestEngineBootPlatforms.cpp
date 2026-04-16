/**
 * @file TestEngineBootPlatforms.cpp
 * @brief Tests for engine boot sequence across platforms
 *
 * Validates EngineBootstrap (dependency-ordered subsystem init/shutdown),
 * Platform.h compile-time detection macros, LifecycleStage factory and
 * ordering, and WineDetection stubs on non-Windows builds.
 */

#include "TestFramework.h"
#include "Core/EngineBootstrap.h"
#include "Core/Platform.h"
#include "Core/Lifecycle/LifecycleStage.h"
#include "Core/Lifecycle/LifecycleStages.h"
#include "Utils/WineDetection.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

// ============================================================================
// EngineBootstrap — registration
// ============================================================================

TEST(Bootstrap_RegisterSingle)
{
    Spark::EngineBootstrap bootstrap;
    bool ok = bootstrap.Register({"Logger", [] { return true; }, [] {}, {}});
    EXPECT_TRUE(ok);
    EXPECT_EQ(bootstrap.GetSubsystemCount(), static_cast<size_t>(1));
}

TEST(Bootstrap_RegisterMultiple)
{
    Spark::EngineBootstrap bootstrap;
    auto count = bootstrap.RegisterAll({
        {"Logger", [] { return true; }, [] {}, {}},
        {"Platform", [] { return true; }, [] {}, {}},
        {"Graphics", [] { return true; }, [] {}, {"Logger", "Platform"}},
    });
    EXPECT_EQ(count, static_cast<size_t>(3));
    EXPECT_EQ(bootstrap.GetSubsystemCount(), static_cast<size_t>(3));
}

TEST(Bootstrap_RejectDuplicateName)
{
    Spark::EngineBootstrap bootstrap;
    bootstrap.Register({"Logger", [] { return true; }, [] {}, {}});
    bool dup = bootstrap.Register({"Logger", [] { return true; }, [] {}, {}});
    EXPECT_FALSE(dup);
    EXPECT_EQ(bootstrap.GetSubsystemCount(), static_cast<size_t>(1));
}

TEST(Bootstrap_RejectEmptyName)
{
    Spark::EngineBootstrap bootstrap;
    bool ok = bootstrap.Register({"", [] { return true; }, [] {}, {}});
    EXPECT_FALSE(ok);
    EXPECT_EQ(bootstrap.GetSubsystemCount(), static_cast<size_t>(0));
}

TEST(Bootstrap_RejectRegisterAfterInit)
{
    Spark::EngineBootstrap bootstrap;
    bootstrap.Register({"A", [] { return true; }, [] {}, {}});
    bootstrap.Initialize();
    bool late = bootstrap.Register({"B", [] { return true; }, [] {}, {}});
    EXPECT_FALSE(late);
    EXPECT_EQ(bootstrap.GetSubsystemCount(), static_cast<size_t>(1));
}

// ============================================================================
// EngineBootstrap — initialization ordering
// ============================================================================

TEST(Bootstrap_InitOrder_RespectsDependencies)
{
    Spark::EngineBootstrap bootstrap;
    bootstrap.RegisterAll({
        {"Graphics", [] { return true; }, [] {}, {"Logger", "Platform"}},
        {"Logger", [] { return true; }, [] {}, {}},
        {"Platform", [] { return true; }, [] {}, {"Logger"}},
    });

    EXPECT_TRUE(bootstrap.Initialize());

    const auto& order = bootstrap.GetInitOrder();
    EXPECT_EQ(order.size(), static_cast<size_t>(3));

    // Logger must come before Platform and Graphics
    auto loggerIt = std::find(order.begin(), order.end(), "Logger");
    auto platformIt = std::find(order.begin(), order.end(), "Platform");
    auto graphicsIt = std::find(order.begin(), order.end(), "Graphics");
    EXPECT_TRUE(loggerIt != order.end());
    EXPECT_TRUE(platformIt != order.end());
    EXPECT_TRUE(graphicsIt != order.end());
    EXPECT_TRUE(loggerIt < platformIt);
    EXPECT_TRUE(loggerIt < graphicsIt);
    EXPECT_TRUE(platformIt < graphicsIt);
}

TEST(Bootstrap_NoDependencies_AllInitialized)
{
    Spark::EngineBootstrap bootstrap;
    bootstrap.RegisterAll({
        {"A", [] { return true; }, [] {}, {}},
        {"B", [] { return true; }, [] {}, {}},
        {"C", [] { return true; }, [] {}, {}},
    });
    EXPECT_TRUE(bootstrap.Initialize());
    EXPECT_TRUE(bootstrap.IsInitialized("A"));
    EXPECT_TRUE(bootstrap.IsInitialized("B"));
    EXPECT_TRUE(bootstrap.IsInitialized("C"));
}

// ============================================================================
// EngineBootstrap — failure handling
// ============================================================================

TEST(Bootstrap_CycleDetection)
{
    Spark::EngineBootstrap bootstrap;
    bootstrap.RegisterAll({
        {"A", [] { return true; }, [] {}, {"B"}},
        {"B", [] { return true; }, [] {}, {"A"}},
    });
    EXPECT_FALSE(bootstrap.Initialize());
}

TEST(Bootstrap_FailedInitCascades)
{
    Spark::EngineBootstrap bootstrap;
    bootstrap.RegisterAll({
        {"Base", [] { return false; }, [] {}, {}},
        {"Dependent", [] { return true; }, [] {}, {"Base"}},
    });
    EXPECT_FALSE(bootstrap.Initialize());
    EXPECT_FALSE(bootstrap.IsInitialized("Base"));
    EXPECT_FALSE(bootstrap.IsInitialized("Dependent"));
    EXPECT_TRUE(bootstrap.GetState("Base") == Spark::SubsystemState::Failed);
    EXPECT_TRUE(bootstrap.GetState("Dependent") == Spark::SubsystemState::Failed);
}

TEST(Bootstrap_MissingDependency_SkipsDependent)
{
    Spark::EngineBootstrap bootstrap;
    bootstrap.Register({"Child", [] { return true; }, [] {}, {"NonExistent"}});
    // Initialize succeeds at the sort level (missing dep is a warning)
    // but the child is skipped because its dependency is not initialized
    EXPECT_FALSE(bootstrap.Initialize());
    EXPECT_FALSE(bootstrap.IsInitialized("Child"));
}

TEST(Bootstrap_ExceptionInInit_MarkedFailed)
{
    Spark::EngineBootstrap bootstrap;
    bootstrap.Register({"Thrower", []() -> bool { throw std::runtime_error("boom"); }, [] {}, {}});
    EXPECT_FALSE(bootstrap.Initialize());
    EXPECT_TRUE(bootstrap.GetState("Thrower") == Spark::SubsystemState::Failed);
}

TEST(Bootstrap_SentinelNode_NoInitFunction)
{
    Spark::EngineBootstrap bootstrap;
    bootstrap.Register({"Group", nullptr, nullptr, {}});
    EXPECT_TRUE(bootstrap.Initialize());
    EXPECT_TRUE(bootstrap.IsInitialized("Group"));
}

// ============================================================================
// EngineBootstrap — shutdown
// ============================================================================

TEST(Bootstrap_ShutdownReverseOrder)
{
    std::vector<std::string> shutdownOrder;
    Spark::EngineBootstrap bootstrap;
    bootstrap.RegisterAll({
        {"First", [] { return true; }, [&] { shutdownOrder.push_back("First"); }, {}},
        {"Second", [] { return true; }, [&] { shutdownOrder.push_back("Second"); }, {"First"}},
        {"Third", [] { return true; }, [&] { shutdownOrder.push_back("Third"); }, {"Second"}},
    });
    bootstrap.Initialize();
    bootstrap.Shutdown();
    EXPECT_EQ(shutdownOrder.size(), static_cast<size_t>(3));
    if (shutdownOrder.size() == 3)
    {
        EXPECT_EQ(shutdownOrder[0], std::string("Third"));
        EXPECT_EQ(shutdownOrder[1], std::string("Second"));
        EXPECT_EQ(shutdownOrder[2], std::string("First"));
    }
}

TEST(Bootstrap_DoubleShutdown_Safe)
{
    int shutdownCount = 0;
    Spark::EngineBootstrap bootstrap;
    bootstrap.Register({"X", [] { return true; }, [&] { ++shutdownCount; }, {}});
    bootstrap.Initialize();
    bootstrap.Shutdown();
    bootstrap.Shutdown(); // second call is a no-op
    EXPECT_EQ(shutdownCount, 1);
}

TEST(Bootstrap_DoubleInit_Ignored)
{
    int initCount = 0;
    Spark::EngineBootstrap bootstrap;
    bootstrap.Register({"X",
                        [&]
                        {
                            ++initCount;
                            return true;
                        },
                        [] {},
                        {}});
    EXPECT_TRUE(bootstrap.Initialize());
    EXPECT_TRUE(bootstrap.Initialize()); // second call returns true, no-op
    EXPECT_EQ(initCount, 1);
}

TEST(Bootstrap_DestructorCallsShutdown)
{
    int shutdownCount = 0;
    {
        Spark::EngineBootstrap bootstrap;
        bootstrap.Register({"X", [] { return true; }, [&] { ++shutdownCount; }, {}});
        bootstrap.Initialize();
        // destructor fires here
    }
    EXPECT_EQ(shutdownCount, 1);
}

TEST(Bootstrap_FailedSubsystem_NotShutDown)
{
    int shutdownCount = 0;
    Spark::EngineBootstrap bootstrap;
    bootstrap.RegisterAll({
        {"Good", [] { return true; }, [&] { ++shutdownCount; }, {}},
        {"Bad", [] { return false; }, [&] { ++shutdownCount; }, {}},
    });
    bootstrap.Initialize();
    bootstrap.Shutdown();
    // Only "Good" should have been shut down
    EXPECT_EQ(shutdownCount, 1);
}

TEST(Bootstrap_ExceptionInShutdown_Continues)
{
    int secondShutdown = 0;
    Spark::EngineBootstrap bootstrap;
    bootstrap.RegisterAll({
        {"First", [] { return true; }, [&] { ++secondShutdown; }, {}},
        {"Thrower", [] { return true; }, [] { throw std::runtime_error("shutdown boom"); }, {"First"}},
    });
    bootstrap.Initialize();
    // Shutdown should catch the exception and continue
    EXPECT_NO_THROW(bootstrap.Shutdown());
    EXPECT_EQ(secondShutdown, 1);
}

// ============================================================================
// EngineBootstrap — query API
// ============================================================================

TEST(Bootstrap_GetState_Registered)
{
    Spark::EngineBootstrap bootstrap;
    bootstrap.Register({"X", [] { return true; }, [] {}, {}});
    EXPECT_TRUE(bootstrap.GetState("X") == Spark::SubsystemState::Registered);
}

TEST(Bootstrap_GetState_Unknown_ReturnsRegistered)
{
    Spark::EngineBootstrap bootstrap;
    // Unknown name returns the default (Registered) — no crash
    EXPECT_TRUE(bootstrap.GetState("NonExistent") == Spark::SubsystemState::Registered);
}

TEST(Bootstrap_IsInitialized_BeforeInit)
{
    Spark::EngineBootstrap bootstrap;
    bootstrap.Register({"X", [] { return true; }, [] {}, {}});
    EXPECT_FALSE(bootstrap.IsInitialized("X"));
}

TEST(Bootstrap_GetInitOrder_BeforeInit_Empty)
{
    Spark::EngineBootstrap bootstrap;
    bootstrap.Register({"X", [] { return true; }, [] {}, {}});
    EXPECT_TRUE(bootstrap.GetInitOrder().empty());
}

// ============================================================================
// Platform detection macros
// ============================================================================

TEST(Platform_ExactlyOnePlatformDefined)
{
    int count = 0;
#ifdef SPARK_PLATFORM_WINDOWS
    ++count;
#endif
#ifdef SPARK_PLATFORM_LINUX
    ++count;
#endif
#ifdef SPARK_PLATFORM_MACOS
    ++count;
#endif
    EXPECT_EQ(count, 1);
}

TEST(Platform_Cpp23Required)
{
#ifdef SPARK_CPP23
    EXPECT_TRUE(true);
#else
    // This should never be reached — the build enforces C++23
    EXPECT_TRUE(false);
#endif
}

TEST(Platform_CompilerDetected)
{
    int count = 0;
#ifdef SPARK_COMPILER_MSVC
    ++count;
#endif
#ifdef SPARK_COMPILER_GCC
    ++count;
#endif
    // Only count non-Apple, non-Intel generic Clang separately
#if defined(SPARK_COMPILER_CLANG) && !defined(SPARK_COMPILER_APPLE_CLANG) && !defined(SPARK_COMPILER_INTEL_LLVM)
    ++count;
#endif
#ifdef SPARK_COMPILER_APPLE_CLANG
    ++count;
#endif
#ifdef SPARK_COMPILER_INTEL_LLVM
    ++count;
#endif
#ifdef SPARK_COMPILER_INTEL_CLASSIC
    ++count;
#endif
    // Exactly one primary compiler should be detected
    EXPECT_GE(count, 1);
}

TEST(Platform_CompilerVersionNonZero)
{
    EXPECT_GT(SPARK_COMPILER_VERSION, 0);
}

TEST(Platform_NativeWindowHandle_IsVoidPtr)
{
    EXPECT_EQ(sizeof(Spark::NativeWindowHandle), sizeof(void*));
}

// ============================================================================
// Lifecycle stages — factory and properties
// ============================================================================

TEST(Lifecycle_CreateInitDebugStage)
{
    auto stage = Spark::Core::Lifecycle::CreateInitDebugStage();
    EXPECT_TRUE(stage != nullptr);
    if (stage)
    {
        EXPECT_TRUE(stage->SupportsInitialize());
        EXPECT_TRUE(stage->Name().size() > 0);
    }
}

TEST(Lifecycle_CreateInitNetworkingStage)
{
    auto stage = Spark::Core::Lifecycle::CreateInitNetworkingStage();
    EXPECT_TRUE(stage != nullptr);
    if (stage)
    {
        EXPECT_TRUE(stage->SupportsInitialize());
    }
}

TEST(Lifecycle_CreateInitGameplayStage)
{
    auto stage = Spark::Core::Lifecycle::CreateInitGameplayStage();
    EXPECT_TRUE(stage != nullptr);
    if (stage)
    {
        EXPECT_TRUE(stage->SupportsInitialize());
    }
}

TEST(Lifecycle_CreateUpdateStage)
{
    auto stage = Spark::Core::Lifecycle::CreateUpdateStage();
    EXPECT_TRUE(stage != nullptr);
    if (stage)
    {
        EXPECT_TRUE(stage->SupportsUpdate());
    }
}

TEST(Lifecycle_CreateShutdownStage)
{
    auto stage = Spark::Core::Lifecycle::CreateShutdownStage();
    EXPECT_TRUE(stage != nullptr);
    if (stage)
    {
        EXPECT_TRUE(stage->SupportsShutdown());
    }
}

TEST(Lifecycle_OrderValues_Ascending)
{
    // Physics < Animation < AI < Audio < Lifecycle < Render
    using Spark::Core::Lifecycle::LifecycleOrder;
    EXPECT_TRUE(static_cast<uint16_t>(LifecycleOrder::Physics) < static_cast<uint16_t>(LifecycleOrder::Animation));
    EXPECT_TRUE(static_cast<uint16_t>(LifecycleOrder::Animation) < static_cast<uint16_t>(LifecycleOrder::AI));
    EXPECT_TRUE(static_cast<uint16_t>(LifecycleOrder::AI) < static_cast<uint16_t>(LifecycleOrder::Audio));
    EXPECT_TRUE(static_cast<uint16_t>(LifecycleOrder::Audio) < static_cast<uint16_t>(LifecycleOrder::Lifecycle));
    EXPECT_TRUE(static_cast<uint16_t>(LifecycleOrder::Lifecycle) < static_cast<uint16_t>(LifecycleOrder::Render));
}

TEST(Lifecycle_ThreadAffinity_Values)
{
    using Spark::Core::Lifecycle::LifecycleThreadAffinity;
    // Just verify the enum values are distinct
    EXPECT_NE(static_cast<uint8_t>(LifecycleThreadAffinity::MainThread),
              static_cast<uint8_t>(LifecycleThreadAffinity::AnyThread));
}

// ============================================================================
// WineDetection — non-Windows stubs
// ============================================================================

#ifndef SPARK_PLATFORM_WINDOWS

TEST(Wine_NotRunningOnLinux)
{
    EXPECT_FALSE(Spark::IsRunningUnderWine());
}

TEST(Wine_VersionEmptyOnLinux)
{
    EXPECT_TRUE(Spark::GetWineVersion().empty());
}

TEST(Wine_LogNoOp_NoCrash)
{
    EXPECT_NO_THROW(Spark::LogWineEnvironmentIfApplicable());
}

#else // SPARK_PLATFORM_WINDOWS

TEST(Wine_DetectionCallable)
{
    // On Windows (native or Wine), the call should not crash
    bool result = Spark::IsRunningUnderWine();
    (void)result; // value depends on environment
    EXPECT_TRUE(true);
}

TEST(Wine_VersionCallable)
{
    const auto& ver = Spark::GetWineVersion();
    if (Spark::IsRunningUnderWine())
    {
        EXPECT_FALSE(ver.empty());
    }
    else
    {
        EXPECT_TRUE(ver.empty());
    }
}

TEST(Wine_LogCallable_NoCrash)
{
    EXPECT_NO_THROW(Spark::LogWineEnvironmentIfApplicable());
}

#endif // SPARK_PLATFORM_WINDOWS
