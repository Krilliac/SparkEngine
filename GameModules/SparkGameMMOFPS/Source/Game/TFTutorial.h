/**
 * @file TFTutorial.h
 * @brief First-join guided tutorial in Sanctuary Haven (W12 tutorial-flow
 *        lane): a compact HUD checklist + world-anchored hint markers that
 *        walk a new player through movement, the firing range, sights, the
 *        class ability, map/ping, the class terminal and finally the travel
 *        terminal ("travel when ready" — completing travel ends the flow).
 *
 * OWNERSHIP: tutorial-flow lane (W12) — this header + TFTutorial.cpp.
 * Owned and driven by TFTravelSystem (constructed in its Initialize, updated
 * from its Update, drawn from its RenderUI, shut down with it) following the
 * TFSanctuaryDecor / TFTargetRange precedent, so no contended Main.cpp
 * wiring is needed. No-op on dedicated servers (HasLocalPlayer gate).
 *
 * ## PURE CLIENT PRESENTATION — read-only detection
 * The tutorial never sends a TFMsg, never touches movement/collision/damage
 * state, and never mutates any other system. Every step is detected from
 * signals that are already observable client-side:
 *  - move/sprint/jump: InputManager key levels/edges (the same VK codes
 *    TFClientNet::SampleAndSendInput reads — W/A/S/D, Shift, Space);
 *  - firing-range hits: TFTargetRange's additive hit-observer seam (the
 *    range's own cosmetic ray-vs-capsule result; see SetHitObserver);
 *  - sight cycling: TFOpticsSystem::Get().ActiveOptic() change (B-key edge
 *    accepted as an acknowledge fallback for irons-only weapons);
 *  - class ability: TFAbilitySystem::GetLocalHudView() leaving Ready (F-key
 *    edge accepted as a fallback when no ability row resolves);
 *  - map: TFMapScreen::IsOpen(); ping: an own-owner entry in
 *    TFPingSystem::ForEachActivePing (Q edge fallback if pings are absent);
 *  - class/travel terminal: pure XZ proximity + the sanctuary-exit mapId
 *    flip (TFTravel_MapIdAt) for the final "deploy" step.
 *
 * ## UX contract (lane brief)
 *  - Skippable: hold ESC for kTFTutorialSkipHoldSec while the checklist is
 *    visible (gated on no fullscreen UI so it never eats an overlay's ESC).
 *  - Combat zones auto-hide: on the continent the tutorial pauses (no
 *    checklist, no markers) — except the final travel step, which COMPLETES
 *    on leaving the sanctuary.
 *  - Re-runnable: `tf_tutorial reset` (also `skip` / `status`).
 *
 * ## Persistence: SESSION-LOCAL by design this wave
 * The lane brief allows a durable tutorialDone flag only through
 * TFPlayerMeta's existing additive API. That store is mutated exclusively by
 * TFProgressionSystem, whose only unlock-set writer (ServerTryUnlock) is
 * TFUnlockTree-gated — an out-of-tree sentinel key can be neither granted
 * nor read back (IsUnlockedInternal returns false for keys the tree does not
 * know). Extending the tree or TFDatabase would leak tutorial state into
 * gameplay unlock semantics / other lanes' files, so per the brief the flag
 * stays session-local: the tutorial re-offers itself once per process run.
 *
 * World markers reuse the world-anchored ImGui foreground recipe from
 * TFTargetRange::RenderUI (own copy — deliberately NOT entangled with
 * TFPingSystem's 3D mesh markers, which need contended RenderWorld wiring).
 */
#pragma once

#include "Core/TFTypes.h"

#include <cstdint>

namespace Terrafront
{

    class TFTargetRange;

    /// Hold ESC this long (checklist visible, no fullscreen UI) to skip.
    constexpr float kTFTutorialSkipHoldSec = 2.0f;
    /// Dummy hits required by the firing-range step.
    constexpr uint32_t kTFTutorialRangeHits = 5;

    class TFTutorial
    {
      public:
        /// Guided steps, in order. Sequential by design: only the CURRENT
        /// step detects, so a veteran sprinting past step 1 still gets a
        /// coherent walk-through (and `tf_tutorial skip` is one command away).
        enum class Step : uint8_t
        {
            Move = 0,  ///< WASD held for a short while
            Sprint,    ///< Shift + a move key held
            Jump,      ///< Space edge
            RangeHits, ///< hit a firing-range dummy kTFTutorialRangeHits times
            Optics,    ///< cycle sights (optic change; B edge fallback)
            Ability,   ///< class ability used (mirror leaves Ready; F fallback)
            Map,       ///< continent map opened (M)
            Ping,      ///< own ping placed (Q)
            ClassTerm, ///< walked up to the class terminal
            Travel,    ///< travel terminal: completing travel ends the tutorial
            COUNT
        };

        TFTutorial();
        ~TFTutorial();

        bool Initialize(TFGameContext& ctx);
        /// Install the read-only hit observer on the sanctuary firing range
        /// (both objects are owned by TFTravelSystem; called right after both
        /// Initialize). Safe with null (range absent -> B/ack fallback never
        /// triggers, the step simply waits for `tf_tutorial skip`).
        void AttachRange(TFTargetRange* range);
        /// Detection state machine. Called by TFTravelSystem::Update every
        /// frame (cheap no-op server-side / while done).
        void Update(float deltaTime);
        void Shutdown();
        /// Checklist window + world-anchored hint markers + toast (ImGui;
        /// headless no-op). Called by TFTravelSystem::RenderUI.
        void RenderUI();

        // --- console / debug surface ---------------------------------------
        bool IsActive() const { return m_active && !m_done; }
        bool IsDone() const { return m_done; }
        Step CurrentStep() const { return m_step; }
        void ResetTutorial(); ///< tf_tutorial reset — restart from Move
        void SkipTutorial();  ///< tf_tutorial skip / ESC hold

      private:
        bool FullscreenUiOpen() const; ///< TFPingUI gate-set mirror
        bool LocalPawn(float outPos[3], bool& outAlive) const;
        void CompleteStep(); ///< advance + confirm bleep
        void FinishTutorial(const char* toast);
        void SetToast(const char* text, float ttlSec);
        void DetectCurrentStep(float dt, const float pawnPos[3]);

        TFGameContext* m_ctx{nullptr};
        TFTargetRange* m_range{nullptr}; ///< observer detach target (owned by TFTravelSystem)
        bool m_initialized{false};
        bool m_debugCmd{false}; ///< tf_tutorial registered by this instance

        // flow state (session-local — see the header persistence note)
        bool m_active{false}; ///< started (first alive sanctuary frame)
        bool m_done{false};   ///< finished or skipped this session
        Step m_step{Step::Move};

        // per-step detection scratch
        float m_moveSec{0.0f};
        float m_sprintSec{0.0f};
        uint32_t m_rangeHits{0}; ///< observer increments while RangeHits is current
        uint8_t m_opticBase{0};  ///< TFOpticKind at Optics step entry
        bool m_opticBaseValid{false};

        // skip / presentation
        float m_escHoldSec{0.0f};
        double m_clock{0.0}; ///< marker bob / toast clock
        char m_toast[96]{};
        float m_toastTTL{0.0f};
    };

} // namespace Terrafront
