/**
 * @file ITransport.h
 * @brief Abstract transport layer interface for network communication
 * @author Spark Engine Team
 * @date 2025
 *
 * Defines the ITransport interface that decouples the NetworkManager from
 * any specific transport implementation (raw UDP, Steam Networking, etc.).
 * Concrete implementations can be swapped at runtime via
 * NetworkManager::SetTransport().
 *
 * All networking code is guarded by ENABLE_NETWORKING.
 */

#pragma once
#include "../../Core/Platform.h"

#include <cstdint>
#include <string>

#ifdef ENABLE_NETWORKING

namespace Spark::Net
{

    /// @brief Abstract transport interface for sending and receiving raw packets.
    ///
    /// Implementations handle the platform-specific details of packet I/O
    /// (BSD sockets, Steam Networking Sockets, etc.) while the NetworkManager
    /// operates against this uniform API.
    class ITransport
    {
      public:
        virtual ~ITransport() = default;

        /// @brief Initialize the transport on the given port.
        /// @param port  UDP port to bind (0 for OS-assigned ephemeral port).
        /// @return true on success.
        virtual bool Initialize(uint16_t port) = 0;

        /// @brief Shut down the transport and release resources.
        virtual void Shutdown() = 0;

        /// @brief Send raw bytes to a remote address.
        /// @param data     Pointer to the data buffer.
        /// @param size     Number of bytes to send.
        /// @param address  Destination IP address string (e.g. "127.0.0.1").
        /// @param port     Destination port.
        /// @return true if the data was handed off to the OS successfully.
        virtual bool Send(const uint8_t* data, size_t size, const std::string& address, uint16_t port) = 0;

        /// @brief Receive raw bytes (non-blocking).
        /// @param buffer       Output buffer for incoming data.
        /// @param bufferSize   Maximum bytes to read.
        /// @param fromAddress  [out] Sender IP address string.
        /// @param fromPort     [out] Sender port.
        /// @return Number of bytes received, 0 if nothing available, or -1 on error.
        virtual int Receive(uint8_t* buffer, size_t bufferSize, std::string& fromAddress, uint16_t& fromPort) = 0;

        /// @brief Check whether the transport is initialized and ready.
        virtual bool IsReady() const = 0;

        /// @brief Human-readable name for logging (e.g. "UDP", "Steam").
        virtual std::string GetTransportName() const = 0;
    };

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
