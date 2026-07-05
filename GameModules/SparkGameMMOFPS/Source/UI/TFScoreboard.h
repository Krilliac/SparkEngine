/**
 * @file TFScoreboard.h
 * @brief Per-faction K/D/score/regions tab screen.
 *
 * OWNERSHIP: this header + TFScoreboard.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 *
 * W2 implementation: hold TAB for a fullscreen translucent three-column
 * (one per faction) table of K / D / score per player, with a header showing
 * the continent name, per-faction region counts (TFRegionSystem::RegionsHeld)
 * and Dominion status, plus the local player's rank/XP/flux footer.
 *
 * Data sources:
 *  - Authority roles (standalone / listen host / dedicated): EvPlayerKilled
 *    on the bus tallies kills/deaths; score & rank come straight from
 *    TFProgressionSystem (XPOf/RankOf/FluxOf).
 *  - Pure clients: TF_KillEvent broadcasts. The NetworkManager handler slot
 *    for KillEvent is owned by TFClientNet (single handler per message type),
 *    so this class exposes ClientNoteKill(const TF_KillEvent&) for
 *    TFClientNet::OnKillEvent to forward into (one-line wire-up, see wave
 *    report / publicApiNotes). Until wired, remote-client boards show the
 *    roster with zeroed tallies.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Net/TFNetProtocol.h"

#include <unordered_map>

namespace Terrafront {

class TFScoreboard {
  public:
    TFScoreboard();
    ~TFScoreboard();

    bool Initialize(TFGameContext& ctx, TFEventBus& events);
    void Update(float deltaTime);
    void FixedUpdate(float fixedDeltaTime);
    void Shutdown();
    void RenderDebugUI();
    void RenderUI();

    // --- W2 additions (documented in wave report) ---------------------------

    /// True while the board is being held open (TAB).
    bool IsOpen() const { return m_open; }

    /// Pure-client tally feed: forward TF_KillEvent broadcasts here (no-op on
    /// authority roles, where the event bus already tallies them).
    void ClientNoteKill(const TF_KillEvent& ke);

  private:
    struct Row {
        uint32_t  kills  = 0;
        uint32_t  deaths = 0;
        uint32_t  score  = 0;                    ///< client-side XP estimate
        FactionId faction = FactionId::None;
    };

    Row&      Ensure(PlayerId player, FactionId faction);
    void      OnPlayerKilled(const EvPlayerKilled& ev);
    void      TallyKill(PlayerId killer, PlayerId victim,
                        FactionId killerF, FactionId victimF, bool headshot);
    void      RefreshRoster();                   ///< pull alive pawns into rows
    uint32_t  ScoreOf(PlayerId player, const Row& row) const;
    FactionId ComputeDominion() const;           ///< None == contested
    void      DrawHeader();
    void      DrawFactionColumn(FactionId faction);

    TFGameContext* m_ctx{nullptr};
    TFEventBus*    m_events{nullptr};
    bool           m_initialized{false};
    bool           m_open{false};

    std::unordered_map<PlayerId, Row> m_rows;
};

} // namespace Terrafront
