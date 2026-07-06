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
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include <array>
#include <cstddef>
#include <unordered_map>

namespace Terrafront {

// Canonical XP reason codes (TF_XPEvent.reasonCode / ServerAwardXP reason).
// The field is an opaque uint8_t on the wire; other systems may extend it.
enum : uint8_t {
    kXPReasonKill            = 0,
    kXPReasonAssist          = 1,
    kXPReasonRevive          = 2,
    kXPReasonRepairTick      = 3,
    kXPReasonCaptureFacility = 4,
    kXPReasonCaptureFort     = 5,
    kXPReasonCaptureOutpost  = 6,
    kXPReasonDefend          = 7,
    kXPReasonFluxTick        = 8,   ///< amount==0; carries a wallet refresh
    kXPReasonSync            = 9,   ///< amount==0; totals refresh (spawn/spend)
};

constexpr uint16_t kTFMaxRank = 30;

class TFProgressionSystem {
  public:
    TFProgressionSystem();
    ~TFProgressionSystem();

    bool Initialize(TFGameContext& ctx, TFEventBus& events);
    void Update(float deltaTime);
    void FixedUpdate(float fixedDeltaTime);
    void Shutdown();
    void RenderDebugUI();

    // --- W2 cross-agent contract (frozen this wave) ------------------------
    void     ServerAwardXP(PlayerId player, uint16_t amount, uint8_t reasonCode);
    uint32_t FluxOf(PlayerId player) const;
    bool     ServerSpendFlux(PlayerId player, uint32_t amount);   ///< false if insufficient
    void     ServerGrantFlux(PlayerId player, uint32_t amount);   ///< debug/test grant (tf_giveflux)
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

  private:
    struct Prog {
        uint32_t xp   = 0;
        uint32_t flux = 0;
        uint16_t rank = 1;
    };

    Prog&    Ensure(PlayerId player);
    uint16_t RankForXP(uint32_t xp) const;
    void     OnPlayerKilled(const EvPlayerKilled& ev);
    void     OnPlayerSpawned(const EvPlayerSpawned& ev);
    void     FluxIncomeTick();
    void     SendXPEvent(PlayerId player, uint16_t amount, uint8_t reason);
    void     SendToOwner(PlayerId owner, uint16_t msgId, const void* payload, size_t size);
    bool     LoadFromDisk();

    TFGameContext* m_ctx{nullptr};
    TFEventBus*    m_events{nullptr};
    bool           m_initialized{false};

    std::unordered_map<PlayerId, Prog>   m_players;
    std::array<uint32_t, kTFMaxRank + 1> m_rankXP{};   ///< [rank] = cumulative XP

    float    m_fluxAccum{0.0f};   ///< seconds toward the next flux income tick
    float    m_sinceSave{0.0f};   ///< seconds since the last disk write
    bool     m_dirty{false};
    uint32_t m_awards{0};
    uint32_t m_saves{0};
    bool     m_showDebug{false};
};

} // namespace Terrafront
