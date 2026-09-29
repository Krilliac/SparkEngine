/**
 * @file NetworkWireLimits.h
 * @brief Authoritative size contract for Spark's UDP message wire format.
 */

#pragma once

#include <cstddef>

namespace Spark::Net
{
    // IPv4 limits a UDP datagram (UDP header + payload) to 65,515 bytes.
    // After the 8-byte UDP header, sendto/recvfrom can carry at most 65,507
    // bytes. Spark's fixed message header consumes 23 of those bytes.
    // The same payload ceiling applies to every channel. In particular, a
    // 5 KiB Reliable/ReliableOrdered payload is supported; fragmentation and
    // reassembly below this UDP boundary are delegated to the IP stack.
    // NET-100 (protocol v2): every datagram is one frame-kind byte plus either the plaintext
    // handshake message or a SecureChannel packet (10-byte header + 16-byte tag) around it, so
    // the payload ceiling leaves room for the largest frame (NetworkConnection.cpp asserts the
    // overhead against SECURE_PACKET_OVERHEAD).
    inline constexpr std::size_t NETWORK_WIRE_HEADER_SIZE = 23;
    inline constexpr std::size_t NETWORK_FRAME_OVERHEAD = 1 + 10 + 16;
    inline constexpr std::size_t MAX_UDP_WIRE_DATAGRAM_SIZE = 65'507;
    inline constexpr std::size_t MAX_NETWORK_MESSAGE_PAYLOAD_SIZE =
        MAX_UDP_WIRE_DATAGRAM_SIZE - NETWORK_WIRE_HEADER_SIZE - NETWORK_FRAME_OVERHEAD;

    // Kernel buffer sizes for Spark's UDP sockets. BSD-derived stacks (macOS) refuse to send a datagram
    // larger than SO_SNDBUF and hold exactly SO_RCVBUF bytes of queued datagrams (Linux doubles the request).
    // The send buffer must admit one maximum datagram. The receive buffer must admit one while other
    // peers' traffic is still queued: at 64 KiB, macOS dropped every maximum-size datagram that arrived
    // behind even one small queued packet.
    inline constexpr std::size_t NETWORK_SOCKET_SEND_BUFFER_SIZE = std::size_t{64} * 1024;
    inline constexpr std::size_t NETWORK_SOCKET_RECEIVE_BUFFER_SIZE = std::size_t{256} * 1024;
    static_assert(NETWORK_SOCKET_SEND_BUFFER_SIZE >= MAX_UDP_WIRE_DATAGRAM_SIZE);
    static_assert(NETWORK_SOCKET_RECEIVE_BUFFER_SIZE >= 2 * MAX_UDP_WIRE_DATAGRAM_SIZE);

    [[nodiscard]] inline constexpr bool IsNetworkPayloadSizeValid(std::size_t payloadSize) noexcept
    {
        return payloadSize <= MAX_NETWORK_MESSAGE_PAYLOAD_SIZE;
    }
} // namespace Spark::Net
