/**
 * @file TFAccountSystem.h
 * @brief TERRAFRONT account register/login core logic (W5 onboarding, Task 2; NET-100 SCRAM verifiers).
 *
 * Core logic only: operates directly on a `TFDatabase*` (no TFGameContext),
 * so it is unit-testable standalone. The client-id -> account-id session map
 * is plain in-memory bookkeeping consumed by the session layer (Task 4).
 *
 * Login is SCRAM-SHA-256 (RFC 5802 / RFC 7677): BeginLogin hands out the salt,
 * iteration count and a single-use server nonce; CompleteLogin checks a proof
 * bound to both nonces. The account row stores only StoredKey and ServerKey,
 * so neither the row nor a captured login lets anyone log in again.
 *
 * Thread affinity: game thread (one TFAccountSystem per server authority).
 * Ownership: borrows the TFDatabase; owns the pending-challenge map.
 * Allocation: per login attempt only; never per frame.
 */
#pragma once

#include "Account/TFCrypto.h"
#include "Persistence/TFDatabase.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Terrafront
{

    enum class TFAuthErr : uint8_t
    {
        Ok = 0,
        BadCredentials,
        UsernameTaken,
        UsernameTooShort,
        PasswordTooShort,
        ServerError,
        NotLoggedIn,
        SessionActive,
        RemoteOnboardingDisabled,
        AccountInUse, // credentials valid, but another connection holds the account (wire value 9)
        WeakVerifier  ///< RegisterVerifier: salt or iteration count outside the kMin/kMaxScram* policy (wire value 10)
    };

    struct TFAuthResult
    {
        bool ok = false;
        TFAuthErr err = TFAuthErr::ServerError;
        uint64_t accountId = 0;
    };

    /// SCRAM server-first-message fields returned by BeginLogin.
    struct TFScramChallenge
    {
        std::vector<uint8_t> salt;
        uint32_t iterations = 0;
        std::string serverNonce; ///< empty when the CSPRNG failed; the login cannot complete
    };

    struct TFScramLoginResult
    {
        TFAuthResult auth;
        Crypto::Sha256Digest serverSignature{}; ///< HMAC(ServerKey, AuthMessage) when auth.ok; proves the server
    };

    class TFAccountSystem
    {
      public:
        void SetDatabase(TFDatabase* db) { m_db = db; } // core logic uses the db directly (unit-testable)

        /**
         * @name SCRAM verifier policy
         * Enforced when a verifier is registered and again whenever a stored row is
         * loaded, because a row's iteration count is paid by every login attempt
         * for that name and is handed to the client in the challenge. A client
         * must refuse a challenge outside [kMinScramIterations, kMaxScramIterations]
         * before running PBKDF2, so a hostile server cannot stall it either.
         * @{
         */
        static constexpr uint32_t kMinScramIterations = 150000; ///< intake floor (stored legacy rows: 100000)
        static constexpr uint32_t kMaxScramIterations = 600000; ///< 4x the current cost; same bound as legacy rows
        static constexpr size_t kMinScramSaltBytes = 16;
        static constexpr size_t kMaxScramSaltBytes = 64;
        /// @}

        /// Store a SCRAM verifier the client derived itself (the password never reaches the server).
        /// WeakVerifier when the salt or iteration count is outside the verifier policy above.
        TFAuthResult RegisterVerifier(const std::string& username, const std::vector<uint8_t>& salt,
                                      uint32_t iterations, const Crypto::Sha256Digest& storedKey,
                                      const Crypto::Sha256Digest& serverKey);

        /**
         * @brief Start a login: salt, iterations and a fresh single-use server nonce.
         *
         * Unknown users get a salt that is stable per username and shaped like a real
         * one, so the reply does not reveal whether the account exists. A legacy
         * pbkdf2-sha256 row is rewritten as a scram-sha256 row here.
         */
        TFScramChallenge BeginLogin(const std::string& username);

        /// Verify a client proof against the outstanding challenge, which is consumed either way.
        TFScramLoginResult CompleteLogin(const std::string& username, const std::string& clientNonce,
                                         const std::string& serverNonce, std::span<const uint8_t> clientProof);

        /// RFC 5802 AuthMessage (no channel binding) shared by both sides of the exchange.
        static std::string ScramAuthMessage(const std::string& username, const std::string& clientNonce,
                                            const std::string& serverNonce, const std::vector<uint8_t>& salt,
                                            uint32_t iterations);

        /// Local wrappers for tests and offline tools: they derive the verifier or proof in-process.
        TFAuthResult Register(const std::string& username, const std::string& password); // min length 3 / 8
        TFAuthResult Login(const std::string& username, const std::string& password);

        /**
         * @brief Bind a connection to an account (session layer, Task 4).
         * @return false when accountId is 0 or the account is already bound to a
         *         different connection (one live session per account); the
         *         existing binding is left untouched.
         */
        bool BindSession(uint32_t clientId, uint64_t accountId);
        uint64_t AccountForClient(uint32_t clientId) const; // 0 if not logged in
        void ClearSession(uint32_t clientId);

        /// Random-byte source with the Spark::SecureRandom::Fill signature.
        using RandomFillFn = bool (*)(void* buffer, size_t size) noexcept;

        /**
         * @brief Override the salt and nonce random source (nullptr restores the OS CSPRNG).
         *
         * Exists so tests can exercise the fail-closed paths; production code never calls it.
         */
        void SetRandomSource(RandomFillFn fill) { m_randomFill = fill; }

        /**
         * @brief Generate a 16-byte hex-encoded salt from the OS CSPRNG.
         * @param fill Random source; nullptr means Spark::SecureRandom::Fill.
         * @return The hex salt, or an empty string when the random source fails.
         */
        static std::string GenerateSalt(RandomFillFn fill = nullptr);
        /// Self-describing "scram-sha256$iters$saltHex$storedKeyHex$serverKeyHex" row for (password, salt).
        static std::string HashPassword(const std::string& password, const std::string& salt);
        /// Constant-time; accepts scram-sha256 and legacy pbkdf2-sha256 rows, false on anything else and,
        /// before any derivation, on stored parameters outside policy (iterations not a plain decimal in
        /// [100000, 600000]; a legacy row's salt not 16 bytes or derived key not 32 bytes).
        static bool VerifyPassword(const std::string& password, const std::string& storedHash);

      private:
        std::string RandomHex(size_t bytes) const; // empty on CSPRNG failure

        TFDatabase* m_db = nullptr;
        RandomFillFn m_randomFill = nullptr;                          // nullptr -> Spark::SecureRandom::Fill
        std::unordered_map<uint32_t, uint64_t> m_sessions;            // clientId -> accountId
        std::unordered_map<uint64_t, uint32_t> m_accountOwners;       // accountId -> clientId (exclusive)
        std::unordered_map<std::string, std::string> m_pendingLogins; // username -> outstanding server nonce
        std::string m_unknownUserKey;                                 // per-process key for unknown-user pseudo salts
    };

} // namespace Terrafront
