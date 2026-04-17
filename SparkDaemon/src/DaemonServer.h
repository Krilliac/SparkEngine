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

#include "../../SparkEngine/Source/Utils/Expected.h"
#include "ServiceBase.h"

#include <atomic>
#include <chrono>
#include <list>
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
         * Returns on:
         *   - clean shutdown (success);
         *   - fatal `poll()` / `accept()` error (returns the error string).
         *
         * @param socketPath  AF_UNIX socket path. File is unlinked first if it
         *                    exists, recreated with permissions 0600 so only
         *                    the owning user can connect.
         * @return            Empty success, or an error string.
         */
        Expected<void, std::string> Run(const std::string& socketPath);

        /// Signal the accept loop to exit. Safe to call from another thread.
        void Stop();

      private:
        /// Per-accept worker state. `done` flips when `HandleConnection` returns,
        /// so the accept loop can reap finished threads without blocking on a
        /// still-live connection — important for long-running daemons where
        /// clients reconnect repeatedly.
        struct ClientWorker
        {
            std::thread thread;
            std::atomic<bool> done{false};

            ClientWorker() = default;
            ClientWorker(const ClientWorker&) = delete;
            ClientWorker& operator=(const ClientWorker&) = delete;
            // `std::list` only requires move-construction for `emplace_back` / `splice`
            // in edge cases; we build ClientWorkers in place via `emplace_back` and
            // never copy them, so the atomic<bool> doesn't need a move constructor.
        };

        void HandleConnection(std::intptr_t connFd, std::atomic<bool>& doneFlag);
        void ReapFinishedWorkers();

        std::unordered_map<uint16_t, std::unique_ptr<ServiceBase>> m_services;
        std::mutex m_threadsMutex;
        std::list<ClientWorker> m_clientWorkers;
        std::atomic<bool> m_shouldStop{false};
        std::intptr_t m_listenFd = -1;
        std::string m_boundPath;
        std::chrono::steady_clock::time_point m_runStartedAt;
    };

} // namespace Spark::Daemon
