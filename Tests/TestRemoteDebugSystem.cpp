// TestRemoteDebugSystem.cpp - Tests for Spark::RemoteDebug::RemoteDebugSystem
#include "TestFramework.h"
#define SPARK_REMOTE_DEBUG_TESTING 1
#include "Engine/RemoteDebug/RemoteDebugSystem.h"
#undef SPARK_REMOTE_DEBUG_TESTING

// The production surface intentionally has no principal-minting API. This
// focused test harness reaches the same private server-owned path that the
// in-process loopback uses, without making that authority available to clients
// or RemoteCommand serialization.
namespace Spark::RemoteDebug
{
    class RemoteDebugAccessControlTestHarness
    {
      public:
        static RemoteDebugPrincipal IssueTrustedLoopbackPrincipal(RemoteDebugServer& server, RemoteDebugRole role,
                                                                   uint64_t lifetimeMilliseconds)
        {
            return server.IssueTrustedLoopbackPrincipal(role, lifetimeMilliseconds);
        }

        static RemoteCommand Dispatch(RemoteDebugServer& server, const RemoteCommand& command,
                                      const RemoteDebugPrincipal& principal)
        {
            return server.ProcessCommandWithPrincipal(command, principal);
        }

        static void Enqueue(RemoteDebugServer& server, const RemoteCommand& command,
                            const RemoteDebugPrincipal& principal)
        {
            server.GetSession().EnqueueReceivedWithPrincipal(command, principal);
        }
    };
} // namespace Spark::RemoteDebug

namespace
{
    [[nodiscard]] bool AuditEndsWith(const Spark::RemoteDebug::RemoteDebugServer& server,
                                     Spark::RemoteDebug::RemoteDebugAuditDecision decision)
    {
        const auto events = server.GetAuditEvents();
        return !events.empty() && events.back().decision == decision;
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
    sys.GetServer()->RegisterCommandHandler("test_cmd",
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

TEST(RemoteDebugSystem_DefaultAndForgedPrincipalsFailClosed)
{
    Spark::RemoteDebug::RemoteDebugServer server;
    server.StartListening(0);
    server.RegisterCommandHandler("principal_probe", [](const Spark::RemoteDebug::RemoteCommand& command)
                                  { return Spark::RemoteDebug::RemoteCommand{"principal_ok", "", command.requestId, 0.0f}; });

    Spark::RemoteDebug::RemoteDebugPrincipal defaultPrincipal;
    Spark::RemoteDebug::RemoteDebugPrincipal forgedByCopy = defaultPrincipal;
    EXPECT_FALSE(defaultPrincipal.IsAuthenticated());
    EXPECT_FALSE(forgedByCopy.IsAuthenticated());

    const Spark::RemoteDebug::RemoteCommand command{"principal_probe", "payload", 3, 0.0f};
    const auto response = Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::Dispatch(
        server, command, forgedByCopy);
    EXPECT_EQ(std::string("error"), response.type);
    EXPECT_STR_CONTAINS(response.payload, "access_denied");
    EXPECT_TRUE(AuditEndsWith(server, Spark::RemoteDebug::RemoteDebugAuditDecision::AnonymousDenied));
}

TEST(RemoteDebugSystem_RolePolicyDeniesObserverConsole)
{
    Spark::RemoteDebug::RemoteDebugServer server;
    server.StartListening(0);

    bool consoleHandlerCalled = false;
    server.RegisterCommandHandler("console_cmd", Spark::RemoteDebug::RemoteDebugCapability::ExecuteConsole,
                                  [&](const Spark::RemoteDebug::RemoteCommand& command)
                                  {
                                      consoleHandlerCalled = true;
                                      return Spark::RemoteDebug::RemoteCommand{"console_ok", "", command.requestId, 0.0f};
                                  });

    const auto observer = Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::IssueTrustedLoopbackPrincipal(
        server, Spark::RemoteDebug::RemoteDebugRole::Observer,
        Spark::RemoteDebug::RemoteDebugAccessControl::kDefaultLoopbackLifetimeMilliseconds);
    const auto inspectResponse = Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::Dispatch(
        server, {"property_get", "player.health", 1, 0.0f}, observer);
    EXPECT_EQ(std::string("property_value"), inspectResponse.type);

    const auto deniedConsoleResponse = Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::Dispatch(
        server, {"console_cmd", "{\"command\":\"quit\"}", 2, 0.0f}, observer);
    EXPECT_FALSE(consoleHandlerCalled);
    EXPECT_EQ(std::string("error"), deniedConsoleResponse.type);
    EXPECT_STR_CONTAINS(deniedConsoleResponse.payload, "access_denied");
    EXPECT_TRUE(AuditEndsWith(server, Spark::RemoteDebug::RemoteDebugAuditDecision::AuthorizationDenied));

    const auto operatorPrincipal = Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::IssueTrustedLoopbackPrincipal(
        server, Spark::RemoteDebug::RemoteDebugRole::Operator,
        Spark::RemoteDebug::RemoteDebugAccessControl::kDefaultLoopbackLifetimeMilliseconds);
    const auto setResponse = Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::Dispatch(
        server, {"property_set", "{\"path\":\"player.health\",\"value\":100}", 1, 0.0f}, operatorPrincipal);
    EXPECT_EQ(std::string("property_set_result"), setResponse.type);

    const auto administrator = Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::IssueTrustedLoopbackPrincipal(
        server, Spark::RemoteDebug::RemoteDebugRole::Administrator,
        Spark::RemoteDebug::RemoteDebugAccessControl::kDefaultLoopbackLifetimeMilliseconds);
    const auto allowedConsoleResponse = Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::Dispatch(
        server, {"console_cmd", "{\"command\":\"stat fps\"}", 1, 0.0f}, administrator);
    EXPECT_TRUE(consoleHandlerCalled);
    EXPECT_EQ(std::string("console_ok"), allowedConsoleResponse.type);
    EXPECT_TRUE(AuditEndsWith(server, Spark::RemoteDebug::RemoteDebugAuditDecision::Allowed));
}

TEST(RemoteDebugSystem_ReplayExpiryAndRateLimitFailClosed)
{
    Spark::RemoteDebug::RemoteDebugServer server;
    server.StartListening(0);
    server.RegisterCommandHandler("rate_probe", Spark::RemoteDebug::RemoteDebugCapability::Inspect,
                                  [](const Spark::RemoteDebug::RemoteCommand& command)
                                  { return Spark::RemoteDebug::RemoteCommand{"rate_ok", "", command.requestId, 0.0f}; });

    const auto replayPrincipal = Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::IssueTrustedLoopbackPrincipal(
        server, Spark::RemoteDebug::RemoteDebugRole::Administrator,
        Spark::RemoteDebug::RemoteDebugAccessControl::kDefaultLoopbackLifetimeMilliseconds);
    const Spark::RemoteDebug::RemoteCommand replayedCommand{"heartbeat", "", 1, 0.0f};
    const auto firstResponse = Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::Dispatch(server, replayedCommand,
                                                                                                    replayPrincipal);
    const auto replayResponse = Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::Dispatch(server, replayedCommand,
                                                                                                     replayPrincipal);
    EXPECT_EQ(std::string("heartbeat"), firstResponse.type);
    EXPECT_EQ(std::string("error"), replayResponse.type);
    EXPECT_TRUE(AuditEndsWith(server, Spark::RemoteDebug::RemoteDebugAuditDecision::ReplayDenied));

    const auto expiredPrincipal = Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::IssueTrustedLoopbackPrincipal(
        server, Spark::RemoteDebug::RemoteDebugRole::Administrator, 0);
    const auto expiredResponse = Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::Dispatch(
        server, {"heartbeat", "", 1, 0.0f}, expiredPrincipal);
    EXPECT_EQ(std::string("error"), expiredResponse.type);
    EXPECT_TRUE(AuditEndsWith(server, Spark::RemoteDebug::RemoteDebugAuditDecision::ExpiredPrincipalDenied));

    const auto ratePrincipal = Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::IssueTrustedLoopbackPrincipal(
        server, Spark::RemoteDebug::RemoteDebugRole::Observer,
        Spark::RemoteDebug::RemoteDebugAccessControl::kDefaultLoopbackLifetimeMilliseconds);
    for (uint32_t requestId = 1; requestId <= Spark::RemoteDebug::RemoteDebugAccessControl::kMaxRequestsPerWindow;
         ++requestId)
    {
        const auto response = Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::Dispatch(
            server, {"rate_probe", "", requestId, 0.0f}, ratePrincipal);
        EXPECT_EQ(std::string("rate_ok"), response.type);
    }
    const auto rateLimitedResponse = Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::Dispatch(
        server, {"rate_probe", "", Spark::RemoteDebug::RemoteDebugAccessControl::kMaxRequestsPerWindow + 1, 0.0f},
        ratePrincipal);
    EXPECT_EQ(std::string("error"), rateLimitedResponse.type);
    EXPECT_STR_CONTAINS(rateLimitedResponse.payload, "access_denied");
    EXPECT_TRUE(AuditEndsWith(server, Spark::RemoteDebug::RemoteDebugAuditDecision::RateLimitedDenied));
}

TEST(RemoteDebugSystem_AuditRecordsDispositionWithoutPayloadsOrGrants)
{
    Spark::RemoteDebug::RemoteDebugServer server;
    server.StartListening(0);
    const auto principal = Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::IssueTrustedLoopbackPrincipal(
        server, Spark::RemoteDebug::RemoteDebugRole::Administrator,
        Spark::RemoteDebug::RemoteDebugAccessControl::kDefaultLoopbackLifetimeMilliseconds);

    const auto response = Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::Dispatch(
        server, {"heartbeat", "sensitive-payload-must-not-enter-audit", 1, 0.0f}, principal);
    EXPECT_EQ(std::string("heartbeat"), response.type);

    const auto events = server.GetAuditEvents();
    ASSERT_TRUE(!events.empty());
    const auto& event = events.back();
    EXPECT_TRUE(event.decision == Spark::RemoteDebug::RemoteDebugAuditDecision::Allowed);
    EXPECT_EQ(std::string("trusted-local-loopback"), event.principal);
    EXPECT_EQ(std::string("in-process-loopback"), event.source);
    EXPECT_EQ(std::string("heartbeat"), event.commandType);
    EXPECT_EQ(static_cast<uint32_t>(1), event.requestId);
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

    // Send a console command from client
    uint32_t reqId = sys.GetClient()->ExecuteConsoleCommand("stat fps");

    // Pump loopback and server update
    sys.Update(0.016f);

    // Client should have received the response
    auto responses = sys.GetClient()->PollResponses();
    EXPECT_EQ(static_cast<size_t>(1), responses.size());
    if (!responses.empty())
    {
        EXPECT_EQ(std::string("console_cmd_result"), responses[0].type);
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
