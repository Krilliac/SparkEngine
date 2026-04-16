/**
 * @file DaemonConnection.h
 * @brief Process-wide accessor for the engine's DaemonClient.
 *
 * Most engine subsystems that want to talk to the daemon (ShaderDiskCache,
 * AssetCache, ...) share a single `DaemonClient` — the socket is mux-capable
 * and `DaemonClient::Request` serialises through an internal mutex, so one
 * connection per process is enough.
 *
 * This helper owns that singleton. Typical lifecycle:
 *
 * @code
 *   // During engine startup
 *   auto& conn = Spark::Daemon::DaemonConnection::Instance();
 *   conn.TryConnect("./.spark-daemon.sock");
 *
 *   // Anywhere downstream (a subsystem, a tool, a test)
 *   if (auto* client = conn.GetClient())
 *       // use client for daemon RPCs
 *
 *   // During engine shutdown
 *   conn.Shutdown();
 * @endcode
 *
 * `TryConnect` is intentionally lenient: if the socket file doesn't exist,
 * or `connect()` fails, or the daemon isn't running, we just report false
 * and stay disconnected. Every daemon feature is opt-in and every engine
 * subsystem must keep a fallback code path that works without a daemon.
 */

#pragma once

#include "DaemonClient.h"

#include <memory>
#include <mutex>
#include <string>

namespace Spark::Daemon
{

    class DaemonConnection
    {
      public:
        static DaemonConnection& Instance();

        /**
         * @brief Try to connect to a running daemon.
         *
         * Safe to call when no daemon is running — just returns false and
         * leaves the connection state unchanged. Safe to call when already
         * connected — no-op, returns true.
         *
         * @param socketPath Path to the daemon's AF_UNIX socket (or empty
         *                   to use the default `./.spark-daemon.sock`).
         * @return true if a live connection is now held, false otherwise.
         */
        bool TryConnect(const std::string& socketPath = {});

        /// Close the connection if open. Idempotent.
        void Shutdown();

        /// True if a live connection is currently held.
        [[nodiscard]] bool IsConnected() const;

        /**
         * @brief Access the shared client, or nullptr if not connected.
         *
         * The returned pointer is valid until `Shutdown()` is called.
         * Callers should re-query on each use rather than caching.
         */
        [[nodiscard]] DaemonClient* GetClient();

        /// The socket path used for the current connection (empty if none).
        [[nodiscard]] std::string GetSocketPath() const;

      private:
        DaemonConnection() = default;
        ~DaemonConnection() = default;
        DaemonConnection(const DaemonConnection&) = delete;
        DaemonConnection& operator=(const DaemonConnection&) = delete;

        mutable std::mutex m_mutex;
        std::unique_ptr<DaemonClient> m_client;
        std::string m_socketPath;
    };

} // namespace Spark::Daemon
