/**
 * @file TestTFScramAuth.cpp
 * @brief NET-100: TERRAFRONT login proves the password with SCRAM-SHA-256 instead of sending it.
 *
 * Drives the shipped TFAccountSystem/TFCrypto over a real TFDatabase file. The
 * server keeps only StoredKey/ServerKey; a login carries a nonce-bound proof
 * that cannot be replayed, and legacy PBKDF2 rows (whose derived key is
 * password-equivalent) are rewritten as SCRAM verifiers on first contact.
 */

#include "TestFramework.h"
#include "Account/TFAccountSystem.h"
#include "Account/TFCrypto.h"
#include "Persistence/TFDatabase.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace Terrafront;

namespace
{
    namespace fs = std::filesystem;

    std::string FreshDb(const std::string& name)
    {
        const std::string path = "Saves/" + name;
        fs::create_directories("Saves");
        fs::remove(path);
        return path;
    }

    std::string ReadFile(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    /// Transport connection ids for the exchanges below (0 is reserved for the local wrapper).
    constexpr uint32_t kConn = 7;
    constexpr uint32_t kAttackerConn = 8;

    /// Client side of one SCRAM exchange against `accounts`, with a caller-chosen proof tweak.
    struct ClientAttempt
    {
        uint32_t clientId = kConn;
        std::string clientNonce = "clientNonce-7f3a";
        TFScramChallenge challenge;
        Crypto::Sha256Digest proof{};
        std::string authMessage;
    };

    ClientAttempt PrepareLogin(TFAccountSystem& accounts, const std::string& user, const std::string& password,
                               uint32_t clientId = kConn)
    {
        ClientAttempt attempt;
        attempt.clientId = clientId;
        attempt.challenge = accounts.BeginLogin(clientId, user);
        const Crypto::ScramKeys keys =
            Crypto::DeriveScramKeys(password, attempt.challenge.salt, attempt.challenge.iterations);
        attempt.authMessage =
            TFAccountSystem::ScramAuthMessage(user, attempt.clientNonce, attempt.challenge.serverNonce,
                                              attempt.challenge.salt, attempt.challenge.iterations);
        attempt.proof = Crypto::ScramClientProof(keys, attempt.authMessage);
        return attempt;
    }

    TFScramLoginResult Complete(TFAccountSystem& accounts, const std::string& user, const ClientAttempt& attempt)
    {
        return accounts.CompleteLogin(attempt.clientId, user, attempt.clientNonce, attempt.challenge.serverNonce,
                                      attempt.proof);
    }
} // namespace

TEST(TFScram_Rfc7677TestVector)
{
    // RFC 7677 section 3: user "user", password "pencil".
    const std::vector<uint8_t> salt = Crypto::FromHex("5b6d99689d12358eeca04b141236fa81");
    ASSERT_EQ(Crypto::Base64Encode(salt), std::string("W22ZaJ0SNY7soEsUEjb6gQ=="));

    const Crypto::ScramKeys keys = Crypto::DeriveScramKeys("pencil", salt, 4096);
    const std::string authMessage =
        TFAccountSystem::ScramAuthMessage("user", "rOprNGfwEbeRWgbNEkqO", "%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0", salt, 4096);
    EXPECT_EQ(authMessage, std::string("n=user,r=rOprNGfwEbeRWgbNEkqO,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)"
                                       "hNlF$k0,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096,c=biws,r=rOprNGfwEbeRWgbNEkqO%"
                                       "hvYDpWUa2RaTCAfuxFIlj)hNlF$k0"));

    const Crypto::Sha256Digest proof = Crypto::ScramClientProof(keys, authMessage);
    EXPECT_EQ(Crypto::Base64Encode(std::vector<uint8_t>(proof.begin(), proof.end())),
              std::string("dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ="));

    const Crypto::Sha256Digest serverSignature =
        Crypto::HmacSha256(keys.serverKey.data(), keys.serverKey.size(),
                           reinterpret_cast<const uint8_t*>(authMessage.data()), authMessage.size());
    EXPECT_EQ(Crypto::Base64Encode(std::vector<uint8_t>(serverSignature.begin(), serverSignature.end())),
              std::string("6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4="));
}

TEST(TFScram_CorrectProofSucceedsAndServerSigns)
{
    const std::string path = FreshDb("test_tfscram_ok.db");
    TFDatabase db;
    ASSERT_TRUE(db.Open(path));
    TFAccountSystem accounts;
    accounts.SetDatabase(&db);
    const TFAuthResult registered = accounts.Register("scram_pilot", "correct horse 1");
    ASSERT_TRUE(registered.ok);

    const ClientAttempt attempt = PrepareLogin(accounts, "scram_pilot", "correct horse 1");
    EXPECT_EQ(attempt.challenge.iterations, uint32_t{150000});
    EXPECT_EQ(attempt.challenge.salt.size(), size_t{16});
    EXPECT_EQ(attempt.challenge.serverNonce.size(), size_t{48});

    const TFScramLoginResult result = Complete(accounts, "scram_pilot", attempt);
    ASSERT_TRUE(result.auth.ok);
    EXPECT_EQ(result.auth.accountId, registered.accountId);

    // The server proves possession of ServerKey back to the client.
    const Crypto::ScramKeys keys = Crypto::DeriveScramKeys("correct horse 1", attempt.challenge.salt, 150000);
    const Crypto::Sha256Digest expected =
        Crypto::HmacSha256(keys.serverKey.data(), keys.serverKey.size(),
                           reinterpret_cast<const uint8_t*>(attempt.authMessage.data()), attempt.authMessage.size());
    EXPECT_TRUE(Crypto::ConstantTimeEquals(result.serverSignature, expected));

    // Nothing the client sends contains the password.
    const std::string wire =
        attempt.clientNonce + attempt.challenge.serverNonce + std::string(attempt.proof.begin(), attempt.proof.end());
    EXPECT_TRUE(wire.find("correct horse") == std::string::npos);

    EXPECT_TRUE(db.Close());
    fs::remove(path);
}

TEST(TFScram_WrongProofFails)
{
    const std::string path = FreshDb("test_tfscram_wrong.db");
    TFDatabase db;
    ASSERT_TRUE(db.Open(path));
    TFAccountSystem accounts;
    accounts.SetDatabase(&db);
    ASSERT_TRUE(accounts.Register("scram_wrong", "right password").ok);

    const TFScramLoginResult wrongPassword =
        Complete(accounts, "scram_wrong", PrepareLogin(accounts, "scram_wrong", "wrong password"));
    EXPECT_FALSE(wrongPassword.auth.ok);
    EXPECT_TRUE(wrongPassword.auth.err == TFAuthErr::BadCredentials);

    ClientAttempt flipped = PrepareLogin(accounts, "scram_wrong", "right password");
    flipped.proof[0] ^= 0x01;
    EXPECT_FALSE(Complete(accounts, "scram_wrong", flipped).auth.ok);

    // A proof computed for one user does not log in as another: the connection's
    // challenge was issued for scram_wrong, so presenting it as scram_other fails.
    ASSERT_TRUE(accounts.Register("scram_other", "right password").ok);
    ClientAttempt crossUser = PrepareLogin(accounts, "scram_wrong", "right password");
    EXPECT_FALSE(accounts
                     .CompleteLogin(kConn, "scram_other", crossUser.clientNonce, crossUser.challenge.serverNonce,
                                    crossUser.proof)
                     .auth.ok);

    EXPECT_TRUE(db.Close());
    fs::remove(path);
}

TEST(TFScram_ReplayedProofFails)
{
    const std::string path = FreshDb("test_tfscram_replay.db");
    TFDatabase db;
    ASSERT_TRUE(db.Open(path));
    TFAccountSystem accounts;
    accounts.SetDatabase(&db);
    ASSERT_TRUE(accounts.Register("scram_replay", "replay me 123").ok);

    const ClientAttempt captured = PrepareLogin(accounts, "scram_replay", "replay me 123");
    ASSERT_TRUE(Complete(accounts, "scram_replay", captured).auth.ok);

    // The same message again: the challenge was consumed.
    EXPECT_FALSE(Complete(accounts, "scram_replay", captured).auth.ok);

    // The captured proof against a fresh challenge: the server nonce differs.
    const TFScramChallenge fresh = accounts.BeginLogin(kConn, "scram_replay");
    EXPECT_TRUE(fresh.serverNonce != captured.challenge.serverNonce);
    EXPECT_FALSE(
        accounts.CompleteLogin(kConn, "scram_replay", captured.clientNonce, fresh.serverNonce, captured.proof).auth.ok);

    // A wrong attempt also consumes the challenge, so guessing cannot reuse it.
    const ClientAttempt good = PrepareLogin(accounts, "scram_replay", "replay me 123");
    ClientAttempt bad = good;
    bad.proof[5] ^= 0x40;
    EXPECT_FALSE(Complete(accounts, "scram_replay", bad).auth.ok);
    EXPECT_FALSE(Complete(accounts, "scram_replay", good).auth.ok);

    EXPECT_TRUE(db.Close());
    fs::remove(path);
}

TEST(TFScram_TruncatedOrMalformedMessagesFail)
{
    const std::string path = FreshDb("test_tfscram_truncated.db");
    TFDatabase db;
    ASSERT_TRUE(db.Open(path));
    TFAccountSystem accounts;
    accounts.SetDatabase(&db);
    ASSERT_TRUE(accounts.Register("scram_trunc", "truncate me 1").ok);

    ClientAttempt attempt = PrepareLogin(accounts, "scram_trunc", "truncate me 1");
    const std::span<const uint8_t> shortProof(attempt.proof.data(), attempt.proof.size() - 1);
    EXPECT_FALSE(
        accounts.CompleteLogin(kConn, "scram_trunc", attempt.clientNonce, attempt.challenge.serverNonce, shortProof)
            .auth.ok);

    for (const std::string& badNonce : {std::string(), std::string("has,comma"), std::string(200, 'x')})
    {
        attempt = PrepareLogin(accounts, "scram_trunc", "truncate me 1");
        EXPECT_FALSE(
            accounts.CompleteLogin(kConn, "scram_trunc", badNonce, attempt.challenge.serverNonce, attempt.proof)
                .auth.ok);
    }

    // No challenge outstanding at all.
    EXPECT_FALSE(Complete(accounts, "scram_trunc", attempt).auth.ok);

    EXPECT_TRUE(db.Close());
    fs::remove(path);
}

TEST(TFScram_LegacyPbkdf2RowMigratesAndDropsDerivedKey)
{
    const std::string path = FreshDb("test_tfscram_migrate.db");
    const std::string password = "legacy-canary-4411";
    const std::vector<uint8_t> salt = Crypto::FromHex("00112233445566778899aabbccddeeff");
    std::vector<uint8_t> dk = Crypto::Pbkdf2HmacSha256(password, salt, 150000, 32);
    const std::string dkHex = Crypto::ToHex(dk);
    const std::string legacyRow = "pbkdf2-sha256$150000$" + Crypto::ToHex(salt) + "$" + dkHex;

    uint64_t accountId = 0;
    {
        TFDatabase db;
        ASSERT_TRUE(db.Open(path));
        TFAccountRecord rec;
        ASSERT_TRUE(db.CreateAccount("legacy_pilot", Crypto::ToHex(salt), legacyRow, rec));
        accountId = rec.id;
        EXPECT_TRUE(TFAccountSystem::VerifyPassword(password, legacyRow)); // still verifiable before migration
        EXPECT_TRUE(ReadFile(path).find(dkHex) != std::string::npos);
        EXPECT_TRUE(db.Close());
    }

    {
        TFDatabase db;
        ASSERT_TRUE(db.Open(path));
        TFAccountSystem accounts;
        accounts.SetDatabase(&db);
        const TFAuthResult login = accounts.Login("legacy_pilot", password);
        ASSERT_TRUE(login.ok);
        EXPECT_EQ(login.accountId, accountId);
        EXPECT_FALSE(accounts.Login("legacy_pilot", password + "x").ok);

        TFAccountRecord rec;
        ASSERT_TRUE(db.FindAccountByUsername("legacy_pilot", rec));
        EXPECT_TRUE(rec.passwordHash.rfind("scram-sha256$150000$" + Crypto::ToHex(salt) + "$", 0) == 0);
        EXPECT_TRUE(TFAccountSystem::VerifyPassword(password, rec.passwordHash));
        EXPECT_TRUE(db.Close());
    }

    // The password-equivalent derived key is gone from disk.
    const std::string raw = ReadFile(path);
    EXPECT_TRUE(raw.find(dkHex) == std::string::npos);
    EXPECT_TRUE(raw.find("pbkdf2-sha256") == std::string::npos);
    EXPECT_TRUE(raw.find(password) == std::string::npos);
    fs::remove(path);
}

TEST(TFScram_UnknownUserGetsStableSalt)
{
    const std::string path = FreshDb("test_tfscram_unknown.db");
    TFDatabase db;
    ASSERT_TRUE(db.Open(path));
    TFAccountSystem accounts;
    accounts.SetDatabase(&db);
    ASSERT_TRUE(accounts.Register("real_pilot", "a real password").ok);

    const TFScramChallenge first = accounts.BeginLogin(kConn, "ghost_pilot");
    const TFScramChallenge second = accounts.BeginLogin(kConn, "ghost_pilot");
    const TFScramChallenge other = accounts.BeginLogin(kConn, "ghost_other");
    const TFScramChallenge real = accounts.BeginLogin(kConn, "real_pilot");

    // Same shape as a real account, stable per name, distinct across names.
    EXPECT_TRUE(first.salt == second.salt);
    EXPECT_TRUE(first.salt != other.salt);
    EXPECT_EQ(first.salt.size(), real.salt.size());
    EXPECT_EQ(first.iterations, real.iterations);
    EXPECT_EQ(first.serverNonce.size(), real.serverNonce.size());
    EXPECT_TRUE(first.serverNonce != second.serverNonce);

    const ClientAttempt attempt = PrepareLogin(accounts, "ghost_pilot", "any password");
    const TFScramLoginResult result = Complete(accounts, "ghost_pilot", attempt);
    EXPECT_FALSE(result.auth.ok);
    EXPECT_TRUE(result.auth.err == TFAuthErr::BadCredentials);

    EXPECT_TRUE(db.Close());
    fs::remove(path);
}

TEST(TFScram_RegisterVerifierRejectsWeakParameters)
{
    const std::string path = FreshDb("test_tfscram_weak.db");
    TFDatabase db;
    ASSERT_TRUE(db.Open(path));
    TFAccountSystem accounts;
    accounts.SetDatabase(&db);

    const std::vector<uint8_t> goodSalt = Crypto::FromHex("0f1e2d3c4b5a69788796a5b4c3d2e1f0");
    const std::vector<uint8_t> shortSalt = Crypto::FromHex("0f1e2d3c4b5a6978");

    const Crypto::ScramKeys lowIterations = Crypto::DeriveScramKeys("client-side pw", goodSalt, 4096);
    const TFAuthResult weakCount =
        accounts.RegisterVerifier("weak_iter", goodSalt, 4096, lowIterations.storedKey, lowIterations.serverKey);
    EXPECT_FALSE(weakCount.ok);
    EXPECT_TRUE(weakCount.err == TFAuthErr::WeakVerifier);

    const Crypto::ScramKeys shortSaltKeys = Crypto::DeriveScramKeys("client-side pw", shortSalt, 150000);
    const TFAuthResult weakSalt =
        accounts.RegisterVerifier("weak_salt", shortSalt, 150000, shortSaltKeys.storedKey, shortSaltKeys.serverKey);
    EXPECT_FALSE(weakSalt.ok);
    EXPECT_TRUE(weakSalt.err == TFAuthErr::WeakVerifier);

    // The verifier is client-derived: a ceiling keeps every later login for the
    // name bounded. The keys are never derived here, so no PBKDF2 run is paid.
    const Crypto::Sha256Digest anyKey{};
    const TFAuthResult hugeCount =
        accounts.RegisterVerifier("huge_iter", goodSalt, TFAccountSystem::kMaxScramIterations + 1, anyKey, anyKey);
    EXPECT_FALSE(hugeCount.ok);
    EXPECT_TRUE(hugeCount.err == TFAuthErr::WeakVerifier);
    const TFAuthResult maxCount = accounts.RegisterVerifier("max_iter", goodSalt, 999999999u, anyKey,
                                                            anyKey); // ParseIterations used to allow 9 digits
    EXPECT_FALSE(maxCount.ok);
    const std::vector<uint8_t> longSalt(TFAccountSystem::kMaxScramSaltBytes + 1, 0x5A);
    const TFAuthResult hugeSalt = accounts.RegisterVerifier("huge_salt", longSalt, 150000, anyKey, anyKey);
    EXPECT_FALSE(hugeSalt.ok);
    EXPECT_TRUE(hugeSalt.err == TFAuthErr::WeakVerifier);

    TFAccountRecord rec;
    EXPECT_FALSE(db.FindAccountByUsername("weak_iter", rec));
    EXPECT_FALSE(db.FindAccountByUsername("weak_salt", rec));
    EXPECT_FALSE(db.FindAccountByUsername("huge_iter", rec));
    EXPECT_FALSE(db.FindAccountByUsername("max_iter", rec));
    EXPECT_FALSE(db.FindAccountByUsername("huge_salt", rec));

    // A client-derived verifier registers without the password reaching the server.
    const Crypto::ScramKeys keys = Crypto::DeriveScramKeys("client-side pw", goodSalt, 150000);
    ASSERT_TRUE(accounts.RegisterVerifier("verifier_pilot", goodSalt, 150000, keys.storedKey, keys.serverKey).ok);
    EXPECT_TRUE(accounts.Login("verifier_pilot", "client-side pw").ok);
    EXPECT_FALSE(accounts.Login("verifier_pilot", "client-side pX").ok);

    EXPECT_TRUE(db.Close());
    fs::remove(path);
}

// A row planted or corrupted outside RegisterVerifier (the save directory is not
// trusted) must not make a login run unbounded PBKDF2 or hand the client an
// unbounded cost: it is unusable, and BeginLogin answers like an unknown user.
// Without the load-time bound this test hangs until the CTest timeout.
TEST(TFScram_StoredRowOutsidePolicyIsUnusable)
{
    const std::string path = FreshDb("test_tfscram_storedbound.db");
    TFDatabase db;
    ASSERT_TRUE(db.Open(path));
    TFAccountSystem accounts;
    accounts.SetDatabase(&db);

    const std::string salt = "0f1e2d3c4b5a69788796a5b4c3d2e1f0";
    const std::string key(64, 'a');
    const std::string hugeIter = "scram-sha256$999999999$" + salt + "$" + key + "$" + key;
    const std::string overMax = "scram-sha256$600001$" + salt + "$" + key + "$" + key;
    const std::string longSalt = "scram-sha256$150000$" + std::string(130, 'b') + "$" + key + "$" + key;
    const std::string oddSalt = "scram-sha256$150000$" + salt + "a$" + key + "$" + key;
    TFAccountRecord rec;
    ASSERT_TRUE(db.CreateAccount("planted_iter", salt, hugeIter, rec));
    ASSERT_TRUE(db.CreateAccount("planted_salt", salt, longSalt, rec));

    for (const std::string& row : {hugeIter, overMax, longSalt, oddSalt})
    {
        EXPECT_FALSE(TFAccountSystem::VerifyPassword("any password", row));
    }

    for (const char* user : {"planted_iter", "planted_salt"})
    {
        const TFScramChallenge challenge = accounts.BeginLogin(kConn, user);
        EXPECT_EQ(challenge.iterations, uint32_t{150000}); // pseudo-salt path, not the row's cost
        EXPECT_EQ(challenge.salt.size(), size_t{16});
        EXPECT_FALSE(accounts.Login(user, "any password").ok);
    }

    EXPECT_TRUE(db.Close());
    fs::remove(path);
}

// Pending challenges used to be keyed only by username: any unauthenticated peer
// could replace a victim's in-flight challenge with BeginLogin(victim) or erase
// it with a failed CompleteLogin(victim, ...), so knowing a name was enough to
// keep that player from ever logging in. Each challenge now belongs to the
// connection that asked for it.
TEST(TFScram_ChallengeIsBoundToItsConnection)
{
    const std::string path = FreshDb("test_tfscram_conn.db");
    TFDatabase db;
    ASSERT_TRUE(db.Open(path));
    TFAccountSystem accounts;
    accounts.SetDatabase(&db);
    ASSERT_TRUE(accounts.Register("victim_pilot", "victim password 1").ok);

    const ClientAttempt victim = PrepareLogin(accounts, "victim_pilot", "victim password 1", kConn);
    ASSERT_FALSE(victim.challenge.serverNonce.empty());

    // The attacker asks for a challenge for the same name on its own connection...
    const TFScramChallenge attackerChallenge = accounts.BeginLogin(kAttackerConn, "victim_pilot");
    EXPECT_FALSE(attackerChallenge.serverNonce.empty());
    EXPECT_TRUE(attackerChallenge.serverNonce != victim.challenge.serverNonce);
    // ...fails a proof on it, and replays the victim's proof and nonce from its connection.
    const Crypto::Sha256Digest junk{};
    EXPECT_FALSE(
        accounts.CompleteLogin(kAttackerConn, "victim_pilot", "attackerNonce", attackerChallenge.serverNonce, junk)
            .auth.ok);
    EXPECT_FALSE(accounts
                     .CompleteLogin(kAttackerConn, "victim_pilot", victim.clientNonce, victim.challenge.serverNonce,
                                    victim.proof)
                     .auth.ok);
    // A connection that never asked for a challenge cannot consume anyone's.
    EXPECT_FALSE(accounts
                     .CompleteLogin(kAttackerConn + 1, "victim_pilot", victim.clientNonce, victim.challenge.serverNonce,
                                    victim.proof)
                     .auth.ok);

    // The victim's challenge survived all of it.
    const TFScramLoginResult result = Complete(accounts, "victim_pilot", victim);
    EXPECT_TRUE(result.auth.ok);

    // A disconnect (ClearSession) drops the connection's in-flight challenge.
    const ClientAttempt dropped = PrepareLogin(accounts, "victim_pilot", "victim password 1", kAttackerConn);
    accounts.ClearSession(kAttackerConn);
    EXPECT_FALSE(Complete(accounts, "victim_pilot", dropped).auth.ok);

    EXPECT_TRUE(db.Close());
    fs::remove(path);
}

namespace
{
    int64_t g_fakeNowMs = 0;
    int64_t FakeNow() noexcept
    {
        return g_fakeNowMs;
    }
} // namespace

// A pending challenge expires, and the pending map is globally bounded: a flood
// of BeginLogin calls from many connections fails closed for the newcomers
// instead of growing without limit or evicting an in-flight login.
TEST(TFScram_PendingChallengesExpireAndAreBounded)
{
    const std::string path = FreshDb("test_tfscram_pending.db");
    TFDatabase db;
    ASSERT_TRUE(db.Open(path));
    TFAccountSystem accounts;
    accounts.SetDatabase(&db);
    g_fakeNowMs = 1000;
    accounts.SetClock(&FakeNow);
    ASSERT_TRUE(accounts.Register("ttl_pilot", "ttl password 12").ok);

    // Expiry: a proof presented after the TTL fails even though it is correct.
    const ClientAttempt late = PrepareLogin(accounts, "ttl_pilot", "ttl password 12");
    g_fakeNowMs += TFAccountSystem::kLoginChallengeTtlMs + 1;
    EXPECT_FALSE(Complete(accounts, "ttl_pilot", late).auth.ok);

    // Bound: the first connection's challenge is in flight, then the map fills.
    const ClientAttempt inFlight = PrepareLogin(accounts, "ttl_pilot", "ttl password 12", 1);
    ASSERT_FALSE(inFlight.challenge.serverNonce.empty());
    for (uint32_t conn = 2; conn <= TFAccountSystem::kMaxPendingLogins; ++conn)
    {
        ASSERT_FALSE(accounts.BeginLogin(conn, "ttl_pilot").serverNonce.empty());
    }
    const uint32_t newcomer = TFAccountSystem::kMaxPendingLogins + 1;
    EXPECT_TRUE(accounts.BeginLogin(newcomer, "ttl_pilot").serverNonce.empty()); // fails closed
    EXPECT_TRUE(!accounts.BeginLogin(2, "ttl_pilot").serverNonce.empty());       // own slot is reusable

    // The in-flight login was not evicted by the flood.
    EXPECT_TRUE(Complete(accounts, "ttl_pilot", inFlight).auth.ok);

    // Refill the freed slot so the map is full again; only expiry frees room now.
    ASSERT_FALSE(accounts.BeginLogin(1, "ttl_pilot").serverNonce.empty());
    EXPECT_TRUE(accounts.BeginLogin(newcomer, "ttl_pilot").serverNonce.empty());

    // Once the flood's challenges expire, newcomers are served again.
    g_fakeNowMs += TFAccountSystem::kLoginChallengeTtlMs + 1;
    const ClientAttempt after = PrepareLogin(accounts, "ttl_pilot", "ttl password 12", newcomer + 1);
    EXPECT_FALSE(after.challenge.serverNonce.empty());
    EXPECT_TRUE(Complete(accounts, "ttl_pilot", after).auth.ok);

    accounts.SetClock(nullptr);
    EXPECT_TRUE(db.Close());
    fs::remove(path);
}
