/**
 * @file PacketValidator.h
 * @brief Network packet validation layer
 *
 * Validates incoming network packets against per-message-type schemas before
 * they reach game logic. Enforces maximum payload sizes, text-field acceptance
 * rules, and directional constraints (client-only vs server-only messages).
 *
 * The receive path REJECTS a packet whose text fields fail the acceptance rule;
 * it does not rewrite them. SanitizeString is an opt-in helper for a caller that
 * wants to clean a string it owns (an editor field, a locally composed message)
 * and is not part of ValidatePacket — see its declaration below.
 *
 * Sits between raw packet deserialization and message dispatch in
 * NetworkManager::ProcessIncoming().
 */

#pragma once

#include "NetworkWireLimits.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Net
{

    // Forward declarations from NetworkManager.h (avoids circular include)
    enum class MessageType : uint16_t;
    struct NetworkMessage;

    /**
     * @brief Why a packet was rejected
     */
    enum class PacketViolation : uint8_t
    {
        None,
        PayloadTooLarge,   ///< Exceeds global or per-type max size
        PayloadTooSmall,   ///< Below minimum expected size for this type
        InvalidType,       ///< Unknown or unregistered message type
        SchemaViolation,   ///< Payload doesn't match expected structure
        BadString,         ///< String contains invalid characters or encoding
        Unauthenticated,   ///< Message requires auth but sender is not authenticated
        DirectionViolation ///< Client sent a server-only message or vice versa
    };

    /**
     * @brief Result of packet validation
     */
    struct ValidationResult
    {
        bool valid = true;
        PacketViolation violation = PacketViolation::None;
        std::string reason;
    };

    /// stringFieldOffset value meaning "this payload carries no text fields".
    inline constexpr size_t NO_STRING_FIELDS = static_cast<size_t>(-1);

    /**
     * @brief Per-message-type validation schema
     */
    struct MessageSchema
    {
        size_t minPayloadSize = 0;
        size_t maxPayloadSize = 4096;
        bool requiresAuth = false;     ///< Sender must be an authenticated client
        bool allowedFromClient = true; ///< Clients may send this message type
        bool allowedFromServer = true; ///< Server may send this message type

        /**
         * @brief Byte offset at which the payload's length-prefixed text fields begin.
         *
         * NO_STRING_FIELDS (the default) means the payload is opaque binary and no
         * text validation is performed. Any other value declares that from that
         * offset to the end of the payload the bytes are a sequence of NetBuffer
         * strings (uint16 little-endian length, then that many bytes), which
         * ValidatePacket walks and passes through ValidateString. Only the schema
         * author knows the layout, which is why this is a schema field and not a
         * bool: a producer with a leading binary header must register its own
         * schema with the matching offset.
         */
        size_t stringFieldOffset = NO_STRING_FIELDS;
    };

    /**
     * @brief Packet validation statistics
     */
    struct PacketValidationStats
    {
        uint64_t totalValidated = 0;
        uint64_t totalRejected = 0;
        uint64_t rejectedTooLarge = 0;
        uint64_t rejectedTooSmall = 0;
        uint64_t rejectedInvalidType = 0;
        uint64_t rejectedBadString = 0;
        uint64_t rejectedUnauthenticated = 0;
        uint64_t rejectedDirection = 0;
    };

    /**
     * @brief Validates and sanitizes incoming network packets
     *
     * Register per-message-type schemas via RegisterSchema(). Call
     * ValidatePacket() on each incoming message before dispatching.
     *
     * Thread safety: RegisterSchema() is not thread-safe (call during init).
     * ValidatePacket() is safe to call from any thread (read-only after init).
     */
    class PacketValidator
    {
      public:
        PacketValidator();

        // ====================================================================
        // Configuration
        // ====================================================================

        /// Set the global maximum payload size (bytes), clamped to the UDP wire maximum.
        void SetMaxPayloadSize(size_t bytes);

        /// Set the global maximum string length for sanitization
        void SetMaxStringLength(size_t chars);

        /// Register a validation schema for a message type
        void RegisterSchema(MessageType type, const MessageSchema& schema);

        /// Register default schemas for all built-in message types
        void RegisterDefaultSchemas();

        // ====================================================================
        // Validation
        // ====================================================================

        /**
         * @brief Validate a network message against registered schemas
         * @param msg The message to validate
         * @param senderIsAuthenticated Whether the sender has completed auth
         * @param senderIsClient True if sender is a client, false if server
         * @return Validation result with reason on failure
         */
        [[nodiscard]] ValidationResult ValidatePacket(const NetworkMessage& msg, bool senderIsAuthenticated,
                                                      bool senderIsClient) const;

        /**
         * @brief Validate a string for dangerous content
         * @param str The string to validate
         * @return true if the string is safe
         */
        [[nodiscard]] bool ValidateString(const std::string& str) const;

        /**
         * @brief Validate the length-prefixed text fields of a payload.
         *
         * Walks NetBuffer string fields from @p offset to the end of @p payload and
         * runs ValidateString on each. ValidatePacket is the only in-tree caller;
         * it is public so a message handler that has already decoded a payload can
         * apply the same acceptance rule, and so the rule is directly testable.
         *
         * @param payload Raw packet payload.
         * @param offset  Byte offset of the first length prefix.
         * @return true when every field is well-formed, in-bounds, and safe.
         */
        [[nodiscard]] bool ValidateStringFields(const std::vector<uint8_t>& payload, size_t offset) const;

        /**
         * @brief Sanitize a string by removing control characters
         *
         * NOT used by the receive path: ValidatePacket rejects a packet with
         * unacceptable text rather than rewriting it, because a mutated payload no
         * longer matches what the sender signed or what the peer believes it sent.
         * This is a helper for a caller that owns the string and wants it cleaned
         * (locally composed text, an editor field). It has no in-tree caller today.
         *
         * @param str The string to sanitize (modified in place)
         */
        void SanitizeString(std::string& str) const;

        // ====================================================================
        // Statistics & Diagnostics
        // ====================================================================

        /// Get validation statistics
        [[nodiscard]] PacketValidationStats GetStatistics() const { return m_stats; }

        /// Reset statistics
        void ResetStatistics();

        /// Register console commands (packet.stats, packet.maxsize)
        void RegisterConsoleCommands();

        /// Get a human-readable status string
        [[nodiscard]] std::string GetStatusString() const;

      private:
        // May be configured lower, but never above the UDP wire contract.
        size_t m_maxPayloadSize = MAX_NETWORK_MESSAGE_PAYLOAD_SIZE;
        size_t m_maxStringLength = 1024;

        std::unordered_map<MessageType, MessageSchema> m_schemas;
        mutable PacketValidationStats m_stats;
    };

} // namespace Spark::Net
