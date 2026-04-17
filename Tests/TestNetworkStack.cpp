#include "TestFramework.h"

#include "Engine/Networking/NetworkIntegration.h"

TEST(NetworkStack_UDPInitializeSucceeds)
{
    Spark::Net::NetworkStack stack;

    Spark::Net::NetworkStackConfig config;
    config.transport = Spark::Net::NetworkStackConfig::TransportType::UDP;
    config.serverPort = 0; // Let OS pick an ephemeral port.
    config.enableEncryption = false;

    EXPECT_TRUE(stack.Initialize(config));
    EXPECT_TRUE(stack.IsInitialized());
    EXPECT_TRUE(stack.GetTransport() != nullptr);
    EXPECT_EQ(stack.GetTransport()->GetTransportName(), std::string("UDP"));

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
    config.enableEncryption = true;

    EXPECT_FALSE(stack.Initialize(config));
    EXPECT_FALSE(stack.IsInitialized());
    EXPECT_TRUE(stack.GetTransport() == nullptr);
}
