/**
 * @file CollaborativeEditSession.cpp
 * @brief Multi-user collaborative editor session with TCP networking
 */

#include "CollaborativeEditSession.h"
#include "Utils/Validate.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketType = SOCKET;
constexpr SocketType INVALID_SOCK = INVALID_SOCKET;
#define CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
using SocketType = int;
constexpr SocketType INVALID_SOCK = -1;
#define CLOSE_SOCKET ::close
#endif

namespace SparkEditor
{

    // ============================================================================
    // Wire Protocol — length-prefixed binary serialization
    // ============================================================================

    namespace
    {
        void WriteU8(std::vector<uint8_t>& buf, uint8_t val)
        {
            buf.push_back(val);
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

        void WriteFloat(std::vector<uint8_t>& buf, float val)
        {
            uint32_t bits;
            std::memcpy(&bits, &val, sizeof(bits));
            WriteU32(buf, bits);
        }

        void WriteString(std::vector<uint8_t>& buf, const std::string& str)
        {
            WriteU32(buf, static_cast<uint32_t>(str.size()));
            buf.insert(buf.end(), str.begin(), str.end());
        }

        struct Reader
        {
            const uint8_t* data;
            size_t size;
            size_t pos = 0;

            bool HasBytes(size_t n) const { return pos + n <= size; }

            bool failed = false;

            uint8_t ReadU8()
            {
                if (!HasBytes(1))
                {
                    failed = true;
                    return 0;
                }
                uint8_t val = data[pos++];
                return val;
            }

            uint32_t ReadU32()
            {
                if (!HasBytes(4))
                {
                    failed = true;
                    return 0;
                }
                uint32_t val = (static_cast<uint32_t>(data[pos]) << 24) | (static_cast<uint32_t>(data[pos + 1]) << 16) |
                               (static_cast<uint32_t>(data[pos + 2]) << 8) | static_cast<uint32_t>(data[pos + 3]);
                pos += 4;
                return val;
            }

            uint64_t ReadU64()
            {
                uint64_t hi = ReadU32();
                uint64_t lo = ReadU32();
                return (hi << 32) | lo;
            }

            float ReadFloat()
            {
                uint32_t bits = ReadU32();
                float val = 0.0f;
                if (!failed)
                    std::memcpy(&val, &bits, sizeof(val));
                return val;
            }

            std::string ReadString()
            {
                uint32_t len = ReadU32();
                if (failed || !HasBytes(len))
                {
                    failed = true;
                    return "";
                }
                std::string str(reinterpret_cast<const char*>(data + pos), len);
                pos += len;
                return str;
            }
        };

        void WriteEditMessage(std::vector<uint8_t>& buf, const EditMessage& edit)
        {
            WriteU8(buf, static_cast<uint8_t>(edit.type));
            WriteU32(buf, edit.sourceEditor);
            WriteString(buf, edit.nodeId);
            WriteString(buf, edit.componentType);
            WriteString(buf, edit.propertyName);
            WriteString(buf, edit.newValue);
            WriteString(buf, edit.oldValue);
            WriteU64(buf, edit.timestamp);
        }

        EditMessage ReadEditMessage(Reader& r)
        {
            EditMessage edit;
            if (r.failed)
                return edit;
            edit.type = static_cast<EditMessageType>(r.ReadU8());
            edit.sourceEditor = r.ReadU32();
            edit.nodeId = r.ReadString();
            edit.componentType = r.ReadString();
            edit.propertyName = r.ReadString();
            edit.newValue = r.ReadString();
            edit.oldValue = r.ReadString();
            edit.timestamp = r.ReadU64();
            return edit;
        }

        void WriteEditorPeer(std::vector<uint8_t>& buf, const EditorPeer& peer)
        {
            WriteU32(buf, peer.id);
            WriteString(buf, peer.userName);
            WriteString(buf, peer.selectedNode);
            WriteFloat(buf, peer.viewportCameraPos.x);
            WriteFloat(buf, peer.viewportCameraPos.y);
            WriteFloat(buf, peer.viewportCameraPos.z);
            WriteFloat(buf, peer.viewportCameraDir.x);
            WriteFloat(buf, peer.viewportCameraDir.y);
            WriteFloat(buf, peer.viewportCameraDir.z);
            WriteFloat(buf, peer.color.r);
            WriteFloat(buf, peer.color.g);
            WriteFloat(buf, peer.color.b);
            WriteFloat(buf, peer.color.a);
        }

        EditorPeer ReadEditorPeer(Reader& r)
        {
            EditorPeer peer;
            if (r.failed)
                return peer;
            peer.id = r.ReadU32();
            peer.userName = r.ReadString();
            peer.selectedNode = r.ReadString();
            peer.viewportCameraPos.x = r.ReadFloat();
            peer.viewportCameraPos.y = r.ReadFloat();
            peer.viewportCameraPos.z = r.ReadFloat();
            peer.viewportCameraDir.x = r.ReadFloat();
            peer.viewportCameraDir.y = r.ReadFloat();
            peer.viewportCameraDir.z = r.ReadFloat();
            peer.color.r = r.ReadFloat();
            peer.color.g = r.ReadFloat();
            peer.color.b = r.ReadFloat();
            peer.color.a = r.ReadFloat();
            peer.isActive = true;
            return peer;
        }

        // Send length-prefixed message over TCP (thread-safe per socket)
        bool SendFramed(int sock, const std::vector<uint8_t>& data)
        {
            if (sock < 0 || data.empty())
                return false;

            uint32_t len = static_cast<uint32_t>(data.size());
            uint8_t header[4];
            header[0] = static_cast<uint8_t>((len >> 24) & 0xFF);
            header[1] = static_cast<uint8_t>((len >> 16) & 0xFF);
            header[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
            header[3] = static_cast<uint8_t>(len & 0xFF);

            auto sendAll = [](int s, const void* buf, size_t totalLen) -> bool
            {
                const auto* ptr = static_cast<const char*>(buf);
                size_t sent = 0;
                while (sent < totalLen)
                {
                    auto n = ::send(s, ptr + sent, static_cast<int>(totalLen - sent), 0);
                    if (n <= 0)
                        return false;
                    sent += static_cast<size_t>(n);
                }
                return true;
            };

            return sendAll(sock, header, 4) && sendAll(sock, data.data(), data.size());
        }

        // Receive one length-prefixed frame from TCP.
        // Socket should have SO_RCVTIMEO set so recv() returns periodically,
        // allowing us to check m_shuttingDown. Returns empty on disconnect/error.
        std::vector<uint8_t> RecvFramed(int sock, const std::atomic<bool>& shuttingDown)
        {
            auto recvAll = [&](void* buf, size_t totalLen) -> bool
            {
                auto* ptr = static_cast<char*>(buf);
                size_t received = 0;
                while (received < totalLen)
                {
                    if (shuttingDown.load(std::memory_order_acquire))
                        return false;
                    auto n = ::recv(sock, ptr + received, static_cast<int>(totalLen - received), 0);
                    if (n > 0)
                    {
                        received += static_cast<size_t>(n);
                        continue;
                    }
                    if (n == 0)
                        return false; // Clean disconnect
#ifdef _WIN32
                    if (WSAGetLastError() == WSAETIMEDOUT)
                        continue; // Timeout — retry after checking shuttingDown
#else
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        continue; // Timeout — retry after checking shuttingDown
#endif
                    return false; // Real error
                }
                return true;
            };

            uint8_t header[4];
            if (!recvAll(header, 4))
                return {};

            uint32_t len = (static_cast<uint32_t>(header[0]) << 24) | (static_cast<uint32_t>(header[1]) << 16) |
                           (static_cast<uint32_t>(header[2]) << 8) | static_cast<uint32_t>(header[3]);

            // Sanity check: max 16MB message
            if (len > 16 * 1024 * 1024)
                return {};

            std::vector<uint8_t> data(len);
            if (!recvAll(data.data(), len))
                return {};

            return data;
        }

#ifdef _WIN32
        struct WinsockInit
        {
            WinsockInit()
            {
                WSADATA wsaData;
                WSAStartup(MAKEWORD(2, 2), &wsaData);
            }
            ~WinsockInit() { WSACleanup(); }
        };

        void EnsureWinsock()
        {
            static WinsockInit init;
        }
#else
        void EnsureWinsock() {}
#endif

        // Set socket receive/send timeout (seconds)
        void SetSocketTimeout(int sock, int timeoutSec)
        {
#ifdef _WIN32
            DWORD timeoutMs = static_cast<DWORD>(timeoutSec * 1000);
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
            timeval tv{timeoutSec, 0};
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
        }

        // Non-blocking connect with timeout (seconds). Returns true on success.
        bool ConnectWithTimeout(int sock, sockaddr* addr, socklen_t addrLen, int timeoutSec)
        {
#ifdef _WIN32
            u_long mode = 1;
            ioctlsocket(static_cast<SOCKET>(sock), FIONBIO, &mode);

            int result = ::connect(sock, addr, addrLen);
            if (result == 0)
            {
                mode = 0;
                ioctlsocket(static_cast<SOCKET>(sock), FIONBIO, &mode);
                return true;
            }

            if (WSAGetLastError() != WSAEWOULDBLOCK)
            {
                mode = 0;
                ioctlsocket(static_cast<SOCKET>(sock), FIONBIO, &mode);
                return false;
            }

            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(static_cast<SOCKET>(sock), &writeSet);
            timeval tv{timeoutSec, 0};
            int ready = ::select(0, nullptr, &writeSet, nullptr, &tv);

            mode = 0;
            ioctlsocket(static_cast<SOCKET>(sock), FIONBIO, &mode);
            return ready > 0;
#else
            int flags = fcntl(sock, F_GETFL, 0);
            fcntl(sock, F_SETFL, flags | O_NONBLOCK);

            int result = ::connect(sock, addr, addrLen);
            if (result == 0)
            {
                fcntl(sock, F_SETFL, flags);
                return true;
            }

            if (errno != EINPROGRESS)
            {
                fcntl(sock, F_SETFL, flags);
                return false;
            }

            pollfd pfd{};
            pfd.fd = sock;
            pfd.events = POLLOUT;
            int ready = ::poll(&pfd, 1, timeoutSec * 1000);

            fcntl(sock, F_SETFL, flags);

            if (ready <= 0)
                return false;

            int err = 0;
            socklen_t errLen = sizeof(err);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &errLen);
            return err == 0;
#endif
        }

    } // namespace

    // ============================================================================
    // Public Serialization API
    // ============================================================================

    std::vector<uint8_t> SerializeMessage(const InternalMessage& msg)
    {
        std::vector<uint8_t> buf;
        buf.reserve(256);

        WriteU8(buf, static_cast<uint8_t>(msg.type));
        WriteU32(buf, msg.sourcePeer);
        WriteString(buf, msg.nodeId);
        WriteString(buf, msg.payload);
        WriteU64(buf, msg.timestamp);
        WriteEditMessage(buf, msg.editMessage);
        WriteEditorPeer(buf, msg.peerInfo);

        return buf;
    }

    bool DeserializeMessage(const uint8_t* data, size_t size, InternalMessage& outMsg)
    {
        if (!data || size == 0)
            return false;

        Reader r{data, size, 0};
        if (!r.HasBytes(1))
            return false;

        outMsg.type = static_cast<InternalMessageType>(r.ReadU8());
        outMsg.sourcePeer = r.ReadU32();
        outMsg.nodeId = r.ReadString();
        outMsg.payload = r.ReadString();
        outMsg.timestamp = r.ReadU64();
        outMsg.editMessage = ReadEditMessage(r);
        outMsg.peerInfo = ReadEditorPeer(r);

        // If any read went out of bounds, the message is malformed
        if (r.failed)
            return false;

        return true;
    }

    // ============================================================================
    // Construction / Destruction
    // ============================================================================

    CollaborativeEditSession::CollaborativeEditSession()
    {
        EnsureWinsock();
    }

    CollaborativeEditSession::~CollaborativeEditSession()
    {
        Disconnect();
    }

    // ============================================================================
    // Connection
    // ============================================================================

    bool CollaborativeEditSession::Host(uint16_t port, const std::string& userName)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (userName.empty())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Cannot host: userName is empty.");
            return false;
        }
        if (m_connected.load(std::memory_order_acquire))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Already connected.");
            return false;
        }

        // Create TCP listen socket
        m_listenSocket = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (m_listenSocket < 0)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to create listen socket.");
            return false;
        }

        // Allow port reuse
        int optval = 1;
        setsockopt(m_listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&optval), sizeof(optval));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (::bind(m_listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to bind on port %u.", port);
            CLOSE_SOCKET(m_listenSocket);
            m_listenSocket = -1;
            return false;
        }

        if (::listen(m_listenSocket, 10) < 0)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to listen on port %u.", port);
            CLOSE_SOCKET(m_listenSocket);
            m_listenSocket = -1;
            return false;
        }

        m_isHost = true;
        m_localUserName = userName;
        m_localPeerID = AllocatePeerID();
        m_sessionTime = 0.0f;
        m_port = port;
        m_shuttingDown.store(false, std::memory_order_release);

        // Register self as a peer
        EditorPeer self;
        self.id = m_localPeerID;
        self.userName = userName;
        self.isActive = true;
        self.color = kPeerColors[0];

        {
            std::lock_guard<std::mutex> lock(m_peerMutex);
            m_peers[m_localPeerID] = self;
        }

        m_connected.store(true, std::memory_order_release);

        // Spawn accept thread
        m_networkThread = std::thread(&CollaborativeEditSession::NetworkThreadHost, this);

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Hosting collab session on port %u as '%s' (PeerID=%u).", port,
                       userName.c_str(), m_localPeerID);
        return true;
    }

    bool CollaborativeEditSession::Connect(const std::string& address, uint16_t port, const std::string& userName)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (address.empty())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Cannot connect: address is empty.");
            return false;
        }
        if (userName.empty())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Cannot connect: userName is empty.");
            return false;
        }
        if (m_connected.load(std::memory_order_acquire))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Already connected.");
            return false;
        }

        m_clientSocket = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (m_clientSocket < 0)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to create client socket.");
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, address.c_str(), &addr.sin_addr);

        if (!ConnectWithTimeout(m_clientSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr), 5))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to connect to %s:%u (timeout 5s).", address.c_str(),
                            port);
            CLOSE_SOCKET(m_clientSocket);
            m_clientSocket = -1;
            return false;
        }

        // Set recv/send timeouts so threads can check m_shuttingDown periodically
        SetSocketTimeout(m_clientSocket, 2);

        m_isHost = false;
        m_localUserName = userName;
        m_localPeerID = AllocatePeerID();
        m_sessionTime = 0.0f;
        m_port = port;
        m_hostAddress = address;
        m_shuttingDown.store(false, std::memory_order_release);

        // Register self as a peer
        EditorPeer self;
        self.id = m_localPeerID;
        self.userName = userName;
        self.isActive = true;
        self.color = kPeerColors[1];

        {
            std::lock_guard<std::mutex> lock(m_peerMutex);
            m_peers[m_localPeerID] = self;
        }

        m_connected.store(true, std::memory_order_release);

        // Send PeerConnect handshake to host
        InternalMessage handshake;
        handshake.type = InternalMessageType::PeerConnect;
        handshake.sourcePeer = m_localPeerID;
        handshake.peerInfo = self;
        handshake.timestamp = 0;
        auto data = SerializeMessage(handshake);
        SendFramed(m_clientSocket, data);

        // Spawn receive thread
        m_networkThread = std::thread(&CollaborativeEditSession::NetworkThreadClient, this);

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Connected to %s:%u as '%s' (PeerID=%u).", address.c_str(), port,
                       userName.c_str(), m_localPeerID);
        return true;
    }

    void CollaborativeEditSession::Disconnect()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (!m_connected.load(std::memory_order_acquire))
            return;

        m_shuttingDown.store(true, std::memory_order_release);
        m_connected.store(false, std::memory_order_release);

        // Release all locks held by the local editor
        {
            std::lock_guard<std::mutex> lock(m_lockMutex);
            auto it = m_nodeLocks.begin();
            while (it != m_nodeLocks.end())
            {
                if (it->second.ownerPeer == m_localPeerID)
                {
                    it = m_nodeLocks.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        // Shutdown sockets first to unblock any recv()/accept() calls in network threads,
        // then join threads, then close the fds. This avoids close()/recv() races.
        ShutdownAllSockets();

        if (m_networkThread.joinable())
            m_networkThread.join();

        for (auto& t : m_clientThreads)
        {
            if (t.joinable())
                t.join();
        }
        m_clientThreads.clear();

        CloseAllSockets();

        {
            std::lock_guard<std::mutex> lock(m_peerMutex);
            m_peers.clear();
        }

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Disconnected from session.");
    }

    void CollaborativeEditSession::ShutdownAllSockets()
    {
        std::lock_guard<std::mutex> lock(m_socketMutex);

        // shutdown() unblocks recv()/accept() in network threads without closing the fd
#ifdef _WIN32
        auto shutdownSock = [](int fd) { ::shutdown(static_cast<SOCKET>(fd), SD_BOTH); };
#else
        auto shutdownSock = [](int fd) { ::shutdown(fd, SHUT_RDWR); };
#endif

        if (m_listenSocket >= 0)
            shutdownSock(m_listenSocket);

        if (m_clientSocket >= 0)
            shutdownSock(m_clientSocket);

        for (auto& [peerId, sock] : m_peerSockets)
        {
            if (sock >= 0)
                shutdownSock(sock);
        }
    }

    void CollaborativeEditSession::CloseAllSockets()
    {
        std::lock_guard<std::mutex> lock(m_socketMutex);

        if (m_listenSocket >= 0)
        {
            CLOSE_SOCKET(m_listenSocket);
            m_listenSocket = -1;
        }

        if (m_clientSocket >= 0)
        {
            CLOSE_SOCKET(m_clientSocket);
            m_clientSocket = -1;
        }

        for (auto& [peerId, sock] : m_peerSockets)
        {
            if (sock >= 0)
                CLOSE_SOCKET(sock);
        }
        m_peerSockets.clear();
    }

    // ============================================================================
    // Network Threads
    // ============================================================================

    void CollaborativeEditSession::NetworkThreadHost()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Host accept thread started.");

        while (!m_shuttingDown.load(std::memory_order_acquire))
        {
            // Snapshot the listen socket under the lock to avoid racing with CloseAllSockets()
            int listenFd;
            {
                std::lock_guard<std::mutex> lock(m_socketMutex);
                listenFd = m_listenSocket;
            }
            if (listenFd < 0)
                break;

                // Use poll/select with timeout to allow shutdown checks
#ifdef _WIN32
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(static_cast<SOCKET>(listenFd), &readSet);
            timeval tv{0, 500000}; // 500ms
            int ready = ::select(0, &readSet, nullptr, nullptr, &tv);
#else
            pollfd pfd{};
            pfd.fd = listenFd;
            pfd.events = POLLIN;
            int ready = ::poll(&pfd, 1, 500);
#endif

            if (ready <= 0)
                continue;

            sockaddr_in clientAddr{};
            socklen_t addrLen = sizeof(clientAddr);
            int clientSock = static_cast<int>(::accept(listenFd, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen));
            if (clientSock < 0)
                continue;

            // Set recv timeout so handler thread can check m_shuttingDown
            SetSocketTimeout(clientSock, 2);

            PeerID newPeerId = AllocatePeerID();
            {
                std::lock_guard<std::mutex> lock(m_socketMutex);
                m_peerSockets[newPeerId] = clientSock;
            }

            // Spawn a thread to handle this client's messages
            m_clientThreads.emplace_back(&CollaborativeEditSession::HandleClientSocket, this, clientSock, newPeerId);

            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Accepted client connection (PeerID=%u).", newPeerId);
        }

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Host accept thread stopped.");
    }

    void CollaborativeEditSession::HandleClientSocket(int clientSocket, PeerID peerId)
    {
        while (!m_shuttingDown.load(std::memory_order_acquire))
        {
            auto data = RecvFramed(clientSocket, m_shuttingDown);
            if (data.empty())
                break;

            InternalMessage msg;
            if (!DeserializeMessage(data.data(), data.size(), msg))
                continue;

            // If this is a PeerConnect, assign the server-side peer ID and register
            if (msg.type == InternalMessageType::PeerConnect)
            {
                msg.peerInfo.id = peerId;
                msg.sourcePeer = peerId;

                // Assign a color based on peer ID
                size_t colorIdx = peerId % (sizeof(kPeerColors) / sizeof(kPeerColors[0]));
                msg.peerInfo.color = kPeerColors[colorIdx];
            }
            else
            {
                // Remap source peer to server-assigned ID
                msg.sourcePeer = peerId;
                if (msg.type == InternalMessageType::EditBroadcast)
                    msg.editMessage.sourceEditor = peerId;
            }

            // Push to incoming queue for main thread processing
            {
                std::lock_guard<std::mutex> lock(m_messageMutex);
                m_incomingMessages.push(msg);
            }

            // Relay to all other connected clients (host-mediated broadcast)
            {
                std::lock_guard<std::mutex> lock(m_socketMutex);
                auto relayData = SerializeMessage(msg);
                for (auto& [otherPeerId, otherSock] : m_peerSockets)
                {
                    if (otherPeerId != peerId && otherSock >= 0)
                    {
                        SendFramed(otherSock, relayData);
                    }
                }
            }
        }

        // Peer disconnected — clean up
        {
            std::lock_guard<std::mutex> lock(m_socketMutex);
            auto it = m_peerSockets.find(peerId);
            if (it != m_peerSockets.end())
            {
                CLOSE_SOCKET(it->second);
                m_peerSockets.erase(it);
            }
        }

        // Queue a disconnect message
        InternalMessage disc;
        disc.type = InternalMessageType::PeerDisconnect;
        disc.sourcePeer = peerId;
        {
            std::lock_guard<std::mutex> lock(m_messageMutex);
            m_incomingMessages.push(disc);
        }

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Client PeerID=%u disconnected.", peerId);
    }

    void CollaborativeEditSession::NetworkThreadClient()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Client receive thread started.");

        while (!m_shuttingDown.load(std::memory_order_acquire))
        {
            int clientFd;
            {
                std::lock_guard<std::mutex> lock(m_socketMutex);
                clientFd = m_clientSocket;
            }
            if (clientFd < 0)
                break;

            auto data = RecvFramed(clientFd, m_shuttingDown);
            if (data.empty())
                break;

            InternalMessage msg;
            if (!DeserializeMessage(data.data(), data.size(), msg))
                continue;

            // Push to incoming queue for main thread processing
            {
                std::lock_guard<std::mutex> lock(m_messageMutex);
                m_incomingMessages.push(msg);
            }
        }

        // Host disconnected
        if (!m_shuttingDown.load(std::memory_order_acquire))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Lost connection to host.");
            m_connected.store(false, std::memory_order_release);
        }

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Client receive thread stopped.");
    }

    // ============================================================================
    // Network Send
    // ============================================================================

    void CollaborativeEditSession::SendToAllPeers(const InternalMessage& msg)
    {
        auto data = SerializeMessage(msg);

        if (m_isHost)
        {
            // Send to all connected client sockets
            std::lock_guard<std::mutex> lock(m_socketMutex);
            for (auto& [peerId, sock] : m_peerSockets)
            {
                if (sock >= 0)
                    SendFramed(sock, data);
            }
        }
        else
        {
            // Send to host
            if (m_clientSocket >= 0)
                SendFramed(m_clientSocket, data);
        }
    }

    void CollaborativeEditSession::SendToPeer(PeerID peerId, const InternalMessage& msg)
    {
        auto data = SerializeMessage(msg);
        std::lock_guard<std::mutex> lock(m_socketMutex);
        auto it = m_peerSockets.find(peerId);
        if (it != m_peerSockets.end() && it->second >= 0)
        {
            SendFramed(it->second, data);
        }
    }

    // ============================================================================
    // Update
    // ============================================================================

    void CollaborativeEditSession::Update(float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (!m_connected.load(std::memory_order_acquire))
            return;

        m_sessionTime += deltaTime;

        // Process incoming messages from the network thread
        ProcessIncomingMessages();

        // Drain outgoing queue and send over network
        {
            std::queue<InternalMessage> outgoing;
            {
                std::lock_guard<std::mutex> lock(m_messageMutex);
                std::swap(outgoing, m_outgoingMessages);
            }
            while (!outgoing.empty())
            {
                SendToAllPeers(outgoing.front());
                outgoing.pop();
            }
        }

        // Broadcast presence periodically
        m_presenceBroadcastTimer += deltaTime;
        if (m_presenceBroadcastTimer >= m_presenceBroadcastInterval)
        {
            m_presenceBroadcastTimer = 0.0f;
            BroadcastPresence();
        }

        // Expire stale locks
        ExpireStaleNodes();
    }

    // ============================================================================
    // Peer Awareness
    // ============================================================================

    std::vector<EditorPeer> CollaborativeEditSession::GetConnectedPeers() const
    {
        std::lock_guard<std::mutex> lock(m_peerMutex);
        std::vector<EditorPeer> result;
        result.reserve(m_peers.size());
        for (const auto& [id, peer] : m_peers)
        {
            result.push_back(peer);
        }
        return result;
    }

    const EditorPeer* CollaborativeEditSession::GetPeer(PeerID id) const
    {
        std::lock_guard<std::mutex> lock(m_peerMutex);
        auto it = m_peers.find(id);
        return it != m_peers.end() ? &it->second : nullptr;
    }

    void CollaborativeEditSession::SetLocalSelection(const std::string& nodeId)
    {
        if (nodeId.size() >= 256)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor,
                           "SetLocalSelection rejected: nodeId exceeds 255 chars (length=%zu).", nodeId.size());
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_peerMutex);
            auto it = m_peers.find(m_localPeerID);
            if (it != m_peers.end())
            {
                it->second.selectedNode = nodeId;
            }
        }

        InternalMessage msg;
        msg.type = InternalMessageType::SelectionChanged;
        msg.sourcePeer = m_localPeerID;
        msg.nodeId = nodeId;
        msg.timestamp = static_cast<uint64_t>(m_sessionTime * 1000.0f);

        {
            std::lock_guard<std::mutex> lock(m_messageMutex);
            m_outgoingMessages.push(std::move(msg));
        }
    }

    void CollaborativeEditSession::SetLocalViewportCamera(const DirectX::XMFLOAT3& position,
                                                          const DirectX::XMFLOAT3& direction)
    {
        std::lock_guard<std::mutex> lock(m_peerMutex);
        auto it = m_peers.find(m_localPeerID);
        if (it != m_peers.end())
        {
            it->second.viewportCameraPos = position;
            it->second.viewportCameraDir = direction;
        }
    }

    // ============================================================================
    // Node Locking
    // ============================================================================

    bool CollaborativeEditSession::RequestLock(const std::string& nodeId)
    {
        std::lock_guard<std::mutex> lock(m_lockMutex);

        auto it = m_nodeLocks.find(nodeId);
        if (it != m_nodeLocks.end())
        {
            if (it->second.ownerPeer == m_localPeerID)
                return true;
            return false;
        }

        NodeLock newLock;
        newLock.nodeId = nodeId;
        newLock.ownerPeer = m_localPeerID;
        newLock.ownerName = m_localUserName;
        newLock.lockTime = std::chrono::steady_clock::now();
        m_nodeLocks[nodeId] = newLock;

        if (m_onLockChanged)
            m_onLockChanged(nodeId, m_localPeerID);

        // Broadcast lock acquisition to peers
        InternalMessage msg;
        msg.type = InternalMessageType::LockRequest;
        msg.sourcePeer = m_localPeerID;
        msg.nodeId = nodeId;
        msg.timestamp = static_cast<uint64_t>(m_sessionTime * 1000.0f);
        {
            std::lock_guard<std::mutex> mlock(m_messageMutex);
            m_outgoingMessages.push(std::move(msg));
        }

        return true;
    }

    void CollaborativeEditSession::ReleaseLock(const std::string& nodeId)
    {
        std::lock_guard<std::mutex> lock(m_lockMutex);

        auto it = m_nodeLocks.find(nodeId);
        if (it != m_nodeLocks.end() && it->second.ownerPeer == m_localPeerID)
        {
            m_nodeLocks.erase(it);

            if (m_onLockChanged)
                m_onLockChanged(nodeId, INVALID_PEER);

            // Broadcast lock release to peers
            InternalMessage msg;
            msg.type = InternalMessageType::LockRelease;
            msg.sourcePeer = m_localPeerID;
            msg.nodeId = nodeId;
            msg.timestamp = static_cast<uint64_t>(m_sessionTime * 1000.0f);
            {
                std::lock_guard<std::mutex> mlock(m_messageMutex);
                m_outgoingMessages.push(std::move(msg));
            }
        }
    }

    PeerID CollaborativeEditSession::GetLockOwner(const std::string& nodeId) const
    {
        std::lock_guard<std::mutex> lock(m_lockMutex);
        auto it = m_nodeLocks.find(nodeId);
        return it != m_nodeLocks.end() ? it->second.ownerPeer : INVALID_PEER;
    }

    bool CollaborativeEditSession::IsLockedByLocal(const std::string& nodeId) const
    {
        return GetLockOwner(nodeId) == m_localPeerID;
    }

    std::vector<NodeLock> CollaborativeEditSession::GetAllLocks() const
    {
        std::lock_guard<std::mutex> lock(m_lockMutex);
        std::vector<NodeLock> result;
        result.reserve(m_nodeLocks.size());
        for (const auto& [id, nodeLock] : m_nodeLocks)
        {
            result.push_back(nodeLock);
        }
        return result;
    }

    // ============================================================================
    // Edit Broadcasting
    // ============================================================================

    void CollaborativeEditSession::BroadcastEdit(const EditMessage& message)
    {
        if (message.nodeId.empty())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "BroadcastEdit rejected: nodeId is empty.");
            return;
        }

        if (message.sourceEditor == INVALID_PEER)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "BroadcastEdit rejected: sourceEditor is not set.");
            return;
        }

        if (static_cast<uint8_t>(message.type) > static_cast<uint8_t>(EditMessageType::ComponentModified))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "BroadcastEdit rejected: invalid EditMessageType (%u).",
                           static_cast<unsigned>(message.type));
            return;
        }

        EditMessage outgoing = message;
        if (outgoing.timestamp == 0)
        {
            outgoing.timestamp = static_cast<uint64_t>(m_sessionTime * 1000.0f);
        }

        m_editsBroadcast++;

        InternalMessage msg;
        msg.type = InternalMessageType::EditBroadcast;
        msg.sourcePeer = outgoing.sourceEditor;
        msg.nodeId = outgoing.nodeId;
        msg.timestamp = outgoing.timestamp;
        msg.editMessage = outgoing;

        {
            std::lock_guard<std::mutex> lock(m_messageMutex);
            m_outgoingMessages.push(std::move(msg));
        }

        // Call local callback immediately
        if (m_onEditReceived)
            m_onEditReceived(outgoing);
    }

    // ============================================================================
    // Queries
    // ============================================================================

    CollaborativeEditSession::SessionStats CollaborativeEditSession::GetStats() const
    {
        SessionStats stats;
        {
            std::lock_guard<std::mutex> lock(m_peerMutex);
            stats.peerCount = static_cast<uint32_t>(m_peers.size());
        }
        {
            std::lock_guard<std::mutex> lock(m_lockMutex);
            stats.activeLocks = static_cast<uint32_t>(m_nodeLocks.size());
        }
        stats.editsBroadcast = m_editsBroadcast;
        stats.editsReceived = m_editsReceived;
        stats.sessionDuration = m_sessionTime;
        return stats;
    }

    std::string CollaborativeEditSession::Console_GetStatus() const
    {
        auto stats = GetStats();
        std::ostringstream oss;
        oss << "CollabEdit: " << (m_connected.load() ? "Connected" : "Disconnected")
            << (m_isHost ? " (Host)" : " (Client)") << " | Peers: " << stats.peerCount
            << " | Locks: " << stats.activeLocks << " | Edits sent/recv: " << stats.editsBroadcast << "/"
            << stats.editsReceived << " | Session: " << stats.sessionDuration << "s";
        return oss.str();
    }

    // ============================================================================
    // Internal
    // ============================================================================

    void CollaborativeEditSession::ProcessIncomingMessages()
    {
        std::queue<InternalMessage> localQueue;
        {
            std::lock_guard<std::mutex> lock(m_messageMutex);
            std::swap(localQueue, m_incomingMessages);
        }

        while (!localQueue.empty())
        {
            InternalMessage msg = std::move(localQueue.front());
            localQueue.pop();

            switch (msg.type)
            {
            case InternalMessageType::Presence:
            {
                std::lock_guard<std::mutex> lock(m_peerMutex);
                auto it = m_peers.find(msg.sourcePeer);
                if (it != m_peers.end())
                {
                    it->second.viewportCameraPos = msg.peerInfo.viewportCameraPos;
                    it->second.viewportCameraDir = msg.peerInfo.viewportCameraDir;
                    it->second.selectedNode = msg.peerInfo.selectedNode;
                    it->second.lastActivityTime = m_sessionTime;
                    it->second.isActive = true;
                }
                break;
            }

            case InternalMessageType::SelectionChanged:
            {
                std::lock_guard<std::mutex> lock(m_peerMutex);
                auto it = m_peers.find(msg.sourcePeer);
                if (it != m_peers.end())
                {
                    it->second.selectedNode = msg.nodeId;
                    it->second.lastActivityTime = m_sessionTime;
                }
                break;
            }

            case InternalMessageType::EditBroadcast:
            {
                m_editsReceived++;
                if (m_onEditReceived)
                    m_onEditReceived(msg.editMessage);
                break;
            }

            case InternalMessageType::LockRequest:
            {
                std::lock_guard<std::mutex> lock(m_lockMutex);
                auto it = m_nodeLocks.find(msg.nodeId);
                if (it == m_nodeLocks.end())
                {
                    NodeLock newLock;
                    newLock.nodeId = msg.nodeId;
                    newLock.ownerPeer = msg.sourcePeer;
                    newLock.lockTime = std::chrono::steady_clock::now();
                    m_nodeLocks[msg.nodeId] = newLock;

                    if (m_onLockChanged)
                        m_onLockChanged(msg.nodeId, msg.sourcePeer);
                }
                break;
            }

            case InternalMessageType::LockRelease:
            {
                std::lock_guard<std::mutex> lock(m_lockMutex);
                auto it = m_nodeLocks.find(msg.nodeId);
                if (it != m_nodeLocks.end() && it->second.ownerPeer == msg.sourcePeer)
                {
                    m_nodeLocks.erase(it);
                    if (m_onLockChanged)
                        m_onLockChanged(msg.nodeId, INVALID_PEER);
                }
                break;
            }

            case InternalMessageType::PeerConnect:
            {
                {
                    std::lock_guard<std::mutex> lock(m_peerMutex);
                    m_peers[msg.sourcePeer] = msg.peerInfo;
                    m_peers[msg.sourcePeer].lastActivityTime = m_sessionTime;
                }

                SPARK_LOG_INFO(Spark::LogCategory::Editor, "Peer connected: '%s' (PeerID=%u).",
                               msg.peerInfo.userName.c_str(), msg.sourcePeer);

                if (m_onPeerConnected)
                    m_onPeerConnected(msg.peerInfo);
                break;
            }

            case InternalMessageType::PeerDisconnect:
            {
                {
                    std::lock_guard<std::mutex> lock(m_peerMutex);
                    m_peers.erase(msg.sourcePeer);
                }

                // Release all locks held by the disconnected peer
                {
                    std::lock_guard<std::mutex> lock(m_lockMutex);
                    auto it = m_nodeLocks.begin();
                    while (it != m_nodeLocks.end())
                    {
                        if (it->second.ownerPeer == msg.sourcePeer)
                        {
                            std::string nodeId = it->first;
                            it = m_nodeLocks.erase(it);
                            if (m_onLockChanged)
                                m_onLockChanged(nodeId, INVALID_PEER);
                        }
                        else
                        {
                            ++it;
                        }
                    }
                }

                if (m_onPeerDisconnected)
                    m_onPeerDisconnected(msg.sourcePeer);
                break;
            }
            }
        }
    }

    void CollaborativeEditSession::BroadcastPresence()
    {
        EditorPeer localPeerSnapshot;
        {
            std::lock_guard<std::mutex> lock(m_peerMutex);
            auto it = m_peers.find(m_localPeerID);
            if (it == m_peers.end())
                return;

            it->second.lastActivityTime = m_sessionTime;
            it->second.isActive = true;
            localPeerSnapshot = it->second;
        }

        InternalMessage msg;
        msg.type = InternalMessageType::Presence;
        msg.sourcePeer = m_localPeerID;
        msg.timestamp = static_cast<uint64_t>(m_sessionTime * 1000.0f);
        msg.peerInfo = localPeerSnapshot;

        {
            std::lock_guard<std::mutex> lock(m_messageMutex);
            m_outgoingMessages.push(std::move(msg));
        }
    }

    void CollaborativeEditSession::ExpireStaleNodes()
    {
        std::lock_guard<std::mutex> lock(m_lockMutex);
        auto now = std::chrono::steady_clock::now();

        auto it = m_nodeLocks.begin();
        while (it != m_nodeLocks.end())
        {
            auto elapsed = std::chrono::duration<float>(now - it->second.lockTime).count();
            if (elapsed > it->second.maxDurationSeconds)
            {
                std::string nodeId = it->first;
                it = m_nodeLocks.erase(it);

                if (m_onLockChanged)
                    m_onLockChanged(nodeId, INVALID_PEER);
            }
            else
            {
                ++it;
            }
        }
    }

    PeerID CollaborativeEditSession::AllocatePeerID()
    {
        return m_nextPeerID++;
    }

} // namespace SparkEditor
