/**
 * @file UDPTransport.h
 * @brief UDP socket transport implementation of ITransport
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a concrete ITransport backed by platform BSD/Winsock UDP sockets.
 * This is the default transport used by NetworkManager.
 *
 * All networking code is guarded by ENABLE_NETWORKING.
 */

#pragma once
#include "ITransport.h"

#ifdef ENABLE_NETWORKING

// Platform socket headers
#ifdef SPARK_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;
#endif // SPARK_PLATFORM_WINDOWS

#include <cstring>
#include <array>

namespace Spark::Net
{

    /// @brief UDP socket-based transport (default transport for NetworkManager).
    ///
    /// Wraps platform-specific BSD/Winsock UDP socket calls behind the
    /// ITransport interface.  Creates a non-blocking socket with enlarged
    /// send/receive buffers suitable for game traffic.
    class UDPTransport final : public ITransport
    {
      public:
        UDPTransport() = default;
        ~UDPTransport() override { Shutdown(); }

        // Non-copyable
        UDPTransport(const UDPTransport&) = delete;
        UDPTransport& operator=(const UDPTransport&) = delete;

        bool Initialize(uint16_t port) override
        {
            if (m_socket != INVALID_SOCKET)
                return true; // Already initialised

            m_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (m_socket == INVALID_SOCKET)
                return false;

            // Set non-blocking mode
#ifdef SPARK_PLATFORM_WINDOWS
            u_long nonBlocking = 1;
            if (ioctlsocket(m_socket, FIONBIO, &nonBlocking) == SOCKET_ERROR)
            {
                Shutdown();
                return false;
            }
#else
            int flags = fcntl(m_socket, F_GETFL, 0);
            if (flags == -1 || fcntl(m_socket, F_SETFL, flags | O_NONBLOCK) == -1)
            {
                Shutdown();
                return false;
            }
#endif // SPARK_PLATFORM_WINDOWS

            // Enlarge OS socket buffers for game traffic
            int sendBufSize = 65536;
            int recvBufSize = 65536;
            setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBufSize),
                       sizeof(sendBufSize));
            setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&recvBufSize),
                       sizeof(recvBufSize));

            // Bind to the requested port (0 = OS-assigned ephemeral port)
            sockaddr_in localAddr{};
            localAddr.sin_family = AF_INET;
            localAddr.sin_addr.s_addr = INADDR_ANY;
            localAddr.sin_port = htons(port);

            if (::bind(m_socket, reinterpret_cast<const sockaddr*>(&localAddr), sizeof(localAddr)) == SOCKET_ERROR)
            {
                Shutdown();
                return false;
            }

            return true;
        }

        void Shutdown() override
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

        bool Send(const uint8_t* data, size_t size, const std::string& address, uint16_t port) override
        {
            if (m_socket == INVALID_SOCKET || data == nullptr || size == 0)
                return false;

            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_port = htons(port);

            if (inet_pton(AF_INET, address.c_str(), &dest.sin_addr) != 1)
                return false;

            int sent = sendto(m_socket, reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
                              reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));

            return sent != SOCKET_ERROR;
        }

        int Receive(uint8_t* buffer, size_t bufferSize, std::string& fromAddress, uint16_t& fromPort) override
        {
            if (m_socket == INVALID_SOCKET)
                return -1;

            sockaddr_in senderAddr{};
            socklen_t senderLen = sizeof(senderAddr);

            int received = recvfrom(m_socket, reinterpret_cast<char*>(buffer), static_cast<int>(bufferSize), 0,
                                    reinterpret_cast<sockaddr*>(&senderAddr), &senderLen);

            if (received <= 0)
                return received;

            // Convert sender address to string
            std::array<char, INET_ADDRSTRLEN> addrBuf{};
            inet_ntop(AF_INET, &senderAddr.sin_addr, addrBuf.data(), static_cast<socklen_t>(addrBuf.size()));
            fromAddress = addrBuf.data();
            fromPort = ntohs(senderAddr.sin_port);

            return received;
        }

        bool IsReady() const override { return m_socket != INVALID_SOCKET; }

        std::string GetTransportName() const override { return "UDP"; }

      private:
        SOCKET m_socket = INVALID_SOCKET;
    };

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
