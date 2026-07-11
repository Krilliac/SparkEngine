/**
 * @file TFScoreboard.h
 * @brief Per-faction K/D/score/regions tab screen.
 *
 * OWNERSHIP: this header + TFScoreboard.cpp belong to ONE implementation agent
 * (W10: the medals-scoreboard lane, together with Game/TFMedalSystem.h/.cpp).
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 *
 * W2 implementation: hold TAB for a fullscreen translucent three-column
 * (one per faction) table of K / D / score per player, with a header showing
 * the continent name, per-faction region counts (TFRegionSystem::RegionsHeld)
 * and Dominion status, plus the local player's rank/XP/flux footer.
 *
 * W10 scoreboard v2 (medals-scoreboard lane): the three faction columns became
 * three stacked faction SECTIONS, each an ImGui table with columns
 *   Name | Outfit | Class icon | Score | K | D | Cap
 * sorted by score (desc), local player row highlighted. Rows merge the
 * authoritative TFMedalSystem score rows (score = kills*100 + captures*250 +
 * medals*50, server-tracked, TF_ScoreUpdate-replicated) over the W2 tallies;
 * with no medal system wired everything degrades to the W2 behavior (kill
 * tallies + XP/score estimate, zero captures). Class icons come from the
 * shipped Assets/Textures/MMOFPS/ui/64/class_*.png set with a text fallback.
 *
 * Data sources:
 *  - Authority roles (standalone / listen host / dedicated): EvPlayerKilled
 *    on the bus tallies kills/deaths; score & rank come straight from
 *    TFProgressionSystem (XPOf/RankOf/FluxOf) until TFMedalSystem rows are
 *    available (then those win).
 *  - Pure clients: TF_KillEvent broadcasts (ClientNoteKill, wired via
 *    TFClientNet) + the TF_ScoreUpdate mirror inside TFMedalSystem.
 *  - TFMedalSystem self-registers through SetMedalSystem from its own
 *    Initialize (TFClientNet::SetChatUI pattern) — no Main.cpp coupling here.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Net/TFNetProtocol.h"

#include <unordered_map>

namespace Terrafront
{

    class TFMedalSystem; // Game/TFMedalSystem.h (same lane; self-registered below)

    class TFScoreboard
    {
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

        // --- W10 medals-scoreboard lane ------------------------------------------

        /// TFMedalSystem::Initialize self-registers here (context pointers are
        /// published before any Initialize, so boot order is irrelevant); its
        /// Shutdown clears it. Null = W2 fallback behavior.
        void SetMedalSystem(TFMedalSystem* medals) { m_medals = medals; }

      private:
        struct Row
        {
            uint32_t kills = 0;
            uint32_t deaths = 0;
            uint32_t score = 0; ///< client-side XP estimate (fallback)
            FactionId faction = FactionId::None;
            // W10 medals-scoreboard additions (merged from TFMedalSystem rows):
            bool hasAuth = false;         ///< authoritative row merged in
            uint32_t authScore = 0;       ///< server score formula result
            uint32_t captures = 0;        ///< capture participations
            uint32_t medals = 0;          ///< medals earned this session
            ClassId cls = ClassId::COUNT; ///< last-known class (COUNT = unknown)
        };

        Row& Ensure(PlayerId player, FactionId faction);
        void OnPlayerKilled(const EvPlayerKilled& ev);
        void TallyKill(PlayerId killer, PlayerId victim, FactionId killerF, FactionId victimF, bool headshot);
        void RefreshRoster();  ///< pull alive pawns into rows
        void MergeMedalRows(); ///< W10: overlay authoritative score rows
        uint32_t ScoreOf(PlayerId player, const Row& row) const;
        FactionId ComputeDominion() const; ///< None == contested
        void DrawHeader();
        void DrawFactionSection(FactionId faction); ///< W10: full-width table per faction

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        TFMedalSystem* m_medals{nullptr}; ///< W10: self-registered (may stay null)
        bool m_initialized{false};
        bool m_open{false};

        std::unordered_map<PlayerId, Row> m_rows;
    };

} // namespace Terrafront
