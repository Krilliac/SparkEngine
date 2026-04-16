/**
 * @file DaemonServer.h
 * @brief Accepts client connections and dispatches framed requests to services.
 *
 * Phase 1 foundation: a single accept loop on an `AF_UNIX` socket plus a
 * per-connection thread that reads one request frame, dispatches to the
 * registered service, and writes the response. Connections are one-shot at
 * this layer — a client calls `Request()` once then either issues the next
 * request on the same socket or disconnects.
 *
 * Services are registered with `AddService(std::unique_ptr<ServiceBase>)`
 * before `Run()`; the map is frozen while the server is running.
 */

#pragma once

#include "ServiceBase.h"

#include <atomic>
#include <chrono>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Spark::Daemon
{

    class DaemonServer
    {
      public:
        DaemonServer();
        ~DaemonServer();

        DaemonServer(const DaemonServer&) = delete;
        DaemonServer& operator=(const DaemonServer&) = delete;
        DaemonServer(DaemonServer&&) = delete;
        DaemonServer& operator=(DaemonServer&&) = delete;

        /// Register a service before `Run()`. Overwrites any existing service with
        /// the same `ServiceId`.
        void AddService(std::unique_ptr<ServiceBase> service);

        /// Flag that forces `Run()` to exit after its current `accept()` cycle.
        /// May be shared with a ControlService so `ShutdownRequest` can stop the
        /// server from the inside.
        [[nodiscard]] std::atomic<bool>& GetShouldStopFlag() noexcept { return m_shouldStop; }

        /// Produce a DaemonStats snapshot (uptime + registered service IDs).
        /// Used by ControlService to answer `StatsRequest`.
        [[nodiscard]] DaemonStats SnapshotStats() const;

        /**
         * @brief Bind, listen, accept, dispatch. Blocks until `Stop()` or a
         *        `ControlMessage::ShutdownRequest` arrives.
         *
         * @param socketPath  AF_UNIX socket path. File is unlinked first if it
         *                    exists, recreated with permissions 0600 so only
         *                    the owning user can connect.
         * @return            Empty success, or an error string.
         */
        std::expected<void, std::string> Run(const std::string& socketPath);

        /// Signal the accept loop to exit. Safe to call from another thread.
        void Stop();

      private:
        void HandleConnection(std::intptr_t connFd);

        std::unordered_map<uint16_t, std::unique_ptr<ServiceBase>> m_services;
        std::mutex m_threadsMutex;
        std::vector<std::thread> m_clientThreads;
        std::atomic<bool> m_shouldStop{false};
        std::intptr_t m_listenFd = -1;
        std::string m_boundPath;
        std::chrono::steady_clock::time_point m_runStartedAt;
    };

} // namespace Spark::Daemon
