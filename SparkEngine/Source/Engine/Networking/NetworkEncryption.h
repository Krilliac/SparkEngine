/**
 * @file NetworkEncryption.h
 * @brief ChaCha20-Poly1305 (RFC 8439) authenticated packet channel plus traffic-control helpers
 * @author Spark Engine Team
 * @date 2026
 *
 * Replaces the former XOR keystream + 32-bit FNV tag prototype. Packets are
 * sealed with the IETF ChaCha20-Poly1305 AEAD construction from RFC 8439,
 * pinned by the RFC's published known-answer vectors in
 * Tests/TestNET100TransportReal.cpp.
 *
 * Security properties provided by SecureChannel (each one is exercised by a
 * production-linked test):
 *  - confidentiality and integrity of the payload, the caller's associated
 *    data, and the packet header (version, key epoch, sequence);
 *  - per-direction keys derived from one shared secret, so a packet reflected
 *    back to its sender does not authenticate;
 *  - nonce uniqueness by construction: the sender owns a strictly increasing
 *    sequence counter and refuses to seal once it is exhausted;
 *  - authenticated replay rejection: the sliding window is only updated after
 *    the tag verifies, so forged packets cannot poison it;
 *  - forward key rotation (epoch + one-way HKDF ratchet);
 *  - fail-closed version handling: there is no plaintext or legacy mode, and
 *    any header version other than SECURE_TRANSPORT_VERSION is rejected.
 *
 * Not provided here (tracked under NET-100): the key-agreement handshake that
 * produces the shared secret, wiring into NetworkManager's live UDP path, and
 * independent review of this in-house implementation of the RFC primitive.
 *
 * Build: Compiled when ENABLE_NETWORKING is defined.
 */

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace Spark::Net
{

    // ============================================================================
    // Constants
    // ============================================================================

    constexpr size_t SESSION_KEY_SIZE = 32; ///< ChaCha20 key / shared-secret size (256-bit).
    constexpr size_t AEAD_NONCE_SIZE = 12;  ///< RFC 8439 96-bit nonce.
    constexpr size_t AEAD_TAG_SIZE = 16;    ///< Full 128-bit Poly1305 tag (never truncated).
    constexpr size_t TOKEN_SIZE = 16;       ///< CSPRNG connection-token size.

    constexpr uint8_t SECURE_TRANSPORT_VERSION = 1;  ///< Only accepted wire version; no downgrade path exists.
    constexpr size_t SECURE_HEADER_SIZE = 1 + 1 + 8; ///< [version][key epoch][sequence u64 LE].
    constexpr size_t SECURE_PACKET_OVERHEAD = SECURE_HEADER_SIZE + AEAD_TAG_SIZE;
    constexpr size_t SECURE_MAX_PAYLOAD = 64 * 1024; ///< Upper bound on a sealed payload (packet bounds).

    using SessionKey = std::array<uint8_t, SESSION_KEY_SIZE>;
    using AeadNonce = std::array<uint8_t, AEAD_NONCE_SIZE>;
    using ConnectionToken = std::array<uint8_t, TOKEN_SIZE>;

    // ============================================================================
    // Key / token generation (fail closed)
    // ============================================================================

    /**
     * @brief Fill a session secret from the OS CSPRNG
     * @param outKey Receives the key; zeroed on failure
     * @return false if the CSPRNG failed; callers must not proceed with the key
     */
    [[nodiscard]] bool GenerateSessionKey(SessionKey& outKey);

    /**
     * @brief Fill a connection token from the OS CSPRNG
     * @param outToken Receives the token; zeroed on failure
     * @return false if the CSPRNG failed
     */
    [[nodiscard]] bool GenerateConnectionToken(ConnectionToken& outToken);

    /**
     * @brief Constant-time token equality (no early exit on mismatch)
     * @param expected The token the server generated
     * @param received The token the client sent
     * @return true if the tokens are byte-identical
     */
    [[nodiscard]] bool ValidateToken(const ConnectionToken& expected, const ConnectionToken& received);

    /**
     * @brief Constant-time byte-span equality; a length mismatch returns false
     * @param a First span
     * @param b Second span
     * @return true if both spans have the same length and contents
     */
    [[nodiscard]] bool ConstantTimeEqual(std::span<const uint8_t> a, std::span<const uint8_t> b);

    // ============================================================================
    // Raw RFC 8439 AEAD
    // ============================================================================

    /**
     * @brief ChaCha20-Poly1305 seal (RFC 8439 section 2.8)
     *
     * Stateless: the caller is responsible for never reusing a (key, nonce)
     * pair. Transport code should use SecureChannel, which guarantees that.
     *
     * @param key       256-bit key
     * @param nonce     96-bit nonce, unique per key
     * @param aad       Associated data authenticated but not encrypted
     * @param plaintext Data to encrypt
     * @return ciphertext followed by the 16-byte tag
     */
    [[nodiscard]] std::vector<uint8_t> ChaCha20Poly1305Seal(const SessionKey& key, const AeadNonce& nonce,
                                                            std::span<const uint8_t> aad,
                                                            std::span<const uint8_t> plaintext);

    /**
     * @brief ChaCha20-Poly1305 open; the tag is verified in constant time before decrypting
     * @param key              256-bit key
     * @param nonce            96-bit nonce used to seal
     * @param aad              Associated data used to seal
     * @param ciphertextAndTag Ciphertext followed by the 16-byte tag
     * @param outPlaintext     Receives the plaintext on success; cleared on failure
     * @return false on truncation or tag mismatch
     */
    [[nodiscard]] bool ChaCha20Poly1305Open(const SessionKey& key, const AeadNonce& nonce, std::span<const uint8_t> aad,
                                            std::span<const uint8_t> ciphertextAndTag,
                                            std::vector<uint8_t>& outPlaintext);

    // ============================================================================
    // Sequence replay window
    // ============================================================================

    /**
     * @brief Sliding-window duplicate/too-old sequence filter
     *
     * On its own this is only a duplicate filter. SecureChannel consults it
     * after the AEAD tag verifies (sequence is authenticated header data), which
     * is what makes it replay protection.
     */
    class ReplayProtection
    {
      public:
        static constexpr size_t WINDOW_SIZE = 256;

        ReplayProtection() = default;

        /**
         * @brief Check whether a sequence would be accepted, without recording it
         * @param sequence The packet sequence number
         * @return true if the sequence is non-zero, not a duplicate, and inside the window
         */
        [[nodiscard]] bool IsFresh(uint64_t sequence) const;

        /**
         * @brief Record a sequence if it is fresh
         * @param sequence The packet sequence number
         * @return true if this is a new, valid sequence number
         */
        bool Accept(uint64_t sequence);

        /** @brief Reset filter state */
        void Reset();

      private:
        uint64_t m_maxSequence = 0;
        std::array<bool, WINDOW_SIZE> m_window{};
    };

    // ============================================================================
    // Authenticated packet channel
    // ============================================================================

    /** @brief Which end of the connection a SecureChannel represents (selects key direction). */
    enum class ChannelRole : uint8_t
    {
        Client,
        Server
    };

    /** @brief Outcome of SecureChannel::Open. Every value other than Ok means the packet was dropped. */
    enum class OpenResult : uint8_t
    {
        Ok,
        Malformed,            ///< Truncated, oversized, or zero sequence
        UnsupportedVersion,   ///< Header version is not SECURE_TRANSPORT_VERSION (downgrade attempt)
        UnknownKeyEpoch,      ///< Epoch is neither current nor the next rotation
        AuthenticationFailed, ///< Tag mismatch: tamper, wrong key, wrong direction, or wrong AAD
        Replayed              ///< Authenticated, but the sequence was already seen or is outside the window
    };

    /**
     * @brief One authenticated, encrypted, replay-protected packet channel
     *
     * Both peers construct a SecureChannel from the same shared secret with
     * opposite roles. Keys are derived per direction with HKDF-SHA256, and
     * rotated forward with a one-way ratchet. Sealed packet layout:
     * [version 1B][key epoch 1B][sequence 8B LE][ciphertext][tag 16B], where the
     * 10-byte header plus the caller's AAD are authenticated.
     */
    class SecureChannel
    {
      public:
        /**
         * @brief Derive per-direction keys from a shared secret
         * @param sharedSecret 256-bit secret agreed by both peers
         * @param role         This endpoint's role
         */
        SecureChannel(const SessionKey& sharedSecret, ChannelRole role);
        ~SecureChannel();

        SecureChannel(const SecureChannel&) = delete;
        SecureChannel& operator=(const SecureChannel&) = delete;

        /**
         * @brief Encrypt and authenticate one payload with the next sequence number
         * @param payload   Plaintext (at most SECURE_MAX_PAYLOAD bytes)
         * @param outPacket Receives the sealed packet
         * @param aad       Extra associated data (e.g. message type) both sides must agree on
         * @return false if the payload is too large or the sequence space is exhausted (rotate first)
         */
        [[nodiscard]] bool Seal(std::span<const uint8_t> payload, std::vector<uint8_t>& outPacket,
                                std::span<const uint8_t> aad = {});

        /**
         * @brief Authenticate, replay-check, and decrypt one packet
         * @param packet     Sealed packet from the peer
         * @param outPayload Receives the plaintext when the result is Ok; cleared otherwise
         * @param aad        Extra associated data; must match the sender's
         * @return OpenResult::Ok or the reason the packet was dropped
         */
        [[nodiscard]] OpenResult Open(std::span<const uint8_t> packet, std::vector<uint8_t>& outPayload,
                                      std::span<const uint8_t> aad = {});

        /**
         * @brief Ratchet the send key forward and restart the sequence at 1
         * @return false once the 8-bit epoch space is exhausted (establish a new session)
         */
        [[nodiscard]] bool RotateSendKey();

        /** @brief Current send key epoch */
        [[nodiscard]] uint8_t GetSendEpoch() const { return m_sendEpoch; }
        /** @brief Current receive key epoch */
        [[nodiscard]] uint8_t GetReceiveEpoch() const { return m_recvEpoch; }

      private:
        SessionKey m_sendKey{};
        SessionKey m_recvKey{};
        uint8_t m_sendEpoch = 0;
        uint8_t m_recvEpoch = 0;
        uint64_t m_nextSendSequence = 1;
        ReplayProtection m_replay;
    };

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

} // namespace Spark::Net
