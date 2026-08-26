/**
 * @file DaemonClient.h
 * @brief Engine-side client for the SparkDaemon IPC foundation (Phase 1).
 *
 * The daemon is an out-of-process service that owns shared mutable state
 * (asset cache, shader cache, collab session, build watcher) across multiple
 * engine / editor / tool instances. `DaemonClient` is the thin, engine-side
 * handle: connect by socket path, send framed requests, read framed
 * responses, disconnect.
 *
 * Phase 1 scope — connection + request/response only. Higher-level service
 * wrappers (AssetClient, ShaderClient, ...) will build on top of this.
 *
 * ## Platforms
 * - **Linux, macOS:** full support via `AF_UNIX` sockets.
 * - **Windows:** byte-mode named pipes scoped to the local machine.
 *
 * ## Thread safety
 * A single `DaemonClient` may be used from multiple threads — every public
 * method that touches the socket takes a mutex internally. `Request()` holds
 * the mutex for the full send+receive cycle so responses are not interleaved.
 */

#pragma once

#include "DaemonProtocol.h"

#include <atomic>
#include <cstdint>
#include "Expected.h"
#include <mutex>
#include <string>
#include <vector>

namespace Spark::Daemon
{

    /// Response frame returned by `DaemonClient::Request`.
    struct Response
    {
        uint16_t messageType = 0; ///< Service-specific message type.
        std::vector<uint8_t> payload;
    };

    class DaemonClient
    {
      public:
        DaemonClient() = default;
        ~DaemonClient();

        DaemonClient(const DaemonClient&) = delete;
        DaemonClient& operator=(const DaemonClient&) = delete;
        DaemonClient(DaemonClient&&) = delete;
        DaemonClient& operator=(DaemonClient&&) = delete;

        /**
         * @brief Connect to a running daemon at @p socketPath.
         *
         * On POSIX this is the path to an `AF_UNIX` socket (typically
         * `<build>/.spark-daemon.sock`). The socket must already exist —
         * `DaemonClient` does not launch the daemon itself. Callers that
         * need auto-launch should invoke the daemon via `Process::Builder`
         * and then call `Connect()` once the socket file appears.
         *
         * If the client is already connected, the old connection is closed first.
         *
         * @return Empty success, or an error string.
         */
        Expected<void, std::string> Connect(const std::string& socketPath);

        /// Close the connection if open. Safe to call repeatedly.
        void Disconnect();

        /// True if a live connection is currently held.
        [[nodiscard]] bool IsConnected() const;

        /**
         * @brief Send a request frame and block until the matching response arrives.
         *
         * Serial: holds an internal mutex for the full send+receive cycle. If
         * another thread is mid-request, this call blocks behind it.
         *
         * @param service       Target service ID.
         * @param messageType   Service-specific request message type.
         * @param payload       Serialised request payload (may be empty).
         * @return              Response frame, or an error string on transport
         *                      failure / disconnect.
         */
        Expected<Response, std::string> Request(ServiceId service, uint16_t messageType,
                                                const std::vector<uint8_t>& payload);

        /**
         * @brief Send a framed message without waiting for a response.
         *
         * Useful for fire-and-forget notifications or as the first half of a
         * custom multi-frame exchange. Most callers should prefer `Request()`.
         */
        Expected<void, std::string> SendOneWay(ServiceId service, uint16_t messageType,
                                               const std::vector<uint8_t>& payload);

        /// Convenience: Control::Ping → expects Control::PingResponse("pong").
        Expected<void, std::string> Ping();

      private:
        mutable std::mutex m_mutex;
        std::atomic<bool> m_shuttingDown{false};
        // NativeSocket is platform-specific (int on POSIX, HANDLE on Windows) —
        // we store it as intptr_t to keep this header free of <windows.h> and
        // <sys/socket.h>. Implementation in DaemonClient.cpp does the casts.
        std::intptr_t m_nativeSocket = -1;
    };

} // namespace Spark::Daemon
