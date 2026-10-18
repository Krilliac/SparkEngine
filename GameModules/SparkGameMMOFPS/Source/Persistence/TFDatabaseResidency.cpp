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
        {
            return false;
        }

        std::error_code lockEc;
        if (!m_authorityLock.Lock(AuthorityLockTarget(continentKey), kLockTimeout, lockEc))
        {
            // Only contention proves a live authority. Any other failure (permissions, a directory where the
            // lock file belongs) is an unusable save root and is reported with its real error.
            const bool contended = SavePaths::ExclusiveFileLock::IsContention(lockEc);
            m_status = contended ? TFDatabaseStatus::AuthorityHeld : TFDatabaseStatus::Unreadable;
            if (contended)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game,
                                "[TF] db %s: continent '%.*s' already has a live authority (%s); bind refused",
                                SavePaths::Utf8ForLog(m_path).c_str(), static_cast<int>(continentKey.size()),
                                continentKey.data(), lockEc.message().c_str());
            }
            else
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game,
                                "[TF] db %s: the authority lock for continent '%.*s' cannot be opened (%s); bind "
                                "refused",
                                SavePaths::Utf8ForLog(m_path).c_str(), static_cast<int>(continentKey.size()),
                                continentKey.data(), lockEc.message().c_str());
            }
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
                                                {
                                                    continue;
                                                }
                                                row.residentContinent.clear();
                                                // An in-flight reservation the dead source left behind becomes a
                                                // terminal rollback. A terminal committed or rolled_back record
                                                // is kept whole: it fences replays of that operation, and a
                                                // terminal row without its source and destination fails load
                                                // validation.
                                                if (!row.migrationOperation.empty())
                                                {
                                                    row.migrationLastOperation = row.migrationOperation;
                                                    row.migrationOperation.clear();
                                                    row.migrationState = "rolled_back";
                                                }
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
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF] db %s: continent '%s' bound; cleared %zu character(s) a dead authority left in world",
                           SavePaths::Utf8ForLog(m_path).c_str(), m_boundContinent.c_str(), cleared);
        }
        m_status = TFDatabaseStatus::ReadyExisting;
        return true;
    }

    bool TFDatabase::IsHeldByLiveAuthority(std::string_view continentKey) const
    {
        // The holder is alive exactly while it holds its authority lock. Callers run this under the file lock,
        // and every claim does too, so the holder cannot claim anything in between. The bound continent's own
        // lock is held by this instance, so it always reads as live.
        SavePaths::ExclusiveFileLock probe;
        std::error_code probeEc;
        return !probe.TryLock(AuthorityLockTarget(continentKey), probeEc);
    }

    bool TFDatabase::ClaimCharacter(uint64_t charId, TFCharacterRecord& out, uint64_t expectedAccountId)
    {
        if (!m_open || m_boundContinent.empty())
        {
            return false;
        }

        TFCharacterRecord claimed;
        std::string heldBy;        // live continent that keeps the character
        std::string takenOverFrom; // dead continent the claim took it from
        bool alreadyHere = false;
        const bool committed = Transact(
            "ClaimCharacter",
            [&](Snapshot& fresh, uint64_t newRevision)
            {
                auto it = std::find_if(fresh.characters.begin(), fresh.characters.end(),
                                       [charId](const TFCharacterRecord& c) { return c.id == charId; });
                // Ownership is checked inside the transaction, so a claim can never commit for a row
                // that is not the caller's (there is no window between a check and the claim).
                if (it == fresh.characters.end() || (expectedAccountId != 0 && it->accountId != expectedAccountId))
                {
                    return false;
                }
                if (it->residentContinent == m_boundContinent)
                {
                    if (!it->migrationOperation.empty())
                    {
                        heldBy = it->migrationDestination;
                        return false;
                    }
                    alreadyHere = true;
                    claimed = *it;
                    return false; // nothing to write; the baseline is adopted below
                }
                if (!it->residentContinent.empty())
                {
                    if (!it->migrationOperation.empty())
                    {
                        if (IsHeldByLiveAuthority(it->migrationSource))
                        {
                            heldBy = it->migrationSource;
                            return false;
                        }
                        it->migrationLastOperation = it->migrationOperation;
                        it->migrationState = "rolled_back";
                        it->migrationOperation.clear();
                    }
                    if (IsHeldByLiveAuthority(it->residentContinent))
                    {
                        heldBy = it->residentContinent;
                        return false;
                    }
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
        {
            return false;
        }
        if (!takenOverFrom.empty())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF] character %llu taken over on '%s' from continent '%s', whose authority is dead",
                           static_cast<unsigned long long>(charId), m_boundContinent.c_str(), takenOverFrom.c_str());
        }
        m_baseRevisions[charId] = claimed.revision;
        m_status = TFDatabaseStatus::ReadyExisting;
        out = claimed;
        return true;
    }

    bool TFDatabase::ReleaseCharacter(uint64_t charId)
    {
        if (!m_open || m_boundContinent.empty())
        {
            return false;
        }

        bool found = false;
        bool residentHere = false;
        bool migrationActive = false;
        const bool committed =
            Transact("ReleaseCharacter",
                     [&](Snapshot& fresh, uint64_t newRevision)
                     {
                         auto it = std::find_if(fresh.characters.begin(), fresh.characters.end(),
                                                [charId](const TFCharacterRecord& c) { return c.id == charId; });
                         if (it == fresh.characters.end())
                         {
                             return false;
                         }
                         found = true;
                         residentHere = it->residentContinent == m_boundContinent;
                         migrationActive = residentHere && !it->migrationOperation.empty();
                         if (migrationActive)
                         {
                             return false;
                         }
                         if (!residentHere)
                         {
                             return false; // already released or taken over: nothing to do
                         }
                         it->residentContinent.clear();
                         it->revision = newRevision;
                         return true;
                     });
        if (!found || migrationActive || (residentHere && !committed))
        {
            if (migrationActive)
            {
                m_status = TFDatabaseStatus::Conflict;
            }
            return false;
        }
        m_baseRevisions.erase(charId);
        m_status = TFDatabaseStatus::ReadyExisting;
        return true;
    }

    bool TFDatabase::ReserveMigration(uint64_t charId, std::string_view operationId, uint64_t operationEpoch,
                                      std::string_view destinationContinent, std::string_view payload,
                                      TFCharacterRecord& out)
    {
        if (!m_open || m_boundContinent.empty() || operationId.empty() || operationId.size() > 128 ||
            operationEpoch == 0 || operationEpoch >= 9007199254740991ULL || payload.size() > 65536 ||
            !SavePaths::IsValidContinentKey(destinationContinent) || destinationContinent == m_boundContinent)
        {
            return false;
        }

        TFCharacterRecord reserved;
        bool alreadyReserved = false;
        bool refused = false;
        const std::string operation(operationId);
        const std::string destination(destinationContinent);
        const std::string state(payload);
        const bool committed = Transact(
            "ReserveMigration",
            [&](Snapshot& fresh, uint64_t newRevision)
            {
                auto it = std::find_if(fresh.characters.begin(), fresh.characters.end(),
                                       [charId](const TFCharacterRecord& c) { return c.id == charId; });
                if (it == fresh.characters.end() || it->residentContinent != m_boundContinent ||
                    m_baseRevisions.find(charId) == m_baseRevisions.end() || m_baseRevisions[charId] != it->revision)
                {
                    refused = true;
                    return false;
                }
                if (!it->migrationOperation.empty())
                {
                    alreadyReserved = it->migrationOperation == operation && it->migrationDestination == destination &&
                                      it->migrationPayload == state && it->migrationState == "reserved" &&
                                      it->migrationEpoch == operationEpoch;
                    refused = !alreadyReserved;
                    reserved = *it;
                    return false;
                }
                if (!it->migrationState.empty() && operationEpoch <= it->migrationEpoch)
                {
                    refused = true;
                    return false;
                }
                it->migrationOperation = operation;
                it->migrationSource = m_boundContinent;
                it->migrationDestination = destination;
                it->migrationPayload = state;
                it->migrationEpoch = operationEpoch;
                it->migrationState = "reserved";
                it->revision = newRevision;
                reserved = *it;
                return true;
            });
        if (refused)
        {
            m_status = TFDatabaseStatus::Conflict;
            return false;
        }
        if (!committed && !alreadyReserved)
        {
            return false;
        }
        m_baseRevisions[charId] = reserved.revision;
        out = reserved;
        m_status = TFDatabaseStatus::ReadyExisting;
        return true;
    }

    bool TFDatabase::CommitMigration(uint64_t charId, std::string_view operationId, TFCharacterRecord& out)
    {
        if (!m_open || m_boundContinent.empty() || operationId.empty() || operationId.size() > 128)
        {
            return false;
        }

        TFCharacterRecord completed;
        bool alreadyCommitted = false;
        bool refused = false;
        const std::string operation(operationId);
        const bool committed =
            Transact("CommitMigration",
                     [&](Snapshot& fresh, uint64_t newRevision)
                     {
                         auto it = std::find_if(fresh.characters.begin(), fresh.characters.end(),
                                                [charId](const TFCharacterRecord& c) { return c.id == charId; });
                         if (it == fresh.characters.end())
                         {
                             refused = true;
                             return false;
                         }
                         if (it->residentContinent == m_boundContinent && it->migrationLastOperation == operation &&
                             it->migrationState == "committed" && it->migrationOperation.empty())
                         {
                             alreadyCommitted = true;
                             completed = *it;
                             return false;
                         }
                         if (it->migrationOperation != operation || it->migrationDestination != m_boundContinent ||
                             it->migrationState != "reserved")
                         {
                             refused = true;
                             return false;
                         }
                         it->residentContinent = m_boundContinent;
                         it->migrationOperation.clear();
                         it->migrationLastOperation = operation;
                         it->migrationState = "committed";
                         it->revision = newRevision;
                         completed = *it;
                         return true;
                     });
        if (refused)
        {
            m_status = TFDatabaseStatus::Conflict;
            return false;
        }
        if (!committed && !alreadyCommitted)
        {
            return false;
        }
        m_baseRevisions[charId] = completed.revision;
        out = completed;
        m_status = TFDatabaseStatus::ReadyExisting;
        return true;
    }

    bool TFDatabase::AbortMigration(uint64_t charId, std::string_view operationId)
    {
        if (!m_open || m_boundContinent.empty() || operationId.empty() || operationId.size() > 128)
        {
            return false;
        }

        bool found = false;
        bool matching = false;
        bool alreadyAborted = false;
        uint64_t resultingRevision = 0;
        const std::string operation(operationId);
        const bool committed =
            Transact("AbortMigration",
                     [&](Snapshot& fresh, uint64_t newRevision)
                     {
                         auto it = std::find_if(fresh.characters.begin(), fresh.characters.end(),
                                                [charId](const TFCharacterRecord& c) { return c.id == charId; });
                         if (it == fresh.characters.end())
                         {
                             return false;
                         }
                         found = true;
                         if (it->migrationOperation.empty())
                         {
                             alreadyAborted =
                                 it->migrationLastOperation == operation && it->migrationState == "rolled_back";
                             return false;
                         }
                         if (it->migrationOperation != operation || it->migrationSource != m_boundContinent ||
                             it->migrationState != "reserved")
                         {
                             return false;
                         }
                         matching = true;
                         it->migrationOperation.clear();
                         it->migrationLastOperation = operation;
                         it->migrationState = "rolled_back";
                         it->revision = newRevision;
                         resultingRevision = newRevision;
                         return true;
                     });
        if (!matching && found && !alreadyAborted)
        {
            m_status = TFDatabaseStatus::Conflict;
            return false;
        }
        if (!found || (!matching && !alreadyAborted))
        {
            return false;
        }
        if (matching && !committed)
        {
            return false;
        }
        if (alreadyAborted)
        {
            const auto it = std::find_if(m_snapshot.characters.begin(), m_snapshot.characters.end(),
                                         [charId](const TFCharacterRecord& row) { return row.id == charId; });
            if (it == m_snapshot.characters.end() || it->residentContinent != m_boundContinent)
            {
                m_status = TFDatabaseStatus::Conflict;
                return false;
            }
        }
        if (matching)
        {
            m_baseRevisions[charId] = resultingRevision;
        }
        m_status = TFDatabaseStatus::ReadyExisting;
        return true;
    }

} // namespace Terrafront
