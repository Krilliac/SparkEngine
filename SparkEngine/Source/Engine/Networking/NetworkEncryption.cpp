/**
 * @file NetworkEncryption.cpp
 * @brief SecureChannel (keyed ChaCha20-Poly1305 packets), CSPRNG keys/tokens, replay window, rate limiter
 *
 * Every cryptographic primitive here is libsodium's (NET-100, owner decision
 * OD-06): the IETF ChaCha20-Poly1305 AEAD, HKDF-SHA256, randombytes_buf,
 * sodium_memcmp and sodium_memzero. This file only composes them.
 */

#include "NetworkEncryption.h"
#include "../../Utils/LogMacros.h"

#include <string_view>

#ifndef SPARK_HAS_LIBSODIUM
#error "NetworkEncryption.cpp requires libsodium (cmake/SparkLibsodium.cmake links spark_sodium)"
#endif
#include <sodium.h>

namespace Spark::Net
{

    static_assert(SESSION_KEY_SIZE == crypto_aead_chacha20poly1305_ietf_KEYBYTES);
    static_assert(AEAD_NONCE_SIZE == crypto_aead_chacha20poly1305_ietf_NPUBBYTES);
    static_assert(AEAD_TAG_SIZE == crypto_aead_chacha20poly1305_ietf_ABYTES);
    static_assert(SESSION_KEY_SIZE == crypto_kdf_hkdf_sha256_KEYBYTES);

    bool EnsureSodium()
    {
        // A function-local static is initialized exactly once, even under
        // concurrent first calls. sodium_init() returns 0 on first success, 1 when
        // already initialized, and -1 on failure.
        static const bool initialized = []
        {
            const bool ok = sodium_init() >= 0;
            if (!ok)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Network, "sodium_init() failed: network crypto is unavailable");
            }
            return ok;
        }();
        return initialized;
    }

    namespace
    {
        void StoreLE64(uint8_t* p, uint64_t v)
        {
            for (size_t i = 0; i < 8; ++i)
                p[i] = static_cast<uint8_t>(v >> (i * 8));
        }

        uint64_t LoadLE64(const uint8_t* p)
        {
            uint64_t v = 0;
            for (size_t i = 0; i < 8; ++i)
                v |= static_cast<uint64_t>(p[i]) << (i * 8);
            return v;
        }

        // ------------------------------------------------------------------------
        // HKDF-SHA256 (RFC 5869) with a single 32-byte output block
        // ------------------------------------------------------------------------

        SessionKey HkdfSha256(std::span<const uint8_t> salt, std::span<const uint8_t> ikm, std::string_view info)
        {
            SessionKey okm{};
            uint8_t prk[crypto_kdf_hkdf_sha256_KEYBYTES];
            // Neither call can fail for these lengths (32-byte output, no oversize input).
            crypto_kdf_hkdf_sha256_extract(prk, salt.data(), salt.size(), ikm.data(), ikm.size());
            crypto_kdf_hkdf_sha256_expand(okm.data(), okm.size(), info.data(), info.size(), prk);
            sodium_memzero(prk, sizeof(prk));
            return okm;
        }

        constexpr std::string_view kHkdfSalt = "SparkNet/v1 channel salt";
        constexpr std::string_view kClientToServerInfo = "SparkNet/v1 client->server";
        constexpr std::string_view kServerToClientInfo = "SparkNet/v1 server->client";
        constexpr std::string_view kRekeyInfo = "SparkNet/v1 rekey";

        std::span<const uint8_t> AsBytes(std::string_view text)
        {
            return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
        }

        /// One-way ratchet: the next epoch key cannot be used to recover the previous one.
        SessionKey NextEpochKey(const SessionKey& current)
        {
            return HkdfSha256(current, current, kRekeyInfo);
        }

        /// Nonce scheme: [epoch u8][0 0 0][sequence u64 LE]. Each epoch has its own
        /// key, and a sender uses each sequence at most once per epoch, so a
        /// (key, nonce) pair never repeats.
        AeadNonce MakeNonce(uint8_t epoch, uint64_t sequence)
        {
            AeadNonce nonce{};
            nonce[0] = epoch;
            StoreLE64(nonce.data() + 4, sequence);
            return nonce;
        }
    } // namespace

    // ============================================================================
    // Key / token generation and comparison
    // ============================================================================

    bool GenerateSessionKey(SessionKey& outKey)
    {
        if (!EnsureSodium())
        {
            sodium_memzero(outKey.data(), outKey.size());
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "CSPRNG unavailable: refusing to create a session key");
            return false;
        }
        randombytes_buf(outKey.data(), outKey.size());
        return true;
    }

    bool GenerateConnectionToken(ConnectionToken& outToken)
    {
        if (!EnsureSodium())
        {
            sodium_memzero(outToken.data(), outToken.size());
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "CSPRNG unavailable: refusing to create a connection token");
            return false;
        }
        randombytes_buf(outToken.data(), outToken.size());
        return true;
    }

    bool ValidateToken(const ConnectionToken& expected, const ConnectionToken& received)
    {
        return ConstantTimeEqual(expected, received);
    }

    bool ConstantTimeEqual(std::span<const uint8_t> a, std::span<const uint8_t> b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        if (a.empty())
        {
            return true;
        }
        return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
    }

    // ============================================================================
    // RFC 8439 AEAD (libsodium IETF ChaCha20-Poly1305)
    // ============================================================================

    std::vector<uint8_t> ChaCha20Poly1305Seal(const SessionKey& key, const AeadNonce& nonce,
                                              std::span<const uint8_t> aad, std::span<const uint8_t> plaintext)
    {
        if (!EnsureSodium())
        {
            return {}; // shorter than a tag: callers treat it as a failed seal
        }
        std::vector<uint8_t> out(plaintext.size() + AEAD_TAG_SIZE);
        unsigned long long outLen = 0;
        crypto_aead_chacha20poly1305_ietf_encrypt(out.data(), &outLen, plaintext.data(), plaintext.size(), aad.data(),
                                                  aad.size(), nullptr, nonce.data(), key.data());
        return out;
    }

    bool ChaCha20Poly1305Open(const SessionKey& key, const AeadNonce& nonce, std::span<const uint8_t> aad,
                              std::span<const uint8_t> ciphertextAndTag, std::vector<uint8_t>& outPlaintext)
    {
        outPlaintext.clear();
        if (ciphertextAndTag.size() < AEAD_TAG_SIZE || !EnsureSodium())
        {
            return false;
        }

        // libsodium verifies the tag in constant time before it decrypts, so
        // unauthenticated plaintext is never written.
        outPlaintext.resize(ciphertextAndTag.size() - AEAD_TAG_SIZE);
        unsigned long long plaintextLen = 0;
        if (crypto_aead_chacha20poly1305_ietf_decrypt(outPlaintext.data(), &plaintextLen, nullptr,
                                                      ciphertextAndTag.data(), ciphertextAndTag.size(), aad.data(),
                                                      aad.size(), nonce.data(), key.data()) != 0)
        {
            outPlaintext.clear();
            return false;
        }
        return true;
    }

    // ============================================================================
    // SecureChannel
    // ============================================================================

    SecureChannel::SecureChannel(const SessionKey& sharedSecret, ChannelRole role)
    {
        SessionKey clientToServer = HkdfSha256(AsBytes(kHkdfSalt), sharedSecret, kClientToServerInfo);
        SessionKey serverToClient = HkdfSha256(AsBytes(kHkdfSalt), sharedSecret, kServerToClientInfo);
        const bool isClient = role == ChannelRole::Client;
        m_sendKey = isClient ? clientToServer : serverToClient;
        m_recvKey = isClient ? serverToClient : clientToServer;
        sodium_memzero(clientToServer.data(), clientToServer.size());
        sodium_memzero(serverToClient.data(), serverToClient.size());
    }

    SecureChannel::~SecureChannel()
    {
        sodium_memzero(m_sendKey.data(), m_sendKey.size());
        sodium_memzero(m_recvKey.data(), m_recvKey.size());
    }

    bool SecureChannel::Seal(std::span<const uint8_t> payload, std::vector<uint8_t>& outPacket,
                             std::span<const uint8_t> aad)
    {
        outPacket.clear();
        if (payload.size() > SECURE_MAX_PAYLOAD)
            return false;

        // Nonce uniqueness is structural: every sequence value is used at most once per epoch key.
        if (m_nextSendSequence == UINT64_MAX)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "SecureChannel send sequence exhausted; rotate the key");
            return false;
        }
        const uint64_t sequence = m_nextSendSequence++;

        uint8_t header[SECURE_HEADER_SIZE];
        header[0] = SECURE_TRANSPORT_VERSION;
        header[1] = m_sendEpoch;
        StoreLE64(header + 2, sequence);

        std::vector<uint8_t> fullAad(header, header + SECURE_HEADER_SIZE);
        fullAad.insert(fullAad.end(), aad.begin(), aad.end());

        const auto sealed = ChaCha20Poly1305Seal(m_sendKey, MakeNonce(m_sendEpoch, sequence), fullAad, payload);
        if (sealed.size() != payload.size() + AEAD_TAG_SIZE)
        {
            return false; // libsodium unavailable; the consumed sequence is never reused
        }
        outPacket.reserve(SECURE_HEADER_SIZE + sealed.size());
        outPacket.assign(header, header + SECURE_HEADER_SIZE);
        outPacket.insert(outPacket.end(), sealed.begin(), sealed.end());
        return true;
    }

    OpenResult SecureChannel::Open(std::span<const uint8_t> packet, std::vector<uint8_t>& outPayload,
                                   std::span<const uint8_t> aad)
    {
        outPayload.clear();
        if (packet.size() < SECURE_PACKET_OVERHEAD || packet.size() > SECURE_PACKET_OVERHEAD + SECURE_MAX_PAYLOAD)
            return OpenResult::Malformed;
        if (packet[0] != SECURE_TRANSPORT_VERSION)
            return OpenResult::UnsupportedVersion;

        const uint8_t epoch = packet[1];
        const uint64_t sequence = LoadLE64(packet.data() + 2);
        if (sequence == 0)
            return OpenResult::Malformed;

        // Only the current epoch or the immediately following rotation is accepted.
        const bool isNextEpoch = m_recvEpoch != UINT8_MAX && epoch == static_cast<uint8_t>(m_recvEpoch + 1);
        if (epoch != m_recvEpoch && !isNextEpoch)
            return OpenResult::UnknownKeyEpoch;

        // Cheap pre-check; the window is only committed after authentication.
        if (!isNextEpoch && !m_replay.IsFresh(sequence))
            return OpenResult::Replayed;

        SessionKey candidateKey = isNextEpoch ? NextEpochKey(m_recvKey) : m_recvKey;

        std::vector<uint8_t> fullAad(packet.begin(), packet.begin() + SECURE_HEADER_SIZE);
        fullAad.insert(fullAad.end(), aad.begin(), aad.end());

        const bool authentic = ChaCha20Poly1305Open(candidateKey, MakeNonce(epoch, sequence), fullAad,
                                                    packet.subspan(SECURE_HEADER_SIZE), outPayload);
        if (!authentic)
        {
            sodium_memzero(candidateKey.data(), candidateKey.size());
            return OpenResult::AuthenticationFailed;
        }

        if (isNextEpoch)
        {
            // The peer proved possession of the next key: ratchet forward and drop the old one.
            sodium_memzero(m_recvKey.data(), m_recvKey.size());
            m_recvKey = candidateKey;
            m_recvEpoch = epoch;
            m_replay.Reset();
        }
        sodium_memzero(candidateKey.data(), candidateKey.size());

        m_replay.Accept(sequence);
        return OpenResult::Ok;
    }

    bool SecureChannel::RotateSendKey()
    {
        if (m_sendEpoch == UINT8_MAX)
            return false;
        SessionKey next = NextEpochKey(m_sendKey);
        m_sendKey = next;
        sodium_memzero(next.data(), next.size());
        ++m_sendEpoch;
        m_nextSendSequence = 1;
        return true;
    }

    // ============================================================================
    // Rate Limiter
    // ============================================================================

    RateLimiter::RateLimiter(uint32_t maxPacketsPerSecond, uint32_t burstAllowance)
        : m_maxPacketsPerSecond(maxPacketsPerSecond), m_burstAllowance(burstAllowance)
    {
    }

    bool RateLimiter::AllowPacket(uint64_t addressHash)
    {
        auto now = std::chrono::steady_clock::now();
        auto& info = m_clients[addressHash];

        // Check if window has expired (1 second sliding window)
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - info.windowStart);
        if (elapsed.count() >= 1000)
        {
            info.packetCount = 0;
            info.windowStart = now;
        }

        info.packetCount++;
        bool allowed = info.packetCount <= (m_maxPacketsPerSecond + m_burstAllowance);
        if (!allowed)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network,
                           "Rate limit exceeded for address hash %llu (%u packets/s, limit %u+%u)",
                           static_cast<unsigned long long>(addressHash), info.packetCount, m_maxPacketsPerSecond,
                           m_burstAllowance);
        }
        return allowed;
    }

    void RateLimiter::ResetClient(uint64_t addressHash)
    {
        m_clients.erase(addressHash);
    }

    void RateLimiter::Clear()
    {
        m_clients.clear();
    }

    uint32_t RateLimiter::GetPacketCount(uint64_t addressHash) const
    {
        auto it = m_clients.find(addressHash);
        return it != m_clients.end() ? it->second.packetCount : 0;
    }

    // ============================================================================
    // Sequence replay window
    // ============================================================================

    bool ReplayProtection::IsFresh(uint64_t sequence) const
    {
        if (sequence == 0)
            return false;
        if (sequence > m_maxSequence)
            return true;
        if (m_maxSequence - sequence >= WINDOW_SIZE)
            return false;
        return !m_window[sequence % WINDOW_SIZE];
    }

    bool ReplayProtection::Accept(uint64_t sequence)
    {
        if (!IsFresh(sequence))
            return false;

        if (sequence > m_maxSequence)
        {
            // New high water mark - clear window entries that are now too old
            const uint64_t diff = sequence - m_maxSequence;
            if (diff >= WINDOW_SIZE)
            {
                m_window.fill(false);
            }
            else
            {
                for (uint64_t i = 0; i < diff; ++i)
                    m_window[(m_maxSequence + 1 + i) % WINDOW_SIZE] = false;
            }
            m_maxSequence = sequence;
        }

        m_window[sequence % WINDOW_SIZE] = true;
        return true;
    }

    void ReplayProtection::Reset()
    {
        m_maxSequence = 0;
        m_window.fill(false);
    }

} // namespace Spark::Net
