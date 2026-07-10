/**
 * @file TFProgressionSystem.h
 * @brief XP events, ranks 1-30, flux income, persistence.
 *
 * OWNERSHIP: this header + TFProgressionSystem.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 *
 * W2 implementation (server-authoritative, DESIGN.md §4):
 *  - ServerAwardXP is the single XP entry point: accumulates XP, recomputes
 *    the rank from a precomputed 30-rank curve (rank N needs ~500*N^1.6
 *    cumulative XP; rank 1 is the floor at 0), fires EvXPAwarded / EvRankUp
 *    on the bus and sends TF_XPEvent to the owning player.
 *  - Kill XP from EvPlayerKilled (kill 100, +25 headshot; no XP for team
 *    kills or suicides). Capture XP is awarded by TFRegionSystem calling
 *    ServerAwardXP directly. Assist tracking is W3.
 *  - Flux income on the continent fluxTickSec cadence: +1 base to every
 *    connected player with a faction, plus a region bonus of
 *    sum(fluxPerTick of held regions) / players-on-faction (min +1 when the
 *    faction holds any flux-producing region). Wallet capped at
 *    kFluxWalletCap. ServerSpendFlux gates W3 vehicle/exosuit purchases.
 *  - Persistence: per-player {xp, rank, flux} under the "progression" key of
 *    Saves/terrafront_state.json (shared with territory state; this system
 *    read-modify-writes only its own key). Written atomically (tmp+rename)
 *    on change (2 s debounce) and on shutdown; loaded on boot. NOTE:
 *    PlayerIds are session-scoped in W2, so persisted rows only re-attach
 *    within reconnects that reuse the same id (accounts are out of scope).
 *
 * W6 progression expansion (this lane):
 *  - Unlock tree (Persistence/TFUnlockTree.h): rank- and flux-gated weapon/
 *    vehicle unlocks; IsUnlocked / IsWeaponUnlocked / IsVehicleUnlocked
 *    queries + ServerTryUnlock purchases (spends the flux wallet).
 *  - Per-weapon aggregate stats (kills/shots/hits/headshot-kills): kills feed
 *    automatically from EvPlayerKilled; shots/hits via the ServerRecordShots/
 *    ServerRecordHits hooks for the weapon server.
 *  - Loadout save/load: ServerSetLoadout / GetLoadout persist a per-character
 *    primary/secondary/tool weapon-key selection.
 *  - All of it persists per CHARACTER through TFDatabase::SaveCharacterMeta
 *    (Persistence/TFPlayerMeta.h store; seeded in ServerLoadCharacter, flushed
 *    in SaveNow / ClearPlayer / Shutdown). Session-only players (charId 0,
 *    e.g. bots) keep runtime stats but are never persisted.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Persistence/TFPlayerMeta.h" // TFWeaponAggStats, TFLoadout, TFPlayerMetaStore

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Terrafront
{

    // Canonical XP reason codes (TF_XPEvent.reasonCode / ServerAwardXP reason).
    // The field is an opaque uint8_t on the wire; other systems may extend it.
    enum : uint8_t
    {
        kXPReasonKill = 0,
        kXPReasonAssist = 1,
        kXPReasonRevive = 2,
        kXPReasonRepairTick = 3,
        kXPReasonCaptureFacility = 4,
        kXPReasonCaptureFort = 5,
        kXPReasonCaptureOutpost = 6,
        kXPReasonDefend = 7,
        kXPReasonFluxTick = 8, ///< amount==0; carries a wallet refresh
        kXPReasonSync = 9,     ///< amount==0; totals refresh (spawn/spend)
        kXPReasonUnlock = 10,  ///< amount==0; unlock granted (wallet refresh; HUD toast cue)
        // 11 is claimed by kXPReasonDirective (Game/TFDirectiveData.h) — keep in sync.
    };

    /// Result of TFProgressionSystem::ServerTryUnlock (W6 progression expansion).
    enum class TFUnlockResult : uint8_t
    {
        Ok = 0,
        UnknownKey,
        AlreadyUnlocked,
        RankTooLow,
        PrereqLocked,
        InsufficientFlux,
        NotAuthority, ///< also: not initialized / invalid player
    };

    constexpr uint16_t kTFMaxRank = 30;

    class TFProgressionSystem
    {
      public:
        TFProgressionSystem();
        ~TFProgressionSystem();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();

        // --- W2 cross-agent contract (frozen this wave) ------------------------
        void ServerAwardXP(PlayerId player, uint16_t amount, uint8_t reasonCode);
        uint32_t FluxOf(PlayerId player) const;
        bool ServerSpendFlux(PlayerId player, uint32_t amount); ///< false if insufficient
        void ServerGrantFlux(PlayerId player, uint32_t amount); ///< debug/test grant (tf_giveflux)
        uint16_t RankOf(PlayerId player) const;

        // --- W2 additions beyond the contract (documented in wave report) ------

        /// Total XP of a player (0 if unknown). TFScoreboard's score column.
        uint32_t XPOf(PlayerId player) const;

        /// Cumulative XP threshold to hold `rank` (rank 1 -> 0). Clamped to 1..30.
        uint32_t XPForRank(uint16_t rank) const;

        /// Flush progression to Saves/terrafront_state.json now (tmp+rename).
        /// Public so the orchestrator can wire a tf_save console command.
        bool SaveNow();

        // --- W5 onboarding additions (final-review #1/#2: durable per-character
        // persistence, since runtime progression here is keyed by the ephemeral
        // PlayerId, not the durable character id) -------------------------------

        /// Seed/overwrite this player's runtime progression from the durable
        /// TFCharacterRecord resolved by TFServerSim::HandleEnterWorld, BEFORE
        /// any spawn/save can run for the session. Without this, a returning
        /// character's stored xp/rank/flux are never loaded, and the next
        /// SaveNow overwrites the durable record back to the runtime default
        /// (rank 1 / 0 xp / 0 flux) -- silent data loss for returning players.
        void ServerLoadCharacter(PlayerId player, uint32_t xp, uint16_t rank, uint32_t flux);

        /// Drop this player's runtime progression record (e.g. on disconnect,
        /// AFTER the final flush to the character has been persisted). Without
        /// this, a recycled PlayerId inherits the prior occupant's xp/flux and
        /// leaks them onto a different account's character.
        void ClearPlayer(PlayerId player);

        /// Debug panel toggle (hidden by default; wired from tf_* console commands).
        void ToggleDebugUI() { m_showDebug = !m_showDebug; }

        // --- W6 progression expansion: unlock tree / per-weapon stats / loadout.
        // Query APIs are safe from any system (HUD, directives, spawn UI, weapon
        // server); Server* mutators are authority-only, like the W2 contract above.
        // Persistence rides the existing per-character seams: seeded in
        // ServerLoadCharacter, flushed via TFDatabase::SaveCharacterMeta on the
        // SaveNow cadence / disconnect (ClearPlayer) / Shutdown. ----------------

        /// True if the player holds `unlockKey` (see Persistence/TFUnlockTree.h):
        /// purchased/granted, or auto-granted (fluxCost==0, rank + prereq met).
        /// Unknown keys are false.
        bool IsUnlocked(PlayerId player, std::string_view unlockKey) const;

        /// Weapon-level convenience: weapons with no unlock-tree entry are default
        /// kit (always true). Returns true while data tables are not loaded, and
        /// false for weapon ids the tables do not know.
        bool IsWeaponUnlocked(PlayerId player, WeaponId weapon) const;

        /// Vehicle-level convenience: vehicles with no tree entry (Drifter) are
        /// always available. This is the one-time ACCESS gate only — the per-spawn
        /// VehicleDef::fluxCost is still charged by the vehicle system.
        bool IsVehicleUnlocked(PlayerId player, VehicleId vehicle) const;

        /// Server: purchase/grant `unlockKey` for the player (rank + prereq gated;
        /// spends fluxCost from the wallet when > 0). Sends a kXPReasonUnlock
        /// TF_XPEvent on success so the owning client refreshes.
        TFUnlockResult ServerTryUnlock(PlayerId player, std::string_view unlockKey);

        /// Lifetime aggregates for one weapon (zeroes if unknown player/weapon).
        /// kills/headshots accrue automatically from EvPlayerKilled; shots/hits
        /// accrue when the weapon server calls the ServerRecord* hooks below.
        TFWeaponAggStats GetStats(PlayerId player, WeaponId weapon) const;
        TFWeaponAggStats GetStatsByKey(PlayerId player, std::string_view weaponKey) const;

        /// All per-weapon stats of a player, keyed by weapons.json key (nullptr if
        /// the player has none). For stats screens; do not hold across frames.
        const std::unordered_map<std::string, TFWeaponAggStats>* AllStats(PlayerId player) const;

        /// Server grant hooks for the weapon lane (validated-fire / confirmed-hit
        /// call sites in TFWeaponServer.cpp). Cheap; safe to call per shot.
        void ServerRecordShots(PlayerId player, WeaponId weapon, uint16_t count);
        void ServerRecordHits(PlayerId player, WeaponId weapon, uint16_t count);

        /// Persisted weapon preference (weapons.json keys; empty slot == class
        /// default). nullptr when the player never saved one — callers fall back
        /// to the class-default loadout exactly as today.
        const TFLoadout* GetLoadout(PlayerId player) const;

        /// Server: validate + store the player's loadout selection. Each non-empty
        /// key must exist in the data tables, sit in the right slot category
        /// (primary: rifle/carbine/lmg/sniper/shotgun/launcher; secondary: pistol;
        /// tool: tool), match the player's faction (or be common pool), and be
        /// unlocked. Returns false (nothing stored) on any violation. Per-class
        /// slot rules stay enforced per-spawn by TFWeaponSystem.
        bool ServerSetLoadout(PlayerId player, const TFLoadout& loadout);

      private:
        struct Prog
        {
            uint32_t xp = 0;
            uint32_t flux = 0;
            uint16_t rank = 1;
        };

        Prog& Ensure(PlayerId player);
        uint16_t RankForXP(uint32_t xp) const;
        void OnPlayerKilled(const EvPlayerKilled& ev);
        void OnPlayerSpawned(const EvPlayerSpawned& ev);
        void FluxIncomeTick();
        void SendXPEvent(PlayerId player, uint16_t amount, uint8_t reason);
        void SendToOwner(PlayerId owner, uint16_t msgId, const void* payload, size_t size);
        bool LoadFromDisk();

        // W6 progression expansion helpers
        bool IsUnlockedInternal(PlayerId player, std::string_view unlockKey, int depth) const;
        const std::string* WeaponKeyOf(WeaponId weapon) const; ///< nullptr if tables unloaded/unknown id
        bool ValidLoadoutSlotKey(PlayerId player, const std::string& weaponKey, int slot) const;

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};

        std::unordered_map<PlayerId, Prog> m_players;
        std::array<uint32_t, kTFMaxRank + 1> m_rankXP{}; ///< [rank] = cumulative XP
        TFPlayerMetaStore m_meta;                        ///< W6: unlocks / loadout / weapon stats

        float m_fluxAccum{0.0f}; ///< seconds toward the next flux income tick
        float m_sinceSave{0.0f}; ///< seconds since the last disk write
        bool m_dirty{false};
        uint32_t m_awards{0};
        uint32_t m_saves{0};
        bool m_showDebug{false};
    };

} // namespace Terrafront
