/**
 * @file TestSparkGatewayCoordinator.cpp
 * @brief Deterministic fenced-handoff and gateway failure-matrix tests.
 */

#include "TestFramework.h"
#include "GatewayCoordinator.h"
#include "GuardedGatewayAuthenticator.h"

#include <chrono>
#include <deque>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>

using namespace Spark::Gateway;
namespace Net = Spark::Net;

namespace
{
    class TestAuthenticator final : public IGatewayAuthenticator
    {
      public:
        AuthenticationResult Authenticate(const AdmissionRequest& request) override
        {
            ++calls;
            if (delay.count() > 0)
                std::this_thread::sleep_for(delay);
            if (throwWithCredential)
                throw std::runtime_error("identity backend down while checking " + request.credential);
            if (!accept)
                return {false, {}, echoCredential ? "denied credential " + request.credential : "denied"};
            return {true, "principal:" + request.playerName, {}};
        }
        bool IsReady() const override { return ready; }

        bool ready = true;
        bool accept = true;
        bool throwWithCredential = false;
        bool echoCredential = false;
        std::chrono::milliseconds delay{0};
        int calls = 0;
    };

    class ScriptedControlPlane final : public IAreaControlPlane
    {
      public:
        bool IsReady() const override { return ready; }
        bool IsEndpointReady(Net::AreaID id) const override { return ready && !offline.contains(id); }
        HandoffOperationResult Prepare(const HandoffCommand& command) override
        {
            return Next("prepare", prepare, command);
        }
        HandoffOperationResult Transfer(const HandoffCommand& command) override
        {
            return Next("transfer", transfer, command);
        }
        HandoffOperationResult Commit(const HandoffCommand& command) override
        {
            return Next("commit", commit, command);
        }
        HandoffOperationResult Acknowledge(const HandoffCommand& command) override
        {
            return Next("ack", acknowledge, command);
        }
        HandoffOperationResult Abort(const HandoffCommand& command) override { return Next("abort", abort, command); }

        HandoffOperationResult Next(const char* phase, std::deque<HandoffOperationResult>& script,
                                    const HandoffCommand& command)
        {
            calls.emplace_back(phase);
            epochs.push_back(command.epoch);
            if (script.empty())
                return HandoffOperationResult::Applied;
            const auto result = script.front();
            script.pop_front();
            return result;
        }

        bool ready = true;
        std::unordered_set<Net::AreaID> offline;
        std::deque<HandoffOperationResult> prepare;
        std::deque<HandoffOperationResult> transfer;
        std::deque<HandoffOperationResult> commit;
        std::deque<HandoffOperationResult> acknowledge;
        std::deque<HandoffOperationResult> abort;
        std::vector<std::string> calls;
        std::vector<uint64_t> epochs;
    };

    std::vector<AreaEndpoint> BuildAreas()
    {
        AreaEndpoint first;
        first.area.areaName = "Town";
        first.area.port = 31001;
        first.area.interServerPort = 31101;
        first.host = "127.0.0.1";
        AreaEndpoint second;
        second.area.areaName = "Forest";
        second.area.port = 31002;
        second.area.interServerPort = 31102;
        second.host = "127.0.0.1";
        return {first, second};
    }

    AdmissionRequest BuildAdmission(Net::ClientID client = 7)
    {
        AdmissionRequest request;
        request.clientId = client;
        request.sessionId = "session-" + std::to_string(client);
        request.playerName = "Player" + std::to_string(client);
        request.credential = "opaque-test-credential";
        request.spawnPosition = {1.0f, 1.0f, 1.0f};
        return request;
    }

    struct GatewayFixture
    {
        GatewayFixture() : coordinator(world, authenticator, control)
        {
            Net::WorldServerConfig config;
            config.worldName = "GatewayTest";
            config.tickRate = 100.0f;
            started = world.Start(config);
            registered = started && coordinator.RegisterAreas(BuildAreas());
        }
        ~GatewayFixture() { world.Stop(); }

        Net::WorldServer world;
        TestAuthenticator authenticator;
        ScriptedControlPlane control;
        GatewayCoordinator coordinator;
        bool started = false;
        bool registered = false;
    };
} // namespace

TEST(SparkGateway_RejectsBeforeAuthentication)
{
    GatewayFixture fixture;
    EXPECT_TRUE(fixture.started);
    EXPECT_TRUE(fixture.registered);
    fixture.authenticator.accept = false;
    const RouteResult result = fixture.coordinator.Admit(BuildAdmission());
    EXPECT_FALSE(result.accepted);
    EXPECT_TRUE(result.failure == RouteFailure::AuthenticationFailed);
    EXPECT_EQ(fixture.coordinator.GetSessionCount(), static_cast<size_t>(0));
}

TEST(SparkGateway_DuplicateClientIdCannotOpenSecondSession)
{
    // WorldServer keys players by ClientID. A second gateway session for a bound client
    // would alias that record, leave the world count flat and escape the world cap.
    GatewayFixture fixture;
    ASSERT_TRUE(fixture.registered);
    const RouteResult first = fixture.coordinator.Admit(BuildAdmission(7));
    ASSERT_TRUE(first.accepted);

    AdmissionRequest alias = BuildAdmission(7);
    alias.sessionId = "session-7-alias";
    const RouteResult rejected = fixture.coordinator.Admit(alias);
    EXPECT_FALSE(rejected.accepted);
    EXPECT_TRUE(rejected.failure == RouteFailure::DuplicateSession);
    EXPECT_EQ(fixture.coordinator.GetSessionCount(), static_cast<size_t>(1));
    EXPECT_FALSE(fixture.coordinator.GetSession("session-7-alias").has_value());

    // Once the first session is retired the client may be admitted again.
    EXPECT_TRUE(fixture.coordinator.Disconnect(first.session.sessionId));
    EXPECT_TRUE(fixture.coordinator.Admit(alias).accepted);
    EXPECT_EQ(fixture.coordinator.GetSessionCount(), static_cast<size_t>(1));
}

TEST(SparkGateway_SessionTableCappedIndependentlyOfWorldMirror)
{
    Net::WorldServer world;
    TestAuthenticator authenticator;
    ScriptedControlPlane control;
    GatewayCoordinator coordinator(world, authenticator, control);
    Net::WorldServerConfig config;
    config.worldName = "GatewayCapTest";
    config.tickRate = 100.0f;
    config.maxTotalClients = 1;
    ASSERT_TRUE(world.Start(config));
    ASSERT_TRUE(coordinator.RegisterAreas(BuildAreas()));

    ASSERT_TRUE(coordinator.Admit(BuildAdmission(7)).accepted);
    // The WorldServer mirror loses the player while the gateway session is still live.
    world.HandlePlayerDisconnect(7);
    const RouteResult overCap = coordinator.Admit(BuildAdmission(8));
    EXPECT_FALSE(overCap.accepted);
    EXPECT_TRUE(overCap.failure == RouteFailure::CapacityReached);
    EXPECT_EQ(coordinator.GetSessionCount(), static_cast<size_t>(1));
    world.Stop();
}

TEST(SparkGateway_HandoffCompletesOnlyAfterCommitAcknowledgement)
{
    GatewayFixture fixture;
    const RouteResult route = fixture.coordinator.Admit(BuildAdmission());
    EXPECT_TRUE(route.accepted);
    const Net::AreaID source = route.session.authoritativeArea;
    const Net::AreaID target = source == 1 ? 2 : 1;
    const auto epoch = fixture.coordinator.BeginHandoff(route.session.sessionId, target);
    EXPECT_TRUE(epoch.has_value());

    EXPECT_TRUE(fixture.coordinator.AdvanceHandoff(route.session.sessionId, *epoch) == AdvanceResult::Advanced);
    EXPECT_EQ(fixture.coordinator.GetSession(route.session.sessionId)->authoritativeArea, source);
    EXPECT_TRUE(fixture.coordinator.AdvanceHandoff(route.session.sessionId, *epoch) == AdvanceResult::Advanced);
    EXPECT_EQ(fixture.coordinator.GetSession(route.session.sessionId)->authoritativeArea, source);
    fixture.control.commit.push_back(HandoffOperationResult::Duplicate);
    EXPECT_TRUE(fixture.coordinator.AdvanceHandoff(route.session.sessionId, *epoch) == AdvanceResult::Advanced);
    EXPECT_EQ(fixture.coordinator.GetSession(route.session.sessionId)->authoritativeArea, source);
    EXPECT_TRUE(fixture.coordinator.AdvanceHandoff(route.session.sessionId, *epoch) == AdvanceResult::Completed);

    const auto completed = fixture.coordinator.GetSession(route.session.sessionId);
    EXPECT_TRUE(completed->state == SessionState::Active);
    EXPECT_EQ(completed->authoritativeArea, target);
    EXPECT_EQ(completed->targetArea, Net::INVALID_AREA);
}

TEST(SparkGateway_UnavailablePhaseIsRetryableAndEpochIsStable)
{
    GatewayFixture fixture;
    const RouteResult route = fixture.coordinator.Admit(BuildAdmission());
    const Net::AreaID target = route.session.authoritativeArea == 1 ? 2 : 1;
    const auto epoch = fixture.coordinator.BeginHandoff(route.session.sessionId, target);
    fixture.control.prepare = {HandoffOperationResult::Unavailable, HandoffOperationResult::Duplicate};

    EXPECT_TRUE(fixture.coordinator.AdvanceHandoff(route.session.sessionId, *epoch) == AdvanceResult::WaitingForRetry);
    const auto waiting = fixture.coordinator.GetSession(route.session.sessionId);
    EXPECT_TRUE(waiting->state == SessionState::Preparing);
    EXPECT_EQ(waiting->epoch, *epoch);
    EXPECT_TRUE(fixture.coordinator.BeginHandoff(route.session.sessionId, target) == epoch);
    EXPECT_TRUE(fixture.coordinator.AdvanceHandoff(route.session.sessionId, *epoch) == AdvanceResult::Advanced);
}

TEST(SparkGateway_RejectionAbortsWithoutDiscardingSourceAuthority)
{
    GatewayFixture fixture;
    const RouteResult route = fixture.coordinator.Admit(BuildAdmission());
    const Net::AreaID source = route.session.authoritativeArea;
    const Net::AreaID target = source == 1 ? 2 : 1;
    const auto epoch = fixture.coordinator.BeginHandoff(route.session.sessionId, target);
    fixture.control.transfer.push_back(HandoffOperationResult::Rejected);
    fixture.control.abort = {HandoffOperationResult::Unavailable, HandoffOperationResult::Duplicate};

    EXPECT_TRUE(fixture.coordinator.AdvanceHandoff(route.session.sessionId, *epoch) == AdvanceResult::Advanced);
    EXPECT_TRUE(fixture.coordinator.AdvanceHandoff(route.session.sessionId, *epoch) == AdvanceResult::Aborting);
    EXPECT_TRUE(fixture.coordinator.AdvanceHandoff(route.session.sessionId, *epoch) == AdvanceResult::WaitingForRetry);
    EXPECT_EQ(fixture.coordinator.GetSession(route.session.sessionId)->authoritativeArea, source);
    EXPECT_TRUE(fixture.coordinator.AdvanceHandoff(route.session.sessionId, *epoch) == AdvanceResult::Completed);
    EXPECT_EQ(fixture.coordinator.GetSession(route.session.sessionId)->authoritativeArea, source);
    EXPECT_TRUE(fixture.coordinator.GetSession(route.session.sessionId)->state == SessionState::Active);
}

TEST(SparkGateway_RejectsStaleEpochWithoutCallingControlPlane)
{
    GatewayFixture fixture;
    const RouteResult route = fixture.coordinator.Admit(BuildAdmission());
    const Net::AreaID target = route.session.authoritativeArea == 1 ? 2 : 1;
    const auto epoch = fixture.coordinator.BeginHandoff(route.session.sessionId, target);
    const size_t callsBefore = fixture.control.calls.size();
    EXPECT_TRUE(fixture.coordinator.AdvanceHandoff(route.session.sessionId, *epoch + 1) == AdvanceResult::StaleEpoch);
    EXPECT_EQ(fixture.control.calls.size(), callsBefore);
}

TEST(SparkGateway_DrainRejectsAdmissionUntilHandoffResolves)
{
    GatewayFixture fixture;
    const RouteResult route = fixture.coordinator.Admit(BuildAdmission());
    const Net::AreaID target = route.session.authoritativeArea == 1 ? 2 : 1;
    const auto epoch = fixture.coordinator.BeginHandoff(route.session.sessionId, target);
    EXPECT_TRUE(epoch.has_value());
    fixture.coordinator.BeginDrain();

    EXPECT_FALSE(fixture.coordinator.IsReady());
    EXPECT_FALSE(fixture.coordinator.CanShutdown());
    const RouteResult rejected = fixture.coordinator.Admit(BuildAdmission(8));
    EXPECT_TRUE(rejected.failure == RouteFailure::NotReady);

    // A rejected prepare moves to abort; the abort acknowledgement restores
    // source authority and makes shutdown safe without admitting new work.
    fixture.control.prepare.push_back(HandoffOperationResult::Rejected);
    EXPECT_TRUE(fixture.coordinator.AdvanceHandoff(route.session.sessionId, *epoch) == AdvanceResult::Aborting);
    EXPECT_TRUE(fixture.coordinator.AdvanceHandoff(route.session.sessionId, *epoch) == AdvanceResult::Completed);
    EXPECT_TRUE(fixture.coordinator.CanShutdown());
}

TEST(SparkGateway_EndpointHealthUpdatesRoutingMirror)
{
    GatewayFixture fixture;
    EXPECT_EQ(fixture.world.GetStats().activeAreas, 2u);
    fixture.control.offline.insert(1);
    EXPECT_FALSE(fixture.coordinator.IsReady());
    EXPECT_EQ(fixture.world.GetStats().activeAreas, 1u);
    fixture.control.offline.insert(2);
    EXPECT_FALSE(fixture.coordinator.IsReady());
    EXPECT_EQ(fixture.world.GetStats().activeAreas, 0u);
    fixture.control.offline.clear();
    EXPECT_TRUE(fixture.coordinator.IsReady());
    EXPECT_EQ(fixture.world.GetStats().activeAreas, 2u);
}

// ============================================================================
// SparkGateway_GuardedAuthenticator — the front GatewayCoordinator installs around
// every authenticator (docs/specs/online-services.md section 5.2, NET-110).
// ============================================================================

TEST(SparkGateway_GuardedAuthenticator_ThrowIsContainedWithoutCredential)
{
    GatewayFixture fixture;
    ASSERT_TRUE(fixture.registered);
    fixture.authenticator.throwWithCredential = true;
    const AdmissionRequest request = BuildAdmission();

    RouteResult result;
    try
    {
        result = fixture.coordinator.Admit(request);
    }
    catch (...)
    {
        EXPECT_TRUE(false); // An authenticator exception must never reach the transport thread.
    }
    EXPECT_FALSE(result.accepted);
    EXPECT_TRUE(result.failure == RouteFailure::AuthenticationFailed);
    EXPECT_EQ(result.reason, std::string("Authentication backend fault"));
    EXPECT_TRUE(result.reason.find(request.credential) == std::string::npos);
    EXPECT_EQ(fixture.coordinator.GetSessionCount(), static_cast<size_t>(0));
    const GatewayAuthenticatorHealth health = fixture.coordinator.GetAuthenticationHealth();
    EXPECT_EQ(health.faults, 1u);
    EXPECT_EQ(health.consecutiveFaults, 1u);
    EXPECT_FALSE(health.circuitOpen);
}

TEST(SparkGateway_GuardedAuthenticator_CircuitOpensAfterConsecutiveFaults)
{
    GatewayFixture fixture;
    ASSERT_TRUE(fixture.registered);
    fixture.authenticator.throwWithCredential = true;
    for (Net::ClientID client = 10; client < 14; ++client)
        EXPECT_FALSE(fixture.coordinator.Admit(BuildAdmission(client)).accepted);
    // Faults below the threshold keep the gateway ready: the adapter is still being asked.
    EXPECT_TRUE(fixture.coordinator.IsReady());
    EXPECT_FALSE(fixture.coordinator.Admit(BuildAdmission(14)).accepted);
    GatewayAuthenticatorHealth health = fixture.coordinator.GetAuthenticationHealth();
    EXPECT_EQ(health.consecutiveFaults, 5u);
    EXPECT_TRUE(health.circuitOpen);
    // Failing fast: health readiness drops, so /health does not report ready while every
    // admission is rejected.
    EXPECT_FALSE(fixture.coordinator.IsReady());

    // Open: even a recovered backend is not called until the 30 s cooldown has elapsed.
    fixture.authenticator.throwWithCredential = false;
    const RouteResult rejected = fixture.coordinator.Admit(BuildAdmission(15));
    EXPECT_FALSE(rejected.accepted);
    EXPECT_TRUE(rejected.failure == RouteFailure::AuthenticationFailed);
    EXPECT_EQ(rejected.reason, std::string("Authentication backend unavailable (circuit open)"));
    EXPECT_EQ(fixture.authenticator.calls, 5);
    health = fixture.coordinator.GetAuthenticationHealth();
    EXPECT_EQ(health.rejectedWhileOpen, 1u);
    EXPECT_EQ(health.faults, 5u);
}

TEST(SparkGateway_GuardedAuthenticator_RejectionsDoNotOpenCircuit)
{
    GatewayFixture fixture;
    ASSERT_TRUE(fixture.registered);
    // Denying a credential is a healthy answer: any number of them keeps the circuit closed.
    fixture.authenticator.accept = false;
    for (Net::ClientID client = 20; client < 27; ++client)
        EXPECT_EQ(fixture.coordinator.Admit(BuildAdmission(client)).reason, std::string("denied"));
    GatewayAuthenticatorHealth health = fixture.coordinator.GetAuthenticationHealth();
    EXPECT_EQ(health.rejected, 7u);
    EXPECT_EQ(health.faults, 0u);
    EXPECT_FALSE(health.circuitOpen);

    // Faults must be consecutive: a healthy answer in between resets the streak.
    fixture.authenticator.throwWithCredential = true;
    for (Net::ClientID client = 30; client < 34; ++client)
        EXPECT_FALSE(fixture.coordinator.Admit(BuildAdmission(client)).accepted);
    fixture.authenticator.throwWithCredential = false;
    EXPECT_FALSE(fixture.coordinator.Admit(BuildAdmission(34)).accepted);
    fixture.authenticator.throwWithCredential = true;
    for (Net::ClientID client = 35; client < 39; ++client)
        EXPECT_FALSE(fixture.coordinator.Admit(BuildAdmission(client)).accepted);
    health = fixture.coordinator.GetAuthenticationHealth();
    EXPECT_EQ(health.faults, 8u);
    EXPECT_EQ(health.consecutiveFaults, 4u);
    EXPECT_FALSE(health.circuitOpen);

    fixture.authenticator.throwWithCredential = false;
    fixture.authenticator.accept = true;
    EXPECT_TRUE(fixture.coordinator.Admit(BuildAdmission(39)).accepted);
    EXPECT_EQ(fixture.coordinator.GetAuthenticationHealth().accepted, 1u);
}

TEST(SparkGateway_GuardedAuthenticator_OverBudgetAcceptanceFailsClosed)
{
    TestAuthenticator slow;
    slow.delay = std::chrono::milliseconds(50);
    GuardedGatewayAuthenticator guard(slow);
    GatewayAuthenticatorPolicy policy;
    policy.callBudget = std::chrono::milliseconds(10);
    guard.SetPolicy(policy);

    // The ingress client stops waiting at the budget, so a late acceptance must not admit.
    const AuthenticationResult result = guard.Authenticate(BuildAdmission());
    EXPECT_FALSE(result.accepted);
    EXPECT_TRUE(result.principalId.empty());
    EXPECT_EQ(result.reason, std::string("Authentication exceeded its time budget"));
    const GatewayAuthenticatorHealth health = guard.GetHealth();
    EXPECT_EQ(health.budgetOverruns, 1u);
    EXPECT_EQ(health.faults, 1u);
    EXPECT_EQ(health.accepted, 0u);
    EXPECT_TRUE(health.maxCallMicroseconds >= 50000u);
}

TEST(SparkGateway_GuardedAuthenticator_ProbeAfterCooldownClosesCircuit)
{
    TestAuthenticator backend;
    backend.throwWithCredential = true;
    GuardedGatewayAuthenticator guard(backend);
    GatewayAuthenticatorPolicy policy;
    policy.failureThreshold = 2;
    policy.cooldown = std::chrono::steady_clock::duration::zero();
    guard.SetPolicy(policy);

    EXPECT_FALSE(guard.Authenticate(BuildAdmission()).accepted);
    EXPECT_FALSE(guard.Authenticate(BuildAdmission()).accepted);
    EXPECT_TRUE(guard.GetHealth().circuitOpen);
    // The cooldown has elapsed, so the guard no longer fails fast and readiness lets the probe in.
    EXPECT_FALSE(guard.IsFailingFast());

    // Cooldown elapsed: a failing probe reaches the adapter and reopens the circuit.
    EXPECT_FALSE(guard.Authenticate(BuildAdmission()).accepted);
    EXPECT_EQ(backend.calls, 3);
    EXPECT_TRUE(guard.GetHealth().circuitOpen);

    // A successful probe closes it.
    backend.throwWithCredential = false;
    EXPECT_TRUE(guard.Authenticate(BuildAdmission()).accepted);
    EXPECT_EQ(backend.calls, 4);
    const GatewayAuthenticatorHealth health = guard.GetHealth();
    EXPECT_FALSE(health.circuitOpen);
    EXPECT_EQ(health.consecutiveFaults, 0u);
    EXPECT_EQ(health.rejectedWhileOpen, 0u);
}

TEST(SparkGateway_GuardedAuthenticator_RedactsEchoedCredential)
{
    GatewayFixture fixture;
    ASSERT_TRUE(fixture.registered);
    fixture.authenticator.accept = false;
    fixture.authenticator.echoCredential = true;
    const AdmissionRequest request = BuildAdmission();
    const RouteResult result = fixture.coordinator.Admit(request);
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.reason, std::string("denied credential <redacted>"));
    EXPECT_TRUE(result.reason.find(request.credential) == std::string::npos);
}
