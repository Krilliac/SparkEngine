/**
 * @file TestSEC100RemoteAdminUnavailableReal.cpp
 * @brief SEC-100 / OD-05: remote administration is permanently unavailable in stable-v1.
 *
 * The owner decision is that no authenticated remote-administration channel is
 * built for stable-v1. The static_asserts below are a compile-time contract on
 * the production headers: they fail the build if any of these named entry
 * points or fields returns:
 *   - RemoteDebugSystem::StartServer() / StartServer(port)
 *   - RemoteDebugSystem::ConnectToTarget(address[, port])
 *   - RemoteDebugClient::Connect(address[, port])
 *   - RemoteDebugServer::StartListening(port)
 *   - RemoteSession::GetPort / SetPort / GetAddress
 *   - ServerConfig::rconPassword / rconPort / enableRcon /
 *     enableRemoteAdministration
 * The field check is by name; it does not detect a differently named switch.
 * The runtime tests prove that the only remaining entry into RemoteDebug
 * dispatch, a raw principal-less queue call, is denied and audited, and that
 * the single local grant cannot reach an administrative command.
 *
 * The stable-v1 Windows Shipping product also builds with ENABLE_NETWORKING=OFF,
 * where DedicatedServer is compiled out entirely; that configuration is covered
 * by the local compile contract Tests/Fixtures/NetworkingDisabledCompileContract.cpp,
 * which no hosted CI lane builds yet.
 */

#include "TestFramework.h"

#include "Engine/Networking/DedicatedServer.h"
#include "Engine/RemoteDebug/RemoteDebugSystem.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// SparkServer's option parser exists wherever the server core is built
// (ENABLE_NETWORKING); the networking-off product has no server to configure.
#if __has_include("ServerApplication.h")
#include "ServerApplication.h"
#define SPARK_TEST_HAS_SERVER_OPTIONS 1
#endif

namespace
{
    using namespace Spark::RemoteDebug;

    // Each concept is satisfied only when an external caller could invoke the
    // entry point. A missing or inaccessible member leaves it unsatisfied.
    template <typename System>
    concept CanStartRemoteServer = requires(System& system, uint16_t port) { system.StartServer(port); } ||
                                   requires(System& system) { system.StartServer(); };

    template <typename System>
    concept CanConnectToRemoteTarget = requires(System& system, const std::string& address, uint16_t port) {
        system.ConnectToTarget(address, port);
    } || requires(System& system, const std::string& address) { system.ConnectToTarget(address); };

    template <typename Client>
    concept CanConnectClientRemotely = requires(Client& client, const std::string& address, uint16_t port) {
        client.Connect(address, port);
    } || requires(Client& client, const std::string& address) { client.Connect(address); };

    template <typename Server>
    concept CanListenOnPort = requires(Server& server, uint16_t port) { server.StartListening(port); };

    template <typename Session>
    concept SessionCarriesRemoteEndpoint = requires(Session& session) { session.GetPort(); } ||
                                           requires(Session& session, uint16_t port) { session.SetPort(port); } ||
                                           requires(Session& session) { session.GetAddress(); };

    static_assert(!CanStartRemoteServer<RemoteDebugSystem>, "OD-05: RemoteDebug must not expose a remote server");
    static_assert(!CanConnectToRemoteTarget<RemoteDebugSystem>,
                  "OD-05: RemoteDebug must not connect to remote targets");
    static_assert(!CanConnectClientRemotely<RemoteDebugClient>, "OD-05: RemoteDebugClient must not connect remotely");
    static_assert(!CanListenOnPort<RemoteDebugServer>, "OD-05: RemoteDebugServer must not accept a listen port");
    static_assert(!SessionCarriesRemoteEndpoint<RemoteSession>, "OD-05: RemoteSession must not carry an endpoint");

#ifdef ENABLE_NETWORKING
    template <typename Config>
    concept ConfigCanEnableRemoteAdministration =
        requires(Config& config) { config.rconPassword; } || requires(Config& config) { config.rconPort; } ||
        requires(Config& config) { config.enableRcon; } ||
        requires(Config& config) { config.enableRemoteAdministration; };

    static_assert(!ConfigCanEnableRemoteAdministration<Spark::Net::ServerConfig>,
                  "OD-05: ServerConfig must not carry a remote-administration switch");
#endif

    [[nodiscard]] bool LastAuditIs(const RemoteDebugServer& server, RemoteDebugAuditDecision decision)
    {
        const auto events = server.GetAuditEvents();
        return !events.empty() && events.back().decision == decision;
    }

    [[nodiscard]] bool IsAccessDenied(const RemoteCommand& response)
    {
        return response.type == "error" && response.payload.find("access_denied") != std::string::npos;
    }
} // namespace

TEST(RemoteAdmin_UnavailableRawTransportDeniedAndAudited)
{
    auto& system = RemoteDebugSystem::GetInstance();
    system.Shutdown();
    ASSERT_TRUE(system.Initialize());
    RemoteDebugServer* server = system.GetServer();
    ASSERT_TRUE(server != nullptr);
    ASSERT_TRUE(server->StartListening());
    EXPECT_STR_CONTAINS(system.Console_GetStatus(), "remote transport unavailable");

    // A raw queue call is all an out-of-process adapter could ever reach. It
    // carries no principal, so the built-in administrative command is denied
    // and exactly that decision is audited.
    const size_t auditBefore = server->GetAuditEvents().size();
    server->GetSession().EnqueueReceived({"console_cmd", R"({"command":"quit"})", 41, 0.0f});
    system.Update(0.016f);

    RemoteCommand response;
    ASSERT_TRUE(server->GetSession().DequeuePendingSend(response));
    EXPECT_TRUE(IsAccessDenied(response));
    EXPECT_EQ(response.requestId, static_cast<uint32_t>(41));
    EXPECT_EQ(server->GetAuditEvents().size(), auditBefore + 1);
    EXPECT_TRUE(LastAuditIs(*server, RemoteDebugAuditDecision::AnonymousDenied));
    EXPECT_FALSE(system.IsConnected());
    system.Shutdown();
}

TEST(RemoteAdmin_UnavailableLocalGrantCannotAdminister)
{
    auto& system = RemoteDebugSystem::GetInstance();
    system.Shutdown();
    ASSERT_TRUE(system.Initialize());
    system.EnableLoopback();

    // The only grant any public API can mint is the bounded local Observer.
    // It must not reach console execution or property mutation.
    RemoteDebugClient* client = system.GetClient();
    ASSERT_TRUE(client != nullptr);
    const uint32_t consoleRequest = client->ExecuteConsoleCommand("quit");
    system.Update(0.016f);
    std::vector<RemoteCommand> responses = client->PollResponses();
    ASSERT_EQ(responses.size(), static_cast<size_t>(1));
    EXPECT_EQ(responses.front().requestId, consoleRequest);
    EXPECT_TRUE(IsAccessDenied(responses.front()));
    EXPECT_TRUE(LastAuditIs(*system.GetServer(), RemoteDebugAuditDecision::AuthorizationDenied));

    const uint32_t propertyRequest = client->SetProperty("player.health", "1");
    system.Update(0.016f);
    responses = client->PollResponses();
    ASSERT_EQ(responses.size(), static_cast<size_t>(1));
    EXPECT_EQ(responses.front().requestId, propertyRequest);
    EXPECT_TRUE(IsAccessDenied(responses.front()));
    EXPECT_TRUE(LastAuditIs(*system.GetServer(), RemoteDebugAuditDecision::AuthorizationDenied));
    system.Shutdown();
}

#ifdef SPARK_TEST_HAS_SERVER_OPTIONS

namespace
{
    Spark::Server::ParseResult ParseServerWith(std::string_view flag, std::string_view value)
    {
        std::vector<std::string_view> arguments{"--module", "Game.dll", flag};
        if (!value.empty())
            arguments.push_back(value);
        return Spark::Server::ParseServerOptions(arguments);
    }

    /** @brief One uniquely named server INI in the temp directory, removed on scope exit. */
    class ScopedServerConfig
    {
      public:
        explicit ScopedServerConfig(const std::string& contents)
        {
            const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
            m_path = std::filesystem::temp_directory_path() / ("spark_sec100_server_" + std::to_string(nonce) + ".ini");
            std::ofstream output(m_path, std::ios::binary);
            output << contents;
        }

        ~ScopedServerConfig()
        {
            std::error_code ignored;
            std::filesystem::remove(m_path, ignored);
        }

        ScopedServerConfig(const ScopedServerConfig&) = delete;
        ScopedServerConfig& operator=(const ScopedServerConfig&) = delete;

        Spark::Server::ParseResult Parse() const
        {
            const std::string path = m_path.string();
            const std::array<std::string_view, 4> arguments{"--config", path, "--module", "Game.dll"};
            return Spark::Server::ParseServerOptions(arguments);
        }

      private:
        std::filesystem::path m_path;
    };
} // namespace

TEST(RemoteAdmin_UnavailableServerCliSwitchesRejected)
{
    struct RemoteAdminSwitch
    {
        std::string_view flag;
        std::string_view value;
    };
    constexpr std::array<RemoteAdminSwitch, 8> switches{{{"--rcon", {}},
                                                         {"--rcon-password", "x"},
                                                         {"--rcon-port", "27015"},
                                                         {"--remote-admin", {}},
                                                         {"--enable-remote-administration", {}},
                                                         {"--admin-port", "1"},
                                                         {"--admin-password", "x"},
                                                         {"--remote-debug", "9999"}}};

    // The same command line without the switch is accepted, so each rejection
    // below is the switch itself and not an otherwise invalid invocation.
    const std::array<std::string_view, 2> baseline{"--module", "Game.dll"};
    ASSERT_TRUE(Spark::Server::ParseServerOptions(baseline).options.has_value());

    for (const RemoteAdminSwitch& candidate : switches)
    {
        const Spark::Server::ParseResult result = ParseServerWith(candidate.flag, candidate.value);
        EXPECT_FALSE(result.options.has_value());
        EXPECT_STR_CONTAINS(result.error, std::string(candidate.flag));
    }
}

TEST(RemoteAdmin_UnavailableServerConfigKeysRejected)
{
    const std::string validConfig = "[Server]\nname=Arena\n[Network]\nport=27015\n";
    const ScopedServerConfig accepted(validConfig);
    ASSERT_TRUE(accepted.Parse().options.has_value());

    constexpr std::array<std::string_view, 8> reservedKeys{"rcon",           "rcon_password",    "rcon_port",
                                                           "enable_rcon",    "remote_admin",     "remote_admin_port",
                                                           "admin_password", "remote_debug_port"};
    constexpr std::array<std::string_view, 2> hostSections{"Server", "Network"};
    for (const std::string_view key : reservedKeys)
    {
        for (const std::string_view section : hostSections)
        {
            const ScopedServerConfig config(validConfig + "[" + std::string(section) + "]\n" + std::string(key) +
                                            "=1\n");
            const Spark::Server::ParseResult result = config.Parse();
            EXPECT_FALSE(result.options.has_value());
            EXPECT_STR_CONTAINS(result.error, "OD-05");
            EXPECT_STR_CONTAINS(result.error, std::string(section) + "." + std::string(key));
        }
    }

    // Case does not smuggle a key past the check, and a dedicated section is
    // rejected even when it holds no reserved key.
    const ScopedServerConfig mixedCase(validConfig + "[Server]\nRCON_Password=x\n");
    EXPECT_STR_CONTAINS(mixedCase.Parse().error, "OD-05");
    constexpr std::array<std::string_view, 3> reservedSections{"RCON", "RemoteAdmin", "admin"};
    for (const std::string_view section : reservedSections)
    {
        const ScopedServerConfig config(validConfig + "[" + std::string(section) + "]\nenabled=true\n");
        const Spark::Server::ParseResult result = config.Parse();
        EXPECT_FALSE(result.options.has_value());
        EXPECT_STR_CONTAINS(result.error, "OD-05");
    }
}

#endif // SPARK_TEST_HAS_SERVER_OPTIONS
