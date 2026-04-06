// TestRemoteDebugSystem.cpp - Tests for Spark::RemoteDebug::RemoteDebugSystem
#include "TestFramework.h"
#include "Engine/RemoteDebug/RemoteDebugSystem.h"

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
    sys.StartServer(0);

    bool handlerCalled = false;
    sys.GetServer()->RegisterCommandHandler("test_cmd",
                                            [&](const Spark::RemoteDebug::RemoteCommand& cmd)
                                            {
                                                handlerCalled = true;
                                                return Spark::RemoteDebug::RemoteCommand{"test_response", "ok",
                                                                                         cmd.requestId, 0.0f};
                                            });

    Spark::RemoteDebug::RemoteCommand cmd{"test_cmd", "payload", 42, 0.0f};
    auto resp = sys.GetServer()->ProcessCommand(cmd);
    EXPECT_TRUE(handlerCalled);
    EXPECT_EQ(std::string("test_response"), resp.type);
    EXPECT_EQ(static_cast<uint32_t>(42), resp.requestId);

    sys.Shutdown();
}

TEST(RemoteDebugSystem_UnknownCommandReturnsError)
{
    auto& sys = Spark::RemoteDebug::RemoteDebugSystem::GetInstance();
    sys.Initialize();
    sys.StartServer(0);

    Spark::RemoteDebug::RemoteCommand cmd{"nonexistent_cmd", "", 1, 0.0f};
    auto resp = sys.GetServer()->ProcessCommand(cmd);
    EXPECT_EQ(std::string("error"), resp.type);

    sys.Shutdown();
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
    EXPECT_TRUE(serverSession != nullptr);
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
