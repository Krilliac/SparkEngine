/**
 * @file NetworkSecurity.h
 * @brief Legacy XOR obfuscation and token-lifecycle prototypes
 * @author Spark Engine Team
 * @date 2025
 *
 * The encryption-named API is retained for compatibility, but it only applies
 * repeating-key XOR obfuscation and provides no confidentiality or integrity.
 * The randomized token set is not wired into NetworkManager admission and does
 * not authenticate a peer. Neither prototype may protect credentials or a
 * remotely exposed production game.
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

    /// Size of the legacy XOR key material in bytes.
    static constexpr size_t SECURITY_KEY_SIZE = 32;

    /// Size of connection tokens in bytes.
    static constexpr size_t CONNECTION_TOKEN_SIZE = 16;

    /// How long (in seconds) a connection token stays valid.
    static constexpr float CONNECTION_TOKEN_LIFETIME = 30.0f;

    /// @brief Isolated obfuscation and token-lifecycle prototypes.
    ///
    /// Legacy method names say encrypt/decrypt, but the byte transform is XOR
    /// and is not encryption. Tokens only prove equality with an in-process
    /// pending value and do not authenticate the active UDP connection.
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
        // Legacy XOR transformation (API names retained for compatibility)
        // -----------------------------------------------------------------

        /// @brief Apply repeating-key XOR obfuscation in-place; not encryption.
        /// @param data  Pointer to the buffer to transform.
        /// @param size  Number of bytes.
        /// @param key   Prototype XOR key material.
        static void PacketEncrypt(uint8_t* data, size_t size, const Key& key)
        {
            if (data == nullptr || size == 0)
                return;
            for (size_t i = 0; i < size; ++i)
            {
                data[i] ^= key[i % SECURITY_KEY_SIZE];
            }
        }

        /// @brief Reverse the XOR transform; not authenticated decryption.
        /// @param data  Pointer to the buffer to transform.
        /// @param size  Number of bytes.
        /// @param key   Prototype XOR key material.
        static void PacketDecrypt(uint8_t* data, size_t size, const Key& key)
        {
            // XOR is symmetric -- reversing the transform is the same operation.
            PacketEncrypt(data, size, key);
        }

        /// @brief Return an XOR-obfuscated copy; legacy encryption-named API.
        static std::vector<uint8_t> Encrypt(const std::vector<uint8_t>& plaintext, const Key& key)
        {
            std::vector<uint8_t> ciphertext = plaintext;
            PacketEncrypt(ciphertext.data(), ciphertext.size(), key);
            return ciphertext;
        }

        /// @brief Reverse XOR obfuscation; legacy decryption-named API.
        static std::vector<uint8_t> Decrypt(const std::vector<uint8_t>& ciphertext, const Key& key)
        {
            std::vector<uint8_t> plaintext = ciphertext;
            PacketDecrypt(plaintext.data(), plaintext.size(), key);
            return plaintext;
        }

        // -----------------------------------------------------------------
        // Prototype token lifecycle (not connection authentication)
        // -----------------------------------------------------------------

        /// @brief Generate randomized prototype bytes and store them with a timestamp.
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

        /// @brief Match and consume token bytes from this instance's pending set.
        /// @param token  The token to validate.
        /// @return true if the bytes are present and have not expired.
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

        /// @brief Get the current prototype XOR key material.
        const Key& GetEncryptionKey() const { return m_encryptionKey; }

        /// @brief Set the prototype XOR key material.
        void SetEncryptionKey(const Key& key) { m_encryptionKey = key; }

        /// @brief Generate non-cryptographic prototype key material.
        static void GenerateKey(Key& outKey) { FillRandom(outKey.data(), SECURITY_KEY_SIZE); }

        /// @brief Read the legacy toggle for prototype XOR obfuscation.
        bool IsEncryptionEnabled() const { return m_obfuscationEnabled; }

        /// @brief Enable or disable prototype XOR obfuscation in legacy callers.
        void SetEncryptionEnabled(bool enabled) { m_obfuscationEnabled = enabled; }

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
        bool m_obfuscationEnabled = false;
        std::unordered_map<std::string, TokenEntry> m_pendingTokens;
    };

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
