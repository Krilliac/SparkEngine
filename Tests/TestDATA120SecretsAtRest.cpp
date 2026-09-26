/**
 * @file TestDATA120SecretsAtRest.cpp
 * @brief DATA-120 / OD-22: no plaintext password, session token, or database
 *        credential may reach persistent storage.
 *
 * OD-22 policy (docs/readiness/OWNER-DECISIONS.md): password material is
 * stored only as salted PBKDF2 hashes, session tokens are never persisted in
 * plaintext, and no database secret is committed or written to shipped
 * config. Encryption at rest is the operator's host full-disk encryption.
 *
 * Every test drives production code against real files: the TERRAFRONT
 * account store (TFAccountSystem over TFDatabase), the MMO account system
 * next to the MMO key-value store (MMOPersistenceSystem over AsyncDatabase),
 * and the config trees the install rules ship.
 */
#include "TestFramework.h"
#include "Account/MMOAccountSystem.h"
#include "Account/TFAccountSystem.h"
#include "Persistence/MMOPersistenceSystem.h"
#include "Persistence/TFDatabase.h"
#include "Utils/JsonUtils.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    constexpr uint32_t kMinimumPbkdf2Iterations = 100000;

    fs::path FreshPath(const char* name)
    {
        const fs::path path = fs::path("Saves") / name;
        fs::create_directories(path.parent_path());
        fs::remove(path);
        fs::remove(fs::path(path.string() + ".tmp"));
        return path;
    }

    std::string ReadFile(const fs::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    std::string ToLowerHex(const std::string& bytes)
    {
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string out;
        out.reserve(bytes.size() * 2);
        for (const unsigned char c : bytes)
        {
            out.push_back(kDigits[c >> 4]);
            out.push_back(kDigits[c & 0x0F]);
        }
        return out;
    }

    /// True when @p haystack holds @p secret verbatim or hex-encoded (either case).
    bool ContainsSecret(const std::string& haystack, const std::string& secret)
    {
        if (haystack.find(secret) != std::string::npos)
            return true;
        std::string lowered = haystack;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lowered.find(ToLowerHex(secret)) != std::string::npos;
    }

    bool IsLowerHex(const std::string& text)
    {
        return !text.empty() && std::all_of(text.begin(), text.end(),
                                            [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }

    std::vector<std::string> SplitOnDollar(const std::string& text)
    {
        std::vector<std::string> parts;
        std::string part;
        std::istringstream in(text);
        while (std::getline(in, part, '$'))
            parts.push_back(part);
        return parts;
    }

    /// OD-22 stored form: "pbkdf2-sha256$<iterations>$<saltHex>$<derivedKeyHex>".
    bool IsSaltedPbkdf2Hash(const std::string& stored, const std::string& salt)
    {
        const std::vector<std::string> parts = SplitOnDollar(stored);
        if (parts.size() != 4 || parts[0] != "pbkdf2-sha256")
            return false;
        if (parts[1].empty() ||
            !std::all_of(parts[1].begin(), parts[1].end(), [](char c) { return c >= '0' && c <= '9'; }))
            return false;
        const unsigned long long iterations = std::stoull(parts[1]);
        // 128-bit salt and a 256-bit derived key, both hex-encoded.
        return iterations >= kMinimumPbkdf2Iterations && parts[2] == salt && parts[2].size() >= 32 &&
               IsLowerHex(parts[2]) && parts[3].size() >= 64 && IsLowerHex(parts[3]);
    }

    Spark::Json::Value ParseStore(const fs::path& path)
    {
        Spark::Json::Value root;
        std::string error;
        if (!Spark::Json::ParseStrict(ReadFile(path), &root, &error))
            return Spark::Json::Value();
        return root;
    }

    // ----------------------------------------------------------------------
    // Shipped-config credential scan
    // ----------------------------------------------------------------------

    /// A key/value assignment that names a credential, or a URL carrying
    /// "user:password@". Matches both INI (`db_password = x`) and JSON
    /// (`"sessionToken": "x"`) shapes.
    bool LineCarriesCredential(const std::string& line)
    {
        static const std::regex kCredentialKey(
            R"((^|[^a-z0-9])((db|database|sql|sqlite|mysql|postgres|postgresql|pg|redis|mongo|mongodb)[_.-]?)?)"
            R"((password|passwd|pwd|passphrase|secret|credentials?|connection[_-]?string|connstr|dsn|)"
            R"(api[_-]?key|access[_-]?token|auth[_-]?token|session[_-]?token|private[_-]?key)["']?[ \t]*[:=])",
            std::regex::ECMAScript | std::regex::icase);
        static const std::regex kUrlWithCredentials(R"([a-z][a-z0-9+.-]*://[^/\s:@"']+:[^/\s@"']+@)",
                                                    std::regex::ECMAScript | std::regex::icase);
        return std::regex_search(line, kCredentialKey) || std::regex_search(line, kUrlWithCredentials);
    }

    bool IsConfigLikeFile(const fs::path& path)
    {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        static const std::set<std::string> kConfigExtensions = {".ini",  ".cfg", ".conf", ".json", ".toml",
                                                                ".yaml", ".yml", ".xml",  ".env"};
        return kConfigExtensions.contains(ext) || path.filename() == ".env";
    }

    bool UsesHashComments(const fs::path& path)
    {
        const std::string ext = path.extension().string();
        return ext != ".json" && ext != ".xml";
    }

    /// Findings as "relative/path:line"; comment lines in INI-style files are skipped.
    std::vector<std::string> ScanForCredentials(const fs::path& file, const fs::path& repoRoot)
    {
        std::vector<std::string> findings;
        std::ifstream in(file, std::ios::binary);
        std::string line;
        size_t lineNumber = 0;
        const bool hashComments = UsesHashComments(file);
        while (std::getline(in, line))
        {
            ++lineNumber;
            if (hashComments)
            {
                const size_t first = line.find_first_not_of(" \t");
                if (first == std::string::npos || line[first] == '#' || line[first] == ';')
                    continue;
            }
            if (LineCarriesCredential(line))
                findings.push_back(fs::relative(file, repoRoot).generic_string() + ":" + std::to_string(lineNumber));
        }
        return findings;
    }
} // namespace

// ==========================================================================
// TERRAFRONT account store (TFAccountSystem -> TFDatabase -> JSON file)
// ==========================================================================

TEST(Persistence_Secrets_TFAccountStoreHoldsOnlySaltedPbkdf2Hashes)
{
    const fs::path path = FreshPath("test_data120_secrets_tf.db");
    const std::string sharedPassword = "Od22-Plaintext-Canary-7f3a";

    uint64_t aliceId = 0;
    {
        Terrafront::TFDatabase db;
        ASSERT_TRUE(db.Open(path));
        Terrafront::TFAccountSystem accounts;
        accounts.SetDatabase(&db);

        // Two accounts with the same password must still get distinct salts.
        const Terrafront::TFAuthResult alice = accounts.Register("od22_alice", sharedPassword);
        const Terrafront::TFAuthResult bob = accounts.Register("od22_bob", sharedPassword);
        ASSERT_TRUE(alice.ok);
        ASSERT_TRUE(bob.ok);
        aliceId = alice.accountId;
        EXPECT_TRUE(accounts.Login("od22_alice", sharedPassword).ok);
        EXPECT_TRUE(db.Close());
    }

    const std::string raw = ReadFile(path);
    ASSERT_FALSE(raw.empty());
    EXPECT_FALSE(ContainsSecret(raw, sharedPassword));

    const Spark::Json::Value root = ParseStore(path);
    ASSERT_TRUE(root.IsObject() && root.HasKey("accounts") && root["accounts"].IsArray());
    const Spark::Json::Value& rows = root["accounts"];
    ASSERT_EQ(rows.Size(), size_t{2});

    // The account row is an allowlist: a new column (plaintext password, a
    // remembered session, a reset token) fails here until OD-22 is re-reviewed.
    const std::set<std::string> allowedColumns = {"id",           "username",    "salt",
                                                  "passwordHash", "createdAtMs", "lastLoginMs"};
    std::set<std::string> salts;
    std::set<std::string> hashes;
    for (size_t i = 0; i < rows.Size(); ++i)
    {
        const Spark::Json::Value& row = rows[i];
        for (const std::string& key : row.GetKeys())
            EXPECT_TRUE(allowedColumns.contains(key));
        ASSERT_TRUE(row["salt"].IsString() && row["passwordHash"].IsString());
        const std::string& salt = row["salt"].AsString();
        const std::string& hash = row["passwordHash"].AsString();
        EXPECT_TRUE(IsSaltedPbkdf2Hash(hash, salt));
        salts.insert(salt);
        hashes.insert(hash);
    }
    EXPECT_EQ(salts.size(), size_t{2});
    EXPECT_EQ(hashes.size(), size_t{2});

    // The persisted hash alone is enough to authenticate after a restart.
    {
        Terrafront::TFDatabase db;
        ASSERT_TRUE(db.Open(path));
        Terrafront::TFAccountSystem accounts;
        accounts.SetDatabase(&db);
        const Terrafront::TFAuthResult relogin = accounts.Login("od22_alice", sharedPassword);
        EXPECT_TRUE(relogin.ok);
        EXPECT_EQ(relogin.accountId, aliceId);
        EXPECT_FALSE(accounts.Login("od22_alice", sharedPassword + "x").ok);
        EXPECT_TRUE(db.Close());
    }
    EXPECT_FALSE(ContainsSecret(ReadFile(path), sharedPassword));
    fs::remove(path);
}

TEST(Persistence_Secrets_TFLoginAndSessionBindingPersistOnlyLoginTime)
{
    const fs::path path = FreshPath("test_data120_secrets_tf_session.db");
    const std::string password = "Od22-Session-Canary-91c4";

    Terrafront::TFDatabase db;
    ASSERT_TRUE(db.Open(path));
    Terrafront::TFAccountSystem accounts;
    accounts.SetDatabase(&db);
    const Terrafront::TFAuthResult registered = accounts.Register("od22_session", password);
    ASSERT_TRUE(registered.ok);
    const Spark::Json::Value before = ParseStore(path);
    ASSERT_TRUE(before.IsObject());

    // A login plus a bound session is the whole authenticated state; the
    // session binding (client id -> account id) must stay in memory.
    constexpr uint32_t clientId = 0x5EC7E7u;
    ASSERT_TRUE(accounts.Login("od22_session", password).ok);
    accounts.BindSession(clientId, registered.accountId);
    EXPECT_EQ(accounts.AccountForClient(clientId), registered.accountId);

    const Spark::Json::Value after = ParseStore(path);
    ASSERT_TRUE(after.IsObject());
    EXPECT_TRUE(before.GetKeys() == after.GetKeys());
    ASSERT_EQ(after["accounts"].Size(), size_t{1});
    const Spark::Json::Value& rowBefore = before["accounts"][0];
    const Spark::Json::Value& rowAfter = after["accounts"][0];
    EXPECT_TRUE(rowBefore.GetKeys() == rowAfter.GetKeys());
    for (const std::string& key : rowAfter.GetKeys())
    {
        if (key == "lastLoginMs")
            continue;
        EXPECT_TRUE(Spark::Json::Stringify(rowBefore[key]) == Spark::Json::Stringify(rowAfter[key]));
    }
    EXPECT_GT(rowAfter["lastLoginMs"].AsNumber(), rowBefore["lastLoginMs"].AsNumber());

    const std::string raw = ReadFile(path);
    EXPECT_FALSE(ContainsSecret(raw, password));
    EXPECT_TRUE(raw.find(std::to_string(clientId)) == std::string::npos);

    accounts.ClearSession(clientId);
    EXPECT_TRUE(db.Close());
    fs::remove(path);
}

// ==========================================================================
// MMO account system beside the MMO key-value store (AsyncDatabase)
// ==========================================================================

TEST(Persistence_Secrets_MMOKeyValueStoreNeverReceivesPasswordOrSessionToken)
{
    const fs::path path = FreshPath("test_data120_secrets_mmo.db");
    const std::string password = "Od22-Mmo-Canary-2b77";

    MMO::MMOAccountSystem accounts;
    ASSERT_TRUE(accounts.Initialize(nullptr));
    const MMO::AuthResult registered = accounts.Register("od22_mmo", password);
    ASSERT_TRUE(registered.success);
    const MMO::AuthResult login = accounts.Login("od22_mmo", password);
    ASSERT_TRUE(login.success);
    ASSERT_FALSE(login.sessionToken.empty());
    const auto account = accounts.GetAccount(registered.accountId);
    ASSERT_TRUE(account.has_value());

    // Drive the real save path a login feeds: create, bind, and save a character.
    MMO::MMOPersistenceSystem persistence;
    ASSERT_TRUE(persistence.Initialize(nullptr, path.string()));
    const uint32_t characterId = persistence.CreateCharacter("Od22Hero", registered.accountId);
    ASSERT_NE(characterId, uint32_t{0});
    EXPECT_TRUE(accounts.SetActiveCharacter(login.sessionToken, characterId));

    MMO::CharacterSaveData save;
    save.characterId = characterId;
    save.accountId = registered.accountId;
    save.name = "Od22Hero";
    save.level = 3;
    save.lastLogin = 1234;
    EXPECT_TRUE(persistence.SaveCharacterSync(save));
    persistence.Shutdown();

    const std::string raw = ReadFile(path);
    ASSERT_FALSE(raw.empty());
    EXPECT_TRUE(raw.find("Od22Hero") != std::string::npos); // the store really was written
    EXPECT_FALSE(ContainsSecret(raw, password));
    EXPECT_FALSE(ContainsSecret(raw, login.sessionToken));
    EXPECT_TRUE(raw.find(account->passwordHash) == std::string::npos);

    // MMOAccountSystem has no persistence path of its own, so this is a structural
    // guard: the store holds the character but no authentication state, and a fresh
    // account system started beside the reopened store does not honour the old token.
    MMO::MMOPersistenceSystem reopened;
    ASSERT_TRUE(reopened.Initialize(nullptr, path.string()));
    MMO::CharacterSaveData loaded;
    EXPECT_TRUE(reopened.LoadCharacter(characterId, loaded));
    EXPECT_EQ(loaded.accountId, registered.accountId);
    MMO::MMOAccountSystem restartedAccounts;
    ASSERT_TRUE(restartedAccounts.Initialize(nullptr));
    EXPECT_FALSE(restartedAccounts.ValidateSession(login.sessionToken));
    restartedAccounts.Shutdown();
    reopened.Shutdown();

    accounts.Logout(login.sessionToken);
    EXPECT_FALSE(accounts.ValidateSession(login.sessionToken));
    accounts.Shutdown();
    fs::remove(path);
}

// ==========================================================================
// Shipped configuration carries no database or session secret
// ==========================================================================

TEST(Persistence_Secrets_CredentialScannerDetectsPlantedSecrets)
{
    // The shipped-config gate below is only as good as this matcher.
    EXPECT_TRUE(LineCarriesCredential("db_password = hunter2"));
    EXPECT_TRUE(LineCarriesCredential("DatabasePassword=hunter2"));
    EXPECT_TRUE(LineCarriesCredential(R"(  "sessionToken": "0123abcd",)"));
    EXPECT_TRUE(LineCarriesCredential(R"("connectionString": "Server=db;")"));
    EXPECT_TRUE(LineCarriesCredential("postgres_dsn: host=db"));
    EXPECT_TRUE(LineCarriesCredential("Url=postgres://spark:hunter2@db.internal/spark"));
    EXPECT_TRUE(LineCarriesCredential("secret=abc"));

    EXPECT_FALSE(LineCarriesCredential("ResolutionWidth=1920"));
    EXPECT_FALSE(LineCarriesCredential(R"("secretArea": true)"));
    EXPECT_FALSE(LineCarriesCredential(R"("passwordHint" is not a key here)"));
    EXPECT_FALSE(LineCarriesCredential("Url=https://example.com/docs"));
}

TEST(Persistence_Secrets_ShippedConfigCarriesNoDatabaseCredential)
{
    // Mirrors the runtime install rules: the engine config tree and every Assets
    // directory the root CMakeLists.txt copies beside the binary, plus the operator
    // example configs SparkServer and SparkGateway install to share/SparkEngine/examples.
    const fs::path repoRoot(SPARK_TEST_SOURCE_DIR);
    std::vector<fs::path> shippedRoots = {repoRoot / "SparkEngine" / "Resources" / "Config",
                                          repoRoot / "SparkServer" / "config", repoRoot / "SparkGateway" / "config"};
    for (const char* assetDir :
         {"Models", "Textures", "Audio", "Materials", "Scenes", "Scripts", "MMO", "MMOFPS", "Engine"})
        shippedRoots.push_back(repoRoot / "Assets" / assetDir);

    size_t scannedFiles = 0;
    bool sawEngineSettings = false;
    int operatorExamples = 0;
    std::vector<std::string> findings;
    for (const fs::path& root : shippedRoots)
    {
        if (!fs::is_directory(root))
            continue;
        for (const auto& entry : fs::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file() || !IsConfigLikeFile(entry.path()))
                continue;
            ++scannedFiles;
            if (entry.path().filename() == "settings.ini")
                sawEngineSettings = true;
            if (entry.path().filename() == "server.example.ini" || entry.path().filename() == "gateway.example.ini")
                ++operatorExamples;
            const std::vector<std::string> fileFindings = ScanForCredentials(entry.path(), repoRoot);
            findings.insert(findings.end(), fileFindings.begin(), fileFindings.end());
        }
    }

    for (const std::string& finding : findings)
        std::printf("  OD-22 credential-shaped entry in shipped config: %s\n", finding.c_str());
    EXPECT_TRUE(sawEngineSettings);
    EXPECT_EQ(operatorExamples, 2);
    EXPECT_GT(scannedFiles, size_t{1});
    EXPECT_TRUE(findings.empty());
}
