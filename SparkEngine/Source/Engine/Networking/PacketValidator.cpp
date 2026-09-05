/**
 * @file PacketValidator.cpp
 * @brief Network packet validation and sanitization implementation
 */

#include "PacketValidator.h"
#include "NetworkManager.h"
#include "../../Utils/LogMacros.h"
#include "../../Utils/SparkConsole.h"
#include "../Security/MemoryIntegrity.h"

#include <algorithm>
#include <format>

namespace Spark::Net
{

    PacketValidator::PacketValidator()
    {
        RegisterDefaultSchemas();
    }

    // ========================================================================
    // Configuration
    // ========================================================================

    void PacketValidator::SetMaxPayloadSize(size_t bytes)
    {
        m_maxPayloadSize = (std::min)(bytes, MAX_NETWORK_MESSAGE_PAYLOAD_SIZE);
    }

    void PacketValidator::SetMaxStringLength(size_t chars)
    {
        m_maxStringLength = chars;
    }

    void PacketValidator::RegisterSchema(MessageType type, const MessageSchema& schema)
    {
        m_schemas[type] = schema;
    }

    void PacketValidator::RegisterDefaultSchemas()
    {
        // Connection messages
        RegisterSchema(MessageType::Connect, {.minPayloadSize = 0,
                                              .maxPayloadSize = 256,
                                              .requiresAuth = false,
                                              .allowedFromClient = true,
                                              .allowedFromServer = false});

        RegisterSchema(MessageType::ConnectAccepted, {.minPayloadSize = 4,
                                                      .maxPayloadSize = 8,
                                                      .requiresAuth = false,
                                                      .allowedFromClient = false,
                                                      .allowedFromServer = true});

        RegisterSchema(MessageType::ConnectRejected, {.minPayloadSize = 0,
                                                      .maxPayloadSize = 256,
                                                      .requiresAuth = false,
                                                      .allowedFromClient = false,
                                                      .allowedFromServer = true});

        RegisterSchema(MessageType::Disconnect, {.minPayloadSize = 0,
                                                 .maxPayloadSize = 64,
                                                 .requiresAuth = false,
                                                 .allowedFromClient = true,
                                                 .allowedFromServer = true});

        RegisterSchema(MessageType::Heartbeat, {.minPayloadSize = 0,
                                                .maxPayloadSize = 16,
                                                .requiresAuth = true,
                                                .allowedFromClient = true,
                                                .allowedFromServer = true});

        // Reliability
        RegisterSchema(MessageType::Ack, {.minPayloadSize = 8,
                                          .maxPayloadSize = 8,
                                          .requiresAuth = true,
                                          .allowedFromClient = true,
                                          .allowedFromServer = true});

        // Replication (server -> client only)
        RegisterSchema(MessageType::EntitySpawn, {.minPayloadSize = 4,
                                                  .maxPayloadSize = 2048,
                                                  .requiresAuth = true,
                                                  .allowedFromClient = false,
                                                  .allowedFromServer = true});

        RegisterSchema(MessageType::EntityDestroy, {.minPayloadSize = 4,
                                                    .maxPayloadSize = 64,
                                                    .requiresAuth = true,
                                                    .allowedFromClient = false,
                                                    .allowedFromServer = true});

        RegisterSchema(MessageType::EntityStateUpdate, {.minPayloadSize = 8,
                                                        .maxPayloadSize = 2048,
                                                        .requiresAuth = true,
                                                        .allowedFromClient = true,
                                                        .allowedFromServer = true});

        RegisterSchema(MessageType::EntityRPC, {.minPayloadSize = 4,
                                                .maxPayloadSize = 1024,
                                                .requiresAuth = true,
                                                .allowedFromClient = true,
                                                .allowedFromServer = true});

        // Input (client -> server only)
        RegisterSchema(MessageType::ClientInput, {.minPayloadSize = 8,
                                                  .maxPayloadSize = 512,
                                                  .requiresAuth = true,
                                                  .allowedFromClient = true,
                                                  .allowedFromServer = false});

        RegisterSchema(MessageType::InputAck, {.minPayloadSize = 4,
                                               .maxPayloadSize = 64,
                                               .requiresAuth = true,
                                               .allowedFromClient = false,
                                               .allowedFromServer = true});

        // Game messages
        RegisterSchema(MessageType::ChatMessage, {.minPayloadSize = 1,
                                                  .maxPayloadSize = 1024,
                                                  .requiresAuth = true,
                                                  .allowedFromClient = true,
                                                  .allowedFromServer = true,
                                                  .stringFieldOffset = 0});

        RegisterSchema(MessageType::GameStateSync, {.minPayloadSize = 4,
                                                    .maxPayloadSize = 4096,
                                                    .requiresAuth = true,
                                                    .allowedFromClient = false,
                                                    .allowedFromServer = true});

        RegisterSchema(MessageType::MatchStart, {.minPayloadSize = 0,
                                                 .maxPayloadSize = 512,
                                                 .requiresAuth = true,
                                                 .allowedFromClient = false,
                                                 .allowedFromServer = true});

        RegisterSchema(MessageType::MatchEnd, {.minPayloadSize = 0,
                                               .maxPayloadSize = 512,
                                               .requiresAuth = true,
                                               .allowedFromClient = false,
                                               .allowedFromServer = true});

        RegisterSchema(MessageType::PlayerRespawn, {.minPayloadSize = 4,
                                                    .maxPayloadSize = 256,
                                                    .requiresAuth = true,
                                                    .allowedFromClient = true,
                                                    .allowedFromServer = true});

        RegisterSchema(MessageType::ScoreUpdate, {.minPayloadSize = 4,
                                                  .maxPayloadSize = 256,
                                                  .requiresAuth = true,
                                                  .allowedFromClient = false,
                                                  .allowedFromServer = true});

        // Delta replication acknowledgement (client -> server only):
        // [4 bytes deltaSequence] echoed from an applied EntityStateUpdate
        RegisterSchema(MessageType::DeltaAck, {.minPayloadSize = 4,
                                               .maxPayloadSize = 16,
                                               .requiresAuth = true,
                                               .allowedFromClient = true,
                                               .allowedFromServer = false});
    }

    // ========================================================================
    // Validation
    // ========================================================================

    ValidationResult PacketValidator::ValidatePacket(const NetworkMessage& msg, bool senderIsAuthenticated,
                                                     bool senderIsClient) const
    {
        // Memory integrity: prove the packet validation pipeline actually runs.
        // If an attacker NOP's this function, the checkpoint will never fire and
        // VerifyBranchExecuted will flag a bypass.
        SPARK_INTEGRITY_CHECKPOINT("packet_validation_entry");

        m_stats.totalValidated++;

        size_t payloadSize = msg.payload.size();

        // Global max payload size
        SPARK_BRANCH_GUARD_BEGIN("packet_size_check")
        if (payloadSize > m_maxPayloadSize)
        {
            m_stats.totalRejected++;
            m_stats.rejectedTooLarge++;
            return {false, PacketViolation::PayloadTooLarge,
                    std::format("Payload {} bytes exceeds global max {}", payloadSize, m_maxPayloadSize)};
        }
        SPARK_BRANCH_GUARD_END("packet_size_check")

        // Look up per-type schema
        auto schemaIt = m_schemas.find(msg.type);

        // Unknown message types above UserDefined are allowed (custom game messages)
        if (schemaIt == m_schemas.end())
        {
            if (static_cast<uint16_t>(msg.type) < static_cast<uint16_t>(MessageType::UserDefined))
            {
                m_stats.totalRejected++;
                m_stats.rejectedInvalidType++;
                return {false, PacketViolation::InvalidType,
                        std::format("Unknown built-in message type: {}", static_cast<uint16_t>(msg.type))};
            }
            // Custom types remain schema-optional for compatibility with game
            // modules, but they may only enter through an authenticated peer.
            if (!senderIsAuthenticated)
            {
                m_stats.totalRejected++;
                m_stats.rejectedUnauthenticated++;
                return {false, PacketViolation::Unauthenticated,
                        std::format("Unauthenticated custom message type: {}", static_cast<uint16_t>(msg.type))};
            }
            return {true, PacketViolation::None, ""};
        }

        const auto& schema = schemaIt->second;

        // Payload size bounds
        if (payloadSize < schema.minPayloadSize)
        {
            m_stats.totalRejected++;
            m_stats.rejectedTooSmall++;
            return {false, PacketViolation::PayloadTooSmall,
                    std::format("Payload {} bytes below minimum {} for type {}", payloadSize, schema.minPayloadSize,
                                static_cast<uint16_t>(msg.type))};
        }

        if (payloadSize > schema.maxPayloadSize)
        {
            m_stats.totalRejected++;
            m_stats.rejectedTooLarge++;
            return {false, PacketViolation::PayloadTooLarge,
                    std::format("Payload {} bytes exceeds max {} for type {}", payloadSize, schema.maxPayloadSize,
                                static_cast<uint16_t>(msg.type))};
        }

        // Authentication check — bypassing this allows unauthenticated privilege escalation
        SPARK_BRANCH_GUARD_BEGIN("packet_auth_check")
        if (schema.requiresAuth && !senderIsAuthenticated)
        {
            m_stats.totalRejected++;
            m_stats.rejectedUnauthenticated++;
            return {false, PacketViolation::Unauthenticated,
                    std::format("Message type {} requires authentication", static_cast<uint16_t>(msg.type))};
        }
        SPARK_BRANCH_GUARD_END("packet_auth_check")

        // Direction check — bypassing allows clients to send server-only commands
        SPARK_BRANCH_GUARD_BEGIN("packet_direction_check")
        if (senderIsClient && !schema.allowedFromClient)
        {
            m_stats.totalRejected++;
            m_stats.rejectedDirection++;
            return {false, PacketViolation::DirectionViolation,
                    std::format("Message type {} not allowed from client", static_cast<uint16_t>(msg.type))};
        }

        if (!senderIsClient && !schema.allowedFromServer)
        {
            m_stats.totalRejected++;
            m_stats.rejectedDirection++;
            return {false, PacketViolation::DirectionViolation,
                    std::format("Message type {} not allowed from server", static_cast<uint16_t>(msg.type))};
        }
        SPARK_BRANCH_GUARD_END("packet_direction_check")

        // Text check — a schema that declares a string-field offset gets its text
        // fields decoded and screened here. Without this the declaration is inert
        // and rejectedBadString reads as "checked, none found" forever.
        if (schema.stringFieldOffset != NO_STRING_FIELDS && !ValidateStringFields(msg.payload, schema.stringFieldOffset))
        {
            m_stats.totalRejected++;
            m_stats.rejectedBadString++;
            return {false, PacketViolation::BadString,
                    std::format("Message type {} carries malformed or unsafe text",
                                static_cast<uint16_t>(msg.type))};
        }

        SPARK_VERIFY_CHECKPOINT("packet_validation_entry");
        return {true, PacketViolation::None, ""};
    }

    bool PacketValidator::ValidateStringFields(const std::vector<uint8_t>& payload, size_t offset) const
    {
        if (offset > payload.size())
            return false;

        size_t pos = offset;
        while (pos < payload.size())
        {
            // NetBuffer::WriteString writes a uint16 little-endian length prefix.
            if (payload.size() - pos < sizeof(uint16_t))
                return false;

            const size_t length =
                static_cast<size_t>(payload[pos]) | (static_cast<size_t>(payload[pos + 1]) << 8);
            pos += sizeof(uint16_t);

            if (length > payload.size() - pos)
                return false;

            const std::string field(reinterpret_cast<const char*>(payload.data() + pos), length);
            if (!ValidateString(field))
                return false;

            pos += length;
        }

        return true;
    }

    bool PacketValidator::ValidateString(const std::string& str) const
    {
        if (str.size() > m_maxStringLength)
        {
            return false;
        }

        // Reject strings with control characters (except newline and tab).
        // The byte must be read UNSIGNED: `char` is signed on MSVC, so every
        // UTF-8 continuation byte (>= 0x80) compares as negative, and screening
        // would reject every non-ASCII chat line as a control character.
        for (const char rawByte : str)
        {
            const auto c = static_cast<unsigned char>(rawByte);
            if (c < 0x20 && c != '\n' && c != '\t' && c != '\r')
            {
                return false;
            }
            // Reject DEL character
            if (c == 0x7F)
            {
                return false;
            }
        }

        return true;
    }

    void PacketValidator::SanitizeString(std::string& str) const
    {
        // Truncate to max length
        if (str.size() > m_maxStringLength)
        {
            str.resize(m_maxStringLength);
        }

        // Remove control characters except newline, tab, carriage return
        // Unsigned for the same reason as ValidateString: a signed char would
        // classify every UTF-8 continuation byte as a control character.
        std::erase_if(str,
                      [](const char ch) -> bool
                      {
                          const auto c = static_cast<unsigned char>(ch);
                          return (c < 0x20 && c != '\n' && c != '\t' && c != '\r') || c == 0x7F;
                      });
    }

    // ========================================================================
    // Statistics & Diagnostics
    // ========================================================================

    void PacketValidator::ResetStatistics()
    {
        m_stats = {};
    }

    std::string PacketValidator::GetStatusString() const
    {
        return std::format(
            "Packet Validator: max payload {} bytes | max string {} chars\n"
            "Validated: {} | Rejected: {} (large: {}, small: {}, type: {}, string: {}, auth: {}, dir: {})\n"
            "Registered schemas: {}",
            m_maxPayloadSize, m_maxStringLength, m_stats.totalValidated, m_stats.totalRejected,
            m_stats.rejectedTooLarge, m_stats.rejectedTooSmall, m_stats.rejectedInvalidType, m_stats.rejectedBadString,
            m_stats.rejectedUnauthenticated, m_stats.rejectedDirection, m_schemas.size());
    }

    void PacketValidator::RegisterConsoleCommands()
    {
        auto& console = SimpleConsole::GetInstance();

        console.RegisterCommand(
            "packet.stats", [this](const std::vector<std::string>&) -> std::string { return GetStatusString(); },
            "Show packet validation statistics");

        console.RegisterCommand(
            "packet.maxsize",
            [this](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty())
                {
                    return std::format("Max payload size: {} bytes", m_maxPayloadSize);
                }

                try
                {
                    size_t newSize = std::stoull(args[0]);
                    SetMaxPayloadSize(newSize);
                    return std::format("Max payload size set to {} bytes", m_maxPayloadSize);
                }
                catch (...)
                {
                    return "Usage: packet.maxsize [bytes]";
                }
            },
            "Get/set maximum packet payload size");

        console.RegisterCommand(
            "packet.reset",
            [this](const std::vector<std::string>&) -> std::string
            {
                ResetStatistics();
                return "Packet validation statistics reset.";
            },
            "Reset packet validation statistics");
    }

} // namespace Spark::Net
