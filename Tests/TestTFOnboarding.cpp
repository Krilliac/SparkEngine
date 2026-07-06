/**
 * @file TestTFOnboarding.cpp
 * @brief TERRAFRONT W5 onboarding tests (Tasks 1-3+: TFDatabase, TFAccountSystem,
 *        TFCharacterSystem). Task 1 adds the TFDatabase round-trip test only.
 */
#include "TestFramework.h"
#include "Persistence/TFDatabase.h"
#include "Account/TFAccountSystem.h"
#include "Account/TFCharacterSystem.h"
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
