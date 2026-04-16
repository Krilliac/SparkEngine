/**
 * @file DaemonServer.cpp
 * @brief Implementation of the AF_UNIX daemon server.
 */

#include "DaemonServer.h"
#include "Utils/DaemonFraming.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
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
    } // namespace

    DaemonServer::DaemonServer() = default;

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
#if !defined(_WIN32)
        if (!m_boundPath.empty())
            ::unlink(m_boundPath.c_str());
#endif
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

    std::expected<void, std::string> DaemonServer::Run(const std::string& socketPath)
    {
#if defined(_WIN32)
        (void)socketPath;
        return std::unexpected<std::string>("DaemonServer: Windows named-pipe transport not yet implemented");
#else
        if (socketPath.empty())
            return std::unexpected<std::string>("DaemonServer: socket path is empty");
        if (socketPath.size() >= sizeof(sockaddr_un{}.sun_path))
            return std::unexpected<std::string>("DaemonServer: socket path exceeds sockaddr_un capacity");

        ::unlink(socketPath.c_str());

        int listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listenFd < 0)
            return std::unexpected<std::string>(std::string("DaemonServer: socket() failed: ") + std::strerror(errno));

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, socketPath.data(), socketPath.size());

        if (::bind(listenFd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            std::string err = std::strerror(errno);
            ::close(listenFd);
            return std::unexpected<std::string>("DaemonServer: bind(" + socketPath + ") failed: " + err);
        }

        // Owner-only permissions.
        ::chmod(socketPath.c_str(), S_IRUSR | S_IWUSR);

        if (::listen(listenFd, 8) < 0)
        {
            std::string err = std::strerror(errno);
            ::close(listenFd);
            ::unlink(socketPath.c_str());
            return std::unexpected<std::string>("DaemonServer: listen() failed: " + err);
        }

        // Non-blocking listen socket + poll() so Stop() can exit the loop within
        // the poll timeout without the undefined-behavior of closing an fd
        // another thread is blocked on.
        int flags = ::fcntl(listenFd, F_GETFL, 0);
        if (flags >= 0)
            ::fcntl(listenFd, F_SETFL, flags | O_NONBLOCK);

        m_listenFd = FromNative(static_cast<NativeSocket>(listenFd));
        m_boundPath = socketPath;
        m_shouldStop.store(false, std::memory_order_release);
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

            timeval tv{0, 500'000};
            ::setsockopt(connFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            // Reap finished workers before pushing a new one so a slow spawn
            // rate of reconnecting clients doesn't let the list grow unboundedly
            // between poll timeouts.
            ReapFinishedWorkers();

            std::lock_guard lock(m_threadsMutex);
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
        ::unlink(m_boundPath.c_str());
        m_boundPath.clear();

        if (!fatalError.empty())
            return std::unexpected<std::string>(std::move(fatalError));
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

            if (!SendFrame(s, it->second->GetServiceId(), response->messageType, response->payload, m_shouldStop))
                break;
        }
        CloseSocket(s);
        // Signal the accept loop that this worker is ready to be reaped.
        // Release so the reap pass's acquire-load observes the thread fully
        // exiting any prior state touched by CloseSocket et al.
        doneFlag.store(true, std::memory_order_release);
    }

} // namespace Spark::Daemon
