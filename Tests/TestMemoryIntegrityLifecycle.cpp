// TestMemoryIntegrityLifecycle.cpp - Lifecycle ownership tests for memory-integrity callbacks

#include "TestFramework.h"
#include "Engine/Security/MemoryIntegrity.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace Spark::Security;

namespace
{
    uint8_t g_lifecycleBuffer[16]{};

    void InitializeLifecycleTestSystem()
    {
        auto& system = MemoryIntegritySystem::GetInstance();
        IntegrityConfig config;
        config.autoDiscoverRegions = false;
        config.enabled = true;
        config.scanIntervalSec = 999.0f;
        system.Configure(config);
        system.Initialize();
    }
} // namespace

TEST(MemoryIntegrityLifecycle_ShutdownClearsInitializedCallback)
{
    auto& system = MemoryIntegritySystem::GetInstance();
    system.Shutdown();
    InitializeLifecycleTestSystem();

    int staleCallbackInvocations = 0;
    auto lifetime = std::make_shared<int>(42);
    std::weak_ptr<int> weakLifetime = lifetime;
    system.SetViolationCallback([lifetime, &staleCallbackInvocations](const Violation&)
                                { ++staleCallbackInvocations; });
    lifetime.reset();

    system.Shutdown();
    EXPECT_TRUE(weakLifetime.expired());

    InitializeLifecycleTestSystem();
    g_lifecycleBuffer[0] = 0;
    system.RegisterCodeRegion("lifecycle_callback", g_lifecycleBuffer, sizeof(g_lifecycleBuffer));
    g_lifecycleBuffer[0] = 1;
    EXPECT_FALSE(system.ScanAllRegions());
    EXPECT_EQ(staleCallbackInvocations, 0);

    system.SetViolationCallback({});
    system.Shutdown();
}

TEST(MemoryIntegrityLifecycle_ShutdownClearsCallbackWhenAlreadyUninitialized)
{
    auto& system = MemoryIntegritySystem::GetInstance();
    system.Shutdown();

    auto lifetime = std::make_shared<int>(42);
    std::weak_ptr<int> weakLifetime = lifetime;
    system.SetViolationCallback([lifetime](const Violation&) {});
    lifetime.reset();

    system.Shutdown();
    EXPECT_TRUE(weakLifetime.expired());

    // RED cleanup: do not leave a captured token in the singleton for the next test.
    system.SetViolationCallback({});
}

TEST(MemoryIntegrityLifecycle_CallbackDestructionMayReenterShutdown)
{
    auto& system = MemoryIntegritySystem::GetInstance();
    system.Shutdown();
    InitializeLifecycleTestSystem();

    std::atomic<bool> reenterShutdown{true};
    std::atomic<bool> callbackStateDestroyed{false};
    auto lifetime = std::shared_ptr<int>(new int(42),
                                         [&](int* value)
                                         {
                                             delete value;
                                             callbackStateDestroyed.store(true, std::memory_order_release);
                                             if (reenterShutdown.load(std::memory_order_acquire))
                                                 system.Shutdown();
                                         });
    system.SetViolationCallback([lifetime](const Violation&) {});
    lifetime.reset();

    system.Shutdown();
    EXPECT_TRUE(callbackStateDestroyed.load(std::memory_order_acquire));

    // RED cleanup avoids re-entering from SetViolationCallback while it owns the state lock.
    reenterShutdown.store(false, std::memory_order_release);
    system.SetViolationCallback({});
    system.Shutdown();
}

TEST(MemoryIntegrityLifecycle_CallbackReplacementDestructionMayReenterSetter)
{
    auto& system = MemoryIntegritySystem::GetInstance();
    system.Shutdown();

    std::atomic<bool> reentryStarted{false};
    std::atomic<bool> reentryFinished{false};
    std::atomic<bool> reentryFinishedBeforeDestructionReturned{false};
    std::thread reentryThread;
    auto lifetime = std::shared_ptr<int>(
        new int(42),
        [&](int* value)
        {
            delete value;
            reentryThread = std::thread(
                [&]
                {
                    reentryStarted.store(true, std::memory_order_release);
                    system.SetViolationCallback({});
                    reentryFinished.store(true, std::memory_order_release);
                });

            const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!reentryStarted.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < startDeadline)
                std::this_thread::yield();

            const auto finishDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (!reentryFinished.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < finishDeadline)
                std::this_thread::yield();
            reentryFinishedBeforeDestructionReturned.store(reentryFinished.load(std::memory_order_acquire),
                                                           std::memory_order_release);
        });
    system.SetViolationCallback([lifetime](const Violation&) {});
    lifetime.reset();

    system.SetViolationCallback({});
    if (reentryThread.joinable())
        reentryThread.join();

    EXPECT_TRUE(reentryStarted.load(std::memory_order_acquire));
    EXPECT_TRUE(reentryFinished.load(std::memory_order_acquire));
    EXPECT_TRUE(reentryFinishedBeforeDestructionReturned.load(std::memory_order_acquire));
    system.Shutdown();
}
