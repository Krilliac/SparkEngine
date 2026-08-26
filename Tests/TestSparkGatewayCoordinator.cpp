/**
 * @file TestSparkGatewayCoordinator.cpp
 * @brief Deterministic fenced-handoff and gateway failure-matrix tests.
 */

#include "TestFramework.h"
#include "GatewayCoordinator.h"

#include <deque>

using namespace Spark::Gateway;
namespace Net = Spark::Net;

namespace
{
    class TestAuthenticator final : public IGatewayAuthenticator
    {
      public:
        AuthenticationResult Authenticate(const AdmissionRequest& request) override
        {
            if (!accept)
                return {false, {}, "denied"};
            return {true, "principal:" + request.playerName, {}};
        }
        bool IsReady() const override { return ready; }

        bool ready = true;
        bool accept = true;
    };

    class ScriptedControlPlane final : public IAreaControlPlane
    {
      public:
        bool IsReady() const override { return ready; }
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
        first.host = "10.0.0.1";
        AreaEndpoint second;
        second.area.areaName = "Forest";
        second.area.port = 31002;
        second.area.interServerPort = 31102;
        second.host = "10.0.0.2";
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
