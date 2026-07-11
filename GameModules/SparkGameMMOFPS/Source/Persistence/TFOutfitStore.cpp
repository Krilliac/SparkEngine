/**
 * @file TFOutfitStore.cpp
 * @brief TFOutfitStore implementation — atomic-JSON-file backing (see header).
 *
 * Kept minimal-dependency (header-only Utils/JsonUtils.h + LogMacros) so it
 * links standalone into SparkTests the same way TFDatabase.cpp does.
 */
#include "Persistence/TFOutfitStore.h"

#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Terrafront
{

    namespace
    {

        constexpr float kOutfitSaveDebounceSec = 2.0f; // TFProgressionSystem save spirit

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

        bool EqualsNoCase(const std::string& a, const std::string& b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            }
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
    // TFOutfitRecord helpers
    // ---------------------------------------------------------------------------

    const TFOutfitMemberRecord* TFOutfitRecord::FindMember(uint64_t charId) const
    {
        for (const TFOutfitMemberRecord& m : members)
            if (m.charId == charId)
                return &m;
        return nullptr;
    }

    TFOutfitMemberRecord* TFOutfitRecord::FindMember(uint64_t charId)
    {
        for (TFOutfitMemberRecord& m : members)
            if (m.charId == charId)
                return &m;
        return nullptr;
    }

    const TFOutfitMemberRecord* TFOutfitRecord::Leader() const
    {
        for (const TFOutfitMemberRecord& m : members)
            if (m.rank == TFOutfitRank::Leader)
                return &m;
        return nullptr;
    }

    // ---------------------------------------------------------------------------
    // Open / Close / flush
    // ---------------------------------------------------------------------------

    TFOutfitStore::~TFOutfitStore()
    {
        if (m_open)
            Close();
    }

    bool TFOutfitStore::Open(const std::string& path)
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

        m_outfits.clear();
        m_nextOutfitId = 1;
        m_dirty = false;
        m_sinceDirty = 0.0f;

        std::error_code existsEc;
        const bool fileExists = fs::exists(m_path, existsEc);
        if (fileExists && !LoadFromDisk())
        {
            // Present but unreadable (corrupt/truncated/foreign). Never fall
            // through with an empty in-memory store: the next debounced flush
            // would silently overwrite the unreadable file with an empty one.
            // Quarantine + refuse instead (TFDatabase::Open pattern).
            std::error_code renameEc;
            const std::string backupPath = m_path + ".corrupt-" + std::to_string(NowMs()) + ".bak";
            fs::rename(m_path, backupPath, renameEc);
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] outfit store open refused: %s is unreadable/corrupt; backed up to %s (backup ok=%d)",
                            m_path.c_str(), backupPath.c_str(), renameEc ? 0 : 1);
            return false;
        }

        m_open = true;
        return true;
    }

    void TFOutfitStore::Close()
    {
        if (!m_open)
            return;
        if (m_dirty)
            SaveNow();
        m_open = false;
    }

    void TFOutfitStore::Tick(float dt)
    {
        if (!m_open || !m_dirty)
            return;
        m_sinceDirty += dt;
        if (m_sinceDirty >= kOutfitSaveDebounceSec)
            SaveNow();
    }

    bool TFOutfitStore::SaveNow()
    {
        if (!m_open && m_path.empty())
            return false;
        const bool ok = WriteToDisk();
        if (ok)
        {
            m_dirty = false;
            m_sinceDirty = 0.0f;
        }
        else
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] outfit store flush FAILED for %s (kept dirty)",
                            m_path.c_str());
        }
        return ok;
    }

    void TFOutfitStore::MarkDirty()
    {
        m_dirty = true;
        m_sinceDirty = 0.0f;
    }

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

    // ---------------------------------------------------------------------------
    // Queries
    // ---------------------------------------------------------------------------

    const TFOutfitRecord* TFOutfitStore::FindById(uint32_t id) const
    {
        for (const TFOutfitRecord& o : m_outfits)
            if (o.id == id)
                return &o;
        return nullptr;
    }

    const TFOutfitRecord* TFOutfitStore::FindByCharacter(uint64_t charId) const
    {
        if (charId == 0)
            return nullptr;
        for (const TFOutfitRecord& o : m_outfits)
            if (o.FindMember(charId))
                return &o;
        return nullptr;
    }

    const TFOutfitRecord* TFOutfitStore::FindByName(const std::string& name) const
    {
        for (const TFOutfitRecord& o : m_outfits)
            if (EqualsNoCase(o.name, name))
                return &o;
        return nullptr;
    }

    const TFOutfitRecord* TFOutfitStore::FindByTag(const std::string& tag) const
    {
        for (const TFOutfitRecord& o : m_outfits)
            if (EqualsNoCase(o.tag, tag))
                return &o;
        return nullptr;
    }

    // ---------------------------------------------------------------------------
    // Mutators
    // ---------------------------------------------------------------------------

    const TFOutfitRecord* TFOutfitStore::Create(const std::string& name, const std::string& tag, uint64_t leaderCharId,
                                                const std::string& leaderName, int64_t nowMs)
    {
        if (!m_open || leaderCharId == 0)
            return nullptr;
        if (FindByName(name) || FindByTag(tag))
            return nullptr;

        TFOutfitRecord rec;
        rec.id = m_nextOutfitId++;
        rec.name = name;
        rec.tag = tag;
        rec.createdAtMs = nowMs;

        TFOutfitMemberRecord leader;
        leader.charId = leaderCharId;
        leader.name = leaderName;
        leader.rank = TFOutfitRank::Leader;
        leader.joinedAtMs = nowMs;
        rec.members.push_back(std::move(leader));

        m_outfits.push_back(std::move(rec));
        MarkDirty();
        return &m_outfits.back();
    }

    bool TFOutfitStore::AddMember(uint32_t outfitId, uint64_t charId, const std::string& name, TFOutfitRank rank,
                                  int64_t nowMs)
    {
        if (!m_open || charId == 0)
            return false;
        TFOutfitRecord* rec = const_cast<TFOutfitRecord*>(FindById(outfitId));
        if (!rec || rec->FindMember(charId))
            return false;

        TFOutfitMemberRecord m;
        m.charId = charId;
        m.name = name;
        m.rank = rank;
        m.joinedAtMs = nowMs;
        rec->members.push_back(std::move(m));
        MarkDirty();
        return true;
    }

    bool TFOutfitStore::RemoveMember(uint32_t outfitId, uint64_t charId)
    {
        if (!m_open)
            return false;
        TFOutfitRecord* rec = const_cast<TFOutfitRecord*>(FindById(outfitId));
        if (!rec)
            return false;
        auto it = std::find_if(rec->members.begin(), rec->members.end(),
                               [charId](const TFOutfitMemberRecord& m) { return m.charId == charId; });
        if (it == rec->members.end())
            return false;
        rec->members.erase(it);
        MarkDirty();
        return true;
    }

    bool TFOutfitStore::SetMemberRank(uint32_t outfitId, uint64_t charId, TFOutfitRank rank)
    {
        if (!m_open)
            return false;
        TFOutfitRecord* rec = const_cast<TFOutfitRecord*>(FindById(outfitId));
        if (!rec)
            return false;
        TFOutfitMemberRecord* m = rec->FindMember(charId);
        if (!m)
            return false;
        if (m->rank == rank)
            return true; // idempotent, no dirty churn
        m->rank = rank;
        MarkDirty();
        return true;
    }

    bool TFOutfitStore::Disband(uint32_t outfitId)
    {
        if (!m_open)
            return false;
        auto it = std::find_if(m_outfits.begin(), m_outfits.end(),
                               [outfitId](const TFOutfitRecord& o) { return o.id == outfitId; });
        if (it == m_outfits.end())
            return false;
        m_outfits.erase(it);
        MarkDirty();
        return true;
    }

    void TFOutfitStore::UpdateMemberName(uint64_t charId, const std::string& name)
    {
        if (!m_open || charId == 0 || name.empty())
            return;
        for (TFOutfitRecord& o : m_outfits)
        {
            if (TFOutfitMemberRecord* m = o.FindMember(charId))
            {
                if (m->name != name)
                {
                    m->name = name;
                    MarkDirty();
                }
                return; // membership is exclusive: one outfit per character
            }
        }
    }

} // namespace Terrafront
