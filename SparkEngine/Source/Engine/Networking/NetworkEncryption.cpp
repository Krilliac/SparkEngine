/**
 * @file NetworkEncryption.cpp
 * @brief Network encryption, token validation, rate limiting, and replay protection
 */

#include "NetworkEncryption.h"
#include "../../Utils/Validate.h"
#include "../../Utils/Logger.h"

#include <algorithm>
#include <cstring>
#include <random>

namespace Spark::Net
{

    // ============================================================================
    // Key / Token Generation
    // ============================================================================

    static std::mt19937_64& GetRNG()
    {
        static std::mt19937_64 rng(std::random_device{}());
        return rng;
    }

    SessionKey GenerateSessionKey()
    {
        SessionKey key;
        auto& rng = GetRNG();
        std::uniform_int_distribution<unsigned int> dist(0, 255);
        for (auto& byte : key)
            byte = static_cast<uint8_t>(dist(rng));
        SPARK_LOG_DEBUG(Spark::LogCategory::Network, "Generated new session key (%zu bytes)", key.size());
        return key;
    }

    ConnectionToken GenerateConnectionToken()
    {
        ConnectionToken token;
        auto& rng = GetRNG();
        std::uniform_int_distribution<unsigned int> dist(0, 255);
        for (auto& byte : token)
            byte = static_cast<uint8_t>(dist(rng));
        SPARK_LOG_DEBUG(Spark::LogCategory::Network, "Generated new connection token (%zu bytes)", token.size());
        return token;
    }

    // ============================================================================
    // Key stream generation (XOR cipher with key + nonce mixing)
    // ============================================================================

    static void GenerateKeyStream(const SessionKey& key, uint64_t nonce, uint8_t* stream, size_t length)
    {
        // Deterministic XOR obfuscation only. This is not cryptographically
        // secure and provides no confidentiality or peer authentication.
        uint8_t state[SESSION_KEY_SIZE];
        std::memcpy(state, key.data(), SESSION_KEY_SIZE);

        // Mix nonce into state
        for (size_t i = 0; i < 8; ++i)
        {
            state[i] ^= static_cast<uint8_t>((nonce >> (i * 8)) & 0xFF);
            state[i + 8] ^= static_cast<uint8_t>((nonce >> (i * 8)) & 0xFF);
            state[i + 16] ^= static_cast<uint8_t>((nonce >> ((7 - i) * 8)) & 0xFF);
            state[i + 24] ^= static_cast<uint8_t>((nonce >> ((7 - i) * 8)) & 0xFF);
        }

        // Generate key stream bytes using state mixing
        for (size_t i = 0; i < length; ++i)
        {
            size_t idx = i % SESSION_KEY_SIZE;
            state[idx] = static_cast<uint8_t>(state[idx] + state[(idx + 13) % SESSION_KEY_SIZE] + 1);
            stream[i] = state[idx];
        }
    }

    // ============================================================================
    // Truncated HMAC (integrity tag)
    // ============================================================================

    static uint32_t ComputeHMAC(const SessionKey& key, const uint8_t* data, size_t length)
    {
        // Simple keyed hash: FNV-1a with key mixing
        uint32_t hash = 0x811C9DC5u; // FNV offset basis

        // Mix in key first
        for (size_t i = 0; i < SESSION_KEY_SIZE; ++i)
        {
            hash ^= key[i];
            hash *= 0x01000193u; // FNV prime
        }

        // Hash data
        for (size_t i = 0; i < length; ++i)
        {
            hash ^= data[i];
            hash *= 0x01000193u;
        }

        return hash;
    }

    // ============================================================================
    // Encrypt / Decrypt
    // ============================================================================

    std::vector<uint8_t> EncryptPacket(const SessionKey& key, uint64_t sequence, const std::vector<uint8_t>& payload)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        // Output: [nonce 8B] [encrypted payload] [hmac 4B]
        std::vector<uint8_t> packet;
        packet.reserve(NONCE_SIZE + payload.size() + HMAC_SIZE);

        // Write nonce (sequence number as little-endian bytes)
        for (size_t i = 0; i < NONCE_SIZE; ++i)
            packet.push_back(static_cast<uint8_t>((sequence >> (i * 8)) & 0xFF));

        // Generate key stream and XOR encrypt
        std::vector<uint8_t> keyStream(payload.size());
        GenerateKeyStream(key, sequence, keyStream.data(), payload.size());

        for (size_t i = 0; i < payload.size(); ++i)
            packet.push_back(payload[i] ^ keyStream[i]);

        // Compute HMAC over nonce + ciphertext
        uint32_t hmac = ComputeHMAC(key, packet.data(), packet.size());
        for (size_t i = 0; i < HMAC_SIZE; ++i)
            packet.push_back(static_cast<uint8_t>((hmac >> (i * 8)) & 0xFF));

        return packet;
    }

    bool DecryptPacket(const SessionKey& key, const std::vector<uint8_t>& packet, std::vector<uint8_t>& outPayload,
                       uint64_t& outSequence)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Network);
        if (packet.size() < ENCRYPTION_OVERHEAD)
            return false;

        size_t payloadSize = packet.size() - ENCRYPTION_OVERHEAD;

        // Extract and verify HMAC
        size_t hmacOffset = packet.size() - HMAC_SIZE;
        uint32_t receivedHMAC = 0;
        for (size_t i = 0; i < HMAC_SIZE; ++i)
            receivedHMAC |= static_cast<uint32_t>(packet[hmacOffset + i]) << (i * 8);

        uint32_t computedHMAC = ComputeHMAC(key, packet.data(), hmacOffset);
        if (receivedHMAC != computedHMAC)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "Packet integrity check failed: HMAC mismatch");
            return false;
        }

        // Extract nonce (sequence number)
        outSequence = 0;
        for (size_t i = 0; i < NONCE_SIZE; ++i)
            outSequence |= static_cast<uint64_t>(packet[i]) << (i * 8);

        // Decrypt payload
        std::vector<uint8_t> keyStream(payloadSize);
        GenerateKeyStream(key, outSequence, keyStream.data(), payloadSize);

        outPayload.resize(payloadSize);
        for (size_t i = 0; i < payloadSize; ++i)
            outPayload[i] = packet[NONCE_SIZE + i] ^ keyStream[i];

        return true;
    }

    // ============================================================================
    // Token Validation (constant-time comparison)
    // ============================================================================

    bool ValidateToken(const ConnectionToken& expected, const ConnectionToken& received)
    {
        uint8_t diff = 0;
        for (size_t i = 0; i < TOKEN_SIZE; ++i)
            diff |= expected[i] ^ received[i];
        bool valid = (diff == 0);
        if (!valid)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "Connection token validation failed");
        }
        else
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Network, "Connection token validated successfully");
        }
        return valid;
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
    // Replay Protection
    // ============================================================================

    bool ReplayProtection::Accept(uint64_t sequence)
    {
        if (sequence == 0)
            return false;

        if (sequence > m_maxSequence)
        {
            // New high water mark - clear window entries that are now too old
            uint64_t diff = sequence - m_maxSequence;
            if (diff >= WINDOW_SIZE)
            {
                m_window.fill(false);
            }
            else
            {
                for (uint64_t i = 0; i < diff; ++i)
                {
                    m_window[(m_maxSequence + 1 + i) % WINDOW_SIZE] = false;
                }
            }
            m_maxSequence = sequence;
            m_window[sequence % WINDOW_SIZE] = true;
            return true;
        }

        // Check if sequence is within the window
        if (m_maxSequence - sequence >= WINDOW_SIZE)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "Replay protection: sequence %llu too old (max=%llu)",
                           static_cast<unsigned long long>(sequence), static_cast<unsigned long long>(m_maxSequence));
            return false;
        }

        size_t idx = sequence % WINDOW_SIZE;
        if (m_window[idx])
        {
            SPARK_LOG_WARN(Spark::LogCategory::Network, "Replay protection: duplicate sequence %llu detected",
                           static_cast<unsigned long long>(sequence));
            return false;
        }

        m_window[idx] = true;
        return true;
    }

    void ReplayProtection::Reset()
    {
        m_maxSequence = 0;
        m_window.fill(false);
    }

} // namespace Spark::Net
