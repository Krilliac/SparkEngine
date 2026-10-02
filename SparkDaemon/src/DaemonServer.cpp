/**
 * @file DaemonServer.cpp
 * @brief Implementation of the AF_UNIX daemon server.
 */

#include "DaemonServer.h"
#include "Utils/DaemonFraming.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#if defined(_WIN32)
#include <processthreadsapi.h>
#include <sddl.h>
#include <securitybaseapi.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Spark::Daemon
{

    namespace
    {
        NativeSocket ToNative(std::intptr_t h) noexcept
        {
#if defined(_WIN32)
            return reinterpret_cast<NativeSocket>(h);
#else
            return static_cast<NativeSocket>(h);
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

#if !defined(_WIN32)
        bool IsSameUserPeer(int socket) noexcept
        {
#if defined(__linux__)
            ucred credentials{};
            socklen_t size = sizeof(credentials);
            return ::getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 &&
                   credentials.uid == ::geteuid();
#elif defined(__APPLE__)
            uid_t effectiveUser = 0;
            gid_t effectiveGroup = 0;
            return ::getpeereid(socket, &effectiveUser, &effectiveGroup) == 0 && effectiveUser == ::geteuid();
#else
            // The owner-only socket mode remains the authorization boundary on
            // POSIX targets without a peer-credential socket API.
            return true;
#endif
        }

        bool EndpointIsActive(const std::string& endpoint)
        {
            const int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (probe < 0)
                return true;
            const int flags = ::fcntl(probe, F_GETFL, 0);
            if (flags >= 0)
                (void)::fcntl(probe, F_SETFL, flags | O_NONBLOCK);
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            std::memcpy(address.sun_path, endpoint.data(), endpoint.size());
            if (::connect(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0)
            {
                ::close(probe);
                return true;
            }
            int connectError = errno;
            if (connectError == EINPROGRESS)
            {
                pollfd descriptor{probe, POLLOUT, 0};
                if (::poll(&descriptor, 1, 250) > 0)
                {
                    socklen_t length = sizeof(connectError);
                    if (::getsockopt(probe, SOL_SOCKET, SO_ERROR, &connectError, &length) != 0)
                        connectError = errno;
                }
                else
                {
                    connectError = ETIMEDOUT;
                }
            }
            ::close(probe);
            return connectError != ECONNREFUSED && connectError != ENOENT;
        }

        void UnlinkOwnedEndpoint(const std::string& endpoint, dev_t device, ino_t inode)
        {
            struct stat current
            {
            };
            if (::lstat(endpoint.c_str(), &current) == 0 && S_ISSOCK(current.st_mode) && current.st_dev == device &&
                current.st_ino == inode)
                (void)::unlink(endpoint.c_str());
        }
#else
        class OwnerOnlySecurity final
        {
          public:
            OwnerOnlySecurity()
            {
                constexpr wchar_t kDescriptor[] = L"D:P(A;;GA;;;OW)(A;;GA;;;SY)(A;;GA;;;BA)";
                if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(kDescriptor, SDDL_REVISION_1, &m_descriptor,
                                                                           nullptr))
                {
                    m_attributes.nLength = sizeof(m_attributes);
                    m_attributes.lpSecurityDescriptor = m_descriptor;
                    m_attributes.bInheritHandle = FALSE;
                }
            }

            ~OwnerOnlySecurity()
            {
                if (m_descriptor)
                    ::LocalFree(m_descriptor);
            }

            OwnerOnlySecurity(const OwnerOnlySecurity&) = delete;
            OwnerOnlySecurity& operator=(const OwnerOnlySecurity&) = delete;

            [[nodiscard]] bool IsValid() const noexcept { return m_descriptor != nullptr; }
            [[nodiscard]] SECURITY_ATTRIBUTES* Attributes() noexcept { return &m_attributes; }

          private:
            PSECURITY_DESCRIPTOR m_descriptor = nullptr;
            SECURITY_ATTRIBUTES m_attributes{};
        };

        bool IsSameUserPeer(HANDLE pipe) noexcept
        {
            ULONG clientProcessId = 0;
            return ::GetNamedPipeClientProcessId(pipe, &clientProcessId) != FALSE &&
                   ProcessRunsAsCurrentUser(clientProcessId);
        }

        HANDLE CreateListeningInstance(const std::wstring& pipeName, bool firstInstance,
                                       SECURITY_ATTRIBUTES* security) noexcept
        {
            // FILE_FLAG_FIRST_PIPE_INSTANCE fails the create when any process,
            // in any session, already holds an instance of this name, so the
            // daemon never silently joins a pipe another account squatted.
            const DWORD openMode = PIPE_ACCESS_DUPLEX | (firstInstance ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0u);
            return ::CreateNamedPipeW(pipeName.c_str(), openMode,
                                      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                      PIPE_UNLIMITED_INSTANCES, 64u * 1024u, 64u * 1024u, 0, security);
        }

        std::wstring EndpointMutexName(std::wstring_view endpoint)
        {
            uint64_t hash = UINT64_C(0xcbf29ce484222325);
            for (wchar_t character : endpoint)
            {
                hash ^= static_cast<uint64_t>(character);
                hash *= UINT64_C(0x100000001b3);
            }
            return L"Local\\SparkDaemon.Endpoint." + std::to_wstring(hash);
        }

        void WaitForPeerClose(HANDLE pipe) noexcept
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline)
            {
                DWORD available = 0;
                if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
#endif
    } // namespace

    DaemonServer::DaemonServer(size_t maximumClientWorkers)
        : m_maximumClientWorkers(std::clamp<size_t>(maximumClientWorkers, 1, 1024))
    {
    }

    DaemonServer::~DaemonServer()
    {
        Stop();
        std::list<ClientWorker> toJoin;
        {
            std::lock_guard lock(m_threadsMutex);
            toJoin = std::move(m_clientWorkers);
        }
        for (auto& worker : toJoin)
        {
            if (worker.thread.joinable())
                worker.thread.join();
        }
    }

    void DaemonServer::AddService(std::unique_ptr<ServiceBase> service)
    {
        if (!service)
            return;
        uint16_t key = static_cast<uint16_t>(service->GetServiceId());
        m_services[key] = std::move(service);
    }

    void DaemonServer::Stop()
    {
        // Run()'s poll loop wakes on its 500ms timer and observes the flag.
        // Avoids closing the listen fd from another thread, which is undefined
        // while accept() is blocked on it.
        m_shouldStop.store(true, std::memory_order_release);
    }

    Expected<void, std::string> DaemonServer::Run(const std::string& socketPath)
    {
#if defined(_WIN32)
        if (socketPath.empty())
            return Unexpected<std::string>("DaemonServer: pipe endpoint is empty");
        const std::wstring pipeName = NormalizePipeName(socketPath);
        if (pipeName.empty())
            return Unexpected<std::string>("DaemonServer: pipe endpoint is not valid UTF-8 or cannot be resolved");

        OwnerOnlySecurity endpointSecurity;
        if (!endpointSecurity.IsValid())
            return Unexpected<std::string>("DaemonServer: could not create owner-only endpoint security descriptor");

        HANDLE endpointMutex = ::CreateMutexW(endpointSecurity.Attributes(), TRUE, EndpointMutexName(pipeName).c_str());
        if (!endpointMutex)
            return Unexpected<std::string>("DaemonServer: could not create endpoint ownership mutex");
        bool ownsEndpoint = ::GetLastError() != ERROR_ALREADY_EXISTS;
        if (!ownsEndpoint)
        {
            const DWORD wait = ::WaitForSingleObject(endpointMutex, 0);
            ownsEndpoint = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        }
        if (!ownsEndpoint)
        {
            ::CloseHandle(endpointMutex);
            return Unexpected<std::string>("DaemonServer: endpoint is already owned by another process");
        }

        m_boundPath = socketPath;
        m_runStartedAt = std::chrono::steady_clock::now();
        std::string fatalError;

        // The server holds at least one instance of the name for its whole run:
        // the first is created with FILE_FLAG_FIRST_PIPE_INSTANCE, the next
        // listener is created before a connected instance is handed to a worker,
        // and rejected peers are disconnected from the same instance instead of
        // closing it. The name is therefore never released for another account
        // to re-create while the daemon runs.
        HANDLE listening = INVALID_HANDLE_VALUE;
        if (!m_shouldStop.load(std::memory_order_acquire))
        {
            listening = CreateListeningInstance(pipeName, /*firstInstance*/ true, endpointSecurity.Attributes());
            if (listening == INVALID_HANDLE_VALUE)
            {
                const DWORD error = ::GetLastError();
                fatalError = error == ERROR_ACCESS_DENIED
                                 ? "DaemonServer: pipe name is already in use by another process (error 5)"
                                 : "DaemonServer: CreateNamedPipeW failed (error " + std::to_string(error) + ")";
            }
            else
            {
                m_listenFd = FromNative(listening);
            }
        }

        while (listening != INVALID_HANDLE_VALUE && !m_shouldStop.load(std::memory_order_acquire))
        {
            bool connected = false;
            while (!m_shouldStop.load(std::memory_order_acquire))
            {
                if (::ConnectNamedPipe(listening, nullptr))
                {
                    connected = true;
                    break;
                }
                const DWORD error = ::GetLastError();
                if (error == ERROR_PIPE_CONNECTED)
                {
                    connected = true;
                    break;
                }
                if (error == ERROR_NO_DATA)
                {
                    // A client connected and already closed; reset the instance
                    // so it can listen again.
                    ::DisconnectNamedPipe(listening);
                }
                else if (error != ERROR_PIPE_LISTENING)
                {
                    fatalError = "DaemonServer: ConnectNamedPipe failed (error " + std::to_string(error) + ")";
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                ReapFinishedWorkers();
            }

            // Stop or a fatal error; teardown closes the listener via m_listenFd.
            if (!connected)
                break;
            if (!IsSameUserPeer(listening))
            {
                ::DisconnectNamedPipe(listening);
                continue;
            }

            ReapFinishedWorkers();
            std::lock_guard lock(m_threadsMutex);
            if (m_clientWorkers.size() >= m_maximumClientWorkers)
            {
                ::DisconnectNamedPipe(listening);
                continue;
            }

            HANDLE next = CreateListeningInstance(pipeName, /*firstInstance*/ false, endpointSecurity.Attributes());
            if (next == INVALID_HANDLE_VALUE)
            {
                fatalError = "DaemonServer: CreateNamedPipeW failed (error " + std::to_string(::GetLastError()) + ")";
                break;
            }
            HANDLE accepted = listening;
            listening = next;
            m_listenFd = FromNative(listening);

            auto& worker = m_clientWorkers.emplace_back();
            std::atomic<bool>& doneFlag = worker.done;
            worker.thread =
                std::thread([this, accepted, &doneFlag] { HandleConnection(FromNative(accepted), doneFlag); });
        }

        std::list<ClientWorker> toJoin;
        {
            std::lock_guard lock(m_threadsMutex);
            toJoin = std::move(m_clientWorkers);
        }
        for (auto& worker : toJoin)
        {
            if (worker.thread.joinable())
                worker.thread.join();
        }
        auto listeningPipe = ToNative(m_listenFd);
        if (listeningPipe != kInvalidSocket)
        {
            ::DisconnectNamedPipe(listeningPipe);
            CloseSocket(listeningPipe);
            m_listenFd = FromNative(kInvalidSocket);
        }
        m_boundPath.clear();
        (void)::ReleaseMutex(endpointMutex);
        ::CloseHandle(endpointMutex);
        // Do not re-arm until every listener and client worker is gone. This
        // preserves a Stop() received while Run() is still establishing its
        // endpoint, while allowing an explicitly reused server to run later.
        m_shouldStop.store(false, std::memory_order_release);
        if (!fatalError.empty())
            return Unexpected<std::string>(std::move(fatalError));
        return {};
#else
        if (socketPath.empty())
            return Unexpected<std::string>("DaemonServer: socket path is empty");
        if (socketPath.size() >= sizeof(sockaddr_un{}.sun_path))
            return Unexpected<std::string>("DaemonServer: socket path exceeds sockaddr_un capacity");

        struct stat existing
        {
        };
        if (::lstat(socketPath.c_str(), &existing) == 0)
        {
            if (!S_ISSOCK(existing.st_mode) || existing.st_uid != ::geteuid())
                return Unexpected<std::string>("DaemonServer: endpoint exists but is not an owner-controlled socket");
            if (EndpointIsActive(socketPath))
                return Unexpected<std::string>("DaemonServer: endpoint is already active");
            if (::unlink(socketPath.c_str()) != 0)
                return Unexpected<std::string>("DaemonServer: could not remove stale endpoint");
        }
        else if (errno != ENOENT)
        {
            return Unexpected<std::string>("DaemonServer: could not inspect endpoint path");
        }

        int listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listenFd < 0)
            return Unexpected<std::string>(std::string("DaemonServer: socket() failed: ") + std::strerror(errno));

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, socketPath.data(), socketPath.size());

        if (::bind(listenFd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            std::string err = std::strerror(errno);
            ::close(listenFd);
            return Unexpected<std::string>("DaemonServer: bind(" + socketPath + ") failed: " + err);
        }

        struct stat boundEndpoint
        {
        };
        if (::lstat(socketPath.c_str(), &boundEndpoint) != 0 || !S_ISSOCK(boundEndpoint.st_mode) ||
            boundEndpoint.st_uid != ::geteuid())
        {
            ::close(listenFd);
            return Unexpected<std::string>("DaemonServer: could not establish endpoint ownership");
        }

        // Owner-only permissions.
        if (::chmod(socketPath.c_str(), S_IRUSR | S_IWUSR) != 0)
        {
            ::close(listenFd);
            UnlinkOwnedEndpoint(socketPath, boundEndpoint.st_dev, boundEndpoint.st_ino);
            return Unexpected<std::string>("DaemonServer: could not restrict endpoint permissions");
        }

        if (::listen(listenFd, 8) < 0)
        {
            std::string err = std::strerror(errno);
            ::close(listenFd);
            UnlinkOwnedEndpoint(socketPath, boundEndpoint.st_dev, boundEndpoint.st_ino);
            return Unexpected<std::string>("DaemonServer: listen() failed: " + err);
        }

        // Non-blocking listen socket + poll() so Stop() can exit the loop within
        // the poll timeout without the undefined-behavior of closing an fd
        // another thread is blocked on.
        int flags = ::fcntl(listenFd, F_GETFL, 0);
        if (flags >= 0)
            ::fcntl(listenFd, F_SETFL, flags | O_NONBLOCK);

        m_listenFd = FromNative(static_cast<NativeSocket>(listenFd));
        m_boundPath = socketPath;
        m_runStartedAt = std::chrono::steady_clock::now();

        std::string fatalError;

        while (!m_shouldStop.load(std::memory_order_acquire))
        {
            pollfd pfd{};
            pfd.fd = listenFd;
            pfd.events = POLLIN;
            int pn = ::poll(&pfd, 1, 500);
            if (pn < 0)
            {
                if (errno == EINTR)
                    continue;
                // Non-recoverable poll() error. Record, exit the loop, surface
                // as unexpected() to the caller so upstream recovery logic can
                // distinguish from a clean shutdown.
                fatalError = std::string("DaemonServer: poll() failed: ") + std::strerror(errno);
                break;
            }
            if (pn == 0)
            {
                // Timeout tick — good opportunity to join any finished workers.
                ReapFinishedWorkers();
                continue; // recheck m_shouldStop
            }

            sockaddr_un peer{};
            socklen_t peerLen = sizeof(peer);
            int connFd = ::accept(listenFd, reinterpret_cast<sockaddr*>(&peer), &peerLen);
            if (connFd < 0)
            {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                fatalError = std::string("DaemonServer: accept() failed: ") + std::strerror(errno);
                break;
            }
#if defined(SO_NOSIGPIPE) && !defined(MSG_NOSIGNAL)
            const int noSigPipe = 1;
            if (::setsockopt(connFd, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe)) != 0)
            {
                ::close(connFd);
                continue;
            }
#endif

            // Do not rely on the filesystem mode alone: authenticate the
            // connected local principal before dispatching any control-plane RPC.
            if (!IsSameUserPeer(connFd))
            {
                ::close(connFd);
                continue;
            }

            timeval tv{0, 500'000};
            ::setsockopt(connFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            // Reap finished workers before pushing a new one so a slow spawn
            // rate of reconnecting clients doesn't let the list grow unboundedly
            // between poll timeouts.
            ReapFinishedWorkers();

            std::lock_guard lock(m_threadsMutex);
            if (m_clientWorkers.size() >= m_maximumClientWorkers)
            {
                ::close(connFd);
                continue;
            }
            auto& worker = m_clientWorkers.emplace_back();
            std::atomic<bool>& doneFlag = worker.done;
            worker.thread = std::thread([this, connFd, &doneFlag]
                                        { HandleConnection(static_cast<std::intptr_t>(connFd), doneFlag); });
        }

        // Drain any clients still running when we stopped accepting. These are
        // joined unconditionally regardless of `done` state.
        std::list<ClientWorker> toJoin;
        {
            std::lock_guard lock(m_threadsMutex);
            toJoin = std::move(m_clientWorkers);
        }
        for (auto& worker : toJoin)
        {
            if (worker.thread.joinable())
                worker.thread.join();
        }

        auto s = ToNative(m_listenFd);
        if (s != kInvalidSocket)
        {
            CloseSocket(s);
            m_listenFd = FromNative(kInvalidSocket);
        }
        UnlinkOwnedEndpoint(m_boundPath, boundEndpoint.st_dev, boundEndpoint.st_ino);
        m_boundPath.clear();

        // As on Windows, startup-time stop requests stay sticky until endpoint
        // and worker teardown completes; only a completed Run() re-arms them.
        m_shouldStop.store(false, std::memory_order_release);
        if (!fatalError.empty())
            return Unexpected<std::string>(std::move(fatalError));
        return {};
#endif
    }

    void DaemonServer::ReapFinishedWorkers()
    {
        std::lock_guard lock(m_threadsMutex);
        for (auto it = m_clientWorkers.begin(); it != m_clientWorkers.end();)
        {
            if (it->done.load(std::memory_order_acquire))
            {
                if (it->thread.joinable())
                    it->thread.join();
                it = m_clientWorkers.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    DaemonStats DaemonServer::SnapshotStats() const
    {
        DaemonStats stats;
        stats.protocolVersion = kProtocolVersion;
        if (m_runStartedAt.time_since_epoch().count() != 0)
        {
            auto elapsed = std::chrono::steady_clock::now() - m_runStartedAt;
            stats.uptimeSeconds =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
        }
        stats.registeredIds.reserve(m_services.size());
        for (const auto& [id, service] : m_services)
            stats.registeredIds.push_back(id);
        std::sort(stats.registeredIds.begin(), stats.registeredIds.end());
        return stats;
    }

    void DaemonServer::HandleConnection(std::intptr_t connFdHandle, std::atomic<bool>& doneFlag)
    {
        auto s = ToNative(connFdHandle);
        while (!m_shouldStop.load(std::memory_order_acquire))
        {
            FrameHeader header;
            std::vector<uint8_t> payload;
            if (!RecvFrame(s, header, payload, m_shouldStop))
                break;

            std::optional<ServiceResponse> response;
            auto it = m_services.find(header.serviceId);
            if (it == m_services.end())
            {
                std::string msg = "unknown service id";
                ServiceResponse r;
                r.messageType = static_cast<uint16_t>(ControlMessage::ErrorResponse);
                r.payload.assign(msg.begin(), msg.end());
                // Error responses are always returned under the Control service ID
                // so clients can detect them uniformly.
                if (!SendFrame(s, ServiceId::Control, r.messageType, r.payload, m_shouldStop))
                    break;
                continue;
            }

            response = it->second->HandleMessage(header.messageType, payload);
            if (!response)
                continue; // one-way message, no reply

            // ControlService flips the shared stop flag while constructing its
            // ShutdownAck. Attempt that bounded response before closing.
            const bool isShutdownAck = it->second->GetServiceId() == ServiceId::Control &&
                                       response->messageType == static_cast<uint16_t>(ControlMessage::ShutdownAck);
            std::atomic<bool> responseCancellation{false};
            const std::atomic<bool>& cancellation = isShutdownAck ? responseCancellation : m_shouldStop;
            if (!SendFrame(s, it->second->GetServiceId(), response->messageType, response->payload, cancellation))
                break;
            if (isShutdownAck)
            {
#if defined(_WIN32)
                // DisconnectNamedPipe discards unread response bytes. Wait only
                // until the requesting client consumes the ack and closes; unlike
                // FlushFileBuffers, this health probe is deadline bounded.
                WaitForPeerClose(s);
#endif
                break;
            }
        }
#if defined(_WIN32)
        ::DisconnectNamedPipe(s);
#endif
        CloseSocket(s);
        // Signal the accept loop that this worker is ready to be reaped.
        // Release so the reap pass's acquire-load observes the thread fully
        // exiting any prior state touched by CloseSocket et al.
        doneFlag.store(true, std::memory_order_release);
    }

} // namespace Spark::Daemon
