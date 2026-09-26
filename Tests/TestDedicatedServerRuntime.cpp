#include "TestFramework.h"

#include "Engine/Networking/DedicatedServer.h"

#ifdef ENABLE_NETWORKING

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Spark::Net;

static_assert(
    std::is_same_v<decltype(std::declval<const DedicatedServer&>().GetLanBroadcastSnapshot()), LanBroadcastSnapshot>);
static_assert(std::is_same_v<decltype(std::declval<const DedicatedServer&>().GetStats()), ServerStats>);

namespace
{
    class MockNetworkRuntime final : public INetworkRuntime
    {
      public:
        bool Initialize() override
        {
            initializeCalled = true;
            return initializeResult;
        }

        bool StartServer(uint16_t port, int maxClients, const NetworkEndpointPolicy& endpointPolicy,
                         bool allowLanAdvertisement) override
        {
            startServerCalled = true;
            startedPort = port;
            startedMaxClients = maxClients;
            startedEndpointPolicy = endpointPolicy;
            startedAllowLanAdvertisement = allowLanAdvertisement;
            return startServerResult;
        }

        void StopServer() override { stopServerCalled = true; }

        void Shutdown() override { shutdownCalled = true; }

        void Update(float deltaTime) override
        {
            updateCalled = true;
            lastUpdateDelta = deltaTime;
        }

        void SendToClient(ClientID client, const NetworkMessage& msg) override
        {
            sendToClientCalls.emplace_back(client, msg);
        }

        void SendToAll(const NetworkMessage& msg) override { sendToAllCalls.push_back(msg); }

        void SendToAllExcept(ClientID excludeClient, const NetworkMessage& msg) override
        {
            sendToAllExceptCalls.emplace_back(excludeClient, msg);
        }

        void RegisterHandler(MessageType type, MessageHandler handler) override
        {
            handlers[static_cast<uint16_t>(type)] = std::move(handler);
        }

        void ClearHandlers() override
        {
            clearHandlersCalled = true;
            handlers.clear();
        }

        std::unordered_map<ClientID, ClientInfo> GetClients() const override { return clients; }

        NetworkStats GetStats() const override { return stats; }

        void KickClient(ClientID client, const std::string& reason) override
        {
            kickedClients.emplace_back(client, reason);
            clients.erase(client);
        }

        void Dispatch(const NetworkMessage& msg)
        {
            auto it = handlers.find(static_cast<uint16_t>(msg.type));
            if (it != handlers.end())
                it->second(msg);
        }

        bool initializeResult = true;
        bool startServerResult = true;

        bool initializeCalled = false;
        bool startServerCalled = false;
        bool stopServerCalled = false;
        bool shutdownCalled = false;
        bool updateCalled = false;
        bool clearHandlersCalled = false;
        float lastUpdateDelta = 0.0f;
        uint16_t startedPort = 0;
        int startedMaxClients = 0;
        NetworkEndpointPolicy startedEndpointPolicy{};
        bool startedAllowLanAdvertisement = false;

        NetworkStats stats{};
        std::unordered_map<ClientID, ClientInfo> clients;
        std::unordered_map<uint16_t, MessageHandler> handlers;
        std::vector<std::pair<ClientID, NetworkMessage>> sendToClientCalls;
        std::vector<NetworkMessage> sendToAllCalls;
        std::vector<std::pair<ClientID, NetworkMessage>> sendToAllExceptCalls;
        std::vector<std::pair<ClientID, std::string>> kickedClients;
    };

    static NetworkMessage BuildStringMessage(MessageType type, ClientID sender, const std::string& text)
    {
        NetworkMessage msg;
        msg.type = type;
        msg.senderID = sender;
        msg.channel = ChannelType::Reliable;
        NetBuffer buf;
        buf.WriteString(text);
        msg.payload = buf.GetData();
        return msg;
    }
} // namespace

TEST(DedicatedServerRuntime_ConnectDisconnectCallbacks)
{
    MockNetworkRuntime runtime;
    DedicatedServer server(runtime);

    ServerConfig config;
    config.enableLanBroadcast = false;
    config.mapRotation = {"arena"};
    config.maxClients = 8;
    config.endpointPolicy = NetworkEndpointPolicy::Loopback();

    ClientInfo client;
    client.id = 7;
    client.name = "Alice";
    runtime.clients[7] = client;

    bool connectedCalled = false;
    bool disconnectedCalled = false;
    std::string connectedName;
    ClientID disconnectedClient = INVALID_CLIENT;
    ServerCallbacks callbacks;
    callbacks.onClientConnected = [&](ClientID id, const std::string& name)
    {
        connectedCalled = true;
        EXPECT_EQ(id, static_cast<ClientID>(7));
        connectedName = name;
    };
    callbacks.onClientDisconnected = [&](ClientID id, const std::string&)
    {
        disconnectedCalled = true;
        disconnectedClient = id;
    };
    server.SetCallbacks(callbacks);

    EXPECT_TRUE(server.InitializeOnly(config));
    EXPECT_TRUE(runtime.initializeCalled);
    EXPECT_TRUE(runtime.startServerCalled);
    EXPECT_EQ(runtime.startedPort, config.port);
    EXPECT_EQ(runtime.startedMaxClients, config.maxClients);
    EXPECT_EQ(runtime.startedEndpointPolicy.BindAddress(), config.endpointPolicy.BindAddress());
    EXPECT_EQ(static_cast<int>(runtime.startedEndpointPolicy.PeerScope()),
              static_cast<int>(config.endpointPolicy.PeerScope()));
    EXPECT_FALSE(runtime.startedAllowLanAdvertisement);

    NetworkMessage connect;
    connect.type = MessageType::Connect;
    connect.senderID = 7;
    runtime.Dispatch(connect);
    EXPECT_TRUE(connectedCalled);
    EXPECT_EQ(connectedName, std::string("Alice"));
    EXPECT_EQ(server.GetStats().totalConnectionsServed, static_cast<uint32_t>(1));
    EXPECT_EQ(server.GetStats().currentPlayers, static_cast<uint32_t>(1));

    NetworkMessage disconnect;
    disconnect.type = MessageType::Disconnect;
    disconnect.senderID = 7;
    runtime.Dispatch(disconnect);
    EXPECT_TRUE(disconnectedCalled);
    EXPECT_EQ(disconnectedClient, static_cast<ClientID>(7));

    server.Stop();
    EXPECT_TRUE(runtime.clearHandlersCalled);
}

TEST(DedicatedServerRuntime_ThreadsAuthoritativeLanAdvertisementIntoNetworkRuntime)
{
    MockNetworkRuntime runtime;
    DedicatedServer server(runtime, []() { return INVALID_SOCKET; });

    ServerConfig config;
    config.enableLanBroadcast = true;
    config.mapRotation = {"arena"};
    config.endpointPolicy = NetworkEndpointPolicy::Loopback();

    ASSERT_TRUE(server.InitializeOnly(config));
    EXPECT_TRUE(runtime.startedAllowLanAdvertisement);
    EXPECT_EQ(runtime.startedEndpointPolicy.BindAddress(), config.endpointPolicy.BindAddress());
    server.Stop();
}

TEST(DedicatedServerRuntime_InvalidEndpointPolicyFailsBeforeRuntimeInitialization)
{
    MockNetworkRuntime runtime;
    DedicatedServer server(runtime);

    ServerConfig config;
    config.endpointPolicy = ResolveNetworkEndpointPolicy("0.0.0.0");
    ASSERT_FALSE(config.endpointPolicy.IsValid());

    EXPECT_FALSE(server.InitializeOnly(config));
    EXPECT_FALSE(runtime.initializeCalled);
    EXPECT_FALSE(runtime.startServerCalled);
}

TEST(DedicatedServerRuntime_ChatCannotInvokeRcon)
{
    MockNetworkRuntime runtime;
    DedicatedServer server(runtime);

    ServerConfig config;
    config.enableLanBroadcast = false;
    config.mapRotation = {"arena"};

    std::string seenChat;
    ServerCallbacks callbacks;
    callbacks.onChatMessage = [&](const std::string& chat) { seenChat = chat; };
    server.SetCallbacks(callbacks);

    EXPECT_TRUE(server.InitializeOnly(config));

    const NetworkMessage chat = BuildStringMessage(MessageType::ChatMessage, 12, "hello squad");
    runtime.Dispatch(chat);
    EXPECT_EQ(runtime.sendToAllExceptCalls.size(), static_cast<size_t>(1));
    EXPECT_EQ(runtime.sendToAllExceptCalls[0].first, static_cast<ClientID>(12));
    EXPECT_EQ(seenChat, std::string("hello squad"));

    const NetworkMessage slashChat = BuildStringMessage(MessageType::ChatMessage, 12, "/status");
    runtime.Dispatch(slashChat);
    EXPECT_EQ(runtime.sendToAllExceptCalls.size(), static_cast<size_t>(2));
    EXPECT_TRUE(runtime.sendToClientCalls.empty());
    EXPECT_EQ(seenChat, std::string("/status"));

    const std::string statusResponse = server.ExecuteRcon("status");
    EXPECT_TRUE(statusResponse.find("Server") != std::string::npos);

    // The help handler takes the registry mutex internally. ExecuteRcon must
    // release that mutex before dispatch or this call self-deadlocks.
    const std::string helpResponse = server.ExecuteRcon("help");
    EXPECT_TRUE(helpResponse.find("Available commands") != std::string::npos);

    server.Stop();
}

TEST(DedicatedServerRuntime_RconAuditRedactsArgumentsAndResponses)
{
    const std::string secretArgument = "SEC100_FAKE_ARGUMENT_73B19";
    const std::string secretResponse = "ERROR: SEC100_FAKE_RESPONSE_84C20";
    const std::string unknownCommand = "SEC100_FAKE_UNKNOWN_95D31";
    const std::string commandLine = "audit_probe " + secretArgument;
    std::vector<std::string> auditMessages;
    std::vector<std::string> receivedArguments;
    std::string callbackCommand;
    std::string callbackResponse;
    int handlerCalls = 0;
    int callbackCalls = 0;
    ServerCallbacks callbacks;
    callbacks.onLogMessage = [&](const std::string& message) { auditMessages.push_back(message); };
    callbacks.onRconCommand = [&](const std::string& command, const std::string& response)
    {
        ++callbackCalls;
        callbackCommand = command;
        callbackResponse = response;
    };

    // Callback captures must outlive the server: a failed assertion can unwind
    // through DedicatedServer::~DedicatedServer(), which logs during Stop().
    MockNetworkRuntime runtime;
    DedicatedServer server(runtime);
    ServerConfig config;
    config.enableLogging = false;
    ASSERT_TRUE(server.InitializeOnly(config));
    server.SetCallbacks(callbacks);
    server.RegisterRconCommand("audit_probe", "Audit redaction probe",
                               [&](const std::vector<std::string>& arguments)
                               {
                                   ++handlerCalls;
                                   receivedArguments = arguments;
                                   return secretResponse;
                               });

    EXPECT_EQ(server.ExecuteRcon(commandLine), secretResponse);
    EXPECT_EQ(handlerCalls, 1);
    ASSERT_EQ(receivedArguments.size(), static_cast<size_t>(1));
    EXPECT_EQ(receivedArguments.front(), secretArgument);
    EXPECT_EQ(callbackCalls, 1);
    EXPECT_EQ(callbackCommand, commandLine);
    EXPECT_EQ(callbackResponse, secretResponse);

    EXPECT_EQ(server.ExecuteRcon(unknownCommand + " " + secretArgument), "Unknown command: " + unknownCommand);
    EXPECT_EQ(handlerCalls, 1);
    EXPECT_EQ(callbackCalls, 1);
    ASSERT_EQ(auditMessages.size(), static_cast<size_t>(2));
    EXPECT_STR_CONTAINS(auditMessages[0], "RCON: command=audit_probe disposition=dispatched");
    EXPECT_STR_CONTAINS(auditMessages[1], "RCON: command=<unknown> disposition=unknown_command");
    for (const auto& message : auditMessages)
    {
        EXPECT_TRUE(message.find(secretArgument) == std::string::npos);
        EXPECT_TRUE(message.find(secretResponse) == std::string::npos);
        EXPECT_TRUE(message.find(unknownCommand) == std::string::npos);
    }
    server.Stop();
}

TEST(DedicatedServerRuntime_RconHandlerExceptionIsAuditedWithoutSecrets)
{
    const std::string secretArgument = "SEC100_FAKE_THROW_ARGUMENT_41E77";
    const std::string secretExceptionText = "SEC100_FAKE_EXCEPTION_52F88";
    const auto logPath =
        std::filesystem::temp_directory_path() /
        ("spark-rcon-throw-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");
    std::error_code error;
    std::filesystem::remove(logPath, error);

    std::vector<std::string> auditMessages;
    int callbackCalls = 0;
    ServerCallbacks callbacks;
    callbacks.onLogMessage = [&](const std::string& message) { auditMessages.push_back(message); };
    callbacks.onRconCommand = [&](const std::string&, const std::string&) { ++callbackCalls; };

    MockNetworkRuntime runtime;
    DedicatedServer server(runtime);
    ServerConfig config;
    config.enableLogging = true;
    config.logFilePath = logPath.string();
    ASSERT_TRUE(server.InitializeOnly(config));
    server.SetCallbacks(callbacks);
    server.RegisterRconCommand("explode", "Handler that throws",
                               [&](const std::vector<std::string>& arguments) -> std::string
                               { throw std::runtime_error(secretExceptionText + " " + arguments.front()); });

    // The exception must not escape, and neither its text nor the arguments
    // may reach the caller.
    EXPECT_EQ(server.ExecuteRcon("explode " + secretArgument), std::string("Command failed: explode"));
    EXPECT_EQ(callbackCalls, 0);
    // The registry is still usable after a failed dispatch.
    EXPECT_TRUE(server.ExecuteRcon("status").find("Server") != std::string::npos);
    server.Stop();

    std::string logText;
    {
        std::ifstream logFile(logPath);
        ASSERT_TRUE(logFile.is_open());
        logText.assign(std::istreambuf_iterator<char>(logFile), std::istreambuf_iterator<char>());
    }
    const std::string failedRecord = "RCON: command=explode disposition=failed";
    const size_t first = logText.find(failedRecord);
    EXPECT_TRUE(first != std::string::npos);
    EXPECT_TRUE(first == std::string::npos || logText.find(failedRecord, first + 1) == std::string::npos);
    EXPECT_TRUE(logText.find("RCON: command=status disposition=dispatched") != std::string::npos);
    EXPECT_TRUE(logText.find(secretArgument) == std::string::npos);
    EXPECT_TRUE(logText.find(secretExceptionText) == std::string::npos);
    for (const auto& message : auditMessages)
    {
        EXPECT_TRUE(message.find(secretArgument) == std::string::npos);
        EXPECT_TRUE(message.find(secretExceptionText) == std::string::npos);
    }
    std::filesystem::remove(logPath, error);
}

TEST(DedicatedServerRuntime_RconRegistrySnapshotIsSafeDuringRegistration)
{
    MockNetworkRuntime runtime;
    DedicatedServer server(runtime);
    ServerConfig config;
    config.enableLogging = false;
    ASSERT_TRUE(server.InitializeOnly(config));
    const size_t builtInCount = server.GetRconCommands().size();

    // GetRconCommands used to hand out an unlocked reference to the registry
    // while RegisterRconCommand reallocated it; a snapshot must be internally
    // consistent and never shrink while registration runs concurrently.
    constexpr size_t registrations = 256;
    std::atomic<bool> registering{true};
    std::thread writer(
        [&]
        {
            for (size_t index = 0; index < registrations; ++index)
            {
                server.RegisterRconCommand("stress_" + std::to_string(index), "Concurrent registration",
                                           [](const std::vector<std::string>&) { return std::string("ok"); });
            }
            registering.store(false, std::memory_order_release);
        });

    size_t previousSize = builtInCount;
    bool monotonic = true;
    bool wellFormed = true;
    while (registering.load(std::memory_order_acquire))
    {
        const std::vector<RconCommand> snapshot = server.GetRconCommands();
        monotonic = monotonic && snapshot.size() >= previousSize;
        previousSize = snapshot.size();
        for (const RconCommand& command : snapshot)
            wellFormed = wellFormed && !command.name.empty() && static_cast<bool>(command.handler);
    }
    writer.join();

    EXPECT_TRUE(monotonic);
    EXPECT_TRUE(wellFormed);
    EXPECT_EQ(server.GetRconCommands().size(), builtInCount + registrations);
    server.Stop();
}

TEST(DedicatedServerRuntime_StopClearsHandlersAndShutsDownRuntime)
{
    MockNetworkRuntime runtime;
    DedicatedServer server(runtime);

    ServerConfig config;
    config.enableLanBroadcast = false;
    config.mapRotation = {"arena"};

    EXPECT_TRUE(server.InitializeOnly(config));
    EXPECT_FALSE(runtime.handlers.empty());

    server.Stop();
    EXPECT_TRUE(runtime.stopServerCalled);
    EXPECT_TRUE(runtime.shutdownCalled);
    EXPECT_TRUE(runtime.clearHandlersCalled);
    EXPECT_TRUE(runtime.handlers.empty());
}

TEST(DedicatedServerRuntime_LanBroadcastPolicyIsAuthoritativeAndEnabledFailureAllowsRestart)
{
    MockNetworkRuntime runtime;
    std::atomic<int> socketAttempts{0};
    DedicatedServer server(runtime,
                           [&socketAttempts]()
                           {
                               socketAttempts.fetch_add(1, std::memory_order_relaxed);
                               return INVALID_SOCKET;
                           });

    ServerConfig config;
    config.enableLanBroadcast = false;
    config.enableLogging = false;
    EXPECT_TRUE(server.InitializeOnly(config));

    const auto waitUntilInactive = [&server]()
    {
        for (int i = 0; i < 500 && server.IsLanBroadcastActive(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return !server.IsLanBroadcastActive();
    };

    server.StartLanBroadcast();
    EXPECT_TRUE(waitUntilInactive());
    EXPECT_EQ(socketAttempts.load(std::memory_order_relaxed), 0);

    server.Stop();
    config.enableLanBroadcast = true;
    EXPECT_TRUE(server.InitializeOnly(config));

    server.StartLanBroadcast();
    EXPECT_TRUE(waitUntilInactive());
    EXPECT_EQ(socketAttempts.load(std::memory_order_relaxed), 1);

    // Starting again must first reclaim the failed joinable worker instead of
    // assigning over it (which would terminate the process).
    server.StartLanBroadcast();
    EXPECT_TRUE(waitUntilInactive());
    EXPECT_EQ(socketAttempts.load(std::memory_order_relaxed), 2);

    server.StopLanBroadcast();
    server.Stop();
}

TEST(DedicatedServerRuntime_LanBroadcastSnapshotIsOwnedAndConsistentDuringMapMutation)
{
    MockNetworkRuntime runtime;
    DedicatedServer server(runtime);

    ServerConfig config;
    config.serverName = "Snapshot Server";
    config.port = 28015;
    config.maxClients = 24;
    config.lanBroadcastPort = 28016;
    config.enableLanBroadcast = false;
    config.enableLogging = false;
    config.mapRotation = {"map_alpha", "map_beta"};
    EXPECT_TRUE(server.InitializeOnly(config));

    const LanBroadcastSnapshot retained = server.GetLanBroadcastSnapshot();
    EXPECT_EQ(retained.server.serverName, std::string("Snapshot Server"));
    EXPECT_EQ(retained.server.mapName, std::string("map_alpha"));
    EXPECT_EQ(retained.server.port, static_cast<uint16_t>(28015));
    EXPECT_EQ(retained.server.maxPlayers, 24);
    EXPECT_EQ(retained.broadcastPort, static_cast<uint16_t>(28016));

    server.ChangeMap("map_beta");
    const LanBroadcastSnapshot changed = server.GetLanBroadcastSnapshot();
    EXPECT_EQ(changed.server.mapName, std::string("map_beta"));
    EXPECT_EQ(retained.server.mapName, std::string("map_alpha"));

    std::atomic<bool> badSnapshot{false};
    std::thread mapWriter(
        [&server]()
        {
            for (int i = 0; i < 64; ++i)
                server.ChangeMap((i & 1) == 0 ? "map_alpha" : "map_beta");
        });

    for (int i = 0; i < 256; ++i)
    {
        const LanBroadcastSnapshot snapshot = server.GetLanBroadcastSnapshot();
        if (snapshot.server.serverName != "Snapshot Server" ||
            (snapshot.server.mapName != "map_alpha" && snapshot.server.mapName != "map_beta") ||
            snapshot.server.port != 28015 || snapshot.server.maxPlayers != 24 || snapshot.broadcastPort != 28016)
        {
            badSnapshot.store(true, std::memory_order_relaxed);
            break;
        }
    }
    mapWriter.join();

    EXPECT_FALSE(badSnapshot.load(std::memory_order_relaxed));
    server.Stop();
}

TEST(DedicatedServerRuntime_StatsSnapshotIsOwnedAndConsistentDuringMapMutation)
{
    MockNetworkRuntime runtime;
    DedicatedServer server(runtime);

    ServerConfig config;
    config.enableLanBroadcast = false;
    config.enableLogging = false;
    config.mapRotation = {"map_alpha", "map_beta"};
    EXPECT_TRUE(server.InitializeOnly(config));

    const ServerStats retained = server.GetStats();
    EXPECT_EQ(retained.currentMap, std::string("map_alpha"));

    std::atomic<bool> badSnapshot{false};
    std::thread mapWriter(
        [&server]()
        {
            for (int i = 0; i < 64; ++i)
                server.ChangeMap((i & 1) == 0 ? "map_alpha" : "map_beta");
        });

    for (int i = 0; i < 256; ++i)
    {
        const ServerStats snapshot = server.GetStats();
        if (snapshot.currentMap != "map_alpha" && snapshot.currentMap != "map_beta")
        {
            badSnapshot.store(true, std::memory_order_relaxed);
            break;
        }
    }
    mapWriter.join();

    EXPECT_FALSE(badSnapshot.load(std::memory_order_relaxed));
    EXPECT_EQ(retained.currentMap, std::string("map_alpha"));
    server.Stop();
}

TEST(DedicatedServerRuntime_StatsSnapshotsRemainMonotonicWhileTickThreadRuns)
{
    MockNetworkRuntime runtime;
    DedicatedServer server(runtime);

    ServerConfig config;
    config.enableLanBroadcast = false;
    config.enableLogging = false;
    config.mapRotation = {"arena"};
    config.tickRate = 1000.0f;
    const bool started = server.Start(config);
    EXPECT_TRUE(started);
    if (!started)
        return;

    uint64_t previousTicks = 0;
    uint32_t snapshotsObserved = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const ServerStats snapshot = server.GetStats();
        ++snapshotsObserved;
        if (snapshot.totalTicksProcessed < previousTicks)
        {
            server.Stop();
            EXPECT_TRUE(false);
            return;
        }
        previousTicks = snapshot.totalTicksProcessed;
        if (previousTicks >= 16)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    server.Stop();
    EXPECT_TRUE(snapshotsObserved > 1);
    EXPECT_TRUE(previousTicks >= 16);
    EXPECT_EQ(server.GetStats().currentMap, std::string("arena"));
}

#endif // ENABLE_NETWORKING
