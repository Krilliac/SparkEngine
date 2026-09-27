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
        // v3: the ConnectAccepted prefix (assigned client id, server time, echoed version) joined
        // the transcript. The v2 transcript never shipped in a release.
        constexpr std::string_view kTranscriptLabel = "SPNH-v3";
        constexpr std::string_view kSessionInfo = "spark-net-100 session v3";

        // ClientHello field offsets.
        constexpr size_t kClientMagic = 0;
        constexpr size_t kClientVersion = 4;
        constexpr size_t kClientSuite = 6;
        constexpr size_t kClientEphemeral = 7;
        constexpr size_t kClientNonce = kClientEphemeral + HANDSHAKE_PUBLIC_KEY_SIZE;
        static_assert(kClientEphemeral == CLIENT_HELLO_EPHEMERAL_OFFSET);

        // ServerHello field offsets; the signature covers everything before it.
        constexpr size_t kServerSuite = 0;
        constexpr size_t kServerIdentity = 1;
        constexpr size_t kServerEphemeral = kServerIdentity + HANDSHAKE_PUBLIC_KEY_SIZE;
        constexpr size_t kServerNonce = kServerEphemeral + HANDSHAKE_PUBLIC_KEY_SIZE;
        constexpr size_t kServerSignature = kServerNonce + HANDSHAKE_NONCE_SIZE;
        static_assert(kServerSignature + HANDSHAKE_SIGNATURE_SIZE == SERVER_HELLO_SIZE);

        using TranscriptHash = std::array<uint8_t, crypto_hash_sha256_BYTES>;

        /// th = SHA-256(label || ClientHello || accept prefix || ServerHello[0 .. signature)).
        /// Every part has a fixed length, so the concatenation is unambiguous.
        TranscriptHash HashTranscript(std::span<const uint8_t> clientHello, std::span<const uint8_t> acceptPrefix,
                                      std::span<const uint8_t> signedServerPart)
        {
            crypto_hash_sha256_state state;
            crypto_hash_sha256_init(&state);
            crypto_hash_sha256_update(&state, reinterpret_cast<const unsigned char*>(kTranscriptLabel.data()),
                                      kTranscriptLabel.size());
            crypto_hash_sha256_update(&state, clientHello.data(), clientHello.size());
            crypto_hash_sha256_update(&state, acceptPrefix.data(), acceptPrefix.size());
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

        /// Every Curve25519 u-coordinate of small order, in canonical and p+ form. This is
        /// libsodium's own blocklist (crypto_scalarmult/curve25519/ref10/x25519_ref10.c,
        /// has_small_order); bit 255 is ignored on comparison, as X25519 ignores it.
        constexpr std::array<std::array<uint8_t, 32>, 7> kSmallOrderPoints = {{
            // 0 (order 4)
            {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
            // 1 (order 1)
            {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
            // order 8
            {0xe0, 0xeb, 0x7a, 0x7c, 0x3b, 0x41, 0xb8, 0xae, 0x16, 0x56, 0xe3, 0xfa, 0xf1, 0x9f, 0xc4, 0x6a,
             0xda, 0x09, 0x8d, 0xeb, 0x9c, 0x32, 0xb1, 0xfd, 0x86, 0x62, 0x05, 0x16, 0x5f, 0x49, 0xb8, 0x00},
            // order 8
            {0x5f, 0x9c, 0x95, 0xbc, 0xa3, 0x50, 0x8c, 0x24, 0xb1, 0xd0, 0xb1, 0x55, 0x9c, 0x83, 0xef, 0x5b,
             0x04, 0x44, 0x5c, 0xc4, 0x58, 0x1c, 0x8e, 0x86, 0xd8, 0x22, 0x4e, 0xdd, 0xd0, 0x9f, 0x11, 0x57},
            // p - 1 (order 2)
            {0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
             0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
            // p (= 0, order 4)
            {0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
             0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
            // p + 1 (= 1, order 1)
            {0xee, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
             0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
        }};
    } // namespace

    bool IsLowOrderX25519PublicKey(std::span<const uint8_t> key) noexcept
    {
        if (key.size() != HANDSHAKE_PUBLIC_KEY_SIZE)
        {
            return true; // not a key at all: never worth a curve operation
        }
        // Public values only, so an early-exit comparison leaks nothing secret.
        for (const auto& point : kSmallOrderPoints)
        {
            bool same = (key[31] & 0x7f) == point[31];
            for (size_t i = 0; same && i < 31; ++i)
            {
                same = key[i] == point[i];
            }
            if (same)
            {
                return true;
            }
        }
        return false;
    }

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
                                                                              std::span<const uint8_t> acceptPrefix,
                                                                              const ServerIdentity& identity)
    {
        if (acceptPrefix.size() != CONNECT_ACCEPT_PREFIX_SIZE)
        {
            return std::unexpected(HandshakeError::Malformed);
        }
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
        // A small-order client key is refused before any curve operation, so a flood of them
        // costs the server comparisons only (NetworkManager::HandleConnect checks it earlier still).
        if (IsLowOrderX25519PublicKey(clientHello.subspan(kClientEphemeral, HANDSHAKE_PUBLIC_KEY_SIZE)))
        {
            return std::unexpected(HandshakeError::WeakSharedSecret);
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

        // Key agreement runs before the signature: any hello that fails costs at most one
        // scalar multiplication and never an Ed25519 signature.
        const TranscriptHash th = HashTranscript(clientHello, acceptPrefix, std::span(hello).first(kServerSignature));
        auto channel = DeriveChannel(ephemeralSecret, &clientHello[kClientEphemeral], th, ChannelRole::Server);
        sodium_memzero(ephemeralSecret.data(), ephemeralSecret.size());
        if (!channel)
        {
            return std::unexpected(channel.error());
        }
        crypto_sign_detached(&hello[kServerSignature], nullptr, th.data(), th.size(), identity.secretKey.data());
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
        std::span<const uint8_t> acceptPrefix, std::span<const uint8_t> serverHello, const ServerPublicKey& pinnedKey)
    {
        if (m_state != State::AwaitingServerHello)
        {
            return std::unexpected(HandshakeError::InvalidState);
        }
        // Whatever happens next, this ephemeral key is never used again.
        m_state = State::Done;

        std::expected<std::unique_ptr<SecureChannel>, HandshakeError> outcome =
            std::unexpected(HandshakeError::Malformed);
        if (serverHello.size() != SERVER_HELLO_SIZE || acceptPrefix.size() != CONNECT_ACCEPT_PREFIX_SIZE)
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
            const TranscriptHash th = HashTranscript(m_clientHello, acceptPrefix, serverHello.first(kServerSignature));
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
