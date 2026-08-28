/**
 * @file NetworkEncryption.h
 * @brief Legacy XOR/FNV packet-format prototype (not cryptography)
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides experimental packet-obfuscation helpers using an XOR-based stream
 * construction. This is not cryptographic confidentiality or authentication,
 * is not the active NetworkManager wire path, and must not protect credentials
 * or remotely exposed production game traffic.
 *
 * The encryption/HMAC/session-key names are legacy compatibility names. The
 * implementation uses predictable XOR state mixing and a short keyed FNV tag;
 * attackers can forge or recover it. Sequence and token helpers are local
 * equality/duplicate filters, not authenticated peer admission or replay
 * protection. Only the independent rate limiter is suitable as traffic control.
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

    constexpr size_t SESSION_KEY_SIZE = 32; ///< Legacy XOR state size; not a cryptographic key guarantee.
    constexpr size_t NONCE_SIZE = 8;        ///< Serialized sequence bytes; not a secure nonce.
    constexpr size_t HMAC_SIZE = 4;         ///< Legacy name for a forgeable 32-bit keyed FNV tag.
    constexpr size_t TOKEN_SIZE = 16;       ///< Prototype random-byte token size.
    constexpr size_t ENCRYPTION_OVERHEAD = NONCE_SIZE + HMAC_SIZE; ///< Legacy packet-format overhead.

    // ============================================================================
    // Session Key
    // ============================================================================

    using SessionKey = std::array<uint8_t, SESSION_KEY_SIZE>;
    using ConnectionToken = std::array<uint8_t, TOKEN_SIZE>;

    /**
     * @brief Generate pseudo-random state for the XOR prototype
     * @return Prototype state; not suitable as a production encryption key
     */
    SessionKey GenerateSessionKey();

    /**
     * @brief Generate pseudo-random bytes for prototype equality checks
     * @return Prototype token bytes; not peer authentication
     */
    ConnectionToken GenerateConnectionToken();

    // ============================================================================
    // Legacy packet transformation API
    // ============================================================================

    /**
     * @brief Apply the legacy XOR/FNV packet transform
     *
     * Prepends sequence bytes, XOR-obfuscates the payload, and appends a
     * forgeable keyed FNV tag. This provides no security boundary.
     *
     * Output layout: [sequence (8B)] [obfuscated payload] [prototype tag (4B)]
     *
     * @param key         Prototype XOR state
     * @param sequence    Packet sequence number
     * @param payload     Input payload data
     * @return Legacy transformed packet
     */
    std::vector<uint8_t> EncryptPacket(const SessionKey& key, uint64_t sequence, const std::vector<uint8_t>& payload);

    /**
     * @brief Reverse the legacy transform after checking its forgeable tag
     *
     * A successful tag comparison is not authentication or integrity against an attacker.
     *
     * @param key         Prototype XOR state
     * @param packet      Legacy transformed packet
     * @param outPayload  Recovered payload on success
     * @param outSequence Extracted sequence value
     * @return true if the packet shape and prototype tag matched
     */
    bool DecryptPacket(const SessionKey& key, const std::vector<uint8_t>& packet, std::vector<uint8_t>& outPayload,
                       uint64_t& outSequence);

    // ============================================================================
    // Prototype token equality
    // ============================================================================

    /**
     * @brief Constant-time byte equality for two prototype tokens
     *
     * This helper does not establish token secrecy, bind a token to an endpoint,
     * or integrate with NetworkManager admission; it does not authenticate peers.
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
     * @brief Per-address traffic-control limiter
     *
     * Tracks packet counts per source address and rejects traffic that exceeds
     * the configured rate. It does not authenticate the address or packet.
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
    // Sequence duplicate filter
    // ============================================================================

    /**
     * @brief Sliding-window duplicate sequence filter
     *
     * Tracks received sequence numbers and rejects previously seen or too-old
     * values. Without authenticated packets an attacker can forge sequences, so
     * this class is not a production replay-defense boundary.
     */
    class ReplayProtection
    {
      public:
        static constexpr size_t WINDOW_SIZE = 256;

        ReplayProtection() = default;

        /**
         * @brief Check whether a sequence number is new within the local window
         * @param sequence The packet sequence number
         * @return true if this is a new, valid sequence number
         */
        bool Accept(uint64_t sequence);

        /**
         * @brief Reset duplicate-sequence filter state
         */
        void Reset();

      private:
        uint64_t m_maxSequence = 0;
        std::array<bool, WINDOW_SIZE> m_window{};
    };

} // namespace Spark::Net
