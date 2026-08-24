/**
 * @file TFDatabase.cpp
 * @brief TFDatabase implementation — atomic-JSON-file backing (see TFDatabase.h).
 *
 * Kept minimal-dependency (Core/TFTypes.h for FactionId + the header-only
 * Utils/JsonUtils.h) so it links standalone into SparkTests, which does not
 * compile module .cpp by default (Tests/CMakeLists.txt adds this file
 * explicitly, mirroring GameMode.cpp).
 */
#include "Persistence/TFDatabase.h"
#include "Persistence/TFJsonStrict.h"
#include "Persistence/TFSavePaths.h"
#include "Game/TFProgressionTypes.h"

#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <type_traits>
#include <unordered_set>

namespace Terrafront
{

    namespace
    {

        // JSON numbers are stored as doubles. Reserve the largest exactly
        // representable integer as an exhausted-counter sentinel so a
        // successful allocation can always persist its incremented counter.
        constexpr uint64_t kExhaustedJsonId = 9007199254740991ULL;

        /// Read a whole file into a string; false if it does not exist / can't open.
        bool ReadAllText(const std::filesystem::path& path, std::string& out)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open())
                return false;
            std::ostringstream ss;
            ss << in.rdbuf();
            if (in.bad())
                return false;
            out = ss.str();
            return true;
        }

        template <typename UInt> bool ReadUnsigned(const Spark::Json::Value& value, UInt& out)
        {
            static_assert(std::is_unsigned_v<UInt>);
            if (!value.IsNumber())
                return false;
            const double number = value.AsNumber(-1.0);
            constexpr double kMaxExactJsonInteger = static_cast<double>(kExhaustedJsonId);
            const double maximum =
                std::min(kMaxExactJsonInteger, static_cast<double>(std::numeric_limits<UInt>::max()));
            if (!std::isfinite(number) || number < 0.0 || number > maximum || std::trunc(number) != number)
                return false;
            out = static_cast<UInt>(number);
            return true;
        }

        bool ReadInt64(const Spark::Json::Value& value, int64_t& out)
        {
            if (!value.IsNumber())
                return false;
            const double number = value.AsNumber(0.0);
            constexpr double kMaxExactJsonInteger = static_cast<double>(kExhaustedJsonId);
            if (!std::isfinite(number) || number < -kMaxExactJsonInteger || number > kMaxExactJsonInteger ||
                std::trunc(number) != number)
                return false;
            out = static_cast<int64_t>(number);
            return true;
        }

        int64_t NowMs()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        }

    } // namespace

    TFDatabase::~TFDatabase()
    {
        if (m_open)
            Close();
    }

    bool TFDatabase::Open(const std::filesystem::path& path)
    {
        if (m_open)
            return false;
        if (m_recoveryLatched)
            return false;
        if (path.empty())
        {
            m_status = TFDatabaseStatus::Unreadable;
            return false;
        }
        m_path = path;

        namespace fs = std::filesystem;
        auto parentPath = m_path.parent_path();
        if (!parentPath.empty())
        {
            std::error_code ec;
            fs::create_directories(parentPath, ec);
            if (ec)
            {
                m_status = TFDatabaseStatus::Unreadable;
                m_recoveryLatched = true;
                return false;
            }
        }

        std::error_code lockEc;
        if (!m_fileLock.TryLock(m_path, lockEc))
        {
            m_status = TFDatabaseStatus::Locked;
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] db open refused for %s: another authority owns the persistence lock (%s)",
                            SavePaths::Utf8ForLog(m_path).c_str(), lockEc.message().c_str());
            return false;
        }

        m_accounts.clear();
        m_characters.clear();
        m_nextAccountId = 1;
        m_nextCharId = 1;

        std::error_code existsEc;
        const bool dbFileExists = fs::exists(m_path, existsEc);
        if (existsEc)
        {
            m_status = TFDatabaseStatus::Unreadable;
            m_recoveryLatched = true;
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] db stat failed for %s: %s; retries latched off",
                            SavePaths::Utf8ForLog(m_path).c_str(), existsEc.message().c_str());
            m_fileLock.Unlock();
            return false;
        }
        if (!dbFileExists)
        {
            fs::path recoveryBackup;
            std::error_code recoveryEc;
            if (SavePaths::FindRecoveryBackup(m_path, recoveryBackup, recoveryEc))
            {
                m_status = TFDatabaseStatus::Corrupt;
                m_recoveryLatched = true;
                SPARK_LOG_ERROR(Spark::LogCategory::Game,
                                "[TF] db primary %s is missing while recovery backup %s exists; recovery required",
                                SavePaths::Utf8ForLog(m_path).c_str(), SavePaths::Utf8ForLog(recoveryBackup).c_str());
                m_fileLock.Unlock();
                return false;
            }
            if (recoveryEc)
            {
                m_status = TFDatabaseStatus::Unreadable;
                m_recoveryLatched = true;
                m_fileLock.Unlock();
                return false;
            }
        }
        if (dbFileExists)
        {
            const LoadResult load = LoadFromDisk();
            if (load != LoadResult::Loaded)
            {
                m_recoveryLatched = true;
                if (load == LoadResult::Corrupt)
                {
                    m_status = TFDatabaseStatus::Corrupt;
                    std::filesystem::path backupPath = m_path;
                    backupPath += ".corrupt-" + std::to_string(NowMs()) + ".bak";
                    std::error_code backupEc;
                    fs::copy_file(m_path, backupPath, fs::copy_options::none, backupEc);
                    SPARK_LOG_ERROR(Spark::LogCategory::Game,
                                    "[TF] corrupt db %s retained; recovery backup %s created=%d; retries latched off",
                                    SavePaths::Utf8ForLog(m_path).c_str(), SavePaths::Utf8ForLog(backupPath).c_str(),
                                    backupEc ? 0 : 1);
                }
                else
                {
                    m_status = TFDatabaseStatus::Unreadable;
                    SPARK_LOG_ERROR(Spark::LogCategory::Game,
                                    "[TF] unreadable db %s left in place; retries latched off",
                                    SavePaths::Utf8ForLog(m_path).c_str());
                }
                m_fileLock.Unlock();
                return false;
            }
        }
        // dbFileExists == false: no prior db, LoadFromDisk() skipped -> fresh db.

        m_open = true;
        m_status = dbFileExists ? TFDatabaseStatus::ReadyExisting : TFDatabaseStatus::ReadyNew;
        return true;
    }

    bool TFDatabase::Close()
    {
        if (!m_open)
            return true;

        // Every mutation is committed atomically before it reports success and
        // is rolled back in memory on failure. Rewriting here creates a second,
        // unnecessary failure point after the module's persistence checkpoint.
        m_open = false;
        m_status = TFDatabaseStatus::Closed;
        m_fileLock.Unlock();
        return true;
    }

    TFDatabase::LoadResult TFDatabase::LoadFromDisk()
    {
        std::string text;
        if (!ReadAllText(m_path, text))
            return LoadResult::Unreadable;

        std::string lexicalError;
        if (!JsonStrict::ValidateLexemes(text, {}, lexicalError))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] db %s rejected by lexical validation: %s",
                            SavePaths::Utf8ForLog(m_path).c_str(), lexicalError.c_str());
            return LoadResult::Corrupt;
        }

        // Strict parse (W9): the lenient Parse accepts truncated/torn files as a
        // partial object, which silently loaded an empty db and wiped the file on
        // the next eager flush. A strict failure here makes Open()'s quarantine
        // path actually trigger.
        Spark::Json::Value root;
        std::string parseError;
        if (!Spark::Json::ParseStrict(text, &root, &parseError))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] db %s rejected by strict JSON parse: %s",
                            SavePaths::Utf8ForLog(m_path).c_str(), parseError.c_str());
            return LoadResult::Corrupt;
        }
        if (!root.IsObject())
            return LoadResult::Corrupt;
        if (!root["accounts"].IsArray() || !root["characters"].IsArray())
            return LoadResult::Corrupt;

        const bool hasNextAccountId = root.HasKey("nextAccountId");
        const bool hasNextCharId = root.HasKey("nextCharId");
        uint64_t nextAccountId = 1;
        uint64_t nextCharId = 1;
        if ((root.HasKey("nextAccountId") &&
             (!ReadUnsigned(root["nextAccountId"], nextAccountId) || nextAccountId == 0)) ||
            (root.HasKey("nextCharId") && (!ReadUnsigned(root["nextCharId"], nextCharId) || nextCharId == 0)))
            return LoadResult::Corrupt;

        const auto& accountRows = root["accounts"];
        std::unordered_set<uint64_t> accountIds;
        std::unordered_set<std::string> usernames;
        uint64_t maxAccountId = 0;
        for (size_t i = 0; i < accountRows.Size(); ++i)
        {
            const auto& row = accountRows[i];
            uint64_t id = 0;
            int64_t createdAt = 0;
            int64_t lastLogin = 0;
            if (!row.IsObject() || !ReadUnsigned(row["id"], id) || id == 0 || id >= kExhaustedJsonId ||
                !row["username"].IsString() || row["username"].AsString().empty() || !row["salt"].IsString() ||
                row["salt"].AsString().empty() || !row["passwordHash"].IsString() ||
                row["passwordHash"].AsString().empty() || !ReadInt64(row["createdAtMs"], createdAt) ||
                !ReadInt64(row["lastLoginMs"], lastLogin) || !accountIds.insert(id).second ||
                !usernames.insert(row["username"].AsString()).second)
                return LoadResult::Corrupt;
            maxAccountId = std::max(maxAccountId, id);
        }
        const auto& characterRows = root["characters"];
        std::unordered_set<uint64_t> characterIds;
        std::unordered_set<std::string> characterNames;
        uint64_t maxCharacterId = 0;
        for (size_t i = 0; i < characterRows.Size(); ++i)
        {
            const auto& row = characterRows[i];
            uint64_t id = 0;
            uint64_t accountId = 0;
            uint8_t faction = 0;
            uint32_t xp = 0;
            uint16_t rank = 0;
            uint32_t flux = 0;
            int64_t createdAt = 0;
            int64_t lastPlayed = 0;
            if (!row.IsObject() || !ReadUnsigned(row["id"], id) || id == 0 || id >= kExhaustedJsonId ||
                !ReadUnsigned(row["accountId"], accountId) || accountId == 0 || !accountIds.contains(accountId) ||
                !row["name"].IsString() || row["name"].AsString().empty() || !ReadUnsigned(row["faction"], faction) ||
                faction == 0 || faction >= static_cast<uint8_t>(FactionId::COUNT) || !ReadUnsigned(row["xp"], xp) ||
                !ReadUnsigned(row["rank"], rank) || rank == 0 || rank > kTFMaxRank ||
                !ReadUnsigned(row["flux"], flux) || flux > kFluxWalletCap ||
                !ReadInt64(row["createdAtMs"], createdAt) || !ReadInt64(row["lastPlayedMs"], lastPlayed) ||
                !characterIds.insert(id).second || !characterNames.insert(row["name"].AsString()).second)
                return LoadResult::Corrupt;

            if (row.HasKey("unlocks"))
            {
                if (!row["unlocks"].IsArray())
                    return LoadResult::Corrupt;
                std::unordered_set<std::string> unlockKeys;
                const auto& unlocks = row["unlocks"];
                for (size_t u = 0; u < unlocks.Size(); ++u)
                    if (!unlocks[u].IsString() || unlocks[u].AsString().empty() ||
                        !unlockKeys.insert(unlocks[u].AsString()).second)
                        return LoadResult::Corrupt;
            }
            if (row.HasKey("loadout"))
            {
                if (!row["loadout"].IsObject())
                    return LoadResult::Corrupt;
                const auto& loadout = row["loadout"];
                constexpr const char* slots[] = {"primary", "secondary", "tool", "grenade", "suit"};
                for (const char* slot : slots)
                    if (loadout.HasKey(slot) && !loadout[slot].IsString())
                        return LoadResult::Corrupt;
            }
            if (row.HasKey("weaponStats"))
            {
                if (!row["weaponStats"].IsArray())
                    return LoadResult::Corrupt;
                std::unordered_set<std::string> weaponKeys;
                const auto& stats = row["weaponStats"];
                for (size_t s = 0; s < stats.Size(); ++s)
                {
                    const auto& stat = stats[s];
                    uint32_t count = 0;
                    if (!stat.IsObject() || !stat["key"].IsString() || stat["key"].AsString().empty() ||
                        !weaponKeys.insert(stat["key"].AsString()).second)
                        return LoadResult::Corrupt;
                    constexpr const char* counters[] = {"kills", "shots", "hits", "headshots"};
                    for (const char* counter : counters)
                        if (stat.HasKey(counter) && !ReadUnsigned(stat[counter], count))
                            return LoadResult::Corrupt;
                }
            }
            maxCharacterId = std::max(maxCharacterId, id);
        }

        if (!hasNextAccountId && maxAccountId != 0)
            nextAccountId = maxAccountId + 1;
        if (!hasNextCharId && maxCharacterId != 0)
            nextCharId = maxCharacterId + 1;
        if (nextAccountId <= maxAccountId || nextCharId <= maxCharacterId)
            return LoadResult::Corrupt;
        m_nextAccountId = nextAccountId;
        m_nextCharId = nextCharId;

        if (root.HasKey("accounts") && root["accounts"].IsArray())
        {
            const auto& arr = root["accounts"];
            for (size_t i = 0; i < arr.Size(); ++i)
            {
                const auto& row = arr[i];
                TFAccountRecord rec;
                rec.id = static_cast<uint64_t>(row["id"].AsNumber(0.0));
                rec.username = row["username"].AsString();
                rec.salt = row["salt"].AsString();
                rec.passwordHash = row["passwordHash"].AsString();
                rec.createdAtMs = static_cast<int64_t>(row["createdAtMs"].AsNumber(0.0));
                rec.lastLoginMs = static_cast<int64_t>(row["lastLoginMs"].AsNumber(0.0));
                m_accounts.push_back(std::move(rec));
            }
        }

        if (root.HasKey("characters") && root["characters"].IsArray())
        {
            const auto& arr = root["characters"];
            for (size_t i = 0; i < arr.Size(); ++i)
            {
                const auto& row = arr[i];
                TFCharacterRecord rec;
                rec.id = static_cast<uint64_t>(row["id"].AsNumber(0.0));
                rec.accountId = static_cast<uint64_t>(row["accountId"].AsNumber(0.0));
                rec.name = row["name"].AsString();
                rec.faction = static_cast<FactionId>(static_cast<uint8_t>(row["faction"].AsNumber(0.0)));
                rec.xp = static_cast<uint32_t>(row["xp"].AsNumber(0.0));
                rec.rank = static_cast<uint16_t>(row["rank"].AsNumber(1.0));
                rec.flux = static_cast<uint32_t>(row["flux"].AsNumber(0.0));
                rec.createdAtMs = static_cast<int64_t>(row["createdAtMs"].AsNumber(0.0));
                rec.lastPlayedMs = static_cast<int64_t>(row["lastPlayedMs"].AsNumber(0.0));

                // W6 progression expansion (additive keys; tolerant of old files)
                if (row.HasKey("unlocks") && row["unlocks"].IsArray())
                {
                    const auto& unlocks = row["unlocks"];
                    for (size_t u = 0; u < unlocks.Size(); ++u)
                        if (unlocks[u].IsString())
                            rec.unlocks.push_back(unlocks[u].AsString());
                }
                if (row.HasKey("loadout") && row["loadout"].IsObject())
                {
                    const auto& lo = row["loadout"];
                    if (lo["primary"].IsString())
                        rec.loadoutPrimary = lo["primary"].AsString();
                    if (lo["secondary"].IsString())
                        rec.loadoutSecondary = lo["secondary"].AsString();
                    if (lo["tool"].IsString())
                        rec.loadoutTool = lo["tool"].AsString();
                    // loadout-depth wave (additive keys; tolerant of old rows)
                    if (lo["grenade"].IsString())
                        rec.loadoutGrenade = lo["grenade"].AsString();
                    if (lo["suit"].IsString())
                        rec.loadoutSuit = lo["suit"].AsString();
                }
                if (row.HasKey("weaponStats") && row["weaponStats"].IsArray())
                {
                    const auto& stats = row["weaponStats"];
                    for (size_t s = 0; s < stats.Size(); ++s)
                    {
                        const auto& srow = stats[s];
                        if (!srow.IsObject() || !srow["key"].IsString())
                            continue;
                        TFWeaponStatsRow stat;
                        stat.weaponKey = srow["key"].AsString();
                        if (stat.weaponKey.empty())
                            continue;
                        stat.kills = static_cast<uint32_t>(srow["kills"].AsNumber(0.0));
                        stat.shots = static_cast<uint32_t>(srow["shots"].AsNumber(0.0));
                        stat.hits = static_cast<uint32_t>(srow["hits"].AsNumber(0.0));
                        stat.headshots = static_cast<uint32_t>(srow["headshots"].AsNumber(0.0));
                        rec.weaponStats.push_back(std::move(stat));
                    }
                }

                m_characters.push_back(std::move(rec));
            }
        }

        return LoadResult::Loaded;
    }

    bool TFDatabase::SaveToDisk() const
    {
        namespace fs = std::filesystem;

        Spark::Json::Value root = Spark::Json::Value::MakeObject();
        root["nextAccountId"] = Spark::Json::Value(static_cast<double>(m_nextAccountId));
        root["nextCharId"] = Spark::Json::Value(static_cast<double>(m_nextCharId));

        Spark::Json::Value accounts = Spark::Json::Value::MakeArray();
        for (const auto& a : m_accounts)
        {
            Spark::Json::Value row = Spark::Json::Value::MakeObject();
            row["id"] = Spark::Json::Value(static_cast<double>(a.id));
            row["username"] = Spark::Json::Value(a.username);
            row["salt"] = Spark::Json::Value(a.salt);
            row["passwordHash"] = Spark::Json::Value(a.passwordHash);
            row["createdAtMs"] = Spark::Json::Value(static_cast<double>(a.createdAtMs));
            row["lastLoginMs"] = Spark::Json::Value(static_cast<double>(a.lastLoginMs));
            accounts.PushBack(std::move(row));
        }
        root["accounts"] = std::move(accounts);

        Spark::Json::Value characters = Spark::Json::Value::MakeArray();
        for (const auto& c : m_characters)
        {
            Spark::Json::Value row = Spark::Json::Value::MakeObject();
            row["id"] = Spark::Json::Value(static_cast<double>(c.id));
            row["accountId"] = Spark::Json::Value(static_cast<double>(c.accountId));
            row["name"] = Spark::Json::Value(c.name);
            row["faction"] = Spark::Json::Value(static_cast<double>(static_cast<uint8_t>(c.faction)));
            row["xp"] = Spark::Json::Value(static_cast<double>(c.xp));
            row["rank"] = Spark::Json::Value(static_cast<double>(c.rank));
            row["flux"] = Spark::Json::Value(static_cast<double>(c.flux));
            row["createdAtMs"] = Spark::Json::Value(static_cast<double>(c.createdAtMs));
            row["lastPlayedMs"] = Spark::Json::Value(static_cast<double>(c.lastPlayedMs));

            // W6 progression expansion (additive keys)
            Spark::Json::Value unlocks = Spark::Json::Value::MakeArray();
            for (const std::string& key : c.unlocks)
                unlocks.PushBack(Spark::Json::Value(key));
            row["unlocks"] = std::move(unlocks);

            Spark::Json::Value lo = Spark::Json::Value::MakeObject();
            lo["primary"] = Spark::Json::Value(c.loadoutPrimary);
            lo["secondary"] = Spark::Json::Value(c.loadoutSecondary);
            lo["tool"] = Spark::Json::Value(c.loadoutTool);
            lo["grenade"] = Spark::Json::Value(c.loadoutGrenade); // loadout-depth wave
            lo["suit"] = Spark::Json::Value(c.loadoutSuit);
            row["loadout"] = std::move(lo);

            Spark::Json::Value stats = Spark::Json::Value::MakeArray();
            for (const TFWeaponStatsRow& s : c.weaponStats)
            {
                Spark::Json::Value srow = Spark::Json::Value::MakeObject();
                srow["key"] = Spark::Json::Value(s.weaponKey);
                srow["kills"] = Spark::Json::Value(static_cast<double>(s.kills));
                srow["shots"] = Spark::Json::Value(static_cast<double>(s.shots));
                srow["hits"] = Spark::Json::Value(static_cast<double>(s.hits));
                srow["headshots"] = Spark::Json::Value(static_cast<double>(s.headshots));
                stats.PushBack(std::move(srow));
            }
            row["weaponStats"] = std::move(stats);

            characters.PushBack(std::move(row));
        }
        root["characters"] = std::move(characters);

        std::error_code ec;
        auto parentPath = m_path.parent_path();
        if (!parentPath.empty())
        {
            fs::create_directories(parentPath, ec);
            if (ec)
            {
                return false;
            }
        }

        std::filesystem::path tmpFile = m_path;
        tmpFile += ".tmp";
        {
            std::ofstream out(tmpFile, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
                return false;
            out << Spark::Json::StringifyPretty(root);
            if (!out.good())
                return false;
        }

        return SavePaths::AtomicReplace(tmpFile, m_path, ec);
    }

    bool TFDatabase::CreateAccount(const std::string& username, const std::string& salt, const std::string& hash,
                                   TFAccountRecord& out)
    {
        if (!m_open || username.empty() || salt.empty() || hash.empty())
            return false;
        TFAccountRecord existing;
        if (FindAccountByUsername(username, existing))
            return false; // username taken
        if (m_nextAccountId >= kExhaustedJsonId)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] CreateAccount refused for '%s': the exactly representable JSON id range is exhausted",
                            username.c_str());
            return false;
        }

        TFAccountRecord rec;
        rec.id = m_nextAccountId++;
        rec.username = username;
        rec.salt = salt;
        rec.passwordHash = hash;
        rec.createdAtMs = NowMs();
        rec.lastLoginMs = 0;
        m_accounts.push_back(rec);

        if (!SaveToDisk())
        {
            m_accounts.pop_back();
            m_nextAccountId = rec.id;
            m_status = TFDatabaseStatus::WriteFailed;
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] CreateAccount failed to persist account '%s' to %s",
                            rec.username.c_str(), SavePaths::Utf8ForLog(m_path).c_str());
            return false;
        }
        m_status = TFDatabaseStatus::ReadyExisting;
        out = rec;
        return true;
    }

    bool TFDatabase::FindAccountByUsername(const std::string& username, TFAccountRecord& out)
    {
        if (!m_open)
            return false;
        auto it = std::find_if(m_accounts.begin(), m_accounts.end(),
                               [&](const TFAccountRecord& a) { return a.username == username; });
        if (it == m_accounts.end())
            return false;
        out = *it;
        return true;
    }

    bool TFDatabase::TouchLogin(uint64_t accountId, int64_t nowMs)
    {
        if (!m_open)
            return false;
        auto it = std::find_if(m_accounts.begin(), m_accounts.end(),
                               [&](const TFAccountRecord& a) { return a.id == accountId; });
        if (it == m_accounts.end())
            return false;
        const int64_t previous = it->lastLoginMs;
        it->lastLoginMs = nowMs;
        if (!SaveToDisk())
        {
            it->lastLoginMs = previous;
            m_status = TFDatabaseStatus::WriteFailed;
            return false;
        }
        m_status = TFDatabaseStatus::ReadyExisting;
        return true;
    }

    bool TFDatabase::CreateCharacter(uint64_t accountId, const std::string& name, FactionId faction,
                                     TFCharacterRecord& out)
    {
        if (!m_open || accountId == 0 || name.empty() || faction == FactionId::None || faction >= FactionId::COUNT ||
            std::none_of(m_accounts.begin(), m_accounts.end(),
                         [accountId](const TFAccountRecord& account) { return account.id == accountId; }))
            return false;
        TFCharacterRecord existing;
        if (FindCharacterByName(name, existing))
            return false; // name taken
        if (m_nextCharId >= kExhaustedJsonId)
        {
            SPARK_LOG_ERROR(
                Spark::LogCategory::Game,
                "[TF] CreateCharacter refused for '%s': the exactly representable JSON id range is exhausted",
                name.c_str());
            return false;
        }

        TFCharacterRecord rec;
        rec.id = m_nextCharId++;
        rec.accountId = accountId;
        rec.name = name;
        rec.faction = faction;
        rec.xp = 0;
        rec.rank = 1;
        rec.flux = 0;
        rec.createdAtMs = NowMs();
        rec.lastPlayedMs = 0;
        m_characters.push_back(rec);

        if (!SaveToDisk())
        {
            m_characters.pop_back();
            m_nextCharId = rec.id;
            m_status = TFDatabaseStatus::WriteFailed;
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] CreateCharacter failed to persist '%s' to %s",
                            rec.name.c_str(), SavePaths::Utf8ForLog(m_path).c_str());
            return false;
        }
        m_status = TFDatabaseStatus::ReadyExisting;
        out = rec;
        return true;
    }

    bool TFDatabase::FindCharacterByName(const std::string& name, TFCharacterRecord& out)
    {
        if (!m_open)
            return false;
        auto it = std::find_if(m_characters.begin(), m_characters.end(),
                               [&](const TFCharacterRecord& c) { return c.name == name; });
        if (it == m_characters.end())
            return false;
        out = *it;
        return true;
    }

    std::vector<TFCharacterRecord> TFDatabase::ListCharacters(uint64_t accountId)
    {
        std::vector<TFCharacterRecord> result;
        if (!m_open)
            return result;
        for (const auto& c : m_characters)
            if (c.accountId == accountId)
                result.push_back(c);
        return result;
    }

    bool TFDatabase::FindCharacter(uint64_t charId, TFCharacterRecord& out)
    {
        if (!m_open)
            return false;
        auto it = std::find_if(m_characters.begin(), m_characters.end(),
                               [&](const TFCharacterRecord& c) { return c.id == charId; });
        if (it == m_characters.end())
            return false;
        out = *it;
        return true;
    }

    bool TFDatabase::DeleteCharacter(uint64_t charId)
    {
        if (!m_open)
            return false;
        auto it = std::find_if(m_characters.begin(), m_characters.end(),
                               [&](const TFCharacterRecord& c) { return c.id == charId; });
        if (it == m_characters.end())
            return false;
        const size_t index = static_cast<size_t>(std::distance(m_characters.begin(), it));
        const TFCharacterRecord removed = *it;
        m_characters.erase(it);
        if (!SaveToDisk())
        {
            m_characters.insert(m_characters.begin() + static_cast<std::ptrdiff_t>(index), removed);
            m_status = TFDatabaseStatus::WriteFailed;
            return false;
        }
        m_status = TFDatabaseStatus::ReadyExisting;
        return true;
    }

    bool TFDatabase::SaveCharacterProgress(uint64_t charId, uint32_t xp, uint16_t rank, uint32_t flux,
                                           int64_t lastPlayedMs)
    {
        if (!m_open || rank == 0 || rank > kTFMaxRank || flux > kFluxWalletCap)
            return false;
        auto it = std::find_if(m_characters.begin(), m_characters.end(),
                               [&](const TFCharacterRecord& c) { return c.id == charId; });
        if (it == m_characters.end())
            return false;
        const TFCharacterRecord previous = *it;
        it->xp = xp;
        it->rank = rank;
        it->flux = flux;
        it->lastPlayedMs = lastPlayedMs;
        if (!SaveToDisk())
        {
            *it = previous;
            m_status = TFDatabaseStatus::WriteFailed;
            return false;
        }
        m_status = TFDatabaseStatus::ReadyExisting;
        return true;
    }

    bool TFDatabase::SaveCharacterMeta(uint64_t charId, const std::vector<std::string>& unlocks,
                                       const std::string& loadoutPrimary, const std::string& loadoutSecondary,
                                       const std::string& loadoutTool, const std::string& loadoutGrenade,
                                       const std::string& loadoutSuit, const std::vector<TFWeaponStatsRow>& stats)
    {
        if (!m_open)
            return false;
        std::unordered_set<std::string> uniqueUnlocks;
        for (const std::string& unlock : unlocks)
            if (unlock.empty() || !uniqueUnlocks.insert(unlock).second)
                return false;
        std::unordered_set<std::string> uniqueWeapons;
        for (const TFWeaponStatsRow& stat : stats)
            if (stat.weaponKey.empty() || !uniqueWeapons.insert(stat.weaponKey).second)
                return false;
        auto it = std::find_if(m_characters.begin(), m_characters.end(),
                               [&](const TFCharacterRecord& c) { return c.id == charId; });
        if (it == m_characters.end())
            return false;
        const TFCharacterRecord previous = *it;
        it->unlocks = unlocks;
        it->loadoutPrimary = loadoutPrimary;
        it->loadoutSecondary = loadoutSecondary;
        it->loadoutTool = loadoutTool;
        it->loadoutGrenade = loadoutGrenade; // loadout-depth wave
        it->loadoutSuit = loadoutSuit;
        it->weaponStats = stats;
        if (!SaveToDisk())
        {
            *it = previous;
            m_status = TFDatabaseStatus::WriteFailed;
            return false;
        }
        m_status = TFDatabaseStatus::ReadyExisting;
        return true;
    }

} // namespace Terrafront
