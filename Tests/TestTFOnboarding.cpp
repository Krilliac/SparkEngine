/**
 * @file TestTFOnboarding.cpp
 * @brief TERRAFRONT W5 onboarding tests: TFDatabase (Task 1), TFAccountSystem
 *        (Task 2), TFCharacterSystem (Task 3), and the Task 4 net-protocol
 *        wire-layout freeze for the new onboarding TFMsg PODs (independent of
 *        TestTFNetProtocolLayout.cpp's coverage of the pre-W5 messages).
 */
#include "TestFramework.h"
#include "Persistence/TFDatabase.h"
#include "Account/TFAccountSystem.h"
#include "Account/TFCharacterSystem.h"
#include "Account/TFCrypto.h"
#include "Net/TFNetProtocol.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace Terrafront;

TEST(TFDatabase_AccountCharacter_RoundTrip)
{
    namespace fs = std::filesystem;
    const std::string path = "Saves/test_tfdb_roundtrip.db";
    fs::remove(path);
    uint64_t acctId=0, charId=0;
    {
        TFDatabase db; EXPECT_FALSE(db.IsOpen()); EXPECT_TRUE(db.Open(path)); EXPECT_TRUE(db.IsOpen());
        TFAccountRecord a;
        EXPECT_TRUE(db.CreateAccount("commander", "abc123", "hashed", a));
        acctId = a.id; EXPECT_TRUE(acctId != 0);
        EXPECT_FALSE(db.CreateAccount("commander", "x", "y", a)); // username taken
        TFCharacterRecord c;
        EXPECT_TRUE(db.CreateCharacter(acctId, "Vanguard", FactionId::MRA, c));
        charId = c.id; EXPECT_TRUE(charId != 0);
        db.SaveCharacterProgress(charId, 4200, 7, 150, 111);
        db.Close(); EXPECT_FALSE(db.IsOpen());
    }
    { // reopen a fresh instance -> data survived
        TFDatabase db; EXPECT_TRUE(db.Open(path));
        TFAccountRecord a; EXPECT_TRUE(db.FindAccountByUsername("commander", a));
        EXPECT_EQ(a.id, acctId); EXPECT_TRUE(a.passwordHash == "hashed"); EXPECT_TRUE(a.salt == "abc123");
        auto chars = db.ListCharacters(acctId);
        EXPECT_EQ(chars.size(), (size_t)1);
        TFCharacterRecord c; EXPECT_TRUE(db.FindCharacter(charId, c));
        EXPECT_TRUE(c.name == "Vanguard"); EXPECT_TRUE(c.faction == FactionId::MRA);
        EXPECT_EQ((int)c.xp, 4200); EXPECT_EQ((int)c.rank, 7); EXPECT_EQ((int)c.flux, 150);
        db.Close();
    }
    fs::remove(path);
}

TEST(TFAccountSystem_Register_Login_HashVerify)
{
    namespace fs = std::filesystem; const std::string path="Saves/test_tfacct.db"; fs::remove(path);
    TFDatabase db; EXPECT_TRUE(db.Open(path));
    TFAccountSystem acct; acct.SetDatabase(&db);
    auto r = acct.Register("commander", "sekret1!");
    EXPECT_TRUE(r.ok); EXPECT_TRUE(r.accountId != 0);
    EXPECT_FALSE(acct.Register("commander", "other456").ok);          // taken
    EXPECT_TRUE(acct.Register("x", "pw123456").err == TFAuthErr::UsernameTooShort);
    EXPECT_TRUE(acct.Login("commander","sekret1!").ok);             // correct
    auto bad = acct.Login("commander","wrong123");
    EXPECT_FALSE(bad.ok); EXPECT_TRUE(bad.err == TFAuthErr::BadCredentials);
    EXPECT_TRUE(acct.Login("ghost","x").err == TFAuthErr::BadCredentials); // no such user
    db.Close(); fs::remove(path);
}

TEST(TFAccountSystem_PasswordHardening)
{
    namespace fs = std::filesystem; const std::string path="Saves/test_tfacct_hardening.db"; fs::remove(path);
    TFDatabase db; EXPECT_TRUE(db.Open(path));
    TFAccountSystem acct; acct.SetDatabase(&db);

    // Empty and too-short passwords are rejected with a distinct error code.
    auto emptyPw = acct.Register("validuser", "");
    EXPECT_FALSE(emptyPw.ok); EXPECT_TRUE(emptyPw.err == TFAuthErr::PasswordTooShort);
    auto shortPw = acct.Register("validuser", "abcd");
    EXPECT_FALSE(shortPw.ok); EXPECT_TRUE(shortPw.err == TFAuthErr::PasswordTooShort);

    // A >=8 char password succeeds.
    auto okPw = acct.Register("validuser", "longenough1");
    EXPECT_TRUE(okPw.ok); EXPECT_TRUE(okPw.accountId != 0);

    // HashPassword is not a bare single std::hash pass, is salt-sensitive, and deterministic.
    std::string bareHash = std::to_string(std::hash<std::string>{}(std::string("abc")));
    EXPECT_TRUE(TFAccountSystem::HashPassword("abc", "salt") != bareHash);
    EXPECT_TRUE(TFAccountSystem::HashPassword("samepw", "salt1") != TFAccountSystem::HashPassword("samepw", "salt2"));
    EXPECT_TRUE(TFAccountSystem::HashPassword("samepw", "saltX") == TFAccountSystem::HashPassword("samepw", "saltX"));

    // Regression: login still round-trips correctly after the HashPassword change.
    EXPECT_TRUE(acct.Login("validuser", "longenough1").ok);
    EXPECT_FALSE(acct.Login("validuser", "wrongpassword").ok);

    db.Close(); fs::remove(path);
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
    auto mac = Terrafront::Crypto::HmacSha256(key.data(), key.size(),
                                               reinterpret_cast<const uint8_t*>(data.data()), data.size());
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
    EXPECT_TRUE(Terrafront::Crypto::ToHex(dk) ==
                "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
}

TEST(TFCrypto_Pbkdf2_SelfConsistency)
{
    const std::vector<uint8_t> saltA{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    const std::vector<uint8_t> saltB{16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1};

    auto d1 = Terrafront::Crypto::Pbkdf2HmacSha256("hunter2", saltA, 1000, 32);
    auto d2 = Terrafront::Crypto::Pbkdf2HmacSha256("hunter2", saltA, 1000, 32);
    EXPECT_TRUE(d1 == d2);          // deterministic: same inputs -> same output

    auto d3 = Terrafront::Crypto::Pbkdf2HmacSha256("hunter2", saltB, 1000, 32);
    EXPECT_TRUE(!(d1 == d3));       // salt-sensitive: different salt -> different output
}

TEST(TFAccountSystem_PBKDF2Scheme_StoredHashAndVerify)
{
    namespace fs = std::filesystem;
    const std::string path = "Saves/test_tfacct_pbkdf2.db";
    fs::remove(path);
    TFDatabase db; EXPECT_TRUE(db.Open(path));
    TFAccountSystem acct; acct.SetDatabase(&db);

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
        EXPECT_TRUE(iters >= 100000);   // hardening floor
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

    db.Close(); fs::remove(path);
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
    EXPECT_FALSE(db.Open(path));   // refuses to open a corrupt existing file

    // The original file was renamed away (quarantined), not silently wiped.
    EXPECT_FALSE(fs::exists(path));

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
                std::ostringstream ss; ss << in.rdbuf();
                backupContents = ss.str();
            }   // close the handle before removing (Windows locks open files)
            fs::remove(entry.path());
            break;
        }
    }
    EXPECT_TRUE(foundBackup);
    EXPECT_TRUE(backupContents == garbage);   // original bytes preserved verbatim

    fs::remove(path);
}

TEST(TFCharacterSystem_CRUD_SlotCap_EnterWorld)
{
    namespace fs = std::filesystem;
    const std::string path = "Saves/test_tfchar.db";
    fs::remove(path);

    TFDatabase db; EXPECT_TRUE(db.Open(path));
    TFAccountRecord a; EXPECT_TRUE(db.CreateAccount("cmd", "s", "h", a));
    TFCharacterSystem cs; cs.SetDatabase(&db);

    // Fill all 5 slots with unique names.
    for (int i = 0; i < kTFMaxCharSlots; ++i)
        EXPECT_TRUE(cs.Create(a.id, std::string("Char") + char('A' + i), FactionId::AUC).ok);

    // 6th create (new name) hits the slot cap.
    EXPECT_TRUE(cs.Create(a.id, "OneMore", FactionId::AUC).err == TFCharErr::SlotsFull);

    auto list = cs.List(a.id);
    EXPECT_EQ(list.size(), (size_t)kTFMaxCharSlots);

    // Ownership is checked before the delete takes effect.
    EXPECT_TRUE(cs.Delete(a.id + 999, list[0].id) == TFCharErr::NotYourCharacter);
    EXPECT_EQ(cs.List(a.id).size(), (size_t)kTFMaxCharSlots);   // unchanged

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
        TFDatabase db2; EXPECT_TRUE(db2.Open(path));
        TFCharacterSystem cs2; cs2.SetDatabase(&db2);
        TFCharacterRecord reread;
        EXPECT_TRUE(db2.FindCharacter(list[1].id, reread));
        EXPECT_EQ((int)reread.xp, 999); EXPECT_EQ((int)reread.rank, 3); EXPECT_EQ((int)reread.flux, 42);
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
    EXPECT_FALSE(TFCharacterSystem::ValidateCharacterName("AB", err));                       // too short
    EXPECT_FALSE(TFCharacterSystem::ValidateCharacterName(std::string(24, 'A'), err));        // too long (24 > 23)
    EXPECT_TRUE(TFCharacterSystem::ValidateCharacterName(std::string(23, 'A'), err));         // exactly 23 ok
    EXPECT_FALSE(TFCharacterSystem::ValidateCharacterName(" Leading", err));
    EXPECT_FALSE(TFCharacterSystem::ValidateCharacterName("Trailing ", err));
    EXPECT_FALSE(TFCharacterSystem::ValidateCharacterName("Double  Space", err));
    EXPECT_FALSE(TFCharacterSystem::ValidateCharacterName("Bad_Name", err));                  // invalid char
}

// ============================================================================
// Task 4: net protocol wire layout (message ids + packed POD sizes) + a
// memcpy round-trip through a raw byte buffer, mirroring
// Tests/TestTFNetProtocolLayout.cpp's convention for the pre-W5 messages.
// ============================================================================

static_assert(static_cast<uint16_t>(TFMsg::LoginRequest)    == 0x5412);
static_assert(static_cast<uint16_t>(TFMsg::LoginReply)      == 0x5413);
static_assert(static_cast<uint16_t>(TFMsg::RegisterRequest) == 0x5414);
static_assert(static_cast<uint16_t>(TFMsg::RegisterReply)   == 0x5415);
static_assert(static_cast<uint16_t>(TFMsg::CharListRequest) == 0x5416);
static_assert(static_cast<uint16_t>(TFMsg::CharListReply)   == 0x5417);
static_assert(static_cast<uint16_t>(TFMsg::CharCreateReq)   == 0x5418);
static_assert(static_cast<uint16_t>(TFMsg::CharCreateReply) == 0x5419);
static_assert(static_cast<uint16_t>(TFMsg::CharDeleteReq)   == 0x541A);
static_assert(static_cast<uint16_t>(TFMsg::CharDeleteReply) == 0x541B);
static_assert(static_cast<uint16_t>(TFMsg::EnterWorldReq)   == 0x541C);

// Compiler-verified sizes (Task 4 explicitly forbids guessing these — every
// number below was read back from an actual build, not hand-computed).
static_assert(sizeof(TF_AuthRequest)       == 96,       "wire layout frozen");
static_assert(sizeof(TF_AuthReply)         == 12,       "wire layout frozen");
static_assert(sizeof(TF_CharBrief)         == 36,       "wire layout frozen");
static_assert(sizeof(TF_CharListReply)     == 4 + 5*36, "wire layout frozen");
static_assert(sizeof(TF_CharCreateRequest) == 28,       "wire layout frozen");
static_assert(sizeof(TF_CharOpReply)       == 12,       "wire layout frozen");
static_assert(sizeof(TF_CharDeleteRequest) == 8,        "wire layout frozen");
static_assert(sizeof(TF_EnterWorldRequest) == 8,        "wire layout frozen");

namespace
{
    template <typename T>
    T OnboardingWireRoundTrip(const T& in)
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
