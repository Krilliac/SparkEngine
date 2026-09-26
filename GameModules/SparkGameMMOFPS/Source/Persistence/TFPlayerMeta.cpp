/**
 * @file TFPlayerMeta.cpp
 * @brief TFPlayerMetaStore — runtime record management + TFDatabase round-trip.
 */
#include "Persistence/TFPlayerMeta.h"

#include "Utils/LogMacros.h"

#include <algorithm>
#include <vector>

namespace Terrafront
{

    TFPlayerMetaStore::Meta* TFPlayerMetaStore::Find(PlayerId player)
    {
        auto it = m_meta.find(player);
        return it != m_meta.end() ? &it->second : nullptr;
    }

    const TFPlayerMetaStore::Meta* TFPlayerMetaStore::Find(PlayerId player) const
    {
        auto it = m_meta.find(player);
        return it != m_meta.end() ? &it->second : nullptr;
    }

    bool TFPlayerMetaStore::IsDirty(PlayerId player) const
    {
        const Meta* meta = Find(player);
        return meta && meta->dirty && meta->charId != 0;
    }

    bool TFPlayerMetaStore::AnyDirty() const
    {
        for (const auto& entry : m_meta)
            if (entry.second.dirty && entry.second.charId != 0)
                return true;
        for (const auto& entry : m_pendingByCharacter)
            if (entry.second.dirty && entry.second.charId != 0)
                return true;
        return false;
    }

    bool TFPlayerMetaStore::Detach(PlayerId player, TFDatabase* db)
    {
        auto it = m_meta.find(player);
        if (it == m_meta.end())
            return true;

        bool persisted = true;
        Meta& meta = it->second;
        if (meta.dirty && meta.charId != 0 && (!db || !PersistOne(meta, *db)))
        {
            meta.parkedBaseRevision = db ? db->BaselineRevision(meta.charId) : std::nullopt;
            m_pendingByCharacter[meta.charId] = std::move(meta);
            persisted = false;
        }
        m_meta.erase(it);
        return persisted;
    }

    void TFPlayerMetaStore::SeedFromRecord(PlayerId player, const TFCharacterRecord& rec)
    {
        if (auto pending = m_pendingByCharacter.find(rec.id); pending != m_pendingByCharacter.end())
        {
            // TF-120: the parked values are only still valid if nobody
            // committed the character since they were computed; otherwise
            // re-adopting them would overwrite another authority's newer row.
            // A row parked without a database has no baseline to check.
            const std::optional<uint64_t>& parkedBase = pending->second.parkedBaseRevision;
            if (!parkedBase || *parkedBase == rec.revision)
            {
                Meta& adopted = m_meta[player];
                adopted = std::move(pending->second);
                adopted.parkedBaseRevision.reset();
                m_pendingByCharacter.erase(pending);
                return;
            }
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] discarded unsaved meta for character %llu on re-entry: another authority changed "
                            "the character after this process's last successful save",
                            static_cast<unsigned long long>(rec.id));
            m_pendingByCharacter.erase(pending);
        }

        Meta& meta = m_meta[player];
        meta.charId = rec.id;

        meta.unlocks.clear();
        for (const std::string& key : rec.unlocks)
            if (!key.empty())
                meta.unlocks.insert(key);

        meta.loadout.primary = rec.loadoutPrimary;
        meta.loadout.secondary = rec.loadoutSecondary;
        meta.loadout.tool = rec.loadoutTool;
        meta.loadout.grenade = rec.loadoutGrenade; // loadout-depth wave (additive)
        meta.loadout.suit = rec.loadoutSuit;

        meta.stats.clear();
        for (const TFWeaponStatsRow& row : rec.weaponStats)
        {
            if (row.weaponKey.empty())
                continue;
            TFWeaponAggStats& s = meta.stats[row.weaponKey];
            s.kills = row.kills;
            s.shots = row.shots;
            s.hits = row.hits;
            s.headshots = row.headshots;
        }

        meta.dirty = false;
    }

    bool TFPlayerMetaStore::PersistIfDirty(PlayerId player, TFDatabase& db)
    {
        auto it = m_meta.find(player);
        if (it == m_meta.end())
            return false;
        return PersistOne(it->second, db);
    }

    bool TFPlayerMetaStore::PersistAllDirty(TFDatabase& db, std::vector<TFCharacterUpdate> progressUpdates)
    {
        // A closed db fails FindCharacter below, so dirty rows report failure
        // while an empty sweep still succeeds.
        bool ok = true;
        std::vector<TFCharacterUpdate> batch;
        std::unordered_map<uint64_t, size_t> batchIndex; // charId -> slot in batch
        auto slotFor = [&](uint64_t charId) -> TFCharacterUpdate*
        {
            TFCharacterRecord existing;
            if (!db.FindCharacter(charId, existing))
                return nullptr;
            auto [it, inserted] = batchIndex.try_emplace(charId, batch.size());
            if (inserted)
            {
                batch.emplace_back();
                batch.back().charId = charId;
            }
            return &batch[it->second];
        };

        for (TFCharacterUpdate& progress : progressUpdates)
        {
            TFCharacterUpdate* slot = slotFor(progress.charId);
            if (!slot || slot->writeProgress)
            {
                ok = false;
                continue;
            }
            slot->writeProgress = true;
            slot->xp = progress.xp;
            slot->rank = progress.rank;
            slot->flux = progress.flux;
            slot->lastPlayedMs = progress.lastPlayedMs;
        }

        std::vector<Meta*> included;
        auto addMeta = [&](Meta& meta)
        {
            TFCharacterUpdate* slot = slotFor(meta.charId);
            if (!slot || slot->writeMeta)
            {
                ok = false;
                return;
            }
            AddMetaToUpdate(meta, *slot);
            included.push_back(&meta);
        };
        for (auto& entry : m_meta)
            if (entry.second.dirty && entry.second.charId != 0)
                addMeta(entry.second);
        for (auto& entry : m_pendingByCharacter)
            if (entry.second.dirty && entry.second.charId != 0)
                addMeta(entry.second);

        if (batch.empty())
            return ok;

        // TF-120: a row another continent authority changed since this
        // process's baseline fails the whole commit with Conflict. Drop that
        // row and commit the rest, so one stale character cannot block every
        // other player's save on this continent. Each retry removes one row,
        // so the loop ends after at most batch.size() commits.
        std::unordered_set<uint64_t> conflicted;
        while (!batch.empty() && !db.CommitCharacterUpdates(batch))
        {
            const uint64_t charId = db.ConflictedCharacter();
            if (db.LastStatus() != TFDatabaseStatus::Conflict || charId == 0 || !conflicted.insert(charId).second)
                return false;
            ok = false;
            std::erase_if(batch, [charId](const TFCharacterUpdate& update) { return update.charId == charId; });
        }

        for (Meta* meta : included)
            if (!conflicted.contains(meta->charId))
                meta->dirty = false;

        // A parked (disconnected) row that conflicts was computed from a
        // baseline another authority has since replaced: the character now
        // lives there, so writing these values would overwrite its newer
        // state. Discard it loudly. A conflicted in-world row stays dirty and
        // keeps reporting failure; it means two authorities hold the character.
        for (const uint64_t charId : conflicted)
        {
            if (m_pendingByCharacter.erase(charId) != 0)
                SPARK_LOG_ERROR(Spark::LogCategory::Game,
                                "[TF] discarded unsaved meta for disconnected character %llu: another authority "
                                "changed the character after this process's last successful save",
                                static_cast<unsigned long long>(charId));
            else
                SPARK_LOG_ERROR(Spark::LogCategory::Game,
                                "[TF] character %llu is in world here but was changed by another authority; its "
                                "progress and meta stay unsaved",
                                static_cast<unsigned long long>(charId));
        }
        std::erase_if(m_pendingByCharacter, [](const auto& entry) { return !entry.second.dirty; });
        return ok;
    }

    void TFPlayerMetaStore::AddMetaToUpdate(const Meta& meta, TFCharacterUpdate& update)
    {
        // Sorted copies so the on-disk JSON is deterministic across runs
        // (unordered containers would otherwise reshuffle every save).
        update.writeMeta = true;
        update.unlocks.assign(meta.unlocks.begin(), meta.unlocks.end());
        std::sort(update.unlocks.begin(), update.unlocks.end());

        update.loadoutPrimary = meta.loadout.primary;
        update.loadoutSecondary = meta.loadout.secondary;
        update.loadoutTool = meta.loadout.tool;
        update.loadoutGrenade = meta.loadout.grenade;
        update.loadoutSuit = meta.loadout.suit;

        update.weaponStats.clear();
        update.weaponStats.reserve(meta.stats.size());
        for (const auto& [key, s] : meta.stats)
            update.weaponStats.push_back(TFWeaponStatsRow{key, s.kills, s.shots, s.hits, s.headshots});
        std::sort(update.weaponStats.begin(), update.weaponStats.end(),
                  [](const TFWeaponStatsRow& a, const TFWeaponStatsRow& b) { return a.weaponKey < b.weaponKey; });
    }

    bool TFPlayerMetaStore::PersistOne(Meta& meta, TFDatabase& db)
    {
        if (!meta.dirty || meta.charId == 0 || !db.IsOpen())
            return false;

        TFCharacterUpdate update;
        update.charId = meta.charId;
        AddMetaToUpdate(meta, update);
        if (!db.CommitCharacterUpdates({update}))
            return false;
        meta.dirty = false;
        return true;
    }

} // namespace Terrafront
