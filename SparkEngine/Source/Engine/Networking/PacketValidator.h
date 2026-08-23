/**
 * @file PacketValidator.h
 * @brief Network packet validation and sanitization layer
 *
 * Validates incoming network packets against per-message-type schemas before
 * they reach game logic. Enforces maximum payload sizes, string sanitization,
 * and directional constraints (client-only vs server-only messages).
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
        bool sanitizeStrings = false;  ///< Strip control characters from string payloads
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
         * @brief Sanitize a string by removing control characters
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
