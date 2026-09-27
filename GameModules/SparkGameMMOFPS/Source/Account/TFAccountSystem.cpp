/**
 * @file TFAccountSystem.cpp
 * @brief TERRAFRONT account register/login core logic (W5 onboarding, Task 2; NET-100 SCRAM verifiers).
 *
 * Kept minimal-dependency (TFDatabase.h + stdlib) so it links standalone
 * into SparkTests without pulling in TFGameContext or the engine module
 * scaffolding.
 */
#include "Account/TFAccountSystem.h"
#include "Utils/ScopeGuard.h"
#include "Utils/SecureMemory.h"
#include "Utils/SecureRandom.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <sstream>
#include <system_error>
#include <vector>

namespace Terrafront
{

    // === Stored credential formats ===
    //
    // Current: "scram-sha256$<iterations>$<saltHex>$<storedKeyHex>$<serverKeyHex>"
    // (RFC 5802 / RFC 7677). StoredKey = SHA-256(ClientKey) and ServerKey are
    // what a SCRAM server keeps: neither is enough to log in, so a leaked row is
    // not password-equivalent, and login verifies a nonce-bound ClientProof
    // instead of receiving the password (NET-100).
    //
    // Legacy: "pbkdf2-sha256$<iterations>$<saltHex>$<dkHex>" (2026-07-06 cutover
    // from iterated std::hash). dk is SCRAM's SaltedPassword, which is
    // password-equivalent, so BeginLogin rewrites such a row as a scram-sha256
    // row (same salt and iterations) and the dk is dropped.
    //
    // Parameters travel with the row, so raising kPbkdf2Iterations never
    // invalidates rows minted under the old count.

    namespace
    {
        constexpr uint32_t kPbkdf2Iterations = 150000; // >= 100000 floor with headroom
        constexpr size_t kSaltBytes = 16;              // 128-bit salt
        constexpr size_t kDkBytes = 32;                // 256-bit derived key / SaltedPassword
        // Accepted range for a STORED row's iteration count (scram-sha256 and
        // legacy pbkdf2-sha256 alike): the hardening floor up to headroom for a
        // future cost bump, never an unbounded (up to 2^32 rounds per login
        // attempt) database-controlled value.
        constexpr uint32_t kMinVerifyIterations = 100000;
        constexpr uint32_t kMaxVerifyIterations = TFAccountSystem::kMaxScramIterations;
        static_assert(kMaxVerifyIterations == kPbkdf2Iterations * 4, "headroom for one future cost bump, no more");
        static_assert(kSaltBytes == TFAccountSystem::kMinScramSaltBytes);
        constexpr size_t kNonceBytes = 24; // server and wrapper client nonces, hex-encoded
        constexpr size_t kMaxNonceChars = 128;
        constexpr const char* kLegacyScheme = "pbkdf2-sha256";
        constexpr const char* kScramScheme = "scram-sha256";

        // Splits on '$' with no regex dependency; e.g. "a$b$c" -> {"a","b","c"}.
        std::vector<std::string> SplitScheme(const std::string& s)
        {
            std::vector<std::string> parts;
            size_t start = 0;
            while (true)
            {
                size_t pos = s.find('$', start);
                parts.push_back(s.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
                if (pos == std::string::npos)
                {
                    break;
                }
                start = pos + 1;
            }
            return parts;
        }

        /// A stored row's cost parameter is untrusted (the row comes from the database
        /// file, and every login attempt for the username pays it): accept only a fully
        /// consumed decimal inside [kMinVerifyIterations, kMaxVerifyIterations].
        bool ParseIterations(const std::string& text, uint32_t& out)
        {
            const char* const textEnd = text.data() + text.size();
            const auto [parsedEnd, parseErr] = std::from_chars(text.data(), textEnd, out);
            if (text.empty() || parseErr != std::errc{} || parsedEnd != textEnd)
            {
                return false;
            }
            return out >= kMinVerifyIterations && out <= kMaxVerifyIterations;
        }

        bool ToDigest(const std::vector<uint8_t>& bytes, Crypto::Sha256Digest& out)
        {
            if (bytes.size() != out.size())
            {
                return false;
            }
            std::copy(bytes.begin(), bytes.end(), out.begin());
            return true;
        }

        /// The server-side half of a SCRAM credential, parsed from a stored row.
        struct ScramVerifier
        {
            uint32_t iterations = 0;
            std::vector<uint8_t> salt;
            Crypto::Sha256Digest storedKey{};
            Crypto::Sha256Digest serverKey{};

            ~ScramVerifier()
            {
                Spark::SecureErase(storedKey.data(), storedKey.size());
                Spark::SecureErase(serverKey.data(), serverKey.size());
            }
        };

        bool ParseScramRow(const std::string& row, ScramVerifier& out)
        {
            const std::vector<std::string> parts = SplitScheme(row);
            if (parts.size() != 5 || parts[0] != kScramScheme || !ParseIterations(parts[1], out.iterations))
            {
                return false;
            }
            // Salt length is bounded like the iteration count: the row is untrusted,
            // and the salt is hashed into the first PBKDF2 block of every attempt.
            const size_t saltHexChars = parts[2].size();
            if (saltHexChars < TFAccountSystem::kMinScramSaltBytes * 2 ||
                saltHexChars > TFAccountSystem::kMaxScramSaltBytes * 2)
            {
                return false;
            }
            out.salt = Crypto::FromHex(parts[2]);
            return out.salt.size() * 2 == saltHexChars && ToDigest(Crypto::FromHex(parts[3]), out.storedKey) &&
                   ToDigest(Crypto::FromHex(parts[4]), out.serverKey);
        }

        std::string FormatScramRow(uint32_t iterations, const std::vector<uint8_t>& salt,
                                   const Crypto::Sha256Digest& storedKey, const Crypto::Sha256Digest& serverKey)
        {
            std::ostringstream ss;
            ss << kScramScheme << '$' << iterations << '$' << Crypto::ToHex(salt) << '$'
               << Crypto::ToHex(storedKey.data(), storedKey.size()) << '$'
               << Crypto::ToHex(serverKey.data(), serverKey.size());
            return ss.str();
        }

        /// pbkdf2-sha256 row -> scram-sha256 row with the same salt and iterations; "" if not a valid legacy row.
        std::string MigrateLegacyRow(const std::string& row)
        {
            const std::vector<std::string> parts = SplitScheme(row);
            uint32_t iterations = 0;
            if (parts.size() != 4 || parts[0] != kLegacyScheme || !ParseIterations(parts[1], iterations))
            {
                return {};
            }
            // Exactly the salt/derived-key sizes the legacy HashPassword wrote.
            if (parts[2].size() != kSaltBytes * 2 || parts[3].size() != kDkBytes * 2)
            {
                return {};
            }
            const std::vector<uint8_t> salt = Crypto::FromHex(parts[2]);
            std::vector<uint8_t> saltedPassword = Crypto::FromHex(parts[3]);
            const auto clearSaltedPassword = Spark::MakeScopeExit([&] { Spark::SecureClear(saltedPassword); });
            if (salt.size() != kSaltBytes || saltedPassword.size() != kDkBytes)
            {
                return {};
            }
            const Crypto::ScramKeys keys = Crypto::DeriveScramKeysFromSaltedPassword(saltedPassword);
            return FormatScramRow(iterations, salt, keys.storedKey, keys.serverKey);
        }

        /// RFC 5802 saslname: '=' and ',' are escaped.
        std::string EscapeSaslName(const std::string& username)
        {
            std::string out;
            for (const char c : username)
            {
                if (c == '=')
                {
                    out += "=3D";
                }
                else if (c == ',')
                {
                    out += "=2C";
                }
                else
                {
                    out += c;
                }
            }
            return out;
        }

        /// RFC 5802 nonce: printable ASCII except ','.
        bool IsValidNonce(const std::string& nonce)
        {
            if (nonce.empty() || nonce.size() > kMaxNonceChars)
            {
                return false;
            }
            return std::all_of(nonce.begin(), nonce.end(), [](char c) { return c >= 0x21 && c <= 0x7E && c != ','; });
        }
    } // namespace

    // === Salts, nonces and stored rows ===

    std::string TFAccountSystem::GenerateSalt(RandomFillFn fill)
    {
        // Salts come from the OS CSPRNG (Spark::SecureRandom), never a seeded
        // PRNG: a Mersenne Twister state is recoverable from observed output,
        // which would make future salts predictable. A failed fill yields an
        // empty string so callers fail closed instead of storing a weak salt.
        if (!fill)
        {
            fill = &Spark::SecureRandom::Fill;
        }
        std::vector<uint8_t> saltBytes(kSaltBytes);
        if (!fill(saltBytes.data(), saltBytes.size()))
        {
            return {};
        }
        return Crypto::ToHex(saltBytes);
    }

    std::string TFAccountSystem::RandomHex(size_t bytes) const
    {
        const RandomFillFn fill = m_randomFill ? m_randomFill : &Spark::SecureRandom::Fill;
        std::vector<uint8_t> random(bytes);
        if (!fill(random.data(), random.size()))
        {
            return {};
        }
        return Crypto::ToHex(random);
    }

    std::string TFAccountSystem::HashPassword(const std::string& password, const std::string& salt)
    {
        std::vector<uint8_t> saltBytes = Crypto::FromHex(salt);
        if (saltBytes.empty() && !salt.empty())
        {
            // `salt` wasn't valid hex (e.g. a caller/test passed an arbitrary
            // string rather than a GenerateSalt() output) -- fall back to the
            // raw bytes of the string so HashPassword stays a total, deterministic
            // function of (password, salt) for any input.
            saltBytes.assign(salt.begin(), salt.end());
        }
        const Crypto::ScramKeys keys = Crypto::DeriveScramKeys(password, saltBytes, kPbkdf2Iterations);
        return FormatScramRow(kPbkdf2Iterations, saltBytes, keys.storedKey, keys.serverKey);
    }

    bool TFAccountSystem::VerifyPassword(const std::string& password, const std::string& storedHash)
    {
        // Both row formats are bounded before any derivation (ParseIterations,
        // MigrateLegacyRow sizes), so a corrupt or tampered row is rejected as bad
        // credentials instead of costing an unbounded PBKDF2 run.
        const std::string migrated = MigrateLegacyRow(storedHash);
        ScramVerifier verifier;
        if (!ParseScramRow(migrated.empty() ? storedHash : migrated, verifier))
        {
            return false; // legacy std::hash or unrecognized format -> caller treats as bad credentials
        }
        const Crypto::ScramKeys keys = Crypto::DeriveScramKeys(password, verifier.salt, verifier.iterations);
        return Crypto::ConstantTimeEquals(keys.storedKey, verifier.storedKey) &&
               Crypto::ConstantTimeEquals(keys.serverKey, verifier.serverKey);
    }

    std::string TFAccountSystem::ScramAuthMessage(const std::string& username, const std::string& clientNonce,
                                                  const std::string& serverNonce, const std::vector<uint8_t>& salt,
                                                  uint32_t iterations)
    {
        // client-first-message-bare "," server-first-message "," client-final-message-without-proof,
        // with no channel binding (gs2 header "n,,", so c=biws).
        const std::string combinedNonce = clientNonce + serverNonce;
        std::ostringstream ss;
        ss << "n=" << EscapeSaslName(username) << ",r=" << clientNonce << ",r=" << combinedNonce
           << ",s=" << Crypto::Base64Encode(salt) << ",i=" << iterations << ",c=biws,r=" << combinedNonce;
        return ss.str();
    }

    // === Registration ===

    TFAuthResult TFAccountSystem::RegisterVerifier(const std::string& username, const std::vector<uint8_t>& salt,
                                                   uint32_t iterations, const Crypto::Sha256Digest& storedKey,
                                                   const Crypto::Sha256Digest& serverKey)
    {
        TFAuthResult result;
        if (!m_db)
        {
            result.err = TFAuthErr::ServerError;
            return result;
        }
        if (username.size() < 3)
        {
            result.err = TFAuthErr::UsernameTooShort;
            return result;
        }
        // The verifier is client-derived, so it is untrusted input: a floor keeps it
        // strong and a ceiling keeps every later login for the name bounded (the
        // row's iteration count is paid by each login and handed to each client).
        if (salt.size() < kMinScramSaltBytes || salt.size() > kMaxScramSaltBytes || iterations < kMinScramIterations ||
            iterations > kMaxScramIterations)
        {
            result.err = TFAuthErr::WeakVerifier;
            return result;
        }

        TFAccountRecord existing;
        if (m_db->FindAccountByUsername(username, existing))
        {
            result.err = TFAuthErr::UsernameTaken;
            return result;
        }

        TFAccountRecord rec;
        if (!m_db->CreateAccount(username, Crypto::ToHex(salt), FormatScramRow(iterations, salt, storedKey, serverKey),
                                 rec))
        {
            result.err = TFAuthErr::ServerError;
            return result;
        }

        result.ok = true;
        result.err = TFAuthErr::Ok;
        result.accountId = rec.id;
        return result;
    }

    TFAuthResult TFAccountSystem::Register(const std::string& username, const std::string& password)
    {
        TFAuthResult result;
        if (!m_db)
        {
            result.err = TFAuthErr::ServerError;
            return result;
        }
        if (username.size() < 3)
        {
            result.err = TFAuthErr::UsernameTooShort;
            return result;
        }
        if (password.size() < 8)
        {
            result.err = TFAuthErr::PasswordTooShort;
            return result;
        }

        const std::string salt = GenerateSalt(m_randomFill);
        if (salt.empty())
        {
            // CSPRNG unavailable: refuse to create the account rather than
            // persist a verifier under a missing or predictable salt.
            result.err = TFAuthErr::ServerError;
            return result;
        }
        const std::vector<uint8_t> saltBytes = Crypto::FromHex(salt);
        const Crypto::ScramKeys keys = Crypto::DeriveScramKeys(password, saltBytes, kPbkdf2Iterations);
        return RegisterVerifier(username, saltBytes, kPbkdf2Iterations, keys.storedKey, keys.serverKey);
    }

    // === SCRAM login ===

    TFScramChallenge TFAccountSystem::BeginLogin(const std::string& username)
    {
        TFScramChallenge challenge;
        challenge.serverNonce = RandomHex(kNonceBytes);

        TFAccountRecord rec;
        ScramVerifier verifier;
        if (m_db && m_db->FindAccountByUsername(username, rec))
        {
            const std::string migrated = MigrateLegacyRow(rec.passwordHash);
            if (!migrated.empty() && m_db->UpdateAccountPasswordHash(rec.id, migrated))
            {
                rec.passwordHash = migrated;
            }
            if (ParseScramRow(rec.passwordHash, verifier))
            {
                challenge.salt = verifier.salt;
                challenge.iterations = verifier.iterations;
                if (!challenge.serverNonce.empty())
                {
                    m_pendingLogins[username] = challenge.serverNonce; // single use; replaces any earlier challenge
                }
                return challenge;
            }
        }

        // Unknown user (or an unusable row): answer with a salt that is stable for
        // this username, so the reply does not reveal whether the account exists.
        if (m_unknownUserKey.empty())
        {
            m_unknownUserKey = RandomHex(32);
        }
        const Crypto::Sha256Digest pseudoSalt = Crypto::HmacSha256(m_unknownUserKey, "unknown-user-salt:" + username);
        challenge.salt.assign(pseudoSalt.begin(), pseudoSalt.begin() + kSaltBytes);
        challenge.iterations = kPbkdf2Iterations;
        return challenge;
    }

    TFScramLoginResult TFAccountSystem::CompleteLogin(const std::string& username, const std::string& clientNonce,
                                                      const std::string& serverNonce,
                                                      std::span<const uint8_t> clientProof)
    {
        TFScramLoginResult outcome;
        outcome.auth.err = TFAuthErr::BadCredentials;
        if (!m_db)
        {
            outcome.auth.err = TFAuthErr::ServerError;
            return outcome;
        }

        // The challenge is consumed by the first attempt, right or wrong, so a
        // captured proof can never be presented again.
        const auto pending = m_pendingLogins.find(username);
        if (pending == m_pendingLogins.end())
        {
            return outcome;
        }
        const std::string expectedNonce = pending->second;
        m_pendingLogins.erase(pending);

        if (!IsValidNonce(clientNonce) || clientProof.size() != Crypto::Sha256Digest{}.size() ||
            !Crypto::ConstantTimeEquals(serverNonce, expectedNonce))
        {
            return outcome;
        }
        Crypto::Sha256Digest proof{};
        std::copy(clientProof.begin(), clientProof.end(), proof.begin());

        TFAccountRecord rec;
        ScramVerifier verifier;
        if (!m_db->FindAccountByUsername(username, rec) || !ParseScramRow(rec.passwordHash, verifier))
        {
            return outcome;
        }

        // ClientKey = ClientProof XOR HMAC(StoredKey, AuthMessage); it must hash to StoredKey.
        const std::string authMessage =
            ScramAuthMessage(username, clientNonce, serverNonce, verifier.salt, verifier.iterations);
        Crypto::Sha256Digest clientSignature =
            Crypto::HmacSha256(verifier.storedKey.data(), verifier.storedKey.size(),
                               reinterpret_cast<const uint8_t*>(authMessage.data()), authMessage.size());
        Crypto::Sha256Digest clientKey = Crypto::XorBytes(proof, clientSignature);
        const bool proven =
            Crypto::ConstantTimeEquals(Crypto::Sha256(clientKey.data(), clientKey.size()), verifier.storedKey);
        Spark::SecureErase(clientSignature.data(), clientSignature.size());
        Spark::SecureErase(clientKey.data(), clientKey.size());
        if (!proven)
        {
            return outcome;
        }

        const int64_t nowMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        if (!m_db->TouchLogin(rec.id, nowMs))
        {
            outcome.auth.err = TFAuthErr::ServerError;
            return outcome;
        }

        outcome.auth.ok = true;
        outcome.auth.err = TFAuthErr::Ok;
        outcome.auth.accountId = rec.id;
        outcome.serverSignature =
            Crypto::HmacSha256(verifier.serverKey.data(), verifier.serverKey.size(),
                               reinterpret_cast<const uint8_t*>(authMessage.data()), authMessage.size());
        return outcome;
    }

    TFAuthResult TFAccountSystem::Login(const std::string& username, const std::string& password)
    {
        // Local wrapper: plays both SCRAM roles in-process (tests, offline tools).
        TFAuthResult result;
        if (!m_db)
        {
            result.err = TFAuthErr::ServerError;
            return result;
        }
        const TFScramChallenge challenge = BeginLogin(username);
        const std::string clientNonce = RandomHex(kNonceBytes);
        if (challenge.serverNonce.empty() || clientNonce.empty())
        {
            m_pendingLogins.erase(username);
            result.err = TFAuthErr::ServerError; // CSPRNG unavailable
            return result;
        }
        // Client role: never run PBKDF2 on a challenge outside the verifier policy.
        if (challenge.iterations > kMaxScramIterations || challenge.salt.size() > kMaxScramSaltBytes)
        {
            m_pendingLogins.erase(username);
            result.err = TFAuthErr::ServerError;
            return result;
        }

        const Crypto::ScramKeys keys = Crypto::DeriveScramKeys(password, challenge.salt, challenge.iterations);
        const std::string authMessage =
            ScramAuthMessage(username, clientNonce, challenge.serverNonce, challenge.salt, challenge.iterations);
        const Crypto::Sha256Digest proof = Crypto::ScramClientProof(keys, authMessage);
        const TFScramLoginResult outcome = CompleteLogin(username, clientNonce, challenge.serverNonce, proof);

        // Mutual authentication: the server proves it holds ServerKey.
        const Crypto::Sha256Digest expectedServerSignature =
            Crypto::HmacSha256(keys.serverKey.data(), keys.serverKey.size(),
                               reinterpret_cast<const uint8_t*>(authMessage.data()), authMessage.size());
        if (outcome.auth.ok && !Crypto::ConstantTimeEquals(outcome.serverSignature, expectedServerSignature))
        {
            result.err = TFAuthErr::ServerError;
            return result;
        }
        return outcome.auth;
    }

    // === Session map ===

    bool TFAccountSystem::BindSession(uint32_t clientId, uint64_t accountId)
    {
        // One live connection per account: a second connection that proves the
        // same credentials must not get its own runtime copy of the account's
        // characters (both copies would later persist to the same rows, letting
        // a stale wallet overwrite a spend). The first session keeps the account
        // until ClearSession (disconnect / logout cleanup).
        if (accountId == 0)
        {
            return false;
        }
        if (const auto owner = m_accountOwners.find(accountId); owner != m_accountOwners.end())
        {
            return owner->second == clientId;
        }

        if (const auto previous = m_sessions.find(clientId); previous != m_sessions.end())
        {
            m_accountOwners.erase(previous->second);
        }
        m_sessions[clientId] = accountId;
        m_accountOwners[accountId] = clientId;
        return true;
    }

    uint64_t TFAccountSystem::AccountForClient(uint32_t clientId) const
    {
        auto it = m_sessions.find(clientId);
        return it != m_sessions.end() ? it->second : 0;
    }

    void TFAccountSystem::ClearSession(uint32_t clientId)
    {
        const auto it = m_sessions.find(clientId);
        if (it == m_sessions.end())
        {
            return;
        }
        if (const auto owner = m_accountOwners.find(it->second);
            owner != m_accountOwners.end() && owner->second == clientId)
        {
            m_accountOwners.erase(owner);
        }
        m_sessions.erase(it);
    }

} // namespace Terrafront
