/**
 * @file FuzzDaemonFrameProduction.cpp
 * @brief libc++-compiled production adapter for the daemon IPC frame libFuzzer harness.
 *
 * The input is the byte stream a local peer writes to a daemon connection. It
 * is written into one end of an AF_UNIX stream socket pair whose write side is
 * then shut down, and the shipped Spark::Daemon::RecvFrame (the receive path of
 * both DaemonServer and DaemonClient) reads frames from the other end until it
 * fails. An independent frame model walks the same bytes, and a violated
 * contract aborts so libFuzzer records a crash rather than a silent pass:
 *  - RecvFrame accepts a frame exactly when the model finds a whole header, a
 *    claimed size within kMaxPayloadSize and that many payload bytes, and the
 *    accepted header and payload equal the model's,
 *  - after every call the payload buffer's capacity stays within twice the
 *    payload bytes the peer delivered plus kRecvFrameGrowthStep, so a header
 *    that only claims 16 MiB cannot make the receiver allocate it,
 *  - every accepted payload also goes through DecodeDaemonStats, which must
 *    leave a sentinel untouched on rejection and, on acceptance, re-encode
 *    (EncodeDaemonStats) to exactly the payload prefix it consumed.
 * The peer's shutdown makes RecvFrame return as soon as the bytes run out, so
 * no input waits for the 5-second I/O deadline.
 */

#include "FuzzDaemonFrameProduction.h"

#include "Utils/DaemonFraming.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;
    constexpr std::uint64_t kSentinelUptime = 0xDEADBEEFDEADBEEFull;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzDaemonFrame: frame receiver violated: %s\n", what);
        std::abort();
    }

    [[noreturn]] void InfrastructureFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzDaemonFrame: harness infrastructure failed: %s\n", what);
        std::abort();
    }

    /// Owns both ends of the socket pair for one input.
    class SocketPair
    {
      public:
        SocketPair()
        {
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, m_descriptors) != 0)
                InfrastructureFailure("socketpair");
        }
        ~SocketPair()
        {
            ::close(m_descriptors[0]);
            ::close(m_descriptors[1]);
        }
        SocketPair(const SocketPair&) = delete;
        SocketPair& operator=(const SocketPair&) = delete;

        /// Buffer the whole input in the kernel and close the peer's write side.
        /// A non-blocking writer turns a buffer too small for the input into an
        /// infrastructure abort instead of a hang.
        void Deliver(const std::uint8_t* data, std::size_t size)
        {
            const int writer = m_descriptors[0];
            const int bufferBytes = 4 * static_cast<int>(kMaxInputBytes);
            ::setsockopt(writer, SOL_SOCKET, SO_SNDBUF, &bufferBytes, sizeof(bufferBytes));
            if (::fcntl(writer, F_SETFL, ::fcntl(writer, F_GETFL) | O_NONBLOCK) != 0)
                InfrastructureFailure("fcntl O_NONBLOCK");
            std::size_t written = 0;
            while (written < size)
            {
                const ssize_t sent = ::write(writer, data + written, size - written);
                if (sent <= 0)
                    InfrastructureFailure("the socket pair could not buffer the whole input");
                written += static_cast<std::size_t>(sent);
            }
            if (::shutdown(writer, SHUT_WR) != 0)
                InfrastructureFailure("shutdown");
        }

        [[nodiscard]] int Reader() const noexcept { return m_descriptors[1]; }

      private:
        int m_descriptors[2] = {-1, -1};
    };

    std::uint32_t ReadLE32(const std::uint8_t* bytes)
    {
        return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
               (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
    }

    std::uint16_t ReadLE16(const std::uint8_t* bytes)
    {
        return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
    }

    void CheckStatsPayload(const std::vector<std::uint8_t>& payload)
    {
        Spark::Daemon::DaemonStats stats;
        stats.uptimeSeconds = kSentinelUptime;
        stats.protocolVersion = "sentinel";
        stats.registeredIds = {0xBEEF};
        if (!Spark::Daemon::DecodeDaemonStats(payload, stats))
        {
            if (stats.uptimeSeconds != kSentinelUptime || stats.protocolVersion != "sentinel" ||
                stats.registeredIds != std::vector<std::uint16_t>{0xBEEF})
                InvariantFailure("a rejected DaemonStats payload modified the caller's output");
            return;
        }
        if (stats.protocolVersion.size() + 2 * stats.registeredIds.size() > payload.size())
            InvariantFailure("DaemonStats decoded more data than its payload holds");
        const std::vector<std::uint8_t> reencoded = Spark::Daemon::EncodeDaemonStats(stats);
        if (reencoded.size() > payload.size() || !std::equal(reencoded.begin(), reencoded.end(), payload.begin()))
            InvariantFailure("DaemonStats re-encoding differs from the consumed payload prefix");
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production receive path.
extern "C" int SparkFuzzRecvDaemonFrames(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    SocketPair sockets;
    sockets.Deliver(data, size);
    const std::atomic<bool> shuttingDown{false};

    std::size_t cursor = 0;
    for (;;)
    {
        Spark::Daemon::FrameHeader header;
        std::vector<std::uint8_t> payload;
        const bool accepted = Spark::Daemon::RecvFrame(sockets.Reader(), header, payload, shuttingDown);

        const std::size_t remaining = size - cursor;
        if (remaining < Spark::Daemon::kFrameHeaderSize)
        {
            if (accepted)
                InvariantFailure("accepted a frame from a truncated header");
            return 0;
        }
        const std::uint8_t* frame = data + cursor;
        const std::uint32_t claimed = ReadLE32(frame);
        const std::size_t available = remaining - Spark::Daemon::kFrameHeaderSize;
        const std::size_t delivered = std::min<std::size_t>(claimed, available);
        if (payload.capacity() > 2 * delivered + Spark::Daemon::kRecvFrameGrowthStep)
            InvariantFailure("payload allocation ran ahead of the bytes the peer delivered");

        const bool expected = claimed <= Spark::Daemon::kMaxPayloadSize && claimed <= available;
        if (accepted != expected)
            InvariantFailure("frame acceptance does not match the frame model");
        if (!accepted)
            return 0;

        if (header.payloadSize != claimed || header.serviceId != ReadLE16(frame + 4) ||
            header.messageType != ReadLE16(frame + 6))
            InvariantFailure("accepted header differs from its wire bytes");
        if (payload.size() != claimed ||
            !std::equal(payload.begin(), payload.end(), frame + Spark::Daemon::kFrameHeaderSize))
            InvariantFailure("accepted payload differs from its wire bytes");
        CheckStatsPayload(payload);
        cursor += Spark::Daemon::kFrameHeaderSize + claimed;
    }
}
