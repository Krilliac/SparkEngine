/**
 * @file TFAlertSystem.h
 * @brief Continent alerts — server-scheduled timed events (Territory Rush /
 *        Facility Control) with per-faction scoring, winner XP payouts, a
 *        replicated alert state, and the local HUD banner/splash.
 *
 * OWNERSHIP: alerts lane (W11 fleet) — this header + the TFAlertSystem*.cpp
 * split parts (core / Net / Ui + TFAlertSystemInternal.h).
 *
 * Design (TFMedalSystem precedent: server registry + client mirror in one
 * class; wire ids + packed structs live HERE, not in TFNetProtocol.h):
 *  - SERVER (authority roles): while no alert is running and at least one
 *    pawn is alive, an idle timer counts down a randomized 8-12 min window;
 *    on expiry a random alert starts (console `tf_alert start [rush|facility]`
 *    forces one immediately for testing/chaos validation):
 *      Territory Rush   — 10 min; the faction with the most capture activity
 *                         wins. Score feeds purely from EXISTING events:
 *                         EvRegionCaptured (+100 to the new owner) and the
 *                         canonical capture-credit XP reasons 4/5/6 riding
 *                         EvXPAwarded (+10 per participation tick — the same
 *                         reason codes TFMedalSystem counts captures from).
 *      Facility Control — 10 min; the faction that HOLDS Fluxwell Prime (the
 *                         regions.json "fluxwell" facility; falls back to the
 *                         first facility-tier region) longest wins. Score ==
 *                         whole seconds of ownership, sampled every Update.
 *    End (or `tf_alert stop`): winner = strictly-highest score (tie == draw,
 *    nobody paid). Every WINNING-faction participant — a player who earned
 *    capture-credit XP (reasons 4/5/6) during the alert — is paid
 *    kTFAlertWinXP through the REAL progression hook only:
 *    TFProgressionSystem::ServerAwardXP(player, xp, kXPReasonAlert) — the
 *    exact seam directives (11) and medals (12) use; alerts claim 13.
 *  - WIRE: reserved TFMsg block 0x5470-0x5473.
 *      0x5470 S->C TF_AlertState (reliable; sent on start/end, rebroadcast at
 *                                 1 Hz while active for the score bars, and
 *                                 burst to late joiners via the
 *                                 NetworkManager::GetClients() diff poll —
 *                                 the TFMedalSystem/TFAbilitySystem pattern)
 *      0x5471-0x5473 free.
 *  - CLIENT: TF_AlertState mirror; handlers self-register when the socket
 *    connects (TFMedalSystem EnsureClientHandlers pattern) — no TFClientNet
 *    edits. RenderUI draws the top-center banner (name + mm:ss countdown +
 *    per-faction score bars, under the HUD compass strip) while an alert is
 *    running and a VICTORY/DEFEAT/DRAW splash for kTFAlertEndSplashSec after
 *    it ends — drawn by THIS system's RenderUI, no TFHUD edits needed (the
 *    medal-toast precedent). Listen-host/standalone reads the server state
 *    directly through the same view accessor, so no loopback send is needed.
 *  - CHAOS: fires EvAlertStarted / EvAlertScoreTick / EvAlertEnded (defined
 *    below — bus events are type-indexed, so lane-owned headers may add them)
 *    for TFChaosHarness's `[TF-VALIDATE] alerts` SKIP/PASS line.
 *  - State is session-scoped; no disk persistence. Determinism contract
 *    untouched: nothing here feeds movement or collision, and the RNG is
 *    server-only scheduling.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_set>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Wire protocol — reserved TFMsg id block 0x5470-0x5473 (alerts lane).
    // TFNetProtocol.h (contended) only gains a block comment via the wave
    // wiringNotes; this constant keeps the lane compiling standalone and MUST
    // stay value-identical to any future enum entry.
    // ---------------------------------------------------------------------------

    constexpr uint16_t kTFMsgAlertState = 0x5470; // S->C  TF_AlertState (reliable state + join burst)
    // 0x5471-0x5473 reserved for future alert traffic.

    /// XP reason code for alert win payouts (TF_XPEvent.reasonCode). 0-10 are
    /// the canonical list in TFProgressionSystem.h; 11 is kXPReasonDirective
    /// (Game/TFDirectiveData.h); 12 is kXPReasonMedal (Game/TFMedalSystem.h);
    /// 13 is claimed here — keep in sync.
    inline constexpr uint8_t kXPReasonAlert = 13;

    /// Alert identities (wire values — append only).
    enum class TFAlertType : uint8_t
    {
        TerritoryRush = 0,  ///< most capture progress/flips wins
        FacilityControl = 1 ///< hold the target facility longest
    };

    /// TF_AlertState.phase values.
    enum class TFAlertPhase : uint8_t
    {
        Idle = 0,    ///< no alert (mirror reset)
        Running = 1, ///< countdown + live scores
        Ended = 2    ///< winner announcement (secondsLeft == splash TTL)
    };

#pragma pack(push, 1)

    /// Full alert state (reliable broadcast: start / 1 Hz refresh / end / join
    /// burst). One message carries everything — no delta bookkeeping needed.
    struct TF_AlertState
    {
        uint8_t phase;  // TFAlertPhase
        uint8_t type;   // TFAlertType (valid when phase != Idle)
        uint8_t winner; // FactionId (valid when phase == Ended; None == draw)
        uint8_t _pad;
        uint16_t regionId; // FacilityControl target (kInvalidRegion otherwise)
        uint16_t _pad2;
        uint32_t score[3]; // MRA / AUC / HLX (faction value - 1)
        float secondsLeft; // Running: countdown; Ended: splash TTL
    };
    static_assert(sizeof(TF_AlertState) == 24, "wire layout frozen");

#pragma pack(pop)

    // ---------------------------------------------------------------------------
    // Bus events (chaos-harness + any interested lane). Type-indexed bus — these
    // live here, NOT in the contended Core/TFEvents.h.
    // ---------------------------------------------------------------------------

    struct EvAlertStarted
    {
        TFAlertType type;
        RegionId target; ///< FacilityControl region (kInvalidRegion for rush)
    };
    struct EvAlertScoreTick
    {
        uint32_t topScore; ///< current highest faction score (1 Hz while active)
    };
    struct EvAlertEnded
    {
        TFAlertType type;
        FactionId winner; ///< None == draw
        uint32_t topScore;
    };

    // ---------------------------------------------------------------------------
    // Tuning
    // ---------------------------------------------------------------------------

    constexpr float kTFAlertIdleMinSec = 8.0f * 60.0f;   ///< uneventful-play window (min)
    constexpr float kTFAlertIdleMaxSec = 12.0f * 60.0f;  ///< uneventful-play window (max)
    constexpr float kTFAlertDurationSec = 10.0f * 60.0f; ///< both alert types
    constexpr float kTFAlertEndSplashSec = 6.0f;         ///< VICTORY/DEFEAT splash hold
    constexpr float kTFAlertBroadcastHz = 1.0f;          ///< score refresh cadence
    constexpr uint16_t kTFAlertWinXP = 300;              ///< per winning participant
    constexpr uint32_t kTFAlertScorePerFlip = 100;       ///< rush: EvRegionCaptured
    constexpr uint32_t kTFAlertScorePerCaptureTick = 10; ///< rush: XP reasons 4/5/6

    /// Display name of an alert type.
    const char* TFAlertName(TFAlertType type);

    class TFAlertSystem
    {
      public:
        TFAlertSystem();
        ~TFAlertSystem();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();
        void RenderUI(); ///< alert banner + end splash (local player only)

        // --- cross-system / console surface ------------------------------------

        /// Unified state view: server truth on authority roles, TF_AlertState
        /// mirror on pure clients (the TFMedalSystem GetScoreRow split).
        void GetView(TF_AlertState& out) const;

        /// Authority: start an alert now (ends a running one first, no payout).
        /// False off-authority or when data tables are not loaded.
        bool ServerStartAlert(TFAlertType type);

        /// Authority: end the running alert NOW with the normal winner/XP flow
        /// (i.e. an early finish, not a cancel). False when none is running.
        bool ServerStopAlert();

        /// tf_alert status payload (any role).
        std::string StatusString() const;

      private:
        static size_t FactionIdx(FactionId f) { return static_cast<size_t>(f) - 1; } ///< MRA/AUC/HLX -> 0..2
        bool AuthorityActive() const;                                                ///< initialized authority role
        uint32_t AlivePawnCount() const;        ///< idle timer only runs with players present
        RegionId ResolveFacilityTarget() const; ///< "fluxwell", else first facility, else invalid
        void RerollIdleTimer();
        void TickServer(float dt);
        void EndAlert(const char* how); ///< winner + XP + Ended broadcast
        void BuildState(TF_AlertState& out) const;
        void BroadcastState(); ///< reliable to every connected client

        void OnRegionCaptured(const EvRegionCaptured& ev);
        void OnXPAwarded(const EvXPAwarded& ev);

        // client mirror
        void ClientHandleState(const TF_AlertState& st);

#ifdef ENABLE_NETWORKING
        bool ClientNetActive() const;
        void EnsureClientHandlers();
        void ReleaseClientHandlers();
        void SendWire(PlayerId target, const TF_AlertState& st);
        void PollJoins(); ///< late-joiner burst while an alert is running/ending
#endif

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};
        bool m_consoleCmd{false}; ///< tf_alert registered by this instance

        // --- server state (authority only) ---
        TFAlertPhase m_phase{TFAlertPhase::Idle};
        TFAlertType m_type{TFAlertType::TerritoryRush};
        RegionId m_target{kInvalidRegion};  ///< FacilityControl region
        float m_secondsLeft{0.0f};          ///< Running: countdown; Ended: splash
        float m_idleLeft{0.0f};             ///< seconds until the next auto alert
        std::array<uint32_t, 3> m_scores{}; ///< MRA/AUC/HLX
        std::array<float, 3> m_holdAccum{}; ///< FacilityControl fractional seconds
        std::array<std::unordered_set<PlayerId>, 3> m_participants;
        FactionId m_winner{FactionId::None}; ///< valid while phase == Ended
        float m_broadcastAccum{0.0f};
        std::mt19937 m_rng; ///< server-only scheduling; never sim-facing

        // --- client mirror (pure clients) ---
        TF_AlertState m_mirror{};
        bool m_mirrorValid{false};
        bool m_clientHandlers{false};

#ifdef ENABLE_NETWORKING
        std::unordered_set<PlayerId> m_knownClients; ///< late-joiner burst bookkeeping
#endif

        // debug counters
        uint32_t m_alertsStarted{0};
        uint32_t m_alertsEnded{0};
        uint32_t m_statesSent{0};
        uint32_t m_statesRx{0};
        uint32_t m_badPackets{0};
        uint32_t m_xpPaid{0};
    };

} // namespace Terrafront
