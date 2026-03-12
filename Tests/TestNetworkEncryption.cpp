// TestNetworkEncryption.cpp - Tests for network encryption, token validation,
// rate limiting, and replay protection

#include "TestFramework.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

// ============================================================================
// Standalone reimplementation of encryption primitives for testing
// (mirrors Spark::Net from NetworkEncryption.h/cpp)
// ============================================================================

namespace
{
    constexpr size_t SESSION_KEY_SIZE = 32;
    constexpr size_t NONCE_SIZE = 8;
    constexpr size_t HMAC_SIZE = 4;
    constexpr size_t TOKEN_SIZE = 16;
    constexpr size_t ENCRYPTION_OVERHEAD = NONCE_SIZE + HMAC_SIZE;

    using SessionKey = std::array<uint8_t, SESSION_KEY_SIZE>;
    using ConnectionToken = std::array<uint8_t, TOKEN_SIZE>;

    static std::mt19937_64& GetRNG()
    {
        static std::mt19937_64 rng(42); // Fixed seed for reproducible tests
        return rng;
    }

    SessionKey GenerateSessionKey()
    {
        SessionKey key;
        auto& rng = GetRNG();
        std::uniform_int_distribution<unsigned int> dist(0, 255);
        for (auto& byte : key)
            byte = static_cast<uint8_t>(dist(rng));
        return key;
    }

    ConnectionToken GenerateConnectionToken()
    {
        ConnectionToken token;
        auto& rng = GetRNG();
        std::uniform_int_distribution<unsigned int> dist(0, 255);
        for (auto& byte : token)
            byte = static_cast<uint8_t>(dist(rng));
        return token;
    }

    void GenerateKeyStream(const SessionKey& key, uint64_t nonce, uint8_t* stream, size_t length)
    {
        uint8_t state[SESSION_KEY_SIZE];
        std::memcpy(state, key.data(), SESSION_KEY_SIZE);
        for (size_t i = 0; i < 8; ++i)
        {
            state[i] ^= static_cast<uint8_t>((nonce >> (i * 8)) & 0xFF);
            state[i + 8] ^= static_cast<uint8_t>((nonce >> (i * 8)) & 0xFF);
            state[i + 16] ^= static_cast<uint8_t>((nonce >> ((7 - i) * 8)) & 0xFF);
            state[i + 24] ^= static_cast<uint8_t>((nonce >> ((7 - i) * 8)) & 0xFF);
        }
        for (size_t i = 0; i < length; ++i)
        {
            size_t idx = i % SESSION_KEY_SIZE;
            state[idx] = static_cast<uint8_t>(state[idx] + state[(idx + 13) % SESSION_KEY_SIZE] + 1);
            stream[i] = state[idx];
        }
    }

    uint32_t ComputeHMAC(const SessionKey& key, const uint8_t* data, size_t length)
    {
        uint32_t hash = 0x811C9DC5u;
        for (size_t i = 0; i < SESSION_KEY_SIZE; ++i)
        {
            hash ^= key[i];
            hash *= 0x01000193u;
        }
        for (size_t i = 0; i < length; ++i)
        {
            hash ^= data[i];
            hash *= 0x01000193u;
        }
        return hash;
    }

    std::vector<uint8_t> EncryptPacket(const SessionKey& key, uint64_t sequence, const std::vector<uint8_t>& payload)
    {
        std::vector<uint8_t> packet;
        packet.reserve(NONCE_SIZE + payload.size() + HMAC_SIZE);
        for (size_t i = 0; i < NONCE_SIZE; ++i)
            packet.push_back(static_cast<uint8_t>((sequence >> (i * 8)) & 0xFF));
        std::vector<uint8_t> keyStream(payload.size());
        GenerateKeyStream(key, sequence, keyStream.data(), payload.size());
        for (size_t i = 0; i < payload.size(); ++i)
            packet.push_back(payload[i] ^ keyStream[i]);
        uint32_t hmac = ComputeHMAC(key, packet.data(), packet.size());
        for (size_t i = 0; i < HMAC_SIZE; ++i)
            packet.push_back(static_cast<uint8_t>((hmac >> (i * 8)) & 0xFF));
        return packet;
    }

    bool DecryptPacket(const SessionKey& key, const std::vector<uint8_t>& packet, std::vector<uint8_t>& outPayload,
                       uint64_t& outSequence)
    {
        if (packet.size() < ENCRYPTION_OVERHEAD)
            return false;
        size_t payloadSize = packet.size() - ENCRYPTION_OVERHEAD;
        size_t hmacOffset = packet.size() - HMAC_SIZE;
        uint32_t receivedHMAC = 0;
        for (size_t i = 0; i < HMAC_SIZE; ++i)
            receivedHMAC |= static_cast<uint32_t>(packet[hmacOffset + i]) << (i * 8);
        uint32_t computedHMAC = ComputeHMAC(key, packet.data(), hmacOffset);
        if (receivedHMAC != computedHMAC)
            return false;
        outSequence = 0;
        for (size_t i = 0; i < NONCE_SIZE; ++i)
            outSequence |= static_cast<uint64_t>(packet[i]) << (i * 8);
        std::vector<uint8_t> keyStream(payloadSize);
        GenerateKeyStream(key, outSequence, keyStream.data(), payloadSize);
        outPayload.resize(payloadSize);
        for (size_t i = 0; i < payloadSize; ++i)
            outPayload[i] = packet[NONCE_SIZE + i] ^ keyStream[i];
        return true;
    }

    bool ValidateToken(const ConnectionToken& expected, const ConnectionToken& received)
    {
        uint8_t diff = 0;
        for (size_t i = 0; i < TOKEN_SIZE; ++i)
            diff |= expected[i] ^ received[i];
        return diff == 0;
    }

    // Replay Protection
    class ReplayProtection
    {
      public:
        static constexpr size_t WINDOW_SIZE = 256;
        bool Accept(uint64_t sequence)
        {
            if (sequence == 0)
                return false;
            if (sequence > m_maxSequence)
            {
                uint64_t diff = sequence - m_maxSequence;
                if (diff >= WINDOW_SIZE)
                    m_window.fill(false);
                else
                    for (uint64_t i = 0; i < diff; ++i)
                        m_window[(m_maxSequence + 1 + i) % WINDOW_SIZE] = false;
                m_maxSequence = sequence;
                m_window[sequence % WINDOW_SIZE] = true;
                return true;
            }
            if (m_maxSequence - sequence >= WINDOW_SIZE)
                return false;
            size_t idx = sequence % WINDOW_SIZE;
            if (m_window[idx])
                return false;
            m_window[idx] = true;
            return true;
        }
        void Reset()
        {
            m_maxSequence = 0;
            m_window.fill(false);
        }

      private:
        uint64_t m_maxSequence = 0;
        std::array<bool, WINDOW_SIZE> m_window{};
    };

} // namespace

// ============================================================================
// Encrypt / Decrypt Round-Trip Tests
// ============================================================================

TEST(Encryption_RoundTrip)
{
    SessionKey key = GenerateSessionKey();
    std::vector<uint8_t> payload = {0x48, 0x65, 0x6C, 0x6C, 0x6F}; // "Hello"

    auto encrypted = EncryptPacket(key, 1, payload);
    EXPECT_GT(encrypted.size(), payload.size());

    std::vector<uint8_t> decrypted;
    uint64_t seq;
    bool ok = DecryptPacket(key, encrypted, decrypted, seq);
    EXPECT_TRUE(ok);
    EXPECT_EQ(seq, (uint64_t)1);
    EXPECT_EQ(decrypted.size(), payload.size());
    for (size_t i = 0; i < payload.size(); ++i)
        EXPECT_EQ(decrypted[i], payload[i]);
}

TEST(Encryption_DifferentSequences)
{
    SessionKey key = GenerateSessionKey();
    std::vector<uint8_t> payload = {1, 2, 3, 4};

    auto enc1 = EncryptPacket(key, 1, payload);
    auto enc2 = EncryptPacket(key, 2, payload);

    // Different sequences should produce different ciphertext
    bool different = false;
    for (size_t i = NONCE_SIZE; i < enc1.size() - HMAC_SIZE && i < enc2.size() - HMAC_SIZE; ++i)
    {
        if (enc1[i] != enc2[i])
        {
            different = true;
            break;
        }
    }
    EXPECT_TRUE(different);
}

TEST(Encryption_WrongKeyFails)
{
    SessionKey key1 = GenerateSessionKey();
    SessionKey key2 = GenerateSessionKey();
    std::vector<uint8_t> payload = {0xDE, 0xAD};

    auto encrypted = EncryptPacket(key1, 1, payload);

    std::vector<uint8_t> decrypted;
    uint64_t seq;
    bool ok = DecryptPacket(key2, encrypted, decrypted, seq);
    EXPECT_FALSE(ok); // HMAC check should fail
}

TEST(Encryption_TamperedPacketFails)
{
    SessionKey key = GenerateSessionKey();
    std::vector<uint8_t> payload = {1, 2, 3};

    auto encrypted = EncryptPacket(key, 1, payload);

    // Tamper with encrypted payload byte
    if (encrypted.size() > NONCE_SIZE + 1)
        encrypted[NONCE_SIZE + 1] ^= 0xFF;

    std::vector<uint8_t> decrypted;
    uint64_t seq;
    bool ok = DecryptPacket(key, encrypted, decrypted, seq);
    EXPECT_FALSE(ok);
}

TEST(Encryption_EmptyPayload)
{
    SessionKey key = GenerateSessionKey();
    std::vector<uint8_t> payload;

    auto encrypted = EncryptPacket(key, 42, payload);
    EXPECT_EQ(encrypted.size(), ENCRYPTION_OVERHEAD);

    std::vector<uint8_t> decrypted;
    uint64_t seq;
    bool ok = DecryptPacket(key, encrypted, decrypted, seq);
    EXPECT_TRUE(ok);
    EXPECT_EQ(seq, (uint64_t)42);
    EXPECT_TRUE(decrypted.empty());
}

TEST(Encryption_TooShortPacket)
{
    SessionKey key = GenerateSessionKey();
    std::vector<uint8_t> shortPacket = {1, 2, 3};

    std::vector<uint8_t> decrypted;
    uint64_t seq;
    bool ok = DecryptPacket(key, shortPacket, decrypted, seq);
    EXPECT_FALSE(ok);
}

// ============================================================================
// Token Validation Tests
// ============================================================================

TEST(Token_MatchingTokens)
{
    ConnectionToken token = GenerateConnectionToken();
    EXPECT_TRUE(ValidateToken(token, token));
}

TEST(Token_MismatchedTokens)
{
    ConnectionToken token1 = GenerateConnectionToken();
    ConnectionToken token2 = GenerateConnectionToken();
    EXPECT_FALSE(ValidateToken(token1, token2));
}

TEST(Token_SingleBitDifference)
{
    ConnectionToken token = GenerateConnectionToken();
    ConnectionToken tampered = token;
    tampered[0] ^= 1; // Flip one bit
    EXPECT_FALSE(ValidateToken(token, tampered));
}

// ============================================================================
// Replay Protection Tests
// ============================================================================

TEST(Replay_SequentialAccept)
{
    ReplayProtection rp;
    EXPECT_TRUE(rp.Accept(1));
    EXPECT_TRUE(rp.Accept(2));
    EXPECT_TRUE(rp.Accept(3));
}

TEST(Replay_DuplicateRejected)
{
    ReplayProtection rp;
    EXPECT_TRUE(rp.Accept(1));
    EXPECT_FALSE(rp.Accept(1)); // Duplicate
}

TEST(Replay_ZeroRejected)
{
    ReplayProtection rp;
    EXPECT_FALSE(rp.Accept(0));
}

TEST(Replay_OutOfOrderWithinWindow)
{
    ReplayProtection rp;
    EXPECT_TRUE(rp.Accept(5));
    EXPECT_TRUE(rp.Accept(3)); // Out of order but within window
    EXPECT_TRUE(rp.Accept(4));
    EXPECT_FALSE(rp.Accept(3)); // Already seen
}

TEST(Replay_TooOldRejected)
{
    ReplayProtection rp;
    EXPECT_TRUE(rp.Accept(300));
    EXPECT_FALSE(rp.Accept(1)); // Way outside window (300 - 1 >= 256)
}

TEST(Replay_WindowBoundary)
{
    ReplayProtection rp;
    EXPECT_TRUE(rp.Accept(256));
    EXPECT_TRUE(rp.Accept(1));  // Just within window (256 - 1 < 256)
    EXPECT_FALSE(rp.Accept(1)); // Duplicate
}

TEST(Replay_LargeJump)
{
    ReplayProtection rp;
    EXPECT_TRUE(rp.Accept(1));
    EXPECT_TRUE(rp.Accept(1000)); // Big jump forward
    EXPECT_FALSE(rp.Accept(1));   // Old packet rejected
    EXPECT_TRUE(rp.Accept(999));  // Recent packet accepted
}

TEST(Replay_Reset)
{
    ReplayProtection rp;
    EXPECT_TRUE(rp.Accept(1));
    EXPECT_FALSE(rp.Accept(1));
    rp.Reset();
    EXPECT_TRUE(rp.Accept(1)); // After reset, same sequence accepted
}
