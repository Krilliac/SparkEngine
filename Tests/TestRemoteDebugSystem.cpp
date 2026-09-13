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
        static constexpr bool kCanMintLegacyPrincipal = requires(Server& server) {
            server.IssueTrustedLoopbackPrincipal(RemoteDebugRole::Administrator, uint64_t{1});
        };

        template <typename Server>
        static constexpr bool kCanMintLoopbackObserver =
            requires(Server& server) { server.IssueLoopbackObserverPrincipal(); };

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

        template <typename Session>
        static constexpr bool kCanQueueOutbound =
            requires(Session& session, const RemoteCommand& command) { session.EnqueueSend(command); };

        template <typename Session>
        static constexpr bool kCanForgeConnectedState =
            requires(Session& session) { session.SetState(SessionState::Connected); };
    };
} // namespace Spark::RemoteDebug

static_assert(!Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::kCanMintLegacyPrincipal<
              Spark::RemoteDebug::RemoteDebugServer>);
static_assert(!Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::kCanMintLoopbackObserver<
              Spark::RemoteDebug::RemoteDebugServer>);
static_assert(!Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::kCanDispatchWithPrincipal<
              Spark::RemoteDebug::RemoteDebugServer>);
static_assert(!Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::kCanQueueWithPrincipal<
              Spark::RemoteDebug::RemoteSession>);
static_assert(
    !Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::kCanQueueOutbound<Spark::RemoteDebug::RemoteSession>);
static_assert(!Spark::RemoteDebug::RemoteDebugAccessControlTestHarness::kCanForgeConnectedState<
              Spark::RemoteDebug::RemoteSession>);

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
} // namespace

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

TEST(RemoteAdmin_ReservedCommandsCannotBeRebound)
{
    using namespace Spark::RemoteDebug;
    auto& sys = RemoteDebugSystem::GetInstance();
    sys.Initialize();
    sys.EnableLoopback();

    bool replacementHandlerCalled = false;
    sys.GetServer()->RegisterCommandHandler("console_cmd", RemoteDebugCapability::Inspect,
                                            [&](const RemoteCommand& command)
                                            {
                                                replacementHandlerCalled = true;
                                                return RemoteCommand{"replacement_ok", "", command.requestId, 0.0f};
                                            });

    const uint32_t requestId = sys.GetClient()->ExecuteConsoleCommand("stat fps");
    sys.Update(0.016f);

    const auto responses = sys.GetClient()->PollResponses();
    EXPECT_EQ(static_cast<size_t>(1), responses.size());
    if (!responses.empty())
    {
        EXPECT_TRUE(IsAccessDenied(responses.front()));
        EXPECT_EQ(requestId, responses.front().requestId);
    }
    EXPECT_FALSE(replacementHandlerCalled);
    EXPECT_TRUE(AuditEndsWith(*sys.GetServer(), RemoteDebugAuditDecision::AuthorizationDenied));
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

TEST(RemoteAdmin_AnonymousDenied)
{
    using namespace Spark::RemoteDebug;
    RemoteDebugServer server;
    server.StartListening(0);

    bool directHandlerCalled = false;
    server.RegisterCommandHandler("direct_probe",
                                  [&](const RemoteCommand& command)
                                  {
                                      directHandlerCalled = true;
                                      return RemoteCommand{"direct_ok", "", command.requestId, 0.0f};
                                  });

    const RemoteCommand command{"direct_probe", "payload", 1, 0.0f};
    const auto response = server.ProcessCommand(command);
    EXPECT_FALSE(directHandlerCalled);
    EXPECT_EQ(std::string("error"), response.type);
    EXPECT_STR_CONTAINS(response.payload, "access_denied");
    EXPECT_TRUE(AuditEndsWith(server, RemoteDebugAuditDecision::AnonymousDenied));

    bool queueHandlerCalled = false;
    server.RegisterCommandHandler("queue_probe",
                                  [&](const RemoteCommand& queued)
                                  {
                                      queueHandlerCalled = true;
                                      return RemoteCommand{"queue_ok", "", queued.requestId, 0.0f};
                                  });

    const RemoteCommand original{"queue_probe", "payload", 7, 0.0f};
    const RemoteCommand copied = original;
    server.GetSession().EnqueueReceived(original);
    server.Update();

    RemoteCommand queuedResponse;
    ASSERT_TRUE(server.GetSession().DequeuePendingSend(queuedResponse));
    EXPECT_FALSE(queueHandlerCalled);
    EXPECT_EQ(std::string("error"), queuedResponse.type);
    EXPECT_STR_CONTAINS(queuedResponse.payload, "access_denied");

    const auto copiedResponse = server.ProcessCommand(copied);
    EXPECT_FALSE(queueHandlerCalled);
    EXPECT_EQ(std::string("error"), copiedResponse.type);
    EXPECT_STR_CONTAINS(copiedResponse.payload, "access_denied");
    EXPECT_TRUE(AuditEndsWith(server, RemoteDebugAuditDecision::AnonymousDenied));

    auto& system = RemoteDebugSystem::GetInstance();
    system.Initialize();
    bool preAuthenticationHandlerCalled = false;
    system.GetServer()->RegisterCommandHandler("preauth_probe", RemoteDebugCapability::Inspect,
                                               [&](const RemoteCommand& queued)
                                               {
                                                   preAuthenticationHandlerCalled = true;
                                                   return RemoteCommand{"preauth_ok", "", queued.requestId, 0.0f};
                                               });
    system.GetClient()->SendCommand({"preauth_probe", "", 1, 0.0f});
    system.EnableLoopback();
    system.Update(0.016f);
    EXPECT_FALSE(preAuthenticationHandlerCalled);
    EXPECT_TRUE(system.GetClient()->PollResponses().empty());
    system.Shutdown();
}

TEST(RemoteAdmin_RoleMatrix)
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

    auto& sys = RemoteDebugSystem::GetInstance();
    sys.Initialize();
    sys.EnableLoopback();

    auto* client = sys.GetClient();
    auto* server = sys.GetServer();
    ASSERT_TRUE(client != nullptr);
    ASSERT_TRUE(server != nullptr);

    bool defaultCapabilityHandlerCalled = false;
    server->RegisterCommandHandler("default_capability_probe", RemoteDebugCapability::None,
                                   [&](const RemoteCommand& command)
                                   {
                                       defaultCapabilityHandlerCalled = true;
                                       return RemoteCommand{"default_capability_ok", "", command.requestId, 0.0f};
                                   });

    const uint32_t inspectId = client->GetProperty("player.health");
    const uint32_t mutationId = client->SetProperty("player.health", "100");
    const uint32_t consoleId = client->ExecuteConsoleCommand("quit");
    constexpr uint32_t defaultCapabilityId = 4;
    client->SendCommand({"default_capability_probe", "", defaultCapabilityId, 0.0f});
    sys.Update(0.016f);

    const auto responses = client->PollResponses();
    EXPECT_EQ(static_cast<size_t>(4), responses.size());
    if (responses.size() == 4)
    {
        EXPECT_EQ(std::string("property_value"), responses[0].type);
        EXPECT_EQ(inspectId, responses[0].requestId);
        EXPECT_TRUE(IsAccessDenied(responses[1]));
        EXPECT_EQ(mutationId, responses[1].requestId);
        EXPECT_TRUE(IsAccessDenied(responses[2]));
        EXPECT_EQ(consoleId, responses[2].requestId);
        EXPECT_TRUE(IsAccessDenied(responses[3]));
        EXPECT_EQ(defaultCapabilityId, responses[3].requestId);
    }
    EXPECT_FALSE(defaultCapabilityHandlerCalled);
    EXPECT_TRUE(AuditEndsWith(*server, Spark::RemoteDebug::RemoteDebugAuditDecision::AuthorizationDenied));

    sys.Shutdown();
}

TEST(RemoteAdmin_ReplayDenied)
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

    std::atomic_uint32_t crossEpochEffects{0};
    server->RegisterCommandHandler("cross_epoch_probe", RemoteDebugCapability::Inspect,
                                   [&](const RemoteCommand& command)
                                   {
                                       ++crossEpochEffects;
                                       return RemoteCommand{"cross_epoch_ok", "", command.requestId, 0.0f};
                                   });
    client->SendCommand({"cross_epoch_probe", "", 2, 0.0f});

    // A new authority epoch must discard the pending old-epoch command rather
    // than attaching the freshly minted principal in PumpLoopback.
    sys.EnableLoopback();
    client = sys.GetClient();
    server = sys.GetServer();
    ASSERT_TRUE(client != nullptr);
    ASSERT_TRUE(server != nullptr);
    sys.Update(0.016f);
    EXPECT_EQ(static_cast<uint32_t>(0), crossEpochEffects.load());
    EXPECT_TRUE(client->PollResponses().empty());

    // The same request identifier is valid in the new grant only when submitted
    // after the new connected epoch is established.
    client->SendCommand({"cross_epoch_probe", "", 1, 0.0f});
    sys.Update(0.016f);
    const auto currentEpochResponses = client->PollResponses();
    EXPECT_EQ(static_cast<uint32_t>(1), crossEpochEffects.load());
    EXPECT_EQ(static_cast<size_t>(1), currentEpochResponses.size());
    if (!currentEpochResponses.empty())
        EXPECT_EQ(std::string("cross_epoch_ok"), currentEpochResponses.front().type);
    sys.Shutdown();
}

TEST(RemoteAdmin_RateLimited)
{
    using namespace Spark::RemoteDebug;
    auto& sys = RemoteDebugSystem::GetInstance();
    sys.Initialize();
    sys.EnableLoopback();

    auto* client = sys.GetClient();
    auto* server = sys.GetServer();
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

namespace
{
    enum class ResponseEpochTransition
    {
        StartListening,
        StopListening
    };

    void VerifyAllowedResponseCannotCrossEpoch(ResponseEpochTransition transition)
    {
        using namespace Spark::RemoteDebug;
        auto& system = RemoteDebugSystem::GetInstance();
        system.Initialize();
        system.EnableLoopback();

        auto* client = system.GetClient();
        auto* server = system.GetServer();
        ASSERT_TRUE(client != nullptr);
        ASSERT_TRUE(server != nullptr);

        struct QueuePrimed
        {
        };
        server->RegisterCommandHandler("prime_authenticated_queue", RemoteDebugCapability::Inspect,
                                       [](const RemoteCommand&) -> RemoteCommand { throw QueuePrimed{}; });

        std::atomic_uint32_t effectCount{0};
        server->RegisterCommandHandler("epoch_response_probe", RemoteDebugCapability::Inspect,
                                       [&](const RemoteCommand& command)
                                       {
                                           ++effectCount;
                                           return RemoteCommand{"epoch_response_ok", "", command.requestId, 0.0f};
                                       });

        // Prime one authenticated request without allowing RemoteDebugSystem to
        // run its post-dispatch response pump. The target request remains in the
        // server queue with its original valid principal.
        client->SendCommand({"prime_authenticated_queue", "", 1, 0.0f});
        client->SendCommand({"epoch_response_probe", "", 2, 0.0f});
        bool queuePrimed = false;
        try
        {
            system.Update(0.016f);
        }
        catch (const QueuePrimed&)
        {
            queuePrimed = true;
        }
        ASSERT_TRUE(queuePrimed);

        std::mutex barrierMutex;
        std::condition_variable responseAtBarrier;
        std::condition_variable releaseResponse;
        std::condition_variable transitionTriedLock;
        bool responseReady = false;
        bool allowResponseEnqueue = false;
        bool transitionTryObserved = false;
        bool transitionTryAcquired = true;

        auto testSeam = std::make_shared<RemoteDebugResponseEpochTestSeam>();
        testSeam->beforeResponseEnqueue = [&]
        {
            std::unique_lock lock(barrierMutex);
            responseReady = true;
            responseAtBarrier.notify_one();
            releaseResponse.wait(lock, [&] { return allowResponseEnqueue; });
        };
        testSeam->onEpochTransitionTryLock = [&](bool acquired)
        {
            std::lock_guard lock(barrierMutex);
            transitionTryAcquired = acquired;
            transitionTryObserved = true;
            transitionTriedLock.notify_one();
        };
        server->SetResponseEpochTestSeam(testSeam);

        std::thread updateThread([server] { server->Update(); });
        {
            std::unique_lock lock(barrierMutex);
            responseAtBarrier.wait(lock, [&] { return responseReady; });
        }

        std::atomic_bool transitionReturned{false};
        std::thread transitionThread(
            [&]
            {
                if (transition == ResponseEpochTransition::StartListening)
                    server->StartListening(0);
                else
                    server->StopListening();
                transitionReturned.store(true, std::memory_order_release);
            });
        {
            std::unique_lock lock(barrierMutex);
            transitionTriedLock.wait(lock, [&] { return transitionTryObserved; });
            EXPECT_FALSE(transitionTryAcquired);
            EXPECT_FALSE(transitionReturned.load(std::memory_order_acquire));
            allowResponseEnqueue = true;
        }
        releaseResponse.notify_one();

        updateThread.join();
        transitionThread.join();
        server->SetResponseEpochTestSeam({});

        EXPECT_TRUE(transitionReturned.load(std::memory_order_acquire));
        EXPECT_EQ(static_cast<uint32_t>(1), effectCount.load());
        EXPECT_TRUE(AuditEndsWith(*server, RemoteDebugAuditDecision::Allowed));

        RemoteCommand staleResponse;
        EXPECT_FALSE(server->GetSession().DequeuePendingSend(staleResponse));
        EXPECT_TRUE(client->PollResponses().empty());

        // Prove the empty queues are an epoch boundary, not blanket response loss.
        system.EnableLoopback();
        client->SendCommand({"epoch_response_probe", "", 1, 0.0f});
        system.Update(0.016f);
        const auto responses = client->PollResponses();
        EXPECT_EQ(static_cast<size_t>(1), responses.size());
        if (!responses.empty())
        {
            EXPECT_EQ(static_cast<uint32_t>(1), responses.front().requestId);
            EXPECT_EQ(std::string("epoch_response_ok"), responses.front().type);
        }
        EXPECT_EQ(static_cast<uint32_t>(2), effectCount.load());
        system.Shutdown();
    }
} // namespace

TEST(RemoteDebug_ResponseEpochStartListening)
{
    VerifyAllowedResponseCannotCrossEpoch(ResponseEpochTransition::StartListening);
}

TEST(RemoteDebug_ResponseEpochStopListening)
{
    VerifyAllowedResponseCannotCrossEpoch(ResponseEpochTransition::StopListening);
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
