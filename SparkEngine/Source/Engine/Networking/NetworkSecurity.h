/**
 * @file NetworkSecurity.h
 * @brief Single-use, expiring connection-token registry
 * @author Spark Engine Team
 * @date 2025
 *
 * Issues CSPRNG connection tokens through NetworkEncryption's
 * GenerateConnectionToken() and matches them with its constant-time
 * ValidateToken(). Both fail closed: a CSPRNG failure issues no token, and an
 * unknown, reused, or expired token is rejected.
 *
 * This registry only proves that a peer echoed a token this process issued. It
 * does not encrypt traffic or authenticate the UDP connection that carries it.
 * The former repeating-key obfuscation "encryption" prototype was removed (NET-100);
 * packet confidentiality and integrity belong to SecureChannel.
 *
 * All networking code is guarded by ENABLE_NETWORKING.
 */

#pragma once

#ifdef ENABLE_NETWORKING

#include "NetworkEncryption.h"

#include <chrono>
#include <vector>

namespace Spark::Net
{

    /// How long (in seconds) a connection token stays valid.
    static constexpr float CONNECTION_TOKEN_LIFETIME = 30.0f;

    /// @brief Registry of pending single-use connection tokens.
    ///
    /// Not thread-safe: call from the thread that owns the connection handshake.
    class NetworkSecurity
    {
      public:
        using Token = ConnectionToken;

        NetworkSecurity() = default;
        ~NetworkSecurity() = default;

        // Non-copyable
        NetworkSecurity(const NetworkSecurity&) = delete;
        NetworkSecurity& operator=(const NetworkSecurity&) = delete;

        /// @brief Issue a fresh CSPRNG token and record it as pending.
        /// @param outToken Receives the token; zeroed when issuance fails.
        /// @return false if the OS CSPRNG failed. Nothing is recorded in that case,
        ///         so no predictable token can ever validate.
        [[nodiscard]] bool GenerateConnectionToken(Token& outToken)
        {
            PruneExpiredTokens();

            if (!Spark::Net::GenerateConnectionToken(outToken))
                return false;

            m_pendingTokens.push_back({outToken, std::chrono::steady_clock::now()});
            return true;
        }

        /// @brief Match and consume a pending token.
        ///
        /// Every pending entry is compared in constant time so the match position
        /// does not leak through timing.
        /// @param token The token the peer presented.
        /// @return true only for a pending, unexpired token issued by this
        ///         registry. The token is consumed on success (single use).
        [[nodiscard]] bool ValidateConnectionToken(const Token& token)
        {
            PruneExpiredTokens();

            auto match = m_pendingTokens.end();
            for (auto it = m_pendingTokens.begin(); it != m_pendingTokens.end(); ++it)
            {
                if (ValidateToken(it->token, token) && match == m_pendingTokens.end())
                    match = it;
            }

            if (match == m_pendingTokens.end())
                return false;

            m_pendingTokens.erase(match);
            return true;
        }

      private:
        struct TokenEntry
        {
            Token token{};
            std::chrono::steady_clock::time_point creationTime;
        };

        void PruneExpiredTokens()
        {
            const auto now = std::chrono::steady_clock::now();
            std::erase_if(m_pendingTokens,
                          [now](const TokenEntry& entry)
                          {
                              const float elapsed = std::chrono::duration<float>(now - entry.creationTime).count();
                              return elapsed > CONNECTION_TOKEN_LIFETIME;
                          });
        }

        std::vector<TokenEntry> m_pendingTokens;
    };

} // namespace Spark::Net

#endif // ENABLE_NETWORKING
