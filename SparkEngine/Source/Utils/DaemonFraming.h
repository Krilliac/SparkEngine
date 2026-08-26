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
 * descriptor. On Windows it is a byte-mode named-pipe `HANDLE`.
 */

#pragma once

#include "DaemonProtocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Spark::Daemon
{

    inline constexpr auto kDaemonIoTimeout = std::chrono::seconds(5);

#if defined(_WIN32)
    using NativeSocket = HANDLE;
    inline const NativeSocket kInvalidSocket = INVALID_HANDLE_VALUE;

    inline std::wstring NormalizePipeName(const std::string& endpoint)
    {
        std::string name = endpoint;
        constexpr const char* prefix = "\\\\.\\pipe\\";
        if (!name.starts_with(prefix))
        {
            const size_t separator = name.find_last_of("/\\");
            name = separator == std::string::npos ? name : name.substr(separator + 1);
            if (name.empty())
                name = "spark-daemon";
            for (char& character : name)
            {
                if (character == '/' || character == '\\' || character == ':' || character == '.')
                    character = '-';
            }
            name = std::string(prefix) + name;
        }

        const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
                                                 static_cast<int>(name.size()), nullptr, 0);
        if (length <= 0)
            return {};
        std::wstring wide(static_cast<size_t>(length), L'\0');
        if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()),
                                  wide.data(), length) != length)
            return {};
        return wide;
    }
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
        const auto deadline = std::chrono::steady_clock::now() + kDaemonIoTimeout;
        while (sent < totalLen && std::chrono::steady_clock::now() < deadline)
        {
#if defined(_WIN32)
            const DWORD chunk = static_cast<DWORD>(
                (std::min)(totalLen - sent, static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD written = 0;
            const BOOL succeeded = ::WriteFile(s, p + sent, chunk, &written, nullptr);
            if (succeeded && written > 0)
            {
                sent += static_cast<size_t>(written);
                continue;
            }
            if (succeeded)
            {
                if (shuttingDown.load(std::memory_order_acquire))
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            const DWORD error = ::GetLastError();
            if (error == ERROR_NO_DATA || error == ERROR_PIPE_BUSY || error == ERROR_PIPE_LISTENING)
            {
                if (shuttingDown.load(std::memory_order_acquire))
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            return false;
#else
            pollfd descriptor{s, POLLOUT, 0};
            const int polled = ::poll(&descriptor, 1, 50);
            if (polled < 0)
            {
                if (errno == EINTR)
                    continue;
                return false;
            }
            if (polled == 0)
            {
                if (shuttingDown.load(std::memory_order_acquire))
                    return false;
                continue;
            }
            int flags = 0;
#ifdef MSG_DONTWAIT
            flags |= MSG_DONTWAIT;
#endif
#ifdef MSG_NOSIGNAL
            flags |= MSG_NOSIGNAL;
#endif
            auto n = ::send(s, p + sent, totalLen - sent, flags);
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
        return sent == totalLen;
    }

    /**
     * @brief Read exactly @p totalLen bytes from the socket.
     *
     * Same aborts-waiting-only semantics as `SendAll`. Polling and an absolute
     * deadline bound partial or stalled peers on every platform.
     */
    inline bool RecvAll(NativeSocket s, void* buf, size_t totalLen, const std::atomic<bool>& shuttingDown) noexcept
    {
        if (s == kInvalidSocket)
            return false;
        auto* p = static_cast<uint8_t*>(buf);
        size_t received = 0;
        const auto deadline = std::chrono::steady_clock::now() + kDaemonIoTimeout;
        while (received < totalLen && std::chrono::steady_clock::now() < deadline)
        {
#if defined(_WIN32)
            const DWORD chunk = static_cast<DWORD>(
                (std::min)(totalLen - received, static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD read = 0;
            const BOOL succeeded = ::ReadFile(s, p + received, chunk, &read, nullptr);
            if (succeeded && read > 0)
            {
                received += static_cast<size_t>(read);
                continue;
            }
            if (succeeded)
            {
                if (shuttingDown.load(std::memory_order_acquire))
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            const DWORD error = ::GetLastError();
            if (error == ERROR_NO_DATA || error == ERROR_PIPE_BUSY || error == ERROR_PIPE_LISTENING)
            {
                if (shuttingDown.load(std::memory_order_acquire))
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            return false;
#else
            pollfd descriptor{s, POLLIN, 0};
            const int polled = ::poll(&descriptor, 1, 50);
            if (polled < 0)
            {
                if (errno == EINTR)
                    continue;
                return false;
            }
            if (polled == 0)
            {
                if (shuttingDown.load(std::memory_order_acquire))
                    return false;
                continue;
            }
            int flags = 0;
#ifdef MSG_DONTWAIT
            flags |= MSG_DONTWAIT;
#endif
            auto n = ::recv(s, p + received, totalLen - received, flags);
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
        return received == totalLen;
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
