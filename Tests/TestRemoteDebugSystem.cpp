// TestRemoteDebugSystem.cpp - Tests for Spark::RemoteDebug::RemoteDebugSystem
#include "TestFramework.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <type_traits>

// Deliberately define the legacy test macro before including the production
// header. A public header must not grant this external translation unit a
// privileged friend path merely because a caller defines a macro.
#define SPARK_REMOTE_DEBUG_TESTING 1
#include "Engine/RemoteDebug/RemoteDebugSystem.h"
#undef SPARK_REMOTE_DEBUG_TESTING

// This intentionally reuses the legacy harness name from the rejected
// implementation. If the production header ever restores macro-gated
// friendship, the compile-time assertions below fail on MSVC and every other
// conforming C++23 compiler before a test can run.
namespace Spark::RemoteDebug
{
    class RemoteDebugAccessControlTestHarness
    {
      public:
        template <typename Server>
        static constexpr bool kCanMintLegacyPrincipal = requires(Server& server)
        {
            server.IssueTrustedLoopbackPrincipal(RemoteDebugRole::Administrator, uint64_t{1});
        };

        template <typename Server>
        static constexpr bool kCanMintLoopbackObserver = requires(Server& server)
        {
            server.IssueLoopbackObserverPrincipal();
        };

        template <typename Server>
        static constexpr bool kCanDispatchWithPrincipal =
            requires(Server& server, const RemoteCommand& command, const RemoteDebugPrincipal& principal)
        {
            server.ProcessCommandWithPrincipal(command, principal);
        };

        template <typename Session>
        static constexpr bool kCanQueueWithPrincipal =
            requires(Session& session, const RemoteCommand& command, const RemoteDebugPrincipal& principal)
        {
            session.EnqueueReceivedWithPrincipal(command, principal);
        };
    };
} // namespace Spark::RemoteDebug

static_assert(!Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::
                  kCanMintLegacyPrincipal<Spark::RemoteDebug::RemoteDebugServer>);
static_assert(!Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::
                  kCanMintLoopbackObserver<Spark::RemoteDebug::RemoteDebugServer>);
static_assert(!Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::
                  kCanDispatchWithPrincipal<Spark::RemoteDebug::RemoteDebugServer>);
static_assert(!Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::
                  kCanQueueWithPrincipal<Spark::RemoteDebug::RemoteSession>);

namespace
{
    [[nodiscard]] bool AuditEndsWith(const Spark::RemoteDebug::RemoteDebugServer& server,
                                     Spark::RemoteDebug::RemoteDebugAuditDecision decision)
    {
        const auto events = server.GetAuditEvents();
        return !events.empty() && events.back().decision == decision;
    }

    [[nodiscard]] bool IsAccessDenied(const Spark::RemoteDebug::RemoteCommand& response)
    {
        return response.type == "error" && response.payload.find("access_denied") != std::string::npos;
    }
}

// ============================================================================
// Initialization
// ============================================================================

TEST(RemoteDebugSystem_Initialize)
{
    auto& sys = Spark::RemoteDebug::RemoteDebugSystem::GetInstance();
    sys.Initialize();
    EXPECT_TRUE(sys.GetServer() != nullptr);
    EXPECT_TRUE(sys.GetClient() != nullptr);
    EXPECT_FALSE(sys.IsConnected());
    sys.Shutdown();
}

TEST(RemoteDebugSystem_ShutdownCleansUp)
{
    auto& sys = Spark::RemoteDebug::RemoteDebugSystem::GetInstance();
    sys.Initialize();
    sys.Shutdown();
    EXPECT_TRUE(sys.GetServer() == nullptr);
    EXPECT_TRUE(sys.GetClient() == nullptr);
}

// ============================================================================
// Command registration and dispatch
// ============================================================================

TEST(RemoteDebugSystem_RegisterCommandHandler)
{
    auto& sys = Spark::RemoteDebug::RemoteDebugSystem::GetInstance();
    sys.Initialize();
    sys.EnableLoopback();

    bool handlerCalled = false;
    sys.GetServer()->RegisterCommandHandler("test_cmd", Spark::RemoteDebug::RemoteDebugCapability::Inspect,
                                            [&](const Spark::RemoteDebug::RemoteCommand& cmd)
                                            {
                                                handlerCalled = true;
                                                return Spark::RemoteDebug::RemoteCommand{"test_response", "ok",
                                                                                         cmd.requestId, 0.0f};
                                            });

    Spark::RemoteDebug::RemoteCommand cmd{"test_cmd", "payload", 42, 0.0f};
    sys.GetClient()->SendCommand(cmd);
    sys.Update(0.016f);
    const auto responses = sys.GetClient()->PollResponses();
    EXPECT_TRUE(handlerCalled);
    EXPECT_EQ(static_cast<size_t>(1), responses.size());
    if (!responses.empty())
    {
        EXPECT_EQ(std::string("test_response"), responses[0].type);
        EXPECT_EQ(static_cast<uint32_t>(42), responses[0].requestId);
    }

    sys.Shutdown();
}

TEST(RemoteDebugSystem_UnknownCommandReturnsError)
{
    auto& sys = Spark::RemoteDebug::RemoteDebugSystem::GetInstance();
    sys.Initialize();
    sys.EnableLoopback();

    Spark::RemoteDebug::RemoteCommand cmd{"nonexistent_cmd", "", 1, 0.0f};
    sys.GetClient()->SendCommand(cmd);
    sys.Update(0.016f);
    const auto responses = sys.GetClient()->PollResponses();
    EXPECT_EQ(static_cast<size_t>(1), responses.size());
    if (!responses.empty())
    {
        EXPECT_EQ(std::string("error"), responses[0].type);
        EXPECT_STR_CONTAINS(responses[0].payload, "unknown_command");
    }

    sys.Shutdown();
}

// ============================================================================
// Server-owned authorization boundary
// ============================================================================

TEST(RemoteDebugSystem_RawDirectDispatchFailsClosed)
{
    Spark::RemoteDebug::RemoteDebugServer server;
    server.StartListening(0);

    bool handlerCalled = false;
    server.RegisterCommandHandler("direct_probe", [&](const Spark::RemoteDebug::RemoteCommand& command)
                                  {
                                      handlerCalled = true;
                                      return Spark::RemoteDebug::RemoteCommand{"direct_ok", "", command.requestId, 0.0f};
                                  });

    const Spark::RemoteDebug::RemoteCommand command{"direct_probe", "payload", 1, 0.0f};
    const auto response = server.ProcessCommand(command);
    EXPECT_FALSE(handlerCalled);
    EXPECT_EQ(std::string("error"), response.type);
    EXPECT_STR_CONTAINS(response.payload, "access_denied");
    EXPECT_TRUE(AuditEndsWith(server, Spark::RemoteDebug::RemoteDebugAuditDecision::AnonymousDenied));
}

TEST(RemoteDebugSystem_RawQueueAndCopiedCommandFailClosed)
{
    Spark::RemoteDebug::RemoteDebugServer server;
    server.StartListening(0);

    bool handlerCalled = false;
    server.RegisterCommandHandler("queue_probe", [&](const Spark::RemoteDebug::RemoteCommand& command)
                                  {
                                      handlerCalled = true;
                                      return Spark::RemoteDebug::RemoteCommand{"queue_ok", "", command.requestId, 0.0f};
                                  });

    const Spark::RemoteDebug::RemoteCommand original{"queue_probe", "payload", 7, 0.0f};
    const Spark::RemoteDebug::RemoteCommand copied = original;
    server.GetSession().EnqueueReceived(original);
    server.Update();

    Spark::RemoteDebug::RemoteCommand queuedResponse;
    ASSERT_TRUE(server.GetSession().DequeuePendingSend(queuedResponse));
    EXPECT_FALSE(handlerCalled);
    EXPECT_EQ(std::string("error"), queuedResponse.type);
    EXPECT_STR_CONTAINS(queuedResponse.payload, "access_denied");

    const auto copiedResponse = server.ProcessCommand(copied);
    EXPECT_FALSE(handlerCalled);
    EXPECT_EQ(std::string("error"), copiedResponse.type);
    EXPECT_STR_CONTAINS(copiedResponse.payload, "access_denied");
    EXPECT_TRUE(AuditEndsWith(server, Spark::RemoteDebug::RemoteDebugAuditDecision::AnonymousDenied));
}

TEST(RemoteDebugSystem_RolePolicyIsLeastPrivilege)
{
    using namespace Spark::RemoteDebug;
    EXPECT_TRUE(HasRemoteDebugCapability(CapabilitiesForRemoteDebugRole(RemoteDebugRole::Observer),
                                          RemoteDebugCapability::Inspect));
    EXPECT_FALSE(HasRemoteDebugCapability(CapabilitiesForRemoteDebugRole(RemoteDebugRole::Observer),
                                           RemoteDebugCapability::ModifyProperties));
    EXPECT_FALSE(HasRemoteDebugCapability(CapabilitiesForRemoteDebugRole(RemoteDebugRole::Observer),
                                           RemoteDebugCapability::ExecuteConsole));
    EXPECT_TRUE(HasRemoteDebugCapability(CapabilitiesForRemoteDebugRole(RemoteDebugRole::Operator),
                                          RemoteDebugCapability::ModifyProperties));
    EXPECT_FALSE(HasRemoteDebugCapability(CapabilitiesForRemoteDebugRole(RemoteDebugRole::Operator),
                                           RemoteDebugCapability::ExecuteConsole));
    EXPECT_TRUE(HasRemoteDebugCapability(CapabilitiesForRemoteDebugRole(RemoteDebugRole::Administrator),
                                          RemoteDebugCapability::ExecuteConsole));
}

TEST(RemoteDebugSystem_PublicLoopbackIsObserverOnly)
{
    auto& sys = Spark::RemoteDebug::RemoteDebugSystem::GetInstance();
    sys.Initialize();
    sys.EnableLoopback();

    auto* client = sys.GetClient();
    auto* server = sys.GetServer();
    ASSERT_TRUE(client != nullptr);
    ASSERT_TRUE(server != nullptr);

    const uint32_t inspectId = client->GetProperty("player.health");
    const uint32_t mutationId = client->SetProperty("player.health", "100");
    const uint32_t consoleId = client->ExecuteConsoleCommand("quit");
    sys.Update(0.016f);

    const auto responses = client->PollResponses();
    EXPECT_EQ(static_cast<size_t>(3), responses.size());
    if (responses.size() == 3)
    {
        EXPECT_EQ(std::string("property_value"), responses[0].type);
        EXPECT_EQ(inspectId, responses[0].requestId);
        EXPECT_TRUE(IsAccessDenied(responses[1]));
        EXPECT_EQ(mutationId, responses[1].requestId);
        EXPECT_TRUE(IsAccessDenied(responses[2]));
        EXPECT_EQ(consoleId, responses[2].requestId);
    }
    EXPECT_TRUE(AuditEndsWith(*server, Spark::RemoteDebug::RemoteDebugAuditDecision::AuthorizationDenied));

    sys.Shutdown();
}

TEST(RemoteDebugSystem_PublicLoopbackReplayAndRateLimitFailClosed)
{
    using namespace Spark::RemoteDebug;
    auto& sys = RemoteDebugSystem::GetInstance();
    sys.Initialize();
    sys.EnableLoopback();

    auto* client = sys.GetClient();
    auto* server = sys.GetServer();
    ASSERT_TRUE(client != nullptr);
    ASSERT_TRUE(server != nullptr);
    client->SendCommand({"heartbeat", "", 1, 0.0f});
    client->SendCommand({"heartbeat", "", 1, 0.0f});
    sys.Update(0.016f);

    const auto replayResponses = client->PollResponses();
    EXPECT_EQ(static_cast<size_t>(2), replayResponses.size());
    if (replayResponses.size() == 2)
    {
        EXPECT_EQ(std::string("heartbeat"), replayResponses[0].type);
        EXPECT_TRUE(IsAccessDenied(replayResponses[1]));
    }
    EXPECT_TRUE(AuditEndsWith(*server, RemoteDebugAuditDecision::ReplayDenied));
    sys.Shutdown();

    sys.Initialize();
    sys.EnableLoopback();
    client = sys.GetClient();
    server = sys.GetServer();
    ASSERT_TRUE(client != nullptr);
    ASSERT_TRUE(server != nullptr);
    for (uint32_t requestId = 1; requestId <= RemoteDebugAccessControl::kMaxRequestsPerWindow + 1; ++requestId)
        client->SendCommand({"heartbeat", "", requestId, 0.0f});
    sys.Update(0.016f);

    const auto rateResponses = client->PollResponses();
    EXPECT_EQ(static_cast<size_t>(RemoteDebugAccessControl::kMaxRequestsPerWindow + 1), rateResponses.size());
    if (!rateResponses.empty())
        EXPECT_TRUE(IsAccessDenied(rateResponses.back()));
    EXPECT_TRUE(AuditEndsWith(*server, RemoteDebugAuditDecision::RateLimitedDenied));
    sys.Shutdown();
}

TEST(RemoteDebugSystem_AuditRecordsDispositionWithoutPayloadsOrGrants)
{
    using namespace Spark::RemoteDebug;
    constexpr const char* secret = "sensitive-payload-must-not-enter-audit";

    auto& sys = RemoteDebugSystem::GetInstance();
    sys.Initialize();
    sys.EnableLoopback();
    auto* client = sys.GetClient();
    auto* server = sys.GetServer();
    ASSERT_TRUE(client != nullptr);
    ASSERT_TRUE(server != nullptr);

    client->SendCommand({"heartbeat", secret, 1, 0.0f});
    sys.Update(0.016f);
    const auto responses = client->PollResponses();
    EXPECT_EQ(static_cast<size_t>(1), responses.size());
    if (!responses.empty())
        EXPECT_EQ(std::string("heartbeat"), responses.front().type);

    const auto events = server->GetAuditEvents();
    ASSERT_TRUE(!events.empty());
    const auto& event = events.back();
    EXPECT_TRUE(event.decision == RemoteDebugAuditDecision::Allowed);
    EXPECT_EQ(std::string("trusted-local-loopback"), event.principal);
    EXPECT_EQ(std::string("in-process-loopback"), event.source);
    EXPECT_EQ(std::string("heartbeat"), event.commandType);
    EXPECT_EQ(static_cast<uint32_t>(1), event.requestId);
    EXPECT_FALSE(event.principal.find(secret) != std::string::npos);
    EXPECT_FALSE(event.source.find(secret) != std::string::npos);
    EXPECT_FALSE(event.commandType.find(secret) != std::string::npos);
    sys.Shutdown();
}

TEST(RemoteDebugSystem_StopListeningWaitsForProtectedEffect)
{
    using namespace Spark::RemoteDebug;
    auto& sys = RemoteDebugSystem::GetInstance();
    sys.Initialize();
    sys.EnableLoopback();

    auto* client = sys.GetClient();
    auto* server = sys.GetServer();
    ASSERT_TRUE(client != nullptr);
    ASSERT_TRUE(server != nullptr);

    std::mutex gateMutex;
    std::condition_variable handlerEntered;
    std::condition_variable releaseHandler;
    std::condition_variable stopAttempted;
    bool handlerIsWaiting = false;
    bool permitEffect = false;
    bool stopIsCalling = false;
    std::atomic_uint32_t effectCount{0};
    std::atomic_bool stopReturned{false};
    std::atomic_bool effectBeforeStopReturned{false};

    server->RegisterCommandHandler("slow_inspect", RemoteDebugCapability::Inspect,
                                   [&](const RemoteCommand& command)
                                   {
                                       {
                                           std::lock_guard lock(gateMutex);
                                           handlerIsWaiting = true;
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
    std::thread updateThread([&sys] { sys.Update(0.016f); });
    {
        std::unique_lock lock(gateMutex);
        handlerEntered.wait(lock, [&] { return handlerIsWaiting; });
    }

    std::thread stopThread([&]
                           {
                               {
                                   std::lock_guard lock(gateMutex);
                                   stopIsCalling = true;
                               }
                               stopAttempted.notify_one();
                               server->StopListening();
                               stopReturned.store(true, std::memory_order_release);
                           });
    {
        std::unique_lock lock(gateMutex);
        stopAttempted.wait(lock, [&] { return stopIsCalling; });
    }
    for (int attempt = 0; attempt < 1024 && !stopReturned.load(std::memory_order_acquire); ++attempt)
        std::this_thread::yield();
    EXPECT_FALSE(stopReturned.load(std::memory_order_acquire));

    {
        std::lock_guard lock(gateMutex);
        permitEffect = true;
    }
    releaseHandler.notify_one();
    updateThread.join();
    stopThread.join();

    EXPECT_EQ(static_cast<uint32_t>(1), effectCount.load());
    EXPECT_TRUE(effectBeforeStopReturned.load(std::memory_order_acquire));
    EXPECT_TRUE(stopReturned.load(std::memory_order_acquire));
    EXPECT_TRUE(AuditEndsWith(*server, RemoteDebugAuditDecision::Allowed));

    // A stale loopback token may remain in client plumbing after StopListening,
    // but the revoked server grant makes this second attempt fail before the
    // protected handler can run.
    client->SendCommand({"slow_inspect", "", 2, 0.0f});
    sys.Update(0.016f);
    const auto responses = client->PollResponses();
    bool sawDeniedResponse = false;
    for (const auto& response : responses)
        sawDeniedResponse = sawDeniedResponse || IsAccessDenied(response);
    EXPECT_TRUE(sawDeniedResponse);
    EXPECT_EQ(static_cast<uint32_t>(1), effectCount.load());
    EXPECT_TRUE(AuditEndsWith(*server, RemoteDebugAuditDecision::InvalidPrincipalDenied));
    sys.Shutdown();
}

// ============================================================================
// Loopback mode
// ============================================================================

TEST(RemoteDebugSystem_EnableLoopback)
{
    auto& sys = Spark::RemoteDebug::RemoteDebugSystem::GetInstance();
    sys.Initialize();
    sys.EnableLoopback();
    EXPECT_TRUE(sys.IsConnected());
    sys.Shutdown();
}

TEST(RemoteDebugSystem_LoopbackMessageRoundtrip)
{
    auto& sys = Spark::RemoteDebug::RemoteDebugSystem::GetInstance();
    sys.Initialize();
    sys.EnableLoopback();

    // Public loopback is observer-only: this public client operation must not
    // silently turn into local administrator console authority.
    uint32_t reqId = sys.GetClient()->ExecuteConsoleCommand("stat fps");

    // Pump loopback and server update
    sys.Update(0.016f);

    // Client should have received the response
    auto responses = sys.GetClient()->PollResponses();
    EXPECT_EQ(static_cast<size_t>(1), responses.size());
    if (!responses.empty())
    {
        EXPECT_TRUE(IsAccessDenied(responses[0]));
        EXPECT_EQ(reqId, responses[0].requestId);
    }

    sys.Shutdown();
}

TEST(RemoteDebugSystem_LoopbackHeartbeat)
{
    auto& sys = Spark::RemoteDebug::RemoteDebugSystem::GetInstance();
    sys.Initialize();
    sys.EnableLoopback();

    sys.GetClient()->SendCommand({"heartbeat", "", 99, 0.0f});
    sys.Update(0.016f);

    auto responses = sys.GetClient()->PollResponses();
    EXPECT_EQ(static_cast<size_t>(1), responses.size());
    if (!responses.empty())
    {
        EXPECT_EQ(std::string("heartbeat"), responses[0].type);
    }

    sys.Shutdown();
}

// ============================================================================
// Session state
// ============================================================================

TEST(RemoteDebugSystem_SessionUptime)
{
    auto& sys = Spark::RemoteDebug::RemoteDebugSystem::GetInstance();
    sys.Initialize();
    sys.EnableLoopback();

    sys.Update(1.0f);
    sys.Update(1.0f);

    const auto* serverSession = sys.GetServerSession();
    ASSERT_TRUE(serverSession != nullptr);
    EXPECT_GT(serverSession->GetUptime(), 1.5f);

    sys.Shutdown();
}

TEST(RemoteDebugSystem_ClientPropertyRequest)
{
    auto& sys = Spark::RemoteDebug::RemoteDebugSystem::GetInstance();
    sys.Initialize();
    sys.EnableLoopback();

    uint32_t reqId = sys.GetClient()->GetProperty("player.health");
    sys.Update(0.016f);

    auto responses = sys.GetClient()->PollResponses();
    EXPECT_EQ(static_cast<size_t>(1), responses.size());
    if (!responses.empty())
    {
        EXPECT_EQ(std::string("property_value"), responses[0].type);
        EXPECT_EQ(reqId, responses[0].requestId);
    }

    sys.Shutdown();
}

TEST(RemoteDebugSystem_MultipleCommandsInFlight)
{
    auto& sys = Spark::RemoteDebug::RemoteDebugSystem::GetInstance();
    sys.Initialize();
    sys.EnableLoopback();

    sys.GetClient()->ExecuteConsoleCommand("cmd1");
    sys.GetClient()->GetProperty("a.b");
    sys.GetClient()->RequestPerformanceSnapshot();

    sys.Update(0.016f);

    auto responses = sys.GetClient()->PollResponses();
    EXPECT_EQ(static_cast<size_t>(3), responses.size());
    if (responses.size() == 3)
    {
        EXPECT_TRUE(IsAccessDenied(responses[0]));
        EXPECT_EQ(std::string("property_value"), responses[1].type);
        EXPECT_EQ(std::string("profile_data"), responses[2].type);
    }

    sys.Shutdown();
}

TEST(RemoteDebugSystem_ConsoleStatus)
{
    auto& sys = Spark::RemoteDebug::RemoteDebugSystem::GetInstance();
    sys.Initialize();
    sys.EnableLoopback();

    std::string status = sys.Console_GetStatus();
    EXPECT_STR_CONTAINS(status, "loopback");

    sys.Shutdown();
}
