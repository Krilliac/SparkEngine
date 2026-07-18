/**
 * @file TFLanDiscoveryInternal.h
 * @brief Shared internals for the TFLanDiscovery*.cpp split parts: the
 *        WinSock/BSD non-blocking UDP socket shim (handle type, close,
 *        non-blocking toggle, errno classification) and the type-erased
 *        invalid-socket sentinel. Include only from the TFLanDiscovery
 *        translation units.
 */
#pragma once

#ifdef ENABLE_NETWORKING
#ifdef SPARK_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h> // INTERFACE_INFO / SIO_GET_INTERFACE_LIST
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#endif // ENABLE_NETWORKING

#include <cstdint>

namespace Terrafront
{
    namespace LanDetail
    {

#ifdef ENABLE_NETWORKING

#ifdef SPARK_PLATFORM_WINDOWS
        using TFSockHandle = SOCKET;

        inline TFSockHandle ToSock(intptr_t s)
        {
            return static_cast<TFSockHandle>(s);
        }

        inline void CloseSockHandle(TFSockHandle s)
        {
            closesocket(s);
        }

        inline bool SetNonBlocking(TFSockHandle s)
        {
            u_long nonBlocking = 1;
            return ioctlsocket(s, FIONBIO, &nonBlocking) == 0;
        }

        inline bool WouldBlock()
        {
            const int e = WSAGetLastError();
            return e == WSAEWOULDBLOCK;
        }

        inline bool IgnorableRecvError()
        {
            // UDP sockets surface ICMP port-unreachable from an earlier send as
            // WSAECONNRESET on recvfrom — not an error for a broadcast listener.
            const int e = WSAGetLastError();
            return e == WSAECONNRESET || e == WSAEMSGSIZE;
        }
#else
        using TFSockHandle = int;

        inline TFSockHandle ToSock(intptr_t s)
        {
            return static_cast<TFSockHandle>(s);
        }

        inline void CloseSockHandle(TFSockHandle s)
        {
            ::close(s);
        }

        inline bool SetNonBlocking(TFSockHandle s)
        {
            const int flags = fcntl(s, F_GETFL, 0);
            return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
        }

        inline bool WouldBlock()
        {
            return errno == EWOULDBLOCK || errno == EAGAIN;
        }

        inline bool IgnorableRecvError()
        {
            return errno == ECONNREFUSED || errno == EINTR;
        }
#endif

        inline constexpr intptr_t kInvalidSock = -1;

#endif // ENABLE_NETWORKING

    } // namespace LanDetail
} // namespace Terrafront
