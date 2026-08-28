/**
 * @file LiveEditBridge.cpp
 * @brief Bridge between collaborative editing and a running AreaServer
 */

#include "LiveEditBridge.h"
#include "CollaborativeEditSession.h"
#include "Utils/Validate.h"

#include <cstring>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#endif

namespace SparkEditor
{

    namespace
    {
        using BridgeSocketHandle = CollaborativeSocketHandle;

#ifdef _WIN32
        SOCKET ToNativeSocket(BridgeSocketHandle socket)
        {
            return static_cast<SOCKET>(socket);
        }
#else
        int ToNativeSocket(BridgeSocketHandle socket)
        {
            return socket;
        }
#endif

        BridgeSocketHandle ToStoredSocket(decltype(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) socket)
        {
            return static_cast<BridgeSocketHandle>(socket);
        }

        bool IsValidSocket(BridgeSocketHandle socket)
        {
            return socket != INVALID_COLLAB_SOCKET;
        }

        void CloseBridgeSocket(BridgeSocketHandle socket)
        {
            if (!IsValidSocket(socket))
                return;
#ifdef _WIN32
            ::closesocket(ToNativeSocket(socket));
#else
            ::close(ToNativeSocket(socket));
#endif
        }

        // Reuse the wire protocol helpers from CollaborativeEditSession
        bool SendAll(BridgeSocketHandle sock, const void* buf, size_t len)
        {
            const auto* ptr = static_cast<const char*>(buf);
            size_t sent = 0;
            while (sent < len)
            {
#ifdef _WIN32
                constexpr int flags = 0;
#elif defined(MSG_NOSIGNAL)
                constexpr int flags = MSG_NOSIGNAL;
#else
                constexpr int flags = 0;
#endif
                auto n = ::send(ToNativeSocket(sock), ptr + sent, static_cast<int>(len - sent), flags);
                if (n <= 0)
                    return false;
                sent += static_cast<size_t>(n);
            }
            return true;
        }

        bool SendFramedBridge(BridgeSocketHandle sock, const std::vector<uint8_t>& data)
        {
            if (!IsValidSocket(sock))
                return false;

            uint32_t len = static_cast<uint32_t>(data.size());
            uint8_t header[4];
            header[0] = static_cast<uint8_t>((len >> 24) & 0xFF);
            header[1] = static_cast<uint8_t>((len >> 16) & 0xFF);
            header[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
            header[3] = static_cast<uint8_t>(len & 0xFF);

            return SendAll(sock, header, 4) && SendAll(sock, data.data(), data.size());
        }

        void WriteU8(std::vector<uint8_t>& buf, uint8_t val)
        {
            buf.push_back(val);
        }

        void WriteU16(std::vector<uint8_t>& buf, uint16_t val)
        {
            buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
            buf.push_back(static_cast<uint8_t>(val & 0xFF));
        }

        void WriteU32(std::vector<uint8_t>& buf, uint32_t val)
        {
            buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
            buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
            buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
            buf.push_back(static_cast<uint8_t>(val & 0xFF));
        }

        void WriteU64(std::vector<uint8_t>& buf, uint64_t val)
        {
            WriteU32(buf, static_cast<uint32_t>((val >> 32) & 0xFFFFFFFF));
            WriteU32(buf, static_cast<uint32_t>(val & 0xFFFFFFFF));
        }

        void WriteString(std::vector<uint8_t>& buf, const std::string& str)
        {
            WriteU32(buf, static_cast<uint32_t>(str.size()));
            buf.insert(buf.end(), str.begin(), str.end());
        }

        // Serialize an EditMessage into the AreaServer wire format
        std::vector<uint8_t> SerializeEditForServer(const EditMessage& edit)
        {
            std::vector<uint8_t> buf;
            buf.reserve(128);

            // Message type header: EditorNetMessageType::SceneEdit
            WriteU16(buf, static_cast<uint16_t>(EditorNetMessageType::SceneEdit));
            WriteU8(buf, static_cast<uint8_t>(edit.type));
            WriteU32(buf, edit.sourceEditor);
            WriteString(buf, edit.nodeId);
            WriteString(buf, edit.componentType);
            WriteString(buf, edit.propertyName);
            WriteString(buf, edit.newValue);
            WriteString(buf, edit.oldValue);
            WriteU64(buf, edit.timestamp);

            return buf;
        }

        std::vector<uint8_t> SerializeEditorJoin(const std::string& editorName)
        {
            std::vector<uint8_t> buf;
            buf.reserve(64);
            WriteU16(buf, static_cast<uint16_t>(EditorNetMessageType::EditorJoin));
            WriteString(buf, editorName);
            return buf;
        }
    } // namespace

    LiveEditBridge::LiveEditBridge()
        : LiveEditBridge([]() { return ToStoredSocket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)); })
    {
    }

    LiveEditBridge::LiveEditBridge(SocketFactory socketFactory) : m_socketFactory(std::move(socketFactory)) {}

    LiveEditBridge::~LiveEditBridge()
    {
        Disconnect();
    }

    bool LiveEditBridge::Connect(const std::string& address, uint16_t port, const std::string& editorName)
    {
        return Connect(address, port, editorName, Spark::Net::CaptureNetworkEndpointPolicy());
    }

    bool LiveEditBridge::Connect(const std::string& address, uint16_t port, const std::string& editorName,
                                 const Spark::Net::NetworkEndpointPolicy& endpointPolicy)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !address.empty(), false);
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, port > 0, false);
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !editorName.empty(), false);

        if (m_connected.load(std::memory_order_acquire))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "LiveEditBridge already connected.");
            return false;
        }

        if (!endpointPolicy.IsValid())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "LiveEditBridge: Invalid endpoint policy: %s",
                            Spark::Net::NetworkEndpointPolicyErrorText(endpointPolicy.Error()).data());
            return false;
        }

        uint32_t serverAddress = 0;
        if (!Spark::Net::ParseIPv4Address(address, serverAddress) || !endpointPolicy.AllowsPeerAddress(serverAddress))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor,
                            "LiveEditBridge: Refusing destination %s outside the captured endpoint policy",
                            address.c_str());
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1 || ntohl(addr.sin_addr.s_addr) != serverAddress)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "LiveEditBridge: Invalid canonical IPv4 destination %s",
                            address.c_str());
            return false;
        }

        m_socket = m_socketFactory ? m_socketFactory() : INVALID_COLLAB_SOCKET;
        if (!IsValidSocket(m_socket))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "LiveEditBridge: Failed to create socket.");
            return false;
        }

        sockaddr_in localAddress{};
        localAddress.sin_family = AF_INET;
        localAddress.sin_port = 0;
        localAddress.sin_addr.s_addr = htonl(endpointPolicy.BindAddress());
        if (::bind(ToNativeSocket(m_socket), reinterpret_cast<const sockaddr*>(&localAddress), sizeof(localAddress)) != 0)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor,
                            "LiveEditBridge: Failed to bind the configured local interface");
            CloseConnectionSocket();
            return false;
        }

        // Non-blocking connect with a five-second timeout. A writable socket can
        // still represent a failed connect, so both platforms verify SO_ERROR.
        bool connectOk = false;
#ifdef _WIN32
        u_long nbMode = 1;
        if (::ioctlsocket(ToNativeSocket(m_socket), FIONBIO, &nbMode) != 0)
        {
            CloseConnectionSocket();
            return false;
        }
        const int connectResult =
            ::connect(ToNativeSocket(m_socket), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        const bool connectPending = connectResult == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK;
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(ToNativeSocket(m_socket), &writeSet);
        timeval tv{5, 0};
        if (connectResult == 0 || (connectPending && ::select(0, nullptr, &writeSet, nullptr, &tv) > 0))
        {
            int socketError = 0;
            int socketErrorLength = sizeof(socketError);
            connectOk = ::getsockopt(ToNativeSocket(m_socket), SOL_SOCKET, SO_ERROR,
                                     reinterpret_cast<char*>(&socketError), &socketErrorLength) == 0 &&
                        socketError == 0;
        }
        nbMode = 0;
        if (::ioctlsocket(ToNativeSocket(m_socket), FIONBIO, &nbMode) != 0)
            connectOk = false;
#else
        const int flags = fcntl(ToNativeSocket(m_socket), F_GETFL, 0);
        if (flags < 0 || fcntl(ToNativeSocket(m_socket), F_SETFL, flags | O_NONBLOCK) != 0)
        {
            CloseConnectionSocket();
            return false;
        }
        const int connectResult =
            ::connect(ToNativeSocket(m_socket), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        pollfd pfd{};
        pfd.fd = ToNativeSocket(m_socket);
        pfd.events = POLLOUT;
        if (connectResult == 0 || (connectResult < 0 && errno == EINPROGRESS && ::poll(&pfd, 1, 5000) > 0))
        {
            int socketError = 0;
            socklen_t socketErrorLength = sizeof(socketError);
            connectOk = ::getsockopt(ToNativeSocket(m_socket), SOL_SOCKET, SO_ERROR, &socketError,
                                     &socketErrorLength) == 0 &&
                        socketError == 0;
        }
        if (fcntl(ToNativeSocket(m_socket), F_SETFL, flags) != 0)
            connectOk = false;
#endif

        if (!connectOk)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "LiveEditBridge: Failed to connect to %s:%u (timeout 5s).",
                            address.c_str(), port);
            CloseConnectionSocket();
            return false;
        }

        m_serverAddress = address;
        m_serverAddressNumeric = serverAddress;
        m_serverPort = port;
        m_editorName = editorName;
        m_endpointPolicy = endpointPolicy;
        m_shuttingDown.store(false, std::memory_order_release);

        const auto joinMsg = SerializeEditorJoin(editorName);
        if (!DestinationAllowed() || !SendFramedBridge(m_socket, joinMsg))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "LiveEditBridge: Failed to send editor join handshake");
            CloseConnectionSocket();
            return false;
        }
        m_connected.store(true, std::memory_order_release);

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "LiveEditBridge: Connected to AreaServer at %s:%u as '%s'.",
                       address.c_str(), port, editorName.c_str());
        return true;
    }

    void LiveEditBridge::Disconnect()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        m_shuttingDown.store(true, std::memory_order_release);
        const bool wasConnected = m_connected.exchange(false, std::memory_order_acq_rel);

        if (wasConnected && IsValidSocket(m_socket))
        {
            // Send leave notification before closing
            std::vector<uint8_t> leaveMsg;
            WriteU16(leaveMsg, static_cast<uint16_t>(EditorNetMessageType::EditorLeave));
            WriteString(leaveMsg, m_editorName);
            if (DestinationAllowed())
                (void)SendFramedBridge(m_socket, leaveMsg);
        }
        CloseConnectionSocket();

        if (wasConnected)
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "LiveEditBridge: Disconnected from AreaServer.");
    }

    void LiveEditBridge::PushEdit(const EditMessage& edit)
    {
        if (!m_connected.load(std::memory_order_acquire))
            return;

        std::lock_guard<std::mutex> lock(m_editMutex);
        m_pendingEdits.push(edit);
    }

    void LiveEditBridge::Update()
    {
        if (!m_connected.load(std::memory_order_acquire))
            return;

        // Drain pending edits and send to AreaServer
        std::queue<EditMessage> editsToSend;
        {
            std::lock_guard<std::mutex> lock(m_editMutex);
            std::swap(editsToSend, m_pendingEdits);
        }

        while (!editsToSend.empty())
        {
            auto data = SerializeEditForServer(editsToSend.front());
            if (!DestinationAllowed() || !SendFramedBridge(m_socket, data))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Editor, "LiveEditBridge: Send failed, disconnecting.");
                m_connected.store(false, std::memory_order_release);
                CloseConnectionSocket();
                break;
            }
            m_editsPushed++;
            editsToSend.pop();
        }
    }

    bool LiveEditBridge::DestinationAllowed() const noexcept
    {
        return IsValidSocket(m_socket) && m_endpointPolicy.IsValid() &&
               m_endpointPolicy.AllowsPeerAddress(m_serverAddressNumeric);
    }

    void LiveEditBridge::CloseConnectionSocket() noexcept
    {
        if (IsValidSocket(m_socket))
            CloseBridgeSocket(m_socket);
        m_socket = INVALID_COLLAB_SOCKET;
    }

} // namespace SparkEditor
