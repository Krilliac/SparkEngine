/**
 * @file TFScramWire.cpp
 * @brief TERRAFRONT SCRAM login/registration wire adapters (see TFScramWire.h)
 */
#include "Net/TFScramWire.h"

#include "Utils/SecureMemory.h"
#include "Utils/SecureRandom.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

namespace Terrafront
{
    namespace
    {
        constexpr size_t kClientNonceBytes = 24;

        /// Bounded C string from a fixed field; empty when the field is not NUL-terminated.
        template <size_t N> std::string TerminatedField(const char (&field)[N])
        {
            const size_t length = strnlen(field, N);
            if (length == N)
            {
                return {};
            }
            return std::string(field, length);
        }

        bool IsLowerHex(const std::string& text)
        {
            return !text.empty() && std::all_of(text.begin(), text.end(), [](char c)
                                                { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
        }

        template <size_t N> void CopyField(char (&field)[N], const std::string& text)
        {
            std::memset(field, 0, N);
            std::memcpy(field, text.data(), std::min(text.size(), N - 1));
        }
    } // namespace

    std::string TFWireUsername(const char (&field)[32])
    {
        return {field, strnlen(field, sizeof(field))};
    }

    // ============================================================================
    // Client
    // ============================================================================

    TFScramClient::~TFScramClient()
    {
        Clear();
    }

    void TFScramClient::Clear()
    {
        Spark::SecureClear(m_password);
        m_password.clear();
        m_user.clear();
        Spark::SecureErase(m_expectedSignature.data(), m_expectedSignature.size());
        m_awaitingReply = false;
    }

    TF_LoginStart TFScramClient::Start(const std::string& user, const std::string& password)
    {
        Clear();
        m_user = user;
        m_password = password;
        TF_LoginStart start{};
        CopyField(start.user, user);
        return start;
    }

    std::optional<TF_LoginProof> TFScramClient::Answer(const TF_LoginChallenge& challenge, TFAuthErr& outError)
    {
        outError = TFAuthErr::ServerError;
        if (m_password.empty())
        {
            return std::nullopt; // no login in flight (or a duplicate challenge)
        }
        std::string password = std::move(m_password);
        m_password.clear();
        const auto wipePassword = [&password] { Spark::SecureClear(password); };

        const std::string serverNonce = TerminatedField(challenge.serverNonce);
        if (challenge.saltLen < TFAccountSystem::kMinScramSaltBytes || challenge.saltLen > kTFScramSaltBytes ||
            challenge.iterations < kTFScramClientMinIterations ||
            challenge.iterations > TFAccountSystem::kMaxScramIterations || !IsLowerHex(serverNonce))
        {
            wipePassword();
            return std::nullopt;
        }
        const std::string clientNonce = Spark::SecureRandom::HexToken(kClientNonceBytes);
        if (clientNonce.empty())
        {
            wipePassword();
            return std::nullopt;
        }

        const std::vector<uint8_t> salt(challenge.salt, challenge.salt + challenge.saltLen);
        const std::string authMessage =
            TFAccountSystem::ScramAuthMessage(m_user, clientNonce, serverNonce, salt, challenge.iterations);
        TF_LoginProof proof{};
        {
            // SaltedPassword, ClientKey, StoredKey and ServerKey live only in this scope;
            // ScramKeys wipes them on destruction.
            const Crypto::ScramKeys keys = Crypto::DeriveScramKeys(password, salt, challenge.iterations);
            wipePassword();
            Crypto::Sha256Digest clientProof = Crypto::ScramClientProof(keys, authMessage);
            std::memcpy(proof.proof, clientProof.data(), sizeof(proof.proof));
            Spark::SecureErase(clientProof.data(), clientProof.size());
            m_expectedSignature =
                Crypto::HmacSha256(keys.serverKey.data(), keys.serverKey.size(),
                                   reinterpret_cast<const uint8_t*>(authMessage.data()), authMessage.size());
        }
        CopyField(proof.user, m_user);
        CopyField(proof.clientNonce, clientNonce);
        CopyField(proof.serverNonce, serverNonce);
        m_awaitingReply = true;
        outError = TFAuthErr::Ok;
        return proof;
    }

    bool TFScramClient::VerifyServer(const TF_AuthReply& reply) const
    {
        if (!m_awaitingReply || reply.ok == 0)
        {
            return false;
        }
        Crypto::Sha256Digest supplied{};
        std::memcpy(supplied.data(), reply.serverSignature, supplied.size());
        return Crypto::ConstantTimeEquals(supplied, m_expectedSignature);
    }

    std::optional<TF_RegisterRequest> MakeScramRegistration(const std::string& user, const std::string& password,
                                                            TFAuthErr& outError, TFAccountSystem::RandomFillFn fill)
    {
        outError = TFAuthErr::ServerError;
        TF_RegisterRequest request{};
        if (user.size() < 3)
        {
            outError = TFAuthErr::UsernameTooShort;
            return std::nullopt;
        }
        if (user.size() >= sizeof(request.user))
        {
            return std::nullopt;
        }
        if (password.size() < kTFMinPasswordLength)
        {
            outError = TFAuthErr::PasswordTooShort;
            return std::nullopt;
        }
        const std::vector<uint8_t> salt = Crypto::FromHex(TFAccountSystem::GenerateSalt(fill));
        if (salt.size() < TFAccountSystem::kMinScramSaltBytes || salt.size() > kTFScramSaltBytes)
        {
            return std::nullopt; // CSPRNG unavailable: never register under a missing salt
        }

        const uint32_t iterations = TFAccountSystem::kMinScramIterations;
        const Crypto::ScramKeys keys = Crypto::DeriveScramKeys(password, salt, iterations);
        CopyField(request.user, user);
        request.saltLen = static_cast<uint8_t>(salt.size());
        request.iterations = iterations;
        std::memcpy(request.salt, salt.data(), salt.size());
        std::memcpy(request.storedKey, keys.storedKey.data(), sizeof(request.storedKey));
        std::memcpy(request.serverKey, keys.serverKey.data(), sizeof(request.serverKey));
        outError = TFAuthErr::Ok;
        return request;
    }

    // ============================================================================
    // Server
    // ============================================================================

    bool MakeLoginChallenge(const TFScramChallenge& challenge, TF_LoginChallenge& out)
    {
        out = TF_LoginChallenge{};
        if (challenge.serverNonce.empty() || challenge.serverNonce.size() >= kTFScramNonceChars ||
            challenge.salt.empty() || challenge.salt.size() > kTFScramSaltBytes)
        {
            return false;
        }
        out.saltLen = static_cast<uint8_t>(challenge.salt.size());
        out.iterations = challenge.iterations;
        std::memcpy(out.salt, challenge.salt.data(), challenge.salt.size());
        CopyField(out.serverNonce, challenge.serverNonce);
        return true;
    }

    TFScramLoginResult CompleteLoginFromProof(TFAccountSystem& accounts, uint32_t clientId, const TF_LoginProof& proof)
    {
        const std::string user = TFWireUsername(proof.user);
        const std::string clientNonce = TerminatedField(proof.clientNonce);
        const std::string serverNonce = TerminatedField(proof.serverNonce);
        // CompleteLogin consumes the challenge and fails an empty or mismatched nonce.
        return accounts.CompleteLogin(clientId, user, clientNonce, serverNonce,
                                      std::span<const uint8_t>(proof.proof, sizeof(proof.proof)));
    }

    TFAuthResult RegisterFromRequest(TFAccountSystem& accounts, const TF_RegisterRequest& request)
    {
        if (request.saltLen > kTFScramSaltBytes)
        {
            TFAuthResult weak;
            weak.err = TFAuthErr::WeakVerifier;
            return weak;
        }
        const std::vector<uint8_t> salt(request.salt, request.salt + request.saltLen);
        Crypto::Sha256Digest storedKey{};
        Crypto::Sha256Digest serverKey{};
        std::memcpy(storedKey.data(), request.storedKey, storedKey.size());
        std::memcpy(serverKey.data(), request.serverKey, serverKey.size());
        TFAuthResult result =
            accounts.RegisterVerifier(TFWireUsername(request.user), salt, request.iterations, storedKey, serverKey);
        Spark::SecureErase(storedKey.data(), storedKey.size());
        Spark::SecureErase(serverKey.data(), serverKey.size());
        return result;
    }
} // namespace Terrafront
