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
    inline constexpr std::size_t NETWORK_WIRE_HEADER_SIZE = 23;
    inline constexpr std::size_t MAX_UDP_WIRE_DATAGRAM_SIZE = 65'507;
    inline constexpr std::size_t MAX_NETWORK_MESSAGE_PAYLOAD_SIZE =
        MAX_UDP_WIRE_DATAGRAM_SIZE - NETWORK_WIRE_HEADER_SIZE;

    [[nodiscard]] inline constexpr bool IsNetworkPayloadSizeValid(std::size_t payloadSize) noexcept
    {
        return payloadSize <= MAX_NETWORK_MESSAGE_PAYLOAD_SIZE;
    }
} // namespace Spark::Net
