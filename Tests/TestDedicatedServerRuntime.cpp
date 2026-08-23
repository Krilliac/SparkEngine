#include "TestFramework.h"

#include "Engine/Networking/DedicatedServer.h"

#ifdef ENABLE_NETWORKING

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Spark::Net;

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

        bool StartServer(uint16_t port, int maxClients) override
        {
            startServerCalled = true;
            startedPort = port;
            startedMaxClients = maxClients;
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

    runtime.Dispatch({.type = MessageType::Connect, .senderID = 7});
    EXPECT_TRUE(connectedCalled);
    EXPECT_EQ(connectedName, std::string("Alice"));
    EXPECT_EQ(server.GetStats().totalConnectionsServed, static_cast<uint32_t>(1));
    EXPECT_EQ(server.GetStats().currentPlayers, static_cast<uint32_t>(1));

    runtime.Dispatch({.type = MessageType::Disconnect, .senderID = 7});
    EXPECT_TRUE(disconnectedCalled);
    EXPECT_EQ(disconnectedClient, static_cast<ClientID>(7));

    server.Stop();
    EXPECT_TRUE(runtime.clearHandlersCalled);
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

#endif // ENABLE_NETWORKING
