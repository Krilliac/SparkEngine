/**
 * @file SecureTestPeer.h
 * @brief NET-100 test fixture: a raw loopback UDP peer that speaks the v2 secure transport
 *
 * The production NetworkManager only exchanges handshake frames (Connect,
 * ConnectAccepted, ConnectRejected) in plaintext; every other datagram is a
 * SecureChannel packet. Tests that drive the singleton from a hand-written peer
 * use this fixture to frame, seal and open datagrams with the shipped
 * SecureHandshake / SecureChannel code, while still controlling every byte of
 * the inner message (docs/specs/networking-wire-format.md).
 *
 * SecureRawClient stands in for a client of the singleton server; SecureRawServer
 * stands in for a server the singleton client connects to (signing with
 * TestServerIdentity() unless told otherwise). Both keep every raw datagram
 * they receive so capture tests can scan the wire.
 *
 * Thread affinity: test thread. Allocation: per datagram (test code only).
 */

#pragma once

// Networking-only fixture: sockets and the secure transport exist only when
// ENABLE_NETWORKING is defined (the stable-v1 shipping profile turns it off).
// Every consumer guards its tests the same way.
#ifdef ENABLE_NETWORKING

#include "Engine/Networking/NetworkManager.h"
#include "Engine/Networking/SecureHandshake.h"
#include "NetworkTestSecurity.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace SparkTestFixtures
{
    constexpr uint8_t kFrameHandshake = 0x01;
    constexpr uint8_t kFrameSealed = 0x02;
    constexpr uint32_t kWireMagic = 0x5350524B; // "SPRK" inner message header
    constexpr size_t kWireHeaderSize = 23;      // plus 4 (ordered sequence) on ReliableOrdered

    /** @brief One decoded inner message (the plaintext of a frame). */
    struct WireMessage
    {
        Spark::Net::MessageType type = Spark::Net::MessageType::UserDefined;
        Spark::Net::ChannelType channel = Spark::Net::ChannelType::Unreliable;
        Spark::Net::ClientID sender = Spark::Net::INVALID_CLIENT;
        uint32_t sequence = 0;
        uint32_t orderedSequence = 0; ///< ReliableOrdered only
        float timestamp = 0.0f;
        std::vector<uint8_t> payload;
    };

    /**
     * @brief Serialize an inner message exactly as NetworkManager::SerializeMessage does
     *
     * @p orderedSequence is written only for ReliableOrdered (protocol v3); 0 delivers unordered.
     */
    std::vector<uint8_t> BuildWire(Spark::Net::MessageType type, std::span<const uint8_t> payload,
                                   Spark::Net::ChannelType channel = Spark::Net::ChannelType::Reliable,
                                   uint32_t sequence = 0, Spark::Net::ClientID sender = Spark::Net::INVALID_CLIENT,
                                   float timestamp = 0.0f, uint32_t orderedSequence = 0);

    /** @brief Parse an inner message; nullopt when the header or length is wrong. */
    std::optional<WireMessage> ParseWire(std::span<const uint8_t> wire);

    /**
     * @brief A handshake frame, the kFrameHandshake byte followed by the wire bytes: the only
     *        plaintext frame the transport accepts.
     */
    std::vector<uint8_t> HandshakeFrame(std::span<const uint8_t> wire);

    /** @brief A player name as NetBuffer::WriteString encodes it (the ClientFinished payload). */
    std::vector<uint8_t> EncodeName(const std::string& name);

    /**
     * @brief Frame a serialized message for sending as NetworkManager would
     *
     * Handshake types get a plaintext Handshake frame; everything else is sealed with
     * @p channel (an empty vector when there is no channel or sealing fails).
     */
    std::vector<uint8_t> FrameForSend(Spark::Net::SecureChannel* channel, std::span<const uint8_t> wire);

    /**
     * @brief The inner message bytes of a received datagram
     *
     * Handshake frames are returned verbatim (minus the kind byte), sealed frames are opened
     * with @p channel; anything else (unframed, unknown kind, failed Open) is nullopt.
     */
    std::optional<std::vector<uint8_t>> OpenFrame(Spark::Net::SecureChannel* channel,
                                                  std::span<const uint8_t> datagram);

    /** @brief A completed client-side raw handshake. */
    struct RawHandshakeResult
    {
        Spark::Net::ClientID id = Spark::Net::INVALID_CLIENT;
        std::unique_ptr<Spark::Net::SecureChannel> channel;
    };

    /**
     * @brief Run the v2 client handshake over a test's own socket
     *
     * Sends a framed Connect through @p send, pumps @p server while polling @p receive for the
     * ConnectAccepted, verifies it against TestServerIdentity(), sends a sealed ClientFinished
     * carrying @p name (sequence 0, so the test's own reliable numbering is untouched), and waits
     * until the server lists the client as Connected.
     */
    std::optional<RawHandshakeResult> RawHandshake(Spark::Net::NetworkManager& server,
                                                   const std::function<bool(std::span<const uint8_t>)>& send,
                                                   const std::function<std::optional<std::vector<uint8_t>>()>& receive,
                                                   const std::string& name,
                                                   std::chrono::milliseconds window = std::chrono::milliseconds(1500));

    /** @brief Non-blocking loopback UDP socket that records every datagram it receives. */
    class LoopbackSocket
    {
      public:
        LoopbackSocket();
        ~LoopbackSocket();
        LoopbackSocket(const LoopbackSocket&) = delete;
        LoopbackSocket& operator=(const LoopbackSocket&) = delete;

        [[nodiscard]] bool IsReady() const { return m_ready; }
        [[nodiscard]] uint16_t Port() const;
        bool SendTo(uint16_t port, std::span<const uint8_t> datagram) const;
        bool SendTo(const sockaddr_in& target, std::span<const uint8_t> datagram) const;
        /// One datagram if any is waiting; the sender is remembered in LastSender().
        std::optional<std::vector<uint8_t>> Receive();
        [[nodiscard]] const sockaddr_in& LastSender() const { return m_lastSender; }
        /// Every datagram received so far, verbatim.
        [[nodiscard]] const std::vector<std::vector<uint8_t>>& Captured() const { return m_captured; }

      private:
        SOCKET m_socket = INVALID_SOCKET;
        bool m_ready = false;
        sockaddr_in m_lastSender{};
        std::vector<std::vector<uint8_t>> m_captured;
#ifdef SPARK_PLATFORM_WINDOWS
        bool m_winsockStarted = false;
#endif
    };

    /** @brief The client half of a hand-driven v2 session against the singleton server. */
    class SecureRawClient
    {
      public:
        SecureRawClient() = default;

        [[nodiscard]] LoopbackSocket& Socket() { return m_socket; }
        [[nodiscard]] Spark::Net::ClientID Id() const { return m_id; }
        [[nodiscard]] bool HasChannel() const { return m_channel != nullptr; }
        [[nodiscard]] Spark::Net::SecureChannel* Channel() { return m_channel.get(); }

        /// Framed Connect carrying a fresh ClientHello (advertising @p version).
        std::vector<uint8_t> BeginConnect(uint16_t version = Spark::Net::NETWORK_PROTOCOL_VERSION);

        /**
         * @brief Full handshake with @p server: Connect, verify ConnectAccepted, send ClientFinished
         * @return true once the server lists this client as Connected
         */
        bool Connect(Spark::Net::NetworkManager& server, const std::string& name,
                     const Spark::Net::ServerPublicKey* pinned = nullptr,
                     std::chrono::milliseconds window = std::chrono::milliseconds(1500));

        /// Seal an inner message into a [kFrameSealed][SecureChannel packet] datagram.
        std::vector<uint8_t> Seal(std::span<const uint8_t> wire);
        /// Seal and send to @p port (the server's bound port when omitted).
        bool SendSealed(std::span<const uint8_t> wire, uint16_t port = 0);
        /// Open a received datagram: sealed frames through the channel, handshake frames verbatim.
        std::optional<WireMessage> Open(std::span<const uint8_t> datagram);

        /// Pump @p manager until a message of @p type arrives (opened) or the window passes.
        std::optional<WireMessage> AwaitType(Spark::Net::NetworkManager& manager, Spark::Net::MessageType type,
                                             std::chrono::milliseconds window = std::chrono::milliseconds(400));

        /// Finish the handshake from a ConnectAccepted the test received itself.
        bool FinishFromAccepted(const WireMessage& accepted, const Spark::Net::ServerPublicKey& pinned);

      private:
        LoopbackSocket m_socket;
        Spark::Net::ClientHandshake m_handshake;
        std::unique_ptr<Spark::Net::SecureChannel> m_channel;
        Spark::Net::ClientID m_id = Spark::Net::INVALID_CLIENT;
        uint16_t m_serverPort = 0;
    };

    /** @brief The server half of a hand-driven v2 session, for tests of the singleton client. */
    class SecureRawServer
    {
      public:
        SecureRawServer() = default;

        [[nodiscard]] LoopbackSocket& Socket() { return m_socket; }
        [[nodiscard]] uint16_t Port() const { return m_socket.Port(); }
        [[nodiscard]] bool HasChannel() const { return m_channel != nullptr; }
        [[nodiscard]] Spark::Net::SecureChannel* Channel() { return m_channel.get(); }

        /// Pump @p client until its framed Connect arrives; returns the ClientHello payload.
        std::optional<std::vector<uint8_t>> AwaitConnect(
            Spark::Net::NetworkManager& client, std::chrono::milliseconds window = std::chrono::milliseconds(400));

        /**
         * @brief Answer the last ClientHello with a signed ConnectAccepted and install the server channel
         * @param id       Client id to assign
         * @param identity Identity to sign with (TestServerIdentity() when null)
         * @param echoedVersion Version echoed in the ConnectAccepted header
         * @return The framed datagram that was sent, or empty when the hello was refused
         */
        std::vector<uint8_t> Accept(Spark::Net::ClientID id, const Spark::Net::ServerIdentity* identity = nullptr,
                                    uint16_t echoedVersion = Spark::Net::NETWORK_PROTOCOL_VERSION);

        /// The CONNECT_ACCEPT_PREFIX_SIZE bytes before the ServerHello (server time 0); they are
        /// signed with it, so tests pass the same prefix to RespondToClientHello and AcceptPayload.
        static std::vector<uint8_t> AcceptPrefix(Spark::Net::ClientID id,
                                                 uint16_t echoedVersion = Spark::Net::NETWORK_PROTOCOL_VERSION);

        /// Plaintext ConnectAccepted payload for @p id carrying @p serverHello (for tamper tests).
        static std::vector<uint8_t> AcceptPayload(Spark::Net::ClientID id, uint16_t echoedVersion,
                                                  std::span<const uint8_t> serverHello);

        std::vector<uint8_t> Seal(std::span<const uint8_t> wire);
        bool SendSealed(std::span<const uint8_t> wire);
        std::optional<WireMessage> Open(std::span<const uint8_t> datagram);
        std::optional<WireMessage> AwaitType(Spark::Net::NetworkManager& manager, Spark::Net::MessageType type,
                                             std::chrono::milliseconds window = std::chrono::milliseconds(400));

        [[nodiscard]] const std::vector<uint8_t>& LastClientHello() const { return m_clientHello; }

      private:
        LoopbackSocket m_socket;
        std::unique_ptr<Spark::Net::SecureChannel> m_channel;
        std::vector<uint8_t> m_clientHello;
    };

    /**
     * @brief Moves a test's own hand-built messages onto the v2 wire
     *
     * Older loopback tests build raw inner messages (the 23-byte header plus payload) and send
     * them on their own sockets. Route those sends through Send(): a Connect runs the real v2
     * handshake against the singleton (its payload may carry a legacy magic/version/name, whose
     * name becomes the ClientFinished name), and every later message is sealed with the session
     * channel. Open() turns a received datagram back into the inner message bytes.
     */
    class WireAdapter
    {
      public:
        using RawSend = std::function<bool(std::span<const uint8_t>)>;
        using RawReceive = std::function<std::optional<std::vector<uint8_t>>()>;

        bool Send(std::span<const uint8_t> message, const RawSend& send, const RawReceive& receive);
        std::optional<std::vector<uint8_t>> Open(std::span<const uint8_t> datagram);

        [[nodiscard]] Spark::Net::ClientID Id() const { return m_id; }
        [[nodiscard]] bool HasChannel() const { return m_channel != nullptr; }

      private:
        std::unique_ptr<Spark::Net::SecureChannel> m_channel;
        Spark::Net::ClientID m_id = Spark::Net::INVALID_CLIENT;
    };

    /// Legacy-shaped Connect payload (magic, version, name) that WireAdapter::Send upgrades.
    std::vector<uint8_t> LegacyConnectPayload(const std::string& name);

    /// True when @p needle occurs anywhere in @p haystack.
    bool ContainsBytes(std::span<const uint8_t> haystack, std::span<const uint8_t> needle);

    /// Pump @p manager with short sleeps until @p predicate holds or @p window passes.
    template <typename Predicate>
    bool PumpUntil(Spark::Net::NetworkManager& manager, Predicate predicate,
                   std::chrono::milliseconds window = std::chrono::milliseconds(1000))
    {
        const auto deadline = std::chrono::steady_clock::now() + window;
        while (std::chrono::steady_clock::now() < deadline)
        {
            manager.Update(0.016f);
            if (predicate())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return predicate();
    }
} // namespace SparkTestFixtures

#endif // ENABLE_NETWORKING
