/**
 * @file TestTFOnboarding.cpp
 * @brief TERRAFRONT W5 onboarding tests (Tasks 1-3+: TFDatabase, TFAccountSystem,
 *        TFCharacterSystem). Task 1 adds the TFDatabase round-trip test only.
 */
#include "TestFramework.h"
#include "Persistence/TFDatabase.h"
#include "Account/TFAccountSystem.h"
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
        TFDatabase db; EXPECT_TRUE(db.Open(path));
        TFAccountRecord a;
        EXPECT_TRUE(db.CreateAccount("commander", "abc123", "hashed", a));
        acctId = a.id; EXPECT_TRUE(acctId != 0);
        EXPECT_FALSE(db.CreateAccount("commander", "x", "y", a)); // username taken
        TFCharacterRecord c;
        EXPECT_TRUE(db.CreateCharacter(acctId, "Vanguard", FactionId::MRA, c));
        charId = c.id; EXPECT_TRUE(charId != 0);
        db.SaveCharacterProgress(charId, 4200, 7, 150, 111);
        db.Close();
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
