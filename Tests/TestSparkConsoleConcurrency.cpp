#include "TestFramework.h"
#include "Utils/SecureMemory.h"
#include "Utils/SparkConsole.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

TEST(SimpleConsole_ConcurrentStateAccessIsConsistent)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    console.Initialize();
    const auto before = console.GetStats();

    constexpr int threadCount = 4;
    constexpr int logsPerThread = 100;
    std::vector<std::thread> workers;
    std::atomic<bool> operationsSucceeded{true};
    workers.reserve(threadCount);
    for (int worker = 0; worker < threadCount; ++worker)
    {
        workers.emplace_back(
            [&, worker]
            {
                const std::string command = "concurrency_probe_" + std::to_string(worker);
                const std::string alias = "concurrency_alias_" + std::to_string(worker);
                console.RegisterCommand(command, [](const std::vector<std::string>&) { return std::string{}; });
                console.SetAlias(alias, command);
                if (!console.ExecuteCommand(alias))
                    operationsSucceeded.store(false, std::memory_order_relaxed);
                for (int i = 0; i < logsPerThread; ++i)
                    console.LogDebug("concurrency probe");
                console.RemoveAlias(alias);
                if (!console.UnregisterCommand(command))
                    operationsSucceeded.store(false, std::memory_order_relaxed);
            });
    }
    for (auto& worker : workers)
        worker.join();

    const auto after = console.GetStats();
    EXPECT_TRUE(operationsSucceeded.load(std::memory_order_relaxed));
    EXPECT_TRUE(after.totalLogsWritten >= before.totalLogsWritten + threadCount * logsPerThread);
    EXPECT_TRUE(after.totalCommandsExecuted >= before.totalCommandsExecuted + threadCount);
}

TEST(SimpleConsole_ShutdownDrainsHandlersAndRejectsLateRegistration)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    console.Initialize();

    std::atomic<bool> handlerEntered{false};
    std::atomic<bool> releaseHandler{false};
    std::atomic<bool> shutdownFinished{false};
    console.RegisterCommand("lifecycle_blocking_probe",
                            [&](const std::vector<std::string>&)
                            {
                                handlerEntered.store(true, std::memory_order_release);
                                while (!releaseHandler.load(std::memory_order_acquire))
                                    std::this_thread::yield();
                                return std::string{};
                            });

    std::thread executor([&] { console.ExecuteCommand("lifecycle_blocking_probe"); });
    while (!handlerEntered.load(std::memory_order_acquire))
        std::this_thread::yield();
    std::thread shutdown(
        [&]
        {
            console.Shutdown();
            shutdownFinished.store(true, std::memory_order_release);
        });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(shutdownFinished.load(std::memory_order_acquire));
    releaseHandler.store(true, std::memory_order_release);
    executor.join();
    shutdown.join();
    EXPECT_TRUE(shutdownFinished.load(std::memory_order_acquire));

    console.RegisterCommand("late_registration_probe", [](const auto&) { return std::string{}; });
    EXPECT_FALSE(console.HasCommand("late_registration_probe"));
    EXPECT_TRUE(console.Initialize());
}

TEST(SimpleConsole_SensitiveCommandRedactsHistory)
{
    auto& console = Spark::SimpleConsole::GetInstance();
    EXPECT_TRUE(console.Initialize());

    constexpr std::string_view secret = "credential-that-must-not-persist";
    bool handlerSawCredential = false;
    console.RegisterSensitiveCommand("sensitive_history_probe",
                                     [&](const std::vector<std::string>& args)
                                     {
                                         handlerSawCredential = args.size() == 2 && args[1] == secret;
                                         return std::string{};
                                     });

    std::string commandLine = "sensitive_history_probe user " + std::string(secret);
    EXPECT_TRUE(console.ExecuteCommand(commandLine));
    Spark::SecureClear(commandLine); // the caller retains ownership of its input buffer

    EXPECT_TRUE(handlerSawCredential);
    const auto history = console.GetCommandHistory();
    EXPECT_FALSE(history.empty());
    if (!history.empty())
        EXPECT_EQ(history.back(), std::string("sensitive_history_probe <arguments-redacted>"));
    for (const auto& entry : history)
        EXPECT_TRUE(entry.find(secret) == std::string::npos);

    EXPECT_TRUE(console.UnregisterCommand("sensitive_history_probe"));
    commandLine = "sensitive_history_probe user " + std::string(secret);
    EXPECT_FALSE(console.ExecuteCommand(commandLine));
    Spark::SecureClear(commandLine);
    const auto postUnregisterHistory = console.GetCommandHistory();
    EXPECT_FALSE(postUnregisterHistory.empty());
    if (!postUnregisterHistory.empty())
        EXPECT_EQ(postUnregisterHistory.back(), std::string("sensitive_history_probe <arguments-redacted>"));

    console.Shutdown();
    EXPECT_TRUE(console.Initialize());
    commandLine = "sensitive_history_probe user " + std::string(secret);
    EXPECT_FALSE(console.ExecuteCommand(commandLine));
    Spark::SecureClear(commandLine);
    const auto postRestartHistory = console.GetCommandHistory();
    EXPECT_FALSE(postRestartHistory.empty());
    if (!postRestartHistory.empty())
        EXPECT_EQ(postRestartHistory.back(), std::string("sensitive_history_probe <arguments-redacted>"));
}
