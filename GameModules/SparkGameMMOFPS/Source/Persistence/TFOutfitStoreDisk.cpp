/**
 * @file TFOutfitStoreDisk.cpp
 * @brief TFOutfitStore disk round-trip: strict-JSON load and atomic
 *        tmp+rename save (Spark::Json; additive keys, tolerant loads).
 *        Split from TFOutfitStore.cpp.
 */
#include "Persistence/TFOutfitStore.h"
#include "Persistence/TFJsonStrict.h"
#include "Persistence/TFSavePaths.h"

#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_set>

namespace Terrafront
{

    namespace
    {

        constexpr size_t kOutfitNameMin = 3;
        constexpr size_t kOutfitNameMax = 24;
        constexpr size_t kOutfitTagMin = 2;
        constexpr size_t kOutfitTagMax = 5;
        constexpr size_t kMaxOutfitMembers = 128;

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
            constexpr double kMaxExactJsonInteger = 9007199254740991.0;
            const double maximum =
                std::min(kMaxExactJsonInteger, static_cast<double>(std::numeric_limits<UInt>::max()));
            if (!std::isfinite(number) || number < 0.0 || number > maximum || std::trunc(number) != number)
                return false;
            out = static_cast<UInt>(number);
            return true;
        }

        template <typename UInt> bool ReadOptionalUnsigned(const Spark::Json::Value& object, const char* key, UInt& out)
        {
            out = 0;
            return !object.HasKey(key) || ReadUnsigned(object[key], out);
        }

        bool ReadInt64(const Spark::Json::Value& value, int64_t& out)
        {
            if (!value.IsNumber())
                return false;
            const double number = value.AsNumber(0.0);
            constexpr double kMaxExactJsonInteger = 9007199254740991.0;
            if (!std::isfinite(number) || number < -kMaxExactJsonInteger || number > kMaxExactJsonInteger ||
                std::trunc(number) != number)
                return false;
            out = static_cast<int64_t>(number);
            return true;
        }

        std::string FoldCase(const std::string& value)
        {
            std::string folded = value;
            std::transform(folded.begin(), folded.end(), folded.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return folded;
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // Disk round-trip (Spark::Json; additive keys, tolerant loads)
    // ---------------------------------------------------------------------------

    TFOutfitStore::LoadResult TFOutfitStore::LoadFromDisk()
    {
        std::string text;
        if (!ReadAllText(m_path, text))
            return LoadResult::Unreadable;

        std::string lexicalError;
        if (!JsonStrict::ValidateLexemes(text, {}, lexicalError))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] outfit store %s rejected by lexical validation: %s",
                            SavePaths::Utf8ForLog(m_path).c_str(), lexicalError.c_str());
            return LoadResult::Corrupt;
        }

        // Strict parse (W9): the lenient Parse accepts truncated/torn files as a
        // partial object, which silently loaded an empty store and wiped the file
        // on the next save. A strict failure here makes Open()'s quarantine path
        // actually trigger.
        Spark::Json::Value root;
        std::string parseError;
        if (!Spark::Json::ParseStrict(text, &root, &parseError))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] outfit store %s rejected by strict JSON parse: %s",
                            SavePaths::Utf8ForLog(m_path).c_str(), parseError.c_str());
            return LoadResult::Corrupt;
        }
        if (!root.IsObject())
            return LoadResult::Corrupt;
        if (!root["outfits"].IsArray())
            return LoadResult::Corrupt;

        const bool hasNextOutfitId = root.HasKey("nextOutfitId");
        uint32_t declaredNextOutfitId = 1;
        if (hasNextOutfitId &&
            (!ReadUnsigned(root["nextOutfitId"], declaredNextOutfitId) || declaredNextOutfitId == 0 ||
             declaredNextOutfitId == std::numeric_limits<uint32_t>::max()))
            return LoadResult::Corrupt;

        uint32_t maxOutfitId = 0;
        std::vector<TFOutfitRecord> validatedOutfits;
        if (root.HasKey("outfits") && root["outfits"].IsArray())
        {
            const auto& arr = root["outfits"];
            validatedOutfits.reserve(arr.Size());
            std::unordered_set<uint32_t> outfitIds;
            std::unordered_set<uint64_t> memberIds;
            std::unordered_set<std::string> outfitNames;
            std::unordered_set<std::string> outfitTags;
            for (size_t i = 0; i < arr.Size(); ++i)
            {
                const auto& row = arr[i];
                TFOutfitRecord rec;
                if (!row.IsObject() || !ReadUnsigned(row["id"], rec.id) || rec.id == 0 ||
                    rec.id == std::numeric_limits<uint32_t>::max() || !outfitIds.insert(rec.id).second ||
                    !row["name"].IsString() || !row["tag"].IsString() ||
                    !ReadInt64(row["createdAtMs"], rec.createdAtMs) || !row["members"].IsArray() ||
                    !ReadOptionalUnsigned(row, "weeklyScore", rec.weeklyScore) ||
                    !ReadOptionalUnsigned(row, "allTimeScore", rec.allTimeScore) ||
                    !ReadOptionalUnsigned(row, "weekKey", rec.weekKey))
                    return LoadResult::Corrupt;
                rec.name = row["name"].AsString();
                rec.tag = row["tag"].AsString();
                if (rec.name.size() < kOutfitNameMin || rec.name.size() > kOutfitNameMax ||
                    rec.tag.size() < kOutfitTagMin || rec.tag.size() > kOutfitTagMax ||
                    !outfitNames.insert(FoldCase(rec.name)).second || !outfitTags.insert(FoldCase(rec.tag)).second ||
                    rec.weeklyScore > rec.allTimeScore)
                    return LoadResult::Corrupt;

                if (row.HasKey("members") && row["members"].IsArray())
                {
                    const auto& members = row["members"];
                    if (members.Size() == 0 || members.Size() > kMaxOutfitMembers)
                        return LoadResult::Corrupt;
                    size_t leaderCount = 0;
                    for (size_t mi = 0; mi < members.Size(); ++mi)
                    {
                        const auto& mrow = members[mi];
                        TFOutfitMemberRecord m;
                        uint8_t rank = 0;
                        if (!mrow.IsObject() || !ReadUnsigned(mrow["charId"], m.charId) || m.charId == 0 ||
                            !memberIds.insert(m.charId).second || !mrow["name"].IsString() ||
                            !ReadUnsigned(mrow["rank"], rank) || rank > static_cast<uint8_t>(TFOutfitRank::Leader) ||
                            !ReadInt64(mrow["joinedAtMs"], m.joinedAtMs))
                            return LoadResult::Corrupt;
                        m.name = mrow["name"].AsString();
                        m.rank = static_cast<TFOutfitRank>(rank);
                        if (m.name.empty())
                            return LoadResult::Corrupt;
                        if (m.rank == TFOutfitRank::Leader)
                            ++leaderCount;
                        rec.members.push_back(std::move(m));
                    }
                    if (leaderCount != 1)
                        return LoadResult::Corrupt;
                }

                maxOutfitId = std::max(maxOutfitId, rec.id);
                validatedOutfits.push_back(std::move(rec));
            }
        }

        // Absence is the one supported legacy form. An explicitly stale
        // allocator is corruption, not a migration hint: silently repairing
        // it would normalize a torn/hand-edited document on the next save.
        if (hasNextOutfitId && declaredNextOutfitId <= maxOutfitId)
            return LoadResult::Corrupt;
        m_outfits = std::move(validatedOutfits);
        m_nextOutfitId = hasNextOutfitId ? declaredNextOutfitId : maxOutfitId + 1;

        return LoadResult::Loaded;
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
        auto parentPath = m_path.parent_path();
        if (!parentPath.empty())
        {
            fs::create_directories(parentPath, ec);
            if (ec)
                return false;
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

} // namespace Terrafront
