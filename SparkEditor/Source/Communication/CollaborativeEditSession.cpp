/**
 * @file CollaborativeEditSession.cpp
 * @brief Multi-user collaborative editor session with TCP networking
 */

#include "CollaborativeEditSession.h"
#include "StandaloneCollaborationClient.h"
#include "Engine/Networking/NetworkBindPolicy.h"
#include "Utils/PasswordHash.h"
#include "Utils/SecureRandom.h"
#include "Utils/Validate.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#endif

namespace SparkEditor
{

    // ============================================================================
    // Wire Protocol — length-prefixed binary serialization
    // ============================================================================

    namespace
    {
#ifdef _WIN32
        using NativeSocket = SOCKET;

        NativeSocket ToNativeSocket(CollaborativeSocketHandle socket)
        {
            return static_cast<NativeSocket>(socket);
        }

        CollaborativeSocketHandle ToStoredSocket(NativeSocket socket)
        {
            return static_cast<CollaborativeSocketHandle>(socket);
        }
#else
        using NativeSocket = int;

        NativeSocket ToNativeSocket(CollaborativeSocketHandle socket)
        {
            return socket;
        }

        CollaborativeSocketHandle ToStoredSocket(NativeSocket socket)
        {
            return socket;
        }
#endif

        bool IsValidSocket(CollaborativeSocketHandle socket)
        {
            return socket != INVALID_COLLAB_SOCKET;
        }

        void CloseSocket(CollaborativeSocketHandle socket)
        {
            if (!IsValidSocket(socket))
                return;
#ifdef _WIN32
            ::closesocket(ToNativeSocket(socket));
#else
            ::close(ToNativeSocket(socket));
#endif
        }

        void ConfigureSigPipeSuppression(CollaborativeSocketHandle socket)
        {
#if !defined(_WIN32) && defined(SO_NOSIGPIPE)
            const int enabled = 1;
            (void)::setsockopt(ToNativeSocket(socket), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#else
            (void)socket;
#endif
        }

        int SendFlags()
        {
#if !defined(_WIN32) && defined(MSG_NOSIGNAL)
            return MSG_NOSIGNAL;
#else
            return 0;
#endif
        }

        // Send length-prefixed message over TCP (thread-safe per socket)
        bool SendFramed(CollaborativeSocketHandle sock, const std::vector<uint8_t>& data)
        {
            if (!IsValidSocket(sock) || data.empty())
                return false;

            uint32_t len = static_cast<uint32_t>(data.size());
            uint8_t header[4];
            header[0] = static_cast<uint8_t>((len >> 24) & 0xFF);
            header[1] = static_cast<uint8_t>((len >> 16) & 0xFF);
            header[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
            header[3] = static_cast<uint8_t>(len & 0xFF);

            auto sendAll = [](CollaborativeSocketHandle s, const void* buf, size_t totalLen) -> bool
            {
                const auto* ptr = static_cast<const char*>(buf);
                size_t sent = 0;
                while (sent < totalLen)
                {
                    auto n = ::send(ToNativeSocket(s), ptr + sent, static_cast<int>(totalLen - sent), SendFlags());
                    if (n <= 0)
                        return false;
                    sent += static_cast<size_t>(n);
                }
                return true;
            };

            return sendAll(sock, header, 4) && sendAll(sock, data.data(), data.size());
        }

        using SteadyClock = std::chrono::steady_clock;

        // Receive one length-prefixed frame from TCP.
        // Socket should have SO_RCVTIMEO set so recv() returns periodically,
        // allowing us to check shuttingDown and the deadlines. Returns empty on
        // disconnect, error, an oversized length, or a missed deadline:
        //  - firstByteDeadline bounds the whole receive, including the wait for the
        //    frame to start (SteadyClock::time_point::max() waits indefinitely
        //    between frames);
        //  - once the first byte arrives, the whole frame must land within
        //    kCollabFrameCompletionSeconds, so a peer that sends a header and then
        //    stalls cannot pin a buffer and a thread forever.
        // The payload buffer grows with the bytes actually received instead of being
        // sized from the untrusted length prefix up front.
        std::vector<uint8_t> RecvFramed(CollaborativeSocketHandle sock, const std::atomic<bool>& shuttingDown,
                                        uint32_t maxFrameBytes,
                                        SteadyClock::time_point firstByteDeadline = SteadyClock::time_point::max())
        {
            SteadyClock::time_point deadline = firstByteDeadline;
            bool frameStarted = false;

            auto recvSome = [&](char* ptr, size_t wanted) -> size_t
            {
                while (true)
                {
                    if (shuttingDown.load(std::memory_order_acquire))
                        return 0;
                    if (SteadyClock::now() > deadline)
                        return 0;
                    auto n = ::recv(ToNativeSocket(sock), ptr, static_cast<int>(wanted), 0);
                    if (n > 0)
                    {
                        if (!frameStarted)
                        {
                            // Never extends a caller's absolute deadline (the handshake's).
                            frameStarted = true;
                            deadline = std::min<SteadyClock::time_point>(
                                deadline, SteadyClock::now() + std::chrono::seconds(kCollabFrameCompletionSeconds));
                        }
                        return static_cast<size_t>(n);
                    }
                    if (n == 0)
                        return 0; // Clean disconnect
#ifdef _WIN32
                    if (WSAGetLastError() == WSAETIMEDOUT)
                        continue; // Timeout — retry after checking shuttingDown/deadline
#else
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        continue; // Timeout — retry after checking shuttingDown/deadline
#endif
                    return 0; // Real error
                }
            };

            uint8_t header[4];
            size_t headerReceived = 0;
            while (headerReceived < sizeof(header))
            {
                const size_t n =
                    recvSome(reinterpret_cast<char*>(header) + headerReceived, sizeof(header) - headerReceived);
                if (n == 0)
                    return {};
                headerReceived += n;
            }

            uint32_t len = (static_cast<uint32_t>(header[0]) << 24) | (static_cast<uint32_t>(header[1]) << 16) |
                           (static_cast<uint32_t>(header[2]) << 8) | static_cast<uint32_t>(header[3]);
            if (len == 0 || len > maxFrameBytes)
                return {};

            constexpr size_t kReceiveChunkBytes = 64 * 1024;
            std::vector<uint8_t> data;
            while (data.size() < len)
            {
                const size_t received = data.size();
                const size_t chunk = std::min(kReceiveChunkBytes, static_cast<size_t>(len) - received);
                data.resize(received + chunk);
                size_t filled = 0;
                while (filled < chunk)
                {
                    const size_t n = recvSome(reinterpret_cast<char*>(data.data() + received + filled), chunk - filled);
                    if (n == 0)
                        return {};
                    filled += n;
                }
            }

            return data;
        }

#ifdef _WIN32
        struct WinsockInit
        {
            bool started = false;

            WinsockInit()
            {
                WSADATA wsaData{};
                const int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
                started = result == 0;
                if (!started)
                    SPARK_LOG_ERROR(Spark::LogCategory::Editor, "WSAStartup failed with error: %d", result);
            }
            ~WinsockInit()
            {
                if (started)
                    WSACleanup();
            }
        };

        // False when Winsock could not be started: callers must not touch the socket API.
        bool EnsureWinsock()
        {
            static WinsockInit init;
            return init.started;
        }
#else
        bool EnsureWinsock()
        {
            return true;
        }
#endif

        // Set socket receive/send timeout (seconds)
        void SetSocketTimeout(CollaborativeSocketHandle sock, int timeoutSec)
        {
            const NativeSocket nativeSocket = ToNativeSocket(sock);
#ifdef _WIN32
            DWORD timeoutMs = static_cast<DWORD>(timeoutSec * 1000);
            setsockopt(nativeSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs),
                       sizeof(timeoutMs));
            setsockopt(nativeSocket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeoutMs),
                       sizeof(timeoutMs));
#else
            timeval tv{timeoutSec, 0};
            setsockopt(nativeSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(nativeSocket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
        }

        // Non-blocking connect with timeout (seconds). Returns true on success.
        bool ConnectWithTimeout(CollaborativeSocketHandle sock, sockaddr* addr, socklen_t addrLen, int timeoutSec,
                                const Spark::Net::NetworkEndpointPolicy& endpointPolicy)
        {
            if (!endpointPolicy.IsValid() || addr == nullptr || addrLen < static_cast<socklen_t>(sizeof(sockaddr_in)) ||
                addr->sa_family != AF_INET)
                return false;
            const auto* ipv4Address = reinterpret_cast<const sockaddr_in*>(addr);
            if (!endpointPolicy.AllowsPeerAddress(ntohl(ipv4Address->sin_addr.s_addr)))
                return false;

            const NativeSocket nativeSocket = ToNativeSocket(sock);
#ifdef _WIN32
            u_long mode = 1;
            if (::ioctlsocket(nativeSocket, FIONBIO, &mode) == SOCKET_ERROR)
                return false;

            int result = ::connect(nativeSocket, addr, addrLen);
            if (result == 0)
            {
                mode = 0;
                ::ioctlsocket(nativeSocket, FIONBIO, &mode);
                return true;
            }

            if (WSAGetLastError() != WSAEWOULDBLOCK)
            {
                mode = 0;
                ::ioctlsocket(nativeSocket, FIONBIO, &mode);
                return false;
            }

            fd_set writeSet;
            fd_set errorSet;
            FD_ZERO(&writeSet);
            FD_ZERO(&errorSet);
            FD_SET(nativeSocket, &writeSet);
            FD_SET(nativeSocket, &errorSet);
            timeval tv{timeoutSec, 0};
            int ready = ::select(0, nullptr, &writeSet, &errorSet, &tv);

            mode = 0;
            ::ioctlsocket(nativeSocket, FIONBIO, &mode);
            if (ready <= 0)
                return false;

            int error = 0;
            int errorLength = sizeof(error);
            return ::getsockopt(nativeSocket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &errorLength) ==
                       0 &&
                   error == 0;
#else
            int flags = fcntl(nativeSocket, F_GETFL, 0);
            if (flags < 0 || fcntl(nativeSocket, F_SETFL, flags | O_NONBLOCK) != 0)
                return false;

            int result = ::connect(nativeSocket, addr, addrLen);
            if (result == 0)
            {
                fcntl(nativeSocket, F_SETFL, flags);
                return true;
            }

            if (errno != EINPROGRESS)
            {
                fcntl(nativeSocket, F_SETFL, flags);
                return false;
            }

            pollfd pfd{};
            pfd.fd = nativeSocket;
            pfd.events = POLLOUT;
            int ready = ::poll(&pfd, 1, timeoutSec * 1000);

            fcntl(nativeSocket, F_SETFL, flags);

            if (ready <= 0)
                return false;

            int err = 0;
            socklen_t errLen = sizeof(err);
            return getsockopt(nativeSocket, SOL_SOCKET, SO_ERROR, &err, &errLen) == 0 && err == 0;
#endif
        }

    } // namespace

    bool IsValidCollabJoinCode(std::string_view joinCode)
    {
        return joinCode.size() == kCollabJoinSecretBytes * 2 &&
               std::all_of(joinCode.begin(), joinCode.end(),
                           [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }

    std::string ComputeCollabJoinProof(std::string_view joinCode, std::string_view nonce, std::string_view userName)
    {
        static constexpr std::string_view kDomain = "SparkCollabJoin/v1";
        std::vector<uint8_t> message;
        message.reserve(kDomain.size() + nonce.size() + userName.size());
        message.insert(message.end(), kDomain.begin(), kDomain.end());
        message.insert(message.end(), nonce.begin(), nonce.end());
        message.insert(message.end(), userName.begin(), userName.end());

        const auto* keyBytes = reinterpret_cast<const uint8_t*>(joinCode.data());
        const auto digest = Spark::PasswordHash::ComputeHmacSha256(std::span<const uint8_t>(keyBytes, joinCode.size()),
                                                                   std::span<const uint8_t>(message));

        static constexpr char kHexDigits[] = "0123456789abcdef";
        std::string hex(digest.size() * 2, '\0');
        for (size_t i = 0; i < digest.size(); ++i)
        {
            hex[i * 2] = kHexDigits[digest[i] >> 4];
            hex[i * 2 + 1] = kHexDigits[digest[i] & 0x0f];
        }
        return hex;
    }

    namespace
    {
        // Compares two strings without an early exit on the first mismatching byte.
        bool ConstantTimeEquals(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
                return false;
            unsigned char difference = 0;
            for (size_t i = 0; i < a.size(); ++i)
                difference |= static_cast<unsigned char>(a[i] ^ b[i]);
            return difference == 0;
        }
    } // namespace

    // ============================================================================
    // Construction / Destruction
    // ============================================================================

    CollaborativeEditSession::CollaborativeEditSession()
    {
        // Eager start-up only: Host()/Connect() re-check and fail when Winsock is unavailable.
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
        return Host(port, userName, Spark::Net::CaptureNetworkEndpointPolicy());
    }

    bool CollaborativeEditSession::Host(uint16_t port, const std::string& userName,
                                        const Spark::Net::NetworkEndpointPolicy& endpointPolicy)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (userName.empty() || userName.size() > kCollabMaxUserNameBytes)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Cannot host: userName is empty or too long.");
            return false;
        }
        if (m_connected.load(std::memory_order_acquire))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Already connected.");
            return false;
        }

        if (!endpointPolicy.IsValid())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Cannot host: %s.",
                            Spark::Net::NetworkEndpointPolicyErrorText(endpointPolicy.Error()).data());
            return false;
        }
        m_endpointPolicy = endpointPolicy;

        if (!EnsureWinsock())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Cannot host: Winsock is unavailable.");
            return false;
        }

        // Create TCP listen socket
        m_listenSocket = ToStoredSocket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (!IsValidSocket(m_listenSocket))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to create listen socket.");
            return false;
        }

        // Allow port reuse
        int optval = 1;
        setsockopt(ToNativeSocket(m_listenSocket), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&optval),
                   sizeof(optval));
        ConfigureSigPipeSuppression(m_listenSocket);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(m_endpointPolicy.BindAddress());
        addr.sin_port = htons(port);

        if (::bind(ToNativeSocket(m_listenSocket), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to bind on port %u.", port);
            CloseSocket(m_listenSocket);
            m_listenSocket = INVALID_COLLAB_SOCKET;
            return false;
        }

        if (::listen(ToNativeSocket(m_listenSocket), 10) < 0)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to listen on port %u.", port);
            CloseSocket(m_listenSocket);
            m_listenSocket = INVALID_COLLAB_SOCKET;
            return false;
        }

        // Report the port actually bound (port 0 asks the OS to choose one).
        sockaddr_in boundAddr{};
        socklen_t boundLength = sizeof(boundAddr);
        if (::getsockname(ToNativeSocket(m_listenSocket), reinterpret_cast<sockaddr*>(&boundAddr), &boundLength) != 0)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to query the bound collaboration port.");
            CloseSocket(m_listenSocket);
            m_listenSocket = INVALID_COLLAB_SOCKET;
            return false;
        }

        // Every peer must prove knowledge of this secret before it is admitted.
        std::string joinCode = Spark::SecureRandom::HexToken(kCollabJoinSecretBytes);
        if (!IsValidCollabJoinCode(joinCode))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Cannot host: the OS random source is unavailable.");
            CloseSocket(m_listenSocket);
            m_listenSocket = INVALID_COLLAB_SOCKET;
            return false;
        }

        m_isHost = true;
        m_joinCode = std::move(joinCode);
        m_localUserName = userName;
        m_localPeerID = AllocatePeerID();
        m_sessionTime = 0.0f;
        m_port = ntohs(boundAddr.sin_port);
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

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Hosting collab session on port %u as '%s' (PeerID=%u).", m_port,
                       userName.c_str(), m_localPeerID);
        return true;
    }

    bool CollaborativeEditSession::Connect(const std::string& address, uint16_t port, const std::string& userName,
                                           const std::string& joinCode)
    {
        return Connect(address, port, userName, joinCode, Spark::Net::CaptureNetworkEndpointPolicy());
    }

    bool CollaborativeEditSession::Connect(const std::string& address, uint16_t port, const std::string& userName,
                                           const std::string& joinCode,
                                           const Spark::Net::NetworkEndpointPolicy& endpointPolicy)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (address.empty())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Cannot connect: address is empty.");
            return false;
        }
        if (userName.empty() || userName.size() > kCollabMaxUserNameBytes)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Cannot connect: userName is empty or too long.");
            return false;
        }
        if (!IsValidCollabJoinCode(joinCode))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Cannot connect: the join code is not a 64-digit hex code.");
            return false;
        }
        if (m_connected.load(std::memory_order_acquire))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Already connected.");
            return false;
        }

        uint32_t remoteAddress = 0;
        if (!endpointPolicy.IsValid() || !Spark::Net::ParseIPv4Address(address, remoteAddress) ||
            !endpointPolicy.AllowsPeerAddress(remoteAddress))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor,
                            "Cannot connect: destination is outside the captured endpoint policy.");
            return false;
        }
        m_endpointPolicy = endpointPolicy;

        if (!EnsureWinsock())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Cannot connect: Winsock is unavailable.");
            return false;
        }

        m_clientSocket = ToStoredSocket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (!IsValidSocket(m_clientSocket))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to create client socket.");
            return false;
        }

        sockaddr_in localAddr{};
        localAddr.sin_family = AF_INET;
        localAddr.sin_port = 0;
        localAddr.sin_addr.s_addr = htonl(m_endpointPolicy.BindAddress());
        if (::bind(ToNativeSocket(m_clientSocket), reinterpret_cast<sockaddr*>(&localAddr), sizeof(localAddr)) < 0)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor,
                            "Failed to bind collaboration client to its requested interface.");
            CloseSocket(m_clientSocket);
            m_clientSocket = INVALID_COLLAB_SOCKET;
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(remoteAddress);

        if (!ConnectWithTimeout(m_clientSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr), 5, m_endpointPolicy))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Failed to connect to %s:%u (timeout 5s).", address.c_str(),
                            port);
            CloseSocket(m_clientSocket);
            m_clientSocket = INVALID_COLLAB_SOCKET;
            return false;
        }

        // Set recv/send timeouts so threads can check m_shuttingDown periodically
        ConfigureSigPipeSuppression(m_clientSocket);
        SetSocketTimeout(m_clientSocket, 2);

        // Prove knowledge of the join code before anything else is exchanged. The
        // host answers with the PeerID it assigned, which this editor adopts so its
        // own messages and the host's view of it agree.
        m_shuttingDown.store(false, std::memory_order_release);
        const PeerID assignedPeerId = AuthenticateToHost(userName, joinCode);
        if (assignedPeerId == INVALID_PEER)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "The collaboration host at %s:%u rejected the join handshake.",
                            address.c_str(), port);
            CloseSocket(m_clientSocket);
            m_clientSocket = INVALID_COLLAB_SOCKET;
            return false;
        }

        m_isHost = false;
        m_localUserName = userName;
        m_localPeerID = assignedPeerId;
        m_sessionTime = 0.0f;
        m_port = port;
        m_hostAddress = address;

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

        if (m_standaloneClient)
        {
            DisconnectStandaloneBroker();
            return;
        }

        // Release all locks held by the local editor
        {
            std::lock_guard<std::mutex> lock(m_lockMutex);
            std::erase_if(m_nodeLocks, [this](const auto& pair) { return pair.second.ownerPeer == m_localPeerID; });
        }

        // Shutdown sockets first to unblock any recv()/accept() calls in network threads,
        // then join threads, then close the fds. This avoids close()/recv() races.
        ShutdownAllSockets();

        if (m_networkThread.joinable())
            m_networkThread.join();

        {
            std::lock_guard<std::mutex> lock(m_clientThreadsMutex);
            for (auto& conn : m_clientThreads)
            {
                if (conn.thread.joinable())
                    conn.thread.join();
            }
            m_clientThreads.clear();
        }

        CloseAllSockets();
        m_joinCode.clear();

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
        auto shutdownSock = [](CollaborativeSocketHandle fd) { ::shutdown(ToNativeSocket(fd), SD_BOTH); };
#else
        auto shutdownSock = [](CollaborativeSocketHandle fd) { ::shutdown(ToNativeSocket(fd), SHUT_RDWR); };
#endif

        if (IsValidSocket(m_listenSocket))
            shutdownSock(m_listenSocket);

        if (IsValidSocket(m_clientSocket))
            shutdownSock(m_clientSocket);

        for (auto& [peerId, sock] : m_peerSockets)
        {
            if (IsValidSocket(sock))
                shutdownSock(sock);
        }
    }

    void CollaborativeEditSession::CloseAllSockets()
    {
        std::lock_guard<std::mutex> lock(m_socketMutex);

        if (IsValidSocket(m_listenSocket))
        {
            CloseSocket(m_listenSocket);
            m_listenSocket = INVALID_COLLAB_SOCKET;
        }

        if (IsValidSocket(m_clientSocket))
        {
            CloseSocket(m_clientSocket);
            m_clientSocket = INVALID_COLLAB_SOCKET;
        }

        for (auto& [peerId, sock] : m_peerSockets)
        {
            if (IsValidSocket(sock))
                CloseSocket(sock);
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
            CollaborativeSocketHandle listenFd;
            {
                std::lock_guard<std::mutex> lock(m_socketMutex);
                listenFd = m_listenSocket;
            }
            if (!IsValidSocket(listenFd))
            {
                break;
            }

            // Use poll/select with timeout to allow shutdown checks
#ifdef _WIN32
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(ToNativeSocket(listenFd), &readSet);
            timeval tv{0, 500000}; // 500ms
            int ready = ::select(0, &readSet, nullptr, nullptr, &tv);
#else
            pollfd pfd{};
            pfd.fd = ToNativeSocket(listenFd);
            pfd.events = POLLIN;
            int ready = ::poll(&pfd, 1, 500);
#endif

            if (ready <= 0)
                continue;

            sockaddr_in clientAddr{};
            socklen_t addrLen = sizeof(clientAddr);
            CollaborativeSocketHandle clientSock =
                ToStoredSocket(::accept(ToNativeSocket(listenFd), reinterpret_cast<sockaddr*>(&clientAddr), &addrLen));
            if (!IsValidSocket(clientSock))
                continue;
            if (clientAddr.sin_family != AF_INET ||
                !m_endpointPolicy.AllowsPeerAddress(ntohl(clientAddr.sin_addr.s_addr)))
            {
                CloseSocket(clientSock);
                continue;
            }

            // Bound concurrent connections (pending handshakes included) before any
            // per-connection thread or buffer exists: each connection costs a thread.
            ReapFinishedClientThreads();
            {
                std::lock_guard<std::mutex> lock(m_clientThreadsMutex);
                if (m_clientThreads.size() >= kCollabMaxPeerConnections)
                {
                    SPARK_LOG_WARN(Spark::LogCategory::Editor,
                                   "Refusing collaboration connection: %zu connections already open.",
                                   m_clientThreads.size());
                    CloseSocket(clientSock);
                    continue;
                }
            }

            // Set recv timeout so handler thread can check m_shuttingDown
            ConfigureSigPipeSuppression(clientSock);
            SetSocketTimeout(clientSock, 2);

            // The socket is registered in m_peerSockets (and so receives relayed
            // traffic) only after HandleClientSocket has verified the join proof.
            PeerID newPeerId = AllocatePeerID();

            // Spawn a thread to handle this client's messages. The shared flag lets the
            // main thread reap this handler once it exits (see ReapFinishedClientThreads).
            auto finished = std::make_shared<std::atomic<bool>>(false);
            std::thread handler(&CollaborativeEditSession::HandleClientSocket, this, clientSock, newPeerId, finished);
            {
                std::lock_guard<std::mutex> lock(m_clientThreadsMutex);
                m_clientThreads.push_back({std::move(handler), std::move(finished)});
            }

            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Accepted client connection (PeerID=%u), awaiting join proof.",
                           newPeerId);
        }

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Host accept thread stopped.");
    }

    bool CollaborativeEditSession::AuthenticatePeer(CollaborativeSocketHandle clientSocket, PeerID peerId,
                                                    InternalMessage& outConnect)
    {
        std::array<uint8_t, kCollabChallengeNonceBytes> nonceBytes{};
        if (!Spark::SecureRandom::Fill(nonceBytes.data(), nonceBytes.size()))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Cannot challenge PeerID=%u: OS random source unavailable.",
                            peerId);
            return false;
        }
        const std::string nonce(reinterpret_cast<const char*>(nonceBytes.data()), nonceBytes.size());

        InternalMessage challenge;
        challenge.type = InternalMessageType::AuthChallenge;
        challenge.payload = nonce;
        if (!SendFramed(clientSocket, SerializeMessage(challenge)))
            return false;

        // The first and only frame accepted before authentication is a PeerConnect
        // carrying the join proof; it is small, and it must arrive promptly.
        const auto deadline = SteadyClock::now() + std::chrono::seconds(kCollabHandshakeTimeoutSeconds);
        const auto data = RecvFramed(clientSocket, m_shuttingDown, kCollabMaxHandshakeFrameBytes, deadline);
        InternalMessage connect;
        if (data.empty() || !DeserializeMessage(data.data(), data.size(), connect) ||
            connect.type != InternalMessageType::PeerConnect)
            return false;

        const std::string& userName = connect.peerInfo.userName;
        if (userName.empty() || userName.size() > kCollabMaxUserNameBytes)
            return false;
        if (!ConstantTimeEquals(connect.payload, ComputeCollabJoinProof(m_joinCode, nonce, userName)))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Rejected collaboration peer: invalid join proof.");
            return false;
        }

        // Identity and presentation are assigned by the host, never taken from the peer.
        connect.payload.clear();
        connect.sourcePeer = peerId;
        connect.peerInfo.id = peerId;
        connect.peerInfo.color = kPeerColors[peerId % (sizeof(kPeerColors) / sizeof(kPeerColors[0]))];
        outConnect = std::move(connect);
        return true;
    }

    PeerID CollaborativeEditSession::AuthenticateToHost(const std::string& userName, const std::string& joinCode)
    {
        const auto deadline = SteadyClock::now() + std::chrono::seconds(kCollabHandshakeTimeoutSeconds);

        const auto challengeData = RecvFramed(m_clientSocket, m_shuttingDown, kCollabMaxHandshakeFrameBytes, deadline);
        InternalMessage challenge;
        if (challengeData.empty() || !DeserializeMessage(challengeData.data(), challengeData.size(), challenge) ||
            challenge.type != InternalMessageType::AuthChallenge ||
            challenge.payload.size() != kCollabChallengeNonceBytes)
            return INVALID_PEER;

        InternalMessage connect;
        connect.type = InternalMessageType::PeerConnect;
        connect.payload = ComputeCollabJoinProof(joinCode, challenge.payload, userName);
        connect.peerInfo.userName = userName;
        connect.peerInfo.isActive = true;
        if (!SendFramed(m_clientSocket, SerializeMessage(connect)))
            return INVALID_PEER;

        const auto acceptData = RecvFramed(m_clientSocket, m_shuttingDown, kCollabMaxHandshakeFrameBytes, deadline);
        InternalMessage accepted;
        if (acceptData.empty() || !DeserializeMessage(acceptData.data(), acceptData.size(), accepted) ||
            accepted.type != InternalMessageType::AuthAccepted)
            return INVALID_PEER;
        return accepted.sourcePeer;
    }

    void CollaborativeEditSession::HandleClientSocket(CollaborativeSocketHandle clientSocket, PeerID peerId,
                                                      std::shared_ptr<std::atomic<bool>> finished)
    {
        // Nothing from this connection is queued, relayed or registered until the
        // peer has proven knowledge of the join code.
        InternalMessage connect;
        const bool authenticated = AuthenticatePeer(clientSocket, peerId, connect);
        if (authenticated)
        {
            InternalMessage accepted;
            accepted.type = InternalMessageType::AuthAccepted;
            accepted.sourcePeer = peerId;
            {
                // Register and acknowledge under the socket lock so no relay can reach
                // the peer ahead of its AuthAccepted frame.
                std::lock_guard<std::mutex> lock(m_socketMutex);
                SendFramed(clientSocket, SerializeMessage(accepted));
                m_peerSockets[peerId] = clientSocket;

                const auto announce = SerializeMessage(connect);
                for (auto& [otherPeerId, otherSock] : m_peerSockets)
                {
                    if (otherPeerId != peerId && IsValidSocket(otherSock))
                        SendFramed(otherSock, announce);
                }
            }
            EnqueueMessage(m_incomingMessages, std::move(connect), "incoming");
        }

        while (authenticated && !m_shuttingDown.load(std::memory_order_acquire))
        {
            auto data = RecvFramed(clientSocket, m_shuttingDown, kCollabMaxFrameBytes);
            if (data.empty())
                break;

            InternalMessage msg;
            if (!DeserializeMessage(data.data(), data.size(), msg))
                continue;

            // Session membership and lock arbitration are host-originated: a peer may
            // not re-announce itself, disconnect someone else, or forge a grant/denial.
            switch (msg.type)
            {
            case InternalMessageType::PeerConnect:
            case InternalMessageType::PeerDisconnect:
            case InternalMessageType::AuthChallenge:
            case InternalMessageType::AuthAccepted:
            case InternalMessageType::LockGranted:
            case InternalMessageType::LockDenied:
                continue;
            default:
                break;
            }

            // Remap source peer to server-assigned ID
            msg.sourcePeer = peerId;
            if (msg.type == InternalMessageType::EditBroadcast)
                msg.editMessage.sourceEditor = peerId;

            // Push to incoming queue for main thread processing (bounded)
            EnqueueMessage(m_incomingMessages, InternalMessage(msg), "incoming");

            // Lock arbitration is authoritative through the host: a LockRequest is
            // answered with LockGranted/LockDenied by ProcessIncomingMessages, so it
            // is never blind-relayed here.
            if (msg.type != InternalMessageType::LockRequest)
            {
                std::lock_guard<std::mutex> lock(m_socketMutex);
                auto relayData = SerializeMessage(msg);
                for (auto& [otherPeerId, otherSock] : m_peerSockets)
                {
                    if (otherPeerId != peerId && IsValidSocket(otherSock))
                    {
                        SendFramed(otherSock, relayData);
                    }
                }
            }
        }

        // Peer disconnected — clean up
        if (authenticated)
        {
            {
                std::lock_guard<std::mutex> lock(m_socketMutex);
                auto it = m_peerSockets.find(peerId);
                if (it != m_peerSockets.end())
                {
                    CloseSocket(it->second);
                    m_peerSockets.erase(it);
                }
            }

            // Queue a disconnect message
            InternalMessage disc;
            disc.type = InternalMessageType::PeerDisconnect;
            disc.sourcePeer = peerId;
            EnqueueMessage(m_incomingMessages, std::move(disc), "incoming");

            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Client PeerID=%u disconnected.", peerId);
        }
        else
        {
            // Never registered, so this thread is the socket's only owner.
            CloseSocket(clientSocket);
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "Connection PeerID=%u closed before authenticating.", peerId);
        }

        // Signal the main thread that this handler has exited so it can be reaped.
        if (finished)
        {
            finished->store(true, std::memory_order_release);
        }
    }

    void CollaborativeEditSession::NetworkThreadClient()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Client receive thread started.");

        while (!m_shuttingDown.load(std::memory_order_acquire))
        {
            CollaborativeSocketHandle clientFd;
            {
                std::lock_guard<std::mutex> lock(m_socketMutex);
                clientFd = m_clientSocket;
            }
            if (!IsValidSocket(clientFd))
                break;

            auto data = RecvFramed(clientFd, m_shuttingDown, kCollabMaxFrameBytes);
            if (data.empty())
                break;

            InternalMessage msg;
            if (!DeserializeMessage(data.data(), data.size(), msg))
                continue;

            // Push to incoming queue for main thread processing (bounded)
            EnqueueMessage(m_incomingMessages, std::move(msg), "incoming");
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
                if (IsValidSocket(sock))
                    SendFramed(sock, data);
            }
        }
        else
        {
            // Send to host
            if (IsValidSocket(m_clientSocket))
                SendFramed(m_clientSocket, data);
        }
    }

    void CollaborativeEditSession::SendToPeer(PeerID peerId, const InternalMessage& msg)
    {
        auto data = SerializeMessage(msg);
        std::lock_guard<std::mutex> lock(m_socketMutex);
        auto it = m_peerSockets.find(peerId);
        if (it != m_peerSockets.end() && IsValidSocket(it->second))
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

        if (m_standaloneClient)
        {
            UpdateStandaloneBroker(deltaTime);
            return;
        }

        // Process incoming messages from the network thread
        ProcessIncomingMessages();

        // Drain outgoing queue and send over network
        {
            std::queue<InternalMessage> outgoing = TakeQueuedMessages(m_outgoingMessages);
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

        // Reap finished client handler threads (host only).
        if (m_isHost)
        {
            ReapFinishedClientThreads();
        }
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

        if (m_standaloneClient)
        {
            m_presenceBroadcastTimer = m_presenceBroadcastInterval;
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

        EnqueueMessage(m_outgoingMessages, std::move(msg), "outgoing");
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
        if (m_standaloneClient)
            return RequestStandaloneLock(nodeId);

        std::lock_guard<std::mutex> lock(m_lockMutex);

        auto it = m_nodeLocks.find(nodeId);
        if (it != m_nodeLocks.end())
        {
            // Already locked (possibly by an authoritative grant relayed from the host):
            // succeed only if we already own it, otherwise the request is denied.
            return it->second.ownerPeer == m_localPeerID;
        }

        // Optimistically take the lock locally so the synchronous caller can proceed.
        // The host is the single authority: a client asks it to arbitrate (LockRequest)
        // and reconciles on the host's LockGranted/LockDenied reply; the host itself is
        // the authority, so it announces the grant to every peer directly (LockGranted).
        NodeLock newLock;
        newLock.nodeId = nodeId;
        newLock.ownerPeer = m_localPeerID;
        newLock.ownerName = m_localUserName;
        newLock.lockTime = std::chrono::steady_clock::now();
        m_nodeLocks[nodeId] = newLock;

        if (m_onLockChanged)
            m_onLockChanged(nodeId, m_localPeerID);

        InternalMessage msg;
        msg.type = m_isHost ? InternalMessageType::LockGranted : InternalMessageType::LockRequest;
        msg.sourcePeer = m_localPeerID;
        msg.nodeId = nodeId;
        msg.timestamp = static_cast<uint64_t>(m_sessionTime * 1000.0f);
        EnqueueMessage(m_outgoingMessages, std::move(msg), "outgoing");

        return true;
    }

    void CollaborativeEditSession::ReleaseLock(const std::string& nodeId)
    {
        if (m_standaloneClient)
        {
            ReleaseStandaloneLock(nodeId);
            return;
        }

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
            EnqueueMessage(m_outgoingMessages, std::move(msg), "outgoing");
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

        if (m_standaloneClient)
        {
            BroadcastStandaloneEdit(outgoing);
            return;
        }

        m_editsBroadcast++;

        InternalMessage msg;
        msg.type = InternalMessageType::EditBroadcast;
        msg.sourcePeer = outgoing.sourceEditor;
        msg.nodeId = outgoing.nodeId;
        msg.timestamp = outgoing.timestamp;
        msg.editMessage = outgoing;

        EnqueueMessage(m_outgoingMessages, std::move(msg), "outgoing");

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
            << (m_standaloneClient ? " (Standalone broker)" : (m_isHost ? " (Host)" : " (Client)"))
            << " | Peers: " << stats.peerCount << " | Locks: " << stats.activeLocks
            << " | Edits sent/recv: " << stats.editsBroadcast << "/" << stats.editsReceived
            << " | Session: " << stats.sessionDuration << "s";
        return oss.str();
    }

    // ============================================================================
    // Internal
    // ============================================================================

    void CollaborativeEditSession::ProcessIncomingMessages()
    {
        std::queue<InternalMessage> localQueue = TakeQueuedMessages(m_incomingMessages);

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
                // Only the host arbitrates locks. A client never receives a raw
                // LockRequest (the host replies with LockGranted/LockDenied instead),
                // but guard defensively.
                if (!m_isHost)
                    break;

                bool granted = false;
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
                        granted = true;
                    }
                    else
                    {
                        // Idempotent: re-granting to the current owner still succeeds.
                        granted = (it->second.ownerPeer == msg.sourcePeer);
                    }
                }

                if (granted && m_onLockChanged)
                    m_onLockChanged(msg.nodeId, msg.sourcePeer);

                // Reply authoritatively: a grant is announced to every peer so all
                // views converge on the new owner; a denial goes only to the requester.
                InternalMessage reply;
                reply.type = granted ? InternalMessageType::LockGranted : InternalMessageType::LockDenied;
                reply.sourcePeer = msg.sourcePeer;
                reply.nodeId = msg.nodeId;
                reply.timestamp = static_cast<uint64_t>(m_sessionTime * 1000.0f);

                if (granted)
                    SendToAllPeers(reply);
                else
                    SendToPeer(msg.sourcePeer, reply);
                break;
            }

            case InternalMessageType::LockGranted:
            {
                // Authoritative grant from the host: converge on the announced owner.
                // Only clients apply this; the host is already the source of truth.
                if (m_isHost)
                    break;

                {
                    std::lock_guard<std::mutex> lock(m_lockMutex);
                    NodeLock& nl = m_nodeLocks[msg.nodeId];
                    nl.nodeId = msg.nodeId;
                    nl.ownerPeer = msg.sourcePeer;
                    nl.lockTime = std::chrono::steady_clock::now();
                }

                if (m_onLockChanged)
                    m_onLockChanged(msg.nodeId, msg.sourcePeer);
                break;
            }

            case InternalMessageType::LockDenied:
            {
                // Authoritative denial from the host: if we optimistically self-granted
                // this node, roll it back so our view matches the host's.
                if (m_isHost)
                    break;

                bool revoked = false;
                {
                    std::lock_guard<std::mutex> lock(m_lockMutex);
                    auto it = m_nodeLocks.find(msg.nodeId);
                    if (it != m_nodeLocks.end() && it->second.ownerPeer == m_localPeerID &&
                        msg.sourcePeer == m_localPeerID)
                    {
                        m_nodeLocks.erase(it);
                        revoked = true;
                    }
                }

                if (revoked && m_onLockChanged)
                    m_onLockChanged(msg.nodeId, INVALID_PEER);
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

            case InternalMessageType::AuthChallenge:
            case InternalMessageType::AuthAccepted:
                // Handshake frames are consumed synchronously by Connect()/HandleClientSocket.
                break;
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

        EnqueueMessage(m_outgoingMessages, std::move(msg), "outgoing");
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

    namespace
    {
        // Heap-owned bytes a queued message pins (string payloads dominate).
        size_t ApproximateQueuedBytes(const InternalMessage& msg)
        {
            const EditMessage& edit = msg.editMessage;
            return sizeof(InternalMessage) + msg.nodeId.size() + msg.payload.size() + edit.nodeId.size() +
                   edit.componentType.size() + edit.propertyName.size() + edit.newValue.size() + edit.oldValue.size() +
                   msg.peerInfo.userName.size() + msg.peerInfo.selectedNode.size();
        }
    } // namespace

    void CollaborativeEditSession::EnqueueMessage(BoundedMessageQueue& queue, InternalMessage&& msg,
                                                  const char* queueName)
    {
        const size_t messageBytes = ApproximateQueuedBytes(msg);
        std::lock_guard<std::mutex> lock(m_messageMutex);

        // Drop the oldest messages until both the entry count and the byte budget
        // fit: a peer streaming faster than the main thread can drain must not be
        // able to exhaust the heap, whether with many small or fewer large frames.
        // Warn once per sustained overflow so the log is not flooded.
        bool dropped = false;
        while (!queue.messages.empty() && (queue.messages.size() >= kMaxQueuedMessages ||
                                           queue.queuedBytes + messageBytes > kCollabMaxQueuedBytes))
        {
            const size_t oldestBytes = ApproximateQueuedBytes(queue.messages.front());
            queue.queuedBytes -= std::min(queue.queuedBytes, oldestBytes);
            queue.messages.pop();
            dropped = true;
        }

        if (dropped && !queue.overflowWarned)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor,
                           "Collab %s message queue exceeded %zu entries or %zu bytes; dropping oldest.", queueName,
                           kMaxQueuedMessages, kCollabMaxQueuedBytes);
        }
        queue.overflowWarned = dropped;

        queue.queuedBytes += messageBytes;
        queue.messages.push(std::move(msg));
    }

    std::queue<InternalMessage> CollaborativeEditSession::TakeQueuedMessages(BoundedMessageQueue& queue)
    {
        std::queue<InternalMessage> taken;
        std::lock_guard<std::mutex> lock(m_messageMutex);
        std::swap(taken, queue.messages);
        queue.queuedBytes = 0;
        return taken;
    }

    void CollaborativeEditSession::ReapFinishedClientThreads()
    {
        std::lock_guard<std::mutex> lock(m_clientThreadsMutex);
        for (auto it = m_clientThreads.begin(); it != m_clientThreads.end();)
        {
            if (it->finished && it->finished->load(std::memory_order_acquire))
            {
                if (it->thread.joinable())
                    it->thread.join();
                it = m_clientThreads.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

} // namespace SparkEditor
