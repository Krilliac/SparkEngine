/**
 * @file DaemonProtocol.h
 * @brief Wire format and shared constants for SparkDaemon IPC.
 *
 * A frame on the wire is:
 *
 * @code
 *   [4 bytes: payload length (LE)][2 bytes: service ID (LE)][2 bytes: message type (LE)][N bytes: payload]
 * @endcode
 *
 * `payload length` counts only the N payload bytes — the 4-byte service ID / message
 * type header is always present in addition and is not counted. Maximum payload size
 * is `kMaxPayloadSize`; frames larger than that are rejected as malformed.
 *
 * Payloads themselves are serialised with `Spark::BinaryWriter` / `Spark::BinaryReader`
 * (little-endian). Each service owns its own message type enum.
 *
 * The same header is used for request and response frames on the same socket.
 * Clients correlate responses to requests by ordering (send one, read one).
 *
 * This header is shared between the engine (`SparkEngineLib`) and the daemon
 * executable (`SparkDaemon`) — keep it free of engine-specific dependencies.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Daemon
{

    /// Wire-format service identifiers. One per daemon service.
    enum class ServiceId : uint16_t
    {
        Control = 0x0000, ///< Built-in: ping, shutdown, version query.
        Asset = 0x0001,   ///< Asset service (compiled asset cache, cook pipeline).
        Shader = 0x0002,  ///< Shader service (compile, disk cache, hot reload).
        Collab = 0x0003,  ///< Collaborative editing broker.
        Build = 0x0004,   ///< Build monitor (watch + incremental rebuild events).
    };

    /// Messages handled by the built-in Control service. Always present on every daemon.
    enum class ControlMessage : uint16_t
    {
        PingRequest = 0x0001,     ///< Empty payload.
        PingResponse = 0x0002,    ///< Payload: string `"pong"`.
        VersionRequest = 0x0003,  ///< Empty payload.
        VersionResponse = 0x0004, ///< Payload: string semver (e.g. `"1.0.0"`).
        ShutdownRequest = 0x0005, ///< Empty payload. Ack then exits.
        ShutdownAck = 0x0006,     ///< Empty payload.
        ErrorResponse = 0x00FF,   ///< Payload: string error message. Returned by server
                                  ///< when a request cannot be dispatched.
    };

    /// Wire-format frame header. All fields little-endian on the wire.
    struct FrameHeader
    {
        uint32_t payloadSize = 0; ///< Length of the payload in bytes (excluding header).
        uint16_t serviceId = 0;   ///< Target service (see ServiceId).
        uint16_t messageType = 0; ///< Service-specific message type.
    };

    /// Size of the fixed-length frame header on the wire.
    inline constexpr size_t kFrameHeaderSize = 8;

    /// Maximum allowed payload size (16 MiB). Frames above this are rejected as
    /// malformed to avoid runaway memory allocation on malicious / corrupted input.
    inline constexpr uint32_t kMaxPayloadSize = 16u * 1024u * 1024u;

    /// Current protocol semver. Bump when the frame header layout changes.
    inline constexpr const char* kProtocolVersion = "1.0.0";

    /**
     * @brief Default socket path for daemon discovery on POSIX systems.
     *
     * Relative to the build directory root. The engine forms the full path at
     * runtime (typically `<build>/.spark-daemon.sock`). On Windows, the daemon
     * listens on a named pipe at `\\.\pipe\spark-daemon` instead.
     */
    inline constexpr const char* kDefaultSocketName = ".spark-daemon.sock";

    /// Default Windows named-pipe name (full path).
    inline constexpr const char* kDefaultPipeName = R"(\\.\pipe\spark-daemon)";

    /**
     * @brief Serialise a frame header into a fixed-size byte array.
     *
     * The caller must pass a buffer of at least `kFrameHeaderSize` bytes.
     */
    inline void EncodeFrameHeader(const FrameHeader& header, uint8_t (&out)[kFrameHeaderSize]) noexcept
    {
        // Little-endian, handwritten so we don't depend on BinaryWriter here
        // (this header is reachable from both SparkEngineLib and SparkDaemon).
        out[0] = static_cast<uint8_t>(header.payloadSize & 0xFFu);
        out[1] = static_cast<uint8_t>((header.payloadSize >> 8) & 0xFFu);
        out[2] = static_cast<uint8_t>((header.payloadSize >> 16) & 0xFFu);
        out[3] = static_cast<uint8_t>((header.payloadSize >> 24) & 0xFFu);
        out[4] = static_cast<uint8_t>(header.serviceId & 0xFFu);
        out[5] = static_cast<uint8_t>((header.serviceId >> 8) & 0xFFu);
        out[6] = static_cast<uint8_t>(header.messageType & 0xFFu);
        out[7] = static_cast<uint8_t>((header.messageType >> 8) & 0xFFu);
    }

    /**
     * @brief Decode a frame header from raw bytes.
     *
     * @param in  Pointer to at least `kFrameHeaderSize` bytes.
     * @return    Decoded header. Callers should validate `payloadSize <= kMaxPayloadSize`.
     */
    [[nodiscard]] inline FrameHeader DecodeFrameHeader(const uint8_t* in) noexcept
    {
        FrameHeader h;
        h.payloadSize = static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
                        (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
        h.serviceId = static_cast<uint16_t>(in[4] | (static_cast<uint16_t>(in[5]) << 8));
        h.messageType = static_cast<uint16_t>(in[6] | (static_cast<uint16_t>(in[7]) << 8));
        return h;
    }

    /**
     * @brief Convenience: build the flat on-wire bytes (header + payload) for a frame.
     */
    [[nodiscard]] inline std::vector<uint8_t> EncodeFrame(ServiceId service, uint16_t messageType,
                                                          const std::vector<uint8_t>& payload)
    {
        std::vector<uint8_t> out;
        out.resize(kFrameHeaderSize + payload.size());

        FrameHeader header;
        header.payloadSize = static_cast<uint32_t>(payload.size());
        header.serviceId = static_cast<uint16_t>(service);
        header.messageType = messageType;

        uint8_t headerBytes[kFrameHeaderSize];
        EncodeFrameHeader(header, headerBytes);
        std::copy(std::begin(headerBytes), std::end(headerBytes), out.begin());
        if (!payload.empty())
            std::copy(payload.begin(), payload.end(), out.begin() + kFrameHeaderSize);

        return out;
    }

} // namespace Spark::Daemon
