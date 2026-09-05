/**
 * @file NetworkManager.cpp
 * @brief Core NetworkManager — singleton, update loop, socket helpers, lag compensation
 *
 * Connection lifecycle methods are in NetworkConnection.cpp.
 * Entity replication methods are in NetworkReplication.cpp.
 * Reliable channel logic is in NetworkReliable.cpp.
 * NetBuffer serialization is in NetworkBuffer.cpp.
 */

#include "NetworkManager.h"
#include "NetworkBindPolicy.h"
#include "../../Core/EngineContext.h"
#include "../../Utils/Assert.h"
#include "../../Utils/DebugHookManager.h"
#include "../../Utils/ScopeGuard.h"
#include "../../Utils/SecureMemory.h"
#include "../../Utils/Validate.h"
#include <sstream>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <thread>
#include <utility>

// Windows headers may redefine SendMessage after our includes.
// Undefine it so NetworkManager::SendMessage compiles correctly.
#ifdef SendMessage
#undef SendMessage
#endif

using namespace DirectX;
namespace Spark::Net
{

    namespace
    {
        /// Rewinding further than the lag compensator keeps history for is not a
        /// legitimate request. This is only the fallback ceiling on halfRTT; the
        /// authoritative window is the compensator's own GetMaxHistoryDuration(),
        /// which SetMaxHistoryDuration can change at runtime.
        constexpr float MAX_LAG_COMPENSATION_SECONDS = 1.0f;

        /// Hard ceiling on a client-declared hitscan range. Without it a client can
        /// ask for 1e30 and reach every entity in the rewound snapshot.
        constexpr float MAX_HIT_VALIDATION_DISTANCE = 10000.0f;

        /// A ray direction shorter than this cannot be normalized meaningfully.
        constexpr float MIN_HIT_DIRECTION_LENGTH_SQ = 1e-12f;
    } // namespace

    NetworkMessage::NetworkMessage(NetworkMessage&& other) noexcept
        : type(other.type), channel(other.channel), senderID(other.senderID), sequence(other.sequence),
          payload(std::move(other.payload)), timestamp(other.timestamp),
          sensitive(std::exchange(other.sensitive, false)), localOnly(std::exchange(other.localOnly, false)),
          ownerLifecycleEpoch(std::exchange(other.ownerLifecycleEpoch, 0))
    {
    }

    NetworkMessage& NetworkMessage::operator=(const NetworkMessage& other)
    {
        if (this == &other)
            return *this;

        // Build the potentially-throwing payload copy before modifying this
        // object. Moving the completed temporary is noexcept and securely
        // clears any sensitive payload this object previously owned.
        NetworkMessage copy(other);
        *this = std::move(copy);
        return *this;
    }

    NetworkMessage& NetworkMessage::operator=(NetworkMessage&& other) noexcept
    {
        if (this == &other)
            return *this;

        ClearSensitivePayload();
        type = other.type;
        channel = other.channel;
        senderID = other.senderID;
        sequence = other.sequence;
        payload = std::move(other.payload);
        timestamp = other.timestamp;
        sensitive = std::exchange(other.sensitive, false);
        localOnly = std::exchange(other.localOnly, false);
        ownerLifecycleEpoch = std::exchange(other.ownerLifecycleEpoch, 0);
        return *this;
    }

    NetworkMessage::~NetworkMessage()
    {
        ClearSensitivePayload();
    }

    void NetworkMessage::ClearSensitivePayload() noexcept
    {
        if (sensitive)
            Spark::SecureClear(payload);
        sensitive = false;
    }

    // ============================================================================
    // LagCompensator
    // ============================================================================

    void LagCompensator::RecordSnapshot(const HistorySnapshot& snapshot)
    {
        m_history.push_back(snapshot);

        // Prune old snapshots
        float cutoff = snapshot.timestamp - m_maxHistoryDuration;
        m_history.erase(std::remove_if(m_history.begin(), m_history.end(),
                                       [cutoff](const HistorySnapshot& s) { return s.timestamp < cutoff; }),
                        m_history.end());
    }

    bool LagCompensator::RewindToTime(float targetTime, HistorySnapshot& outSnapshot) const
    {
        if (m_history.empty())
            return false;

        // Find the two snapshots bracketing the target time
        const HistorySnapshot* before = nullptr;
        const HistorySnapshot* after = nullptr;

        for (size_t i = 0; i < m_history.size(); ++i)
        {
            if (m_history[i].timestamp <= targetTime)
            {
                before = &m_history[i];
            }
            if (m_history[i].timestamp >= targetTime && !after)
            {
                after = &m_history[i];
            }
        }

        if (!before && !after)
            return false;

        // If we have both, interpolate; otherwise use whichever we have
        if (before && after && before != after)
        {
            float timeDelta = after->timestamp - before->timestamp;
            float t = (timeDelta > 0.0f) ? (targetTime - before->timestamp) / timeDelta : 0.0f;
            t = (std::max)(0.0f, (std::min)(1.0f, t));

            outSnapshot.timestamp = targetTime;
            outSnapshot.entities.clear();

            for (const auto& entityBefore : before->entities)
            {
                for (const auto& entityAfter : after->entities)
                {
                    if (entityBefore.networkID == entityAfter.networkID)
                    {
                        HistorySnapshot::EntityState interpolated;
                        interpolated.networkID = entityBefore.networkID;

                        XMVECTOR pb = XMLoadFloat3(&entityBefore.position);
                        XMVECTOR pa = XMLoadFloat3(&entityAfter.position);
                        XMStoreFloat3(&interpolated.position, XMVectorLerp(pb, pa, t));

                        XMVECTOR rb = XMLoadFloat3(&entityBefore.rotation);
                        XMVECTOR ra = XMLoadFloat3(&entityAfter.rotation);
                        XMStoreFloat3(&interpolated.rotation, XMVectorLerp(rb, ra, t));

                        XMVECTOR bminB = XMLoadFloat3(&entityBefore.boundsMin);
                        XMVECTOR bminA = XMLoadFloat3(&entityAfter.boundsMin);
                        XMStoreFloat3(&interpolated.boundsMin, XMVectorLerp(bminB, bminA, t));

                        XMVECTOR bmaxB = XMLoadFloat3(&entityBefore.boundsMax);
                        XMVECTOR bmaxA = XMLoadFloat3(&entityAfter.boundsMax);
                        XMStoreFloat3(&interpolated.boundsMax, XMVectorLerp(bmaxB, bmaxA, t));

                        outSnapshot.entities.push_back(interpolated);
                        break;
                    }
                }
            }
            return true;
        }
        else if (before)
        {
            outSnapshot = *before;
            return true;
        }
        else if (after)
        {
            outSnapshot = *after;
            return true;
        }

        return false;
    }

    // ============================================================================
    // NetworkManager — Singleton & Lifecycle
    // ============================================================================

    NetworkManager& NetworkManager::GetInstance()
    {
        if (auto* ctx = EngineContext::Get())
        {
            if (auto* service = ctx->GetNetworkService())
            {
                if (auto* network = dynamic_cast<NetworkManager*>(service))
                {
                    return *network;
                }
            }
        }

        static NetworkManager instance;
        return instance;
    }

    NetworkManager::~NetworkManager()
    {
        Shutdown();
    }

    // --------------------------------------------------------------------------
    // Socket helpers (ENABLE_NETWORKING only)
    // --------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING

    bool NetworkManager::CreateSocket(uint16_t port, const NetworkEndpointPolicy& endpointPolicy)
    {
        if (!endpointPolicy.IsValid())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "Refusing UDP startup: %s",
                            NetworkEndpointPolicyErrorText(endpointPolicy.Error()).data());
            return false;
        }

        // Retry socket creation up to 3 times for transient OS-level failures
        constexpr int kMaxSocketRetries = 3;
        for (int attempt = 0; attempt < kMaxSocketRetries; ++attempt)
        {
            m_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (m_socket != INVALID_SOCKET)
                break;

            SPARK_LOG_WARN(Spark::LogCategory::Network, "Socket creation attempt %d/%d failed — retrying", attempt + 1,
                           kMaxSocketRetries);
            std::this_thread::sleep_for(std::chrono::milliseconds(100 * (1 << attempt)));
        }
        if (m_socket == INVALID_SOCKET)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "Failed to create UDP socket after %d attempts",
                            kMaxSocketRetries);
            return false;
        }

        // Set non-blocking mode
#ifdef SPARK_PLATFORM_WINDOWS
        u_long nonBlocking = 1;
        if (ioctlsocket(m_socket, FIONBIO, &nonBlocking) == SOCKET_ERROR)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "Failed to set socket to non-blocking mode");
            CloseSocket();
            return false;
        }
#else
        int flags = fcntl(m_socket, F_GETFL, 0);
        if (flags == -1 || fcntl(m_socket, F_SETFL, flags | O_NONBLOCK) == -1)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "Failed to set socket to non-blocking mode");
            CloseSocket();
            return false;
        }
#endif // SPARK_PLATFORM_WINDOWS

        // Set socket buffer sizes for game traffic
        int sendBufSize = 65536;
        int recvBufSize = 65536;
        setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBufSize), sizeof(sendBufSize));
        setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&recvBufSize), sizeof(recvBufSize));

#ifdef SPARK_PLATFORM_WINDOWS
        // A game listener owns its requested endpoint exclusively. Fail closed
        // if Windows cannot apply this before bind; SO_REUSEADDR would permit a
        // second socket to intercept or split UDP traffic on the same port.
        const int exclusiveAddressUse = 1;
        if (setsockopt(m_socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusiveAddressUse),
                       sizeof(exclusiveAddressUse)) == SOCKET_ERROR)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "Failed to make UDP socket address-exclusive");
            CloseSocket();
            return false;
        }
#endif

        // Bind exactly to the requested port (0 = OS-assigned ephemeral port
        // for clients/tests). A caller that requests a concrete port must never
        // be silently moved to an adjacent endpoint.
        sockaddr_in localAddr{};
        localAddr.sin_family = AF_INET;
        localAddr.sin_addr.s_addr = htonl(endpointPolicy.BindAddress());
        localAddr.sin_port = htons(port);

        if (::bind(m_socket, reinterpret_cast<const sockaddr*>(&localAddr), sizeof(localAddr)) == SOCKET_ERROR)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "Failed to bind UDP socket to %s:%u",
                            FormatIPv4Address(endpointPolicy.BindAddress()).c_str(), port);
            CloseSocket();
            return false;
        }

        m_endpointPolicy = endpointPolicy;
        return true;
    }

    bool NetworkManager::IsEndpointAllowed(const sockaddr_in& address) const noexcept
    {
        return address.sin_family == AF_INET && m_endpointPolicy.AllowsPeerAddress(ntohl(address.sin_addr.s_addr));
    }

    void NetworkManager::CloseSocket()
    {
        if (m_socket != INVALID_SOCKET)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            closesocket(m_socket);
#else
            close(m_socket);
#endif
            m_socket = INVALID_SOCKET;
        }
    }

    std::vector<uint8_t> NetworkManager::SerializeMessage(const NetworkMessage& msg) const
    {
        // Wire format (little-endian):
        //   [4] magic 0x5350524B ("SPRK")
        //   [2] message type
        //   [1] channel type
        //   [4] sender ID
        //   [4] sequence number
        //   [4] timestamp (float bits)
        //   [4] payload length
        //   [N] payload bytes

        if (!IsNetworkPayloadSizeValid(msg.payload.size()))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "Payload size %zu exceeds UDP wire maximum (%zu)",
                           msg.payload.size(), MAX_NETWORK_MESSAGE_PAYLOAD_SIZE);
            return {};
        }

        NetBuffer buf;
        const auto clearSensitiveWire = Spark::MakeScopeExit(
            [&buf, &msg]
            {
                if (msg.sensitive)
                    buf.SecureReset();
            });
        buf.WriteUint32(0x5350524B); // Magic
        buf.WriteUint16(static_cast<uint16_t>(msg.type));
        // Sensitivity is deliberately process-local ownership metadata. Keep
        // the version-1 channel byte byte-for-byte compatible with older peers.
        buf.WriteUint8(static_cast<uint8_t>(msg.channel));
        buf.WriteUint32(msg.senderID);
        buf.WriteUint32(msg.sequence);
        buf.WriteFloat(msg.timestamp);
        buf.WriteUint32(static_cast<uint32_t>(msg.payload.size()));
        if (!msg.payload.empty())
        {
            buf.WriteBytes(msg.payload.data(), msg.payload.size());
        }
        return std::vector<uint8_t>(buf.GetData().begin(), buf.GetData().end());
    }

    bool NetworkManager::DeserializeMessage(const uint8_t* data, size_t length, NetworkMessage& outMsg) const
    {
        // Callers may reuse an output object across receives. Never let a later
        // packet assignment release an earlier sensitive payload un-scrubbed.
        outMsg.ClearSensitivePayload();
        outMsg.localOnly = false;
        outMsg.ownerLifecycleEpoch = 0;

        // Minimum header: magic(4) + type(2) + channel(1) + sender(4) + seq(4) + timestamp(4) + payloadLen(4) = 23
        if (length < NETWORK_WIRE_HEADER_SIZE)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "Packet too small (%zu bytes, need %zu minimum)", length,
                           NETWORK_WIRE_HEADER_SIZE);
            return false;
        }
        if (length > MAX_UDP_WIRE_DATAGRAM_SIZE)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "Packet size %zu exceeds UDP wire maximum (%zu)", length,
                           MAX_UDP_WIRE_DATAGRAM_SIZE);
            return false;
        }

        NetBuffer buf;
        buf.WriteBytes(data, length);
        const auto clearWireCopy = Spark::MakeScopeExit([&buf] { buf.SecureReset(); });

        uint32_t magic = buf.ReadUint32();
        if (magic != 0x5350524B)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "Invalid packet magic 0x%08X (expected 0x5350524B)", magic);
            return false;
        }

        outMsg.type = static_cast<MessageType>(buf.ReadUint16());
        const uint8_t rawChannel = buf.ReadUint8();
        if (rawChannel > static_cast<uint8_t>(ChannelType::ReliableOrdered))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "Invalid packet channel %u", rawChannel);
            return false;
        }
        outMsg.channel = static_cast<ChannelType>(rawChannel);
        {
            std::lock_guard<std::mutex> lock(m_handlerMutex);
            outMsg.sensitive = m_sensitiveMessageTypes.contains(static_cast<uint16_t>(outMsg.type));
        }
        outMsg.senderID = buf.ReadUint32();
        outMsg.sequence = buf.ReadUint32();
        outMsg.timestamp = buf.ReadFloat();
        // The wire timestamp feeds lag compensation and interpolation. A NaN here
        // poisons every comparison downstream, so refuse the packet at the boundary.
        if (!std::isfinite(outMsg.timestamp))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "Rejected packet with non-finite timestamp");
            return false;
        }
        uint32_t payloadLen = buf.ReadUint32();

        if (payloadLen > length || buf.GetReadPosition() > length - payloadLen)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "Payload length %u exceeds remaining packet data", payloadLen);
            return false;
        }

        if (!IsNetworkPayloadSizeValid(payloadLen))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "Payload length %u exceeds max allowed (%zu)", payloadLen,
                           MAX_NETWORK_MESSAGE_PAYLOAD_SIZE);
            return false;
        }

        outMsg.payload.resize(payloadLen);
        if (payloadLen > 0)
        {
            buf.ReadBytes(outMsg.payload.data(), payloadLen);
        }

        return true;
    }

    bool NetworkManager::SendRawTo(const std::vector<uint8_t>& data, const sockaddr_in& addr, bool localOnly)
    {
        // This is the final transmission boundary. Reacquire the recursive API
        // lock here so local-only policy is checked against the exact resolved
        // destination that sendto will consume, including delayed/retry paths.
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        if (!IsEndpointAllowed(addr))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network,
                            "Refusing transmission to an endpoint outside the captured bind policy");
            m_droppedOutgoingMessages.fetch_add(1, std::memory_order_relaxed);
            m_stats.packetsDropped++;
            return false;
        }
        if (localOnly && !IsIPv4LoopbackAddress(ntohl(addr.sin_addr.s_addr)))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network,
                            "Refusing local-only network message transmission to a non-loopback destination");
            m_droppedOutgoingMessages.fetch_add(1, std::memory_order_relaxed);
            m_stats.packetsDropped++;
            return false;
        }

        if (m_socket == INVALID_SOCKET || data.empty() || data.size() > MAX_UDP_WIRE_DATAGRAM_SIZE)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "SendRawTo failed: %s",
                           m_socket == INVALID_SOCKET
                               ? "socket not open"
                               : (data.empty() ? "empty data" : "datagram exceeds UDP wire maximum"));
            return false;
        }

        int sent = sendto(m_socket, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
                          reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));

        if (sent == SOCKET_ERROR)
        {
            m_stats.packetsDropped++;
            return false;
        }

        m_bytesSentSinceSample += static_cast<uint64_t>(sent);
        m_stats.bytesSent += static_cast<uint64_t>(sent);
        return true;
    }

    int NetworkManager::ReceiveRaw(std::vector<uint8_t>& outData, sockaddr_in& outSender)
    {
        if (m_socket == INVALID_SOCKET)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "ReceiveRaw called with invalid socket");
            return -1;
        }

        constexpr int maxPacketSize = static_cast<int>(MAX_UDP_WIRE_DATAGRAM_SIZE);
        outData.resize(MAX_UDP_WIRE_DATAGRAM_SIZE);

        socklen_t senderLen = sizeof(outSender);
        int received = recvfrom(m_socket, reinterpret_cast<char*>(outData.data()), maxPacketSize, 0,
                                reinterpret_cast<sockaddr*>(&outSender), &senderLen);

        if (received <= 0)
        {
            outData.clear();
            return received;
        }

        outData.resize(static_cast<size_t>(received));
        m_bytesReceivedSinceSample += static_cast<uint64_t>(received);
        m_stats.bytesReceived += static_cast<uint64_t>(received);
        return received;
    }

#endif // ENABLE_NETWORKING

    // --------------------------------------------------------------------------
    // Update
    // --------------------------------------------------------------------------

    void NetworkManager::SetAutoReconnect(const AutoReconnectConfig& config)
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        m_autoReconnect = config;
        m_reconnectAttempts = 0;
        m_reconnectNextRetryTime = 0.0f;
    }

    std::function<void()> NetworkManager::TryAutoReconnect(float deltaTime)
    {
        if (!m_autoReconnect.enabled || !m_wasConnected)
            return {};

        // Check if max attempts exhausted
        if (m_autoReconnect.maxAttempts > 0 && m_reconnectAttempts >= m_autoReconnect.maxAttempts)
        {
            m_autoReconnect.enabled = false;
            m_wasConnected = false;
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "AUTO-RECONNECT: Max attempts (%u) exhausted — giving up",
                            m_autoReconnect.maxAttempts);
            return m_reconnectFailedCallback;
        }

        // Wait for cooldown
        if (m_serverTime < m_reconnectNextRetryTime)
            return {};

        m_reconnectAttempts++;
        float backoff = m_autoReconnect.baseDelay * static_cast<float>(1u << std::min(m_reconnectAttempts - 1, 5u));
        backoff = std::min(backoff, m_autoReconnect.maxDelay);
        m_reconnectNextRetryTime = m_serverTime + backoff;

        SPARK_LOG_INFO(Spark::LogCategory::Network, "AUTO-RECONNECT: Attempt %u/%s to %s:%u (next retry in %.1fs)",
                       m_reconnectAttempts,
                       m_autoReconnect.maxAttempts > 0 ? std::to_string(m_autoReconnect.maxAttempts).c_str()
                                                       : "unlimited",
                       m_lastServerAddress.c_str(), m_lastServerPort, backoff);

        const NetworkEndpointPolicy reconnectPolicy = m_endpointPolicy;
        if (Connect(m_lastServerAddress, m_lastServerPort, m_lastPlayerName, reconnectPolicy))
        {
            SPARK_LOG_INFO(Spark::LogCategory::Network, "AUTO-RECONNECT: Connection attempt initiated successfully");
            // m_reconnectAttempts reset happens when we receive ConnectAccepted
        }
        return {};
    }

    void NetworkManager::Update(float deltaTime)
    {
        std::unique_lock<std::recursive_mutex> apiLock(m_apiMutex);
        uint64_t updateLifecycleEpoch = m_lifecycleEpoch;
        std::function<void()> reconnectFailedCallback;
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        SPARK_DEBUG_HOOK_SYSTEM(SystemPreUpdate, "Network", 0.0);

        SPARK_WARN_IF(Spark::LogCategory::Network, deltaTime < 0.0f, "Negative deltaTime in NetworkManager::Update");

        // Auto-reconnect: when disconnected and auto-reconnect enabled, try to reconnect
        auto currentRole = GetRole(); // thread-safe read
        bool advancedForReconnect = false;
        if (currentRole == NetworkRole::None || GetConnectionState() == ConnectionState::Disconnected)
        {
            if (m_autoReconnect.enabled && m_wasConnected)
            {
                m_serverTime += deltaTime;
                advancedForReconnect = true;
                reconnectFailedCallback = TryAutoReconnect(deltaTime);
                // A successful reconnect attempt is an Update-owned lifecycle
                // transition, not callback interference.
                updateLifecycleEpoch = m_lifecycleEpoch;
            }
            if (GetRole() == NetworkRole::None)
            {
                // Never call user code while holding the public API serialization lock.
                apiLock.unlock();
                if (reconnectFailedCallback)
                    reconnectFailedCallback();
                return;
            }
        }

        // Advance server time exactly once for active client/server updates. Idle
        // initialized managers intentionally remain at their current time.
        if (!advancedForReconnect)
            m_serverTime += deltaTime;

        // Runs the mandatory protocol handler under the API lock and hands the
        // application observer back to the caller, which invokes it with the lock
        // released — the release stays in this function body so the lock lifetime
        // is visible. Protocol state changes stay under the API lock. Application
        // code runs afterward with that lock fully released and can never replace
        // the mandatory handler that established the invariant. A terminal protocol
        // message still reaches its observer; the caller's epoch check then stops
        // every remaining item in this batch.
        auto dispatchMessage = [this](const NetworkMessage& message, bool warnIfUnhandled = true) -> MessageHandler
        {
            MessageHandler internalHandler;
            MessageHandler applicationObserver;
            {
                std::lock_guard<std::mutex> lock(m_handlerMutex);
                const uint16_t type = static_cast<uint16_t>(message.type);
                auto internalIt = m_internalHandlers.find(type);
                if (internalIt != m_internalHandlers.end())
                    internalHandler = internalIt->second;
                auto observerIt = m_handlers.find(type);
                if (observerIt != m_handlers.end())
                    applicationObserver = observerIt->second;
            }

            if (internalHandler)
            {
                internalHandler(message);
            }
            else if (!applicationObserver && warnIfUnhandled)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Network, "Unknown message type %u — dropping",
                               static_cast<unsigned>(message.type));
            }
            return applicationObserver;
        };

        // Receive from socket and enqueue. Successful admission events are
        // surfaced to application observers exactly once before initial sync,
        // allowing a DedicatedServer policy callback to kick the client first.
        const std::vector<NetworkMessage> newlyConnected = ProcessIncoming();
        for (const NetworkMessage& event : newlyConnected)
        {
            if (const MessageHandler observer = dispatchMessage(event, false))
            {
                apiLock.unlock();
                observer(event);
                apiLock.lock();
            }
            if (m_lifecycleEpoch != updateLifecycleEpoch)
                return;

            bool remainsAdmitted = false;
            {
                std::lock_guard<std::mutex> lock(m_clientsMutex);
                remainsAdmitted = m_clients.contains(event.senderID);
            }
            if (!remainsAdmitted)
                continue;

            // SendFullEntitySync owns its lock so its property callbacks can
            // completely release it (recursive unlock would leave this frame's
            // outer acquisition held).
            apiLock.unlock();
            SendFullEntitySync(event.senderID);
            apiLock.lock();
            if (m_lifecycleEpoch != updateLifecycleEpoch)
                return;
        }

        auto isDispatchableSender = [this](const NetworkMessage& message)
        {
            if (GetRole() != NetworkRole::Server)
                return true;

            std::lock_guard<std::mutex> lock(m_clientsMutex);
            return message.senderID != INVALID_CLIENT && m_clients.contains(message.senderID);
        };

        // Dispatch queued messages to handlers
        {
            std::queue<NetworkMessage> toDispatch;
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                std::swap(toDispatch, m_incomingQueue);
            }

            while (!toDispatch.empty())
            {
                const auto& msg = toDispatch.front();

                if (msg.ownerLifecycleEpoch != 0 && msg.ownerLifecycleEpoch != updateLifecycleEpoch)
                {
                    m_droppedIncomingMessages.fetch_add(1, std::memory_order_relaxed);
                    toDispatch.pop();
                    continue;
                }

                // Admission can be revoked by an earlier callback after this
                // datagram passed endpoint/session checks and was queued. Fence before ACK,
                // reliability, or application handling so a removed client
                // cannot recreate peer state or produce gameplay effects.
                if (!isDispatchableSender(msg))
                {
                    m_droppedIncomingMessages.fetch_add(1, std::memory_order_relaxed);
                    toDispatch.pop();
                    continue;
                }

                // Handle ACK messages internally (not dispatched to user handlers)
                if (msg.type == MessageType::Ack)
                {
                    HandleAck(msg);
                    m_stats.packetsReceived++;
                    toDispatch.pop();
                    continue;
                }

                // Reliability state is per sending peer: every client numbers its
                // reliable stream independently from 1, so dedup/ACK/ordered
                // bookkeeping must never be shared across senders.
                const ClientID peerKey = GetIncomingPeerKey(msg);

                // Duplicate detection for reliable messages
                if (msg.channel != ChannelType::Unreliable && msg.sequence > 0)
                {
                    PeerState& peer = GetPeerState(peerKey);
                    if (IsDuplicateSequence(peer, msg.sequence))
                    {
                        // Already received — send ACK again but don't dispatch
                        RecordReceivedSequence(peer, msg.sequence);
                        m_stats.packetsReceived++;
                        toDispatch.pop();
                        continue;
                    }
                    // ReliableOrdered recording is deferred until the message is
                    // accepted below — a sequence dropped by a full reorder buffer
                    // must not be ACKed, or the sender stops retransmitting and the
                    // ordered channel wedges on the gap forever.
                    if (msg.channel != ChannelType::ReliableOrdered)
                        RecordReceivedSequence(peer, msg.sequence);
                }

                // Ordered delivery: buffer out-of-order ReliableOrdered messages
                if (msg.channel == ChannelType::ReliableOrdered && msg.sequence > 0)
                {
                    PeerState& peer = GetPeerState(peerKey);
                    if (msg.sequence != peer.expectedOrderedSequence)
                    {
                        // Bound the reorder buffer so a peer that never sends the
                        // expected sequence (always leaving a gap) cannot grow this
                        // map without limit — a remote memory-exhaustion DoS.
                        // Overwriting an already-buffered sequence is fine; only a
                        // *new* out-of-order sequence past the cap is dropped.
                        if (peer.orderedBuffer.size() >= kMaxQueuedMessages &&
                            !peer.orderedBuffer.contains(msg.sequence))
                        {
                            m_droppedIncomingMessages.fetch_add(1, std::memory_order_relaxed);
                            SPARK_LOG_WARN(Spark::LogCategory::Network,
                                           "Ordered reorder buffer full (%zu) — dropping out-of-order sequence %u",
                                           peer.orderedBuffer.size(), static_cast<unsigned>(msg.sequence));
                            toDispatch.pop();
                            continue;
                        }
                        // Buffer for later delivery
                        RecordReceivedSequence(peer, msg.sequence);
                        peer.orderedBuffer[msg.sequence] = msg;
                        m_stats.packetsReceived++;
                        toDispatch.pop();
                        continue;
                    }
                    // This is the expected sequence — deliver it, then flush buffer
                    RecordReceivedSequence(peer, msg.sequence);
                    peer.expectedOrderedSequence++;
                }

                if (const MessageHandler observer = dispatchMessage(msg))
                {
                    apiLock.unlock();
                    observer(msg);
                    apiLock.lock();
                }
                if (m_lifecycleEpoch != updateLifecycleEpoch)
                    return;

                // Check channel before popping — msg reference is invalidated by pop()
                bool wasReliableOrdered = (msg.channel == ChannelType::ReliableOrdered);
                m_stats.packetsReceived++;
                toDispatch.pop();

                // After delivering an ordered message, flush any buffered
                // successors from the same peer (by key — the handler above may
                // have mutated m_peers, invalidating references)
                if (wasReliableOrdered)
                {
                    NetworkMessage orderedMessage;
                    while (PopNextOrderedMessage(peerKey, orderedMessage))
                    {
                        if (orderedMessage.ownerLifecycleEpoch != 0 &&
                            orderedMessage.ownerLifecycleEpoch != updateLifecycleEpoch)
                        {
                            m_droppedIncomingMessages.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }
                        if (!isDispatchableSender(orderedMessage))
                        {
                            m_droppedIncomingMessages.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }
                        if (const MessageHandler observer = dispatchMessage(orderedMessage))
                        {
                            apiLock.unlock();
                            observer(orderedMessage);
                            apiLock.lock();
                        }
                        if (m_lifecycleEpoch != updateLifecycleEpoch)
                            return;
                    }
                }
            }
        }

        // Send periodic ACK packets
        m_ackSendTimer += deltaTime;
        if (m_ackSendTimer >= ACK_SEND_INTERVAL)
        {
            m_ackSendTimer = 0.0f;
            SendAckPacket();
            PruneReceivedSequences();
        }

        // UpdateReplication owns its lock so its property serializers can
        // completely release it (recursive unlock would leave this frame's
        // outer acquisition held).
        apiLock.unlock();
        const bool replicationLifecycleUnchanged = UpdateReplication(deltaTime, updateLifecycleEpoch);
        apiLock.lock();
        if (!replicationLifecycleUnchanged || m_lifecycleEpoch != updateLifecycleEpoch)
            return;
        UpdateHeartbeat(deltaTime);

        // Check for timed-out clients (server) or server timeout (client)
        std::vector<ClientID> timedOutClients = CheckConnectionTimeouts();
        TimeoutHandler timeoutHandler = m_timeoutHandler;

        // Update bandwidth stats every second
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastBandwidthSample).count();
        if (elapsed >= 1000)
        {
            float seconds = static_cast<float>(elapsed) / 1000.0f;
            m_stats.bandwidthUp = static_cast<float>(m_bytesSentSinceSample) / (1024.0f * seconds);
            m_stats.bandwidthDown = static_cast<float>(m_bytesReceivedSinceSample) / (1024.0f * seconds);
            m_bytesSentSinceSample = 0;
            m_bytesReceivedSinceSample = 0;
            m_lastBandwidthSample = now;
        }

        ProcessOutgoing();
        SPARK_DEBUG_HOOK_SYSTEM(SystemPostUpdate, "Network", 0.0);

        // Connection state and transport cleanup are complete before external
        // timeout/reconnect notifications run. Never call user code while
        // holding the public API serialization lock.
        apiLock.unlock();
        if (timeoutHandler)
        {
            for (ClientID id : timedOutClients)
            {
                timeoutHandler(id);
                apiLock.lock();
                const bool lifecycleUnchanged = (m_lifecycleEpoch == updateLifecycleEpoch);
                apiLock.unlock();
                if (!lifecycleUnchanged)
                    return;
            }
        }
        if (reconnectFailedCallback)
            reconnectFailedCallback();
    }

    // --------------------------------------------------------------------------
    // Client Input
    // --------------------------------------------------------------------------

    void NetworkManager::SendClientInput(const ClientInputState& input)
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        if (GetRole() != NetworkRole::Client)
            return;

        ClientInputState timestamped = input;
        timestamped.inputSequence = m_inputSequence++;
        timestamped.timestamp = m_serverTime;

        {
            std::lock_guard<std::mutex> lock(m_inputMutex);
            m_inputHistory.push_back(timestamped);

            // Cap history size
            if (m_inputHistory.size() > 120)
                m_inputHistory.erase(m_inputHistory.begin());
        }

        NetworkMessage msg;
        msg.type = MessageType::ClientInput;
        msg.channel = ChannelType::Unreliable;
        msg.senderID = m_localClientID;
        NetBuffer buf;
        buf.WriteUint32(timestamped.inputSequence);
        buf.WriteFloat(timestamped.moveForward);
        buf.WriteFloat(timestamped.moveRight);
        buf.WriteFloat(timestamped.lookYaw);
        buf.WriteFloat(timestamped.lookPitch);
        buf.WriteUint8(static_cast<uint8_t>((timestamped.jump ? 1 : 0) | (timestamped.fire ? 2 : 0) |
                                            (timestamped.reload ? 4 : 0) | (timestamped.sprint ? 8 : 0) |
                                            (timestamped.crouch ? 16 : 0)));
        buf.WriteFloat(timestamped.deltaTime);
        msg.payload = buf.GetData();
        SendMessage(msg);
    }

    // --------------------------------------------------------------------------
    // RTT Estimation (Jacobson/Karels)
    // --------------------------------------------------------------------------

    void NetworkManager::UpdateRTTEstimate(float sampleRTT)
    {
        if (!m_rttInitialized)
        {
            m_smoothedRTT = sampleRTT;
            m_rttVariance = sampleRTT / 2.0f;
            m_rttInitialized = true;
        }
        else
        {
            // RFC 6298: RTTVAR = (1 - beta) * RTTVAR + beta * |SRTT - R|
            //           SRTT  = (1 - alpha) * SRTT  + alpha * R
            constexpr float alpha = 0.125f;
            constexpr float beta = 0.25f;
            float diff = std::abs(m_smoothedRTT - sampleRTT);
            m_rttVariance = (1.0f - beta) * m_rttVariance + beta * diff;
            m_smoothedRTT = (1.0f - alpha) * m_smoothedRTT + alpha * sampleRTT;
        }

        // Update stats
        m_stats.ping = m_smoothedRTT * 1000.0f;
        m_stats.jitter = m_rttVariance * 1000.0f;
        SPARK_LOG_DEBUG(Spark::LogCategory::Network, "RTT estimate: %.1fms (jitter: %.1fms)", m_stats.ping,
                        m_stats.jitter);
    }

    float NetworkManager::GetRetransmitTimeout() const
    {
        if (!m_rttInitialized)
            return m_reliableRetransmitInterval;

        // RTO = SRTT + max(G, 4 * RTTVAR), clamped to [0.2s, 8s]
        float rto = m_smoothedRTT + 4.0f * m_rttVariance;
        return (std::max)(0.2f, (std::min)(rto, 8.0f));
    }

    // --------------------------------------------------------------------------
    // Server-side hit validation (lag compensation integration)
    // --------------------------------------------------------------------------

    NetworkManager::HitValidationResult NetworkManager::ValidateHit(float clientTimestamp, float halfRTT,
                                                                    const XMFLOAT3& rayOrigin,
                                                                    const XMFLOAT3& rayDirection, float maxDistance)
    {
        std::lock_guard<std::recursive_mutex> apiLock(m_apiMutex);
        HitValidationResult result;

        // Every scalar below is client-supplied. Unchecked, a client can pass
        // maxDistance = 1e30 to reach anything in the rewound snapshot, NaN to make
        // every comparison false (a silent no-hit), or a halfRTT of its choosing to
        // rewind arbitrarily far. This is the classic lag-compensation abuse surface.
        if (!std::isfinite(clientTimestamp) || !std::isfinite(halfRTT) || !std::isfinite(maxDistance) ||
            !std::isfinite(rayOrigin.x) || !std::isfinite(rayOrigin.y) || !std::isfinite(rayOrigin.z) ||
            !std::isfinite(rayDirection.x) || !std::isfinite(rayDirection.y) || !std::isfinite(rayDirection.z))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "ValidateHit: rejected non-finite hit request");
            return result;
        }

        const float directionLengthSq =
            rayDirection.x * rayDirection.x + rayDirection.y * rayDirection.y + rayDirection.z * rayDirection.z;
        if (directionLengthSq < MIN_HIT_DIRECTION_LENGTH_SQ)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "ValidateHit: rejected degenerate ray direction");
            return result;
        }

        halfRTT = std::clamp(halfRTT, 0.0f, MAX_LAG_COMPENSATION_SECONDS);
        maxDistance = std::clamp(maxDistance, 0.0f, MAX_HIT_VALIDATION_DISTANCE);

        const float rewindTime = clientTimestamp - halfRTT;

        // clientTimestamp is client-supplied too, and clamping halfRTT alone does not
        // bound it. RewindToTime falls back to "whichever snapshot we have" when the
        // target lies outside the retained history, so an unbounded timestamp still
        // lets a client select the oldest or newest snapshot at will — a shot fired
        // against a world state the client never saw. Bound the rewind target against
        // the server's own newest snapshot and the compensator's retained window.
        float newestServerTime = 0.0f;
        if (!m_lagCompensator.GetNewestSnapshotTime(newestServerTime))
            return result;

        // The retained window is the compensator's own setting (SetMaxHistoryDuration
        // is public), not a duplicated constant: anything older than this simply is
        // not in the history any more.
        const float configuredWindow = m_lagCompensator.GetMaxHistoryDuration();
        const float historyWindow = std::isfinite(configuredWindow) ? (std::max)(0.0f, configuredWindow) : 0.0f;
        const float oldestValidTime = newestServerTime - historyWindow;
        if (rewindTime > newestServerTime || rewindTime < oldestValidTime)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network,
                           "ValidateHit: rejected out-of-window rewind target %.3f (server window [%.3f, %.3f])",
                           static_cast<double>(rewindTime), static_cast<double>(oldestValidTime),
                           static_cast<double>(newestServerTime));
            return result;
        }

        // Rewind to the client's perceived time
        HistorySnapshot rewoundSnapshot;
        if (!m_lagCompensator.RewindToTime(rewindTime, rewoundSnapshot))
            return result;

        // Ray-AABB intersection against rewound hitboxes
        XMVECTOR origin = XMLoadFloat3(&rayOrigin);
        XMVECTOR direction = XMVector3Normalize(XMLoadFloat3(&rayDirection));

        float closestDist = maxDistance;
        for (const auto& entity : rewoundSnapshot.entities)
        {
            // Ray-AABB slab test
            XMVECTOR bmin = XMLoadFloat3(&entity.boundsMin);
            XMVECTOR bmax = XMLoadFloat3(&entity.boundsMax);

            XMFLOAT3 dir;
            XMStoreFloat3(&dir, direction);
            float invDirX = (dir.x != 0.0f) ? (1.0f / dir.x) : 1e30f;
            float invDirY = (dir.y != 0.0f) ? (1.0f / dir.y) : 1e30f;
            float invDirZ = (dir.z != 0.0f) ? (1.0f / dir.z) : 1e30f;

            XMFLOAT3 orig;
            XMStoreFloat3(&orig, origin);
            XMFLOAT3 bminF, bmaxF;
            XMStoreFloat3(&bminF, bmin);
            XMStoreFloat3(&bmaxF, bmax);

            float t1 = (bminF.x - orig.x) * invDirX;
            float t2 = (bmaxF.x - orig.x) * invDirX;
            float tmin = (std::min)(t1, t2);
            float tmax = (std::max)(t1, t2);

            t1 = (bminF.y - orig.y) * invDirY;
            t2 = (bmaxF.y - orig.y) * invDirY;
            tmin = (std::max)(tmin, (std::min)(t1, t2));
            tmax = (std::min)(tmax, (std::max)(t1, t2));

            t1 = (bminF.z - orig.z) * invDirZ;
            t2 = (bmaxF.z - orig.z) * invDirZ;
            tmin = (std::max)(tmin, (std::min)(t1, t2));
            tmax = (std::min)(tmax, (std::max)(t1, t2));

            if (tmax >= tmin && tmax >= 0.0f && tmin < closestDist)
            {
                float hitT = (tmin >= 0.0f) ? tmin : tmax;
                closestDist = hitT;
                result.hit = true;
                result.entityID = entity.networkID;
                XMStoreFloat3(&result.hitPoint, XMVectorAdd(origin, XMVectorScale(direction, hitT)));
            }
        }

        return result;
    }

} // namespace Spark::Net
