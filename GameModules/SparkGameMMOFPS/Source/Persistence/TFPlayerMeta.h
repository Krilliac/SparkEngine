/**
 * @file TFPlayerMeta.h
 * @brief Runtime per-player progression meta (unlocks, loadout, per-weapon
 *        aggregate stats) + its TFDatabase round-trip.
 *
 * OWNERSHIP: progression lane. TFProgressionSystem owns one TFPlayerMetaStore
 * by value and is the ONLY mutator; other systems query through the
 * TFProgressionSystem API (IsUnlocked / GetStats / GetLoadout), never through
 * this store directly.
 *
 * The store is deliberately policy-free: rank/flux/prereq gating lives in
 * TFProgressionSystem (which owns the rank curve and the flux wallet). This
 * class only holds the per-PlayerId runtime records and knows how to seed
 * them from / persist them to the durable TFCharacterRecord.
 *
 * Keying: live runtime records are keyed by the session PlayerId; a failed
 * disconnect write is detached into a retry map keyed by durable charId so a
 * recycled PlayerId cannot inherit another account's metadata. Stats and
 * loadout entries are keyed by the durable weapons.json weapon KEY (not
 * WeaponId, which is a load-order index) so saved data survives table
 * reordering. Records never seeded (bots, pre-onboarding/standalone sessions)
 * have charId 0 and stay session-only.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Persistence/TFDatabase.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace Terrafront
{

    /// Per-weapon lifetime aggregates. "headshots" counts headshot KILLS (from
    /// EvPlayerKilled); "hits" counts confirmed damage-dealing shots reported by
    /// the weapon server via TFProgressionSystem::ServerRecordHits.
    struct TFWeaponAggStats
    {
        uint32_t kills = 0;
        uint32_t shots = 0;
        uint32_t hits = 0;
        uint32_t headshots = 0;
    };

    /// Persisted weapon selection (weapons.json keys). An empty slot string means
    /// "use the class default" — TFWeaponSystem's per-class slot rules remain the
    /// authoritative per-spawn gate; this is only the player's saved preference.
    ///
    /// loadout-depth wave: `grenade` (frag_grenade/smoke_grenade/flash_grenade,
    /// weapons.json keys, empty == frag default) and `suit` (Assets/MMOFPS/Data/
    /// suits.json key, empty == no passive) are additive fields, set/validated
    /// ONLY through TFProgressionSystem::ServerSetLoadoutExt (kept separate from
    /// ServerSetLoadout's frozen primary/secondary/tool triad so the existing
    /// TF_LoadoutChange wire message, which never populates these two fields,
    /// cannot silently wipe them). grenade IS consumed at spawn (TFGrenadeSystem::
    /// OnPlayerSpawned resolves the thrower's kind from this field); suit IS
    /// consumed at spawn (TFPlayerSystem::CreatePawnEntity applies its scalar via
    /// the TFProgressionSystem::Suit*Mult queries). The pre-existing primary/
    /// secondary/tool fields remain validated-and-persisted only — nothing reads
    /// them back into the spawn equip path yet (pre-existing W6 gap, not touched
    /// by this wave).
    struct TFLoadout
    {
        std::string primary;
        std::string secondary;
        std::string tool;
        std::string grenade; ///< weapons.json key; empty == frag_grenade default
        std::string suit;    ///< suits.json key; empty == no passive

        bool Any() const
        {
            return !primary.empty() || !secondary.empty() || !tool.empty() || !grenade.empty() || !suit.empty();
        }
    };

    class TFPlayerMetaStore
    {
      public:
        struct Meta
        {
            uint64_t charId = 0;                     ///< 0 == session-only (never persisted)
            std::unordered_set<std::string> unlocks; ///< purchased/granted TFUnlockDef keys
            TFLoadout loadout;
            std::unordered_map<std::string, TFWeaponAggStats> stats; ///< by weapons.json key
            bool dirty = false;
        };

        Meta& Ensure(PlayerId player) { return m_meta[player]; }
        Meta* Find(PlayerId player);
        const Meta* Find(PlayerId player) const;
        void Erase(PlayerId player) { m_meta.erase(player); }
        void Clear()
        {
            m_meta.clear();
            m_pendingByCharacter.clear();
        }
        bool IsDirty(PlayerId player) const;
        bool AnyDirty() const;
        size_t Count() const { return m_meta.size(); }

        /// Remove a disconnected PlayerId without losing a failed durable
        /// write or exposing its metadata if that transient id is reused.
        bool Detach(PlayerId player, TFDatabase* db);

        /// Overwrite (not merge) this player's runtime meta from the durable
        /// character record — same replace semantics and call site as
        /// TFProgressionSystem::ServerLoadCharacter's xp/rank/flux seeding.
        void SeedFromRecord(PlayerId player, const TFCharacterRecord& rec);

        /// Persist one player's meta through db.SaveCharacterMeta if it is dirty
        /// and bound to a character. Returns true if a write happened.
        bool PersistIfDirty(PlayerId player, TFDatabase& db);

        /// Persist every dirty character-bound record (SaveNow / Shutdown sweep).
        bool PersistAllDirty(TFDatabase& db);

        /// Debug UI iteration only.
        const std::unordered_map<PlayerId, Meta>& AllMeta() const { return m_meta; }

      private:
        static bool PersistOne(Meta& meta, TFDatabase& db);

        std::unordered_map<PlayerId, Meta> m_meta;
        std::unordered_map<uint64_t, Meta> m_pendingByCharacter;
    };

} // namespace Terrafront
