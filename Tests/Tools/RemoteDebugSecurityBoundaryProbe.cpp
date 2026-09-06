// RemoteDebugSecurityBoundaryProbe.cpp
//
// Header-only hostile boundary probe intended for direct MSVC compilation. It
// deliberately emulates an external translation unit: defining the legacy test
// macro must not mint a principal, public loopback must stay observer-only, and
// StopListening must serialize revocation with a protected handler.
//
// test-registration: ignore
//
// This probe carries its own Logger/SimpleConsole mirrors, so linking it into
// SparkTests produces LNK2005 duplicate symbols. It stays a manual tool driven
// by Tests/Tools/run-remote-debug-security-probes.ps1; the protections it
// probes are enforced in-suite by Tests/TestRemoteDebugSystem.cpp. Deleting the
// marker without registering the file is what the guard is for.

#define NDEBUG 1
#define SPARK_REMOTE_DEBUG_TESTING 1
#include "Engine/RemoteDebug/RemoteDebugSystem.h"
#undef SPARK_REMOTE_DEBUG_TESTING

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>

namespace Spark::RemoteDebug
{
    // Exact legacy harness name: the rejected header made this arbitrary
    // external type a friend when SPARK_REMOTE_DEBUG_TESTING was defined.
    class RemoteDebugAccessControlTestHarness
    {
      public:
        template <typename Server>
        static constexpr bool kCanMintLegacyPrincipal = requires(Server& server) {
            server.IssueTrustedLoopbackPrincipal(RemoteDebugRole::Administrator, uint64_t{1});
        };

        template <typename Server>
        static constexpr bool kCanDispatchWithPrincipal =
            requires(Server& server, const RemoteCommand& command, const RemoteDebugPrincipal& principal) {
                server.ProcessCommandWithPrincipal(command, principal);
            };

        template <typename Session>
        static constexpr bool kCanQueueWithPrincipal =
            requires(Session& session, const RemoteCommand& command, const RemoteDebugPrincipal& principal) {
                session.EnqueueReceivedWithPrincipal(command, principal);
            };
    };
} // namespace Spark::RemoteDebug

static_assert(!Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::kCanMintLegacyPrincipal<
              Spark::RemoteDebug::RemoteDebugServer>);
static_assert(!Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::kCanDispatchWithPrincipal<
              Spark::RemoteDebug::RemoteDebugServer>);
static_assert(!Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::kCanQueueWithPrincipal<
              Spark::RemoteDebug::RemoteSession>);

namespace Spark
{
    namespace
    {
        std::atomic_bool g_consoleExecuted{false};
    }

    // Minimal link stubs: this probe exercises the header-only boundary without
    // starting the engine. Console invocation is captured as a security signal.
    Logger& Logger::Get()
    {
        static Logger logger;
        return logger;
    }

    void Logger::Initialize(bool) {}
    void Logger::Shutdown() {}
    bool Logger::ShouldLog(LogLevel, LogCategory) const
    {
        return false;
    }
    void Logger::Log(LogLevel, LogCategory, const char*, int, const char*, const std::string&) {}

    SimpleConsole& SimpleConsole::GetInstance()
    {
        static SimpleConsole console;
        return console;
    }

    SimpleConsole::ConsoleStats SimpleConsole::GetStats() const
    {
        return {};
    }
    bool SimpleConsole::ExecuteCommand(const std::string&)
    {
        g_consoleExecuted.store(true, std::memory_order_release);
        return true;
    }
    std::vector<SimpleConsole::LogEntry> SimpleConsole::GetLogHistory() const
    {
        return {};
    }
} // namespace Spark

namespace
{
    using namespace Spark::RemoteDebug;

    [[nodiscard]] bool IsDenied(const RemoteCommand& command)
    {
        return command.type == "error" && command.payload.find("access_denied") != std::string::npos;
    }

    void Report(const char* name, bool passed, bool& allPassed)
    {
        std::cout << name << '=' << (passed ? "PASS" : "FAIL") << '\n';
        allPassed = allPassed && passed;
    }

    [[nodiscard]] bool PublicLoopbackConsoleIsDenied()
    {
        Spark::g_consoleExecuted.store(false, std::memory_order_release);
        auto& system = RemoteDebugSystem::GetInstance();
        system.Initialize();
        system.EnableLoopback();
        auto* client = system.GetClient();
        if (client == nullptr)
            return false;

        const uint32_t requestId = client->ExecuteConsoleCommand("audit_public_loopback_exec");
        system.Update(0.016f);
        const auto responses = client->PollResponses();
        system.Shutdown();
        return !Spark::g_consoleExecuted.load(std::memory_order_acquire) && responses.size() == 1 &&
               IsDenied(responses.front()) && responses.front().requestId == requestId;
    }

    [[nodiscard]] bool RawDispatchAndQueueAreDenied()
    {
        RemoteDebugServer server;
        server.StartListening(0);
        std::atomic_uint32_t effectCount{0};
        server.RegisterCommandHandler("raw_exec", RemoteDebugCapability::ExecuteConsole,
                                      [&effectCount](const RemoteCommand& command)
                                      {
                                          ++effectCount;
                                          return RemoteCommand{"raw_exec_ok", "", command.requestId, 0.0f};
                                      });

        const auto directResponse = server.ProcessCommand({"raw_exec", "", 1, 0.0f});
        server.GetSession().EnqueueReceived({"raw_exec", "", 2, 0.0f});
        server.Update();
        RemoteCommand queuedResponse;
        const bool receivedQueuedResponse = server.GetSession().DequeuePendingSend(queuedResponse);
        return IsDenied(directResponse) && receivedQueuedResponse && IsDenied(queuedResponse) &&
               effectCount.load() == 0;
    }

    [[nodiscard]] bool RevocationWaitsForEffectAndThenFailsClosed()
    {
        auto& system = RemoteDebugSystem::GetInstance();
        system.Initialize();
        system.EnableLoopback();
        auto* client = system.GetClient();
        auto* server = system.GetServer();
        if (client == nullptr || server == nullptr)
            return false;

        std::mutex gateMutex;
        std::condition_variable handlerEntered;
        std::condition_variable releaseHandler;
        std::condition_variable stopAttempted;
        bool handlerWaiting = false;
        bool permitEffect = false;
        bool stopCalling = false;
        std::atomic_uint32_t effectCount{0};
        std::atomic_bool stopReturned{false};
        std::atomic_bool effectBeforeStopReturned{false};

        server->RegisterCommandHandler("slow_inspect", RemoteDebugCapability::Inspect,
                                       [&](const RemoteCommand& command)
                                       {
                                           {
                                               std::lock_guard lock(gateMutex);
                                               handlerWaiting = true;
                                           }
                                           handlerEntered.notify_one();

                                           std::unique_lock lock(gateMutex);
                                           releaseHandler.wait(lock, [&] { return permitEffect; });
                                           effectBeforeStopReturned.store(!stopReturned.load(std::memory_order_acquire),
                                                                          std::memory_order_release);
                                           ++effectCount;
                                           return RemoteCommand{"slow_inspect_ok", "", command.requestId, 0.0f};
                                       });

        client->SendCommand({"slow_inspect", "", 1, 0.0f});
        std::thread updateThread([&system] { system.Update(0.016f); });
        {
            std::unique_lock lock(gateMutex);
            handlerEntered.wait(lock, [&] { return handlerWaiting; });
        }

        std::thread stopThread(
            [&]
            {
                {
                    std::lock_guard lock(gateMutex);
                    stopCalling = true;
                }
                stopAttempted.notify_one();
                server->StopListening();
                stopReturned.store(true, std::memory_order_release);
            });
        {
            std::unique_lock lock(gateMutex);
            stopAttempted.wait(lock, [&] { return stopCalling; });
        }
        for (int attempt = 0; attempt < 1024 && !stopReturned.load(std::memory_order_acquire); ++attempt)
            std::this_thread::yield();
        const bool waitedForHandler = !stopReturned.load(std::memory_order_acquire);

        {
            std::lock_guard lock(gateMutex);
            permitEffect = true;
        }
        releaseHandler.notify_one();
        updateThread.join();
        stopThread.join();

        const auto eventsAfterStop = server->GetAuditEvents();
        const bool allowedAudit =
            !eventsAfterStop.empty() && eventsAfterStop.back().decision == RemoteDebugAuditDecision::Allowed;

        client->SendCommand({"slow_inspect", "", 2, 0.0f});
        system.Update(0.016f);
        const auto responses = client->PollResponses();
        bool deniedAfterStop = false;
        for (const auto& response : responses)
            deniedAfterStop = deniedAfterStop || IsDenied(response);
        const auto eventsAfterRetry = server->GetAuditEvents();
        const bool invalidAudit = !eventsAfterRetry.empty() &&
                                  eventsAfterRetry.back().decision == RemoteDebugAuditDecision::InvalidPrincipalDenied;

        const bool passed =
            waitedForHandler && effectCount.load() == 1 && effectBeforeStopReturned.load(std::memory_order_acquire) &&
            stopReturned.load(std::memory_order_acquire) && allowedAudit && deniedAfterStop && invalidAudit;
        system.Shutdown();
        return passed;
    }
} // namespace

int main()
{
    bool allPassed = true;
    Report("legacy_test_macro_cannot_mint", true, allPassed);
    Report("public_loopback_console_denied", PublicLoopbackConsoleIsDenied(), allPassed);
    Report("raw_dispatch_and_queue_denied", RawDispatchAndQueueAreDenied(), allPassed);
    Report("revocation_execution_lease", RevocationWaitsForEffectAndThenFailsClosed(), allPassed);
    return allPassed ? 0 : 1;
}
