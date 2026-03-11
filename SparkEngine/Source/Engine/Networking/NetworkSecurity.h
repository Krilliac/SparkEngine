/**
 * @file NetworkSecurity.h
 * @brief Network security layer -- packet encryption and connection token auth
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a lightweight security layer for the networking subsystem:
 * - XOR-based packet encryption/decryption (placeholder for a future
 *   DTLS or AES-GCM implementation).
 * - Connection token generation and validation so that only clients
 *   holding a valid token can join the server.
 *
 * All networking code is guarded by ENABLE_NETWORKING.
 */

#pragma once
#include "../../Core/Platform.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef ENABLE_NETWORKING

namespace Spark::Net
{

    /// Size of encryption keys in bytes.
    static constexpr size_t SECURITY_KEY_SIZE = 32;

    /// Size of connection tokens in bytes.
    static constexpr size_t CONNECTION_TOKEN_SIZE = 16;

    /// How long (in seconds) a connection token stays valid.
    static constexpr float CONNECTION_TOKEN_LIFETIME = 30.0f;

    /// @brief Lightweight network security utilities.
    ///
    /// Encrypts / decrypts packets with XOR (a simple starting point --
    /// production games should replace this with DTLS or AES-GCM).
    /// Also manages short-lived connection tokens for server authentication.
    class NetworkSecurity
    {
      public:
        using Key = std::array<uint8_t, SECURITY_KEY_SIZE>;
        using Token = std::array<uint8_t, CONNECTION_TOKEN_SIZE>;

        NetworkSecurity() { GenerateKey(m_encryptionKey); }

        ~NetworkSecurity() = default;

        // Non-copyable
        NetworkSecurity(const NetworkSecurity&) = delete;
        NetworkSecurity& operator=(const NetworkSecurity&) = delete;

        // -----------------------------------------------------------------
        // Encryption / Decryption
        // -----------------------------------------------------------------

        /// @brief Encrypt data in-place using XOR with the current key.
        /// @param data  Pointer to the buffer to encrypt.
        /// @param size  Number of bytes.
        /// @param key   Encryption key.
        static void PacketEncrypt(uint8_t* data, size_t size, const Key& key)
        {
            if (data == nullptr || size == 0)
                return;
            for (size_t i = 0; i < size; ++i)
            {
                data[i] ^= key[i % SECURITY_KEY_SIZE];
            }
        }

        /// @brief Decrypt data in-place using XOR with the current key.
        /// @param data  Pointer to the buffer to decrypt.
        /// @param size  Number of bytes.
        /// @param key   Decryption key (same as encryption key for XOR).
        static void PacketDecrypt(uint8_t* data, size_t size, const Key& key)
        {
            // XOR encryption is symmetric -- decrypt is the same operation.
            PacketEncrypt(data, size, key);
        }

        /// @brief Encrypt a byte vector and return the result.
        static std::vector<uint8_t> Encrypt(const std::vector<uint8_t>& plaintext, const Key& key)
        {
            std::vector<uint8_t> ciphertext = plaintext;
            PacketEncrypt(ciphertext.data(), ciphertext.size(), key);
            return ciphertext;
        }

        /// @brief Decrypt a byte vector and return the result.
        static std::vector<uint8_t> Decrypt(const std::vector<uint8_t>& ciphertext, const Key& key)
        {
            std::vector<uint8_t> plaintext = ciphertext;
            PacketDecrypt(plaintext.data(), plaintext.size(), key);
            return plaintext;
        }

        // -----------------------------------------------------------------
        // Connection Token Authentication
        // -----------------------------------------------------------------

        /// @brief Generate a new connection token and store it with a timestamp.
        /// @return The generated token.
        Token GenerateConnectionToken()
        {
            Token token{};
            FillRandom(token.data(), CONNECTION_TOKEN_SIZE);

            TokenEntry entry;
            entry.token = token;
            entry.creationTime = std::chrono::steady_clock::now();

            m_pendingTokens[TokenToString(token)] = entry;

            // Prune expired tokens while we are here
            PruneExpiredTokens();

            return token;
        }

        /// @brief Validate a connection token against the pending set.
        /// @param token  The token to validate.
        /// @return true if the token is valid and has not expired.
        ///         The token is consumed (removed) on success.
        bool ValidateConnectionToken(const Token& token)
        {
            PruneExpiredTokens();

            std::string key = TokenToString(token);
            auto it = m_pendingTokens.find(key);
            if (it == m_pendingTokens.end())
                return false;

            // Check expiry
            auto now = std::chrono::steady_clock::now();
            float elapsed = std::chrono::duration<float>(now - it->second.creationTime).count();
            if (elapsed > CONNECTION_TOKEN_LIFETIME)
            {
                m_pendingTokens.erase(it);
                return false;
            }

            // Consume the token (single-use)
            m_pendingTokens.erase(it);
            return true;
        }

        // -----------------------------------------------------------------
        // Key management helpers
        // -----------------------------------------------------------------

        /// @brief Get the current encryption key.
        const Key& GetEncryptionKey() const { return m_encryptionKey; }

        /// @brief Set the encryption key (e.g. received from server during handshake).
        void SetEncryptionKey(const Key& key) { m_encryptionKey = key; }

        /// @brief Generate a cryptographically-random key.
        static void GenerateKey(Key& outKey) { FillRandom(outKey.data(), SECURITY_KEY_SIZE); }

        /// @brief Check whether encryption is enabled.
        bool IsEncryptionEnabled() const { return m_encryptionEnabled; }

        /// @brief Enable or disable encryption on the send/receive path.
        void SetEncryptionEnabled(bool enabled) { m_encryptionEnabled = enabled; }

      private:
        // -----------------------------------------------------------------
        // Internal helpers
        // -----------------------------------------------------------------

        struct TokenEntry
        {
            Token token{};
            std::chrono::steady_clock::time_point creationTime;
        };

        static void FillRandom(uint8_t* buffer, size_t size)
        {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<uint16_t> dist(0, 255);
            for (size_t i = 0; i < size; ++i)
            {
                buffer[i] = static_cast<uint8_t>(dist(gen));
            }
        }

        static std::string TokenToString(const Token& token)
        {
            std::string result;
            result.reserve(CONNECTION_TOKEN_SIZE * 2);
            for (uint8_t byte : token)
            {
                static constexpr char HEX[] = "0123456789abcdef";
                result.push_back(HEX[(byte >> 4) & 0x0F]);
                result.push_back(HEX[byte & 0x0F]);
            }
            return result;
        }

        void PruneExpiredTokens()
        {
            auto now = std::chrono::steady_clock::now();
            for (auto it = m_pendingTokens.begin(); it != m_pendingTokens.end();)
            {
                float elapsed = std::chrono::duration<float>(now - it->second.creationTime).count();
                if (elapsed > CONNECTION_TOKEN_LIFETIME)
                {
                    it = m_pendingTokens.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        Key m_encryptionKey{};
        bool m_encryptionEnabled = false;
        std::unordered_map<std::string, TokenEntry> m_pendingTokens;
    };

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
