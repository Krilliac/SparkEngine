/**
 * @file TFPlayerMeta.cpp
 * @brief TFPlayerMetaStore — runtime record management + TFDatabase round-trip.
 */
#include "Persistence/TFPlayerMeta.h"

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

    bool TFPlayerMetaStore::AnyDirty() const
    {
        for (const auto& entry : m_meta)
            if (entry.second.dirty && entry.second.charId != 0)
                return true;
        return false;
    }

    void TFPlayerMetaStore::SeedFromRecord(PlayerId player, const TFCharacterRecord& rec)
    {
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

    void TFPlayerMetaStore::PersistAllDirty(TFDatabase& db)
    {
        for (auto& entry : m_meta)
            PersistOne(entry.second, db);
    }

    bool TFPlayerMetaStore::PersistOne(Meta& meta, TFDatabase& db)
    {
        if (!meta.dirty || meta.charId == 0 || !db.IsOpen())
            return false;

        // Sorted copies so the on-disk JSON is deterministic across runs
        // (unordered containers would otherwise reshuffle every save).
        std::vector<std::string> unlocks(meta.unlocks.begin(), meta.unlocks.end());
        std::sort(unlocks.begin(), unlocks.end());

        std::vector<TFWeaponStatsRow> stats;
        stats.reserve(meta.stats.size());
        for (const auto& [key, s] : meta.stats)
            stats.push_back(TFWeaponStatsRow{key, s.kills, s.shots, s.hits, s.headshots});
        std::sort(stats.begin(), stats.end(),
                  [](const TFWeaponStatsRow& a, const TFWeaponStatsRow& b) { return a.weaponKey < b.weaponKey; });

        db.SaveCharacterMeta(meta.charId, unlocks, meta.loadout.primary, meta.loadout.secondary, meta.loadout.tool,
                             meta.loadout.grenade, meta.loadout.suit, stats);
        meta.dirty = false;
        return true;
    }

} // namespace Terrafront
