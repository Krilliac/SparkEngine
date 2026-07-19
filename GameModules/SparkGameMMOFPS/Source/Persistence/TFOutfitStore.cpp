/**
 * @file TFOutfitStore.cpp
 * @brief TFOutfitStore implementation — atomic-JSON-file backing (see header).
 *        Lifecycle, queries, mutators, and the ISO-week key; the disk
 *        round-trip lives in TFOutfitStoreDisk.cpp.
 *
 * Kept minimal-dependency (header-only LogMacros; JsonUtils only in the disk
 * TU) so it links standalone into SparkTests the same way TFDatabase.cpp does.
 */
#include "Persistence/TFOutfitStore.h"

#include "Utils/LogMacros.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>

namespace Terrafront
{

    namespace
    {

        constexpr float kOutfitSaveDebounceSec = 2.0f; // TFProgressionSystem save spirit

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

        // --- civil-calendar helpers (Howard Hinnant's algorithms; UTC) ----------

        /// days since 1970-01-01 -> (year, month, day)
        void CivilFromDays(int64_t days, int64_t& y, unsigned& m, unsigned& d)
        {
            const int64_t z = days + 719468;
            const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
            const uint64_t doe = static_cast<uint64_t>(z - era * 146097);
            const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
            const uint64_t mp = (5 * doy + 2) / 153;
            d = static_cast<unsigned>(doy - (153 * mp + 2) / 5 + 1);
            m = static_cast<unsigned>(mp < 10 ? mp + 3 : mp - 9);
            y = static_cast<int64_t>(yoe) + era * 400 + (m <= 2 ? 1 : 0);
        }

        /// (year, month, day) -> days since 1970-01-01
        int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d)
        {
            y -= m <= 2 ? 1 : 0;
            const int64_t era = (y >= 0 ? y : y - 399) / 400;
            const uint64_t yoe = static_cast<uint64_t>(y - era * 400);
            const uint64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
            const uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return era * 146097 + static_cast<int64_t>(doe) - 719468;
        }

    } // namespace

    uint32_t TFOutfitISOWeekKey(int64_t unixMs)
    {
        constexpr int64_t kMsPerDay = 86400000;
        int64_t days = unixMs / kMsPerDay;
        if (unixMs < 0 && (unixMs % kMsPerDay) != 0)
            --days; // floor toward -inf for pre-epoch times

        // ISO weekday 1=Mon..7=Sun (1970-01-01 was a Thursday). The %-of-negative
        // fold keeps pre-epoch days correct too.
        const int isoDow = static_cast<int>(((days % 7) + 10) % 7) + 1;

        // The ISO week/year of any date equal those of its week's Thursday.
        const int64_t thursday = days + (4 - isoDow);
        int64_t isoYear = 0;
        unsigned m = 0, d = 0;
        CivilFromDays(thursday, isoYear, m, d);
        const int64_t jan1 = DaysFromCivil(isoYear, 1, 1);
        const uint32_t week = static_cast<uint32_t>((thursday - jan1) / 7) + 1;
        return static_cast<uint32_t>(isoYear) * 100 + week;
    }

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

    bool TFOutfitStore::AddScore(uint32_t outfitId, uint32_t points, uint32_t weekKey)
    {
        if (!m_open || points == 0)
            return false;
        TFOutfitRecord* rec = const_cast<TFOutfitRecord*>(FindById(outfitId));
        if (!rec)
            return false;
        if (rec->weekKey != weekKey)
        {
            // Self-healing rollover: score landing in a new ISO week resets the
            // weekly counter even before the next RolloverWeek sweep runs.
            rec->weeklyScore = 0;
            rec->weekKey = weekKey;
        }
        rec->weeklyScore += points;
        rec->allTimeScore += points;
        MarkDirty();
        return true;
    }

    size_t TFOutfitStore::RolloverWeek(uint32_t weekKey)
    {
        if (!m_open)
            return 0;
        size_t stamped = 0;
        for (TFOutfitRecord& rec : m_outfits)
        {
            if (rec.weekKey == weekKey)
                continue;
            rec.weeklyScore = 0;
            rec.weekKey = weekKey;
            ++stamped;
        }
        if (stamped > 0)
            MarkDirty();
        return stamped;
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
