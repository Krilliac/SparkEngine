/**
 * @file TestSparkServerApplication.cpp
 * @brief Focused SparkServer command-line contract tests.
 */

#include "TestFramework.h"
#include "ServerApplication.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

using namespace Spark::Server;

namespace
{
    class ScopedNetworkBindMode
    {
      public:
        explicit ScopedNetworkBindMode(const char* value)
        {
            if (const char* previous = std::getenv("SPARK_NETWORK_BIND_MODE"))
                m_previous = previous;
            if (const char* previousAddress = std::getenv("SPARK_NETWORK_BIND_ADDRESS"))
                m_previousAddress = previousAddress;
#ifdef SPARK_PLATFORM_WINDOWS
            _putenv_s("SPARK_NETWORK_BIND_ADDRESS", "");
            _putenv_s("SPARK_NETWORK_BIND_MODE", value);
#else
            unsetenv("SPARK_NETWORK_BIND_ADDRESS");
            setenv("SPARK_NETWORK_BIND_MODE", value, 1);
#endif
        }

        ~ScopedNetworkBindMode()
        {
#ifdef SPARK_PLATFORM_WINDOWS
            _putenv_s("SPARK_NETWORK_BIND_MODE", m_previous ? m_previous->c_str() : "");
            _putenv_s("SPARK_NETWORK_BIND_ADDRESS", m_previousAddress ? m_previousAddress->c_str() : "");
#else
            if (m_previous)
                setenv("SPARK_NETWORK_BIND_MODE", m_previous->c_str(), 1);
            else
                unsetenv("SPARK_NETWORK_BIND_MODE");
            if (m_previousAddress)
                setenv("SPARK_NETWORK_BIND_ADDRESS", m_previousAddress->c_str(), 1);
            else
                unsetenv("SPARK_NETWORK_BIND_ADDRESS");
#endif
        }

      private:
        std::optional<std::string> m_previous;
        std::optional<std::string> m_previousAddress;
    };
} // namespace

TEST(SparkServerOptions_RequiresDynamicGameSelection)
{
    const std::array<std::string_view, 0> arguments{};
    const ParseResult result = ParseServerOptions(arguments);
    EXPECT_FALSE(result.options.has_value());
    EXPECT_TRUE(result.error.find("dynamic game module") != std::string::npos);
}

TEST(SparkServerOptions_ParsesValidatedOverrides)
{
    const std::array arguments = {std::string_view{"--module"},
                                  std::string_view{"Game.dll"},
                                  std::string_view{"--port"},
                                  std::string_view{"28015"},
                                  std::string_view{"--max-clients"},
                                  std::string_view{"128"},
                                  std::string_view{"--tick-rate"},
                                  std::string_view{"30.5"},
                                  std::string_view{"--map"},
                                  std::string_view{"town, dungeon"},
                                  std::string_view{"--no-lan-broadcast"}};
    const ParseResult result = ParseServerOptions(arguments);
    EXPECT_TRUE(result.options.has_value());
    EXPECT_EQ(result.options->server.port, static_cast<uint16_t>(28015));
    EXPECT_EQ(result.options->server.maxClients, 128);
    EXPECT_NEAR(result.options->server.tickRate, 30.5f, 0.001f);
    EXPECT_EQ(result.options->server.mapRotation.size(), static_cast<size_t>(2));
    EXPECT_FALSE(result.options->server.enableLanBroadcast);
}

TEST(SparkServerOptions_RejectsAmbiguousModuleSelection)
{
    const std::array arguments = {std::string_view{"--module"}, std::string_view{"Game.dll"},
                                  std::string_view{"--manifest"}, std::string_view{"spark.modules.json"}};
    const ParseResult result = ParseServerOptions(arguments);
    EXPECT_FALSE(result.options.has_value());
    EXPECT_TRUE(result.error.find("either") != std::string::npos);
}

TEST(SparkServerOptions_RejectsOutOfRangePort)
{
    const std::array arguments = {std::string_view{"--module"}, std::string_view{"Game.dll"},
                                  std::string_view{"--port"}, std::string_view{"0"}};
    const ParseResult result = ParseServerOptions(arguments);
    EXPECT_FALSE(result.options.has_value());
    EXPECT_TRUE(result.error.find("65535") != std::string::npos);
}

TEST(SparkServerOptions_ParsesEditorStopSentinel)
{
    const std::array arguments = {std::string_view{"--module"}, std::string_view{"Game.dll"},
                                  std::string_view{"--stop-file"}, std::string_view{"Temp/server.stop"}};
    const ParseResult result = ParseServerOptions(arguments);
    EXPECT_TRUE(result.options.has_value());
    EXPECT_EQ(result.options->stopFile.string(), std::string("Temp/server.stop"));
}

TEST(SparkServerOptions_GatewayManagedServerForcesLoopbackScopeAtParseTime)
{
    const ScopedNetworkBindMode bindMode("all");
    const std::array arguments = {std::string_view{"--module"},           std::string_view{"Game.dll"},
                                  std::string_view{"--control-endpoint"}, std::string_view{"spark-area-control-test"},
                                  std::string_view{"--gateway-key-file"}, std::string_view{"Config/gateway.key"}};

    const ParseResult result = ParseServerOptions(arguments);
    ASSERT_TRUE(result.options.has_value());
    EXPECT_EQ(result.options->server.endpointPolicy.BindAddress(), uint32_t{0x7F000001u});
    EXPECT_EQ(static_cast<int>(result.options->server.endpointPolicy.PeerScope()),
              static_cast<int>(Spark::Net::NetworkPeerScope::LoopbackOnly));
}

TEST(SparkServerOptions_CapturedGatewayScopeCannotBeWidenedAfterParse)
{
    Spark::Net::ServerConfig captured;
    {
        const ScopedNetworkBindMode bindMode("loopback");
        const std::array arguments = {
            std::string_view{"--module"},           std::string_view{"Game.dll"},
            std::string_view{"--control-endpoint"}, std::string_view{"spark-area-control-test"},
            std::string_view{"--gateway-key-file"}, std::string_view{"Config/gateway.key"}};
        const ParseResult result = ParseServerOptions(arguments);
        ASSERT_TRUE(result.options.has_value());
        captured = result.options->server;
    }

    const ScopedNetworkBindMode widenedEnvironment("all");
    EXPECT_EQ(captured.endpointPolicy.BindAddress(), uint32_t{0x7F000001u});
    EXPECT_EQ(static_cast<int>(captured.endpointPolicy.PeerScope()),
              static_cast<int>(Spark::Net::NetworkPeerScope::LoopbackOnly));
}

TEST(SparkServerOptions_NonGatewayLegacyWildcardIsRejected)
{
    const ScopedNetworkBindMode bindMode("all");
    const std::array arguments = {std::string_view{"--module"}, std::string_view{"Game.dll"}};
    const ParseResult result = ParseServerOptions(arguments);
    EXPECT_FALSE(result.options.has_value());
    EXPECT_TRUE(result.error.find("rejected") != std::string::npos);
}

TEST(SparkServerOptions_CapturesConcretePrivateBindAddress)
{
    const std::array arguments = {std::string_view{"--module"}, std::string_view{"Game.dll"},
                                  std::string_view{"--bind-address"}, std::string_view{"192.168.42.9"}};
    const ParseResult result = ParseServerOptions(arguments);
    ASSERT_TRUE(result.options.has_value());
    EXPECT_EQ(result.options->server.endpointPolicy.BindAddress(), uint32_t{0xC0A82A09u});
    EXPECT_EQ(static_cast<int>(result.options->server.endpointPolicy.PeerScope()),
              static_cast<int>(Spark::Net::NetworkPeerScope::PrivateLan));
}

TEST(SparkServerOptions_LegacyLanOnlyConfigFailsClosed)
{
    const auto configPath = std::filesystem::temp_directory_path() / "spark-net100-legacy-server.ini";
    {
        std::ofstream config(configPath, std::ios::binary | std::ios::trunc);
        config << "[Network]\nlan_only = false\n[Modules]\nmodule = Game.dll\n";
    }

    const std::string configPathText = configPath.string();
    const std::array arguments = {std::string_view{"--config"}, std::string_view{configPathText}};
    const ParseResult result = ParseServerOptions(arguments);
    EXPECT_FALSE(result.options.has_value());
    EXPECT_TRUE(result.error.find("lan_only=false") != std::string::npos);

    std::error_code error;
    std::filesystem::remove(configPath, error);
}

TEST(SparkServerOptions_LegacyLanOnlyTrueMeansLoopback)
{
    const auto configPath = std::filesystem::temp_directory_path() / "spark-net100-local-server.ini";
    {
        std::ofstream config(configPath, std::ios::binary | std::ios::trunc);
        config << "[Network]\nlan_only = true\n[Modules]\nmodule = Game.dll\n";
    }

    const std::string configPathText = configPath.string();
    const std::array arguments = {std::string_view{"--config"}, std::string_view{configPathText}};
    const ParseResult result = ParseServerOptions(arguments);
    ASSERT_TRUE(result.options.has_value());
    EXPECT_EQ(result.options->server.endpointPolicy.BindAddress(), uint32_t{0x7F000001u});

    std::error_code error;
    std::filesystem::remove(configPath, error);
}
