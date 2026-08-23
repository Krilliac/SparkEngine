/**
 * @file NetworkConnection.cpp
 * @brief Connection lifecycle and message routing for NetworkManager
 *
 * Extracted from NetworkManager.cpp — implements Initialize, Shutdown,
 * StartServer, StopServer, Connect, Disconnect, Send*, RegisterHandler,
 * KickClient, HandleConnect, HandleDisconnect, CheckConnectionTimeouts,
 * ProcessIncoming, ProcessOutgoing, FlushOutgoingQueue, HandleRetransmissions,
 * and UpdateHeartbeat.
 */

#include "NetworkManager.h"
#include "ConnectionScopeFilter.h"
#include "DeltaSnapshotManager.h"
#include "InstabilitySimulator.h"
#include "../Security/MemoryIntegrity.h"
#include "../../Utils/Assert.h"
#include "../../Utils/DebugHookManager.h"
#include "../../Utils/Validate.h"
#include <sstream>
#include <cstring>
#include <algorithm>

#ifdef SendMessage
#undef SendMessage
#endif

using namespace DirectX;
namespace Spark::Net
{
    namespace
    {
        bool IsSameEndpoint(const sockaddr_in& lhs, const sockaddr_in& rhs) noexcept
        {
            return lhs.sin_family == rhs.sin_family && lhs.sin_port == rhs.sin_port &&
                   lhs.sin_addr.s_addr == rhs.sin_addr.s_addr;
        }
    } // namespace

    // --------------------------------------------------------------------------
    // Initialize / Shutdown
    // --------------------------------------------------------------------------

    bool NetworkManager::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        SPARK_DEBUG_HOOK_SYSTEM(SystemPreInit, "Network", 0.0);
        if (m_initialized)
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Network, "NetworkManager::Initialize — already initialized");
            return true;
        }

#ifdef ENABLE_NETWORKING
#ifdef SPARK_PLATFORM_WINDOWS
        WSADATA wsaData{};
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "WSAStartup failed with error: %d", result);
            return false;
        }
#endif // SPARK_PLATFORM_WINDOWS
#endif // ENABLE_NETWORKING

        SPARK_LOG_INFO(Spark::LogCategory::Network, "NetworkManager initialized");
        m_initialized = true;
        m_lastBandwidthSample = std::chrono::steady_clock::now();
        m_bytesSentSinceSample = 0;
        m_bytesReceivedSinceSample = 0;
        m_stats = {};

        // Register built-in message handlers
        RegisterHandler(MessageType::Connect, [this](const NetworkMessage& msg) { HandleConnect(msg); });
        RegisterHandler(MessageType::Disconnect, [this](const NetworkMessage& msg) { HandleDisconnect(msg); });
        RegisterHandler(MessageType::ConnectAccepted,
                        [this](const NetworkMessage& msg)
                        {
                            if (GetRole() == NetworkRole::Client)
                            {
                                NetBuffer buf;
                                buf.WriteBytes(msg.payload.data(), msg.payload.size());
                                const ClientID assignedID = buf.ReadUint32();
                                if (buf.HasError() || assignedID == INVALID_CLIENT)
                                {
                                    SPARK_LOG_WARN(Spark::LogCategory::Network,
                                                   "Ignoring malformed ConnectAccepted packet");
                                    return;
                                }

                                std::lock_guard<std::mutex> stateLock(m_stateMutex);
                                m_localClientID = assignedID;
                                m_connectionState = ConnectionState::Connected;

                                // Mark as connected for auto-reconnect tracking
                                m_wasConnected = true;
                                m_reconnectAttempts = 0;
                            }
                        });
        RegisterHandler(MessageType::Heartbeat,
                        [this](const NetworkMessage& msg)
                        {
                            NetworkRole role;
                            {
                                std::lock_guard<std::mutex> lock(m_stateMutex);
                                role = m_role;
                            }
                            if (role == NetworkRole::Server)
                            {
                                std::lock_guard<std::mutex> lock(m_clientsMutex);
                                auto it = m_clients.find(msg.senderID);
                                if (it != m_clients.end())
                                {
                                    it->second.lastHeartbeatTime = m_serverTime;
                                }
                            }
                        });
        RegisterHandler(MessageType::EntityStateUpdate,
                        [this](const NetworkMessage& msg)
                        {
                            NetworkRole role;
                            ClientID localID;
                            {
                                std::lock_guard<std::mutex> lock(m_stateMutex);
                                role = m_role;
                                localID = m_localClientID;
                            }
                            if (role == NetworkRole::Client)
                            {
                                NetBuffer buf;
                                buf.WriteBytes(msg.payload.data(), msg.payload.size());
                                DeserializeEntityState(buf);

                                // Delta-ack echo: unreliable state updates carry the server's
                                // per-connection delta sequence in the message header (its own
                                // sequence space — the reliable-channel sequence is only ever
                                // assigned on reliable messages, so the two cannot collide
                                // here). Echo it so the server's DeltaSnapshotManager advances
                                // this client's baseline and releases the pending delta.
                                if (msg.channel == ChannelType::Unreliable && msg.sequence > 0)
                                {
                                    NetworkMessage deltaAck;
                                    deltaAck.type = MessageType::DeltaAck;
                                    deltaAck.channel = ChannelType::Unreliable;
                                    deltaAck.senderID = localID;
                                    NetBuffer ackBuf;
                                    ackBuf.WriteUint32(msg.sequence);
                                    deltaAck.payload = ackBuf.GetData();
                                    SendMessage(deltaAck);
                                }
                            }
                        });
        RegisterHandler(MessageType::DeltaAck,
                        [this](const NetworkMessage& msg)
                        {
                            // Server-only: advance the sending client's delta baseline.
                            // senderID is trusted (stamped by ProcessIncoming from the
                            // address->client table), so one client's ack can never advance
                            // another client's baseline. Stale or duplicate sequences are
                            // ignored inside AcknowledgeSequence (latest-wins).
                            if (GetRole() != NetworkRole::Server || msg.senderID == INVALID_CLIENT)
                                return;
                            if (msg.payload.size() < sizeof(uint32_t))
                                return;

                            NetBuffer buf;
                            buf.WriteBytes(msg.payload.data(), msg.payload.size());
                            DeltaSnapshotManager::GetInstance().AcknowledgeSequence(msg.senderID, buf.ReadUint32());
                        });
        RegisterHandler(MessageType::ClientInput,
                        [this](const NetworkMessage& msg)
                        {
                            NetworkRole role;
                            {
                                std::lock_guard<std::mutex> lock(m_stateMutex);
                                role = m_role;
                            }
                            if (role == NetworkRole::Server)
                            {
                                NetBuffer buf;
                                buf.WriteBytes(msg.payload.data(), msg.payload.size());
                                ClientInputState input;
                                input.inputSequence = buf.ReadUint32();
                                input.moveForward = buf.ReadFloat();
                                input.moveRight = buf.ReadFloat();
                                input.lookYaw = buf.ReadFloat();
                                input.lookPitch = buf.ReadFloat();
                                uint8_t flags = buf.ReadUint8();
                                input.jump = (flags & 1) != 0;
                                input.fire = (flags & 2) != 0;
                                input.reload = (flags & 4) != 0;
                                input.sprint = (flags & 8) != 0;
                                input.crouch = (flags & 16) != 0;
                                input.deltaTime = buf.ReadFloat();
                                input.timestamp = m_serverTime;
                                {
                                    std::lock_guard<std::mutex> lock(m_inputMutex);
                                    m_pendingInputs.push_back(input);
                                }
                            }
                        });

        SPARK_DEBUG_HOOK_SYSTEM(SystemPostInit, "Network", 0.0);
        return true;
    }

    void NetworkManager::Shutdown()
    {
        if (!m_initialized)
            return;

        SPARK_DEBUG_HOOK_SYSTEM(SystemPreShutdown, "Network", 0.0);
        if (m_connectionState != ConnectionState::Disconnected)
        {
            Disconnect();
        }

#ifdef ENABLE_NETWORKING
        CloseSocket();

#ifdef SPARK_PLATFORM_WINDOWS
        WSACleanup();
#endif // SPARK_PLATFORM_WINDOWS

        m_clientAddresses.clear();
#endif // ENABLE_NETWORKING

        // Acquire locks in documented order:
        // m_stateMutex -> m_clientsMutex -> m_queueMutex -> m_replicationMutex -> m_inputMutex -> m_handlerMutex
        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            m_role = NetworkRole::None;
            m_connectionState = ConnectionState::Disconnected;
            m_localClientID = INVALID_CLIENT;
        }
        {
            std::lock_guard<std::mutex> clientLock(m_clientsMutex);
            m_clients.clear();
        }
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            while (!m_outgoingQueue.empty())
                m_outgoingQueue.pop();
            while (!m_incomingQueue.empty())
                m_incomingQueue.pop();
        }
        {
            std::lock_guard<std::mutex> replicationLock(m_replicationMutex);
            m_replicatedEntities.clear();
        }
        {
            std::lock_guard<std::mutex> inputLock(m_inputMutex);
            m_pendingInputs.clear();
            m_inputHistory.clear();
        }
        {
            std::lock_guard<std::mutex> lock(m_handlerMutex);
            m_handlers.clear();
        }

        m_peers.clear();
        m_lagCompensator.Clear();
        m_serverTime = 0.0f;
        m_nextClientID = 1;
        m_nextNetworkID = 1;
        m_inputSequence = 0;
        m_heartbeatTimer = 0.0f;
        m_replicationTimer = 0.0f;
        m_smoothedRTT = 0.0f;
        m_rttVariance = 0.0f;
        m_rttInitialized = false;
        m_stats = {};
        m_initialized = false;
        SPARK_DEBUG_HOOK_SYSTEM(SystemPostShutdown, "Network", 0.0);
    }

    // --------------------------------------------------------------------------
    // StartServer / StopServer
    // --------------------------------------------------------------------------

    bool NetworkManager::StartServer(uint16_t port, int maxClients)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        SPARK_REQUIRE_MSG(Spark::LogCategory::Network, port > 0, "port must be greater than 0");
        SPARK_REQUIRE_MSG(Spark::LogCategory::Network, maxClients > 0 && maxClients <= 256,
                          "maxClients must be in [1, 256]");
        SPARK_LOG_INFO(Spark::LogCategory::Network, "Starting server on port %u (maxClients=%d)", port, maxClients);
        if (!m_initialized)
        {
            if (!Initialize())
                return false;
        }

#ifdef ENABLE_NETWORKING
        if (!CreateSocket(port))
            return false;
#endif // ENABLE_NETWORKING

        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            m_role = NetworkRole::Server;
            m_connectionState = ConnectionState::Connected;
            m_localClientID = 0; // Server is client 0
        }
        m_maxClients = maxClients;
        m_serverTime = 0.0f;
        m_heartbeatTimer = 0.0f;
        m_replicationTimer = 0.0f;
        return true;
    }

    void NetworkManager::StopServer()
    {
        if (GetRole() != NetworkRole::Server)
            return;

        // Collect client IDs under lock, then notify outside the lock
        std::vector<ClientID> clientIDs;
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            clientIDs.reserve(m_clients.size());
            for (const auto& [id, info] : m_clients)
                clientIDs.push_back(id);
        }

        SPARK_LOG_INFO(Spark::LogCategory::Network, "Stopping server, notifying %zu connected clients",
                       clientIDs.size());

        // Notify all connected clients
        NetworkMessage disconnectMsg;
        disconnectMsg.type = MessageType::Disconnect;
        disconnectMsg.channel = ChannelType::Reliable;
        NetBuffer buf;
        buf.WriteString("Server shutting down");
        disconnectMsg.payload = buf.GetData();

        for (ClientID id : clientIDs)
        {
            SendToClient(id, disconnectMsg);
        }

        // Flush the outgoing queue so disconnect messages are sent
        ProcessOutgoing();

#ifdef ENABLE_NETWORKING
        CloseSocket();
        m_clientAddresses.clear();
#endif // ENABLE_NETWORKING

        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            m_clients.clear();
        }
        {
            std::lock_guard<std::mutex> lock(m_replicationMutex);
            m_replicatedEntities.clear();
        }
        m_lagCompensator.Clear();
        {
            std::lock_guard<std::mutex> lock(m_inputMutex);
            m_pendingInputs.clear();
        }
        m_peers.clear();

        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            m_role = NetworkRole::None;
            m_connectionState = ConnectionState::Disconnected;
            m_localClientID = INVALID_CLIENT;
        }
    }

    // --------------------------------------------------------------------------
    // Connect / Disconnect
    // --------------------------------------------------------------------------

    bool NetworkManager::Connect(const std::string& address, uint16_t port, const std::string& playerName)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        SPARK_REQUIRE_MSG(Spark::LogCategory::Network, !address.empty(), "address must not be empty");
        SPARK_REQUIRE_MSG(Spark::LogCategory::Network, port > 0, "port must be greater than 0");
        SPARK_REQUIRE_MSG(Spark::LogCategory::Network, !playerName.empty(), "playerName must not be empty");
        SPARK_LOG_INFO(Spark::LogCategory::Network, "Connecting to %s:%u as '%s'", address.c_str(), port,
                       playerName.c_str());

        // Save connection params for auto-reconnect
        m_lastServerAddress = address;
        m_lastServerPort = port;
        m_lastPlayerName = playerName;
        if (!m_initialized)
        {
            if (!Initialize())
                return false;
        }

#ifdef ENABLE_NETWORKING
        // Create a client socket on an ephemeral port (0)
        if (!CreateSocket(0))
            return false;

        // Resolve the server address
        std::memset(&m_serverAddress, 0, sizeof(m_serverAddress));
        m_serverAddress.sin_family = AF_INET;
        m_serverAddress.sin_port = htons(port);

        if (inet_pton(AF_INET, address.c_str(), &m_serverAddress.sin_addr) != 1)
        {
            CloseSocket();
            return false;
        }
#endif // ENABLE_NETWORKING

        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            m_role = NetworkRole::Client;
            m_connectionState = ConnectionState::Connecting;
        }

        // Send connect request
        NetworkMessage connectMsg;
        connectMsg.type = MessageType::Connect;
        connectMsg.channel = ChannelType::Reliable;
        connectMsg.senderID = INVALID_CLIENT;
        connectMsg.timestamp = 0.0f;
        NetBuffer buf;
        buf.WriteString(playerName);
        connectMsg.payload = buf.GetData();
        SendMessage(connectMsg);

        return true;
    }

    void NetworkManager::Disconnect()
    {
        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            if (m_connectionState == ConnectionState::Disconnected)
                return;
            m_connectionState = ConnectionState::Disconnecting;
        }

        NetworkMessage disconnectMsg;
        disconnectMsg.type = MessageType::Disconnect;
        disconnectMsg.channel = ChannelType::Reliable;
        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            disconnectMsg.senderID = m_localClientID;
        }
        SendMessage(disconnectMsg);

        // Flush so the disconnect message actually goes out
        ProcessOutgoing();

#ifdef ENABLE_NETWORKING
        CloseSocket();
        m_clientAddresses.clear();
#endif // ENABLE_NETWORKING

        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            m_connectionState = ConnectionState::Disconnected;
            m_role = NetworkRole::None;
            m_localClientID = INVALID_CLIENT;
        }
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            m_clients.clear();
        }
        {
            std::lock_guard<std::mutex> lock(m_replicationMutex);
            m_replicatedEntities.clear();
        }
        m_lagCompensator.Clear();
        {
            std::lock_guard<std::mutex> lock(m_inputMutex);
            m_pendingInputs.clear();
            m_inputHistory.clear();
        }
        m_peers.clear();
    }

    // --------------------------------------------------------------------------
    // Send helpers
    // --------------------------------------------------------------------------

    void NetworkManager::SendMessage(const NetworkMessage& msg)
    {
        // Server-side reliable broadcasts must fan out per client: each peer
        // has its own sequence stream and unacked map, so a single broadcast
        // sequence number cannot represent delivery to N independent peers.
        if (GetRole() == NetworkRole::Server && msg.channel != ChannelType::Unreliable)
        {
            SendToAll(msg);
            return;
        }

        std::lock_guard<std::mutex> lock(m_queueMutex);

        NetworkMessage queued = msg;
        queued.timestamp = m_serverTime;

        // Back-pressure only on Unreliable. Dropping a ReliableOrdered message
        // after sequence assignment would create a permanent gap the receiver
        // cannot recover from (later sequences buffer forever waiting for it).
        if (queued.channel == ChannelType::Unreliable && m_outgoingQueue.size() >= kMaxQueuedMessages)
        {
            m_droppedOutgoingMessages.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Assign sequence number for reliable messages (after the drop check so we
        // never burn a sequence number on a dropped packet). Only the client path
        // reaches here for reliable traffic, so the peer is always the server.
        if (queued.channel != ChannelType::Unreliable)
        {
            queued.sequence = GetPeerState(SERVER_PEER).nextOutgoingSequence++;
        }

        m_outgoingQueue.push(queued);
        m_stats.packetsSent++;
    }

    void NetworkManager::SendToClient(ClientID client, const NetworkMessage& msg)
    {
        ASSERT_MSG(client != INVALID_CLIENT, "NetworkManager::SendToClient — client ID must not be INVALID_CLIENT");
        if (m_role != NetworkRole::Server)
            return;

        NetworkMessage copy = msg;
        copy.senderID = 0; // From server
        copy.timestamp = m_serverTime;

#ifdef ENABLE_NETWORKING
        auto addrIt = m_clientAddresses.find(client);
        if (addrIt == m_clientAddresses.end())
            return;

        // Assign this client's own reliable sequence and track for
        // retransmission — sequence streams and unacked maps are per peer, so
        // an ACK from one client can never erase another client's messages.
        if (copy.channel != ChannelType::Unreliable)
        {
            PeerState& peer = GetPeerState(client);
            copy.sequence = peer.nextOutgoingSequence++;
            peer.unacknowledgedMessages[copy.sequence] = copy;
            peer.reliableOriginalSendTime.try_emplace(copy.sequence, m_serverTime);
        }

        auto serialized = SerializeMessage(copy);
        SendRawTo(serialized, addrIt->second);
        m_stats.packetsSent++;
#else
        // Without networking, just enqueue for local testing
        std::lock_guard<std::mutex> lock(m_queueMutex);
        // Only drop Unreliable messages to avoid reliable-sequence gaps.
        if (copy.channel == ChannelType::Unreliable && m_outgoingQueue.size() >= kMaxQueuedMessages)
        {
            m_droppedOutgoingMessages.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (copy.channel != ChannelType::Unreliable)
        {
            copy.sequence = GetPeerState(client).nextOutgoingSequence++;
        }
        m_outgoingQueue.push(copy);
        m_stats.packetsSent++;
#endif // ENABLE_NETWORKING
    }

    std::vector<ClientID> NetworkManager::GetConnectedClientIDs() const
    {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        std::vector<ClientID> clientIDs;
        clientIDs.reserve(m_clients.size());
        for (const auto& [id, info] : m_clients)
            clientIDs.push_back(id);
        return clientIDs;
    }

    void NetworkManager::SendToAll(const NetworkMessage& msg)
    {
        for (ClientID id : GetConnectedClientIDs())
            SendToClient(id, msg);
    }

    void NetworkManager::SendToAllExcept(ClientID excludeClient, const NetworkMessage& msg)
    {
        for (ClientID id : GetConnectedClientIDs())
        {
            if (id != excludeClient)
                SendToClient(id, msg);
        }
    }

    void NetworkManager::BroadcastMessage(const NetworkMessage& msg)
    {
        SendToAll(msg);
    }

    void NetworkManager::RegisterHandler(MessageType type, MessageHandler handler)
    {
        ASSERT_MSG(handler != nullptr, "NetworkManager::RegisterHandler — handler must not be null");
        std::lock_guard<std::mutex> lock(m_handlerMutex);
        m_handlers[static_cast<uint16_t>(type)] = std::move(handler);
    }

    void NetworkManager::ClearHandlers()
    {
        std::lock_guard<std::mutex> lock(m_handlerMutex);
        m_handlers.clear();
    }

    // --------------------------------------------------------------------------
    // KickClient
    // --------------------------------------------------------------------------

    void NetworkManager::KickClient(ClientID client, const std::string& reason)
    {
        ASSERT_MSG(client != INVALID_CLIENT, "NetworkManager::KickClient — client ID must not be INVALID_CLIENT");
        SPARK_LOG_INFO(Spark::LogCategory::Network, "Kicking client %u: %s", client, reason.c_str());

        std::lock_guard<std::mutex> lock(m_clientsMutex);
        auto it = m_clients.find(client);
        if (it == m_clients.end())
            return;

        NetworkMessage msg;
        msg.type = MessageType::Disconnect;
        msg.channel = ChannelType::Reliable;
        NetBuffer buf;
        buf.WriteString(reason);
        msg.payload = buf.GetData();
        SendToClient(client, msg);

        m_clients.erase(it);
#ifdef ENABLE_NETWORKING
        m_clientAddresses.erase(client);
#endif
        // Drop the peer's reliability state so a future client reusing the ID
        // starts with fresh sequence/dedup/ordered bookkeeping.
        m_peers.erase(client);
        // Drop any interest-management scope tied to this connection so
        // a future client reusing the ID starts with "see everything".
        ConnectionScopeFilter::GetInstance().RemoveConnection(static_cast<uint32_t>(client));
    }

    // --------------------------------------------------------------------------
    // HandleConnect / HandleDisconnect
    // --------------------------------------------------------------------------

    ClientID NetworkManager::HandleConnect(const NetworkMessage& msg)
    {
        if (GetRole() != NetworkRole::Server)
            return INVALID_CLIENT;

        // ProcessIncoming pre-registers m_clientAddresses[m_nextClientID] so
        // that SendToClient can reach the new client. If we reject, we must
        // clean up that entry to avoid stale addresses receiving broadcasts.
        ClientID pendingID = m_nextClientID;

        if (static_cast<int>(m_clients.size()) >= m_maxClients)
        {
            NetworkMessage reject;
            reject.type = MessageType::ConnectRejected;
            reject.channel = ChannelType::Reliable;
            NetBuffer rejectBuf;
            rejectBuf.WriteString("Server full");
            reject.payload = rejectBuf.GetData();

            // Send rejection only to the connecting client (not broadcast)
            SPARK_LOG_WARN(Spark::LogCategory::Network,
                           "Connection rejected for pending client %u: server full (%d/%d)", pendingID,
                           static_cast<int>(m_clients.size()), m_maxClients);
            SendToClient(pendingID, reject);

#ifdef ENABLE_NETWORKING
            // Clean up the pre-registered address so the rejected client
            // doesn't receive broadcast traffic meant for real clients
            m_clientAddresses.erase(pendingID);
#endif
            return INVALID_CLIENT;
        }

        ClientID newID = m_nextClientID++;
        ClientInfo info;
        info.id = newID;
        info.state = ConnectionState::Connected;
        info.lastHeartbeatTime = m_serverTime;

        // Parse player name from connection request payload
        if (!msg.payload.empty())
        {
            NetBuffer buf;
            buf.WriteBytes(msg.payload.data(), msg.payload.size());
            info.name = buf.ReadString();
        }
        if (info.name.empty())
        {
            info.name = "Player_" + std::to_string(newID);
        }
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            m_clients[newID] = info;
        }

        // Register the new connection for delta snapshot tracking
        DeltaSnapshotManager::GetInstance().RegisterConnection(newID);

        // Send acceptance with assigned client ID
        NetworkMessage accept;
        accept.type = MessageType::ConnectAccepted;
        accept.channel = ChannelType::Reliable;
        NetBuffer respBuf;
        respBuf.WriteUint32(newID);
        respBuf.WriteFloat(m_serverTime);
        accept.payload = respBuf.GetData();
        SendToClient(newID, accept);
        SPARK_LOG_INFO(Spark::LogCategory::Network, "Client %u accepted ('%s'), %d/%d slots used", newID,
                       info.name.c_str(), static_cast<int>(m_clients.size()), m_maxClients);
        return newID;
    }

    void NetworkManager::HandleDisconnect(const NetworkMessage& msg)
    {
        ClientID clientID = msg.senderID;
        SPARK_LOG_INFO(Spark::LogCategory::Network, "Client %u disconnecting", clientID);

        // Unregister from delta snapshot tracking before removing the client
        DeltaSnapshotManager::GetInstance().UnregisterConnection(clientID);

        // Drop interest-management scope so a future connection reusing this
        // ID starts fresh instead of inheriting the prior player's visibility.
        ConnectionScopeFilter::GetInstance().RemoveConnection(static_cast<uint32_t>(clientID));

        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            m_clients.erase(clientID);
#ifdef ENABLE_NETWORKING
            m_clientAddresses.erase(clientID);
#endif
        }

        // Drop the peer's reliability state (sequence streams, unacked maps,
        // dedup window) so a reused ClientID starts fresh.
        m_peers.erase(clientID);

        // Remove entities owned by this client
        SPARK_LOG_DEBUG(Spark::LogCategory::Network, "Cleaning up entities owned by client %u", clientID);
        std::vector<uint32_t> ownedEntities;
        {
            std::lock_guard<std::mutex> replicationLock(m_replicationMutex);
            for (const auto& [netID, entity] : m_replicatedEntities)
            {
                if (entity.ownerID == clientID)
                {
                    ownedEntities.push_back(netID);
                }
            }
        }
        for (uint32_t netID : ownedEntities)
        {
            UnregisterReplicatedEntity(netID);
        }
        if (!ownedEntities.empty())
        {
            SPARK_LOG_INFO(Spark::LogCategory::Network, "Removed %zu entities owned by disconnected client %u",
                           ownedEntities.size(), clientID);
        }
    }

    // --------------------------------------------------------------------------
    // CheckConnectionTimeouts
    // --------------------------------------------------------------------------

    void NetworkManager::CheckConnectionTimeouts()
    {
        if (m_role == NetworkRole::Server)
        {
            std::vector<ClientID> timedOut;
            {
                std::lock_guard<std::mutex> lock(m_clientsMutex);
                for (const auto& [id, info] : m_clients)
                {
                    if (m_serverTime - info.lastHeartbeatTime > m_connectionTimeout)
                    {
                        timedOut.push_back(id);
                    }
                }
            }

            for (ClientID id : timedOut)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Network, "Client %u timed out (no heartbeat for %.1fs)", id,
                               m_connectionTimeout);

                if (m_timeoutHandler)
                    m_timeoutHandler(id);

                // Clean up owned entities
                std::vector<uint32_t> ownedEntities;
                {
                    std::lock_guard<std::mutex> replicationLock(m_replicationMutex);
                    for (const auto& [netID, entity] : m_replicatedEntities)
                    {
                        if (entity.ownerID == id)
                            ownedEntities.push_back(netID);
                    }
                }
                for (uint32_t netID : ownedEntities)
                    UnregisterReplicatedEntity(netID);

                {
                    std::lock_guard<std::mutex> lock(m_clientsMutex);
                    m_clients.erase(id);
#ifdef ENABLE_NETWORKING
                    m_clientAddresses.erase(id);
#endif
                }

                // Drop the timed-out peer's reliability state.
                m_peers.erase(id);

                // Drop interest-management scope for timed-out clients.
                ConnectionScopeFilter::GetInstance().RemoveConnection(static_cast<uint32_t>(id));
            }
        }
        else if (m_role == NetworkRole::Client)
        {
            // Client-side: detect server timeout (no heartbeat received)
            // The client tracks server liveness via m_serverTime advancing and
            // heartbeat responses. If no messages arrive for connectionTimeout,
            // the connection is considered lost.
            // (Server heartbeat responses update m_serverTime via Update())
        }
    }

    // --------------------------------------------------------------------------
    // ProcessIncoming -- read from socket, parse, enqueue
    // --------------------------------------------------------------------------

    void NetworkManager::ProcessIncoming()
    {
#ifdef ENABLE_NETWORKING
        if (m_socket == INVALID_SOCKET)
            return;

        // Track newly connected clients for deferred entity sync
        std::vector<ClientID> newlyConnected;

        // Read all available datagrams
        for (int i = 0; i < 256; ++i) // Cap per-frame reads
        {
            std::vector<uint8_t> rawData;
            sockaddr_in senderAddr{};
            int received = ReceiveRaw(rawData, senderAddr);
            if (received <= 0)
                break;

            const NetworkRole role = GetRole();
            if (role == NetworkRole::Client && !IsSameEndpoint(senderAddr, m_serverAddress))
            {
                SPARK_LOG_DEBUG(Spark::LogCategory::Network, "Dropping packet from unexpected server endpoint");
                continue;
            }

            NetworkMessage msg;
            if (!DeserializeMessage(rawData.data(), rawData.size(), msg))
                continue;

            ClientID trustedSender = INVALID_CLIENT;
            if (role == NetworkRole::Server)
            {
                for (const auto& [id, addr] : m_clientAddresses)
                {
                    if (IsSameEndpoint(addr, senderAddr))
                    {
                        trustedSender = id;
                        break;
                    }
                }
                msg.senderID = trustedSender;
            }

            // Validate the packet against registered schemas — bypassing
            // allows malicious packets to reach game logic unvalidated
            SPARK_BRANCH_GUARD_BEGIN("network_packet_gateway")
            {
                // NEVER derive auth from the wire-supplied senderID: the sender fully
                // controls that field on packets it transmits, so trusting it lets a
                // client stamp any non-zero value and bypass every requiresAuth schema
                // server-side. It also collides with SendToClient's senderID=0 stamp
                // (0 == INVALID_CLIENT) for server-sent messages, which made every
                // server->client message read as unauthenticated on the client and
                // silently dropped all requiresAuth replication traffic. Authenticate
                // from OUR OWN trusted state instead: the address->client table on the
                // server, live connection state on the client.
                bool isAuthenticated;
                if (role == NetworkRole::Server)
                    isAuthenticated = (trustedSender != INVALID_CLIENT);
                else
                    isAuthenticated = (GetConnectionState() == ConnectionState::Connected);
                bool isFromClient = (role == NetworkRole::Server);
                auto validation = m_packetValidator.ValidatePacket(msg, isAuthenticated, isFromClient);
                if (!validation.valid)
                {
                    SPARK_LOG_DEBUG(Spark::LogCategory::Network, "Packet rejected: %s", validation.reason.c_str());
                    continue;
                }
            }
            SPARK_BRANCH_GUARD_END("network_packet_gateway")

            // On the server, map sender address to a client ID
            if (role == NetworkRole::Server)
            {
                // Check if this is a new connection
                if (msg.type == MessageType::Connect)
                {
                    msg.senderID = INVALID_CLIENT;

                    // Duplicate-address detection: if this address already has
                    // an active connection, ignore the redundant Connect to
                    // prevent slot exhaustion from repeated packets.
                    if (trustedSender != INVALID_CLIENT)
                    {
                        SPARK_LOG_DEBUG(Spark::LogCategory::Network,
                                        "Ignoring duplicate Connect from already-connected address");
                        continue;
                    }

                    // Pre-register the client address so HandleConnect's
                    // SendToClient(ConnectAccepted) can reach the new client.
                    const ClientID pendingID = m_nextClientID;
                    m_clientAddresses[pendingID] = senderAddr;

                    const ClientID admittedID = HandleConnect(msg);
                    if (admittedID != INVALID_CLIENT)
                        newlyConnected.push_back(admittedID);
                    continue;
                }

                if (trustedSender == INVALID_CLIENT)
                {
                    SPARK_LOG_DEBUG(Spark::LogCategory::Network, "Dropping packet from unknown client endpoint");
                    continue;
                }
            }

            std::lock_guard<std::mutex> lock(m_queueMutex);
            // Unreliable can be dropped under flood; reliable/ordered must be kept
            // so the ack/resequence path stays consistent.
            if (msg.channel == ChannelType::Unreliable && m_incomingQueue.size() >= kMaxQueuedMessages)
            {
                m_droppedIncomingMessages.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                m_incomingQueue.push(msg);
            }
        }

        // Send full entity state to newly connected clients (deferred to
        // avoid blocking the receive loop with mutex acquisitions / sends)
        for (ClientID id : newlyConnected)
        {
            SendFullEntitySync(id);
        }

#else
        // Without networking, just dispatch whatever is in the queue already
        // (for local/testing scenarios)
#endif // ENABLE_NETWORKING
    }

    // --------------------------------------------------------------------------
    // ProcessOutgoing -- serialize and send queued messages
    // --------------------------------------------------------------------------

    void NetworkManager::ProcessOutgoing()
    {
        FlushOutgoingQueue();
        HandleRetransmissions();
    }

    void NetworkManager::FlushOutgoingQueue()
    {
        std::queue<NetworkMessage> toSend;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            std::swap(toSend, m_outgoingQueue);
        }

#ifdef ENABLE_NETWORKING
        auto& instability = InstabilitySimulator::GetInstance();
        float currentTimeMs = m_serverTime * 1000.0f;

        // Reliable messages only reach this queue on the client path (server
        // reliable sends fan out through SendToClient), so they are tracked
        // against the single implicit server peer.
        auto trackReliable = [this](const NetworkMessage& reliableMsg)
        {
            if (reliableMsg.channel != ChannelType::Unreliable && reliableMsg.sequence > 0)
            {
                PeerState& peer = GetPeerState(SERVER_PEER);
                peer.unacknowledgedMessages[reliableMsg.sequence] = reliableMsg;
                peer.reliableOriginalSendTime.try_emplace(reliableMsg.sequence, m_serverTime);
            }
        };

        // First, flush any delayed packets that are now ready for transmission
        if (instability.GetSettings().enabled)
        {
            auto readyPackets = instability.GetReadyPackets(currentTimeMs);
            for (auto& pktData : readyPackets)
            {
                if (m_role == NetworkRole::Client)
                {
                    SendRawTo(pktData, m_serverAddress);
                }
                else if (m_role == NetworkRole::Server)
                {
                    for (const auto& [id, addr] : m_clientAddresses)
                    {
                        SendRawTo(pktData, addr);
                    }
                }
            }
        }

        while (!toSend.empty())
        {
            const auto& msg = toSend.front();
            auto serialized = SerializeMessage(msg);

            // Apply instability simulation if enabled
            if (instability.GetSettings().enabled)
            {
                // Simulated packet loss
                if (instability.ShouldDropPacket())
                {
                    m_stats.packetsDropped++;
                    // Track reliable messages even if "dropped" (for retransmission)
                    trackReliable(msg);
                    toSend.pop();
                    continue;
                }

                // Simulated latency + jitter: queue for delayed delivery
                float delayMs = instability.GetDelayMs();
                if (delayMs > 0.0f)
                {
                    instability.QueuePacket(serialized, currentTimeMs + delayMs);

                    // Track reliable messages for retransmission
                    trackReliable(msg);
                    toSend.pop();
                    continue;
                }
            }

            // No instability or zero delay — send immediately
            if (m_role == NetworkRole::Client)
            {
                SendRawTo(serialized, m_serverAddress);
            }
            else if (m_role == NetworkRole::Server)
            {
                for (const auto& [id, addr] : m_clientAddresses)
                {
                    SendRawTo(serialized, addr);
                }
            }

            // Track reliable messages for retransmission
            trackReliable(msg);

            toSend.pop();
        }
#else
        // Without networking, just discard
        while (!toSend.empty())
        {
            toSend.pop();
        }
#endif // ENABLE_NETWORKING
    }

    void NetworkManager::HandleRetransmissions()
    {
#ifdef ENABLE_NETWORKING
        // Retransmit each peer's unacknowledged reliable messages with
        // exponential backoff — unicast to the owning peer only, never a
        // broadcast (another client's ACK must not silence a retransmission,
        // and other clients must not receive foreign sequences).
        const NetworkRole role = GetRole();
        for (auto& [peerKey, peer] : m_peers)
        {
            if (peer.unacknowledgedMessages.empty())
                continue;

            // Resolve this peer's destination address
            const sockaddr_in* destination = nullptr;
            if (role == NetworkRole::Client)
            {
                destination = &m_serverAddress;
            }
            else
            {
                auto addrIt = m_clientAddresses.find(peerKey);
                if (addrIt != m_clientAddresses.end())
                    destination = &addrIt->second;
            }
            if (!destination)
            {
                // Peer has no address (disconnected or never addressable) —
                // drop its outgoing state instead of retransmitting forever.
                peer.unacknowledgedMessages.clear();
                peer.reliableOriginalSendTime.clear();
                peer.retransmitCounts.clear();
                continue;
            }

            std::vector<SequenceNumber> toRetransmit;
            for (auto& [seq, unacked] : peer.unacknowledgedMessages)
            {
                float age = m_serverTime - unacked.timestamp;
                int retryCount = 0;
                auto rcIt = peer.retransmitCounts.find(seq);
                if (rcIt != peer.retransmitCounts.end())
                    retryCount = rcIt->second;

                // Exponential backoff: base * 2^retryCount, capped at 8x base
                float backoff = m_reliableRetransmitInterval * static_cast<float>(1 << (std::min)(retryCount, 3));
                if (age > backoff)
                {
                    toRetransmit.push_back(seq);
                }
            }

            for (SequenceNumber seq : toRetransmit)
            {
                auto it = peer.unacknowledgedMessages.find(seq);
                if (it == peer.unacknowledgedMessages.end())
                    continue;

                // Increment retry count for exponential backoff
                int& retryCount = peer.retransmitCounts[seq];
                retryCount++;

                // Drop if max retries exceeded
                if (retryCount > m_maxReliableRetries)
                {
                    peer.unacknowledgedMessages.erase(it);
                    peer.reliableOriginalSendTime.erase(seq);
                    peer.retransmitCounts.erase(seq);
                    m_stats.packetsDropped++;
                    continue;
                }

                auto& retransmitMsg = it->second;
                retransmitMsg.timestamp = m_serverTime; // Reset retransmit timer
                auto serialized = SerializeMessage(retransmitMsg);
                SendRawTo(serialized, *destination);
            }
        }
#endif // ENABLE_NETWORKING
    }

    // --------------------------------------------------------------------------
    // UpdateHeartbeat
    // --------------------------------------------------------------------------

    void NetworkManager::UpdateHeartbeat(float deltaTime)
    {
        m_heartbeatTimer += deltaTime;
        if (m_heartbeatTimer >= m_heartbeatInterval)
        {
            m_heartbeatTimer = 0.0f;

            NetworkMessage heartbeat;
            heartbeat.type = MessageType::Heartbeat;
            heartbeat.channel = ChannelType::Unreliable;
            heartbeat.senderID = m_localClientID;
            heartbeat.timestamp = m_serverTime;
            SendMessage(heartbeat);
        }
    }

    // --------------------------------------------------------------------------
    // Console commands
    // --------------------------------------------------------------------------

    std::string NetworkManager::Console_GetStatus() const
    {
        std::ostringstream ss;
        ss << "=== Network Status ===\n";
        ss << "Initialized: " << (m_initialized ? "Yes" : "No") << "\n";
        ss << "Role: ";
        switch (m_role)
        {
        case NetworkRole::None:
            ss << "None";
            break;
        case NetworkRole::Server:
            ss << "Server";
            break;
        case NetworkRole::Client:
            ss << "Client";
            break;
        }
        ss << "\nState: ";
        switch (m_connectionState)
        {
        case ConnectionState::Disconnected:
            ss << "Disconnected";
            break;
        case ConnectionState::Connecting:
            ss << "Connecting";
            break;
        case ConnectionState::Connected:
            ss << "Connected";
            break;
        case ConnectionState::Disconnecting:
            ss << "Disconnecting";
            break;
        }
        ss << "\nServer Time: " << m_serverTime << "s\n";
        ss << "Replicated Entities: " << m_replicatedEntities.size() << "\n";
        if (m_role == NetworkRole::Server)
            ss << "Connected Clients: " << m_clients.size() << "/" << m_maxClients << "\n";
        if (m_role == NetworkRole::Client)
            ss << "Local Client ID: " << m_localClientID << "\n";
        return ss.str();
    }

    std::string NetworkManager::Console_ListClients() const
    {
        std::ostringstream ss;
        ss << "=== Connected Clients (" << m_clients.size() << ") ===\n";
        for (const auto& [id, info] : m_clients)
        {
            ss << "  Client " << id << ": " << info.name << " [Ping: " << info.stats.ping << "ms]"
               << " [Last heartbeat: " << info.lastHeartbeatTime << "s]\n";
        }
        return ss.str();
    }

    std::string NetworkManager::Console_GetStats() const
    {
        std::ostringstream ss;
        ss << "=== Network Stats ===\n";
        ss << "Ping: " << m_stats.ping << "ms (jitter: " << m_stats.jitter << "ms)\n";
        ss << "Packet Loss: " << (m_stats.packetLoss * 100.0f) << "%\n";
        ss << "Bandwidth: Up " << m_stats.bandwidthUp << " KB/s, Down " << m_stats.bandwidthDown << " KB/s\n";
        ss << "Packets: Sent " << m_stats.packetsSent << ", Received " << m_stats.packetsReceived << ", Dropped "
           << m_stats.packetsDropped << "\n";
        ss << "Prediction Corrections: " << m_stats.correctionCount << "\n";
        ss << "Bytes: Sent " << m_stats.bytesSent << ", Received " << m_stats.bytesReceived << "\n";
        size_t unackedTotal = 0;
        for (const auto& [peerKey, peer] : m_peers)
            unackedTotal += peer.unacknowledgedMessages.size();
        ss << "Unacked reliable messages: " << unackedTotal << "\n";
        return ss.str();
    }

} // namespace Spark::Net
