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
#include "NetworkBindPolicy.h"
#include "../Security/MemoryIntegrity.h"
#include "../../Utils/Assert.h"
#include "../../Utils/DebugHookManager.h"
#include "../../Utils/ScopeGuard.h"
#include "../../Utils/SecureMemory.h"
#include "../../Utils/Validate.h"
#include <format>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <utility>
#include <vector>

#ifdef SendMessage
#undef SendMessage
#endif

using namespace DirectX;
namespace Spark::Net
{
#ifdef ENABLE_NETWORKING
    namespace
    {
        bool IsSameEndpoint(const sockaddr_in& lhs, const sockaddr_in& rhs) noexcept
        {
            return lhs.sin_family == rhs.sin_family && lhs.sin_port == rhs.sin_port &&
                   lhs.sin_addr.s_addr == rhs.sin_addr.s_addr;
        }

        bool IsLoopbackEndpoint(const sockaddr_in& address) noexcept
        {
            return address.sin_family == AF_INET && (ntohl(address.sin_addr.s_addr) & 0xFF000000u) == 0x7F000000u;
        }
    } // namespace
#endif // ENABLE_NETWORKING

    // --------------------------------------------------------------------------
    // Initialize / Shutdown
    // --------------------------------------------------------------------------

    bool NetworkManager::Initialize()
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
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
        ++m_lifecycleEpoch;
        m_lastBandwidthSample = std::chrono::steady_clock::now();
        m_bytesSentSinceSample = 0;
        m_bytesReceivedSinceSample = 0;
        m_stats = {};

        // Mandatory protocol handlers are intentionally separate from application
        // observers. RegisterHandler/ClearHandlers must never replace or erase
        // transport lifecycle invariants.
        auto registerInternal = [this](MessageType type, MessageHandler handler)
        { m_internalHandlers[static_cast<uint16_t>(type)] = std::move(handler); };
        registerInternal(MessageType::Disconnect, [this](const NetworkMessage& msg) { HandleDisconnect(msg); });
        registerInternal(MessageType::ConnectAccepted,
                         [this](const NetworkMessage& msg)
                         {
                             if (GetRole() == NetworkRole::Client)
                             {
                                 NetBuffer buf;
                                 buf.WriteBytes(msg.payload.data(), msg.payload.size());
                                 const ClientID assignedID = buf.ReadUint32();
                                 buf.ReadFloat(); // server time; informational only
                                 const uint16_t serverVersion = buf.ReadUint16();
                                 if (buf.HasError() || assignedID == INVALID_CLIENT)
                                 {
                                     SPARK_LOG_WARN(Spark::LogCategory::Network,
                                                    "Ignoring malformed ConnectAccepted packet");
                                     return;
                                 }

                                 // The server must echo the version this client offered. Anything
                                 // else means the peers disagree about the wire format, so refuse the
                                 // session instead of exchanging traffic neither side can trust.
                                 if (serverVersion != NETWORK_PROTOCOL_VERSION)
                                 {
                                     AbandonClientHandshake(ConnectRejectReason::ProtocolMismatch,
                                                            std::format("Server accepted with protocol version {}, "
                                                                        "client requires {}",
                                                                        serverVersion, NETWORK_PROTOCOL_VERSION));
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
        registerInternal(MessageType::ConnectRejected,
                         [this](const NetworkMessage& msg)
                         {
                             // Payload: reason text, then a typed trailer (reason code + the
                             // server's protocol version). Any rejection is terminal, so a missing
                             // or malformed trailer still fails the handshake, just untyped.
                             std::string text = "Connection rejected";
                             ConnectRejectReason reason = ConnectRejectReason::Unspecified;
                             if (!msg.payload.empty())
                             {
                                 NetBuffer buf;
                                 buf.WriteBytes(msg.payload.data(), msg.payload.size());
                                 std::string suppliedText = buf.ReadString();
                                 if (!buf.HasError() && !suppliedText.empty())
                                     text = std::move(suppliedText);
                                 const uint8_t code = buf.ReadUint8();
                                 const uint16_t serverVersion = buf.ReadUint16();
                                 if (!buf.HasError() &&
                                     code <= static_cast<uint8_t>(ConnectRejectReason::MalformedHandshake))
                                 {
                                     reason = static_cast<ConnectRejectReason>(code);
                                     SPARK_LOG_DEBUG(Spark::LogCategory::Network,
                                                     "ConnectRejected reason %u from server protocol version %u",
                                                     static_cast<unsigned>(code), static_cast<unsigned>(serverVersion));
                                 }
                             }
                             AbandonClientHandshake(reason, std::move(text));
                         });
        registerInternal(MessageType::Heartbeat,
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
        registerInternal(MessageType::EntityStateUpdate,
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
                                 if (GetRole() != NetworkRole::Client)
                                     return;

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
        registerInternal(MessageType::DeltaAck,
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
        // ClientInput deliberately has no protocol handler. The transport used to
        // parse every datagram into an unbounded, unattributed, never-drained
        // server queue (a remote memory-exhaustion sink with no consumer). Input
        // is gameplay data: an application observer owns its validation, its
        // per-client attribution (msg.senderID) and its bounds.

        SPARK_DEBUG_HOOK_SYSTEM(SystemPostInit, "Network", 0.0);
        return true;
    }

    void NetworkManager::Shutdown()
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        if (!m_initialized)
            return;
        ++m_lifecycleEpoch;

        SPARK_DEBUG_HOOK_SYSTEM(SystemPreShutdown, "Network", 0.0);
        if (m_connectionState != ConnectionState::Disconnected)
        {
            Disconnect();
        }
        InstabilitySimulator::GetInstance().DiscardPacketsThroughLifecycle(m_lifecycleEpoch);

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
        ReleaseAllClientConnectionState();
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
            ++m_replicationMutationEpoch;
        }
        {
            std::lock_guard<std::mutex> inputLock(m_inputMutex);
            m_inputHistory.clear();
        }
        {
            std::lock_guard<std::mutex> lock(m_handlerMutex);
            m_internalHandlers.clear();
            m_handlers.clear();
            m_sensitiveMessageTypes.clear();
            m_handlerOwners.clear();
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
        m_allowLanAdvertisement = false;
        m_initialized = false;
        SPARK_DEBUG_HOOK_SYSTEM(SystemPostShutdown, "Network", 0.0);
    }

    // --------------------------------------------------------------------------
    // StartServer / StopServer
    // --------------------------------------------------------------------------

    bool NetworkManager::StartServer(uint16_t port, int maxClients)
    {
        return StartServer(port, maxClients, CaptureNetworkEndpointPolicy(), false);
    }

    bool NetworkManager::StartServer(uint16_t port, int maxClients, const NetworkEndpointPolicy& endpointPolicy)
    {
        return StartServer(port, maxClients, endpointPolicy, false);
    }

    bool NetworkManager::StartServer(uint16_t port, int maxClients, const NetworkEndpointPolicy& endpointPolicy,
                                     bool allowLanAdvertisement)
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        // Port 0 requests an OS-assigned ephemeral port and is the only
        // conflict-free choice for parallel tests/tools.
        SPARK_REQUIRE_MSG(Spark::LogCategory::Network, maxClients > 0 && maxClients <= 256,
                          "maxClients must be in [1, 256]");
        if (!IsEndpointLifecycleIdle())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network,
                            "Refusing server startup while another endpoint lifecycle is active");
            return false;
        }
        if (!endpointPolicy.IsValid())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "Refusing server startup: %s",
                            NetworkEndpointPolicyErrorText(endpointPolicy.Error()).data());
            return false;
        }
        SPARK_LOG_INFO(Spark::LogCategory::Network, "Starting server on %s:%u (maxClients=%d)",
                       FormatIPv4Address(endpointPolicy.BindAddress()).c_str(), port, maxClients);
        if (!m_initialized)
        {
            if (!Initialize())
                return false;
        }

#ifdef ENABLE_NETWORKING
        if (!CreateSocket(port, endpointPolicy))
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
        m_allowLanAdvertisement = allowLanAdvertisement;
        ++m_lifecycleEpoch;
        return true;
    }

    void NetworkManager::StopServer()
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        if (GetRole() != NetworkRole::Server)
            return;
        ++m_lifecycleEpoch;

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
        // ProcessOutgoing may have handed serialized datagrams to the global
        // instability simulator. Securely erase only completed manager
        // lifecycles; generic simulator users and later lifecycles remain.
        InstabilitySimulator::GetInstance().DiscardPacketsThroughLifecycle(m_lifecycleEpoch);

#ifdef ENABLE_NETWORKING
        CloseSocket();
        m_clientAddresses.clear();
#endif // ENABLE_NETWORKING

        ReleaseAllClientConnectionState();
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            m_clients.clear();
        }
        {
            std::lock_guard<std::mutex> lock(m_replicationMutex);
            m_replicatedEntities.clear();
            ++m_replicationMutationEpoch;
        }
        m_lagCompensator.Clear();
        m_peers.clear();
        m_allowLanAdvertisement = false;

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

    void NetworkManager::DiscardClientLifecycleTraffic(uint64_t lifecycleEpoch)
    {
        // The API mutex serializes lifecycle changes, but queue ownership has
        // its own lock. Swap first and let the temporary queues destroy their
        // messages after releasing m_queueMutex so sensitive payload wiping
        // never nests another subsystem's lock under the queue lock.
        std::queue<NetworkMessage> discardedOutgoing;
        std::queue<NetworkMessage> discardedIncoming;
        {
            std::lock_guard<std::mutex> queueLock(m_queueMutex);
            std::swap(discardedOutgoing, m_outgoingQueue);
            std::swap(discardedIncoming, m_incomingQueue);
        }

        // A client has one remote peer. Clearing its reliability state destroys
        // sensitive unacknowledged and ordered-buffer copies from this lifecycle.
        m_peers.clear();

        // The simulator is process-global, so discard only manager-owned packets
        // from completed lifecycles and preserve epoch-zero generic traffic.
        InstabilitySimulator::GetInstance().DiscardPacketsThroughLifecycle(lifecycleEpoch);
    }

    bool NetworkManager::Connect(const std::string& address, uint16_t port, const std::string& playerName)
    {
        return Connect(address, port, playerName, CaptureNetworkEndpointPolicy());
    }

    bool NetworkManager::Connect(const std::string& address, uint16_t port, const std::string& playerName,
                                 const NetworkEndpointPolicy& endpointPolicy)
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        SPARK_REQUIRE_MSG(Spark::LogCategory::Network, !address.empty(), "address must not be empty");
        SPARK_REQUIRE_MSG(Spark::LogCategory::Network, port > 0, "port must be greater than 0");
        SPARK_REQUIRE_MSG(Spark::LogCategory::Network, !playerName.empty(), "playerName must not be empty");
        if (!IsEndpointLifecycleIdle())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network,
                            "Refusing client startup while another endpoint lifecycle is active");
            return false;
        }
        if (!endpointPolicy.IsValid())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "Refusing client startup: %s",
                            NetworkEndpointPolicyErrorText(endpointPolicy.Error()).data());
            return false;
        }
        uint32_t serverAddress = 0;
        if (!ParseIPv4Address(address, serverAddress) || !endpointPolicy.AllowsPeerAddress(serverAddress))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "Refusing destination %s outside the captured endpoint policy",
                            address.c_str());
            return false;
        }
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
        if (!CreateSocket(0, endpointPolicy))
            return false;

        // Resolve the server address
        std::memset(&m_serverAddress, 0, sizeof(m_serverAddress));
        m_serverAddress.sin_family = AF_INET;
        m_serverAddress.sin_port = htons(port);
        m_serverAddress.sin_addr.s_addr = htonl(serverAddress);
#endif // ENABLE_NETWORKING

        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            m_role = NetworkRole::Client;
            m_connectionState = ConnectionState::Connecting;
            m_lastConnectionError.clear();
            m_lastConnectRejectReason = ConnectRejectReason::Unspecified;
        }
        m_lastServerPacketTime = m_serverTime;
        m_allowLanAdvertisement = false;
        ++m_lifecycleEpoch;

        // Send connect request
        NetworkMessage connectMsg;
        connectMsg.type = MessageType::Connect;
        connectMsg.channel = ChannelType::Reliable;
        connectMsg.senderID = INVALID_CLIENT;
        connectMsg.timestamp = 0.0f;
        NetBuffer buf;
        WriteConnectRequest(buf, playerName);
        connectMsg.payload = buf.GetData();
        SendMessage(connectMsg);

        return true;
    }

    bool NetworkManager::IsEndpointLifecycleIdle() const
    {
        // All callers hold m_apiMutex. NetworkManager has no background socket
        // worker: Update owns the pump, so state plus the native handle fully
        // describe whether an endpoint lifecycle is stopped.
        std::lock_guard<std::mutex> stateLock(m_stateMutex);
        if (m_role.load(std::memory_order_acquire) != NetworkRole::None ||
            m_connectionState != ConnectionState::Disconnected)
            return false;
#ifdef ENABLE_NETWORKING
        return m_socket == INVALID_SOCKET;
#else
        return true;
#endif
    }

    void NetworkManager::Disconnect()
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            if (m_connectionState == ConnectionState::Disconnected)
                return;
            m_connectionState = ConnectionState::Disconnecting;
        }
        ++m_lifecycleEpoch;

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
        // A delayed credential or reliable packet must never survive into a
        // reconnect. Discarding its lifecycle invokes DelayedPacket's secure
        // wire-buffer erasure without resetting unrelated simulator traffic.
        InstabilitySimulator::GetInstance().DiscardPacketsThroughLifecycle(m_lifecycleEpoch);

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
        ReleaseAllClientConnectionState();
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
            m_inputHistory.clear();
        }
        m_peers.clear();
        m_allowLanAdvertisement = false;
    }

    NetworkDiscoveryConfiguration NetworkManager::GetDiscoveryConfiguration() const
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        const NetworkRole role = GetRole();
        return NetworkDiscoveryConfiguration{m_endpointPolicy, role != NetworkRole::None,
                                             role == NetworkRole::Server && m_allowLanAdvertisement};
    }

    // --------------------------------------------------------------------------
    // Send helpers
    // --------------------------------------------------------------------------

    void NetworkManager::SendMessage(const NetworkMessage& msg)
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        // Reject before queueing or assigning a reliable sequence. Otherwise a
        // locally accepted message can be truncated/rejected by the receiver,
        // and ReliableOrdered creates a permanent sequence gap.
        if (!IsNetworkPayloadSizeValid(msg.payload.size()))
        {
            m_droppedOutgoingMessages.fetch_add(1, std::memory_order_relaxed);
            m_stats.packetsDropped++;
            return;
        }

#ifdef ENABLE_NETWORKING
        if (GetRole() == NetworkRole::Client && !IsEndpointAllowed(m_serverAddress))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network,
                            "Rejecting network message before queueing for a disallowed server endpoint");
            m_droppedOutgoingMessages.fetch_add(1, std::memory_order_relaxed);
            m_stats.packetsDropped++;
            return;
        }
        if (msg.localOnly && GetRole() == NetworkRole::Client && !IsLoopbackEndpoint(m_serverAddress))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network,
                            "Rejecting local-only network message before queueing for a non-loopback server");
            m_droppedOutgoingMessages.fetch_add(1, std::memory_order_relaxed);
            m_stats.packetsDropped++;
            return;
        }
#endif

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
        queued.ownerLifecycleEpoch = m_lifecycleEpoch;

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
            queued.sequence = TakeReliableSequence(GetPeerState(SERVER_PEER).nextOutgoingSequence);
        }

        m_outgoingQueue.push(queued);
        m_stats.packetsSent++;
    }

    void NetworkManager::SendToClient(ClientID client, const NetworkMessage& msg)
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        ASSERT_MSG(client != INVALID_CLIENT, "NetworkManager::SendToClient — client ID must not be INVALID_CLIENT");
        if (m_role != NetworkRole::Server)
            return;

        // This check must precede per-peer sequence allocation and unacked
        // insertion so rejected reliable messages leave no delivery gap/state.
        if (!IsNetworkPayloadSizeValid(msg.payload.size()))
        {
            m_droppedOutgoingMessages.fetch_add(1, std::memory_order_relaxed);
            m_stats.packetsDropped++;
            return;
        }

        NetworkMessage copy = msg;
        copy.senderID = 0; // From server
        copy.timestamp = m_serverTime;
        copy.ownerLifecycleEpoch = m_lifecycleEpoch;

#ifdef ENABLE_NETWORKING
        auto addrIt = m_clientAddresses.find(client);
        if (addrIt == m_clientAddresses.end())
            return;
        if (!IsEndpointAllowed(addrIt->second))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "Rejecting network message for a disallowed client endpoint");
            m_droppedOutgoingMessages.fetch_add(1, std::memory_order_relaxed);
            m_stats.packetsDropped++;
            return;
        }
        if (copy.localOnly && !IsLoopbackEndpoint(addrIt->second))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network,
                            "Rejecting local-only network message for a non-loopback client");
            m_droppedOutgoingMessages.fetch_add(1, std::memory_order_relaxed);
            m_stats.packetsDropped++;
            return;
        }

        // Assign this client's own reliable sequence and track for
        // retransmission — sequence streams and unacked maps are per peer, so
        // an ACK from one client can never erase another client's messages.
        if (copy.channel != ChannelType::Unreliable)
        {
            PeerState& peer = GetPeerState(client);
            copy.sequence = TakeReliableSequence(peer.nextOutgoingSequence);
            peer.unacknowledgedMessages[copy.sequence] = copy;
            peer.reliableOriginalSendTime.try_emplace(copy.sequence, m_serverTime);
        }

        auto serialized = SerializeMessage(copy);
        const auto clearSerialized = Spark::MakeScopeExit(
            [sensitive = copy.sensitive, &serialized]
            {
                if (sensitive)
                    Spark::SecureClear(serialized);
            });
        SendImpaired(serialized, client, addrIt->second, copy);
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
            copy.sequence = TakeReliableSequence(GetPeerState(client).nextOutgoingSequence);
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
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        for (ClientID id : GetConnectedClientIDs())
            SendToClient(id, msg);
    }

    void NetworkManager::SendToAllExcept(ClientID excludeClient, const NetworkMessage& msg)
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        for (ClientID id : GetConnectedClientIDs())
        {
            if (id != excludeClient)
                SendToClient(id, msg);
        }
    }

    void NetworkManager::BroadcastMessage(const NetworkMessage& msg)
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        SendToAll(msg);
    }

    NetworkManager::ScopedRegistrationOwner::ScopedRegistrationOwner(NetworkManager& manager, std::string ownerId,
                                                                     bool teardown)
        : m_manager(manager)
    {
        std::lock_guard<std::recursive_mutex> apiLock(manager.m_apiMutex);
        m_previousOwner = std::move(manager.m_registrationOwner);
        m_previousTeardown = manager.m_registrationTeardown;
        manager.m_registrationOwner = std::move(ownerId);
        manager.m_registrationTeardown = teardown;
    }

    NetworkManager::ScopedRegistrationOwner::~ScopedRegistrationOwner()
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_manager.m_apiMutex);
        m_manager.m_registrationOwner = std::move(m_previousOwner);
        m_manager.m_registrationTeardown = m_previousTeardown;
    }

    bool NetworkManager::MayWriteOwnedSlot(const std::string& slotOwner, bool replacing) const
    {
        // Host/engine code outside any scope keeps the historical unrestricted behavior.
        if (m_registrationOwner.empty() || slotOwner.empty() || slotOwner == m_registrationOwner)
        {
            return true;
        }
        // An initializing owner may take over another owner's slot (a hot-reload replacement installs its
        // handlers before the outgoing image is torn down). Nobody may remove another owner's slot, and a
        // tearing-down owner may not overwrite one either.
        return replacing && !m_registrationTeardown;
    }

    void NetworkManager::RegisterHandler(MessageType type, MessageHandler handler)
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        ASSERT_MSG(handler != nullptr, "NetworkManager::RegisterHandler — handler must not be null");
        MessageHandler displaced;
        {
            std::lock_guard<std::mutex> lock(m_handlerMutex);
            const auto value = static_cast<uint16_t>(type);
            const auto ownerIt = m_handlerOwners.find(value);
            const std::string& slotOwner = ownerIt != m_handlerOwners.end() ? ownerIt->second : std::string();
            if (!MayWriteOwnedSlot(slotOwner, true))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Network,
                               "RegisterHandler: '%s' is tearing down and may not replace message type %u owned by "
                               "'%s'",
                               m_registrationOwner.c_str(), static_cast<unsigned>(value), slotOwner.c_str());
                return;
            }
            // Complete the potentially-allocating handler insertion first. A
            // normal registration replaces both the callback and its local
            // classification; allocation failure must leave the old pair intact.
            MessageHandler& slot = m_handlers[value];
            if (!m_registrationOwner.empty())
            {
                m_handlerOwners[value] = m_registrationOwner;
            }
            else
            {
                m_handlerOwners.erase(value);
            }
            displaced = std::exchange(slot, std::move(handler));
            m_sensitiveMessageTypes.erase(value);
        }
        // `displaced` is destroyed here, outside m_handlerMutex, while its owner's image is still mapped.
    }

    void NetworkManager::RegisterSensitiveHandler(MessageType type, MessageHandler handler)
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        ASSERT_MSG(handler != nullptr, "NetworkManager::RegisterSensitiveHandler — handler must not be null");
        MessageHandler displaced;
        {
            std::lock_guard<std::mutex> lock(m_handlerMutex);
            const auto value = static_cast<uint16_t>(type);
            const auto ownerIt = m_handlerOwners.find(value);
            const std::string& slotOwner = ownerIt != m_handlerOwners.end() ? ownerIt->second : std::string();
            if (!MayWriteOwnedSlot(slotOwner, true))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Network,
                               "RegisterSensitiveHandler: '%s' is tearing down and may not replace message type %u "
                               "owned by '%s'",
                               m_registrationOwner.c_str(), static_cast<unsigned>(value), slotOwner.c_str());
                return;
            }
            const auto [sensitiveIt, inserted] = m_sensitiveMessageTypes.insert(value);
            try
            {
                MessageHandler& slot = m_handlers[value];
                if (!m_registrationOwner.empty())
                {
                    m_handlerOwners[value] = m_registrationOwner;
                }
                else
                {
                    m_handlerOwners.erase(value);
                }
                displaced = std::exchange(slot, std::move(handler));
            }
            catch (...)
            {
                if (inserted)
                {
                    m_sensitiveMessageTypes.erase(sensitiveIt);
                }
                throw;
            }
        }
    }

    void NetworkManager::UnregisterHandler(MessageType type)
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        MessageHandler removed;
        {
            std::lock_guard<std::mutex> lock(m_handlerMutex);
            const auto value = static_cast<uint16_t>(type);
            const auto ownerIt = m_handlerOwners.find(value);
            const std::string slotOwner = ownerIt != m_handlerOwners.end() ? ownerIt->second : std::string();
            if (!MayWriteOwnedSlot(slotOwner, false))
            {
                return;
            }
            const auto handlerIt = m_handlers.find(value);
            if (handlerIt != m_handlers.end())
            {
                removed = std::move(handlerIt->second);
                m_handlers.erase(handlerIt);
            }
            m_handlerOwners.erase(value);
            m_sensitiveMessageTypes.erase(value);
        }
    }

    std::vector<NetworkManager::MessageHandler> NetworkManager::TakeHandlersOwnedBy(const std::string& ownerId)
    {
        std::vector<MessageHandler> removed;
        std::lock_guard<std::mutex> lock(m_handlerMutex);
        for (auto ownerIt = m_handlerOwners.begin(); ownerIt != m_handlerOwners.end();)
        {
            if (ownerIt->second != ownerId)
            {
                ++ownerIt;
                continue;
            }
            const auto handlerIt = m_handlers.find(ownerIt->first);
            if (handlerIt != m_handlers.end())
            {
                removed.push_back(std::move(handlerIt->second));
                m_handlers.erase(handlerIt);
            }
            m_sensitiveMessageTypes.erase(ownerIt->first);
            ownerIt = m_handlerOwners.erase(ownerIt);
        }
        return removed;
    }

    size_t NetworkManager::UnregisterHandlersByOwner(const std::string& ownerId)
    {
        if (ownerId.empty())
        {
            return 0;
        }

        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        const std::vector<MessageHandler> removed = TakeHandlersOwnedBy(ownerId);
        size_t removedCount = removed.size();
        if (m_timeoutHandlerOwner == ownerId)
        {
            m_timeoutHandler = nullptr;
            m_timeoutHandlerOwner.clear();
            ++removedCount;
        }
        // `removed` destroys the owner's callbacks here, outside m_handlerMutex, before its image is unmapped.
        return removedCount;
    }

    void NetworkManager::SetTimeoutHandler(std::function<void(ClientID)> handler)
    {
        std::lock_guard<std::recursive_mutex> lock(m_apiMutex);
        const bool clearing = !handler;
        if (!MayWriteOwnedSlot(m_timeoutHandlerOwner, !clearing))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network,
                           "SetTimeoutHandler: '%s' may not %s the timeout handler owned by '%s'",
                           m_registrationOwner.c_str(), clearing ? "clear" : "replace", m_timeoutHandlerOwner.c_str());
            return;
        }
        m_timeoutHandler = std::move(handler);
        m_timeoutHandlerOwner = clearing ? std::string() : m_registrationOwner;
    }

    void NetworkManager::ClearHandlers()
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        if (!m_registrationOwner.empty())
        {
            // A scoped owner clears only the observers it registered (never the timeout handler, as before).
            [[maybe_unused]] const std::vector<MessageHandler> removed = TakeHandlersOwnedBy(m_registrationOwner);
            return;
        }
        std::unordered_map<uint16_t, MessageHandler> removed;
        {
            std::lock_guard<std::mutex> lock(m_handlerMutex);
            removed.swap(m_handlers);
            m_sensitiveMessageTypes.clear();
            m_handlerOwners.clear();
        }
    }

    // --------------------------------------------------------------------------
    // KickClient
    // --------------------------------------------------------------------------

    void NetworkManager::KickClient(ClientID client, const std::string& reason)
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        ASSERT_MSG(client != INVALID_CLIENT, "NetworkManager::KickClient — client ID must not be INVALID_CLIENT");
        SPARK_LOG_INFO(Spark::LogCategory::Network, "Kicking client %u: %s", client, reason.c_str());

        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            if (!m_clients.contains(client))
            {
                return;
            }
        }

        NetworkMessage msg;
        msg.type = MessageType::Disconnect;
        msg.channel = ChannelType::Reliable;
        NetBuffer buf;
        buf.WriteString(reason);
        msg.payload = buf.GetData();
        SendToClient(client, msg);

        RemoveClientState(client);
    }

    // --------------------------------------------------------------------------
    // HandleConnect / HandleDisconnect
    // --------------------------------------------------------------------------

    ClientID NetworkManager::PrepareNextClientID()
    {
        std::lock_guard<std::mutex> clientsLock(m_clientsMutex);
        ClientID candidate = NormalizeGeneratedClientID(m_nextClientID);

        // There are vastly more generated IDs than live clients. Examining one
        // more candidate than the number of occupied IDs guarantees a free ID
        // without ever walking the application-reserved high range.
        const size_t attempts = m_clients.size() + 1;
        for (size_t attempt = 0; attempt < attempts; ++attempt)
        {
            if (!m_clients.contains(candidate))
            {
                m_nextClientID = candidate;
                return candidate;
            }
            candidate = AdvanceGeneratedClientID(candidate);
        }
        return INVALID_CLIENT;
    }

    void NetworkManager::RejectPendingConnect(ClientID pendingID, ConnectRejectReason reason, const std::string& text)
    {
        NetworkMessage reject;
        reject.type = MessageType::ConnectRejected;
        reject.channel = ChannelType::Reliable;
        NetBuffer rejectBuf;
        rejectBuf.WriteString(text);
        rejectBuf.WriteUint8(static_cast<uint8_t>(reason));
        rejectBuf.WriteUint16(NETWORK_PROTOCOL_VERSION);
        reject.payload = rejectBuf.GetData();

        // Send rejection only to the connecting client (not broadcast)
        SPARK_LOG_WARN(Spark::LogCategory::Network, "Connection rejected for pending client %u: %s", pendingID,
                       text.c_str());
        SendToClient(pendingID, reject);

#ifdef ENABLE_NETWORKING
        // Clean up the pre-registered address so the rejected client
        // doesn't receive broadcast traffic meant for real clients
        m_clientAddresses.erase(pendingID);
#endif
    }

    void NetworkManager::AbandonClientHandshake(ConnectRejectReason reason, std::string text)
    {
        uint64_t rejectedLifecycleEpoch = 0;
        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            if (m_role.load(std::memory_order_acquire) != NetworkRole::Client ||
                m_connectionState != ConnectionState::Connecting)
                return;
            rejectedLifecycleEpoch = m_lifecycleEpoch;
            m_lastConnectionError = text;
            m_lastConnectRejectReason = reason;
            m_connectionState = ConnectionState::Disconnected;
            m_role = NetworkRole::None;
            m_localClientID = INVALID_CLIENT;
            m_wasConnected = false;
        }
        ++m_lifecycleEpoch;
#ifdef ENABLE_NETWORKING
        CloseSocket();
#endif
        DiscardClientLifecycleTraffic(rejectedLifecycleEpoch);
        SPARK_LOG_WARN(Spark::LogCategory::Network, "Connection rejected: %s", text.c_str());
    }

    void NetworkManager::TerminateClientSession(const std::string& reason, bool keepReconnectArmed)
    {
        uint64_t endedLifecycleEpoch = 0;
        {
            std::lock_guard<std::mutex> stateLock(m_stateMutex);
            if (m_role.load(std::memory_order_acquire) != NetworkRole::Client ||
                m_connectionState == ConnectionState::Disconnected)
            {
                return;
            }
            endedLifecycleEpoch = m_lifecycleEpoch;
            m_lastConnectionError = reason;
            m_connectionState = ConnectionState::Disconnected;
            m_role = NetworkRole::None;
            m_localClientID = INVALID_CLIENT;
            if (!keepReconnectArmed)
            {
                // The server ended the session on purpose (kick, ban, shutdown). Its
                // decision is authoritative: reconnecting would loop admit -> kick
                // forever, because every ConnectAccepted resets the attempt counter.
                m_wasConnected = false;
                m_reconnectAttempts = 0;
            }
            // Otherwise m_wasConnected is left as-is: a lost session that was
            // established keeps auto-reconnect armed, a handshake that never
            // completed does not.
        }
        // The epoch bump stops the rest of the Update batch that delivered the
        // terminal packet, exactly like an application-initiated Disconnect().
        ++m_lifecycleEpoch;
#ifdef ENABLE_NETWORKING
        CloseSocket();
#endif
        DiscardClientLifecycleTraffic(endedLifecycleEpoch);
        {
            std::lock_guard<std::mutex> lock(m_replicationMutex);
            m_replicatedEntities.clear();
            ++m_replicationMutationEpoch;
        }
        m_lagCompensator.Clear();
        {
            std::lock_guard<std::mutex> lock(m_inputMutex);
            m_inputHistory.clear();
        }
        m_allowLanAdvertisement = false;
        SPARK_LOG_WARN(Spark::LogCategory::Network, "Client session ended: %s", reason.c_str());
    }

    ClientID NetworkManager::HandleConnect(const NetworkMessage& msg)
    {
        if (GetRole() != NetworkRole::Server)
            return INVALID_CLIENT;

        // ProcessIncoming pre-registers m_clientAddresses[m_nextClientID] so
        // that SendToClient can reach the new client. Every rejection below
        // must clean up that entry to avoid stale addresses receiving broadcasts.
        const ClientID pendingID = m_nextClientID;

        // Protocol negotiation happens before any slot is considered: a peer that
        // cannot speak this exact wire version never occupies server state.
        NetBuffer request;
        request.WriteBytes(msg.payload.data(), msg.payload.size());
        const uint32_t magic = request.ReadUint32();
        const uint16_t clientVersion = request.ReadUint16();
        if (request.HasError() || magic != NETWORK_HANDSHAKE_MAGIC)
        {
            RejectPendingConnect(pendingID, ConnectRejectReason::ProtocolMissing,
                                 std::format("Protocol version required (server speaks {})", NETWORK_PROTOCOL_VERSION));
            return INVALID_CLIENT;
        }
        if (clientVersion != NETWORK_PROTOCOL_VERSION)
        {
            const bool older = clientVersion < NETWORK_PROTOCOL_VERSION;
            RejectPendingConnect(pendingID,
                                 older ? ConnectRejectReason::ProtocolTooOld : ConnectRejectReason::ProtocolTooNew,
                                 std::format("Client protocol version {} is {} than server version {}", clientVersion,
                                             older ? "older" : "newer", NETWORK_PROTOCOL_VERSION));
            return INVALID_CLIENT;
        }
        std::string playerName = request.ReadString();
        if (request.HasError() || request.RemainingBytes() != 0)
        {
            RejectPendingConnect(pendingID, ConnectRejectReason::MalformedHandshake, "Malformed connect request");
            return INVALID_CLIENT;
        }

        if (static_cast<int>(m_clients.size()) >= m_maxClients)
        {
            RejectPendingConnect(pendingID, ConnectRejectReason::ServerFull,
                                 std::format("Server full ({}/{})", m_clients.size(), m_maxClients));
            return INVALID_CLIENT;
        }

        const ClientID newID = m_nextClientID;
        m_nextClientID = AdvanceGeneratedClientID(newID);
        ClientInfo info;
        info.id = newID;
        info.state = ConnectionState::Connected;
        info.lastHeartbeatTime = m_serverTime;
        info.name = playerName.empty() ? "Player_" + std::to_string(newID) : std::move(playerName);
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            m_clients[newID] = info;
        }

        // Register the new connection for delta snapshot tracking
        DeltaSnapshotManager::GetInstance().RegisterConnection(newID);

        // Send acceptance with assigned client ID, echoing the negotiated protocol version
        NetworkMessage accept;
        accept.type = MessageType::ConnectAccepted;
        accept.channel = ChannelType::Reliable;
        NetBuffer respBuf;
        respBuf.WriteUint32(newID);
        respBuf.WriteFloat(m_serverTime);
        respBuf.WriteUint16(NETWORK_PROTOCOL_VERSION);
        accept.payload = respBuf.GetData();
        SendToClient(newID, accept);
        SPARK_LOG_INFO(Spark::LogCategory::Network, "Client %u accepted ('%s'), %d/%d slots used", newID,
                       info.name.c_str(), static_cast<int>(m_clients.size()), m_maxClients);
        return newID;
    }

    void NetworkManager::HandleDisconnect(const NetworkMessage& msg)
    {
        // A client only hears Disconnect from its own server endpoint (ProcessIncoming
        // drops every other source): the server kicked it or shut down, so the session
        // is over. The server-side cleanup below must never run here -- it would erase
        // the client's SERVER_PEER reliability state and INVALID_CLIENT-owned entities
        // while leaving the session Connected with its socket open.
        if (GetRole() == NetworkRole::Client)
        {
            std::string reason = "Disconnected by server";
            if (!msg.payload.empty())
            {
                NetBuffer buf;
                buf.WriteBytes(msg.payload.data(), msg.payload.size());
                const std::string supplied = buf.ReadString();
                if (!buf.HasError() && !supplied.empty())
                {
                    reason += ": " + supplied;
                }
            }
            TerminateClientSession(reason, /*keepReconnectArmed=*/false);
            return;
        }

        const ClientID clientID = msg.senderID;
        SPARK_LOG_INFO(Spark::LogCategory::Network, "Client %u disconnecting", clientID);
        RemoveClientState(clientID);
    }

    void NetworkManager::RemoveClientState(ClientID clientID)
    {
        // Every per-connection record is dropped here, so a timed-out, kicked or
        // departed client leaves nothing behind in the process-global singletons
        // and a future connection reusing the ID starts fresh.
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
        // Each admitted client is queued for its initial sync at most once.
        const auto pendingSync = std::find(m_pendingFullSyncs.begin(), m_pendingFullSyncs.end(), clientID);
        if (pendingSync != m_pendingFullSyncs.end())
        {
            m_pendingFullSyncs.erase(pendingSync);
        }

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

    void NetworkManager::ReleaseAllClientConnectionState()
    {
        std::vector<ClientID> clientIDs;
        {
            std::lock_guard<std::mutex> lock(m_clientsMutex);
            clientIDs.reserve(m_clients.size());
            for (const auto& [id, info] : m_clients)
            {
                clientIDs.push_back(id);
            }
        }
        m_pendingFullSyncs.clear();
        auto& deltaManager = DeltaSnapshotManager::GetInstance();
        auto& scopeFilter = ConnectionScopeFilter::GetInstance();
        for (const ClientID id : clientIDs)
        {
            deltaManager.UnregisterConnection(id);
            scopeFilter.RemoveConnection(static_cast<uint32_t>(id));
        }
    }

    // --------------------------------------------------------------------------
    // CheckConnectionTimeouts
    // --------------------------------------------------------------------------

    std::vector<ClientID> NetworkManager::CheckConnectionTimeouts()
    {
        std::vector<ClientID> timedOut;
        if (m_role == NetworkRole::Server)
        {
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
                RemoveClientState(id);
            }
        }
#ifdef ENABLE_NETWORKING
        else if (m_role == NetworkRole::Client)
        {
            // The server heartbeats every m_heartbeatInterval, so a client that has
            // heard nothing from its server endpoint for m_connectionTimeout (while
            // handshaking or connected) has lost the session. Ending it here closes
            // the socket and lets auto-reconnect run instead of idling forever.
            const ConnectionState state = GetConnectionState();
            if ((state == ConnectionState::Connecting || state == ConnectionState::Connected) &&
                m_serverTime - m_lastServerPacketTime > m_connectionTimeout)
            {
                TerminateClientSession(
                    std::format("Server timed out (no traffic for {:.1f}s)", m_serverTime - m_lastServerPacketTime),
                    /*keepReconnectArmed=*/true);
            }
        }
#endif // ENABLE_NETWORKING

        return timedOut;
    }

    // --------------------------------------------------------------------------
    // ProcessIncoming -- read from socket, parse, enqueue
    // --------------------------------------------------------------------------

    std::vector<NetworkMessage> NetworkManager::ProcessIncoming()
    {
        std::vector<NetworkMessage> newlyConnected;
#ifdef ENABLE_NETWORKING
        if (m_socket == INVALID_SOCKET)
            return newlyConnected;

        // Read all available datagrams
        for (int i = 0; i < 256; ++i) // Cap per-frame reads
        {
            std::vector<uint8_t> rawData;
            sockaddr_in senderAddr{};
            int received = ReceiveRaw(rawData, senderAddr);
            if (received <= 0)
                break;
            // ReceiveRaw owns the first plaintext wire copy. Erase every
            // datagram on scope exit so malformed/early-rejected sensitive
            // packets are covered before their marker can be trusted.
            const auto clearRawData = Spark::MakeScopeExit([&rawData] { Spark::SecureClear(rawData); });

            const NetworkRole role = GetRole();
            if (!IsEndpointAllowed(senderAddr))
            {
                SPARK_LOG_DEBUG(Spark::LogCategory::Network,
                                "Dropping packet from an endpoint outside the captured policy before parsing");
                m_droppedIncomingMessages.fetch_add(1, std::memory_order_relaxed);
                m_stats.packetsDropped++;
                continue;
            }
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
                // NEVER derive admission from the wire-supplied senderID: the sender fully
                // controls that field on packets it transmits, so trusting it lets a
                // client stamp any non-zero value and bypass every requiresAuth schema
                // server-side. It also collides with SendToClient's senderID=0 stamp
                // (0 == INVALID_CLIENT) for server-sent messages, which made every
                // server->client message read as unadmitted on the client and
                // silently dropped all requiresAuth replication traffic. Derive this
                // legacy schema-admission flag from OUR OWN connection state instead:
                // the address->client table on the server, live connection state on the
                // client. This is not cryptographic peer authentication.
                bool isAdmitted;
                if (role == NetworkRole::Server)
                    isAdmitted = (trustedSender != INVALID_CLIENT);
                else
                    isAdmitted = (GetConnectionState() == ConnectionState::Connected);
                bool isFromClient = (role == NetworkRole::Server);
                auto validation = m_packetValidator.ValidatePacket(msg, isAdmitted, isFromClient);
                if (!validation.valid)
                {
                    SPARK_LOG_DEBUG(Spark::LogCategory::Network, "Packet rejected: %s", validation.reason.c_str());
                    continue;
                }
            }
            SPARK_BRANCH_GUARD_END("network_packet_gateway")

            // Client liveness: any validated datagram from the server endpoint
            // (heartbeats included) proves the session is still alive.
            if (role == NetworkRole::Client)
            {
                m_lastServerPacketTime = m_serverTime;
            }

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
                    const ClientID pendingID = PrepareNextClientID();
                    if (pendingID == INVALID_CLIENT)
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Network,
                                        "Cannot admit client: generated client ID space is exhausted");
                        continue;
                    }
                    m_clientAddresses[pendingID] = senderAddr;

                    const ClientID admittedID = HandleConnect(msg);
                    if (admittedID != INVALID_CLIENT)
                    {
                        msg.senderID = admittedID;
                        newlyConnected.push_back(std::move(msg));
                    }
                    continue;
                }

                if (trustedSender == INVALID_CLIENT)
                {
                    SPARK_LOG_DEBUG(Spark::LogCategory::Network, "Dropping packet from unknown client endpoint");
                    continue;
                }
            }

            msg.ownerLifecycleEpoch = m_lifecycleEpoch;
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

#else
        // Without networking, just dispatch whatever is in the queue already
        // (for local/testing scenarios)
#endif // ENABLE_NETWORKING
        return newlyConnected;
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
            for (auto& packet : readyPackets)
            {
                if (packet.lifecycleEpoch != 0 && packet.lifecycleEpoch != m_lifecycleEpoch)
                {
                    m_droppedOutgoingMessages.fetch_add(1, std::memory_order_relaxed);
                    m_stats.packetsDropped++;
                    continue;
                }
                if (m_role == NetworkRole::Client)
                {
                    if (packet.localOnly && !IsLoopbackEndpoint(m_serverAddress))
                    {
                        if (packet.sequence != 0)
                        {
                            PeerState& peer = GetPeerState(SERVER_PEER);
                            peer.unacknowledgedMessages.erase(packet.sequence);
                            peer.reliableOriginalSendTime.erase(packet.sequence);
                            peer.retransmitCounts.erase(packet.sequence);
                        }
                        m_droppedOutgoingMessages.fetch_add(1, std::memory_order_relaxed);
                        m_stats.packetsDropped++;
                        continue;
                    }
                    SendRawTo(packet.data, m_serverAddress, packet.localOnly);
                }
                else if (m_role == NetworkRole::Server)
                {
                    // Delayed packets are per destination (SendImpaired): a
                    // unicast must never fan out, and a client that left while
                    // its packet was held simply loses it.
                    const auto addrIt = m_clientAddresses.find(static_cast<ClientID>(packet.destinationKey));
                    if (addrIt == m_clientAddresses.end())
                    {
                        m_droppedOutgoingMessages.fetch_add(1, std::memory_order_relaxed);
                        m_stats.packetsDropped++;
                        continue;
                    }
                    SendRawTo(packet.data, addrIt->second, packet.localOnly);
                }
            }
        }

        while (!toSend.empty())
        {
            const auto& msg = toSend.front();
            if (msg.ownerLifecycleEpoch != 0 && msg.ownerLifecycleEpoch != m_lifecycleEpoch)
            {
                m_droppedOutgoingMessages.fetch_add(1, std::memory_order_relaxed);
                m_stats.packetsDropped++;
                toSend.pop();
                continue;
            }
            if (m_role == NetworkRole::Client && msg.localOnly && !IsLoopbackEndpoint(m_serverAddress))
            {
                m_droppedOutgoingMessages.fetch_add(1, std::memory_order_relaxed);
                m_stats.packetsDropped++;
                toSend.pop();
                continue;
            }
            auto serialized = SerializeMessage(msg);
            const auto clearSerialized = Spark::MakeScopeExit(
                [sensitive = msg.sensitive, &serialized]
                {
                    if (sensitive)
                        Spark::SecureClear(serialized);
                });
            if (serialized.empty())
            {
                m_droppedOutgoingMessages.fetch_add(1, std::memory_order_relaxed);
                m_stats.packetsDropped++;
                toSend.pop();
                continue;
            }

            if (m_role == NetworkRole::Client)
            {
                SendImpaired(serialized, SERVER_PEER, m_serverAddress, msg);
            }
            else if (m_role == NetworkRole::Server)
            {
                // Impairment decisions are per destination, so each client
                // gets its own copy to drop, delay or duplicate independently.
                const bool impaired = instability.GetSettings().enabled;
                for (const auto& [id, addr] : m_clientAddresses)
                {
                    if (!impaired)
                    {
                        SendRawTo(serialized, addr, msg.localOnly);
                        continue;
                    }
                    std::vector<uint8_t> perClient = serialized;
                    const auto clearPerClient = Spark::MakeScopeExit(
                        [sensitive = msg.sensitive, &perClient]
                        {
                            if (sensitive)
                                Spark::SecureClear(perClient);
                        });
                    SendImpaired(perClient, id, addr, msg);
                }
            }

            // Track reliable messages for retransmission, including ones the
            // simulator dropped or is still holding.
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

#ifdef ENABLE_NETWORKING
    void NetworkManager::SendImpaired(std::vector<uint8_t>& serialized, ClientID destination, const sockaddr_in& addr,
                                      const NetworkMessage& msg)
    {
        auto& instability = InstabilitySimulator::GetInstance();
        const InstabilitySettings settings = instability.GetSettings();

        // Disconnect is a terminal control packet: Disconnect() immediately
        // closes the socket and purges this lifecycle after this flush, so
        // delaying or dropping it would guarantee it is never transmitted.
        if (!settings.enabled || msg.type == MessageType::Disconnect)
        {
            SendRawTo(serialized, addr, msg.localOnly);
            return;
        }

        if (instability.ShouldDropPacket())
        {
            m_stats.packetsDropped++;
            return;
        }

        const float nowMs = m_serverTime * 1000.0f;
        float delayMs = instability.GetDelayMs();
        // A reordered packet is held past the normal delay so packets sent
        // after it overtake it on the wire.
        if (instability.ShouldReorder())
            delayMs += settings.reorderHoldMs;

        // The duplicate trails the original by 1 ms and carries the same
        // reliable sequence, so the receiver's dedup must deliver it once.
        if (instability.ShouldDuplicate())
            instability.QueuePacket(std::vector<uint8_t>(serialized), nowMs + delayMs + 1.0f, msg.localOnly,
                                    msg.sequence, msg.ownerLifecycleEpoch, destination);

        if (delayMs > 0.0f)
        {
            instability.QueuePacket(std::move(serialized), nowMs + delayMs, msg.localOnly, msg.sequence,
                                    msg.ownerLifecycleEpoch, destination);
            return;
        }
        SendRawTo(serialized, addr, msg.localOnly);
    }
#endif // ENABLE_NETWORKING

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
                if (retransmitMsg.ownerLifecycleEpoch != 0 && retransmitMsg.ownerLifecycleEpoch != m_lifecycleEpoch)
                {
                    peer.unacknowledgedMessages.erase(it);
                    peer.reliableOriginalSendTime.erase(seq);
                    peer.retransmitCounts.erase(seq);
                    m_droppedOutgoingMessages.fetch_add(1, std::memory_order_relaxed);
                    m_stats.packetsDropped++;
                    continue;
                }
                if (!IsEndpointAllowed(*destination) || (retransmitMsg.localOnly && !IsLoopbackEndpoint(*destination)))
                {
                    peer.unacknowledgedMessages.erase(it);
                    peer.reliableOriginalSendTime.erase(seq);
                    peer.retransmitCounts.erase(seq);
                    m_droppedOutgoingMessages.fetch_add(1, std::memory_order_relaxed);
                    m_stats.packetsDropped++;
                    continue;
                }
                retransmitMsg.timestamp = m_serverTime; // Reset retransmit timer
                auto serialized = SerializeMessage(retransmitMsg);
                const auto clearSerialized = Spark::MakeScopeExit(
                    [sensitive = retransmitMsg.sensitive, &serialized]
                    {
                        if (sensitive)
                            Spark::SecureClear(serialized);
                    });
                SendRawTo(serialized, *destination, retransmitMsg.localOnly);
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

    uint16_t NetworkManager::GetBoundPort() const
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
#ifdef ENABLE_NETWORKING
        if (m_socket == INVALID_SOCKET)
            return 0;
        sockaddr_in address{};
        socklen_t addressLength = sizeof(address);
        if (getsockname(m_socket, reinterpret_cast<sockaddr*>(&address), &addressLength) == SOCKET_ERROR)
            return 0;
        return ntohs(address.sin_port);
#else
        return 0;
#endif
    }

    bool NetworkManager::IsClientLoopback(ClientID client) const
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
#ifdef ENABLE_NETWORKING
        {
            std::lock_guard<std::mutex> clientsLock(m_clientsMutex);
            if (!m_clients.contains(client))
                return false;
        }
        const auto address = m_clientAddresses.find(client);
        if (address == m_clientAddresses.end())
            return false;
        return (ntohl(address->second.sin_addr.s_addr) & 0xFF000000u) == 0x7F000000u;
#else
        (void)client;
        return false;
#endif
    }

    std::string NetworkManager::Console_GetStatus() const
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
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
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
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
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
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
