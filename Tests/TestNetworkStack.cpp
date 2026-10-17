#include "TestFramework.h"

#include "Engine/Networking/NetworkIntegration.h"

TEST(NetworkStack_UDPInitializeSucceeds)
{
    Spark::Net::NetworkStack stack;

    Spark::Net::NetworkStackConfig config;
    config.transport = Spark::Net::NetworkStackConfig::TransportType::UDP;
    config.serverPort = 0; // Let OS pick an ephemeral port.

    EXPECT_TRUE(stack.Initialize(config));
    EXPECT_TRUE(stack.IsInitialized());
    // Guard the chained dereference — if Initialize failed on this runner the
    // transport is null, and chaining GetTransportName() would segfault.
    auto* transport = stack.GetTransport();
    EXPECT_TRUE(transport != nullptr);
    if (!transport)
        return;
    EXPECT_EQ(transport->GetTransportName(), std::string("UDP"));

    const std::string status = stack.Console_GetStatus();
    EXPECT_TRUE(status.find("Ready:          YES") != std::string::npos);

    stack.Shutdown();
    EXPECT_FALSE(stack.IsInitialized());
}

TEST(NetworkStack_SteamInitializeFailsFastWithoutSDK)
{
    Spark::Net::NetworkStack stack;

    Spark::Net::NetworkStackConfig config;
    config.transport = Spark::Net::NetworkStackConfig::TransportType::Steam;
    config.serverPort = 27015;

    EXPECT_FALSE(stack.Initialize(config));
    EXPECT_FALSE(stack.IsInitialized());
    EXPECT_TRUE(stack.GetTransport() == nullptr);
}

TEST(NetworkStack_TokensFailClosedWhenNotInitialized)
{
    Spark::Net::NetworkStack stack;

    // Before NET-100 an uninitialized stack accepted every token.
    Spark::Net::NetworkSecurity::Token forged{};
    forged.fill(0x5A);
    EXPECT_FALSE(stack.ValidateToken(forged));

    Spark::Net::NetworkSecurity::Token token{};
    token.fill(0xFF);
    EXPECT_FALSE(stack.GenerateConnectionToken(token));
    for (uint8_t byte : token)
        EXPECT_EQ(byte, static_cast<uint8_t>(0));
}

TEST(NetworkStack_IssuedTokenValidatesOnceUntilShutdown)
{
    Spark::Net::NetworkStack stack;

    Spark::Net::NetworkStackConfig config;
    config.transport = Spark::Net::NetworkStackConfig::TransportType::UDP;
    config.serverPort = 0;
    ASSERT_TRUE(stack.Initialize(config));

    Spark::Net::NetworkSecurity::Token token{};
    ASSERT_TRUE(stack.GenerateConnectionToken(token));
    EXPECT_TRUE(stack.ValidateToken(token));
    EXPECT_FALSE(stack.ValidateToken(token));

    Spark::Net::NetworkSecurity::Token pending{};
    ASSERT_TRUE(stack.GenerateConnectionToken(pending));
    stack.Shutdown();
    // Shutdown discards every pending token.
    EXPECT_FALSE(stack.ValidateToken(pending));
}
