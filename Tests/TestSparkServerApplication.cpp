/**
 * @file TestSparkServerApplication.cpp
 * @brief Focused SparkServer command-line contract tests.
 */

#include "TestFramework.h"
#include "ServerApplication.h"

#include <array>
#include <cstdlib>
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
#ifdef SPARK_PLATFORM_WINDOWS
            _putenv_s("SPARK_NETWORK_BIND_MODE", value);
#else
            setenv("SPARK_NETWORK_BIND_MODE", value, 1);
#endif
        }

        ~ScopedNetworkBindMode()
        {
#ifdef SPARK_PLATFORM_WINDOWS
            _putenv_s("SPARK_NETWORK_BIND_MODE", m_previous ? m_previous->c_str() : "");
#else
            if (m_previous)
                setenv("SPARK_NETWORK_BIND_MODE", m_previous->c_str(), 1);
            else
                unsetenv("SPARK_NETWORK_BIND_MODE");
#endif
        }

      private:
        std::optional<std::string> m_previous;
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
    EXPECT_EQ(static_cast<int>(result.options->server.bindScope),
              static_cast<int>(Spark::Net::NetworkBindScope::LoopbackOnly));
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
    EXPECT_EQ(static_cast<int>(captured.bindScope), static_cast<int>(Spark::Net::NetworkBindScope::LoopbackOnly));
}

TEST(SparkServerOptions_NonGatewayAllInterfaceOptInIsCapturedOnce)
{
    Spark::Net::ServerConfig captured;
    {
        const ScopedNetworkBindMode bindMode("all");
        const std::array arguments = {std::string_view{"--module"}, std::string_view{"Game.dll"}};
        const ParseResult result = ParseServerOptions(arguments);
        ASSERT_TRUE(result.options.has_value());
        captured = result.options->server;
    }

    const ScopedNetworkBindMode narrowedEnvironment("loopback");
    EXPECT_EQ(static_cast<int>(captured.bindScope), static_cast<int>(Spark::Net::NetworkBindScope::AllInterfaces));
}
