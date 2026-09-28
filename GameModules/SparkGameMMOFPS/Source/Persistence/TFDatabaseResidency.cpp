/**
 * @file TFDatabaseResidency.cpp
 * @brief TF-120 character residency fence: BindAuthority / ClaimCharacter / ReleaseCharacter.
 *
 * Contract in TFDatabase.h. Every change runs through TFDatabase::Transact, so it
 * is one atomic commit under the per-call file lock, serialized against every
 * other authority's claims and writes on the same save root.
 */
#include "Persistence/TFDatabase.h"
#include "Persistence/TFSavePaths.h"

#include "Utils/LogMacros.h"

#include <algorithm>
#include <string>
#include <system_error>

namespace Terrafront
{

    std::filesystem::path TFDatabase::AuthorityLockTarget(std::string_view continentKey) const
    {
        std::filesystem::path target = m_path;
        target += ".authority.";
        target += std::string(continentKey);
        return target;
    }

    bool TFDatabase::BindAuthority(std::string_view continentKey)
    {
        if (!m_open || !m_boundContinent.empty() || !SavePaths::IsValidContinentKey(continentKey))
            return false;

        std::error_code lockEc;
        if (!m_authorityLock.Lock(AuthorityLockTarget(continentKey), kLockTimeout, lockEc))
        {
            m_status = TFDatabaseStatus::AuthorityHeld;
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] db %s: continent '%.*s' already has a live authority (%s); bind refused",
                            SavePaths::Utf8ForLog(m_path).c_str(), static_cast<int>(continentKey.size()),
                            continentKey.data(), lockEc.message().c_str());
            return false;
        }
        m_boundContinent = std::string(continentKey);

        // The lock proves every earlier authority for this continent is dead, so whatever it left resident
        // here is stale. Clear it before this authority serves anyone (own-crash recovery).
        size_t cleared = 0;
        bool mutationRan = false;
        const bool committed = Transact("BindAuthority",
                                        [&](Snapshot& fresh, uint64_t newRevision)
                                        {
                                            mutationRan = true;
                                            for (TFCharacterRecord& row : fresh.characters)
                                            {
                                                if (row.residentContinent != m_boundContinent)
                                                    continue;
                                                row.residentContinent.clear();
                                                row.revision = newRevision;
                                                ++cleared;
                                            }
                                            return cleared != 0; // nothing to clear: no write
                                        });
        if (!mutationRan || (cleared != 0 && !committed))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] db %s: bind to continent '%s' failed while clearing stale residency",
                            SavePaths::Utf8ForLog(m_path).c_str(), m_boundContinent.c_str());
            m_boundContinent.clear();
            m_authorityLock.Unlock();
            return false;
        }
        if (cleared != 0)
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF] db %s: continent '%s' bound; cleared %zu character(s) a dead authority left in world",
                           SavePaths::Utf8ForLog(m_path).c_str(), m_boundContinent.c_str(), cleared);
        m_status = TFDatabaseStatus::ReadyExisting;
        return true;
    }

    bool TFDatabase::ClaimCharacter(uint64_t charId, TFCharacterRecord& out)
    {
        if (!m_open || m_boundContinent.empty())
            return false;

        TFCharacterRecord claimed;
        std::string heldBy;        // live continent that keeps the character
        std::string takenOverFrom; // dead continent the claim took it from
        bool alreadyHere = false;
        const bool committed =
            Transact("ClaimCharacter",
                     [&](Snapshot& fresh, uint64_t newRevision)
                     {
                         auto it = std::find_if(fresh.characters.begin(), fresh.characters.end(),
                                                [charId](const TFCharacterRecord& c) { return c.id == charId; });
                         if (it == fresh.characters.end())
                             return false;
                         if (it->residentContinent == m_boundContinent)
                         {
                             alreadyHere = true;
                             claimed = *it;
                             return false; // nothing to write; the baseline is adopted below
                         }
                         if (!it->residentContinent.empty())
                         {
                             // The holder is alive exactly while it holds its authority lock. This probe runs under the
                             // file lock, and every claim does too, so the holder cannot claim anything in between.
                             SavePaths::ExclusiveFileLock probe;
                             std::error_code probeEc;
                             if (!probe.TryLock(AuthorityLockTarget(it->residentContinent), probeEc))
                             {
                                 heldBy = it->residentContinent;
                                 return false;
                             }
                             probe.Unlock();
                             takenOverFrom = it->residentContinent;
                         }
                         it->residentContinent = m_boundContinent;
                         it->revision = newRevision;
                         claimed = *it;
                         return true;
                     });

        if (!heldBy.empty())
        {
            m_status = TFDatabaseStatus::ResidentElsewhere;
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF] claim of character %llu on '%s' refused: it is in world on live continent '%s'",
                           static_cast<unsigned long long>(charId), m_boundContinent.c_str(), heldBy.c_str());
            return false;
        }
        if (!committed && !alreadyHere)
            return false;
        if (!takenOverFrom.empty())
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF] character %llu taken over on '%s' from continent '%s', whose authority is dead",
                           static_cast<unsigned long long>(charId), m_boundContinent.c_str(), takenOverFrom.c_str());
        m_baseRevisions[charId] = claimed.revision;
        m_status = TFDatabaseStatus::ReadyExisting;
        out = claimed;
        return true;
    }

    bool TFDatabase::ReleaseCharacter(uint64_t charId)
    {
        if (!m_open || m_boundContinent.empty())
            return false;

        bool found = false;
        bool residentHere = false;
        const bool committed =
            Transact("ReleaseCharacter",
                     [&](Snapshot& fresh, uint64_t newRevision)
                     {
                         auto it = std::find_if(fresh.characters.begin(), fresh.characters.end(),
                                                [charId](const TFCharacterRecord& c) { return c.id == charId; });
                         if (it == fresh.characters.end())
                             return false;
                         found = true;
                         residentHere = it->residentContinent == m_boundContinent;
                         if (!residentHere)
                             return false; // already released or taken over: nothing to do
                         it->residentContinent.clear();
                         it->revision = newRevision;
                         return true;
                     });
        if (!found || (residentHere && !committed))
            return false;
        m_baseRevisions.erase(charId);
        m_status = TFDatabaseStatus::ReadyExisting;
        return true;
    }

} // namespace Terrafront
