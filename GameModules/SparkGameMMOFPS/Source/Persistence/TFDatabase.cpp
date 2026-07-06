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

#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Terrafront {

namespace {

/// Read a whole file into a string; false if it does not exist / can't open.
bool ReadAllText(const std::string& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
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

bool TFDatabase::Open(const std::string& path)
{
    m_path = path;

    namespace fs = std::filesystem;
    auto parentPath = fs::path(m_path).parent_path();
    if (!parentPath.empty())
    {
        std::error_code ec;
        fs::create_directories(parentPath, ec);
        if (ec)
            return false;
    }

    m_accounts.clear();
    m_characters.clear();
    m_nextAccountId = 1;
    m_nextCharId = 1;

    std::error_code existsEc;
    const bool dbFileExists = fs::exists(m_path, existsEc);
    if (dbFileExists && !LoadFromDisk())
    {
        // The file is present but failed to parse (corrupt/truncated/foreign
        // content). Do NOT fall through with an empty in-memory db: every
        // mutator flushes eagerly via SaveToDisk(), which would silently
        // overwrite (wipe) the unreadable file with a brand-new empty
        // database on the very next write. Quarantine the bad file and
        // refuse to open instead, so the caller can investigate/restore it.
        std::error_code renameEc;
        const std::string backupPath = m_path + ".corrupt-" + std::to_string(NowMs()) + ".bak";
        fs::rename(m_path, backupPath, renameEc);
        SPARK_LOG_ERROR(Spark::LogCategory::Game,
                        "[TF] db open refused: %s is unreadable/corrupt; backed up to %s (backup ok=%d)",
                        m_path.c_str(), backupPath.c_str(), renameEc ? 0 : 1);
        return false;
    }
    // dbFileExists == false: no prior db, LoadFromDisk() skipped -> fresh db.

    m_open = true;
    return true;
}

void TFDatabase::Close()
{
    if (!m_open)
        return;
    SaveToDisk();   // safety-net flush; mutators already flush eagerly
    m_open = false;
}

bool TFDatabase::LoadFromDisk()
{
    std::string text;
    if (!ReadAllText(m_path, text))
        return false;

    Spark::Json::Value root = Spark::Json::Parse(text);
    if (!root.IsObject())
        return false;

    if (root.HasKey("nextAccountId"))
        m_nextAccountId = static_cast<uint64_t>(root["nextAccountId"].AsNumber(1.0));
    if (root.HasKey("nextCharId"))
        m_nextCharId = static_cast<uint64_t>(root["nextCharId"].AsNumber(1.0));

    if (root.HasKey("accounts") && root["accounts"].IsArray())
    {
        const auto& arr = root["accounts"];
        for (size_t i = 0; i < arr.Size(); ++i)
        {
            const auto& row = arr[i];
            TFAccountRecord rec;
            rec.id           = static_cast<uint64_t>(row["id"].AsNumber(0.0));
            rec.username     = row["username"].AsString();
            rec.salt         = row["salt"].AsString();
            rec.passwordHash = row["passwordHash"].AsString();
            rec.createdAtMs  = static_cast<int64_t>(row["createdAtMs"].AsNumber(0.0));
            rec.lastLoginMs  = static_cast<int64_t>(row["lastLoginMs"].AsNumber(0.0));
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
            rec.id           = static_cast<uint64_t>(row["id"].AsNumber(0.0));
            rec.accountId    = static_cast<uint64_t>(row["accountId"].AsNumber(0.0));
            rec.name         = row["name"].AsString();
            rec.faction      = static_cast<FactionId>(static_cast<uint8_t>(row["faction"].AsNumber(0.0)));
            rec.xp           = static_cast<uint32_t>(row["xp"].AsNumber(0.0));
            rec.rank         = static_cast<uint16_t>(row["rank"].AsNumber(1.0));
            rec.flux         = static_cast<uint32_t>(row["flux"].AsNumber(0.0));
            rec.createdAtMs  = static_cast<int64_t>(row["createdAtMs"].AsNumber(0.0));
            rec.lastPlayedMs = static_cast<int64_t>(row["lastPlayedMs"].AsNumber(0.0));
            m_characters.push_back(std::move(rec));
        }
    }

    return true;
}

bool TFDatabase::SaveToDisk() const
{
    namespace fs = std::filesystem;

    Spark::Json::Value root = Spark::Json::Value::MakeObject();
    root["nextAccountId"] = Spark::Json::Value(static_cast<double>(m_nextAccountId));
    root["nextCharId"]    = Spark::Json::Value(static_cast<double>(m_nextCharId));

    Spark::Json::Value accounts = Spark::Json::Value::MakeArray();
    for (const auto& a : m_accounts)
    {
        Spark::Json::Value row = Spark::Json::Value::MakeObject();
        row["id"]           = Spark::Json::Value(static_cast<double>(a.id));
        row["username"]     = Spark::Json::Value(a.username);
        row["salt"]         = Spark::Json::Value(a.salt);
        row["passwordHash"] = Spark::Json::Value(a.passwordHash);
        row["createdAtMs"]  = Spark::Json::Value(static_cast<double>(a.createdAtMs));
        row["lastLoginMs"]  = Spark::Json::Value(static_cast<double>(a.lastLoginMs));
        accounts.PushBack(std::move(row));
    }
    root["accounts"] = std::move(accounts);

    Spark::Json::Value characters = Spark::Json::Value::MakeArray();
    for (const auto& c : m_characters)
    {
        Spark::Json::Value row = Spark::Json::Value::MakeObject();
        row["id"]           = Spark::Json::Value(static_cast<double>(c.id));
        row["accountId"]    = Spark::Json::Value(static_cast<double>(c.accountId));
        row["name"]         = Spark::Json::Value(c.name);
        row["faction"]      = Spark::Json::Value(static_cast<double>(static_cast<uint8_t>(c.faction)));
        row["xp"]           = Spark::Json::Value(static_cast<double>(c.xp));
        row["rank"]         = Spark::Json::Value(static_cast<double>(c.rank));
        row["flux"]         = Spark::Json::Value(static_cast<double>(c.flux));
        row["createdAtMs"]  = Spark::Json::Value(static_cast<double>(c.createdAtMs));
        row["lastPlayedMs"] = Spark::Json::Value(static_cast<double>(c.lastPlayedMs));
        characters.PushBack(std::move(row));
    }
    root["characters"] = std::move(characters);

    std::error_code ec;
    auto parentPath = fs::path(m_path).parent_path();
    if (!parentPath.empty())
        fs::create_directories(parentPath, ec);

    const std::string tmpFile = m_path + ".tmp";
    {
        std::ofstream out(tmpFile, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return false;
        out << Spark::Json::StringifyPretty(root);
        if (!out.good())
            return false;
    }

    fs::rename(tmpFile, m_path, ec);   // atomic replace (MoveFileEx semantics)
    if (ec)
    {
        fs::remove(m_path, ec);
        fs::rename(tmpFile, m_path, ec);
        if (ec)
            return false;
    }

    return true;
}

bool TFDatabase::CreateAccount(const std::string& username, const std::string& salt, const std::string& hash,
                                TFAccountRecord& out)
{
    TFAccountRecord existing;
    if (FindAccountByUsername(username, existing))
        return false;   // username taken

    TFAccountRecord rec;
    rec.id           = m_nextAccountId++;
    rec.username     = username;
    rec.salt         = salt;
    rec.passwordHash = hash;
    rec.createdAtMs  = NowMs();
    rec.lastLoginMs  = 0;
    m_accounts.push_back(rec);

    if (!SaveToDisk())
        SPARK_LOG_ERROR(Spark::LogCategory::Game,
                        "[TF] CreateAccount: account '%s' (id=%llu) created in memory but SaveToDisk failed for %s",
                        rec.username.c_str(), static_cast<unsigned long long>(rec.id), m_path.c_str());
    out = rec;
    return true;
}

bool TFDatabase::FindAccountByUsername(const std::string& username, TFAccountRecord& out)
{
    auto it = std::find_if(m_accounts.begin(), m_accounts.end(),
                            [&](const TFAccountRecord& a) { return a.username == username; });
    if (it == m_accounts.end())
        return false;
    out = *it;
    return true;
}

void TFDatabase::TouchLogin(uint64_t accountId, int64_t nowMs)
{
    auto it = std::find_if(m_accounts.begin(), m_accounts.end(),
                            [&](const TFAccountRecord& a) { return a.id == accountId; });
    if (it == m_accounts.end())
        return;
    it->lastLoginMs = nowMs;
    if (!SaveToDisk())
        SPARK_LOG_ERROR(Spark::LogCategory::Game,
                        "[TF] TouchLogin: SaveToDisk failed for account id=%llu (%s)",
                        static_cast<unsigned long long>(accountId), m_path.c_str());
}

bool TFDatabase::CreateCharacter(uint64_t accountId, const std::string& name, FactionId faction,
                                  TFCharacterRecord& out)
{
    TFCharacterRecord existing;
    if (FindCharacterByName(name, existing))
        return false;   // name taken

    TFCharacterRecord rec;
    rec.id           = m_nextCharId++;
    rec.accountId    = accountId;
    rec.name         = name;
    rec.faction      = faction;
    rec.xp           = 0;
    rec.rank         = 1;
    rec.flux         = 0;
    rec.createdAtMs  = NowMs();
    rec.lastPlayedMs = 0;
    m_characters.push_back(rec);

    if (!SaveToDisk())
        SPARK_LOG_ERROR(Spark::LogCategory::Game,
                        "[TF] CreateCharacter: character '%s' (id=%llu) created in memory but SaveToDisk failed for %s",
                        rec.name.c_str(), static_cast<unsigned long long>(rec.id), m_path.c_str());
    out = rec;
    return true;
}

bool TFDatabase::FindCharacterByName(const std::string& name, TFCharacterRecord& out)
{
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
    for (const auto& c : m_characters)
        if (c.accountId == accountId)
            result.push_back(c);
    return result;
}

bool TFDatabase::FindCharacter(uint64_t charId, TFCharacterRecord& out)
{
    auto it = std::find_if(m_characters.begin(), m_characters.end(),
                            [&](const TFCharacterRecord& c) { return c.id == charId; });
    if (it == m_characters.end())
        return false;
    out = *it;
    return true;
}

bool TFDatabase::DeleteCharacter(uint64_t charId)
{
    auto it = std::find_if(m_characters.begin(), m_characters.end(),
                            [&](const TFCharacterRecord& c) { return c.id == charId; });
    if (it == m_characters.end())
        return false;
    m_characters.erase(it);
    if (!SaveToDisk())
        SPARK_LOG_ERROR(Spark::LogCategory::Game,
                        "[TF] DeleteCharacter: character id=%llu removed in memory but SaveToDisk failed for %s",
                        static_cast<unsigned long long>(charId), m_path.c_str());
    return true;
}

void TFDatabase::SaveCharacterProgress(uint64_t charId, uint32_t xp, uint16_t rank, uint32_t flux,
                                        int64_t lastPlayedMs)
{
    auto it = std::find_if(m_characters.begin(), m_characters.end(),
                            [&](const TFCharacterRecord& c) { return c.id == charId; });
    if (it == m_characters.end())
        return;
    it->xp = xp;
    it->rank = rank;
    it->flux = flux;
    it->lastPlayedMs = lastPlayedMs;
    if (!SaveToDisk())
        SPARK_LOG_ERROR(Spark::LogCategory::Game,
                        "[TF] SaveCharacterProgress: SaveToDisk failed for character id=%llu (%s)",
                        static_cast<unsigned long long>(charId), m_path.c_str());
}

} // namespace Terrafront
