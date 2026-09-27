/**
 * @file SecureHandshake.cpp
 * @brief Signed-ephemeral X25519 handshake (see SecureHandshake.h for the wire layout)
 */

#include "SecureHandshake.h"
#include "NetworkManager.h"

#include <string_view>

#ifndef SPARK_HAS_LIBSODIUM
#error "SecureHandshake.cpp requires libsodium (cmake/SparkLibsodium.cmake links spark_sodium)"
#endif
#include <sodium.h>

namespace Spark::Net
{

    static_assert(HANDSHAKE_PUBLIC_KEY_SIZE == crypto_sign_PUBLICKEYBYTES);
    static_assert(HANDSHAKE_PUBLIC_KEY_SIZE == crypto_box_PUBLICKEYBYTES);
    static_assert(HANDSHAKE_PUBLIC_KEY_SIZE == crypto_scalarmult_BYTES);
    static_assert(SERVER_SIGNING_SECRET_SIZE == crypto_sign_SECRETKEYBYTES);
    static_assert(HANDSHAKE_SIGNATURE_SIZE == crypto_sign_BYTES);
    static_assert(CLIENT_HELLO_SIZE == 55);
    static_assert(SERVER_HELLO_SIZE == 145);

    namespace
    {
        constexpr std::string_view kTranscriptLabel = "SPNH-v2";
        constexpr std::string_view kSessionInfo = "spark-net-100 session v2";

        // ClientHello field offsets.
        constexpr size_t kClientMagic = 0;
        constexpr size_t kClientVersion = 4;
        constexpr size_t kClientSuite = 6;
        constexpr size_t kClientEphemeral = 7;
        constexpr size_t kClientNonce = kClientEphemeral + HANDSHAKE_PUBLIC_KEY_SIZE;

        // ServerHello field offsets; the signature covers everything before it.
        constexpr size_t kServerSuite = 0;
        constexpr size_t kServerIdentity = 1;
        constexpr size_t kServerEphemeral = kServerIdentity + HANDSHAKE_PUBLIC_KEY_SIZE;
        constexpr size_t kServerNonce = kServerEphemeral + HANDSHAKE_PUBLIC_KEY_SIZE;
        constexpr size_t kServerSignature = kServerNonce + HANDSHAKE_NONCE_SIZE;
        static_assert(kServerSignature + HANDSHAKE_SIGNATURE_SIZE == SERVER_HELLO_SIZE);

        using TranscriptHash = std::array<uint8_t, crypto_hash_sha256_BYTES>;

        /// th = SHA-256(label || ClientHello || ServerHello[0 .. signature)).
        TranscriptHash HashTranscript(std::span<const uint8_t> clientHello, std::span<const uint8_t> signedServerPart)
        {
            crypto_hash_sha256_state state;
            crypto_hash_sha256_init(&state);
            crypto_hash_sha256_update(&state, reinterpret_cast<const unsigned char*>(kTranscriptLabel.data()),
                                      kTranscriptLabel.size());
            crypto_hash_sha256_update(&state, clientHello.data(), clientHello.size());
            crypto_hash_sha256_update(&state, signedServerPart.data(), signedServerPart.size());
            TranscriptHash th{};
            crypto_hash_sha256_final(&state, th.data());
            return th;
        }

        /// X25519 then HKDF-SHA256 into a SecureChannel. Fails on a low-order peer key.
        std::expected<std::unique_ptr<SecureChannel>, HandshakeError> DeriveChannel(
            const std::array<uint8_t, 32>& ephemeralSecret, const uint8_t* peerEphemeral, const TranscriptHash& th,
            ChannelRole role)
        {
            uint8_t shared[crypto_scalarmult_BYTES];
            // crypto_scalarmult rejects an all-zero result, which is what every
            // low-order (small-subgroup) point produces.
            if (crypto_scalarmult(shared, ephemeralSecret.data(), peerEphemeral) != 0)
            {
                sodium_memzero(shared, sizeof(shared));
                return std::unexpected(HandshakeError::WeakSharedSecret);
            }

            uint8_t prk[crypto_kdf_hkdf_sha256_KEYBYTES];
            SessionKey sessionSecret{};
            crypto_kdf_hkdf_sha256_extract(prk, th.data(), th.size(), shared, sizeof(shared));
            crypto_kdf_hkdf_sha256_expand(sessionSecret.data(), sessionSecret.size(), kSessionInfo.data(),
                                          kSessionInfo.size(), prk);
            auto channel = std::make_unique<SecureChannel>(sessionSecret, role);
            sodium_memzero(shared, sizeof(shared));
            sodium_memzero(prk, sizeof(prk));
            sodium_memzero(sessionSecret.data(), sessionSecret.size());
            return channel;
        }

        uint32_t LoadLE32(const uint8_t* p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }

        void StoreLE32(uint8_t* p, uint32_t v)
        {
            for (size_t i = 0; i < 4; ++i)
            {
                p[i] = static_cast<uint8_t>(v >> (i * 8));
            }
        }
    } // namespace

    // ============================================================================
    // Server
    // ============================================================================

    ServerIdentity::~ServerIdentity()
    {
        sodium_memzero(secretKey.data(), secretKey.size());
    }

    std::expected<ServerIdentity, HandshakeError> GenerateServerIdentity()
    {
        if (!EnsureSodium())
        {
            return std::unexpected(HandshakeError::CsprngFailure);
        }
        ServerIdentity identity;
        crypto_sign_keypair(identity.publicKey.data(), identity.secretKey.data());
        return identity;
    }

    std::expected<ServerHandshakeResult, HandshakeError> RespondToClientHello(std::span<const uint8_t> clientHello,
                                                                              const ServerIdentity& identity)
    {
        if (clientHello.size() != CLIENT_HELLO_SIZE || LoadLE32(&clientHello[kClientMagic]) != NETWORK_HANDSHAKE_MAGIC)
        {
            return std::unexpected(HandshakeError::Malformed);
        }
        const auto version =
            static_cast<uint16_t>(clientHello[kClientVersion] | (clientHello[kClientVersion + 1] << 8));
        if (version != NETWORK_PROTOCOL_VERSION)
        {
            return std::unexpected(HandshakeError::UnsupportedVersion);
        }
        if (clientHello[kClientSuite] != HANDSHAKE_SUITE_X25519_ED25519_CHACHAPOLY_HKDF_SHA256)
        {
            return std::unexpected(HandshakeError::UnsupportedSuite);
        }
        if (!EnsureSodium())
        {
            return std::unexpected(HandshakeError::CsprngFailure);
        }

        ServerHandshakeResult result;
        ServerHello& hello = result.serverHello;
        std::array<uint8_t, 32> ephemeralSecret{};
        hello[kServerSuite] = HANDSHAKE_SUITE_X25519_ED25519_CHACHAPOLY_HKDF_SHA256;
        std::copy(identity.publicKey.begin(), identity.publicKey.end(), hello.begin() + kServerIdentity);
        crypto_box_keypair(&hello[kServerEphemeral], ephemeralSecret.data());
        randombytes_buf(&hello[kServerNonce], HANDSHAKE_NONCE_SIZE);

        const TranscriptHash th = HashTranscript(clientHello, std::span(hello).first(kServerSignature));
        crypto_sign_detached(&hello[kServerSignature], nullptr, th.data(), th.size(), identity.secretKey.data());

        auto channel = DeriveChannel(ephemeralSecret, &clientHello[kClientEphemeral], th, ChannelRole::Server);
        sodium_memzero(ephemeralSecret.data(), ephemeralSecret.size());
        if (!channel)
        {
            return std::unexpected(channel.error());
        }
        result.channel = std::move(*channel);
        return result;
    }

    // ============================================================================
    // Client
    // ============================================================================

    ClientHandshake::~ClientHandshake()
    {
        Wipe();
    }

    void ClientHandshake::Wipe()
    {
        sodium_memzero(m_ephemeralSecret.data(), m_ephemeralSecret.size());
    }

    std::expected<ClientHello, HandshakeError> ClientHandshake::Begin(uint16_t protocolVersion)
    {
        if (m_state != State::Idle)
        {
            return std::unexpected(HandshakeError::InvalidState);
        }
        if (!EnsureSodium())
        {
            return std::unexpected(HandshakeError::CsprngFailure);
        }

        StoreLE32(&m_clientHello[kClientMagic], NETWORK_HANDSHAKE_MAGIC);
        m_clientHello[kClientVersion] = static_cast<uint8_t>(protocolVersion);
        m_clientHello[kClientVersion + 1] = static_cast<uint8_t>(protocolVersion >> 8);
        m_clientHello[kClientSuite] = HANDSHAKE_SUITE_X25519_ED25519_CHACHAPOLY_HKDF_SHA256;
        crypto_box_keypair(&m_clientHello[kClientEphemeral], m_ephemeralSecret.data());
        randombytes_buf(&m_clientHello[kClientNonce], HANDSHAKE_NONCE_SIZE);
        m_state = State::AwaitingServerHello;
        return m_clientHello;
    }

    std::expected<std::unique_ptr<SecureChannel>, HandshakeError> ClientHandshake::Finish(
        std::span<const uint8_t> serverHello, const ServerPublicKey& pinnedKey)
    {
        if (m_state != State::AwaitingServerHello)
        {
            return std::unexpected(HandshakeError::InvalidState);
        }
        // Whatever happens next, this ephemeral key is never used again.
        m_state = State::Done;

        std::expected<std::unique_ptr<SecureChannel>, HandshakeError> outcome =
            std::unexpected(HandshakeError::Malformed);
        if (serverHello.size() != SERVER_HELLO_SIZE)
        {
            outcome = std::unexpected(HandshakeError::Malformed);
        }
        else if (serverHello[kServerSuite] != HANDSHAKE_SUITE_X25519_ED25519_CHACHAPOLY_HKDF_SHA256)
        {
            outcome = std::unexpected(HandshakeError::UnsupportedSuite);
        }
        else if (sodium_memcmp(&serverHello[kServerIdentity], pinnedKey.data(), pinnedKey.size()) != 0)
        {
            outcome = std::unexpected(HandshakeError::ServerIdentityMismatch);
        }
        else
        {
            const TranscriptHash th = HashTranscript(m_clientHello, serverHello.first(kServerSignature));
            if (crypto_sign_verify_detached(&serverHello[kServerSignature], th.data(), th.size(), pinnedKey.data()) !=
                0)
            {
                outcome = std::unexpected(HandshakeError::BadSignature);
            }
            else
            {
                outcome = DeriveChannel(m_ephemeralSecret, &serverHello[kServerEphemeral], th, ChannelRole::Client);
            }
        }

        Wipe();
        return outcome;
    }

} // namespace Spark::Net
