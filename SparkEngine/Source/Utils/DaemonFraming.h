/**
 * @file DaemonFraming.h
 * @brief Shared low-level framing helpers for SparkDaemon IPC.
 *
 * Header-only on purpose: the same code is reachable from both `SparkEngineLib`
 * (for `DaemonClient`) and the standalone `SparkDaemon` executable (for
 * `DaemonServer`), neither of which links the other. Keep this file small and
 * free of engine dependencies.
 *
 * On POSIX (Linux, macOS) a `NativeSocket` is a Unix domain socket file
 * descriptor. On Windows a `NativeSocket` is a `HANDLE` to a named pipe, but
 * Phase 1 of the daemon feature targets POSIX only — the Windows side of this
 * header defines the types so `DaemonClient` compiles, but the send/recv
 * helpers always report failure. Named-pipe support can be added in a
 * follow-up phase.
 */

#pragma once

#include "DaemonProtocol.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Spark::Daemon
{

#if defined(_WIN32)
    using NativeSocket = HANDLE;
    inline constexpr NativeSocket kInvalidSocket = nullptr;
#else
    using NativeSocket = int;
    inline constexpr NativeSocket kInvalidSocket = -1;
#endif

    /// Close the native socket/handle. Safe to call on `kInvalidSocket`.
    inline void CloseSocket(NativeSocket& s) noexcept
    {
        if (s == kInvalidSocket)
            return;
#if defined(_WIN32)
        ::CloseHandle(s);
#else
        ::close(s);
#endif
        s = kInvalidSocket;
    }

    /**
     * @brief Write exactly @p totalLen bytes to the socket, handling partial sends.
     *
     * @p shuttingDown aborts a *waiting* loop — it is only consulted on `EINTR`
     * or `EAGAIN`/`EWOULDBLOCK`. Bytes already accepted by the kernel are never
     * abandoned, which matters for server-side code that flips its own
     * shutdown flag from a request handler and still needs to write the
     * response before tearing down.
     */
    inline bool SendAll(NativeSocket s, const void* buf, size_t totalLen,
                        const std::atomic<bool>& shuttingDown) noexcept
    {
        if (s == kInvalidSocket)
            return false;
        const auto* p = static_cast<const uint8_t*>(buf);
        size_t sent = 0;
        while (sent < totalLen)
        {
#if defined(_WIN32)
            (void)p;
            (void)shuttingDown;
            return false; // Not yet implemented on Windows.
#else
            auto n = ::send(s, p + sent, totalLen - sent, 0);
            if (n > 0)
            {
                sent += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                if (shuttingDown.load(std::memory_order_acquire))
                    return false;
                continue;
            }
            return false;
#endif
        }
        return true;
    }

    /**
     * @brief Read exactly @p totalLen bytes from the socket.
     *
     * Same aborts-waiting-only semantics as `SendAll`. Assumes the socket has a
     * recv timeout (`SO_RCVTIMEO`) so `EAGAIN`/`EWOULDBLOCK` cycles let us
     * observe @p shuttingDown periodically.
     */
    inline bool RecvAll(NativeSocket s, void* buf, size_t totalLen, const std::atomic<bool>& shuttingDown) noexcept
    {
        if (s == kInvalidSocket)
            return false;
        auto* p = static_cast<uint8_t*>(buf);
        size_t received = 0;
        while (received < totalLen)
        {
#if defined(_WIN32)
            (void)p;
            (void)shuttingDown;
            return false; // Not yet implemented on Windows.
#else
            auto n = ::recv(s, p + received, totalLen - received, 0);
            if (n > 0)
            {
                received += static_cast<size_t>(n);
                continue;
            }
            if (n == 0)
                return false; // Peer closed cleanly.
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (shuttingDown.load(std::memory_order_acquire))
                    return false;
                continue;
            }
            return false;
#endif
        }
        return true;
    }

    /**
     * @brief Send a framed message: 8-byte header followed by @p payload.
     */
    inline bool SendFrame(NativeSocket s, ServiceId service, uint16_t messageType, const std::vector<uint8_t>& payload,
                          const std::atomic<bool>& shuttingDown)
    {
        if (payload.size() > kMaxPayloadSize)
            return false;

        FrameHeader header;
        header.payloadSize = static_cast<uint32_t>(payload.size());
        header.serviceId = static_cast<uint16_t>(service);
        header.messageType = messageType;

        uint8_t headerBytes[kFrameHeaderSize];
        EncodeFrameHeader(header, headerBytes);

        if (!SendAll(s, headerBytes, kFrameHeaderSize, shuttingDown))
            return false;
        if (payload.empty())
            return true;
        return SendAll(s, payload.data(), payload.size(), shuttingDown);
    }

    /**
     * @brief Receive one framed message. On success fills @p header and @p payload.
     *
     * Returns false on malformed header (payload > kMaxPayloadSize), hard I/O error,
     * peer close, or @p shuttingDown.
     */
    inline bool RecvFrame(NativeSocket s, FrameHeader& header, std::vector<uint8_t>& payload,
                          const std::atomic<bool>& shuttingDown)
    {
        uint8_t headerBytes[kFrameHeaderSize];
        if (!RecvAll(s, headerBytes, kFrameHeaderSize, shuttingDown))
            return false;

        header = DecodeFrameHeader(headerBytes);
        if (header.payloadSize > kMaxPayloadSize)
            return false;

        payload.resize(header.payloadSize);
        if (header.payloadSize == 0)
            return true;
        return RecvAll(s, payload.data(), header.payloadSize, shuttingDown);
    }

} // namespace Spark::Daemon
