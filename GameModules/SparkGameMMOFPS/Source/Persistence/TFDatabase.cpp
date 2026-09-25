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

        // Bound on waiting for another authority's transaction. Transactions
        // are a single small file rewrite, so a longer wait means a stuck peer.
        constexpr std::chrono::milliseconds kLockTimeout{2000};

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

        // Transaction-scoped: held only while the committed file is read.
        SavePaths::ExclusiveFileLock lock;
        std::error_code lockEc;
        if (!lock.Lock(m_path, kLockTimeout, lockEc))
        {
            m_status = TFDatabaseStatus::Locked;
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] db open refused for %s: persistence lock not acquired within %lld ms (%s)",
                            SavePaths::Utf8ForLog(m_path).c_str(), static_cast<long long>(kLockTimeout.count()),
                            lockEc.message().c_str());
            return false;
        }

        std::error_code existsEc;
        const bool dbFileExists = fs::exists(m_path, existsEc);
        if (existsEc)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] db stat failed for %s: %s; retries latched off",
                            SavePaths::Utf8ForLog(m_path).c_str(), existsEc.message().c_str());
            FailClosed(LoadResult::Unreadable);
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
                return false;
            }
            if (recoveryEc)
            {
                m_status = TFDatabaseStatus::Unreadable;
                m_recoveryLatched = true;
                return false;
            }
        }

        // dbFileExists == false: no prior db -> fresh, empty snapshot.
        Snapshot loaded;
        if (dbFileExists)
        {
            const LoadResult load = LoadFromDisk(loaded);
            if (load != LoadResult::Loaded)
            {
                FailClosed(load);
                return false;
            }
        }

        m_snapshot = std::move(loaded);
        m_baseRevisions.clear();
        for (const TFCharacterRecord& character : m_snapshot.characters)
            m_baseRevisions[character.id] = character.revision;
        m_open = true;
        m_status = dbFileExists ? TFDatabaseStatus::ReadyExisting : TFDatabaseStatus::ReadyNew;
        return true;
    }

    bool TFDatabase::Close()
    {
        if (!m_open)
            return true;

        // Every mutation is committed atomically before it reports success, and
        // no lock outlives a call. Rewriting here would only add a second,
        // unnecessary failure point after the module's persistence checkpoint.
        m_open = false;
        m_status = TFDatabaseStatus::Closed;
        m_snapshot = Snapshot{};
        m_baseRevisions.clear();
        return true;
    }

    void TFDatabase::FailClosed(LoadResult load)
    {
        namespace fs = std::filesystem;
        m_open = false;
        m_recoveryLatched = true;
        if (load == LoadResult::UnsupportedVersion)
        {
            // Not corrupt: a newer build owns this file. Leave it byte-for-byte
            // intact and refuse to serve from it.
            m_status = TFDatabaseStatus::UnsupportedVersion;
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] db %s was written by a newer schema (this build reads <= v%u); "
                            "refusing to serve it so a rollback cannot rewrite it",
                            SavePaths::Utf8ForLog(m_path).c_str(), kSchemaVersion);
        }
        else if (load == LoadResult::Corrupt)
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
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] unreadable db %s left in place; retries latched off",
                            SavePaths::Utf8ForLog(m_path).c_str());
        }
    }

    bool TFDatabase::Refresh(Snapshot& fresh)
    {
        // Caller holds the persistence lock, so the file cannot change under us.
        std::error_code existsEc;
        const bool exists = std::filesystem::exists(m_path, existsEc);
        if (existsEc)
        {
            FailClosed(LoadResult::Unreadable);
            return false;
        }
        if (!exists)
        {
            // Nothing was ever committed: still a fresh database. Anything else
            // means the committed primary vanished under a running authority;
            // recreating it would silently drop every row.
            if (m_snapshot.revision == 0 && m_snapshot.accounts.empty() && m_snapshot.characters.empty())
            {
                fresh = Snapshot{};
                return true;
            }
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] committed db %s disappeared; refusing to recreate it",
                            SavePaths::Utf8ForLog(m_path).c_str());
            FailClosed(LoadResult::Unreadable);
            return false;
        }

        const LoadResult load = LoadFromDisk(fresh);
        if (load != LoadResult::Loaded)
        {
            FailClosed(load);
            return false;
        }
        if (fresh.revision < m_snapshot.revision)
        {
            // The shared file was replaced by an older copy (e.g. a restore
            // under a running authority). Writing on top would fork history.
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] db %s revision went backwards (%llu < %llu); refusing to serve it",
                            SavePaths::Utf8ForLog(m_path).c_str(), static_cast<unsigned long long>(fresh.revision),
                            static_cast<unsigned long long>(m_snapshot.revision));
            FailClosed(LoadResult::Unreadable);
            return false;
        }
        return true;
    }

    bool TFDatabase::RefreshSnapshot()
    {
        if (!m_open)
            return false;
        SavePaths::ExclusiveFileLock lock;
        std::error_code lockEc;
        if (!lock.Lock(m_path, kLockTimeout, lockEc))
        {
            m_status = TFDatabaseStatus::Locked;
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] db read of %s refused: lock not acquired (%s)",
                            SavePaths::Utf8ForLog(m_path).c_str(), lockEc.message().c_str());
            return false;
        }
        Snapshot fresh;
        if (!Refresh(fresh))
            return false;
        m_snapshot = std::move(fresh);
        return true;
    }

    bool TFDatabase::Transact(const char* operation, const Mutation& mutation)
    {
        if (!m_open)
            return false;
        SavePaths::ExclusiveFileLock lock;
        std::error_code lockEc;
        if (!lock.Lock(m_path, kLockTimeout, lockEc))
        {
            m_status = TFDatabaseStatus::Locked;
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] %s on %s refused: lock not acquired (%s)", operation,
                            SavePaths::Utf8ForLog(m_path).c_str(), lockEc.message().c_str());
            return false;
        }

        Snapshot fresh;
        if (!Refresh(fresh))
            return false;
        if (fresh.revision + 1 >= kExhaustedJsonId)
        {
            m_status = TFDatabaseStatus::WriteFailed;
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] %s refused: db revision range is exhausted", operation);
            m_snapshot = std::move(fresh);
            return false;
        }

        const uint64_t newRevision = fresh.revision + 1;
        Snapshot next = fresh;
        if (!mutation(next, newRevision))
        {
            m_snapshot = std::move(fresh);
            return false;
        }
        next.revision = newRevision;
        if (!SaveToDisk(next))
        {
            m_snapshot = std::move(fresh);
            m_status = TFDatabaseStatus::WriteFailed;
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] %s failed to persist to %s; nothing was committed",
                            operation, SavePaths::Utf8ForLog(m_path).c_str());
            return false;
        }
        m_snapshot = std::move(next);
        m_status = TFDatabaseStatus::ReadyExisting;
        return true;
    }

    TFDatabase::LoadResult TFDatabase::LoadFromDisk(Snapshot& out) const
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

        // Absent schemaVersion == legacy v0 (pre-DATA-120), which is the same
        // row shape as v1 and upgrades on the next write. Anything newer may
        // carry fields this build would drop, so it must not be loaded.
        if (root.HasKey("schemaVersion"))
        {
            uint32_t schemaVersion = 0;
            if (!ReadUnsigned(root["schemaVersion"], schemaVersion) || schemaVersion == 0)
                return LoadResult::Corrupt;
            if (schemaVersion > kSchemaVersion)
                return LoadResult::UnsupportedVersion;
        }

        // TF-120: absent on v0/v1 files, which load as revision 0.
        uint64_t fileRevision = 0;
        if (root.HasKey("revision") &&
            (!ReadUnsigned(root["revision"], fileRevision) || fileRevision >= kExhaustedJsonId))
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
            uint64_t rowRevision = 0;
            if (row.HasKey("revision") && (!ReadUnsigned(row["revision"], rowRevision) || rowRevision > fileRevision))
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
        out = Snapshot{};
        out.revision = fileRevision;
        out.nextAccountId = nextAccountId;
        out.nextCharId = nextCharId;

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
                out.accounts.push_back(std::move(rec));
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
                rec.revision = static_cast<uint64_t>(row["revision"].AsNumber(0.0));

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

                out.characters.push_back(std::move(rec));
            }
        }

        return LoadResult::Loaded;
    }

    bool TFDatabase::SaveToDisk(const Snapshot& snapshot) const
    {
        namespace fs = std::filesystem;

        Spark::Json::Value root = Spark::Json::Value::MakeObject();
        root["schemaVersion"] = Spark::Json::Value(static_cast<double>(kSchemaVersion));
        root["revision"] = Spark::Json::Value(static_cast<double>(snapshot.revision));
        root["nextAccountId"] = Spark::Json::Value(static_cast<double>(snapshot.nextAccountId));
        root["nextCharId"] = Spark::Json::Value(static_cast<double>(snapshot.nextCharId));

        Spark::Json::Value accounts = Spark::Json::Value::MakeArray();
        for (const auto& a : snapshot.accounts)
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
        for (const auto& c : snapshot.characters)
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
            row["revision"] = Spark::Json::Value(static_cast<double>(c.revision));

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

        return SavePaths::WriteDurableReplace(m_path, Spark::Json::StringifyPretty(root), ec);
    }

    bool TFDatabase::CreateAccount(const std::string& username, const std::string& salt, const std::string& hash,
                                   TFAccountRecord& out)
    {
        if (!m_open || username.empty() || salt.empty() || hash.empty())
            return false;

        TFAccountRecord created;
        const bool committed = Transact(
            "CreateAccount",
            [&](Snapshot& fresh, uint64_t)
            {
                // Uniqueness and id allocation run against every authority's
                // committed rows, not this instance's last view.
                if (std::any_of(fresh.accounts.begin(), fresh.accounts.end(),
                                [&](const TFAccountRecord& a) { return a.username == username; }))
                    return false; // username taken
                if (fresh.nextAccountId >= kExhaustedJsonId)
                {
                    SPARK_LOG_ERROR(
                        Spark::LogCategory::Game,
                        "[TF] CreateAccount refused for '%s': the exactly representable JSON id range is exhausted",
                        username.c_str());
                    return false;
                }
                created.id = fresh.nextAccountId++;
                created.username = username;
                created.salt = salt;
                created.passwordHash = hash;
                created.createdAtMs = NowMs();
                created.lastLoginMs = 0;
                fresh.accounts.push_back(created);
                return true;
            });
        if (!committed)
            return false;
        out = created;
        return true;
    }

    bool TFDatabase::FindAccountByUsername(const std::string& username, TFAccountRecord& out)
    {
        if (!RefreshSnapshot())
            return false;
        auto it = std::find_if(m_snapshot.accounts.begin(), m_snapshot.accounts.end(),
                               [&](const TFAccountRecord& a) { return a.username == username; });
        if (it == m_snapshot.accounts.end())
            return false;
        out = *it;
        return true;
    }

    bool TFDatabase::TouchLogin(uint64_t accountId, int64_t nowMs)
    {
        return Transact("TouchLogin",
                        [&](Snapshot& fresh, uint64_t)
                        {
                            auto it = std::find_if(fresh.accounts.begin(), fresh.accounts.end(),
                                                   [&](const TFAccountRecord& a) { return a.id == accountId; });
                            if (it == fresh.accounts.end())
                                return false;
                            it->lastLoginMs = nowMs;
                            return true;
                        });
    }

    bool TFDatabase::CreateCharacter(uint64_t accountId, const std::string& name, FactionId faction,
                                     TFCharacterRecord& out)
    {
        if (!m_open || accountId == 0 || name.empty() || faction == FactionId::None || faction >= FactionId::COUNT)
            return false;

        TFCharacterRecord created;
        const bool committed = Transact(
            "CreateCharacter",
            [&](Snapshot& fresh, uint64_t newRevision)
            {
                if (std::none_of(fresh.accounts.begin(), fresh.accounts.end(),
                                 [accountId](const TFAccountRecord& account) { return account.id == accountId; }))
                    return false;
                if (std::any_of(fresh.characters.begin(), fresh.characters.end(),
                                [&](const TFCharacterRecord& c) { return c.name == name; }))
                    return false; // name taken
                if (fresh.nextCharId >= kExhaustedJsonId)
                {
                    SPARK_LOG_ERROR(
                        Spark::LogCategory::Game,
                        "[TF] CreateCharacter refused for '%s': the exactly representable JSON id range is exhausted",
                        name.c_str());
                    return false;
                }
                created.id = fresh.nextCharId++;
                created.accountId = accountId;
                created.name = name;
                created.faction = faction;
                created.xp = 0;
                created.rank = 1;
                created.flux = 0;
                created.createdAtMs = NowMs();
                created.lastPlayedMs = 0;
                created.revision = newRevision;
                fresh.characters.push_back(created);
                return true;
            });
        if (!committed)
            return false;
        // The creator works from exactly the committed row.
        m_baseRevisions[created.id] = created.revision;
        out = created;
        return true;
    }

    bool TFDatabase::FindCharacterByName(const std::string& name, TFCharacterRecord& out)
    {
        if (!RefreshSnapshot())
            return false;
        auto it = std::find_if(m_snapshot.characters.begin(), m_snapshot.characters.end(),
                               [&](const TFCharacterRecord& c) { return c.name == name; });
        if (it == m_snapshot.characters.end())
            return false;
        out = *it;
        return true;
    }

    std::vector<TFCharacterRecord> TFDatabase::ListCharacters(uint64_t accountId)
    {
        std::vector<TFCharacterRecord> result;
        if (!RefreshSnapshot())
            return result;
        for (const auto& c : m_snapshot.characters)
            if (c.accountId == accountId)
                result.push_back(c);
        return result;
    }

    bool TFDatabase::FindCharacter(uint64_t charId, TFCharacterRecord& out)
    {
        if (!RefreshSnapshot())
            return false;
        auto it = std::find_if(m_snapshot.characters.begin(), m_snapshot.characters.end(),
                               [&](const TFCharacterRecord& c) { return c.id == charId; });
        if (it == m_snapshot.characters.end())
            return false;
        out = *it;
        return true;
    }

    bool TFDatabase::AcquireCharacter(uint64_t charId, TFCharacterRecord& out)
    {
        if (!FindCharacter(charId, out))
            return false;
        m_baseRevisions[charId] = out.revision;
        return true;
    }

    bool TFDatabase::DeleteCharacter(uint64_t charId)
    {
        const bool committed =
            Transact("DeleteCharacter",
                     [&](Snapshot& fresh, uint64_t)
                     {
                         const auto removed = std::erase_if(fresh.characters,
                                                            [&](const TFCharacterRecord& c) { return c.id == charId; });
                         return removed != 0;
                     });
        if (committed)
            m_baseRevisions.erase(charId);
        return committed;
    }

    bool TFDatabase::SaveCharacterProgress(uint64_t charId, uint32_t xp, uint16_t rank, uint32_t flux,
                                           int64_t lastPlayedMs)
    {
        TFCharacterUpdate update;
        update.charId = charId;
        update.writeProgress = true;
        update.xp = xp;
        update.rank = rank;
        update.flux = flux;
        update.lastPlayedMs = lastPlayedMs;
        return CommitCharacterUpdates({update});
    }

    bool TFDatabase::SaveCharacterMeta(uint64_t charId, const std::vector<std::string>& unlocks,
                                       const std::string& loadoutPrimary, const std::string& loadoutSecondary,
                                       const std::string& loadoutTool, const std::string& loadoutGrenade,
                                       const std::string& loadoutSuit, const std::vector<TFWeaponStatsRow>& stats)
    {
        TFCharacterUpdate update;
        update.charId = charId;
        update.writeMeta = true;
        update.unlocks = unlocks;
        update.loadoutPrimary = loadoutPrimary;
        update.loadoutSecondary = loadoutSecondary;
        update.loadoutTool = loadoutTool;
        update.loadoutGrenade = loadoutGrenade; // loadout-depth wave
        update.loadoutSuit = loadoutSuit;
        update.weaponStats = stats;
        return CommitCharacterUpdates({update});
    }

    bool TFDatabase::CommitCharacterUpdates(const std::vector<TFCharacterUpdate>& updates)
    {
        m_conflictCharId = 0;
        if (!m_open || updates.empty())
            return false;

        // Validate every row before touching anything so a bad row late in the
        // batch cannot leave earlier rows applied.
        std::unordered_set<uint64_t> seenCharacters;
        for (const TFCharacterUpdate& update : updates)
        {
            if (!update.writeProgress && !update.writeMeta)
                return false;
            if (!seenCharacters.insert(update.charId).second)
                return false;
            if (update.writeProgress && (update.rank == 0 || update.rank > kTFMaxRank || update.flux > kFluxWalletCap))
                return false;
            if (update.writeMeta)
            {
                std::unordered_set<std::string> uniqueUnlocks;
                for (const std::string& unlock : update.unlocks)
                    if (unlock.empty() || !uniqueUnlocks.insert(unlock).second)
                        return false;
                std::unordered_set<std::string> uniqueWeapons;
                for (const TFWeaponStatsRow& stat : update.weaponStats)
                    if (stat.weaponKey.empty() || !uniqueWeapons.insert(stat.weaponKey).second)
                        return false;
            }
        }

        uint64_t conflictCharId = 0;
        const bool committed =
            Transact("CommitCharacterUpdates",
                     [&](Snapshot& fresh, uint64_t newRevision)
                     {
                         // These are absolute values computed from an earlier read, so
                         // every row must still be exactly the one this instance based
                         // them on; otherwise another authority's change would be lost.
                         std::vector<TFCharacterRecord*> rows;
                         rows.reserve(updates.size());
                         for (const TFCharacterUpdate& update : updates)
                         {
                             auto it = std::find_if(fresh.characters.begin(), fresh.characters.end(),
                                                    [&](const TFCharacterRecord& c) { return c.id == update.charId; });
                             if (it == fresh.characters.end())
                                 return false;
                             const auto base = m_baseRevisions.find(update.charId);
                             if (base == m_baseRevisions.end() || base->second != it->revision)
                             {
                                 conflictCharId = update.charId;
                                 return false;
                             }
                             rows.push_back(&*it);
                         }

                         for (size_t i = 0; i < updates.size(); ++i)
                         {
                             const TFCharacterUpdate& update = updates[i];
                             TFCharacterRecord& row = *rows[i];
                             if (update.writeProgress)
                             {
                                 row.xp = update.xp;
                                 row.rank = update.rank;
                                 row.flux = update.flux;
                                 row.lastPlayedMs = update.lastPlayedMs;
                             }
                             if (update.writeMeta)
                             {
                                 row.unlocks = update.unlocks;
                                 row.loadoutPrimary = update.loadoutPrimary;
                                 row.loadoutSecondary = update.loadoutSecondary;
                                 row.loadoutTool = update.loadoutTool;
                                 row.loadoutGrenade = update.loadoutGrenade;
                                 row.loadoutSuit = update.loadoutSuit;
                                 row.weaponStats = update.weaponStats;
                             }
                             row.revision = newRevision;
                         }
                         return true;
                     });

        if (conflictCharId != 0)
        {
            m_status = TFDatabaseStatus::Conflict;
            m_conflictCharId = conflictCharId;
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF] character commit of %zu row(s) to %s rejected: character %llu was changed by another "
                           "authority; re-acquire it and retry",
                           updates.size(), SavePaths::Utf8ForLog(m_path).c_str(),
                           static_cast<unsigned long long>(conflictCharId));
            return false;
        }
        if (!committed)
            return false;
        for (const TFCharacterUpdate& update : updates)
            m_baseRevisions[update.charId] = m_snapshot.revision;
        return true;
    }

} // namespace Terrafront
