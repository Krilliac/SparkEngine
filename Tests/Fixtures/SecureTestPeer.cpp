/**
 * @file SecureTestPeer.cpp
 * @brief Raw loopback peers that speak the NET-100 v2 secure transport (see SecureTestPeer.h)
 */

#include "SecureTestPeer.h"
#include "NetworkTestSecurity.h"

#ifdef ENABLE_NETWORKING

#include <algorithm>
#include <array>
#include <functional>
#include <thread>

#ifndef SPARK_PLATFORM_WINDOWS
#include <fcntl.h>
#endif

using namespace Spark::Net;

namespace SparkTestFixtures
{
    namespace
    {
        constexpr std::array<uint8_t, 1> kSealedAad = {kFrameSealed};

        bool IsConnectedOn(const NetworkManager& server, ClientID id)
        {
            const auto clients = server.GetClients();
            const auto it = clients.find(id);
            return it != clients.end() && it->second.state == ConnectionState::Connected;
        }

        std::vector<uint8_t> SealWith(SecureChannel* channel, std::span<const uint8_t> wire)
        {
            std::vector<uint8_t> frame;
            if (!channel)
            {
                return frame;
            }
            std::vector<uint8_t> sealed;
            if (!channel->Seal(wire, sealed, kSealedAad))
            {
                return frame;
            }
            frame.reserve(sealed.size() + 1);
            frame.push_back(kFrameSealed);
            frame.insert(frame.end(), sealed.begin(), sealed.end());
            return frame;
        }

        std::optional<WireMessage> OpenWith(SecureChannel* channel, std::span<const uint8_t> datagram)
        {
            if (datagram.empty())
            {
                return std::nullopt;
            }
            if (datagram[0] == kFrameHandshake)
            {
                return ParseWire(datagram.subspan(1));
            }
            if (datagram[0] != kFrameSealed || !channel)
            {
                return std::nullopt;
            }
            std::vector<uint8_t> inner;
            if (channel->Open(datagram.subspan(1), inner, kSealedAad) != OpenResult::Ok)
            {
                return std::nullopt;
            }
            return ParseWire(inner);
        }
    } // namespace

    std::vector<uint8_t> BuildWire(MessageType type, std::span<const uint8_t> payload, ChannelType channel,
                                   uint32_t sequence, ClientID sender, float timestamp, uint32_t orderedSequence)
    {
        NetBuffer buf;
        buf.WriteUint32(kWireMagic);
        buf.WriteUint16(static_cast<uint16_t>(type));
        buf.WriteUint8(static_cast<uint8_t>(channel));
        buf.WriteUint32(sender);
        buf.WriteUint32(sequence);
        buf.WriteFloat(timestamp);
        buf.WriteUint32(static_cast<uint32_t>(payload.size()));
        if (channel == ChannelType::ReliableOrdered)
        {
            buf.WriteUint32(orderedSequence);
        }
        if (!payload.empty())
        {
            buf.WriteBytes(payload.data(), payload.size());
        }
        return buf.GetData();
    }

    std::optional<WireMessage> ParseWire(std::span<const uint8_t> wire)
    {
        if (wire.size() < kWireHeaderSize)
        {
            return std::nullopt;
        }
        NetBuffer buf;
        buf.WriteBytes(wire.data(), wire.size());
        if (buf.ReadUint32() != kWireMagic)
        {
            return std::nullopt;
        }
        WireMessage message;
        message.type = static_cast<MessageType>(buf.ReadUint16());
        message.channel = static_cast<ChannelType>(buf.ReadUint8());
        message.sender = buf.ReadUint32();
        message.sequence = buf.ReadUint32();
        message.timestamp = buf.ReadFloat();
        const uint32_t payloadSize = buf.ReadUint32();
        if (message.channel == ChannelType::ReliableOrdered)
        {
            message.orderedSequence = buf.ReadUint32();
        }
        const size_t headerSize = buf.GetReadPosition();
        if (buf.HasError() || wire.size() != headerSize + payloadSize)
        {
            return std::nullopt;
        }
        message.payload.assign(wire.begin() + static_cast<std::ptrdiff_t>(headerSize), wire.end());
        return message;
    }

    std::vector<uint8_t> HandshakeFrame(std::span<const uint8_t> wire)
    {
        std::vector<uint8_t> frame;
        frame.reserve(wire.size() + 1);
        frame.push_back(kFrameHandshake);
        frame.insert(frame.end(), wire.begin(), wire.end());
        return frame;
    }

    std::vector<uint8_t> EncodeName(const std::string& name)
    {
        NetBuffer buf;
        buf.WriteString(name);
        return buf.GetData();
    }

    std::vector<uint8_t> FrameForSend(SecureChannel* channel, std::span<const uint8_t> wire)
    {
        if (wire.size() >= kWireHeaderSize)
        {
            const auto type =
                static_cast<MessageType>(static_cast<uint16_t>(wire[4]) | static_cast<uint16_t>(wire[5] << 8));
            if (IsHandshakeMessage(type))
            {
                return HandshakeFrame(wire);
            }
        }
        return SealWith(channel, wire);
    }

    std::optional<std::vector<uint8_t>> OpenFrame(SecureChannel* channel, std::span<const uint8_t> datagram)
    {
        if (datagram.empty())
        {
            return std::nullopt;
        }
        if (datagram[0] == kFrameHandshake)
        {
            return std::vector<uint8_t>(datagram.begin() + 1, datagram.end());
        }
        if (datagram[0] != kFrameSealed || !channel)
        {
            return std::nullopt;
        }
        std::vector<uint8_t> inner;
        if (channel->Open(datagram.subspan(1), inner, kSealedAad) != OpenResult::Ok)
        {
            return std::nullopt;
        }
        return inner;
    }

    std::optional<RawHandshakeResult> RawHandshake(NetworkManager& server,
                                                   const std::function<bool(std::span<const uint8_t>)>& send,
                                                   const std::function<std::optional<std::vector<uint8_t>>()>& receive,
                                                   const std::string& name, std::chrono::milliseconds window)
    {
        ClientHandshake handshake;
        auto hello = handshake.Begin(NETWORK_PROTOCOL_VERSION);
        if (!hello || !send(HandshakeFrame(BuildWire(MessageType::Connect, *hello, ChannelType::Reliable))))
        {
            return std::nullopt;
        }

        // A ConnectRejected ends the handshake just as it ends a real client's. Stop pumping on it:
        // every PumpUntil step advances the singleton's clock by a fixed 16 ms while sleeping only
        // ~2 ms, so waiting out the window after a refusal would age every other client on the
        // server by ~10 simulated seconds and time out the ones that are silent in the test.
        std::optional<WireMessage> accepted;
        bool rejected = false;
        PumpUntil(
            server,
            [&]
            {
                while (auto datagram = receive())
                {
                    if (!datagram->empty() && (*datagram)[0] == kFrameHandshake)
                    {
                        auto message = ParseWire(std::span(*datagram).subspan(1));
                        if (message && message->type == MessageType::ConnectAccepted)
                        {
                            accepted = std::move(message);
                        }
                        else if (message && message->type == MessageType::ConnectRejected)
                        {
                            rejected = true;
                        }
                    }
                }
                return accepted.has_value() || rejected;
            },
            window);
        constexpr size_t kPrefix = CONNECT_ACCEPT_PREFIX_SIZE;
        if (!accepted || accepted->payload.size() != kPrefix + SERVER_HELLO_SIZE)
        {
            return std::nullopt;
        }

        RawHandshakeResult result;
        NetBuffer buf;
        buf.WriteBytes(accepted->payload.data(), accepted->payload.size());
        result.id = buf.ReadUint32();
        const std::span<const uint8_t> acceptPayload(accepted->payload);
        auto channel = handshake.Finish(acceptPayload.first(kPrefix), acceptPayload.subspan(kPrefix),
                                        TestServerIdentity().publicKey);
        if (!channel)
        {
            return std::nullopt;
        }
        result.channel = std::move(*channel);

        const auto finished =
            BuildWire(MessageType::ClientFinished, EncodeName(name), ChannelType::Reliable, 0, result.id);
        const auto frame = SealWith(result.channel.get(), finished);
        if (frame.empty() || !send(frame))
        {
            return std::nullopt;
        }
        const ClientID id = result.id;
        if (!PumpUntil(server, [&] { return IsConnectedOn(server, id); }, window))
        {
            return std::nullopt;
        }
        return result;
    }

    std::vector<uint8_t> LegacyConnectPayload(const std::string& name)
    {
        NetBuffer buf;
        buf.WriteUint32(NETWORK_HANDSHAKE_MAGIC);
        buf.WriteUint16(NETWORK_PROTOCOL_VERSION);
        buf.WriteString(name);
        return buf.GetData();
    }

    bool WireAdapter::Send(std::span<const uint8_t> message, const RawSend& send, const RawReceive& receive)
    {
        const auto parsed = ParseWire(message);
        if (parsed && parsed->type == MessageType::Connect)
        {
            if (m_channel)
            {
                return true; // already in session: a repeated Connect is a no-op, as on the real server
            }
            std::string name = "Player";
            if (parsed->payload.size() > 6)
            {
                NetBuffer buf;
                buf.WriteBytes(parsed->payload.data() + 6, parsed->payload.size() - 6);
                std::string supplied = buf.ReadString();
                if (!buf.HasError() && !supplied.empty())
                {
                    name = std::move(supplied);
                }
            }
            auto session = RawHandshake(NetworkManager::GetInstance(), send, receive, name);
            if (!session)
            {
                return false;
            }
            m_id = session->id;
            m_channel = std::move(session->channel);
            return true;
        }
        const auto frame = FrameForSend(m_channel.get(), message);
        // Without a session (or for bytes that are not a message) the datagram goes out as the
        // test built it; the server then drops it as unframed plaintext, exactly as it would
        // drop the same bytes from a real attacker.
        return send(frame.empty() ? message : std::span<const uint8_t>(frame));
    }

    std::optional<std::vector<uint8_t>> WireAdapter::Open(std::span<const uint8_t> datagram)
    {
        return OpenFrame(m_channel.get(), datagram);
    }

    bool ContainsBytes(std::span<const uint8_t> haystack, std::span<const uint8_t> needle)
    {
        if (needle.empty() || haystack.size() < needle.size())
        {
            return false;
        }
        return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
    }

    // ============================================================================
    // LoopbackSocket
    // ============================================================================

    LoopbackSocket::LoopbackSocket()
    {
#ifdef SPARK_PLATFORM_WINDOWS
        // Hold a Winsock reference of our own: the singleton's Shutdown() must not
        // pull Winsock out from under this socket. WSAStartup is reference-counted.
        WSADATA winsockData{};
        m_winsockStarted = WSAStartup(MAKEWORD(2, 2), &winsockData) == 0;
        if (!m_winsockStarted)
        {
            return;
        }
#endif
        m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_socket == INVALID_SOCKET)
        {
            return;
        }
        // The engine's socket buffer sizes: macOS otherwise refuses to send a datagram over 9 KiB,
        // so this peer could not deliver the large frames an engine peer can.
        const int sendBufferSize = static_cast<int>(Spark::Net::NETWORK_SOCKET_SEND_BUFFER_SIZE);
        const int receiveBufferSize = static_cast<int>(Spark::Net::NETWORK_SOCKET_RECEIVE_BUFFER_SIZE);
        const bool buffersSized =
            setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBufferSize),
                       sizeof(sendBufferSize)) == 0 &&
            setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receiveBufferSize),
                       sizeof(receiveBufferSize)) == 0;
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = 0;
        m_ready = buffersSized && bind(m_socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0;
#ifdef SPARK_PLATFORM_WINDOWS
        u_long nonBlocking = 1;
        m_ready = m_ready && ioctlsocket(m_socket, FIONBIO, &nonBlocking) == 0;
#else
        const int flags = fcntl(m_socket, F_GETFL, 0);
        m_ready = m_ready && flags >= 0 && fcntl(m_socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    }

    LoopbackSocket::~LoopbackSocket()
    {
        if (m_socket != INVALID_SOCKET)
        {
            closesocket(m_socket);
        }
#ifdef SPARK_PLATFORM_WINDOWS
        if (m_winsockStarted)
        {
            WSACleanup();
        }
#endif
    }

    uint16_t LoopbackSocket::Port() const
    {
        sockaddr_in local{};
        socklen_t length = sizeof(local);
        if (getsockname(m_socket, reinterpret_cast<sockaddr*>(&local), &length) != 0)
        {
            return 0;
        }
        return ntohs(local.sin_port);
    }

    bool LoopbackSocket::SendTo(uint16_t port, std::span<const uint8_t> datagram) const
    {
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        target.sin_port = htons(port);
        return SendTo(target, datagram);
    }

    bool LoopbackSocket::SendTo(const sockaddr_in& target, std::span<const uint8_t> datagram) const
    {
        const int sent =
            sendto(m_socket, reinterpret_cast<const char*>(datagram.data()), static_cast<int>(datagram.size()), 0,
                   reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        return sent == static_cast<int>(datagram.size());
    }

    std::optional<std::vector<uint8_t>> LoopbackSocket::Receive()
    {
        std::array<uint8_t, 65536> buffer{};
        sockaddr_in from{};
        socklen_t fromLength = sizeof(from);
        const int received = recvfrom(m_socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()),
                                      0, reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (received <= 0)
        {
            return std::nullopt;
        }
        m_lastSender = from;
        std::vector<uint8_t> datagram(buffer.begin(), buffer.begin() + received);
        m_captured.push_back(datagram);
        return datagram;
    }

    // ============================================================================
    // SecureRawClient
    // ============================================================================

    std::vector<uint8_t> SecureRawClient::BeginConnect(uint16_t version)
    {
        auto hello = m_handshake.Begin(version);
        if (!hello)
        {
            return {};
        }
        return HandshakeFrame(BuildWire(MessageType::Connect, *hello, ChannelType::Reliable));
    }

    bool SecureRawClient::FinishFromAccepted(const WireMessage& accepted, const ServerPublicKey& pinned)
    {
        constexpr size_t kPrefix = CONNECT_ACCEPT_PREFIX_SIZE;
        if (accepted.type != MessageType::ConnectAccepted || accepted.payload.size() != kPrefix + SERVER_HELLO_SIZE)
        {
            return false;
        }
        NetBuffer buf;
        buf.WriteBytes(accepted.payload.data(), accepted.payload.size());
        const ClientID id = buf.ReadUint32();
        const std::span<const uint8_t> acceptPayload(accepted.payload);
        auto channel = m_handshake.Finish(acceptPayload.first(kPrefix), acceptPayload.subspan(kPrefix), pinned);
        if (!channel)
        {
            return false;
        }
        m_channel = std::move(*channel);
        m_id = id;
        return true;
    }

    bool SecureRawClient::Connect(NetworkManager& server, const std::string& name, const ServerPublicKey* pinned,
                                  std::chrono::milliseconds window)
    {
        const auto frame = BeginConnect();
        m_serverPort = server.GetBoundPort();
        if (frame.empty() || !m_socket.IsReady() || !m_socket.SendTo(m_serverPort, frame))
        {
            return false;
        }
        const auto accepted = AwaitType(server, MessageType::ConnectAccepted, window);
        if (!accepted || !FinishFromAccepted(*accepted, pinned ? *pinned : TestServerIdentity().publicKey))
        {
            return false;
        }
        const auto finished = BuildWire(MessageType::ClientFinished, EncodeName(name), ChannelType::Reliable, 0, m_id);
        if (!SendSealed(finished))
        {
            return false;
        }
        return PumpUntil(server, [&] { return IsConnectedOn(server, m_id); }, window);
    }

    std::vector<uint8_t> SecureRawClient::Seal(std::span<const uint8_t> wire)
    {
        return SealWith(m_channel.get(), wire);
    }

    bool SecureRawClient::SendSealed(std::span<const uint8_t> wire, uint16_t port)
    {
        const auto frame = Seal(wire);
        return !frame.empty() && m_socket.SendTo(port != 0 ? port : m_serverPort, frame);
    }

    std::optional<WireMessage> SecureRawClient::Open(std::span<const uint8_t> datagram)
    {
        return OpenWith(m_channel.get(), datagram);
    }

    std::optional<WireMessage> SecureRawClient::AwaitType(NetworkManager& manager, MessageType type,
                                                          std::chrono::milliseconds window)
    {
        const auto deadline = std::chrono::steady_clock::now() + window;
        while (std::chrono::steady_clock::now() < deadline)
        {
            manager.Update(0.016f);
            while (auto datagram = m_socket.Receive())
            {
                auto message = Open(*datagram);
                if (message && message->type == type)
                {
                    return message;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return std::nullopt;
    }

    // ============================================================================
    // SecureRawServer
    // ============================================================================

    std::optional<std::vector<uint8_t>> SecureRawServer::AwaitConnect(NetworkManager& client,
                                                                      std::chrono::milliseconds window)
    {
        const auto deadline = std::chrono::steady_clock::now() + window;
        while (std::chrono::steady_clock::now() < deadline)
        {
            client.Update(0.016f);
            while (auto datagram = m_socket.Receive())
            {
                if (datagram->empty() || (*datagram)[0] != kFrameHandshake)
                {
                    continue;
                }
                auto message = ParseWire(std::span(*datagram).subspan(1));
                if (message && message->type == MessageType::Connect)
                {
                    m_clientHello = message->payload;
                    return m_clientHello;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return std::nullopt;
    }

    std::vector<uint8_t> SecureRawServer::AcceptPrefix(ClientID id, uint16_t echoedVersion)
    {
        NetBuffer buf;
        buf.WriteUint32(id);
        buf.WriteFloat(0.0f);
        buf.WriteUint16(echoedVersion);
        return buf.GetData();
    }

    std::vector<uint8_t> SecureRawServer::AcceptPayload(ClientID id, uint16_t echoedVersion,
                                                        std::span<const uint8_t> serverHello)
    {
        std::vector<uint8_t> payload = AcceptPrefix(id, echoedVersion);
        payload.insert(payload.end(), serverHello.begin(), serverHello.end());
        return payload;
    }

    std::vector<uint8_t> SecureRawServer::Accept(ClientID id, const ServerIdentity* identity, uint16_t echoedVersion)
    {
        auto response = RespondToClientHello(m_clientHello, AcceptPrefix(id, echoedVersion),
                                             identity ? *identity : TestServerIdentity());
        if (!response)
        {
            return {};
        }
        m_channel = std::move(response->channel);
        const auto payload = AcceptPayload(id, echoedVersion, response->serverHello);
        const auto frame = HandshakeFrame(BuildWire(MessageType::ConnectAccepted, payload, ChannelType::Unreliable));
        m_socket.SendTo(m_socket.LastSender(), frame);
        return frame;
    }

    std::vector<uint8_t> SecureRawServer::Seal(std::span<const uint8_t> wire)
    {
        return SealWith(m_channel.get(), wire);
    }

    bool SecureRawServer::SendSealed(std::span<const uint8_t> wire)
    {
        const auto frame = Seal(wire);
        return !frame.empty() && m_socket.SendTo(m_socket.LastSender(), frame);
    }

    std::optional<WireMessage> SecureRawServer::Open(std::span<const uint8_t> datagram)
    {
        return OpenWith(m_channel.get(), datagram);
    }

    std::optional<WireMessage> SecureRawServer::AwaitType(NetworkManager& manager, MessageType type,
                                                          std::chrono::milliseconds window)
    {
        const auto deadline = std::chrono::steady_clock::now() + window;
        while (std::chrono::steady_clock::now() < deadline)
        {
            manager.Update(0.016f);
            while (auto datagram = m_socket.Receive())
            {
                auto message = Open(*datagram);
                if (message && message->type == type)
                {
                    return message;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return std::nullopt;
    }
} // namespace SparkTestFixtures

#endif // ENABLE_NETWORKING
