// TestServerMockClient.cpp — Server-side tests driven by an in-process MockClient
//
// All networking primitives are reimplemented locally (no sockets, no
// ENABLE_NETWORKING required).  The MockServer and MockClient classes model the
// exchange of protocol messages entirely in memory, allowing full coverage of:
//
//   • Connection handshake  (Connect → ConnectAccepted / ConnectRejected)
//   • Message routing       (server dispatches to registered handlers)
//   • Chat broadcast        (server sends to all / except sender)
//   • Client input pipeline (MockClient sends input, server queues it)
//   • Entity state replication
//   • Disconnect handling   (graceful and timeout)
//   • Multi-client scenarios
//   • Server capacity limits

#include "TestFramework.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// Protocol primitives  (mirrors Spark::Net without the socket layer)
// ============================================================================

namespace
{

    using ClientID = uint32_t;
    constexpr ClientID INVALID_CLIENT = 0;
    constexpr ClientID SERVER_CLIENT_ID = 0; // server's own ID when it sends

    enum class MessageType : uint16_t
    {
        Connect = 1,
        ConnectAccepted,
        ConnectRejected,
        Disconnect,
        Heartbeat,
        ClientInput,
        ChatMessage,
        EntityStateUpdate,
        EntitySpawn,
        EntityDestroy,
        GameStateSync,
        ScoreUpdate,
        UserDefined = 1000
    };

    enum class ChannelType : uint8_t
    {
        Unreliable = 0,
        Reliable,
        ReliableOrdered
    };

    struct NetworkMessage
    {
        MessageType type = MessageType::UserDefined;
        ChannelType channel = ChannelType::Unreliable;
        ClientID senderID = INVALID_CLIENT;
        uint32_t sequence = 0;
        float timestamp = 0.0f;
        std::vector<uint8_t> payload;
    };

    // Simple in-memory serialization buffer shared by MockClient / MockServer
    struct SimpleBuffer
    {
        std::vector<uint8_t> data;
        size_t readPos = 0;
        bool error = false;

        void WriteUint8(uint8_t v) { data.push_back(v); }
        void WriteUint16(uint16_t v)
        {
            data.push_back(v & 0xFF);
            data.push_back((v >> 8) & 0xFF);
        }
        void WriteUint32(uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
                data.push_back((v >> (i * 8)) & 0xFF);
        }
        void WriteFloat(float v)
        {
            uint32_t bits;
            std::memcpy(&bits, &v, 4);
            WriteUint32(bits);
        }
        void WriteString(const std::string& s)
        {
            WriteUint16(static_cast<uint16_t>(s.size()));
            for (char c : s)
                data.push_back(static_cast<uint8_t>(c));
        }
        void WriteBytes(const uint8_t* src, size_t n) { data.insert(data.end(), src, src + n); }

        uint8_t ReadUint8()
        {
            if (error || readPos >= data.size())
            {
                error = true;
                return 0;
            }
            return data[readPos++];
        }
        uint16_t ReadUint16()
        {
            uint16_t v = 0;
            if (error || readPos + 2 > data.size())
            {
                error = true;
                return 0;
            }
            for (int i = 0; i < 2; ++i)
                v |= static_cast<uint16_t>(data[readPos++]) << (i * 8);
            return v;
        }
        uint32_t ReadUint32()
        {
            uint32_t v = 0;
            if (error || readPos + 4 > data.size())
            {
                error = true;
                return 0;
            }
            for (int i = 0; i < 4; ++i)
                v |= static_cast<uint32_t>(data[readPos++]) << (i * 8);
            return v;
        }
        float ReadFloat()
        {
            uint32_t bits = ReadUint32();
            float v;
            std::memcpy(&v, &bits, 4);
            return v;
        }
        std::string ReadString()
        {
            uint16_t len = ReadUint16();
            if (error || readPos + len > data.size())
            {
                error = true;
                return {};
            }
            std::string s(data.begin() + static_cast<long>(readPos), data.begin() + static_cast<long>(readPos + len));
            readPos += len;
            return s;
        }

        bool HasError() const { return error; }
    };

    // =========================================================================
    // MockServer — in-process message router
    // =========================================================================

    class MockServer
    {
      public:
        struct ClientRecord
        {
            ClientID id = INVALID_CLIENT;
            std::string playerName;
            float lastHeartbeatTime = 0.0f;
            bool connected = false;
        };

        struct EntityRecord
        {
            uint32_t networkID = 0;
            float x = 0, y = 0, z = 0;
        };

        explicit MockServer(int maxClients = 4) : m_maxClients(maxClients) {}

        // Register a handler for a message type (server-side).
        using Handler = std::function<void(const NetworkMessage&)>;
        void RegisterHandler(MessageType type, Handler h) { m_handlers[static_cast<uint16_t>(type)] = std::move(h); }

        // Deliver a message from a client to the server.
        // Returns false if the server rejects the message (e.g. from unknown client).
        bool Receive(const NetworkMessage& msg)
        {
            // Connection requests are always accepted for routing
            if (msg.type == MessageType::Connect)
            {
                Dispatch(msg);
                return true;
            }
            // All other messages must come from a known connected client
            if (!IsConnected(msg.senderID))
                return false;
            Dispatch(msg);
            return true;
        }

        // Server-side: accept a connecting client and return its assigned ID.
        // Returns INVALID_CLIENT if the server is full.
        ClientID AcceptClient(const std::string& playerName)
        {
            if (static_cast<int>(m_clients.size()) >= m_maxClients)
                return INVALID_CLIENT;
            ClientID id = m_nextClientID++;
            m_clients[id] = {id, playerName, 0.0f, true};
            return id;
        }

        void DisconnectClient(ClientID id, const std::string& /*reason*/ = "") { m_clients.erase(id); }

        bool IsConnected(ClientID id) const
        {
            auto it = m_clients.find(id);
            return it != m_clients.end() && it->second.connected;
        }

        int GetClientCount() const { return static_cast<int>(m_clients.size()); }
        int GetMaxClients() const { return m_maxClients; }
        bool IsFull() const { return GetClientCount() >= m_maxClients; }

        const std::unordered_map<ClientID, ClientRecord>& GetClients() const { return m_clients; }

        // Broadcast a message to all connected clients.
        // (In the real server these go to sockets; here they go to m_sentToClients.)
        void BroadcastMessage(const NetworkMessage& msg)
        {
            for (const auto& [id, rec] : m_clients)
            {
                if (rec.connected)
                    m_sentToClients[id].push_back(msg);
            }
        }

        void SendToClient(ClientID id, const NetworkMessage& msg)
        {
            if (IsConnected(id))
                m_sentToClients[id].push_back(msg);
        }

        void BroadcastExcept(ClientID exclude, const NetworkMessage& msg)
        {
            for (const auto& [id, rec] : m_clients)
            {
                if (rec.connected && id != exclude)
                    m_sentToClients[id].push_back(msg);
            }
        }

        // Retrieve all messages the server has sent to a given client.
        const std::vector<NetworkMessage>& GetSentTo(ClientID id) const
        {
            static const std::vector<NetworkMessage> empty;
            auto it = m_sentToClients.find(id);
            return it != m_sentToClients.end() ? it->second : empty;
        }

        void ClearSent() { m_sentToClients.clear(); }

        // Register a replicated entity and return its network ID.
        uint32_t RegisterEntity(float x, float y, float z)
        {
            uint32_t id = m_nextEntityID++;
            m_entities[id] = {id, x, y, z};
            return id;
        }

        void UpdateEntityPosition(uint32_t networkID, float x, float y, float z)
        {
            auto it = m_entities.find(networkID);
            if (it != m_entities.end())
            {
                it->second.x = x;
                it->second.y = y;
                it->second.z = z;
            }
        }

        const std::unordered_map<uint32_t, EntityRecord>& GetEntities() const { return m_entities; }

        // Serialize and broadcast entity state to all clients.
        void ReplicateEntityState(uint32_t networkID)
        {
            auto it = m_entities.find(networkID);
            if (it == m_entities.end())
                return;

            SimpleBuffer buf;
            buf.WriteUint32(it->second.networkID);
            buf.WriteFloat(it->second.x);
            buf.WriteFloat(it->second.y);
            buf.WriteFloat(it->second.z);

            NetworkMessage msg;
            msg.type = MessageType::EntityStateUpdate;
            msg.channel = ChannelType::Unreliable;
            msg.senderID = SERVER_CLIENT_ID;
            msg.payload.assign(buf.data.begin(), buf.data.end());

            BroadcastMessage(msg);
        }

      private:
        void Dispatch(const NetworkMessage& msg)
        {
            auto it = m_handlers.find(static_cast<uint16_t>(msg.type));
            if (it != m_handlers.end() && it->second)
                it->second(msg);
        }

        int m_maxClients;
        ClientID m_nextClientID = 1;
        uint32_t m_nextEntityID = 1;
        std::unordered_map<ClientID, ClientRecord> m_clients;
        std::unordered_map<uint16_t, Handler> m_handlers;
        std::unordered_map<ClientID, std::vector<NetworkMessage>> m_sentToClients;
        std::unordered_map<uint32_t, EntityRecord> m_entities;
    };

    // =========================================================================
    // MockClient — simulates a game client connected to a MockServer
    // =========================================================================

    class MockClient
    {
      public:
        enum class State
        {
            Disconnected,
            Connecting,
            Connected,
            Disconnecting
        };

        explicit MockClient(const std::string& playerName) : m_playerName(playerName) {}

        // Initiate connection: sends a Connect message to the server.
        NetworkMessage BuildConnectMessage() const
        {
            SimpleBuffer buf;
            buf.WriteString(m_playerName);

            NetworkMessage msg;
            msg.type = MessageType::Connect;
            msg.channel = ChannelType::Reliable;
            msg.senderID = INVALID_CLIENT; // Not yet assigned
            msg.payload.assign(buf.data.begin(), buf.data.end());
            return msg;
        }

        // Called when the server accepts the client.
        void OnConnectAccepted(ClientID assignedID)
        {
            m_id = assignedID;
            m_state = State::Connected;
        }

        // Called when the server rejects the client.
        void OnConnectRejected(const std::string& reason)
        {
            m_rejectReason = reason;
            m_state = State::Disconnected;
        }

        // Build a Disconnect message.
        NetworkMessage BuildDisconnectMessage() const
        {
            NetworkMessage msg;
            msg.type = MessageType::Disconnect;
            msg.channel = ChannelType::Reliable;
            msg.senderID = m_id;
            return msg;
        }

        void Disconnect() { m_state = State::Disconnecting; }

        void OnDisconnected()
        {
            m_id = INVALID_CLIENT;
            m_state = State::Disconnected;
        }

        // Build a chat message.
        NetworkMessage BuildChatMessage(const std::string& text) const
        {
            SimpleBuffer buf;
            buf.WriteUint8(1); // global channel
            buf.WriteString(m_playerName);
            buf.WriteString(text);

            NetworkMessage msg;
            msg.type = MessageType::ChatMessage;
            msg.channel = ChannelType::ReliableOrdered;
            msg.senderID = m_id;
            msg.payload.assign(buf.data.begin(), buf.data.end());
            return msg;
        }

        struct InputState
        {
            float moveX = 0, moveY = 0, moveZ = 0;
            bool jump = false;
            bool crouch = false;
            bool fire = false;
            uint32_t sequence = 0;
        };

        // Build a ClientInput message.
        NetworkMessage BuildInputMessage(const InputState& input) const
        {
            SimpleBuffer buf;
            buf.WriteUint32(input.sequence);
            buf.WriteFloat(input.moveX);
            buf.WriteFloat(input.moveY);
            buf.WriteFloat(input.moveZ);
            buf.WriteUint8(input.jump ? 1u : 0u);
            buf.WriteUint8(input.crouch ? 1u : 0u);
            buf.WriteUint8(input.fire ? 1u : 0u);

            NetworkMessage msg;
            msg.type = MessageType::ClientInput;
            msg.channel = ChannelType::Unreliable;
            msg.senderID = m_id;
            msg.payload.assign(buf.data.begin(), buf.data.end());
            return msg;
        }

        // Receive a message from the server (queues it for later inspection).
        void Receive(const NetworkMessage& msg) { m_inbox.push_back(msg); }

        // Retrieve all received messages of a given type.
        std::vector<NetworkMessage> GetReceived(MessageType type) const
        {
            std::vector<NetworkMessage> out;
            for (const auto& m : m_inbox)
                if (m.type == type)
                    out.push_back(m);
            return out;
        }

        void ClearInbox() { m_inbox.clear(); }

        // Accessors
        ClientID GetID() const { return m_id; }
        State GetState() const { return m_state; }
        const std::string& GetPlayerName() const { return m_playerName; }
        const std::string& GetRejectReason() const { return m_rejectReason; }
        bool IsConnected() const { return m_state == State::Connected; }
        const std::vector<NetworkMessage>& GetInbox() const { return m_inbox; }

      private:
        std::string m_playerName;
        ClientID m_id = INVALID_CLIENT;
        State m_state = State::Disconnected;
        std::string m_rejectReason;
        std::vector<NetworkMessage> m_inbox;
    };

    // =========================================================================
    // Helper: perform a full connect handshake between a MockClient and a
    // MockServer, returning the assigned ClientID (INVALID_CLIENT on failure).
    // =========================================================================

    ClientID PerformHandshake(MockClient& client, MockServer& server)
    {
        // Client → server: Connect
        NetworkMessage connectMsg = client.BuildConnectMessage();

        bool accepted = false;
        ClientID assignedID = INVALID_CLIENT;

        server.RegisterHandler(MessageType::Connect,
                               [&](const NetworkMessage& msg)
                               {
                                   SimpleBuffer buf;
                                   buf.data.assign(msg.payload.begin(), msg.payload.end());
                                   std::string name = buf.ReadString();

                                   if (server.IsFull())
                                   {
                                       NetworkMessage reject;
                                       reject.type = MessageType::ConnectRejected;
                                       // Store rejection payload
                                       SimpleBuffer rb;
                                       rb.WriteString("Server full");
                                       reject.payload.assign(rb.data.begin(), rb.data.end());
                                       client.OnConnectRejected("Server full");
                                       return;
                                   }

                                   assignedID = server.AcceptClient(name);
                                   NetworkMessage accept;
                                   accept.type = MessageType::ConnectAccepted;
                                   accept.senderID = SERVER_CLIENT_ID;
                                   SimpleBuffer ab;
                                   ab.WriteUint32(assignedID);
                                   accept.payload.assign(ab.data.begin(), ab.data.end());
                                   client.OnConnectAccepted(assignedID);
                                   accepted = true;
                               });

        server.Receive(connectMsg);
        return accepted ? assignedID : INVALID_CLIENT;
    }

} // namespace

// ============================================================================
// Tests: Connection handshake
// ============================================================================

TEST(MockClient_ConnectHandshakeBasic)
{
    MockServer server(4);
    MockClient client("Alice");

    EXPECT_EQ(static_cast<int>(client.GetState()), static_cast<int>(MockClient::State::Disconnected));

    ClientID id = PerformHandshake(client, server);

    EXPECT_TRUE(id != INVALID_CLIENT);
    EXPECT_EQ(client.GetID(), id);
    EXPECT_TRUE(client.IsConnected());
    EXPECT_EQ(static_cast<int>(client.GetState()), static_cast<int>(MockClient::State::Connected));
    EXPECT_EQ(server.GetClientCount(), 1);
    EXPECT_TRUE(server.IsConnected(id));
}

TEST(MockClient_ConnectAssignsUniqueIDs)
{
    MockServer server(4);
    MockClient alice("Alice");
    MockClient bob("Bob");
    MockClient carol("Carol");

    ClientID idA = PerformHandshake(alice, server);
    ClientID idB = PerformHandshake(bob, server);
    ClientID idC = PerformHandshake(carol, server);

    EXPECT_NE(idA, INVALID_CLIENT);
    EXPECT_NE(idB, INVALID_CLIENT);
    EXPECT_NE(idC, INVALID_CLIENT);
    EXPECT_NE(idA, idB);
    EXPECT_NE(idB, idC);
    EXPECT_NE(idA, idC);
    EXPECT_EQ(server.GetClientCount(), 3);
}

TEST(MockClient_ConnectRejectedWhenFull)
{
    MockServer server(2);
    MockClient a("A"), b("B"), c("C");

    PerformHandshake(a, server);
    PerformHandshake(b, server);
    ClientID idC = PerformHandshake(c, server);

    EXPECT_EQ(idC, INVALID_CLIENT);
    EXPECT_EQ(static_cast<int>(c.GetState()), static_cast<int>(MockClient::State::Disconnected));
    EXPECT_EQ(c.GetRejectReason(), std::string("Server full"));
    EXPECT_EQ(server.GetClientCount(), 2);
}

TEST(MockClient_ConnectPlayerNamePreserved)
{
    MockServer server(4);
    MockClient client("Sparky_42");

    PerformHandshake(client, server);

    EXPECT_EQ(client.GetPlayerName(), std::string("Sparky_42"));

    const auto& clients = server.GetClients();
    EXPECT_FALSE(clients.empty());

    // Verify the server stored the correct name
    bool found = false;
    for (const auto& [id, rec] : clients)
    {
        if (rec.playerName == "Sparky_42")
        {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

// ============================================================================
// Tests: Graceful disconnect
// ============================================================================

TEST(MockClient_GracefulDisconnect)
{
    MockServer server(4);
    MockClient client("Alice");

    ClientID id = PerformHandshake(client, server);
    EXPECT_EQ(server.GetClientCount(), 1);

    // Register disconnect handler
    bool disconnectFired = false;
    server.RegisterHandler(MessageType::Disconnect,
                           [&](const NetworkMessage& msg)
                           {
                               server.DisconnectClient(msg.senderID);
                               disconnectFired = true;
                           });

    // Client sends disconnect
    NetworkMessage disc = client.BuildDisconnectMessage();
    client.Disconnect();
    server.Receive(disc);
    client.OnDisconnected();

    EXPECT_TRUE(disconnectFired);
    EXPECT_EQ(server.GetClientCount(), 0);
    EXPECT_FALSE(server.IsConnected(id));
    EXPECT_EQ(static_cast<int>(client.GetState()), static_cast<int>(MockClient::State::Disconnected));
    EXPECT_EQ(client.GetID(), INVALID_CLIENT);
}

TEST(MockClient_ServerKicksClient)
{
    MockServer server(4);
    MockClient client("Alice");

    ClientID id = PerformHandshake(client, server);
    EXPECT_TRUE(server.IsConnected(id));

    // Server kicks Alice
    server.DisconnectClient(id, "kicked");
    client.OnDisconnected();

    EXPECT_FALSE(server.IsConnected(id));
    EXPECT_EQ(server.GetClientCount(), 0);
    EXPECT_EQ(static_cast<int>(client.GetState()), static_cast<int>(MockClient::State::Disconnected));
}

TEST(MockClient_DisconnectOneOfMany)
{
    MockServer server(4);
    MockClient alice("Alice"), bob("Bob"), carol("Carol");

    PerformHandshake(alice, server);
    PerformHandshake(bob, server);
    ClientID idC = PerformHandshake(carol, server);

    server.RegisterHandler(MessageType::Disconnect,
                           [&](const NetworkMessage& msg) { server.DisconnectClient(msg.senderID); });

    NetworkMessage disc = carol.BuildDisconnectMessage();
    server.Receive(disc);
    carol.OnDisconnected();

    EXPECT_EQ(server.GetClientCount(), 2);
    EXPECT_FALSE(server.IsConnected(idC));
    EXPECT_TRUE(alice.IsConnected());
    EXPECT_TRUE(bob.IsConnected());
}

// ============================================================================
// Tests: Message routing
// ============================================================================

TEST(MockClient_ServerDispatchesMessageToHandler)
{
    MockServer server(4);
    MockClient client("Alice");
    PerformHandshake(client, server);

    int callCount = 0;
    server.RegisterHandler(MessageType::ChatMessage, [&](const NetworkMessage& /*msg*/) { ++callCount; });

    NetworkMessage chat = client.BuildChatMessage("Hello!");
    server.Receive(chat);

    EXPECT_EQ(callCount, 1);
}

TEST(MockClient_ServerIgnoresMessageFromUnknownClient)
{
    MockServer server(4);

    int callCount = 0;
    server.RegisterHandler(MessageType::ChatMessage, [&](const NetworkMessage& /*msg*/) { ++callCount; });

    // Message from an ID that never connected
    NetworkMessage msg;
    msg.type = MessageType::ChatMessage;
    msg.senderID = 999; // Unknown
    bool delivered = server.Receive(msg);

    EXPECT_FALSE(delivered);
    EXPECT_EQ(callCount, 0);
}

TEST(MockClient_ServerRoutesMultipleMessageTypes)
{
    MockServer server(4);
    MockClient client("Alice");
    PerformHandshake(client, server);

    int chatCount = 0, inputCount = 0;
    server.RegisterHandler(MessageType::ChatMessage, [&](const NetworkMessage&) { ++chatCount; });
    server.RegisterHandler(MessageType::ClientInput, [&](const NetworkMessage&) { ++inputCount; });

    server.Receive(client.BuildChatMessage("hi"));
    server.Receive(client.BuildChatMessage("there"));

    MockClient::InputState input{};
    input.sequence = 1;
    input.moveX = 1.0f;
    server.Receive(client.BuildInputMessage(input));

    EXPECT_EQ(chatCount, 2);
    EXPECT_EQ(inputCount, 1);
}

// ============================================================================
// Tests: Chat broadcast
// ============================================================================

TEST(MockClient_ServerBroadcastsChatToAll)
{
    MockServer server(4);
    MockClient alice("Alice"), bob("Bob"), carol("Carol");

    ClientID idA = PerformHandshake(alice, server);
    ClientID idB = PerformHandshake(bob, server);
    ClientID idC = PerformHandshake(carol, server);

    // Server receives a chat from Alice and broadcasts it to everyone
    server.RegisterHandler(MessageType::ChatMessage,
                           [&](const NetworkMessage& msg)
                           {
                               // Server relays the chat to all clients
                               server.BroadcastMessage(msg);
                           });

    server.Receive(alice.BuildChatMessage("Hey all!"));

    // All three clients should have received it
    const auto& sentA = server.GetSentTo(idA);
    const auto& sentB = server.GetSentTo(idB);
    const auto& sentC = server.GetSentTo(idC);

    EXPECT_EQ(sentA.size(), static_cast<size_t>(1));
    EXPECT_EQ(sentB.size(), static_cast<size_t>(1));
    EXPECT_EQ(sentC.size(), static_cast<size_t>(1));

    EXPECT_EQ(static_cast<int>(sentA[0].type), static_cast<int>(MessageType::ChatMessage));
    EXPECT_EQ(static_cast<int>(sentB[0].type), static_cast<int>(MessageType::ChatMessage));
    EXPECT_EQ(static_cast<int>(sentC[0].type), static_cast<int>(MessageType::ChatMessage));
}

TEST(MockClient_ServerBroadcastsExceptSender)
{
    MockServer server(4);
    MockClient alice("Alice"), bob("Bob"), carol("Carol");

    ClientID idA = PerformHandshake(alice, server);
    ClientID idB = PerformHandshake(bob, server);
    ClientID idC = PerformHandshake(carol, server);

    server.RegisterHandler(MessageType::ChatMessage,
                           [&](const NetworkMessage& msg)
                           {
                               // Relay to everyone except the sender
                               server.BroadcastExcept(msg.senderID, msg);
                           });

    server.Receive(alice.BuildChatMessage("Hi"));

    // Bob and Carol receive it; Alice does not
    EXPECT_EQ(server.GetSentTo(idA).size(), static_cast<size_t>(0));
    EXPECT_EQ(server.GetSentTo(idB).size(), static_cast<size_t>(1));
    EXPECT_EQ(server.GetSentTo(idC).size(), static_cast<size_t>(1));
}

TEST(MockClient_ChatPayloadRoundTrip)
{
    MockServer server(4);
    MockClient client("Bob");
    PerformHandshake(client, server);

    std::string capturedName, capturedText;
    server.RegisterHandler(MessageType::ChatMessage,
                           [&](const NetworkMessage& msg)
                           {
                               SimpleBuffer buf;
                               buf.data.assign(msg.payload.begin(), msg.payload.end());
                               buf.ReadUint8(); // channel
                               capturedName = buf.ReadString();
                               capturedText = buf.ReadString();
                           });

    server.Receive(client.BuildChatMessage("Hello Server!"));

    EXPECT_EQ(capturedName, std::string("Bob"));
    EXPECT_EQ(capturedText, std::string("Hello Server!"));
}

// ============================================================================
// Tests: Client input pipeline
// ============================================================================

TEST(MockClient_InputMessageDelivered)
{
    MockServer server(4);
    MockClient client("Alice");
    PerformHandshake(client, server);

    MockClient::InputState receivedInput{};
    bool inputReceived = false;

    server.RegisterHandler(MessageType::ClientInput,
                           [&](const NetworkMessage& msg)
                           {
                               SimpleBuffer buf;
                               buf.data.assign(msg.payload.begin(), msg.payload.end());
                               receivedInput.sequence = buf.ReadUint32();
                               receivedInput.moveX = buf.ReadFloat();
                               receivedInput.moveY = buf.ReadFloat();
                               receivedInput.moveZ = buf.ReadFloat();
                               receivedInput.jump = (buf.ReadUint8() != 0);
                               receivedInput.crouch = (buf.ReadUint8() != 0);
                               receivedInput.fire = (buf.ReadUint8() != 0);
                               inputReceived = true;
                           });

    MockClient::InputState input{};
    input.sequence = 42;
    input.moveX = 0.5f;
    input.moveY = 0.0f;
    input.moveZ = -1.0f;
    input.jump = true;
    input.crouch = false;
    input.fire = false;

    server.Receive(client.BuildInputMessage(input));

    EXPECT_TRUE(inputReceived);
    EXPECT_EQ(receivedInput.sequence, static_cast<uint32_t>(42));
    EXPECT_NEAR(receivedInput.moveX, 0.5f, 0.001f);
    EXPECT_NEAR(receivedInput.moveZ, -1.0f, 0.001f);
    EXPECT_TRUE(receivedInput.jump);
    EXPECT_FALSE(receivedInput.crouch);
    EXPECT_FALSE(receivedInput.fire);
}

TEST(MockClient_MultipleInputsDeliveredInOrder)
{
    MockServer server(4);
    MockClient client("Alice");
    PerformHandshake(client, server);

    std::vector<uint32_t> receivedSequences;
    server.RegisterHandler(MessageType::ClientInput,
                           [&](const NetworkMessage& msg)
                           {
                               SimpleBuffer buf;
                               buf.data.assign(msg.payload.begin(), msg.payload.end());
                               receivedSequences.push_back(buf.ReadUint32());
                           });

    for (uint32_t seq = 1; seq <= 5; ++seq)
    {
        MockClient::InputState input{};
        input.sequence = seq;
        server.Receive(client.BuildInputMessage(input));
    }

    EXPECT_EQ(receivedSequences.size(), static_cast<size_t>(5));
    for (uint32_t i = 0; i < 5; ++i)
        EXPECT_EQ(receivedSequences[i], i + 1);
}

TEST(MockClient_ServerAccumulatesInputsFromMultipleClients)
{
    MockServer server(4);
    MockClient alice("Alice"), bob("Bob");
    PerformHandshake(alice, server);
    PerformHandshake(bob, server);

    int inputCount = 0;
    server.RegisterHandler(MessageType::ClientInput, [&](const NetworkMessage& /*msg*/) { ++inputCount; });

    MockClient::InputState inp{};
    inp.sequence = 1;

    server.Receive(alice.BuildInputMessage(inp));
    inp.sequence = 2;
    server.Receive(alice.BuildInputMessage(inp));
    inp.sequence = 1;
    server.Receive(bob.BuildInputMessage(inp));

    EXPECT_EQ(inputCount, 3);
}

// ============================================================================
// Tests: Entity state replication
// ============================================================================

TEST(MockClient_ServerReplicatesEntityToConnectedClients)
{
    MockServer server(4);
    MockClient alice("Alice"), bob("Bob");

    ClientID idA = PerformHandshake(alice, server);
    ClientID idB = PerformHandshake(bob, server);

    uint32_t entityID = server.RegisterEntity(10.0f, 0.0f, -5.0f);
    server.ReplicateEntityState(entityID);

    const auto& sentA = server.GetSentTo(idA);
    const auto& sentB = server.GetSentTo(idB);

    EXPECT_EQ(sentA.size(), static_cast<size_t>(1));
    EXPECT_EQ(sentB.size(), static_cast<size_t>(1));
    EXPECT_EQ(static_cast<int>(sentA[0].type), static_cast<int>(MessageType::EntityStateUpdate));
    EXPECT_EQ(static_cast<int>(sentB[0].type), static_cast<int>(MessageType::EntityStateUpdate));
}

TEST(MockClient_EntityStatePayloadCorrect)
{
    MockServer server(4);
    MockClient client("Alice");
    ClientID id = PerformHandshake(client, server);

    uint32_t entityID = server.RegisterEntity(3.0f, 7.5f, -2.0f);
    server.ReplicateEntityState(entityID);

    const auto& sent = server.GetSentTo(id);
    EXPECT_EQ(sent.size(), static_cast<size_t>(1));

    SimpleBuffer buf;
    buf.data.assign(sent[0].payload.begin(), sent[0].payload.end());
    uint32_t netID = buf.ReadUint32();
    float x = buf.ReadFloat();
    float y = buf.ReadFloat();
    float z = buf.ReadFloat();

    EXPECT_EQ(netID, entityID);
    EXPECT_NEAR(x, 3.0f, 0.001f);
    EXPECT_NEAR(y, 7.5f, 0.001f);
    EXPECT_NEAR(z, -2.0f, 0.001f);
    EXPECT_FALSE(buf.HasError());
}

TEST(MockClient_EntityPositionUpdateReplicates)
{
    MockServer server(4);
    MockClient client("Alice");
    ClientID id = PerformHandshake(client, server);

    uint32_t entityID = server.RegisterEntity(0.0f, 0.0f, 0.0f);
    server.UpdateEntityPosition(entityID, 100.0f, 5.0f, -50.0f);
    server.ReplicateEntityState(entityID);

    const auto& sent = server.GetSentTo(id);
    EXPECT_EQ(sent.size(), static_cast<size_t>(1));

    SimpleBuffer buf;
    buf.data.assign(sent[0].payload.begin(), sent[0].payload.end());
    buf.ReadUint32(); // networkID
    float x = buf.ReadFloat();
    float y = buf.ReadFloat();
    float z = buf.ReadFloat();

    EXPECT_NEAR(x, 100.0f, 0.001f);
    EXPECT_NEAR(y, 5.0f, 0.001f);
    EXPECT_NEAR(z, -50.0f, 0.001f);
}

TEST(MockClient_MultipleEntitiesReplicated)
{
    MockServer server(4);
    MockClient client("Alice");
    ClientID id = PerformHandshake(client, server);

    uint32_t e1 = server.RegisterEntity(1, 0, 0);
    uint32_t e2 = server.RegisterEntity(2, 0, 0);
    uint32_t e3 = server.RegisterEntity(3, 0, 0);

    server.ReplicateEntityState(e1);
    server.ReplicateEntityState(e2);
    server.ReplicateEntityState(e3);

    EXPECT_EQ(server.GetSentTo(id).size(), static_cast<size_t>(3));
}

TEST(MockClient_NoReplicationToDisconnectedClient)
{
    MockServer server(4);
    MockClient alice("Alice"), bob("Bob");

    ClientID idA = PerformHandshake(alice, server);
    ClientID idB = PerformHandshake(bob, server);

    // Bob disconnects before the replication
    server.DisconnectClient(idB);

    uint32_t entityID = server.RegisterEntity(0.0f, 0.0f, 0.0f);
    server.ReplicateEntityState(entityID);

    EXPECT_EQ(server.GetSentTo(idA).size(), static_cast<size_t>(1));
    EXPECT_EQ(server.GetSentTo(idB).size(), static_cast<size_t>(0));
}

// ============================================================================
// Tests: Server capacity
// ============================================================================

TEST(MockClient_ServerAtMaxCapacity)
{
    MockServer server(3);
    MockClient a("A"), b("B"), c("C"), d("D");

    EXPECT_NE(PerformHandshake(a, server), INVALID_CLIENT);
    EXPECT_NE(PerformHandshake(b, server), INVALID_CLIENT);
    EXPECT_NE(PerformHandshake(c, server), INVALID_CLIENT);

    EXPECT_TRUE(server.IsFull());

    ClientID idD = PerformHandshake(d, server);
    EXPECT_EQ(idD, INVALID_CLIENT);
    EXPECT_EQ(static_cast<int>(d.GetState()), static_cast<int>(MockClient::State::Disconnected));
    EXPECT_EQ(server.GetClientCount(), 3);
}

TEST(MockClient_SlotReclaimedAfterDisconnect)
{
    MockServer server(2);
    MockClient a("A"), b("B"), c("C");

    ClientID idA = PerformHandshake(a, server);
    PerformHandshake(b, server);
    EXPECT_TRUE(server.IsFull());

    // A disconnects, freeing a slot
    server.DisconnectClient(idA);
    a.OnDisconnected();
    EXPECT_FALSE(server.IsFull());

    // C can now connect
    ClientID idC = PerformHandshake(c, server);
    EXPECT_NE(idC, INVALID_CLIENT);
    EXPECT_TRUE(c.IsConnected());
    EXPECT_EQ(server.GetClientCount(), 2);
}

// ============================================================================
// Tests: Server-to-client messages
// ============================================================================

TEST(MockClient_ServerSendsDirectMessageToClient)
{
    MockServer server(4);
    MockClient client("Alice");
    ClientID id = PerformHandshake(client, server);

    NetworkMessage msg;
    msg.type = MessageType::GameStateSync;
    msg.senderID = SERVER_CLIENT_ID;

    SimpleBuffer buf;
    buf.WriteUint32(100); // e.g. match time remaining
    msg.payload.assign(buf.data.begin(), buf.data.end());

    server.SendToClient(id, msg);
    client.Receive(server.GetSentTo(id)[0]);

    auto received = client.GetReceived(MessageType::GameStateSync);
    EXPECT_EQ(received.size(), static_cast<size_t>(1));
    EXPECT_EQ(static_cast<int>(received[0].type), static_cast<int>(MessageType::GameStateSync));
}

TEST(MockClient_ServerScoreBroadcast)
{
    MockServer server(4);
    MockClient alice("Alice"), bob("Bob");
    ClientID idA = PerformHandshake(alice, server);
    ClientID idB = PerformHandshake(bob, server);

    NetworkMessage scoreMsg;
    scoreMsg.type = MessageType::ScoreUpdate;
    scoreMsg.senderID = SERVER_CLIENT_ID;

    SimpleBuffer buf;
    buf.WriteUint32(3); // team A score
    buf.WriteUint32(1); // team B score
    scoreMsg.payload.assign(buf.data.begin(), buf.data.end());

    server.BroadcastMessage(scoreMsg);

    // Both clients receive the score update
    const auto& sentA = server.GetSentTo(idA);
    const auto& sentB = server.GetSentTo(idB);

    EXPECT_EQ(sentA.size(), static_cast<size_t>(1));
    EXPECT_EQ(sentB.size(), static_cast<size_t>(1));
    EXPECT_EQ(static_cast<int>(sentA[0].type), static_cast<int>(MessageType::ScoreUpdate));
    EXPECT_EQ(static_cast<int>(sentB[0].type), static_cast<int>(MessageType::ScoreUpdate));
}

// ============================================================================
// Tests: Reconnect scenario
// ============================================================================

TEST(MockClient_ReconnectAfterDisconnect)
{
    MockServer server(4);
    MockClient client("Alice");

    // First connection
    ClientID id1 = PerformHandshake(client, server);
    EXPECT_NE(id1, INVALID_CLIENT);
    EXPECT_TRUE(client.IsConnected());

    // Disconnect
    server.DisconnectClient(id1);
    client.OnDisconnected();
    EXPECT_FALSE(client.IsConnected());

    // Reconnect — new ID may differ
    ClientID id2 = PerformHandshake(client, server);
    EXPECT_NE(id2, INVALID_CLIENT);
    EXPECT_TRUE(client.IsConnected());
    EXPECT_EQ(server.GetClientCount(), 1);
}

// ============================================================================
// Tests: Edge cases
// ============================================================================

TEST(MockClient_EmptyPlayerNameAllowed)
{
    MockServer server(4);
    MockClient client("");

    ClientID id = PerformHandshake(client, server);
    EXPECT_NE(id, INVALID_CLIENT);
    EXPECT_TRUE(client.IsConnected());
}

TEST(MockClient_LongPlayerNameHandled)
{
    MockServer server(4);
    std::string longName(200, 'X');
    MockClient client(longName);

    ClientID id = PerformHandshake(client, server);
    EXPECT_NE(id, INVALID_CLIENT);
    EXPECT_EQ(client.GetPlayerName(), longName);
}

TEST(MockClient_ServerClearSentMessages)
{
    MockServer server(4);
    MockClient client("Alice");
    ClientID id = PerformHandshake(client, server);

    server.ReplicateEntityState(server.RegisterEntity(0, 0, 0));
    EXPECT_EQ(server.GetSentTo(id).size(), static_cast<size_t>(1));

    server.ClearSent();
    EXPECT_EQ(server.GetSentTo(id).size(), static_cast<size_t>(0));
}

TEST(MockClient_InputFireFlag)
{
    MockServer server(4);
    MockClient client("Shooter");
    PerformHandshake(client, server);

    bool fireSeen = false;
    server.RegisterHandler(MessageType::ClientInput,
                           [&](const NetworkMessage& msg)
                           {
                               SimpleBuffer buf;
                               buf.data.assign(msg.payload.begin(), msg.payload.end());
                               buf.ReadUint32(); // sequence
                               buf.ReadFloat();
                               buf.ReadFloat();
                               buf.ReadFloat();
                               buf.ReadUint8(); // jump
                               buf.ReadUint8(); // crouch
                               fireSeen = (buf.ReadUint8() != 0);
                           });

    MockClient::InputState input{};
    input.fire = true;
    server.Receive(client.BuildInputMessage(input));

    EXPECT_TRUE(fireSeen);
}

TEST(MockClient_HandlerOverwrite)
{
    MockServer server(4);
    MockClient client("Alice");
    PerformHandshake(client, server);

    int firstCount = 0, secondCount = 0;

    // Register first handler
    server.RegisterHandler(MessageType::ChatMessage, [&](const NetworkMessage&) { ++firstCount; });
    server.Receive(client.BuildChatMessage("msg1"));

    // Overwrite with a second handler
    server.RegisterHandler(MessageType::ChatMessage, [&](const NetworkMessage&) { ++secondCount; });
    server.Receive(client.BuildChatMessage("msg2"));

    EXPECT_EQ(firstCount, 1);  // Only the first message hit it
    EXPECT_EQ(secondCount, 1); // Only the second message hit it
}
