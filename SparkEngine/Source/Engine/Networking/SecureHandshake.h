/**
 * @file SecureHandshake.h
 * @brief NET-100 key agreement: a signed-ephemeral X25519 handshake that yields a SecureChannel
 * @author Spark Engine Team
 * @date 2026
 *
 * The server holds a long-term Ed25519 identity whose public key the client has
 * pinned. The client is anonymous at the transport layer (players authenticate
 * later, inside the channel). One round trip:
 *
 *   ClientHello (55 bytes, little-endian integers)
 *     [magic u32 = NETWORK_HANDSHAKE_MAGIC][version u16 = NETWORK_PROTOCOL_VERSION]
 *     [suite u8][client X25519 pub 32][client nonce 16]
 *   ServerHello (145 bytes)
 *     [suite u8][server Ed25519 pub 32][server X25519 pub 32][server nonce 16][signature 64]
 *
 * Both sides compute the transcript hash
 *   th = SHA-256("SPNH-v2" || ClientHello || ServerHello without its signature)
 * and the server signs th. Version and suite sit inside the signed transcript,
 * so rewriting either breaks the signature (no downgrade). The session secret is
 * HKDF-SHA256(salt = th, ikm = X25519(ephemeral, peer ephemeral),
 * info = "spark-net-100 session v2"), and it is the only input to SecureChannel.
 * Fresh ephemerals and nonces on both sides make every session secret unique,
 * so SecureChannel's per-key nonce counter never repeats across sessions.
 *
 * Every primitive is libsodium's (crypto_box_keypair, crypto_scalarmult,
 * crypto_sign_*, crypto_hash_sha256, crypto_kdf_hkdf_sha256_*).
 *
 * Thread affinity: none. Each ClientHandshake is used by one thread at a time;
 * RespondToClientHello is reentrant.
 * Ownership: ClientHandshake owns its ephemeral secret and wipes it when Finish
 * returns or the object is destroyed. Results own their SecureChannel.
 * Allocation: one SecureChannel per completed handshake; never per packet.
 * Scalability tier: connection setup only, not a per-frame path.
 *
 * NetworkManager carries these messages: the ClientHello is the Connect payload
 * and the ServerHello follows the echoed version in ConnectAccepted
 * (docs/specs/networking-wire-format.md). Not provided: anti-amplification
 * cookies (a later NET-100 slice).
 */

#pragma once

#include "NetworkEncryption.h"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

namespace Spark::Net
{

    /// Suite 1: X25519 key agreement, Ed25519 server signature, ChaCha20-Poly1305
    /// channel, HKDF-SHA256 key derivation. The only suite accepted.
    constexpr uint8_t HANDSHAKE_SUITE_X25519_ED25519_CHACHAPOLY_HKDF_SHA256 = 1;

    constexpr size_t HANDSHAKE_PUBLIC_KEY_SIZE = 32;
    constexpr size_t HANDSHAKE_NONCE_SIZE = 16;
    constexpr size_t HANDSHAKE_SIGNATURE_SIZE = 64;
    constexpr size_t SERVER_SIGNING_SECRET_SIZE = 64;
    constexpr size_t CLIENT_HELLO_SIZE = 4 + 2 + 1 + HANDSHAKE_PUBLIC_KEY_SIZE + HANDSHAKE_NONCE_SIZE;
    constexpr size_t SERVER_HELLO_SIZE =
        1 + HANDSHAKE_PUBLIC_KEY_SIZE + HANDSHAKE_PUBLIC_KEY_SIZE + HANDSHAKE_NONCE_SIZE + HANDSHAKE_SIGNATURE_SIZE;

    using ServerPublicKey = std::array<uint8_t, HANDSHAKE_PUBLIC_KEY_SIZE>;
    using ClientHello = std::array<uint8_t, CLIENT_HELLO_SIZE>;
    using ServerHello = std::array<uint8_t, SERVER_HELLO_SIZE>;

    /** @brief Why a handshake step refused to proceed. No channel exists after any of these. */
    enum class HandshakeError : uint8_t
    {
        Malformed,              ///< Wrong length or magic
        UnsupportedVersion,     ///< ClientHello version is not NETWORK_PROTOCOL_VERSION
        UnsupportedSuite,       ///< Suite byte is not suite 1
        BadSignature,           ///< Transcript signature does not verify (tamper, replay, or reorder)
        ServerIdentityMismatch, ///< ServerHello names a key other than the pinned one
        WeakSharedSecret,       ///< Peer ephemeral key is low-order (all-zero X25519 result)
        CsprngFailure,          ///< libsodium failed to initialize
        InvalidState            ///< Begin/Finish called out of order or twice
    };

    /**
     * @brief Long-term server signing identity (Ed25519)
     *
     * The secret key never leaves the server and is wiped on destruction.
     */
    struct ServerIdentity
    {
        ServerPublicKey publicKey{};
        std::array<uint8_t, SERVER_SIGNING_SECRET_SIZE> secretKey{};

        ~ServerIdentity();
    };

    /**
     * @brief Generate a fresh server identity from the OS CSPRNG
     * @return The identity, or CsprngFailure
     */
    [[nodiscard]] std::expected<ServerIdentity, HandshakeError> GenerateServerIdentity();

    /** @brief What the server sends back and keeps after a valid ClientHello. */
    struct ServerHandshakeResult
    {
        ServerHello serverHello{};
        std::unique_ptr<SecureChannel> channel; ///< ChannelRole::Server
    };

    /**
     * @brief Validate a ClientHello and complete the server side of the handshake
     *
     * Stateless: nothing is allocated for a rejected hello.
     *
     * @param clientHello Bytes received from the client
     * @param identity    This server's signing identity
     * @return The ServerHello to send and the server's channel, or why the hello was refused
     */
    [[nodiscard]] std::expected<ServerHandshakeResult, HandshakeError> RespondToClientHello(
        std::span<const uint8_t> clientHello, const ServerIdentity& identity);

    /**
     * @brief Client side of the handshake: Begin once, then Finish once
     *
     * Any Finish result is final: the ephemeral secret is wiped, so a failed
     * handshake cannot be retried with a different ServerHello.
     */
    class ClientHandshake
    {
      public:
        ClientHandshake() = default;
        ~ClientHandshake();

        ClientHandshake(const ClientHandshake&) = delete;
        ClientHandshake& operator=(const ClientHandshake&) = delete;

        /**
         * @brief Generate the ephemeral key and nonce and build the ClientHello
         * @param protocolVersion Version to advertise. Production passes
         *                        NETWORK_PROTOCOL_VERSION (NetworkManager.h); tests pass
         *                        others to prove the server rejects them.
         * @return The ClientHello to send, or CsprngFailure / InvalidState
         */
        [[nodiscard]] std::expected<ClientHello, HandshakeError> Begin(uint16_t protocolVersion);

        /**
         * @brief Verify the ServerHello against the pinned key and derive the client channel
         * @param serverHello Bytes received from the server
         * @param pinnedKey   The server public key this client trusts
         * @return The client's channel (ChannelRole::Client), or why the handshake failed
         */
        [[nodiscard]] std::expected<std::unique_ptr<SecureChannel>, HandshakeError> Finish(
            std::span<const uint8_t> serverHello, const ServerPublicKey& pinnedKey);

      private:
        enum class State : uint8_t
        {
            Idle,
            AwaitingServerHello,
            Done
        };

        void Wipe();

        State m_state = State::Idle;
        std::array<uint8_t, 32> m_ephemeralSecret{};
        ClientHello m_clientHello{};
    };

} // namespace Spark::Net
