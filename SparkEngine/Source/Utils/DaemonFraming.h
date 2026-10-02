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

    /// RecvFrame never grows a payload buffer more than this far past the bytes
    /// the peer has actually delivered (or twice the delivered bytes, once that is
    /// larger), so a header that merely claims kMaxPayloadSize costs no memory.
    inline constexpr size_t kRecvFrameGrowthStep = size_t{64} * 1024u;

#if defined(_WIN32)
    using NativeSocket = HANDLE;
    inline const NativeSocket kInvalidSocket = INVALID_HANDLE_VALUE;

    /**
     * @brief Fixed-size storage for a token's `TOKEN_USER`, big enough for any SID.
     *
     * Allocation-free so the peer checks below stay `noexcept`. GetTokenInformation
     * writes the TOKEN_USER header followed by the SID it points at.
     */
    struct TokenUserStorage
    {
        TOKEN_USER header{};
        BYTE sidSpace[SECURITY_MAX_SID_SIZE] = {};

        [[nodiscard]] PSID Sid() noexcept { return header.User.Sid; }
    };

    /// Read the user SID of @p process's primary token. Fails closed.
    inline bool QueryProcessUser(HANDLE process, TokenUserStorage& out) noexcept
    {
        HANDLE token = nullptr;
        if (!::OpenProcessToken(process, TOKEN_QUERY, &token))
            return false;
        DWORD returned = 0;
        const BOOL queried = ::GetTokenInformation(token, TokenUser, &out, sizeof(out), &returned);
        ::CloseHandle(token);
        return queried != FALSE && out.Sid() != nullptr && ::IsValidSid(out.Sid()) != FALSE;
    }

    /**
     * @brief True only when process @p processId runs as the same user SID as this process.
     *
     * Any failure (process gone, access denied, token unreadable) returns false,
     * so callers that gate trust on it fail closed. A same-user peer running at a
     * higher integrity level may refuse the token query; that is also rejected.
     */
    inline bool ProcessRunsAsCurrentUser(DWORD processId) noexcept
    {
        if (processId == 0)
            return false;
        HANDLE peer = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (!peer)
            return false;
        TokenUserStorage peerUser;
        TokenUserStorage selfUser;
        const bool queried = QueryProcessUser(peer, peerUser) && QueryProcessUser(::GetCurrentProcess(), selfUser);
        ::CloseHandle(peer);
        return queried && ::EqualSid(peerUser.Sid(), selfUser.Sid()) != FALSE;
    }

    /**
     * @brief Authenticate the server end of a connected client pipe handle.
     *
     * Named pipes are machine-global, so any local account can create a pipe
     * with the daemon's name before the daemon does. The kernel records the
     * process that created the instance this handle connected to; the client
     * refuses to talk to it unless that process runs as the current user.
     */
    inline bool IsPipeServerCurrentUser(HANDLE clientPipe) noexcept
    {
        ULONG serverProcessId = 0;
        return ::GetNamedPipeServerProcessId(clientPipe, &serverProcessId) != FALSE &&
               ProcessRunsAsCurrentUser(serverProcessId);
    }

    /**
     * @brief Map a daemon endpoint string to a Windows named-pipe name.
     *
     * - `\\.\pipe\<name>` is used verbatim.
     * - A bare name (no `/` or `\`) maps to `\\.\pipe\<name>` with `.` and `:`
     *   replaced by `-`. Gateway and test endpoints rely on this literal mapping.
     * - A filesystem-style path (the default `./.spark-daemon.sock`) keeps the
     *   POSIX meaning of "one endpoint per resolved path": the name is the
     *   sanitised basename plus an FNV-1a hash of the absolute, ASCII-case-folded
     *   path and the current user's SID. Two build trees, or two users, therefore
     *   never collapse onto one pipe. The name is not a secret: squatting is
     *   stopped by FILE_FLAG_FIRST_PIPE_INSTANCE on the server and by
     *   IsPipeServerCurrentUser() on the client.
     *
     * @return The wide pipe name, or an empty string when the endpoint is not
     *         valid UTF-8, is oversized, or cannot be resolved.
     */
    inline std::wstring NormalizePipeName(const std::string& endpoint)
    {
        // Far above any real path; keeps the int conversions below in range.
        constexpr size_t kMaximumEndpointBytes = 32u * 1024u;
        if (endpoint.size() > kMaximumEndpointBytes)
            return {};

        std::wstring wide;
        if (!endpoint.empty())
        {
            const int length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, endpoint.data(),
                                                     static_cast<int>(endpoint.size()), nullptr, 0);
            if (length <= 0)
                return {};
            wide.assign(static_cast<size_t>(length), L'\0');
            if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, endpoint.data(), static_cast<int>(endpoint.size()),
                                      wide.data(), length) != length)
                return {};
        }

        const std::wstring prefix = L"\\\\.\\pipe\\";
        if (wide.starts_with(prefix))
            return wide;

        const auto sanitize = [](std::wstring name)
        {
            if (name.empty())
                name = L"spark-daemon";
            for (wchar_t& character : name)
            {
                if (character == L':' || character == L'.')
                    character = L'-';
            }
            return name;
        };

        const size_t separator = wide.find_last_of(L"/\\");
        if (separator == std::wstring::npos)
            return prefix + sanitize(std::move(wide));

        // Keep a readable, bounded basename; the hash carries the identity.
        constexpr size_t kMaximumBasenameChars = 64;
        const std::wstring basename = sanitize(wide.substr(separator + 1, kMaximumBasenameChars));

        const DWORD required = ::GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
        if (required == 0)
            return {};
        std::wstring absolutePath(static_cast<size_t>(required), L'\0');
        const DWORD written = ::GetFullPathNameW(wide.c_str(), required, absolutePath.data(), nullptr);
        if (written == 0 || written >= required)
            return {};
        absolutePath.resize(written);
        for (wchar_t& character : absolutePath)
        {
            if (character >= L'A' && character <= L'Z')
                character = static_cast<wchar_t>(character - L'A' + L'a');
        }

        TokenUserStorage user;
        if (!QueryProcessUser(::GetCurrentProcess(), user))
            return {};

        uint64_t hash = UINT64_C(0xcbf29ce484222325);
        const auto mix = [&hash](const void* data, size_t size)
        {
            const auto* bytes = static_cast<const unsigned char*>(data);
            for (size_t index = 0; index < size; ++index)
            {
                hash ^= bytes[index];
                hash *= UINT64_C(0x100000001b3);
            }
        };
        mix(absolutePath.data(), absolutePath.size() * sizeof(wchar_t));
        const unsigned char pathSidSeparator = 0;
        mix(&pathSidSeparator, 1);
        mix(user.Sid(), ::GetLengthSid(user.Sid()));

        constexpr wchar_t kHexDigits[] = L"0123456789abcdef";
        std::wstring hashText(16, L'0');
        for (size_t index = 0; index < hashText.size(); ++index)
            hashText[hashText.size() - 1 - index] = kHexDigits[(hash >> (4 * index)) & 0xF];

        return prefix + basename + L"-" + hashText;
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
     * @brief Read exactly @p totalLen bytes from the socket before @p deadline.
     *
     * Same aborts-waiting-only semantics as `SendAll`. Polling and the absolute
     * deadline bound partial or stalled peers on every platform; RecvFrame passes
     * one deadline to the several reads that fill a payload.
     */
    inline bool RecvAllUntil(NativeSocket s, void* buf, size_t totalLen, const std::atomic<bool>& shuttingDown,
                             std::chrono::steady_clock::time_point deadline) noexcept
    {
        if (s == kInvalidSocket)
            return false;
        auto* p = static_cast<uint8_t*>(buf);
        size_t received = 0;
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

    /// Read exactly @p totalLen bytes within kDaemonIoTimeout (see RecvAllUntil).
    inline bool RecvAll(NativeSocket s, void* buf, size_t totalLen, const std::atomic<bool>& shuttingDown) noexcept
    {
        return RecvAllUntil(s, buf, totalLen, shuttingDown, std::chrono::steady_clock::now() + kDaemonIoTimeout);
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
     * peer close, a payload not delivered within kDaemonIoTimeout, or @p shuttingDown.
     *
     * The payload buffer grows with the bytes that actually arrive (at most
     * kRecvFrameGrowthStep, or the bytes already received, ahead of them) instead
     * of trusting the header's claimed size, so a peer that sends only a header
     * claiming 16 MiB cannot make the receiver allocate it. After a failed call the
     * contents of @p payload are unspecified.
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

        payload.clear();
        const size_t payloadSize = header.payloadSize;
        const auto deadline = std::chrono::steady_clock::now() + kDaemonIoTimeout;
        size_t received = 0;
        while (received < payloadSize)
        {
            // Geometric growth keeps the copying linear; the step keeps small frames cheap.
            const size_t step = (std::min)(payloadSize - received, (std::max)(received, kRecvFrameGrowthStep));
            payload.resize(received + step);
            if (!RecvAllUntil(s, payload.data() + received, step, shuttingDown, deadline))
                return false;
            received += step;
        }
        return true;
    }

} // namespace Spark::Daemon
