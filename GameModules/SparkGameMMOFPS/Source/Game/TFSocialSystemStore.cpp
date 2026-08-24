/**
 * @file TFSocialSystemStore.cpp
 * @brief TFSocialSystem persistence: the atomic-JSON social store
 *        ("terrafront_social.json" under the shared save root, using the
 *        TFDatabase tmp+rename pattern) with
 *        strict-parse quarantine and the debounced flush. Split from
 *        TFSocialSystem.cpp; the shared helpers live in
 *        TFSocialSystemInternal.h.
 */
#include "Game/TFSocialSystem.h"

#include "Game/TFSocialSystemInternal.h"
#include "Persistence/TFSavePaths.h"

#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace Terrafront
{

    using namespace SocialDetail;

    namespace
    {

        constexpr float kSocialSaveDebounceSec = 5.0f; // recent-list flush debounce
        constexpr uint64_t kMaxExactJsonInteger = 9007199254740991ULL;

        bool HasCanonicalSchemaLexemes(std::string_view text, std::string& detail)
        {
            using ObjectKeys = std::unordered_set<std::string>;
            std::vector<std::optional<ObjectKeys>> containers;
            for (size_t i = 0; i < text.size();)
            {
                if (text[i] == '"')
                {
                    const size_t start = i++;
                    while (i < text.size() && text[i] != '"')
                        i += text[i] == '\\' && i + 1 < text.size() ? 2 : 1;
                    if (i < text.size())
                        ++i;

                    size_t after = i;
                    while (after < text.size() &&
                           (text[after] == ' ' || text[after] == '\t' || text[after] == '\r' || text[after] == '\n'))
                    {
                        ++after;
                    }
                    if (after < text.size() && text[after] == ':')
                    {
                        Spark::Json::Value key;
                        if (containers.empty() || !containers.back().has_value() ||
                            !Spark::Json::ParseStrict(text.substr(start, i - start), &key) || !key.IsString() ||
                            !containers.back()->insert(key.AsString()).second)
                        {
                            detail = "object contains a duplicate or invalid field name";
                            return false;
                        }
                    }
                    continue;
                }

                if (text[i] == '{')
                {
                    containers.emplace_back(ObjectKeys{});
                    ++i;
                    continue;
                }
                if (text[i] == '[')
                {
                    containers.emplace_back(std::nullopt);
                    ++i;
                    continue;
                }
                if (text[i] == '}' || text[i] == ']')
                {
                    if (!containers.empty())
                        containers.pop_back();
                    ++i;
                    continue;
                }

                const bool startsNumber = text[i] == '-' || (text[i] >= '0' && text[i] <= '9');
                if (!startsNumber)
                {
                    ++i;
                    continue;
                }

                const size_t start = i++;
                while (i < text.size())
                {
                    const char c = text[i];
                    if (!((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-'))
                        break;
                    ++i;
                }
                const std::string_view number = text.substr(start, i - start);
                if (number.front() == '-' || number.find_first_of(".eE") != std::string_view::npos)
                {
                    detail = "numeric fields must use non-negative decimal integer syntax";
                    return false;
                }
            }
            return true;
        }

        bool HasExactKeys(const Spark::Json::Value& object, std::initializer_list<std::string_view> expected)
        {
            if (!object.IsObject() || object.GetKeys().size() != expected.size())
                return false;
            return std::all_of(expected.begin(), expected.end(),
                               [&object](std::string_view key) { return object.HasKey(std::string(key)); });
        }

        bool ReadPositiveId(const Spark::Json::Value& value, uint64_t& out)
        {
            if (!value.IsNumber())
                return false;
            const double number = value.AsNumber(-1.0);
            if (!std::isfinite(number) || number < 1.0 || number > static_cast<double>(kMaxExactJsonInteger) ||
                std::trunc(number) != number)
            {
                return false;
            }
            out = static_cast<uint64_t>(number);
            return true;
        }

        bool ReadTimestamp(const Spark::Json::Value& value, int64_t& out)
        {
            if (!value.IsNumber())
                return false;
            const double number = value.AsNumber(-1.0);
            if (!std::isfinite(number) || number < 0.0 || number > static_cast<double>(kMaxExactJsonInteger) ||
                std::trunc(number) != number)
            {
                return false;
            }
            out = static_cast<int64_t>(number);
            return true;
        }

        bool IsValidStoredName(const std::string& name)
        {
            if (name.size() < 3 || name.size() >= kTFSocialNameLen)
                return false;

            bool previousSpace = false;
            for (size_t i = 0; i < name.size(); ++i)
            {
                const unsigned char c = static_cast<unsigned char>(name[i]);
                const bool isAsciiAlnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                if (c == ' ')
                {
                    if (i == 0 || i + 1 == name.size() || previousSpace)
                        return false;
                    previousSpace = true;
                }
                else
                {
                    if (!isAsciiAlnum)
                        return false;
                    previousSpace = false;
                }
            }
            return true;
        }

        std::string FoldName(const std::string& name)
        {
            std::string folded = name;
            for (char& c : folded)
            {
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            }
            return folded;
        }

        bool FindRecoveryBackup(const std::filesystem::path& storePath, std::filesystem::path& found,
                                std::error_code& ec)
        {
            namespace fs = std::filesystem;
            found.clear();
            fs::path directory = storePath.parent_path();
            if (directory.empty())
                directory = fs::path(".");
            if (!fs::exists(directory, ec))
                return false;
            if (ec)
                return false;

            fs::path prefixPath = storePath.filename();
            prefixPath += ".corrupt-";
            const auto prefix = prefixPath.native();
            const auto suffix = fs::path(".bak").native();
            for (fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec))
            {
                const auto name = it->path().filename().native();
                if (name.size() >= prefix.size() + suffix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
                    name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
                {
                    found = it->path();
                    return true;
                }
            }
            return false;
        }

        bool CreateRecoveryBackup(const std::filesystem::path& storePath, std::filesystem::path& backup,
                                  std::error_code& ec)
        {
            namespace fs = std::filesystem;
            const std::string stamp = std::to_string(NowMs());
            for (unsigned int attempt = 0; attempt < 1000; ++attempt)
            {
                backup = storePath;
                backup += ".corrupt-" + stamp;
                if (attempt != 0)
                    backup += "-" + std::to_string(attempt);
                backup += ".bak";

                ec.clear();
                if (fs::copy_file(storePath, backup, fs::copy_options::none, ec))
                    return true;
                if (ec != std::errc::file_exists)
                    return false;
            }
            ec = std::make_error_code(std::errc::file_exists);
            return false;
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // Persistence (atomic JSON, TFDatabase tmp+rename pattern)
    // ---------------------------------------------------------------------------

    bool TFSocialSystem::ParseStoreDocument(std::string_view text, std::unordered_map<uint64_t, SocialRecord>& loaded,
                                            std::string& detail)
    {
        loaded.clear();
        detail.clear();

        Spark::Json::Value root;
        if (!Spark::Json::ParseStrict(text, &root, &detail))
            return false;
        // The JSON DOM stores numbers as doubles. Inspect the original tokens
        // before reading values so a fractional lexeme that rounds to an
        // integer (or negative zero) cannot be silently normalized on save.
        if (!HasCanonicalSchemaLexemes(text, detail))
            return false;
        if (!HasExactKeys(root, {"characters"}) || !root["characters"].IsArray())
        {
            detail = "root must contain only a characters array";
            return false;
        }

        const auto readNameArray = [&detail](const Spark::Json::Value& row, const char* field, size_t cap,
                                             std::vector<std::string>& output) -> bool
        {
            const auto& array = row[field];
            if (!array.IsArray() || array.Size() > cap)
            {
                detail = std::string(field) + " must be an array within its configured limit";
                return false;
            }

            std::unordered_set<std::string> names;
            output.reserve(array.Size());
            for (size_t i = 0; i < array.Size(); ++i)
            {
                if (!array[i].IsString() || !IsValidStoredName(array[i].AsString()))
                {
                    detail = std::string(field) + " contains an invalid name at index " + std::to_string(i);
                    return false;
                }
                const std::string& name = array[i].AsString();
                if (!names.insert(FoldName(name)).second)
                {
                    detail = std::string(field) + " contains a duplicate name";
                    return false;
                }
                output.push_back(name);
            }
            return true;
        };

        std::unordered_set<uint64_t> characterIds;
        const auto& characters = root["characters"];
        for (size_t i = 0; i < characters.Size(); ++i)
        {
            const auto& row = characters[i];
            if (!HasExactKeys(row, {"charId", "friends", "blocked", "recent"}))
            {
                detail = "character row " + std::to_string(i) + " has missing or unknown fields";
                return false;
            }

            uint64_t charId = 0;
            if (!ReadPositiveId(row["charId"], charId) || !characterIds.insert(charId).second)
            {
                detail = "character row " + std::to_string(i) + " has an invalid or duplicate charId";
                return false;
            }

            SocialRecord record;
            if (!readNameArray(row, "friends", kTFMaxFriends, record.friends) ||
                !readNameArray(row, "blocked", kTFMaxBlocked, record.blocked))
            {
                detail = "character row " + std::to_string(i) + ": " + detail;
                return false;
            }

            std::unordered_set<std::string> friendNames;
            for (const std::string& name : record.friends)
                friendNames.insert(FoldName(name));
            for (const std::string& name : record.blocked)
            {
                if (friendNames.contains(FoldName(name)))
                {
                    detail = "character row " + std::to_string(i) + " lists a name as both friend and blocked";
                    return false;
                }
            }

            const auto& recent = row["recent"];
            if (!recent.IsArray() || recent.Size() > kTFMaxRecent)
            {
                detail = "character row " + std::to_string(i) + " has an invalid recent array";
                return false;
            }
            std::unordered_set<std::string> recentNames;
            record.recent.reserve(recent.Size());
            for (size_t r = 0; r < recent.Size(); ++r)
            {
                const auto& recentRow = recent[r];
                if (!HasExactKeys(recentRow, {"name", "lastSeenMs"}) || !recentRow["name"].IsString() ||
                    !IsValidStoredName(recentRow["name"].AsString()))
                {
                    detail = "character row " + std::to_string(i) + " has an invalid recent entry at index " +
                             std::to_string(r);
                    return false;
                }

                RecentRec recentRecord;
                recentRecord.name = recentRow["name"].AsString();
                if (!recentNames.insert(FoldName(recentRecord.name)).second ||
                    !ReadTimestamp(recentRow["lastSeenMs"], recentRecord.lastSeenMs))
                {
                    detail = "character row " + std::to_string(i) + " has a duplicate recent name or invalid timestamp";
                    return false;
                }
                record.recent.push_back(std::move(recentRecord));
            }

            loaded.emplace(charId, std::move(record));
        }
        return true;
    }

    TFSocialSystem::StoreLoadStatus TFSocialSystem::LoadStoreFromPath(
        const std::filesystem::path& path, std::unordered_map<uint64_t, SocialRecord>& loaded, std::string& detail)
    {
        namespace fs = std::filesystem;
        loaded.clear();
        detail.clear();

        std::error_code ec;
        const bool exists = fs::exists(path, ec);
        if (ec)
        {
            detail = "stat failed: " + ec.message();
            return StoreLoadStatus::Unreadable;
        }
        if (!exists)
        {
            fs::path recoveryBackup;
            if (FindRecoveryBackup(path, recoveryBackup, ec))
            {
                detail = "primary is missing while recovery backup exists at " + SavePaths::Utf8ForLog(recoveryBackup);
                return StoreLoadStatus::RecoveryRequired;
            }
            if (ec)
            {
                detail = "recovery-backup scan failed: " + ec.message();
                return StoreLoadStatus::Unreadable;
            }
            return StoreLoadStatus::Missing;
        }

        std::ifstream input(path, std::ios::binary);
        if (!input.is_open())
        {
            detail = "open failed";
            return StoreLoadStatus::Unreadable;
        }
        std::ostringstream stream;
        stream << input.rdbuf();
        if (input.bad())
        {
            detail = "read failed";
            return StoreLoadStatus::Unreadable;
        }

        if (ParseStoreDocument(stream.str(), loaded, detail))
            return StoreLoadStatus::Loaded;

        fs::path backup;
        std::error_code backupEc;
        if (CreateRecoveryBackup(path, backup, backupEc))
            detail += "; corrupt primary preserved; recovery backup written to " + SavePaths::Utf8ForLog(backup);
        else
            detail += "; corrupt primary preserved; recovery backup failed: " + backupEc.message();
        loaded.clear();
        return StoreLoadStatus::Corrupt;
    }

    bool TFSocialSystem::StoreEnsureLoaded()
    {
        if (m_storeLoaded)
            return true;
        if (m_storeLoadFailed)
            return false;

        std::unordered_map<uint64_t, SocialRecord> loaded;
        std::string detail;
        const StoreLoadStatus status = LoadStoreFromPath(m_storePath, loaded, detail);
        if (status == StoreLoadStatus::Missing)
        {
            m_store.clear();
            m_storeLoaded = true;
            return true;
        }
        if (status == StoreLoadStatus::Loaded)
        {
            m_store = std::move(loaded);
            m_storeLoaded = true;
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] social store loaded (%zu character(s))", m_store.size());
            return true;
        }

        m_storeLoadFailed = true;
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] social store %s rejected; writes latched off (%s)",
                        SavePaths::Utf8ForLog(m_storePath).c_str(), detail.c_str());
        return false;
    }

#ifdef SPARK_SOCIAL_STORE_TESTS
    bool TFSocialSystem::ValidateStoreJsonForTesting(std::string_view text, std::string* detail)
    {
        std::unordered_map<uint64_t, SocialRecord> loaded;
        std::string validationDetail;
        const bool valid = ParseStoreDocument(text, loaded, validationDetail);
        if (detail)
            *detail = std::move(validationDetail);
        return valid;
    }

    TFSocialSystem::StoreLoadTestResult TFSocialSystem::LoadStoreForTesting(const std::filesystem::path& path)
    {
        std::unordered_map<uint64_t, SocialRecord> loaded;
        StoreLoadTestResult result;
        const StoreLoadStatus status = LoadStoreFromPath(path, loaded, result.detail);
        result.accepted = status == StoreLoadStatus::Missing || status == StoreLoadStatus::Loaded;
        result.missing = status == StoreLoadStatus::Missing;
        result.recordCount = loaded.size();
        return result;
    }
#endif

    bool TFSocialSystem::StoreSaveToDisk() const
    {
        namespace fs = std::filesystem;

        Spark::Json::Value root = Spark::Json::Value::MakeObject();
        Spark::Json::Value characters = Spark::Json::Value::MakeArray();
        for (const auto& [charId, rec] : m_store)
        {
            if (charId == 0 || charId > kMaxExactJsonInteger ||
                std::any_of(rec.recent.begin(), rec.recent.end(),
                            [](const RecentRec& recent)
                            {
                                return recent.lastSeenMs < 0 ||
                                       recent.lastSeenMs > static_cast<int64_t>(kMaxExactJsonInteger);
                            }))
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game,
                                "[TF] social save refused: in-memory record contains an unsafe integer");
                return false;
            }
            if (rec.friends.empty() && rec.blocked.empty() && rec.recent.empty())
                continue;
            Spark::Json::Value row = Spark::Json::Value::MakeObject();
            row["charId"] = Spark::Json::Value(static_cast<double>(charId));

            Spark::Json::Value friends = Spark::Json::Value::MakeArray();
            for (const std::string& n : rec.friends)
                friends.PushBack(Spark::Json::Value(n));
            row["friends"] = std::move(friends);

            Spark::Json::Value blocked = Spark::Json::Value::MakeArray();
            for (const std::string& n : rec.blocked)
                blocked.PushBack(Spark::Json::Value(n));
            row["blocked"] = std::move(blocked);

            Spark::Json::Value recent = Spark::Json::Value::MakeArray();
            for (const RecentRec& r : rec.recent)
            {
                Spark::Json::Value rrow = Spark::Json::Value::MakeObject();
                rrow["name"] = Spark::Json::Value(r.name);
                rrow["lastSeenMs"] = Spark::Json::Value(static_cast<double>(r.lastSeenMs));
                recent.PushBack(std::move(rrow));
            }
            row["recent"] = std::move(recent);

            characters.PushBack(std::move(row));
        }
        root["characters"] = std::move(characters);

        std::error_code ec;
        const auto parentPath = m_storePath.parent_path();
        if (!parentPath.empty())
        {
            fs::create_directories(parentPath, ec);
            if (ec)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] social save failed: cannot create %s (%s)",
                                SavePaths::Utf8ForLog(parentPath).c_str(), ec.message().c_str());
                return false;
            }
        }

        std::filesystem::path tmpFile = m_storePath;
        tmpFile += ".tmp";
        {
            std::ofstream out(tmpFile, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
                return false;
            out << Spark::Json::StringifyPretty(root);
            if (!out.good())
                return false;
        }

        return SavePaths::AtomicReplace(tmpFile, m_storePath, ec);
    }

    void TFSocialSystem::StoreFlushIfDue(float dt)
    {
        if (!m_storeDirty)
        {
            m_saveAccum = 0.0f;
            return;
        }
        m_saveAccum += dt;
        if (m_saveAccum < kSocialSaveDebounceSec)
            return;
        m_saveAccum = 0.0f;
        if (StoreSaveToDisk())
            m_storeDirty = false;
    }

} // namespace Terrafront
