/**
 * @file TestTFOnboarding.cpp
 * @brief TERRAFRONT W5 onboarding tests: TFDatabase (Task 1), TFAccountSystem
 *        (Task 2), TFCharacterSystem (Task 3), and the Task 4 net-protocol
 *        wire-layout freeze for the new onboarding TFMsg PODs (independent of
 *        TestTFNetProtocolLayout.cpp's coverage of the pre-W5 messages).
 */
#include "TestFramework.h"
#include "Persistence/TFDatabase.h"
#include "Persistence/TFPlayerMeta.h"
#include "Persistence/TFSavePaths.h"
#include "Persistence/TFShutdownOrder.h"
#include "Persistence/TFWorldSave.h"
#include "Account/TFAccountSystem.h"
#include "Account/TFCharacterSystem.h"
#include "Account/TFCrypto.h"
#include "Net/TFNetProtocol.h"
#include "Net/TFClientSessionState.h"
#include "Net/TFClientSessionEnd.h"
#include "Net/TFNetworkLifecycle.h"
#include "Net/TFOnboardingSessionRules.h"
#include "Console/TFQuickplay.h"
#include "UI/TFLoginFlow.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

using namespace Terrafront;

TEST(TFQuickplay_OptionsAreBoundedAndDeterministic)
{
    TFQuickplayOptions options;
    std::string error;
    EXPECT_TRUE(ParseQuickplayOptions({"pilot", "long-password", "hlx", "medtech", "24"}, options, error));
    EXPECT_TRUE(options.username == "pilot");
    EXPECT_TRUE(options.faction == FactionId::HLX);
    EXPECT_TRUE(options.playerClass == ClassId::Medtech);
    EXPECT_EQ(options.botCount, static_cast<uint32_t>(24));
    EXPECT_TRUE(error.empty());

    EXPECT_FALSE(ParseQuickplayOptions({"pilot"}, options, error));
    EXPECT_TRUE(options.password.empty());
    EXPECT_FALSE(ParseQuickplayOptions({"pilot", "password", "bad"}, options, error));
    EXPECT_TRUE(options.password.empty());
    EXPECT_FALSE(ParseQuickplayOptions({"pilot", "password", "mra", "bad"}, options, error));
    EXPECT_TRUE(options.password.empty());
    EXPECT_FALSE(ParseQuickplayOptions({"pilot", "password", "mra", "striker", "33"}, options, error));
    EXPECT_TRUE(options.password.empty());
    EXPECT_TRUE(QuickplayCharacterName(42, FactionId::AUC) == "Demo42A");
    EXPECT_TRUE(QuickplayCharacterName(UINT64_MAX, FactionId::HLX).size() <= 23);
}

TEST(TFQuickplay_RequiresLiveListenHostRuntime)
{
    EXPECT_TRUE(IsQuickplayListenHostRuntime(NetRole::ListenHost, true, true, true));
    EXPECT_FALSE(IsQuickplayListenHostRuntime(NetRole::Standalone, true, true, true));
    EXPECT_FALSE(IsQuickplayListenHostRuntime(NetRole::DedicatedServer, true, true, true));
    EXPECT_FALSE(IsQuickplayListenHostRuntime(NetRole::Client, true, true, true));
    EXPECT_FALSE(IsQuickplayListenHostRuntime(NetRole::ListenHost, false, true, true));
    EXPECT_FALSE(IsQuickplayListenHostRuntime(NetRole::ListenHost, true, false, true));
    EXPECT_FALSE(IsQuickplayListenHostRuntime(NetRole::ListenHost, true, true, false));
}

TEST(TFLoginFlow_SynchronousLoopbackReplyWinsOverArmedRequestState)
{
    enum class Pending
    {
        None,
        Login
    };

    Pending pending = Pending::None;
    DispatchAfterArmingOnboardingState([&] { pending = Pending::Login; },
                                       [&]
                                       {
                                           EXPECT_TRUE(pending == Pending::Login);
                                           pending = Pending::None; // synchronous login reply sink
                                       });
    EXPECT_TRUE(pending == Pending::None);

    TFFlowState flow = TFFlowState::Login;
    float enterTimer = 7.0f;
    DispatchAfterArmingOnboardingState(
        [&]
        {
            enterTimer = 0.0f;
            flow = TFFlowState::EnteringWorld;
        },
        [&]
        {
            EXPECT_TRUE(flow == TFFlowState::EnteringWorld);
            flow = TFFlowState::InWorld; // synchronous TF_WorldWelcome sink
        });
    EXPECT_TRUE(flow == TFFlowState::InWorld);
    EXPECT_EQ(enterTimer, 0.0f);
}

TEST(TFClientSessionState_ResetClearsAuthenticationAndCharacters)
{
    TFClientSessionState state;
    state.loggedIn = true;
    state.accountId = 42;
    state.lastAuthError = TFAuthErr::Ok;
    state.characters.push_back(TF_CharBrief{});
    state.lastCharacterError = TFCharErr::Ok;
    state.lastCharacterId = 77;

    state.Reset();

    EXPECT_FALSE(state.loggedIn);
    EXPECT_EQ(state.accountId, uint64_t{0});
    EXPECT_TRUE(state.lastAuthError == TFAuthErr::NotLoggedIn);
    EXPECT_TRUE(state.characters.empty());
    EXPECT_TRUE(state.lastCharacterError == TFCharErr::NotLoggedIn);
    EXPECT_EQ(state.lastCharacterId, uint64_t{0});
}

TEST(TFClientSessionState_LoginRepliesNeverExposeAStaleProfile)
{
    TFClientSessionState state;
    state.loggedIn = true;
    state.accountId = 42;
    state.characters.push_back(TF_CharBrief{});
    state.lastCharacterError = TFCharErr::Ok;
    state.lastCharacterId = 77;

    state.ApplyLoginReply(false, 0, TFAuthErr::BadCredentials);
    EXPECT_FALSE(state.loggedIn);
    EXPECT_EQ(state.accountId, uint64_t{0});
    EXPECT_TRUE(state.characters.empty());
    EXPECT_TRUE(state.lastCharacterError == TFCharErr::NotLoggedIn);
    EXPECT_EQ(state.lastCharacterId, uint64_t{0});

    state.loggedIn = true;
    state.accountId = 99;
    state.characters.push_back(TF_CharBrief{});
    state.ApplyLoginReply(false, 0, TFAuthErr::SessionActive);
    EXPECT_TRUE(state.loggedIn);
    EXPECT_EQ(state.accountId, uint64_t{99});
    EXPECT_EQ(state.characters.size(), size_t{1});
}

TEST(TFClientSessionEnd_UnexpectedRemoteDropRestoresStandaloneLogin)
{
    const TFClientSessionEndDecision dropped = PlanClientSessionEnd(NetRole::Client, false);
    EXPECT_TRUE(dropped.role == NetRole::Standalone);
    EXPECT_TRUE(dropped.resetLoginFlow);

    const TFClientSessionEndDecision alreadyLogin = PlanClientSessionEnd(NetRole::Client, true);
    EXPECT_TRUE(alreadyLogin.role == NetRole::Standalone);
    EXPECT_FALSE(alreadyLogin.resetLoginFlow);

    const TFClientSessionEndDecision listenHost = PlanClientSessionEnd(NetRole::ListenHost, false);
    EXPECT_TRUE(listenHost.role == NetRole::ListenHost);
    EXPECT_TRUE(listenHost.resetLoginFlow);
}

TEST(TFOnboardingSessionRules_RejectReauthAndMidWorldProfileMutation)
{
    EXPECT_TRUE(CanBeginAuthentication(false, false));
    EXPECT_FALSE(CanBeginAuthentication(true, false));
    EXPECT_FALSE(CanBeginAuthentication(false, true));
    EXPECT_FALSE(CanBeginAuthentication(true, true));
    EXPECT_TRUE(CanMutateCharacterProfile(false));
    EXPECT_FALSE(CanMutateCharacterProfile(true));
}

TEST(TFNetworkLifecycle_DrainsRemoteSessionsAndSupportsImmediateRehost)
{
    std::unordered_set<PlayerId> knownClients{11, 12};
    bool handlersRegistered = true;
    std::vector<PlayerId> cleaned;
    int unregisterCalls = 0;

    StopNetworkSessionLifecycle(
        knownClients, handlersRegistered, {12, 13, kInvalidPlayer}, [&](PlayerId player) { cleaned.push_back(player); },
        [&] { ++unregisterCalls; });

    std::sort(cleaned.begin(), cleaned.end());
    EXPECT_EQ(cleaned.size(), size_t{3});
    EXPECT_EQ(cleaned[0], PlayerId{11});
    EXPECT_EQ(cleaned[1], PlayerId{12});
    EXPECT_EQ(cleaned[2], PlayerId{13});
    EXPECT_TRUE(knownClients.empty());
    EXPECT_FALSE(handlersRegistered);
    EXPECT_EQ(unregisterCalls, 1);

    // A host can restart before another fixed tick: registration state is
    // already false, so the new server installs handlers and drains normally.
    handlersRegistered = true;
    knownClients.insert(21);
    cleaned.clear();
    StopNetworkSessionLifecycle(
        knownClients, handlersRegistered, {}, [&](PlayerId player) { cleaned.push_back(player); },
        [&] { ++unregisterCalls; });
    EXPECT_EQ(cleaned.size(), size_t{1});
    EXPECT_EQ(cleaned[0], PlayerId{21});
    EXPECT_FALSE(handlersRegistered);
    EXPECT_EQ(unregisterCalls, 2);
}

TEST(TFQuickplay_ScriptsRespectTheEnterWorldGate)
{
#ifdef SPARK_TEST_SOURCE_DIR
    const std::filesystem::path sourceRoot = SPARK_TEST_SOURCE_DIR;
#else
    const std::filesystem::path sourceRoot = std::filesystem::current_path();
#endif
    for (const char* relative : {"Tools/tf_play.cfg", "Tools/tf_smoke_host.cfg"})
    {
        std::ifstream input(sourceRoot / relative, std::ios::binary);
        EXPECT_TRUE(input.good());
        std::ostringstream buffer;
        buffer << input.rdbuf();
        const std::string script = buffer.str();
        EXPECT_TRUE(script.find("tf_host") != std::string::npos);
        EXPECT_TRUE(script.find("tf_bots") != std::string::npos);
        EXPECT_TRUE(script.find(" tf_spawn") == std::string::npos);
        EXPECT_TRUE(script.find(" tf_faction") == std::string::npos);
    }
}

TEST(TFDatabase_AccountCharacter_RoundTrip)
{
    namespace fs = std::filesystem;
    const std::string path = "Saves/test_tfdb_roundtrip.db";
    fs::remove(path);
    uint64_t acctId = 0, charId = 0;
    {
        TFDatabase db;
        EXPECT_FALSE(db.IsOpen());
        EXPECT_TRUE(db.Open(path));
        EXPECT_TRUE(db.IsOpen());
        TFAccountRecord a;
        EXPECT_TRUE(db.CreateAccount("commander", "abc123", "hashed", a));
        acctId = a.id;
        EXPECT_TRUE(acctId != 0);
        EXPECT_FALSE(db.CreateAccount("commander", "x", "y", a)); // username taken
        TFCharacterRecord c;
        EXPECT_TRUE(db.CreateCharacter(acctId, "Vanguard", FactionId::MRA, c));
        charId = c.id;
        EXPECT_TRUE(charId != 0);
        db.SaveCharacterProgress(charId, 4200, 7, 150, 111);
        db.Close();
        EXPECT_FALSE(db.IsOpen());
    }
    { // reopen a fresh instance -> data survived
        TFDatabase db;
        EXPECT_TRUE(db.Open(path));
        TFAccountRecord a;
        EXPECT_TRUE(db.FindAccountByUsername("commander", a));
        EXPECT_EQ(a.id, acctId);
        EXPECT_TRUE(a.passwordHash == "hashed");
        EXPECT_TRUE(a.salt == "abc123");
        auto chars = db.ListCharacters(acctId);
        EXPECT_EQ(chars.size(), (size_t)1);
        TFCharacterRecord c;
        EXPECT_TRUE(db.FindCharacter(charId, c));
        EXPECT_TRUE(c.name == "Vanguard");
        EXPECT_TRUE(c.faction == FactionId::MRA);
        EXPECT_EQ((int)c.xp, 4200);
        EXPECT_EQ((int)c.rank, 7);
        EXPECT_EQ((int)c.flux, 150);
        db.Close();
    }
    fs::remove(path);
}

TEST(TFAccountSystem_Register_Login_HashVerify)
{
    namespace fs = std::filesystem;
    const std::string path = "Saves/test_tfacct.db";
    fs::remove(path);
    TFDatabase db;
    EXPECT_TRUE(db.Open(path));
    TFAccountSystem acct;
    acct.SetDatabase(&db);
    auto r = acct.Register("commander", "sekret1!");
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(r.accountId != 0);
    EXPECT_FALSE(acct.Register("commander", "other456").ok); // taken
    EXPECT_TRUE(acct.Register("x", "pw123456").err == TFAuthErr::UsernameTooShort);
    EXPECT_TRUE(acct.Login("commander", "sekret1!").ok); // correct
    auto bad = acct.Login("commander", "wrong123");
    EXPECT_FALSE(bad.ok);
    EXPECT_TRUE(bad.err == TFAuthErr::BadCredentials);
    EXPECT_TRUE(acct.Login("ghost", "x").err == TFAuthErr::BadCredentials); // no such user
    db.Close();
    fs::remove(path);
}

TEST(TFAccountSystem_PasswordHardening)
{
    namespace fs = std::filesystem;
    const std::string path = "Saves/test_tfacct_hardening.db";
    fs::remove(path);
    TFDatabase db;
    EXPECT_TRUE(db.Open(path));
    TFAccountSystem acct;
    acct.SetDatabase(&db);

    // Empty and too-short passwords are rejected with a distinct error code.
    auto emptyPw = acct.Register("validuser", "");
    EXPECT_FALSE(emptyPw.ok);
    EXPECT_TRUE(emptyPw.err == TFAuthErr::PasswordTooShort);
    auto shortPw = acct.Register("validuser", "abcd");
    EXPECT_FALSE(shortPw.ok);
    EXPECT_TRUE(shortPw.err == TFAuthErr::PasswordTooShort);

    // A >=8 char password succeeds.
    auto okPw = acct.Register("validuser", "longenough1");
    EXPECT_TRUE(okPw.ok);
    EXPECT_TRUE(okPw.accountId != 0);

    // HashPassword is not a bare single std::hash pass, is salt-sensitive, and deterministic.
    std::string bareHash = std::to_string(std::hash<std::string>{}(std::string("abc")));
    EXPECT_TRUE(TFAccountSystem::HashPassword("abc", "salt") != bareHash);
    EXPECT_TRUE(TFAccountSystem::HashPassword("samepw", "salt1") != TFAccountSystem::HashPassword("samepw", "salt2"));
    EXPECT_TRUE(TFAccountSystem::HashPassword("samepw", "saltX") == TFAccountSystem::HashPassword("samepw", "saltX"));

    // Regression: login still round-trips correctly after the HashPassword change.
    EXPECT_TRUE(acct.Login("validuser", "longenough1").ok);
    EXPECT_FALSE(acct.Login("validuser", "wrongpassword").ok);

    db.Close();
    fs::remove(path);
}

// ============================================================================
// PBKDF2-HMAC-SHA256 hardening (replaces the demo iterated-std::hash scheme):
// known-answer tests against published/authoritative vectors, plus
// self-consistency and account round-trip coverage.
// ============================================================================

TEST(TFCrypto_Sha256_KnownAnswer)
{
    // FIPS 180-4 standard test vector: SHA-256("abc").
    auto digest = Terrafront::Crypto::Sha256(std::string("abc"));
    EXPECT_TRUE(Terrafront::Crypto::ToHex(digest.data(), digest.size()) ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(TFCrypto_HmacSha256_KnownAnswer)
{
    // RFC 4231 Test Case 1: Key = 20 bytes of 0x0b, Data = "Hi There".
    std::vector<uint8_t> key(20, 0x0b);
    const std::string data = "Hi There";
    auto mac = Terrafront::Crypto::HmacSha256(key.data(), key.size(), reinterpret_cast<const uint8_t*>(data.data()),
                                              data.size());
    EXPECT_TRUE(Terrafront::Crypto::ToHex(mac.data(), mac.size()) ==
                "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

TEST(TFCrypto_Pbkdf2HmacSha256_KnownAnswer)
{
    // Published PBKDF2-HMAC-SHA256 vector: P="password" S="salt" c=1 dkLen=32.
    // Cross-verified against golang.org/x/crypto/pbkdf2's SHA-256 test table
    // (first 20 bytes) and independent PBKDF2-HMAC-SHA2 vector tables (full
    // 32-byte output) before landing this test.
    const std::vector<uint8_t> salt{'s', 'a', 'l', 't'};
    auto dk = Terrafront::Crypto::Pbkdf2HmacSha256("password", salt, 1, 32);
    EXPECT_TRUE(Terrafront::Crypto::ToHex(dk) == "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
}

TEST(TFCrypto_Pbkdf2_SelfConsistency)
{
    const std::vector<uint8_t> saltA{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const std::vector<uint8_t> saltB{16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};

    auto d1 = Terrafront::Crypto::Pbkdf2HmacSha256("hunter2", saltA, 1000, 32);
    auto d2 = Terrafront::Crypto::Pbkdf2HmacSha256("hunter2", saltA, 1000, 32);
    EXPECT_TRUE(d1 == d2); // deterministic: same inputs -> same output

    auto d3 = Terrafront::Crypto::Pbkdf2HmacSha256("hunter2", saltB, 1000, 32);
    EXPECT_TRUE(!(d1 == d3)); // salt-sensitive: different salt -> different output
}

TEST(TFAccountSystem_PBKDF2Scheme_StoredHashAndVerify)
{
    namespace fs = std::filesystem;
    const std::string path = "Saves/test_tfacct_pbkdf2.db";
    fs::remove(path);
    TFDatabase db;
    EXPECT_TRUE(db.Open(path));
    TFAccountSystem acct;
    acct.SetDatabase(&db);

    auto r = acct.Register("kdftest", "correcthorse1");
    EXPECT_TRUE(r.ok);

    TFAccountRecord rec;
    EXPECT_TRUE(db.FindAccountByUsername("kdftest", rec));

    // Stored hash uses the new self-describing scheme, not the old bare-hex
    // std::hash output.
    EXPECT_TRUE(rec.passwordHash.rfind("pbkdf2-sha256$", 0) == 0);
    {
        size_t p1 = rec.passwordHash.find('$');
        size_t p2 = rec.passwordHash.find('$', p1 + 1);
        EXPECT_TRUE(p1 != std::string::npos && p2 != std::string::npos);
        int iters = std::stoi(rec.passwordHash.substr(p1 + 1, p2 - p1 - 1));
        EXPECT_TRUE(iters >= 100000); // hardening floor
    }

    // Direct KDF verification API: correct password accepts, wrong rejects.
    EXPECT_TRUE(TFAccountSystem::VerifyPassword("correcthorse1", rec.passwordHash));
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("wrongpassword", rec.passwordHash));

    // End-to-end Login still round-trips through the new scheme.
    EXPECT_TRUE(acct.Login("kdftest", "correcthorse1").ok);
    EXPECT_FALSE(acct.Login("kdftest", "wrongpassword").ok);

    // A stored hash in the old (legacy) format is rejected, not crashed on.
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("anything", "deadbeefcafef00d"));
    EXPECT_FALSE(TFAccountSystem::VerifyPassword("anything", ""));

    db.Close();
    fs::remove(path);
}

TEST(TFDatabase_CorruptFile_QuarantinedNotWiped)
{
    namespace fs = std::filesystem;
    const std::string path = "Saves/test_tfdb_corrupt.db";
    fs::remove(path);

    // Write garbage (valid JSON, but not an object — a JSON array) where a db
    // file would be. NOTE: intentionally a syntactically-valid-but-wrong-type
    // payload rather than truncated/unparseable text: this repo's vendored
    // JSON parser parses malformed object syntax (e.g. "{not json") leniently
    // into an empty object rather than failing, so it would not exercise the
    // IsObject() corruption check. A non-object top-level value reliably does.
    const std::string garbage = "[1,2,3]";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << garbage;
    }

    TFDatabase db;
    EXPECT_FALSE(db.Open(path)); // refuses to open a corrupt existing file

    // The primary remains corrupt so a fresh process cannot reinterpret the
    // missing path as a brand-new authentication database.
    EXPECT_TRUE(fs::exists(path));

    bool foundBackup = false;
    std::string backupContents;
    for (const auto& entry : fs::directory_iterator(fs::path(path).parent_path()))
    {
        const std::string fname = entry.path().filename().string();
        if (fname.rfind("test_tfdb_corrupt.db.corrupt-", 0) == 0)
        {
            foundBackup = true;
            {
                std::ifstream in(entry.path(), std::ios::binary);
                std::ostringstream ss;
                ss << in.rdbuf();
                backupContents = ss.str();
            } // close the handle before removing (Windows locks open files)
            fs::remove(entry.path());
            break;
        }
    }
    EXPECT_TRUE(foundBackup);
    EXPECT_TRUE(backupContents == garbage); // original bytes preserved verbatim

    // Both the live object and a fresh process fail closed until an operator
    // explicitly restores or removes the corrupt primary.
    EXPECT_TRUE(db.RecoveryLatched());
    EXPECT_TRUE(db.LastStatus() == TFDatabaseStatus::Corrupt);
    EXPECT_FALSE(db.Open(path));
    EXPECT_TRUE(fs::exists(path));
    TFDatabase restarted;
    EXPECT_FALSE(restarted.Open(path));
    EXPECT_TRUE(restarted.LastStatus() == TFDatabaseStatus::Corrupt);

    for (const auto& entry : fs::directory_iterator(fs::path(path).parent_path()))
    {
        if (entry.path().filename().string().rfind("test_tfdb_corrupt.db.corrupt-", 0) == 0)
            fs::remove(entry.path());
    }
    fs::remove(path);
}

TEST(TFDatabase_WriteFailureRollsBackMutation)
{
    namespace fs = std::filesystem;
    const fs::path path = fs::path("Saves") / "test_tfdb_write_failure.db";
    fs::remove(path);
    fs::remove_all(fs::path(path.wstring() + L".tmp"));

    TFDatabase db;
    EXPECT_TRUE(db.Open(path));
    const fs::path blocker(path.wstring() + L".tmp");
    EXPECT_TRUE(fs::create_directory(blocker));

    TFAccountRecord account;
    EXPECT_FALSE(db.CreateAccount("rollback", "salt", "hash", account));
    EXPECT_TRUE(db.LastStatus() == TFDatabaseStatus::WriteFailed);
    TFAccountRecord missing;
    EXPECT_FALSE(db.FindAccountByUsername("rollback", missing));

    fs::remove_all(blocker);
    EXPECT_TRUE(db.CreateAccount("rollback", "salt", "hash", account));
    EXPECT_EQ(account.id, uint64_t{1});

    TFCharacterRecord character;
    EXPECT_TRUE(db.CreateCharacter(account.id, "RollbackChar", FactionId::MRA, character));
    EXPECT_TRUE(fs::create_directory(blocker));
    EXPECT_FALSE(db.TouchLogin(account.id, 4242));
    EXPECT_FALSE(db.SaveCharacterProgress(character.id, 900, 4, 75, 5151));

    TFAccountRecord unchangedAccount;
    TFCharacterRecord unchangedCharacter;
    EXPECT_TRUE(db.FindAccountByUsername("rollback", unchangedAccount));
    EXPECT_EQ(unchangedAccount.lastLoginMs, int64_t{0});
    EXPECT_TRUE(db.FindCharacter(character.id, unchangedCharacter));
    EXPECT_EQ(unchangedCharacter.xp, uint32_t{0});
    EXPECT_EQ(unchangedCharacter.rank, uint16_t{1});
    EXPECT_EQ(unchangedCharacter.flux, uint32_t{0});

    fs::remove_all(blocker);
    EXPECT_TRUE(db.Close());
    fs::remove(path);
}

TEST(TFDatabase_MalformedAdditiveSchemaCannotBeNormalizedByLaterWrites)
{
    namespace fs = std::filesystem;
    const fs::path path = fs::path("Saves") / "test_tfdb_bad_schema.db";
    fs::create_directories(path.parent_path());
    fs::remove(path);
    {
        std::ofstream out(path, std::ios::binary);
        out << R"json({
          "nextAccountId": 2,
          "nextCharId": 2,
          "accounts": [{"id":1,"username":"schema","salt":"salt","passwordHash":"hash",
                        "createdAtMs":1,"lastLoginMs":0}],
          "characters": [{"id":1,"accountId":1,"name":"SchemaChar","faction":1,
                          "xp":0,"rank":1,"flux":0,"createdAtMs":1,"lastPlayedMs":0,
                          "unlocks":[7]}]
        })json";
    }

    TFDatabase db;
    EXPECT_FALSE(db.Open(path));
    EXPECT_TRUE(db.LastStatus() == TFDatabaseStatus::Corrupt);
    EXPECT_TRUE(db.RecoveryLatched());
    EXPECT_FALSE(db.Open(path));
    EXPECT_TRUE(fs::exists(path));

    for (const auto& entry : fs::directory_iterator(path.parent_path()))
    {
        const std::string filename = entry.path().filename().string();
        if (filename.rfind("test_tfdb_bad_schema.db.corrupt-", 0) == 0)
            fs::remove(entry.path());
    }
    fs::remove(path);
}

TEST(TFDatabase_LexicallyRoundedIntegerAndDuplicateKeysAreCorrupt)
{
    namespace fs = std::filesystem;
    const fs::path dir = "Saves";
    fs::create_directories(dir);
    const auto expectRejected = [&](const char* leaf, const char* json)
    {
        const fs::path path = dir / leaf;
        fs::remove(path);
        for (const auto& entry : fs::directory_iterator(dir))
        {
            if (entry.path().filename().string().rfind(std::string(leaf) + ".corrupt-", 0) == 0)
                fs::remove(entry.path());
        }
        {
            std::ofstream out(path, std::ios::binary);
            out << json;
        }
        TFDatabase db;
        EXPECT_FALSE(db.Open(path));
        EXPECT_TRUE(db.LastStatus() == TFDatabaseStatus::Corrupt);
        for (const auto& entry : fs::directory_iterator(dir))
        {
            if (entry.path().filename().string().rfind(std::string(leaf) + ".corrupt-", 0) == 0)
                fs::remove(entry.path());
        }
        fs::remove(path);
    };

    expectRejected("test_tfdb_rounded_integer.db",
                   R"({"nextAccountId":1.0000000000000001,"nextCharId":1,"accounts":[],"characters":[]})");
    expectRejected("test_tfdb_duplicate_key.db",
                   R"({"nextAccountId":1,"nextAccountId":2,"nextCharId":1,"accounts":[],"characters":[]})");
}

TEST(TFDatabase_AllocatorExhaustionNeverPersistsAnUnsafeJsonInteger)
{
    namespace fs = std::filesystem;
    constexpr const char* kExhaustedId = "9007199254740991";
    const fs::path dir = "Saves";
    const fs::path path = dir / "test_tfdb_exhausted_allocator.db";
    fs::create_directories(dir);
    fs::remove(path);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << R"json({"nextAccountId":)json" << kExhaustedId << R"json(,"nextCharId":)json" << kExhaustedId
            << R"json(,"accounts":[{"id":1,"username":"seed","salt":"salt","passwordHash":"hash",
                      "createdAtMs":1,"lastLoginMs":0}],"characters":[]})json";
    }

    {
        TFDatabase db;
        EXPECT_TRUE(db.Open(path));
        TFAccountRecord account;
        TFCharacterRecord character;
        EXPECT_FALSE(db.CreateAccount("overflow", "salt", "hash", account));
        EXPECT_FALSE(db.CreateCharacter(1, "OverflowChar", FactionId::MRA, character));
        EXPECT_TRUE(db.Close());
    }
    // Failed allocations do not rewrite the file into a document the strict
    // loader rejects on restart.
    {
        TFDatabase reopened;
        EXPECT_TRUE(reopened.Open(path));
        TFAccountRecord seed;
        EXPECT_TRUE(reopened.FindAccountByUsername("seed", seed));
        EXPECT_EQ(reopened.ListCharacters(seed.id).size(), size_t{0});
        EXPECT_TRUE(reopened.Close());
    }
    fs::remove(path);

    const auto expectMaxIdRejected = [&](const char* leaf, const std::string& json)
    {
        const fs::path corruptPath = dir / leaf;
        fs::remove(corruptPath);
        for (const auto& entry : fs::directory_iterator(dir))
            if (entry.path().filename().string().rfind(std::string(leaf) + ".corrupt-", 0) == 0)
                fs::remove(entry.path());
        {
            std::ofstream out(corruptPath, std::ios::binary | std::ios::trunc);
            out << json;
        }
        TFDatabase db;
        EXPECT_FALSE(db.Open(corruptPath));
        EXPECT_TRUE(db.LastStatus() == TFDatabaseStatus::Corrupt);
        EXPECT_TRUE(fs::exists(corruptPath));
        for (const auto& entry : fs::directory_iterator(dir))
            if (entry.path().filename().string().rfind(std::string(leaf) + ".corrupt-", 0) == 0)
                fs::remove(entry.path());
        fs::remove(corruptPath);
    };

    expectMaxIdRejected(
        "test_tfdb_exhausted_legacy_account.db",
        std::string(R"json({"nextCharId":1,"accounts":[{"id":)json") + kExhaustedId +
            R"json(,"username":"seed","salt":"salt","passwordHash":"hash","createdAtMs":1,"lastLoginMs":0}],
                 "characters":[]})json");
    expectMaxIdRejected("test_tfdb_exhausted_legacy_character.db",
                        std::string(R"json({"nextAccountId":2,"accounts":[{"id":1,"username":"seed","salt":"salt",
                 "passwordHash":"hash","createdAtMs":1,"lastLoginMs":0}],"characters":[{"id":)json") +
                            kExhaustedId +
                            R"json(,"accountId":1,"name":"LegacyMax","faction":1,"xp":0,"rank":1,"flux":0,
                 "createdAtMs":1,"lastPlayedMs":0}]})json");
}

TEST(TFDatabase_MissingPrimaryWithRecoveryBackupFailsClosed)
{
    namespace fs = std::filesystem;
    const fs::path path = fs::path("Saves") / "test_tfdb_missing_primary.db";
    const fs::path backup = fs::path(path.string() + ".corrupt-legacy.bak");
    fs::remove(path);
    fs::remove(backup);
    {
        std::ofstream out(backup, std::ios::binary);
        out << "corrupt legacy primary";
    }

    TFDatabase db;
    EXPECT_FALSE(db.Open(path));
    EXPECT_TRUE(db.LastStatus() == TFDatabaseStatus::Corrupt);
    EXPECT_TRUE(db.RecoveryLatched());
    fs::remove(backup);
}

TEST(TFDatabase_ExclusivePersistenceLockRejectsSecondAuthority)
{
    namespace fs = std::filesystem;
    const fs::path path = fs::path("Saves") / "test_tfdb_exclusive_lock.db";
    fs::remove(path);

    TFDatabase first;
    TFDatabase second;
    EXPECT_TRUE(first.Open(path));
    EXPECT_FALSE(second.Open(path));
    EXPECT_TRUE(second.LastStatus() == TFDatabaseStatus::Locked);
    EXPECT_TRUE(first.Close());
    EXPECT_TRUE(second.Open(path));
    EXPECT_TRUE(second.Close());

    fs::remove(path);
    fs::remove(fs::path(path.wstring() + L".lock"));
}

TEST(TFDatabase_CloseOnlyReleasesLockAfterEagerCommit)
{
    namespace fs = std::filesystem;
    const fs::path path = fs::path("Saves") / "test_tfdb_close_retry.db";
    const fs::path blocker(path.wstring() + L".tmp");
    fs::remove(path);
    fs::remove_all(blocker);

    TFDatabase db;
    TFDatabase contender;
    EXPECT_TRUE(db.Open(path));
    TFAccountRecord created;
    EXPECT_TRUE(db.CreateAccount("close_commit", "salt", "hash", created));

    // A directory at the atomic writer's temporary path would make any
    // redundant close-time rewrite fail. Close must not need one because the
    // successful mutation above is already durable.
    EXPECT_TRUE(fs::create_directory(blocker));
    EXPECT_TRUE(db.Close());
    EXPECT_FALSE(db.IsOpen());
    EXPECT_TRUE(contender.Open(path));
    TFAccountRecord loaded;
    EXPECT_TRUE(contender.FindAccountByUsername("close_commit", loaded));
    EXPECT_EQ(loaded.id, created.id);
    EXPECT_TRUE(contender.Close());

    fs::remove_all(blocker);
    fs::remove(path);
}

TEST(TFPlayerMeta_FailedDisconnectFlushIsKeyedByCharacterNotRecycledPlayer)
{
    namespace fs = std::filesystem;
    const fs::path path = fs::path("Saves") / "test_tfmeta_disconnect.db";
    const fs::path blocker(path.wstring() + L".tmp");
    fs::remove(path);
    fs::remove_all(blocker);

    TFDatabase db;
    EXPECT_TRUE(db.Open(path));
    TFAccountRecord account;
    TFCharacterRecord character;
    EXPECT_TRUE(db.CreateAccount("meta_retry", "salt", "hash", account));
    EXPECT_TRUE(db.CreateCharacter(account.id, "MetaRetry", FactionId::MRA, character));

    TFPlayerMetaStore store;
    constexpr PlayerId oldPlayer = 41;
    constexpr PlayerId recycledPlayer = 42;
    store.SeedFromRecord(oldPlayer, character);
    store.Ensure(oldPlayer).unlocks.insert("retry_unlock");
    store.Ensure(oldPlayer).dirty = true;

    EXPECT_TRUE(fs::create_directory(blocker));
    EXPECT_FALSE(store.Detach(oldPlayer, &db));
    EXPECT_TRUE(store.Find(oldPlayer) == nullptr);
    EXPECT_TRUE(store.AnyDirty());

    store.SeedFromRecord(recycledPlayer, character);
    const TFPlayerMetaStore::Meta* restored = store.Find(recycledPlayer);
    EXPECT_TRUE(restored != nullptr);
    if (restored)
    {
        EXPECT_TRUE(restored->charId == character.id);
        EXPECT_TRUE(restored->unlocks.contains("retry_unlock"));
        EXPECT_TRUE(restored->dirty);
    }

    fs::remove_all(blocker);
    EXPECT_TRUE(store.Detach(recycledPlayer, &db));
    EXPECT_FALSE(store.AnyDirty());
    EXPECT_TRUE(db.Close());
    fs::remove(path);
}

TEST(TFDatabase_UnicodePathRoundTrip)
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::path("Saves") / L"terrafront_保存_тест";
    const fs::path path = dir / L"账户.db";
    fs::remove_all(dir);

    TFDatabase db;
    EXPECT_TRUE(db.Open(path));
    TFAccountRecord account;
    EXPECT_TRUE(db.CreateAccount("unicodepath", "salt", "hash", account));
    EXPECT_TRUE(db.Close());

    TFDatabase reopened;
    EXPECT_TRUE(reopened.Open(path));
    TFAccountRecord loaded;
    EXPECT_TRUE(reopened.FindAccountByUsername("unicodepath", loaded));
    EXPECT_TRUE(reopened.Close());
    fs::remove_all(dir);
}

TEST(TFWorldSave_DistinguishesMissingUnreadableCorruptAndContinent)
{
    namespace fs = std::filesystem;
    using Terrafront::WorldSave::ReadStatus;
    const fs::path dir = fs::path("Saves") / "test_tf_world_status";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Spark::Json::Value root;
    std::string detail;

    EXPECT_TRUE(Terrafront::WorldSave::ReadJson(dir / "missing.json", "cindral_wastes", "Cindral Wastes", false, root,
                                                detail) == ReadStatus::Missing);
    EXPECT_TRUE(Terrafront::WorldSave::ReadJson(dir, "cindral_wastes", "Cindral Wastes", false, root, detail) ==
                ReadStatus::Unreadable);

    {
        std::ofstream out(dir / "corrupt.json", std::ios::binary);
        out << "{truncated";
    }
    EXPECT_TRUE(Terrafront::WorldSave::ReadJson(dir / "corrupt.json", "cindral_wastes", "Cindral Wastes", false, root,
                                                detail) == ReadStatus::Corrupt);

    {
        std::ofstream out(dir / "other.json", std::ios::binary);
        out << R"({"continentKey":"veyra_highlands"})";
    }
    EXPECT_TRUE(Terrafront::WorldSave::ReadJson(dir / "other.json", "cindral_wastes", "Cindral Wastes", false, root,
                                                detail) == ReadStatus::WrongContinent);

    {
        std::ofstream out(dir / "bad-key-type.json", std::ios::binary);
        out << R"({"continentKey":42,"continent":"Cindral Wastes"})";
    }
    EXPECT_TRUE(Terrafront::WorldSave::ReadJson(dir / "bad-key-type.json", "cindral_wastes", "Cindral Wastes", true,
                                                root, detail) == ReadStatus::Corrupt);

    {
        std::ofstream out(dir / "rounded-integer.json", std::ios::binary);
        out << R"({"continentKey":"cindral_wastes","regionCount":1.0000000000000001})";
    }
    EXPECT_TRUE(Terrafront::WorldSave::ReadJson(dir / "rounded-integer.json", "cindral_wastes", "Cindral Wastes", true,
                                                root, detail) == ReadStatus::Corrupt);
    {
        std::ofstream out(dir / "duplicate-key.json", std::ios::binary);
        out << R"({"continentKey":"cindral_wastes","continentKey":"veyra_highlands"})";
    }
    EXPECT_TRUE(Terrafront::WorldSave::ReadJson(dir / "duplicate-key.json", "cindral_wastes", "Cindral Wastes", true,
                                                root, detail) == ReadStatus::Corrupt);

    {
        std::ofstream out(dir / "legacy.json", std::ios::binary);
        out << R"({"continent":"Cindral Wastes"})";
    }
    EXPECT_TRUE(Terrafront::WorldSave::ReadJson(dir / "legacy.json", "cindral_wastes", "Cindral Wastes", true, root,
                                                detail) == ReadStatus::Loaded);
    EXPECT_TRUE(Terrafront::WorldSave::ReadJson(dir / "legacy.json", "veyra_highlands", "Veyra Highlands", true, root,
                                                detail) == ReadStatus::WrongContinent);

    uint32_t parsed = 0;
    EXPECT_TRUE(Terrafront::WorldSave::ReadUint32(Spark::Json::Value(17), parsed));
    EXPECT_EQ(parsed, uint32_t{17});
    EXPECT_FALSE(Terrafront::WorldSave::ReadUint32(Spark::Json::Value(-1), parsed));
    EXPECT_FALSE(Terrafront::WorldSave::ReadUint32(Spark::Json::Value(1.5), parsed));
    EXPECT_FALSE(Terrafront::WorldSave::ReadUint32(Spark::Json::Value("17"), parsed));
    fs::remove_all(dir);
}

TEST(TFWorldSave_DominionV1RequiresCanonicalCompleteState)
{
    using Spark::Json::Parse;
    Terrafront::WorldSave::DominionState state;
    constexpr uint32_t factionCount = static_cast<uint32_t>(FactionId::COUNT);

    EXPECT_TRUE(Terrafront::WorldSave::ReadDominionState(Parse(R"({"active":true,"faction":1,"remainingSec":45.5})"),
                                                         true, factionCount, state));
    EXPECT_TRUE(state.active);
    EXPECT_EQ(state.faction, uint32_t{1});
    EXPECT_NEAR(state.remainingSec, 45.5, 0.001);

    EXPECT_TRUE(Terrafront::WorldSave::ReadDominionState(Parse(R"({"active":false,"faction":0,"remainingSec":0})"),
                                                         true, factionCount, state));
    EXPECT_FALSE(state.active);
    EXPECT_FALSE(Terrafront::WorldSave::ReadDominionState(Parse(R"({"faction":0,"remainingSec":0})"), true,
                                                          factionCount, state));
    EXPECT_FALSE(Terrafront::WorldSave::ReadDominionState(Parse(R"({"active":false})"), true, factionCount, state));
    EXPECT_FALSE(Terrafront::WorldSave::ReadDominionState(Parse(R"({"active":false,"faction":"bad","remainingSec":0})"),
                                                          true, factionCount, state));
    EXPECT_FALSE(Terrafront::WorldSave::ReadDominionState(Parse(R"({"active":false,"faction":0,"remainingSec":"bad"})"),
                                                          true, factionCount, state));
    EXPECT_FALSE(Terrafront::WorldSave::ReadDominionState(Parse(R"({"active":false,"faction":1,"remainingSec":0})"),
                                                          true, factionCount, state));
    EXPECT_FALSE(Terrafront::WorldSave::ReadDominionState(Parse(R"({"active":false,"faction":0,"remainingSec":1})"),
                                                          true, factionCount, state));
}

TEST(TFWorldSave_DominionLegacyAllowsMissingInactiveFieldsOnly)
{
    using Spark::Json::Parse;
    Terrafront::WorldSave::DominionState state;
    constexpr uint32_t factionCount = static_cast<uint32_t>(FactionId::COUNT);

    EXPECT_TRUE(Terrafront::WorldSave::ReadDominionState(Parse(R"({"active":false})"), false, factionCount, state));
    EXPECT_FALSE(state.active);
    EXPECT_FALSE(Terrafront::WorldSave::ReadDominionState(Parse(R"({"active":true})"), false, factionCount, state));
    EXPECT_FALSE(Terrafront::WorldSave::ReadDominionState(Parse(R"({"active":true,"faction":0,"remainingSec":10})"),
                                                          false, factionCount, state));
}

TEST(TFPersistence_ShutdownFlushPrecedesDatabaseClose)
{
    struct FakeProgression
    {
        int& order;
        bool Shutdown()
        {
            order = order * 10 + 1;
            return true;
        }
    };
    struct FakeDatabase
    {
        int& order;
        bool Close()
        {
            order = order * 10 + 2;
            return true;
        }
    };

    int order = 0;
    FakeProgression progression{order};
    FakeDatabase database{order};
    EXPECT_TRUE(Terrafront::FlushProgressionThenCloseDatabase(progression, database));
    EXPECT_EQ(order, 12);
}

TEST(TFPersistence_CheckpointGateRunsEveryStoreAndAggregatesFailure)
{
    struct FakeStore
    {
        int calls = 0;
        bool succeeds = true;
        bool Checkpoint()
        {
            ++calls;
            return succeeds;
        }
    };

    FakeStore social;
    FakeStore outfit;
    FakeStore progression;
    FakeStore region;
    outfit.succeeds = false;

    EXPECT_FALSE(Terrafront::CheckpointPersistenceBeforeTeardown(social, outfit, progression, region));
    EXPECT_EQ(social.calls, 1);
    EXPECT_EQ(outfit.calls, 1);
    EXPECT_EQ(progression.calls, 1);
    EXPECT_EQ(region.calls, 1);
}

TEST(TFPersistence_ShutdownFailureKeepsDatabaseOpenForRetry)
{
    struct FakeProgression
    {
        bool Shutdown() { return false; }
    };
    struct FakeDatabase
    {
        bool closed = false;
        bool Close()
        {
            closed = true;
            return true;
        }
    };

    FakeProgression progression;
    FakeDatabase database;
    EXPECT_FALSE(Terrafront::FlushProgressionThenCloseDatabase(progression, database));
    EXPECT_FALSE(database.closed);
}

TEST(TFCharacterSystem_CRUD_SlotCap_EnterWorld)
{
    namespace fs = std::filesystem;
    const std::string path = "Saves/test_tfchar.db";
    fs::remove(path);

    TFDatabase db;
    EXPECT_TRUE(db.Open(path));
    TFAccountRecord a;
    EXPECT_TRUE(db.CreateAccount("cmd", "s", "h", a));
    TFCharacterSystem cs;
    cs.SetDatabase(&db);

    // Fill all 5 slots with unique names.
    for (int i = 0; i < kTFMaxCharSlots; ++i)
        EXPECT_TRUE(cs.Create(a.id, std::string("Char") + char('A' + i), FactionId::AUC).ok);

    // 6th create (new name) hits the slot cap.
    EXPECT_TRUE(cs.Create(a.id, "OneMore", FactionId::AUC).err == TFCharErr::SlotsFull);

    auto list = cs.List(a.id);
    EXPECT_EQ(list.size(), (size_t)kTFMaxCharSlots);

    // Ownership is checked before the delete takes effect.
    EXPECT_TRUE(cs.Delete(a.id + 999, list[0].id) == TFCharErr::NotYourCharacter);
    EXPECT_EQ(cs.List(a.id).size(), (size_t)kTFMaxCharSlots); // unchanged

    // Delete own character frees a slot.
    EXPECT_TRUE(cs.Delete(a.id, list[0].id) == TFCharErr::Ok);
    EXPECT_EQ(cs.List(a.id).size(), (size_t)(kTFMaxCharSlots - 1));

    // Deleting an already-deleted / unknown id.
    EXPECT_TRUE(cs.Delete(a.id, list[0].id) == TFCharErr::NoSuchCharacter);

    // With a slot free, a duplicate name (one of the survivors) is still rejected.
    EXPECT_TRUE(cs.Create(a.id, list[1].name, FactionId::AUC).err == TFCharErr::NameTaken);

    // Invalid name (too short).
    EXPECT_TRUE(cs.Create(a.id, "AB", FactionId::AUC).err == TFCharErr::NameInvalid);

    // Invalid faction.
    EXPECT_TRUE(cs.Create(a.id, "ValidName", FactionId::None).err == TFCharErr::NameInvalid);

    // Enter world with the right owner returns the record with its faction.
    TFCharacterRecord ew;
    EXPECT_TRUE(cs.EnterWorld(a.id, list[1].id, ew));
    EXPECT_TRUE(ew.faction == FactionId::AUC);
    EXPECT_TRUE(ew.name == list[1].name);

    // Enter world with the wrong owner fails.
    TFCharacterRecord ew2;
    EXPECT_FALSE(cs.EnterWorld(a.id + 999, list[1].id, ew2));

    // Progression persists and survives a reopen.
    cs.PersistProgress(list[1].id, 999, 3, 42);
    db.Close();
    {
        TFDatabase db2;
        EXPECT_TRUE(db2.Open(path));
        TFCharacterSystem cs2;
        cs2.SetDatabase(&db2);
        TFCharacterRecord reread;
        EXPECT_TRUE(db2.FindCharacter(list[1].id, reread));
        EXPECT_EQ((int)reread.xp, 999);
        EXPECT_EQ((int)reread.rank, 3);
        EXPECT_EQ((int)reread.flux, 42);
        db2.Close();
    }

    fs::remove(path);
}

TEST(TFCharacterSystem_ValidateCharacterName)
{
    std::string err;
    EXPECT_TRUE(TFCharacterSystem::ValidateCharacterName("Vanguard", err));
    EXPECT_TRUE(TFCharacterSystem::ValidateCharacterName("War Chief", err));
    EXPECT_FALSE(TFCharacterSystem::ValidateCharacterName("", err));
    EXPECT_FALSE(TFCharacterSystem::ValidateCharacterName("AB", err));                 // too short
    EXPECT_FALSE(TFCharacterSystem::ValidateCharacterName(std::string(24, 'A'), err)); // too long (24 > 23)
    EXPECT_TRUE(TFCharacterSystem::ValidateCharacterName(std::string(23, 'A'), err));  // exactly 23 ok
    EXPECT_FALSE(TFCharacterSystem::ValidateCharacterName(" Leading", err));
    EXPECT_FALSE(TFCharacterSystem::ValidateCharacterName("Trailing ", err));
    EXPECT_FALSE(TFCharacterSystem::ValidateCharacterName("Double  Space", err));
    EXPECT_FALSE(TFCharacterSystem::ValidateCharacterName("Bad_Name", err)); // invalid char
}

// ============================================================================
// Task 4: net protocol wire layout (message ids + packed POD sizes) + a
// memcpy round-trip through a raw byte buffer, mirroring
// Tests/TestTFNetProtocolLayout.cpp's convention for the pre-W5 messages.
// ============================================================================

static_assert(static_cast<uint16_t>(TFMsg::LoginRequest) == 0x5412);
static_assert(static_cast<uint16_t>(TFMsg::LoginReply) == 0x5413);
static_assert(static_cast<uint16_t>(TFMsg::RegisterRequest) == 0x5414);
static_assert(static_cast<uint16_t>(TFMsg::RegisterReply) == 0x5415);
static_assert(static_cast<uint16_t>(TFMsg::CharListRequest) == 0x5416);
static_assert(static_cast<uint16_t>(TFMsg::CharListReply) == 0x5417);
static_assert(static_cast<uint16_t>(TFMsg::CharCreateReq) == 0x5418);
static_assert(static_cast<uint16_t>(TFMsg::CharCreateReply) == 0x5419);
static_assert(static_cast<uint16_t>(TFMsg::CharDeleteReq) == 0x541A);
static_assert(static_cast<uint16_t>(TFMsg::CharDeleteReply) == 0x541B);
static_assert(static_cast<uint16_t>(TFMsg::EnterWorldReq) == 0x541C);

// Compiler-verified sizes (Task 4 explicitly forbids guessing these — every
// number below was read back from an actual build, not hand-computed).
static_assert(sizeof(TF_AuthRequest) == 96, "wire layout frozen");
static_assert(sizeof(TF_AuthReply) == 12, "wire layout frozen");
static_assert(sizeof(TF_CharBrief) == 36, "wire layout frozen");
static_assert(sizeof(TF_CharListReply) == 4 + 5 * 36, "wire layout frozen");
static_assert(sizeof(TF_CharCreateRequest) == 28, "wire layout frozen");
static_assert(sizeof(TF_CharOpReply) == 12, "wire layout frozen");
static_assert(sizeof(TF_CharDeleteRequest) == 8, "wire layout frozen");
static_assert(sizeof(TF_EnterWorldRequest) == 8, "wire layout frozen");

namespace
{
    template <typename T> T OnboardingWireRoundTrip(const T& in)
    {
        unsigned char wire[sizeof(T)];
        std::memcpy(wire, &in, sizeof(T));
        T out{};
        std::memcpy(&out, wire, sizeof(T));
        return out;
    }
} // namespace

TEST(TFNetProtocol_Onboarding_AuthMessages_RoundTrip)
{
    TF_AuthRequest req{};
    std::strncpy(req.user, "commander", sizeof(req.user) - 1);
    std::strncpy(req.pass, "sekret1", sizeof(req.pass) - 1);
    const TF_AuthRequest req2 = OnboardingWireRoundTrip(req);
    EXPECT_TRUE(std::string(req2.user) == "commander");
    EXPECT_TRUE(std::string(req2.pass) == "sekret1");

    TF_AuthReply rep{};
    rep.ok = 1;
    rep.err = static_cast<uint8_t>(TFAuthErr::Ok);
    rep.accountId = 4242;
    const TF_AuthReply rep2 = OnboardingWireRoundTrip(rep);
    EXPECT_EQ(static_cast<int>(rep2.ok), 1);
    EXPECT_EQ(rep2.accountId, (uint64_t)4242);
}

TEST(TFNetProtocol_Onboarding_CharMessages_RoundTrip)
{
    TF_CharListReply lst{};
    lst.count = 2;
    lst.chars[0].id = 7;
    std::strncpy(lst.chars[0].name, "Vanguard", sizeof(lst.chars[0].name) - 1);
    lst.chars[0].faction = static_cast<uint8_t>(FactionId::MRA);
    lst.chars[0].rank = 3;
    lst.chars[1].id = 9;
    std::strncpy(lst.chars[1].name, "Ironclad", sizeof(lst.chars[1].name) - 1);
    lst.chars[1].faction = static_cast<uint8_t>(FactionId::AUC);
    lst.chars[1].rank = 1;
    const TF_CharListReply lst2 = OnboardingWireRoundTrip(lst);
    EXPECT_EQ(static_cast<int>(lst2.count), 2);
    EXPECT_TRUE(std::string(lst2.chars[0].name) == "Vanguard");
    EXPECT_EQ(lst2.chars[1].id, (uint64_t)9);
    EXPECT_EQ(static_cast<int>(lst2.chars[1].faction), static_cast<int>(FactionId::AUC));

    TF_CharCreateRequest cc{};
    std::strncpy(cc.name, "War Chief", sizeof(cc.name) - 1);
    cc.faction = static_cast<uint8_t>(FactionId::HLX);
    const TF_CharCreateRequest cc2 = OnboardingWireRoundTrip(cc);
    EXPECT_TRUE(std::string(cc2.name) == "War Chief");
    EXPECT_EQ(static_cast<int>(cc2.faction), static_cast<int>(FactionId::HLX));

    TF_CharOpReply op{};
    op.ok = 1;
    op.err = static_cast<uint8_t>(TFCharErr::Ok);
    op.charId = 55;
    EXPECT_EQ(OnboardingWireRoundTrip(op).charId, (uint64_t)55);

    TF_CharDeleteRequest del{};
    del.charId = 66;
    EXPECT_EQ(OnboardingWireRoundTrip(del).charId, (uint64_t)66);

    TF_EnterWorldRequest ew{};
    ew.charId = 77;
    EXPECT_EQ(OnboardingWireRoundTrip(ew).charId, (uint64_t)77);
}
