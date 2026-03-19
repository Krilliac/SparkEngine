/**
 * @file NetworkEncryption.h
 * @brief Lightweight symmetric encryption layer for game network traffic
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides packet-level encryption for game network traffic using XOR-based
 * stream cipher with per-connection session keys. This is not a replacement
 * for TLS/DTLS but provides adequate protection against casual packet
 * sniffing and replay attacks for real-time game data.
 *
 * Features:
 * - Per-connection 256-bit session keys derived from handshake
 * - Sequence-number-based nonce to prevent replay attacks
 * - Connection token validation to prevent spoofing
 * - Rate limiting per client IP
 * - HMAC-style integrity check per packet
 *
 * Build: Compiled when ENABLE_NETWORKING is defined.
 */

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Net
{

    // ============================================================================
    // Constants
    // ============================================================================

    constexpr size_t SESSION_KEY_SIZE = 32;                        ///< 256-bit session key
    constexpr size_t NONCE_SIZE = 8;                               ///< 64-bit nonce (sequence-based)
    constexpr size_t HMAC_SIZE = 4;                                ///< 32-bit truncated integrity tag
    constexpr size_t TOKEN_SIZE = 16;                              ///< 128-bit connection token
    constexpr size_t ENCRYPTION_OVERHEAD = NONCE_SIZE + HMAC_SIZE; ///< Bytes added per packet

    // ============================================================================
    // Session Key
    // ============================================================================

    using SessionKey = std::array<uint8_t, SESSION_KEY_SIZE>;
    using ConnectionToken = std::array<uint8_t, TOKEN_SIZE>;

    /**
     * @brief Generate a random session key
     * @return 256-bit random key suitable for packet encryption
     */
    SessionKey GenerateSessionKey();

    /**
     * @brief Generate a random connection token
     * @return 128-bit random token for connection authentication
     */
    ConnectionToken GenerateConnectionToken();

    // ============================================================================
    // Packet Encryption / Decryption
    // ============================================================================

    /**
     * @brief Encrypt a packet payload in-place
     *
     * Prepends a nonce derived from the sequence number, XOR-encrypts the
     * payload with a key stream derived from the session key + nonce, and
     * appends a truncated HMAC for integrity verification.
     *
     * Output layout: [nonce (8B)] [encrypted payload] [hmac (4B)]
     *
     * @param key         Session key for this connection
     * @param sequence    Packet sequence number (used as nonce base)
     * @param payload     Plaintext payload data
     * @return Encrypted packet with nonce and HMAC prepended/appended
     */
    std::vector<uint8_t> EncryptPacket(const SessionKey& key, uint64_t sequence, const std::vector<uint8_t>& payload);

    /**
     * @brief Decrypt a packet and verify integrity
     *
     * Extracts the nonce, verifies the HMAC, and decrypts the payload.
     *
     * @param key         Session key for this connection
     * @param packet      Encrypted packet (nonce + ciphertext + hmac)
     * @param outPayload  Decrypted payload on success
     * @param outSequence Extracted sequence number from nonce
     * @return true if decryption and integrity check succeeded
     */
    bool DecryptPacket(const SessionKey& key, const std::vector<uint8_t>& packet, std::vector<uint8_t>& outPayload,
                       uint64_t& outSequence);

    // ============================================================================
    // Connection Token Validation
    // ============================================================================

    /**
     * @brief Validates that a client's connection token matches the expected value
     *
     * During the handshake, the server generates a token and sends it to the
     * client. The client must echo this token in subsequent packets.
     *
     * @param expected The token the server generated
     * @param received The token the client sent
     * @return true if tokens match (constant-time comparison)
     */
    bool ValidateToken(const ConnectionToken& expected, const ConnectionToken& received);

    // ============================================================================
    // Rate Limiter
    // ============================================================================

    /**
     * @brief Per-client rate limiter to prevent packet flooding
     *
     * Tracks packet counts per source address and rejects clients that
     * exceed the configured rate. Uses a sliding window approach.
     */
    class RateLimiter
    {
      public:
        /**
         * @brief Configure rate limits
         * @param maxPacketsPerSecond Maximum packets allowed per second per client
         * @param burstAllowance Extra packets allowed in short bursts
         */
        explicit RateLimiter(uint32_t maxPacketsPerSecond = 100, uint32_t burstAllowance = 20);

        /**
         * @brief Check if a packet from this address should be allowed
         * @param addressHash Hash of the source IP:port
         * @return true if the packet is within rate limits
         */
        bool AllowPacket(uint64_t addressHash);

        /**
         * @brief Reset rate tracking for a specific client
         * @param addressHash Hash of the source IP:port
         */
        void ResetClient(uint64_t addressHash);

        /**
         * @brief Clear all rate tracking data
         */
        void Clear();

        /**
         * @brief Get current packet count for a client
         * @param addressHash Hash of the source IP:port
         * @return Number of packets received in the current window
         */
        uint32_t GetPacketCount(uint64_t addressHash) const;

      private:
        struct ClientRateInfo
        {
            uint32_t packetCount = 0;                          ///< Packets counted in the current window.
            std::chrono::steady_clock::time_point windowStart; ///< Start of the current 1-second counting window.
        };

        uint32_t m_maxPacketsPerSecond;                         ///< Hard limit before packets are dropped.
        uint32_t m_burstAllowance;                              ///< Extra packets allowed in short bursts.
        std::unordered_map<uint64_t, ClientRateInfo> m_clients; ///< Per-client rate tracking (keyed by address hash).
    };

    // ============================================================================
    // Replay Protection
    // ============================================================================

    /**
     * @brief Sliding-window replay protection using sequence numbers
     *
     * Tracks received sequence numbers and rejects packets with previously
     * seen or too-old sequence numbers.
     */
    class ReplayProtection
    {
      public:
        static constexpr size_t WINDOW_SIZE = 256;

        ReplayProtection() = default;

        /**
         * @brief Check if a sequence number is acceptable (not replayed)
         * @param sequence The packet sequence number
         * @return true if this is a new, valid sequence number
         */
        bool Accept(uint64_t sequence);

        /**
         * @brief Reset replay protection state
         */
        void Reset();

      private:
        uint64_t m_maxSequence = 0;
        std::array<bool, WINDOW_SIZE> m_window{};
    };

} // namespace Spark::Net
