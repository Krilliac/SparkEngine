/**
 * @file TestOnlineServicesLocalStack.cpp
 * @brief NET-110: the engine runs locally with deterministic adapters.
 *
 * The local stack is GatewayCoordinator + WorldServer + NullOnlinePlatform, with the local,
 * deterministic stand-ins for the product-owned services (docs/specs/online-services.md
 * section 6): LocalFixtureAuthenticator for identity, entitlement and moderation (B4) and
 * LocalDeterministicPlacement for matchmaking (B9). Fleet (B8) stays SparkDaemon plus
 * GatewayCoordinator::BeginDrain, which the admission script drives.
 */

#include "TestFramework.h"
#include "GatewayApplication.h"
#include "GatewayCoordinator.h"
#include "GatewayLocalAdapters.h"
#include "GatewaySecurity.h"
#include "Engine/Networking/GatewayAuthenticator.h"
#include "ScopedLoggerBaseline.h"
#include "Engine/OnlineServices/OnlineServices.h"
#include "Utils/Logger.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace Spark::Gateway;
namespace Net = Spark::Net;

namespace
{
    // Every credential below is fixture data; none may appear in a log, reason or transcript.
    constexpr std::string_view kFixture = R"({
  "version": 1,
  "principals": [
    {"credential": "cred-alpha-4c1d", "principalId": "principal:alpha", "entitled": true, "moderation": "none"},
    {"credential": "cred-bravo-9e2f", "principalId": "principal:bravo", "entitled": true, "moderation": "none"},
    {"credential": "cred-charlie-17aa", "principalId": "principal:charlie", "entitled": true, "moderation": "none"},
    {"credential": "cred-delta-b3c8", "principalId": "principal:delta", "entitled": true, "moderation": "none"},
    {"credential": "cred-echo-5d60", "principalId": "principal:echo", "entitled": false, "moderation": "none"},
    {"credential": "cred-foxtrot-8a21", "principalId": "principal:foxtrot", "entitled": true, "moderation": "banned"},
    {"credential": "cred-golf-2b7e", "principalId": "principal:golf", "entitled": true, "moderation": "none"},
    {"credential": "cred-hotel-6f04", "principalId": "principal:hotel", "entitled": true, "moderation": "none"}
  ]
})";

    constexpr std::array<std::string_view, 9> kCredentials{"cred-alpha-4c1d", "cred-bravo-9e2f", "cred-charlie-17aa",
                                                           "cred-delta-b3c8", "cred-echo-5d60",  "cred-foxtrot-8a21",
                                                           "cred-golf-2b7e",  "cred-hotel-6f04", "cred-unknown-0000"};

    /** Every area endpoint answers; handoff phases apply. Only admission is exercised here. */
    class ReadyControlPlane final : public IAreaControlPlane
    {
      public:
        bool IsReady() const override { return true; }
        HandoffOperationResult Prepare(const HandoffCommand&) override { return HandoffOperationResult::Applied; }
        HandoffOperationResult Transfer(const HandoffCommand&) override { return HandoffOperationResult::Applied; }
        HandoffOperationResult Commit(const HandoffCommand&) override { return HandoffOperationResult::Applied; }
        HandoffOperationResult Acknowledge(const HandoffCommand&) override { return HandoffOperationResult::Applied; }
        HandoffOperationResult Abort(const HandoffCommand&) override { return HandoffOperationResult::Applied; }
    };

    std::vector<AreaEndpoint> BuildAreas(int maxClients)
    {
        AreaEndpoint harbor;
        harbor.area.areaName = "Harbor";
        harbor.area.port = 32001;
        harbor.area.interServerPort = 32101;
        harbor.area.maxClients = maxClients;
        AreaEndpoint ridge;
        ridge.area.areaName = "Ridge";
        ridge.area.port = 32002;
        ridge.area.interServerPort = 32102;
        ridge.area.maxClients = maxClients;
        return {harbor, ridge};
    }

    /** One fresh gateway stack: nothing is shared between two instances. */
    struct LocalStack
    {
        /** Placement by the coordinator's default LocalDeterministicPlacement. */
        explicit LocalStack(IGatewayAuthenticator& authenticator, int maxClients = 2)
            : coordinator(world, authenticator, control)
        {
            Start(maxClients);
        }
        LocalStack(IGatewayAuthenticator& authenticator, IAreaPlacementPolicy& placement, int maxClients)
            : coordinator(world, authenticator, control, placement)
        {
            Start(maxClients);
        }
        ~LocalStack() { world.Stop(); }

        void Start(int maxClients)
        {
            Net::WorldServerConfig config;
            config.worldName = "LocalStack";
            config.tickRate = 100.0f;
            started = world.Start(config);
            registered = started && coordinator.RegisterAreas(BuildAreas(maxClients));
        }

        Net::WorldServer world;
        ReadyControlPlane control;
        GatewayCoordinator coordinator;
        bool started = false;
        bool registered = false;
    };

    AdmissionRequest BuildAdmission(Net::ClientID client, std::string_view player, std::string_view credential)
    {
        AdmissionRequest request;
        request.clientId = client;
        request.sessionId = "session-" + std::string(player);
        request.playerName = std::string(player);
        request.credential = std::string(credential);
        return request;
    }

    std::string_view FailureName(RouteFailure failure)
    {
        switch (failure)
        {
        case RouteFailure::None:
            return "None";
        case RouteFailure::NotReady:
            return "NotReady";
        case RouteFailure::InvalidRequest:
            return "InvalidRequest";
        case RouteFailure::AuthenticationFailed:
            return "AuthenticationFailed";
        case RouteFailure::DuplicateSession:
            return "DuplicateSession";
        case RouteFailure::CapacityReached:
            return "CapacityReached";
        case RouteFailure::NoAreaAvailable:
            return "NoAreaAvailable";
        }
        return "Unknown";
    }

    /**
     * Runs the fixed admission script on a fresh stack and returns its transcript. The client side
     * is NullOnlinePlatform (boundary B1): each player logs in and joins the host's lobby, and the
     * lobby id names the gateway session. The platform login never backs the admission decision
     * (spec section 4, invariant 1): the fixture credential does.
     */
    std::string RunAdmissionScript()
    {
        LocalFixtureAuthenticator authenticator = LocalFixtureAuthenticator::FromText(kFixture);
        LocalStack stack(authenticator);
        std::ostringstream transcript;
        transcript << "stack started=" << stack.started << " registered=" << stack.registered
                   << " authenticatorReady=" << authenticator.IsReady() << '\n';

        Spark::OnlineServices::NullOnlinePlatform platform;
        (void)platform.Login("host", "");
        Spark::OnlineServices::SessionInfo lobby;
        lobby.hostName = "host";
        lobby.maxPlayers = 8;
        (void)platform.CreateSession(lobby);
        const std::string lobbyId = platform.GetCurrentSession().sessionId;

        Net::ClientID nextClient = 1;
        auto admit = [&](std::string_view player, std::string_view credential)
        {
            (void)platform.Login(std::string(player), "");
            const bool joined = platform.JoinSession(lobbyId);
            AdmissionRequest request = BuildAdmission(nextClient++, platform.GetLocalPlayer().displayName, credential);
            request.sessionId = lobbyId + "/" + request.playerName;
            const RouteResult route = stack.coordinator.Admit(request);
            transcript << "admit " << player << " lobby=" << (joined ? lobbyId : "none") << " -> ";
            if (route.accepted)
                transcript << "accepted area=" << route.session.authoritativeArea << " port=" << route.port
                           << " principal=" << route.session.principalId << '\n';
            else
                transcript << "rejected " << FailureName(route.failure) << ": " << route.reason << '\n';
        };

        admit("alpha", "cred-alpha-4c1d");
        admit("bravo", "cred-bravo-9e2f");
        admit("echo", "cred-echo-5d60");
        admit("foxtrot", "cred-foxtrot-8a21");
        admit("charlie", "cred-charlie-17aa");
        admit("mallory", "cred-unknown-0000");
        admit("delta", "cred-delta-b3c8");
        admit("golf", "cred-golf-2b7e");
        transcript << "disconnect alpha -> " << stack.coordinator.Disconnect(lobbyId + "/alpha") << '\n';
        admit("golf", "cred-golf-2b7e");
        stack.coordinator.BeginDrain();
        transcript << "drain canShutdown=" << stack.coordinator.CanShutdown() << '\n';
        admit("hotel", "cred-hotel-6f04");

        const GatewayAuthenticatorHealth health = stack.coordinator.GetAuthenticationHealth();
        transcript << "sessions=" << stack.coordinator.GetSessionCount()
                   << " players=" << stack.world.GetTotalPlayerCount() << " accepted=" << health.accepted
                   << " rejected=" << health.rejected << " faults=" << health.faults << '\n';
        return transcript.str();
    }

    std::filesystem::path TemporaryFixturePath(std::string_view name)
    {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::filesystem::temp_directory_path() /
               ("spark-local-fixture-" + std::string(name) + "-" + std::to_string(tick) + ".json");
    }

    void WriteFile(const std::filesystem::path& path, std::string_view text)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    /** Records what the coordinator offers and returns a scripted answer. */
    class ScriptedPlacement final : public IAreaPlacementPolicy
    {
      public:
        Net::AreaID Place(const PlacementRequest& request, std::span<const AreaSnapshot> areas) override
        {
            principals.emplace_back(request.principalId);
            offered.assign(areas.begin(), areas.end());
            if (throwOnPlace)
                throw std::runtime_error("matchmaker unavailable");
            return answer;
        }

        Net::AreaID answer = Net::INVALID_AREA;
        bool throwOnPlace = false;
        std::vector<std::string> principals;
        std::vector<AreaSnapshot> offered;
    };
} // namespace

TEST(OnlineServices_LocalStack_TranscriptIsByteIdenticalAcrossFreshRuns)
{
    const std::string first = RunAdmissionScript();
    const std::string second = RunAdmissionScript();
    EXPECT_EQ(first, second);

    // The fixed expected transcript pins the placement rule (fewest sessions, then lowest AreaID),
    // the fixed rejection reasons, per-area capacity and drain, not only run-to-run equality.
    const std::string expected =
        "stack started=1 registered=1 authenticatorReady=1\n"
        "admit alpha lobby=local_1 -> accepted area=1 port=32001 principal=principal:alpha\n"
        "admit bravo lobby=local_1 -> accepted area=2 port=32002 principal=principal:bravo\n"
        "admit echo lobby=local_1 -> rejected AuthenticationFailed: Principal is not entitled\n"
        "admit foxtrot lobby=local_1 -> rejected AuthenticationFailed: Principal is banned\n"
        "admit charlie lobby=local_1 -> accepted area=1 port=32001 principal=principal:charlie\n"
        "admit mallory lobby=local_1 -> rejected AuthenticationFailed: Unknown credential\n"
        "admit delta lobby=local_1 -> accepted area=2 port=32002 principal=principal:delta\n"
        "admit golf lobby=local_1 -> rejected NoAreaAvailable: No healthy area with free "
        "capacity is available\n"
        "disconnect alpha -> 1\n"
        "admit golf lobby=local_1 -> accepted area=1 port=32001 principal=principal:golf\n"
        "drain canShutdown=1\n"
        "admit hotel lobby=local_1 -> rejected NotReady: Gateway admission or area control "
        "plane is not ready\n"
        "sessions=4 players=4 accepted=6 rejected=3 faults=0\n";
    EXPECT_EQ(first, expected);
    for (const std::string_view credential : kCredentials)
        EXPECT_TRUE(first.find(credential) == std::string::npos);
}

TEST(OnlineServices_LocalStack_BannedAndUnentitledRejectedWithoutLoggingCredential)
{
    ScopedLoggerBaseline loggerBaseline;
    struct CapturedLog
    {
        std::mutex mutex;
        std::vector<std::string> messages;
    };
    const auto captured = std::make_shared<CapturedLog>();
    Spark::Logger::Get().AddSink(std::make_unique<Spark::CallbackSink>(
        [captured](const Spark::LogMessage& message)
        {
            std::lock_guard lock(captured->mutex);
            captured->messages.push_back(message.message);
        }));

    LocalFixtureAuthenticator authenticator = LocalFixtureAuthenticator::FromText(kFixture);
    ASSERT_TRUE(authenticator.IsReady());
    LocalStack stack(authenticator);
    ASSERT_TRUE(stack.registered);

    const RouteResult banned = stack.coordinator.Admit(BuildAdmission(1, "foxtrot", "cred-foxtrot-8a21"));
    EXPECT_FALSE(banned.accepted);
    EXPECT_TRUE(banned.failure == RouteFailure::AuthenticationFailed);
    EXPECT_EQ(banned.reason, std::string("Principal is banned"));

    const RouteResult unentitled = stack.coordinator.Admit(BuildAdmission(2, "echo", "cred-echo-5d60"));
    EXPECT_FALSE(unentitled.accepted);
    EXPECT_TRUE(unentitled.failure == RouteFailure::AuthenticationFailed);
    EXPECT_EQ(unentitled.reason, std::string("Principal is not entitled"));

    // A rejection is a healthy answer: it creates no session and does not count as a fault.
    EXPECT_EQ(stack.coordinator.GetSessionCount(), static_cast<size_t>(0));
    EXPECT_EQ(stack.world.GetTotalPlayerCount(), 0u);
    const GatewayAuthenticatorHealth health = stack.coordinator.GetAuthenticationHealth();
    EXPECT_EQ(health.rejected, 2u);
    EXPECT_EQ(health.faults, 0u);

    // An accepted admission logs the player name, never the credential that admitted it.
    const RouteResult admitted = stack.coordinator.Admit(BuildAdmission(3, "alpha", "cred-alpha-4c1d"));
    EXPECT_TRUE(admitted.accepted);
    EXPECT_EQ(admitted.session.principalId, std::string("principal:alpha"));

    Spark::Logger::Get().FlushAll();
    std::lock_guard lock(captured->mutex);
    bool loggedAdmission = false;
    for (const std::string& message : captured->messages)
    {
        loggedAdmission |= message.find("Player 'alpha' connected") != std::string::npos;
        for (const std::string_view credential : kCredentials)
            EXPECT_TRUE(message.find(credential) == std::string::npos);
    }
    // The capture really saw the admission path, so the absence check above is not vacuous.
    EXPECT_TRUE(loggedAdmission);
}

TEST(OnlineServices_LocalStack_PlacementTieResolvesToLowestAreaId)
{
    LocalDeterministicPlacement placement;
    const PlacementRequest request{"principal:any", "session-any", {0.0f, 0.0f, 0.0f}};
    const AreaSnapshot one{1, true, 0, 4};
    const AreaSnapshot two{2, true, 0, 4};
    const AreaSnapshot three{3, true, 0, 4};

    // A tie resolves to the lowest AreaID whatever order the areas are offered in.
    const std::vector<std::vector<AreaSnapshot>> orders{
        {one, two, three}, {three, two, one}, {two, three, one}, {three, one, two}};
    for (const auto& order : orders)
        EXPECT_EQ(placement.Place(request, order), 1u);

    // Fewest sessions wins before the AreaID tie-break; offline and full areas are skipped.
    EXPECT_EQ(placement.Place(request, std::vector<AreaSnapshot>{{1, true, 3, 4}, {2, true, 1, 4}, {3, true, 1, 4}}),
              2u);
    EXPECT_EQ(placement.Place(request, std::vector<AreaSnapshot>{{1, false, 0, 4}, {2, true, 2, 4}}), 2u);
    EXPECT_EQ(placement.Place(request, std::vector<AreaSnapshot>{{1, true, 4, 4}, {2, true, 3, 4}}), 2u);
    EXPECT_EQ(placement.Place(request, std::vector<AreaSnapshot>{{1, true, 4, 4}, {2, false, 0, 4}}),
              Net::INVALID_AREA);
    EXPECT_EQ(placement.Place(request, std::span<const AreaSnapshot>{}), Net::INVALID_AREA);

    // The coordinator offers the policy every registered area with its live session load, and
    // passes the authenticated principal, never the credential.
    LocalFixtureAuthenticator authenticator = LocalFixtureAuthenticator::FromText(kFixture);
    ScriptedPlacement scripted;
    scripted.answer = 2;
    LocalStack stack(authenticator, scripted, 4);
    ASSERT_TRUE(stack.registered);
    EXPECT_TRUE(stack.coordinator.Admit(BuildAdmission(1, "alpha", "cred-alpha-4c1d")).accepted);
    EXPECT_TRUE(stack.coordinator.Admit(BuildAdmission(2, "bravo", "cred-bravo-9e2f")).accepted);
    ASSERT_EQ(scripted.offered.size(), static_cast<size_t>(2));
    EXPECT_EQ(scripted.offered[0].areaId, 1u);
    EXPECT_EQ(scripted.offered[0].sessions, 0u);
    EXPECT_EQ(scripted.offered[1].areaId, 2u);
    EXPECT_EQ(scripted.offered[1].sessions, 1u);
    EXPECT_EQ(scripted.offered[1].capacity, 4u);
    EXPECT_TRUE(scripted.offered[1].online);
    ASSERT_EQ(scripted.principals.size(), static_cast<size_t>(2));
    EXPECT_EQ(scripted.principals[1], std::string("principal:bravo"));
}

TEST(OnlineServices_LocalStack_CoordinatorRejectsUnsafePlacement)
{
    LocalFixtureAuthenticator authenticator = LocalFixtureAuthenticator::FromText(kFixture);
    ScriptedPlacement scripted;
    LocalStack stack(authenticator, scripted, 1);
    ASSERT_TRUE(stack.registered);

    // An area that was never registered, and no area at all, are both rejected.
    scripted.answer = 99;
    RouteResult route = stack.coordinator.Admit(BuildAdmission(1, "alpha", "cred-alpha-4c1d"));
    EXPECT_FALSE(route.accepted);
    EXPECT_TRUE(route.failure == RouteFailure::NoAreaAvailable);
    scripted.answer = Net::INVALID_AREA;
    route = stack.coordinator.Admit(BuildAdmission(1, "alpha", "cred-alpha-4c1d"));
    EXPECT_TRUE(route.failure == RouteFailure::NoAreaAvailable);

    // A policy that throws is contained as a rejection with a fixed reason.
    scripted.throwOnPlace = true;
    route = stack.coordinator.Admit(BuildAdmission(1, "alpha", "cred-alpha-4c1d"));
    EXPECT_TRUE(route.failure == RouteFailure::NoAreaAvailable);
    EXPECT_EQ(route.reason, std::string("Area placement policy fault"));
    scripted.throwOnPlace = false;

    // A full area is refused even when the policy insists on it (capacity 1 per area here).
    scripted.answer = 1;
    EXPECT_TRUE(stack.coordinator.Admit(BuildAdmission(1, "alpha", "cred-alpha-4c1d")).accepted);
    route = stack.coordinator.Admit(BuildAdmission(2, "bravo", "cred-bravo-9e2f"));
    EXPECT_FALSE(route.accepted);
    EXPECT_TRUE(route.failure == RouteFailure::NoAreaAvailable);

    // Only the one valid placement reached the WorldServer routing mirror.
    EXPECT_EQ(stack.coordinator.GetSessionCount(), static_cast<size_t>(1));
    EXPECT_EQ(stack.world.GetTotalPlayerCount(), 1u);
}

TEST(OnlineServices_LocalStack_OversizedOrMalformedFixtureFailsClosed)
{
    const std::string entry =
        R"({"credential": "cred-secret-91ab", "principalId": "principal:one", "entitled": true, "moderation": "none"})";
    const std::vector<std::string> rejected{
        "",
        "{\"version\": 1, \"principals\": [" + entry, // truncated
        "{\"version\": 2, \"principals\": [" + entry + "]}",
        "{\"version\": 1, \"principals\": [" + entry + "], \"extra\": true}",
        "{\"version\": 1, \"version\": 1, \"principals\": []}",
        "{\"version\": 1, \"principals\": [" + entry + ", " + entry + "]}", // repeated credential
        R"({"version": 1, "principals": [{"credential": "cred-secret-91ab", "principalId": "p", "entitled": "yes", "moderation": "none"}]})",
        R"({"version": 1, "principals": [{"credential": "cred-secret-91ab", "principalId": "p", "entitled": true, "moderation": "muted"}]})",
        R"({"version": 1, "principals": [{"credential": "", "principalId": "p", "entitled": true, "moderation": "none"}]})",
        R"({"version": 1, "principals": [{"credential": "cred secret", "principalId": "p", "entitled": true, "moderation": "none"}]})",
        R"({"version": 1, "principals": [{"credential": "cred-secret-91ab", "principalId": "p", "entitled": true}]})",
        R"({"version": 1, "principals": [{"credential": "cred-secret-91ab", "principalId": "p", "entitled": true, "moderation": "none"},
                                         {"credential": "cred-other-77cd", "principalId": "p", "entitled": true, "moderation": "none"}]})",
        R"({"version": 1, "principals": [)" + std::string(LocalFixtureMaximumBytes, ' ') + "]}", // oversized
        "[]"};
    for (const std::string& text : rejected)
    {
        LocalFixtureAuthenticator authenticator = LocalFixtureAuthenticator::FromText(text);
        EXPECT_FALSE(authenticator.IsReady());
        EXPECT_FALSE(authenticator.Error().empty());
        EXPECT_TRUE(authenticator.Error().find("cred-secret-91ab") == std::string::npos);
        EXPECT_EQ(authenticator.GetPrincipalCount(), static_cast<size_t>(0));
        const AuthenticationResult result = authenticator.Authenticate(BuildAdmission(1, "one", "cred-secret-91ab"));
        EXPECT_FALSE(result.accepted);
    }

    // The control case parses, so each rejection above is caused by its one defect.
    const LocalFixtureAuthenticator valid =
        LocalFixtureAuthenticator::FromText("{\"version\": 1, \"principals\": [" + entry + "]}");
    EXPECT_TRUE(valid.IsReady());
    EXPECT_EQ(valid.GetPrincipalCount(), static_cast<size_t>(1));

    // A gateway given a rejected fixture is not ready and admits nobody.
    LocalFixtureAuthenticator broken = LocalFixtureAuthenticator::FromText(rejected[1]);
    LocalStack stack(broken);
    ASSERT_TRUE(stack.registered);
    EXPECT_FALSE(stack.coordinator.IsReady());
    const RouteResult route = stack.coordinator.Admit(BuildAdmission(1, "one", "cred-secret-91ab"));
    EXPECT_FALSE(route.accepted);
    EXPECT_TRUE(route.failure == RouteFailure::NotReady);

    // The file loader applies the same budget before reading, and a missing file fails closed.
    const std::filesystem::path oversized = TemporaryFixturePath("oversized");
    WriteFile(oversized, rejected[rejected.size() - 2]);
    EXPECT_FALSE(LocalFixtureAuthenticator(oversized).IsReady());
    const std::filesystem::path missing = TemporaryFixturePath("missing");
    EXPECT_FALSE(LocalFixtureAuthenticator(missing).IsReady());
    std::error_code error;
    std::filesystem::remove(oversized, error);
}

TEST(OnlineServices_LocalStack_GatewayOptionsSelectFixtureAuthenticator)
{
    const std::filesystem::path fixture = TemporaryFixturePath("gateway");
    WriteFile(fixture, kFixture);
    const std::filesystem::path config = TemporaryFixturePath("gateway-config").replace_extension(".ini");
    WriteFile(config, "[Gateway]\nport = 27020\n\n[Area.Harbor]\nhost = 127.0.0.1\nport = 32001\n"
                      "inter_server_port = 32101\n\n[Security]\nkey_file = unused.key\nadmission_fixture = " +
                          fixture.generic_string() + "\n");

    // [Security] admission_fixture selects the fixture authenticator through the process path.
    const std::string configText = config.string();
    const std::vector<std::string_view> fromConfig{"--config", configText};
    const GatewayParseResult parsed = ParseGatewayOptions(fromConfig);
    ASSERT_TRUE(parsed.options.has_value());
    EXPECT_EQ(parsed.options->admissionFixture, std::filesystem::path(fixture.generic_string()));
    const std::unique_ptr<IGatewayAuthenticator> selected = CreateGatewayAuthenticator(*parsed.options);
    const auto* local = dynamic_cast<const LocalFixtureAuthenticator*>(selected.get());
    ASSERT_TRUE(local != nullptr);
    EXPECT_TRUE(local->IsReady());
    EXPECT_EQ(local->GetPrincipalCount(), static_cast<size_t>(8));

    // --admission-fixture overrides the config; without either, admission stays on the key file.
    const std::vector<std::string_view> fromFlag{"--config", configText, "--admission-fixture", "other.json"};
    const GatewayParseResult overridden = ParseGatewayOptions(fromFlag);
    ASSERT_TRUE(overridden.options.has_value());
    EXPECT_EQ(overridden.options->admissionFixture, std::filesystem::path("other.json"));
    GatewayOptions keyOnly = *parsed.options;
    keyOnly.admissionFixture.clear();
    const std::unique_ptr<IGatewayAuthenticator> keyFile = CreateGatewayAuthenticator(keyOnly);
    EXPECT_TRUE(dynamic_cast<const KeyFileAuthenticator*>(keyFile.get()) != nullptr);
    EXPECT_TRUE(std::string(GatewayHelpText()).find("--admission-fixture") != std::string::npos);

    std::error_code error;
    std::filesystem::remove(fixture, error);
    std::filesystem::remove(config, error);
}
