/**
 * @file TFScramWire.h
 * @brief NET-100: TERRAFRONT SCRAM-SHA-256 login and registration on the wire
 *
 * Builds and checks the onboarding structs in Net/TFNetProtocolOnboarding.h so
 * that no message ever carries a password, SaltedPassword or ClientKey:
 *  - TFScramClient is the client half of one login. It keeps the password only
 *    between Start() and the server's challenge, derives the proof, wipes the
 *    password and every derived key, and later verifies the server's signature
 *    (mutual authentication: a server without the account's ServerKey fails).
 *  - MakeScramRegistration derives a verifier (StoredKey, ServerKey) from a fresh
 *    client-generated salt, so registration never sends the password either.
 *  - The server functions adapt TFAccountSystem's BeginLogin / CompleteLogin /
 *    RegisterVerifier to the wire structs, with every field bounded.
 *
 * Both TFClientNet/TFServerSim (module) and SparkTests use this unit, so the wire
 * behaviour tested is the wire behaviour shipped.
 *
 * Thread affinity: game thread. Ownership: TFScramClient owns its secrets and
 * wipes them (Clear, destructor). Allocation: per login attempt, never per frame.
 */
#pragma once

#include "Account/TFAccountSystem.h"
#include "Account/TFCrypto.h"
#include "Net/TFNetProtocol.h"

#include <cstdint>
#include <optional>
#include <string>

namespace Terrafront
{
    /// Lowest PBKDF2 cost a client accepts in a challenge (legacy rows were stored at 100000).
    inline constexpr uint32_t kTFScramClientMinIterations = 100000;
    /// Shortest password a client registers (the account policy TFAccountSystem::Register enforced).
    inline constexpr size_t kTFMinPasswordLength = 8;

    /** @brief The client half of one SCRAM login. */
    class TFScramClient
    {
      public:
        TFScramClient() = default;
        ~TFScramClient();
        TFScramClient(const TFScramClient&) = delete;
        TFScramClient& operator=(const TFScramClient&) = delete;

        /// Remember the credentials (replacing any login in flight) and build the LoginStart.
        [[nodiscard]] TF_LoginStart Start(const std::string& user, const std::string& password);

        /**
         * @brief Answer the server's challenge
         *
         * Refuses a challenge outside the client policy (salt 16..64 bytes, iterations
         * in [kTFScramClientMinIterations, TFAccountSystem::kMaxScramIterations], a
         * well-formed nonce) before running PBKDF2, so a hostile server cannot stall
         * the client. The password is wiped whatever the outcome.
         *
         * @param challenge Server challenge
         * @param outError  ServerError when the challenge was refused or no login is in flight
         * @return The proof to send, or nullopt
         */
        [[nodiscard]] std::optional<TF_LoginProof> Answer(const TF_LoginChallenge& challenge, TFAuthErr& outError);

        /// True when @p reply is a success whose ServerSignature proves the server holds this account's ServerKey.
        [[nodiscard]] bool VerifyServer(const TF_AuthReply& reply) const;

        [[nodiscard]] bool AwaitingChallenge() const { return !m_password.empty(); }
        [[nodiscard]] bool AwaitingReply() const { return m_awaitingReply; }

        /// Forget the login in flight and wipe every secret.
        void Clear();

      private:
        std::string m_user;
        std::string m_password;
        Crypto::Sha256Digest m_expectedSignature{};
        bool m_awaitingReply = false;
    };

    /**
     * @brief Derive a registration verifier on the client
     * @param user      Username (3..31 bytes)
     * @param password  Password (at least kTFMinPasswordLength bytes); never leaves this function
     * @param outError  Why the registration was refused locally
     * @param fill      Salt random source; nullptr means the OS CSPRNG
     * @return The request to send, or nullopt
     */
    [[nodiscard]] std::optional<TF_RegisterRequest> MakeScramRegistration(const std::string& user,
                                                                          const std::string& password,
                                                                          TFAuthErr& outError,
                                                                          TFAccountSystem::RandomFillFn fill = nullptr);

    /// Server: the wire challenge for BeginLogin's result; false when that login cannot complete.
    [[nodiscard]] bool MakeLoginChallenge(const TFScramChallenge& challenge, TF_LoginChallenge& out);

    /// Server: check a proof against @p clientId's outstanding challenge (consumed either way).
    [[nodiscard]] TFScramLoginResult CompleteLoginFromProof(TFAccountSystem& accounts, uint32_t clientId,
                                                            const TF_LoginProof& proof);

    /// Server: store the verifier a client derived (WeakVerifier for an out-of-policy salt or cost).
    [[nodiscard]] TFAuthResult RegisterFromRequest(TFAccountSystem& accounts, const TF_RegisterRequest& request);

    /// Username carried in a fixed wire field (bounded, NUL or field end terminates it).
    [[nodiscard]] std::string TFWireUsername(const char (&field)[32]);
} // namespace Terrafront
