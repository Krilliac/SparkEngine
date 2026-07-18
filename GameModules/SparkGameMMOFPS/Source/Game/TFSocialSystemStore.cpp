/**
 * @file TFSocialSystemStore.cpp
 * @brief TFSocialSystem persistence: the atomic-JSON social store
 *        ("Saves/terrafront_social.json", TFDatabase tmp+rename pattern) with
 *        strict-parse quarantine and the debounced flush. Split from
 *        TFSocialSystem.cpp; the shared helpers live in
 *        TFSocialSystemInternal.h.
 */
#include "Game/TFSocialSystem.h"

#include "Game/TFSocialSystemInternal.h"

#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace Terrafront
{

    using namespace SocialDetail;

    namespace
    {

        constexpr float kSocialSaveDebounceSec = 5.0f; // recent-list flush debounce

    } // namespace

    // ---------------------------------------------------------------------------
    // Persistence (atomic JSON, TFDatabase tmp+rename pattern)
    // ---------------------------------------------------------------------------

    bool TFSocialSystem::StoreEnsureLoaded()
    {
        if (m_storeLoaded)
            return true;
        if (m_storeLoadFailed)
            return false;

        namespace fs = std::filesystem;
        std::error_code existsEc;
        if (!fs::exists(m_storePath, existsEc))
        {
            m_storeLoaded = true; // fresh store
            return true;
        }

        std::ifstream in(m_storePath, std::ios::binary);
        std::ostringstream ss;
        if (in.is_open())
            ss << in.rdbuf();
        in.close();

        // Strict parse (W9): the lenient Parse accepts truncated/torn files as a
        // partial object, which could pass the structural checks below with
        // silently dropped rows; strict rejection routes them into quarantine.
        Spark::Json::Value root;
        std::string parseError;
        const bool parsedOk = Spark::Json::ParseStrict(ss.str(), &root, &parseError);
        if (!parsedOk)
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] social store %s rejected by strict JSON parse: %s",
                            m_storePath.c_str(), parseError.c_str());
        if (!parsedOk || !root.IsObject() || !root.HasKey("characters") || !root["characters"].IsArray())
        {
            // Corrupt/foreign content: quarantine instead of silently overwriting
            // on the next flush (TFDatabase::Open precedent).
            std::error_code renameEc;
            const std::string backup = m_storePath + ".corrupt-" + std::to_string(NowMs()) + ".bak";
            fs::rename(m_storePath, backup, renameEc);
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] social store unreadable; quarantined to %s (ok=%d)",
                            backup.c_str(), renameEc ? 0 : 1);
            m_storeLoadFailed = renameEc ? true : false; // writable again once quarantined
            m_storeLoaded = !m_storeLoadFailed;
            return m_storeLoaded;
        }

        const auto& arr = root["characters"];
        for (size_t i = 0; i < arr.Size(); ++i)
        {
            const auto& row = arr[i];
            if (!row.IsObject())
                continue;
            const auto charId = static_cast<uint64_t>(row["charId"].AsNumber(0.0));
            if (charId == 0)
                continue;
            SocialRecord rec;
            if (row.HasKey("friends") && row["friends"].IsArray())
            {
                const auto& friends = row["friends"];
                for (size_t f = 0; f < friends.Size(); ++f)
                    if (friends[f].IsString())
                        rec.friends.push_back(friends[f].AsString());
            }
            if (row.HasKey("blocked") && row["blocked"].IsArray())
            {
                const auto& blocked = row["blocked"];
                for (size_t b = 0; b < blocked.Size(); ++b)
                    if (blocked[b].IsString())
                        rec.blocked.push_back(blocked[b].AsString());
            }
            if (row.HasKey("recent") && row["recent"].IsArray())
            {
                const auto& recent = row["recent"];
                for (size_t r = 0; r < recent.Size(); ++r)
                {
                    const auto& rrow = recent[r];
                    if (!rrow.IsObject() || !rrow["name"].IsString())
                        continue;
                    RecentRec rr;
                    rr.name = rrow["name"].AsString();
                    rr.lastSeenMs = static_cast<int64_t>(rrow["lastSeenMs"].AsNumber(0.0));
                    rec.recent.push_back(std::move(rr));
                }
            }
            m_store[charId] = std::move(rec);
        }

        m_storeLoaded = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] social store loaded (%zu character(s))", m_store.size());
        return true;
    }

    bool TFSocialSystem::StoreSaveToDisk() const
    {
        namespace fs = std::filesystem;

        Spark::Json::Value root = Spark::Json::Value::MakeObject();
        Spark::Json::Value characters = Spark::Json::Value::MakeArray();
        for (const auto& [charId, rec] : m_store)
        {
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
        const auto parentPath = fs::path(m_storePath).parent_path();
        if (!parentPath.empty())
            fs::create_directories(parentPath, ec);

        const std::string tmpFile = m_storePath + ".tmp";
        {
            std::ofstream out(tmpFile, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
                return false;
            out << Spark::Json::StringifyPretty(root);
            if (!out.good())
                return false;
        }

        fs::rename(tmpFile, m_storePath, ec); // atomic replace (MoveFileEx semantics)
        if (ec)
        {
            fs::remove(m_storePath, ec);
            fs::rename(tmpFile, m_storePath, ec);
            if (ec)
                return false;
        }
        return true;
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
