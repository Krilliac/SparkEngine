/**
 * @file TFOutfitStoreDisk.cpp
 * @brief TFOutfitStore disk round-trip: strict-JSON load and atomic
 *        tmp+rename save (Spark::Json; additive keys, tolerant loads).
 *        Split from TFOutfitStore.cpp.
 */
#include "Persistence/TFOutfitStore.h"

#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace Terrafront
{

    namespace
    {

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

        TFOutfitRank RankFromNumber(double n)
        {
            const uint8_t v = static_cast<uint8_t>(n);
            if (v >= static_cast<uint8_t>(TFOutfitRank::Leader))
                return TFOutfitRank::Leader;
            return static_cast<TFOutfitRank>(v);
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // Disk round-trip (Spark::Json; additive keys, tolerant loads)
    // ---------------------------------------------------------------------------

    bool TFOutfitStore::LoadFromDisk()
    {
        std::string text;
        if (!ReadAllText(m_path, text))
            return false;

        // Strict parse (W9): the lenient Parse accepts truncated/torn files as a
        // partial object, which silently loaded an empty store and wiped the file
        // on the next save. A strict failure here makes Open()'s quarantine path
        // actually trigger.
        Spark::Json::Value root;
        std::string parseError;
        if (!Spark::Json::ParseStrict(text, &root, &parseError))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] outfit store %s rejected by strict JSON parse: %s",
                            m_path.c_str(), parseError.c_str());
            return false;
        }
        if (!root.IsObject())
            return false;

        if (root.HasKey("nextOutfitId"))
            m_nextOutfitId = static_cast<uint32_t>(root["nextOutfitId"].AsNumber(1.0));

        if (root.HasKey("outfits") && root["outfits"].IsArray())
        {
            const auto& arr = root["outfits"];
            for (size_t i = 0; i < arr.Size(); ++i)
            {
                const auto& row = arr[i];
                if (!row.IsObject())
                    continue;
                TFOutfitRecord rec;
                rec.id = static_cast<uint32_t>(row["id"].AsNumber(0.0));
                rec.name = row["name"].AsString();
                rec.tag = row["tag"].AsString();
                rec.createdAtMs = static_cast<int64_t>(row["createdAtMs"].AsNumber(0.0));
                // W12 competition score — additive keys, absent loads as 0.
                rec.weeklyScore = static_cast<uint64_t>(row["weeklyScore"].AsNumber(0.0));
                rec.allTimeScore = static_cast<uint64_t>(row["allTimeScore"].AsNumber(0.0));
                rec.weekKey = static_cast<uint32_t>(row["weekKey"].AsNumber(0.0));
                if (rec.id == 0 || rec.name.empty())
                    continue;

                if (row.HasKey("members") && row["members"].IsArray())
                {
                    const auto& members = row["members"];
                    for (size_t mi = 0; mi < members.Size(); ++mi)
                    {
                        const auto& mrow = members[mi];
                        if (!mrow.IsObject())
                            continue;
                        TFOutfitMemberRecord m;
                        m.charId = static_cast<uint64_t>(mrow["charId"].AsNumber(0.0));
                        m.name = mrow["name"].AsString();
                        m.rank = RankFromNumber(mrow["rank"].AsNumber(0.0));
                        m.joinedAtMs = static_cast<int64_t>(mrow["joinedAtMs"].AsNumber(0.0));
                        if (m.charId != 0)
                            rec.members.push_back(std::move(m));
                    }
                }

                // keep the id allocator ahead of every persisted row even if
                // "nextOutfitId" is absent/stale in an old file
                if (rec.id >= m_nextOutfitId)
                    m_nextOutfitId = rec.id + 1;
                m_outfits.push_back(std::move(rec));
            }
        }

        return true;
    }

    bool TFOutfitStore::WriteToDisk() const
    {
        namespace fs = std::filesystem;

        Spark::Json::Value root = Spark::Json::Value::MakeObject();
        root["nextOutfitId"] = Spark::Json::Value(static_cast<double>(m_nextOutfitId));

        Spark::Json::Value outfits = Spark::Json::Value::MakeArray();
        for (const TFOutfitRecord& rec : m_outfits)
        {
            Spark::Json::Value row = Spark::Json::Value::MakeObject();
            row["id"] = Spark::Json::Value(static_cast<double>(rec.id));
            row["name"] = Spark::Json::Value(rec.name);
            row["tag"] = Spark::Json::Value(rec.tag);
            row["createdAtMs"] = Spark::Json::Value(static_cast<double>(rec.createdAtMs));
            row["weeklyScore"] = Spark::Json::Value(static_cast<double>(rec.weeklyScore));
            row["allTimeScore"] = Spark::Json::Value(static_cast<double>(rec.allTimeScore));
            row["weekKey"] = Spark::Json::Value(static_cast<double>(rec.weekKey));

            Spark::Json::Value members = Spark::Json::Value::MakeArray();
            for (const TFOutfitMemberRecord& m : rec.members)
            {
                Spark::Json::Value mrow = Spark::Json::Value::MakeObject();
                mrow["charId"] = Spark::Json::Value(static_cast<double>(m.charId));
                mrow["name"] = Spark::Json::Value(m.name);
                mrow["rank"] = Spark::Json::Value(static_cast<double>(static_cast<uint8_t>(m.rank)));
                mrow["joinedAtMs"] = Spark::Json::Value(static_cast<double>(m.joinedAtMs));
                members.PushBack(std::move(mrow));
            }
            row["members"] = std::move(members);

            outfits.PushBack(std::move(row));
        }
        root["outfits"] = std::move(outfits);

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

        fs::rename(tmpFile, m_path, ec); // atomic replace (MoveFileEx semantics)
        if (ec)
        {
            fs::remove(m_path, ec);
            fs::rename(tmpFile, m_path, ec);
            if (ec)
                return false;
        }

        return true;
    }

} // namespace Terrafront
