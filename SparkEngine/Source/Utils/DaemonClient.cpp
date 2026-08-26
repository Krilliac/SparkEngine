/**
 * @file DaemonClient.cpp
 * @brief Implementation of DaemonClient — connect, send, receive.
 */

#include "DaemonClient.h"
#include "DaemonFraming.h"

#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Spark::Daemon
{

    namespace
    {
        NativeSocket ToNative(std::intptr_t handle) noexcept
        {
#if defined(_WIN32)
            return reinterpret_cast<NativeSocket>(handle);
#else
            return static_cast<NativeSocket>(handle);
#endif
        }

        std::intptr_t FromNative(NativeSocket s) noexcept
        {
#if defined(_WIN32)
            return reinterpret_cast<std::intptr_t>(s);
#else
            return static_cast<std::intptr_t>(s);
#endif
        }
    } // namespace

    DaemonClient::~DaemonClient()
    {
        Disconnect();
    }

    bool DaemonClient::IsConnected() const
    {
        std::lock_guard lock(m_mutex);
        return ToNative(m_nativeSocket) != kInvalidSocket;
    }

    Expected<void, std::string> DaemonClient::Connect(const std::string& socketPath)
    {
#if defined(_WIN32)
        if (socketPath.empty())
            return Unexpected<std::string>("DaemonClient: pipe endpoint is empty");
        const std::wstring pipeName = NormalizePipeName(socketPath);
        if (pipeName.empty())
            return Unexpected<std::string>("DaemonClient: pipe endpoint is not valid UTF-8");

        {
            std::lock_guard lock(m_mutex);
            auto existing = ToNative(m_nativeSocket);
            if (existing != kInvalidSocket)
            {
                CloseSocket(existing);
                m_nativeSocket = FromNative(kInvalidSocket);
            }
        }

        if (!::WaitNamedPipeW(pipeName.c_str(), 5000))
            return Unexpected<std::string>("DaemonClient: WaitNamedPipeW failed (error " +
                                           std::to_string(::GetLastError()) + ")");

        HANDLE pipe = ::CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
            return Unexpected<std::string>("DaemonClient: CreateFileW failed (error " +
                                           std::to_string(::GetLastError()) + ")");

        DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
        if (!::SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr))
        {
            const DWORD error = ::GetLastError();
            ::CloseHandle(pipe);
            return Unexpected<std::string>("DaemonClient: SetNamedPipeHandleState failed (error " +
                                           std::to_string(error) + ")");
        }

        m_shuttingDown.store(false, std::memory_order_release);
        {
            std::lock_guard lock(m_mutex);
            m_nativeSocket = FromNative(pipe);
        }
        return {};
#else
        if (socketPath.empty())
            return Unexpected<std::string>("DaemonClient: socket path is empty");
        if (socketPath.size() >= sizeof(sockaddr_un{}.sun_path))
            return Unexpected<std::string>("DaemonClient: socket path exceeds sockaddr_un capacity");

        {
            std::lock_guard lock(m_mutex);
            auto existing = ToNative(m_nativeSocket);
            if (existing != kInvalidSocket)
            {
                CloseSocket(existing);
                m_nativeSocket = FromNative(kInvalidSocket);
            }
        }

        int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0)
            return Unexpected<std::string>(std::string("DaemonClient: socket() failed: ") + std::strerror(errno));
#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
        const int noSigPipe = 1;
        if (::setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe)) != 0)
        {
            const std::string err = std::strerror(errno);
            ::close(sock);
            return Unexpected<std::string>("DaemonClient: could not disable SIGPIPE: " + err);
        }
#endif

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, socketPath.data(), socketPath.size());

        if (::connect(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            std::string err = std::strerror(errno);
            ::close(sock);
            return Unexpected<std::string>("DaemonClient: connect() to " + socketPath + " failed: " + err);
        }

        // 500ms recv timeout so shutdown cycles pick up m_shuttingDown promptly.
        timeval tv{0, 500'000};
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        m_shuttingDown.store(false, std::memory_order_release);
        {
            std::lock_guard lock(m_mutex);
            m_nativeSocket = FromNative(static_cast<NativeSocket>(sock));
        }
        return {};
#endif
    }

    void DaemonClient::Disconnect()
    {
        m_shuttingDown.store(true, std::memory_order_release);
        std::lock_guard lock(m_mutex);
        auto s = ToNative(m_nativeSocket);
        if (s != kInvalidSocket)
        {
            CloseSocket(s);
            m_nativeSocket = FromNative(kInvalidSocket);
        }
    }

    Expected<Response, std::string> DaemonClient::Request(ServiceId service, uint16_t messageType,
                                                          const std::vector<uint8_t>& payload)
    {
        std::lock_guard lock(m_mutex);
        auto s = ToNative(m_nativeSocket);
        if (s == kInvalidSocket)
            return Unexpected<std::string>("DaemonClient: not connected");

        if (!SendFrame(s, service, messageType, payload, m_shuttingDown))
            return Unexpected<std::string>("DaemonClient: SendFrame failed");

        FrameHeader header;
        std::vector<uint8_t> responsePayload;
        if (!RecvFrame(s, header, responsePayload, m_shuttingDown))
            return Unexpected<std::string>("DaemonClient: RecvFrame failed");

        if (header.serviceId != static_cast<uint16_t>(service) &&
            header.serviceId != static_cast<uint16_t>(ServiceId::Control))
            return Unexpected<std::string>("DaemonClient: response service mismatch");

        // Message type 0x00FF is reserved across all services for error replies
        // (see DaemonProtocol.h::ControlMessage::ErrorResponse). Surface these
        // uniformly so per-service wrappers don't need bespoke error handling.
        if (header.messageType == static_cast<uint16_t>(ControlMessage::ErrorResponse))
        {
            std::string msg(responsePayload.begin(), responsePayload.end());
            return Unexpected<std::string>("DaemonClient: server error: " + msg);
        }

        Response out;
        out.messageType = header.messageType;
        out.payload = std::move(responsePayload);
        return out;
    }

    Expected<void, std::string> DaemonClient::SendOneWay(ServiceId service, uint16_t messageType,
                                                         const std::vector<uint8_t>& payload)
    {
        std::lock_guard lock(m_mutex);
        auto s = ToNative(m_nativeSocket);
        if (s == kInvalidSocket)
            return Unexpected<std::string>("DaemonClient: not connected");
        if (!SendFrame(s, service, messageType, payload, m_shuttingDown))
            return Unexpected<std::string>("DaemonClient: SendFrame failed");
        return {};
    }

    Expected<void, std::string> DaemonClient::Ping()
    {
        auto result = Request(ServiceId::Control, static_cast<uint16_t>(ControlMessage::PingRequest), /*payload*/ {});
        if (!result)
            return Unexpected<std::string>(result.error());
        if (result->messageType != static_cast<uint16_t>(ControlMessage::PingResponse))
            return Unexpected<std::string>("DaemonClient: unexpected ping response type");
        std::string body(result->payload.begin(), result->payload.end());
        if (body != "pong")
            return Unexpected<std::string>("DaemonClient: ping body mismatch: " + body);
        return {};
    }

} // namespace Spark::Daemon
